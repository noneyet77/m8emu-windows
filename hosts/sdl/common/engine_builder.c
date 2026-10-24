#include "engine_builder.h"
#include "engine_select.h"
#include "user_paths.h"
#include <SDL3/SDL.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef __APPLE__
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

static struct {
#ifdef __APPLE__
    pid_t pid;
#endif
    m8_engine_state state;
    int percent;
    char text[96], sha[65], dir[4096], log[4096];
} build;

static void say(char *out, size_t capacity, const char *text) { if (out && capacity) SDL_strlcpy(out, text, capacity); }
const char *m8_engine_builder_log(void) { return build.log; }

#ifdef __APPLE__
extern char **environ;

static int read_text(const char *path, char *out, size_t capacity)
{
    FILE *file = fopen(path, "rb");
    if (!file) return -1;
    size_t n = fread(out, 1, capacity - 1, file);
    out[n] = 0;
    fclose(file);
    return 0;
}

static int source_dir(char *out, size_t capacity)
{
    const char *base = SDL_GetBasePath();
    char path[4200];
    struct stat info;
    if (!base || snprintf(out, capacity, "%sengine-src", base) >= (int)capacity) return -1;
    snprintf(path, sizeof path, "%s/engine_build.py", out);
    return stat(path, &info) ? -1 : 0;
}

/* group: lead a new process group, so cancel reaches the compilers too. */
static int spawn(char *const argv[], int out, int group, pid_t *pid)
{
    posix_spawnattr_t attr;
    posix_spawn_file_actions_t actions;
    sigset_t mask, defaults;
    sigemptyset(&mask);
    sigemptyset(&defaults);
    sigaddset(&defaults, SIGINT); sigaddset(&defaults, SIGTERM);
    sigaddset(&defaults, SIGPIPE); sigaddset(&defaults, SIGHUP);
    posix_spawnattr_init(&attr);
    posix_spawn_file_actions_init(&actions);
    /* Close the app's descriptors (card, audio) in the child. */
    posix_spawnattr_setflags(&attr, (short)(POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF |
        POSIX_SPAWN_CLOEXEC_DEFAULT | (group ? POSIX_SPAWN_SETPGROUP : 0)));
    posix_spawnattr_setsigmask(&attr, &mask);
    posix_spawnattr_setsigdefault(&attr, &defaults);
    posix_spawnattr_setpgroup(&attr, 0);
    posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
    if (out >= 0) {
        posix_spawn_file_actions_adddup2(&actions, out, 1);
        posix_spawn_file_actions_adddup2(&actions, out, 2);
    } else {
        posix_spawn_file_actions_addopen(&actions, 1, "/dev/null", O_WRONLY, 0);
        posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", O_WRONLY, 0);
    }
    int status = posix_spawn(pid, argv[0], &actions, &attr, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attr);
    return status;
}

