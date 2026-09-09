; PitchNet Windows installer.

#define MyAppName "PitchNet"
#define MyAppVersion "0.5.6"
#define MyAppPublisher "Session Loops"
#define MyAppURL "https://www.sessionloops.com/"
#define MyAppExeName "PitchNet.exe"

[Setup]
; This script lives in packaging\windows. SourceDir points back at the repository
; root so every relative path below (build\..., Resources\..., Output\...) is
; resolved from the repo root, exactly as when the script sat there.
SourceDir=..\..
OutputDir=Output
AppId={{D6C85D0D-3B96-4B45-B8B5-3E25B18D24F0}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
AppUpdatesURL={#MyAppURL}
DefaultDirName={code:getPreProgramPath}\{#MyAppName}
DefaultGroupName={#MyAppPublisher}
DisableProgramGroupPage=yes
OutputBaseFilename=PitchNet_Installer
ArchitecturesInstallIn64BitMode=x64compatible
SetupIconFile=build\PitchNet_artefacts\JuceLibraryCode\icon.ico
Compression=lzma
SolidCompression=yes
WizardStyle=modern
InfoBeforeFile=THIRD_PARTY_NOTICES.txt

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Types]
Name: "custom"; Description: "Custom installation"; Flags: iscustom

[Components]
Name: "main"; Description: "Standalone application and runtime resources"; Types: custom; Flags: fixed
Name: "vst3"; Description: "VST3"; Types: custom
Name: "aax"; Description: "AAX"; Types: custom

[Files]
Source: "build\PitchNet_artefacts\Release\PitchNet.exe"; DestDir: "{app}"; Components: main; Flags: ignoreversion
Source: "build\PitchNet_artefacts\Release\onnxruntime.dll"; DestDir: "{app}"; Components: main; Flags: ignoreversion
Source: "build\PitchNetPlugin_artefacts\Release\VST3\PitchNet.vst3\*"; DestDir: "{code:getPreVST64Path}"; Components: vst3; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "build\PitchNetPlugin_artefacts\Release\AAX\PitchNet.aaxplugin\*"; DestDir: "{code:getPreAAXPath}"; Components: aax; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "Resources\models\*"; DestDir: "{commonappdata}\{#MyAppPublisher}\{#MyAppName}\models"; Components: main; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "Resources\fonts\Montserrat-Medium.ttf"; DestDir: "{commonfonts}"; FontInstall: "Montserrat-Medium"; Components: main; Flags: onlyifdoesntexist uninsneveruninstall
Source: "Resources\fonts\Montserrat-Regular.ttf"; DestDir: "{commonfonts}"; FontInstall: "Montserrat-Regular"; Components: main; Flags: onlyifdoesntexist uninsneveruninstall
; NOTE: Don't use "Flags: ignoreversion" on any shared system files.

[Icons]
Name: "{group}\{cm:UninstallProgram,{#MyAppName}}"; Filename: "{uninstallexe}"
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; WorkingDir: "{app}"

[Code]
var
  Page64: TInputDirWizardPage;

function getPreProgramPath(Param: String): String;
begin
  Result := 'C:\Program Files\{#MyAppPublisher}';
end;

function getPreVST64Path(Param: String): String;
begin
  Result := 'C:\Program Files\Common Files\VST3\{#MyAppPublisher}\{#MyAppName}.vst3';
end;

function getPreAAXPath(Param: String): String;
begin
  Result := 'C:\Program Files\Common Files\Avid\Audio\Plug-Ins\{#MyAppName}.aaxplugin';
end;

function getVST64Dir(Param: String): String;
begin
  if IsWin64 then
    Result := Page64.Values[0];
end;

procedure InitializeWizard;
begin
  if IsWin64 then
  begin
    Page64 := CreateInputDirPage(wpSelectDir,
      'Select VST3 Plugins Directory',
      'Ignore and click next if you do not have a directory for VST3 plugins',
      'Please choose the path of your VST3 plugins: (Ignore and click next if you do not have a directory for VST3 plugins)',
      False, '');

    Page64.Add('');
    Page64.Values[0] := ExpandConstant(getPreVST64Path(''));
  end;
end;
