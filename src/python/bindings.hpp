#pragma once

#include "ludus/core/diagnostic.hpp"
#include "ludus/python/handles.hpp"
#include "ludus/rule_ir/program.hpp"
#include "ludus/rules/action.hpp"
#include "ludus/rules/game_state.hpp"
#include "ludus/rules/transaction.hpp"

#include <pybind11/pybind11.h>

#include <expected>
#include <memory>

namespace ludus::python_detail {

namespace py = pybind11;

struct CallbackCapability {
    const GameState* state{nullptr};
    Transaction* transaction{nullptr};
    const ActionIntent* intent{nullptr};
    bool active{true};
};

class ContextProxy {
  public:
    explicit ContextProxy(std::shared_ptr<CallbackCapability> capability)
        : capability_(std::move(capability)) {}

    [[nodiscard]] const GameState& state() const;
    [[nodiscard]] const ActionIntent& intent() const;

  private:
    std::shared_ptr<CallbackCapability> capability_;
};

class TransactionProxy {
  public:
    explicit TransactionProxy(std::shared_ptr<CallbackCapability> capability)
        : capability_(std::move(capability)) {}

    [[nodiscard]] const GameState& state() const;
    [[nodiscard]] Transaction& transaction() const;

  private:
    std::shared_ptr<CallbackCapability> capability_;
};

void bind_native_module(py::module_& module);

[[nodiscard]] std::expected<RuleProgram, Diagnostic>
compile_movement_spec(py::handle rule, const SymbolRegistry& symbols);

[[nodiscard]] py::list action_targets_to_python(const ActionIntent& intent);

[[nodiscard]] Diagnostic python_exception_diagnostic(py::error_already_set& error,
                                                     DiagnosticCode code);

} // namespace ludus::python_detail
