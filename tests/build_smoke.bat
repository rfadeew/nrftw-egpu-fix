@echo off
rem Builds build\smoke.exe (D3D12 smoke test for build\version.dll). Run: build\smoke.exe [frames]
setlocal
cd /d "%~dp0.."
where cl >nul 2>nul
if not errorlevel 1 goto :have_cl
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
for /f "usebackq tokens=*" %%i in (`vswhere.exe -latest -property installationPath`) do set "VSDIR=%%i"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
:have_cl
if not exist build mkdir build
cl /nologo /O2 /W3 /EHsc /MT /DUNICODE /D_UNICODE /Fobuild\ tests\smoke.cpp /link /OUT:build\smoke.exe
if errorlevel 1 (echo BUILD FAILED & exit /b 1)
echo Built build\smoke.exe
