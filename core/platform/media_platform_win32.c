/* Windows adapter for media_platform_posix.h (the interface name is kept so
 * the portable media owner is unchanged). Paths are UTF-8; links, junctions
 * and other reparse points are refused, matching the POSIX no-link rule. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "win_utf8.h"
#include "media_platform_posix.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
static char *join(const char *dir, const char *name)
{
    size_t a = strlen(dir), b = strlen(name);
    char *out = malloc(a + b + 2);
    if (!out) return NULL;
    memcpy(out, dir, a);
    size_t at = a;
    if (a && dir[a - 1] != '/' && dir[a - 1] != '\\') out[at++] = '/';
    memcpy(out + at, name, b + 1);
    return out;
}
static int read_handle(media *m, HANDLE h, node *n)
{
    LARGE_INTEGER size;
    if (!GetFileSizeEx(h, &size) || size.QuadPart < 0 || (uint64_t)size.QuadPart > UINT32_MAX)
        return m8_media_fail(m, "Input must be a regular file of at most 4 GiB: %s", n->name);
    n->size = (size_t)size.QuadPart;
    n->data = n->size ? malloc(n->size) : NULL;
    if (n->size && !n->data) return m8_media_fail(m, "Cannot allocate input file: %s", n->name);
    size_t offset = 0;
    while (offset < n->size) {
        DWORD want = n->size - offset > 0x40000000u ? 0x40000000u : (DWORD)(n->size - offset), got = 0;
        if (!ReadFile(h, n->data + offset, want, &got, NULL) || !got)
            return m8_media_fail(m, "Cannot read input file: %s", n->name);
        offset += got;
    }
    unsigned char extra; DWORD got = 0;
    if (!ReadFile(h, &extra, 1, &got, NULL) || got)
        return m8_media_fail(m, "Input file changed while reading: %s", n->name);
    return 0;
}
static int read_path(media *m, const char *path, node *n)
{
    HANDLE h = u8_CreateFile(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                           FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (h == INVALID_HANDLE_VALUE) return m8_media_fail(m, "Cannot open input without a link: %s", n->name);
    BY_HANDLE_FILE_INFORMATION info;
    int result;
    if (!GetFileInformationByHandle(h, &info)
        || (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE)))
        result = m8_media_fail(m, "Input must be a regular file of at most 4 GiB: %s", n->name);
    else result = read_handle(m, h, n);
    CloseHandle(h);
    return result;
}
static int compare_nodes(const void *a, const void *b)
{
    const node *left = *(node *const *)a, *right = *(node *const *)b;
    int order = strcmp(left->folded, right->folded);
    return order ? order : strcmp(left->name, right->name);
}
static int scan_folder(media *m, const char *directory, node *parent, unsigned depth)
{
    if (depth > MAX_DEPTH) return m8_media_fail(m, "Host directory depth exceeds %u", MAX_DEPTH);
    u8_find entry;
    if (!u8_find_first(&entry, directory)) {
        DWORD e = GetLastError();
        u8_find_close(&entry);
        if (e == ERROR_FILE_NOT_FOUND) return 0;
        return m8_media_fail(m, "Cannot read import directory: %s", win_error());
    }
    int result = 0;
    do {
        if (!strcmp(entry.name, ".") || !strcmp(entry.name, "..")) continue;
        DWORD a = entry.attributes;
        /* Explorer and OS clutter never belongs on the card. */
        if ((a & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) || !_stricmp(entry.name, "desktop.ini") ||
            !_stricmp(entry.name, "Thumbs.db")) continue;
        if (a & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE)) {
            result = m8_media_fail(m, "Links and special files cannot be imported: %s", entry.name);
            break;
        }
        int is_dir = (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
        node *child = m8_media_new_node(m, entry.name, is_dir);
        if (!child) { result = -1; break; }
        char *path = join(directory, entry.name);
        if (!path) { m8_media_free_node(child); result = m8_media_fail(m, "Cannot allocate import path"); break; }
        result = is_dir ? scan_folder(m, path, child, depth + 1) : read_path(m, path, child);
        free(path);
        if (!result) result = m8_media_add_child(m, parent, child);
        if (result) { m8_media_free_node(child); break; }
    } while (u8_find_next(&entry));
    if (!result && GetLastError() != ERROR_NO_MORE_FILES)
        result = m8_media_fail(m, "Cannot enumerate import directory: %s", win_error());
    u8_find_close(&entry);
    if (!result && parent->child_count)
        qsort(parent->children, parent->child_count, sizeof(*parent->children), compare_nodes);
    return result;
}
node *m8_media_platform_read_folder(media *m, const char *directory)
{
    node *root = m8_media_new_node(m, "", 1);
    if (!root) return NULL;
    DWORD a = u8_GetFileAttributes(directory);
    if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY) || (a & FILE_ATTRIBUTE_REPARSE_POINT)) {
        m8_media_free_node(root);
        m8_media_fail(m, "Import source must be a directory without a link: %s", directory);
        return NULL;
    }
    if (scan_folder(m, directory, root, 0)) { m8_media_free_node(root); return NULL; }
    return root;
}
int m8_media_platform_read_image(media *m, const char *path)
{
    node temporary = {0};
    temporary.name = (char *)path;
    int result = read_path(m, path, &temporary);
    if (!result && !temporary.size) { free(temporary.data); temporary.data = NULL;
        result = m8_media_fail(m, "Image must be a nonempty regular file: %s", path); }
    m->image = temporary.data; m->image_size = temporary.size;
    return result;
}
static int empty_directory(media *m, const char *path)
{
    DWORD a = u8_GetFileAttributes(path);
    if (a == INVALID_FILE_ATTRIBUTES) {
        DWORD e = GetLastError();
        if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) return 0;
        return m8_media_fail(m, "Cannot inspect export destination: %s", win_error());
    }
    if (!(a & FILE_ATTRIBUTE_DIRECTORY) || (a & FILE_ATTRIBUTE_REPARSE_POINT))
        return m8_media_fail(m, "Export folder must be new or empty, without a link: %s", path);
    u8_find entry;
    if (!u8_find_first(&entry, path)) {
        u8_find_close(&entry);
        return m8_media_fail(m, "Cannot open export directory: %s", win_error());
    }
    int result = 0;
    do {
        if (strcmp(entry.name, ".") && strcmp(entry.name, "..")) {
            result = m8_media_fail(m, "Export folder must be empty: %s", path);
            break;
        }
    } while (u8_find_next(&entry));
    u8_find_close(&entry);
    return result;
}
static int create_directory_path(media *m, const char *path)
{
    size_t copy_n = strlen(path) + 1;
    char *copy = malloc(copy_n);
    if (copy) memcpy(copy, path, copy_n);
    if (!copy) return m8_media_fail(m, "Cannot allocate export path");
    for (char *p = copy; *p; ++p) if (*p == '\\') *p = '/';
    char *p = copy;
    if (p[0] && p[1] == ':' && p[2] == '/') p += 3;              /* C:/ */
    else if (p[0] == '/' && p[1] == '/') {                        /* //server/share/ */
        char *s = strchr(p + 2, '/');
        s = s ? strchr(s + 1, '/') : NULL;
        p = s ? s + 1 : p + strlen(p);
    } else if (*p == '/') p++;
    for (;; p++) {
        if (*p && *p != '/') continue;
        char old = *p; *p = 0;
        if (*copy && !u8_CreateDirectory(copy) && GetLastError() != ERROR_ALREADY_EXISTS) {
            free(copy); return m8_media_fail(m, "Cannot create export directory: %s", win_error());
        }
        DWORD a = u8_GetFileAttributes(copy);
        if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY) || (a & FILE_ATTRIBUTE_REPARSE_POINT)) {
            free(copy); return m8_media_fail(m, "Export path must contain directories without links");
        }
        *p = old;
        if (!old || !p[1]) break;
    }
    free(copy);
    return 0;
}
static int write_file(media *m, const char *path, const node *n)
{
    HANDLE h = u8_CreateFile(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return m8_media_fail(m, "Cannot create exported entry: %s", n->name);
    size_t offset = 0;
    int result = 0;
    while (offset < n->size) {
        DWORD want = n->size - offset > 0x40000000u ? 0x40000000u : (DWORD)(n->size - offset), wrote = 0;
        if (!WriteFile(h, n->data + offset, want, &wrote, NULL) || !wrote) {
            result = m8_media_fail(m, "Cannot write exported file: %s", n->name); break;
        }
        offset += wrote;
    }
    if (!CloseHandle(h) && !result) result = m8_media_fail(m, "Cannot close exported entry: %s", n->name);
    return result;
}
static int export_tree(media *m, const char *directory, const node *root)
{
    for (size_t i = 0; i < root->child_count; i++) {
        const node *n = root->children[i];
        char *path = join(directory, n->name);
        if (!path) return m8_media_fail(m, "Cannot allocate export path");
        int result;
        if (n->directory) {
            if (!u8_CreateDirectory(path)) result = m8_media_fail(m, "Cannot create exported directory: %s", n->name);
            else result = export_tree(m, path, n);
        } else result = write_file(m, path, n);
        free(path);
        if (result) return -1;
    }
    return 0;
}
int m8_media_platform_export(media *m, const node *root, const char *directory)
{
    if (empty_directory(m, directory) || create_directory_path(m, directory)) return -1;
    return export_tree(m, directory, root);
}
