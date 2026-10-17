#ifndef M8_ENGINE_VIEW_H
#define M8_ENGINE_VIEW_H

struct app_context;

/* The on-device builder's entry point: the ENGINE action of the firmware
 * screen (F4, firmware_view.h). It reads BUILD NATIVE ENGINE, REBUILD NATIVE
 * ENGINE, INSTALL COMMAND LINE TOOLS or CANCEL BUILD, and the screen's
 * message row shows progress and the result. A finished engine loads from
 * the next launch. rejected: why an existing user engine was refused at this
 * launch, or NULL/empty; the screen then opens once and offers a rebuild. */
void m8_engine_view_attach(struct app_context *ctx, const char *firmware, int user_engine, const char *rejected);
/* Main loop, every iteration: polls a running build. Cheap when idle. */
void m8_engine_view_tick(void);
/* At exit: stops a running build. */
void m8_engine_view_shutdown(void);

#endif
