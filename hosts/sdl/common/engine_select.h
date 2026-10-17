#ifndef M8_ENGINE_SELECT_H
#define M8_ENGINE_SELECT_H

/* Where the generated engine comes from. The application ships none: the
 * libraries are derived from the firmware and are built on the user's machine.
 * Order: explicit CLI/environment paths, the per-firmware user engine folder
 * (user_paths.h), libraries bundled beside the executable (developer and
 * Linux builds), else the reference engine (Unicorn only). */
typedef enum { M8_ENGINE_EXPLICIT, M8_ENGINE_USER, M8_ENGINE_BUNDLED, M8_ENGINE_NONE } m8_engine_source;
typedef struct m8_engine_paths {
    m8_engine_source source;
    char *aot, *step, *regions; /* Set for M8_ENGINE_USER only; SDL_free() them. */
} m8_engine_paths;

/* explicit: a CLI path or M8RT_* library variable selects the libraries.
 * firmware_sha: lowercase binary SHA-256 of the active firmware, or NULL.
 * The user folder counts only when all three libraries are regular files;
 * loading still applies every existing contract check. */
void m8_engine_locate(int explicit_selection, const char *firmware_sha, m8_engine_paths *out);
void m8_engine_paths_free(m8_engine_paths *paths);
const char *m8_engine_source_name(m8_engine_source source);
#endif
