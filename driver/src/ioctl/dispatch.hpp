#pragma once

#include "../nt/functions.hpp"

namespace memview {

// IRP_MJ_CREATE / IRP_MJ_CLOSE: nothing to set up per-open, just succeed.
NTSTATUS CreateClose(PDEVICE_OBJECT device, PIRP irp);

// IRP_MJ_DEVICE_CONTROL: validates and dispatches each MEMVIEW_IOCTL_* to the
// matching memory/memory.hpp call.
NTSTATUS DeviceControl(PDEVICE_OBJECT device, PIRP irp);

} // namespace memview
