#include "vc_stream_out.h"
#include "focus_ctl.h"
#include "gal_hook.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define VC_ACK_SOCK_PATH       "/tmp/gal_ack.sock"
/*
 * Total time one frame may spend waiting for a slow reader. This runs on
 * gal's reader thread, which also answers the phone's PINGs: about 5 s
 * without a reply and the phone tears the session down. The old 250 ms was
 * per select() round, so a client draining a few bytes per round could hold
 * this thread -- and g_lock, and so the ACK pump -- for far longer.
 */
#define VC_WRITE_BUDGET_MS     250
/*
 * A frame larger than the socket buffer cannot be handed over in one go: the
 * kernel clamps our 2 MB request to about 125 KB, and the largest IDR
 * measured on the car is 139 KB. Those need several drain rounds, so the
 * budget scales with the frame -- still far below the ~5 s that costs the
 * session, and only ever spent when the player has stopped reading.
 */
#define VC_WRITE_BUDGET_MAX_MS 1000

static pid_t g_server_pid = 0;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_listen_fd = -1;
static int g_client_fd = -1;
static unsigned long g_frames_no_client = 0ul;
static int g_open_failed = 0;                 /* do not retry a hard failure */
static unsigned long g_sent_bytes = 0ul;
static unsigned long g_dropped_bytes = 0ul;
static unsigned long g_dropped_chunks = 0ul;
static unsigned char g_sps_pps_cache[512];
static size_t g_sps_pps_len = 0u;
static unsigned char g_keyframe_cache[524288]; /* 512KB persistent IDR keyframe cache */
static size_t g_keyframe_len = 0u;
static int g_client_has_keyframe = 0;
static unsigned long g_accepts = 0ul;
/* Bumped on every accept: a descriptor number can be closed and reused while
 * the controller is in poll() on it. */
static unsigned long g_client_gen = 0ul;
static size_t g_sndbuf_actual = 0u;   /* what the kernel granted, not what we asked */
/* fix=stream_timing, g_lock */
static unsigned long g_timing_sends = 0ul;
static unsigned long g_timing_sum_us = 0ul;
static unsigned long g_timing_max_us = 0ul;
static unsigned long g_timing_eagain = 0ul;

/* ACK Feedback Pipe file descriptors */
static int g_ack_listen_fd = -1;
static int g_ack_client_fd = -1;
static unsigned long g_ack_accepts = 0ul;

static void drop_client(const char *reason, int err);

static long long stream_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + (long long)(ts.tv_nsec / 1000000L);
}

static int contains_idr_nal(const unsigned char *p, size_t len)
{
    size_t i;
    for (i = 0; i + 4 < len; ++i) {
        if (p[i] == 0 && p[i+1] == 0) {
            if (p[i+2] == 1) {
                unsigned char nal_type = p[i+3] & 0x1F;
                if (nal_type == 5) return 1;
            } else if (p[i+2] == 0 && p[i+3] == 1) {
                unsigned char nal_type = p[i+4] & 0x1F;
                if (nal_type == 5) return 1;
            }
        }
    }
    return 0;
}

