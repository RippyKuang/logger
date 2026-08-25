#pragma once
#include "rtplot/shm_ipc.hpp"
#include "rtplot/storage.hpp"
#include "rtplot/udp.hpp"

#include "PlotWidget.hpp"
#include "TrajectoryWidget.hpp"

#include <QColor>
#include <QMainWindow>
#include <QMap>
#include <QSet>
#include <QString>
#include <QVector>

#include <vector>

class QComboBox;
class QDockWidget;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QSlider;
class QStackedWidget;
class QTabWidget;
class QTimer;

namespace rtplot::gui {

/// Single viewer application window. Data source is selected in `apps/viewer.cpp`.
class MainWindow : public QMainWindow {
public:
  enum class Source { None, File, Shm, Udp, Record };

  explicit MainWindow(QWidget* parent = nullptr);
  ~MainWindow() override;

  bool openDatabase(const QString& path);
  bool startShmSource(const QString& controlName = "rtplot_ctrl");
  bool startUdpSource(uint16_t port = 9870);
  bool startRecordSource(const QString& dbPath, double durationSeconds);
  void loadDemoData();

  [[nodiscard]] Source source() const { return source_; }
  [[nodiscard]] bool isChannelVisible(const QString& name) const { return visibleChannels_.contains(name); }
  [[nodiscard]] PlotGridWidget* plotWidget() const { return plot_; }

private:
  struct ChannelData {
    QString name;
    std::vector<Sample> samples;
    std::vector<ArraySample> arrays;
    bool isArray = false;
    QColor color;
    int strip = 0;
  };

  struct FrameConfig {
    QString name;
    QString positionChannel;   ///< array channel with at least 3 values
    QString orientationChannel;///< array channel with 3 (Euler) or 4 (quaternion) values
    QColor color;
  };

  void setupUi();
  bool eventFilter(QObject* watched, QEvent* event) override;
  void setupTimeSeriesSidebar(QWidget* page);
  void setupPoseSidebar(QWidget* page);
  void stopSource();
  void stopPlayback();
  void startPlayback();
  void stepForward();
  void applyTime(Timestamp t, bool updateSlider);
  void exportCurrent();
  bool writeCsv(const QString& path, const double* t0, const double* t1);
  void updateLiveShm();
  void updateLiveUdp();
  void updateRecordSource();
  void refreshComboAndTrajectory();
  void populateScalarChannelList();
  void populateRoiChannelCombo();
  void populateArrayCombo(QComboBox* cb, const QString& selected);
  void syncFrameList();
  void syncFrameUiFromConfig();
  void updateFrameConfigFromUi();
  void addFrame();
  void removeSelectedFrame();
  void buildFrames();
  void updateRoiSidebar();
  void showChannel(const QString& name);
  void hideChannel(const QString& name);
  void rebuildStripIndices();
  void updateVisibleCurves();
  void updatePoseAt(Timestamp t);
  int subplotIndex(const QString& channel);
  const ChannelData* findChannel(const QString& name) const;
  ChannelData* findChannelMutable(const QString& name);
  void addChannelData(const QString& name, bool isArray);
  void addChannel(const QString& name, std::vector<Sample> data, int stripHint = -1);
  FrameConfig* selectedFrame();
  const FrameConfig* selectedFrame() const;

  PlotGridWidget* plot_ = nullptr;
  TrajectoryWidget* trajectory_ = nullptr;
  QTabWidget* tabs_ = nullptr;
  QDockWidget* dock_ = nullptr;
  QStackedWidget* sideStack_ = nullptr;

  QListWidget* channelList_ = nullptr;
  QComboBox* roiChannel_ = nullptr;
  QPlainTextEdit* roiStatsEdit_ = nullptr;

  QListWidget* frameList_ = nullptr;
  QLineEdit* frameNameEdit_ = nullptr;
  QComboBox* framePos_ = nullptr;
  QComboBox* frameOrient_ = nullptr;
  QLabel* frameOrientInfo_ = nullptr;

  QComboBox* speedBox_ = nullptr;
  QSlider* slider_ = nullptr;
  QLabel* roiLabel_ = nullptr;
  QLabel* timeLabel_ = nullptr;
  QTimer* playbackTimer_ = nullptr;
  QTimer* liveTimer_ = nullptr;
  QTimer* recordTimer_ = nullptr;

  QVector<ChannelData> channels_;
  QMap<QString, int> prefixStripMap_;
  QSet<QString> visibleChannels_;
  std::vector<Event> events_;
  std::vector<FrameConfig> frames_;
  int currentFrame_ = -1;
  StorageReader reader_;
  ShmReader shm_;
  UdpReceiver udp_;
  QString dbPath_;
  Source source_ = Source::None;
  bool playing_ = false;
  bool syncingFrameUi_ = false;
  bool trajectoryNeedsFit_ = true;
  double speed_ = 1.0;
  Timestamp tStart_ = 0;
  Timestamp tEnd_ = 1;
  Timestamp tCurrent_ = 0;
  double recordElapsed_ = 0.0;
  double recordDuration_ = 0.0;
  double nextRecordEvent_ = 0.0;
  double lastFrameBuildTime_ = -1.0;
  Timestamp recordBase_ = 0;
  std::vector<PoseSample> poseSamples_;
};

} // namespace rtplot::gui
