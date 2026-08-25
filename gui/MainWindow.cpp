#include "MainWindow.hpp"

#include "rtplot/logger.hpp"

#include <QAction>
#include <QComboBox>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPlainTextEdit>
#include <QMenuBar>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPushButton>
#include <QSet>
#include <QSlider>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTabWidget>
#include <QTimer>
#include <QToolBar>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace rtplot::gui {

namespace {
constexpr double kTickSeconds = 0.033;
constexpr double kRecordTickSeconds = 0.020;
constexpr double kRecordSampleDt = 0.001;
constexpr int kRecordSamplesPerTick = 20;
constexpr size_t kMaxLiveSamples = 1u << 20; // 1M samples/channel in live view
}

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) { setupUi(); }

MainWindow::~MainWindow() { stopSource(); }

void MainWindow::setupUi() {
  setWindowTitle("rtplot viewer");
  resize(1360, 840);

  auto* root = new QWidget(this);
  auto* rootLayout = new QHBoxLayout(root);
  rootLayout->setContentsMargins(0, 0, 0, 0);
  rootLayout->setSpacing(0);
  setCentralWidget(root);

  tabs_ = new QTabWidget(root);
  plot_ = new PlotGridWidget(tabs_);
  tabs_->addTab(plot_, "Time series");

  auto* trajPage = new QWidget(tabs_);
  auto* trajLayout = new QVBoxLayout(trajPage);
  trajLayout->setContentsMargins(0, 0, 0, 0);
  trajectory_ = new TrajectoryWidget(trajPage);
  trajLayout->addWidget(trajectory_, 1);
  tabs_->addTab(trajPage, "3D / Pose");
  rootLayout->addWidget(tabs_, 1);

  dock_ = new QDockWidget("Panel", this);
  dock_->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
  dock_->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
  sideStack_ = new QStackedWidget(dock_);
  sideStack_->setMinimumWidth(320);
  sideStack_->setMaximumWidth(480);
  auto* timeSide = new QWidget(sideStack_);
  auto* poseSide = new QWidget(sideStack_);
  setupTimeSeriesSidebar(timeSide);
  setupPoseSidebar(poseSide);
  sideStack_->addWidget(timeSide);
  sideStack_->addWidget(poseSide);
  dock_->setWidget(sideStack_);
  addDockWidget(Qt::RightDockWidgetArea, dock_);

  connect(tabs_, &QTabWidget::currentChanged, this, [this](int idx) {
    sideStack_->setCurrentIndex(idx);
    if (idx == 0) updateRoiSidebar();
    else buildFrames();
  });

  auto* tb = addToolBar("Player");
  tb->setMovable(false);
  QAction* actOpen = tb->addAction("Open .db");
  QAction* actPlay = tb->addAction("Play/Pause");
  QAction* actStep = tb->addAction("Step");
  QAction* actAuto = tb->addAction("Auto Scale");
  QAction* actExport = tb->addAction("Export CSV");

  tb->addSeparator();
  tb->addWidget(new QLabel("Speed"));
  speedBox_ = new QComboBox;
  for (double v : {0.5, 1.0, 2.0, 5.0, 10.0}) speedBox_->addItem(QString("%1x").arg(v), v);
  speedBox_->setCurrentIndex(1);
  tb->addWidget(speedBox_);

  slider_ = new QSlider(Qt::Horizontal);
  slider_->setRange(0, 100000);
  slider_->setValue(0);
  tb->addWidget(slider_);

  timeLabel_ = new QLabel("t=0.000 s");
  tb->addWidget(timeLabel_);

  connect(actOpen, &QAction::triggered, this, [this] {
    const QString path = QFileDialog::getOpenFileName(this, "Open rtplot .db", QString(), "rtplot db (*.db);;All (*)");
    if (!path.isEmpty()) openDatabase(path);
  });
  connect(actPlay, &QAction::triggered, this, [this] {
    playing_ ? stopPlayback() : startPlayback();
  });
  connect(actStep, &QAction::triggered, this, [this] { stepForward(); });
  connect(actAuto, &QAction::triggered, this, [this] {
    plot_->autoScaleX();
    plot_->autoScaleY();
  });
  connect(actExport, &QAction::triggered, this, [this] { exportCurrent(); });

  connect(speedBox_, QOverload<int>::of(&QComboBox::activated), this, [this](int idx) {
    speed_ = speedBox_->itemData(idx).toDouble();
  });
  connect(slider_, &QSlider::sliderMoved, this, [this](int pos) {
    const double f = pos / 100000.0;
    applyTime(static_cast<Timestamp>(tStart_ + (tEnd_ - tStart_) * f), false);
  });

  plot_->setRoiStatsCallback([this](const QString&) {
    statusBar()->showMessage("ROI updated", 2000);
    updateRoiSidebar();
  });

  roiLabel_ = new QLabel("Right-drag ROI on the time-series plot");
  statusBar()->addPermanentWidget(roiLabel_);

  playbackTimer_ = new QTimer(this);
  playbackTimer_->setInterval(33);
  connect(playbackTimer_, &QTimer::timeout, this, [this] {
    if (!playing_) return;
    tCurrent_ = static_cast<Timestamp>(tCurrent_ + (tEnd_ - tStart_) * kTickSeconds * speed_);
    if (tCurrent_ >= tEnd_) { tCurrent_ = tEnd_; stopPlayback(); }
    applyTime(tCurrent_, true);
  });

  liveTimer_ = new QTimer(this);
  liveTimer_->setInterval(50);
  connect(liveTimer_, &QTimer::timeout, this, [this] {
    if (source_ == Source::Shm) updateLiveShm();
    else if (source_ == Source::Udp) updateLiveUdp();
  });

  recordTimer_ = new QTimer(this);
  recordTimer_->setInterval(static_cast<int>(kRecordTickSeconds * 1000.0));
  connect(recordTimer_, &QTimer::timeout, this, [this] { updateRecordSource(); });

  plot_->addStrip();
  sideStack_->setCurrentIndex(0);
}

void MainWindow::setupTimeSeriesSidebar(QWidget* page) {
  auto* lay = new QVBoxLayout(page);
  lay->setContentsMargins(6, 6, 6, 6);

  auto* chanGrp = new QGroupBox("Channels", page);
  auto* cg = new QVBoxLayout(chanGrp);
  cg->addWidget(new QLabel("Check a channel to display it."));
  channelList_ = new QListWidget(chanGrp);
  cg->addWidget(channelList_);
  lay->addWidget(chanGrp, 3);

  auto* roiGrp = new QGroupBox("ROI Statistics", page);
  auto* rg = new QVBoxLayout(roiGrp);
  rg->addWidget(new QLabel("Channel:"));
  roiChannel_ = new QComboBox(roiGrp);
  rg->addWidget(roiChannel_);
  roiStatsEdit_ = new QPlainTextEdit(roiGrp);
  roiStatsEdit_->setReadOnly(true);
  roiStatsEdit_->setPlaceholderText("Right-drag an ROI on the plot.");
  rg->addWidget(roiStatsEdit_, 2);
  lay->addWidget(roiGrp, 2);

  connect(channelList_, &QListWidget::itemChanged, this, [this](QListWidgetItem* item) {
    if (syncingFrameUi_) return;
    const QString name = item->text();
    if (item->checkState() == Qt::Checked) showChannel(name);
    else hideChannel(name);
  });
  // Let Qt handle clicks inside the checkbox; handle clicks on the row text
  // ourselves so the whole row behaves as a toggle without double-toggling.
  channelList_->viewport()->installEventFilter(this);
  connect(roiChannel_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) { updateRoiSidebar(); });
}

