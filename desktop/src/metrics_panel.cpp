#include "metrics_panel.h"

#include <QFontMetrics>
#include <QConicalGradient>
#include <QJsonArray>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>

#include "theme.h"

namespace {
QString number(double value, int decimals = 0) {
  if (!std::isfinite(value)) {
    return QStringLiteral("—");
  }
  return QString::number(value, 'f', decimals);
}

QString compact(double value) {
  if (value >= 1000000.0) {
    return QString::number(value / 1000000.0, 'f', 1) + QStringLiteral("M");
  }
  if (value >= 1000.0) {
    return QString::number(value / 1000.0, 'f', 1) + QStringLiteral("k");
  }
  return number(value);
}

QColor taskColor(const MetricsPanel::TaskRow& task) {
  if (task.missed) {
    return theme::c("#FB7185");
  }
  if (task.deadline > 0 && task.wcrt > task.deadline * 0.8) {
    return theme::c("#FBBF24");
  }
  return theme::c("#22D3EE");
}
}  // namespace

MetricsPanel::MetricsPanel(QWidget* parent) : QWidget(parent) {
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  setMinimumWidth(300);
}

void MetricsPanel::setResult(const AnalysisResult& result) {
  metrics_ = result.metrics;
  net_ = result.net;
  tasks_ = parseTasks(metrics_);
  cores_ = parseCores(metrics_);
  hasResult_ = !metrics_.isEmpty();
  updateGeometry();
  update();
}

QSize MetricsPanel::sizeHint() const {
  const int taskRows = std::max(1, static_cast<int>(tasks_.size()));
  return QSize(980, 500 + taskRows * 46);
}

QVector<MetricsPanel::TaskRow> MetricsPanel::parseTasks(const QJsonObject& metrics) {
  QVector<TaskRow> tasks;
  const QJsonArray array = metrics.value(QStringLiteral("tasks")).toArray();
  for (const QJsonValue& value : array) {
    if (!value.isObject()) {
      continue;
    }
    const QJsonObject object = value.toObject();
    TaskRow row;
    row.name = object.value(QStringLiteral("name")).toString();
    row.core = object.value(QStringLiteral("core")).toInt(-1);
    row.priority = object.value(QStringLiteral("priority")).toInt();
    row.wcet = object.value(QStringLiteral("wcet")).toDouble();
    row.wcrt = object.value(QStringLiteral("wcrt")).toDouble();
    row.deadline = object.value(QStringLiteral("deadline")).toDouble();
    row.capped = object.value(QStringLiteral("wcrt_capped")).toBool();
    row.missed = object.value(QStringLiteral("deadline_missed")).toBool();
    row.observed = object.value(QStringLiteral("observed")).toBool();
    row.verdict = object.value(QStringLiteral("verdict")).toString();
    const QJsonValue slack = object.value(QStringLiteral("slack"));
    if (slack.isDouble()) {
      row.slack = slack.toDouble();
      row.hasSlack = true;
    }
    tasks.push_back(row);
  }
  std::sort(tasks.begin(), tasks.end(), [](const TaskRow& lhs, const TaskRow& rhs) {
    if (lhs.core != rhs.core) {
      return lhs.core < rhs.core;
    }
    if (lhs.priority != rhs.priority) {
      return lhs.priority > rhs.priority;
    }
    return lhs.name < rhs.name;
  });
  return tasks;
}

QVector<MetricsPanel::CoreRow> MetricsPanel::parseCores(const QJsonObject& metrics) {
  QVector<CoreRow> cores;
  const QJsonArray array = metrics.value(QStringLiteral("cores")).toArray();
  for (const QJsonValue& value : array) {
    if (!value.isObject()) {
      continue;
    }
    const QJsonObject object = value.toObject();
    CoreRow row;
    row.core = object.value(QStringLiteral("core")).toInt();
    row.utilMin = object.value(QStringLiteral("util_min")).toDouble();
    row.utilMax = object.value(QStringLiteral("util_max")).toDouble();
    row.busy = object.value(QStringLiteral("graph_busy_fraction")).toDouble();
    cores.push_back(row);
  }
  return cores;
}

