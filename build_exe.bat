@echo off
rem Builds "Inventory Manager.exe" - one file that runs without Python installed.
rem Run this again after changing server.py or anything in the static folder.
setlocal
cd /d "%~dp0"

if not exist "build\venv\Scripts\python.exe" (
  echo Setting up the build tools - first time only...
  python -m venv build\venv || goto fail
)
rem Quick when everything is already installed; downloads anything missing (needs internet).
build\venv\Scripts\python.exe -m pip install --disable-pip-version-check -q pyinstaller pillow pystray || goto fail
if not exist "packaging\app.ico" (
  build\venv\Scripts\python.exe packaging\make_icon.py || goto fail
)

echo Building...
build\venv\Scripts\python.exe -m PyInstaller --noconfirm --clean --log-level WARN --onefile --noconsole ^
  --hidden-import pystray._win32 ^
  --name "Inventory Manager" ^
  --icon "%~dp0packaging\app.ico" ^
  --version-file "%~dp0packaging\version.txt" ^
  --add-data "%~dp0static;static" ^
  --add-data "%~dp0packaging\app.ico;." ^
  --distpath "%~dp0build\dist" --workpath "%~dp0build\work" --specpath "%~dp0build" ^
  server.py || goto fail

copy /y "build\dist\Inventory Manager.exe" "Inventory Manager.exe" >nul || (
  echo Couldn't replace "Inventory Manager.exe" - quit the app from its tray icon if it's running, then try again.
  goto fail
)
echo.
echo Done: "Inventory Manager.exe" is ready in this folder.
goto end

:fail
echo.
echo Build failed - see the messages above.

:end
pause
