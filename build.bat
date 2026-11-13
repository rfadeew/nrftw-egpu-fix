@echo off
rem Builds build\version.dll. Works from any prompt: sets up the VS x64 environment if cl is not on PATH.
rem Needs Visual Studio 2019/2022 (or Build Tools) with the "Desktop development with C++" workload.
setlocal
cd /d "%~dp0"

where cl >nul 2>nul
if not errorlevel 1 goto :have_cl
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
set "VSDIR="
for /f "usebackq tokens=*" %%i in (`vswhere.exe -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (echo Visual Studio with C++ tools not found & exit /b 1)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
:have_cl

if not exist build mkdir build

set SRC=src\version_proxy.cpp src\hooks.cpp src\vtbl_index.c ^
    third_party\minhook\src\buffer.c third_party\minhook\src\hook.c ^
    third_party\minhook\src\trampoline.c third_party\minhook\src\hde\hde64.c

rc /nologo /Isrc /fo build\egpu_fix.res src\egpu_fix.rc
if errorlevel 1 (echo BUILD FAILED & exit /b 1)

rem /PDBALTPATH keeps only "version.pdb" in the DLL's debug directory (no local build path in the release file).
cl /nologo /LD /O2 /W3 /EHsc /std:c++17 /MT /Zi /DUNICODE /D_UNICODE ^
   /Isrc /Ithird_party\minhook\include /Fobuild\ /Fdbuild\ ^
   %SRC% build\egpu_fix.res ^
   /link /OUT:build\version.dll /PDB:build\version.pdb /PDBALTPATH:%%_PDB%% /IMPLIB:build\version.lib ^
   /DEBUG /OPT:REF /OPT:ICF
if errorlevel 1 (echo BUILD FAILED & exit /b 1)
echo Built build\version.dll
