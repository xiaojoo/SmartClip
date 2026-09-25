@echo off
setlocal enabledelayedexpansion
REM =====================================================================
REM  One-shot release for SmartClip:
REM    build Release -> windeployqt -> trim 3 tiers -> portable zips
REM    -> Inno Setup installers -> measured size report
REM
REM  build-release.bat            full chain
REM  build-release.bat trimonly   reuse build\pkg (skip compile + windeployqt)
REM
REM  Exit codes: 0 ok / 1 compile or missing exe / 2 windeployqt failed
REM              3 ISCC.exe not found (zips were still produced)
REM              4 package tree incomplete / 5 packaged exe failed its self-test
REM              6 no app-local MSVC runtime in the package
REM              7 a code-signing step failed (see SMARTCLIP_SIGN_CMD below)
REM              8 an .iss script failed to compile
REM
REM  ASCII only on purpose: cmd reads .bat in the OEM codepage (936 here),
REM  and a Chinese comment can silently break a line. Same lesson as the
REM  PowerShell probe scripts.
REM =====================================================================

rem Derived from where this script sits, so the repo can be cloned to another drive
rem without editing paths. pushd/popd turns "...\packaging\.." into a plain root.
pushd "%~dp0.."
set "ROOT=%CD%"
popd
set "BUILD=%ROOT%\build"
set "REL=%BUILD%\Release"
set "PKG=%BUILD%\pkg"
set "DIST=%BUILD%\dist"
if not defined QTDIR set "QTDIR=D:\Program\Qt\6.11.2\msvc2022_64"
if not defined JOMDIR set "JOMDIR=D:\Program\Qt\Tools\QtCreator\bin\jom"
if not defined CMK set "CMK=D:\Program\CMake\bin\cmake.exe"
if not defined VCCall set "VCCall=D:\Program\VisualStudio\VC\Auxiliary\Build\vcvars64.bat"
if not defined SEVENZ set "SEVENZ=D:\Program\7-Zip\7z.exe"

set "SKIPBUILD=0"
if /i "%~1"=="trimonly" set "SKIPBUILD=1"

rem ---- optional code signing -------------------------------------------------
rem Set SMARTCLIP_SIGN_CMD to a command that signs ONE file, whose full path this
rem script appends as the last argument. Nothing secret is committed here - put it
rem in your own shell before running, e.g. from a PFX:
rem    set "SMARTCLIP_SIGN_CMD=signtool sign /fd sha256 /td sha256 /tr http://timestamp.digicert.com /f H:\keys\smartclip.pfx /p secret"
rem or with a certificate that already sits in your Windows certificate store
rem (USB token, Azure Key Vault, ...):
rem    set "SMARTCLIP_SIGN_CMD=signtool sign /fd sha256 /td sha256 /tr http://timestamp.digicert.com /sha1 3F2C1A..."
rem With no SMARTCLIP_SIGN_CMD set the whole thing is a no-op and you get the same
rem unsigned build as before. A sign that FAILS aborts the chain (exit 7) rather
rem than shipping something you believed was signed.
rem Not covered: the uninstaller Inno generates inside setup.exe - that needs
rem SignedUninstaller=yes plus a [SignTools] entry in the .iss.
set "SIGNFAIL=0"
set "SIGTOOL="
if exist "C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\signtool.exe" set "SIGTOOL=C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\signtool.exe"
if not defined SIGTOOL if exist "C:\Program Files (x86)\Windows Kits\10\bin\x64\signtool.exe" set "SIGTOOL=C:\Program Files (x86)\Windows Kits\10\bin\x64\signtool.exe"
if not defined SIGTOOL where signtool >nul 2>nul && set "SIGTOOL=signtool"
rem Inno has no [SignTools] script section: the tool has to be registered on the
rem compiler command line, and the .iss only names it ("SignTool=scsign").
rem Two characters the sign command must not contain: ';' (Inno reads it as the start
rem of a comment in the .iss) and '!' (this script runs with delayed expansion, so a
rem '!' in a password would be eaten here).
set "SIGNARG="
if defined SMARTCLIP_SIGN_CMD set "SIGNARG=-s"scsign=%SMARTCLIP_SIGN_CMD% $f""

