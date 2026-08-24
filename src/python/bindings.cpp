#include "bindings.hpp"

#include "ludus/core/symbol.hpp"
#include "ludus/core/value.hpp"
#include "ludus/rules/random.hpp"
#include "ludus/topology/topology.hpp"

#include <pybind11/stl.h>
#include <pybind11/operators.h>

#include <algorithm>
#include <concepts>
#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <stdexcept>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace ludus::python_detail {
namespace {

struct EntityView {
    EntityHandle entity;
    std::optional<SpaceHandle> location;
    std::optional<PlayerHandle> owner;
    std::vector<std::uint32_t> tags;
};

const CallbackCapability& require_capability(
    const std::shared_ptr<CallbackCapability>& capability, bool require_transaction) {
    if (!capability || !capability->active || capability->state == nullptr ||
        capability->intent == nullptr ||
        (require_transaction && capability->transaction == nullptr)) {
        throw std::runtime_error{"Python rule capability has expired"};
    }
    return *capability;
}

void throw_diagnostic(const Diagnostic& diagnostic) {
    throw py::value_error{diagnostic.message};
}

PropertyValue property_from_python(py::handle value) {
    if (py::isinstance<py::bool_>(value)) {
        return py::cast<bool>(value);
    }
    if (py::isinstance<py::int_>(value)) {
        return py::cast<std::int64_t>(value);
    }
    if (py::isinstance<py::str>(value)) {
        return py::cast<std::string>(value);
    }
    throw py::type_error{"authoritative properties accept bool, int, or str"};
}

py::object property_to_python(const PropertyValue& value) {
    return std::visit(
        [](const auto& typed) -> py::object {
            using T = std::remove_cvref_t<decltype(typed)>;
            if constexpr (std::same_as<T, Fixed>) {
                return py::int_{typed.raw()};
            } else {
                return py::cast(typed);
            }
        },
        value);
}

PropertyId property_id(const GameState& state, std::string_view name) {
    const auto property = state.symbols().properties.find(name);
    if (!property) {
        throw py::key_error{property.error()};
    }
    return *property;
}

TagId tag_id(const GameState& state, std::string_view name) {
    const auto tag = state.symbols().tags.find(name);
    if (!tag) {
        throw py::key_error{tag.error()};
    }
    return *tag;
}

DirectionId direction_id(const GameState& state, std::string_view name) {
    const auto direction = state.symbols().directions.find(name);
    if (!direction) {
        throw py::key_error{direction.error()};
    }
    return *direction;
}

py::dict rule_to_spec(py::handle rule) {
    py::object object = py::reinterpret_borrow<py::object>(rule);
    if (py::hasattr(object, "to_spec")) {
        object = object.attr("to_spec")();
    }
    if (!py::isinstance<py::dict>(object)) {
        throw py::type_error{"movement rule must provide to_spec() returning dict"};
    }
    return py::reinterpret_borrow<py::dict>(object);
}

template <typename ResolveDirection>
std::expected<RuleProgram, Diagnostic> compile_spec(py::handle rule,
                                                    ResolveDirection&& resolve_direction) {
    try {
        const auto spec = rule_to_spec(rule);
        const auto kind = py::cast<std::string>(spec["kind"]);
        const auto direction_names = py::cast<std::vector<std::string>>(spec["directions"]);
        MovementRuleGraph graph;
        graph.directions.reserve(direction_names.size());
        for (const auto& name : direction_names) {
            auto direction = resolve_direction(name);
            if (!direction) {
                return std::unexpected(direction.error());
            }
            graph.directions.push_back(*direction);
        }

        if (kind == "rays") {
            const auto max_steps = py::cast<std::uint32_t>(spec["distance"]);
            graph.nodes.push_back({RuleOpcode::traverse_rays, max_steps});
            if (py::cast<bool>(spec["until_blocked"])) {
                graph.nodes.push_back({RuleOpcode::until_blocked, 0U});
            }
        } else if (kind == "jumps") {
            graph.nodes.push_back(
                {RuleOpcode::traverse_jumps, py::cast<std::uint32_t>(spec["distance"])});
        } else {
            return std::unexpected(Diagnostic{DiagnosticCode::validation_failed,
                                              "unknown Python movement traversal: " + kind, {}});
        }
        if (py::cast<bool>(spec["allow_empty"])) {
            graph.nodes.push_back({RuleOpcode::emit_empty, 0U});
        }
        if (py::cast<bool>(spec["capture_enemy"])) {
            graph.nodes.push_back({RuleOpcode::emit_enemy_capture, 0U});
        }
        return lower_movement_rule(graph);
    } catch (py::error_already_set& error) {
        return std::unexpected(
            python_exception_diagnostic(error, DiagnosticCode::validation_failed));
    } catch (const py::cast_error& error) {
        return std::unexpected(Diagnostic{DiagnosticCode::validation_failed,
                                          std::string{"invalid movement rule field: "} + error.what(),
                                          {}});
    }
}

std::string handle_repr(std::string_view type, std::uint32_t index, std::uint32_t generation) {
    return std::string{"<"} + std::string{type} + " " + std::to_string(index) + ":" +
           std::to_string(generation) + ">";
}

} // namespace

