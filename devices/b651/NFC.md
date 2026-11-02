# NFC and the secure element: what is known, and what the port would write

The HiBreak Pro has an NFC controller with an embedded secure element. This file is what was established about it
from the shipped configuration, the Android source tree on disk (`/private/tmp/mt6877/src`), and the phone's dumps —
and it corrects one thing an earlier audit of this repository concluded too quickly.

**The short version: the driver does not exist in this kernel, but every input needed to write it does.**

## What is on the phone

From `dumps/kernel.config.txt`, which is the shipped Android kernel's configuration:

```text
:1838  CONFIG_NFC_CHIP_SUPPORT=y
:1839  CONFIG_NFC_ST54_SPI=y          the ST54 series (NFC controller + eSE), on SPI
:1840  CONFIG_MTK_NFC_CLKBUF_ENABLE=y
:1302  # CONFIG_NFC is not set        Linux's generic net/nfc stack is off
:2707  CONFIG_SPI=y
:2722  CONFIG_SPI_MT65XX=y            the host controller is MediaTek's, not a GENI/QUP
```

From `dumps/devicetree-nodes.txt` (node **names only** — the dump has no property values):

```text
spi0@1100a000 spi1@11010000 spi2@11012000 spi3@11013000
spi4@11018000 spi5@11019000 spi6@1101d000 spi7@1101e000
nfc            irq_nfc
```

So: an ST54 on one of eight `spi-mt65xx` masters. **Which master, which chip select, which IRQ and which reset GPIO
are not in the dumps.** That is a `dtbo` read (root) or a logic analyser, not a code question.

## What is on disk, and why it changes the plan

An earlier audit looked for an NFC driver **in this repository** and found none — which is true — and then appeared to
conclude the protocol was equally unknown. It is not. The vendor tree this port already keeps for other facts has
the whole stack:

| file | lines | what it is |
|---|---|---|
| `drivers/nfc/st54-spi/st54spi.c` | 1292 | the ST54 **SPI transport**, as a `spidev`-shaped character device |
| `drivers/nfc/st21nfc-i2c/st21nfc.c` | 44.8K | the ST21 NFC core: power, reset, wakeup polarity, eSE selection |
| `drivers/nfc/st21nfc-i2c/st21nfc.h` | 2.2K | the ioctl ABI (`ST21NFC_MAGIC 0xEA`, `ST21NFC_PULSE_RESET`, `USE_ESE`, ...) |
| `drivers/nfc/oplus_nfc/` | 5.1K | the OEM glue |
| `drivers/spi/spi-mt65xx.c` | 1151 | the **host controller**, with every register named (`SPI_CFG0_REG 0x0000` ... `SPI_CFG2_REG 0x0028`) |

That is a port with a reference implementation, not a research project. The register layout the earlier audit called
"unknown / weeks" is 40 `#define`s at the top of `spi-mt65xx.c`.

## What the port would actually be

```text
       userspace (Android's nfc stack)          on Darwin: nothing yet
                    |  ioctl(ST21NFC_*), read(), write()
   drivers/nfc/hakios.st21nfc.c                <- to write: the ioctl ABI + power/reset state machine
                    |  spi_sync() equivalent
   drivers/spi/hakios.spi_mt65xx.c             <- to write: FIFO mode only, no DMA, no pinctrl
                    |  the eight 0x1101xxxx blocks
   the ST54                                    <- a QEMU model would stand in here, APDU-level
```

Four pieces, in dependency order:

1. **`/dev/hakios-spi` (or a direct call path).** MT65xx SPI in FIFO mode. The parts of `spi-mt65xx.c` that matter
   are `spi_hw_init` (CFG0/CFG1 packet timing) and the transfer path; DMA (`SPI_CMD_TX_DMA`) can be skipped entirely
   — APDUs are small.
2. **The ST54 transport.** A character device with the `ST21NFC_*` ioctls: reset pulses, wakeup polarity, eSE
   in/out. This is `st54spi.c` minus Linux's `spi_master` plumbing.
3. **The QEMU stand-in.** There is no NFC device in QEMU and no MTK SPI master either. A model at **APDU level**
   (`0x00 0xA4 0x04 0x00 ...` SELECT, `0x80 0xCA ...` GET DATA) is far cheaper than modelling SPI electrically, and
   it is enough to develop the driver's state machine — the same lesson the e-ink panel taught.
4. **What is still missing, and cannot be faked:** the ST54's exact line protocol above SPI (the `st54spi.c` in
   here is a `spidev` passthrough — the framing lives inside the vendor's firmware, not in this source), the chip
   select, and the IRQ.

## The honest verdict

**Portable now, without the phone:** the ioctl ABI, the power/reset state machine, the eSE select, and a QEMU APDU
model. All of that can be written and tested in this simulator exactly as `hakios.epd.c` was.

**Needs the phone:** which SPI master, the CS, the IRQ, the reset GPIO, and the real line protocol. Registers, not
logic — the same split `SIM.md` draws for everything else.

**Not on the table:** Google Wallet. It needs Play Services and Play Integrity attestation, which needs a hardware
key this kernel does not have (see `MiniAppleKeyStore.cpp`: its "wrapping" is reversible and protects nothing). No
NFC driver changes that.

## Rules

The phone's bootloader is locked (`sys.oem_unlock_allowed=0`) and nothing in this directory writes to it. Nothing
here needs the phone until the last three rows above, and that is after gate 1.
