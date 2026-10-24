#define _POSIX_C_SOURCE 200809L
#include "user_paths.h"
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "win_utf8.h"
#endif

char *m8_user_join(const char *dir, const char *name)
{
    if (!dir || !name) { errno = EINVAL; return NULL; }
    size_t a = strlen(dir), b = strlen(name);
    if (a > SIZE_MAX - b - 2) { errno = ENOMEM; return NULL; }
    char *result = malloc(a + b + 2);
    if (!result) return NULL;
    memcpy(result, dir, a);
    result[a] = '/';
    memcpy(result + a + 1, name, b + 1);
    return result;
}

/* The same base as the default card in core/platform/card_platform_posix.c. */
char *m8_user_data_dir(void)
{
    const char *data = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
    if (data && *data) return m8_user_join(data, "m8-live");
#if defined(_WIN32)
    /* %APPDATA%/m8-live, the same base as core/platform/card_platform_win32.c. */
    char *appdata = u8_getenv("APPDATA");
    (void)home;
    if (appdata) {
        char *path = m8_user_join(appdata, "m8-live");
        free(appdata);
        if (path) for (char *p = path; *p; ++p) if (*p == '\\') *p = '/';
        return path;
    }
#elif defined(__APPLE__)
    if (home && *home) return m8_user_join(home, "Library/Application Support/m8-live");
#else
    if (home && *home) return m8_user_join(home, ".local/share/m8-live");
#endif
    errno = ENOENT;
    return NULL;
}

char *m8_user_firmware_path(void)
{
    char *data = m8_user_data_dir(), *dir = data ? m8_user_join(data, "firmware") : NULL;
    char *path = dir ? m8_user_join(dir, M8_USER_FIRMWARE_NAME) : NULL;
    free(dir); free(data);
    return path;
}

char *m8_user_engine_dir(const char *sha)
{
    char lower[65];
    if (!sha || strlen(sha) != 64) { errno = EINVAL; return NULL; }
    for (unsigned i = 0; i < 64; ++i) {
        char c = sha[i];
        if (c >= 'A' && c <= 'F') c = (char)(c - 'A' + 'a');
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) { errno = EINVAL; return NULL; }
        lower[i] = c;
    }
    lower[64] = 0;
    char *data = m8_user_data_dir(), *dir = data ? m8_user_join(data, "engine") : NULL;
    char *path = dir ? m8_user_join(dir, lower) : NULL;
    free(dir); free(data);
    return path;
}

int m8_user_make_parents(const char *path)
{
    char *copy = path ? strdup(path) : NULL;
    if (!copy) { if (path) errno = ENOMEM; else errno = EINVAL; return -1; }
#ifdef _WIN32
    for (char *p = copy; *p; ++p) if (*p == '\\') *p = '/';
#endif
    char *slash = strrchr(copy, '/');
    if (!slash || slash == copy) { free(copy); return 0; }
    *slash = 0;
    char *start = copy + 1;
#ifdef _WIN32
    /* Skip the drive root ("C:/") or UNC share ("//server/share/"). */
    if (copy[0] && copy[1] == ':' && copy[2] == '/') start = copy + 3;
    else if (copy[0] == '/' && copy[1] == '/') {
        char *s = strchr(copy + 2, '/');
        s = s ? strchr(s + 1, '/') : NULL;
        start = s ? s + 1 : copy + strlen(copy);
    }
    if (!*start && start > copy + 1) { free(copy); return 0; }
#endif
    for (char *p = start;; ++p) {
        if (*p && *p != '/') continue;
        char saved = *p; *p = 0;
        struct stat st;
        if (mkdir(copy, 0700) && errno != EEXIST) { free(copy); return -1; }
        if (stat(copy, &st) || !S_ISDIR(st.st_mode)) { free(copy); errno = ENOTDIR; return -1; }
        *p = saved;
        if (!saved) break;
    }
    free(copy);
    return 0;
}
