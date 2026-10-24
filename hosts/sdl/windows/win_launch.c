/* Windows launch wrapper. Output goes to m8emu.log beside the executable
 * (or %APPDATA%/m8-live/m8emu.log when that folder is not writable), and a
 * failure or crash shows a message box naming the error and the log. The
 * GUI-subsystem executable opens no console; launched from a terminal, it
 * attaches to that terminal and prints there. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "win_utf8.h"
#include "win_launch.h"
#include <SDL3/SDL.h>
#include <fcntl.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static wchar_t log_path[MAX_PATH * 2];
static int owns_console;

static int open_log_at(const wchar_t *path)
{
    /* A GUI process starts without standard handles: reopen both streams.
     * Append mode plus no buffering keeps their lines whole and in order. */
    FILE *truncate = _wfopen(path, L"wb");
    if (!truncate) return -1;
    fclose(truncate);
    if (!_wfreopen(path, L"ab", stdout) || !_wfreopen(path, L"ab", stderr)) return -1;
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    wcsncpy(log_path, path, sizeof log_path / sizeof *log_path - 1);
    return 0;
}
/* SDL and the m8c renderer report through SDL_Log; keep that in the log. */
static void SDLCALL log_output(void *userdata, int category, SDL_LogPriority priority, const char *message)
{
    (void)userdata; (void)category;
    static const char *names[] = {"", "TRACE", "VERBOSE", "DEBUG", "INFO", "WARN", "ERROR", "CRITICAL"};
    const char *name = priority >= 0 && priority < (int)(sizeof names / sizeof *names) ? names[priority] : "";
    fprintf(stderr, "%s%s%s\n", name, *name ? ": " : "", message);
}
static int writable_log(const wchar_t *path)
{
    HANDLE h = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    CloseHandle(h);
    return 1;
}
static void open_log(void)
{
    wchar_t path[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(NULL, path, MAX_PATH * 2 - 16);
    if (n && n < MAX_PATH * 2 - 16) {
        wchar_t *slash = wcsrchr(path, L'\\');
        if (slash) {
            wcscpy(slash + 1, L"m8emu.log");
            if (writable_log(path) && !open_log_at(path)) return;
        }
    }
    const wchar_t *appdata = _wgetenv(L"APPDATA");
    if (appdata && *appdata && wcslen(appdata) < MAX_PATH) {
        _snwprintf(path, MAX_PATH * 2, L"%ls\\m8-live", appdata);
        CreateDirectoryW(path, NULL);
        wcscat(path, L"\\m8emu.log");
        if (!open_log_at(path)) return;
    }
    log_path[0] = 0; /* Keep the console. */
}

/* Last "first_failure=" or "ERROR" line in the log, for the message box. */
static void last_failure(char *out, size_t capacity)
{
    out[0] = 0;
    FILE *f = log_path[0] ? _wfopen(log_path, L"rb") : NULL;
    if (!f) return;
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        const char *text = NULL;
        if (!strncmp(line, "first_failure=", 14)) text = line + 14;
        else if (strstr(line, "ERROR")) text = line;
        if (text) {
            snprintf(out, capacity, "%s", text);
            size_t n = strlen(out);
            while (n && (out[n - 1] == '\n' || out[n - 1] == '\r')) out[--n] = 0;
        }
    }
    fclose(f);
}
static void report(const char *headline)
{
    char failure[1024], *path = u8_narrow(log_path), message[2600];
    last_failure(failure, sizeof failure);
    snprintf(message, sizeof message, "%s\n\n%s%s%sFull log:\n%s", headline,
             failure[0] ? "Error: " : "", failure, failure[0] ? "\n\n" : "", path ? path : "(none)");
    if (owns_console) {
        wchar_t *wide = u8_widen(message);
        MessageBoxW(NULL, wide ? wide : L"m8emu stopped. See m8emu.log.", L"M8 emulator",
                    MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
        free(wide);
    }
    free(path);
}
static LONG WINAPI crash_handler(EXCEPTION_POINTERS *info)
{
    DWORD code = info->ExceptionRecord->ExceptionCode;
    void *address = info->ExceptionRecord->ExceptionAddress;
    wchar_t module[MAX_PATH] = L"?";
    HMODULE base = NULL;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)address, &base))
        GetModuleFileNameW(base, module, MAX_PATH);
    const wchar_t *name = wcsrchr(module, L'\\');
    fprintf(stderr, "first_failure=Crash: exception 0x%08lx at %ls+0x%llx\n", (unsigned long)code,
            name ? name + 1 : module, (unsigned long long)((char *)address - (char *)base));
    fflush(stderr);
    report("m8emu crashed.");
    return EXCEPTION_EXECUTE_HANDLER;
}

