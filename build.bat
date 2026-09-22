@echo off
setlocal
rem One-command build from source: build.bat <path to Melee NTSC 1.02 ISO>
rem Drop the ISO onto this file to do the same. Installs missing tools with winget when it can.
cd /d "%~dp0"
set ISO=%~1
if "%ISO%"=="" set ISO=%~dp0melee.iso
if not exist "%ISO%" (
  echo Usage: build.bat ^<path to Melee NTSC 1.02 ISO^>   or drop the ISO onto build.bat
  pause
  exit /b 1
)

where python >nul 2>nul || (echo Installing Python... & winget install --id Python.Python.3.12 -e --accept-source-agreements --accept-package-agreements || goto :tools)
where cmake >nul 2>nul || (echo Installing CMake... & winget install --id Kitware.CMake -e --accept-source-agreements --accept-package-agreements || goto :tools)
set VSWHERE="%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist %VSWHERE% (
  echo Installing Visual Studio 2022 Build Tools with the C++ workload ^(this takes a while^)...
  winget install --id Microsoft.VisualStudio.2022.BuildTools -e --accept-source-agreements --accept-package-agreements --override "--quiet --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended" || goto :tools
)

echo.
echo [1/4] Extracting main.dol from the ISO
python tools\extract_dol.py "%ISO%" build\main.dol || goto :fail
echo [2/4] Translating the game to C++ ^(about 20 s^)
python port\recomp\recomp.py --dol build\main.dol --gct-base 0x8065CC80 || goto :fail
echo [3/4] Configuring
cmake -S . -B build-review -G "Visual Studio 17 2022" -A x64 -DMELEE_BUILD_EXPERIMENTAL_PORT=ON || goto :fail
echo [4/4] Compiling ^(20 to 40 minutes the first time^)
cmake --build build-review --config Release --target melee_port --parallel || goto :fail
echo.
echo Done: build-review\port\Release\melee_port.exe
echo Play with:  play.bat "%ISO%"
if not defined MELEE_BUILD_NO_PAUSE if not "%~1"=="" pause
exit /b 0

:tools
echo Could not install the tools automatically. Install Python 3, CMake and Visual Studio 2022
echo Build Tools ^(C++ desktop workload^), then run this file again.
if not defined MELEE_BUILD_NO_PAUSE pause
exit /b 1

:fail
echo Build failed; see the messages above.
if not defined MELEE_BUILD_NO_PAUSE pause
exit /b 1
