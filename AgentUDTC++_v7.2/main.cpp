// main.cpp (AgentUDTC++ entry point)
#include <thread>
#include <memory> // For std::unique_ptr
#include <string>
#include <cstdio> // For printf
#include <cstring> // For strcmp

// --- ESSENTIAL INCLUDES ---
// Ensure these includes match the concrete classes used by this project.
// If the primary class is RadarAgent, adjust includes and g_Agent type accordingly.
#include "RadarAgent.h"     // Classe principal da sua aplicação (AJUSTE SE NECESSÁRIO)
#include "Interfaces.h"     // Para IFileIntegrityVerifier (se usado diretamente aqui)
#include "Database.h"       // Para inicialização do Banco de Dados
#include "Logger.h"         // Para logging
#include "RadarConfig.h"    // Para configurações (usado no Logger e potencialmente na inicialização)
// --- END ESSENTIAL INCLUDES ---

#include <windows.h>
#include <winsvc.h> // Para APIs de serviço
#include <iostream> // Para std::cout no modo debug

// --- Service Name ---
// Use a stable and meaningful service name.
constexpr auto SERVICE_NAME = L"RadarAgentUDTService"; // AJUSTE O NOME CONFORME NECESSÁRIO (Wide string)

// --- Global Variables for Service and Console ---
SERVICE_STATUS        g_ServiceStatus = { 0 };
SERVICE_STATUS_HANDLE g_ServiceStatusHandle = NULL;
HANDLE                g_hStopEvent = NULL; // Evento para sinalizar parada (usado em ambos os modos)

// --- Global Pointers for Core Components ---
// ADJUST THE TYPE HERE to match your main class (e.g., RadarAgent).
std::unique_ptr<RadarAgent> g_AgentInstance;
// If your main class depends on other global components (e.g., Verifier or DB), declare them here.
std::unique_ptr<IFileIntegrityVerifier> g_FileVerifierInstance;
// Database is a singleton, so a global pointer is not required.

// --- Runtime mode flag ---
bool g_isRunningAsService = false;

// --- Function Prototypes ---
VOID WINAPI ServiceMain(DWORD argc, LPWSTR* argv);
VOID WINAPI ServiceCtrlHandler(DWORD CtrlCode);
VOID ReportSvcStatus(DWORD dwCurrentState, DWORD dwWin32ExitCode, DWORD dwWaitHint);
bool InitializeSubsystems(bool isService); // Função unificada de inicialização
void ShutdownSubsystems();                 // Função unificada de desligamento
VOID LogEvent(const std::string& message, WORD eventType = EVENTLOG_INFORMATION_TYPE, bool forceDebugOutput = false); // Helper de Log
BOOL WINAPI ConsoleHandler(DWORD CEvent); // Handler para Ctrl+C no modo console

