#pragma once

#include "ludus/core/diagnostic.hpp"

#include <array>
#include <compare>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ludus::chess {

enum class Color : std::uint8_t { white, black };

enum class PieceType : std::uint8_t {
    none,
    pawn,
    knight,
    bishop,
    rook,
    queen,
    king,
};

struct Piece {
    PieceType type{PieceType::none};
    Color color{Color::white};

    [[nodiscard]] constexpr bool empty() const noexcept { return type == PieceType::none; }
    auto operator<=>(const Piece&) const = default;
};

enum class MoveFlag : std::uint8_t {
    none = 0U,
    capture = 1U << 0U,
    double_push = 1U << 1U,
    en_passant = 1U << 2U,
    king_castle = 1U << 3U,
    queen_castle = 1U << 4U,
    promotion = 1U << 5U,
};

[[nodiscard]] constexpr MoveFlag operator|(MoveFlag left, MoveFlag right) noexcept {
    return static_cast<MoveFlag>(static_cast<std::uint8_t>(left) |
                                 static_cast<std::uint8_t>(right));
}

[[nodiscard]] constexpr bool has_flag(MoveFlag value, MoveFlag flag) noexcept {
    return (static_cast<std::uint8_t>(value) & static_cast<std::uint8_t>(flag)) != 0U;
}

struct ChessMove {
    std::uint8_t from{0U};
    std::uint8_t to{0U};
    PieceType promotion{PieceType::none};
    MoveFlag flags{MoveFlag::none};

    auto operator<=>(const ChessMove&) const = default;
};

enum class PositionStatus : std::uint8_t { ongoing, checkmate, stalemate };

class Position {
  public:
    static constexpr std::uint8_t white_king_side = 1U << 0U;
    static constexpr std::uint8_t white_queen_side = 1U << 1U;
    static constexpr std::uint8_t black_king_side = 1U << 2U;
    static constexpr std::uint8_t black_queen_side = 1U << 3U;

    [[nodiscard]] static Position initial();
    [[nodiscard]] static std::expected<Position, Diagnostic> from_fen(std::string_view fen);
    [[nodiscard]] static std::expected<Position, Diagnostic>
    from_components(std::array<Piece, 64U> board, Color side_to_move,
                    std::uint8_t castling_rights,
                    std::optional<std::uint8_t> en_passant_square,
                    std::uint32_t halfmove_clock, std::uint32_t fullmove_number);
    [[nodiscard]] std::string to_fen() const;

    [[nodiscard]] Color side_to_move() const noexcept { return side_to_move_; }
    [[nodiscard]] std::uint8_t castling_rights() const noexcept { return castling_rights_; }
    [[nodiscard]] std::optional<std::uint8_t> en_passant_square() const noexcept;
    [[nodiscard]] std::uint32_t halfmove_clock() const noexcept { return halfmove_clock_; }
    [[nodiscard]] std::uint32_t fullmove_number() const noexcept { return fullmove_number_; }
    [[nodiscard]] Piece piece_at(std::uint8_t square) const noexcept;

    [[nodiscard]] bool in_check(Color color) const noexcept;
    [[nodiscard]] std::vector<ChessMove> legal_moves() const;
    [[nodiscard]] std::expected<ChessMove, Diagnostic>
    find_legal_move(std::string_view uci) const;
    [[nodiscard]] std::expected<void, Diagnostic> apply(ChessMove move);
    [[nodiscard]] PositionStatus status() const;

  private:
    struct UndoState {
        Piece moved;
        Piece captured;
        std::int16_t captured_square{-1};
        std::uint8_t castling_rights{0U};
        std::int16_t en_passant_square{-1};
        std::uint32_t halfmove_clock{0U};
        std::uint32_t fullmove_number{1U};
    };

    [[nodiscard]] bool square_attacked(std::uint8_t square, Color attacker) const noexcept;
    [[nodiscard]] std::vector<ChessMove> pseudo_legal_moves() const;
    [[nodiscard]] std::vector<ChessMove> legal_moves_in_place();
    [[nodiscard]] UndoState make_unchecked(ChessMove move) noexcept;
    void unmake_unchecked(ChessMove move, const UndoState& undo) noexcept;

    std::array<Piece, 64U> board_{};
    Color side_to_move_{Color::white};
    std::uint8_t castling_rights_{0U};
    std::int16_t en_passant_square_{-1};
    std::uint32_t halfmove_clock_{0U};
    std::uint32_t fullmove_number_{1U};

    friend std::uint64_t perft(Position position, std::uint32_t depth);
};

[[nodiscard]] std::string to_uci(ChessMove move);
/// Convert a legal move to Standard Algebraic Notation without mutating the position.
[[nodiscard]] std::expected<std::string, Diagnostic> to_san(const Position& position,
                                                            ChessMove move);
[[nodiscard]] std::uint64_t perft(Position position, std::uint32_t depth);

} // namespace ludus::chess