const GameState& ContextProxy::state() const {
    return *require_capability(capability_, false).state;
}

const ActionIntent& ContextProxy::intent() const {
    return *require_capability(capability_, false).intent;
}

const GameState& TransactionProxy::state() const {
    return *require_capability(capability_, true).state;
}

Transaction& TransactionProxy::transaction() const {
    return *require_capability(capability_, true).transaction;
}

std::expected<RuleProgram, Diagnostic>
compile_movement_spec(py::handle rule, const SymbolRegistry& symbols) {
    return compile_spec(rule, [&symbols](std::string_view name)
                                  -> std::expected<DirectionId, Diagnostic> {
        const auto direction = symbols.directions.find(name);
        if (!direction) {
            return std::unexpected(Diagnostic{DiagnosticCode::validation_failed,
                                              direction.error(), {}});
        }
        return *direction;
    });
}

py::list action_targets_to_python(const ActionIntent& intent) {
    py::list targets;
    for (const auto& target : intent.targets) {
        std::visit(
            [&targets](auto id) {
                using T = decltype(id);
                if constexpr (std::same_as<T, EntityId>) {
                    targets.append(py::cast(EntityHandle{id}));
                } else if constexpr (std::same_as<T, SpaceId>) {
                    targets.append(py::cast(SpaceHandle{id}));
                } else {
                    targets.append(py::cast(PlayerHandle{id}));
                }
            },
            target);
    }
    return targets;
}

Diagnostic python_exception_diagnostic(py::error_already_set& error, DiagnosticCode code) {
    SourceLocation source;
    std::string type_name = "PythonError";
    std::string value;
    std::string detail = error.what();
    try {
        type_name = py::str(error.type().attr("__name__")).cast<std::string>();
        value = py::str(error.value()).cast<std::string>();
        py::object traceback = error.trace();
        while (traceback && !traceback.is_none()) {
            source.path =
                py::str(traceback.attr("tb_frame").attr("f_code").attr("co_filename"))
                    .cast<std::string>();
            source.line = py::cast<std::size_t>(traceback.attr("tb_lineno"));
            traceback = traceback.attr("tb_next");
        }
        const auto traceback_module = py::module_::import("traceback");
        traceback = error.trace();
        const auto formatted = traceback_module.attr("format_exception")(
            error.type(), error.value(), traceback ? traceback : py::none());
        detail = py::str("").attr("join")(formatted).cast<std::string>();
    } catch (const py::error_already_set&) {
        // error.what() remains a useful fallback even if traceback formatting itself fails.
    }
    return Diagnostic{code, type_name + (value.empty() ? std::string{} : ": " + value),
                      std::move(source), std::move(detail)};
}