void MainWindow::setupPoseSidebar(QWidget* page) {
  auto* lay = new QVBoxLayout(page);
  lay->setContentsMargins(6, 6, 6, 6);

  auto* framesGrp = new QGroupBox("Frames", page);
  auto* fg = new QVBoxLayout(framesGrp);
  frameList_ = new QListWidget(framesGrp);
  fg->addWidget(frameList_);
  auto* btnRow = new QHBoxLayout;
  auto* addBtn = new QPushButton("Add Frame", framesGrp);
  auto* delBtn = new QPushButton("Remove Frame", framesGrp);
  btnRow->addWidget(addBtn);
  btnRow->addWidget(delBtn);
  fg->addLayout(btnRow);
  lay->addWidget(framesGrp, 2);

  auto* cfgGrp = new QGroupBox("Selected Frame", page);
  auto* form = new QFormLayout(cfgGrp);
  frameNameEdit_ = new QLineEdit(cfgGrp);
  form->addRow("Name", frameNameEdit_);
  framePos_ = new QComboBox(cfgGrp);
  form->addRow("Position array", framePos_);
  frameOrient_ = new QComboBox(cfgGrp);
  form->addRow("Orientation array", frameOrient_);
  frameOrientInfo_ = new QLabel("Orientation: None", cfgGrp);
  form->addRow("Detected", frameOrientInfo_);
  lay->addWidget(cfgGrp);

  connect(addBtn, &QPushButton::clicked, this, [this] { addFrame(); });
  connect(delBtn, &QPushButton::clicked, this, [this] { removeSelectedFrame(); });
  connect(frameList_, &QListWidget::currentRowChanged, this, [this](int) { syncFrameUiFromConfig(); });
  connect(frameNameEdit_, &QLineEdit::textChanged, this, [this](const QString&) { updateFrameConfigFromUi(); });
  connect(framePos_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) { updateFrameConfigFromUi(); });
  connect(frameOrient_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) { updateFrameConfigFromUi(); });
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
  if (channelList_ && watched == channelList_->viewport() &&
      event->type() == QEvent::MouseButtonPress) {
    auto* me = static_cast<QMouseEvent*>(event);
    QListWidgetItem* item = channelList_->itemAt(me->pos());
    if (item && me->pos().x() > 24) {
      item->setCheckState(item->checkState() == Qt::Checked ? Qt::Unchecked : Qt::Checked);
      return true; // handled; do not start selection drag
    }
  }
  return QMainWindow::eventFilter(watched, event);
}

void MainWindow::stopSource() {
  if (source_ == Source::Shm) shm_.stop();
  if (source_ == Source::Udp) udp_.stop();
  if (source_ == Source::Record) {
    recordTimer_->stop();
    if (Logger::instance().running()) Logger::instance().stop();
  }
  liveTimer_->stop();
  if (source_ != Source::Record) recordTimer_->stop();
  source_ = Source::None;
  trajectoryNeedsFit_ = true;
}

int MainWindow::subplotIndex(const QString& channel) {
  const int slash = channel.indexOf('/');
  const QString prefix = slash < 0 ? channel : channel.left(slash);
  auto it = prefixStripMap_.constFind(prefix);
  if (it != prefixStripMap_.constEnd()) return it.value();

  // Assign a new stable subplot index for every newly seen prefix.
  int idx = 0;
  for (const int v : prefixStripMap_) idx = std::max(idx, v + 1);
  prefixStripMap_.insert(prefix, idx);
  return idx;
}

const MainWindow::ChannelData* MainWindow::findChannel(const QString& name) const {
  for (const auto& c : channels_) if (c.name == name) return &c;
  return nullptr;
}

MainWindow::ChannelData* MainWindow::findChannelMutable(const QString& name) {
  for (auto& c : channels_) if (c.name == name) return &c;
  return nullptr;
}

void MainWindow::addChannelData(const QString& name, bool isArray) {
  if (findChannel(name)) return;
  static const QColor palette[] = {QColor(0x4E9AF1), QColor(0xF1A94E), QColor(0x6BCB77),
                                   QColor(0xE65F5F), QColor(0xB07CE8)};
  ChannelData cd;
  cd.name = name;
  cd.isArray = isArray;
  cd.color = palette[channels_.size() % 5];
  channels_.push_back(std::move(cd));
}

void MainWindow::addChannel(const QString& name, std::vector<Sample> data, int stripHint) {
  addChannelData(name, false);
  ChannelData* c = findChannelMutable(name);
  if (!c) return;
  c->samples = std::move(data);
  c->isArray = false;
  c->strip = stripHint >= 0 ? stripHint : subplotIndex(name);
  if (visibleChannels_.contains(name)) showChannel(name);
}

void MainWindow::showChannel(const QString& name) {
  const ChannelData* c = findChannel(name);
  if (!c || c->isArray) return;
  if (visibleChannels_.contains(name)) return;
  if (plot_->strips().isEmpty()) plot_->addStrip();
  plot_->addCurve(c->strip, name, c->samples, c->color);
  visibleChannels_.insert(name);
  plot_->removeEmptyStrips();
  rebuildStripIndices();
  plot_->autoScaleY();
  plot_->update();
  statusBar()->showMessage("Channel visible: " + name, 2000);
}

void MainWindow::hideChannel(const QString& name) {
  if (!visibleChannels_.contains(name)) return;
  for (auto* s : plot_->strips()) s->removeCurve(name);
  visibleChannels_.remove(name);
  plot_->removeEmptyStrips();
  rebuildStripIndices();
  plot_->autoScaleY();
  statusBar()->showMessage("Channel hidden: " + name, 2000);
}

void MainWindow::rebuildStripIndices() {
  prefixStripMap_.clear();
  const auto strips = plot_->strips();
  for (int i = 0; i < strips.size(); ++i) {
    for (const auto& name : strips[i]->curveNames()) {
      const int slash = name.find('/');
      const QString prefix = slash < 0 ? QString::fromStdString(name) : QString::fromStdString(name).left(slash);
      if (!prefixStripMap_.contains(prefix)) prefixStripMap_.insert(prefix, i);
    }
  }
  for (auto& c : channels_) {
    if (!c.isArray) c.strip = subplotIndex(c.name);
  }
}

