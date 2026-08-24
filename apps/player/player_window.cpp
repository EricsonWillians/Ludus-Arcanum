#include "player_window.hpp"

#include "ludus/chess/chess.hpp"
#include "ludus/chess/presentation.hpp"
#include "ludus/render/theme.hpp"

#include <gdk/gdkkeysyms.h>
#include <gtkmm/cssprovider.h>
#include <gtkmm/stylecontext.h>
#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <cmath>
#include <numbers>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace ludus::player {
namespace {

std::vector<std::string> python_search_paths() {
    return {
        std::string{LUDUS_SOURCE_DIR} + "/python",
        std::string{LUDUS_SOURCE_DIR} + "/games/orthodox_chess/python",
        std::string{LUDUS_SOURCE_DIR} + "/games/tactical_rpg/python",
        std::string{LUDUS_BINARY_DIR} + "/python",
    };
}

} // namespace

PlayerWindow::PlayerWindow(PlayerGame game, RendererPreference renderer,
                           bool print_renderer_info, std::size_t stress_sprites,
                           bool hot_seat)
    : board_(renderer), controller_(game, python_search_paths(), hot_seat), game_(game),
      print_renderer_info_(print_renderer_info), stress_sprites_(stress_sprites) {
    set_title(game == PlayerGame::chess ? "Ludus Arcanum — Orthodox Chess"
                                        : "Ludus Arcanum — Tactical RPG");
    set_default_size(stress_sprites_ == 0U ? (game == PlayerGame::chess ? 1'280 : 920)
                                          : 1'920,
                     stress_sprites_ == 0U ? (game == PlayerGame::chess ? 900 : 860)
                                          : 1'080);
    if (stress_sprites_ != 0U) {
        set_decorated(false);
        fullscreen();
    }
    set_child(root_);

    toolbar_.set_margin_start(10);
    toolbar_.set_margin_end(10);
    toolbar_.set_margin_top(10);
    toolbar_.set_margin_bottom(10);
    toolbar_.append(undo_button_);
    toolbar_.append(redo_button_);
    toolbar_.append(restart_button_);
    toolbar_.append(replay_button_);
    toolbar_.append(fit_button_);
    toolbar_.append(flip_button_);
    toolbar_.append(promotion_label_);
    promotion_model_ = Gtk::StringList::create(
        std::vector<Glib::ustring>{"Queen", "Rook", "Bishop", "Knight"});
    promotion_.set_model(promotion_model_);
    promotion_.set_selected(0U);
    toolbar_.append(promotion_);
    choice_model_ = Gtk::StringList::create(std::vector<Glib::ustring>{});
    choice_.set_model(choice_model_);
    toolbar_.append(choice_label_);
    toolbar_.append(choice_);
    toolbar_.append(choice_button_);
    choice_label_.set_visible(false);
    choice_.set_visible(false);
    choice_button_.set_visible(false);
    promotion_label_.set_visible(false);
    promotion_.set_visible(false);
    status_.set_hexpand(true);
    status_.set_halign(Gtk::Align::END);
    status_.set_text("Starting simulation…");
    toolbar_.append(status_);
    toolbar_.set_visible(stress_sprites_ == 0U);
    content_.set_hexpand(true);
    content_.set_vexpand(true);
    hud_.set_xalign(0.0F);
    hud_.set_yalign(0.0F);
    hud_.set_wrap(true);
    hud_.set_selectable(true);
    hud_.set_margin_start(14);
    hud_.set_margin_end(14);
    hud_.set_margin_top(14);
    hud_.set_margin_bottom(14);
    hud_scroll_.set_child(hud_);
    hud_scroll_.set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    hud_scroll_.set_size_request(286, -1);
    hud_scroll_.set_visible(false);
    load_preferences();
    if (game_ == PlayerGame::chess && stress_sprites_ == 0U) {
        configure_chess_shell();
    } else {
        configure_tactical_shell();
    }
    board_.set_focusable(true);
    const auto keys = Gtk::EventControllerKey::create();
    keys->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
    keys->signal_key_pressed().connect(
        [this](guint keyval, guint, Gdk::ModifierType modifiers) {
            return on_key_pressed(keyval, modifiers);
        },
        false);
    add_controller(keys);
    promotion_dialog_.set_title("Promotion — Ludus Arcanum");
    promotion_dialog_.set_transient_for(*this);
    promotion_dialog_.set_modal(true);
    promotion_dialog_.set_hide_on_close(true);
    promotion_dialog_.set_default_size(420, 120);
    promotion_dialog_root_.set_margin_start(18);
    promotion_dialog_root_.set_margin_end(18);
    promotion_dialog_root_.set_margin_top(18);
    promotion_dialog_root_.set_margin_bottom(18);
    promotion_dialog_root_.append(promotion_dialog_label_);
    promotion_dialog_buttons_.append(promote_queen_);
    promotion_dialog_buttons_.append(promote_rook_);
    promotion_dialog_buttons_.append(promote_bishop_);
    promotion_dialog_buttons_.append(promote_knight_);
    promotion_dialog_root_.append(promotion_dialog_buttons_);
    if (game_ == PlayerGame::chess && stress_sprites_ == 0U) {
        promotion_popover_.set_child(promotion_dialog_root_);
        promotion_popover_.set_parent(board_);
        promotion_popover_.set_autohide(false);
    } else {
        promotion_dialog_.set_child(promotion_dialog_root_);
    }
    promote_queen_.signal_clicked().connect([this] {
        choose_promotion(static_cast<std::uint32_t>(chess::PieceType::queen));
    });
    promote_rook_.signal_clicked().connect([this] {
        choose_promotion(static_cast<std::uint32_t>(chess::PieceType::rook));
    });
    promote_bishop_.signal_clicked().connect([this] {
        choose_promotion(static_cast<std::uint32_t>(chess::PieceType::bishop));
    });
    promote_knight_.signal_clicked().connect([this] {
        choose_promotion(static_cast<std::uint32_t>(chess::PieceType::knight));
    });

    const auto package_root = std::filesystem::path{LUDUS_SOURCE_DIR} / "games" /
                              (game == PlayerGame::chess ? "orthodox_chess"
                                                         : "tactical_rpg");
    auto visual_theme = VisualTheme::load_package(package_root);
    if (visual_theme) {
        board_.set_font_families(std::vector<std::string>{
            visual_theme->font_families().begin(), visual_theme->font_families().end()});
        board_.set_texture_atlas(visual_theme->atlas());
        if (game == PlayerGame::chess) {
            board_material_sprite_ = visual_theme->sprite("board.material.dark");
            frame_corner_sprite_ = visual_theme->sprite("decoration.frame.corner");
        }
    } else if (game == PlayerGame::chess) {
        auto atlas = chess::make_default_chess_atlas();
        if (const auto* atlas_path = std::getenv("LUDUS_CHESS_ATLAS");
            atlas_path != nullptr && *atlas_path != '\0') {
            auto loaded = chess::load_chess_atlas(std::filesystem::path{atlas_path});
            if (loaded) {
                atlas = std::move(loaded);
            } else {
                status_.set_text("Custom atlas failed; using built-in pieces");
            }
        }
        if (atlas) {
            board_.set_texture_atlas(std::move(*atlas));
        }
    } else {
        status_.set_text("Package visuals failed: " + visual_theme.error().message);
    }

    board_.signal_space_activated().connect(
        sigc::mem_fun(*this, &PlayerWindow::on_space_activated));
    board_.signal_space_dropped().connect(
        sigc::mem_fun(*this, &PlayerWindow::on_space_dropped));
    board_.signal_render_error().connect([this](const std::string& message) {
        status_.set_text(message);
    });
    board_.signal_renderer_changed().connect([this](const RendererInfo& info) {
        if (print_renderer_info_) {
            std::cout << "ludus-player renderer: " << info.summary() << std::endl;
        }
        status_.set_tooltip_text(info.summary());
    });
    board_.signal_stress_complete().connect([this] {
        if (stress_sprites_ != 0U) {
            Glib::signal_idle().connect_once([this] { close(); });
        }
    });
    if (board_.renderer_info().backend == RendererBackend::software) {
        status_.set_tooltip_text(board_.renderer_info().summary());
        if (print_renderer_info_) {
            std::cout << "ludus-player renderer: " << board_.renderer_info().summary()
                      << std::endl;
        }
    }
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
        clear_selection();
        if (!controller_.restart()) {
            report_queue_full();
        }
    });
    replay_button_.signal_clicked().connect([this] {
        clear_selection();
        if (!controller_.replay()) {
            report_queue_full();
        }
    });
    fit_button_.signal_clicked().connect([this] { board_.reset_camera(); });
    flip_button_.signal_clicked().connect([this] { board_.flip_board(); });
    choice_button_.signal_clicked().connect(
        sigc::mem_fun(*this, &PlayerWindow::submit_choice));
    new_button_.signal_clicked().connect(sigc::mem_fun(*this, &PlayerWindow::present_new_match));
    open_button_.signal_clicked().connect([this] { choose_file(false, false); });
    save_button_.signal_clicked().connect([this] { choose_file(true, false); });
    overflow_replay_.signal_clicked().connect([this] {
        overflow_popover_.popdown();
        clear_selection();
        if (!controller_.replay()) {
            report_queue_full();
        }
    });
    overflow_open_.signal_clicked().connect([this] { choose_file(false, false); });
    overflow_save_.signal_clicked().connect([this] { choose_file(true, false); });
    overflow_undo_.signal_clicked().connect([this] {
        if (!controller_.undo()) {
            report_queue_full();
        }
    });
    overflow_redo_.signal_clicked().connect([this] {
        if (!controller_.redo()) {
            report_queue_full();
        }
    });
    overflow_flip_.signal_clicked().connect([this] {
        board_.flip_board();
        board_flipped_ = !board_flipped_;
        save_preferences();
        update_chess_hud();
    });
    overflow_fit_.signal_clicked().connect([this] { board_.reset_camera(); });
    overflow_import_.signal_clicked().connect([this] { choose_file(false, true); });
    overflow_export_.signal_clicked().connect([this] { choose_file(true, true); });
    const auto copy_export = [this](std::string_view format) {
        if (!view_) {
            return;
        }
        const auto found = std::ranges::find(view_->text_exports, format,
                                             &TextExportView::format);
        const auto display = Gdk::Display::get_default();
        if (found != view_->text_exports.end() && display) {
            display->get_clipboard()->set_text(found->text);
            status_.set_text(std::string{format} + " copied to clipboard");
        }
    };
    overflow_copy_fen_.signal_clicked().connect([copy_export] { copy_export("FEN"); });
    overflow_copy_pgn_.signal_clicked().connect([copy_export] { copy_export("PGN"); });
    overflow_paste_.signal_clicked().connect([this] {
        const auto display = Gdk::Display::get_default();
        if (!display) {
            return;
        }
        const auto clipboard = display->get_clipboard();
        clipboard->read_text_async([this, clipboard](const Glib::RefPtr<Gio::AsyncResult>& result) {
            try {
                const auto text = clipboard->read_text_finish(result);
                if (text.empty()) {
                    status_.set_text("Clipboard does not contain text");
                } else if (text.find('[') != Glib::ustring::npos ||
                           text.find("1.") != Glib::ustring::npos) {
                    if (!controller_.import_pgn_text(text.raw())) {
                        report_queue_full();
                    }
                } else {
                    fen_entry_.set_text(text);
                    present_new_match();
                }
            } catch (const Glib::Error& error) {
                status_.set_text(std::string{"Clipboard import failed: "} + error.what());
            }
        });
    });
    overflow_resign_.signal_clicked().connect(sigc::mem_fun(*this, &PlayerWindow::confirm_resign));
    overflow_draw_.signal_clicked().connect(sigc::mem_fun(*this, &PlayerWindow::confirm_draw));
    rematch_button_.signal_clicked().connect(sigc::mem_fun(*this, &PlayerWindow::present_new_match));
    result_export_button_.signal_clicked().connect([this] { choose_file(true, true); });
    return_live_button_.signal_clicked().connect([this] {
        if (!controller_.preview_history(std::nullopt)) {
            report_queue_full();
        }
        if (compact_layout_) {
            sidebar_visible_ = false;
            chess_sidebar_.set_visible(false);
            match_column_.set_visible(true);
        }
    });
    sidebar_toggle_.signal_clicked().connect([this] {
        sidebar_visible_ = !sidebar_visible_;
        chess_sidebar_.set_visible(sidebar_visible_);
        if (compact_layout_) {
            match_column_.set_visible(!sidebar_visible_);
        }
    });
    high_contrast_.signal_toggled().connect([this] {
        high_contrast_enabled_ = high_contrast_.get_active();
        apply_preferences();
        save_preferences();
        on_snapshot();
    });
    reduced_motion_.signal_toggled().connect([this] {
        reduced_motion_enabled_ = reduced_motion_.get_active();
        save_preferences();
        on_snapshot();
    });
    ui_scale_control_.signal_value_changed().connect([this] {
        ui_scale_ = ui_scale_control_.get_value();
        apply_preferences();
        save_preferences();
    });

    add_tick_callback([this](const Glib::RefPtr<Gdk::FrameClock>&) {
        if (game_ == PlayerGame::chess && stress_sprites_ == 0U) {
            update_responsive_layout();
            update_clock_display();
        }
        return true;
    });

    dispatcher_connection_ =
        controller_.dispatcher().connect(sigc::mem_fun(*this, &PlayerWindow::on_snapshot));
    controller_.start();
    if (game_ == PlayerGame::chess && stress_sprites_ == 0U &&
        std::filesystem::exists(PlayerController::autosave_path())) {
        Glib::signal_idle().connect_once([this] {
            confirmation_ = std::make_unique<Gtk::MessageDialog>(
                *this, "Restore the previous chess match?", false,
                Gtk::MessageType::QUESTION, Gtk::ButtonsType::YES_NO, true);
            confirmation_->set_secondary_text(
                "A crash-safe autosave was found. It will replace the new empty match only after validation.");
            confirmation_->signal_response().connect([this](int response) {
                if (response == static_cast<int>(Gtk::ResponseType::YES)) {
                    if (!controller_.open_match(PlayerController::autosave_path())) {
                        report_queue_full();
                    }
                } else {
                    std::error_code ignored;
                    std::filesystem::remove(PlayerController::autosave_path(), ignored);
                }
                confirmation_->hide();
                Glib::signal_idle().connect_once([this] { confirmation_.reset(); });
            });
            confirmation_->present();
        });
    }
}

