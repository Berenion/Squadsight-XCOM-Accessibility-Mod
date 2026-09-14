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
   "%HERE%main.c" "%HERE%natives.c" "%HERE%names.c" "%HERE%speech.c" ^
   "%MH%\src\hook.c" "%MH%\src\buffer.c" "%MH%\src\trampoline.c" "%MH%\src\hde\hde32.c" ^
   /Fe:xcom_uihook.dll ^
   /link /OUT:xcom_uihook.dll ole32.lib oleaut32.lib sapi.lib
if errorlevel 1 (popd & echo DLL BUILD FAILED & exit /b 1)

cl /nologo /W3 /O2 /MT "%HERE%inject.c" /Fe:inject.exe
if errorlevel 1 (popd & echo INJECTOR BUILD FAILED & exit /b 1)

cl /nologo /W3 /O2 /MT "%HERE%test_natives.c" "%HERE%natives.c" /Fe:test_natives.exe
if errorlevel 1 (popd & echo TEST BUILD FAILED & exit /b 1)

cl /nologo /W3 /O2 /MT "%HERE%test_speech.c" "%HERE%speech.c" /Fe:test_speech.exe /link ole32.lib oleaut32.lib sapi.lib
if errorlevel 1 (popd & echo SPEECH TEST BUILD FAILED & exit /b 1)

popd
echo.
echo Built: %OUT%\xcom_uihook.dll
echo        %OUT%\inject.exe
