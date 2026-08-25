#pragma once
#include "rtplot/types.hpp"

#include <QColor>
#include <QWidget>

#include <array>
#include <vector>

namespace rtplot::gui {

struct Vec3 { double x = 0, y = 0, z = 0; };
struct PoseSample {
  Timestamp t = 0;
  double x = 0, y = 0, z = 0;
  double qw = 1, qx = 0, qy = 0, qz = 0;
};

struct PoseFrame {
  QString name;
  QColor color = QColor(0x4E, 0x9A, 0xF1);
  std::vector<Vec3> path;
  std::vector<Timestamp> pathTimes;
  std::vector<PoseSample> poses;
  bool showPath = true;
  bool showPose = true;
};

/// Lightweight software-rendered 3D trajectory/pose view. Supports mouse
/// orbit, pan, wheel zoom and draws an orientation triad for quaternion poses.
class TrajectoryWidget : public QWidget {
public:
  explicit TrajectoryWidget(QWidget* parent = nullptr);

  void setTrajectory(std::vector<Vec3> path);
  void setPoseSamples(std::vector<PoseSample> poses);
  void setFrames(std::vector<PoseFrame> frames, bool refit = true);
  void setPlayhead(Timestamp t);
  void setCurrentPose(size_t index);
  void clear();

protected:
  void paintEvent(QPaintEvent*) override;
  void mousePressEvent(QMouseEvent*) override;
  void mouseMoveEvent(QMouseEvent*) override;
  void mouseReleaseEvent(QMouseEvent*) override;
  void wheelEvent(QWheelEvent*) override;

private:
  QPointF project(double x, double y, double z) const;
  void drawAxis(QPainter& p, const std::array<double, 3>& origin,
                const std::array<double, 3>& xa, const std::array<double, 3>& ya,
                const std::array<double, 3>& za, double scale, bool world = false);
  void fit();

  std::vector<Vec3> path_;
  std::vector<PoseSample> poses_;
  std::vector<PoseFrame> frames_;
  size_t currentPose_ = 0;
  Timestamp playhead_ = 0;
  bool hasPlayhead_ = false;
  double yaw_ = -0.7, pitch_ = 0.45, distance_ = 4.0;
  QPointF target_ = {0, 0};
  QPointF lastMouse_;
  QPointF lastPan_;
  bool rotating_ = false, panning_ = false;
  double cx_ = 0, cy_ = 0, cz_ = 0;
  double maxRadius_ = 1.0;
};

} // namespace rtplot::gui