void MainWindow::updateVisibleCurves() {
  for (const auto& name : visibleChannels_) {
    const ChannelData* c = findChannel(name);
    if (c && !c->isArray) plot_->setCurveData(name, c->samples);
  }
}

void MainWindow::populateScalarChannelList() {
  if (!channelList_) return;
  QSet<QString> checked;
  for (int i = 0; i < channelList_->count(); ++i) {
    if (channelList_->item(i)->checkState() == Qt::Checked) checked.insert(channelList_->item(i)->text());
  }
  const bool first = channelList_->count() == 0;
  syncingFrameUi_ = true;
  channelList_->clear();
  for (const auto& c : channels_) {
    if (c.isArray) continue;
    auto* it = new QListWidgetItem(c.name, channelList_);
    it->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
    it->setCheckState((first && visibleChannels_.contains(c.name)) || checked.contains(c.name)
                          ? Qt::Checked : Qt::Unchecked);
  }
  syncingFrameUi_ = false;
  for (int i = 0; i < channelList_->count(); ++i) {
    const QString name = channelList_->item(i)->text();
    if (channelList_->item(i)->checkState() == Qt::Checked && !visibleChannels_.contains(name)) showChannel(name);
  }
}

void MainWindow::populateRoiChannelCombo() {
  if (!roiChannel_) return;
  const QString current = roiChannel_->currentText();
  const QSignalBlocker blocker(roiChannel_);
  roiChannel_->clear();
  roiChannel_->addItem("None", QString());
  for (const auto& c : channels_) {
    if (c.isArray) continue;
    roiChannel_->addItem(c.name, c.name);
  }
  const int idx = current.isEmpty() ? 0 : std::max(0, roiChannel_->findText(current));
  roiChannel_->setCurrentIndex(idx);
}

void MainWindow::populateArrayCombo(QComboBox* cb, const QString& selected) {
  if (!cb) return;
  const QSignalBlocker blocker(cb);
  cb->clear();
  cb->addItem("None", QString());
  for (const auto& c : channels_) {
    if (c.isArray) cb->addItem(c.name, c.name);
  }
  const int idx = selected.isEmpty() ? 0 : std::max(0, cb->findText(selected));
  cb->setCurrentIndex(idx);
}

void MainWindow::refreshComboAndTrajectory() {
  if (frames_.empty()) {
    FrameConfig f;
    f.name = "frame_0";
    f.positionChannel = findChannel("pose/position") ? "pose/position" : QString();
    f.orientationChannel = findChannel("pose/euler") ? "pose/euler" :
                           (findChannel("pose/quaternion") ? "pose/quaternion" : QString());
    f.color = QColor(0x4E, 0x9A, 0xF1);
    frames_.push_back(f);
    currentFrame_ = 0;
  }

  FrameConfig* f = selectedFrame();
  if (f) {
    populateArrayCombo(framePos_, f->positionChannel);
    populateArrayCombo(frameOrient_, f->orientationChannel);
  }
  populateScalarChannelList();
  populateRoiChannelCombo();
  syncFrameList();
  syncFrameUiFromConfig();
  buildFrames();
}

bool MainWindow::openDatabase(const QString& path) {
  stopSource();
  stopPlayback();
  if (!reader_.open(path.toStdString())) {
    QMessageBox::warning(this, "rtplot", "Cannot open database: " + path);
    return false;
  }
  source_ = Source::File;
  dbPath_ = path;
  plot_->clear();
  channels_.clear();
  prefixStripMap_.clear();
  visibleChannels_.clear();
  events_ = reader_.events();
  tStart_ = INT64_MAX;
  tEnd_ = 0;
  for (const auto& ci : reader_.channels()) {
    const QString qname = QString::fromStdString(ci.name);
    if (ci.isArray) {
      addChannelData(qname, true);
      ChannelData* c = findChannelMutable(qname);
      if (c) {
        c->arrays = reader_.readArraySamples(ci.name);
        if (!c->arrays.empty()) {
          tStart_ = std::min(tStart_, c->arrays.front().t);
          tEnd_ = std::max(tEnd_, c->arrays.back().t);
        }
      }
    } else {
      auto samples = reader_.readSamples(ci.name);
      if (!samples.empty()) {
        tStart_ = std::min(tStart_, samples.front().t);
        tEnd_ = std::max(tEnd_, samples.back().t);
      }
      addChannel(qname, std::move(samples));
    }
  }
  if (channels_.isEmpty()) { tStart_ = 0; tEnd_ = 1; }
  tCurrent_ = tStart_;
  plot_->setTimeOrigin(tStart_);
  plot_->setEvents(events_);
  plot_->setXRange(static_cast<double>(tStart_), static_cast<double>(tEnd_));
  plot_->autoScaleY();
  refreshComboAndTrajectory();
  trajectory_->setPlayhead(tStart_);

  const double total = (tEnd_ - tStart_) * 1e-9;
  timeLabel_->setText(QString("t=0.000 / %1 s").arg(total, 0, 'f', 3));
  setWindowTitle(QString("rtplot viewer — %1").arg(QFileInfo(path).fileName()));
  statusBar()->showMessage(QString("Opened %1 (%2 channels)").arg(path).arg(channels_.size()), 5000);
  return true;
}

