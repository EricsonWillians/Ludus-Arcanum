#include "board_gl_area.hpp"

#include "ludus/render/batch.hpp"

#include <epoxy/gl.h>
#include <cairomm/context.h>
#include <cairomm/surface.h>
#include <gtkmm/eventcontrollerscroll.h>
#include <gtkmm/eventcontrollermotion.h>
#include <gtkmm/gestureclick.h>
#include <gtkmm/gesturedrag.h>
#include <pangomm/fontdescription.h>
#include <pangomm/layout.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <numbers>
#include <iomanip>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace ludus {
namespace {

constexpr std::string_view vertex_shader_body = R"glsl(
layout(location = 0) in vec2 vertex_position;
layout(location = 1) in vec2 instance_center;
layout(location = 2) in vec2 instance_size;
layout(location = 3) in vec4 instance_color;
layout(location = 4) in vec4 instance_uv;
layout(location = 5) in float instance_textured;
layout(location = 6) in float instance_rotation;
layout(location = 7) in float instance_shape;
layout(location = 8) in float instance_border_width;
layout(location = 9) in vec4 instance_border_color;
layout(location = 10) in float instance_atlas_page;
layout(location = 11) in float instance_nearest;

uniform vec2 camera_center;
uniform vec2 camera_half_extent;
uniform float camera_rotation;

out vec2 fragment_uv;
out vec4 fragment_color;
flat out float fragment_textured;
out vec2 fragment_local;
flat out float fragment_shape;
flat out float fragment_border_width;
flat out vec4 fragment_border_color;
flat out float fragment_atlas_page;
flat out float fragment_nearest;

void main() {
    float cosine = cos(instance_rotation);
    float sine = sin(instance_rotation);
    mat2 rotation = mat2(cosine, sine, -sine, cosine);
    vec2 world = instance_center + rotation * (vertex_position * instance_size);
    float camera_cosine = cos(-camera_rotation);
    float camera_sine = sin(-camera_rotation);
    mat2 camera_transform = mat2(camera_cosine, camera_sine,
                                  -camera_sine, camera_cosine);
    vec2 clip = camera_transform * (world - camera_center) / camera_half_extent;
    gl_Position = vec4(clip, 0.0, 1.0);
    vec2 interpolation = vertex_position + vec2(0.5);
    fragment_uv = vec2(mix(instance_uv.x, instance_uv.z, interpolation.x),
                       mix(instance_uv.w, instance_uv.y, interpolation.y));
    fragment_color = instance_color;
    fragment_textured = instance_textured;
    fragment_local = vertex_position * 2.0;
    fragment_shape = instance_shape;
    fragment_border_width = instance_border_width;
    fragment_border_color = instance_border_color;
    fragment_atlas_page = instance_atlas_page;
    fragment_nearest = instance_nearest;
}
)glsl";

constexpr std::string_view fragment_shader_body = R"glsl(
in vec2 fragment_uv;
in vec4 fragment_color;
flat in float fragment_textured;
in vec2 fragment_local;
flat in float fragment_shape;
flat in float fragment_border_width;
flat in vec4 fragment_border_color;
flat in float fragment_atlas_page;
flat in float fragment_nearest;

uniform sampler2DArray atlas_texture;
uniform sampler2DArray text_texture;
out vec4 output_color;

float shape_distance(vec2 point, float shape) {
    vec2 absolute_point = abs(point);
    if (shape > 2.5) {
        return max(absolute_point.x, absolute_point.x * 0.5 + absolute_point.y) - 1.0;
    }
    if (shape > 1.5) {
        return length(point) - 1.0;
    }
    if (shape > 0.5) {
        vec2 rounded = absolute_point - vec2(0.82);
        return length(max(rounded, vec2(0.0))) + min(max(rounded.x, rounded.y), 0.0) - 0.18;
    }
    return max(absolute_point.x, absolute_point.y) - 1.0;
}

void main() {
    float distance_to_edge = shape_distance(fragment_local, fragment_shape);
    float smoothing = max(fwidth(distance_to_edge), 0.001);
    float coverage = 1.0 - smoothstep(-smoothing, smoothing, distance_to_edge);
    if (coverage <= 0.0) {
        discard;
    }
    vec4 sampled = vec4(1.0);
    if (fragment_textured > 1.5) {
        sampled = texture(text_texture, vec3(fragment_uv, fragment_atlas_page));
    } else if (fragment_textured > 0.5 && fragment_nearest > 0.5) {
        ivec2 dimensions = textureSize(atlas_texture, 0).xy;
        ivec2 texel = clamp(ivec2(fragment_uv * vec2(dimensions)), ivec2(0),
                            dimensions - ivec2(1));
        sampled = texelFetch(atlas_texture,
                             ivec3(texel, int(fragment_atlas_page + 0.5)), 0);
    } else if (fragment_textured > 0.5) {
        sampled = texture(atlas_texture, vec3(fragment_uv, fragment_atlas_page));
    }
    vec4 color = vec4(sampled.rgb * fragment_color.rgb * fragment_color.a,
                      sampled.a * fragment_color.a);
    if (fragment_border_width > 0.0 && fragment_border_color.a > 0.0) {
        float border = smoothstep(-max(fragment_border_width, smoothing), -smoothing,
                                  distance_to_edge);
        vec4 premultiplied_border = vec4(fragment_border_color.rgb * fragment_border_color.a,
                                         fragment_border_color.a);
        color = mix(color, premultiplied_border, border);
    }
    output_color = color * coverage;
}
)glsl";

std::string shader_source(RendererBackend backend, std::string_view body) {
    const auto header = backend == RendererBackend::gles
                            ? "#version 300 es\n"
                              "precision highp float;\n"
                              "precision highp int;\n"
                              "precision lowp sampler2DArray;\n"
                            : "#version 330 core\n";
    return std::string{header} + std::string{body};
}

