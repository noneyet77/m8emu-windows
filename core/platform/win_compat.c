/* Out-of-line pieces of win_compat.h (Windows builds only). */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <stdint.h>
#include <stdio.h>

/* The CRT's tmpfile() creates its file in the root of the system drive,
 * which standard accounts cannot write. Use the user's temp folder; the
 * file is deleted when it is closed or the process ends. */
FILE *m8_win_tmpfile(void)
{
    wchar_t dir[MAX_PATH + 1], path[MAX_PATH + 64];
    DWORD n = GetTempPathW(MAX_PATH + 1, dir);
    if (!n || n > MAX_PATH) return NULL;
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
        _snwprintf(path, sizeof path / sizeof *path, L"%lsm8emu-%lu-%lu-%u.tmp", dir,
                   (unsigned long)GetCurrentProcessId(), (unsigned long)GetTickCount(), attempt);
        HANDLE h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_NEW,
                               FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
        if (h == INVALID_HANDLE_VALUE) {
            if (GetLastError() == ERROR_FILE_EXISTS) continue;
            return NULL;
        }
        int fd = _open_osfhandle((intptr_t)h, _O_RDWR | _O_BINARY);
        if (fd < 0) { CloseHandle(h); return NULL; }
        FILE *f = _fdopen(fd, "w+b");
        if (!f) _close(fd);
        return f;
    }
    return NULL;
}
#endif
