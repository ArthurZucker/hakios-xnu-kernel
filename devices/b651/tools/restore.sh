#!/bin/bash
# restore.sh - put the HiBreak Pro's own boot partition back.
#
# THIS SCRIPT DOES NOT TOUCH A PHONE. Every device command it knows is printed,
# not run. `--run` gates the two commands that only read or write a *file* on
# the host (sha256sum, mtkclient's read to a path); it still never runs a write
# against the phone. Flashing is printed for you to paste, deliberately.
#
# usage:
#   restore.sh backup   --backup DIR            where the stock dumps are (GATE1 step 5)
#   restore.sh verify   --backup DIR            check every dump against its recorded hash
#   restore.sh plan     --backup DIR            print the whole restore, in order
#   restore.sh restore  --backup DIR [--slot a|b]
#
#   --backup DIR   the directory GATE1 step 5 wrote into: boot_a.img, boot_b.img, ...
#   --slot a|b     which boot slot to restore (default: a)
#   --run          actually run host-side commands (sha256sum, mtk read); default dry-run
#   --serial X     pass --serial X to mtkclient (more than one MediaTek device attached)
#
# Exit codes: 0 ok, 1 usage/refused, 2 verification failed.

set -u

BACKUP=''
SLOT=a
RUN=0
SERIAL=''
MODE=''

usage() { sed -n '2,26p' "$0" | sed 's/^# \{0,1\}//'; exit 1; }

while [ $# -gt 0 ]; do
	case "$1" in
		backup|verify|plan|restore) MODE=$1; shift ;;
		--backup)   BACKUP=${2-}; shift 2 ;;
		--slot)     SLOT=${2-}; shift 2 ;;
		--serial)   SERIAL=${2-}; shift 2 ;;
		--run)      RUN=1; shift ;;
		-h|--help)  usage ;;
		*) echo "restore.sh: unknown argument: $1" >&2; usage ;;
	esac
done
[ -n "$MODE" ] || usage
[ -n "$BACKUP" ] || { echo "restore.sh: --backup DIR is required" >&2; exit 1; }
case "$SLOT" in a|b) ;; *) echo "restore.sh: --slot must be a or b" >&2; exit 1 ;; esac
[ -d "$BACKUP" ] || { echo "restore.sh: $BACKUP is not a directory" >&2; exit 1; }

# The dumps GATE1 step 5 asks for. `required` ones are the restore; the rest are
# checked if present because losing them is the expensive part.
NVRAM_PARTS="nvdata nvram proinfo"
FLASH_PARTS="boot_${SLOT} lk_${SLOT} preloader_${SLOT} seccfg ${NVRAM_PARTS}"

say()  { printf '%s\n' "$*"; }
run_it() {                       # run a host-side command, or print it
	if [ "$RUN" = 1 ]; then
		say "\$ $*"
		"$@"
	else
		say "$*"
	fi
}
device_cmd() {                   # never executed, only ever printed
	say "$*"
}

