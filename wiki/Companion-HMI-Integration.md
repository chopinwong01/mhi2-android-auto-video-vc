# Companion HMI Integration

While `libgal_hook.so` and `stream-player` manage the video stream at the QNX RTOS level, integrating with the instrument cluster and steering wheel controls requires interfacing with Volkswagen's Java HMI layer (`lsd.jxe`).

---

## 1. Baseline Requirement: `NavActiveIgnore`

* **What it does:** Suppresses the factory navigation mutual exclusion check.
* **Why it is necessary:** Stock MIB2 firmware enforces that only one navigation source may run at a time. Starting route guidance in Android Auto terminates factory cluster maps, and opening the cluster map cancels Android Auto navigation.
* **The Baseline Experience:** Installing `NavActiveIgnore.jar` satisfies the minimum requirement: the cluster video feed projects smoothly without conflicting with the stock infotainment navigation state.

---

## 2. Full HMI Features: `mib2-android-auto-vc`

For interactive controls, the companion project [`mib2-android-auto-vc`](https://github.com/adi961/mib2-android-auto-vc) (by **@adi961**) provides `VCAndroidAuto.jar`, extended as `VCAndroidAuto_mapmode.jar`.

### Key Capabilities
1. **Steering Wheel Zoom:** Intercepts Multi-Function Steering Wheel (MFL) right-side physical D-pad Up/Down button events (BAP key events 6 and 7) and converts them to Android Auto map zoom in/out commands.
2. **D-Pad Route Guidance Injection:** Passes cluster navigation menu button events into Android Auto.
3. **Maneuver Banner Suppression:** Suppresses duplicate OEM turn-by-turn prompts that overlap with the projected map.
4. **Listener Null Guards:** Prevents NullPointerExceptions during app transitions when `NavigationListener` is not yet bound.

---

## 3. Strict IBM JDK 1.2 Requirement (The "Lost UI" Rule)

> [!CAUTION]
> **Never compile MIB2 Java classes with modern OpenJDK or Oracle javac!**  
> The MIB2 Java HMI runs on an **IBM J9 Virtual Machine**.  
> * Bytecode MUST be compiled with the **IBM Java SDK (`javac 1.6.0-internal`) targeting `-source 1.2 -target 1.2`**.
> * The bytecode major version MUST strictly be `0x2E` (46 decimal).
> * If modern Java compilers are used, the IBM J9 VM verifier throws an instant `VerifyError` on boot, **immediately crashing the center console and losing all vehicle UI.**

### Running Legacy 32-bit IBM JDK on Modern Linux/WSL2
Modern 64-bit Linux kernels enforce `PT_GNU_STACK` security and refuse to execute 32-bit legacy binaries with executable stack flags (`PF_X`).

To run the legacy IBM compiler on WSL2 or Linux, clear the executable stack bit in `libjvm.so`:

```python
import os, struct

def clear_exec_stack(path):
    with open(path, 'r+b') as f:
        elf = f.read(52)
        if elf[:4] != b'\x7fELF' or elf[4] != 1: return
        phoff, phentsize, phnum = struct.unpack('<III', elf[28:40])[0], struct.unpack('<H', elf[42:44])[0], struct.unpack('<H', elf[44:46])[0]
        for i in range(phnum):
            f.seek(phoff + i * phentsize)
            p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags = struct.unpack('<IIIIIII', f.read(28))
            if p_type == 0x6474e551: # PT_GNU_STACK
                f.seek(phoff + i * phentsize + 24)
                f.write(struct.pack('<I', p_flags & ~1))
                print(f"Cleared exec stack on {path}")
                break
```
