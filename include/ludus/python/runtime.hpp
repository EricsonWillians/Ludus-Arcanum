#pragma once

#include "ludus/core/diagnostic.hpp"
#include "ludus/rule_ir/program.hpp"
#include "ludus/rules/action.hpp"
#include "ludus/rules/game_state.hpp"
#include "ludus/rules/transaction.hpp"

#include <cstddef>
#include <expected>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ludus {

class PythonRuntime;
using PythonReloadValidator =
    std::function<std::expected<void, Diagnostic>(const PythonRuntime&)>;

/// Simulation-thread-confined owner of one trusted embedded CPython interpreter.
class PythonRuntime {
  public:
    PythonRuntime(const PythonRuntime&) = delete;
    PythonRuntime& operator=(const PythonRuntime&) = delete;
    PythonRuntime(PythonRuntime&&) = delete;
    PythonRuntime& operator=(PythonRuntime&&) = delete;
    ~PythonRuntime();

    [[nodiscard]] static std::expected<std::unique_ptr<PythonRuntime>, Diagnostic>
    create(std::span<const std::string> search_paths = {});

    [[nodiscard]] std::expected<void, Diagnostic> load_module(std::string_view module_name);
    [[nodiscard]] std::expected<std::vector<std::string>, Diagnostic> action_names() const;
    [[nodiscard]] std::expected<std::vector<std::string>, Diagnostic>
    movement_rule_names() const;
    [[nodiscard]] std::expected<RuleProgram, Diagnostic>
    compile_movement(std::string_view rule_name, const SymbolRegistry& symbols) const;

    [[nodiscard]] std::expected<void, Diagnostic>
    invoke_action(std::string_view action_name, const GameState& state, Transaction& transaction,
                  const ActionIntent& intent) const;

    [[nodiscard]] std::expected<void, Diagnostic> request_reload();
    [[nodiscard]] std::expected<bool, Diagnostic>
    reload_if_safe(bool at_session_boundary, PythonReloadValidator validator = {});
    [[nodiscard]] std::size_t generation() const noexcept;
    [[nodiscard]] std::string_view module_name() const noexcept;

  private:
    struct Impl;
    explicit PythonRuntime(std::unique_ptr<Impl> impl);

    [[nodiscard]] std::expected<void, Diagnostic> check_thread() const;

    std::unique_ptr<Impl> impl_;
};

} // namespace ludus
