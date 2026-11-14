#include "state_graph_view.h"

#include <QFontMetrics>
#include <QLinearGradient>
#include <QMap>
#include <QMouseEvent>
#include <QPainter>
#include <QPolygonF>
#include <QQueue>
#include <QResizeEvent>
#include <QToolTip>
#include <QWheelEvent>
#include <QtMath>

#include <algorithm>
#include <cmath>
#include <limits>

#include "theme.h"

namespace {
constexpr double kNodeWidth = 176.0;
constexpr double kNodeHeight = 76.0;
}

StateGraphView::StateGraphView(QWidget* parent) : QWidget(parent) {
  setMouseTracking(true);
  setFocusPolicy(Qt::StrongFocus);
  setMinimumSize(380, 280);
  setAttribute(Qt::WA_OpaquePaintEvent, true);
}

void StateGraphView::setGraph(const StateGraph& graph) {
  graph_ = graph;
  hasGraph_ = graph.valid;
  positions_.clear();
  levels_.clear();
  edges_.clear();
  selectedNode_ = -1;
  hoverNode_ = -1;
  needsFit_ = true;
  scale_ = 1.0;
  pan_ = QPointF();
  layoutGraph();
  rebuildEdges();
  if (isVisible()) {
    fitView();
  }
  update();
  emit statusMessage(hasGraph_ ? QStringLiteral("%1 state classes · %2 edges")
                                     .arg(graph_.nodes.size())
                                     .arg(graph_.edges.size())
                               : QStringLiteral("No state-class graph exported"));
}

void StateGraphView::layoutGraph() {
  const int count = graph_.nodes.size();
  positions_ = QVector<QPointF>(count);
  levels_ = QVector<int>(count, 0);
  if (count == 0) {
    worldCenter_ = QPointF();
    return;
  }

  QVector<QVector<int>> outgoing(count);
  QVector<int> indegree(count, 0);
  for (const StateEdge& edge : graph_.edges) {
    if (edge.from < 0 || edge.to < 0 || edge.from >= count || edge.to >= count) {
      continue;
    }
    outgoing[edge.from].push_back(edge.to);
    ++indegree[edge.to];
  }

  QQueue<int> queue;
  for (int i = 0; i < count; ++i) {
    if (indegree.at(i) == 0) {
      queue.enqueue(i);
    }
  }
  if (queue.isEmpty()) {
    queue.enqueue(0);
  }
  QVector<bool> visited(count, false);
  while (!queue.isEmpty()) {
    const int node = queue.dequeue();
    if (visited.at(node)) {
      continue;
    }
    visited[node] = true;
    for (const int target : outgoing.at(node)) {
      levels_[target] = std::max(levels_.at(target), levels_.at(node) + 1);
      if (--indegree[target] == 0) {
        queue.enqueue(target);
      }
    }
  }
  for (int i = 0; i < count; ++i) {
    if (!visited.at(i)) {
      levels_[i] = count == 1 ? 0 : (i % 5);
    }
  }

  QMap<int, QVector<int>> columns;
  for (int i = 0; i < count; ++i) {
    columns[levels_.at(i)].push_back(i);
  }
  for (auto it = columns.begin(); it != columns.end(); ++it) {
    QVector<int>& column = it.value();
    std::sort(column.begin(), column.end());
    const int size = column.size();
    for (int row = 0; row < size; ++row) {
      positions_[column.at(row)] =
          QPointF(it.key() * 224.0, (row - (size - 1) / 2.0) * 120.0);
    }
  }

  double minX = std::numeric_limits<double>::max();
  double minY = std::numeric_limits<double>::max();
  double maxX = std::numeric_limits<double>::lowest();
  double maxY = std::numeric_limits<double>::lowest();
  for (const QPointF& position : positions_) {
    minX = std::min(minX, position.x() - kNodeWidth / 2.0);
    minY = std::min(minY, position.y() - kNodeHeight / 2.0);
    maxX = std::max(maxX, position.x() + kNodeWidth / 2.0);
    maxY = std::max(maxY, position.y() + kNodeHeight / 2.0);
  }
  worldCenter_ = QPointF((minX + maxX) / 2.0, (minY + maxY) / 2.0);
}