std::string gl_string(GLenum name) {
    const auto* value = glGetString(name);
    return value == nullptr ? std::string{} : std::string{reinterpret_cast<const char*>(value)};
}

void upload_texture_array(GLuint texture, const TextureAtlas& atlas, GLenum texture_unit) {
    const auto pages = atlas.pages();
    if (pages.empty()) {
        return;
    }
    const auto& image = pages.front();
    GLint maximum_layers = 0;
    glGetIntegerv(GL_MAX_ARRAY_TEXTURE_LAYERS, &maximum_layers);
    if (pages.size() > static_cast<std::size_t>(std::max(maximum_layers, 0))) {
        throw std::runtime_error{"texture atlas exceeds GPU array layer capacity"};
    }
    glActiveTexture(texture_unit);
    glBindTexture(GL_TEXTURE_2D_ARRAY, texture);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_SRGB8_ALPHA8,
                 static_cast<GLsizei>(image.width), static_cast<GLsizei>(image.height),
                 static_cast<GLsizei>(pages.size()), 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    for (std::size_t page = 0U; page < pages.size(); ++page) {
        glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, static_cast<GLint>(page),
                        static_cast<GLsizei>(image.width),
                        static_cast<GLsizei>(image.height), 1, GL_RGBA,
                        GL_UNSIGNED_BYTE, pages[page].pixels.data());
    }
}

class ShaderHandle {
  public:
    explicit ShaderHandle(GLuint shader) noexcept : shader_(shader) {}
    ShaderHandle(const ShaderHandle&) = delete;
    ShaderHandle& operator=(const ShaderHandle&) = delete;
    ~ShaderHandle() {
        if (shader_ != 0U) {
            glDeleteShader(shader_);
        }
    }

    [[nodiscard]] GLuint get() const noexcept { return shader_; }

  private:
    GLuint shader_{0U};
};

GLuint compile_shader(GLenum type, std::string_view source) {
    const auto shader = glCreateShader(type);
    const auto* data = source.data();
    const auto length = static_cast<GLint>(source.size());
    glShaderSource(shader, 1, &data, &length);
    glCompileShader(shader);
    GLint compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled == GL_TRUE) {
        return shader;
    }
    GLint log_length = 0;
    glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &log_length);
    std::string log(static_cast<std::size_t>(std::max(log_length, 1)), '\0');
    glGetShaderInfoLog(shader, log_length, nullptr, log.data());
    glDeleteShader(shader);
    throw std::runtime_error{"OpenGL shader compilation failed: " + log};
}

GLuint link_program(GLuint vertex_shader, GLuint fragment_shader) {
    const auto program = glCreateProgram();
    glAttachShader(program, vertex_shader);
    glAttachShader(program, fragment_shader);
    glLinkProgram(program);
    GLint linked = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (linked == GL_TRUE) {
        return program;
    }
    GLint log_length = 0;
    glGetProgramiv(program, GL_INFO_LOG_LENGTH, &log_length);
    std::string log(static_cast<std::size_t>(std::max(log_length, 1)), '\0');
    glGetProgramInfoLog(program, log_length, nullptr, log.data());
    glDeleteProgram(program);
    throw std::runtime_error{"OpenGL program linking failed: " + log};
}

double percentile(std::vector<double> values, double quantile) {
    if (values.empty()) {
        return 0.0;
    }
    std::ranges::sort(values);
    const auto index = static_cast<std::size_t>(
        std::clamp(quantile, 0.0, 1.0) * static_cast<double>(values.size() - 1U));
    return values[index];
}

} // namespace

struct BoardGLArea::GpuResources {
    // Deep enough to avoid synchronizing the CPU with composited GL command queues.
    static constexpr std::size_t query_count = 128U;
    static constexpr std::size_t instance_ring_size = 3U;
    GLuint program{0U};
    GLuint vertex_array{0U};
    GLuint quad_buffer{0U};
    GLuint instance_buffer{0U};
    GLuint texture{0U};
    GLuint text_texture{0U};
    GLint camera_center{-1};
    GLint camera_half_extent{-1};
    GLint camera_rotation{-1};
    SpriteBatch batch;
    std::size_t instance_segment_capacity_bytes{0U};
    std::size_t instance_ring_cursor{0U};
    std::byte* mapped_instances{nullptr};
    std::array<GLsync, instance_ring_size> instance_fences{};
    std::array<GLuint, query_count> timing_queries{};
    std::array<bool, query_count> query_pending{};
    std::array<bool, query_count> query_capture{};
    std::vector<double> gpu_milliseconds;
    std::vector<double> cpu_milliseconds;
    std::vector<double> presented_milliseconds;
    std::chrono::steady_clock::time_point previous_frame{};
    std::uint64_t stress_frame{0U};
    bool timer_queries{false};
    bool persistent_instances{false};
    bool instance_ring_initialized{false};
    bool stress_reported{false};
    bool ready{false};
};

struct BoardGLArea::TextResources {
    struct Placement {
        SpriteId sprite;
        Vec2 position;
        Vec2 pixel_size;
        Color color;
        float layer{0.0F};
        bool screen_space{false};
    };

    std::optional<TextureAtlas> atlas;
    std::vector<Placement> placements;
    std::vector<QuadInstance> frame_instances;
    int device_scale{0};
};

