/* SPDX-License-Identifier: BSD-3-Clause-Clear
 * PCIe MCU diagnostic and explicit admin-only persistent radio interface.
 * No automatic firmware/RF startup; lifecycle cleanup revokes running sessions.
 */
#include "device.h"
#include <initguid.h>
#include <wdmguid.h>
#include <wdmsec.h>
#include <acpiioct.h>
#include <ntiologc.h>

DEFINE_GUID(GUID_DEVINTERFACE_MT7925_BRINGUP,
    0x3c9e8b97, 0x7d9b, 0x4db2, 0x91, 0x3d, 0xda, 0x42, 0x79, 0x25, 0x00, 0x01);
C_ASSERT(sizeof(mt_desc) == 16);
C_ASSERT(sizeof(MT_INFO) == 48);
C_ASSERT(sizeof(MT_PROBE) == 48);
C_ASSERT(sizeof(mt_fw_preflight) == 32);
C_ASSERT(sizeof(mt_chip_result) == 108);
C_ASSERT(sizeof(mt_platform) == 248);
C_ASSERT(ACPI_EVAL_OUTPUT_BUFFER_SIGNATURE == MT_ACPI_OUTPUT_SIGNATURE);
C_ASSERT(FIELD_OFFSET(ACPI_EVAL_OUTPUT_BUFFER, Argument) == 12);
C_ASSERT(sizeof(ACPI_METHOD_ARGUMENT) == 8);
C_ASSERT(FIELD_OFFSET(ACPI_METHOD_ARGUMENT, Data) == 4);
C_ASSERT(ACPI_METHOD_ARGUMENT_INTEGER == 0);
C_ASSERT((ULONG)STATUS_OBJECT_NAME_NOT_FOUND == MT_ACPI_NAME_NOT_FOUND);
enum {
    MtFailureMapBar = 1, MtFailureDmaEnabler = 2,
    MtFailureDescriptorBuffer = 3,
    MtFailureRuntimePrepare = 5, MtFailureDescriptorSpan = 6,
    MtFailureReadPci = 8,
    MtFailureDriverCreate = 10, MtFailureSddl = 11,
    MtFailureDeviceCreate = 12, MtFailureWaitLock = 13,
    MtFailureDeviceInterface = 14, MtFailureQueue = 15,
    MtFailureD0Entry = 20
};
#define MT_FAILURE_DUMP_BYTES (5u * sizeof(ULONG))
#define MT_FAILURE_LOG_BYTES (sizeof(IO_ERROR_LOG_PACKET) + MT_FAILURE_DUMP_BYTES)
C_ASSERT(MT_FAILURE_LOG_BYTES <= ERROR_LOG_MAXIMUM_SIZE);
C_ASSERT(MT_FAILURE_LOG_BYTES <= MAXUCHAR);

/* Best effort: failure to allocate a log must never replace the original error.
 * Fixed System log payload: magic "MTFL", version, stage, requested bytes,
 * original NTSTATUS. No addresses or caller data; zero bytes means no buffer
 * size applies. This remains readable when device creation/start has failed. */
static NTSTATUS MtLogFailure(PVOID object, ULONG stage, ULONG bytes, NTSTATUS status) {
    PIO_ERROR_LOG_PACKET entry = IoAllocateErrorLogEntry(object, (UCHAR)MT_FAILURE_LOG_BYTES);
    if (!entry) return status;
    RtlZeroMemory(entry, MT_FAILURE_LOG_BYTES);
    entry->ErrorCode = status == STATUS_INSUFFICIENT_RESOURCES ?
        IO_ERR_INSUFFICIENT_RESOURCES : IO_ERR_DRIVER_ERROR;
    entry->FinalStatus = status;
    entry->UniqueErrorValue = stage;
    entry->DumpDataSize = (USHORT)MT_FAILURE_DUMP_BYTES;
    entry->DumpData[0] = 0x4c46544du;
    entry->DumpData[1] = 1u;
    entry->DumpData[2] = stage;
    entry->DumpData[3] = bytes;
    entry->DumpData[4] = (ULONG)status;
    IoWriteErrorLogEntry(entry); /* The I/O manager owns/frees the queued entry. */
    return status;
}
static int MtPrepareData(void *context);
static int MtReadPlatform(void *context, mt_platform *out);
static int32_t MtCreateDataBuffer(void *context,uint32_t bytes,mt_data_block *out) {
    MT_DEVICE *ctx=context;
    WDFCOMMONBUFFER handle;
    NTSTATUS status=WdfCommonBufferCreate(ctx->dma,bytes,WDF_NO_OBJECT_ATTRIBUTES,&handle);
    if (!NT_SUCCESS(status)) return status;
    out->handle=handle;
    out->virtual_address=WdfCommonBufferGetAlignedVirtualAddress(handle);
    out->address=(uint64_t)WdfCommonBufferGetAlignedLogicalAddress(handle).QuadPart;
    RtlZeroMemory(out->virtual_address,bytes);
    return MT_OK;
}
static void MtReleaseDataBuffer(void *context,void *handle) {
    UNREFERENCED_PARAMETER(context);
    WdfObjectDelete((WDFCOMMONBUFFER)handle);
}

