#include "HdrCore.h"
#include "ConfigManager.h"
#include "ResourceInstaller.h"
#include <iostream>
#include <vector>
#include <psapi.h>

#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "version.lib")

static constexpr BYTE HDR_PATCH_BYTES[] = { 0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3 };

// ============================================================================
// INITIALIZATION / CLEANUP
// ============================================================================

bool HdrEnabler::Initialize() {
    if (!symbolDownloader.Initialize()) {
        std::wcout << L"[-] Failed to initialize symbol downloader\n";
        return false;
    }
    return true;
}

void HdrEnabler::Cleanup() {
    if (hDuoDriver != INVALID_HANDLE_VALUE) {
        CloseHandle(hDuoDriver);
        hDuoDriver = INVALID_HANDLE_VALUE;
    }
    if (hDriver != INVALID_HANDLE_VALUE) {
        CloseHandle(hDriver);
        hDriver = INVALID_HANDLE_VALUE;
    }
}

// ============================================================================
// RTCORE64 MEMORY OPERATIONS (reading + DSE bypass)
// ============================================================================

bool HdrEnabler::WriteMemory32(uint64_t address, uint32_t value) {
    if (hDriver == INVALID_HANDLE_VALUE) return false;

    RTC_MEMORY_WRITE writePacket{};
    writePacket.Address = address;
    writePacket.Size = sizeof(uint32_t);
    writePacket.Value = value;

    DWORD bytesReturned = 0;
    return DeviceIoControl(hDriver, RTC_IOCTL_MEMORY_WRITE, &writePacket, sizeof(writePacket),
                          &writePacket, sizeof(writePacket), &bytesReturned, nullptr);
}

bool HdrEnabler::WriteMemory64(uint64_t address, uint64_t value) {
    return WriteMemory32(address, static_cast<uint32_t>(value & 0xFFFFFFFF)) &&
           WriteMemory32(address + 4, static_cast<uint32_t>((value >> 32) & 0xFFFFFFFF));
}

