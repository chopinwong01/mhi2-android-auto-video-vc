#include "vc_player_mgr.h"
#include "gal_hook.h"
#include "vc_stream_out.h"
#include "focus_ctl.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/*
 * Build identity, stamped in by the Makefile. Every car capture so far had
 * to be dated by which event names it did and did not contain; that guess
 * work is what made the 2026-08-28 log so expensive to read.
 */
#ifndef GAL_HOOK_BUILD
#define GAL_HOOK_BUILD "unknown"
#endif

#define GAL_HOOK_LOG "/fs/sdb0/logs/gal_dualscreen.log"
#define GAL_HOOK_FALLBACK_LOG "/tmp/gal_dualscreen.log"
#define GAL_HOOK_LOG_MAX_MB_DEFAULT 10u

/*
 * vc_display is 4, not 1.
 *
 * 1 is the index `dmdt gs` prints for the virtual cockpit, and it was
 * taken as the argument for `sc`/`sb` -- but those subcommands want a
 * different id. Two independent sources say 4: on-car experimentation
 * recorded in the project notes, and VcMOSTRenderMqb's stream player,
 * which is known to work on this hardware and issues
 *
 *     dmdt dc 70 3        (put displayable 3 into context 70)
 *     dmdt sc 4 70        (point the VC at context 70)
 *
 * and restores with `dc 70 33` + `sc 4 70`. With 1 here the activation
 * silently does nothing: dmdt accepts it and the cockpit never switches,
 * so a perfectly decoded stream would land on a displayable nothing is
 * showing.
 *
 * Outer margins are fixed to 0x0 -- no margin.
 *
 * Margins tell the phone how much of the coded 800x480 frame the head
 * unit will crop, so it keeps its interface inside the rest. The stock VW
 * cluster reserves 400x80, leaving a 400x400 visible area at offset
 * (200, 40), and a real head unit's discovery response declares exactly
 * that. But in own mode we decode and present the frame ourselves and are
 * not cropping anything, so reserving margins would only shrink the
 * phone's usable canvas for nothing. UI placement is controlled separately
 * through UiConfig field 2/contentInsets.
 *
 * The compiled fallback is dpi 140 and viewing_distance 700. The active
 * package config deliberately overrides DPI to the car-tested value 125.
 *
 * It is a UI-SCALE knob, not a physical measurement: gal.json calls its
 * own value a "DPI override for tuning UI element size" and notes that
 * setting it disables automatic calculation. 125 DPI matches the exact
 * physical panel density of the 12.3-inch Virtual Cockpit (1440x540 at 125.03 PPI),
 * scaling the turn banner to an ultra-sleek, compact HUD size, while 700mm
 * brings the 3D map perspective closer to the vehicle. Change it in
 * gal_dualscreen.conf rather than rebuilding.
 *
 * Order: width, height, fps, codec, displayable_id, vc_display,
 * context_id, restore_displayable_id, service_id, display_id, dpi,
 * viewing_distance, pixel_aspect_ratio_e4, insets_top, insets_bottom,
 * insets_left, insets_right, ui_theme, exact-payload length and bytes.
 */
static gal_secondary_config g_config = {
    800u, 480u, 30u, 1u, 3u, 4u, 70u, 33u, 0u, 1u,
    140u, 700u, 10000u,
    40u, 40u, 127u, 127u,
    2u, 0u, { 0 }
};
static volatile int g_initialized;
static int g_enabled;
static int g_debug;
static int g_second_sink;
static int g_inject_meta;
static unsigned g_aap_minor = 2u;
static int g_aap_minor_overridden;
static int g_cluster_input;
static int g_log_fd = -1;

static int parse_hex_payload(const char *value, unsigned char *out, unsigned *out_len)
{
    unsigned n = 0;
    while (value != NULL && value[0] != '\0') {
        unsigned v;
        char a = value[0], b = value[1];
        if (b == '\0' || sscanf(value, "%2x", &v) != 1 || n >= 15u) return 0;
        if (!((a >= '0' && a <= '9') || (a >= 'a' && a <= 'f') || (a >= 'A' && a <= 'F')) ||
            !((b >= '0' && b <= '9') || (b >= 'a' && b <= 'f') || (b >= 'A' && b <= 'F'))) return 0;
        out[n++] = (unsigned char)v;
        value += 2;
    }
    *out_len = n;
    return n > 0u;
}
/*
 * The log fd is opened lazily on first use, and GAL's callback dispatch is
 * genuinely cross-thread, so two threads can both find it closed and both
 * open it -- leaking one descriptor and racing the assignment. The open is
 * once per process; a mutex around it costs nothing.
 *
 * g_log_unavailable stops the retry loop when neither path can be opened:
 * without it every single log call would attempt two open() syscalls,
 * forever, inside GAL's own threads.
 */
