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
- Saude dos componentes: distingue `healthy`, `degraded` e `unhealthy` para
  banco do Dashboard, banco/log do Server e heartbeats dos Agents.
- Concluidas e erros na janela: eventos persistidos pelo Server nas ultimas 6,
  24, 72 ou 168 horas.
- Tendencia de backlog: ultimo heartbeat de cada Agent por bucket de tempo.
- Alertas ativos: Agent offline, servico fora de `running`, arquivo com falha ou
  disco livre abaixo do limite.

Probes para monitoramento externo:

```text
GET https://servidor:8443/health/live
GET https://servidor:8443/api/health
GET https://servidor:8443/api/metrics?windowHours=24
```

Use somente `/health/live` sem autenticacao. Os dois endpoints `/api/*` exigem
sessao de operador e retornam detalhes operacionais. Nao use a liveness para
decidir se bancos, logs ou Agents estao prontos; essa decisao pertence ao
readiness autenticado.

Defaults do historico e painel:

```json
"HeartbeatRetentionDays": 30,
"MetricsWindowHours": 24,
"MetricsBucketMinutes": 60,
"LowDiskWarningBytes": 5368709120,
"AutoRefreshSeconds": 30
```

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

## Rotacao e revogacao de certificados

1. Adicione a nova CA ao bundle existente em Agent e Server.
2. Atualize atomicamente o bundle PEM de CRLs para cobrir todas as CAs emissoras.
3. Substitua certificado e chave privada correspondentes.
4. Confirme novos handshakes e acompanhe alertas de expiracao.
5. Remova a CA antiga somente depois da migracao de todos os peers.

Use `security.revocation.mode=crl` e `security.crl_path` nos dois lados. CRL
ausente, invalida, expirada ou que revogue o peer bloqueia novas conexoes. As
sessoes ja estabelecidas nao sao encerradas; uma revogacao passa a valer no
proximo handshake.
