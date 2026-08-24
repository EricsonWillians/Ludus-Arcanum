#include "studio_window.hpp"

#include <gtkmm/application.h>

int main(int argc, char* argv[]) {
    const auto application = Gtk::Application::create("org.ludus-arcanum.studio");
    return application->make_window_and_run<ludus::studio_app::StudioWindow>(argc, argv);
}
