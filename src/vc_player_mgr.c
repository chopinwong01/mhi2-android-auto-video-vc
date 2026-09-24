#include "vc_player_mgr.h"
#include "focus_ctl.h"
#include "gal_hook.h"
#include "vc_stream_out.h"

#include <errno.h>
#include <fcntl.h>
#ifdef __QNX__
#include <process.h>
#include <spawn.h>
#endif
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define PLAYER_LOG_PATH      "/tmp/stream-player.log"
#define PLAYER_LOG_MAX_BYTES (1024L * 1024L)
#define DMDT_PATH            "/eso/bin/apps/dmdt"
#define DMDT_LIBRARY_PATH    "LD_LIBRARY_PATH=/eso/lib:/lib:/usr/lib"
#define DMDT_CONFIG_DIR      "IPL_CONFIG_DIR=/etc/eso/production"
/* Same list the player uses: dmdt output to a pipe is lost without it. */
#define DMDT_FLUSH_PRELOAD   "LD_PRELOAD=/eso/lib/gal_dualscreen/libdmdt_flush.so:/mnt/app/eso/lib/gal_dualscreen/libdmdt_flush.so:/mnt/app/eso/lib/libdmdt_flush.so:/fs/sdb0/lib/libdmdt_flush.so:/fs/sda0/lib/libdmdt_flush.so"

static pid_t g_player_pid = -1;
/* Bumped on every spawn, so a queued display restore can tell that the
 * player it was restoring for has already been replaced. */
static volatile unsigned long g_player_generation = 0ul;
static int g_orphans_checked = 0;
/* One restore at a time: two of them racing would each spawn dmdt and
 * fight over the same display context. */
static pthread_mutex_t g_restore_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile int g_restore_busy = 0;

static void block_all_signals(void);

/*
 * A regular file, and nothing else.
 *
 * The execute bit is not usable as the test here: FAT32 does not carry one,
 * so a player on the SD card would be rejected -- which is why the check was
 * relaxed in the first place. But it was relaxed to
 * `(st_mode & S_IXUSR) || S_ISREG(st_mode)`, and a DIRECTORY named
 * stream-player satisfies the first half, because directories normally do
 * carry the user-execute bit. Such a path then wins the search and spawnv
 * fails on it, while a real binary further down the candidate list is never
 * reached. S_ISREG alone covers both filesystems; if the file turns out not
 * to be executable, spawnv reports that plainly.
 */
static int is_runnable(const struct stat *st)
{
    return S_ISREG(st->st_mode) != 0;
}

static const char *find_player_binary(void)
{
    int i;
    const char *custom = getenv("GAL_PLAYER_PATH");
    if (custom != NULL && *custom != '\0') {
        struct stat st;
        if (stat(custom, &st) == 0 && is_runnable(&st))
            return custom;
    }

    /* Check persistent /navigation SSD first, then SD card mounts and current dir */
    static const char *candidates[] = {
        "/mnt/app/eso/bin/stream-player",
        "/mnt/app/navigation/stream-player",
        "/navigation/stream-player",
        "/fs/sdb0/stream-player",
        "/fs/sda0/stream-player",
        "./stream-player",
        "/tmp/stream-player",
        NULL
    };

    for (i = 0; candidates[i] != NULL; ++i) {
        struct stat st;
        if (stat(candidates[i], &st) == 0 && is_runnable(&st))
            return candidates[i];
    }
    return NULL;
}

static int is_auto_player_enabled(void)
{
    const char *v = getenv("GAL_AUTORUN_PLAYER");
    if (v != NULL && (*v == '0' || strcmp(v, "false") == 0 || strcmp(v, "no") == 0))
        return 0;
    return 1;
}

static long long mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + (long long)(ts.tv_nsec / 1000000L);
}

/*
 * spawn() with an explicit environment and exactly three descriptors:
 * stdin on /dev/null, stdout/stderr as given (or /dev/null). Nothing else of
 * gal's reaches the child -- not its USB and socket descriptors, not
 * LD_PRELOAD -- and the child starts with no signals blocked, whatever the
 * calling thread blocks.
 */
