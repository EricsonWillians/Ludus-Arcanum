#pragma once

#include "ludus/render/snapshot.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ludus {

/// Package-neutral, value-only HUD records. They may cross the simulation/render
/// boundary but never contain callbacks, Python objects, or authoritative references.
struct UnitCardView {
    EntityId entity;
    std::string name;
    std::string role;
    std::int64_t health{0};
    std::int64_t maximum_health{0};
    std::int64_t action_points{0};
    std::optional<SpriteId> portrait;
    std::vector<std::string> statuses;

    auto operator<=>(const UnitCardView&) const = default;
};

struct InitiativeView {
    EntityId entity;
    std::string label;
    std::int64_t initiative{0};
    bool active{false};
    bool defeated{false};

    auto operator<=>(const InitiativeView&) const = default;
};

struct ObjectiveScoreView {
    std::string first_label;
    std::string second_label;
    std::int64_t first{0};
    std::int64_t second{0};
    std::int64_t target{0};

    auto operator<=>(const ObjectiveScoreView&) const = default;
};

struct AbilityView {
    std::uint64_t token{0U};
    std::string label;
    std::string description;
    std::int64_t action_point_cost{0};
    std::optional<SpriteId> icon;
    bool enabled{true};

    auto operator<=>(const AbilityView&) const = default;
};

struct CombatLogView {
    std::uint64_t sequence{0U};
    std::string text;
    Color color{0.82F, 0.84F, 0.9F, 1.0F};

    auto operator<=>(const CombatLogView&) const = default;
};

struct EndStateView {
    std::string title;
    std::string detail;
    bool draw{false};

    auto operator<=>(const EndStateView&) const = default;
};

enum class ParticipantSide : std::uint8_t { first, second };

struct ParticipantView {
    ParticipantSide side{ParticipantSide::first};
    std::string name;
    std::string subtitle;
    std::optional<SpriteId> crest;
    bool active{false};

    auto operator<=>(const ParticipantView&) const = default;
};

struct ClockView {
    ParticipantSide side{ParticipantSide::first};
    std::int64_t committed_remaining_milliseconds{0};
    std::int64_t increment_milliseconds{0};
    bool active{false};
    bool paused{false};
    bool expired{false};

    auto operator<=>(const ClockView&) const = default;
};

struct CapturedItemView {
    ParticipantSide captured_from{ParticipantSide::first};
    std::string label;
    std::int64_t count{0};
    std::int64_t material_value{0};
    std::optional<SpriteId> sprite;

    auto operator<=>(const CapturedItemView&) const = default;
};

struct TimelineEntryView {
    std::size_t ply{0U};
    std::uint32_t move_number{1U};
    ParticipantSide side{ParticipantSide::first};
    std::string notation;
    bool current{false};
    bool previewed{false};

    auto operator<=>(const TimelineEntryView&) const = default;
};

struct MatchControlView {
    bool can_undo{false};
    bool can_redo{false};
    bool can_resign{false};
    bool can_offer_draw{false};
    bool can_return_to_live{false};

    auto operator<=>(const MatchControlView&) const = default;
};

struct DrawClaimView {
    std::string label;
    std::optional<std::uint64_t> intended_action_token;

    auto operator<=>(const DrawClaimView&) const = default;
};

struct MatchResultView {
    std::string title;
    std::string reason;
    std::string result_token;
    bool draw{false};

    auto operator<=>(const MatchResultView&) const = default;
};

struct TextExportView {
    std::string format;
    std::string text;

    auto operator<=>(const TextExportView&) const = default;
};

/// One immutable publication unit for the board and surrounding package-neutral HUD.
struct PlayerView {
    RenderSnapshot render;
    std::vector<ParticipantView> participants;
    std::vector<ClockView> clocks;
    std::vector<CapturedItemView> captured_items;
    std::vector<TimelineEntryView> timeline;
    MatchControlView match_controls;
    std::vector<DrawClaimView> draw_claims;
    std::optional<MatchResultView> match_result;
    std::vector<TextExportView> text_exports;
    std::vector<UnitCardView> units;
    std::vector<InitiativeView> initiative;
    std::optional<ObjectiveScoreView> objective;
    std::vector<AbilityView> abilities;
    std::vector<CombatLogView> combat_log;
    std::optional<EndStateView> end_state;
};

} // namespace ludus
