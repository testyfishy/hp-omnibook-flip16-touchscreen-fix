# HP OmniBook X Flip 16 (Lunar Lake) touchscreen fix for Linux

Makes the ELAN2514 (`04F3:43F0`) touchscreen work on the HP OmniBook X Flip 16
(boards `8DA0` / `8DA1`, Intel Core Ultra 200V) and removes the ~42,000 interrupts/s
storm on `i2c_designware.2` that these machines show on every boot.

Tested on Ubuntu 26.04.1, kernel 7.0.0-31-generic, BIOS F.10, board 8DA1.
Verified across cold boot, warm reboot and suspend/resume (s2idle).

## Symptoms

* `/proc/interrupts`: IRQ of `i2c_designware.2` climbs at ~42k/s from boot; the ELAN2514
  line (level, active low) at ~600/s. `top` shows a busy kworker/irq thread, package
  C-states never go below PC0, about 0.55 W extra SoC power at idle.
* The touchscreen enumerates fine (HID descriptor reads, reset is acknowledged,
  `hid-multitouch` creates a Touchscreen input node) but never delivers a single report.
* Bug is intermittent on some units (see the linux-input thread linked below); on this unit
  it happened on all 29 boots since May 2026, kernels 7.0.0-14 through 7.0.0-31.

## Root cause

The touch controller's interrupt is GPIO pad **GPP_E_18** (community `INTC105D:01`
pin 44, IO-APIC input 108). For a GPIO pad to be delivered as an APIC interrupt, bit 20
(`GPIROUTIOXAPIC`) of its `PADCFG0` register must be set.

HP's SSDT declares an ACPI power resource `PTPL` for the panel/touch controller. Its
`_ON` method ends with

```
\_SB.SGRA (TPI2, TPIP)     // set PADCFG0.bit20 of pad TPI2 := TPIP
```

and `TPIP` is `0` on this firmware, so `_ON` **clears** the IO-APIC routing bit.
(`_OFF` sets it to `TPIP ^ 1 = 1`, i.e. the polarities are inverted; the DSDT `_INI`
path sets it to `1` correctly.) Linux evaluates `_ON` for every power resource that is
already on during boot (`acpi_bus_init_power`) and again on every D3 to D0 transition, so
from the first i2c-hid probe onwards:

* the IO-APIC input 108 is not connected to the pad and floats asserted: the kernel's
  level-triggered handler runs continuously (the 42k/s "storm" is the I2C reads it issues),
* real touch interrupts never reach the CPU,
* the controller is polled with garbage during and after its reset and ends up in a state
  from which only a panel power cycle (cold boot) recovers.

Windows evidently does not evaluate `_ON` this way, which is why the panel works there.

Evidence: with the driver bound, `PADCFG0(GPP_E_18) = 0x80000102` (bit 20 clear); with the
device in D3cold (after `_OFF`) it reads `0x80100102`; the working touchpad pad GPP_F_18
reads `0x80100102`. Calling `\_SB.SGRA(\GPLI, 1)` by hand (acpi_call) flips the register
to `0x80100102` and both interrupt rates drop to 0 instantly. `\GPLI` is the firmware's
own variable holding the pad number (`0x180492`).

## Before / after, measured on this unit

Idle on battery, lid open, HP power-saver profile, same session, switching the routing bit
back and forth with the driver attached (`storm-cost` run, 17 Sep 2026):

| | Storm (stock) | Fixed | 
|---|---|---|
| `i2c_designware.2` IRQ 20 rate | ~26,000 to 42,000 /s | 0 /s |
| ELAN2514 IRQ 108 rate (idle) | ~600 /s | 0 /s |
| Total system interrupts | 32,374 /s | 4,287 /s |
| SoC package power | 1.85 W | 1.30 W |
| Core power | 0.39 W | 0.16 W |
| Package C2 residency | 0.0 % | 25.1 % |
| Package C6 / C10 residency | 0.0 % / 0.0 % | 0.7 % / 0.5 % |
| Core C7 residency | 24.5 % | 38.5 % |
| Average core clock | 1259 MHz | 988 MHz |
| `PADCFG0` of GPP_E_18 | `0x80000102` (bit 20 clear) | `0x80100102` (bit 20 set) |
| Touch input reports | none, ever | multitouch works |

Touch behaviour by how and when the bit gets set:

| Scenario | IRQ 20 storm | Touch reports in a 15 s finger test |
|---|---|---|
| Stock kernel 7.0.0-31 (29 boots since May 2026) | yes, every boot | 0 |
| Bit set from user space after boot (acpi_call + udev/boot service) | stops | 0 (controller already hung) |
| Bit set by the driver quirk, warm reload, same boot | none | 0 (controller already hung) |
| Bit set by the driver quirk, cold boot | none from byte 0 | 2,091 events, IRQ 108 +346 |
| Same, after s2idle suspend/resume | none | 3,506 events, full X/Y range |
| Same, warm reboot with stock i2c-hid core (final package) | none | 1,619 events, 14 touches |

The storm alone costs about 0.55 W of SoC power and blocks all package C-states. Battery
draw at idle barely moves (the OLED panel dominates), but the controller being hung is what
makes the touchscreen dead.

## The fix

`hp-elan-irq-route.patch` (about 50 lines against `drivers/hid/i2c-hid/i2c-hid-acpi.c`,
kernel 7.0) adds a `power_up` hook to the ACPI glue driver. On HP boards 8DA0/8DA1, for the
`ELAN2514` device only, it evaluates `\_SB.SGRA(\GPLI, 1)` before the i2c-hid core issues its
first reset, and again on every resume. That re-sets the routing bit using the firmware's own
helper, so nothing hardware-specific is hard-coded beyond the DMI match and the device HID.

Applying the fix from user space after boot (a udev rule or boot service calling acpi_call)
stops the storm but does **not** bring touch back on that boot: by then the controller has
already been hung by the garbage polling. The fix has to run inside the driver before the
first reset, which is why it is a kernel patch and not a script. The i2c-hid core module is
left completely stock.

## Install (DKMS, Ubuntu / Debian)

```
git clone https://github.com/testyfishy/hp-omnibook-flip16-touchscreen-fix
cd hp-omnibook-flip16-touchscreen-fix
sudo ./install.sh
sudo reboot
```

Check after reboot:

```
journalctl -b -k | grep 'HP quirk'
   ... \_SB_.PC00.I2C4.TPL1: HP quirk: routed touch INT pad 0x180492 to the IO-APIC (AE_OK)
grep -E '^ *(20|108):' /proc/interrupts    # both counts should stay small
```

`dkms/i2c-hid.h` is a verbatim copy of the in-tree private header (not shipped in
`linux-headers`), needed only to build the module out of tree. `sudo ./uninstall.sh`
returns to the stock module.

## Notes

* The quirk logs `AE_NOT_FOUND` and does nothing on firmware without `\_SB.SGRA` or `\GPLI`.
* This does not touch the separate ELAN pen report-rate issue tracked in Launchpad bug 2142384.
* Users who already see touch working on 7.x kernels most likely have firmware where `TPIP`
  is `1`, or where the intermittent variant of the bug did not strike on that boot.
* A cleaner upstream fix could live in the i2c-hid-acpi driver as here, or HP could ship a
  BIOS with `TPIP` corrected. Reported to the linux-input list; see the thread from May 2026:
  https://ratatoskr.run/linux-input/2026/05/9013594

## License

GPL-2.0, same as the kernel sources this is derived from.
