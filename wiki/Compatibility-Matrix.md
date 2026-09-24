# Hardware & Firmware Compatibility Matrix

## Supported Hardware

| Component | Specification | Compatibility Status | Notes |
| :--- | :--- | :--- | :--- |
| **Head Unit (MMX)** | Harman MIB2.5 High (MHI2) | **Supported** | Multi-board system with MMX (Multimedia Extension) and RCC (Radio & Car Control). |
| **SoC** | Nvidia Tegra 30 (T30) | **Supported** | Quad-core ARM Cortex-A9 @ 1.4 GHz + 12-core GeForce ULP GPU. |
| **SoC (Alternative)** | NXP / Freescale i.MX6 Quad (MHI2Q) | ❌ **Unsupported** | MHI2Q uses a different graphics pipeline (Vivante GPU) and different memory layout. Do not run on i.MX6! |
| **Operating System** | QNX Neutrino RTOS 6.5.0 SP1 | **Required** | Native POSIX RTOS with OpenKODE / KD graphics. |
| **Cluster Display** | 12.3" Virtual Cockpit (FPK / AID) | **Supported** | Receives navigation video feed over MOST150 optical bus (`/dev/mlb/isoTX2`). |

---

## Verified Firmware Trains

This project has been extensively field-tested and telemetry-verified on:

* **Train Version:** `MHI2_ER_VWG13_P4521_MU1367`
* **Brand & Region:** Volkswagen Europe (VW Golf Mk7.5)
* **MMX Software Version:** MU1367

### Other VAG Brands (Audi, Skoda, SEAT, Porsche)
* The core C hook (`libgal_hook.so`) and `stream-player` run natively at the QNX RTOS / Tegra 3 level and are generic to Harman MHI2 units running GAL.
* However, the Java HMI layer (`VCAndroidAuto_mapmode.jar`) interacts with `de.vw.mib.asl` namespaces and VW-specific BAP keys. On Audi (Audi Virtual Cockpit) or Skoda, you should use `NavActiveIgnore` baseline or brand-specific ASL patches.

---

## Display Resolutions & Hardware Constraints

### 1. Instrument Cluster Display
* **Physical Resolution:** 1440×540 LCD
* **Virtual Screen Injection Resolution:** **800×480 @ 30 fps**
* **Routing:** Display ID `4`, Context `70`, Displayable `3` over MOST150 `/dev/mlb/isoTX2`.

### 2. Tegra 3 Decoding Capacity & The 30 FPS Cap
* The Nvidia Tegra 3 hardware video decoder cannot sustain concurrent 1080p@60fps primary video alongside a 800×480 secondary video stream.
* **The Rule:** Forcing the primary screen to **30 fps** (`"supportedFrameRates": [ 30 ]` in `gal.json`) cuts decode workloads in half, preventing thermal throttling, frame stutter, and audio/video desync.
