#pragma once

#include <cstddef>
#include <string>
#include <utility>

namespace ludus {

enum class DiagnosticCode {
    invalid_argument,
    invalid_handle,
    invalid_state,
    validation_failed,
    serialization_error,
    unknown_action,
    transaction_failed,
    random_expression_error,
};

struct SourceLocation {
    std::string path;
    std::size_t line{0};
    std::size_t column{0};

    auto operator<=>(const SourceLocation&) const = default;
};

struct Diagnostic {
    DiagnosticCode code{DiagnosticCode::invalid_state};
    std::string message;
    SourceLocation source;
    std::string detail;

    Diagnostic() = default;
    Diagnostic(DiagnosticCode diagnostic_code, std::string diagnostic_message,
               SourceLocation diagnostic_source = {}, std::string diagnostic_detail = {})
        : code(diagnostic_code), message(std::move(diagnostic_message)),
          source(std::move(diagnostic_source)), detail(std::move(diagnostic_detail)) {}

    auto operator<=>(const Diagnostic&) const = default;
};

} // namespace ludus