static pthread_mutex_t g_log_open_mutex = PTHREAD_MUTEX_INITIALIZER;
static int g_log_unavailable;

static off_t hook_log_max_bytes(void)
{
    const char *value = getenv("GAL_HOOK_LOG_MAX_MB");
    unsigned long mb = value != NULL && *value != '\0' ?
        strtoul(value, NULL, 10) : GAL_HOOK_LOG_MAX_MB_DEFAULT;
    if (mb == 0ul) return 0;
    if (mb > 1024ul) mb = 1024ul;
    return (off_t)(mb * 1024ul * 1024ul);
}

/* Like environment_flag(), but absent/empty means ENABLED. Used for the
 * bisect switches, which must not change behaviour when unset. */
static int environment_flag_default_on(const char *name)
{
    const char *value = getenv(name);
    if (value == NULL || *value == '\0') return 1;
    return strcmp(value, "0") != 0 && strcmp(value, "false") != 0 &&
           strcmp(value, "no") != 0;
}

static int environment_flag(const char *name)
{
    const char *value = getenv(name);
    return value != NULL && *value != '\0' && strcmp(value, "0") != 0 &&
           strcmp(value, "false") != 0 && strcmp(value, "no") != 0;
}

/* Read once in gal_hook_init, after the config file, so the controller
 * thread never calls getenv while vc_player_start edits the environment. */
static int g_fix_on[HOOK_FIX_COUNT];
static int g_output_mode = GAL_OUTPUT_WITHHOLD;
static int g_focus_mirror;
static int g_focus_start_native = 1;
static int g_focus_wait_kombi;

int hook_fix_enabled(hook_fix fix)
{
    return (unsigned)fix < (unsigned)HOOK_FIX_COUNT ? g_fix_on[fix] : 0;
}

int gal_hook_focus_start_native(void) { return g_focus_start_native; }
int gal_hook_focus_wait_kombi(void) { return g_focus_wait_kombi; }

static void hook_vlog(const char *level, const char *format, va_list arguments)
{
    char message[768];
    char buffer[960];
    struct timespec now;
    unsigned long seconds;
    unsigned milliseconds;
    int n;
    const char *log_path;

    if (g_log_fd < 0 && !g_log_unavailable) {
        pthread_mutex_lock(&g_log_open_mutex);
        if (g_log_fd < 0 && !g_log_unavailable) {   /* re-check under the lock */
            log_path = getenv("GAL_HOOK_LOG");
            if (log_path == NULL || *log_path == '\0') log_path = GAL_HOOK_LOG;
            g_log_fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0666);
            if (g_log_fd < 0 && strcmp(log_path, GAL_HOOK_FALLBACK_LOG) != 0)
                g_log_fd = open(GAL_HOOK_FALLBACK_LOG,
                                O_WRONLY | O_CREAT | O_APPEND, 0666);
            if (g_log_fd < 0) g_log_unavailable = 1;
        }
        pthread_mutex_unlock(&g_log_open_mutex);
    }
    if (g_log_fd < 0) return;

    (void)vsnprintf(message, sizeof(message), format, arguments);
    if (clock_gettime(CLOCK_MONOTONIC, &now) == 0) {
        seconds = (unsigned long)now.tv_sec;
        milliseconds = (unsigned)(now.tv_nsec / 1000000L);
    } else {
        seconds = 0ul;
        milliseconds = 0u;
    }
    n = snprintf(buffer, sizeof(buffer),
                 "gal_dualscreen t=%lu.%03u pid=%ld level=%s %s\n",
                 seconds, milliseconds, (long)getpid(), level, message);
    if (n > 0) {
        size_t length = (size_t)n;
        size_t written = 0u;
        if (length >= sizeof(buffer)) length = sizeof(buffer) - 1u;
        /*
         * Serialise the write. GAL logs from several threads at once and a
         * bare write() from each interleaved them: the 2026-08-28 car capture
         * came back with 465 NUL bytes in four runs and spliced records like
         * "...mode=2 uncgal_dualscreen t=24.643...", destroying four complete
         * records and truncating two more -- all inside the setup bursts that
         * every focus-ordering argument depends on. The mutex already exists
         * for the lazy open; hold it across the write too.
         *
         * Also loop on short writes rather than assuming one call suffices;
         * a partial write was the other way a record could end up spliced.
         */
        pthread_mutex_lock(&g_log_open_mutex);
        {
            struct stat st;
            off_t max_bytes = hook_log_max_bytes();
            if (max_bytes > 0 && fstat(g_log_fd, &st) == 0 &&
                st.st_size + (off_t)length > max_bytes) {
                /* Keep the active log bounded without relying on logrotate,
                 * which is not available on the head unit. */
                (void)ftruncate(g_log_fd, 0);
            }
        }
        while (written < length) {
            ssize_t w = write(g_log_fd, buffer + written, length - written);
            if (w > 0) { written += (size_t)w; continue; }
            if (w < 0 && errno == EINTR) continue;
            break;
        }
        pthread_mutex_unlock(&g_log_open_mutex);
    }
}

