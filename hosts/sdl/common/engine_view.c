#include "engine_view.h"
#include "engine_builder.h"
#include "firmware_view.h"
#include <SDL3/SDL.h>
#include <stdio.h>

#define SECONDS(n) ((uint64_t)((n) * 1e9))

static struct {
    struct app_context *ctx;
    char firmware[4096], notice[41];
    bool attached, built, tools_missing, notice_error;
    m8_engine_state state;
    uint64_t polled, checked, notice_until;
} view;

static void notice(const char *text, bool error, double seconds)
{
    SDL_strlcpy(view.notice, text, sizeof view.notice);
    view.notice_error = error;
    view.notice_until = SDL_GetTicksNS() + SECONDS(seconds);
    m8_firmware_view_message(view.notice, error);
}

static void action(void *context);

/* The label follows the builder: no action when this app cannot build. */
static void relabel(void)
{
    char reason[64];
    view.checked = SDL_GetTicksNS();
    if (view.state == M8_ENGINE_RUNNING) { m8_firmware_view_set_engine_action("CANCEL BUILD", action, NULL); return; }
    int tools = 0, unavailable = m8_engine_builder_available(&tools, reason, sizeof reason);
    view.tools_missing = unavailable && tools;
    m8_firmware_view_set_engine_action(view.tools_missing ? "INSTALL COMMAND LINE TOOLS" : view.built ?
        "REBUILD NATIVE ENGINE" : "BUILD NATIVE ENGINE", unavailable && !tools ? NULL : action, NULL);
}

static void action(void *context)
{
    char error[96];
    (void)context;
    if (view.state == M8_ENGINE_RUNNING) {
        m8_engine_builder_cancel();
        view.state = M8_ENGINE_CANCELLED;
        notice("BUILD CANCELLED", false, 5);
    } else if (view.tools_missing && (relabel(), view.tools_missing)) {
        if (m8_engine_builder_install_tools(error, sizeof error)) notice(error, true, 6);
        else notice("INSTALL THE TOOLS, THEN SELECT AGAIN", false, 30);
        return;
    } else if (m8_engine_builder_start(view.firmware, error, sizeof error)) notice(error, true, 8);
    else {
        view.state = M8_ENGINE_RUNNING;
        notice("BUILDING: A FEW MINUTES, PLAY ON", false, 4);
        printf("engine_build=started log=%s\n", m8_engine_builder_log());
        fflush(stdout);
    }
    relabel();
}

void m8_engine_view_attach(struct app_context *ctx, const char *firmware, int user_engine, const char *rejected)
{
    view.ctx = ctx;
    view.built = user_engine;
    SDL_strlcpy(view.firmware, firmware ? firmware : "", sizeof view.firmware);
    view.attached = firmware != NULL;
    if (!view.attached) return;
    relabel();
    if (rejected && *rejected) {
        char text[64];
        snprintf(text, sizeof text, "ENGINE %s", rejected);
        view.built = true;
        relabel();
        /* The screen opens once on a fresh launch; a rebuild is one EDIT away.
         * Windows cannot build engines yet, so there it stays behind F4. */
#ifndef _WIN32
        if (!m8_firmware_view_is_open()) m8_firmware_view_toggle(ctx);
#endif
        notice(text, true, 20);
    }
}

void m8_engine_view_tick(void)
{
    if (!view.attached) return;
    uint64_t now = SDL_GetTicksNS();
    if (view.state == M8_ENGINE_RUNNING && now - view.polled >= SECONDS(0.25)) {
        char text[96], line[64];
        int percent = 0;
        view.polled = now;
        view.state = m8_engine_builder_poll(&percent, text, sizeof text);
        if (view.state == M8_ENGINE_RUNNING) {
            snprintf(line, sizeof line, "%3d%% %s", percent, text);
            notice(line, false, 2);
        } else {
            printf("engine_build=%s status=\"%s\" log=%s\n", view.state == M8_ENGINE_DONE ? "done" : "failed",
                   text, m8_engine_builder_log());
            fflush(stdout);
            if (view.state == M8_ENGINE_DONE) { view.built = true; notice("ENGINE BUILT. RESTART M8EMU TO USE IT", false, 60); }
            else { snprintf(line, sizeof line, "FAILED: %s", text); notice(line, true, 60); }
            relabel();
        }
    } else if (view.notice_until > now && now - view.polled >= SECONDS(1)) {
        /* The screen's message row fades after seconds; keep results up. */
        view.polled = now;
        m8_firmware_view_message(view.notice, view.notice_error);
    }
    /* Notice a finished Command Line Tools installation while the screen is up. */
    if (view.tools_missing && m8_firmware_view_is_open() && now - view.checked >= SECONDS(3)) relabel();
}

void m8_engine_view_shutdown(void) { m8_engine_builder_cancel(); }