BoardGLArea::BoardGLArea(RendererBackend backend)
    : gpu_(std::make_unique<GpuResources>()), text_(std::make_unique<TextResources>()),
      backend_(backend) {
    if (backend_ != RendererBackend::desktop_gl && backend_ != RendererBackend::gles) {
        throw std::invalid_argument{"BoardGLArea requires an accelerated renderer backend"};
    }
    set_use_es(backend_ == RendererBackend::gles);
    set_required_version(backend_ == RendererBackend::gles ? 3 : 3,
                         backend_ == RendererBackend::gles ? 0 : 3);
    set_has_depth_buffer(false);
    set_has_stencil_buffer(false);
    set_auto_render(false);
    set_hexpand(true);
    set_vexpand(true);
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
            queue_render();
        } else if (snapshot_ && has_active_animations(*snapshot_, now)) {
            queue_render();
        }
        return true;
    });

    signal_realize().connect(sigc::mem_fun(*this, &BoardGLArea::realize_resources), false);
    signal_unrealize().connect(sigc::mem_fun(*this, &BoardGLArea::release_resources), false);

    const auto click = Gtk::GestureClick::create();
    click->signal_pressed().connect(sigc::mem_fun(*this, &BoardGLArea::on_click));
    add_controller(click);

    const auto motion = Gtk::EventControllerMotion::create();
    motion->signal_motion().connect([this](double x, double y) {
        pointer_position_ = {static_cast<float>(x), static_cast<float>(y)};
        if (snapshot_) {
            const auto hovered = pick_space(*snapshot_, camera_.screen_to_world(pointer_position_));
            if (hovered != interaction_.hovered) {
                interaction_.hovered = hovered;
                queue_render();
            }
        }
    });
    motion->signal_leave().connect([this] {
        interaction_.hovered.reset();
        queue_render();
    });
    add_controller(motion);

    const auto piece_drag = Gtk::GestureDrag::create();
    piece_drag->set_button(1U);
    piece_drag->signal_drag_begin().connect(
        sigc::mem_fun(*this, &BoardGLArea::on_piece_drag_begin));
    piece_drag->signal_drag_update().connect(
        sigc::mem_fun(*this, &BoardGLArea::on_piece_drag_update));
    piece_drag->signal_drag_end().connect(
        sigc::mem_fun(*this, &BoardGLArea::on_piece_drag_end));
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
        queue_render();
    });
    add_controller(pan_drag);

    const auto scroll = Gtk::EventControllerScroll::create();
    scroll->set_flags(Gtk::EventControllerScroll::Flags::BOTH_AXES);
    scroll->signal_scroll().connect(sigc::mem_fun(*this, &BoardGLArea::on_scroll), false);
    add_controller(scroll);
}

BoardGLArea::~BoardGLArea() = default;

void BoardGLArea::set_snapshot(std::shared_ptr<const RenderSnapshot> snapshot) {
    snapshot_ = std::move(snapshot);
    if (snapshot_ && !camera_fitted_) {
        camera_.fit(snapshot_->world_bounds);
        camera_fitted_ = true;
    }
    rebuild_text_cache();
    queue_render();
}

void BoardGLArea::set_texture_atlas(TextureAtlas atlas) {
    atlas_ = std::move(atlas);
    if (gpu_->ready) {
        make_current();
        if (!has_error()) {
            upload_atlas();
        }
    }
    queue_render();
}

void BoardGLArea::set_font_families(std::vector<std::string> families) {
    if (families.empty()) {
        families.emplace_back("Serif");
    }
    font_families_.clear();
    for (const auto& family : families) {
        if (!font_families_.empty()) {
            font_families_ += ", ";
        }
        font_families_ += family;
    }
    rebuild_text_cache();
    queue_render();
}

void BoardGLArea::set_focused_space(std::optional<SpaceId> space) {
    interaction_.keyboard_focus = space;
    queue_render();
}

void BoardGLArea::set_interaction(InteractionState interaction) {
    interaction_ = std::move(interaction);
    std::ranges::sort(interaction_.targets, [](const auto& left, const auto& right) {
        return left.space == right.space ? left.kind < right.kind : left.space < right.space;
    });
    const auto unique = std::ranges::unique(interaction_.targets).begin();
    interaction_.targets.erase(unique, interaction_.targets.end());
    queue_render();
}

