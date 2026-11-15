#include "workbench_text.h"

#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QMap>
#include <QPair>
#include <QSet>
#include <QStringList>
#include <QVector>

#include <algorithm>
#include <cmath>

namespace {

constexpr int kRuleWidth = 96;

struct TaskInfo {
  QString id;
  QString type = QStringLiteral("task");
  QString wcetKind = QStringLiteral("model");
  QString wcetMethod;
  int core = -1;
  bool hasCore = false;
  int priority = 0;
  bool hasPriority = false;
  QVector<int> allowedCores;
  QVector<QPair<double, double>> times;
  double bcet = 0.0;
  double wcet = 0.0;
  double wcrt = 0.0;
  double deadline = 0.0;
  double period = 0.0;
  double slack = 0.0;
  bool hasSlack = false;
  int preemptions = 0;
  double interference = 0.0;
  double blocking = 0.0;
  bool observed = false;
  bool missed = false;
  bool capped = false;
  bool wcrtExact = false;
};

struct TdgConfig {
  QString name;
  QString policy = QStringLiteral("fixed");
  int numCpus = 1;
  int coresPerCpu = 1;
  int capacity = 1;
  QStringList starts;
  QStringList ends;
  QVector<QPair<QString, double>> periodic;
};

struct TdgEdgeInfo {
  QString source;
  QString target;
  QString label;
};

struct ParsedModel {
  TdgConfig config;
  QVector<TaskInfo> tasks;
  QVector<TdgEdgeInfo> edges;
  bool hasTdg = false;
};

QString rule() { return QString(kRuleWidth, QChar(0x2500)); }

QString fit(QString value, int width) {
  value.replace(QLatin1Char('\n'), QLatin1Char(' '));
  if (value.size() <= width) {
    return value.leftJustified(width);
  }
  if (width <= 1) {
    return value.left(width);
  }
  return value.left(width - 1) + QChar(0x2026);
}

QString number(double value) {
  if (!std::isfinite(value)) {
    return QStringLiteral("—");
  }
  if (std::abs(value - std::round(value)) < 0.0001) {
    return QString::number(static_cast<qint64>(std::llround(value)));
  }
  return QString::number(value, 'f', 2);
}

QString interval(double low, double high) {
  return QStringLiteral("[%1, %2]").arg(number(low), number(high));
}

QString tag(const QString& value) { return QLatin1Char('[') + value + QLatin1Char(']'); }

QStringList splitTopLevel(const QString& value) {
  QStringList tokens;
  int depth = 0;
  int start = 0;
  for (int index = 0; index < value.size(); ++index) {
    const QChar ch = value.at(index);
    if (ch == QLatin1Char('(')) {
      ++depth;
    } else if (ch == QLatin1Char(')') && depth > 0) {
      --depth;
    } else if (depth == 0 && ch == QLatin1Char(',') &&
               index + 1 < value.size() && value.at(index + 1) == QLatin1Char(' ')) {
      tokens.push_back(value.mid(start, index - start).trimmed());
      start = index + 2;
      ++index;
    }
  }
  tokens.push_back(value.mid(start).trimmed());
  tokens.removeAll(QString());
  return tokens;
}

void appendWrapped(QStringList& lines, const QString& prefix, const QString& value,
                   int width = 92) {
  const QString firstPrefix = QStringLiteral("    ") + prefix;
  const QString nextPrefix(firstPrefix.size(), QLatin1Char(' '));
  if (value.isEmpty()) {
    lines << firstPrefix + QStringLiteral("—");
    return;
  }
  QString current = firstPrefix;
  for (const QString& token : splitTopLevel(value)) {
    const QString piece = current == firstPrefix ? token : QStringLiteral(", ") + token;
    if (current.size() > firstPrefix.size() && current.size() + piece.size() > width) {
      lines << current;
      current = nextPrefix + token;
    } else {
      current += piece;
    }
  }
  lines << current;
}

void appendWrappedWords(QStringList& lines, const QString& value, int width = 94) {
  const QStringList words = value.split(QLatin1Char(' '), Qt::SkipEmptyParts);
  const QString prefix = QStringLiteral("  ");
  const QString continuation = QStringLiteral("    ");
  QString current = prefix;
  for (const QString& word : words) {
    const QString piece = current == prefix ? word : QLatin1Char(' ') + word;
    if (current.size() > prefix.size() && current.size() + piece.size() > width) {
      lines << current;
      current = continuation + word;
    } else {
      current += piece;
    }
  }
  lines << current;
}

QString taskLane(const QString& id) {
  int index = 0;
  while (index < id.size()) {
    const QChar ch = id.at(index);
    if (!ch.isUpper() && !ch.isDigit()) {
      break;
    }
    ++index;
  }
  return index == 0 ? QStringLiteral("ctl") : id.left(index);
}

QString sourceText(const TaskInfo& task) {
  if (task.wcetKind.isEmpty()) {
    return tag(QStringLiteral("model"));
  }
  return tag(task.wcetKind);
}

QString coreText(const TaskInfo& task) {
  if (!task.hasCore || task.core < 0) {
    return tag(QStringLiteral("unknown"));
  }
  return QString::number(task.core);
}

void parseTask(const QJsonObject& node, TaskInfo* task) {
  task->id = node.value(QStringLiteral("id")).toString();
  task->type = node.value(QStringLiteral("type")).toString(QStringLiteral("task"));
  const QJsonValue core = node.value(QStringLiteral("core"));
  if (core.isDouble()) {
    task->core = static_cast<int>(core.toDouble());
    task->hasCore = task->core >= 0;
  }
  const QJsonValue priority = node.value(QStringLiteral("priority"));
  if (priority.isDouble()) {
    task->priority = static_cast<int>(priority.toDouble());
    task->hasPriority = true;
  }
  task->wcetKind = node.value(QStringLiteral("wcet_kind")).toString(QStringLiteral("model"));
  task->wcetMethod = node.value(QStringLiteral("wcet_method")).toString();

  const QJsonArray allowed = node.value(QStringLiteral("allowed_cores")).toArray();
  for (const QJsonValue& value : allowed) {
    if (value.isDouble()) {
      task->allowedCores.push_back(static_cast<int>(value.toDouble()));
    }
  }

  const QJsonArray times = node.value(QStringLiteral("time")).toArray();
  for (const QJsonValue& value : times) {
    const QJsonArray pair = value.toArray();
    if (pair.size() >= 2) {
      task->times.push_back({pair.at(0).toDouble(), pair.at(1).toDouble()});
    }
  }

  const QJsonValue deadline = node.value(QStringLiteral("deadline"));
  if (deadline.isDouble()) {
    task->deadline = deadline.toDouble();
  }
}

void mergeMetrics(TaskInfo* task, const QJsonObject& metrics) {
  if (metrics.isEmpty()) {
    return;
  }
  const auto metric = [&](const char* key) { return metrics.value(QLatin1String(key)); };
  const QJsonValue bcet = metric("bcet");
  const QJsonValue wcet = metric("wcet");
  const QJsonValue wcrt = metric("wcrt");
  const QJsonValue deadline = metric("deadline");
  const QJsonValue period = metric("period");
  if (bcet.isDouble()) {
    task->bcet = bcet.toDouble();
  }
  if (wcet.isDouble()) {
    task->wcet = wcet.toDouble();
  }
  if (wcrt.isDouble()) {
    task->wcrt = wcrt.toDouble();
  }
  if (deadline.isDouble() && deadline.toDouble() > 0.0) {
    task->deadline = deadline.toDouble();
  }
  if (period.isDouble()) {
    task->period = period.toDouble();
  }
  const QJsonValue slack = metric("slack");
  if (slack.isDouble()) {
    task->slack = slack.toDouble();
    task->hasSlack = true;
  }
  task->preemptions = static_cast<int>(metric("max_preemptions").toDouble());
  task->interference = metric("worst_interference").toDouble();
  task->blocking = metric("worst_blocking").toDouble();
  task->observed = metric("observed").toBool();
  task->missed = metric("deadline_missed").toBool();
  task->capped = metric("wcrt_capped").toBool();
  task->wcrtExact = metric("wcrt_exact").toBool();

  if (task->times.isEmpty() && wcet.isDouble()) {
    task->times.push_back({bcet.toDouble(), wcet.toDouble()});
  }
  if (!task->times.isEmpty()) {
    for (const auto& time : task->times) {
      task->bcet = task->bcet <= 0.0 ? time.first : std::min(task->bcet, time.first);
      task->wcet = std::max(task->wcet, time.second);
    }
  }
}

ParsedModel parseModel(const AnalysisResult& result) {
  ParsedModel parsed;
  const QJsonObject tdg = result.tdg;
  const QJsonObject configuration = tdg.value(QStringLiteral("configuration")).toObject();
  parsed.hasTdg = !tdg.isEmpty();

  if (!configuration.isEmpty()) {
    parsed.config.name = tdg.value(QStringLiteral("graph"))
                             .toObject()
                             .value(QStringLiteral("name"))
                             .toString();
    parsed.config.policy =
        configuration.value(QStringLiteral("policy")).toString(QStringLiteral("fixed"));
    parsed.config.numCpus =
        std::max(1, configuration.value(QStringLiteral("num_cpus")).toInt(1));
    parsed.config.coresPerCpu =
        std::max(1, configuration.value(QStringLiteral("cores_per_cpu")).toInt(1));
    parsed.config.capacity =
        std::max(1, configuration.value(QStringLiteral("task_place_capacity")).toInt(1));

    const auto appendNames = [](const QJsonArray& array, QStringList* destination) {
      for (const QJsonValue& value : array) {
        if (value.isString()) {
          destination->push_back(value.toString());
        } else if (value.isObject()) {
          const QJsonObject object = value.toObject();
          const QString task = object.value(QStringLiteral("task")).toString();
          if (!task.isEmpty()) {
            destination->push_back(task);
          }
        }
      }
    };
    appendNames(configuration.value(QStringLiteral("start")).toArray(), &parsed.config.starts);
    appendNames(configuration.value(QStringLiteral("end")).toArray(), &parsed.config.ends);

    for (const QJsonValue& value : configuration.value(QStringLiteral("periodic")).toArray()) {
      const QJsonObject object = value.toObject();
      const QString task = object.value(QStringLiteral("task")).toString();
      if (!task.isEmpty()) {
        parsed.config.periodic.push_back({task, object.value(QStringLiteral("period")).toDouble()});
      }
    }
  }

  QHash<QString, QJsonObject> metricsTasks;
  const QJsonArray taskMetrics = result.metrics.value(QStringLiteral("tasks")).toArray();
  for (const QJsonValue& value : taskMetrics) {
    const QJsonObject object = value.toObject();
    metricsTasks.insert(object.value(QStringLiteral("name")).toString(), object);
  }

  for (const QJsonValue& value : tdg.value(QStringLiteral("nodes")).toArray()) {
    const QJsonObject object = value.toObject();
    const QString id = object.value(QStringLiteral("id")).toString();
    if (id.isEmpty()) {
      continue;
    }
    TaskInfo task;
    parseTask(object, &task);
    mergeMetrics(&task, metricsTasks.value(id));
    parsed.tasks.push_back(task);
  }

  for (auto it = metricsTasks.constBegin(); it != metricsTasks.constEnd(); ++it) {
    bool present = false;
    for (const TaskInfo& task : parsed.tasks) {
      if (task.id == it.key()) {
        present = true;
        break;
      }
    }
    if (present) {
      continue;
    }
    TaskInfo task;
    task.id = it.key();
    const QJsonObject metrics = it.value();
    task.core = static_cast<int>(metrics.value(QStringLiteral("core")).toDouble(-1));
    task.hasCore = task.core >= 0;
    task.priority = static_cast<int>(metrics.value(QStringLiteral("priority")).toDouble());
    task.hasPriority = metrics.contains(QStringLiteral("priority"));
    mergeMetrics(&task, metrics);
    parsed.tasks.push_back(task);
  }

  for (const QJsonValue& value : tdg.value(QStringLiteral("edges")).toArray()) {
    const QJsonObject object = value.toObject();
    TdgEdgeInfo edge;
    edge.source = object.value(QStringLiteral("source")).toString();
    edge.target = object.value(QStringLiteral("target")).toString();
    edge.label = object.value(QStringLiteral("label")).toString();
    if (!edge.source.isEmpty() && !edge.target.isEmpty()) {
      parsed.edges.push_back(edge);
    }
  }

  if (parsed.config.numCpus <= 1) {
    const QJsonArray cores = result.metrics.value(QStringLiteral("cores")).toArray();
    if (!cores.isEmpty()) {
      parsed.config.numCpus = cores.size();
    }
    for (const TaskInfo& task : parsed.tasks) {
      if (task.hasCore) {
        parsed.config.numCpus = std::max(parsed.config.numCpus, task.core + 1);
      }
    }
  }

  for (TaskInfo& task : parsed.tasks) {
    if (task.wcet <= 0.0) {
      for (const auto& time : task.times) {
        task.bcet = std::min(task.bcet, time.first);
        task.wcet = std::max(task.wcet, time.second);
      }
    }
  }

  std::sort(parsed.tasks.begin(), parsed.tasks.end(),
            [](const TaskInfo& lhs, const TaskInfo& rhs) { return lhs.id < rhs.id; });
  std::sort(parsed.edges.begin(), parsed.edges.end(),
            [](const TdgEdgeInfo& lhs, const TdgEdgeInfo& rhs) {
              if (lhs.source != rhs.source) {
                return lhs.source < rhs.source;
              }
              return lhs.target < rhs.target;
            });
  return parsed;
}

QString workflowFor(const AnalysisResult& result) {
  const QString suffix = QFileInfo(result.path).suffix().toLower();
  if (suffix == QStringLiteral("rtsl")) {
    return QStringLiteral("SPEC-FIRST · RTSL");
  }
  if (suffix == QStringLiteral("json")) {
    return QStringLiteral("SPEC-FIRST · TDG");
  }
  if (suffix == QStringLiteral("ptpn")) {
    return QStringLiteral("MANUAL NET · PTPN");
  }
  return QStringLiteral("SPEC-FIRST");
}

QString inputKindFor(const AnalysisResult& result) {
  const QString suffix = QFileInfo(result.path).suffix().toLower();
  if (suffix == QStringLiteral("rtsl")) {
    return QStringLiteral("RTSL");
  }
  if (suffix == QStringLiteral("json")) {
    return QStringLiteral("TDG JSON");
  }
  if (suffix == QStringLiteral("ptpn")) {
    return QStringLiteral("PTPN");
  }
  return QStringLiteral("MODEL");
}

QStringList configurationLines(const ParsedModel& parsed) {
  QStringList lines;
  lines << QStringLiteral("CONFIGURATION");
  lines << QStringLiteral("  policy              ") + parsed.config.policy;
  lines << QStringLiteral("  cores               ") + QString::number(parsed.config.numCpus);
  lines << QStringLiteral("  cores / cpu         ") + QString::number(parsed.config.coresPerCpu);
  lines << QStringLiteral("  task capacity       ") + QString::number(parsed.config.capacity);
  return lines;
}

QStringList taskTable(const ParsedModel& parsed) {
  QStringList lines;
  int idWidth = 10;
  for (const TaskInfo& task : parsed.tasks) {
    idWidth = std::max(idWidth, std::min(22, static_cast<int>(task.id.size()) + 2));
  }
  lines << QStringLiteral("TASKS  ") + QString::number(parsed.tasks.size());
  lines << QStringLiteral("  ") + fit(QStringLiteral("ID"), idWidth) + fit(QStringLiteral("TYPE"), 8) +
               fit(QStringLiteral("CORE"), 10) + fit(QStringLiteral("PRIO"), 7) +
               fit(QStringLiteral("WCET"), 13) + fit(QStringLiteral("DEADLINE"), 10) +
               fit(QStringLiteral("PERIOD"), 9) + QStringLiteral("SOURCE");
  for (const TaskInfo& task : parsed.tasks) {
    const QString wcet = task.times.isEmpty()
                             ? QStringLiteral("—")
                             : interval(task.times.front().first, task.times.front().second);
    const QString deadline = task.deadline > 0.0 ? number(task.deadline) : QStringLiteral("—");
    const QString period = task.period > 0.0 ? number(task.period) : QStringLiteral("—");
    lines << QStringLiteral("  ") + fit(task.id, idWidth) + fit(task.type, 8) + fit(coreText(task), 10) +
                 fit(task.hasPriority ? QString::number(task.priority) : QStringLiteral("—"), 7) +
                 fit(wcet, 13) + fit(deadline, 10) + fit(period, 9) + sourceText(task);
    if (task.allowedCores.size() > 1) {
      QStringList allowed;
      for (const int core : task.allowedCores) {
        allowed.push_back(QString::number(core));
      }
      lines << QStringLiteral("    allowed cores    ") + allowed.join(QStringLiteral(", "));
    }
    if (task.missed) {
      lines << QStringLiteral("    ") + tag(QStringLiteral("deadline miss")) +
                   QStringLiteral("  wcrt ") + number(task.wcrt);
    } else if (task.capped) {
      lines << QStringLiteral("    ") + tag(QStringLiteral("capped")) + QStringLiteral("  wcrt ") +
                   number(task.wcrt);
    }
  }
  return lines;
}

QString buildDag(const AnalysisResult& result, const ParsedModel& parsed) {
  QStringList lines;
  lines << QStringLiteral("TASK GRAPH  ·  ") + workflowFor(result);
  lines << QStringLiteral("INPUT  ") + QFileInfo(result.path).fileName() + QStringLiteral("  ·  ") +
               inputKindFor(result);
  lines << rule();
  lines.append(configurationLines(parsed));
  lines << QString();
  lines.append(taskTable(parsed));

  if (!parsed.config.starts.isEmpty() || !parsed.config.periodic.isEmpty() ||
      !parsed.config.ends.isEmpty()) {
    lines << QString();
    lines << QStringLiteral("RELEASE / COMPLETION");
    for (const QString& task : parsed.config.starts) {
      lines << QStringLiteral("  ") + fit(task, 18) + QStringLiteral("start");
    }
    for (const auto& periodic : parsed.config.periodic) {
      lines << QStringLiteral("  ") + fit(periodic.first, 18) + QStringLiteral("periodic  ") +
                   number(periodic.second);
    }
    for (const QString& task : parsed.config.ends) {
      lines << QStringLiteral("  ") + fit(task, 18) + QStringLiteral("end");
    }
  }

  lines << QString();
  lines << QStringLiteral("EDGES  ") + QString::number(parsed.edges.size());
  if (parsed.edges.isEmpty()) {
    lines << QStringLiteral("  (no TDG edges exported for this input)");
    if (!parsed.hasTdg) {
      lines << QStringLiteral("  NOTE  Full dependency edges need a TDG JSON or RTSL input.");
    }
  } else {
    int edgeWidth = 6;
    for (const TdgEdgeInfo& edge : parsed.edges) {
      edgeWidth =
          std::max(edgeWidth,
                   std::min(22, static_cast<int>(std::max(edge.source.size(), edge.target.size())) + 2));
    }
    for (const TdgEdgeInfo& edge : parsed.edges) {
      QString line = QStringLiteral("  ") + fit(edge.source, edgeWidth) + QStringLiteral("  →  ") +
                     fit(edge.target, edgeWidth);
      if (!edge.label.isEmpty()) {
        line += QStringLiteral("  label ") + edge.label;
      }
      lines << line;
    }
  }

  if (!parsed.hasTdg) {
    lines << QString();
    lines << QStringLiteral("  NOTE  No TDG export was available; task rows are reconstructed from "
                            "metrics and PTPN structure.");
  }
  return lines.join(QLatin1Char('\n'));
}

QString buildPtpn(const AnalysisResult& result, const ParsedModel& parsed) {
  QStringList lines;
  lines << QStringLiteral("PTPN STRUCTURE");
  lines << QStringLiteral("%1 places  ·  %2 transitions  ·  %3 arcs  ·  %4")
               .arg(result.net.places.size())
               .arg(result.net.transitions.size())
               .arg(result.net.arcs.size())
               .arg(parsed.config.policy);
  lines << rule();
  if (!result.net.valid) {
    lines << QStringLiteral("  No PTPN structure was exported.");
    return lines.join(QLatin1Char('\n'));
  }

  int idWidth = 12;
  for (const Place& place : result.net.places) {
    idWidth = std::max(idWidth, std::min(26, static_cast<int>(place.id.size()) + 2));
  }
  for (const Transition& transition : result.net.transitions) {
    idWidth = std::max(idWidth, std::min(26, static_cast<int>(transition.id.size()) + 2));
  }
  const int arcWidth = std::min(24, idWidth);

  lines << QStringLiteral("PLACES  ") + QString::number(result.net.places.size());
  lines << QStringLiteral("  ") + fit(QStringLiteral("ID"), idWidth) + fit(QStringLiteral("MARK"), 7) +
               fit(QStringLiteral("CAP"), 7) + QStringLiteral("TASK");
  for (int index = 0; index < result.net.places.size(); ++index) {
    const Place& place = result.net.places.at(index);
    const int marking = result.net.initialMarking.value(index, place.tokens);
    const QString capacity = place.capacity > 0 ? QString::number(place.capacity) : QStringLiteral("∞");
    lines << QStringLiteral("  ") + fit(place.id, idWidth) + fit(QString::number(marking), 7) +
                 fit(capacity, 7) + taskLane(place.id);
  }

  lines << QString();
  lines << QStringLiteral("TRANSITIONS  ") + QString::number(result.net.transitions.size());
  lines << QStringLiteral("  ") + fit(QStringLiteral("ID"), idWidth) + fit(QStringLiteral("WINDOW"), 14) +
               fit(QStringLiteral("PRIO"), 6) + fit(QStringLiteral("CORE"), 6) +
               QStringLiteral("FLAGS");
  for (const Transition& transition : result.net.transitions) {
    const QString window = transition.hasInterval
                               ? interval(transition.intervalMin, transition.intervalMax)
                               : QStringLiteral("—");
    QStringList flags;
    if (transition.suspendable) {
      flags << QStringLiteral("suspend");
    }
    if (transition.hasInterval) {
      flags << QStringLiteral("timed");
    }
    const QString core = transition.core < 0 ? QStringLiteral("ctl") : QString::number(transition.core);
    lines << QStringLiteral("  ") + fit(transition.id, idWidth) + fit(window, 14) +
                 fit(QString::number(transition.priority), 6) + fit(core, 6) +
                 (flags.isEmpty() ? QStringLiteral("—") : flags.join(QStringLiteral(", ")));
  }

  lines << QString();
  lines << QStringLiteral("ARCS  ") + QString::number(result.net.arcs.size());
  for (const PetriArc& arc : result.net.arcs) {
    QString from;
    QString to;
    if (arc.fromPlace) {
      if (arc.from >= 0 && arc.from < result.net.places.size()) {
        from = result.net.places.at(arc.from).id;
      }
      if (arc.to >= 0 && arc.to < result.net.transitions.size()) {
        to = result.net.transitions.at(arc.to).id;
      }
    } else {
      if (arc.from >= 0 && arc.from < result.net.transitions.size()) {
        from = result.net.transitions.at(arc.from).id;
      }
      if (arc.to >= 0 && arc.to < result.net.places.size()) {
        to = result.net.places.at(arc.to).id;
      }
    }
    if (!from.isEmpty() && !to.isEmpty()) {
      lines << QStringLiteral("  ") + fit(from, arcWidth) + QStringLiteral("  →  ") + to;
    }
  }

  QMap<QString, QStringList> lanes;
  for (const Place& place : result.net.places) {
    lanes[taskLane(place.id)].push_back(place.id);
  }
  for (const Transition& transition : result.net.transitions) {
    lanes[taskLane(transition.id)].push_back(transition.id);
  }
  if (!lanes.isEmpty()) {
    lines << QString();
    lines << QStringLiteral("TASK LANES");
    for (auto it = lanes.constBegin(); it != lanes.constEnd(); ++it) {
      lines << QStringLiteral("  ") + fit(it.key(), 8) + it.value().join(QStringLiteral("  "));
    }
  }

  QStringList marking;
  for (int index = 0; index < result.net.places.size(); ++index) {
    const int tokens = result.net.initialMarking.value(index, 0);
    if (tokens > 0) {
      marking << QStringLiteral("%1=%2").arg(result.net.places.at(index).id).arg(tokens);
    }
  }
  if (!marking.isEmpty()) {
    lines << QString();
    lines << QStringLiteral("INITIAL MARKING  ") + marking.join(QStringLiteral("  ·  "));
  }
  return lines.join(QLatin1Char('\n'));
}

QString buildState(const AnalysisResult& result) {
  QStringList lines;
  lines << QStringLiteral("STATE-CLASS SPACE");
  const bool exact = jsonBool(result.metrics, "exact", true);
  lines << QStringLiteral("%1 states  ·  %2 edges  ·  %3")
               .arg(result.scg.nodes.size())
               .arg(result.scg.edges.size())
               .arg(exact ? tag(QStringLiteral("exact")) : tag(QStringLiteral("approximate")));
  lines << rule();
  if (!result.scg.valid) {
    lines << QStringLiteral("  No state-class graph was exported.");
    return lines.join(QLatin1Char('\n'));
  }

  constexpr int kStateLimit = 5000;
  const int stateCount =
      std::min(static_cast<int>(result.scg.nodes.size()), kStateLimit);
  lines << QStringLiteral("STATES  ") + QString::number(result.scg.nodes.size());
  for (int index = 0; index < stateCount; ++index) {
    const StateNode& state = result.scg.nodes.at(index);
    const QString id = state.id.isEmpty() ? QStringLiteral("S%1").arg(state.index) : state.id;
    lines << QStringLiteral("%1  ·  elapsed %2").arg(fit(id, 8), state.elapsed);
    appendWrapped(lines, QStringLiteral("marking    "), state.marking);
    appendWrapped(lines, QStringLiteral("active     "), state.active);
    appendWrapped(lines, QStringLiteral("suspended  "), state.suspended);
  }
  if (result.scg.nodes.size() > stateCount) {
    lines << QStringLiteral("  NOTE  Showing first %1 of %2 states; use PREVIEW for the full graph.")
                 .arg(stateCount)
                 .arg(result.scg.nodes.size());
  }

  lines << QString();
  lines << QStringLiteral("EDGES  ") + QString::number(result.scg.edges.size());
  constexpr int kEdgeLimit = 8000;
  const int edgeCount =
      std::min(static_cast<int>(result.scg.edges.size()), kEdgeLimit);
  for (int index = 0; index < edgeCount; ++index) {
    const StateEdge& edge = result.scg.edges.at(index);
    const QString from = edge.from >= 0 && edge.from < result.scg.nodes.size()
                             ? result.scg.nodes.at(edge.from).id
                             : QStringLiteral("S?");
    const QString to = edge.to >= 0 && edge.to < result.scg.nodes.size()
                           ? result.scg.nodes.at(edge.to).id
                           : QStringLiteral("S?");
    lines << QStringLiteral("  ") + fit(from, 9) + QStringLiteral("  ──") + fit(edge.label, 18) +
                 QStringLiteral("──▶  ") + to;
  }
  if (result.scg.edges.size() > edgeCount) {
    lines << QStringLiteral("  NOTE  Showing first %1 of %2 edges.").arg(edgeCount).arg(result.scg.edges.size());
  }
  return lines.join(QLatin1Char('\n'));
}

QString buildTimeline(const AnalysisResult& result, const ParsedModel& parsed) {
  QStringList lines;
  lines << QStringLiteral("RESPONSE ENVELOPES  ·  TEXT TIMELINE");
  lines << QStringLiteral("derived from WCRT / WCET metrics  ·  not a concrete schedule");
  lines << rule();
  if (parsed.tasks.isEmpty()) {
    lines << QStringLiteral("  No task metrics available.");
    return lines.join(QLatin1Char('\n'));
  }

  double domain = 1.0;
  for (const TaskInfo& task : parsed.tasks) {
    domain = std::max({domain, task.wcrt, task.wcet, task.deadline});
  }
  domain *= 1.04;

  QMap<int, QVector<const TaskInfo*>> cores;
  for (const TaskInfo& task : parsed.tasks) {
    cores[task.hasCore && task.core >= 0 ? task.core : -1].push_back(&task);
  }

  QMap<int, QJsonObject> coreMetrics;
  for (const QJsonValue& value : result.metrics.value(QStringLiteral("cores")).toArray()) {
    const QJsonObject object = value.toObject();
    coreMetrics.insert(object.value(QStringLiteral("core")).toInt(), object);
  }

  const int barWidth = 34;
  int taskWidth = 10;
  for (const TaskInfo& task : parsed.tasks) {
    taskWidth = std::max(taskWidth, std::min(22, static_cast<int>(task.id.size()) + 2));
  }
  for (auto it = cores.constBegin(); it != cores.constEnd(); ++it) {
    const QString coreName = it.key() < 0 ? QStringLiteral("UNASSIGNED") : QStringLiteral("CORE %1").arg(it.key());
    QString util;
    if (coreMetrics.contains(it.key())) {
      const QJsonObject core = coreMetrics.value(it.key());
      util = QStringLiteral("  ·  util %1  ·  busy %2")
                .arg(interval(core.value(QStringLiteral("util_min")).toDouble(),
                              core.value(QStringLiteral("util_max")).toDouble()),
                     number(core.value(QStringLiteral("graph_busy_fraction")).toDouble()));
    }
    lines << coreName + util;
    lines << QStringLiteral("  ") + fit(QStringLiteral("TASK"), taskWidth) + fit(QStringLiteral("WCET"), 12) +
                 fit(QStringLiteral("WCRT"), 8) + fit(QStringLiteral("DEADLINE"), 10) +
                 fit(QStringLiteral("SLACK"), 8) + fit(QStringLiteral("PREEMPT"), 9) +
                 fit(QStringLiteral("INTERF"), 8) + QStringLiteral("BLOCK");
    for (const TaskInfo* task : it.value()) {
      const QString wcet = task->times.isEmpty() ? number(task->wcet)
                                                 : interval(task->times.front().first,
                                                            task->times.front().second);
      const QString slack = task->hasSlack ? number(task->slack) : QStringLiteral("—");
      lines << QStringLiteral("  ") + fit(task->id, taskWidth) + fit(wcet, 12) +
                   fit(number(task->wcrt), 8) +
                   fit(task->deadline > 0.0 ? number(task->deadline) : QStringLiteral("—"), 10) +
                   fit(slack, 8) + fit(QString::number(task->preemptions), 9) +
                   fit(number(task->interference), 8) + number(task->blocking);
      int filled = static_cast<int>(std::round(barWidth * task->wcrt / domain));
      filled = std::clamp(filled, 1, barWidth);
      QString bar(barWidth, QChar(0x00B7));
      for (int index = 0; index < filled; ++index) {
        bar[index] = QChar(0x2588);
      }
      if (task->deadline > 0.0) {
        const int marker = std::clamp(
            static_cast<int>(std::round(barWidth * task->deadline / domain)), 0, barWidth - 1);
        bar[marker] = QChar(0x2502);
      }
      lines << QStringLiteral("      |") + bar + QStringLiteral("|  wcrt ") + number(task->wcrt) +
                   (task->wcrtExact ? QStringLiteral(" ") + tag(QStringLiteral("exact"))
                                    : QStringLiteral(" ") + tag(QStringLiteral("approximate")));
    }
    lines << QString();
  }
  lines << QStringLiteral("  NOTE  The bars are response envelopes, not an execution trace. "
                          "Use PREVIEW / STATE SPACE for witness structure.");
  return lines.join(QLatin1Char('\n'));
}

QString buildAssumptions(const AnalysisResult& result, const ParsedModel& parsed) {
  QStringList lines;
  lines << QStringLiteral("WORKFLOW      ") + workflowFor(result);
  lines << QStringLiteral("INPUT         ") + QFileInfo(result.path).fileName();
  lines << QStringLiteral("POLICY        ") + parsed.config.policy;
  lines << QStringLiteral("CORES         ") + QString::number(parsed.config.numCpus) +
               QStringLiteral(" real  ·  ") + QString::number(parsed.config.coresPerCpu) +
               QStringLiteral(" per cpu");
  lines << QStringLiteral("CAPACITY      ") + QString::number(parsed.config.capacity);

  int knownCore = 0;
  int knownPriority = 0;
  QHash<QString, int> wcetKinds;
  for (const TaskInfo& task : parsed.tasks) {
    if (task.hasCore && task.core >= 0) {
      ++knownCore;
    }
    if (task.hasPriority) {
      ++knownPriority;
    }
    wcetKinds[task.wcetKind.isEmpty() ? QStringLiteral("model") : task.wcetKind] += 1;
  }
  QStringList timing;
  for (auto it = wcetKinds.constBegin(); it != wcetKinds.constEnd(); ++it) {
    timing << QStringLiteral("%1 %2").arg(it.key()).arg(it.value());
  }
  lines << QStringLiteral("TIMING        ") +
               (timing.isEmpty() ? QStringLiteral("—") : timing.join(QStringLiteral("  ·  ")));
  if (parsed.tasks.isEmpty()) {
    lines << QStringLiteral("CORE BINDING  ") + tag(QStringLiteral("unknown"));
    lines << QStringLiteral("PRIORITY      ") + tag(QStringLiteral("unknown"));
  } else {
    lines << QStringLiteral("CORE BINDING  ") + QString::number(knownCore) + QStringLiteral("/") +
                 QString::number(parsed.tasks.size()) +
                 (knownCore == parsed.tasks.size() ? QString() : QStringLiteral(" ") + tag(QStringLiteral("unknown")));
    lines << QStringLiteral("PRIORITY      ") + QString::number(knownPriority) + QStringLiteral("/") +
                 QString::number(parsed.tasks.size()) +
                 (knownPriority == parsed.tasks.size() ? QString() : QStringLiteral(" ") + tag(QStringLiteral("unknown")));
  }

  const bool ok = result.ok;
  lines << QStringLiteral("EXACT         ") +
               (ok ? QString(jsonBool(result.metrics, "exact", true) ? "true" : "false")
                   : tag(QStringLiteral("unknown")));
  lines << QStringLiteral("TRUNCATED     ") +
               (ok ? QString(jsonBool(result.metrics, "truncated", false) ? "true" : "false")
                   : tag(QStringLiteral("unknown")));
  const bool bounded = jsonBool(result.metrics, "bounded", true);
  lines << QStringLiteral("BOUNDED       ") + QString(bounded ? "true" : "false");
  int exactWcrt = 0;
  for (const TaskInfo& task : parsed.tasks) {
    if (task.wcrtExact) {
      ++exactWcrt;
    }
  }
  if (!parsed.tasks.isEmpty()) {
    lines << QStringLiteral("WCRT EXACT    ") + QString::number(exactWcrt) + QStringLiteral("/") +
                 QString::number(parsed.tasks.size());
  }
  lines << QStringLiteral("EVIDENCE      ") + QString::number(result.scg.nodes.size()) +
               QStringLiteral(" states  ·  ") + QString::number(result.scg.edges.size()) +
               QStringLiteral(" edges");
  if (ok) {
    lines << QStringLiteral("VERDICT       ") +
                 (jsonBool(result.metrics, "schedulable", false) ? QStringLiteral("schedulable")
                                                                 : QStringLiteral("unschedulable"));
  } else {
    lines << QStringLiteral("VERDICT       ") + tag(QStringLiteral("unknown"));
  }
  return lines.join(QLatin1Char('\n'));
}

QString buildReport(const AnalysisResult& result, const ParsedModel& parsed,
                    const QString& assumptions) {
  QStringList lines;
  lines << QStringLiteral("PTPN ANALYSIS REPORT");
  lines << QStringLiteral("input      ") + result.path;
  lines << QStringLiteral("workflow   ") + workflowFor(result);
  lines << rule();
  lines << QStringLiteral("VERDICT");
  lines << QStringLiteral("  ") +
               (result.ok ? (jsonBool(result.metrics, "schedulable", false)
                                  ? QStringLiteral("SCHEDULABLE")
                                  : QStringLiteral("UNSCHEDULABLE"))
                          : QStringLiteral("NO RESULT"));
  lines << QStringLiteral("  exact ") + QString(jsonBool(result.metrics, "exact", true) ? "true" : "false") +
               QStringLiteral("  ·  truncated ") +
               QString(jsonBool(result.metrics, "truncated", false) ? "true" : "false");
  lines << QStringLiteral("  states ") + QString::number(result.scg.nodes.size()) +
               QStringLiteral("  ·  hyperperiod ") +
               (jsonNumber(result.metrics, "hyperperiod", 0.0) > 0.0
                    ? number(jsonNumber(result.metrics, "hyperperiod", 0.0))
                    : QStringLiteral("—"));
  lines << QString();
  lines << QStringLiteral("TASK OUTCOMES");
  int taskWidth = 14;
  for (const TaskInfo& task : parsed.tasks) {
    taskWidth = std::max(taskWidth, std::min(22, static_cast<int>(task.id.size()) + 2));
  }
  lines << QStringLiteral("  ") + fit(QStringLiteral("TASK"), taskWidth) + fit(QStringLiteral("WCET"), 13) +
               fit(QStringLiteral("WCRT"), 8) + fit(QStringLiteral("DEADLINE"), 10) +
               fit(QStringLiteral("SLACK"), 8) + QStringLiteral("RESULT");
  for (const TaskInfo& task : parsed.tasks) {
    const QString wcet = task.times.isEmpty() ? number(task.wcet)
                                               : interval(task.times.front().first,
                                                          task.times.front().second);
    lines << QStringLiteral("  ") + fit(task.id, taskWidth) + fit(wcet, 13) + fit(number(task.wcrt), 8) +
                 fit(task.deadline > 0.0 ? number(task.deadline) : QStringLiteral("—"), 10) +
                 fit(task.hasSlack ? number(task.slack) : QStringLiteral("—"), 8) +
                 (task.missed ? tag(QStringLiteral("deadline miss"))
                              : (task.wcrtExact ? tag(QStringLiteral("exact"))
                                                : tag(QStringLiteral("approximate"))));
  }
  lines << QString();
  lines << QStringLiteral("ASSUMPTIONS / EVIDENCE");
  for (const QString& line : assumptions.split(QLatin1Char('\n'))) {
    lines << QStringLiteral("  ") + line;
  }
  lines << QString();
  lines << QStringLiteral("  NOTE  Text is the primary representation; use PREVIEW for the "
                          "PTPN/SCG graphs.");
  return lines.join(QLatin1Char('\n'));
}

QString failureDoc(const AnalysisResult& result) {
  QStringList lines;
  lines << QStringLiteral("ANALYSIS FAILED");
  lines << rule();
  lines << QStringLiteral("input   ") + result.path;
  lines << QStringLiteral("error");
  const QStringList errorLines = result.error.split(QLatin1Char('\n'));
  for (const QString& line : errorLines) {
    if (line.isEmpty()) {
      lines << QString();
    } else {
      appendWrappedWords(lines, line);
    }
  }
  lines << QString();
  lines << QStringLiteral("  NOTE  The run log in the right panel has the full analyzer output.");
  return lines.join(QLatin1Char('\n'));
}

}  // namespace

