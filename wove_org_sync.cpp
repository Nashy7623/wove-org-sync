// wove_org_sync.cpp
// Syncs TMS Organizations from a Microsoft Fabric Lakehouse to the Wove API.
// Reads source data via ODBC, fetches current Wove orgs, diffs them, then
// POSTs new records and PUTs changed records.
//
// Dependencies:
//   nlohmann/json single header — download json.hpp from:
//   https://github.com/nlohmann/json/releases  (place next to this file)
//
// Build (MSVC Developer Command Prompt):
//   cl wove_org_sync.cpp /EHsc /link winhttp.lib odbc32.lib odbccp32.lib

#include <windows.h>
#include <winhttp.h>
#include <sql.h>
#include <sqlext.h>
#include <string>
#include <sstream>
#include <fstream>
#include <iostream>
#include <vector>
#include <map>
#include <ctime>
#include "json.hpp"

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "odbc32.lib")
#pragma comment(lib, "odbccp32.lib")

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

// Wove API
static const wchar_t* WOVE_HOST        = L"api.wove.com";
static const wchar_t* TOKEN_PATH       = L"/api/v1/external/auth/token";
static const wchar_t* ORG_PATH         = L"/api/v1/external/tms/organizations";
static const char*    CLIENT_ID        = "YOUR_CLIENT_ID";
static const char*    CLIENT_SECRET    = "YOUR_CLIENT_SECRET";
static const char*    ORG_SOURCE       = "cw";

// Fabric ODBC — use DSN or full connection string
// Example DSN-less string for Fabric SQL Analytics Endpoint:
//   "Driver={ODBC Driver 18 for SQL Server};"
//   "Server=<workspace-guid>.datawarehouse.fabric.microsoft.com,1433;"
//   "Database=<lakehouse-name>;"
//   "Authentication=ActiveDirectoryServicePrincipal;"
//   "UID=<app-client-id>;"
//   "PWD=<app-client-secret>;"
//   "Encrypt=yes;"
static const char* ODBC_CONN_STRING =
    "Driver={ODBC Driver 18 for SQL Server};"
    "Server=YOUR_WORKSPACE.datawarehouse.fabric.microsoft.com,1433;"
    "Database=YOUR_LAKEHOUSE;"
    "Authentication=ActiveDirectoryServicePrincipal;"
    "UID=YOUR_APP_CLIENT_ID;"
    "PWD=YOUR_APP_CLIENT_SECRET;"
    "Encrypt=yes;";

// Adjust this query and column names to match your actual Fabric table
static const char* FABRIC_QUERY =
    "SELECT code, name, types, carrier_code, "
    "       address1, address2, city, state, postal_code, country, "
    "       phone_number, mobile_number, fax_number, email, website, is_active "
    "FROM   dbo.tms_organizations";

static const char* LOG_FILE = "wove_org_sync.log";

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
// Org record — mirrors Wove's TmsOrganization fields
// ---------------------------------------------------------------------------
struct OrgRecord {
    std::string woweId;      // populated from Wove GET; empty for new records
    std::string code;
    std::string name;
    std::string types;       // comma-delimited in Fabric, converted to JSON array on POST/PUT
    std::string carrierCode;
    std::string address1;
    std::string address2;
    std::string city;
    std::string state;
    std::string postalCode;
    std::string country;
    std::string phoneNumber;
    std::string mobileNumber;
    std::string faxNumber;
    std::string email;
    std::string website;
    bool        isActive = true;
};

static bool orgsEqual(const OrgRecord& a, const OrgRecord& b)
{
    return a.name        == b.name
        && a.types       == b.types
        && a.carrierCode == b.carrierCode
        && a.address1    == b.address1
        && a.address2    == b.address2
        && a.city        == b.city
        && a.state       == b.state
        && a.postalCode  == b.postalCode
        && a.country     == b.country
        && a.phoneNumber == b.phoneNumber
        && a.mobileNumber== b.mobileNumber
        && a.faxNumber   == b.faxNumber
        && a.email       == b.email
        && a.website     == b.website
        && a.isActive    == b.isActive;
}

// ---------------------------------------------------------------------------
// JSON helpers
// ---------------------------------------------------------------------------

// Escapes a string value for embedding in a JSON string literal
static std::string jsonEscape(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out += c;
        }
    }
    return out;
}

// Converts "shipper,consignee" → ["shipper","consignee"]
static std::string typesToJsonArray(const std::string& csv)
{
    if (csv.empty()) return "[]";
    std::ostringstream arr;
    arr << "[";
    std::istringstream ss(csv);
    std::string token;
    bool first = true;
    while (std::getline(ss, token, ',')) {
        if (!token.empty()) {
            if (!first) arr << ",";
            arr << "\"" << jsonEscape(token) << "\"";
            first = false;
        }
    }
    arr << "]";
    return arr.str();
}

