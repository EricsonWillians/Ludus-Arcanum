"""Declarative movement and transactional effects for orthodox chess."""

from ludus_arcanum import action, move


ORTHOGONAL = ("north", "east", "south", "west")
DIAGONAL = ("north_east", "south_east", "south_west", "north_west")
ADJACENT = ORTHOGONAL + DIAGONAL
KNIGHT = (
    "knight_nne",
    "knight_ene",
    "knight_ese",
    "knight_sse",
    "knight_ssw",
    "knight_wsw",
    "knight_wnw",
    "knight_nnw",
)

MOVEMENT_RULES = {
    "rook": move.rays(ORTHOGONAL).until_blocked().allow_empty().capture_enemy(),
    "bishop": move.rays(DIAGONAL).until_blocked().allow_empty().capture_enemy(),
    "queen": move.rays(ADJACENT).until_blocked().allow_empty().capture_enemy(),
    "king": move.jumps(ADJACENT).allow_empty().capture_enemy(),
    "knight": move.jumps(KNIGHT).allow_empty().capture_enemy(),
}

CAPTURE = 1 << 0
KING_CASTLE = 1 << 3
QUEEN_CASTLE = 1 << 4
PROMOTION = 1 << 5


@action("chess_move")
def chess_move(ctx, tx, actor, targets):
    """Apply one native-validated move through the controlled transaction surface."""

    flags = ctx.argument("move_flags")
    cursor = 2
    if flags & CAPTURE:
        tx.destroy(targets[cursor])
        cursor += 1

    tx.move(actor, targets[0])
    if flags & (KING_CASTLE | QUEEN_CASTLE):
        tx.move(targets[cursor], targets[cursor + 1])

    if flags & PROMOTION:
        tx.set_property(actor, "piece_type", ctx.argument("promotion"))

    metadata = targets[1]
    tx.set_property(metadata, "side_to_move", ctx.argument("next_side"))
    tx.set_property(metadata, "castling_rights", ctx.argument("next_castling"))
    tx.set_property(metadata, "en_passant", ctx.argument("next_en_passant"))
    tx.set_property(metadata, "halfmove_clock", ctx.argument("next_halfmove"))
    tx.set_property(metadata, "fullmove_number", ctx.argument("next_fullmove"))