void PlayerWindow::configure_tactical_shell() {
    root_.append(toolbar_);
    content_.append(board_);
    content_.append(hud_scroll_);
    root_.append(content_);
}

void PlayerWindow::configure_chess_shell() {
    toolbar_.remove(undo_button_);
    toolbar_.remove(redo_button_);
    toolbar_.remove(status_);

    header_title_.add_css_class("reliquary-title");
    header_.set_title_widget(header_title_);
    header_.pack_start(new_button_);
    header_.pack_start(open_button_);
    header_.pack_start(save_button_);
    header_.pack_end(overflow_button_);
    header_.pack_end(redo_button_);
    header_.pack_end(undo_button_);
    header_.pack_end(sidebar_toggle_);
    overflow_button_.set_label("More");
    overflow_box_.set_margin_start(8);
    overflow_box_.set_margin_end(8);
    overflow_box_.set_margin_top(8);
    overflow_box_.set_margin_bottom(8);
    ui_scale_adjustment_ = Gtk::Adjustment::create(ui_scale_, 0.8, 1.6, 0.1, 0.1);
    ui_scale_control_.set_adjustment(ui_scale_adjustment_);
    ui_scale_control_.set_digits(1);
    ui_scale_control_.set_tooltip_text("Interface scale");
    for (auto* widget : std::array<Gtk::Widget*, 17U>{
             &overflow_open_, &overflow_save_, &overflow_undo_, &overflow_redo_,
             &overflow_replay_, &overflow_flip_, &overflow_fit_, &overflow_import_,
             &overflow_export_, &overflow_copy_fen_, &overflow_copy_pgn_, &overflow_paste_,
             &overflow_resign_, &overflow_draw_, &high_contrast_, &reduced_motion_,
             &ui_scale_control_}) {
        overflow_box_.append(*widget);
    }
    overflow_popover_.set_child(overflow_box_);
    overflow_button_.set_popover(overflow_popover_);
    root_.append(header_);

    const auto configure_card = [](Gtk::Box& card, Gtk::Picture& crest,
                                   Gtk::Box& text, Gtk::Label& name,
                                   Gtk::Label& detail, Gtk::Label& captures,
                                   Gtk::Label& clock) {
        card.add_css_class("player-card");
        card.set_margin_start(12);
        card.set_margin_end(12);
        card.set_margin_top(5);
        card.set_margin_bottom(5);
        crest.set_size_request(48, 48);
        crest.set_can_shrink(true);
        name.set_xalign(0.0F);
        name.add_css_class("player-name");
        detail.set_xalign(0.0F);
        detail.add_css_class("player-detail");
        captures.set_xalign(0.0F);
        captures.set_ellipsize(Pango::EllipsizeMode::END);
        text.append(name);
        text.append(detail);
        text.append(captures);
        text.set_hexpand(true);
        clock.add_css_class("chess-clock");
        clock.set_valign(Gtk::Align::CENTER);
        card.append(crest);
        card.append(text);
        card.append(clock);
    };
    configure_card(top_player_card_, top_crest_, top_player_text_, top_player_name_,
                   top_player_detail_, top_captures_, top_clock_);
    configure_card(bottom_player_card_, bottom_crest_, bottom_player_text_,
                   bottom_player_name_, bottom_player_detail_, bottom_captures_,
                   bottom_clock_);
    const auto asset_root = std::filesystem::path{LUDUS_SOURCE_DIR} /
                            "games/orthodox_chess/assets/ui";
    top_crest_.set_filename((asset_root / "iron-crest.png").string());
    bottom_crest_.set_filename((asset_root / "ivory-crest.png").string());
    const auto promotion_button = [](Gtk::Button& button,
                                     const std::filesystem::path& image_path,
                                     std::string_view label) {
        auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
        auto* picture = Gtk::make_managed<Gtk::Picture>();
        auto* text = Gtk::make_managed<Gtk::Label>(std::string{label});
        picture->set_filename(image_path.string());
        picture->set_size_request(54, 68);
        picture->set_can_shrink(true);
        box->append(*picture);
        box->append(*text);
        button.set_child(*box);
    };
    const auto ivory_pieces = std::filesystem::path{LUDUS_SOURCE_DIR} /
                              "games/orthodox_chess/assets/pieces/ivory";
    promotion_button(promote_queen_, ivory_pieces / "queen.png", "Queen");
    promotion_button(promote_rook_, ivory_pieces / "rook.png", "Rook");
    promotion_button(promote_bishop_, ivory_pieces / "bishop.png", "Bishop");
    promotion_button(promote_knight_, ivory_pieces / "knight.png", "Knight");
    gtk_accessible_update_property(GTK_ACCESSIBLE(board_.gobj()),
                                   GTK_ACCESSIBLE_PROPERTY_LABEL,
                                   "Chessboard. Use arrow keys to move focus and Enter to select.",
                                   -1);
    gtk_accessible_update_property(GTK_ACCESSIBLE(moves_list_.gobj()),
                                   GTK_ACCESSIBLE_PROPERTY_LABEL,
                                   "Mainline move history", -1);

    // Presentation-only analysis marks. They deliberately live in the window rather
    // than the match/controller, so they cannot enter archives, PGN, hashes, or replay.
    auto annotation_drag = Gtk::GestureDrag::create();
    annotation_drag->set_button(GDK_BUTTON_SECONDARY);
    annotation_drag->signal_drag_begin().connect([this](double x, double y) {
        if (!snapshot_ || !view_ || view_->match_controls.can_return_to_live) {
            annotation_start_.reset();
            return;
        }
        annotation_pointer_start_ = {static_cast<float>(x), static_cast<float>(y)};
        annotation_start_ = pick_space(
            *snapshot_, board_.camera().screen_to_world(
                            {static_cast<float>(x), static_cast<float>(y)}));
    });
    annotation_drag->signal_drag_end().connect([this](double offset_x, double offset_y) {
        if (!annotation_start_ || !snapshot_) {
            return;
        }
        const auto origin = *annotation_start_;
        const auto origin_space = std::ranges::find_if(
            snapshot_->spaces, [origin](const SpaceVisual& space) { return space.id == origin; });
        if (origin_space == snapshot_->spaces.end()) {
            annotation_start_.reset();
            return;
        }
        const auto destination = pick_space(
            *snapshot_, board_.camera().screen_to_world(
                            {annotation_pointer_start_.x + static_cast<float>(offset_x),
                             annotation_pointer_start_.y + static_cast<float>(offset_y)}));
        annotation_start_.reset();
        if (!destination) {
            return;
        }
        if (*destination == origin) {
            const auto existing = std::ranges::find(board_markers_, origin);
            if (existing == board_markers_.end()) {
                board_markers_.push_back(origin);
            } else {
                board_markers_.erase(existing);
            }
        } else {
            const auto arrow = std::pair{origin, *destination};
            const auto existing = std::ranges::find(board_arrows_, arrow);
            if (existing == board_arrows_.end()) {
                board_arrows_.push_back(arrow);
            } else {
                board_arrows_.erase(existing);
            }
        }
        ++annotation_revision_;
        on_snapshot();
    });
    board_.add_controller(annotation_drag);

    board_.set_size_request(500, 500);
    board_overlay_.set_child(board_);
    result_panel_.add_css_class("result-panel");
    result_panel_.set_halign(Gtk::Align::CENTER);
    result_panel_.set_valign(Gtk::Align::CENTER);
    result_panel_.set_margin_start(32);
    result_panel_.set_margin_end(32);
    result_title_.add_css_class("result-title");
    result_detail_.set_wrap(true);
    result_detail_.set_justify(Gtk::Justification::CENTER);
    result_actions_.set_halign(Gtk::Align::CENTER);
    result_actions_.append(rematch_button_);
    result_actions_.append(result_export_button_);
    result_panel_.append(result_title_);
    result_panel_.append(result_detail_);
    result_panel_.append(result_actions_);
    result_panel_.set_visible(false);
    board_overlay_.add_overlay(result_panel_);
    match_column_.set_hexpand(true);
    match_column_.set_vexpand(true);
    match_column_.append(top_player_card_);
    match_column_.append(board_overlay_);
    match_column_.append(bottom_player_card_);

    sidebar_switcher_.set_stack(sidebar_stack_);
    sidebar_switcher_.set_halign(Gtk::Align::CENTER);
    moves_list_.set_selection_mode(Gtk::SelectionMode::SINGLE);
    moves_list_.set_activate_on_single_click(true);
    moves_list_.signal_row_activated().connect([this](Gtk::ListBoxRow* row) {
        if (row == nullptr || !view_) {
            return;
        }
        const auto row_index = static_cast<std::size_t>(row->get_index());
        const auto candidate = std::min((row_index + 1U) * 2U, view_->timeline.size());
        if (!controller_.preview_history(candidate)) {
            report_queue_full();
        }
        if (compact_layout_) {
            sidebar_visible_ = false;
            chess_sidebar_.set_visible(false);
            match_column_.set_visible(true);
        }
    });
    moves_scroll_.set_child(moves_list_);
    moves_scroll_.set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    moves_scroll_.set_vexpand(true);
    moves_page_.append(moves_scroll_);
    moves_page_.append(return_live_button_);
    game_summary_.set_xalign(0.0F);
    game_summary_.set_yalign(0.0F);
    game_summary_.set_wrap(true);
    game_page_.append(game_summary_);
    game_page_.append(claim_box_);
    game_page_.append(status_);
    sidebar_stack_.add(moves_page_, "moves", "Moves");
    sidebar_stack_.add(game_page_, "game", "Game");
    chess_sidebar_.add_css_class("match-sidebar");
    chess_sidebar_.set_hexpand(false);
    chess_sidebar_.set_size_request(320, -1);
    chess_sidebar_.set_margin_start(8);
    chess_sidebar_.set_margin_end(12);
    chess_sidebar_.set_margin_top(10);
    chess_sidebar_.set_margin_bottom(10);
    chess_sidebar_.append(sidebar_switcher_);
    chess_sidebar_.append(sidebar_stack_);
    content_.append(match_column_);
    content_.append(chess_sidebar_);
    root_.append(content_);

    new_match_dialog_.set_title("New Match — The Ivory Reliquary");
    new_match_dialog_.set_transient_for(*this);
    new_match_dialog_.set_modal(true);
    new_match_dialog_.set_hide_on_close(true);
    new_match_dialog_.set_default_size(480, 520);
    new_match_root_.set_margin_start(20);
    new_match_root_.set_margin_end(20);
    new_match_root_.set_margin_top(20);
    new_match_root_.set_margin_bottom(20);
    const auto field = [this](std::string_view label, Gtk::Widget& widget) {
        auto* title = Gtk::make_managed<Gtk::Label>(std::string{label});
        title->set_xalign(0.0F);
        title->add_css_class("field-title");
        new_match_root_.append(*title);
        new_match_root_.append(widget);
    };
    white_name_entry_.set_text("Ivory");
    black_name_entry_.set_text("Iron");
    fen_entry_.set_placeholder_text("Standard position, or paste a FEN");
    field("White player", white_name_entry_);
    field("Black player", black_name_entry_);
    field("Starting position", fen_entry_);
    time_control_model_ = Gtk::StringList::create(std::vector<Glib::ustring>{
        "Unclocked", "1 + 0", "3 + 2", "5 + 0", "10 + 0", "15 + 10",
        "30 + 0", "Custom"});
    time_control_.set_model(time_control_model_);
    time_control_.set_selected(remembered_time_control_);
    field("Time control", time_control_);
    base_adjustment_ = Gtk::Adjustment::create(10.0, 1.0, 1'440.0, 1.0, 10.0);
    increment_adjustment_ = Gtk::Adjustment::create(0.0, 0.0, 3'600.0, 1.0, 10.0);
    base_minutes_.set_adjustment(base_adjustment_);
    increment_seconds_.set_adjustment(increment_adjustment_);
    field("Custom base minutes", base_minutes_);
    field("Custom increment seconds", increment_seconds_);
    orientation_model_ = Gtk::StringList::create(
        std::vector<Glib::ustring>{"White at bottom", "Black at bottom"});
    orientation_.set_model(orientation_model_);
    orientation_.set_selected(board_flipped_ ? 1U : 0U);
    field("Orientation", orientation_);
    new_match_error_.add_css_class("error");
    new_match_error_.set_wrap(true);
    new_match_root_.append(new_match_error_);
    new_match_actions_.set_halign(Gtk::Align::END);
    new_match_actions_.append(new_match_cancel_);
    new_match_actions_.append(new_match_start_);
    new_match_root_.append(new_match_actions_);
    new_match_dialog_.set_child(new_match_root_);
    new_match_cancel_.signal_clicked().connect([this] { new_match_dialog_.hide(); });
    new_match_start_.signal_clicked().connect(sigc::mem_fun(*this, &PlayerWindow::start_new_match));

    high_contrast_.set_active(high_contrast_enabled_);
    reduced_motion_.set_active(reduced_motion_enabled_);
    if (board_flipped_) {
        board_.flip_board();
    }
    apply_preferences();
}

PlayerWindow::~PlayerWindow() {
    controller_.stop();
    dispatcher_connection_.disconnect();
    if (promotion_popover_.get_parent() != nullptr) {
        promotion_popover_.unparent();
    }
}

void PlayerWindow::on_snapshot() {
    view_ = controller_.view();
    if (!view_) {
        return;
    }
    if (game_ == PlayerGame::chess) {
        std::vector<std::string> timeline;
        timeline.reserve(view_->timeline.size());
        std::ranges::transform(view_->timeline, std::back_inserter(timeline),
                               [](const TimelineEntryView& entry) { return entry.notation; });
        if (annotation_timeline_initialized_ && timeline != annotation_timeline_) {
            if (!board_arrows_.empty() || !board_markers_.empty()) {
                ++annotation_revision_;
            }
            board_arrows_.clear();
            board_markers_.clear();
        }
        annotation_timeline_ = std::move(timeline);
        annotation_timeline_initialized_ = true;
    }
    if (game_ == PlayerGame::chess &&
        (board_material_sprite_ || frame_corner_sprite_ || reduced_motion_enabled_ ||
         high_contrast_enabled_ || !board_arrows_.empty() || !board_markers_.empty())) {
        auto decorated = std::make_shared<RenderSnapshot>(view_->render);
        decorated->static_revision ^= annotation_revision_ + 0x9e3779b97f4a7c15ULL +
                                      (decorated->static_revision << 6U) +
                                      (decorated->static_revision >> 2U);
        if (reduced_motion_enabled_) {
            decorated->animations.clear();
            decorated->effects.clear();
        }
        if (high_contrast_enabled_) {
            for (auto& space : decorated->spaces) {
                const auto luminance = space.color.red * 0.2126F +
                                       space.color.green * 0.7152F +
                                       space.color.blue * 0.0722F;
                space.color = luminance > 0.25F
                                  ? Color{0.72F, 0.65F, 0.50F, 1.0F}
                                  : Color{0.055F, 0.080F, 0.13F, 1.0F};
            }
        }
        if (board_material_sprite_) {
            decorated->decorations.reserve(decorated->decorations.size() + 64U);
            for (const auto& space : decorated->spaces) {
                decorated->decorations.push_back(DecorationSprite{
                    *board_material_sprite_, space.bounds.center(),
                    {space.bounds.width(), space.bounds.height()},
                    {1.0F, 1.0F, 1.0F,
                     high_contrast_enabled_ ? 0.035F : 0.14F}, 0.1F});
            }
        }
        if (frame_corner_sprite_) {
            constexpr float distance = 4.27F;
            constexpr float size = 1.18F;
            for (const auto& [center, rotation] : std::array{
                     std::pair{Vec2{-distance, -distance}, 0.0F},
                     std::pair{Vec2{distance, -distance}, std::numbers::pi_v<float> * 0.5F},
                     std::pair{Vec2{distance, distance}, std::numbers::pi_v<float>},
                     std::pair{Vec2{-distance, distance}, std::numbers::pi_v<float> * 1.5F}}) {
                decorated->decorations.push_back(DecorationSprite{
                    *frame_corner_sprite_, center, {size, size},
                    {1.0F, 1.0F, 1.0F, 0.92F}, 0.2F, rotation});
            }
        }
        const auto space_center = [&decorated](SpaceId id) -> std::optional<Vec2> {
            const auto found = std::ranges::find_if(
                decorated->spaces, [id](const SpaceVisual& space) { return space.id == id; });
            if (found == decorated->spaces.end()) {
                return std::nullopt;
            }
            return found->bounds.center();
        };
        for (const auto& [from, to] : board_arrows_) {
            const auto from_center = space_center(from);
            const auto to_center = space_center(to);
            if (from_center && to_center) {
                const auto delta = *to_center - *from_center;
                const auto length = std::max(std::hypot(delta.x, delta.y), 0.001F);
                const auto direction = delta * (1.0F / length);
                const Vec2 perpendicular{-direction.y, direction.x};
                const auto arrow_base = *to_center - direction * 0.34F;
                const auto left = arrow_base + perpendicular * 0.18F;
                const auto right = arrow_base - perpendicular * 0.18F;
                decorated->links.push_back(LinkVisual{
                    from, to, *from_center, *to_center,
                    {0.92F, 0.66F, 0.18F, 0.76F}, 0.13F});
                decorated->links.push_back(LinkVisual{
                    to, to, left, *to_center,
                    {0.92F, 0.66F, 0.18F, 0.76F}, 0.13F});
                decorated->links.push_back(LinkVisual{
                    to, to, right, *to_center,
                    {0.92F, 0.66F, 0.18F, 0.76F}, 0.13F});
            }
        }
        for (const auto space : board_markers_) {
            if (const auto center = space_center(space)) {
                decorated->shapes.push_back(ShapeVisual{
                    {{center->x - 0.42F, center->y - 0.42F},
                     {center->x + 0.42F, center->y + 0.42F}},
                    {0.76F, 0.16F, 0.12F, 0.16F}, SpaceShape::rounded_rectangle,
                    {0.92F, 0.34F, 0.20F, 0.82F}, 0.08F, 0.0F, 8.5F});
            }
        }
        snapshot_ = std::move(decorated);
    } else {
        snapshot_ = std::shared_ptr<const RenderSnapshot>{view_, &view_->render};
    }
    if (stress_sprites_ != 0U && !stress_snapshot_installed_) {
        RenderSnapshot stress;
        stress.revision = 1U;
        stress.static_revision = 1U;
        stress.dynamic_revision = 1U;
        stress.world_bounds = {{-8.8889F, -5.0F}, {8.8889F, 5.0F}};
        stress.spaces.push_back(SpaceVisual{SpaceId{0U, 1U}, stress.world_bounds,
                                            {0.018F, 0.022F, 0.032F, 1.0F}});
        const auto columns = static_cast<std::size_t>(
            std::ceil(std::sqrt(static_cast<double>(stress_sprites_) * 16.0 / 9.0)));
        const auto rows = (stress_sprites_ + columns - 1U) / columns;
        stress.pieces.reserve(stress_sprites_);
        stress.animations.reserve(stress_sprites_);
        const auto start = std::chrono::steady_clock::now();
        for (std::size_t index = 0U; index < stress_sprites_; ++index) {
            const auto column = index % columns;
            const auto row = index / columns;
            const auto x = -8.75F + 17.5F * (static_cast<float>(column) + 0.5F) /
                                         static_cast<float>(columns);
            const auto y = -4.86F + 9.72F * (static_cast<float>(row) + 0.5F) /
                                        static_cast<float>(rows);
            const auto entity = EntityId{static_cast<std::uint32_t>(index), 1U};
            const Vec2 center{x, y};
            stress.pieces.push_back(PieceVisual{
                entity, SpaceId{0U, 1U}, center, {0.13F, 0.18F},
                SpriteId{static_cast<std::uint32_t>(index % 12U)},
                {1.0F, 1.0F, 1.0F, 0.92F}, static_cast<float>(row)});
            const auto phase = std::chrono::milliseconds{
                static_cast<std::chrono::milliseconds::rep>(index % 1'000U)};
            stress.animations.push_back(PieceAnimation{
                entity, {x - 0.035F, y}, {x + 0.035F, y}, start - phase,
                std::chrono::milliseconds{1'000}, true});
        }
        stress.status = "Physical GPU stress — " + std::to_string(stress_sprites_) +
                        " animated sprites — 120 warm-up + 600 captured frames";
        snapshot_ = std::make_shared<const RenderSnapshot>(std::move(stress));
        stress_snapshot_installed_ = true;
    }
    board_.set_snapshot(snapshot_);
    status_.set_text(snapshot_->status);
    clock_view_received_ = std::chrono::steady_clock::now();
    if (game_ == PlayerGame::chess && stress_sprites_ == 0U) {
        update_chess_hud();
    } else if (stress_sprites_ == 0U) {
        std::ostringstream text;
        if (view_->objective) {
            text << "SHATTERED SHRINE\n"
                 << view_->objective->first_label << ' ' << view_->objective->first
                 << "  —  " << view_->objective->second_label << ' '
                 << view_->objective->second << "  (first to "
                 << view_->objective->target << ")\n\n";
        }
        if (!view_->initiative.empty()) {
            text << "INITIATIVE\n";
            for (const auto& entry : view_->initiative) {
                text << (entry.active ? "▶ " : "  ") << entry.label << "  "
                     << entry.initiative << (entry.defeated ? "  defeated" : "") << '\n';
            }
            text << '\n';
        }
        if (!view_->units.empty()) {
            text << "UNITS\n";
            for (const auto& unit : view_->units) {
                text << unit.name << " · " << unit.role << "\n  " << unit.health << '/'
                     << unit.maximum_health << " HP · " << unit.action_points << " AP";
                for (const auto& status : unit.statuses) {
                    text << " · " << status;
                }
                text << '\n';
            }
            text << '\n';
        }
        if (!view_->abilities.empty()) {
            text << "AVAILABLE ACTIONS\n";
            std::vector<std::string> labels;
            for (const auto& ability : view_->abilities) {
                if (std::ranges::find(labels, ability.label) != labels.end()) {
                    continue;
                }
                labels.push_back(ability.label);
                text << ability.label << " · " << ability.action_point_cost << " AP\n  "
                     << ability.description << '\n';
            }
            text << '\n';
        }
        if (!view_->combat_log.empty()) {
            text << "COMBAT LOG\n";
            for (const auto& entry : view_->combat_log) {
                text << entry.sequence << ". " << entry.text << '\n';
            }
        }
        if (view_->end_state) {
            text << "\n" << view_->end_state->title << "\n"
                 << view_->end_state->detail << '\n';
        }
        const auto hud_text = text.str();
        hud_.set_text(hud_text);
        hud_scroll_.set_visible(!hud_text.empty());
    }
    std::vector<Glib::ustring> choice_labels;
    choice_labels.reserve(snapshot_->choices.size());
    for (const auto& choice : snapshot_->choices) {
        choice_labels.emplace_back(choice.label);
    }
    choice_model_ = Gtk::StringList::create(choice_labels);
    choice_.set_model(choice_model_);
    if (!choice_labels.empty()) {
        choice_.set_selected(0U);
    }
    const bool choosing = !choice_labels.empty();
    choice_label_.set_visible(choosing);
    choice_.set_visible(choosing);
    choice_button_.set_visible(choosing);
    if (selected_ &&
        std::ranges::none_of(snapshot_->actions, [this](const ActionHint& hint) {
            return hint.actor == selected_;
        })) {
        selected_.reset();
    }
    if (keyboard_focus_ &&
        std::ranges::none_of(snapshot_->spaces, [this](const SpaceVisual& space) {
            return space.id == *keyboard_focus_;
        })) {
        keyboard_focus_.reset();
        board_.set_focused_space(std::nullopt);
    }
    update_interaction();
}

void PlayerWindow::on_space_activated(SpaceId space) {
    if (!snapshot_) {
        return;
    }
    if (selected_) {
        std::vector<const ActionHint*> candidates;
        for (const auto& hint : snapshot_->actions) {
            if (hint.actor == *selected_ && hint.destination == space) {
                candidates.push_back(&hint);
            }
        }
        if (!candidates.empty()) {
            const bool promotion = candidates.size() > 1U &&
                                   std::ranges::all_of(candidates, [](const auto* hint) {
                                       return hint->variant != 0U;
                                   });
            if (promotion) {
                pending_promotions_.fill(std::nullopt);
                for (const auto* candidate : candidates) {
                    if (candidate->variant < pending_promotions_.size()) {
                        pending_promotions_[candidate->variant] = candidate->token;
                    }
                }
                clear_selection();
                if (game_ == PlayerGame::chess) {
                    const auto visual = std::ranges::find(snapshot_->spaces, space,
                                                          &SpaceVisual::id);
                    if (visual != snapshot_->spaces.end()) {
                        const auto screen = board_.camera().world_to_screen(
                            visual->bounds.center());
                        promotion_popover_.set_pointing_to(Gdk::Rectangle{
                            static_cast<int>(screen.x), static_cast<int>(screen.y), 1, 1});
                    }
                    promotion_popover_.popup();
                } else {
                    promotion_dialog_.present();
                }
                return;
            }
            const auto* action = candidates.front();
            clear_selection();
            if (!controller_.submit(action->token)) {
                report_queue_full();
            }
            return;
        }
    }

    const auto* piece = find_piece_at(*snapshot_, space);
    if (piece != nullptr &&
        std::ranges::any_of(snapshot_->actions, [piece](const ActionHint& hint) {
            return hint.actor == piece->id;
        })) {
        selected_ = piece->id;
    } else {
        selected_.reset();
    }
    update_interaction();
}

void PlayerWindow::on_space_dropped(SpaceId origin, SpaceId destination) {
    if (!snapshot_) {
        return;
    }
    const auto* piece = find_piece_at(*snapshot_, origin);
    if (piece == nullptr) {
        return;
    }
    selected_ = piece->id;
    update_interaction();
    on_space_activated(destination);
}

void PlayerWindow::submit_choice() {
    if (!snapshot_) {
        return;
    }
    const auto selected = choice_.get_selected();
    if (selected >= snapshot_->choices.size()) {
        return;
    }
    clear_selection();
    if (!controller_.submit(snapshot_->choices[selected].token)) {
        report_queue_full();
    }
}

bool PlayerWindow::on_key_pressed(guint keyval, Gdk::ModifierType modifiers) {
    const auto control = (modifiers & Gdk::ModifierType::CONTROL_MASK) !=
                         Gdk::ModifierType{};
    const auto shift = (modifiers & Gdk::ModifierType::SHIFT_MASK) !=
                       Gdk::ModifierType{};
    if (control && (keyval == GDK_KEY_n || keyval == GDK_KEY_N)) {
        present_new_match();
    } else if (control && (keyval == GDK_KEY_o || keyval == GDK_KEY_O)) {
        choose_file(false, false);
    } else if (control && (keyval == GDK_KEY_s || keyval == GDK_KEY_S)) {
        choose_file(true, shift);
    } else if (control && (keyval == GDK_KEY_z || keyval == GDK_KEY_Z)) {
        if (!controller_.undo()) {
            report_queue_full();
        }
    } else if (control && (keyval == GDK_KEY_y || keyval == GDK_KEY_Y)) {
        if (!controller_.redo()) {
            report_queue_full();
        }
    } else if (game_ == PlayerGame::chess && (keyval == GDK_KEY_f || keyval == GDK_KEY_F)) {
        board_.flip_board();
        board_flipped_ = !board_flipped_;
        save_preferences();
        update_chess_hud();
    } else if (game_ == PlayerGame::chess && keyval == GDK_KEY_Home) {
        if (!controller_.preview_history(std::nullopt)) {
            report_queue_full();
        }
    } else if (keyval == GDK_KEY_Left || keyval == GDK_KEY_KP_Left) {
        move_keyboard_focus(-1.0F, 0.0F);
    } else if (keyval == GDK_KEY_Right || keyval == GDK_KEY_KP_Right) {
        move_keyboard_focus(1.0F, 0.0F);
    } else if (keyval == GDK_KEY_Up || keyval == GDK_KEY_KP_Up) {
        move_keyboard_focus(0.0F, -1.0F);
    } else if (keyval == GDK_KEY_Down || keyval == GDK_KEY_KP_Down) {
        move_keyboard_focus(0.0F, 1.0F);
    } else if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter ||
               keyval == GDK_KEY_space) {
        if (keyboard_focus_) {
            on_space_activated(*keyboard_focus_);
        }
    } else if (keyval == GDK_KEY_Escape) {
        clear_selection();
    } else {
        return false;
    }
    return true;
}

void PlayerWindow::move_keyboard_focus(float screen_x, float screen_y) {
    if (!snapshot_ || snapshot_->spaces.empty()) {
        return;
    }
    if (!keyboard_focus_) {
        const auto* selected_piece = selected_ ? find_piece(*snapshot_, *selected_) : nullptr;
        keyboard_focus_ = selected_piece != nullptr ? selected_piece->location
                                                    : snapshot_->spaces.front().id;
        board_.set_focused_space(keyboard_focus_);
        return;
    }
    const auto current = std::ranges::find(snapshot_->spaces, *keyboard_focus_,
                                           &SpaceVisual::id);
    if (current == snapshot_->spaces.end()) {
        keyboard_focus_ = snapshot_->spaces.front().id;
        board_.set_focused_space(keyboard_focus_);
        return;
    }
    const auto world_origin = board_.camera().screen_to_world({0.0F, 0.0F});
    auto direction = board_.camera().screen_to_world({screen_x, screen_y}) - world_origin;
    const auto direction_length = std::max(std::hypot(direction.x, direction.y), 0.0001F);
    direction = direction * (1.0F / direction_length);
    const auto origin = current->bounds.center();
    const SpaceVisual* best = nullptr;
    auto best_score = std::numeric_limits<float>::max();
    for (const auto& candidate : snapshot_->spaces) {
        const auto delta = candidate.bounds.center() - origin;
        const auto forward = delta.x * direction.x + delta.y * direction.y;
        if (forward <= 0.01F) {
            continue;
        }
        const auto perpendicular = std::abs(delta.x * direction.y - delta.y * direction.x);
        const auto distance = std::hypot(delta.x, delta.y);
        const auto score = perpendicular * 4.0F + distance;
        if (score < best_score ||
            (score == best_score && best != nullptr && candidate.id < best->id)) {
            best = &candidate;
            best_score = score;
        }
    }
    if (best != nullptr) {
        keyboard_focus_ = best->id;
        board_.set_focused_space(keyboard_focus_);
    }
}

void PlayerWindow::choose_promotion(std::uint32_t variant) {
    if (variant >= pending_promotions_.size() || !pending_promotions_[variant]) {
        status_.set_text("That promotion choice is no longer available");
        if (game_ == PlayerGame::chess) {
            promotion_popover_.popdown();
        } else {
            promotion_dialog_.hide();
        }
        return;
    }
    const auto token = *pending_promotions_[variant];
    pending_promotions_.fill(std::nullopt);
    if (game_ == PlayerGame::chess) {
        promotion_popover_.popdown();
    } else {
        promotion_dialog_.hide();
    }
    if (!controller_.submit(token)) {
        report_queue_full();
    }
}

void PlayerWindow::update_chess_hud() {
    if (!view_ || view_->participants.size() < 2U) {
        return;
    }
    const auto top = board_flipped_ ? 0U : 1U;
    const auto bottom = board_flipped_ ? 1U : 0U;
    const auto update_card = [this](std::size_t participant_index, Gtk::Box& card,
                                    Gtk::Label& name, Gtk::Label& detail,
                                    Gtk::Label& captures, Gtk::Label& clock) {
        const auto& participant = view_->participants[participant_index];
        name.set_text(participant.name);
        detail.set_text(participant.subtitle);
        card.remove_css_class("active-player");
        if (participant.active) {
            card.add_css_class("active-player");
        }
        std::string tray;
        const auto opposing = participant.side == ParticipantSide::first
                                  ? ParticipantSide::second : ParticipantSide::first;
        for (const auto& item : view_->captured_items) {
            if (item.captured_from != opposing) {
                continue;
            }
            if (!tray.empty()) {
                tray += "  ·  ";
            }
            tray += item.label + " ×" + std::to_string(item.count);
        }
        captures.set_text(tray.empty() ? "No captures" : tray);
        const auto found = std::ranges::find(view_->clocks, participant.side,
                                             &ClockView::side);
        clock.set_visible(found != view_->clocks.end());
        if (found != view_->clocks.end()) {
            clock.set_text(format_clock(found->committed_remaining_milliseconds));
            clock.remove_css_class("active-clock");
            clock.remove_css_class("expired-clock");
            if (found->active) {
                clock.add_css_class("active-clock");
            }
            if (found->expired) {
                clock.add_css_class("expired-clock");
            }
        }
    };
    update_card(top, top_player_card_, top_player_name_, top_player_detail_,
                top_captures_, top_clock_);
    update_card(bottom, bottom_player_card_, bottom_player_name_, bottom_player_detail_,
                bottom_captures_, bottom_clock_);

    undo_button_.set_sensitive(view_->match_controls.can_undo);
    redo_button_.set_sensitive(view_->match_controls.can_redo);
    overflow_undo_.set_sensitive(view_->match_controls.can_undo);
    overflow_redo_.set_sensitive(view_->match_controls.can_redo);
    overflow_resign_.set_sensitive(view_->match_controls.can_resign);
    overflow_draw_.set_sensitive(view_->match_controls.can_offer_draw);
    return_live_button_.set_visible(view_->match_controls.can_return_to_live);
    rebuild_move_list();

    std::ostringstream summary;
    summary << view_->participants[0].name << "  vs  " << view_->participants[1].name;
    if (view_->clocks.empty()) {
        summary << "\nUnclocked local match";
    } else {
        summary << "\n" << view_->clocks.front().increment_milliseconds / 1'000
                << " second increment";
    }
    if (!snapshot_->status.empty()) {
        summary << "\n\n" << snapshot_->status;
    }
    game_summary_.set_text(summary.str());

    while (auto* child = claim_box_.get_first_child()) {
        claim_box_.remove(*child);
    }
    for (const auto& claim : view_->draw_claims) {
        auto* button = Gtk::make_managed<Gtk::Button>(claim.label);
        const auto reason = claim.label.find("threefold") != std::string::npos
                                ? chess::MatchResultReason::threefold_repetition
                                : chess::MatchResultReason::fifty_move_rule;
        const auto intended = claim.intended_action_token;
        button->signal_clicked().connect([this, reason, intended] {
            if (!controller_.claim_draw(reason, intended)) {
                report_queue_full();
            }
        });
        claim_box_.append(*button);
    }
    result_panel_.set_visible(view_->match_result.has_value());
    if (view_->match_result) {
        result_title_.set_text(view_->match_result->title);
        result_detail_.set_text(view_->match_result->result_token + "  ·  " +
                                view_->match_result->reason);
    }
}

void PlayerWindow::rebuild_move_list() {
    while (auto* child = moves_list_.get_first_child()) {
        auto* row = dynamic_cast<Gtk::ListBoxRow*>(child);
        if (row == nullptr) {
            break;
        }
        moves_list_.remove(*row);
    }
    if (!view_) {
        return;
    }
    for (std::size_t index = 0U; index < view_->timeline.size(); index += 2U) {
        auto* row = Gtk::make_managed<Gtk::ListBoxRow>();
        auto* grid = Gtk::make_managed<Gtk::Grid>();
        grid->set_column_spacing(10);
        grid->set_margin_start(8);
        grid->set_margin_end(8);
        grid->set_margin_top(5);
        grid->set_margin_bottom(5);
        const auto& first = view_->timeline[index];
        auto* number = Gtk::make_managed<Gtk::Label>(std::to_string(first.move_number) + ".");
        auto* white = Gtk::make_managed<Gtk::Label>();
        auto* black = Gtk::make_managed<Gtk::Label>();
        white->set_xalign(0.0F);
        black->set_xalign(0.0F);
        const TimelineEntryView* white_entry = nullptr;
        const TimelineEntryView* black_entry = nullptr;
        for (std::size_t offset = 0U; offset < 2U && index + offset < view_->timeline.size();
             ++offset) {
            const auto& entry = view_->timeline[index + offset];
            if (entry.side == ParticipantSide::first) {
                white_entry = &entry;
            } else {
                black_entry = &entry;
            }
        }
        white->set_text(white_entry != nullptr ? white_entry->notation : "—");
        black->set_text(black_entry != nullptr ? black_entry->notation : "—");
        grid->attach(*number, 0, 0);
        grid->attach(*white, 1, 0);
        grid->attach(*black, 2, 0);
        white->set_hexpand(true);
        black->set_hexpand(true);
        const bool current = (white_entry != nullptr && white_entry->current) ||
                             (black_entry != nullptr && black_entry->current);
        const bool previewed = (white_entry != nullptr && white_entry->previewed) ||
                               (black_entry != nullptr && black_entry->previewed);
        if (current) {
            row->add_css_class("current-ply");
        }
        if (previewed) {
            row->add_css_class("previewed-ply");
        }
        row->set_child(*grid);
        moves_list_.append(*row);
    }
    if (auto adjustment = moves_scroll_.get_vadjustment(); adjustment && !view_->timeline.empty()) {
        adjustment->set_value(adjustment->get_upper());
    }
}

void PlayerWindow::update_responsive_layout() {
    const auto width = get_width();
    const auto compact = width < 900;
    if (compact != compact_layout_) {
        compact_layout_ = compact;
        content_.set_orientation(compact ? Gtk::Orientation::VERTICAL
                                         : Gtk::Orientation::HORIZONTAL);
        chess_sidebar_.set_size_request(compact ? -1 : 320, compact ? 250 : -1);
        sidebar_toggle_.set_label(compact ? "Drawer" : "Moves");
        board_.set_size_request(compact ? 340 : 500, compact ? 340 : 500);
        board_.set_vexpand(!compact);
        board_overlay_.set_vexpand(!compact);
        open_button_.set_visible(!compact);
        save_button_.set_visible(!compact);
        undo_button_.set_visible(!compact);
        redo_button_.set_visible(!compact);
        if (compact) {
            sidebar_visible_ = false;
            chess_sidebar_.set_visible(false);
            match_column_.set_visible(true);
        }
    }
    sidebar_toggle_.set_visible(width < 1'150);
    if (width >= 1'150 && !sidebar_visible_) {
        sidebar_visible_ = true;
        chess_sidebar_.set_visible(true);
    }
}

void PlayerWindow::present_new_match() {
    new_match_error_.set_text({});
    new_match_dialog_.present();
}

void PlayerWindow::start_new_match() {
    chess::ChessMatchSettings settings;
    settings.white_name = white_name_entry_.get_text();
    settings.black_name = black_name_entry_.get_text();
    const auto fen = fen_entry_.get_text();
    if (!fen.empty()) {
        const auto position = chess::Position::from_fen(fen.raw());
        if (!position) {
            new_match_error_.set_text("Invalid FEN: " + position.error().message);
            return;
        }
        settings.initial_position = *position;
    }
    constexpr std::array<chess::TimeControl, 7U> presets{
        chess::TimeControl{}, chess::TimeControl{60'000, 0},
        chess::TimeControl{180'000, 2'000}, chess::TimeControl{300'000, 0},
        chess::TimeControl{600'000, 0}, chess::TimeControl{900'000, 10'000},
        chess::TimeControl{1'800'000, 0}};
    const auto selected = time_control_.get_selected();
    if (selected < presets.size()) {
        settings.time_control = presets[selected];
    } else {
        settings.time_control = chess::TimeControl{
            static_cast<std::int64_t>(base_minutes_.get_value_as_int()) * 60'000,
            static_cast<std::int64_t>(increment_seconds_.get_value_as_int()) * 1'000};
    }
    if (settings.white_name.empty() || settings.black_name.empty()) {
        new_match_error_.set_text("Both player names are required.");
        return;
    }
    const auto wants_flipped = orientation_.get_selected() == 1U;
    if (wants_flipped != board_flipped_) {
        board_.flip_board();
        board_flipped_ = wants_flipped;
    }
    remembered_time_control_ = selected;
    save_preferences();
    clear_selection();
    if (!controller_.new_chess_match(std::move(settings))) {
        report_queue_full();
        return;
    }
    new_match_dialog_.hide();
}

void PlayerWindow::choose_file(bool save, bool pgn) {
    const auto title = save ? (pgn ? "Export PGN" : "Save Match")
                            : (pgn ? "Import PGN" : "Open Match");
    file_chooser_ = Gtk::FileChooserNative::create(
        title, *this, save ? Gtk::FileChooser::Action::SAVE
                           : Gtk::FileChooser::Action::OPEN,
        save ? "Save" : "Open", "Cancel");
    if (save) {
        file_chooser_->set_current_name(pgn ? "ivory-reliquary.pgn" : "match.lmatch");
    }
    file_chooser_->signal_response().connect([this, save, pgn](int response) {
        if (response == static_cast<int>(Gtk::ResponseType::ACCEPT) && file_chooser_) {
            const auto file = file_chooser_->get_file();
            if (file) {
                const auto path = std::filesystem::path{file->get_path()};
                const auto queued = save ? (pgn ? controller_.export_pgn(path)
                                                : controller_.save_match(path))
                                         : (pgn ? controller_.import_pgn(path)
                                                : controller_.open_match(path));
                if (!queued) {
                    report_queue_full();
                }
            }
        }
        file_chooser_.reset();
    });
    file_chooser_->show();
}

void PlayerWindow::confirm_resign() {
    if (!view_) {
        return;
    }
    confirmation_ = std::make_unique<Gtk::MessageDialog>(
        *this, "Resign the current match?", false, Gtk::MessageType::QUESTION,
        Gtk::ButtonsType::YES_NO, true);
    confirmation_->set_secondary_text("This records a final match result and can be undone.");
    confirmation_->signal_response().connect([this](int response) {
        if (response == static_cast<int>(Gtk::ResponseType::YES) && view_) {
            const auto active = std::ranges::find(view_->participants, true,
                                                  &ParticipantView::active);
            const auto color = active != view_->participants.end() &&
                                       active->side == ParticipantSide::second
                                   ? chess::Color::black : chess::Color::white;
            if (!controller_.resign(color)) {
                report_queue_full();
            }
        }
        confirmation_->hide();
        Glib::signal_idle().connect_once([this] { confirmation_.reset(); });
    });
    confirmation_->present();
}

void PlayerWindow::confirm_draw() {
    confirmation_ = std::make_unique<Gtk::MessageDialog>(
        *this, "Record an agreed draw?", false, Gtk::MessageType::QUESTION,
        Gtk::ButtonsType::YES_NO, true);
    confirmation_->set_secondary_text("Both local players should agree before continuing.");
    confirmation_->signal_response().connect([this](int response) {
        if (response == static_cast<int>(Gtk::ResponseType::YES) &&
            !controller_.agree_draw()) {
            report_queue_full();
        }
        confirmation_->hide();
        Glib::signal_idle().connect_once([this] { confirmation_.reset(); });
    });
    confirmation_->present();
}

std::filesystem::path PlayerWindow::preferences_path() {
    return std::filesystem::path{g_get_user_config_dir()} / "ludus-arcanum" /
           "player-preferences.toml";
}

void PlayerWindow::load_preferences() {
    std::ifstream input{preferences_path()};
    std::string line;
    while (std::getline(input, line)) {
        const auto separator = line.find('=');
        if (separator == std::string::npos) {
            continue;
        }
        const auto key = line.substr(0U, separator);
        const auto value = line.substr(separator + 1U);
        if (key == "high_contrast") {
            high_contrast_enabled_ = value == "true";
        } else if (key == "reduced_motion") {
            reduced_motion_enabled_ = value == "true";
        } else if (key == "ui_scale") {
            try {
                ui_scale_ = std::clamp(std::stod(value), 0.8, 1.6);
            } catch (const std::exception&) {
                ui_scale_ = 1.0;
            }
        } else if (key == "time_control") {
            try {
                remembered_time_control_ = std::min<std::uint32_t>(
                    static_cast<std::uint32_t>(std::stoul(value)), 7U);
            } catch (const std::exception&) {
                remembered_time_control_ = 0U;
            }
        } else if (key == "orientation") {
            board_flipped_ = value == "black";
        }
    }
}

void PlayerWindow::save_preferences() {
    const auto path = preferences_path();
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) {
        status_.set_text("Unable to create preferences directory");
        return;
    }
    auto temporary = path;
    temporary += ".tmp";
    {
        std::ofstream output{temporary, std::ios::trunc};
        output << "high_contrast=" << (high_contrast_enabled_ ? "true" : "false")
               << "\nreduced_motion=" << (reduced_motion_enabled_ ? "true" : "false")
               << "\nui_scale=" << ui_scale_
               << "\ntime_control=" << remembered_time_control_
               << "\norientation=" << (board_flipped_ ? "black" : "white") << '\n';
        if (!output.flush()) {
            status_.set_text("Unable to write preferences");
            return;
        }
    }
    std::filesystem::rename(temporary, path, error);
    if (error) {
        std::filesystem::remove(path, error);
        error.clear();
        std::filesystem::rename(temporary, path, error);
    }
    if (error) {
        status_.set_text("Unable to publish preferences");
    }
}

void PlayerWindow::apply_preferences() {
    if (!css_provider_) {
        css_provider_ = Gtk::CssProvider::create();
        Gtk::StyleContext::add_provider_for_display(
            Gdk::Display::get_default(), css_provider_, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }
    const auto foreground = high_contrast_enabled_ ? "#fff4d5" : "#ded5c3";
    const auto panel = high_contrast_enabled_ ? "#08090d" : "#11141c";
    const auto border = high_contrast_enabled_ ? "#f5c96a" : "#72562c";
    std::ostringstream css;
    css << "window { background: #080a10; color: " << foreground << "; font-size: "
        << (100.0 * ui_scale_) << "%; }"
        << ".reliquary-title { font-family: serif; font-weight: 800; letter-spacing: 2px; }"
        << ".player-card, .match-sidebar { background: " << panel
        << "; border: 1px solid " << border << "; border-radius: 10px; padding: 8px; }"
        << ".active-player { border-color: #d5aa50; box-shadow: inset 0 0 0 1px #d5aa50; }"
        << ".player-name, .result-title { font-family: serif; font-size: 1.25em; font-weight: 800; }"
        << ".player-detail { color: #bcae92; }"
        << ".chess-clock { font-family: monospace; font-size: 1.5em; font-weight: 800; padding: 6px 10px; }"
        << ".active-clock { background: #3c321d; color: #ffe29a; border-radius: 7px; }"
        << ".expired-clock { background: #591b20; color: white; }"
        << ".current-ply { background: #45371f; } .previewed-ply { background: #263d50; }"
        << ".match-sidebar list, .match-sidebar viewport, .match-sidebar scrolledwindow { background: "
        << panel << "; color: " << foreground << "; }"
        << "headerbar { background: #11131a; color: #ead7aa; border-bottom: 1px solid "
        << border << "; }"
        << ".result-panel { background: rgba(8,9,13,0.94); border: 2px solid #c99a45; border-radius: 14px; padding: 24px; }"
        << ".error { color: #ff7777; } .field-title { font-weight: 700; margin-top: 5px; }";
    css_provider_->load_from_data(css.str());
}

std::string PlayerWindow::format_clock(std::int64_t milliseconds) {
    milliseconds = std::max<std::int64_t>(0, milliseconds);
    const auto total_seconds = (milliseconds + 999) / 1'000;
    const auto hours = total_seconds / 3'600;
    const auto minutes = (total_seconds / 60) % 60;
    const auto seconds = total_seconds % 60;
    std::ostringstream output;
    if (hours != 0) {
        output << hours << ':';
        if (minutes < 10) {
            output << '0';
        }
    }
    output << minutes << ':';
    if (seconds < 10) {
        output << '0';
    }
    output << seconds;
    return output.str();
}

void PlayerWindow::update_clock_display() {
    if (!view_ || view_->clocks.empty()) {
        return;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - clock_view_received_).count();
    const auto displayed = [&](ParticipantSide side) {
        const auto found = std::ranges::find(view_->clocks, side, &ClockView::side);
        if (found == view_->clocks.end()) {
            return std::string{};
        }
        const auto remaining = found->active && !found->paused
                                   ? found->committed_remaining_milliseconds - elapsed
                                   : found->committed_remaining_milliseconds;
        return format_clock(remaining);
    };
    const auto top = board_flipped_ ? ParticipantSide::first : ParticipantSide::second;
    const auto bottom = board_flipped_ ? ParticipantSide::second : ParticipantSide::first;
    top_clock_.set_text(displayed(top));
    bottom_clock_.set_text(displayed(bottom));
}

void PlayerWindow::update_interaction() {
    InteractionState interaction;
    interaction.reduced_motion = reduced_motion_enabled_;
    interaction.selected = selected_;
    interaction.keyboard_focus = keyboard_focus_;
    if (snapshot_ && selected_) {
        for (const auto& hint : snapshot_->actions) {
            if (hint.actor == *selected_) {
                auto kind = InteractionTargetKind::quiet_move;
                if (hint.kind == ActionVisualKind::capture) {
                    kind = InteractionTargetKind::capture;
                } else if (hint.kind == ActionVisualKind::castle) {
                    kind = InteractionTargetKind::castle;
                } else if (hint.kind == ActionVisualKind::promotion) {
                    kind = InteractionTargetKind::promotion;
                } else if (hint.kind == ActionVisualKind::draw_claim) {
                    kind = InteractionTargetKind::draw_claim;
                }
                interaction.targets.push_back(InteractionTarget{hint.destination, kind});
            }
        }
    }
    board_.set_interaction(std::move(interaction));
}

void PlayerWindow::clear_selection() {
    selected_.reset();
    update_interaction();
}

std::uint32_t PlayerWindow::promotion_variant() const noexcept {
    constexpr std::array variants{
        static_cast<std::uint32_t>(chess::PieceType::queen),
        static_cast<std::uint32_t>(chess::PieceType::rook),
        static_cast<std::uint32_t>(chess::PieceType::bishop),
        static_cast<std::uint32_t>(chess::PieceType::knight),
    };
    const auto selected = promotion_.get_selected();
    return selected < variants.size() ? variants[selected] : variants.front();
}

void PlayerWindow::report_queue_full() {
    status_.set_text("Simulation command queue is full");
}

} // namespace ludus::player