// --- Unified Subsystem Initialization ---
bool InitializeSubsystems(bool isService) {
    g_isRunningAsService = isService; // Define o modo de execução
    try {
        // 1. Load configuration (RadarConfig singleton initializes on first call).
        LogEvent("Initializing Configuration...", EVENTLOG_INFORMATION_TYPE);
        std::cout << "Initializing Configuration..." << std::endl;
        auto& config = RadarConfig::getInstance(); // Garante que a config seja carregada
        LogEvent("Configuration loaded.", EVENTLOG_INFORMATION_TYPE);
        std::cout << "Configuration loaded." << std::endl;
        if (isService) ReportSvcStatus(SERVICE_START_PENDING, NO_ERROR, 1000);

        // 2. Initialize logger (must run after configuration).
        // If logger initialization fails, startup fails critically.
        try {
            // Use values loaded from configuration.
            Logger::Initialize(config.getLogFilePath(),
                static_cast<std::uintmax_t>(config.getLogMaxSizeMB()) * 1024 * 1024,
                config.getLogBackupCount(), config.getLogFlushLevel());
            Logger::getInstance().info("Initialize", "Logger initialized successfully.");

            // 0. Initialize Windows socket API (Winsock).
            WSADATA wsaData;
            int iResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
            if (iResult != 0) {
                // Logger is not ready yet, so use direct output.
                std::string errMsg = "FATAL: WSAStartup failed with error: " + std::to_string(iResult);
                LogEvent(errMsg, EVENTLOG_ERROR_TYPE, true);
                fprintf(stderr, "%s\n", errMsg.c_str());
                return false;
            }

            LogEvent("Initializing UDT...", EVENTLOG_INFORMATION_TYPE);
            Logger::getInstance().info("Initialize", "Initializing UDT...");
            if (UDT::startup() != 0) {
                std::string errMsg = "FATAL: UDT::startup() failed: " + std::string(UDT::getlasterror().getErrorMessage());
                LogEvent(errMsg, EVENTLOG_ERROR_TYPE, true);
                Logger::getInstance().critical("Initialize", errMsg);
                return false;
            }
            Logger::getInstance().info("Initialize", "UDT initialized.");
        }
        catch (const std::exception& logEx) {
            std::string errMsg = "FATAL: Logger Initialization Failed: " + std::string(logEx.what());
            LogEvent(errMsg, EVENTLOG_ERROR_TYPE, true); // Tenta logar no EventLog e DebugOutput
            // Logger cannot be used here because initialization failed.
            // Console/debug output as a last resort.
            OutputDebugStringA(errMsg.c_str());
            fprintf(stderr, "%s\n", errMsg.c_str());
            return false; // Falha crítica
        }
        catch (...) {
            std::string errMsg = "FATAL: Unknown error during Logger Initialization.";
            LogEvent(errMsg, EVENTLOG_ERROR_TYPE, true);
            OutputDebugStringA(errMsg.c_str());
            fprintf(stderr, "%s\n", errMsg.c_str());
            return false; // Falha crítica
        }

        if (isService) ReportSvcStatus(SERVICE_START_PENDING, NO_ERROR, 2000);

        // 3. Initialize database (Database is singleton).
        LogEvent("Initializing Database...", EVENTLOG_INFORMATION_TYPE);
        Logger::getInstance().info("Initialize", "Initializing Database...");
        Database::initialize(config.getDBFilePath()); // Usa o caminho da config
        Logger::getInstance().info("Initialize", "Database initialized.");
        if (isService) ReportSvcStatus(SERVICE_START_PENDING, NO_ERROR, 3000);

        // 4. Initialize file integrity verifier.
        LogEvent("Initializing File Verifier...", EVENTLOG_INFORMATION_TYPE);
        Logger::getInstance().info("Initialize", "Initializing File Verifier...");
        g_FileVerifierInstance = std::make_unique<SHA256FileVerifier>(); // Ou qualquer implementação que você use
        Logger::getInstance().info("Initialize", "File Verifier initialized.");
        if (isService) ReportSvcStatus(SERVICE_START_PENDING, NO_ERROR, 4000);

        // 5. Create main agent instance.
        // Pass required dependencies (Database, Verifier).
        LogEvent("Creating Agent instance...", EVENTLOG_INFORMATION_TYPE);
        Logger::getInstance().info("Initialize", "Creating Agent instance...");
        // Ensure RadarAgent constructor accepts Database& and IFileIntegrityVerifier&.
        g_AgentInstance = std::make_unique<RadarAgent>(Database::getInstance(), *g_FileVerifierInstance);
        Logger::getInstance().info("Initialize", "Agent instance created.");
        if (isService) ReportSvcStatus(SERVICE_START_PENDING, NO_ERROR, 5000);

        return true;
    }
    catch (const std::exception& e) {
        std::string errMsg = "FATAL: Subsystem initialization failed: " + std::string(e.what());
        LogEvent(errMsg, EVENTLOG_ERROR_TYPE, true);
        // Logger may or may not be available here.
        try { Logger::getInstance().critical("Initialize", errMsg); }
        catch (...) {}
        fprintf(stderr, "%s\n", errMsg.c_str()); // Tenta console
        return false;
    }
    catch (...) {
        std::string errMsg = "FATAL: Unknown exception during subsystem initialization.";
        LogEvent(errMsg, EVENTLOG_ERROR_TYPE, true);
        try { Logger::getInstance().critical("Initialize", errMsg); }
        catch (...) {}
        fprintf(stderr, "%s\n", errMsg.c_str());
        return false;
    }
}

