# Gate 1: can this phone run an unsigned image?

The cheapest decisive test, in the order that risks nothing. Each step says what it answers and what stops the
project. Nothing here writes to the phone until step 5, and step 5 says so first.

**Needs:** the phone on `adb`, `adb`/`fastboot` (platform-tools), and `mtkclient` for steps 5–7. `mtkclient` is
happiest on Linux — on macOS its BROM handshake works but needs `libusb` and a `sudo`, and it can hang on Apple
Silicon. A Linux VM with the USB device passed through is the calmer option.

## 1. Who is this phone, really

```sh
adb shell getprop | grep -E 'ro\.(product|build|boot|hardware|board|soc)' | sort
```

Answers: the model string (`B651`?), the product (`hibreak_eea`?), `ro.hardware` (`mt6877`?), the build
fingerprint and the Android version. Everything in `devices/b651/` was written from second-hand reports; this is
where it stops being second-hand. **If `ro.hardware` is not `mt6877`, stop and revisit `soc/soc.conf`.**

```sh
adb shell cat /proc/cmdline
```

MediaTek's LK passes a long cmdline: the `bootopt`, the `androidboot` fields, and often the `lk` version. Worth
keeping verbatim — it is the same string the loader will have to build.

## 2. The device tree (the input for everything else)

```sh
adb shell ls -l /sys/firmware/
```

If `fdt` is there and readable, this is the file `tools/new-device.py --fdt` wants:

```sh
adb pull /sys/firmware/fdt /tmp/b651.dtb
```

If it is root-only (Bigme's ROM may make it so), and the phone is already rooted:

```sh
adb shell 'su -c "cat /sys/firmware/fdt > /data/local/tmp/fdt && chmod 644 /data/local/tmp/fdt"'
adb pull /data/local/tmp/fdt /tmp/b651.dtb
adb shell 'su -c "rm -f /data/local/tmp/fdt"'
```

Answers: the real memory map, the UART pins, the panel and touch compatible strings, the PMIC part numbers, the
regulator tree. Every "?" in `SCOPE.md` gets an answer out of this one file.

Then, on the fork's common tree:

```sh
tools/new-device.py b651 --fdt /tmp/b651.dtb --force      # regenerates soc.args + README section
python3 tools/soc-args.py /tmp/b651.dtb                    # see what it extracts
```

Expect `soc-args.py` to find **nothing**: it greps for Qualcomm node names (`qcom,ipa`, `qcom,smem`, `aoss`).
That is the finding, not a bug — it is the SoC layer of `SCOPE.md` restated in one command.

## 3. The partition map

```sh
adb shell 'ls -l /dev/block/by-name/ 2>/dev/null || ls -l /dev/block/platform/*/by-name/'
```

Answers: whether it is A/B (it should be — the HiBreak Pro takes full OTAs), where `boot_a`, `lk_a`, `preloader_a`,
`seccfg`, `nvdata` and `vendor_boot` live, and how large the `super` is. Note the **`seccfg`** partition — step 6
reads it.

## 4. The cheapest possible test: does an unsigned image run at all

Two ways, and the first writes nothing.

**4a. `fastboot boot` — RAM only, no flash.** Build a payload small enough to see something, or take any
`boot`-format image you trust to fail visibly. MediaTek's LK frequently **removes** `fastboot boot` entirely, or
accepts it and then hangs at the preloader handshake. Both outcomes are informative and cost nothing.

```sh
adb reboot bootloader
fastboot oem device-info          # may not exist on MTK LK; try anyway
fastboot getvar all 2>&1 | head -40   # look for 'unlocked'/'unlocked: yes'
fastboot boot <any-boot.img>
```

- **It runs** → gate 1 is open, and HakiOS's normal "writes nothing" path works. Best case.
- **`unknown command`** → LK has no `fastboot boot`. You must flash. Go to step 5.
- **It hangs / reboots** → LK refuses unsigned payloads. Go to step 5 and read `seccfg` first.

**4b. If 4a is unavailable**, `fastboot flash boot` of the *stock* `boot_a` image you are about to back up is a
free rehearsal of the write path before you put anything foreign in it. It proves the flash works and gives you a
restore you have already practised.

## 5. Back up before anything is written ← the first step that touches the phone

Nothing in HakiOS writes to a partition unless asked. **This step does.** Do it with `mtkclient`, with the phone
in BROM mode (power off, then hold both volume keys or the vendor's key combo while plugging in).

```sh
mtk gettarget                                  # confirms MT6877 and the DA handshake
mtk r boot_a   boot_a.img
mtk r boot_b   boot_b.img
mtk r lk_a     lk_a.img
mtk r preloader_a preloader_a.img
mtk r seccfg   seccfg.img
mtk r nvdata   nvdata.img
mtk r nvram    nvram.img
```

Verify every dump is non-empty and the expected size. **`nvdata`/`nvram` are the ones you cannot regenerate** —
lose them and the radio/modem identity is gone. Copy the lot somewhere that is not the phone and not one disk.

Also worth having: Bigme's own full OTA (the stock images to restore from), which
`marek-vybiral/bigme-ota-client` resolves from Bigme's server for exactly this model.

## 6. Read the lock state out of `seccfg` (the definitive answer)

`fastboot oem device-info` lies on MediaTek phones that run a lock-state spoofer — and this one has Magisk in the
wild, so `tricky_store`-style spoofing is plausible. `seccfg` is the partition LK actually reads.

```sh
mtk r seccfg seccfg.img
hexdump -C seccfg.img | head -8
```

An unlocked `seccfg` starts with `SEccfg` magic and carries a `lock_state` field that reads `0` (unlocked) rather
than `1`/`2`. Any tool that prints it is fine; the important thing is that it is read **off the flash**, not asked
of the bootloader.

- **unlocked** → unsigned images are allowed; step 7.
- **locked** → the port stops here until the phone is unlocked through Bigme. Do not start the UEFI work.

## 7. Then, and only then

Only after 1–6 is the ordering of `SCOPE.md` meaningful. Gate 2 (a Mu-Silicium `mt6877Pkg`) is the months-long
part, and it is worth starting **only** once step 6 says unlocked and step 2 has produced a device tree.

## What would make this project not worth starting

Written down so it is checked rather than drifted past:

- `seccfg` locked, and Bigme will not unlock → **stop**. Nothing here can proceed.
- `ro.hardware` is not `mt6877` → the SoC layer is wrong; redo `soc/soc.conf` before anything else.
- No `fastboot` or BROM access at all → **stop**.
- You want Darwin's UI, not a text console → **stop now**, on the Mali-G68 (see `SCOPE.md`, "The Metal question").
