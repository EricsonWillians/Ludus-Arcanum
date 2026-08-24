#pragma once

#include "ludus/core/binary.hpp"
#include "ludus/core/diagnostic.hpp"
#include "ludus/core/id.hpp"
#include "ludus/core/value.hpp"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ludus {

struct ChoiceOption {
    std::uint32_t id{0U};
    std::string label;
    PropertySet arguments;

    auto operator<=>(const ChoiceOption&) const = default;
};

/// A native, serializable decision boundary. It never stores a Python continuation.
struct ChoiceWindow {
    std::uint64_t id{0U};
    PlayerId player;
    std::string prompt;
    std::vector<ChoiceOption> options;

    auto operator<=>(const ChoiceWindow&) const = default;
};

/// Value-only continuation data retained while an effect waits for a choice.
struct EffectRecord {
    std::uint64_t id{0U};
    ActionTypeId continuation;
    std::optional<EntityId> source;
    std::vector<EntityId> entity_targets;
    std::vector<SpaceId> space_targets;
    PropertySet arguments;

    auto operator<=>(const EffectRecord&) const = default;
};

class EffectStack {
  public:
    static constexpr std::size_t maximum_depth = 1'024U;
    static constexpr std::size_t maximum_targets = 4'096U;
    static constexpr std::size_t maximum_choice_options = 256U;
    static constexpr std::size_t maximum_arguments = 4'096U;
    static constexpr std::size_t maximum_text_length = 4'096U;

    [[nodiscard]] std::span<const EffectRecord> effects() const noexcept { return effects_; }
    [[nodiscard]] const EffectRecord* top() const noexcept {
        return effects_.empty() ? nullptr : &effects_.back();
    }
    [[nodiscard]] const std::optional<ChoiceWindow>& pending_choice() const noexcept {
        return pending_choice_;
    }

    void encode(BinaryWriter& writer) const;
    [[nodiscard]] static std::expected<EffectStack, Diagnostic> decode(BinaryReader& reader);

    auto operator<=>(const EffectStack&) const = default;

  private:
    std::vector<EffectRecord> effects_;
    std::optional<ChoiceWindow> pending_choice_;

    friend class GameSession;
    friend class Transaction;
};

void encode_effect_record(BinaryWriter& writer, const EffectRecord& effect);
[[nodiscard]] std::expected<EffectRecord, Diagnostic>
decode_effect_record(BinaryReader& reader);
void encode_choice_window(BinaryWriter& writer, const ChoiceWindow& choice);
[[nodiscard]] std::expected<ChoiceWindow, Diagnostic>
decode_choice_window(BinaryReader& reader);

} // namespace ludus
