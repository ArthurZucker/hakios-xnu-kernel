<!-- Made by tools/docs-gen.py from devices/*/device.conf and socs/*/soc.conf. Do not edit: change that, then run tools/docs-gen.py. -->


# Devices

Every device with a repository, and how far each part of it works. A device of yours: [Port a device](../porting/index.md).

| Device | SoC | display | touch | buttons | cpu | gpu | storage | audio | battery | haptic | camera | torch | wifi | bluetooth | cellular | network | thermal | usb |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| [Bigme HiBreak Pro](b651.md) | `mt6877` | no | no | no | no | no | no | no | no | no | no | no | no | no | no |  | no | no |
| [QEMU virt (aarch64)](qemu.md) | `none` | yes | yes | yes |  | no | yes | yes |  |  |  |  |  |  |  | yes |  |  |

An empty cell is a part nobody has looked at on that device yet.

## SoC families

| SoC | What | |
|---|---|---|
| `mt6877` | MediaTek Dimensity 900 (MT6877) | carried by b651 (`devices/b651/soc/`) |