rem ---- version straight out of CMakeLists.txt (single source) ----
set "VER=0.0.0"
for /f "tokens=3" %%v in ('findstr /b /c:"project(SmartClip VERSION" "%ROOT%\CMakeLists.txt"') do set "VER=%%v"
echo SmartClip %VER%  --  release chain starting

rem ================= 1. compile Release =================
rem vcvars64 is called even in trimonly mode: the app-local CRT copy below
rem needs %VCToolsRedistDir%, which only that script sets.
call "%VCCall%" >nul
set "PATH=%QTDIR%\bin;%JOMDIR%;%PATH%"
if "%SKIPBUILD%"=="1" goto :deploy
if not exist "%REL%" mkdir "%REL%"
echo [1/7] cmake configure (Release) ...
"%CMK%" -S "%ROOT%" -B "%REL%" -G "NMake Makefiles JOM" -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="%QTDIR%" >"%BUILD%\release-conf.log" 2>&1
if errorlevel 1 goto :fail_conf
echo [1/7] jom build SmartClip ...
"%CMK%" --build "%REL%" --target SmartClip >"%BUILD%\release-build.log" 2>&1
if errorlevel 1 goto :fail_build
if not exist "%REL%\SmartClip.exe" goto :fail_noexe

rem ================= 2. windeployqt =================
:deploy
echo [2/7] windeployqt ...
if exist "%PKG%" rd /s /q "%PKG%"
mkdir "%PKG%" 2>nul
rem --no-quick-import was tried once and produced a package with NO qml\ tree
rem at all (the app is Quick Controls - it would not even start). Never add it.
rem --no-compiler-runtime: windeployqt otherwise drops a 24 MB vc_redist.x64.exe
rem into the package; we ship the three CRT DLLs app-local instead (1.5 MB).
rem --no-system-dxc-compiler: matches the tree that was measured working on
rem 2026-09-23 (dxcompiler.dll + dxil.dll are another ~15 MB; Qt still has
rem D3Dcompiler_47.dll as the shader-compiler fallback).
"%QTDIR%\bin\windeployqt.exe" --release --no-translations --no-compiler-runtime ^
  --no-system-dxc-compiler --qmldir "%ROOT%\qml" --dir "%PKG%" "%REL%\SmartClip.exe" ^
  >"%BUILD%\windeploy.log" 2>&1
if errorlevel 1 goto :fail_deploy
copy /y "%REL%\SmartClip.exe" "%PKG%\" >nul
if not exist "%PKG%\SmartClip.exe" goto :fail_deploy
rem app-local MSVC runtime so a clean machine runs the green zip too
if defined VCToolsRedistDir (
  copy /y "%VCToolsRedistDir%x64\Microsoft.VC143.CRT\vcruntime140.dll"   "%PKG%\" >nul
  copy /y "%VCToolsRedistDir%x64\Microsoft.VC143.CRT\vcruntime140_1.dll" "%PKG%\" >nul
  copy /y "%VCToolsRedistDir%x64\Microsoft.VC143.CRT\msvcp140.dll"       "%PKG%\" >nul
) else (
  echo [-] VCToolsRedistDir is not set - the package will need VC_redist installed
)

rem sign the app binary BEFORE the tiers are copied out of pkg, so the exe inside
rem every zip and every setup.exe carries the signature (green build too)
call :sign "%PKG%\SmartClip.exe"
rem fail fast: every tier, zip and setup is built out of this exe, so a signing
rem failure here makes the rest of the run pointless (and it was burning six
rem minutes of compression before stopping at the end)
if "%SIGNFAIL%"=="1" goto :fail_sign

