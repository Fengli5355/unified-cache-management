#pragma once

#include <cstdint>
#include "status/status.h"

namespace UC::Tool {

Status GetPhysicalDeviceId(std::int32_t logicalDeviceId, std::int32_t& physicalDeviceId);

}  // namespace UC::Tool
