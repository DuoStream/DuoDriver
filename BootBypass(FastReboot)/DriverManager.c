#include "DriverManager.h"

BOOLEAN IsDriverLoaded(PCWSTR serviceName) {
    WCHAR fullServicePath[MAX_PATH_LEN];
    UNICODE_STRING usServiceName;
    NTSTATUS status;

    // Safe path construction with overflow check
    SIZE_T baseLen = wcscpy_safe(fullServicePath, MAX_PATH_LEN, 
                                  L"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\");
    if (baseLen >= MAX_PATH_LEN - 1) return FALSE;
    
    SIZE_T finalLen = wcscat_safe(fullServicePath, MAX_PATH_LEN, serviceName);
    if (finalLen >= MAX_PATH_LEN) {
        // Truncation occurred - path invalid
        return FALSE;
    }
    
    RtlInitUnicodeString(&usServiceName, fullServicePath);
    
    status = NtLoadDriver(&usServiceName);
    if (status == STATUS_IMAGE_ALREADY_LOADED) return TRUE;
    if (NT_SUCCESS(status)) {
        NtUnloadDriver(&usServiceName);
        return FALSE;
    }
    return FALSE;
}

NTSTATUS CreateDriverRegistryEntry(PCWSTR serviceName, PCWSTR imagePath, PCWSTR driverType, PCWSTR startType) {
    WCHAR fullServicePath[MAX_PATH_LEN];
    UNICODE_STRING usServiceName, usValueName;
    OBJECT_ATTRIBUTES oa;
    HANDLE hKey = NULL;
    NTSTATUS status;
    ULONG disposition;
    DWORD dwValue;
    WCHAR tempBuffer[MAX_PATH_LEN];
    ULONG dataSize;

    // Safe path construction
    SIZE_T baseLen = wcscpy_safe(fullServicePath, MAX_PATH_LEN, 
                                  L"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\");
    if (baseLen >= MAX_PATH_LEN - 1) return STATUS_OBJECT_NAME_INVALID;
    
    SIZE_T finalLen = wcscat_safe(fullServicePath, MAX_PATH_LEN, serviceName);
    if (finalLen >= MAX_PATH_LEN) return STATUS_OBJECT_NAME_INVALID;
    
    RtlInitUnicodeString(&usServiceName, fullServicePath);
    InitializeObjectAttributes(&oa, &usServiceName, OBJ_CASE_INSENSITIVE, NULL, NULL);

    status = NtCreateKey(&hKey, KEY_ALL_ACCESS, &oa, 0, NULL, REG_OPTION_NON_VOLATILE, &disposition);
    if (!NT_SUCCESS(status)) return status;

    // ImagePath value
    RtlInitUnicodeString(&usValueName, L"ImagePath");
    SIZE_T pathLen = wcscpy_safe(tempBuffer, MAX_PATH_LEN, imagePath);
    if (pathLen >= MAX_PATH_LEN) {
        NtClose(hKey);
        return STATUS_OBJECT_NAME_INVALID;
    }
    dataSize = (ULONG)((pathLen + 1) * sizeof(WCHAR));
    status = NtSetValueKey(hKey, &usValueName, 0, REG_EXPAND_SZ, tempBuffer, dataSize);

    // DisplayName value
    RtlInitUnicodeString(&usValueName, L"DisplayName");
    SIZE_T nameLen = wcslen(serviceName);
    if (nameLen >= MAX_PATH_LEN) {
        NtClose(hKey);
        return STATUS_OBJECT_NAME_INVALID;
    }
    dataSize = (ULONG)((nameLen + 1) * sizeof(WCHAR));
    NtSetValueKey(hKey, &usValueName, 0, REG_SZ, (PVOID)serviceName, dataSize);

    // Type value
    dwValue = (_wcsicmp_impl(driverType, L"FILE_SYSTEM") == 0) ? 2 : 1;
    RtlInitUnicodeString(&usValueName, L"Type");
    NtSetValueKey(hKey, &usValueName, 0, REG_DWORD, &dwValue, sizeof(DWORD));

    // Start value
    if (_wcsicmp_impl(startType, L"BOOT") == 0) dwValue = 0;
    else if (_wcsicmp_impl(startType, L"SYSTEM") == 0) dwValue = 1;
    else if (_wcsicmp_impl(startType, L"AUTO") == 0) dwValue = 2;
    else if (_wcsicmp_impl(startType, L"DISABLED") == 0) dwValue = 4;
    else dwValue = 3;

    RtlInitUnicodeString(&usValueName, L"Start");
    NtSetValueKey(hKey, &usValueName, 0, REG_DWORD, &dwValue, sizeof(DWORD));

    // ErrorControl value
    dwValue = 1;
    RtlInitUnicodeString(&usValueName, L"ErrorControl");
    NtSetValueKey(hKey, &usValueName, 0, REG_DWORD, &dwValue, sizeof(DWORD));

    NtClose(hKey);
    return status;
}

NTSTATUS LoadDriver(PCWSTR serviceName, PCWSTR imagePath, PCWSTR driverType, PCWSTR startType) {
    WCHAR fullServicePath[MAX_PATH_LEN];
    UNICODE_STRING usServiceName;
    NTSTATUS status;

    status = CreateDriverRegistryEntry(serviceName, imagePath, driverType, startType);
    if (!NT_SUCCESS(status) && status != STATUS_OBJECT_NAME_COLLISION) return status;

    // Safe path construction
    SIZE_T baseLen = wcscpy_safe(fullServicePath, MAX_PATH_LEN, 
                                  L"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\");
    if (baseLen >= MAX_PATH_LEN - 1) return STATUS_OBJECT_NAME_INVALID;
    
    SIZE_T finalLen = wcscat_safe(fullServicePath, MAX_PATH_LEN, serviceName);
    if (finalLen >= MAX_PATH_LEN) return STATUS_OBJECT_NAME_INVALID;
    
    RtlInitUnicodeString(&usServiceName, fullServicePath);
    return NtLoadDriver(&usServiceName);
}

NTSTATUS UnloadDriver(PCWSTR serviceName) {
    WCHAR fullServicePath[MAX_PATH_LEN];
    UNICODE_STRING usServiceName;
    
    // Safe path construction
    SIZE_T baseLen = wcscpy_safe(fullServicePath, MAX_PATH_LEN, 
                                  L"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\");
    if (baseLen >= MAX_PATH_LEN - 1) return STATUS_OBJECT_NAME_INVALID;
    
    SIZE_T finalLen = wcscat_safe(fullServicePath, MAX_PATH_LEN, serviceName);
    if (finalLen >= MAX_PATH_LEN) return STATUS_OBJECT_NAME_INVALID;
    
    RtlInitUnicodeString(&usServiceName, fullServicePath);
    return NtUnloadDriver(&usServiceName);
}