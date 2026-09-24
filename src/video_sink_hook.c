/*
 * P4521-specific Android Auto secondary-display experiment.
 *
 * The old GAL executable is a fixed-address ARM ET_EXEC image.  This hook
 * creates a second VideoSink and a second CVideoSinkCallbackHandler. Secondary
 * frames are ACKed in-hook and forwarded as raw H.264 Annex-B over a Unix
 * socket to an external stream-player process for decode and display.
 */
#include "gal_hook.h"
#include "vc_stream_out.h"
#include "vc_player_mgr.h"
#include "focus_ctl.h"

#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define GAL_CONTROLLER_INSTANCE_ADDRESS ((uintptr_t)0x00237f70u)
#define GAL_VIDEO_SINK_IMPL_CTOR_ADDRESS ((uintptr_t)0x001d9658u)
#define GAL_VIDEO_SINK_IMPL_VTABLE       ((uintptr_t)0x00231e10u)
/*
 * The object VideoSink keeps at +0x38 is a gal::CVideoSinkCallbackHandler,
 * NOT a gal::CVideoSinkImpl. VideoSink calls it purely virtually --
 * handleSetup invokes vtable+0x14, playbackStart +0x18, playbackStop +0x1c
 * (from libautoreceiver.so, decompiled) -- so what belongs there is an
 * IVideoSinkCallbacks implementor. GAL_VIDEO_SINK_IMPL_CTOR_ADDRESS builds
 * exactly that: it writes five vptrs, all pointing inside
 * _ZTVN3gal25CVideoSinkCallbackHandlerE (0x00231d18, size 188) at +8, +0x58,
 * +0x6c, +0x78 and +0x94 -- a multiply-inheriting class. Its primary vptr is
 * therefore 0x00231d20, which is exactly what the primary sink's +0x38 object
 * reports on-car.
 */
#define GAL_VIDEO_SINK_CBHANDLER_VTABLE  ((uintptr_t)0x00231d20u)

/*
 * Object sizes. Both are verified rather than assumed, because a value
 * smaller than the real class means GAL's own code writes past the end of
 * our heap block -- corruption that would surface anywhere but here.
 *
 * VIDEO_SINK_IMPL_SIZE is the historical macro name, but the object is a
 * CVideoSinkCallbackHandler. Its size is exact: GAL uses `mov r0, #240`
 * immediately before `bl 0x1d9658` (the constructor this hook calls), i.e.
 * operator new(0xf0). tools/verify_constants.sh checks that instruction pair
 * still says 240.
 *
 * VIDEO_SINK_SIZE is a bound, not an exact size: VideoSink has no
 * exported constructor, so there is no allocation to read. Disassembling
 * every VideoSink and MediaSinkBase method and taking the highest
 * this-relative access gives 0x44, so 0x48 would suffice and 0x50 leaves
 * margin. The hook's own highest write is +0x4c.
 */
#define VIDEO_SINK_SIZE 0x50u
#define VIDEO_SINK_IMPL_SIZE 0xf0u
#define VIDEO_CODEC_RESOLUTION_800_480 1

/* (GalReceiver *this, ProtocolEndpointBase *endpoint) -- see the hook's
 * own definition below for why both parameters are mandatory. */
typedef int (*register_service_fn)(void *, void *);
typedef void (*add_configuration_fn)(void *, int, int, int, int, int, int, int);
typedef void (*add_discovery_fn)(void *, void *);
typedef int (*handle_setup_fn)(void *, int);
typedef int (*handle_media_configuration_fn)(void *, int);
typedef void (*playback_fn)(void *, int);
typedef void (*handle_data_fn)(void *, uint64_t, const void *, unsigned);
typedef int (*handle_video_focus_request_fn)(void *, const void *);
typedef void *(*video_sink_impl_ctor_fn)(void *, void *, void *, void *);
/* MessageRouter::queueOutgoingUnencrypted(this, unsigned char channel,
 * void *buf, unsigned int len) -- mangled EhPvj. */
typedef void (*queue_unencrypted_fn)(void *, unsigned char, void *, unsigned);
/* MessageRouter::routeMessage(this, unsigned char channel,
 * shared_ptr<IoBuffer> const&) -- mangled EhRK10shared_ptrI8IoBufferE. */
typedef void (*route_message_fn)(void *, unsigned char, const void *);

static register_service_fn g_register_service;
static add_configuration_fn g_add_configuration;
static add_discovery_fn g_add_discovery;
static handle_setup_fn g_handle_setup;
static handle_media_configuration_fn g_handle_media_configuration;
static add_discovery_fn g_input_add_discovery;
static playback_fn g_playback_start;
static playback_fn g_playback_stop;
static handle_data_fn g_handle_data;
/* VideoSink::handleCodecConfig(this, void *data, unsigned len) -- from the
 * disassembly at 0xfa854 it forwards (data, len) untouched to handler
 * vtable slot 4, so r1/r2 are the parameter-set bytes and their length. */
typedef void (*handle_codec_config_fn)(void *, void *, unsigned);
typedef void (*set_video_focus_fn)(void *, int, int);
/* MediaSinkBase::ackFrames(this, int session, unsigned count) -- prologue
 * at 0xec294 in libautoreceiver.so keeps r0/r1/r2 as this/session/count.
 * Exported, so the hook can call it directly. */
typedef int (*ack_frames_fn)(void *, int, unsigned);
static handle_codec_config_fn g_handle_codec_config;
static handle_video_focus_request_fn g_handle_video_focus_request;
static set_video_focus_fn g_set_video_focus;
static ack_frames_fn g_ack_frames;
typedef void (*send_version_fn)(void *);
static send_version_fn g_send_version_request;
/* MediaSinkBase::sendConfig(this, int) -- 0xec748 in libautoreceiver.so.
 * Emits the AVChannelSetupResponse (message id 0x8003) that carries the
 * configuration index list. Reached through the PLT (R_ARM_JUMP_SLOT at
 * 0x104778), so it is interposable. Declared int and the value forwarded:
 * if the real one is void, r0 is passed through unchanged either way. */
typedef int (*send_config_fn)(void *, int);
static send_config_fn g_send_config;
/* VideoSink::getNumberOfConfigurations() -- 0xfa95c, vtable slot +0x3c.
 * Returns ([this+0x44] - [this+0x40]) >> 3. Exported, so dlsym it rather
 * than reopening the vector layout here. */
typedef unsigned (*config_count_fn)(void *);
static config_count_fn g_config_count;
/* MessageRouter::handleChannelOpenReq(this, unsigned char channel,
 * ChannelOpenRequest const &) -- 0xecc4c, R_ARM_JUMP_SLOT at 0x1042bc. */
typedef int (*channel_open_fn)(void *, unsigned char, const void *);
static channel_open_fn g_handle_channel_open;
static unsigned long g_secondary_acks;
static unsigned long g_undelivered_credits;  /* fix=ack_rendered_only */
static unsigned long g_primary_frames;

static void *g_primary_sink;
static void *g_primary_impl;
static void *g_secondary_sink;
/*
 * The primary VideoSink identifies itself, so this hook does not have to
 * guess at CGALController's layout: GAL calls
 * VideoSink::addSupportedConfiguration on it (four times, one per
 * supported resolution/fps pair) immediately before handing that same
 * object to GalReceiver::registerService. Confirmed on-car across two
 * independent boots -- the object address changes every boot, the
 * ordering does not.
 */
static void *g_pending_primary_sink;
static void *g_secondary_impl;
static unsigned g_secondary_service_id;
static unsigned long g_secondary_frames;
static unsigned long g_secondary_bytes;

/*
 * Off unless explicitly enabled, so a build carrying this code behaves
 * exactly as before on a unit that has not opted in. Read once and cached:
 * this sits on the per-frame path, and getenv on QNX walks the environment
 * block on every call.
 */
static int stream_forward_enabled(void)
{
    static int cached = -1;
    if (cached < 0) {
        const char *v = getenv("GAL_STREAM_ENABLE");
        cached = (v == NULL || *v == '\0' ||
                  (strcmp(v, "0") != 0 && strcmp(v, "false") != 0 &&
                   strcmp(v, "no") != 0)) ? 1 : 0;
        gal_hook_logf("event=stream.mode enabled=%d", cached);
    }
    return cached;
}
static struct timespec g_secondary_first_frame_time;
/*
 * Set when the secondary's playbackStart is accepted, so the no-frame
 * watchdog below can say how long the phone has been silent. Zeroed on stop.
 */
static struct timespec g_secondary_start_time;
static int g_secondary_no_frame_warned;
static int g_secondary_focus_mode = -1;
static int g_secondary_registered;
static int g_build_attempted;
static int g_secondary_started;
static queue_unencrypted_fn g_queue_unencrypted;
static route_message_fn g_route_message;
static unsigned long g_channel_seen[256];
static void **g_controller_instance_slot;
static uintptr_t g_resolved_impl_vtable;
static uintptr_t g_resolved_cbhandler_vtable;
static int g_firmware_check_attempted;
static int g_firmware_verified;

static void on_frames_rendered(unsigned count);

/* Focus-control writes, which call the stock function past this hook. */
static void fc_write_focus(void *sink, int mode, int unconstrained)
{
    g_secondary_focus_mode = mode;
    g_set_video_focus(sink, mode, unconstrained);
}

static uint32_t load_u32(const void *base, size_t offset)
{
    uint32_t value;
    memcpy(&value, (const unsigned char *)base + offset, sizeof(value));
    return value;
}

static unsigned load_u8(const void *base, size_t offset)
{
    return *((const unsigned char *)base + offset);
}

static void store_u32(void *base, size_t offset, uint32_t value)
{
    memcpy((unsigned char *)base + offset, &value, sizeof(value));
}

static void store_u8(void *base, size_t offset, unsigned value)
{
    *((unsigned char *)base + offset) = (unsigned char)value;
}

static unsigned endpoint_service_id(const void *endpoint)
{
    return endpoint == NULL ? 0xffu : *((const unsigned char *)endpoint + 0x0c);
}

static void *resolve_symbol(const char *name, void **slot)
{
    if (*slot == NULL) *slot = dlsym(RTLD_NEXT, name);
    return *slot;
}

static int resolve_runtime(void)
{
    (void)resolve_symbol("_ZN11GalReceiver15registerServiceEP20ProtocolEndpointBase",
                         (void **)&g_register_service);
    (void)resolve_symbol("_ZN9VideoSink25addSupportedConfigurationEiiiiiii",
                         (void **)&g_add_configuration);
    (void)resolve_symbol("_ZN9VideoSink16addDiscoveryInfoEP24ServiceDiscoveryResponse",
                         (void **)&g_add_discovery);
    if (g_register_service == NULL || g_add_configuration == NULL ||
        g_add_discovery == NULL) {
        gal_hook_logf("event=abi.resolve result=failed register=%p add_config=%p discovery=%p",
                      (void *)g_register_service, (void *)g_add_configuration,
                      (void *)g_add_discovery);
        return -1;
    }
    return 0;
}

static int verify_firmware_abi(void)
{
    void *vtable_symbol;
    void *cb_vtable_symbol;
    if (g_firmware_check_attempted) return g_firmware_verified ? 0 : -1;
    g_firmware_check_attempted = 1;
    g_controller_instance_slot = (void **)dlsym(
        RTLD_DEFAULT, "_ZN3gal14CGALController10s_instanceE");
    vtable_symbol = dlsym(RTLD_DEFAULT, "_ZTVN3gal14CVideoSinkImplE");
    cb_vtable_symbol = dlsym(RTLD_DEFAULT,
                             "_ZTVN3gal25CVideoSinkCallbackHandlerE");
    if ((uintptr_t)cb_vtable_symbol != GAL_VIDEO_SINK_CBHANDLER_VTABLE - 8u ||
        (uintptr_t)g_controller_instance_slot !=
            GAL_CONTROLLER_INSTANCE_ADDRESS ||
        (uintptr_t)vtable_symbol != GAL_VIDEO_SINK_IMPL_VTABLE - 8u) {
        /*
         * Report all three, including the callback-handler vtable, which
         * was checked but never printed -- so a mismatch there produced a
         * failure message showing only values that looked correct.
         */
        gal_hook_logf(
            "event=firmware.verify result=failed controller_symbol=%p expected_controller=0x%08x impl_vtable_symbol=%p expected_impl=0x%08x cbhandler_vtable_symbol=%p expected_cbhandler=0x%08x action=leave_stock_gal_untouched",
            (void *)g_controller_instance_slot,
            (unsigned)GAL_CONTROLLER_INSTANCE_ADDRESS, vtable_symbol,
            (unsigned)(GAL_VIDEO_SINK_IMPL_VTABLE - 8u), cb_vtable_symbol,
            (unsigned)(GAL_VIDEO_SINK_CBHANDLER_VTABLE - 8u));
        return -1;
    }
    g_resolved_impl_vtable = (uintptr_t)vtable_symbol + 8u;
    g_resolved_cbhandler_vtable = (uintptr_t)cb_vtable_symbol + 8u;
    g_firmware_verified = 1;
    gal_hook_logf("event=firmware.verify result=success target=MHI2_P4521 controller_symbol=%p impl_vtable=0x%08x hidden_impl_ctor=0x%08x",
                  (void *)g_controller_instance_slot,
                  (unsigned)g_resolved_impl_vtable,
                  (unsigned)GAL_VIDEO_SINK_IMPL_CTOR_ADDRESS);
    return 0;
}