void MetricsPanel::drawSectionTitle(QPainter& painter, int x, int y, const QString& title,
                                    const QString& subtitle) const {
  painter.setFont(theme::ui(8, QFont::DemiBold));
  painter.setPen(theme::c("#7DD3FC"));
  const QString upper = title.toUpper();
  painter.drawText(QRect(x, y, 620, 18), Qt::AlignLeft | Qt::AlignVCenter, upper);
  const int titleWidth = painter.fontMetrics().horizontalAdvance(upper);
  painter.setFont(theme::mono(7));
  painter.setPen(theme::c("#52627A"));
  painter.drawText(QRect(x + titleWidth + 10, y, 700, 18), Qt::AlignLeft | Qt::AlignVCenter,
                   QStringLiteral("/  ") + subtitle);
}

void MetricsPanel::drawStatCard(QPainter& painter, const QRectF& rect, const QString& label,
                                const QString& value, const QString& detail,
                                const QColor& accent) const {
  QLinearGradient fill(rect.topLeft(), rect.bottomRight());
  fill.setColorAt(0.0, theme::alpha(accent, 22));
  fill.setColorAt(1.0, theme::alpha(theme::c("#0B1220"), 235));
  painter.setPen(QPen(theme::alpha(accent, 70), 1.0));
  painter.setBrush(fill);
  painter.drawRoundedRect(rect, 12, 12);

  painter.setPen(Qt::NoPen);
  painter.setBrush(accent);
  painter.drawRoundedRect(QRectF(rect.left() + 14, rect.top() + 16, 3, 15), 1.5, 1.5);

  painter.setFont(theme::mono(7));
  painter.setPen(theme::c("#7C8CA8"));
  painter.drawText(QRectF(rect.left() + 26, rect.top() + 13, rect.width() - 34, 18),
                   Qt::AlignLeft | Qt::AlignVCenter, label.toUpper());
  painter.setFont(theme::display(19, QFont::Bold));
  painter.setPen(theme::c("#F1F5F9"));
  painter.drawText(QRectF(rect.left() + 14, rect.top() + 32, rect.width() - 28, 32),
                   Qt::AlignLeft | Qt::AlignVCenter, value);
  painter.setFont(theme::ui(7.5));
  painter.setPen(theme::c("#7C8CA8"));
  painter.drawText(QRectF(rect.left() + 14, rect.bottom() - 22, rect.width() - 28, 16),
                   Qt::AlignLeft | Qt::AlignVCenter, detail);
}

