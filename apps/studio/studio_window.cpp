#include "studio_window.hpp"

#include "ludus/chess/presentation.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <memory>
#include <limits>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ludus::studio_app {
namespace {

std::vector<std::string> python_search_paths() {
    return {
        std::string{LUDUS_SOURCE_DIR} + "/python",
        std::string{LUDUS_SOURCE_DIR} + "/games/orthodox_chess/python",
        std::string{LUDUS_BINARY_DIR} + "/python",
    };
}

void add_field(Gtk::Grid& grid, int row, std::string_view label, Gtk::Widget& field) {
    auto* caption = Gtk::make_managed<Gtk::Label>(std::string{label});
    caption->set_halign(Gtk::Align::START);
    grid.attach(*caption, 0, row, 1, 1);
    grid.attach(field, 1, row, 1, 1);
}

void configure_output(Gtk::TextView& view) {
    view.set_editable(false);
    view.set_cursor_visible(false);
    view.set_monospace(true);
    view.set_left_margin(8);
    view.set_right_margin(8);
    view.set_top_margin(8);
    view.set_bottom_margin(8);
}

std::expected<chess::Position, Diagnostic>
position_from_document(const studio::PackageDocument& document) {
    const auto& definition = document.board();
    if (definition.width != 8U || definition.height != 8U) {
        return std::unexpected(Diagnostic{
            DiagnosticCode::validation_failed,
            "chess-like playtest mode currently requires an 8x8 board",
            {document.board_path().string(), 0U, 0U},
            "The topology editor supports larger boards; the milestone playtest adapter is "
            "the chess package."});
    }
    constexpr std::array<std::pair<std::string_view, chess::PieceType>, 6U> piece_types{{
        {"pawn", chess::PieceType::pawn},
        {"knight", chess::PieceType::knight},
        {"bishop", chess::PieceType::bishop},
        {"rook", chess::PieceType::rook},
        {"queen", chess::PieceType::queen},
        {"king", chess::PieceType::king},
    }};
    std::array<chess::Piece, 64U> board{};
    for (const auto& entity : definition.entities) {
        if (entity.owner > 1U) {
            return std::unexpected(Diagnostic{
                DiagnosticCode::validation_failed,
                "chess-like playtest entities require owner 0 or 1",
                {document.board_path().string(), 0U, 0U}, entity.name});
        }
        const auto found = std::ranges::find_if(piece_types, [&entity](const auto& candidate) {
            return candidate.first == entity.type;
        });
        if (found == piece_types.end()) {
            return std::unexpected(Diagnostic{
                DiagnosticCode::validation_failed,
                "unsupported chess-like entity type: " + entity.type,
                {document.board_path().string(), 0U, 0U},
                "Use pawn, knight, bishop, rook, queen, or king for playtest entities."});
        }
        const auto index = static_cast<std::size_t>(entity.y * 8U + entity.x);
        board[index] = chess::Piece{found->second, entity.owner == 0U
                                                       ? chess::Color::white
                                                       : chess::Color::black};
    }
    return chess::Position::from_components(
        std::move(board), definition.side_to_move == 0U ? chess::Color::white
                                                        : chess::Color::black,
        static_cast<std::uint8_t>(definition.castling_rights), std::nullopt, 0U, 1U);
}

} // namespace

