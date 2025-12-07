#ifndef SETUP_MANAGER_H
#define SETUP_MANAGER_H

#include "BootBypass.h"
#include "SystemUtils.h"

BOOLEAN ExtractOmniDriverFromResource(void);
NTSTATUS CleanupOmniDriver(void);
BOOLEAN CheckAndDisableHVCI(void);
NTSTATUS RestoreHVCI(void);

// wesmar from ds_test 
// int PatchSystemHiveHVCI(BOOLEAN enable);

#endif