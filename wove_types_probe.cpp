// wove_types_probe.cpp
// Diagnostic for the "every org is an update" problem: the live sync flags
// ~154k of 154k orgs as changed, almost all on the `types` field. This probe
// pulls a sample of orgs from Wove, looks up the SAME codes in CW1, and shows
// the raw Wove `types` next to the CW1-mapped `types` so the vocabulary/case
// gap is visible. It also tallies the distinct token vocabulary on each side.
//
// It reuses the exact TYPE_MAP + mapping/normalization logic from
// wove_org_sync.cpp, so a MATCH here means the sync would treat them as equal.
//
// Usage:
//   wove_types_probe.exe [--pages N] [--show M]
//     --pages N   Wove pages to sample (100 orgs/page, default 3)
//     --show  M   per-code side-by-side rows to print (default 40)
//
// Env: WOVE_CLIENT_ID, WOVE_CLIENT_SECRET, CW1_SERVER, CW1_DATABASE,
//      CW1_DB_USER, CW1_DB_PASSWORD
//
// Build (MSVC Developer Command Prompt):
//   cl wove_types_probe.cpp /EHsc /std:c++17 /link winhttp.lib odbc32.lib odbccp32.lib

#include <windows.h>
#include <winhttp.h>
#include <sql.h>
#include <sqlext.h>
#include <algorithm>
#include <chrono>
#include <cstdlib>
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

static const wchar_t* WOVE_HOST  = L"api.wove.com";
static const wchar_t* TOKEN_PATH = L"/api/v1/external/auth/token";
static const wchar_t* ORG_PATH   = L"/api/v1/external/tms/organizations";
static const int PAGE_SIZE        = 100;
static const int PACE_INTERVAL_MS = 1100;

// Mirror of wove_org_sync.cpp's TYPE_MAP — keep in sync.
static const std::vector<std::pair<const char*, const char*>> TYPE_MAP = {
    { "Consignor",       "shipper"          },
    { "Consignee",       "consignee"        },
    { "Forwarder",       "forwarder"        },  // (unconfirmed)
    { "Carrier",         "carrier"          },
    { "Broker",          "broker"           },  // (unconfirmed)
    { "WarehouseClient", "warehouse"        },
    { "TransportClient", "transport_client" },  // (unconfirmed)
};

// --- helpers copied from wove_org_sync.cpp so the comparison is identical ----
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

static std::string sanitize(const std::string& s)
{
    std::string out = s;
    for (char& c : out) if ((unsigned char)c < 0x20) c = ' ';
    size_t b = out.find_first_not_of(' ');
    if (b == std::string::npos) return "";
    size_t e = out.find_last_not_of(' ');
    return out.substr(b, e - b + 1);
}

static std::vector<std::string> splitCsv(const std::string& csv)
{
    std::vector<std::string> out;
    std::istringstream ss(csv);
    std::string tok;
    while (std::getline(ss, tok, ',')) { tok = sanitize(tok); if (!tok.empty()) out.push_back(tok); }
    return out;
}

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

static std::string jstr(const json& o, const char* key)
{
    auto it = o.find(key);
    if (it == o.end() || !it->is_string()) return "";
    return it->get<std::string>();
}

static std::string joinSet(const std::vector<std::string>& v)
{
    std::string s;
    for (const auto& x : v) s += (s.empty() ? "" : ",") + x;
    return s.empty() ? "(none)" : s;
}

// --- minimal Wove HTTP (auth + GET), copied/trimmed from the sync ------------
struct Response { DWORD statusCode = 0; std::string body; };
static std::string g_token;

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

