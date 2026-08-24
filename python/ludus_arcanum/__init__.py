"""Strongly typed Python authoring SDK for Ludus Arcanum."""

from .authoring import MovementRule, action, compile_rule, move

try:
    from . import _native
except ImportError:
    try:
        import _ludus_arcanum_native as _native
    except ImportError:
        _native = None

NATIVE_AVAILABLE = _native is not None

if NATIVE_AVAILABLE:
    ActionHandle = _native.ActionHandle
    EntityHandle = _native.EntityHandle
    PlayerHandle = _native.PlayerHandle
    RuleContext = _native.RuleContext
    RuleProgram = _native.RuleProgram
    SpaceHandle = _native.SpaceHandle
    Transaction = _native.Transaction

__all__ = [
    "MovementRule",
    "NATIVE_AVAILABLE",
    "action",
    "compile_rule",
    "move",
]

if NATIVE_AVAILABLE:
    __all__ += [
        "ActionHandle",
        "EntityHandle",
        "PlayerHandle",
        "RuleContext",
        "RuleProgram",
        "SpaceHandle",
        "Transaction",
    ]
