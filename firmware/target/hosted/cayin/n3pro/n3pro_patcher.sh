#!/bin/bash
#
# n3pro_patcher.sh - create a Rockbox installation image for the Cayin N3Pro.
#
# The N3Pro runs HiByOS (Ingenic X1000, Linux 3.10).  Its .upt update file is an
# ISO9660 image holding the boot loader, kernel and a UBIFS root filesystem.
# Rockbox is installed "hosted": the stock player binary is left untouched and
# /usr/bin/hiby_player.sh is replaced with a small launcher that runs the
# Rockbox bootloader (which in turn offers a menu to run Rockbox or the stock
# player).
#
# Prerequisites (Debian/Ubuntu):
#   sudo apt install -y p7zip-full genisoimage mtd-utils
#   pip install ubireader            # provides ubireader_extract_files
#
# Usage:
#   ./n3pro_patcher.sh n3pro.upt bootloader.n3pro
#
# Advanced usage:
#   ./n3pro_patcher.sh --unpack     n3pro.upt   [working_dir]
#   ./n3pro_patcher.sh --inject-app bootloader.n3pro [working_dir]
#   ./n3pro_patcher.sh --pack       [working_dir] [output.upt]
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# as published by the Free Software Foundation; either version 2
# of the License, or (at your option) any later version.

set -euo pipefail
trap 'echo "ERROR at line ${LINENO}: ${BASH_COMMAND}" >&2' ERR

# UBIFS geometry of the N3Pro rootfs partition (must match the stock image).
UBIFS_MIN_IO=2048
UBIFS_LEB_SIZE=126976
UBIFS_MAX_LEBS=480
LEB_CNT=480

# All stock files are owned by uid/gid 1001; rcS runs as root so this only has
# to stay consistent with what the updater expects.
FS_UID=1001
FS_GID=1001

BOOTLOADER_BASENAME="bootloader.n3pro"

usage() {
    echo 'Usage:' >&2
    echo '  ./n3pro_patcher.sh n3pro.upt bootloader.n3pro' >&2
    echo '' >&2
    echo 'Advanced usage:' >&2
    echo '  ./n3pro_patcher.sh --unpack     n3pro.upt      [working_dir]' >&2
    echo '  ./n3pro_patcher.sh --inject-app bootloader.n3pro [working_dir]' >&2
    echo '  ./n3pro_patcher.sh --pack       [working_dir]  [output.upt]' >&2
    exit 1
}

