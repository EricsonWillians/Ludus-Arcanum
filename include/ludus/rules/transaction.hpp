#pragma once

#include "ludus/core/diagnostic.hpp"
#include "ludus/core/entity_store.hpp"
#include "ludus/core/id.hpp"
#include "ludus/core/value.hpp"
#include "ludus/rules/event.hpp"
#include "ludus/rules/game_state.hpp"
#include "ludus/rules/random.hpp"

#include <expected>
#include <optional>
#include <string_view>
#include <variant>
#include <vector>

namespace ludus {

namespace detail {

struct NoopPatch {
    auto operator<=>(const NoopPatch&) const = default;
};
struct SpawnPatch {
    EntitySnapshot entity;
    bool appended{false};
};
struct DestroyPatch {
    EntitySnapshot entity;
};
struct LocationPatch {
    EntityId entity;
    std::optional<SpaceId> before;
    std::optional<SpaceId> after;
};
struct OwnerPatch {
    EntityId entity;
    std::optional<PlayerId> before;
    std::optional<PlayerId> after;
};
struct PropertyPatch {
    EntityId entity;
    PropertyId property;
    std::optional<PropertyValue> before;
    std::optional<PropertyValue> after;
};
struct TagPatch {
    EntityId entity;
    TagId tag;
    bool before{false};
    bool after{false};
};
struct EffectPushPatch {
    EffectRecord effect;
};
struct EffectPopPatch {
    EffectRecord effect;
};
struct ChoicePatch {
    std::optional<ChoiceWindow> before;
    std::optional<ChoiceWindow> after;
};

using StatePatch =
    std::variant<NoopPatch, SpawnPatch, DestroyPatch, LocationPatch, OwnerPatch, PropertyPatch,
                 TagPatch, EffectPushPatch, EffectPopPatch, ChoicePatch>;

struct TransactionCommit {
    std::vector<StatePatch> patches;
    std::vector<Event> events;
};

} // namespace detail

struct SpawnOptions {
    std::optional<SpaceId> location;
    std::optional<PlayerId> owner;
    TagSet tags;
    PropertySet properties;
};

class Transaction {
  public:
    static constexpr std::size_t max_events = 65'536U;

    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    Transaction(Transaction&&) = delete;
    Transaction& operator=(Transaction&&) = delete;
    ~Transaction();

    [[nodiscard]] std::expected<EntityId, Diagnostic> spawn(SpawnOptions options = {});
    [[nodiscard]] std::expected<void, Diagnostic> destroy(EntityId entity);
    [[nodiscard]] std::expected<void, Diagnostic> move(EntityId entity,
                                                       std::optional<SpaceId> destination);
    [[nodiscard]] std::expected<void, Diagnostic> set_owner(EntityId entity,
                                                            std::optional<PlayerId> owner);
    [[nodiscard]] std::expected<void, Diagnostic> set_property(EntityId entity,
                                                               PropertyId property,
                                                               PropertyValue value);
    [[nodiscard]] std::expected<void, Diagnostic> erase_property(EntityId entity,
                                                                 PropertyId property);
    [[nodiscard]] std::expected<void, Diagnostic> add_tag(EntityId entity, TagId tag);
    [[nodiscard]] std::expected<void, Diagnostic> remove_tag(EntityId entity, TagId tag);
    [[nodiscard]] std::expected<void, Diagnostic> push_effect(EffectRecord effect);
    [[nodiscard]] std::expected<EffectRecord, Diagnostic> pop_effect(std::uint64_t expected_id);
    [[nodiscard]] std::expected<void, Diagnostic> request_choice(ChoiceWindow choice);
    [[nodiscard]] std::expected<ChoiceOption, Diagnostic>
    resolve_choice(std::uint64_t choice_id, std::uint32_t option_id);
    [[nodiscard]] std::expected<DiceResult, Diagnostic> roll(std::string_view expression,
                                                            std::string_view stream);

  private:
    Transaction(GameState& state, DeterministicRandom& random);
    [[nodiscard]] std::expected<detail::TransactionCommit, Diagnostic> commit();
    void rollback() noexcept;
    [[nodiscard]] std::unexpected<Diagnostic> reject(Diagnostic diagnostic);
    [[nodiscard]] bool has_event_capacity() const noexcept { return events_.size() < max_events; }

    GameState* state_;
    DeterministicRandom* random_;
    RandomSnapshot random_before_;
    std::vector<detail::StatePatch> patches_;
    std::vector<Event> events_;
    std::optional<Diagnostic> failure_;
    bool finished_{false};

    friend class GameSession;
};

} // namespace ludus