static int is_primary_video_sink(void *endpoint, void **controller_out)
{
    void *controller;
    int match;

    if (verify_firmware_abi() != 0) {
        if (controller_out != NULL) *controller_out = NULL;
        return 0;
    }
    controller = *g_controller_instance_slot;
    if (controller_out != NULL) *controller_out = controller;
    if (controller == NULL) {
        gal_hook_debugf("event=primary.check controller=NULL endpoint=%p", endpoint);
        return 0;
    }
    /*
     * This used to compare the endpoint against *(controller + 0x88).
     * That was wrong and could never match: on-car, that slot reads
     * 0x002300d4 -- byte-identical across reboots even though the
     * controller itself moves -- and 0x002300d4 lies inside
     * _ZTVN3gal14CGALControllerE (0x0022ff88 + 424). It is a secondary
     * vptr for one of CGALController's own multiple-inheritance base
     * subobjects, not a pointer to anything, let alone the sink.
     *
     * The addSupportedConfiguration ordering above is used instead. It
     * needs no offset into GAL's objects at all, so it cannot rot the
     * same way; it is also type-safe by construction, since
     * addSupportedConfiguration is a VideoSink member function and
     * nothing else can be its `this`.
     */
    match = endpoint != NULL && endpoint == g_pending_primary_sink;
    gal_hook_debugf("event=primary.check controller=%p pending_primary=%p endpoint=%p match=%s",
                    controller, g_pending_primary_sink, endpoint,
                    match ? "yes" : "no");
    return match;
}

static unsigned select_service_id(void *router)
{
    const gal_secondary_config *config = gal_hook_config();
    unsigned id;

    if (config->service_id != 0u) return config->service_id & 0xffu;
    if (router == NULL) return 0xffu;
    /* Stock services occupy low IDs and some register after VideoSink. */
    for (id = 0x40u; id < 0xffu; ++id) {
        if (load_u32(router, ((size_t)id + 0x40u) * 4u) == 0u) return id;
    }
    return 0xffu;
}


static size_t encode_varint(unsigned char *buf, uint32_t val)
{
    size_t len = 0;
    while (val >= 0x80u) {
        buf[len++] = (unsigned char)((val & 0x7fu) | 0x80u);
        val >>= 7;
    }
    buf[len++] = (unsigned char)(val & 0x7fu);
    return len;
}

static size_t build_insets_payload(unsigned char *out, size_t max_out,
                                   uint32_t top, uint32_t bottom,
                                   uint32_t left, uint32_t right,
                                   uint32_t ui_theme)
{
    unsigned char insets[16];
    size_t insets_len = 0;
    unsigned char uiconfig[24];
    size_t uiconfig_len = 0;
    size_t total_len = 0;

    if (top > 0u) {
        insets[insets_len++] = 0x08; /* tag 1: top */
        insets_len += encode_varint(insets + insets_len, top);
    }
    /*
     * Protobuf convention: omitting an integer field defaults it to 0.
     * Omitting bottom when bottom==0 saves 2 bytes (0x10 0x00), allowing
     * left=150 and right=150 (2-byte varints) to fit into 14B <= 15B SSO!
     */
    if (bottom > 0u) {
        insets[insets_len++] = 0x10; /* tag 2: bottom */
        insets_len += encode_varint(insets + insets_len, bottom);
    }
    if (left > 0u) {
        insets[insets_len++] = 0x18; /* tag 3: left */
        insets_len += encode_varint(insets + insets_len, left);
    }
    if (right > 0u) {
        insets[insets_len++] = 0x20; /* tag 4: right */
        insets_len += encode_varint(insets + insets_len, right);
    }

    /*
     * UiConfig field 2 becomes DisplayParams.contentInsets on the phone.
     * Field 1 is deliberately absent: ipq.c() uses field 1 to shrink the
     * encoded content rectangle, which produces black borders.
     */
    uiconfig[uiconfig_len++] = 0x12; /* tag 2: UI content safe area */
    uiconfig_len += encode_varint(uiconfig + uiconfig_len, (uint32_t)insets_len);
    memcpy(uiconfig + uiconfig_len, insets, insets_len);
    uiconfig_len += insets_len;
    /* UiConfig field 4 is UiTheme: 0=automatic, 1=light, 2=dark/night. */
    uiconfig[uiconfig_len++] = 0x20; /* tag 4: ui_theme, wire type varint */
    uiconfig[uiconfig_len++] = (unsigned char)(ui_theme <= 2u ? ui_theme : 0u);

    out[total_len++] = 0x5a; /* tag 11: UiConfig */
    total_len += encode_varint(out + total_len, (uint32_t)uiconfig_len);
    if (total_len + uiconfig_len > max_out) {
        return 0; /* Exceeds SSO capacity */
    }
    memcpy(out + total_len, uiconfig, uiconfig_len);
    total_len += uiconfig_len;
    return total_len;
}

static void apply_video_config_insets(void *vconf, const gal_secondary_config *config, const char *context_tag)
{
    unsigned char *obj;
    unsigned char payload[16];
    size_t payload_len;
    uint32_t top = (config != NULL) ? config->insets_top : 40u;
    uint32_t bottom = (config != NULL) ? config->insets_bottom : 40u;
    uint32_t left = (config != NULL) ? config->insets_left : 127u;
    uint32_t right = (config != NULL) ? config->insets_right : 127u;
    uint32_t ui_theme = (config != NULL) ? config->ui_theme : 2u;
    uint32_t viewing_distance = (config != NULL) ? config->viewing_distance : 700u;

    if (vconf == NULL) return;

    /* Set Field 7 (viewing_distance) at offset +0x40 and enable has_bit 6 (0x40) */
    if (viewing_distance > 0u) {
        store_u32(vconf, 0x40u, viewing_distance);
        store_u32(vconf, 0x20u, load_u32(vconf, 0x20u) | 0x40u);
        gal_hook_logf("event=video.viewing_distance result=success tag=%s value=%u",
                      context_tag, viewing_distance);
    }

    if (load_u32(vconf, 0x1cu) > 15u || load_u32(vconf, 0x18u) != 0u) {
        gal_hook_logf("event=video.insets result=skipped tag=%s reason=string_not_sso_or_not_empty vconf=%p len=%u cap=%u",
                      context_tag, vconf, load_u32(vconf, 0x18u), load_u32(vconf, 0x1cu));
        return;
    }

    if (config != NULL && config->ui_config_payload_len > 0u) {
        payload_len = config->ui_config_payload_len;
        memcpy(payload, config->ui_config_payload, payload_len);
    } else {
        payload_len = build_insets_payload(payload, 15u, top, bottom, left, right,
                                           ui_theme);
    }

    if (payload_len > 0u && payload_len <= 15u) {
        obj = (unsigned char *)vconf;
        memcpy(obj + 0x08, payload, payload_len);
        obj[0x08 + payload_len] = 0;
        store_u32(vconf, 0x18u, (uint32_t)payload_len);
        gal_hook_logf("event=video.insets result=success tag=%s top=%u bottom=%u left=%u right=%u len=%zu",
                      context_tag, top, bottom, left, right, payload_len);
    }
}

static void set_video_config_ui_insets(void *sink, const gal_secondary_config *config)
{
    uintptr_t vec_base;
    void *vconf;
    uint32_t fps_val;

    if (sink == NULL) return;
    vec_base = load_u32(sink, 0x40u);
    if (vec_base == 0u) return;

    vconf = (void *)(uintptr_t)load_u32((void *)vec_base, 4u);
    if (vconf == NULL) return;

    fps_val = load_u32(vconf, 0x2cu);
    /* In protobuf schema, fps is enum (VIDEO_FPS_30 = 2), not integer 30 */
    if (load_u32(vconf, 0x28u) != VIDEO_CODEC_RESOLUTION_800_480 ||
        (fps_val != 2u && fps_val != 30u)) {
        gal_hook_logf("event=video.insets result=mismatch vconf=%p res=%u fps=%u",
                      vconf, load_u32(vconf, 0x28u), fps_val);
        return;
    }

    apply_video_config_insets(vconf, config, "build_sink");
}