void bind_native_module(py::module_& module) {
    module.doc() = "Native Ludus Arcanum rule boundary";

    py::class_<EntityHandle>(module, "EntityHandle")
        .def_property_readonly("index", [](EntityHandle handle) { return handle.id.index(); })
        .def_property_readonly("generation",
                               [](EntityHandle handle) { return handle.id.generation(); })
        .def_property_readonly("packed", [](EntityHandle handle) { return handle.id.packed(); })
        .def("__repr__", [](EntityHandle handle) {
            return handle_repr("EntityHandle", handle.id.index(), handle.id.generation());
        })
        .def("__hash__", [](EntityHandle handle) { return handle.id.packed(); })
        .def(py::self == py::self);

    py::class_<SpaceHandle>(module, "SpaceHandle")
        .def_property_readonly("index", [](SpaceHandle handle) { return handle.id.index(); })
        .def_property_readonly("generation", [](SpaceHandle handle) { return handle.id.generation(); })
        .def_property_readonly("packed", [](SpaceHandle handle) { return handle.id.packed(); })
        .def("__repr__", [](SpaceHandle handle) {
            return handle_repr("SpaceHandle", handle.id.index(), handle.id.generation());
        })
        .def("__hash__", [](SpaceHandle handle) { return handle.id.packed(); })
        .def(py::self == py::self);

    py::class_<PlayerHandle>(module, "PlayerHandle")
        .def_property_readonly("index", [](PlayerHandle handle) { return handle.id.index(); })
        .def_property_readonly("generation",
                               [](PlayerHandle handle) { return handle.id.generation(); })
        .def_property_readonly("packed", [](PlayerHandle handle) { return handle.id.packed(); })
        .def("__repr__", [](PlayerHandle handle) {
            return handle_repr("PlayerHandle", handle.id.index(), handle.id.generation());
        })
        .def("__hash__", [](PlayerHandle handle) { return handle.id.packed(); })
        .def(py::self == py::self);

    py::class_<ActionHandle>(module, "ActionHandle")
        .def_property_readonly("value", [](ActionHandle handle) { return handle.id.value(); })
        .def("__repr__", [](ActionHandle handle) {
            return std::string{"<ActionHandle "} + std::to_string(handle.id.value()) + ">";
        })
        .def("__hash__", [](ActionHandle handle) { return handle.id.value(); })
        .def(py::self == py::self);

    py::class_<EntityView>(module, "EntityView")
        .def_readonly("entity", &EntityView::entity)
        .def_readonly("location", &EntityView::location)
        .def_readonly("owner", &EntityView::owner)
        .def_readonly("tag_ids", &EntityView::tags);

    py::class_<MoveCandidate>(module, "MoveCandidate")
        .def_property_readonly("destination", [](const MoveCandidate& candidate) {
            return SpaceHandle{candidate.destination};
        })
        .def_property_readonly("capture", [](const MoveCandidate& candidate) -> py::object {
            return candidate.capture ? py::cast(EntityHandle{*candidate.capture}) : py::none();
        });

    py::class_<DiceResult>(module, "DiceResult")
        .def_readonly("stream", &DiceResult::stream)
        .def_readonly("expression", &DiceResult::expression)
        .def_readonly("dice", &DiceResult::dice)
        .def_readonly("total", &DiceResult::total);

    py::class_<RuleProgram>(module, "RuleProgram")
        .def_property_readonly("hash", &RuleProgram::canonical_hash)
        .def_property_readonly("instruction_count",
                               [](const RuleProgram& program) {
                                   return program.instructions().size();
                               })
        .def_property_readonly("direction_count", [](const RuleProgram& program) {
            return program.directions().size();
        });

    py::class_<ContextProxy>(module, "RuleContext")
        .def_property_readonly("action", [](const ContextProxy& context) {
            return ActionHandle{context.intent().type};
        })
        .def_property_readonly("issuer", [](const ContextProxy& context) {
            return PlayerHandle{context.intent().issuer};
        })
        .def_property_readonly("actor", [](const ContextProxy& context) -> py::object {
            return context.intent().actor ? py::cast(EntityHandle{*context.intent().actor})
                                          : py::none();
        })
        .def_property_readonly("targets", [](const ContextProxy& context) {
            return action_targets_to_python(context.intent());
        })
        .def("entity", [](const ContextProxy& context, EntityHandle handle) {
            const auto snapshot = context.state().entities().snapshot(handle.id);
            if (!snapshot) {
                throw_diagnostic(snapshot.error());
            }
            std::vector<std::uint32_t> tags;
            for (const auto tag : snapshot->tags.values()) {
                tags.push_back(tag.value());
            }
            return EntityView{handle,
                              snapshot->location
                                  ? std::optional<SpaceHandle>{SpaceHandle{*snapshot->location}}
                                  : std::nullopt,
                              snapshot->owner
                                  ? std::optional<PlayerHandle>{PlayerHandle{*snapshot->owner}}
                                  : std::nullopt,
                              std::move(tags)};
        })
        .def("entities_at", [](const ContextProxy& context, SpaceHandle space) {
            if (!context.state().topology().contains(space.id)) {
                throw py::value_error{"space handle is stale or invalid"};
            }
            std::vector<EntityHandle> result;
            for (const auto entity : context.state().entities().entities()) {
                const auto snapshot = context.state().entities().snapshot(entity);
                if (snapshot && snapshot->location == space.id) {
                    result.push_back(EntityHandle{entity});
                }
            }
            return result;
        })
        .def("neighbors", [](const ContextProxy& context, SpaceHandle space,
                             std::string_view direction_name) {
            const auto direction = direction_id(context.state(), direction_name);
            std::vector<SpaceHandle> result;
            for (const auto& link : context.state().topology().outgoing(space.id)) {
                if (link.direction == direction) {
                    result.push_back(SpaceHandle{link.to});
                }
            }
            return result;
        })
        .def("property", [](const ContextProxy& context, EntityHandle entity,
                            std::string_view name) -> py::object {
            const auto snapshot = context.state().entities().snapshot(entity.id);
            if (!snapshot) {
                throw_diagnostic(snapshot.error());
            }
            const auto* value = snapshot->properties.find(property_id(context.state(), name));
            return value == nullptr ? py::none() : property_to_python(*value);
        })
        .def("has_tag", [](const ContextProxy& context, EntityHandle entity,
                            std::string_view name) {
            const auto snapshot = context.state().entities().snapshot(entity.id);
            if (!snapshot) {
                throw_diagnostic(snapshot.error());
            }
            return snapshot->tags.contains(tag_id(context.state(), name));
        })
        .def("argument", [](const ContextProxy& context,
                            std::string_view name) -> py::object {
            const auto* value = context.intent().arguments.find(property_id(context.state(), name));
            return value == nullptr ? py::none() : property_to_python(*value);
        })
        .def("evaluate_movement", [](const ContextProxy& context, EntityHandle actor,
                                     const RuleProgram& program) {
            const auto candidates = evaluate_movement(context.state(), actor.id, program);
            if (!candidates) {
                throw_diagnostic(candidates.error());
            }
            return *candidates;
        });

    py::class_<TransactionProxy>(module, "Transaction")
        .def("move", [](const TransactionProxy& proxy, EntityHandle entity, py::object destination) {
            std::optional<SpaceId> location;
            if (!destination.is_none()) {
                location = destination.cast<SpaceHandle>().id;
            }
            if (const auto moved = proxy.transaction().move(entity.id, location); !moved) {
                throw_diagnostic(moved.error());
            }
        })
        .def("destroy", [](const TransactionProxy& proxy, EntityHandle entity) {
            if (const auto destroyed = proxy.transaction().destroy(entity.id); !destroyed) {
                throw_diagnostic(destroyed.error());
            }
        })
        .def("set_owner", [](const TransactionProxy& proxy, EntityHandle entity,
                              py::object owner) {
            std::optional<PlayerId> player;
            if (!owner.is_none()) {
                player = owner.cast<PlayerHandle>().id;
            }
            if (const auto changed = proxy.transaction().set_owner(entity.id, player); !changed) {
                throw_diagnostic(changed.error());
            }
        })
        .def("spawn",
             [](const TransactionProxy& proxy, py::object location, py::object owner,
                const std::vector<std::string>& tag_names, const py::dict& properties) {
                 SpawnOptions options;
                 if (!location.is_none()) {
                     options.location = location.cast<SpaceHandle>().id;
                 }
                 if (!owner.is_none()) {
                     options.owner = owner.cast<PlayerHandle>().id;
                 }
                 for (const auto& name : tag_names) {
                     static_cast<void>(options.tags.add(tag_id(proxy.state(), name)));
                 }
                 for (const auto& [name, value] : properties) {
                     static_cast<void>(options.properties.set(
                         property_id(proxy.state(), py::cast<std::string>(name)),
                         property_from_python(value)));
                 }
                 const auto spawned = proxy.transaction().spawn(std::move(options));
                 if (!spawned) {
                     throw_diagnostic(spawned.error());
                 }
                 return EntityHandle{*spawned};
             },
             py::arg("location") = py::none(), py::arg("owner") = py::none(),
             py::arg("tags") = std::vector<std::string>{}, py::arg("properties") = py::dict{})
        .def("set_property", [](const TransactionProxy& proxy, EntityHandle entity,
                                std::string_view name, py::handle value) {
            const auto changed = proxy.transaction().set_property(
                entity.id, property_id(proxy.state(), name), property_from_python(value));
            if (!changed) {
                throw_diagnostic(changed.error());
            }
        })
        .def("erase_property", [](const TransactionProxy& proxy, EntityHandle entity,
                                  std::string_view name) {
            const auto changed = proxy.transaction().erase_property(
                entity.id, property_id(proxy.state(), name));
            if (!changed) {
                throw_diagnostic(changed.error());
            }
        })
        .def("add_tag", [](const TransactionProxy& proxy, EntityHandle entity,
                           std::string_view name) {
            const auto changed =
                proxy.transaction().add_tag(entity.id, tag_id(proxy.state(), name));
            if (!changed) {
                throw_diagnostic(changed.error());
            }
        })
        .def("remove_tag", [](const TransactionProxy& proxy, EntityHandle entity,
                              std::string_view name) {
            const auto changed =
                proxy.transaction().remove_tag(entity.id, tag_id(proxy.state(), name));
            if (!changed) {
                throw_diagnostic(changed.error());
            }
        })
        .def("roll", [](const TransactionProxy& proxy, std::string_view expression,
                        std::string_view stream) {
            const auto result = proxy.transaction().roll(expression, stream);
            if (!result) {
                throw_diagnostic(result.error());
            }
            return *result;
        });

    module.def("compile_movement", [](py::handle rule, const py::dict& direction_ids) {
        const auto program = compile_spec(
            rule, [&direction_ids](const std::string& name)
                      -> std::expected<DirectionId, Diagnostic> {
                if (!direction_ids.contains(name.c_str())) {
                    return std::unexpected(Diagnostic{DiagnosticCode::validation_failed,
                                                      "unknown direction: " + name, {}});
                }
                const DirectionId direction{py::cast<std::uint32_t>(direction_ids[name.c_str()])};
                if (!direction.valid()) {
                    return std::unexpected(Diagnostic{DiagnosticCode::validation_failed,
                                                      "direction ID cannot be zero", {}});
                }
                return direction;
            });
        if (!program) {
            throw_diagnostic(program.error());
        }
        return *program;
    });
}

} // namespace ludus::python_detail
