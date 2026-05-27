#include "kv_test/kv_test_app.h"
#include "asu_client/asu_client.h"

namespace UC::KVTest {

namespace {

constexpr int kExitSuccess = 0;
constexpr int kExitInvalidArgument = 1;

int ToExitCode(const Status& status) { return status.Ok() ? kExitSuccess : status.code; }

}  // namespace

KvTestApp::KvTestApp() = default;

int KvTestApp::Run(int argc, char** argv)
{
    CommandOptions options;
    auto status = argParser_.Parse(argc, argv, options);
    if (!status.Ok()) { return ToExitCode(status); }

    KvTestConfig config;
    status = configLoader_.Load(options.configPath, config);
    if (!status.Ok()) { return ToExitCode(status); }

    status = configLoader_.MergeCommandOptions(options, config);
    if (!status.Ok()) { return ToExitCode(status); }

    status = hcommConfigAdapter_.ValidateChannelSource(config);
    if (!status.Ok()) { return ToExitCode(status); }

    status = resultWriter_.Open(config.output);
    if (!status.Ok()) { return ToExitCode(status); }

    CommandResult result;
    AsuClientRunner clientRunner(UC::ASU::CreateAsuClient());
    status = clientRunner.Init(config);
    if (status.Ok()) { status = RunCommand(options, config, clientRunner, result); }

    auto shutdownStatus = clientRunner.Shutdown();
    if (status.Ok() && !shutdownStatus.Ok()) { status = shutdownStatus; }

    result.status = status;
    auto writeStatus = resultWriter_.WriteSummary(options, result);
    if (status.Ok() && !writeStatus.Ok()) { status = writeStatus; }

    auto closeStatus = resultWriter_.Close();
    if (status.Ok() && !closeStatus.Ok()) { status = closeStatus; }

    return ToExitCode(status);
}

Status KvTestApp::RunCommand(const CommandOptions& options, const KvTestConfig& config,
                             AsuClientRunner& clientRunner, CommandResult& result)
{
    switch (options.command) {
        case CommandType::CONNECT: return Status::Success();
        case CommandType::STORE:
        case CommandType::BATCH_STORE:
        case CommandType::POWER_CYCLE_PREPARE:
            return RunStoreLikeCommand(options, config, clientRunner, result);
        case CommandType::RETRIEVE:
        case CommandType::BATCH_RETRIEVE:
        case CommandType::POWER_CYCLE_VERIFY:
            return RunRetrieveLikeCommand(options, config, clientRunner, result);
        case CommandType::DELETE: return RunDeleteCommand(options, config, clientRunner, result);
        case CommandType::EXIST: return RunExistCommand(options, config, clientRunner, result);
        case CommandType::BENCH: return benchRunner_.Run(options, config, clientRunner, result);
        case CommandType::UNKNOWN:
        default: return Status::Error(kExitInvalidArgument, "unknown kv-test command");
    }
}

Status KvTestApp::RunStoreLikeCommand(const CommandOptions& options, const KvTestConfig& config,
                                      AsuClientRunner& clientRunner, CommandResult& result)
{
    GeneratedData data;
    auto status = generator_.Generate(options, config, data);
    if (!status.Ok()) { return status; }

    BufferSet buffers;
    status = bufferAllocator_.BuildStoreBuffers(data, buffers);
    if (!status.Ok()) { return status; }

    status = clientRunner.RegisterBuffers(buffers);
    if (!status.Ok()) {
        auto unregisterStatus = clientRunner.UnregisterBuffers(buffers);
        if (unregisterStatus.Ok()) { return status; }
        return unregisterStatus;
    }

    const SubmitMode submitMode = options.command == CommandType::STORE
                                      ? SubmitMode::SINGLE_ENTRY_PER_CALL
                                      : SubmitMode::ALL_ENTRIES_IN_ONE_CALL;
    status = clientRunner.Store(buffers, submitMode, options.timeoutMs, result);

    auto unregisterStatus = clientRunner.UnregisterBuffers(buffers);
    if (status.Ok() && !unregisterStatus.Ok()) { status = unregisterStatus; }
    if (!status.Ok() || !options.check) { return status; }

    BufferSet retrievedBuffers;
    status = bufferAllocator_.BuildRetrieveBuffers(data, retrievedBuffers);
    if (!status.Ok()) { return status; }

    status = clientRunner.RegisterBuffers(retrievedBuffers);
    if (!status.Ok()) {
        auto retrieveUnregisterStatus = clientRunner.UnregisterBuffers(retrievedBuffers);
        if (retrieveUnregisterStatus.Ok()) { return status; }
        return retrieveUnregisterStatus;
    }

    CommandResult retrieveResult;
    status = clientRunner.Retrieve(retrievedBuffers, submitMode, options.timeoutMs, retrieveResult);
    if (status.Ok()) {
        status = consistencyChecker_.CheckStoreResult(data, retrievedBuffers, retrieveResult);
    }

    auto retrieveUnregisterStatus = clientRunner.UnregisterBuffers(retrievedBuffers);
    if (status.Ok() && !retrieveUnregisterStatus.Ok()) { status = retrieveUnregisterStatus; }
    return status;
}

Status KvTestApp::RunRetrieveLikeCommand(const CommandOptions& options, const KvTestConfig& config,
                                         AsuClientRunner& clientRunner, CommandResult& result)
{
    GeneratedData data;
    auto status = generator_.Generate(options, config, data);
    if (!status.Ok()) { return status; }

    BufferSet buffers;
    status = bufferAllocator_.BuildRetrieveBuffers(data, buffers);
    if (!status.Ok()) { return status; }

    status = clientRunner.RegisterBuffers(buffers);
    if (!status.Ok()) {
        auto unregisterStatus = clientRunner.UnregisterBuffers(buffers);
        if (unregisterStatus.Ok()) { return status; }
        return unregisterStatus;
    }

    const SubmitMode submitMode = options.command == CommandType::RETRIEVE
                                      ? SubmitMode::SINGLE_ENTRY_PER_CALL
                                      : SubmitMode::ALL_ENTRIES_IN_ONE_CALL;
    status = clientRunner.Retrieve(buffers, submitMode, options.timeoutMs, result);
    if (status.Ok() && options.check) {
        status = consistencyChecker_.CheckRetrieveResult(data, buffers, result);
    }

    auto unregisterStatus = clientRunner.UnregisterBuffers(buffers);
    if (status.Ok() && !unregisterStatus.Ok()) { status = unregisterStatus; }
    return status;
}

Status KvTestApp::RunDeleteCommand(const CommandOptions& options, const KvTestConfig& config,
                                   AsuClientRunner& clientRunner, CommandResult& result)
{
    GeneratedData data;
    auto status = generator_.Generate(options, config, data);
    if (!status.Ok()) { return status; }

    status = clientRunner.Delete(data.keys, options.timeoutMs, result);
    if (!status.Ok() || !options.check) { return status; }

    CommandResult existResult;
    status = clientRunner.Exist(data.keys, options.timeoutMs, existResult);
    if (!status.Ok()) { return status; }
    return consistencyChecker_.CheckDeleteResult(data.keys, result, existResult);
}

Status KvTestApp::RunExistCommand(const CommandOptions& options, const KvTestConfig& config,
                                  AsuClientRunner& clientRunner, CommandResult& result)
{
    GeneratedData data;
    auto status = generator_.Generate(options, config, data);
    if (!status.Ok()) { return status; }

    return clientRunner.Exist(data.keys, options.timeoutMs, result);
}

}  // namespace UC::KVTest