StudioWindow::StudioWindow() : controller_(python_search_paths()) {
    set_title("Ludus Arcanum Studio");
    set_default_size(1380, 920);
    set_child(root_);

    toolbar_.set_margin_start(8);
    toolbar_.set_margin_end(8);
    toolbar_.set_margin_top(8);
    toolbar_.set_margin_bottom(8);
    path_entry_.set_hexpand(true);
    path_entry_.set_placeholder_text("Package directory");
    path_entry_.set_text(
        (std::filesystem::current_path() / "my-variation.ludus").string());
    toolbar_.append(path_entry_);
    toolbar_.append(new_button_);
    toolbar_.append(open_button_);
    toolbar_.append(save_button_);
    toolbar_.append(edit_button_);
    toolbar_.append(playtest_button_);
    toolbar_.append(reload_button_);
    toolbar_.append(undo_button_);
    toolbar_.append(redo_button_);
    toolbar_.append(restart_button_);
    toolbar_.append(fit_button_);
    status_.set_hexpand(true);
    status_.set_halign(Gtk::Align::END);
    status_.set_text("Create or open a package to begin");
    toolbar_.append(status_);
    root_.append(toolbar_);

    content_.set_position(300);
    content_.set_wide_handle(true);
    root_.append(content_);
    inspector_scroll_.set_policy(Gtk::PolicyType::AUTOMATIC, Gtk::PolicyType::AUTOMATIC);
    inspector_scroll_.set_child(inspector_);
    inspector_.set_margin_start(10);
    inspector_.set_margin_end(10);
    inspector_.set_margin_top(10);
    inspector_.set_margin_bottom(10);
    content_.set_start_child(inspector_scroll_);
    content_.set_end_child(workspace_);

    package_heading_.set_markup("<b>Package inspector</b>");
    package_heading_.set_halign(Gtk::Align::START);
    inspector_.append(package_heading_);
    package_grid_.set_column_spacing(8);
    package_grid_.set_row_spacing(6);
    add_field(package_grid_, 0, "ID", package_id_entry_);
    add_field(package_grid_, 1, "Version", package_version_entry_);
    add_field(package_grid_, 2, "Python module", module_entry_);
    inspector_.append(package_grid_);

    board_heading_.set_markup("<b>Board generator</b>");
    board_heading_.set_halign(Gtk::Align::START);
    inspector_.append(board_heading_);
    board_grid_.set_column_spacing(8);
    board_grid_.set_row_spacing(6);
    width_spin_.set_range(1.0, static_cast<double>(studio::BoardDefinition::maximum_extent));
    height_spin_.set_range(1.0, static_cast<double>(studio::BoardDefinition::maximum_extent));
    width_spin_.set_increments(1.0, 8.0);
    height_spin_.set_increments(1.0, 8.0);
    add_field(board_grid_, 0, "Width", width_spin_);
    add_field(board_grid_, 1, "Height", height_spin_);
    board_grid_.attach(generate_button_, 0, 2, 2, 1);
    board_grid_.attach(chess_setup_button_, 0, 3, 2, 1);
    inspector_.append(board_grid_);

    asset_heading_.set_markup("<b>Visual assets</b>");
    asset_heading_.set_halign(Gtk::Align::START);
    inspector_.append(asset_heading_);
    asset_grid_.set_column_spacing(8);
    asset_grid_.set_row_spacing(6);
    asset_path_entry_.set_placeholder_text("/path/to/transparent-sprite.png");
    asset_key_entry_.set_placeholder_text("piece.custom");
    add_field(asset_grid_, 0, "PNG source", asset_path_entry_);
    add_field(asset_grid_, 1, "Sprite name", asset_key_entry_);
    asset_grid_.attach(asset_import_button_, 0, 2, 2, 1);
    inspector_.append(asset_grid_);

    entity_heading_.set_markup("<b>Entity inspector</b>");
    entity_heading_.set_halign(Gtk::Align::START);
    inspector_.append(entity_heading_);
    entity_grid_.set_column_spacing(8);
    entity_grid_.set_row_spacing(6);
    entity_owner_spin_.set_range(0.0, 1.0);
    entity_x_spin_.set_range(0.0, 255.0);
    entity_y_spin_.set_range(0.0, 255.0);
    sprite_model_ = Gtk::StringList::create(std::vector<Glib::ustring>{
        "piece.ivory.pawn", "piece.ivory.knight", "piece.ivory.bishop",
        "piece.ivory.rook", "piece.ivory.queen", "piece.ivory.king",
        "piece.iron.pawn", "piece.iron.knight", "piece.iron.bishop",
        "piece.iron.rook", "piece.iron.queen", "piece.iron.king"});
    entity_sprite_picker_.set_model(sprite_model_);
    entity_sprite_picker_.set_selected(0U);
    add_field(entity_grid_, 0, "Name", entity_name_entry_);
    add_field(entity_grid_, 1, "Type", entity_type_entry_);
    add_field(entity_grid_, 2, "Owner", entity_owner_spin_);
    add_field(entity_grid_, 3, "X", entity_x_spin_);
    add_field(entity_grid_, 4, "Y", entity_y_spin_);
    add_field(entity_grid_, 5, "Named sprite", entity_sprite_picker_);
    entity_grid_.attach(entity_apply_button_, 0, 6, 2, 1);
    entity_grid_.attach(entity_delete_button_, 0, 7, 2, 1);
    inspector_.append(entity_grid_);

    workspace_.set_position(650);
    workspace_.set_wide_handle(true);
    workspace_.set_start_child(editor_notebook_);
    workspace_.set_end_child(output_notebook_);
    editor_notebook_.append_page(board_, "Board / topology");
    source_editor_.set_monospace(true);
    source_editor_.set_left_margin(8);
    source_editor_.set_right_margin(8);
    source_editor_.set_top_margin(8);
    source_editor_.set_bottom_margin(8);
    source_scroll_.set_policy(Gtk::PolicyType::AUTOMATIC, Gtk::PolicyType::AUTOMATIC);
    source_scroll_.set_child(source_editor_);
    editor_notebook_.append_page(source_scroll_, "scripts/game.py");
    configure_output(diagnostics_view_);
    configure_output(events_view_);
    configure_output(state_view_);
    diagnostics_scroll_.set_child(diagnostics_view_);
    events_scroll_.set_child(events_view_);
    state_scroll_.set_child(state_view_);
    output_notebook_.append_page(diagnostics_scroll_, "Diagnostics");
    output_notebook_.append_page(events_scroll_, "Event log");
    output_notebook_.append_page(state_scroll_, "State inspector");

    if (auto atlas = chess::make_default_chess_atlas(); atlas) {
        board_.set_texture_atlas(std::move(*atlas));
    }
    board_.signal_space_activated().connect(
        sigc::mem_fun(*this, &StudioWindow::on_space_activated));
    board_.signal_render_error().connect(
        [this](const std::string& message) { show_message(message, true); });
    new_button_.signal_clicked().connect(sigc::mem_fun(*this, &StudioWindow::create_package));
    open_button_.signal_clicked().connect(sigc::mem_fun(*this, &StudioWindow::open_package));
    save_button_.signal_clicked().connect([this] { static_cast<void>(save_package()); });
    edit_button_.signal_clicked().connect(sigc::mem_fun(*this, &StudioWindow::return_to_edit));
    playtest_button_.signal_clicked().connect(
        sigc::mem_fun(*this, &StudioWindow::start_playtest));
    reload_button_.signal_clicked().connect(sigc::mem_fun(*this, &StudioWindow::reload_rules));
    undo_button_.signal_clicked().connect([this] {
        if (!controller_.undo()) {
            report_queue_full();
        }
    });
    redo_button_.signal_clicked().connect([this] {
        if (!controller_.redo()) {
            report_queue_full();
        }
    });
    restart_button_.signal_clicked().connect([this] {
        selected_piece_.reset();
        update_playtest_interaction();
        if (!controller_.restart()) {
            report_queue_full();
        }
    });
    fit_button_.signal_clicked().connect([this] { board_.reset_camera(); });
    generate_button_.signal_clicked().connect(sigc::mem_fun(*this, &StudioWindow::generate_board));
    chess_setup_button_.signal_clicked().connect(
        sigc::mem_fun(*this, &StudioWindow::reset_chess_setup));
    asset_import_button_.signal_clicked().connect(
        sigc::mem_fun(*this, &StudioWindow::import_asset));
    entity_apply_button_.signal_clicked().connect(
        sigc::mem_fun(*this, &StudioWindow::upsert_entity));
    entity_delete_button_.signal_clicked().connect(
        sigc::mem_fun(*this, &StudioWindow::delete_entity));

    dispatcher_connection_ =
        controller_.dispatcher().connect(sigc::mem_fun(*this, &StudioWindow::on_playtest_view));
    controller_.start();
    return_to_edit();
}