static std::string buildPayload(const OrgRecord& org)
{
    std::ostringstream j;
    j << "{"
      << "\"source\":\"" << ORG_SOURCE << "\","
      << "\"code\":\""   << jsonEscape(org.code)   << "\","
      << "\"name\":\""   << jsonEscape(org.name)   << "\","
      << "\"isActive\":"  << (org.isActive ? "true" : "false") << ","
      << "\"types\":"     << typesToJsonArray(org.types);

    auto addStr = [&](const char* key, const std::string& val) {
        if (!val.empty())
            j << ",\"" << key << "\":\"" << jsonEscape(val) << "\"";
    };
    addStr("carrierCode",  org.carrierCode);
    addStr("address1",     org.address1);
    addStr("address2",     org.address2);
    addStr("city",         org.city);
    addStr("state",        org.state);
    addStr("postalCode",   org.postalCode);
    addStr("country",      org.country);
    addStr("phoneNumber",  org.phoneNumber);
    addStr("mobileNumber", org.mobileNumber);
    addStr("faxNumber",    org.faxNumber);
    addStr("email",        org.email);
    addStr("website",      org.website);

    j << "}";
    return j.str();
}

// ---------------------------------------------------------------------------
// WinHTTP
// ---------------------------------------------------------------------------
struct Response { DWORD statusCode = 0; std::string body; };

static bool sendRequest(
    const wchar_t* host,
    const wchar_t* path,
    const wchar_t* method,
    const std::string& body,
    const wchar_t* contentType,
    const std::wstring& authHeader,
    Response& out)
{
    HINTERNET hSession = WinHttpOpen(L"NaviaWoveSync/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) { log("WinHttpOpen failed"); return false; }

    HINTERNET hConnect = WinHttpConnect(hSession, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); log("WinHttpConnect failed"); return false; }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, method, path,
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
        log("WinHttpOpenRequest failed"); return false;
    }

    if (contentType && wcslen(contentType) > 0) {
        std::wstring ct = std::wstring(L"Content-Type: ") + contentType;
        WinHttpAddRequestHeaders(hRequest, ct.c_str(), (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);
    }
    if (!authHeader.empty()) {
        std::wstring auth = L"Authorization: " + authHeader;
        WinHttpAddRequestHeaders(hRequest, auth.c_str(), (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);
    }

    BOOL sent = WinHttpSendRequest(hRequest,
        WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)body.c_str(),
        (DWORD)body.size(), (DWORD)body.size(), 0);

    if (!sent || !WinHttpReceiveResponse(hRequest, nullptr)) {
        WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
        log("Send/receive failed"); return false;
    }

    DWORD statusCode = 0, statusSize = sizeof(statusCode);
    WinHttpQueryHeaders(hRequest,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX);
    out.statusCode = statusCode;

    DWORD bytesAvail = 0;
    while (WinHttpQueryDataAvailable(hRequest, &bytesAvail) && bytesAvail > 0) {
        std::string chunk(bytesAvail, '\0');
        DWORD bytesRead = 0;
        WinHttpReadData(hRequest, &chunk[0], bytesAvail, &bytesRead);
        out.body.append(chunk.data(), bytesRead);
    }

    WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
    return true;
}

// ---------------------------------------------------------------------------
// Wove auth
// ---------------------------------------------------------------------------
static std::string getAccessToken()
{
    std::string body = std::string("{")
        + "\"grant_type\":\"client_credentials\","
        + "\"client_id\":\"" + CLIENT_ID + "\","
        + "\"client_secret\":\"" + CLIENT_SECRET + "\""
        + "}";

    Response resp;
    if (!sendRequest(WOVE_HOST, TOKEN_PATH, L"POST", body, L"application/json", L"", resp)) {
        log("Token request failed"); return "";
    }
    if (resp.statusCode != 200) { log("Token error: " + resp.body); return ""; }

    auto j = json::parse(resp.body, nullptr, false);
    if (j.is_discarded() || !j.contains("access_token")) {
        log("Could not parse access_token"); return "";
    }
    return j["access_token"].get<std::string>();
}

