#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "Logger.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>

namespace fs = std::filesystem;

/**
 * Watches one directory and notifies the caller for newly detected files.
 * Uses ReadDirectoryChangesW with overlapped I/O on Windows.
 */
class FileWatcher {
public:
    /**
     * @param directoryPath Directory to monitor.
     * @param checkIntervalSeconds Compatibility interval; not used by the
     * ReadDirectoryChangesW loop.
     */
    FileWatcher(const fs::path& directoryPath, int checkIntervalSeconds = 5);

    /// Stops the watcher thread and releases native handles.
    ~FileWatcher();

    /// Starts asynchronous watch and calls fileCallback on new files.
    void startWatching(std::function<void(const fs::path&)> fileCallback);

    /// Stops asynchronous watch.
    void stopWatching();

private:
    /// Directory being watched.
    fs::path directoryPath;

    /// Compatibility polling interval (currently unused by the main loop).
    int checkIntervalSeconds;

    /// Global run flag for the watcher thread.
    std::atomic<bool> running;

    /// Background thread that processes notifications.
    std::thread watcherThread;

    /// Tracks files already being handled.
    std::set<std::string> filesInProgress;

    /// Synchronizes access to filesInProgress.
    mutable std::mutex filesMutex;

    /// Native directory handle used by ReadDirectoryChangesW.
    HANDLE hDir = INVALID_HANDLE_VALUE;
    /// OVERLAPPED state used for asynchronous I/O.
    OVERLAPPED overlapped = { 0 };
    /// Reusable native buffer for change notifications.
    std::vector<BYTE> buffer;
    /// Default notification buffer size (64 KB).
    static const DWORD BUFFER_SIZE = 64 * 1024;
    /// Filters create/rename/modify events for files and directories.
    DWORD notifyFilter = FILE_NOTIFY_CHANGE_FILE_NAME |
                          FILE_NOTIFY_CHANGE_DIR_NAME |
                          FILE_NOTIFY_CHANGE_LAST_WRITE |
                          FILE_NOTIFY_CHANGE_SIZE;

    /// Worker thread routine.
    void watcherThreadFunc(std::function<void(const fs::path&)> fileCallback);

    /// Emits callback for files that already exist when watching starts.
    void processExistingFiles(const std::function<void(const fs::path&)>& fileCallback);

    /// Issues an overlapped ReadDirectoryChangesW request.
    bool startReadDirectoryChanges();
};
