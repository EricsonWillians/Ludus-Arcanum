#include "ludus/rules/session.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

struct TestIds {
    ludus::TagId unit;
    ludus::PropertyId health;
    ludus::PropertyId ability;
    ludus::DirectionId north;
    ludus::DirectionId east;
    ludus::DirectionId south;
    ludus::DirectionId west;
    ludus::ActionTypeId spawn;
    ludus::ActionTypeId move;
    ludus::ActionTypeId fail;
    ludus::ActionTypeId destroy;
    ludus::ActionTypeId begin_choice;
    ludus::ActionTypeId resolve_choice;
};

struct SessionFixture {
    ludus::GameSession session;
    TestIds ids;
};

SessionFixture make_session() {
    ludus::SymbolRegistry symbols;
    TestIds ids;
    ids.unit = symbols.tags.intern("unit");
    ids.health = symbols.properties.intern("health");
    ids.ability = symbols.properties.intern("ability");
    ids.north = symbols.directions.intern("north");
    ids.east = symbols.directions.intern("east");
    ids.south = symbols.directions.intern("south");
    ids.west = symbols.directions.intern("west");
    ids.spawn = symbols.actions.intern("spawn");
    ids.move = symbols.actions.intern("move");
    ids.fail = symbols.actions.intern("fail");
    ids.destroy = symbols.actions.intern("destroy");
    ids.begin_choice = symbols.actions.intern("begin_choice");
    ids.resolve_choice = symbols.actions.intern("resolve_choice");

    auto topology = ludus::make_rectangular_grid(
        2U, 2U, {ids.north, ids.east, ids.south, ids.west});
    REQUIRE(topology);
    return {ludus::GameSession{ludus::GameState{std::move(symbols), std::move(*topology)}, 77U},
            ids};
}

ludus::ActionIntent intent(ludus::ActionTypeId type, std::optional<ludus::EntityId> actor = {}) {
    return ludus::ActionIntent{type, ludus::PlayerId{0U, 1U}, actor, {}, {}};
}

void define_spawn(ludus::GameSession& session, const TestIds& ids) {
    REQUIRE(session.define_action(
        ludus::ActionDefinition{ids.spawn}, {},
        [ids](const ludus::RuleContext&, ludus::Transaction& transaction,
              const ludus::ActionIntent&) -> std::expected<void, ludus::Diagnostic> {
            ludus::TagSet tags;
            static_cast<void>(tags.add(ids.unit));
            ludus::PropertySet properties;
            static_cast<void>(properties.set(ids.health, std::int64_t{10}));
            auto spawned = transaction.spawn(ludus::SpawnOptions{
                ludus::rectangular_space_id(0U, 0U, 2U), ludus::PlayerId{0U, 1U},
                std::move(tags), std::move(properties)});
            if (!spawned) {
                return std::unexpected(spawned.error());
            }
            return {};
        },
        [ids](const ludus::RuleContext&, ludus::PlayerId player) {
            return std::vector<ludus::ActionIntent>{
                ludus::ActionIntent{ids.spawn, player, std::nullopt, {}, {}}};
        }));
}

ludus::EntityId spawned_entity(const ludus::EventBatch& batch) {
    REQUIRE_FALSE(batch.events.empty());
    return std::get<ludus::EntitySpawned>(batch.events.front().payload).entity.id;
}

