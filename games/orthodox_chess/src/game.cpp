#include "ludus/chess/game.hpp"

#include "ludus/core/symbol.hpp"
#include "ludus/rule_ir/program.hpp"
#include "ludus/rules/action.hpp"
#include "ludus/rules/transaction.hpp"
#include "ludus/topology/topology.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace ludus::chess {
namespace {

constexpr PlayerId white_player{0U, 1U};
constexpr PlayerId black_player{1U, 1U};
constexpr std::int64_t no_en_passant = -1;

struct ChessIds {
    TagId piece_tag;
    TagId metadata_tag;
    PropertyId piece_type;
    PropertyId side_to_move;
    PropertyId castling_rights;
    PropertyId en_passant;
    PropertyId halfmove_clock;
    PropertyId fullmove_number;
    PropertyId promotion;
    PropertyId move_flags;
    PropertyId next_side;
    PropertyId next_castling;
    PropertyId next_en_passant;
    PropertyId next_halfmove;
    PropertyId next_fullmove;
    ActionTypeId setup_action;
    ActionTypeId move_action;
    std::array<DirectionId, 16U> directions{};
};

struct StateView {
    Position position;
    EntityId metadata;
    std::array<std::optional<EntityId>, 64U> entities_by_square{};
};

struct DirectionSpec {
    std::string_view name;
    int file_delta;
    int rank_delta;
};

constexpr std::array<DirectionSpec, 16U> direction_specs{{
    {"north", 0, 1},
    {"east", 1, 0},
    {"south", 0, -1},
    {"west", -1, 0},
    {"north_east", 1, 1},
    {"south_east", 1, -1},
    {"south_west", -1, -1},
    {"north_west", -1, 1},
    {"knight_nne", 1, 2},
    {"knight_ene", 2, 1},
    {"knight_ese", 2, -1},
    {"knight_sse", 1, -2},
    {"knight_ssw", -1, -2},
    {"knight_wsw", -2, -1},
    {"knight_wnw", -2, 1},
    {"knight_nnw", -1, 2},
}};

[[nodiscard]] Diagnostic invalid_state(std::string message) {
    return Diagnostic{DiagnosticCode::invalid_state, std::move(message), {}};
}

[[nodiscard]] Diagnostic invalid_move(std::string message) {
    return Diagnostic{DiagnosticCode::validation_failed, std::move(message), {}};
}

[[nodiscard]] constexpr std::size_t square_index(std::uint8_t square) noexcept {
    return static_cast<std::size_t>(square);
}

[[nodiscard]] constexpr SpaceId space_id(std::uint8_t square) noexcept {
    return SpaceId{static_cast<std::uint32_t>(square), 1U};
}

[[nodiscard]] constexpr PlayerId player_id(Color color) noexcept {
    return color == Color::white ? white_player : black_player;
}

[[nodiscard]] constexpr Color color_for(PlayerId player) noexcept {
    return player == white_player ? Color::white : Color::black;
}

[[nodiscard]] std::expected<std::int64_t, Diagnostic>
integer_property(const EntitySnapshot& entity, PropertyId property, std::string_view name) {
    const auto* value = entity.properties.find(property);
    if (value == nullptr) {
        return std::unexpected(invalid_state("chess state is missing property: " +
                                             std::string{name}));
    }
    const auto* integer = std::get_if<std::int64_t>(value);
    if (integer == nullptr) {
        return std::unexpected(invalid_state("chess property is not an integer: " +
                                             std::string{name}));
    }
    return *integer;
}

void set_integer(PropertySet& properties, PropertyId property, std::int64_t value) {
    static_cast<void>(properties.set(property, PropertyValue{value}));
}

[[nodiscard]] std::expected<StateView, Diagnostic>
state_view(const ChessIds& ids, const GameState& state) {
    std::array<Piece, 64U> board{};
    StateView result;
    bool found_metadata = false;
    std::optional<Color> side;
    std::optional<std::uint8_t> rights;
    std::optional<std::uint8_t> en_passant_square;
    std::optional<std::uint32_t> halfmove;
    std::optional<std::uint32_t> fullmove;

    for (const auto entity_id : state.entities().entities()) {
        const auto snapshot = state.entities().snapshot(entity_id);
        if (!snapshot) {
            return std::unexpected(snapshot.error());
        }
        if (snapshot->tags.contains(ids.metadata_tag)) {
            if (found_metadata || snapshot->location || snapshot->owner) {
                return std::unexpected(invalid_state("chess metadata entity is malformed"));
            }
            found_metadata = true;
            result.metadata = entity_id;
            const auto side_value = integer_property(*snapshot, ids.side_to_move, "side_to_move");
            const auto rights_value =
                integer_property(*snapshot, ids.castling_rights, "castling_rights");
            const auto ep_value = integer_property(*snapshot, ids.en_passant, "en_passant");
            const auto halfmove_value =
                integer_property(*snapshot, ids.halfmove_clock, "halfmove_clock");
            const auto fullmove_value =
                integer_property(*snapshot, ids.fullmove_number, "fullmove_number");
            if (!side_value || !rights_value || !ep_value || !halfmove_value ||
                !fullmove_value) {
                if (!side_value) {
                    return std::unexpected(side_value.error());
                }
                if (!rights_value) {
                    return std::unexpected(rights_value.error());
                }
                if (!ep_value) {
                    return std::unexpected(ep_value.error());
                }
                if (!halfmove_value) {
                    return std::unexpected(halfmove_value.error());
                }
                return std::unexpected(fullmove_value.error());
            }
            if ((*side_value != 0 && *side_value != 1) || *rights_value < 0 ||
                *rights_value > 15 || *ep_value < no_en_passant || *ep_value >= 64 ||
                *halfmove_value < 0 ||
                *halfmove_value > std::numeric_limits<std::uint32_t>::max() ||
                *fullmove_value <= 0 ||
                *fullmove_value > std::numeric_limits<std::uint32_t>::max()) {
                return std::unexpected(invalid_state("chess metadata values are out of range"));
            }
            side = *side_value == 0 ? Color::white : Color::black;
            rights = static_cast<std::uint8_t>(*rights_value);
            if (*ep_value >= 0) {
                en_passant_square = static_cast<std::uint8_t>(*ep_value);
            }
            halfmove = static_cast<std::uint32_t>(*halfmove_value);
            fullmove = static_cast<std::uint32_t>(*fullmove_value);
            continue;
        }
        if (!snapshot->tags.contains(ids.piece_tag)) {
            continue;
        }
        if (!snapshot->location || snapshot->location->index() >= 64U ||
            snapshot->location->generation() != 1U ||
            (snapshot->owner != white_player && snapshot->owner != black_player)) {
            return std::unexpected(invalid_state("chess piece entity is malformed"));
        }
        const auto type_value = integer_property(*snapshot, ids.piece_type, "piece_type");
        if (!type_value || *type_value < static_cast<std::int64_t>(PieceType::pawn) ||
            *type_value > static_cast<std::int64_t>(PieceType::king)) {
            return std::unexpected(type_value ? invalid_state("chess piece type is out of range")
                                              : type_value.error());
        }
        const auto index = static_cast<std::size_t>(snapshot->location->index());
        if (result.entities_by_square[index]) {
            return std::unexpected(invalid_state("multiple chess pieces occupy one square"));
        }
        result.entities_by_square[index] = entity_id;
        board[index] = Piece{static_cast<PieceType>(*type_value), color_for(*snapshot->owner)};
    }

    if (!found_metadata || !side || !rights || !halfmove || !fullmove) {
        return std::unexpected(invalid_state("chess metadata entity is missing"));
    }
    auto position = Position::from_components(std::move(board), *side, *rights,
                                              en_passant_square, *halfmove, *fullmove);
    if (!position) {
        return std::unexpected(position.error());
    }
    result.position = std::move(*position);
    return result;
}

[[nodiscard]] std::string_view movement_name(PieceType type) noexcept {
    switch (type) {
    case PieceType::knight:
        return "knight";
    case PieceType::bishop:
        return "bishop";
    case PieceType::rook:
        return "rook";
    case PieceType::queen:
        return "queen";
    case PieceType::king:
        return "king";
    case PieceType::none:
    case PieceType::pawn:
        return {};
    }
    return {};
}

[[nodiscard]] std::expected<std::vector<ChessMove>, Diagnostic>
filtered_legal_moves(const ChessRuntimeData& runtime, const GameState& state,
                     const StateView& view);

[[nodiscard]] std::expected<ChessMove, Diagnostic>
canonical_move(const ChessRuntimeData& runtime, const GameState& state, const StateView& view,
               const ActionIntent& intent);

[[nodiscard]] ActionIntent action_intent(const ChessIds& ids, const StateView& view,
                                         ChessMove move) {
    ActionIntent result;
    result.type = ids.move_action;
    result.issuer = player_id(view.position.side_to_move());
    result.actor = view.entities_by_square[square_index(move.from)];
    result.targets.emplace_back(space_id(move.to));
    set_integer(result.arguments, ids.promotion, static_cast<std::int64_t>(move.promotion));
    return result;
}

[[nodiscard]] std::expected<ActionIntent, Diagnostic>
python_intent(const ChessIds& ids, const StateView& view, ChessMove move) {
    auto next = view.position;
    if (auto applied = next.apply(move); !applied) {
        return std::unexpected(applied.error());
    }

    auto result = action_intent(ids, view, move);
    result.targets.clear();
    result.targets.emplace_back(space_id(move.to));
    result.targets.emplace_back(view.metadata);
    if (has_flag(move.flags, MoveFlag::capture)) {
        const int captured_square = has_flag(move.flags, MoveFlag::en_passant)
                                        ? static_cast<int>(move.to) +
                                              (view.position.side_to_move() == Color::white ? -8 : 8)
                                        : static_cast<int>(move.to);
        if (captured_square < 0 || captured_square >= 64 ||
            !view.entities_by_square[static_cast<std::size_t>(captured_square)]) {
            return std::unexpected(invalid_state("captured chess entity is missing"));
        }
        result.targets.emplace_back(
            *view.entities_by_square[static_cast<std::size_t>(captured_square)]);
    }
    if (has_flag(move.flags, MoveFlag::king_castle) ||
        has_flag(move.flags, MoveFlag::queen_castle)) {
        const int rook_from = has_flag(move.flags, MoveFlag::king_castle)
                                  ? static_cast<int>(move.from) + 3
                                  : static_cast<int>(move.from) - 4;
        const int rook_to = has_flag(move.flags, MoveFlag::king_castle)
                                ? static_cast<int>(move.from) + 1
                                : static_cast<int>(move.from) - 1;
        if (rook_from < 0 || rook_from >= 64 ||
            !view.entities_by_square[static_cast<std::size_t>(rook_from)]) {
            return std::unexpected(invalid_state("castling rook entity is missing"));
        }
        result.targets.emplace_back(
            *view.entities_by_square[static_cast<std::size_t>(rook_from)]);
        result.targets.emplace_back(space_id(static_cast<std::uint8_t>(rook_to)));
    }

    set_integer(result.arguments, ids.move_flags, static_cast<std::int64_t>(move.flags));
    set_integer(result.arguments, ids.next_side,
                next.side_to_move() == Color::white ? std::int64_t{0} : std::int64_t{1});
    set_integer(result.arguments, ids.next_castling,
                static_cast<std::int64_t>(next.castling_rights()));
    set_integer(result.arguments, ids.next_en_passant,
                next.en_passant_square()
                    ? static_cast<std::int64_t>(*next.en_passant_square())
                    : no_en_passant);
    set_integer(result.arguments, ids.next_halfmove,
                static_cast<std::int64_t>(next.halfmove_clock()));
    set_integer(result.arguments, ids.next_fullmove,
                static_cast<std::int64_t>(next.fullmove_number()));
    return result;
}

[[nodiscard]] ChessIds intern_symbols(SymbolRegistry& symbols) {
    ChessIds ids;
    ids.piece_tag = symbols.tags.intern("chess_piece");
    ids.metadata_tag = symbols.tags.intern("chess_metadata");
    ids.piece_type = symbols.properties.intern("piece_type");
    ids.side_to_move = symbols.properties.intern("side_to_move");
    ids.castling_rights = symbols.properties.intern("castling_rights");
    ids.en_passant = symbols.properties.intern("en_passant");
    ids.halfmove_clock = symbols.properties.intern("halfmove_clock");
    ids.fullmove_number = symbols.properties.intern("fullmove_number");
    ids.promotion = symbols.properties.intern("promotion");
    ids.move_flags = symbols.properties.intern("move_flags");
    ids.next_side = symbols.properties.intern("next_side");
    ids.next_castling = symbols.properties.intern("next_castling");
    ids.next_en_passant = symbols.properties.intern("next_en_passant");
    ids.next_halfmove = symbols.properties.intern("next_halfmove");
    ids.next_fullmove = symbols.properties.intern("next_fullmove");
    ids.setup_action = symbols.actions.intern("chess_setup");
    ids.move_action = symbols.actions.intern("chess_move");
    for (std::size_t index = 0U; index < direction_specs.size(); ++index) {
        ids.directions[index] = symbols.directions.intern(direction_specs[index].name);
    }
    return ids;
}

[[nodiscard]] std::expected<Topology, Diagnostic> make_chess_topology(const ChessIds& ids) {
    TopologyBuilder builder;
    for (std::uint32_t square = 0U; square < 64U; ++square) {
        const auto added = builder.add_space();
        if (added != SpaceId{square, 1U}) {
            return std::unexpected(invalid_state("chess topology space identity is unstable"));
        }
    }
    for (int rank = 0; rank < 8; ++rank) {
        for (int file = 0; file < 8; ++file) {
            const auto from = static_cast<std::uint8_t>(rank * 8 + file);
            for (std::size_t direction = 0U; direction < direction_specs.size(); ++direction) {
                const int to_file = file + direction_specs[direction].file_delta;
                const int to_rank = rank + direction_specs[direction].rank_delta;
                if (to_file < 0 || to_file >= 8 || to_rank < 0 || to_rank >= 8) {
                    continue;
                }
                const auto to = static_cast<std::uint8_t>(to_rank * 8 + to_file);
                if (auto linked = builder.add_link(space_id(from), space_id(to),
                                                   ids.directions[direction]);
                    !linked) {
                    return std::unexpected(linked.error());
                }
            }
        }
    }
    return std::move(builder).build();
}

[[nodiscard]] std::expected<void, Diagnostic>
spawn_position(Transaction& transaction, const ChessIds& ids, const Position& position) {
    SpawnOptions metadata;
    static_cast<void>(metadata.tags.add(ids.metadata_tag));
    set_integer(metadata.properties, ids.side_to_move,
                position.side_to_move() == Color::white ? std::int64_t{0} : std::int64_t{1});
    set_integer(metadata.properties, ids.castling_rights,
                static_cast<std::int64_t>(position.castling_rights()));
    set_integer(metadata.properties, ids.en_passant,
                position.en_passant_square()
                    ? static_cast<std::int64_t>(*position.en_passant_square())
                    : no_en_passant);
    set_integer(metadata.properties, ids.halfmove_clock,
                static_cast<std::int64_t>(position.halfmove_clock()));
    set_integer(metadata.properties, ids.fullmove_number,
                static_cast<std::int64_t>(position.fullmove_number()));
    if (auto spawned = transaction.spawn(std::move(metadata)); !spawned) {
        return std::unexpected(spawned.error());
    }

    for (std::uint8_t square = 0U; square < 64U; ++square) {
        const auto piece = position.piece_at(square);
        if (piece.empty()) {
            continue;
        }
        SpawnOptions options;
        options.location = space_id(square);
        options.owner = player_id(piece.color);
        static_cast<void>(options.tags.add(ids.piece_tag));
        set_integer(options.properties, ids.piece_type,
                    static_cast<std::int64_t>(piece.type));
        if (auto spawned = transaction.spawn(std::move(options)); !spawned) {
            return std::unexpected(spawned.error());
        }
    }
    return {};
}

} // namespace

