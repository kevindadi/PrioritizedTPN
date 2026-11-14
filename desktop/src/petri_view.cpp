#include "petri_view.h"

#include <QFontMetrics>
#include <QEnterEvent>
#include <QLineF>
#include <QLinearGradient>
#include <QMap>
#include <QMouseEvent>
#include <QPainter>
#include <QPolygonF>
#include <QQueue>
#include <QRadialGradient>
#include <QRandomGenerator>
#include <QResizeEvent>
#include <QToolTip>
#include <QWheelEvent>
#include <QtMath>

#include <algorithm>
#include <cmath>
#include <limits>

#include "theme.h"

namespace {
constexpr double kPlaceRadius = 28.0;
constexpr double kTransitionWidth = 136.0;
constexpr double kTransitionHeight = 58.0;
constexpr double kPi = 3.14159265358979323846;

QString laneKey(const QString& id) {
  QString key;
  for (const QChar ch : id) {
    if (ch.isUpper() || ch.isDigit()) {
      key += ch;
    } else {
      break;
    }
  }
  return key.isEmpty() ? id.left(1) : key;
}

QString compactInterval(const Transition& transition) {
  if (!transition.hasInterval) {
    return QStringLiteral("untimed");
  }
  auto number = [](double value) {
    if (std::abs(value - std::round(value)) < 0.001) {
      return QString::number(static_cast<long long>(std::llround(value)));
    }
    return QString::number(value, 'f', 1);
  };
  return QStringLiteral("[%1, %2]").arg(number(transition.intervalMin), number(transition.intervalMax));
}
}  // namespace

PetriView::PetriView(QWidget* parent) : QWidget(parent) {
  setMouseTracking(true);
  setFocusPolicy(Qt::StrongFocus);
  setMinimumSize(380, 280);
  setAttribute(Qt::WA_OpaquePaintEvent, true);
  tickTimer_.setTimerType(Qt::PreciseTimer);
  tickTimer_.setInterval(16);
  connect(&tickTimer_, &QTimer::timeout, this, &PetriView::tick);
  tickTimer_.start();
  clock_.start();
  lastTickMs_ = clock_.elapsed();
}

void PetriView::setNet(const PetriNet& net) {
  net_ = net;
  hasNet_ = net.valid;
  nodes_.clear();
  edges_.clear();
  tokens_.clear();
  hasActiveFire_ = false;
  hoverNode_ = -1;
  selectedNode_ = -1;
  placeNode_ = QVector<int>(net_.places.size(), -1);
  transitionNode_ = QVector<int>(net_.transitions.size(), -1);
  rebuildNodes();
  layoutNodes();
  rebuildEdges();
  initialMarking_ = net_.initialMarking;
  initialMarking_.resize(net_.places.size());
  marking_ = initialMarking_;
  for (int i = 0; i < marking_.size(); ++i) {
    if (marking_.at(i) <= 0) {
      marking_[i] = 0;
    }
  }
  updateEnabled();
  needsFit_ = true;
  scale_ = 1.0;
  pan_ = QPointF();
  if (isVisible()) {
    fitView();
  }
  update();
  emit statusMessage(hasNet_ ? QStringLiteral("%1 places · %2 transitions · %3 arcs")
                                   .arg(net_.places.size())
                                   .arg(net_.transitions.size())
                                   .arg(net_.arcs.size())
                             : QStringLiteral("No Petri structure"));
}

void PetriView::rebuildNodes() {
  for (int i = 0; i < net_.places.size(); ++i) {
    VisualNode node;
    node.place = true;
    node.index = i;
    placeNode_[i] = nodes_.size();
    nodes_.push_back(node);
  }
  for (int i = 0; i < net_.transitions.size(); ++i) {
    VisualNode node;
    node.place = false;
    node.index = i;
    transitionNode_[i] = nodes_.size();
    nodes_.push_back(node);
  }
}

QSizeF PetriView::nodeSize(int nodeIndex) const {
  if (nodeIndex < 0 || nodeIndex >= nodes_.size()) {
    return {};
  }
  return nodes_.at(nodeIndex).place ? QSizeF(kPlaceRadius * 2.0, kPlaceRadius * 2.0)
                                    : QSizeF(kTransitionWidth, kTransitionHeight);
}

