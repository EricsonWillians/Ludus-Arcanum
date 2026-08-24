#include "ludus/gtk/board_canvas.hpp"

#include "board_gl_area.hpp"

#include <cairomm/context.h>
#include <cairomm/surface.h>
#include <glibmm/main.h>
#include <gtkmm/drawingarea.h>
#include <gtkmm/eventcontrollerscroll.h>
#include <gtkmm/eventcontrollermotion.h>
#include <gtkmm/gestureclick.h>
#include <gtkmm/gesturedrag.h>
#include <pangomm/fontdescription.h>
#include <pangomm/layout.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <numbers>
#include <string>
#include <utility>

namespace ludus {
namespace {

void source(const Cairo::RefPtr<Cairo::Context>& context, Color color) {
    context->set_source_rgba(color.red, color.green, color.blue, color.alpha);
}

Vec2 rotate_local(Vec2 point, float radians) {
    const auto cosine = std::cos(radians);
    const auto sine = std::sin(radians);
    return {point.x * cosine - point.y * sine, point.x * sine + point.y * cosine};
}

} // namespace

class BoardDrawingArea final : public Gtk::DrawingArea {
  public:
    BoardDrawingArea() {
        set_hexpand(true);
        set_vexpand(true);
        set_draw_func(sigc::mem_fun(*this, &BoardDrawingArea::draw));
        add_tick_callback([this](const Glib::RefPtr<Gdk::FrameClock>&) {
            const auto now = std::chrono::steady_clock::now();
            if (spring_back_start_ && interaction_.drag) {
                constexpr auto duration = std::chrono::milliseconds{160};
                const auto elapsed = now - *spring_back_start_;
                const auto linear = std::clamp(
                    std::chrono::duration<float>{elapsed}.count() /
                        std::chrono::duration<float>{duration}.count(),
                    0.0F, 1.0F);
                const auto eased = 1.0F - std::pow(1.0F - linear, 3.0F);
                interaction_.drag->pointer = lerp(spring_back_from_, spring_back_to_, eased);
                if (linear >= 1.0F) {
                    interaction_.drag.reset();
                    spring_back_start_.reset();
                }
                queue_draw();
            } else if (snapshot_ && has_active_animations(*snapshot_, now)) {
                queue_draw();
            }
            return true;
        });
        signal_resize().connect([this](int width, int height) { camera_.resize(width, height); });

        const auto click = Gtk::GestureClick::create();
        click->signal_pressed().connect(
            [this](int, double x, double y) { activate_at(x, y); });
        add_controller(click);

        const auto motion = Gtk::EventControllerMotion::create();
        motion->signal_motion().connect([this](double x, double y) {
            pointer_position_ = {static_cast<float>(x), static_cast<float>(y)};
            if (snapshot_) {
                const auto hovered =
                    pick_space(*snapshot_, camera_.screen_to_world(pointer_position_));
                if (hovered != interaction_.hovered) {
                    interaction_.hovered = hovered;
                    queue_draw();
                }
            }
        });
        motion->signal_leave().connect([this] {
            interaction_.hovered.reset();
            queue_draw();
        });
        add_controller(motion);

        const auto piece_drag = Gtk::GestureDrag::create();
        piece_drag->set_button(1U);
        piece_drag->signal_drag_begin().connect([this](double x, double y) {
            spring_back_start_.reset();
            piece_drag_start_ = {static_cast<float>(x), static_cast<float>(y)};
            if (!snapshot_) {
                return;
            }
            const auto origin = pick_space(
                *snapshot_, camera_.screen_to_world(piece_drag_start_));
            const auto* piece = origin ? find_piece_at(*snapshot_, *origin) : nullptr;
            if (origin && piece != nullptr) {
                interaction_.drag = DragInteraction{
                    piece->id, *origin, camera_.screen_to_world(piece_drag_start_), true,
                    false};
                set_cursor("grabbing");
                queue_draw();
            }
        });
        piece_drag->signal_drag_update().connect([this](double offset_x, double offset_y) {
            if (!snapshot_ || !interaction_.drag) {
                return;
            }
            const auto screen = piece_drag_start_ +
                                Vec2{static_cast<float>(offset_x),
                                     static_cast<float>(offset_y)};
            interaction_.drag->pointer = camera_.screen_to_world(screen);
            const auto destination = pick_space(*snapshot_, interaction_.drag->pointer);
            interaction_.drag->valid = destination && std::ranges::any_of(
                interaction_.targets, [&](const InteractionTarget& target) {
                    return target.space == *destination;
                });
            set_cursor(interaction_.drag->valid ? "grabbing" : "not-allowed");
            queue_draw();
        });
        piece_drag->signal_drag_end().connect([this](double offset_x, double offset_y) {
            if (!snapshot_ || !interaction_.drag) {
                return;
            }
            set_cursor();
            if (std::hypot(offset_x, offset_y) < 6.0) {
                interaction_.drag.reset();
                queue_draw();
                return;
            }
            const auto valid = interaction_.drag->valid;
            const auto origin = pick_space(*snapshot_, camera_.screen_to_world(piece_drag_start_));
            const auto destination = pick_space(
                *snapshot_, camera_.screen_to_world(
                                piece_drag_start_ +
                                Vec2{static_cast<float>(offset_x), static_cast<float>(offset_y)}));
            if (valid && origin && destination && *origin != *destination) {
                interaction_.drag.reset();
                queue_draw();
                space_dropped_.emit(*origin, *destination);
                return;
            }
            const auto origin_visual = std::ranges::find(snapshot_->spaces,
                                                          interaction_.drag->origin,
                                                          &SpaceVisual::id);
            if (interaction_.reduced_motion || origin_visual == snapshot_->spaces.end()) {
                interaction_.drag.reset();
            } else {
                spring_back_from_ = interaction_.drag->pointer;
                spring_back_to_ = origin_visual->bounds.center();
                spring_back_start_ = std::chrono::steady_clock::now();
            }
            queue_draw();
        });
        add_controller(piece_drag);

        const auto pan_drag = Gtk::GestureDrag::create();
        pan_drag->set_button(2U);
        pan_drag->signal_drag_begin().connect([this](double, double) {
            pan_drag_offset_ = {};
        });
        pan_drag->signal_drag_update().connect([this](double offset_x, double offset_y) {
            const Vec2 offset{static_cast<float>(offset_x), static_cast<float>(offset_y)};
            camera_.pan_pixels(offset - pan_drag_offset_);
            pan_drag_offset_ = offset;
            queue_draw();
        });
        add_controller(pan_drag);

        const auto scroll = Gtk::EventControllerScroll::create();
        scroll->set_flags(Gtk::EventControllerScroll::Flags::BOTH_AXES);
        scroll->signal_scroll().connect(
            [this](double, double delta_y) {
                camera_.zoom_at(pointer_position_,
                                static_cast<float>(std::exp(-delta_y * 0.12)));
                queue_draw();
                return true;
            },
            false);
    }