static int build_secondary_sink(void *primary, void *controller)
{
    const gal_secondary_config *config = gal_hook_config();
    video_sink_impl_ctor_fn impl_ctor;
    void *router;
    void *renderer_creator;
    void *config_vector;
    /*
     * Static, not stack-local: build_secondary_sink() runs at most once
     * (guarded by g_build_attempted below), and the hidden impl_ctor's
     * handling of the config-vector pointer -- copy vs. retain -- is a
     * reverse-engineered assumption, not something this file can verify.
     * A static buffer removes the dangling-pointer risk if it retains.
     */
    static unsigned char secondary_config_entry[40];
    /*
     * Static for exactly the reason secondary_config_entry above is: the
     * constructor is handed this vector and it is not known whether it
     * copies or retains. The entry was made static on that reasoning, but
     * the vector POINTING AT IT was left on the stack -- so if the ctor
     * retains, the impl ends up holding a pointer into a stack frame that
     * has already returned. That is a use-after-return producing sporadic
     * corruption somewhere else entirely, which is the worst failure this
     * project could have on a car.
     *
     * Two elements, written once, never freed: static costs eight bytes
     * and removes the question.
     */
    static uint32_t secondary_config_vector[2];
    uint32_t *reference_count = NULL;
    int result;
    unsigned offset;
    unsigned index;
    unsigned primary_config_count;
    void *primary_config_data;
    int found_video_config = 0;
    unsigned patched_displayable = 0u;

    if (g_build_attempted) return g_secondary_registered ? 0 : -1;
    g_build_attempted = 1;
    if (!gal_hook_is_enabled()) {
        gal_hook_log("event=secondary.build result=skipped reason=disabled");
        return -1;
    }
    if (!gal_hook_second_sink_enabled()) {
        gal_hook_logf("event=secondary.build result=skipped reason=second_sink_disabled");
        return -1;
    }
    if (resolve_runtime() != 0) return -1;

    router = (void *)(uintptr_t)load_u32(primary, 0x08u);
    /*
     * primary+0x38 is the primary sink's CVideoSinkCallbackHandler. This
     * used to be validated against the CVideoSinkImpl vtable, which it is
     * not and never was: on-car it reported 0x00231d20
     * (CVideoSinkCallbackHandler + 8) against an expected 0x00231e10, and
     * build_secondary_sink() bailed here every time. The construction
     * below was already correct -- only this check was wrong.
     */
    g_primary_impl = (void *)(uintptr_t)load_u32(primary, 0x38u);
    if (router == NULL || g_primary_impl == NULL ||
        load_u32(g_primary_impl, 0u) != g_resolved_cbhandler_vtable) {
        gal_hook_logf("event=abi.verify result=failed router=%p primary_cbhandler=%p vtable=0x%08x expected=0x%08x",
                      router, g_primary_impl,
                      g_primary_impl == NULL ? 0u : load_u32(g_primary_impl, 0u),
                      (unsigned)g_resolved_cbhandler_vtable);
        return -1;
    }

    g_secondary_service_id = select_service_id(router);
    if (g_secondary_service_id == 0xffu) {
        gal_hook_log("event=secondary.build result=failed reason=no_free_service_id");
        return -1;
    }

    g_secondary_sink = calloc(1, VIDEO_SINK_SIZE);
    g_secondary_impl = calloc(1, VIDEO_SINK_IMPL_SIZE);
    reference_count = (uint32_t *)calloc(1, sizeof(*reference_count));
    if (g_secondary_sink == NULL || g_secondary_impl == NULL ||
        reference_count == NULL) {
        /*
         * Release whichever succeeded and clear the globals. Leaving
         * g_secondary_sink pointing at a half-built object would make
         * every `sink == g_secondary_sink` test in this file compare
         * against something that was never registered with GAL.
         */
        free(g_secondary_sink);
        free(g_secondary_impl);
        free(reference_count);
        g_secondary_sink = NULL;
        g_secondary_impl = NULL;
        gal_hook_log("event=secondary.build result=failed reason=out_of_memory");
        return -1;
    }

    /* VideoSink base state. The vtable/router come from the verified primary. */
    store_u32(g_secondary_sink, 0x00u, load_u32(primary, 0x00u));
    store_u32(g_secondary_sink, 0x08u, (uint32_t)(uintptr_t)router);
    store_u8(g_secondary_sink, 0x0cu, g_secondary_service_id);
    store_u32(g_secondary_sink, 0x14u, 3u);  /* H.264 */
    store_u32(g_secondary_sink, 0x18u, 0xffffffffu);
    store_u32(g_secondary_sink, 0x1cu, 8u);
    store_u8(g_secondary_sink, 0x30u, 1u); /* request startup video focus */
    store_u32(g_secondary_sink, 0x4cu, config->viewing_distance);

    /*
     * The callback-handler constructor copies both the configuration vector
     * and the controller-owned renderer-creator shared pointer. The handler
     * and its embedded CRunStateManager are independent, but the creator is
     * not: CVideoRendererCreator::create stores its single renderer pointer at
     * creator+0xfc. This is why the secondary stock render path is withheld.
     */
    impl_ctor = (video_sink_impl_ctor_fn)GAL_VIDEO_SINK_IMPL_CTOR_ADDRESS;
    primary_config_count = load_u32(g_primary_impl, 0x80u);
    primary_config_data = (void *)(uintptr_t)load_u32(g_primary_impl, 0x84u);
    if (primary_config_count == 0u || primary_config_count > 64u ||
        primary_config_data == NULL) {
        gal_hook_logf("event=secondary.video_config result=failed count=%u data=%p",
                      primary_config_count, primary_config_data);
        goto fail;
    }
    for (index = 0u; index < primary_config_count; ++index) {
        unsigned char *entry = (unsigned char *)primary_config_data + index * 40u;
        if (load_u32(entry, 0u) == VIDEO_CODEC_RESOLUTION_800_480 &&
            load_u32(entry, 4u) == config->fps) {
            memcpy(secondary_config_entry, entry, sizeof(secondary_config_entry));
            store_u32(secondary_config_entry, 16u, config->dpi);
            store_u32(secondary_config_entry, 20u, 0u);
            store_u32(secondary_config_entry, 24u, 0u);
            found_video_config = 1;
            gal_hook_logf("event=secondary.video_config result=success primary_index=%u resolution_enum=%u fps=%u dpi=%u margins=%ux%u",
                          index, load_u32(entry, 0u), load_u32(entry, 4u),
                          config->dpi, 0u, 0u);
            break;
        }
    }
    if (!found_video_config) {
        gal_hook_logf("event=secondary.video_config result=failed reason=no_matching_800x480_fps count=%u requested_fps=%u",
                      primary_config_count, config->fps);
        goto fail;
    }
    secondary_config_vector[0] = 1u;
    secondary_config_vector[1] = (uint32_t)(uintptr_t)secondary_config_entry;
    config_vector = secondary_config_vector;
    renderer_creator = (unsigned char *)controller + 0xc4u;
    (void)impl_ctor(g_secondary_impl, (unsigned char *)controller + 0x94u,
                    config_vector, renderer_creator);
    /*
     * Same class as the primary's +0x38 object: impl_ctor builds a
     * gal::CVideoSinkCallbackHandler, so verify against its vptr. This
     * check previously used the CVideoSinkImpl vtable and rejected a
     * correctly-constructed object -- on-car it reported the object's
     * real vtable 0x00231d20 as a failure.
     */
    if (load_u32(g_secondary_impl, 0u) != g_resolved_cbhandler_vtable) {
        gal_hook_logf("event=secondary.impl result=failed impl=%p vtable=0x%08x expected=0x%08x",
                      g_secondary_impl, load_u32(g_secondary_impl, 0u),
                      (unsigned)g_resolved_cbhandler_vtable);
        goto fail_constructed;
    }

    /*
     * Renderer identity check -- validity-gated.
     *
     * The previous form of this line compared handler+0x60 between the two
     * callback handlers and printed shared=yes in EVERY car run. That was a
     * false positive, and the "the secondary clobbered the primary's
     * renderer" conclusion drawn from it is unsupported.
     *
     * handler+0x60 is an EMBEDDED gal::CRunStateManager sub-object, not a
     * renderer pointer. Its first word is that class's vptr,
     * _ZTVN3gal16CRunStateManagerE + 8 == 0x00232020 -- a link-time
     * constant, identical in every instance -- so comparing it between two
     * handlers could only ever say "yes". gal's dynamic symbol table has
     * _ZTVN3gal16CRunStateManagerE at 0x00232018, and both handlers
     * reported exactly 0x00232020 on-car while genuine heap objects in the
     * same log read 0x2c1828 / 0x30b788 / 0x3c9db0.
     *
     * Which offset does hold a renderer is not known, and guessing another
     * one would just move the lie. Resolve the CRunStateManager vtable at
     * runtime instead and refuse to answer when the slot is that constant.
     * "cannot tell" is worth more than a confident wrong answer.
     */
    {
        void *rsm_vtable = dlsym(RTLD_DEFAULT,
                                 "_ZTVN3gal16CRunStateManagerE");
        uint32_t primary_slot =
            g_primary_impl == NULL ? 0u : load_u32(g_primary_impl, 0x60u);
        uint32_t secondary_slot = load_u32(g_secondary_impl, 0x60u);
        uint32_t embedded_vptr = rsm_vtable == NULL ?
            0u : (uint32_t)((uintptr_t)rsm_vtable + 8u);
        const char *verdict;

        if (g_primary_impl == NULL)
            verdict = "unknown_no_primary";
        else if (embedded_vptr != 0u && (primary_slot == embedded_vptr ||
                                         secondary_slot == embedded_vptr))
            verdict = "invalid_slot_is_embedded_crunstatemanager_vptr";
        else if (primary_slot == 0u || secondary_slot == 0u)
            verdict = "unknown_slot_null";
        else
            verdict = primary_slot == secondary_slot ? "yes" : "no";
        gal_hook_logf("event=secondary.renderer primary_handler=%p primary_slot60=0x%08x secondary_handler=%p secondary_slot60=0x%08x crunstatemanager_vptr=0x%08x shared=%s",
                      g_primary_impl, primary_slot, g_secondary_impl,
                      secondary_slot, embedded_vptr, verdict);
    }

    /*
     * P4521 copies its nine-word renderer settings into +0x34..+0x58.
     * Replace only an exact stock displayable ID in the private secondary
     * copy. If this firmware stores it elsewhere, the hook leaves it alone.
     */
    for (offset = 0x34u; offset <= 0x58u; offset += 4u) {
        uint32_t value = load_u32(g_secondary_impl, offset);
        gal_hook_debugf("event=secondary.impl_config offset=0x%02x value=0x%08x",
                        offset, value);
        if (value == 59u) {
            store_u32(g_secondary_impl, offset, config->displayable_id);
            patched_displayable = 1u;
            gal_hook_logf("event=secondary.output_patch stage=impl offset=0x%02x from=59 to=%u result=success",
                          offset, config->displayable_id);
        }
    }
    if (!patched_displayable) {
        /*
         * Expected on this firmware: the impl never holds a displayable id
         * of its own. This scan is a belt-and-braces check kept because a
         * build that DID store one here would need it.
         */
        gal_hook_debugf("event=secondary.output_patch stage=impl result=not_found expected=yes reason=displayable_selected_at_open action=preserve_primary_route");
    }

    *reference_count = 1u;
    store_u32(g_secondary_sink, 0x34u,
              (uint32_t)(uintptr_t)reference_count);
    store_u32(g_secondary_sink, 0x38u,
              (uint32_t)(uintptr_t)g_secondary_impl);

    /*
     * 800x480 (the only resolution the cluster takes), configured fps,
     * zero outer margins, configured DPI, decoder depth 3 and square pixels.
     */
    /* Pass decoder_depth=3 for phone MediaCodec 3-frame pipelining */
    g_add_configuration(g_secondary_sink,
        VIDEO_CODEC_RESOLUTION_800_480, (int)config->fps,
        0, 0,
        (int)config->dpi, 3, (int)config->pixel_aspect_ratio_e4);
    set_video_config_ui_insets(g_secondary_sink, config);

    /*
     * State the result of that call rather than assuming it. This is the
     * number MediaSinkBase::sendConfig will fall back to when the sink's
     * allowed-configuration vector at +0x24 is empty -- which for a
     * calloc'd sink it always is -- and therefore the number of
     * configuration indices the phone receives. Zero here means the phone
     * gets an empty list and answers "No configuration indices."
     */
    {
        unsigned advertised = 0u;
        (void)resolve_symbol("_ZN9VideoSink25getNumberOfConfigurationsEv",
                             (void **)&g_config_count);
        if (g_config_count != NULL) advertised = g_config_count(g_secondary_sink);
        gal_hook_logf("event=secondary.config_advertised count=%u expected=1 result=%s",
                      advertised, advertised == 1u ? "ok" : "UNEXPECTED");
    }

    /*
     * `router` is the same GalReceiver `this` registerService expects: it
     * comes from primary+0x08, and select_service_id() above already
     * probes it as router[(id + 0x40) * 4] -- the exact slot layout the
     * real MessageRouter::registerService indexes as
     * this->table[serviceId + 0x40]. Passing it explicitly rather than
     * relying on the (previously missing) implicit argument.
     */
    result = g_register_service(router, g_secondary_sink);
    g_secondary_registered = result != 0;
    gal_hook_logf(
        "event=secondary.register result=%s service=%u sink=%p impl=%p primary_sink=%p primary_impl=%p independent_sink=%s independent_impl=%s config=%ux%u@%u dpi=%u margins=%ux%u viewing_distance=%u",
        g_secondary_registered ? "success" : "failed",
        g_secondary_service_id, g_secondary_sink, g_secondary_impl,
        g_primary_sink, g_primary_impl,
        g_secondary_sink != g_primary_sink ? "yes" : "no",
        g_secondary_impl != g_primary_impl ? "yes" : "no",
        config->width, config->height, config->fps, config->dpi,
        0, 0,
        config->viewing_distance);
    if (g_secondary_registered) {
        (void)resolve_symbol("_ZN9VideoSink13setVideoFocusEib",
                             (void **)&g_set_video_focus);
        if (hook_fix_enabled(HOOK_FIX_FOCUS_CONTROL) && g_set_video_focus != NULL) {
            fc_ops ops;
            ops.set_focus = fc_write_focus;
            ops.frames_rendered = on_frames_rendered;
            fc_start(g_secondary_sink, &ops);
        }
        return 0;
    }
    /*
     * registerService rejected the endpoint. Deliberately NOT freed: whether
     * the router stored the pointer before failing is a reverse-engineered
     * detail, and a dangling entry in GAL's live routing table is far worse
     * than leaking 0x140 bytes once per process. Clearing the globals is the
     * part that matters.
     */
    g_secondary_sink = NULL;
    g_secondary_impl = NULL;
    return -1;

fail_constructed:
    /*
     * As fail: below, except that impl_ctor has already RUN on
     * g_secondary_impl by the time control reaches here. It was handed
     * controller+0x94 and controller+0xc4 and is documented as copying the
     * renderer-creator shared pointer, so it may have taken a reference or
     * published the object into GAL. free()ing it would be the same
     * dangling-pointer bet the registerService path above declines to make,
     * so it is leaked instead -- once per process, and only on a firmware
     * mismatch that already means the hook cannot work on this unit.
     *
     * The sink and the refcount are different: sink+0x34 and sink+0x38 are
     * not written until after the check that jumps here, so neither has been
     * handed to anything and both are safe to release.
     */
    free(g_secondary_sink);
    free(reference_count);
    g_secondary_sink = NULL;
    g_secondary_impl = NULL;
    return -1;

fail:
    /*
     * Reached only from failures BEFORE registerService, so nothing outside
     * this function ever saw these pointers and they are safe to release.
     *
     * Clearing the globals is the whole point, and the reason this label
     * exists at all: the out-of-memory path above already documented that
     * leaving g_secondary_sink set "would make every `sink ==
     * g_secondary_sink` test in this file compare against something that was
     * never registered with GAL" -- but the three failures after it returned
     * without doing so. VideoSink::setVideoFocus's dual-sink mirror would
     * then hand GAL a sink whose +0x38 handler is still zero from the calloc.
     *
     * Both paths that reach HERE do so before impl_ctor runs, so freeing
     * g_secondary_impl is safe; the one that does not is fail_constructed
     * above.
     */
    free(g_secondary_sink);
    free(g_secondary_impl);
    free(reference_count);
    g_secondary_sink = NULL;
    g_secondary_impl = NULL;
    return -1;
}

