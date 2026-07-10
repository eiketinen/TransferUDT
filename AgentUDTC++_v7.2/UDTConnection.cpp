#include "UDTConnection.h"
#include <limits>
#include <stdexcept>
#include <vector>

/**
 * Constructor - Initializes connection parameters.
 */
UDTConnection::UDTConnection()
    : sock(UDT::INVALID_SOCK), connected(false), serverPort(0) {}

/**
 * Destructor - Ensures the connection is properly closed.
 */
UDTConnection::~UDTConnection() { close(); }

/**
 * Connect to a UDT server using IPv4 or IPv6.
 *
 * @param host Server hostname or IP address.
 * @param port Server port.
 * @param maxBandwidth Maximum bandwidth in bytes/sec (0 for unlimited).
 * @param segmentSize UDT segment size.
 * @param sendBuffer Send buffer size in bytes.
 * @param receiveBuffer Receive buffer size in bytes.
 * @param sendTimeout Send timeout in milliseconds.
 * @param receiveTimeout Receive timeout in milliseconds.
 * @return true if connection is successful, false otherwise.
 */
bool UDTConnection::connect(const std::string &host, int port,
                            int64_t maxBandwidth, int segmentSize,
                            int64_t sendBuffer, int64_t receiveBuffer,
                            int sendTimeout, int receiveTimeout) {
  // Store server information
  serverHost = host;
  serverPort = port;

  connected = false;
  sock = UDT::INVALID_SOCK;
  secureSessionKey.clear();
  secureSessionId.clear();
  nextSecureOutboundSequence = 1;
  expectedSecureInboundSequence = 1;

  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;     // Allow IPv4 or IPv6 resolution
  hints.ai_socktype = SOCK_STREAM; // UDT uses stream semantics
  hints.ai_protocol = 0;

  addrinfo *result = nullptr;
  std::string portStr = std::to_string(port);
  int gaiResult = getaddrinfo(host.c_str(), portStr.c_str(), &hints, &result);
  if (gaiResult != 0) {
    const char *gaiMsg = gai_strerrorA(gaiResult);
    Logger::getInstance().error(
        "UDTConnection::connect",
        "Failed to resolve host '" + host +
            "': " + (gaiMsg ? std::string(gaiMsg) : std::to_string(gaiResult)));
    return false;
  }

  bool connectedSuccessfully = false;

  for (addrinfo *ptr = result; ptr != nullptr; ptr = ptr->ai_next) {
    sock = UDT::socket(ptr->ai_family, ptr->ai_socktype, ptr->ai_protocol);
    if (sock == UDT::INVALID_SOCK) {
      Logger::getInstance().warning(
          "UDTConnection::connect",
          "Failed to create UDT socket for resolved address: " +
              std::string(UDT::getlasterror().getErrorMessage()));
      continue;
    }

    int ret;
    if (maxBandwidth > 0) {
      ret = UDT::setsockopt(sock, 0, UDT_MAXBW, &maxBandwidth,
                            sizeof(maxBandwidth));
      if (ret == UDT::ERROR) {
        Logger::getInstance().warning(
            "UDTConnection::connect",
            "Failed to set UDT_MAXBW: " +
                std::string(UDT::getlasterror().getErrorMessage()));
        UDT::close(sock);
        sock = UDT::INVALID_SOCK;
        continue;
      }
    }

    ret = UDT::setsockopt(sock, 0, UDT_MSS, &segmentSize, sizeof(segmentSize));
    if (ret == UDT::ERROR) {
      Logger::getInstance().warning(
          "UDTConnection::connect",
          "Failed to set UDT_MSS: " +
              std::string(UDT::getlasterror().getErrorMessage()));
      UDT::close(sock);
      sock = UDT::INVALID_SOCK;
      continue;
    }

    ret = UDT::setsockopt(sock, 0, UDT_SNDBUF, &sendBuffer, sizeof(sendBuffer));
    if (ret == UDT::ERROR) {
      Logger::getInstance().warning(
          "UDTConnection::connect",
          "Failed to set UDT_SNDBUF: " +
              std::string(UDT::getlasterror().getErrorMessage()));
      UDT::close(sock);
      sock = UDT::INVALID_SOCK;
      continue;
    }

    ret = UDT::setsockopt(sock, 0, UDP_SNDBUF, &sendBuffer, sizeof(sendBuffer));
    if (ret == UDT::ERROR) {
      Logger::getInstance().warning(
          "UDTConnection::connect",
          "Failed to set UDP_SNDBUF: " +
              std::string(UDT::getlasterror().getErrorMessage()));
      // We don't abort connection here since it's underlying OS buffer and it
      // might just cap it
    }

    ret = UDT::setsockopt(sock, 0, UDT_RCVBUF, &receiveBuffer,
                          sizeof(receiveBuffer));
    if (ret == UDT::ERROR) {
      Logger::getInstance().warning(
          "UDTConnection::connect",
          "Failed to set UDT_RCVBUF: " +
              std::string(UDT::getlasterror().getErrorMessage()));
      UDT::close(sock);
      sock = UDT::INVALID_SOCK;
      continue;
    }

    ret = UDT::setsockopt(sock, 0, UDP_RCVBUF, &receiveBuffer,
                          sizeof(receiveBuffer));
    if (ret == UDT::ERROR) {
      Logger::getInstance().warning(
          "UDTConnection::connect",
          "Failed to set UDP_RCVBUF: " +
              std::string(UDT::getlasterror().getErrorMessage()));
      // We don't abort connection here since it's underlying OS buffer and it
      // might just cap it
    }

    ret = UDT::setsockopt(sock, 0, UDT_SNDTIMEO, &sendTimeout,
                          sizeof(sendTimeout));
    if (ret == UDT::ERROR) {
      Logger::getInstance().warning(
          "UDTConnection::connect",
          "Failed to set UDT_SNDTIMEO: " +
              std::string(UDT::getlasterror().getErrorMessage()));
      UDT::close(sock);
      sock = UDT::INVALID_SOCK;
      continue;
    }

    ret = UDT::setsockopt(sock, 0, UDT_RCVTIMEO, &receiveTimeout,
                          sizeof(receiveTimeout));
    if (ret == UDT::ERROR) {
      Logger::getInstance().warning(
          "UDTConnection::connect",
          "Failed to set UDT_RCVTIMEO: " +
              std::string(UDT::getlasterror().getErrorMessage()));
      UDT::close(sock);
      sock = UDT::INVALID_SOCK;
      continue;
    }

    if (UDT::connect(sock, ptr->ai_addr, static_cast<int>(ptr->ai_addrlen)) ==
        UDT::ERROR) {
      Logger::getInstance().warning(
          "UDTConnection::connect",
          "UDT::connect failed: " +
              std::string(UDT::getlasterror().getErrorMessage()));
      UDT::close(sock);
      sock = UDT::INVALID_SOCK;
      continue;
    }

    char numericHost[NI_MAXHOST] = {0};
    std::string resolvedAddress = host;
    if (getnameinfo(ptr->ai_addr, static_cast<socklen_t>(ptr->ai_addrlen),
                    numericHost, sizeof(numericHost), nullptr, 0,
                    NI_NUMERICHOST) == 0) {
      resolvedAddress.assign(numericHost);
    }

    Logger::getInstance().info("UDTConnection::connect",
                               "Connection established with " +
                                   resolvedAddress + ":" +
                                   std::to_string(port));

    connectedSuccessfully = true;
    break;
  }

  freeaddrinfo(result);

  if (!connectedSuccessfully) {
    Logger::getInstance().error("UDTConnection::connect",
                                "Failed to connect to " + host + ":" +
                                    std::to_string(port) +
                                    " using any resolved address.");
    sock = UDT::INVALID_SOCK;
    return false;
  }

  connected = true;
  return true;
}