StudioWindow::~StudioWindow() {
    controller_.stop();
    dispatcher_connection_.disconnect();
}

void StudioWindow::create_package() {
    const auto identifier = package_id_entry_.get_text().empty()
                                ? std::string{"org.example.my-variation"}
                                : package_id_entry_.get_text().raw();
    auto created = studio::PackageDocument::create(
        std::filesystem::path{path_entry_.get_text().raw()}, identifier);
    if (!created) {
        show_diagnostic(created.error());
        return;
    }
    load_document(std::move(*created));
    show_message("Created complete package: " + document_->root().string());
}

void StudioWindow::open_package() {
    auto opened = studio::PackageDocument::open(
        std::filesystem::path{path_entry_.get_text().raw()});
    if (!opened) {
        show_diagnostic(opened.error());
        return;
    }
    load_document(std::move(*opened));
    show_message("Opened " + document_->manifest().id);
}

bool StudioWindow::save_package() {
    if (!document_) {
        show_message("No package is open", true);
        return false;
    }
    auto manifest = document_->manifest();
    manifest.id = package_id_entry_.get_text().raw();
    manifest.version = package_version_entry_.get_text().raw();
    manifest.entry_point = module_entry_.get_text().raw();
    if (auto updated = document_->set_manifest(std::move(manifest)); !updated) {
        show_diagnostic(updated.error());
        return false;
    }
    document_->set_python_source(source_editor_.get_buffer()->get_text().raw());
    if (auto saved = document_->save(); !saved) {
        show_diagnostic(saved.error());
        return false;
    }
    show_message("Saved package atomically");
    return true;
}

