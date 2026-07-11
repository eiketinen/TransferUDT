# TransferUDT Product Manual

Versao do documento: 1.0.0
Plataforma alvo: Windows x64
Status: pacote oficial para rede privada/controlada

## Visao geral

TransferUDT e um sistema Windows para transferencia de arquivos em alta
performance usando UDT. O produto e composto por tres partes:

- Agent: monitora uma ou mais pastas locais, divide arquivos em chunks e envia
  para um ou mais Servers.
- Server: recebe chunks, valida integridade, controla estado em SQLite e
  reconstrui os arquivos no destino.
- Dashboard: painel HTTPS local para observar agentes, status do servidor,
  transferencias e logs.

O instalador grafico permite instalar Agent, Server, Dashboard ou combinacoes
desses componentes como servicos Windows.

## Quando usar

Use TransferUDT quando precisar mover arquivos entre maquinas Windows em rede
privada/controlada, com retomada por chunks, controle de falhas e observabilidade
local. O produto foi preparado para operar como servico, sem depender de uma
janela de terminal aberta.

## Limites de responsabilidade

TransferUDT nao substitui uma revisao formal de seguranca para ambientes
regulados ou expostos a Internet. Para producao, mantenha o sistema em rede
privada, use senhas fortes, certificados internos confiaveis e acesso
administrativo restrito aos diretorios de configuracao.

## Componentes instalados

Diretorio de binarios:

```text
C:\Program Files\TransferUDT
```

Diretorio de dados e configuracao:

```text
C:\ProgramData\TransferUDT
```

Servicos Windows:

```text
RadarAgentUDTService
ServerUDTService
TransferUDTDashboardService
```

## Fluxo de dados

1. Um arquivo aparece em uma pasta monitorada pelo Agent.
2. O Agent aguarda estabilidade do arquivo antes de processar.
3. O Agent calcula o fingerprint do conteudo para evitar reenvio duplicado.
4. O Agent divide o arquivo em chunks e envia para o Server.
5. O Server valida metadata, tamanho, hash e politica de cliente.
6. O Server grava chunks recebidos e reconstrui o arquivo quando todos chegam.
7. Se o mesmo caminho for enviado com conteudo novo, o Server aplica a politica
   `resend.changed_files.server_policy`.
8. O Dashboard le bancos e logs locais para exibir status operacional.
9. O Agent pode enviar heartbeats assinados para o Dashboard.

## Seguranca

O modo recomendado usa:

- `security.enabled=true`
- `security.handshake.enabled=true`
- `security.allow_insecure=false`
- PSK forte por cliente ou `signed_handshake` com chaves assimetricas
- Dashboard somente em HTTPS

O wizard gera valores de teste para facilitar laboratorio local. Para producao,
substitua por certificados e segredos controlados pela sua organizacao.

## Arquivos importantes

Agent:

```text
C:\ProgramData\TransferUDT\Agent\config.properties
C:\ProgramData\TransferUDT\Agent\watch
C:\ProgramData\TransferUDT\Agent\db\agent.db
C:\ProgramData\TransferUDT\Agent\logs\agent.log
```

Server:

```text
C:\ProgramData\TransferUDT\Server\config.properties
C:\ProgramData\TransferUDT\Server\storage
C:\ProgramData\TransferUDT\Server\reconstructed
C:\ProgramData\TransferUDT\Server\db\server.db
C:\ProgramData\TransferUDT\Server\logs\server.log
```

Dashboard:

```text
C:\ProgramData\TransferUDT\Dashboard\appsettings.json
C:\ProgramData\TransferUDT\Dashboard\db\dashboard.db
C:\ProgramData\TransferUDT\Dashboard\logs\dashboard.log
C:\ProgramData\TransferUDT\Dashboard\certs
```

## Requisitos

Maquina de execucao:

- Windows 10/11 ou Windows Server x64
- Usuario administrador para instalar servicos
- Porta TCP do Server liberada entre Agent e Server, padrao `50051`
- Porta HTTPS do Dashboard liberada para operadores, padrao `8443`

Maquina de build:

- Windows x64
- Visual Studio 2022 com workload "Desktop development with C++"
- .NET SDK 8
- Inno Setup 6
- Dependencias C++ via vcpkg conforme os `vcpkg.json`

## Instalacao rapida com wizard

1. Execute o instalador como administrador.
2. Escolha o tipo:
   - `Somente Agent`
   - `Somente Server`
   - `Server e Dashboard`
   - `Agent, Server e Dashboard`
3. Configure os campos solicitados pelo wizard.
4. Mantenha marcado `Iniciar os servicos apos instalar`.
5. Ao final, valide os servicos:

```powershell
Get-Service RadarAgentUDTService,ServerUDTService,TransferUDTDashboardService
```

6. Abra o Dashboard, quando instalado:

```text
https://localhost:8443/
```

