# Troubleshooting & Diagnostics

## Runtime Diagnostics with `hook_status.sh`

The diagnostic script `scripts/hook_status.sh` checks system health on the MIB2 unit.

Run it directly from the QNX terminal:
```sh
sh /fs/sda0/scripts/hook_status.sh
```

### What `hook_status.sh` Verifies:
1. **Supervisor Environment Limit:** Asserts `envs` count in `smartphone_integrator.json` is $\le 10$.
2. **Library Preload:** Confirms `libgal_hook.so` is resident in `/mnt/app/eso/lib/gal_dualscreen/` and executable.
3. **Active Processes:** Checks whether `gal` and `stream-player` are actively running.
4. **Socket Status:** Verifies that TCP loopback port `12346` and ACK pipe `/tmp/gal_ack.sock` are active.

---

## Verified In-Car Telemetry Benchmarks

During a 15-minute real-world test drive on a 2019 Volkswagen Golf Mk7.5 (Tegra 30, MU1367):

* **Drive Duration:** 14.8 minutes active driving (297 consecutive 3-second intervals), 26.8 minutes total connection.
* **Total Delivered Frames:** **45,300 frames** at **27.89 FPS** (93.0% efficiency against the 30.0 FPS cap).
* **Frame Drops / Overflows:** **0** (Zero macroblocking or green artifacts).
* **`stream-player` CPU:** **31.7%** total SoC average (~1.27 cores), 47.0% 95th percentile, 89.8% peak burst.
* **`gal` Daemon CPU:** **5.0%** total SoC average (~0.20 cores), 16.8% peak burst.
* **System Headroom:** **38.9% IDLE** average (~1.56 cores completely free), minimum dip 6.0% IDLE.
* **Network Bitrate:** **377.3 MB** (~1.87 Mbps average video bitrate).
* **Hardware ACK Synchrony:** **99.84%** of frames acknowledged synchronously by the Tegra GPU blitter.

---

## Diagnostic Quick Reference

| Symptom | Probable Cause | Immediate Resolution |
| :--- | :--- | :--- |
| **Severe stutter / ~3.3–4 FPS on AF_UNIX** | QNX 6.5.0 AF_UNIX buffer is hardcoded to 5 KB; 28KB–140KB frames require 6–27 round trips per frame. | Use production TCP loopback (`GAL_STREAM_TRANSPORT=tcp`, port 12346) which supports full 2 MB socket buffers. |
| **Severe stutter / ~3.3 FPS with ~300ms latency** | Multi-frame threading (`FF_THREAD_FRAME`) buffered frames while flow control withheld ACK, exhausting phone credit. | Ensure `stream-player` runs with `AV_CODEC_FLAG_LOW_DELAY` and `FF_THREAD_SLICE` (zero frame delay). |
| **First frame corrupted / green flash on connection** | Static focus mode 1 granted before `stream-player` was connected, dropping early IDR keyframe. | Enable `focus_ctl` (`GAL_FOCUS_CONTROL=1`) to hold mode 2 until player connects, triggering a fresh phone IDR. |
| **Cluster drops when shifting into reverse** | Socket write timeout too short or idle context revert timer active. | Ensure write timeout is **250ms** (`tv_usec = 250000`). Never use idle context timers while driving. |
| **Green artifacts / macroblock tearing** | Flow control disabled or unpaced USB ACKs overflowing buffers. | Ensure Player-ACK pipe (`/tmp/gal_ack.sock`) is active and written by `stream-player`. |
| **Cluster frozen on last frame** | Lingering zombie player process locking Displayable 3. | Execute `slay -9 stream-player`. Ensure `vc_player_mgr.c` executes `slay -9 -f -q stream-player` before spawn. |
| **Hook does not load at all** | More than 10 entries in `smartphone_integrator.json` `envs` array. | Run `scripts/hook_status.sh`. Trim environment overrides so `count <= 10`. |
| **Black screen / UI crash on boot** | Modern non-IBM JDK used to compile Java HMI classes. | Check byte 7 with `od` (`2e`). Recompile using IBM `javac 1.6.0-internal` targeting `-source 1.2 -target 1.2`. |
| **Script error (`^M: not found`)** | Windows CRLF line endings. | Run `sed -i 's/\r$//' <script>` before deploying to QNX. |
