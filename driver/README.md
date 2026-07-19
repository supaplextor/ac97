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

### Prerequisites

| Tool | Version | Where to get it |
|------|---------|-----------------|
| Visual Studio 2026 | 20.x or newer (Community edition is fine) | [visualstudio.microsoft.com](https://visualstudio.microsoft.com/) |
| Windows Driver Kit (WDK) | Matching your VS 2026 install | [learn.microsoft.com/windows-hardware/drivers/download-the-wdk](https://learn.microsoft.com/en-us/windows-hardware/drivers/download-the-wdk) |
| Windows SDK | Installed automatically with VS 2026 | Included in VS installer |

> **Important:** The WDK version must match the Windows SDK version installed with Visual Studio. The WDK installer will warn you if there is a mismatch. Both should target **Windows 10/11 (10.0.26100 or later)** for Windows 11 compatibility.

### Step 1 — Install Visual Studio 2026

1. Download and run the **Visual Studio 2026** installer.
2. In the **Workloads** tab, select **Desktop development with C++**.
3. In the **Individual components** tab, ensure the following are checked:
   - `MSVC v144 - VS 2026 C++ x64/x86 build tools`
   - `Windows 11 SDK (10.0.26100.0)` (or the latest available)
   - `Spectre-mitigated libraries` (recommended for kernel code)
4. Click **Install**.

### Step 2 — Install the WDK

1. After VS 2026 finishes, download the **WDK for Windows 11** from the link in the Prerequisites table above.
2. Run the WDK installer. It will automatically install the **WDK VS extension** into Visual Studio 2026.
3. Confirm the extension is present: in Visual Studio, go to **Extensions → Manage Extensions** and verify **Windows Driver Kit** appears under Installed.

### Step 3 — Create a kernel-mode driver project

If you are building from the existing source files in this folder rather than a `.vcxproj`, create a new project to host them:

1. Open Visual Studio 2026.
2. Select **Create a new project**, search for **"Kernel Mode Driver, Empty (KMDF)"**, and click **Next**.
   - If you do not need KMDF, choose **"Empty WDM Driver"** instead. This driver uses WDM directly.
3. Set the project name (e.g. `ac97qemu`) and choose a location, then click **Create**.
4. In **Solution Explorer**, right-click **Source Files → Add → Existing Item** and add `ac97qemu.c`.
5. Add `ac97qemu.inf` to the project's **Driver Files** filter (right-click **Driver Files → Add → Existing Item**).

### Step 4 — Configure project properties

Right-click the project in Solution Explorer, choose **Properties**, and verify or set the following:

| Property path | Value |
|---------------|-------|
| **General → Platform Toolset** | `Windows Driver Kit (v144)` |
| **General → Target Platform** | `Universal` or `Desktop` |
| **General → Target Platform Version** | `10.0.26100.0` (Windows 11) |
| **C/C++ → General → Warning Level** | `W4` |
| **Driver Settings → Target OS Version** | `Windows 10 and later` |
| **Driver Settings → Driver Type** | `WDM` |
| **Inf2Cat → Run Inf2Cat** | `Yes` (generates the `.cat` for signing) |

Set **Configuration** to `Release` and **Platform** to `x64` in the toolbar before building.

### Step 5 — Build

```
Build → Build Solution   (Ctrl+Shift+B)
```

On success the output directory (e.g. `x64\Release\`) will contain:

```
ac97qemu.sys   ← kernel driver binary
ac97qemu.inf   ← installation manifest
ac97qemu.cat   ← security catalog (if Inf2Cat ran)
```

If you see **error MSB8040** (Spectre-mitigated libraries required), install them via the VS installer under **Individual components → MSVC … Spectre-mitigated libs**.

### Step 6 — Sign the driver package

**For test use only (Windows 11 test VM / QEMU guest):**

```cmd
:: Create a self-signed test certificate (one-time)
makecert -r -pe -ss TestCertStore -n "CN=AC97TestCert" AC97TestCert.cer

:: Sign the .sys and .cat
signtool sign /s TestCertStore /n "AC97TestCert" /t http://timestamp.digicert.com ac97qemu.sys
signtool sign /s TestCertStore /n "AC97TestCert" /t http://timestamp.digicert.com ac97qemu.cat
```

> `makecert` and `signtool` are shipped with the WDK/SDK and are available in a **Developer Command Prompt for VS 2026**.

For production use, replace the self-signed certificate with an EV code-signing certificate submitted through the [Windows Hardware Dev Center (WHQL)](https://partner.microsoft.com/dashboard/hardware).

## Install in a Windows guest (test signing)

1. Enable testsigning mode:
   - `bcdedit /set testsigning on`
2. Reboot the guest.
3. In Device Manager, update the AC97 audio controller driver and point to this folder.
4. Verify the device binds to **QEMU AC97 Audio Driver**.