static int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* Remove a stale pathname only when no live listener owns it. */
static int remove_stale_socket(const char *path)
{
    struct sockaddr_un addr;
    int probe;
    int saved_errno;

    probe = socket(AF_UNIX, SOCK_STREAM, 0);
    if (probe < 0) return -1;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
    if (connect(probe, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
        close(probe);
        errno = EADDRINUSE;
        return -1;
    }
    saved_errno = errno;
    close(probe);
    if (saved_errno != ENOENT && saved_errno != ECONNREFUSED &&
        saved_errno != ENOTSOCK) {
        errno = saved_errno;
        return -1;
    }
    return unlink(path);
}


/*
 * Transport, and why the default is TCP on loopback rather than the Unix
 * socket this branch was built around.
 *
 * Measured on the car: an AF_UNIX stream socket gets 7168 bytes of send
 * buffer and 5120 of receive; AF_INET gets 128480. Neither setsockopt nor
 * any sysctl can raise the AF_UNIX figure -- QNX 6.5's local-domain code
 * exposes no tunable for it, so 5 KB is structural. Frames on this stream
 * run median 1,028 B, p95 28,460, max 139,485, so a large one crosses
 * AF_UNIX in dozens of 5 KB rounds, each a scheduler hop, against one or two
 * writes over TCP. That is the measured 3.3-4 fps against 25-30.
 *
 * Both families are served by io-pkt here, so the Unix socket was never
 * avoiding the network stack either way; it is simply the worse-sized of
 * the two. GAL_STREAM_TRANSPORT=unix goes back, for comparison runs and for
 * when the shared-memory path lands.
 */
static int stream_use_tcp(void)
{
    static int cached = -1;
    if (cached < 0) {
        const char *v = getenv("GAL_STREAM_TRANSPORT");
        cached = (v != NULL && strcmp(v, "unix") == 0) ? 0 : 1;
    }
    return cached;
}

static unsigned short stream_port(void)
{
    const char *v = getenv("GAL_STREAM_PORT");
    unsigned long p = (v != NULL && *v != '\0') ? strtoul(v, NULL, 10) : 0ul;
    if (p == 0ul || p > 65535ul) p = 12346ul;
    return (unsigned short)p;
}

/* Caller holds g_lock. */
static int ensure_listening_tcp(void)
{
    struct sockaddr_in addr;
    int one = 1;
    unsigned short port = stream_port();

    g_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_listen_fd < 0) {
        g_open_failed = 1;
        gal_hook_logf("event=stream.open result=failed reason=socket transport=tcp errno=%d", errno);
        return -1;
    }
    (void)setsockopt(g_listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (set_nonblocking(g_listen_fd) != 0) {
        gal_hook_logf("event=stream.open result=failed reason=nonblock transport=tcp errno=%d", errno);
        close(g_listen_fd); g_listen_fd = -1; g_open_failed = 1;
        return -1;
    }
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    if (bind(g_listen_fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
        gal_hook_logf("event=stream.open result=failed reason=bind transport=tcp port=%u errno=%d",
                      (unsigned)port, errno);
        close(g_listen_fd); g_listen_fd = -1; g_open_failed = 1;
        return -1;
    }
    if (listen(g_listen_fd, 1) != 0) {
        gal_hook_logf("event=stream.open result=failed reason=listen transport=tcp port=%u errno=%d",
                      (unsigned)port, errno);
        close(g_listen_fd); g_listen_fd = -1; g_open_failed = 1;
        return -1;
    }
    g_server_pid = getpid();
    (void)fcntl(g_listen_fd, F_SETFD, FD_CLOEXEC);
    gal_hook_logf("event=stream.open result=success transport=tcp port=%u pid=%d",
                  (unsigned)port, (int)g_server_pid);
    return 0;
}

/* Caller holds g_lock. */
static int ensure_listening(void)
{
    struct sockaddr_un addr;
    const char *sock_path = VC_STREAM_SOCK_PATH;
    int sndbuf = 2097152; /* 2MB send buffer */

    if (g_listen_fd >= 0) return 0;
    if (g_open_failed) return -1;
    if (stream_use_tcp()) return ensure_listening_tcp();

    g_listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (g_listen_fd < 0) {
        g_open_failed = 1;
        gal_hook_logf("event=stream.open result=failed reason=socket errno=%d", errno);
        return -1;
    }
    (void)setsockopt(g_listen_fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof sndbuf);

    if (set_nonblocking(g_listen_fd) != 0) {
        gal_hook_logf("event=stream.open result=failed reason=nonblock errno=%d", errno);
        close(g_listen_fd); g_listen_fd = -1; g_open_failed = 1;
        return -1;
    }

    /* Remove only a stale pathname; never unlink a live listener. */
    if (remove_stale_socket(sock_path) != 0 && errno == EADDRINUSE) {
        gal_hook_logf("event=stream.open result=failed reason=socket_in_use path=%s", sock_path);
        close(g_listen_fd); g_listen_fd = -1; g_open_failed = 1;
        return -1;
    }

    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);

    if (bind(g_listen_fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
        gal_hook_logf("event=stream.open result=failed reason=bind path=%s errno=%d", sock_path, errno);
        close(g_listen_fd); g_listen_fd = -1; g_open_failed = 1;
        return -1;
    }
    if (listen(g_listen_fd, 1) != 0) {
        gal_hook_logf("event=stream.open result=failed reason=listen path=%s errno=%d", sock_path, errno);
        close(g_listen_fd); g_listen_fd = -1; g_open_failed = 1;
        return -1;
    }

    g_server_pid = getpid();
    (void)fcntl(g_listen_fd, F_SETFD, FD_CLOEXEC);
    gal_hook_logf("event=stream.open result=success path=%s pid=%d", sock_path, (int)g_server_pid);
    return 0;
}

/*
 * Scan an access unit for SPS (NAL type 7) and PPS (NAL type 8).
 * Android Auto delivers these at the head of IDR keyframes in handleDataAvailable.
 * If present, retain them in g_sps_pps_cache so codec config is available even if
 * handleCodecConfig was never dispatched. Caller holds g_lock.
 */
static void extract_inline_sps_pps(const unsigned char *p, size_t len)
{
    size_t i;
    size_t sps_start = (size_t)-1;
    size_t pps_end = (size_t)-1;
    int found_sps = 0;
    int found_pps = 0;

    for (i = 0; i + 4 < len; ++i) {
        if (p[i] == 0 && p[i+1] == 0) {
            size_t sc_len = 0;
            unsigned char nal_type = 0;
            if (p[i+2] == 1) {
                sc_len = 3;
                nal_type = p[i+3] & 0x1F;
            } else if (p[i+2] == 0 && p[i+3] == 1) {
                sc_len = 4;
                nal_type = p[i+4] & 0x1F;
            }
            if (sc_len > 0) {
                if (nal_type == 7 && !found_sps) {
                    sps_start = i;
                    found_sps = 1;
                } else if (nal_type == 8 && found_sps && !found_pps) {
                    found_pps = 1;
                } else if (found_sps && found_pps && nal_type != 7 && nal_type != 8) {
                    pps_end = i;
                    break;
                }
            }
        }
    }
    if (found_sps && found_pps) {
        if (pps_end == (size_t)-1) pps_end = len;
        if (sps_start < pps_end && (pps_end - sps_start) <= sizeof(g_sps_pps_cache)) {
            g_sps_pps_len = pps_end - sps_start;
            memcpy(g_sps_pps_cache, p + sps_start, g_sps_pps_len);
            gal_hook_logf("event=stream.codec_config result=extracted_inline bytes=%u",
                          (unsigned)g_sps_pps_len);
        }
    }
}

/* Send entire buffer on non-blocking socket with select timeout. Caller holds g_lock. */
/* timeout_ms is the budget for the whole buffer, not for each round. */
static int send_all_timeout(int fd, const unsigned char *buf, size_t len, int timeout_ms)
{
    size_t left = len;
    const unsigned char *p = buf;
    long long deadline = stream_now_ms() + timeout_ms;
    while (left > 0u) {
        ssize_t n = send(fd, p, left, MSG_NOSIGNAL);
        if (n > 0) {
            p += (size_t)n;
            left -= (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EWOULDBLOCK || errno == EAGAIN)) {
            struct timeval tv;
            fd_set wfds;
            int sret;
            long long left_ms = deadline - stream_now_ms();
            long remaining;
            if (left_ms <= 0) return -1;
            /* Narrow before dividing: 64-bit division drags libgcc's helpers
             * into the hook, and with them the _Unwind_* symbols -- which an
             * LD_PRELOAD library must never export over gal's own. */
            remaining = left_ms > (long long)timeout_ms ? timeout_ms : (long)left_ms;
            FD_ZERO(&wfds);
            FD_SET(fd, &wfds);
            tv.tv_sec = remaining / 1000L;
            tv.tv_usec = (remaining % 1000L) * 1000L;
            sret = select(fd + 1, NULL, &wfds, NULL, &tv);
            if (sret > 0 && FD_ISSET(fd, &wfds)) {
                continue;
            }
            return -1; /* timed out or stalled */
        }
        return -1; /* hard socket error */
    }
    return 0;
}

/*
 * fix=focus_control transmit queue.
 *
 * gal's reader thread used to write each frame to the player itself, under a
 * time budget, and dropped the connection when a slow player made the budget
 * run out. The focus controller took the drop for a dead player: teardown,
 * mode 2, mode 1. On the car on 2026-09-16 every such recovery left the phone
 * streaming 3.3 fps and resetting USB 122-123 s later, four sessions running --
 * and the trigger was only ever a stall, never a dead player.
 *
 * Now gal's thread only links frames in here and wakes the controller, which
 * sends them with non-blocking writes and waits for the socket to become
 * writable. Nothing times out and no frame is abandoned half sent. A slow
 * player just leaves frames queued, which holds back its ACKs, which makes the
 * phone wait: the phone's own window bounds the queue. The cap is a backstop.
 */
typedef struct vc_frame {
    struct vc_frame *next;
    size_t len;
    size_t off;
    long long queued_us;
    unsigned char data[1];
} vc_frame;

#define VC_QUEUE_MAX_FRAMES 64u
#define VC_QUEUE_MAX_BYTES  ((size_t)4u * 1024u * 1024u)

static vc_frame *g_q_head = NULL;
static vc_frame *g_q_tail = NULL;
static unsigned g_q_count = 0u;
static size_t g_q_bytes = 0u;
static unsigned long g_q_overflows = 0ul;
static int g_wake_fd[2] = { -1, -1 };
static long long g_last_idr_ms = 0;
static unsigned long g_idr_count = 0ul;

static long long stream_now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000LL + (long long)(ts.tv_nsec / 1000L);
}

/* Caller holds g_lock. */
static void queue_free_locked(void)
{
    while (g_q_head != NULL) {
        vc_frame *f = g_q_head;
        g_q_head = f->next;
        free(f);
    }
    g_q_tail = NULL;
    g_q_count = 0u;
    g_q_bytes = 0u;
}

/* Caller holds g_lock. The controller polls the read end. */
static void wake_ensure_locked(void)
{
    if (g_wake_fd[0] >= 0) return;
    if (pipe(g_wake_fd) != 0) {
        g_wake_fd[0] = -1;
        g_wake_fd[1] = -1;
        return;
    }
    (void)set_nonblocking(g_wake_fd[0]);
    (void)set_nonblocking(g_wake_fd[1]);
    (void)fcntl(g_wake_fd[0], F_SETFD, FD_CLOEXEC);
    (void)fcntl(g_wake_fd[1], F_SETFD, FD_CLOEXEC);
}

/* Caller holds g_lock. */
static void wake_locked(void)
{
    wake_ensure_locked();
    if (g_wake_fd[1] >= 0) {
        char b = 1;
        (void)write(g_wake_fd[1], &b, 1u);   /* a full pipe means it is awake anyway */
    }
}

/* Caller holds g_lock. */
static void queue_link_locked(vc_frame *f)
{
    if (g_q_tail != NULL) g_q_tail->next = f;
    else g_q_head = f;
    g_q_tail = f;
    ++g_q_count;
    g_q_bytes += f->len;
    wake_locked();
}

/*
 * Controller thread, caller holds g_lock. Sends what the socket takes right
 * now and returns without waiting. Returns 1 while data is still queued.
 */
static int queue_flush_locked(void)
{
    while (g_q_head != NULL && g_client_fd >= 0) {
        vc_frame *f = g_q_head;
        ssize_t n = send(g_client_fd, f->data + f->off, f->len - f->off, MSG_NOSIGNAL);
        if (n > 0) {
            f->off += (size_t)n;
            if (f->off < f->len) continue;
            {
                unsigned long us = (unsigned long)(stream_now_us() - f->queued_us);
                g_timing_sum_us += us;
                if (us > g_timing_max_us) g_timing_max_us = us;
                ++g_timing_sends;
            }
            g_sent_bytes += (unsigned long)f->len;
            g_q_head = f->next;
            if (g_q_head == NULL) g_q_tail = NULL;
            --g_q_count;
            g_q_bytes -= f->len;
            free(f);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            ++g_timing_eagain;
            return 1;
        }
        drop_client(n == 0 ? "eof" : "send_error", errno);
        return 0;
    }
    return g_q_head != NULL;
}

/*
 * gal's reader thread: hand a frame to the controller. *delivered = 1 once it
 * is queued for the player; the focus controller's ACK accounting counts it
 * from then on. Never blocks on the player.
 */
static int enqueue_frame(const unsigned char *p, size_t bytes, int *delivered)
{
    vc_frame *f;
    int is_idr = contains_idr_nal(p, bytes);
    long long now_ms = stream_now_ms();

    f = (vc_frame *)malloc(sizeof *f + bytes);
    if (f == NULL) return -1;
    f->next = NULL;
    f->len = bytes;
    f->off = 0u;
    f->queued_us = stream_now_us();
    memcpy(f->data, p, bytes);

    pthread_mutex_lock(&g_lock);
    if (ensure_listening() != 0 || g_client_fd < 0) {
        if (g_client_fd < 0) {
            ++g_frames_no_client;
            if (g_frames_no_client == 1ul || g_frames_no_client % 300ul == 0ul)
                gal_hook_logf("event=stream.write result=no_client frames=%lu bytes=%lu",
                              g_frames_no_client, (unsigned long)bytes);
        }
        pthread_mutex_unlock(&g_lock);
        free(f);
        return -1;
    }
    if (is_idr) {
        /* How often the phone sends a keyframe on its own decides whether a
         * replaced player could resume without a focus cycle. */
        ++g_idr_count;
        gal_hook_logf("event=stream.idr count=%lu bytes=%lu since_last_ms=%lld",
                      g_idr_count, (unsigned long)bytes,
                      g_last_idr_ms != 0 ? now_ms - g_last_idr_ms : -1LL);
        g_last_idr_ms = now_ms;
    }
    if (!g_client_has_keyframe) {
        if (!is_idr) {
            pthread_mutex_unlock(&g_lock);
            free(f);
            return 0;                      /* held back until a keyframe */
        }
        g_client_has_keyframe = 1;
    }
    if (g_q_count >= VC_QUEUE_MAX_FRAMES || g_q_bytes + bytes > VC_QUEUE_MAX_BYTES) {
        /* A frame missing mid-stream would smear every frame after it, so
         * wait for the next keyframe rather than decode on. */
        ++g_q_overflows;
        g_client_has_keyframe = 0;
        if (g_q_overflows <= 5ul || g_q_overflows % 100ul == 0ul)
            gal_hook_logf("event=stream.queue result=overflow frames=%u bytes=%lu total=%lu action=drop_await_keyframe",
                          g_q_count, (unsigned long)g_q_bytes, g_q_overflows);
        pthread_mutex_unlock(&g_lock);
        free(f);
        return -1;
    }
    queue_link_locked(f);
    pthread_mutex_unlock(&g_lock);
    if (delivered != NULL) *delivered = 1;
    return 0;
}

/* Caller holds g_lock. Never blocks: if nobody is waiting, we stay unconnected. */
static void try_accept(void)
{
    int fd;
    if (g_client_fd >= 0 || g_listen_fd < 0) return;
    fd = accept(g_listen_fd, NULL, NULL);
    if (fd < 0) return;                       /* EWOULDBLOCK: nobody there */
    if (set_nonblocking(fd) != 0) { close(fd); return; }
    {
        int buf_size = 2097152; /* 2MB buffer for large I-frames */
        int actual_snd = 0;
        int actual_rcv = 0;
        socklen_t len;
        (void)setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buf_size, sizeof buf_size);
        (void)setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buf_size, sizeof buf_size);
        if (stream_use_tcp()) {
            int one = 1;
            /* Frames are latency-critical and mostly ~1 KB: never wait to
             * coalesce them. */
            (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        }
        /*
         * What the kernel actually granted, which is nothing like the request:
         * QNX clamps this to about 125 KB, and the largest IDR measured on the
         * car is 139 KB. The real size decides how many drain rounds a frame
         * needs, so record it rather than assume.
         */
        len = sizeof actual_snd;
        if (getsockopt(fd, SOL_SOCKET, SO_SNDBUF, &actual_snd, &len) != 0) actual_snd = 0;
        len = sizeof actual_rcv;
        if (getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &actual_rcv, &len) != 0) actual_rcv = 0;
        g_sndbuf_actual = actual_snd > 0 ? (size_t)actual_snd : 0u;
        gal_hook_logf("event=stream.sockbuf requested=%d sndbuf=%d rcvbuf=%d",
                      buf_size, actual_snd, actual_rcv);
    }
    (void)fcntl(fd, F_SETFD, FD_CLOEXEC);

    g_client_fd = fd;
    ++g_accepts;
    ++g_client_gen;
    g_sent_bytes = 0ul;
    g_dropped_bytes = 0ul;
    g_dropped_chunks = 0ul;
    gal_hook_logf("event=stream.client result=connected accepts=%lu", g_accepts);

    if (hook_fix_enabled(HOOK_FIX_NO_IDR_REPLAY) && fc_enabled()) {
        /*
         * fix=no_idr_replay. Focus control grants mode 1 only after this
         * accept, and the phone answers mode 1 with codec config and a fresh
         * IDR, so nothing cached earlier is valid for this client.
         */
        g_client_has_keyframe = 0;
        gal_hook_log("event=stream.client action=await_phone_config fix=no_idr_replay");
        return;
    }

    /*
     * Replay cached SPS/PPS codec configuration immediately upon connection.
     * This eliminates the race condition where stream-player connects after
     * handleCodecConfig was dispatched, allowing FFmpeg to identify the stream
     * and initialize its decoder without waiting for a new keyframe.
     */
    if (g_sps_pps_len > 0u) {
        if (send_all_timeout(g_client_fd, g_sps_pps_cache, g_sps_pps_len, 1000) == 0) {
            g_sent_bytes += (unsigned long)g_sps_pps_len;
            gal_hook_logf("event=stream.client action=replayed_codec_config bytes=%u", (unsigned)g_sps_pps_len);
        } else {
            drop_client("replay_codec_config_failed", errno);
            return;
        }
    }
    /*
     * Do not replay the cached IDR on the Unix-socket client. The player can
     * connect immediately after playback starts, and the cached access unit
     * may only be the phone's AA startup/icon frame. Let the decoder receive
     * codec config, then wait for the next live IDR; P-frames remain gated
     * below until that fresh IDR arrives.
     */
    g_client_has_keyframe = 0;
    if (g_keyframe_len > 0u)
        gal_hook_logf("event=stream.client action=withhold_cached_keyframe bytes=%u reason=await_live_idr",
                      (unsigned)g_keyframe_len);
}