static wchar_t *relaunch_line;

/* Quote one argument for CommandLineToArgvW / the CRT parser. */
static void append_quoted(wchar_t **out, size_t *used, size_t *capacity, const wchar_t *arg)
{
    size_t need = *used + wcslen(arg) * 2 + 4;
    if (need > *capacity) {
        size_t grown = need * 2;
        wchar_t *bigger = realloc(*out, grown * sizeof **out);
        if (!bigger) return;
        *out = bigger; *capacity = grown;
    }
    wchar_t *p = *out + *used;
    if (*used) *p++ = L' ';
    *p++ = L'"';
    for (const wchar_t *a = arg;; ++a) {
        size_t slashes = 0;
        while (*a == L'\\') { ++a; ++slashes; }
        if (!*a) { while (slashes--) { *p++ = L'\\'; *p++ = L'\\'; } break; }
        if (*a == L'"') { while (slashes--) { *p++ = L'\\'; *p++ = L'\\'; } *p++ = L'\\'; *p++ = L'"'; }
        else { while (slashes--) *p++ = L'\\'; *p++ = *a; }
    }
    *p++ = L'"';
    *p = 0;
    *used = (size_t)(p - *out);
}
void m8_win_request_relaunch(int argc, char **args)
{
    wchar_t exe[MAX_PATH * 2];
    if (!GetModuleFileNameW(NULL, exe, MAX_PATH * 2)) return;
    size_t used = 0, capacity = 0;
    wchar_t *line = NULL;
    append_quoted(&line, &used, &capacity, exe);
    for (int i = 1; i < argc; ++i) {
        wchar_t *wide = u8_widen(args[i]);
        if (wide) append_quoted(&line, &used, &capacity, wide);
        free(wide);
    }
    free(relaunch_line);
    relaunch_line = line;
}
static void relaunch(void)
{
    STARTUPINFOW startup;
    memset(&startup, 0, sizeof startup);
    startup.cb = sizeof startup;
    PROCESS_INFORMATION process;
    if (CreateProcessW(NULL, relaunch_line, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &process)) {
        CloseHandle(process.hThread); CloseHandle(process.hProcess);
    } else {
        fprintf(stderr, "first_failure=Cannot restart for import (error %lu)\n", (unsigned long)GetLastError());
        report("m8emu could not restart to import the folders.");
    }
}

int m8_win_launch(int (*run)(int, char **), int argc, char **argv)
{
    /* The executable uses the GUI subsystem, so a double-click opens no
     * console. From a terminal, attach to it and print there instead. */
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        if (_fileno(stdout) < 0 || _get_osfhandle(_fileno(stdout)) < 0) freopen("CONOUT$", "w", stdout);
        if (_fileno(stderr) < 0 || _get_osfhandle(_fileno(stderr)) < 0) freopen("CONOUT$", "w", stderr);
    } else {
        owns_console = 1; /* No terminal: log to a file, report with a box. */
        open_log();
        if (log_path[0]) SDL_SetLogOutputFunction(log_output, NULL);
    }
    SetUnhandledExceptionFilter(crash_handler);
    int result = run(argc, argv);
    fflush(stdout); fflush(stderr);
    if (result && log_path[0]) report("m8emu stopped with an error.");
    else if (!result && relaunch_line) relaunch();
    return result;
}
