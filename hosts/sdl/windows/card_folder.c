/* A browsable mirror of the card in an ordinary folder (Documents\M8 Card),
 * with the card itself a sparse FAT32 image of a fixed size (6 GB default).
 *
 * Before the M8 starts: if the folder changed since the emulator last wrote
 * it (or the card is not yet a FAT32 card of the chosen size), the card is
 * rebuilt from the folder, streamed straight to disk and verified. A missing
 * folder is first created from the card. Dropped folders are copied into the
 * folder first, so they reach the card the same way.
 * After the card is saved: only what the M8 changed is copied back into the
 * folder, using the pages the M8 wrote this session.
 * Edits made in the folder while the emulator runs are never overwritten:
 * the session's card goes to a separate "emulator copy" folder instead, and
 * the next start takes the edited folder.
 *
 * "Changed" means a fingerprint of names, sizes and modification times
 * differs from the one recorded beside the card (<card>.folder). A sync
 * interrupted by a crash is detected there and redone from the card. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <winioctl.h>
#include "win_utf8.h"
#include "card_folder.h"
#include "media.h"
#include "fat32.h"
#include "storage.h"
#include <SDL3/SDL.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The card sizes itself to what is on it: content plus HEADROOM free for
 * new songs and renders, in whole GB, between CARD_MIN and CARD_MAX. Its
 * FAT is reserved for CARD_MAX, so resizing only rewrites size fields. */
#define GIB ((uint64_t)1 << 30)
#define HEADROOM (2 * GIB)
#define CARD_MIN (3 * GIB)
#define CARD_MAX (64 * GIB)
static uint64_t size_for(uint64_t used)
{
    uint64_t want = (used + HEADROOM + GIB - 1) / GIB * GIB;
    return want < CARD_MIN ? CARD_MIN : want > CARD_MAX ? CARD_MAX : want;
}
static uint64_t start_fingerprint;
static int folder_active, folder_conflict;

char *m8_card_folder_default(void)
{
    PWSTR documents = NULL;
    char *result = NULL;
    if (SUCCEEDED(SHGetKnownFolderPath(&FOLDERID_Documents, 0, NULL, &documents))) {
        char *base = u8_narrow(documents);
        if (base) {
            size_t n = strlen(base) + 16;
            result = malloc(n);
            if (result) snprintf(result, n, "%s\\M8 Card", base);
            free(base);
        }
    }
    CoTaskMemFree(documents);
    return result;
}

static int skipped(const WIN32_FIND_DATAW *e)
{
    return !wcscmp(e->cFileName, L".") || !wcscmp(e->cFileName, L"..") ||
           (e->dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) ||
           !_wcsicmp(e->cFileName, L"desktop.ini") || !_wcsicmp(e->cFileName, L"Thumbs.db");
}
static void mix(uint64_t *h, const void *data, size_t n)
{
    const unsigned char *p = data;
    for (size_t i = 0; i < n; ++i) { *h ^= p[i]; *h *= UINT64_C(0x100000001b3); }
}
/* Fingerprint of a folder tree in a stable (sorted by FindFirstFile on
 * NTFS) order. Returns 0 when the folder does not exist. */
