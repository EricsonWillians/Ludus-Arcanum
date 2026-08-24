#pragma once

#include "ludus/chess/game.hpp"
#include "ludus/chess/match.hpp"
#include "ludus/render/atlas.hpp"
#include "ludus/render/player_view.hpp"
#include "ludus/render/snapshot.hpp"

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>

namespace ludus::chess {

class ChessPresentation {
  public:
    [[nodiscard]] static std::expected<ChessPresentation, Diagnostic>
    create(const ChessGame& game);

    [[nodiscard]] std::expected<RenderSnapshot, Diagnostic>
    build(const ChessGame& game, std::uint64_t revision,
          const RenderSnapshot* previous = nullptr, const EventBatch* batch = nullptr,
          std::chrono::steady_clock::time_point animation_start =
              std::chrono::steady_clock::now()) const;

    [[nodiscard]] std::expected<PlayerView, Diagnostic>
    build_view(const ChessMatch& match, std::uint64_t revision,
               const RenderSnapshot* previous = nullptr,
               const EventBatch* batch = nullptr,
               std::chrono::steady_clock::time_point animation_start =
                   std::chrono::steady_clock::now(),
               bool reduced_motion = false) const;

  private:
    ChessPresentation(TagId piece_tag, PropertyId piece_type)
        : piece_tag_(piece_tag), piece_type_(piece_type) {}

    TagId piece_tag_;
    PropertyId piece_type_;
};

[[nodiscard]] constexpr std::uint64_t encode_action_token(ChessMove move) noexcept {
    return static_cast<std::uint64_t>(move.from) |
           (static_cast<std::uint64_t>(move.to) << 8U) |
           (static_cast<std::uint64_t>(move.promotion) << 16U);
}

[[nodiscard]] constexpr ChessMove decode_action_token(std::uint64_t token) noexcept {
    return ChessMove{static_cast<std::uint8_t>(token & 0xffU),
                     static_cast<std::uint8_t>((token >> 8U) & 0xffU),
                     static_cast<PieceType>((token >> 16U) & 0xffU), MoveFlag::none};
}

[[nodiscard]] std::expected<TextureAtlas, Diagnostic> make_default_chess_atlas();
[[nodiscard]] std::expected<TextureAtlas, Diagnostic>
load_chess_atlas(const std::filesystem::path& path);

} // namespace ludus::chess