void StudioWindow::load_document(studio::PackageDocument document) {
    document_ = std::move(document);
    path_entry_.set_text(document_->root().string());
    source_editor_.get_buffer()->set_text(document_->python_source());
    selected_entity_name_.reset();
    select_entity(nullptr);
    refresh_package_fields();
    refresh_visual_theme();
    return_to_edit();
    refresh_preview();
}

void StudioWindow::refresh_package_fields() {
    if (!document_) {
        return;
    }
    package_id_entry_.set_text(document_->manifest().id);
    package_version_entry_.set_text(document_->manifest().version);
    module_entry_.set_text(document_->manifest().entry_point);
    width_spin_.set_value(document_->board().width);
    height_spin_.set_value(document_->board().height);
    entity_x_spin_.set_range(0.0, static_cast<double>(document_->board().width - 1U));
    entity_y_spin_.set_range(0.0, static_cast<double>(document_->board().height - 1U));
}

void StudioWindow::refresh_preview() {
    if (!document_) {
        return;
    }
    auto preview = document_->preview_snapshot(++preview_revision_);
    if (!preview) {
        show_diagnostic(preview.error());
        return;
    }
    snapshot_ = std::make_shared<const RenderSnapshot>(std::move(*preview));
    board_.set_snapshot(snapshot_);
    board_.set_interaction({}, {});
    status_.set_text(snapshot_->status);
}

void StudioWindow::generate_board() {
    if (!document_) {
        show_message("No package is open", true);
        return;
    }
    const auto width = static_cast<std::uint32_t>(width_spin_.get_value_as_int());
    const auto height = static_cast<std::uint32_t>(height_spin_.get_value_as_int());
    if (auto generated = document_->regenerate_board(width, height); !generated) {
        show_diagnostic(generated.error());
        return;
    }
    selected_entity_name_.reset();
    select_entity(nullptr);
    refresh_package_fields();
    refresh_preview();
    show_message("Regenerated bounded rectangular topology");
}

void StudioWindow::reset_chess_setup() {
    if (!document_) {
        show_message("No package is open", true);
        return;
    }
    if (auto reset = document_->reset_chess_setup(); !reset) {
        show_diagnostic(reset.error());
        return;
    }
    refresh_preview();
    show_message("Restored the 32-piece chess-like template");
}

void StudioWindow::import_asset() {
    if (!document_) {
        show_message("No package is open", true);
        return;
    }
    const auto imported = document_->import_png(
        std::filesystem::path{asset_path_entry_.get_text().raw()},
        asset_key_entry_.get_text().raw());
    if (!imported) {
        show_diagnostic(imported.error());
        return;
    }
    refresh_visual_theme();
    refresh_preview();
    show_message("Imported " + imported->generic_string() +
                 " and updated the package theme");
}

