# driver

Optional kernel-mode component for reading/writing a target process's memory via
IOCTLs, for processes where `ReadProcessMemory` is blocked. The app works fine
without it - the "Use kernel mode driver" checkbox in Settings falls back to the
WinAPI path when it's not loaded.

## build

```bat
driver\build.cmd
```

Needs the WDK + matching Windows SDK. Output:
`driver\build\<debug|release>\memview.sys` - copy it next to `memview.exe`. Run
from an elevated shell and it also test-signs the result; otherwise it just
builds and warns.

## loading it

Windows won't load an unsigned driver by default:

```bat
bcdedit /set testsigning on
```

(needs a reboot). Then run MemView as Administrator and tick the driver checkbox
in Settings, or load it by hand:

```bat
sc create MemViewDrv type= kernel start= demand binPath= C:\full\path\to\memview.sys
sc start  MemViewDrv
```