void vc_stream_out_begin_stream(void)
{
    pthread_mutex_lock(&g_lock);
    /*
     * An IDR is only valid for the stream/session that produced it. Replaying
     * the previous phone session cached IDR lets FFmpeg start immediately,
     * but its following P-frames reference a different encoder state and show
     * corruption until the next current-session IDR. Keep any codec config
     * already delivered for the new session, but require a fresh IDR.
     */
    drop_client("new_stream", 0);
    g_keyframe_len = 0u;
    g_client_has_keyframe = 0;
    gal_hook_log("event=stream.bootstrap result=reset reason=new_playback_session");
    pthread_mutex_unlock(&g_lock);
}

void vc_stream_out_set_codec_config(const void *data, size_t bytes)
{
    int changed;
    if (data == NULL || bytes == 0u || bytes > sizeof(g_sps_pps_cache)) return;
    pthread_mutex_lock(&g_lock);
    changed = (bytes != g_sps_pps_len ||
               memcmp(g_sps_pps_cache, data, bytes) != 0);
    memcpy(g_sps_pps_cache, data, bytes);
    g_sps_pps_len = bytes;
    if (changed) {
        g_keyframe_len = 0u;
        g_client_has_keyframe = 0;
    }
    gal_hook_logf("event=stream.codec_config result=cached bytes=%u", (unsigned)bytes);
    /* If a client is already connected, forward the new codec config immediately */
    if (g_client_fd >= 0 && fc_enabled()) {
        /* Through the queue: it keeps its place ahead of the keyframe that
         * follows, and gal's thread never blocks on the player. */
        vc_frame *f = (vc_frame *)malloc(sizeof *f + g_sps_pps_len);
        if (f != NULL) {
            f->next = NULL;
            f->len = g_sps_pps_len;
            f->off = 0u;
            f->queued_us = stream_now_us();
            memcpy(f->data, g_sps_pps_cache, g_sps_pps_len);
            queue_link_locked(f);
        }
    } else if (g_client_fd >= 0) {
        /* Same budget as a frame: this runs on gal's reader thread. */
        if (send_all_timeout(g_client_fd, g_sps_pps_cache, g_sps_pps_len,
                             VC_WRITE_BUDGET_MS) == 0) {
            g_sent_bytes += (unsigned long)g_sps_pps_len;
        } else {
            drop_client("codec_config_send_failed", errno);
        }
    }
    pthread_mutex_unlock(&g_lock);
}

