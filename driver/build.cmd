@echo off

rem Usage: build.cmd [debug|release]  (defaults to release)
set "CONFIG=%~1"
if not defined CONFIG set "CONFIG=release"
if /i not "%CONFIG%"=="debug" if /i not "%CONFIG%"=="release" (
    echo ERROR: unknown config "%CONFIG%" - expected "debug" or "release".
    exit /b 1
)

for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
    call "%%i\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64
)
if not defined VCINSTALLDIR (
    echo ERROR: No Visual Studio installation with VC++ tools found.
    echo Install Visual Studio with the "Desktop development with C++" workload.
    exit /b 1
)

pushd "%~dp0"
cmake --preset %CONFIG% || (popd & exit /b 1)
cmake --build --preset %CONFIG% || (popd & exit /b 1)
popd

echo.
echo Built and test-signed driver\build\%CONFIG%\memview.sys - copy it next to memview.exe.
echo (If the sign step above warned about elevation, re-run from an elevated shell.)
