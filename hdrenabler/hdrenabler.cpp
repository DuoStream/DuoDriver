#include "UserInterface.h"
#include "HdrCore.h"
#include <iostream>
#include <algorithm>

int HandleEnableCommand(HdrEnabler& enabler);
int HandleDisableCommand(HdrEnabler& enabler);
int HandleStatusCommand(HdrEnabler& enabler);
void ShowHelp();

int wmain(int argc, wchar_t* argv[]) {
    UI::DisplayBanner();

    HdrEnabler enabler;

    if (!enabler.Initialize()) {
        std::wcout << L"\n[FAILED] Initialization failed\n";
        UI::WaitForAnyKey(L"\nPress any key to exit...");
        return 1;
    }

    if (argc > 1) {
        std::wstring cmd = argv[1];
        std::transform(cmd.begin(), cmd.end(), cmd.begin(), ::towlower);

        if (cmd == L"enable" || cmd == L"on" || cmd == L"patch") {
            return HandleEnableCommand(enabler);
        }
        else if (cmd == L"disable" || cmd == L"off" || cmd == L"unpatch") {
            return HandleDisableCommand(enabler);
        }
        else if (cmd == L"status") {
            return HandleStatusCommand(enabler);
        }
        else if (cmd == L"help" || cmd == L"/?") {
            ShowHelp();
            return 0;
        }
        else {
            std::wcout << L"[-] Unknown command: " << argv[1] << L"\n\n";
            ShowHelp();
            return 1;
        }
    }

    bool isPatched = false;
    if (!enabler.CheckHdrStatus(isPatched)) {
        std::wcout << L"\n[FAILED] Could not check HDR status\n";
        UI::WaitForAnyKey(L"\nPress any key to exit...");
        return 1;
    }

    while (true) {
        UI::DisplayMenu(isPatched);

        int choice = 0;
        std::wcin >> choice;
        UI::ClearInputBuffer();

        if (choice == 1) {
            if (isPatched) {
                if (enabler.DisableHdr()) {
                    std::wcout << L"\n[SUCCESS] HDR disabled (IsHdrSourceModePinned restored)\n";
                    isPatched = false;
                } else {
                    std::wcout << L"\n[FAILED] Disable failed\n";
                }
            } else {
                if (enabler.EnableHdr()) {
                    std::wcout << L"\n[SUCCESS] HDR enabled (IsHdrSourceModePinned patched)\n";
                    isPatched = true;
                } else {
                    std::wcout << L"\n[FAILED] Enable failed\n";
                }
            }
            UI::WaitForAnyKey(L"\nPress any key...");
        }
        else if (choice == 2) {
            if (!enabler.CheckHdrStatus(isPatched)) {
                std::wcout << L"\n[FAILED] Could not check status\n";
            }
            UI::WaitForAnyKey(L"\nPress any key...");
        }
        else if (choice == 3) {
            break;
        }
        else {
            std::wcout << L"\n[!] Invalid option\n";
        }
    }

    return 0;
}

int HandleEnableCommand(HdrEnabler& enabler) {
    return enabler.EnableHdr() ? 0 : 1;
}

int HandleDisableCommand(HdrEnabler& enabler) {
    return enabler.DisableHdr() ? 0 : 1;
}

int HandleStatusCommand(HdrEnabler& enabler) {
    bool isPatched = false;
    if (enabler.CheckHdrStatus(isPatched)) return 0;
    return 1;
}

void ShowHelp() {
    std::wcout << L"\nUsage: hdrenabler [command]\n\n";
    std::wcout << L"Commands:\n";
    std::wcout << L"  (no args)              Interactive mode\n";
    std::wcout << L"  enable | on | patch    Patch IsHdrSourceModePinned to return true\n";
    std::wcout << L"  disable | off | unpatch Restore IsHdrSourceModePinned to original\n";
    std::wcout << L"  status                 Check current patch status\n";
}
