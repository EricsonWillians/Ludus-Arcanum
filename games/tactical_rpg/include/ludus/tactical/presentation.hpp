#pragma once

#include "ludus/render/player_view.hpp"
#include "ludus/tactical/game.hpp"

#include <cstdint>
#include <expected>
#include <optional>

namespace ludus::tactical {

/// Builds a viewer-specific immutable projection; hidden entities never enter it.
class TacticalPresentation {
  public:
    [[nodiscard]] static std::expected<TacticalPresentation, Diagnostic>
    create(const TacticalGame& game);

    [[nodiscard]] std::expected<RenderSnapshot, Diagnostic>
    build(const TacticalGame& game, PlayerId viewer, std::uint64_t revision) const;
    [[nodiscard]] std::expected<PlayerView, Diagnostic>
    build_view(const TacticalGame& game, PlayerId viewer, std::uint64_t revision) const;

  private:
    TacticalPresentation(TagId unit, TagId card, TagId obstacle, TagId poisoned,
                         PropertyId kind,
                         PropertyId health, PropertyId q, PropertyId r,
                         PropertyId active_player, PropertyId round,
                         std::optional<PropertyId> active_unit,
                         std::optional<PropertyId> action_points,
                         std::optional<PropertyId> vanguard_score,
                         std::optional<PropertyId> raiders_score,
                         std::optional<PropertyId> outcome)
        : unit_(unit), card_(card), obstacle_(obstacle), poisoned_(poisoned), kind_(kind),
          health_(health), q_(q), r_(r), active_player_(active_player), round_(round),
          active_unit_(active_unit), action_points_(action_points),
          vanguard_score_(vanguard_score), raiders_score_(raiders_score), outcome_(outcome) {}

    TagId unit_;
    TagId card_;
    TagId obstacle_;
    TagId poisoned_;
    PropertyId kind_;
    PropertyId health_;
    PropertyId q_;
    PropertyId r_;
    PropertyId active_player_;
    PropertyId round_;
    std::optional<PropertyId> active_unit_;
    std::optional<PropertyId> action_points_;
    std::optional<PropertyId> vanguard_score_;
    std::optional<PropertyId> raiders_score_;
    std::optional<PropertyId> outcome_;
};

} // namespace ludus::tactical
