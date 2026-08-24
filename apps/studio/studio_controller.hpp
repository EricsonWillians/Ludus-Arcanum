#pragma once

#include "ludus/chess/chess.hpp"
#include "ludus/render/snapshot.hpp"

#include <glibmm/dispatcher.h>

#include <array>
#include <atomic>
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

namespace ludus::studio_app {

struct PlaytestConfiguration {
    std::filesystem::path package_root;
    std::string module_name;
    chess::Position initial_position;
};

struct StudioView {
    RenderSnapshot snapshot;
    std::string diagnostics;
    std::string event_log;
    std::string state_inspector;
    bool active{false};
};

class StudioController {
  public:
    explicit StudioController(std::vector<std::string> python_search_paths);
    StudioController(const StudioController&) = delete;
    StudioController& operator=(const StudioController&) = delete;
    StudioController(StudioController&&) = delete;
    StudioController& operator=(StudioController&&) = delete;
    ~StudioController();

    void start();
    void stop();

    [[nodiscard]] bool launch(PlaytestConfiguration configuration);
    [[nodiscard]] bool move(std::uint64_t action_token);
    [[nodiscard]] bool undo();
    [[nodiscard]] bool redo();
    [[nodiscard]] bool restart();
    [[nodiscard]] bool reload_rules();

    [[nodiscard]] std::shared_ptr<const StudioView> view() const noexcept {
        return view_.load();
    }
    [[nodiscard]] Glib::Dispatcher& dispatcher() noexcept { return dispatcher_; }

  private:
    enum class CommandKind : std::uint8_t { launch, move, undo, redo, restart, reload };
    struct Command {
        CommandKind kind{CommandKind::restart};
        std::uint64_t token{0U};
        std::shared_ptr<const PlaytestConfiguration> configuration;
    };

    template <std::size_t Capacity>
    class CommandQueue {
      public:
        [[nodiscard]] bool try_push(Command command) {
            std::scoped_lock lock{mutex_};
            if (closed_ || size_ == Capacity) {
                return false;
            }
            entries_[tail_] = std::move(command);
            tail_ = (tail_ + 1U) % Capacity;
            ++size_;
            ready_.notify_one();
            return true;
        }

        [[nodiscard]] std::optional<Command> pop(std::stop_token stop) {
            std::unique_lock lock{mutex_};
            static_cast<void>(ready_.wait(lock, stop, [this] {
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

    [[nodiscard]] bool enqueue(CommandKind kind, std::uint64_t token = 0U);
    void run(std::stop_token stop);

    std::vector<std::string> python_search_paths_;
    Glib::Dispatcher dispatcher_;
    CommandQueue<64U> commands_;
    std::atomic<std::shared_ptr<const StudioView>> view_;
    std::optional<std::jthread> worker_;
};

} // namespace ludus::studio_app
