@echo off
setlocal
rem Builds the mod and packs a release: dist\Squadsight-<version>.zip, holding
rem one folder of everything an install copies.  The version is version.h's
rem MOD_VERSION; bump it there first.  The file list is the launcher's own
rem (install.c, FILES), staged with `launcher.exe /stage`, so that what is
rem zipped and what an install copies cannot drift apart.
rem
rem Then publish it as a GitHub release tagged v<version>, with the zip
rem attached; the launcher offers it to everyone on an older version:
rem   gh release create v<version> dist\Squadsight-<version>.zip --title "Squadsight <version>" --notes "..."

set HERE=%~dp0
call "%HERE%build.bat"
if errorlevel 1 (echo PACKAGE: BUILD FAILED & exit /b 1)

for /f "tokens=3" %%v in ('findstr /b /c:"#define MOD_VERSION " "%HERE%version.h"') do set VER=%%~v
if "%VER%"=="" (echo PACKAGE: NO MOD_VERSION IN version.h & exit /b 1)

set NAME=Squadsight-%VER%
set DIST=%HERE%dist
if exist "%DIST%\%NAME%" rmdir /s /q "%DIST%\%NAME%"
if exist "%DIST%\%NAME%.zip" del /q "%DIST%\%NAME%.zip"
if not exist "%DIST%" mkdir "%DIST%"

start "" /wait "%HERE%build\launcher.exe" /stage "%DIST%\%NAME%"
if errorlevel 1 (echo PACKAGE: STAGING FAILED, see launcher.log & exit /b 1)

rem Windows' own tar (bsdtar) writes zip when the name ends in .zip; Git's GNU
rem tar, if it is first on PATH, cannot.
"%SystemRoot%\System32\tar.exe" -a -c -f "%DIST%\%NAME%.zip" -C "%DIST%" "%NAME%"
if errorlevel 1 (echo PACKAGE: ZIP FAILED & exit /b 1)

echo.
echo Packed: %DIST%\%NAME%.zip
echo Publish: gh release create v%VER% "%DIST%\%NAME%.zip" --title "Squadsight %VER%"
