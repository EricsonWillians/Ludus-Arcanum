#include "player_window.hpp"

#include <gtkmm/application.h>

#include <iostream>
#include <charconv>
#include <cstddef>
#include <string_view>

int main(int argc, char* argv[]) {
    auto game = ludus::player::PlayerGame::chess;
    auto renderer = ludus::RendererPreference::automatic;
    bool renderer_info = false;
    bool hot_seat = false;
    std::size_t stress_sprites = 0U;
    int output = 1;
    for (int input = 1; input < argc; ++input) {
        const std::string_view argument{argv[input]};
        std::string_view value;
        enum class Option { none, game_option, renderer_option } option{Option::none};
        if (argument == "--game" && input + 1 < argc) {
            value = argv[++input];
            option = Option::game_option;
        } else if (argument.starts_with("--game=")) {
            value = argument.substr(7U);
            option = Option::game_option;
        } else if (argument == "--renderer" && input + 1 < argc) {
            value = argv[++input];
            option = Option::renderer_option;
        } else if (argument.starts_with("--renderer=")) {
            value = argument.substr(11U);
            option = Option::renderer_option;
        } else if (argument == "--renderer-info") {
            renderer_info = true;
            continue;
        } else if (argument == "--hot-seat") {
            hot_seat = true;
            continue;
        } else if (argument == "--stress-sprites" && input + 1 < argc) {
            const std::string_view count{argv[++input]};
            const auto parsed = std::from_chars(count.data(), count.data() + count.size(),
                                                stress_sprites);
            if (parsed.ec != std::errc{} || parsed.ptr != count.data() + count.size() ||
                stress_sprites == 0U || stress_sprites > 100'000U) {
                std::cerr << "ludus-player: --stress-sprites requires 1..100000\n";
                return 2;
            }
            renderer_info = true;
            continue;
        } else if (argument.starts_with("--stress-sprites=")) {
            const auto count = argument.substr(17U);
            const auto parsed = std::from_chars(count.data(), count.data() + count.size(),
                                                stress_sprites);
            if (parsed.ec != std::errc{} || parsed.ptr != count.data() + count.size() ||
                stress_sprites == 0U || stress_sprites > 100'000U) {
                std::cerr << "ludus-player: --stress-sprites requires 1..100000\n";
                return 2;
            }
            renderer_info = true;
            continue;
        } else {
            argv[output++] = argv[input];
            continue;
        }
        if (option == Option::game_option) {
            if (value == "chess") {
                game = ludus::player::PlayerGame::chess;
            } else if (value == "tactical") {
                game = ludus::player::PlayerGame::tactical;
            } else {
                std::cerr << "ludus-player: --game must be 'chess' or 'tactical'\n";
                return 2;
            }
        } else if (value == "auto") {
            renderer = ludus::RendererPreference::automatic;
        } else if (value == "gl") {
            renderer = ludus::RendererPreference::desktop_gl;
        } else if (value == "gles") {
            renderer = ludus::RendererPreference::gles;
        } else if (value == "software") {
            renderer = ludus::RendererPreference::software;
        } else {
            std::cerr << "ludus-player: --renderer must be 'auto', 'gl', 'gles', or "
                         "'software'\n";
            return 2;
        }
    }
    argc = output;
    argv[argc] = nullptr;
    const auto application = Gtk::Application::create("org.ludus-arcanum.player");
    return application->make_window_and_run<ludus::player::PlayerWindow>(
        argc, argv, game, renderer, renderer_info, stress_sprites, hot_seat);
}
