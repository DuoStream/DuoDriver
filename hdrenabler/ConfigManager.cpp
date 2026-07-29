#include "ConfigManager.h"
#include <iostream>
#include <conio.h>
#include <limits>

namespace ConfigManager {

std::wstring GetDriverPath() {
    WCHAR systemRoot[MAX_PATH];
    DWORD len = GetEnvironmentVariableW(L"SystemRoot", systemRoot, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        GetWindowsDirectoryW(systemRoot, MAX_PATH);
    }
    return std::wstring(systemRoot) + L"\\System32\\drivers\\RTCore64.sys";
}

std::wstring GetDuoDriverPath() {
    WCHAR systemRoot[MAX_PATH];
    DWORD len = GetEnvironmentVariableW(L"SystemRoot", systemRoot, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        GetWindowsDirectoryW(systemRoot, MAX_PATH);
    }
    return std::wstring(systemRoot) + L"\\System32\\drivers\\DuoDriver.sys";
}

bool CheckAndDisableMemoryIntegrity() {
    HKEY hKey;
    LSTATUS result = RegOpenKeyExW(HKEY_LOCAL_MACHINE, 
        L"SYSTEM\\CurrentControlSet\\Control\\DeviceGuard\\Scenarios\\HypervisorEnforcedCodeIntegrity",
        0, KEY_READ | KEY_WRITE, &hKey);

    if (result != ERROR_SUCCESS) {
        return true;
    }

    DWORD enabled = 0;
    DWORD dataSize = sizeof(DWORD);
    result = RegQueryValueExW(hKey, L"Enabled", nullptr, nullptr, (LPBYTE)&enabled, &dataSize);

    if (result == ERROR_SUCCESS && enabled == 1) {
        std::wcout << L"\n[!] WARNING: Memory Integrity (Hypervisor Enforced Code Integrity) is enabled!\n";
        std::wcout << L"[!] DSE patching will succeed, but loading unsigned drivers will cause BSOD.\n";
        std::wcout << L"[!] Do you want to disable Memory Integrity and reboot? (Y/N): ";

        wchar_t choice;
        std::wcin >> choice;
        std::wcin.clear();
        std::wcin.ignore((std::numeric_limits<std::streamsize>::max)(), L'\n');

        if (choice == L'Y' || choice == L'y') {
            enabled = 0;
            RegSetValueExW(hKey, L"Enabled", 0, REG_DWORD, (const BYTE*)&enabled, sizeof(enabled));
            RegDeleteValueW(hKey, L"WasEnabledBy");

            std::wcout << L"[+] Memory Integrity disabled. System will reboot to apply changes.\n";
            std::wcout << L"[+] Press any key to continue with reboot...";
            std::wcout.flush();
            FlushConsoleInputBuffer(GetStdHandle(STD_INPUT_HANDLE));
            _getwch();
            std::wcout << L"\n";

            HANDLE hToken;
            TOKEN_PRIVILEGES tkp;

            if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken)) {
                LookupPrivilegeValueW(nullptr, SE_SHUTDOWN_NAME, &tkp.Privileges[0].Luid);
                tkp.PrivilegeCount = 1;
                tkp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

                AdjustTokenPrivileges(hToken, FALSE, &tkp, 0, nullptr, 0);
                CloseHandle(hToken);
            }

            WCHAR rebootMsg[] = L"Memory Integrity disabled for DSE bypass";
            InitiateSystemShutdownExW(nullptr, rebootMsg, 0, TRUE, TRUE, SHTDN_REASON_MAJOR_OTHER | SHTDN_REASON_MINOR_OTHER);
            ExitProcess(0);
        } else {
            std::wcout << L"[!] Memory Integrity remains enabled. DSE bypass may cause BSOD on driver load.\n";
            std::wcout << L"[!] Continuing at your own risk...\n\n";
        }
    }

    RegCloseKey(hKey);
    return true;
}

bool SaveOriginalFunctionToRegistry(uint64_t functionBytes) {
    HKEY hKey;

    LSTATUS result = RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\hdrenabler\\PatchState", 0, nullptr,
        REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hKey, nullptr);

    if (result != ERROR_SUCCESS) {
        std::wcout << L"[-] Failed to create registry key (error: " << result << L")\n";
        return false;
    }

    RegSetValueExW(hKey, L"OriginalFunction", 0, REG_QWORD, (BYTE*)&functionBytes, sizeof(uint64_t));

    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t timestampStr[64];
    swprintf_s(timestampStr, L"%04d-%02d-%02d %02d:%02d:%02d",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    RegSetValueExW(hKey, L"PatchTimestamp", 0, REG_SZ, (BYTE*)timestampStr,
        (DWORD)((wcslen(timestampStr) + 1) * sizeof(wchar_t)));

    RegCloseKey(hKey);

    std::wcout << L"[+] Saved original function to registry: 0x" << std::hex << std::uppercase
               << functionBytes << std::dec << L"\n";

    return true;
}