void PetriView::layoutNodes() {
  const int count = nodes_.size();
  if (count == 0) {
    worldCenter_ = QPointF();
    return;
  }

  QVector<QVector<int>> outgoing(count);
  QVector<int> indegree(count, 0);
  for (const PetriArc& arc : net_.arcs) {
    const int from = arc.fromPlace ? placeNode_.value(arc.from, -1) : transitionNode_.value(arc.from, -1);
    const int to = arc.fromPlace ? transitionNode_.value(arc.to, -1) : placeNode_.value(arc.to, -1);
    if (from < 0 || to < 0) {
      continue;
    }
    // Layout should not chase cycles through initially marked places: those
    // places are natural roots of the token flow (periodic release, initial
    // tokens), so ignore arcs that would make them depend on a transition.
    if (!arc.fromPlace && arc.to >= 0 && arc.to < net_.initialMarking.size() &&
        net_.initialMarking.at(arc.to) > 0) {
      continue;
    }
    outgoing[from].push_back(to);
    ++indegree[to];
  }

  QVector<int> level(count, 0);
  QQueue<int> queue;
  for (int i = 0; i < count; ++i) {
    if (indegree.at(i) == 0) {
      queue.enqueue(i);
    }
  }
  while (!queue.isEmpty()) {
    const int node = queue.dequeue();
    for (const int target : outgoing.at(node)) {
      level[target] = std::max(level.at(target), level.at(node) + 1);
      if (--indegree[target] == 0) {
        queue.enqueue(target);
      }
    }
  }

  auto idOf = [&](int nodeIndex) -> QString {
    const VisualNode& node = nodes_.at(nodeIndex);
    return node.place ? net_.places.at(node.index).id : net_.transitions.at(node.index).id;
  };

  QMap<QString, QVector<int>> lanes;
  for (int i = 0; i < count; ++i) {
    lanes[laneKey(idOf(i))].push_back(i);
  }

  if (lanes.size() > 1 && lanes.size() <= 6) {
    // Task lanes read much better than one long alternating column: each task
    // chain becomes a horizontal row, with control/dependency arcs between rows.
    int laneIndex = 0;
    for (auto it = lanes.begin(); it != lanes.end(); ++it) {
      QVector<int>& lane = it.value();
      std::sort(lane.begin(), lane.end(), [&](int lhs, int rhs) {
        if (level.at(lhs) != level.at(rhs)) {
          return level.at(lhs) < level.at(rhs);
        }
        return idOf(lhs) < idOf(rhs);
      });
      for (int index = 0; index < lane.size(); ++index) {
        nodes_[lane.at(index)].pos =
            QPointF(index * 165.0, (laneIndex - (lanes.size() - 1) / 2.0) * 145.0);
      }
      ++laneIndex;
    }
  } else {
    QMap<int, QVector<int>> columns;
    for (int i = 0; i < count; ++i) {
      columns[level.at(i)].push_back(i);
    }

    const double columnSpacing = 185.0;
    const double rowSpacing = 100.0;
    for (auto it = columns.begin(); it != columns.end(); ++it) {
      QVector<int>& column = it.value();
      std::sort(column.begin(), column.end(), [&](int lhs, int rhs) {
        const QString leftLane = laneKey(idOf(lhs));
        const QString rightLane = laneKey(idOf(rhs));
        if (leftLane != rightLane) {
          return leftLane < rightLane;
        }
        return idOf(lhs) < idOf(rhs);
      });
      const int size = column.size();
      for (int row = 0; row < size; ++row) {
        VisualNode& node = nodes_[column.at(row)];
        node.pos = QPointF(it.key() * columnSpacing, (row - (size - 1) / 2.0) * rowSpacing);
      }
    }
  }

  QPointF minPoint(std::numeric_limits<double>::max(), std::numeric_limits<double>::max());
  QPointF maxPoint(std::numeric_limits<double>::lowest(), std::numeric_limits<double>::lowest());
  for (int i = 0; i < count; ++i) {
    const QSizeF size = nodeSize(i);
    minPoint.setX(std::min(minPoint.x(), nodes_.at(i).pos.x() - size.width() / 2.0));
    minPoint.setY(std::min(minPoint.y(), nodes_.at(i).pos.y() - size.height() / 2.0));
    maxPoint.setX(std::max(maxPoint.x(), nodes_.at(i).pos.x() + size.width() / 2.0));
    maxPoint.setY(std::max(maxPoint.y(), nodes_.at(i).pos.y() + size.height() / 2.0));
  }
  worldCenter_ = (minPoint + maxPoint) / 2.0;
}

QPointF PetriView::edgeAnchor(int nodeIndex, const QPointF& toward) const {
  if (nodeIndex < 0 || nodeIndex >= nodes_.size()) {
    return toward;
  }
  const VisualNode& node = nodes_.at(nodeIndex);
  const QPointF delta = toward - node.pos;
  if (node.place) {
    const double length = std::hypot(delta.x(), delta.y());
    if (length < 0.001) {
      return node.pos;
    }
    return node.pos + delta / length * (kPlaceRadius + 1.0);
  }
  const double dx = delta.x();
  const double dy = delta.y();
  const double halfWidth = kTransitionWidth / 2.0;
  const double halfHeight = kTransitionHeight / 2.0;
  double scale = 1.0;
  if (std::abs(dx) > 0.001) {
    scale = std::min(scale, halfWidth / std::abs(dx));
  }
  if (std::abs(dy) > 0.001) {
    scale = std::min(scale, halfHeight / std::abs(dy));
  }
  return node.pos + delta * scale;
}

