#pragma once

#include <QElapsedTimer>
#include <QPainterPath>
#include <QPointF>
#include <QSet>
#include <QTimer>
#include <QVector>
#include <QWidget>

#include "app_model.h"

class PetriView : public QWidget {
  Q_OBJECT

 public:
  explicit PetriView(QWidget* parent = nullptr);

  void setNet(const PetriNet& net);
  void resetSimulation();
  void setAutoFire(bool enabled);
  bool autoFire() const;
  void setSpeed(double speed);
  void fitView();
  void zoomBy(double factor);

 signals:
  void nodeSelected(const QString& kind, int index);
  void statusMessage(const QString& message);

 protected:
  void paintEvent(QPaintEvent* event) override;
  void mousePressEvent(QMouseEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;
  void mouseReleaseEvent(QMouseEvent* event) override;
  void mouseDoubleClickEvent(QMouseEvent* event) override;
  void wheelEvent(QWheelEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;
  void enterEvent(QEnterEvent* event) override;
  void leaveEvent(QEvent* event) override;

 private:
  struct VisualNode {
    bool place = false;
    int index = 0;
    QPointF pos;
  };

  struct VisualEdge {
    int arc = -1;
    int from = -1;
    int to = -1;
    QPainterPath path;
    QPointF tip;
    double angle = 0.0;
  };

  struct TokenAnim {
    int edge = -1;
    double t = 0.0;
    double speed = 2.0;
  };

  struct ActiveFire {
    int transition = -1;
    double progress = 0.0;
    double duration = 0.4;
  };

  void rebuildNodes();
  void layoutNodes();
  void rebuildEdges();
  void updateEnabled();
  void tick();
  void fireTransition(int transition);
  void startFiring(int transition, bool manual);
  void zoomAt(const QPointF& screenPos, double factor);
  QPointF screenToWorld(const QPointF& screenPos) const;
  int nodeAt(const QPointF& worldPos) const;
  QPointF edgeAnchor(int nodeIndex, const QPointF& toward) const;
  QSizeF nodeSize(int nodeIndex) const;
  QColor nodeAccent(int nodeIndex) const;
  void paintNode(QPainter& painter, int nodeIndex);
  void paintEdge(QPainter& painter, const VisualEdge& edge);
  void paintEmpty(QPainter& painter);
  QString tooltipFor(int nodeIndex) const;

  PetriNet net_;
  bool hasNet_ = false;
  QVector<VisualNode> nodes_;
  QVector<VisualEdge> edges_;
  QVector<int> placeNode_;
  QVector<int> transitionNode_;
  QVector<int> marking_;
  QVector<int> initialMarking_;
  QSet<int> enabled_;
  QVector<TokenAnim> tokens_;
  bool hasActiveFire_ = false;
  ActiveFire activeFire_;
  double autoCountdown_ = 0.0;
  bool autoFire_ = false;
  double speed_ = 1.0;
  bool needsFit_ = true;

  double scale_ = 1.0;
  QPointF pan_;
  QPointF worldCenter_;
  bool dragging_ = false;
  QPointF lastMouse_;
  int hoverNode_ = -1;
  int selectedNode_ = -1;

  QTimer tickTimer_;
  QElapsedTimer clock_;
  qint64 lastTickMs_ = 0;
};