/*
 * GalReceiver::registerService(ProtocolEndpointBase*)
 *
 * Non-static C++ member function: the ABI passes `this` (the GalReceiver,
 * whose MessageRouter base is at offset 0 -- GalReceiver::registerService
 * is a plain `b` thunk to MessageRouter::registerService with no
 * this-adjustment, unlike its siblings which `add r0, r0, #0x500` first)
 * in r0 and the endpoint in r1. Both parameters must be declared, and
 * both must be forwarded.
 *
 * Declaring only the endpoint here silently made `receiver` the endpoint
 * and left r1 holding whatever the preceding varargs log call happened to
 * leave there. The real implementation is:
 *
 *     ldrb  r3, [r1, #12]         ; endpoint->serviceId
 *     add   r3, r3, #64
 *     ldr   r2, [r0, r3, lsl #2]  ; this->table[serviceId + 0x40]
 *     streq r1, [r0, r3, lsl #2]  ; table[serviceId] = endpoint
 *
 * so a garbage r1 meant a garbage service id indexing a garbage slot and
 * storing a garbage pointer into GAL's live service routing table, for
 * every service GAL registers. Service discovery then found nothing
 * (START_RECEIVER expired / SERVICE_DISCOVERY_MISSING) and GAL exited -2
 * in a restart loop -- with no Android Auto session, on stock code paths,
 * long before any of this hook's own secondary-sink work could run.
 */
int _ZN11GalReceiver15registerServiceEP20ProtocolEndpointBase(void *receiver,
                                                              void *endpoint)
{
    void *controller = NULL;
    int primary;
    int result;

    if (resolve_runtime() != 0) return 0;
    primary = is_primary_video_sink(endpoint, &controller);
    gal_hook_debugf("event=service.register role=%s receiver=%p endpoint=%p service=%u",
                    primary ? "primary_video" : "other", receiver, endpoint,
                    endpoint_service_id(endpoint));
    result = g_register_service(receiver, endpoint);
    if (primary) {
        g_primary_sink = endpoint;
        gal_hook_logf("event=primary.register result=%s service=%u sink=%p impl=%p",
                      result != 0 ? "success" : "failed",
                      endpoint_service_id(endpoint), endpoint,
                      (void *)(uintptr_t)load_u32(endpoint, 0x38u));
        if (result != 0) (void)build_secondary_sink(endpoint, controller);
    }
    return result;
}

/*
 * Tag GAL's own main InputSourceService with display id 0.
 *
 * A working two-display head unit declares FOUR display-bearing services
 * (decoded from the DHU capture in reference/): cluster video 6:1 7:1,
 * cluster input 5:1, main video 6:0 7:0, and main input 5:0. This hook
 * emits the first three and left the fourth stock, carrying no field 5.
 *
 * That asymmetry is the point. Protobuf optional fields distinguish absent
 * from zero, so once the cluster's input service says 5:1, the main one
 * saying nothing is not "display 0" -- it is an input service that names no
 * display, on a receiver that has just told the phone it has two. The
 * project already measured the mirror image: retagging the CLUSTER's input
 * service in an otherwise working DHU session made the phone stop
 * responding entirely, 83 ping timeouts against 0 in three controls.
 *
 * Same wire bytes as the cluster entry uses, with the id set to 0:
 *     28 00      field 5, varint, display id 0
 */
static void set_input_service_display_id(void *input_service)
{
    unsigned char *object = (unsigned char *)input_service;

    if (input_service == NULL || load_u32(input_service, 0x1cu) >= 0x10u) {
        gal_hook_logf("event=discovery.input_meta result=failed reason=unknown_string_not_sso input_service=%p capacity=%u",
                      input_service,
                      input_service == NULL ? 0u : load_u32(input_service, 0x1cu));
        return;
    }
    if (load_u32(input_service, 0x18u) != 0u) {
        gal_hook_logf("event=discovery.input_meta result=failed reason=unknown_string_not_empty input_service=%p length=%u",
                      input_service, load_u32(input_service, 0x18u));
        return;
    }
    object[0x08] = 0x28u;   /* field 5, varint */
    object[0x09] = 0x00u;   /* display id 0 -- the main screen */
    object[0x0a] = 0u;
    store_u32(input_service, 0x18u, 2u);
    gal_hook_log("event=discovery.input_meta result=success role=main bytes=28:00 display_id=0");
}

static void set_media_sink_unknown_fields(void *media_sink, int secondary)
{
    const gal_secondary_config *config = gal_hook_config();
    unsigned char *object = (unsigned char *)media_sink;

    if (media_sink == NULL || load_u32(media_sink, 0x1cu) >= 0x10u) {
        gal_hook_logf("event=discovery.metadata result=failed role=%s reason=unknown_string_not_sso media_sink=%p capacity=%u",
                      secondary ? "secondary" : "primary", media_sink,
                      media_sink == NULL ? 0u : load_u32(media_sink, 0x1cu));
        return;
    }
    /*
     * This writes fields 6/7 at offset 0x08, not append. Only safe while
     * the unknown-field string is still empty (freshly built response).
     * Refuse to clobber it if that assumption ever stops holding.
     */
    if (load_u32(media_sink, 0x18u) != 0u) {
        gal_hook_logf("event=discovery.metadata result=failed role=%s reason=unknown_string_not_empty media_sink=%p length=%u",
                      secondary ? "secondary" : "primary", media_sink,
                      load_u32(media_sink, 0x18u));
        return;
    }
    object[0x08] = 0x30u;
    object[0x09] = (unsigned char)(secondary ? config->display_id : 0u);
    object[0x0a] = 0x38u;
    object[0x0b] = (unsigned char)(secondary ? 1u : 0u);
    object[0x0c] = 0u;
    store_u32(media_sink, 0x18u, 4u);
    gal_hook_logf("event=discovery.metadata result=success role=%s service=%u bytes=30:%02x:38:%02x display_id=%u display_type=%s",
                  secondary ? "secondary" : "primary",
                  secondary ? g_secondary_service_id : endpoint_service_id(g_primary_sink),
                  object[0x09], object[0x0b], object[0x09],
                  secondary ? "cluster" : "main");
}

/*
 * Append a complete cluster InputSourceService to the discovery response.
 *
 * A DHU capture of a working two-display session (reference/) shows the
 * phone is given one InputSourceService per display, tagged with that
 * display's id in field 5. Subtractively removing the cluster's entry
 * from that working session -- one byte, retagging it so the receiver
 * skips it -- makes the phone stop responding entirely (83 ping timeouts
 * against 0 in three controls). That is the same silence the car shows,
 * so a video sink advertised with no paired input service is very likely
 * why this hook's secondary display is refused.
 *
 * Rather than construct and register a real second InputSource endpoint,
 * this writes the whole Service entry as raw protobuf into the
 * *response's* own unknown-field string. `Service` is field 1 of
 * ServiceDiscoveryResponse and protobuf allows repeated entries anywhere
 * in the stream, so the phone parses it as an ordinary service. The
 * bytes are DHU's, with only the service id changed:
 *
 *     0a 0c                      Service, len 12
 *        08 41                     id = 65 (0x41; DHU used 3)
 *        22 08                     InputSourceService, len 8
 *           0a 04 17 80 80 04        KeycodesSupported = [23, 65536]
 *           28 01                    field 5 = 1  (display id)
 *
 * Keycode 23 is Android's KEYCODE_DPAD_CENTER. No touchscreen or
 * touchpad is declared: DHU's cluster is dpad-only.
 *
 * Known limitation, and the reason this is a probe rather than the final
 * shape: GAL has no endpoint behind this service, so anything the phone
 * sends to it has nowhere to go. It is enough to answer whether the
 * phone will grant a cluster video stream at all; if it does, the
 * endpoint has to be built for real.
 */
static void append_cluster_input_service(void *response)
{
    /*
     * Channel id at index 3 is filled in from the secondary video sink's
     * actual id rather than hardcoded.
     *
     * It used to be a literal 0x41 (65) while the video sink's id comes
     * from select_service_id(), which scans the router for the first free
     * slot from 0x40 upwards. Those agree only as long as 0x40 happens to
     * be free: if it were not, the video sink would take 65 and this entry
     * would declare an input service on the same channel -- a collision in
     * the routing table, discoverable only as traffic arriving at the
     * wrong endpoint.
     */
    unsigned char entry[14] = {
        0x0au, 0x0cu,
            0x08u, 0x41u,
            0x22u, 0x08u,
                0x0au, 0x04u, 0x17u, 0x80u, 0x80u, 0x04u,
                0x28u, 0x01u
    };
    unsigned input_service_id = (g_secondary_service_id + 1u) & 0xffu;
    unsigned char *object = (unsigned char *)response;
    unsigned capacity;
    unsigned length;
    unsigned i;

    if (response == NULL) return;
    if (g_secondary_service_id == 0u || input_service_id == 0u ||
        input_service_id >= 0xffu) {
        gal_hook_logf("event=cluster.input result=failed reason=no_service_id secondary=%u",
                      g_secondary_service_id);
        return;
    }
    entry[3] = (unsigned char)input_service_id;
    capacity = load_u32(response, 0x1cu);
    length = load_u32(response, 0x18u);
    /*
     * Same guards as the media-sink injection: only write into a short
     * string that is still empty, so a real unknown-field payload is
     * never clobbered and the inline buffer can hold all 14 bytes.
     */
    /*
     * Already carrying exactly this entry: a second addDiscoveryInfo call
     * within one discovery round, not a failure. Distinguished from a real
     * conflict so the log does not cry wolf.
     */
    if (length == (unsigned)sizeof(entry) && capacity < 0x10u &&
        memcmp(object + 0x08u, entry, sizeof(entry)) == 0) {
        gal_hook_debugf("event=cluster.input result=already_present response=%p",
                        response);
        return;
    }
    if (capacity >= 0x10u || length != 0u) {
        gal_hook_logf("event=cluster.input result=failed reason=%s response=%p length=%u capacity=%u",
                      capacity >= 0x10u ? "unknown_string_not_sso" : "unknown_string_not_empty",
                      response, length, capacity);
        return;
    }
    for (i = 0u; i < sizeof(entry); ++i) object[0x08u + i] = entry[i];
    object[0x08u + sizeof(entry)] = 0u;
    store_u32(response, 0x18u, (uint32_t)sizeof(entry));
    gal_hook_logf("event=cluster.input result=success service=%u video_service=%u keycodes=23,65536 display_id=1 bytes=%u",
                  input_service_id, g_secondary_service_id,
                  (unsigned)sizeof(entry));
}

/* VideoSink::addDiscoveryInfo(ServiceDiscoveryResponse*) */
void _ZN9VideoSink16addDiscoveryInfoEP24ServiceDiscoveryResponse(
    void *sink, void *response)
{
    unsigned count;
    void *service;
    void *media_sink;
    int secondary;

    if (resolve_runtime() != 0) return;
    g_add_discovery(sink, response);
    if (!gal_hook_is_enabled() ||
        (sink != g_primary_sink && sink != g_secondary_sink)) return;
    count = load_u32(response, 0x2cu);
    if (count == 0u) {
        gal_hook_log("event=discovery.metadata result=failed reason=no_service");
        return;
    }
    service = (void *)(uintptr_t)load_u32(
        (void *)(uintptr_t)load_u32(response, 0x28u), (count - 1u) * 4u);
    if (service == NULL || load_u32(service, 0x58u) != endpoint_service_id(sink)) {
        gal_hook_logf("event=discovery.metadata result=failed reason=service_mismatch sink_service=%u response_service=%u",
                      endpoint_service_id(sink),
                      service == NULL ? 0xffu : load_u32(service, 0x58u));
        return;
    }
    media_sink = (void *)(uintptr_t)load_u32(service, 0x2cu);
    secondary = sink == g_secondary_sink;
    if (!gal_hook_inject_meta_enabled()) {
        gal_hook_logf("event=discovery.metadata result=skipped reason=inject_meta_disabled role=%s service=%u",
                      secondary ? "secondary" : "primary",
                      endpoint_service_id(sink));
        return;
    }
    set_media_sink_unknown_fields(media_sink, secondary);
    if (secondary) {
        uint32_t vconf_count = load_u32(media_sink, 0x44u);
        void **vconf_array = (void **)(uintptr_t)load_u32(media_sink, 0x40u);
        if (vconf_count > 0u && vconf_array != NULL && vconf_array[0] != NULL) {
            apply_video_config_insets(vconf_array[0], gal_hook_config(), "discovery");
        } else {
            gal_hook_logf("event=discovery.vconf_insets result=failed reason=no_vconf media_sink=%p count=%u array=%p",
                          media_sink, vconf_count, vconf_array);
        }
    }
    /*
     * Only alongside the secondary sink: the entry is a sibling of the
     * services GAL builds, not part of this one.
     *
     * Deliberately NOT gated on a once-per-process flag. Service discovery
     * runs again on every phone reconnect against a freshly built
     * response, and a process-wide flag meant the cluster
     * InputSourceService was advertised on the first connection only --
     * after which the phone would stop granting the second display, since
     * that service is what makes it offer one. append_cluster_input_service
     * already refuses to overwrite a non-empty unknown-field string, which
     * is the correct per-response guard and is what the media-sink
     * injection above has always relied on.
     */
    if (secondary && gal_hook_cluster_input_enabled())
        append_cluster_input_service(response);
}

/*
 * GAL's own input service reaches the response through this. Let it run,
 * then tag the entry it just appended.
 *
 * Service member offsets are read from Service::SerializeWithCachedSizes
 * (0x6e168 in libautoreceiver.so), which writes members in field order
 * against the has-bits word at Service+0x20:
 *     has-bit 0x4 -> [service+0x2c]  field 3  MediaSinkService
 *     has-bit 0x8 -> [service+0x30]  field 4  InputSourceService
 */