void BoardGLArea::set_interaction(std::optional<EntityId> selected,
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

void BoardGLArea::reset_camera() {
    if (snapshot_) {
        camera_.fit(snapshot_->world_bounds);
        camera_fitted_ = true;
        queue_render();
    }
}

void BoardGLArea::flip_board() {
    camera_.set_rotation(camera_.rotation() + std::numbers::pi_v<float>);
    queue_render();
}

void BoardGLArea::realize_resources() {
    make_current();
    if (has_error()) {
        std::string message = "Unable to create a ";
        message += renderer_backend_name(backend_);
        message += " context";
        try {
            throw_if_error();
        } catch (const Glib::Error& error) {
            message += ": ";
            message += error.what();
        }
        render_error_.emit(message);
        return;
    }
    try {
        const auto vertex_source = shader_source(backend_, vertex_shader_body);
        const auto fragment_source = shader_source(backend_, fragment_shader_body);
        const ShaderHandle vertex_shader{compile_shader(GL_VERTEX_SHADER, vertex_source)};
        const ShaderHandle fragment_shader{
            compile_shader(GL_FRAGMENT_SHADER, fragment_source)};
        gpu_->program = link_program(vertex_shader.get(), fragment_shader.get());

        constexpr std::array<float, 12U> quad{
            -0.5F, -0.5F, 0.5F,  -0.5F, 0.5F, 0.5F,
            -0.5F, -0.5F, 0.5F, 0.5F,  -0.5F, 0.5F,
        };
        glGenVertexArrays(1, &gpu_->vertex_array);
        glBindVertexArray(gpu_->vertex_array);
        glGenBuffers(1, &gpu_->quad_buffer);
        glBindBuffer(GL_ARRAY_BUFFER, gpu_->quad_buffer);
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(sizeof(quad)), quad.data(),
                     GL_STATIC_DRAW);
        glEnableVertexAttribArray(0U);
        glVertexAttribPointer(0U, 2, GL_FLOAT, GL_FALSE, 2 * static_cast<GLsizei>(sizeof(float)),
                              nullptr);

        glGenBuffers(1, &gpu_->instance_buffer);
        glBindBuffer(GL_ARRAY_BUFFER, gpu_->instance_buffer);
        gpu_->persistent_instances =
            backend_ == RendererBackend::gles
                ? epoxy_has_gl_extension("GL_EXT_buffer_storage")
                : epoxy_has_gl_extension("GL_ARB_buffer_storage");
        constexpr auto stride = static_cast<GLsizei>(sizeof(QuadInstance));
        glEnableVertexAttribArray(1U);
        glVertexAttribPointer(1U, 2, GL_FLOAT, GL_FALSE, stride,
                              reinterpret_cast<void*>(offsetof(QuadInstance, center)));
        glEnableVertexAttribArray(2U);
        glVertexAttribPointer(2U, 2, GL_FLOAT, GL_FALSE, stride,
                              reinterpret_cast<void*>(offsetof(QuadInstance, size)));
        glEnableVertexAttribArray(3U);
        glVertexAttribPointer(3U, 4, GL_FLOAT, GL_FALSE, stride,
                              reinterpret_cast<void*>(offsetof(QuadInstance, color)));
        glEnableVertexAttribArray(4U);
        glVertexAttribPointer(4U, 4, GL_FLOAT, GL_FALSE, stride,
                              reinterpret_cast<void*>(offsetof(QuadInstance, uv)));
        glEnableVertexAttribArray(5U);
        glVertexAttribPointer(5U, 1, GL_FLOAT, GL_FALSE, stride,
                              reinterpret_cast<void*>(offsetof(QuadInstance, textured)));
        glEnableVertexAttribArray(6U);
        glVertexAttribPointer(6U, 1, GL_FLOAT, GL_FALSE, stride,
                              reinterpret_cast<void*>(offsetof(QuadInstance, rotation)));
        glEnableVertexAttribArray(7U);
        glVertexAttribPointer(7U, 1, GL_FLOAT, GL_FALSE, stride,
                              reinterpret_cast<void*>(offsetof(QuadInstance, shape)));
        glEnableVertexAttribArray(8U);
        glVertexAttribPointer(8U, 1, GL_FLOAT, GL_FALSE, stride,
                              reinterpret_cast<void*>(offsetof(QuadInstance, border_width)));
        glEnableVertexAttribArray(9U);
        glVertexAttribPointer(9U, 4, GL_FLOAT, GL_FALSE, stride,
                              reinterpret_cast<void*>(offsetof(QuadInstance, border_color)));
        glEnableVertexAttribArray(10U);
        glVertexAttribPointer(10U, 1, GL_FLOAT, GL_FALSE, stride,
                              reinterpret_cast<void*>(offsetof(QuadInstance, atlas_page)));
        glEnableVertexAttribArray(11U);
        glVertexAttribPointer(11U, 1, GL_FLOAT, GL_FALSE, stride,
                              reinterpret_cast<void*>(offsetof(QuadInstance, nearest)));
        for (GLuint attribute = 1U; attribute <= 11U; ++attribute) {
            glVertexAttribDivisor(attribute, 1U);
        }

        glGenTextures(1, &gpu_->texture);
        glGenTextures(1, &gpu_->text_texture);
        gpu_->timer_queries = backend_ == RendererBackend::desktop_gl ||
                              epoxy_has_gl_extension("GL_EXT_disjoint_timer_query");
        if (gpu_->timer_queries) {
            if (backend_ == RendererBackend::gles) {
                glGenQueriesEXT(static_cast<GLsizei>(gpu_->query_count),
                                gpu_->timing_queries.data());
            } else {
                glGenQueries(static_cast<GLsizei>(gpu_->query_count),
                             gpu_->timing_queries.data());
            }
        }
        gpu_->camera_center = glGetUniformLocation(gpu_->program, "camera_center");
        gpu_->camera_half_extent = glGetUniformLocation(gpu_->program, "camera_half_extent");
        gpu_->camera_rotation = glGetUniformLocation(gpu_->program, "camera_rotation");
        glUseProgram(gpu_->program);
        glUniform1i(glGetUniformLocation(gpu_->program, "atlas_texture"), 0);
        glUniform1i(glGetUniformLocation(gpu_->program, "text_texture"), 1);
        gpu_->ready = true;
        upload_atlas();
        upload_text_atlas();
        info_ = RendererInfo{backend_, true, renderer_backend_name(backend_),
                             gl_string(GL_VENDOR), gl_string(GL_RENDERER),
                             gl_string(GL_VERSION), {}};
        renderer_ready_.emit(info_);
    } catch (const std::exception& error) {
        release_resources();
        render_error_.emit(error.what());
        std::cerr << "ludus-player: " << error.what() << '\n';
    }
}

