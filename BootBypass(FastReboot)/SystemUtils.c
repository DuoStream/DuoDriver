#include "SystemUtils.h"

// Required by some compilers for large stack allocations
void __chkstk(void) {}

void* memset_impl(void* dest, int c, SIZE_T count) {
    unsigned char* d = (unsigned char*)dest;
    while (count--) *d++ = (unsigned char)c;
    return dest;
}

SIZE_T wcslen(const WCHAR* str) {
    const WCHAR* s = str;
    while (*s) s++;
    return s - str;
}

WCHAR* wcscpy(WCHAR* dest, const WCHAR* src) {
    WCHAR* d = dest;
    while ((*d++ = *src++) != 0);
    return dest;
}

WCHAR* wcscat(WCHAR* dest, const WCHAR* src) {
    WCHAR* d = dest + wcslen(dest);
    while ((*d++ = *src++) != 0);
    return dest;
}

int _wcsicmp_impl(const WCHAR* str1, const WCHAR* str2) {
    while (*str1 && *str2) {
        WCHAR c1 = *str1, c2 = *str2;
        if (c1 >= L'a' && c1 <= L'z') c1 -= 32;
        if (c2 >= L'a' && c2 <= L'z') c2 -= 32;
        if (c1 != c2) return (c1 < c2) ? -1 : 1;
        str1++; str2++;
    }
    if (*str1) return 1;
    if (*str2) return -1;
    return 0;
}

// Bounded string length - returns length up to maxLen, never reads beyond
SIZE_T wcsnlen_safe(const WCHAR* str, SIZE_T maxLen) {
    if (!str) return 0;
    
    SIZE_T len = 0;
    while (len < maxLen && str[len] != 0) {
        len++;
    }
    return len;
}

// Safe string copy with size limit
// Returns: length of src (what would be copied if buffer was infinite)
// Result is always null-terminated if destSize > 0
SIZE_T wcscpy_safe(WCHAR* dest, SIZE_T destSize, const WCHAR* src) {
    if (!dest || destSize == 0) {
        return src ? wcslen(src) : 0;
    }
    
    if (!src) {
        dest[0] = 0;
        return 0;
    }
    
    SIZE_T srcLen = wcslen(src);
    SIZE_T copyLen = (srcLen < destSize - 1) ? srcLen : (destSize - 1);
    
    SIZE_T i;
    for (i = 0; i < copyLen; i++) {
        dest[i] = src[i];
    }
    dest[i] = 0;
    
    return srcLen; // Return full source length (may be > copyLen if truncated)
}

// Safe string concatenate with size limit
// Returns: length of dest+src combined (what would be the result if buffer was infinite)
// Result is always null-terminated if destSize > 0
SIZE_T wcscat_safe(WCHAR* dest, SIZE_T destSize, const WCHAR* src) {
    if (!dest || destSize == 0) {
        return src ? wcslen(src) : 0;
    }
    
    if (!src) {
        return wcsnlen_safe(dest, destSize);
    }
    
    // Use bounded length check in case dest is not properly terminated
    SIZE_T destLen = wcsnlen_safe(dest, destSize);
    SIZE_T srcLen = wcslen(src);
    
    // If dest already fills buffer, cannot append
    if (destLen >= destSize - 1) {
        return destLen + srcLen;
    }
    
    SIZE_T remaining = destSize - destLen - 1;
    SIZE_T copyLen = (srcLen < remaining) ? srcLen : remaining;
    
    SIZE_T i;
    for (i = 0; i < copyLen; i++) {
        dest[destLen + i] = src[i];
    }
    dest[destLen + i] = 0;
    
    return destLen + srcLen; // Return total length that would result
}

// Check if concatenation would fit without truncation
BOOLEAN wcscat_check(WCHAR* dest, SIZE_T destSize, const WCHAR* src) {
    if (!dest || !src || destSize == 0) return FALSE;
    
    SIZE_T destLen = wcsnlen_safe(dest, destSize);
    SIZE_T srcLen = wcslen(src);
    
    // Check overflow protection: destLen + srcLen + 1 <= destSize
    if (destLen >= destSize) return FALSE;
    if (srcLen > (destSize - destLen - 1)) return FALSE;
    
    return TRUE;
}

