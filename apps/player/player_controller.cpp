#include "player_controller.hpp"

#include "ludus/chess/match.hpp"
#include "ludus/chess/pgn.hpp"
#include "ludus/chess/presentation.hpp"
#include "ludus/python/runtime.hpp"
#include "ludus/tactical/game.hpp"
#include "ludus/tactical/presentation.hpp"

#include <algorithm>
#include <fcntl.h>
#include <glib.h>
#include <chrono>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>
#include <unistd.h>

namespace ludus::player {
namespace {

std::string diagnostic_text(const Diagnostic& diagnostic) {
    auto result = diagnostic.message;
    if (!diagnostic.detail.empty()) {
        result += " — " + diagnostic.detail;
    }
    return result;
}

Diagnostic io_error(std::string message) {
    return Diagnostic{DiagnosticCode::serialization_error, std::move(message), {}};
}

std::expected<std::vector<std::byte>, Diagnostic>
read_binary_file(const std::filesystem::path& path, std::uintmax_t maximum) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size > maximum) {
        return std::unexpected(io_error("unable to inspect bounded file: " + path.string()));
    }
    std::ifstream input{path, std::ios::binary};
    std::vector<std::byte> result(static_cast<std::size_t>(size));
    if (!input.read(reinterpret_cast<char*>(result.data()),
                    static_cast<std::streamsize>(result.size()))) {
        return std::unexpected(io_error("unable to read file: " + path.string()));
    }
    return result;
}

std::expected<std::string, Diagnostic>
read_text_file(const std::filesystem::path& path, std::uintmax_t maximum) {
    auto bytes = read_binary_file(path, maximum);
    if (!bytes) {
        return std::unexpected(bytes.error());
    }
    return std::string{reinterpret_cast<const char*>(bytes->data()), bytes->size()};
}

std::expected<void, Diagnostic>
write_atomic(const std::filesystem::path& path, std::span<const std::byte> bytes) {
    std::error_code error;
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), error);
        if (error) {
            return std::unexpected(io_error("unable to create save directory: " +
                                            error.message()));
        }
    }
    auto temporary = path;
    temporary += ".tmp";
    {
        std::ofstream output{temporary, std::ios::binary | std::ios::trunc};
        if (!output.write(reinterpret_cast<const char*>(bytes.data()),
                          static_cast<std::streamsize>(bytes.size())) || !output.flush()) {
            return std::unexpected(io_error("unable to write temporary save: " +
                                            temporary.string()));
        }
    }
    const auto temporary_descriptor = ::open(temporary.c_str(), O_RDONLY | O_CLOEXEC);
    if (temporary_descriptor < 0 || ::fsync(temporary_descriptor) != 0) {
        if (temporary_descriptor >= 0) {
            static_cast<void>(::close(temporary_descriptor));
        }
        return std::unexpected(io_error("unable to synchronize temporary save: " +
                                        temporary.string()));
    }
    static_cast<void>(::close(temporary_descriptor));
    std::filesystem::rename(temporary, path, error);
    if (error) {
        std::filesystem::remove(path, error);
        error.clear();
        std::filesystem::rename(temporary, path, error);
    }
    if (error) {
        return std::unexpected(io_error("unable to publish save atomically: " +
                                        error.message()));
    }
    const auto directory = path.parent_path().empty() ? std::filesystem::path{"."}
                                                       : path.parent_path();
    const auto directory_descriptor =
        ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory_descriptor < 0 || ::fsync(directory_descriptor) != 0) {
        if (directory_descriptor >= 0) {
            static_cast<void>(::close(directory_descriptor));
        }
        return std::unexpected(io_error("save was published but its directory could not be "
                                        "synchronized: " + directory.string()));
    }
    static_cast<void>(::close(directory_descriptor));
    return {};
}

std::expected<void, Diagnostic>
write_atomic(const std::filesystem::path& path, std::string_view text) {
    return write_atomic(path, std::as_bytes(std::span{text.data(), text.size()}));
}

} // namespace

PlayerController::PlayerController(PlayerGame game,
                                   std::vector<std::string> python_search_paths,
                                   bool hot_seat)
    : game_(game), hot_seat_(hot_seat),
      python_search_paths_(std::move(python_search_paths)) {}

PlayerController::~PlayerController() { stop(); }

