@echo off
rem Build the lifted Indeo decoder and its runtime.
rem
rem 32-bit on purpose: cpu.h's model is that a register holds a real host
rem address, so all of it has to live inside a 32-bit address space.
rem
rem   build.bat <IR32.DLL>
rem
setlocal
if "%~1"=="" (
    echo usage: build.bat ^<path to IR32.DLL^>
    exit /b 2
)

rem %~dp0 keeps its trailing backslash, which would escape the closing quote of
rem /I "%HERE%" and silently break every include path. Strip it and put the
rem separator back explicitly everywhere it is used.
set HERE=%~dp0
set HERE=%HERE:~0,-1%
set INDEO=%HERE%\..
set PCRECOMP=G:\recomp\pc\tools
set OUT=%HERE%\build

if not defined VSINSTALLDIR (
    for %%v in (Community Professional Enterprise BuildTools) do (
        if exist "%ProgramFiles%\Microsoft Visual Studio\2022\%%v\VC\Auxiliary\Build\vcvars32.bat" (
            call "%ProgramFiles%\Microsoft Visual Studio\2022\%%v\VC\Auxiliary\Build\vcvars32.bat" >nul
            goto :havevc
        )
    )
    echo could not find vcvars32.bat - run from a VS x86 prompt
    exit /b 1
)
:havevc

if not exist "%OUT%" mkdir "%OUT%"

echo [1/2] lifting every code segment
py "%INDEO%\lift_all.py" "%~1" -o "%OUT%" || exit /b 1

echo [2/2] compiling
cl /nologo /W3 /O2 /EHa ^
   "%OUT%"\ir32_seg*.c ^
   "%HERE%\ne_mem.c" "%HERE%\ne_dispatch16.c" "%HERE%\ne_dispatch32.c" ^
   "%HERE%\ir32_reg16.c" "%HERE%\ir32_reg32.c" "%HERE%\ir32_run.c" ^
   /I "%HERE%" /I "%OUT%" /I "%PCRECOMP%\runtime" ^
   /Fo:"%OUT%\\" /Fe:"%OUT%\ir32_run.exe" || exit /b 1

echo.
echo built %OUT%\ir32_run.exe
echo   ir32_run ^<IR32.DLL^> init     the decoder's own initialisation
echo   ir32_run ^<IR32.DLL^> sweep    every lifted 32-bit entry
echo   ir32_run ^<IR32.DLL^> driver   DriverProc, the 16-bit entry point
endlocal
