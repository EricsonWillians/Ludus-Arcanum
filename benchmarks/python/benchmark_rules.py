from ludus_arcanum import action


@action("noop")
def noop(ctx, tx, actor, targets):
    del ctx, tx, actor, targets
