#include "TrajectoryWidget.hpp"

#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace rtplot::gui {

TrajectoryWidget::TrajectoryWidget(QWidget* parent) : QWidget(parent) {
  setMinimumSize(320, 240);
  setMouseTracking(true);
}

void TrajectoryWidget::clear() {
  path_.clear();
  poses_.clear();
  frames_.clear();
  currentPose_ = 0;
  hasPlayhead_ = false;
  playhead_ = 0;
  update();
}

void TrajectoryWidget::fit() {
  double minx = 1e300, miny = 1e300, minz = 1e300;
  double maxx = -1e300, maxy = -1e300, maxz = -1e300;
  auto include = [&](double x, double y, double z) {
    minx = std::min(minx, x); maxx = std::max(maxx, x);
    miny = std::min(miny, y); maxy = std::max(maxy, y);
    minz = std::min(minz, z); maxz = std::max(maxz, z);
  };
  for (const auto& v : path_) include(v.x, v.y, v.z);
  for (const auto& p : poses_) include(p.x, p.y, p.z);
  for (const auto& f : frames_) {
    for (const auto& v : f.path) include(v.x, v.y, v.z);
    for (const auto& p : f.poses) include(p.x, p.y, p.z);
  }
  if (minx > maxx) { minx = maxx = miny = maxy = minz = maxz = 0.0; }
  cx_ = (minx + maxx) * 0.5;
  cy_ = (miny + maxy) * 0.5;
  cz_ = (minz + maxz) * 0.5;
  const double dx = maxx - minx, dy = maxy - miny, dz = maxz - minz;
  maxRadius_ = std::max({dx, dy, dz}) * 0.7;
  if (maxRadius_ < 1e-6) {
    // Pose-only data (no trajectory) should still render a sane-sized frame.
    maxRadius_ = 1.0;
  }
  distance_ = maxRadius_ * 2.6;
  update();
}

void TrajectoryWidget::setTrajectory(std::vector<Vec3> path) {
  path_ = std::move(path);
  frames_.clear();
  fit();
}
void TrajectoryWidget::setPoseSamples(std::vector<PoseSample> poses) {
  poses_ = std::move(poses);
  frames_.clear();
  currentPose_ = 0;
  fit();
}
void TrajectoryWidget::setFrames(std::vector<PoseFrame> frames, bool refit) {
  path_.clear();
  poses_.clear();
  frames_ = std::move(frames);
  if (refit) { currentPose_ = 0; fit(); }
  else update();
}
void TrajectoryWidget::setPlayhead(Timestamp t) {
  hasPlayhead_ = true;
  playhead_ = t;
  update();
}
void TrajectoryWidget::setCurrentPose(size_t index) {
  currentPose_ = index;
  update();
}

QPointF TrajectoryWidget::project(double x, double y, double z) const {
  const double X = x - cx_, Y = y - cy_, Z = z - cz_;
  const double cy = std::cos(yaw_), sy = std::sin(yaw_);
  const double cp = std::cos(pitch_), sp = std::sin(pitch_);
  double x1 = cy * X - sy * Y;
  double y1 = sy * X + cy * Y;
  double z1 = Z;
  double x2 = x1;
  double up = sp * y1 + cp * z1;
  // Orthographic projection: keeps object axes equal-length and mutually
  // perpendicular on screen, which makes quaternion/Euler orientations easier
  // to verify visually.
  const double f = std::min(width(), height()) * 0.55 / std::max(1e-6, distance_);
  return QPointF(width() * 0.5 + target_.x() + x2 * f,
                 height() * 0.5 + target_.y() - up * f);
}

void TrajectoryWidget::drawAxis(QPainter& p, const std::array<double, 3>& o,
                                const std::array<double, 3>& xa,
                                const std::array<double, 3>& ya,
                                const std::array<double, 3>& za, double s,
                                bool world) {
  QPointF oo = project(o[0], o[1], o[2]);
  const QColor red = world ? QColor(0x88, 0x44, 0x44) : QColor(0xE5, 0x5A, 0x5A);
  const QColor green = world ? QColor(0x44, 0x88, 0x44) : QColor(0x5A, 0xC8, 0x5A);
  const QColor blue = world ? QColor(0x44, 0x44, 0x88) : QColor(0x5A, 0x8A, 0xE5);
  struct Axis { QPointF q; QColor c; const char* label; };
  const Axis axes[] = {
      {project(o[0] + xa[0] * s, o[1] + xa[1] * s, o[2] + xa[2] * s), red, "x"},
      {project(o[0] + ya[0] * s, o[1] + ya[1] * s, o[2] + ya[2] * s), green, "y"},
      {project(o[0] + za[0] * s, o[1] + za[1] * s, o[2] + za[2] * s), blue, "z"}};
  for (int i = 0; i < 3; ++i) {
    p.setPen(QPen(axes[i].c, world ? 1 : 2));
    p.drawLine(oo, axes[i].q);
    p.setPen(axes[i].c);
    p.drawText(axes[i].q + QPointF(4, -4), axes[i].label);
  }
}