WorkbenchDocs renderWorkbench(const AnalysisResult& result) {
  WorkbenchDocs docs;
  docs.workflow = workflowFor(result);
  docs.inputKind = inputKindFor(result);
  const ParsedModel parsed = parseModel(result);

  docs.dagSummary = QStringLiteral("%1 TASKS  ·  %2 %3")
                        .arg(parsed.tasks.size())
                        .arg(parsed.edges.size())
                        .arg(parsed.edges.size() == 1 ? QStringLiteral("EDGE") : QStringLiteral("EDGES"));
  docs.ptpnSummary = QStringLiteral("%1 PLACES  ·  %2 TRANSITIONS  ·  %3 ARCS")
                         .arg(result.net.places.size())
                         .arg(result.net.transitions.size())
                         .arg(result.net.arcs.size());
  docs.stateSummary = QStringLiteral("%1 STATES  ·  %2 EDGES  ·  %3")
                          .arg(result.scg.nodes.size())
                          .arg(result.scg.edges.size())
                          .arg(jsonBool(result.metrics, "exact", true) ? tag(QStringLiteral("exact"))
                                                                     : tag(QStringLiteral("approximate")));
  docs.timelineSummary = QStringLiteral("%1 TASKS  ·  %2 %3")
                             .arg(parsed.tasks.size())
                             .arg(parsed.config.numCpus)
                             .arg(parsed.config.numCpus == 1 ? QStringLiteral("CORE")
                                                             : QStringLiteral("CORES"));
  docs.reportSummary = result.ok
                           ? (jsonBool(result.metrics, "schedulable", false) ? QStringLiteral("SCHEDULABLE")
                                                                            : QStringLiteral("UNSCHEDULABLE"))
                           : QStringLiteral("NO RESULT");

  if (!result.ok) {
    const QString failed = failureDoc(result);
    docs.dag = failed;
    docs.ptpn = failed;
    docs.state = failed;
    docs.timeline = failed;
    docs.report = failed;
    docs.assumptions = QStringLiteral("VERDICT       ") + tag(QStringLiteral("unknown")) +
                       QStringLiteral("\nERROR         ") + result.error.left(180);
    return docs;
  }

  docs.assumptions = buildAssumptions(result, parsed);
  docs.dag = buildDag(result, parsed);
  docs.ptpn = buildPtpn(result, parsed);
  docs.state = buildState(result);
  docs.timeline = buildTimeline(result, parsed);
  docs.report = buildReport(result, parsed, docs.assumptions);
  return docs;
}
