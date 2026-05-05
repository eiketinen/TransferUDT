#include "ServerUDT.h"
#include <limits>

// --- Constructor ---
ServerUDT::ServerUDT() {
  // Initialize thread pool based on config (assuming config is loaded by
  // getInstance)
  try {
    threadPool = std::make_unique<ThreadPool>(
        ServerConfig::getInstance().getNumThreads(),
        ServerConfig::getInstance().getMaxPendingTasks());
  } catch (const std::exception &e) {
    // If thread pool fails, critical error - maybe log if possible, then
    // rethrow?
    Logger::getInstance().critical("ServerUDT",
                                   "Failed to create ThreadPool: {}", e.what());
    OutputDebugStringA(
        ("FATAL: Failed to create ThreadPool: " + std::string(e.what()) + "\n")
            .c_str());
    throw; // Rethrow to prevent server object creation in invalid state
  }
}

// --- Destructor ---
ServerUDT::~ServerUDT() {
  try {
    if (running) {
      Logger::getInstance().info(
          "ServerUDT", "Destructor called while running. Initiating stop...");
      stop(); // Ensure stop is called if destructor is reached while running
    }
    // Join accept thread if it's somehow still joinable (should be joined by
    // stop)
    if (acceptThread.joinable()) {
      acceptThread.join();
    }
    // ThreadPool unique_ptr will call its destructor, stopping threads if not
    // already done.
  } catch (const std::exception &e) {
    try {
      Logger::getInstance().error(
          "ServerUDT", "Exception during destructor cleanup: {}", e.what());
    } catch (...) {
    }
  } catch (...) {
    try {
      Logger::getInstance().error(
          "ServerUDT", "Unknown exception during destructor cleanup.");
    } catch (...) {
    }
  }
  Logger::getInstance().info("ServerUDT", "Destructor finished.");
}

