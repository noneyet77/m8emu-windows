#ifndef M8_WIN_COMPAT_H
#define M8_WIN_COMPAT_H
/* Force-included into project sources on Windows (MinGW-w64) only.
 * Maps the small POSIX calls used outside core/platform onto the CRT.
 * Paths are UTF-8: the executable's manifest sets the active code page to
 * UTF-8 (hosts/sdl/windows/m8emu.manifest), so narrow CRT and Win32 calls
 * accept UTF-8. Binary I/O is the process default (main sets _fmode). */
#ifdef _WIN32
#include <direct.h>
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif
#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
#ifndef O_DIRECTORY
#define O_DIRECTORY 0
#endif

static inline int m8_win_mkdir(const char *path) { return _mkdir(path); }
#define mkdir(path, mode) m8_win_mkdir(path)
#define fsync(fd) _commit(fd)
#define fchmod(fd, mode) ((void)(fd), (void)(mode), 0)
/* Windows has no symlinks in the paths this app creates; reparse points are
 * rejected by the Windows storage adapters themselves. */
#define lstat stat

/* POSIX rename replaces an existing destination; the CRT's does not. */
__declspec(dllimport) int __stdcall MoveFileExA(const char *, const char *, unsigned long);
static inline int m8_win_rename(const char *from, const char *to)
{
    /* MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH */
    if (MoveFileExA(from, to, 0x1 | 0x8)) return 0;
    errno = EACCES;
    return -1;
}
#define rename(from, to) m8_win_rename(from, to)

/* tmpfile() in the user's temp folder (win_compat.c). */
FILE *m8_win_tmpfile(void);
#define tmpfile() m8_win_tmpfile()
#endif
#endif
