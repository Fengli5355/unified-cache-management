#include "device_id_helper.h"
#include <acl/acl.h>
#include <dlfcn.h>
#include <fmt/format.h>

namespace UC::Tool {
namespace {

using GetPhysicalDeviceIdFunc = aclError (*)(std::int32_t, std::int32_t*);

GetPhysicalDeviceIdFunc GetPhysicalDeviceIdFunction()
{
    return reinterpret_cast<GetPhysicalDeviceIdFunc>(
        dlsym(RTLD_DEFAULT, "aclrtGetPhyDevIdByLogicDevId"));
}

}  // namespace

Status GetPhysicalDeviceId(std::int32_t logicalDeviceId, std::int32_t& physicalDeviceId)
{
    static const auto getPhysicalDeviceId = GetPhysicalDeviceIdFunction();
    if (getPhysicalDeviceId == nullptr) {
        return Status::Error("aclrtGetPhyDevIdByLogicDevId is unavailable");
    }

    const auto ret = getPhysicalDeviceId(logicalDeviceId, &physicalDeviceId);
    if (ret == ACL_SUCCESS) { return Status::OK(); }
    return Status::Error(
        fmt::format("failed to resolve physical device ID from logical device ID({}), ret({})",
                    logicalDeviceId, ret));
}

}  // namespace UC::Tool