void MainWindow::loadDemoData() {
  stopSource();
  stopPlayback();
  plot_->clear();
  channels_.clear();
  prefixStripMap_.clear();
  visibleChannels_.clear();
  events_.clear();
  dbPath_.clear();
  source_ = Source::None;

  constexpr size_t N = 120000;
  constexpr double dt = 0.001;
  std::vector<Sample> pos, vel, sinWave, tx, ty, tz, qx, qy, qz, qw;
  pos.reserve(N); vel.reserve(N); sinWave.reserve(N);
  tx.reserve(N); ty.reserve(N); tz.reserve(N);
  qx.reserve(N); qy.reserve(N); qz.reserve(N); qw.reserve(N);
  const Timestamp base = nowNs();
  for (size_t i = 0; i < N; ++i) {
    const Timestamp t = base + static_cast<Timestamp>(i * dt * 1e9);
    const double x = i * dt;
    pos.push_back({t, std::sin(2.0 * M_PI * 0.5 * x)});
    vel.push_back({t, 2.0 * M_PI * 0.5 * std::cos(2.0 * M_PI * 0.5 * x)});
    sinWave.push_back({t, std::sin(2.0 * M_PI * 5.0 * x) + 0.1 * std::sin(2.0 * M_PI * 50.0 * x)});
    tx.push_back({t, std::cos(x * 0.2) * 0.8});
    ty.push_back({t, std::sin(x * 0.2) * 0.8});
    tz.push_back({t, x * 0.01});
    const double yaw = x * 0.2;
    qx.push_back({t, 0.0});
    qy.push_back({t, 0.0});
    qz.push_back({t, std::sin(yaw * 0.5)});
    qw.push_back({t, std::cos(yaw * 0.5)});
  }
  tStart_ = pos.front().t;
  tEnd_ = pos.back().t;
  tCurrent_ = tStart_;
  plot_->setTimeOrigin(tStart_);

  std::vector<ArraySample> positions, quats, eulers;
  positions.reserve(N); quats.reserve(N); eulers.reserve(N);
  for (size_t i = 0; i < N; ++i) {
    const Timestamp t = tx[i].t;
    const double yaw = (i * dt) * 0.2;
    ArraySample p; p.t = t; p.size = 3; p.values = {tx[i].v, ty[i].v, tz[i].v};
    ArraySample q; q.t = t; q.size = 4; q.values = {qw[i].v, qx[i].v, qy[i].v, qz[i].v};
    ArraySample e; e.t = t; e.size = 3; e.values = {0.0, 0.0, yaw};
    positions.push_back(p); quats.push_back(q); eulers.push_back(e);
  }
  addChannelData("pose/position", true);
  findChannelMutable("pose/position")->arrays = std::move(positions);
  addChannelData("pose/quaternion", true);
  findChannelMutable("pose/quaternion")->arrays = std::move(quats);
  addChannelData("pose/euler", true);
  findChannelMutable("pose/euler")->arrays = std::move(eulers);

  addChannel("joint1/pos", std::move(pos));
  addChannel("joint1/vel", std::move(vel));
  addChannel("sensor/sin", std::move(sinWave));
  addChannel("traj/x", std::move(tx));
  addChannel("traj/y", std::move(ty));
  addChannel("traj/z", std::move(tz));
  addChannel("pose/qx", std::move(qx));
  addChannel("pose/qy", std::move(qy));
  addChannel("pose/qz", std::move(qz));
  addChannel("pose/qw", std::move(qw));

  for (int i = 0; i < 6; ++i) {
    Event ev;
    ev.t = tStart_ + (tEnd_ - tStart_) * i / 6;
    ev.name = "StateChange";
    ev.payload = (i % 2 == 0) ? "IDLE -> RUNNING" : "RUNNING -> IDLE";
    events_.push_back(ev);
  }
  plot_->setEvents(events_);
  plot_->setXRange(static_cast<double>(tStart_), static_cast<double>(tEnd_));
  plot_->autoScaleY();
  refreshComboAndTrajectory();
  trajectory_->setPlayhead(tStart_);
  timeLabel_->setText(QString("t=0.000 / %1 s").arg((tEnd_ - tStart_) * 1e-9, 0, 'f', 3));
  setWindowTitle("rtplot viewer — demo data (120k samples/channel)");
}

void MainWindow::startPlayback() {
  if (source_ == Source::Shm || source_ == Source::Udp || source_ == Source::Record) return;
  if (channels_.isEmpty()) return;
  if (tCurrent_ >= tEnd_) tCurrent_ = tStart_;
  playing_ = true;
  playbackTimer_->start();
}

void MainWindow::stopPlayback() {
  playing_ = false;
  playbackTimer_->stop();
}

void MainWindow::stepForward() {
  if (channels_.isEmpty()) return;
  stopPlayback();
  const Timestamp step = std::max<Timestamp>(1, (tEnd_ - tStart_) / 10000);
  tCurrent_ = std::min(tEnd_, tCurrent_ + step);
  applyTime(tCurrent_, true);
}

void MainWindow::applyTime(Timestamp t, bool updateSlider) {
  tCurrent_ = t;
  for (const auto& c : channels_) {
    if (c.isArray || !visibleChannels_.contains(c.name)) continue;
    auto end = std::upper_bound(c.samples.begin(), c.samples.end(), Sample{t, 0.0},
                                [](const Sample& a, const Sample& b) { return a.t < b.t; });
    std::vector<Sample> prefix(c.samples.begin(), end);
    plot_->setCurveData(c.name, std::move(prefix));
  }
  if (trajectory_) trajectory_->setPlayhead(t);
  std::vector<Event> evs;
  for (const auto& e : events_) if (e.t <= t) evs.push_back(e);
  plot_->setEvents(evs);
  updatePoseAt(t);
  if (updateSlider) {
    const double f = (tEnd_ > tStart_) ? static_cast<double>(t - tStart_) / (tEnd_ - tStart_) : 0.0;
    slider_->setValue(static_cast<int>(f * 100000));
  }
  const double rel = (t - tStart_) * 1e-9;
  const double total = (tEnd_ - tStart_) * 1e-9;
  timeLabel_->setText(QString("t=%1 / %2 s").arg(rel, 0, 'f', 3).arg(total, 0, 'f', 3));
}

bool MainWindow::writeCsv(const QString& path, const double* t0, const double* t1) {
  struct Col {
    QString name;
    std::vector<Sample> sel;
    size_t idx = 0;
    bool has = false;
    double last = 0.0;
  };
  std::vector<Col> cols;
  cols.reserve(static_cast<size_t>(channels_.size()));
  for (const auto& c : channels_) {
    Col col;
    col.name = c.name;
    if (t0 && t1) {
      auto lo = std::lower_bound(c.samples.begin(), c.samples.end(),
                                 Sample{static_cast<Timestamp>(*t0), 0.0},
                                 [](const Sample& a, const Sample& b) { return a.t < b.t; });
      auto hi = std::upper_bound(c.samples.begin(), c.samples.end(),
                                 Sample{static_cast<Timestamp>(*t1), 0.0},
                                 [](const Sample& a, const Sample& b) { return a.t < b.t; });
      col.sel.assign(lo, hi);
    } else {
      col.sel = c.samples;
    }
    cols.push_back(std::move(col));
  }

  bool any = false;
  for (const auto& c : cols) any = any || !c.sel.empty();
  if (!any) {
    QMessageBox::information(this, "Export CSV", "No samples to export.");
    return false;
  }

  std::ofstream out(path.toStdString(), std::ios::binary);
  if (!out) {
    QMessageBox::warning(this, "Export CSV", "Cannot create file: " + path);
    return false;
  }
  auto escape = [](const QString& s) {
    QString r = s;
    r.replace("\"", "\"\"");
    return r;
  };
  out << "t_ns,t_rel_s";
  for (const auto& c : cols) out << ",\"" << escape(c.name).toStdString() << "\"";
  out << "\n";
  out << std::setprecision(17);

  auto pick = [&]() -> int {
    int best = -1;
    Timestamp bt = 0;
    for (size_t i = 0; i < cols.size(); ++i) {
      if (cols[i].idx >= cols[i].sel.size()) continue;
      const Timestamp t = cols[i].sel[cols[i].idx].t;
      if (best < 0 || t < bt) { best = static_cast<int>(i); bt = t; }
    }
    return best;
  };

  while (true) {
    const int bi = pick();
    if (bi < 0) break;
    const Timestamp t = cols[static_cast<size_t>(bi)].sel[cols[static_cast<size_t>(bi)].idx].t;
    for (auto& c : cols) {
      while (c.idx < c.sel.size() && c.sel[c.idx].t == t) {
        c.last = c.sel[c.idx].v;
        c.has = true;
        ++c.idx;
      }
    }
    out << t << ',' << ((t - tStart_) * 1e-9);
    for (const auto& c : cols) {
      out << ',';
      if (c.has) out << c.last;
    }
    out << "\n";
  }
  out.close();
  return true;
}

