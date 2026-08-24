from ludus_arcanum import action, move


MOVEMENT_RULES = {
    "slider": (
        move.rays(("forward",))
        .until_blocked()
        .allow_empty()
        .capture_enemy()
    ),
    "jumper": move.jumps(("forward",), distance=2).allow_empty().capture_enemy(),
}

SAVED_CONTEXT = None


@action("python_move")
def python_move(ctx, tx, actor, targets):
    assert ctx.action.value > 0
    assert ctx.issuer.generation > 0
    assert ctx.actor == actor
    assert ctx.targets == targets
    assert ctx.entity(actor).location is not None
    tx.move(actor, targets[0])


@action("python_failure")
def python_failure(ctx, tx, actor, targets):
    tx.move(actor, targets[0])
    raise RuntimeError("deliberate Python failure")


@action("remember_context")
def remember_context(ctx, tx, actor, targets):
    global SAVED_CONTEXT
    SAVED_CONTEXT = ctx
    tx.move(actor, targets[0])


@action("use_expired_context")
def use_expired_context(ctx, tx, actor, targets):
    del ctx, tx, actor
    SAVED_CONTEXT.entities_at(targets[0])


@action("python_roll")
def python_roll(ctx, tx, actor, targets):
    del ctx, targets
    result = tx.roll("2d6+1", "python-test")
    tx.set_property(actor, "last_roll", result.total)
