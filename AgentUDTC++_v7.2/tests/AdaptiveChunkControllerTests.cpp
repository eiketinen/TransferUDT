#include "tests/TestSuites.h"

#include "AdaptiveChunkController.h"

void runAdaptiveChunkControllerTests(TestStats &stats) {
  runTest(
      "AdaptiveChunkController - grows on fast ACK and shrinks on slow ACK",
      [&]() {
        AdaptiveChunkController controller({
            true,
            32ULL * 1024ULL,
            4ULL * 1024ULL * 1024ULL,
            256ULL * 1024ULL,
            700,
        });

        const uint64_t initial = controller.suggestedChunkSize();
        controller.recordSuccess(initial, std::chrono::milliseconds(100));
        const uint64_t afterFastAck = controller.suggestedChunkSize();
        require(afterFastAck > initial,
                "Fast ACK should increase the recommended chunk size.");

        controller.recordSuccess(afterFastAck, std::chrono::milliseconds(1600));
        const uint64_t afterSlowAck = controller.suggestedChunkSize();
        require(afterSlowAck < afterFastAck,
                "Slow ACK should decrease the recommended chunk size.");

        const auto snapshot = controller.snapshot();
        require(snapshot.successfulSamples == 2,
                "Successful samples should be tracked.");
        require(snapshot.ewmaAckMillis > 0.0,
                "EWMA ACK latency should be populated after success.");
        require(snapshot.ewmaThroughputBytesPerSecond > 0.0,
                "EWMA throughput should be populated after success.");
      },
      stats);

  runTest(
      "AdaptiveChunkController - failure halves recommendation without crossing minimum",
      [&]() {
        AdaptiveChunkController controller({
            true,
            64ULL * 1024ULL,
            4ULL * 1024ULL * 1024ULL,
            128ULL * 1024ULL,
            700,
        });

        controller.recordFailure();
        require(controller.suggestedChunkSize() == 64ULL * 1024ULL,
                "Failure should clamp recommendation to the minimum.");

        controller.recordFailure();
        require(controller.suggestedChunkSize() == 64ULL * 1024ULL,
                "Repeated failures must not go below the minimum.");

        const auto snapshot = controller.snapshot();
        require(snapshot.failedSamples == 2,
                "Failed samples should be tracked.");
        require(snapshot.consecutiveFailures == 2,
                "Consecutive failures should be tracked.");
      },
      stats);

  runTest(
      "AdaptiveChunkController - disabled mode leaves recommendation stable",
      [&]() {
        AdaptiveChunkController controller({
            false,
            32ULL * 1024ULL,
            4ULL * 1024ULL * 1024ULL,
            256ULL * 1024ULL,
            700,
        });

        const uint64_t initial = controller.suggestedChunkSize();
        controller.recordSuccess(initial, std::chrono::milliseconds(10));
        controller.recordFailure();
        require(controller.suggestedChunkSize() == initial,
                "Disabled controller should not mutate recommendations.");
      },
      stats);
}