void PetriView::rebuildEdges() {
  edges_.clear();
  for (int i = 0; i < net_.arcs.size(); ++i) {
    const PetriArc& arc = net_.arcs.at(i);
    const int from = arc.fromPlace ? placeNode_.value(arc.from, -1) : transitionNode_.value(arc.from, -1);
    const int to = arc.fromPlace ? transitionNode_.value(arc.to, -1) : placeNode_.value(arc.to, -1);
    if (from < 0 || to < 0) {
      continue;
    }
    VisualEdge edge;
    edge.arc = i;
    edge.from = from;
    edge.to = to;
    const QPointF start = edgeAnchor(from, nodes_.at(to).pos);
    const QPointF end = edgeAnchor(to, nodes_.at(from).pos);
    const QPointF delta = end - start;
    QPainterPath path(start);
    if (std::abs(delta.x()) >= std::abs(delta.y())) {
      const double direction = delta.x() >= 0 ? 1.0 : -1.0;
      path.cubicTo(start + QPointF(delta.x() * 0.45 * direction, 0.0),
                   end - QPointF(delta.x() * 0.45 * direction, 0.0), end);
    } else {
      const double direction = delta.y() >= 0 ? 1.0 : -1.0;
      path.cubicTo(start + QPointF(0.0, delta.y() * 0.45 * direction),
                   end - QPointF(0.0, delta.y() * 0.45 * direction), end);
    }
    edge.path = path;
    edge.tip = end;
    edge.angle = std::atan2(end.y() - start.y(), end.x() - start.x());
    edges_.push_back(edge);
  }
}

void PetriView::updateEnabled() {
  enabled_.clear();
  for (int t = 0; t < net_.transitions.size(); ++t) {
    bool enabled = true;
    bool hasInput = false;
    for (const PetriArc& arc : net_.arcs) {
      if (arc.fromPlace && arc.to == t) {
        hasInput = true;
        if (arc.from < 0 || arc.from >= marking_.size() || marking_.at(arc.from) <= 0) {
          enabled = false;
          break;
        }
      }
    }
    if (hasInput && enabled) {
      enabled_.insert(t);
    }
  }
}

void PetriView::resetSimulation() {
  marking_ = initialMarking_;
  tokens_.clear();
  hasActiveFire_ = false;
  autoCountdown_ = 0.0;
  selectedNode_ = -1;
  updateEnabled();
  update();
  emit statusMessage(QStringLiteral("Initial marking restored"));
}

void PetriView::setAutoFire(bool enabled) {
  autoFire_ = enabled;
  autoCountdown_ = 0.0;
  emit statusMessage(enabled ? QStringLiteral("Auto-firing enabled") : QStringLiteral("Auto-firing paused"));
}

bool PetriView::autoFire() const { return autoFire_; }

void PetriView::setSpeed(double speed) { speed_ = qBound(0.25, speed, 4.0); }

void PetriView::tick() {
  const qint64 now = clock_.elapsed();
  double delta = (now - lastTickMs_) / 1000.0;
  lastTickMs_ = now;
  delta = qBound(0.0, delta, 0.08) * speed_;
  if (!hasNet_) {
    return;
  }

  if (hasActiveFire_) {
    activeFire_.progress += delta / activeFire_.duration;
    if (activeFire_.progress >= 1.0) {
      const int transition = activeFire_.transition;
      hasActiveFire_ = false;
      fireTransition(transition);
      autoCountdown_ = 0.55 / speed_;
    }
  }

  for (int i = tokens_.size() - 1; i >= 0; --i) {
    tokens_[i].t += delta * tokens_.at(i).speed;
    if (tokens_.at(i).t >= 1.0) {
      tokens_.removeAt(i);
    }
  }

  if (autoFire_ && !hasActiveFire_) {
    autoCountdown_ -= delta;
    if (autoCountdown_ <= 0.0) {
      int candidate = -1;
      int bestPriority = std::numeric_limits<int>::min();
      for (const int transition : enabled_) {
        const int priority = net_.transitions.at(transition).priority;
        if (priority > bestPriority) {
          bestPriority = priority;
          candidate = transition;
        } else if (priority == bestPriority && candidate >= 0) {
          if (QRandomGenerator::global()->bounded(2) == 0) {
            candidate = transition;
          }
        }
      }
      if (candidate >= 0) {
        startFiring(candidate, false);
      } else {
        autoCountdown_ = 0.4;
      }
    }
  }

  update();
}

