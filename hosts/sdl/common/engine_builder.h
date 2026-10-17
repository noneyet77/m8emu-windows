#ifndef M8_ENGINE_BUILDER_H
#define M8_ENGINE_BUILDER_H
#include <stddef.h>

/* The distributable app carries no firmware-derived code. This module builds
 * libm8aot, libm8native-step and libm8native-regions on the user's Mac from
 * the user's own firmware, with the generators, recipe and capstone staged in
 * the bundle's engine-src/ (cmake/M8EngineSource.cmake), into
 * <data>/m8-live/engine/<BIN sha256>/ (user_paths.h). A build is a child
 * process group, engine_build.py under the Command Line Tools python3, that
 * logs to build.log there and installs engine.json last; engine_select.c
 * validates it at every launch. Nothing is written inside the app bundle.
 * macOS only: elsewhere the builder reports itself unavailable. */

typedef enum m8_engine_state {
    M8_ENGINE_IDLE, M8_ENGINE_RUNNING, M8_ENGINE_DONE, M8_ENGINE_FAILED, M8_ENGINE_CANCELLED
} m8_engine_state;

/* 0 when this Mac can build now. Otherwise -1, a short reason, and
 * *tools_missing = 1 when installing the Command Line Tools would help. */
int m8_engine_builder_available(int *tools_missing, char *reason, size_t capacity);
/* Opens Apple's Command Line Tools installer (xcode-select --install). */
int m8_engine_builder_install_tools(char *error, size_t capacity);
/* Starts a build for this firmware; the file is only read. */
int m8_engine_builder_start(const char *firmware, char *error, size_t capacity);
/* Non-blocking. percent is 0..100; text is a short upper-case status. DONE
 * means the installed engine passed the launch-time validation. */
m8_engine_state m8_engine_builder_poll(int *percent, char *text, size_t capacity);
/* Stops the build and its compilers; an installed engine stays valid. */
void m8_engine_builder_cancel(void);
/* Builds synchronously with progress on stdout (m8emu --build-engine). */
int m8_engine_builder_run(const char *firmware);
/* The current or last build's log; empty before any build. */
const char *m8_engine_builder_log(void);

#endif