// --- run ---
bool ServerUDT::run(bool runAsService) {

  // 2. Initialize Database & FileReceiver (now that logger is up)
  try {
    Database::initialize(
        ServerConfig::getInstance().getDBFilePath()); // Init singleton
    database = &Database::getInstance();              // Get reference

    auto verifier =
        std::make_unique<SHA256FileVerifier>(); // Using SHA256 implementation
    if (!verifier) {
      Logger::getInstance().critical("ServerUDT",
                                     "Verifier instanciado é nullptr!");
      return false;
    }
    fileReceiver = std::make_shared<FileReceiver>(
        *database, std::move(verifier), ServerConfig::getInstance(),
        *threadPool);
    Logger::getInstance().info("ServerUDT::run",
                               "Database and FileReceiver initialized.");
  } catch (const std::exception &e) {
    Logger::getInstance().critical(
        "ServerUDT::run",
        std::string("Initialization error (DB/FileReceiver): ") + e.what());
    return false;
  }

  // 3. Server Startup Logic
  if (running.load()) { // Use load for atomic bool
    Logger::getInstance().warning("ServerUDT::run", "Server already running.");
    return false;
  }

  // Create required data directories from config
  if (!ensureDirectoryExists(ServerConfig::getInstance().getStoragePath()) ||
      !ensureDirectoryExists(
          ServerConfig::getInstance().getReconstructedPath())) {
    Logger::getInstance().critical(
        "ServerUDT::run", "Failed to create necessary data directories.");
    return false;
  }

  // Initialize UDT library
  if (UDT::startup() != 0) {
    Logger::getInstance().critical("ServerUDT::run", "UDT::startup() failed.");
    return false;
  }
  Logger::getInstance().info("ServerUDT::run", "UDT library started.");

  // Create server socket
  serverSocket = UDT::socket(AF_INET, SOCK_STREAM, 0);
  if (serverSocket == UDT::INVALID_SOCK) {
    Logger::getInstance().critical("ServerUDT::run",
                                   "Failed to create server socket: {}",
                                   UDT::getlasterror().getErrorMessage());
    UDT::cleanup();
    return false;
  }

  // Configure socket options
  configureServerSocketOptions();

  // Bind socket
  sockaddr_in serverAddr{};
  serverAddr.sin_family = AF_INET;
  serverAddr.sin_port =
      htons(static_cast<u_short>(ServerConfig::getInstance().getServerPort()));
  const std::string bindAddress = ServerConfig::getInstance().getBindAddress();
  if (InetPtonA(AF_INET, bindAddress.c_str(), &serverAddr.sin_addr) != 1) {
    Logger::getInstance().critical("ServerUDT::run",
                                   "Invalid server.bind_address '{}'",
                                   bindAddress);
    UDT::close(serverSocket);
    UDT::cleanup();
    return false;
  }

  if (UDT::bind(serverSocket, (sockaddr *)&serverAddr, sizeof(serverAddr)) ==
      UDT::ERROR) {
    Logger::getInstance().critical("ServerUDT::run",
                                   "Failed to bind server socket: {}",
                                   UDT::getlasterror().getErrorMessage());
    UDT::close(serverSocket);
    UDT::cleanup();
    return false;
  }

  // Start listening
  int maxConn = ServerConfig::getInstance().getMaxConnection();

  if (UDT::listen(serverSocket, maxConn) == UDT::ERROR) {
    Logger::getInstance().critical("ServerUDT::run",
                                   "Failed to listen on server socket: {}",
                                   UDT::getlasterror().getErrorMessage());
    UDT::close(serverSocket);
    UDT::cleanup();
    return false;
  }

  Logger::getInstance().info("ServerUDT::run",
                             "Server listening on {}:{}",
                             bindAddress,
                             ServerConfig::getInstance().getServerPort());

  running = true;

  shutdownRequested = false; // Reset flag on start

  // Start accepting clients in a separate thread
  try {
    if (acceptThread.joinable())
      acceptThread.join(); // Join previous if any
    acceptThread = std::thread(&ServerUDT::acceptClients, this);
  } catch (const std::exception &e) {
    Logger::getInstance().critical(
        "ServerUDT::run", "Failed to start accept thread: {}", e.what());
    UDT::close(serverSocket);
    UDT::cleanup();
    running = false;
    return false;
  }

  // 4. Blocking or Returning based on mode
  if (!runAsService) {
    // Console mode: Wait here until shutdown is signaled
    Logger::getInstance().info(
        "ServerUDT::run",
        "Running in console mode. Waiting for shutdown signal...");
    std::unique_lock<std::mutex> lock(shutdownMutex);
    shutdownCV.wait(
        lock, [this] { return shutdownRequested.load(); }); // Wait for flag
    Logger::getInstance().info(
        "ServerUDT::run", "Console mode shutdown detected. Stopping server...");
    stop(); // Call stop synchronously
    Logger::getInstance().info("ServerUDT::run",
                               "Console mode server stopped.");
  } else {
    Logger::getInstance().info(
        "ServerUDT::run",
        "Running in service mode. Server accept thread started.");
    // ServiceMain will wait and call signalShutdown()/stop() externally
  }

  return true; // Indicate successful start (in service mode, doesn't mean it
               // finished)
}

// --- signalShutdown ---
void ServerUDT::signalShutdown() {
  bool alreadyRequested =
      shutdownRequested.exchange(true); // Atomically set flag
  if (!alreadyRequested) {
    Logger::getInstance().info("ServerUDT::signalShutdown",
                               "Shutdown signal received.");
    {
      std::lock_guard<std::mutex> lock(
          shutdownMutex); // Lock needed for CV notify
      // Close the listening socket to potentially unblock the accept thread
      if (serverSocket != UDT::INVALID_SOCK) {
        Logger::getInstance().debug(
            "ServerUDT::signalShutdown",
            "Closing server socket to unblock accept thread.");
        UDT::close(serverSocket);
        // Note: Let stop() set serverSocket_ to INVALID_SOCK for final cleanup
        // state check
      }
    }
    shutdownCV
        .notify_all(); // Wake up any waiting threads (e.g., console run's wait)
  } else {
    Logger::getInstance().debug("ServerUDT::signalShutdown",
                                "Shutdown already requested.");
  }
}