static pid_t spawn_clean(const char *path, char *argv[], char *envp[], int out_fd, int err_fd)
{
    int null_fd = open("/dev/null", O_RDWR);
    int map[3];
    pid_t pid;

    map[0] = null_fd;
    map[1] = out_fd >= 0 ? out_fd : null_fd;
    map[2] = err_fd >= 0 ? err_fd : null_fd;
    if (null_fd < 0) {
        /* Substitute any descriptor we do have: -1 in QNX's fd_map means
         * CLOSED, not "skip", and a child whose fd 0 is closed puts its first
         * socket there. Inheriting everything is the last resort, because the
         * child would then also hold our pipe ends open. */
        int sub = out_fd >= 0 ? out_fd : err_fd;
        if (sub >= 0) {
            map[0] = sub;
            map[1] = out_fd >= 0 ? out_fd : sub;
            map[2] = err_fd >= 0 ? err_fd : sub;
        }
    }
#ifdef __QNX__
    {
        struct inheritance inherit;
        memset(&inherit, 0, sizeof inherit);
        inherit.flags = SPAWN_SETSIGMASK;
        sigemptyset(&inherit.sigmask);
        /*
         * QNX reads -1 in fd_map as SPAWN_FDCLOSED, so if /dev/null could not
         * be opened the child would start with 0, 1 and 2 CLOSED -- its first
         * socket would then land on fd 0 and any library writing to stdout
         * would write into it. Fall back to inheriting instead.
         */
        if (map[0] < 0 || map[1] < 0 || map[2] < 0) {
            gal_hook_logf("event=player.spawn note=no_dev_null errno=%d action=inherit_fds", errno);
            pid = spawn(path, 0, NULL, &inherit, argv, envp);
        } else {
            pid = spawn(path, 3, map, &inherit, argv, envp);
        }
    }
#else
    pid = fork();
    if (pid == 0) {
        sigset_t none;
        int i;
        for (i = 0; i < 3; ++i)
            if (map[i] >= 0) (void)dup2(map[i], i);
        sigemptyset(&none);
        (void)sigprocmask(SIG_SETMASK, &none, NULL);
        execve(path, argv, envp);
        _exit(127);
    }
#endif
    if (null_fd >= 0) close(null_fd);
    return pid;
}

/* 1 once the child is gone (reaped, or not our child and no longer alive). */
static int wait_child(pid_t pid, int timeout_ms, int *status)
{
    int waited = 0;
    for (;;) {
        pid_t r = waitpid(pid, status, WNOHANG);
        if (r == pid) return 1;
        if (r < 0 && errno != EINTR && (errno != ECHILD || kill(pid, 0) != 0)) {
            *status = -1;
            return 1;
        }
        if (waited >= timeout_ms) return 0;
        usleep(50000);
        waited += 50;
    }
}

