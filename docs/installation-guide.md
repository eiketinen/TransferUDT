# TransferUDT Installation Guide

Este guia descreve instalacao, configuracao inicial e validacao pos-instalacao.

## 1. Preparar ambiente

Defina antes de instalar:

- Quais maquinas serao Agents.
- Qual maquina sera Server.
- Se o Dashboard ficara no mesmo host do Server.
- Porta UDT do Server, padrao `50051`.
- Porta HTTPS do Dashboard, padrao `8443`.
- `client_id` de cada Agent.
- PSK forte, par de chaves para `signed_handshake`, ou PKI interna com
  certificados `clientAuth`/`serverAuth` para `certificate_handshake`.
- Bundle PEM de CRLs e politica de renovacao/rollover quando a revogacao local
  estiver habilitada.
- Certificado `.pfx` HTTPS do Dashboard para producao.

## 2. Instalar Server

1. Execute o instalador como administrador.
2. Escolha `Somente Server` ou `Server e Dashboard`.
3. Informe:

```text
Bind address: 0.0.0.0
Porta: 50051
Clientes permitidos: IPs ou ranges da rede de Agents
Storage: C:\ProgramData\TransferUDT\Server\storage
Reconstructed: C:\ProgramData\TransferUDT\Server\reconstructed
Client IDs permitidos: agent-001,agent-002
PSK por client_id: mesma PSK configurada no Agent
```

4. Finalize mantendo `Iniciar os servicos apos instalar` marcado.
5. Valide:

```powershell
Get-Service ServerUDTService
Test-NetConnection localhost -Port 50051
```

## 3. Instalar Dashboard

1. Escolha `Server e Dashboard` ou `Agent, Server e Dashboard`.
2. Informe:

```text
Porta HTTPS: 8443
Arquivo PFX: certificado do Dashboard
Senha do PFX: senha do certificado
Senha do operador da UI: senha forte para login
Client ID do Agent para heartbeat: opcional
Chave publica do Agent para heartbeat: opcional
```

3. Para laboratorio local, o wizard pode preencher um PFX de avaliacao.
   Para producao, substitua por PFX emitido por CA interna.
4. Valide:

```powershell
Get-Service TransferUDTDashboardService
Start-Process https://localhost:8443/
```

## 4. Instalar Agent

1. Execute o instalador como administrador na maquina Agent.
2. Escolha `Somente Agent` ou instalacao completa para teste local.
3. Informe:

```text
Server targets: servidor:50051
Diretorios monitorados: C:\ProgramData\TransferUDT\Agent\watch
Client ID: agent-001
PSK: mesma PSK registrada no Server para agent-001
Dashboard URL: https://servidor-dashboard:8443
Chave privada do Agent: caminho da chave privada do Agent
```

4. Valide:

```powershell
Get-Service RadarAgentUDTService
Get-Content C:\ProgramData\TransferUDT\Agent\logs\agent.log -Tail 50
```

## 5. Teste funcional

Crie um arquivo na pasta monitorada:

```powershell
Set-Content -Path C:\ProgramData\TransferUDT\Agent\watch\teste-transferudt.txt -Value "TransferUDT OK" -Encoding UTF8
```

No Server, confira:

```powershell
Get-ChildItem C:\ProgramData\TransferUDT\Server\reconstructed -Recurse -File
```

No Dashboard, clique em `Atualizar` e verifique:

- Agent online.
- Falhas igual a zero.
- Logs recentes sem erro recorrente.
- Arquivo reconstruido no Server.

## 6. Portas e firewall

Liberar entrada no Server:

```powershell
New-NetFirewallRule -DisplayName "TransferUDT Server 50051" -Direction Inbound -Protocol TCP -LocalPort 50051 -Action Allow
```

Liberar Dashboard, se acessado remotamente:

```powershell
New-NetFirewallRule -DisplayName "TransferUDT Dashboard 8443" -Direction Inbound -Protocol TCP -LocalPort 8443 -Action Allow
```

## 7. Atualizar uma instalacao existente

1. Faca backup de `C:\ProgramData\TransferUDT`.
2. Execute o novo instalador como administrador.
3. Nao marque substituicao de configuracoes se quiser preservar os arquivos
   atuais.
4. Reinicie os servicos e valide logs.

Backup rapido:

```powershell
Compress-Archive -Path C:\ProgramData\TransferUDT -DestinationPath C:\Temp\TransferUDT-ProgramData-backup.zip -Force
```

## 8. Remover

Use o desinstalador do Windows ou:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "C:\Program Files\TransferUDT\installer\Uninstall-TransferUDT.ps1" -Component All
```