void PetriView::startFiring(int transition, bool manual) {
  if (transition < 0 || transition >= net_.transitions.size() || hasActiveFire_ ||
      !enabled_.contains(transition)) {
    return;
  }
  const Transition& info = net_.transitions.at(transition);
  double duration = manual ? 0.22 : 0.34;
  if (!manual && info.hasInterval) {
    duration = qBound(0.3, info.intervalMin / 18.0, 1.2);
  }
  activeFire_.transition = transition;
  activeFire_.progress = 0.0;
  activeFire_.duration = duration / qMax(0.25, speed_);
  hasActiveFire_ = true;
  selectedNode_ = transitionNode_.value(transition, -1);
  emit statusMessage(QStringLiteral("Firing %1").arg(info.id));
  emit nodeSelected(QStringLiteral("transition"), transition);
  update();
}

void PetriView::fireTransition(int transition) {
  QVector<int> touchedEdges;
  for (const PetriArc& arc : net_.arcs) {
    if (arc.fromPlace && arc.to == transition) {
      if (arc.from >= 0 && arc.from < marking_.size()) {
        marking_[arc.from] = std::max(0, marking_.at(arc.from) - 1);
      }
    } else if (!arc.fromPlace && arc.from == transition) {
      if (arc.to >= 0 && arc.to < marking_.size()) {
        marking_[arc.to] = marking_.at(arc.to) + 1;
      }
    }
  }

  for (int i = 0; i < edges_.size(); ++i) {
    const VisualEdge& edge = edges_.at(i);
    const PetriArc& arc = net_.arcs.at(edge.arc);
    if ((arc.fromPlace && arc.to == transition) || (!arc.fromPlace && arc.from == transition)) {
      touchedEdges.push_back(i);
      TokenAnim token;
      token.edge = i;
      token.t = 0.0;
      token.speed = 1.9;
      tokens_.push_back(token);
    }
  }

  updateEnabled();
  emit statusMessage(QStringLiteral("%1 fired · %2 token(s) moved")
                         .arg(net_.transitions.at(transition).id)
                         .arg(touchedEdges.size()));
  update();
}

void PetriView::fitView() {
  if (!hasNet_ || nodes_.isEmpty()) {
    return;
  }
  double minX = std::numeric_limits<double>::max();
  double minY = std::numeric_limits<double>::max();
  double maxX = std::numeric_limits<double>::lowest();
  double maxY = std::numeric_limits<double>::lowest();
  for (int i = 0; i < nodes_.size(); ++i) {
    const QSizeF size = nodeSize(i);
    minX = std::min(minX, nodes_.at(i).pos.x() - size.width() / 2.0);
    minY = std::min(minY, nodes_.at(i).pos.y() - size.height() / 2.0);
    maxX = std::max(maxX, nodes_.at(i).pos.x() + size.width() / 2.0);
    maxY = std::max(maxY, nodes_.at(i).pos.y() + size.height() / 2.0);
  }
  const double width = std::max(1.0, maxX - minX);
  const double height = std::max(1.0, maxY - minY);
  const double availableWidth = std::max(120.0, this->width() - 100.0);
  const double availableHeight = std::max(120.0, this->height() - 100.0);
  scale_ = qBound(0.32, std::min(availableWidth / width, availableHeight / height), 1.45);
  worldCenter_ = QPointF((minX + maxX) / 2.0, (minY + maxY) / 2.0);
  pan_ = QPointF();
  needsFit_ = false;
  update();
}

void PetriView::zoomBy(double factor) {
  zoomAt(QPointF(width() / 2.0, height() / 2.0), factor);
}

void PetriView::zoomAt(const QPointF& screenPos, double factor) {
  const QPointF before = screenToWorld(screenPos);
  scale_ = qBound(0.25, scale_ * factor, 3.2);
  const QPointF center(width() / 2.0, height() / 2.0);
  pan_ = screenPos - center - (before - worldCenter_) * scale_;
  update();
}

QPointF PetriView::screenToWorld(const QPointF& screenPos) const {
  const QPointF center(width() / 2.0 + pan_.x(), height() / 2.0 + pan_.y());
  return worldCenter_ + (screenPos - center) / scale_;
}