QPointF StateGraphView::anchor(int nodeIndex, const QPointF& toward) const {
  if (nodeIndex < 0 || nodeIndex >= positions_.size()) {
    return toward;
  }
  const QPointF delta = toward - positions_.at(nodeIndex);
  double scale = 1.0;
  if (std::abs(delta.x()) > 0.001) {
    scale = std::min(scale, (kNodeWidth / 2.0 + 2.0) / std::abs(delta.x()));
  }
  if (std::abs(delta.y()) > 0.001) {
    scale = std::min(scale, (kNodeHeight / 2.0 + 2.0) / std::abs(delta.y()));
  }
  return positions_.at(nodeIndex) + delta * scale;
}

void StateGraphView::rebuildEdges() {
  edges_.clear();
  for (const StateEdge& edge : graph_.edges) {
    if (edge.from < 0 || edge.to < 0 || edge.from >= positions_.size() ||
        edge.to >= positions_.size()) {
      continue;
    }
    EdgeVisual visual;
    visual.from = edge.from;
    visual.to = edge.to;
    visual.label = edge.label;
    const QPointF start = anchor(edge.from, positions_.at(edge.to));
    const QPointF end = anchor(edge.to, positions_.at(edge.from));
    const QPointF delta = end - start;
    QPainterPath path(start);
    if (std::abs(delta.x()) >= std::abs(delta.y())) {
      path.cubicTo(start + QPointF(delta.x() * 0.45, 0.0), end - QPointF(delta.x() * 0.45, 0.0),
                   end);
    } else {
      path.cubicTo(start + QPointF(0.0, delta.y() * 0.45), end - QPointF(0.0, delta.y() * 0.45),
                   end);
    }
    visual.path = path;
    visual.tip = end;
    visual.angle = std::atan2(end.y() - start.y(), end.x() - start.x());
    edges_.push_back(visual);
  }
}

void StateGraphView::fitView() {
  if (!hasGraph_ || positions_.isEmpty()) {
    return;
  }
  double minX = std::numeric_limits<double>::max();
  double minY = std::numeric_limits<double>::max();
  double maxX = std::numeric_limits<double>::lowest();
  double maxY = std::numeric_limits<double>::lowest();
  for (const QPointF& position : positions_) {
    minX = std::min(minX, position.x() - kNodeWidth / 2.0);
    minY = std::min(minY, position.y() - kNodeHeight / 2.0);
    maxX = std::max(maxX, position.x() + kNodeWidth / 2.0);
    maxY = std::max(maxY, position.y() + kNodeHeight / 2.0);
  }
  const double w = std::max(1.0, maxX - minX);
  const double h = std::max(1.0, maxY - minY);
  const double availableWidth = std::max(120.0, width() - 130.0);
  const double availableHeight = std::max(120.0, height() - 130.0);
  scale_ = qBound(0.3, std::min(availableWidth / w, availableHeight / h), 1.4);
  worldCenter_ = QPointF((minX + maxX) / 2.0, (minY + maxY) / 2.0);
  pan_ = QPointF();
  needsFit_ = false;
  update();
}

void StateGraphView::zoomBy(double factor) {
  zoomAt(QPointF(width() / 2.0, height() / 2.0), factor);
}

void StateGraphView::zoomAt(const QPointF& screenPos, double factor) {
  const QPointF before = screenToWorld(screenPos);
  scale_ = qBound(0.22, scale_ * factor, 3.0);
  const QPointF center(width() / 2.0, height() / 2.0);
  pan_ = screenPos - center - (before - worldCenter_) * scale_;
  update();
}

QPointF StateGraphView::screenToWorld(const QPointF& screenPos) const {
  const QPointF center(width() / 2.0 + pan_.x(), height() / 2.0 + pan_.y());
  return worldCenter_ + (screenPos - center) / scale_;
}