void TrajectoryWidget::paintEvent(QPaintEvent*) {
  QPainter p(this);
  p.fillRect(rect(), QColor(0x12, 0x14, 0x1A));
  p.setRenderHint(QPainter::Antialiasing, true);

  const double s = maxRadius_;
  // Blue translucent ground grid on the z=0 plane. Lines are placed at
  // integer multiples of a fixed step so one grid line always passes through
  // the origin (x=0 and y=0).
  const double step = std::max(s * 0.1, distance_ * 0.05);
  const int gridN = 20;
  const double gridR = static_cast<double>(gridN) * step;
  p.setPen(QPen(QColor(60, 130, 255, 70), 1));
  for (int i = -gridN; i <= gridN; ++i) {
    const double t = static_cast<double>(i) * step;
    p.drawLine(project(t, -gridR, 0), project(t, gridR, 0));
    p.drawLine(project(-gridR, t, 0), project(gridR, t, 0));
  }
  drawAxis(p, {0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}, s, true);

  auto drawPath = [&](const std::vector<Vec3>& path, const std::vector<Timestamp>& times,
                       const QColor& color) {
    if (path.size() < 2) return;
    size_t visible = path.size();
    if (hasPlayhead_ && times.size() == path.size()) {
      visible = 1;
      while (visible < path.size() && times[visible] <= playhead_) ++visible;
    }
    if (visible < 2) return;
    QPolygonF poly;
    poly.reserve(static_cast<int>(std::min<size_t>(visible, 4096)));
    const size_t step = std::max<size_t>(1, visible / 4096);
    for (size_t i = 0; i < visible; i += step) {
      poly.append(project(path[i].x, path[i].y, path[i].z));
    }
    if ((visible - 1) % step != 0) poly.append(project(path[visible - 1].x, path[visible - 1].y, path[visible - 1].z));
    p.setPen(QPen(color, 1.8));
    p.drawPolyline(poly);
  };

  auto drawPose = [&](const PoseSample& pose, const QColor& color) {
    const double qw = pose.qw, qx = pose.qx, qy = pose.qy, qz = pose.qz;
    const double xa[3] = {1 - 2*(qy*qy + qz*qz), 2*(qx*qy + qw*qz), 2*(qx*qz - qw*qy)};
    const double ya[3] = {2*(qx*qy - qw*qz), 1 - 2*(qx*qx + qz*qz), 2*(qy*qz + qw*qx)};
    const double za[3] = {2*(qx*qz + qw*qy), 2*(qy*qz - qw*qx), 1 - 2*(qx*qx + qy*qy)};
    drawAxis(p, {pose.x, pose.y, pose.z}, {xa[0], xa[1], xa[2]},
             {ya[0], ya[1], ya[2]}, {za[0], za[1], za[2]}, s * 0.35);
    const QPointF oo = project(pose.x, pose.y, pose.z);
    p.setPen(color);
    p.drawEllipse(oo, 4, 4);
  };

  auto poseIndex = [&](const std::vector<PoseSample>& poses) {
    size_t idx = currentPose_ < poses.size() ? currentPose_ : 0;
    if (hasPlayhead_ && !poses.empty()) {
      idx = 0;
      while (idx + 1 < poses.size() && poses[idx + 1].t <= playhead_) ++idx;
    }
    return idx;
  };

  drawPath(path_, {}, QColor(0x4E, 0x9A, 0xF1));
  if (!poses_.empty()) drawPose(poses_[poseIndex(poses_)], QColor(0x4E, 0x9A, 0xF1));

  for (const auto& f : frames_) {
    if (f.showPath) drawPath(f.path, f.pathTimes, f.color);
    if (f.showPose && !f.poses.empty()) {
      const size_t idx = poseIndex(f.poses);
      drawPose(f.poses[idx], f.color);
      const PoseSample& pose = f.poses[idx];
      p.setPen(f.color);
      const QPointF oo = project(pose.x, pose.y, pose.z);
      p.drawText(oo + QPointF(6, -6), f.name);
    }
  }

  p.setPen(QColor(0xA0, 0xAA, 0xBB));
  p.drawText(12, 18, QString("yaw/pitch: drag left | pan: drag middle | zoom: wheel"));
}

void TrajectoryWidget::mousePressEvent(QMouseEvent* e) {
  lastMouse_ = e->pos();
  if (e->button() == Qt::LeftButton) { rotating_ = true; }
  else if (e->button() == Qt::MiddleButton) { panning_ = true; lastPan_ = e->pos(); }
}
void TrajectoryWidget::mouseMoveEvent(QMouseEvent* e) {
  if (rotating_) {
    yaw_ += (e->pos().x() - lastMouse_.x()) * 0.008;
    pitch_ = std::max(-1.45, std::min(1.45, pitch_ + (e->pos().y() - lastMouse_.y()) * 0.008));
    lastMouse_ = e->pos();
    update();
  } else if (panning_) {
    target_ += (e->pos() - lastPan_);
    lastPan_ = e->pos();
    update();
  }
}
void TrajectoryWidget::mouseReleaseEvent(QMouseEvent*) { rotating_ = panning_ = false; }
void TrajectoryWidget::wheelEvent(QWheelEvent* e) {
  distance_ *= e->angleDelta().y() > 0 ? 0.85 : 1.18;
  distance_ = std::max(maxRadius_ * 0.4, std::min(maxRadius_ * 20.0, distance_));
  update();
}

} // namespace rtplot::gui
