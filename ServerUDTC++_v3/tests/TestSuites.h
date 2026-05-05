#pragma once

#include "tests/TestSupport.h"

void runServerConfigTests(TestStats& stats, const TestEnvironment& env);
void runChunkPacketParserTests(TestStats& stats);
void runCircuitBreakerTests(TestStats& stats);
void runDatabaseTests(TestStats& stats, Database& db, const TestEnvironment& env);
void runFileReceiverTests(TestStats& stats, Database& db, const TestEnvironment& env);
void runSecureTransferIntegrationTests(TestStats& stats, Database& db, const TestEnvironment& env);
