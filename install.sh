#!/bin/bash
# Installs the i2c-hid-acpi HP touch-INT routing quirk as DKMS package i2c-hid-elan-fix/2.0
# and rebuilds the running kernel's initrd so the patched module loads from the very first probe.
set -eu
V=2.0; SRC=/usr/src/i2c-hid-elan-fix-$V; K=$(uname -r)
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
command -v dkms >/dev/null || { echo "installing dkms + headers"; apt-get install -y dkms "linux-headers-$K"; }
rm -rf "$SRC"; mkdir -p "$SRC"; cp "$(dirname "$0")"/dkms/* "$SRC"/
dkms add -m i2c-hid-elan-fix -v $V 2>/dev/null || true
dkms build -m i2c-hid-elan-fix -v $V -k "$K"
dkms install -m i2c-hid-elan-fix -v $V -k "$K" --force
echo "module: $(modinfo -n i2c_hid_acpi)"
if command -v dracut >/dev/null; then dracut -f "/boot/initrd.img-$K" "$K"; else update-initramfs -u -k "$K"; fi
echo "done - reboot. Verify with: journalctl -b -k | grep 'HP quirk'"
