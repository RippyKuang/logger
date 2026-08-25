#include "PlotWidget.hpp"

#include "rtplot/downsample.hpp"

#include <QEvent>
#include <QLayout>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>
#include <QResizeEvent>
#include <QStringList>
#include <QToolTip>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <limits>

namespace rtplot::gui {

class SelectionOverlay : public QWidget {
public:
  explicit SelectionOverlay(QWidget* parent) : QWidget(parent) {
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_NoSystemBackground);
    setVisible(false);
  }
  void setRectF(const QRectF& r) { rect_ = r.normalized(); update(); }
  void clearRect() { setVisible(false); rect_ = QRectF(); }
protected:
  void paintEvent(QPaintEvent*) override {
    QPainter p(this);
    p.setPen(QPen(QColor(0xAA, 0xAA, 0xAA), 1.2, Qt::DashLine));
    p.setBrush(QColor(128, 128, 128, 70));
    p.drawRect(rect_);
  }
private:
  QRectF rect_;
};


namespace {
constexpr int kMarginLeft = 56;
constexpr int kMarginRight = 12;
constexpr int kMarginTop = 10;
constexpr int kMarginBottom = 22;

QColor colorForIndex(int i) {
  static const QColor palette[] = {
      QColor(0x4E9AF1), QColor(0xF1A94E), QColor(0x6BCB77),
      QColor(0xE65F5F), QColor(0xB07CE8), QColor(0x00B8A9),
      QColor(0xF5D300), QColor(0xFF6B9A)};
  return palette[i % 8];
}
} // namespace

// ============================================================================
// PlotStrip
// ============================================================================
PlotStrip::PlotStrip(QWidget* parent) : QWidget(parent) {
  setMinimumHeight(120);
  setMouseTracking(true);
  setAttribute(Qt::WA_OpaquePaintEvent, true);
  setAutoFillBackground(false);
}

void PlotStrip::addCurve(const QString& name, const std::vector<Sample>& data, QColor color) {
  for (auto& c : curves_) {
    if (c.style.name == name) { c.samples = data; c.style.color = color; update(); return; }
  }
  Curve c;
  c.style.name = name;
  c.style.color = color.isValid() ? color : colorForIndex(static_cast<int>(curves_.size()));
  c.samples = data;
  curves_.push_back(std::move(c));
  computeRanges();
  update();
}

void PlotStrip::setCurveData(const QString& name, const std::vector<Sample>& data) {
  for (auto& c : curves_) {
    if (c.style.name == name) {
      c.samples = data;
      computeRanges();
      update();
      return;
    }
  }
  addCurve(name, data, colorForIndex(static_cast<int>(curves_.size())));
}

void PlotStrip::appendCurveData(const QString& name, const std::vector<Sample>& data) {
  if (data.empty()) return;
  for (auto& c : curves_) {
    if (c.style.name == name) {
      c.samples.insert(c.samples.end(), data.begin(), data.end());
      update();
      return;
    }
  }
  addCurve(name, data, colorForIndex(static_cast<int>(curves_.size())));
}

void PlotStrip::removeCurve(const QString& name) {
  curves_.erase(std::remove_if(curves_.begin(), curves_.end(),
                               [&](const Curve& c) { return c.style.name == name; }),
                curves_.end());
  computeRanges();
  update();
}

void PlotStrip::clear() {
  curves_.clear();
  events_.clear();
  roiActive_ = false;
  hasReference_ = false;
  referenceChannel_.clear();
  hoverActive_ = false;
  computeRanges();
  update();
}

void PlotStrip::setXRange(double x0, double x1, bool autoX) {
  autoX_ = autoX;
  if (!autoX) { xMin_ = x0; xMax_ = x1; }
  computeRanges();
  update();
}

void PlotStrip::setTimeOrigin(Timestamp t) {
  timeOrigin_ = t;
  update();
}