struct ChessRuntimeData {
    PythonRuntime* python{nullptr};
    ChessIds ids;
    std::map<PieceType, RuleProgram> movement_programs;
};

namespace {

std::expected<std::map<PieceType, RuleProgram>, Diagnostic>
compile_movement_programs(const PythonRuntime& python, const SymbolRegistry& symbols) {
    const auto actions = python.action_names();
    if (!actions) {
        return std::unexpected(actions.error());
    }
    if (std::ranges::find(*actions, "chess_move") == actions->end()) {
        return std::unexpected(invalid_state(
            "chess-like rule modules must register the chess_move action"));
    }
    std::map<PieceType, RuleProgram> result;
    for (const auto type : {PieceType::knight, PieceType::bishop, PieceType::rook,
                            PieceType::queen, PieceType::king}) {
        auto program = python.compile_movement(movement_name(type), symbols);
        if (!program) {
            return std::unexpected(program.error());
        }
        result.emplace(type, std::move(*program));
    }
    return result;
}

std::expected<std::vector<ChessMove>, Diagnostic>
filtered_legal_moves(const ChessRuntimeData& runtime, const GameState& state,
                     const StateView& view) {
    auto reference = view.position.legal_moves();
    std::map<std::uint8_t, std::vector<MoveCandidate>> native_candidates;
    std::vector<ChessMove> result;
    result.reserve(reference.size());
    for (const auto move : reference) {
        const auto piece = view.position.piece_at(move.from);
        if (piece.type == PieceType::pawn || has_flag(move.flags, MoveFlag::king_castle) ||
            has_flag(move.flags, MoveFlag::queen_castle)) {
            result.push_back(move);
            continue;
        }
        const auto program = runtime.movement_programs.find(piece.type);
        const auto actor = view.entities_by_square[square_index(move.from)];
        if (program == runtime.movement_programs.end() || !actor) {
            return std::unexpected(invalid_state("chess movement program or actor is missing"));
        }
        if (!native_candidates.contains(move.from)) {
            auto candidates = evaluate_movement(state, *actor, program->second);
            if (!candidates) {
                return std::unexpected(candidates.error());
            }
            native_candidates.emplace(move.from, std::move(*candidates));
        }
        const auto expected_capture = view.entities_by_square[square_index(move.to)];
        const auto& candidates = native_candidates.at(move.from);
        const auto found = std::ranges::find_if(candidates, [&](const MoveCandidate& candidate) {
            return candidate.destination == space_id(move.to) &&
                   candidate.capture == expected_capture;
        });
        if (found != candidates.end()) {
            result.push_back(move);
        }
    }
    return result;
}

std::expected<ChessMove, Diagnostic>
canonical_move(const ChessRuntimeData& runtime, const GameState& state, const StateView& view,
               const ActionIntent& intent) {
    if (intent.type != runtime.ids.move_action ||
        intent.issuer != player_id(view.position.side_to_move()) || !intent.actor ||
        intent.targets.size() != 1U ||
        !std::holds_alternative<SpaceId>(intent.targets.front()) ||
        intent.arguments.entries().size() != 1U ||
        intent.arguments.entries().front().id != runtime.ids.promotion) {
        return std::unexpected(invalid_move("chess move intent has a non-canonical shape"));
    }
    const auto destination = std::get<SpaceId>(intent.targets.front());
    const auto* promotion_value = intent.arguments.find(runtime.ids.promotion);
    const auto* promotion = promotion_value == nullptr
                                ? nullptr
                                : std::get_if<std::int64_t>(promotion_value);
    if (destination.index() >= 64U || destination.generation() != 1U || promotion == nullptr ||
        *promotion < static_cast<std::int64_t>(PieceType::none) ||
        *promotion > static_cast<std::int64_t>(PieceType::king)) {
        return std::unexpected(invalid_move("chess move destination or promotion is invalid"));
    }
    const auto moves = filtered_legal_moves(runtime, state, view);
    if (!moves) {
        return std::unexpected(moves.error());
    }
    const auto found = std::ranges::find_if(*moves, [&](const ChessMove& move) {
        return view.entities_by_square[square_index(move.from)] == intent.actor &&
               static_cast<std::uint32_t>(move.to) == destination.index() &&
               static_cast<std::int64_t>(move.promotion) == *promotion;
    });
    if (found == moves->end()) {
        return std::unexpected(invalid_move("chess move is not legal in the current position"));
    }
    return *found;
}

} // namespace

