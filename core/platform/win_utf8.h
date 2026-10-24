#ifndef M8_WIN_UTF8_H
#define M8_WIN_UTF8_H
/* UTF-8 front ends for the wide Win32 file API. They do not depend on the
 * process code page, so non-ASCII sample and folder names survive on every
 * Windows version. Include after <windows.h>. */
#include <stdlib.h>
#include <string.h>

static inline wchar_t *u8_widen(const char *text)
{
    if (!text) return NULL;
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, NULL, 0);
    wchar_t *out = n > 0 ? malloc((size_t)n * sizeof *out) : NULL;
    if (!out || !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, out, n)) {
        free(out); SetLastError(ERROR_NO_UNICODE_TRANSLATION); return NULL;
    }
    return out;
}
static inline char *u8_narrow(const wchar_t *text)
{
    if (!text) return NULL;
    int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, NULL, 0, NULL, NULL);
    char *out = n > 0 ? malloc((size_t)n) : NULL;
    if (!out || !WideCharToMultiByte(CP_UTF8, 0, text, -1, out, n, NULL, NULL)) { free(out); return NULL; }
    return out;
}
/* Malloc'd UTF-8 value of an environment variable, or NULL. */
static inline char *u8_getenv(const char *name)
{
    wchar_t *wide_name = u8_widen(name), *value = wide_name ? _wgetenv(wide_name) : NULL;
    free(wide_name);
    return value && *value ? u8_narrow(value) : NULL;
}
static inline HANDLE u8_CreateFile(const char *path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security,
                            DWORD disposition, DWORD flags, HANDLE template_file)
{
    wchar_t *w = u8_widen(path);
    HANDLE h = w ? CreateFileW(w, access, share, security, disposition, flags, template_file) : INVALID_HANDLE_VALUE;
    DWORD e = GetLastError(); free(w); SetLastError(e);
    return h;
}
static inline DWORD u8_GetFileAttributes(const char *path)
{
    wchar_t *w = u8_widen(path);
    DWORD a = w ? GetFileAttributesW(w) : INVALID_FILE_ATTRIBUTES;
    DWORD e = GetLastError(); free(w); SetLastError(e);
    return a;
}
static inline BOOL u8_CreateDirectory(const char *path)
{
    wchar_t *w = u8_widen(path);
    BOOL ok = w ? CreateDirectoryW(w, NULL) : FALSE;
    DWORD e = GetLastError(); free(w); SetLastError(e);
    return ok;
}
static inline BOOL u8_DeleteFile(const char *path)
{
    wchar_t *w = u8_widen(path);
    BOOL ok = w ? DeleteFileW(w) : FALSE;
    DWORD e = GetLastError(); free(w); SetLastError(e);
    return ok;
}
static inline BOOL u8_MoveFileEx(const char *from, const char *to, DWORD flags)
{
    wchar_t *a = u8_widen(from), *b = u8_widen(to);
    BOOL ok = a && b ? MoveFileExW(a, b, flags) : FALSE;
    DWORD e = GetLastError(); free(a); free(b); SetLastError(e);
    return ok;
}
/* Malloc'd absolute UTF-8 path with forward slashes. */
static inline char *u8_full_path(const char *path)
{
    wchar_t *w = u8_widen(path);
    DWORD need = w ? GetFullPathNameW(w, 0, NULL, NULL) : 0;
    wchar_t *full = need ? malloc((size_t)need * sizeof *full) : NULL;
    char *out = NULL;
    if (full && GetFullPathNameW(w, need, full, NULL)) out = u8_narrow(full);
    free(w); free(full);
    if (out) for (char *p = out; *p; ++p) if (*p == '\\') *p = '/';
    return out;
}

/* Directory enumeration yielding UTF-8 names. */
typedef struct u8_find {
    HANDLE handle;
    WIN32_FIND_DATAW data;
    char *name;
    DWORD attributes;
} u8_find;
static inline int u8_find_take(u8_find *f)
{
    free(f->name);
    f->name = u8_narrow(f->data.cFileName);
    f->attributes = f->data.dwFileAttributes;
    if (!f->name) SetLastError(ERROR_NO_UNICODE_TRANSLATION);
    return f->name ? 1 : 0;
}
/* 1 with the first entry, 0 for an empty or failed listing (see GetLastError). */
static inline int u8_find_first(u8_find *f, const char *directory)
{
    memset(f, 0, sizeof *f);
    size_t n = strlen(directory);
    char *pattern = malloc(n + 3);
    if (!pattern) { f->handle = INVALID_HANDLE_VALUE; SetLastError(ERROR_NOT_ENOUGH_MEMORY); return 0; }
    memcpy(pattern, directory, n);
    size_t at = n;
    if (n && directory[n - 1] != '/' && directory[n - 1] != '\\') pattern[at++] = '/';
    pattern[at++] = '*'; pattern[at] = 0;
    wchar_t *w = u8_widen(pattern);
    free(pattern);
    f->handle = w ? FindFirstFileExW(w, FindExInfoBasic, &f->data, FindExSearchNameMatch, NULL,
                                     FIND_FIRST_EX_LARGE_FETCH) : INVALID_HANDLE_VALUE;
    DWORD e = GetLastError(); free(w); SetLastError(e);
    return f->handle != INVALID_HANDLE_VALUE && u8_find_take(f);
}
static inline int u8_find_next(u8_find *f)
{
    return FindNextFileW(f->handle, &f->data) && u8_find_take(f);
}
static inline void u8_find_close(u8_find *f)
{
    if (f->handle != INVALID_HANDLE_VALUE) FindClose(f->handle);
    f->handle = INVALID_HANDLE_VALUE;
    free(f->name); f->name = NULL;
}
#endif
