#pragma once

#include "ludus/gtk/board_canvas.hpp"

#include "player_controller.hpp"

#include <gtkmm.h>

#include <array>
#include <memory>
#include <optional>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace ludus::player {

class PlayerWindow final : public Gtk::ApplicationWindow {
  public:
    explicit PlayerWindow(PlayerGame game,
                          RendererPreference renderer = RendererPreference::automatic,
                          bool print_renderer_info = false,
                          std::size_t stress_sprites = 0U,
                          bool hot_seat = false);
    PlayerWindow(const PlayerWindow&) = delete;
    PlayerWindow& operator=(const PlayerWindow&) = delete;
    PlayerWindow(PlayerWindow&&) = delete;
    PlayerWindow& operator=(PlayerWindow&&) = delete;
    ~PlayerWindow() override;

  private:
    void on_snapshot();
    void on_space_activated(SpaceId space);
    void on_space_dropped(SpaceId origin, SpaceId destination);
    void update_interaction();
    void clear_selection();
    void submit_choice();
    bool on_key_pressed(guint keyval, Gdk::ModifierType modifiers = {});
    void move_keyboard_focus(float screen_x, float screen_y);
    void choose_promotion(std::uint32_t variant);
    [[nodiscard]] std::uint32_t promotion_variant() const noexcept;
    void report_queue_full();
    void configure_chess_shell();
    void configure_tactical_shell();
    void update_chess_hud();
    void update_responsive_layout();
    void present_new_match();
    void start_new_match();
    void choose_file(bool save, bool pgn);
    void confirm_resign();
    void confirm_draw();
    void rebuild_move_list();
    void load_preferences();
    void save_preferences();
    void apply_preferences();
    void update_clock_display();
    [[nodiscard]] static std::filesystem::path preferences_path();
    [[nodiscard]] static std::string format_clock(std::int64_t milliseconds);

