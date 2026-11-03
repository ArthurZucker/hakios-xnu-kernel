# `b651/tools/` — two tools, and what each one proves

Both run on the host, with no phone attached, and neither writes to a device. A third section covers the e-ink
model's calibration file `qemu/panel.json`: it is not a tool in this directory, but it is the file the loop below
edits, and that loop is the one procedure here that a measurement feeds.

| Tool | What it does | What it proves | One-line check |
|---|---|---|---|
| `mkbootimg.py` | packs a kernel + ramdisk into an Android/MediaTek boot image, and reads one back (`--info`, `--unpack`) | an image in LK's format can be produced **and** parsed here, so the format work is not blocked on the phone | `python3 devices/b651/tools/mkbootimg.py --help` |
| `restore.sh` | prints the backup / verify / restore sequence, and verifies dump hashes | the restore path exists on paper and its backup is checked, without touching the phone | `bash -n devices/b651/tools/restore.sh` |

Why `SHIP.md` matters more than either tool: what is *not* proved is in there.

## `mkbootimg.py`

Standard library only (Python 3.8+). The header field table, the two meanings of
offset 40, and every default are in its module docstring; `--help` prints the same.

```sh
tools/mkbootimg.py --kernel Image --ramdisk r.cpio.gz -o boot.img        # header v0
tools/mkbootimg.py --info boot.img                                       # parse it back
tools/mkbootimg.py --unpack boot.img -o outdir                           # sections + sha256s
tools/mkbootimg.py --kernel Image --ramdisk r.cpio.gz --devicetree b651.dtb \
    --header-version 2 -o boot.img                                       # dtb as its own section
```

Round-trip check (three header versions and the legacy MediaTek tag; the exact
command and output are in `SHIP.md` §6):

```sh
python3 - <<'PY'
import subprocess, tempfile, os, struct, hashlib
d = tempfile.mkdtemp()
k, r = os.urandom(123457), os.urandom(20011)
open(f'{d}/k','wb').write(k); open(f'{d}/r','wb').write(r)
subprocess.run(['python3','devices/b651/tools/mkbootimg.py','--kernel',f'{d}/k',
                '--ramdisk',f'{d}/r','-o',f'{d}/b.img'], check=True, stdout=subprocess.DEVNULL)
img = open(f'{d}/b.img','rb').read()
ks, = struct.unpack_from('<I', img, 8); rs, = struct.unpack_from('<I', img, 16)
print('kernel ok:', img[2048:2048+ks] == k, 'ramdisk ok:', img[2048+((ks+2047)//2048)*2048:][:rs] == r)
PY
```

**What it does not do**: it does not know this phone's real load addresses,
page size, or header version. Those are in the phone's own `boot_a`, which has
not been read. Defaults are the usual MediaTek arm64 ones and are labelled as
defaults.

## `restore.sh`

Never executes a device command. `mtk`, `fastboot` and `adb` lines are printed
for you to paste. `--run` enables only host-side work (`sha256sum`, and reading
`SHA256SUMS`); there is no code path that writes to a device, with or without it.

```sh
tools/restore.sh backup  --backup ~/b651-stock            # prints the mtk read commands
tools/restore.sh verify  --backup ~/b651-stock --run      # checks dumps against SHA256SUMS
tools/restore.sh plan    --backup ~/b651-stock            # the whole restore, in order
tools/restore.sh restore --backup ~/b651-stock --slot a   # verify, then print the flash
```

Exit codes: `0` ok, `1` usage or refused, `2` verification failed. `restore`
refuses to print a plan when `boot_<slot>` did not verify.

`verify` hashes with `sha256sum`, which is coreutils — on macOS it is `shasum -a 256`
unless coreutils is installed. The script does not detect this; it will report
`NO --run`-style errors if `sha256sum` is absent. Check with `command -v sha256sum`
before trusting a `verify --run` result.

## `qemu/panel.json` — calibrating the e-ink model

