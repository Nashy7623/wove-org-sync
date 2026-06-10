# WoveOrg — TMS Organization Sync

Syncs **AU-region** TMS organizations from the CW1 reporting SQL Server (direct
ODBC query — no intermediate store) to the [Wove](https://www.wove.com) API.
Runs as a Windows scheduled task: it queries active orgs from CW1 (main address
resolved server-side by `dbo.MainAddressPkForOrg`), fetches the current orgs
from Wove (page/limit pagination), diffs by org `code`, then POSTs new records
and PUTs changed ones. Orgs missing from the source are logged as orphans,
never deleted.

PRD with full requirements and verified API constraints:
[issue #1](https://github.com/Nashy7623/wove-org-sync/issues/1).

## Files

| File | Purpose |
|---|---|
| `wove_org_sync.cpp` | Main sync program (CW1 → Wove diff/sync) |
| `MainAddressPKForOrg.sql` | CW1 TVF definition (reference; deployed server-side) |
| `vw_Report_ActiveOrganisationsv2 31 Mar 2026.sql` | Legacy ingest view SQL (reference only — not used by the sync) |
| `fabric_test.cpp` | Fabric-era ODBC probe (legacy; Fabric route abandoned) |

## Build

MSVC Developer Command Prompt:

```
cl wove_org_sync.cpp /EHsc /std:c++17 /link winhttp.lib odbc32.lib odbccp32.lib
```

Requires:
- [nlohmann/json](https://github.com/nlohmann/json/releases) single header `json.hpp` placed next to the source (gitignored)
- ODBC Driver 18 for SQL Server
- Host allow-listed against the CW1 reporting SQL Server

## Configuration

All credentials come from environment variables — nothing is compiled in:

| Variable | Purpose |
|---|---|
| `WOVE_CLIENT_ID` / `WOVE_CLIENT_SECRET` | Wove OAuth2 client credentials |
| `CW1_SERVER` | CW1 reporting server, e.g. `NLPMEL.db.wisegrid.net` |
| `CW1_DATABASE` | e.g. `OdysseyNLPMEL` |
| `CW1_DB_USER` / `CW1_DB_PASSWORD` | Dedicated read-only SQL login |
| `WOVE_MAX_WRITES_PER_RUN` | Optional write budget per run (default 8000; the API allows 10,000 requests/day and a full GET sweep uses ~1,550) |

## Running

```
wove_org_sync.exe --dry-run    # full pipeline, no writes; logs every intended create/update
wove_org_sync.exe              # live sync
```

Output goes to stdout and `wove_org_sync.log`. Exit code is non-zero on any
aborted run (missing credentials, auth failure, fetch failure, SQL contract
violation, or any failed write) so Task Scheduler can surface failures.

The Wove API is rate-limited (60 req/min, 10,000 req/day); requests are paced
automatically and writes beyond the per-run budget are deferred to the next
run, so the initial ~176k-org load completes across multiple scheduled runs.

## Deployment

Compile to an `.exe`, set the environment variables for the task's account,
and register with Windows Task Scheduler. Gate the first live run on a
reviewed `--dry-run` log per the PRD's testing decisions.
