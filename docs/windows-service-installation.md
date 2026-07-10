# Instalacao como servico Windows

Este projeto pode ser instalado como dois servicos Windows:

- `RadarAgentUDTService`: agente que monitora arquivos e envia chunks.
- `ServerUDTService`: servidor que recebe, valida e reconstrui arquivos.
- `TransferUDTDashboardService`: dashboard web HTTPS read-only para observabilidade.

O instalador PowerShell em `installer/Install-TransferUDT.ps1` copia os binarios para `C:\Program Files\TransferUDT`, copia as configuracoes para `C:\ProgramData\TransferUDT` e registra variaveis de ambiente especificas de cada servico no Service Control Manager.

## Por que nao usar System32

Quando um executavel roda como servico, o diretorio atual costuma ser `C:\Windows\System32`. Por isso, quando o codigo faz fallback para `config.properties`, ele acaba procurando a configuracao nesse diretorio.

O instalador evita esse comportamento configurando as variaveis abaixo diretamente no registro do servico:

- `AGENT_CONFIG_PATH=C:\ProgramData\TransferUDT\Agent\config.properties`
- `SERVER_CONFIG_PATH=C:\ProgramData\TransferUDT\Server\config.properties`
- `TRANSFERUDT_DASHBOARD_CERT_PASSWORD=<senha-do-pfx>`
- `TRANSFERUDT_DASHBOARD_OPERATOR_PASSWORD=<senha-do-operador>`

Essas variaveis sao carregadas quando o servico inicia. Portanto, para mudar o caminho ou atualizar a configuracao, nao e necessario reiniciar a maquina; reinicie apenas o servico afetado.

## Pre-requisitos

1. Compile os binarios do Agent e do Server.
2. Para gerar o wizard, instale o Inno Setup 6 na maquina de build.
3. Para instalacao via PowerShell, abra o PowerShell como Administrador.
4. Para instalacao via PowerShell, prepare os arquivos reais `config.properties` do Agent e do Server. No wizard, esses arquivos sao gerados pelas telas de configuracao.

## Instalacao com wizard

Para distribuir o Agent para varias maquinas, gere um instalador grafico com Inno Setup 6:

```powershell
powershell -ExecutionPolicy Bypass -File .\installer\build-installer.ps1 -Configuration Release
```

O executavel sera gerado em:

```text
dist\TransferUDT-Setup-Release.exe
```

No wizard, escolha:

- `Somente Agent` para maquinas que apenas enviam arquivos.
- `Somente Server` para maquinas que recebem arquivos.
- `Server e Dashboard` para maquinas que recebem arquivos e expõem a observabilidade.
- `Agent, Server e Dashboard` para ambientes de teste ou maquinas que acumulam as três funcoes.

O wizard grava as configuracoes em `C:\ProgramData\TransferUDT` e instala os servicos automaticamente. Para o Agent, informe `server.targets` no formato `host:porta`; para mais de um Server, separe por ponto e virgula, por exemplo:

```text
10.0.0.10:50051;10.0.0.11:50051
```

Para o Server, informe os IPs permitidos conforme a regra usada pelo projeto em `server.allowed_clients`. O `Client ID` e a `PSK` do Agent precisam existir no Server como `security.allowed_client_ids` e `security.client_psk.<client_id>`.

O wizard preenche a PSK inicial com um segredo de 32 bytes gerado pelo RNG do Windows. Em instalacoes separadas, guarde a PSK usada no Agent e cadastre a mesma PSK no Server para o `Client ID` correspondente.

Para instalar o Dashboard, informe:

- porta HTTPS, por padrão `8443`;
- arquivo `.pfx` emitido pela CA interna;
- senha do `.pfx`;
- senha do operador da UI;
- opcionalmente, `client_id` e chave pública do Agent para validar heartbeats assinados.

O Dashboard é instalado em `C:\Program Files\TransferUDT\Dashboard`, grava dados em `C:\ProgramData\TransferUDT\Dashboard` e lê o banco/log do Server em modo read-only. O serviço usa HTTPS obrigatório em produção.

Para habilitar telemetria do Agent para o Dashboard no wizard, preencha a URL `https://servidor:8443` e o caminho da chave privada do Agent. Se esses campos ficarem vazios, o Agent será instalado com `dashboard.enabled=false`.

Por padrao, o instalador restringe a ACL de `C:\ProgramData\TransferUDT\Agent` e `C:\ProgramData\TransferUDT\Server` para `Administrators` e `SYSTEM`. Se algum processo de negocio precisar gravar diretamente na pasta monitorada do Agent, ajuste a ACL apenas dessa pasta especifica depois da instalacao.

