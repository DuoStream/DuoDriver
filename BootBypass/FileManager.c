#include "FileManager.h"

typedef struct _FILE_DIRECTORY_INFORMATION {
    ULONG NextEntryOffset;
    ULONG FileIndex;
    LARGE_INTEGER CreationTime;
    LARGE_INTEGER LastAccessTime;
    LARGE_INTEGER LastWriteTime;
    LARGE_INTEGER ChangeTime;
    LARGE_INTEGER EndOfFile;
    LARGE_INTEGER AllocationSize;
    ULONG FileAttributes;
    ULONG FileNameLength;
    WCHAR FileName[1];
} FILE_DIRECTORY_INFORMATION, *PFILE_DIRECTORY_INFORMATION;

#define FileDirectoryInformation 1
#define FILE_ATTRIBUTE_DIRECTORY 0x00000010

NTSTATUS ExecuteRename(PINI_ENTRY entry) {
    UNICODE_STRING usSourcePath, usTargetPath;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    HANDLE hFile;
    NTSTATUS status;
    UCHAR buffer[512];
    PFILE_RENAME_INFORMATION pRename = (PFILE_RENAME_INFORMATION)buffer;

    RtlInitUnicodeString(&usSourcePath, entry->SourcePath);
    RtlInitUnicodeString(&usTargetPath, entry->TargetPath);

    InitializeObjectAttributes(&oa, &usTargetPath, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    status = NtOpenFile(&hFile, FILE_READ_DATA | SYNCHRONIZE, &oa, &iosb, FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_SYNCHRONOUS_IO_NONALERT);
    if (NT_SUCCESS(status)) {
        NtClose(hFile);
        InitializeObjectAttributes(&oa, &usSourcePath, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
        status = NtOpenFile(&hFile, FILE_READ_DATA | SYNCHRONIZE, &oa, &iosb, FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_SYNCHRONOUS_IO_NONALERT);
        if (!NT_SUCCESS(status)) { DisplayMessage(L"SKIPPED: Rename complete\r\n"); return STATUS_SUCCESS; }
        NtClose(hFile);
    }

    InitializeObjectAttributes(&oa, &usSourcePath, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    status = NtOpenFile(&hFile, DELETE | SYNCHRONIZE, &oa, &iosb, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, FILE_OPEN_FOR_BACKUP_INTENT | FILE_SYNCHRONOUS_IO_NONALERT);
    if (!NT_SUCCESS(status)) return status;

    memset_impl(buffer, 0, sizeof(FILE_RENAME_INFORMATION) + usTargetPath.Length);
    pRename->ReplaceIfExists = entry->ReplaceIfExists ? 1 : 0;
    pRename->FileNameLength = usTargetPath.Length;
    for (ULONG i = 0; i < (ULONG)usTargetPath.Length / sizeof(WCHAR); i++) pRename->FileName[i] = usTargetPath.Buffer[i];

    status = NtSetInformationFile(hFile, &iosb, pRename, sizeof(FILE_RENAME_INFORMATION) + usTargetPath.Length - 2, 10);
    NtClose(hFile);
    if (NT_SUCCESS(status)) DisplayMessage(L"SUCCESS: File renamed\r\n");
    return status;
}

BOOLEAN IsDotDirectory(PWSTR name, ULONG nameLen) {
    if (nameLen == sizeof(WCHAR) && name[0] == L'.') return TRUE;
    if (nameLen == 2 * sizeof(WCHAR) && name[0] == L'.' && name[1] == L'.') return TRUE;
    return FALSE;
}

NTSTATUS DeleteDirectoryRecursive(PUNICODE_STRING dirPath) {
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    HANDLE hDir;
    NTSTATUS status;
    UCHAR buffer[4096];
    PFILE_DIRECTORY_INFORMATION dirInfo;
    BOOLEAN firstQuery = TRUE;

    InitializeObjectAttributes(&oa, dirPath, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    status = NtOpenFile(&hDir, FILE_LIST_DIRECTORY | DELETE | SYNCHRONIZE, &oa, &iosb, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT | FILE_OPEN_FOR_BACKUP_INTENT);
    if (!NT_SUCCESS(status)) return status;

    while (TRUE) {
        memset_impl(buffer, 0, sizeof(buffer));
        status = NtQueryDirectoryFile(hDir, NULL, NULL, NULL, &iosb, buffer, sizeof(buffer), FileDirectoryInformation, FALSE, NULL, firstQuery);
        if (status == 0x80000006 || !NT_SUCCESS(status)) break;
        firstQuery = FALSE;
        dirInfo = (PFILE_DIRECTORY_INFORMATION)buffer;

        while (TRUE) {
            if (!IsDotDirectory(dirInfo->FileName, dirInfo->FileNameLength)) {
                WCHAR fullPath[MAX_PATH_LEN];
                UNICODE_STRING usFullPath;
                wcscpy(fullPath, dirPath->Buffer); wcscat(fullPath, L"\\");
                ULONG fnChars = dirInfo->FileNameLength / sizeof(WCHAR);
                ULONG curLen = (ULONG)wcslen(fullPath); // <--- POPRAWKA RZUTOWANIA
                for (ULONG i = 0; i < fnChars && (curLen + i) < (MAX_PATH_LEN - 1); i++) fullPath[curLen + i] = dirInfo->FileName[i];
                fullPath[curLen + fnChars] = 0;
                RtlInitUnicodeString(&usFullPath, fullPath);

                if (dirInfo->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) DeleteDirectoryRecursive(&usFullPath);

                OBJECT_ATTRIBUTES oaItem;
                HANDLE hItem;
                InitializeObjectAttributes(&oaItem, &usFullPath, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
                status = NtOpenFile(&hItem, DELETE | SYNCHRONIZE, &oaItem, &iosb, FILE_SHARE_DELETE, FILE_OPEN_FOR_BACKUP_INTENT | FILE_SYNCHRONOUS_IO_NONALERT);
                if (NT_SUCCESS(status)) {
                    FILE_DISPOSITION_INFORMATION dispInfo; dispInfo.DeleteFile = TRUE;
                    NtSetInformationFile(hItem, &iosb, &dispInfo, sizeof(dispInfo), 13);
                    NtClose(hItem);
                }
            }
            if (dirInfo->NextEntryOffset == 0) break;
            dirInfo = (PFILE_DIRECTORY_INFORMATION)((UCHAR*)dirInfo + dirInfo->NextEntryOffset);
        }
    }
    NtClose(hDir);
    InitializeObjectAttributes(&oa, dirPath, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    status = NtOpenFile(&hDir, DELETE | SYNCHRONIZE, &oa, &iosb, FILE_SHARE_DELETE, FILE_DIRECTORY_FILE | FILE_OPEN_FOR_BACKUP_INTENT | FILE_SYNCHRONOUS_IO_NONALERT);
    if (NT_SUCCESS(status)) {
        FILE_DISPOSITION_INFORMATION dispInfo; dispInfo.DeleteFile = TRUE;
        NtSetInformationFile(hDir, &iosb, &dispInfo, sizeof(dispInfo), 13);
        NtClose(hDir);
    }
    return STATUS_SUCCESS;
}

NTSTATUS ExecuteDelete(PINI_ENTRY entry) {
    UNICODE_STRING usPath;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    HANDLE hFile;
    NTSTATUS status;
    FILE_DISPOSITION_INFORMATION dispInfo;
    RtlInitUnicodeString(&usPath, entry->DeletePath);
    InitializeObjectAttributes(&oa, &usPath, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    status = NtOpenFile(&hFile, DELETE | FILE_READ_ATTRIBUTES | SYNCHRONIZE, &oa, &iosb, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, FILE_OPEN_FOR_BACKUP_INTENT | FILE_SYNCHRONOUS_IO_NONALERT);
    if (!NT_SUCCESS(status)) return status;

    FILE_STANDARD_INFORMATION fileInfo;
    memset_impl(&fileInfo, 0, sizeof(fileInfo));
    status = NtQueryInformationFile(hFile, &iosb, &fileInfo, sizeof(fileInfo), FileStandardInformation);

    if (NT_SUCCESS(status) && fileInfo.Directory) {
        NtClose(hFile);
        if (entry->RecursiveDelete) {
            status = DeleteDirectoryRecursive(&usPath);
            if (NT_SUCCESS(status)) DisplayMessage(L"SUCCESS: Tree deleted\r\n");
        } else {
            status = NtOpenFile(&hFile, DELETE | SYNCHRONIZE, &oa, &iosb, FILE_SHARE_DELETE, FILE_DIRECTORY_FILE | FILE_OPEN_FOR_BACKUP_INTENT | FILE_SYNCHRONOUS_IO_NONALERT);
            if (NT_SUCCESS(status)) {
                dispInfo.DeleteFile = TRUE;
                status = NtSetInformationFile(hFile, &iosb, &dispInfo, sizeof(dispInfo), 13);
                NtClose(hFile);
                if (NT_SUCCESS(status)) DisplayMessage(L"SUCCESS: Directory deleted\r\n");
            }
        }
    } else {
        dispInfo.DeleteFile = TRUE;
        status = NtSetInformationFile(hFile, &iosb, &dispInfo, sizeof(dispInfo), 13);
        NtClose(hFile);
        if (NT_SUCCESS(status)) DisplayMessage(L"SUCCESS: File deleted\r\n");
    }
    return status;
}