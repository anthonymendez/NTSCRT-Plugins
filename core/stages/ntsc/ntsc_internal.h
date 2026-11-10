// Private helpers of the NTSC stage (agent A), visible to tests.
#pragma once

#include <string>

#include "../../src/internal.h"

namespace ntscrt {
// The ntsc-rs settings JSON the stage actually loads for `look`: house
// defaults (ntsc-rs defaults + upstream AppState.appNtscDefaults) overlaid
// with look["ntsc"]["settings"], values coerced to ntsc-rs's JSON types.
std::string ntsc_effective_settings_json(const json &look);
}  // namespace ntscrt
