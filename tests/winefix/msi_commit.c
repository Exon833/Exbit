// winefix_msi_commit.exe <folder>: what Kontakt 8's setup does to its .msi, small (tests/test_wine_fixes.cpp
// runs it in a prefix; src/winefix/msi_fix.c). A database of 50,000 strings is made; opened again, 12,000
// more are added to one table (more than the free places in Wine's string pool, which then grows past
// 65,535 places) and it is committed; a table that wasn't touched is read back. Prints "OK" and ends with
// 0 when that table reads as it was written, "BROKEN" and 1 if not (Wine's own msi.dll: it is read with
// the wrong width of string numbers).
#include <windows.h>
#include <msiquery.h>
#include <stdio.h>
#include <string.h>

// runs a query for its effect; its error (0: none)
static UINT runQuery(MSIHANDLE database, const char* query) {
    MSIHANDLE view = 0;
    UINT result = MsiDatabaseOpenViewA(database, query, &view);
    if (!result) result = MsiViewExecute(view, 0);
    if (view) {
        MsiViewClose(view);
        MsiCloseHandle(view);
    }
    if (result) printf("query failed (%u): %s\n", result, query);
    return result;
}

// inserts `count` keys "<prefix><n>" into `table` (one column, Key)
static int insertKeys(MSIHANDLE database, const char* table, const char* prefix, int count) {
    char query[128];
    MSIHANDLE view = 0, record = MsiCreateRecord(1);
    snprintf(query, sizeof query, "INSERT INTO `%s` (`Key`) VALUES (?)", table);
    if (MsiDatabaseOpenViewA(database, query, &view)) return 0;
    for (int index = 0; index < count; index++) {
        char key[32];
        snprintf(key, sizeof key, "%s%05d", prefix, index);
        MsiRecordSetStringA(record, 1, key);
        if (MsiViewExecute(view, record)) {
            printf("insert %s failed\n", key);
            return 0;
        }
    }
    MsiCloseHandle(record);
    MsiViewClose(view);
    MsiCloseHandle(view);
    return 1;
}

int main(int argc, char** argv) {
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s\\exbit-msi-commit-test.msi", argc > 1 ? argv[1] : ".");
    DeleteFileA(path);
    MSIHANDLE database = 0;
    if (MsiOpenDatabaseA(path, MSIDBOPEN_CREATE, &database)) {
        printf("can't make %s\n", path);
        return 2;
    }
    runQuery(database, "CREATE TABLE `Big` (`Key` CHAR(72) NOT NULL PRIMARY KEY `Key`)");
    runQuery(database, "CREATE TABLE `Other` (`Key` CHAR(72) NOT NULL, `Value` CHAR(72) PRIMARY KEY `Key`)");
    runQuery(database, "CREATE TABLE `Small` (`Key` CHAR(72) NOT NULL PRIMARY KEY `Key`)");
    // (3 rows of 2 strings: 12 bytes, as long as 2 rows read with 3-byte numbers)
    runQuery(database, "INSERT INTO `Other` (`Key`, `Value`) VALUES ('alpha', 'one')");
    runQuery(database, "INSERT INTO `Other` (`Key`, `Value`) VALUES ('beta', 'two')");
    runQuery(database, "INSERT INTO `Other` (`Key`, `Value`) VALUES ('gamma', 'three')");
    if (!insertKeys(database, "Big", "k", 50000) || MsiDatabaseCommit(database)) return 2;
    MsiCloseHandle(database);

    if (MsiOpenDatabaseA(path, MSIDBOPEN_TRANSACT, &database)) return 2;
    if (!insertKeys(database, "Small", "new", 12000) || MsiDatabaseCommit(database)) return 2;
    MsiCloseHandle(database);

    if (MsiOpenDatabaseA(path, MSIDBOPEN_READONLY, &database)) {
        printf("can't open it again\nBROKEN\n");
        return 1;
    }
    MSIHANDLE view = 0, record = 0;
    char rows[256] = "";
    int count = 0;
    MsiDatabaseOpenViewA(database, "SELECT `Key`, `Value` FROM `Other`", &view);
    MsiViewExecute(view, 0);
    while (MsiViewFetch(view, &record) == ERROR_SUCCESS) {
        char key[80] = "", value[80] = "";
        DWORD keySize = sizeof key, valueSize = sizeof value;
        MsiRecordGetStringA(record, 1, key, &keySize);
        MsiRecordGetStringA(record, 2, value, &valueSize);
        if (strlen(rows) + keySize + valueSize + 3 < sizeof rows) {
            strcat(rows, key);
            strcat(rows, "=");
            strcat(rows, value);
            strcat(rows, " ");
        }
        count++;
        MsiCloseHandle(record);
    }
    MsiViewClose(view);
    MsiCloseHandle(view);
    MsiCloseHandle(database);
    DeleteFileA(path);
    int intact = count == 3 && strstr(rows, "alpha=one") && strstr(rows, "beta=two") && strstr(rows, "gamma=three");
    printf("Other: %d rows: %s\n%s\n", count, rows, intact ? "OK" : "BROKEN");
    return intact ? 0 : 1;
}