void _ZN11InputSource16addDiscoveryInfoEP24ServiceDiscoveryResponse(
    void *input_source, void *response)
{
    unsigned count;
    void *service;
    void *input_service;

    (void)resolve_symbol("_ZN11InputSource16addDiscoveryInfoEP24ServiceDiscoveryResponse",
                         (void **)&g_input_add_discovery);
    if (g_input_add_discovery == NULL) return;
    g_input_add_discovery(input_source, response);

    if (!gal_hook_is_enabled() || !gal_hook_inject_meta_enabled()) return;
    if (!gal_hook_main_input_id_enabled()) {
        gal_hook_log("event=discovery.input_meta result=skipped reason=disabled");
        return;
    }
    count = load_u32(response, 0x2cu);
    if (count == 0u) return;
    service = (void *)(uintptr_t)load_u32(
        (void *)(uintptr_t)load_u32(response, 0x28u), (count - 1u) * 4u);
    if (service == NULL) return;
    /* Only touch it if this Service really carries an InputSourceService. */
    if ((load_u32(service, 0x20u) & 0x8u) == 0u) {
        gal_hook_debugf("event=discovery.input_meta result=skipped reason=no_input_service service=%p hasbits=0x%x",
                        service, load_u32(service, 0x20u));
        return;
    }
    input_service = (void *)(uintptr_t)load_u32(service, 0x30u);
    set_input_service_display_id(input_service);
}

void _ZN9VideoSink13setVideoFocusEib(void *sink, int mode, int unconstrained)
{
    (void)resolve_symbol("_ZN9VideoSink13setVideoFocusEib", (void **)&g_set_video_focus);
    if (g_set_video_focus == NULL) return;

    if (sink == g_secondary_sink && fc_enabled()) {
        int requested = mode;
        int held = fc_filter_focus(sink, &mode);
        gal_hook_logf("event=aap.video_focus role=secondary sink=%p mode=%d requested_mode=%d unconstrained=%d action=%s fix=focus_control",
                      sink, mode, requested, unconstrained, held ? "hold_native" : "pass");
    } else {
        gal_hook_logf("event=aap.video_focus role=%s sink=%p mode=%d unconstrained=%d",
                      sink == g_secondary_sink ? "secondary" : "primary", sink, mode, unconstrained);
    }

    if (sink == g_secondary_sink) g_secondary_focus_mode = mode;

    /* Apply the notification to the sink on which GAL issued it. */
    g_set_video_focus(sink, mode, unconstrained);

    /*
     * Lockstep dual-screen synchronization:
     * When apps/gal grants/revokes focus on the primary screen, synchronously
     * apply the exact same focus transition to the secondary cluster screen.
     * Both screens receive focus simultaneously on user tap, and release focus
     * simultaneously on disconnect -- no shortcuts, 100% proper sync.
     */
    /*
     * g_secondary_registered, NOT g_secondary_sink != NULL. build_secondary_sink
     * allocates the sink early and can still fail afterwards; before the fix in
     * that function this test would mirror focus into a half-built object whose
     * +0x38 callback handler is zero, i.e. hand GAL a NULL handler to dispatch
     * through, on the user's first tap. Registration is the real precondition:
     * it is the point at which GAL itself knows about the sink.
     */
    /*
     * Mirroring is OFF by default, because on the car it took focus away
     * from the cluster and the phone stopped the stream. From the
     * 2026-08-28 capture, all within the same millisecond:
     *
     *   role=secondary        mode=1 unconstrained=1   stock grant
     *   role=secondary_synced mode=2 unconstrained=0   this mirror
     *   ...then 0x8002 MediaStopRequest from the phone, ~0.9s later
     *
     * The secondary is granted focus on its own by the stock setup path,
     * so copying the primary's mode over it can only ever contradict that
     * -- and a mode 2 write says "the cluster is not being projected",
     * which is exactly what the phone acted on. Each display owns its own
     * focus on a real dual-screen head unit; there is nothing to sync.
     *
     * Kept behind a switch rather than deleted so the old behaviour can be
     * reproduced for comparison: GAL_DUALSCREEN_FOCUS_MIRROR=1.
     */
    if (sink != g_secondary_sink && g_secondary_registered &&
        gal_hook_focus_mirror_enabled() && !fc_enabled()) {
        g_secondary_focus_mode = mode;
        gal_hook_logf("event=aap.video_focus role=secondary_synced sink=%p mode=%d unconstrained=%d",
                      g_secondary_sink, mode, unconstrained);
        g_set_video_focus(g_secondary_sink, mode, unconstrained);
    } else if (sink != g_secondary_sink && g_secondary_registered) {
        gal_hook_logf("event=aap.video_focus role=secondary_synced action=skipped "
                      "primary_mode=%d reason=%s secondary_keeps=%d",
                      mode, fc_enabled() ? "focus_control" : "mirror_disabled",
                      g_secondary_focus_mode);
    }
}

/*
 * VideoSink::handleVideoFocusRequest(VideoFocusRequestNotification const&)
 *
 * The factory path forwards every sink's request through callback-handler
 * slot 9 into one display-unqualified CDSIAndroidAuto2 callback. LSD then
 * applies it to one process-global center-screen focus state. That is sound
 * with the factory's single VideoSink, but a secondary mode transition can
 * now make LSD believe the center canvas is already native and suppress the
 * later CANVAS_LEFT event that actually opens App-Connect.
 *
 * Keep the primary path byte-for-byte native. For the registered secondary,
 * acknowledge the two valid focus modes on that same sink's AAP channel and
 * do not enqueue the display-unqualified resource-state job. This is not a
 * renderer shortcut: libautoreceiver's native setVideoFocus() only marshals
 * message 0x8008 and queues it on sink+0x05 through the router at sink+0x08.
 * The stock GAL response path ultimately calls that exact function with
 * unsolicited=false. The incoming callback-handler slot records no local
 * state before it posts the global job, so there is no per-sink pending state
 * to bypass; the hook owns g_secondary_focus_mode instead.
 *
 * Unknown modes retain the complete stock path so firmware validation and
 * error behavior are not invented here. Failure to resolve setVideoFocus
 * also falls back to stock rather than leaving the phone without a response.
 */
int _ZN9VideoSink23handleVideoFocusRequestERK29VideoFocusRequestNotification(
    void *sink, const void *request)
{
    unsigned has_bits;
    int mode;
    int reason;

    (void)resolve_symbol(
        "_ZN9VideoSink23handleVideoFocusRequestERK29VideoFocusRequestNotification",
        (void **)&g_handle_video_focus_request);
    if (g_handle_video_focus_request == NULL) return 0;

    if (sink != g_secondary_sink || !g_secondary_registered || request == NULL)
        return g_handle_video_focus_request(sink, request);

    has_bits = load_u32(request, 0x20u);
    mode = (int)load_u32(request, 0x2cu);
    reason = (has_bits & 0x4u) != 0u ?
        (int)load_u32(request, 0x30u) : 0;

    (void)resolve_symbol("_ZN9VideoSink13setVideoFocusEib",
                         (void **)&g_set_video_focus);
    if ((mode != 1 && mode != 2) || g_set_video_focus == NULL) {
        gal_hook_logf(
            "event=aap.video_focus_request role=secondary sink=%p mode=%d reason=%d action=stock_passthrough cause=%s",
            sink, mode, reason,
            g_set_video_focus == NULL ? "set_focus_unresolved" : "unknown_mode");
        return g_handle_video_focus_request(sink, request);
    }

    if (fc_enabled()) {
        int reply = fc_request_reply(sink, mode);
        if (reply >= 0) {
            gal_hook_logf(
                "event=aap.video_focus_request role=secondary sink=%p mode=%d reason=%d reply=%d focus_state=%s action=local_ack fix=focus_control",
                sink, mode, reason, reply, fc_state_name());
            g_secondary_focus_mode = reply;
            return 1;
        }
    }
    gal_hook_logf(
        "event=aap.video_focus_request role=secondary sink=%p mode=%d reason=%d previous=%d action=local_ack isolation=preserve_primary_lsd_state",
        sink, mode, reason, g_secondary_focus_mode);
    g_secondary_focus_mode = mode;
    g_set_video_focus(sink, mode, 0);
    return 1;
}

int _ZN9VideoSink11handleSetupEi(void *sink, int type)
{
    int result;
    (void)resolve_symbol("_ZN9VideoSink11handleSetupEi", (void **)&g_handle_setup);
    if (g_handle_setup == NULL) return -1;
    gal_hook_logf("event=aap.setup role=%s service=%u type=%d selected_config=%d sink=%p impl=%p",
                  sink == g_secondary_sink ? "secondary" : "primary",
                  endpoint_service_id(sink), type,
                  (int)load_u32(sink, 0x18u), sink,
                  (void *)(uintptr_t)load_u32(sink, 0x38u));
    /*
     * Preserve 100% stock setup behavior across all sinks (sink+0x30 untouched).
     * Each display keeps its own AAP focus. Primary requests retain the stock
     * GAL -> LSD path; registered-secondary requests are isolated by the
     * handleVideoFocusRequest interposer above because the LSD callback has no
     * display identifier.
     */
    if (sink == g_secondary_sink) fc_setup_begin(sink);
    result = g_handle_setup(sink, type);
    if (sink == g_secondary_sink) fc_setup_end(sink);
    gal_hook_logf("event=aap.setup.complete role=%s result=%d service=%u",
                  sink == g_secondary_sink ? "secondary" : "primary",
                  result, endpoint_service_id(sink));
    return result;
}

int _ZN9VideoSink24handleMediaConfigurationEi(void *sink, int config)
{
    int result;
    (void)resolve_symbol("_ZN9VideoSink24handleMediaConfigurationEi",
                         (void **)&g_handle_media_configuration);
    if (sink == g_secondary_sink) {
        /*
         * Do NOT forward to GAL's CVideoSinkCallbackHandler. Each sink has a
         * separate handler, but both handlers share the controller-owned
         * CVideoRendererCreator and its one renderer slot at +0xfc. Forwarding
         * this call reconfigures that native render path with the secondary's
         * 800x480 geometry, breaking the primary center screen (1280x720).
         * Store the selected config on the secondary sink and return success.
         */
        store_u32(sink, 0x18u, (uint32_t)config);
        gal_hook_logf("event=aap.media_config role=secondary service=%u requested=%d selected=%d result=0 action=withheld_shared_renderer",
                      endpoint_service_id(sink), config, config);
        return 0;
    }
    if (g_handle_media_configuration == NULL) return -1;
    result = g_handle_media_configuration(sink, config);
    gal_hook_logf("event=aap.media_config role=primary service=%u requested=%d selected=%d result=%d",
                  endpoint_service_id(sink), config,
                  (int)load_u32(sink, 0x18u), result);
    return result;
}

static void ack_secondary_frame(void *sink);
static pthread_t g_ack_thread;
static volatile int g_ack_thread_running = 0;
static struct timespec g_last_ack_rx_time;
static struct timespec g_last_real_ack_rx_time;
static struct timespec g_last_frame_rx_time;

static void on_frames_rendered(unsigned count)
{
    unsigned i;
    clock_gettime(CLOCK_MONOTONIC, &g_last_ack_rx_time);
    clock_gettime(CLOCK_MONOTONIC, &g_last_real_ack_rx_time);
    if (g_secondary_sink) {
        for (i = 0; i < count; ++i) {
            ack_secondary_frame(g_secondary_sink);
        }
    }
}

static void *ack_poll_thread(void *arg)
{
    unsigned tick_counter = 0;
    (void)arg;
    gal_hook_log("event=ack.thread result=started");
    while (g_ack_thread_running) {
        vc_stream_out_poll_ack(on_frames_rendered);
        if (++tick_counter >= 25) { /* 25 * 20ms = 500ms */
            struct timespec now;
            long frame_silence_ms;
            long ack_silence_ms;
            tick_counter = 0;
            clock_gettime(CLOCK_MONOTONIC, &now);
            frame_silence_ms = (now.tv_sec - g_last_frame_rx_time.tv_sec) * 1000 +
                               (now.tv_nsec - g_last_frame_rx_time.tv_nsec) / 1000000;
            ack_silence_ms = (now.tv_sec - g_last_real_ack_rx_time.tv_sec) * 1000 +
                             (now.tv_nsec - g_last_real_ack_rx_time.tv_nsec) / 1000000;
            vc_player_supervisor_tick(frame_silence_ms, ack_silence_ms);
        }
        usleep(20000); /* 20ms poll interval = 50 Hz */
    }
    gal_hook_log("event=ack.thread result=stopped");
    return NULL;
}

