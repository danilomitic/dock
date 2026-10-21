@echo off
rem Builds DockX (Release) with the Visual Studio 2022 Build Tools.
setlocal
set VSDEV="%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat"
if not exist %VSDEV% (
  echo Visual Studio 2022 Build Tools not found.
  exit /b 1
)
set CFG=%1
if "%CFG%"=="" set CFG=Release
call %VSDEV% -arch=x64 -host_arch=x64 >nul
cmake -S "%~dp0." -B "%~dp0build" -G Ninja -DCMAKE_BUILD_TYPE=%CFG% || exit /b 1
cmake --build "%~dp0build" || exit /b 1
ctest --test-dir "%~dp0build" --output-on-failure || exit /b 1