std::optional<uint64_t> LoadOriginalFunctionFromRegistry() {
    HKEY hKey;

    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\hdrenabler\\PatchState", 0,
        KEY_READ, &hKey) != ERROR_SUCCESS) {
        return std::nullopt;
    }

    uint64_t functionBytes = 0;
    DWORD dataSize = sizeof(uint64_t);

    if (RegQueryValueExW(hKey, L"OriginalFunction", nullptr, nullptr,
        (BYTE*)&functionBytes, &dataSize) != ERROR_SUCCESS) {
        RegCloseKey(hKey);
        return std::nullopt;
    }

    RegCloseKey(hKey);

    std::wcout << L"[+] Loaded original function from registry: 0x" << std::hex << std::uppercase
               << functionBytes << std::dec << L"\n";

    return functionBytes;
}

bool SavePatchState(bool isPatched) {
    HKEY hKey;

    LSTATUS result = RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\hdrenabler\\PatchState", 0, nullptr,
        REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hKey, nullptr);

    if (result != ERROR_SUCCESS) {
        return false;
    }

    DWORD value = isPatched ? 1 : 0;
    RegSetValueExW(hKey, L"IsPatched", 0, REG_DWORD, (BYTE*)&value, sizeof(DWORD));

    RegCloseKey(hKey);
    return true;
}

bool IsCurrentlyPatched() {
    HKEY hKey;

    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\hdrenabler\\PatchState", 0,
        KEY_READ, &hKey) != ERROR_SUCCESS) {
        return false;
    }

    DWORD value = 0;
    DWORD dataSize = sizeof(DWORD);

    if (RegQueryValueExW(hKey, L"IsPatched", nullptr, nullptr,
        (BYTE*)&value, &dataSize) != ERROR_SUCCESS) {
        RegCloseKey(hKey);
        return false;
    }

    RegCloseKey(hKey);
    return (value == 1);
}

bool ClearPatchState() {
    HKEY hKey;

    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\hdrenabler\\PatchState", 0,
        KEY_WRITE, &hKey) != ERROR_SUCCESS) {
        return false;
    }

    RegDeleteValueW(hKey, L"OriginalFunction");
    RegDeleteValueW(hKey, L"PatchTimestamp");

    DWORD value = 0;
    RegSetValueExW(hKey, L"IsPatched", 0, REG_DWORD, (BYTE*)&value, sizeof(DWORD));

    RegCloseKey(hKey);

    std::wcout << L"[+] Cleared patch state from registry\n";
    return true;
}

bool SaveDseStateToRegistry(uint64_t originalCallback) {
    HKEY hKey;

    LSTATUS result = RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\hdrenabler\\DseState", 0, nullptr,
        REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hKey, nullptr);

    if (result != ERROR_SUCCESS) {
        std::wcout << L"[-] Failed to create DSE state registry key (error: " << result << L")\n";
        return false;
    }

    RegSetValueExW(hKey, L"OriginalCallback", 0, REG_QWORD, (BYTE*)&originalCallback, sizeof(uint64_t));

    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t timestampStr[64];
    swprintf_s(timestampStr, L"%04d-%02d-%02d %02d:%02d:%02d",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    RegSetValueExW(hKey, L"PatchTimestamp", 0, REG_SZ, (BYTE*)timestampStr,
        (DWORD)((wcslen(timestampStr) + 1) * sizeof(wchar_t)));

    DWORD isPatched = 1;
    RegSetValueExW(hKey, L"IsPatched", 0, REG_DWORD, (BYTE*)&isPatched, sizeof(DWORD));

    RegCloseKey(hKey);

    std::wcout << L"[+] Saved DSE state to registry: 0x" << std::hex << std::uppercase
               << originalCallback << std::dec << L"\n";

    return true;
}

std::optional<uint64_t> LoadDseStateFromRegistry() {
    HKEY hKey;

    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\hdrenabler\\DseState", 0,
        KEY_READ, &hKey) != ERROR_SUCCESS) {
        return std::nullopt;
    }

    uint64_t callback = 0;
    DWORD dataSize = sizeof(uint64_t);

    if (RegQueryValueExW(hKey, L"OriginalCallback", nullptr, nullptr,
        (BYTE*)&callback, &dataSize) != ERROR_SUCCESS) {
        RegCloseKey(hKey);
        return std::nullopt;
    }

    RegCloseKey(hKey);

    std::wcout << L"[+] Loaded DSE original callback: 0x" << std::hex << std::uppercase
               << callback << std::dec << L"\n";

    return callback;
}

bool ClearDseStateFromRegistry() {
    HKEY hKey;

    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\hdrenabler\\DseState", 0,
        KEY_WRITE, &hKey) != ERROR_SUCCESS) {
        return false;
    }

    RegDeleteValueW(hKey, L"OriginalCallback");
    RegDeleteValueW(hKey, L"PatchTimestamp");

    DWORD value = 0;
    RegSetValueExW(hKey, L"IsPatched", 0, REG_DWORD, (BYTE*)&value, sizeof(DWORD));

    RegCloseKey(hKey);

    std::wcout << L"[+] Cleared DSE state from registry\n";
    return true;
}

} // namespace ConfigManager