static VOID MtFreeResources(MT_DEVICE *ctx) {
    mt_data_allocator allocator;
    ctx->prepared = FALSE;
    /* Called only for prepare failure (no addresses ever published), or from
     * EvtDeviceReleaseHardware AFTER KMDF has powered the device off. A failed
     * boot/stop/D0Exit NEVER calls this: its common buffers stay allocated until
     * that PnP boundary. ReleaseHardware errors cannot veto object deletion.
     * No MMIO here: the device may already have been surprise-removed. */
    mt_runtime_disarm(&ctx->runtime);
    RtlZeroMemory(&ctx->runtime, sizeof(ctx->runtime));
    RtlZeroMemory(&ctx->chip, sizeof(ctx->chip));
    RtlZeroMemory(&ctx->radio, sizeof(ctx->radio));
    RtlZeroMemory(&ctx->data_tx, sizeof(ctx->data_tx));
    ctx->powered = FALSE; ctx->writes_enabled = FALSE;
    allocator.context=ctx;allocator.create=MtCreateDataBuffer;allocator.release=MtReleaseDataBuffer;
    mt_data_memory_release(&ctx->data_memory,&allocator);
    mt_mcu_memory_release(&ctx->mcu_memory,&allocator);
    if (ctx->descriptors) { WdfObjectDelete(ctx->descriptors); ctx->descriptors = NULL; }
    if (ctx->dma) { WdfObjectDelete(ctx->dma); ctx->dma = NULL; }
    if (ctx->bar) { MmUnmapIoSpace(ctx->bar, ctx->bar_bytes); ctx->bar = NULL; }
    ctx->bar_bytes = 0;
}

NTSTATUS DriverEntry(PDRIVER_OBJECT driver, PUNICODE_STRING registry) {
    WDF_DRIVER_CONFIG config;
    NTSTATUS status;
    WDF_DRIVER_CONFIG_INIT(&config, MtDeviceAdd);
    status = WdfDriverCreate(driver, registry, WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
    if (!NT_SUCCESS(status)) return MtLogFailure(driver, MtFailureDriverCreate, 0, status);
    return status;
}

NTSTATUS MtDeviceAdd(WDFDRIVER driver, PWDFDEVICE_INIT init) {
    WDF_PNPPOWER_EVENT_CALLBACKS callbacks;
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_IO_QUEUE_CONFIG queue;
    WDF_FILEOBJECT_CONFIG files;
    WDFDEVICE device;
    NTSTATUS status;
    UNICODE_STRING sddl;
    RtlInitUnicodeString(&sddl, L"D:P(A;;GA;;;SY)(A;;GA;;;BA)");
    WdfDeviceInitSetDeviceType(init, FILE_DEVICE_UNKNOWN);
    WdfDeviceInitSetCharacteristics(init, FILE_DEVICE_SECURE_OPEN | FILE_AUTOGENERATED_DEVICE_NAME, TRUE);
    WdfDeviceInitSetExclusive(init, TRUE);
    WdfDeviceInitSetIoType(init, WdfDeviceIoBuffered);
    status = WdfDeviceInitAssignSDDLString(init, &sddl);
    if (!NT_SUCCESS(status)) return MtLogFailure(WdfDriverWdmGetDriverObject(driver), MtFailureSddl, 0, status);
    WDF_FILEOBJECT_CONFIG_INIT(&files, WDF_NO_EVENT_CALLBACK, WDF_NO_EVENT_CALLBACK, MtFileCleanup);
    files.AutoForwardCleanupClose = WdfFalse;
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ExecutionLevel = WdfExecutionLevelPassive;
    attributes.SynchronizationScope = WdfSynchronizationScopeNone;
    WdfDeviceInitSetFileObjectConfig(init, &files, &attributes);
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&callbacks);
    callbacks.EvtDevicePrepareHardware = MtPrepare;
    callbacks.EvtDeviceReleaseHardware = MtRelease;
    callbacks.EvtDeviceD0Entry = MtD0Entry;
    callbacks.EvtDeviceD0Exit = MtD0Exit;
    callbacks.EvtDeviceSurpriseRemoval = MtSurpriseRemoval;
    WdfDeviceInitSetPnpPowerEventCallbacks(init, &callbacks);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, MT_DEVICE);
    attributes.ExecutionLevel = WdfExecutionLevelPassive;
    attributes.SynchronizationScope = WdfSynchronizationScopeDevice;
    status = WdfDeviceCreate(&init, &attributes, &device);
    if (!NT_SUCCESS(status)) return MtLogFailure(WdfDriverWdmGetDriverObject(driver), MtFailureDeviceCreate, 0, status);
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = device;
    status = WdfWaitLockCreate(&attributes, &MtGetDevice(device)->lifecycle_lock);
    if (!NT_SUCCESS(status)) return MtLogFailure(WdfDeviceWdmGetDeviceObject(device), MtFailureWaitLock, 0, status);
    status = WdfDeviceCreateDeviceInterface(device, &GUID_DEVINTERFACE_MT7925_BRINGUP, NULL);
    if (!NT_SUCCESS(status)) return MtLogFailure(WdfDeviceWdmGetDeviceObject(device), MtFailureDeviceInterface, 0, status);
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queue, WdfIoQueueDispatchSequential);
    queue.PowerManaged = WdfTrue;
    queue.EvtIoDeviceControl = MtControl;
    status = WdfIoQueueCreate(device, &queue, WDF_NO_OBJECT_ATTRIBUTES, WDF_NO_HANDLE);
    if (!NT_SUCCESS(status)) return MtLogFailure(WdfDeviceWdmGetDeviceObject(device), MtFailureQueue, 0, status);
    return status;
}

