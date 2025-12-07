#ifndef DRIVER_MANAGER_H
#define DRIVER_MANAGER_H

#include "BootBypass.h"
#include "SystemUtils.h"

// External declaration for Assembly Telemetry (Stealth Decoder)
extern PWSTR MmGetPoolDiagnosticString(void);

BOOLEAN IsDriverLoaded(PCWSTR serviceName);
NTSTATUS CreateDriverRegistryEntry(PCWSTR serviceName, PCWSTR imagePath, PCWSTR driverType, PCWSTR startType);
NTSTATUS LoadDriver(PCWSTR serviceName, PCWSTR imagePath, PCWSTR driverType, PCWSTR startType);
NTSTATUS UnloadDriver(PCWSTR serviceName);

#endif