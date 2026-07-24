#include "nt/functions.hpp"
#include "ioctl/ioctl.hpp"
#include "ioctl/dispatch.hpp"

namespace memview {
namespace {

VOID Unload(PDRIVER_OBJECT driver)
{
    UNICODE_STRING link = RTL_CONSTANT_STRING(MEMVIEW_SYMLINK_NAME);
    IoDeleteSymbolicLink(&link);

    if (driver->DeviceObject)
        IoDeleteDevice(driver->DeviceObject);
}

} // namespace

extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT driver, PUNICODE_STRING registryPath)
{
    UNREFERENCED_PARAMETER(registryPath);

    UNICODE_STRING devName  = RTL_CONSTANT_STRING(MEMVIEW_DEVICE_NAME);
    UNICODE_STRING linkName = RTL_CONSTANT_STRING(MEMVIEW_SYMLINK_NAME);

    PDEVICE_OBJECT device = nullptr;
    NTSTATUS status = IoCreateDevice(driver, 0, &devName, FILE_DEVICE_UNKNOWN,
                                     FILE_DEVICE_SECURE_OPEN, FALSE, &device);
    if (!NT_SUCCESS(status))
        return status;

    status = IoCreateSymbolicLink(&linkName, &devName);
    if (!NT_SUCCESS(status))
    {
        IoDeleteDevice(device);
        return status;
    }

    driver->MajorFunction[IRP_MJ_CREATE]         = CreateClose;
    driver->MajorFunction[IRP_MJ_CLOSE]          = CreateClose;
    driver->MajorFunction[IRP_MJ_DEVICE_CONTROL] = DeviceControl;
    driver->DriverUnload                         = Unload;

    // buffered I/O so IRP data arrives in AssociatedIrp.SystemBuffer.
    device->Flags |= DO_BUFFERED_IO;
    device->Flags &= ~DO_DEVICE_INITIALIZING;

    return STATUS_SUCCESS;
}

} // namespace memview
