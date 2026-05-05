// main.cpp
#include <thread>
#include <memory> // For std::unique_ptr
#include <string>
#include <cstdio> // For printf
#include <cstring> // For strcmp
#include "ServerUDT.h"      // Use this consistently
#include "Logger.h"         // Include logger for service logging
#include "ServerConfig.h"   // Include ServerConfig to get log path for initialization
#include <windows.h>

constexpr auto SERVICE_NAME = L"ServerUDTService"; // Generic Service name (MUST be wide string) - Adjusted;

// Global variables for service
SERVICE_STATUS        g_ServiceStatus = { 0 };
SERVICE_STATUS_HANDLE g_ServiceStatusHandle = NULL;
HANDLE                g_hStopEvent = NULL; // Event to signal service stop

std::unique_ptr<ServerUDT> g_Server; // Use ServerUDT consistently
std::thread                g_ServerThread; // Thread for running the server's main loop

// Forward declarations
VOID WINAPI ServiceMain(DWORD argc, LPWSTR* argv);
VOID WINAPI ServiceCtrlHandler(DWORD CtrlCode);
VOID ReportSvcStatus(DWORD dwCurrentState, DWORD dwWin32ExitCode, DWORD dwWaitHint);
VOID LogServiceEvent(const std::string& message, WORD eventType = EVENTLOG_INFORMATION_TYPE);
bool InitializeGlobalLogger(); // Helper function for logger initialization

// --- Logger Initialization Helper ---
bool InitializeGlobalLogger() {
    try {
        std::string logFilePath = "server.log"; // Default fallback
        int logMaxSizeMB = 50;
        int logBackupCount = 5;
        std::string logFlushLevel = "warn";
        try {
            // Access config temporarily - this might print errors if config itself fails
            auto& config = ServerConfig::getInstance();
            logFilePath = config.getLogFilePath();
            logMaxSizeMB = config.getLogMaxSizeMB();
            logBackupCount = config.getLogBackupCount();
            logFlushLevel = config.getLogFlushLevel();
        }
        catch (const std::exception& e) {
            OutputDebugStringA(("Warning: Failed to get config for logger init: " + std::string(e.what()) + ". Using defaults.\n").c_str());
            // Use default log path and settings if config fails early
        }
        catch (...) {
            OutputDebugStringA("Warning: Unknown error getting config for logger init. Using defaults.\n");
        }

        Logger::Initialize(logFilePath,
            static_cast<std::uintmax_t>(logMaxSizeMB) * 1024 * 1024,
            logBackupCount, logFlushLevel);
        Logger::getInstance().info("main", "Logger initialized.");
        return true;
    }
    catch (const std::exception& e) {
        std::string errorMsg = "FATAL: Logger initialization failed: " + std::string(e.what()) + "\n";
        OutputDebugStringA(errorMsg.c_str()); // Log to debugger output
        printf("%s", errorMsg.c_str()); // Log to console if possible
        // Try to log to event log as last resort (might fail if source not registered)
        HANDLE hEventSource = RegisterEventSourceW(NULL, SERVICE_NAME);
        if (hEventSource != NULL) {
            LPCWSTR lpszStrings[1];
            std::wstring wMessage = std::wstring(errorMsg.begin(), errorMsg.end());
            lpszStrings[0] = wMessage.c_str();
            ReportEventW(hEventSource, EVENTLOG_ERROR_TYPE, 0, 0, NULL, 1, 0, lpszStrings, NULL);
            DeregisterEventSource(hEventSource);
        }
        return false;
    }
    catch (...) {
        OutputDebugStringA("FATAL: Unknown exception during Logger initialization.\n");
        printf("FATAL: Unknown exception during Logger initialization.\n");
        return false;
    }
}

// --- Service Status Reporting ---
VOID ReportSvcStatus(DWORD dwCurrentState, DWORD dwWin32ExitCode, DWORD dwWaitHint)
{
    static DWORD dwCheckPoint = 1;

    // Fill in the SERVICE_STATUS structure.
    g_ServiceStatus.dwCurrentState = dwCurrentState;
    g_ServiceStatus.dwWin32ExitCode = dwWin32ExitCode;
    g_ServiceStatus.dwWaitHint = dwWaitHint;

    // Accept stop controls when running, pending, or paused.
    if (dwCurrentState == SERVICE_START_PENDING)
        g_ServiceStatus.dwControlsAccepted = 0;
    else
        g_ServiceStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP;

    if ((dwCurrentState == SERVICE_RUNNING) || (dwCurrentState == SERVICE_STOPPED))
        g_ServiceStatus.dwCheckPoint = 0;
    else
        g_ServiceStatus.dwCheckPoint = dwCheckPoint++;

    // Report the status of the service to the SCM.
    // Only report if status handle is valid
    if (g_ServiceStatusHandle != NULL) {
        SetServiceStatus(g_ServiceStatusHandle, &g_ServiceStatus);
    }
}