std::vector<std::byte> read_hex_fixture(std::string_view name) {
    const auto path = std::string{LUDUS_SOURCE_DIR} + "/tests/fixtures/" +
                      std::string{name};
    std::ifstream input{path};
    if (!input) {
        throw std::runtime_error{"unable to open retained archive fixture: " + path};
    }
    std::string text;
    std::string line;
    while (std::getline(input, line)) {
        text += line;
    }
    if (input.bad()) {
        throw std::runtime_error{"unable to read retained archive fixture: " + path};
    }
    std::vector<std::byte> result;
    std::optional<unsigned int> high;
    const auto nibble = [](char character) -> std::optional<unsigned int> {
        if (character >= '0' && character <= '9') {
            return static_cast<unsigned int>(character - '0');
        }
        if (character >= 'a' && character <= 'f') {
            return static_cast<unsigned int>(character - 'a' + 10);
        }
        if (character >= 'A' && character <= 'F') {
            return static_cast<unsigned int>(character - 'A' + 10);
        }
        return std::nullopt;
    };
    for (const char character : text) {
        if (character == ' ' || character == '\t' || character == '\r' ||
            character == '\n') {
            continue;
        }
        const auto value = nibble(character);
        if (!value) {
            throw std::runtime_error{"retained archive fixture contains non-hex data"};
        }
        if (!high) {
            high = *value;
        } else {
            result.push_back(static_cast<std::byte>((*high << 4U) | *value));
            high.reset();
        }
    }
    if (high || result.empty()) {
        throw std::runtime_error{"retained archive fixture has an invalid length"};
    }
    return result;
}

} // namespace

TEST_CASE("actions validate, enumerate, commit typed events, and mutate through transactions",
          "[rules][actions][transaction]") {
    auto fixture = make_session();
    define_spawn(fixture.session, fixture.ids);

    const auto legal = fixture.session.legal_actions(ludus::PlayerId{0U, 1U});
    REQUIRE(legal.size() == 1U);
    REQUIRE(legal.front().type == fixture.ids.spawn);

    const auto batch = fixture.session.submit(intent(fixture.ids.spawn));
    REQUIRE(batch);
    REQUIRE(batch->events.size() == 1U);
    REQUIRE(batch->events.front().sequence == 1U);

    const auto entity = spawned_entity(*batch);
    const auto snapshot = fixture.session.state().entities().snapshot(entity);
    REQUIRE(snapshot);
    REQUIRE(snapshot->location == ludus::rectangular_space_id(0U, 0U, 2U));
    REQUIRE(snapshot->owner == ludus::PlayerId{0U, 1U});
    REQUIRE(snapshot->tags.contains(fixture.ids.unit));
    const auto* health_value = snapshot->properties.find(fixture.ids.health);
    REQUIRE(health_value != nullptr);
    if (health_value == nullptr) {
        return;
    }
    const auto* health = std::get_if<std::int64_t>(health_value);
    REQUIRE(health != nullptr);
    if (health == nullptr) {
        return;
    }
    REQUIRE(*health == 10);
    REQUIRE(batch->resulting_state_hash == fixture.session.state_hash());
}

TEST_CASE("a failed resolver rolls back state and random streams atomically",
          "[rules][rollback][random]") {
    auto fixture = make_session();
    define_spawn(fixture.session, fixture.ids);
    const auto spawned = fixture.session.submit(intent(fixture.ids.spawn));
    REQUIRE(spawned);
    const auto entity = spawned_entity(*spawned);

    REQUIRE(fixture.session.define_action(
        ludus::ActionDefinition{fixture.ids.fail, 0, true}, {},
        [](const ludus::RuleContext&, ludus::Transaction& transaction,
           const ludus::ActionIntent& action) -> std::expected<void, ludus::Diagnostic> {
            auto moved = transaction.move(*action.actor, ludus::rectangular_space_id(1U, 1U, 2U));
            if (!moved) {
                return moved;
            }
            auto rolled = transaction.roll("2d6", "combat");
            if (!rolled) {
                return std::unexpected(rolled.error());
            }
            return std::unexpected(ludus::Diagnostic{ludus::DiagnosticCode::transaction_failed,
                                                     "deliberate failure", {}});
        }));

    const auto before = fixture.session.state_hash();
    const auto before_history = fixture.session.history_size();
    const auto failed = fixture.session.submit(intent(fixture.ids.fail, entity));
    REQUIRE_FALSE(failed);
    REQUIRE(fixture.session.state_hash() == before);
    REQUIRE(fixture.session.history_size() == before_history);
    REQUIRE(fixture.session.state().entities().snapshot(entity)->location ==
            ludus::rectangular_space_id(0U, 0U, 2U));
}