void BoardGLArea::release_resources() noexcept {
    if (!gpu_->ready && gpu_->program == 0U && gpu_->vertex_array == 0U) {
        return;
    }
    make_current();
    for (auto& fence : gpu_->instance_fences) {
        if (fence != nullptr) {
            glDeleteSync(fence);
            fence = nullptr;
        }
    }
    if (gpu_->mapped_instances != nullptr && gpu_->instance_buffer != 0U) {
        glBindBuffer(GL_ARRAY_BUFFER, gpu_->instance_buffer);
        glUnmapBuffer(GL_ARRAY_BUFFER);
        gpu_->mapped_instances = nullptr;
    }
    if (gpu_->texture != 0U) {
        glDeleteTextures(1, &gpu_->texture);
    }
    if (gpu_->text_texture != 0U) {
        glDeleteTextures(1, &gpu_->text_texture);
    }
    if (gpu_->timing_queries.front() != 0U) {
        if (backend_ == RendererBackend::gles) {
            glDeleteQueriesEXT(static_cast<GLsizei>(gpu_->query_count),
                               gpu_->timing_queries.data());
        } else {
            glDeleteQueries(static_cast<GLsizei>(gpu_->query_count),
                            gpu_->timing_queries.data());
        }
    }
    if (gpu_->instance_buffer != 0U) {
        glDeleteBuffers(1, &gpu_->instance_buffer);
    }
    if (gpu_->quad_buffer != 0U) {
        glDeleteBuffers(1, &gpu_->quad_buffer);
    }
    if (gpu_->vertex_array != 0U) {
        glDeleteVertexArrays(1, &gpu_->vertex_array);
    }
    if (gpu_->program != 0U) {
        glDeleteProgram(gpu_->program);
    }
    *gpu_ = GpuResources{};
}

void BoardGLArea::upload_atlas() {
    if (!gpu_->ready || !atlas_ || !atlas_->image().valid()) {
        return;
    }
    upload_texture_array(gpu_->texture, *atlas_, GL_TEXTURE0);
}

void BoardGLArea::rebuild_text_cache() {
    text_->placements.clear();
    text_->atlas.reset();
    text_->device_scale = std::max(get_scale_factor(), 1);
    if (!snapshot_ || snapshot_->texts.empty()) {
        return;
    }
    std::vector<ImageRgba> images;
    images.reserve(snapshot_->texts.size());
    text_->placements.reserve(snapshot_->texts.size());
    for (const auto& visual : snapshot_->texts) {
        const auto layout = create_pango_layout(visual.text);
        Pango::FontDescription font;
        font.set_family(font_families_);
        font.set_weight(Pango::Weight::BOLD);
        font.set_absolute_size(static_cast<double>(visual.size) *
                               static_cast<double>(text_->device_scale) * Pango::SCALE);
        layout->set_font_description(font);
        int layout_width = 0;
        int layout_height = 0;
        layout->get_pixel_size(layout_width, layout_height);
        const auto width = std::max(layout_width + 4, 1);
        const auto height = std::max(layout_height + 4, 1);
        const auto surface = Cairo::ImageSurface::create(Cairo::Surface::Format::ARGB32,
                                                         width, height);
        const auto cairo = Cairo::Context::create(surface);
        cairo->set_source_rgba(1.0, 1.0, 1.0, 1.0);
        cairo->move_to(2.0, 2.0);
        layout->show_in_cairo_context(cairo);
        surface->flush();
        ImageRgba image{static_cast<std::uint32_t>(width),
                        static_cast<std::uint32_t>(height),
                        std::vector<std::uint8_t>(
                            static_cast<std::size_t>(width) *
                                static_cast<std::size_t>(height) * 4U,
                            0U)};
        const auto* data = surface->get_data();
        const auto stride = surface->get_stride();
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const auto source_offset = static_cast<std::size_t>(y) *
                                               static_cast<std::size_t>(stride) +
                                           static_cast<std::size_t>(x) * 4U;
                const auto destination_offset =
                    (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                     static_cast<std::size_t>(x)) *
                    4U;
                image.pixels[destination_offset] = 255U;
                image.pixels[destination_offset + 1U] = 255U;
                image.pixels[destination_offset + 2U] = 255U;
                image.pixels[destination_offset + 3U] = data[source_offset + 3U];
            }
        }
        const auto sprite = SpriteId{static_cast<std::uint32_t>(images.size())};
        images.push_back(std::move(image));
        text_->placements.push_back(TextResources::Placement{
            sprite, visual.position,
            {static_cast<float>(width) / static_cast<float>(text_->device_scale),
             static_cast<float>(height) / static_cast<float>(text_->device_scale)},
            visual.color, visual.layer, visual.screen_space});
    }
    auto atlas = TextureAtlas::pack(images, 2U);
    if (!atlas) {
        render_error_.emit("Unable to build high-DPI Pango text cache: " +
                           atlas.error().message);
        text_->placements.clear();
        return;
    }
    text_->atlas = std::move(*atlas);
    if (gpu_->ready) {
        make_current();
        if (!has_error()) {
            upload_text_atlas();
        }
    }
}

void BoardGLArea::upload_text_atlas() {
    if (gpu_->ready && text_->atlas) {
        upload_texture_array(gpu_->text_texture, *text_->atlas, GL_TEXTURE1);
        glActiveTexture(GL_TEXTURE0);
    }
}

