#include "DrvCore.h"
#include "ConfigManager.h"
#include "ResourceInstaller.h"
#include <iostream>
#include <vector>
#include <psapi.h>

#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "version.lib")

bool DrvLoader::Initialize() {
    originalCallback = ConfigManager::LoadOriginalCallbackFromRegistry();
    
    if (originalCallback) {
        std::wcout << L"[+] Found previous patch state in registry\n";
    } else {
        std::wcout << L"[*] No previous patch state found\n";
    }
    
    if (!symbolDownloader.Initialize()) {
        std::wcout << L"[-] Failed to initialize symbol downloader\n";
        return false;
    }
    
    return true;
}

void DrvLoader::Cleanup() {
    if (hDriver != INVALID_HANDLE_VALUE) {
        CloseHandle(hDriver);
        hDriver = INVALID_HANDLE_VALUE;
    }
}

bool DrvLoader::WriteMemory32(uint64_t address, uint32_t value) {
    if (hDriver == INVALID_HANDLE_VALUE) return false;
    
    RTC_MEMORY_WRITE writePacket{};
    writePacket.Address = address;
    writePacket.Size = sizeof(uint32_t);
    writePacket.Value = value;
    
    DWORD bytesReturned = 0;
    return DeviceIoControl(hDriver, RTC_IOCTL_MEMORY_WRITE, &writePacket, sizeof(writePacket), 
                          &writePacket, sizeof(writePacket), &bytesReturned, nullptr);
}

bool DrvLoader::WriteMemory64(uint64_t address, uint64_t value) {
    return WriteMemory32(address, static_cast<uint32_t>(value & 0xFFFFFFFF)) && 
           WriteMemory32(address + 4, static_cast<uint32_t>((value >> 32) & 0xFFFFFFFF));
}

std::optional<uint32_t> DrvLoader::ReadMemory32(uint64_t address) {
    if (hDriver == INVALID_HANDLE_VALUE) return std::nullopt;
    
    RTC_MEMORY_READ readPacket{};
    readPacket.Address = address;
    readPacket.Size = sizeof(uint32_t);
    
    DWORD bytesReturned = 0;
    if (!DeviceIoControl(hDriver, RTC_IOCTL_MEMORY_READ, &readPacket, sizeof(readPacket), 
                        &readPacket, sizeof(readPacket), &bytesReturned, nullptr))
        return std::nullopt;
    
    return readPacket.Value;
}

std::optional<uint64_t> DrvLoader::ReadMemory64(uint64_t address) {
    auto low = ReadMemory32(address);
    auto high = ReadMemory32(address + 4);
    if (!low || !high) return std::nullopt;
    
    return (static_cast<uint64_t>(*high) << 32) | *low;
}

std::optional<uint64_t> DrvLoader::GetNtoskrnlBase() {
    std::vector<LPVOID> drivers(1024);
    DWORD needed = 0;
    
    if (!EnumDeviceDrivers(drivers.data(), static_cast<DWORD>(drivers.size() * sizeof(LPVOID)), &needed))
        return std::nullopt;
    
    drivers.resize(needed / sizeof(LPVOID));
    
    for (const auto& driver : drivers) {
        WCHAR driverName[MAX_PATH];
        if (GetDeviceDriverBaseNameW(driver, driverName, MAX_PATH) && wcscmp(driverName, L"ntoskrnl.exe") == 0) {
            return reinterpret_cast<uint64_t>(driver);
        }
    }
    
    return std::nullopt;
}

