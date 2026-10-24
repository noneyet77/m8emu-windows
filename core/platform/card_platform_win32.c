/* Windows adapter for card_platform.h. Same contract as the POSIX adapter:
 * one exclusive lock, source-change detection, staged and verified images,
 * .bak published before the card, partial publication preserved on failure.
 * Paths are UTF-8 (the executable manifest selects the UTF-8 code page) and
 * are normalized to forward slashes internally. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "win_utf8.h"
#include "card_platform.h"
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define COPY_SIZE ((size_t)64 * 1024)

typedef struct identity {
    DWORD volume, index_high, index_low, attributes;
    uint64_t size;
    FILETIME write_time;
} identity;

struct protected_file {
    char *path;
    identity id;
    struct protected_file *next;
};

struct m8_card_platform {
    char *path, *backup, *directory, *lock, *journal;
    HANDLE lock_handle, source;
    int read_only;
    struct protected_file *protected_files;
    identity source_id;
    char error[512];
};

static int fail(m8_card_platform *card, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vsnprintf(card->error, sizeof(card->error), format, args);
    va_end(args);
    return -1;
}
static const char *win_error(void)
{
    static char text[160];
    DWORD code = GetLastError();
    DWORD n = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, code,
                             0, text, sizeof text, NULL);
    while (n && (text[n - 1] == '\r' || text[n - 1] == '\n' || text[n - 1] == '.')) text[--n] = 0;
    if (!n) snprintf(text, sizeof text, "Windows error %lu", (unsigned long)code);
    return text;
}
static char *append(const char *base, const char *suffix)
{
    size_t a = strlen(base), b = strlen(suffix);
    if (a > SIZE_MAX - b - 1) return NULL;
    char *result = malloc(a + b + 1);
    if (result) { memcpy(result, base, a); memcpy(result + a, suffix, b + 1); }
    return result;
}

/* Length of the part of an absolute path that cannot be created: "C:/" or
 * "//server/share/". */