/* Caller holds g_lock. */
static void drop_client(const char *reason, int err)
{
    if (g_client_fd < 0) return;
    close(g_client_fd);
    g_client_fd = -1;
    g_client_has_keyframe = 0;
    queue_free_locked();   /* queued frames are credited by the controller's loss path */
    gal_hook_logf("event=stream.client result=disconnected reason=%s errno=%d sent=%lu dropped=%lu",
                  reason, err, g_sent_bytes, g_dropped_bytes);
}

/*
 * An explicit open clears the hard-failure latch first. ensure_listening sets
 * it so the per-frame write path cannot retry-storm, but a caller asking for
 * the listener (init, or the focus controller while it waits) must be able to
 * recover from a bind that lost a race with a stale socket.
 */
int vc_stream_out_open(void)
{
    int rc;
    pthread_mutex_lock(&g_lock);
    g_open_failed = 0;
    rc = ensure_listening();
    pthread_mutex_unlock(&g_lock);
    return rc;
}

int vc_stream_out_listening(void)
{
    int listening;
    pthread_mutex_lock(&g_lock);
    listening = (g_listen_fd >= 0);
    pthread_mutex_unlock(&g_lock);
    return listening;
}

int vc_stream_out_write(const void *data, size_t bytes)
{
    return vc_stream_out_write_ex(data, bytes, NULL);
}