void PlotStrip::computeRanges() {
  if (autoX_) {
    xMin_ = 0; xMax_ = 1;
    bool first = true;
    for (const auto& c : curves_) {
      if (c.samples.empty()) continue;
      const double a = static_cast<double>(c.samples.front().t);
      const double b = static_cast<double>(c.samples.back().t);
      if (first) { xMin_ = a; xMax_ = b; first = false; }
      else { xMin_ = std::min(xMin_, a); xMax_ = std::max(xMax_, b); }
    }
  }
  if (xMax_ <= xMin_) xMax_ = xMin_ + 1.0;

  if (autoY_) {
    bool first = true;
    yMin_ = 0; yMax_ = 1;
    for (const auto& c : curves_) {
      if (c.samples.empty()) continue;
      for (const auto& s : c.samples) {
        const double t = static_cast<double>(s.t);
        if (t < xMin_ || t > xMax_) continue;
        if (first) { yMin_ = s.v; yMax_ = s.v; first = false; }
        else { yMin_ = std::min(yMin_, s.v); yMax_ = std::max(yMax_, s.v); }
      }
    }
    if (!first && yMax_ - yMin_ < 1e-12) { yMin_ -= 0.5; yMax_ += 0.5; }
  }
  if (yMax_ <= yMin_) { yMin_ = -1; yMax_ = 1; }
}

void PlotStrip::autoScaleY() { autoY_ = true; computeRanges(); update(); }
void PlotStrip::setYRange(double y0, double y1) { autoY_ = false; yMin_ = y0; yMax_ = y1; update(); }

double PlotStrip::pixelToTime(int px) const {
  const double w = std::max(1, width() - kMarginLeft - kMarginRight);
  return xMin_ + (px - kMarginLeft) / w * (xMax_ - xMin_);
}
double PlotStrip::pixelToValue(int py) const {
  const double h = std::max(1, height() - kMarginTop - kMarginBottom);
  return yMax_ - (py - kMarginTop) / h * (yMax_ - yMin_);
}

std::vector<std::string> PlotStrip::curveNames() const {
  std::vector<std::string> out;
  for (const auto& c : curves_) out.push_back(c.style.name.toStdString());
  return out;
}
const std::vector<Sample>* PlotStrip::curveData(const QString& name) const {
  for (const auto& c : curves_) if (c.style.name == name) return &c.samples;
  return nullptr;
}

bool PlotStrip::hitTest(int px, int py, double tolPixels, HoverInfo* out) const {
  if (px < kMarginLeft || px > width() - kMarginRight ||
      py < kMarginTop || py > height() - kMarginBottom) return false;
  const double t0 = pixelToTime(px - static_cast<int>(tolPixels));
  const double t1 = pixelToTime(px + static_cast<int>(tolPixels));
  const double vTop = pixelToValue(py + static_cast<int>(tolPixels));
  const double vBottom = pixelToValue(py - static_cast<int>(tolPixels));
  const double vLo = std::min(vTop, vBottom);
  const double vHi = std::max(vTop, vBottom);
  const double w = std::max(1, width() - kMarginLeft - kMarginRight);
  const double h = std::max(1, height() - kMarginTop - kMarginBottom);
  const double best0 = tolPixels * tolPixels;
  double best = best0;
  bool found = false;
  HoverInfo info;

  for (const auto& c : curves_) {
    if (!c.style.visible || c.samples.empty()) continue;
    auto it = std::lower_bound(c.samples.begin(), c.samples.end(), Sample{static_cast<Timestamp>(t0), 0.0},
                               [](const Sample& a, const Sample& b) { return a.t < b.t; });
    if (it != c.samples.begin()) --it;
    for (; it != c.samples.end(); ++it) {
      const double t = static_cast<double>(it->t);
      if (t > t1) break;
      if (it->v < vLo || it->v > vHi) continue;
      const double sx = kMarginLeft + (t - xMin_) / (xMax_ - xMin_) * w;
      const double sy = kMarginTop + (yMax_ - it->v) / (yMax_ - yMin_) * h;
      const double dx = sx - px;
      const double dy = sy - py;
      const double d = dx * dx + dy * dy;
      if (d <= best) {
        best = d;
        found = true;
        info.channel = c.style.name;
        info.sample = *it;
        info.pixel = QPointF(sx, sy);
      }
    }
  }
  if (found && out) *out = info;
  return found;
}

