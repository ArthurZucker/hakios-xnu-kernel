# Bigme HiBreak Pro (b651)

A HakiOS device port for the Bigme HiBreak Pro e-ink phone (model `B651`, MediaTek MT6877 "Dimensity 900",
product string `hibreak_eea`). **Nothing here has booted yet** — read `SCOPE.md` before anything else.

A HakiOS device repository: what is true of this device and of no other. The kernel, the loader, the drivers and the tools are
the common repository's; the mt6877 family's facts are in `soc/`, carried with the device. How the three are put together:
[docs/porting/repositories.md](https://github.com/oosexgang/hakios-xnu-kernel/blob/main/docs/porting/repositories.md).

> **This is not a Qualcomm phone.** Every driver in the common repository under `drivers/` was written against a
> Qualcomm SoC: the SPMI arbiter, RPMh, socinfo's SMEM item 137, the UFS host controller, the ADSP, the MDM/IPA modem.
> A MediaTek SoC shares none of those blocks, so "put a driver back in `configs/b651.config`" is not the usual step
> here — most of these drivers need a MediaTek counterpart written first. `SCOPE.md` lists them.

| File | What |
|---|---|
| `device.conf` | what the device is, and how far each part of it works: its page in the documentation is made from it |
| `configs/b651_defconfig` | the common kernel, the SoC's fragment, this device's: `make b651_defconfig` |
| `configs/b651.config` | what is this device's own: the drivers built in, the arguments it needs |
| `soc/` | its SoC family's layer (mt6877): `soc.conf`, `configs/mt6877.config`, the SoC's drivers; carried here so that this one repository is all the device needs |
| `drivers/` | drivers of its own, if it needs any (`drivers/<subsystem>/conf`, as in the common repository) |

Every phone driver of the common repository is left out to begin with: their addresses and pins are the phone they were written
for. Put one back in `configs/b651.config` when it has been made to work here, and say so in `device.conf`.

Check with `tools/hakios-device.py check b651`.
