# TransferUDT Troubleshooting

## Instalador falha com "O instalador dos servicos retornou erro: 1"

Verifique:

```powershell
Get-Content .\dist\dashboard-install.log -ErrorAction SilentlyContinue
Get-EventLog -LogName Application -Newest 50 | Where-Object Source -like "*TransferUDT*"
```

Causas comuns:

- Instalador nao foi executado como administrador.
- Servico antigo ainda bloqueando o executavel.
- Caminho de certificado `.pfx` invalido.
- Senha do `.pfx` incorreta.
- Configuracao antiga em `C:\ProgramData\TransferUDT` conflitante.

Acao:

1. Feche instancias antigas.
2. Execute o wizard como administrador.
3. Se for teste limpo, marque substituicao de configuracoes.
4. Se for producao, preserve ProgramData e corrija a configuracao manualmente.

## Dashboard abre, mas mostra tudo zero

Validar API:

```powershell
$env:NODE_TLS_REJECT_UNAUTHORIZED='0'
node -e "async function main(){const b='https://localhost:8443';const l=await fetch(b+'/api/login',{method:'POST',headers:{'content-type':'application/json'},body:JSON.stringify({password:'SENHA_OPERADOR'})});const c=l.headers.get('set-cookie');for(const p of ['/api/overview','/api/agents','/api/logs?limit=5']){const r=await fetch(b+p,{headers:{cookie:c}});console.log(p,r.status,await r.text())}}main()"
```

Se `/api/overview` mostra Agent online mas a UI nao atualiza, use `Ctrl+F5`.

Se `/api/agents` estiver vazio:

- Confirme `dashboard.enabled=true` no Agent.
- Confirme `dashboard.url=https://host:8443`.
- Confirme chave privada do Agent e chave publica cadastrada no Dashboard.
- Confira `agent.log` por rejeicoes HTTP 400/401.

## Agent nao conecta ao Server

No Agent:

```powershell
Test-NetConnection servidor -Port 50051
Get-Content C:\ProgramData\TransferUDT\Agent\logs\agent.log -Tail 100
```

No Server:

```powershell
Get-Service ServerUDTService
Get-Content C:\ProgramData\TransferUDT\Server\logs\server.log -Tail 100
```

Causas comuns:

- Porta bloqueada no firewall.
- `server.targets` incorreto.
- `server.allowed_clients` nao inclui o Agent.
- PSK/client_id divergente.
- `security.allow_insecure=false` com seguranca incompleta.
- Em `certificate_handshake`: CA incorreta, certificado expirado, EKU errado,
  SAN/CN diferente de `client_id`/`server_identity`, chave privada incompatível
  ou relogio do Windows fora de sincronismo.
- Em `security.revocation.mode=crl`: CRL ausente, maior que 4 MiB, malformada,
  expirada, assinada por outra CA, sem cobertura para a cadeia ou com o serial
  do certificado revogado. Atualize a CRL atomicamente e tente nova conexao.

## Arquivo nao e reconstruido

Verifique:

```powershell
Get-ChildItem C:\ProgramData\TransferUDT\Server\storage -Recurse -File
Get-ChildItem C:\ProgramData\TransferUDT\Server\reconstructed -Recurse -File
Get-Content C:\ProgramData\TransferUDT\Server\logs\server.log -Tail 200
```

Causas comuns:

- Arquivo ainda esta sendo escrito no Agent.
- Falta chunk.
- Hash de chunk invalido.
- Nome de arquivo rejeitado por seguranca.
- Nova versao do mesmo caminho rejeitada por
  `resend.changed_files.server_policy=reject`.
- Quota de cliente ou tamanho maximo excedido.

## Dashboard HTTPS com alerta de certificado

Em laboratorio local, o wizard pode usar certificado autoassinado. Para producao,
instale um `.pfx` emitido por CA interna confiada pelos navegadores dos
operadores.

## Logs com "Dashboard heartbeat rejected"

HTTP 400:

- Payload invalido.
- JSON malformado.
- Campo obrigatorio ausente.

HTTP 401:

- Assinatura invalida.
- Client ID sem chave publica no Dashboard.
- Chave privada do Agent nao corresponde a chave publica cadastrada.

HTTP 404/connection refused:

- Dashboard indisponivel.
- URL/porta incorreta.

## Reinstalacao limpa em laboratorio

Use apenas em maquina de teste:

```powershell
Stop-Service RadarAgentUDTService,ServerUDTService,TransferUDTDashboardService -ErrorAction SilentlyContinue
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "C:\Program Files\TransferUDT\installer\Uninstall-TransferUDT.ps1" -Component All
```

Depois remova manualmente `C:\ProgramData\TransferUDT` somente se nao precisar
preservar dados, logs ou chaves.
