#include "device_id_helper.h"

namespace UC::Tool {

Status GetPhysicalDeviceId(std::int32_t logicalDeviceId, std::int32_t& physicalDeviceId)
{
    physicalDeviceId = logicalDeviceId;
    return Status::OK();
}

}  // namespace UC::Tool
