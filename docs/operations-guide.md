# TransferUDT Operations Guide

Este guia cobre rotina de operacao, monitoramento e resposta a falhas.

## Retomada apos interrupcao

O Agent persiste o identificador da transferencia, offsets e chunks pendentes
no SQLite local. Se o processo for encerrado durante um envio, o proximo inicio
recupera o estado `processing`, reenvia os chunks necessarios e aceita o ACK
idempotente dos chunks que o Server ja havia armazenado. O Server nao mistura
chunks de identificadores de transferencia diferentes.

Para validar esse comportamento em uma instalacao de teste:

```powershell
.\scripts\e2e-production-readiness.ps1 -Configuration Release -Architecture x64 -ScenarioName agent-restart-resume -KeepRunRoot
```

O cenario somente passa quando o Agent reiniciado conclui o arquivo com o mesmo
SHA-256 e o Server registra a rejeicao idempotente do chunk duplicado.

## Servicos

Verificar estado:

```powershell
Get-Service RadarAgentUDTService,ServerUDTService,TransferUDTDashboardService
```

Reiniciar:

```powershell
Restart-Service RadarAgentUDTService
Restart-Service ServerUDTService
Restart-Service TransferUDTDashboardService
```

## Logs

Agent:

```powershell
Get-Content C:\ProgramData\TransferUDT\Agent\logs\agent.log -Tail 200
```

Server:

```powershell
Get-Content C:\ProgramData\TransferUDT\Server\logs\server.log -Tail 200
```

Dashboard:

```powershell
Get-Content C:\ProgramData\TransferUDT\Dashboard\logs\dashboard.log -Tail 200
```

## Dashboard

Acesse:

```text
https://localhost:8443/
```

Indicadores principais:

- Agents online: Agents com heartbeat recente.
- Agents offline: Agents conhecidos sem heartbeat recente.
- Arquivos pendentes: itens aguardando envio/finalizacao.
- Falhas: falhas reportadas pelo Agent ou Server.
- Server: estado de DB, log, chunks e reconstruidos.
- Logs: ultimas mensagens de Agent e Server.

## Validacoes apos manutencao

Depois de atualizar ou reiniciar:

```powershell
Get-Service RadarAgentUDTService,ServerUDTService,TransferUDTDashboardService
Test-NetConnection localhost -Port 50051
Test-NetConnection localhost -Port 8443
```

Se houver Agents remotos, rode `Test-NetConnection servidor -Port 50051` a partir
do host Agent.

## Backup recomendado

Backup operacional:

```powershell
Compress-Archive -Path C:\ProgramData\TransferUDT -DestinationPath C:\Backups\TransferUDT-ProgramData.zip -Force
```

Inclui:

- Configuracoes.
- Bancos SQLite.
- Logs.
- Chaves/certificados instalados.
- Chunks e arquivos reconstruidos, se ainda estiverem nas pastas padrao.

## Politica de logs

Os componentes possuem rotacao configuravel. Exemplo Agent:

```properties
log.max_size_mb = 50
log.backup_count = 5
log.move.rotate = true
```

Mantenha espaco em disco suficiente para storage, reconstructed, bancos e logs.

## Incidentes comuns

Agent offline:

1. Verifique `RadarAgentUDTService`.
2. Leia `agent.log`.
3. Confirme conectividade com o Server.
4. Confirme PSK/client_id.
5. Confirme permissao de leitura na pasta monitorada.

Server sem reconstruir:

1. Verifique se chunks chegaram ao storage.
2. Confira `server.log`.
3. Confirme que nao houve rejeicao por hash, tamanho ou quota.
4. Verifique `server.max_file_size_mb` e `server.max_client_storage_mb`.

Dashboard vazio:

1. Confirme login.
2. Clique `Atualizar`.
3. Confirme `TransferUDTDashboardService`.
4. Confirme `dashboard.enabled=true` no Agent.
5. Confira se a URL do Agent aponta para o Dashboard correto.

## Hardening operacional

- Restringir acesso a `C:\ProgramData\TransferUDT`.
- Usar PSK distinta por Agent, `signed_handshake`, ou
  `certificate_handshake` com PKI interna para maior garantia de identidade.
- Usar certificado HTTPS emitido por CA interna.
- Evitar `security.allow_insecure=true`.
- Expor Dashboard apenas em rede administrativa.
- Auditar alteracoes em configuracoes e chaves.