// --- stop ---
void ServerUDT::stop() {
  if (!running.exchange(false)) {
    return; // Já estava parado
  }
  // 1. Ensure shutdown is signaled (idempotent)
  signalShutdown();

  // 2. Join the accept thread
  Logger::getInstance().info("ServerUDT::stop", "Joining accept thread...");
  if (acceptThread.joinable()) {
    acceptThread.join();
    Logger::getInstance().info("ServerUDT::stop", "Accept thread joined.");
  } else {
    Logger::getInstance().info("ServerUDT::stop",
                               "Accept thread was not joinable.");
  }

  // 3. Stop the thread pool (wait for client handlers to finish)
  if (threadPool) {
    Logger::getInstance().info("ServerUDT::stop", "Stopping thread pool...");
    threadPool->stop(); // Explicitly stop and wait
    threadPool.reset(); // Release resources
    Logger::getInstance().info("ServerUDT::stop", "Thread pool stopped.");
  }

  // 4. Close server socket (if not already closed by signalShutdown)
  // Safer to just try closing again.
  if (serverSocket != UDT::INVALID_SOCK) {
    UDT::close(serverSocket);
    serverSocket = UDT::INVALID_SOCK;
    Logger::getInstance().debug("ServerUDT::stop", "Server socket closed.");
  }

  // 5. Cleanup UDT library
  UDT::cleanup();
  Logger::getInstance().info("ServerUDT::stop", "UDT library cleaned up.");

  // 6. Update state
  Logger::getInstance().info("ServerUDT::stop", "Server stopped completely.");
}

// --- acceptClients ---
void ServerUDT::acceptClients() {

  Logger::getInstance().info("ServerUDT::acceptClients",
                             "Accept thread started.");

  while (true) {
    // Check for shutdown *before* blocking accept
    if (shutdownRequested.load()) {
      Logger::getInstance().info(
          "ServerUDT::acceptClients",
          "Shutdown detected before accept. Exiting loop.");
      break;
    }

    sockaddr_in clientAddr{};
    int addrLen = sizeof(clientAddr);
    UDTSOCKET clientSocket = UDT::accept(
        serverSocket, reinterpret_cast<sockaddr *>(&clientAddr), &addrLen);

    // Check for shutdown *immediately* after accept returns
    if (shutdownRequested.load()) {
      Logger::getInstance().info("ServerUDT::acceptClients",
                                 "Shutdown detected after accept return. "
                                 "Cleaning up socket if valid.");
      if (clientSocket != UDT::INVALID_SOCK)
        UDT::close(clientSocket);
      break; // Exit loop
    }

    // Handle accept result
    if (clientSocket == UDT::INVALID_SOCK) {
      int errorCode = UDT::getlasterror().getErrorCode();
      // Only log error if not shutting down (errorCode 6003 often means socket
      // closed)
      if (!shutdownRequested.load() && errorCode != 0 && errorCode != 6003) {
        Logger::getInstance().error("ServerUDT::acceptClients",
                                    "Accept failed: {}",
                                    UDT::getlasterror().getErrorMessage());
      }
      // If accept fails (e.g., socket closed), break loop if shutdown intended,
      // otherwise pause
      if (shutdownRequested.load() || errorCode == 6003) {
        Logger::getInstance().info("ServerUDT::acceptClients",
                                   "Accept failed likely due to shutdown or "
                                   "socket close. Exiting loop.");
        break;
      }
      // Prevent spinning on non-fatal errors
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      continue; // Try accepting again
    }

    char clientIp[INET_ADDRSTRLEN] = {0};
    InetNtopA(AF_INET, &(clientAddr.sin_addr), clientIp, INET_ADDRSTRLEN);
    const std::string acceptedClientId =
        std::string(clientIp) + ":" +
        std::to_string(ntohs(clientAddr.sin_port));

    if (!ServerConfig::getInstance().isClientAllowed(clientIp)) {
      Logger::getInstance().warning("ServerUDT::acceptClients",
                                    "Rejected client {} because it is not in "
                                    "server.allowed_clients.",
                                    acceptedClientId);
      UDT::close(clientSocket);
      continue;
    }

    // Enqueue client handling task
    try {
      if (threadPool && fileReceiver) { // Ensure pool and receiver are valid
        std::shared_ptr<FileReceiver> receiver = fileReceiver;
        threadPool->addTask([clientSock = clientSocket,
                             cAddr = std::make_shared<sockaddr_in>(clientAddr),
                             receiver, acceptedClientId]() mutable {
          std::string clientIdForLog = acceptedClientId;
          // Capture necessary data, create handler inside the task lambda
          try {
            ClientHandler handler(clientSock, cAddr.get(),
                                  receiver); // Pass socket, addr, receiver
            handler.handleClient();          // Handle client connection
            clientIdForLog = handler.getClient();
            Logger::getInstance().debug("ServerUDT::acceptClients",
                                        "Client handler finished for {}",
                                        clientIdForLog);
          } catch (const std::exception &e) {
            Logger::getInstance().error(
                "ServerUDT::acceptClients",
                "Exception in ClientHandler task for {}: {}", clientIdForLog,
                e.what());
            // Ensure socket is closed if handler fails catastrophically
            UDT::close(clientSock);
          } catch (...) {
            Logger::getInstance().error(
                "ServerUDT::acceptClients",
                "Unknown exception in ClientHandler task for {}",
                clientIdForLog);
            // Ensure socket is closed if handler fails catastrophically
            UDT::close(clientSock);
          }
          // Socket closure is managed by ClientHandler/UDTConnection destructor
          // now
        });
        // Detach clientSocket from this scope - ownership passed to
        // lambda/ClientHandler
        clientSocket = UDT::INVALID_SOCK;
      } else {
        Logger::getInstance().error("ServerUDT::acceptClients",
                                    "ThreadPool or FileReceiver not "
                                    "initialized. Cannot handle client {}.",
                                    acceptedClientId);
        UDT::close(clientSocket); // Close the connection if we can't handle it
      }
    } catch (const std::exception &e) {
      Logger::getInstance().error("ServerUDT::acceptClients",
                                  "Failed to enqueue client handler for {}: {}",
                                  acceptedClientId, e.what());
      UDT::close(clientSocket); // Close if enqueue fails
    }

  } // End while loop

  Logger::getInstance().info("ServerUDT::acceptClients",
                             "Accept thread finished.");
}

