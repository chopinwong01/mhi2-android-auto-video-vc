# Installation & Safety Guide

## Strict Safety Policies

Before deploying to an active vehicle, you must understand the two critical safety rules for the Harman MIB2 platform:

> [!CAUTION]
> ### 1. The OEM Read-Only Safety Rule
> **NEVER overwrite or modify files in `/lib` or `/usr/lib` on the MIB2 root partition.**  
> Modifying or replacing system libraries can corrupt KD/OpenMAX graphics, break system services, and permanently brick the head unit.  
> * **Safe Installation Pattern:** Custom shared libraries reside strictly on the internal app partition at `/mnt/app/eso/lib/` and are injected at runtime via `LD_PRELOAD`.

> [!WARNING]
> ### 2. The Supervisor "Rule of 10"
> The MIB2 `smartphone_integrator` daemon has a hardcoded limit of **10 environment variables** in its `envs` array inside `smartphone_integrator.json`.  
> If an 11th entry is added, **the supervisor silently discards the entire environment array on boot**. The hook will not load, no logs will be written, and Android Auto will start without preloading. `enable_hook.sh` enforces this constraint automatically.

---

## Prerequisites

1. **SD Card:** Formatted as FAT32.
2. **Terminal Access:** D-PDU / D-Link DUB-E100 USB-to-Ethernet adapter (or UART console) with Telnet / SSH access to QNX IP (`10.81.225.46` by default).
3. **Firmware Check:** Ensure unit is running `MHI2_ER_VWG13_P4521_MU1367` (or compatible Harman MHI2).

---

## Step-by-Step Installation

### 1. Preparing the SD Card
Copy the following files to your SD card (inserted into Slot 1 `/fs/sda0`):

```text
/fs/sda0/
├── libgal_hook.so
├── stream-player
├── scripts/
│   ├── enable_hook.sh
│   ├── disable_hook.sh
│   ├── hook_status.sh
│   ├── lib_app_mount.sh
│   └── gal_dualscreen.conf.example
└── config.txt
```

### 2. Running `enable_hook.sh`
Log into the QNX terminal on the head unit and execute:

```sh
cd /fs/sda0
sh scripts/enable_hook.sh
```

**What `enable_hook.sh` does safely:**
1. Verifies the current `smartphone_integrator.json` has fewer than 10 environment overrides.
2. Creates a timestamped persistent backup of the factory configuration at `/mnt/system/etc/eso/production/smartphone_integrator.json.gal-dualscreen.original`.
3. Copies `libgal_hook.so` into `/mnt/app/eso/lib/gal_dualscreen/libgal_hook.so`.
4. Adds `LD_PRELOAD=/eso/lib/gal_dualscreen/libgal_hook.so` to the `gal` child configuration.
5. Re-validates the JSON syntax using `/mnt/app/eso/bin/jsonlint`.
6. Safely remounts all partitions read-only.

### 3. Verifying Installation with `hook_status.sh`
Run the diagnostic status check:

```sh
sh scripts/hook_status.sh
```

Ensure all validation checks display `[OK]`.

### 4. Rebooting the Head Unit
Force a complete hard reboot of the MIB2 unit by holding the power button down for **10 seconds** until the display turns off and the VW splash screen appears.

---

## Uninstallation / Factory Reset

To completely revert the unit to its 100% factory state:

```sh
cd /fs/sda0
sh scripts/disable_hook.sh
```

`disable_hook.sh` restores the original factory `smartphone_integrator.json` from backup, removes custom preloads, and deletes the `/mnt/app/eso/lib/gal_dualscreen` directory.