Para producao em rede privada controlada, prefira configurar
`security.identity.mode=signed_handshake` apos instalar os arquivos de chave.
Para identidade X.509, instale chave privada, bundle de certificados e CA com
ACL de leitura para a conta do servico, configure
`security.identity.mode=certificate_handshake` nos dois peers. Para revogacao,
instale tambem o bundle PEM de CRLs e configure
`security.revocation.mode=crl`. Depois da ativacao inicial, substituicoes
atomicas de certificado, chave, CA e CRL sao usadas por novos handshakes sem
encerrar as sessoes ativas.
Veja `docs/security.md` para o fluxo de geracao e distribuicao de chaves.

## Instalacao via PowerShell

Exemplo usando os caminhos padrao de build do repositorio:

```powershell
powershell -ExecutionPolicy Bypass -File .\installer\Install-TransferUDT.ps1 `
  -AgentExe .\AgentUDTC++_v7.2\bin\Debug\AgentUDTC++.exe `
  -ServerExe .\ServerUDTC++_v3\bin\x64\Debug\ServerUDTC++.exe `
  -AgentConfig .\AgentUDTC++_v7.2\config.properties `
  -ServerConfig .\ServerUDTC++_v3\config.properties
```

Instalar Server + Dashboard via PowerShell:

```powershell
powershell -ExecutionPolicy Bypass -File .\installer\Install-TransferUDT.ps1 `
  -Component ServerDashboard `
  -ServerExe .\ServerUDTC++_v3\bin\x64\Debug\ServerUDTC++.exe `
  -ServerConfig .\ServerUDTC++_v3\config.properties `
  -DashboardSource .\DashboardWeb\bin\Release\net8.0\win-x64\publish `
  -DashboardCertificate C:\certs\dashboard.pfx `
  -DashboardCertificatePassword "<senha-do-pfx>" `
  -DashboardOperatorPassword "<senha-do-operador>"
```

Instalar apenas um componente:

```powershell
powershell -ExecutionPolicy Bypass -File .\installer\Install-TransferUDT.ps1 `
  -Component Agent `
  -AgentExe .\AgentUDTC++_v7.2\bin\Debug\AgentUDTC++.exe `
  -AgentConfig .\AgentUDTC++_v7.2\config.properties
```

Por padrao o instalador preserva configuracoes ja existentes em `C:\ProgramData\TransferUDT`. Para substituir a configuracao existente, use `-ForceConfig`.

## Atualizar configuracao sem reboot

Atualize o arquivo em `C:\ProgramData\TransferUDT` e reinicie somente o servico:

```powershell
Copy-Item .\AgentUDTC++_v7.2\config.properties C:\ProgramData\TransferUDT\Agent\config.properties -Force
Restart-Service RadarAgentUDTService
```

Para o servidor:

```powershell
Copy-Item .\ServerUDTC++_v3\config.properties C:\ProgramData\TransferUDT\Server\config.properties -Force
Restart-Service ServerUDTService
```

## Verificacao

Verifique os servicos:

```powershell
Get-Service RadarAgentUDTService,ServerUDTService,TransferUDTDashboardService
```

Verifique as variaveis especificas do servico:

```powershell
Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\RadarAgentUDTService' -Name Environment
Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\ServerUDTService' -Name Environment
```

Verifique os caminhos instalados:

```powershell
Get-ChildItem 'C:\Program Files\TransferUDT'
Get-ChildItem 'C:\ProgramData\TransferUDT'
```

## Atualizar binarios

Recompile o projeto e rode novamente o instalador apontando para os novos executaveis. O script para os servicos, substitui os binarios, reaplica o ambiente do servico e inicia novamente.

```powershell
powershell -ExecutionPolicy Bypass -File .\installer\Install-TransferUDT.ps1
```

## Remocao

Remover apenas os servicos, mantendo binarios e dados:

```powershell
powershell -ExecutionPolicy Bypass -File .\installer\Uninstall-TransferUDT.ps1
```

Remover tambem os binarios instalados:

```powershell
powershell -ExecutionPolicy Bypass -File .\installer\Uninstall-TransferUDT.ps1 -RemoveFiles
```

Remover binarios, configuracoes e dados:

```powershell
powershell -ExecutionPolicy Bypass -File .\installer\Uninstall-TransferUDT.ps1 -RemoveFiles -RemoveData
```
