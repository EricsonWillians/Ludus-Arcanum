from dataclasses import FrozenInstanceError
from pathlib import Path
import tomllib

import pytest

from orthodox_chess import MOVEMENT_RULES
from orthodox_chess import rules


def test_manifest_declares_a_portable_python_package():
    manifest_path = Path(__file__).parents[1] / "package.toml"
    manifest = tomllib.loads(manifest_path.read_text(encoding="utf-8"))

    package = manifest["package"]
    assert package["id"] == "org.ludus-arcanum.orthodox-chess"
    assert package["version"] == "1.0.0"
    assert package["engine_api"] == ">=0.1.0,<0.2.0"
    assert package["entry_point"] == "orthodox_chess.rules"
    assert package["save_compatibility"] == 1
    assert package["visuals"] == "visuals/theme.toml"
    expected_pieces = {
        f"assets/pieces/{material}/{piece}.png"
        for material in ("ivory", "iron")
        for piece in ("pawn", "knight", "bishop", "rook", "queen", "king")
    }
    assert set(package["assets"]) == expected_pieces | {
        "assets/ui/ivory-crest.png",
        "assets/ui/iron-crest.png",
        "assets/ui/frame-corner.png",
        "assets/ui/board-material.png",
    }
    assert package["permissions"] == []
    assert package["dependencies"] == []
    assert manifest["native"]["enabled_by_default"] is False


def test_piece_movement_is_declarative_and_immutable():
    assert set(MOVEMENT_RULES) == {"bishop", "king", "knight", "queen", "rook"}
    assert MOVEMENT_RULES["bishop"].to_spec()["kind"] == "rays"
    assert MOVEMENT_RULES["knight"].to_spec()["kind"] == "jumps"
    with pytest.raises(FrozenInstanceError):
        MOVEMENT_RULES["rook"].distance = 4


def test_package_registers_its_transactional_move_resolver():
    assert rules.__ludus_actions__ == {"chess_move": rules.chess_move}
