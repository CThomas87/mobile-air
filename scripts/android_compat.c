/*
 * android_compat.c — Compatibility shims for Android Bionic API 24.
 *
 * Functions referenced by pre-built static libraries (libsodium.a,
 * libiconv.a) or by PHP itself that are not available in Android
 * Bionic at API level 24.
 *
 * Compiled as a .o and linked into libphp.so via Makefile EXTRA_LIBS.
 */

#include <errno.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <glob.h>    /* glob_t, GLOB_* macros (defined at all API levels) */
#include <fnmatch.h> /* fnmatch() — available since API 21              */
#include <dirent.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h> /* PATH_MAX */

/*
 * getrandom() — Available natively in Bionic API 28+.
 * libsodium.a references it for its CSPRNG.  On API 24 we fall back to
 * the raw Linux syscall which has been available since kernel 3.17 (all
 * Android devices on API 21+ ship kernel 3.18+).
 */
ssize_t getrandom(void *buf, size_t buflen, unsigned int flags)
{
    return syscall(__NR_getrandom, buf, buflen, flags);
}

/*
 * nl_langinfo() — Not present in Android Bionic at any API level.
 * libiconv.a (localcharset.c) calls it to detect the locale charset.
 * Android always uses UTF-8, so returning a static "UTF-8" is safe.
 */
char *nl_langinfo(int item)
{
    (void)item;
    return (char *)"UTF-8";
}

/* ------------------------------------------------------------------ */
/*  glob() / globfree() — Available natively in Bionic API 28+.       */
/*  PHP's ext/standard/dir.c needs these for the userland glob()      */
/*  function.  We implement them using fnmatch() + opendir/readdir.   */
/* ------------------------------------------------------------------ */

/*
 * <glob.h> provides glob_t and most GLOB_* macros unconditionally,
 * but GLOB_BRACE is behind __USE_BSD and the function prototypes are
 * behind __ANDROID_API__ >= 28.  Provide our own declarations.
 */
#ifndef GLOB_BRACE
#define GLOB_BRACE 0x0080
#endif

/* Forward-declare our own implementations (hidden by API-level guard) */
int glob(const char *pattern, int flags,
         int (*errfunc)(const char *, int), glob_t *pglob);
void globfree(glob_t *pglob);

/* Compare helper for qsort */
static int glob_cmp(const void *a, const void *b)
{
    return strcmp(*(const char **)a, *(const char **)b);
}

/* Append one path to the result set */
static int glob_add(glob_t *pglob, const char *path, int flags)
{
    size_t len = strlen(path);
    int add_slash = 0;

    if (flags & GLOB_MARK)
    {
        struct stat st;
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode))
        {
            if (len == 0 || path[len - 1] != '/')
                add_slash = 1;
        }
    }

    char **nv = realloc(pglob->gl_pathv,
                        (pglob->gl_pathc + 2) * sizeof(char *));
    if (!nv)
        return GLOB_NOSPACE;
    pglob->gl_pathv = nv;

    char *dup = malloc(len + add_slash + 1);
    if (!dup)
        return GLOB_NOSPACE;
    memcpy(dup, path, len);
    if (add_slash)
        dup[len++] = '/';
    dup[len] = '\0';

    pglob->gl_pathv[pglob->gl_pathc++] = dup;
    pglob->gl_pathv[pglob->gl_pathc] = NULL;
    return 0;
}

/* Check whether a string contains glob meta-characters */
static int has_magic(const char *s)
{
    for (; *s; s++)
        if (*s == '*' || *s == '?' || *s == '[')
            return 1;
    return 0;
}

/* Match entries in a single directory against a filename pattern */
static int glob_dir(const char *dir, const char *pat, int flags,
                    int (*errfunc)(const char *, int), glob_t *pglob)
{
    DIR *dp;
    struct dirent *de;
    int fnm_flags = 0;

    if (flags & GLOB_NOESCAPE)
        fnm_flags |= FNM_NOESCAPE;

    dp = opendir(dir[0] ? dir : ".");
    if (!dp)
    {
        if (errfunc && errfunc(dir, errno))
            return GLOB_ABORTED;
        if (flags & GLOB_ERR)
            return GLOB_ABORTED;
        return GLOB_NOMATCH;
    }

    int found = 0;
    while ((de = readdir(dp)) != NULL)
    {
        /* skip hidden files unless pattern starts with '.' */
        if (de->d_name[0] == '.' && pat[0] != '.')
            continue;

        if (fnmatch(pat, de->d_name, fnm_flags) == 0)
        {
            char fullpath[PATH_MAX];
            if (dir[0])
                snprintf(fullpath, sizeof(fullpath), "%s/%s", dir, de->d_name);
            else
                snprintf(fullpath, sizeof(fullpath), "%s", de->d_name);

            int r = glob_add(pglob, fullpath, flags);
            if (r != 0)
            {
                closedir(dp);
                return r;
            }
            found = 1;
        }
    }
    closedir(dp);
    return found ? 0 : GLOB_NOMATCH;
}

/* Forward declarations */
static int glob_internal(const char *pattern, int flags,
                         int (*errfunc)(const char *, int), glob_t *pglob);

