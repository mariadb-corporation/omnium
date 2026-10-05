@echo off
rem  omnium - Windows build
rem
rem  omnium is written against POSIX: it forks and execs child processes, keeps a control pipe on
rem  fd 3 and 4 and reads /proc, and its selftest opens a pty to check the live view. Windows has no native equivalent
rem  for that process model, so the Windows build is made under MSYS2, whose runtime provides it.
rem  The result is a Windows .exe that behaves the same as the Linux binary for everything that
rem  does not need a Linux-only facility. docs/windows.md covers the Windows side, including
rem  what does not carry over.
rem
rem  usage:  build_windows.bat [release|debug|clean]

setlocal enabledelayedexpansion
set MODE=%1
if "%MODE%"=="" set MODE=release

rem ---- MSYS2 --------------------------------------------------------------------------------
set MSYS=
if exist "C:\msys64\usr\bin\bash.exe" set MSYS=C:\msys64
if exist "%SystemDrive%\msys64\usr\bin\bash.exe" set MSYS=%SystemDrive%\msys64
if defined MSYS2_ROOT if exist "%MSYS2_ROOT%\usr\bin\bash.exe" set MSYS=%MSYS2_ROOT%

if not defined MSYS (
  echo [build_windows] MSYS2 was not found.
  echo.
  echo   Install it once, then run this script again:
  echo.
  echo     winget install --id MSYS2.MSYS2
  echo.
  echo   Or download the installer from https://www.msys2.org and accept the default
  echo   location C:\msys64. If it is somewhere else, set MSYS2_ROOT to that folder:
  echo.
  echo     set MSYS2_ROOT=D:\msys64
  echo.
  exit /b 1
)
echo [build_windows] MSYS2 at %MSYS%

rem ---- the packages the build and the runs need ---------------------------------------------
rem  Names are the MSYS2 (not the mingw) packages, because omnium is built against the MSYS2
rem  runtime for fork and the rest. build.sh compiles with clang++ when the box has it, else g++.
rem  MSYS2 has no package for the MariaDB client library under that runtime, so build.sh builds
rem  Connector/C from source with connector_windows.sh the first time: that is what cmake and the
rem  openssl and zlib headers are for.
rem  openssl, xz, findutils and perl are called by the trials, not by the build: the encryption
rem  area's key files, the INFILE extraction, the all-disk SQL index and the MTR runner.
set PKGS=gcc binutils ninja ccache git cmake pcre2-devel libcurl-devel
set PKGS=%PKGS% openssl-devel zlib-devel openssl xz findutils perl
set MISSING=
for %%P in (%PKGS%) do (
  "%MSYS%\usr\bin\bash.exe" -lc "pacman -Q %%P >/dev/null 2>&1" || set MISSING=!MISSING! %%P
)
if not "!MISSING!"=="" (
  echo [build_windows] These MSYS2 packages are missing:!MISSING!
  echo.
  echo   Install them with:
  echo.
  echo     "%MSYS%\usr\bin\bash.exe" -lc "pacman -S --needed --noconfirm!MISSING!"
  echo.
  echo   If pacman itself is not there, open "MSYS2 MSYS" from the Start menu once and run:
  echo.
  echo     pacman -Syu
  echo.
  exit /b 1
)

rem ---- build ---------------------------------------------------------------------------------
rem  A login shell starts in the home folder unless CHERE_INVOKING is set, so the shell is started
rem  in the folder of this script and no Windows path has to be turned into a POSIX one.
set HERE=%~dp0
pushd "%HERE%"
set CHERE_INVOKING=1
"%MSYS%\usr\bin\bash.exe" -lc "./build.sh %MODE%"
set RC=%errorlevel%
popd
if not "%RC%"=="0" (
  echo [build_windows] the build failed; the lines above say why
  exit /b 1
)
set BIN=omnium.exe
if /i "%MODE%"=="debug" set BIN=omnium_dbg.exe
if /i "%MODE%"=="dbg" set BIN=omnium_dbg.exe
echo [build_windows] done. The binary is %BIN% in %HERE%
echo [build_windows] run it from an MSYS2 shell, or add %MSYS%\usr\bin to PATH.
exit /b 0