/**
 * Close the UDT connection.
 */
void UDTConnection::close() {
  if (sock != UDT::INVALID_SOCK) {
    UDT::close(sock);
    sock = UDT::INVALID_SOCK;
  }
  connected = false;
  secureSessionKey.clear();
  secureSessionId.clear();
  nextSecureOutboundSequence = 1;
  expectedSecureInboundSequence = 1;
}

/**
 * Check if the connection is currently open.
 *
 * @return true if connected, false otherwise.
 */
bool UDTConnection::isConnected() const {
  if (!connected || sock == UDT::INVALID_SOCK) {
    return false;
  }

  UDTSTATUS status = UDT::getsockstate(sock);
  return status == CONNECTED;
}

/**
 * Send all data over the connection.
 *
 * @param data Pointer to the data to send.
 * @param size Size of the data in bytes.
 * @return true if successful, false otherwise.
 */
bool UDTConnection::sendAll(const char *data, int size) {
  if (!isConnected()) {
    return false;
  }

  int sent = 0;
  while (sent < size) {
    int ret = UDT::send(sock, data + sent, size - sent, 0);
    if (ret <= 0) {
      Logger::getInstance().error(
          "UDTConnection::sendAll",
          "Error sending data: " +
              std::string(UDT::getlasterror().getErrorMessage()));
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
bool UDTConnection::recvAll(char *buffer, int length) {
  if (!isConnected()) {
    return false;
  }

  int received = 0;
  while (received < length) {
    int ret = UDT::recv(sock, buffer + received, length - received, 0);
    if (ret <= 0) {
      Logger::getInstance().error(
          "UDTConnection::recvAll",
          "Error receiving data: " +
              std::string(UDT::getlasterror().getErrorMessage()));
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
bool UDTConnection::sendString(const std::string &str) {
  uint32_t len = static_cast<uint32_t>(str.size());
  uint32_t netLen = htonl(len);

  if (!sendAll(reinterpret_cast<const char *>(&netLen), sizeof(netLen))) {
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
bool UDTConnection::recvString(std::string &outStr) {
  uint32_t netLen;
  if (!recvAll(reinterpret_cast<char *>(&netLen), sizeof(netLen))) {
    return false;
  }

  uint32_t len = ntohl(netLen);
  // Limit maximum size to prevent abuse
  if (len > 65536) {
    Logger::getInstance().error("UDTConnection::recvString",
                                "Received string length exceeds maximum limit");
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
bool UDTConnection::sendInt(int value) {
  int netVal = htonl(value);
  return sendAll(reinterpret_cast<const char *>(&netVal), sizeof(netVal));
}

/**
 * Receive an integer over the connection.
 *
 * @param value Reference to store the received integer.
 * @return true if successful, false otherwise.
 */
bool UDTConnection::recvInt(int &value) {
  int netVal;
  if (!recvAll(reinterpret_cast<char *>(&netVal), sizeof(netVal))) {
    return false;
  }

  value = ntohl(netVal);
  return true;
}

bool UDTConnection::setReceiveTimeout(int millis) {
  if (sock == UDT::INVALID_SOCK) {
    return false;
  }
  int timeout = millis;
  if (UDT::setsockopt(sock, 0, UDT_RCVTIMEO, &timeout, sizeof(timeout)) ==
      UDT::ERROR) {
    Logger::getInstance().error(
        "UDTConnection::setReceiveTimeout",
        "Error setting UDT_RCVTIMEO: " +
            std::string(UDT::getlasterror().getErrorMessage()));
    return false;
  }
  return true;
}

bool UDTConnection::setSendTimeout(int millis) {
  if (sock == UDT::INVALID_SOCK) {
    return false;
  }
  int timeout = millis;
  if (UDT::setsockopt(sock, 0, UDT_SNDTIMEO, &timeout, sizeof(timeout)) ==
      UDT::ERROR) {
    Logger::getInstance().error(
        "UDTConnection::setSendTimeout",
        "Error setting UDT_SNDTIMEO: " +
            std::string(UDT::getlasterror().getErrorMessage()));
    return false;
  }
  return true;
}

bool UDTConnection::sendKeepAlive(const std::string &payload) {
  if (!isConnected()) {
    return false;
  }
  if (payload.empty()) {
    return true;
  }
  if (!sendString(payload)) {
    Logger::getInstance().warning("UDTConnection::sendKeepAlive",
                                  "Failed to send keep-alive payload.");
    return false;
  }
  return true;
}

UDTSTATUS UDTConnection::getState() const {
  if (sock == UDT::INVALID_SOCK) {
    return NONEXIST;
  }
  return UDT::getsockstate(sock);
}

void UDTConnection::setSecureSessionId(std::string sessionId) {
  if (sessionId.empty()) {
    throw std::invalid_argument("Secure session id cannot be empty.");
  }
  secureSessionId = std::move(sessionId);
  nextSecureOutboundSequence = 1;
  expectedSecureInboundSequence = 1;
}

uint64_t UDTConnection::takeNextSecureOutboundSequence() {
  if (secureSessionId.empty()) {
    throw std::runtime_error("Secure session id has not been established.");
  }
  if (nextSecureOutboundSequence == (std::numeric_limits<uint64_t>::max)()) {
    throw std::runtime_error("Secure outbound sequence space is exhausted.");
  }
  return nextSecureOutboundSequence++;
}

bool UDTConnection::acceptSecureInboundSequence(uint64_t sequenceNumber) {
  if (secureSessionId.empty() ||
      expectedSecureInboundSequence == (std::numeric_limits<uint64_t>::max)() ||
      sequenceNumber != expectedSecureInboundSequence) {
    return false;
  }
  ++expectedSecureInboundSequence;
  return true;
}