void StudioWindow::refresh_visual_theme() {
    std::vector<Glib::ustring> names;
    if (document_) {
        auto theme = document_->visual_theme();
        if (theme) {
            names.reserve(theme->sprites().size());
            for (const auto& sprite : theme->sprites()) {
                names.emplace_back(sprite.id);
            }
            board_.set_font_families(std::vector<std::string>{
                theme->font_families().begin(), theme->font_families().end()});
            board_.set_texture_atlas(theme->atlas());
            status_.set_tooltip_text(theme->display_name() + " — " +
                                     std::to_string(theme->sprites().size()) + " sprites, " +
                                     std::to_string(theme->atlas().pages().size()) +
                                     " atlas page(s)");
        }
    }
    if (names.empty()) {
        names = {"piece.ivory.pawn", "piece.ivory.knight", "piece.ivory.bishop",
                 "piece.ivory.rook", "piece.ivory.queen", "piece.ivory.king",
                 "piece.iron.pawn", "piece.iron.knight", "piece.iron.bishop",
                 "piece.iron.rook", "piece.iron.queen", "piece.iron.king"};
        if (auto atlas = chess::make_default_chess_atlas(); atlas) {
            board_.set_texture_atlas(std::move(*atlas));
        }
    }
    const auto selection = entity_sprite_picker_.get_selected();
    sprite_model_ = Gtk::StringList::create(names);
    entity_sprite_picker_.set_model(sprite_model_);
    entity_sprite_picker_.set_selected(
        selection < names.size() ? selection : static_cast<guint>(0U));
}

void StudioWindow::upsert_entity() {
    if (!document_) {
        show_message("No package is open", true);
        return;
    }
    const auto selected_sprite = entity_sprite_picker_.get_selected();
    const auto sprite_name = selected_sprite < sprite_model_->get_n_items()
                                 ? sprite_model_->get_string(selected_sprite).raw()
                                 : std::string{"legacy.unknown.sprite_4095"};
    const auto entity_name = entity_name_entry_.get_text().raw();
    studio::BoardEntity entity{
        entity_name,
        entity_type_entry_.get_text().raw(),
        static_cast<std::uint32_t>(entity_owner_spin_.get_value_as_int()),
        static_cast<std::uint32_t>(entity_x_spin_.get_value_as_int()),
        static_cast<std::uint32_t>(entity_y_spin_.get_value_as_int()),
        selected_sprite < 4'096U ? selected_sprite : 4'095U,
        sprite_name,
    };
    const auto replaced = selected_entity_name_.value_or(std::string{});
    if (auto updated = document_->upsert_entity(entity, replaced); !updated) {
        show_diagnostic(updated.error());
        return;
    }
    selected_entity_name_ = entity_name;
    refresh_preview();
    show_message("Updated entity " + entity_name);
}

void StudioWindow::delete_entity() {
    if (!document_ || !selected_entity_name_) {
        show_message("Select an entity on the board first", true);
        return;
    }
    const auto removed = *selected_entity_name_;
    if (!document_->remove_entity(removed)) {
        show_message("Selected entity no longer exists", true);
        return;
    }
    selected_entity_name_.reset();
    select_entity(nullptr);
    refresh_preview();
    show_message("Deleted entity " + removed);
}

void StudioWindow::select_entity(const studio::BoardEntity* entity) {
    if (entity == nullptr) {
        selected_entity_name_.reset();
        entity_name_entry_.set_text("");
        entity_type_entry_.set_text("pawn");
        entity_owner_spin_.set_value(0.0);
        entity_sprite_picker_.set_selected(0U);
        return;
    }
    selected_entity_name_ = entity->name;
    entity_name_entry_.set_text(entity->name);
    entity_type_entry_.set_text(entity->type);
    entity_owner_spin_.set_value(entity->owner);
    entity_x_spin_.set_value(entity->x);
    entity_y_spin_.set_value(entity->y);
    auto selected = std::numeric_limits<guint>::max();
    for (guint index = 0U; index < sprite_model_->get_n_items(); ++index) {
        if (sprite_model_->get_string(index).raw() == entity->sprite_name) {
            selected = index;
            break;
        }
    }
    entity_sprite_picker_.set_selected(selected);
}

