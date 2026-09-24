#pragma once

#include "tabengine/platform.h"

#include <memory>

namespace tabengine {

[[nodiscard]] std::unique_ptr<IPlatform> make_win32_platform();

} // namespace tabengine

