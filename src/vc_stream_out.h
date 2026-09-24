#ifndef VC_STREAM_OUT_H
#define VC_STREAM_OUT_H

#define VC_STREAM_SOCK_PATH "/tmp/gal_video.sock"

#include <stddef.h>

/*
 * Forwards the secondary sink's H.264 elementary stream out of gal over a
 * local Unix domain socket (/tmp/gal_video.sock), so a separate process can
 * decode and display it.
 *
 * Every call is non-blocking and every failure is silent-but-counted. A
 * hook running inside gal must never stall or kill the process it is
 * living in. With focus control, gal's thread only queues a frame and the
 * controller thread sends it when the socket takes it; a reader that falls
 * behind leaves frames queued (the phone's ACK window bounds that), and only
 * past the queue cap are frames dropped, until the next keyframe.
 */

int  vc_stream_out_open(void);                       /* idempotent; retries a failed bind */
int  vc_stream_out_listening(void);                   /* is the video listener up? */
void vc_stream_out_begin_stream(void);                /* reset per-session bootstrap */
int  vc_stream_out_write(const void *data, size_t bytes);
int  vc_stream_out_write_ex(const void *data, size_t bytes, int *delivered);
void vc_stream_out_require_keyframe(void);            /* gate P-frames until the next IDR */
unsigned long vc_stream_out_accept_count(void);
unsigned vc_stream_out_queue_depth(void);            /* frames queued, not yet sent */
/* fix=stream_timing: sends since the last call, their avg/max send time and EAGAIN waits */
unsigned long vc_stream_out_take_timing(unsigned long *avg_us, unsigned long *max_us,
                                        unsigned long *eagain);
void vc_stream_out_set_codec_config(const void *data, size_t bytes);
void vc_stream_out_close(void);
int  vc_stream_out_is_connected(void);
const char *vc_stream_out_path(void);
const char *vc_stream_out_url(void);
unsigned vc_stream_out_port(void);

typedef void (*vc_ack_callback_fn)(unsigned rendered_count);
int  vc_stream_out_ack_init(void);
void vc_stream_out_ack_close(void);
void vc_stream_out_poll_ack(vc_ack_callback_fn cb);
int  vc_stream_out_ack_client_connected(void);
unsigned long vc_stream_out_ack_accept_count(void);
/* Controller thread only: poll() both sockets, then accept, detect EOF, read
 * ACKs. Returns 1 when a player connected or went away on either socket. */
int  vc_stream_out_wait(int timeout_ms, vc_ack_callback_fn cb);
void vc_stream_out_wake(void);                        /* any thread: end the controller's wait */

#endif