TEST_CASE("C++ exceptions roll back the complete transaction", "[rules][rollback][exception]") {
    auto fixture = make_session();
    define_spawn(fixture.session, fixture.ids);
    const auto spawned = fixture.session.submit(intent(fixture.ids.spawn));
    REQUIRE(spawned);
    const auto entity = spawned_entity(*spawned);
    REQUIRE(fixture.session.define_action(
        ludus::ActionDefinition{fixture.ids.fail, 0, true}, {},
        [](const ludus::RuleContext&, ludus::Transaction& transaction,
           const ludus::ActionIntent& action) -> std::expected<void, ludus::Diagnostic> {
            auto moved = transaction.move(*action.actor, ludus::rectangular_space_id(1U, 0U, 2U));
            if (!moved) {
                return moved;
            }
            throw std::runtime_error{"boom"};
        }));
    const auto hash = fixture.session.state_hash();

    REQUIRE_FALSE(fixture.session.submit(intent(fixture.ids.fail, entity)));
    REQUIRE(fixture.session.state_hash() == hash);
}

TEST_CASE("undo, redo, replay, and canonical state round trips agree", "[rules][history]") {
    auto fixture = make_session();
    define_spawn(fixture.session, fixture.ids);
    const auto spawned = fixture.session.submit(intent(fixture.ids.spawn));
    REQUIRE(spawned);
    const auto entity = spawned_entity(*spawned);

    REQUIRE(fixture.session.define_action(
        ludus::ActionDefinition{fixture.ids.move, 0, true}, {},
        [](const ludus::RuleContext&, ludus::Transaction& transaction,
           const ludus::ActionIntent& action) {
            return transaction.move(*action.actor, ludus::rectangular_space_id(1U, 1U, 2U));
        }));
    const auto moved = fixture.session.submit(intent(fixture.ids.move, entity));
    REQUIRE(moved);
    const auto final_hash = fixture.session.state_hash();
    REQUIRE(fixture.session.replayed_state_hash() == final_hash);

    const auto state_bytes = fixture.session.state().canonical_bytes();
    const auto restored_state = ludus::GameState::from_canonical_bytes(state_bytes);
    REQUIRE(restored_state);
    REQUIRE(restored_state->canonical_hash() == fixture.session.state().canonical_hash());

    REQUIRE(fixture.session.undo());
    REQUIRE(fixture.session.state().entities().snapshot(entity)->location ==
            ludus::rectangular_space_id(0U, 0U, 2U));
    REQUIRE(fixture.session.redo());
    REQUIRE(fixture.session.state_hash() == final_hash);
}

TEST_CASE("save and load preserve history cursor, redo, seed, and replay hashes",
          "[rules][save][replay]") {
    auto fixture = make_session();
    define_spawn(fixture.session, fixture.ids);
    const auto first = fixture.session.submit(intent(fixture.ids.spawn));
    const auto second = fixture.session.submit(intent(fixture.ids.spawn));
    REQUIRE(first);
    REQUIRE(second);
    REQUIRE(fixture.session.undo());
    const auto saved_hash = fixture.session.state_hash();

    const auto archive = fixture.session.save();
    auto loaded = ludus::GameSession::load(archive);
    REQUIRE(loaded);
    REQUIRE(loaded->state_hash() == saved_hash);
    REQUIRE(loaded->history_cursor() == 1U);
    REQUIRE(loaded->history_size() == 2U);
    REQUIRE(loaded->random().master_seed() == 77U);
    REQUIRE(loaded->replayed_state_hash() == loaded->state_hash());

    REQUIRE(loaded->redo());
    REQUIRE(loaded->state_hash() == second->resulting_state_hash);
    REQUIRE(loaded->replayed_state_hash() == loaded->state_hash());
}

