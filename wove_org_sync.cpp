// wove_org_sync.cpp
// Syncs AU-region TMS Organizations from the CW1 reporting SQL Server (direct
// ODBC query) to the Wove External API. Fetches all current Wove orgs
// (page/limit pagination), fetches all CW1 orgs incl. inactive (main address
// resolved by dbo.MainAddressPkForOrg), diffs in memory keyed on org code,
// POSTs new orgs and PUTs changed ones. Orgs inactive in CW1 are soft-deleted
// in Wove (isActive=false via PUT); inactive orgs absent from Wove are never
// created. Orphans (in Wove, not in CW1) are logged as warnings, never deleted.
//
// PRD: https://github.com/Nashy7623/wove-org-sync/issues/1
//
// Usage:
//   wove_org_sync.exe [--dry-run] [--sql-only]
//
//   --dry-run   Full pipeline (auth, both fetches, diff) but no writes;
//               logs every intended create/update with the changed fields.
//   --sql-only  Local test mode: skips Wove auth and the (~28 min) GET sweep,
//               runs ONLY the CW1 ODBC fetch through the real code path,
//               logs counts + a sample, then exits. Needs only CW1_* env vars.
//
// Environment (all required):
//   WOVE_CLIENT_ID, WOVE_CLIENT_SECRET
//   CW1_SERVER, CW1_DATABASE, CW1_DB_USER, CW1_DB_PASSWORD
// Optional:
//   WOVE_MAX_WRITES_PER_RUN  write budget per run (default 8000 — keeps a
//                            full run under the API's 10,000 req/day cap
//                            after the ~1,550-page GET sweep)
//
// Wove API constraints (verified live 2026-06-10):
//   - Pagination is ?page=N&limit=100. `skip` is SILENTLY IGNORED, so every
//     response's pagination.page is asserted to echo the request.
//   - `limit` is server-clamped to 100.
//   - Rate limits: 60 req/min, 10,000 req/day. Rate-limit errors can arrive
//     as HTTP 200 with {"success":false,"error":{"code":"RATE_LIMIT_ERROR"}}
//     so response bodies are checked, not just status codes.
//   - Page ordering is unstable; a sweep may miss/duplicate a handful of
//     records, so reconciliation counts allow a small tolerance.
//
// Build (MSVC Developer Command Prompt):
//   cl wove_org_sync.cpp /EHsc /std:c++17 /link winhttp.lib odbc32.lib odbccp32.lib

#include <windows.h>
#include <winhttp.h>
#include <sql.h>
#include <sqlext.h>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include "json.hpp"

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "odbc32.lib")
#pragma comment(lib, "odbccp32.lib")

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------
static const wchar_t* WOVE_HOST  = L"api.wove.com";
static const wchar_t* TOKEN_PATH = L"/api/v1/external/auth/token";
static const wchar_t* ORG_PATH   = L"/api/v1/external/tms/organizations";
static const char*    ORG_SOURCE = "cw";
static const char*    SOURCE_REGION = "AU";
static const char*    LOG_FILE   = "wove_org_sync.log";

static const int  PAGE_SIZE            = 100;     // server-clamped maximum
static const int  HTTP_MAX_ATTEMPTS    = 6;       // per request, incl. first try
static const int  HTTP_BACKOFF_BASE_MS = 2000;    // 2s, 4s, 8s, 16s, 32s
static const int  PACE_INTERVAL_MS     = 1100;    // < 60 requests/minute
static const int  DEFAULT_MAX_WRITES   = 8000;    // per-run write budget
static const long CW1_MIN_EXPECTED_ROWS = 100000; // contract: sanity floor
static const int  RECONCILE_TOLERANCE  = 100;     // unstable-ordering allowance

// CW1 OH_Is* role flag -> Wove `types` value.
// Wove's vocabulary is lowercase; values observed in live Wove data:
// carrier, shipper, consignee, warehouse, ocean_carrier. Mappings marked
// (unconfirmed) have no observed counterpart yet — validate via dry-run /
// Wove before first live run and adjust here only.
static const std::vector<std::pair<const char*, const char*>> TYPE_MAP = {
    { "Consignor",       "shipper"          },
    { "Consignee",       "consignee"        },
    { "Forwarder",       "forwarder"        },  // (unconfirmed)
    { "Carrier",         "carrier"          },
    { "Broker",          "broker"           },  // (unconfirmed)
    { "WarehouseClient", "warehouse"        },
    { "TransportClient", "transport_client" },  // (unconfirmed)
};

