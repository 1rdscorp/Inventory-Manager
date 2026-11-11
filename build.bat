@echo off
rem Builds build\InventoryManager.exe with the Microsoft C++ compiler, then the
rem installer (build\installer\InventoryManager-Setup-<version>.exe) if Inno Setup 6 is installed.
rem Needs: Visual Studio 2022+ or Build Tools with "Desktop development with C++", and internet
rem access the first time (to download the libraries listed in CMakeLists.txt).
setlocal
cd /d "%~dp0"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto novs
"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath > "%TEMP%\im-vsdir.txt"
set /p VSDIR=<"%TEMP%\im-vsdir.txt"
del "%TEMP%\im-vsdir.txt" >nul 2>&1
if not defined VSDIR goto novs
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || goto fail
rem Prefer the CMake and Ninja that come with Visual Studio (they use the Windows certificate store).
set "VSCMAKE=%VSDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake"
if exist "%VSCMAKE%\CMake\bin\cmake.exe" set "PATH=%VSCMAKE%\CMake\bin;%VSCMAKE%\Ninja;%PATH%"

echo Building Inventory Manager...
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl || goto fail
cmake --build build || goto fail
echo.
echo Built build\InventoryManager.exe

set "ISCC="
if exist "%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe" set "ISCC=%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe"
if exist "%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe" set "ISCC=%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe"
if exist "%ProgramFiles%\Inno Setup 6\ISCC.exe" set "ISCC=%ProgramFiles%\Inno Setup 6\ISCC.exe"
if not defined ISCC (
  echo Inno Setup 6 isn't installed, so the installer wasn't built.
  echo Get it from https://jrsoftware.org/isdl.php and run this again.
  goto end
)
echo Building the installer...
"%ISCC%" /Q installer\InventoryManager.iss || goto fail
echo Built the installer in build\installer
goto end

:novs
echo The Microsoft C++ build tools weren't found.
echo Install "Build Tools for Visual Studio" with "Desktop development with C++" and run this again.
:fail
echo.
echo Build failed - see the messages above.
if /i not "%~1"=="--no-pause" pause
exit /b 1

:end
if /i not "%~1"=="--no-pause" pause
exit /b 0