int PetriView::nodeAt(const QPointF& worldPos) const {
  for (int i = nodes_.size() - 1; i >= 0; --i) {
    const VisualNode& node = nodes_.at(i);
    if (node.place) {
      if (QLineF(worldPos, node.pos).length() <= kPlaceRadius + 5.0) {
        return i;
      }
    } else {
      const QRectF rect(node.pos.x() - kTransitionWidth / 2.0,
                        node.pos.y() - kTransitionHeight / 2.0, kTransitionWidth,
                        kTransitionHeight);
      if (rect.contains(worldPos)) {
        return i;
      }
    }
  }
  return -1;
}

QColor PetriView::nodeAccent(int nodeIndex) const {
  if (nodeIndex < 0 || nodeIndex >= nodes_.size()) {
    return theme::c("#22D3EE");
  }
  const VisualNode& node = nodes_.at(nodeIndex);
  if (node.place) {
    return theme::c("#22D3EE");
  }
  return enabled_.contains(node.index) ? theme::c("#F472B6") : theme::c("#F59E0B");
}

QString PetriView::tooltipFor(int nodeIndex) const {
  if (nodeIndex < 0 || nodeIndex >= nodes_.size()) {
    return {};
  }
  const VisualNode& node = nodes_.at(nodeIndex);
  if (node.place) {
    const Place& place = net_.places.at(node.index);
    return QStringLiteral("%1\nTokens: %2%3")
        .arg(place.id)
        .arg(marking_.value(node.index, 0))
        .arg(place.capacity > 0 ? QStringLiteral(" / %1").arg(place.capacity) : QString());
  }
  const Transition& transition = net_.transitions.at(node.index);
  return QStringLiteral("%1\ncore %2 · priority %3\nwindow %4%5")
      .arg(transition.id)
      .arg(transition.core)
      .arg(transition.priority)
      .arg(compactInterval(transition))
      .arg(transition.suspendable ? QStringLiteral(" · suspendable") : QString());
}

void PetriView::paintEvent(QPaintEvent*) {
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing, true);
  painter.setRenderHint(QPainter::TextAntialiasing, true);

  QLinearGradient background(0, 0, width(), height());
  background.setColorAt(0.0, theme::c("#060A13"));
  background.setColorAt(0.52, theme::c("#08101D"));
  background.setColorAt(1.0, theme::c("#050810"));
  painter.fillRect(rect(), background);

  QRadialGradient glow(QPointF(width() * 0.55, height() * 0.42), std::max(width(), height()) * 0.7);
  glow.setColorAt(0.0, theme::alpha(theme::c("#0E7490"), 38));
  glow.setColorAt(0.6, theme::alpha(theme::c("#0E7490"), 8));
  glow.setColorAt(1.0, Qt::transparent);
  painter.fillRect(rect(), glow);

  painter.save();
  painter.setTransform(QTransform());
  const int grid = 30;
  const QPointF offset(std::fmod(pan_.x(), static_cast<double>(grid)),
                       std::fmod(pan_.y(), static_cast<double>(grid)));
  painter.setPen(QPen(theme::alpha(theme::c("#1E293B"), 95), 1.0));
  for (double x = offset.x(); x < width(); x += grid) {
    for (double y = offset.y(); y < height(); y += grid) {
      painter.drawPoint(QPointF(x, y));
    }
  }
  painter.restore();

  if (!hasNet_) {
    paintEmpty(painter);
    return;
  }

  QTransform transform;
  transform.translate(width() / 2.0 + pan_.x(), height() / 2.0 + pan_.y());
  transform.scale(scale_, scale_);
  transform.translate(-worldCenter_.x(), -worldCenter_.y());
  painter.setTransform(transform);

  for (const VisualEdge& edge : edges_) {
    paintEdge(painter, edge);
  }

  for (const TokenAnim& token : tokens_) {
    if (token.edge < 0 || token.edge >= edges_.size()) {
      continue;
    }
    const QPointF point = edges_.at(token.edge).path.pointAtPercent(qBound(0.0, token.t, 1.0));
    QRadialGradient tokenGlow(point, 13.0);
    tokenGlow.setColorAt(0.0, theme::alpha(theme::c("#E0F2FE"), 230));
    tokenGlow.setColorAt(0.3, theme::alpha(theme::c("#22D3EE"), 160));
    tokenGlow.setColorAt(1.0, Qt::transparent);
    painter.setPen(Qt::NoPen);
    painter.setBrush(tokenGlow);
    painter.drawEllipse(point, 13.0, 13.0);
    painter.setBrush(theme::c("#ECFEFF"));
    painter.drawEllipse(point, 3.4, 3.4);
  }

  for (int i = 0; i < nodes_.size(); ++i) {
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
                   QStringLiteral("PETRI NET  ·  %1 P  /  %2 T  /  %3 A")
                       .arg(net_.places.size())
                       .arg(net_.transitions.size())
                       .arg(net_.arcs.size()));
  painter.drawText(QRect(18, height() - 26, width() - 36, 16), Qt::AlignRight,
                   autoFire_ ? QStringLiteral("AUTO-FIRE ON") : QStringLiteral("AUTO-FIRE OFF"));
}