std::optional<std::pair<uint64_t, uint64_t>> DrvLoader::GetTextSectionBounds(const std::wstring& ntoskrnlPath) {
    HANDLE hFile = CreateFileW(ntoskrnlPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        return std::nullopt;
    }
    
    HANDLE hMapping = CreateFileMappingW(hFile, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!hMapping) {
        CloseHandle(hFile);
        return std::nullopt;
    }
    
    LPVOID pBase = MapViewOfFile(hMapping, FILE_MAP_READ, 0, 0, 0);
    if (!pBase) {
        CloseHandle(hMapping);
        CloseHandle(hFile);
        return std::nullopt;
    }
    
    std::optional<std::pair<uint64_t, uint64_t>> result;
    
    PIMAGE_DOS_HEADER pDos = (PIMAGE_DOS_HEADER)pBase;
    if (pDos->e_magic == IMAGE_DOS_SIGNATURE) {
        PIMAGE_NT_HEADERS pNt = (PIMAGE_NT_HEADERS)((BYTE*)pBase + pDos->e_lfanew);
        if (pNt->Signature == IMAGE_NT_SIGNATURE) {
            PIMAGE_SECTION_HEADER pSection = IMAGE_FIRST_SECTION(pNt);
            
            for (WORD i = 0; i < pNt->FileHeader.NumberOfSections; i++) {
                char sectionName[9] = {0};
                memcpy(sectionName, pSection[i].Name, 8);
                
                if (strcmp(sectionName, ".text") == 0) {
                    uint64_t textStart = pSection[i].VirtualAddress;
                    uint64_t textEnd = textStart + pSection[i].Misc.VirtualSize;
                    result = {textStart, textEnd};
                    break;
                }
            }
        }
    }
    
    UnmapViewOfFile(pBase);
    CloseHandle(hMapping);
    CloseHandle(hFile);
    
    return result;
}

bool DrvLoader::ValidateKernelAddresses(uint64_t ntBase, uint64_t seCiRva, uint64_t zwRva) {
    WCHAR systemRoot[MAX_PATH];
    GetSystemDirectoryW(systemRoot, MAX_PATH);
    std::wstring ntoskrnlPath = std::wstring(systemRoot) + L"\\ntoskrnl.exe";
    
    // Get .text section bounds
    auto textBounds = GetTextSectionBounds(ntoskrnlPath);
    if (!textBounds) {
        std::wcout << L"[-] Failed to read .text section from ntoskrnl.exe\n";
        return false;
    }
    
    auto [textStart, textEnd] = *textBounds;
    
    // Validate ZwFlushInstructionCache is in .text section
    if (zwRva < textStart || zwRva >= textEnd) {
        std::wcout << L"[-] ZwFlushInstructionCache offset 0x" << std::hex << zwRva 
                   << L" is outside .text section [0x" << textStart << L"-0x" << textEnd << L"]\n" << std::dec;
        return false;
    }
    
    std::wcout << L"[+] ZwFlushInstructionCache validated in .text section\n";
    
    // Validate SeCiCallbacks address is readable
    uint64_t seCiCallbacks = ntBase + seCiRva;
    uint64_t callbackAddress = seCiCallbacks + 0x20;
    
    auto testRead = ReadMemory64(callbackAddress);
    if (!testRead) {
        std::wcout << L"[-] Cannot read SeCiCallbacks+0x20 at 0x" << std::hex << callbackAddress << std::dec << L"\n";
        std::wcout << L"[-] Address may be invalid or driver primitives not working\n";
        return false;
    }
    
    std::wcout << L"[+] SeCiCallbacks address validated (readable)\n";
    
    return true;
}