void gal_hook_log(const char *message)
{
    gal_hook_logf("%s", message);
}

void gal_hook_logf(const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    hook_vlog("info", format, arguments);
    va_end(arguments);
}

void gal_hook_debugf(const char *format, ...)
{
    va_list arguments;
    if (!g_debug) return;
    va_start(arguments, format);
    hook_vlog("debug", format, arguments);
    va_end(arguments);
}

static void load_environment(void)
{
    const char *value;

    value = getenv("GAL_SECONDARY_WIDTH");
    if (value != NULL) g_config.width = (unsigned)strtoul(value, NULL, 10);
    value = getenv("GAL_SECONDARY_HEIGHT");
    if (value != NULL) g_config.height = (unsigned)strtoul(value, NULL, 10);
    value = getenv("GAL_SECONDARY_FPS");
    if (value != NULL) g_config.fps = (unsigned)strtoul(value, NULL, 10);
    value = getenv("GAL_VC_DISPLAYABLE_ID");
    if (value == NULL) value = getenv("GAL_SECONDARY_DISPLAY_ID");
    if (value != NULL) g_config.displayable_id = (unsigned)strtoul(value, NULL, 10);
    value = getenv("GAL_VC_DISPLAY");
    if (value != NULL) g_config.vc_display = (unsigned)strtoul(value, NULL, 10);
    value = getenv("GAL_VC_CONTEXT");
    if (value != NULL) g_config.context_id = (unsigned)strtoul(value, NULL, 10);
    value = getenv("GAL_VC_RESTORE_DISPLAYABLE_ID");
    if (value != NULL)
        g_config.restore_displayable_id = (unsigned)strtoul(value, NULL, 10);
    value = getenv("GAL_SECONDARY_SERVICE_ID");
    if (value != NULL) g_config.service_id = (unsigned)strtoul(value, NULL, 0);
    value = getenv("GAL_SECONDARY_AAP_DISPLAY_ID");
    if (value != NULL) g_config.display_id = (unsigned)strtoul(value, NULL, 0);
    value = getenv("GAL_SECONDARY_DPI");
    if (value != NULL) g_config.dpi = (unsigned)strtoul(value, NULL, 0);
    value = getenv("GAL_SECONDARY_VIEWING_DISTANCE");
    if (value != NULL) g_config.viewing_distance = (unsigned)strtoul(value, NULL, 0);
    value = getenv("GAL_SECONDARY_PIXEL_ASPECT_RATIO_E4");
    if (value != NULL)
        g_config.pixel_aspect_ratio_e4 = (unsigned)strtoul(value, NULL, 0);
    value = getenv("GAL_SECONDARY_INSETS");
    if (value != NULL && value[0] != 0) {
        unsigned t, b, l, r;
        if (sscanf(value, "%u,%u,%u,%u", &t, &b, &l, &r) == 4) {
            g_config.insets_top = t;
            g_config.insets_bottom = b;
            g_config.insets_left = l;
            g_config.insets_right = r;
        }
    }
    value = getenv("GAL_SECONDARY_INSETS_TOP");
    if (value != NULL) g_config.insets_top = (unsigned)strtoul(value, NULL, 0);
    value = getenv("GAL_SECONDARY_INSETS_BOTTOM");
    if (value != NULL) g_config.insets_bottom = (unsigned)strtoul(value, NULL, 0);
    value = getenv("GAL_SECONDARY_INSETS_LEFT");
    if (value != NULL) g_config.insets_left = (unsigned)strtoul(value, NULL, 0);
    value = getenv("GAL_SECONDARY_INSETS_RIGHT");
    if (value != NULL) g_config.insets_right = (unsigned)strtoul(value, NULL, 0);
    value = getenv("GAL_SECONDARY_UI_THEME");
    if (value != NULL) g_config.ui_theme = (unsigned)strtoul(value, NULL, 0);
    value = getenv("GAL_SECONDARY_UI_CONFIG_HEX");
    if (value != NULL && !parse_hex_payload(value, g_config.ui_config_payload,
                                             &g_config.ui_config_payload_len)) {
        gal_hook_logf("event=config.ui_payload result=invalid value=%s", value);
    }
}