std::optional<uint32_t> HdrEnabler::ReadMemory32(uint64_t address) {
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

std::optional<uint64_t> HdrEnabler::ReadMemory64(uint64_t address) {
    auto low = ReadMemory32(address);
    auto high = ReadMemory32(address + 4);
    if (!low || !high) return std::nullopt;

    return (static_cast<uint64_t>(*high) << 32) | *low;
}

// ============================================================================
// DUODRIVER PROTECTED MEMORY PATCHING
// ============================================================================

bool HdrEnabler::PatchProtectedMemory(uint64_t address, const void* data, size_t size) {
    if (hDuoDriver == INVALID_HANDLE_VALUE) return false;
    if (!data || size == 0 || size > 32) return false;

    std::vector<BYTE> inputBuffer(sizeof(DUO_WRITE_PROTECTED_REQUEST) + size);
    auto req = reinterpret_cast<DUO_WRITE_PROTECTED_REQUEST*>(inputBuffer.data());
    req->Address = address;
    req->Size = size;
    memcpy(inputBuffer.data() + sizeof(DUO_WRITE_PROTECTED_REQUEST), data, size);

    DWORD bytesReturned = 0;
    return DeviceIoControl(hDuoDriver, DUO_IOCTL_PATCH_PROTECTED_MEMORY,
                          inputBuffer.data(), static_cast<DWORD>(inputBuffer.size()),
                          nullptr, 0, &bytesReturned, nullptr);
}

// ============================================================================
// MODULE BASE ADDRESS RESOLUTION
// ============================================================================

std::optional<uint64_t> HdrEnabler::GetModuleBaseByName(const WCHAR* targetName) {
    std::vector<LPVOID> drivers(1024);
    DWORD needed = 0;

    if (!EnumDeviceDrivers(drivers.data(), static_cast<DWORD>(drivers.size() * sizeof(LPVOID)), &needed))
        return std::nullopt;

    drivers.resize(needed / sizeof(LPVOID));

    for (const auto& driver : drivers) {
        WCHAR driverName[MAX_PATH];
        if (GetDeviceDriverBaseNameW(driver, driverName, MAX_PATH) && _wcsicmp(driverName, targetName) == 0) {
            return reinterpret_cast<uint64_t>(driver);
        }
    }

    return std::nullopt;
}

std::optional<uint64_t> HdrEnabler::GetDxgkrnlBase() {
    return GetModuleBaseByName(L"dxgkrnl.sys");
}

std::optional<uint64_t> HdrEnabler::GetNtoskrnlBase() {
    return GetModuleBaseByName(L"ntoskrnl.exe");
}

// ============================================================================
// SYMBOL RESOLUTION
// ============================================================================

std::optional<std::pair<uint64_t, std::wstring>> HdrEnabler::ResolveDxgOffsetsStrict() {
    WCHAR systemRoot[MAX_PATH];
    GetSystemDirectoryW(systemRoot, MAX_PATH);
    std::wstring dxgkrnlPath = std::wstring(systemRoot) + L"\\drivers\\dxgkrnl.sys";

    std::wcout << L"[*] Strict offset resolution from dxgkrnl PDB...\n";

    auto [pdbName, pdbGuid] = symbolDownloader.GetPdbInfoFromPe(dxgkrnlPath);
    if (pdbGuid.empty()) {
        std::wcout << L"[-] Failed to extract PDB GUID from dxgkrnl.sys\n";
        return std::nullopt;
    }

    std::wcout << L"[+] dxgkrnl PDB GUID: " << pdbGuid << L"\n";

    if (!symbolDownloader.DownloadSymbolsForModule(dxgkrnlPath)) {
        std::wcout << L"[-] Failed to obtain dxgkrnl PDB symbols\n";
        return std::nullopt;
    }

    auto hdrOpt = symbolDownloader.GetSymbolOffset(dxgkrnlPath, L"IsHdrSourceModePinned");

    if (!hdrOpt) {
        std::wcout << L"[-] Failed to resolve IsHdrSourceModePinned from PDB\n";
        return std::nullopt;
    }

    std::wcout << L"[+] Resolved offset from dxgkrnl PDB:\n";
    std::wcout << L"    IsHdrSourceModePinned: 0x" << std::hex << *hdrOpt << std::dec << L"\n";

    return std::make_pair(*hdrOpt, pdbGuid);
}

std::optional<std::pair<uint64_t, uint64_t>> HdrEnabler::ResolveNtoskrnlOffsetsStrict() {
    WCHAR systemRoot[MAX_PATH];
    GetSystemDirectoryW(systemRoot, MAX_PATH);
    std::wstring ntoskrnlPath = std::wstring(systemRoot) + L"\\ntoskrnl.exe";

    std::wcout << L"[*] Strict offset resolution from ntoskrnl PDB...\n";

    auto [pdbName, pdbGuid] = symbolDownloader.GetPdbInfoFromPe(ntoskrnlPath);
    if (pdbGuid.empty()) {
        std::wcout << L"[-] Failed to extract PDB GUID from ntoskrnl.exe\n";
        return std::nullopt;
    }

    std::wcout << L"[+] ntoskrnl PDB GUID: " << pdbGuid << L"\n";

    if (!symbolDownloader.DownloadSymbolsForModule(ntoskrnlPath)) {
        std::wcout << L"[-] Failed to obtain ntoskrnl PDB symbols\n";
        return std::nullopt;
    }

    auto seCiOpt = symbolDownloader.GetSymbolOffset(ntoskrnlPath, L"SeCiCallbacks");
    auto zwOpt = symbolDownloader.GetSymbolOffset(ntoskrnlPath, L"ZwFlushInstructionCache");

    if (!seCiOpt || !zwOpt) {
        std::wcout << L"[-] Failed to resolve required symbols from ntoskrnl PDB\n";
        return std::nullopt;
    }

    std::wcout << L"[+] Resolved offsets from ntoskrnl PDB:\n";
    std::wcout << L"    SeCiCallbacks: 0x" << std::hex << *seCiOpt << std::dec << L"\n";
    std::wcout << L"    ZwFlushInstructionCache: 0x" << std::hex << *zwOpt << std::dec << L"\n";

    return std::make_pair(*seCiOpt, *zwOpt);
}

bool HdrEnabler::GetSymbolOffsets(uint64_t* hdrSourceModePinned) {
    auto offsets = ResolveDxgOffsetsStrict();
    if (!offsets) {
        return false;
    }

    *hdrSourceModePinned = offsets->first;
    return true;
}

// ============================================================================
// PE SECTION VALIDATION
// ============================================================================

std::optional<std::pair<uint64_t, uint64_t>> HdrEnabler::GetTextSectionBounds(const std::wstring& pePath) {
    HANDLE hFile = CreateFileW(pePath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
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

bool HdrEnabler::ValidateKernelAddresses(uint64_t ntBase, uint64_t seCiRva, uint64_t zwRva) {
    WCHAR systemRoot[MAX_PATH];
    GetSystemDirectoryW(systemRoot, MAX_PATH);
    std::wstring ntoskrnlPath = std::wstring(systemRoot) + L"\\ntoskrnl.exe";

    auto textBounds = GetTextSectionBounds(ntoskrnlPath);
    if (!textBounds) {
        std::wcout << L"[-] Failed to read .text section from ntoskrnl.exe\n";
        return false;
    }

    auto [textStart, textEnd] = *textBounds;

    if (zwRva < textStart || zwRva >= textEnd) {
        std::wcout << L"[-] ZwFlushInstructionCache offset 0x" << std::hex << zwRva
                   << L" is outside .text section [0x" << textStart << L"-0x" << textEnd << L"]\n" << std::dec;
        return false;
    }

    std::wcout << L"[+] ZwFlushInstructionCache validated in .text section\n";

    uint64_t callbackAddress = ntBase + seCiRva + 0x20;

    auto testRead = ReadMemory64(callbackAddress);
    if (!testRead) {
        std::wcout << L"[-] Cannot read SeCiCallbacks+0x20 at 0x" << std::hex << callbackAddress << std::dec << L"\n";
        return false;
    }

    std::wcout << L"[+] SeCiCallbacks address validated (readable)\n";

    return true;
}

// ============================================================================
// RTCORE64 (SIGNED HELPER DRIVER) MANAGEMENT
// ============================================================================

bool HdrEnabler::CheckRtcDriverFileExists() {
    std::wstring driverPath = ConfigManager::GetDriverPath();
    if (GetFileAttributesW(driverPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::wcout << L"[*] Installing RTCore64 from embedded resource...\n";
        if (!ResourceInstaller::InstallDriverFromResource()) return false;

        if (GetFileAttributesW(driverPath.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
    }
    return true;
}

bool HdrEnabler::StopAndRemoveRtcDriver() {
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

bool HdrEnabler::InstallAndStartRtcDriver() {
    if (!CheckRtcDriverFileExists()) return false;
    StopAndRemoveRtcDriver();

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

// ============================================================================
// DUODRIVER (UNSIGNED DRIVER) MANAGEMENT
// ============================================================================

bool HdrEnabler::CheckDuoDriverFileExists() {
    std::wstring driverPath = ConfigManager::GetDuoDriverPath();
    if (GetFileAttributesW(driverPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::wcout << L"[*] Installing DuoDriver from embedded resource...\n";
        if (!ResourceInstaller::InstallDuoDriverFromResource()) return false;

        if (GetFileAttributesW(driverPath.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
    }
    return true;
}

bool HdrEnabler::StopAndRemoveDuoDriver() {
    SC_HANDLE hSCM = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (!hSCM) return false;

    SC_HANDLE hService = OpenServiceW(hSCM, L"DuoDriver", SERVICE_ALL_ACCESS);
    if (hService) {
        SERVICE_STATUS serviceStatus;
        ControlService(hService, SERVICE_CONTROL_STOP, &serviceStatus);
        DeleteService(hService);
        CloseServiceHandle(hService);
    }

    CloseServiceHandle(hSCM);
    return true;
}

bool HdrEnabler::InstallAndStartDuoDriver() {
    if (!CheckDuoDriverFileExists()) return false;
    StopAndRemoveDuoDriver();

    SC_HANDLE hSCM = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (!hSCM) return false;

    std::wstring driverPath = L"System32\\drivers\\DuoDriver.sys";

    SC_HANDLE hService = CreateServiceW(hSCM, L"DuoDriver", L"DuoDriver", SERVICE_ALL_ACCESS,
        SERVICE_KERNEL_DRIVER, SERVICE_DEMAND_START, SERVICE_ERROR_NORMAL,
        driverPath.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr);

    if (!hService) {
        CloseServiceHandle(hSCM);
        return false;
    }

    if (!StartServiceW(hService, 0, nullptr)) {
        CloseServiceHandle(hService);
        CloseServiceHandle(hSCM);
        return false;
    }

    CloseServiceHandle(hService);
    CloseServiceHandle(hSCM);
    return true;
}

// ============================================================================
// DSE BYPASS AND RESTORE
// ============================================================================

bool HdrEnabler::BypassDSE() {
    std::wcout << L"\n[*] Bypassing DSE...\n";

    auto offsets = ResolveNtoskrnlOffsetsStrict();
    if (!offsets) {
        std::wcout << L"[-] Failed to resolve ntoskrnl offsets for DSE bypass\n";
        return false;
    }

    auto [seCiOffset, zwFlushOffset] = *offsets;

    auto ntBase = GetNtoskrnlBase();
    if (!ntBase) {
        std::wcout << L"[-] Failed to get ntoskrnl.exe base address\n";
        return false;
    }

    std::wcout << L"[+] ntoskrnl.exe base: 0x" << std::hex << *ntBase << std::dec << L"\n";

    if (!ValidateKernelAddresses(*ntBase, seCiOffset, zwFlushOffset)) {
        std::wcout << L"[-] Address validation failed - aborting DSE bypass\n";
        return false;
    }

    uint64_t callbackAddress = *ntBase + seCiOffset + 0x20;
    uint64_t safeFunction = *ntBase + zwFlushOffset;

    auto currentCallback = ReadMemory64(callbackAddress);
    if (!currentCallback) {
        std::wcout << L"[-] Failed to read current callback\n";
        return false;
    }

    if (*currentCallback == safeFunction) {
        std::wcout << L"[+] DSE already bypassed\n";
        return true;
    }

    std::wcout << L"[*] Patching callback:\n";
    std::wcout << L"    From: 0x" << std::hex << *currentCallback << L"\n";
    std::wcout << L"    To:   0x" << safeFunction << std::dec << L"\n";

    originalCallback = *currentCallback;
    if (!ConfigManager::SaveDseStateToRegistry(*originalCallback)) {
        std::wcout << L"[-] Failed to save DSE state to registry\n";
        return false;
    }

    if (!WriteMemory64(callbackAddress, safeFunction)) {
        std::wcout << L"[-] Failed to write patched callback\n";
        return false;
    }

    auto verifyCallback = ReadMemory64(callbackAddress);
    if (!verifyCallback || *verifyCallback != safeFunction) {
        std::wcout << L"[-] DSE patch verification failed\n";
        return false;
    }

    std::wcout << L"[+] DSE bypass successful\n";
    return true;
}

bool HdrEnabler::RestoreDSE() {
    std::wcout << L"\n[*] Restoring DSE...\n";

    if (!originalCallback) {
        std::wcout << L"[-] No original callback value available\n";
        return false;
    }

    auto offsets = ResolveNtoskrnlOffsetsStrict();
    if (!offsets) {
        std::wcout << L"[-] Failed to resolve ntoskrnl offsets for DSE restore\n";
        return false;
    }

    auto [seCiOffset, zwFlushOffset] = *offsets;

    auto ntBase = GetNtoskrnlBase();
    if (!ntBase) {
        std::wcout << L"[-] Failed to get ntoskrnl.exe base address\n";
        return false;
    }

    uint64_t callbackAddress = *ntBase + seCiOffset + 0x20;

    auto currentCallback = ReadMemory64(callbackAddress);
    if (!currentCallback) {
        std::wcout << L"[-] Failed to read current callback\n";
        return false;
    }

    if (*currentCallback == *originalCallback) {
        std::wcout << L"[+] DSE already restored\n";
        ConfigManager::ClearDseStateFromRegistry();
        originalCallback.reset();
        return true;
    }

    std::wcout << L"[*] Restoring callback:\n";
    std::wcout << L"    From: 0x" << std::hex << *currentCallback << L"\n";
    std::wcout << L"    To:   0x" << *originalCallback << std::dec << L"\n";

    if (!WriteMemory64(callbackAddress, *originalCallback)) {
        std::wcout << L"[-] Failed to restore callback\n";
        return false;
    }

    auto verifyCallback = ReadMemory64(callbackAddress);
    if (!verifyCallback || *verifyCallback != *originalCallback) {
        std::wcout << L"[-] DSE restoration verification failed\n";
        return false;
    }

    std::wcout << L"[+] DSE restored successfully\n";
    ConfigManager::ClearDseStateFromRegistry();
    originalCallback.reset();
    return true;
}

// ============================================================================
// HDR OPERATIONS
// ============================================================================

bool HdrEnabler::EnableHdr() {
    std::wcout << L"\n[=== Enable HDR (Patch IsHdrSourceModePinned) ===]\n";

    if (!ConfigManager::CheckAndDisableMemoryIntegrity()) {
        return false;
    }

    auto offsets = ResolveDxgOffsetsStrict();
    if (!offsets) {
        std::wcout << L"[-] Failed to resolve dxgkrnl offsets\n";
        return false;
    }

    auto [hdrOffset, pdbGuid] = *offsets;

    auto dxgBase = GetDxgkrnlBase();
    if (!dxgBase) {
        std::wcout << L"[-] Failed to get dxgkrnl.sys base address\n";
        return false;
    }

    std::wcout << L"[+] dxgkrnl.sys base: 0x" << std::hex << *dxgBase << std::dec << L"\n";

    // Step 1: Install and start RTCore64 (signed helper driver)
    if (!InstallAndStartRtcDriver()) return false;

    hDriver = CreateFileW(L"\\\\.\\RTCore64", GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (hDriver == INVALID_HANDLE_VALUE) {
        StopAndRemoveRtcDriver();
        return false;
    }

    uint64_t targetAddress = *dxgBase + hdrOffset;
    std::wcout << L"[+] IsHdrSourceModePinned kernel address: 0x" << std::hex << targetAddress << std::dec << L"\n";

    // Step 2: Read current bytes to check if already patched
    auto currentBytes = ReadMemory64(targetAddress);
    if (!currentBytes) {
        std::wcout << L"[-] Failed to read IsHdrSourceModePinned\n";
        Cleanup();
        StopAndRemoveRtcDriver();
        return false;
    }

    uint64_t patchValue = 0;
    memcpy(&patchValue, HDR_PATCH_BYTES, sizeof(HDR_PATCH_BYTES));
    constexpr uint64_t patchMask = 0x0000FFFFFFFFFFFF;

    if ((*currentBytes & patchMask) == (patchValue & patchMask)) {
        std::wcout << L"[+] IsHdrSourceModePinned is already patched\n";
        Cleanup();
        StopAndRemoveRtcDriver();
        return true;
    }

    std::wcout << L"[*] Original bytes: 0x" << std::hex << *currentBytes << std::dec << L"\n";

    // Step 3: Save original bytes to registry
    if (!ConfigManager::SaveOriginalFunctionToRegistry(*currentBytes)) {
        std::wcout << L"[-] Failed to save original function to registry\n";
        Cleanup();
        StopAndRemoveRtcDriver();
        return false;
    }

    // Step 4: Disable DSE (patch SeCiCallbacks via RTCore64)
    if (!BypassDSE()) {
        std::wcout << L"[-] Failed to bypass DSE\n";
        Cleanup();
        StopAndRemoveRtcDriver();
        return false;
    }

    // Step 5: Load unsigned DuoDriver
    if (!InstallAndStartDuoDriver()) {
        std::wcout << L"[-] Failed to load DuoDriver\n";
        RestoreDSE();
        Cleanup();
        StopAndRemoveRtcDriver();
        return false;
    }

    hDuoDriver = CreateFileW(L"\\\\.\\DuoDriver", GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (hDuoDriver == INVALID_HANDLE_VALUE) {
        std::wcout << L"[-] Failed to open DuoDriver device\n";
        StopAndRemoveDuoDriver();
        RestoreDSE();
        Cleanup();
        StopAndRemoveRtcDriver();
        return false;
    }

    // Step 6: Patch IsHdrSourceModePinned via DuoDriver IOCTL_PATCH_PROTECTED_MEMORY
    if (!PatchProtectedMemory(targetAddress, HDR_PATCH_BYTES, sizeof(HDR_PATCH_BYTES))) {
        std::wcout << L"[-] Failed to patch via DuoDriver\n";
        Cleanup();
        StopAndRemoveDuoDriver();
        RestoreDSE();
        StopAndRemoveRtcDriver();
        return false;
    }

    // Step 7: Unload DuoDriver
    if (hDuoDriver != INVALID_HANDLE_VALUE) {
        CloseHandle(hDuoDriver);
        hDuoDriver = INVALID_HANDLE_VALUE;
    }
    StopAndRemoveDuoDriver();

    // Step 8: Re-enable DSE
    RestoreDSE();

    // Step 9: Verify patch via RTCore64
    hDriver = CreateFileW(L"\\\\.\\RTCore64", GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (hDriver == INVALID_HANDLE_VALUE) {
        StopAndRemoveRtcDriver();
        return false;
    }

    auto verifyBytes = ReadMemory64(targetAddress);
    if (!verifyBytes || (*verifyBytes & patchMask) != (patchValue & patchMask)) {
        std::wcout << L"[-] Patch verification failed\n";
        Cleanup();
        StopAndRemoveRtcDriver();
        return false;
    }

    ConfigManager::SavePatchState(true);

    std::wcout << L"[+] IsHdrSourceModePinned patched successfully\n";
    std::wcout << L"    mov eax, 1 ; ret\n";

    Cleanup();
    StopAndRemoveRtcDriver();
    return true;
}

bool HdrEnabler::DisableHdr() {
    std::wcout << L"\n[=== Disable HDR (Restore IsHdrSourceModePinned) ===]\n";

    if (!ConfigManager::CheckAndDisableMemoryIntegrity()) {
        return false;
    }

    auto originalBytes = ConfigManager::LoadOriginalFunctionFromRegistry();
    if (!originalBytes) {
        std::wcout << L"[-] No original function state found - cannot restore\n";
        return false;
    }

    auto offsets = ResolveDxgOffsetsStrict();
    if (!offsets) {
        std::wcout << L"[-] Failed to resolve dxgkrnl offsets\n";
        return false;
    }

    auto [hdrOffset, pdbGuid] = *offsets;

    auto dxgBase = GetDxgkrnlBase();
    if (!dxgBase) {
        std::wcout << L"[-] Failed to get dxgkrnl.sys base address\n";
        return false;
    }

    std::wcout << L"[+] dxgkrnl.sys base: 0x" << std::hex << *dxgBase << std::dec << L"\n";

    // Step 1: Install and start RTCore64 (signed helper driver)
    if (!InstallAndStartRtcDriver()) return false;

    hDriver = CreateFileW(L"\\\\.\\RTCore64", GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (hDriver == INVALID_HANDLE_VALUE) {
        StopAndRemoveRtcDriver();
        return false;
    }

    uint64_t targetAddress = *dxgBase + hdrOffset;

    uint64_t patchValue = 0;
    memcpy(&patchValue, HDR_PATCH_BYTES, sizeof(HDR_PATCH_BYTES));
    constexpr uint64_t patchMask = 0x0000FFFFFFFFFFFF;

    // Step 2: Read current bytes to check if actually patched
    auto currentBytes = ReadMemory64(targetAddress);
    if (!currentBytes) {
        std::wcout << L"[-] Failed to read IsHdrSourceModePinned\n";
        Cleanup();
        StopAndRemoveRtcDriver();
        return false;
    }

    if ((*currentBytes & patchMask) != (patchValue & patchMask)) {
        std::wcout << L"[+] IsHdrSourceModePinned is not patched - nothing to restore\n";
        Cleanup();
        StopAndRemoveRtcDriver();
        return true;
    }

    std::wcout << L"[*] Restoring original function bytes: 0x" << std::hex << *originalBytes << std::dec << L"\n";

    // Step 3: Disable DSE
    if (!BypassDSE()) {
        std::wcout << L"[-] Failed to bypass DSE\n";
        Cleanup();
        StopAndRemoveRtcDriver();
        return false;
    }

    // Step 4: Load unsigned DuoDriver
    if (!InstallAndStartDuoDriver()) {
        std::wcout << L"[-] Failed to load DuoDriver\n";
        RestoreDSE();
        Cleanup();
        StopAndRemoveRtcDriver();
        return false;
    }

    hDuoDriver = CreateFileW(L"\\\\.\\DuoDriver", GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (hDuoDriver == INVALID_HANDLE_VALUE) {
        std::wcout << L"[-] Failed to open DuoDriver device\n";
        StopAndRemoveDuoDriver();
        RestoreDSE();
        Cleanup();
        StopAndRemoveRtcDriver();
        return false;
    }

    // Step 5: Restore original bytes via DuoDriver IOCTL_PATCH_PROTECTED_MEMORY
    if (!PatchProtectedMemory(targetAddress, &*originalBytes, sizeof(uint64_t))) {
        std::wcout << L"[-] Failed to restore via DuoDriver\n";
        Cleanup();
        StopAndRemoveDuoDriver();
        RestoreDSE();
        StopAndRemoveRtcDriver();
        return false;
    }

    // Step 6: Unload DuoDriver
    if (hDuoDriver != INVALID_HANDLE_VALUE) {
        CloseHandle(hDuoDriver);
        hDuoDriver = INVALID_HANDLE_VALUE;
    }
    StopAndRemoveDuoDriver();

    // Step 7: Re-enable DSE
    RestoreDSE();

    // Step 8: Verify restoration via RTCore64
    hDriver = CreateFileW(L"\\\\.\\RTCore64", GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (hDriver == INVALID_HANDLE_VALUE) {
        StopAndRemoveRtcDriver();
        return false;
    }

    auto verifyBytes = ReadMemory64(targetAddress);
    if (!verifyBytes || *verifyBytes != *originalBytes) {
        std::wcout << L"[-] Restoration verification failed\n";
        Cleanup();
        StopAndRemoveRtcDriver();
        return false;
    }

    ConfigManager::SavePatchState(false);

    std::wcout << L"[+] IsHdrSourceModePinned restored successfully\n";

    Cleanup();
    StopAndRemoveRtcDriver();
    return true;
}

bool HdrEnabler::CheckHdrStatus(bool& isPatched) {
    std::wcout << L"\n[=== Check HDR Status ===]\n\n";

    auto offsets = ResolveDxgOffsetsStrict();
    if (!offsets) {
        std::wcout << L"[-] Failed to resolve dxgkrnl offsets\n";
        return false;
    }

    auto [hdrOffset, pdbGuid] = *offsets;

    // CheckHdrStatus only needs to read, so no DSE bypass or DuoDriver needed
    if (!InstallAndStartRtcDriver()) return false;

    hDriver = CreateFileW(L"\\\\.\\RTCore64", GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (hDriver == INVALID_HANDLE_VALUE) {
        StopAndRemoveRtcDriver();
        return false;
    }

    auto dxgBase = GetDxgkrnlBase();
    if (!dxgBase) {
        Cleanup();
        StopAndRemoveRtcDriver();
        return false;
    }

    std::wcout << L"[+] dxgkrnl.sys base: 0x" << std::hex << *dxgBase << std::dec << L"\n";

    uint64_t targetAddress = *dxgBase + hdrOffset;

    auto currentBytes = ReadMemory64(targetAddress);
    if (!currentBytes) {
        Cleanup();
        StopAndRemoveRtcDriver();
        return false;
    }

    uint64_t patchValue = 0;
    memcpy(&patchValue, HDR_PATCH_BYTES, sizeof(HDR_PATCH_BYTES));

    isPatched = (*currentBytes == patchValue);

    std::wcout << L"[+] HDR Status: " << (isPatched ? L"PATCHED (enabled)" : L"ORIGINAL (disabled)") << L"\n";
    std::wcout << L"    Current function: 0x" << std::hex << *currentBytes << std::dec << L"\n";

    ConfigManager::SavePatchState(isPatched);

    Cleanup();
    StopAndRemoveRtcDriver();
    return true;
}
