#include "studio_controller.hpp"

#include "ludus/chess/game.hpp"
#include "ludus/chess/presentation.hpp"
#include "ludus/python/runtime.hpp"
#include "ludus/studio/inspection.hpp"

#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ludus::studio_app {
namespace {

std::string diagnostic_text(const Diagnostic& diagnostic) {
    auto result = diagnostic.message;
    if (!diagnostic.source.path.empty()) {
        result += "\n" + diagnostic.source.path;
        if (diagnostic.source.line != 0U) {
            result += ':' + std::to_string(diagnostic.source.line);
            if (diagnostic.source.column != 0U) {
                result += ':' + std::to_string(diagnostic.source.column);
            }
        }
    }
    if (!diagnostic.detail.empty()) {
        result += "\n" + diagnostic.detail;
    }
    return result;
}

} // namespace

StudioController::StudioController(std::vector<std::string> python_search_paths)
    : python_search_paths_(std::move(python_search_paths)) {}

StudioController::~StudioController() { stop(); }

void StudioController::start() {
    if (!worker_) {
        worker_.emplace([this](std::stop_token stop) { run(stop); });
    }
}

void StudioController::stop() {
    commands_.close();
    if (worker_) {
        worker_->request_stop();
        worker_->join();
        worker_.reset();
    }
}

bool StudioController::launch(PlaytestConfiguration configuration) {
    return commands_.try_push(Command{CommandKind::launch, 0U,
                                      std::make_shared<PlaytestConfiguration>(
                                          std::move(configuration))});
}

bool StudioController::move(std::uint64_t action_token) {
    return enqueue(CommandKind::move, action_token);
}

bool StudioController::undo() { return enqueue(CommandKind::undo); }

bool StudioController::redo() { return enqueue(CommandKind::redo); }

bool StudioController::restart() { return enqueue(CommandKind::restart); }

bool StudioController::reload_rules() { return enqueue(CommandKind::reload); }

bool StudioController::enqueue(CommandKind kind, std::uint64_t token) {
    return commands_.try_push(Command{kind, token, {}});
}

