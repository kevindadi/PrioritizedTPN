#pragma once

#include <QPainterPath>
#include <QPointF>
#include <QVector>
#include <QWidget>

#include "app_model.h"

class StateGraphView : public QWidget {
  Q_OBJECT

 public:
  explicit StateGraphView(QWidget* parent = nullptr);

  void setGraph(const StateGraph& graph, bool generic = false);
  void fitView();
  void zoomBy(double factor);

 signals:
  void nodeSelected(int index);
  void statusMessage(const QString& message);

 protected:
  void paintEvent(QPaintEvent* event) override;
  void mousePressEvent(QMouseEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;
  void mouseReleaseEvent(QMouseEvent* event) override;
  void mouseDoubleClickEvent(QMouseEvent* event) override;
  void wheelEvent(QWheelEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;
  void leaveEvent(QEvent* event) override;

 private:
  void layoutGraph();
  void rebuildEdges();
  QPointF screenToWorld(const QPointF& screenPos) const;
  void zoomAt(const QPointF& screenPos, double factor);
  int nodeAt(const QPointF& worldPos) const;
  QPointF anchor(int nodeIndex, const QPointF& toward) const;
  void paintNode(QPainter& painter, int index);

  StateGraph graph_;
  bool hasGraph_ = false;
  bool generic_ = false;
  QVector<QPointF> positions_;
  QVector<int> levels_;
  struct EdgeVisual {
    int from = -1;
    int to = -1;
    QString label;
    QPainterPath path;
    QPointF tip;
    double angle = 0.0;
  };
  QVector<EdgeVisual> edges_;

  double scale_ = 1.0;
  QPointF pan_;
  QPointF worldCenter_;
  bool dragging_ = false;
  QPointF lastMouse_;
  int hoverNode_ = -1;
  int selectedNode_ = -1;
  bool needsFit_ = true;
};