void PlotStrip::setReferencePoint(const QString& channel, Sample s) {
  referenceChannel_ = channel;
  reference_ = s;
  hasReference_ = true;
  update();
}

void PlotStrip::clearReferencePoint() {
  hasReference_ = false;
  update();
}

void PlotStrip::setHoverPoint(const HoverInfo& info) {
  if (hoverActive_ && hoverPoint_.sample.t == info.sample.t &&
      hoverPoint_.sample.v == info.sample.v && hoverPoint_.channel == info.channel) return;
  hoverActive_ = true;
  hoverPoint_ = info;
  update();
}

void PlotStrip::clearHoverPoint() {
  if (!hoverActive_) return;
  hoverActive_ = false;
  update();
}

bool PlotStrip::referenceAt(int px, int py, double tolPixels) const {
  if (!hasReference_) return false;
  const double rt = static_cast<double>(reference_.t);
  if (rt < xMin_ || rt > xMax_) return false;
  const double w = std::max(1, width() - kMarginLeft - kMarginRight);
  const double h = std::max(1, height() - kMarginTop - kMarginBottom);
  const double sx = kMarginLeft + (rt - xMin_) / (xMax_ - xMin_) * w;
  const double sy = kMarginTop + (yMax_ - reference_.v) / (yMax_ - yMin_) * h;
  const double dx = sx - px;
  const double dy = sy - py;
  return dx * dx + dy * dy <= tolPixels * tolPixels;
}

bool PlotStrip::eventNear(double t, double tol, Event* out) const {
  for (const auto& e : events_) {
    if (std::fabs(static_cast<double>(e.t) - t) <= tol) {
      if (out) *out = e;
      return true;
    }
  }
  return false;
}

QPolygonF PlotStrip::polygonForCurve(const Curve& c, double x0, double x1, int widthPx) {
  const auto& v = c.samples;
  QPolygonF poly;
  if (v.empty()) return poly;
  auto first = std::lower_bound(v.begin(), v.end(), Sample{static_cast<Timestamp>(x0), 0.0},
                                [](const Sample& a, const Sample& b) { return a.t < b.t; });
  auto last = std::upper_bound(v.begin(), v.end(), Sample{static_cast<Timestamp>(x1), 0.0},
                               [](const Sample& a, const Sample& b) { return a.t < b.t; });
  if (first != v.begin() && first != v.end()) --first;
  if (last != v.end()) ++last;
  const size_t count = static_cast<size_t>(last - first);
  if (count == 0) return poly;

  std::vector<Sample> view(first, last);
  const size_t target = static_cast<size_t>(std::max<int>(2, widthPx * 2));
  if (view.size() > target) view = downsample(view, target, algo_);

  poly.reserve(static_cast<int>(view.size()));
  const double w = std::max(1, width() - kMarginLeft - kMarginRight);
  const double h = std::max(1, height() - kMarginTop - kMarginBottom);
  for (const auto& s : view) {
    const double px = kMarginLeft + (static_cast<double>(s.t) - xMin_) / (xMax_ - xMin_) * w;
    const double py = kMarginTop + (yMax_ - s.v) / (yMax_ - yMin_) * h;
    poly.append(QPointF(px, py));
  }
  return poly;
}

