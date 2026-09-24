# stream-player

Hardware-accelerated video renderer for Harman MIB2.5 High (Nvidia Tegra 30, QNX 6.5.0) cluster displays.

`stream-player` decodes secondary Android Auto H.264 video frames received over local TCP loopback (`tcp://127.0.0.1:12346`) and renders them directly onto the Volkswagen Virtual Cockpit (Displayable 3, Context 70 / MOST150 Display 4).

---

## Technical Highlights & Architecture

### 1. Zero Frame Delay Low-Delay Decoding (`AV_CODEC_FLAG_LOW_DELAY`)
* **The 3.3 FPS Deadlock Failure:** Previous builds attempted multi-core parallel frame decoding via `FF_THREAD_FRAME`. In FFmpeg, frame threading holds onto each decoded frame until the subsequent packet is queued. However, under hardware-paced flow control, the render ACK is withheld until that frame is shown. When the phone runs low on sliding-window credit, it stops sending packets until an ACK arrives. This created a circular deadlock: the phone waited for an ACK, while FFmpeg waited for the next packet to release the frame. Every frame stalled until the phone-side ~300ms timeout expired, collapsing playback to **3.3 FPS with 313ms latency**.
* **The Resolution:** `stream-player` configures `AV_CODEC_FLAG_LOW_DELAY` with `FF_THREAD_SLICE`. Decoded frames are presented immediately without holding delays. Because 800×480 H.264 decode takes only ~1 ms on a Cortex-A9 core, slice threading provides immediate presentation and returns the render ACK synchronously at rock-solid 30 FPS.

### 2. Transport Architecture: TCP Loopback vs AF_UNIX Buffer Limits
* **The AF_UNIX 5KB Buffer Failure:** An attempt was made to stream frames over a Unix domain socket (`unix:///tmp/gal_video.sock`). However, on QNX 6.5.0 SP1, `AF_UNIX` stream sockets are hardcoded to a fixed buffer of **7,168 bytes send / 5,120 bytes receive**, and `setsockopt(SO_SNDBUF/SO_RCVBUF)` is completely ignored. Because H.264 video frames range from 28 KB (p95) to 140 KB (IDR keyframes), transmitting over AF_UNIX required 6 to 27 round trips per frame through the 5 KB window, incurring severe scheduler hop thrashing and collapsing framerate to 3.3–4 FPS.
* **Production TCP Loopback:** `tcp://127.0.0.1:12346` accepts full 2 MB socket buffers (`SO_SNDBUF` / `SO_RCVBUF`), allowing even 140 KB keyframes to cross in a single atomic write. Because both `AF_UNIX` and `AF_INET` are handled by `io-pkt` on QNX 6.5.0, TCP loopback is the proven high-throughput production transport.

### 3. Hardware Blitter & Fragment Shader Pipeline
* **Tegra 3 GLES2 Implementation:** The MIB2 Harman Tegra 30 GLES2 stack disables online shader compilation (`GL_SHADER_COMPILER == 0`). `stream-player` renders via Nvidia's hardware blit extension `GL_NV_draw_texture` (`glDrawTextureNV`) or GPU texture mapping.
* Uploads Y, U, and V planes as separate luminance textures and performs BT.601 color matrix math directly in hardware.

### 4. End-to-End Hardware Flow Control
* After each frame is swapped onto the display surface via `eglSwapBuffers()`, `stream-player` writes a 1-byte ACK (`0x01`) to `/tmp/gal_ack.sock`.
* This delivers hardware backpressure directly back to Android Auto, keeping phone encoding locked synchronously to physical display presentation and eliminating buffer overflows.

### 5. Resilient Lifecycle & DMDT Context Management
* **Keyframe Gate:** Gated to display frames only after the initial IDR keyframe has been decoded.
* **Context Restoration:** Cleanly restores factory instrument dials (Context 33) upon normal teardown, SIGTERM/SIGINT, or unhandled exit.

---

## Attribution & Licenses

* **Base Architecture:** Derived and adapted from the `VcMOSTRenderMqb` project by **Andrew Leech**.
* **FFmpeg:** Video decoding relies on static builds of FFmpeg (`libavcodec`, `libavformat`, `libavutil`), licensed under LGPL v2.1+.