// --- Unified Subsystem Shutdown ---
void ShutdownSubsystems() {
    LogEvent("Shutting down subsystems...", EVENTLOG_INFORMATION_TYPE);
    Logger::safeInfo("Shutdown", "Shutting down subsystems...");


    // 1. Stop agent (if created and running).
    if (g_AgentInstance) {
        try {
            Logger::safeInfo("Shutdown", "Stopping agent instance...");
            g_AgentInstance->stop(); // Chama o método stop do seu agente
            Logger::safeInfo("Shutdown", "Agent instance stopped.");
        }
        catch (const std::exception& e) {
            LogEvent("Exception during agent stop: " + std::string(e.what()), EVENTLOG_WARNING_TYPE);
            Logger::safeError("Shutdown", "Exception during agent stop: " + std::string(e.what()));
        }
        catch (...) {
            LogEvent("Unknown exception during agent stop.", EVENTLOG_WARNING_TYPE);
            Logger::safeError("Shutdown", "Unknown exception during agent stop.");
        }
    }

    try {
        Logger::safeInfo("Shutdown", "Executando UDT::cleanup()...");
        int cleanup_ret = UDT::cleanup();
        if (cleanup_ret != 0) {
             // UDT::getlasterror_desc() may provide additional detail.
            std::string errMsg = UDT::getlasterror_desc();
            Logger::safeError("Shutdown","UDT::cleanup() retornou erro: " + std::to_string(cleanup_ret));
        } else {
            Logger::safeInfo("Shutdown", "UDT::cleanup() concluído com sucesso.");
        }

        // Cleanup Windows socket API (Winsock).
        Logger::safeInfo("Shutdown", "Executing WSACleanup()...");
        WSACleanup();

    } catch (const std::exception& e) {
        // Catch unexpected exceptions from UDT::cleanup.
         Logger::safeError("Shutdown", "Exceção durante UDT::cleanup(): " + std::string(e.what()));
    } catch (...) {
        // Catch non-standard exceptions.
         Logger::safeError("Shutdown", "Exceção desconhecida durante UDT::cleanup()");
    }

    // 2. Release pointers in reverse creation order (safe with unique_ptr).
    Logger::safeInfo("Shutdown", "Releasing resources...");
    g_AgentInstance.reset();
    g_FileVerifierInstance.reset();
    // Database is singleton; explicit reset is not required here.
    // Logger is singleton and will terminate with the process.

    // 3. Close stop event handle.
    if (g_hStopEvent != NULL && g_hStopEvent != INVALID_HANDLE_VALUE) {
        CloseHandle(g_hStopEvent);
        g_hStopEvent = NULL;
    }

    Logger::safeInfo("Shutdown", "Subsystems shut down complete.");
    LogEvent("Subsystems shut down complete.", EVENTLOG_INFORMATION_TYPE);
}