void StudioWindow::on_space_activated(SpaceId space) {
    if (!playtesting_) {
        if (!document_) {
            return;
        }
        const auto width = document_->board().width;
        select_entity(document_->entity_at(space.index() % width, space.index() / width));
        return;
    }
    if (!snapshot_) {
        return;
    }
    if (selected_piece_) {
        const auto action =
            std::ranges::find_if(snapshot_->actions, [this, space](const auto& hint) {
                return hint.actor == *selected_piece_ && hint.destination == space;
            });
        if (action != snapshot_->actions.end()) {
            selected_piece_.reset();
            update_playtest_interaction();
            if (!controller_.move(action->token)) {
                report_queue_full();
            }
            return;
        }
    }
    const auto* piece = find_piece_at(*snapshot_, space);
    if (piece != nullptr &&
        std::ranges::any_of(snapshot_->actions,
                            [piece](const auto& hint) { return hint.actor == piece->id; })) {
        selected_piece_ = piece->id;
    } else {
        selected_piece_.reset();
    }
    update_playtest_interaction();
}

void StudioWindow::start_playtest() {
    if (!document_ || !save_package()) {
        return;
    }
    auto position = position_from_document(*document_);
    if (!position) {
        show_diagnostic(position.error());
        return;
    }
    PlaytestConfiguration configuration{document_->root(), document_->manifest().entry_point,
                                        std::move(*position)};
    if (!controller_.launch(std::move(configuration))) {
        report_queue_full();
        return;
    }
    playtesting_ = true;
    selected_piece_.reset();
    reload_button_.set_sensitive(true);
    undo_button_.set_sensitive(true);
    redo_button_.set_sensitive(true);
    restart_button_.set_sensitive(true);
    status_.set_text("Starting isolated playtest…");
}

void StudioWindow::return_to_edit() {
    playtesting_ = false;
    selected_piece_.reset();
    reload_button_.set_sensitive(false);
    undo_button_.set_sensitive(false);
    redo_button_.set_sensitive(false);
    restart_button_.set_sensitive(false);
    if (document_) {
        refresh_preview();
    }
}

void StudioWindow::reload_rules() {
    if (!playtesting_ || !document_) {
        show_message("Start a playtest before reloading rules", true);
        return;
    }
    if (!save_package()) {
        return;
    }
    if (!controller_.reload_rules()) {
        report_queue_full();
    } else {
        status_.set_text("Validating candidate rule set…");
    }
}

void StudioWindow::on_playtest_view() {
    if (!playtesting_) {
        return;
    }
    playtest_view_ = controller_.view();
    if (!playtest_view_) {
        return;
    }
    snapshot_ = std::shared_ptr<const RenderSnapshot>(playtest_view_, &playtest_view_->snapshot);
    board_.set_snapshot(snapshot_);
    diagnostics_view_.get_buffer()->set_text(playtest_view_->diagnostics.empty()
                                                  ? "No playtest diagnostics."
                                                  : playtest_view_->diagnostics);
    events_view_.get_buffer()->set_text(playtest_view_->event_log);
    state_view_.get_buffer()->set_text(playtest_view_->state_inspector);
    status_.set_text(snapshot_->status);
    if (selected_piece_ &&
        std::ranges::none_of(snapshot_->actions,
                             [this](const auto& hint) { return hint.actor == selected_piece_; })) {
        selected_piece_.reset();
    }
    update_playtest_interaction();
}

void StudioWindow::update_playtest_interaction() {
    std::vector<SpaceId> destinations;
    if (playtesting_ && snapshot_ && selected_piece_) {
        for (const auto& action : snapshot_->actions) {
            if (action.actor == *selected_piece_) {
                destinations.push_back(action.destination);
            }
        }
    }
    board_.set_interaction(selected_piece_, std::move(destinations));
}

void StudioWindow::show_diagnostic(const Diagnostic& diagnostic) {
    const auto formatted = studio::format_diagnostic(diagnostic);
    diagnostics_view_.get_buffer()->set_text(formatted);
    output_notebook_.set_current_page(0);
    status_.set_text("Error: " + diagnostic.message);
}

void StudioWindow::show_message(std::string message, bool error) {
    status_.set_text(error ? "Error: " + message : message);
    diagnostics_view_.get_buffer()->set_text(error ? std::move(message)
                                                   : "No diagnostics.\n" + message);
    if (error) {
        output_notebook_.set_current_page(0);
    }
}

void StudioWindow::report_queue_full() {
    show_message("Simulation command queue is full", true);
}

} // namespace ludus::studio_app