// --- configureServerSocketOptions ---
void ServerUDT::configureServerSocketOptions() {
  if (serverSocket == UDT::INVALID_SOCK)
    return;

  Logger::getInstance().debug("ServerUDT::configureServerSocketOptions",
                              "Configuring server socket options...");

  // Set linger options (optional, can help ensure data is sent on close)
  linger lingerOpt;
  lingerOpt.l_onoff = ServerConfig::getInstance().getLingerOnOff();
  lingerOpt.l_linger = ServerConfig::getInstance().getLingerTime();

  if (UDT::setsockopt(serverSocket, 0, UDT_LINGER, &lingerOpt,
                      sizeof(linger)) == UDT::ERROR) {
    Logger::getInstance().warning("ServerUDT::configureServerSocketOptions",
                                  "Failed to set UDT_LINGER: {}",
                                  UDT::getlasterror().getErrorMessage());
  }

  // Blocking settings (typically true for server accept/listen)
  bool block = true;
  UDT::setsockopt(serverSocket, 0, UDT_RCVSYN, &block,
                  sizeof(bool)); // Blocking receive
  UDT::setsockopt(serverSocket, 0, UDT_SNDSYN, &block,
                  sizeof(bool)); // Blocking send

  // Retrieve and set options from ServerConfig::getInstance()
  try {
    const DWORDLONG maxInt =
        static_cast<DWORDLONG>((std::numeric_limits<int>::max)());

    DWORDLONG rcvbufCfg = ServerConfig::getInstance().getReceiveBuffer();
    int rcvbuf = static_cast<int>(rcvbufCfg > maxInt ? maxInt : rcvbufCfg);
    if (rcvbuf > 0) {
      if (UDT::setsockopt(serverSocket, 0, UDT_RCVBUF, &rcvbuf,
                          sizeof(rcvbuf)) == UDT::ERROR) {
        Logger::getInstance().warning("ServerUDT::configureServerSocketOptions",
                                      "Failed to set UDT_RCVBUF: {}",
                                      UDT::getlasterror().getErrorMessage());
      }
      if (UDT::setsockopt(serverSocket, 0, UDP_RCVBUF, &rcvbuf,
                          sizeof(rcvbuf)) == UDT::ERROR) {
        Logger::getInstance().warning("ServerUDT::configureServerSocketOptions",
                                      "Failed to set UDP_RCVBUF: {}",
                                      UDT::getlasterror().getErrorMessage());
      }
    }

    DWORDLONG sndbufCfg = ServerConfig::getInstance().getSendBuffer();
    int sndbuf = static_cast<int>(sndbufCfg > maxInt ? maxInt : sndbufCfg);
    if (sndbuf > 0) {
      if (UDT::setsockopt(serverSocket, 0, UDT_SNDBUF, &sndbuf,
                          sizeof(sndbuf)) == UDT::ERROR) {
        Logger::getInstance().warning("ServerUDT::configureServerSocketOptions",
                                      "Failed to set UDT_SNDBUF: {}",
                                      UDT::getlasterror().getErrorMessage());
      }
      if (UDT::setsockopt(serverSocket, 0, UDP_SNDBUF, &sndbuf,
                          sizeof(sndbuf)) == UDT::ERROR) {
        Logger::getInstance().warning("ServerUDT::configureServerSocketOptions",
                                      "Failed to set UDP_SNDBUF: {}",
                                      UDT::getlasterror().getErrorMessage());
      }
    }

    int64_t maxbw = ServerConfig::getInstance().getMaxBandwidth();
    if (maxbw > 0) {
      if (UDT::setsockopt(serverSocket, 0, UDT_MAXBW, &maxbw, sizeof(maxbw)) ==
          UDT::ERROR) {
        Logger::getInstance().warning("ServerUDT::configureServerSocketOptions",
                                      "Failed to set UDT_MAXBW: {}",
                                      UDT::getlasterror().getErrorMessage());
      }
    }

    int mss = ServerConfig::getInstance().getSegmentSize();
    if (mss > 0 && UDT::setsockopt(serverSocket, 0, UDT_MSS, &mss,
                                   sizeof(mss)) == UDT::ERROR) {
      Logger::getInstance().warning("ServerUDT::configureServerSocketOptions",
                                    "Failed to set UDT_MSS: {}",
                                    UDT::getlasterror().getErrorMessage());
    }

    int sendTimeout = ServerConfig::getInstance().getSendTimeout();
    if (sendTimeout > 0 &&
        UDT::setsockopt(serverSocket, 0, UDT_SNDTIMEO, &sendTimeout,
                        sizeof(sendTimeout)) == UDT::ERROR) {
      Logger::getInstance().warning("ServerUDT::configureServerSocketOptions",
                                    "Failed to set UDT_SNDTIMEO: {}",
                                    UDT::getlasterror().getErrorMessage());
    }

    int rcvTimeout = ServerConfig::getInstance().getReceiveTimeout();
    if (rcvTimeout > 0 &&
        UDT::setsockopt(serverSocket, 0, UDT_RCVTIMEO, &rcvTimeout,
                        sizeof(rcvTimeout)) == UDT::ERROR) {
      Logger::getInstance().warning("ServerUDT::configureServerSocketOptions",
                                    "Failed to set UDT_RCVTIMEO: {}",
                                    UDT::getlasterror().getErrorMessage());
    }
  } catch (const std::exception &e) {
    Logger::getInstance().error(
        "ServerUDT::configureServerSocketOptions",
        "Error accessing configuration for socket options: {}", e.what());
  }
}

// --- ensureDirectoryExists ---
bool ServerUDT::ensureDirectoryExists(const fs::path &dir) {
  try {
    if (!fs::exists(dir)) {
      if (fs::create_directories(dir)) {
        Logger::getInstance().debug("ServerUDT::ensureDirectoryExists",
                                    "Created directory: {}", dir.string());
        return true;
      } else {
        Logger::getInstance().error("ServerUDT::ensureDirectoryExists",
                                    "Failed to create directory: {}",
                                    dir.string());
        return false;
      }
    } else if (!fs::is_directory(dir)) {
      Logger::getInstance().error("ServerUDT::ensureDirectoryExists",
                                  "Path exists but is not a directory: {}",
                                  dir.string());
      return false;
    }
    return true; // Directory exists
  } catch (const fs::filesystem_error &e) {
    Logger::getInstance().error("ServerUDT::ensureDirectoryExists",
                                "Filesystem error for directory {}: {}",
                                dir.string(), e.what());
    return false;
  }
}
