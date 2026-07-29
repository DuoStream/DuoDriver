#include <Windows.h>
#include <iostream>
#include <vector>
#include <string>
#include <cstdio>
#include <io.h>
#include <fcntl.h>
#include <fci.h>
#include "resource.h"

#pragma comment(lib, "cabinet.lib")

static constexpr BYTE XOR_KEY[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE, 0xBA, 0xBE };
static constexpr size_t XOR_KEY_LEN = sizeof(XOR_KEY);

// ============================================================================
// UTILITY
// ============================================================================

static std::string ToNarrow(const std::wstring& w) {
    if (w.empty()) return {};
    int sz = WideCharToMultiByte(CP_ACP, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string r(sz, 0);
    WideCharToMultiByte(CP_ACP, 0, w.c_str(), (int)w.size(), &r[0], sz, nullptr, nullptr);
    return r;
}

static std::vector<BYTE> ReadFileBytes(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};

    DWORD sz = GetFileSize(h, nullptr);
    if (sz == INVALID_FILE_SIZE) {
        CloseHandle(h);
        return {};
    }

    std::vector<BYTE> data(sz);
    DWORD bytesRead = 0;
    BOOL ok = ReadFile(h, data.data(), sz, &bytesRead, nullptr);
    CloseHandle(h);

    return (ok && bytesRead == sz) ? data : std::vector<BYTE>{};
}

static bool WriteFileBytes(const std::wstring& path, const void* data, size_t size) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    DWORD written = 0;
    BOOL ok = WriteFile(h, data, (DWORD)size, &written, nullptr);
    CloseHandle(h);

    return ok && written == size;
}

// ============================================================================
// FCI CALLBACKS (SDK 10.0.28000 compatible)
// ============================================================================

static void FAR *DIAMONDAPI fci_alloc(ULONG cb) {
    return malloc(cb);
}

static void DIAMONDAPI fci_free(void HUGE *memory) {
    free(memory);
}

static int DIAMONDAPI fci_filePlaced(PCCAB pccab, LPSTR pszFile, long cbFile, BOOL fContinuation, void FAR *pv) {
    return 0;
}

static INT_PTR DIAMONDAPI fci_open(LPSTR pszFile, int oflag, int pmode, int FAR *err, void FAR *pv) {
    int fh = -1;
    _sopen_s(&fh, pszFile, oflag | _O_BINARY, _SH_DENYNO, pmode);
    return fh;
}

static UINT DIAMONDAPI fci_read(INT_PTR hf, void FAR *memory, UINT cb, int FAR *err, void FAR *pv) {
    return (UINT)_read((int)hf, memory, cb);
}

static UINT DIAMONDAPI fci_write(INT_PTR hf, void FAR *memory, UINT cb, int FAR *err, void FAR *pv) {
    return (UINT)_write((int)hf, memory, cb);
}

static int DIAMONDAPI fci_close(INT_PTR hf, int FAR *err, void FAR *pv) {
    return _close((int)hf);
}

static long DIAMONDAPI fci_seek(INT_PTR hf, long dist, int seektype, int FAR *err, void FAR *pv) {
    return (long)_lseek((int)hf, dist, seektype);
}

static int DIAMONDAPI fci_delete(LPSTR pszFile, int FAR *err, void FAR *pv) {
    return _unlink(pszFile);
}

static BOOL DIAMONDAPI fci_getTempFile(char *pszTempName, int cbTempName, void FAR *pv) {
    char tempDir[MAX_PATH];
    GetTempPathA(MAX_PATH, tempDir);
    if (GetTempFileNameA(tempDir, "fci", 0, pszTempName) == 0)
        return FALSE;
    DeleteFileA(pszTempName);
    return TRUE;
}

static BOOL DIAMONDAPI fci_getNextCabinet(PCCAB pccab, ULONG cbPrevCab, void FAR *pv) {
    return TRUE;
}

static long DIAMONDAPI fci_status(UINT typeStatus, ULONG cb1, ULONG cb2, void FAR *pv) {
    return 0;
}

