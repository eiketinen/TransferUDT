# Official Release Package

O pacote oficial e gerado por:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\New-TransferUDTRelease.ps1 -Version 1.0.0 -Configuration Release -Build -RunTests
```

Saidas:

```text
dist\releases\TransferUDT-1.0.0-windows-x64\
dist\releases\TransferUDT-1.0.0-windows-x64.zip
dist\releases\TransferUDT-1.0.0-windows-x64-SHA256SUMS.txt
```

Conteudo:

- `bin\TransferUDT-1.0.0-windows-x64-setup.exe`
- `README.html`
- `docs\TransferUDT-1.0.0-user-guide.html`
- `source\TransferUDT-1.0.0-source.zip`
- `SHA256SUMS.txt`
- `LICENSE`
- `SECURITY.md`

O pacote de fonte remove artefatos de build, bancos, logs, executaveis,
certificados, chaves privadas, `dist`, `.git`, `bin`, `obj`, `vcpkg_installed`
e arquivos locais de runtime.

Os documentos voltados ao usuario final devem ser publicados em HTML
auto-contido. Markdown pode continuar existindo no pacote de fonte para
manutencao tecnica, mas a distribuicao oficial deve apontar o usuario para
`README.html` e para o guia HTML em `docs`.

## Checklist antes de entregar

1. `git status --short` revisado.
2. Build Release completo.
3. Testes Release passando.
4. Instalador gerado em `dist`.
5. Pacote oficial gerado em `dist\releases`.
6. Hash SHA256 conferido.
7. Documentacao revisada.
8. Nenhum segredo real incluido no pacote de fonte.
