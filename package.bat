@echo off
rem Builds the release DLL and packs dist\No-Rest-for-the-Wicked-eGPU-Fix-v<version>.zip.
rem Prints the SHA-256 of the zip and of version.dll for the release notes.
setlocal
cd /d "%~dp0"

call "%~dp0build.bat" || exit /b 1

for /f "tokens=3 delims= " %%v in ('findstr /c:"#define EGPU_FIX_VERSION_STR" src\egpu_version.h') do set "VER=%%~v"
if not defined VER (echo cannot read the version from src\egpu_version.h & exit /b 1)
set "NAME=No-Rest-for-the-Wicked-eGPU-Fix-v%VER%"
set "STAGE=dist\%NAME%"

if exist "%STAGE%" rmdir /s /q "%STAGE%"
if exist "dist\%NAME%.zip" del /q "dist\%NAME%.zip"
mkdir "%STAGE%" || exit /b 1

copy /y build\version.dll        "%STAGE%\" >nul || exit /b 1
copy /y package\egpu_fix.ini     "%STAGE%\" >nul || exit /b 1
copy /y package\README.txt       "%STAGE%\" >nul || exit /b 1
copy /y LICENSE                  "%STAGE%\LICENSE.txt" >nul || exit /b 1
copy /y THIRD_PARTY_NOTICES.md   "%STAGE%\THIRD_PARTY_NOTICES.md" >nul || exit /b 1

rem The release files must not contain local paths (user folders, Steam library, build directories).
powershell -NoProfile -ExecutionPolicy Bypass -File tools\check_no_paths.ps1 -Dir "%STAGE%"
if errorlevel 1 (echo ERROR: a release file contains a local path & exit /b 1)

findstr /c:"GITHUB_URL" "%STAGE%\README.txt" >nul && echo WARNING: package\README.txt still has the GITHUB_URL placeholder

powershell -NoProfile -Command "Compress-Archive -Path '%STAGE%\*' -DestinationPath 'dist\%NAME%.zip' -Force" || exit /b 1

echo.
echo Package: dist\%NAME%.zip
powershell -NoProfile -Command "Get-FileHash -Algorithm SHA256 'dist\%NAME%.zip','%STAGE%\version.dll' | ForEach-Object { '{0}  {1}' -f $_.Hash.ToLower(), (Split-Path $_.Path -Leaf) }"
