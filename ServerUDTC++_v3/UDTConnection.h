#pragma once

#include <string>
#include <vector>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#pragma warning(push)
#pragma warning(disable : 4251)
#include <udt/udt.h>
#pragma warning(pop)
#include "Logger.h"

/**
 * Class for managing UDT protocol connections
 */
class UDTConnection {
public:
    /**
     * Constructor
     */
    UDTConnection(UDTSOCKET client, sockaddr_in *clientAddr);

    /**
     * Destructor - ensures connection is closed
     */
    ~UDTConnection();

	/**
	 * Set the UDT socket
	 * @param client UDT socket to set
	 */
	void setSocket(UDTSOCKET client) {
		sock = client;
		connected = true;
	}
	/*
     * Convert client address to string for logging and identification
    */
	std::string getClient() const {
        char ipStr[INET_ADDRSTRLEN] = { 0 };
        InetNtopA(AF_INET, &(clientAddr->sin_addr), ipStr, INET_ADDRSTRLEN);
        std::string clientId = std::string(ipStr) + ":" + std::to_string(ntohs(clientAddr->sin_port));
		return clientId;
	}
    /**
     * Retrieves the client's IP address as a string, excluding the port number.
     *
     * @return The client's IP address as a string.
     */
    std::string getClientNoPort() const {
        char ipStr[INET_ADDRSTRLEN] = { 0 };
        InetNtopA(AF_INET, &(clientAddr->sin_addr), ipStr, INET_ADDRSTRLEN);
        std::string clientId = std::string(ipStr);
        return clientId;
    }
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

private:
    // UDT socket
    UDTSOCKET sock;
	sockaddr_in *clientAddr;
    // Connection state
    bool connected;
};