// --- Service Control Handler ---
VOID WINAPI ServiceCtrlHandler(DWORD CtrlCode)
{
    switch (CtrlCode)
    {
    case SERVICE_CONTROL_STOP:
        LogServiceEvent("Received SERVICE_CONTROL_STOP request.");
        ReportSvcStatus(SERVICE_STOP_PENDING, NO_ERROR, 5000); // Give 5 seconds initial hint
        // Signal the service to stop.
        if (g_hStopEvent != NULL)
            SetEvent(g_hStopEvent); // This signals the ServiceMain loop
        break;

    case SERVICE_CONTROL_INTERROGATE:
        // Fall through to send current status
        break;

    default:
        break;
    }

    // Report current status even for interrogate or unknown controls
    // Use the last known state unless it's STOP_PENDING
    if (CtrlCode != SERVICE_CONTROL_STOP) {
        ReportSvcStatus(g_ServiceStatus.dwCurrentState, NO_ERROR, 0);
    }
}


// --- Service Main Function ---
VOID WINAPI ServiceMain(DWORD argc, LPWSTR* argv)
{
    // 1. Registrar o handler de controle do serviço com o SCM (Service Control Manager)
    g_ServiceStatusHandle = RegisterServiceCtrlHandlerW(SERVICE_NAME, ServiceCtrlHandler);
    if (g_ServiceStatusHandle == NULL)
    {
        // Falha crítica inicial, não é possível reportar ao SCM
        OutputDebugStringA("FATAL: RegisterServiceCtrlHandlerW failed.");
        return;
    }

    // 2. Inicializar a estrutura de status do serviço
    g_ServiceStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_ServiceStatus.dwServiceSpecificExitCode = 0;

    // 3. Reportar o estado inicial: PENDENTE PARA INICIAR
    ReportSvcStatus(SERVICE_START_PENDING, NO_ERROR, 3000);

    // 4. Criar o evento de parada
    g_hStopEvent = CreateEvent(NULL, TRUE, FALSE, NULL); // Reset manual, inicialmente não sinalizado
    if (g_hStopEvent == NULL)
    {
        // Se a criação do evento falhar, o serviço não pode operar
        ReportSvcStatus(SERVICE_STOPPED, GetLastError(), 0);
        return;
    }

    // 5. Bloco Try-Catch para todo o ciclo de vida do serviço
    try
    {
        // 6. Inicializar o logger global. É uma etapa crítica.
        if (!InitializeGlobalLogger())
        {
            // A própria função InitializeGlobalLogger tenta registrar no Event Log em caso de falha.
            // Lançar uma exceção garante que o serviço pare de forma limpa.
            throw std::runtime_error("Logger initialization failed. The service cannot continue.");
        }
        LogServiceEvent("Logger initialized. Creating server instance.");

        // 7. Criar a instância do servidor. O construtor pode falhar.
        g_Server = std::make_unique<ServerUDT>();
        LogServiceEvent("Server instance created successfully.");

        ReportSvcStatus(SERVICE_START_PENDING, NO_ERROR, 5000); // Atualiza o tempo de espera

        // 8. Iniciar o servidor. run(true) é não-bloqueante e retorna false em caso de falha.
        if (!g_Server->run(true))
        {
            // Se run() retornar false, a inicialização falhou (ex: porta em uso, erro de banco de dados).
            throw std::runtime_error("ServerUDT::run(true) returned false, indicating a critical startup failure.");
        }
        LogServiceEvent("ServerUDT::run(true) completed. The server is now listening in the background.");

        // 9. Reportar que o serviço está em execução
        ReportSvcStatus(SERVICE_RUNNING, NO_ERROR, 0);
        LogServiceEvent("Service is now fully running.");

        // 10. Aguardar indefinidamente pelo sinal de parada do SCM
        WaitForSingleObject(g_hStopEvent, INFINITE);

        // --- Início da Sequência de Desligamento ---
        LogServiceEvent("Stop event received. Beginning graceful shutdown.");
        ReportSvcStatus(SERVICE_STOP_PENDING, NO_ERROR, 30000); // Dar 30 segundos para desligar

        // 11. O reset do unique_ptr chamará o destrutor de ServerUDT,
        // que por sua vez chama o método stop() para um desligamento limpo.
        LogServiceEvent("Cleaning up server instance (this will trigger the shutdown routine)...");
        g_Server.reset();
        LogServiceEvent("Server instance has been successfully reset and cleaned up.");
    }
    catch (const std::exception& e)
    {
        // Captura qualquer exceção padrão durante a inicialização ou execução
        std::string errorMessage = "A critical exception occurred, causing the service to stop: " + std::string(e.what());
        LogServiceEvent(errorMessage, EVENTLOG_ERROR_TYPE);

        // Garante que o servidor seja limpo se a exceção ocorreu após sua criação
        if (g_Server) g_Server.reset();

        ReportSvcStatus(SERVICE_STOPPED, ERROR_SERVICE_SPECIFIC_ERROR, 1);
        if (g_hStopEvent) CloseHandle(g_hStopEvent);
        return; // Sai da função de serviço
    }
    catch (...)
    {
        // Captura todas as outras exceções
        LogServiceEvent("An unknown non-standard exception occurred, causing the service to stop.", EVENTLOG_ERROR_TYPE);
        if (g_Server) g_Server.reset();
        ReportSvcStatus(SERVICE_STOPPED, ERROR_SERVICE_SPECIFIC_ERROR, 1);
        if (g_hStopEvent) CloseHandle(g_hStopEvent);
        return; // Sai da função de serviço
    }

    // 12. Limpeza final e relatório de parada bem-sucedida
    if (g_hStopEvent) CloseHandle(g_hStopEvent);
    ReportSvcStatus(SERVICE_STOPPED, NO_ERROR, 0);
    LogServiceEvent("Service stopped successfully.");
}


