@echo off
setlocal
rem Builds the mod and lays out a release in dist\Squadsight-<version>\: every
rem file an install copies, each to be attached to the release on its own, and
rem Squadsight-Setup.exe, the one file a player downloads (setup.c), which
rem fetches the others from the release.  The version is version.h's
rem MOD_VERSION; bump it there first.  The file list is the launcher's own
rem (install.c, FILES), staged with `launcher.exe /stage`, so that what is
rem attached and what an install copies cannot drift apart.
rem
rem Then publish it as a GitHub release tagged v<version>, every file of the
rem folder attached; the launcher offers it to everyone on an older version:
rem   gh release create v<version> dist\Squadsight-<version>\* --title "Squadsight <version>" --notes "..."

set HERE=%~dp0
call "%HERE%build.bat"
if errorlevel 1 (echo PACKAGE: BUILD FAILED & exit /b 1)

for /f "tokens=3" %%v in ('findstr /b /c:"#define MOD_VERSION " "%HERE%version.h"') do set VER=%%~v
if "%VER%"=="" (echo PACKAGE: NO MOD_VERSION IN version.h & exit /b 1)

set NAME=Squadsight-%VER%
set DIST=%HERE%dist
if exist "%DIST%\%NAME%" rmdir /s /q "%DIST%\%NAME%"
if not exist "%DIST%" mkdir "%DIST%"

start "" /wait "%HERE%build\launcher.exe" /stage "%DIST%\%NAME%"
if errorlevel 1 (echo PACKAGE: STAGING FAILED, see launcher.log & exit /b 1)

copy /y "%HERE%build\Squadsight-Setup.exe" "%DIST%\%NAME%\Squadsight-Setup.exe" >nul
if errorlevel 1 (echo PACKAGE: SETUP COPY FAILED & exit /b 1)

echo.
echo Staged: %DIST%\%NAME%
echo Publish: gh release create v%VER% "%DIST%\%NAME%\*" --title "Squadsight %VER%"
