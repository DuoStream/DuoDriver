#pragma once

#include <Windows.h>
#include <string>

namespace UI {
    void DisplayBanner();
    void ClearInputBuffer();
    void WaitForAnyKey(const std::wstring& message);
    void DisplayMenu(bool isPatched);
}