// Source query. One row per active org; the comma-delimited cw1_types tokens
// are the left-hand side of TYPE_MAP. Main address resolved exactly as the
// legacy ingest view did (OUTER APPLY dbo.MainAddressPkForOrg).
static const char* CW1_QUERY =
    "SET NOCOUNT ON;"
    "SELECT"
    "    CONVERT(varchar(36), oh.OH_PK)        AS org_pk,"
    "    oh.OH_Code                            AS code,"
    "    ISNULL(oh.OH_FullName,'')             AS name,"
    "    SUBSTRING("
    "          CASE WHEN oh.OH_IsConsignor        = 1 THEN ',Consignor'       ELSE '' END"
    "        + CASE WHEN oh.OH_IsConsignee        = 1 THEN ',Consignee'       ELSE '' END"
    "        + CASE WHEN oh.OH_IsForwarder        = 1 THEN ',Forwarder'       ELSE '' END"
    "        + CASE WHEN oh.OH_IsShippingProvider = 1 THEN ',Carrier'         ELSE '' END"
    "        + CASE WHEN oh.OH_IsBroker           = 1 THEN ',Broker'          ELSE '' END"
    "        + CASE WHEN oh.OH_IsWarehouseClient  = 1 THEN ',WarehouseClient' ELSE '' END"
    "        + CASE WHEN oh.OH_IsTransportClient  = 1 THEN ',TransportClient' ELSE '' END"
    "    , 2, 4000)                            AS cw1_types,"
    "    ISNULL(oa.OA_Address1,'')             AS address1,"
    "    ISNULL(oa.OA_Address2,'')             AS address2,"
    "    ISNULL(oa.OA_City,'')                 AS city,"
    "    ISNULL(oa.OA_State,'')                AS state,"
    "    ISNULL(oa.OA_PostCode,'')             AS postal_code,"
    "    ISNULL(oa.OA_RN_NKCountryCode,'')     AS country,"
    "    ISNULL(oa.OA_Phone,'')                AS phone,"
    "    ISNULL(oa.OA_Mobile,'')               AS mobile,"
    "    ISNULL(oa.OA_Fax,'')                  AS fax,"
    "    ISNULL(oa.OA_Email,'')                AS email,"
    "    CONVERT(char(1), oh.OH_IsActive)      AS is_active "
    "FROM dbo.OrgHeader oh "
    "OUTER APPLY dbo.MainAddressPkForOrg(oh.OH_PK) m "
    "LEFT JOIN dbo.OrgAddress oa ON oa.OA_PK = m.PK";

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------
static void log(const std::string& msg)
{
    time_t now = time(nullptr);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", localtime(&now));
    std::string line = std::string(ts) + "  " + msg;
    std::cout << line << "\n";
    std::ofstream f(LOG_FILE, std::ios::app);
    if (f) f << line << "\n";
}

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------
static std::string getEnvOrEmpty(const char* name)
{
    char buf[1024];
    DWORD n = GetEnvironmentVariableA(name, buf, sizeof(buf));
    return (n > 0 && n < sizeof(buf)) ? std::string(buf, n) : std::string();
}

static std::wstring toWide(const std::string& s)
{
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

// Control chars (embedded tabs/newlines exist in e.g. OA_PostCode) -> space,
// then trim. Applied to every CW1 text column so payloads and comparisons
// never see transport junk.
static std::string sanitize(const std::string& s)
{
    std::string out = s;
    for (char& c : out)
        if ((unsigned char)c < 0x20) c = ' ';
    size_t b = out.find_first_not_of(' ');
    if (b == std::string::npos) return "";
    size_t e = out.find_last_not_of(' ');
    return out.substr(b, e - b + 1);
}

// Wove JSON fields are frequently null rather than absent. nlohmann's
// value(key, default) THROWS json::type_error on a present-but-null (or
// wrong-typed) value — an uncaught throw that aborts the whole sweep
// (0xC0000409) a few records into page 1. These read defensively instead:
// null / missing / wrong-type all collapse to the default.
static std::string jstr(const json& o, const char* key)
{
    auto it = o.find(key);
    if (it == o.end() || !it->is_string()) return "";
    return it->get<std::string>();
}

static bool jbool(const json& o, const char* key, bool def)
{
    auto it = o.find(key);
    if (it == o.end() || !it->is_boolean()) return def;
    return it->get<bool>();
}

static std::vector<std::string> splitCsv(const std::string& csv)
{
    std::vector<std::string> out;
    std::istringstream ss(csv);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
        tok = sanitize(tok);
        if (!tok.empty()) out.push_back(tok);
    }
    return out;
}

// Sorted, deduped — comparison and payload are both order-insensitive.
static std::vector<std::string> normalizeTypes(std::vector<std::string> v)
{
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
    return v;
}

static std::set<std::string> g_unknownCw1Types;