std::expected<ChessGame, Diagnostic>
ChessGame::create(PythonRuntime& python, Position initial, std::string_view rule_module) {
    if (auto loaded = python.load_module(rule_module); !loaded) {
        return std::unexpected(loaded.error());
    }

    SymbolRegistry symbols;
    const auto ids = intern_symbols(symbols);
    auto runtime = std::make_shared<ChessRuntimeData>();
    runtime->python = &python;
    runtime->ids = ids;
    auto programs = compile_movement_programs(python, symbols);
    if (!programs) {
        return std::unexpected(programs.error());
    }
    runtime->movement_programs = std::move(*programs);
    auto topology = make_chess_topology(ids);
    if (!topology) {
        return std::unexpected(topology.error());
    }
    GameSession session{GameState{std::move(symbols), std::move(*topology)}, 0U};

    auto setup_defined = session.define_action(
        ActionDefinition{ids.setup_action, 0, false}, {},
        [ids, initial](const RuleContext&, Transaction& transaction,
                       const ActionIntent&) { return spawn_position(transaction, ids, initial); });
    if (!setup_defined) {
        return std::unexpected(setup_defined.error());
    }
    ActionIntent setup_intent{ids.setup_action, white_player, std::nullopt, {}, {}};
    if (auto setup = session.submit(setup_intent); !setup) {
        return std::unexpected(setup.error());
    }

    auto move_defined = session.define_action(
        ActionDefinition{ids.move_action, 0, true},
        [runtime](const RuleContext& context,
                  const ActionIntent& intent) -> std::expected<void, Diagnostic> {
            const auto view = state_view(runtime->ids, context.state());
            if (!view) {
                return std::unexpected(view.error());
            }
            const auto move = canonical_move(*runtime, context.state(), *view, intent);
            if (!move) {
                return std::unexpected(move.error());
            }
            return {};
        },
        [runtime](const RuleContext& context, Transaction& transaction,
                  const ActionIntent& intent) -> std::expected<void, Diagnostic> {
            const auto view = state_view(runtime->ids, context.state());
            if (!view) {
                return std::unexpected(view.error());
            }
            const auto move = canonical_move(*runtime, context.state(), *view, intent);
            if (!move) {
                return std::unexpected(move.error());
            }
            const auto canonical = python_intent(runtime->ids, *view, *move);
            if (!canonical) {
                return std::unexpected(canonical.error());
            }
            return runtime->python->invoke_action("chess_move", context.state(), transaction,
                                                  *canonical);
        },
        [runtime](const RuleContext& context, PlayerId player) {
            std::vector<ActionIntent> result;
            const auto view = state_view(runtime->ids, context.state());
            if (!view || player != player_id(view->position.side_to_move())) {
                return result;
            }
            const auto moves = filtered_legal_moves(*runtime, context.state(), *view);
            if (!moves) {
                return result;
            }
            result.reserve(moves->size());
            for (const auto move : *moves) {
                result.push_back(action_intent(runtime->ids, *view, move));
            }
            return result;
        });
    if (!move_defined) {
        return std::unexpected(move_defined.error());
    }
    return ChessGame{std::move(runtime), std::move(session), std::move(initial)};
}

