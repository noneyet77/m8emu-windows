/* See fat32.h. */
#include "fat32.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define END_OF_CHAIN UINT32_C(0x0FFFFFFF)
#define MIN_CLUSTERS 65525u
#define MAX_CLUSTERS UINT32_C(0x0FFFFFF5)
#define PART_START 8192u          /* 4 MiB alignment, as SD formatters do */
#define RESERVED 32u
#define MAX_DEPTH 64

static int fail(char *error, size_t capacity, const char *format, ...)
{
    if (error && capacity) {
        va_list args;
        va_start(args, format);
        vsnprintf(error, capacity, format, args);
        va_end(args);
    }
    return -1;
}
static void put16(unsigned char *p, uint32_t v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
static void put32(unsigned char *p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }
static uint32_t get16(const unsigned char *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t get32(const unsigned char *p) { return get16(p) | get16(p + 2) << 16; }

/* ---- tree ---- */

m8fat_node *m8fat_node_new(const char *name, int directory)
{
    m8fat_node *n = calloc(1, sizeof *n);
    if (!n) return NULL;
    size_t length = strlen(name) + 1;
    n->name = malloc(length);
    if (!n->name) { free(n); return NULL; }
    memcpy(n->name, name, length);
    n->directory = directory;
    n->date = (uint16_t)(((2026 - 1980) << 9) | (1 << 5) | 1);
    return n;
}
int m8fat_node_add(m8fat_node *parent, m8fat_node *child)
{
    if (parent->child_count == parent->child_capacity) {
        size_t capacity = parent->child_capacity ? parent->child_capacity * 2 : 8;
        m8fat_node **grown = realloc(parent->children, capacity * sizeof *grown);
        if (!grown) return -1;
        parent->children = grown; parent->child_capacity = capacity;
    }
    parent->children[parent->child_count++] = child;
    return 0;
}
void m8fat_node_free(m8fat_node *n)
{
    if (!n) return;
    for (size_t i = 0; i < n->child_count; ++i) m8fat_node_free(n->children[i]);
    free(n->children); free(n->name); free(n);
}

/* ---- UTF-8 / UTF-16 ---- */

/* UTF-8 to UTF-16 units; -1 when invalid or longer than capacity. */
static int to_utf16(const char *s, uint16_t *out, int capacity)
{
    int n = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        uint32_t c; int extra;
        if (*p < 0x80) { c = *p; extra = 0; }
        else if ((*p & 0xe0) == 0xc0) { c = *p & 0x1f; extra = 1; }
        else if ((*p & 0xf0) == 0xe0) { c = *p & 0x0f; extra = 2; }
        else if ((*p & 0xf8) == 0xf0) { c = *p & 0x07; extra = 3; }
        else return -1;
        ++p;
        for (int i = 0; i < extra; ++i, ++p) {
            if ((*p & 0xc0) != 0x80) return -1;
            c = c << 6 | (*p & 0x3f);
        }
        if (c >= 0x10000) {
            if (c > 0x10ffff || n + 2 > capacity) return -1;
            c -= 0x10000;
            out[n++] = (uint16_t)(0xd800 | c >> 10); out[n++] = (uint16_t)(0xdc00 | (c & 0x3ff));
        } else {
            if ((c >= 0xd800 && c < 0xe000) || n + 1 > capacity) return -1;
            out[n++] = (uint16_t)c;
        }
    }
    return n;
}
static char *from_utf16(const uint16_t *units, int count)
{
    char *out = malloc((size_t)count * 3 + 1), *p = out;
    if (!out) return NULL;
    for (int i = 0; i < count; ++i) {
        uint32_t c = units[i];
        if (c >= 0xd800 && c < 0xdc00 && i + 1 < count && units[i + 1] >= 0xdc00 && units[i + 1] < 0xe000)
            c = 0x10000 + ((c - 0xd800) << 10) + (units[++i] - 0xdc00);
        if (c < 0x80) *p++ = (char)c;
        else if (c < 0x800) { *p++ = (char)(0xc0 | c >> 6); *p++ = (char)(0x80 | (c & 0x3f)); }
        else if (c < 0x10000) {
            *p++ = (char)(0xe0 | c >> 12); *p++ = (char)(0x80 | (c >> 6 & 0x3f)); *p++ = (char)(0x80 | (c & 0x3f));
        } else {
            *p++ = (char)(0xf0 | c >> 18); *p++ = (char)(0x80 | (c >> 12 & 0x3f));
            *p++ = (char)(0x80 | (c >> 6 & 0x3f)); *p++ = (char)(0x80 | (c & 0x3f));
        }
    }
    *p = 0;
    return out;
}

/* ---- layout ---- */

typedef struct {
    uint32_t part_start, part_sectors, fat_sectors, spc, clusters, cluster_bytes;
    uint64_t fat_offset, data_offset;
} layout;

/* 32 KiB clusters (as SD cards of this size are formatted) and FATs sized
 * for max_bytes, so the card can later grow up to max_bytes, or shrink, by
 * changing only its size fields. */
#define SPC 64u
static uint32_t fat_sectors_for(uint64_t max_bytes)
{
    uint64_t clusters = (max_bytes / 512 - PART_START - RESERVED) / SPC;
    return (uint32_t)(((clusters + 2) * 4 + 511) / 512);
}
static int plan_with(uint64_t bytes, uint32_t fat, layout *l)
{
    if (bytes % 512 || bytes / 512 > UINT32_MAX || bytes / 512 <= (uint64_t)PART_START + RESERVED + 2u * fat)
        return -1;
    memset(l, 0, sizeof *l);
    l->part_start = PART_START;
    l->part_sectors = (uint32_t)(bytes / 512 - PART_START);
    uint64_t clusters = ((uint64_t)l->part_sectors - RESERVED - 2u * fat) / SPC;
    if (clusters < MIN_CLUSTERS || clusters > MAX_CLUSTERS || (clusters + 2) * 4 > (uint64_t)fat * 512) return -1;
    l->spc = SPC; l->fat_sectors = fat; l->clusters = (uint32_t)clusters; l->cluster_bytes = SPC * 512;
    l->fat_offset = ((uint64_t)l->part_start + RESERVED) * 512;
    l->data_offset = l->fat_offset + (uint64_t)2 * fat * 512;
    return 0;
}
static int plan(uint64_t bytes, uint64_t max_bytes, layout *l)
{
    return plan_with(bytes, fat_sectors_for(max_bytes > bytes ? max_bytes : bytes), l);
}
static uint64_t cluster_offset(const layout *l, uint32_t cluster)
{
    return l->data_offset + (uint64_t)(cluster - 2) * l->cluster_bytes;
}

/* ---- names ---- */

static int valid_name(const char *name, uint16_t *units, int *count)
{
    if (!*name || !strcmp(name, ".") || !strcmp(name, "..")) return 0;
    *count = to_utf16(name, units, 255);
    if (*count <= 0) return 0;
    for (int i = 0; i < *count; ++i)
        if (units[i] < 0x20 || (units[i] < 0x80 && strchr("\\/:*?\"<>|", units[i]))) return 0;
    return units[*count - 1] != ' ' && units[*count - 1] != '.';
}
static int entries_for(const m8fat_node *n)
{
    uint16_t units[256]; int count;
    if (!valid_name(n->name, units, &count)) return -1;
    return (count + 12) / 13 + 1;
}
static int dir_clusters(const m8fat_node *dir, int root, uint32_t cluster_bytes, uint32_t *out)
{
    uint64_t entries = root ? 1 : 2;
    for (size_t i = 0; i < dir->child_count; ++i) {
        int e = entries_for(dir->children[i]);
        if (e < 0) return -1;
        entries += (uint64_t)e;
    }
    uint64_t clusters = (entries * 32 + cluster_bytes - 1) / cluster_bytes;
    if (clusters > 65536u * 32u / cluster_bytes + 1) return -2; /* FAT caps a directory at 65536 entries */
    *out = clusters ? (uint32_t)clusters : 1;
    return 0;
}
/* Assign clusters depth first: a directory, then its files, then subdirectories. */
static int allocate(m8fat_node *dir, int root, const layout *l, uint32_t *next, char *error, size_t capacity)
{
    uint32_t count;
    int r = dir_clusters(dir, root, l->cluster_bytes, &count);
    if (r == -1) {
        for (size_t i = 0; i < dir->child_count; ++i)
            if (entries_for(dir->children[i]) < 0)
                return fail(error, capacity, "Name not allowed on the card: %s", dir->children[i]->name);
    }
    if (r) return fail(error, capacity, "Too many entries in one folder: %s", *dir->name ? dir->name : "(root)");
    dir->first = *next; dir->clusters = count; *next += count;
    for (size_t i = 0; i < dir->child_count; ++i) {
        m8fat_node *n = dir->children[i];
        if (n->directory) continue;
        if (n->size > UINT32_MAX) return fail(error, capacity, "File larger than 4 GiB: %s", n->name);
        n->clusters = (uint32_t)((n->size + l->cluster_bytes - 1) / l->cluster_bytes);
        n->first = n->clusters ? *next : 0;
        *next += n->clusters;
        if (*next - 2 > l->clusters) return fail(error, capacity, "The card is full");
    }
    for (size_t i = 0; i < dir->child_count; ++i)
        if (dir->children[i]->directory && allocate(dir->children[i], 0, l, next, error, capacity)) return -1;
    if (*next - 2 > l->clusters) return fail(error, capacity, "The card is full");
    return 0;
}
uint64_t m8fat_required(const m8fat_node *root, uint64_t max_bytes)
{
    layout l;
    if (plan(max_bytes, max_bytes, &l)) return UINT64_MAX;
    uint32_t next = 2;
    /* allocate() records clusters in the nodes; that is harmless before a build. */
    if (allocate((m8fat_node *)root, 1, &l, &next, NULL, 0)) return UINT64_MAX;
    return (uint64_t)(next - 2) * l.cluster_bytes;
}

static unsigned char short_checksum(const unsigned char name[11])
{
    unsigned char sum = 0;
    for (int i = 0; i < 11; ++i) sum = (unsigned char)(((sum & 1) << 7) + (sum >> 1) + name[i]);
    return sum;
}
static int short_char(unsigned c)
{
    if (c >= 'a' && c <= 'z') return (int)(c - 32);
    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || (c && c < 0x80 && strchr("!#$%&'()-@^_`{}~", (int)c)))
        return (int)c;
    return 0;
}
/* Unique 8.3 alias "BASE~N  EXT" for a long name within used[0..count). */
static void short_alias(const char *name, unsigned char (*used)[11], size_t count, unsigned char out[11])
{
    const char *dot = strrchr(name, '.');
    if (dot == name) dot = NULL;
    char base[9] = {0}, ext[4] = {0};
    size_t b = 0, e = 0;
    for (const char *p = name; *p && (!dot || p < dot) && b < 8; ++p) {
        unsigned char u = (unsigned char)*p;
        if (u >= 0x80 && (u & 0xc0) == 0x80) continue;   /* UTF-8 continuation */
        int c = u >= 0x80 ? '_' : short_char(u);
        if (c) base[b++] = (char)c;
    }
    if (dot) for (const char *p = dot + 1; *p && e < 3; ++p) {
        unsigned char u = (unsigned char)*p;
        int c = u < 0x80 ? short_char(u) : 0;
        if (c) ext[e++] = (char)c;
    }
    if (!b) base[b++] = '_';
    for (unsigned long tail = 1;; ++tail) {
        char suffix[12];
        int s = snprintf(suffix, sizeof suffix, "~%lu", tail);
        size_t keep = b + (size_t)s > 8 ? 8 - (size_t)s : b;
        memset(out, ' ', 11);
        memcpy(out, base, keep); memcpy(out + keep, suffix, (size_t)s); memcpy(out + 8, ext, e);
        size_t i = 0;
        while (i < count && memcmp(used[i], out, 11)) ++i;
        if (i == count) return;
    }
}
static unsigned char *put_entries(unsigned char *p, const m8fat_node *n, unsigned char (*used)[11], size_t *used_count)
{
    uint16_t units[256]; int count;
    valid_name(n->name, units, &count);
    unsigned char alias[11];
    short_alias(n->name, used, *used_count, alias);
    memcpy(used[(*used_count)++], alias, 11);
    unsigned char sum = short_checksum(alias);
    int parts = (count + 12) / 13;
    static const int offsets[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
    for (int part = parts; part >= 1; --part) {
        memset(p, 0, 32);
        p[0] = (unsigned char)(part | (part == parts ? 0x40 : 0));
        p[11] = 0x0f; p[13] = sum;
        for (int k = 0; k < 13; ++k) {
            int i = (part - 1) * 13 + k;
            uint32_t unit = i < count ? units[i] : i == count ? 0 : 0xffff;
            put16(p + offsets[k], unit);
        }
        p += 32;
    }
    memset(p, 0, 32);
    memcpy(p, alias, 11);
    p[11] = n->directory ? 0x10 : 0x20;
    put16(p + 14, n->time); put16(p + 16, n->date); put16(p + 18, n->date);
    put16(p + 20, n->first >> 16); put16(p + 22, n->time); put16(p + 24, n->date);
    put16(p + 26, n->first & 0xffff); put32(p + 28, n->directory ? 0 : (uint32_t)n->size);
    return p + 32;
}
static void dot_entry(unsigned char *p, const char *name, uint32_t cluster, const m8fat_node *n)
{
    memset(p, 0, 32); memset(p, ' ', 11); memcpy(p, name, strlen(name));
    p[11] = 0x10;
    put16(p + 14, n->time); put16(p + 16, n->date); put16(p + 18, n->date);
    put16(p + 20, cluster >> 16); put16(p + 22, n->time); put16(p + 24, n->date); put16(p + 26, cluster & 0xffff);
}

/* ---- builder ---- */

static uint64_t fnv(uint64_t h, const unsigned char *p, size_t n)
{
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= UINT64_C(0x100000001b3); }
    return h;
}
static void chain(uint32_t *fat, uint32_t first, uint32_t count)
{
    for (uint32_t i = 0; i < count; ++i) fat[first + i] = i + 1 < count ? first + i + 1 : END_OF_CHAIN;
}
static void chains(uint32_t *fat, const m8fat_node *dir)
{
    chain(fat, dir->first, dir->clusters);
    for (size_t i = 0; i < dir->child_count; ++i) {
        const m8fat_node *n = dir->children[i];
        if (n->directory) chains(fat, n);
        else if (n->clusters) chain(fat, n->first, n->clusters);
    }
}
static int write_dirs(const m8fat_node *dir, const m8fat_node *parent, const layout *l, const m8fat_sink *sink,
                      char *error, size_t capacity)
{
    size_t bytes = (size_t)dir->clusters * l->cluster_bytes;
    unsigned char *buffer = calloc(1, bytes);
    unsigned char (*used)[11] = calloc(dir->child_count + 1, 11);
    size_t used_count = 0;
    if (!buffer || !used) { free(buffer); free(used); return fail(error, capacity, "Out of memory"); }
    unsigned char *p = buffer;
    if (!parent) {
        memset(p, ' ', 11); memcpy(p, "M8", 2); p[11] = 0x08;
        put16(p + 22, dir->time); put16(p + 24, dir->date);
        p += 32;
    } else {
        dot_entry(p, ".", dir->first, dir); p += 32;
        dot_entry(p, "..", parent->name[0] ? parent->first : 0, dir); p += 32;
    }
    for (size_t i = 0; i < dir->child_count; ++i) p = put_entries(p, dir->children[i], used, &used_count);
    int result = sink->write(sink->context, cluster_offset(l, dir->first), buffer, bytes);
    free(buffer); free(used);
    if (result) return fail(error, capacity, "Cannot write a folder to the card image");
    for (size_t i = 0; i < dir->child_count; ++i)
        if (dir->children[i]->directory && write_dirs(dir->children[i], dir, l, sink, error, capacity)) return -1;
    return 0;
}
static int write_files(m8fat_node *dir, const layout *l, const m8fat_sink *sink, unsigned char *buffer,
                       size_t buffer_bytes, char *error, size_t capacity)
{
    for (size_t i = 0; i < dir->child_count; ++i) {
        m8fat_node *n = dir->children[i];
        if (n->directory) {
            if (write_files(n, l, sink, buffer, buffer_bytes, error, capacity)) return -1;
            continue;
        }
        uint64_t h = UINT64_C(0xcbf29ce484222325), base = n->first ? cluster_offset(l, n->first) : 0;
        for (uint64_t offset = 0; offset < n->size;) {
            size_t chunk = n->size - offset < buffer_bytes ? (size_t)(n->size - offset) : buffer_bytes;
            if (sink->read_file(sink->context, n, offset, buffer, chunk))
                return fail(error, capacity, "Cannot read %s", n->name);
            h = fnv(h, buffer, chunk);
            if (sink->write(sink->context, base + offset, buffer, chunk))
                return fail(error, capacity, "Cannot write %s to the card image", n->name);
            offset += chunk;
        }
        n->hash = h;
    }
    return 0;
}
int m8fat_build(m8fat_node *root, uint64_t image_bytes, uint64_t max_bytes, const m8fat_sink *sink,
                char *error, size_t capacity)
{
    layout l;
    if (plan(image_bytes, max_bytes, &l)) return fail(error, capacity, "Card size %llu is not usable for FAT32",
                                                      (unsigned long long)image_bytes);
    uint32_t next = 2;
    if (allocate(root, 1, &l, &next, error, capacity)) return -1;
    uint32_t used = next - 2;

    unsigned char sector[512];
    /* MBR: one FAT32 (LBA) partition. */
    memset(sector, 0, sizeof sector);
    unsigned char *entry = sector + 446;
    entry[1] = 0xfe; entry[2] = 0xff; entry[3] = 0xff; entry[4] = 0x0c; entry[5] = 0xfe; entry[6] = 0xff; entry[7] = 0xff;
    put32(entry + 8, l.part_start); put32(entry + 12, l.part_sectors);
    put16(sector + 510, 0xaa55);
    if (sink->write(sink->context, 0, sector, 512)) return fail(error, capacity, "Cannot write the card image");
    /* Boot sector. */
    unsigned char boot[512];
    memset(boot, 0, sizeof boot);
    memcpy(boot, "\xeb\x58\x90" "MSWIN4.1", 11);
    put16(boot + 11, 512); boot[13] = (unsigned char)l.spc; put16(boot + 14, RESERVED); boot[16] = 2;
    boot[21] = 0xf8; put16(boot + 24, 63); put16(boot + 26, 255);
    put32(boot + 28, l.part_start); put32(boot + 32, l.part_sectors); put32(boot + 36, l.fat_sectors);
    put32(boot + 44, 2); put16(boot + 48, 1); put16(boot + 50, 6);
    boot[64] = 0x80; boot[66] = 0x29; put32(boot + 67, UINT32_C(0x4d384341));
    memcpy(boot + 71, "M8         FAT32   ", 19);
    put16(boot + 510, 0xaa55);
    /* FSInfo. */
    unsigned char info[512];
    memset(info, 0, sizeof info);
    put32(info, UINT32_C(0x41615252)); put32(info + 484, UINT32_C(0x61417272));
    /* Free-space hints "unknown": the M8 firmware does not maintain them, and
     * a stale hint is worse than none. */
    (void)used;
    put32(info + 488, UINT32_C(0xffffffff)); put32(info + 492, UINT32_C(0xffffffff));
    put32(info + 508, UINT32_C(0xaa550000));
    uint64_t part = (uint64_t)l.part_start * 512;
    if (sink->write(sink->context, part, boot, 512) || sink->write(sink->context, part + 512, info, 512)
        || sink->write(sink->context, part + 6 * 512, boot, 512) || sink->write(sink->context, part + 7 * 512, info, 512))
        return fail(error, capacity, "Cannot write the card image");
    /* FAT, twice. */
    size_t fat_bytes = (((size_t)l.clusters + 2) * 4 + 511) / 512 * 512;   /* the part in use */
    uint64_t fat_stride = (uint64_t)l.fat_sectors * 512;
    uint32_t *fat = calloc((size_t)l.clusters + 2, sizeof *fat);
    unsigned char *raw = calloc(1, fat_bytes);
    if (!fat || !raw) { free(fat); free(raw); return fail(error, capacity, "Out of memory"); }
    fat[0] = UINT32_C(0x0ffffff8); fat[1] = END_OF_CHAIN;
    chains(fat, root);
    for (uint32_t c = 0; c < l.clusters + 2; ++c) put32(raw + (size_t)c * 4, fat[c]);
    free(fat);
    int r = sink->write(sink->context, l.fat_offset, raw, fat_bytes)
         || sink->write(sink->context, l.fat_offset + fat_stride, raw, fat_bytes);
    free(raw);
    if (r) return fail(error, capacity, "Cannot write the card image");
    if (write_dirs(root, NULL, &l, sink, error, capacity)) return -1;
    size_t buffer_bytes = (size_t)1 << 20;
    unsigned char *buffer = malloc(buffer_bytes);
    if (!buffer) return fail(error, capacity, "Out of memory");
    r = write_files(root, &l, sink, buffer, buffer_bytes, error, capacity);
    free(buffer);
    return r;
}