/* Exit status, or -1 when it could not run or was killed on timeout. */
static int run_helper(const char *path, char *argv[], char *envp[], int timeout_ms)
{
    int status = -1;
    pid_t pid = spawn_clean(path, argv, envp, -1, -1);
    if (pid < 0) return -1;
    if (!wait_child(pid, timeout_ms, &status)) {
        kill(pid, SIGKILL);
        (void)wait_child(pid, 500, &status);
        return -1;
    }
    return status >= 0 && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* fix=orphan_restore: put the stock Kombi map back, with a clean environment. */
static void restore_display_sync(const char *reason)
{
    const gal_secondary_config *cfg = gal_hook_config();
    char context[16];
    char displayable[16];
    char display[16];
    char *envp[] = { (char *)DMDT_LIBRARY_PATH, (char *)DMDT_CONFIG_DIR, NULL };
    char *dc[] = { (char *)"dmdt", (char *)"dc", context, displayable, NULL };
    char *sc[] = { (char *)"dmdt", (char *)"sc", display, context, NULL };
    int rc_dc;
    int rc_sc;

    (void)snprintf(context, sizeof context, "%u", cfg->context_id);
    (void)snprintf(displayable, sizeof displayable, "%u", cfg->restore_displayable_id);
    (void)snprintf(display, sizeof display, "%u", cfg->vc_display);
    rc_dc = run_helper(DMDT_PATH, dc, envp, 1500);
    rc_sc = run_helper(DMDT_PATH, sc, envp, 1500);
    gal_hook_logf("event=player.display_restore reason=%s dc=%d sc=%d fix=orphan_restore",
                  reason, rc_dc, rc_sc);
}

typedef struct restore_request {
    unsigned long generation;
    char reason[32];
} restore_request;

static void *restore_thread(void *arg)
{
    restore_request *req = (restore_request *)arg;

    block_all_signals();
    pthread_mutex_lock(&g_restore_lock);
    /* Re-checked here, not at dispatch: a player may have been spawned while
     * this request waited, and then the display is already its business. */
    if (req->generation == g_player_generation)
        restore_display_sync(req->reason);
    else
        gal_hook_logf("event=player.display_restore reason=%s result=skipped cause=player_replaced fix=orphan_restore",
                      req->reason);
    g_restore_busy = 0;
    pthread_mutex_unlock(&g_restore_lock);
    free(req);
    return NULL;
}

/*
 * Two dmdt spawns take up to 3 s. The focus controller calls this from the
 * player's death path and is also the only thread that reads render ACKs,
 * accepts sockets and issues focus writes, so it must not wait here. Only
 * for a player that could not restore the map itself: killed, or crashed.
 */
static void restore_display_async(const char *reason)
{
    pthread_attr_t attr;
    pthread_t thread;
    restore_request *req;

    if (g_restore_busy) {
        gal_hook_logf("event=player.display_restore reason=%s result=skipped cause=already_running fix=orphan_restore",
                      reason);
        return;
    }
    g_restore_busy = 1;
    req = (restore_request *)malloc(sizeof *req);
    if (req == NULL) {
        g_restore_busy = 0;
        gal_hook_logf("event=player.display_restore reason=%s result=skipped cause=no_memory fix=orphan_restore",
                      reason);
        return;
    }
    req->generation = g_player_generation;
    (void)snprintf(req->reason, sizeof req->reason, "%s", reason);
    (void)pthread_attr_init(&attr);
    (void)pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&thread, &attr, restore_thread, req) != 0) {
        g_restore_busy = 0;
        free(req);
        gal_hook_logf("event=player.display_restore reason=%s result=skipped cause=no_thread fix=orphan_restore",
                      reason);
    }
    (void)pthread_attr_destroy(&attr);
}

/*
 * fix=orphan_restore. slay exits with the number of processes it signalled;
 * a player killed with -9 cannot restore the display itself, so do it here.
 */
static void kill_orphan_players(void)
{
    static const char *const paths[] = { "/proc/boot/slay", "/bin/slay", "/usr/bin/slay", NULL };
    char *argv[] = { (char *)"slay", (char *)"-9", (char *)"-f", (char *)"-q",
                     (char *)"stream-player", NULL };
    char *envp[] = { (char *)"PATH=/proc/boot:/bin:/usr/bin:/sbin:/usr/sbin", NULL };
    const char *path = NULL;
    struct stat st;
    int i;
    int killed;

    for (i = 0; paths[i] != NULL; ++i)
        if (stat(paths[i], &st) == 0) { path = paths[i]; break; }
    if (path == NULL) {
        gal_hook_log("event=player.orphan_check result=skipped reason=no_slay fix=orphan_restore");
        return;
    }
    killed = run_helper(path, argv, envp, 1000);
    if (killed != 0)
        gal_hook_logf("event=player.orphan_check killed=%d fix=orphan_restore", killed);
    if (killed > 0) restore_display_sync("orphan_killed");
}

static int open_player_log(void)
{
    struct stat st;
    int flags = O_WRONLY | O_CREAT | O_APPEND;
    int fd;
    char header[96];
    int n;

    if (stat(PLAYER_LOG_PATH, &st) == 0 && st.st_size > PLAYER_LOG_MAX_BYTES)
        flags |= O_TRUNC;
    fd = open(PLAYER_LOG_PATH, flags, 0666);
    if (fd < 0) return -1;
    n = snprintf(header, sizeof header, "==== stream-player spawn by gal pid=%ld t_ms=%lld ====\n",
                 (long)getpid(), mono_ms());
    if (n > 0) (void)write(fd, header, (size_t)n);
    return fd;
}