// Validate if adding addLen to currentLen would exceed maxLen
// Protected against arithmetic overflow
BOOLEAN validate_string_space(SIZE_T currentLen, SIZE_T addLen, SIZE_T maxLen) {
    if (currentLen >= maxLen) return FALSE;
    if (addLen > (maxLen - currentLen - 1)) return FALSE;
    return TRUE;
}

void TrimString(PWSTR str) {
    PWSTR start = str, end;
    while (*start == L' ' || *start == L'\t' || *start == L'\r' || *start == L'\n') start++;
    if (*start == 0) { *str = 0; return; }
    
    PWSTR semicolon = start;
    while (*semicolon && *semicolon != L';') semicolon++;
    if (*semicolon == L';') *semicolon = 0;
    
    end = start + wcslen(start) - 1;
    while (end > start && (*end == L' ' || *end == L'\t' || *end == L'\r' || *end == L'\n')) end--;
    *(end + 1) = 0;
    if (start != str) wcscpy(str, start);
}

BOOLEAN StringToULONGLONG(PCWSTR str, ULONGLONG* out) {
    ULONGLONG result = 0;
    PCWSTR p = str;
    if (p[0] == L'0' && (p[1] == L'x' || p[1] == L'X')) {
        p += 2;
        while (*p) {
            WCHAR c = *p;
            ULONGLONG digit;
            if (c >= L'0' && c <= L'9') digit = c - L'0';
            else if (c >= L'a' && c <= L'f') digit = c - L'a' + 10;
            else if (c >= L'A' && c <= L'F') digit = c - L'A' + 10;
            else return FALSE;
            result = (result << 4) | digit;
            p++;
        }
    } else {
        while (*p) {
            if (*p < L'0' || *p > L'9') return FALSE;
            result = result * 10 + (*p - L'0');
            p++;
        }
    }
    *out = result;
    return TRUE;
}

BOOLEAN StringToULONG(PCWSTR str, PULONG out) {
    ULONGLONG result;
    if (!StringToULONGLONG(str, &result) || result > 0xFFFFFFFF) return FALSE;
    *out = (ULONG)result;
    return TRUE;
}

void ULONGLONGToHexString(ULONGLONG value, PWSTR buffer, BOOLEAN includePrefix) {
    const WCHAR hexChars[] = L"0123456789ABCDEF";
    int i, offset = 0;
    if (includePrefix) { buffer[0] = L'0'; buffer[1] = L'x'; offset = 2; }
    for (i = 0; i < 16; i++) {
        int nibble = (value >> (60 - i * 4)) & 0xF;
        buffer[offset + i] = hexChars[nibble];
    }
    buffer[offset + 16] = 0;
}

void DisplayMessage(PCWSTR message) {
    if (!message) return;
    WCHAR tempBuffer[512];
    SIZE_T len = wcslen(message);
    if (len >= 512) len = 511;
    wcscpy(tempBuffer, message);
    tempBuffer[len] = L'\0';
    UNICODE_STRING usMsg;
    RtlInitUnicodeString(&usMsg, tempBuffer);
    NtDisplayString(&usMsg);
}

void DisplayStatus(NTSTATUS status) {
    WCHAR statusMsg[20];
    WCHAR hexChars[] = L"0123456789ABCDEF";
    statusMsg[0] = L' '; statusMsg[1] = L'('; statusMsg[2] = L'0'; statusMsg[3] = L'x';
    for (int i = 0; i < 8; i++) {
        int nibble = (status >> (28 - i * 4)) & 0xF;
        statusMsg[4 + i] = hexChars[nibble];
    }
    statusMsg[12] = L')'; statusMsg[13] = L'\r'; statusMsg[14] = L'\n'; statusMsg[15] = 0;
    DisplayMessage(statusMsg);
}