/*
 * Load settings from a plain KEY=VALUE file.
 *
 * smartphone_integrator silently drops the whole environment array past
 * about ten entries, which caps how many switches can be delivered that
 * way and has already cost one session to diagnose. A file has no such
 * limit -- and, more usefully on a car, it can be edited between boots
 * without re-running enable_hook.sh and without touching the system
 * config at all.
 *
 * Each key is pushed into the environment with setenv(..., 0), i.e. only
 * when not already set, so every existing getenv() in this file keeps
 * working untouched and an explicitly injected variable still wins. That
 * ordering matters: the environment is what enable_hook.sh deliberately
 * chose, the file is a default.
 *
 * Blank lines and lines beginning with # are ignored, as is anything
 * without an '='. Whitespace around the key and value is trimmed so
 * "GAL_DUALSCREEN_OUTPUT = own" behaves as expected.
 */
static void load_config_file(void)
{
    const char *candidates[5];
    char line[512];
    const char *chosen = NULL;
    FILE *f = NULL;
    unsigned applied = 0u;
    unsigned already = 0u;
    unsigned rejected = 0u;
    unsigned truncated = 0u;
    unsigned i;

    candidates[0] = getenv("GAL_HOOK_CONF");
    candidates[1] = "/fs/sdb0/gal_dualscreen.conf";
    candidates[2] = "/fs/sda0/gal_dualscreen.conf";
    candidates[3] = "/eso/lib/gal_dualscreen/gal_dualscreen.conf";
    candidates[4] = "/tmp/gal_dualscreen.conf";
    for (i = 0u; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
        if (candidates[i] == NULL || *candidates[i] == '\0') continue;
        f = fopen(candidates[i], "r");
        if (f != NULL) { chosen = candidates[i]; break; }
    }
    if (f == NULL) return;

    while (fgets(line, (int)sizeof(line), f) != NULL) {
        char *eq, *key, *val, *end;
        size_t len = strlen(line);
        /*
         * A line longer than the buffer would otherwise be split, and the
         * tail processed as if it were a line of its own -- silently
         * truncating a value, or worse, inventing a key out of whatever
         * followed an '=' in the middle of it. Discard the remainder of
         * the physical line and count it.
         */
        if (len > 0u && line[len - 1u] != '\n' && !feof(f)) {
            int c;
            while ((c = fgetc(f)) != EOF && c != '\n') { }
            ++truncated;
            continue;
        }
        key = line;
        while (*key == ' ' || *key == '\t') ++key;
        if (*key == '#' || *key == '\n' || *key == '\r' || *key == '\0') continue;
        eq = strchr(key, '=');
        if (eq == NULL) continue;
        *eq = '\0';
        val = eq + 1;
        /* Trim the key's trailing space. Walk down from eq rather than
         * starting at eq-1, which is a pointer before the array when the
         * line begins with '='. */
        end = eq;
        while (end > key && (end[-1] == ' ' || end[-1] == '\t')) *--end = '\0';
        while (*val == ' ' || *val == '\t') ++val;
        /* trim the value's trailing space and newline */
        end = val + strlen(val);
        while (end > val && (end[-1] == '\n' || end[-1] == '\r' ||
                             end[-1] == ' ' || end[-1] == '\t')) *--end = '\0';
        if (*key == '\0') continue;
        /*
         * Only GAL_ keys. This file lands in the environment of a live
         * GAL process, and without a prefix rule a stray line could set
         * LD_LIBRARY_PATH, PATH or anything else that later dlopen calls
         * depend on. Every key the hook reads is GAL_-prefixed, so nothing
         * legitimate is lost, and a typo'd or stray key is reported rather
         * than quietly taking effect somewhere unrelated.
         */
        if (strncmp(key, "GAL_", 4) != 0) { ++rejected; continue; }
        /*
         * setenv(..., 0) returns success whether or not it actually set
         * anything, so ask first. Otherwise the count below reports every
         * parsed line as applied and an injected variable that overrode
         * the file looks like it came from the file.
         */
        if (getenv(key) != NULL) {
            ++already;
        } else if (setenv(key, val, 0) == 0) {
            ++applied;
        }
    }
    fclose(f);
    /*
     * Logged after the fact rather than as we go: the log path itself can
     * come from this file, so nothing here can be logged until it is all
     * read.
     */
    gal_hook_logf("event=config.file result=loaded path=%s applied=%u already_set=%u rejected_non_gal=%u overlong_lines=%u",
                  chosen, applied, already, rejected, truncated);
}

