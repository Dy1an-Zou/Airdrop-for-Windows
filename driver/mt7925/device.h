/* SPDX-License-Identifier: BSD-3-Clause-Clear */
#pragma once
#include <ntddk.h>
#include <wdf.h>
#include "core.h"
#include "firmware.h"
#include "mcu_runtime.h"
#include "chip_control.h"
#include "platform_acpi.h"
#include "radio_device.h"
#include "mcu_memory.h"

#define IOCTL_MT_GET_INFO CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_READ_DATA)
#define IOCTL_MT_PROBE CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_READ_DATA)
#define IOCTL_MT_PREFLIGHT CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA)
#define IOCTL_MT_BOOT_DIAGNOSTIC CTL_CODE(FILE_DEVICE_UNKNOWN, 0x803, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA)
#define IOCTL_MT_PLATFORM_INFO CTL_CODE(FILE_DEVICE_UNKNOWN, 0x804, METHOD_BUFFERED, FILE_READ_DATA)
#define IOCTL_MT_RADIO_START CTL_CODE(FILE_DEVICE_UNKNOWN, MT_RADIO_START, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA)
#define IOCTL_MT_RADIO_STATUS CTL_CODE(FILE_DEVICE_UNKNOWN, MT_RADIO_STATUS, METHOD_BUFFERED, FILE_READ_DATA)
#define IOCTL_MT_RADIO_CHANNEL CTL_CODE(FILE_DEVICE_UNKNOWN, MT_RADIO_CHANNEL, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA)
#define IOCTL_MT_RADIO_TX CTL_CODE(FILE_DEVICE_UNKNOWN, MT_RADIO_TX, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA)
#define IOCTL_MT_RADIO_POLL CTL_CODE(FILE_DEVICE_UNKNOWN, MT_RADIO_POLL, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA)
#define IOCTL_MT_RADIO_STOP CTL_CODE(FILE_DEVICE_UNKNOWN, MT_RADIO_STOP, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA)
#define IOCTL_MT_RADIO_ADDRESS CTL_CODE(FILE_DEVICE_UNKNOWN, MT_RADIO_ADDRESS, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA)
#define IOCTL_MT_RADIO_DIAGNOSTICS CTL_CODE(FILE_DEVICE_UNKNOWN, MT_RADIO_DIAGNOSTICS, METHOD_BUFFERED, FILE_READ_DATA)

typedef struct MT_PROBE {
    ULONG size, version, ownership, firmware_state, dma_config, interrupt_mask;
    ULONG command_cpu, command_dma, download_cpu, download_dma, rx_cpu, rx_dma;
} MT_PROBE;

/* Stable query ABI: no kernel virtual or DMA addresses cross the interface. */
typedef struct MT_INFO {
    ULONG size, version, vendor, device;
    ULONG bar_bytes, dma_address_bits, mcu_entries, fwdl_entries;
    ULONG buffer_bytes, prepared, radio_ready, reserved;
} MT_INFO;

typedef struct MT_DEVICE {
    PUCHAR bar;
    ULONG bar_bytes;
    WDFDMAENABLER dma;
    WDFCOMMONBUFFER descriptors;
    mt_mcu_memory mcu_memory;
    mt_mcu_runtime runtime;
    mt_chip chip;
    mt_data_memory data_memory;
    mt_raw_tx data_tx;
    mt_radio_device radio;
    uint32_t tx_states[MT_DATA_TX_TOKENS], tx_slots[MT_DATA_TX_ENTRIES];
    uint8_t radio_scratch[MT_DATA_RX_CAPACITY];
    WDFWAITLOCK lifecycle_lock;
    volatile LONG removed;
    BOOLEAN prepared, powered, writes_enabled, failed;
} MT_DEVICE;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(MT_DEVICE, MtGetDevice)

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD MtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE MtPrepare;
EVT_WDF_DEVICE_RELEASE_HARDWARE MtRelease;
EVT_WDF_DEVICE_D0_ENTRY MtD0Entry;
EVT_WDF_DEVICE_D0_EXIT MtD0Exit;
EVT_WDF_DEVICE_SURPRISE_REMOVAL MtSurpriseRemoval;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL MtControl;
EVT_WDF_FILE_CLEANUP MtFileCleanup;