/* Expand {a,b,c} brace alternatives (GLOB_BRACE) */
static int glob_brace(const char *pattern, int flags,
                      int (*errfunc)(const char *, int), glob_t *pglob)
{
    const char *p;
    const char *brace_open = NULL, *brace_close = NULL;
    int depth = 0;

    for (p = pattern; *p; p++)
    {
        if (*p == '{' && depth == 0)
        {
            brace_open = p;
            depth = 1;
        }
        else if (*p == '{')
        {
            depth++;
        }
        else if (*p == '}')
        {
            if (--depth == 0)
            {
                brace_close = p;
                break;
            }
        }
    }

    if (!brace_open || !brace_close)
        return glob_internal(pattern, flags & ~GLOB_BRACE, errfunc, pglob);

    size_t prefix_len = (size_t)(brace_open - pattern);
    const char *suffix = brace_close + 1;
    size_t suffix_len = strlen(suffix);
    int result = GLOB_NOMATCH;

    const char *alt = brace_open + 1;
    while (alt <= brace_close)
    {
        const char *end = alt;
        int d = 0;
        while (end < brace_close)
        {
            if (*end == '{')
                d++;
            else if (*end == '}')
                d--;
            else if (*end == ',' && !d)
                break;
            end++;
        }

        size_t alt_len = (size_t)(end - alt);
        size_t total_len = prefix_len + alt_len + suffix_len;
        char *expanded = malloc(total_len + 1);
        if (!expanded)
            return GLOB_NOSPACE;

        memcpy(expanded, pattern, prefix_len);
        memcpy(expanded + prefix_len, alt, alt_len);
        memcpy(expanded + prefix_len + alt_len, suffix, suffix_len);
        expanded[total_len] = '\0';

        int r = glob_brace(expanded, flags, errfunc, pglob);
        free(expanded);

        if (r != 0 && r != GLOB_NOMATCH)
            return r;
        if (r == 0)
            result = 0;

        alt = end + 1;
    }
    return result;
}

/* Core glob logic (no brace expansion) */
static int glob_internal(const char *pattern, int flags,
                         int (*errfunc)(const char *, int), glob_t *pglob)
{
    const char *slash = strrchr(pattern, '/');
    char dir[PATH_MAX] = "";
    const char *base;

    if (slash)
    {
        size_t dir_len = (size_t)(slash - pattern);
        if (dir_len >= sizeof(dir))
            return GLOB_NOSPACE;
        memcpy(dir, pattern, dir_len);
        dir[dir_len] = '\0';
        base = slash + 1;
    }
    else
    {
        base = pattern;
    }

    /* If the directory part itself contains wildcards, glob it first */
    if (has_magic(dir))
    {
        glob_t dirglob;
        memset(&dirglob, 0, sizeof(dirglob));

        int r = glob_internal(dir,
                              GLOB_MARK | (flags & (GLOB_ERR | GLOB_NOESCAPE)),
                              errfunc, &dirglob);
        if (r != 0 && r != GLOB_NOMATCH)
        {
            globfree(&dirglob);
            return r;
        }

        int result = GLOB_NOMATCH;
        for (size_t i = 0; i < dirglob.gl_pathc; i++)
        {
            char *d = dirglob.gl_pathv[i];
            size_t dlen = strlen(d);
            if (dlen > 0 && d[dlen - 1] == '/')
                d[dlen - 1] = '\0';

            r = glob_dir(d, base, flags, errfunc, pglob);
            if (r == 0)
                result = 0;
            else if (r != GLOB_NOMATCH)
            {
                globfree(&dirglob);
                return r;
            }
        }
        globfree(&dirglob);
        return result;
    }

    /* No wildcards in base — just stat the literal path */
    if (!has_magic(base))
    {
        struct stat st;
        if (stat(pattern, &st) == 0)
            return glob_add(pglob, pattern, flags);
        return GLOB_NOMATCH;
    }

    /* Simple case: fixed directory, wildcard filename */
    return glob_dir(dir, base, flags, errfunc, pglob);
}

/* ---- public API -------------------------------------------------- */

int glob(const char *pattern, int flags,
         int (*errfunc)(const char *, int), glob_t *pglob)
{
    if (!(flags & GLOB_APPEND))
    {
        pglob->gl_pathc = 0;
        pglob->gl_pathv = NULL;
    }

    int result;
    if (flags & GLOB_BRACE)
        result = glob_brace(pattern, flags, errfunc, pglob);
    else
        result = glob_internal(pattern, flags, errfunc, pglob);

    if (result == GLOB_NOMATCH && (flags & GLOB_NOCHECK))
        result = glob_add(pglob, pattern, flags);

    if (result == 0 && pglob->gl_pathc > 1 && !(flags & GLOB_NOSORT))
        qsort(pglob->gl_pathv, pglob->gl_pathc, sizeof(char *), glob_cmp);

    if (pglob->gl_pathc == 0 && result == 0)
        result = GLOB_NOMATCH;

    return result;
}

void globfree(glob_t *pglob)
{
    if (pglob->gl_pathv)
    {
        for (size_t i = 0; i < pglob->gl_pathc; i++)
            free(pglob->gl_pathv[i]);
        free(pglob->gl_pathv);
        pglob->gl_pathv = NULL;
    }
    pglob->gl_pathc = 0;
}