static size_t root_length(const char *path)
{
    if (path[0] && path[1] == ':' && path[2] == '/') return 3;
    if (path[0] == '/' && path[1] == '/') {
        const char *p = strchr(path + 2, '/');
        p = p ? strchr(p + 1, '/') : NULL;
        return p ? (size_t)(p - path) + 1 : strlen(path);
    }
    return path[0] == '/' ? 1 : 0;
}
static char *absolute_path(const char *path)
{
    char *expanded = NULL;
    if (!path) {
        char *data = u8_getenv("XDG_DATA_HOME");
        if (!data) data = u8_getenv("APPDATA");
        if (!data) { SetLastError(ERROR_PATH_NOT_FOUND); return NULL; }
        expanded = append(data, "/m8-live/card.img");
        free(data);
    } else if (path[0] == '~' && (!path[1] || path[1] == '/' || path[1] == '\\')) {
        char *home = u8_getenv("USERPROFILE");
        if (!home) { SetLastError(ERROR_PATH_NOT_FOUND); return NULL; }
        expanded = append(home, path + 1);
        free(home);
    } else expanded = append(path, "");
    if (!expanded) return NULL;
    char *full = u8_full_path(expanded);
    free(expanded);
    return full;
}
static int query_attributes(const char *path, DWORD *attributes)
{
    *attributes = u8_GetFileAttributes(path);
    return *attributes == INVALID_FILE_ATTRIBUTES ? -1 : 0;
}
static int missing_error(void)
{
    DWORD e = GetLastError();
    return e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND;
}
/* Regular file: not a directory and not a reparse point (link/junction). */
static int regular_path(m8_card_platform *card, const char *path, int missing_ok)
{
    DWORD attributes;
    if (query_attributes(path, &attributes)) {
        if (missing_ok && missing_error()) return 0;
        return fail(card, "Cannot inspect %s: %s", path, win_error());
    }
    if (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE))
        return fail(card, "Card file must be a regular file: %s", path);
    return 0;
}
static int make_directories(m8_card_platform *card)
{
    char *path = append(card->directory, "");
    if (!path) return fail(card, "Cannot allocate card directory path");
    size_t start = root_length(path);
    if (start >= strlen(path)) { free(path); return 0; }
    for (char *p = path + (start ? start : 1);; p++) {
        if (*p && *p != '/') continue;
        char saved = *p; *p = 0;
        if (*path && !u8_CreateDirectory(path) && GetLastError() != ERROR_ALREADY_EXISTS) {
            fail(card, "Cannot create %s: %s", path, win_error());
            free(path); return -1;
        }
        DWORD attributes;
        if (query_attributes(path, &attributes) || !(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
            fail(card, "Card parent must be a directory: %s", path);
            free(path); return -1;
        }
        *p = saved;
        if (!saved || !p[1]) break;
    }
    free(path); return 0;
}
static int handle_identity(HANDLE handle, identity *id)
{
    BY_HANDLE_FILE_INFORMATION info;
    if (!GetFileInformationByHandle(handle, &info)) return -1;
    id->volume = info.dwVolumeSerialNumber;
    id->index_high = info.nFileIndexHigh; id->index_low = info.nFileIndexLow;
    id->attributes = info.dwFileAttributes;
    id->size = ((uint64_t)info.nFileSizeHigh << 32) | info.nFileSizeLow;
    id->write_time = info.ftLastWriteTime;
    return 0;
}
/* Identity of a path without following a reparse point and without holding
 * it open: other handles keep their share modes. */
static int path_identity(const char *path, identity *id)
{
    HANDLE h = u8_CreateFile(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h == INVALID_HANDLE_VALUE) return -1;
    int result = handle_identity(h, id);
    CloseHandle(h);
    return result;
}
static int same_file(const identity *a, const identity *b)
{
    return a->volume == b->volume && a->index_high == b->index_high && a->index_low == b->index_low;
}
static int handle_read(void *context, size_t offset, unsigned char *out, size_t n)
{
    HANDLE handle = *(HANDLE *)context;
    while (n) {
        OVERLAPPED at = {0};
        at.Offset = (DWORD)((uint64_t)offset & 0xffffffffu);
        at.OffsetHigh = (DWORD)((uint64_t)offset >> 32);
        DWORD want = n > 0x40000000u ? 0x40000000u : (DWORD)n, got = 0;
        if (!ReadFile(handle, out, want, &got, &at) || !got) return -1;
        out += got; offset += got; n -= got;
    }
    return 0;
}
static int write_all(HANDLE handle, const unsigned char *data, size_t n)
{
    while (n) {
        DWORD want = n > 0x40000000u ? 0x40000000u : (DWORD)n, wrote = 0;
        if (!WriteFile(handle, data, want, &wrote, NULL) || !wrote) return -1;
        data += wrote; n -= wrote;
    }
    return 0;
}
static void remove_stage(char **path)
{
    if (*path) { u8_DeleteFile(*path); free(*path); *path = NULL; }
}
static int stage_image(m8_card_platform *card, const char *target, size_t size,
                       m8_card_platform_reader reader, void *context, char **stage)
{
    int result = -1;
    HANDLE handle = INVALID_HANDLE_VALUE;
    unsigned char *data = NULL, *check = NULL;
    static unsigned counter;
    size_t length = strlen(target) + 48;
    *stage = malloc(length);
    if (!*stage) return fail(card, "Cannot allocate temporary card path");
    for (unsigned attempt = 0; attempt < 100 && handle == INVALID_HANDLE_VALUE; ++attempt) {
        snprintf(*stage, length, "%s.tmp.%lu.%u", target, (unsigned long)GetCurrentProcessId(), ++counter);
        handle = u8_CreateFile(*stage, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_NEW,
                             FILE_ATTRIBUTE_NORMAL, NULL);
        if (handle == INVALID_HANDLE_VALUE && GetLastError() != ERROR_FILE_EXISTS) break;
    }
    if (handle == INVALID_HANDLE_VALUE) {
        fail(card, "Cannot create temporary card: %s", win_error());
        free(*stage); *stage = NULL; return -1;
    }
    if (regular_path(card, target, 1)) goto done;
    data = malloc(COPY_SIZE); check = malloc(COPY_SIZE);
    if (!data || !check) { fail(card, "Cannot allocate card copy buffers"); goto done; }
    for (size_t offset = 0; offset < size;) {
        size_t n = size - offset < COPY_SIZE ? size - offset : COPY_SIZE;
        if (reader(context, offset, data, n)) { fail(card, "Cannot read card image at byte %zu", offset); goto done; }
        if (write_all(handle, data, n)) { fail(card, "Cannot stage card at byte %zu: %s", offset, win_error()); goto done; }
        offset += n;
    }
    if (!FlushFileBuffers(handle)) { fail(card, "Cannot sync card file: %s", win_error()); goto done; }
    for (size_t offset = 0; offset < size;) {
        size_t n = size - offset < COPY_SIZE ? size - offset : COPY_SIZE;
        if (reader(context, offset, data, n)) { fail(card, "Cannot reread card image at byte %zu", offset); goto done; }
        if (handle_read(&handle, offset, check, n)) {
            fail(card, "Cannot verify card at byte %zu: %s", offset, win_error()); goto done;
        }
        if (memcmp(data, check, n)) { fail(card, "Card verification failed at byte %zu", offset); goto done; }
        offset += n;
    }
    result = 0;
done:
    if (!CloseHandle(handle) && !result) result = fail(card, "Cannot close staged card: %s", win_error());
    free(data); free(check);
    if (result) remove_stage(stage);
    return result;
}
static int protected_unchanged(m8_card_platform *card);
static int source_unchanged(m8_card_platform *card);

static int publish(m8_card_platform *card, char **stage, const char *target)
{
    if (protected_unchanged(card) || regular_path(card, target, 1)) return -1;
    if (!u8_MoveFileEx(*stage, target, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return fail(card, "Cannot replace %s: %s", target, win_error());
    free(*stage); *stage = NULL;
    return 0;
}
static int open_source(m8_card_platform *card)
{
    card->source = u8_CreateFile(card->path, GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                               OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (card->source == INVALID_HANDLE_VALUE || handle_identity(card->source, &card->source_id)
        || (card->source_id.attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
        || !card->source_id.size || card->source_id.size > SIZE_MAX)
        return fail(card, "Cannot open nonempty regular card: %s", card->path);
    return 0;
}

/* ---- journaled in-place saves ----
 * Journal: "M8JNL1\0\0", then records {u64 offset, u32 length, bytes}, then
 * "M8JEND\0\0" and a u64 FNV-1a of every record byte. It is written and
 * flushed before the card is touched, so after a crash it is either
 * complete (replay it) or not (the card was never touched; discard it). */
static const unsigned char journal_head[8] = "M8JNL1\0", journal_tail[8] = "M8JEND\0";
static uint64_t fnv_update(uint64_t h, const void *data, size_t n)
{
    const unsigned char *p = data;
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= UINT64_C(0x100000001b3); }
    return h;
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
static int replay_journal(m8_card_platform *card)
{
    HANDLE j = u8_CreateFile(card->journal, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                             FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (j == INVALID_HANDLE_VALUE) return 0;
    LARGE_INTEGER length;
    unsigned char head[8], *buffer = NULL;
    int complete = 0;
    if (GetFileSizeEx(j, &length) && length.QuadPart >= 24 && (uint64_t)length.QuadPart <= SIZE_MAX
        && (buffer = malloc((size_t)length.QuadPart)) && !handle_read(&j, 0, buffer, (size_t)length.QuadPart)) {
        size_t n = (size_t)length.QuadPart, at = 8;
        memcpy(head, buffer, 8);
        uint64_t h = UINT64_C(0xcbf29ce484222325);
        if (!memcmp(head, journal_head, 8)) {
            while (at + 12 <= n - 16) {
                uint64_t offset; uint32_t size;
                memcpy(&offset, buffer + at, 8); memcpy(&size, buffer + at + 8, 4);
                if (at + 12 + size > n - 16) break;
                h = fnv_update(h, buffer + at, 12 + (size_t)size);
                at += 12 + (size_t)size;
            }
            uint64_t stored;
            memcpy(&stored, buffer + n - 8, 8);
            complete = at == n - 16 && !memcmp(buffer + n - 16, journal_tail, 8) && stored == h;
        }
    }
    CloseHandle(j);
    int result = 0;
    if (complete) {
        HANDLE target = u8_CreateFile(card->path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                      NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (target == INVALID_HANDLE_VALUE) result = fail(card, "Cannot finish an interrupted card save: %s", win_error());
        else {
            size_t n = (size_t)length.QuadPart;
            for (size_t at = 8; at < n - 16 && !result;) {
                uint64_t offset; uint32_t size;
                memcpy(&offset, buffer + at, 8); memcpy(&size, buffer + at + 8, 4);
                if (write_at(target, offset, buffer + at + 12, size))
                    result = fail(card, "Cannot finish an interrupted card save: %s", win_error());
                at += 12 + (size_t)size;
            }
            if (!result && !FlushFileBuffers(target)) result = fail(card, "Cannot sync the card: %s", win_error());
            CloseHandle(target);
        }
    }
    free(buffer);
    if (!result) u8_DeleteFile(card->journal);
    return result;
}
/* Coalesced runs of set pages, clipped to size. */
static int next_run(const unsigned char *dirty, size_t pages, size_t *page, size_t *first, size_t *count)
{
    while (*page < pages && !(dirty[*page >> 3] & (1u << (*page & 7)))) ++*page;
    if (*page >= pages) return 0;
    *first = *page;
    while (*page < pages && (dirty[*page >> 3] & (1u << (*page & 7))) && *page - *first < 4096) ++*page;
    *count = *page - *first;
    return 1;
}
int m8_card_platform_save_pages(m8_card_platform *card, size_t size, m8_card_platform_reader reader,
                                void *context, const unsigned char *dirty, int all, size_t page_bytes)
{
    if (!card || !reader) return -1;
    if (all || !dirty || !page_bytes || size != m8_card_platform_size(card))
        return m8_card_platform_save(card, size, reader, context);
    if (card->read_only) return fail(card, "Card is read-only: %s", card->path);
    size_t pages = (size + page_bytes - 1) / page_bytes, page = 0, first, count;
    if (!next_run(dirty, pages, &page, &first, &count)) return 0;     /* Nothing changed. */
    if (source_unchanged(card) || protected_unchanged(card)) return -1;
    size_t buffer_bytes = 4096 * page_bytes;
    unsigned char *buffer = malloc(buffer_bytes);
    if (!buffer) return fail(card, "Cannot allocate card save buffer");
    int result = 0;
    HANDLE j = u8_CreateFile(card->journal, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (j == INVALID_HANDLE_VALUE) { free(buffer); return fail(card, "Cannot create card journal: %s", win_error()); }
    uint64_t h = UINT64_C(0xcbf29ce484222325), at = 0;
    if (write_at(j, at, journal_head, 8)) result = -1;
    at += 8;
    for (page = 0; !result && next_run(dirty, pages, &page, &first, &count);) {
        uint64_t offset = (uint64_t)first * page_bytes;
        uint32_t n = (uint32_t)(offset + count * page_bytes > size ? size - offset : count * page_bytes);
        unsigned char header[12];
        memcpy(header, &offset, 8); memcpy(header + 8, &n, 4);
        if (reader(context, (size_t)offset, buffer, n) || write_at(j, at, header, 12) || write_at(j, at + 12, buffer, n)) {
            result = -1; break;
        }
        h = fnv_update(fnv_update(h, header, 12), buffer, n);
        at += 12 + (uint64_t)n;
    }
    if (!result && (write_at(j, at, journal_tail, 8) || write_at(j, at + 8, &h, 8) || !FlushFileBuffers(j))) result = -1;
    CloseHandle(j);
    if (result) { u8_DeleteFile(card->journal); free(buffer); return fail(card, "Cannot write card journal: %s", win_error()); }
    /* The journal is durable: apply it to the card. */
    HANDLE target = u8_CreateFile(card->path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (target == INVALID_HANDLE_VALUE) { free(buffer); return fail(card, "Cannot open the card for saving: %s", win_error()); }
    for (page = 0; !result && next_run(dirty, pages, &page, &first, &count);) {
        uint64_t offset = (uint64_t)first * page_bytes;
        size_t n = offset + count * page_bytes > size ? (size_t)(size - offset) : count * page_bytes;
        if (reader(context, (size_t)offset, buffer, n) || write_at(target, offset, buffer, n)) result = -1;
    }
    if (!result && !FlushFileBuffers(target)) result = -1;
    CloseHandle(target);
    free(buffer);
    if (result) return fail(card, "Card save failed; it will be finished on the next start: %s", win_error());
    u8_DeleteFile(card->journal);
    handle_identity(card->source, &card->source_id);   /* Our own write changed the time. */
    return 0;
}
int m8_card_platform_replace(m8_card_platform *card, const char *path)
{
    if (!card || !path) return -1;
    if (card->read_only) return fail(card, "Card is read-only: %s", card->path);
    if (source_unchanged(card) || protected_unchanged(card)) return -1;
    CloseHandle(card->source); card->source = INVALID_HANDLE_VALUE;
    int result = 0;
    if (!u8_MoveFileEx(card->path, card->backup, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        result = fail(card, "Cannot keep the previous card as %s: %s", card->backup, win_error());
    else if (!u8_MoveFileEx(path, card->path, MOVEFILE_WRITE_THROUGH)) {
        result = fail(card, "Cannot put the new card in place: %s", win_error());
        u8_MoveFileEx(card->backup, card->path, MOVEFILE_WRITE_THROUGH);
    }
    int reopened = open_source(card);
    return result ? result : reopened;
}

m8_card_platform *m8_card_platform_open(const char *path, int read_only, int create, size_t initial_size,
    m8_card_platform_reader initial_reader, void *initial_context,
    char *error, size_t error_capacity)
{
    m8_card_platform *card = calloc(1, sizeof(*card));
    if (error && error_capacity) *error = 0;
    if (!card) {
        if (error && error_capacity) snprintf(error, error_capacity, "Cannot allocate card state");
        return NULL;
    }
    card->lock_handle = card->source = INVALID_HANDLE_VALUE;
    card->read_only = read_only != 0;
    card->path = absolute_path(path);
    if (!card->path) { fail(card, "Cannot resolve card path: %s", win_error()); goto failed; }
    card->backup = append(card->path, ".bak");
    card->directory = append(card->path, "");
    if (!card->backup || !card->directory) { fail(card, "Cannot allocate card paths"); goto failed; }
    char *slash = strrchr(card->directory, '/');
    if (!slash || !slash[1]) { fail(card, "Card path must name a file"); goto failed; }
    if ((size_t)(slash - card->directory) + 1 <= root_length(card->directory)) slash[1] = 0; else *slash = 0;
    if (regular_path(card, card->path, create) || regular_path(card, card->backup, 1)
        || make_directories(card)) goto failed;
    card->lock = append(card->path, ".lock");
    card->journal = append(card->path, ".journal");
    if (!card->lock || !card->journal) { fail(card, "Cannot allocate card lock path"); goto failed; }
    if (regular_path(card, card->lock, 1)) goto failed;
    /* No sharing: a second process cannot open the lock at all. */
    card->lock_handle = u8_CreateFile(card->lock, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (card->lock_handle == INVALID_HANDLE_VALUE) {
        if (GetLastError() == ERROR_SHARING_VIOLATION) fail(card, "Card is in use: %s", card->path);
        else fail(card, "Cannot open regular card lock: %s", win_error());
        goto failed;
    }
    /* Recheck after lock acquisition; another owner may have replaced it. */
    if (regular_path(card, card->path, create) || regular_path(card, card->backup, 1)) goto failed;
    DWORD attributes;
    if (query_attributes(card->path, &attributes)) {
        if (!missing_error() || !create) { fail(card, "Card does not exist: %s", card->path); goto failed; }
        if (card->read_only) { fail(card, "Cannot create a read-only card: %s", card->path); goto failed; }
        if (!initial_reader || !initial_size) { fail(card, "Cannot create card without an initial image"); goto failed; }
        char *stage = NULL;
        int result = stage_image(card, card->path, initial_size, initial_reader, initial_context, &stage);
        if (!result) result = publish(card, &stage, card->path);
        remove_stage(&stage);
        if (result) goto failed;
    }
    if (replay_journal(card)) goto failed;
    if (open_source(card)) goto failed;
    if (card->source_id.attributes & FILE_ATTRIBUTE_READONLY) card->read_only = 1;
    else {
        HANDLE probe = u8_CreateFile(card->path, GENERIC_WRITE,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (probe == INVALID_HANDLE_VALUE) card->read_only = 1; else CloseHandle(probe);
    }
    return card;
failed:
    if (error && error_capacity) snprintf(error, error_capacity, "%s", card->error);
    m8_card_platform_close(card); return NULL;
}
const char *m8_card_platform_path(const m8_card_platform *card) { return card ? card->path : NULL; }
int m8_card_platform_read_only(const m8_card_platform *card) { return !card || card->read_only; }
const char *m8_card_platform_error(const m8_card_platform *card) { return card ? card->error : "Card is absent"; }

static int source_unchanged(m8_card_platform *card)
{
    identity at_path, at_handle;
    if (path_identity(card->path, &at_path) || handle_identity(card->source, &at_handle))
        return fail(card, "Cannot inspect source card before save: %s", win_error());
    const identity *original = &card->source_id;
    if ((at_path.attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
        || !same_file(&at_path, original) || at_handle.size != original->size
        || CompareFileTime(&at_handle.write_time, &original->write_time) != 0
        || (at_handle.attributes & FILE_ATTRIBUTE_READONLY))
        return fail(card, "Source card changed during the session; save refused: %s", card->path);
    return 0;
}
size_t m8_card_platform_size(const m8_card_platform *card)
{
    return card ? (size_t)card->source_id.size : 0;
}
static int inspect_aliases(m8_card_platform *card, const identity *protected, const char *protected_path)
{
    const char *paths[] = {card->path, card->backup, card->lock};
    const char *roles[] = {"source", "backup", "lock"};
    for (unsigned i = 0; i < sizeof paths / sizeof paths[0]; ++i) {
        identity id;
        if (path_identity(paths[i], &id)) {
            if (i == 1 && missing_error()) continue;
            return fail(card, "Cannot inspect card %s %s: %s", roles[i], paths[i], win_error());
        }
        if (same_file(&id, protected))
            return fail(card, "Card %s aliases protected file %s: %s", roles[i], protected_path, paths[i]);
    }
    const HANDLE handles[] = {card->source, card->lock_handle};
    const char *handle_roles[] = {"opened source", "opened lock"};
    for (unsigned i = 0; i < sizeof handles / sizeof handles[0]; ++i) {
        identity id;
        if (handle_identity(handles[i], &id))
            return fail(card, "Cannot inspect card %s: %s", handle_roles[i], win_error());
        if (same_file(&id, protected))
            return fail(card, "Card %s aliases protected file: %s", handle_roles[i], protected_path);
    }
    return 0;
}
int m8_card_platform_protect_file(m8_card_platform *card, const char *path)
{
    if (!card || !path || !*path) return card ? fail(card, "Protected file path is empty") : -1;
    struct protected_file *file = calloc(1, sizeof *file);
    if (!file) return fail(card, "Cannot allocate protected file state");
    file->path = absolute_path(path);
    if (!file->path) { free(file); return fail(card, "Cannot resolve protected file path: %s", win_error()); }
    int result = 0;
    if (path_identity(file->path, &file->id))
        result = fail(card, "Cannot inspect protected file %s: %s", file->path, win_error());
    else if (file->id.attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_DEVICE))
        result = fail(card, "Protected input must be a regular file: %s", file->path);
    else result = inspect_aliases(card, &file->id, file->path);
    if (result) { free(file->path); free(file); return -1; }
    file->next = card->protected_files;
    card->protected_files = file;
    return 0;
}
static int protected_unchanged(m8_card_platform *card)
{
    for (const struct protected_file *file = card->protected_files; file; file = file->next) {
        identity current;
        if (path_identity(file->path, &current))
            return fail(card, "Cannot inspect protected file before save %s: %s", file->path, win_error());
        if (!same_file(&current, &file->id))
            return fail(card, "Protected file changed during the session; save refused: %s", file->path);
        if (inspect_aliases(card, &file->id, file->path)) return -1;
    }
    return 0;
}
int m8_card_platform_save(m8_card_platform *card, size_t size,
                         m8_card_platform_reader reader, void *context)
{
    if (!card || !reader) return -1;
    if (card->read_only) return fail(card, "Card is read-only: %s", card->path);
    if (source_unchanged(card) || protected_unchanged(card) || regular_path(card, card->backup, 1)) return -1;
    if (size != m8_card_platform_size(card))
        return fail(card, "Card size changed: source=%zu, session=%zu", m8_card_platform_size(card), size);
    char *current_stage = NULL, *backup_stage = NULL;
    int result = stage_image(card, card->path, size, reader, context, &current_stage);
    if (!result) result = stage_image(card, card->backup, size, handle_read, &card->source, &backup_stage);
    if (!result) result = source_unchanged(card);
    if (!result) result = publish(card, &backup_stage, card->backup);
    if (!result) {
        /* Windows cannot replace a name another handle still holds under
         * legacy rename semantics; release the source, publish, reopen. */
        result = protected_unchanged(card) || regular_path(card, card->path, 1) ? -1 : 0;
        if (!result) {
            CloseHandle(card->source); card->source = INVALID_HANDLE_VALUE;
            if (!u8_MoveFileEx(current_stage, card->path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                result = fail(card, "Cannot replace %s: %s", card->path, win_error());
            else { free(current_stage); current_stage = NULL; }
            int reopened = open_source(card);
            if (!result) result = reopened;
        }
    }
    remove_stage(&current_stage); remove_stage(&backup_stage);
    return result;
}
void m8_card_platform_close(m8_card_platform *card)
{
    if (!card) return;
    if (card->source != INVALID_HANDLE_VALUE) CloseHandle(card->source);
    if (card->lock_handle != INVALID_HANDLE_VALUE) CloseHandle(card->lock_handle);
    while (card->protected_files) {
        struct protected_file *file = card->protected_files;
        card->protected_files = file->next;
        free(file->path); free(file);
    }
    free(card->path); free(card->backup); free(card->directory); free(card->lock); free(card->journal); free(card);
}
void m8_card_platform_refresh(m8_card_platform *card)
{
    if (card && card->source != INVALID_HANDLE_VALUE) handle_identity(card->source, &card->source_id);
}