void PlotStrip::paintEvent(QPaintEvent*) {
  QPainter p(this);
  p.fillRect(rect(), QColor(0x15, 0x17, 0x1E));
  const int plotW = std::max(1, width() - kMarginLeft - kMarginRight);
  const int plotH = std::max(1, height() - kMarginTop - kMarginBottom);

  p.setPen(QPen(QColor(0x30, 0x36, 0x42), 1));
  for (int i = 0; i <= 4; ++i) {
    const int y = kMarginTop + plotH * i / 4;
    p.drawLine(kMarginLeft, y, kMarginLeft + plotW, y);
  }
  for (int i = 0; i <= 6; ++i) {
    const int x = kMarginLeft + plotW * i / 6;
    p.drawLine(x, kMarginTop, x, kMarginTop + plotH);
  }
  p.setPen(QColor(0x70, 0x7A, 0x8C));
  p.drawLine(kMarginLeft, kMarginTop + plotH, kMarginLeft + plotW, kMarginTop + plotH);
  p.drawLine(kMarginLeft, kMarginTop, kMarginLeft, kMarginTop + plotH);

  QFont labelFont = font();
  labelFont.setPointSize(8);
  p.setFont(labelFont);
  p.setPen(QColor(0xA8, 0xB2, 0xC4));
  for (int i = 0; i <= 4; ++i) {
    const double y = yMax_ - (yMax_ - yMin_) * i / 4;
    p.drawText(QRect(2, kMarginTop + plotH * i / 4 - 8, kMarginLeft - 6, 16),
               Qt::AlignRight | Qt::AlignVCenter, QString::number(y, 'g', 3));
  }
  for (int i = 0; i <= 6; ++i) {
    const double t = xMin_ + (xMax_ - xMin_) * i / 6;
    const double rel = (t - static_cast<double>(timeOrigin_)) * 1e-9;
    p.drawText(QRect(kMarginLeft + plotW * i / 6 - 50, kMarginTop + plotH + 3, 100, 14),
               Qt::AlignHCenter, QString::number(rel, 'f', 3));
  }

  p.setRenderHint(QPainter::Antialiasing, true);
  for (const auto& c : curves_) {
    if (!c.style.visible) continue;
    QPolygonF poly = polygonForCurve(c, xMin_, xMax_, plotW);
    if (poly.size() < 2) continue;
    QPen pen(c.style.color, lineWidth_);
    pen.setCosmetic(true);
    p.setPen(pen);
    p.drawPolyline(poly);
  }

  if (hoverActive_) {
    const double ht = static_cast<double>(hoverPoint_.sample.t);
    if (ht >= xMin_ && ht <= xMax_) {
      const int hx = kMarginLeft + static_cast<int>((ht - xMin_) / (xMax_ - xMin_) * plotW);
      const int hy = kMarginTop + static_cast<int>((yMax_ - hoverPoint_.sample.v) / (yMax_ - yMin_) * plotH);
      QPen hp(QColor(0xD8, 0xDC, 0xE2, 150), 1, Qt::DashLine);
      p.setPen(hp);
      p.drawLine(hx, hy, hx, kMarginTop + plotH);
      p.drawLine(hx, hy, kMarginLeft, hy);
      p.setPen(QColor(0xF2, 0xF4, 0xF7));
      p.drawEllipse(QPoint(hx, hy), 2, 2);
    }
  }

  if (hasReference_) {
    const double rt = static_cast<double>(reference_.t);
    if (rt >= xMin_ && rt <= xMax_) {
      const int rx = kMarginLeft + static_cast<int>((rt - xMin_) / (xMax_ - xMin_) * plotW);
      const int ry = kMarginTop + static_cast<int>((yMax_ - reference_.v) / (yMax_ - yMin_) * plotH);
      p.setPen(QPen(QColor(0xD0, 0xD4, 0xDC, 190), 1, Qt::DashLine));
      p.drawLine(rx, kMarginTop, rx, kMarginTop + plotH);
      p.setPen(QColor(0xF0, 0xF2, 0xF5));
      p.drawEllipse(QPoint(rx, ry), 3, 3);
      const double rel = (rt - static_cast<double>(timeOrigin_)) * 1e-9;
      p.drawText(QPoint(rx + 7, ry - 6),
                 QString("%1  t=%2 s  v=%3")
                     .arg(referenceChannel_)
                     .arg(rel, 0, 'f', 4)
                     .arg(reference_.v, 0, 'g', 6));
    }
  }

  if (roiActive_) {
    const double t0 = std::min(roi0_, roi1_);
    const double t1 = std::max(roi0_, roi1_);
    const double w = plotW;
    const int x0 = kMarginLeft + static_cast<int>((t0 - xMin_) / (xMax_ - xMin_) * w);
    const int x1 = kMarginLeft + static_cast<int>((t1 - xMin_) / (xMax_ - xMin_) * w);
    p.fillRect(QRect(x0, kMarginTop, x1 - x0, plotH), QColor(128, 128, 128, 40));
    p.setPen(QPen(QColor(170, 170, 170), 1, Qt::DashLine));
    p.drawLine(x0, kMarginTop, x0, kMarginTop + plotH);
    p.drawLine(x1, kMarginTop, x1, kMarginTop + plotH);
  }

  // Event timeline markers.
  int evLabel = 0;
  for (const auto& e : events_) {
    const double t = static_cast<double>(e.t);
    if (t < xMin_ || t > xMax_) continue;
    const int x = kMarginLeft + static_cast<int>((t - xMin_) / (xMax_ - xMin_) * plotW);
    p.setPen(QPen(QColor(0xFF, 0x55, 0x55, 180), 1, Qt::DashLine));
    p.drawLine(x, kMarginTop, x, kMarginTop + plotH);
    p.setPen(QColor(0xFF, 0x80, 0x80));
    p.drawLine(x, kMarginTop, x + 5, kMarginTop + 5);
    p.drawText(QPoint(x + 7, kMarginTop + 10 + (evLabel++ % 5) * 12),
               QString::fromStdString(e.name));
  }

  p.setPen(QColor(0x50, 0x55, 0x60));
  p.drawRect(rect().adjusted(0, 0, -1, -1));
}

