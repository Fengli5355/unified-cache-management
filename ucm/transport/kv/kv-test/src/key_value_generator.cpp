#include "kv_test/key_value_generator.h"
#include <iomanip>
#include <limits>
#include <sstream>

namespace UC::KVTest {

namespace {

constexpr int kExitInvalidArgument = 1;

constexpr std::uint64_t kFnvOffsetBasis64 = 14695981039346656037ULL;
constexpr std::uint64_t kFnvPrime64 = 1099511628211ULL;
constexpr std::uint64_t kSplitMixIncrement = 0x9E3779B97F4A7C15ULL;
constexpr std::uint64_t kCrc64EcmaPolynomial = 0x42F0E1EBA9EA3693ULL;

std::uint64_t HashByte(std::uint64_t hash, std::uint8_t value)
{
    hash ^= value;
    hash *= kFnvPrime64;
    return hash;
}

std::uint64_t HashUint64(std::uint64_t hash, std::uint64_t value)
{
    for (std::uint32_t index = 0; index < 8; ++index) {
        hash = HashByte(hash, static_cast<std::uint8_t>((value >> (index * 8)) & 0xFFU));
    }
    return hash;
}

std::uint64_t HashString(std::uint64_t hash, const std::string& value)
{
    for (const char byte : value) { hash = HashByte(hash, static_cast<std::uint8_t>(byte)); }
    return hash;
}

std::uint64_t SplitMix64Next(std::uint64_t& state)
{
    state += kSplitMixIncrement;
    std::uint64_t value = state;
    value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ULL;
    value = (value ^ (value >> 27)) * 0x94D049BB133111EBULL;
    return value ^ (value >> 31);
}

std::vector<std::uint8_t> GenerateValueBytes(const UC::ASU::CacheKey& key, std::uint64_t seed,
                                             std::uint64_t valueSize)
{
    std::vector<std::uint8_t> value(static_cast<std::size_t>(valueSize));
    std::uint64_t state = HashString(HashUint64(kFnvOffsetBasis64, seed), key);
    for (std::size_t offset = 0; offset < value.size();) {
        const std::uint64_t random = SplitMix64Next(state);
        for (std::uint32_t byteIndex = 0; byteIndex < 8 && offset < value.size();
             ++byteIndex, ++offset) {
            value[offset] = static_cast<std::uint8_t>((random >> (byteIndex * 8)) & 0xFFU);
        }
    }
    return value;
}

}  // namespace

Status KeyValueGenerator::Generate(const CommandOptions& options, const KvTestConfig& config,
                                   GeneratedData& data) const
{
    const std::uint64_t count = options.count == 0 ? config.count : options.count;
    const std::uint64_t seed = options.seed == 0 ? config.seed : options.seed;
    const std::uint64_t valueSize = options.valueSize == 0 ? config.valueSize : options.valueSize;

    if (valueSize > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        return Status::Error(kExitInvalidArgument, "value-size exceeds addressable memory");
    }

    data.keys.clear();
    data.values.clear();

    if (!options.keys.empty()) {
        data.keys.reserve(options.keys.size());
        for (const auto& key : options.keys) {
            if (key.empty()) { return Status::Error(kExitInvalidArgument, "key cannot be empty"); }
            data.keys.push_back(key);
        }
    } else {
        if (count > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
            return Status::Error(kExitInvalidArgument, "count exceeds addressable memory");
        }
        data.keys.reserve(static_cast<std::size_t>(count));
        for (std::uint64_t index = 0; index < count; ++index) {
            data.keys.push_back(config.keyPrefix + std::to_string(index));
        }
    }

    data.values.reserve(data.keys.size());
    for (const auto& key : data.keys) {
        data.values.push_back(GenerateValueBytes(key, seed, valueSize));
    }
    return Status::Success();
}

Status KeyValueGenerator::Digest(const std::vector<std::uint8_t>& value, std::string& digest) const
{
    std::uint64_t crc = 0;
    for (const std::uint8_t byte : value) {
        crc ^= static_cast<std::uint64_t>(byte) << 56;
        for (std::uint32_t bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x8000000000000000ULL) != 0 ? (crc << 1) ^ kCrc64EcmaPolynomial : crc << 1;
        }
    }

    std::ostringstream output;
    output << std::hex << std::nouppercase << std::setfill('0') << std::setw(16) << crc;
    digest = output.str();
    return Status::Success();
}

}  // namespace UC::KVTest