/* ---- reader ---- */

struct m8fat_volume {
    m8fat_source source;
    layout l;
    uint32_t root_cluster;
    uint32_t *fat;
    unsigned char *seen;   /* directory clusters already walked */
};

m8fat_volume *m8fat_open(const m8fat_source *source, char *error, size_t capacity)
{
    unsigned char sector[512];
    if (source->image_bytes < 1024 * 1024 || source->read(source->context, 0, sector, 512)) {
        fail(error, capacity, "Cannot read the card image"); return NULL;
    }
    uint32_t start = 0;
    int is_boot = (sector[0] == 0xeb || sector[0] == 0xe9) && get16(sector + 11) == 512 && !memcmp(sector + 82, "FAT32", 5);
    if (!is_boot) {
        if (get16(sector + 510) != 0xaa55 || (sector[450] != 0x0b && sector[450] != 0x0c)) {
            fail(error, capacity, "Not a FAT32 card"); return NULL;
        }
        start = get32(sector + 454);
        if ((uint64_t)start * 512 + 512 > source->image_bytes || source->read(source->context, (uint64_t)start * 512, sector, 512)) {
            fail(error, capacity, "Card partition is outside the image"); return NULL;
        }
    }
    uint32_t spc = sector[13], reserved = get16(sector + 14), fats = sector[16], fat_sectors = get32(sector + 36);
    uint32_t total = get16(sector + 19) ? get16(sector + 19) : get32(sector + 32);
    if (get16(sector + 11) != 512 || !spc || (spc & (spc - 1)) || spc > 128 || !reserved || !fats || fats > 2
        || get16(sector + 17) || get16(sector + 22) || !fat_sectors || get16(sector + 510) != 0xaa55) {
        fail(error, capacity, "Not a FAT32 card"); return NULL;
    }
    m8fat_volume *v = calloc(1, sizeof *v);
    if (!v) { fail(error, capacity, "Out of memory"); return NULL; }
    v->source = *source;
    layout *l = &v->l;
    l->part_start = start; l->part_sectors = total; l->spc = spc; l->fat_sectors = fat_sectors;
    l->cluster_bytes = spc * 512;
    l->fat_offset = ((uint64_t)start + reserved) * 512;
    l->data_offset = l->fat_offset + (uint64_t)fats * fat_sectors * 512;
    uint64_t overhead = (uint64_t)reserved + (uint64_t)fats * fat_sectors;
    uint64_t clusters = total > overhead ? (total - overhead) / spc : 0;
    if (clusters < MIN_CLUSTERS || clusters > MAX_CLUSTERS || (clusters + 2) * 4 > (uint64_t)fat_sectors * 512
        || (uint64_t)(start + total) * 512 > source->image_bytes) {
        free(v); fail(error, capacity, "Invalid FAT32 geometry"); return NULL;
    }
    l->clusters = (uint32_t)clusters;
    v->root_cluster = get32(sector + 44);
    size_t fat_bytes = ((size_t)l->clusters + 2) * 4;
    unsigned char *raw = malloc(fat_bytes);
    v->fat = malloc(((size_t)l->clusters + 2) * sizeof *v->fat);
    v->seen = calloc(((size_t)l->clusters + 2 + 7) / 8, 1);
    if (!raw || !v->fat || !v->seen || source->read(source->context, l->fat_offset, raw, fat_bytes)) {
        free(raw); m8fat_close(v); fail(error, capacity, "Cannot read the card's FAT"); return NULL;
    }
    for (uint32_t c = 0; c < l->clusters + 2; ++c) v->fat[c] = get32(raw + (size_t)c * 4) & 0x0fffffffu;
    free(raw);
    return v;
}
void m8fat_close(m8fat_volume *v)
{
    if (!v) return;
    free(v->fat); free(v->seen); free(v);
}
static int next_cluster(const m8fat_volume *v, uint32_t c, uint32_t *next)
{
    uint32_t n = v->fat[c];
    if (n >= 0x0ffffff8u) { *next = 0; return 0; }
    if (n < 2 || n >= v->l.clusters + 2) return -1;
    *next = n;
    return 0;
}
static int walk(m8fat_volume *v, m8fat_node *dir, uint32_t first, unsigned depth, char *error, size_t capacity)
{
    if (depth > MAX_DEPTH) return fail(error, capacity, "Folders on the card are nested too deeply");
    unsigned char *buffer = malloc(v->l.cluster_bytes);
    if (!buffer) return fail(error, capacity, "Out of memory");
    uint16_t lfn[20 * 13];
    int lfn_parts = 0; unsigned char lfn_sum = 0; unsigned lfn_mask = 0;
    int result = 0, done = 0;
    for (uint32_t c = first, steps = 0; c && !done; ++steps) {
        if (c < 2 || c >= v->l.clusters + 2 || steps > v->l.clusters || (v->seen[c >> 3] & (1u << (c & 7)))) {
            result = fail(error, capacity, "Damaged folder on the card"); break;
        }
        v->seen[c >> 3] |= (unsigned char)(1u << (c & 7));
        if (v->source.read(v->source.context, cluster_offset(&v->l, c), buffer, v->l.cluster_bytes)) {
            result = fail(error, capacity, "Cannot read the card image"); break;
        }
        for (uint32_t off = 0; off < v->l.cluster_bytes && !done; off += 32) {
            unsigned char *e = buffer + off;
            if (!e[0]) { done = 1; break; }
            if (e[0] == 0xe5) { lfn_parts = 0; continue; }
            if ((e[11] & 0x3f) == 0x0f) {
                int ord = e[0] & 0x1f;
                if (ord < 1 || ord > 20) { lfn_parts = 0; continue; }
                if (e[0] & 0x40) { lfn_parts = ord; lfn_sum = e[13]; lfn_mask = 0; memset(lfn, 0xff, sizeof lfn); }
                else if (!lfn_parts || e[13] != lfn_sum) { lfn_parts = 0; continue; }
                static const int offsets[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
                for (int k = 0; k < 13; ++k) lfn[(ord - 1) * 13 + k] = (uint16_t)get16(e + offsets[k]);
                lfn_mask |= 1u << (ord - 1);
                continue;
            }
            if (e[11] & 0x08) { lfn_parts = 0; continue; }      /* volume label */
            if (e[0] == '.') { lfn_parts = 0; continue; }       /* . and .. */
            char *name = NULL;
            if (lfn_parts && lfn_mask == (1u << lfn_parts) - 1 && short_checksum(e) == lfn_sum) {
                int count = 0;
                while (count < lfn_parts * 13 && lfn[count] && lfn[count] != 0xffff) ++count;
                name = from_utf16(lfn, count);
            } else {
                char shortname[13]; int n = 0;
                for (int i = 0; i < 8 && e[i] != ' '; ++i) {
                    char ch = (char)(i == 0 && e[0] == 0x05 ? 0xe5 : e[i]);
                    shortname[n++] = (char)((e[12] & 0x08) && ch >= 'A' && ch <= 'Z' ? ch + 32 : ch);
                }
                if (e[8] != ' ') {
                    shortname[n++] = '.';
                    for (int i = 8; i < 11 && e[i] != ' '; ++i)
                        shortname[n++] = (char)((e[12] & 0x10) && e[i] >= 'A' && e[i] <= 'Z' ? e[i] + 32 : e[i]);
                }
                shortname[n] = 0;
                name = malloc((size_t)n + 1);
                if (name) memcpy(name, shortname, (size_t)n + 1);
            }
            lfn_parts = 0;
            if (!name) { result = fail(error, capacity, "Out of memory"); break; }
            m8fat_node *node = m8fat_node_new(name, (e[11] & 0x10) != 0);
            free(name);
            if (!node || m8fat_node_add(dir, node)) { m8fat_node_free(node); result = fail(error, capacity, "Out of memory"); break; }
            node->first = get16(e + 20) << 16 | get16(e + 26);
            node->size = node->directory ? 0 : get32(e + 28);
            node->time = (uint16_t)get16(e + 22); node->date = (uint16_t)get16(e + 24);
        }
        if (result || done) break;
        uint32_t following;
        if (next_cluster(v, c, &following)) { result = fail(error, capacity, "Damaged folder chain on the card"); break; }
        c = following;
    }
    free(buffer);
    if (result) return -1;
    for (size_t i = 0; i < dir->child_count; ++i) {
        m8fat_node *n = dir->children[i];
        if (n->directory && n->first && walk(v, n, n->first, depth + 1, error, capacity)) return -1;
    }
    return 0;
}
m8fat_node *m8fat_tree(m8fat_volume *v, char *error, size_t capacity)
{
    memset(v->seen, 0, ((size_t)v->l.clusters + 2 + 7) / 8);
    m8fat_node *root = m8fat_node_new("", 1);
    if (!root) { fail(error, capacity, "Out of memory"); return NULL; }
    root->first = v->root_cluster;
    if (walk(v, root, v->root_cluster, 0, error, capacity)) { m8fat_node_free(root); return NULL; }
    return root;
}
int m8fat_stream(m8fat_volume *v, const m8fat_node *file, int (*out)(void *, const void *, size_t), void *context)
{
    uint64_t remaining = file->size;
    if (!remaining) return 0;
    unsigned char *buffer = malloc(v->l.cluster_bytes);
    if (!buffer) return -1;
    int result = 0;
    for (uint32_t c = file->first, steps = 0; remaining; ++steps) {
        if (c < 2 || c >= v->l.clusters + 2 || steps > v->l.clusters) { result = -1; break; }
        size_t n = remaining < v->l.cluster_bytes ? (size_t)remaining : v->l.cluster_bytes;
        if (v->source.read(v->source.context, cluster_offset(&v->l, c), buffer, n) || out(context, buffer, n)) {
            result = -1; break;
        }
        remaining -= n;
        if (remaining && (next_cluster(v, c, &c) || !c)) { result = -1; break; }
    }
    free(buffer);
    return result;
}
int m8fat_touches(m8fat_volume *v, const m8fat_node *file, const uint8_t *dirty, size_t page_bytes)
{
    /* A folder's chain runs to its end-of-chain mark; a file's covers its size. */
    uint64_t remaining = file->directory ? UINT64_MAX : file->size;
    for (uint32_t c = file->first, steps = 0; remaining && c >= 2 && c < v->l.clusters + 2 && steps <= v->l.clusters; ++steps) {
        uint64_t start = cluster_offset(&v->l, c);
        for (uint64_t page = start / page_bytes, last = (start + v->l.cluster_bytes - 1) / page_bytes; page <= last; ++page)
            if (dirty[page >> 3] & (1u << (page & 7))) return 1;
        if (!file->directory) remaining -= remaining < v->l.cluster_bytes ? remaining : v->l.cluster_bytes;
        if (remaining && next_cluster(v, c, &c)) return 1; /* damaged: rewrite to be safe */
        if (!c) break;
    }
    return 0;
}

/* ---- sizing ---- */

void m8fat_usage(m8fat_volume *v, uint64_t *used_bytes, uint64_t *min_bytes, uint64_t *max_bytes)
{
    uint32_t used = 0, highest = 1;
    for (uint32_t c = 2; c < v->l.clusters + 2; ++c)
        if (v->fat[c]) { ++used; highest = c; }
    *used_bytes = (uint64_t)used * v->l.cluster_bytes;
    /* Smallest size that still holds the highest cluster in use. */
    *min_bytes = v->l.data_offset + (uint64_t)(highest - 1) * v->l.cluster_bytes;
    /* Largest size the reserved FAT can describe. */
    uint64_t clusters = (uint64_t)v->l.fat_sectors * 512 / 4 - 2;
    if (clusters > MAX_CLUSTERS) clusters = MAX_CLUSTERS;
    *max_bytes = v->l.data_offset + clusters * v->l.cluster_bytes;
}
int m8fat_resize(m8fat_volume *v, uint64_t new_bytes,
                 int (*write)(void *context, uint64_t offset, const void *data, size_t n), void *context,
                 char *error, size_t capacity)
{
    if (!v->l.part_start) return fail(error, capacity, "Only partitioned cards can be resized");
    layout l;
    if (plan_with(new_bytes, v->l.fat_sectors, &l) || l.data_offset != v->l.data_offset || l.spc != v->l.spc)
        return fail(error, capacity, "The card cannot be resized to %llu bytes", (unsigned long long)new_bytes);
    for (uint32_t c = l.clusters + 2; c < v->l.clusters + 2; ++c)
        if (v->fat[c]) return fail(error, capacity, "Files sit beyond the new end of the card");
    unsigned char sector[512];
    if (v->source.read(v->source.context, 0, sector, 512)) return fail(error, capacity, "Cannot read the card");
    put32(sector + 458, l.part_sectors);
    if (write(context, 0, sector, 512)) return fail(error, capacity, "Cannot write the card");
    uint64_t part = (uint64_t)v->l.part_start * 512;
    for (unsigned copy = 0; copy < 2; ++copy) {
        uint64_t boot = part + copy * 6 * 512;
        if (v->source.read(v->source.context, boot, sector, 512)) return fail(error, capacity, "Cannot read the card");
        put32(sector + 32, l.part_sectors);
        if (write(context, boot, sector, 512)) return fail(error, capacity, "Cannot write the card");
        if (v->source.read(v->source.context, boot + 512, sector, 512)) return fail(error, capacity, "Cannot read the card");
        if (get32(sector) == UINT32_C(0x41615252)) {
            put32(sector + 488, UINT32_C(0xffffffff)); put32(sector + 492, UINT32_C(0xffffffff));
            if (write(context, boot + 512, sector, 512)) return fail(error, capacity, "Cannot write the card");
        }
    }
    return 0;
}
