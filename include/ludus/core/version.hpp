#pragma once

#include <string_view>

namespace ludus {

/// Returns the semantic version of the linked Ludus Arcanum runtime.
[[nodiscard]] std::string_view version() noexcept;

} // namespace ludus
