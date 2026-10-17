#include "firmware_view.h"
#include "firmware.h"
#include "handheld.h"
#include "m8_ui.h"
#include "common.h"
#include "render.h"
#include <stdio.h>
#include <string.h>

enum { ROW_TITLE = 2, ROW_FIRMWARE = 8, ROW_ENGINE = 9, ROW_HOOK = 10, ROW_ACTIONS = 12, ROW_DROP = 16,
       MESSAGE_ROW = 18, HINT_ROW = 22, VALUE_COL = 11 };
enum { ACT_ENGINE, ACT_CHOOSE, ACT_DOWNLOAD, ACT_CLOSE, ACT_COUNT };
enum { A_NONE, A_UP, A_DOWN, A_EDIT, A_OPTION, A_CLOSE, A_CHOOSE };
#define SECONDS(n) ((uint64_t)((n) * 1e9))
#ifdef __APPLE__
#define CHOOSE_KEY "CMD+O"
#else
#define CHOOSE_KEY "CTRL+O"
#endif

static struct {
    struct app_context *ctx;
    SDL_Texture *texture;
    char *destination;
    bool open, setup, installed, dialog, option, option_used;
    int row;
    Uint32 event_type;
    char firmware[M8_UI_COLUMNS + 1], engine[M8_UI_COLUMNS + 1], hook[M8_UI_COLUMNS + 1];
    m8_firmware_view_action action;
    void *action_context;
    char message[M8_UI_COLUMNS + 1];
    bool message_error;
    uint64_t message_until, hover_until, installed_ns;
} view = {.row = ACT_CHOOSE, .firmware = "NOT SET", .engine = "REFERENCE"};

static void copy(char *out, const char *text) { SDL_strlcpy(out, text ? text : "", M8_UI_COLUMNS + 1); }
static void say(const char *text, bool error, uint64_t seconds_ns) {
    copy(view.message, text); view.message_error = error;
    view.message_until = seconds_ns == UINT64_MAX ? UINT64_MAX : SDL_GetTicksNS() + seconds_ns;
    renderer_request_redraw();
}
void m8_firmware_view_message(const char *text, bool error) { say(text, error, SECONDS(4)); }
void m8_firmware_view_set_firmware(const char *value) { copy(view.firmware, value); }
void m8_firmware_view_set_engine(const char *value) { copy(view.engine, value); }
void m8_firmware_view_set_engine_action(const char *label, m8_firmware_view_action action, void *context) {
    copy(view.hook, action ? label : NULL);
    view.action = action; view.action_context = context;
    if (!action && view.row == ACT_ENGINE) view.row = ACT_CHOOSE;
}

void m8_firmware_view_init(const char *destination) {
    SDL_free(view.destination);
    view.destination = destination ? SDL_strdup(destination) : NULL;
    if (!view.event_type) view.event_type = SDL_RegisterEvents(1);
}
bool m8_firmware_view_is_open(void) { return view.open; }

/* ---- actions ---- */

static void install(const char *path) {
    static const char *names[] = {"ok", "wrong", "invalid", "read", "write"};
    static const char *messages[] = {("FIRMWARE " M8_FIRMWARE_VERSION " INSTALLED"), ("WRONG FIRMWARE: NEED " M8_FIRMWARE_VERSION),
                                     "NOT AN M8 FIRMWARE FILE", "CANNOT READ THAT FILE", "CANNOT SAVE THE FIRMWARE"};
    char error[512] = {0};
    m8_firmware_status status = view.destination ?
        m8_firmware_install(path, view.destination, error, sizeof error) : M8_FIRMWARE_WRITE;
    printf("firmware_install=%s status=%s destination=%s%s%s\n", path, names[status],
           view.destination ? view.destination : "<none>", *error ? " detail=" : "", error);
    fflush(stdout);
    if (status == M8_FIRMWARE_OK) {
        copy(view.firmware, view.setup ? M8_FIRMWARE_VERSION " OK" : M8_FIRMWARE_VERSION " SAVED");
        say(view.setup ? messages[0] : "SAVED. USED FROM THE NEXT LAUNCH", false, view.setup ? UINT64_MAX : SECONDS(5));
        if (view.setup) { view.installed = true; view.installed_ns = SDL_GetTicksNS(); }
    } else say(messages[status], true, view.setup ? UINT64_MAX : SECONDS(6));
}

