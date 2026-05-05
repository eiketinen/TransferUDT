#include "tests/TestSuites.h"

void runApplicationTests(TestStats& stats, const TestEnvironment& env) {
    runTest("Application - Database singleton returns same instance", [&]() {
        Database::initialize(env.dbPath.string());
        auto& instanceA = Database::getInstance();
        auto& instanceB = Database::getInstance();

        require(&instanceA == &instanceB, "Database::getInstance should always return the same singleton instance.");
    }, stats);

    runTest("Application - singleton init/destructor smoke", [&]() {
        Database::initialize(env.dbPath.string());
        auto& singleton = Database::getInstance();

        const std::filesystem::path smokePath = env.tempRoot / "app_singleton" / "singleton_smoke.bin";
        singleton.markFileAsProcessing(smokePath);
        require(singleton.isFileProcessing(smokePath), "Singleton database should accept operations.");
    }, stats);
}