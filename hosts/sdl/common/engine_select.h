#ifndef M8_ENGINE_SELECT_H
#define M8_ENGINE_SELECT_H
#include <stddef.h>

/* Where the generated engine comes from. The application ships none: the
 * libraries are derived from the firmware and are built on the user's machine.
 * Order: explicit CLI/environment paths, the per-firmware user engine folder
 * (user_paths.h), libraries bundled beside the executable (developer and
 * Linux builds), else the reference engine (Unicorn only). */
typedef enum { M8_ENGINE_EXPLICIT, M8_ENGINE_USER, M8_ENGINE_BUNDLED, M8_ENGINE_NONE } m8_engine_source;
typedef struct m8_engine_paths {
    m8_engine_source source;
    char *aot, *step, *regions; /* Set for M8_ENGINE_USER only; SDL_free() them. */
    char rejected[64];          /* Why an existing user engine was skipped, else empty. */
} m8_engine_paths;

/* explicit: a CLI path or M8RT_* library variable selects the libraries.
 * firmware_sha: lowercase binary SHA-256 of the active firmware, or NULL.
 * The user folder counts only when m8_engine_user_check accepts it; loading
 * still applies every existing contract check. */
void m8_engine_locate(int explicit_selection, const char *firmware_sha, m8_engine_paths *out);
void m8_engine_paths_free(m8_engine_paths *paths);
const char *m8_engine_source_name(m8_engine_source source);

/* The on-device builder (engine_builder.h) writes engine.json last. With its
 * sources bundled (engine-src/ENGINE_ID beside the program), a user engine is
 * complete only when engine.json names this firmware, that ENGINE_ID, and the
 * size and SHA-256 of each library; a new app version with other generators
 * or flags therefore asks for a rebuild. Bundles without the builder accept
 * the three files alone. 0: complete. 1: nothing built. -1: unusable, with a
 * short upper-case reason. */
int m8_engine_user_check(const char *dir, const char *firmware_sha, char *reason, size_t capacity);
/* Lowercase SHA-256 of a whole file; 0 or -1. */
int m8_engine_file_sha256(const char *path, char hex[65]);
#endif