/* SDL may call this on another thread; the main loop receives the path. */
static void SDLCALL chosen(void *context, const char *const *files, int filter) {
    (void)context; (void)filter;
    SDL_Event event;
    SDL_zero(event);
    event.type = view.event_type;
    event.user.code = !files ? 2 : !files[0] ? 1 : 0;
    event.user.data1 = files && files[0] ? SDL_strdup(files[0]) : NULL;
    if (!SDL_PushEvent(&event)) SDL_free(event.user.data1);
}

static void choose(void) {
    static const SDL_DialogFileFilter filters[] = {{"M8 firmware (.hex, .bin)", "hex;bin"}, {"All files", "*"}};
    if (view.dialog || view.installed) return;
    int count = 0;
    SDL_Window **windows = SDL_GetWindows(&count), *window = count ? windows[0] : NULL;
    SDL_free(windows);
    view.dialog = true;
    SDL_ShowOpenFileDialog(chosen, NULL, window, filters, 2, SDL_GetUserFolder(SDL_FOLDER_DOWNLOADS), false);
}

static void close_view(void) {
    if (!view.open) return;
    view.open = false;
    if (view.ctx) m8_handheld_release(view.ctx);
    renderer_request_redraw();
}

static void activate(void) {
    switch (view.row) {
    case ACT_ENGINE: if (view.action) view.action(view.action_context); break;
    case ACT_CHOOSE: choose(); break;
    case ACT_DOWNLOAD:
        if (SDL_OpenURL(M8_FIRMWARE_PAGE)) say("DOWNLOAD PAGE OPENED IN THE BROWSER", false, SECONDS(5));
        else say("CANNOT OPEN THE BROWSER", true, SECONDS(5));
        break;
    default:
        if (view.setup && view.ctx) view.ctx->app_state = QUIT;
        else close_view();
    }
}

static void move(int d) {
    int row = view.row;
    do row += d; while (row == ACT_ENGINE && !view.action);
    if (row >= 0 && row < ACT_COUNT) view.row = row;
}

/* ---- input ---- */

static int key_action(const config_params_s *c, const SDL_KeyboardEvent *k) {
    unsigned u = (unsigned)k->scancode;
    if (k->key == SDLK_O && (k->mod & (SDL_KMOD_GUI | SDL_KMOD_CTRL))) return A_CHOOSE;
    if (k->key == SDLK_ESCAPE || k->key == SDLK_F4) return A_CLOSE;
    if (k->key == SDLK_UP || u == c->key_up) return A_UP;
    if (k->key == SDLK_DOWN || u == c->key_down) return A_DOWN;
    if (k->key == SDLK_RETURN || k->key == SDLK_KP_ENTER || u == c->key_edit || u == c->key_edit_alt) return A_EDIT;
    if (u == c->key_opt || u == c->key_opt_alt) return A_OPTION;
    return A_NONE;
}
/* The D-pad, and SOUTH/EAST while EDIT/OPTION are unbound, keep the screen usable. */
static int button_action(const config_params_s *c, int b) {
    if (b == c->gamepad_edit || (c->gamepad_edit < 0 && b == SDL_GAMEPAD_BUTTON_SOUTH)) return A_EDIT;
    if (b == c->gamepad_opt || (c->gamepad_opt < 0 && b == SDL_GAMEPAD_BUTTON_EAST)) return A_OPTION;
    if (b == c->gamepad_up || b == SDL_GAMEPAD_BUTTON_DPAD_UP) return A_UP;
    if (b == c->gamepad_down || b == SDL_GAMEPAD_BUTTON_DPAD_DOWN) return A_DOWN;
    return A_NONE;
}

