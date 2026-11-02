# MT6877 hardware facts — Bigme HiBreak Pro (b651)

Source: `github.com/Winkmoon/android_kernel_xinran_oppo_mt6877` @ `--depth 1` (OPPO Reno 7 SE, same SoC).
All line numbers are in that clone. `mt6877.dts` = `arch/arm64/boot/dts/mediatek/mt6877.dts`.

**Class:** `SoC` = true of MT6877 anywhere (this is the HiBreak Pro's silicon). `board` = this OPPO board only — a clue, not a spec.

## Facts

| fact | value | class | source file:line | confidence |
|---|---|---|---|---|
| SoC compatible | `mediatek,MT6877` | SoC | mt6877.dts:23 | verified in source |
| **UART** early-console port | UART0 `serial@11002000`, `reg = <0 0x11002000 0 0x1000>` | SoC | mt6877.dts:1844-1846 | verified in source |
| UART0 IRQ | `GIC_SPI 141` level-high | SoC | mt6877.dts:1847 | verified in source |
| UART compatible | `mediatek,mt6577-uart` (8250_mtk) | SoC | mt6877.dts:1845; drivers/tty/serial/8250/8250_mtk.c:761 | verified in source |
| UART console baud | `921600n1`, from `console=ttyS0,921600n1` | board (cmdline) | mt6877.dts:32 | verified in source |
| UART console device name | `ttyS0` → apuart0 **assumed**: DT has no `aliases { serialN }` node; numbering comes from probe order | SoC | mt6877.dts:690-698 (aliases, display only) | assumed |
| UART earlycon | `OF_EARLYCON_DECLARE(mtk8250, "mediatek,mt6577-uart", …)`, `UPIO_MEM32`; bootargs contain no `earlycon` token, so earlycon is opt-in | SoC | 8250_mtk.c:778-789 | verified in source |
| UART high-baud path | `baud >= 115200` → `MTK_UART_HIGHS=0x3`, divisor `/256`, writes sample-count/sample-point/fraction regs | SoC | 8250_mtk.c:431, 450-460 | verified in source |
| UART DMA | `apdma @10220e00`, `mediatek,mt6577-uart-dma`, 4 channels, `dma-bits = <34>` | SoC | mt6877.dts:1827-1841 | verified in source |
| UART `stdout-path` | **not found** — no `stdout-path` / `chosen` serial alias in mt6877.dts; only EVB dts files (mt6755/mt6779/mt6795-evb) set one | SoC | grep over dts/*.dts* | not found |
| UART1 | `serial@11003000`, GIC_SPI 142; OPPO board sets it `disabled` | SoC (addr) / board (status) | mt6877.dts:1855-1858; oplus6877_2169E.dts:1106-1108 | verified in source |
| **GIC** | `arm,gic-v3`; dist `0x0c000000` +0x40000, redist `0x0c040000` +0x200000, 1 redist region | SoC | mt6877.dts:700-711 | verified in source |
| **Generic timer** | `arm,armv8-timer`, GIC_PPI 13/14/11/10, `clock-frequency = <13000000>` | SoC | mt6877.dts:1812-1820 | verified in source |
| PSCI | `arm,psci-1.0`, `method = "smc"`; all CPUs `enable-method = "psci"` | SoC | mt6877.dts:590-593 | verified in source |
| **PMIC — main** | MT6359P, `mediatek,mt6359-pmic`, child of pwrap; `CONFIG_MTK_PMIC_CHIP_MT6359P=y` | SoC | mt6877.dts:1604-1605; defconfig:105 | verified in source |
| PMIC control path (MT6359) | **pwrap** (SPI-style wrapper) `@10026000` +0x1000, `mediatek,mt6877-pwrap`, GIC_SPI 264 | SoC | mt6877.dts:1588-1593 | verified in source |
| PMIF / SPMI controller | `spmi@10027000`, `mediatek,mt6877-pmif-m`; `pmif` 0x10027000+0x8ff, `pmifmpu` 0x10027900+0x500, `spmimst` 0x10029000+0x100, GIC_SPI 265 | SoC | mt6877.dts:2750-2757 | verified in source |
| PMIC on SPMI | MT6315 ×2 — `mt6315@6` (USID 6) and `mt6315@3` (USID 3), `mediatek,mt6315`+`mtk,spmi-pmic`; supply VPROC1/VPROC2 | SoC | mt6877.dts:6657-6681 | verified in source |
| PMIC — charger/flash | MT6360, `mediatek,mt6360_pmu`, on **i2c5** (`i2c5@11d00000`), *not* SPMI | SoC (device+i2c) / board (irq gpio 4) | mediatek/v1/mt6360.dtsi:6-11; mt6877.dts:5962 | verified in source |
| PMIF control base for buttons | **not found** — no PMIF key/`pwrkey` node; MT6359 keys go through pwrap, MT6360 on i2c | SoC | grep `pwrkey`, `keys` in mt6877.dts | not found |
| **UFS host** | `ufshci@11270000`, `reg = <0 0x11270000 0 0x2300>`, GIC_SPI 137 | SoC | mt6877.dts:2543-2546 | verified in source |
| UFS compatible / variant | `mediatek,mt8183-ufshci` → `ufs_mtk_mt8183_cfg`, driver `drivers/scsi/ufs/ufs-mediatek.c` | SoC | mt6877.dts:2544; ufs-mediatek.c:88-94 | verified in source |
| UFS PHY | `ufs_mphy@11fa0000`, `mediatek,ufs_mphy`, +0xc000 | SoC | mt6877.dts:2583-2586 | verified in source |
| UFS refclk mode | `mediatek,refclk_ctrl = <2>` (HW mode), `mediatek,ufs-qos` present | SoC | mt6877.dts:2570-2573 | verified in source |
| **Thermal** | node is **`therm_ctrl@1100b000`**, `mediatek,therm_ctrl`, `reg = <0 0x1100b000 0 0x26e200>`, GIC_SPI 213/214, clk `CLK_IFRAO_THERM` | SoC | mt6877.dts:4964-4971 | verified in source |
| Thermal node named "lvts" | **not found** — no `lvts` string in any dts; the LVTS driver binds by `mediatek,therm_ctrl` | SoC | mtk_lvts_tc.c:80; mtk_tc_wrapper.c:883 | verified in source |
| Thermal driver | `drivers/misc/mediatek/thermal/mt6877/` (`mtk_lvts_tc.c`, `mtk_tc_wrapper.c`, `mtk_ts_6359*.c`) | SoC | dir listing | verified in source |
| **cpufreq mechanism** | vendor node `mt_cpufreq`, `mediatek,mt-cpufreq` — **not** `cpufreq-dt`, **not** generic `mediatek-cpufreq` | SoC | mt6877.dts:2325-2326; cpufreq_v1/src/mtk_cpufreq_main.c:1709 | verified in source |
| cpufreq backing blocks | MCUPM `@0c540000` (`mediatek,mcupm`, 8 mboxes), DVFSP `mt6877-dvfsp` (L-/B-table in DT), EEM FSM `@11278000`, `mcupm_share` reserved 6MB | SoC | mt6877.dts:803, 2261-2280, 2329, 608-619 | verified in source |
| Generic `mediatek-cpufreq` support | **not MT6877** — of_match lists only mt2701/mt2712/mt7622/mt7623/mt817x/mt8173/mt8176 | SoC | drivers/cpufreq/mediatek-cpufreq.c:547-553 | verified in source |
| OPP table shape | two `operating-points-v2` tables, `opp-shared`, `opp-hz` 64-bit + `opp-microvolt` only (no `required-opps`, no `opp-supported-hw`) | SoC | mt6877.dts:40-107, 109-176 | verified in source |
| OPP ranges | cluster0: 16 OPPs 500 MHz–2.0 GHz @650–1050 mV; cluster1: 16 OPPs 650 MHz–**2.4 GHz** | SoC | mt6877.dts:44, 104, 113, 173 | verified in source |
| **CPU topology** | 6× `arm,cortex-a55` (cpu0-5, reg 0x000–0x500, capacity-dmips 273) + 2× `arm,cortex-a76` (cpu6/7, reg **0x600/0x700**, capacity-dmips **1024**); cpu-map cluster0=6, cluster1=2 | SoC | mt6877.dts:182-284, 286-320 | verified in source |
| "A78" confirmation | **not found / contradicted** — DT says `cortex-a76` for the big cluster; no `cortex-a78` anywhere in the dts dir except mt6893. Vendor DT commonly keeps a76 compatible strings | SoC | grep `cortex-a78` over dts/ | not found |
| Which cores the kernel treats as big | cpu6/cpu7 only (higher `capacity-dmips-mhz`, own `cluster1_opp`, `cpuoff_b`/`clusteroff_b` idle states) | SoC | mt6877.dts:260-284 | verified in source |
| **GPU node** | `mali@13000000`, `reg = <0 0x13000000 0 0x4000>`, compatible `mediatek,mali` + `arm,mali-valhall` | SoC | mt6877.dts:3315-3317 | verified in source |
| GPU interrupts | GIC_SPI 453/454/455/456/457 = GPU, MMU, JOB, EVENT, PWR | SoC | mt6877.dts:3318-3329 | verified in source |
| GPU DVFS node shape | separate `gpufreq` node, `mediatek,gpufreq`: clocks (MFG mux/MFGPLL1/internal1/MFGPLL4/mfgcfg BG3D/6× MTCMOS), `_vgpu-supply = vproc2_buck`, `_vsram_gpu-supply = vsram_md_ldo`; `ged` consumes it | SoC | mt6877.dts:6602-6631 | verified in source |
| GPU DVFS tables | C-file arrays `g_opp_table_segment_1/2/3` (not DT); segment_1 0.95→0.67 GHz; MFG regs remapped from `mediatek,mfgcfg` +0x13fbf000 | SoC | gpufreq/v1/include/mt6877/mtk_gpufreq_internal_plat.h:378-400; gpufreq/v1/mt6877/mtk_gpufreq_plat.c:3661 | verified in source |
| GPU model string | driver product table names `Mali-G68` (product LBEX); matching it to this die is an inference | SoC | mali_valhall/mali-r32p1/…/mali_kbase_core_linux.c:3151 | assumed |
| **DRAM base/size in DT** | `memory { reg = <0 0x40000000 0 0x3e605000> }` = 998 MiB, top `0x7e605000` | board (placeholder map) | mt6877.dts:585-588 | verified in source |
| DRAM size actually fitted | **not found** — no size node; LK/bootloader passes it. Vendor DT map is a fixed 998 MiB stub | board | grep `dram_size`, `emi` in DT | not found |
| Reserved memory: SSMR CMA | 256 MiB, `alloc-range = <0 0xc0000000 0 0x10000000>`, reusable | SoC | mt6877.dts:600-606 | verified in source |
| Reserved: mcupm_share | 6 MiB + 64 KiB (`0x610000`), no-map, alloc-range 0x40000000+0x60000000 | SoC | mt6877.dts:608-619 | verified in source |
| Reserved: others | consys 0x780000; scp_share 0x1A0000; sspm_share 0x510000; adsp_share 0x1000000; wifi 0xF20000; gps 0x100000; ion carveout 0xc000 | SoC | mt6877.dts:632-686 | verified in source |
| **EPDC / e-ink** | **not found** — zero `epdc`/`eink`/`e-ink` hits in mt6877.dts, in any MT6877 dtsi, or in `drivers/misc/mediatek/video/mt6877` (dir absent) | SoC | grep over dts/ + drivers/ | not found |
| EPD elsewhere in tree | `ext_disp/*/extd_epd.c` exists only for mt6765/6768/6779/6785/6833/6885 — **no mt6877 variant** | n/a | drivers/misc/mediatek/ext_disp/ | verified in source |
| Display controller | `dispsys_config@14000000`, `mediatek,mt6877-mmsys` + `mediatek,dispsys_config` | SoC | mt6877.dts:3381-3385 | verified in source |
| DSI | `dsi0: dsi@14013000`, `mediatek,mt6877-dsi`, GIC_SPI 313, dphy `mipi_tx_config0` | SoC | mt6877.dts:4086-4096 | verified in source |
| OVLS | ovl0 @14005000, ovl0_2l @14006000 (SPI 299/300) | SoC | mt6877.dts:3952, 3964 | verified in source |
| Watchdog | `toprgu@10007000`, `mediatek,mt6877-wdt`, GIC_SPI 109 | SoC | mt6877.dts:1459-1464 | verified in source |
| Board panel | OPPO panels only (`oplus2169E_samsung_ams643ag01_1080p_dsi_cmd,lcm` etc.) — irrelevant to b651 | board | oplus6877_2169E.dts:291-331 | verified in source |
| Board framebuffer hint | `atag,videolfb-fb_base_l = <0x7e605000>` (= top of the 998 MiB DT map) | board | oplus6877_2169E.dts:23 | verified in source |
| efuse/devinfo | `efuse@11c10000` but `reg = <0 0x11f10000 0 0x10000>` — node name and reg disagree (vendor bug, trust `reg`) | SoC | mt6877.dts:3290-3292 | verified in source |
| DVFSP reg | `dvfsp@0011bc00`, `reg = <0 0x0011bc00 …>` — 1.1 MB address is implausible as a physical base; likely an apmixedsys offset (apmixedsys is @0x1000C000) | SoC | mt6877.dts:2261-2264 | assumed |
| SPMI/interconnect straps | `pmif` `swinf_ch_start = <3>`, `ap_swinf_no = <2>` | SoC | mt6877.dts:2788-2789 | verified in source |

Count: **48 verified in source, 3 assumed, 6 rows carrying a "not found"** (56 rows).

## What this means for the port

```text
HakiOS driver (Qualcomm)          MT6877 equivalent here            carries over?
------------------------------    ------------------------------    -------------
core ARM64 (GIC/timer/PSCI)       arm,gic-v3 / armv8-timer / PSCI   YES  -- generic XNU code
hakios.ufshc.c (ufs-qcom)         ufs-mediatek @11270000            PARTLY -- same UFSHCI regs
hakios.adreno.c                   arm,mali-valhall (kbase)          NO
hakios.cpufreq.c                  mt_cpufreq + MCUPM + DVFSP        NO  -- mailbox-bound
hakios.thermal.c                  therm_ctrl / LVTS @1100b000       SHAPE only (compat string)
hakios.pmic.c (SPMI arbiter)      pwrap (MT6359) + PMIF (MT6315)    NO  -- two buses, neither SPMI-arbiter
hakios.buttons.c                  keys via pwrap (MT6359)           NO  -- no PMIF key node at all
hakios.battery.c                  MT6359P gauge (pwrap)             NO
hakios.wifi/bt.c                  MT6877 consys                     NO
hakios.adsp.c                     MTK ADSP/SCP                      NO
hakios.socinfo.c (SMEM 137)       no SMEM; efuse/devinfo @11f10000  NO
```

Direct-equivalent set is exactly two things: the **generic ARM64 core** (GICv3 + arch timer + PSCI, all standard — the XNU ARM64 platform code should carry over unmodified) and the **UFSHCI register block** at `0x11270000` (same DWC UFSHCI 3.x model as `ufs-qcom`; the delta is clocks, the MPHY at `0x11fa0000`, and `refclk_ctrl = 2`).

Two traps the table exposes:

```text
1. PMIC is TWO buses, not one
   MT6359P  --pwrap  @0x10026000  (SPI-like, regulators/gauge/RTC/audio)
   MT6315x2 --PMIF   @0x10027000  (SPMI master; only VPROC1/VPROC2 bucks)
   MT6360   --i2c5   @0x11d00000  (charger, flash)
   -> hakios.pmic.c models one SPMI arbiter. Nothing here matches that shape.

2. No e-ink anywhere in MT6877 vendor trees
   0 hits for epdc/eink in mt6877.dts, in any dtsi, and no video/mt6877
   or ext_disp/mt6877 directory. Bigme's EPD controller + waveforms are
   out-of-tree and are the single largest unknown.
```

**Single most useful next fact:** the HiBreak Pro's *own* device tree — pull `dtb`/`dtbo` (or `/proc/device-tree`) from its boot image. It answers the e-ink controller and its base, the real DRAM size, the UART pinmux, the panel, and the board's PMIC rails in one file, and nothing in this OPPO tree can substitute for any of them.