static bool sendOnce(const wchar_t* method, const std::wstring& path,
                     const std::string& body, const std::string& bearer, Response& out)
{
    bool ok = false;
    HINTERNET hSession = WinHttpOpen(L"NaviaWoveTypesProbe/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return false;
    WinHttpSetTimeouts(hSession, 30000, 30000, 30000, 120000);
    HINTERNET hConnect = WinHttpConnect(hSession, WOVE_HOST, INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET hRequest = hConnect ? WinHttpOpenRequest(hConnect, method, path.c_str(),
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
    if (hRequest) {
        WinHttpAddRequestHeaders(hRequest, L"Content-Type: application/json", (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);
        if (!bearer.empty()) {
            std::wstring auth = L"Authorization: Bearer " + toWide(bearer);
            WinHttpAddRequestHeaders(hRequest, auth.c_str(), (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);
        }
        BOOL sent = WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
            body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)body.c_str(),
            (DWORD)body.size(), (DWORD)body.size(), 0);
        if (sent && WinHttpReceiveResponse(hRequest, nullptr)) {
            DWORD status = 0, size = sizeof(status);
            WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
            out.statusCode = status;
            DWORD avail = 0;
            while (WinHttpQueryDataAvailable(hRequest, &avail) && avail > 0) {
                std::string chunk(avail, '\0'); DWORD got = 0;
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

static bool authenticate()
{
    json body = {
        { "grant_type",    "client_credentials" },
        { "client_id",     getEnvOrEmpty("WOVE_CLIENT_ID") },
        { "client_secret", getEnvOrEmpty("WOVE_CLIENT_SECRET") },
    };
    pace();
    Response resp;
    if (!sendOnce(L"POST", TOKEN_PATH, body.dump(), "", resp) || resp.statusCode != 200) {
        std::cerr << "AUTH failed: HTTP " << resp.statusCode << " " << resp.body << "\n";
        return false;
    }
    json j = json::parse(resp.body, nullptr, false);
    if (j.is_discarded() || !j.contains("access_token")) { std::cerr << "AUTH: no token\n"; return false; }
    g_token = j["access_token"].get<std::string>();
    return true;
}

// --- ODBC: fetch CW1 types for a specific set of codes -----------------------
static bool getCol(SQLHSTMT h, SQLUSMALLINT col, std::string& dest)
{
    dest.clear(); char buf[4096]; SQLLEN ind = 0;
    SQLRETURN rc = SQLGetData(h, col, SQL_C_CHAR, buf, sizeof(buf), &ind);
    while (rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO) {
        if (ind == SQL_NULL_DATA) return true;
        size_t got = (ind == SQL_NO_TOTAL || (size_t)ind >= sizeof(buf)) ? sizeof(buf) - 1 : (size_t)ind;
        dest.append(buf, got);
        if (rc == SQL_SUCCESS) return true;
        rc = SQLGetData(h, col, SQL_C_CHAR, buf, sizeof(buf), &ind);
    }
    return rc == SQL_NO_DATA;
}

// Same type-derivation expression as the sync, restricted to the sampled codes.
static bool fetchCw1TypesFor(const std::set<std::string>& codes,
                             std::map<std::string, std::string>& rawCsvByCode)
{
    std::string inList;
    for (const auto& c : codes) {
        std::string e; for (char ch : c) { if (ch == '\'') e += "''"; else e += ch; }
        inList += (inList.empty() ? "" : ",") + std::string("'") + e + "'";
    }
    std::string q =
        "SET NOCOUNT ON;"
        "SELECT oh.OH_Code,"
        "  SUBSTRING("
        "      CASE WHEN oh.OH_IsConsignor        = 1 THEN ',Consignor'       ELSE '' END"
        "    + CASE WHEN oh.OH_IsConsignee        = 1 THEN ',Consignee'       ELSE '' END"
        "    + CASE WHEN oh.OH_IsForwarder        = 1 THEN ',Forwarder'       ELSE '' END"
        "    + CASE WHEN oh.OH_IsShippingProvider = 1 THEN ',Carrier'         ELSE '' END"
        "    + CASE WHEN oh.OH_IsBroker           = 1 THEN ',Broker'          ELSE '' END"
        "    + CASE WHEN oh.OH_IsWarehouseClient  = 1 THEN ',WarehouseClient' ELSE '' END"
        "    + CASE WHEN oh.OH_IsTransportClient  = 1 THEN ',TransportClient' ELSE '' END"
        "  , 2, 4000) AS cw1_types "
        "FROM dbo.OrgHeader oh WHERE oh.OH_Code IN (" + inList + ")";

    SQLHENV hEnv = SQL_NULL_HANDLE; SQLHDBC hDbc = SQL_NULL_HANDLE; SQLHSTMT hStmt = SQL_NULL_HANDLE;
    bool ok = false;
    std::string connStr =
        "Driver={ODBC Driver 18 for SQL Server};Server=tcp:" + getEnvOrEmpty("CW1_SERVER") + ";"
        "Database=" + getEnvOrEmpty("CW1_DATABASE") + ";UID=" + getEnvOrEmpty("CW1_DB_USER") + ";"
        "PWD=" + getEnvOrEmpty("CW1_DB_PASSWORD") + ";Encrypt=yes;TrustServerCertificate=yes;Connection Timeout=30;";
    do {
        if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &hEnv))) break;
        SQLSetEnvAttr(hEnv, SQL_ATTR_ODBC_VERSION, (SQLPOINTER)SQL_OV_ODBC3, 0);
        if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_DBC, hEnv, &hDbc))) break;
        SQLCHAR o[1024]; SQLSMALLINT ol;
        if (!SQL_SUCCEEDED(SQLDriverConnect(hDbc, nullptr, (SQLCHAR*)connStr.c_str(), SQL_NTS,
            o, sizeof(o), &ol, SQL_DRIVER_NOPROMPT))) { std::cerr << "ODBC connect failed\n"; break; }
        if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_STMT, hDbc, &hStmt))) break;
        if (!SQL_SUCCEEDED(SQLExecDirect(hStmt, (SQLCHAR*)q.c_str(), SQL_NTS))) { std::cerr << "ODBC query failed\n"; break; }
        while (true) {
            SQLRETURN rc = SQLFetch(hStmt);
            if (rc == SQL_NO_DATA) { ok = true; break; }
            if (!SQL_SUCCEEDED(rc)) break;
            std::string code, csv;
            if (!getCol(hStmt, 1, code) || !getCol(hStmt, 2, csv)) break;
            rawCsvByCode[sanitize(code)] = csv;
        }
    } while (false);
    if (hStmt) SQLFreeHandle(SQL_HANDLE_STMT, hStmt);
    if (hDbc) { SQLDisconnect(hDbc); SQLFreeHandle(SQL_HANDLE_DBC, hDbc); }
    if (hEnv) SQLFreeHandle(SQL_HANDLE_ENV, hEnv);
    return ok;
}

