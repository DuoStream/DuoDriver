# DuoDriver

Windows Update KB5094126 (June 2026) removed official support for HDR inside remote desktop sessions.

This repository implements a two-driver based patch that brings HDR support back by pinning the HDR source mode in Windows' `dxgkrnl.sys` driver.

> **Note:** Both "Memory integrity" and "Microsoft Vulnerable Driver Blocklist" under Core isolation must be **disabled** for the code in this repository to function.

## Projects

### DuoDriver
A Windows Driver Framework (WDF) kernel driver that provides read-only memory write support for patching protected kernel memory.

It works by mapping the physical address of the target memory via `MmMapIoSpace`, bypassing page-level read-only protection without requiring a TLB flush.

It also supports traditional `MmCopyVirtualMemory` based read/write, bulk operations, and kernel module enumeration.

### drvcab
A packaging tool that bundles driver binaries into encrypted cabinet files for embedded resource use.

It's used to ship `DuoDriver.sys` and `RTCore64.sys` inside the `hdrenabler` and `drvloader` executables.

### drvloader
A sample program that demonstrates loading unsigned kernel drivers from user mode.

It's internally used for development and testing of the driver components in this repository.

### hdrenabler
A user-mode tool that downloads the appropriate `dxgkrnl.sys` symbol file, resolves the `IsHdrSourceModePinned` function offset, and patches the kernel to force-enable HDR support in remote sessions.

It can also restore the original behavior.

Supports both interactive and command-line modes (`enable`, `disable`, `status`).