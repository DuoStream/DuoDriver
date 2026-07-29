#pragma once
#include <ntifs.h>
#include <wdf.h>

// =============================================================
// EXTERNAL PROTOTYPES
// =============================================================
NTKERNELAPI
NTSTATUS
MmCopyVirtualMemory(
    PEPROCESS SourceProcess,
    PVOID SourceAddress,
    PEPROCESS TargetProcess,
    PVOID TargetAddress,
    SIZE_T BufferSize,
    KPROCESSOR_MODE PreviousMode,
    PSIZE_T ReturnSize
    );

NTKERNELAPI
PVOID
MmMapIoSpace(
    PHYSICAL_ADDRESS PhysicalAddress,
    SIZE_T NumberOfBytes,
    MEMORY_CACHING_TYPE CacheType
    );

NTKERNELAPI
VOID
MmUnmapIoSpace(
    PVOID BaseAddress,
    SIZE_T NumberOfBytes
    );

NTKERNELAPI
PHYSICAL_ADDRESS
MmGetPhysicalAddress(
    PVOID BaseAddress
    );

NTSYSAPI
NTSTATUS
NTAPI
ZwQuerySystemInformation(
    ULONG SystemInformationClass,
    PVOID SystemInformation,
    ULONG SystemInformationLength,
    PULONG ReturnLength
    );

// =============================================================
// DEFINITIONS
// =============================================================

// Device names
#define DEVICE_NAME L"\\Device\\DuoDriver"
#define SYMBOLIC_NAME L"\\DosDevices\\DuoDriver"

// Original IOCTLs (OmniDriver compatible)
#define IOCTL_READWRITE_DRIVER_READ    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_READWRITE_DRIVER_WRITE   CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_READWRITE_DRIVER_BULK    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)

// New IOCTLs: read-only kernel memory patching via MmMapIoSpace
#define IOCTL_PATCH_PROTECTED_MEMORY   CTL_CODE(FILE_DEVICE_UNKNOWN, 0x803, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_ENUM_KERNEL_MODULES      CTL_CODE(FILE_DEVICE_UNKNOWN, 0x804, METHOD_BUFFERED, FILE_ANY_ACCESS)

// Safety limits
#define MAX_TRANSFER_SIZE (PAGE_SIZE * 256)
#define MAX_BULK_OPERATIONS 64
#define MAX_PATCH_SIZE 32
#define MAX_MODULES 512

// ZwQuerySystemInformation class
#define SystemModuleInformation 11

// =============================================================
// STRUCTURES
// =============================================================

// --- Original OmniDriver structures ---

typedef struct _KERNEL_READWRITE_REQUEST {
    ULONG ProcessId;
    ULONG64 Address;
    ULONG64 Buffer;
    SIZE_T Size;
    BOOLEAN Write;
    NTSTATUS Status;
} KERNEL_READWRITE_REQUEST, *PKERNEL_READWRITE_REQUEST;

typedef struct _KERNEL_BULK_OPERATION {
    ULONG Count;
    KERNEL_READWRITE_REQUEST Operations[MAX_BULK_OPERATIONS];
} KERNEL_BULK_OPERATION, *PKERNEL_BULK_OPERATION;

// --- New: write to read-only kernel memory ---

typedef struct _WRITE_PROTECTED_REQUEST {
    ULONG64 Address;
    SIZE_T Size;
    // Patch data bytes follow immediately after this header in the input buffer
} WRITE_PROTECTED_REQUEST, *PWRITE_PROTECTED_REQUEST;

// --- New: kernel module enumeration ---

typedef struct _MODULE_INFO_ENTRY {
    ULONG64 BaseAddress;
    ULONG Size;
    CHAR Name[256];
} MODULE_INFO_ENTRY, *PMODULE_INFO_ENTRY;

typedef struct _ENUM_MODULES_RESPONSE {
    ULONG Count;
    MODULE_INFO_ENTRY Modules[1];
} ENUM_MODULES_RESPONSE, *PENUM_MODULES_RESPONSE;

// --- System module information (ZwQuerySystemInformation) ---

#pragma pack(push, 1)

typedef struct _SYSTEM_MODULE_ENTRY {
    HANDLE Section;
    PVOID MappedBase;
    PVOID ImageBase;
    ULONG ImageSize;
    ULONG Flags;
    UCHAR LoadOrderIndex;
    UCHAR InitOrderIndex;
    UCHAR LoadCount;
    UCHAR OffsetToFileName;
    UCHAR FullPathName[256];
} SYSTEM_MODULE_ENTRY, *PSYSTEM_MODULE_ENTRY;

typedef struct _SYSTEM_MODULE_INFORMATION {
    ULONG NumberOfModules;
    SYSTEM_MODULE_ENTRY Modules[1];
} SYSTEM_MODULE_INFORMATION, *PSYSTEM_MODULE_INFORMATION;

#pragma pack(pop)
