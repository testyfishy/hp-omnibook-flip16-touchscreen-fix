#!/bin/bash
# Removes the DKMS quirk and restores the stock i2c-hid-acpi module (storm and dead touch return).
set -eu; K=$(uname -r)
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
dkms remove i2c-hid-elan-fix/2.0 --all || true
rm -rf /usr/src/i2c-hid-elan-fix-2.0
if command -v dracut >/dev/null; then dracut -f "/boot/initrd.img-$K" "$K"; else update-initramfs -u -k "$K"; fi
echo "done - reboot"