/* *delivered = 1 only when the whole frame reached the client. */
int vc_stream_out_write_ex(const void *data, size_t bytes, int *delivered)
{
    const unsigned char *p = (const unsigned char *)data;
    size_t left = bytes;
    int rc = 0;
    /* fc_enabled(), not just the flag: if the controller failed to start we
     * are on the legacy path, where nothing makes the phone send a fresh IDR. */
    int no_replay = hook_fix_enabled(HOOK_FIX_NO_IDR_REPLAY) && fc_enabled();
    int timing = hook_fix_enabled(HOOK_FIX_STREAM_TIMING);
    int is_idr = 0;
    struct timespec t0;

    t0.tv_sec = 0;
    t0.tv_nsec = 0;
    if (delivered != NULL) *delivered = 0;
    if (data == NULL || bytes == 0u) return 0;
    if (fc_enabled()) return enqueue_frame(p, bytes, delivered);

    pthread_mutex_lock(&g_lock);

    /* fix=no_idr_replay: no IDR cache, and the frame is scanned only at the
     * keyframe gate below. */
    if (!no_replay) is_idr = contains_idr_nal(p, bytes);
    /* Retain latest IDR Keyframe for instant client bootstrap */
    if (is_idr) {
        if (bytes <= sizeof(g_keyframe_cache)) {
            memcpy(g_keyframe_cache, data, bytes);
            g_keyframe_len = bytes;
        } else {
            /* Do not leave an older IDR available for replay. Following
             * frames may reference this newer decoder reset point, so an old
             * cached IDR would not be a valid bootstrap for a late client. */
            g_keyframe_len = 0u;
            gal_hook_logf("event=stream.keyframe result=dropped_oversize bytes=%u max=%u",
                          (unsigned)bytes, (unsigned)sizeof(g_keyframe_cache));
        }
        /* Inline SPS/PPS belongs to this IDR and supersedes stale config. */
        extract_inline_sps_pps(p, bytes);
    }

    if (ensure_listening() != 0) { pthread_mutex_unlock(&g_lock); return -1; }
    try_accept();
    if (g_client_fd < 0) {
        /*
         * No consumer attached, so this frame goes nowhere. Say so, or a
         * capture shows the secondary happily receiving and forwarding
         * frames with nothing to indicate the far end never existed.
         * Rate limited because this is normal whenever player is not running.
         */
        ++g_frames_no_client;
        if (g_frames_no_client == 1ul || g_frames_no_client % 300ul == 0ul)
            gal_hook_logf("event=stream.write result=no_client frames=%lu bytes=%lu",
                          g_frames_no_client, (unsigned long)bytes);
        pthread_mutex_unlock(&g_lock);
        return -1;
    }

    /*
     * Keyframe Gate:
     * Never forward P-frames to a client that hasn not received an IDR keyframe yet.
     * Prevents macroblock mosaic / decoding corruptions during initial connection.
     */
    if (!g_client_has_keyframe) {
        if (no_replay) is_idr = contains_idr_nal(p, bytes);
        if (is_idr) {
            g_client_has_keyframe = 1;
        } else {
            pthread_mutex_unlock(&g_lock);
            return 0; /* Withhold pre-keyframe delta frame */
        }
    }

    if (timing) clock_gettime(CLOCK_MONOTONIC, &t0);
    {
        size_t sent_here = 0u;
        /* One budget per drain round the frame needs, not per frame. */
        long rounds = (g_sndbuf_actual != 0u && bytes > g_sndbuf_actual) ?
            (long)(bytes / g_sndbuf_actual) + 1L : 1L;
        long budget_ms = VC_WRITE_BUDGET_MS * rounds;
        long long deadline;
        if (budget_ms > VC_WRITE_BUDGET_MAX_MS) budget_ms = VC_WRITE_BUDGET_MAX_MS;
        deadline = stream_now_ms() + budget_ms;
        while (left > 0u) {
            ssize_t n = send(g_client_fd, p, left, MSG_NOSIGNAL);
            if (n > 0) {
                p += (size_t)n; left -= (size_t)n; sent_here += (size_t)n;
                continue;
            }
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && (errno == EWOULDBLOCK || errno == EAGAIN)) {
                struct timeval tv;
                fd_set wfds;
                int sret;
                long long remaining = deadline - stream_now_ms();
                ++g_timing_eagain;
                if (remaining <= 0) {
                    g_dropped_bytes += (unsigned long)left;
                    g_dropped_chunks += 1ul;
                    gal_hook_logf("event=stream.write result=timeout bytes=%u sent=%u budget_ms=%ld sndbuf=%u",
                                  (unsigned)bytes, (unsigned)sent_here, budget_ms,
                                  (unsigned)g_sndbuf_actual);
                    drop_client(sent_here > 0u ? "partial_write_timeout" : "write_timeout",
                                errno);
                    rc = -1;
                    break;
                }
                FD_ZERO(&wfds);
                FD_SET(g_client_fd, &wfds);
                /*
                 * What is left of this frame's whole budget, not a fresh
                 * 250 ms for every round of a slow drain.
                 */
                tv.tv_sec = 0;
                tv.tv_usec = (long)(remaining * 1000LL);
                sret = select(g_client_fd + 1, NULL, &wfds, NULL, &tv);
                if (sret > 0 && FD_ISSET(g_client_fd, &wfds)) continue;
                g_dropped_bytes += (unsigned long)left;
                g_dropped_chunks += 1ul;
                drop_client(sent_here > 0u ?
                            "partial_write_timeout" : "write_timeout",
                            errno);
                rc = -1;
                break;
            }
            drop_client(n == 0 ? "eof" : "send_error", errno);
            rc = -1;
            break;
        }
    }
    if (rc == 0) {
        g_sent_bytes += (unsigned long)bytes;
        if (delivered != NULL) *delivered = 1;
    }
    if (timing) {
        struct timespec t1;
        unsigned long us;
        clock_gettime(CLOCK_MONOTONIC, &t1);
        us = (unsigned long)((t1.tv_sec - t0.tv_sec) * 1000000L +
                             (t1.tv_nsec - t0.tv_nsec) / 1000L);
        g_timing_sum_us += us;
        if (us > g_timing_max_us) g_timing_max_us = us;
        ++g_timing_sends;
    }
    pthread_mutex_unlock(&g_lock);
    return rc;
}