// --- Program Entry Point ---
int main(int argc, char* argv[]) {
    std::locale::global(std::locale(".utf-8"));  // Para Windows/MSVC
    // --- Argument Handling ---
    bool runAsDebug = false;
    if (argc > 1) {
        if (strcmp(argv[1], "/install") == 0 || strcmp(argv[1], "-install") == 0) {
            printf("Instrucoes para instalar o servico '%ls':\n", SERVICE_NAME);
            printf("1. Abra o Prompt de Comando como Administrador.\n");
            printf("2. Navegue ate o diretorio onde este executavel esta localizado.\n");
            // Try to get current executable path.
            char exePath[MAX_PATH];
            GetModuleFileNameA(NULL, exePath, MAX_PATH);
            printf("3. Execute o comando:\n");
            printf("   sc create %ls binPath= \"%s\" start= auto\n", SERVICE_NAME, exePath);
            printf("4. (Opcional) Configure dependencias ou conta de usuario via 'sc config'.\n");
            printf("5. Inicie o servico com: sc start %ls\n", SERVICE_NAME);
            return 0;
        }
        if (strcmp(argv[1], "/uninstall") == 0 || strcmp(argv[1], "-uninstall") == 0) {
            printf("Instrucoes para desinstalar o servico '%ls':\n", SERVICE_NAME);
            printf("1. Abra o Prompt de Comando como Administrador.\n");
            printf("2. Pare o servico (se estiver rodando): sc stop %ls\n", SERVICE_NAME);
            printf("3. Exclua o servico: sc delete %ls\n", SERVICE_NAME);
            printf("Aguarde alguns segundos para que o servico seja completamente removido.\n");
            return 0;
        }
        if (strcmp(argv[1], "/debug") == 0 || strcmp(argv[1], "-debug") == 0) {
            runAsDebug = true;
        }
    }

    // --- Execution ---
    if (runAsDebug) {
        // --- CONSOLE MODE ---
        std::cout << "Running in debug console mode..." << std::endl;
        SetConsoleTitleA("RadarAgent UDT Debug Console"); // Define título do console

        // Initialize subsystems (Logger, DB, etc.).
        if (!InitializeSubsystems(false)) { // false = não é serviço
            std::cerr << "Failed to initialize subsystems. Exiting." << std::endl;
            ShutdownSubsystems();
            return 1;
        }

        // Create stop event used by ConsoleHandler.
        g_hStopEvent = CreateEvent(NULL, TRUE, FALSE, NULL); // Manual reset, non-signaled
        if (g_hStopEvent == NULL) {
            Logger::getInstance().critical("main-debug", "Failed to create stop event.", GetLastError());
            std::cerr << "Failed to create stop event. Error: " << GetLastError() << std::endl;
            ShutdownSubsystems(); // Tenta limpar o que foi inicializado
            return 1;
        }

        // Configure Ctrl+C handler.
        if (!SetConsoleCtrlHandler((PHANDLER_ROUTINE)ConsoleHandler, TRUE)) {
            Logger::getInstance().error("main-debug", "Could not set console control handler.", GetLastError());
            std::cerr << "WARNING: Could not set console control handler. Ctrl+C might not shut down gracefully." << std::endl;
            // Continue anyway, but notify user.
        }
        else {
            Logger::getInstance().info("main-debug", "Console Ctrl+C handler registered.");
        }

        // Start the agent (main runtime logic).
        try {
            Logger::getInstance().info("main-debug", "Starting agent in console mode...");
            g_AgentInstance->start(); // Chama o método start do seu agente
            Logger::getInstance().info("main-debug", "Agent started. Press Ctrl+C to stop.");
            std::cout << "Agent started. Press Ctrl+C to stop." << std::endl;

            // Keep process alive until stop event is signaled.
            WaitForSingleObject(g_hStopEvent, INFINITE);

            Logger::getInstance().info("main-debug", "Stop signal received. Shutting down...");
            std::cout << "\nStop signal received. Shutting down..." << std::endl;
        }
        catch (const std::exception& e) {
            Logger::getInstance().critical("main-debug", "FATAL ERROR during agent execution: " + std::string(e.what()));
            std::cerr << "FATAL ERROR during agent execution: " << e.what() << std::endl;
            // Signal stop event to guarantee shutdown path runs.
            if (g_hStopEvent) SetEvent(g_hStopEvent);
            // Flow continues to ShutdownSubsystems.
        }
        catch (...) {
            Logger::getInstance().critical("main-debug", "FATAL UNKNOWN ERROR during agent execution.");
            std::cerr << "FATAL UNKNOWN ERROR during agent execution." << std::endl;
            if (g_hStopEvent) SetEvent(g_hStopEvent);
            // Flow continues to ShutdownSubsystems.
        }

        // Shutdown subsystems.
        ShutdownSubsystems();
        std::cout << "Application finished." << std::endl;
        return 0;
    }
    else {
        // --- SERVICE MODE ---
        // Try starting service dispatcher.
        // If this fails, process was likely not started by SCM.
        SERVICE_TABLE_ENTRYW DispatchTable[] =
        {
            { (LPWSTR)SERVICE_NAME, (LPSERVICE_MAIN_FUNCTIONW)ServiceMain },
            { NULL, NULL }
        };

        // This call blocks until service stop.
        if (!StartServiceCtrlDispatcherW(DispatchTable))
        {
            DWORD dwError = GetLastError();
            char errorMsgBuf[256];
            snprintf(errorMsgBuf, sizeof(errorMsgBuf), "StartServiceCtrlDispatcher failed with error %lu.", dwError);
            LogEvent(errorMsgBuf, EVENTLOG_ERROR_TYPE, true); // Tenta logar erro

            // Error 1063 usually means process was not started by SCM.
            if (dwError == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) {
                printf("ERRO: Aplicacao nao iniciada como servico.\n");
                printf("Para executar em modo console, use o argumento /debug.\n");
                printf("Para gerenciar o servico, use /install ou /uninstall.\n");
            }
            else {
                printf("Erro ao iniciar o dispatcher do servico: %lu\n", dwError);
            }
            return 1; // Falha ao iniciar como serviço
        }
        // Program exits here after service stops.
        return 0;
    }
}