int StateGraphView::nodeAt(const QPointF& worldPos) const {
  for (int i = positions_.size() - 1; i >= 0; --i) {
    const QRectF rect(positions_.at(i).x() - kNodeWidth / 2.0,
                      positions_.at(i).y() - kNodeHeight / 2.0, kNodeWidth, kNodeHeight);
    if (rect.contains(worldPos)) {
      return i;
    }
  }
  return -1;
}

void StateGraphView::paintEvent(QPaintEvent*) {
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing, true);

  QLinearGradient background(0, 0, width(), height());
  background.setColorAt(0.0, theme::c("#050910"));
  background.setColorAt(0.5, theme::c("#080F1B"));
  background.setColorAt(1.0, theme::c("#05080F"));
  painter.fillRect(rect(), background);

  QRadialGradient glow(QPointF(width() * 0.62, height() * 0.35), std::max(width(), height()) * 0.65);
  glow.setColorAt(0.0, theme::alpha(theme::c("#7C3AED"), 28));
  glow.setColorAt(0.7, theme::alpha(theme::c("#7C3AED"), 4));
  glow.setColorAt(1.0, Qt::transparent);
  painter.fillRect(rect(), glow);

  painter.save();
  painter.setTransform(QTransform());
  const int grid = 32;
  const QPointF offset(std::fmod(pan_.x(), static_cast<double>(grid)),
                       std::fmod(pan_.y(), static_cast<double>(grid)));
  painter.setPen(QPen(theme::alpha(theme::c("#1E293B"), 80), 1.0));
  for (double x = offset.x(); x < width(); x += grid) {
    for (double y = offset.y(); y < height(); y += grid) {
      painter.drawPoint(QPointF(x, y));
    }
  }
  painter.restore();

  if (!hasGraph_) {
    painter.setFont(theme::display(17, QFont::Bold));
    painter.setPen(theme::c("#D9F5FF"));
    painter.drawText(rect().adjusted(0, -20, 0, -20), Qt::AlignCenter,
                     QStringLiteral("STATE SPACE"));
    painter.setFont(theme::ui(9));
    painter.setPen(theme::c("#5F7790"));
    painter.drawText(rect().adjusted(0, 24, 0, 24), Qt::AlignCenter,
                     QStringLiteral("Run an analysis to inspect the state-class graph"));
    return;
  }

  QTransform transform;
  transform.translate(width() / 2.0 + pan_.x(), height() / 2.0 + pan_.y());
  transform.scale(scale_, scale_);
  transform.translate(-worldCenter_.x(), -worldCenter_.y());
  painter.setTransform(transform);

  for (const EdgeVisual& edge : edges_) {
    const bool incident = selectedNode_ >= 0 && (edge.from == selectedNode_ || edge.to == selectedNode_);
    QColor color = theme::c("#8B5CF6");
    if (incident) {
      color = theme::c("#C4B5FD");
    }
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(theme::alpha(color, 35), 6.0, Qt::SolidLine, Qt::RoundCap));
    painter.drawPath(edge.path);
    painter.setPen(QPen(theme::alpha(color, 190), 1.4, Qt::SolidLine, Qt::RoundCap));
    painter.drawPath(edge.path);

    const double size = 9.0;
    QPolygonF head;
    head << edge.tip
         << edge.tip + QPointF(std::cos(edge.angle + 2.55) * size, std::sin(edge.angle + 2.55) * size)
         << edge.tip + QPointF(std::cos(edge.angle - 2.55) * size, std::sin(edge.angle - 2.55) * size);
    painter.setPen(Qt::NoPen);
    painter.setBrush(theme::alpha(color, 210));
    painter.drawPolygon(head);

    if (!edge.label.isEmpty()) {
      const QPointF middle = edge.path.pointAtPercent(0.5);
      painter.setFont(theme::mono(6.5));
      const QString label = painter.fontMetrics().elidedText(edge.label, Qt::ElideRight, 120);
      const double textWidth = painter.fontMetrics().horizontalAdvance(label);
      const QRectF bubble(middle.x() - textWidth / 2.0 - 5, middle.y() - 8, textWidth + 10, 16);
      painter.setPen(Qt::NoPen);
      painter.setBrush(theme::alpha(theme::c("#060C16"), 210));
      painter.drawRoundedRect(bubble, 4, 4);
      painter.setPen(theme::c("#93A8C5"));
      painter.drawText(bubble, Qt::AlignCenter, label);
    }
  }

  for (int i = 0; i < graph_.nodes.size(); ++i) {
    if (i != selectedNode_ && i != hoverNode_) {
      paintNode(painter, i);
    }
  }
  if (hoverNode_ >= 0 && hoverNode_ != selectedNode_) {
    paintNode(painter, hoverNode_);
  }
  if (selectedNode_ >= 0) {
    paintNode(painter, selectedNode_);
  }

  painter.resetTransform();
  painter.setFont(theme::mono(7, QFont::Medium));
  painter.setPen(theme::alpha(theme::c("#52627A"), 220));
  painter.drawText(QRect(18, height() - 26, width() - 36, 16), Qt::AlignLeft,
                   QStringLiteral("STATE CLASSES  ·  %1 states  /  %2 transitions")
                       .arg(graph_.nodes.size())
                       .arg(graph_.edges.size()));
  painter.drawText(QRect(18, height() - 26, width() - 36, 16), Qt::AlignRight,
                   QStringLiteral("DBM ZONES"));
}

