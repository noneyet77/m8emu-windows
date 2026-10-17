#include "engine_select.h"
#include "aot.h"
#include "firmware.h"
#include "user_paths.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
#endif

static const char *const libraries[] = {"libm8aot" M8_LIBRARY_SUFFIX, "libm8native-step" M8_LIBRARY_SUFFIX,
                                        "libm8native-regions" M8_LIBRARY_SUFFIX};

static char *regular(const char *dir, const char *name) {
    char *path = NULL;
    SDL_PathInfo info;
    if (!dir || SDL_asprintf(&path, "%s/%s", dir, name) < 0) return NULL;
    if (SDL_GetPathInfo(path, &info) && info.type == SDL_PATHTYPE_FILE) return path;
    SDL_free(path);
    return NULL;
}

int m8_engine_file_sha256(const char *path, char hex[65]) {
#ifdef __APPLE__
    /* Hardware-assisted: launch validation hashes about 24 MB of libraries. */
    static const char digits[] = "0123456789abcdef";
    unsigned char block[1 << 16], digest[CC_SHA256_DIGEST_LENGTH];
    FILE *file = fopen(path, "rb");
    if (!file) return -1;
    CC_SHA256_CTX ctx;
    CC_SHA256_Init(&ctx);
    for (size_t n; (n = fread(block, 1, sizeof block, file)) > 0;) CC_SHA256_Update(&ctx, block, (CC_LONG)n);
    CC_SHA256_Final(digest, &ctx);
    int failed = ferror(file);
    fclose(file);
    for (int i = 0; i < 32; ++i) { hex[i * 2] = digits[digest[i] >> 4]; hex[i * 2 + 1] = digits[digest[i] & 15]; }
    hex[64] = 0;
    return failed ? -1 : 0;
#else
    return m8_firmware_file_sha256(path, hex, NULL, 0);
#endif
}

static int read_text(const char *path, char *out, size_t capacity) {
    FILE *file = path ? fopen(path, "rb") : NULL;
    if (!file) return -1;
    size_t n = fread(out, 1, capacity - 1, file);
    out[n] = 0;
    fclose(file);
    return 0;
}

/* The first "key": "value" or "key": number after from, in json.dumps output. */
static int json_value(const char *from, const char *key, char *out, size_t capacity) {
    char pattern[80];
    snprintf(pattern, sizeof pattern, "\"%s\": ", key);
    const char *p = from ? strstr(from, pattern) : NULL;
    if (!p) return -1;
    p += strlen(pattern);
    int quoted = *p == '"';
    p += quoted;
    size_t n = quoted ? strcspn(p, "\"") : strspn(p, "0123456789");
    if (!n || n >= capacity) return -1;
    memcpy(out, p, n);
    out[n] = 0;
    return 0;
}

static int fail(char *reason, size_t capacity, const char *text, int result) {
    if (reason && capacity) SDL_strlcpy(reason, text, capacity);
    return result;
}

int m8_engine_user_check(const char *dir, const char *sha, char *reason, size_t capacity) {
    char text[4096], value[96], id[96], *path = NULL;
    int present = 0;
    fail(reason, capacity, "", 0);
    for (unsigned i = 0; i < 3; ++i) { char *file = regular(dir, libraries[i]); present += file != NULL; SDL_free(file); }
    const char *base = SDL_GetBasePath();
    if (base && SDL_asprintf(&path, "%sengine-src/ENGINE_ID", base) >= 0 && !read_text(path, id, sizeof id))
        id[strcspn(id, "\r\n")] = 0;
    else *id = 0;
    SDL_free(path); path = NULL;
    if (!*id) return present == 3 ? 0 : fail(reason, capacity, "NOT BUILT", 1);
    if (dir) SDL_asprintf(&path, "%s/engine.json", dir);
    int stamped = !read_text(path, text, sizeof text);
    SDL_free(path);
    if (!stamped) return present ? fail(reason, capacity, "INCOMPLETE OR UNVERIFIED BUILD", -1) : fail(reason, capacity, "NOT BUILT", 1);
    if (json_value(text, "schema", value, sizeof value) || strcmp(value, "m8-user-engine/1"))
        return fail(reason, capacity, "UNKNOWN ENGINE STAMP", -1);
    if (!sha || json_value(text, "firmware_sha256", value, sizeof value) || strcmp(value, sha))
        return fail(reason, capacity, "BUILT FOR OTHER FIRMWARE", -1);
    if (json_value(text, "engine_id", value, sizeof value) || strcmp(value, id))
        return fail(reason, capacity, "BUILT BY ANOTHER APP VERSION", -1);
    for (unsigned i = 0; i < 3; ++i) {
        char key[64], size[32], digest[65];
        SDL_PathInfo info;
        snprintf(key, sizeof key, "\"%s\"", libraries[i]);
        const char *entry = strstr(text, key);
        char *file = regular(dir, libraries[i]);
        int bad = !entry || !file || json_value(entry, "size", size, sizeof size) ||
                  json_value(entry, "sha256", value, sizeof value) || !SDL_GetPathInfo(file, &info) ||
                  (unsigned long long)info.size != strtoull(size, NULL, 10) ||
                  m8_engine_file_sha256(file, digest) || strcmp(digest, value);
        SDL_free(file);
        if (bad) {
            snprintf(key, sizeof key, "%s DAMAGED", libraries[i] + 3);
            for (char *c = key; *c; ++c) if (*c >= 'a' && *c <= 'z') *c = (char)(*c - 32);
            return fail(reason, capacity, key, -1);
        }
    }
    return 0;
}

void m8_engine_locate(int explicit_selection, const char *firmware_sha, m8_engine_paths *out) {
    *out = (m8_engine_paths){M8_ENGINE_EXPLICIT, NULL, NULL, NULL, ""};
    if (explicit_selection) return;
    char *dir = firmware_sha ? m8_user_engine_dir(firmware_sha) : NULL;
    if (dir && m8_engine_user_check(dir, firmware_sha, out->rejected, sizeof out->rejected) == 0) {
        out->aot = regular(dir, libraries[0]);
        out->step = regular(dir, libraries[1]);
        out->regions = regular(dir, libraries[2]);
    }
    free(dir);
    if (out->aot && out->step && out->regions) { out->source = M8_ENGINE_USER; return; }
    m8_engine_paths_free(out);
    /* The same folders the AOT and native loaders search. */
    const char *base = SDL_GetBasePath();
    static const char *const folders[] = {"lib", "libs.aarch64"};
    out->source = M8_ENGINE_NONE;
    for (unsigned i = 0; base && i < sizeof folders / sizeof folders[0]; ++i) {
        char *folder = NULL, *aot = NULL;
        if (SDL_asprintf(&folder, "%s%s", base, folders[i]) >= 0) aot = regular(folder, libraries[0]);
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