int gal_hook_init(const gal_secondary_config *config)
{
    const char *value;

    /* Before anything reads the environment. */
    load_config_file();
    value = getenv("GAL_DUALSCREEN_OUTPUT");
    if (value == NULL || *value == '\0' || strcmp(value, "withhold") == 0) {
        g_output_mode = GAL_OUTPUT_WITHHOLD;
    } else if (strcmp(value, "gal") == 0) {
        g_output_mode = GAL_OUTPUT_GAL;
    } else {
        gal_hook_logf("event=config.output result=unknown value=%s fallback=withhold", value);
        g_output_mode = GAL_OUTPUT_WITHHOLD;
    }
    g_focus_mirror = environment_flag("GAL_DUALSCREEN_FOCUS_MIRROR");
    g_fix_on[HOOK_FIX_HELPER_INIT_GUARD] =
        environment_flag_default_on("GAL_FIX_HELPER_INIT_GUARD");
    g_fix_on[HOOK_FIX_FOCUS_CONTROL] =
        environment_flag_default_on("GAL_FOCUS_CONTROL") &&
        g_output_mode == GAL_OUTPUT_WITHHOLD &&
        environment_flag_default_on("GAL_STREAM_ENABLE");
    g_fix_on[HOOK_FIX_ACK_RENDERED_ONLY] =
        environment_flag_default_on("GAL_FIX_ACK_RENDERED_ONLY") &&
        g_fix_on[HOOK_FIX_FOCUS_CONTROL];
    g_fix_on[HOOK_FIX_NO_IDR_REPLAY] =
        environment_flag_default_on("GAL_FIX_NO_IDR_REPLAY") &&
        g_fix_on[HOOK_FIX_FOCUS_CONTROL];
    g_fix_on[HOOK_FIX_STREAM_TIMING] =
        environment_flag_default_on("GAL_FIX_STREAM_TIMING");
    g_fix_on[HOOK_FIX_PLAYER_LOG] =
        environment_flag_default_on("GAL_FIX_PLAYER_LOG");
    g_fix_on[HOOK_FIX_ORPHAN_RESTORE] =
        environment_flag_default_on("GAL_FIX_ORPHAN_RESTORE");
    value = getenv("GAL_FOCUS_START");
    g_focus_start_native = !(value != NULL && strcmp(value, "stock") == 0);
    g_focus_wait_kombi = environment_flag_default_on("GAL_FOCUS_WAIT_KOMBI");

    if (config != NULL) {
        g_config = *config;
    } else {
        load_environment();
    }
    g_enabled = environment_flag("GAL_DUALSCREEN_ENABLE");
    g_debug = environment_flag("GAL_DUALSCREEN_DEBUG");
    g_second_sink = environment_flag_default_on("GAL_DUALSCREEN_SECOND_SINK");
    g_inject_meta = environment_flag_default_on("GAL_DUALSCREEN_INJECT_META");
    value = getenv("GAL_DUALSCREEN_AAP_MINOR");
    g_aap_minor_overridden = (value != NULL && *value != '\0');
    g_aap_minor = g_aap_minor_overridden ?
        (unsigned)strtoul(value, NULL, 10) : 2u;
    if (g_aap_minor > 255u) { g_aap_minor = 2u; g_aap_minor_overridden = 0; }
    g_cluster_input = environment_flag_default_on("GAL_DUALSCREEN_CLUSTER_INPUT");

    /*
     * The cluster takes 800x480 and nothing else, so the resolution is not
     * a tuning knob and is pinned here regardless of what was configured.
     *
     * VideoSink::codecResolutionToPixels (0xfa870 in libautoreceiver.so)
     * maps codec_resolution enum 1 to exactly 800x480 and returns -8 for
     * anything it does not recognise, and the captured discovery response
     * from a working head unit declares enum 1 for its cluster sink. The
     * hook already advertises VIDEO_CODEC_RESOLUTION_800_480 to the phone,
     * so letting width/height say something else only desynchronises our
     * own decoder from the stream the phone actually sends -- which is how
     * the 400x400 attempt went wrong: 400x400 is a possible VISIBLE area
     * inside the coded frame, not a coded-frame size. Outer margins are
     * fixed to zero in this build; UiConfig carries the UI safe area.
     */
    if (g_config.width != 800u || g_config.height != 480u) {
        gal_hook_logf("event=config.resolution result=pinned requested=%ux%u using=800x480 reason=cluster_accepts_only_800x480",
                      g_config.width, g_config.height);
        g_config.width = 800u;
        g_config.height = 480u;
    }
    /*
     * Width and height are pinned above and can no longer be zero, so this
     * is now purely an fps check -- and the firmware's own gal.json says
     * "Supported framerates are 30 or 60 fps". Anything else is not a
     * gentle degradation: the hook matches the primary's configuration
     * table on (resolution, fps) to build the secondary's entry, so an
     * unsupported rate makes that lookup fail and the secondary sink is
     * never built at all, reported only as no_matching_800x480_fps.
     * Rejecting it here says so plainly instead.
     */
    if (g_config.fps != 30u && g_config.fps != 60u) {
        errno = EINVAL;
        gal_hook_logf("event=config.invalid reason=unsupported_fps fps=%u supported=30,60",
                      g_config.fps);
        return -1;
    }

    /*
     * Open the forwarding listener now rather than lazily on the first
     * secondary frame. The consumer is a separate process the user starts
     * by hand, and if nothing is listening until frames already flow, that
     * process fails to connect and exits before there is anything to
     * connect to. Opening at init lets it attach at any time and simply
     * wait.
     */
    if (environment_flag_default_on("GAL_STREAM_ENABLE")) {
        (void)vc_stream_out_open();
    }

    g_initialized = 1;
    gal_hook_logf(
        "event=init build=%s enabled=%d debug=%d second_sink=%d inject_meta=%d aap_minor=%u cluster_input=%d width=%u height=%u fps=%u dpi=%u insets=%u,%u,%u,%u ui_theme=%u viewing_distance=%u pixel_aspect_e4=%u qnx_displayable=%u vc_display=%u context=%u aap_display=%u service=%s",
        GAL_HOOK_BUILD, g_enabled, g_debug, g_second_sink, g_inject_meta, g_aap_minor, g_cluster_input, g_config.width, g_config.height, g_config.fps,
        g_config.dpi,
        g_config.insets_top, g_config.insets_bottom, g_config.insets_left, g_config.insets_right,
        g_config.ui_theme,
        g_config.viewing_distance, g_config.pixel_aspect_ratio_e4,
        g_config.displayable_id, g_config.vc_display, g_config.context_id,
        g_config.display_id, g_config.service_id == 0u ? "auto" : "fixed");
    gal_hook_logf("event=fix.config helper_init_guard=%d focus_control=%d focus_start=%s focus_wait_kombi=%d ack_rendered_only=%d no_idr_replay=%d stream_timing=%d player_log=%d orphan_restore=%d output=%s",
                  g_fix_on[HOOK_FIX_HELPER_INIT_GUARD],
                  g_fix_on[HOOK_FIX_FOCUS_CONTROL],
                  g_focus_start_native ? "native" : "stock",
                  g_focus_wait_kombi,
                  g_fix_on[HOOK_FIX_ACK_RENDERED_ONLY],
                  g_fix_on[HOOK_FIX_NO_IDR_REPLAY],
                  g_fix_on[HOOK_FIX_STREAM_TIMING],
                  g_fix_on[HOOK_FIX_PLAYER_LOG],
                  g_fix_on[HOOK_FIX_ORPHAN_RESTORE],
                  gal_hook_output_mode_name());
    return 0;
}

