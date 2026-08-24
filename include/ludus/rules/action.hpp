#pragma once

#include "ludus/core/diagnostic.hpp"
#include "ludus/core/id.hpp"
#include "ludus/core/value.hpp"

#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace ludus {

class GameState;
class Transaction;

using ActionTarget = std::variant<EntityId, SpaceId, PlayerId>;

struct ActionIntent {
    ActionTypeId type;
    PlayerId issuer;
    std::optional<EntityId> actor;
    std::vector<ActionTarget> targets;
    PropertySet arguments;

    auto operator<=>(const ActionIntent&) const = default;
};

struct ActionDefinition {
    ActionTypeId type;
    std::int32_t priority{0};
    bool requires_actor{false};

    auto operator<=>(const ActionDefinition&) const = default;
};

class RuleContext {
  public:
    explicit RuleContext(const GameState& state) : state_(&state) {}

    [[nodiscard]] const GameState& state() const noexcept { return *state_; }

  private:
    const GameState* state_;
};

using ActionValidator =
    std::function<std::expected<void, Diagnostic>(const RuleContext&, const ActionIntent&)>;
using ActionResolver = std::function<std::expected<void, Diagnostic>(
    const RuleContext&, Transaction&, const ActionIntent&)>;
using ActionEnumerator = std::function<std::vector<ActionIntent>(const RuleContext&, PlayerId)>;

} // namespace ludus