    void set_snapshot(std::shared_ptr<const RenderSnapshot> snapshot) {
        snapshot_ = std::move(snapshot);
        if (snapshot_ && !camera_fitted_) {
            camera_.fit(snapshot_->world_bounds);
            camera_fitted_ = true;
        }
        queue_draw();
    }

    void set_texture_atlas(const TextureAtlas& atlas) {
        atlas_ = atlas;
        cairo_pixels_.clear();
        cairo_atlases_.clear();
        const auto pages = atlas.pages();
        cairo_pixels_.resize(pages.size());
        cairo_atlases_.resize(pages.size());
        for (std::size_t page = 0U; page < pages.size(); ++page) {
            const auto& image = pages[page];
            if (!image.valid()) {
                continue;
            }
            const auto stride = Cairo::ImageSurface::format_stride_for_width(
                Cairo::Surface::Format::ARGB32, static_cast<int>(image.width));
            cairo_pixels_[page].assign(static_cast<std::size_t>(stride) * image.height, 0U);
            for (std::uint32_t y = 0U; y < image.height; ++y) {
                for (std::uint32_t x = 0U; x < image.width; ++x) {
                    const auto source_offset =
                        (static_cast<std::size_t>(y) * image.width + x) * 4U;
                    const auto destination_offset = static_cast<std::size_t>(y) *
                                                        static_cast<std::size_t>(stride) +
                                                    static_cast<std::size_t>(x) * 4U;
                    const auto alpha = image.pixels[source_offset + 3U];
                    // Cairo ARGB32 is native-endian premultiplied BGRA on supported desktop
                    // little-endian targets.
                    cairo_pixels_[page][destination_offset] =
                        image.pixels[source_offset + 2U];
                    cairo_pixels_[page][destination_offset + 1U] =
                        image.pixels[source_offset + 1U];
                    cairo_pixels_[page][destination_offset + 2U] =
                        image.pixels[source_offset];
                    cairo_pixels_[page][destination_offset + 3U] = alpha;
                }
            }
            cairo_atlases_[page] = Cairo::ImageSurface::create(
                cairo_pixels_[page].data(), Cairo::Surface::Format::ARGB32,
                static_cast<int>(image.width), static_cast<int>(image.height), stride);
        }
        queue_draw();
    }