void _ZN9VideoSink13playbackStartEi(void *sink, int session)
{
    (void)resolve_symbol("_ZN9VideoSink13playbackStartEi",
                         (void **)&g_playback_start);
    if (g_playback_start == NULL) return;
    gal_hook_logf("event=aap.playback action=start role=%s service=%u session=%d",
                  sink == g_secondary_sink ? "secondary" : "primary",
                  endpoint_service_id(sink), session);
    if (sink == g_secondary_sink) {
        const gal_secondary_config *cfg = gal_hook_config();
        int mode = gal_hook_output_mode();

        /*
         * Recover from a session that ended without playbackStop.
         *
         * On the car the phone can vanish -- USB pulled, or the phone simply
         * stops -- without GAL ever seeing a stop, so the whole teardown path
         * below never runs: the stream-player stays alive holding a socket
         * nothing will write to, and g_secondary_started stays 1. The next
         * connection then starts a second player while the first is still
         * running. playbackStart is the one place that reliably observes a new
         * session, so treat "already started" as the tail of an abrupt
         * disconnect and tear the old one down first.
         */
        if (g_secondary_started && !fc_enabled()) {
            gal_hook_logf("event=aap.playback action=start role=secondary note=previous_session_never_stopped action_taken=cleanup frames=%lu acks=%lu",
                          g_secondary_frames, g_secondary_acks);
            g_secondary_started = 0;
            vc_player_stop();
            vc_stream_out_begin_stream();
        }

        if (!g_ack_thread_running && !fc_enabled()) {
            clock_gettime(CLOCK_MONOTONIC, &g_last_ack_rx_time);
            /*
             * Give a newly spawned player the full supervisor grace period
             * before its first real render ACK. Leaving this timestamp zero
             * makes the first tick interpret system uptime as ACK silence and
             * immediately kill the player.
             */
            g_last_real_ack_rx_time = g_last_ack_rx_time;
            clock_gettime(CLOCK_MONOTONIC, &g_last_frame_rx_time);
            g_ack_thread_running = 1;
            pthread_create(&g_ack_thread, NULL, ack_poll_thread, NULL);
        }

        /*
         * Reset the per-session counters. These only ever incremented, and
         * the cockpit route is triggered on the SECOND frame -- so after
         * the first disconnect the counter sailed past 2 and the route
         * never re-activated. The cluster would work once and then never
         * again, with reconnecting the phone (the natural thing to try)
         * making it permanently worse rather than better. Resetting here
         * also makes the measured-fps figure per session rather than an
         * average since GAL started.
         */
        g_secondary_frames = 0ul;
        g_secondary_bytes = 0ul;
        g_secondary_acks = 0ul;
        memset(&g_secondary_first_frame_time, 0,
               sizeof(g_secondary_first_frame_time));
        /* Watchdog baseline: "started at T, still no frame" is the single
         * most useful line this log can carry, and until now the no-frame
         * case produced no line at all. */
        if (clock_gettime(CLOCK_MONOTONIC, &g_secondary_start_time) != 0)
            memset(&g_secondary_start_time, 0, sizeof(g_secondary_start_time));
        g_secondary_no_frame_warned = 0;
        /*
         * Also the per-channel first-sight counters. They are diagnostic
         * only, but cumulative counters mean the "first inbound on channel
         * N" lines appear for the first session and never again -- so a
         * second session's log reads as though no traffic arrived on the
         * secondary's channel at all, which is exactly the wrong
         * conclusion to hand someone sitting in a car.
         */
        memset(g_channel_seen, 0, sizeof(g_channel_seen));

        gal_hook_logf("event=aap.playback action=start role=secondary output_mode=%s displayable=%u",
                      gal_hook_output_mode_name(), cfg->displayable_id);

        /* Only gal mode leads to a GAL-side secondary open. See above. */

        if (mode == GAL_OUTPUT_WITHHOLD) {
            /*
             * Each callback handler has its own CRunStateManager, but both
             * copy the same controller-owned CVideoRendererCreator shared
             * pointer. CVideoRendererCreator::create allocates 0xc0 bytes and
             * stores one renderer at creator+0xfc. Forwarding this call can
             * therefore replace/reconfigure the primary native render path
             * and blank the main screen. The sink stays registered and the
             * phone keeps streaming, so frames still reach
             * handleDataAvailable; only GAL's render step is withheld.
             */
            g_secondary_started = 1;
            if (fc_enabled())
                fc_playback_start(sink);   /* the controller owns the player */
            else
                vc_player_start();
            gal_hook_logf("event=aap.playback action=start role=secondary result=withheld reason=shared_renderer service=%u session=%d",
                          endpoint_service_id(sink), session);
            return;
        }
    }
    g_playback_start(sink, session);
    if (sink == g_secondary_sink) {
        g_secondary_started = 1;
        vc_player_start();
    }
}

void _ZN9VideoSink12playbackStopEi(void *sink, int session)
{
    (void)resolve_symbol("_ZN9VideoSink12playbackStopEi",
                         (void **)&g_playback_stop);
    if (g_playback_stop == NULL) return;
    gal_hook_logf("event=aap.playback action=stop role=%s service=%u session=%d frames=%lu bytes=%lu",
                  sink == g_secondary_sink ? "secondary" : "primary",
                  endpoint_service_id(sink), session,
                  sink == g_secondary_sink ? g_secondary_frames : 0ul,
                  sink == g_secondary_sink ? g_secondary_bytes : 0ul);
    g_playback_stop(sink, session);
    if (sink == g_secondary_sink && fc_enabled()) {
        /*
         * fix=focus_control. A stop that follows our own mode 2 keeps the
         * player and its sockets for the next grant; the controller handles
         * any other stop. Nothing here waits on gal's reader thread.
         */
        int ours = fc_playback_stop(sink);
        gal_hook_logf("event=aap.playback action=stop role=secondary ours=%d focus_state=%s player=controller fix=focus_control",
                      ours, fc_state_name());
        g_secondary_started = 0;
        memset(&g_secondary_start_time, 0, sizeof(g_secondary_start_time));
        g_secondary_no_frame_warned = 0;
        return;
    }
    if (sink == g_secondary_sink) {
        if (g_ack_thread_running) {
            g_ack_thread_running = 0;
            pthread_join(g_ack_thread, NULL);
        }
        g_secondary_started = 0;
        memset(&g_secondary_start_time, 0, sizeof(g_secondary_start_time));
        g_secondary_no_frame_warned = 0;
        g_secondary_focus_mode = -1;
        vc_player_stop();
        vc_stream_out_begin_stream();
    }
}

/*
 * Recover the encoded payload from the shared_ptr<IoBuffer> GAL passes in.
 * Layout and arithmetic are taken verbatim from the disassembly of the
 * stock handleDataAvailable (reference/iobuffer_layout.md):
 *
 *     IoBuffer* = shared_ptr[+4]
 *     payload   = base(+0) + offset(+8) + length
 *     bytes     = (limit(+12) - offset(+8)) - length
 *
 * The fourth argument is a header length to skip, NOT the payload size.
 * Returns NULL if the buffer is missing or the arithmetic would underflow,
 * which is the only defence available against a layout that has drifted.
 */
static const void *encoded_payload(const void *buffer, unsigned length,
                                   unsigned *out_bytes)
{
    const void *io = NULL;
    uint32_t base, offset, limit;

    *out_bytes = 0u;
    if (buffer == NULL) return NULL;
    memcpy(&io, (const unsigned char *)buffer + 4, sizeof(io));
    if (io == NULL) return NULL;

    base   = load_u32(io, 0);
    offset = load_u32(io, 8);
    limit  = load_u32(io, 12);
    if (base == 0u) return NULL;
    if (limit < offset || (limit - offset) < length) return NULL;

    *out_bytes = (limit - offset) - length;
    if (*out_bytes == 0u) return NULL;
    return (const unsigned char *)(uintptr_t)base + offset + length;
}

/*
 * The whole of an inbound IoBuffer, with no header skipped.
 *
 * encoded_payload() above is the video path and deliberately steps over the
 * media header; routeMessage needs the opposite -- the message exactly as it
 * arrived, so the two-byte AAP message id at the front can be read.
 */
static const void *iobuffer_bytes(const void *buffer, unsigned *out_bytes)
{
    const void *io = NULL;
    uint32_t base, offset, limit;

    *out_bytes = 0u;
    if (buffer == NULL) return NULL;
    memcpy(&io, (const unsigned char *)buffer + 4, sizeof(io));
    if (io == NULL) return NULL;
    base   = load_u32(io, 0);
    offset = load_u32(io, 8);
    limit  = load_u32(io, 12);
    if (base == 0u || limit < offset) return NULL;
    *out_bytes = limit - offset;
    if (*out_bytes == 0u) return NULL;
    return (const unsigned char *)(uintptr_t)base + offset;
}

/*
 * Does the recovered payload actually start an H.264 access unit?
 *
 * encoded_payload() locates the payload by skipping `length` header bytes,
 * an offset taken from the stock disassembly rather than from anything the
 * buffer itself declares. If that ever drifts, the result is still a
 * plausible-looking pointer into a live buffer: the write succeeds, the
 * player receives bytes, and the only symptom is a cluster that stays black.
 * A start code is the one invariant the payload must satisfy, so check it
 * and say so while the log is still short enough to read.
 */
static int payload_is_annex_b(const void *payload, unsigned bytes)
{
    const unsigned char *b = (const unsigned char *)payload;
    if (payload == NULL || bytes < 4u) return 0;
    if (b[0] == 0u && b[1] == 0u && b[2] == 1u) return 1;
    return b[0] == 0u && b[1] == 0u && b[2] == 0u && b[3] == 1u;
}

/*
 * Tell the phone the frame was consumed. GAL normally issues this from its
 * render path, so a secondary that bypasses GAL's renderer must do it
 * itself or the phone stops sending -- the ~17s teardown seen on-car, with
 * audio continuing, is exactly what an unacknowledged video stream looks
 * like. The session id lives at sink+0x18: MediaSinkBase::handleStart
 * stores Start[+40] there, and handleDataAvailable forwards that same word
 * to the callback handler.
 */
static void ack_secondary_frame(void *sink)
{
    int session;
    /*
     * Cache the switch. This runs on every decoded frame, and
     * gal_hook_frame_ack_enabled() is a getenv plus three strcmps each
     * time -- pointless work thirty times a second inside GAL's own
     * receive path, and getenv is not something to lean on repeatedly
     * from a callback thread.
     */
    static int ack_enabled = -1;
    if (ack_enabled < 0) ack_enabled = gal_hook_frame_ack_enabled() ? 1 : 0;
    if (!ack_enabled) return;
    (void)resolve_symbol("_ZN13MediaSinkBase9ackFramesEij",
                         (void **)&g_ack_frames);
    if (g_ack_frames == NULL) {
        /* Separate flag so the ack counter stays a true count. */
        static int warned;
        if (!warned) {
            warned = 1;
            gal_hook_logf("event=own.ack result=unavailable symbol=ackFrames");
        }
        return;
    }
    session = (int)load_u32(sink, 0x18);
    {
        /*
         * ackFrames marshals the Ack and then decides whether to transmit:
         *
         *     ldrb  r0, [r5, #4]      ; gate byte at sink+0x04
         *     cmp   r0, #0
         *     moveq r5, r0            ; gate 0 -> return 0, NOTHING SENT
         *     bne   send              ; gate set -> ldr r0,[r5,#8]  (router)
         *                             ;             ldrb r1,[r5,#5] (channel)
         *
         * A zero gate therefore discards every ack silently while this code
         * happily counts them, and the phone stops streaming once its
         * unacked allowance is used up. That exact failure cost days on the
         * x86 DHU harness, where the same gate lives at +0x08 and the
         * equivalent skip path returns *true*, so nothing in the log
         * distinguished a delivered ack from a discarded one.
         *
         * Log the gate and channel with the first few acks so a car capture
         * answers "were the acks actually sent?" on its own. Do not infer it
         * from the return value: 0 means either gate-off or the send call
         * returning 0, which are different things.
         */
        int rc;
        fc_write_lock();
        rc = g_ack_frames(sink, session, 1u);
        fc_write_unlock();
        ++g_secondary_acks;
        if (g_secondary_acks <= 3ul || g_secondary_acks % 300ul == 0ul)
            gal_hook_logf("event=own.ack count=%lu session=%d rc=%d "
                          "send_gate=+04:%02x channel=+05:%02x",
                          g_secondary_acks, session, rc,
                          (unsigned)load_u8(sink, 0x04),
                          (unsigned)load_u8(sink, 0x05));
    }
}