static int fingerprint_into(const wchar_t *dir, uint64_t *h, unsigned depth)
{
    if (depth > 64) return -1;
    size_t n = wcslen(dir);
    wchar_t *pattern = malloc((n + 3) * sizeof *pattern);
    if (!pattern) return -1;
    swprintf(pattern, n + 3, L"%ls\\*", dir);
    WIN32_FIND_DATAW e;
    HANDLE find = FindFirstFileExW(pattern, FindExInfoBasic, &e, FindExSearchNameMatch, NULL, 0);
    free(pattern);
    if (find == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND ? 0 : -1;
    int result = 0;
    do {
        if (skipped(&e)) continue;
        int is_dir = (e.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        mix(h, e.cFileName, wcslen(e.cFileName) * sizeof(wchar_t));
        mix(h, &is_dir, sizeof is_dir);
        if (is_dir) {
            size_t m = n + wcslen(e.cFileName) + 2;
            wchar_t *child = malloc(m * sizeof *child);
            if (!child) { result = -1; break; }
            swprintf(child, m, L"%ls\\%ls", dir, e.cFileName);
            mix(h, "{", 1);
            result = fingerprint_into(child, h, depth + 1);
            mix(h, "}", 1);
            free(child);
            if (result) break;
        } else {
            mix(h, &e.nFileSizeHigh, sizeof e.nFileSizeHigh); mix(h, &e.nFileSizeLow, sizeof e.nFileSizeLow);
            mix(h, &e.ftLastWriteTime, sizeof e.ftLastWriteTime);
        }
    } while (FindNextFileW(find, &e));
    FindClose(find);
    return result;
}
static uint64_t fingerprint(const char *folder)
{
    wchar_t *w = u8_widen(folder);
    DWORD a = w ? GetFileAttributesW(w) : INVALID_FILE_ATTRIBUTES;
    if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) { free(w); return 0; }
    uint64_t h = UINT64_C(0xcbf29ce484222325);
    int failed = fingerprint_into(w, &h, 0);
    free(w);
    return failed ? 1 : (h ? h : 2); /* 0 means missing; 1 means unreadable. */
}

/* Recursive delete that never follows links or junctions. */
static int delete_tree(const wchar_t *path)
{
    DWORD a = GetFileAttributesW(path);
    if (a == INVALID_FILE_ATTRIBUTES) return GetLastError() == ERROR_FILE_NOT_FOUND ? 0 : -1;
    if (!(a & FILE_ATTRIBUTE_DIRECTORY) || (a & FILE_ATTRIBUTE_REPARSE_POINT)) {
        if (a & FILE_ATTRIBUTE_READONLY) SetFileAttributesW(path, a & ~FILE_ATTRIBUTE_READONLY);
        return (a & FILE_ATTRIBUTE_DIRECTORY ? RemoveDirectoryW(path) : DeleteFileW(path)) ? 0 : -1;
    }
    size_t n = wcslen(path);
    wchar_t *pattern = malloc((n + 3) * sizeof *pattern);
    if (!pattern) return -1;
    swprintf(pattern, n + 3, L"%ls\\*", path);
    WIN32_FIND_DATAW e;
    HANDLE find = FindFirstFileW(pattern, &e);
    free(pattern);
    int result = 0;
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(e.cFileName, L".") || !wcscmp(e.cFileName, L"..")) continue;
            size_t m = n + wcslen(e.cFileName) + 2;
            wchar_t *child = malloc(m * sizeof *child);
            if (!child) { result = -1; break; }
            swprintf(child, m, L"%ls\\%ls", path, e.cFileName);
            if (delete_tree(child)) result = -1;
            free(child);
        } while (FindNextFileW(find, &e));
        FindClose(find);
    }
    if (!RemoveDirectoryW(path)) result = -1;
    return result;
}
static wchar_t *wide_suffix(const char *base, const wchar_t *suffix)
{
    wchar_t *w = u8_widen(base);
    if (!w) return NULL;
    size_t n = wcslen(w) + wcslen(suffix) + 1;
    wchar_t *out = malloc(n * sizeof *out);
    if (out) swprintf(out, n, L"%ls%ls", w, suffix);
    free(w);
    return out;
}

/* Write the card into folder: export beside it, then swap it in. */
static int export_swap(const char *card, const char *folder, char *error, size_t capacity)
{
    wchar_t *target = u8_widen(folder), *staging = wide_suffix(folder, L".syncing"),
            *old = wide_suffix(folder, L".old");
    char *staging_u8 = staging ? u8_narrow(staging) : NULL;
    int result = -1;
    if (!target || !staging || !old || !staging_u8) { snprintf(error, capacity, "Out of memory"); goto done; }
    delete_tree(staging); delete_tree(old);
    if (m8_media_export(card, staging_u8, error, capacity)) goto done;
    DWORD a = GetFileAttributesW(target);
    if (a != INVALID_FILE_ATTRIBUTES && !MoveFileExW(target, old, 0)) {
        snprintf(error, capacity, "Cannot update the folder; close any program using files in it "
                 "(the new copy is in \"%s\")", staging_u8);
        goto done;
    }
    if (!MoveFileExW(staging, target, 0)) {
        MoveFileExW(old, target, 0);
        snprintf(error, capacity, "Cannot move the new copy into place (it is in \"%s\")", staging_u8);
        goto done;
    }
    delete_tree(old);
    result = 0;
done:
    free(target); free(staging); free(old); free(staging_u8);
    return result;
}

static void notify(const char *title, const char *text, int warning)
{
    printf("card_folder_%s=%s\n", warning ? "warning" : "note", text);
    SDL_ShowSimpleMessageBox(warning ? SDL_MESSAGEBOX_WARNING : SDL_MESSAGEBOX_INFORMATION, title, text, NULL);
}


/* ---- state beside the card ---- */

typedef struct { uint64_t fingerprint; int syncing; } folder_state;