void MainWindow::exportCurrent() {
  double roiT0 = 0, roiT1 = 0;
  const bool hasRoi = plot_->currentRoi(&roiT0, &roiT1);

  QString suggested = "export.csv";
  if (hasRoi) {
    suggested = "roi.csv";
  } else if (!dbPath_.isEmpty()) {
    suggested = dbPath_.left(dbPath_.lastIndexOf('.')) + ".csv";
  }
  const QString path = QFileDialog::getSaveFileName(this, hasRoi ? "Export ROI CSV" : "Export CSV",
                                                    suggested, "CSV (*.csv)");
  if (path.isEmpty()) return;

  const bool ok = hasRoi ? writeCsv(path, &roiT0, &roiT1) : writeCsv(path, nullptr, nullptr);
  if (ok) {
    statusBar()->showMessage(QString("%1 CSV written: %2").arg(hasRoi ? "ROI" : "Full").arg(path), 6000);
  }
}

bool MainWindow::startShmSource(const QString& controlName) {
  stopSource();
  stopPlayback();
  plot_->clear();
  channels_.clear();
  prefixStripMap_.clear();
  visibleChannels_.clear();
  events_.clear();
  dbPath_.clear();
  if (!shm_.start(controlName.toStdString())) {
    QMessageBox::warning(this, "SHM source",
        QString("Cannot open /%1. Start a recorder process with shmPublish=true first.").arg(controlName));
    return false;
  }
  source_ = Source::Shm;
  liveTimer_->start();
  statusBar()->showMessage(QString("Live SHM source running: /%1").arg(controlName), 4000);
  setWindowTitle(QString("rtplot viewer — SHM /%1").arg(controlName));
  return true;
}

bool MainWindow::startUdpSource(uint16_t port) {
  stopSource();
  stopPlayback();
  plot_->clear();
  channels_.clear();
  prefixStripMap_.clear();
  visibleChannels_.clear();
  events_.clear();
  dbPath_.clear();
  if (!udp_.start(port, true)) {
    QMessageBox::warning(this, "UDP source", QString("Cannot bind UDP port %1").arg(port));
    return false;
  }
  source_ = Source::Udp;
  liveTimer_->start();
  statusBar()->showMessage(QString("Live UDP source running on port %1").arg(port), 4000);
  setWindowTitle(QString("rtplot viewer — UDP :%1").arg(port));
  return true;
}

bool MainWindow::startRecordSource(const QString& dbPath, double durationSeconds) {
  stopSource();
  stopPlayback();
  plot_->clear();
  channels_.clear();
  prefixStripMap_.clear();
  visibleChannels_.clear();
  events_.clear();
  dbPath_ = dbPath;
  recordElapsed_ = 0.0;
  recordDuration_ = std::max(0.1, durationSeconds);
  nextRecordEvent_ = 2.0;
  recordBase_ = nowNs();

  LoggerConfig cfg;
  cfg.persist = true;
  cfg.dbPath = dbPath.toStdString();
  cfg.shmPublish = false;   // direct mode: UI is in the same process
  cfg.udpPublish = false;
  cfg.flushIntervalMs = 5;
  cfg.ringCapacity = 1u << 16;
  auto& logger = Logger::instance();
  if (logger.running()) logger.stop();
  if (!logger.start(cfg)) {
    QMessageBox::warning(this, "Record source", "Cannot start Logger for direct recording.");
    return false;
  }

  // Pre-register curves so the subplot layout is available immediately.
  addChannel("joint1/pos", {});
  addChannel("joint1/vel", {});
  addChannel("sensor/sin", {});
  addChannel("traj/x", {});
  addChannel("traj/y", {});
  addChannel("traj/z", {});
  addChannel("pose/qx", {});
  addChannel("pose/qy", {});
  addChannel("pose/qz", {});
  addChannel("pose/qw", {});
  addChannelData("pose/position", true);
  addChannelData("pose/quaternion", true);
  addChannelData("pose/euler", true);
  refreshComboAndTrajectory();
  tStart_ = recordBase_;
  tEnd_ = recordBase_ + 1;
  tCurrent_ = recordBase_;
  plot_->setTimeOrigin(tStart_);
  plot_->setXRange(static_cast<double>(tStart_), static_cast<double>(tStart_ + 1000000000LL));
  trajectory_->setPlayhead(tStart_);

  source_ = Source::Record;
  recordTimer_->start();
  statusBar()->showMessage(QString("Direct record source: %1 (%2 s)").arg(dbPath).arg(recordDuration_), 5000);
  setWindowTitle(QString("rtplot viewer — direct record %1").arg(QFileInfo(dbPath).fileName()));
  return true;
}

