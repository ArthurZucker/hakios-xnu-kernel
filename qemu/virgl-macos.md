# 3D in the simulator: virgl on macOS

`virtio-gpu-gl-device` gives the guest a real virgl 3D device — the thing a GL driver in the guest talks
to — and it now runs here. Getting it up took five separate things that each looked like "3D is
impossible on a Mac". None of them was.

```console
make qemu-b651-gl QEMU_BIN=/tmp/qemu-src/build/qemu-system-aarch64 SECS=60
# or by hand:
QEMU_BIN=<path>/qemu-system-aarch64 QEMU_GL=1 ./qemu/run-b651.sh 60
```

## Why it failed, in the order it failed

| # | what it said | what it actually was |
|---|---|---|
| 1 | `virtio-gpu-gl-device` does not exist | Homebrew's QEMU is built without virglrenderer. Build QEMU with `--enable-virglrenderer`. |
| 2 | `virgl could not be initialized: 22`, `Unable to create OpenGL context >= 3.0` | Homebrew's `libvirglrenderer` is built **without EGL**: `pkg-config --variable=epoxy_has_egl epoxy` is `0` on macOS, so vrend has no way to make a GL context and the whole 3D path is compiled out. |
| 3 | a segfault at `libGL.dylib`glGetString +12 | Homebrew's `libepoxy` is also built without EGL, so its `gl*` entry points resolve through Apple's CGL and cannot drive a Mesa EGL context. |
| 4 | shader error `version '300' is not supported` | QEMU's blit shaders are `#version 300 es`, which a desktop core context rejects. |
| 5 | `Unable to create OpenGL context >= 3.0` only once the **guest** started, `EGL_BAD_ATTRIBUTE` on every version | Mesa refuses to create a context that shares a context current on a **different thread**. QEMU's headless context belongs to the main thread; vrend initialises on the guest's. |

## What made it work

All five are build-linkage and threading problems, not platform limits.

1. **QEMU from source**, `--enable-virglrenderer` — `/tmp/qemu-src`.
2. **libepoxy built with `-Degl=yes`** — Homebrew's has EGL compiled out. Its `epoxy.pc` then reports
   `epoxy_has_egl=1`, which both virglrenderer's and QEMU's meson check.
3. **virglrenderer built with `-Dplatforms=egl`** against that epoxy.
4. **QEMU stops linking a second `libEGL`** when epoxy already carries it: two instances means a display
   initialised through one is invisible to `eglGetCurrentContext()` in the other.
5. **QEMU does not share its EGL display with virgl off DRM hosts.** On macOS the display is surfaceless
   and vrend runs on another thread, so virgl makes its own surfaceless display instead
   (`VIRGL_RENDERER_USE_EGL | VIRGL_RENDERER_USE_SURFACELESS`).

Plus two small shims: Mesa's `libEGL` exports the EGL 1.5 core names only, so the KHR fence entry points
virglrenderer calls get four forwarding functions (`egl_khr_fence_shim.c`); and QEMU's blit shaders are
rewritten from `#version 300 es` to `#version 330 core`.

The display backend is `-display egl-headless`, rendering into a surfaceless Mesa display (llvmpipe,
`GL 4.6 (Core Profile)`). No window, no DRM, no dma-buf, no GBM.

## Verified

```text
hakios-kernel: 8 drivers built in (.config): virtio_sound virtio_gpu virtio_input virtio_net virtio_blk epd fb nfc
gpu: virtio GPU in slot 31: a 1125x2436 2D resource on scanout 0, driven from the console's framebuffer.
     2D only: this driver sends no 3D command (the device has virgl, but nothing here uses it)
nfc_test: all 14 checks pass
```

`(the device has virgl, but nothing here uses it)` is the guest reading `VIRTIO_GPU_F_VIRGL` off the
device's feature word: the 3D device exists and is offered to the guest. `hakios.virtio_gpu.c` still
speaks only the 2D command set, so **no OpenGL context runs in the guest yet** — what this establishes
is that the machine has a 3D device for one to run on. llvmpipe is a software rasteriser, and the phone's
GPU is a Mali-G68 the kernel has no driver for, so this is a development target, not the phone's GPU.

## Reproducing from scratch

```console
git clone --depth 1 https://github.com/anholt/libepoxy.git /tmp/epoxy-src
git -C /tmp/epoxy-src apply <repo>/qemu/virgl-patches/libepoxy.patch
meson setup /tmp/epoxy-build /tmp/epoxy-src -Degl=yes -Dglx=no -Dx11=false --prefix=/tmp/epoxy-install
ninja -C /tmp/epoxy-build install

git clone --depth 1 -b 1.3.0 https://gitlab.freedesktop.org/virgl/virglrenderer.git /tmp/virgl-src
git -C /tmp/virgl-src apply <repo>/qemu/virgl-patches/virglrenderer.patch
cp <repo>/qemu/virgl-patches/egl_khr_fence_shim.c /tmp/eglshim/
PKG_CONFIG_PATH=/tmp/epoxy-install/lib/pkgconfig:/opt/homebrew/lib/pkgconfig \
  meson setup /tmp/virgl-build /tmp/virgl-src -Dplatforms=egl --prefix=/tmp/virgl-install
ninja -C /tmp/virgl-build install

# QEMU v11.1.3, then its patch
git -C /tmp/qemu-src apply <repo>/qemu/virgl-patches/qemu.patch
PKG_CONFIG_PATH=/tmp/virgl-install/lib/pkgconfig:/tmp/epoxy-install/lib/pkgconfig:/opt/homebrew/lib/pkgconfig \
  /tmp/qemu-src/configure --enable-virglrenderer --enable-opengl --disable-pvg --enable-fdt=system
```

`DYLD_LIBRARY_PATH` must contain `/opt/homebrew/lib` when running: epoxy `dlopen`s `libEGL.dylib` by bare
name, and Mesa's is the only one on this machine.
