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

// =============================================================
// DEFINITIONS
// =============================================================

// Device names
#define DEVICE_NAME L"\\Device\\ReadWriteDriver"
#define SYMBOLIC_NAME L"\\DosDevices\\ReadWriteDriver"

// IOCTL Definitions
#define IOCTL_READWRITE_DRIVER_READ  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_READWRITE_DRIVER_WRITE CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_READWRITE_DRIVER_BULK  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)

// Safety limits
#define MAX_TRANSFER_SIZE (PAGE_SIZE * 256)  // 1MB
#define MAX_BULK_OPERATIONS 64

// Communication structures
typedef struct _KERNEL_READWRITE_REQUEST {
    ULONG ProcessId;        // Target PID
    ULONG64 Address;        // Target Address
    ULONG64 Buffer;         // User Mode Buffer
    SIZE_T Size;
    BOOLEAN Write;          // TRUE = Write, FALSE = Read
    NTSTATUS Status;
} KERNEL_READWRITE_REQUEST, *PKERNEL_READWRITE_REQUEST;

// Struktura dla operacji zbiorczych
typedef struct _KERNEL_BULK_OPERATION {
    ULONG Count;
    KERNEL_READWRITE_REQUEST Operations[MAX_BULK_OPERATIONS];
} KERNEL_BULK_OPERATION, *PKERNEL_BULK_OPERATION;