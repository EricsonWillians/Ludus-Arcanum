#pragma once

#include "ludus/chess/match.hpp"
#include "ludus/render/exchange.hpp"

#include <glibmm/dispatcher.h>

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace ludus::player {

enum class PlayerGame : std::uint8_t { chess, tactical };

class PlayerController {
  public:
    PlayerController(PlayerGame game, std::vector<std::string> python_search_paths,
                     bool hot_seat = false);
    PlayerController(const PlayerController&) = delete;
    PlayerController& operator=(const PlayerController&) = delete;
    PlayerController(PlayerController&&) = delete;
    PlayerController& operator=(PlayerController&&) = delete;
    ~PlayerController();

    void start();
    void stop();

    [[nodiscard]] bool submit(std::uint64_t action_token);
    [[nodiscard]] bool undo();
    [[nodiscard]] bool redo();
    [[nodiscard]] bool restart();
    [[nodiscard]] bool replay();
    [[nodiscard]] bool new_chess_match(chess::ChessMatchSettings settings);
    [[nodiscard]] bool resign(chess::Color color);
    [[nodiscard]] bool agree_draw();
    [[nodiscard]] bool claim_draw(chess::MatchResultReason reason,
                                  std::optional<std::uint64_t> intended_token = std::nullopt);
    [[nodiscard]] bool preview_history(std::optional<std::size_t> ply);
    [[nodiscard]] bool open_match(std::filesystem::path path);
    [[nodiscard]] bool save_match(std::filesystem::path path);
    [[nodiscard]] bool import_pgn(std::filesystem::path path);
    [[nodiscard]] bool import_pgn_text(std::string text);
    [[nodiscard]] bool export_pgn(std::filesystem::path path);

    [[nodiscard]] PlayerGame game() const noexcept { return game_; }
    [[nodiscard]] static std::filesystem::path autosave_path();

    [[nodiscard]] std::shared_ptr<const PlayerView> view() const noexcept {
        return views_.load();
    }
    [[nodiscard]] Glib::Dispatcher& dispatcher() noexcept { return dispatcher_; }

  private:
    enum class CommandKind : std::uint8_t {
        action,
        undo,
        redo,
        restart,
        replay,
        new_match,
        resign,
        agree_draw,
        claim_draw,
        preview_history,
        return_live,
        open_match,
        save_match,
        import_pgn,
        import_pgn_text,
        export_pgn,
    };
    struct Command {
        CommandKind kind{CommandKind::restart};
        std::uint64_t token{0U};
        std::optional<chess::ChessMatchSettings> settings;
        std::filesystem::path path;
        std::string text;
        std::size_t ply{0U};
        chess::Color color{chess::Color::white};
        chess::MatchResultReason reason{chess::MatchResultReason::none};
        std::optional<std::uint64_t> intended_token;
    };

    template <std::size_t Capacity>
    class CommandQueue {
      public:
        [[nodiscard]] bool try_push(Command command) {
            std::scoped_lock lock{mutex_};
            if (closed_ || size_ == Capacity) {
                return false;
            }
            entries_[tail_] = command;
            tail_ = (tail_ + 1U) % Capacity;
            ++size_;
            ready_.notify_one();
            return true;
        }

        [[nodiscard]] std::optional<Command>
        pop_for(std::stop_token stop, std::chrono::milliseconds timeout) {
            std::unique_lock lock{mutex_};
            static_cast<void>(ready_.wait_for(lock, stop, timeout, [this] {
                return closed_ || size_ != 0U;
            }));
            if (size_ == 0U) {
                return std::nullopt;
            }
            auto result = std::move(entries_[head_]);
            entries_[head_].reset();
            head_ = (head_ + 1U) % Capacity;
            --size_;
            return result;
        }

        void close() noexcept {
            {
                std::scoped_lock lock{mutex_};
                closed_ = true;
            }
            ready_.notify_all();
        }

      private:
        std::array<std::optional<Command>, Capacity> entries_{};
        std::size_t head_{0U};
        std::size_t tail_{0U};
        std::size_t size_{0U};
        bool closed_{false};
        std::mutex mutex_;
        std::condition_variable_any ready_;
    };

    [[nodiscard]] bool enqueue(Command command);
    [[nodiscard]] bool enqueue(CommandKind kind, std::uint64_t token = 0U);
    void run(std::stop_token stop);

    PlayerGame game_;
    bool hot_seat_{false};
    std::vector<std::string> python_search_paths_;
    Glib::Dispatcher dispatcher_;
    CommandQueue<64U> commands_;
    PlayerViewExchange views_;
    std::optional<std::jthread> worker_;
};

} // namespace ludus::player