static NTSTATUS MtReadPci(WDFDEVICE device, PCI_COMMON_CONFIG *pci) {
    BUS_INTERFACE_STANDARD bus;
    NTSTATUS status;
    ULONG read;
    RtlZeroMemory(&bus, sizeof(bus));
    RtlZeroMemory(pci, sizeof(*pci));
    status = WdfFdoQueryForInterface(device, &GUID_BUS_INTERFACE_STANDARD,
        (PINTERFACE)&bus, sizeof(bus), 1, NULL);
    if (!NT_SUCCESS(status)) return status;
    if (!bus.InterfaceDereference) return STATUS_INVALID_DEVICE_STATE;
    if (!bus.GetBusData) {
        bus.InterfaceDereference(bus.Context);
        return STATUS_INVALID_DEVICE_STATE;
    }
    read = bus.GetBusData(bus.Context, PCI_WHICHSPACE_CONFIG, pci, 0, PCI_COMMON_HDR_LENGTH);
    bus.InterfaceDereference(bus.Context);
    if (read != PCI_COMMON_HDR_LENGTH) return STATUS_DEVICE_DATA_ERROR;
    if (pci->VendorID != 0x14c3 || pci->DeviceID != 0x7925 ||
        (pci->HeaderType & 0x7f) != PCI_DEVICE_TYPE) return STATUS_NOT_SUPPORTED;
    return STATUS_SUCCESS;
}

/* The wait lock serializes control/PnP callbacks. Surprise removal revokes MMIO
 * atomically before waiting, so an in-progress poll exits without more writes.
 * Runtime doorbells additionally require successful chip/ring attachment. */
static int MtReadRegister(void *context, uint32_t offset, uint32_t *value) {
    MT_DEVICE *ctx = context;
    if (!ctx->prepared || !ctx->powered || ctx->removed || !ctx->bar || !value || (offset & 3u) ||
        ctx->bar_bytes < 4 || offset > ctx->bar_bytes-4) return MT_INVALID;
    *value = READ_REGISTER_ULONG((PULONG)(ctx->bar + offset));
    return *value == MAXULONG ? MT_FW_PROTOCOL : MT_OK;
}
static int MtWriteRegister(void *context, uint32_t offset, uint32_t value) {
    MT_DEVICE *ctx = context;
    if (!ctx->prepared || !ctx->powered || ctx->removed || !ctx->writes_enabled || !ctx->bar || (offset & 3u) ||
        ctx->bar_bytes < 4 || offset > ctx->bar_bytes-4) return MT_INVALID;
    WRITE_REGISTER_ULONG((PULONG)(ctx->bar + offset), value);
    return MT_OK;
}
static void MtMemoryBarrier(void *context) {
    UNREFERENCED_PARAMETER(context);
    KeMemoryBarrier();
}
static uint64_t MtMilliseconds(void *context) {
    UNREFERENCED_PARAMETER(context);
    return KeQueryInterruptTime() / 10000u;
}
static void MtPause(void *context) {
    LARGE_INTEGER interval;
    UNREFERENCED_PARAMETER(context);
    interval.QuadPart = -10000; /* Passive-level 1 ms polling interval. */
    (void)KeDelayExecutionThread(KernelMode, FALSE, &interval);
}