void PlotStrip::leaveEvent(QEvent*) { QToolTip::hideText(); }

// ============================================================================
// PlotGridWidget
// ============================================================================
PlotGridWidget::PlotGridWidget(QWidget* parent) : QWidget(parent) {
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);
  setMouseTracking(true);
  setMinimumHeight(240);
  overlay_ = new SelectionOverlay(this);
  overlay_->setGeometry(rect());
  overlay_->raise();
}

PlotStrip* PlotGridWidget::addStrip() {
  auto* strip = new PlotStrip(this);
  strips_.push_back(strip);
  static_cast<QVBoxLayout*>(layout())->addWidget(strip, 1);
  strip->setXRange(xMin_, xMax_, false);
  strip->setTimeOrigin(timeOrigin_);
  strip->setEvents(events_);
  if (overlay_) overlay_->raise();
  return strip;
}

void PlotGridWidget::addCurve(int strip, const QString& name, const std::vector<Sample>& data, QColor color) {
  while (strips_.size() <= strip) addStrip();
  strips_[strip]->addCurve(name, data, color);
  if (!data.empty()) {
    if (xMin_ == 0.0 && xMax_ == 1.0 && !strips_.isEmpty()) {
      xMin_ = static_cast<double>(data.front().t);
      xMax_ = static_cast<double>(data.back().t);
    } else {
      xMin_ = std::min(xMin_, static_cast<double>(data.front().t));
      xMax_ = std::max(xMax_, static_cast<double>(data.back().t));
    }
    for (auto* s : strips_) s->setXRange(xMin_, xMax_, false);
  }
}

void PlotGridWidget::setCurveData(const QString& name, const std::vector<Sample>& data) {
  bool found = false;
  for (auto* s : strips_) {
    if (s->curveData(name)) { s->setCurveData(name, data); found = true; }
  }
  if (!found && !strips_.empty()) strips_.front()->setCurveData(name, data);
}

void PlotGridWidget::removeEmptyStrips() {
  for (int i = static_cast<int>(strips_.size()) - 1; i >= 0; --i) {
    if (!strips_[i]->curveNames().empty() || strips_.size() <= 1) continue;
    PlotStrip* s = strips_[i];
    strips_.removeAt(i);
    layout()->removeWidget(s);
    delete s;
  }
}

void PlotGridWidget::appendCurveData(const QString& name, const std::vector<Sample>& data) {
  for (auto* s : strips_) {
    if (s->curveData(name)) { s->appendCurveData(name, data); return; }
  }
  if (!strips_.empty()) strips_.front()->appendCurveData(name, data);
}

void PlotGridWidget::clear() {
  events_.clear();
  for (auto* s : strips_) s->clear();
  clearRoi();
  removeEmptyStrips();
}