    Gtk::Box root_{Gtk::Orientation::VERTICAL, 0};
    Gtk::Box content_{Gtk::Orientation::HORIZONTAL, 0};
    Gtk::HeaderBar header_;
    Gtk::Label header_title_{"THE IVORY RELIQUARY"};
    Gtk::Button new_button_{"New"};
    Gtk::Button open_button_{"Open"};
    Gtk::Button save_button_{"Save"};
    Gtk::MenuButton overflow_button_;
    Gtk::Popover overflow_popover_;
    Gtk::Box overflow_box_{Gtk::Orientation::VERTICAL, 4};
    Gtk::Button overflow_open_{"Open match"};
    Gtk::Button overflow_save_{"Save match"};
    Gtk::Button overflow_undo_{"Undo"};
    Gtk::Button overflow_redo_{"Redo"};
    Gtk::Button overflow_replay_{"Replay match"};
    Gtk::Button overflow_flip_{"Flip board"};
    Gtk::Button overflow_fit_{"Fit board"};
    Gtk::Button overflow_import_{"Import PGN"};
    Gtk::Button overflow_export_{"Export PGN"};
    Gtk::Button overflow_copy_fen_{"Copy FEN"};
    Gtk::Button overflow_copy_pgn_{"Copy PGN"};
    Gtk::Button overflow_paste_{"Import clipboard"};
    Gtk::Button overflow_resign_{"Resign…"};
    Gtk::Button overflow_draw_{"Agree draw…"};
    Gtk::CheckButton high_contrast_{"High contrast"};
    Gtk::CheckButton reduced_motion_{"Reduced motion"};
    Glib::RefPtr<Gtk::Adjustment> ui_scale_adjustment_;
    Gtk::Scale ui_scale_control_{Gtk::Orientation::HORIZONTAL};
    Gtk::Button sidebar_toggle_{"Moves"};
    Gtk::Box toolbar_{Gtk::Orientation::HORIZONTAL, 8};
    Gtk::Button undo_button_{"Undo"};
    Gtk::Button redo_button_{"Redo"};
    Gtk::Button restart_button_{"Restart"};
    Gtk::Button replay_button_{"Replay"};
    Gtk::Button fit_button_{"Fit board"};
    Gtk::Button flip_button_{"Flip board"};
    Gtk::Label promotion_label_{"Promotion:"};
    Glib::RefPtr<Gtk::StringList> promotion_model_;
    Gtk::DropDown promotion_;
    Gtk::Label choice_label_{"Choice:"};
    Glib::RefPtr<Gtk::StringList> choice_model_;
    Gtk::DropDown choice_;
    Gtk::Button choice_button_{"Apply"};
    Gtk::Window promotion_dialog_;
    Gtk::Popover promotion_popover_;
    Gtk::Box promotion_dialog_root_{Gtk::Orientation::VERTICAL, 10};
    Gtk::Label promotion_dialog_label_{"Choose the piece for promotion"};
    Gtk::Box promotion_dialog_buttons_{Gtk::Orientation::HORIZONTAL, 8};
    Gtk::Button promote_queen_{"Queen"};
    Gtk::Button promote_rook_{"Rook"};
    Gtk::Button promote_bishop_{"Bishop"};
    Gtk::Button promote_knight_{"Knight"};
    Gtk::Label status_;
    BoardCanvas board_;
    Gtk::Box match_column_{Gtk::Orientation::VERTICAL, 8};
    Gtk::Box top_player_card_{Gtk::Orientation::HORIZONTAL, 10};
    Gtk::Picture top_crest_;
    Gtk::Box top_player_text_{Gtk::Orientation::VERTICAL, 1};
    Gtk::Label top_player_name_;
    Gtk::Label top_player_detail_;
    Gtk::Label top_captures_;
    Gtk::Label top_clock_;
    Gtk::Overlay board_overlay_;
    Gtk::Box result_panel_{Gtk::Orientation::VERTICAL, 8};
    Gtk::Label result_title_;
    Gtk::Label result_detail_;
    Gtk::Box result_actions_{Gtk::Orientation::HORIZONTAL, 6};
    Gtk::Button rematch_button_{"Rematch"};
    Gtk::Button result_export_button_{"Export PGN"};
    Gtk::Box bottom_player_card_{Gtk::Orientation::HORIZONTAL, 10};
    Gtk::Picture bottom_crest_;
    Gtk::Box bottom_player_text_{Gtk::Orientation::VERTICAL, 1};
    Gtk::Label bottom_player_name_;
    Gtk::Label bottom_player_detail_;
    Gtk::Label bottom_captures_;
    Gtk::Label bottom_clock_;
    Gtk::Box chess_sidebar_{Gtk::Orientation::VERTICAL, 8};
    Gtk::StackSwitcher sidebar_switcher_;
    Gtk::Stack sidebar_stack_;
    Gtk::Box moves_page_{Gtk::Orientation::VERTICAL, 6};
    Gtk::ScrolledWindow moves_scroll_;
    Gtk::ListBox moves_list_;
    Gtk::Button return_live_button_{"Return to live position"};
    Gtk::Box game_page_{Gtk::Orientation::VERTICAL, 8};
    Gtk::Label game_summary_;
    Gtk::Box claim_box_{Gtk::Orientation::VERTICAL, 4};
    Gtk::ScrolledWindow hud_scroll_;
    Gtk::Label hud_;
    Gtk::Window new_match_dialog_;
    Gtk::Box new_match_root_{Gtk::Orientation::VERTICAL, 10};
    Gtk::Entry white_name_entry_;
    Gtk::Entry black_name_entry_;
    Gtk::Entry fen_entry_;
    Glib::RefPtr<Gtk::StringList> time_control_model_;
    Gtk::DropDown time_control_;
    Glib::RefPtr<Gtk::Adjustment> base_adjustment_;
    Glib::RefPtr<Gtk::Adjustment> increment_adjustment_;
    Gtk::SpinButton base_minutes_;
    Gtk::SpinButton increment_seconds_;
    Glib::RefPtr<Gtk::StringList> orientation_model_;
    Gtk::DropDown orientation_;
    Gtk::Label new_match_error_;
    Gtk::Box new_match_actions_{Gtk::Orientation::HORIZONTAL, 8};
    Gtk::Button new_match_cancel_{"Cancel"};
    Gtk::Button new_match_start_{"Start match"};
    PlayerController controller_;
    sigc::connection dispatcher_connection_;
    std::shared_ptr<const PlayerView> view_;
    std::shared_ptr<const RenderSnapshot> snapshot_;
    std::optional<EntityId> selected_;
    std::optional<SpaceId> keyboard_focus_;
    std::array<std::optional<std::uint64_t>, 7U> pending_promotions_{};
    std::optional<SpriteId> board_material_sprite_;
    std::optional<SpriteId> frame_corner_sprite_;
    std::optional<SpaceId> annotation_start_;
    Vec2 annotation_pointer_start_{};
    std::vector<std::pair<SpaceId, SpaceId>> board_arrows_;
    std::vector<SpaceId> board_markers_;
    std::vector<std::string> annotation_timeline_;
    std::uint64_t annotation_revision_{1U};
    bool annotation_timeline_initialized_{false};
    PlayerGame game_{PlayerGame::chess};
    Glib::RefPtr<Gtk::FileChooserNative> file_chooser_;
    std::unique_ptr<Gtk::MessageDialog> confirmation_;
    Glib::RefPtr<Gtk::CssProvider> css_provider_;
    std::chrono::steady_clock::time_point clock_view_received_{};
    bool sidebar_visible_{true};
    bool board_flipped_{false};
    bool compact_layout_{false};
    bool high_contrast_enabled_{false};
    bool reduced_motion_enabled_{false};
    double ui_scale_{1.0};
    std::uint32_t remembered_time_control_{0U};
    bool print_renderer_info_{false};
    std::size_t stress_sprites_{0U};
    bool stress_snapshot_installed_{false};
};

} // namespace ludus::player
