#pragma once

#include "tests/TestSupport.h"

void runLoggerTests(TestStats& stats, const TestEnvironment& env);
void runAdaptiveChunkControllerTests(TestStats& stats);
void runRadarConfigTests(TestStats& stats, const TestEnvironment& env);
void runWatchedPathMapperTests(TestStats& stats, const TestEnvironment& env);
void runCircuitBreakerTests(TestStats& stats);
void runDatabaseTests(TestStats& stats, Database& db, const TestEnvironment& env);
void runNetworkManagerTests(TestStats& stats, Database& db, const TestEnvironment& env);
void runMemoryManagerTests(TestStats& stats);
void runConnectionPoolTests(TestStats& stats);
void runApplicationTests(TestStats& stats, const TestEnvironment& env);