## Configuracao minima do Agent

```properties
server.targets = 10.0.0.10:50051
data.dirs = C:/ProgramData/TransferUDT/Agent/watch
resend.changed_files.enabled = true
resend.changed_files.identity = sha256
security.enabled = true
security.handshake.enabled = true
security.allow_insecure = false
security.identity.mode = psk
security.client_id = agent-001
security.psk = use-um-segredo-forte-com-32-ou-mais-caracteres
```

Para Dashboard:

```properties
dashboard.enabled = true
dashboard.url = https://servidor-dashboard:8443
dashboard.heartbeat.interval.seconds = 15
security.client_private_key_path = C:/ProgramData/TransferUDT/Agent/keys/agent-001.key
```

## Configuracao minima do Server

```properties
server.bind_address = 0.0.0.0
server.port = 50051
server.allowed_clients = 10.0.0.11,10.0.0.12
server.storage_path = C:/ProgramData/TransferUDT/Server/storage
server.reconstructed_path = C:/ProgramData/TransferUDT/Server/reconstructed
resend.changed_files.server_policy = overwrite
security.enabled = true
security.handshake.enabled = true
security.allow_insecure = false
security.identity.mode = psk
security.allowed_client_ids = agent-001
security.client_psk.agent-001 = use-um-segredo-forte-com-32-ou-mais-caracteres
```

Para maior garantia de identidade em rede privada com PKI interna, configure
Agent e Server com `security.identity.mode=certificate_handshake`. O Agent usa
`security.client_private_key_path`, `security.client_certificate_path`,
`security.ca_bundle_path` e `security.server_identity`. O Server usa
`security.server_private_key_path`, `security.server_certificate_path` e
`security.ca_bundle_path`. Certificados de Agent precisam de EKU `clientAuth`;
o certificado do Server precisa de `serverAuth`. O perfil valida CA e SAN/CN,
mas nao encapsula UDT em TLS/mTLS.

Para revogacao local, configure nos dois componentes
`security.revocation.mode=crl` e `security.crl_path`. A CRL e aplicada de forma
fail-closed em novos handshakes. Certificado, chave, bundle de CA e CRL sao
recarregados a cada nova conexao; transferencias ja autenticadas continuam.
Durante rollover, mantenha temporariamente as CAs antiga e nova no mesmo bundle.
`security.certificate_expiry_warning_days` controla o alerta de vencimento.

## Configuracao minima do Dashboard

O wizard cria `appsettings.json` em `C:\ProgramData\TransferUDT\Dashboard`.
Para producao, use um `.pfx` emitido por CA interna e uma senha de operador
forte.

Campos principais:

```json
{
  "Dashboard": {
    "BindAddress": "0.0.0.0",
    "HttpsPort": 8443,
    "RequireHttps": true,
    "CertificatePath": "C:/ProgramData/TransferUDT/Dashboard/certs/dashboard.pfx",
    "DatabasePath": "C:/ProgramData/TransferUDT/Dashboard/db/dashboard.db",
    "HeartbeatRetentionDays": 30,
    "MetricsWindowHours": 24,
    "MetricsBucketMinutes": 60,
    "LowDiskWarningBytes": 5368709120,
    "AutoRefreshSeconds": 30,
    "ServerDatabasePath": "C:/ProgramData/TransferUDT/Server/db/server.db",
    "ServerLogPath": "C:/ProgramData/TransferUDT/Server/logs/server.log"
  }
}
```

## Operacao diaria

Comandos uteis:

```powershell
Get-Service RadarAgentUDTService,ServerUDTService,TransferUDTDashboardService
Restart-Service RadarAgentUDTService
Restart-Service ServerUDTService
Restart-Service TransferUDTDashboardService
```

Validar arquivos reconstruidos:

```powershell
Get-ChildItem C:\ProgramData\TransferUDT\Server\reconstructed -Recurse -File
```

Ler logs recentes:

```powershell
Get-Content C:\ProgramData\TransferUDT\Agent\logs\agent.log -Tail 100
Get-Content C:\ProgramData\TransferUDT\Server\logs\server.log -Tail 100
Get-Content C:\ProgramData\TransferUDT\Dashboard\logs\dashboard.log -Tail 100
```

## Atualizacao

Para atualizar:

1. Gere um novo instalador oficial.
2. Execute o instalador como administrador na maquina alvo.
3. Use a opcao de substituir configuracoes apenas quando quiser sobrescrever
   `C:\ProgramData\TransferUDT`.
4. Reinicie os servicos afetados.
5. Valide Dashboard e logs.

## Desinstalacao

Use "Aplicativos instalados" do Windows ou rode:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "C:\Program Files\TransferUDT\installer\Uninstall-TransferUDT.ps1" -Component All
```

Os dados em `C:\ProgramData\TransferUDT` podem ser preservados para auditoria ou
removidos manualmente conforme politica interna.