    void set_font_families(const std::vector<std::string>& families) {
        font_families_.clear();
        for (const auto& family : families) {
            if (!font_families_.empty()) {
                font_families_ += ", ";
            }
            font_families_ += family;
        }
        if (font_families_.empty()) {
            font_families_ = "Serif";
        }
        queue_draw();
    }

    void set_focused_space(std::optional<SpaceId> space) {
        interaction_.keyboard_focus = space;
        queue_draw();
    }

    void set_interaction(InteractionState interaction) {
        interaction_ = std::move(interaction);
        std::ranges::sort(interaction_.targets, [](const auto& left, const auto& right) {
            return left.space == right.space ? left.kind < right.kind
                                             : left.space < right.space;
        });
        const auto unique = std::ranges::unique(interaction_.targets).begin();
        interaction_.targets.erase(unique, interaction_.targets.end());
        queue_draw();
    }

    void set_interaction(std::optional<EntityId> selected,
                         std::vector<SpaceId> legal_destinations) {
        InteractionState interaction;
        interaction.selected = selected;
        interaction.targets.reserve(legal_destinations.size());
        for (const auto destination : legal_destinations) {
            interaction.targets.push_back(
                InteractionTarget{destination, InteractionTargetKind::quiet_move});
        }
        set_interaction(std::move(interaction));
    }

    void reset_camera() {
        if (snapshot_) {
            camera_.fit(snapshot_->world_bounds);
            camera_fitted_ = true;
            queue_draw();
        }
    }

    void flip_board() {
        camera_.set_rotation(camera_.rotation() + std::numbers::pi_v<float>);
        queue_draw();
    }

    [[nodiscard]] const Camera2D& camera() const noexcept { return camera_; }
    [[nodiscard]] sigc::signal<void(SpaceId)>& signal_space_activated() noexcept {
        return space_activated_;
    }
    [[nodiscard]] sigc::signal<void(SpaceId, SpaceId)>& signal_space_dropped() noexcept {
        return space_dropped_;
    }

  private:
    void polygon(const Cairo::RefPtr<Cairo::Context>& context, Vec2 center, Vec2 half,
                 SpaceShape shape, float rotation) {
        std::array<Vec2, 6U> points{};
        std::size_t count = 4U;
        if (shape == SpaceShape::hexagon) {
            count = 6U;
            points = {{{half.x, 0.0F},
                       {half.x * 0.5F, half.y},
                       {-half.x * 0.5F, half.y},
                       {-half.x, 0.0F},
                       {-half.x * 0.5F, -half.y},
                       {half.x * 0.5F, -half.y}}};
        } else {
            points[0] = {-half.x, -half.y};
            points[1] = {half.x, -half.y};
            points[2] = {half.x, half.y};
            points[3] = {-half.x, half.y};
        }
        for (std::size_t index = 0U; index < count; ++index) {
            const auto screen = camera_.world_to_screen(center + rotate_local(points[index], rotation));
            if (index == 0U) {
                context->move_to(screen.x, screen.y);
            } else {
                context->line_to(screen.x, screen.y);
            }
        }
        context->close_path();
    }