static INT_PTR DIAMONDAPI fci_getOpenInfo(LPSTR pszName, USHORT *pdate, USHORT *ptime, USHORT *pattribs, int FAR *err, void FAR *pv) {
    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA(pszName, &fd);
    if (hFind == INVALID_HANDLE_VALUE) return -1;

    SYSTEMTIME st;
    FILETIME ft = fd.ftLastWriteTime;
    FileTimeToLocalFileTime(&ft, &ft);
    FileTimeToSystemTime(&ft, &st);

    *pdate = (USHORT)(((st.wMonth & 0x0F) << 9) | ((st.wDay & 0x1F) << 5) | (st.wYear & 0x1F));
    *ptime = (USHORT)(((st.wHour & 0x1F) << 11) | ((st.wMinute & 0x3F) << 5) | ((st.wSecond / 2) & 0x1F));
    *pattribs = (USHORT)(fd.dwFileAttributes & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM));

    FindClose(hFind);

    int fh = -1;
    _sopen_s(&fh, pszName, _O_RDONLY | _O_BINARY, _SH_DENYNO, 0);
    return fh;
}

// ============================================================================
// CAB COMPRESSION
// ============================================================================

static std::vector<BYTE> CompressToCabinet(const std::vector<BYTE>& driverData, const std::wstring& tempDir) {
    std::string tempDirNarrow = ToNarrow(tempDir);
    std::string tempDriverNarrow = tempDirNarrow + "\\input.sys";
    std::string cabPath = tempDirNarrow + "\\";

    // Write driver data to temp file for FCI
    if (!WriteFileBytes(tempDir + L"\\input.sys", driverData.data(), driverData.size())) {
        std::wcout << L"[-] Failed to write temp driver file\n";
        return {};
    }

    ERF erf{};

    CCAB ccab{};
    ccab.cb = 0x7FFFFFFF;
    ccab.cbFolderThresh = 0x7FFFFFFF;
    ccab.iCab = 1;
    ccab.iDisk = 0;
    ccab.fFailOnIncompressible = FALSE;
    ccab.setID = 0x1234;
    strncpy_s(ccab.szCab, "output.cab", _TRUNCATE);
    strncpy_s(ccab.szCabPath, cabPath.c_str(), _TRUNCATE);
    ccab.szDisk[0] = '\0';

    HFCI hfci = FCICreate(&erf, fci_filePlaced, fci_alloc, fci_free,
                          fci_open, fci_read, fci_write, fci_close,
                          fci_seek, fci_delete, fci_getTempFile,
                          &ccab, nullptr);
    if (!hfci) {
        std::wcout << L"[-] FCICreate failed (error: " << erf.erfOper << L")\n";
        return {};
    }

    char srcFile[256];
    char dstFile[256];
    strncpy_s(srcFile, tempDriverNarrow.c_str(), _TRUNCATE);
    strncpy_s(dstFile, "driver.sys", _TRUNCATE);

    if (!FCIAddFile(hfci, srcFile, dstFile,
                    FALSE, fci_getNextCabinet, fci_status, fci_getOpenInfo, tcompTYPE_MSZIP)) {
        std::wcout << L"[-] FCIAddFile failed (error: " << erf.erfOper << L")\n";
        FCIDestroy(hfci);
        return {};
    }

    if (!FCIFlushCabinet(hfci, FALSE, fci_getNextCabinet, fci_status)) {
        std::wcout << L"[-] FCIFlushCabinet failed (error: " << erf.erfOper << L")\n";
        FCIDestroy(hfci);
        return {};
    }

    FCIDestroy(hfci);

    // Read the generated cabinet file
    std::wstring cabFilePath = tempDir + L"\\output.cab";
    auto cabData = ReadFileBytes(cabFilePath);

    // Clean up temp files
    DeleteFileW((tempDir + L"\\input.sys").c_str());
    DeleteFileW(cabFilePath.c_str());

    return cabData;
}

// ============================================================================
// MAIN
// ============================================================================

static void ShowHelp() {
    std::wcout << L"\nUsage: drvcab <input.sys> <output.bin>\n\n";
    std::wcout << L"Compresses an unsigned driver into an encrypted resource for hdrenabler.\n\n";
    std::wcout << L"Arguments:\n";
    std::wcout << L"  input.sys      Path to the unsigned driver file (.sys)\n";
    std::wcout << L"  output.bin     Path for the output resource file (.bin)\n\n";
    std::wcout << L"The output file format is: [icon bytes] + [XOR-encrypted CAB]\n";
    std::wcout << L"Embed it as RCDATA in your .rc file for hdrenabler/drvloader.\n";
}

