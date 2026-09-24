# Welcome to the `mhi2-android-auto-video-vc` Wiki

This wiki provides comprehensive technical documentation, architectural specifications, deployment runbooks, and troubleshooting guides for running native dual-screen Android Auto projection on Harman MIB2.5 High (MHI2) head units.

---

### 🧩 Ecosystem & Referenced Projects

This project focuses on the **native C preload hook and hardware video streaming pipeline**. It is designed to work in synergy with established projects:

* 📺 **[VcMOSTRenderMqb](https://github.com/andrewleech/VcMOSTRenderMqb)** *(by [@andrewleech](https://github.com/andrewleech))* — Pioneer MOST150 video transmission and Tegra 3 OpenKODE/GLES2 rendering foundation.
* 🧭 **[NavActiveIgnore](https://github.com/jille/mib2-navignore)** (`navignore` *(by [@jille](https://github.com/jille) / [M.I.B.](https://github.com/Mr-MIBoner/M.I.B._More-Incredible-Bash))* — Baseline vehicle Java HMI patch to suppress mutual exclusion lockouts between factory maps and Android Auto.
* 🎮 **[mib2-android-auto-vc](https://github.com/chopinwong01/mib2-android-auto-vc)** *(by [@chopinwong01](https://github.com/chopinwong01))* — Companion Java HMI patch (`VCAndroidAuto_mapmode.jar`) for MFL steering wheel zoom and cluster D-pad controls.
* 🛠️ **[MIB SDK](https://gitlab.com/andrewleech/mibsdk)** *(by [@andrewleech](https://github.com/andrewleech))* — Official Dockerized QNX 6.5.0 SP1 cross-toolchain.

---

> [!CAUTION]
> **CRITICAL WARNING — RISK OF HEAD UNIT DAMAGE OR BRICKING:**  
> This software interacts directly with low-level QNX RTOS services, hardware graphics controllers, and vehicle bus gateways.  
> * **Software Risk:** Improper configuration, exceeding supervisor environment limits (Rule of 10), or deploying incompatible Java bytecode will cause bootloops, supervisor crashes, or complete loss of the vehicle's infotainment UI (black screen).  
> * **Hardware / System Risk:** Flash memory corruption, overheating from unthrottled decoding workloads, or bus desync can permanently disable the MMX unit (requiring bench flashing / hardware recovery).  
> * **Development Disclosure ("Vibe Coded"):** This project was heavily "vibe coded" and iteratively developed with various Large Language Models (LLMs)—including **Google Gemini**, **Anthropic Claude**, and **OpenAI GPT**. While rigorously bench-tested and telemetry-audited on real vehicle hardware, AI-assisted low-level code inherently demands thorough review before deployment.  
> **Never modify files in `/lib` or `/usr/lib`. Proceed strictly at your own risk.**

---

## 📚 Wiki Contents

1. **[Hardware & Firmware Compatibility Matrix](Compatibility-Matrix.md)**
   * Supported SoCs (Tegra 3 vs i.MX6)
   * Tested train versions (`MHI2_ER_VWG13_P4521_MU1367`)
   * MOST150 optical bus & Virtual Cockpit screen specifications
2. **[Installation & Safety Runbook](Installation-and-Safety-Guide.md)**
   * Safe deployment via SD Card
   * The OEM Read-Only Safety Rule (never touch `/lib`)
   * The Supervisor Environment Variable Limit ("Rule of 10")
   * Step-by-step installation with `enable_hook.sh` and clean uninstallation
3. **[Architecture Deep-Dive](Architecture-Deep-Dive.md)**
   * Google Automotive Link (`gal`) internal structure
   * Dynamic heap injection of `ProtocolEndpointBase` & focus controller (`focus_ctl`)
   * TCP loopback transport (`127.0.0.1:12346` / 2MB buffer) vs `AF_UNIX` 5KB buffer bottleneck
   * Low-delay zero-frame-delay flow control vs 3.3 FPS deadlock failure
   * Tegra 3 `glDrawTextureNV` hardware blitter
4. **[Companion HMI Integration](Companion-HMI-Integration.md)**
   * Baseline requirement: `NavActiveIgnore` (suppressing factory map lockouts)
   * Full HMI features: `VCAndroidAuto_mapmode.jar` / `mib2-android-auto-vc`
   * IBM J9 VM compiler requirements (avoiding `VerifyError` bootloops)
   * MFL steering wheel zoom routing
5. **[Troubleshooting & Diagnostics](Troubleshooting-and-Diagnostics.md)**
   * Diagnostic inspection with `hook_status.sh`
   * Telemetry benchmarks (27.89 FPS, 0 drops, 38.9% idle headroom)
   * Diagnosing AF_UNIX buffer starvation, `FF_THREAD_FRAME` phone credit deadlocks, and early IDR drops
6. **[Build Environment & Toolchain](Build-Environment.md)**
   * Setting up the Docker MIB SDK
   * Cross-compiling `libgal_hook.so`
   * Compiling `stream-player` with minimal FFmpeg

---

## 🗺️ Roadmap & Implementation Status

* [x] **Dynamic Secondary Video Focus & Mode Switch:**
  * Implemented via `focus_ctl.c` / `focus_ctl.h`. Holds secondary sink in mode 2 (native) until `stream-player` connects, triggering an immediate native SPS/PPS + IDR keyframe from the phone.
  * Verified Kombi map readiness check (`GAL_FOCUS_WAIT_KOMBI`).
  * Seamless RVC transitions via 250ms socket buffer with live secondary sink.
* [x] ~~**Unix Domain Socket Migration (`AF_UNIX`)**~~ *(Evaluated & Abandoned — Proven RTOS Limitation)*:
  * Measured on-car via `vc_sockbuf`: QNX 6.5.0 hardcodes `AF_UNIX` buffers to **7,168 bytes send / 5,120 bytes receive**, and `setsockopt(SO_SNDBUF/SO_RCVBUF)` is completely ignored. Transmitting 28 KB–140 KB H.264 video frames required 6 to 27 round trips per frame, collapsing framerate to **3.3–4 FPS**. Furthermore, both families are served by `io-pkt`. TCP loopback (`tcp://127.0.0.1:12346`) with 2 MB buffers is the definitive production transport.
* [x] **Zero Frame Delay Decoding Pipeline:**
  * Replaced `FF_THREAD_FRAME` with `AV_CODEC_FLAG_LOW_DELAY` (`FF_THREAD_SLICE`) to eliminate the 3.3 FPS / 300ms phone credit timeout deadlock.
* [ ] **Hardware NVSS / NvMedia Video Decoder Renderer:**
  * Transition from software multi-threaded FFmpeg decoding to hardware video decoding via **NvSS / NvMedia** (`/dev/nvss`, Nvidia Tegra hardware video decoder), substantially cutting Cortex-A9 CPU utilization.
* [x] ~~**GPS Sensor Uncertainty Hook (Tunnel Loss Prevention)**~~ *(Abandoned — Proven Architectural Dead-End)*:
  * Artificially clamping accuracy corrupted Google Maps' Extended Kalman Filter ($R_k \to 0$), causing compass spinning and route flapping. Removed; vehicle's rock-solid Kombi / ESP dead-reckoning hardware passes through uncorrupted.

---

## ⚠️ Important Disclaimer
This project is an experimental research endeavor intended strictly for personal study and educational exploration. Modifying automotive infotainment firmware carries inherent risks of permanent bricking or software instability. Always keep verified eMMC/NAND backups before modifying unit configurations.
