#ifndef SQLITE3_H
#define SQLITE3_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sqlite3 sqlite3;
typedef struct sqlite3_stmt sqlite3_stmt;

enum
{
    SQLITE_OK = 0,
    SQLITE_ROW = 100,
    SQLITE_DONE = 101,
};

enum
{
    SQLITE_OPEN_READONLY = 0x00000001,
    SQLITE_OPEN_READWRITE = 0x00000002,
    SQLITE_OPEN_CREATE = 0x00000004,
    SQLITE_OPEN_NOMUTEX = 0x00008000,
};

#define SQLITE_STATIC ((void (*)(void *))0)
#define SQLITE_TRANSIENT ((void (*)(void *))-1)

int sqlite3_open_v2(const char *filename, sqlite3 **ppDb, int flags, const char *zVfs);
int sqlite3_close(sqlite3 *);
int sqlite3_exec(sqlite3 *, const char *sql, int (*callback)(void *, int, char **, char **), void *, char **errmsg);
void sqlite3_free(void *);
const char *sqlite3_errmsg(sqlite3 *);
int sqlite3_prepare_v2(sqlite3 *, const char *zSql, int nByte, sqlite3_stmt **ppStmt, const char **pzTail);
int sqlite3_step(sqlite3_stmt *);
int sqlite3_finalize(sqlite3_stmt *);
int sqlite3_bind_int64(sqlite3_stmt *, int, long long);
int sqlite3_bind_text(sqlite3_stmt *, int, const char *, int, void (*)(void *));
int sqlite3_column_int(sqlite3_stmt *, int iCol);
long long sqlite3_column_int64(sqlite3_stmt *, int iCol);
const unsigned char *sqlite3_column_text(sqlite3_stmt *, int iCol);

#ifdef __cplusplus
}
#endif

#endif
