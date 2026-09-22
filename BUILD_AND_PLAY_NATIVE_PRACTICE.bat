@echo off
setlocal EnableExtensions DisableDelayedExpansion
title Melee Unlocked - Build and Play Native Practice

rem The file also works when downloaded by itself. In that case, clone the
rem feature branch beside it and hand control to the copy inside the checkout.
if exist "%~dp0CMakeLists.txt" if exist "%~dp0tools\native_practice_diagnostic.ps1" goto :repo_ready

set "CHECKOUT=%~dp0Melee-Unlocked-Native-Practice"
if exist "%CHECKOUT%\BUILD_AND_PLAY_NATIVE_PRACTICE.bat" goto :run_existing_checkout
if exist "%CHECKOUT%" (
  echo Cannot create the checkout because this path already exists:
  echo   "%CHECKOUT%"
  echo Rename or remove that incomplete folder, then run this file again.
  pause
  exit /b 1
)

set "GIT_EXE=git"
where git >nul 2>nul
if errorlevel 1 (
  echo Git is missing. Installing Git for Windows...
  winget install --id Git.Git -e --accept-source-agreements --accept-package-agreements
  if errorlevel 1 (
    echo Git could not be installed automatically.
    pause
    exit /b 1
  )
  set "GIT_EXE=%ProgramFiles%\Git\cmd\git.exe"
  if not exist "%ProgramFiles%\Git\cmd\git.exe" (
    echo Git was installed, but Windows needs a restart before it is available.
    echo Restart Windows, then open this BAT again.
    pause
    exit /b 1
  )
)

echo Downloading the native-practice branch...
"%GIT_EXE%" clone --branch feature/native-practice --single-branch https://github.com/finalstock-dev/melee-unlocked.git "%CHECKOUT%"
if errorlevel 1 (
  echo The repository download failed.
  pause
  exit /b 1
)

:run_existing_checkout
call "%CHECKOUT%\BUILD_AND_PLAY_NATIVE_PRACTICE.bat" %*
exit /b %ERRORLEVEL%

:repo_ready
cd /d "%~dp0"

echo ============================================================
echo   Melee Unlocked - Native Practice / Unranked test build
echo ============================================================
echo.
echo This keeps your ISO local. It is read to build the native port
echo and is never copied into the repository or the output package.
echo.

set "ISO_PATH=%~1"
if not defined ISO_PATH if defined MELEE_ISO set "ISO_PATH=%MELEE_ISO%"
if not defined ISO_PATH if exist "%~dp0melee.iso" set "ISO_PATH=%~dp0melee.iso"

if not defined ISO_PATH (
  echo Choose your clean NTSC 1.02 Melee ISO in the window that opens.
  for /f "usebackq delims=" %%I in (`powershell -NoProfile -ExecutionPolicy Bypass -Command "Add-Type -AssemblyName System.Windows.Forms; $d = New-Object System.Windows.Forms.OpenFileDialog; $d.Title = 'Choose your clean Melee NTSC 1.02 ISO'; $d.Filter = 'GameCube ISO (*.iso)|*.iso|All files (*.*)|*.*'; if ($d.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) { $d.FileName }"`) do set "ISO_PATH=%%I"
)

if not defined ISO_PATH (
  echo.
  echo No ISO was selected. Nothing was built or changed.
  goto :fail
)
if not exist "%ISO_PATH%" (
  echo.
  echo ISO not found: "%ISO_PATH%"
  goto :fail
)

echo.
echo [1/3] Building the Release executable...
set "MELEE_BUILD_NO_PAUSE=1"
call build.bat "%ISO_PATH%"
set "BUILD_EXIT=%ERRORLEVEL%"
set "MELEE_BUILD_NO_PAUSE="
if not "%BUILD_EXIT%"=="0" goto :fail

echo.
echo [2/3] Building and running the native-practice model test...
cmake --build build-review --config Release --target native_practice_model_test --parallel
if errorlevel 1 goto :fail
ctest --test-dir build-review -C Release -R native_practice_model --output-on-failure
if errorlevel 1 goto :fail

echo.
echo [3/3] Starting Melee Unlocked...
echo.
echo In Training:
echo   1. Press Tab.
echo   2. Click Unranked.
echo   3. Keep practicing while the search timer runs.
echo   4. Press Tab again to view status or click Cancel.
echo.
powershell -NoProfile -ExecutionPolicy Bypass -File "tools\native_practice_diagnostic.ps1" -Iso "%ISO_PATH%" -Configuration Release -SkipBuild
set "GAME_EXIT=%ERRORLEVEL%"

echo.
if "%GAME_EXIT%"=="0" (
  echo The game closed normally. The diagnostic report is under:
  echo   reports\native-practice-YYYYMMDD-HHMMSS\summary.txt
) else (
  echo The game or diagnostic exited with code %GAME_EXIT%.
  echo Check the newest reports\native-practice-* folder and melee_port.log.
)
echo.
pause
exit /b %GAME_EXIT%

:fail
echo.
echo Build did not complete. Read the error above, then press any key.
pause
exit /b 1
