//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// What every port has, whichever kind of driver carries it: the COM name, the
// two interfaces, the file object rules, the device context and the queues.
// The driver of each kind (driver.c for the kernel-mode driver, vsscom_um/ for
// the user-mode one) adds what only its framework has, such as a name for the
// device object, and then calls VssComCreatePort.
//

#include "vsscom.h"


// Gives applications the COM name the Ports class installer chose.
//
// The installer writes PortName under the device's hardware key. A serial
// driver then has three jobs of its own, the same three the Microsoft
// VirtualSerial2 sample does: expose GUID_DEVINTERFACE_COMPORT, make the
// \DosDevices\Global\COMn symbolic link that CreateFile("\\.\COMn") resolves,
// and list the port under HARDWARE\DEVICEMAP\SERIALCOMM, which is what
// SerialPort.GetPortNames and most port pickers read.
NTSTATUS VssComRegisterPort(_In_ PVSSCOM_DEVICE_CONTEXT Context)
{
    WDFDEVICE device = Context->Device;
    WDFKEY key = NULL;
    WCHAR portNameBuffer[16];
    WCHAR linkBuffer[48];
    UNICODE_STRING portName;
    UNICODE_STRING link;
    UNICODE_STRING pdoName;
    WDFMEMORY memory = NULL;
    WDF_OBJECT_ATTRIBUTES attributes;
    DECLARE_CONST_UNICODE_STRING(portNameValue, L"PortName");
    DECLARE_CONST_UNICODE_STRING(serialCommKey, L"SERIALCOMM");

    NTSTATUS status = WdfDeviceCreateDeviceInterface(
        device, &GUID_DEVINTERFACE_COMPORT, NULL);
    if (!NT_SUCCESS(status))
        return status;

    status = WdfDeviceOpenRegistryKey(device, PLUGPLAY_REGKEY_DEVICE,
                                      KEY_QUERY_VALUE, WDF_NO_OBJECT_ATTRIBUTES,
                                      &key);
    if (!NT_SUCCESS(status))
        return status;

    RtlInitEmptyUnicodeString(&portName, portNameBuffer, sizeof(portNameBuffer));
    status = WdfRegistryQueryUnicodeString(key, &portNameValue, NULL, &portName);
    WdfRegistryClose(key);
    key = NULL;
    if (!NT_SUCCESS(status))
        return status;

    RtlInitEmptyUnicodeString(&link, linkBuffer, sizeof(linkBuffer));
    status = RtlAppendUnicodeToString(&link, L"\\DosDevices\\Global\\");
    if (NT_SUCCESS(status))
        status = RtlAppendUnicodeStringToString(&link, &portName);
    if (!NT_SUCCESS(status))
        return status;

    status = WdfDeviceCreateSymbolicLink(device, &link);
    if (!NT_SUCCESS(status))
        return status;

    // The legacy list is keyed by the device object's name.
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = device;
    status = WdfDeviceAllocAndQueryProperty(
        device, DevicePropertyPhysicalDeviceObjectName, NonPagedPoolNx,
        &attributes, &memory);
    if (!NT_SUCCESS(status))
        return status;
    Context->PdoName = (PWSTR)WdfMemoryGetBuffer(memory, NULL);
    RtlInitUnicodeString(&pdoName, Context->PdoName);

    status = WdfDeviceOpenDevicemapKey(device, &serialCommKey, KEY_SET_VALUE,
                                       WDF_NO_OBJECT_ATTRIBUTES, &key);
    if (!NT_SUCCESS(status))
        return status;
    status = WdfRegistryAssignUnicodeString(key, &pdoName, &portName);
    WdfRegistryClose(key);
    if (NT_SUCCESS(status))
        Context->CreatedSerialCommValue = TRUE;
    return status;
}

