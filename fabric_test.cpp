#include <sql.h>
#include <sqlext.h>
#include <iostream>
#include <string>

static void printError(SQLHANDLE handle, SQLSMALLINT type) {
    SQLCHAR state[6], msg[512];
    SQLINTEGER nativeErr;
    SQLSMALLINT msgLen;
    SQLSMALLINT i = 1;
    while (SQLGetDiagRec(type, handle, i++, state, &nativeErr, msg, sizeof(msg), &msgLen) == SQL_SUCCESS)
        std::cerr << "  [" << state << "] " << msg << "\n";
}

static bool check(SQLRETURN ret, SQLHANDLE handle, SQLSMALLINT type, const char* context) {
    if (ret == SQL_SUCCESS || ret == SQL_SUCCESS_WITH_INFO) return true;
    std::cerr << context << " failed:\n";
    printError(handle, type);
    return false;
}

int main() {
    const char* secret = getenv("FABRIC_CLIENT_SECRET");
    if (!secret) {
        std::cerr << "Set FABRIC_CLIENT_SECRET env var first:\n"
                  << "  export FABRIC_CLIENT_SECRET='...'\n";
        return 1;
    }

    const std::string connStr =
        "Driver={ODBC Driver 18 for SQL Server};"
        "Server=mljtcijn4haujbb3ufat224wum-wx6oi7zfomhujix6oeybbrzaty.datawarehouse.fabric.microsoft.com,1433;"
        "Database=lh_silver;"
        "Authentication=ActiveDirectoryServicePrincipal;"
        "UID=b1c00683-e8b5-4ba4-baff-bde9f840c98a;"
        "PWD=" + std::string(secret) + ";"
        "Encrypt=yes;"
        "TrustServerCertificate=no;";

    SQLHENV env = SQL_NULL_HANDLE;
    SQLHDBC dbc = SQL_NULL_HANDLE;
    SQLHSTMT stmt = SQL_NULL_HANDLE;

    SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &env);
    SQLSetEnvAttr(env, SQL_ATTR_ODBC_VERSION, (void*)SQL_OV_ODBC3, 0);
    SQLAllocHandle(SQL_HANDLE_DBC, env, &dbc);

    std::cout << "Connecting to lh_silver on Fabric...\n\n";

    SQLRETURN ret = SQLDriverConnect(dbc, NULL,
        (SQLCHAR*)connStr.c_str(), SQL_NTS,
        NULL, 0, NULL, SQL_DRIVER_NOPROMPT);

    if (!check(ret, dbc, SQL_HANDLE_DBC, "SQLDriverConnect")) {
        SQLFreeHandle(SQL_HANDLE_DBC, dbc);
        SQLFreeHandle(SQL_HANDLE_ENV, env);
        return 1;
    }

    std::cout << "Connected. Fetching columns for dbo.OrgHeader and dbo.OrgAddress:\n\n";

    SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt);
    ret = SQLExecDirect(stmt,
        (SQLCHAR*)"SELECT TABLE_NAME, COLUMN_NAME, DATA_TYPE "
                  "FROM INFORMATION_SCHEMA.COLUMNS "
                  "WHERE TABLE_SCHEMA = 'dbo' AND TABLE_NAME IN ('OrgHeader','OrgAddress') "
                  "ORDER BY TABLE_NAME, ORDINAL_POSITION",
        SQL_NTS);

    if (!check(ret, stmt, SQL_HANDLE_STMT, "SQLExecDirect")) {
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        SQLDisconnect(dbc);
        SQLFreeHandle(SQL_HANDLE_DBC, dbc);
        SQLFreeHandle(SQL_HANDLE_ENV, env);
        return 1;
    }

    SQLCHAR col1[256], col2[256], col3[256];
    std::string lastTable;
    while (SQLFetch(stmt) == SQL_SUCCESS) {
        SQLGetData(stmt, 1, SQL_C_CHAR, col1, sizeof(col1), NULL);
        SQLGetData(stmt, 2, SQL_C_CHAR, col2, sizeof(col2), NULL);
        SQLGetData(stmt, 3, SQL_C_CHAR, col3, sizeof(col3), NULL);
        std::string tbl((char*)col1);
        if (tbl != lastTable) {
            std::cout << "\n[" << tbl << "]\n";
            lastTable = tbl;
        }
        std::cout << "  " << col2 << " (" << col3 << ")\n";
    }

    SQLFreeHandle(SQL_HANDLE_STMT, stmt);
    SQLDisconnect(dbc);
    SQLFreeHandle(SQL_HANDLE_DBC, dbc);
    SQLFreeHandle(SQL_HANDLE_ENV, env);
    return 0;
}