// ---------------------------------------------------------------------------
// Wove GET — fetches all orgs with pagination, keyed by code
// ---------------------------------------------------------------------------
static bool fetchWoveOrgs(const std::wstring& auth, std::map<std::string, OrgRecord>& out)
{
    const int PAGE = 100;
    int skip = 0;
    int total = -1;

    do {
        std::wstring path = std::wstring(ORG_PATH)
            + L"?skip=" + std::to_wstring(skip)
            + L"&limit=" + std::to_wstring(PAGE);

        Response resp;
        if (!sendRequest(WOVE_HOST, path.c_str(), L"GET", "", L"", auth, resp)) {
            log("GET orgs failed at skip=" + std::to_string(skip)); return false;
        }
        if (resp.statusCode != 200) {
            log("GET orgs HTTP " + std::to_string(resp.statusCode) + ": " + resp.body);
            return false;
        }

        auto j = json::parse(resp.body, nullptr, false);
        if (j.is_discarded()) { log("Failed to parse GET response"); return false; }

        if (total < 0 && j.contains("pagination"))
            total = j["pagination"].value("total", 0);

        for (auto& item : j["data"]) {
            OrgRecord r;
            r.woweId      = item.value("id", "");
            r.code        = item.value("code", "");
            r.name        = item.value("name", "");
            r.carrierCode = item.value("carrierCode", "");
            r.address1    = item.value("address1", "");
            r.address2    = item.value("address2", "");
            r.city        = item.value("city", "");
            r.state       = item.value("state", "");
            r.postalCode  = item.value("postalCode", "");
            r.country     = item.value("country", "");
            r.phoneNumber = item.value("phoneNumber", "");
            r.mobileNumber= item.value("mobileNumber", "");
            r.faxNumber   = item.value("faxNumber", "");
            r.email       = item.value("email", "");
            r.website     = item.value("website", "");
            r.isActive    = item.value("isActive", true);

            // Flatten types array back to comma-delimited for comparison
            if (item.contains("types") && item["types"].is_array()) {
                std::string t;
                for (auto& v : item["types"]) {
                    if (!t.empty()) t += ",";
                    t += v.get<std::string>();
                }
                r.types = t;
            }

            if (!r.code.empty()) out[r.code] = r;
        }

        skip += PAGE;
    } while (skip < total);

    log("Fetched " + std::to_string(out.size()) + " orgs from Wove.");
    return true;
}

// ---------------------------------------------------------------------------
// Fabric ODBC query — returns orgs keyed by code
// ---------------------------------------------------------------------------
static bool fetchFabricOrgs(std::map<std::string, OrgRecord>& out)
{
    SQLHENV  hEnv  = SQL_NULL_HANDLE;
    SQLHDBC  hDbc  = SQL_NULL_HANDLE;
    SQLHSTMT hStmt = SQL_NULL_HANDLE;

    if (SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &hEnv) != SQL_SUCCESS) {
        log("ODBC: SQLAllocHandle ENV failed"); return false;
    }
    SQLSetEnvAttr(hEnv, SQL_ATTR_ODBC_VERSION, (SQLPOINTER)SQL_OV_ODBC3, 0);

    if (SQLAllocHandle(SQL_HANDLE_DBC, hEnv, &hDbc) != SQL_SUCCESS) {
        log("ODBC: SQLAllocHandle DBC failed");
        SQLFreeHandle(SQL_HANDLE_ENV, hEnv); return false;
    }

    SQLCHAR outConnStr[1024]; SQLSMALLINT outLen;
    SQLRETURN rc = SQLDriverConnect(hDbc, nullptr,
        (SQLCHAR*)ODBC_CONN_STRING, SQL_NTS,
        outConnStr, sizeof(outConnStr), &outLen, SQL_DRIVER_NOPROMPT);

    if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
        log("ODBC: connection failed");
        SQLFreeHandle(SQL_HANDLE_DBC, hDbc);
        SQLFreeHandle(SQL_HANDLE_ENV, hEnv);
        return false;
    }
    log("ODBC: connected to Fabric.");

    if (SQLAllocHandle(SQL_HANDLE_STMT, hDbc, &hStmt) != SQL_SUCCESS) {
        log("ODBC: SQLAllocHandle STMT failed");
        SQLDisconnect(hDbc); SQLFreeHandle(SQL_HANDLE_DBC, hDbc);
        SQLFreeHandle(SQL_HANDLE_ENV, hEnv); return false;
    }

    rc = SQLExecDirect(hStmt, (SQLCHAR*)FABRIC_QUERY, SQL_NTS);
    if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
        log("ODBC: query failed");
        SQLFreeHandle(SQL_HANDLE_STMT, hStmt);
        SQLDisconnect(hDbc); SQLFreeHandle(SQL_HANDLE_DBC, hDbc);
        SQLFreeHandle(SQL_HANDLE_ENV, hEnv); return false;
    }

    // Column bind helper — reads nullable varchar into std::string
    auto getCol = [&](SQLUSMALLINT col, std::string& dest) {
        char buf[512] = {};
        SQLLEN ind = 0;
        SQLGetData(hStmt, col, SQL_C_CHAR, buf, sizeof(buf), &ind);
        dest = (ind == SQL_NULL_DATA) ? "" : std::string(buf);
    };

    while (SQLFetch(hStmt) == SQL_SUCCESS) {
        OrgRecord r;
        SQLLEN isActiveInd = 0;
        SQLSMALLINT isActiveBit = 1;

        getCol(1,  r.code);
        getCol(2,  r.name);
        getCol(3,  r.types);
        getCol(4,  r.carrierCode);
        getCol(5,  r.address1);
        getCol(6,  r.address2);
        getCol(7,  r.city);
        getCol(8,  r.state);
        getCol(9,  r.postalCode);
        getCol(10, r.country);
        getCol(11, r.phoneNumber);
        getCol(12, r.mobileNumber);
        getCol(13, r.faxNumber);
        getCol(14, r.email);
        getCol(15, r.website);
        SQLGetData(hStmt, 16, SQL_C_SSHORT, &isActiveBit, sizeof(isActiveBit), &isActiveInd);
        r.isActive = (isActiveInd != SQL_NULL_DATA) ? (isActiveBit != 0) : true;

        if (!r.code.empty()) out[r.code] = r;
    }

    SQLFreeHandle(SQL_HANDLE_STMT, hStmt);
    SQLDisconnect(hDbc);
    SQLFreeHandle(SQL_HANDLE_DBC, hDbc);
    SQLFreeHandle(SQL_HANDLE_ENV, hEnv);

    log("Fetched " + std::to_string(out.size()) + " orgs from Fabric.");
    return true;
}

