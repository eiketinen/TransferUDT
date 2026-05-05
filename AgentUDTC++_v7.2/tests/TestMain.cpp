#include "tests/TestSuites.h"

#include "Database.h"

#include <iostream>

int main() {
    TestStats stats;

    TestEnvironment env;
    env.tempRoot = createTempRoot();
    env.logPath = env.tempRoot / "tests.log";
    env.dbPath = env.tempRoot / "tests.db";

    runLoggerTests(stats, env);
    runRadarConfigTests(stats, env);
    runCircuitBreakerTests(stats);

    Database::initialize(env.dbPath.string());
    auto& db = Database::getInstance();
    runNetworkManagerTests(stats, db, env);
    runDatabaseTests(stats, db, env);
    runMemoryManagerTests(stats);
    runConnectionPoolTests(stats);
    runApplicationTests(stats, env);

    std::cout << "\nSummary: " << stats.passed << " passed, " << stats.failed << " failed.\n";
    return stats.failed == 0 ? 0 : 1;
}