@echo off
setlocal enableextensions enabledelayedexpansion
rem ===========================================================================
rem  make-arm64x-vk.bat - build the ARM64X venus Vulkan ICD.
rem
rem  Like the Triton UMD (see ..\..\neptune\triton\tools\make-arm64x.bat),
rem  venus is built twice, for arm64 and arm64ec, and the two builds ship
rem  behind one ARM64X DLL so that both native ARM64 and ARM64EC / x64-emulated
rem  Vulkan applications can load the ICD.
rem
rem  Unlike the UMD, the front DLL cannot be a code-less forwarder: the Vulkan
rem  loader looks the ICD entry points up with GetProcAddress, and a forwarded
rem  export is resolved through the default DLL search path, which does not
rem  include the DriverStore directory the payloads live in.  The front DLL is
rem  therefore a small loader stub (vk_arm64x_stub.c), compiled for arm64 and
rem  arm64ec and linked as ARM64X; each view loads its payload from the
rem  directory the stub lives in:
rem
rem     ARM64 native view --> vulkan_virtio_arm64.dll  (arm64 build)
rem     EC view           --> vulkan_virtio_ec.dll     (arm64ec build)
rem
rem  Inputs are routed by machine type, not by argument order:
rem     arm64    (dumpbin: "AA64 machine (ARM64)")           --> native view
rem     arm64ec  (dumpbin: "8664 machine (x64) (ARM64X)")    --> EC view
rem
rem  The deliverable is THREE files that must ship side by side:
rem     vulkan_virtio.dll        <- the ARM64X stub (what the loader opens)
rem     vulkan_virtio_arm64.dll  <- the arm64 build, renamed
rem     vulkan_virtio_ec.dll     <- the arm64ec build, renamed
rem
rem  Usage:
rem     make-arm64x-vk.bat <dll_a> <dll_b> [output_dir]
rem
rem     <dll_a> <dll_b>  the two vulkan_virtio.dll builds, in either order
rem     [output_dir]     where the three files are written (default: .\arm64x)
rem ===========================================================================

set "SCRIPT_DIR=%~dp0"
set "SRC_STUB=%SCRIPT_DIR%vk_arm64x_stub.c"

if "%~1"=="" goto :usage
if "%~2"=="" goto :usage
set "DLL_A=%~f1"
set "DLL_B=%~f2"
if "%~3"=="" (
  set "OUT_DIR=%CD%\arm64x"
) else (
  set "OUT_DIR=%~f3"
)

if not exist "%DLL_A%"    ( echo [error] input DLL not found: %DLL_A%& exit /b 1 )
if not exist "%DLL_B%"    ( echo [error] input DLL not found: %DLL_B%& exit /b 1 )
if not exist "%SRC_STUB%" ( echo [error] stub source not found: %SRC_STUB%& exit /b 1 )

