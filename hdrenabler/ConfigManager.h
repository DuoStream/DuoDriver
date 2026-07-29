#pragma once

#include <Windows.h>
#include <cstdint>
#include <optional>
#include <string>

namespace ConfigManager {
    std::wstring GetDriverPath();
    std::wstring GetDuoDriverPath();

    bool CheckAndDisableMemoryIntegrity();

    bool SaveOriginalFunctionToRegistry(uint64_t functionBytes);
    std::optional<uint64_t> LoadOriginalFunctionFromRegistry();
    bool SavePatchState(bool isPatched);
    bool IsCurrentlyPatched();
    bool ClearPatchState();

    bool SaveDseStateToRegistry(uint64_t originalCallback);
    std::optional<uint64_t> LoadDseStateFromRegistry();
    bool ClearDseStateFromRegistry();
}
