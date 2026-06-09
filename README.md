# WoveOrg — TMS Organization Sync

Syncs TMS organizations from Navia's Microsoft Fabric Lakehouse (`lh_silver`) to the
[Wove](https://www.wove.com) API. Intended to run as a Windows scheduled task: it reads
the source orgs via ODBC, fetches the current orgs from Wove, diffs by org `code`, then
POSTs new records and PUTs changed ones. Orgs missing from Fabric are logged as orphans,
never deleted.

## Files

| File | Purpose |
|---|---|
| `wove_org_sync.cpp` | Main sync program (Fabric → Wove diff/sync) |
| `wove_org_post.cpp` | Early proof-of-concept single-org POST (superseded by sync) |
| `fabric_test.cpp` | Probe utility: connects to Fabric and dumps `OrgHeader`/`OrgAddress` column schemas |

## Build

MSVC Developer Command Prompt:

```
cl wove_org_sync.cpp /EHsc /link winhttp.lib odbc32.lib odbccp32.lib
```

Requires:
- [nlohmann/json](https://github.com/nlohmann/json/releases) single header `json.hpp` placed next to the source (gitignored)
- ODBC Driver 18 for SQL Server

## Configuration

Before compiling, fill in the placeholders at the top of `wove_org_sync.cpp`:
Wove `CLIENT_ID`/`CLIENT_SECRET`, the Fabric ODBC connection string
(service-principal auth), and the `FABRIC_QUERY` column mapping.

`fabric_test.cpp` reads the service-principal secret from the
`FABRIC_CLIENT_SECRET` environment variable.

## Deployment

Compile to an `.exe` and register it with Windows Task Scheduler on the desired
cadence. Output is written to stdout and appended to `wove_org_sync.log`.