    void fill_space(const Cairo::RefPtr<Cairo::Context>& context, const SpaceVisual& space,
                    Color color) {
        const auto center = space.bounds.center();
        const Vec2 half{space.bounds.width() * 0.5F, space.bounds.height() * 0.5F};
        if (space.shape == SpaceShape::circle) {
            const auto screen = camera_.world_to_screen(center);
            const auto edge = camera_.world_to_screen({center.x + half.x, center.y});
            context->arc(screen.x, screen.y, std::hypot(edge.x - screen.x, edge.y - screen.y),
                         0.0, 2.0 * std::numbers::pi);
        } else {
            polygon(context, center, half, space.shape, space.rotation);
        }
        source(context, color);
        context->fill_preserve();
        if (space.border_width > 0.0F && space.border.alpha > 0.0F) {
            source(context, space.border);
            context->set_line_width(std::max(space.border_width * camera_.zoom() * 16.0F, 1.0F));
            context->stroke();
        } else {
            context->begin_new_path();
        }
    }

    void draw(const Cairo::RefPtr<Cairo::Context>& context, int width, int height) {
        source(context, {0.035F, 0.039F, 0.052F, 1.0F});
        context->paint();
        if (!snapshot_) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        for (const auto& space : snapshot_->spaces) {
            fill_space(context, space, space.color);
        }
        for (const auto& link : snapshot_->links) {
            const auto from = camera_.world_to_screen(link.from_center);
            const auto to = camera_.world_to_screen(link.to_center);
            source(context, link.color);
            context->set_line_width(std::max(link.width * camera_.zoom() * 16.0F, 1.0F));
            context->move_to(from.x, from.y);
            context->line_to(to.x, to.y);
            context->stroke();
        }
        for (const auto& shape : snapshot_->shapes) {
            fill_space(context,
                       SpaceVisual{SpaceId{}, shape.bounds, shape.color, shape.shape,
                                   shape.border, shape.border_width, shape.rotation},
                       shape.color);
        }
        for (const auto& space : snapshot_->spaces) {
            const bool selected_space = interaction_.selected &&
                std::ranges::any_of(snapshot_->pieces, [this, &space](const PieceVisual& piece) {
                    return piece.id == *interaction_.selected && piece.location == space.id;
                });
            const bool hovered = interaction_.hovered == space.id;
            const bool focused = interaction_.keyboard_focus == space.id;
            if (selected_space || hovered || focused) {
                auto overlay = space;
                overlay.color = selected_space ? Color{0.91F, 0.67F, 0.18F, 0.19F}
                                : focused      ? Color{0.26F, 0.62F, 0.82F, 0.12F}
                                               : Color{0.70F, 0.82F, 0.95F, 0.08F};
                overlay.border = selected_space ? Color{0.96F, 0.76F, 0.31F, 0.96F}
                                 : focused      ? Color{0.56F, 0.86F, 1.0F, 0.96F}
                                                : Color{0.72F, 0.82F, 0.95F, 0.55F};
                overlay.border_width = selected_space ? 0.055F : 0.035F;
                fill_space(context, overlay, overlay.color);
            }
        }
        for (const auto& target : interaction_.targets) {
            const auto found = std::ranges::find(snapshot_->spaces, target.space,
                                                 &SpaceVisual::id);
            if (found == snapshot_->spaces.end()) {
                continue;
            }
            auto marker = *found;
            marker.color = {0.0F, 0.0F, 0.0F, 0.0F};
            marker.border = {0.86F, 0.69F, 0.30F, 0.9F};
            marker.border_width = 0.05F;
            if (target.kind == InteractionTargetKind::quiet_move) {
                const auto center = marker.bounds.center();
                const auto radius = std::min(marker.bounds.width(), marker.bounds.height()) *
                                    0.105F;
                marker.bounds = {{center.x - radius, center.y - radius},
                                 {center.x + radius, center.y + radius}};
                marker.shape = SpaceShape::circle;
                marker.color = {0.83F, 0.72F, 0.46F, 0.78F};
                marker.border_width = 0.0F;
            } else if (target.kind == InteractionTargetKind::capture) {
                marker.shape = SpaceShape::circle;
                marker.border = {0.92F, 0.34F, 0.28F, 0.94F};
                marker.border_width = 0.075F;
            } else if (target.kind == InteractionTargetKind::castle) {
                marker.border = {0.45F, 0.72F, 0.94F, 0.92F};
            } else if (target.kind == InteractionTargetKind::promotion) {
                marker.shape = SpaceShape::circle;
                marker.border = {0.73F, 0.48F, 0.96F, 0.96F};
                marker.border_width = 0.065F;
            } else if (target.kind == InteractionTargetKind::draw_claim) {
                marker.color = {0.76F, 0.58F, 0.22F, 0.13F};
                marker.border = {0.96F, 0.81F, 0.43F, 0.9F};
            } else if (target.kind == InteractionTargetKind::invalid) {
                marker.color = {0.75F, 0.08F, 0.10F, 0.16F};
                marker.border = {0.98F, 0.22F, 0.20F, 0.96F};
            }
            fill_space(context, marker, marker.color);
        }
        const auto draw_sprite = [&](Vec2 center, Vec2 size, SpriteId sprite, Color tint,
                                     Color outline, float outline_width, bool shadow) {
            const auto screen = camera_.world_to_screen(center);
            const auto horizontal = camera_.world_to_screen(
                {center.x + size.x * 0.5F, center.y});
            const auto vertical = camera_.world_to_screen(
                {center.x, center.y + size.y * 0.5F});
            const auto target_width =
                std::max(std::hypot(horizontal.x - screen.x, horizontal.y - screen.y) * 2.0F,
                         6.0F);
            const auto target_height =
                std::max(std::hypot(vertical.x - screen.x, vertical.y - screen.y) * 2.0F,
                         6.0F);
            const auto radius = std::max(target_width * 0.44F, 3.0F);
            if (shadow) {
                source(context, {0.0F, 0.0F, 0.0F, 0.35F});
                context->arc(static_cast<double>(screen.x + radius * 0.08F),
                             static_cast<double>(screen.y + radius * 0.12F), radius, 0.0,
                             2.0 * std::numbers::pi);
                context->fill();
            }
            const auto* region = atlas_ ? atlas_->region(sprite) : nullptr;
            if (region != nullptr && region->page < cairo_atlases_.size() &&
                cairo_atlases_[region->page]) {
                context->save();
                context->translate(static_cast<double>(screen.x - target_width * 0.5F),
                                   static_cast<double>(screen.y - target_height * 0.5F));
                context->rectangle(0.0, 0.0, target_width, target_height);
                context->clip();
                context->scale(static_cast<double>(target_width) /
                                   static_cast<double>(region->width),
                               static_cast<double>(target_height) /
                                   static_cast<double>(region->height));
                context->set_source(cairo_atlases_[region->page],
                                    -static_cast<double>(region->x),
                                    -static_cast<double>(region->y));
                context->paint_with_alpha(tint.alpha);
                context->restore();
            } else {
                source(context, tint);
                context->arc(screen.x, screen.y, radius, 0.0, 2.0 * std::numbers::pi);
                context->fill();
            }
            if (outline.alpha > 0.0F) {
                source(context, outline);
                context->arc(screen.x, screen.y, radius, 0.0, 2.0 * std::numbers::pi);
                context->set_line_width(std::max(outline_width * 12.0F, 1.0F));
                context->stroke();
            }
        };
        for (const auto& decoration : snapshot_->decorations) {
            draw_sprite(decoration.center, decoration.size, decoration.sprite,
                        decoration.tint, Color{0.0F, 0.0F, 0.0F, 0.0F}, 0.0F, false);
        }
        std::vector<const PieceVisual*> pieces;
        pieces.reserve(snapshot_->pieces.size());
        for (const auto& piece : snapshot_->pieces) {
            pieces.push_back(&piece);
        }
        std::ranges::sort(pieces, [](const PieceVisual* left, const PieceVisual* right) {
            return left->layer == right->layer ? left->id < right->id
                                                : left->layer < right->layer;
        });
        for (const auto* piece : pieces) {
            if (interaction_.drag && interaction_.drag->active &&
                interaction_.drag->entity == piece->id) {
                continue;
            }
            const auto center = animated_center(*snapshot_, *piece, now);
            draw_sprite(center, piece->size, piece->sprite, piece->tint,
                        piece->outline, piece->outline_width, true);
        }
        if (interaction_.drag && interaction_.drag->active) {
            const auto* piece = find_piece(*snapshot_, interaction_.drag->entity);
            if (piece != nullptr) {
                auto tint = piece->tint;
                tint.alpha *= interaction_.drag->valid ? 0.96F : 0.62F;
                draw_sprite(interaction_.drag->pointer + Vec2{0.0F, 0.08F},
                            piece->size * 1.08F, piece->sprite, tint,
                            interaction_.drag->valid
                                ? Color{0.96F, 0.78F, 0.34F, 0.88F}
                                : Color{0.96F, 0.18F, 0.16F, 0.9F},
                            0.045F, true);
            }
        }
        for (const auto& bar : snapshot_->bars) {
            const auto minimum = camera_.world_to_screen(bar.bounds.minimum);
            const auto maximum = camera_.world_to_screen(bar.bounds.maximum);
            const auto x = std::min(minimum.x, maximum.x);
            const auto y = std::min(minimum.y, maximum.y);
            const auto bar_width = std::abs(maximum.x - minimum.x);
            const auto bar_height = std::abs(maximum.y - minimum.y);
            source(context, bar.background);
            context->rectangle(x, y, bar_width, bar_height);
            context->fill();
            source(context, bar.fill);
            const auto progress = std::clamp(bar.value / std::max(bar.maximum, 0.001F), 0.0F, 1.0F);
            context->rectangle(x, y, bar_width * progress, bar_height);
            context->fill();
        }
        for (const auto& effect : snapshot_->effects) {
            if (effect.duration.count() <= 0 || now >= effect.start + effect.duration) {
                continue;
            }
            const auto elapsed = std::chrono::duration<float>{now - effect.start}.count();
            const auto duration = std::chrono::duration<float>{effect.duration}.count();
            const auto progress = std::clamp(elapsed / duration, 0.0F, 1.0F);
            const bool travels = effect.kind == EffectKind::projectile ||
                                 effect.kind == EffectKind::poison;
            const auto center = travels ? lerp(effect.from, effect.to, progress) : effect.to;
            auto scale = effect.initial_scale +
                         (effect.final_scale - effect.initial_scale) * progress;
            if (effect.kind == EffectKind::impact) {
                scale *= 0.5F + progress;
            } else if (effect.kind == EffectKind::pulse || effect.kind == EffectKind::check) {
                scale *= 0.88F + 0.16F * std::sin(progress * 12.566370614359172F);
            }
            auto color = effect.color;
            color.alpha *= 1.0F - progress;
            const auto screen = camera_.world_to_screen(center);
            const auto edge = camera_.world_to_screen(
                {center.x + effect.size * scale, center.y});
            const auto radius = std::max(std::hypot(edge.x - screen.x, edge.y - screen.y),
                                         1.0F);
            context->set_operator(effect.blend == EffectBlend::additive
                                      ? Cairo::Context::Operator::ADD
                                      : Cairo::Context::Operator::OVER);
            source(context, color);
            context->arc(screen.x, screen.y, radius, 0.0, 2.0 * std::numbers::pi);
            context->fill();
        }
        context->set_operator(Cairo::Context::Operator::OVER);
        for (const auto& text : snapshot_->texts) {
            const auto position = text.screen_space
                                      ? Vec2{text.position.x * static_cast<float>(width),
                                             text.position.y * static_cast<float>(height)}
                                      : camera_.world_to_screen(text.position);
            const auto layout = create_pango_layout(text.text);
            Pango::FontDescription font;
            font.set_family(font_families_);
            font.set_weight(Pango::Weight::BOLD);
            font.set_absolute_size(static_cast<double>(text.size * Pango::SCALE));
            layout->set_font_description(font);
            int text_width = 0;
            int text_height = 0;
            layout->get_pixel_size(text_width, text_height);
            source(context, text.color);
            context->move_to(static_cast<double>(position.x) -
                                 static_cast<double>(text_width) * 0.5,
                             static_cast<double>(position.y) -
                                 static_cast<double>(text_height) * 0.5);
            layout->show_in_cairo_context(context);
        }
    }

