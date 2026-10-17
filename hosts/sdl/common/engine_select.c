#include "engine_select.h"
#include "aot.h"
#include "user_paths.h"
#include <SDL3/SDL.h>
#include <stdlib.h>

static char *regular(const char *dir, const char *name) {
    char *path = NULL;
    SDL_PathInfo info;
    if (!dir || SDL_asprintf(&path, "%s/%s", dir, name) < 0) return NULL;
    if (SDL_GetPathInfo(path, &info) && info.type == SDL_PATHTYPE_FILE) return path;
    SDL_free(path);
    return NULL;
}

void m8_engine_locate(int explicit_selection, const char *firmware_sha, m8_engine_paths *out) {
    *out = (m8_engine_paths){M8_ENGINE_EXPLICIT, NULL, NULL, NULL};
    if (explicit_selection) return;
    char *dir = firmware_sha ? m8_user_engine_dir(firmware_sha) : NULL;
    out->aot = regular(dir, "libm8aot" M8_LIBRARY_SUFFIX);
    out->step = regular(dir, "libm8native-step" M8_LIBRARY_SUFFIX);
    out->regions = regular(dir, "libm8native-regions" M8_LIBRARY_SUFFIX);
    free(dir);
    if (out->aot && out->step && out->regions) { out->source = M8_ENGINE_USER; return; }
    m8_engine_paths_free(out);
    /* The same folders the AOT and native loaders search. */
    const char *base = SDL_GetBasePath();
    static const char *const folders[] = {"lib", "libs.aarch64"};
    out->source = M8_ENGINE_NONE;
    for (unsigned i = 0; base && i < sizeof folders / sizeof folders[0]; ++i) {
        char *folder = NULL, *aot = NULL;
        if (SDL_asprintf(&folder, "%s%s", base, folders[i]) >= 0) aot = regular(folder, "libm8aot" M8_LIBRARY_SUFFIX);
        SDL_free(folder);
        if (aot) { SDL_free(aot); out->source = M8_ENGINE_BUNDLED; return; }
    }
}

void m8_engine_paths_free(m8_engine_paths *paths) {
    if (!paths) return;
    SDL_free(paths->aot); SDL_free(paths->step); SDL_free(paths->regions);
    paths->aot = paths->step = paths->regions = NULL;
}

const char *m8_engine_source_name(m8_engine_source source) {
    static const char *const names[] = {"explicit", "user", "bundled", "none"};
    return (unsigned)source < sizeof names / sizeof names[0] ? names[source] : "unknown";
}
