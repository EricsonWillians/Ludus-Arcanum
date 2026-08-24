#include "ludus/gtk/renderer.hpp"

#include <sstream>

namespace ludus {

const char* renderer_backend_name(RendererBackend backend) noexcept {
    switch (backend) {
    case RendererBackend::desktop_gl:
        return "OpenGL";
    case RendererBackend::gles:
        return "OpenGL ES";
    case RendererBackend::software:
        return "Cairo software";
    }
    return "Unknown";
}

std::string RendererInfo::summary() const {
    std::ostringstream output;
    output << (api.empty() ? renderer_backend_name(backend) : api);
    if (!vendor.empty() || !renderer.empty()) {
        output << " — ";
        if (!vendor.empty()) {
            output << vendor;
            if (!renderer.empty()) {
                output << " / ";
            }
        }
        output << renderer;
    }
    if (!version.empty()) {
        output << " (" << version << ')';
    }
    if (!fallback_reason.empty()) {
        output << "; fallback: " << fallback_reason;
    }
    return output.str();
}

} // namespace ludus