// --- Main entry point ---
int main(int argc, char* argv[]) // Standard entry point
{
    std::locale::global(std::locale(".utf-8"));  // Para Windows/MSVC
    // If run with /install argument, install the service
    if (argc > 1 && (strcmp(argv[1], "/install") == 0 || strcmp(argv[1], "-install") == 0)) {
        printf("Instrucoes para instalar o servico '%ls':\n", SERVICE_NAME);
        printf("1. Abra o Prompt de Comando como Administrador.\n");
        printf("2. Navegue ate o diretorio onde este executavel esta localizado.\n");
        // Tenta obter o caminho do executável atual
        char exePath[MAX_PATH];
        GetModuleFileNameA(NULL, exePath, MAX_PATH);
        printf("3. Execute o comando:\n");
        printf("   sc create %ls binPath= \"%s\" start= auto\n", SERVICE_NAME, exePath);
        printf("4. (Opcional) Configure dependencias ou conta de usuario via 'sc config'.\n");
        printf("5. Inicie o servico com: sc start %ls\n", SERVICE_NAME);
        return 0;
    }
    // If run with /uninstall argument, uninstall the service
    if (argc > 1 && (strcmp(argv[1], "/uninstall") == 0 || strcmp(argv[1], "-uninstall") == 0)) {
        printf("Instrucoes para desinstalar o servico '%ls':\n", SERVICE_NAME);
        printf("1. Abra o Prompt de Comando como Administrador.\n");
        printf("2. Pare o servico (se estiver rodando): sc stop %ls\n", SERVICE_NAME);
        printf("3. Exclua o servico: sc delete %ls\n", SERVICE_NAME);
        printf("Aguarde alguns segundos para que o servico seja completamente removido.\n");
        return 0;
    }
    // If run with /debug argument, run as console app
    if (argc > 1 && (strcmp(argv[1], "/debug") == 0 || strcmp(argv[1], "-debug") == 0)) {
        printf("Running in debug console mode...\n");

        // Initialize logger for console run
        if (!InitializeGlobalLogger()) {
            printf("Exiting due to logger initialization failure.\n");
            return 1;
        }

        // Create and run the server directly
        // Use temporary unique_ptr to manage scope before assigning to global g_Server
        std::unique_ptr<ServerUDT> consoleServerInstance;
        try {
            consoleServerInstance = std::make_unique<ServerUDT>(); // Create ServerUDT instance

            // Setup console control handler to catch Ctrl+C
            SetConsoleCtrlHandler([](DWORD ctrlType) -> BOOL {
                switch (ctrlType) {
                case CTRL_C_EVENT:
                case CTRL_BREAK_EVENT:
                case CTRL_CLOSE_EVENT:
                    printf("\nCtrl+C detected. Signaling server shutdown...\n");
                    Logger::getInstance().info("main-debug", "Ctrl+C detected. Signaling server shutdown...");
                    if (g_Server) { // Use the global pointer for handler access
                        g_Server->signalShutdown();
                    }
                    // Give run() time to react to the signal before handler returns TRUE
                    Sleep(1000); // Simple wait to allow signal processing
                    return TRUE; // Indicate we handled it
                default:
                    return FALSE; // Didn't handle
                }
                }, TRUE);

            // Assign to global pointer so Ctrl+C handler can access it
            // Do this *before* calling run()
            g_Server = std::move(consoleServerInstance);

            if (g_Server->run(false)) { // Pass false for console mode (blocking)
                // run(false) should now block until g_Server->signalShutdown() is called (e.g., by Ctrl+C)
                // and then call stop() internally before returning.
                printf("Server running. Press Ctrl+C to stop...\n");
                // The blocking happens inside run(false) waiting on shutdownCV
                printf("Server run loop finished.\n"); // This line will be reached after run(false) returns
            }
            else {
                printf("Server failed to start in console mode.\n");
                Logger::getInstance().critical("main-debug", "Server failed to start in console mode.");
                g_Server.reset(); // Clean up
                return 1;
            }
        }
        catch (const std::exception& e) {
            printf("FATAL ERROR during console run: %s\n", e.what());
            Logger::getInstance().critical("main-debug", std::string("FATAL ERROR during console run: ") + e.what());            
            g_Server.reset(); // Clean up if possible
            return 1;
        }
        catch (...) {
            printf("FATAL UNKNOWN ERROR during console run.\n");
            Logger::getInstance().critical("main-debug", "FATAL UNKNOWN ERROR during console run.");
            g_Server.reset(); // Clean up if possible
            return 1;
        }

        printf("Server shutting down or already shut down.\n");
        // Explicit stop call removed - run(false) calls it internally, and reset handles final cleanup
        g_Server.reset(); // Clean up the server instance via destructor
        Logger::getInstance().info("main-debug", "Server has shut down.");
        return 0;
    }


    // --- Default: Run as a service ---
    SERVICE_TABLE_ENTRYW DispatchTable[] =
    {
        { (LPWSTR)SERVICE_NAME, (LPSERVICE_MAIN_FUNCTIONW)ServiceMain },
        { NULL, NULL }
    };

    // This call blocks until the service has stopped.
    if (!StartServiceCtrlDispatcherW(DispatchTable))
    {
        // GetLastError() will often be ERROR_FAILED_SERVICE_CONTROLLER_CONNECT (1063)
        // if the program is run from a console that isn't the SCM.
        DWORD dwError = GetLastError();
        char errorMsgBuf[256];
        snprintf(errorMsgBuf, sizeof(errorMsgBuf), "StartServiceCtrlDispatcher failed with error %lu. Running as console app?\n", dwError);
        OutputDebugStringA(errorMsgBuf);

        // Print usage instructions if run from console without valid arguments
        printf("StartServiceCtrlDispatcher failed. This application might need to be run as a service.\n");
        printf("Use '/install' or '/uninstall' to manage the service.\n");
        printf("Use '/debug' to run in console mode.\n");
    }

    return 0;
}