The e-ink model's numbers are not in `qemu/epd.py` once anyone measures the panel: they are in
`qemu/panel.json`, the path `epd.py`'s `CALIBRATION` constant points at. `epd.py` loads that file at import, so
every knob in it overrides the literals for `epd.py`, `epd_live.py` and `epd_view.py` alike. That file is not a
tool and does not live in this directory, but the loop below is the only way those numbers change, so it is
documented here with the tools.

Every knob is **ASSUMED** today — 16 greys, ink `0x22`, paper `0xDD`, and the per-waveform
frame/flash/residue triplets — which is the whole reason the numbers belong in a data file: a measurement goes in
as a diff on `panel.json` and is reviewable on its own, instead of a Python edit whose history is mixed with the
model. `epd.py`'s own docstring says the same and lists each knob's type and admissible range.

| knob | value | what it is | visible on the test page? |
|---|---|---|---|
| `grey.levels` | 16 | greys `gc16`/`gl16` can address | yes — the ramp breaks into fewer bands |
| `grey.black` | 34 (`0x22`) | darkest the ink gets, of 255 | yes — the whole page's dark end moves |
| `grey.white` | 221 (`0xDD`) | lightest the paper gets, of 255 | yes — the whole page's light end moves |
| `gc16.frames` | 24 | frames to settle | **no** — `settle()` unpacks `frames` but never reads it |
| `gc16.flash` | `true` | blanks the panel first | no while `gc16.residue` is 0.0 — both branches render `new`; yes at residue 0.5 |
| `gc16.residue` | 0.0 | old image surviving one update | no while `gc16.flash` is `true` — a flashing mode discards residue by construction; yes with `flash=false` |
| `gl16.frames` | 16 | frames to settle | **no** — same as `gc16.frames` |
| `gl16.flash` | `false` | blanks the panel first | yes — flipping it to `true` changes the `gl16` patch |
| `gl16.residue` | 0.06 | old image surviving one update | no at small moves (0.06→0.2 is identical); yes at 0.06→0.3 |
| `du.frames` | 6 | frames to settle | **no** — same as `gc16.frames` |
| `du.flash` | `false` | blanks the panel first | yes — the `du` patch changes |
| `du.residue` | 0.22 | old image surviving one update | **no** — see "what the page cannot show" |
| `du.levels` | 2 | per-pixel states for `du` | yes — a grey patch instead of two states |
| `a2.frames` | 2 | frames to settle | **no** — same as `gc16.frames` |
| `a2.flash` | `false` | blanks the panel first | yes — the `a2` patch changes |
| `a2.residue` | 0.45 | old image surviving one update | **no** — same reason as `du.residue` |
| `a2.levels` | 2 | per-pixel states for `a2` | yes — a grey patch instead of two states |
| `panel.fps` | 2.0 | how often the model repaints | **no** — a still PNG has no repaint rate |

`panel.fps` is the one knob whose consumer is not a table: it times `--burst`, so a change to it is visible in
how many `fNN.png` a boot produces, not in any single frame.

### The calibration loop

The reference frame is `epd.py --calibrate`'s own test page — a 480x360 ramp, text-sized bars, and one settled
patch per waveform. It is deterministic: the same knobs produce the same PNG bytes, so a before/after diff is
attributable to the knob and nothing else. (Verified: two consecutive `--calibrate ref.png` runs at the defaults
hash identically, and `--calibrate` writes the same file twice.) That determinism is what makes it usable as a
reference: a noisy frame would need a threshold, this one needs none.

1. **Print the table, and where it came from.** `python3 qemu/epd.py --calibrate` prints all 18 knobs and names
   the file its numbers came from, or says "the built-in defaults". Do this first. Two caveats, both true of
   `epd.py` as it stands: it prints the *stderr* line `... does not fit the model (...); using the built-in
   numbers` when `load()` rejects the file, and its "numbers from" header tests `os.path.exists` only — so a file
   that exists but failed to load is still named in the header while the values above are the literals. Read
   stderr, not just the header. If you cannot tell, `--demo` check 5 is the wrong instrument (see below); read
   the file and the literals side by side instead.
