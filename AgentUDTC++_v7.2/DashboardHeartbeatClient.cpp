#include "DashboardHeartbeatClient.h"

#include "Logger.h"
#include "SecurityHandshake.h"

#include <Windows.h>
#include <winhttp.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <vector>

#pragma comment(lib, "winhttp.lib")

namespace {

std::wstring widen(const std::string &value) {
  if (value.empty()) {
    return L"";
  }
  const int size = MultiByteToWideChar(CP_UTF8, 0, value.c_str(),
                                       static_cast<int>(value.size()), nullptr,
                                       0);
  std::wstring result(size, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, value.c_str(),
                      static_cast<int>(value.size()), result.data(), size);
  return result;
}

std::string jsonEscape(const std::string &value) {
  std::ostringstream out;
  for (const unsigned char ch : value) {
    switch (ch) {
    case '\\':
      out << "\\\\";
      break;
    case '"':
      out << "\\\"";
      break;
    case '\n':
      out << "\\n";
      break;
    case '\r':
      out << "\\r";
      break;
    case '\t':
      out << "\\t";
      break;
    default:
      if (ch < 0x20) {
        out << "\\u";
        out << std::hex << std::uppercase << std::setw(4) << std::setfill('0')
            << static_cast<int>(ch);
      } else {
        out << static_cast<char>(ch);
      }
    }
  }
  return out.str();
}

std::string jsonStringArray(const std::vector<std::string> &values) {
  std::ostringstream out;
  out << "[";
  for (size_t i = 0; i < values.size(); ++i) {
    if (i > 0) {
      out << ",";
    }
    out << "\"" << jsonEscape(values[i]) << "\"";
  }
  out << "]";
  return out.str();
}

std::vector<std::string> tailLines(const std::string &path, int maxLines) {
  std::vector<std::string> lines;
  if (maxLines <= 0) {
    return lines;
  }

  std::ifstream file(path);
  if (!file) {
    return lines;
  }

  std::string line;
  while (std::getline(file, line)) {
    lines.push_back(line);
    if (static_cast<int>(lines.size()) > maxLines) {
      lines.erase(lines.begin());
    }
  }
  return lines;
}

std::vector<std::string> filterErrors(const std::vector<std::string> &lines) {
  std::vector<std::string> errors;
  for (const auto &line : lines) {
    std::string lower = line;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lower.find("error") != std::string::npos ||
        lower.find("critical") != std::string::npos ||
        lower.find("failed") != std::string::npos) {
      errors.push_back(line);
    }
  }
  return errors;
}

std::string hostname() {
  char buffer[MAX_COMPUTERNAME_LENGTH + 1] = {};
  DWORD size = sizeof(buffer);
  if (GetComputerNameA(buffer, &size)) {
    return std::string(buffer, size);
  }
  return "unknown";
}

int64_t freeBytesForFirstDir(const std::vector<std::string> &dirs) {
  if (dirs.empty()) {
    return 0;
  }
  ULARGE_INTEGER available = {};
  if (GetDiskFreeSpaceExA(dirs[0].c_str(), &available, nullptr, nullptr)) {
    return static_cast<int64_t>(available.QuadPart);
  }
  return 0;
}

struct ParsedUrl {
  std::wstring host;
  std::wstring path;
  INTERNET_PORT port;
};

ParsedUrl parseHttpsUrl(const std::string &url) {
  std::wstring wideUrl = widen(url);
  URL_COMPONENTS parts = {};
  parts.dwStructSize = sizeof(parts);
  parts.dwSchemeLength = static_cast<DWORD>(-1);
  parts.dwHostNameLength = static_cast<DWORD>(-1);
  parts.dwUrlPathLength = static_cast<DWORD>(-1);
  parts.dwExtraInfoLength = static_cast<DWORD>(-1);
  if (!WinHttpCrackUrl(wideUrl.c_str(), static_cast<DWORD>(wideUrl.size()), 0,
                       &parts)) {
    throw std::runtime_error("Invalid dashboard.url.");
  }
  if (parts.nScheme != INTERNET_SCHEME_HTTPS) {
    throw std::runtime_error("dashboard.url must use https.");
  }

  std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
  if (parts.dwExtraInfoLength > 0) {
    path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
  }
  if (path.empty() || path == L"/") {
    path = L"/api/agents/heartbeat";
  }
  return {std::wstring(parts.lpszHostName, parts.dwHostNameLength), path,
          parts.nPort};
}

} // namespace

DashboardHeartbeatClient::DashboardHeartbeatClient(Database &database,
                                                   const RadarConfig &config)
    : database(database), config(config),
      startedAt(std::chrono::steady_clock::now()) {}

DashboardHeartbeatClient::~DashboardHeartbeatClient() { stop(); }

void DashboardHeartbeatClient::start() {
  if (!config.isDashboardEnabled()) {
    return;
  }
  if (running.exchange(true)) {
    return;
  }
  worker = std::thread(&DashboardHeartbeatClient::run, this);
  Logger::getInstance().info("DashboardHeartbeatClient",
                             "Dashboard heartbeat started.");
}