void MainWindow::updateRecordSource() {
  if (source_ != Source::Record || recordElapsed_ >= recordDuration_) return;

  auto append = [this](const QString& name, Timestamp t, double v) {
    for (auto& c : channels_) {
      if (c.name == name) {
        c.samples.push_back({t, v});
        if (visibleChannels_.contains(name)) plot_->appendCurveData(name, {{t, v}});
        return;
      }
    }
    addChannel(name, {{t, v}});
  };
  auto appendArray = [this](const QString& name, Timestamp t, std::initializer_list<double> vals) {
    ChannelData* c = findChannelMutable(name);
    if (!c) { addChannelData(name, true); c = findChannelMutable(name); }
    ArraySample a;
    a.t = t;
    a.size = static_cast<uint32_t>(vals.size());
    size_t i = 0;
    for (double v : vals) a.values[i++] = v;
    c->arrays.push_back(a);
  };

  auto& logger = Logger::instance();
  for (int i = 0; i < kRecordSamplesPerTick && recordElapsed_ < recordDuration_; ++i) {
    recordElapsed_ += kRecordSampleDt;
    const Timestamp t = recordBase_ + static_cast<Timestamp>(recordElapsed_ * 1e9);
    const double x = recordElapsed_;
    const double yaw = x * 0.2;
    append("joint1/pos", t, std::sin(2.0 * M_PI * 0.5 * x));
    append("joint1/vel", t, 2.0 * M_PI * 0.5 * std::cos(2.0 * M_PI * 0.5 * x));
    append("sensor/sin", t, std::sin(2.0 * M_PI * 5.0 * x));
    append("traj/x", t, std::cos(yaw) * 0.8);
    append("traj/y", t, std::sin(yaw) * 0.8);
    append("traj/z", t, x * 0.01);
    append("pose/qx", t, 0.0);
    append("pose/qy", t, 0.0);
    append("pose/qz", t, std::sin(yaw * 0.5));
    append("pose/qw", t, std::cos(yaw * 0.5));

    logger.log("joint1/pos", std::sin(2.0 * M_PI * 0.5 * x), t);
    logger.log("joint1/vel", 2.0 * M_PI * 0.5 * std::cos(2.0 * M_PI * 0.5 * x), t);
    logger.log("sensor/sin", std::sin(2.0 * M_PI * 5.0 * x), t);
    logger.log("traj/x", std::cos(yaw) * 0.8, t);
    logger.log("traj/y", std::sin(yaw) * 0.8, t);
    logger.log("traj/z", x * 0.01, t);
    logger.log("pose/qx", 0.0, t);
    logger.log("pose/qy", 0.0, t);
    logger.log("pose/qz", std::sin(yaw * 0.5), t);
    logger.log("pose/qw", std::cos(yaw * 0.5), t);

    appendArray("pose/position", t, {std::cos(yaw) * 0.8, std::sin(yaw) * 0.8, x * 0.01});
    appendArray("pose/quaternion", t, {std::cos(yaw * 0.5), 0.0, 0.0, std::sin(yaw * 0.5)});
    appendArray("pose/euler", t, {0.0, 0.0, yaw});
    double posv[3] = {std::cos(yaw) * 0.8, std::sin(yaw) * 0.8, x * 0.01};
    double quatv[4] = {std::cos(yaw * 0.5), 0.0, 0.0, std::sin(yaw * 0.5)};
    double eulerv[3] = {0.0, 0.0, yaw};
    logger.logArray("pose/position", posv, 3, t);
    logger.logArray("pose/quaternion", quatv, 4, t);
    logger.logArray("pose/euler", eulerv, 3, t);

    if (recordElapsed_ >= nextRecordEvent_ - kRecordSampleDt * 0.5) {
      events_.push_back(Event{t, "StateChange",
                              (static_cast<int>(recordElapsed_) % 4 < 2) ? "IDLE -> RUNNING"
                                                                         : "RUNNING -> IDLE"});
      logger.event("StateChange", events_.back().payload, t);
      nextRecordEvent_ += 2.0;
    }
  }

  tEnd_ = recordBase_ + static_cast<Timestamp>(recordElapsed_ * 1e9);
  tCurrent_ = tEnd_;
  if (trajectory_) trajectory_->setPlayhead(tEnd_);
  if (recordElapsed_ - lastFrameBuildTime_ >= 0.2) {
    buildFrames();
    lastFrameBuildTime_ = recordElapsed_;
  }
  plot_->setEvents(events_);
  plot_->setXRange(static_cast<double>(tStart_), static_cast<double>(tEnd_));
  plot_->autoScaleY();
  timeLabel_->setText(QString("t=%1 / %2 s").arg(recordElapsed_, 0, 'f', 3).arg(recordDuration_, 0, 'f', 3));
  slider_->setValue(static_cast<int>(recordElapsed_ / recordDuration_ * 100000));
  updatePoseAt(tCurrent_);

  if (recordElapsed_ >= recordDuration_) {
    recordTimer_->stop();
    Logger::instance().flushAndStop();
    source_ = Source::None;
    if (!openDatabase(dbPath_)) {
      statusBar()->showMessage(QString("Recording finished: %1").arg(dbPath_), 8000);
    }
  }
}

void MainWindow::updateLiveShm() {
  if (source_ != Source::Shm) return;
  const auto discovered = shm_.discover();
  for (const auto& d : discovered) {
    const QString qname = QString::fromStdString(d.name);
    if (d.isArray) {
      std::vector<ArraySample> achunk;
      if (shm_.readArray(d.name, achunk, 1u << 16) == 0) continue;
      bool isNew = false;
      ChannelData* c = findChannelMutable(qname);
      if (!c) { addChannelData(qname, true); c = findChannelMutable(qname); isNew = true; }
      c->arrays.insert(c->arrays.end(), achunk.begin(), achunk.end());
      if (c->arrays.size() > kMaxLiveSamples) {
        c->arrays.erase(c->arrays.begin(), c->arrays.end() - kMaxLiveSamples);
      }
      // Only rebuild the sidebar once when the channel is first discovered.
      // Rebuilding on every SHM packet makes checkboxes flicker continuously.
      if (isNew) refreshComboAndTrajectory();
      continue;
    }
    std::vector<Sample> chunk;
    if (shm_.read(d.name, chunk, 1u << 16) == 0) continue;
    bool found = false;
    for (auto& c : channels_) {
      if (c.name == qname) {
        c.samples.insert(c.samples.end(), chunk.begin(), chunk.end());
        if (c.samples.size() > 1 && c.samples.back().t < c.samples[c.samples.size() - 2].t) {
          std::sort(c.samples.begin(), c.samples.end(),
                    [](const Sample& a, const Sample& b) { return a.t < b.t; });
        }
        if (c.samples.size() > kMaxLiveSamples) {
          c.samples.erase(c.samples.begin(), c.samples.end() - kMaxLiveSamples);
        }
        if (visibleChannels_.contains(qname)) plot_->appendCurveData(qname, chunk);
        found = true;
        break;
      }
    }
    if (!found) { addChannel(qname, chunk); refreshComboAndTrajectory(); }
  }
  if (channels_.isEmpty()) return;
  bool first = true;
  for (const auto& c : channels_) {
    if (c.samples.empty()) continue;
    if (first) { tStart_ = c.samples.front().t; tEnd_ = c.samples.back().t; first = false; }
    else {
      tStart_ = std::min(tStart_, c.samples.front().t);
      tEnd_ = std::max(tEnd_, c.samples.back().t);
    }
  }
  plot_->setTimeOrigin(tStart_);
  plot_->setXRange(static_cast<double>(tEnd_ - 10000000000LL), static_cast<double>(tEnd_ + 1000000LL));
  plot_->autoScaleY();
  if (trajectory_) trajectory_->setPlayhead(tEnd_);
  if (lastFrameBuildTime_ < 0 || (tEnd_ - lastFrameBuildTime_ * 1e9) > 200000000LL) {
    buildFrames();
    lastFrameBuildTime_ = tEnd_ * 1e-9;
  }
  timeLabel_->setText(QString("live t=%1 s").arg((tEnd_ - tStart_) * 1e-9, 0, 'f', 3));
}

