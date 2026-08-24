#include "ludus/python/runtime.hpp"

#include "bindings.hpp"

#include <pybind11/embed.h>
#include <pybind11/stl.h>

#include <algorithm>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace py = pybind11;

PYBIND11_EMBEDDED_MODULE(_ludus_arcanum_native, module) {
    ludus::python_detail::bind_native_module(module);
}

namespace ludus {
namespace {

std::mutex runtime_mutex;
bool runtime_active = false;

std::expected<void, Diagnostic> validate_module(const py::object& module) {
    if (!py::hasattr(module, "__ludus_actions__") ||
        !py::isinstance<py::dict>(module.attr("__ludus_actions__"))) {
        return std::unexpected(Diagnostic{
            DiagnosticCode::validation_failed,
            "Python rule module must expose the action registry created by @action", {}});
    }
    for (const auto& [name, callback] : module.attr("__ludus_actions__").cast<py::dict>()) {
        if (!py::isinstance<py::str>(name) || py::cast<std::string>(name).empty() ||
            PyCallable_Check(callback.ptr()) == 0) {
            return std::unexpected(Diagnostic{DiagnosticCode::validation_failed,
                                              "Python action registry entries must map non-empty "
                                              "string names to callables",
                                              {}});
        }
    }
    if (py::hasattr(module, "MOVEMENT_RULES") &&
        !py::isinstance<py::dict>(module.attr("MOVEMENT_RULES"))) {
        return std::unexpected(Diagnostic{DiagnosticCode::validation_failed,
                                          "MOVEMENT_RULES must be a dict", {}});
    }
    if (py::hasattr(module, "MOVEMENT_RULES")) {
        for (const auto& [name, rule] : module.attr("MOVEMENT_RULES").cast<py::dict>()) {
            if (!py::isinstance<py::str>(name) || py::cast<std::string>(name).empty() ||
                (!py::isinstance<py::dict>(rule) && !py::hasattr(rule, "to_spec"))) {
                return std::unexpected(Diagnostic{
                    DiagnosticCode::validation_failed,
                    "MOVEMENT_RULES entries must map non-empty string names to rule specs", {}});
            }
        }
    }
    return {};
}

std::vector<std::string> sorted_dict_keys(const py::dict& dictionary) {
    std::vector<std::string> result;
    result.reserve(dictionary.size());
    for (const auto& [key, value] : dictionary) {
        static_cast<void>(value);
        result.push_back(py::cast<std::string>(key));
    }
    std::ranges::sort(result);
    return result;
}

struct CapabilityGuard {
    std::shared_ptr<python_detail::CallbackCapability> capability;
    ~CapabilityGuard() {
        if (capability) {
            capability->active = false;
            capability->state = nullptr;
            capability->transaction = nullptr;
            capability->intent = nullptr;
        }
    }
};

} // namespace

#if defined(__GNUC__) || defined(__clang__)
#define LUDUS_PYTHON_HIDDEN __attribute__((visibility("hidden")))
#else
#define LUDUS_PYTHON_HIDDEN
#endif

struct LUDUS_PYTHON_HIDDEN PythonRuntime::Impl {
    Impl() : interpreter{}, owner_thread(std::this_thread::get_id()), module(py::none()) {}

    void initialize(std::span<const std::string> search_paths) {
        auto path = py::module_::import("sys").attr("path");
        for (auto iterator = search_paths.rbegin(); iterator != search_paths.rend(); ++iterator) {
            path.attr("insert")(0, *iterator);
        }
        static_cast<void>(py::module_::import("ludus_arcanum"));
    }