bool BoardGLArea::on_render(const Glib::RefPtr<Gdk::GLContext>& context) {
    static_cast<void>(context);
    const auto pixel_width = std::max(get_width() * get_scale_factor(), 1);
    const auto pixel_height = std::max(get_height() * get_scale_factor(), 1);
    glViewport(0, 0, pixel_width, pixel_height);
    glClearColor(0.055F, 0.065F, 0.08F, 1.0F);
    glClear(GL_COLOR_BUFFER_BIT);
    if (!gpu_->ready || !snapshot_) {
        return true;
    }

    const auto now = std::chrono::steady_clock::now();
    const bool stress = snapshot_->pieces.size() >= 10'000U && !gpu_->stress_reported;
    bool capture_frame = false;
    bool query_started = false;
    std::size_t query_slot = 0U;
    if (stress) {
        query_slot = static_cast<std::size_t>(gpu_->stress_frame % gpu_->query_count);
        if (gpu_->timer_queries && gpu_->query_pending[query_slot]) {
            GLuint available = GL_FALSE;
            if (backend_ == RendererBackend::gles) {
                glGetQueryObjectuivEXT(gpu_->timing_queries[query_slot],
                                       GL_QUERY_RESULT_AVAILABLE, &available);
            } else {
                glGetQueryObjectuiv(gpu_->timing_queries[query_slot],
                                    GL_QUERY_RESULT_AVAILABLE, &available);
            }
            if (available != GL_FALSE) {
                GLuint64 nanoseconds = 0U;
                if (backend_ == RendererBackend::gles) {
                    glGetQueryObjectui64vEXT(gpu_->timing_queries[query_slot],
                                             GL_QUERY_RESULT, &nanoseconds);
                } else {
                    glGetQueryObjectui64v(gpu_->timing_queries[query_slot], GL_QUERY_RESULT,
                                          &nanoseconds);
                }
                if (gpu_->query_capture[query_slot]) {
                    gpu_->gpu_milliseconds.push_back(
                        static_cast<double>(nanoseconds) / 1'000'000.0);
                }
                gpu_->query_pending[query_slot] = false;
            }
        }
        capture_frame = gpu_->stress_frame >= 120U && gpu_->stress_frame < 720U;
        if (gpu_->previous_frame != std::chrono::steady_clock::time_point{} && capture_frame) {
            gpu_->presented_milliseconds.push_back(
                std::chrono::duration<double, std::milli>{now - gpu_->previous_frame}.count());
        }
        gpu_->previous_frame = now;
        if (gpu_->timer_queries && !gpu_->query_pending[query_slot]) {
            if (backend_ == RendererBackend::gles) {
                glBeginQueryEXT(GL_TIME_ELAPSED_EXT, gpu_->timing_queries[query_slot]);
            } else {
                glBeginQuery(GL_TIME_ELAPSED, gpu_->timing_queries[query_slot]);
            }
            gpu_->query_pending[query_slot] = true;
            gpu_->query_capture[query_slot] = capture_frame;
            query_started = true;
        }
    }
    gpu_->batch.prepare(*snapshot_, atlas_ ? &*atlas_ : nullptr, interaction_, now);
    const auto batch_instances = gpu_->batch.instances();
    const auto batch_additive_offset =
        std::min(gpu_->batch.additive_offset(), batch_instances.size());
    text_->frame_instances.assign(
        batch_instances.begin(),
        batch_instances.begin() + static_cast<std::ptrdiff_t>(batch_additive_offset));
    if (text_->atlas && get_width() > 0 && get_height() > 0) {
        const auto world_per_pixel_x = camera_.visible_half_width() * 2.0F /
                                       static_cast<float>(get_width());
        const auto world_per_pixel_y = camera_.visible_half_height() * 2.0F /
                                       static_cast<float>(get_height());
        for (const auto& placement : text_->placements) {
            const auto* region = text_->atlas->region(placement.sprite);
            if (region == nullptr) {
                continue;
            }
            const auto screen = placement.screen_space
                                    ? Vec2{placement.position.x * static_cast<float>(get_width()),
                                           placement.position.y * static_cast<float>(get_height())}
                                    : camera_.world_to_screen(placement.position);
            const auto center = camera_.screen_to_world(screen);
            text_->frame_instances.push_back(QuadInstance{
                {center.x, center.y},
                {placement.pixel_size.x * world_per_pixel_x,
                 placement.pixel_size.y * world_per_pixel_y},
                {placement.color.red, placement.color.green, placement.color.blue,
                 placement.color.alpha},
                {region->u_min, region->v_min, region->u_max, region->v_max},
                2.0F,
                camera_.rotation(),
                static_cast<float>(SpaceShape::rectangle),
                0.0F,
                {0.0F, 0.0F, 0.0F, 0.0F},
                static_cast<float>(region->page),
                0.0F,
                placement.layer});
        }
    }
    std::ranges::stable_sort(text_->frame_instances, {}, &QuadInstance::layer);
    const auto additive_instance_offset = text_->frame_instances.size();
    text_->frame_instances.insert(
        text_->frame_instances.end(),
        batch_instances.begin() + static_cast<std::ptrdiff_t>(batch_additive_offset),
        batch_instances.end());
    const auto instances = std::span<const QuadInstance>{text_->frame_instances};

    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    if (backend_ == RendererBackend::desktop_gl) {
        glEnable(GL_FRAMEBUFFER_SRGB);
    }
    glUseProgram(gpu_->program);
    glUniform2f(gpu_->camera_center, camera_.center().x, camera_.center().y);
    glUniform2f(gpu_->camera_half_extent, camera_.visible_half_width(),
                camera_.visible_half_height());
    glUniform1f(gpu_->camera_rotation, camera_.rotation());
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D_ARRAY, gpu_->texture);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D_ARRAY, gpu_->text_texture);
    glActiveTexture(GL_TEXTURE0);
    glBindVertexArray(gpu_->vertex_array);
    glBindBuffer(GL_ARRAY_BUFFER, gpu_->instance_buffer);
    const auto required_bytes = instances.size() * sizeof(QuadInstance);
    if (required_bytes > gpu_->instance_segment_capacity_bytes) {
        for (auto& fence : gpu_->instance_fences) {
            if (fence != nullptr) {
                glDeleteSync(fence);
                fence = nullptr;
            }
        }
        if (gpu_->mapped_instances != nullptr) {
            glUnmapBuffer(GL_ARRAY_BUFFER);
            gpu_->mapped_instances = nullptr;
        }
        glDeleteBuffers(1, &gpu_->instance_buffer);
        glGenBuffers(1, &gpu_->instance_buffer);
        glBindBuffer(GL_ARRAY_BUFFER, gpu_->instance_buffer);
        std::size_t capacity = 4'096U;
        while (capacity < required_bytes) {
            capacity *= 2U;
        }
        const auto ring_capacity = capacity * GpuResources::instance_ring_size;
        if (gpu_->persistent_instances) {
            constexpr auto flags = static_cast<GLbitfield>(
                GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT);
            if (backend_ == RendererBackend::gles) {
                glBufferStorageEXT(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(ring_capacity),
                                   nullptr, flags);
            } else {
                glBufferStorage(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(ring_capacity),
                                nullptr, flags);
            }
            gpu_->mapped_instances = static_cast<std::byte*>(glMapBufferRange(
                GL_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>(ring_capacity), flags));
            if (gpu_->mapped_instances == nullptr) {
                glDeleteBuffers(1, &gpu_->instance_buffer);
                glGenBuffers(1, &gpu_->instance_buffer);
                glBindBuffer(GL_ARRAY_BUFFER, gpu_->instance_buffer);
                gpu_->persistent_instances = false;
            }
        }
        if (!gpu_->persistent_instances) {
            glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(ring_capacity), nullptr,
                         GL_DYNAMIC_DRAW);
        }
        gpu_->instance_segment_capacity_bytes = capacity;
        gpu_->instance_ring_cursor = 0U;
        gpu_->instance_ring_initialized = false;
    }
    const auto ring_segment =
        gpu_->instance_ring_cursor % GpuResources::instance_ring_size;
    const auto instance_offset = ring_segment * gpu_->instance_segment_capacity_bytes;
    if (required_bytes != 0U) {
        if (gpu_->persistent_instances) {
            auto& fence = gpu_->instance_fences[ring_segment];
            if (fence != nullptr) {
                const auto wait = glClientWaitSync(fence, GL_SYNC_FLUSH_COMMANDS_BIT,
                                                   GL_TIMEOUT_IGNORED);
                if (wait == GL_WAIT_FAILED) {
                    throw std::runtime_error{"OpenGL instance-ring fence wait failed"};
                }
                glDeleteSync(fence);
                fence = nullptr;
            }
            std::memcpy(gpu_->mapped_instances + instance_offset, instances.data(),
                        required_bytes);
        } else {
            if (ring_segment == 0U && gpu_->instance_ring_initialized) {
                const auto ring_capacity = gpu_->instance_segment_capacity_bytes *
                                           GpuResources::instance_ring_size;
                glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(ring_capacity), nullptr,
                             GL_DYNAMIC_DRAW);
            }
            glBufferSubData(GL_ARRAY_BUFFER, static_cast<GLintptr>(instance_offset),
                            static_cast<GLsizeiptr>(required_bytes), instances.data());
        }
    }
    constexpr auto stride = static_cast<GLsizei>(sizeof(QuadInstance));
    const auto configure_attributes = [instance_offset](std::size_t first_instance) {
        const auto base = instance_offset + first_instance * sizeof(QuadInstance);
        const auto attribute_offset = [base](std::size_t member) {
            return reinterpret_cast<void*>(base + member);
        };
        glVertexAttribPointer(1U, 2, GL_FLOAT, GL_FALSE, stride,
                              attribute_offset(offsetof(QuadInstance, center)));
        glVertexAttribPointer(2U, 2, GL_FLOAT, GL_FALSE, stride,
                              attribute_offset(offsetof(QuadInstance, size)));
        glVertexAttribPointer(3U, 4, GL_FLOAT, GL_FALSE, stride,
                              attribute_offset(offsetof(QuadInstance, color)));
        glVertexAttribPointer(4U, 4, GL_FLOAT, GL_FALSE, stride,
                              attribute_offset(offsetof(QuadInstance, uv)));
        glVertexAttribPointer(5U, 1, GL_FLOAT, GL_FALSE, stride,
                              attribute_offset(offsetof(QuadInstance, textured)));
        glVertexAttribPointer(6U, 1, GL_FLOAT, GL_FALSE, stride,
                              attribute_offset(offsetof(QuadInstance, rotation)));
        glVertexAttribPointer(7U, 1, GL_FLOAT, GL_FALSE, stride,
                              attribute_offset(offsetof(QuadInstance, shape)));
        glVertexAttribPointer(8U, 1, GL_FLOAT, GL_FALSE, stride,
                              attribute_offset(offsetof(QuadInstance, border_width)));
        glVertexAttribPointer(9U, 4, GL_FLOAT, GL_FALSE, stride,
                              attribute_offset(offsetof(QuadInstance, border_color)));
        glVertexAttribPointer(10U, 1, GL_FLOAT, GL_FALSE, stride,
                              attribute_offset(offsetof(QuadInstance, atlas_page)));
        glVertexAttribPointer(11U, 1, GL_FLOAT, GL_FALSE, stride,
                              attribute_offset(offsetof(QuadInstance, nearest)));
    };
    configure_attributes(0U);
    glDrawArraysInstanced(GL_TRIANGLES, 0, 6,
                          static_cast<GLsizei>(additive_instance_offset));
    if (additive_instance_offset < instances.size()) {
        glBlendFunc(GL_ONE, GL_ONE);
        configure_attributes(additive_instance_offset);
        glDrawArraysInstanced(
            GL_TRIANGLES, 0, 6,
            static_cast<GLsizei>(instances.size() - additive_instance_offset));
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    }
    if (required_bytes != 0U) {
        if (gpu_->persistent_instances) {
            gpu_->instance_fences[ring_segment] =
                glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0U);
        }
        gpu_->instance_ring_initialized = true;
        ++gpu_->instance_ring_cursor;
    }
    if (stress && query_started) {
        if (backend_ == RendererBackend::gles) {
            glEndQueryEXT(GL_TIME_ELAPSED_EXT);
        } else {
            glEndQuery(GL_TIME_ELAPSED);
        }
        glFlush();
    }
    glDisable(GL_BLEND);
    if (backend_ == RendererBackend::desktop_gl) {
        glDisable(GL_FRAMEBUFFER_SRGB);
    }

    if (stress) {
        const auto renderer_cpu = std::chrono::duration<double, std::milli>{
            std::chrono::steady_clock::now() - now}.count();
        if (capture_frame) {
            gpu_->cpu_milliseconds.push_back(renderer_cpu);
        }
        ++gpu_->stress_frame;
        if (gpu_->stress_frame == 10U) {
            std::cout << "ludus-player stress: warm-up renderer CPU sample="
                      << renderer_cpu << " ms" << std::endl;
        }
        if (gpu_->stress_frame == 120U) {
            std::cout << "ludus-player stress: warm-up complete; capturing 600 frames"
                      << std::endl;
        }
        if (gpu_->stress_frame >= 848U) {
            bool disjoint = false;
            if (backend_ == RendererBackend::gles && gpu_->timer_queries) {
                GLint value = GL_FALSE;
                glGetIntegerv(GL_GPU_DISJOINT_EXT, &value);
                disjoint = value != GL_FALSE;
            }
            const auto cpu_median = percentile(gpu_->cpu_milliseconds, 0.5);
            const auto cpu_p95 = percentile(gpu_->cpu_milliseconds, 0.95);
            const auto cpu_p99 = percentile(gpu_->cpu_milliseconds, 0.99);
            const auto presented_median = percentile(gpu_->presented_milliseconds, 0.5);
            const auto presented_p95 = percentile(gpu_->presented_milliseconds, 0.95);
            const auto presented_p99 = percentile(gpu_->presented_milliseconds, 0.99);
            const auto missed = std::ranges::count_if(
                gpu_->presented_milliseconds, [](double milliseconds) {
                    return milliseconds > 18.0;
                });
            std::cout << std::fixed << std::setprecision(3)
                      << "ludus-player stress: backend=" << renderer_backend_name(backend_)
                      << " vendor=\"" << info_.vendor << "\" renderer=\"" << info_.renderer
                      << "\" driver=\"" << info_.version << "\" resolution=" << pixel_width
                      << 'x' << pixel_height << " sprites=" << snapshot_->pieces.size()
                      << " frames=" << gpu_->cpu_milliseconds.size()
                      << " cpu_ms(median/p95/p99)=" << cpu_median << '/' << cpu_p95 << '/'
                      << cpu_p99;
            if (gpu_->timer_queries && !disjoint) {
                std::cout << " gpu_ms(median/p95/p99)="
                          << percentile(gpu_->gpu_milliseconds, 0.5) << '/'
                          << percentile(gpu_->gpu_milliseconds, 0.95) << '/'
                          << percentile(gpu_->gpu_milliseconds, 0.99);
            } else {
                std::cout << " gpu_ms=unavailable";
            }
            std::cout << " presented_ms(median/p95/p99)=" << presented_median << '/'
                      << presented_p95 << '/' << presented_p99 << " missed_frames=" << missed
                      << std::endl;
            gpu_->stress_reported = true;
            stress_complete_.emit();
        }
    }

    if (has_active_animations(*snapshot_, now)) {
        queue_render();
    }
    return true;
}

