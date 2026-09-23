@echo off
setlocal
rem Builds the 32-bit UI hook DLL and its injector.
rem The game is 32-bit, so vcvars32 (not vcvarsall x64) is required.

set VS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools
call "%VS%\VC\Auxiliary\Build\vcvars32.bat" >nul 2>&1
if errorlevel 1 (echo Could not initialise the 32-bit MSVC environment & exit /b 1)

set HERE=%~dp0
set MH=%HERE%..\..\tools\vendor\MinHook
set OUT=%HERE%build
if not exist "%OUT%" mkdir "%OUT%"
pushd "%OUT%"

cl /nologo /W3 /O2 /MT /LD ^
   /I"%MH%\include" /I"%MH%\src" ^
   "%HERE%main.c" "%HERE%natives.c" "%HERE%names.c" "%HERE%speech.c" "%HERE%focus.c" "%HERE%dialog.c" "%HERE%help.c" "%HERE%shot.c" "%HERE%combat.c" "%HERE%history.c" "%HERE%soldier.c" "%HERE%info.c" "%HERE%sight.c" "%HERE%mission.c" "%HERE%abar.c" "%HERE%hq.c" "%HERE%cursor.c" "%HERE%nav.c" "%HERE%tile.c" "%HERE%sonar.c" "%HERE%audio.c" "%HERE%learn.c" ^
   "%HERE%props.c" "%HERE%input.c" "%HERE%scan.c" "%HERE%objects.c" "%HERE%settings.c" ^
   "%MH%\src\hook.c" "%MH%\src\buffer.c" "%MH%\src\trampoline.c" "%MH%\src\hde\hde32.c" ^
   /Fe:xcom_uihook.dll ^
   /link /OUT:xcom_uihook.dll /MAP:xcom_uihook.map ole32.lib oleaut32.lib sapi.lib user32.lib winmm.lib
if errorlevel 1 (popd & echo DLL BUILD FAILED & exit /b 1)

cl /nologo /W3 /O2 /MT "%HERE%inject.c" "%HERE%injector.c" /Fe:inject.exe /link user32.lib
if errorlevel 1 (popd & echo INJECTOR BUILD FAILED & exit /b 1)

rem The launcher is a GUI app so that no console window steals focus from the
rem game, and its dialog comes from a compiled resource rather than from code.
rem NB: %HERE% ends in a backslash, which would escape the closing quote.
rc /nologo /fo launcher.res /i "%HERE:~0,-1%" "%HERE%launcher.rc"
if errorlevel 1 (popd & echo RESOURCE BUILD FAILED & exit /b 1)

cl /nologo /W3 /O2 /MT "%HERE%launcher.c" "%HERE%gamepaths.c" "%HERE%injector.c" ^
   launcher.res /Fe:launcher.exe ^
   /link /SUBSYSTEM:WINDOWS user32.lib advapi32.lib shlwapi.lib
if errorlevel 1 (popd & echo LAUNCHER BUILD FAILED & exit /b 1)

cl /nologo /W3 /O2 /MT "%HERE%test_natives.c" "%HERE%natives.c" /Fe:test_natives.exe
if errorlevel 1 (popd & echo TEST BUILD FAILED & exit /b 1)

cl /nologo /W3 /O2 /MT "%HERE%test_speech.c" "%HERE%speech.c" /Fe:test_speech.exe /link ole32.lib oleaut32.lib sapi.lib
if errorlevel 1 (popd & echo SPEECH TEST BUILD FAILED & exit /b 1)

cl /nologo /W3 /O2 /MT "%HERE%test_focus.c" "%HERE%nav.c" "%HERE%tile.c" "%HERE%scan.c" "%HERE%focus.c" "%HERE%dialog.c" "%HERE%help.c" "%HERE%shot.c" "%HERE%combat.c" "%HERE%history.c" "%HERE%soldier.c" "%HERE%info.c" "%HERE%sight.c" "%HERE%mission.c" "%HERE%abar.c" "%HERE%hq.c" "%HERE%speech.c" "%HERE%input.c" "%HERE%settings.c" /Fe:test_focus.exe /link ole32.lib oleaut32.lib sapi.lib
if errorlevel 1 (popd & echo FOCUS TEST BUILD FAILED & exit /b 1)

cl /nologo /W3 /O2 /MT "%HERE%test_sonar.c" "%HERE%sonar.c" "%HERE%audio.c" "%HERE%learn.c" "%HERE%settings.c" "%HERE%speech.c" /Fe:test_sonar.exe ^
   /link winmm.lib user32.lib ole32.lib oleaut32.lib sapi.lib
if errorlevel 1 (popd & echo SONAR TEST BUILD FAILED & exit /b 1)

cl /nologo /W3 /O2 /MT "%HERE%test_paths.c" "%HERE%gamepaths.c" /Fe:test_paths.exe ^
   /link advapi32.lib shlwapi.lib
if errorlevel 1 (popd & echo PATHS TEST BUILD FAILED & exit /b 1)

popd
echo.
echo Built: %OUT%\launcher.exe   (start here)
echo        %OUT%\xcom_uihook.dll
echo        %OUT%\inject.exe