// Takes the port off the legacy list again. The symbolic link and the device
// interface go with the device object.
VOID VssComEvtDeviceCleanup(_In_ WDFOBJECT Object)
{
    WDFDEVICE device = (WDFDEVICE)Object;
    PVSSCOM_DEVICE_CONTEXT context = VssComGetContext(device);
    WDFKEY key = NULL;
    UNICODE_STRING pdoName;
    DECLARE_CONST_UNICODE_STRING(serialCommKey, L"SERIALCOMM");

    if (!context->CreatedSerialCommValue || context->PdoName == NULL)
        return;

    if (NT_SUCCESS(WdfDeviceOpenDevicemapKey(device, &serialCommKey,
                                             KEY_SET_VALUE,
                                             WDF_NO_OBJECT_ATTRIBUTES, &key)))
    {
        RtlInitUnicodeString(&pdoName, context->PdoName);
        (void)WdfRegistryRemoveValue(key, &pdoName);
        WdfRegistryClose(key);
    }
}

NTSTATUS VssComCreatePort(_Inout_ PWDFDEVICE_INIT DeviceInit,
                          _In_ BOOLEAN OnDemand, _In_ ULONG Slot)
{
    WDF_FILEOBJECT_CONFIG fileConfig;
    WDF_OBJECT_ATTRIBUTES fileAttributes;
    WDF_OBJECT_ATTRIBUTES attributes;
    DECLARE_CONST_UNICODE_STRING(hubReference, VSSCOM_HUB_REFERENCE);
    WDFDEVICE device;
    PVSSCOM_DEVICE_CONTEXT context;
    NTSTATUS status;

    // Buffered I/O: a waiting read can be filled later from another thread.
    WdfDeviceInitSetIoType(DeviceInit, WdfDeviceIoBuffered);
    // Each end has one handle at a time. The device is not marked exclusive,
    // because that would shut the hub out while an application has the port;
    // VssComEvtDeviceFileCreate refuses a second handle on an end itself.
    // The file name tells the ends apart, so the framework must keep it.
    WDF_FILEOBJECT_CONFIG_INIT(&fileConfig, VssComEvtDeviceFileCreate,
                               WDF_NO_EVENT_CALLBACK, VssComEvtFileCleanup);
    fileConfig.FileObjectClass = WdfFileObjectWdfCanUseFsContext2;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&fileAttributes, VSSCOM_FILE_CONTEXT);
    WdfDeviceInitSetFileObjectConfig(DeviceInit, &fileConfig, &fileAttributes);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, VSSCOM_DEVICE_CONTEXT);
    attributes.EvtCleanupCallback = VssComEvtDeviceCleanup;

    status = WdfDeviceCreate(&DeviceInit, &attributes, &device);
    if (!NT_SUCCESS(status))
        return status;

    context = VssComGetContext(device);
    context->Device = device;
    context->OnDemand = OnDemand;
    context->Slot = Slot;
    context->BaudRate = 115200;
    context->LineControl.StopBits = STOP_BIT_1;
    context->LineControl.Parity = NO_PARITY;
    context->LineControl.WordLength = 8;
    vsscom_ring_init(&context->Rx, context->RxStorage, VSSCOM_RX_SIZE);
    vsscom_ring_init(&context->Tx, context->TxStorage, VSSCOM_TX_SIZE);
    vsscom_marks_init(&context->Marks);
    vsscom_events_init(&context->Events);

    // Child of the device, so that it goes when the device does. Ports now come
    // and go while the driver stays loaded.
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = device;
    status = VssComLockCreate(&attributes, &context->Lock);
    if (!NT_SUCCESS(status))
        return status;

    // The hub end: <interface path>\hub.
    status = WdfDeviceCreateDeviceInterface(device, &GUID_DEVINTERFACE_VSSCOM,
                                            &hubReference);
    if (!NT_SUCCESS(status))
        return status;

    status = VssComRegisterPort(context);
    if (!NT_SUCCESS(status))
        return status;

    return VssComCreateQueues(context);
}