void MainWindow::updateLiveUdp() {
  if (source_ != Source::Udp) return;
  std::string channel;
  std::vector<Sample> chunk;
  for (int i = 0; i < 500; ++i) {
    if (!udp_.receive(channel, chunk)) break;
    const QString qname = QString::fromStdString(channel);
    bool found = false;
    for (auto& c : channels_) {
      if (c.name == qname) {
        c.samples.insert(c.samples.end(), chunk.begin(), chunk.end());
        if (c.samples.size() > 1 && c.samples.back().t < c.samples[c.samples.size() - 2].t) {
          std::sort(c.samples.begin(), c.samples.end(),
                    [](const Sample& a, const Sample& b) { return a.t < b.t; });
        }
        if (c.samples.size() > kMaxLiveSamples) {
          c.samples.erase(c.samples.begin(), c.samples.end() - kMaxLiveSamples);
        }
        if (visibleChannels_.contains(qname)) plot_->appendCurveData(qname, chunk);
        found = true;
        break;
      }
    }
    if (!found) { addChannel(qname, chunk); refreshComboAndTrajectory(); }
  }
  if (channels_.isEmpty()) return;
  bool first = true;
  for (const auto& c : channels_) {
    if (c.samples.empty()) continue;
    if (first) { tStart_ = c.samples.front().t; tEnd_ = c.samples.back().t; first = false; }
    else {
      tStart_ = std::min(tStart_, c.samples.front().t);
      tEnd_ = std::max(tEnd_, c.samples.back().t);
    }
  }
  plot_->setTimeOrigin(tStart_);
  plot_->setXRange(static_cast<double>(tEnd_ - 10000000000LL), static_cast<double>(tEnd_ + 1000000LL));
  plot_->autoScaleY();
  if (trajectory_) trajectory_->setPlayhead(tEnd_);
  if (lastFrameBuildTime_ < 0 || (tEnd_ - lastFrameBuildTime_ * 1e9) > 200000000LL) {
    buildFrames();
    lastFrameBuildTime_ = tEnd_ * 1e-9;
  }
  timeLabel_->setText(QString("live t=%1 s").arg((tEnd_ - tStart_) * 1e-9, 0, 'f', 3));
}

MainWindow::FrameConfig* MainWindow::selectedFrame() {
  return (currentFrame_ >= 0 && currentFrame_ < static_cast<int>(frames_.size()))
             ? &frames_[static_cast<size_t>(currentFrame_)] : nullptr;
}
const MainWindow::FrameConfig* MainWindow::selectedFrame() const {
  return (currentFrame_ >= 0 && currentFrame_ < static_cast<int>(frames_.size()))
             ? &frames_[static_cast<size_t>(currentFrame_)] : nullptr;
}

void MainWindow::syncFrameList() {
  if (!frameList_) return;
  const QSignalBlocker blocker(frameList_);
  frameList_->clear();
  for (const auto& f : frames_) frameList_->addItem(f.name);
  if (currentFrame_ >= 0 && currentFrame_ < static_cast<int>(frames_.size())) {
    frameList_->setCurrentRow(currentFrame_);
  }
}

void MainWindow::syncFrameUiFromConfig() {
  const FrameConfig* f = selectedFrame();
  syncingFrameUi_ = true;
  if (!f) {
    frameNameEdit_->clear();
    framePos_->setCurrentIndex(0);
    frameOrient_->setCurrentIndex(0);
    frameOrientInfo_->setText("Orientation: None");
    syncingFrameUi_ = false;
    return;
  }
  frameNameEdit_->setText(f->name);
  const QString none = QStringLiteral("None");
  framePos_->setCurrentIndex(std::max(0, framePos_->findText(f->positionChannel.isEmpty() ? none : f->positionChannel)));
  frameOrient_->setCurrentIndex(std::max(0, frameOrient_->findText(f->orientationChannel.isEmpty() ? none : f->orientationChannel)));
  syncingFrameUi_ = false;

  const ChannelData* oc = f->orientationChannel.isEmpty() ? nullptr : findChannel(f->orientationChannel);
  if (oc && oc->isArray && !oc->arrays.empty()) {
    const uint32_t len = oc->arrays.front().size;
    if (len == 3) frameOrientInfo_->setText("Detected: Euler (RPY), length=3");
    else if (len == 4) frameOrientInfo_->setText("Detected: Quaternion (w,x,y,z), length=4");
    else frameOrientInfo_->setText(QString("Detected: unsupported length=%1").arg(len));
  } else {
    frameOrientInfo_->setText("Orientation: None");
  }
}

static QString comboChannel(QComboBox* cb) {
  if (!cb || cb->currentIndex() <= 0) return QString();
  return cb->currentText();
}

void MainWindow::updateFrameConfigFromUi() {
  if (syncingFrameUi_) return;
  FrameConfig* f = selectedFrame();
  if (!f) return;
  f->name = frameNameEdit_->text().trimmed();
  if (f->name.isEmpty()) f->name = QString("frame_%1").arg(currentFrame_);
  f->positionChannel = comboChannel(framePos_);
  f->orientationChannel = comboChannel(frameOrient_);
  if (frameList_ && currentFrame_ >= 0 && currentFrame_ < frameList_->count()) {
    const QSignalBlocker blocker(frameList_);
    frameList_->item(currentFrame_)->setText(f->name);
  }
  syncFrameUiFromConfig();
  buildFrames();
}

void MainWindow::addFrame() {
  static const QColor palette[] = {QColor(0x4E9AF1), QColor(0xF1A94E), QColor(0x6BCB77),
                                   QColor(0xE65F5F), QColor(0xB07CE8), QColor(0x00B8A9)};
  FrameConfig f;
  f.name = QString("frame_%1").arg(frames_.size());
  f.positionChannel = findChannel("pose/position") ? "pose/position" : QString();
  f.orientationChannel = findChannel("pose/euler") ? "pose/euler" :
                         (findChannel("pose/quaternion") ? "pose/quaternion" : QString());
  f.color = palette[frames_.size() % 6];
  frames_.push_back(f);
  currentFrame_ = static_cast<int>(frames_.size()) - 1;
  refreshComboAndTrajectory();
}

void MainWindow::removeSelectedFrame() {
  if (currentFrame_ < 0 || currentFrame_ >= static_cast<int>(frames_.size())) return;
  frames_.erase(frames_.begin() + currentFrame_);
  if (frames_.empty()) {
    currentFrame_ = -1;
    trajectory_->setFrames({});
    poseSamples_.clear();
  } else {
    currentFrame_ = std::min<int>(currentFrame_, static_cast<int>(frames_.size()) - 1);
  }
  syncFrameList();
  syncFrameUiFromConfig();
  buildFrames();
}