rem --- Locate VsDevCmd.bat via vswhere. ---------------------------------------
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo [error] vswhere.exe not found; is Visual Studio installed?
  exit /b 1
)
set "VSINSTALL="
for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.ARM64 -property installationPath 2^>nul`) do set "VSINSTALL=%%I"
if not defined VSINSTALL (
  echo [error] no Visual Studio installation with the ARM64 build tools found.
  exit /b 1
)
set "VSDEVCMD=%VSINSTALL%\Common7\Tools\VsDevCmd.bat"
if not exist "%VSDEVCMD%" (
  echo [error] VsDevCmd.bat not found under: %VSINSTALL%
  exit /b 1
)

set "HOST_ARCH=x64"
if /i "%PROCESSOR_ARCHITECTURE%"=="ARM64" set "HOST_ARCH=arm64"
if /i "%PROCESSOR_ARCHITEW6432%"=="ARM64" set "HOST_ARCH=arm64"

echo [info] Visual Studio : %VSINSTALL%
echo [info] host/target    : %HOST_ARCH% -^> arm64x
echo [info] output dir     : %OUT_DIR%

for %%V in ("%VSWHERE%") do set "PATH=%%~dpV;%PATH%"
call "%VSDEVCMD%" -arch=arm64 -host_arch=%HOST_ARCH% -startdir=none -no_logo >nul
if errorlevel 1 (
  echo [error] VsDevCmd.bat failed to initialize the arm64 environment.
  exit /b 1
)

rem --- Route each input to a view by its machine type. -------------------------
set "NATIVE_INPUT="
set "EC_INPUT="
call :classify "%DLL_A%" ROLE_A
call :classify "%DLL_B%" ROLE_B
echo [info] %DLL_A% -^> %ROLE_A%
echo [info] %DLL_B% -^> %ROLE_B%
if "%ROLE_A%"=="native" if "%ROLE_B%"=="ec"     ( set "NATIVE_INPUT=%DLL_A%" & set "EC_INPUT=%DLL_B%" )
if "%ROLE_A%"=="ec"     if "%ROLE_B%"=="native" ( set "NATIVE_INPUT=%DLL_B%" & set "EC_INPUT=%DLL_A%" )
if not defined NATIVE_INPUT (
  echo [error] expected one arm64 input and one arm64ec input; got '%ROLE_A%' and '%ROLE_B%'.
  exit /b 1
)

rem --- Build the stub in a scratch directory. ----------------------------------
set "WORK=%TEMP%\venus-arm64x-%RANDOM%%RANDOM%"
mkdir "%WORK%" 2>nul
if not exist "%WORK%" ( echo [error] could not create work dir: %WORK%& exit /b 1 )

set "RC=0"
cl /nologo /c /O2 /MT /W3 /Fo"%WORK%\stub_arm64.obj" "%SRC_STUB%" || set "RC=1"
if "%RC%"=="0" cl /nologo /c /O2 /MT /W3 /arm64EC /Fo"%WORK%\stub_ec.obj" "%SRC_STUB%" || set "RC=1"
if "%RC%"=="0" link /nologo /dll /machine:arm64x /out:"%WORK%\vulkan_virtio.dll" "%WORK%\stub_arm64.obj" "%WORK%\stub_ec.obj" || set "RC=1"
if not "%RC%"=="0" (
  echo [error] building the ARM64X stub failed.
  rmdir /s /q "%WORK%" 2>nul
  exit /b 1
)

if not exist "%OUT_DIR%" mkdir "%OUT_DIR%"
copy /y "%WORK%\vulkan_virtio.dll" "%OUT_DIR%\vulkan_virtio.dll" >nul || set "RC=1"
copy /y "%NATIVE_INPUT%" "%OUT_DIR%\vulkan_virtio_arm64.dll" >nul || set "RC=1"
copy /y "%EC_INPUT%" "%OUT_DIR%\vulkan_virtio_ec.dll" >nul || set "RC=1"
rmdir /s /q "%WORK%" 2>nul
if not "%RC%"=="0" ( echo [error] copying the outputs failed.& exit /b 1 )

dumpbin /nologo /headers "%OUT_DIR%\vulkan_virtio.dll" | findstr /i "machine"
dumpbin /nologo /exports "%OUT_DIR%\vulkan_virtio.dll" | findstr /i "vk_icd"
echo [ok] wrote vulkan_virtio.dll, vulkan_virtio_arm64.dll, vulkan_virtio_ec.dll to %OUT_DIR%
exit /b 0

:usage
echo usage: %~nx0 ^<dll_a^> ^<dll_b^> [output_dir]
echo        the arm64 and arm64ec builds of vulkan_virtio.dll, in either order
exit /b 2

rem ---------------------------------------------------------------------------
rem  :classify <dll> <out_var> - native (AA64), ec (8664), arm64x or unknown
rem ---------------------------------------------------------------------------
:classify
setlocal enabledelayedexpansion
set "_line="
for /f "usebackq delims=" %%H in (`dumpbin /nologo /headers "%~1" 2^>nul ^| findstr /i "machine"`) do set "_line=%%H"
set "_role=unknown"
echo(!_line! | findstr /i "8664" >nul
if not errorlevel 1 (
  set "_role=ec"
) else (
  echo(!_line! | findstr /i "AA64" >nul
  if not errorlevel 1 (
    echo(!_line! | findstr /i "ARM64X" >nul
    if not errorlevel 1 ( set "_role=arm64x" ) else ( set "_role=native" )
  )
)
endlocal & set "%~2=%_role%"
exit /b
