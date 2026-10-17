#ifndef M8_USER_PATHS_H
#define M8_USER_PATHS_H
#include <stddef.h>

/* Per-user application data, beside the default card:
 * $XDG_DATA_HOME/m8-live, else ~/Library/Application Support/m8-live on macOS
 * or ~/.local/share/m8-live elsewhere. Plain libc; every result is malloc'd
 * (free() it) and NULL with errno set when HOME and XDG_DATA_HOME are unset. */
#define M8_USER_FIRMWARE_NAME "M8_V6_5_2C_HEADLESS.bin"
char *m8_user_data_dir(void);
/* <data>/firmware/M8_V6_5_2C_HEADLESS.bin: the verified binary chosen by the
 * user. The application never ships it. */
char *m8_user_firmware_path(void);
/* <data>/engine/<sha>: per-firmware generated engine built on this machine,
 * holding libm8aot, libm8native-step and libm8native-regions with the host
 * library suffix. sha is the firmware binary's SHA-256, 64 hex digits; it is
 * lowercased. NULL with EINVAL for any other string. No trailing slash. */
char *m8_user_engine_dir(const char *bin_sha256);
/* <dir>/<name>, malloc'd. */
char *m8_user_join(const char *dir, const char *name);
/* Create every missing parent directory of path, mode 0700. 0 or -1/errno. */
int m8_user_make_parents(const char *path);
#endif
