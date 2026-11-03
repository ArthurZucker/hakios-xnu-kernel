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
gpu: virtio GPU in slot 31: a 1125x2436 2D resource on scanout 0, driven from the console's framebuffer
gpu: 3D: a virgl2 context on ctx 1 -- the device made a GL context for it on the host
gpu: cmd 0x207 (724 bytes) -> reply 0x1100
gpu: 3D: the readback holds 671740 pixels of the triangle's colour and 2068760 of the background's, of 2740500
gpu: 3D: a triangle on resource 2 drawn by the host's GL, read back into the framebuffer, on scanout 0
nfc_test: all 14 checks pass
```

A **triangle**, not a colour fill: `hakios.virtio_gpu.c` opens a virgl2 context, creates a BGRA8 render target and an
R8_UNORM vertex buffer, sends one `SUBMIT_3D` carrying a 724-byte virgl command stream (two TGSI shaders, blend/
rasterizer/depth state, a surface, the vertices as a `RESOURCE_INLINE_WRITE`, a `CLEAR`, and a `DRAW_VBO`), and reads
the result back with `TRANSFER_FROM_HOST_3D` into the console framebuffer, which the 2D scanout shows. The counts
add to exactly the panel's 2,740,500 pixels, and a `screendump` of the running guest shows the triangle.

What this does **not** mean: there is still no OpenGL in the guest. No GL driver, no shader compiler, no window
server; the shaders are TGSI text baked into the driver and the geometry is a constant. It establishes that the 3D
half of the device works end to end from this kernel -- context, resources, command stream, host rasterisation,
readback -- which is what a guest GL driver would be built on. llvmpipe is a software rasteriser and the phone's GPU
is a Mali-G68 the kernel has no driver for, so this is a development target, not the phone's GPU.

Two encoding facts cost a day between them, and both fail silently because a SUBMIT_3D replies `0x1100` whether or
not vrend accepted the stream inside it:

| # | symptom | what it was |
|---|---|---|
| 1 | stream refused with `failed to dispatch CLEAR: 22` | a `CREATE_OBJECT` length of `4 + ndw` instead of `5 + ndw` -- the handle dword counts, and one short slips vrend's parser one command out of step |
| 2 | every later command refused (`0x1205`) | `gpu_submit_large()` set descriptor 0's *length* but not its *address*, leaving the device reading `buffer_mem`; and `gpu_command()` never re-set the address afterwards, so it stayed wrong |

The stream was validated byte-for-byte against virglrenderer 1.3.0 on the host before it went into the kernel: a
harness compiles the driver's own `gpu_draw_triangle_3d()` (extracted from the source, not copied) and reads the
result back. That is what found both bugs above without a boot cycle per attempt.


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