BOOLEAN ReadIniFile(PCWSTR filePath, PWSTR* outBuffer) {
    UNICODE_STRING usFilePath;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    HANDLE hFile;
    NTSTATUS status;
    static WCHAR staticBuffer[8192];
    LARGE_INTEGER byteOffset;

    RtlInitUnicodeString(&usFilePath, filePath);
    InitializeObjectAttributes(&oa, &usFilePath, OBJ_CASE_INSENSITIVE, NULL, NULL);

    status = NtOpenFile(&hFile, FILE_READ_DATA | SYNCHRONIZE, &oa, &iosb, FILE_SHARE_READ | FILE_SHARE_WRITE, 0);
    if (!NT_SUCCESS(status)) return FALSE;

    memset_impl(staticBuffer, 0, sizeof(staticBuffer));
    byteOffset.QuadPart = 0;
    status = NtReadFile(hFile, NULL, NULL, NULL, &iosb, staticBuffer, sizeof(staticBuffer) - sizeof(WCHAR), &byteOffset, NULL);
    NtClose(hFile);
    if (!NT_SUCCESS(status) && status != 0x103) return FALSE;
    *outBuffer = staticBuffer;
    return TRUE;
}

ULONG ParseIniFile(PWSTR iniContent, PINI_ENTRY entries, ULONG maxEntries, PCONFIG_SETTINGS config) {
    ULONG entryCount = 0;
    PWSTR line = iniContent, nextLine;
    WCHAR lineBuf[MAX_PATH_LEN];
    ULONG i;
    int currentEntry = -1;
    BOOLEAN inConfigSection = FALSE;

    // Defaults
    config->Execute = TRUE;
    config->RestoreHVCI = TRUE;
    config->DriverDevice[0] = 0;
    config->IoControlCode_Read = 0;
    config->IoControlCode_Write = 0;
    
    if (!iniContent || iniContent[0] == 0) return 0;
    if (iniContent[0] == 0xFEFF) line++;

    while (*line && entryCount < maxEntries) {
        nextLine = line;
        while (*nextLine && *nextLine != L'\r' && *nextLine != L'\n') nextLine++;
        
        i = 0;
        while (line < nextLine && i < (MAX_PATH_LEN - 1)) lineBuf[i++] = *line++;
        lineBuf[i] = 0;
        line = nextLine;
        if (*line == L'\r') line++;
        if (*line == L'\n') line++;
        
        TrimString(lineBuf);
        if (lineBuf[0] == 0 || lineBuf[0] == L';' || lineBuf[0] == L'#') continue;

        if (lineBuf[0] == L'[') {
            if (_wcsicmp_impl(lineBuf, L"[Config]") == 0) {
                inConfigSection = TRUE;
                currentEntry = -1;
                continue;
            }
            if (_wcsicmp_impl(lineBuf, L"[DSE_STATE]") == 0) {
                inConfigSection = FALSE;
                currentEntry = -1;
                continue;
            }
            inConfigSection = FALSE;
            if (currentEntry >= 0) {
                if (entries[currentEntry].DisplayName[0] == 0 && entries[currentEntry].ServiceName[0]) {
                    wcscpy_safe(entries[currentEntry].DisplayName, MAX_PATH_LEN, entries[currentEntry].ServiceName);
                }
                entryCount++;
            }
            if (entryCount < maxEntries) {
                currentEntry = (LONG)entryCount;
                memset_impl(&entries[currentEntry], 0, sizeof(INI_ENTRY));
                wcscpy_safe(entries[currentEntry].DriverType, 16, L"KERNEL");
                wcscpy_safe(entries[currentEntry].StartType, 16, L"DEMAND");
            } else currentEntry = -1;
            continue;
        }

        if (inConfigSection && lineBuf[0] != 0) {
            PWSTR equals = lineBuf;
            while (*equals && *equals != L'=') equals++;
            if (*equals == L'=') {
                *equals = 0;
                PWSTR key = lineBuf, value = equals + 1;
                TrimString(key); TrimString(value);
                if (_wcsicmp_impl(key, L"Execute") == 0) config->Execute = (_wcsicmp_impl(value, L"YES") == 0 || _wcsicmp_impl(value, L"1") == 0);
                else if (_wcsicmp_impl(key, L"RestoreHVCI") == 0) config->RestoreHVCI = (_wcsicmp_impl(value, L"YES") == 0 || _wcsicmp_impl(value, L"1") == 0);
                else if (_wcsicmp_impl(key, L"DriverDevice") == 0) wcscpy_safe(config->DriverDevice, MAX_PATH_LEN, value);
                else if (_wcsicmp_impl(key, L"IoControlCode_Read") == 0) StringToULONG(value, &config->IoControlCode_Read);
                else if (_wcsicmp_impl(key, L"IoControlCode_Write") == 0) StringToULONG(value, &config->IoControlCode_Write);
                else if (_wcsicmp_impl(key, L"Offset_SeCiCallbacks") == 0) StringToULONGLONG(value, &config->Offset_SeCiCallbacks);
                else if (_wcsicmp_impl(key, L"Offset_Callback") == 0) StringToULONGLONG(value, &config->Offset_Callback);
                else if (_wcsicmp_impl(key, L"Offset_SafeFunction") == 0) StringToULONGLONG(value, &config->Offset_SafeFunction);
            }
            continue;
        }

        if (currentEntry >= 0 && (ULONG)currentEntry < maxEntries) {
            PWSTR equals = lineBuf;
            while (*equals && *equals != L'=') equals++;
            if (*equals == L'=') {
                *equals = 0;
                PWSTR key = lineBuf, value = equals + 1;
                TrimString(key); TrimString(value);
                if (_wcsicmp_impl(key, L"Action") == 0) {
                    if (_wcsicmp_impl(value, L"LOAD") == 0) entries[currentEntry].Action = ACTION_LOAD;
                    else if (_wcsicmp_impl(value, L"UNLOAD") == 0) entries[currentEntry].Action = ACTION_UNLOAD;
                    else if (_wcsicmp_impl(value, L"RENAME") == 0) entries[currentEntry].Action = ACTION_RENAME;
                    else if (_wcsicmp_impl(value, L"DELETE") == 0) entries[currentEntry].Action = ACTION_DELETE;
                }
                else if (_wcsicmp_impl(key, L"ServiceName") == 0) wcscpy_safe(entries[currentEntry].ServiceName, MAX_PATH_LEN, value);
                else if (_wcsicmp_impl(key, L"DisplayName") == 0) wcscpy_safe(entries[currentEntry].DisplayName, MAX_PATH_LEN, value);
                else if (_wcsicmp_impl(key, L"ImagePath") == 0) wcscpy_safe(entries[currentEntry].ImagePath, MAX_PATH_LEN, value);
                else if (_wcsicmp_impl(key, L"Type") == 0) wcscpy_safe(entries[currentEntry].DriverType, 16, value);
                else if (_wcsicmp_impl(key, L"StartType") == 0) wcscpy_safe(entries[currentEntry].StartType, 16, value);
                else if (_wcsicmp_impl(key, L"CheckIfLoaded") == 0) entries[currentEntry].CheckIfLoaded = (_wcsicmp_impl(value, L"YES") == 0);
                else if (_wcsicmp_impl(key, L"AutoPatch") == 0) entries[currentEntry].AutoPatch = (_wcsicmp_impl(value, L"YES") == 0 || _wcsicmp_impl(value, L"1") == 0);
                else if (_wcsicmp_impl(key, L"SourcePath") == 0) wcscpy_safe(entries[currentEntry].SourcePath, MAX_PATH_LEN, value);
                else if (_wcsicmp_impl(key, L"TargetPath") == 0) wcscpy_safe(entries[currentEntry].TargetPath, MAX_PATH_LEN, value);
                else if (_wcsicmp_impl(key, L"ReplaceIfExists") == 0) entries[currentEntry].ReplaceIfExists = (_wcsicmp_impl(value, L"YES") == 0);
                else if (_wcsicmp_impl(key, L"DeletePath") == 0) wcscpy_safe(entries[currentEntry].DeletePath, MAX_PATH_LEN, value);
                else if (_wcsicmp_impl(key, L"RecursiveDelete") == 0) entries[currentEntry].RecursiveDelete = (_wcsicmp_impl(value, L"YES") == 0);
            }
        }
    }
    if (currentEntry >= 0) {
        if (entries[currentEntry].DisplayName[0] == 0 && entries[currentEntry].ServiceName[0]) {
            wcscpy_safe(entries[currentEntry].DisplayName, MAX_PATH_LEN, entries[currentEntry].ServiceName);
        }
        entryCount++;
    }
    return entryCount;
}