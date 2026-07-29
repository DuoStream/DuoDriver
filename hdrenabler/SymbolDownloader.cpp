#include "SymbolDownloader.h"
#include <iostream>
#include <shlwapi.h>
#include <shlobj.h>

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shell32.lib")

SymbolDownloader::SymbolDownloader(const std::wstring& cachePath, bool useProgramData)
    : symbolServer(L"https://msdl.microsoft.com/download/symbols"),
      useProgramDataStore(useProgramData) {

    if (!cachePath.empty()) {
        symbolCachePath = cachePath;
        useProgramDataStore = false;
    } else if (useProgramDataStore) {
        symbolCachePath = GetProgramDataSymbolPath();
    } else {
        WCHAR exePath[MAX_PATH];
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        PathRemoveFileSpecW(exePath);
        symbolCachePath = std::wstring(exePath) + L"\\symbols";
    }
}

std::wstring SymbolDownloader::GetProgramDataSymbolPath() {
    WCHAR progData[MAX_PATH];
    if (SHGetFolderPathW(nullptr, CSIDL_COMMON_APPDATA, nullptr, 0, progData) == S_OK) {
        return std::wstring(progData) + L"\\dbg\\sym";
    }
    return L"C:\\ProgramData\\dbg\\sym";
}

bool SymbolDownloader::Initialize() {
    if (!EnsureSymbolCache()) {
        return false;
    }

    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_DEBUG);

    if (!SymInitializeW(GetCurrentProcess(), symbolCachePath.c_str(), FALSE)) {
        std::wcout << L"[-] Failed to initialize symbol handler (error: " << GetLastError() << L")\n";
        return false;
    }

    std::wcout << L"[+] Symbol handler initialized\n";
    std::wcout << L"    Cache: " << symbolCachePath << L"\n";
    return true;
}

bool SymbolDownloader::EnsureSymbolCache() {
    DWORD attrib = GetFileAttributesW(symbolCachePath.c_str());

    if (attrib == INVALID_FILE_ATTRIBUTES) {
        if (SHCreateDirectoryExW(nullptr, symbolCachePath.c_str(), nullptr) != ERROR_SUCCESS) {
            std::wcout << L"[-] Failed to create symbol cache directory: " << symbolCachePath << L"\n";
            return false;
        }
        std::wcout << L"[+] Created symbol cache directory: " << symbolCachePath << L"\n";
    }

    return true;
}

std::pair<std::wstring, std::wstring>
SymbolDownloader::GetPdbInfoFromPe(const std::wstring& pePath)
{
    HANDLE hFile = CreateFileW(
        pePath.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);

    if (hFile == INVALID_HANDLE_VALUE)
        return { L"", L"" };

    HANDLE hMapping = CreateFileMappingW(
        hFile,
        nullptr,
        PAGE_READONLY,
        0,
        0,
        nullptr);

    if (!hMapping)
    {
        CloseHandle(hFile);
        return { L"", L"" };
    }

    BYTE* base = reinterpret_cast<BYTE*>(
        MapViewOfFile(hMapping, FILE_MAP_READ, 0, 0, 0));

    if (!base)
    {
        CloseHandle(hMapping);
        CloseHandle(hFile);
        return { L"", L"" };
    }

    auto cleanup = [&]()
        {
            UnmapViewOfFile(base);
            CloseHandle(hMapping);
            CloseHandle(hFile);
        };

    IMAGE_NT_HEADERS* nt =
        ImageNtHeader(base);

    if (!nt)
    {
        cleanup();
        return { L"", L"" };
    }

    IMAGE_DATA_DIRECTORY& dbgDir =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];

    if (dbgDir.VirtualAddress == 0 || dbgDir.Size == 0)
    {
        cleanup();
        return { L"", L"" };
    }

    IMAGE_DEBUG_DIRECTORY* debug =
        reinterpret_cast<IMAGE_DEBUG_DIRECTORY*>(
            ImageRvaToVa(
                nt,
                base,
                dbgDir.VirtualAddress,
                nullptr));

    if (!debug)
    {
        cleanup();
        return { L"", L"" };
    }

    DWORD count = dbgDir.Size / sizeof(IMAGE_DEBUG_DIRECTORY);

    for (DWORD i = 0; i < count; i++)
    {
        if (debug[i].Type != IMAGE_DEBUG_TYPE_CODEVIEW)
            continue;

        BYTE* cvData =
            reinterpret_cast<BYTE*>(
                ImageRvaToVa(
                    nt,
                    base,
                    debug[i].AddressOfRawData,
                    nullptr));

        if (!cvData)
            continue;

        struct CV_INFO_PDB70
        {
            DWORD Signature;
            GUID Guid;
            DWORD Age;
            char PdbFileName[1];
        };

        auto* cv = reinterpret_cast<CV_INFO_PDB70*>(cvData);

        if (cv->Signature != 'SDSR') // "RSDS"
            continue;

        wchar_t guidStr[64];

        swprintf_s(
            guidStr,
            L"%08X%04X%04X"
            L"%02X%02X"
            L"%02X%02X%02X%02X%02X%02X"
            L"%X",
            cv->Guid.Data1,
            cv->Guid.Data2,
            cv->Guid.Data3,
            cv->Guid.Data4[0],
            cv->Guid.Data4[1],
            cv->Guid.Data4[2],
            cv->Guid.Data4[3],
            cv->Guid.Data4[4],
            cv->Guid.Data4[5],
            cv->Guid.Data4[6],
            cv->Guid.Data4[7],
            cv->Age);

        std::string pdbPath(cv->PdbFileName);

        size_t pos = pdbPath.find_last_of("\\/");
        if (pos != std::string::npos)
            pdbPath = pdbPath.substr(pos + 1);

        std::wstring pdbName(
            pdbPath.begin(),
            pdbPath.end());

        cleanup();
        return { pdbName, guidStr };
    }

    cleanup();
    return { L"", L"" };
}

