#include "ludus/rules/session.hpp"

#include <algorithm>
#include <concepts>
#include <exception>
#include <limits>
#include <string>
#include <type_traits>
#include <utility>

namespace ludus {
namespace {

constexpr std::string_view archive_magic = "LUDUS-SAVE";
constexpr std::string_view state_magic = "LUDUS-STATE";

void write_id(BinaryWriter& writer, auto id) {
    writer.u32(id.index());
    writer.u32(id.generation());
}

template <typename Id>
Id read_id(BinaryReader& reader) {
    return Id{reader.u32(), reader.u32()};
}

void write_value(BinaryWriter& writer, const PropertyValue& value) {
    writer.u8(static_cast<std::uint8_t>(value.index()));
    std::visit(
        [&writer](const auto& typed) {
            using T = std::remove_cvref_t<decltype(typed)>;
            if constexpr (std::same_as<T, bool>) {
                writer.boolean(typed);
            } else if constexpr (std::same_as<T, std::int64_t>) {
                writer.i64(typed);
            } else if constexpr (std::same_as<T, Fixed>) {
                writer.i64(typed.raw());
            } else {
                writer.string(typed);
            }
        },
        value);
}

std::expected<PropertyValue, Diagnostic> read_value(BinaryReader& reader) {
    switch (reader.u8()) {
    case 0:
        return PropertyValue{reader.boolean()};
    case 1:
        return PropertyValue{reader.i64()};
    case 2:
        return PropertyValue{Fixed::from_raw(reader.i64())};
    case 3:
        return PropertyValue{reader.string()};
    default:
        return std::unexpected(Diagnostic{DiagnosticCode::serialization_error,
                                          "unknown action argument value kind", {}});
    }
}

void write_properties(BinaryWriter& writer, const PropertySet& properties) {
    writer.u64(static_cast<std::uint64_t>(properties.entries().size()));
    for (const auto& property : properties.entries()) {
        writer.u32(property.id.value());
        write_value(writer, property.value);
    }
}

std::expected<PropertySet, Diagnostic> read_properties(BinaryReader& reader) {
    PropertySet result;
    const auto count = reader.u64();
    for (std::uint64_t index = 0; index < count && reader.ok(); ++index) {
        const PropertyId property{reader.u32()};
        auto value = read_value(reader);
        if (!property.valid() || !value || result.find(property) != nullptr) {
            return std::unexpected(Diagnostic{DiagnosticCode::serialization_error,
                                              "invalid serialized action arguments", {}});
        }
        static_cast<void>(result.set(property, std::move(*value)));
    }
    return result;
}

void write_intent(BinaryWriter& writer, const ActionIntent& intent) {
    writer.u32(intent.type.value());
    write_id(writer, intent.issuer);
    writer.boolean(intent.actor.has_value());
    if (intent.actor) {
        write_id(writer, *intent.actor);
    }
    writer.u64(static_cast<std::uint64_t>(intent.targets.size()));
    for (const auto& target : intent.targets) {
        writer.u8(static_cast<std::uint8_t>(target.index()));
        std::visit([&writer](auto id) { write_id(writer, id); }, target);
    }
    write_properties(writer, intent.arguments);
}

std::expected<ActionIntent, Diagnostic> read_intent(BinaryReader& reader) {
    ActionIntent result;
    result.type = ActionTypeId{reader.u32()};
    result.issuer = read_id<PlayerId>(reader);
    if (reader.boolean()) {
        result.actor = read_id<EntityId>(reader);
    }
    const auto count = reader.u64();
    for (std::uint64_t index = 0; index < count && reader.ok(); ++index) {
        switch (reader.u8()) {
        case 0:
            result.targets.emplace_back(read_id<EntityId>(reader));
            break;
        case 1:
            result.targets.emplace_back(read_id<SpaceId>(reader));
            break;
        case 2:
            result.targets.emplace_back(read_id<PlayerId>(reader));
            break;
        default:
            return std::unexpected(Diagnostic{DiagnosticCode::serialization_error,
                                              "unknown serialized action target kind", {}});
        }
    }
    auto arguments = read_properties(reader);
    if (!arguments || !result.type.valid() || !result.issuer.valid()) {
        return std::unexpected(Diagnostic{DiagnosticCode::serialization_error,
                                          "invalid serialized action intent", {}});
    }
    result.arguments = std::move(*arguments);
    return result;
}

void write_snapshot(BinaryWriter& writer, const RandomSnapshot& snapshot) {
    writer.u64(static_cast<std::uint64_t>(snapshot.streams.size()));
    for (const auto& [name, stream] : snapshot.streams) {
        writer.string(name);
        writer.u64(stream.state);
        writer.u64(stream.increment);
        writer.u64(stream.draws);
    }
}

std::expected<RandomSnapshot, Diagnostic> read_snapshot(BinaryReader& reader) {
    RandomSnapshot result;
    const auto count = reader.u64();
    for (std::uint64_t index = 0; index < count && reader.ok(); ++index) {
        auto name = reader.string();
        RandomStreamState state{reader.u64(), reader.u64(), reader.u64()};
        if (name.empty() || (state.increment & 1U) == 0U || result.streams.contains(name)) {
            return std::unexpected(Diagnostic{DiagnosticCode::serialization_error,
                                              "invalid archived random snapshot", {}});
        }
        result.streams.emplace(std::move(name), state);
    }
    return result;
}

std::uint64_t hash_state_and_random(const GameState& state, const DeterministicRandom& random) {
    BinaryWriter writer;
    const auto state_bytes = state.canonical_bytes();
    writer.bytes(state_bytes);
    random.encode(writer);
    return canonical_hash(writer.data());
}

std::expected<std::uint64_t, Diagnostic>
legacy_hash_state_and_random(const GameState& state, const DeterministicRandom& random) {
    if (!state.effect_stack().effects().empty() || state.effect_stack().pending_choice()) {
        return std::unexpected(Diagnostic{
            DiagnosticCode::serialization_error,
            "version one archives cannot contain an effect stack", {}});
    }
    auto state_bytes = state.canonical_bytes();
    constexpr std::size_t version_offset = sizeof(std::uint64_t) + state_magic.size();
    constexpr std::size_t empty_effect_stack_size = sizeof(std::uint64_t) + 1U;
    if (state_bytes.size() < version_offset + sizeof(std::uint32_t) +
                                 empty_effect_stack_size) {
        return std::unexpected(Diagnostic{DiagnosticCode::serialization_error,
                                          "canonical state is too small to migrate", {}});
    }
    state_bytes[version_offset] = std::byte{1U};
    state_bytes[version_offset + 1U] = std::byte{0U};
    state_bytes[version_offset + 2U] = std::byte{0U};
    state_bytes[version_offset + 3U] = std::byte{0U};
    state_bytes.resize(state_bytes.size() - empty_effect_stack_size);

    BinaryWriter writer;
    writer.bytes(state_bytes);
    random.encode(writer);
    return canonical_hash(writer.data());
}

bool properties_are_interned(const PropertySet& properties, const GameState& state) {
    return std::ranges::all_of(properties.entries(), [&state](const PropertyEntry& property) {
        return property.id.valid() && property.id.value() <= state.symbols().properties.size();
    });
}

bool effect_is_applicable(const EffectRecord& effect, const GameState& state) {
    const auto& stack = state.effect_stack();
    return effect.id != 0U && effect.continuation.valid() &&
           effect.continuation.value() <= state.symbols().actions.size() &&
           effect.entity_targets.size() <= EffectStack::maximum_targets &&
           effect.space_targets.size() <= EffectStack::maximum_targets &&
           effect.arguments.entries().size() <= EffectStack::maximum_arguments &&
           (!effect.source || state.entities().contains(*effect.source)) &&
           std::ranges::all_of(effect.entity_targets, [&state](EntityId target) {
               return state.entities().contains(target);
           }) &&
           std::ranges::all_of(effect.space_targets, [&state](SpaceId target) {
               return state.topology().contains(target);
           }) &&
           properties_are_interned(effect.arguments, state) &&
           std::ranges::none_of(stack.effects(), [&effect](const EffectRecord& current) {
               return current.id == effect.id;
           });
}

bool choice_is_applicable(const ChoiceWindow& choice, const GameState& state) {
    const auto* top = state.effect_stack().top();
    if (top == nullptr || choice.id == 0U || choice.id != top->id || !choice.player.valid() ||
        choice.prompt.empty() || choice.prompt.size() > EffectStack::maximum_text_length ||
        choice.options.empty() || choice.options.size() > EffectStack::maximum_choice_options) {
        return false;
    }
    std::vector<std::uint32_t> ids;
    ids.reserve(choice.options.size());
    for (const auto& option : choice.options) {
        if (option.id == 0U || option.label.empty() ||
            option.label.size() > EffectStack::maximum_text_length ||
            option.arguments.entries().size() > EffectStack::maximum_arguments ||
            !properties_are_interned(option.arguments, state) ||
            std::ranges::find(ids, option.id) != ids.end()) {
            return false;
        }
        ids.push_back(option.id);
    }
    return true;
}

} // namespace

GameSession::GameSession(GameState initial_state, std::uint64_t seed)
    : state_(std::move(initial_state)), random_(seed), initial_state_(state_.canonical_bytes()) {
    checkpoints_.push_back(Checkpoint{0U, initial_state_, random_.snapshot()});
}

std::uint64_t GameSession::state_hash() const { return hash_state_and_random(state_, random_); }

std::expected<void, Diagnostic>
GameSession::define_action(ActionDefinition definition, ActionValidator validator,
                           ActionResolver resolver, ActionEnumerator enumerator) {
    if (!definition.type.valid() || definition.type.value() > state_.symbols_.actions.size()) {
        return std::unexpected(Diagnostic{DiagnosticCode::invalid_argument,
                                          "action definition type is not interned", {}});
    }
    if (!resolver) {
        return std::unexpected(Diagnostic{DiagnosticCode::invalid_argument,
                                          "action definition requires a resolver", {}});
    }
    if (actions_.contains(definition.type)) {
        return std::unexpected(Diagnostic{DiagnosticCode::invalid_argument,
                                          "action type is already defined", {}});
    }
    actions_.emplace(definition.type,
                     RegisteredAction{definition, std::move(validator), std::move(resolver),
                                      std::move(enumerator)});
    return {};
}

std::expected<void, Diagnostic> GameSession::validate(const ActionIntent& intent) const {
    const auto found = actions_.find(intent.type);
    if (found == actions_.end()) {
        return std::unexpected(
            Diagnostic{DiagnosticCode::unknown_action, "action type is not registered", {}});
    }
    if (!intent.issuer.valid()) {
        return std::unexpected(
            Diagnostic{DiagnosticCode::validation_failed, "action issuer is invalid", {}});
    }
    if (found->second.definition.requires_actor && !intent.actor) {
        return std::unexpected(
            Diagnostic{DiagnosticCode::validation_failed, "action requires an actor", {}});
    }
    if (intent.actor && !state_.entities_.contains(*intent.actor)) {
        return std::unexpected(Diagnostic{DiagnosticCode::validation_failed,
                                          "action actor is stale or invalid", {}});
    }
    for (const auto& target : intent.targets) {
        const bool valid = std::visit(
            [this](auto id) {
                using T = decltype(id);
                if constexpr (std::same_as<T, EntityId>) {
                    return state_.entities_.contains(id);
                } else if constexpr (std::same_as<T, SpaceId>) {
                    return state_.topology_.contains(id);
                } else {
                    return id.valid();
                }
            },
            target);
        if (!valid) {
            return std::unexpected(Diagnostic{DiagnosticCode::validation_failed,
                                              "action contains a stale or invalid target", {}});
        }
    }
    for (const auto& argument : intent.arguments.entries()) {
        if (argument.id.value() > state_.symbols_.properties.size()) {
            return std::unexpected(Diagnostic{DiagnosticCode::validation_failed,
                                              "action argument name is not interned", {}});
        }
    }
    if (found->second.validator) {
        return found->second.validator(RuleContext{state_}, intent);
    }
    return {};
}

std::vector<ActionIntent> GameSession::legal_actions(PlayerId player) const {
    std::vector<ActionIntent> result;
    const RuleContext context{state_};
    for (const auto& [type, action] : actions_) {
        static_cast<void>(type);
        if (!action.enumerator) {
            continue;
        }
        auto candidates = action.enumerator(context, player);
        for (auto& candidate : candidates) {
            if (validate(candidate)) {
                result.push_back(std::move(candidate));
                if (result.size() == max_legal_actions) {
                    return result;
                }
            }
        }
    }
    return result;
}

std::expected<EventBatch, Diagnostic> GameSession::submit(const ActionIntent& intent) {
    if (cursor_ >= max_history_entries) {
        return std::unexpected(Diagnostic{DiagnosticCode::invalid_state,
                                          "session history limit exceeded", {}});
    }
    if (auto checked = validate(intent); !checked) {
        return std::unexpected(checked.error());
    }
    const auto action = actions_.find(intent.type);
    const auto random_before = random_.snapshot();
    Transaction transaction{state_, random_};
    try {
        const auto resolved = action->second.resolver(RuleContext{state_}, transaction, intent);
        if (!resolved) {
            static_cast<void>(transaction.reject(resolved.error()));
        }
    } catch (const std::exception& error) {
        static_cast<void>(transaction.reject(Diagnostic{
            DiagnosticCode::transaction_failed,
            std::string{"C++ rule resolver threw an exception: "} + error.what(), {}}));
    } catch (...) {
        static_cast<void>(transaction.reject(Diagnostic{
            DiagnosticCode::transaction_failed, "C++ rule resolver threw an unknown exception", {}}));
    }

    auto committed = transaction.commit();
    if (!committed) {
        return std::unexpected(committed.error());
    }
    truncate_redo();
    for (auto& event : committed->events) {
        event.sequence = next_event_sequence_++;
    }
    EventBatch batch{std::move(committed->events), state_hash()};
    history_.push_back(HistoryEntry{intent, batch, std::move(committed->patches), random_before,
                                    random_.snapshot()});
    cursor_ = history_.size();
    maybe_checkpoint();
    return batch;
}

void GameSession::apply_patch(GameState& state, const detail::StatePatch& patch, bool forward) {
    std::visit(
        [&state, forward](const auto& typed) {
            using T = std::remove_cvref_t<decltype(typed)>;
            if constexpr (std::same_as<T, detail::NoopPatch>) {
                return;
            } else if constexpr (std::same_as<T, detail::SpawnPatch>) {
                if (forward) {
                    state.entities_.redo_spawn(typed.entity, typed.appended);
                } else {
                    state.entities_.undo_spawn(typed.entity.id, typed.appended);
                }
            } else if constexpr (std::same_as<T, detail::DestroyPatch>) {
                if (forward) {
                    state.entities_.redo_destroy(typed.entity.id);
                } else {
                    state.entities_.undo_destroy(typed.entity);
                }
            } else if constexpr (std::same_as<T, detail::LocationPatch>) {
                state.entities_.slots_[typed.entity.index()].location =
                    forward ? typed.after : typed.before;
            } else if constexpr (std::same_as<T, detail::OwnerPatch>) {
                state.entities_.slots_[typed.entity.index()].owner = forward ? typed.after : typed.before;
            } else if constexpr (std::same_as<T, detail::PropertyPatch>) {
                auto& properties = state.entities_.slots_[typed.entity.index()].properties;
                const auto& value = forward ? typed.after : typed.before;
                if (value) {
                    static_cast<void>(properties.set(typed.property, *value));
                } else {
                    static_cast<void>(properties.erase(typed.property));
                }
            } else if constexpr (std::same_as<T, detail::TagPatch>) {
                auto& tags = state.entities_.slots_[typed.entity.index()].tags;
                const bool desired = forward ? typed.after : typed.before;
                if (desired) {
                    static_cast<void>(tags.add(typed.tag));
                } else {
                    static_cast<void>(tags.remove(typed.tag));
                }
            } else if constexpr (std::same_as<T, detail::EffectPushPatch>) {
                if (forward) {
                    state.effect_stack_.effects_.push_back(typed.effect);
                } else {
                    state.effect_stack_.effects_.pop_back();
                }
            } else if constexpr (std::same_as<T, detail::EffectPopPatch>) {
                if (forward) {
                    state.effect_stack_.effects_.pop_back();
                } else {
                    state.effect_stack_.effects_.push_back(typed.effect);
                }
            } else if constexpr (std::same_as<T, detail::ChoicePatch>) {
                state.effect_stack_.pending_choice_ = forward ? typed.after : typed.before;
            }
        },
        patch);
}

std::expected<detail::StatePatch, Diagnostic> GameSession::apply_event(GameState& state,
                                                                      const Event& event) {
    return std::visit(
        [&state](const auto& payload) -> std::expected<detail::StatePatch, Diagnostic> {
            using T = std::remove_cvref_t<decltype(payload)>;
            if constexpr (std::same_as<T, EntitySpawned>) {
                const auto id = payload.entity.id;
                const bool appended = id.index() == state.entities_.slots_.size();
                if (!id.valid() || (!appended &&
                                    (!state.entities_.free_slots_.contains(id.index()) ||
                                     state.entities_.slots_[id.index()].generation != id.generation()))) {
                    return std::unexpected(Diagnostic{DiagnosticCode::serialization_error,
                                                      "spawn event cannot be applied", {}});
                }
                state.entities_.redo_spawn(payload.entity, appended);
                return detail::SpawnPatch{payload.entity, appended};
            } else if constexpr (std::same_as<T, EntityDestroyed>) {
                const auto current = state.entities_.snapshot(payload.entity.id);
                if (!current || *current != payload.entity) {
                    return std::unexpected(Diagnostic{DiagnosticCode::serialization_error,
                                                      "destroy event does not match current state", {}});
                }
                state.entities_.redo_destroy(payload.entity.id);
                return detail::DestroyPatch{payload.entity};
            } else if constexpr (std::same_as<T, EntityMoved>) {
                auto slot = state.entities_.slot(payload.entity);
                if (!slot || (*slot)->location != payload.from ||
                    (payload.to && !state.topology_.contains(*payload.to))) {
                    return std::unexpected(Diagnostic{DiagnosticCode::serialization_error,
                                                      "move event cannot be applied", {}});
                }
                (*slot)->location = payload.to;
                return detail::LocationPatch{payload.entity, payload.from, payload.to};
            } else if constexpr (std::same_as<T, EntityOwnerChanged>) {
                auto slot = state.entities_.slot(payload.entity);
                if (!slot || (*slot)->owner != payload.from ||
                    (payload.to && !payload.to->valid())) {
                    return std::unexpected(Diagnostic{DiagnosticCode::serialization_error,
                                                      "owner event cannot be applied", {}});
                }
                (*slot)->owner = payload.to;
                return detail::OwnerPatch{payload.entity, payload.from, payload.to};
            } else if constexpr (std::same_as<T, EntityPropertyChanged>) {
                auto slot = state.entities_.slot(payload.entity);
                if (!slot) {
                    return std::unexpected(slot.error());
                }
                const auto* current = (*slot)->properties.find(payload.property);
                if ((current == nullptr) != !payload.from ||
                    (current != nullptr && *current != *payload.from)) {
                    return std::unexpected(Diagnostic{DiagnosticCode::serialization_error,
                                                      "property event cannot be applied", {}});
                }
                if (payload.to) {
                    static_cast<void>((*slot)->properties.set(payload.property, *payload.to));
                } else {
                    static_cast<void>((*slot)->properties.erase(payload.property));
                }
                return detail::PropertyPatch{payload.entity, payload.property, payload.from,
                                             payload.to};
            } else if constexpr (std::same_as<T, EntityTagChanged>) {
                auto slot = state.entities_.slot(payload.entity);
                if (!slot || (*slot)->tags.contains(payload.tag) == payload.added) {
                    return std::unexpected(Diagnostic{DiagnosticCode::serialization_error,
                                                      "tag event cannot be applied", {}});
                }
                if (payload.added) {
                    static_cast<void>((*slot)->tags.add(payload.tag));
                } else {
                    static_cast<void>((*slot)->tags.remove(payload.tag));
                }
                return detail::TagPatch{payload.entity, payload.tag, !payload.added,
                                        payload.added};
            } else if constexpr (std::same_as<T, EffectPushed>) {
                auto& stack = state.effect_stack_;
                if (stack.pending_choice_ || stack.effects_.size() >= EffectStack::maximum_depth ||
                    !effect_is_applicable(payload.effect, state)) {
                    return std::unexpected(Diagnostic{DiagnosticCode::serialization_error,
                                                      "effect push event cannot be applied", {}});
                }
                stack.effects_.push_back(payload.effect);
                return detail::EffectPushPatch{payload.effect};
            } else if constexpr (std::same_as<T, EffectPopped>) {
                auto& stack = state.effect_stack_;
                if (stack.pending_choice_ || stack.effects_.empty() ||
                    stack.effects_.back() != payload.effect) {
                    return std::unexpected(Diagnostic{DiagnosticCode::serialization_error,
                                                      "effect pop event cannot be applied", {}});
                }
                stack.effects_.pop_back();
                return detail::EffectPopPatch{payload.effect};
            } else if constexpr (std::same_as<T, ChoiceRequested>) {
                auto& stack = state.effect_stack_;
                if (stack.pending_choice_ || !choice_is_applicable(payload.choice, state)) {
                    return std::unexpected(Diagnostic{DiagnosticCode::serialization_error,
                                                      "choice request event cannot be applied", {}});
                }
                stack.pending_choice_ = payload.choice;
                return detail::ChoicePatch{std::nullopt, payload.choice};
            } else if constexpr (std::same_as<T, ChoiceResolved>) {
                auto& pending = state.effect_stack_.pending_choice_;
                if (!pending || *pending != payload.choice ||
                    std::ranges::none_of(pending->options,
                                         [&payload](const ChoiceOption& option) {
                                             return option.id == payload.option_id;
                                         })) {
                    return std::unexpected(Diagnostic{DiagnosticCode::serialization_error,
                                                      "choice resolution event cannot be applied", {}});
                }
                const auto previous = *pending;
                pending.reset();
                return detail::ChoicePatch{previous, std::nullopt};
            } else {
                return detail::NoopPatch{};
            }
        },
        event.payload);
}

std::expected<void, Diagnostic> GameSession::undo() {
    if (cursor_ == 0U) {
        return std::unexpected(
            Diagnostic{DiagnosticCode::invalid_state, "there is no transaction to undo", {}});
    }
    auto& entry = history_[cursor_ - 1U];
    for (auto patch = entry.patches.rbegin(); patch != entry.patches.rend(); ++patch) {
        apply_patch(state_, *patch, false);
    }
    random_.restore(entry.random_before);
    --cursor_;
    return {};
}

std::expected<void, Diagnostic> GameSession::redo() {
    if (cursor_ == history_.size()) {
        return std::unexpected(
            Diagnostic{DiagnosticCode::invalid_state, "there is no transaction to redo", {}});
    }
    auto& entry = history_[cursor_];
    for (const auto& patch : entry.patches) {
        apply_patch(state_, patch, true);
    }
    random_.restore(entry.random_after);
    ++cursor_;
    return {};
}

std::expected<std::uint64_t, Diagnostic> GameSession::replayed_state_hash() const {
    const Checkpoint* checkpoint = &checkpoints_.front();
    for (const auto& candidate : checkpoints_) {
        if (candidate.history_index <= cursor_ &&
            candidate.history_index >= checkpoint->history_index) {
            checkpoint = &candidate;
        }
    }
    auto state = GameState::from_canonical_bytes(checkpoint->state);
    if (!state) {
        return std::unexpected(state.error());
    }
    DeterministicRandom random{random_.master_seed()};
    random.restore(checkpoint->random);
    for (std::size_t index = checkpoint->history_index; index < cursor_; ++index) {
        for (const auto& event : history_[index].batch.events) {
            auto patch = apply_event(*state, event);
            if (!patch) {
                return std::unexpected(patch.error());
            }
        }
        random.restore(history_[index].random_after);
        const auto hash = hash_state_and_random(*state, random);
        if (hash != history_[index].batch.resulting_state_hash) {
            return std::unexpected(Diagnostic{DiagnosticCode::invalid_state,
                                              "replay diverged from its recorded state hash", {}});
        }
    }
    return hash_state_and_random(*state, random);
}

void GameSession::truncate_redo() {
    if (cursor_ == history_.size()) {
        return;
    }
    history_.erase(history_.begin() + static_cast<std::ptrdiff_t>(cursor_), history_.end());
    std::erase_if(checkpoints_,
                  [this](const Checkpoint& checkpoint) { return checkpoint.history_index > cursor_; });
    next_event_sequence_ = 1U;
    for (std::size_t index = cursor_; index > 0U; --index) {
        if (!history_[index - 1U].batch.events.empty()) {
            next_event_sequence_ = history_[index - 1U].batch.events.back().sequence + 1U;
            break;
        }
    }
}

void GameSession::maybe_checkpoint() {
    if (cursor_ != 0U && cursor_ % checkpoint_interval == 0U) {
        checkpoints_.push_back(Checkpoint{cursor_, state_.canonical_bytes(), random_.snapshot()});
    }
}

std::vector<std::byte> GameSession::save() const {
    BinaryWriter writer;
    writer.string(archive_magic);
    writer.u32(archive_version);
    writer.u64(random_.master_seed());
    writer.bytes(initial_state_);
    writer.u64(static_cast<std::uint64_t>(cursor_));
    writer.u64(next_event_sequence_);
    writer.u64(static_cast<std::uint64_t>(history_.size()));
    for (const auto& entry : history_) {
        write_intent(writer, entry.intent);
        writer.u64(static_cast<std::uint64_t>(entry.batch.events.size()));
        for (const auto& event : entry.batch.events) {
            encode_event(writer, event);
        }
        writer.u64(entry.batch.resulting_state_hash);
        write_snapshot(writer, entry.random_after);
    }
    writer.u64(static_cast<std::uint64_t>(checkpoints_.size()));
    for (const auto& checkpoint : checkpoints_) {
        writer.u64(static_cast<std::uint64_t>(checkpoint.history_index));
        writer.bytes(checkpoint.state);
        write_snapshot(writer, checkpoint.random);
    }
    writer.u64(state_hash());
    return std::move(writer).take();
}

std::expected<GameSession, Diagnostic> GameSession::load(std::span<const std::byte> archive) {
    BinaryReader reader{archive};
    const auto magic = reader.string();
    const auto version = reader.u32();
    if (magic != archive_magic || (version != 1U && version != archive_version)) {
        return std::unexpected(Diagnostic{DiagnosticCode::serialization_error,
                                          "unsupported or invalid save archive header", {}});
    }
    const auto seed = reader.u64();
    auto initial_bytes = reader.bytes();
    auto initial = GameState::from_canonical_bytes(initial_bytes);
    if (!initial) {
        return std::unexpected(initial.error());
    }
    const auto saved_cursor = reader.u64();
    const auto saved_next_sequence = reader.u64();
    const auto history_count = reader.u64();
    if (saved_cursor > history_count || history_count > max_history_entries ||
        history_count > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        return std::unexpected(Diagnostic{DiagnosticCode::serialization_error,
                                          "invalid save history bounds", {}});
    }

    GameSession result{std::move(*initial), seed};
    const auto initial_hash = result.state_hash();
    result.initial_state_ = result.state_.canonical_bytes();
    result.history_.clear();
    result.checkpoints_.clear();
    result.checkpoints_.push_back(Checkpoint{0U, result.initial_state_, result.random_.snapshot()});
    std::uint64_t expected_sequence = 1U;
    for (std::uint64_t index = 0; index < history_count && reader.ok(); ++index) {
        auto intent = read_intent(reader);
        if (!intent) {
            return std::unexpected(intent.error());
        }
        EventBatch batch;
        const auto event_count = reader.u64();
        if (event_count > Transaction::max_events) {
            return std::unexpected(Diagnostic{DiagnosticCode::serialization_error,
                                              "save transaction event limit exceeded", {}});
        }
        std::vector<detail::StatePatch> patches;
        patches.reserve(static_cast<std::size_t>(event_count));
        for (std::uint64_t event_index = 0; event_index < event_count && reader.ok();
             ++event_index) {
            auto event = decode_event(reader);
            if (!event || event->sequence != expected_sequence++) {
                return std::unexpected(Diagnostic{DiagnosticCode::serialization_error,
                                                  "save event sequence is invalid", {}});
            }
            auto patch = apply_event(result.state_, *event);
            if (!patch) {
                return std::unexpected(patch.error());
            }
            patches.push_back(std::move(*patch));
            batch.events.push_back(std::move(*event));
        }
        const auto archived_resulting_hash = reader.u64();
        batch.resulting_state_hash = archived_resulting_hash;
        auto after = read_snapshot(reader);
        if (!after) {
            return std::unexpected(after.error());
        }
        const auto before = result.random_.snapshot();
        result.random_.restore(*after);
        const auto migrated_hash = result.state_hash();
        const auto archived_hash =
            version == 1U ? legacy_hash_state_and_random(result.state_, result.random_)
                          : std::expected<std::uint64_t, Diagnostic>{migrated_hash};
        if (!archived_hash || *archived_hash != archived_resulting_hash) {
            return std::unexpected(Diagnostic{DiagnosticCode::serialization_error,
                                              "save history state hash does not match", {}});
        }
        batch.resulting_state_hash = migrated_hash;
        result.history_.push_back(HistoryEntry{std::move(*intent), std::move(batch),
                                               std::move(patches), before, std::move(*after)});
        result.cursor_ = result.history_.size();
        result.maybe_checkpoint();
    }
    if (saved_next_sequence != expected_sequence) {
        return std::unexpected(Diagnostic{DiagnosticCode::serialization_error,
                                          "save next event sequence is invalid", {}});
    }
    result.next_event_sequence_ = saved_next_sequence;

    const auto checkpoint_count = reader.u64();
    if (checkpoint_count > history_count / checkpoint_interval + 1U) {
        return std::unexpected(Diagnostic{DiagnosticCode::serialization_error,
                                          "save checkpoint count is invalid", {}});
    }
    std::vector<Checkpoint> archived_checkpoints;
    archived_checkpoints.reserve(static_cast<std::size_t>(checkpoint_count));
    for (std::uint64_t index = 0; index < checkpoint_count && reader.ok(); ++index) {
        const auto history_index = reader.u64();
        auto bytes = reader.bytes();
        auto snapshot = read_snapshot(reader);
        auto checked_state = GameState::from_canonical_bytes(bytes);
        if (history_index > history_count || !snapshot || !checked_state) {
            return std::unexpected(Diagnostic{DiagnosticCode::serialization_error,
                                              "invalid archived checkpoint", {}});
        }
        DeterministicRandom checkpoint_random{seed};
        checkpoint_random.restore(*snapshot);
        const auto checkpoint_hash = hash_state_and_random(*checked_state, checkpoint_random);
        const auto recorded_hash = history_index == 0U
                                       ? initial_hash
                                       : result.history_[history_index - 1U]
                                             .batch.resulting_state_hash;
        if (checkpoint_hash != recorded_hash ||
            (!archived_checkpoints.empty() &&
             history_index <= archived_checkpoints.back().history_index)) {
            return std::unexpected(Diagnostic{DiagnosticCode::serialization_error,
                                              "archived checkpoint does not match history", {}});
        }
        archived_checkpoints.push_back(
            Checkpoint{static_cast<std::size_t>(history_index),
                       checked_state->canonical_bytes(), std::move(*snapshot)});
    }
    if (!archived_checkpoints.empty() && archived_checkpoints.front().history_index == 0U) {
        result.checkpoints_ = std::move(archived_checkpoints);
    }

    while (result.cursor_ > saved_cursor) {
        if (auto undone = result.undo(); !undone) {
            return std::unexpected(undone.error());
        }
    }
    const auto expected_hash = reader.u64();
    const auto actual_hash =
        version == 1U ? legacy_hash_state_and_random(result.state_, result.random_)
                      : std::expected<std::uint64_t, Diagnostic>{result.state_hash()};
    if (!reader.ok() || !reader.at_end() || !actual_hash || *actual_hash != expected_hash) {
        return std::unexpected(Diagnostic{DiagnosticCode::serialization_error,
                                          "save archive final state hash does not match", {}});
    }
    return result;
}

} // namespace ludus