void PlotGridWidget::setEvents(std::vector<Event> events) {
  events_ = std::move(events);
  for (auto* s : strips_) s->setEvents(events_);
}

void PlotGridWidget::setTimeOrigin(Timestamp t) {
  timeOrigin_ = t;
  for (auto* s : strips_) s->setTimeOrigin(t);
}

void PlotGridWidget::setXRange(double x0, double x1) {
  // Time-series axes are relative to timeOrigin_; do not show negative time.
  xMin_ = std::max<double>(x0, static_cast<double>(timeOrigin_));
  xMax_ = std::max<double>(x1, xMin_ + 1.0);
  if (xMax_ <= xMin_) xMax_ = xMin_ + 1.0;
  for (auto* s : strips_) s->setXRange(xMin_, xMax_, false);
}

void PlotGridWidget::autoScaleX() {
  bool first = true;
  for (auto* s : strips_) {
    for (const auto& name : s->curveNames()) {
      const auto* d = s->curveData(QString::fromStdString(name));
      if (!d || d->empty()) continue;
      const double a = static_cast<double>(d->front().t);
      const double b = static_cast<double>(d->back().t);
      if (first) { xMin_ = a; xMax_ = b; first = false; }
      else { xMin_ = std::min(xMin_, a); xMax_ = std::max(xMax_, b); }
    }
  }
  if (!first) setXRange(xMin_, xMax_);
}

void PlotGridWidget::autoScaleY() { for (auto* s : strips_) s->autoScaleY(); }

bool PlotGridWidget::currentRoi(double* t0, double* t1) const {
  if (!roiActive_) return false;
  if (t0) *t0 = roiT0_;
  if (t1) *t1 = roiT1_;
  return true;
}

void PlotGridWidget::clearRoi() {
  roiActive_ = false;
  for (auto* s : strips_) s->clearRoi();
  if (roiStats_) roiStats_(QString());
  update();
}


void PlotGridWidget::mousePressEvent(QMouseEvent* e) {
  pressPos_ = currentPos_ = e->pos();
  if (e->button() == Qt::MiddleButton) drag_ = DragMode::Pan;
  else if (e->button() == Qt::LeftButton) drag_ = DragMode::BoxZoom;
  else if (e->button() == Qt::RightButton) drag_ = DragMode::Roi;
  if (drag_ == DragMode::BoxZoom || drag_ == DragMode::Roi) {
    if (overlay_) { overlay_->setVisible(true); overlay_->setRectF(QRectF(pressPos_, pressPos_)); overlay_->raise(); }
  } else if (overlay_) {
    overlay_->clearRect();
  }
}