void vc_player_start(void)
{
    const char *bin;
    pid_t pid;
    char *argv[3];
    char url[64];
    int use_spawn = hook_fix_enabled(HOOK_FIX_PLAYER_LOG);

    if (!is_auto_player_enabled()) return;
    if (g_player_pid > 0) {
        int status;
        if (waitpid(g_player_pid, &status, WNOHANG) == 0) {
            return; /* already running */
        }
        g_player_pid = -1;
    }

#ifdef __QNX__
    /* Terminate any orphan or lingering stream-player before spawning fresh instance */
    if (hook_fix_enabled(HOOK_FIX_ORPHAN_RESTORE)) {
        /*
         * Once per process. An orphan can only be left by a previous gal;
         * every later spawn follows our own vc_player_stop. Running it on
         * each respawn cost the controller -- the only ACK pump -- seconds of
         * slay and dmdt waits, and on gal's own thread it risked PING_TIMEOUT.
         */
        if (!g_orphans_checked) {
            g_orphans_checked = 1;
            kill_orphan_players();
        }
    } else
        (void)system("slay -9 -f -q stream-player 2>/dev/null");
#endif
    bin = find_player_binary();
    if (bin == NULL) {
        gal_hook_logf("event=player.spawn result=skipped reason=binary_not_found");
        return;
    }

    if (!use_spawn && !fc_enabled()) {
        /*
         * Only on gal's own thread. setenv/unsetenv rewrite the environ array
         * in place, and gal's threads call getenv while logging; doing this
         * from the focus controller would fault them. The envp below is
         * explicit anyway, so this only matters for the legacy path.
         */
        /* Ensure IPL_CONFIG_DIR and LD_LIBRARY_PATH are set for player graphics initialization */
        setenv("IPL_CONFIG_DIR", "/etc/eso/production", 0);
        setenv("LD_LIBRARY_PATH", "/proc/boot:/lib:/lib/dll:/usr/lib:/mnt/app/root/lib-target:/mnt/app/usr/lib:/mnt/app/armle/lib:/mnt/app/armle/lib/dll:/mnt/app/armle/usr/lib:/eso/lib:/mnt/app/eso/lib:/fs/sdb0/lib", 1);
        unsetenv("LD_PRELOAD");
    }

    /*
     * Built from vc_stream_out's own port rather than hardcoded. GAL_STREAM_PORT
     * is a documented knob in gal_dualscreen.conf, and with a literal 12346 here
     * setting it made the hook listen on one port while the player it spawned
     * dialled another -- a black cluster whose only trace is a connect failure
     * in the player's output, not in this log.
     */
    (void)snprintf(url, sizeof(url), "%s", vc_stream_out_url());
    argv[0] = (char *)"stream-player";
    argv[1] = url;
    argv[2] = NULL;

    {
        char *envp[] = {
            (char *)"IPL_CONFIG_DIR=/etc/eso/production",
            (char *)"LD_LIBRARY_PATH=/proc/boot:/lib:/lib/dll:/usr/lib:/mnt/app/root/lib-target:/mnt/app/usr/lib:/mnt/app/armle/lib:/mnt/app/armle/lib/dll:/mnt/app/armle/usr/lib:/eso/lib:/mnt/app/eso/lib:/fs/sdb0/lib",
            (char *)"PATH=/proc/boot:/bin:/usr/bin:/usr/sbin:/sbin:/eso/bin:/mnt/app/eso/bin:/fs/sdb0/bin:/mnt/app/navigation",
            NULL
        };
        int log_fd = use_spawn ? open_player_log() : -1;
        pid = spawn_clean(bin, argv, envp, log_fd, log_fd);
        if (log_fd >= 0) close(log_fd);
    }

    if (pid < 0) {
        gal_hook_logf("event=player.spawn result=failed path=%s errno=%d method=spawn",
                      bin, errno);
        g_player_pid = -1;
    } else {
        g_player_pid = pid;
        ++g_player_generation;
        gal_hook_logf("event=player.spawn result=success path=%s pid=%ld url=%s method=spawn%s",
                      bin, (long)pid, url,
                      use_spawn ? " log=" PLAYER_LOG_PATH " fix=player_log" : "");
    }
}

void vc_player_stop(void)
{
    vc_player_stop_wait(300);
}

/*
 * SIGTERM, then SIGKILL if the player is still there after grace_ms; returns
 * as soon as it exits. On SIGTERM the player puts the Kombi map back itself,
 * which is two dmdt runs -- 0.57-1.93 s when the hook ran the same pair on the
 * car. A 300 ms grace cut all 7 stops on 2026-09-16 off in the middle of it:
 * SIGKILL, then the same two dmdt runs again from here. vc_player_stop keeps
 * 300 ms for gal's reader thread, which also answers the phone's pings.
 */
