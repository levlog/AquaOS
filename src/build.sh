#!/usr/bin/env bash
# AquaOS build: initramfs + GRUB BIOS ISO.
# Usage: [DEBUG=1] [WALLPAPER=path/to/img.jpg] ./build.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC=$ROOT/src
BUILD=$ROOT/build
WALL_SRC=${WALLPAPER:-$ROOT/assets/wallpaper.jpg}
source "$ROOT/tools/env.sh"
P=$AQUA_TOOLS/prefix
DEBUG=${DEBUG:-0}

mkdir -p "$BUILD"

KVER=$(basename "$(ls "$P"/boot/vmlinuz-* | head -n1)" | sed 's/^vmlinuz-//')
VMLINUZ="$P/boot/vmlinuz-$KVER"
echo "[*] Kernel: $KVER"
echo "[*] Wallpaper: $WALL_SRC"

echo "[1/6] Font + wallpaper"
python3 "$SRC/mkfont.py" "$BUILD/font.h"
python3 "$SRC/mkwallpaper.py" "$WALL_SRC" "$BUILD/wallpaper.raw"

echo "[2/6] splash (static)"
gcc -O2 -std=gnu11 -static -I"$BUILD" -o "$BUILD/splash" "$SRC/splash.c" -lm
strip "$BUILD/splash"

echo "[3/6] initramfs"
RM="$BUILD/rootfs"
rm -rf "$RM"
mkdir -p "$RM"/bin "$RM"/usr/bin "$RM"/usr/share/splash "$RM"/proc "$RM"/sys "$RM"/dev "$RM"/run "$RM"/tmp
cp "$P/usr/bin/busybox" "$RM/bin/busybox"
cp "$BUILD/splash" "$RM/usr/bin/splash"
cp "$BUILD/wallpaper.raw" "$RM/usr/share/splash/wallpaper.raw"

# PS/2 mouse driver (busybox insmod cannot read .xz, so unpack it here)
mkdir -p "$RM/lib/modules"
KMOD="$P/usr/lib/modules/$KVER/kernel/drivers/input/mouse/psmouse.ko.xz"
if [ -f "$KMOD" ]; then
    unxz -c "$KMOD" > "$RM/lib/modules/psmouse.ko"
else
    echo "WARN: psmouse.ko.xz not found - mouse will not work"
fi
install -m 0755 "$SRC/init" "$RM/init"
( cd "$RM" && find . -print0 | cpio --null -o -H newc --quiet | gzip -9 > "$BUILD/initrd.gz" )
ls -la "$BUILD/initrd.gz"

echo "[4/6] GRUB core image (BIOS)"
MODDIR="$P/usr/lib/grub/i386-pc"
MODS="iso9660 biosdisk linux normal gfxterm all_video font configfile echo ls test sleep reboot halt terminal cat"
grub-mkimage -O i386-pc -d "$MODDIR" -o "$BUILD/core.img" -p '(cd)/boot/grub' $MODS
cat "$MODDIR/cdboot.img" "$BUILD/core.img" > "$BUILD/boot_grub.img"

echo "[5/6] ISO tree"
ISO="$BUILD/isodir"
rm -rf "$ISO"
mkdir -p "$ISO/boot/grub/fonts" "$ISO/boot/grub/i386-pc"
cp "$VMLINUZ" "$ISO/boot/vmlinuz"
cp "$BUILD/initrd.gz" "$ISO/boot/initrd.gz"
cp "$P/usr/share/grub/unicode.pf2" "$ISO/boot/grub/fonts/"
cp "$MODDIR"/*.mod "$ISO/boot/grub/i386-pc/" 2>/dev/null || true
cp "$BUILD/boot_grub.img" "$ISO/boot/grub/boot_grub.img"

if [ "$DEBUG" = "1" ]; then
    PARAMS="loglevel=0 vt.global_cursor_default=0 aquadebug"
else
    PARAMS="loglevel=0 vt.global_cursor_default=0"
fi
cat > "$ISO/boot/grub/grub.cfg" <<EOF
set default=0
set timeout=1
set timeout_style=hidden
insmod all_video
set gfxmode=1024x768x32,1024x768x24,800x600x32,800x600x24,auto
set gfxpayload=keep
insmod gfxterm
terminal_output gfxterm
menuentry "AquaOS" {
    linux /boot/vmlinuz $PARAMS
    initrd /boot/initrd.gz
}
EOF

echo "[6/6] ISO"
xorriso -as mkisofs -o "$BUILD/AquaOS.iso" \
    -isohybrid-mbr "$MODDIR/boot_hybrid.img" \
    -c /boot/grub/boot.cat -b /boot/grub/boot_grub.img \
    -no-emul-boot -boot-load-size 4 -boot-info-table \
    -volid AQUAOS -rational-rock -joliet \
    "$ISO" > /tmp/aqua_xorriso.log 2>&1
tail -2 /tmp/aqua_xorriso.log
ls -la "$BUILD/AquaOS.iso"
echo "BUILD OK: $BUILD/AquaOS.iso"