/* fix=stream_timing: one line per 30 secondary frames. */
static void log_stream_timing(void)
{
    static struct timespec last;
    struct timespec now;
    unsigned long avg_us;
    unsigned long max_us;
    unsigned long eagain;
    unsigned long sends;
    unsigned acked = 0u;
    unsigned ack_avg = 0u;
    unsigned ack_max = 0u;
    unsigned unacked = 0u;
    long window_ms = 0;

    sends = vc_stream_out_take_timing(&avg_us, &max_us, &eagain);
    if (fc_enabled()) acked = fc_take_latency(&ack_avg, &ack_max, &unacked);
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (last.tv_sec != 0)
        window_ms = (long)(now.tv_sec - last.tv_sec) * 1000L +
                    (now.tv_nsec - last.tv_nsec) / 1000000L;
    last = now;
    gal_hook_logf("event=stream.timing frames=%lu window_ms=%ld input_fps=%.1f ack_fps=%.1f sends=%lu send_us_avg=%lu send_us_max=%lu eagain=%lu acked=%u ack_ms_avg=%u ack_ms_max=%u unacked=%u focus_state=%s fix=stream_timing",
                  g_secondary_frames, window_ms,
                  window_ms > 0 ? 30000.0 / (double)window_ms : 0.0,
                  window_ms > 0 ? (double)acked * 1000.0 / (double)window_ms : 0.0,
                  sends, avg_us, max_us, eagain, acked, ack_avg, ack_max, unacked,
                  fc_enabled() ? fc_state_name() : "off");
}

void _ZN9VideoSink17handleCodecConfigEPvj(void *sink, void *data,
                                          unsigned length)
{
    (void)resolve_symbol("_ZN9VideoSink17handleCodecConfigEPvj",
                         (void **)&g_handle_codec_config);
    if (g_handle_codec_config == NULL) return;
    gal_hook_logf("event=aap.codec_config role=%s bytes=%u",
                  sink == g_secondary_sink ? "secondary" : "primary", length);
    if (sink == g_secondary_sink) {
        vc_stream_out_set_codec_config(data, (size_t)length);
        return; /* Never forward secondary codec config to stock GAL hardware decoder! */
    }
    g_handle_codec_config(sink, data, length);
}

void _ZN9VideoSink19handleDataAvailableEyRK10shared_ptrI8IoBufferEj(
    void *sink, uint64_t timestamp, const void *buffer, unsigned length)
{
    (void)resolve_symbol(
        "_ZN9VideoSink19handleDataAvailableEyRK10shared_ptrI8IoBufferEj",
        (void **)&g_handle_data);
    if (g_handle_data == NULL) return;
    /*
     * Count the PRIMARY's frames too. Without this a session where the main
     * screen stayed blank could not be told apart from one where the phone
     * sent no video at all -- the 2026-08-28 capture had both screens dark
     * and no way to say which. The distinction decides where to look: frames
     * arriving means GAL is receiving video it fails to render, frames never
     * arriving means the session-level negotiation is wrong. Rate limited to
     * the first few and then every 300th, so it costs nothing at 30fps.
     */
    if (sink != g_secondary_sink) {
        ++g_primary_frames;
        if (g_primary_frames <= 3ul || g_primary_frames % 300ul == 0ul)
            gal_hook_logf("event=aap.frame role=primary count=%lu header_len=%u",
                          g_primary_frames, length);
    }
    if (sink == g_secondary_sink) {
        struct timespec now;
        double measured_fps = 0.0;
        /*
         * Recovered once, up here, because everything below needs it: the
         * byte accounting, the start-code check, the forward and the ack.
         * The caller holds the shared_ptr for the duration of this call, so
         * the payload is alive for exactly this scope and not afterwards.
         */
        unsigned payload_bytes = 0u;
        const void *payload = encoded_payload(buffer, length, &payload_bytes);
        ++g_secondary_frames;
        /*
         * payload_bytes, not length. `length` is the HEADER length to skip
         * (see encoded_payload), so this counter and every bytes= figure
         * derived from it used to report 8-16 bytes per frame -- which reads
         * as a working stream carrying almost no data.
         */
        g_secondary_bytes += payload_bytes;
        if (clock_gettime(CLOCK_MONOTONIC, &now) == 0) {
            if (g_secondary_frames == 1ul) {
                g_secondary_first_frame_time = now;
            } else {
                double elapsed =
                    (double)(now.tv_sec - g_secondary_first_frame_time.tv_sec) +
                    (double)(now.tv_nsec - g_secondary_first_frame_time.tv_nsec) /
                        1000000000.0;
                if (elapsed > 0.0)
                    measured_fps = (double)(g_secondary_frames - 1ul) / elapsed;
            }
        }
        if (gal_hook_is_debug() &&
            (g_secondary_frames <= 10ul || g_secondary_frames % 30ul == 0ul)) {
            gal_hook_debugf("event=aap.frame role=secondary seq=%lu header_len=%u payload_bytes=%u total_bytes=%lu timestamp=%llu measured_input_fps=%.2f started=%d focus=%d buffer_ref=%p",
                            g_secondary_frames, length, payload_bytes,
                            g_secondary_bytes, (unsigned long long)timestamp,
                            measured_fps, g_secondary_started,
                            g_secondary_focus_mode, buffer);
        }
        /*
         * Prove the framing assumption on the way past, for the first few
         * frames only. The first eight bytes are printed either way: if this
         * ever says annex_b=no, the header offset is wrong and no amount of
         * player-side debugging will find it.
         */
        if (g_secondary_frames <= 3ul && payload != NULL) {
            const unsigned char *b = (const unsigned char *)payload;
            unsigned n = payload_bytes < 8u ? payload_bytes : 8u;
            char hex[3 * 8 + 1];
            unsigned i;
            for (i = 0u; i < n; ++i)
                (void)snprintf(hex + i * 3u, 4u, "%02x ", b[i]);
            hex[n == 0u ? 0 : n * 3u - 1u] = '\0';
            gal_hook_logf("event=aap.frame.framing seq=%lu annex_b=%s header_len=%u payload_bytes=%u first8=%s",
                          g_secondary_frames,
                          payload_is_annex_b(payload, payload_bytes) ? "yes" : "NO",
                          length, payload_bytes, hex);
        }
        clock_gettime(CLOCK_MONOTONIC, &g_last_frame_rx_time);
        if (payload == NULL) {
            if (g_secondary_frames <= 5ul)
                gal_hook_logf("event=own.decode result=no_payload seq=%lu header_len=%u",
                              g_secondary_frames, length);
        } else if (stream_forward_enabled()) {
            /*
             * Return value deliberately ignored: a failed write means the
             * reader is absent or behind, which is not the phone's problem.
             * vc_stream_out accounts for what it discarded.
             */
            if (fc_enabled()) {
                int delivered = 0;
                (void)vc_stream_out_write_ex(payload, payload_bytes, &delivered);
                if (delivered) {
                    fc_frame_delivered();
                } else if (hook_fix_enabled(HOOK_FIX_ACK_RENDERED_ONLY)) {
                    /*
                     * Not delivered -- no player connected, held back until a
                     * keyframe, or the write failed -- so no render ACK will
                     * ever come for it. Return the phone's credit now: it
                     * never forgets an unACKed frame, and on the car the
                     * frames lost across one player replacement closed its
                     * window to a single slot for the rest of the session.
                     */
                    ++g_undelivered_credits;
                    if (g_undelivered_credits <= 5ul || g_undelivered_credits % 100ul == 0ul)
                        gal_hook_logf("event=ack.credit_returned count=1 total=%lu reason=undelivered fix=ack_rendered_only",
                                      g_undelivered_credits);
                    ack_secondary_frame(sink);
                }
            } else {
                (void)vc_stream_out_write(payload, payload_bytes);
            }
            if (hook_fix_enabled(HOOK_FIX_STREAM_TIMING) &&
                g_secondary_frames % 30ul == 0ul)
                log_stream_timing();

            /*
             * Check once per second. The stream gate withholds delta frames
             * until a current-session IDR has arrived, and the player itself
             * waits for both decoded frames and a ready Kombi map window. It
             * has no forced display-switch timeout.
             */
            if (!fc_enabled() && g_secondary_started && (g_secondary_frames % 30ul == 0ul)) {
                if (!vc_player_is_running()) {
                    vc_player_start();
                }
            }
        }
        /*
         * Flow Control / ACK handling:
         * If stream-player is connected via the ACK socket, frame ACKs are
         * deferred until the player confirms rendering via /tmp/gal_ack.sock.
         * Failsafe fallback:
         * 1) If stream-player ACK client is NOT connected, or
         * 2) If >500ms have elapsed without any ACK from the player while frames flow,
         * dispatch immediate fallback ACK so the AAP session never hangs or disconnects.
         */
        int ack_client = vc_stream_out_ack_client_connected();
        if (fc_enabled() && hook_fix_enabled(HOOK_FIX_ACK_RENDERED_ONLY)) {
            /* fix=ack_rendered_only: the focus controller ACKs rendered frames. */
        } else if (!ack_client) {
            ack_secondary_frame(sink);
        } else {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            long elapsed_ms = (now.tv_sec - g_last_ack_rx_time.tv_sec) * 1000 +
                              (now.tv_nsec - g_last_ack_rx_time.tv_nsec) / 1000000;
            if (elapsed_ms > 500) {
                gal_hook_logf("event=ack.fallback reason=timeout elapsed_ms=%ld", elapsed_ms);
                ack_secondary_frame(sink);
                g_last_ack_rx_time = now;
            }
        }
    }
    /*
     * Never forward secondary frames to GAL. build_secondary_sink writes
     * our CVideoSinkImpl into the secondary's sink+0x38, and
     * handleDataAvailable dispatches through that object's vtable slot 3 --
     * which has a different signature than CVideoSinkCallbackHandler.
     * Calling it is undefined behaviour; skipping it is correct because
     * the hook has already forwarded the payload to stream-player above.
     */
    if (sink == g_secondary_sink) {
        if (g_secondary_frames == 1ul)
            gal_hook_logf("event=aap.frame role=secondary forward_to_gal=skipped reason=impl_at_sink_0x38_wrong_vtable");
    } else {
        g_handle_data(sink, timestamp, buffer, length);
    }
}


/*
 * Controller::sendVersionRequest()
 *
 * The stock implementation builds a six-byte message by hand and sends it
 * unencrypted on the control channel:
 *
 *     buf[0]=0 buf[1]=1   message id 0x0001, version request
 *     buf[2]=0 buf[3]=1   major = 1
 *     buf[4]=0 buf[5]=2   minor = 2
 *     MessageRouter::queueOutgoingUnencrypted(*(this+8), this[5], buf, 6)
 *
 * Google's own Desktop Head Unit requests 1.7, and a DHU capture taken
 * against this project's test phone shows a working two-display
 * ServiceDiscoveryResponse at that version (see reference/). The display
 * identity our hook injects -- MediaSinkService fields 6 and 7 -- appears
 * there with exactly the values we already write, so the remaining
 * question is whether the phone will parse those fields at all in a
 * session negotiated at 1.2.
 *
 * This override exists to answer that on-car. It changes only the minor
 * version byte, and only when GAL_DUALSCREEN_AAP_MINOR is set, so an
 * unset environment sends the stock 1.2 exactly as before.
 *
 * Understand the risk before enabling it: claiming 1.7 tells the phone
 * this receiver implements five minor versions of protocol that it does
 * not. GAL's protobuf schema is compiled in and cannot grow fields to
 * match. Expect new failure modes rather than a fix, and treat a working
 * Android Auto session afterwards as the thing to verify first.
 */
void _ZN10Controller18sendVersionRequestEv(void *receiver)
{
    const gal_secondary_config *config = gal_hook_config();
    unsigned char message[6];
    unsigned minor;

    /*
     * Unless a version was explicitly configured, hand this straight back to
     * the stock implementation rather than rebuilding the message ourselves.
     * The firmware's own sendVersionRequest writes major=1 minor=2 with
     * immediates (mov r2,#1 / strb r2,[r0,#3]; mov r1,#2 / strb r1,[r0,#5]),
     * so reconstructing it could only ever match that or be wrong, and there
     * is nothing to gain from re-implementing a function we do not want to
     * change. The second display is carried by the injected unknown fields
     * 6 and 7, not by the advertised version.
     */
    if (!gal_hook_aap_minor_overridden()) {
        (void)resolve_symbol("_ZN10Controller18sendVersionRequestEv",
                             (void **)&g_send_version_request);
        if (g_send_version_request != NULL) {
            g_send_version_request(receiver);
            gal_hook_log("event=aap.version result=stock action=delegated reason=no_override");
            return;
        }
        /* Fall through and build it by hand only if the real one is missing. */
    }

    (void)resolve_symbol("_ZN13MessageRouter24queueOutgoingUnencryptedEhPvj",
                         (void **)&g_queue_unencrypted);
    minor = gal_hook_aap_minor();
    message[0] = 0u; message[1] = 1u;    /* message id 0x0001 */
    message[2] = 0u; message[3] = 1u;    /* major 1 */
    message[4] = 0u; message[5] = (unsigned char)minor;

    if (g_queue_unencrypted == NULL || receiver == NULL) {
        gal_hook_logf("event=aap.version result=failed reason=unresolved receiver=%p queue=%p",
                      receiver, (void *)g_queue_unencrypted);
        return;
    }
    /* Mirror the stock guard: it only sends while this flag is set. */
    if (((const unsigned char *)receiver)[4] != 0u) {
        void *router = (void *)(uintptr_t)load_u32(receiver, 0x08u);
        unsigned char channel = ((const unsigned char *)receiver)[5];
        g_queue_unencrypted(router, channel, message, sizeof(message));
        gal_hook_logf("event=aap.version result=sent major=1 minor=%u stock_minor=2 router=%p channel=%u",
                      minor, router, (unsigned)channel);
    } else {
        gal_hook_logf("event=aap.version result=skipped reason=controller_not_ready minor=%u",
                      minor);
    }
    (void)config;
}