// --- Helper to Log to Windows Event Log ---
VOID LogServiceEvent(const std::string& message, WORD eventType)
{
    // Also log to file logger if available and initialized
    try {
        if (eventType == EVENTLOG_ERROR_TYPE) Logger::getInstance().error("ServiceEvent", message);
        else if (eventType == EVENTLOG_WARNING_TYPE) Logger::getInstance().warning("ServiceEvent", message);
        else Logger::getInstance().info("ServiceEvent", message);
    }
    catch (...) { /* Ignore if logger not ready or throws */ }

    HANDLE hEventSource = NULL;
    LPCWSTR lpszStrings[1];
    std::wstring wMessage = std::wstring(message.begin(), message.end()); // Convert to wide string

    hEventSource = RegisterEventSourceW(NULL, SERVICE_NAME); // Use service name as source

    if (NULL != hEventSource)
    {
        lpszStrings[0] = wMessage.c_str();

        ReportEventW(hEventSource,        // event log handle
            eventType,           // event type
            0,                   // event category (can define categories)
            0,                   // event identifier (use specific IDs for different events)
            NULL,                // no user security identifier
            1,                   // number of substitution strings
            0,                   // no binary data size
            lpszStrings,         // array of substitution strings
            NULL);               // no binary data

        DeregisterEventSource(hEventSource);
    }
    else {
        // Fallback logging if event source fails (e.g., source not registered)
        OutputDebugStringA(("Failed to register event source. Event log message: " + message + "\n").c_str());
    }
}