static std::vector<std::string> mapCw1Types(const std::string& csv)
{
    std::vector<std::string> out;
    for (const auto& tok : splitCsv(csv)) {
        bool found = false;
        for (const auto& [cw1, wove] : TYPE_MAP)
            if (tok == cw1) { out.push_back(wove); found = true; break; }
        if (!found) g_unknownCw1Types.insert(tok);
    }
    return normalizeTypes(out);
}

// ---------------------------------------------------------------------------
// Org record
// ---------------------------------------------------------------------------
struct OrgRecord {
    std::string woveId;   // populated from Wove GET; empty for new records
    std::string orgPk;    // CW1 OH_PK (source side only)
    std::string code;
    std::string name;
    std::vector<std::string> types;   // normalized Wove vocabulary
    std::string address1, address2, city, state, postalCode, country;
    std::string phone, mobile, fax, email;
    bool isActive = true;
};

// Fields compared for PUT decisions. carrierCode/website are not sourced from
// CW1, so they are neither sent nor compared.
static std::vector<std::string> changedFields(const OrgRecord& src, const OrgRecord& dst)
{
    std::vector<std::string> d;
    auto cmp = [&](const char* f, const std::string& a, const std::string& b) {
        if (a != b) d.push_back(f);
    };
    cmp("name",        src.name,       dst.name);
    if (src.types != dst.types) d.push_back("types");
    cmp("address1",    src.address1,   dst.address1);
    cmp("address2",    src.address2,   dst.address2);
    cmp("city",        src.city,       dst.city);
    cmp("state",       src.state,      dst.state);
    cmp("postal_code", src.postalCode, dst.postalCode);
    cmp("country",     src.country,    dst.country);
    cmp("phone",       src.phone,      dst.phone);
    cmp("mobile",      src.mobile,     dst.mobile);
    cmp("fax",         src.fax,        dst.fax);
    cmp("email",       src.email,      dst.email);
    if (src.isActive != dst.isActive) d.push_back("is_active");
    return d;
}

static json buildPayload(const OrgRecord& org)
{
    json j;
    j["source"]   = ORG_SOURCE;
    j["code"]     = org.code;
    j["name"]     = org.name;
    j["isActive"] = org.isActive;
    j["types"]    = org.types;
    j["metadata"] = { { "cw1OrgPk", org.orgPk }, { "sourceRegion", SOURCE_REGION } };

    auto add = [&](const char* key, const std::string& val) {
        if (!val.empty()) j[key] = val;
    };
    add("address1",     org.address1);
    add("address2",     org.address2);
    add("city",         org.city);
    add("state",        org.state);
    add("postalCode",   org.postalCode);
    add("country",      org.country);
    add("phoneNumber",  org.phone);
    add("mobileNumber", org.mobile);
    add("faxNumber",    org.fax);
    add("email",        org.email);
    return j;
}

// ---------------------------------------------------------------------------
// HTTP layer: WinHTTP + timeouts + pacing + retries + token refresh
// ---------------------------------------------------------------------------
struct Response { DWORD statusCode = 0; std::string body; };

// <60 req/min across ALL requests (GETs and writes share the rate budget).
static void pace()
{
    using clock = std::chrono::steady_clock;
    static clock::time_point last{};
    auto now = clock::now();
    if (last != clock::time_point{}) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last).count();
        if (elapsed < PACE_INTERVAL_MS)
            std::this_thread::sleep_for(std::chrono::milliseconds(PACE_INTERVAL_MS - elapsed));
    }
    last = clock::now();
}