    void activate_at(double x, double y) {
        if (!snapshot_) {
            return;
        }
        const auto picked = pick_space(
            *snapshot_, camera_.screen_to_world({static_cast<float>(x), static_cast<float>(y)}));
        if (picked) {
            space_activated_.emit(*picked);
        }
    }

    std::shared_ptr<const RenderSnapshot> snapshot_;
    std::optional<TextureAtlas> atlas_;
    std::vector<std::vector<std::uint8_t>> cairo_pixels_;
    std::vector<Cairo::RefPtr<Cairo::ImageSurface>> cairo_atlases_;
    InteractionState interaction_;
    Camera2D camera_;
    bool camera_fitted_{false};
    Vec2 pointer_position_{};
    Vec2 piece_drag_start_{};
    Vec2 pan_drag_offset_{};
    std::optional<std::chrono::steady_clock::time_point> spring_back_start_;
    Vec2 spring_back_from_{};
    Vec2 spring_back_to_{};
    std::string font_families_{"Serif"};
    sigc::signal<void(SpaceId)> space_activated_;
    sigc::signal<void(SpaceId, SpaceId)> space_dropped_;
};

BoardCanvas::BoardCanvas(RendererPreference preference)
    : Gtk::Box(Gtk::Orientation::VERTICAL, 0), preference_(preference) {
    set_hexpand(true);
    set_vexpand(true);
    const auto initial = preference == RendererPreference::gles
                             ? RendererBackend::gles
                         : preference == RendererPreference::software
                             ? RendererBackend::software
                             : RendererBackend::desktop_gl;
    activate(initial);
}

BoardCanvas::~BoardCanvas() = default;

void BoardCanvas::activate(RendererBackend backend, std::string fallback_reason) {
    if (gl_area_ != nullptr) {
        remove(*gl_area_);
        gl_area_ = nullptr;
    }
    if (drawing_area_ != nullptr) {
        remove(*drawing_area_);
        drawing_area_ = nullptr;
    }
    active_backend_ = backend;
    info_ = RendererInfo{backend, backend != RendererBackend::software,
                         renderer_backend_name(backend), {}, {}, {},
                         std::move(fallback_reason)};
    if (backend == RendererBackend::software) {
        drawing_area_ = Gtk::make_managed<BoardDrawingArea>();
        drawing_area_->signal_space_activated().connect(
            [this](SpaceId space) { space_activated_.emit(space); });
        drawing_area_->signal_space_dropped().connect(
            [this](SpaceId origin, SpaceId destination) {
                space_dropped_.emit(origin, destination);
            });
        append(*drawing_area_);
        apply_state();
        renderer_changed_.emit(info_);
        return;
    }

    gl_area_ = Gtk::make_managed<BoardGLArea>(backend);
    gl_area_->signal_space_activated().connect(
        [this](SpaceId space) { space_activated_.emit(space); });
    gl_area_->signal_space_dropped().connect(
        [this](SpaceId origin, SpaceId destination) {
            space_dropped_.emit(origin, destination);
        });
    gl_area_->signal_renderer_ready().connect([this](const RendererInfo& info) {
        const auto retained_fallback_reason = info_.fallback_reason;
        info_ = info;
        info_.fallback_reason = retained_fallback_reason;
        renderer_changed_.emit(info_);
    });
    gl_area_->signal_stress_complete().connect([this] { stress_complete_.emit(); });
    gl_area_->signal_render_error().connect(
        [this](const std::string& message) { handle_accelerated_error(message); });
    append(*gl_area_);
    apply_state();
}

void BoardCanvas::handle_accelerated_error(const std::string& message) {
    if (preference_ != RendererPreference::automatic) {
        info_.accelerated = false;
        info_.fallback_reason = message;
        renderer_changed_.emit(info_);
        render_error_.emit(message);
        return;
    }
    if (fallback_pending_) {
        return;
    }
    fallback_pending_ = true;
    const auto failed = active_backend_;
    Glib::signal_idle().connect_once([this, failed, message] {
        fallback_pending_ = false;
        if (failed == RendererBackend::desktop_gl) {
            activate(RendererBackend::gles, message);
        } else {
            activate(RendererBackend::software, message);
        }
    });
}

void BoardCanvas::apply_state() {
    if (gl_area_ != nullptr) {
        gl_area_->set_font_families(font_families_);
        if (atlas_) {
            gl_area_->set_texture_atlas(*atlas_);
        }
        gl_area_->set_snapshot(snapshot_);
        gl_area_->set_interaction(interaction_);
    } else if (drawing_area_ != nullptr) {
        drawing_area_->set_font_families(font_families_);
        if (atlas_) {
            drawing_area_->set_texture_atlas(*atlas_);
        }
        drawing_area_->set_snapshot(snapshot_);
        drawing_area_->set_interaction(interaction_);
    }
}

void BoardCanvas::set_focused_space(std::optional<SpaceId> space) {
    interaction_.keyboard_focus = space;
    if (gl_area_ != nullptr) {
        gl_area_->set_focused_space(space);
    } else if (drawing_area_ != nullptr) {
        drawing_area_->set_focused_space(space);
    }
}

void BoardCanvas::set_font_families(std::vector<std::string> families) {
    if (families.empty()) {
        families.emplace_back("Serif");
    }
    font_families_ = std::move(families);
    if (gl_area_ != nullptr) {
        gl_area_->set_font_families(font_families_);
    } else if (drawing_area_ != nullptr) {
        drawing_area_->set_font_families(font_families_);
    }
}

void BoardCanvas::set_snapshot(std::shared_ptr<const RenderSnapshot> snapshot) {
    snapshot_ = std::move(snapshot);
    if (gl_area_ != nullptr) {
        gl_area_->set_snapshot(snapshot_);
    } else if (drawing_area_ != nullptr) {
        drawing_area_->set_snapshot(snapshot_);
    }
}

void BoardCanvas::set_texture_atlas(TextureAtlas atlas) {
    atlas_ = std::move(atlas);
    if (gl_area_ != nullptr) {
        gl_area_->set_texture_atlas(*atlas_);
    } else if (drawing_area_ != nullptr) {
        drawing_area_->set_texture_atlas(*atlas_);
    }
}

void BoardCanvas::set_interaction(InteractionState interaction) {
    interaction_ = std::move(interaction);
    if (gl_area_ != nullptr) {
        gl_area_->set_interaction(interaction_);
    } else if (drawing_area_ != nullptr) {
        drawing_area_->set_interaction(interaction_);
    }
}

void BoardCanvas::set_interaction(std::optional<EntityId> selected,
                                  std::vector<SpaceId> legal_destinations) {
    InteractionState interaction;
    interaction.selected = selected;
    interaction.keyboard_focus = interaction_.keyboard_focus;
    interaction.targets.reserve(legal_destinations.size());
    for (const auto destination : legal_destinations) {
        interaction.targets.push_back(
            InteractionTarget{destination, InteractionTargetKind::quiet_move});
    }
    set_interaction(std::move(interaction));
}

void BoardCanvas::reset_camera() {
    if (gl_area_ != nullptr) {
        gl_area_->reset_camera();
    } else if (drawing_area_ != nullptr) {
        drawing_area_->reset_camera();
    }
}

void BoardCanvas::flip_board() {
    if (gl_area_ != nullptr) {
        gl_area_->flip_board();
    } else if (drawing_area_ != nullptr) {
        drawing_area_->flip_board();
    }
}

const Camera2D& BoardCanvas::camera() const noexcept {
    return gl_area_ != nullptr ? gl_area_->camera() : drawing_area_->camera();
}

} // namespace ludus