// --- Service Main Function ---
VOID WINAPI ServiceMain(DWORD argc, LPWSTR* argv) {
    g_isRunningAsService = true; // Confirma que estamos no modo serviço

    // 1. Register service control handler.
    g_ServiceStatusHandle = RegisterServiceCtrlHandlerW(SERVICE_NAME, ServiceCtrlHandler);
    if (g_ServiceStatusHandle == NULL) {
        LogEvent("RegisterServiceCtrlHandler failed.", EVENTLOG_ERROR_TYPE, true);
        return; // Não pode continuar sem um handle
    }

    // 2. Set initial service status.
    g_ServiceStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_ServiceStatus.dwServiceSpecificExitCode = 0;
    ReportSvcStatus(SERVICE_START_PENDING, NO_ERROR, 3000); // 3 segundos hint inicial

    // 3. Create stop event.
    g_hStopEvent = CreateEvent(NULL, TRUE, FALSE, NULL); // Manual reset, non-signaled
    if (g_hStopEvent == NULL) {
        LogEvent("CreateEvent for stop handle failed.", EVENTLOG_ERROR_TYPE);
        ReportSvcStatus(SERVICE_STOPPED, GetLastError(), 0);
        return;
    }
    ReportSvcStatus(SERVICE_START_PENDING, NO_ERROR, 1000); // +1 segundo hint

    // 4. Initialize subsystems (Logger, DB, Agent instance, etc.).
    LogEvent("Initializing subsystems in service mode...", EVENTLOG_INFORMATION_TYPE);
    if (!InitializeSubsystems(true)) { // true = é serviço
        LogEvent("Subsystem initialization failed. Stopping service.", EVENTLOG_ERROR_TYPE);
        ReportSvcStatus(SERVICE_STOPPED, ERROR_SERVICE_SPECIFIC_ERROR, 0);
        ShutdownSubsystems(); // Tenta limpar
        return;
    }
    LogEvent("Subsystems initialized successfully.", EVENTLOG_INFORMATION_TYPE);
    ReportSvcStatus(SERVICE_START_PENDING, NO_ERROR, 2000); // +2 segundos hint

    // 5. Start agent logic on a dedicated thread.
    std::thread agentThread;
    bool threadStarted = false;
    try {
        agentThread = std::thread([&]() {
            try {
                LogEvent("Agent thread started. Calling Agent::start()...", EVENTLOG_INFORMATION_TYPE);
                Logger::getInstance().info("Service", "Agent thread started. Calling Agent::start()...");

                // start() now returns immediately after bootstrapping components.
                g_AgentInstance->start();

                // Agent thread waits for stop signal internally.
                // This keeps worker logic alive until service is interrupted.
                LogEvent("Agent components started. Agent thread is now waiting for stop signal.", EVENTLOG_INFORMATION_TYPE);
                Logger::getInstance().info("ServiceThread", "Agent components started. Waiting for stop signal...");
                WaitForSingleObject(g_hStopEvent, INFINITE);

                // When event is signaled, wait ends and thread exits cleanly.
                LogEvent("Agent thread stop signal received. Exiting thread.", EVENTLOG_INFORMATION_TYPE);
                Logger::getInstance().info("ServiceThread", "Stop signal received, exiting agent thread.");
            }
            catch (const std::exception& ex) {
                std::string errMsg = "Exception in agent thread: " + std::string(ex.what());
                LogEvent(errMsg, EVENTLOG_ERROR_TYPE);
                Logger::getInstance().critical("ServiceThread", errMsg);
                if (g_hStopEvent != NULL) SetEvent(g_hStopEvent); // Sinaliza falha
            }
            catch (...) {
                std::string errMsg = "Unknown exception in agent thread.";
                LogEvent(errMsg, EVENTLOG_ERROR_TYPE);
                Logger::getInstance().critical("ServiceThread", errMsg);
                if (g_hStopEvent != NULL) SetEvent(g_hStopEvent); // Sinaliza falha
            }
            LogEvent("Agent thread exiting.", EVENTLOG_INFORMATION_TYPE);
            Logger::getInstance().info("ServiceThread", "Agent thread exiting.");
            });
        threadStarted = true;
        LogEvent("Agent thread launched.", EVENTLOG_INFORMATION_TYPE);
        Logger::getInstance().info("Service", "Agent thread launched.");

    }
    catch (const std::system_error& e) {
        std::string errMsg = "Failed to start agent thread: " + std::string(e.what());
        LogEvent(errMsg, EVENTLOG_ERROR_TYPE);
        Logger::getInstance().critical("Service", errMsg);
        ReportSvcStatus(SERVICE_STOPPED, ERROR_SERVICE_SPECIFIC_ERROR, 0);
        ShutdownSubsystems(); // Tenta limpar
        return;
    }
    catch (...) {
        std::string errMsg = "Unknown error starting agent thread.";
        LogEvent(errMsg, EVENTLOG_ERROR_TYPE);
        Logger::getInstance().critical("Service", errMsg);
        ReportSvcStatus(SERVICE_STOPPED, ERROR_SERVICE_SPECIFIC_ERROR, 0);
        ShutdownSubsystems(); // Tenta limpar
        return;
    }

    // 6. Report service as running.
    ReportSvcStatus(SERVICE_RUNNING, NO_ERROR, 0);
    LogEvent("Service is running.", EVENTLOG_INFORMATION_TYPE);
    Logger::getInstance().info("Service", "Service reported as RUNNING.");

    // 7. Wait for stop signal.
    WaitForSingleObject(g_hStopEvent, INFINITE);
    LogEvent("Stop event received.", EVENTLOG_INFORMATION_TYPE);
    Logger::getInstance().info("Service", "Stop event received.");

    // 8. Begin shutdown.
    ReportSvcStatus(SERVICE_STOP_PENDING, NO_ERROR, 10000); // 10 segundos para desligar

    // Signal agent shutdown if an explicit non-blocking method exists.
    // g_AgentInstance->stop() should already be called by thread logic or
    // by ShutdownSubsystems. If stop() blocks, call signalShutdown() first.
    // Example: if (g_AgentInstance) g_AgentInstance->signalShutdown();
    // Exemplo: if (g_AgentInstance) g_AgentInstance->signalShutdown();

    // Wait for agent thread to finish.
    LogEvent("Waiting for agent thread to join...", EVENTLOG_INFORMATION_TYPE);
    Logger::getInstance().info("Service", "Waiting for agent thread to join...");
    if (threadStarted && agentThread.joinable()) {
        agentThread.join();
        LogEvent("Agent thread joined.", EVENTLOG_INFORMATION_TYPE);
        Logger::getInstance().info("Service", "Agent thread joined.");
    }
    else {
        LogEvent("Agent thread was not joinable or not started.", EVENTLOG_WARNING_TYPE);
        Logger::getInstance().warning("Service", "Agent thread was not joinable or not started.");
    }
    ReportSvcStatus(SERVICE_STOP_PENDING, NO_ERROR, 2000); // +2 segundos

    // 9. Shutdown subsystems and cleanup.
    ShutdownSubsystems(); // Chama a função unificada de limpeza

    // 10. Report service as stopped.
    ReportSvcStatus(SERVICE_STOPPED, NO_ERROR, 0);
    // LogEvent("Service stopped.", EVENTLOG_INFORMATION_TYPE); // Already logged in ShutdownSubsystems.
    // Logger::getInstance().info("Service", "Service reported as STOPPED."); // Same as above.
}

