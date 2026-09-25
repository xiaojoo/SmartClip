; Inno Setup script for the "min" tier (build\pkg-min): the rec tier minus the
; software-rendering fallback (opengl32sw.dll + D3Dcompiler_47.dll). Smallest, but a
; machine with a broken GL/driver stack gets nothing to fall back on - which is why
; "rec" is the one to hand out. The file name still says api-only from when this was
; measured against a GPU-less assumption. ASCII on purpose: ISPP reads .iss as ANSI
; unless there is a BOM, and this box has a GBK default codepage.
;
; "api-only" is the min tier (no software-rendering fallback). Version comes
; from CMakeLists.txt through build-release.bat (ISCC /DAPPVER=x.y.z); the
; default keeps a manual "ISCC setup-api-only.iss" run working.
#ifndef APPVER
#define APPVER "0.1.0"
#endif

; Where the trimmed trees live and where setup.exe goes. build-release.bat exports
; SC_BUILD; the fallback keeps a bare "ISCC setup-rec.iss" run working in this repo.
#define SCBUILD GetEnv("SC_BUILD")
#if SCBUILD == ""
#define SCBUILD "H:\steward\build"
#endif

; Uninstaller signing. There is no [SignTools] script section in Inno: the tool has to
; be registered when the compiler is invoked - build-release.bat passes
;     -s"scsign=<the sign command> $f"
; Without that, SignTool=scsign is rejected with "Value of [Setup] section directive
; SignTool is invalid" (measured: the same script compiles the moment -s is given).
; So this file only NAMES the tool; the command, certificate path and password
; included, never lives in a tracked file - ISCC inherits the environment from
; build-release.bat, which keeps it in SMARTCLIP_SIGN_CMD.
; There is no "sign if a tool happens to be configured" mode. Inno re-checks the
; result itself: a tool that exits 0 without really signing aborts the compile with
; "the file does not have a digital signature" (measured with a do-nothing wrapper).
#define SIGNCMD GetEnv("SMARTCLIP_SIGN_CMD")

[Setup]
AppName=SmartClip
AppVersion={#APPVER}
AppPublisher=xiaojoo
DefaultDirName={autopf}\SmartClip
DisableProgramGroupPage=yes
DisableWelcomePage=no
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
Compression=lzma2/ultra64
SolidCompression=yes
OutputDir={#SCBUILD}\dist
OutputBaseFilename=smartclip-min-{#APPVER}
UninstallDisplayIcon={app}\SmartClip.exe
#if SIGNCMD != ""
SignedUninstaller=yes
SignTool=scsign
#endif

[Languages]
Name: "en"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; Flags: unchecked

[Files]
Source: "{#SCBUILD}\pkg-min\*"; DestDir: "{app}"; Flags: recursesubdirs createallsubdirs ignoreversion

[Icons]
Name: "{group}\SmartClip"; Filename: "{app}\SmartClip.exe"
Name: "{group}\Uninstall SmartClip"; Filename: "{uninstallexe}"
Name: "{autodesktop}\SmartClip"; Filename: "{app}\SmartClip.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\SmartClip.exe"; Description: "Launch SmartClip"; Flags: nowait postinstall skipifsilent
