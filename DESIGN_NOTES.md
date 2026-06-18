# wove_org_sync — Design Notes

Decisions and findings from the 2026-06-11 review. Covers (1) the soft-delete
change already implemented, and (2) the incremental-sync design, not yet built.

---

## 1. Soft-delete of inactive orgs — IMPLEMENTED (full sweep)

### Problem
The CW1 query filtered `WHERE OH_IsActive = 1`, so inactive orgs vanished from
the source set and became "orphans" in Wove — logged, never acted on.
Deactivation propagated nowhere. `OrgRecord.isActive` was hardcoded `true`, so
the existing `is_active` diff/payload support was dead code.

### Decision
- **Soft-delete only** (set `isActive=false` via PUT). No hard deletes — nothing
  removed that might need to be restored.
- **Never create an org just to mark it inactive.** Inactive + absent from Wove
  → skipped, not created.
- **Orphans should never exist** (every Wove org must map to a CW1 org). An
  orphan now signals drift → logged as a loud `WARNING` + surfaced in summary,
  but still no destructive action.

### Changes (`wove_org_sync.cpp`)
| Location | Change |
|---|---|
| `CW1_QUERY` | Removed `WHERE OH_IsActive = 1`; added `CONVERT(char(1), oh.OH_IsActive) AS is_active` (now 15 columns) |
| `fetchCw1Orgs` | Reads col 15 → `r.isActive = (isActiveStr == "1")` (was hardcoded `true`) |
| Diff loop | Inactive + not-in-Wove → skip create (`skippedInactive` counter); inactive + in-Wove → existing update path flags `is_active` → PUT `isActive=false` |
| Orphans | Escalated to `WARNING` + summary count |
| Header comment | Updated to describe soft-delete behaviour |

`changedFields` and `buildPayload` already handle `isActive` — no change needed.

### Not yet done
- Not compiled/tested (no MSVC or `json.hpp` on the review machine). Build with
  the README command and run `--dry-run` to confirm: expect
  `DRY-RUN UPDATE <code> fields: is_active` lines, zero deletes.
- Row-count floor (`CW1_MIN_EXPECTED_ROWS`, ≥100k) now counts active+inactive;
  still valid as a floor but semantics changed — review the value.

---

## 2. Incremental sync — DESIGNED, NOT BUILT

### Motivation
Current full sweep reads all Wove orgs every run (~1,550 GETs, ~15% of the
10k/day budget, ~28 min just to read). Going incremental off CW1 change
timestamps removes the need to read Wove state at all for the common case.

Rejected alternative: per-org GET for each CW1 org = ~178k reads/run (~18 days
of budget). Worse than the bulk read. Not viable.

### Key constraint discovered (decisive)
**A header-only watermark is unsafe.** Editing an address does NOT bump
`OrgHeader.OH_SystemLastEditTimeUTC`.

Evidence (NLPMEL, 2026-06-11):
- `OrgHeader`: 178,463 rows, 178,462 with a timestamp (1 NULL), range 2006 → today.
- `PEIHAISUB`: header edited 2026-04-21, its address edited 2026-06-04 —
  **64,147 min (~44.5 days) later**. Header never moved.

So incremental **must watermark on both** `OrgHeader.OH_SystemLastEditTimeUTC`
**and** `OrgAddress.OA_SystemLastEditTimeUtc`.

Schema confirmed: `OrgAddress.OA_OH` → `OrgHeader.OH_PK`; address edit column is
`OA_SystemLastEditTimeUtc`.

### Proposed query — two-step (keeps the MainAddressPkForOrg TVF off the hot path)

Step 1 — changed org PKs from both tables (index seeks, tiny result):
```sql
SELECT OH_PK FROM dbo.OrgHeader  WHERE OH_SystemLastEditTimeUTC > @lastRun
UNION
SELECT OA_OH FROM dbo.OrgAddress WHERE OA_SystemLastEditTimeUtc > @lastRun;
```

Step 2 — existing full SELECT (TVF + `is_active`) constrained to that set:
```sql
... FROM dbo.OrgHeader oh
OUTER APPLY dbo.MainAddressPkForOrg(oh.OH_PK) m
LEFT JOIN dbo.OrgAddress oa ON oa.OA_PK = m.PK
WHERE oh.OH_PK IN ( <changed PKs from step 1> );
```
TVF only fires for changed orgs → fast. Catches header edits, address edits,
and deactivations (flipping `OH_IsActive` bumps the header).

### Watermark mechanics
- Persist `@lastRun` (UTC) in a state file; on success advance to the run's
  start time **minus a ~30 min safety overlap** so mid-run edits aren't skipped
  (idempotent diff makes re-processing harmless).
- The 1 NULL-timestamp header row never appears in a delta — accept it, or fold
  in via the periodic full sweep.
- **Keep an occasional full sweep** (e.g. weekly) as backstop: only it catches
  Wove-side drift, orphans, and the NULL row.

### Open / next step
- Run the Step-1 count with a 1-day watermark to size typical changes/day and
  confirm the delta is small & fast — informs per-run budget impact.
- Confirm indexes exist on both edit-time columns (else Step 1 scans).
- Decide full-sweep cadence.

---

## Environment note
SQL MCP connections (`hub`, `staging`) currently fail with
`connect() got an unexpected keyword argument 'encrypt'` — server-side config
bug. Validation queries above were run manually against NLPMEL.