void PetriView::paintEdge(QPainter& painter, const VisualEdge& edge) {
  const bool incident =
      (selectedNode_ >= 0 && (edge.from == selectedNode_ || edge.to == selectedNode_)) ||
      (hoverNode_ >= 0 && (edge.from == hoverNode_ || edge.to == hoverNode_));
  const bool firing =
      hasActiveFire_ &&
      ((nodes_.at(edge.from).place == false && nodes_.at(edge.from).index == activeFire_.transition) ||
       (nodes_.at(edge.to).place == false && nodes_.at(edge.to).index == activeFire_.transition));
  QColor color = nodeAccent(edge.from);
  if (firing) {
    color = theme::c("#F472B6");
  } else if (incident) {
    color = theme::c("#A5F3FC");
  }
  const double intensity = incident || firing ? 1.0 : 0.62;

  painter.setBrush(Qt::NoBrush);
  painter.setPen(QPen(theme::alpha(color, static_cast<int>(40 * intensity)), firing ? 9.0 : 6.0,
                      Qt::SolidLine, Qt::RoundCap));
  painter.drawPath(edge.path);
  painter.setPen(QPen(theme::alpha(color, static_cast<int>(220 * intensity)),
                      incident || firing ? 2.2 : 1.45, Qt::SolidLine, Qt::RoundCap));
  painter.drawPath(edge.path);

  const double size = incident || firing ? 11.0 : 8.5;
  QPolygonF head;
  head << edge.tip
       << edge.tip + QPointF(std::cos(edge.angle + 2.55) * size, std::sin(edge.angle + 2.55) * size)
       << edge.tip + QPointF(std::cos(edge.angle - 2.55) * size, std::sin(edge.angle - 2.55) * size);
  painter.setPen(Qt::NoPen);
  painter.setBrush(theme::alpha(color, static_cast<int>(235 * intensity)));
  painter.drawPolygon(head);
}

