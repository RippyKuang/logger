// Unified rtplot viewer / recorder.
//
// Source selection is done with one flag:
//   viewer [file.db]                         offline replay
//   viewer --source shm   [--shm-name N]     live view from shared memory
//   viewer --source udp   [--udp-port P]     live view from UDP stream
//   viewer --source record [--record-db F]   record directly and display
//                         [--record-duration S]
#include "MainWindow.hpp"

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QString>

#include <cstdio>

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  QCoreApplication::setApplicationName("viewer");
  QCoreApplication::setApplicationVersion("1.0");

  QCommandLineParser parser;
  parser.setApplicationDescription(
      "rtplot viewer: SHM live / UDP live / direct-record / offline .db player");
  parser.addHelpOption();
  parser.addVersionOption();

  QCommandLineOption sourceOpt(
      QStringList() << "s" << "source",
      "Data source: shm (default), udp, record", "source", "shm");
  QCommandLineOption shmNameOpt(QStringList() << "shm-name",
                                "POSIX shared-memory control segment name.", "name", "rtplot_ctrl");
  QCommandLineOption udpPortOpt(QStringList() << "udp-port", "UDP listen port.", "port", "9870");
  QCommandLineOption recordDbOpt(QStringList() << "record-db",
                                 "Output .db for --source record.", "path", "viewer_record.db");
  QCommandLineOption recordDurationOpt(QStringList() << "record-duration",
                                       "Recording duration in seconds for --source record.",
                                       "seconds", "10.0");
  QCommandLineOption demoOpt(QStringList() << "demo", "Load built-in demo data (offline mode).");

  parser.addOption(sourceOpt);
  parser.addOption(shmNameOpt);
  parser.addOption(udpPortOpt);
  parser.addOption(recordDbOpt);
  parser.addOption(recordDurationOpt);
  parser.addOption(demoOpt);
  parser.process(app);

  rtplot::gui::MainWindow win;
  const QStringList args = parser.positionalArguments();
  const QString source = parser.value(sourceOpt).toLower();

  bool ok = true;
  if (parser.isSet(demoOpt) || source == "demo") {
    win.loadDemoData();
  } else if (!args.isEmpty()) {
    ok = win.openDatabase(args.first());
  } else if (source == "udp") {
    ok = win.startUdpSource(static_cast<uint16_t>(parser.value(udpPortOpt).toUInt()));
  } else if (source == "record") {
    const double seconds = parser.value(recordDurationOpt).toDouble();
    ok = win.startRecordSource(parser.value(recordDbOpt), seconds > 0.0 ? seconds : 10.0);
  } else { // shm (default)
    ok = win.startShmSource(parser.value(shmNameOpt));
  }

  if (!ok) {
    std::fprintf(stderr, "viewer: failed to initialize source '%s'. Use --help for options.\n",
                 source.toUtf8().constData());
    return 1;
  }

  win.show();
  return app.exec();
}
