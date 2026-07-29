#pragma once

#include <Windows.h>
#include <vector>
#include <cstdint>

namespace ResourceInstaller {
    constexpr BYTE XOR_KEY[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE, 0xBA, 0xBE };
    constexpr size_t XOR_KEY_LEN = sizeof(XOR_KEY);

    std::vector<BYTE> ExtractAndDecryptDriver(HINSTANCE hInstance, int resourceId);
    bool InstallDriverFromResource();
    bool InstallDuoDriverFromResource();
    bool IsDriverInstalled();
}
