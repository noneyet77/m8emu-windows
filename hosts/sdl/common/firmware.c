#define _POSIX_C_SOURCE 200809L
#include "firmware.h"
#include "user_paths.h"
#include "sha256.h"
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define FLASH_BASE 0x60000000ll
#define FLASH_SIZE (8ll * 1024 * 1024)
/* The official HEX is about 2.8 times its 2 MiB image; anything near this
 * limit is not a firmware file. */
#define SOURCE_LIMIT ((size_t)64 * 1024 * 1024)

static void fail(char *error, size_t capacity, const char *format, ...)
{
    if (!error || !capacity) return;
    va_list args;
    va_start(args, format);
    vsnprintf(error, capacity, format, args);
    va_end(args);
}

static void digest(const void *data, size_t size, char out[65])
{
    sha256 s;
    sha_init(&s); sha_update(&s, data, size); sha_finish(&s, out);
}

static unsigned char *read_file(const char *path, size_t *size, char *error, size_t capacity)
{
    unsigned char *data = NULL;
    int fd = open(path, O_RDONLY);
    struct stat st;
    if (fd < 0 || fstat(fd, &st)) { fail(error, capacity, "Cannot open %s: %s", path, strerror(errno)); goto failed; }
    if (!S_ISREG(st.st_mode)) { fail(error, capacity, "Not a regular file: %s", path); goto failed; }
    if ((uintmax_t)st.st_size > SOURCE_LIMIT) { fail(error, capacity, "File is too large: %s", path); goto failed; }
    *size = (size_t)st.st_size;
    data = malloc(*size ? *size : 1);
    if (!data) { fail(error, capacity, "Cannot allocate %zu bytes", *size); goto failed; }
    for (size_t done = 0; done < *size;) {
        ssize_t got = read(fd, data + done, *size - done);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) { fail(error, capacity, "Cannot read %s: %s", path, got ? strerror(errno) : "short file"); goto failed; }
        done += (size_t)got;
    }
    close(fd);
    return data;
failed:
    if (fd >= 0) close(fd);
    free(data);
    return NULL;
}

int m8_firmware_file_sha256(const char *path, char out[65], char *error, size_t capacity)
{
    size_t size = 0;
    unsigned char *data = read_file(path, &size, error, capacity);
    if (!data) return -1;
    digest(data, size, out);
    free(data);
    return 0;
}

int m8_firmware_verified(const char *path)
{
    char sha[65];
    return path && !m8_firmware_file_sha256(path, sha, NULL, 0) && !strcmp(sha, M8_FIRMWARE_BIN_SHA256);
}

