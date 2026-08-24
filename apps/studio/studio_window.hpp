#pragma once

#include "ludus/gtk/board_canvas.hpp"
#include "ludus/studio/package_document.hpp"

#include "studio_controller.hpp"

#include <gtkmm.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace ludus::studio_app {

class StudioWindow final : public Gtk::ApplicationWindow {
  public:
    StudioWindow();
    StudioWindow(const StudioWindow&) = delete;
    StudioWindow& operator=(const StudioWindow&) = delete;
    StudioWindow(StudioWindow&&) = delete;
    StudioWindow& operator=(StudioWindow&&) = delete;
    ~StudioWindow() override;

  private:
    void create_package();
    void open_package();
    [[nodiscard]] bool save_package();
    void load_document(studio::PackageDocument document);
    void refresh_preview();
    void refresh_package_fields();
    void generate_board();
    void reset_chess_setup();
    void import_asset();
    void refresh_visual_theme();
    void upsert_entity();
    void delete_entity();
    void select_entity(const studio::BoardEntity* entity);
    void on_space_activated(SpaceId space);
    void start_playtest();
    void return_to_edit();
    void reload_rules();
    void on_playtest_view();
    void update_playtest_interaction();
    void show_diagnostic(const Diagnostic& diagnostic);
    void show_message(std::string message, bool error = false);
    void report_queue_full();

    Gtk::Box root_{Gtk::Orientation::VERTICAL, 0};
    Gtk::Box toolbar_{Gtk::Orientation::HORIZONTAL, 6};
    Gtk::Entry path_entry_;
    Gtk::Button new_button_{"New"};
    Gtk::Button open_button_{"Open"};
    Gtk::Button save_button_{"Save"};
    Gtk::Button edit_button_{"Edit"};
    Gtk::Button playtest_button_{"Playtest"};
    Gtk::Button reload_button_{"Reload rules"};
    Gtk::Button undo_button_{"Undo"};
    Gtk::Button redo_button_{"Redo"};
    Gtk::Button restart_button_{"Restart"};
    Gtk::Button fit_button_{"Fit"};
    Gtk::Label status_;

    Gtk::Paned content_{Gtk::Orientation::HORIZONTAL};
    Gtk::ScrolledWindow inspector_scroll_;
    Gtk::Box inspector_{Gtk::Orientation::VERTICAL, 8};
    Gtk::Label package_heading_;
    Gtk::Grid package_grid_;
    Gtk::Entry package_id_entry_;
    Gtk::Entry package_version_entry_;
    Gtk::Entry module_entry_;
    Gtk::Label board_heading_;
    Gtk::Grid board_grid_;
    Gtk::SpinButton width_spin_;
    Gtk::SpinButton height_spin_;
    Gtk::Button generate_button_{"Generate topology"};
    Gtk::Button chess_setup_button_{"Reset chess setup"};
    Gtk::Label asset_heading_;
    Gtk::Grid asset_grid_;
    Gtk::Entry asset_path_entry_;
    Gtk::Entry asset_key_entry_;
    Gtk::Button asset_import_button_{"Import PNG"};
    Gtk::Label entity_heading_;
    Gtk::Grid entity_grid_;
    Gtk::Entry entity_name_entry_;
    Gtk::Entry entity_type_entry_;
    Gtk::SpinButton entity_owner_spin_;
    Gtk::SpinButton entity_x_spin_;
    Gtk::SpinButton entity_y_spin_;
    Glib::RefPtr<Gtk::StringList> sprite_model_;
    Gtk::DropDown entity_sprite_picker_;
    Gtk::Button entity_apply_button_{"Add / update entity"};
    Gtk::Button entity_delete_button_{"Delete selected"};

    Gtk::Paned workspace_{Gtk::Orientation::VERTICAL};
    Gtk::Notebook editor_notebook_;
    BoardCanvas board_;
    Gtk::ScrolledWindow source_scroll_;
    Gtk::TextView source_editor_;
    Gtk::Notebook output_notebook_;
    Gtk::ScrolledWindow diagnostics_scroll_;
    Gtk::TextView diagnostics_view_;
    Gtk::ScrolledWindow events_scroll_;
    Gtk::TextView events_view_;
    Gtk::ScrolledWindow state_scroll_;
    Gtk::TextView state_view_;

    StudioController controller_;
    sigc::connection dispatcher_connection_;
    std::optional<studio::PackageDocument> document_;
    std::shared_ptr<const RenderSnapshot> snapshot_;
    std::shared_ptr<const StudioView> playtest_view_;
    std::optional<std::string> selected_entity_name_;
    std::optional<EntityId> selected_piece_;
    std::uint64_t preview_revision_{0U};
    bool playtesting_{false};
};

} // namespace ludus::studio_app
