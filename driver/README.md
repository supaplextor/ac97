# QEMU AC97 Windows 10 driver (alpha)

This folder contains an initial Windows 10 x64 driver package for the QEMU AC97 PCI audio controller.

## Matched QEMU hardware IDs

- `PCI\\VEN_8086&DEV_2415`
- `PCI\\VEN_8086&DEV_2425`

## Build

1. Install Visual Studio 2022 and Windows Driver Kit (WDK) for Windows 10/11.
2. Build `ac97qemu.c` as a kernel-mode `.sys` driver (x64).
3. Place the resulting `ac97qemu.sys` next to `ac97qemu.inf` and sign the package (`.cat`) for test or production use.

## Install in a Windows 10 QEMU guest (test signing)

1. Enable testsigning mode:
   - `bcdedit /set testsigning on`
2. Reboot the guest.
3. In Device Manager, update the AC97 audio controller driver and point to this folder.
4. Verify the device binds to **QEMU AC97 Audio Driver**.
