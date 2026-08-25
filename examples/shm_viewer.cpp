// Minimal Qt real-time SHM display.
// Build: cmake --build build --target shm_viewer
// Run:   ./build/shm_recorder   (terminal 1)
//        ./build/shm_viewer     (terminal 2)
#include "MainWindow.hpp"

#include <QApplication>

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  rtplot::gui::MainWindow win;
  if (!win.startShmSource("rtplot_ctrl")) return 1;
  win.show();
  return app.exec();
}
