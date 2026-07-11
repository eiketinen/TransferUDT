# TransferUDT Release Notes 1.0.0

Data do pacote oficial: 2026-05-13

## Resumo

Primeiro pacote organizado para distribuicao oficial em ambiente Windows x64.
Inclui Agent, Server, Dashboard HTTPS, wizard grafico, scripts de instalacao e
documentacao operacional.

## Destaques tecnicos

- Agent como servico Windows.
- Server como servico Windows.
- Dashboard HTTPS como servico Windows.
- Wizard com perfis Agent, Server, Server+Dashboard e completo.
- Transferencia por chunks com validacao de metadata e hash.
- Reconstrucao server-side.
- SQLite para estado local.
- Retentativa de chunks pendentes.
- Retomada idempotente apos encerramento inesperado do Agent, com
  `transfer_id`, offsets persistidos e deduplicacao server-side.
- Limites de tamanho de arquivo e quota por cliente.
- Reenvio configuravel de nova versao do mesmo caminho quando o conteudo muda:
  Agent detecta por SHA-256 e Server aplica politica `overwrite`, `reject` ou
  `versioned`.
- PSK challenge-response e AES-256-GCM.
- Suporte a identidade por `signed_handshake`.
- Suporte opcional a identidade mutua X.509 por `certificate_handshake`, com
  validacao de CA, EKU, SAN/CN e prova de posse da chave privada.
- Revogacao fail-closed por CRL PEM, rollover com CAs sobrepostas, recarga de
  material criptografico por handshake e alertas de expiracao configuraveis.
- Heartbeat do Agent para Dashboard.
- Health model com liveness publica minima e readiness autenticado por
  componente.
- Metricas operacionais por janela, tendencia de backlog, alertas ativos,
  filtros de logs, refresh automatico e retencao limitada de heartbeats.
- Contadores de heartbeat corrigidos para arquivos distintos, com totais
  persistidos de processados/falhos e horario UTC da ultima transferencia.
- Empacotamento x64/x86 gera e reutiliza em `dist\installer-assets` o mesmo
  conjunto completo de ativos de teste do wizard, sem reescrever a arvore
  `installer`, e rejeita conjuntos parciais para evitar identidades divergentes.

## Validacao esperada para release

- Build Release limpo de Agent e Server.
- Publish self-contained win-x64 do Dashboard.
- E2E do Dashboard cobrindo login, heartbeats assinados, transicao completa de
  fila para conclusao, health, metricas, tendencias e filtros de logs.
- Geracao do wizard Inno Setup.
- Agent tests: 37/37.
- Server tests: 44/44.
- E2E de producao: 14/14 cenarios com handshake X.509 e CRL, incluindo reinicio
  do Agent durante transferencia, certificado revogado, CA incorreta e SHA-256.
- SHA256 publicado para o instalador e para o pacote final.

## Observacoes de seguranca

Os assets gerados pelo wizard sao adequados para laboratorio local e primeiro
teste. Para producao, substitua:

- PFX do Dashboard por certificado emitido por CA interna.
- PSK por segredo forte e unico por Agent.
- Chaves de heartbeat por material criptografico controlado pela organizacao.

Nao publique `config.properties`, `.key`, `.pfx`, bancos SQLite ou logs com
dados sensiveis.

## Caminhos padrao

```text
C:\Program Files\TransferUDT
C:\ProgramData\TransferUDT
https://localhost:8443/
```

## Upgrade

Execute o instalador novo como administrador. Preserve configuracoes existentes
quando a maquina ja estiver em producao; substitua configuracoes apenas em
laboratorio ou quando essa for a intencao explicita.