bool SymbolDownloader::DownloadFile(const std::wstring& url, const std::wstring& outputPath) {
    std::wcout << L"[*] Downloading from: " << url << L"\n";

    URL_COMPONENTSW urlParts = { sizeof(urlParts) };
    wchar_t host[256], path[1024];
    urlParts.lpszHostName = host;
    urlParts.dwHostNameLength = _countof(host);
    urlParts.lpszUrlPath = path;
    urlParts.dwUrlPathLength = _countof(path);

    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &urlParts)) {
        std::wcout << L"[-] Failed to parse URL\n";
        return false;
    }

    HINTERNET hSession = WinHttpOpen(L"SymbolDownloader/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) {
        std::wcout << L"[-] Failed to open HTTP session\n";
        return false;
    }

    HINTERNET hConnect = WinHttpConnect(hSession, host, urlParts.nPort, 0);
    if (!hConnect) {
        std::wcout << L"[-] Failed to connect to server\n";
        WinHttpCloseHandle(hSession);
        return false;
    }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path, nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, urlParts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    if (!hRequest) {
        std::wcout << L"[-] Failed to open HTTP request\n";
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        std::wcout << L"[-] Failed to send HTTP request\n";
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    if (!WinHttpReceiveResponse(hRequest, nullptr)) {
        std::wcout << L"[-] Failed to receive HTTP response\n";
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    DWORD statusCode = 0;
    DWORD statusCodeSize = sizeof(statusCode);
    WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusCodeSize, WINHTTP_NO_HEADER_INDEX);

    if (statusCode != 200) {
        std::wcout << L"[-] HTTP error: " << statusCode << L"\n";
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    HANDLE hFile = CreateFileW(outputPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        std::wcout << L"[-] Failed to create output file\n";
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    BYTE buffer[8192];
    DWORD bytesRead = 0, bytesWritten = 0;
    DWORD totalBytes = 0;

    while (WinHttpReadData(hRequest, buffer, sizeof(buffer), &bytesRead) && bytesRead > 0) {
        if (!WriteFile(hFile, buffer, bytesRead, &bytesWritten, nullptr)) {
            std::wcout << L"[-] Failed to write to file\n";
            CloseHandle(hFile);
            WinHttpCloseHandle(hRequest);
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            return false;
        }
        totalBytes += bytesWritten;
    }

    std::wcout << L"[+] Downloaded " << totalBytes << L" bytes\n";

    CloseHandle(hFile);
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    return true;
}

bool SymbolDownloader::DownloadPdb(const std::wstring& modulePath) {
    std::wcout << L"[*] Extracting PDB information from: " << modulePath << L"\n";

    auto [pdbName, guid] = GetPdbInfoFromPe(modulePath);

    if (guid.empty() || pdbName.empty()) {
        std::wcout << L"[-] Failed to extract PDB info from PE file\n";
        return false;
    }

    std::wcout << L"[+] PDB Name: " << pdbName << L"\n";
    std::wcout << L"[+] PDB GUID: " << guid << L"\n";

    std::wstring url = symbolServer + L"/" + pdbName + L"/" + guid + L"/" + pdbName;

    std::wstring localDir = symbolCachePath + L"\\" + pdbName + L"\\" + guid;
    std::wstring localPath = localDir + L"\\" + pdbName;

    if (GetFileAttributesW(localPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
        std::wcout << L"[+] PDB already exists in cache: " << localPath << L"\n";
        return true;
    }

    SHCreateDirectoryExW(nullptr, localDir.c_str(), nullptr);

    if (!DownloadFile(url, localPath)) {
        std::wcout << L"[-] Failed to download PDB file\n";
        return false;
    }

    std::wcout << L"[+] PDB downloaded successfully: " << localPath << L"\n";
    return true;
}

bool SymbolDownloader::DownloadSymbolsForModule(const std::wstring& modulePath) {
    std::wcout << L"[*] Resolving symbols for: " << modulePath << L"\n";

    auto [pdbName, guid] = GetPdbInfoFromPe(modulePath);

    if (guid.empty() || pdbName.empty()) {
        std::wcout << L"[-] Failed to read PE debug info\n";
        return false;
    }

    std::wstring localPath = symbolCachePath + L"\\" + pdbName + L"\\" + guid + L"\\" + pdbName;

    if (GetFileAttributesW(localPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::wcout << L"[*] PDB not in cache, downloading from Microsoft...\n";
        if (!DownloadPdb(modulePath)) {
            std::wcout << L"[-] Failed to download PDB\n";
            return false;
        }
    } else {
        std::wcout << L"[+] Using PDB from cache: " << localPath << L"\n";
    }

    if (GetFileAttributesW(localPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::wcout << L"[-] PDB file does not exist after download attempt\n";
        return false;
    }

    return true;
}

std::optional<uint64_t> SymbolDownloader::GetSymbolOffset(const std::wstring& moduleName, const std::wstring& symbolName) {
    std::wcout << L"[*] Looking up symbol: " << symbolName << L"\n";

    std::wstring moduleBaseName = moduleName;
    size_t pos = moduleBaseName.find_last_of(L"\\/");
    if (pos != std::wstring::npos) {
        moduleBaseName = moduleBaseName.substr(pos + 1);
    }

    DWORD64 baseAddr = SymLoadModuleExW(GetCurrentProcess(), nullptr, moduleName.c_str(), nullptr, 0x10000000, 0, nullptr, 0);
    if (!baseAddr) {
        std::wcout << L"[-] Failed to load module symbols (error: " << GetLastError() << L")\n";
        return std::nullopt;
    }

    BYTE buffer[sizeof(SYMBOL_INFOW) + MAX_SYM_NAME * sizeof(wchar_t)];
    PSYMBOL_INFOW pSymbol = (PSYMBOL_INFOW)buffer;
    pSymbol->SizeOfStruct = sizeof(SYMBOL_INFOW);
    pSymbol->MaxNameLen = MAX_SYM_NAME;

    std::wstring qualified = moduleBaseName + L"!" + symbolName;
    bool found = SymFromNameW(GetCurrentProcess(), qualified.c_str(), pSymbol);

    if (!found) {
        found = SymFromNameW(GetCurrentProcess(), symbolName.c_str(), pSymbol);
    }

    if (!found) {
        std::wcout << L"[-] Symbol not found: " << symbolName << L" (error: " << GetLastError() << L")\n";
        SymUnloadModule64(GetCurrentProcess(), baseAddr);
        return std::nullopt;
    }

    uint64_t offset = pSymbol->Address - baseAddr;
    std::wcout << L"[+] Symbol found: " << symbolName << L" at offset 0x" << std::hex << offset << std::dec << L"\n";

    SymUnloadModule64(GetCurrentProcess(), baseAddr);
    return offset;
}