void PlayerController::start() {
    if (!worker_) {
        worker_.emplace([this](std::stop_token stop) { run(stop); });
    }
}

void PlayerController::stop() {
    commands_.close();
    if (worker_) {
        worker_->request_stop();
        worker_->join();
        worker_.reset();
    }
}

bool PlayerController::submit(std::uint64_t action_token) {
    return enqueue(CommandKind::action, action_token);
}

bool PlayerController::undo() { return enqueue(CommandKind::undo); }

bool PlayerController::redo() { return enqueue(CommandKind::redo); }

bool PlayerController::restart() { return enqueue(CommandKind::restart); }

bool PlayerController::replay() { return enqueue(CommandKind::replay); }

bool PlayerController::new_chess_match(chess::ChessMatchSettings settings) {
    Command command;
    command.kind = CommandKind::new_match;
    command.settings = std::move(settings);
    return enqueue(std::move(command));
}

bool PlayerController::resign(chess::Color color) {
    Command command;
    command.kind = CommandKind::resign;
    command.color = color;
    return enqueue(std::move(command));
}

bool PlayerController::agree_draw() { return enqueue(CommandKind::agree_draw); }

bool PlayerController::claim_draw(chess::MatchResultReason reason,
                                  std::optional<std::uint64_t> intended_token) {
    Command command;
    command.kind = CommandKind::claim_draw;
    command.reason = reason;
    command.intended_token = intended_token;
    return enqueue(std::move(command));
}

bool PlayerController::preview_history(std::optional<std::size_t> ply) {
    Command command;
    command.kind = ply ? CommandKind::preview_history : CommandKind::return_live;
    command.ply = ply.value_or(0U);
    return enqueue(std::move(command));
}

bool PlayerController::open_match(std::filesystem::path path) {
    Command command;
    command.kind = CommandKind::open_match;
    command.path = std::move(path);
    return enqueue(std::move(command));
}

bool PlayerController::save_match(std::filesystem::path path) {
    Command command;
    command.kind = CommandKind::save_match;
    command.path = std::move(path);
    return enqueue(std::move(command));
}

bool PlayerController::import_pgn(std::filesystem::path path) {
    Command command;
    command.kind = CommandKind::import_pgn;
    command.path = std::move(path);
    return enqueue(std::move(command));
}

bool PlayerController::import_pgn_text(std::string text) {
    Command command;
    command.kind = CommandKind::import_pgn_text;
    command.text = std::move(text);
    return enqueue(std::move(command));
}

bool PlayerController::export_pgn(std::filesystem::path path) {
    Command command;
    command.kind = CommandKind::export_pgn;
    command.path = std::move(path);
    return enqueue(std::move(command));
}

std::filesystem::path PlayerController::autosave_path() {
    return std::filesystem::path{g_get_user_config_dir()} / "ludus-arcanum" /
           "chess-autosave.lmatch";
}

bool PlayerController::enqueue(Command command) {
    return commands_.try_push(std::move(command));
}

bool PlayerController::enqueue(CommandKind kind, std::uint64_t token) {
    Command command;
    command.kind = kind;
    command.token = token;
    return enqueue(std::move(command));
}

