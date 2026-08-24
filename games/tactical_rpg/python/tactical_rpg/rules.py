"""Transactional tactical-combat effects; spatial legality stays native and package-local."""

from ludus_arcanum import action


QUICK_SHOT = 1
FOCUSED_SHOT = 2
VENOM_SHOT = 3
SHIELD_BASH = 4
GUARD = 5
ARC_BOLT = 6
WARD = 7
CRUSH = 8
BULWARK = 9
AMBUSH = 11
BLIGHT_BOLT = 12
DRAIN = 13


def _optional_property(ctx, entity, name, fallback=0):
    """Read a scenario-v2 property while retaining scenario-v1 save compatibility."""

    try:
        value = ctx.property(entity, name)
        return fallback if value is None else value
    except (KeyError, RuntimeError, ValueError):
        return fallback


def _optional_argument(ctx, name, fallback=0):
    try:
        value = ctx.argument(name)
        return fallback if value is None else value
    except (KeyError, RuntimeError, ValueError):
        return fallback


@action("resolve_attack")
def resolve_attack(ctx, tx, actor, targets):
    """Resolve the chosen attack, including card, status, and reaction triggers."""

    ability = ctx.argument("ability")
    target = targets[0]

    defensive = ability in (GUARD, WARD, BULWARK)
    if defensive:
        status = {GUARD: "guarded", WARD: "warded", BULWARK: "bulwark"}[ability]
        tx.set_property(actor, "armor_bonus", 2)
        tx.add_tag(actor, status)
        return

    expression = {
        QUICK_SHOT: "1d6",
        FOCUSED_SHOT: "2d6",
        VENOM_SHOT: "1d4",
        SHIELD_BASH: "1d4",
        ARC_BOLT: "1d6",
        CRUSH: "1d8",
        AMBUSH: "1d6",
        BLIGHT_BOLT: "1d4",
        DRAIN: "1d6",
    }[ability]
    rolled = tx.roll(expression, "combat")

    attack = ctx.property(actor, "attack")
    armor = 0 if ability == ARC_BOLT else (
        ctx.property(target, "armor") + _optional_property(ctx, target, "armor_bonus")
        + _optional_argument(ctx, "cover_bonus")
    )
    situational = 2 if ability == AMBUSH else 0
    damage = max(1, rolled.total + attack + situational - armor)
    target_health = ctx.property(target, "health")
    damage_dealt = min(target_health, damage)
    tx.set_property(target, "health", max(0, target_health - damage_dealt))

    if ability == FOCUSED_SHOT:
        tx.move(targets[1], targets[2])
    elif ability in (VENOM_SHOT, BLIGHT_BOLT):
        tx.add_tag(target, "poisoned")
        tx.set_property(target, "poison_ticks", 2)
    elif ability == DRAIN:
        kind = ctx.property(actor, "kind")
        maximum = {
            "ranger": 12,
            "warden": 16,
            "arcanist": 10,
            "thorn_guardian": 18,
            "guardian": 18,
            "stalker": 10,
            "scout": 10,
            "hexer": 11,
        }.get(kind, 12)
        tx.set_property(actor, "health", min(maximum, ctx.property(actor, "health") + damage_dealt))

    # Package-defined reaction trigger. The generic transaction only records effects.
    if ctx.has_tag(target, "thorns") and ability in (SHIELD_BASH, CRUSH, AMBUSH):
        actor_health = ctx.property(actor, "health")
        tx.set_property(actor, "health", max(0, actor_health - 2))