# ---------------------------------------------------------------- backup -----
cmd_backup() {
	local serial=''
	[ -n "$SERIAL" ] && serial=" --serial $SERIAL"
	say "# Reading the stock partitions. The phone must be in BROM mode:"
	say "# power off, then hold BOTH volume keys while plugging the cable in."
	say "# Nothing below is executed, even with --run: these are for you to paste."
	say ''
	say "# 0. which device, and does the DA handshake work at all"
	device_cmd "mtk${serial} gettarget"
	say ''
	say "# 1. the boot partitions. boot_a is the restore; boot_b is the one this"
	say "#    phone is running when ro.boot.slot_suffix is _b (see ../PHONE.md)."
	for p in boot_a boot_b init_boot_a init_boot_b vendor_boot_a vendor_boot_b; do
		device_cmd "mtk${serial} r $p ${BACKUP}/${p}.img"
	done
	say ''
	say "# 2. the bootloader and the lock state"
	for p in lk_a lk_b preloader_a preloader_b seccfg; do
		device_cmd "mtk${serial} r $p ${BACKUP}/${p}.img"
	done
	say ''
	say "# 3. the partitions that cannot be regenerated. Without these the radio's"
	say "#    identity is gone; GATE1 step 5 says the same in the same words."
	for p in $NVRAM_PARTS; do
		device_cmd "mtk${serial} r $p ${BACKUP}/${p}.img"
	done
	say ''
	say "# 4. the e-ink panel's own calibration, if the partitions have those names"
	for p in waveform vcom dtbo_a dtbo_b; do
		device_cmd "mtk${serial} r $p ${BACKUP}/${p}.img"
	done
	say ''
	# 5. record what was read: size and sha256 of every dump, at dump time. The
	#    hash is the only thing that later proves the restore image is unchanged.
	if [ "$RUN" = 1 ]; then
		say "\$ sha256sum $BACKUP/*.img > $BACKUP/SHA256SUMS"
		sha256sum "$BACKUP"/*.img > "$BACKUP/SHA256SUMS"
	else
		say "sha256sum $BACKUP/*.img > $BACKUP/SHA256SUMS"
	fi
	say ''
	say "Copy $BACKUP off this machine and off this disk."
	say "A backup that exists once is not a backup."
}

# ---------------------------------------------------------------- verify -----
# A dump is usable when it is non-empty and its hash matches the record. Without
# a record, a plausible-looking image is not evidence of anything: say so.
#
# Only boot_$SLOT is required. GATE1 step 5 dumps the rest as a precaution, and
# an absent lk_b (never dumped, or not present on a slot-a-only unit) does not
# stop a boot_$SLOT restore. Anything that is present and does not verify is a
# failure: that is the difference between "absent" and "wrong".
REQUIRED_PART="boot_${SLOT}"

verify_one() {
	local p=$1 f="$BACKUP/$1.img" required=$2
	if [ ! -f "$f" ]; then
		printf '%-16s %-12s %-10s %s\n' "$p" 'MISSING' '-' '-'
		[ "$required" = 1 ] && VERIFY_FAILS=$((VERIFY_FAILS + 1))
		return
	fi
	local size hash=''
	size=$(wc -c < "$f" | tr -d ' ')
	# Hash in a way that never mixes the printed command line into the value.
	if [ "$RUN" = 1 ]; then
		hash=$(sha256sum "$f" | cut -d' ' -f1)
	fi
	if [ "$size" -eq 0 ]; then
		printf '%-16s %-12s %-10s %s\n' "$p" 'EMPTY' 0 '-'
		VERIFY_FAILS=$((VERIFY_FAILS + 1))
		return
	fi
	local rec=''
	if [ -f "$BACKUP/SHA256SUMS" ]; then
		rec=$(awk -v n="$f" '$2 == n || $2 == "./"n { print $1 }' "$BACKUP/SHA256SUMS" | head -1)
	fi
	if [ -z "$hash" ]; then
		printf '%-16s %-12s %-10s %s\n' "$p" 'NO --run' "$size" "${rec:-not recorded}"
		VERIFY_FAILS=$((VERIFY_FAILS + 1))
		[ "$required" = 1 ] && [ -z "$rec" ] && VERIFY_FAILS=$((VERIFY_FAILS + 1))
	elif [ -z "$rec" ]; then
		printf '%-16s %-12s %-10s %s\n' "$p" 'UNRECORDED' "$size" "$hash"
		VERIFY_FAILS=$((VERIFY_FAILS + 1))
	elif [ "$rec" = "$hash" ]; then
		printf '%-16s %-12s %-10s %s\n' "$p" 'OK' "$size" "$hash"
		VERIFY_CHECKED=$((VERIFY_CHECKED + 1))
	else
		printf '%-16s %-12s %-10s %s\n' "$p" 'MISMATCH' "$size" "$hash"
		VERIFY_FAILS=$((VERIFY_FAILS + 1))
	fi
}

cmd_verify() {
	VERIFY_FAILS=0
	VERIFY_CHECKED=0
	say "backup: $BACKUP   (slot $SLOT; $REQUIRED_PART is the one that must verify)"
	say ''
	printf '%-16s %-12s %-10s %s\n' partition status bytes sha256
	for p in $FLASH_PARTS; do
		if [ "$p" = "$REQUIRED_PART" ]; then
			verify_one "$p" 1
		else
			verify_one "$p" 0
		fi
	done
	say ''
	if [ "$VERIFY_CHECKED" -eq 0 ]; then
		say "0 verified, $VERIFY_FAILS failed. Nothing was proved; --run is what does the hashing."
		say "Run: restore.sh verify --backup $BACKUP --run"
	else
		say "$VERIFY_CHECKED verified, $VERIFY_FAILS failed"
	fi
	[ "$VERIFY_FAILS" -eq 0 ] && [ "$VERIFY_CHECKED" -gt 0 ] || return 2
	return 0
}

# ------------------------------------------------------------------ plan -----
cmd_plan() {
	say "The restore, in order. Nothing here is executed by this script."
	say ''
	say "  0. the phone is locked (../PHONE.md: ro.boot.flash.locked=1,"
	say "     sys.oem_unlock_allowed=0). Reread seccfg over BROM first; if it"
	say "     still says locked, NOTHING below can run. ../GATE1.md step 6."
	say ''
	say "  1. verify the backup:            restore.sh verify --backup $BACKUP --run"
	say ''
	say "  2. pick the slot you are restoring. The running slot is"
	say "     ro.boot.slot_suffix; flashing the other one changes nothing today."
	say ''
	say "  3. write boot_${SLOT} back. Two ways, in order of preference:"
	say ''
	say "     a. fastboot, if LK's fastboot works and the phone is unlocked:"
	device_cmd "     adb reboot bootloader"
	device_cmd "     fastboot devices                         # is it there"
	device_cmd "     fastboot flash boot_${SLOT} ${BACKUP}/boot_${SLOT}.img"
	device_cmd "     fastboot reboot"
	say ''
	say "     b. BROM + mtkclient, if fastboot does not:"
	device_cmd "     mtk w boot_${SLOT} ${BACKUP}/boot_${SLOT}.img"
	say ''
	say "  4. read it back and compare - a flash you did not verify is a guess:"
	device_cmd "     mtk r boot_${SLOT} /tmp/boot_${SLOT}.readback.img"
	device_cmd "     sha256sum /tmp/boot_${SLOT}.readback.img"
	say "     This must equal the sha256 'restore.sh verify --run' printed for"
	say "     boot_${SLOT}.img. The readback is padded to the partition size, so"
	say "     compare the dump's bytes, not the whole file:"
	say "       cmp -n \$(wc -c < ${BACKUP}/boot_${SLOT}.img) ${BACKUP}/boot_${SLOT}.img /tmp/boot_${SLOT}.readback.img"
	say ''
	say "  5. the other partitions, only if they were touched:"
	for p in lk_${SLOT} preloader_${SLOT} ${NVRAM_PARTS}; do
		device_cmd "     mtk w $p ${BACKUP}/$p.img"
	done
	say ''
	say "  6. if the phone will not start at all, the preloader is the recovery of"
	say "     last resort and it is the one partition where a wrong write is"
	say "     unrecoverable without service hardware. Do not flash preloader unless"
	say "     the phone is already dead."
	say ''
	say "What this cannot tell you: whether the stock image is what the phone"
	say "actually shipped with. That is what 'verify --run' against a hash recorded"
	say "at dump time proves, and nothing else does."
}

# --------------------------------------------------------------- restore -----
# The restore itself. Deliberately: it checks the backup, prints the flash, and
# stops. There is no code path in this script that writes to a device.
cmd_restore() {
	say "# restore.sh restore: this will NOT write to the phone."
	say "# It verifies the backup, then prints the commands for you to run."
	say ''
	cmd_verify || { say ''; say "Refusing to plan a restore from a backup that did not verify."; exit 2; }
	say ''
	cmd_plan
}

case "$MODE" in
	backup)  cmd_backup ;;
	verify)  cmd_verify; exit $? ;;
	plan)    cmd_plan ;;
	restore) cmd_restore ;;
esac
