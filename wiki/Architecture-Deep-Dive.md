# Architecture Deep-Dive

This document details the reverse engineering, protocol hooks, graphics pipeline, and architectural lessons learned that make dual-screen Android Auto projection possible on the Harman MIB2.5 High (MHI2 / Tegra 30 / QNX 6.5.0).

---

## 1. Dynamic Endpoint Injection & Focus Control

The factory `/usr/bin/gal` daemon implements Google Automotive Link. In stock firmware, `gal.json` only specifies a single primary `VideoSinkService` (Channel 1).

### The Problem with Modifying `gal.json`
Directly editing `gal.json` to define a secondary video sink causes `gal` to crash or silently ignore the configuration due to hardcoded validation routines in the stock parser.

### Dynamic Heap Injection (`src/video_sink_hook.c`)
`libgal_hook.so` intercepts `GalReceiver::registerService`:
```cpp
// Demangled symbol: GalReceiver::registerService(ProtocolEndpointBase*)
_ZN11GalReceiver15registerServiceEP20ProtocolEndpointBase
```
1. When GAL initializes Channel 1, the hook intercepts the call.
2. It dynamically allocates a C++ `ProtocolEndpointBase` structure on the heap.
3. It initializes the structure with service ID `3` (Secondary VideoSink) and injects it into GAL's internal dispatch table at offset `service_id + 0x40`.
4. When the phone sends a secondary `ChannelOpenRequest`, the hook intercepts `handleChannelOpenReq` and synthesizes an immediate accept response.

### Dynamic Focus Controller (`src/focus_ctl.c`)
* **Previous Failure (Static Focus Hazard):** Earlier prototypes hardcoded static focus (`sink+0x30 = 1u`) during early initialization. Consequently, the phone immediately began encoding and streaming video over USB before `stream-player` was running, before DMDT was routed, and before the Kombi cluster was initialized. This flooded buffers, wasted CPU decoding off-screen, and meant the player joined mid-stream—requiring fragile pre-IDR buffering and cached codec-config replay hacks (`HOOK_FIX_NO_IDR_REPLAY`).
* **Why Dynamic Focus is Superior:** `focus_ctl.c` holds the secondary sink in focus mode 2 (native: cluster not showing projection) during startup. It monitors the player process and socket connections, and only grants video focus mode 1 when:
  1. `stream-player` is running and connected to `/tmp/gal_video.sock`.
  2. The Kombi instrument cluster map context is confirmed ready (`GAL_FOCUS_WAIT_KOMBI`).
* **Instant Native IDR:** When focus mode 1 is granted, Android Auto's `MediaCodec` encoder naturally generates a fresh, clean SPS/PPS parameter set and IDR keyframe directly to the waiting player. If the player crashes or exits, focus immediately reverts to mode 2 before restarting.

---

## 2. End-to-End Hardware Paced Flow Control

```text
[ Android Phone ]
      │  (Sends H.264 video NALU)
      ▼
[ MIB2 USB / GAL ]
      │
[ libgal_hook.so ] ──(Unix Domain Socket /tmp/gal_video.sock)──► [ stream-player ]
      │                                                                  │
 (Withholds AAP ACK)                                                     │ (Decode & eglSwapBuffers)
      │                                                                  ▼
      │◄─── (Writes 0x01 ACK) ────────── [ /tmp/gal_ack.sock ] ──────────┘
      ▼
 (Sends AAP ACK to Phone)
```

### Why Previous Flow Control Designs Failed

1. **Failure Mode A: Unpaced USB ACKs (Buffer Saturation & Green Macroblocking):**
   * Android Auto uses sliding-window flow control. If the head unit ACKs video packets the moment they arrive over USB, the phone's encoder assumes infinite throughput and ramps up bitrate faster than the Tegra 3 GPU can consume.
   * This overflows socket buffers, drops critical IDR keyframes, and causes severe green macroblock tearing.