void StateGraphView::paintNode(QPainter& painter, int index) {
  if (index < 0 || index >= graph_.nodes.size()) {
    return;
  }
  const StateNode& node = graph_.nodes.at(index);
  const QPointF position = positions_.at(index);
  const bool selected = index == selectedNode_;
  const bool hovered = index == hoverNode_;
  const QRectF body(position.x() - kNodeWidth / 2.0, position.y() - kNodeHeight / 2.0, kNodeWidth,
                    kNodeHeight);

  painter.save();
  painter.translate(position);
  const int hue = (levels_.value(index) * 31 + 190) % 360;
  QColor accent = QColor::fromHsv(hue, 170, 235);

  if (selected || hovered) {
    QRadialGradient halo(QPointF(0, 0), 118.0);
    halo.setColorAt(0.0, theme::alpha(accent, selected ? 50 : 25));
    halo.setColorAt(1.0, Qt::transparent);
    painter.setPen(Qt::NoPen);
    painter.setBrush(halo);
    painter.drawEllipse(QPointF(0, 0), 118.0, 118.0);
  }

  QLinearGradient fill(body.topLeft(), body.bottomRight());
  fill.setColorAt(0.0, theme::c("#111C2D"));
  fill.setColorAt(1.0, theme::c("#0A111D"));
  painter.setBrush(fill);
  painter.setPen(QPen(selected ? theme::c("#A5F3FC") : theme::alpha(accent, hovered ? 230 : 120),
                      selected ? 2.4 : 1.2));
  painter.drawRoundedRect(QRectF(-kNodeWidth / 2.0, -kNodeHeight / 2.0, kNodeWidth, kNodeHeight),
                          12, 12);

  painter.setFont(theme::display(11, QFont::Bold));
  painter.setPen(theme::c("#E6F7FF"));
  painter.drawText(QRectF(-kNodeWidth / 2.0 + 12, -kNodeHeight / 2.0 + 9, 54, 20),
                   Qt::AlignLeft | Qt::AlignVCenter,
                   QStringLiteral("S%1").arg(node.index));

  painter.setFont(theme::mono(6.5, QFont::Medium));
  painter.setPen(theme::alpha(accent, 230));
  const QString elapsed = node.elapsed.isEmpty() ? QStringLiteral("—") : node.elapsed;
  painter.drawText(QRectF(kNodeWidth / 2.0 - 64, -kNodeHeight / 2.0 + 9, 52, 18),
                   Qt::AlignRight | Qt::AlignVCenter, QStringLiteral("t=%1").arg(elapsed));

  painter.setFont(theme::ui(8));
  painter.setPen(theme::c("#A8C5D8"));
  const QString marking = node.marking.isEmpty() ? QStringLiteral("M = []") : node.marking;
  const QString markingLine =
      painter.fontMetrics().elidedText(marking, Qt::ElideRight, static_cast<int>(kNodeWidth - 24));
  painter.drawText(QRectF(-kNodeWidth / 2.0 + 12, -6, kNodeWidth - 24, 16),
                   Qt::AlignLeft | Qt::AlignVCenter, markingLine);

  painter.setFont(theme::mono(6.5));
  painter.setPen(theme::c("#64748B"));
  const QString active = node.active.isEmpty() ? QStringLiteral("idle") : node.active;
  const QString activeLine =
      painter.fontMetrics().elidedText(active, Qt::ElideRight, static_cast<int>(kNodeWidth - 24));
  painter.drawText(QRectF(-kNodeWidth / 2.0 + 12, 14, kNodeWidth - 24, 14),
                   Qt::AlignLeft | Qt::AlignVCenter, activeLine);

  if (!node.suspended.isEmpty() && node.suspended != QStringLiteral("(none)")) {
    painter.setPen(theme::c("#F59E0B"));
    painter.setFont(theme::mono(6));
    painter.drawText(QRectF(-kNodeWidth / 2.0 + 12, 28, kNodeWidth - 24, 12),
                     Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("SUSPENDED"));
  }

  painter.restore();
}