NTSTATUS MtPrepare(WDFDEVICE device, WDFCMRESLIST raw, WDFCMRESLIST translated) {
    MT_DEVICE *ctx = MtGetDevice(device);
    PCI_COMMON_CONFIG pci;
    WDF_DMA_ENABLER_CONFIG dma;
    PCM_PARTIAL_RESOURCE_DESCRIPTOR r, t, selected = NULL;
    PHYSICAL_ADDRESS logical, descriptor_address;
    ULONGLONG bar0;
    ULONG i, low, failure_stage, failure_bytes;
    mt_desc *entries;
    mt_pcie_io io;
    mt_data_allocator allocator;
    mt_data_allocation allocation;
    NTSTATUS status;
    const ULONG descriptor_bytes = MT_DESCRIPTOR_BYTES;
    if (ctx->bar || ctx->dma) return STATUS_INVALID_DEVICE_STATE;
    ctx->removed = 0; ctx->failed = FALSE;
    status = MtReadPci(device, &pci);
    if (!NT_SUCCESS(status)) return MtLogFailure(WdfDeviceWdmGetDeviceObject(device), MtFailureReadPci, 0, status);
    low = pci.u.type0.BaseAddresses[0];
    if ((low & PCI_ADDRESS_IO_SPACE) || (low & 6u) == 2u || (low & 6u) == 6u)
        return STATUS_NOT_SUPPORTED;
    bar0 = low & 0xfffffff0u;
    if ((low & 6u) == 4u) bar0 |= (ULONGLONG)pci.u.type0.BaseAddresses[1] << 32;
    if (!bar0 || WdfCmResourceListGetCount(raw) != WdfCmResourceListGetCount(translated))
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    /* Match the bus-relative BAR0 address, not just the first large resource. */
    for (i = 0; i < WdfCmResourceListGetCount(raw); ++i) {
        r = WdfCmResourceListGetDescriptor(raw, i);
        t = WdfCmResourceListGetDescriptor(translated, i);
        if (!r || !t) return STATUS_DEVICE_CONFIGURATION_ERROR;
        if (r->Type != CmResourceTypeMemory || t->Type != CmResourceTypeMemory ||
            (ULONGLONG)r->u.Memory.Start.QuadPart != bar0) continue;
        if (selected || t->u.Memory.Length < 0x200000u ||
            (t->Flags & CM_RESOURCE_MEMORY_READ_ONLY)) return STATUS_DEVICE_CONFIGURATION_ERROR;
        selected = t;
    }
    if (!selected) return STATUS_DEVICE_CONFIGURATION_ERROR;
    ctx->bar_bytes = selected->u.Memory.Length;
    failure_stage = MtFailureMapBar; failure_bytes = ctx->bar_bytes;
    ctx->bar = MmMapIoSpace(selected->u.Memory.Start, ctx->bar_bytes, MmNonCached);
    if (!ctx->bar) { status = STATUS_INSUFFICIENT_RESOURCES; goto fail; }
    WdfDeviceSetAlignmentRequirement(device, 16u - 1u); /* WDF takes an alignment mask. */
    /* Non-64 profile instructs the OS/IOMMU to allocate 32-bit device addresses. */
    WDF_DMA_ENABLER_CONFIG_INIT(&dma, WdfDmaProfileScatterGather, MT_SLOT_BYTES);
    failure_stage = MtFailureDmaEnabler; failure_bytes = 0;
    status = WdfDmaEnablerCreate(device, &dma, WDF_NO_OBJECT_ATTRIBUTES, &ctx->dma);
    if (!NT_SUCCESS(status)) goto fail;
    failure_stage = MtFailureDescriptorBuffer; failure_bytes = descriptor_bytes;
    status = WdfCommonBufferCreate(ctx->dma, descriptor_bytes, WDF_NO_OBJECT_ATTRIBUTES, &ctx->descriptors);
    if (!NT_SUCCESS(status)) goto fail;
    logical = WdfCommonBufferGetAlignedLogicalAddress(ctx->descriptors);
    descriptor_address = logical;
    failure_stage = MtFailureDescriptorSpan;
    if (!mt_dma_span32((ULONGLONG)logical.QuadPart, descriptor_bytes) || (logical.QuadPart & 15)) {
        status = STATUS_CONFLICTING_ADDRESSES; goto fail;
    }
    entries = (mt_desc *)WdfCommonBufferGetAlignedVirtualAddress(ctx->descriptors);
    RtlZeroMemory(&io, sizeof(io));
    io.context = ctx; io.read = MtReadRegister; io.write = MtWriteRegister;
    io.barrier = MtMemoryBarrier; io.milliseconds = MtMilliseconds; io.pause = MtPause;
    failure_stage = MtFailureRuntimePrepare; failure_bytes = 0;
    allocator.context=ctx;allocator.create=MtCreateDataBuffer;allocator.release=MtReleaseDataBuffer;
    RtlZeroMemory(&allocation,sizeof(allocation));
    status=mt_mcu_memory_prepare(&ctx->mcu_memory,&allocation,&allocator,&ctx->runtime,
        &io,entries,(ULONGLONG)descriptor_address.QuadPart,descriptor_bytes);
    if (status != MT_OK) {
        if (allocation.stage>=MT_MCU_ALLOCATION_STAGE &&
            allocation.stage<MT_MCU_ALLOCATION_STAGE+MT_MCU_BANK_COUNT) {
            failure_stage=allocation.stage;failure_bytes=allocation.request_bytes;
        } else status=STATUS_INTERNAL_ERROR;
        goto fail;
    }
    ctx->prepared = TRUE;
    ctx->radio.runtime = &ctx->runtime; ctx->radio.chip = &ctx->chip;
    ctx->radio.context = device; ctx->radio.read_platform = MtReadPlatform;
    ctx->radio.prepare_data = MtPrepareData; ctx->radio.scratch = ctx->radio_scratch;
    ctx->radio.scratch_capacity = sizeof(ctx->radio_scratch);
    ctx->radio.allocation.bank_index = 0xffffffffu;
    return STATUS_SUCCESS;
fail:
    (void)MtLogFailure(WdfDeviceWdmGetDeviceObject(device), failure_stage, failure_bytes, status);
    MtFreeResources(ctx);
    return status;
}

