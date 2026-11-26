// Exbit's msi.dll for Wine prefixes (see wine_own_dll.h): Wine's own msi.dll, but for MsiDatabaseCommit.
//
// An MSI database keeps its strings in one pool; a table refers to a string by its number in it, in
// 2 bytes, or in 3 when the pool has more than 65535 places (a flag in the pool's header says which).
// Wine's pool grows by half when a string is added to a full one, and Wine writes every place, used
// or not, so adding a few strings to a database of 43,691 to 65,535 strings makes Wine write it with
// the 3-byte flag. It then writes the tables it holds in memory (those read or changed since the
// database was opened) with 3-byte numbers, and leaves the others as they were, with 2: the next
// reading of them, by the flag, is garbage. (Windows writes only the strings in use, and every table:
// its database stays readable.) Kontakt 8's setup (InstallAware) adds its feature choices to its
// .msi this way and then read its Component table as nonsense: Wine's msi crashed in it, and the
// setup stood still at "Computing space requirements".
//
// Here, before Wine's commit, every table of the database is read (SELECT * FROM each in _Tables), so
// Wine holds them all and writes them all with the same width as the pool's flag says.
#include <windows.h>

#include <stdio.h>
#include <wchar.h>

#include "winefix/export_thunks.h"
#include "winefix/wine_own_dll.h"

enum {
#define EXPORT(index, ordinal, name) MSI_INDEX_##name = index,
#include "winefix/msi_exports.h"
#undef EXPORT
    MSI_EXPORT_COUNT
};

#define EXPORT(index, ordinal, name) EXPORT_THUNK(index)
#include "winefix/msi_exports.h"
#undef EXPORT

void* exbitRealFunctions[MSI_EXPORT_COUNT];
static void* wineCommit;  // (Wine's own MsiDatabaseCommit)
static int missingExports = -1;

static const char* const exportNames[] = {
#define EXPORT(index, ordinal, name) #name,
#include "winefix/msi_exports.h"
#undef EXPORT
};
static const WORD exportOrdinals[] = {
#define EXPORT(index, ordinal, name) ordinal,
#include "winefix/msi_exports.h"
#undef EXPORT
};

typedef unsigned long MsiHandle;
typedef UINT(WINAPI* OpenViewFunction)(MsiHandle database, const wchar_t* query, MsiHandle* view);
typedef UINT(WINAPI* ExecuteFunction)(MsiHandle view, MsiHandle record);
typedef UINT(WINAPI* FetchFunction)(MsiHandle view, MsiHandle* record);
typedef UINT(WINAPI* GetStringFunction)(MsiHandle record, UINT field, wchar_t* value, DWORD* size);
typedef UINT(WINAPI* HandleFunction)(MsiHandle handle);
typedef int(WINAPI* StateFunction)(MsiHandle database);
typedef UINT(WINAPI* CommitFunction)(MsiHandle database);

#define REAL(name) exbitRealFunctions[MSI_INDEX_##name]

// runs a query for nothing but its effect (Wine reads the table into memory); true if it ran
static int runQuery(MsiHandle database, const wchar_t* query) {
    MsiHandle view = 0;
    UINT result = ((OpenViewFunction)REAL(MsiDatabaseOpenViewW))(database, query, &view);
    if (result == ERROR_SUCCESS) {
        result = ((ExecuteFunction)REAL(MsiViewExecute))(view, 0);
        ((HandleFunction)REAL(MsiViewClose))(view);
        ((HandleFunction)REAL(MsiCloseHandle))(view);
    }
    return result == ERROR_SUCCESS;
}

// reads every table of the database (named in _Tables) into Wine's memory; how many it read
static int readEveryTable(MsiHandle database) {
    MsiHandle view = 0, record = 0;
    int tablesRead = 0;
    if (((OpenViewFunction)REAL(MsiDatabaseOpenViewW))(database, L"SELECT `Name` FROM `_Tables`", &view))
        return 0;
    if (((ExecuteFunction)REAL(MsiViewExecute))(view, 0) == ERROR_SUCCESS) {
        while (((FetchFunction)REAL(MsiViewFetch))(view, &record) == ERROR_SUCCESS) {
            wchar_t name[128], query[200];
            DWORD size = 128;
            if (((GetStringFunction)REAL(MsiRecordGetStringW))(record, 1, name, &size) == ERROR_SUCCESS &&
                !wcschr(name, L'`')) {
                swprintf(query, 200, L"SELECT * FROM `%ls`", name);
                tablesRead += runQuery(database, query);
            }
            ((HandleFunction)REAL(MsiCloseHandle))(record);
        }
        ((HandleFunction)REAL(MsiViewClose))(view);
    }
    ((HandleFunction)REAL(MsiCloseHandle))(view);
    return tablesRead;
}

// MsiDatabaseCommit: Wine's, after every table is read (see the top). EXBIT_WINE_MSI_AS_IS=1: Wine's alone
static UINT WINAPI commitWithEveryTable(MsiHandle database) {
    const int MSI_DATABASE_STATE_WRITE = 1;  // (MSIDBSTATE_WRITE: opened to be changed)
    if (!GetEnvironmentVariableW(L"EXBIT_WINE_MSI_AS_IS", NULL, 0) &&
        ((StateFunction)REAL(MsiGetDatabaseState))(database) == MSI_DATABASE_STATE_WRITE)
        readEveryTable(database);
    return ((CommitFunction)wineCommit)(database);
}

// (exported as ExbitWineFixReady, for Exbit's check: wine-fixes/exbit-wine-fix-check.exe) 1 when Wine's msi.dll is behind this one with all
// its exports, 0 if not
int exbitWineFixReady(void) { return missingExports == 0; }

// finds Wine's own msi.dll and every export of it; the commit is replaced by commitWithEveryTable
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void* reserved) {
    if (reason != DLL_PROCESS_ATTACH)
        return TRUE;
    DisableThreadLibraryCalls(instance);
    HMODULE wine = loadWineOwnDll(L"msi.dll");
    if (!wine)
        return FALSE;  // (Wine says it couldn't load msi.dll; Exbit's check takes the stand-in away)
    missingExports = resolveExports(wine, exportNames, exportOrdinals, exbitRealFunctions, MSI_EXPORT_COUNT);
    wineCommit = REAL(MsiDatabaseCommit);
    if (wineCommit)
        REAL(MsiDatabaseCommit) = (void*)commitWithEveryTable;
    return TRUE;
}