unsigned vc_stream_out_port(void)
{
    return stream_use_tcp() ? (unsigned)stream_port() : 0u;
}

const char *vc_stream_out_path(void)
{
    return VC_STREAM_SOCK_PATH;
}

const char *vc_stream_out_url(void)
{
    static char url[64];
    if (!stream_use_tcp()) return "unix://" VC_STREAM_SOCK_PATH;
    (void)snprintf(url, sizeof url, "tcp://127.0.0.1:%u", (unsigned)stream_port());
    return url;
}

int vc_stream_out_is_connected(void)
{
    int c;
    pthread_mutex_lock(&g_lock);
    c = (g_client_fd >= 0);
    pthread_mutex_unlock(&g_lock);
    return c;
}

unsigned vc_stream_out_queue_depth(void)
{
    unsigned n;
    pthread_mutex_lock(&g_lock);
    n = g_q_count;
    pthread_mutex_unlock(&g_lock);
    return n;
}

unsigned long vc_stream_out_accept_count(void)
{
    unsigned long n;
    pthread_mutex_lock(&g_lock);
    n = g_accepts;
    pthread_mutex_unlock(&g_lock);
    return n;
}

void vc_stream_out_require_keyframe(void)
{
    pthread_mutex_lock(&g_lock);
    g_client_has_keyframe = 0;
    pthread_mutex_unlock(&g_lock);
}