int gal_hook_is_enabled(void) { return g_enabled; }
int gal_hook_second_sink_enabled(void) { return g_second_sink; }
int gal_hook_inject_meta_enabled(void) { return g_inject_meta; }
unsigned gal_hook_aap_minor(void) { return g_aap_minor; }
int gal_hook_aap_minor_overridden(void) { return g_aap_minor_overridden; }
int gal_hook_cluster_input_enabled(void) { return g_cluster_input; }

int gal_hook_output_mode(void)
{
    return g_output_mode;
}

/*
 * Focus mirroring defaults OFF. It copied the primary's focus mode onto the
 * cluster sink, which on the car overwrote the cluster's own grant with
 * "not projected" and made the phone stop the stream. Set
 * GAL_DUALSCREEN_FOCUS_MIRROR=1 to restore the old behaviour for comparison.
 */
/*
 * Tag GAL's own main InputSourceService with display id 0. Default ON: a
 * two-display declaration in which only one input service names its display
 * is the leading explanation for the phone sending no video to either
 * screen. Set GAL_DUALSCREEN_MAIN_INPUT_ID=0 to bisect it back out.
 */
int gal_hook_main_input_id_enabled(void)
{
    return environment_flag_default_on("GAL_DUALSCREEN_MAIN_INPUT_ID");
}

