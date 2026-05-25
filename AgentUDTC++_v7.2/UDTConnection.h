#pragma once

#include <string>
#include <utility>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#pragma warning(push)
#pragma warning(disable : 4251)
#include <udt/udt.h>
#pragma warning(pop)
#include "Logger.h"
#include "CircuitBreaker.h"

/**
 * Class for managing UDT protocol connections
 */
class UDTConnection {
public:
    /**
     * Constructor
     */
    UDTConnection();

    /**
     * Destructor - ensures connection is closed
     */
    ~UDTConnection();

    /**
     * Connect to a UDT server
     * @param host Server hostname or IP
     * @param port Server port
     * @param maxBandwidth Maximum bandwidth in bytes/sec (0 for unlimited)
     * @param segmentSize UDT segment size
     * @param sendBuffer Send buffer size in bytes
     * @param receiveBuffer Receive buffer size in bytes
     * @param sendTimeout Send timeout in milliseconds
     * @param receiveTimeout Receive timeout in milliseconds
     * @return true if connection successful, false otherwise
     */
    bool connect(
        const std::string& host,
        int port,
        int64_t maxBandwidth = 0,
        int segmentSize = 1350,
        int64_t sendBuffer = 4 * 1024 * 1024,
        int64_t receiveBuffer = 4 * 1024 * 1024,
        int sendTimeout = 20000,
        int receiveTimeout = 20000
    );

    /**
     * Close the connection
     */
    void close();

    /**
     * Check if the connection is open
     * @return true if connected, false otherwise
     */
    bool isConnected() const;

    /**
     * Send data over the connection
     * @param data Data to send
     * @return true if successful, false otherwise
     */
    bool sendAll(const char* data, int size);

    /**
     * Receive exact number of bytes from the connection
     * @param buffer Buffer to store received data
     * @param length Number of bytes to receive
     * @return true if successful, false otherwise
     */
    bool recvAll(char* buffer, int length);

    /**
     * Send a string with length prefix
     * @param str String to send
     * @return true if successful, false otherwise
     */
    bool sendString(const std::string& str);

    /**
     * Receive a string with length prefix
     * @param outStr Reference to store the received string
     * @return true if successful, false otherwise
     */
    bool recvString(std::string& outStr);

    /**
     * Send an integer in network byte order
     * @param value Integer to send
     * @return true if successful, false otherwise
     */
    bool sendInt(int value);

    /**
     * Receive an integer in network byte order
     * @param value Reference to store the received integer
     * @return true if successful, false otherwise
     */
    bool recvInt(int& value);

    /**
     * Adjust the receive timeout for the underlying UDT socket.
     * @param millis Timeout in milliseconds (0 disables timeout).
     * @return true if the option was applied successfully.
     */
    bool setReceiveTimeout(int millis);

    /**
     * Adjust the send timeout for the underlying UDT socket.
     * @param millis Timeout in milliseconds (0 disables timeout).
     * @return true if the option was applied successfully.
     */
    bool setSendTimeout(int millis);

    /**
     * Sends a keep-alive payload over the connection.
     * @param payload Message to send; empty payload results in no data sent.
     * @return true if successful, false otherwise.
     */
    bool sendKeepAlive(const std::string& payload);

    /**
     * Returns the current UDT socket state (see UDTSTATUS).
     */
    UDTSTATUS getState() const;

    void setSecureSessionKey(std::string key) { secureSessionKey = std::move(key); }
    const std::string& getSecureSessionKey() const { return secureSessionKey; }

private:
    // UDT socket
    UDTSOCKET sock;

    // Connection state
    bool connected;

    // Server information
    std::string serverHost;
    int serverPort;
    std::string secureSessionKey;
};