void MetricsPanel::drawTaskTable(QPainter& painter, int x, int y, int width,
                                 const QVector<TaskRow>& tasks) const {
  const int headerY = y + 4;
  painter.setFont(theme::mono(7));
  painter.setPen(theme::c("#52627A"));
  painter.drawText(QRect(x + 8, headerY, 150, 16), Qt::AlignLeft | Qt::AlignVCenter,
                   QStringLiteral("TASK"));
  painter.drawText(QRect(x + 170, headerY, 60, 16), Qt::AlignLeft | Qt::AlignVCenter,
                   QStringLiteral("CORE"));
  painter.drawText(QRect(x + 232, headerY, 70, 16), Qt::AlignLeft | Qt::AlignVCenter,
                   QStringLiteral("PRIO"));
  painter.drawText(QRect(x + 306, headerY, width - 530, 16), Qt::AlignLeft | Qt::AlignVCenter,
                   QStringLiteral("RESPONSE ENVELOPE  ·  WCET → WCRT / DEADLINE"));
  painter.drawText(QRect(x + width - 206, headerY, 110, 16), Qt::AlignRight | Qt::AlignVCenter,
                   QStringLiteral("WCRT"));
  painter.drawText(QRect(x + width - 84, headerY, 74, 16), Qt::AlignRight | Qt::AlignVCenter,
                   QStringLiteral("SLACK"));

  double globalDomain = 1.0;
  for (const TaskRow& task : tasks) {
    globalDomain = std::max({globalDomain, task.deadline, task.wcrt, task.wcet});
  }
  globalDomain *= 1.08;

  int rowY = y + 24;
  for (int i = 0; i < tasks.size(); ++i) {
    const TaskRow& task = tasks.at(i);
    const QRectF rowRect(x, rowY, width, 42);
    if (i % 2 == 0) {
      painter.setPen(Qt::NoPen);
      painter.setBrush(theme::alpha(theme::c("#0D1828"), 130));
      painter.drawRoundedRect(rowRect, 8, 8);
    }

    painter.setFont(theme::ui(9, QFont::DemiBold));
    painter.setPen(theme::c("#DCE9F8"));
    painter.drawText(QRectF(x + 8, rowY + 7, 150, 18), Qt::AlignLeft | Qt::AlignVCenter,
                     painter.fontMetrics().elidedText(task.name, Qt::ElideRight, 145));

    painter.setFont(theme::mono(6.5));
    painter.setPen(theme::c("#8FA6C4"));
    painter.drawText(QRectF(x + 12, rowY + 23, 150, 13), Qt::AlignLeft | Qt::AlignVCenter,
                     task.observed ? QStringLiteral("observed") : QStringLiteral("model"));

    painter.setFont(theme::mono(8));
    painter.setPen(task.core >= 0 ? theme::c("#A5F3FC") : theme::c("#7C8CA8"));
    painter.drawText(QRectF(x + 170, rowY + 7, 54, 18), Qt::AlignLeft | Qt::AlignVCenter,
                     task.core >= 0 ? QStringLiteral("C%1").arg(task.core) : QStringLiteral("—"));

    painter.setPen(theme::c("#A8C5D8"));
    painter.drawText(QRectF(x + 232, rowY + 7, 60, 18), Qt::AlignLeft | Qt::AlignVCenter,
                     QString::number(task.priority));

    const int barX = x + 306;
    const int barWidth = std::max(80, width - 530);
    const QRectF track(barX, rowY + 14, barWidth, 12);
    painter.setPen(Qt::NoPen);
    painter.setBrush(theme::c("#081321"));
    painter.drawRoundedRect(track, 5, 5);

    const double wcetWidth = std::max(2.0, track.width() * task.wcet / globalDomain);
    const double wcrtWidth = std::max(2.0, track.width() * task.wcrt / globalDomain);
    const QColor barColor = taskColor(task);

    QLinearGradient wcrtFill(track.topLeft(), track.topRight());
    wcrtFill.setColorAt(0.0, theme::alpha(barColor, 70));
    wcrtFill.setColorAt(1.0, theme::alpha(barColor, 220));
    painter.setBrush(wcrtFill);
    painter.drawRoundedRect(QRectF(track.left(), track.top(), wcrtWidth, track.height()), 5, 5);

    painter.setBrush(theme::alpha(theme::c("#E0F2FE"), 210));
    painter.drawRoundedRect(QRectF(track.left(), track.top() + 3.0, wcetWidth, 6.0), 3, 3);

    if (task.deadline > 0) {
      const double deadlineX = track.left() + track.width() * task.deadline / globalDomain;
      painter.setPen(QPen(theme::c("#F59E0B"), 1.2));
      painter.drawLine(QPointF(deadlineX, track.top() - 4), QPointF(deadlineX, track.bottom() + 4));
    }

    painter.setFont(theme::mono(7.5));
    painter.setPen(barColor);
    painter.drawText(QRectF(x + width - 206, rowY + 7, 108, 18), Qt::AlignRight | Qt::AlignVCenter,
                     (task.capped ? QStringLiteral("≥") : QString()) + number(task.wcrt));

    painter.setFont(theme::mono(7));
    painter.setPen(task.hasSlack ? (task.slack < 0 ? theme::c("#FB7185") : theme::c("#86EFAC"))
                                 : theme::c("#52627A"));
    painter.drawText(QRectF(x + width - 84, rowY + 7, 72, 18), Qt::AlignRight | Qt::AlignVCenter,
                     task.hasSlack ? number(task.slack) : QStringLiteral("—"));

    rowY += 44;
  }
}

