# The HiBreak Pro as it reports itself (first contact, 2026-11-02)

Read over `adb` as the `shell` user. No root on this unit (`su` is absent), so everything that needs root is listed as
**blocked**, not guessed. Values below are copied from the phone.

## Identity — verified

| | |
|---|---|
| Serial | `B651DNB2FK0D006000554` |
| `ro.product.model` / `name` / `device` | `HiBreak` / `HiBreak` / `Smartphone` |
| `ro.build.versionCode` | `B651 NO_EEA_M2` → the **BW (monochrome)** variant, EEA |
| `ro.build.ProductCode` | `BW` |
| Android | **14**, SDK 34, build `UP1A.231005.007`, `Bigme_HiBreak_V1.0_20260306` |
| Build date | 2026-03-06 |
| `ro.product.board` | `k6877v1_64_k419` |
| `ro.soc.model` / `ro.soc.manufacturer` | `MT6877` / `Mediatek` |
| `ro.hardware` | `mt6877` |
| Kernel | `4.19.191 #3 SMP PREEMPT`, `aarch64` |
| `CONFIG_MTK_PLATFORM` | `"mt6853"` — the vendor tree reuses the MT6853 platform for MT6877 |

The HiBreak Pro in this form is **not** the model the KOReader userpatch expects (`product=hibreak_eea`,
`hardware=mt6877`): here `product` is `HiBreak`. Marketing names and product strings diverge again.

## Storage — A/B, dynamic partitions, on UFS

```text
ro.build.ab_update = true          ro.boot.slot_suffix = _b      (running slot b)
ro.boot.dynamic_partitions = true
ro.boot.boot_devices = bootdevice,soc/11230000.mmc,...,soc/11270000.ufshci
                       ^ the 11270000 UFS node is in the list: this unit boots from UFS, not eMMC
```

Partitions that matter to this port, from `/dev/block/by-name/`:

| Partition | Why |
|---|---|
| `boot_a` / `boot_b`, `init_boot_a/b`, `vendor_boot_a/b` | what a HakiOS image would have to replace |
| `lk_a` / `lk_b` | the bootloader whose flash decisions matter |
| `dtbo_a` / `dtbo_b` | board device-tree overlays — the only source for the EPD block's properties |
| **`waveform`** | **the e-paper waveform LUTs, on their own partition** |
| **`vcom`** | **the panel's VCOM calibration, on its own partition** |
| `seccfg` | lock state, read off flash rather than asked of the bootloader |
| `nvdata`, `nvram`, `proinfo` | radio / product identity; the ones that cannot be regenerated |
| `mcupm_a/b`, `spmfw_a/b`, `sspm_a/b`, `scp_a/b`, `gz_a/b`, `tee_a/b` | the firmware blocks the SoC's power and DRM stacks need |

`waveform` and `vcom` existing as first-class partitions confirms the shape `EPDC.md` predicted: the LUTs and the
per-unit VCOM value are separate flagships in the vendor's own layout, not buried inside a kernel blob.

## The e-ink block IS in the device tree

This contradicts the conclusion drawn from the MT6877 Linux source (which has no EPD node for this SoC): the
**phone's own** tree has one. Node names and property names are readable without root even though their values are
not.

```text
/eink                     compatible, name, eink_panel_type,
                          eink_fb_width, eink_fb_height, eink_width, eink_height,
                          eink_bit_num, eink_data_len,
                          eink_fdl, eink_fel, eink_fsl, eink_fbl,
                          eink_ldl, eink_lel, eink_lsl, eink_lbl,
                          eink_max_fps, eink_fresh_hz, eink_dynamic_fps,
                          eink_mirror, eink_anti_alias, eink_gdck_sta,
                          eink_glx16_disable, eink_lgonl
/epd_pmic_det             a second node: the EPD PMIC's detection
```

The `f..`, `l..` and `s..` groups are panel timing triplets (frame / line / source start-length-end) — exactly the
DSI-timing parameters MediaTek's `extd_epd.c` programs before handing off. `eink_glx16_disable` names a *waveform
mode* (GL16 is an e-ink grey-level waveform), which means the vendor driver already knows about waveform modes in the
kernel.

Reserved-memory blocks reserved by name:

```text
mblock-20-waveform    mblock-21-eink0    mblock-22-eink1
mblock-23-vcom        mblock-24-eink2    mblock-25-eink_temp_buf    mblock-40-vcom
```