TEST_CASE("retained version one archives migrate to the current canonical format",
          "[rules][save][migration]") {
    const auto archive = read_hex_fixture("session-v1-empty.hex");
    ludus::BinaryReader legacy_header{archive};
    REQUIRE(legacy_header.string() == "LUDUS-SAVE");
    REQUIRE(legacy_header.u32() == 1U);

    auto migrated = ludus::GameSession::load(archive);
    REQUIRE(migrated);
    REQUIRE(migrated->random().master_seed() == 77U);
    REQUIRE(migrated->history_size() == 0U);
    REQUIRE(migrated->state().effect_stack().effects().empty());
    REQUIRE_FALSE(migrated->state().effect_stack().pending_choice());
    REQUIRE(migrated->state_hash() == 0x1755DF30D1B7F3E7ULL);

    const auto upgraded = migrated->save();
    ludus::BinaryReader current_header{upgraded};
    REQUIRE(current_header.string() == "LUDUS-SAVE");
    REQUIRE(current_header.u32() == ludus::GameSession::archive_version);
    static_cast<void>(current_header.u64());
    const auto current_state = current_header.bytes();
    ludus::BinaryReader state_header{current_state};
    REQUIRE(state_header.string() == "LUDUS-STATE");
    REQUIRE(state_header.u32() == 2U);

    const auto reloaded = ludus::GameSession::load(upgraded);
    REQUIRE(reloaded);
    REQUIRE(reloaded->state_hash() == migrated->state_hash());

    const auto history_archive = read_hex_fixture("session-v1-noop.hex");
    auto history_migrated = ludus::GameSession::load(history_archive);
    REQUIRE(history_migrated);
    REQUIRE(history_migrated->history_cursor() == 1U);
    REQUIRE(history_migrated->history_size() == 1U);
    REQUIRE(history_migrated->state_hash() == 0x1282F4BE188B3F3CULL);
    REQUIRE(history_migrated->replayed_state_hash() == history_migrated->state_hash());
    REQUIRE(history_migrated->undo());
    REQUIRE(history_migrated->redo());
    REQUIRE(history_migrated->state_hash() == 0x1282F4BE188B3F3CULL);
    REQUIRE(ludus::GameSession::load(history_migrated->save()));
}

TEST_CASE("destroyed entity handles become stale when slots are reused", "[core][ids][rules]") {
    auto fixture = make_session();
    define_spawn(fixture.session, fixture.ids);
    const auto spawned = fixture.session.submit(intent(fixture.ids.spawn));
    REQUIRE(spawned);
    const auto old_id = spawned_entity(*spawned);

    REQUIRE(fixture.session.define_action(
        ludus::ActionDefinition{fixture.ids.destroy, 0, true}, {},
        [](const ludus::RuleContext&, ludus::Transaction& transaction,
           const ludus::ActionIntent& action) { return transaction.destroy(*action.actor); }));
    REQUIRE(fixture.session.submit(intent(fixture.ids.destroy, old_id)));
    const auto replacement = fixture.session.submit(intent(fixture.ids.spawn));
    REQUIRE(replacement);
    const auto new_id = spawned_entity(*replacement);

    REQUIRE(new_id.index() == old_id.index());
    REQUIRE(new_id.generation() != old_id.generation());
    REQUIRE_FALSE(fixture.session.state().entities().contains(old_id));
    REQUIRE(fixture.session.state().entities().contains(new_id));
}

TEST_CASE("corrupt saves are rejected", "[rules][save][validation]") {
    auto fixture = make_session();
    auto archive = fixture.session.save();
    archive.back() ^= std::byte{0xff};
    REQUIRE_FALSE(ludus::GameSession::load(archive));
}