std::expected<Position, Diagnostic> ChessGame::position() const {
    const auto view = state_view(runtime_data_->ids, session_.state());
    if (!view) {
        return std::unexpected(view.error());
    }
    return view->position;
}

std::expected<std::vector<ChessMove>, Diagnostic> ChessGame::legal_moves() const {
    const auto view = state_view(runtime_data_->ids, session_.state());
    if (!view) {
        return std::unexpected(view.error());
    }
    return filtered_legal_moves(*runtime_data_, session_.state(), *view);
}

std::expected<PositionStatus, Diagnostic> ChessGame::status() const {
    const auto view = state_view(runtime_data_->ids, session_.state());
    if (!view) {
        return std::unexpected(view.error());
    }
    const auto moves = filtered_legal_moves(*runtime_data_, session_.state(), *view);
    if (!moves) {
        return std::unexpected(moves.error());
    }
    if (!moves->empty()) {
        return PositionStatus::ongoing;
    }
    return view->position.in_check(view->position.side_to_move()) ? PositionStatus::checkmate
                                                                  : PositionStatus::stalemate;
}

std::expected<EventBatch, Diagnostic> ChessGame::submit(ChessMove move) {
    if (move.from >= 64U || move.to >= 64U) {
        return std::unexpected(invalid_move("chess move square is out of range"));
    }
    const auto view = state_view(runtime_data_->ids, session_.state());
    if (!view) {
        return std::unexpected(view.error());
    }
    auto intent = action_intent(runtime_data_->ids, *view, move);
    const auto canonical = canonical_move(*runtime_data_, session_.state(), *view, intent);
    if (!canonical) {
        return std::unexpected(canonical.error());
    }
    auto batch = session_.submit(intent);
    if (!batch) {
        return std::unexpected(batch.error());
    }
    if (history_cursor_ < history_.size()) {
        history_.erase(history_.begin() + static_cast<std::ptrdiff_t>(history_cursor_),
                       history_.end());
    }
    history_.push_back(*canonical);
    history_cursor_ = history_.size();
    return batch;
}

