#include "DuoDriver.h"

// Global device handle
WDFDEVICE g_Device = NULL;

// =============================================================
// ORIGINAL: MmCopyVirtualMemory-based read/write
// =============================================================

NTSTATUS ReadWriteMemory(PKERNEL_READWRITE_REQUEST Req)
{
    PEPROCESS TargetProcess = NULL;
    PEPROCESS ClientProcess = NULL;
    NTSTATUS status = STATUS_SUCCESS;
    SIZE_T bytesCopied = 0;

    if (!Req || Req->Size == 0 || Req->Size > MAX_TRANSFER_SIZE)
        return STATUS_INVALID_PARAMETER;

    if (Req->Address == 0 || Req->Buffer == 0)
        return STATUS_INVALID_PARAMETER;

    status = PsLookupProcessByProcessId((HANDLE)(ULONG_PTR)Req->ProcessId, &TargetProcess);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    ClientProcess = PsGetCurrentProcess();

    if (Req->Write) {
        status = MmCopyVirtualMemory(
            ClientProcess,
            (PVOID)(ULONG_PTR)Req->Buffer,
            TargetProcess,
            (PVOID)(ULONG_PTR)Req->Address,
            Req->Size,
            KernelMode,
            &bytesCopied
        );
    } else {
        status = MmCopyVirtualMemory(
            TargetProcess,
            (PVOID)(ULONG_PTR)Req->Address,
            ClientProcess,
            (PVOID)(ULONG_PTR)Req->Buffer,
            Req->Size,
            KernelMode,
            &bytesCopied
        );
    }

    if (TargetProcess) {
        ObDereferenceObject(TargetProcess);
    }

    return status;
}

NTSTATUS HandleBulkOperations(PKERNEL_BULK_OPERATION BulkReq)
{
    NTSTATUS status = STATUS_SUCCESS;
    ULONG i;

    if (!BulkReq || BulkReq->Count == 0 || BulkReq->Count > MAX_BULK_OPERATIONS) {
        return STATUS_INVALID_PARAMETER;
    }

    for (i = 0; i < BulkReq->Count; i++) {
        BulkReq->Operations[i].Status = ReadWriteMemory(&BulkReq->Operations[i]);

        if (!NT_SUCCESS(BulkReq->Operations[i].Status) && NT_SUCCESS(status)) {
            status = BulkReq->Operations[i].Status;
        }
    }

    return status;
}

// =============================================================
// NEW: write to read-only kernel memory via MmMapIoSpace
//
// MmMapIoSpace maps a physical address range into kernel virtual
// space with its own PTE, bypassing the original page's read-only
// protection. On x86/x64, hardware cache coherency (MESI) ensures
// other CPUs see the updated bytes without an explicit TLB flush.
// =============================================================

NTSTATUS WriteProtectedKernelMemory(
    PVOID TargetAddress,
    PVOID PatchData,
    SIZE_T PatchSize
)
{
    PHYSICAL_ADDRESS physAddr;
    PVOID mapping;

    if (!TargetAddress || !PatchData || PatchSize == 0 || PatchSize > MAX_PATCH_SIZE)
        return STATUS_INVALID_PARAMETER;

    physAddr = MmGetPhysicalAddress(TargetAddress);
    if (physAddr.QuadPart == 0)
        return STATUS_INVALID_PARAMETER;

    mapping = MmMapIoSpace(physAddr, PatchSize, MmNonCached);
    if (!mapping)
        return STATUS_INSUFFICIENT_RESOURCES;

    RtlCopyMemory(mapping, PatchData, PatchSize);

    MmUnmapIoSpace(mapping, PatchSize);

    return STATUS_SUCCESS;
}

NTSTATUS HandlePatchProtectedMemory(PWRITE_PROTECTED_REQUEST Req, SIZE_T InputLength)
{
    PVOID patchData;
    SIZE_T headerSize = sizeof(WRITE_PROTECTED_REQUEST);

    if (!Req)
        return STATUS_INVALID_PARAMETER;

    if (Req->Size == 0 || Req->Size > MAX_PATCH_SIZE)
        return STATUS_INVALID_PARAMETER;

    if (InputLength < headerSize + Req->Size)
        return STATUS_BUFFER_TOO_SMALL;

    if (Req->Address == 0)
        return STATUS_INVALID_PARAMETER;

    patchData = (PVOID)((PUCHAR)Req + headerSize);

    return WriteProtectedKernelMemory(
        (PVOID)(ULONG_PTR)Req->Address,
        patchData,
        Req->Size
    );
}