void BoardGLArea::on_resize(int width, int height) {
    camera_.resize(width, height);
    if (text_->device_scale != std::max(get_scale_factor(), 1)) {
        rebuild_text_cache();
    }
    Gtk::GLArea::on_resize(width, height);
}

void BoardGLArea::on_click(int press_count, double x, double y) {
    static_cast<void>(press_count);
    if (!snapshot_) {
        return;
    }
    const auto world = camera_.screen_to_world(
        Vec2{static_cast<float>(x), static_cast<float>(y)});
    const auto picked = pick_space(*snapshot_, world);
    if (picked) {
        space_activated_.emit(*picked);
    }
}

bool BoardGLArea::on_scroll(double delta_x, double delta_y) {
    static_cast<void>(delta_x);
    const auto factor = static_cast<float>(std::exp(-delta_y * 0.12));
    camera_.zoom_at(pointer_position_, factor);
    queue_render();
    return true;
}

void BoardGLArea::on_piece_drag_begin(double x, double y) {
    spring_back_start_.reset();
    piece_drag_start_ = {static_cast<float>(x), static_cast<float>(y)};
    if (!snapshot_) {
        return;
    }
    const auto origin = pick_space(*snapshot_, camera_.screen_to_world(piece_drag_start_));
    const auto* piece = origin ? find_piece_at(*snapshot_, *origin) : nullptr;
    if (origin && piece != nullptr) {
        interaction_.drag = DragInteraction{piece->id, *origin,
                                            camera_.screen_to_world(piece_drag_start_),
                                            true, false};
        set_cursor("grabbing");
        queue_render();
    }
}

void BoardGLArea::on_piece_drag_update(double offset_x, double offset_y) {
    if (!snapshot_ || !interaction_.drag) {
        return;
    }
    const auto screen = piece_drag_start_ +
                        Vec2{static_cast<float>(offset_x), static_cast<float>(offset_y)};
    interaction_.drag->pointer = camera_.screen_to_world(screen);
    const auto destination = pick_space(*snapshot_, interaction_.drag->pointer);
    interaction_.drag->valid = destination && std::ranges::any_of(
        interaction_.targets, [&](const InteractionTarget& target) {
            return target.space == *destination;
        });
    set_cursor(interaction_.drag->valid ? "grabbing" : "not-allowed");
    queue_render();
}

void BoardGLArea::on_piece_drag_end(double offset_x, double offset_y) {
    if (!snapshot_ || !interaction_.drag) {
        return;
    }
    set_cursor();
    if (std::hypot(offset_x, offset_y) < 6.0) {
        interaction_.drag.reset();
        queue_render();
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
        queue_render();
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
    queue_render();
}

} // namespace ludus