void MetricsPanel::drawCores(QPainter& painter, int x, int y, int width,
                             const QVector<CoreRow>& cores) const {
  if (cores.isEmpty()) {
    painter.setFont(theme::ui(8));
    painter.setPen(theme::c("#52627A"));
    painter.drawText(QRect(x + 8, y, width, 20), Qt::AlignLeft | Qt::AlignVCenter,
                     QStringLiteral("No per-core utilization reported."));
    return;
  }
  const int cardWidth = 176;
  int currentX = x + 4;
  for (const CoreRow& core : cores) {
    const QRectF card(currentX, y, cardWidth, 150);
    painter.setPen(QPen(theme::c("#16243A"), 1.0));
    painter.setBrush(theme::c("#0A1321"));
    painter.drawRoundedRect(card, 12, 12);

    const QPointF center(card.center().x(), card.top() + 55);
    const double radius = 32.0;
    const QRectF arcRect(center.x() - radius, center.y() - radius, radius * 2, radius * 2);
    painter.setPen(QPen(theme::c("#14243A"), 8.0, Qt::SolidLine, Qt::RoundCap));
    painter.drawArc(arcRect, 0, 360 * 16);
    const double fraction = qBound(0.0, std::max(core.utilMax, core.busy), 1.0);
    QConicalGradient utilization(center, 90);
    utilization.setColorAt(0.0, theme::c("#22D3EE"));
    utilization.setColorAt(0.7, theme::c("#8B5CF6"));
    utilization.setColorAt(1.0, theme::c("#22D3EE"));
    QPen arcPen(QBrush(utilization), 8.0, Qt::SolidLine, Qt::RoundCap);
    painter.setPen(arcPen);
    painter.drawArc(arcRect, 90 * 16, -static_cast<int>(fraction * 360 * 16));

    painter.setFont(theme::display(14, QFont::Bold));
    painter.setPen(theme::c("#E0F2FE"));
    painter.drawText(QRectF(center.x() - 38, center.y() - 12, 76, 24), Qt::AlignCenter,
                     QStringLiteral("%1%").arg(qRound(fraction * 100.0)));

    painter.setFont(theme::mono(7.5));
    painter.setPen(theme::c("#A5F3FC"));
    painter.drawText(QRectF(card.left(), card.top() + 102, card.width(), 18), Qt::AlignCenter,
                     QStringLiteral("CORE %1").arg(core.core));
    painter.setFont(theme::mono(6.5));
    painter.setPen(theme::c("#64748B"));
    painter.drawText(QRectF(card.left(), card.top() + 120, card.width(), 16), Qt::AlignCenter,
                     QStringLiteral("util %1–%2  ·  busy %3")
                         .arg(number(core.utilMin, 2), number(core.utilMax, 2),
                              number(core.busy, 2)));
    currentX += cardWidth + 12;
  }
}

void MetricsPanel::drawPills(QPainter& painter, int x, int y, const QJsonObject& metrics) const {
  struct Pill {
    QString text;
    QColor color;
  };
  QVector<Pill> pills;
  pills.push_back({jsonBool(metrics, "exact", true) ? QStringLiteral("EXACT")
                                                    : QStringLiteral("APPROXIMATE"),
                   jsonBool(metrics, "exact", true) ? theme::c("#34D399") : theme::c("#F59E0B")});
  pills.push_back({jsonBool(metrics, "wcrt_lb_valid", true) ? QStringLiteral("WCRT LOWER BOUND")
                                                            : QStringLiteral("NO LOWER BOUND"),
                   jsonBool(metrics, "wcrt_lb_valid", true) ? theme::c("#22D3EE")
                                                            : theme::c("#7C8CA8")});
  if (jsonBool(metrics, "truncated", false)) {
    pills.push_back({QStringLiteral("TRUNCATED"), theme::c("#FB7185")});
  }
  if (jsonBool(metrics, "has_steady_cycle", false)) {
    pills.push_back({QStringLiteral("STEADY CYCLE"), theme::c("#8B5CF6")});
  }
  int currentX = x;
  for (const Pill& pill : pills) {
    painter.setFont(theme::mono(6.5, QFont::DemiBold));
    const int textWidth = painter.fontMetrics().horizontalAdvance(pill.text);
    const QRectF rect(currentX, y, textWidth + 20, 22);
    painter.setPen(QPen(theme::alpha(pill.color, 120), 1.0));
    painter.setBrush(theme::alpha(pill.color, 18));
    painter.drawRoundedRect(rect, 11, 11);
    painter.setPen(pill.color);
    painter.drawText(rect, Qt::AlignCenter, pill.text);
    currentX += rect.width() + 8;
  }
}

