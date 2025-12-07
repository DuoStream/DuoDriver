#ifndef SYSTEM_UTILS_H
#define SYSTEM_UTILS_H

#include "BootBypass.h"

#if DEBUG_LOGGING_ENABLED
    #define DEBUG_LOG(msg) DisplayMessage(msg)
    #define DEBUG_STATUS(status) DisplayStatus(status)
#else
    #define DEBUG_LOG(msg)
    #define DEBUG_STATUS(status)
#endif

void* memset_impl(void* dest, int c, SIZE_T count);
SIZE_T wcslen(const WCHAR* str);
WCHAR* wcscpy(WCHAR* dest, const WCHAR* src);
WCHAR* wcscat(WCHAR* dest, const WCHAR* src);
int _wcsicmp_impl(const WCHAR* str1, const WCHAR* str2);
void TrimString(PWSTR str);
BOOLEAN StringToULONGLONG(PCWSTR str, ULONGLONG* out);
BOOLEAN StringToULONG(PCWSTR str, PULONG out);
void ULONGLONGToHexString(ULONGLONG value, PWSTR buffer, BOOLEAN includePrefix);
void DisplayMessage(PCWSTR message);
void DisplayStatus(NTSTATUS status);
BOOLEAN ReadIniFile(PCWSTR filePath, PWSTR* outBuffer);
ULONG ParseIniFile(PWSTR iniContent, PINI_ENTRY entries, ULONG maxEntries, PCONFIG_SETTINGS config);

#endif