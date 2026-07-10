; TransferUDT graphical installer for Inno Setup.
; Build with: iscc installer\TransferUDT.iss

#ifndef Configuration
#define Configuration "Debug"
#endif

#ifndef PackageArchitecture
#define PackageArchitecture "x64"
#endif

#define AppName "TransferUDT"
#define AppVersion "1.0.0"
#define Publisher "TransferUDT"

#if PackageArchitecture == "x86"
#define AgentBin "..\AgentUDTC++_v7.2\bin\Win32\" + Configuration
#define ServerBin "..\ServerUDTC++_v3\bin\Win32\" + Configuration
#define DashboardBin "..\DashboardWeb\bin\" + Configuration + "\net8.0\win-x86\publish"
#define OutputSuffix "-x86"
#define SetupArchitecturesAllowed "x86compatible"
#else
#define AgentBin "..\AgentUDTC++_v7.2\bin\" + Configuration
#define ServerBin "..\ServerUDTC++_v3\bin\x64\" + Configuration
#define DashboardBin "..\DashboardWeb\bin\" + Configuration + "\net8.0\win-x64\publish"
#define OutputSuffix ""
#define SetupArchitecturesAllowed "x64compatible"
#endif

[Setup]
AppId={{2F3A43BC-CC52-4F0F-B5E1-C4E56CE4E9F1}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher={#Publisher}
DefaultDirName={autopf}\TransferUDT
DefaultGroupName=TransferUDT
DisableProgramGroupPage=yes
OutputDir=..\dist
OutputBaseFilename=TransferUDT-Setup-{#Configuration}{#OutputSuffix}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=admin
ArchitecturesAllowed={#SetupArchitecturesAllowed}
#if PackageArchitecture == "x64"
ArchitecturesInstallIn64BitMode=x64compatible
#endif
UninstallDisplayName=TransferUDT

[Languages]
Name: "brazilianportuguese"; MessagesFile: "compiler:Languages\BrazilianPortuguese.isl"

[Types]
Name: "full"; Description: "Agent, Server e Dashboard"
Name: "agent"; Description: "Somente Agent"
Name: "server"; Description: "Somente Server"
Name: "serverdashboard"; Description: "Server e Dashboard"
Name: "custom"; Description: "Personalizada"; Flags: iscustom

[Components]
Name: "agent"; Description: "TransferUDT Agent"; Types: full agent custom; Flags: checkablealone
Name: "server"; Description: "TransferUDT Server"; Types: full server serverdashboard custom; Flags: checkablealone
Name: "dashboard"; Description: "TransferUDT Dashboard HTTPS"; Types: full serverdashboard custom; Flags: checkablealone

[Tasks]
Name: "startservices"; Description: "Iniciar os servicos apos instalar"; Flags: checkedonce
Name: "overwriteconfig"; Description: "Substituir configuracoes existentes em C:\ProgramData\TransferUDT"

[Files]
Source: "{#AgentBin}\AgentUDTC++.exe"; DestDir: "{app}\Agent"; Components: agent; Flags: ignoreversion
Source: "{#AgentBin}\*.dll"; DestDir: "{app}\Agent"; Components: agent; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#ServerBin}\ServerUDTC++.exe"; DestDir: "{app}\Server"; Components: server; Flags: ignoreversion
Source: "{#ServerBin}\*.dll"; DestDir: "{app}\Server"; Components: server; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#DashboardBin}\*"; DestDir: "{app}\Dashboard"; Components: dashboard; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "TransferUDT-Dashboard-Test.pfx"; DestDir: "{tmp}"; Components: dashboard; Flags: dontcopy
Source: "TransferUDT-Agent-Test.key"; DestDir: "{commonappdata}\TransferUDT\Agent\keys"; Components: agent; Flags: ignoreversion
Source: "TransferUDT-Agent-Test.pub"; DestDir: "{tmp}"; Components: dashboard; Flags: dontcopy
Source: "Install-TransferUDT.ps1"; DestDir: "{app}\installer"; Flags: ignoreversion
Source: "Uninstall-TransferUDT.ps1"; DestDir: "{app}\installer"; Flags: ignoreversion

[UninstallRun]
Filename: "powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\installer\Uninstall-TransferUDT.ps1"" -Component All"; Flags: runhidden waituntilterminated; RunOnceId: "TransferUDT.UninstallServices"

[Code]
var
  AgentPage: TInputQueryWizardPage;
  ServerPage: TInputQueryWizardPage;
  DashboardPage: TInputQueryWizardPage;
  GeneratedPsk: String;

function ToConfigPath(Value: String): String;
begin
  Result := Value;
  StringChangeEx(Result, '\', '/', True);
end;

function HasBadConfigChars(Value: String): Boolean;
begin
  Result := (Pos(#10, Value) > 0) or (Pos(#13, Value) > 0);
end;

function IsSafeClientId(Value: String): Boolean;
var
  I: Integer;
  C: Char;
begin
  Result := Length(Value) > 0;
  for I := 1 to Length(Value) do
  begin
    C := Value[I];
    if not (((C >= 'a') and (C <= 'z')) or
            ((C >= 'A') and (C <= 'Z')) or
            ((C >= '0') and (C <= '9')) or
            (C = '-') or (C = '_')) then
    begin
      Result := False;
      Exit;
    end;
  end;
end;

function StrongSecret(Value: String): Boolean;
begin
  Result := (Length(Value) >= 32) and
            (Value <> 'replace-with-a-strong-shared-secret-of-32-plus-chars') and
            not HasBadConfigChars(Value);
end;

function StartsWithHttps(Value: String): Boolean;
begin
  Result := Copy(Value, 1, 8) = 'https://';
end;

function GenerateStrongSecret(): String;
var
  SecretPath: String;
  Params: String;
  Secret: AnsiString;
  ResultCode: Integer;
begin
  Result := 'replace-with-a-strong-shared-secret-of-32-plus-chars';
  SecretPath := ExpandConstant('{tmp}\transferudt-generated-psk.txt');
  DeleteFile(SecretPath);
  Params :=
    '-NoProfile -ExecutionPolicy Bypass -Command "' +
    '$p=''' + SecretPath + '''; ' +
    '$b=New-Object byte[] 32; ' +
    '$rng=[Security.Cryptography.RandomNumberGenerator]::Create(); ' +
    '$rng.GetBytes($b); ' +
    '[IO.File]::WriteAllText($p,[Convert]::ToBase64String($b),[Text.Encoding]::ASCII)"';

  if Exec('powershell.exe', Params, '', SW_HIDE, ewWaitUntilTerminated, ResultCode) then
  begin
    if ResultCode = 0 then
    begin
      if LoadStringFromFile(SecretPath, Secret) then
      begin
        if StrongSecret(Secret) then
          Result := Secret;
      end;
    end;
  end;
  DeleteFile(SecretPath);
end;

function IsAgentSelected(): Boolean;
begin
  Result := WizardIsComponentSelected('agent');
end;

function IsServerSelected(): Boolean;
begin
  Result := WizardIsComponentSelected('server');
end;

function IsDashboardSelected(): Boolean;
begin
  Result := WizardIsComponentSelected('dashboard');
end;

procedure InitializeWizard;
begin
  GeneratedPsk := GenerateStrongSecret();

  AgentPage := CreateInputQueryPage(
    wpSelectComponents,
    'Configuracao do Agent',
    'Informe como este Agent deve se conectar ao Server.',
    'Essas opcoes serao gravadas em C:\ProgramData\TransferUDT\Agent\config.properties.');
  AgentPage.Add('Server targets, exemplo 10.0.0.10:50051:', False);
  AgentPage.Add('Diretorios monitorados, separados por ponto e virgula:', False);
  AgentPage.Add('Client ID:', False);
  AgentPage.Add('PSK do Agent:', True);
  AgentPage.Add('Dashboard HTTPS URL opcional, exemplo https://server:8443:', False);
  AgentPage.Add('Chave privada do Agent para heartbeat opcional:', False);
  AgentPage.Values[0] := '127.0.0.1:50051';
  AgentPage.Values[1] := ExpandConstant('{commonappdata}\TransferUDT\Agent\watch');
  AgentPage.Values[2] := 'agent-default';
  AgentPage.Values[3] := GeneratedPsk;
  AgentPage.Values[4] := '';
  AgentPage.Values[5] := '';

  ServerPage := CreateInputQueryPage(
    AgentPage.ID,
    'Configuracao do Server',
    'Informe como o Server deve escutar conexoes.',
    'Essas opcoes serao gravadas em C:\ProgramData\TransferUDT\Server\config.properties.');
  ServerPage.Add('Bind address:', False);
  ServerPage.Add('Porta:', False);
  ServerPage.Add('IPs permitidos, separados por virgula:', False);
  ServerPage.Add('Client ID permitido:', False);
  ServerPage.Add('PSK do client permitido:', True);
  ServerPage.Values[0] := '0.0.0.0';
  ServerPage.Values[1] := '50051';
  ServerPage.Values[2] := '127.0.0.1';
  ServerPage.Values[3] := 'agent-default';
  ServerPage.Values[4] := GeneratedPsk;

  DashboardPage := CreateInputQueryPage(
    ServerPage.ID,
    'Configuracao do Dashboard HTTPS',
    'Informe o certificado e as credenciais do Dashboard.',
    'O Dashboard sera instalado como servico Windows em HTTPS. Para teste local, mantenha o PFX padrao embutido.');
  DashboardPage.Add('Porta HTTPS:', False);
  DashboardPage.Add('Arquivo PFX HTTPS:', False);
  DashboardPage.Add('Senha do PFX:', True);
  DashboardPage.Add('Senha do operador da UI:', True);
  DashboardPage.Add('Client ID do Agent para heartbeat opcional:', False);
  DashboardPage.Add('Chave publica do Agent para heartbeat opcional:', False);
  ExtractTemporaryFile('TransferUDT-Dashboard-Test.pfx');
  ExtractTemporaryFile('TransferUDT-Agent-Test.pub');
  DashboardPage.Values[0] := '8443';
  DashboardPage.Values[1] := ExpandConstant('{tmp}\TransferUDT-Dashboard-Test.pfx');
  DashboardPage.Values[2] := 'TransferUDT-Test-123!';
  DashboardPage.Values[3] := '';
  DashboardPage.Values[4] := '';
  DashboardPage.Values[5] := '';
end;

function ShouldSkipPage(PageID: Integer): Boolean;
begin
  Result := False;
  if PageID = AgentPage.ID then
    Result := not IsAgentSelected();
  if PageID = ServerPage.ID then
    Result := not IsServerSelected();
  if PageID = DashboardPage.ID then
    Result := not IsDashboardSelected();
end;

function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;

  if CurPageID = wpSelectComponents then
  begin
    if not IsAgentSelected() and not IsServerSelected() and not IsDashboardSelected() then
    begin
      MsgBox('Selecione pelo menos Agent, Server ou Dashboard.', mbError, MB_OK);
      Result := False;
      Exit;
    end;
    if IsAgentSelected() and IsDashboardSelected() then
    begin
      if AgentPage.Values[4] = '' then
        AgentPage.Values[4] := 'https://localhost:' + DashboardPage.Values[0];
      if AgentPage.Values[5] = '' then
        AgentPage.Values[5] := ExpandConstant('{commonappdata}\TransferUDT\Agent\keys\TransferUDT-Agent-Test.key');
      if DashboardPage.Values[4] = '' then
        DashboardPage.Values[4] := AgentPage.Values[2];
      if DashboardPage.Values[5] = '' then
        DashboardPage.Values[5] := ExpandConstant('{tmp}\TransferUDT-Agent-Test.pub');
    end;
  end;

  if CurPageID = AgentPage.ID then
  begin
    if HasBadConfigChars(AgentPage.Values[0]) or HasBadConfigChars(AgentPage.Values[1]) then
    begin
      MsgBox('Server targets e diretorios monitorados nao podem conter quebra de linha.', mbError, MB_OK);
      Result := False;
      Exit;
    end;
    if not IsSafeClientId(AgentPage.Values[2]) then
    begin
      MsgBox('Client ID deve usar apenas letras, numeros, hifen ou underline.', mbError, MB_OK);
      Result := False;
      Exit;
    end;
    if not StrongSecret(AgentPage.Values[3]) then
    begin
      MsgBox('Informe uma PSK real com pelo menos 32 caracteres.', mbError, MB_OK);
      Result := False;
      Exit;
    end;
    if ((AgentPage.Values[4] <> '') or (AgentPage.Values[5] <> '')) then
    begin
      if (AgentPage.Values[4] = '') or (AgentPage.Values[5] = '') or not StartsWithHttps(AgentPage.Values[4]) or
         HasBadConfigChars(AgentPage.Values[4]) or HasBadConfigChars(AgentPage.Values[5]) then
      begin
        MsgBox('Para habilitar dashboard no Agent, informe URL https:// e caminho da chave privada.', mbError, MB_OK);
        Result := False;
        Exit;
      end;
    end;

    if IsServerSelected() and (ServerPage.Values[4] = GeneratedPsk) then
    begin
      ServerPage.Values[3] := AgentPage.Values[2];
      ServerPage.Values[4] := AgentPage.Values[3];
    end;
    if IsDashboardSelected() then
    begin
      if (DashboardPage.Values[4] = '') or (DashboardPage.Values[4] = 'agent-default') then
        DashboardPage.Values[4] := AgentPage.Values[2];
      if DashboardPage.Values[5] = '' then
        DashboardPage.Values[5] := ExpandConstant('{tmp}\TransferUDT-Agent-Test.pub');
    end;
  end;

  if CurPageID = ServerPage.ID then
  begin
    if HasBadConfigChars(ServerPage.Values[0]) or HasBadConfigChars(ServerPage.Values[1]) or
       HasBadConfigChars(ServerPage.Values[2]) then
    begin
      MsgBox('Bind address, porta e IPs permitidos nao podem conter quebra de linha.', mbError, MB_OK);
      Result := False;
      Exit;
    end;
    if not IsSafeClientId(ServerPage.Values[3]) then
    begin
      MsgBox('Client ID permitido deve usar apenas letras, numeros, hifen ou underline.', mbError, MB_OK);
      Result := False;
      Exit;
    end;
    if not StrongSecret(ServerPage.Values[4]) then
    begin
      MsgBox('Informe uma PSK real com pelo menos 32 caracteres.', mbError, MB_OK);
      Result := False;
      Exit;
    end;
  end;

  if CurPageID = DashboardPage.ID then
  begin
    if IsAgentSelected() and IsDashboardSelected() then
    begin
      if AgentPage.Values[5] = ExpandConstant('{commonappdata}\TransferUDT\Agent\keys\TransferUDT-Agent-Test.key') then
        AgentPage.Values[4] := 'https://localhost:' + DashboardPage.Values[0];
      if DashboardPage.Values[4] = '' then
        DashboardPage.Values[4] := AgentPage.Values[2];
      if DashboardPage.Values[5] = '' then
        DashboardPage.Values[5] := ExpandConstant('{tmp}\TransferUDT-Agent-Test.pub');
    end;

    if DashboardPage.Values[1] = '' then
    begin
      DashboardPage.Values[1] := ExpandConstant('{tmp}\TransferUDT-Dashboard-Test.pfx');
    end;
    if DashboardPage.Values[2] = '' then
    begin
      DashboardPage.Values[2] := 'TransferUDT-Test-123!';
    end;

    if HasBadConfigChars(DashboardPage.Values[0]) or HasBadConfigChars(DashboardPage.Values[1]) or
       HasBadConfigChars(DashboardPage.Values[2]) or HasBadConfigChars(DashboardPage.Values[3]) then
    begin
      MsgBox('Configuracoes do Dashboard nao podem conter quebra de linha.', mbError, MB_OK);
      Result := False;
      Exit;
    end;
    if (DashboardPage.Values[0] = '') or (DashboardPage.Values[1] = '') or
       (DashboardPage.Values[2] = '') or (DashboardPage.Values[3] = '') then
    begin
      MsgBox('Informe porta, PFX, senha do PFX e senha do operador do Dashboard.', mbError, MB_OK);
      Result := False;
      Exit;
    end;
    if not FileExists(DashboardPage.Values[1]) then
    begin
      MsgBox('Arquivo PFX do Dashboard nao encontrado. Para teste local, mantenha o PFX padrao do wizard.', mbError, MB_OK);
      Result := False;
      Exit;
    end;
    if ((DashboardPage.Values[4] <> '') or (DashboardPage.Values[5] <> '')) then
    begin
      if (DashboardPage.Values[4] = '') or (DashboardPage.Values[5] = '') or
         not IsSafeClientId(DashboardPage.Values[4]) or not FileExists(DashboardPage.Values[5]) then
      begin
        MsgBox('Para validar heartbeat, informe Client ID valido e chave publica existente.', mbError, MB_OK);
        Result := False;
        Exit;
      end;
    end;
  end;
end;

function ConfigExists(Path: String): Boolean;
begin
  Result := FileExists(Path) and not WizardIsTaskSelected('overwriteconfig');
end;

procedure WriteAgentConfig;
var
  ConfigPath: String;
  DataRoot: String;
  Content: String;
begin
  if not IsAgentSelected() then
    Exit;

  DataRoot := ExpandConstant('{commonappdata}\TransferUDT');
  ConfigPath := DataRoot + '\Agent\config.properties';
  ForceDirectories(DataRoot + '\Agent');
  ForceDirectories(DataRoot + '\Agent\logs');
  ForceDirectories(DataRoot + '\Agent\db');
  ForceDirectories(DataRoot + '\Agent\watch');

  if ConfigExists(ConfigPath) then
    Exit;

  Content :=
    '# Generated by TransferUDT Setup' + #13#10 +
    'server.targets = ' + AgentPage.Values[0] + #13#10 +
    'chunk.size = 256' + #13#10 +
    'chunk.adaptive.enabled = true' + #13#10 +
    'chunk.adaptive.min_kb = 32' + #13#10 +
    'chunk.adaptive.max_kb = 4096' + #13#10 +
    'chunk.adaptive.initial_kb = 256' + #13#10 +
    'chunk.adaptive.target_ack_ms = 700' + #13#10 +
    'data.dirs = ' + ToConfigPath(AgentPage.Values[1]) + #13#10 +
    'resend.changed_files.enabled = true' + #13#10 +
    'resend.changed_files.identity = sha256' + #13#10 +
    'max.band = 10' + #13#10 +
    'seg.len = 1350' + #13#10 +
    'snd.buf = 64' + #13#10 +
    'rcv.buf = 64' + #13#10 +
    'snd.timeout = 20' + #13#10 +
    'rcv.timeout = 20' + #13#10 +
    'max.retries = 5' + #13#10 +
    'max.retries.abandon = 25' + #13#10 +
    'work.threads = 4' + #13#10 +
    'stability.check.interval.seconds = 5' + #13#10 +
    'stability.check.count = 3' + #13#10 +
    'watcher.check.interval.seconds = 5' + #13#10 +
    'pending.check.interval.seconds = 15' + #13#10 +
    'circuitbreaker.failure.threshold = 5' + #13#10 +
    'circuitbreaker.reset.timeout.seconds = 60' + #13#10 +
    'memory.usage.percent.limit = 70' + #13#10 +
    'log.filepath = ' + ToConfigPath(DataRoot + '\Agent\logs\agent.log') + #13#10 +
    'log.max_size_mb = 50' + #13#10 +
    'log.backup_count = 5' + #13#10 +
    'log.move.rotate = true' + #13#10 +
    'watcher.exclude.files = agent.log' + #13#10 +
    'db.filepath = ' + ToConfigPath(DataRoot + '\Agent\db\agent.db') + #13#10 +
    'pool.size = 4' + #13#10 +
    'log.flush_level = warn' + #13#10 +
    'security.enabled = true' + #13#10 +
    'security.handshake.enabled = true' + #13#10 +
    'security.allow_insecure = false' + #13#10 +
    'security.identity.mode = psk' + #13#10 +
    'security.client_id = ' + AgentPage.Values[2] + #13#10 +
    'security.psk = ' + AgentPage.Values[3] + #13#10 +
    '# certificate_handshake advanced settings:' + #13#10 +
    '# security.client_certificate_path = C:/ProgramData/TransferUDT/Agent/pki/agent-chain.pem' + #13#10 +
    '# security.ca_bundle_path = C:/ProgramData/TransferUDT/Agent/pki/ca.pem' + #13#10 +
    '# security.server_identity = transfer-server.example.internal' + #13#10;

  if (AgentPage.Values[4] <> '') and (AgentPage.Values[5] <> '') then
  begin
    Content := Content +
      'security.client_private_key_path = ' + ToConfigPath(AgentPage.Values[5]) + #13#10 +
      'dashboard.enabled = true' + #13#10 +
      'dashboard.url = ' + AgentPage.Values[4] + #13#10 +
      'dashboard.heartbeat.interval.seconds = 15' + #13#10 +
      'dashboard.log_tail.lines = 200' + #13#10;
  end
  else
  begin
    Content := Content +
      'dashboard.enabled = false' + #13#10 +
      'dashboard.url = https://server:8443' + #13#10 +
      'dashboard.heartbeat.interval.seconds = 15' + #13#10 +
      'dashboard.log_tail.lines = 200' + #13#10;
  end;

  SaveStringToFile(ConfigPath, Content, False);
end;

procedure WriteServerConfig;
var
  ConfigPath: String;
  DataRoot: String;
  Content: String;
begin
  if not IsServerSelected() then
    Exit;

  DataRoot := ExpandConstant('{commonappdata}\TransferUDT');
  ConfigPath := DataRoot + '\Server\config.properties';
  ForceDirectories(DataRoot + '\Server');
  ForceDirectories(DataRoot + '\Server\logs');
  ForceDirectories(DataRoot + '\Server\db');
  ForceDirectories(DataRoot + '\Server\storage');
  ForceDirectories(DataRoot + '\Server\reconstructed');

  if ConfigExists(ConfigPath) then
    Exit;

  Content :=
    '# Generated by TransferUDT Setup' + #13#10 +
    'server.port = ' + ServerPage.Values[1] + #13#10 +
    'server.bind_address = ' + ServerPage.Values[0] + #13#10 +
    'server.allowed_clients = ' + ServerPage.Values[2] + #13#10 +
    'server.storage_path = ' + ToConfigPath(DataRoot + '\Server\storage') + #13#10 +
    'server.reconstructed_path = ' + ToConfigPath(DataRoot + '\Server\reconstructed') + #13#10 +
    'server.max_file_size_mb = 10240' + #13#10 +
    'server.max_client_storage_mb = 20480' + #13#10 +
    'resend.changed_files.server_policy = overwrite' + #13#10 +
    'log.filepath = ' + ToConfigPath(DataRoot + '\Server\logs\server.log') + #13#10 +
    'db.filepath = ' + ToConfigPath(DataRoot + '\Server\db\server.db') + #13#10 +
    'max.band = 10' + #13#10 +
    'seg.len = 1350' + #13#10 +
    'snd.buf = 64' + #13#10 +
    'rcv.buf = 64' + #13#10 +
    'snd.timeout = 60' + #13#10 +
    'rcv.timeout = 60' + #13#10 +
    'work.threads = 4' + #13#10 +
    'work.max_pending_tasks = 256' + #13#10 +
    'max.connection = 100' + #13#10 +
    'circuitbreaker.failure.threshold = 5' + #13#10 +
    'circuitbreaker.reset.timeout.seconds = 60' + #13#10 +
    'log.max_size_mb = 50' + #13#10 +
    'log.backup_count = 5' + #13#10 +
    'log.flush_level = warn' + #13#10 +
    'test.ack_delay_pattern_ms =' + #13#10 +
    'security.enabled = true' + #13#10 +
    'security.handshake.enabled = true' + #13#10 +
    'security.allow_insecure = false' + #13#10 +
    'security.identity.mode = psk' + #13#10 +
    'security.allowed_client_ids = ' + ServerPage.Values[3] + #13#10 +
    'security.client_psk.' + ServerPage.Values[3] + ' = ' + ServerPage.Values[4] + #13#10 +
    '# certificate_handshake advanced settings:' + #13#10 +
    '# security.server_private_key_path = C:/ProgramData/TransferUDT/Server/pki/server.key' + #13#10 +
    '# security.server_certificate_path = C:/ProgramData/TransferUDT/Server/pki/server-chain.pem' + #13#10 +
    '# security.ca_bundle_path = C:/ProgramData/TransferUDT/Server/pki/ca.pem' + #13#10;

  SaveStringToFile(ConfigPath, Content, False);
end;

function SelectedComponentArgument(): String;
begin
  if IsAgentSelected() and IsServerSelected() and IsDashboardSelected() then
    Result := 'All'
  else if IsServerSelected() and IsDashboardSelected() then
    Result := 'ServerDashboard'
  else if IsAgentSelected() and IsServerSelected() then
    Result := 'Both'
  else if IsAgentSelected() then
    Result := 'Agent'
  else if IsDashboardSelected() then
    Result := 'Dashboard'
  else
    Result := 'Server';
end;

procedure RunServiceInstaller;
var
  Args: String;
  ResultCode: Integer;
begin
  Args :=
    '-NoProfile -ExecutionPolicy Bypass -File "' + ExpandConstant('{app}\installer\Install-TransferUDT.ps1') + '"' +
    ' -Component ' + SelectedComponentArgument() +
    ' -InstallRoot "' + ExpandConstant('{app}') + '"' +
    ' -DataRoot "' + ExpandConstant('{commonappdata}\TransferUDT') + '"';

  if IsAgentSelected() then
    Args := Args +
      ' -AgentExe "' + ExpandConstant('{app}\Agent\AgentUDTC++.exe') + '"' +
      ' -AgentConfig "' + ExpandConstant('{commonappdata}\TransferUDT\Agent\config.properties') + '"';

  if IsServerSelected() then
    Args := Args +
      ' -ServerExe "' + ExpandConstant('{app}\Server\ServerUDTC++.exe') + '"' +
      ' -ServerConfig "' + ExpandConstant('{commonappdata}\TransferUDT\Server\config.properties') + '"';

  if IsDashboardSelected() then
  begin
    Args := Args +
      ' -DashboardSource "' + ExpandConstant('{app}\Dashboard') + '"' +
      ' -DashboardPort ' + DashboardPage.Values[0] +
      ' -DashboardCertificate "' + DashboardPage.Values[1] + '"' +
      ' -DashboardCertificatePassword "' + DashboardPage.Values[2] + '"' +
      ' -DashboardOperatorPassword "' + DashboardPage.Values[3] + '"';
    if (DashboardPage.Values[4] <> '') and (DashboardPage.Values[5] <> '') then
      Args := Args +
        ' -DashboardAgentClientId "' + DashboardPage.Values[4] + '"' +
        ' -DashboardAgentPublicKey "' + DashboardPage.Values[5] + '"';
  end;

  if WizardIsTaskSelected('overwriteconfig') then
    Args := Args + ' -ForceConfig';

  if not WizardIsTaskSelected('startservices') then
    Args := Args + ' -NoStart';

  if IsDashboardSelected() and (CompareText(ExtractFileName(DashboardPage.Values[1]), 'TransferUDT-Dashboard-Test.pfx') = 0) then
    Args := Args + ' -TrustDashboardCertificate';

  if not Exec('powershell.exe', Args, '', SW_SHOW, ewWaitUntilTerminated, ResultCode) then
    RaiseException('Falha ao executar o instalador dos servicos.');

  if ResultCode <> 0 then
    RaiseException('O instalador dos servicos retornou erro: ' + IntToStr(ResultCode));
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
  begin
    WriteAgentConfig();
    WriteServerConfig();
    RunServiceInstaller();
  end;
end;