static char *state_path(const char *card)
{
    size_t n = strlen(card) + 8;
    char *p = malloc(n);
    if (p) snprintf(p, n, "%s.folder", card);
    return p;
}
static folder_state load_state(const char *card, const char *folder)
{
    folder_state s = {0, 0};
    char *path = state_path(card), line[2048], saved_folder[2048] = "";
    wchar_t *w = u8_widen(path);
    FILE *f = w ? _wfopen(w, L"rb") : NULL;
    free(w); free(path);
    if (!f) return s;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strncmp(line, "folder=", 7)) snprintf(saved_folder, sizeof saved_folder, "%s", line + 7);
        else if (!strncmp(line, "fingerprint=", 12)) s.fingerprint = strtoull(line + 12, NULL, 16);
        else if (!strncmp(line, "syncing=", 8)) s.syncing = atoi(line + 8);
    }
    fclose(f);
    if (_stricmp(saved_folder, folder)) s.fingerprint = 0, s.syncing = 0;  /* Another folder: no history. */
    return s;
}
static void save_state(const char *card, const char *folder, uint64_t value, int syncing)
{
    char *path = state_path(card);
    wchar_t *w = u8_widen(path);
    FILE *f = w ? _wfopen(w, L"wb") : NULL;
    free(w); free(path);
    if (!f) return;
    fprintf(f, "folder=%s\nfingerprint=%016llx\nsyncing=%d\n", folder, (unsigned long long)value, syncing);
    fflush(f); _commit(_fileno(f));
    fclose(f);
}

/* ---- small helpers ---- */

static int failf(char *error, size_t capacity, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vsnprintf(error, capacity, format, args);
    va_end(args);
    return -1;
}

static wchar_t *wjoin(const wchar_t *dir, const wchar_t *name)
{
    size_t n = wcslen(dir) + wcslen(name) + 2;
    wchar_t *out = malloc(n * sizeof *out);
    if (out) swprintf(out, n, L"%ls\\%ls", dir, name);
    return out;
}
static int write_at(HANDLE h, uint64_t offset, const void *data, size_t n)
{
    const unsigned char *p = data;
    while (n) {
        OVERLAPPED at = {0};
        at.Offset = (DWORD)(offset & 0xffffffffu); at.OffsetHigh = (DWORD)(offset >> 32);
        DWORD want = n > 0x40000000u ? 0x40000000u : (DWORD)n, wrote = 0;
        if (!WriteFile(h, p, want, &wrote, &at) || !wrote) return -1;
        p += wrote; offset += wrote; n -= wrote;
    }
    return 0;
}
static int read_at(HANDLE h, uint64_t offset, void *out, size_t n)
{
    unsigned char *p = out;
    while (n) {
        OVERLAPPED at = {0};
        at.Offset = (DWORD)(offset & 0xffffffffu); at.OffsetHigh = (DWORD)(offset >> 32);
        DWORD want = n > 0x40000000u ? 0x40000000u : (DWORD)n, got = 0;
        if (!ReadFile(h, p, want, &got, &at) || !got) return -1;
        p += got; offset += got; n -= got;
    }
    return 0;
}
static const char *gb(uint64_t bytes, char *out, size_t n)
{
    snprintf(out, n, "%.1f GB", (double)bytes / (1024.0 * 1024 * 1024));
    return out;
}

/* ---- folder -> tree (for building) ---- */

