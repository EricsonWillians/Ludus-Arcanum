from dataclasses import FrozenInstanceError

import pytest

from ludus_arcanum import NATIVE_AVAILABLE, MovementRule, compile_rule, move


def test_movement_dsl_is_immutable_and_chainable():
    base = move.rays(("diagonal",))
    complete = base.until_blocked().allow_empty().capture_enemy()

    assert base.stop_at_blocker is False
    assert complete.to_spec() == {
        "kind": "rays",
        "directions": ("diagonal",),
        "distance": 0,
        "until_blocked": True,
        "allow_empty": True,
        "capture_enemy": True,
    }
    with pytest.raises(FrozenInstanceError):
        complete.distance = 4


@pytest.mark.parametrize("distance", [0, -1])
def test_jump_distance_is_validated_in_python(distance):
    with pytest.raises(ValueError, match="positive"):
        move.jumps(("forward",), distance=distance)


@pytest.mark.skipif(not NATIVE_AVAILABLE, reason="native extension was not built")
def test_python_rule_lowers_to_native_bytecode():
    rule = move.rays(("forward",)).until_blocked().allow_empty().capture_enemy()
    program = compile_rule(rule, {"forward": 1})

    assert program.direction_count == 1
    assert program.instruction_count == 5
    assert program.hash != 0


@pytest.mark.skipif(not NATIVE_AVAILABLE, reason="native extension was not built")
def test_native_lowering_rejects_unknown_direction():
    rule = move.jumps(("missing",), distance=2).allow_empty()
    with pytest.raises(ValueError, match="unknown direction"):
        compile_rule(rule, {"forward": 1})


@pytest.mark.skipif(not NATIVE_AVAILABLE, reason="native extension was not built")
def test_native_handles_cannot_be_forged_from_python():
    from ludus_arcanum import ActionHandle, EntityHandle, PlayerHandle, SpaceHandle

    for handle_type in (ActionHandle, EntityHandle, PlayerHandle, SpaceHandle):
        with pytest.raises(TypeError):
            handle_type()
