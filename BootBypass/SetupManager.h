#ifndef SETUP_MANAGER_H
#define SETUP_MANAGER_H

#include "BootBypass.h"
#include "SystemUtils.h"

// Resource Extraction
BOOLEAN ExtractOmniDriverFromResource(void);
NTSTATUS CleanupOmniDriver(void);

// HVCI & Reboot Guardian Logic
BOOLEAN CheckAndDisableHVCI(void);
NTSTATUS RestoreHVCI(void);
NTSTATUS CreateRebootGuardianService(void);
NTSTATUS RemoveRebootGuardianService(void);
NTSTATUS AddThemesDependency(void);
NTSTATUS RemoveThemesDependency(void);

#endif