// --- Service Control Handler ---
VOID WINAPI ServiceCtrlHandler(DWORD CtrlCode) {
    switch (CtrlCode) {
    case SERVICE_CONTROL_STOP:
        LogEvent("SERVICE_CONTROL_STOP received.", EVENTLOG_INFORMATION_TYPE);
        Logger::safeInfo("ServiceCtrl", "SERVICE_CONTROL_STOP received.");
        // Notify SCM we are stopping and signal local stop event.
        ReportSvcStatus(SERVICE_STOP_PENDING, NO_ERROR, 5000); // 5 segundos hint inicial
        if (g_hStopEvent != NULL) {
            SetEvent(g_hStopEvent);
        }
        break;

    case SERVICE_CONTROL_INTERROGATE:
        LogEvent("SERVICE_CONTROL_INTERROGATE received.", EVENTLOG_INFORMATION_TYPE);
        Logger::safeDebug("ServiceCtrl", "SERVICE_CONTROL_INTERROGATE received.");
        // Only report current status (done below).
        break;

        // Other controls (SHUTDOWN, PAUSE, CONTINUE) can be handled here if needed.
    case SERVICE_CONTROL_SHUTDOWN:
        LogEvent("SERVICE_CONTROL_SHUTDOWN received.", EVENTLOG_INFORMATION_TYPE);
        Logger::safeInfo("ServiceCtrl", "SERVICE_CONTROL_SHUTDOWN received.");
        // Treat as STOP.
        ReportSvcStatus(SERVICE_STOP_PENDING, NO_ERROR, 15000); // Mais tempo para shutdown
        if (g_hStopEvent != NULL) {
            SetEvent(g_hStopEvent);
        }
        break;

    default:
        Logger::safeWarning("ServiceCtrl", "Received unhandled control code: " + std::to_string(CtrlCode));
        break;
    }

    // Ensure status is reported for INTERROGATE and unknown control codes.
    // Report last known state unless STOP_PENDING is active.
    if (g_ServiceStatusHandle != NULL && g_ServiceStatus.dwCurrentState != SERVICE_STOP_PENDING) {
        ReportSvcStatus(g_ServiceStatus.dwCurrentState, NO_ERROR, 0);
    }
}