static void act(int action, bool down) {
    /* OPTION closes the overlay on release, as in the gamepad view. */
    if (action == A_OPTION) {
        if (down) { view.option = true; view.option_used = false; return; }
        if (view.option && !view.option_used && !view.setup) close_view();
        view.option = false;
        return;
    }
    if (!down) return;
    if (view.option) view.option_used = true;
    switch (action) {
    case A_UP: move(-1); break;
    case A_DOWN: move(1); break;
    case A_EDIT: activate(); break;
    case A_CHOOSE: choose(); break;
    case A_CLOSE: if (!view.setup) close_view(); break;
    default: break;
    }
}

static void open_view(struct app_context *ctx, bool setup) {
    view.ctx = ctx; view.setup = setup; view.open = true;
    view.installed = view.option = false;
    if (view.row == ACT_ENGINE && !view.action) view.row = ACT_CHOOSE;
    if (ctx) m8_handheld_release(ctx);
    renderer_request_redraw();
}

void m8_firmware_view_toggle(struct app_context *ctx) {
    if (view.open) { if (!view.setup) close_view(); return; }
    view.message_until = 0;
    open_view(ctx, false);
}

bool m8_firmware_view_event(struct app_context *ctx, const SDL_Event *e) {
    if (view.event_type && e->type == view.event_type) {
        view.dialog = false;
        if (e->user.code == 0 && e->user.data1) {
            if (!view.open) open_view(ctx, false);
            install(e->user.data1);
        } else if (e->user.code == 2) say("THE FILE DIALOG FAILED", true, SECONDS(5));
        SDL_free(e->user.data1);
        return true;
    }
    switch (e->type) {
    case SDL_EVENT_DROP_POSITION:
        view.hover_until = SDL_GetTicksNS() + SECONDS(0.25);
        renderer_request_redraw();
        return view.open;
    case SDL_EVENT_DROP_FILE:
        if (!e->drop.data || view.installed) return true;
        if (!view.open) { view.message_until = 0; open_view(ctx, false); }
        view.hover_until = 0;
        install(e->drop.data);
        return true;
    case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP:
        if (!view.open) {
            if (e->type == SDL_EVENT_KEY_DOWN && !e->key.repeat && e->key.key == SDLK_F4) { m8_firmware_view_toggle(ctx); return true; }
            return false;
        }
        view.ctx = ctx;
        if (!e->key.repeat) act(key_action(&ctx->conf, &e->key), e->key.down);
        renderer_request_redraw();
        return true;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: case SDL_EVENT_GAMEPAD_BUTTON_UP:
        if (!view.open) return false;
        act(button_action(&ctx->conf, e->gbutton.button), e->gbutton.down);
        renderer_request_redraw();
        return true;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        return view.open;
    default:
        return false;
    }
}

int m8_firmware_view_setup(struct app_context *ctx, const char *notice, const char *smoke) {
    open_view(ctx, true);
    copy(view.firmware, notice ? "FAILED ITS CHECK" : "NOT SET");
    view.message_until = 0;
    if (notice) say(notice, true, UINT64_MAX);
    uint64_t smoke_at = smoke ? SDL_GetTicksNS() + SECONDS(0.3) : 0;
    int result = 0;
    printf("firmware_picker=shown reason=%s destination=%s\n", notice ? "invalid" : "missing",
           view.destination ? view.destination : "<none>");
    fflush(stdout);
    while (ctx->app_state != QUIT) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_TERMINATING) ctx->app_state = QUIT;
            else if (event.type == SDL_EVENT_WINDOW_RESIZED || event.type == SDL_EVENT_WINDOW_MOVED)
                renderer_fix_texture_scaling_after_window_resize(&ctx->conf);
            else if (event.type == SDL_EVENT_GAMEPAD_ADDED || event.type == SDL_EVENT_GAMEPAD_REMOVED ||
                     event.type == SDL_EVENT_GAMEPAD_REMAPPED) m8_handheld_event(ctx, &event);
            else m8_firmware_view_event(ctx, &event);
        }
        uint64_t now = SDL_GetTicksNS();
        if (smoke_at && now >= smoke_at) {
            SDL_Event drop;
            SDL_zero(drop);
            drop.type = SDL_EVENT_DROP_FILE; drop.drop.data = smoke;
            SDL_PushEvent(&drop);
            smoke_at = 0;
        }
        if (view.installed && now - view.installed_ns >= SECONDS(0.7)) { result = 1; break; }
        renderer_request_redraw();
        render_screen(&ctx->conf);
        SDL_Delay(15);
    }
    view.installed = false;
    close_view();
    view.message_until = 0;
    return result;
}