unsigned long vc_stream_out_take_timing(unsigned long *avg_us, unsigned long *max_us,
                                        unsigned long *eagain)
{
    unsigned long sends;
    pthread_mutex_lock(&g_lock);
    sends = g_timing_sends;
    *avg_us = sends != 0ul ? g_timing_sum_us / sends : 0ul;
    *max_us = g_timing_max_us;
    *eagain = g_timing_eagain;
    g_timing_sends = 0ul;
    g_timing_sum_us = 0ul;
    g_timing_max_us = 0ul;
    g_timing_eagain = 0ul;
    pthread_mutex_unlock(&g_lock);
    return sends;
}

/*
 * One controller-thread wait: sleep in poll() until a player connects to
 * either socket, sends an ACK, closes its video connection, or timeout_ms
 * passes; then accept, notice a closed video client, and read ACKs.
 * The player never writes on the video socket, so readable means EOF.
 */
void vc_stream_out_wake(void)
{
    pthread_mutex_lock(&g_lock);
    wake_locked();
    pthread_mutex_unlock(&g_lock);
}

typedef struct conn_state {
    unsigned long accepts;
    unsigned long ack_accepts;
    int client;
    int ack_client;
} conn_state;

/* Caller holds g_lock; the ACK fields are the controller thread's own. */
static void conn_snapshot_locked(conn_state *s)
{
    s->accepts = g_accepts;
    s->ack_accepts = g_ack_accepts;
    s->client = g_client_fd >= 0;
    s->ack_client = g_ack_client_fd >= 0;
}

static int conn_changed(const conn_state *before)
{
    conn_state after;
    pthread_mutex_lock(&g_lock);
    conn_snapshot_locked(&after);
    pthread_mutex_unlock(&g_lock);
    return after.accepts != before->accepts || after.ack_accepts != before->ack_accepts ||
           after.client != before->client || after.ack_client != before->ack_client;
}

