# `b651/tools/` — two tools, and what each one proves

Both run on the host, with no phone attached, and neither writes to a device.

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

## What is unverified

- No image made by `mkbootimg.py` has been booted, or even fed to an LK.
- The legacy MediaTek 512-byte tag (`--mtk-header`) is reproduced from a
  community tool, not from a MediaTek document or a real image off this phone.
- `restore.sh` has been exercised only against a synthetic backup directory.
- `boot_a` itself has not been read, so the defaults may be entirely wrong for
  this device. `SHIP.md` says which fields have to come from it.