static void free_host_tree(m8fat_node *n)
{
    if (!n) return;
    for (size_t i = 0; i < n->child_count; ++i) free_host_tree(n->children[i]), n->children[i] = NULL;
    n->child_count = 0;
    free(n->host);
    m8fat_node_free(n);
}
static int scan(const wchar_t *dir, m8fat_node *parent, unsigned depth, uint64_t *total, char *error, size_t capacity)
{
    if (depth > 60) return failf(error, capacity, "Folders are nested too deeply");
    wchar_t *pattern = wjoin(dir, L"*");
    if (!pattern) return failf(error, capacity, "Out of memory");
    WIN32_FIND_DATAW e;
    HANDLE find = FindFirstFileExW(pattern, FindExInfoBasic, &e, FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
    free(pattern);
    if (find == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND ? 0 : (failf(error, capacity, "Cannot read the card folder"));
    int result = 0;
    do {
        if (skipped(&e)) continue;
        char *name = u8_narrow(e.cFileName);
        if (!name) { result = failf(error, capacity, "Out of memory"); break; }
        if (e.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
            snprintf(error, capacity, "Shortcuts and links cannot go on the card: %s", name);
            free(name); result = -1; break;
        }
        int is_dir = (e.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        m8fat_node *node = m8fat_node_new(name, is_dir);
        free(name);
        wchar_t *path = wjoin(dir, e.cFileName);
        if (!node || !path || m8fat_node_add(parent, node)) {
            free(path); m8fat_node_free(node); result = failf(error, capacity, "Out of memory"); break;
        }
        FILETIME local; WORD date = 0, time = 0;
        if (FileTimeToLocalFileTime(&e.ftLastWriteTime, &local) && FileTimeToDosDateTime(&local, &date, &time))
            node->date = date, node->time = time;
        if (is_dir) {
            result = scan(path, node, depth + 1, total, error, capacity);
            free(path);
            if (result) break;
        } else {
            node->size = (uint64_t)e.nFileSizeHigh << 32 | e.nFileSizeLow;
            node->host = path;
            *total += node->size;
        }
    } while (FindNextFileW(find, &e));
    FindClose(find);
    return result;
}

typedef struct { HANDLE image, file; const m8fat_node *open; } build_ctx;
static int sink_write(void *c, uint64_t offset, const void *data, size_t n) { return write_at(((build_ctx *)c)->image, offset, data, n); }
static int sink_read(void *c, const m8fat_node *file, uint64_t offset, void *out, size_t n)
{
    build_ctx *b = c;
    if (b->open != file) {
        if (b->file != INVALID_HANDLE_VALUE) CloseHandle(b->file);
        b->file = CreateFileW(file->host, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
        b->open = b->file == INVALID_HANDLE_VALUE ? NULL : file;
        if (!b->open) return -1;
    }
    return read_at(b->file, offset, out, n);
}
typedef struct { HANDLE h; } handle_ctx;
static int handle_source_read(void *c, uint64_t offset, void *out, size_t n) { return read_at(((handle_ctx *)c)->h, offset, out, n); }
typedef struct { uint64_t h; } hash_ctx;
static int hash_out(void *c, const void *data, size_t n)
{
    hash_ctx *x = c; const unsigned char *p = data;
    for (size_t i = 0; i < n; ++i) { x->h ^= p[i]; x->h *= UINT64_C(0x100000001b3); }
    return 0;
}
/* The built image must hold exactly the tree that was written. */
static int same(m8fat_volume *v, const m8fat_node *a, const m8fat_node *b, char *error, size_t capacity)
{
    if (a->child_count != b->child_count) return failf(error, capacity, "Card check failed in %s", *a->name ? a->name : "(root)");
    for (size_t i = 0; i < a->child_count; ++i) {
        const m8fat_node *x = a->children[i], *y = b->children[i];
        if (strcmp(x->name, y->name) || x->directory != y->directory || x->size != y->size)
            return failf(error, capacity, "Card check failed at %s", x->name);
        if (x->directory) { if (same(v, x, y, error, capacity)) return -1; continue; }
        hash_ctx h = {UINT64_C(0xcbf29ce484222325)};
        if (m8fat_stream(v, y, hash_out, &h) || h.h != x->hash)
            return failf(error, capacity, "Card check failed: the bytes of %s differ", x->name);
    }
    return 0;
}
/* Build a card image of the folder beside the card; returns its path. */
static char *build_card(const char *card, const char *folder, uint64_t *built_bytes, char *error, size_t capacity)
{
    uint64_t bytes = 0;
    wchar_t *wfolder = u8_widen(folder);
    m8fat_node *root = m8fat_node_new("", 1);
    uint64_t total = 0;
    char *out = NULL, a[32], b[32];
    size_t n = strlen(card) + 16;
    char *building = malloc(n);
    if (!wfolder || !root || !building) { snprintf(error, capacity, "Out of memory"); goto done; }
    snprintf(building, n, "%s.building", card);
    if (scan(wfolder, root, 0, &total, error, capacity)) goto done;
    uint64_t required = m8fat_required(root, CARD_MAX);
    if (required == UINT64_MAX || required + HEADROOM > CARD_MAX) {
        if (total + HEADROOM > CARD_MAX)
            snprintf(error, capacity, "The card folder holds %s; the largest card is %s", gb(total, a, sizeof a), gb(CARD_MAX, b, sizeof b));
        else snprintf(error, capacity, "The card folder holds a name the M8's card cannot use, or a folder with too many items");
        goto done;
    }
    bytes = size_for(required);
    *built_bytes = bytes;
    HANDLE image = u8_CreateFile(building, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (image == INVALID_HANDLE_VALUE) { snprintf(error, capacity, "Cannot create the new card image"); goto done; }
    DWORD returned;
    DeviceIoControl(image, FSCTL_SET_SPARSE, NULL, 0, NULL, 0, &returned, NULL); /* best effort */
    LARGE_INTEGER end; end.QuadPart = (LONGLONG)bytes;
    build_ctx ctx = {image, INVALID_HANDLE_VALUE, NULL};
    m8fat_sink sink = {&ctx, sink_write, sink_read};
    int ok = SetFilePointerEx(image, end, NULL, FILE_BEGIN) && SetEndOfFile(image);
    if (!ok) snprintf(error, capacity, "Not enough disk space for the card image");
    else if (m8fat_build(root, bytes, CARD_MAX, &sink, error, capacity)) ok = 0;
    if (ctx.file != INVALID_HANDLE_VALUE) CloseHandle(ctx.file);
    if (ok && !FlushFileBuffers(image)) { ok = 0; snprintf(error, capacity, "Cannot write the card image to disk"); }
    if (ok) {
        handle_ctx hc = {image};
        m8fat_source source = {&hc, handle_source_read, bytes};
        m8fat_volume *v = m8fat_open(&source, error, capacity);
        m8fat_node *check = v ? m8fat_tree(v, error, capacity) : NULL;
        ok = check && !same(v, root, check, error, capacity);
        m8fat_node_free(check); m8fat_close(v);
    }
    CloseHandle(image);
    if (ok) { out = building; building = NULL; }
    else u8_DeleteFile(building);
done:
    free(building); free(wfolder); free_host_tree(root);
    return out;
}

/* ---- card -> folder ---- */

typedef struct { HANDLE h; } file_out;
static int write_out(void *c, const void *data, size_t n)
{
    DWORD wrote = 0;
    const unsigned char *p = data;
    while (n) {
        if (!WriteFile(((file_out *)c)->h, p, n > 0x40000000u ? 0x40000000u : (DWORD)n, &wrote, NULL) || !wrote) return -1;
        p += wrote; n -= wrote;
    }
    return 0;
}
typedef struct { wchar_t *name; DWORD attributes; uint64_t size; int matched; } host_entry;

/* Make host hold exactly dir. Files are rewritten when new, resized, or on
 * pages the M8 wrote (dirty NULL: rewrite all), or when their folder's own
 * entries were written (a rename or replacement). */
static int sync_dir(m8fat_volume *v, const m8fat_node *dir, const wchar_t *host, const uint8_t *dirty,
                    char *error, size_t capacity)
{
    DWORD a = GetFileAttributesW(host);
    if (a != INVALID_FILE_ATTRIBUTES && (!(a & FILE_ATTRIBUTE_DIRECTORY) || (a & FILE_ATTRIBUTE_REPARSE_POINT))) {
        delete_tree(host); a = INVALID_FILE_ATTRIBUTES;
    }
    if (a == INVALID_FILE_ATTRIBUTES && !CreateDirectoryW(host, NULL))
        return failf(error, capacity, "Cannot create a folder in the card folder");
    host_entry *entries = NULL;
    size_t count = 0, cap = 0;
    wchar_t *pattern = wjoin(host, L"*");
    WIN32_FIND_DATAW e;
    HANDLE find = pattern ? FindFirstFileW(pattern, &e) : INVALID_HANDLE_VALUE;
    free(pattern);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(e.cFileName, L".") || !wcscmp(e.cFileName, L"..")) continue;
            if (count == cap) {
                cap = cap ? cap * 2 : 32;
                host_entry *grown = realloc(entries, cap * sizeof *grown);
                if (!grown) break;
                entries = grown;
            }
            host_entry *h = &entries[count++];
            h->name = _wcsdup(e.cFileName); h->attributes = e.dwFileAttributes;
            h->size = (uint64_t)e.nFileSizeHigh << 32 | e.nFileSizeLow;
            h->matched = skipped(&e);   /* clutter is left alone */
        } while (FindNextFileW(find, &e));
        FindClose(find);
    }
    int rewrite_all = !dirty || m8fat_touches(v, dir, dirty, M8RT_SD_PAGE);
    int result = 0;
    for (size_t i = 0; i < dir->child_count && !result; ++i) {
        const m8fat_node *n = dir->children[i];
        wchar_t *wname = u8_widen(n->name), *path = wname ? wjoin(host, wname) : NULL;
        if (!path) { free(wname); result = failf(error, capacity, "Out of memory"); break; }
        host_entry *found = NULL;
        for (size_t k = 0; k < count; ++k)
            if (entries[k].name && CompareStringOrdinal(entries[k].name, -1, wname, -1, TRUE) == CSTR_EQUAL) { found = &entries[k]; break; }
        if (found) {
            found->matched = 1;
            if (wcscmp(found->name, wname)) {                 /* case-only rename */
                wchar_t *old = wjoin(host, found->name);
                if (old) MoveFileW(old, path);
                free(old);
            }
        }
        if (n->directory) {
            result = sync_dir(v, n, path, dirty, error, capacity);
        } else {
            if (found && (found->attributes & FILE_ATTRIBUTE_DIRECTORY)) { delete_tree(path); found = NULL; }
            int need = !found || found->size != n->size || rewrite_all || m8fat_touches(v, n, dirty, M8RT_SD_PAGE);
            if (need) {
                if (found && (found->attributes & FILE_ATTRIBUTE_READONLY))
                    SetFileAttributesW(path, found->attributes & ~FILE_ATTRIBUTE_READONLY);
                file_out out = {CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL)};
                if (out.h == INVALID_HANDLE_VALUE) result = failf(error, capacity, "Cannot write %s in the card folder (is it open in another program?)", n->name);
                else {
                    if (m8fat_stream(v, n, write_out, &out)) result = failf(error, capacity, "Cannot copy %s from the card", n->name);
                    CloseHandle(out.h);
                }
            }
        }
        free(wname); free(path);
    }
    for (size_t k = 0; k < count; ++k) {
        if (!result && !entries[k].matched && entries[k].name) {
            wchar_t *path = wjoin(host, entries[k].name);
            if (path && delete_tree(path)) result = failf(error, capacity, "Cannot remove a deleted item from the card folder (is it open in another program?)");
            free(path);
        }
        free(entries[k].name);
    }
    free(entries);
    return result;
}
static int sync_volume(m8fat_volume *v, const char *folder, const uint8_t *dirty, char *error, size_t capacity)
{
    m8fat_node *root = m8fat_tree(v, error, capacity);
    wchar_t *w = u8_widen(folder);
    int result = root && w ? sync_dir(v, root, w, dirty, error, capacity) : -1;
    free(w); m8fat_node_free(root);
    return result;
}
typedef struct { m8rt *rt; } runtime_ctx;
static int runtime_read(void *c, uint64_t offset, void *out, size_t n)
{
    return m8rt_sd_read_image(((runtime_ctx *)c)->rt, (size_t)offset, out, n);
}
/* Open the card file (as it is on disk) as a FAT32 volume. */
static m8fat_volume *open_card_file(const char *card, HANDLE *h, uint64_t *bytes)
{
    *h = u8_CreateFile(card, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    LARGE_INTEGER size;
    if (*h == INVALID_HANDLE_VALUE || !GetFileSizeEx(*h, &size)) return NULL;
    *bytes = (uint64_t)size.QuadPart;
    static handle_ctx ctx;
    ctx.h = *h;
    m8fat_source source = {&ctx, handle_source_read, *bytes};
    char error[256];
    return m8fat_open(&source, error, sizeof error);
}

/* ---- dropped folders ---- */

/* Files identical to what is already there are skipped; a different file of
 * the same name is a conflict. check = 1 only looks for conflicts. */
static int same_bytes(const wchar_t *a, const wchar_t *b)
{
    HANDLE x = CreateFileW(a, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    HANDLE y = CreateFileW(b, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    int same_content = x != INVALID_HANDLE_VALUE && y != INVALID_HANDLE_VALUE;
    static unsigned char p[1 << 16], q[1 << 16];
    while (same_content) {
        DWORD m = 0, n = 0;
        if (!ReadFile(x, p, sizeof p, &m, NULL) || !ReadFile(y, q, sizeof q, &n, NULL) || m != n || memcmp(p, q, m)) same_content = 0;
        else if (!m) break;
    }
    if (x != INVALID_HANDLE_VALUE) CloseHandle(x);
    if (y != INVALID_HANDLE_VALUE) CloseHandle(y);
    return same_content;
}
static int copy_drop(const wchar_t *from, const wchar_t *to, int check, char *error, size_t capacity)
{
    if (!check && !CreateDirectoryW(to, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
        return failf(error, capacity, "Cannot create a folder in the card folder");
    wchar_t *pattern = wjoin(from, L"*");
    WIN32_FIND_DATAW e;
    HANDLE find = pattern ? FindFirstFileW(pattern, &e) : INVALID_HANDLE_VALUE;
    free(pattern);
    if (find == INVALID_HANDLE_VALUE) return 0;
    int result = 0;
    do {
        if (skipped(&e)) continue;
        wchar_t *src = wjoin(from, e.cFileName), *dst = wjoin(to, e.cFileName);
        char *name = u8_narrow(e.cFileName);
        if (!src || !dst || !name) result = failf(error, capacity, "Out of memory");
        else if (e.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
            result = failf(error, capacity, "Shortcuts and links cannot go on the card: %s", name);
        else if (e.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            DWORD a = GetFileAttributesW(dst);
            if (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY))
                result = failf(error, capacity, "A file on the card has the same name as the folder %s", name);
            else result = copy_drop(src, dst, check, error, capacity);
        } else {
            DWORD a = GetFileAttributesW(dst);
            if (a != INVALID_FILE_ATTRIBUTES) {
                if ((a & FILE_ATTRIBUTE_DIRECTORY) || !same_bytes(src, dst))
                    result = failf(error, capacity, "A different file with this name is already on the card: %s", name);
            } else if (!check && !CopyFileW(src, dst, TRUE))
                result = failf(error, capacity, "Cannot copy %s into the card folder", name);
        }
        free(src); free(dst); free(name);
    } while (!result && FindNextFileW(find, &e));
    FindClose(find);
    return result;
}

/* ---- entry points ---- */

typedef struct { HANDLE h; } write_ctx;
static int handle_write(void *c, uint64_t offset, const void *data, size_t n) { return write_at(((write_ctx *)c)->h, offset, data, n); }
/* Grow or shrink the card to suit what is on it. Growing extends the file
 * first, shrinking truncates it last, so an interruption leaves a card
 * whose size fields never exceed its file. */
static void fit_card(m8_card *card)
{
    const char *path = m8_card_path(card);
    char error[256], a[32], b[32];
    HANDLE h = u8_CreateFile(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    LARGE_INTEGER size;
    if (h == INVALID_HANDLE_VALUE || !GetFileSizeEx(h, &size)) { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); return; }
    handle_ctx hc = {h};
    m8fat_source source = {&hc, handle_source_read, (uint64_t)size.QuadPart};
    m8fat_volume *v = m8fat_open(&source, error, sizeof error);
    if (!v) { CloseHandle(h); return; }
    uint64_t used, minimum, maximum, current = (uint64_t)size.QuadPart;
    m8fat_usage(v, &used, &minimum, &maximum);
    uint64_t target = size_for(used);
    if (target < minimum) target = (minimum + GIB - 1) / GIB * GIB;   /* files sit high: shrink less */
    if (target > maximum) target = maximum / GIB * GIB;
    if (target != current) {
        LARGE_INTEGER end;
        end.QuadPart = (LONGLONG)target;
        write_ctx wc = {h};
        int ok = 1;
        if (target > current) ok = SetFilePointerEx(h, end, NULL, FILE_BEGIN) && SetEndOfFile(h);
        if (ok) ok = !m8fat_resize(v, target, handle_write, &wc, error, sizeof error) && FlushFileBuffers(h);
        if (ok && target < current) ok = SetFilePointerEx(h, end, NULL, FILE_BEGIN) && SetEndOfFile(h);
        if (ok) printf("card_resized=%s from=%s to=%s used=%s\n", path, gb(current, a, sizeof a), gb(target, b, sizeof b), gb(used, error, sizeof error));
        else printf("card_resize_skipped=%s\n", error);
    }
    m8fat_close(v);
    CloseHandle(h);
    m8_card_refresh(card);
}

int m8_card_folder_prepare(m8_card *card, const char *folder,
                           const char *const *drops, const char *const *destinations, unsigned drop_count)
{
    char error[512], text[1600];
    const char *path = m8_card_path(card);
    folder_active = 1; folder_conflict = 0;
    folder_state state = load_state(path, folder);
    HANDLE h;
    uint64_t card_bytes = 0, used = 0, minimum = 0, maximum = 0;
    m8fat_volume *v = open_card_file(path, &h, &card_bytes);
    int fat32 = v != NULL;
    if (v) m8fat_usage(v, &used, &minimum, &maximum);
    (void)card_bytes;
    /* Cards made before resizing existed cannot grow: rebuild them once. */
    int growable = fat32 && maximum >= CARD_MAX - GIB;

    if (!fingerprint(folder) || state.syncing) {
        /* No folder yet, or the last copy into it was interrupted: (re)make it from the card. */
        int r;
        save_state(path, folder, 0, 1);
        if (fat32) r = sync_volume(v, folder, NULL, error, sizeof error);
        else r = export_swap(path, folder, error, sizeof error);
        if (r) {
            m8fat_close(v); if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
            snprintf(text, sizeof text, "Could not create the card folder:\n%s\n\n%s", folder, error);
            notify("M8 Card folder", text, 1);
            folder_active = 0;
            return -1;
        }
        state.fingerprint = fingerprint(folder);
        save_state(path, folder, state.fingerprint, 0);
        printf("card_folder_created=%s\n", folder);
    }
    m8fat_close(v);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);

    /* Dropped folders go into the card folder, then onto the card with it. */
    int dropped = 0;
    wchar_t *wfolder = u8_widen(folder);
    for (unsigned i = 0; wfolder && i < drop_count; ++i) {
        wchar_t *from = u8_widen(drops[i]), *dest = u8_widen(destinations[i]), *to = NULL;
        if (from && dest) {
            for (wchar_t *p = dest; *p; ++p) if (*p == L'/') *p = L'\\';
            to = *dest ? wjoin(wfolder, dest) : _wcsdup(wfolder);
        }
        if (to && *dest) {   /* create the parents of /Samples/<name> */
            for (wchar_t *p = to + wcslen(wfolder) + 1; *p; ++p)
                if (*p == L'\\') { *p = 0; CreateDirectoryW(to, NULL); *p = L'\\'; }
        }
        if (!to || copy_drop(from, to, 1, error, sizeof error) || copy_drop(from, to, 0, error, sizeof error)) {
            snprintf(text, sizeof text, "%s was not added to the card:\n%s", drops[i], to ? error : "Out of memory");
            notify("Add to card", text, 1);
        } else {
            dropped = 1;
            printf("imported=%s destination=/%s\n", drops[i], destinations[i]);
        }
        free(from); free(dest); free(to);
    }
    free(wfolder);

    uint64_t current = fingerprint(folder);
    if (current == 1) {
        snprintf(text, sizeof text, "The card folder could not be read, so it was not applied:\n%s\n\n"
                 "The M8 starts with the card as it was. The folder is left as it is.", folder);
        notify("M8 Card folder", text, 1);
        folder_conflict = 1; start_fingerprint = current;
        return -1;
    }
    start_fingerprint = current;
    if (growable && current == state.fingerprint && !dropped) { fit_card(card); return 0; }
    uint64_t bytes = 0;
    char *built = build_card(path, folder, &bytes, error, sizeof error);
    if (!built || m8_card_replace_image(card, built)) {
        if (built) { snprintf(error, sizeof error, "%s", m8_card_error(card)); u8_DeleteFile(built); }
        snprintf(text, sizeof text, "Your card folder could not be put on the card:\n%s\n\n"
                 "The M8 starts with the card as it was, and the folder is left untouched so you can fix it.", error);
        notify("M8 Card folder", text, 1);
        free(built);
        folder_conflict = 1;
        return -1;
    }
    free(built);
    save_state(path, folder, current, 0);
    printf("card_folder_applied=%s card_bytes=%llu\n", folder, (unsigned long long)bytes);
    return 0;
}

void m8_card_folder_finish(m8_card *card, m8rt *rt, const char *folder)
{
    if (!folder_active) return;
    const char *path = m8_card_path(card);
    char error[512], text[1600];
    runtime_ctx rc = {rt};
    m8fat_source source = {&rc, runtime_read, m8rt_sd_image_size(rt)};
    m8fat_volume *v = m8fat_open(&source, error, sizeof error);
    uint64_t now = fingerprint(folder);
    if (folder_conflict || now != start_fingerprint) {
        /* The folder holds edits the card does not have: keep them, and put
         * this session's card in a separate copy instead. */
        SYSTEMTIME t;
        GetLocalTime(&t);
        char copy[2048];
        snprintf(copy, sizeof copy, "%s (emulator copy %04u-%02u-%02u %02u%02u)", folder, t.wYear, t.wMonth,
                 t.wDay, t.wHour, t.wMinute);
        int r = v ? sync_volume(v, copy, NULL, error, sizeof error) : export_swap(path, copy, error, sizeof error);
        if (r)
            snprintf(text, sizeof text, "The card folder was changed while the M8 was running, so it was left "
                     "as it is. Saving this session's card to a separate folder also failed:\n%s", error);
        else
            snprintf(text, sizeof text, "The card folder was changed while the M8 was running, so it was left "
                     "as it is and will be put on the card next time.\n\nThis session's card (including any "
                     "songs you saved) is in:\n%s", copy);
        notify("M8 Card folder", text, 1);
        m8fat_close(v);
        return;
    }
    int all = 1;
    const uint8_t *dirty = m8rt_sd_dirty_pages(rt, &all);
    int changed = all || !dirty;
    for (size_t i = 0, n = (m8rt_sd_image_size(rt) / M8RT_SD_PAGE + 7) / 8; !changed && i < n; ++i) changed = dirty[i] != 0;
    if (!changed) { m8fat_close(v); printf("card_folder_unchanged=%s\n", folder); return; }
    save_state(path, folder, 0, 1);
    int r = v ? sync_volume(v, folder, all ? NULL : dirty, error, sizeof error) : export_swap(path, folder, error, sizeof error);
    m8fat_close(v);
    if (r) {
        snprintf(text, sizeof text, "The card was saved, but the card folder could not be fully updated:\n%s\n\n"
                 "It will be refreshed from the card next time.", error);
        notify("M8 Card folder", text, 1);
        return;   /* syncing=1 stays: the next start remakes the folder from the card */
    }
    save_state(path, folder, fingerprint(folder), 0);
    printf("card_folder_updated=%s\n", folder);
}