// =============================================================
// NEW: enumerate loaded kernel modules
// =============================================================

NTSTATUS HandleEnumKernelModules(
    PVOID OutputBuffer,
    SIZE_T OutputBufferLength,
    PULONG_PTR BytesReturned
)
{
    NTSTATUS status;
    ULONG allocSize = 0;
    PVOID moduleInfo = NULL;
    PSYSTEM_MODULE_INFORMATION smi;
    PENUM_MODULES_RESPONSE response;
    ULONG i;
    ULONG count = 0;

    *BytesReturned = 0;

    status = ZwQuerySystemInformation(SystemModuleInformation, &allocSize, 0, &allocSize);
    if (allocSize == 0)
        return STATUS_INSUFFICIENT_RESOURCES;

    allocSize += 0x1000;
    moduleInfo = ExAllocatePool2(POOL_FLAG_NON_PAGED, allocSize, 'DuoM');
    if (!moduleInfo)
        return STATUS_INSUFFICIENT_RESOURCES;

    status = ZwQuerySystemInformation(SystemModuleInformation, moduleInfo, allocSize, &allocSize);
    if (!NT_SUCCESS(status)) {
        ExFreePool(moduleInfo);
        return status;
    }

    smi = (PSYSTEM_MODULE_INFORMATION)moduleInfo;

    count = smi->NumberOfModules;
    if (count > MAX_MODULES)
        count = MAX_MODULES;

    // Calculate required output size
    {
        SIZE_T requiredSize = sizeof(ENUM_MODULES_RESPONSE) +
                              (SIZE_T)(count - 1) * sizeof(MODULE_INFO_ENTRY);

        if (OutputBufferLength < requiredSize) {
            ExFreePool(moduleInfo);
            return STATUS_BUFFER_TOO_SMALL;
        }
    }

    response = (PENUM_MODULES_RESPONSE)OutputBuffer;
    response->Count = 0;

    for (i = 0; i < count && i < smi->NumberOfModules; i++) {
        PSYSTEM_MODULE_ENTRY entry = &smi->Modules[i];
        PMODULE_INFO_ENTRY out = &response->Modules[response->Count];

        out->BaseAddress = (ULONG64)entry->ImageBase;
        out->Size = entry->ImageSize;

        RtlZeroMemory(out->Name, sizeof(out->Name));
        {
            PUCHAR fileName = entry->FullPathName + entry->OffsetToFileName;
            RtlCopyMemory(out->Name, fileName,
                sizeof(out->Name) - 1);
        }

        response->Count++;
    }

    *BytesReturned = sizeof(ENUM_MODULES_RESPONSE) +
                     (SIZE_T)(response->Count - 1) * sizeof(MODULE_INFO_ENTRY);

    ExFreePool(moduleInfo);
    return STATUS_SUCCESS;
}

// =============================================================
// IOCTL DISPATCH
// =============================================================

