#include "kv_test/result_writer.h"
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace UC::KVTest {

namespace {

constexpr int kExitInvalidArgument = 1;

constexpr std::uint64_t kDefaultRealtimeFileMaxBytes = 100ULL * 1024ULL * 1024ULL;

const char* kRealtimeCsvHeader =
    "timestamp_sec,op,bandwidth_value,bandwidth_unit,iops_value,iops_unit,avg_latency_value,"
    "avg_latency_unit,error_count\n";
const char* kLatencyCsvHeader =
    "op,avg_value,avg_unit,min_value,min_unit,max_value,max_unit,p99_9_value,p99_9_unit,"
    "p99_99_value,p99_99_unit,p99_999_value,p99_999_unit\n";

std::string JsonEscape(const std::string& value)
{
    std::ostringstream stream;
    for (const auto ch : value) {
        switch (ch) {
            case '\\': stream << "\\\\"; break;
            case '"': stream << "\\\""; break;
            case '\b': stream << "\\b"; break;
            case '\f': stream << "\\f"; break;
            case '\n': stream << "\\n"; break;
            case '\r': stream << "\\r"; break;
            case '\t': stream << "\\t"; break;
            default:
                if (static_cast<unsigned char>(ch) < 0x20) {
                    stream << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                           << static_cast<int>(static_cast<unsigned char>(ch)) << std::dec
                           << std::setfill(' ');
                } else {
                    stream << ch;
                }
                break;
        }
    }
    return stream.str();
}

std::string JsonString(const std::string& value) { return "\"" + JsonEscape(value) + "\""; }

std::string FormatLocalTimestamp(const char* format)
{
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm localTime{};
#ifdef _WIN32
    localtime_s(&localTime, &time);
#else
    localtime_r(&time, &localTime);
#endif

    std::ostringstream stream;
    stream << std::put_time(&localTime, format);
    return stream.str();
}

std::string CommandTypeName(CommandType command)
{
    switch (command) {
        case CommandType::CONNECT: return "connect";
        case CommandType::CONFIG_CHECK: return "config check";
        case CommandType::STORE: return "store";
        case CommandType::RETRIEVE: return "retrieve";
        case CommandType::DELETE: return "delete";
        case CommandType::EXIST: return "exist";
        case CommandType::BATCH_STORE: return "batch-store";
        case CommandType::BATCH_RETRIEVE: return "batch-retrieve";
        case CommandType::POWER_CYCLE_PREPARE: return "power-cycle prepare";
        case CommandType::POWER_CYCLE_VERIFY: return "power-cycle verify";
        case CommandType::BENCH: return "bench";
        case CommandType::VERSION: return "version";
        case CommandType::UNKNOWN:
        default: return "unknown";
    }
}

std::string BenchOpTypeName(BenchOpType op)
{
    switch (op) {
        case BenchOpType::STORE: return "store";
        case BenchOpType::RETRIEVE: return "retrieve";
        case BenchOpType::BATCH_STORE: return "batch-store";
        case BenchOpType::BATCH_RETRIEVE: return "batch-retrieve";
        case BenchOpType::MIX: return "mix";
        case BenchOpType::UNKNOWN:
        default: return "unknown";
    }
}

std::string AsuStatusCodeName(UC::ASU::StatusCode code)
{
    switch (code) {
        case UC::ASU::StatusCode::OK: return "OK";
        case UC::ASU::StatusCode::INVALID_ARGUMENT: return "INVALID_ARGUMENT";
        case UC::ASU::StatusCode::NOT_INITIALIZED: return "NOT_INITIALIZED";
        case UC::ASU::StatusCode::TIMEOUT: return "TIMEOUT";
        case UC::ASU::StatusCode::NOT_FOUND: return "NOT_FOUND";
        case UC::ASU::StatusCode::PARTIAL_FAILED: return "PARTIAL_FAILED";
        case UC::ASU::StatusCode::CONNECTION_ERROR: return "CONNECTION_ERROR";
        case UC::ASU::StatusCode::IO_ERROR: return "IO_ERROR";
        case UC::ASU::StatusCode::BUFFER_NOT_REGISTERED: return "BUFFER_NOT_REGISTERED";
        case UC::ASU::StatusCode::BUFFER_NOT_SUPPORTED: return "BUFFER_NOT_SUPPORTED";
        case UC::ASU::StatusCode::TASK_NOT_FOUND: return "TASK_NOT_FOUND";
        case UC::ASU::StatusCode::RESOURCE_BUSY: return "RESOURCE_BUSY";
        case UC::ASU::StatusCode::UNSUPPORTED: return "UNSUPPORTED";
        case UC::ASU::StatusCode::IN_PROGRESS: return "IN_PROGRESS";
        case UC::ASU::StatusCode::INTERNAL_ERROR: return "INTERNAL_ERROR";
        case UC::ASU::StatusCode::CANCELED: return "CANCELED";
        default: return "UNKNOWN";
    }
}

std::string ResultStatusName(const CommandResult& result)
{
    if (!result.status.Ok()) { return "failed"; }
    if (result.taskResult.status.code == UC::ASU::StatusCode::PARTIAL_FAILED) {
        return "partial_failed";
    }
    if (!result.taskResult.status.ok()) { return "failed"; }

    const auto failedEntry =
        std::find_if(result.taskResult.entryStatus.begin(), result.taskResult.entryStatus.end(),
                     [](const UC::ASU::Status& status) { return !status.ok(); });
    return failedEntry == result.taskResult.entryStatus.end() ? "success" : "partial_failed";
}

std::uint64_t KeyCount(const CommandOptions& options, const CommandResult& result)
{
    if (!options.keys.empty()) { return options.keys.size(); }
    if (options.keyStartSet && options.keyEndSet) { return options.keyEnd - options.keyStart + 1; }
    if (options.count != 0) { return options.count; }
    if (!result.taskResult.entryStatus.empty()) { return result.taskResult.entryStatus.size(); }
    if (!result.queryResult.exists.empty()) { return result.queryResult.exists.size(); }
    return 0;
}

std::filesystem::path BuildOutputPath(const std::string& outputDir, const std::string& fileName)
{
    return std::filesystem::path(outputDir) / fileName;
}

}  // namespace

Status ResultWriter::Open(const OutputConfig& config)
{
    Close();

    const auto baseDir =
        config.path.empty() ? std::filesystem::path(".") : std::filesystem::path(config.path);
    const auto runDir = baseDir / ("run-" + FormatLocalTimestamp("%Y%m%d-%H%M%S"));

    std::error_code errorCode;
    std::filesystem::create_directories(runDir, errorCode);
    if (errorCode) {
        return Status::Error(
            kExitInvalidArgument,
            "failed to create output directory " + runDir.string() + ": " + errorCode.message());
    }

    outputDir_ = runDir.string();
    realtimeFileMaxBytes_ = config.realtimeFileMaxBytes == 0 ? kDefaultRealtimeFileMaxBytes
                                                             : config.realtimeFileMaxBytes;
    realtimeFileIndex_ = 0;
    realtimeFileBytes_ = 0;

    latencyFile_.open(BuildOutputPath(outputDir_, "latency.csv"));
    if (!latencyFile_.is_open()) {
        return Status::Error(kExitInvalidArgument, "failed to open latency.csv");
    }
    latencyFile_ << kLatencyCsvHeader;

    consistencyErrorFile_.open(BuildOutputPath(outputDir_, "consistency_errors.jsonl"));
    if (!consistencyErrorFile_.is_open()) {
        return Status::Error(kExitInvalidArgument, "failed to open consistency_errors.jsonl");
    }

    return OpenRealtimeFile();
}

Status ResultWriter::WriteSummary(const CommandOptions& options, const CommandResult& result)
{
    if (outputDir_.empty()) {
        return Status::Error(kExitInvalidArgument, "ResultWriter is not open");
    }

    const auto commandName = CommandTypeName(options.command);
    const auto opName =
        options.command == CommandType::BENCH ? BenchOpTypeName(options.benchOp) : commandName;
    const auto statusName = ResultStatusName(result);
    const auto exitCode = result.status.Ok() ? 0 : result.status.code;
    const auto keyCount = KeyCount(options, result);
    const auto asuStatusCode = AsuStatusCodeName(result.taskResult.status.code);
    const auto summaryTime = FormatLocalTimestamp("%Y-%m-%dT%H:%M:%S%z");

    std::ofstream textFile{BuildOutputPath(outputDir_, "summary.txt")};
    if (!textFile.is_open()) {
        return Status::Error(kExitInvalidArgument, "failed to open summary.txt");
    }
    textFile << "tool: kv-test\n"
             << "command: " << commandName << '\n'
             << "op: " << opName << '\n'
             << "status: " << statusName << '\n'
             << "exit_code: " << exitCode << '\n'
             << "configpath: " << options.configPath << '\n'
             << "key_count: " << keyCount << '\n'
             << "value_size: " << options.valueSize << '\n'
             << "batch_size: " << options.batchSize << '\n'
             << "concurrency: " << options.concurrency << '\n'
             << "duration_sec: " << options.durationSec << '\n'
             << "asu_status_code: " << asuStatusCode << '\n';
    if (!result.status.Ok()) { textFile << "error: " << result.status.message << '\n'; }
    if (!result.taskResult.status.message.empty()) {
        textFile << "asu_message: " << result.taskResult.status.message << '\n';
    }
    if (!textFile.good()) {
        return Status::Error(kExitInvalidArgument, "failed to write summary.txt");
    }

    std::ofstream jsonFile{BuildOutputPath(outputDir_, "summary.json")};
    if (!jsonFile.is_open()) {
        return Status::Error(kExitInvalidArgument, "failed to open summary.json");
    }

    jsonFile << "{\n"
             << "  \"tool\": \"kv-test\",\n"
             << "  \"command\": " << JsonString(commandName) << ",\n"
             << "  \"status\": " << JsonString(statusName) << ",\n"
             << "  \"exit_code\": " << exitCode << ",\n"
             << "  \"configpath\": " << JsonString(options.configPath) << ",\n"
             << "  \"start_time\": null,\n"
             << "  \"end_time\": " << JsonString(summaryTime) << ",\n"
             << "  \"connection\": {\n"
             << "    \"status\": " << JsonString(result.status.Ok() ? "success" : "failed") << ",\n"
             << "    \"asu_status_code\": " << JsonString(asuStatusCode) << "\n"
             << "  },\n"
             << "  \"request\": {\n"
             << "    \"op\": " << JsonString(opName) << ",\n"
             << "    \"key_count\": " << keyCount << ",\n"
             << "    \"value_size\": " << options.valueSize << ",\n"
             << "    \"batch_size\": " << options.batchSize << ",\n"
             << "    \"concurrency\": " << options.concurrency << ",\n"
             << "    \"duration_sec\": " << options.durationSec << "\n"
             << "  },\n";
    if (options.command == CommandType::BENCH) {
        jsonFile << "  \"metrics\": {\n"
                 << "    \"bandwidth\": {\"avg\": {\"value\": "
                 << (result.benchMetrics.avgBandwidthBytesPerSec / (1024.0 * 1024.0))
                 << ", \"unit\": \"MiB/s\"}, "
                    "\"realtime_file_pattern\": \"bench-realtime-*.csv\"},\n"
                 << "    \"iops\": {\"avg\": {\"value\": " << result.benchMetrics.avgIops
                 << ", \"unit\": \"1/s\"}, \"avg_batch\": {\"value\": "
                 << result.benchMetrics.avgBatchIops << ", \"unit\": \"1/s\"}},\n"
                 << "    \"latency\": {\"avg\": {\"value\": " << result.benchMetrics.latency.avgUs
                 << ", \"unit\": \"us\"}, \"min\": {\"value\": "
                 << result.benchMetrics.latency.minUs
                 << ", \"unit\": \"us\"}, \"max\": {\"value\": "
                 << result.benchMetrics.latency.maxUs
                 << ", \"unit\": \"us\"}, \"p99_9\": {\"value\": "
                 << result.benchMetrics.latency.p99_9Us
                 << ", \"unit\": \"us\"}, \"p99_99\": {\"value\": "
                 << result.benchMetrics.latency.p99_99Us
                 << ", \"unit\": \"us\"}, \"p99_999\": {\"value\": "
                 << result.benchMetrics.latency.p99_999Us << ", \"unit\": \"us\"}}\n"
                 << "  },\n";
    } else {
        jsonFile << "  \"metrics\": null,\n";
    }

    if (result.consistency.enabled) {
        const auto passRate = result.consistency.checked == 0
                                  ? 0.0
                                  : static_cast<double>(result.consistency.passed) /
                                        static_cast<double>(result.consistency.checked);
        jsonFile << "  \"consistency\": {\"enabled\": true, \"checked\": "
                 << result.consistency.checked << ", \"passed\": " << result.consistency.passed
                 << ", \"failed\": " << result.consistency.failed << ", \"pass_rate\": " << passRate
                 << ", \"key\": " << JsonString(result.consistency.key)
                 << ", \"expected\": " << JsonString(result.consistency.expected)
                 << ", \"actual\": " << JsonString(result.consistency.actual) << "},\n";
    } else {
        jsonFile << "  \"consistency\": " << (options.check ? "{\"enabled\": true}" : "null")
                 << ",\n";
    }
    if (result.status.Ok() && result.taskResult.status.ok()) {
        jsonFile << "  \"error\": null\n";
    } else {
        const auto message =
            !result.status.Ok() ? result.status.message : result.taskResult.status.message;
        jsonFile << "  \"error\": {\n"
                 << "    \"code\": " << JsonString(statusName) << ",\n"
                 << "    \"message\": " << JsonString(message) << ",\n"
                 << "    \"asu_status_code\": " << JsonString(asuStatusCode) << ",\n"
                 << "    \"retryable\": false\n"
                 << "  }\n";
    }
    jsonFile << "}\n";
    if (!jsonFile.good()) {
        return Status::Error(kExitInvalidArgument, "failed to write summary.json");
    }

    return Status::Success();
}

Status ResultWriter::WriteRealtimeSample(const std::string& csvLine)
{
    if (!realtimeFile_.is_open()) {
        return Status::Error(kExitInvalidArgument, "realtime CSV file is not open");
    }

    const auto lineBytes = csvLine.size() + 1;
    auto status = RollRealtimeFileIfNeeded(lineBytes);
    if (!status.Ok()) { return status; }

    realtimeFile_ << csvLine << '\n';
    if (!realtimeFile_.good()) {
        return Status::Error(kExitInvalidArgument, "failed to write realtime CSV sample");
    }
    realtimeFileBytes_ += lineBytes;
    return Status::Success();
}

Status ResultWriter::WriteLatencySample(const std::string& csvLine)
{
    if (!latencyFile_.is_open()) {
        return Status::Error(kExitInvalidArgument, "latency CSV file is not open");
    }

    latencyFile_ << csvLine << '\n';
    if (!latencyFile_.good()) {
        return Status::Error(kExitInvalidArgument, "failed to write latency CSV sample");
    }
    return Status::Success();
}

Status ResultWriter::WriteConsistencyError(const std::string& line)
{
    if (!consistencyErrorFile_.is_open()) {
        return Status::Error(kExitInvalidArgument, "consistency error file is not open");
    }

    consistencyErrorFile_ << line << '\n';
    if (!consistencyErrorFile_.good()) {
        return Status::Error(kExitInvalidArgument, "failed to write consistency error");
    }
    return Status::Success();
}

Status ResultWriter::Close()
{
    if (realtimeFile_.is_open()) { realtimeFile_.close(); }
    if (latencyFile_.is_open()) { latencyFile_.close(); }
    if (consistencyErrorFile_.is_open()) { consistencyErrorFile_.close(); }

    outputDir_.clear();
    realtimeFileBytes_ = 0;
    realtimeFileIndex_ = 0;
    return Status::Success();
}

Status ResultWriter::OpenRealtimeFile()
{
    if (realtimeFile_.is_open()) { realtimeFile_.close(); }

    const auto fileName = "bench-realtime-" + std::to_string(realtimeFileIndex_) + ".csv";
    realtimeFile_.open(BuildOutputPath(outputDir_, fileName));
    if (!realtimeFile_.is_open()) {
        return Status::Error(kExitInvalidArgument, "failed to open " + fileName);
    }

    realtimeFile_ << kRealtimeCsvHeader;
    if (!realtimeFile_.good()) {
        return Status::Error(kExitInvalidArgument, "failed to write " + fileName);
    }
    realtimeFileBytes_ = std::string(kRealtimeCsvHeader).size();
    return Status::Success();
}

Status ResultWriter::RollRealtimeFileIfNeeded(std::uint64_t incomingBytes)
{
    if (realtimeFileMaxBytes_ == 0 || realtimeFileBytes_ + incomingBytes <= realtimeFileMaxBytes_) {
        return Status::Success();
    }

    ++realtimeFileIndex_;
    return OpenRealtimeFile();
}

}  // namespace UC::KVTest