void vc_player_stop_wait(int grace_ms)
{
    if (g_player_pid > 0) {
        int status;
        int i;
        int use_wait = hook_fix_enabled(HOOK_FIX_PLAYER_LOG);
        long long t0 = mono_ms();
        gal_hook_logf("event=player.stop pid=%ld action=SIGTERM grace_ms=%d", (long)g_player_pid, grace_ms);
        kill(g_player_pid, SIGTERM);
        if (use_wait) {
            if (wait_child(g_player_pid, grace_ms, &status)) {
                gal_hook_logf("event=player.stop pid=%ld result=clean_exit elapsed_ms=%lld",
                              (long)g_player_pid, mono_ms() - t0);
                g_player_pid = -1;
                return;
            }
        } else {
            for (i = 0; i < (grace_ms + 49) / 50; ++i) {
                if (waitpid(g_player_pid, &status, WNOHANG) != 0) {
                    gal_hook_logf("event=player.stop pid=%ld result=clean_exit elapsed_ms=%d",
                                  (long)g_player_pid, (i + 1) * 50);
                    g_player_pid = -1;
                    return;
                }
                usleep(50000);
            }
        }
        gal_hook_logf("event=player.stop pid=%ld action=SIGKILL elapsed_ms=%lld",
                      (long)g_player_pid, mono_ms() - t0);
        kill(g_player_pid, SIGKILL);
        if (use_wait) {
            if (!wait_child(g_player_pid, 100, &status))
                gal_hook_logf("event=player.stop result=unreaped pid=%ld", (long)g_player_pid);
        } else {
            for (i = 0; i < 2; ++i) {
                if (waitpid(g_player_pid, &status, WNOHANG) != 0) break;
                usleep(50000);
            }
            if (i == 2) {
                gal_hook_logf("event=player.stop result=unreaped pid=%ld",
                              (long)g_player_pid);
            }
        }
        g_player_pid = -1;
        /* Emergency DMDT restore if player had to be forcefully terminated */
#ifdef __QNX__
        if (hook_fix_enabled(HOOK_FIX_ORPHAN_RESTORE))
            restore_display_async("player_sigkill");
        else
            (void)system("LD_LIBRARY_PATH=/eso/lib:/lib:/usr/lib IPL_CONFIG_DIR=/etc/eso/production /eso/bin/apps/dmdt dc 70 33 2>/dev/null; "
                         "LD_LIBRARY_PATH=/eso/lib:/lib:/usr/lib IPL_CONFIG_DIR=/etc/eso/production /eso/bin/apps/dmdt sc 4 70 2>/dev/null");
#endif
    }
}

void vc_player_restart(void)
{
    vc_player_stop();
    usleep(50000); /* 50ms pause before respawn */
    vc_player_start();
}

int vc_player_is_running(void)
{
    int status;
    pid_t ret;
    if (g_player_pid <= 0) return 0;
    ret = waitpid(g_player_pid, &status, WNOHANG);
    if (ret == 0) return 1; /* Process is still active */

    /* Process has terminated or no longer exists; clean up state */
    g_player_pid = -1;
    return 0;
}

int vc_player_poll(void)
{
    int status = 0;
    pid_t r;

    if (g_player_pid <= 0) return 0;
    r = waitpid(g_player_pid, &status, WNOHANG);
    if (r == 0) return 1;
    if (r < 0 && (errno == EINTR || (errno == ECHILD && kill(g_player_pid, 0) == 0)))
        return 1;
    {
        /* Only a zero exit is known to have run the player's own restore. */
        int clean = r == g_player_pid && WIFEXITED(status) && WEXITSTATUS(status) == 0;
        if (r == g_player_pid)
            gal_hook_logf("event=player.exit pid=%ld exit_code=%d signal=%d",
                          (long)g_player_pid,
                          WIFEXITED(status) ? WEXITSTATUS(status) : -1,
                          WIFSIGNALED(status) ? WTERMSIG(status) : 0);
        else
            gal_hook_logf("event=player.exit pid=%ld reason=not_waitable errno=%d",
                          (long)g_player_pid, errno);
        g_player_pid = -1;
#ifdef __QNX__
        /* fix=orphan_restore: a crashed or -9 killed player leaves the
         * Kombi context on its closed window ("starting map view"). */
        if (!clean && hook_fix_enabled(HOOK_FIX_ORPHAN_RESTORE))
            restore_display_async("player_died");
#else
        (void)clean;
#endif
    }
    return 0;
}