/* Python's str.splitlines() boundaries and str.strip() whitespace, for ASCII. */
static int line_break(unsigned char c) { return c == '\n' || c == '\r' || c == '\v' || c == '\f' || (c >= 0x1c && c <= 0x1e); }
static int blank(unsigned char c) { return c == ' ' || c == '\t' || line_break(c) || c == 0x1f; }
/* bytes.fromhex() also skips ASCII whitespace between digit pairs. */
static int hex_space(unsigned char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f'; }
static int nibble(unsigned char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

unsigned char *m8_firmware_hex_to_binary(const unsigned char *source, size_t size,
                                         size_t *out_size, char *error, size_t capacity)
{
    unsigned char *image = malloc((size_t)FLASH_SIZE), record[300];
    if (!image) { fail(error, capacity, "Cannot allocate the flash image"); return NULL; }
    memset(image, 0xff, (size_t)FLASH_SIZE);
    long long base = 0, end = 0;
    int eof = 0;
    unsigned lineno = 0;
    for (size_t i = 0; i < size; ++i) if (source[i] > 0x7f) { fail(error, capacity, "byte %zu: not ASCII", i); goto failed; }
    for (size_t at = 0; at < size;) {
        size_t start = at, stop;
        while (at < size && !line_break(source[at])) ++at;
        stop = at;
        if (at < size) at += source[at] == '\r' && at + 1 < size && source[at + 1] == '\n' ? 2 : 1;
        ++lineno;
        while (start < stop && blank(source[start])) ++start;
        while (stop > start && blank(source[stop - 1])) --stop;
        if (start == stop) continue;
        if (eof) { fail(error, capacity, "line %u: record after EOF", lineno); goto failed; }
        if (source[start] != ':') { fail(error, capacity, "line %u: missing record marker", lineno); goto failed; }
        size_t n = 0;
        for (size_t p = start + 1; p < stop;) {
            if (hex_space(source[p])) { ++p; continue; }
            int high = nibble(source[p]), low = p + 1 < stop ? nibble(source[p + 1]) : -1;
            if (high < 0 || low < 0) { fail(error, capacity, "line %u: invalid hexadecimal data", lineno); goto failed; }
            if (n == sizeof record) { fail(error, capacity, "line %u: invalid record length", lineno); goto failed; }
            record[n++] = (unsigned char)(high << 4 | low);
            p += 2;
        }
        if (n < 5 || n != (size_t)record[0] + 5) { fail(error, capacity, "line %u: invalid record length", lineno); goto failed; }
        unsigned sum = 0;
        for (size_t k = 0; k < n; ++k) sum += record[k];
        if (sum & 0xff) { fail(error, capacity, "line %u: invalid record checksum", lineno); goto failed; }
        unsigned count = record[0], kind = record[3], address = (unsigned)record[1] << 8 | record[2];
        const unsigned char *data = record + 4;
        if (kind == 0) {
            long long offset = base + address - FLASH_BASE;
            if (offset < 0 || offset + count > FLASH_SIZE) { fail(error, capacity, "line %u: data outside flash", lineno); goto failed; }
            memcpy(image + offset, data, count);
            if (offset + count > end) end = offset + count;
        } else if (kind == 1 && count == 0 && address == 0) eof = 1;
        else if (kind == 2 && count == 2 && address == 0) base = (long long)((unsigned)data[0] << 8 | data[1]) << 4;
        else if (kind == 4 && count == 2 && address == 0) base = (long long)((unsigned)data[0] << 8 | data[1]) << 16;
        else if ((kind == 3 || kind == 5) && count == 4 && address == 0) {
            /* Entry-address records do not contain flash bytes. */
        } else { fail(error, capacity, "line %u: unsupported or malformed record type %u", lineno, kind); goto failed; }
    }
    if (!eof) { fail(error, capacity, "missing EOF record"); goto failed; }
    if (!end) { fail(error, capacity, "no flash data"); goto failed; }
    *out_size = (size_t)end;
    return image;
failed:
    free(image);
    return NULL;
}

static int write_all(int fd, const unsigned char *data, size_t n)
{
    while (n) {
        ssize_t wrote = write(fd, data, n);
        if (wrote < 0 && errno == EINTR) continue;
        if (wrote <= 0) { if (!wrote) errno = EIO; return -1; }
        data += wrote; n -= (size_t)wrote;
    }
    return 0;
}

/* Temporary file in the destination directory, synced, reread, renamed. */
static int publish(const char *destination, const unsigned char *data, size_t size, char *error, size_t capacity)
{
    if (m8_user_make_parents(destination)) {
        fail(error, capacity, "Cannot create the folder for %s: %s", destination, strerror(errno));
        return -1;
    }
    size_t length = strlen(destination);
    char *stage = malloc(length + 12), sha[65];
    if (!stage) { fail(error, capacity, "Cannot allocate a temporary path"); return -1; }
    memcpy(stage, destination, length);
    memcpy(stage + length, ".tmp.XXXXXX", 12);
    int fd = mkstemp(stage), result = -1;
    if (fd < 0) { fail(error, capacity, "Cannot create %s: %s", stage, strerror(errno)); free(stage); return -1; }
    if (fchmod(fd, 0644) || write_all(fd, data, size) || fsync(fd)) {
        fail(error, capacity, "Cannot write %s: %s", stage, strerror(errno)); close(fd); goto done;
    }
    if (close(fd)) { fail(error, capacity, "Cannot close %s: %s", stage, strerror(errno)); goto done; }
    if (m8_firmware_file_sha256(stage, sha, error, capacity)) goto done;
    if (strcmp(sha, M8_FIRMWARE_BIN_SHA256)) { fail(error, capacity, "Written firmware failed verification: %s", stage); goto done; }
    if (rename(stage, destination)) { fail(error, capacity, "Cannot replace %s: %s", destination, strerror(errno)); goto done; }
    char *slash = strrchr(stage, '/');
    if (slash) {
        *slash = 0;
        int dir = open(*stage ? stage : "/", O_RDONLY);
        if (dir >= 0) { fsync(dir); close(dir); }
    }
    result = 0;
done:
    if (result) unlink(stage);
    free(stage);
    return result;
}

m8_firmware_status m8_firmware_install(const char *source, const char *destination,
                                       char *error, size_t capacity)
{
    if (error && capacity) *error = 0;
    size_t size = 0, image_size = 0;
    char sha[65];
    unsigned char *data = read_file(source, &size, error, capacity), *image = NULL;
    if (!data) return M8_FIRMWARE_READ;
    m8_firmware_status status = M8_FIRMWARE_OK;
    const unsigned char *binary = data;
    digest(data, size, sha);
    size_t first = 0;
    while (first < size && blank(data[first])) ++first;
    if (strcmp(sha, M8_FIRMWARE_BIN_SHA256)) {
        if (first == size || data[first] != ':') {
            /* A Teensy 4 flash image opens with the FlexSPI configuration block. */
            status = size >= 4 && !memcmp(data, "FCFB", 4) ? M8_FIRMWARE_WRONG : M8_FIRMWARE_INVALID;
            fail(error, capacity, "%s is %s: sha256=%s", source,
                 status == M8_FIRMWARE_WRONG ? "another firmware image" : "neither Intel HEX nor the supported image", sha);
            goto done;
        }
        int pinned_hex = !strcmp(sha, M8_FIRMWARE_HEX_SHA256);
        image = m8_firmware_hex_to_binary(data, size, &image_size, error, capacity);
        if (!image) { status = M8_FIRMWARE_INVALID; goto done; }
        digest(image, image_size, sha);
        if (strcmp(sha, M8_FIRMWARE_BIN_SHA256)) {
            status = pinned_hex ? M8_FIRMWARE_INVALID : M8_FIRMWARE_WRONG;
            fail(error, capacity, "%s converts to another firmware: binary sha256=%s expected=%s",
                 source, sha, M8_FIRMWARE_BIN_SHA256);
            goto done;
        }
        binary = image; size = image_size;
    }
    if (publish(destination, binary, size, error, capacity)) status = M8_FIRMWARE_WRITE;
done:
    free(image);
    free(data);
    return status;
}