void StudioController::run(std::stop_token stop) {
    std::unique_ptr<PythonRuntime> runtime;
    std::optional<chess::ChessGame> game;
    std::optional<chess::ChessPresentation> presentation;
    std::shared_ptr<const PlaytestConfiguration> configuration;
    std::vector<EventBatch> event_batches;
    std::size_t event_cursor = 0U;
    std::uint64_t revision = 0U;
    std::shared_ptr<const StudioView> current_view;

    const auto notify = [this] {
        static_cast<const Glib::Dispatcher&>(dispatcher_).emit();
    };
    const auto publish_view = [this, &current_view, &notify](StudioView next) {
        current_view = std::make_shared<const StudioView>(std::move(next));
        view_.store(current_view);
        notify();
    };
    const auto publish_failure = [&publish_view, &revision, &current_view](
                                     std::string message, bool preserve_board) {
        StudioView failed;
        if (preserve_board && current_view) {
            failed = *current_view;
        }
        failed.snapshot.revision = ++revision;
        failed.snapshot.status = "Error: " + message;
        failed.diagnostics = std::move(message);
        publish_view(std::move(failed));
    };
    const auto publish_game = [&]() -> std::expected<void, Diagnostic> {
        if (!game || !presentation) {
            return std::unexpected(Diagnostic{DiagnosticCode::invalid_state,
                                              "no playtest is active", {}});
        }
        const auto* previous = current_view ? &current_view->snapshot : nullptr;
        auto snapshot = presentation->build(*game, ++revision, previous);
        if (!snapshot) {
            return std::unexpected(snapshot.error());
        }
        StudioView next;
        next.snapshot = std::move(*snapshot);
        next.event_log = studio::inspect_event_log(event_batches, event_cursor,
                                                    game->session().state().symbols());
        next.state_inspector = studio::inspect_state(game->session().state());
        next.active = true;
        publish_view(std::move(next));
        return {};
    };
    const auto create_game = [&]() -> std::expected<void, Diagnostic> {
        if (!runtime || !configuration) {
            return std::unexpected(Diagnostic{DiagnosticCode::invalid_state,
                                              "playtest runtime is not configured", {}});
        }
        auto replacement = chess::ChessGame::create(
            *runtime, configuration->initial_position, configuration->module_name);
        if (!replacement) {
            return std::unexpected(replacement.error());
        }
        auto replacement_presentation = chess::ChessPresentation::create(*replacement);
        if (!replacement_presentation) {
            return std::unexpected(replacement_presentation.error());
        }
        game = std::move(*replacement);
        presentation = std::move(*replacement_presentation);
        event_batches.clear();
        event_cursor = 0U;
        return {};
    };

    while (!stop.stop_requested()) {
        const auto command = commands_.pop(stop);
        if (!command) {
            continue;
        }

        if (command->kind == CommandKind::launch) {
            presentation.reset();
            game.reset();
            runtime.reset();
            configuration = command->configuration;
            current_view.reset();
            event_batches.clear();
            event_cursor = 0U;

            auto search_paths = python_search_paths_;
            search_paths.push_back(configuration->package_root.string());
            auto created_runtime = PythonRuntime::create(search_paths);
            if (!created_runtime) {
                publish_failure(diagnostic_text(created_runtime.error()), false);
                continue;
            }
            runtime = std::move(*created_runtime);
            if (auto created = create_game(); !created) {
                publish_failure(diagnostic_text(created.error()), false);
                continue;
            }
            if (auto published = publish_game(); !published) {
                publish_failure(diagnostic_text(published.error()), false);
            }
            continue;
        }

        if (!game || !presentation || !runtime || !configuration) {
            publish_failure("Start a playtest before issuing simulation commands", false);
            continue;
        }

        switch (command->kind) {
        case CommandKind::launch:
            break;
        case CommandKind::move: {
            auto batch = game->submit(chess::decode_action_token(command->token));
            if (!batch) {
                publish_failure(diagnostic_text(batch.error()), true);
                break;
            }
            if (event_cursor < event_batches.size()) {
                event_batches.resize(event_cursor);
            }
            event_batches.push_back(std::move(*batch));
            event_cursor = event_batches.size();
            if (auto published = publish_game(); !published) {
                publish_failure(diagnostic_text(published.error()), true);
            }
            break;
        }
        case CommandKind::undo:
            if (auto undone = game->undo(); !undone) {
                publish_failure(diagnostic_text(undone.error()), true);
            } else {
                if (event_cursor != 0U) {
                    --event_cursor;
                }
                if (auto published = publish_game(); !published) {
                    publish_failure(diagnostic_text(published.error()), true);
                }
            }
            break;
        case CommandKind::redo:
            if (auto redone = game->redo(); !redone) {
                publish_failure(diagnostic_text(redone.error()), true);
            } else {
                if (event_cursor < event_batches.size()) {
                    ++event_cursor;
                }
                if (auto published = publish_game(); !published) {
                    publish_failure(diagnostic_text(published.error()), true);
                }
            }
            break;
        case CommandKind::restart:
            if (auto restarted = create_game(); !restarted) {
                publish_failure(diagnostic_text(restarted.error()), true);
            } else if (auto published = publish_game(); !published) {
                publish_failure(diagnostic_text(published.error()), true);
            }
            break;
        case CommandKind::reload: {
            auto reloaded = game->reload_rules();
            if (!reloaded) {
                publish_failure(diagnostic_text(reloaded.error()), true);
                break;
            }
            if (auto published = publish_game(); !published) {
                publish_failure(diagnostic_text(published.error()), true);
                break;
            }
            auto next = *current_view;
            next.snapshot.status = "Rules reloaded safely (generation " +
                                   std::to_string(runtime->generation()) + ')';
            next.diagnostics = "Reload accepted: the complete candidate rule set compiled "
                               "before replacing the active programs.";
            publish_view(std::move(next));
            break;
        }
        }
    }
}

} // namespace ludus::studio_app