void PetriView::paintNode(QPainter& painter, int nodeIndex) {
  if (nodeIndex < 0 || nodeIndex >= nodes_.size()) {
    return;
  }
  const VisualNode& node = nodes_.at(nodeIndex);
  const bool selected = nodeIndex == selectedNode_;
  const bool hovered = nodeIndex == hoverNode_;
  painter.save();
  painter.translate(node.pos);

  if (node.place) {
    const Place& place = net_.places.at(node.index);
    QRadialGradient halo(QPointF(0, 0), selected ? 58.0 : 48.0);
    const QColor accent = theme::c("#22D3EE");
    halo.setColorAt(0.0, theme::alpha(accent, selected ? 90 : 54));
    halo.setColorAt(0.55, theme::alpha(accent, 16));
    halo.setColorAt(1.0, Qt::transparent);
    painter.setPen(Qt::NoPen);
    painter.setBrush(halo);
    painter.drawEllipse(QPointF(0, 0), selected ? 58.0 : 48.0, selected ? 58.0 : 48.0);

    QRadialGradient fill(QPointF(-8, -10), kPlaceRadius * 1.8);
    fill.setColorAt(0.0, theme::c("#123247"));
    fill.setColorAt(0.6, theme::c("#0A1B2A"));
    fill.setColorAt(1.0, theme::c("#06101C"));
    painter.setBrush(fill);
    painter.setPen(QPen(selected ? theme::c("#A5F3FC") : theme::alpha(accent, hovered ? 245 : 190),
                        selected ? 2.8 : 1.6));
    painter.drawEllipse(QPointF(0, 0), kPlaceRadius, kPlaceRadius);

    const int count = marking_.value(node.index, 0);
    if (count > 0) {
      painter.setPen(Qt::NoPen);
      painter.setBrush(theme::c("#ECFEFF"));
      if (count <= 6) {
        for (int i = 0; i < count; ++i) {
          const double angle = -kPi / 2.0 + i * (2.0 * kPi / std::max(1, count));
          const double radius = count == 1 ? 0.0 : 9.5;
          const QPointF tokenPos(std::cos(angle) * radius, std::sin(angle) * radius);
          QRadialGradient tokenGlow(tokenPos, 6.0);
          tokenGlow.setColorAt(0.0, theme::alpha(theme::c("#F0FDFF"), 230));
          tokenGlow.setColorAt(0.5, theme::alpha(theme::c("#22D3EE"), 120));
          tokenGlow.setColorAt(1.0, Qt::transparent);
          painter.setBrush(tokenGlow);
          painter.drawEllipse(tokenPos, 6.2, 6.2);
          painter.setBrush(theme::c("#F0FDFF"));
          painter.drawEllipse(tokenPos, 3.05, 3.05);
        }
      } else {
        painter.setPen(theme::c("#ECFEFF"));
        painter.setFont(theme::display(12, QFont::Bold));
        painter.drawText(QRectF(-20, -15, 40, 26), Qt::AlignCenter, QString::number(count));
      }
    }

    painter.setFont(theme::ui(10, QFont::DemiBold));
    painter.setPen(selected ? theme::c("#E0F2FE") : theme::c("#A8C5D8"));
    const QString name = painter.fontMetrics().elidedText(place.id, Qt::ElideRight, 138);
    painter.drawText(QRectF(-69, kPlaceRadius + 6, 138, 15), Qt::AlignHCenter, name);
    painter.setFont(theme::mono(7.5, QFont::Medium));
    painter.setPen(theme::c("#5E7A90"));
    painter.drawText(QRectF(-69, kPlaceRadius + 21, 138, 14), Qt::AlignHCenter,
                     place.capacity > 0 ? QStringLiteral("%1 / %2").arg(count).arg(place.capacity)
                                        : QStringLiteral("tokens %1").arg(count));
  } else {
    const Transition& transition = net_.transitions.at(node.index);
    const bool enabled = enabled_.contains(node.index);
    const bool firing = hasActiveFire_ && activeFire_.transition == node.index;
    const double pulse = 0.5 + 0.5 * std::sin(clock_.elapsed() / 260.0);

    if (enabled || firing) {
      const QColor haloColor = firing ? theme::c("#F472B6") : theme::c("#F59E0B");
      QRadialGradient halo(QPointF(0, 0), 104.0);
      halo.setColorAt(0.0, theme::alpha(haloColor, static_cast<int>((firing ? 70 : 45) + 24 * pulse)));
      halo.setColorAt(0.6, theme::alpha(haloColor, 12));
      halo.setColorAt(1.0, Qt::transparent);
      painter.setPen(Qt::NoPen);
      painter.setBrush(halo);
      painter.drawEllipse(QPointF(0, 0), 104.0, 104.0);
    }

    const QRectF body(-kTransitionWidth / 2.0, -kTransitionHeight / 2.0, kTransitionWidth,
                      kTransitionHeight);
    QLinearGradient fill(body.topLeft(), body.bottomRight());
    if (firing) {
      fill.setColorAt(0.0, theme::c("#3B1735"));
      fill.setColorAt(0.55, theme::c("#23152D"));
      fill.setColorAt(1.0, theme::c("#150F22"));
    } else if (enabled) {
      fill.setColorAt(0.0, theme::c("#2A2113"));
      fill.setColorAt(0.6, theme::c("#1A1A22"));
      fill.setColorAt(1.0, theme::c("#111624"));
    } else {
      fill.setColorAt(0.0, theme::c("#101B2A"));
      fill.setColorAt(0.6, theme::c("#0C1420"));
      fill.setColorAt(1.0, theme::c("#0A111C"));
    }
    painter.setBrush(fill);
    const QColor border = firing ? theme::c("#F472B6")
                                 : (enabled ? theme::c("#FBBF24") : theme::c("#263A52"));
    painter.setPen(QPen(selected ? theme::c("#A5F3FC") : border, selected ? 2.6 : 1.4));
    painter.drawRoundedRect(body, 12, 12);

    painter.setFont(theme::ui(10, QFont::DemiBold));
    painter.setPen(enabled || firing ? theme::c("#FFF7ED") : theme::c("#AABBCF"));
    const QString name =
        painter.fontMetrics().elidedText(transition.id, Qt::ElideRight, static_cast<int>(kTransitionWidth - 20));
    painter.drawText(QRectF(-kTransitionWidth / 2.0 + 10, -22, kTransitionWidth - 20, 17),
                     Qt::AlignCenter, name);

    painter.setFont(theme::mono(7.5, QFont::Medium));
    painter.setPen(theme::c("#7C8CA8"));
    painter.drawText(QRectF(-kTransitionWidth / 2.0 + 8, -3, kTransitionWidth - 16, 13),
                     Qt::AlignCenter,
                     QStringLiteral("P%1  C%2  %3")
                         .arg(transition.priority)
                         .arg(transition.core)
                         .arg(compactInterval(transition)));

    if (enabled) {
      painter.setPen(Qt::NoPen);
      painter.setBrush(theme::c("#FBBF24"));
      painter.drawEllipse(QPointF(kTransitionWidth / 2.0 - 10, -kTransitionHeight / 2.0 + 10), 3.2,
                          3.2);
    }
    if (firing) {
      const double progress = qBound(0.0, activeFire_.progress, 1.0);
      painter.setPen(Qt::NoPen);
      painter.setBrush(theme::c("#F472B6"));
      painter.drawRoundedRect(
          QRectF(-kTransitionWidth / 2.0 + 8, kTransitionHeight / 2.0 - 7,
                 (kTransitionWidth - 16) * progress, 3.0),
          1.5, 1.5);
    }
  }

  painter.restore();
}

