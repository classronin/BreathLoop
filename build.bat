@echo off
setlocal EnableExtensions EnableDelayedExpansion
cd /d "%~dp0"
rem  Some shells hand us a PATH without System32 (then where/powershell vanish).
set "PATH=%SystemRoot%\System32;%SystemRoot%;%SystemRoot%\System32\WindowsPowerShell\v1.0;%PATH%"

rem =====================================================================
rem  BreathLoop build script (Release, static link, no ImGui)
rem  Usage:  build.bat            full rebuild
rem          build.bat fast       incremental (no configure, no clean)
rem  Output: BreathLoop.exe (project root) + out\build_log.txt
rem =====================================================================

set "FAST=0"
if /i "%~1"=="fast" set "FAST=1"

rem ---- 1. MSVC environment -------------------------------------------
set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" (
  for /f "usebackq delims=" %%I in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%I"
  if defined VSDIR set "VCVARS=!VSDIR!\VC\Auxiliary\Build\vcvars64.bat"
)
if not exist "%VCVARS%" (
  echo [build] ERROR: vcvars64.bat not found.
  echo [build] Install Visual Studio 2022 with "Desktop development with C++".
  exit /b 1
)
echo [build] vcvars: %VCVARS%
call "%VCVARS%" >nul
if errorlevel 1 (
  echo [build] ERROR: vcvars64.bat failed.
  exit /b 1
)

rem ---- 2. Windows SDK ------------------------------------------------
rem  On some machines vcvars64 does not put the Windows SDK into INCLUDE/LIB,
rem  and the compile then dies on crtdbg.h / windows.h / libcpmt.lib.
rem  Prepend the newest 10.x SDK we can find (prepending is harmless when
rem  vcvars already provided it).
set "SDKROOT=C:\Program Files (x86)\Windows Kits\10"
set "SDKVER="
if exist "%SDKROOT%\Include" (
  for /d %%D in ("%SDKROOT%\Include\10.*") do (
    if not defined SDKVER (
      set "SDKVER=%%~nxD"
    ) else (
      if "%%~nxD" GTR "!SDKVER!" set "SDKVER=%%~nxD"
    )
  )
)
if defined SDKVER (
  set "INCLUDE=%SDKROOT%\Include\!SDKVER!\ucrt;%SDKROOT%\Include\!SDKVER!\um;%SDKROOT%\Include\!SDKVER!\shared;!INCLUDE!"
  set "LIB=%SDKROOT%\Lib\!SDKVER!\ucrt\x64;%SDKROOT%\Lib\!SDKVER!\um\x64;!LIB!"
  echo [build] Windows SDK: !SDKVER!
) else (
  echo [build] WARNING: no Windows SDK found under "%SDKROOT%".
)

rem ---- 3. CMake ------------------------------------------------------
where /q cmake
if errorlevel 1 (
  echo [build] ERROR: cmake not in PATH.
  exit /b 1
)
where /q ninja
if errorlevel 1 (
  echo [build] ERROR: ninja not in PATH ^(CMake preset uses the Ninja generator^).
  exit /b 1
)
if not exist "out" mkdir "out"

if "%FAST%"=="0" (
  echo [build] configure: cmake --preset release
  cmake --preset release
  if errorlevel 1 (
    echo [build] ERROR: configure failed. See the messages above.
    exit /b 1
  )
  echo [build] build: cmake --build --preset release --clean-first
  cmake --build --preset release --clean-first > "out\build_log.txt" 2>&1
) else (
  echo [build] build: cmake --build --preset release ^(incremental^)
  cmake --build --preset release > "out\build_log.txt" 2>&1
)

if errorlevel 1 (
  echo.
  echo [build] FAILED - tail of out\build_log.txt:
  powershell -NoProfile -Command "Get-Content -LiteralPath 'out\build_log.txt' -Tail 40"
  exit /b 1
)

echo.
echo [build] OK - BreathLoop.exe built.
powershell -NoProfile -Command "$f = Get-Item -LiteralPath 'BreathLoop.exe'; 'size   : {0:N0} bytes' -f $f.Length; 'sha256 : ' + ([BitConverter]::ToString([Security.Cryptography.SHA256]::Create().ComputeHash([IO.File]::ReadAllBytes($f.FullName)))).Replace('-', ''); 'log    : out\build_log.txt'"
exit /b 0