std::optional<std::pair<uint64_t, uint64_t>> DrvLoader::ResolveKernelOffsetsStrict() {
    WCHAR systemRoot[MAX_PATH];
    GetSystemDirectoryW(systemRoot, MAX_PATH);
    std::wstring ntoskrnlPath = std::wstring(systemRoot) + L"\\ntoskrnl.exe";
    
    std::wcout << L"[*] Strict offset resolution from PDB...\n";
    
    // Get PDB GUID from current ntoskrnl.exe
    auto [pdbName, pdbGuid] = symbolDownloader.GetPdbInfoFromPe(ntoskrnlPath);
    if (pdbGuid.empty()) {
        std::wcout << L"[-] Failed to extract PDB GUID from ntoskrnl.exe\n";
        return std::nullopt;
    }
    
    std::wcout << L"[+] Current kernel PDB GUID: " << pdbGuid << L"\n";
    
    // Ensure symbols exist in ProgramData store (download if needed)
    if (!symbolDownloader.DownloadSymbolsForModule(ntoskrnlPath)) {
        std::wcout << L"[-] Failed to obtain PDB symbols\n";
        return std::nullopt;
    }
    
    // Resolve symbols from PDB
    auto seCiOpt = symbolDownloader.GetSymbolOffset(ntoskrnlPath, L"SeCiCallbacks");
    auto zwOpt = symbolDownloader.GetSymbolOffset(ntoskrnlPath, L"ZwFlushInstructionCache");
    
    if (!seCiOpt || !zwOpt) {
        std::wcout << L"[-] Failed to resolve required symbols from PDB\n";
        return std::nullopt;
    }
    
    std::wcout << L"[+] Resolved offsets from PDB:\n";
    std::wcout << L"    SeCiCallbacks: 0x" << std::hex << *seCiOpt << std::dec << L"\n";
    std::wcout << L"    ZwFlushInstructionCache: 0x" << std::hex << *zwOpt << std::dec << L"\n";
    
    return std::make_pair(*seCiOpt, *zwOpt);
}

bool DrvLoader::GetSymbolOffsets(uint64_t* seCiCallbacks, uint64_t* safeFunction) {
    auto offsets = ResolveKernelOffsetsStrict();
    if (!offsets) {
        return false;
    }
    
    *seCiCallbacks = offsets->first;
    *safeFunction = offsets->second;
    
    return true;
}

std::optional<uint64_t> DrvLoader::GetKernelSymbolOffset(const std::wstring& symbolName) {
    WCHAR systemRoot[MAX_PATH];
    GetSystemDirectoryW(systemRoot, MAX_PATH);
    std::wstring ntoskrnlPath = std::wstring(systemRoot) + L"\\ntoskrnl.exe";
    
    if (!symbolDownloader.DownloadSymbolsForModule(ntoskrnlPath)) {
        return std::nullopt;
    }
    
    return symbolDownloader.GetSymbolOffset(ntoskrnlPath, symbolName);
}