int vc_stream_out_wait(int timeout_ms, vc_ack_callback_fn cb)
{
    struct pollfd fds[4];
    nfds_t n = 0;
    int stream_fd;
    unsigned long stream_gen;
    int stream_index = -1;
    int wake_index = -1;
    int pending;
    conn_state before;

    if (g_ack_listen_fd < 0) (void)vc_stream_out_ack_init();
    pthread_mutex_lock(&g_lock);
    conn_snapshot_locked(&before);
    /* Send first: whatever the socket cannot take makes this poll wait for
     * POLLOUT instead of a timeout. */
    pending = queue_flush_locked();
    stream_fd = g_client_fd;
    stream_gen = g_client_gen;
    wake_ensure_locked();
    if (g_client_fd < 0 && g_listen_fd >= 0) {
        fds[n].fd = g_listen_fd;
        fds[n].events = POLLIN;
        fds[n].revents = 0;
        ++n;
    }
    if (g_wake_fd[0] >= 0) {
        wake_index = (int)n;
        fds[n].fd = g_wake_fd[0];
        fds[n].events = POLLIN;
        fds[n].revents = 0;
        ++n;
    }
    pthread_mutex_unlock(&g_lock);
    if (stream_fd >= 0) {
        stream_index = (int)n;
        fds[n].fd = stream_fd;
        fds[n].events = pending ? (POLLIN | POLLOUT) : POLLIN;
        fds[n].revents = 0;
        ++n;
    }
    if (g_ack_client_fd >= 0 || g_ack_listen_fd >= 0) {
        fds[n].fd = g_ack_client_fd >= 0 ? g_ack_client_fd : g_ack_listen_fd;
        fds[n].events = POLLIN;
        fds[n].revents = 0;
        ++n;
    }
    if (timeout_ms < 0) timeout_ms = 0;
    if (n == 0 || (poll(fds, n, timeout_ms) < 0 && errno != EINTR)) {
        if (timeout_ms > 0) usleep((useconds_t)timeout_ms * 1000u);
    }
    if (wake_index >= 0 && (fds[wake_index].revents & POLLIN) != 0) {
        char drain[64];
        while (read(fds[wake_index].fd, drain, sizeof drain) > 0) {
        }
    }

    pthread_mutex_lock(&g_lock);
    if (g_listen_fd >= 0) try_accept();
    if (stream_index >= 0 && (fds[stream_index].revents & POLLNVAL) != 0) {
        /* gal's thread closed it while we were in poll(), which returns at
         * once: sleep rather than spin until the next snapshot. */
        pthread_mutex_unlock(&g_lock);
        usleep(10000);
        vc_stream_out_poll_ack(cb);
        return 1;
    }
    /* The same descriptor number is not the same connection: check the accept
     * generation too, or a recycled fd reads as a disconnect. */
    if (stream_index >= 0 &&
        (fds[stream_index].revents & (POLLIN | POLLHUP | POLLERR)) != 0 &&
        g_client_fd >= 0 && g_client_fd == stream_fd && g_client_gen == stream_gen) {
        unsigned char scratch[64];
        ssize_t r = recv(g_client_fd, scratch, sizeof scratch, 0);
        if (r == 0)
            drop_client("eof", 0);
        else if (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
            drop_client("recv_error", errno);
    }
    (void)queue_flush_locked();
    pthread_mutex_unlock(&g_lock);
    vc_stream_out_poll_ack(cb);
    return conn_changed(&before);
}

/* ---------------- Player-ACK Feedback Pipe ---------------- */

int vc_stream_out_ack_init(void)
{
    struct sockaddr_un addr;
    const char *sock_path = VC_ACK_SOCK_PATH;

    if (g_ack_listen_fd >= 0) return 0;

    g_ack_listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (g_ack_listen_fd < 0) {
        gal_hook_logf("event=ack.open result=failed reason=socket errno=%d", errno);
        return -1;
    }
    if (set_nonblocking(g_ack_listen_fd) != 0) {
        close(g_ack_listen_fd); g_ack_listen_fd = -1;
        return -1;
    }
    if (remove_stale_socket(sock_path) != 0 && errno == EADDRINUSE) {
        gal_hook_logf("event=ack.open result=failed reason=socket_in_use path=%s", sock_path);
        close(g_ack_listen_fd); g_ack_listen_fd = -1;
        return -1;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);

    if (bind(g_ack_listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        gal_hook_logf("event=ack.open result=failed reason=bind path=%s errno=%d", sock_path, errno);
        close(g_ack_listen_fd); g_ack_listen_fd = -1;
        return -1;
    }
    if (listen(g_ack_listen_fd, 1) != 0) {
        gal_hook_logf("event=ack.open result=failed reason=listen path=%s errno=%d", sock_path, errno);
        close(g_ack_listen_fd); g_ack_listen_fd = -1;
        return -1;
    }
    (void)fcntl(g_ack_listen_fd, F_SETFD, FD_CLOEXEC);
    gal_hook_logf("event=ack.open result=success path=%s", sock_path);
    return 0;
}

void vc_stream_out_ack_close(void)
{
    if (g_ack_client_fd >= 0) {
        close(g_ack_client_fd);
        g_ack_client_fd = -1;
    }
    if (g_ack_listen_fd >= 0) {
        close(g_ack_listen_fd);
        g_ack_listen_fd = -1;
        if (g_server_pid != 0 && getpid() == g_server_pid) {
            (void)unlink(VC_ACK_SOCK_PATH);
        }
    }
}

int vc_stream_out_ack_client_connected(void)
{
    return (g_ack_client_fd >= 0);
}

unsigned long vc_stream_out_ack_accept_count(void)
{
    return g_ack_accepts;
}

void vc_stream_out_poll_ack(vc_ack_callback_fn cb)
{
    unsigned char buf[64];
    ssize_t n;

    if (g_ack_listen_fd < 0) {
        if (vc_stream_out_ack_init() != 0) return;
    }

    if (g_ack_client_fd < 0) {
        int cfd = accept(g_ack_listen_fd, NULL, NULL);
        if (cfd >= 0) {
            if (set_nonblocking(cfd) == 0) {
                (void)fcntl(cfd, F_SETFD, FD_CLOEXEC);
                g_ack_client_fd = cfd;
                ++g_ack_accepts;
                gal_hook_logf("event=ack.client result=connected accepts=%lu", g_ack_accepts);
            } else {
                close(cfd);
            }
        }
    }

    if (g_ack_client_fd >= 0) {
        while ((n = recv(g_ack_client_fd, buf, sizeof(buf), 0)) > 0) {
            if (cb) cb((unsigned)n);
        }
        if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
            close(g_ack_client_fd);
            g_ack_client_fd = -1;
            gal_hook_log("event=ack.client result=disconnected");
        }
    }
}

void vc_stream_out_close(void)
{
    pthread_mutex_lock(&g_lock);
    drop_client("shutdown", 0);
    if (g_listen_fd >= 0) {
        close(g_listen_fd);
        g_listen_fd = -1;
        if (!stream_use_tcp() && g_server_pid != 0 && getpid() == g_server_pid) {
            (void)unlink(VC_STREAM_SOCK_PATH);
        }
    }
    g_open_failed = 0;
    g_sps_pps_len = 0u;
    g_keyframe_len = 0u;
    g_client_has_keyframe = 0;
    if (g_wake_fd[0] >= 0) {
        close(g_wake_fd[0]);
        close(g_wake_fd[1]);
        g_wake_fd[0] = -1;
        g_wake_fd[1] = -1;
    }
    pthread_mutex_unlock(&g_lock);

    vc_stream_out_ack_close();
    gal_hook_log("event=stream.close result=success");
}