NTSTATUS MtRelease(WDFDEVICE device, WDFCMRESLIST translated) {
    MT_DEVICE *ctx = MtGetDevice(device);
    UNREFERENCED_PARAMETER(translated);
    WdfWaitLockAcquire(ctx->lifecycle_lock, NULL);
    MtFreeResources(ctx);
    WdfWaitLockRelease(ctx->lifecycle_lock);
    return STATUS_SUCCESS;
}

NTSTATUS MtD0Entry(WDFDEVICE device, WDF_POWER_DEVICE_STATE previous) {
    MT_DEVICE *ctx = MtGetDevice(device);
    NTSTATUS status = STATUS_SUCCESS;
    UNREFERENCED_PARAMETER(previous);
    WdfWaitLockAcquire(ctx->lifecycle_lock, NULL);
    if (!ctx->prepared || ctx->removed || ctx->failed) status = STATUS_INVALID_DEVICE_STATE;
    else ctx->powered = TRUE;
    WdfWaitLockRelease(ctx->lifecycle_lock);
    if (!NT_SUCCESS(status)) return MtLogFailure(WdfDeviceWdmGetDeviceObject(device), MtFailureD0Entry, 0, status);
    return status; /* Never auto-start firmware or reuse an old DMA generation. */
}
NTSTATUS MtD0Exit(WDFDEVICE device, WDF_POWER_DEVICE_STATE target) {
    MT_DEVICE *ctx = MtGetDevice(device);
    NTSTATUS status = STATUS_SUCCESS;
    UNREFERENCED_PARAMETER(target);
    WdfWaitLockAcquire(ctx->lifecycle_lock, NULL);
    mt_runtime_disarm(&ctx->runtime);
    if (!ctx->removed && ctx->chip.published && !ctx->chip.quiescent) {
        ctx->writes_enabled = TRUE;
        if ((ctx->radio.session.attempted ? mt_radio_device_close(&ctx->radio, 1) :
            mt_chip_stop(&ctx->chip)) != MT_OK) {
            ctx->failed = TRUE; status = STATUS_DEVICE_HARDWARE_ERROR;
        }
    }
    ctx->writes_enabled = FALSE; ctx->powered = FALSE;
    ctx->radio.session.ready = 0;
    WdfWaitLockRelease(ctx->lifecycle_lock);
    return status; /* Buffers retained, even if the hardware cannot be stopped. */
}
VOID MtSurpriseRemoval(WDFDEVICE device) {
    MT_DEVICE *ctx = MtGetDevice(device);
    InterlockedExchange(&ctx->removed, 1);
    WdfWaitLockAcquire(ctx->lifecycle_lock, NULL);
    mt_runtime_disarm(&ctx->runtime);
    (void)mt_radio_device_close(&ctx->radio, 0);
    ctx->writes_enabled = FALSE; ctx->powered = FALSE;
    WdfWaitLockRelease(ctx->lifecycle_lock);
}

