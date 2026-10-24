#ifndef M8_CARD_PLATFORM_H
#define M8_CARD_PLATFORM_H
#include <stddef.h>

/* The card policy and FAT formatter use this filesystem boundary. A backend
 * holds one exclusive card lock, reads the opened source, rejects source
 * changes, verifies staged images, and publishes .bak before the card. It
 * must preserve partial publication on failure; the caller never retries.
 * POSIX app-private files implement this contract. Android document-provider
 * URIs require a separate backend with its own stated durability contract. */
typedef struct m8_card_platform m8_card_platform;
typedef int (*m8_card_platform_reader)(void *context, size_t offset,
                                     unsigned char *out, size_t count);

m8_card_platform *m8_card_platform_open(const char *path, int read_only,
    int create, size_t initial_size, m8_card_platform_reader initial_reader,
    void *initial_context, char *error, size_t error_capacity);
const char *m8_card_platform_path(const m8_card_platform *card);
int m8_card_platform_read_only(const m8_card_platform *card);
size_t m8_card_platform_size(const m8_card_platform *card);
const char *m8_card_platform_error(const m8_card_platform *card);
int m8_card_platform_protect_file(m8_card_platform *card, const char *path);
int m8_card_platform_save(m8_card_platform *card, size_t size,
                         m8_card_platform_reader reader, void *context);
void m8_card_platform_close(m8_card_platform *card);
/* Save only the pages set in dirty (bit per page_bytes, LSB first) in place,
 * through a journal that is replayed on the next open if interrupted. With
 * all set, or where unsupported, this is m8_card_platform_save. No set
 * pages and !all: nothing is written. */
int m8_card_platform_save_pages(m8_card_platform *card, size_t size, m8_card_platform_reader reader,
                                void *context, const unsigned char *dirty, int all, size_t page_bytes);
/* Replace the card file with the image at path (same folder), keeping the
 * previous card as .bak. Only before a runtime has loaded the card. */
int m8_card_platform_replace(m8_card_platform *card, const char *path);
/* Accept the card file's current size and time as the session's own (after
 * a deliberate resize under the lock, before a runtime loads it). */
void m8_card_platform_refresh(m8_card_platform *card);
#endif
