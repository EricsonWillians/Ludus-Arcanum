#include "ludus/chess/chess.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <sstream>
#include <string>
#include <utility>

namespace ludus::chess {
namespace {

constexpr Piece empty_piece{};

constexpr Color opposite(Color color) noexcept {
    return color == Color::white ? Color::black : Color::white;
}

constexpr bool on_board(int file, int rank) noexcept {
    return file >= 0 && file < 8 && rank >= 0 && rank < 8;
}

constexpr std::uint8_t make_square(int file, int rank) noexcept {
    return static_cast<std::uint8_t>(rank * 8 + file);
}

constexpr int file_of(std::uint8_t square) noexcept {
    return static_cast<int>(square % 8U);
}

constexpr int rank_of(std::uint8_t square) noexcept {
    return static_cast<int>(square / 8U);
}

constexpr std::size_t board_index(std::uint8_t square) noexcept {
    return static_cast<std::size_t>(square);
}

constexpr std::array<PieceType, 4U> promotion_types{
    PieceType::queen, PieceType::rook, PieceType::bishop, PieceType::knight};

constexpr std::array<std::pair<int, int>, 8U> knight_offsets{{
    {1, 2}, {2, 1}, {2, -1}, {1, -2}, {-1, -2}, {-2, -1}, {-2, 1}, {-1, 2},
}};

constexpr std::array<std::pair<int, int>, 8U> king_offsets{{
    {0, 1}, {1, 1}, {1, 0}, {1, -1}, {0, -1}, {-1, -1}, {-1, 0}, {-1, 1},
}};

constexpr std::array<std::pair<int, int>, 4U> bishop_directions{{
    {1, 1}, {1, -1}, {-1, -1}, {-1, 1},
}};

constexpr std::array<std::pair<int, int>, 4U> rook_directions{{
    {0, 1}, {1, 0}, {0, -1}, {-1, 0},
}};

Diagnostic invalid_fen(std::string message) {
    return Diagnostic{DiagnosticCode::validation_failed,
                      "invalid chess FEN: " + std::move(message), {}};
}

std::optional<std::uint8_t> parse_square(std::string_view name) noexcept {
    if (name.size() != 2U || name[0] < 'a' || name[0] > 'h' || name[1] < '1' ||
        name[1] > '8') {
        return std::nullopt;
    }
    return make_square(static_cast<int>(name[0] - 'a'), static_cast<int>(name[1] - '1'));
}

std::string square_name(std::uint8_t square) {
    std::string result(2U, ' ');
    result[0] = static_cast<char>('a' + file_of(square));
    result[1] = static_cast<char>('1' + rank_of(square));
    return result;
}

Piece piece_from_fen(char symbol) noexcept {
    const auto color = symbol >= 'A' && symbol <= 'Z' ? Color::white : Color::black;
    switch (symbol) {
    case 'P':
    case 'p':
        return {PieceType::pawn, color};
    case 'N':
    case 'n':
        return {PieceType::knight, color};
    case 'B':
    case 'b':
        return {PieceType::bishop, color};
    case 'R':
    case 'r':
        return {PieceType::rook, color};
    case 'Q':
    case 'q':
        return {PieceType::queen, color};
    case 'K':
    case 'k':
        return {PieceType::king, color};
    default:
        return {};
    }
}

char fen_symbol(Piece piece) noexcept {
    char result = ' ';
    switch (piece.type) {
    case PieceType::pawn:
        result = 'p';
        break;
    case PieceType::knight:
        result = 'n';
        break;
    case PieceType::bishop:
        result = 'b';
        break;
    case PieceType::rook:
        result = 'r';
        break;
    case PieceType::queen:
        result = 'q';
        break;
    case PieceType::king:
        result = 'k';
        break;
    case PieceType::none:
        break;
    }
    if (piece.color == Color::white && result != ' ') {
        result = static_cast<char>(result - ('a' - 'A'));
    }
    return result;
}

template <typename Integer>
bool parse_integer(std::string_view text, Integer& destination) noexcept {
    const auto* begin = text.data();
    const auto* end = text.data() + text.size();
    const auto parsed = std::from_chars(begin, end, destination);
    return parsed.ec == std::errc{} && parsed.ptr == end;
}

void add_promotion_moves(std::vector<ChessMove>& moves, std::uint8_t from, std::uint8_t to,
                         MoveFlag flags) {
    for (const auto promotion : promotion_types) {
        moves.push_back(ChessMove{from, to, promotion, flags | MoveFlag::promotion});
    }
}

PieceType promotion_from_char(char symbol) noexcept {
    switch (symbol) {
    case 'q':
        return PieceType::queen;
    case 'r':
        return PieceType::rook;
    case 'b':
        return PieceType::bishop;
    case 'n':
        return PieceType::knight;
    default:
        return PieceType::none;
    }
}

char promotion_to_char(PieceType type) noexcept {
    switch (type) {
    case PieceType::queen:
        return 'q';
    case PieceType::rook:
        return 'r';
    case PieceType::bishop:
        return 'b';
    case PieceType::knight:
        return 'n';
    case PieceType::none:
    case PieceType::pawn:
    case PieceType::king:
        return '\0';
    }
    return '\0';
}

} // namespace

Position Position::initial() {
    const auto position = from_fen("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    return *position;
}

std::expected<Position, Diagnostic> Position::from_fen(std::string_view fen) {
    std::istringstream stream{std::string{fen}};
    std::string placement;
    std::string side;
    std::string castling;
    std::string en_passant;
    std::string halfmove;
    std::string fullmove;
    std::string trailing;
    if (!(stream >> placement >> side >> castling >> en_passant >> halfmove >> fullmove) ||
        (stream >> trailing)) {
        return std::unexpected(invalid_fen("expected exactly six fields"));
    }

    Position result;
    int rank = 7;
    int file = 0;
    std::uint32_t white_kings = 0U;
    std::uint32_t black_kings = 0U;
    for (const char symbol : placement) {
        if (symbol == '/') {
            if (file != 8 || rank == 0) {
                return std::unexpected(invalid_fen("rank width is invalid"));
            }
            --rank;
            file = 0;
            continue;
        }
        if (symbol >= '1' && symbol <= '8') {
            file += static_cast<int>(symbol - '0');
            if (file > 8) {
                return std::unexpected(invalid_fen("rank exceeds eight files"));
            }
            continue;
        }
        const auto piece = piece_from_fen(symbol);
        if (piece.empty() || file >= 8 || rank < 0) {
            return std::unexpected(invalid_fen("piece placement is invalid"));
        }
        result.board_[board_index(make_square(file, rank))] = piece;
        if (piece.type == PieceType::king) {
            piece.color == Color::white ? ++white_kings : ++black_kings;
        }
        ++file;
    }
    if (rank != 0 || file != 8 || white_kings != 1U || black_kings != 1U) {
        return std::unexpected(invalid_fen("position must contain eight ranks and both kings"));
    }

    if (side == "w") {
        result.side_to_move_ = Color::white;
    } else if (side == "b") {
        result.side_to_move_ = Color::black;
    } else {
        return std::unexpected(invalid_fen("active color is invalid"));
    }

    if (castling != "-") {
        for (const char right : castling) {
            std::uint8_t bit = 0U;
            switch (right) {
            case 'K':
                bit = white_king_side;
                break;
            case 'Q':
                bit = white_queen_side;
                break;
            case 'k':
                bit = black_king_side;
                break;
            case 'q':
                bit = black_queen_side;
                break;
            default:
                return std::unexpected(invalid_fen("castling rights are invalid"));
            }
            if ((result.castling_rights_ & bit) != 0U) {
                return std::unexpected(invalid_fen("castling rights are duplicated"));
            }
            result.castling_rights_ = static_cast<std::uint8_t>(result.castling_rights_ | bit);
        }
    }

    if (en_passant != "-") {
        const auto square = parse_square(en_passant);
        if (!square || (rank_of(*square) != 2 && rank_of(*square) != 5) ||
            (result.side_to_move_ == Color::white && rank_of(*square) != 5) ||
            (result.side_to_move_ == Color::black && rank_of(*square) != 2)) {
            return std::unexpected(invalid_fen("en-passant target is invalid"));
        }
        result.en_passant_square_ = static_cast<std::int16_t>(*square);
    }

    if (!parse_integer(halfmove, result.halfmove_clock_) ||
        !parse_integer(fullmove, result.fullmove_number_) || result.fullmove_number_ == 0U) {
        return std::unexpected(invalid_fen("move counters are invalid"));
    }
    return result;
}

std::expected<Position, Diagnostic>
Position::from_components(std::array<Piece, 64U> board, Color side_to_move,
                          std::uint8_t castling_rights,
                          std::optional<std::uint8_t> en_passant_square,
                          std::uint32_t halfmove_clock, std::uint32_t fullmove_number) {
    if ((castling_rights & 0xf0U) != 0U ||
        (en_passant_square && *en_passant_square >= 64U) || fullmove_number == 0U) {
        return std::unexpected(invalid_fen("native position metadata is invalid"));
    }
    std::uint32_t white_kings = 0U;
    std::uint32_t black_kings = 0U;
    for (const auto piece : board) {
        if (piece.type == PieceType::king) {
            piece.color == Color::white ? ++white_kings : ++black_kings;
        } else if (piece.type > PieceType::king) {
            return std::unexpected(invalid_fen("native piece type is invalid"));
        }
    }
    if (white_kings != 1U || black_kings != 1U) {
        return std::unexpected(invalid_fen("native position must contain both kings"));
    }
    Position result;
    result.board_ = std::move(board);
    result.side_to_move_ = side_to_move;
    result.castling_rights_ = castling_rights;
    result.en_passant_square_ = en_passant_square
                                    ? static_cast<std::int16_t>(*en_passant_square)
                                    : static_cast<std::int16_t>(-1);
    result.halfmove_clock_ = halfmove_clock;
    result.fullmove_number_ = fullmove_number;
    return result;
}

std::string Position::to_fen() const {
    std::string result;
    for (int rank = 7; rank >= 0; --rank) {
        int empty = 0;
        for (int file = 0; file < 8; ++file) {
            const auto piece = board_[board_index(make_square(file, rank))];
            if (piece.empty()) {
                ++empty;
                continue;
            }
            if (empty != 0) {
                result.push_back(static_cast<char>('0' + empty));
                empty = 0;
            }
            result.push_back(fen_symbol(piece));
        }
        if (empty != 0) {
            result.push_back(static_cast<char>('0' + empty));
        }
        if (rank != 0) {
            result.push_back('/');
        }
    }
    result += side_to_move_ == Color::white ? " w " : " b ";
    if (castling_rights_ == 0U) {
        result.push_back('-');
    } else {
        if ((castling_rights_ & white_king_side) != 0U) {
            result.push_back('K');
        }
        if ((castling_rights_ & white_queen_side) != 0U) {
            result.push_back('Q');
        }
        if ((castling_rights_ & black_king_side) != 0U) {
            result.push_back('k');
        }
        if ((castling_rights_ & black_queen_side) != 0U) {
            result.push_back('q');
        }
    }
    result.push_back(' ');
    result += en_passant_square_ < 0
                  ? std::string{"-"}
                  : square_name(static_cast<std::uint8_t>(en_passant_square_));
    result.push_back(' ');
    result += std::to_string(halfmove_clock_);
    result.push_back(' ');
    result += std::to_string(fullmove_number_);
    return result;
}

std::optional<std::uint8_t> Position::en_passant_square() const noexcept {
    return en_passant_square_ < 0
               ? std::nullopt
               : std::optional<std::uint8_t>{static_cast<std::uint8_t>(en_passant_square_)};
}

Piece Position::piece_at(std::uint8_t square) const noexcept {
    return square < 64U ? board_[board_index(square)] : empty_piece;
}

bool Position::square_attacked(std::uint8_t square, Color attacker) const noexcept {
    const int target_file = file_of(square);
    const int target_rank = rank_of(square);
    const int pawn_source_rank = target_rank + (attacker == Color::white ? -1 : 1);
    for (const int file_delta : {-1, 1}) {
        const int source_file = target_file + file_delta;
        if (on_board(source_file, pawn_source_rank)) {
            const auto piece = board_[board_index(make_square(source_file, pawn_source_rank))];
            if (piece == Piece{PieceType::pawn, attacker}) {
                return true;
            }
        }
    }

    for (const auto& [file_delta, rank_delta] : knight_offsets) {
        const int source_file = target_file + file_delta;
        const int source_rank = target_rank + rank_delta;
        if (on_board(source_file, source_rank) &&
            board_[board_index(make_square(source_file, source_rank))] ==
                Piece{PieceType::knight, attacker}) {
            return true;
        }
    }
    for (const auto& [file_delta, rank_delta] : king_offsets) {
        const int source_file = target_file + file_delta;
        const int source_rank = target_rank + rank_delta;
        if (on_board(source_file, source_rank) &&
            board_[board_index(make_square(source_file, source_rank))] ==
                Piece{PieceType::king, attacker}) {
            return true;
        }
    }

    const auto ray_attacked = [this, target_file, target_rank, attacker](
                                  const auto& directions, PieceType primary) {
        for (const auto& [file_delta, rank_delta] : directions) {
            int file = target_file + file_delta;
            int rank = target_rank + rank_delta;
            while (on_board(file, rank)) {
                const auto piece = board_[board_index(make_square(file, rank))];
                if (!piece.empty()) {
                    if (piece.color == attacker &&
                        (piece.type == primary || piece.type == PieceType::queen)) {
                        return true;
                    }
                    break;
                }
                file += file_delta;
                rank += rank_delta;
            }
        }
        return false;
    };
    return ray_attacked(bishop_directions, PieceType::bishop) ||
           ray_attacked(rook_directions, PieceType::rook);
}

bool Position::in_check(Color color) const noexcept {
    for (std::uint8_t square = 0U; square < 64U; ++square) {
        if (board_[board_index(square)] == Piece{PieceType::king, color}) {
            return square_attacked(square, opposite(color));
        }
    }
    return true;
}

std::vector<ChessMove> Position::pseudo_legal_moves() const {
    std::vector<ChessMove> moves;
    moves.reserve(96U);
    const auto add_destination = [this, &moves](std::uint8_t from, std::uint8_t to) {
        const auto target = board_[board_index(to)];
        if (target.empty()) {
            moves.push_back({from, to, PieceType::none, MoveFlag::none});
        } else if (target.color != side_to_move_ && target.type != PieceType::king) {
            moves.push_back({from, to, PieceType::none, MoveFlag::capture});
        }
    };

    for (std::uint8_t from = 0U; from < 64U; ++from) {
        const auto piece = board_[board_index(from)];
        if (piece.empty() || piece.color != side_to_move_) {
            continue;
        }
        const int from_file = file_of(from);
        const int from_rank = rank_of(from);
        if (piece.type == PieceType::pawn) {
            const int advance = piece.color == Color::white ? 1 : -1;
            const int start_rank = piece.color == Color::white ? 1 : 6;
            const int promotion_rank = piece.color == Color::white ? 7 : 0;
            const int one_rank = from_rank + advance;
            if (on_board(from_file, one_rank)) {
                const auto one = make_square(from_file, one_rank);
                if (board_[board_index(one)].empty()) {
                    if (one_rank == promotion_rank) {
                        add_promotion_moves(moves, from, one, MoveFlag::none);
                    } else {
                        moves.push_back({from, one, PieceType::none, MoveFlag::none});
                        const int two_rank = from_rank + advance * 2;
                        if (from_rank == start_rank) {
                            const auto two = make_square(from_file, two_rank);
                            if (board_[board_index(two)].empty()) {
                                moves.push_back(
                                    {from, two, PieceType::none, MoveFlag::double_push});
                            }
                        }
                    }
                }
            }
            for (const int file_delta : {-1, 1}) {
                const int to_file = from_file + file_delta;
                const int to_rank = from_rank + advance;
                if (!on_board(to_file, to_rank)) {
                    continue;
                }
                const auto to = make_square(to_file, to_rank);
                const auto target = board_[board_index(to)];
                if (!target.empty() && target.color != piece.color &&
                    target.type != PieceType::king) {
                    if (to_rank == promotion_rank) {
                        add_promotion_moves(moves, from, to, MoveFlag::capture);
                    } else {
                        moves.push_back({from, to, PieceType::none, MoveFlag::capture});
                    }
                } else if (en_passant_square_ == static_cast<std::int16_t>(to)) {
                    const auto captured = make_square(to_file, from_rank);
                    if (board_[board_index(captured)] ==
                        Piece{PieceType::pawn, opposite(piece.color)}) {
                        moves.push_back({from, to, PieceType::none,
                                         MoveFlag::capture | MoveFlag::en_passant});
                    }
                }
            }
            continue;
        }

        if (piece.type == PieceType::knight || piece.type == PieceType::king) {
            const auto& offsets = piece.type == PieceType::knight ? knight_offsets : king_offsets;
            for (const auto& [file_delta, rank_delta] : offsets) {
                const int to_file = from_file + file_delta;
                const int to_rank = from_rank + rank_delta;
                if (on_board(to_file, to_rank)) {
                    add_destination(from, make_square(to_file, to_rank));
                }
            }
        }

        if (piece.type == PieceType::bishop || piece.type == PieceType::rook ||
            piece.type == PieceType::queen) {
            const auto add_rays = [this, &moves, from, from_file, from_rank](
                                      const auto& directions) {
                for (const auto& [file_delta, rank_delta] : directions) {
                    int to_file = from_file + file_delta;
                    int to_rank = from_rank + rank_delta;
                    while (on_board(to_file, to_rank)) {
                        const auto to = make_square(to_file, to_rank);
                        const auto target = board_[board_index(to)];
                        if (target.empty()) {
                            moves.push_back({from, to, PieceType::none, MoveFlag::none});
                        } else {
                            if (target.color != side_to_move_ && target.type != PieceType::king) {
                                moves.push_back(
                                    {from, to, PieceType::none, MoveFlag::capture});
                            }
                            break;
                        }
                        to_file += file_delta;
                        to_rank += rank_delta;
                    }
                }
            };
            if (piece.type == PieceType::bishop || piece.type == PieceType::queen) {
                add_rays(bishop_directions);
            }
            if (piece.type == PieceType::rook || piece.type == PieceType::queen) {
                add_rays(rook_directions);
            }
        }

        if (piece.type != PieceType::king) {
            continue;
        }
        const int home_rank = piece.color == Color::white ? 0 : 7;
        if (from != make_square(4, home_rank) || in_check(piece.color)) {
            continue;
        }
        const auto enemy = opposite(piece.color);
        const auto king_bit = piece.color == Color::white ? white_king_side : black_king_side;
        if ((castling_rights_ & king_bit) != 0U &&
            board_[board_index(make_square(5, home_rank))].empty() &&
            board_[board_index(make_square(6, home_rank))].empty() &&
            board_[board_index(make_square(7, home_rank))] ==
                Piece{PieceType::rook, piece.color} &&
            !square_attacked(make_square(5, home_rank), enemy) &&
            !square_attacked(make_square(6, home_rank), enemy)) {
            moves.push_back({from, make_square(6, home_rank), PieceType::none,
                             MoveFlag::king_castle});
        }
        const auto queen_bit = piece.color == Color::white ? white_queen_side : black_queen_side;
        if ((castling_rights_ & queen_bit) != 0U &&
            board_[board_index(make_square(1, home_rank))].empty() &&
            board_[board_index(make_square(2, home_rank))].empty() &&
            board_[board_index(make_square(3, home_rank))].empty() &&
            board_[board_index(make_square(0, home_rank))] ==
                Piece{PieceType::rook, piece.color} &&
            !square_attacked(make_square(3, home_rank), enemy) &&
            !square_attacked(make_square(2, home_rank), enemy)) {
            moves.push_back({from, make_square(2, home_rank), PieceType::none,
                             MoveFlag::queen_castle});
        }
    }
    return moves;
}

Position::UndoState Position::make_unchecked(ChessMove move) noexcept {
    const auto moved = board_[board_index(move.from)];
    const auto captured_square = has_flag(move.flags, MoveFlag::en_passant)
                                     ? static_cast<std::int16_t>(
                                           static_cast<int>(move.to) +
                                           (moved.color == Color::white ? -8 : 8))
                                     : static_cast<std::int16_t>(move.to);
    const auto captured = board_[static_cast<std::size_t>(captured_square)];
    const UndoState undo{moved, captured, captured_square, castling_rights_,
                         en_passant_square_, halfmove_clock_, fullmove_number_};

    board_[board_index(move.from)] = empty_piece;
    board_[static_cast<std::size_t>(captured_square)] = empty_piece;
    board_[board_index(move.to)] = has_flag(move.flags, MoveFlag::promotion)
                                       ? Piece{move.promotion, moved.color}
                                       : moved;

    const int home_rank = moved.color == Color::white ? 0 : 7;
    if (has_flag(move.flags, MoveFlag::king_castle)) {
        board_[board_index(make_square(5, home_rank))] =
            board_[board_index(make_square(7, home_rank))];
        board_[board_index(make_square(7, home_rank))] = empty_piece;
    } else if (has_flag(move.flags, MoveFlag::queen_castle)) {
        board_[board_index(make_square(3, home_rank))] =
            board_[board_index(make_square(0, home_rank))];
        board_[board_index(make_square(0, home_rank))] = empty_piece;
    }

    const auto clear_rights = [this](std::uint8_t mask) {
        castling_rights_ = static_cast<std::uint8_t>(castling_rights_ & ~mask);
    };
    if (moved.type == PieceType::king) {
        clear_rights(moved.color == Color::white
                         ? static_cast<std::uint8_t>(white_king_side | white_queen_side)
                         : static_cast<std::uint8_t>(black_king_side | black_queen_side));
    }
    if (moved.type == PieceType::rook) {
        if (move.from == make_square(0, 0)) {
            clear_rights(white_queen_side);
        } else if (move.from == make_square(7, 0)) {
            clear_rights(white_king_side);
        } else if (move.from == make_square(0, 7)) {
            clear_rights(black_queen_side);
        } else if (move.from == make_square(7, 7)) {
            clear_rights(black_king_side);
        }
    }
    if (captured.type == PieceType::rook) {
        if (captured_square == static_cast<std::int16_t>(make_square(0, 0))) {
            clear_rights(white_queen_side);
        } else if (captured_square == static_cast<std::int16_t>(make_square(7, 0))) {
            clear_rights(white_king_side);
        } else if (captured_square == static_cast<std::int16_t>(make_square(0, 7))) {
            clear_rights(black_queen_side);
        } else if (captured_square == static_cast<std::int16_t>(make_square(7, 7))) {
            clear_rights(black_king_side);
        }
    }

    en_passant_square_ = -1;
    if (has_flag(move.flags, MoveFlag::double_push)) {
        en_passant_square_ = static_cast<std::int16_t>(
            (static_cast<int>(move.from) + static_cast<int>(move.to)) / 2);
    }
    halfmove_clock_ = moved.type == PieceType::pawn || !captured.empty()
                          ? 0U
                          : halfmove_clock_ + 1U;
    if (side_to_move_ == Color::black) {
        ++fullmove_number_;
    }
    side_to_move_ = opposite(side_to_move_);
    return undo;
}

void Position::unmake_unchecked(ChessMove move, const UndoState& undo) noexcept {
    side_to_move_ = opposite(side_to_move_);
    castling_rights_ = undo.castling_rights;
    en_passant_square_ = undo.en_passant_square;
    halfmove_clock_ = undo.halfmove_clock;
    fullmove_number_ = undo.fullmove_number;

    const int home_rank = undo.moved.color == Color::white ? 0 : 7;
    if (has_flag(move.flags, MoveFlag::king_castle)) {
        board_[board_index(make_square(7, home_rank))] =
            board_[board_index(make_square(5, home_rank))];
        board_[board_index(make_square(5, home_rank))] = empty_piece;
    } else if (has_flag(move.flags, MoveFlag::queen_castle)) {
        board_[board_index(make_square(0, home_rank))] =
            board_[board_index(make_square(3, home_rank))];
        board_[board_index(make_square(3, home_rank))] = empty_piece;
    }

    board_[board_index(move.from)] = undo.moved;
    board_[board_index(move.to)] = empty_piece;
    board_[static_cast<std::size_t>(undo.captured_square)] = undo.captured;
}

std::vector<ChessMove> Position::legal_moves_in_place() {
    const auto candidates = pseudo_legal_moves();
    std::vector<ChessMove> result;
    result.reserve(candidates.size());
    const auto moving_color = side_to_move_;
    for (const auto move : candidates) {
        const auto undo = make_unchecked(move);
        if (!in_check(moving_color)) {
            result.push_back(move);
        }
        unmake_unchecked(move, undo);
    }
    return result;
}

std::vector<ChessMove> Position::legal_moves() const {
    auto copy = *this;
    return copy.legal_moves_in_place();
}

std::expected<ChessMove, Diagnostic> Position::find_legal_move(std::string_view uci) const {
    if (uci.size() != 4U && uci.size() != 5U) {
        return std::unexpected(Diagnostic{DiagnosticCode::validation_failed,
                                          "chess move must use long algebraic notation", {}});
    }
    const auto from = parse_square(uci.substr(0U, 2U));
    const auto to = parse_square(uci.substr(2U, 2U));
    const auto promotion = uci.size() == 5U ? promotion_from_char(uci[4]) : PieceType::none;
    if (!from || !to || (uci.size() == 5U && promotion == PieceType::none)) {
        return std::unexpected(Diagnostic{DiagnosticCode::validation_failed,
                                          "chess move notation is invalid", {}});
    }
    for (const auto move : legal_moves()) {
        if (move.from == *from && move.to == *to && move.promotion == promotion) {
            return move;
        }
    }
    return std::unexpected(
        Diagnostic{DiagnosticCode::validation_failed, "chess move is not legal", {}});
}

std::expected<void, Diagnostic> Position::apply(ChessMove move) {
    const auto legal = legal_moves();
    const auto found = std::ranges::find(legal, move);
    if (found == legal.end()) {
        return std::unexpected(
            Diagnostic{DiagnosticCode::validation_failed, "chess move is not legal", {}});
    }
    static_cast<void>(make_unchecked(move));
    return {};
}

PositionStatus Position::status() const {
    if (!legal_moves().empty()) {
        return PositionStatus::ongoing;
    }
    return in_check(side_to_move_) ? PositionStatus::checkmate : PositionStatus::stalemate;
}

std::string to_uci(ChessMove move) {
    auto result = square_name(move.from) + square_name(move.to);
    if (const char promotion = promotion_to_char(move.promotion); promotion != '\0') {
        result.push_back(promotion);
    }
    return result;
}

std::expected<std::string, Diagnostic> to_san(const Position& position, ChessMove requested) {
    const auto legal = position.legal_moves();
    const auto matching = std::ranges::find_if(legal, [requested](ChessMove candidate) {
        return candidate.from == requested.from && candidate.to == requested.to &&
               candidate.promotion == requested.promotion;
    });
    if (matching == legal.end()) {
        return std::unexpected(Diagnostic{DiagnosticCode::validation_failed,
                                          "SAN requires a legal chess move", {}});
    }
    const auto move = *matching;
    std::string result;
    if (has_flag(move.flags, MoveFlag::king_castle)) {
        result = "O-O";
    } else if (has_flag(move.flags, MoveFlag::queen_castle)) {
        result = "O-O-O";
    } else {
        const auto piece = position.piece_at(move.from);
        constexpr std::array<char, 7U> piece_letters{'\0', '\0', 'N', 'B', 'R', 'Q', 'K'};
        const bool capture = has_flag(move.flags, MoveFlag::capture) ||
                             has_flag(move.flags, MoveFlag::en_passant);
        if (piece.type != PieceType::pawn) {
            result.push_back(piece_letters[static_cast<std::size_t>(piece.type)]);
            bool competing = false;
            bool same_file = false;
            bool same_rank = false;
            for (const auto candidate : legal) {
                if (candidate.from == move.from || candidate.to != move.to ||
                    position.piece_at(candidate.from).type != piece.type) {
                    continue;
                }
                competing = true;
                same_file = same_file || candidate.from % 8U == move.from % 8U;
                same_rank = same_rank || candidate.from / 8U == move.from / 8U;
            }
            if (competing) {
                if (!same_file) {
                    result.push_back(static_cast<char>('a' + move.from % 8U));
                } else if (!same_rank) {
                    result.push_back(static_cast<char>('1' + move.from / 8U));
                } else {
                    result.push_back(static_cast<char>('a' + move.from % 8U));
                    result.push_back(static_cast<char>('1' + move.from / 8U));
                }
            }
        } else if (capture) {
            result.push_back(static_cast<char>('a' + move.from % 8U));
        }
        if (capture) {
            result.push_back('x');
        }
        result.push_back(static_cast<char>('a' + move.to % 8U));
        result.push_back(static_cast<char>('1' + move.to / 8U));
        if (move.promotion != PieceType::none) {
            result.push_back('=');
            result.push_back(piece_letters[static_cast<std::size_t>(move.promotion)]);
        }
    }
    auto after = position;
    if (auto applied = after.apply(move); !applied) {
        return std::unexpected(applied.error());
    }
    if (after.in_check(after.side_to_move())) {
        result.push_back(after.status() == PositionStatus::checkmate ? '#' : '+');
    }
    return result;
}

std::uint64_t perft(Position position, std::uint32_t depth) {
    const auto visit = [](auto&& self, Position& current,
                          std::uint32_t remaining_depth) -> std::uint64_t {
        if (remaining_depth == 0U) {
            return 1U;
        }
        const auto moves = current.legal_moves_in_place();
        if (remaining_depth == 1U) {
            return static_cast<std::uint64_t>(moves.size());
        }
        std::uint64_t nodes = 0U;
        for (const auto move : moves) {
            const auto undo = current.make_unchecked(move);
            nodes += self(self, current, remaining_depth - 1U);
            current.unmake_unchecked(move, undo);
        }
        return nodes;
    };
    return visit(visit, position, depth);
}

} // namespace ludus::chess
