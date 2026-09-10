; SPDX-License-Identifier: Apache-2.0
#ifndef SdkVersion
  #error SdkVersion is required
#endif
#ifndef ManagerBuild
  #error ManagerBuild is required
#endif
#ifndef PayloadDir
  #error PayloadDir is required
#endif
#ifndef ReleaseDir
  #error ReleaseDir is required
#endif
[Setup]
AppId=MicroPixel.SDK
AppName=MicroPixel SDK Preview
AppVersion={#SdkVersion}
AppPublisher=MicroPixel
AppPublisherURL=https://micropixel.ai
DefaultDirName={localappdata}\MicroPixel
DisableDirPage=yes
UsePreviousAppDir=no
PrivilegesRequired=lowest
ArchitecturesAllowed=x64os
ArchitecturesInstallIn64BitMode=x64os
MinVersion=10.0.19045
WizardStyle=modern
ChangesEnvironment=yes
CloseApplications=no
RestartApplications=no
AlwaysRestart=no
RestartIfNeededByRun=no
Compression=lzma2/normal
SolidCompression=yes
OutputDir={#ReleaseDir}
OutputBaseFilename=micropixel-setup-{#SdkVersion}-windows-x64-preview
UninstallDisplayName=MicroPixel SDK Preview
LicenseFile=..\..\LICENSE

[Files]
Source: "{#PayloadDir}\{#ManagerBuild}\*"; DestDir: "{app}\versions\{#ManagerBuild}"; Flags: recursesubdirs createallsubdirs ignoreversion onlyifdoesntexist
Source: "{#PayloadDir}\micropixel.exe"; DestDir: "{app}\bin"; Flags: ignoreversion
Source: "..\..\docs\development\windows-sdk.zh-CN.md"; DestDir: "{app}\docs"; Flags: ignoreversion
Source: "..\..\guest\sdk\AI.md"; DestDir: "{app}\docs"; Flags: ignoreversion

[Icons]
Name: "{userprograms}\MicroPixel SDK\Developer command prompt"; Filename: "{cmd}"; Parameters: "/K """"{app}\bin\micropixel.exe"" doctor"""; WorkingDir: "{userdocs}"
Name: "{userprograms}\MicroPixel SDK\Windows guide"; Filename: "https://github.com/78/micropixel/blob/sdk-v{#SdkVersion}/docs/development/windows-sdk.zh-CN.md"

[Registry]
Root: HKCU; Subkey: "Software\MicroPixel"; Flags: uninsdeletekeyifempty

[Code]
var
  Prepared: Boolean;
  RetryButton: TNewButton;
  PurgeCache: Boolean;

function NormalPath(Value: String): String;
begin
  Result := Lowercase(Trim(Value));
  if (Length(Result) > 1) and (Result[1] = '"') and (Result[Length(Result)] = '"') then
    Result := Copy(Result, 2, Length(Result) - 2);
  StringChangeEx(Result, '/', '\', True);
  StringChangeEx(Result, '%localappdata%', Lowercase(ExpandConstant('{localappdata}')), True);
  while (Length(Result) > 0) and (Result[Length(Result)] = '\') do
    Delete(Result, Length(Result), 1);
end;

function FilterPath(Value: String; var Found: Boolean): String;
var Part: String; Position: Integer; Last, First: Boolean;
begin
  Result := ''; Found := False; First := True;
  repeat
    Position := Pos(';', Value);
    Last := Position = 0;
    if Last then begin Part := Value; Value := ''; end
    else begin Part := Copy(Value, 1, Position - 1); Delete(Value, 1, Position); end;
    if NormalPath(Part) = NormalPath(ExpandConstant('{app}\bin')) then Found := True
    else begin
      if not First then Result := Result + ';';
      Result := Result + Part;
      First := False;
    end;
  until Last;
end;

procedure AddUserPath;
var Value, Filtered: String; Found: Boolean;
begin
  RegQueryStringValue(HKCU, 'Environment', 'Path', Value);
  Filtered := FilterPath(Value, Found);
  if not Found then begin
    if Value <> '' then Value := Value + ';';
    Value := Value + ExpandConstant('{app}\bin');
    if not RegWriteExpandStringValue(HKCU, 'Environment', 'Path', Value) then
      RaiseException('Could not update the current user PATH.');
    RegWriteStringValue(HKCU, 'Software\MicroPixel', 'OwnsPathEntry', '1');
  end;
end;

procedure RemoveUserPath;
var Value, Owner: String; Found: Boolean;
begin
  if RegQueryStringValue(HKCU, 'Software\MicroPixel', 'OwnsPathEntry', Owner) and (Owner = '1') then begin
    RegQueryStringValue(HKCU, 'Environment', 'Path', Value);
    Value := FilterPath(Value, Found);
    if Found then RegWriteExpandStringValue(HKCU, 'Environment', 'Path', Value);
    RegDeleteValue(HKCU, 'Software\MicroPixel', 'OwnsPathEntry');
  end;
end;

function RunManager(Arguments: String; Show: Integer): Boolean;
var Code: Integer;
begin
  Result := Exec(ExpandConstant('{app}\bin\micropixel.exe'), Arguments,
    ExpandConstant('{app}'), Show, ewWaitUntilTerminated, Code) and (Code = 0);
end;

procedure PrepareEnvironment;
begin
  WizardForm.StatusLabel.Caption := 'Preparing SDK and toolchains. Download progress is shown in the console.';
  Prepared := RunManager('setup --version {#SdkVersion} --yes --json', SW_SHOW);
  if Prepared then Prepared := RunManager('doctor --json', SW_SHOW);
  if Prepared then WizardForm.FinishedLabel.Caption := 'MicroPixel development environment is ready. Open a new terminal and run micropixel doctor --json.'
  else WizardForm.FinishedLabel.Caption := 'Environment is not ready. Check the connection and retry, or run micropixel setup --yes --json followed by micropixel doctor --json. Installer success alone does not mean the toolchain is ready.';
  RetryButton.Visible := not Prepared;
end;

procedure RetryPreparation(Sender: TObject);
begin
  RetryButton.Enabled := False;
  try PrepareEnvironment; finally RetryButton.Enabled := True; end;
end;

procedure InitializeWizard;
begin
  Prepared := False;
  WizardForm.FinishedLabel.Height := ScaleY(130);
  RetryButton := TNewButton.Create(WizardForm);
  RetryButton.Parent := WizardForm.FinishedPage;
  RetryButton.Caption := 'Retry tool preparation';
  RetryButton.Width := ScaleX(180);
  RetryButton.Left := WizardForm.FinishedLabel.Left;
  RetryButton.Top := WizardForm.FinishedLabel.Top + WizardForm.FinishedLabel.Height + ScaleY(16);
  RetryButton.OnClick := @RetryPreparation;
  RetryButton.Visible := False;
end;

procedure CurStepChanged(CurStep: TSetupStep);
var Code: Integer;
begin
  if CurStep = ssPostInstall then begin
    AddUserPath;
    if not Exec(ExpandConstant('{app}\versions\{#ManagerBuild}\python\python.exe'),
      '-I -X utf8 "' + ExpandConstant('{app}\versions\{#ManagerBuild}\launch.py') + '" --activate-manager "' + ExpandConstant('{app}') + '"',
      ExpandConstant('{app}'), SW_HIDE, ewWaitUntilTerminated, Code) or (Code <> 0) then
      RaiseException('Could not activate the manager. Existing cached SDKs were preserved.');
    if not WizardSilent then PrepareEnvironment;
  end;
end;

procedure CurPageChanged(CurPageID: Integer);
begin
  if (CurPageID = wpFinished) and not WizardSilent then begin
    if Prepared then WizardForm.FinishedLabel.Caption := 'MicroPixel development environment is ready. Open a new terminal and run micropixel doctor --json.'
    else WizardForm.FinishedLabel.Caption := 'Environment is not ready. Retry tool preparation, or run micropixel setup --yes --json and micropixel doctor --json. Installer success alone does not mean the toolchain is ready.';
    RetryButton.Visible := not Prepared;
  end;
end;

function InitializeUninstall: Boolean;
var Index: Integer;
begin
  PurgeCache := False;
  for Index := 1 to ParamCount do
    if CompareText(ParamStr(Index), '/PURGECACHE') = 0 then PurgeCache := True;
  if not UninstallSilent then
    PurgeCache := MsgBox('Also remove downloaded SDK/toolchain caches? Your game projects are preserved.', mbConfirmation, MB_YESNO or MB_DEFBUTTON2) = IDYES;
  Result := True;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usPostUninstall then begin
    RemoveUserPath;
    DeleteFile(ExpandConstant('{app}\current.json'));
    if PurgeCache then begin
      DelTree(ExpandConstant('{app}\downloads'), True, True, True);
      DelTree(ExpandConstant('{app}\packages'), True, True, True);
      DelTree(ExpandConstant('{app}\environments'), True, True, True);
      DelTree(ExpandConstant('{app}\manifests'), True, True, True);
      DelTree(ExpandConstant('{app}\notices'), True, True, True);
      DeleteFile(ExpandConstant('{app}\default.json'));
      DeleteFile(ExpandConstant('{app}\index-cache.json'));
    end;
  end;
end;
