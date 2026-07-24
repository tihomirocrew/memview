#pragma once
#include <string>

// User-mode client for the MemView kernel driver (driver/src/main.cpp): installs and
// starts it via the SCM, opens \\.\MemView, and registers mem::g_kernel. Needs the
// app to run elevated and the driver to be loadable (signed, or test signing on).
namespace mem::driver {

// Installs/starts the service, opens the device, registers the kernel backend.
// False leaves the backend unregistered with a reason in `status`. Idempotent.
bool start(std::string& status);

// Unregisters the backend and closes the device. removeService also stops/deletes
// the service; otherwise it's left registered so a later start() is cheap.
void stop(bool removeService = false);

// True while the device is open and the backend is registered.
bool active();

} // namespace mem::driver
