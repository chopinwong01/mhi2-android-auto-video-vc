# mhi2-android-auto-video-vc

> **Experimental hook for Harman MHI2 stock GAL to enable Android Auto instrument cluster projection.**  
> *Developed for personal research and study; tested and verified strictly on Volkswagen MIB2.5 High EU MU1367 (`MHI2_ER_VWG13_P4521_MU1367`).*

📖 **For in-depth technical guides, troubleshooting, and architectural deep-dives, visit the [Project Wiki](wiki/Home.md).**

---

### 🧩 Upstream Foundations & Companion Projects

This project provides the **native C preload hook and hardware video streaming pipeline**. It builds upon and integrates with key projects in the MIB2 / MQB ecosystem:

* 📺 **[VcMOSTRenderMqb](https://github.com/andrewleech/VcMOSTRenderMqb)** *(by [@andrewleech](https://github.com/andrewleech))*  
  The foundational MOST150 video transmission and Tegra 3 OpenKODE/GLES2 cluster rendering architecture adapted by `stream-player`.
* 🧭 **[NavActiveIgnore](https://github.com/jille/mib2-navignore)** (`navignore` *(by [@jille](https://github.com/jille) / [M.I.B.](https://github.com/Mr-MIBoner/M.I.B._More-Incredible-Bash))*  
  The **minimum baseline requirement** on the vehicle's Java HMI. Bypasses the factory mutual exclusion check so phone navigation and cluster displays run concurrently without kicking each other out.
* 🎮 **[mib2-android-auto-vc](https://github.com/chopinwong01/mib2-android-auto-vc)** *(by [@chopinwong01](https://github.com/chopinwong01))*  
  The **companion Java HMI patch** (`VCAndroidAuto_mapmode.jar`). Routes steering wheel (MFL) scroll wheel events to zoom the Android Auto cluster map, injects D-pad keys, and suppresses duplicate cluster turn banners.
* 🛠️ **[MIB SDK](https://gitlab.com/andrewleech/mibsdk)** *(by [@andrewleech](https://github.com/andrewleech))*  
  The Dockerized QNX Neutrino 6.5.0 cross-compilation toolchain used to build all native binaries.

---

> [!CAUTION]
> **CRITICAL WARNING — RISK OF HEAD UNIT DAMAGE OR BRICKING:**  
> This software interacts directly with low-level QNX RTOS services, hardware graphics controllers, and vehicle bus gateways.  
> * **Software Risk:** Improper configuration, exceeding supervisor environment limits (Rule of 10), or deploying incompatible Java bytecode will cause bootloops, supervisor crashes, or complete loss of the vehicle's infotainment UI (black screen).  
> * **Hardware / System Risk:** Flash memory corruption, overheating from unthrottled decoding workloads, or bus desync can permanently disable the MMX unit (requiring bench flashing / hardware recovery).  
> * **Development Disclosure ("Vibe Coded"):** This project was heavily "vibe coded" and iteratively developed with various Large Language Models (LLMs)—including **Google Gemini**, **Anthropic Claude**, and **OpenAI GPT**. While rigorously bench-tested and telemetry-audited on real vehicle hardware, AI-assisted low-level code inherently demands thorough review before deployment.  
> **Never modify files in `/lib` or `/usr/lib`. Proceed strictly at your own risk.**

---

## Overview

Modern Volkswagen Group vehicles equipped with the Virtual Cockpit (Active Info Display / FPK) receive navigation video feeds over the MOST150 optical bus (`/dev/mlb/isoTX2`). The factory Harman MIB2 High (MMX / Nvidia Tegra 30 / QNX 6.5.0) `gal` daemon only implements a single primary display sink (Channel 1).

**`mhi2-android-auto-video-vc`** provides a native runtime injection and rendering pipeline to project secondary Android Auto navigation video (e.g. Google Maps, Waze) directly onto the instrument cluster.

```text
[ Android Phone ] --(AAP USB)--> [ MIB2 gal Daemon ]
                                         │
                                  [ libgal_hook.so ]
                                         │ (Unix Domain Socket /tmp/gal_video.sock)
                                         ▼
                                  [ stream-player ]
                                         │ (glDrawTextureNV -> Context 70)
                                         ▼
                             [ Virtual Cockpit Display ]
```

---

## Repository Structure

The repository is organized into self-contained, minimal modules:

```text
mhi2-android-auto-video-vc/
├── Makefile                 # Docker-based build for libgal_hook.so
├── LICENSE                  # GNU General Public License v3.0 (GPLv3)
├── README.md                # Project documentation & reference
├── src/                     # Native C hook source (libgal_hook.so)
│   ├── gal_hook.c           # Entry point, symbol interceptors & lifecycle
│   ├── gal_hook.h
│   ├── focus_ctl.c          # Secondary video focus state machine & phone credit pacing
│   ├── focus_ctl.h
│   ├── video_sink_hook.c    # ProtocolEndpointBase allocation & Channel 3 spoof
│   ├── vc_stream_out.c      # Unix domain socket streaming & player-ACK flow control
│   ├── vc_stream_out.h
│   ├── vc_player_mgr.c      # Automatic stream-player process supervisor & Kombi watcher
│   └── vc_player_mgr.h
├── player/                  # Cluster video renderer (stream-player)
│   ├── opengl_gpu.cc        # Low-delay zero-frame-delay renderer (glDrawTextureNV)
│   ├── config.txt           # OpenKODE / Displayable context definition
│   ├── Makefile             # Docker build script linking against ffmpeg-mini
│   └── README.md            # Technical details & upstream attribution
├── scripts/                 # Safe QNX installation & management scripts
│   ├── enable_hook.sh       # Patches smartphone_integrator.json (enforces Rule of 10)
│   ├── disable_hook.sh      # Clean uninstaller & factory backup restoration
│   ├── hook_status.sh       # In-car diagnostic utility for hook & player status
│   ├── lib_app_mount.sh     # Shared /mnt/app safe mounting helper
│   ├── deploy_to_car.sh     # SCP / SSH deployment script
│   └── gal_dualscreen.conf.example # Configuration file template
└── wiki/                    # Comprehensive documentation & engineering runbooks
    ├── Home.md              # Wiki index & quick navigation
    ├── Compatibility-Matrix.md # Hardware & firmware specifications
    ├── Installation-and-Safety-Guide.md # Step-by-step install & safety rules
    ├── Architecture-Deep-Dive.md # Reverse engineering & flow control
    ├── Companion-HMI-Integration.md # Java HMI layer (NavActiveIgnore & zoom)
    ├── Troubleshooting-and-Diagnostics.md # Diagnostics & verified telemetry
    └── Build-Environment.md # Docker cross-compilation toolchain
```

---

## Technical Highlights & Runtime Process Flow

The dual-screen projection pipeline executes across four synchronized phases from the moment the Android phone is connected to the vehicle:

```text
  [ Android Phone ]
         │
  Phase 1: Handshake & Dynamic Focus (focus_ctl)
         ▼
  [ MIB2 gal Daemon ] ◄── (libgal_hook.so holds mode 2; grants mode 1 on player connect)
         │
  Phase 2: H.264 NALU Stream (/tmp/gal_video.sock)
         ▼
  [ stream-player ] ◄── (Zero-delay low-delay decode, AV_CODEC_FLAG_LOW_DELAY)
         │
  Phase 3: Hardware Blit (glDrawTextureNV)
         ▼
  [ Virtual Cockpit ] (Displayable 3, Context 70 on MOST150)
         │
  Phase 4: 1-Byte Hardware ACK (/tmp/gal_ack.sock)
         ▼
  [ libgal_hook.so ] ──► (Releases AAP Frame ACK to Phone — Hardware Flow Control)
```

### 1. Dynamic Service Injection & Focus Control (`focus_ctl`)
* **Dynamic Protocol Allocation:** The stock `/usr/bin/gal` daemon strictly rejects secondary screen blocks in `gal.json`. `libgal_hook.so` intercepts `GalReceiver::registerService`, dynamically allocates a C++ `ProtocolEndpointBase` structure in heap memory, and binds it into GAL's internal dispatch table at offset `service_id + 0x40`.
* **Why Static Focus Failed (The Early-Stream Flood):** Earlier prototypes hardcoded static focus (`sink+0x30 = 1u`). Android Auto immediately flooded H.264 frames before `stream-player` launched or the Kombi cluster was ready, dropping initial IDR keyframes and causing green artifact flashes.
* **The Dynamic Focus Solution:** `focus_ctl.c` holds the secondary sink in focus mode 2 (native: projection inactive) until `stream-player` connects to `/tmp/gal_video.sock` and the Kombi map is verified ready (`GAL_FOCUS_WAIT_KOMBI`). Only then is mode 1 granted. The phone responds by naturally emitting a fresh, clean SPS/PPS parameter set and IDR keyframe directly to the player.

### 2. High-Performance Transport: Unix Domain Socket (`/tmp/gal_video.sock`)
* **Why Early Unix Sockets Failed (The Child Destructor Trap):** Helper child processes spawned by GAL inherited `LD_PRELOAD` of `libgal_hook.so` and on exit executed `unlink("/tmp/gal_video.sock")`, deleting the socket file out from under the running daemon. While TCP loopback temporarily worked around this, it added TCP stack latency and buffer tuning overhead.
* **The Helper Init Guard (`HOOK_FIX_HELPER_INIT_GUARD`):** By inspecting process identity on startup, child helper processes skip hook initialization entirely, permanently eliminating the destructor unlink trap. Video NALUs stream directly over `/tmp/gal_video.sock` with minimal latency and zero TCP network overhead.
* **RVC / OPS Resilience:** A **250ms socket write timeout** smoothly absorbs transient display pauses (e.g., shifting into Reverse gear for the Rear View Camera or parking sensors). Keeping the secondary video sink continuously live in combination with this 250ms buffer allows playback to resume instantaneously when shifting back into Drive (D), completely avoiding the latency and keyframe renegotiation overhead of focus cycling.

### 3. Low-Delay Zero-Frame-Delay Decoding (`stream-player`)
* **Why Multi-Frame Threading Failed (The 3.3 FPS Deadlock):** Early builds configured FFmpeg with `FF_THREAD_FRAME`. Frame threading holds each decoded frame until the *subsequent* packet arrives. Under hardware flow control (which withholds ACKs until display swap), the phone exhausted its sliding-window credit and paused waiting for an ACK before sending the next packet. This circular lock caused a ~300ms phone timeout per frame, collapsing playback to **3.3 FPS with 313ms latency**.
* **The Zero-Delay Solution:** `stream-player` configures `AV_CODEC_FLAG_LOW_DELAY` with `FF_THREAD_SLICE`. Decoded frames are presented immediately without holding delays. Because 800×480 H.264 decode takes only ~1 ms on a Cortex-A9 core, slice threading provides immediate presentation and returns the render ACK synchronously at rock-solid 30 FPS.
* Direct rendering is performed using Nvidia's dedicated hardware blit extension `GL_NV_draw_texture` (`glDrawTextureNV`), bypassing Tegra 3's disabled online GLSL compiler.

### 4. End-to-End Hardware Paced Flow Control (Zero Macroblocking)
* **Why Unpaced USB ACKs Failed:** ACKing video packets the moment they arrived over USB caused the phone's encoder to assume infinite bandwidth, flooding socket buffers, dropping reference frames, and causing severe green macroblocking.
* **The Closed Backpressure Loop:** `libgal_hook.so` withholds the AAP protocol frame ACK until `stream-player` completes hardware presentation (`eglSwapBuffers()`) and writes a 1-byte ACK (`0x01`) into `/tmp/gal_ack.sock`.
* This organically throttles the phone's hardware video encoder to the exact physical swap rate of the Tegra 3 GPU (verified at 99.84% synchronous lockstep with zero packet drops).

---

## Companion HMI Requirements

The video pipeline operates independently at the QNX RTOS level. To integrate with the Volkswagen Java HMI:

* **Minimum Requirement (`NavActiveIgnore`):** Suppresses the mutual exclusion check that prevents Android Auto and cluster navigation from running concurrently.
* **Full Steering Wheel Integration:** See [wiki/Companion-HMI-Integration.md](wiki/Companion-HMI-Integration.md) for details on [`mib2-android-auto-vc`](https://github.com/chopinwong01/mib2-android-auto-vc) which adds steering wheel scroll wheel zoom and D-pad key routing.

---

## Building

### Prerequisites
* Docker
* Access to the MIB SDK Docker image: `registry.gitlab.com/andrewleech/mibsdk:latest`

### Building the Preload Hook (`libgal_hook.so`)
```bash
make hook
```
The compiled library will be output to `./libgal_hook.so`.

### Building the Stream Player (`stream-player`)
See [player/README.md](player/README.md) for build instructions linking against minimal FFmpeg.

---

## Installation & Safety

> [!CAUTION]
> **Strict OEM Safety Rules:**
> 1. **Never modify files in `/lib` or `/usr/lib` on the MIB2 root partition.** Custom shared libraries live strictly in `/mnt/app/eso/lib/`.
> 2. **Supervisor "Rule of 10":** `smartphone_integrator.json` silently discards the entire environment array if it contains 11 or more variables. `enable_hook.sh` strictly enforces `ENTRY_COUNT <= 10`.

1. Copy compiled binaries and scripts to an SD card.
2. In QNX terminal on the head unit:
   ```sh
   sh /fs/sda0/scripts/enable_hook.sh
   ```
3. Verify status:
   ```sh
   sh /fs/sda0/scripts/hook_status.sh
   ```
4. Reboot the head unit by holding the power button for 10 seconds.

---

## 🗺️ Roadmap / Implementation Status

* [x] **Dynamic Secondary Video Focus & Mode Switch:**
  * Implemented via `focus_ctl.c` / `focus_ctl.h`. Holds secondary sink in mode 2 (native) until `stream-player` connects, triggering an immediate native SPS/PPS + IDR keyframe from the phone.
  * Verified Kombi map readiness check (`GAL_FOCUS_WAIT_KOMBI`).
  * Seamless RVC transitions via 250ms socket buffer with live secondary sink.
* [x] **Unix Domain Socket Migration:**
  * Implemented via `unix:///tmp/gal_video.sock`.
  * `HOOK_FIX_HELPER_INIT_GUARD` prevents child helper processes from running destructors and unlinking the socket file.
* [x] **Zero Frame Delay Decoding Pipeline:**
  * Replaced `FF_THREAD_FRAME` with `AV_CODEC_FLAG_LOW_DELAY` (`FF_THREAD_SLICE`) to eliminate the 3.3 FPS / 300ms phone credit timeout deadlock.
* [ ] **Hardware NVSS / NvMedia Video Decoder Renderer:**
  * Transition `stream-player` from software multi-threaded FFmpeg decoding to hardware video decoding via **NvSS / NvMedia** (`/dev/nvss`, Nvidia Tegra hardware video decoder), substantially cutting Cortex-A9 CPU utilization.
* [x] ~~**GPS Sensor Uncertainty Hook (Tunnel Loss Prevention)**~~ *(Abandoned — Proven Architectural Dead-End)*:
  * **Finding:** Intercepting `SensorSource::reportLocationData` to clamp reported accuracy to $\le 10\text{m}$ corrupted Google Maps' Extended Kalman Filter (EKF) covariance matrix ($R_k \to 0$). Normal 5–12m urban multipath noise was interpreted as real physical vehicle displacement, causing violent compass spinning, 90°/180° map orientation flipping, and endless reroute loops. Overriding `has_acc = false` also suppressed Android Auto's native failover to the phone's internal dual-frequency L1/L5 GNSS. Production code leaves OEM sensor data untouched (`src/sensor_hook.c` deleted in `7eb2317`).

---

## Attribution & Acknowledgments

* **Andrew Leech:** For the [MIB SDK](https://gitlab.com/andrewleech/mibsdk) toolchain and the foundation of `VcMOSTRenderMqb`.
* **FFmpeg Project:** Multi-threaded H.264 video decoding.
* **MQB / MIB2 Hacking Community:** Research and tools on Harman MHI2 architectures.
* **Large Language Models (LLMs):** Rapid reverse-engineering, architecture synthesis, and "vibe coding" across **Google Gemini**, **Anthropic Claude**, and **OpenAI GPT**.

---

## License

This project is released under the [GNU General Public License v3.0](LICENSE) (GPLv3).
