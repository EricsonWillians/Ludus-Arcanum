#pragma once

#include "ludus/core/binary.hpp"
#include "ludus/core/diagnostic.hpp"
#include "ludus/core/entity_store.hpp"
#include "ludus/core/symbol.hpp"
#include "ludus/rules/effect.hpp"
#include "ludus/topology/topology.hpp"

#include <cstdint>
#include <expected>
#include <span>
#include <utility>
#include <vector>

namespace ludus {

class GameState {
  public:
    GameState() = default;
    GameState(SymbolRegistry symbols, Topology topology)
        : symbols_(std::move(symbols)), topology_(std::move(topology)) {}

    [[nodiscard]] const SymbolRegistry& symbols() const noexcept { return symbols_; }
    [[nodiscard]] const Topology& topology() const noexcept { return topology_; }
    [[nodiscard]] const EntityStore& entities() const noexcept { return entities_; }
    [[nodiscard]] const EffectStack& effect_stack() const noexcept { return effect_stack_; }

    [[nodiscard]] std::vector<std::byte> canonical_bytes() const;
    [[nodiscard]] std::uint64_t canonical_hash() const;
    [[nodiscard]] static std::expected<GameState, Diagnostic>
    from_canonical_bytes(std::span<const std::byte> bytes);

    auto operator<=>(const GameState&) const = default;

  private:
    SymbolRegistry symbols_;
    Topology topology_;
    EntityStore entities_;
    EffectStack effect_stack_;

    friend class Transaction;
    friend class GameSession;
};

} // namespace ludus