mode=""
if [[ $# -ge 1 && "$1" == --* ]]; then
    mode="$1"
    shift
fi

case "$mode" in
    "")
        [[ $# -eq 2 ]] || usage
        updatefile="$1"
        bootloader="$2"
        workingdir="$(realpath -m ./working_dir)"
        updatefile_rb="${updatefile%.*}_rb.upt"
        do_unpack=1; do_inject=1; do_pack=1; do_cleanup=1
        ;;
    --unpack)
        [[ $# -ge 1 && $# -le 2 ]] || usage
        updatefile="$1"
        workingdir="$(realpath -m "${2:-./working_dir}")"
        do_unpack=1; do_inject=0; do_pack=0; do_cleanup=0
        ;;
    --inject-app)
        [[ $# -ge 1 && $# -le 2 ]] || usage
        bootloader="$1"
        workingdir="$(realpath -m "${2:-./working_dir}")"
        do_unpack=0; do_inject=1; do_pack=0; do_cleanup=0
        ;;
    --pack)
        [[ $# -le 2 ]] || usage
        workingdir="$(realpath -m "${1:-./working_dir}")"
        updatefile_rb="${2:-$(basename "$workingdir")_rb.upt}"
        do_unpack=0; do_inject=0; do_pack=1; do_cleanup=0
        ;;
    *)
        usage
        ;;
esac

if [[ -n "${updatefile_rb:-}" && "$updatefile_rb" != /* ]]; then
    updatefile_rb="$(pwd)/$updatefile_rb"
fi

iso_in="$workingdir/in/iso"
rootfs="$workingdir/in/rootfs"
iso_out="$workingdir/out/iso"

################################################################################
### unpack
################################################################################
if [[ "$do_unpack" -eq 1 ]]; then
    rm -rf "$workingdir"
    mkdir -p "$iso_in" "$rootfs" "$iso_out"

    7z -o"$iso_in" x "$updatefile" >/dev/null

    # ISO9660 stores file names in upper case; normalise to the lower-case
    # names that update.txt refers to.
    (
        cd "$iso_in"
        for f in *; do
            lc="$(printf '%s' "$f" | tr '[:upper:]' '[:lower:]')"
            [ "$f" = "$lc" ] || mv -- "$f" "$lc"
        done
    )

    echo "Extracting rootfs (this may take a while)..."
    ubireader_extract_files -k -o "$rootfs" "$iso_in/system.ubi" >/dev/null

    # ubireader keeps the UBIFS inode uid/gid; normalise like the stock image.
    sudo chown -R "$FS_UID:$FS_GID" "$rootfs"

    echo "Rootfs extracted to: $rootfs"
fi

################################################################################
### inject-app
################################################################################
if [[ "$do_inject" -eq 1 ]]; then
    [[ -d "$rootfs" ]] || { echo "No extracted rootfs in $rootfs; run --unpack first" >&2; exit 1; }
    bl="$(realpath "$bootloader")"

    # 1. install the Rockbox bootloader next to the stock player
    sudo cp "$bl" "$rootfs/usr/bin/$BOOTLOADER_BASENAME"
    sudo chmod 0775 "$rootfs/usr/bin/$BOOTLOADER_BASENAME"
    sudo chown "$FS_UID:$FS_GID" "$rootfs/usr/bin/$BOOTLOADER_BASENAME"

    # 2. replace the stock player launcher; the stock /usr/bin/hiby_player
    #    binary itself is left in place and can be selected from the menu.
    sudo tee "$rootfs/usr/bin/hiby_player.sh" >/dev/null << EOF
#!/bin/sh
# Rockbox hosted launcher (Cayin N3Pro)
killall    hiby_player      &>/dev/null
killall -9 hiby_player      &>/dev/null
killall    $BOOTLOADER_BASENAME &>/dev/null
killall -9 $BOOTLOADER_BASENAME &>/dev/null
/usr/bin/$BOOTLOADER_BASENAME
sleep 1
reboot
EOF
    sudo chmod 0775 "$rootfs/usr/bin/hiby_player.sh"
    sudo chown "$FS_UID:$FS_GID" "$rootfs/usr/bin/hiby_player.sh"

    # 3. SD hotplug helpers (mmc -> /mnt/sd_0, usb -> /mnt/usb)
    sudo tee "$rootfs/etc/rb_inserting.sh" >/dev/null << 'EOF'
#!/bin/sh
case $MDEV in
 mmc*) MNT_POINT=/mnt/sd_0 ;;
 sd*)  MNT_POINT=/mnt/usb  ;;
esac
[ -d "$MNT_POINT" ] || mkdir -p "$MNT_POINT"
mount "$MDEV" "$MNT_POINT"
EOF
    sudo tee "$rootfs/etc/rb_removing.sh" >/dev/null << 'EOF'
#!/bin/sh
case $MDEV in
 mmc*) MNT_POINT=/mnt/sd_0 ;;
 sd*)  MNT_POINT=/mnt/usb  ;;
esac
sync
umount -f "$MNT_POINT" || umount -f "$MDEV"
EOF
    sudo chmod 0775 "$rootfs/etc/rb_inserting.sh" "$rootfs/etc/rb_removing.sh"
    sudo chown "$FS_UID:$FS_GID" "$rootfs/etc/rb_inserting.sh" "$rootfs/etc/rb_removing.sh"

    mdev="$rootfs/etc/mdev.conf"
    grep -q 'rb_inserting.sh' "$mdev" || sudo tee -a "$mdev" >/dev/null << 'EOF'
sd[a-z][0-9]+     0:0 664 @ /etc/rb_inserting.sh
mmcblk[0-9]p[0-9] 0:0 664 @ /etc/rb_inserting.sh
mmcblk[0-9]       0:0 664 @ /etc/rb_inserting.sh
sd[a-z]           0:0 664 $ /etc/rb_removing.sh
mmcblk[0-9]       0:0 664 $ /etc/rb_removing.sh
EOF

    # 4. do not let sys_server wipe the mounted card
    sudo perl -pni -e 's/\brm -rf\b/#rm -Rf/;' "$rootfs/etc/init.d/S50sys_server"

    # 5. bootloader version marker
    if [[ -f tools/rockbox-info.txt ]]; then
        ver="$(grep '^Version' tools/rockbox-info.txt | cut -f2 -d' ')"
    else
        ver="unknown"
    fi
    echo "$ver" | sudo tee "$rootfs/etc/rockbox-bl-info.txt" >/dev/null
    sudo chmod 0644 "$rootfs/etc/rockbox-bl-info.txt"
    sudo chown "$FS_UID:$FS_GID" "$rootfs/etc/rockbox-bl-info.txt"
fi

################################################################################
### pack
################################################################################
if [[ "$do_pack" -eq 1 ]]; then
    [[ -d "$rootfs" ]] || { echo "No rootfs in $rootfs; run --unpack first" >&2; exit 1; }

    # kernel md5 is taken from the original update.txt so the stock kernel is
    # left completely untouched.
    kernel_md5="$(sed -n 's/.*md5=\([0-9a-f]\{32\}\).*/\1/p' "$iso_in/update.txt" | head -1)"
    [[ -n "$kernel_md5" ]] || { echo "cannot find kernel md5 in $iso_in/update.txt" >&2; exit 1; }

    rm -f "$workingdir/system.ubi"
    sudo mkfs.ubifs -r "$rootfs" -m "$UBIFS_MIN_IO" -e "$UBIFS_LEB_SIZE" \
                    -c "$LEB_CNT" -o "$workingdir/system.ubi"
    rootfs_md5="$(md5sum "$workingdir/system.ubi" | awk '{print $1}')"

    cat > "$iso_out/update.txt" << EOF

kernel={
        name=kernel
        file_path=autoupdate/uimage.bin
        md5=$kernel_md5
}

rootfs={
        name=rootfs
        full_upgrade=yes
        file_path=autoupdate/system.ubi
        md5=$rootfs_md5
}
EOF

    cp "$iso_in/uboot.bin" "$iso_in/uimage.bin" "$iso_in/version.txt" "$iso_out/" 2>/dev/null || true
    [[ -f "$iso_in/_gitigno" ]] && cp "$iso_in/_gitigno" "$iso_out/"
    cp "$workingdir/system.ubi" "$iso_out/system.ubi"

    genisoimage -R -o "$updatefile_rb" \
        "$iso_out/uboot.bin" "$iso_out/uimage.bin" "$iso_out/system.ubi" \
        "$iso_out/update.txt" "$iso_out/version.txt" "$iso_out/_gitigno"

    echo "Image created: $updatefile_rb"
fi

if [[ "$do_cleanup" -eq 1 ]]; then
    sudo rm -rf "$workingdir"
fi

exit 0
