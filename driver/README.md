# QEMU AC97 Windows driver skeleton (alpha)

This folder contains an expanded Windows x64 driver skeleton for the QEMU AC97 PCI audio controller.

## Current skeleton scope

The driver now mirrors the first controller-side steps from the FreeBSD 15.1 Intel ICH AC97 driver:

- discover the mixer (`NAMBAR`) and bus-master (`NABMBAR`) resource ranges from translated PnP resources
- support either port-mapped or memory-mapped register access
- perform controller codec-reset and channel-reset sequences during `IRP_MN_START_DEVICE`
- release mapped resources on stop, surprise-removal, and remove
- pass power IRPs down the stack correctly

This is still not a full audio driver yet: DMA programming, interrupt handling, codec enumeration, mixer controls, and PCM playback/record support are not implemented.

## Matched QEMU hardware IDs

- `PCI\\VEN_8086&DEV_2415`
- `PCI\\VEN_8086&DEV_2425`

## Build

1. Install Visual Studio 2022 or newer and the Windows Driver Kit (WDK) for Windows 10/11.
2. Build `ac97qemu.c` as a kernel-mode `.sys` driver (x64).
3. Place the resulting `ac97qemu.sys` next to `ac97qemu.inf` and sign the package (`.cat`) for test or production use.

## Install in a Windows guest (test signing)

1. Enable testsigning mode:
   - `bcdedit /set testsigning on`
2. Reboot the guest.
3. In Device Manager, update the AC97 audio controller driver and point to this folder.
4. Verify the device binds to **QEMU AC97 Audio Driver**.
