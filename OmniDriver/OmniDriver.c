#include "OmniDriver.h"

// Global device handle
WDFDEVICE g_Device = NULL;

// Helper to validate and copy memory safely using MmCopyVirtualMemory
NTSTATUS ReadWriteMemory(PKERNEL_READWRITE_REQUEST Req)
{
    PEPROCESS TargetProcess = NULL;
    PEPROCESS ClientProcess = NULL;
    NTSTATUS status = STATUS_SUCCESS;
    SIZE_T bytesCopied = 0;

    // 1. Parameter validation
    if (!Req || Req->Size == 0 || Req->Size > MAX_TRANSFER_SIZE) 
        return STATUS_INVALID_PARAMETER;

    if (Req->Address == 0 || Req->Buffer == 0)
        return STATUS_INVALID_PARAMETER;
    
    // 2. Downloading the target process (always fresh handle - no cache = no BSOD)
    status = PsLookupProcessByProcessId((HANDLE)(ULONG_PTR)Req->ProcessId, &TargetProcess);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    // 3. Downloading the client process (the one that called the driver)
    ClientProcess = PsGetCurrentProcess();

    // 4. Making a secure copy (KernelMode bypasses user restrictions)
    if (Req->Write) {
        // Write: from client buffer to destination address
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
        // Read: from destination address to client buffer
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

    // 5. We ALWAYS release the process object after use.
    if (TargetProcess) {
        ObDereferenceObject(TargetProcess);
    }

    return status;
}

// Handler for bulk operations
// Allows multiple reads/writes to be performed in a single IOCTL call (huge performance gain)
NTSTATUS HandleBulkOperations(PKERNEL_BULK_OPERATION BulkReq)
{
    NTSTATUS status = STATUS_SUCCESS;
    ULONG i;

    if (!BulkReq || BulkReq->Count == 0 || BulkReq->Count > MAX_BULK_OPERATIONS) {
        return STATUS_INVALID_PARAMETER;
    }

    for (i = 0; i < BulkReq->Count; i++) {
        // We call a safe function for each element
        BulkReq->Operations[i].Status = ReadWriteMemory(&BulkReq->Operations[i]);
        
        // Error logic: if most succeeded but one failed, we return the status of that error, 
        // but the loop continues to try to execute the rest.
        if (!NT_SUCCESS(BulkReq->Operations[i].Status) && NT_SUCCESS(status)) {
            status = BulkReq->Operations[i].Status;
        }
    }

    return status;
}

// Main IOCTL Handler
VOID EvtIoDeviceControl(
    WDFQUEUE Queue,
    WDFREQUEST Request,
    size_t OutputBufferLength,
    size_t InputBufferLength,
    ULONG IoControlCode
)
{
    NTSTATUS status = STATUS_SUCCESS;
    PKERNEL_READWRITE_REQUEST req = NULL;
    PKERNEL_BULK_OPERATION bulkReq = NULL;
    size_t bytesReturned = 0;

    UNREFERENCED_PARAMETER(Queue);
    UNREFERENCED_PARAMETER(OutputBufferLength);

    // 1. Fail-Fast Check for minimum size
    if (InputBufferLength < sizeof(KERNEL_READWRITE_REQUEST)) {
        WdfRequestComplete(Request, STATUS_BUFFER_TOO_SMALL);
        return;
    }

    switch (IoControlCode) {
        case IOCTL_READWRITE_DRIVER_READ:
        case IOCTL_READWRITE_DRIVER_WRITE:
            // Pobranie bufora
            status = WdfRequestRetrieveInputBuffer(Request, sizeof(KERNEL_READWRITE_REQUEST), (PVOID*)&req, NULL);
            if (!NT_SUCCESS(status)) {
                WdfRequestComplete(Request, status);
                return;
            }

            // Setting a flag based on IOCTL code
            req->Write = (IoControlCode == IOCTL_READWRITE_DRIVER_WRITE);
            
            // Performing the operation
            req->Status = ReadWriteMemory(req);
            
            bytesReturned = sizeof(KERNEL_READWRITE_REQUEST);
            break;

        case IOCTL_READWRITE_DRIVER_BULK:
            // Checking the size for Bulk
            if (InputBufferLength < sizeof(KERNEL_BULK_OPERATION)) {
                WdfRequestComplete(Request, STATUS_BUFFER_TOO_SMALL);
                return;
            }

            status = WdfRequestRetrieveInputBuffer(Request, InputBufferLength, (PVOID*)&bulkReq, NULL);
            if (!NT_SUCCESS(status)) {
                WdfRequestComplete(Request, status);
                return;
            }

            // Performing batch operations
            status = HandleBulkOperations(bulkReq);
            bytesReturned = InputBufferLength;
            break;

        default:
            status = STATUS_INVALID_DEVICE_REQUEST;
            break;
    }

    WdfRequestCompleteWithInformation(Request, status, bytesReturned);
}

// Clean up resources
VOID EvtDriverUnload(WDFDRIVER Driver)
{
    UNREFERENCED_PARAMETER(Driver);
}

// Driver Entry Point
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
    
    // SDDL: Allow access only to System (SY) and Administrators (BA)
    DECLARE_CONST_UNICODE_STRING(sddl, L"D:P(A;;GA;;;SY)(A;;GA;;;BA)");

    // Initialize driver config
    WDF_DRIVER_CONFIG_INIT(&config, WDF_NO_EVENT_CALLBACK);
    config.EvtDriverUnload = EvtDriverUnload;
    config.DriverInitFlags = WdfDriverInitNonPnpDriver;

    status = WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &config, &driver);
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