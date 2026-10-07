@echo off
rem Builds UE4SS + RoNPassthrough (Game__Shipping__Win64, Ninja, MSVC) into src\build.
setlocal
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VS=%%i
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set SRC=%~dp0..\src
if not exist "%SRC%\build\build.ninja" (
  cmake -S "%SRC%" -B "%SRC%\build" -G Ninja -DCMAKE_BUILD_TYPE=Game__Shipping__Win64 || exit /b 1
)
cmake --build "%SRC%\build" --target RoNPassthrough %* || exit /b 1
echo built: %SRC%\build\Game__Shipping__Win64\bin
