#pragma once

#include "Database.h"
#include "RadarConfig.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

class DashboardHeartbeatClient {
public:
  DashboardHeartbeatClient(Database &database, const RadarConfig &config);
  ~DashboardHeartbeatClient();

  void start();
  void stop();

private:
  Database &database;
  const RadarConfig &config;
  std::atomic<bool> running{false};
  std::thread worker;
  std::mutex mutex;
  std::condition_variable cv;
  std::chrono::steady_clock::time_point startedAt;

  void run();
  void sendOnce();
  std::string buildPayload();
  std::string buildSignature(const std::string &timestamp,
                             const std::string &nonce,
                             const std::string &payload) const;
};
