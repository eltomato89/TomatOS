#!/bin/sh
#
# TomatOS - write the bootable disk image to a USB stick
#
#     tools/usb-boot.sh <image> <device>
#     make usb-boot DEV=/dev/sdX
#
# ---------------------------------------------------------------------------
# WHY THIS IS A SCRIPT AND NOT A RECIPE
# ---------------------------------------------------------------------------
# The "usb" target for the ISO does the same job in eight lines inside the
# Makefile, and that was fine while it had two checks. This one has six, and
# every one of them exists because of a specific way a person loses a disk.
# A refusal whose reason is not written down is a refusal someone eventually
# deletes to make a build work, so each one here gets the sentence that says
# what it protects against. That does not fit in a Makefile recipe, where
# every line needs a trailing backslash and every dollar has to be doubled.
#
# ---------------------------------------------------------------------------
# THE ORDER OF THE CHECKS IS THE POINT
# ---------------------------------------------------------------------------
# All six run before the prompt, not after it. The prompt is the LAST line of
# defence and the weakest one: a person who has already decided to type "yes"
# will type it. So the question is never "did they confirm" but "is this
# device one we are willing to write to at all", and that is answered from
# what the kernel says about the hardware, not from what the caller typed.
#
# In particular /sys/block/<name>/removable is checked before anything is
# written and cannot be overridden from the command line. It is the check a
# mistyped letter cannot get past: sdc and sda differ by one keystroke, and on
# this machine one of them is a 3.6 GB stick and the other holds /boot.
# ---------------------------------------------------------------------------

set -eu

IMG=${1:-}
DEV=${2:-}

die() {
	printf '\nERROR: %s\n' "$1"
	shift
	for line in "$@"; do
		printf '       %s\n' "$line"
	done
	printf '\n'
	exit 1
}

# --- the image ------------------------------------------------------------
# Checked first only because it is the cheap one: there is no point examining
# a disk if there is nothing to write to it. It is also the one case here that
# is a build problem rather than a user problem.
[ -n "$IMG" ] || die 'no image given.' \
	'Usage:  tools/usb-boot.sh <image> <device>'
[ -f "$IMG" ] || die "\"$IMG\" does not exist or is not a regular file." \
	'Build it first:  make bootdisk'

# --- refusal 1: no device -------------------------------------------------
# There is deliberately no default. A default device in a script that calls dd
# is a script that destroys whatever happened to be plugged in when somebody
# runs it with no arguments to see what it does.
[ -n "$DEV" ] || die 'no target device given.' \
	'Usage:  make usb-boot DEV=/dev/sdX' \
	'Find the right device with:  lsblk -o NAME,SIZE,MODEL,TRAN,MOUNTPOINTS' \
	'Look for TRAN=usb and a size that matches the stick you plugged in.'

# --- refusal 2: not a block device ----------------------------------------
# Catches the two typos that would otherwise silently succeed: a path that
# does not exist at all (dd would happily CREATE it as a regular file, report
# 32 MB written, and leave the stick untouched), and a path that exists but is
# a file or a directory.
[ -b "$DEV" ] || die "\"$DEV\" is not a block device." \
	'dd would create it as an ordinary file and report success, and the' \
	'stick would still be empty.'

NAME=$(basename "$DEV")

# --- refusal 3: a partition, not a whole disk -----------------------------
# Writing this image to /dev/sdc1 puts the boot sector at the first sector of
# the PARTITION, so the BIOS never sees it -- it reads LBA 0 of the DISK,
# which still holds whatever was there before. The stick then does not boot,
# and nothing anywhere reports an error: the write succeeded, it just went one
# sector-range too far in.
#
# The distinction the kernel itself draws: every whole disk gets a directory
# under /sys/block, while a partition appears only nested inside its parent's
# directory (/sys/block/sdc/sdc1). So the presence of /sys/block/<name> is the
# question "is this a disk", asked of the kernel rather than guessed at by
# stripping trailing digits off the name -- which would be wrong for nvme0n1
# and for mmcblk0 in opposite directions.
[ -d "/sys/block/$NAME" ] || die "\"$DEV\" is a partition, not a whole disk." \
	"There is no /sys/block/$NAME, so the kernel does not consider this a" \
	'disk of its own.' \
	'The image contains its own partition table and boot sector and has to' \
	'go to the start of the DISK, or the BIOS will never find stage 1.' \
	"Try:  ${DEV%%[0-9]*}"

