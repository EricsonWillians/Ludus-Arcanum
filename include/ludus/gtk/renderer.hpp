#pragma once

#include <cstdint>
#include <string>

namespace ludus {

enum class RendererPreference : std::uint8_t { automatic, desktop_gl, gles, software };
enum class RendererBackend : std::uint8_t { desktop_gl, gles, software };

struct RendererInfo {
    RendererBackend backend{RendererBackend::software};
    bool accelerated{false};
    std::string api;
    std::string vendor;
    std::string renderer;
    std::string version;
    std::string fallback_reason;

    [[nodiscard]] std::string summary() const;
};

[[nodiscard]] const char* renderer_backend_name(RendererBackend backend) noexcept;

} // namespace ludus