void vc_player_supervisor_tick(long frame_silence_ms, long ack_silence_ms)
{
    static long s_cooldown_ticks = 0;
    /* Only supervise if a player process is currently alive */
    if (!vc_player_is_running()) {
        s_cooldown_ticks = 0;
        return;
    }

    if (s_cooldown_ticks > 0) {
        s_cooldown_ticks--;
        return;
    }

    /*
     * Condition for hang detection:
     * 1. Frames are actively arriving from the phone (frame_silence_ms <= 2000).
     * 2. The player has not produced any render ACK for >= 15000ms (15 seconds).
     */
    if (frame_silence_ms >= 0 && frame_silence_ms <= 2000) {
        if (ack_silence_ms >= 15000) {
            gal_hook_logf("event=player.supervisor action=kill_hung_player ack_silence_ms=%ld pid=%ld",
                          ack_silence_ms, (long)g_player_pid);
            s_cooldown_ticks = 20; /* 20 * 500ms = 10s anti-flapping cooldown */
            vc_player_restart();
        }
    }
}

/* ---------------- Kombi map readiness ---------------- */

/*
 * GAL_FOCUS_WAIT_KOMBI holds the first grant until the Kombi map is up. Only
 * what is actually observed counts; nothing is inferred from a clock.
 *
 *  marker  /tmp/gal_kombi_ready, written the first time readiness is seen.
 *          /tmp is RAM, empty after every reboot, so a gal restarted later in
 *          the same boot starts ready without slog or dmdt.
 *  dmdt    with no marker -- the first gal of a boot -- one `dmdt gs`: ready
 *          if context 70 already shows 33 (or our 3). This answers a phone
 *          plugged in long after boot, when the slog line may no longer be in
 *          slogger's buffer. Early in boot it reads "not ready".
 *  slog    then wait for the display manager's "window 33 performed 1st
 *          swap". On the car a read returned slogger's buffered events at
 *          once (every gal restart found the line within 3 ms), and the boot
 *          line in the same millisecond it was logged (39.800 s).
 *
 * When none of them can answer -- dmdt gave nothing and slog is unreadable --
 * the cluster stays native, and the log says so. GAL_FOCUS_WAIT_KOMBI=0
 * skips the wait.
 */
#define KOMBI_READY_MARKER "/tmp/gal_kombi_ready"
#define KOMBI_BOOT_PROBE_MIN_UPTIME_MS 90000LL

static volatile int g_kombi_ready;
static int g_kombi_watch_started;

static void kombi_mark_ready(const char *source, const char *detail)
{
    if (g_kombi_ready) return;
    g_kombi_ready = 1;
    gal_hook_logf("event=kombi.ready source=%s detail=\"%s\" uptime_ms=%lld fix=focus_control",
                  source, detail, mono_ms());
    if (strcmp(source, "marker") != 0) {
        int fd = open(KOMBI_READY_MARKER, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            char line[96];
            int n = snprintf(line, sizeof line, "source=%s uptime_ms=%lld", source, mono_ms());
            if (n > 0) (void)write(fd, line, (size_t)n);
            close(fd);
        } else {
            gal_hook_logf("event=kombi.marker result=write_failed errno=%d fix=focus_control", errno);
        }
    }
    fc_notify();
}

static void block_all_signals(void)
{
    sigset_t all;
    sigfillset(&all);
    (void)pthread_sigmask(SIG_BLOCK, &all, NULL);
}

static int find_bytes(const char *hay, size_t len, const char *needle, size_t needle_len)
{
    size_t i;
    if (needle_len == 0u || len < needle_len) return 0;
    for (i = 0u; i + needle_len <= len; ++i)
        if (hay[i] == needle[0] && memcmp(hay + i, needle, needle_len) == 0) return 1;
    return 0;
}