/*
 * MessageRouter::routeMessage(unsigned char channel, shared_ptr<IoBuffer>&)
 *
 * Every inbound AAP message passes through here, and the channel IS the
 * service id, so this is the only place that can answer "did the phone
 * ever address our services at all".
 *
 * That matters because the cluster InputSourceService this hook now
 * advertises has NO endpoint behind it. Without this, a phone that sets
 * up the input service, gets silence and abandons the display would look
 * identical in the log to a phone that ignored the cluster entirely --
 * two very different problems. Here they are distinguishable: an
 * unanswered service still shows inbound traffic on its channel.
 *
 * Logs the first message on each channel and then every 500th, so a
 * video channel streaming frames cannot flood the log.
 */
void _ZN13MessageRouter12routeMessageEhRK10shared_ptrI8IoBufferE(
    void *router, unsigned char channel, const void *buffer)
{
    unsigned long count;

    (void)resolve_symbol("_ZN13MessageRouter12routeMessageEhRK10shared_ptrI8IoBufferE",
                         (void **)&g_route_message);
    count = ++g_channel_seen[channel];
    if (count == 1ul) {
        const gal_secondary_config *config = gal_hook_config();
        const char *role = "other";
        if (channel == (unsigned char)g_secondary_service_id) role = "secondary_video";
        else if (g_primary_sink != NULL &&
                 channel == (unsigned char)endpoint_service_id(g_primary_sink))
            role = "primary_video";
        else if (g_secondary_service_id != 0u &&
                 channel == (unsigned char)((g_secondary_service_id + 1u) & 0xffu))
            role = "cluster_input";
        gal_hook_logf("event=aap.inbound.first channel=%u role=%s secondary_service=%u display_id=%u",
                      (unsigned)channel, role,
                      g_secondary_service_id, config->display_id);
    } else if (count % 500ul == 0ul) {
        gal_hook_debugf("event=aap.inbound.count channel=%u messages=%lu",
                        (unsigned)channel, count);
    }

    /*
     * Everything the phone says on the secondary's channel, until the first
     * frame arrives.
     *
     * The throttle above logs message 1 and then every 500th, so a channel
     * that carries twenty control messages and no video looked identical in
     * the log to a channel that carried nothing at all -- and that is exactly
     * the state every on-car session has ended in so far. Once frames flow
     * this goes quiet on its own and the throttle takes over again.
     */
    if (g_secondary_service_id != 0u && g_secondary_frames == 0ul &&
        channel == (unsigned char)g_secondary_service_id) {
        unsigned bytes = 0u;
        const unsigned char *p = (const unsigned char *)iobuffer_bytes(buffer, &bytes);
        if (p != NULL && bytes >= 2u)
            gal_hook_logf("event=aap.inbound.secondary seq=%lu msg_id=0x%04x bytes=%u started=%d focus=%d",
                          count, (unsigned)((p[0] << 8) | p[1]), bytes,
                          g_secondary_started, g_secondary_focus_mode);
        else
            gal_hook_logf("event=aap.inbound.secondary seq=%lu msg_id=unreadable bytes=%u started=%d focus=%d",
                          count, bytes, g_secondary_started,
                          g_secondary_focus_mode);
    }

    /*
     * No-frame watchdog.
     *
     * Playback started on the cluster and the phone has sent nothing. Until
     * now that produced no log line whatsoever -- the single outcome the log
     * could not distinguish from a hook that never loaded, and the one every
     * session so far actually hit. routeMessage is the heartbeat because it
     * fires for inbound traffic on every other channel while the secondary
     * stays silent; no timer thread is needed inside GAL for this.
     */
    if (g_secondary_started && g_secondary_frames == 0ul &&
        !g_secondary_no_frame_warned && g_secondary_start_time.tv_sec != 0) {
        struct timespec now;
        if (clock_gettime(CLOCK_MONOTONIC, &now) == 0 &&
            (now.tv_sec - g_secondary_start_time.tv_sec) >= 5) {
            g_secondary_no_frame_warned = 1;
            gal_hook_logf("event=secondary.no_frames elapsed_s=%ld service=%u channel_messages=%lu focus=%d input_service_messages=%lu hint=phone_started_playback_but_sent_no_video",
                          (long)(now.tv_sec - g_secondary_start_time.tv_sec),
                          g_secondary_service_id,
                          g_channel_seen[g_secondary_service_id & 0xffu],
                          g_secondary_focus_mode,
                          g_channel_seen[(g_secondary_service_id + 1u) & 0xffu]);
        }
    }
    if (g_secondary_service_id != 0u &&
        channel == (unsigned char)((g_secondary_service_id + 1u) & 0xffu)) {
        /*
         * Traffic on the synthetic cluster input channel. Since there is no
         * C++ endpoint object in router->table[service + 0x40], swallow safely
         * rather than delegating to g_route_message which would dereference NULL.
         */
        fc_run_mailbox();
        return;
    }
    if (g_route_message != NULL) g_route_message(router, channel, buffer);
    fc_run_mailbox();
}

/* Diagnostic only: log stock/secondary VideoConfiguration registrations. */
void _ZN9VideoSink25addSupportedConfigurationEiiiiiii(
    void *sink, int resolution, int fps, int width_margin, int height_margin,
    int density, int decoder_depth, int pixel_aspect_ratio_e4)
{
    (void)resolve_symbol("_ZN9VideoSink25addSupportedConfigurationEiiiiiii",
                         (void **)&g_add_configuration);
    if (g_add_configuration == NULL) return;
    /* See g_pending_primary_sink's declaration: this call is how the
     * primary VideoSink announces itself just before it registers. The
     * secondary is excluded because build_secondary_sink() drives its
     * configuration through g_add_configuration directly, but a future
     * change routing it back through here must not clobber this. */
    if (sink != NULL && sink != g_secondary_sink) g_pending_primary_sink = sink;
    gal_hook_debugf("event=video.config.add role=%s sink=%p resolution_enum=%d fps=%d margins=%dx%d dpi=%d decoder_depth=%d pixel_aspect_e4=%d",
                    sink == g_secondary_sink ? "secondary" : "primary",
                    sink, resolution, fps, width_margin, height_margin,
                    density, decoder_depth, pixel_aspect_ratio_e4);
    g_add_configuration(sink, resolution, fps, width_margin, height_margin,
                        density, decoder_depth, pixel_aspect_ratio_e4);
}


/*
 * The channel/service role labels used by the two interposers below and by
 * routeMessage. Kept in one place so a log line cannot disagree with
 * another about what channel 64 is.
 */
static const char *channel_role(unsigned id)
{
    if (g_secondary_service_id != 0u && id == g_secondary_service_id)
        return "secondary_video";
    if (g_primary_sink != NULL && id == endpoint_service_id(g_primary_sink))
        return "primary_video";
    if (g_secondary_service_id != 0u &&
        id == ((g_secondary_service_id + 1u) & 0xffu))
        return "cluster_input";
    return "other";
}

/*
 * MediaSinkBase::sendConfig(this, int)
 *
 * This is where the head unit tells the phone which video configurations
 * it supports -- message id 0x8003, built at 0xec880 in libautoreceiver.so
 * -- and the phone refuses video until it has one. Nothing in this hook
 * could see it before, so a run in which the phone logged
 * "No configuration indices." (Critical error 6 detail: 40) looked
 * identical on the head-unit side to a run in which the list was fine.
 *
 * From the disassembly at 0xec748:
 *
 *     r1 = [this+0x24]                    ; allowed-configuration vector
 *     if (r1 == 0)             goto fallback
 *     n = ([this+0x28] - r1) >> 2         ; 4-byte elements
 *     if (n == 0)              goto fallback
 *     ...serialise those n indices...
 *   fallback:
 *     n = (*(*this + 0x3c))(this)         ; VideoSink::getNumberOfConfigurations
 *     ...serialise indices 0 .. n-1...
 *
 * vtable slot +0x3c of _ZTV9VideoSink (0x103148; the entries start at
 * 0x103150) is _ZN9VideoSink25getNumberOfConfigurationsEv at 0xfa95c.
 *
 * indices = allowed when allowed > 0, else supported. indices == 0 is the
 * empty-list failure, and it is now stated outright.
 */
int _ZN13MediaSinkBase10sendConfigEi(void *sink, int arg)
{
    unsigned allowed = 0u;
    unsigned supported = 0u;
    unsigned indices;
    const char *role;
    int known_video_sink;

    (void)resolve_symbol("_ZN13MediaSinkBase10sendConfigEi",
                         (void **)&g_send_config);
    if (g_send_config == NULL) {
        gal_hook_log("event=aap.config_response result=unresolved symbol=sendConfig");
        return -1;
    }
    if (sink != NULL) {
        uint32_t begin = load_u32(sink, 0x24u);
        if (begin != 0u)
            allowed = (load_u32(sink, 0x28u) - begin) >> 2;
    }
    known_video_sink = sink != NULL &&
                       (sink == g_secondary_sink || sink == g_primary_sink);
    if (known_video_sink) {
        (void)resolve_symbol("_ZN9VideoSink25getNumberOfConfigurationsEv",
                             (void **)&g_config_count);
        if (g_config_count != NULL) supported = g_config_count(sink);
    }
    indices = allowed != 0u ? allowed : supported;
    if (sink == g_secondary_sink) role = "secondary";
    else if (sink == g_primary_sink) role = "primary";
    else role = "other";
    gal_hook_logf("event=aap.config_response role=%s service=%u msg_id=0x8003 arg=%d allowed_list=%u supported=%u indices=%u result=%s",
                  role, endpoint_service_id(sink), arg, allowed, supported,
                  indices,
                  known_video_sink && indices == 0u ? "EMPTY_PHONE_WILL_REJECT"
                                                    : "ok");
    return g_send_config(sink, arg);
}

/*
 * MessageRouter::handleChannelOpenReq(this, channel, ChannelOpenRequest &)
 *
 * The only place that can answer "did the phone ever try to open our
 * channels, and was it refused". routeMessage does NOT see channel-open
 * control traffic -- it arrives through routeChannelControlMsg -- so the
 * existing input_service_messages=0 counter could never distinguish "the
 * phone never asked for the cluster input service" from "it asked and the
 * router refused because that service has no endpoint".
 *
 * From 0xecc4c in libautoreceiver.so: the requested service id is a byte at
 * request+0x2c; the router indexes this->table[service_id + 0x40] and
 * returns -4 for service id 0xff or a NULL slot, -254 when the endpoint's
 * mayOpenChannel (vtable+0x0c) refuses, and 0 on success.
 */
int _ZN13MessageRouter20handleChannelOpenReqEhRK18ChannelOpenRequest(
    void *router, unsigned char channel, const void *request)
{
    unsigned service;
    int rc;

    (void)resolve_symbol(
        "_ZN13MessageRouter20handleChannelOpenReqEhRK18ChannelOpenRequest",
        (void **)&g_handle_channel_open);
    service = request == NULL ? 0xffu : load_u8(request, 0x2cu);
    if (g_handle_channel_open == NULL) {
        gal_hook_logf("event=aap.channel.open result=unresolved channel=%u service=%u",
                      (unsigned)channel, service);
        return -4;
    }
    if (g_secondary_service_id != 0u && service == ((g_secondary_service_id + 1u) & 0xffu)) {
        /*
         * Synthetic cluster input service (service 65).
         * Stock GAL has no C++ endpoint in router->table[service + 0x40],
         * so the real handleChannelOpenReq would return -4 (no endpoint).
         * Map the channel in the router table (router[channel] = service)
         * and return 0 (Success) so GAL sends ChannelOpenResponse(status=0).
         */
        if (router != NULL) {
            *((unsigned char *)router + channel) = (unsigned char)service;
        }
        gal_hook_logf("event=aap.channel.open channel=%u service=%u role=cluster_input rc=0 meaning=opened_synthetic",
                      (unsigned)channel, service);
        return 0;
    }
    rc = g_handle_channel_open(router, channel, request);
    gal_hook_logf("event=aap.channel.open channel=%u service=%u role=%s rc=%d meaning=%s",
                  (unsigned)channel, service, channel_role(service), rc,
                  rc == 0 ? "opened" :
                  rc == -4 ? "no_endpoint_or_bad_service_id" :
                  rc == -254 ? "endpoint_refused" : "other");
    return rc;
}
