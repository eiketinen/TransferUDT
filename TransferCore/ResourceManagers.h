#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#pragma warning(push)
#pragma warning(disable : 4251)
#include <udt/udt.h>
#pragma warning(pop)
#include "Logger.h"
#include <fstream>
#include <memory>

// RAII Wrapper para sockets UDT
class UDTSocketRAII {
private:
  UDTSOCKET &sockRef;
  bool released;

public:
  UDTSocketRAII(UDTSOCKET &sock) : sockRef(sock), released(false) {}

  ~UDTSocketRAII() { close(); }

  void close() {
    if (!released && sockRef != UDT::INVALID_SOCK) {
      if (UDT::close(sockRef) == UDT::ERROR) {
        Logger::getInstance().error(
            "UDTSocketRAII", "Erro ao fechar socket UDT: {}",
            std::string(UDT::getlasterror().getErrorMessage()));
      }
      sockRef = UDT::INVALID_SOCK;
      released = true;
    }
  }

  void release() { released = true; }
};

// RAII Wrapper para filestreams
template <typename StreamType> class FileStreamRAII {
private:
  StreamType &streamRef;
  bool released;

public:
  FileStreamRAII(StreamType &stream) : streamRef(stream), released(false) {}

  ~FileStreamRAII() { close(); }

  void close() {
    if (!released && streamRef.is_open()) {
      streamRef.close();
      released = true;
    }
  }

  void release() { released = true; }
};

using IfstreamRAII = FileStreamRAII<std::ifstream>;
using OfstreamRAII = FileStreamRAII<std::ofstream>;

// RAII Wrapper para Mutex Locks
class MutexLockRAII {
private:
  std::mutex &mutexRef;
  bool locked;

public:
  MutexLockRAII(std::mutex &mutex) : mutexRef(mutex), locked(true) {
    mutexRef.lock();
  }

  ~MutexLockRAII() { unlock(); }

  void unlock() {
    if (locked) {
      mutexRef.unlock();
      locked = false;
    }
  }

  void relock() {
    if (!locked) {
      mutexRef.lock();
      locked = true;
    }
  }
};