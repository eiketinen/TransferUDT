# Build do instalador Windows

Use sempre o fluxo unico abaixo para gerar o instalador:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\installer\build-installer.ps1 -Configuration Release
```

Esse comando faz a esteira completa:

1. Localiza o MSBuild do Visual Studio.
2. Compila `AgentUDTC++` em `Release|x64`.
3. Compila `ServerUDTC++` em `Release|x64`.
4. Publica `DashboardWeb` self-contained para `win-x64`.
5. Gera os assets padrao do wizard:
   - `installer\TransferUDT-Dashboard-Test.pfx`
   - `installer\TransferUDT-Agent-Test.key`
   - `installer\TransferUDT-Agent-Test.pub`
6. Valida que o `AgentUDTC++.exe` empacotado contem o cliente de heartbeat do Dashboard.
7. Executa o Inno Setup.
8. Gera:
   - `dist\TransferUDT-Setup-Release.exe`
   - `dist\TransferUDT-Setup-Release-Complete.exe`

Nao rode o Inno Setup diretamente para release. Isso pode empacotar executaveis antigos ou assets faltando.

## Padrao de teste local

Quando `Agent` e `Dashboard` forem selecionados juntos no wizard, o instalador configura automaticamente:

```text
dashboard.enabled = true
dashboard.url = https://localhost:8443
security.client_private_key_path = C:/ProgramData/TransferUDT/Agent/keys/TransferUDT-Agent-Test.key
```

O Dashboard recebe a chave publica correspondente em `AgentPublicKeys`, e o certificado HTTPS local de teste e instalado como confiavel no Windows para o Agent conseguir enviar heartbeat.

Os tres ativos de laboratorio (`TransferUDT-Dashboard-Test.pfx`,
`TransferUDT-Agent-Test.key` e `TransferUDT-Agent-Test.pub`) sao reutilizados
quando ja existem, garantindo que os pacotes x64 e x86 da mesma release usem o
mesmo par de chaves. Para regenera-los, remova os tres juntos antes do build;
um conjunto parcial interrompe o empacotamento para evitar identidades
inconsistentes.