// Single transport attempt. Returns false on any network-level failure.
static bool sendOnce(const wchar_t* method, const std::wstring& path,
                     const std::string& body, const std::string& bearer,
                     Response& out)
{
    bool ok = false;
    HINTERNET hSession = WinHttpOpen(L"NaviaWoveSync/2.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return false;

    // resolve / connect / send / receive timeouts — a hung connection cannot
    // stall the scheduled task indefinitely
    WinHttpSetTimeouts(hSession, 30000, 30000, 30000, 120000);

    HINTERNET hConnect = WinHttpConnect(hSession, WOVE_HOST, INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET hRequest = hConnect ? WinHttpOpenRequest(hConnect, method, path.c_str(),
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;

    if (hRequest) {
        WinHttpAddRequestHeaders(hRequest, L"Content-Type: application/json",
            (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);
        if (!bearer.empty()) {
            std::wstring auth = L"Authorization: Bearer " + toWide(bearer);
            WinHttpAddRequestHeaders(hRequest, auth.c_str(), (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);
        }

        BOOL sent = WinHttpSendRequest(hRequest,
            WINHTTP_NO_ADDITIONAL_HEADERS, 0,
            body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)body.c_str(),
            (DWORD)body.size(), (DWORD)body.size(), 0);

        if (sent && WinHttpReceiveResponse(hRequest, nullptr)) {
            DWORD status = 0, size = sizeof(status);
            WinHttpQueryHeaders(hRequest,
                WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
            out.statusCode = status;

            DWORD avail = 0;
            while (WinHttpQueryDataAvailable(hRequest, &avail) && avail > 0) {
                std::string chunk(avail, '\0');
                DWORD got = 0;
                WinHttpReadData(hRequest, &chunk[0], avail, &got);
                out.body.append(chunk.data(), got);
            }
            ok = true;
        }
    }
    if (hRequest) WinHttpCloseHandle(hRequest);
    if (hConnect) WinHttpCloseHandle(hConnect);
    if (hSession) WinHttpCloseHandle(hSession);
    return ok;
}

static std::string g_clientId, g_clientSecret, g_token;
static long g_requestCount = 0;   // visibility against the 10k/day cap

static bool authenticate()
{
    json body = {
        { "grant_type",    "client_credentials" },
        { "client_id",     g_clientId },
        { "client_secret", g_clientSecret },
    };
    pace();
    g_requestCount++;
    Response resp;
    if (!sendOnce(L"POST", TOKEN_PATH, body.dump(), "", resp)) {
        log("AUTH: network failure"); return false;
    }
    if (resp.statusCode != 200) {
        log("AUTH: HTTP " + std::to_string(resp.statusCode) + ": " + resp.body);
        return false;
    }
    json j = json::parse(resp.body, nullptr, false);
    if (j.is_discarded() || !j.contains("access_token")) {
        log("AUTH: could not parse access_token"); return false;
    }
    g_token = j["access_token"].get<std::string>();
    return true;
}

// Full request with retries. Handles:
//   - network failures, 429, 5xx        -> bounded exponential backoff
//   - 401                                -> re-authenticate once per attempt
//   - HTTP 200 with success:false body   -> RATE_LIMIT_ERROR backs off,
//                                           anything else is a hard error
// On success returns true and `out` holds the parsed JSON body.
static bool apiRequest(const wchar_t* method, const std::wstring& path,
                       const std::string& body, json& out)
{
    for (int attempt = 1; attempt <= HTTP_MAX_ATTEMPTS; ++attempt) {
        if (attempt > 1) {
            int delayMs = HTTP_BACKOFF_BASE_MS << (attempt - 2);   // 2s..32s
            log("  retry " + std::to_string(attempt) + "/" + std::to_string(HTTP_MAX_ATTEMPTS)
                + " in " + std::to_string(delayMs / 1000) + "s");
            std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
        }

        pace();
        g_requestCount++;
        Response resp;
        if (!sendOnce(method, path, body, g_token, resp)) {
            log("  network failure"); continue;
        }
        if (resp.statusCode == 401) {
            log("  HTTP 401 — refreshing token");
            if (!authenticate()) continue;
            pace();
            g_requestCount++;
            resp = Response{};
            if (!sendOnce(method, path, body, g_token, resp)) { log("  network failure"); continue; }
        }
        if (resp.statusCode == 429 || resp.statusCode >= 500) {
            log("  HTTP " + std::to_string(resp.statusCode)); continue;
        }

        json j = json::parse(resp.body, nullptr, false);
        if (j.is_discarded()) {
            log("  unparseable response body (HTTP " + std::to_string(resp.statusCode) + ")");
            continue;
        }
        // Rate-limit errors arrive as HTTP 200 with success:false — check body
        if (j.contains("success") && j["success"].is_boolean() && !j["success"].get<bool>()) {
            std::string code = j.value("error", json::object()).value("code", "");
            if (code == "RATE_LIMIT_ERROR") { log("  RATE_LIMIT_ERROR (body)"); continue; }
            log("  API error: " + j.dump());
            return false;
        }
        if (resp.statusCode < 200 || resp.statusCode >= 300) {
            log("  HTTP " + std::to_string(resp.statusCode) + ": " + resp.body);
            return false;
        }
        out = std::move(j);
        return true;
    }
    log("  giving up after " + std::to_string(HTTP_MAX_ATTEMPTS) + " attempts");
    return false;
}

// ---------------------------------------------------------------------------
// Wove GET — all orgs via page/limit pagination, keyed by code
// ---------------------------------------------------------------------------
static bool fetchWoveOrgs(std::map<std::string, OrgRecord>& out)
{
    int totalPages = -1, total = -1, dupCodes = 0;

    for (int page = 1; totalPages < 0 || page <= totalPages; ++page) {
        std::wstring path = std::wstring(ORG_PATH)
            + L"?page=" + std::to_wstring(page)
            + L"&limit=" + std::to_wstring(PAGE_SIZE);

        json j;
        if (!apiRequest(L"GET", path, "", j)) {
            log("GET orgs failed at page " + std::to_string(page)); return false;
        }

        // A partial org list mistaken for the full list would cause duplicate
        // creates — missing pagination metadata is fatal, not recoverable.
        if (!j.contains("pagination") || !j["pagination"].contains("total")) {
            log("FATAL: response missing pagination.total at page " + std::to_string(page));
            return false;
        }
        // The API silently ignores unknown pagination params (`skip` bug) —
        // the echoed page number is the only proof we got the page we asked for.
        int echoedPage = j["pagination"].value("page", -1);
        if (echoedPage != page) {
            log("FATAL: requested page " + std::to_string(page)
                + " but response says page " + std::to_string(echoedPage)
                + " — pagination contract broken");
            return false;
        }
        if (totalPages < 0) {
            total      = j["pagination"].value("total", 0);
            totalPages = j["pagination"].value("totalPages", 0);
            log("Wove reports " + std::to_string(total) + " orgs over "
                + std::to_string(totalPages) + " pages (~"
                + std::to_string((long)totalPages * PACE_INTERVAL_MS / 60000)
                + " min to sweep at " + std::to_string(PACE_INTERVAL_MS) + "ms/page)");
            if (total <= 0 || totalPages <= 0) { log("FATAL: implausible pagination totals"); return false; }
        }

        if (!j.contains("data") || !j["data"].is_array()) {
            log("FATAL: no data array at page " + std::to_string(page)); return false;
        }
        for (auto& item : j["data"]) {
            OrgRecord r;
            r.woveId     = jstr(item, "id");
            r.code       = sanitize(jstr(item, "code"));
            r.name       = sanitize(jstr(item, "name"));
            r.address1   = sanitize(jstr(item, "address1"));
            r.address2   = sanitize(jstr(item, "address2"));
            r.city       = sanitize(jstr(item, "city"));
            r.state      = sanitize(jstr(item, "state"));
            r.postalCode = sanitize(jstr(item, "postalCode"));
            r.country    = sanitize(jstr(item, "country"));
            r.phone      = sanitize(jstr(item, "phoneNumber"));
            r.mobile     = sanitize(jstr(item, "mobileNumber"));
            r.fax        = sanitize(jstr(item, "faxNumber"));
            r.email      = sanitize(jstr(item, "email"));
            r.isActive   = jbool(item, "isActive", true);
            if (item.contains("types") && item["types"].is_array()) {
                std::vector<std::string> t;
                for (auto& v : item["types"])
                    if (v.is_string()) t.push_back(v.get<std::string>());
                r.types = normalizeTypes(std::move(t));
            }
            if (r.code.empty()) continue;
            if (out.count(r.code)) dupCodes++;
            out[r.code] = r;
        }

        if (page % 25 == 0 || page == totalPages)
            log("  fetched page " + std::to_string(page) + "/" + std::to_string(totalPages)
                + " (" + std::to_string(out.size()) + " orgs)");
    }

    // Unstable page ordering means a sweep can miss/duplicate a few records.
    long missed = total - (long)out.size();
    log("Fetched " + std::to_string(out.size()) + " unique orgs from Wove (total="
        + std::to_string(total) + ", dup codes across pages=" + std::to_string(dupCodes)
        + ", missed=" + std::to_string(missed) + ")");
    if (missed > RECONCILE_TOLERANCE) {
        log("FATAL: missed " + std::to_string(missed) + " orgs (> tolerance "
            + std::to_string(RECONCILE_TOLERANCE) + ") — unstable pagination sweep");
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// CW1 ODBC
// ---------------------------------------------------------------------------
static void logOdbcDiagnostics(SQLSMALLINT handleType, SQLHANDLE handle, const char* where)
{
    SQLSMALLINT i = 1;
    SQLCHAR state[6], text[1024];
    SQLINTEGER native;
    SQLSMALLINT len;
    while (SQLGetDiagRec(handleType, handle, i, state, &native, text, sizeof(text), &len)
           == SQL_SUCCESS) {
        log(std::string("ODBC ") + where + " [" + (char*)state + "] ("
            + std::to_string(native) + ") " + (char*)text);
        i++;
    }
}

// Reads one column fully via get-data-in-parts — no fixed-buffer truncation.
static bool getColString(SQLHSTMT hStmt, SQLUSMALLINT col, std::string& dest)
{
    dest.clear();
    char buf[4096];
    SQLLEN ind = 0;
    SQLRETURN rc = SQLGetData(hStmt, col, SQL_C_CHAR, buf, sizeof(buf), &ind);
    while (rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO) {
        if (ind == SQL_NULL_DATA) return true;
        // chunk is NUL-terminated; length is min(ind, buf-1) or full buffer on SQL_NO_TOTAL
        size_t got = (ind == SQL_NO_TOTAL || (size_t)ind >= sizeof(buf))
                       ? sizeof(buf) - 1 : (size_t)ind;
        dest.append(buf, got);
        if (rc == SQL_SUCCESS) return true;          // whole value consumed
        rc = SQLGetData(hStmt, col, SQL_C_CHAR, buf, sizeof(buf), &ind);
    }
    return rc == SQL_NO_DATA ? true : false;
}

static bool fetchCw1Orgs(std::map<std::string, OrgRecord>& out)
{
    std::string server   = getEnvOrEmpty("CW1_SERVER");
    std::string database = getEnvOrEmpty("CW1_DATABASE");
    std::string user     = getEnvOrEmpty("CW1_DB_USER");
    std::string password = getEnvOrEmpty("CW1_DB_PASSWORD");

    std::string connStr =
        "Driver={ODBC Driver 18 for SQL Server};"
        "Server=tcp:" + server + ";"
        "Database=" + database + ";"
        "UID=" + user + ";"
        "PWD=" + password + ";"
        "Encrypt=yes;TrustServerCertificate=yes;"
        "Connection Timeout=30;";

    SQLHENV hEnv = SQL_NULL_HANDLE;
    SQLHDBC hDbc = SQL_NULL_HANDLE;
    SQLHSTMT hStmt = SQL_NULL_HANDLE;
    bool ok = false;
    long emptyCodes = 0, dupCodes = 0;

    do {
        if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &hEnv))) {
            log("ODBC: env alloc failed"); break;
        }
        SQLSetEnvAttr(hEnv, SQL_ATTR_ODBC_VERSION, (SQLPOINTER)SQL_OV_ODBC3, 0);
        if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_DBC, hEnv, &hDbc))) {
            log("ODBC: dbc alloc failed"); break;
        }

        SQLCHAR outStr[1024]; SQLSMALLINT outLen;
        SQLRETURN rc = SQLDriverConnect(hDbc, nullptr,
            (SQLCHAR*)connStr.c_str(), SQL_NTS,
            outStr, sizeof(outStr), &outLen, SQL_DRIVER_NOPROMPT);
        if (!SQL_SUCCEEDED(rc)) {
            log("ODBC: connection to " + server + " failed");
            logOdbcDiagnostics(SQL_HANDLE_DBC, hDbc, "connect");
            break;
        }
        log("ODBC: connected to " + server + "/" + database);

        if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_STMT, hDbc, &hStmt))) {
            log("ODBC: stmt alloc failed"); break;
        }
        rc = SQLExecDirect(hStmt, (SQLCHAR*)CW1_QUERY, SQL_NTS);
        if (!SQL_SUCCEEDED(rc)) {
            log("ODBC: query failed");
            logOdbcDiagnostics(SQL_HANDLE_STMT, hStmt, "query");
            break;
        }

        // SQL_SUCCEEDED tolerates SQL_SUCCESS_WITH_INFO — the TVF emits a
        // null-aggregate warning that must not end the loop early.
        while (true) {
            rc = SQLFetch(hStmt);
            if (rc == SQL_NO_DATA) { ok = true; break; }
            if (!SQL_SUCCEEDED(rc)) {
                log("ODBC: fetch failed at row " + std::to_string(out.size() + 1));
                logOdbcDiagnostics(SQL_HANDLE_STMT, hStmt, "fetch");
                break;
            }

            OrgRecord r;
            std::string typesCsv, isActiveStr;
            bool colOk = getColString(hStmt, 1,  r.orgPk)
                      && getColString(hStmt, 2,  r.code)
                      && getColString(hStmt, 3,  r.name)
                      && getColString(hStmt, 4,  typesCsv)
                      && getColString(hStmt, 5,  r.address1)
                      && getColString(hStmt, 6,  r.address2)
                      && getColString(hStmt, 7,  r.city)
                      && getColString(hStmt, 8,  r.state)
                      && getColString(hStmt, 9,  r.postalCode)
                      && getColString(hStmt, 10, r.country)
                      && getColString(hStmt, 11, r.phone)
                      && getColString(hStmt, 12, r.mobile)
                      && getColString(hStmt, 13, r.fax)
                      && getColString(hStmt, 14, r.email)
                      && getColString(hStmt, 15, isActiveStr);
            if (!colOk) {
                log("ODBC: column read failed at row " + std::to_string(out.size() + 1));
                logOdbcDiagnostics(SQL_HANDLE_STMT, hStmt, "getdata");
                break;
            }

            r.code       = sanitize(r.code);
            r.name       = sanitize(r.name);
            r.address1   = sanitize(r.address1);
            r.address2   = sanitize(r.address2);
            r.city       = sanitize(r.city);
            r.state      = sanitize(r.state);
            r.postalCode = sanitize(r.postalCode);
            r.country    = sanitize(r.country);
            r.phone      = sanitize(r.phone);
            r.mobile     = sanitize(r.mobile);
            r.fax        = sanitize(r.fax);
            r.email      = sanitize(r.email);
            r.types      = mapCw1Types(typesCsv);
            // OH_IsActive (bit) read as char: '1' active, '0'/NULL inactive.
            r.isActive   = (isActiveStr == "1");

            if (r.code.empty()) { emptyCodes++; continue; }
            if (out.count(r.code)) { dupCodes++; continue; }
            out[r.code] = r;
        }
    } while (false);

    if (hStmt) SQLFreeHandle(SQL_HANDLE_STMT, hStmt);
    if (hDbc)  { SQLDisconnect(hDbc); SQLFreeHandle(SQL_HANDLE_DBC, hDbc); }
    if (hEnv)  SQLFreeHandle(SQL_HANDLE_ENV, hEnv);
    if (!ok) return false;

    // SQL contract: one row per org, non-null unique codes, plausible volume.
    log("Fetched " + std::to_string(out.size()) + " orgs from CW1.");
    if (emptyCodes > 0) { log("FATAL: contract violation — " + std::to_string(emptyCodes) + " rows with empty code"); return false; }
    if (dupCodes   > 0) { log("FATAL: contract violation — " + std::to_string(dupCodes) + " duplicate codes"); return false; }
    if ((long)out.size() < CW1_MIN_EXPECTED_ROWS) {
        log("FATAL: contract violation — row count " + std::to_string(out.size())
            + " below expected floor " + std::to_string(CW1_MIN_EXPECTED_ROWS));
        return false;
    }
    if (!g_unknownCw1Types.empty()) {
        std::string s;
        for (const auto& t : g_unknownCw1Types) s += (s.empty() ? "" : ",") + t;
        log("WARNING: unmapped CW1 type tokens ignored: " + s);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Wove writes
// ---------------------------------------------------------------------------
static bool postOrg(const OrgRecord& org)
{
    json out;
    if (!apiRequest(L"POST", ORG_PATH, buildPayload(org).dump(), out)) {
        log("  CREATE FAILED " + org.code);
        return false;
    }
    log("  CREATED " + org.code);
    return true;
}

static bool putOrg(const OrgRecord& org)
{
    std::wstring path = std::wstring(ORG_PATH) + L"/" + toWide(org.woveId);
    json out;
    if (!apiRequest(L"PUT", path, buildPayload(org).dump(), out)) {
        log("  UPDATE FAILED " + org.code);
        return false;
    }
    log("  UPDATED " + org.code);
    return true;
}

// ---------------------------------------------------------------------------
static std::string joinFields(const std::vector<std::string>& v)
{
    std::string s;
    for (const auto& f : v) s += (s.empty() ? "" : ",") + f;
    return s;
}

int main(int argc, char* argv[])
{
    bool dryRun = false, sqlOnly = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--dry-run") dryRun = true;
        else if (std::string(argv[i]) == "--sql-only") sqlOnly = true;
        else { std::cerr << "Unknown argument: " << argv[i] << "\n"; return 1; }
    }

    log(std::string("=== wove_org_sync start")
        + (sqlOnly ? " (SQL-ONLY TEST)" : (dryRun ? " (DRY RUN)" : "")) + " ===");

    // Credentials from environment — never compiled in. --sql-only exercises
    // only the CW1 path, so it needs only the CW1 vars.
    g_clientId     = getEnvOrEmpty("WOVE_CLIENT_ID");
    g_clientSecret = getEnvOrEmpty("WOVE_CLIENT_SECRET");
    const char* requiredAll[] = { "WOVE_CLIENT_ID", "WOVE_CLIENT_SECRET",
                                  "CW1_SERVER", "CW1_DATABASE", "CW1_DB_USER", "CW1_DB_PASSWORD" };
    const char* requiredSql[] = { "CW1_SERVER", "CW1_DATABASE", "CW1_DB_USER", "CW1_DB_PASSWORD" };
    const char** required = sqlOnly ? requiredSql : requiredAll;
    int requiredN = sqlOnly ? 4 : 6;
    for (int i = 0; i < requiredN; ++i) {
        if (getEnvOrEmpty(required[i]).empty()) {
            log(std::string("FATAL: missing required environment variable ") + required[i]);
            return 1;
        }
    }

    // --sql-only: run just the CW1 fetch (real code path), report, exit.
    if (sqlOnly) {
        std::map<std::string, OrgRecord> cw1Orgs;
        log("Fetching CW1 orgs (SQL-only test)...");
        if (!fetchCw1Orgs(cw1Orgs)) { log("Aborting: CW1 fetch failed."); return 1; }
        long active = 0, sampled = 0;
        for (auto& [code, o] : cw1Orgs) if (o.isActive) active++;
        for (auto& [code, o] : cw1Orgs) {
            if (sampled++ >= 5) break;
            log("  SAMPLE " + code + " | " + o.name + " | types=" + joinFields(o.types)
                + " | " + o.city + " " + o.country + " | active=" + (o.isActive ? "1" : "0"));
        }
        log("SQL-only test OK: " + std::to_string(cw1Orgs.size()) + " orgs ("
            + std::to_string(active) + " active).");
        log("=== wove_org_sync end (SQL-ONLY TEST) ===");
        return 0;
    }

    long maxWrites = DEFAULT_MAX_WRITES;
    std::string mw = getEnvOrEmpty("WOVE_MAX_WRITES_PER_RUN");
    if (!mw.empty()) {
        try { maxWrites = std::stol(mw); }
        catch (...) { log("FATAL: WOVE_MAX_WRITES_PER_RUN is not a number: " + mw); return 1; }
        if (maxWrites < 0) { log("FATAL: WOVE_MAX_WRITES_PER_RUN must be >= 0"); return 1; }
    }

    // 1. Auth
    if (!authenticate()) { log("Aborting: authentication failed."); return 1; }
    log("Authenticated with Wove.");

    // 2. Fetch both sides
    std::map<std::string, OrgRecord> woveOrgs, cw1Orgs;
    if (!fetchWoveOrgs(woveOrgs)) { log("Aborting: Wove fetch failed."); return 1; }
    if (!fetchCw1Orgs(cw1Orgs))   { log("Aborting: CW1 fetch failed.");  return 1; }

    // 3. Diff
    std::vector<const OrgRecord*> toCreate;
    std::vector<std::pair<OrgRecord, std::vector<std::string>>> toUpdate;
    long unchanged = 0, skippedInactive = 0;

    for (auto& [code, src] : cw1Orgs) {
        auto it = woveOrgs.find(code);
        if (it == woveOrgs.end()) {
            // Never create an org just to mark it inactive — only orgs that
            // already exist in Wove get soft-deactivated via the update path.
            if (!src.isActive) { skippedInactive++; continue; }
            toCreate.push_back(&src);
        } else {
            auto diffs = changedFields(src, it->second);
            if (diffs.empty()) { unchanged++; continue; }
            OrgRecord merged = src;
            merged.woveId = it->second.woveId;
            toUpdate.emplace_back(std::move(merged), std::move(diffs));
        }
    }

    // There should never be a real orphan: every Wove org must map to a CW1
    // org. An orphan signals drift (manual Wove edit, code mismatch) — surface
    // it loudly, but take no destructive action.
    long orphans = 0;
    for (auto& [code, w] : woveOrgs) {
        if (!cw1Orgs.count(code)) {
            orphans++;
            log("  WARNING: ORPHAN in Wove (no CW1 source, left untouched): " + code);
        }
    }

    log("Diff: creates=" + std::to_string(toCreate.size())
        + " updates=" + std::to_string(toUpdate.size())
        + " unchanged=" + std::to_string(unchanged)
        + " skipped-inactive-noncreate=" + std::to_string(skippedInactive)
        + " orphans=" + std::to_string(orphans));
    if (orphans > 0)
        log("WARNING: " + std::to_string(orphans)
            + " orphan org(s) in Wove with no CW1 source — investigate drift.");

    // 4. Apply (or enumerate, in dry-run)
    long created = 0, updated = 0, failed = 0, deferred = 0, writes = 0;

    for (const OrgRecord* org : toCreate) {
        if (dryRun) { log("  DRY-RUN CREATE " + org->code + " (new)"); continue; }
        if (writes >= maxWrites) { deferred++; continue; }
        writes++;
        postOrg(*org) ? created++ : failed++;
    }
    for (auto& [org, diffs] : toUpdate) {
        if (dryRun) { log("  DRY-RUN UPDATE " + org.code + " fields: " + joinFields(diffs)); continue; }
        if (writes >= maxWrites) { deferred++; continue; }
        writes++;
        putOrg(org) ? updated++ : failed++;
    }

    if (deferred > 0)
        log("Write budget (" + std::to_string(maxWrites) + ") exhausted — "
            + std::to_string(deferred) + " writes deferred to next run.");

    log("Sync complete — created: " + std::to_string(created)
        + "  updated: " + std::to_string(updated)
        + "  unchanged: " + std::to_string(unchanged)
        + "  orphans: " + std::to_string(orphans)
        + "  failed: " + std::to_string(failed)
        + "  deferred: " + std::to_string(deferred)
        + "  api requests: " + std::to_string(g_requestCount));
    log(std::string("=== wove_org_sync end") + (dryRun ? " (DRY RUN)" : "") + " ===");

    return failed > 0 ? 1 : 0;
}