void MetricsPanel::paintEvent(QPaintEvent*) {
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing, true);

  QLinearGradient background(0, 0, width(), height());
  background.setColorAt(0.0, theme::c("#070C16"));
  background.setColorAt(1.0, theme::c("#060A12"));
  painter.fillRect(rect(), background);

  if (!hasResult_) {
    painter.setFont(theme::display(16, QFont::Bold));
    painter.setPen(theme::c("#D9F5FF"));
    painter.drawText(rect().adjusted(0, -20, 0, -20), Qt::AlignCenter,
                     QStringLiteral("TELEMETRY"));
    painter.setFont(theme::ui(9));
    painter.setPen(theme::c("#5F7790"));
    painter.drawText(rect().adjusted(0, 22, 0, 22), Qt::AlignCenter,
                     QStringLiteral("No metrics available for this run"));
    return;
  }

  const int margin = 18;
  const int contentWidth = width() - margin * 2;
  int y = 16;

  drawSectionTitle(painter, margin, y, QStringLiteral("Analysis telemetry"),
                   QStringLiteral("state-class reachability · DBM zones"));
  y += 30;

  const bool schedulable = jsonBool(metrics_, "schedulable", false);
  const QString verdict = jsonString(metrics_, "schedulability",
                                     schedulable ? QStringLiteral("schedulable")
                                                 : QStringLiteral("not schedulable"));
  const double hyperperiod = jsonNumber(metrics_, "hyperperiod");
  const int deadlocks = metrics_.value(QStringLiteral("deadlock_states")).toArray().size();
  const int states = static_cast<int>(jsonNumber(metrics_, "states"));
  const int transitions = static_cast<int>(jsonNumber(metrics_, "transitions"));
  const double cardGap = 12.0;
  const double cardWidth = (contentWidth - cardGap * 3.0) / 4.0;
  drawStatCard(painter, QRectF(margin, y, cardWidth, 94),
               QStringLiteral("Schedulability"), schedulable ? QStringLiteral("PASS") : QStringLiteral("FAIL"),
               verdict, schedulable ? theme::c("#34D399") : theme::c("#FB7185"));
  drawStatCard(painter, QRectF(margin + cardWidth + cardGap, y, cardWidth, 94),
               QStringLiteral("State classes"), compact(states),
               QStringLiteral("%1 transitions").arg(transitions), theme::c("#22D3EE"));
  drawStatCard(painter, QRectF(margin + (cardWidth + cardGap) * 2.0, y, cardWidth, 94),
               QStringLiteral("Hyperperiod"), hyperperiod > 0 ? compact(hyperperiod) : QStringLiteral("—"),
               hyperperiod > 0 ? QStringLiteral("engine ticks") : QStringLiteral("aperiodic model"),
               theme::c("#8B5CF6"));
  drawStatCard(painter, QRectF(margin + (cardWidth + cardGap) * 3.0, y, cardWidth, 94),
               QStringLiteral("Deadlocks"), QString::number(deadlocks),
               deadlocks == 0 ? QStringLiteral("no deadlock state") : QStringLiteral("inspect witness"),
               deadlocks == 0 ? theme::c("#FBBF24") : theme::c("#FB7185"));
  y += 112;

  drawSectionTitle(painter, margin, y, QStringLiteral("Task response envelope"),
                   QStringLiteral("WCET / WCRT / deadline · ms ticks"));
  y += 28;
  drawTaskTable(painter, margin, y, contentWidth, tasks_);
  y += 30 + std::max(1, static_cast<int>(tasks_.size())) * 44 + 26;

  drawSectionTitle(painter, margin, y, QStringLiteral("Core utilization"),
                   QStringLiteral("per-core demand and busy fraction"));
  y += 28;
  drawCores(painter, margin, y, contentWidth, cores_);
  y += 166;

  drawSectionTitle(painter, margin, y, QStringLiteral("Certification"),
                   QStringLiteral("exactness flags carried by metrics JSON"));
  y += 28;
  drawPills(painter, margin, y, metrics_);
}