int main(int argc, char* argv[])
{
    int pages = 3, show = 40; bool raw = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--pages" && i + 1 < argc) pages = std::atoi(argv[++i]);
        else if (a == "--show" && i + 1 < argc) show = std::atoi(argv[++i]);
        else if (a == "--raw") raw = true;
        else { std::cerr << "Unknown arg: " << a << "\n"; return 1; }
    }
    for (const char* n : { "WOVE_CLIENT_ID","WOVE_CLIENT_SECRET","CW1_SERVER","CW1_DATABASE","CW1_DB_USER","CW1_DB_PASSWORD" })
        if (getEnvOrEmpty(n).empty()) { std::cerr << "Missing env var " << n << "\n"; return 1; }

    if (!authenticate()) return 1;
    std::cout << "Authenticated with Wove. Sampling " << pages << " page(s)...\n";

    // --raw: dump one org from the LIST endpoint and the same org from the
    // single-org DETAIL endpoint, so we can see whether `types` is returned by
    // the list (what the sync diffs against) or only by the detail call.
    if (raw) {
        pace(); Response r1;
        sendOnce(L"GET", std::wstring(ORG_PATH) + L"?page=1&limit=1", "", g_token, r1);
        json j1 = json::parse(r1.body, nullptr, false);
        std::cout << "\n--- LIST endpoint, first org (raw) ---\n";
        std::string id;
        if (!j1.is_discarded() && j1.contains("data") && j1["data"].is_array() && !j1["data"].empty()) {
            std::cout << j1["data"][0].dump(2) << "\n";
            std::cout << "list has `types` key: " << (j1["data"][0].contains("types") ? "YES" : "NO") << "\n";
            id = jstr(j1["data"][0], "id");
        }
        if (!id.empty()) {
            pace(); Response r2;
            sendOnce(L"GET", std::wstring(ORG_PATH) + L"/" + toWide(id), "", g_token, r2);
            json j2 = json::parse(r2.body, nullptr, false);
            std::cout << "\n--- DETAIL endpoint, same org (raw) ---\n";
            const json& d = (!j2.is_discarded() && j2.contains("data")) ? j2["data"] : j2;
            std::cout << d.dump(2) << "\n";
            std::cout << "detail has `types` key: " << (!d.is_discarded() && d.contains("types") ? "YES" : "NO") << "\n";
        }
        return 0;
    }

    // 1. Pull Wove orgs (code -> normalized types) and a token-frequency tally.
    std::map<std::string, std::vector<std::string>> woveTypes;
    std::map<std::string, int> woveTokenTally;
    int woveEmptyTypes = 0;
    for (int page = 1; page <= pages; ++page) {
        std::wstring path = std::wstring(ORG_PATH) + L"?page=" + std::to_wstring(page)
            + L"&limit=" + std::to_wstring(PAGE_SIZE);
        pace();
        Response resp;
        if (!sendOnce(L"GET", path, "", g_token, resp)) { std::cerr << "GET page " << page << " net fail\n"; return 1; }
        json j = json::parse(resp.body, nullptr, false);
        if (j.is_discarded() || !j.contains("data") || !j["data"].is_array()) {
            std::cerr << "GET page " << page << " bad body (HTTP " << resp.statusCode << ")\n"; return 1;
        }
        for (auto& item : j["data"]) {
            std::string code = sanitize(jstr(item, "code"));
            if (code.empty()) continue;
            std::vector<std::string> t;
            if (item.contains("types") && item["types"].is_array())
                for (auto& v : item["types"]) if (v.is_string()) { t.push_back(v.get<std::string>()); woveTokenTally[v.get<std::string>()]++; }
            if (t.empty()) woveEmptyTypes++;
            woveTypes[code] = normalizeTypes(std::move(t));
        }
    }
    std::cout << "Pulled " << woveTypes.size() << " orgs from Wove.\n";

    // 2. Look up the same codes in CW1.
    std::set<std::string> codes;
    for (auto& [c, _] : woveTypes) codes.insert(c);
    std::map<std::string, std::string> cw1RawCsv;
    if (!fetchCw1TypesFor(codes, cw1RawCsv)) { std::cerr << "CW1 lookup failed\n"; return 1; }
    std::cout << "Matched " << cw1RawCsv.size() << " of those codes in CW1.\n\n";

    // 3. Side-by-side, with match accounting.
    std::map<std::string, int> cw1TokenTally;      // raw CW1 tokens (pre-map)
    long compared = 0, match = 0, diff = 0, notInCw1 = 0, printed = 0;
    std::cout << "CODE            WOVE types                | CW1-mapped types            | verdict\n";
    std::cout << "--------------------------------------------------------------------------------\n";
    for (auto& [code, wTypes] : woveTypes) {
        auto it = cw1RawCsv.find(code);
        if (it == cw1RawCsv.end()) { notInCw1++; continue; }
        for (const auto& tok : splitCsv(it->second)) cw1TokenTally[tok]++;
        std::vector<std::string> cTypes = mapCw1Types(it->second);
        compared++;
        bool eq = (wTypes == cTypes);
        eq ? match++ : diff++;
        if (printed++ < show) {
            std::string w = joinSet(wTypes), c = joinSet(cTypes);
            if (w.size() < 26) w.append(26 - w.size(), ' ');
            if (c.size() < 27) c.append(27 - c.size(), ' ');
            std::cout << code; if (code.size() < 16) std::cout << std::string(16 - code.size(), ' ');
            std::cout << w << "| " << c << "| " << (eq ? "MATCH" : "DIFF") << "\n";
        }
    }

    // 4. Vocabulary summary — the actionable bit.
    std::cout << "\n=== Token vocabulary ===\n";
    std::cout << "Wove `types` tokens seen (token=count):\n  ";
    if (woveTokenTally.empty()) std::cout << "(none — Wove returned no types at all)";
    for (auto& [t, n] : woveTokenTally) std::cout << t << "=" << n << "  ";
    std::cout << "\n  orgs with EMPTY Wove types: " << woveEmptyTypes << "\n";
    std::cout << "CW1 raw role tokens seen (pre-map):\n  ";
    for (auto& [t, n] : cw1TokenTally) std::cout << t << "=" << n << "  ";
    std::cout << "\nCW1 tokens with NO TYPE_MAP entry (would be dropped):\n  ";
    if (g_unknownCw1Types.empty()) std::cout << "(none)";
    for (auto& t : g_unknownCw1Types) std::cout << t << "  ";

    // Wove tokens our mapping never produces -> guaranteed permanent diffs.
    std::set<std::string> mappedTargets;
    for (auto& [cw1, wove] : TYPE_MAP) mappedTargets.insert(wove);
    std::cout << "\nWove tokens NOT produced by any TYPE_MAP target (force perpetual diffs):\n  ";
    bool any = false;
    for (auto& [t, n] : woveTokenTally) if (!mappedTargets.count(t)) { std::cout << t << "  "; any = true; }
    if (!any) std::cout << "(none)";

    std::cout << "\n\n=== Verdict ===\n";
    std::cout << "compared=" << compared << "  MATCH=" << match << "  DIFF=" << diff
              << "  (wove codes not in CW1=" << notInCw1 << ")\n";
    if (compared > 0)
        std::cout << "types match rate: " << (100.0 * match / compared) << "%\n";
    return 0;
}