TEST_CASE("new actions after undo form a replayable branch with contiguous event sequences",
          "[rules][history][branch]") {
    auto fixture = make_session();
    define_spawn(fixture.session, fixture.ids);
    REQUIRE(fixture.session.submit(intent(fixture.ids.spawn)));
    REQUIRE(fixture.session.submit(intent(fixture.ids.spawn)));
    REQUIRE(fixture.session.undo());

    const auto replacement = fixture.session.submit(intent(fixture.ids.spawn));
    REQUIRE(replacement);
    REQUIRE(replacement->events.front().sequence == 2U);
    REQUIRE(fixture.session.history_size() == 2U);
    REQUIRE(fixture.session.replayed_state_hash() == fixture.session.state_hash());
    REQUIRE(ludus::GameSession::load(fixture.session.save()));
}

TEST_CASE("periodic checkpoints preserve deterministic replay", "[rules][checkpoint]") {
    auto fixture = make_session();
    define_spawn(fixture.session, fixture.ids);
    for (std::size_t index = 0; index < ludus::GameSession::checkpoint_interval + 5U; ++index) {
        REQUIRE(fixture.session.submit(intent(fixture.ids.spawn)));
    }

    REQUIRE(fixture.session.replayed_state_hash() == fixture.session.state_hash());
    const auto loaded = ludus::GameSession::load(fixture.session.save());
    REQUIRE(loaded);
    REQUIRE(loaded->replayed_state_hash() == loaded->state_hash());
}