Two VCOM blocks and a temperature buffer: the panel needs a temperature-compensated waveform, and it keeps two VCOM
copies (a primary and a backup — consistent with VCOM being per-unit calibration that must survive a bad write).

VCOM driver chips present as pinctrl nodes: `SYV776Avcom@2`, `SYV776Avcom@3`, and `tps65185vcom@7`,
`tps65185vcom@8`. So the board supports two VCOM chip options — which one this unit uses is a root-level question.

## The kernel config, read in full

`/proc/config.gz` is world-readable here, so the vendor's configuration is known rather than inferred:

| | |
|---|---|
| `CONFIG_MACH_MT6877=y`, `CONFIG_MTK_LPM_MT6877=y` | the SoC is really in the build |
| `CONFIG_SCSI_UFS_MEDIATEK=y` | MediaTek's UFS driver — the thing HakiOS has no counterpart for |
| `CONFIG_USB_MTU3=y`, `CONFIG_USB_MTU3_DUAL_ROLE=y`, `CONFIG_USB_GADGET=y`, `CONFIG_USB_F_FS=y` | MediaTek's own USB device controller is in the kernel and working: **USB device mode demonstrably exists on this silicon**, it is only absent from Mu-Silicium |
| `CONFIG_ARM_MEDIATEK_CPUFREQ` **not set**; `CONFIG_MTK_CPU_FREQ=y` | cpufreq is the vendor path, confirming HARDWARE.md |
| `# CONFIG_EPD_TEMP_DET is not set` | the *only* EPD symbol in the mainline-shaped part of the config; the rest lives under `CONFIG_MTK_*` |
| `CONFIG_DRM_PANEL_NT36672A_RT4801_VDO`, `..._TRULY_TD4330_CMD` | DSI panels for *other* boards in the same tree; neither is this phone's e-ink panel |

The USB finding is the important one: it is **not** that the hardware cannot do USB device mode, it is that
Mu-Silicium has no MediaTek implementation of it. That keeps GATE1's delivery problem a software problem.

## Gate 1 status: the phone looks locked, and the toggle is off

```text
ro.boot.flash.locked          = 1
ro.boot.vbmeta.device_state   = locked
ro.boot.vbmeta.avb_version    = 1.2
ro.boot.verifiedbootstate     = green
ro.boot.veritymode            = enforcing
sys.oem_unlock_allowed        = 0     ← the developer-options "OEM unlocking" toggle is disabled
ro.db.secure                  = 1
```

`sys.oem_unlock_allowed=0` is the blocker: with it at 0 the OEM-unlocking switch in Developer options is greyed out,
so `fastboot flashing unlock` has nothing to authorise and the bootloader will refuse an unsigned image. It comes
from `seccfg`, which is only readable with root or over BROM.

Two caveats, stated because they are the difference between "give up" and "check":

- these are **props**, and props can be spoofed by exactly the tools this phone's ecosystem uses (`tricky_store`
  exists for this purpose). The authoritative read is `seccfg` over BROM, which `GATE1.md` step 6 covers.
- **no root on this unit**: `su` is absent, `dmesg` and `/sys/firmware/fdt` are denied to `shell`, and the
  `/dev/block/by-name/*` nodes are unreadable. So `mtkclient`/BROM is currently the *only* path to the partitions —
  which requires the same unlock that is in question.

## What this changes

| Claim before first contact | Now |
|---|---|
| "no e-ink controller in any MT6877 source" | true of the Linux source; **false of this phone** — `/eink` and `epd_pmic_det` are in its tree |
| waveform + VCOM are "likely on `vendor_a.img`" | they are **their own partitions** (`waveform`, `vcom`), and are reserved-memory blocks too |
| USB device mode impossible | **exists**: `CONFIG_USB_MTU3` + gadget + `f_fs` are in the vendor kernel |
| GPU is "Mali-G68 MC4" | `ro.hardware.vulkan` is `mali`; the exact model is still unverified |
| phone may be unlockable | locked, **and `sys.oem_unlock_allowed=0`** — needs confirming off flash |

## Still blocked on root or BROM

- the values of every `/eink` property (`eink_fb_width`, the timing triplets, `compatible`) — the panel's real geometry
  and controller identity
- which VCOM chip this unit has, and its calibrated value
- `/proc/cmdline` (permission denied to `shell`)
- `dtbo_a`, and therefore the board overlay that overrides the base tree
- the `waveform` partition's size and header