void DashboardHeartbeatClient::stop() {
  if (!running.exchange(false)) {
    return;
  }
  cv.notify_all();
  if (worker.joinable()) {
    worker.join();
  }
  Logger::getInstance().info("DashboardHeartbeatClient",
                             "Dashboard heartbeat stopped.");
}

void DashboardHeartbeatClient::run() {
  while (running) {
    try {
      sendOnce();
    } catch (const std::exception &ex) {
      Logger::getInstance().warning(
          "DashboardHeartbeatClient",
          std::string("Failed to send dashboard heartbeat: ") + ex.what());
    }

    std::unique_lock<std::mutex> lock(mutex);
    cv.wait_for(lock,
                std::chrono::seconds(
                    config.getDashboardHeartbeatIntervalSeconds()),
                [this] { return !running.load(); });
  }
}

void DashboardHeartbeatClient::sendOnce() {
  const ParsedUrl parsed = parseHttpsUrl(config.getDashboardUrl());
  const std::string payload = buildPayload();
  const std::string timestamp =
      std::to_string(std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count());
  const std::string nonce = SecurityHandshake::CreateChallenge();
  const std::string signature = buildSignature(timestamp, nonce, payload);

  HINTERNET session = WinHttpOpen(L"TransferUDT-Agent/1.0",
                                  WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                  WINHTTP_NO_PROXY_NAME,
                                  WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session) {
    throw std::runtime_error("WinHttpOpen failed.");
  }

  HINTERNET connect =
      WinHttpConnect(session, parsed.host.c_str(), parsed.port, 0);
  if (!connect) {
    WinHttpCloseHandle(session);
    throw std::runtime_error("WinHttpConnect failed.");
  }

  HINTERNET request = WinHttpOpenRequest(
      connect, L"POST", parsed.path.c_str(), nullptr, WINHTTP_NO_REFERER,
      WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
  if (!request) {
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    throw std::runtime_error("WinHttpOpenRequest failed.");
  }

  std::wstring headers =
      L"Content-Type: application/json\r\nX-Client-Id: " +
      widen(config.getSecurityClientId()) + L"\r\nX-Timestamp: " +
      widen(timestamp) + L"\r\nX-Nonce: " + widen(nonce) +
      L"\r\nX-Signature: " + widen(signature) + L"\r\n";

  const BOOL sent = WinHttpSendRequest(
      request, headers.c_str(), static_cast<DWORD>(headers.size()),
      const_cast<char *>(payload.data()), static_cast<DWORD>(payload.size()),
      static_cast<DWORD>(payload.size()), 0);
  if (!sent || !WinHttpReceiveResponse(request, nullptr)) {
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    throw std::runtime_error("Dashboard heartbeat HTTP request failed.");
  }

  DWORD status = 0;
  DWORD statusSize = sizeof(status);
  WinHttpQueryHeaders(request,
                      WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                      WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
                      WINHTTP_NO_HEADER_INDEX);
  WinHttpCloseHandle(request);
  WinHttpCloseHandle(connect);
  WinHttpCloseHandle(session);

  if (status < 200 || status >= 300) {
    throw std::runtime_error("Dashboard heartbeat rejected with HTTP status " +
                             std::to_string(status) + ".");
  }
}

std::string DashboardHeartbeatClient::buildPayload() {
  const auto dirs = config.getDataDirs();
  const auto logs = tailLines(config.getLogFilePath(),
                              config.getDashboardLogTailLines());
  const auto errors = filterErrors(logs);
  int pendingFiles = 0;
  int failedFiles = 0;
  try {
    pendingFiles = static_cast<int>(database.getPendingChunks(0).size());
    failedFiles = static_cast<int>(database.getFailedFiles().size());
  } catch (const std::exception &) {
  }

  const auto uptime = std::chrono::duration_cast<std::chrono::seconds>(
                          std::chrono::steady_clock::now() - startedAt)
                          .count();
  std::ostringstream json;
  json.imbue(std::locale::classic());
  json << "{";
  json << "\"clientId\":\"" << jsonEscape(config.getSecurityClientId())
       << "\",";
  json << "\"hostname\":\"" << jsonEscape(hostname()) << "\",";
  json << "\"agentVersion\":\"AgentUDTC++\",";
  json << "\"serviceStatus\":\"running\",";
  json << "\"uptimeSeconds\":" << std::to_string(uptime) << ",";
  json << "\"watchedDirs\":" << jsonStringArray(dirs) << ",";
  json << "\"pendingFiles\":" << std::to_string(pendingFiles) << ",";
  json << "\"processedFiles\":0,";
  json << "\"failedFiles\":" << std::to_string(failedFiles) << ",";
  json << "\"diskFreeBytes\":" << std::to_string(freeBytesForFirstDir(dirs)) << ",";
  json << "\"lastTransferAt\":null,";
  json << "\"recentErrors\":" << jsonStringArray(errors) << ",";
  json << "\"recentLogs\":" << jsonStringArray(logs);
  json << "}";
  return json.str();
}

std::string DashboardHeartbeatClient::buildSignature(
    const std::string &timestamp, const std::string &nonce,
    const std::string &payload) const {
  const std::string canonical = timestamp + "\n" + nonce + "\n" + payload;
  return SecurityHandshake::SignMessageHex(
      canonical, config.getSecurityClientPrivateKeyPath());
}
