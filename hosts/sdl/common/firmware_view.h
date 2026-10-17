#ifndef M8_FIRMWARE_VIEW_H
#define M8_FIRMWARE_VIEW_H
#include <SDL3/SDL.h>

struct app_context;

/* An M8-style firmware screen on the full 320x240 canvas. The application
 * ships without Dirtywave's firmware; this screen asks for the official
 * 6.5.2C release, verifies it and stores the binary in the user data folder.
 * A file arrives through the native open panel (EDIT on CHOOSE FILE or
 * Cmd/Ctrl+O) or by dropping it on the window. UP/DOWN move, EDIT activates.
 *
 * Init once, after the renderer, with the user firmware path (malloc'd copy
 * kept). Setup runs before the guest exists and returns 1 once a verified
 * firmware is installed at destination, or 0 when the user quits. notice, if any, is
 * shown first (e.g. a stored firmware that failed its check). smoke, if any,
 * is dropped on the window once, for unattended tests. */
void m8_firmware_view_init(const char *destination);
int m8_firmware_view_setup(struct app_context *ctx, const char *notice, const char *smoke);
/* While the guest runs, F4 or a dropped file opens the same screen as an
 * overlay; a newly installed file applies from the next launch. OPTION,
 * ESC or F4 closes it. */
bool m8_firmware_view_is_open(void);
void m8_firmware_view_toggle(struct app_context *ctx);
/* Consumes keyboard, gamepad and drop input while open; F4 and drops open it. */
bool m8_firmware_view_event(struct app_context *ctx, const SDL_Event *event);
/* Called by m8c render.c after the other overlays, on the canvas target. */
void m8_firmware_view_render(SDL_Renderer *renderer, int width, int height);

/* Status rows while the guest runs, e.g. "6.5.2C USER" and "NATIVE". */
void m8_firmware_view_set_firmware(const char *value);
void m8_firmware_view_set_engine(const char *value);
/* Hook for an on-device engine builder: a selectable action on the ENGINE
 * row, e.g. "BUILD NATIVE ENGINE". NULL removes it. The action runs on the
 * main thread and may report progress with m8_firmware_view_message(). */
typedef void (*m8_firmware_view_action)(void *context);
void m8_firmware_view_set_engine_action(const char *label, m8_firmware_view_action action, void *context);
/* Message row; error selects the warning color. Shown for a few seconds. */
void m8_firmware_view_message(const char *text, bool error);
#endif
