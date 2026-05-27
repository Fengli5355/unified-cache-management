#pragma once

#include "kv_test/kv_test_types.h"

namespace UC::KVTest {

class KvTestConfigLoader {
public:
    // Loads the existing AsuClientConfig key-value format and derives kv-test fields.
    Status Load(const std::string& configPath, KvTestConfig& config) const;
    Status MergeCommandOptions(const CommandOptions& options, KvTestConfig& config) const;
};

}  // namespace UC::KVTest