// --- Console Handler (for /debug mode) ---
BOOL WINAPI ConsoleHandler(DWORD CEvent) {
    switch (CEvent) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        Logger::getInstance().info("ConsoleHandler", "Console event received: " + std::to_string(CEvent) + ". Signaling stop.");
        std::cout << "\nConsole event received (" << CEvent << "). Signaling stop..." << std::endl;
        // Signal stop event awaited by debug mode main loop.
        if (g_hStopEvent != NULL) {
            SetEvent(g_hStopEvent);
        }
        // Return TRUE to indicate this event was handled.
        // Prevents default/system handlers from processing it again.
        // A short delay before returning can help shutdown begin cleanly.
        Sleep(1000); // Pequena pausa
        return TRUE;
    default:
        break;
    }
    return FALSE; // Não lidamos com outros eventos
}


// --- Helper to Report Status to SCM ---
VOID ReportSvcStatus(DWORD dwCurrentState, DWORD dwWin32ExitCode, DWORD dwWaitHint) {
    static DWORD dwCheckPoint = 1;

    if (g_ServiceStatusHandle == NULL) {
        // LogEvent("ReportSvcStatus called with invalid handle.", EVENTLOG_WARNING_TYPE);
        // Logger may not be initialized at this stage.
        return;
    }

    // Fill SERVICE_STATUS structure.
    g_ServiceStatus.dwCurrentState = dwCurrentState;
    g_ServiceStatus.dwWin32ExitCode = dwWin32ExitCode;
    g_ServiceStatus.dwWaitHint = dwWaitHint;

    // Define which controls the service accepts.
    if (dwCurrentState == SERVICE_START_PENDING) {
        g_ServiceStatus.dwControlsAccepted = 0; // Não aceita nada enquanto inicia
    }
    else {
        // Accept STOP and SHUTDOWN when running (and paused if implemented).
        g_ServiceStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    }

    // Increment checkpoint while in pending states.
    if ((dwCurrentState == SERVICE_RUNNING) ||
        (dwCurrentState == SERVICE_STOPPED)) {
        g_ServiceStatus.dwCheckPoint = 0; // Reseta checkpoint quando não está pendente
    }
    else {
        g_ServiceStatus.dwCheckPoint = dwCheckPoint++;
    }

    // Report status to SCM.
    if (!SetServiceStatus(g_ServiceStatusHandle, &g_ServiceStatus)) {
        DWORD dwError = GetLastError();
        // Avoid log storms while still reporting first failure.
        static bool firstSetStatusFail = true;
        if (firstSetStatusFail) {
            LogEvent("SetServiceStatus failed. Error: " + std::to_string(dwError), EVENTLOG_ERROR_TYPE);
            try { Logger::getInstance().error("ReportSvcStatus", "SetServiceStatus failed.", dwError); }
            catch (...) {}
            firstSetStatusFail = false;
        }
    }
}