void PlayerController::run(std::stop_token stop) {
    std::uint64_t revision = 0U;
    std::shared_ptr<const PlayerView> current_view;
    const auto notify = [this] {
        static_cast<const Glib::Dispatcher&>(dispatcher_).emit();
    };
    const auto publish_failure = [this, &revision, &current_view, &notify](
                                     std::string message) {
        PlayerView view;
        view.render.revision = ++revision;
        view.render.status = "Error: " + std::move(message);
        current_view = views_.publish(std::move(view));
        notify();
    };

    auto runtime = PythonRuntime::create(python_search_paths_);
    if (!runtime) {
        publish_failure(diagnostic_text(runtime.error()));
        return;
    }
    using Game = std::variant<chess::ChessMatch, tactical::TacticalGame>;
    using Presentation =
        std::variant<chess::ChessPresentation, tactical::TacticalPresentation>;
    chess::ChessMatchSettings chess_settings;
    std::optional<std::size_t> preview_ply;
    std::optional<std::int64_t> paused_clock_elapsed;
    auto active_clock_started = std::chrono::steady_clock::now();

    const auto create_game = [this, &runtime, &chess_settings]()
        -> std::expected<Game, Diagnostic> {
        if (game_ == PlayerGame::chess) {
            auto created = chess::ChessMatch::create(**runtime, chess_settings);
            if (!created) {
                return std::unexpected(created.error());
            }
            return Game{std::in_place_type<chess::ChessMatch>, std::move(*created)};
        }
        auto created = tactical::TacticalGame::create(**runtime);
        if (!created) {
            return std::unexpected(created.error());
        }
        created->set_hot_seat(hot_seat_);
        return Game{std::in_place_type<tactical::TacticalGame>, std::move(*created)};
    };
    const auto create_presentation = [](Game& value)
        -> std::expected<Presentation, Diagnostic> {
        if (auto* chess_match = std::get_if<chess::ChessMatch>(&value)) {
            auto created = chess::ChessPresentation::create(chess_match->game());
            if (!created) {
                return std::unexpected(created.error());
            }
            return Presentation{std::in_place_type<chess::ChessPresentation>,
                                std::move(*created)};
        }
        auto created =
            tactical::TacticalPresentation::create(std::get<tactical::TacticalGame>(value));
        if (!created) {
            return std::unexpected(created.error());
        }
        return Presentation{std::in_place_type<tactical::TacticalPresentation>,
                            std::move(*created)};
    };

    auto created_game = create_game();
    if (!created_game) {
        publish_failure(diagnostic_text(created_game.error()));
        return;
    }
    Game game = std::move(*created_game);
    auto created_presentation = create_presentation(game);
    if (!created_presentation) {
        publish_failure(diagnostic_text(created_presentation.error()));
        return;
    }
    Presentation presentation = std::move(*created_presentation);

    const auto publish_game = [this, &revision, &current_view, &notify, &presentation,
                               &game, &runtime, &preview_ply, &paused_clock_elapsed,
                               &active_clock_started](
                                  const EventBatch* batch = nullptr,
                                  std::string_view status_override = {}) -> bool {
        auto view = [&]() -> std::expected<PlayerView, Diagnostic> {
            if (auto* chess_match = std::get_if<chess::ChessMatch>(&game)) {
                auto result = std::get<chess::ChessPresentation>(presentation).build_view(
                    *chess_match, ++revision,
                    current_view ? &current_view->render : nullptr, batch);
                if (!result) {
                    return std::unexpected(result.error());
                }
                if (chess_match->settings().time_control.clocked()) {
                    const auto display_elapsed = preview_ply && paused_clock_elapsed
                                                     ? *paused_clock_elapsed
                                                     : std::max<std::int64_t>(
                                                           0,
                                                           std::chrono::duration_cast<
                                                               std::chrono::milliseconds>(
                                                               std::chrono::steady_clock::now() -
                                                               active_clock_started)
                                                               .count());
                    for (auto& clock : result->clocks) {
                        if (clock.active) {
                            clock.committed_remaining_milliseconds =
                                std::max<std::int64_t>(
                                    0, clock.committed_remaining_milliseconds -
                                           display_elapsed);
                        }
                    }
                }
                if (preview_ply) {
                    const auto history = chess_match->history();
                    const auto count = std::min(*preview_ply, history.size());
                    auto preview = chess::ChessGame::create(
                        **runtime, chess_match->settings().initial_position);
                    if (!preview) {
                        return std::unexpected(preview.error());
                    }
                    for (std::size_t index = 0U; index < count; ++index) {
                        if (auto committed = preview->submit(history[index].move); !committed) {
                            return std::unexpected(committed.error());
                        }
                    }
                    auto rendered = std::get<chess::ChessPresentation>(presentation).build(
                        *preview, revision);
                    if (!rendered) {
                        return std::unexpected(rendered.error());
                    }
                    result->render = std::move(*rendered);
                    result->render.status = count == history.size()
                                                ? "Live position"
                                                : "History preview — moves are disabled";
                    for (auto& entry : result->timeline) {
                        entry.current = false;
                        entry.previewed = count != 0U && entry.ply + 1U == count;
                    }
                    for (auto& clock : result->clocks) {
                        clock.paused = true;
                        clock.active = false;
                    }
                    result->render.actions.clear();
                    result->render.choices.clear();
                    result->draw_claims.clear();
                    result->match_result.reset();
                    result->match_controls.can_undo = false;
                    result->match_controls.can_redo = false;
                    result->match_controls.can_resign = false;
                    result->match_controls.can_offer_draw = false;
                    result->match_controls.can_return_to_live = true;
                }
                return result;
            }
            auto& tactical_game = std::get<tactical::TacticalGame>(game);
            auto viewer = tactical_game.active_player();
            if (!viewer) {
                return std::unexpected(viewer.error());
            }
            return std::get<tactical::TacticalPresentation>(presentation).build_view(
                tactical_game, *viewer, ++revision);
        }();
        if (!view) {
            PlayerView failed;
            failed.render.revision = revision;
            failed.render.status = "Error: " + diagnostic_text(view.error());
            current_view = views_.publish(std::move(failed));
            notify();
            return false;
        }
        if (!status_override.empty()) {
            view->render.status = status_override;
        }
        current_view = views_.publish(std::move(*view));
        notify();
        return true;
    };
    const auto recreate = [&]() -> std::expected<void, Diagnostic> {
        auto replacement = create_game();
        if (!replacement) {
            return std::unexpected(replacement.error());
        }
        auto replacement_presentation = create_presentation(*replacement);
        if (!replacement_presentation) {
            return std::unexpected(replacement_presentation.error());
        }
        game = std::move(*replacement);
        presentation = std::move(*replacement_presentation);
        current_view.reset();
        preview_ply.reset();
        paused_clock_elapsed.reset();
        active_clock_started = std::chrono::steady_clock::now();
        return {};
    };
    const auto elapsed_now = [&game, &active_clock_started]() -> std::int64_t {
        const auto* match = std::get_if<chess::ChessMatch>(&game);
        if (match == nullptr || !match->settings().time_control.clocked()) {
            return 0;
        }
        return std::max<std::int64_t>(
            0, std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - active_clock_started).count());
    };
    const auto submit_token = [&game, &elapsed_now](std::uint64_t token,
                                                    std::optional<std::int64_t> elapsed =
                                                        std::nullopt)
        -> std::expected<EventBatch, Diagnostic> {
        if (auto* chess_match = std::get_if<chess::ChessMatch>(&game)) {
            const auto position = chess_match->game().position();
            if (!position) {
                return std::unexpected(position.error());
            }
            const auto decoded = chess::decode_action_token(token);
            const auto move = position->find_legal_move(chess::to_uci(decoded));
            if (!move) {
                return std::unexpected(move.error());
            }
            auto update = chess_match->submit(*move, elapsed.value_or(elapsed_now()));
            if (!update) {
                return std::unexpected(update.error());
            }
            return update->events.value_or(EventBatch{});
        }
        return std::get<tactical::TacticalGame>(game).submit_token(token);
    };
    const auto submit_player_token = [&game, &submit_token](std::uint64_t token)
        -> std::expected<EventBatch, Diagnostic> {
        if (std::holds_alternative<chess::ChessMatch>(game)) {
            return submit_token(token);
        }
        auto batches = std::get<tactical::TacticalGame>(game).submit_player_token(token);
        if (!batches) {
            return std::unexpected(batches.error());
        }
        if (batches->empty()) {
            return std::unexpected(Diagnostic{DiagnosticCode::invalid_state,
                                              "tactical decision produced no transaction", {}});
        }
        return std::move(batches->back());
    };
    const auto undo_game = [&game]() -> std::expected<void, Diagnostic> {
        if (auto* chess_match = std::get_if<chess::ChessMatch>(&game)) {
            return chess_match->undo();
        }
        return std::get<tactical::TacticalGame>(game).undo_player_decision();
    };
    const auto redo_game = [&game]() -> std::expected<void, Diagnostic> {
        if (auto* chess_match = std::get_if<chess::ChessMatch>(&game)) {
            return chess_match->redo();
        }
        return std::get<tactical::TacticalGame>(game).redo_player_decision();
    };
    const auto autosave = [&game]() -> std::expected<void, Diagnostic> {
        const auto* match = std::get_if<chess::ChessMatch>(&game);
        if (match == nullptr) {
            return {};
        }
        if (match->result().terminal()) {
            std::error_code ignored;
            std::filesystem::remove(PlayerController::autosave_path(), ignored);
            return {};
        }
        const auto archive = match->save();
        return write_atomic(PlayerController::autosave_path(), std::span{archive});
    };

    static_cast<void>(publish_game());
    struct ReplayPly {
        std::uint64_t token{0U};
        std::int64_t elapsed{0};
    };
    std::vector<ReplayPly> replay_line;
    std::size_t replay_cursor = 0U;
    auto next_replay_step = std::chrono::steady_clock::time_point::max();

    while (!stop.stop_requested()) {
        const auto now = std::chrono::steady_clock::now();
        if (replay_cursor < replay_line.size() && now >= next_replay_step) {
            auto committed = submit_token(replay_line[replay_cursor].token,
                                          replay_line[replay_cursor].elapsed);
            if (!committed) {
                publish_failure(diagnostic_text(committed.error()));
                replay_line.clear();
                continue;
            }
            ++replay_cursor;
            static_cast<void>(publish_game(&*committed));
            next_replay_step = now + std::chrono::milliseconds{280};
            continue;
        }

        if (!preview_ply) {
            if (auto* match = std::get_if<chess::ChessMatch>(&game);
                match != nullptr && !match->result().terminal() &&
                match->settings().time_control.clocked()) {
                const auto position = match->game().position();
                if (!position) {
                    publish_failure(diagnostic_text(position.error()));
                    return;
                }
                const auto remaining = match->remaining(position->side_to_move()).value_or(0);
                const auto elapsed = elapsed_now();
                if (elapsed >= remaining) {
                    const auto flagged = match->flag(position->side_to_move(), elapsed);
                    if (!flagged) {
                        publish_failure(diagnostic_text(flagged.error()));
                        return;
                    }
                    static_cast<void>(autosave());
                    static_cast<void>(publish_game());
                    continue;
                }
            }
        }

        const auto waiting_for_replay = replay_cursor < replay_line.size();
        const auto timeout = waiting_for_replay
                                 ? std::chrono::duration_cast<std::chrono::milliseconds>(
                                       std::max(next_replay_step - now,
                                                std::chrono::steady_clock::duration::zero()))
                                 : std::chrono::milliseconds{50};
        const auto command = commands_.pop_for(stop, timeout);
        if (!command) {
            continue;
        }
        replay_line.clear();
        replay_cursor = 0U;
        next_replay_step = std::chrono::steady_clock::time_point::max();

        const auto mutates_match = [](CommandKind kind) noexcept {
            switch (kind) {
            case CommandKind::preview_history:
            case CommandKind::return_live:
            case CommandKind::save_match:
            case CommandKind::export_pgn:
                return false;
            default:
                return true;
            }
        };
        if (preview_ply && mutates_match(command->kind)) {
            static_cast<void>(publish_game(
                nullptr, "Return to the live position before changing the match"));
            continue;
        }

        switch (command->kind) {
        case CommandKind::action: {
            if (preview_ply) {
                static_cast<void>(publish_game(nullptr,
                    "Return to the live position before moving"));
                break;
            }
            auto committed = submit_player_token(command->token);
            if (!committed) {
                static_cast<void>(publish_game(nullptr, diagnostic_text(committed.error())));
                break;
            }
            active_clock_started = std::chrono::steady_clock::now();
            static_cast<void>(autosave());
            static_cast<void>(publish_game(&*committed));
            break;
        }
        case CommandKind::undo:
            if (auto undone = undo_game(); !undone) {
                static_cast<void>(publish_game(nullptr, diagnostic_text(undone.error())));
            } else {
                active_clock_started = std::chrono::steady_clock::now();
                preview_ply.reset();
                static_cast<void>(autosave());
                static_cast<void>(publish_game());
            }
            break;
        case CommandKind::redo:
            if (auto redone = redo_game(); !redone) {
                static_cast<void>(publish_game(nullptr, diagnostic_text(redone.error())));
            } else {
                active_clock_started = std::chrono::steady_clock::now();
                preview_ply.reset();
                static_cast<void>(autosave());
                static_cast<void>(publish_game());
            }
            break;
        case CommandKind::restart:
            if (auto restarted = recreate(); !restarted) {
                publish_failure(diagnostic_text(restarted.error()));
            } else {
                static_cast<void>(autosave());
                static_cast<void>(publish_game());
            }
            break;
        case CommandKind::replay: {
            if (const auto* chess_match = std::get_if<chess::ChessMatch>(&game)) {
                const auto history = chess_match->history();
                replay_line.reserve(history.size());
                for (const auto& ply : history) {
                    replay_line.push_back(
                        ReplayPly{chess::encode_action_token(ply.move),
                                  ply.elapsed_milliseconds});
                }
            } else {
                const auto history =
                    std::get<tactical::TacticalGame>(game).action_history();
                for (const auto token : history) {
                    replay_line.push_back(ReplayPly{token, 0});
                }
            }
            if (auto restarted = recreate(); !restarted) {
                publish_failure(diagnostic_text(restarted.error()));
                replay_line.clear();
                break;
            }
            static_cast<void>(publish_game(nullptr, replay_line.empty() ? "Nothing to replay"
                                                                        : "Replaying game…"));
            replay_cursor = 0U;
            next_replay_step = std::chrono::steady_clock::now() +
                               std::chrono::milliseconds{320};
            break;
        }
        case CommandKind::new_match: {
            if (game_ != PlayerGame::chess || !command->settings) {
                static_cast<void>(publish_game(nullptr, "New Match is only available in chess"));
                break;
            }
            auto replacement = chess::ChessMatch::create(**runtime, *command->settings);
            if (!replacement) {
                static_cast<void>(publish_game(nullptr, diagnostic_text(replacement.error())));
                break;
            }
            Game candidate{std::in_place_type<chess::ChessMatch>, std::move(*replacement)};
            auto candidate_presentation = create_presentation(candidate);
            if (!candidate_presentation) {
                static_cast<void>(publish_game(nullptr,
                                               diagnostic_text(candidate_presentation.error())));
                break;
            }
            chess_settings = *command->settings;
            game = std::move(candidate);
            presentation = std::move(*candidate_presentation);
            current_view.reset();
            preview_ply.reset();
            active_clock_started = std::chrono::steady_clock::now();
            static_cast<void>(autosave());
            static_cast<void>(publish_game());
            break;
        }
        case CommandKind::resign: {
            auto* match = std::get_if<chess::ChessMatch>(&game);
            const auto completed = match != nullptr
                                       ? match->resign(command->color)
                                       : std::expected<void, Diagnostic>{
                                             std::unexpected(Diagnostic{
                                                 DiagnosticCode::invalid_state,
                                                 "Resign is only available in chess", {}})};
            if (!completed) {
                static_cast<void>(publish_game(nullptr, diagnostic_text(completed.error())));
            } else {
                static_cast<void>(autosave());
                static_cast<void>(publish_game());
            }
            break;
        }
        case CommandKind::agree_draw: {
            auto* match = std::get_if<chess::ChessMatch>(&game);
            const auto completed = match != nullptr
                                       ? match->agree_draw()
                                       : std::expected<void, Diagnostic>{
                                             std::unexpected(Diagnostic{
                                                 DiagnosticCode::invalid_state,
                                                 "Draw agreement is only available in chess", {}})};
            if (!completed) {
                static_cast<void>(publish_game(nullptr, diagnostic_text(completed.error())));
            } else {
                static_cast<void>(autosave());
                static_cast<void>(publish_game());
            }
            break;
        }
        case CommandKind::claim_draw: {
            auto* match = std::get_if<chess::ChessMatch>(&game);
            std::optional<chess::ChessMove> intended;
            if (match != nullptr && command->intended_token) {
                const auto position = match->game().position();
                if (position) {
                    const auto decoded = chess::decode_action_token(*command->intended_token);
                    const auto move = position->find_legal_move(chess::to_uci(decoded));
                    if (move) {
                        intended = *move;
                    }
                }
            }
            const auto completed = match != nullptr
                                       ? match->claim_draw(command->reason, intended)
                                       : std::expected<void, Diagnostic>{
                                             std::unexpected(Diagnostic{
                                                 DiagnosticCode::invalid_state,
                                                 "Draw claims are only available in chess", {}})};
            if (!completed) {
                static_cast<void>(publish_game(nullptr, diagnostic_text(completed.error())));
            } else {
                static_cast<void>(autosave());
                static_cast<void>(publish_game());
            }
            break;
        }
        case CommandKind::preview_history:
            if (auto* match = std::get_if<chess::ChessMatch>(&game)) {
                if (!preview_ply) {
                    paused_clock_elapsed = elapsed_now();
                }
                preview_ply = std::min(command->ply, match->history().size());
                static_cast<void>(publish_game());
            }
            break;
        case CommandKind::return_live:
            active_clock_started = std::chrono::steady_clock::now() -
                                   std::chrono::milliseconds{
                                       paused_clock_elapsed.value_or(0)};
            preview_ply.reset();
            paused_clock_elapsed.reset();
            static_cast<void>(publish_game());
            break;
        case CommandKind::open_match: {
            auto bytes = read_binary_file(command->path, 1U << 24U);
            auto loaded = bytes ? chess::ChessMatch::load(**runtime, *bytes)
                                : std::expected<chess::ChessMatch, Diagnostic>{
                                      std::unexpected(bytes.error())};
            if (!loaded) {
                static_cast<void>(publish_game(nullptr, diagnostic_text(loaded.error())));
                break;
            }
            Game candidate{std::in_place_type<chess::ChessMatch>, std::move(*loaded)};
            auto candidate_presentation = create_presentation(candidate);
            if (!candidate_presentation) {
                static_cast<void>(publish_game(nullptr,
                                               diagnostic_text(candidate_presentation.error())));
                break;
            }
            chess_settings = std::get<chess::ChessMatch>(candidate).settings();
            game = std::move(candidate);
            presentation = std::move(*candidate_presentation);
            preview_ply.reset();
            active_clock_started = std::chrono::steady_clock::now();
            static_cast<void>(autosave());
            static_cast<void>(publish_game());
            break;
        }
        case CommandKind::save_match: {
            const auto* match = std::get_if<chess::ChessMatch>(&game);
            if (match == nullptr) {
                static_cast<void>(publish_game(nullptr, "Native match saves are chess-only"));
                break;
            }
            const auto bytes = match->save();
            const auto saved = write_atomic(command->path, std::span{bytes});
            static_cast<void>(publish_game(nullptr, saved ? "Match saved"
                                                           : diagnostic_text(saved.error())));
            break;
        }
        case CommandKind::import_pgn: {
            auto text = read_text_file(command->path, 1U << 20U);
            if (!text) {
                static_cast<void>(publish_game(nullptr, diagnostic_text(text.error())));
                break;
            }
            auto loaded = chess::import_pgn(**runtime, *text);
            if (!loaded) {
                static_cast<void>(publish_game(
                    nullptr, loaded.error().message + " at " +
                                 std::to_string(loaded.error().line) + ':' +
                                 std::to_string(loaded.error().column)));
                break;
            }
            Game candidate{std::in_place_type<chess::ChessMatch>, std::move(*loaded)};
            auto candidate_presentation = create_presentation(candidate);
            if (!candidate_presentation) {
                static_cast<void>(publish_game(nullptr,
                                               diagnostic_text(candidate_presentation.error())));
                break;
            }
            chess_settings = std::get<chess::ChessMatch>(candidate).settings();
            game = std::move(candidate);
            presentation = std::move(*candidate_presentation);
            preview_ply.reset();
            active_clock_started = std::chrono::steady_clock::now();
            static_cast<void>(autosave());
            static_cast<void>(publish_game());
            break;
        }
        case CommandKind::import_pgn_text: {
            auto loaded = chess::import_pgn(**runtime, command->text);
            if (!loaded) {
                static_cast<void>(publish_game(
                    nullptr, loaded.error().message + " at " +
                                 std::to_string(loaded.error().line) + ':' +
                                 std::to_string(loaded.error().column)));
                break;
            }
            Game candidate{std::in_place_type<chess::ChessMatch>, std::move(*loaded)};
            auto candidate_presentation = create_presentation(candidate);
            if (!candidate_presentation) {
                static_cast<void>(publish_game(nullptr,
                                               diagnostic_text(candidate_presentation.error())));
                break;
            }
            chess_settings = std::get<chess::ChessMatch>(candidate).settings();
            game = std::move(candidate);
            presentation = std::move(*candidate_presentation);
            preview_ply.reset();
            active_clock_started = std::chrono::steady_clock::now();
            static_cast<void>(autosave());
            static_cast<void>(publish_game());
            break;
        }
        case CommandKind::export_pgn: {
            const auto* match = std::get_if<chess::ChessMatch>(&game);
            if (match == nullptr) {
                static_cast<void>(publish_game(nullptr, "PGN export is chess-only"));
                break;
            }
            const auto saved = write_atomic(command->path, chess::export_pgn(*match));
            static_cast<void>(publish_game(nullptr, saved ? "PGN exported"
                                                           : diagnostic_text(saved.error())));
            break;
        }
        }
    }
}

} // namespace ludus::player