void PlotGridWidget::mouseMoveEvent(QMouseEvent* e) {
  currentPos_ = e->pos();

  if (drag_ == DragMode::Pan) {
    const double w = std::max(1, width() - kMarginLeft - kMarginRight);
    const double dx = (pressPos_.x() - e->pos().x()) / w * (xMax_ - xMin_);
    const double span = xMax_ - xMin_;
    setXRange(xMin_ + dx, xMin_ + dx + span);
    pressPos_.setX(e->pos().x());
    return;
  }
  if (drag_ == DragMode::BoxZoom || drag_ == DragMode::Roi) {
    if (overlay_) overlay_->setRectF(QRectF(pressPos_, e->pos()));
    return;
  }

  // No drag: hover crosshair + tooltip. Cache the last position so a motion
  // event that does not change the pixel does not repeat the nearest-point
  // search or trigger a repaint.
  if (lastHoverValid_ && e->pos() == lastHoverPos_) return;
  lastHoverValid_ = true;
  lastHoverPos_ = e->pos();

  PlotStrip* strip = nullptr;
  for (auto* s : strips_) {
    if (s->geometry().contains(e->pos())) { strip = s; break; }
  }
  if (strip != hoverStrip_) {
    if (hoverStrip_) hoverStrip_->clearHoverPoint();
    hoverStrip_ = strip;
  }

  if (strip) {
    const QPoint local = strip->mapFrom(this, e->pos());
    HoverInfo hi;
    if (strip->hitTest(local.x(), local.y(), 8.0, &hi)) {
      strip->setHoverPoint(hi);
      const double t = (hi.sample.t - timeOrigin_) * 1e-9;
      QString text = QString("<b>%1</b><br>t=%2 s<br>v=%3")
                         .arg(hi.channel.toHtmlEscaped())
                         .arg(t, 0, 'f', 6)
                         .arg(hi.sample.v, 0, 'g', 8);
      if (strip->hasReferencePoint()) {
        const Sample& r = strip->referencePoint();
        const double dt = (hi.sample.t - r.t) * 1e-9;
        const double dv = hi.sample.v - r.v;
        text += QString("<br><b>Δt=%1 s</b><br><b>Δv=%2</b>")
                    .arg(dt, 0, 'f', 6)
                    .arg(dv, 0, 'g', 8);
      }
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
      QToolTip::showText(e->globalPosition().toPoint(), text);
#else
      QToolTip::showText(e->globalPos(), text);
#endif
      return;
    }
  }

  if (hoverStrip_) {
    hoverStrip_->clearHoverPoint();
    hoverStrip_ = nullptr;
  }

  if (!strips_.empty()) {
    Event ev;
    const double tolPx = 6.0;
    const double tol = tolPx / std::max(1, width() - kMarginLeft - kMarginRight) * (xMax_ - xMin_);
    const double t = strips_.first()->pixelToTime(e->pos().x());
    if (strips_.first()->eventNear(t, tol, &ev)) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
      QToolTip::showText(e->globalPosition().toPoint(),
#else
      QToolTip::showText(e->globalPos(),
#endif
                         QString("<b>%1</b><br>%2<br>t=%3 s")
                             .arg(QString::fromStdString(ev.name).toHtmlEscaped(),
                                  QString::fromStdString(ev.payload).toHtmlEscaped(),
                                  QString::number((ev.t - timeOrigin_) * 1e-9, 'f', 6)));
    }
  }
}

void PlotGridWidget::applyBox(const QPointF& a, const QPointF& b) {
  if (std::fabs(a.x() - b.x()) < 8 || strips_.empty()) return;
  PlotStrip* s = strips_.first();
  double t0 = s->pixelToTime(static_cast<int>(std::min(a.x(), b.x())));
  double t1 = s->pixelToTime(static_cast<int>(std::max(a.x(), b.x())));
  t0 = std::max(t0, s->xMin() - (s->xMax() - s->xMin()) * 0.1);
  t1 = std::min(t1, s->xMin() + (s->xMax() - s->xMin()) * 1.1);
  if (t1 <= t0) return;
  setXRange(t0, t1);
  for (auto* strip : strips_) strip->autoScaleY();
}

void PlotGridWidget::applyRoi(const QPointF& a, const QPointF& b) {
  if (strips_.empty()) return;
  PlotStrip* s0 = strips_.first();
  const double rawT0 = s0->pixelToTime(static_cast<int>(std::min(a.x(), b.x())));
  const double rawT1 = s0->pixelToTime(static_cast<int>(std::max(a.x(), b.x())));
  const double t0 = std::min(rawT0, rawT1);
  const double t1 = std::max(rawT0, rawT1);
  if (t1 <= t0) return;

  roiActive_ = true;
  roiT0_ = t0;
  roiT1_ = t1;

  uint64_t n = 0;
  double min = std::numeric_limits<double>::infinity();
  double max = -std::numeric_limits<double>::infinity();
  double sum = 0, sum2 = 0;
  for (auto* strip : strips_) {
    strip->setRoi(t0, t1);
    for (const auto& name : strip->curveNames()) {
      const auto* d = strip->curveData(QString::fromStdString(name));
      if (!d) continue;
      auto lo = std::lower_bound(d->begin(), d->end(), Sample{static_cast<Timestamp>(t0), 0},
                                 [](const Sample& x, const Sample& y) { return x.t < y.t; });
      auto hi = std::upper_bound(d->begin(), d->end(), Sample{static_cast<Timestamp>(t1), 0},
                                 [](const Sample& x, const Sample& y) { return x.t < y.t; });
      for (auto it = lo; it != hi; ++it) {
        ++n; min = std::min(min, it->v); max = std::max(max, it->v);
        sum += it->v; sum2 += it->v * it->v;
      }
    }
  }
  if (n && roiStats_) {
    const double mean = sum / static_cast<double>(n);
    const double var = n > 1 ? (sum2 - sum * mean) / static_cast<double>(n - 1) : 0.0;
    roiStats_(QString("ROI [%1, %2] s  n=%3  mean=%4  std=%5  min=%6  max=%7")
                  .arg((t0 - timeOrigin_) * 1e-9, 0, 'g', 5)
                  .arg((t1 - timeOrigin_) * 1e-9, 0, 'g', 5)
                  .arg(n).arg(mean, 0, 'g', 5)
                  .arg(std::sqrt(std::max(0.0, var)), 0, 'g', 5)
                  .arg(min, 0, 'g', 5).arg(max, 0, 'g', 5));
  }
}