VOID EvtIoDeviceControl(
    WDFQUEUE Queue,
    WDFREQUEST Request,
    size_t OutputBufferLength,
    size_t InputBufferLength,
    ULONG IoControlCode
)
{
    NTSTATUS status = STATUS_SUCCESS;
    PVOID inputBuffer = NULL;
    PVOID outputBuffer = NULL;
    size_t bytesReturned = 0;

    UNREFERENCED_PARAMETER(Queue);

    switch (IoControlCode) {

    // --- Original OmniDriver IOCTLs ---

    case IOCTL_READWRITE_DRIVER_READ:
    case IOCTL_READWRITE_DRIVER_WRITE:
    {
        PKERNEL_READWRITE_REQUEST req = NULL;

        if (InputBufferLength < sizeof(KERNEL_READWRITE_REQUEST)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }

        status = WdfRequestRetrieveInputBuffer(Request, sizeof(KERNEL_READWRITE_REQUEST),
                                                &inputBuffer, NULL);
        if (!NT_SUCCESS(status))
            break;

        req = (PKERNEL_READWRITE_REQUEST)inputBuffer;
        req->Write = (IoControlCode == IOCTL_READWRITE_DRIVER_WRITE);
        req->Status = ReadWriteMemory(req);

        bytesReturned = sizeof(KERNEL_READWRITE_REQUEST);
        break;
    }

    case IOCTL_READWRITE_DRIVER_BULK:
    {
        PKERNEL_BULK_OPERATION bulkReq = NULL;

        if (InputBufferLength < sizeof(KERNEL_BULK_OPERATION)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }

        status = WdfRequestRetrieveInputBuffer(Request, InputBufferLength,
                                                &inputBuffer, NULL);
        if (!NT_SUCCESS(status))
            break;

        bulkReq = (PKERNEL_BULK_OPERATION)inputBuffer;
        status = HandleBulkOperations(bulkReq);
        bytesReturned = InputBufferLength;
        break;
    }

    // --- New: read-only kernel memory patching ---

    case IOCTL_PATCH_PROTECTED_MEMORY:
    {
        PWRITE_PROTECTED_REQUEST req = NULL;

        if (InputBufferLength < sizeof(WRITE_PROTECTED_REQUEST)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }

        status = WdfRequestRetrieveInputBuffer(Request, InputBufferLength,
                                                &inputBuffer, NULL);
        if (!NT_SUCCESS(status))
            break;

        req = (PWRITE_PROTECTED_REQUEST)inputBuffer;
        status = HandlePatchProtectedMemory(req, InputBufferLength);
        break;
    }

    case IOCTL_ENUM_KERNEL_MODULES:
    {
        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(ENUM_MODULES_RESPONSE),
                                                 &outputBuffer, NULL);
        if (!NT_SUCCESS(status))
            break;

        status = HandleEnumKernelModules(outputBuffer, (SIZE_T)OutputBufferLength,
                                          &bytesReturned);
        break;
    }

    default:
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

    WdfRequestCompleteWithInformation(Request, status, bytesReturned);
}

// =============================================================
// CLEANUP
// =============================================================

VOID EvtDriverUnload(WDFDRIVER Driver)
{
    UNREFERENCED_PARAMETER(Driver);
}

// =============================================================
// ENTRY POINT
// =============================================================

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG config;
    NTSTATUS status;
    WDFDRIVER driver;
    PWDFDEVICE_INIT deviceInit = NULL;
    WDF_OBJECT_ATTRIBUTES deviceAttributes;
    WDF_IO_QUEUE_CONFIG queueConfig;
    WDFQUEUE queue;
    UNICODE_STRING deviceName;
    UNICODE_STRING symbolicLink;

    DECLARE_CONST_UNICODE_STRING(sddl, L"D:P(A;;GA;;;SY)(A;;GA;;;BA)");

    WDF_DRIVER_CONFIG_INIT(&config, WDF_NO_EVENT_CALLBACK);
    config.EvtDriverUnload = EvtDriverUnload;
    config.DriverInitFlags = WdfDriverInitNonPnpDriver;

    status = WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES,
                              &config, &driver);
    if (!NT_SUCCESS(status)) return status;

    deviceInit = WdfControlDeviceInitAllocate(driver, &sddl);
    if (!deviceInit) return STATUS_INSUFFICIENT_RESOURCES;

    RtlInitUnicodeString(&deviceName, DEVICE_NAME);
    status = WdfDeviceInitAssignName(deviceInit, &deviceName);
    if (!NT_SUCCESS(status)) {
        WdfDeviceInitFree(deviceInit);
        return status;
    }

    WdfDeviceInitSetIoType(deviceInit, WdfDeviceIoBuffered);

    WDF_OBJECT_ATTRIBUTES_INIT(&deviceAttributes);
    status = WdfDeviceCreate(&deviceInit, &deviceAttributes, &g_Device);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    RtlInitUnicodeString(&symbolicLink, SYMBOLIC_NAME);
    status = WdfDeviceCreateSymbolicLink(g_Device, &symbolicLink);
    if (!NT_SUCCESS(status)) return status;

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchSequential);
    queueConfig.EvtIoDeviceControl = EvtIoDeviceControl;

    status = WdfIoQueueCreate(g_Device, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, &queue);
    if (!NT_SUCCESS(status)) return status;

    WdfControlFinishInitializing(g_Device);

    return STATUS_SUCCESS;
}
