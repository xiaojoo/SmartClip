@echo off
REM Embed a manifest into a built binary with the Windows SDK's mt.exe.
REM
REM Why a .bat instead of calling mt.exe straight from CMake: mt wants the target as
REM   -outputresource:"path;#1"
REM and that string has to survive CMake list expansion (the ';'), the NMake makefile
REM (the '#' starts a comment there) and cmd's quoting rules all at once. Measured on
REM this repo: VERBATIM -> mt gets a trailing quote (c101008d), no VERBATIM -> the
REM argument splits (c1010008), unquoted -> the ";#1" is eaten (c1010011).
REM Doing it in a batch file means only cmd sees the string.
REM
REM ASCII only on purpose: cmd reads .bat in the OEM codepage (936 here).
REM
REM Usage: embed-manifest.bat <mt.exe> <manifest> <target.exe>

if "%~1"=="" (
    echo usage: embed-manifest.bat ^<mt.exe^> ^<manifest^> ^<target.exe^> 1>&2
    exit /b 2
)
"%~1" -manifest "%~2" -outputresource:"%~3;#1"
exit /b %ERRORLEVEL%
