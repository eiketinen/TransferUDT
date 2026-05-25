#include "tests/TestSuites.h"

#include "WatchedPathMapper.h"

#include <fstream>

namespace {

void writeFile(const std::filesystem::path &path, const std::string &content) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary);
  output << content;
}

} // namespace

void runWatchedPathMapperTests(TestStats &stats, const TestEnvironment &env) {
  runTest(
      "WatchedPathMapper - builds nested relative directory",
      [&]() {
        const auto root = env.tempRoot / "watch_root";
        const auto filePath = root / "XXX" / "XXXX" / "nomedoarquivo.bin";
        writeFile(filePath, "payload");

        std::string relativeDirectory;
        require(WatchedPathMapper::TryBuildRelativeDirectory(
                    filePath, {root.string()}, relativeDirectory),
                "Expected file inside watched root to map successfully.");
        require(relativeDirectory == "XXX/XXXX",
                "Expected nested relative directory to be preserved.");
      },
      stats);

  runTest(
      "WatchedPathMapper - direct child has empty relative directory",
      [&]() {
        const auto root = env.tempRoot / "watch_direct";
        const auto filePath = root / "nomedoarquivo.bin";
        writeFile(filePath, "payload");

        std::string relativeDirectory = "unchanged";
        require(WatchedPathMapper::TryBuildRelativeDirectory(
                    filePath, {root.string()}, relativeDirectory),
                "Expected direct child to map successfully.");
        require(relativeDirectory.empty(),
                "Direct child should not add an artificial directory.");
      },
      stats);

  runTest(
      "WatchedPathMapper - rejects files outside watched roots",
      [&]() {
        const auto root = env.tempRoot / "watch_outside_root";
        const auto outside = env.tempRoot / "outside" / "nomedoarquivo.bin";
        std::filesystem::create_directories(root);
        writeFile(outside, "payload");

        std::string relativeDirectory;
        require(!WatchedPathMapper::TryBuildRelativeDirectory(
                    outside, {root.string()}, relativeDirectory),
                "Expected outside file to be rejected.");
      },
      stats);
}