rem ================= 3. trim the three tiers =================
echo [3/7] trimming pkg-full / pkg-rec / pkg-min ...
if exist "%BUILD%\pkg-full" rd /s /q "%BUILD%\pkg-full"
if exist "%BUILD%\pkg-rec"  rd /s /q "%BUILD%\pkg-rec"
if exist "%BUILD%\pkg-min"  rd /s /q "%BUILD%\pkg-min"
rem /XD .git stops robocopy recursing into the repo working tree if a junction
rem ever shows up under pkg; /XF drops the QML type descriptions (debug-time only)
robocopy "%PKG%" "%BUILD%\pkg-full" /e /xf *.qmltypes /xd .git /njh /njs /ndl /nc /ns >nul
robocopy "%PKG%" "%BUILD%\pkg-rec"  /e /xf *.qmltypes /xd .git /njh /njs /ndl /nc /ns >nul
call :trim_rec
rem min is derived FROM rec (it used to be derived from the full tree, which
rem silently left every rec-tier removal in place - min ended up bigger than rec)
robocopy "%BUILD%\pkg-rec" "%BUILD%\pkg-min" /e /xd .git /njh /njs /ndl /nc /ns >nul
call :trim_min

rem ================= 4. is this tree actually bootable =================
echo [4/7] package self-check ...
set "MISSING=0"
for %%f in (SmartClip.exe Qt6Core.dll Qt6Gui.dll Qt6Qml.dll Qt6Quick.dll ^
            Qt6QuickControls2.dll Qt6QuickControls2Fusion.dll ^
            platforms\qwindows.dll sqldrivers\qsqlite.dll) do call :need pkg-rec "%%f"
for %%d in (qml\QtQml qml\QtQuick qml\QtQuick\Controls qml\QtQuick\Layouts ^
            qml\QtQuick\Window qml\QtQuick\Templates) do call :needdir pkg-rec "%%d"
if "%MISSING%"=="1" goto :fail_pkg
if not exist "%BUILD%\pkg-rec\vcruntime140.dll" goto :fail_crt

rem the real gate: run the self-test from INSIDE the package with Qt removed from
rem PATH. If it boots, every DLL/plugin/qml module came from the package itself.
rem PATH is restored right after - the size report below still needs powershell.
set "PATH_SAVED=%PATH%"
set "PATH=%SystemRoot%\system32;%SystemRoot%"
pushd "%BUILD%\pkg-rec"
SmartClip.exe --summarize-test >"%BUILD%\pkg-smoke.log" 2>&1
set "SMOKE=%ERRORLEVEL%"
popd
set "PATH=%PATH_SAVED%"
echo     packaged tree self-test exit=%SMOKE% (log: pkg-smoke.log)
if not "%SMOKE%"=="0" goto :fail_smoke

rem ================= 5. portable zips =================
echo [5/7] portable zips ...
if not exist "%DIST%" mkdir "%DIST%"
if not exist "%SEVENZ%" set "SEVENZ=7z"
call :zip pkg-full full
call :zip pkg-rec  rec
call :zip pkg-min  min