/* 1 ready, 0 not ready, -1 could not run or parse. */
static int kombi_dmdt_probe(char *detail, size_t detail_len, unsigned *first_displayable)
{
    const gal_secondary_config *cfg = gal_hook_config();
    char *argv[] = { (char *)"dmdt", (char *)"gs", NULL };
    char *envp[] = { (char *)DMDT_FLUSH_PRELOAD, (char *)DMDT_LIBRARY_PATH,
                     (char *)DMDT_CONFIG_DIR, NULL };
    char out[8192];
    char scratch[512];
    char sample[97];
    char want[32];
    size_t used = 0u;
    int pipefd[2];
    int status = 0;
    int eof = 0;
    int result = -1;
    long long deadline;
    pid_t pid;
    char *line;

    (void)snprintf(detail, detail_len, "none");
    if (pipe(pipefd) != 0) return -1;
    (void)fcntl(pipefd[0], F_SETFD, FD_CLOEXEC);
    pid = spawn_clean(DMDT_PATH, argv, envp, pipefd[1], -1);
    close(pipefd[1]);
    if (pid < 0) {
        close(pipefd[0]);
        return -1;
    }

    /* Drain to EOF, so dmdt never blocks on a full pipe, then reap it. */
    deadline = mono_ms() + 1500;
    for (;;) {
        struct pollfd p;
        long long left = deadline - mono_ms();
        ssize_t n;
        if (left <= 0) break;
        p.fd = pipefd[0];
        p.events = POLLIN;
        p.revents = 0;
        if (poll(&p, 1, (int)left) <= 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (used < sizeof out - 1u)
            n = read(pipefd[0], out + used, sizeof out - 1u - used);
        else
            n = read(pipefd[0], scratch, sizeof scratch);
        if (n > 0) {
            if (used < sizeof out - 1u) used += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        eof = (n == 0);
        break;
    }
    close(pipefd[0]);
    if (!eof || !wait_child(pid, 1000, &status)) {
        kill(pid, SIGKILL);
        (void)wait_child(pid, 500, &status);
        return -1;
    }

    out[used] = '\0';
    /*
     * Copy the sample BEFORE parsing: the loop below turns every newline into
     * a NUL, so a sample taken afterwards is only dmdt's first line -- never
     * the display table the matcher failed on.
     */
    {
        size_t n = used < 96u ? used : 96u;
        size_t i;
        for (i = 0u; i < n; ++i)
            sample[i] = (out[i] == '\n' || out[i] == '"') ? ' ' : out[i];
        sample[n] = '\0';
    }
    if (used == 0u) {
        /* dmdt exited without output: its libdmdt_flush preload is missing,
         * or it could not reach the display manager. */
        return -1;
    }
    /*
     * The table on this unit is multi-line, not the single-line form an old
     * capture showed:
     *
     *     display 1:
     *             context id: 70
     *                     3 (--)
     *
     * So: find the display whose "context id:" is ours, then read the
     * indented displayable ids beneath it. 33 is the stock Kombi map; 3 is
     * our own window, which means we already own the cluster.
     */
    (void)snprintf(want, sizeof want, "context id:");
    {
        int in_context = 0;
        for (line = out; line != NULL && *line != '\0'; ) {
            char *end = strchr(line, '\n');
            char *ctx;
            char *q;
            if (end != NULL) *end = '\0';
            if (strstr(line, "display ") == line) {
                in_context = 0;          /* next display block */
                /*
                 * The table is readable, so this is a real "not ready yet",
                 * not something we failed to parse. Early in boot our context
                 * simply is not there: only 2 displayables exist at t=33,
                 * while the Kombi map's first swap is at t=42.
                 */
                if (result < 0) result = 0;
            } else if ((ctx = strstr(line, want)) != NULL) {
                unsigned id = (unsigned)strtoul(ctx + strlen(want), NULL, 10);
                in_context = (id == cfg->context_id);
                /* The table parsed, which is what -2 distinguishes. */
                if (in_context && result < 0) result = 0;
            } else if (in_context) {
                q = line;
                while (*q == ' ' || *q == '\t') ++q;
                if (*q >= '0' && *q <= '9') {
                    unsigned shown = (unsigned)strtoul(q, NULL, 10);
                    result = 0;
                    if (first_displayable != NULL) *first_displayable = shown;
                    (void)snprintf(detail, detail_len, "context %u -> %s",
                                   cfg->context_id, q);
                    if (shown == cfg->restore_displayable_id ||
                        shown == cfg->displayable_id) {
                        result = 1;
                        break;
                    }
                }
            }
            line = end != NULL ? end + 1 : NULL;
        }
    }
    if (result < 0) {
        /* Ran, but nothing looked like the display table: quote what it did
         * print, so one car log is enough to fix the matcher. */
        (void)snprintf(detail, detail_len, "%s", sample);
        return -2;
    }
    return result;
}

static void kombi_slog_wait(void)
{
    char needle[48];
    char buf[4096];
    char seam[96];
    size_t needle_len;
    size_t tail = 0u;
    unsigned long bytes = 0ul;
    int fd;

    (void)snprintf(needle, sizeof needle, "window %u performed 1st swap",
                   gal_hook_config()->restore_displayable_id);
    needle_len = strlen(needle);
    fd = open("/dev/slog", O_RDONLY);
    if (fd < 0) {
        gal_hook_logf("event=kombi.slog result=unavailable errno=%d fix=focus_control", errno);
        return;
    }
    gal_hook_logf("event=kombi.slog result=watching needle=\"%s\" fix=focus_control", needle);
    while (!g_kombi_ready) {
        /*
         * Always the whole buffer: slogger rejects a read too small for the
         * next event with EINVAL, and carrying bytes forward inside the same
         * buffer shrinks it until it does. The seam is kept separately.
         */
        ssize_t n = read(fd, buf, sizeof buf);
        if (n > 0) {
            size_t len = (size_t)n;
            bytes += (unsigned long)n;
            if (tail != 0u) {
                /* The needle may straddle two reads. */
                size_t head = len < needle_len ? len : needle_len;
                memcpy(seam + tail, buf, head);
                if (find_bytes(seam, tail + head, needle, needle_len)) {
                    kombi_mark_ready("slog", needle);
                    break;
                }
            }
            if (find_bytes(buf, len, needle, needle_len)) {
                kombi_mark_ready("slog", needle);
                break;
            }
            tail = len < needle_len - 1u ? len : needle_len - 1u;
            memcpy(seam, buf + len - tail, tail);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) {
            gal_hook_logf("event=kombi.slog result=read_failed errno=%d bytes=%lu fix=focus_control",
                          errno, bytes);
            break;
        }
        usleep(500000); /* 500ms poll: 2 Hz, catches swap within <=0.5s with minimal boot wakeups */
    }
    close(fd);
}

static void *kombi_watch_thread(void *arg)
{
    char detail[160];
    int r;
    long long uptime;
    (void)arg;

    block_all_signals();
    uptime = mono_ms();
    /* Only probe DMDT if started long after boot (>= 90s) where slog might have wrapped.
     * Before 90s, context 70 is either not up yet or caught live by slog; skip the 300ms fork/exec. */
    if (uptime >= KOMBI_BOOT_PROBE_MIN_UPTIME_MS) {
        r = kombi_dmdt_probe(detail, sizeof detail, NULL);
        if (r > 0) {
            kombi_mark_ready("dmdt", detail);
            return NULL;
        }
        gal_hook_logf("event=kombi.dmdt result=%s detail=\"%s\" action=watch_slog fix=focus_control",
                      r == 0 ? "not_ready" : (r == -2 ? "unparsed" : "unavailable"), detail);
    } else {
        gal_hook_logf("event=kombi.dmdt action=skip_early_boot uptime_ms=%lld threshold_ms=%lld action=watch_slog fix=focus_control",
                      uptime, KOMBI_BOOT_PROBE_MIN_UPTIME_MS);
    }
    kombi_slog_wait();
    if (!g_kombi_ready)
        gal_hook_log("event=kombi.wait result=no_source action=hold_native fix=focus_control");
    return NULL;
}

void vc_kombi_watch_start(void)
{
    pthread_attr_t attr;
    pthread_t thread;
    char detail[96];
    int fd;

    if (g_kombi_watch_started) return;
    g_kombi_watch_started = 1;
    fd = open(KOMBI_READY_MARKER, O_RDONLY);
    if (fd >= 0) {
        ssize_t n = read(fd, detail, sizeof detail - 1u);
        close(fd);
        detail[n > 0 ? (size_t)n : 0u] = '\0';
        kombi_mark_ready("marker", detail);
        return;
    }
    (void)pthread_attr_init(&attr);
    (void)pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&thread, &attr, kombi_watch_thread, NULL) != 0)
        gal_hook_log("event=kombi.wait result=no_thread action=hold_native fix=focus_control");
    (void)pthread_attr_destroy(&attr);
}

int vc_kombi_ready(void)
{
    return g_kombi_ready;
}