/* ---- drawing ---- */

void m8_firmware_view_render(SDL_Renderer *renderer, int width, int height) {
    if (!view.open || !m8_ui_begin(renderer, &view.texture, width, height)) return;
    uint64_t now = SDL_GetTicksNS();
    m8_ui_fill(0, 0, width, height, M8_UI_BACKGROUND);
    m8_ui_text(1, ROW_TITLE, "FIRMWARE", M8_UI_TITLE);
    if (view.setup) {
        m8_ui_text(1, 4, "M8 HEADLESS FIRMWARE " M8_FIRMWARE_VERSION " IS NEEDED", M8_UI_VALUE);
        m8_ui_text(1, 5, "IT IS NOT INCLUDED. GET IT FREE FROM", M8_UI_INFO);
        m8_ui_text(1, 6, "DIRTYWAVE AND SELECT THE .HEX FILE.", M8_UI_INFO);
    } else {
        m8_ui_text(1, 4, "M8 HEADLESS FIRMWARE " M8_FIRMWARE_VERSION, M8_UI_VALUE);
        m8_ui_text(1, 5, "A NEW FILE IS USED AFTER A RESTART.", M8_UI_INFO);
    }
    bool ok = !SDL_strncmp(view.firmware, M8_FIRMWARE_VERSION, sizeof M8_FIRMWARE_VERSION - 1);
    m8_ui_text(1, ROW_FIRMWARE, "FIRMWARE", M8_UI_TEXT);
    m8_ui_text(VALUE_COL, ROW_FIRMWARE, view.firmware,
               ok ? M8_UI_VALUE : !SDL_strcmp(view.firmware, "NOT SET") ? M8_UI_INFO : M8_UI_TITLE);
    m8_ui_text(1, ROW_ENGINE, "ENGINE", M8_UI_TEXT);
    m8_ui_text(VALUE_COL, ROW_ENGINE, view.engine, M8_UI_VALUE);
    /* Engine builder hook: absent until a builder registers its action. */
    if (view.action) m8_ui_field(VALUE_COL, ROW_HOOK, view.hook, view.row == ACT_ENGINE, M8_UI_VALUE);
    static const char *labels[ACT_COUNT] = {NULL, "CHOOSE FILE...", "OPEN DOWNLOAD PAGE", "QUIT"};
    for (int i = ACT_CHOOSE; i < ACT_COUNT; ++i) {
        const char *label = i == ACT_CLOSE && !view.setup ? "CLOSE" : labels[i];
        m8_ui_field(1, ROW_ACTIONS + i - ACT_CHOOSE, label, view.row == i, M8_UI_VALUE);
    }
    bool hover = view.hover_until > now;
    m8_ui_text(1, ROW_DROP, "OR DROP M8_V6_5_2C_HEADLESS.HEX HERE", hover ? M8_UI_SELECTED : M8_UI_INFO);
    if (hover) m8_ui_text(1, MESSAGE_ROW, "RELEASE TO USE THIS FILE", M8_UI_VALUE);
    else if (view.dialog) m8_ui_text(1, MESSAGE_ROW, "CHOOSE THE FILE IN THE OPEN PANEL", M8_UI_INFO);
    else if (view.message_until > now)
        m8_ui_text(1, MESSAGE_ROW, view.message, view.message_error ? M8_UI_TITLE : view.installed ? M8_UI_PLAY : M8_UI_VALUE);
    m8_ui_text(1, HINT_ROW, view.setup ? "EDIT SELECT  " CHOOSE_KEY " CHOOSE FILE" : "EDIT SELECT  OPTION CLOSE", M8_UI_INFO);
    m8_ui_end();
    renderer_request_redraw();
}