rem ================= 6. installers =================
echo [6/7] Inno Setup ...
set "ISCC="
if not defined ISCC if exist "D:\Program\Inno Setup 7\ISCC.exe"      set "ISCC=D:\Program\Inno Setup 7\ISCC.exe"
if not defined ISCC if exist "D:\Program\Inno Setup 6\ISCC.exe"      set "ISCC=D:\Program\Inno Setup 6\ISCC.exe"
if not defined ISCC if exist "C:\Program Files\Inno Setup 7\ISCC.exe"  set "ISCC=C:\Program Files\Inno Setup 7\ISCC.exe"
if not defined ISCC if exist "C:\Program Files\Inno Setup 6\ISCC.exe"  set "ISCC=C:\Program Files\Inno Setup 6\ISCC.exe"
if not defined ISCC if exist "C:\Program Files (x86)\Inno Setup 7\ISCC.exe" set "ISCC=C:\Program Files (x86)\Inno Setup 7\ISCC.exe"
if not defined ISCC if exist "C:\Program Files (x86)\Inno Setup 6\ISCC.exe" set "ISCC=C:\Program Files (x86)\Inno Setup 6\ISCC.exe"
if not defined ISCC (
    echo [!] ISCC.exe not found: installers skipped, zips are still in dist\
    echo     download: https://github.com/jrsoftware/issrc/releases
    echo     or:       winget install --id JRSoftware.InnoSetup -e
    set "RC=3"
    goto :report
)
echo     using !ISCC!
rem The .iss files live next to this script (they are tracked in packaging/), and
rem they read two things from the environment instead of having them written in:
rem SC_BUILD = where pkg-* / dist are, SMARTCLIP_SIGN_CMD = how to sign the
rem uninstaller Inno generates. Neither a path nor a cert belongs in a tracked file.
set "SC_BUILD=%BUILD%"
set "ISCCFAIL=0"
"!ISCC!" /DAPPVER=%VER% !SIGNARG! "%~dp0setup-api-only.iss" >"%BUILD%\iscc-api-only.log" 2>&1
if errorlevel 1 set "ISCCFAIL=1"
"!ISCC!" /DAPPVER=%VER% !SIGNARG! "%~dp0setup-rec.iss" >"%BUILD%\iscc-rec.log" 2>&1
if errorlevel 1 set "ISCCFAIL=1"
"!ISCC!" /DAPPVER=%VER% !SIGNARG! "%~dp0setup-full.iss" >"%BUILD%\iscc-full.log" 2>&1
if errorlevel 1 set "ISCCFAIL=1"
if "%ISCCFAIL%"=="1" (
    echo [x] at least one .iss failed - see iscc-api-only.log / iscc-rec.log / iscc-full.log
    goto :fail_iscc
)

if defined ISCC for %%T in (full rec min) do call :sign "%DIST%\smartclip-%%T-%VER%.exe"
if "%SIGNFAIL%"=="1" goto :fail_sign

rem ================= 7. report =================
:report
echo [7/7] measured sizes:
echo   tier  files  tree_MB  zip_MB  installer_MB
for %%T in (full rec min) do call :row %%T
echo.
echo   installers go to  %DIST%
echo   green (no-install) builds are the same folders, zipped
if not defined RC set "RC=0"
endlocal & exit /b %RC%

rem ---------------------------------------------------------------------
:need
rem %1 = package folder, %2 = file that must exist inside it
if not exist "%BUILD%\%~1\%~2" (
    echo [x] missing file in %~1: %~2
    set "MISSING=1"
)
exit /b 0

:needdir
if not exist "%BUILD%\%~1\%~2\" (
    echo [x] missing directory in %~1: %~2
    set "MISSING=1"
)
exit /b 0

:trim_rec
rem recipe measured off the 2026-09-23 trees (full -> rec): the app locks the
rem Fusion style (src/main.cpp setStyle), so Basic + Fusion stay, the rest go.
rd /s /q "%BUILD%\pkg-rec\qmltooling" 2>nul
rd /s /q "%BUILD%\pkg-min\qmltooling" 2>nul
rd /s /q "%BUILD%\pkg-rec\qml\QtQuick\Dialogs" 2>nul
rd /s /q "%BUILD%\pkg-rec\qml\QtQuick\NativeStyle" 2>nul
for %%d in (
  Qt6QuickControls2Imagine Qt6QuickControls2ImagineStyleImpl
  Qt6QuickControls2Material Qt6QuickControls2MaterialStyleImpl
  Qt6QuickControls2Universal Qt6QuickControls2UniversalStyleImpl
  Qt6QuickControls2WindowsStyleImpl Qt6QuickControls2FluentWinUI3StyleImpl
) do del /q "%BUILD%\pkg-rec\%%d.dll" 2>nul
for %%d in (qsqlibase qsqlmimer qsqloci qsqlodbc qsqlpsql) do del /q "%BUILD%\pkg-rec\sqldrivers\%%d.dll" 2>nul
exit /b 0

:trim_min
rem rec -> min: drop the software-rendering fallback (opengl32sw 19.7M +
rem D3Dcompiler_47 4.0M). Only safe when the target always has a real GPU
rem driver; that is why "rec" is the recommended tier, not "min".
del /q "%BUILD%\pkg-min\opengl32sw.dll" "%BUILD%\pkg-min\D3Dcompiler_47.dll" 2>nul
exit /b 0

