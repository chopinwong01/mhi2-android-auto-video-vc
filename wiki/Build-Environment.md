# Build Environment & Toolchain

All native binaries for Harman MHI2 are built using the QNX 6.5.0 SP1 cross-compilation toolchain encapsulated inside the official **MIB SDK** Docker container.

---

## 1. Prerequisites

* Docker installed on host (Linux, macOS, or Windows via WSL2).
* Pull the MIB SDK image:
  ```bash
  docker pull registry.gitlab.com/andrewleech/mibsdk:latest
  ```

---

## 2. Building the Preload Hook (`libgal_hook.so`)

From the root of the repository:

```bash
make hook
```

### Under the Hood
The build target invokes Docker and runs:
```sh
arm-unknown-nto-qnx6.5.0eabi-gcc -O2 -Wall -Wextra -Werror -shared -fPIC \
    -I./src -DGAL_HOOK_BUILD="release" \
    ./src/*.c \
    -lsocket \
    -o ./libgal_hook.so
```
* **Output:** `libgal_hook.so` (~60 KB).

---

## 3. Building the Stream Player (`stream-player`)

`stream-player` links against minimal static FFmpeg libraries (`libavcodec.a`, `libavformat.a`, `libavutil.a`) and QNX EGL/GLES2 graphics libraries:

```bash
cd player
make
```

### Manual Docker Build Command
If building outside the Makefile:
```bash
docker run --rm -v $(pwd)/../..:/work -w /work/mhi2-android-auto-video-vc/player \
  registry.gitlab.com/andrewleech/mibsdk:latest sh -c '
    . /etc/qnx/env
    qcc -Vgcc_ntoarmv7 -c -O2 -Wc,-Wall -I/work/VcMOSTRenderMqb/build/ffmpeg-mini -DNDEBUG \
        -I. -I/usr/qnx650/target/qnx6/usr/include \
        -EL -DVARIANT_le -DVARIANT_v7 -DBUILDENV_qss \
        opengl_gpu.cc -o opengl_gpu.o
    qcc -V4.4.2,gcc_ntoarmv7le opengl_gpu.o \
        -L/usr/qnx650/target/qnx6/armle-v7/lib -L/usr/qnx650/target/qnx6/armle-v7/usr/lib \
        -Wl,--start-group \
        /work/VcMOSTRenderMqb/build/ffmpeg-mini/libavformat/libavformat.a \
        /work/VcMOSTRenderMqb/build/ffmpeg-mini/libavcodec/libavcodec.a \
        /work/VcMOSTRenderMqb/build/ffmpeg-mini/libavutil/libavutil.a \
        -Bstatic -lbz2 -lz -lcpp -Bdynamic \
        -Wl,--end-group \
        -lEGL -lGLESv2 -lsocket -lm -lc \
        -o stream-player
    ntoarmv7-strip -s stream-player
'
```
* **Output:** `player/stream-player` (~4.5 MB stripped executable).

---

## 4. Network Deployment to the Car (`scripts/deploy_to_car.sh`)

If your car's head unit is connected via Ethernet (D-Link DUB-E100 adapter):

```bash
sh scripts/deploy_to_car.sh 10.81.225.46
```

The script transfers `libgal_hook.so` and `stream-player` directly to `/mnt/app/eso/lib/gal_dualscreen` and `/mnt/app/eso/bin/` over SCP, installs the configuration, and prompts you to reboot the head unit.