/* Runs a short tool to completion; the first stdout line goes to out. */
static int capture(char *const argv[], char *out, size_t capacity)
{
    int pipes[2];
    pid_t pid;
    if (pipe(pipes)) return -1;
    if (spawn(argv, pipes[1], 0, &pid)) { close(pipes[0]); close(pipes[1]); return -1; }
    close(pipes[1]);
    size_t used = 0;
    char discard[256];
    for (ssize_t n; (n = read(pipes[0], used < capacity - 1 ? out + used : discard,
                               used < capacity - 1 ? capacity - 1 - used : sizeof discard)) > 0;)
        if (used < capacity - 1) used += (size_t)n;
    out[used] = 0;
    out[strcspn(out, "\r\n")] = 0;
    close(pipes[0]);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

int m8_engine_builder_available(int *tools_missing, char *reason, size_t capacity)
{
    char dir[4096], python[4200];
    struct stat info;
    if (tools_missing) *tools_missing = 0;
    if (source_dir(dir, sizeof dir)) { say(reason, capacity, "BUILDER NOT IN THIS APP"); return -1; }
    /* xcode-select -p answers without the install prompt that the
     * /usr/bin/clang and /usr/bin/python3 shims raise when tools are absent. */
    char *argv[] = {"/usr/bin/xcode-select", "-p", NULL};
    if (capture(argv, dir, sizeof dir) || !*dir) {
        if (tools_missing) *tools_missing = 1;
        say(reason, capacity, "COMMAND LINE TOOLS MISSING");
        return -1;
    }
    snprintf(python, sizeof python, "%s/usr/bin/python3", dir);
    if (stat(python, &info)) {
        if (tools_missing) *tools_missing = 1;
        say(reason, capacity, "DEVELOPER TOOLS LACK PYTHON3");
        return -1;
    }
    say(reason, capacity, "");
    return 0;
}

int m8_engine_builder_install_tools(char *error, size_t capacity)
{
    char output[256];
    char *argv[] = {"/usr/bin/xcode-select", "--install", NULL};
    if (capture(argv, output, sizeof output) < 0) { say(error, capacity, "CANNOT START INSTALLER"); return -1; }
    say(error, capacity, "");
    return 0;
}

int m8_engine_builder_start(const char *firmware, char *error, size_t capacity)
{
    char source[4096], script[4200], status[4200], jobs[16];
    int tools;
    if (build.pid > 0) { say(error, capacity, "A BUILD IS RUNNING"); return -1; }
    if (m8_engine_builder_available(&tools, error, capacity) || source_dir(source, sizeof source)) return -1;
    if (!firmware || m8_engine_file_sha256(firmware, build.sha)) { say(error, capacity, "FIRMWARE UNREADABLE"); return -1; }
    char *dir = m8_user_engine_dir(build.sha), *marker = dir ? m8_user_join(dir, "build.log") : NULL;
    int made = marker && !m8_user_make_parents(marker) && SDL_strlcpy(build.dir, dir, sizeof build.dir) < sizeof build.dir;
    free(marker); free(dir);
    if (!made) { say(error, capacity, "CANNOT CREATE ENGINE FOLDER"); return -1; }
    snprintf(script, sizeof script, "%s/engine_build.py", source);
    snprintf(build.log, sizeof build.log, "%s/build.log", build.dir);
    snprintf(status, sizeof status, "%s/build.status", build.dir);
    unlink(status);
    int log = open(build.log, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (log < 0) { say(error, capacity, "CANNOT CREATE BUILD LOG"); return -1; }
    const char *requested = getenv("M8_ENGINE_JOBS");
    snprintf(jobs, sizeof jobs, "%d", requested && atoi(requested) > 0 ? atoi(requested) : 0);
    char *argv[] = {"/usr/bin/python3", "-B", "-I", script, "--source", source, "--firmware", (char *)firmware,
                    "--output", build.dir, jobs[0] != '0' ? "--jobs" : NULL, jobs, NULL};
    int failed = spawn(argv, log, 1, &build.pid);
    close(log);
    if (failed) { build.pid = 0; say(error, capacity, "CANNOT START PYTHON3"); return -1; }
    build.state = M8_ENGINE_RUNNING; build.percent = 0;
    SDL_strlcpy(build.text, "STARTING", sizeof build.text);
    say(error, capacity, "");
    return 0;
}

/* Returns 1 when the driver reported its own failure. */
static int read_status(void)
{
    char path[4200], text[256], state[16];
    int percent;
    snprintf(path, sizeof path, "%s/build.status", build.dir);
    if (read_text(path, text, sizeof text) || sscanf(text, "%d %15s", &percent, state) != 2) return 0;
    const char *message = strchr(text, ' ');
    message = message ? strchr(message + 1, ' ') : NULL;
    build.percent = percent < 0 ? 0 : percent > 100 ? 100 : percent;
    if (message) { SDL_strlcpy(build.text, message + 1, sizeof build.text); build.text[strcspn(build.text, "\r\n")] = 0; }
    return !strcmp(state, "failed");
}

m8_engine_state m8_engine_builder_poll(int *percent, char *text, size_t capacity)
{
    if (build.pid > 0) {
        int status = 0;
        pid_t done = waitpid(build.pid, &status, WNOHANG);
        int reported = read_status();
        if (done == build.pid) {
            build.pid = 0;
            char reason[64];
            if (!WIFEXITED(status) || WEXITSTATUS(status)) {
                build.state = M8_ENGINE_FAILED;
                if (!reported) snprintf(build.text, sizeof build.text, "BUILDER STOPPED: %s %d",
                    WIFEXITED(status) ? "EXIT" : "SIGNAL", WIFEXITED(status) ? WEXITSTATUS(status) : WTERMSIG(status));
            } else if (m8_engine_user_check(build.dir, build.sha, reason, sizeof reason)) {
                /* Report done only for what the next launch accepts. */
                build.state = M8_ENGINE_FAILED;
                snprintf(build.text, sizeof build.text, "REJECTED: %s", reason);
            } else build.state = M8_ENGINE_DONE;
        }
    }
    if (percent) *percent = build.percent;
    say(text, capacity, build.text);
    return build.state;
}

void m8_engine_builder_cancel(void)
{
    if (build.pid <= 0) return;
    /* The driver removes its staging files on SIGTERM; SIGKILL is the backstop. */
    kill(-build.pid, SIGTERM);
    int status = 0;
    for (int i = 0; i < 500 && waitpid(build.pid, &status, WNOHANG) == 0; ++i) {
        if (i == 400) kill(-build.pid, SIGKILL);
        usleep(10000);
    }
    if (waitpid(build.pid, &status, WNOHANG) == 0) { kill(-build.pid, SIGKILL); waitpid(build.pid, &status, 0); }
    kill(-build.pid, SIGKILL);
    build.pid = 0;
    build.state = M8_ENGINE_CANCELLED;
    SDL_strlcpy(build.text, "CANCELLED", sizeof build.text);
}
#else
int m8_engine_builder_available(int *tools_missing, char *reason, size_t capacity)
{
    if (tools_missing) *tools_missing = 0;
    say(reason, capacity, "MACOS ONLY");
    return -1;
}
int m8_engine_builder_install_tools(char *error, size_t capacity) { say(error, capacity, "MACOS ONLY"); return -1; }
int m8_engine_builder_start(const char *firmware, char *error, size_t capacity)
{
    (void)firmware;
    say(error, capacity, "MACOS ONLY");
    return -1;
}
m8_engine_state m8_engine_builder_poll(int *percent, char *text, size_t capacity)
{
    if (percent) *percent = 0;
    say(text, capacity, build.text);
    return M8_ENGINE_IDLE;
}
void m8_engine_builder_cancel(void) {}
#endif

int m8_engine_builder_run(const char *firmware)
{
    char text[96], error[96], last[96] = "";
    int percent = 0, tools = 0;
    if (m8_engine_builder_available(&tools, error, sizeof error) ||
        m8_engine_builder_start(firmware, error, sizeof error)) {
        printf("engine_build=failed reason=\"%s\"%s\n", error, tools ? " fix=\"xcode-select --install\"" : "");
        return 1;
    }
    printf("engine_build=started dir=%s log=%s\n", build.dir, build.log);
    m8_engine_state state;
    while ((state = m8_engine_builder_poll(&percent, text, sizeof text)) == M8_ENGINE_RUNNING) {
        if (strcmp(text, last)) { printf("engine_build=running percent=%d status=\"%s\"\n", percent, text); fflush(stdout); }
        SDL_strlcpy(last, text, sizeof last);
        SDL_Delay(200);
    }
    printf("engine_build=%s percent=%d status=\"%s\"\n", state == M8_ENGINE_DONE ? "done" : "failed", percent, text);
    return state == M8_ENGINE_DONE ? 0 : 1;
}