2. **Render the reference at the current numbers.** `python3 qemu/epd.py --calibrate /tmp/before.png`.
3. **Change one knob and re-render.** `python3 qemu/epd.py --set du.levels=4 --calibrate /tmp/after.png`. One
   knob at a time: the test page cannot show most knobs at all (table above), so a multi-knob diff tells you
   something moved without telling you which. `--set` accepts hex (`grey.black=0x60`) for the integer knobs,
   which is the base the panel's own numbers are written in.
4. **Compare against the panel.** The intended reference is a photograph of the real B651 under the same
   content. **That photograph does not exist and cannot be taken today**: the phone is read-only by the rules of
   engagement (`../SIM.md`, "Rules of engagement") and gate 1 is open (`../SCOPE.md`), so there is no source to
   diff a frame against. Until there is, step 4 is replaced by the fallbacks that need no phone — the waveform
   LUTs and `/eink` properties named in `../WAVEFORMS.md` §5 — and every number stays ASSUMED.
5. **Write the measurement back with the model's own writer.** `python3 qemu/epd.py --set ... --save` writes
   `panel.json`; do not hand-edit it. The writer keeps the key set and the value types honest, and hand-editing
   is how a boolean becomes the string `"true"` or an integer becomes a float. `--save` is idempotent: run
   against the file as it stands it produces a byte-identical one.
6. **Re-run the self-check.** `python3 qemu/epd.py --demo` needs no QEMU and writes nothing.

### What the test page cannot show, and why it matters

`--calibrate` is a picture of the *quantised panel*, not of the model's internals, and two families of knobs
fall through it:

- **`*.frames` never reaches a render.** `settle()` unpacks the triple as `frames, flashes, ghost` and then
  never reads `frames`; it renders the settled state only. So `--set a2.frames=24` is accepted, printed by
  `--calibrate`, and produces a byte-identical picture at the defaults. A frame count is only observable through
  `--burst` timing against a real controller, or in `epd_view.py`'s labels. Do not calibrate a frame count by
  looking at the reference frame — you will "confirm" a change that did nothing.
- **`du.residue` and `a2.residue` saturate while `du.levels`/`a2.levels` are 2.** With two per-pixel states there
  is no grey to put residue in: at `du.residue` 0.001 the patch is paper white, and from 0.01 upward it is ink
  black — `0.01` through `0.9` render identically. Measured, not reasoned: the settled patch goes 0.22→34, 0.45
  →34, 0.9→34. `gl16.residue` *is* observable because `gl16` addresses the full 16-level ramp, which is why the
  table above shows it and not the other two. Raise `du.levels` to 16 in the same `--set` and `du.residue`
  0.22→0.9 does change the page; to measure it honestly, use `--burst` on a real boot.

A corollary worth stating plainly: **`--demo`'s "panel.json matches KNOBS" check cannot catch a stale file.**
`load()` runs at import, so `panel.json` has already overwritten `KNOBS` by the time the check compares the
file against `KNOBS` — the file is compared against itself. Demonstrated by putting `gc16.frames: 99` in
`panel.json`: the check reports `18 knobs agree with KNOBS` while the literals still say 24. The check is worth
having for a wrong *type* or a missing key (both of which it does catch, because they fail `apply()` and load
falls back), but it does not prove the file is what the literals say. When the file and the literals are
expected to agree, compare them by reading, not by trusting that line. This is a defect in `epd.py`, which is
owned elsewhere; the workaround here is step 1's "where it came from" line.

## What is unverified

- No image made by `mkbootimg.py` has been booted, or even fed to an LK.
- The legacy MediaTek 512-byte tag (`--mtk-header`) is reproduced from a
  community tool, not from a MediaTek document or a real image off this phone.
- `restore.sh` has been exercised only against a synthetic backup directory.
- `boot_a` itself has not been read, so the defaults may be entirely wrong for
  this device. `SHIP.md` says which fields have to come from it.