2. **Failure Mode B: Multi-Frame Threading Deadlock (`FF_THREAD_FRAME` -> 3.3 FPS Timeout Stall):**
   * Early versions configured FFmpeg with `FF_THREAD_FRAME` (2 frame-parallel worker threads).
   * In FFmpeg, frame threading buffers each decoded frame until the *subsequent* packet arrives. But under hardware flow control, the render ACK is withheld until that frame is swapped to the display.
   * When the phone was low on sliding-window credit, it waited for the render ACK before sending the next packet.
   * This created a circular deadlock: the phone waited for an ACK, while FFmpeg waited for the next packet to release the frame. Every frame stalled until the phone's internal ~300ms timeout expired, collapsing framerate to **3.3 FPS with 313ms latency** (verified in on-car testing on 2026-09-16).

### Why the Low-Delay Architecture is Superior
* **Zero Frame Delay (`AV_CODEC_FLAG_LOW_DELAY` & `FF_THREAD_SLICE`):**
  * H.264 decoding at 800×480 takes only ~1 ms on a single Cortex-A9 core. Frame-parallel threading bought no throughput gains and caused pipeline deadlocks.
  * Setting `AV_CODEC_FLAG_LOW_DELAY` ensures FFmpeg decodes and outputs frames immediately without holding buffers.
* **Hardware Backpressure Loop:**
  * `libgal_hook.so` forwards NALUs over `/tmp/gal_video.sock` and withholds the AAP protocol ACK.
  * `stream-player` decodes the frame, presents it via `eglSwapBuffers()`, and writes `0x01` into `/tmp/gal_ack.sock`.
  * `libgal_hook.so` receives the ACK and releases window credit back to the phone.
* **Result:** The phone's hardware encoder is naturally throttled to the exact hardware swap cadence of the Tegra 3 GPU blitter (verified at 99.84% synchronous lockstep with 0 dropped frames).

---

## 3. High-Performance Transport: Unix Domain Socket (`/tmp/gal_video.sock`)

### The Child Process Destructor Unlink Hazard
Earlier attempts to use Unix domain sockets encountered a severe QNX runtime trap:
* `smartphone_integrator` spawns short-lived helper child processes that inherited `LD_PRELOAD` of `libgal_hook.so`.
* Upon child process exit, the C runtime executed shared library destructors (`__attribute__((destructor)) -> unlink("/tmp/gal_video.sock")`), deleting the socket file out from under the running GAL daemon.
* **Temporary Workaround:** We temporarily migrated to TCP loopback (`tcp://127.0.0.1:12346`) to avoid `/tmp` unlinks, but this introduced TCP stack overhead, packetization delay, and buffer tuning constraints.

### The Permanent Solution: Helper Init Guard (`HOOK_FIX_HELPER_INIT_GUARD`)
* `libgal_hook.so` now inspects the process environment and name during initialization.
* Helper child processes spawned by GAL skip hook initialization entirely. Destructors are never registered in child processes, cleanly eliminating the unlink hazard.
* Video transport operates over high-performance native Unix domain sockets (`/tmp/gal_video.sock`) with zero TCP network stack overhead and ultra-low latency.

### Reverse Camera (RVC) Context Switch & 250ms Timeout
Shifting into reverse gear activates the Optical Parking System (OPS) or Rear View Camera (RVC), briefly pausing video buffer consumption for ~30–50ms.
* A standard 20ms `select()` write timeout caused `vc_stream_out.c` to drop the connection with `errno 260` (`ETIMEDOUT`).
* **The Rule:** The `select()` write timeout must be strictly **250ms** (`tv_usec = 250000`) to absorb reverse camera and Kombi view transitions cleanly.
* **Why this is superior to focus toggling:** By keeping the secondary video sink continuously live in memory and letting the 250ms socket buffer absorb the transient display pause, playback resumes instantaneously upon shifting back to Drive (D). This completely bypasses the latency, mode cycling, and phone encoder renegotiation required by focus pulsing.

---

## 4. Tegra 3 Hardware Blitter (`glDrawTextureNV`)

Hardware probe `probe_gl_caps` confirmed that the Tegra 3 GLES2 implementation on MIB2 Harman firmware has `GL_SHADER_COMPILER == 0`. Online GLSL compilation via `glCompileShader` fails with `0x502 GL_INVALID_OPERATION`.

* `stream-player` renders using Nvidia's dedicated hardware blit extension:
  ```c
  glDrawTextureNV(0, 0, 0, 800, 480);
  ```
* This achieves zero-copy GPU texturing directly into the OpenKODE displayable surface.