TEST_CASE("value-only effects and choices are transactional and serializable",
          "[rules][effects][choice][save][rollback]") {
    auto fixture = make_session();
    define_spawn(fixture.session, fixture.ids);
    const auto spawned = fixture.session.submit(intent(fixture.ids.spawn));
    REQUIRE(spawned);
    const auto entity = spawned_entity(*spawned);

    REQUIRE(fixture.session.define_action(
        ludus::ActionDefinition{fixture.ids.begin_choice, 0, true}, {},
        [ids = fixture.ids](const ludus::RuleContext&, ludus::Transaction& transaction,
                            const ludus::ActionIntent& action)
            -> std::expected<void, ludus::Diagnostic> {
            ludus::EffectRecord effect;
            effect.id = 42U;
            effect.continuation = ids.resolve_choice;
            effect.source = action.actor;
            effect.entity_targets.push_back(*action.actor);
            effect.space_targets.push_back(ludus::rectangular_space_id(1U, 1U, 2U));
            static_cast<void>(effect.arguments.set(ids.ability, std::int64_t{7}));
            if (auto pushed = transaction.push_effect(std::move(effect)); !pushed) {
                return pushed;
            }
            ludus::ChoiceWindow choice{42U, ludus::PlayerId{0U, 1U}, "Choose a mode", {}};
            ludus::ChoiceOption first{1U, "Safe", {}};
            ludus::ChoiceOption second{2U, "Bold", {}};
            static_cast<void>(second.arguments.set(ids.ability, std::int64_t{9}));
            choice.options.push_back(std::move(first));
            choice.options.push_back(std::move(second));
            return transaction.request_choice(std::move(choice));
        }));
    REQUIRE(fixture.session.define_action(
        ludus::ActionDefinition{fixture.ids.resolve_choice}, {},
        [](const ludus::RuleContext&, ludus::Transaction& transaction,
           const ludus::ActionIntent&) -> std::expected<void, ludus::Diagnostic> {
            auto option = transaction.resolve_choice(42U, 2U);
            if (!option) {
                return std::unexpected(option.error());
            }
            auto popped = transaction.pop_effect(42U);
            return popped ? std::expected<void, ludus::Diagnostic>{}
                          : std::expected<void, ludus::Diagnostic>{
                                std::unexpected(popped.error())};
        }));

    const auto paused = fixture.session.submit(intent(fixture.ids.begin_choice, entity));
    REQUIRE(paused);
    REQUIRE(paused->events.size() == 2U);
    REQUIRE(std::holds_alternative<ludus::EffectPushed>(paused->events[0].payload));
    REQUIRE(std::holds_alternative<ludus::ChoiceRequested>(paused->events[1].payload));
    REQUIRE(fixture.session.state().effect_stack().top()->id == 42U);
    REQUIRE(fixture.session.state().effect_stack().pending_choice()->options.size() == 2U);
    const auto paused_hash = fixture.session.state_hash();

    const auto state_copy = ludus::GameState::from_canonical_bytes(
        fixture.session.state().canonical_bytes());
    REQUIRE(state_copy);
    REQUIRE(state_copy->effect_stack() == fixture.session.state().effect_stack());
    auto loaded = ludus::GameSession::load(fixture.session.save());
    REQUIRE(loaded);
    REQUIRE(loaded->state_hash() == paused_hash);
    REQUIRE(loaded->state().effect_stack().pending_choice());
    REQUIRE(loaded->replayed_state_hash() == paused_hash);

    REQUIRE(fixture.session.define_action(
        ludus::ActionDefinition{fixture.ids.destroy, 0, true}, {},
        [](const ludus::RuleContext&, ludus::Transaction& transaction,
           const ludus::ActionIntent& action) { return transaction.destroy(*action.actor); }));
    REQUIRE_FALSE(fixture.session.submit(intent(fixture.ids.destroy, entity)));
    REQUIRE(fixture.session.state_hash() == paused_hash);
    REQUIRE(fixture.session.state().entities().contains(entity));
    REQUIRE(fixture.session.state().effect_stack().pending_choice());

    const auto resolved = fixture.session.submit(intent(fixture.ids.resolve_choice));
    REQUIRE(resolved);
    REQUIRE(resolved->events.size() == 2U);
    REQUIRE(std::holds_alternative<ludus::ChoiceResolved>(resolved->events[0].payload));
    REQUIRE(std::holds_alternative<ludus::EffectPopped>(resolved->events[1].payload));
    REQUIRE(fixture.session.state().effect_stack().effects().empty());
    REQUIRE_FALSE(fixture.session.state().effect_stack().pending_choice());
    REQUIRE(fixture.session.replayed_state_hash() == fixture.session.state_hash());
    const auto resolved_hash = fixture.session.state_hash();
    REQUIRE(fixture.session.undo());
    REQUIRE(fixture.session.state_hash() == paused_hash);
    REQUIRE(fixture.session.state().effect_stack().pending_choice());
    REQUIRE(fixture.session.redo());
    REQUIRE(fixture.session.state_hash() == resolved_hash);

    REQUIRE(fixture.session.define_action(
        ludus::ActionDefinition{fixture.ids.fail, 0, true}, {},
        [ids = fixture.ids](const ludus::RuleContext&, ludus::Transaction& transaction,
                            const ludus::ActionIntent& action)
            -> std::expected<void, ludus::Diagnostic> {
            ludus::EffectRecord effect{99U, ids.resolve_choice, action.actor, {}, {}, {}};
            if (auto pushed = transaction.push_effect(std::move(effect)); !pushed) {
                return pushed;
            }
            ludus::ChoiceWindow choice{
                99U, ludus::PlayerId{0U, 1U}, "Temporary", {{1U, "One", {}}}};
            if (auto requested = transaction.request_choice(std::move(choice)); !requested) {
                return requested;
            }
            return std::unexpected(ludus::Diagnostic{
                ludus::DiagnosticCode::transaction_failed, "deliberate effect failure", {}});
        }));
    const auto before_failure = fixture.session.state_hash();
    REQUIRE_FALSE(fixture.session.submit(intent(fixture.ids.fail, entity)));
    REQUIRE(fixture.session.state_hash() == before_failure);
    REQUIRE(fixture.session.state().effect_stack().effects().empty());
    REQUIRE_FALSE(fixture.session.state().effect_stack().pending_choice());
}