    py::scoped_interpreter interpreter;
    std::thread::id owner_thread;
    py::object module;
    std::string loaded_module_name;
    std::size_t reload_generation{0};
    bool reload_requested{false};
};

#undef LUDUS_PYTHON_HIDDEN

PythonRuntime::PythonRuntime(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

PythonRuntime::~PythonRuntime() {
    impl_.reset();
    const std::scoped_lock lock{runtime_mutex};
    runtime_active = false;
}

std::expected<std::unique_ptr<PythonRuntime>, Diagnostic>
PythonRuntime::create(std::span<const std::string> search_paths) {
    {
        const std::scoped_lock lock{runtime_mutex};
        if (runtime_active) {
            return std::unexpected(Diagnostic{DiagnosticCode::invalid_state,
                                              "an embedded Python runtime is already active", {}});
        }
        runtime_active = true;
    }
    std::unique_ptr<PythonRuntime> runtime;
    try {
        runtime = std::unique_ptr<PythonRuntime>{new PythonRuntime{std::make_unique<Impl>()}};
    } catch (const std::exception& error) {
        const std::scoped_lock lock{runtime_mutex};
        runtime_active = false;
        return std::unexpected(Diagnostic{DiagnosticCode::invalid_state,
                                          std::string{"failed to initialize CPython: "} +
                                              error.what(),
                                          {}});
    }

    try {
        runtime->impl_->initialize(search_paths);
        return runtime;
    } catch (py::error_already_set& error) {
        const auto diagnostic = python_detail::python_exception_diagnostic(
            error, DiagnosticCode::invalid_state);
        runtime.reset();
        return std::unexpected(diagnostic);
    } catch (const std::exception& error) {
        const auto diagnostic = Diagnostic{DiagnosticCode::invalid_state,
                                           std::string{"failed to configure CPython: "} +
                                               error.what(),
                                           {}};
        runtime.reset();
        return std::unexpected(diagnostic);
    }
}

std::expected<void, Diagnostic> PythonRuntime::check_thread() const {
    if (!impl_ || impl_->owner_thread != std::this_thread::get_id()) {
        return std::unexpected(Diagnostic{
            DiagnosticCode::invalid_state,
            "embedded Python runtime may only be used from its simulation thread", {}});
    }
    return {};
}

std::expected<void, Diagnostic> PythonRuntime::load_module(std::string_view module_name) {
    if (const auto checked = check_thread(); !checked) {
        return checked;
    }
    if (module_name.empty()) {
        return std::unexpected(Diagnostic{DiagnosticCode::invalid_argument,
                                          "Python rule module name cannot be empty", {}});
    }
    try {
        py::gil_scoped_acquire acquire;
        auto candidate = py::module_::import(std::string{module_name}.c_str());
        if (const auto validated = validate_module(candidate); !validated) {
            return validated;
        }
        impl_->module = std::move(candidate);
        impl_->loaded_module_name = module_name;
        impl_->reload_generation = 1U;
        impl_->reload_requested = false;
        return {};
    } catch (py::error_already_set& error) {
        return std::unexpected(python_detail::python_exception_diagnostic(
            error, DiagnosticCode::validation_failed));
    }
}

std::expected<std::vector<std::string>, Diagnostic> PythonRuntime::action_names() const {
    if (const auto checked = check_thread(); !checked) {
        return std::unexpected(checked.error());
    }
    if (!impl_ || impl_->module.is_none()) {
        return std::unexpected(Diagnostic{DiagnosticCode::invalid_state,
                                          "no Python rule module is loaded", {}});
    }
    try {
        py::gil_scoped_acquire acquire;
        return sorted_dict_keys(impl_->module.attr("__ludus_actions__").cast<py::dict>());
    } catch (py::error_already_set& error) {
        return std::unexpected(python_detail::python_exception_diagnostic(
            error, DiagnosticCode::invalid_state));
    }
}

std::expected<std::vector<std::string>, Diagnostic>
PythonRuntime::movement_rule_names() const {
    if (const auto checked = check_thread(); !checked) {
        return std::unexpected(checked.error());
    }
    if (!impl_ || impl_->module.is_none()) {
        return std::unexpected(Diagnostic{DiagnosticCode::invalid_state,
                                          "no Python rule module is loaded", {}});
    }
    try {
        py::gil_scoped_acquire acquire;
        if (!py::hasattr(impl_->module, "MOVEMENT_RULES")) {
            return std::vector<std::string>{};
        }
        return sorted_dict_keys(impl_->module.attr("MOVEMENT_RULES").cast<py::dict>());
    } catch (py::error_already_set& error) {
        return std::unexpected(python_detail::python_exception_diagnostic(
            error, DiagnosticCode::invalid_state));
    }
}

std::expected<RuleProgram, Diagnostic>
PythonRuntime::compile_movement(std::string_view rule_name, const SymbolRegistry& symbols) const {
    if (const auto checked = check_thread(); !checked) {
        return std::unexpected(checked.error());
    }
    if (!impl_ || impl_->module.is_none()) {
        return std::unexpected(Diagnostic{DiagnosticCode::invalid_state,
                                          "no Python rule module is loaded", {}});
    }
    try {
        py::gil_scoped_acquire acquire;
        if (!py::hasattr(impl_->module, "MOVEMENT_RULES")) {
            return std::unexpected(Diagnostic{DiagnosticCode::validation_failed,
                                              "Python module has no MOVEMENT_RULES", {}});
        }
        const auto rules = impl_->module.attr("MOVEMENT_RULES").cast<py::dict>();
        const auto name = std::string{rule_name};
        if (!rules.contains(name.c_str())) {
            return std::unexpected(Diagnostic{DiagnosticCode::validation_failed,
                                              "unknown Python movement rule: " + name, {}});
        }
        return python_detail::compile_movement_spec(rules[name.c_str()], symbols);
    } catch (py::error_already_set& error) {
        return std::unexpected(python_detail::python_exception_diagnostic(
            error, DiagnosticCode::validation_failed));
    }
}

std::expected<void, Diagnostic>
PythonRuntime::invoke_action(std::string_view action_name, const GameState& state,
                             Transaction& transaction, const ActionIntent& intent) const {
    if (const auto checked = check_thread(); !checked) {
        return checked;
    }
    if (!impl_ || impl_->module.is_none()) {
        return std::unexpected(Diagnostic{DiagnosticCode::invalid_state,
                                          "no Python rule module is loaded", {}});
    }
    try {
        py::gil_scoped_acquire acquire;
        const auto actions = impl_->module.attr("__ludus_actions__").cast<py::dict>();
        const auto name = std::string{action_name};
        if (!actions.contains(name.c_str())) {
            return std::unexpected(Diagnostic{DiagnosticCode::unknown_action,
                                              "unknown Python action: " + name, {}});
        }
        auto capability = std::make_shared<python_detail::CallbackCapability>(
            python_detail::CallbackCapability{&state, &transaction, &intent, true});
        const CapabilityGuard guard{capability};
        py::object actor = py::none();
        if (intent.actor) {
            actor = py::cast(EntityHandle{*intent.actor});
        }
        const auto result = actions[name.c_str()](python_detail::ContextProxy{capability},
                                                  python_detail::TransactionProxy{capability}, actor,
                                                  python_detail::action_targets_to_python(intent));
        if (!result.is_none()) {
            return std::unexpected(Diagnostic{DiagnosticCode::transaction_failed,
                                              "Python action callbacks must return None", {}});
        }
        return {};
    } catch (py::error_already_set& error) {
        return std::unexpected(python_detail::python_exception_diagnostic(
            error, DiagnosticCode::transaction_failed));
    }
}

std::expected<void, Diagnostic> PythonRuntime::request_reload() {
    if (const auto checked = check_thread(); !checked) {
        return checked;
    }
    if (!impl_ || impl_->module.is_none()) {
        return std::unexpected(Diagnostic{DiagnosticCode::invalid_state,
                                          "cannot reload before loading a Python module", {}});
    }
    impl_->reload_requested = true;
    return {};
}

std::expected<bool, Diagnostic>
PythonRuntime::reload_if_safe(bool at_session_boundary, PythonReloadValidator validator) {
    if (const auto checked = check_thread(); !checked) {
        return std::unexpected(checked.error());
    }
    if (!impl_->reload_requested) {
        return false;
    }
    if (!at_session_boundary) {
        return std::unexpected(Diagnostic{DiagnosticCode::invalid_state,
                                          "Python hot reload requires a safe session boundary", {}});
    }

    try {
        py::gil_scoped_acquire acquire;
        const auto dictionary = impl_->module.attr("__dict__").cast<py::dict>();
        const auto backup = dictionary.attr("copy")().cast<py::dict>();
        const auto restore = [&] {
            dictionary.attr("clear")();
            dictionary.attr("update")(backup);
        };
        try {
            // importlib.reload() deliberately retains a module dictionary. Remove the
            // authoritative exports so decorators build a fresh registry and deleted
            // movement tables do not survive a source reload.
            dictionary.attr("pop")("__ludus_actions__", py::none());
            dictionary.attr("pop")("MOVEMENT_RULES", py::none());
            py::module_::import("importlib").attr("invalidate_caches")();
            if (py::hasattr(impl_->module, "__cached__")) {
                const auto cached = impl_->module.attr("__cached__");
                if (!cached.is_none()) {
                    py::module_::import("pathlib")
                        .attr("Path")(cached)
                        .attr("unlink")(py::arg("missing_ok") = true);
                }
            }
            auto reloaded = py::module_::import("importlib").attr("reload")(impl_->module);
            if (const auto validated = validate_module(reloaded); !validated) {
                restore();
                return std::unexpected(validated.error());
            }
            impl_->module = std::move(reloaded);
            if (validator) {
                const auto replacement_valid = validator(*this);
                if (!replacement_valid) {
                    restore();
                    return std::unexpected(replacement_valid.error());
                }
            }
            impl_->reload_requested = false;
            ++impl_->reload_generation;
            return true;
        } catch (py::error_already_set& error) {
            restore();
            return std::unexpected(python_detail::python_exception_diagnostic(
                error, DiagnosticCode::validation_failed));
        } catch (const std::exception& error) {
            restore();
            return std::unexpected(Diagnostic{
                DiagnosticCode::validation_failed,
                std::string{"Python reload validation failed: "} + error.what(), {}});
        }
    } catch (py::error_already_set& error) {
        return std::unexpected(python_detail::python_exception_diagnostic(
            error, DiagnosticCode::validation_failed));
    }
}

std::size_t PythonRuntime::generation() const noexcept {
    return impl_ ? impl_->reload_generation : 0U;
}

std::string_view PythonRuntime::module_name() const noexcept {
    return impl_ ? std::string_view{impl_->loaded_module_name} : std::string_view{};
}

} // namespace ludus
