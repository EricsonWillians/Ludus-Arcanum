#include "ludus/rules/transaction.hpp"

#include <algorithm>
#include <set>
#include <string>
#include <type_traits>
#include <utility>

namespace ludus {
namespace {

Diagnostic invalid_symbol(std::string kind) {
    return Diagnostic{DiagnosticCode::invalid_argument, std::move(kind) + " is not interned", {}};
}

Diagnostic invalid_effect(std::string message) {
    return Diagnostic{DiagnosticCode::invalid_argument, std::move(message), {}};
}

bool properties_are_interned(const PropertySet& properties, const GameState& state) {
    return std::ranges::all_of(properties.entries(), [&state](const PropertyEntry& property) {
        return property.id.valid() && property.id.value() <= state.symbols().properties.size();
    });
}

} // namespace

Transaction::Transaction(GameState& state, DeterministicRandom& random)
    : state_(&state), random_(&random), random_before_(random.snapshot()) {}

Transaction::~Transaction() {
    if (!finished_) {
        rollback();
    }
}

std::unexpected<Diagnostic> Transaction::reject(Diagnostic diagnostic) {
    if (!failure_) {
        failure_ = diagnostic;
    }
    return std::unexpected(std::move(diagnostic));
}

std::expected<EntityId, Diagnostic> Transaction::spawn(SpawnOptions options) {
    if (failure_) {
        return std::unexpected(*failure_);
    }
    if (!has_event_capacity()) {
        return reject(Diagnostic{DiagnosticCode::transaction_failed,
                                 "transaction event limit exceeded", {}});
    }
    if (options.location && !state_->topology_.contains(*options.location)) {
        return reject(Diagnostic{DiagnosticCode::invalid_handle,
                                 "spawn location is not part of the topology", {}});
    }
    if (options.owner && !options.owner->valid()) {
        return reject(Diagnostic{DiagnosticCode::invalid_handle, "spawn owner is invalid", {}});
    }
    for (const auto tag : options.tags.values()) {
        if (tag.value() > state_->symbols_.tags.size()) {
            return reject(invalid_symbol("spawn tag"));
        }
    }
    for (const auto& property : options.properties.entries()) {
        if (property.id.value() > state_->symbols_.properties.size()) {
            return reject(invalid_symbol("spawn property"));
        }
    }

    auto allocation = state_->entities_.spawn(EntitySnapshot{
        {}, options.location, options.owner, std::move(options.tags), std::move(options.properties)});
    auto entity = state_->entities_.snapshot(allocation.id);
    if (!entity) {
        return reject(entity.error());
    }
    patches_.push_back(detail::SpawnPatch{*entity, allocation.appended});
    events_.push_back(Event{0U, EntitySpawned{*entity}});
    return allocation.id;
}

std::expected<void, Diagnostic> Transaction::destroy(EntityId entity) {
    if (failure_) {
        return std::unexpected(*failure_);
    }
    if (!has_event_capacity()) {
        return reject(Diagnostic{DiagnosticCode::transaction_failed,
                                 "transaction event limit exceeded", {}});
    }
    if (!state_->entities_.contains(entity)) {
        return reject(Diagnostic{DiagnosticCode::invalid_handle,
                                 "cannot destroy a stale or invalid entity", {}});
    }
    auto previous = state_->entities_.destroy(entity);
    patches_.push_back(detail::DestroyPatch{previous});
    events_.push_back(Event{0U, EntityDestroyed{previous}});
    return {};
}

std::expected<void, Diagnostic> Transaction::move(EntityId entity,
                                                  std::optional<SpaceId> destination) {
    if (failure_) {
        return std::unexpected(*failure_);
    }
    if (!has_event_capacity()) {
        return reject(Diagnostic{DiagnosticCode::transaction_failed,
                                 "transaction event limit exceeded", {}});
    }
    auto slot = state_->entities_.slot(entity);
    if (!slot) {
        return reject(slot.error());
    }
    if (destination && !state_->topology_.contains(*destination)) {
        return reject(Diagnostic{DiagnosticCode::invalid_handle,
                                 "move destination is not part of the topology", {}});
    }
    const auto previous = (*slot)->location;
    if (previous == destination) {
        return {};
    }
    (*slot)->location = destination;
    patches_.push_back(detail::LocationPatch{entity, previous, destination});
    events_.push_back(Event{0U, EntityMoved{entity, previous, destination}});
    return {};
}

std::expected<void, Diagnostic> Transaction::set_owner(EntityId entity,
                                                       std::optional<PlayerId> owner) {
    if (failure_) {
        return std::unexpected(*failure_);
    }
    if (!has_event_capacity()) {
        return reject(Diagnostic{DiagnosticCode::transaction_failed,
                                 "transaction event limit exceeded", {}});
    }
    auto slot = state_->entities_.slot(entity);
    if (!slot) {
        return reject(slot.error());
    }
    if (owner && !owner->valid()) {
        return reject(Diagnostic{DiagnosticCode::invalid_handle, "new owner is invalid", {}});
    }
    const auto previous = (*slot)->owner;
    if (previous == owner) {
        return {};
    }
    (*slot)->owner = owner;
    patches_.push_back(detail::OwnerPatch{entity, previous, owner});
    events_.push_back(Event{0U, EntityOwnerChanged{entity, previous, owner}});
    return {};
}

std::expected<void, Diagnostic> Transaction::set_property(EntityId entity, PropertyId property,
                                                          PropertyValue value) {
    if (failure_) {
        return std::unexpected(*failure_);
    }
    if (!has_event_capacity()) {
        return reject(Diagnostic{DiagnosticCode::transaction_failed,
                                 "transaction event limit exceeded", {}});
    }
    auto slot = state_->entities_.slot(entity);
    if (!slot) {
        return reject(slot.error());
    }
    if (!property.valid() || property.value() > state_->symbols_.properties.size()) {
        return reject(invalid_symbol("property"));
    }
    std::optional<PropertyValue> previous;
    if (const auto* current = (*slot)->properties.find(property); current != nullptr) {
        previous = *current;
        if (*current == value) {
            return {};
        }
    }
    const auto after = value;
    static_cast<void>((*slot)->properties.set(property, std::move(value)));
    patches_.push_back(detail::PropertyPatch{entity, property, previous, after});
    events_.push_back(Event{0U, EntityPropertyChanged{entity, property, previous, after}});
    return {};
}

std::expected<void, Diagnostic> Transaction::erase_property(EntityId entity,
                                                            PropertyId property) {
    if (failure_) {
        return std::unexpected(*failure_);
    }
    if (!has_event_capacity()) {
        return reject(Diagnostic{DiagnosticCode::transaction_failed,
                                 "transaction event limit exceeded", {}});
    }
    auto slot = state_->entities_.slot(entity);
    if (!slot) {
        return reject(slot.error());
    }
    if (!property.valid() || property.value() > state_->symbols_.properties.size()) {
        return reject(invalid_symbol("property"));
    }
    auto previous = (*slot)->properties.erase(property);
    if (!previous) {
        return {};
    }
    patches_.push_back(detail::PropertyPatch{entity, property, previous, std::nullopt});
    events_.push_back(
        Event{0U, EntityPropertyChanged{entity, property, previous, std::nullopt}});
    return {};
}

std::expected<void, Diagnostic> Transaction::add_tag(EntityId entity, TagId tag) {
    if (failure_) {
        return std::unexpected(*failure_);
    }
    if (!has_event_capacity()) {
        return reject(Diagnostic{DiagnosticCode::transaction_failed,
                                 "transaction event limit exceeded", {}});
    }
    auto slot = state_->entities_.slot(entity);
    if (!slot) {
        return reject(slot.error());
    }
    if (!tag.valid() || tag.value() > state_->symbols_.tags.size()) {
        return reject(invalid_symbol("tag"));
    }
    if (!(*slot)->tags.add(tag)) {
        return {};
    }
    patches_.push_back(detail::TagPatch{entity, tag, false, true});
    events_.push_back(Event{0U, EntityTagChanged{entity, tag, true}});
    return {};
}

std::expected<void, Diagnostic> Transaction::remove_tag(EntityId entity, TagId tag) {
    if (failure_) {
        return std::unexpected(*failure_);
    }
    if (!has_event_capacity()) {
        return reject(Diagnostic{DiagnosticCode::transaction_failed,
                                 "transaction event limit exceeded", {}});
    }
    auto slot = state_->entities_.slot(entity);
    if (!slot) {
        return reject(slot.error());
    }
    if (!tag.valid() || tag.value() > state_->symbols_.tags.size()) {
        return reject(invalid_symbol("tag"));
    }
    if (!(*slot)->tags.remove(tag)) {
        return {};
    }
    patches_.push_back(detail::TagPatch{entity, tag, true, false});
    events_.push_back(Event{0U, EntityTagChanged{entity, tag, false}});
    return {};
}

std::expected<void, Diagnostic> Transaction::push_effect(EffectRecord effect) {
    if (failure_) {
        return std::unexpected(*failure_);
    }
    if (!has_event_capacity()) {
        return reject(Diagnostic{DiagnosticCode::transaction_failed,
                                 "transaction event limit exceeded", {}});
    }
    auto& stack = state_->effect_stack_;
    if (stack.pending_choice_) {
        return reject(invalid_effect("cannot push an effect while a choice is pending"));
    }
    if (stack.effects_.size() >= EffectStack::maximum_depth) {
        return reject(invalid_effect("effect stack depth limit exceeded"));
    }
    if (effect.id == 0U || !effect.continuation.valid() ||
        effect.continuation.value() > state_->symbols_.actions.size() ||
        std::ranges::any_of(stack.effects_, [&effect](const EffectRecord& current) {
            return current.id == effect.id;
        })) {
        return reject(invalid_effect("effect identity or continuation is invalid"));
    }
    if (effect.entity_targets.size() > EffectStack::maximum_targets ||
        effect.space_targets.size() > EffectStack::maximum_targets ||
        effect.arguments.entries().size() > EffectStack::maximum_arguments ||
        (effect.source && !state_->entities_.contains(*effect.source)) ||
        std::ranges::any_of(effect.entity_targets, [this](EntityId target) {
            return !state_->entities_.contains(target);
        }) ||
        std::ranges::any_of(effect.space_targets, [this](SpaceId target) {
            return !state_->topology_.contains(target);
        }) ||
        !properties_are_interned(effect.arguments, *state_)) {
        return reject(invalid_effect("effect references invalid state"));
    }
    stack.effects_.push_back(effect);
    patches_.push_back(detail::EffectPushPatch{effect});
    events_.push_back(Event{0U, EffectPushed{std::move(effect)}});
    return {};
}

std::expected<EffectRecord, Diagnostic> Transaction::pop_effect(std::uint64_t expected_id) {
    if (failure_) {
        return std::unexpected(*failure_);
    }
    if (!has_event_capacity()) {
        return reject(Diagnostic{DiagnosticCode::transaction_failed,
                                 "transaction event limit exceeded", {}});
    }
    auto& stack = state_->effect_stack_;
    if (stack.pending_choice_) {
        return reject(invalid_effect("cannot pop an effect while its choice is pending"));
    }
    if (stack.effects_.empty() || stack.effects_.back().id != expected_id) {
        return reject(invalid_effect("effect is not at the top of the stack"));
    }
    auto effect = std::move(stack.effects_.back());
    stack.effects_.pop_back();
    patches_.push_back(detail::EffectPopPatch{effect});
    events_.push_back(Event{0U, EffectPopped{effect}});
    return effect;
}

std::expected<void, Diagnostic> Transaction::request_choice(ChoiceWindow choice) {
    if (failure_) {
        return std::unexpected(*failure_);
    }
    if (!has_event_capacity()) {
        return reject(Diagnostic{DiagnosticCode::transaction_failed,
                                 "transaction event limit exceeded", {}});
    }
    auto& stack = state_->effect_stack_;
    if (stack.pending_choice_ || stack.effects_.empty() || choice.id == 0U ||
        choice.id != stack.effects_.back().id ||
        !choice.player.valid() || choice.prompt.empty() ||
        choice.prompt.size() > EffectStack::maximum_text_length || choice.options.empty() ||
        choice.options.size() > EffectStack::maximum_choice_options) {
        return reject(invalid_effect("choice window is invalid for the current effect stack"));
    }
    std::set<std::uint32_t> ids;
    for (const auto& option : choice.options) {
        if (option.id == 0U || option.label.empty() ||
            option.label.size() > EffectStack::maximum_text_length ||
            option.arguments.entries().size() > EffectStack::maximum_arguments ||
            !ids.insert(option.id).second || !properties_are_interned(option.arguments, *state_)) {
            return reject(invalid_effect("choice option is invalid or duplicated"));
        }
    }
    stack.pending_choice_ = choice;
    patches_.push_back(detail::ChoicePatch{std::nullopt, choice});
    events_.push_back(Event{0U, ChoiceRequested{std::move(choice)}});
    return {};
}

std::expected<ChoiceOption, Diagnostic>
Transaction::resolve_choice(std::uint64_t choice_id, std::uint32_t option_id) {
    if (failure_) {
        return std::unexpected(*failure_);
    }
    if (!has_event_capacity()) {
        return reject(Diagnostic{DiagnosticCode::transaction_failed,
                                 "transaction event limit exceeded", {}});
    }
    auto& pending = state_->effect_stack_.pending_choice_;
    if (!pending || pending->id != choice_id) {
        return reject(invalid_effect("choice is stale or is not pending"));
    }
    const auto option = std::ranges::find_if(
        pending->options, [option_id](const ChoiceOption& candidate) {
            return candidate.id == option_id;
        });
    if (option == pending->options.end()) {
        return reject(invalid_effect("choice option is not available"));
    }
    const ChoiceWindow previous = *pending;
    const ChoiceOption selected = *option;
    pending.reset();
    patches_.push_back(detail::ChoicePatch{previous, std::nullopt});
    events_.push_back(Event{0U, ChoiceResolved{previous, option_id}});
    return selected;
}

std::expected<DiceResult, Diagnostic> Transaction::roll(std::string_view expression,
                                                       std::string_view stream) {
    if (failure_) {
        return std::unexpected(*failure_);
    }
    if (!has_event_capacity()) {
        return reject(Diagnostic{DiagnosticCode::transaction_failed,
                                 "transaction event limit exceeded", {}});
    }
    auto result = random_->roll(expression, stream);
    if (!result) {
        return reject(result.error());
    }
    events_.push_back(Event{0U, DiceRolled{*result}});
    return result;
}

std::expected<detail::TransactionCommit, Diagnostic> Transaction::commit() {
    if (!failure_) {
        for (const auto& effect : state_->effect_stack_.effects_) {
            if ((effect.source && !state_->entities_.contains(*effect.source)) ||
                std::ranges::any_of(effect.entity_targets, [this](EntityId target) {
                    return !state_->entities_.contains(target);
                }) ||
                std::ranges::any_of(effect.space_targets, [this](SpaceId target) {
                    return !state_->topology_.contains(target);
                })) {
                failure_ = Diagnostic{DiagnosticCode::transaction_failed,
                                      "transaction left a dangling effect reference", {}};
                break;
            }
        }
        const auto& stack = state_->effect_stack_;
        if (!failure_ && stack.pending_choice_ &&
            (stack.effects_.empty() || stack.pending_choice_->id != stack.effects_.back().id)) {
            failure_ = Diagnostic{DiagnosticCode::transaction_failed,
                                  "transaction left an invalid choice boundary", {}};
        }
    }
    if (failure_) {
        const auto error = *failure_;
        rollback();
        return std::unexpected(error);
    }
    finished_ = true;
    return detail::TransactionCommit{std::move(patches_), std::move(events_)};
}

void Transaction::rollback() noexcept {
    for (auto iterator = patches_.rbegin(); iterator != patches_.rend(); ++iterator) {
        std::visit(
            [this](const auto& patch) {
                using T = std::remove_cvref_t<decltype(patch)>;
                if constexpr (std::same_as<T, detail::SpawnPatch>) {
                    state_->entities_.undo_spawn(patch.entity.id, patch.appended);
                } else if constexpr (std::same_as<T, detail::DestroyPatch>) {
                    state_->entities_.undo_destroy(patch.entity);
                } else if constexpr (std::same_as<T, detail::LocationPatch>) {
                    state_->entities_.slots_[patch.entity.index()].location = patch.before;
                } else if constexpr (std::same_as<T, detail::OwnerPatch>) {
                    state_->entities_.slots_[patch.entity.index()].owner = patch.before;
                } else if constexpr (std::same_as<T, detail::PropertyPatch>) {
                    auto& properties = state_->entities_.slots_[patch.entity.index()].properties;
                    if (patch.before) {
                        static_cast<void>(properties.set(patch.property, *patch.before));
                    } else {
                        static_cast<void>(properties.erase(patch.property));
                    }
                } else if constexpr (std::same_as<T, detail::TagPatch>) {
                    auto& tags = state_->entities_.slots_[patch.entity.index()].tags;
                    if (patch.before) {
                        static_cast<void>(tags.add(patch.tag));
                    } else {
                        static_cast<void>(tags.remove(patch.tag));
                    }
                } else if constexpr (std::same_as<T, detail::EffectPushPatch>) {
                    state_->effect_stack_.effects_.pop_back();
                } else if constexpr (std::same_as<T, detail::EffectPopPatch>) {
                    state_->effect_stack_.effects_.push_back(patch.effect);
                } else if constexpr (std::same_as<T, detail::ChoicePatch>) {
                    state_->effect_stack_.pending_choice_ = patch.before;
                }
            },
            *iterator);
    }
    random_->restore(random_before_);
    finished_ = true;
}

} // namespace ludus