/* Fixed read-only OEM SAR methods on this device's underlying ACPI stack.
 * No caller-selected method/path, no retry with another namespace or fabricated
 * absence. A 1024-byte output fits every supported table's integer package;
 * overflow, unsupported stack, timeout and malformed output remain errors.
 * WDF's synchronous timeout waits for cancellation/completion before these
 * stack-backed descriptors/buffers go out of scope. No RF/DMA access here. */
static int32_t MtEvaluateAcpi(void *context, const uint8_t name[4], uint8_t *output,
                            uint32_t capacity, uint32_t *size) {
    WDFDEVICE device = (WDFDEVICE)context;
    MT_DEVICE *ctx = MtGetDevice(device);
    ACPI_EVAL_INPUT_BUFFER input;
    WDF_MEMORY_DESCRIPTOR in_desc, out_desc;
    WDF_REQUEST_SEND_OPTIONS options;
    ULONG_PTR received = 0;
    NTSTATUS status;
    *size = 0;
    if (ctx->removed || !ctx->powered) return STATUS_DEVICE_NOT_READY;
    RtlZeroMemory(&input, sizeof(input));
    input.Signature = ACPI_EVAL_INPUT_BUFFER_SIGNATURE;
    RtlCopyMemory(input.MethodName, name, 4);
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&in_desc, &input, sizeof(input));
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&out_desc, output, capacity);
    WDF_REQUEST_SEND_OPTIONS_INIT(&options, WDF_REQUEST_SEND_OPTION_TIMEOUT);
    WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&options, WDF_REL_TIMEOUT_IN_SEC(2));
    status = WdfIoTargetSendIoctlSynchronously(WdfDeviceGetIoTarget(device), NULL,
        IOCTL_ACPI_EVAL_METHOD, &in_desc, &out_desc, &options, &received);
    if (received > capacity) return STATUS_DEVICE_DATA_ERROR;
    *size = (uint32_t)received;
    return status;
}

static int MtReadPlatform(void *context, mt_platform *out) {
    mt_acpi_io io;
    io.context = context; io.evaluate = MtEvaluateAcpi;
    return mt_platform_read(&io, out);
}
static int MtPrepareData(void *context) {
    MT_DEVICE *ctx = MtGetDevice((WDFDEVICE)context);
    mt_data_allocator allocator;
    if (!ctx->prepared || !ctx->powered || ctx->removed || ctx->chip.attempted)
        return MT_INVALID;
    allocator.context=ctx;allocator.create=MtCreateDataBuffer;allocator.release=MtReleaseDataBuffer;
    return mt_data_memory_prepare(&ctx->data_memory,&ctx->radio.allocation,&allocator,
        &ctx->runtime,&ctx->data_tx,ctx->tx_states,ctx->tx_slots);
}
VOID MtFileCleanup(WDFFILEOBJECT file) {
    WDFDEVICE device = WdfFileObjectGetDevice(file);
    MT_DEVICE *ctx = MtGetDevice(device);
    BOOLEAN failed;
    WdfWaitLockAcquire(ctx->lifecycle_lock, NULL);
    ctx->writes_enabled = ctx->powered && !ctx->removed;
    if (mt_radio_device_close(&ctx->radio, ctx->writes_enabled ? 1u : 0u) != MT_OK &&
        ctx->chip.published && !ctx->chip.quiescent) ctx->failed = TRUE;
    ctx->writes_enabled = FALSE; failed = ctx->failed;
    WdfWaitLockRelease(ctx->lifecycle_lock);
    if (failed) WdfDeviceSetFailed(device, WdfDeviceFailedNoRestart);
}

