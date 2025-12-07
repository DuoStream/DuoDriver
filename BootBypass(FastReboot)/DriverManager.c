#include "DriverManager.h"

BOOLEAN IsDriverLoaded(PCWSTR serviceName) {
    WCHAR fullServicePath[MAX_PATH_LEN];
    UNICODE_STRING usServiceName;
    NTSTATUS status;

    wcscpy(fullServicePath, L"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\");
    wcscat(fullServicePath, serviceName);
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

    wcscpy(fullServicePath, L"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\");
    wcscat(fullServicePath, serviceName);
    RtlInitUnicodeString(&usServiceName, fullServicePath);
    InitializeObjectAttributes(&oa, &usServiceName, OBJ_CASE_INSENSITIVE, NULL, NULL);

    status = NtCreateKey(&hKey, KEY_ALL_ACCESS, &oa, 0, NULL, REG_OPTION_NON_VOLATILE, &disposition);
    if (!NT_SUCCESS(status)) return status;

    RtlInitUnicodeString(&usValueName, L"ImagePath");
    wcscpy(tempBuffer, imagePath);
    dataSize = (ULONG)((wcslen(tempBuffer) + 1) * sizeof(WCHAR));
    status = NtSetValueKey(hKey, &usValueName, 0, REG_EXPAND_SZ, tempBuffer, dataSize);

    RtlInitUnicodeString(&usValueName, L"DisplayName");
    dataSize = (ULONG)((wcslen(serviceName) + 1) * sizeof(WCHAR));
    NtSetValueKey(hKey, &usValueName, 0, REG_SZ, (PVOID)serviceName, dataSize);

    dwValue = (_wcsicmp_impl(driverType, L"FILE_SYSTEM") == 0) ? 2 : 1;
    RtlInitUnicodeString(&usValueName, L"Type");
    NtSetValueKey(hKey, &usValueName, 0, REG_DWORD, &dwValue, sizeof(DWORD));

    if (_wcsicmp_impl(startType, L"BOOT") == 0) dwValue = 0;
    else if (_wcsicmp_impl(startType, L"SYSTEM") == 0) dwValue = 1;
    else if (_wcsicmp_impl(startType, L"AUTO") == 0) dwValue = 2;
    else if (_wcsicmp_impl(startType, L"DISABLED") == 0) dwValue = 4;
    else dwValue = 3;

    RtlInitUnicodeString(&usValueName, L"Start");
    NtSetValueKey(hKey, &usValueName, 0, REG_DWORD, &dwValue, sizeof(DWORD));

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

    wcscpy(fullServicePath, L"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\");
    wcscat(fullServicePath, serviceName);
    RtlInitUnicodeString(&usServiceName, fullServicePath);
    return NtLoadDriver(&usServiceName);
}

NTSTATUS UnloadDriver(PCWSTR serviceName) {
    WCHAR fullServicePath[MAX_PATH_LEN];
    UNICODE_STRING usServiceName;
    wcscpy(fullServicePath, L"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\");
    wcscat(fullServicePath, serviceName);
    RtlInitUnicodeString(&usServiceName, fullServicePath);
    return NtUnloadDriver(&usServiceName);
}