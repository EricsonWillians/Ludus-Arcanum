#pragma once

#include "ludus/rules/event.hpp"
#include "ludus/rules/game_state.hpp"

#include <cstddef>
#include <span>
#include <string>

namespace ludus::studio {

[[nodiscard]] std::string inspect_state(const GameState& state);
[[nodiscard]] std::string inspect_event_log(std::span<const EventBatch> batches,
                                            std::size_t cursor,
                                            const SymbolRegistry& symbols);

} // namespace ludus::studio