:zip
rem %1 = source folder name, %2 = short tier name
set "_SRC=%BUILD%\%~1"
set "_OUT=%DIST%\smartclip-%VER%-%~2-portable.zip"
if exist "%_OUT%" del /q "%_OUT%"
pushd "%_SRC%"
"!SEVENZ!" a -tzip -mx=9 -bso0 -bsp0 "%_OUT%" * >nul 2>&1
popd
if not exist "%_OUT%" echo [x] zip failed for %~1
exit /b 0

:row
set "_T=%~1"
set "_N=0"
set "_B=0"
for /f "usebackq tokens=1,2" %%a in (`powershell -NoProfile -Command "$f=Get-ChildItem -Recurse -File '%BUILD%\pkg-%_T%'; '{0} {1}' -f $f.Count,[math]::Round((($f|Measure-Object Length -Sum).Sum/1MB),1)"`) do (
    set "_N=%%a"
    set "_B=%%b"
)
set "_Z=-"
for /f "usebackq tokens=*" %%a in (`powershell -NoProfile -Command "$p='%DIST%\smartclip-%VER%-%_T%-portable.zip'; if(Test-Path $p){[math]::Round(((Get-Item $p).Length/1MB),1)}"`) do set "_Z=%%a"
set "_I=-"
for /f "usebackq tokens=*" %%a in (`powershell -NoProfile -Command "$p=Get-ChildItem '%DIST%\smartclip-%_T%-%VER%.exe' -ErrorAction SilentlyContinue; if($p){[math]::Round(($p.Length/1MB),1)}"`) do set "_I=%%a"
echo   !_T!   !_N!   !_B!   !_Z!   !_I!
exit /b 0

:fail_conf
echo [x] cmake configure failed - see release-conf.log
endlocal & exit /b 1
:fail_build
echo [x] build failed - see release-build.log
endlocal & exit /b 1
:fail_noexe
echo [x] no Release\SmartClip.exe after the build
endlocal & exit /b 1
:fail_deploy
echo [x] windeployqt failed - see windeploy.log
endlocal & exit /b 2
:fail_pkg
echo [x] the package tree is incomplete - nothing was zipped, nothing was built
endlocal & exit /b 4
:fail_crt
echo [x] no app-local vcruntime140/msvcp140 in the package.
echo     vcvars64 did not set VCToolsRedistDir - run this from a shell where
echo     "%VCCall%" works, or the green zip will need VC_redist on the target.
endlocal & exit /b 6
:fail_smoke
echo [x] the packaged exe did not pass its own self-test - see pkg-smoke.log
endlocal & exit /b 5

:sign
rem %~1 = one file that must end up signed. No-op when SMARTCLIP_SIGN_CMD is unset.
if not defined SMARTCLIP_SIGN_CMD goto :sign_skip
%SMARTCLIP_SIGN_CMD% "%~1"
if errorlevel 1 goto :sign_bad
if not defined SIGTOOL (
    echo     signed, not verified - no signtool found: %~nx1
    exit /b 0
)
"!SIGTOOL!" verify /pa "%~1" >nul 2>&1
if errorlevel 1 goto :sign_badverify
echo     signed and verified: %~nx1
exit /b 0

:sign_skip
if defined SIGNSHOWN exit /b 0
set "SIGNSHOWN=1"
echo     [skip sign] set SMARTCLIP_SIGN_CMD to sign the app binary and the setups
exit /b 0

:sign_bad
echo [x] signing command failed for %~nx1
set "SIGNFAIL=1"
exit /b 1

:sign_badverify
echo [x] %~nx1 was signed but signtool verify /pa rejects it
set "SIGNFAIL=1"
exit /b 1

:fail_iscc
echo [x] Inno Setup failed to compile one of the three scripts - nothing is shipped
endlocal & exit /b 8
:fail_sign
echo [x] a signing step failed - do not ship these artifacts. Fix the certificate
echo     setup (SMARTCLIP_SIGN_CMD) and run again.
endlocal & exit /b 7
