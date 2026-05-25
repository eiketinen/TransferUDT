#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>

class AdaptiveChunkController {
public:
  struct Settings {
    bool enabled{false};
    uint64_t minChunkSizeBytes{32ULL * 1024ULL};
    uint64_t maxChunkSizeBytes{4ULL * 1024ULL * 1024ULL};
    uint64_t initialChunkSizeBytes{256ULL * 1024ULL};
    int targetAckMillis{700};
  };

  struct Snapshot {
    bool enabled{false};
    uint64_t suggestedChunkSizeBytes{0};
    double ewmaAckMillis{0.0};
    double ewmaThroughputBytesPerSecond{0.0};
    uint64_t successfulSamples{0};
    uint64_t failedSamples{0};
    uint32_t consecutiveSuccesses{0};
    uint32_t consecutiveFailures{0};
  };

  AdaptiveChunkController();
  explicit AdaptiveChunkController(Settings settings);

  bool isEnabled() const;
  uint64_t suggestedChunkSize(uint64_t remainingBytes = 0) const;
  void recordSuccess(uint64_t bytesSent, std::chrono::milliseconds ackTime);
  void recordFailure();
  Snapshot snapshot() const;

private:
  static uint64_t clamp(uint64_t value, uint64_t minValue, uint64_t maxValue);
  void normalizeSettings();

  mutable std::mutex mutex_;
  Settings settings_;
  uint64_t currentChunkSizeBytes_;
  double ewmaAckMillis_{0.0};
  double ewmaThroughputBytesPerSecond_{0.0};
  uint64_t successfulSamples_{0};
  uint64_t failedSamples_{0};
  uint32_t consecutiveSuccesses_{0};
  uint32_t consecutiveFailures_{0};
};