int wmain(int argc, wchar_t* argv[]) {
    std::wcout << L"\n";
    std::wcout << L"+-----------------------------------------------+\n";
    std::wcout << L"|  Driver Cabinet Tool - Resource Packager      |\n";
    std::wcout << L"+-----------------------------------------------+\n";
    std::wcout << L"\n";

    if (argc < 3) {
        ShowHelp();
        return 1;
    }

    std::wstring inputPath = argv[1];
    std::wstring outputPath = argv[2];

    if (GetFileAttributesW(inputPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::wcout << L"[-] Input file not found: " << inputPath << L"\n";
        return 1;
    }

    std::wcout << L"[*] Reading driver: " << inputPath << L"\n";
    auto driverData = ReadFileBytes(inputPath);
    if (driverData.empty()) {
        std::wcout << L"[-] Failed to read driver file\n";
        return 1;
    }
    std::wcout << L"[+] Driver size: " << driverData.size() << L" bytes\n";

    if (driverData.size() < 2 || driverData[0] != 'M' || driverData[1] != 'Z') {
        std::wcout << L"[-] Input file is not a valid PE (no MZ signature)\n";
        return 1;
    }

    std::wcout << L"[*] Loading embedded icon...\n";
    HRSRC hRes = FindResourceW(nullptr, MAKEINTRESOURCEW(IDR_KVC_ICON), RT_RCDATA);
    if (!hRes) {
        std::wcout << L"[-] Failed to find embedded icon resource\n";
        return 1;
    }
    HGLOBAL hData = LoadResource(nullptr, hRes);
    if (!hData) {
        std::wcout << L"[-] Failed to load icon resource\n";
        return 1;
    }
    void* pRes = LockResource(hData);
    DWORD resSize = SizeofResource(nullptr, hRes);
    if (!pRes || resSize == 0) {
        std::wcout << L"[-] Failed to lock icon resource\n";
        return 1;
    }
    std::vector<BYTE> iconData(static_cast<BYTE*>(pRes), static_cast<BYTE*>(pRes) + resSize);
    std::wcout << L"[+] Icon size: " << iconData.size() << L" bytes\n";

    WCHAR tempPath[MAX_PATH];
    GetTempPathW(MAX_PATH, tempPath);
    WCHAR tempDir[MAX_PATH];
    GetTempFileNameW(tempPath, L"dvc", 0, tempDir);
    DeleteFileW(tempDir);
    CreateDirectoryW(tempDir, nullptr);

    std::wstring tempDirStr = tempDir;

    std::wcout << L"[*] Compressing driver to cabinet (MSZIP)...\n";
    auto cabData = CompressToCabinet(driverData, tempDirStr);
    RemoveDirectoryW(tempDir);

    if (cabData.empty()) {
        std::wcout << L"[-] Cabinet compression failed\n";
        return 1;
    }
    std::wcout << L"[+] Cabinet size: " << cabData.size() << L" bytes\n";

    std::wcout << L"[*] XOR encrypting cabinet...\n";
    std::vector<BYTE> encryptedCab = cabData;
    for (size_t i = 0; i < encryptedCab.size(); i++) {
        encryptedCab[i] ^= XOR_KEY[i % XOR_KEY_LEN];
    }
    std::wcout << L"[+] Encrypted size: " << encryptedCab.size() << L" bytes\n";

    std::vector<BYTE> output;
    output.reserve(iconData.size() + encryptedCab.size());
    output.insert(output.end(), iconData.begin(), iconData.end());
    output.insert(output.end(), encryptedCab.begin(), encryptedCab.end());

    std::wcout << L"[*] Writing output: " << outputPath << L"\n";
    if (!WriteFileBytes(outputPath, output.data(), output.size())) {
        std::wcout << L"[-] Failed to write output file\n";
        return 1;
    }

    std::wcout << L"\n[+] Resource file created successfully\n";
    std::wcout << L"    Total size:    " << output.size() << L" bytes\n";
    std::wcout << L"    Icon prefix:   " << iconData.size() << L" bytes\n";
    std::wcout << L"    Encrypted CAB: " << encryptedCab.size() << L" bytes\n";
    std::wcout << L"\n    Embed as RCDATA in your .rc file.\n";

    return 0;
}