static VOID MtDispatch(WDFQUEUE queue, WDFREQUEST request, size_t output_length,
                       size_t input_length, ULONG code) {
    MT_DEVICE *ctx = MtGetDevice(WdfIoQueueGetDevice(queue));
    MT_INFO *info;
    NTSTATUS status;
    ULONG op = 0;
    if (code == IOCTL_MT_RADIO_START) op = MT_RADIO_START;
    if (code == IOCTL_MT_RADIO_STATUS) op = MT_RADIO_STATUS;
    if (code == IOCTL_MT_RADIO_CHANNEL) op = MT_RADIO_CHANNEL;
    if (code == IOCTL_MT_RADIO_TX) op = MT_RADIO_TX;
    if (code == IOCTL_MT_RADIO_POLL) op = MT_RADIO_POLL;
    if (code == IOCTL_MT_RADIO_STOP) op = MT_RADIO_STOP;
    if (code == IOCTL_MT_RADIO_ADDRESS) op = MT_RADIO_ADDRESS;
    if (code == IOCTL_MT_RADIO_DIAGNOSTICS) op = MT_RADIO_DIAGNOSTICS;
    if (!ctx->prepared || (op != MT_RADIO_STATUS && op != MT_RADIO_DIAGNOSTICS && (!ctx->powered || ctx->removed || ctx->failed))) {
        WdfRequestComplete(request, STATUS_INVALID_DEVICE_STATE); return;
    }
    if (op) {
        PUCHAR input = NULL, output;
        uint32_t written = 0;
        int result;
        if (input_length > 56u + 2u*MT_FW_MAX_SIZE || output_length > MAXULONG) {
            WdfRequestComplete(request, STATUS_INVALID_PARAMETER); return;
        }
        if (input_length) {
            status = WdfRequestRetrieveInputBuffer(request, input_length, (PVOID *)&input, NULL);
            if (!NT_SUCCESS(status)) { WdfRequestComplete(request, status); return; }
        }
        status = WdfRequestRetrieveOutputBuffer(request,
            op == MT_RADIO_DIAGNOSTICS ? MT_RADIO_DIAGNOSTIC_BYTES :
            op == MT_RADIO_POLL ? MT_RADIO_POLL_BYTES : MT_RADIO_REPLY_BYTES, (PVOID *)&output, NULL);
        if (!NT_SUCCESS(status)) { WdfRequestComplete(request, status); return; }
        ctx->writes_enabled = op != MT_RADIO_STATUS && op != MT_RADIO_DIAGNOSTICS;
        result = mt_radio_request(&ctx->radio, op, input, (uint32_t)input_length,
            output, (uint32_t)output_length, &written);
        ctx->writes_enabled = FALSE;
        if (ctx->chip.published && !ctx->chip.quiescent && ctx->radio.session.stop_error) ctx->failed = TRUE;
        WdfRequestCompleteWithInformation(request,
            result == MT_OK ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER, result == MT_OK ? written : 0);
        return;
    }
    if (code == IOCTL_MT_PLATFORM_INFO) {
        mt_platform result, *output;
        mt_acpi_io io;
        if (input_length) { WdfRequestComplete(request, STATUS_INVALID_PARAMETER); return; }
        status = WdfRequestRetrieveOutputBuffer(request, sizeof(result), (PVOID *)&output, NULL);
        if (!NT_SUCCESS(status)) { WdfRequestComplete(request, status); return; }
        io.context = WdfIoQueueGetDevice(queue); io.evaluate = MtEvaluateAcpi;
        (void)mt_platform_read(&io, &result);
        *output = result; /* Method failures are retained as exact NTSTATUS fields. */
        WdfRequestCompleteWithInformation(request, STATUS_SUCCESS, sizeof(result)); return;
    }
    if (code == IOCTL_MT_BOOT_DIAGNOSTIC) {
        PUCHAR data;
        mt_chip_result result, *output;
        if (ctx->chip.attempted || ctx->radio.start_attempted || input_length > 16u + 2u*MT_FW_MAX_SIZE) {
            WdfRequestComplete(request, STATUS_INVALID_DEVICE_STATE); return;
        }
        status = WdfRequestRetrieveInputBuffer(request, 16, (PVOID *)&data, NULL);
        if (!NT_SUCCESS(status)) { WdfRequestComplete(request, status); return; }
        status = WdfRequestRetrieveOutputBuffer(request, sizeof(result), (PVOID *)&output, NULL);
        if (!NT_SUCCESS(status)) { WdfRequestComplete(request, status); return; }
        RtlZeroMemory(&result, sizeof(result));
        ctx->writes_enabled = TRUE;
        (void)mt_chip_boot_bundle(&ctx->chip, &ctx->runtime, data, (uint32_t)input_length, &result);
        ctx->writes_enabled = FALSE;
        if (!result.size) { WdfRequestComplete(request, STATUS_INVALID_PARAMETER); return; }
        if (ctx->chip.published && !ctx->chip.quiescent) ctx->failed = TRUE;
        *output = result; /* Input/output alias: consume all firmware first. */
        WdfRequestCompleteWithInformation(request, STATUS_SUCCESS, sizeof(result)); return;
    }
    if (code == IOCTL_MT_PREFLIGHT) {
        PUCHAR data;
        mt_fw_preflight result, *output;
        if (!ctx->prepared || input_length > 16u + 2u*MT_FW_MAX_SIZE) {
            WdfRequestComplete(request, STATUS_INVALID_PARAMETER); return;
        }
        status = WdfRequestRetrieveInputBuffer(request, 16, (PVOID *)&data, NULL);
        if (!NT_SUCCESS(status)) { WdfRequestComplete(request, status); return; }
        if (mt_fw_preflight_bundle(data, (uint32_t)input_length, &result) != MT_OK) {
            WdfRequestComplete(request, STATUS_INVALID_PARAMETER); return;
        }
        status = WdfRequestRetrieveOutputBuffer(request, sizeof(result), (PVOID *)&output, NULL);
        if (!NT_SUCCESS(status)) { WdfRequestComplete(request, status); return; }
        *output = result; /* Buffered input and output may alias; copy only after parsing. */
        WdfRequestCompleteWithInformation(request, STATUS_SUCCESS, sizeof(result)); return;
    }
    if (code == IOCTL_MT_PROBE) {
        MT_PROBE *probe;
        uint32_t values[10];
        ULONG i;
        static const uint32_t offsets[10] = {0xe0010u,0xe00f0u,0xd4208u,0xd4204u,
            0xd43f8u,0xd43fcu,0xd4408u,0xd440cu,0xd4508u,0xd450cu};
        if (input_length || !ctx->prepared || !ctx->bar || ctx->bar_bytes < 0xe00f4u) {
            WdfRequestComplete(request, STATUS_INVALID_DEVICE_STATE); return;
        }
        status = WdfRequestRetrieveOutputBuffer(request, sizeof(*probe), (PVOID *)&probe, NULL);
        if (!NT_SUCCESS(status)) { WdfRequestComplete(request, status); return; }
        /* Fixed read-only BAR offsets from mt7925's mapping, never supplied by user.
         * This may observe firmware ownership; it does not acquire ownership,
         * reset the card, publish DMA addresses, or enable an interrupt. */
        for (i = 0; i < RTL_NUMBER_OF(offsets); ++i) {
            if (MtReadRegister(ctx, offsets[i], &values[i]) != MT_OK) {
                WdfRequestComplete(request, STATUS_DEVICE_HARDWARE_ERROR); return;
            }
        }
        probe->size = sizeof(*probe); probe->version = 1;
        RtlCopyMemory(&probe->ownership, values, sizeof(values));
        WdfRequestCompleteWithInformation(request, STATUS_SUCCESS, sizeof(*probe)); return;
    }
    if (code != IOCTL_MT_GET_INFO) {
        WdfRequestComplete(request, STATUS_INVALID_DEVICE_REQUEST); return;
    }
    if (input_length != 0 || !ctx->prepared) {
        WdfRequestComplete(request, STATUS_INVALID_DEVICE_STATE); return;
    }
    status = WdfRequestRetrieveOutputBuffer(request, sizeof(*info), (PVOID *)&info, NULL);
    if (!NT_SUCCESS(status)) { WdfRequestComplete(request, status); return; }
    RtlZeroMemory(info, sizeof(*info));
    info->size = sizeof(*info); info->version = 1;
    info->vendor = 0x14c3; info->device = 0x7925;
    info->bar_bytes = ctx->bar_bytes; info->dma_address_bits = 32;
    info->mcu_entries = MT_MCU_ENTRIES; info->fwdl_entries = MT_FWDL_ENTRIES;
    info->buffer_bytes = MT_POOL_BYTES; info->prepared = TRUE;
    info->radio_ready = ctx->radio.session.ready && ctx->runtime.active && !ctx->radio.session.stopped;
    /* Reports completed local initialization, not over-air/iPhone acceptance. */
    WdfRequestCompleteWithInformation(request, STATUS_SUCCESS, sizeof(*info));
}

VOID MtControl(WDFQUEUE queue, WDFREQUEST request, size_t output_length,
               size_t input_length, ULONG code) {
    WDFDEVICE device = WdfIoQueueGetDevice(queue);
    MT_DEVICE *ctx = MtGetDevice(device);
    BOOLEAN failed;
    WdfWaitLockAcquire(ctx->lifecycle_lock, NULL);
    MtDispatch(queue, request, output_length, input_length, code);
    failed = ctx->failed;
    WdfWaitLockRelease(ctx->lifecycle_lock);
    /* Let the OS power down/remove the failed device. Do not free DMA memory
     * here and do not repeatedly restart a broken card. */
    if (failed) WdfDeviceSetFailed(device, WdfDeviceFailedNoRestart);
}