std::expected<EventBatch, Diagnostic> ChessGame::submit_uci(std::string_view uci) {
    const auto current = position();
    if (!current) {
        return std::unexpected(current.error());
    }
    const auto move = current->find_legal_move(uci);
    if (!move) {
        return std::unexpected(move.error());
    }
    return submit(*move);
}

std::expected<void, Diagnostic> ChessGame::undo() {
    if (history_cursor_ == 0U) {
        return std::unexpected(invalid_state("there is no chess move to undo"));
    }
    if (auto undone = session_.undo(); !undone) {
        return std::unexpected(undone.error());
    }
    --history_cursor_;
    return {};
}

std::expected<void, Diagnostic> ChessGame::redo() {
    if (history_cursor_ >= history_.size()) {
        return std::unexpected(invalid_state("there is no chess move to redo"));
    }
    if (auto redone = session_.redo(); !redone) {
        return std::unexpected(redone.error());
    }
    ++history_cursor_;
    return {};
}

std::expected<bool, Diagnostic> ChessGame::reload_rules() {
    if (auto requested = runtime_data_->python->request_reload(); !requested) {
        return std::unexpected(requested.error());
    }
    std::map<PieceType, RuleProgram> replacement;
    const auto reloaded = runtime_data_->python->reload_if_safe(
        true, [this, &replacement](const PythonRuntime& runtime) {
            auto compiled = compile_movement_programs(runtime, session_.state().symbols());
            if (!compiled) {
                return std::expected<void, Diagnostic>{std::unexpected(compiled.error())};
            }
            replacement = std::move(*compiled);
            return std::expected<void, Diagnostic>{};
        });
    if (!reloaded) {
        return std::unexpected(reloaded.error());
    }
    if (*reloaded) {
        runtime_data_->movement_programs = std::move(replacement);
    }
    return *reloaded;
}

} // namespace ludus::chess
