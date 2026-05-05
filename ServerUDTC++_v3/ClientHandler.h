#pragma once

#include "UDTConnection.h"
#include "FileReceiver.h"
#include "ChunkMetadata.h"
#include "CircuitBreaker.h"
#include "Logger.h"
#include <vector>
#include <stdexcept> // Para std::runtime_error
#include <memory> 
#include <unordered_set>

class ClientHandler {
public:
    ClientHandler(UDTSOCKET clientSocket, sockaddr_in *clientAddress, std::shared_ptr<FileReceiver> fileReceiver);
    ~ClientHandler(); // Certifique-se de fechar o UDTConnection

    void handleClient(); // Método principal executado pela thread
	std::string getClient() const { return connection.getClient(); } // Para obter o ID do cliente

private:
    UDTConnection connection;
    std::shared_ptr<FileReceiver> fileReceiver; // Usa shared_ptr se múltiplas threads acessam o mesmo FileReceiver
    bool running = true;
    CircuitBreaker circuitBreaker;
    std::unordered_set<std::string> seenSecureNonces;
    std::string connectionPreSharedKey;
    bool sendControlMessage(const std::string& message);
    bool receiveControlMessage(std::string& message);
    bool sendErrorToClient(const std::string& message);
	bool parseChunkMessage(const std::vector<char>& buffer, ChunkMetadata &chunk);
};
