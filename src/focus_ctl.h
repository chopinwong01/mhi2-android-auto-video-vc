#ifndef FOCUS_CTL_H
#define FOCUS_CTL_H

/*
 * Secondary video-focus flow control (GAL_FOCUS_CONTROL).
 *
 * The secondary sink is held in video focus mode 2 (native: the cluster is
 * not showing projection) until stream-player runs and is connected, and
 * only then granted mode 1. The phone streams only while granted, so no
 * frame is ACKed without being rendered, and the first frame a player
 * receives is the phone's own fresh IDR. A lost player sends the secondary
 * back to mode 2 before it is replaced.
 *
 * gal's reader thread only posts events and runs queued focus writes. The
 * controller thread owns the player process, the socket accepts and the ACK
 * pipe, and is the only place that spawns, waits or blocks. Session end is
 * ProtocolEndpointBase's channel-open byte at sink+0x04 going 1 -> 0: only
 * onChannelOpened sets it and only onChannelClosed clears it.
 */

typedef void (*fc_set_focus_fn)(void *sink, int mode, int unconstrained);
typedef void (*fc_frames_rendered_fn)(unsigned count);

typedef struct fc_ops {
    fc_set_focus_fn set_focus;             /* stock VideoSink::setVideoFocus */
    fc_frames_rendered_fn frames_rendered; /* ACK this many frames to the phone */
} fc_ops;

int  fc_enabled(void);            /* controller running */
void fc_notify(void);             /* any thread: have the controller step now */
void fc_write_lock(void);         /* serialise writes to the secondary endpoint */
void fc_write_unlock(void);
void fc_start(void *secondary_sink, const fc_ops *ops);
void fc_shutdown(void);

/* gal's reader thread */
void fc_setup_begin(void *sink);
void fc_setup_end(void *sink);
int  fc_filter_focus(void *sink, int *mode);  /* 1 = setup-time mode 1 held native */
/* Answer the phone's focus request, honouring mode 2; returns the mode
 * written, or -1 when focus control is not running. */
int  fc_request_reply(void *sink, int requested);
void fc_playback_start(void *sink);
int  fc_playback_stop(void *sink);            /* 1 = follows our own mode 2 */
void fc_frame_delivered(void);
void fc_run_mailbox(void);

/* diagnostics: render ACKs since the last call, their send->ACK latency */
unsigned fc_take_latency(unsigned *avg_ms, unsigned *max_ms, unsigned *unacked);
const char *fc_state_name(void);

#endif
