#pragma once

#include "ludus/gtk/renderer.hpp"
#include "ludus/render/atlas.hpp"
#include "ludus/render/camera.hpp"
#include "ludus/render/snapshot.hpp"

#include <gtkmm/box.h>
#include <sigc++/sigc++.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ludus {

class BoardGLArea;
class BoardDrawingArea;

/// One package-neutral native board canvas with automatic accelerated/software fallback.
class BoardCanvas final : public Gtk::Box {
  public:
    explicit BoardCanvas(RendererPreference preference = RendererPreference::automatic);
    BoardCanvas(const BoardCanvas&) = delete;
    BoardCanvas& operator=(const BoardCanvas&) = delete;
    BoardCanvas(BoardCanvas&&) = delete;
    BoardCanvas& operator=(BoardCanvas&&) = delete;
    ~BoardCanvas() override;

    void set_snapshot(std::shared_ptr<const RenderSnapshot> snapshot);
    void set_texture_atlas(TextureAtlas atlas);
    void set_font_families(std::vector<std::string> families);
    void set_focused_space(std::optional<SpaceId> space);
    void set_interaction(InteractionState interaction);
    /// Compatibility bridge for package panels that only expose destinations.
    void set_interaction(std::optional<EntityId> selected,
                         std::vector<SpaceId> legal_destinations);
    void reset_camera();
    void flip_board();

    [[nodiscard]] const Camera2D& camera() const noexcept;
    [[nodiscard]] const RendererInfo& renderer_info() const noexcept { return info_; }
    [[nodiscard]] sigc::signal<void(SpaceId)>& signal_space_activated() noexcept {
        return space_activated_;
    }
    [[nodiscard]] sigc::signal<void(SpaceId, SpaceId)>& signal_space_dropped() noexcept {
        return space_dropped_;
    }
    [[nodiscard]] sigc::signal<void(const std::string&)>& signal_render_error() noexcept {
        return render_error_;
    }
    [[nodiscard]] sigc::signal<void(const RendererInfo&)>& signal_renderer_changed() noexcept {
        return renderer_changed_;
    }
    [[nodiscard]] sigc::signal<void()>& signal_stress_complete() noexcept {
        return stress_complete_;
    }

  private:
    void activate(RendererBackend backend, std::string fallback_reason = {});
    void handle_accelerated_error(const std::string& message);
    void apply_state();

    RendererPreference preference_;
    RendererBackend active_backend_{RendererBackend::software};
    BoardGLArea* gl_area_{nullptr};
    BoardDrawingArea* drawing_area_{nullptr};
    std::shared_ptr<const RenderSnapshot> snapshot_;
    std::optional<TextureAtlas> atlas_;
    std::vector<std::string> font_families_{"Serif"};
    InteractionState interaction_;
    RendererInfo info_;
    bool fallback_pending_{false};
    sigc::signal<void(SpaceId)> space_activated_;
    sigc::signal<void(SpaceId, SpaceId)> space_dropped_;
    sigc::signal<void(const std::string&)> render_error_;
    sigc::signal<void(const RendererInfo&)> renderer_changed_;
    sigc::signal<void()> stress_complete_;
};

} // namespace ludus
