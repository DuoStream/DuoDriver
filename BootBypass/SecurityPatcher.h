#ifndef SECURITY_PATCHER_H
#define SECURITY_PATCHER_H

#include "BootBypass.h"
#include "SystemUtils.h"
#include "DriverManager.h"
#include "SetupManager.h"

// IOCTL Operations
BOOLEAN WriteMemory32(HANDLE hDriver, ULONGLONG address, ULONG value, ULONG ioctl);
BOOLEAN WriteMemory64(HANDLE hDriver, ULONGLONG address, ULONGLONG value, ULONG ioctl);
BOOLEAN ReadMemory64(HANDLE hDriver, ULONGLONG address, ULONGLONG* value, ULONG ioctl);
ULONGLONG GetNtoskrnlBase(void);
HANDLE OpenDriverDevice(PCWSTR deviceName);

// DSE State Logic
BOOLEAN SaveStateSection(ULONGLONG callback);
BOOLEAN LoadStateSection(ULONGLONG* outCallback);
BOOLEAN RemoveStateSection(void);

// Main Patch Routine
NTSTATUS ExecuteAutoPatchLoad(PINI_ENTRY entry, PCONFIG_SETTINGS config, PULONGLONG originalCallback);

#endif