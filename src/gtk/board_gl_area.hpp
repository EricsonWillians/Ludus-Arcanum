#pragma once

#include "ludus/gtk/renderer.hpp"
#include "ludus/render/atlas.hpp"
#include "ludus/render/camera.hpp"
#include "ludus/render/snapshot.hpp"

#include <gtkmm/glarea.h>
#include <sigc++/sigc++.h>

#include <memory>
#include <chrono>
#include <optional>
#include <string>
#include <vector>

namespace ludus {

/// Accelerated BoardCanvas backend. This type is intentionally not a public API.
class BoardGLArea final : public Gtk::GLArea {
  public:
    explicit BoardGLArea(RendererBackend backend = RendererBackend::desktop_gl);
    BoardGLArea(const BoardGLArea&) = delete;
    BoardGLArea& operator=(const BoardGLArea&) = delete;
    BoardGLArea(BoardGLArea&&) = delete;
    BoardGLArea& operator=(BoardGLArea&&) = delete;
    ~BoardGLArea() override;

    void set_snapshot(std::shared_ptr<const RenderSnapshot> snapshot);
    void set_texture_atlas(TextureAtlas atlas);
    void set_font_families(std::vector<std::string> families);
    void set_focused_space(std::optional<SpaceId> space);
    void set_interaction(InteractionState interaction);
    void set_interaction(std::optional<EntityId> selected,
                         std::vector<SpaceId> legal_destinations);
    void reset_camera();
    void flip_board();

    [[nodiscard]] const Camera2D& camera() const noexcept { return camera_; }
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
    [[nodiscard]] sigc::signal<void(const RendererInfo&)>& signal_renderer_ready() noexcept {
        return renderer_ready_;
    }
    [[nodiscard]] sigc::signal<void()>& signal_stress_complete() noexcept {
        return stress_complete_;
    }

  protected:
    bool on_render(const Glib::RefPtr<Gdk::GLContext>& context) override;
    void on_resize(int width, int height) override;

  private:
    struct GpuResources;
    struct TextResources;

    void realize_resources();
    void release_resources() noexcept;
    void upload_atlas();
    void rebuild_text_cache();
    void upload_text_atlas();
    void on_click(int press_count, double x, double y);
    void on_piece_drag_begin(double x, double y);
    void on_piece_drag_update(double offset_x, double offset_y);
    void on_piece_drag_end(double offset_x, double offset_y);
    bool on_scroll(double delta_x, double delta_y);

    std::unique_ptr<GpuResources> gpu_;
    std::unique_ptr<TextResources> text_;
    std::shared_ptr<const RenderSnapshot> snapshot_;
    std::optional<TextureAtlas> atlas_;
    InteractionState interaction_;
    Camera2D camera_;
    RendererBackend backend_;
    RendererInfo info_;
    std::string font_families_{"Serif"};
    bool camera_fitted_{false};
    Vec2 pointer_position_{};
    Vec2 piece_drag_start_{};
    Vec2 pan_drag_offset_{};
    std::optional<std::chrono::steady_clock::time_point> spring_back_start_;
    Vec2 spring_back_from_{};
    Vec2 spring_back_to_{};
    sigc::signal<void(SpaceId)> space_activated_;
    sigc::signal<void(SpaceId, SpaceId)> space_dropped_;
    sigc::signal<void(const std::string&)> render_error_;
    sigc::signal<void(const RendererInfo&)> renderer_ready_;
    sigc::signal<void()> stress_complete_;
};

} // namespace ludus
