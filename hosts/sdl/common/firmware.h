#ifndef M8_HOST_FIRMWARE_H
#define M8_HOST_FIRMWARE_H
#include <stddef.h>

/* The one supported firmware. CMake passes the pins of cmake/M8Firmware.cmake. */
#ifndef M8_FIRMWARE_HEX_SHA256
#error "M8_FIRMWARE_HEX_SHA256 must come from cmake/M8Firmware.cmake"
#endif
#ifndef M8_FIRMWARE_BIN_SHA256
#error "M8_FIRMWARE_BIN_SHA256 must come from cmake/M8Firmware.cmake"
#endif
#define M8_FIRMWARE_VERSION "6.5.2C"
#define M8_FIRMWARE_PAGE "https://github.com/Dirtywave/M8HeadlessFirmware/blob/main/Releases/M8_V6_5_2C_HEADLESS.hex"

typedef enum m8_firmware_status {
    M8_FIRMWARE_OK,
    M8_FIRMWARE_WRONG,   /* A firmware image, but not the supported release. */
    M8_FIRMWARE_INVALID, /* Not a usable Intel HEX or flash image. */
    M8_FIRMWARE_READ,    /* The source cannot be read. */
    M8_FIRMWARE_WRITE    /* The destination cannot be written. */
} m8_firmware_status;

/* Lowercase hex SHA-256 of a whole file. 0 or -1 with error set. */
int m8_firmware_file_sha256(const char *path, char out[65], char *error, size_t capacity);
/* 1 when path is a regular file with the pinned binary SHA-256, else 0. */
int m8_firmware_verified(const char *path);
/* Port of cmake/firmware.py hex_to_binary: strict records and checksums,
 * flash at 0x60000000..+8 MiB, 0xff fill, trimmed at the last data byte.
 * Returns a malloc'd image, or NULL with error set. */
unsigned char *m8_firmware_hex_to_binary(const unsigned char *source, size_t size,
                                         size_t *out_size, char *error, size_t capacity);
/* Verify an official Intel HEX or flash image, convert it and publish the
 * pinned binary at destination atomically (temporary file, fsync, rename).
 * The binary SHA-256 is authoritative: a HEX whose bytes differ only in
 * ways that convert to the pinned image, such as CRLF line ends, passes. */
m8_firmware_status m8_firmware_install(const char *source, const char *destination,
                                       char *error, size_t capacity);
#endif