# --- refusal 4: not removable ---------------------------------------------
# THE IMPORTANT ONE. This is what stands between a mistyped letter and a
# destroyed system disk, and it is why the confirmation prompt is not the last
# word: an internal disk is refused here, before the prompt, no matter what
# anyone would have typed.
#
# /sys/block/<name>/removable is the kernel's own answer, taken from the
# device rather than from the name or the size. It reads 1 for a USB stick or
# a card reader and 0 for anything soldered to a SATA or NVMe port.
REMOVABLE=$(cat "/sys/block/$NAME/removable" 2>/dev/null || echo '?')
[ "$REMOVABLE" = "1" ] || die \
	"\"$DEV\" is not a removable device (/sys/block/$NAME/removable = $REMOVABLE)." \
	'This is almost certainly an internal disk. Nothing has been written.' \
	'The device letters differ by one keystroke and this check is the reason' \
	'that is survivable, so it is not overridable -- find the right device:' \
	'    lsblk -o NAME,SIZE,MODEL,TRAN,MOUNTPOINTS'

# --- refusal 5: not on the USB bus ----------------------------------------
# "Removable" alone is not enough. An internal SD card reader, an eSATA bay
# and some hot-plug SATA backplanes also report removable=1, and a disk in a
# hot-swap bay can be the machine's own. TRAN is the transport the kernel
# assigned, so this narrows "removable" down to "actually reached over USB",
# which is what the image is being written for.
TRAN=$(lsblk -dno TRAN "$DEV" 2>/dev/null | tr -d ' ')
[ "$TRAN" = "usb" ] || die \
	"\"$DEV\" is not on the USB bus (lsblk reports TRAN=\"${TRAN:-unknown}\")." \
	'Removable and USB are not the same thing: internal card readers and' \
	'hot-swap SATA bays report removable=1 as well, and one of those can be' \
	"the machine's own disk."

# --- refusal 6: something is mounted --------------------------------------
# Writing under a mounted filesystem corrupts it in both directions at once:
# the page cache still holds the old metadata and will write it back over the
# new image, and anything reading the mount gets a mix of the two. The result
# is a stick that fails in a way that looks like a bad dd or a bad build.
#
# The mountpoint is deliberately NOT unmounted here. Unmounting is a decision
# about someone else's open files -- there may be a copy running, or an editor
# with an unsaved buffer -- and a script that is about to destroy a disk is
# the last place that should be making it quietly. So it prints the command
# and stops. udisksctl is the one that works without root for a user-mounted
# stick, which is how it got mounted in the first place.
MOUNTED=$(lsblk -nro NAME,MOUNTPOINT "$DEV" 2>/dev/null |
          awk 'NF > 1 { printf "/dev/%s on %s\n", $1, $2 }')
if [ -n "$MOUNTED" ]; then
	printf '\nERROR: "%s" has mounted partitions:\n\n' "$DEV"
	printf '%s\n' "$MOUNTED" | sed 's/^/       /'
	printf '\n       Writing under a mounted filesystem corrupts it: the page cache\n'
	printf '       still holds the old metadata and writes it back over the image.\n'
	printf '       Unmount them yourself -- they may be in use by something this\n'
	printf '       script cannot see:\n\n'
	printf '%s\n' "$MOUNTED" | awk '{ printf "           udisksctl unmount -b %s\n", $1 }'
	printf '\n'
	exit 1
fi

# --- the prompt -----------------------------------------------------------
# Everything above has already established that this is a removable USB whole
# disk with nothing mounted on it. What is left for the human is the one
# question no check can answer: whether this particular stick is the one whose
# contents are expendable. So it gets shown, in full, before the question.
printf '\n  About to OVERWRITE this device with %s:\n\n' "$IMG"
lsblk -o NAME,SIZE,TYPE,MODEL,TRAN,MOUNTPOINTS "$DEV" 2>/dev/null |
	sed 's/^/    /' || true
printf '\n    image: %s bytes\n' "$(stat -c%s "$IMG")"
printf '\n  ALL DATA ON %s WILL BE DESTROYED.\n\n' "$DEV"

# "yes" and nothing else -- not "y", not "Y", not the empty line that a stray
# Return produces. The three extra keystrokes are the point.
printf '  Type exactly "yes" to continue: '
read -r ANSWER || ANSWER=''
if [ "$ANSWER" != "yes" ]; then
	printf '\n  Aborted. Nothing was written.\n\n'
	exit 1
fi

# --- the write ------------------------------------------------------------
# oflag=sync makes each block hit the device rather than the page cache, which
# is what makes status=progress tell the truth: without it the counter races
# to the end of a 32 MB image and then the process sits in the final close for
# as long as the stick actually needs. conv=fsync and the sync afterwards
# cover the rest, because a USB stick pulled out while its own internal cache
# is still writing is a stick with a half written boot sector.
printf '\n  Writing...\n'
sudo dd if="$IMG" of="$DEV" bs=4M status=progress oflag=sync conv=fsync
sync

printf '\n  Done. %s now carries the TomatOS boot chain.\n' "$DEV"
printf '  It is safe to remove.\n\n'