int gal_hook_focus_mirror_enabled(void)
{
    return g_focus_mirror;
}

int gal_hook_frame_ack_enabled(void)
{
    return environment_flag_default_on("GAL_DUALSCREEN_ACK");
}

const char *gal_hook_output_mode_name(void)
{
    return gal_hook_output_mode() == GAL_OUTPUT_WITHHOLD ? "withhold" : "gal";
}
int gal_hook_is_debug(void) { return g_debug; }
const gal_secondary_config *gal_hook_config(void) { return &g_config; }

static pid_t g_main_pid = 0;

static void gal_hook_set_owner_pid(void)
{
    const char *owner = getenv("GAL_HOOK_OWNER_PID");
    char value[32];
    char *end = NULL;
    long parsed;

    if (owner != NULL && *owner != '\0') {
        parsed = strtol(owner, &end, 10);
        if (end != owner && *end == '\0' && parsed > 0) {
            g_main_pid = (pid_t)parsed;
            return;
        }
    }

    g_main_pid = getpid();
    (void)snprintf(value, sizeof(value), "%ld", (long)g_main_pid);
    (void)setenv("GAL_HOOK_OWNER_PID", value, 1);
}

/* QNX loads shared objects without requiring a custom entry point. */
__attribute__((constructor))
static void gal_hook_constructor(void)
{
    if (g_main_pid == 0) gal_hook_set_owner_pid();
    /*
     * fix=helper_init_guard. Processes GAL spawns inherit LD_PRELOAD and
     * GAL_HOOK_OWNER_PID; vc_player_start's system("slay ...") runs sh and
     * slay with both. Each ran a full hook init and probed
     * /tmp/gal_video.sock. On 2026-09-15 (t=170.018) sh's probe connected,
     * slay's was refused, remove_stale_socket took that for a stale path, and
     * slay unlinked and rebound it: every later player reached gal_ack.sock
     * but never the video socket. Read straight from the environment, which
     * already holds the values GAL applied from the config file.
     */
    if (getpid() != g_main_pid &&
        environment_flag_default_on("GAL_FIX_HELPER_INIT_GUARD")) {
        gal_hook_logf("event=init result=skipped reason=helper_process owner_pid=%ld fix=helper_init_guard",
                      (long)g_main_pid);
        return;
    }
    (void)gal_hook_init(NULL);
}

__attribute__((destructor))
static void gal_hook_destructor(void)
{
    if (g_main_pid != 0 && getpid() != g_main_pid) {
        /*
         * Do not tear down sockets, player, or log file from a child process!
         * When gal or smartphone_integrator spawns helper binaries or slay,
         * their C runtimes invoke shared library destructors on exit.
         */
        return;
    }
    fc_shutdown();
    /* gal is exiting: no pings left to answer, so let the player put the
     * Kombi map back itself. */
    vc_player_stop_wait(3000);
    vc_stream_out_close();
    if (g_log_fd >= 0) {
        close(g_log_fd);
        g_log_fd = -1;
    }
}