void MainWindow::buildFrames() {
  if (!trajectory_) return;
  // Defer the expensive 3D frame construction until the 3D/Pose tab is active.
  if (tabs_ && tabs_->currentIndex() != 1) {
    trajectoryNeedsFit_ = true;
    return;
  }
  constexpr size_t kMaxFramePoints = 10000;

  auto eulerToQuat = [](double r, double p, double y, double* q) {
    const double cr = std::cos(r * 0.5), sr = std::sin(r * 0.5);
    const double cp = std::cos(p * 0.5), sp = std::sin(p * 0.5);
    const double cy = std::cos(y * 0.5), sy = std::sin(y * 0.5);
    q[0] = cr * cp * cy + sr * sp * sy;
    q[1] = sr * cp * cy - cr * sp * sy;
    q[2] = cr * sp * cy + sr * cp * sy;
    q[3] = cr * cp * sy - sr * sp * cy;
  };

  std::vector<PoseFrame> out;
  poseSamples_.clear();
  for (const auto& cfg : frames_) {
    const ChannelData* pc = cfg.positionChannel.isEmpty() ? nullptr : findChannel(cfg.positionChannel);
    const ChannelData* oc = cfg.orientationChannel.isEmpty() ? nullptr : findChannel(cfg.orientationChannel);
    const bool hasPos = pc && pc->isArray && !pc->arrays.empty() && pc->arrays.front().size >= 3;
    const bool hasOrient = oc && oc->isArray && !oc->arrays.empty() &&
                           (oc->arrays.front().size == 3 || oc->arrays.front().size == 4);
    if (!hasPos && !hasOrient) continue;

    const std::vector<ArraySample>* timeline = hasPos ? &pc->arrays : &oc->arrays;
    if (timeline->empty()) continue;
    const size_t step = std::max<size_t>(1, timeline->size() / kMaxFramePoints);

    PoseFrame pf;
    pf.name = cfg.name;
    pf.color = cfg.color;
    pf.showPath = hasPos;
    pf.showPose = hasOrient;
    const size_t nSamples = (timeline->size() + step - 1) / step;
    if (hasPos) {
      pf.path.reserve(nSamples);
      pf.pathTimes.reserve(nSamples);
      for (size_t i = 0; i < timeline->size(); i += step) {
        const ArraySample& a = (*timeline)[i];
        pf.pathTimes.push_back(a.t);
        pf.path.push_back({a[0], a[1], a[2]});
      }
    }
    if (hasOrient) {
      const bool quat = oc->arrays.front().size == 4;
      pf.poses.reserve(nSamples);
      size_t posIdx = 0;
      size_t orientIdx = 0;
      for (size_t i = 0; i < timeline->size(); i += step) {
        const ArraySample& a = (*timeline)[i];
        double q[4] = {1, 0, 0, 0};
        if (hasPos) {
          // Position and orientation are independent array channels. Resample
          // the orientation channel at the same timestamp as the position.
          while (orientIdx + 1 < oc->arrays.size() && oc->arrays[orientIdx + 1].t <= a.t) ++orientIdx;
          const ArraySample& o = oc->arrays[orientIdx];
          if (quat) { q[0] = o[0]; q[1] = o[1]; q[2] = o[2]; q[3] = o[3]; }
          else eulerToQuat(o[0], o[1], o[2], q);
        } else {
          if (quat) { q[0] = a[0]; q[1] = a[1]; q[2] = a[2]; q[3] = a[3]; }
          else eulerToQuat(a[0], a[1], a[2], q);
        }
        // Normalize the quaternion so the three body axes stay orthonormal.
        const double qn = std::sqrt(q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3]);
        if (qn > 1e-12) { for (int k = 0; k < 4; ++k) q[k] /= qn; }
        else { q[0] = 1.0; q[1] = q[2] = q[3] = 0.0; }
        double x = 0, y = 0, z = 0;
        if (hasPos) {
          while (posIdx + 1 < pc->arrays.size() && pc->arrays[posIdx + 1].t <= a.t) ++posIdx;
          x = pc->arrays[posIdx][0]; y = pc->arrays[posIdx][1]; z = pc->arrays[posIdx][2];
        }
        pf.poses.push_back(PoseSample{a.t, x, y, z, q[0], q[1], q[2], q[3]});
      }
      if (poseSamples_.empty()) poseSamples_ = pf.poses;
    }
    out.push_back(std::move(pf));
  }
  trajectory_->setFrames(out, trajectoryNeedsFit_);
  trajectoryNeedsFit_ = false;
}

void MainWindow::updateRoiSidebar() {
  if (!roiStatsEdit_) return;
  double t0 = 0, t1 = 0;
  if (!plot_ || !plot_->currentRoi(&t0, &t1)) {
    roiStatsEdit_->setPlainText("No active ROI.\nRight-drag on the time-series plot to create one.");
    return;
  }
  const QString name = roiChannel_ && roiChannel_->currentIndex() > 0 ? roiChannel_->currentText() : QString();
  const ChannelData* c = name.isEmpty() ? nullptr : findChannel(name);
  if (!c) {
    roiStatsEdit_->setPlainText("Select a scalar channel for ROI statistics.");
    return;
  }
  auto lo = std::lower_bound(c->samples.begin(), c->samples.end(), Sample{static_cast<Timestamp>(t0), 0.0},
                             [](const Sample& a, const Sample& b) { return a.t < b.t; });
  auto hi = std::upper_bound(c->samples.begin(), c->samples.end(), Sample{static_cast<Timestamp>(t1), 0.0},
                             [](const Sample& a, const Sample& b) { return a.t < b.t; });
  uint64_t n = 0;
  double min = std::numeric_limits<double>::infinity();
  double max = -std::numeric_limits<double>::infinity();
  double sum = 0, sum2 = 0;
  for (auto it = lo; it != hi; ++it) {
    ++n; min = std::min(min, it->v); max = std::max(max, it->v);
    sum += it->v; sum2 += it->v * it->v;
  }
  QString text = QString("ROI [%1, %2] s\nChannel: %3\n")
                     .arg((t0 - tStart_) * 1e-9, 0, 'f', 6)
                     .arg((t1 - tStart_) * 1e-9, 0, 'f', 6)
                     .arg(name);
  if (n == 0) {
    text += "No samples in this ROI.";
  } else {
    const double mean = sum / static_cast<double>(n);
    const double var = n > 1 ? (sum2 - sum * mean) / static_cast<double>(n - 1) : 0.0;
    text += QString("n=%1\nmean=%2\nstd=%3\nmin=%4\nmax=%5")
                .arg(n).arg(mean, 0, 'g', 8).arg(std::sqrt(std::max(0.0, var)), 0, 'g', 8)
                .arg(min, 0, 'g', 8).arg(max, 0, 'g', 8);
  }
  roiStatsEdit_->setPlainText(text);
}

void MainWindow::updatePoseAt(Timestamp t) {
  if (poseSamples_.empty()) return;
  auto it = std::lower_bound(poseSamples_.begin(), poseSamples_.end(), t,
                             [](const PoseSample& p, Timestamp t) { return p.t < t; });
  size_t idx = static_cast<size_t>(it - poseSamples_.begin());
  trajectory_->setCurrentPose(idx < poseSamples_.size() ? idx : poseSamples_.size() - 1);
}

} // namespace rtplot::gui