void PetriView::paintEmpty(QPainter& painter) {
  const QPointF center(width() / 2.0, height() / 2.0 - 22.0);
  painter.save();
  painter.translate(center);
  painter.setPen(QPen(theme::alpha(theme::c("#22D3EE"), 50), 1.2));
  painter.setBrush(theme::alpha(theme::c("#0B1B2B"), 120));
  for (int i = 0; i < 3; ++i) {
    const double x = (i - 1) * 110.0;
    painter.drawEllipse(QPointF(x, 0), 17, 17);
    painter.drawRoundedRect(QRectF(x - 10, -58, 20, 9), 3, 3);
  }
  painter.drawLine(QPointF(-93, 0), QPointF(-78, 0));
  painter.drawLine(QPointF(-55, 0), QPointF(-40, 0));
  painter.drawLine(QPointF(-18, 0), QPointF(-3, 0));
  painter.drawLine(QPointF(18, 0), QPointF(33, 0));
  painter.drawLine(QPointF(55, 0), QPointF(70, 0));
  painter.restore();

  painter.setFont(theme::display(18, QFont::Bold));
  painter.setPen(theme::c("#D9F5FF"));
  painter.drawText(QRectF(0, center.y() + 46, width(), 30), Qt::AlignHCenter,
                   QStringLiteral("DROP A MODEL"));
  painter.setFont(theme::ui(9));
  painter.setPen(theme::c("#5F7790"));
  painter.drawText(QRectF(0, center.y() + 78, width(), 20), Qt::AlignHCenter,
                   QStringLiteral("RTSL  ·  TDG JSON  ·  PTPN"));
}

void PetriView::mousePressEvent(QMouseEvent* event) {
  if (event->button() == Qt::LeftButton) {
    const int node = nodeAt(screenToWorld(event->position()));
    if (node >= 0) {
      selectedNode_ = node;
      if (nodes_.at(node).place) {
        emit nodeSelected(QStringLiteral("place"), nodes_.at(node).index);
        emit statusMessage(tooltipFor(node));
      } else {
        emit nodeSelected(QStringLiteral("transition"), nodes_.at(node).index);
        if (enabled_.contains(nodes_.at(node).index)) {
          startFiring(nodes_.at(node).index, true);
        } else {
          emit statusMessage(QStringLiteral("%1 is not enabled in the current marking")
                                 .arg(net_.transitions.at(nodes_.at(node).index).id));
        }
      }
      update();
      return;
    }
    dragging_ = true;
    lastMouse_ = event->position();
    setCursor(Qt::ClosedHandCursor);
  }
}

void PetriView::mouseMoveEvent(QMouseEvent* event) {
  if (dragging_) {
    const QPointF delta = event->position() - lastMouse_;
    pan_ += delta;
    lastMouse_ = event->position();
    update();
    return;
  }
  const int node = nodeAt(screenToWorld(event->position()));
  if (node != hoverNode_) {
    hoverNode_ = node;
    if (node >= 0) {
      QToolTip::showText(event->globalPosition().toPoint(), tooltipFor(node), this);
    } else {
      QToolTip::hideText();
    }
    setCursor(node >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
    update();
  }
}

void PetriView::mouseReleaseEvent(QMouseEvent* event) {
  if (event->button() == Qt::LeftButton) {
    dragging_ = false;
    setCursor(Qt::ArrowCursor);
  }
}

void PetriView::mouseDoubleClickEvent(QMouseEvent* event) {
  if (event->button() == Qt::LeftButton && nodeAt(screenToWorld(event->position())) < 0) {
    fitView();
  }
}

void PetriView::wheelEvent(QWheelEvent* event) {
  const double factor = std::pow(1.0015, event->angleDelta().y());
  zoomAt(event->position(), factor);
}

void PetriView::resizeEvent(QResizeEvent* event) {
  QWidget::resizeEvent(event);
  if (needsFit_) {
    fitView();
  }
}

void PetriView::enterEvent(QEnterEvent* event) {
  QWidget::enterEvent(event);
  setFocus(Qt::MouseFocusReason);
}

void PetriView::leaveEvent(QEvent* event) {
  hoverNode_ = -1;
  QToolTip::hideText();
  update();
  QWidget::leaveEvent(event);
}
