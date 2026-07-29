#pragma once

#include <Windows.h>
#include <cstdint>
#include <optional>
#include <vector>
#include <utility>
#include "SymbolDownloader.h"

// RTCore64 IOCTLs (signed helper driver for kernel memory R/W)
constexpr DWORD RTC_IOCTL_MEMORY_READ = 0x80002048;
constexpr DWORD RTC_IOCTL_MEMORY_WRITE = 0x8000204C;

struct alignas(8) RTC_MEMORY_READ {
    BYTE Pad0[8];
    uint64_t Address;
    BYTE Pad1[8];
    uint32_t Size;
    uint32_t Value;
    BYTE Pad3[16];
};

struct alignas(8) RTC_MEMORY_WRITE {
    BYTE Pad0[8];
    uint64_t Address;
    BYTE Pad1[8];
    uint32_t Size;
    uint32_t Value;
    BYTE Pad3[16];
};

// DuoDriver IOCTLs (unsigned driver for protected memory patching)
constexpr DWORD DUO_IOCTL_PATCH_PROTECTED_MEMORY = 0x0022200C;

#pragma pack(push, 1)
struct DUO_WRITE_PROTECTED_REQUEST {
    uint64_t Address;
    size_t Size;
    // Patch data bytes follow immediately after this header
};
#pragma pack(pop)

class HdrEnabler {
public:
    HANDLE hDriver{ INVALID_HANDLE_VALUE };      // RTCore64 handle
    HANDLE hDuoDriver{ INVALID_HANDLE_VALUE };   // DuoDriver handle
    std::optional<uint64_t> originalCallback;     // DSE original callback value
    SymbolDownloader symbolDownloader;

    bool Initialize();
    void Cleanup();

    // RTCore64 memory operations (used for reading + DSE bypass)
    bool WriteMemory32(uint64_t address, uint32_t value);
    bool WriteMemory64(uint64_t address, uint64_t value);
    std::optional<uint32_t> ReadMemory32(uint64_t address);
    std::optional<uint64_t> ReadMemory64(uint64_t address);

    // DuoDriver protected memory patching
    bool PatchProtectedMemory(uint64_t address, const void* data, size_t size);

    bool EnableHdr();
    bool DisableHdr();
    bool CheckHdrStatus(bool& isPatched);

    bool GetSymbolOffsets(uint64_t* hdrSourceModePinned);

private:
    std::optional<uint64_t> GetModuleBaseByName(const WCHAR* targetName);
    std::optional<uint64_t> GetDxgkrnlBase();
    std::optional<uint64_t> GetNtoskrnlBase();

    std::optional<std::pair<uint64_t, std::wstring>> ResolveDxgOffsetsStrict();
    std::optional<std::pair<uint64_t, uint64_t>> ResolveNtoskrnlOffsetsStrict();

    std::optional<std::pair<uint64_t, uint64_t>> GetTextSectionBounds(const std::wstring& pePath);
    bool ValidateKernelAddresses(uint64_t ntBase, uint64_t seCiRva, uint64_t zwRva);

    // RTCore64 (signed helper driver) management
    bool CheckRtcDriverFileExists();
    bool InstallAndStartRtcDriver();
    bool StopAndRemoveRtcDriver();

    // DuoDriver (unsigned driver) management
    bool CheckDuoDriverFileExists();
    bool InstallAndStartDuoDriver();
    bool StopAndRemoveDuoDriver();

    // DSE bypass and restore (requires RTCore64 handle to be open)
    bool BypassDSE();
    bool RestoreDSE();
};