bool DrvLoader::CheckDriverFileExists() {
    std::wstring driverPath = ConfigManager::GetDriverPath();
    if (GetFileAttributesW(driverPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::wcout << L"[*] Installing driver from embedded resource...\n";
        if (!ResourceInstaller::InstallDriverFromResource()) return false;
        
        if (GetFileAttributesW(driverPath.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
    }
    return true;
}

bool DrvLoader::StopAndRemoveDriver() {
    SC_HANDLE hSCM = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (!hSCM) return false;
    
    SC_HANDLE hService = OpenServiceW(hSCM, L"RTCore64", SERVICE_ALL_ACCESS);
    if (hService) {
        SERVICE_STATUS serviceStatus;
        ControlService(hService, SERVICE_CONTROL_STOP, &serviceStatus);
        DeleteService(hService);
        CloseServiceHandle(hService);
    }
    
    CloseServiceHandle(hSCM);
    return true;
}

bool DrvLoader::InstallAndStartDriver() {
    if (!CheckDriverFileExists()) return false;
    StopAndRemoveDriver();
    
    SC_HANDLE hSCM = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (!hSCM) return false;
    
    std::wstring driverPath = L"System32\\drivers\\RTCore64.sys";
    
    SC_HANDLE hService = CreateServiceW(hSCM, L"RTCore64", L"RTCore64", SERVICE_ALL_ACCESS,
        SERVICE_KERNEL_DRIVER, SERVICE_SYSTEM_START, SERVICE_ERROR_NORMAL,
        driverPath.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr);
    
    if (!hService) {
        CloseServiceHandle(hSCM);
        return false;
    }
    
    StartServiceW(hService, 0, nullptr);
    CloseServiceHandle(hService);
    CloseServiceHandle(hSCM);
    return true;
}

bool DrvLoader::CheckDSEStatus(bool& isPatched) {
    std::wcout << L"\n[=== Checking DSE Status ===]\n\n";
    
    // Strict resolution - no cache
    auto offsets = ResolveKernelOffsetsStrict();
    if (!offsets) {
        std::wcout << L"[-] Failed to resolve kernel offsets\n";
        return false;
    }
    
    auto [seCiCallbacksOffset, zwFlushOffset] = *offsets;
    
    if (!InstallAndStartDriver()) return false;
    
    hDriver = CreateFileW(L"\\\\.\\RTCore64", GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (hDriver == INVALID_HANDLE_VALUE) {
        StopAndRemoveDriver();
        return false;
    }
    
    auto ntBase = GetNtoskrnlBase();
    if (!ntBase) {
        Cleanup();
        StopAndRemoveDriver();
        return false;
    }
    
    std::wcout << L"[+] ntoskrnl.exe base: 0x" << std::hex << *ntBase << std::dec << L"\n";
    
    // Validate addresses before checking status
    if (!ValidateKernelAddresses(*ntBase, seCiCallbacksOffset, zwFlushOffset)) {
        std::wcout << L"[-] Address validation failed - offsets may be incorrect\n";
        Cleanup();
        StopAndRemoveDriver();
        return false;
    }
    
    uint64_t seCiCallbacks = *ntBase + seCiCallbacksOffset;
    uint64_t safeFunction = *ntBase + zwFlushOffset;
    uint64_t callbackAddress = seCiCallbacks + 0x20;
    
    auto currentCallback = ReadMemory64(callbackAddress);
    if (!currentCallback) {
        Cleanup();
        StopAndRemoveDriver();
        return false;
    }
    
    isPatched = (*currentCallback == safeFunction);
    
    std::wcout << L"[+] DSE Status: " << (isPatched ? L"PATCHED" : L"ACTIVE") << L"\n";
    std::wcout << L"    Current callback: 0x" << std::hex << *currentCallback << L"\n";
    std::wcout << L"    Safe function: 0x" << safeFunction << std::dec << L"\n";
    
    // Update configuration files with validated offsets
    ConfigManager::UpdateDriversIni(seCiCallbacksOffset, zwFlushOffset);
    std::wstring buildInfo = ConfigManager::GetWindowsBuildNumber();
    ConfigManager::SaveOffsetsToRegistry(seCiCallbacksOffset, zwFlushOffset, buildInfo);
    
    Cleanup();
    StopAndRemoveDriver();
    return true;
}

bool DrvLoader::BypassDSEInternal() {
    // Strict resolution - no cache
    auto offsets = ResolveKernelOffsetsStrict();
    if (!offsets) {
        std::wcout << L"[-] Failed to resolve kernel offsets\n";
        return false;
    }
    
    auto [seCiOffset, zwFlushOffset] = *offsets;
    
    auto ntBase = GetNtoskrnlBase();
    if (!ntBase) {
        std::wcout << L"[-] Failed to get ntoskrnl.exe base address\n";
        return false;
    }
    
    std::wcout << L"[+] ntoskrnl.exe base: 0x" << std::hex << *ntBase << std::dec << L"\n";
    
    // Validate addresses before patching
    if (!ValidateKernelAddresses(*ntBase, seCiOffset, zwFlushOffset)) {
        std::wcout << L"[-] Address validation failed - aborting patch\n";
        return false;
    }
    
    uint64_t seCiCallbacks = *ntBase + seCiOffset;
    uint64_t safeFunction = *ntBase + zwFlushOffset;
    uint64_t callbackToPatch = seCiCallbacks + 0x20;
    
    auto currentCallback = ReadMemory64(callbackToPatch);
    if (!currentCallback) {
        std::wcout << L"[-] Failed to read current callback\n";
        return false;
    }
    
    // Already patched?
    if (*currentCallback == safeFunction) {
        std::wcout << L"[+] DSE already bypassed\n";
        return true;
    }
    
    std::wcout << L"[*] Patching callback:\n";
    std::wcout << L"    From: 0x" << std::hex << *currentCallback << L"\n";
    std::wcout << L"    To:   0x" << safeFunction << std::dec << L"\n";
    
    // Save original callback
    originalCallback = *currentCallback;
    if (!ConfigManager::SaveOriginalCallbackToRegistry(*originalCallback)) {
        std::wcout << L"[-] Failed to save original callback to registry\n";
        return false;
    }
    
    // Perform patch
    if (!WriteMemory64(callbackToPatch, safeFunction)) {
        std::wcout << L"[-] Failed to write patched callback\n";
        return false;
    }
    
    // Verify patch
    auto verifyCallback = ReadMemory64(callbackToPatch);
    if (!verifyCallback || *verifyCallback != safeFunction) {
        std::wcout << L"[-] Patch verification failed\n";
        return false;
    }
    
    std::wcout << L"[+] DSE bypass successful\n";
    return true;
}

bool DrvLoader::BypassDSE() {
    std::wcout << L"\n[=== DSE Bypass ===]\n";
    
    if (!InstallAndStartDriver()) return false;
    
    hDriver = CreateFileW(L"\\\\.\\RTCore64", GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (hDriver == INVALID_HANDLE_VALUE) {
        StopAndRemoveDriver();
        return false;
    }
    
    bool result = BypassDSEInternal();
    
    Cleanup();
    StopAndRemoveDriver();
    return result;
}

bool DrvLoader::LoadDriver(const std::wstring& driverPath, DWORD startType, const std::wstring& dependencies) {
    std::wcout << L"\n[=== Load Driver ===]\n";
    
    std::wstring normalizedPath = ConfigManager::NormalizeDriverPath(driverPath);
    std::wstring serviceName = ConfigManager::ExtractServiceName(normalizedPath);
    
    std::wcout << L"[*] Service: " << serviceName << L"\n";
    
    if (GetFileAttributesW(normalizedPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::wcout << L"[-] File not found: " << normalizedPath << L"\n";
        ConfigManager::SaveDriverLoadHistory(normalizedPath, serviceName, startType, false);
        return false;
    }
    
    // Step 1: Install RTCore
    if (!InstallAndStartDriver()) return false;
    
    hDriver = CreateFileW(L"\\\\.\\RTCore64", GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (hDriver == INVALID_HANDLE_VALUE) {
        StopAndRemoveDriver();
        return false;
    }
    
    // Step 2: Patch DSE
    if (!BypassDSEInternal()) {
        Cleanup();
        StopAndRemoveDriver();
        ConfigManager::SaveDriverLoadHistory(normalizedPath, serviceName, startType, false);
        return false;
    }
    
    // Step 3: Create and Start Target Service
    bool serviceSuccess = false;
    SC_HANDLE hSCM = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (hSCM) {
        SC_HANDLE hService = CreateServiceW(hSCM, serviceName.c_str(), serviceName.c_str(),
            SERVICE_ALL_ACCESS, SERVICE_KERNEL_DRIVER, startType, SERVICE_ERROR_NORMAL,
            normalizedPath.c_str(), nullptr, nullptr, dependencies.empty() ? nullptr : dependencies.c_str(), nullptr, nullptr);
            
        if (!hService && GetLastError() == ERROR_SERVICE_EXISTS) {
            hService = OpenServiceW(hSCM, serviceName.c_str(), SERVICE_ALL_ACCESS);
        }
        
        if (hService) {
            if (StartServiceW(hService, 0, nullptr) || GetLastError() == ERROR_SERVICE_ALREADY_RUNNING) {
                std::wcout << L"[+] Service started successfully\n";
                serviceSuccess = true;
            } else {
                std::wcout << L"[-] Failed to start service (error: " << GetLastError() << L")\n";
            }
            CloseServiceHandle(hService);
        }
        CloseServiceHandle(hSCM);
    }
    
    // Step 4: Restore DSE
    RestoreDSEInternal();
    Cleanup();
    StopAndRemoveDriver();
    
    ConfigManager::SaveDriverLoadHistory(normalizedPath, serviceName, startType, serviceSuccess);
    return serviceSuccess;
}

bool DrvLoader::ReloadDriver(const std::wstring& driverPath) {
    std::wcout << L"\n[=== Reload Driver ===]\n";

    std::wstring normalizedPath = ConfigManager::NormalizeDriverPath(driverPath);
    std::wstring serviceName = ConfigManager::ExtractServiceName(normalizedPath);
    
    // Step 1: Install RTCore
    if (!InstallAndStartDriver()) return false;

    hDriver = CreateFileW(L"\\\\.\\RTCore64", GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (hDriver == INVALID_HANDLE_VALUE) {
        StopAndRemoveDriver();
        return false;
    }

    // Step 2: Stop target service if running
    SC_HANDLE hSCM = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (hSCM) {
        SC_HANDLE hService = OpenServiceW(hSCM, serviceName.c_str(), SERVICE_ALL_ACCESS);
        if (hService) {
            SERVICE_STATUS_PROCESS ssp;
            DWORD needed;
            if (QueryServiceStatusEx(hService, SC_STATUS_PROCESS_INFO, (LPBYTE)&ssp, sizeof(ssp), &needed)) {
                if (ssp.dwCurrentState == SERVICE_RUNNING) {
                     ControlService(hService, SERVICE_CONTROL_STOP, (LPSERVICE_STATUS)&ssp);
                }
            }
            CloseServiceHandle(hService);
        }
        
        // Ensure service exists/recreated
        SC_HANDLE hCreate = CreateServiceW(hSCM, serviceName.c_str(), serviceName.c_str(),
             SERVICE_ALL_ACCESS, SERVICE_KERNEL_DRIVER, SERVICE_DEMAND_START, SERVICE_ERROR_NORMAL,
             normalizedPath.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr);
        if (hCreate) CloseServiceHandle(hCreate);
        
        CloseServiceHandle(hSCM);
    }

    // Step 3: Patch DSE
    if (!BypassDSEInternal()) {
        Cleanup();
        StopAndRemoveDriver();
        return false;
    }

    // Step 4: Start target service
    bool startSuccess = false;
    hSCM = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (hSCM) {
        SC_HANDLE hService = OpenServiceW(hSCM, serviceName.c_str(), SERVICE_START);
        if (hService) {
            if (StartServiceW(hService, 0, nullptr)) startSuccess = true;
            CloseServiceHandle(hService);
        }
        CloseServiceHandle(hSCM);
    }

    // Step 5: Restore DSE
    RestoreDSEInternal();
    Cleanup();
    StopAndRemoveDriver();
    
    ConfigManager::SaveDriverLoadHistory(normalizedPath, serviceName, SERVICE_DEMAND_START, startSuccess);
    return startSuccess;
}

bool DrvLoader::StopDriver(const std::wstring& serviceNameOrPath) {
    std::wcout << L"\n[=== Stop Driver ===]\n";
    
    std::wstring serviceName = ConfigManager::ExtractServiceName(serviceNameOrPath);
    std::wcout << L"[*] Service: " << serviceName << L"\n";
    
    SC_HANDLE hSCM = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (!hSCM) {
        std::wcout << L"[-] Failed to open SCM\n";
        return false;
    }
    
    SC_HANDLE hService = OpenServiceW(hSCM, serviceName.c_str(), SERVICE_STOP | SERVICE_QUERY_STATUS);
    if (!hService) {
        std::wcout << L"[-] Service not found\n";
        CloseServiceHandle(hSCM);
        return false;
    }
    
    SERVICE_STATUS_PROCESS ssp;
    DWORD bytesNeeded;
    if (QueryServiceStatusEx(hService, SC_STATUS_PROCESS_INFO, (LPBYTE)&ssp, sizeof(ssp), &bytesNeeded)) {
        if (ssp.dwCurrentState == SERVICE_STOPPED) {
            std::wcout << L"[*] Service is already stopped\n";
            CloseServiceHandle(hService);
            CloseServiceHandle(hSCM);
            return true;
        }
    }
    
    SERVICE_STATUS status;
    if (ControlService(hService, SERVICE_CONTROL_STOP, &status)) {
        std::wcout << L"[+] Stop command sent\n";
    } else {
        std::wcout << L"[-] Failed to stop service (Error: " << GetLastError() << L")\n";
        CloseServiceHandle(hService);
        CloseServiceHandle(hSCM);
        return false;
    }
    
    CloseServiceHandle(hService);
    CloseServiceHandle(hSCM);
    return true;
}

bool DrvLoader::RemoveDriver(const std::wstring& serviceNameOrPath) {
    std::wcout << L"\n[=== Remove Driver ===]\n";
    
    std::wstring serviceName = ConfigManager::ExtractServiceName(serviceNameOrPath);
    std::wcout << L"[*] Service: " << serviceName << L"\n";
    
    // Stop it first using internal logic or SCM calls
    StopDriver(serviceName);
    
    SC_HANDLE hSCM = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (!hSCM) return false;
    
    SC_HANDLE hService = OpenServiceW(hSCM, serviceName.c_str(), DELETE);
    if (!hService) {
        std::wcout << L"[-] Service not found or access denied\n";
        CloseServiceHandle(hSCM);
        return false;
    }
    
    if (DeleteService(hService)) {
        std::wcout << L"[+] Service marked for deletion\n";
    } else {
        std::wcout << L"[-] Failed to delete service (Error: " << GetLastError() << L")\n";
        CloseServiceHandle(hService);
        CloseServiceHandle(hSCM);
        return false;
    }

    CloseServiceHandle(hService);
    CloseServiceHandle(hSCM);
    return true;
} 

bool DrvLoader::RestoreDSEInternal() {
    if (!originalCallback) {
        std::wcout << L"[-] No original callback value available\n";
        return false;
    }

    // Strict resolution
    auto offsets = ResolveKernelOffsetsStrict();
    if (!offsets) {
        std::wcout << L"[-] Failed to resolve kernel offsets\n";
        return false;
    }

    auto [seCiOffset, zwFlushOffset] = *offsets;

    auto ntBase = GetNtoskrnlBase();
    if (!ntBase) {
        std::wcout << L"[-] Failed to get ntoskrnl.exe base address\n";
        return false;
    }

    // Validate addresses
    if (!ValidateKernelAddresses(*ntBase, seCiOffset, zwFlushOffset)) {
        std::wcout << L"[-] Address validation failed\n";
        return false;
    }

    uint64_t callbackAddress = *ntBase + seCiOffset + 0x20;

    auto currentCallback = ReadMemory64(callbackAddress);
    if (!currentCallback) {
        std::wcout << L"[-] Failed to read current callback\n";
        return false;
    }

    // Already restored?
    if (*currentCallback == *originalCallback) {
        std::wcout << L"[+] DSE already restored\n";
        ConfigManager::ClearPatchStateFromRegistry();
        return true;
    }

    std::wcout << L"[*] Restoring callback:\n";
    std::wcout << L"    From: 0x" << std::hex << *currentCallback << L"\n";
    std::wcout << L"    To:   0x" << *originalCallback << std::dec << L"\n";

    if (WriteMemory64(callbackAddress, *originalCallback)) {
        // Verify restoration

        auto verifyCallback = ReadMemory64(callbackAddress);
        if (!verifyCallback || *verifyCallback != *originalCallback) {
            std::wcout << L"[-] Restoration verification failed\n";
            return false;
        }

        std::wcout << L"[+] DSE restored successfully\n";
        ConfigManager::ClearPatchStateFromRegistry();
        return true;
    }

    std::wcout << L"[-] Failed to restore callback\n";
    return false;
}

bool DrvLoader::RestoreDSE() {
    std::wcout << L"\n[=== Restore DSE ===]\n";
    if (!originalCallback) {
        std::wcout << L"[-] No original callback state known\n";
        return false;
    }

    if (!InstallAndStartDriver())
        return false;

    hDriver = CreateFileW(
        L"\\\\.\\RTCore64",
        GENERIC_READ | GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        0,
        nullptr
    );

    if (hDriver == INVALID_HANDLE_VALUE) {
        StopAndRemoveDriver();
        return false;
    }

    bool result = RestoreDSEInternal();

    Cleanup();
    StopAndRemoveDriver();
    return result;
}
