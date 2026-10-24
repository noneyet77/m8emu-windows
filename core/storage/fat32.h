#ifndef M8_FAT32_H
#define M8_FAT32_H
/* A small FAT32 card builder and reader for the card folder mirror.
 *
 * The builder lays out a whole partitioned FAT32 image from a tree whose file
 * bytes are pulled through a callback, writing through another callback, so a
 * multi-gigabyte card never has to be held in memory. Files are stored in
 * contiguous clusters; every name gets long-name entries and a unique 8.3
 * alias. The reader walks an image through a read callback. */
#include <stddef.h>
#include <stdint.h>

typedef struct m8fat_node m8fat_node;
struct m8fat_node {
    char *name;              /* UTF-8; "" for the root */
    int directory;
    uint64_t size;           /* file bytes */
    uint16_t date, time;     /* DOS timestamp */
    void *host;              /* builder: caller's handle for the file's bytes */
    uint32_t first, clusters;/* first cluster and cluster count (builder) */
    uint64_t hash;           /* builder: FNV-1a of the bytes written */
    m8fat_node **children;
    size_t child_count, child_capacity;
};

m8fat_node *m8fat_node_new(const char *name, int directory);
int m8fat_node_add(m8fat_node *parent, m8fat_node *child);  /* takes ownership */
void m8fat_node_free(m8fat_node *node);

typedef struct {
    void *context;
    /* Write image bytes. Regions never written must read back as zero
     * (a sparse or freshly extended file does). */
    int (*write)(void *context, uint64_t offset, const void *data, size_t n);
    /* Read n bytes of a file at offset from the caller's source. */
    int (*read_file)(void *context, const m8fat_node *file, uint64_t offset, void *out, size_t n);
} m8fat_sink;

/* Bytes of card space the tree needs, including directories. */
uint64_t m8fat_required(const m8fat_node *root, uint64_t max_bytes);
/* Build an image of image_bytes whose FATs leave room to grow to max_bytes. */
int m8fat_build(m8fat_node *root, uint64_t image_bytes, uint64_t max_bytes, const m8fat_sink *sink,
                char *error, size_t capacity);

typedef struct {
    void *context;
    int (*read)(void *context, uint64_t offset, void *out, size_t n);
    uint64_t image_bytes;
} m8fat_source;

typedef struct m8fat_volume m8fat_volume;
/* Opens a FAT32 image (MBR-partitioned or not); NULL with error otherwise. */
m8fat_volume *m8fat_open(const m8fat_source *source, char *error, size_t capacity);
void m8fat_close(m8fat_volume *volume);
/* Directory tree with names, sizes and first clusters (host is NULL). */
m8fat_node *m8fat_tree(m8fat_volume *volume, char *error, size_t capacity);
/* Stream a file's bytes to out in order. */
int m8fat_stream(m8fat_volume *volume, const m8fat_node *file,
                 int (*out)(void *context, const void *data, size_t n), void *context);
/* 1 when any cluster of the file (or a folder's own entries) lies on a page set in dirty (bit per
 * page_bytes, LSB first), 0 otherwise. */
/* Bytes in use, the smallest image size that keeps every cluster in use,
 * and the largest size the reserved FAT allows. */
void m8fat_usage(m8fat_volume *volume, uint64_t *used_bytes, uint64_t *min_bytes, uint64_t *max_bytes);
/* Rewrite the size fields for new_bytes (the caller then extends or
 * truncates the file: extend first when growing, truncate after when
 * shrinking). Refused when clusters in use lie beyond the new end. */
int m8fat_resize(m8fat_volume *volume, uint64_t new_bytes,
                 int (*write)(void *context, uint64_t offset, const void *data, size_t n), void *context,
                 char *error, size_t capacity);
int m8fat_touches(m8fat_volume *volume, const m8fat_node *file, const uint8_t *dirty, size_t page_bytes);
#endif
