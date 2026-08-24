from pathlib import Path
import tomllib

from tactical_rpg import rules


def test_manifest_declares_an_external_tactical_package():
    manifest_path = Path(__file__).parents[1] / "package.toml"
    manifest = tomllib.loads(manifest_path.read_text(encoding="utf-8"))

    assert manifest["package"]["id"] == "org.ludus-arcanum.tactical-rpg"
    assert manifest["package"]["entry_point"] == "tactical_rpg.rules"
    assert manifest["package"]["permissions"] == []
    assert manifest["native"]["enabled_by_default"] is False


def test_package_registers_its_transactional_attack_resolver():
    assert rules.__ludus_actions__ == {"resolve_attack": rules.resolve_attack}
    assert {
        rules.QUICK_SHOT,
        rules.FOCUSED_SHOT,
        rules.VENOM_SHOT,
        rules.SHIELD_BASH,
        rules.GUARD,
        rules.ARC_BOLT,
        rules.WARD,
        rules.CRUSH,
        rules.BULWARK,
        rules.AMBUSH,
        rules.BLIGHT_BOLT,
        rules.DRAIN,
    } == {1, 2, 3, 4, 5, 6, 7, 8, 9, 11, 12, 13}
