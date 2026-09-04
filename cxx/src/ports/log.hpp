#pragma once
// =============================================================================
// Port: progress reporting. Stages emit one line per notable event.
// =============================================================================

#include <functional>
#include <string_view>

namespace abistudy::ports {

using Log = std::function<void(std::string_view)>;

} // namespace abistudy::ports
