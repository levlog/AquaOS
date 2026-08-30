#!/usr/bin/env bash
# AquaOS toolchain bootstrap (no root required).
# Downloads .deb packages and extracts them into a local prefix.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEBS=$ROOT/tools/debs
PREF=$ROOT/tools/prefix
mkdir -p "$DEBS" "$PREF"

PKGS="busybox-static linux-image-amd64 grub-common grub-pc-bin xorriso cpio qemu-system-x86 qemu-system-data qemu-block-extra"

cd "$DEBS"
echo "[1/3] Resolving dependencies..."
ALL=$({ echo "$PKGS" | tr ' ' '\n'
        apt-cache depends --recurse --no-recommends --no-suggests --no-conflicts --no-breaks --no-replaces --no-enhances $PKGS 2>/dev/null \
          | awk '/Pre?Depends:|Depends:/{print $2}'
      } | sed 's/[():,].*//' | grep -E '^[a-z0-9][a-z0-9.+-]*$' | sort -u)
echo "Packages to download: $(echo "$ALL" | wc -l)"

echo "[2/3] Downloading (this may take a few minutes)..."
apt-get download $ALL > /tmp/aqua_dl.log 2>&1
RC=$?
echo "apt-get download rc=$RC; debs: $(ls ./*.deb 2>/dev/null | wc -l)"
# Retry any stragglers one by one
for p in $ALL; do
    if ! ls ./"${p}"_*.deb >/dev/null 2>&1; then
        apt-get download "$p" >/dev/null 2>&1 || true
    fi
done

echo "[3/3] Extracting into prefix..."
for f in ./*.deb; do
    dpkg -x "$f" "$PREF" 2>/dev/null || echo "WARN: failed to extract $f"
done

cat > "$ROOT/tools/env.sh" <<EOF
export AQUA_TOOLS=$ROOT/tools
export PATH=\$AQUA_TOOLS/prefix/usr/bin:\$AQUA_TOOLS/prefix/usr/sbin:\$AQUA_TOOLS/prefix/bin:\$PATH
export LD_LIBRARY_PATH=\$AQUA_TOOLS/prefix/usr/lib/x86_64-linux-gnu:\$AQUA_TOOLS/prefix/lib/x86_64-linux-gnu:\${LD_LIBRARY_PATH:-}
EOF

echo "=== Verify ==="
source "$ROOT/tools/env.sh"
command -v grub-mkimage && echo "grub-mkimage OK"
command -v xorriso && echo "xorriso OK"
command -v cpio && echo "cpio OK"
command -v qemu-system-x86_64 && echo "qemu OK"
ls "$PREF"/usr/bin/busybox && echo "busybox OK"
ls "$PREF"/boot/vmlinuz-* && echo "kernel OK"
ls "$PREF"/usr/lib/grub/i386-pc/cdboot.img && echo "cdboot OK"
ls "$PREF"/usr/share/grub/unicode.pf2 && echo "unicode font OK"
echo "DONE"