void StateGraphView::mousePressEvent(QMouseEvent* event) {
  if (event->button() != Qt::LeftButton) {
    return;
  }
  const int node = nodeAt(screenToWorld(event->position()));
  if (node >= 0) {
    selectedNode_ = node;
    emit nodeSelected(node);
    update();
    return;
  }
  dragging_ = true;
  lastMouse_ = event->position();
  setCursor(Qt::ClosedHandCursor);
}

void StateGraphView::mouseMoveEvent(QMouseEvent* event) {
  if (dragging_) {
    pan_ += event->position() - lastMouse_;
    lastMouse_ = event->position();
    update();
    return;
  }
  const int node = nodeAt(screenToWorld(event->position()));
  if (node != hoverNode_) {
    hoverNode_ = node;
    if (node >= 0) {
      const StateNode& state = graph_.nodes.at(node);
      QString text = QStringLiteral("State %1").arg(state.index);
      if (!state.marking.isEmpty()) {
        text += QStringLiteral("\n%1").arg(state.marking);
      }
      if (!state.active.isEmpty()) {
        text += QStringLiteral("\nActive: %1").arg(state.active);
      }
      if (!state.suspended.isEmpty()) {
        text += QStringLiteral("\nSuspended: %1").arg(state.suspended);
      }
      QToolTip::showText(event->globalPosition().toPoint(), text, this);
    } else {
      QToolTip::hideText();
    }
    setCursor(node >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
    update();
  }
}

void StateGraphView::mouseReleaseEvent(QMouseEvent* event) {
  if (event->button() == Qt::LeftButton) {
    dragging_ = false;
    setCursor(Qt::ArrowCursor);
  }
}

void StateGraphView::mouseDoubleClickEvent(QMouseEvent* event) {
  if (event->button() == Qt::LeftButton && nodeAt(screenToWorld(event->position())) < 0) {
    fitView();
  }
}

void StateGraphView::wheelEvent(QWheelEvent* event) {
  zoomAt(event->position(), std::pow(1.0015, event->angleDelta().y()));
}

void StateGraphView::resizeEvent(QResizeEvent* event) {
  QWidget::resizeEvent(event);
  if (needsFit_) {
    fitView();
  }
}

void StateGraphView::leaveEvent(QEvent* event) {
  hoverNode_ = -1;
  QToolTip::hideText();
  update();
  QWidget::leaveEvent(event);
}
