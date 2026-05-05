#include "UDTConnection.h"
/**
 * Constructor - Initializes connection parameters.
 */
UDTConnection::UDTConnection(UDTSOCKET client, sockaddr_in *clientAddr)
    : sock(client),
	clientAddr(clientAddr),
    connected(false)
{
	if (client != UDT::INVALID_SOCK) {
		connected = true;
	}
}
/**
 * Destructor - Ensures the connection is properly closed.
 */
UDTConnection::~UDTConnection()
{
    close();
}

/**
 * Close the UDT connection.
 */
void UDTConnection::close()
{
    if (sock != UDT::INVALID_SOCK) {
        UDT::close(sock);
        sock = UDT::INVALID_SOCK;
        connected = false;
    }
}

/**
 * Check if the connection is currently open.
 *
 * @return true if connected, false otherwise.
 */
bool UDTConnection::isConnected() const
{
    return connected && (sock != UDT::INVALID_SOCK);
}

/**
 * Send all data over the connection.
 *
 * @param data Pointer to the data to send.
 * @param size Size of the data in bytes.
 * @return true if successful, false otherwise.
 */
bool UDTConnection::sendAll(const char* data, int size)
{
    if (!isConnected()) {
        return false;
    }

    int sent = 0;
    while (sent < size) {
        int ret = UDT::send(sock, data + sent, size - sent, 0);
        if (ret <= 0) {
            Logger::getInstance().error("UDTConnection",
                "Error sending data: " + std::string(UDT::getlasterror().getErrorMessage()));
            close();
            return false;
        }
        sent += ret;
    }
    return true;
}

/**
 * Receive all data over the connection.
 *
 * @param buffer Pointer to the buffer to receive data into.
 * @param length Size of the buffer in bytes.
 * @return true if successful, false otherwise.
 */
bool UDTConnection::recvAll(char* buffer, int length)
{
    if (!isConnected()) {
        return false;
    }

    int received = 0;
    while (received < length) {
        int ret = UDT::recv(sock, buffer + received, length - received, 0);
        if (ret <= 0) {
            Logger::getInstance().error("UDTConnection",
                "Error receiving data: " + std::string(UDT::getlasterror().getErrorMessage()));
            close();
            return false;
        }
        received += ret;
    }
    return true;
}

/**
 * Send a string over the connection.
 *
 * @param str The string to send.
 * @return true if successful, false otherwise.
 */
bool UDTConnection::sendString(const std::string& str)
{
    uint32_t len = static_cast<uint32_t>(str.size());
    uint32_t netLen = htonl(len);

    if (!sendAll(reinterpret_cast<const char*>(&netLen), sizeof(netLen))) {
        return false;
    }

    if (len > 0 && !sendAll(str.c_str(), len)) {
        return false;
    }

    return true;
}

/**
 * Receive a string over the connection.
 *
 * @param outStr Reference to the string to store the received data.
 * @return true if successful, false otherwise.
 */
bool UDTConnection::recvString(std::string& outStr)
{
    uint32_t netLen;
    if (!recvAll(reinterpret_cast<char*>(&netLen), sizeof(netLen))) {
        return false;
    }

    uint32_t len = ntohl(netLen);
    // Limit maximum size to prevent abuse
    if (len > 65536) {
        Logger::getInstance().error("UDTConnection", "Received string length exceeds maximum limit");
        close();
        return false;
    }

    std::vector<char> buf(len);
    if (!recvAll(buf.data(), len)) {
        return false;
    }

    outStr.assign(buf.begin(), buf.end());
    return true;
}

/**
 * Send an integer over the connection.
 *
 * @param value The integer to send.
 * @return true if successful, false otherwise.
 */
bool UDTConnection::sendInt(int value)
{
    int netVal = htonl(value);
    return sendAll(reinterpret_cast<const char*>(&netVal), sizeof(netVal));
}

/**
 * Receive an integer over the connection.
 *
 * @param value Reference to store the received integer.
 * @return true if successful, false otherwise.
 */
bool UDTConnection::recvInt(int& value)
{
    int netVal;
    if (!recvAll(reinterpret_cast<char*>(&netVal), sizeof(netVal))) {
        return false;
    }

    value = ntohl(netVal);
    return true;
}

