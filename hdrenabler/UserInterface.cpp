#include "UserInterface.h"
#include <iostream>
#include <limits>
#include <conio.h>

namespace UI {

void DisplayBanner() {
    std::wcout << L"\n";
    std::wcout << L"+----------------------------------------------------------+\n";
    std::wcout << L"|  HDR Enabler - dxgkrnl.sys IsHdrSourceModePinned Patcher |\n";
    std::wcout << L"+----------------------------------------------------------+\n";
    std::wcout << L"\n";
}

void ClearInputBuffer() {
    std::wcin.clear();
    std::wcin.ignore(std::numeric_limits<std::streamsize>::max(), L'\n');
}

void WaitForAnyKey(const std::wstring& message) {
    std::wcout << message;
    std::wcout.flush();
    FlushConsoleInputBuffer(GetStdHandle(STD_INPUT_HANDLE));
    _getwch();
    std::wcout << L"\n";
}

void DisplayMenu(bool isPatched) {
    std::wcout << L"\n";
    std::wcout << L"=========================================================\n";
    std::wcout << L"                    AVAILABLE OPERATIONS\n";
    std::wcout << L"=========================================================\n";

    if (isPatched) {
        std::wcout << L"[1] Disable HDR (restore IsHdrSourceModePinned)\n";
    } else {
        std::wcout << L"[1] Enable HDR (patch IsHdrSourceModePinned)\n";
    }

    std::wcout << L"[2] Check status\n";
    std::wcout << L"[3] Exit\n";
    std::wcout << L"=========================================================\n";
    std::wcout << L"\nSelect option: ";
}

} // namespace UI
