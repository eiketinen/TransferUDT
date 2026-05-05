#include "tests/TestSuites.h"

#include "Logger.h"

void runLoggerTests(TestStats &stats, const TestEnvironment &env) {
  runTest(
      "Logger - getInstance throws before initialization",
      [&]() {
        // Logger::Initialize has NOT been called yet at this point in TestMain.
        bool threw = false;
        try {
          Logger::getInstance();
        } catch (const std::runtime_error &) {
          threw = true;
        }
        require(threw,
                "getInstance() must throw before Initialize() is called.");
      },
      stats);

  runTest(
      "Logger - Initialize and basic logging",
      [&]() {
        Logger::Initialize(env.logPath.string(), 1024 * 1024, 1, "always");

        // After init, getInstance must not throw.
        bool ok = true;
        try {
          Logger::getInstance();
        } catch (...) {
          ok = false;
        }
        require(ok, "getInstance() must succeed after Initialize().");

        // Exercise the plain-string API.
        Logger::getInstance().debug("UnitTest", "debug message");
        Logger::getInstance().info("UnitTest", "info message");
        Logger::getInstance().warning("UnitTest", "warning message");
        Logger::getInstance().error("UnitTest", "error message");
        Logger::getInstance().critical("UnitTest", "critical message");

        // Exercise the safe* static helpers.
        Logger::safeDebug("UnitTest", "safeDebug after init");
        Logger::safeInfo("UnitTest", "safeInfo after init");
        Logger::safeWarning("UnitTest", "safeWarning after init");
        Logger::safeError("UnitTest", "safeError after init");
      },
      stats);

  runTest(
      "Logger - duplicate Initialize is ignored",
      [&]() {
        // A second call to Initialize must be silently ignored (call_once).
        Logger::Initialize("other_path.log");
        // The original logger should still work.
        Logger::getInstance().info("UnitTest",
                                   "still works after duplicate init");
      },
      stats);

  runTest(
      "Logger - setMinLevel",
      [&]() {
        Logger::getInstance().setMinLevel(Logger::Level::WARNING);
        Logger::getInstance().warning("UnitTest", "this should appear");
        // Reset for subsequent tests.
        Logger::getInstance().setMinLevel(Logger::Level::DEBUG);
      },
      stats);

  runTest(
      "Logger - template format API",
      [&]() {
        Logger::info("UnitTest", "formatted value: {}", 42);
        Logger::error("UnitTest", "formatted error: {} {}", "hello", 100);
        Logger::debug("UnitTest", "debug {}", "ok");
        Logger::warning("UnitTest", "warn {}", true);
        Logger::critical("UnitTest", "critical {}", 3.14);
      },
      stats);

  runTest(
      "Logger - context template API (C suffix)",
      [&]() {
        Logger::infoC("UnitTest", "myfile.bin", "chunk {} of {}", 1, 10);
        Logger::errorC("UnitTest", "myfile.bin", "failed at chunk {}", 5);
        Logger::debugC("UnitTest", "myfile.bin", "debug ctx {}", "ok");
        Logger::warningC("UnitTest", "myfile.bin", "warn ctx {}", true);
        Logger::criticalC("UnitTest", "myfile.bin", "critical ctx {}", 0);
      },
      stats);
}