// --- Helper to Log to Event Log and Debug Output ---
VOID LogEvent(const std::string& message, WORD eventType, bool forceDebugOutput) {
    // 1. Try writing to main logger if initialized.
    // Prevent infinite recursion if logger internals call LogEvent.
    thread_local bool inLogEvent = false;
    if (!inLogEvent) {
        try {
            inLogEvent = true; // Proteção simples contra recursão
            std::string tagged_message = "[ServiceLifetime] " + message;
            if (eventType == EVENTLOG_ERROR_TYPE) Logger::getInstance().error("Service", tagged_message);
            else if (eventType == EVENTLOG_WARNING_TYPE) Logger::getInstance().warning("Service", tagged_message);
            else Logger::getInstance().info("Service", tagged_message);            
            inLogEvent = false;
        }
        catch (...) {
            inLogEvent = false;
            // Ignore logger failure here and continue with fallback outputs.
        }
    }


    // 2. Write to OutputDebugString (visible in DebugView / debugger).
    std::string debugMsg = "[";
    debugMsg += (eventType == EVENTLOG_ERROR_TYPE ? "ERROR" : (eventType == EVENTLOG_WARNING_TYPE ? "WARN" : "INFO"));
    debugMsg += "] " + message + "\n";
    OutputDebugStringA(debugMsg.c_str());

    // 3. Write to Windows Event Log (only as service or when forced).
    // Avoid Event Log writes outside service mode unless startup is critically failing.
    if (g_isRunningAsService || forceDebugOutput) {
        HANDLE hEventSource = NULL;
        LPCWSTR lpszStrings[1];
        std::wstring wMessage;

        try {
            // Convert UTF-8 to wide string including null terminator safely.
            int size_needed = MultiByteToWideChar(CP_UTF8, 0, message.c_str(), -1, nullptr, 0);
            if (size_needed > 0) {
                std::wstring converted(static_cast<size_t>(size_needed), L'\0');
                int convertedCount = MultiByteToWideChar(CP_UTF8, 0, message.c_str(), -1, converted.data(), size_needed);
                if (convertedCount > 0) {
                    converted.resize(static_cast<size_t>(convertedCount - 1));
                    wMessage = std::move(converted);
                }
                else {
                    wMessage = L"Error converting message to WCHAR for Event Log.";
                }
            }
            else {
                wMessage = L"Error converting message to WCHAR for Event Log.";
            }
        }
        catch (...) {
            // Fallback when conversion fails.
            wMessage = L"Error converting message to WCHAR for Event Log.";
        }


        hEventSource = RegisterEventSourceW(NULL, SERVICE_NAME); // Usa o nome do serviço como source

        if (NULL != hEventSource) {
            lpszStrings[0] = wMessage.c_str();

            ReportEventW(hEventSource,  // event log handle
                eventType,              // event type
                0,                      // event category (0 for default)
                0,                      // event identifier (poderia usar códigos específicos)
                NULL,                   // no user security identifier
                1,                      // number of substitution strings
                0,                      // no binary data size
                lpszStrings,            // array of substitution strings
                NULL);                  // no binary data

            DeregisterEventSource(hEventSource);
        }
        else {
            // Fallback when source registration is unavailable.
            std::string serviceNameUtf8 = "RadarAgentUDTService";
            int serviceNameSize = WideCharToMultiByte(CP_UTF8, 0, SERVICE_NAME, -1, nullptr, 0, nullptr, nullptr);
            if (serviceNameSize > 0) {
                std::string converted(static_cast<size_t>(serviceNameSize), '\0');
                int convertedCount = WideCharToMultiByte(CP_UTF8, 0, SERVICE_NAME, -1, converted.data(), serviceNameSize, nullptr, nullptr);
                if (convertedCount > 0) {
                    converted.resize(static_cast<size_t>(convertedCount - 1));
                    serviceNameUtf8 = converted;
                }
            }
            OutputDebugStringA(("Failed to register event source '" + serviceNameUtf8 + "'. Event log message: " + message + "\n").c_str());
        }
    }
}