// ---------------------------------------------------------------------------
// Wove POST / PUT
// ---------------------------------------------------------------------------
static bool postOrg(const std::wstring& auth, const OrgRecord& org)
{
    std::string payload = buildPayload(org);
    Response resp;
    if (!sendRequest(WOVE_HOST, ORG_PATH, L"POST", payload, L"application/json", auth, resp))
        return false;
    bool ok = (resp.statusCode == 200 || resp.statusCode == 201);
    log((ok ? "  CREATED " : "  CREATE FAILED ") + org.code + " HTTP " + std::to_string(resp.statusCode));
    return ok;
}

static bool putOrg(const std::wstring& auth, const OrgRecord& org)
{
    std::wstring path = std::wstring(ORG_PATH) + L"/"
        + std::wstring(org.woweId.begin(), org.woweId.end());
    std::string payload = buildPayload(org);
    Response resp;
    if (!sendRequest(WOVE_HOST, path.c_str(), L"PUT", payload, L"application/json", auth, resp))
        return false;
    bool ok = (resp.statusCode == 200 || resp.statusCode == 201);
    log((ok ? "  UPDATED " : "  UPDATE FAILED ") + org.code + " HTTP " + std::to_string(resp.statusCode));
    return ok;
}

// ---------------------------------------------------------------------------
int main()
{
    log("=== wove_org_sync start ===");

    // 1. Auth
    std::string token = getAccessToken();
    if (token.empty()) { log("Aborting: no access token."); return 1; }
    std::wstring auth = L"Bearer " + std::wstring(token.begin(), token.end());

    // 2. Fetch from both sources
    std::map<std::string, OrgRecord> woveOrgs;
    std::map<std::string, OrgRecord> fabricOrgs;

    if (!fetchWoveOrgs(auth, woveOrgs))   { log("Aborting: Wove fetch failed.");   return 1; }
    if (!fetchFabricOrgs(fabricOrgs))     { log("Aborting: Fabric fetch failed."); return 1; }

    // 3. Diff and sync
    int created = 0, updated = 0, skipped = 0;

    for (auto& [code, fabricOrg] : fabricOrgs) {
        auto it = woveOrgs.find(code);
        if (it == woveOrgs.end()) {
            // New — POST
            if (postOrg(auth, fabricOrg)) created++;
        } else {
            // Existing — PUT only if something changed
            OrgRecord merged = fabricOrg;
            merged.woweId = it->second.woweId;
            if (!orgsEqual(fabricOrg, it->second)) {
                if (putOrg(auth, merged)) updated++;
            } else {
                skipped++;
            }
        }
    }

    // Log any orgs in Wove that are no longer in Fabric (not auto-deleted)
    for (auto& [code, woveOrg] : woveOrgs) {
        if (fabricOrgs.find(code) == fabricOrgs.end())
            log("  ORPHAN in Wove (not in Fabric, not deleted): " + code);
    }

    log("Sync complete — created: " + std::to_string(created)
        + "  updated: " + std::to_string(updated)
        + "  unchanged: " + std::to_string(skipped));
    log("=== wove_org_sync end ===\n");

    return 0;
}
