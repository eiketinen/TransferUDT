# Contributing

## Development Flow

1. Create a branch from `main`.
2. Keep changes scoped to one issue or feature.
3. Run the Server and Agent test suites before opening a pull request.
4. Do not commit build outputs, databases, logs, local configs, or secrets.

## Build and Test

```powershell
msbuild ServerUDTC++_v3\ServerUDTC++Tests.vcxproj /p:Configuration=Debug /p:Platform=x64 /m
.\ServerUDTC++_v3\tests\bin\Debug\ServerUDTC++Tests.exe

msbuild AgentUDTC++_v7.2\AgentUDTC++Tests.vcxproj /p:Configuration=Debug /p:Platform=x64 /m
& "$env:LOCALAPPDATA\AgentUDTCppTests\bin\Debug\AgentUDTC++Tests.exe"
```

## Pull Request Checklist

- Tests pass locally.
- Public docs are updated when behavior or configuration changes.
- Security-sensitive changes include negative tests.
- No secrets, logs, databases, binaries, or local paths are added.

