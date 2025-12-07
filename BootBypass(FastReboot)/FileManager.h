#ifndef FILE_MANAGER_H
#define FILE_MANAGER_H

#include "BootBypass.h"
#include "SystemUtils.h"

NTSTATUS ExecuteRename(PINI_ENTRY entry);
NTSTATUS ExecuteDelete(PINI_ENTRY entry);

#endif