#include "FileWatcher.h"
#include <algorithm>
#include <chrono>
#include <cwctype>
#include <stdexcept> // Para std::runtime_error
#include <vector>
#include <stringapiset.h> // Para conversões de string
#include "RadarConfig.h"

// Helper function to check excluded files
static bool isFileExcluded(const fs::path& filePath) {
    const auto& excludedFiles = RadarConfig::getInstance().getWatcherExcludeFiles();
    std::string filename = filePath.filename().string();
    for (const auto& excluded : excludedFiles) {
        if (filename == excluded) {
            return true;
        }
    }
    return false;
}

#ifdef _WIN32
static std::wstring normalizePathElement(const fs::path& path) {
    std::wstring value = path.wstring();
    std::transform(value.begin(), value.end(), value.begin(),
        [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    return value;
}
#else
static std::string normalizePathElement(const fs::path& path) {
    return path.string();
}
#endif

static bool samePathElement(const fs::path& left, const fs::path& right) {
    return normalizePathElement(left) == normalizePathElement(right);
}

static bool pathStartsWith(const fs::path& root, const fs::path& candidate) {
    auto rootIt = root.begin();
    auto candidateIt = candidate.begin();
    for (; rootIt != root.end(); ++rootIt, ++candidateIt) {
        if (candidateIt == candidate.end() ||
            !samePathElement(*rootIt, *candidateIt)) {
            return false;
        }
    }
    return true;
}

static fs::path absoluteNormalPath(const fs::path& path) {
    std::error_code ec;
    fs::path absolute = fs::absolute(path, ec);
    if (ec) {
        absolute = path;
    }
    return absolute.lexically_normal();
}

static fs::path weaklyCanonicalPath(const fs::path& path, std::error_code& ec) {
    fs::path canonical = fs::weakly_canonical(path, ec);
    if (ec) {
        return {};
    }
    return canonical.lexically_normal();
}

static bool isReparsePointOrUnknown(const fs::path& path) {
#ifdef _WIN32
    const DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        return true;
    }
    return (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
    std::error_code ec;
    const auto status = fs::symlink_status(path, ec);
    return ec || fs::is_symlink(status);
#endif
}

static bool hasReparsePointInPath(const fs::path& root,
                                  const fs::path& candidate) {
    const fs::path rootAbs = absoluteNormalPath(root);
    const fs::path candidateAbs = absoluteNormalPath(candidate);
    if (!pathStartsWith(rootAbs, candidateAbs)) {
        return true;
    }

    fs::path current = rootAbs;
    if (isReparsePointOrUnknown(current)) {
        return true;
    }

    auto candidateIt = candidateAbs.begin();
    for (auto rootIt = rootAbs.begin(); rootIt != rootAbs.end(); ++rootIt) {
        if (candidateIt != candidateAbs.end()) {
            ++candidateIt;
        }
    }

    for (; candidateIt != candidateAbs.end(); ++candidateIt) {
        current /= *candidateIt;
        if (isReparsePointOrUnknown(current)) {
            return true;
        }
    }
    return false;
}

static bool isSafeWatchedRegularFile(const fs::path& watchRoot,
                                     const fs::path& candidate) {
    std::error_code ec;
    const fs::path canonicalRoot = weaklyCanonicalPath(watchRoot, ec);
    if (ec) {
        Logger::getInstance().warning("FileWatcher",
            "Unable to canonicalize watched directory. Rejecting path.");
        return false;
    }

    const fs::path canonicalCandidate = weaklyCanonicalPath(candidate, ec);
    if (ec || !pathStartsWith(canonicalRoot, canonicalCandidate) ||
        samePathElement(canonicalRoot, canonicalCandidate)) {
        Logger::getInstance().warning("FileWatcher",
            "Rejected file outside watched directory.");
        return false;
    }

    if (hasReparsePointInPath(watchRoot, candidate)) {
        Logger::getInstance().warning("FileWatcher",
            "Rejected symlink or reparse-point path.");
        return false;
    }

    const auto status = fs::symlink_status(candidate, ec);
    return !ec && fs::is_regular_file(status);
}

// Constructor and destructor
FileWatcher::FileWatcher(const fs::path& directoryPath, int checkIntervalSeconds)
    : directoryPath(directoryPath),
    checkIntervalSeconds(checkIntervalSeconds), // Mantido
    running(false),
    buffer(BUFFER_SIZE) // Inicializa o buffer
{
}
/**
 * Destructor - ensures watcher thread is stopped.
 *
 * @return None
 *
 * @throws None
 */
FileWatcher::~FileWatcher()
{
    stopWatching();
}

/**
 * Starts the file watcher, monitoring the specified directory for new files.
 * If the directory does not exist, an error is logged and a runtime_error is thrown.
 * If the watcher is already running, a warning is logged and the function returns without starting a new watcher.
 *
 * @param fileCallback Function to be called when a new file is detected.
 *
 * @throws std::runtime_error If the watched directory does not exist, or if there is an error opening the directory handle, creating the overlapped event, or starting the watcher thread.
 */
void FileWatcher::startWatching(std::function<void(const fs::path&)> fileCallback)
{
    std::string directoryPathStr = directoryPath.u8string();
    // Check if directory exists
    if (!fs::exists(directoryPath)) {
        Logger::getInstance().error("FileWatcher::startWatching", "Directory for watching files does not exist: " + directoryPathStr);
        throw std::runtime_error("Watched directory does not exist: " + directoryPathStr); // Lança exceção
    }

    // If already running
    if (running) {
        Logger::getInstance().warning("FileWatcher::startWatching", "Watcher already running.");
        return;
    }

    // --- Acquire directory handle ---
    hDir = CreateFileW(
        directoryPath.c_str(),
        FILE_LIST_DIRECTORY,            // Direito de acesso necessário
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, // Compartilhamento
        NULL,                           // Atributos de segurança
        OPEN_EXISTING,                  // Abrir diretório existente
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, // Flags (OVERLAPPED é crucial)
        NULL                            // Handle de template (não necessário)
    );

    if (hDir == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        Logger::getInstance().error("FileWatcher::startWatching", "Failed to open directory handle. Error code: " + std::to_string(error));
        throw std::runtime_error("Failed to get directory handle for watching.");
    }
    // --- End handle acquisition ---

    // --- Initialize OVERLAPPED state ---
    overlapped.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL); // Evento de reset manual
    if (overlapped.hEvent == NULL) {
        DWORD error = GetLastError();
        CloseHandle(hDir);
        hDir = INVALID_HANDLE_VALUE;
        Logger::getInstance().error("FileWatcher::startWatching", "Failed to create overlapped event. Error code: " + std::to_string(error));
        throw std::runtime_error("Failed to create overlapped event.");
    }
    // --- End OVERLAPPED initialization ---


    // Set running flag
    running = true;

    // Start the watcher thread
    try {
        // Start the first read before launching the thread so handles are ready.
        if (!startReadDirectoryChanges()) {
            // If startup read fails, clean up and throw.
            running = false;
            CloseHandle(overlapped.hEvent);
            CloseHandle(hDir);
            hDir = INVALID_HANDLE_VALUE;
            overlapped.hEvent = NULL;
			Logger::getInstance().error("FileWatcher::startWatching", "Failed to initiate directory changes monitoring.");
            throw std::runtime_error("Failed to initiate directory changes monitoring.");
        }
        watcherThread = std::thread(&FileWatcher::watcherThreadFunc, this, fileCallback);
    }
    catch (const std::system_error& e) {
        running = false;
        CloseHandle(overlapped.hEvent);
        CloseHandle(hDir);
        hDir = INVALID_HANDLE_VALUE;
        overlapped.hEvent = NULL;
        Logger::getInstance().error("FileWatcher::startWatching", "Failed to start watcher thread: " + std::string(e.what()));
        throw; // Re-lança a exceção
    }


    Logger::getInstance().info("FileWatcher::startWatching", "File watcher started using ReadDirectoryChangesW for directory: " + directoryPathStr);
}

// Enumerates current regular files so startup state is processed once.
void FileWatcher::processExistingFiles(const std::function<void(const fs::path&)>& fileCallback) {
    std::string directoryPathStr = directoryPath.u8string();
    Logger::getInstance().info("FileWatcher::processExistingFiles", "Starting initial scan for existing files in: " + directoryPathStr);
    try {
        // Iterate through every entry in the directory.
        for (const auto& entry : fs::directory_iterator(directoryPath)) {
            if (!running) break; // Permite parar a varredura se o serviço for interrompido


            // Process only regular files inside the watched tree.
            if (isSafeWatchedRegularFile(directoryPath, entry.path())) {
                std::string filePathStr = entry.path().u8string();

                if (isFileExcluded(entry.path())) {
                    continue; // Pula o arquivo se estiver na lista de exclusão
                }

                Logger::getInstance().info("FileWatcher::processExistingFiles", "Found existing file to process: " + filePathStr);

                try {
                    fileCallback(entry.path()); // Chama a função principal de manipulação de arquivos
                }
                catch (const std::exception& e) {
                    Logger::getInstance().error("FileWatcher::processExistingFiles", "Exception in file callback for existing file " + filePathStr + ": " + e.what());
                }
                catch (...) {
                    Logger::getInstance().error("FileWatcher::processExistingFiles", "Unknown exception in file callback for existing file " + filePathStr);
                }                
            }
        }
    }
    catch (const fs::filesystem_error& e) {
        Logger::getInstance().error("FileWatcher::processExistingFiles", "Filesystem error during initial scan: " + std::string(e.what()));
        // Log file-system access failures (permissions, etc.).
    }
    catch (const std::exception& e) {
        Logger::getInstance().error("FileWatcher::processExistingFiles", "Generic error during initial scan: " + std::string(e.what()));
        // Log unexpected errors.
    }
    Logger::getInstance().info("FileWatcher::processExistingFiles", "Initial scan finished.");
}
/**
 * Starts reading directory changes using ReadDirectoryChangesW.
 * This function is called at the beginning and after processing each batch of changes.
 *
 * @return true if the operation was initiated successfully, false otherwise.
 */
bool FileWatcher::startReadDirectoryChanges() {
    if (!running || hDir == INVALID_HANDLE_VALUE) {
        return false;
    }

    BOOL success = ReadDirectoryChangesW(
        hDir,
        buffer.data(),          // Buffer para receber os dados
        static_cast<DWORD>(buffer.size()), // Tamanho do buffer
        TRUE,                   // Monitorar subdiretórios? (TRUE = Sim) - ajuste conforme necessário
        notifyFilter,           // Tipos de alterações a monitorar
        NULL,                   // Bytes retornados (para I/O síncrono)
        &overlapped,            // Estrutura OVERLAPPED
        NULL                    // Completion Routine (não usada aqui)
    );

    if (!success) {
        DWORD error = GetLastError();
        if (error != ERROR_IO_PENDING) {
            Logger::getInstance().error("FileWatcher::startReadDirectoryChanges", "ReadDirectoryChangesW failed initially. Error code: " + std::to_string(error));
            // Consider stopping the watcher if this becomes fatal.
            // running = false;
            return false;
        }
    }
    // Logger::getInstance().debug("FileWatcher", "ReadDirectoryChangesW request issued.");
    return true;
}
/**
 * Stops the file watcher, cleaning up resources and ensuring the watcher thread is joined.
 * If the watcher is not running, it simply returns without doing anything.
 */
void FileWatcher::stopWatching()
{
    // Return early if watcher is not running.
    if (!running.exchange(false)) { // Atomicamente define como false e obtém o valor anterior
        return;
    }

    Logger::getInstance().info("FileWatcher::stopWatching", "Stopping file watcher...");

    // Signal the watcher thread to stop.
    // Cancel pending ReadDirectoryChangesW to wake the waiting thread.
    if (hDir != INVALID_HANDLE_VALUE) {
        CancelIoEx(hDir, &overlapped); // Cancela I/O pendente associado a este handle/overlapped
    }
    // Also signal the event to guarantee WaitForSingleObject returns.
    if (overlapped.hEvent != NULL) {
        SetEvent(overlapped.hEvent);
    }


    // Wait for the thread to finish if it's joinable
    if (watcherThread.joinable()) {
        try {
            watcherThread.join();
        }
        catch (const std::system_error& e) {
            Logger::getInstance().error("FileWatcher::stopWatching", "Error joining watcher thread: " + std::string(e.what()));
        }
    }

    // --- Release resources ---
    if (overlapped.hEvent != NULL) {
        CloseHandle(overlapped.hEvent);
        overlapped.hEvent = NULL;
    }
    if (hDir != INVALID_HANDLE_VALUE) {
        CloseHandle(hDir);
        hDir = INVALID_HANDLE_VALUE;
    }
    // --- End resource release ---

    Logger::getInstance().info("FileWatcher::stopWatching", "File watcher stopped.");
}

/**
 * The main function of the watcher thread.
 * It waits for directory change events and processes them.
 *
 * @param fileCallback Function to be called when a new file is detected.
 */
void FileWatcher::watcherThreadFunc(std::function<void(const fs::path&)> fileCallback)
{
    Logger::getInstance().info("FileWatcher::watcherThreadFunc", "Watcher thread started.");
    
    processExistingFiles(fileCallback);

    while (running)
    {
        // Wait for the OVERLAPPED event (set by read completion or stopWatching).
        DWORD waitStatus = WaitForSingleObject(overlapped.hEvent, INFINITE);

        if (!running) { // Verifica se foi instruído a parar enquanto esperava
            break;
        }

        if (waitStatus == WAIT_OBJECT_0)
        {
            DWORD bytesTransferred = 0;
            // Read asynchronous operation result.
            BOOL resultSuccess = GetOverlappedResult(hDir, &overlapped, &bytesTransferred, FALSE); // FALSE = não espera

            if (!resultSuccess) {
                DWORD error = GetLastError();
                // ERROR_OPERATION_ABORTED is expected after CancelIoEx.
                if (error == ERROR_OPERATION_ABORTED) {
                    Logger::getInstance().info("FileWatcher::watcherThreadFunc", "ReadDirectoryChangesW operation aborted (likely stopping).");
                    continue; // Volta para o início do loop para verificar 'running'
                }
                // ERROR_NOTIFY_ENUM_DIR may indicate the buffer was too small.
                if (error == ERROR_NOTIFY_ENUM_DIR) {
                    Logger::getInstance().warning("FileWatcher::watcherThreadFunc", "Buffer overflow during ReadDirectoryChangesW (ERROR_NOTIFY_ENUM_DIR). Reenqueuing full directory scan.");
                    processExistingFiles(fileCallback); // Garante que arquivos recentes nao sejam perdidos
                    ResetEvent(overlapped.hEvent);
                    if (running && !startReadDirectoryChanges()) {
                        Logger::getInstance().critical("FileWatcher::watcherThreadFunc", "Failed to reissue ReadDirectoryChangesW after overflow. Stopping watcher.");
                        running = false;
                    }
                    continue;
                }
                else {
                    Logger::getInstance().error("FileWatcher::watcherThreadFunc", "GetOverlappedResult failed. Error code: " + std::to_string(error));
                    // Could stop watcher or attempt recovery; continue for now.
                    // Reset event before next ReadDirectoryChangesW
                    ResetEvent(overlapped.hEvent);
                    // Reissue the read request
                    if (running && !startReadDirectoryChanges()) {
                        Logger::getInstance().critical("FileWatcher::watcherThreadFunc", "Failed to reissue ReadDirectoryChangesW after error. Stopping watcher.");
                        running = false; // Para o loop
                    }
                    continue;
                }
            }


            // bytesTransferred == 0 may indicate overflow or another edge condition.
            // Docs suggest re-enumerating the directory; keep normal flow for now.
            if (bytesTransferred == 0) {
                Logger::getInstance().warning("FileWatcher::watcherThreadFunc", "ReadDirectoryChangesW returned 0 bytes transferred. Reenqueuing full directory scan.");
                processExistingFiles(fileCallback);
                ResetEvent(overlapped.hEvent);
                if (running && !startReadDirectoryChanges()) {
                    Logger::getInstance().critical("FileWatcher::watcherThreadFunc", "Failed to reissue ReadDirectoryChangesW after 0 bytes. Stopping watcher.");
                    running = false;
                }
                continue;
            }


            // --- Process notifications from buffer ---
            FILE_NOTIFY_INFORMATION* pNotify = (FILE_NOTIFY_INFORMATION*)buffer.data();
            DWORD offset = 0;

            do {
                // pNotify points to the current FILE_NOTIFY_INFORMATION.
                pNotify = (FILE_NOTIFY_INFORMATION*)((BYTE*)buffer.data() + offset);

                // Extract file name (WCHAR, not null-terminated).
                std::wstring wFilename(pNotify->FileName, pNotify->FileNameLength / sizeof(WCHAR));
                fs::path fullPath = fs::path(directoryPath) / wFilename; // Usa fs::path para juntar corretamente
                std::string filePathStr = fullPath.u8string();

                if (isFileExcluded(fullPath)) {
                    // Skip file when it is in the exclusion list.
                    offset += pNotify->NextEntryOffset;
                    continue;
                }

                // Handle action (added or renamed to this name).
                if (pNotify->Action == FILE_ACTION_ADDED || pNotify->Action == FILE_ACTION_RENAMED_NEW_NAME)
                {
                    // Recheck if target is a safe regular file after the event.
                    if (isSafeWatchedRegularFile(directoryPath, fullPath))
                    {
                       
                        Logger::getInstance().info("FileWatcher::watcherThreadFunc", "Detected new/renamed file: " + filePathStr);
                        try {
                            fileCallback(fullPath);
                        }
                        catch (const std::exception& e) {
                            Logger::getInstance().error("FileWatcher::watcherThreadFunc", "Exception in file callback for " + filePathStr + ": " + e.what());
                        }
                        catch (...) {
                            Logger::getInstance().error("FileWatcher::watcherThreadFunc", "Unknown exception in file callback for " + filePathStr);
                        }
                    }
                    // Ignore non-regular paths or entries with status errors.
                }

                // Advance to the next notification entry, if any.
                offset += pNotify->NextEntryOffset;

            } while (pNotify->NextEntryOffset != 0);
            // --- End notification processing ---


            // Reissue ReadDirectoryChangesW for the next notification batch.
            // This must happen before returning to wait.
            ResetEvent(overlapped.hEvent); // Reseta o evento manualmente antes da próxima chamada
            if (running && !startReadDirectoryChanges()) {
                Logger::getInstance().critical("FileWatcher::watcherThreadFunc", "Failed to reissue ReadDirectoryChangesW. Stopping watcher.");
                running = false; // Para o loop principal
            }

        }
        else {
            // WaitForSingleObject failed for an unexpected reason.
            DWORD error = GetLastError();
            Logger::getInstance().error("FileWatcher::watcherThreadFunc", "WaitForSingleObject failed unexpectedly. Error code: " + std::to_string(error));
            // Consider stopping the watcher here.
            running = false;
        }
    } // Fim while(running)

    Logger::getInstance().info("FileWatcher::watcherThreadFunc", "Watcher thread finished.");
}
