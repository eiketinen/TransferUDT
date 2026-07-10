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
- Heartbeat do Agent para Dashboard.

## Validacao esperada para release

- Build Release limpo de Agent e Server.
- Publish self-contained win-x64 do Dashboard.
- Geracao do wizard Inno Setup.
- Agent tests: 34/34.
- Server tests: 42/42.
- E2E de reinicio do Agent durante transferencia com validacao SHA-256.
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
