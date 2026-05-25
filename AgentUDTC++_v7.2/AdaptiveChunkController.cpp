#include "AdaptiveChunkController.h"

#include <algorithm>

namespace {
constexpr double kEwmaAlpha = 0.2;
constexpr uint64_t kMinimumIncreaseStepBytes = 16ULL * 1024ULL;
} // namespace

AdaptiveChunkController::AdaptiveChunkController()
    : AdaptiveChunkController(Settings{}) {}

AdaptiveChunkController::AdaptiveChunkController(Settings settings)
    : settings_(settings), currentChunkSizeBytes_(settings.initialChunkSizeBytes) {
  normalizeSettings();
  currentChunkSizeBytes_ =
      clamp(currentChunkSizeBytes_, settings_.minChunkSizeBytes,
            settings_.maxChunkSizeBytes);
}

bool AdaptiveChunkController::isEnabled() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return settings_.enabled;
}

uint64_t AdaptiveChunkController::suggestedChunkSize(
    uint64_t remainingBytes) const {
  std::lock_guard<std::mutex> lock(mutex_);
  uint64_t suggested = currentChunkSizeBytes_;
  if (remainingBytes > 0) {
    if (remainingBytes < settings_.minChunkSizeBytes) {
      return remainingBytes;
    }
    suggested = std::min(suggested, remainingBytes);
  }
  return clamp(suggested, settings_.minChunkSizeBytes,
               settings_.maxChunkSizeBytes);
}

void AdaptiveChunkController::recordSuccess(
    uint64_t bytesSent, std::chrono::milliseconds ackTime) {
  if (bytesSent == 0) {
    return;
  }

  const auto ackMillis = std::max<int64_t>(ackTime.count(), 1);
  const double throughput =
      static_cast<double>(bytesSent) * 1000.0 / static_cast<double>(ackMillis);

  std::lock_guard<std::mutex> lock(mutex_);
  if (!settings_.enabled) {
    return;
  }

  if (successfulSamples_ == 0) {
    ewmaAckMillis_ = static_cast<double>(ackMillis);
    ewmaThroughputBytesPerSecond_ = throughput;
  } else {
    ewmaAckMillis_ =
        (kEwmaAlpha * static_cast<double>(ackMillis)) +
        ((1.0 - kEwmaAlpha) * ewmaAckMillis_);
    ewmaThroughputBytesPerSecond_ =
        (kEwmaAlpha * throughput) +
        ((1.0 - kEwmaAlpha) * ewmaThroughputBytesPerSecond_);
  }

  ++successfulSamples_;
  ++consecutiveSuccesses_;
  consecutiveFailures_ = 0;

  const int targetAckMillis = std::max(settings_.targetAckMillis, 1);
  if (ackMillis <= targetAckMillis) {
    const uint64_t additiveStep =
        std::max(kMinimumIncreaseStepBytes, currentChunkSizeBytes_ / 8U);
    currentChunkSizeBytes_ += additiveStep;
  } else if (ackMillis >= static_cast<int64_t>(targetAckMillis) * 2) {
    currentChunkSizeBytes_ /= 2U;
  } else {
    currentChunkSizeBytes_ =
        static_cast<uint64_t>(static_cast<double>(currentChunkSizeBytes_) *
                              0.875);
  }

  currentChunkSizeBytes_ =
      clamp(currentChunkSizeBytes_, settings_.minChunkSizeBytes,
            settings_.maxChunkSizeBytes);
}

void AdaptiveChunkController::recordFailure() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!settings_.enabled) {
    return;
  }

  ++failedSamples_;
  ++consecutiveFailures_;
  consecutiveSuccesses_ = 0;
  currentChunkSizeBytes_ /= 2U;
  currentChunkSizeBytes_ =
      clamp(currentChunkSizeBytes_, settings_.minChunkSizeBytes,
            settings_.maxChunkSizeBytes);
}

AdaptiveChunkController::Snapshot AdaptiveChunkController::snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return Snapshot{settings_.enabled,
                  currentChunkSizeBytes_,
                  ewmaAckMillis_,
                  ewmaThroughputBytesPerSecond_,
                  successfulSamples_,
                  failedSamples_,
                  consecutiveSuccesses_,
                  consecutiveFailures_};
}

uint64_t AdaptiveChunkController::clamp(uint64_t value, uint64_t minValue,
                                        uint64_t maxValue) {
  if (minValue > maxValue) {
    std::swap(minValue, maxValue);
  }
  return std::max(minValue, std::min(value, maxValue));
}

void AdaptiveChunkController::normalizeSettings() {
  if (settings_.minChunkSizeBytes == 0) {
    settings_.minChunkSizeBytes = 32ULL * 1024ULL;
  }

  if (settings_.maxChunkSizeBytes < settings_.minChunkSizeBytes) {
    settings_.maxChunkSizeBytes = settings_.minChunkSizeBytes;
  }

  if (settings_.initialChunkSizeBytes == 0) {
    settings_.initialChunkSizeBytes = settings_.minChunkSizeBytes;
  }

  if (settings_.targetAckMillis <= 0) {
    settings_.targetAckMillis = 700;
  }
}
