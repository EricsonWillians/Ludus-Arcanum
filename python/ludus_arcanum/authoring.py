"""Immutable rule authoring primitives lowered by the native runtime."""

from __future__ import annotations

from dataclasses import dataclass, replace
import sys
from typing import Any, Callable, Iterable, Mapping, TypeVar

RuleCallback = TypeVar("RuleCallback", bound=Callable[..., None])


def action(name: str) -> Callable[[RuleCallback], RuleCallback]:
    """Register a trusted Python callback on its defining game module."""

    if not name or not isinstance(name, str):
        raise ValueError("action name must be a non-empty string")

    def decorate(callback: RuleCallback) -> RuleCallback:
        module = sys.modules[callback.__module__]
        registry = module.__dict__.setdefault("__ludus_actions__", {})
        if name in registry:
            raise ValueError(f"duplicate action registration: {name}")
        registry[name] = callback
        return callback

    return decorate


@dataclass(frozen=True, slots=True)
class MovementRule:
    """Immutable movement graph description; it never contains authoritative state."""

    kind: str
    directions: tuple[str, ...]
    distance: int = 0
    stop_at_blocker: bool = False
    emits_empty: bool = False
    captures_enemy: bool = False

    def __post_init__(self) -> None:
        if self.kind not in {"rays", "jumps"}:
            raise ValueError(f"unsupported traversal kind: {self.kind}")
        if not self.directions or any(not name for name in self.directions):
            raise ValueError("movement rules require non-empty direction names")
        if self.kind == "jumps" and self.distance <= 0:
            raise ValueError("jump distance must be positive")
        if self.kind == "rays" and self.distance < 0:
            raise ValueError("ray distance cannot be negative")

    def until_blocked(self) -> MovementRule:
        if self.kind != "rays":
            raise ValueError("until_blocked is only valid for rays")
        return replace(self, stop_at_blocker=True)

    def allow_empty(self) -> MovementRule:
        return replace(self, emits_empty=True)

    def capture_enemy(self) -> MovementRule:
        return replace(self, captures_enemy=True)

    def to_spec(self) -> dict[str, Any]:
        return {
            "kind": self.kind,
            "directions": self.directions,
            "distance": self.distance,
            "until_blocked": self.stop_at_blocker,
            "allow_empty": self.emits_empty,
            "capture_enemy": self.captures_enemy,
        }


class _MoveFactory:
    def rays(self, directions: Iterable[str], *, max_steps: int = 0) -> MovementRule:
        return MovementRule("rays", tuple(directions), max_steps)

    def jumps(self, directions: Iterable[str], *, distance: int = 1) -> MovementRule:
        return MovementRule("jumps", tuple(directions), distance)


move = _MoveFactory()


def compile_rule(rule: MovementRule, direction_ids: Mapping[str, int]) -> Any:
    """Lower a rule through the optional native extension used by tools and tests."""

    from . import _native

    if _native is None:
        raise RuntimeError("the Ludus Arcanum native Python module is unavailable")
    return _native.compile_movement(rule, dict(direction_ids))
