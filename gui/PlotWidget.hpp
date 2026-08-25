#pragma once
#include "rtplot/types.hpp"

#include <QColor>
#include <QPointF>
#include <QString>
#include <QVector>
#include <QWidget>

#include <functional>
#include <vector>

namespace rtplot::gui {

class SelectionOverlay;

struct CurveStyle {
  QString name;
  QColor color = Qt::red;
  bool visible = true;
};

struct HoverInfo {
  QString channel;
  Sample sample;
  QPointF pixel;
};

/// High-performance QPainter strip. Raw samples are kept in a std::vector and
/// downsampled to roughly 2 points per pixel on every paint (MinMax by default,
/// LTTB optional). This keeps 100k+ point channels comfortably at 60 FPS.
class PlotStrip : public QWidget {
public:
  explicit PlotStrip(QWidget* parent = nullptr);

  void addCurve(const QString& name, const std::vector<Sample>& data, QColor color);
  void setCurveData(const QString& name, const std::vector<Sample>& data);
  void appendCurveData(const QString& name, const std::vector<Sample>& data);
  void removeCurve(const QString& name);
  void clear();

  void setEvents(std::vector<Event> events) { events_ = std::move(events); update(); }

  void setXRange(double x0, double x1, bool autoX);
  void setTimeOrigin(Timestamp t);
  [[nodiscard]] Timestamp timeOrigin() const { return timeOrigin_; }
  void autoScaleY();
  void setYRange(double y0, double y1);

  void setRoi(double t0, double t1) { roiActive_ = true; roi0_ = t0; roi1_ = t1; update(); }
  void clearRoi() { roiActive_ = false; update(); }


  [[nodiscard]] double pixelToTime(int px) const;
  [[nodiscard]] double pixelToValue(int py) const;
  [[nodiscard]] std::vector<std::string> curveNames() const;
  [[nodiscard]] const std::vector<Sample>* curveData(const QString& name) const;
  [[nodiscard]] bool eventNear(double t, double tol, Event* out) const;

  /// Nearest curve sample within `tolPixels` of the strip-local point `px,py`.
  [[nodiscard]] bool hitTest(int px, int py, double tolPixels, HoverInfo* out) const;

  void setReferencePoint(const QString& channel, Sample s);
  void clearReferencePoint();
  void setHoverPoint(const HoverInfo& info);
  void clearHoverPoint();
  [[nodiscard]] bool hasReferencePoint() const { return hasReference_; }
  [[nodiscard]] const Sample& referencePoint() const { return reference_; }
  [[nodiscard]] bool referenceAt(int px, int py, double tolPixels) const;

  [[nodiscard]] double xMin() const { return xMin_; }
  [[nodiscard]] double xMax() const { return xMax_; }

protected:
  void paintEvent(QPaintEvent*) override;
  void leaveEvent(QEvent*) override;

private:
  struct Curve {
    CurveStyle style;
    std::vector<Sample> samples;
  };

  void computeRanges();
  QPolygonF polygonForCurve(const Curve& c, double x0, double x1, int widthPx);

  std::vector<Curve> curves_;
  std::vector<Event> events_;
  double xMin_ = 0, xMax_ = 1;
  double yMin_ = -1, yMax_ = 1;
  bool autoX_ = true;
  bool autoY_ = true;
  Timestamp timeOrigin_ = 0;
  bool roiActive_ = false;
  double roi0_ = 0, roi1_ = 0;
  bool hasReference_ = false;
  Sample reference_{};
  QString referenceChannel_;
  bool hoverActive_ = false;
  HoverInfo hoverPoint_;
  DownsampleAlgorithm algo_ = DownsampleAlgorithm::MinMax;
  double lineWidth_ = 1.2;
  bool eventTooltipArmed_ = false;

  friend class PlotGridWidget;
};

/// Multi-strip plotting surface with a linked/shared time axis and mouse
/// interactions: left-drag box zoom, right-drag ROI, middle-drag pan.
class PlotGridWidget : public QWidget {
public:
  explicit PlotGridWidget(QWidget* parent = nullptr);

  PlotStrip* addStrip();
  void addCurve(int strip, const QString& name, const std::vector<Sample>& data, QColor color);
  void setCurveData(const QString& name, const std::vector<Sample>& data);
  void appendCurveData(const QString& name, const std::vector<Sample>& data);
  void removeEmptyStrips();
  void clear();

  void setEvents(std::vector<Event> events);
  void setXRange(double x0, double x1);
  void setTimeOrigin(Timestamp t);
  void autoScaleX();
  void autoScaleY();
  [[nodiscard]] bool currentRoi(double* t0, double* t1) const;
  void clearRoi();

  using RoiStatsCallback = std::function<void(const QString&)>;
  void setRoiStatsCallback(RoiStatsCallback cb) { roiStats_ = std::move(cb); }

  [[nodiscard]] QVector<PlotStrip*> strips() const { return strips_; }

protected:
  void mousePressEvent(QMouseEvent*) override;
  void mouseMoveEvent(QMouseEvent*) override;
  void mouseReleaseEvent(QMouseEvent*) override;
  void wheelEvent(QWheelEvent*) override;
  void mouseDoubleClickEvent(QMouseEvent*) override;
  void resizeEvent(QResizeEvent*) override;
  void paintEvent(QPaintEvent*) override;
  void leaveEvent(QEvent*) override;

private:
  void applyBox(const QPointF& a, const QPointF& b);
  void applyRoi(const QPointF& a, const QPointF& b);

  QVector<PlotStrip*> strips_;
  QPointF pressPos_;
  QPointF currentPos_;
  enum class DragMode { None, BoxZoom, Pan, Roi };
  DragMode drag_ = DragMode::None;
  bool boxZoomEnabled_ = true;
  bool roiMode_ = false;
  double xMin_ = 0, xMax_ = 1;
  Timestamp timeOrigin_ = 0;
  bool roiActive_ = false;
  double roiT0_ = 0, roiT1_ = 0;
  std::vector<Event> events_;
  SelectionOverlay* overlay_ = nullptr;
  PlotStrip* hoverStrip_ = nullptr;
  QPointF lastHoverPos_;
  bool lastHoverValid_ = false;
  RoiStatsCallback roiStats_;
};

} // namespace rtplot::gui