void PlotGridWidget::mouseReleaseEvent(QMouseEvent* e) {
  if (drag_ == DragMode::BoxZoom) {
    applyBox(pressPos_, e->pos());
  } else if (drag_ == DragMode::Roi) {
    const QPointF delta = e->pos() - pressPos_;
    if (e->button() == Qt::RightButton && std::fabs(delta.x()) < 6 && std::fabs(delta.y()) < 6) {
      // Right-click on a curve point selects the measurement reference.
      for (auto* s : strips_) {
        if (!s->geometry().contains(e->pos())) continue;
        const QPoint local = s->mapFrom(this, e->pos());
        HoverInfo hi;
        if (s->hitTest(local.x(), local.y(), 9.0, &hi)) {
          s->setReferencePoint(hi.channel, hi.sample);
          lastHoverValid_ = false;
          const QString refText = QString("Reference: %1  t=%2 s  v=%3")
                                      .arg(hi.channel)
                                      .arg((hi.sample.t - timeOrigin_) * 1e-9, 0, 'f', 6)
                                      .arg(hi.sample.v, 0, 'g', 8);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
          QToolTip::showText(e->globalPosition().toPoint(), refText);
#else
          QToolTip::showText(e->globalPos(), refText);
#endif
        }
        break;
      }
    } else {
      applyRoi(pressPos_, e->pos());
    }
  }
  drag_ = DragMode::None;
  if (overlay_) overlay_->clearRect();
  update();
}

void PlotGridWidget::wheelEvent(QWheelEvent* e) {
  const double factor = e->angleDelta().y() > 0 ? 0.85 : 1.15;
  if (strips_.empty()) return;
  const double center = strips_.first()->pixelToTime(e->position().x());
  const double span = (xMax_ - xMin_) * factor;
  const double frac = (center - xMin_) / std::max(1e-12, xMax_ - xMin_);
  setXRange(center - span * frac, center + span * (1.0 - frac));
}

void PlotGridWidget::paintEvent(QPaintEvent*) {}

void PlotGridWidget::resizeEvent(QResizeEvent* e) {
  QWidget::resizeEvent(e);
  if (overlay_) { overlay_->setGeometry(rect()); overlay_->raise(); }
}

void PlotGridWidget::mouseDoubleClickEvent(QMouseEvent* e) {
  if (e->button() == Qt::RightButton && roiActive_) {
    clearRoi();
    lastHoverValid_ = false;
    QToolTip::hideText();
    e->accept();
    return;
  }
  if (e->button() == Qt::LeftButton) {
    for (auto* s : strips_) {
      if (!s->geometry().contains(e->pos())) continue;
      const QPoint local = s->mapFrom(this, e->pos());
      if (s->referenceAt(local.x(), local.y(), 10.0)) {
        s->clearReferencePoint();
        lastHoverValid_ = false;
        e->accept();
        return;
      }
    }
  }
  QWidget::mouseDoubleClickEvent(e);
}

void PlotGridWidget::leaveEvent(QEvent*) {
  if (hoverStrip_) { hoverStrip_->clearHoverPoint(); hoverStrip_ = nullptr; }
  lastHoverValid_ = false;
  QToolTip::hideText();
}

} // namespace rtplot::gui
