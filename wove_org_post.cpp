// wove_org_post.cpp
// Posts a TMS Organization to the Wove API using OAuth2 client credentials.
// Build: cl wove_org_post.cpp /link winhttp.lib
// Schedule via Windows Task Scheduler pointing at the compiled .exe

#include <windows.h>
#include <winhttp.h>
#include <string>
#include <sstream>
#include <fstream>
#include <iostream>
#include <ctime>

#pragma comment(lib, "winhttp.lib")

// ---------------------------------------------------------------------------
// Configuration — fill these in before compiling, or load from a config file
// ---------------------------------------------------------------------------
static const wchar_t* WOVE_HOST     = L"api.wove.com";
static const wchar_t* TOKEN_PATH    = L"/api/v1/external/auth/token";
static const wchar_t* ORG_PATH      = L"/api/v1/external/tms/organizations";
static const char*    CLIENT_ID     = "YOUR_CLIENT_ID";
static const char*    CLIENT_SECRET = "YOUR_CLIENT_SECRET";
static const char*    LOG_FILE      = "wove_org_post.log";

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
// WinHTTP helpers
// ---------------------------------------------------------------------------
struct Response {
    DWORD  statusCode = 0;
    std::string body;
};

// Sends an HTTPS request and returns the response.
static bool sendRequest(
    const wchar_t* host,
    const wchar_t* path,
    const wchar_t* method,          // L"POST", L"GET", etc.
    const std::string& body,        // empty for GET
    const wchar_t* contentType,     // e.g. L"application/json"
    const std::wstring& authHeader, // full value e.g. L"Bearer <token>", or empty
    Response& out)
{
    HINTERNET hSession = WinHttpOpen(
        L"NaviaWoveClient/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) { log("WinHttpOpen failed"); return false; }

    HINTERNET hConnect = WinHttpConnect(hSession, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); log("WinHttpConnect failed"); return false; }

    HINTERNET hRequest = WinHttpOpenRequest(
        hConnect, method, path, nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        log("WinHttpOpenRequest failed");
        return false;
    }

    // Content-Type header
    if (contentType && wcslen(contentType) > 0) {
        std::wstring ct = std::wstring(L"Content-Type: ") + contentType;
        WinHttpAddRequestHeaders(hRequest, ct.c_str(), (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);
    }

    // Authorization header
    if (!authHeader.empty()) {
        std::wstring auth = L"Authorization: " + authHeader;
        WinHttpAddRequestHeaders(hRequest, auth.c_str(), (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);
    }

    BOOL sent = WinHttpSendRequest(
        hRequest,
        WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)body.c_str(),
        (DWORD)body.size(),
        (DWORD)body.size(), 0);

    if (!sent || !WinHttpReceiveResponse(hRequest, nullptr)) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        log("Send/receive failed");
        return false;
    }

    // Status code
    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    WinHttpQueryHeaders(hRequest,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX,
        &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX);
    out.statusCode = statusCode;

    // Body
    DWORD bytesAvail = 0;
    while (WinHttpQueryDataAvailable(hRequest, &bytesAvail) && bytesAvail > 0) {
        std::string chunk(bytesAvail, '\0');
        DWORD bytesRead = 0;
        WinHttpReadData(hRequest, &chunk[0], bytesAvail, &bytesRead);
        out.body.append(chunk.data(), bytesRead);
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return true;
}

// ---------------------------------------------------------------------------
// Naive JSON field extractor — avoids pulling in a JSON library.
// Finds the first occurrence of "key":"<value>" and returns value.
// ---------------------------------------------------------------------------
static std::string extractJsonString(const std::string& json, const std::string& key)
{
    std::string search = "\"" + key + "\"";
    size_t pos = json.find(search);
    if (pos == std::string::npos) return "";

    pos = json.find(':', pos + search.size());
    if (pos == std::string::npos) return "";

    pos = json.find('"', pos + 1);
    if (pos == std::string::npos) return "";

    size_t end = json.find('"', pos + 1);
    if (end == std::string::npos) return "";

    return json.substr(pos + 1, end - pos - 1);
}

// ---------------------------------------------------------------------------
// Step 1 — get OAuth2 Bearer token
// ---------------------------------------------------------------------------
static std::string getAccessToken()
{
    std::string body =
        std::string("{")
        + "\"grant_type\":\"client_credentials\","
        + "\"client_id\":\"" + CLIENT_ID + "\","
        + "\"client_secret\":\"" + CLIENT_SECRET + "\""
        + "}";

    Response resp;
    if (!sendRequest(WOVE_HOST, TOKEN_PATH, L"POST", body, L"application/json", L"", resp)) {
        log("Token request failed (network error)");
        return "";
    }

    log("Token response HTTP " + std::to_string(resp.statusCode));

    if (resp.statusCode != 200) {
        log("Token error body: " + resp.body);
        return "";
    }

    std::string token = extractJsonString(resp.body, "access_token");
    if (token.empty()) {
        log("Could not parse access_token from response: " + resp.body);
    }
    return token;
}

// ---------------------------------------------------------------------------
// Step 2 — POST a TMS organization
// ---------------------------------------------------------------------------
static bool postOrganization(const std::string& token, const std::string& orgJson)
{
    std::wstring auth = L"Bearer " + std::wstring(token.begin(), token.end());

    Response resp;
    if (!sendRequest(WOVE_HOST, ORG_PATH, L"POST", orgJson, L"application/json", auth, resp)) {
        log("Organization POST failed (network error)");
        return false;
    }

    log("Organization POST HTTP " + std::to_string(resp.statusCode));
    log("Response body: " + resp.body);

    return (resp.statusCode == 200 || resp.statusCode == 201);
}

// ---------------------------------------------------------------------------
// Build the organization payload
// Edit this function to match the data you want to send.
// ---------------------------------------------------------------------------
static std::string buildOrgPayload()
{
    // Replace with real values or load from a file / database query.
    std::ostringstream json;
    json << "{"
         << "\"name\":\"Navia Freight\","
         << "\"email\":\"ops@naviafreight.com\","
         << "\"phone\":\"+1-555-0100\","
         << "\"address\":{"
         <<   "\"street\":\"123 Freight Way\","
         <<   "\"city\":\"Minneapolis\","
         <<   "\"state\":\"MN\","
         <<   "\"postalCode\":\"55401\","
         <<   "\"country\":\"US\""
         << "}"
         << "}";
    return json.str();
}

// ---------------------------------------------------------------------------
int main()
{
    log("=== wove_org_post start ===");

    std::string token = getAccessToken();
    if (token.empty()) {
        log("Aborting: could not obtain access token.");
        return 1;
    }
    log("Access token obtained.");

    std::string payload = buildOrgPayload();
    log("Posting payload: " + payload);

    bool ok = postOrganization(token, payload);
    log(ok ? "Organization posted successfully." : "Organization POST failed.");
    log("=== wove_org_post end ===\n");

    return ok ? 0 : 1;
}
