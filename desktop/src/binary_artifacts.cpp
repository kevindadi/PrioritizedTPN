#include "binary_artifacts.h"

#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QStringList>

#include <algorithm>
#include <cmath>

namespace {

constexpr int kRuleWidth = 96;

QString rule() { return QString(kRuleWidth, QChar(0x2500)); }

QString fit(QString value, int width) {
  value.replace(QLatin1Char('\n'), QLatin1Char(' '));
  if (value.size() <= width) {
    return value.leftJustified(width);
  }
  return width > 1 ? value.left(width - 1) + QChar(0x2026) : value.left(width);
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

QString scalarText(const QJsonValue& value) {
  if (value.isString()) {
    return value.toString();
  }
  if (value.isBool()) {
    return value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
  }
  if (value.isDouble()) {
    return number(value.toDouble());
  }
  if (value.isArray()) {
    QStringList parts;
    for (const QJsonValue& item : value.toArray()) {
      parts << scalarText(item);
    }
    return parts.join(QStringLiteral(", "));
  }
  if (value.isObject()) {
    return QString::fromUtf8(QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact));
  }
  return QStringLiteral("—");
}

QString field(const QJsonObject& object, const QStringList& keys,
              const QString& fallback = QStringLiteral("—")) {
  for (const QString& key : keys) {
    const QJsonValue value = object.value(key);
    if (!value.isUndefined() && !value.isNull()) {
      return scalarText(value);
    }
  }
  return fallback;
}

QJsonArray firstArray(const QJsonObject& object, const QStringList& keys) {
  for (const QString& key : keys) {
    const QJsonValue value = object.value(key);
    if (value.isArray()) {
      return value.toArray();
    }
  }
  return {};
}

QJsonObject firstObject(const QJsonObject& object, const QStringList& keys) {
  for (const QString& key : keys) {
    const QJsonValue value = object.value(key);
    if (value.isObject()) {
      return value.toObject();
    }
  }
  return {};
}

QString prettyJson(const QJsonObject& object, int maxLines = 160) {
  if (object.isEmpty()) {
    return QStringLiteral("  (no data)");
  }
  const QByteArray bytes = QJsonDocument(object).toJson(QJsonDocument::Indented);
  const QStringList lines = QString::fromUtf8(bytes).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
  QStringList output;
  const int count = std::min(static_cast<int>(lines.size()), maxLines);
  for (int index = 0; index < count; ++index) {
    output << QStringLiteral("  ") + lines.at(index);
  }
  if (lines.size() > count) {
    output << QStringLiteral("  NOTE  JSON preview truncated at %1 of %2 lines.")
                  .arg(count)
                  .arg(lines.size());
  }
  return output.join(QLatin1Char('\n'));
}

struct GraphData {
  QVector<QString> ids;
  QVector<QString> labels;
  QVector<QString> details;
  QVector<QString> extras;
  QVector<QPair<int, int>> edges;
  QVector<QString> edgeLabels;
};

QJsonArray graphNodes(const QJsonObject& object, const QStringList& keys) {
  const QJsonArray nodes = firstArray(object, keys);
  if (!nodes.isEmpty()) {
    return nodes;
  }
  // Some parsers nest basic blocks under functions. Flatten that common shape
  // while preserving function names in the node detail column.
  QJsonArray flattened;
  for (const QJsonValue& value : object.value(QStringLiteral("functions")).toArray()) {
    const QJsonObject function = value.toObject();
    const QString functionName = field(function, {QStringLiteral("name"), QStringLiteral("id")},
                                       QStringLiteral("function"));
    const QJsonArray blocks =
        firstArray(function, {QStringLiteral("nodes"), QStringLiteral("blocks")});
    for (const QJsonValue& blockValue : blocks) {
      QJsonObject block = blockValue.toObject();
      if (!block.contains(QStringLiteral("function"))) {
        block.insert(QStringLiteral("function"), functionName);
      }
      flattened.push_back(block);
    }
  }
  return flattened;
}

GraphData graphDataFrom(const QJsonObject& object, const QStringList& nodeKeys) {
  GraphData data;
  const QJsonArray nodes = graphNodes(object, nodeKeys);
  QHash<QString, int> indexById;
  for (int index = 0; index < nodes.size(); ++index) {
    const QJsonObject node = nodes.at(index).toObject();
    const QString id = field(node, {QStringLiteral("id"), QStringLiteral("name"),
                                    QStringLiteral("label"), QStringLiteral("block")},
                             QStringLiteral("N%1").arg(index));
    const QString kind = field(node, {QStringLiteral("kind"), QStringLiteral("type")},
                               QStringLiteral("node"));
    const QString address =
        field(node, {QStringLiteral("address"), QStringLiteral("addr"), QStringLiteral("pc")},
              QStringLiteral("—"));
    const QString instructions =
        field(node, {QStringLiteral("instructions"), QStringLiteral("insn_count"),
                     QStringLiteral("size")},
              QString());
    const QString text =
        field(node, {QStringLiteral("text"), QStringLiteral("function"), QStringLiteral("detail")},
              QString());
    data.ids.push_back(id);
    data.labels.push_back(kind);
    data.details.push_back(address);
    data.extras.push_back(instructions.isEmpty() ? text
                                                 : QStringLiteral("%1 insn  %2").arg(instructions, text));
    indexById.insert(id, index);
  }

  QJsonArray edges = firstArray(object, {QStringLiteral("edges"), QStringLiteral("arcs"),
                                        QStringLiteral("transitions")});
  if (edges.isEmpty()) {
    for (const QJsonValue& value : object.value(QStringLiteral("functions")).toArray()) {
      const QJsonObject function = value.toObject();
      for (const QJsonValue& edge : firstArray(function, {QStringLiteral("edges"),
                                                          QStringLiteral("branches")})) {
        edges.push_back(edge);
      }
    }
  }
  for (const QJsonValue& value : edges) {
    const QJsonObject edge = value.toObject();
    const QString source = field(edge, {QStringLiteral("source"), QStringLiteral("from"),
                                        QStringLiteral("src")});
    const QString target = field(edge, {QStringLiteral("target"), QStringLiteral("to"),
                                        QStringLiteral("dst")});
    if (!indexById.contains(source) || !indexById.contains(target)) {
      continue;
    }
    data.edges.push_back({indexById.value(source), indexById.value(target)});
    data.edgeLabels.push_back(
        field(edge, {QStringLiteral("kind"), QStringLiteral("label")}, QString()));
  }
  return data;
}

StateGraph graphToStateGraph(const GraphData& data) {
  StateGraph graph;
  for (int index = 0; index < data.ids.size(); ++index) {
    StateNode node;
    node.id = data.ids.at(index);
    node.index = index;
    node.marking = data.labels.at(index);
    node.active = data.details.at(index);
    node.suspended = data.extras.at(index);
    node.level = index;
    graph.nodes.push_back(node);
  }
  for (int index = 0; index < data.edges.size(); ++index) {
    StateEdge edge;
    edge.from = data.edges.at(index).first;
    edge.to = data.edges.at(index).second;
    edge.label = data.edgeLabels.value(index);
    graph.edges.push_back(edge);
  }
  graph.valid = !graph.nodes.isEmpty();
  return graph;
}

QString buildElfText(const BinaryArtifacts& artifacts) {
  QStringList lines;
  lines << QStringLiteral("ELF ARTIFACT");
  lines << QStringLiteral("file        ") + QFileInfo(artifacts.elfPath).fileName();
  lines << QStringLiteral("path        ") + artifacts.elfPath;
  lines << QStringLiteral("size        ") + QString::number(artifacts.fileSize) +
               QStringLiteral(" bytes");
  if (artifacts.fileSize >= 1024) {
    lines << QStringLiteral("size        ") +
                 QString::number(artifacts.fileSize / 1024.0, 'f', 1) + QStringLiteral(" KiB");
  }
  lines << rule();

  const QJsonObject& elf = artifacts.elf;
  if (!elf.isEmpty()) {
    lines << QStringLiteral("METADATA");
    const QStringList keys = {QStringLiteral("format"), QStringLiteral("class"),
                              QStringLiteral("endian"), QStringLiteral("machine"),
                              QStringLiteral("architecture"), QStringLiteral("arch"),
                              QStringLiteral("language"), QStringLiteral("frontend"),
                              QStringLiteral("runtime"), QStringLiteral("entry"),
                              QStringLiteral("entry_point"), QStringLiteral("interpreter"),
                              QStringLiteral("compiler"), QStringLiteral("target")};
    for (const QString& key : keys) {
      if (!elf.contains(key)) {
        continue;
      }
      lines << QStringLiteral("  ") + fit(key, 16) + scalarText(elf.value(key));
    }
  }

  const QJsonArray sections = firstArray(elf, {QStringLiteral("sections")});
  if (!sections.isEmpty()) {
    lines << QString();
    lines << QStringLiteral("SECTIONS  ") + QString::number(sections.size());
    lines << QStringLiteral("  ") + fit(QStringLiteral("NAME"), 22) + fit(QStringLiteral("ADDRESS"), 14) +
                 fit(QStringLiteral("SIZE"), 10) + QStringLiteral("FLAGS");
    const int limit = std::min(120, static_cast<int>(sections.size()));
    for (int index = 0; index < limit; ++index) {
      const QJsonObject section = sections.at(index).toObject();
      lines << QStringLiteral("  ") +
                   fit(field(section, {QStringLiteral("name"), QStringLiteral("id")}), 22) +
                   fit(field(section, {QStringLiteral("address"), QStringLiteral("addr")}), 14) +
                   fit(field(section, {QStringLiteral("size")}), 10) +
                   field(section, {QStringLiteral("flags"), QStringLiteral("type")});
    }
  }

  const QJsonArray symbols = firstArray(elf, {QStringLiteral("symbols"), QStringLiteral("functions")});
  if (!symbols.isEmpty()) {
    lines << QString();
    lines << QStringLiteral("SYMBOLS  ") + QString::number(symbols.size());
    lines << QStringLiteral("  ") + fit(QStringLiteral("NAME"), 30) + fit(QStringLiteral("ADDRESS"), 14) +
                 QStringLiteral("KIND");
    const int limit = std::min(160, static_cast<int>(symbols.size()));
    for (int index = 0; index < limit; ++index) {
      const QJsonObject symbol = symbols.at(index).toObject();
      lines << QStringLiteral("  ") +
                   fit(field(symbol, {QStringLiteral("name"), QStringLiteral("id")}), 30) +
                   fit(field(symbol, {QStringLiteral("address"), QStringLiteral("addr")}), 14) +
                   field(symbol, {QStringLiteral("kind"), QStringLiteral("type")});
    }
  }

  if (!artifacts.hasBundle) {
    lines << QString();
    lines << QStringLiteral("  NOTE  No parser bundle was found next to this ELF.");
    lines << QStringLiteral("        Expected <elf>.bundle.json with elf/cfg/threads/net sections.");
  } else if (sections.isEmpty() && symbols.isEmpty()) {
    lines << QString();
    lines << QStringLiteral("  NOTE  The parser bundle carries no ELF sections/symbols; showing metadata only.");
  }

  if (elf.isEmpty() && !artifacts.hasBundle) {
    lines << QString();
    lines << prettyJson(elf, 40);
  }
  return lines.join(QLatin1Char('\n'));
}

QString buildCfgText(const BinaryArtifacts& artifacts) {
  QStringList lines;
  lines << QStringLiteral("CONTROL-FLOW GRAPH");
  lines << QStringLiteral("source  ") + QFileInfo(artifacts.elfPath).fileName();
  lines << rule();
  if (!artifacts.hasCfg || artifacts.cfg.isEmpty()) {
    lines << QStringLiteral("  No CFG artifacts in the parser bundle.");
    lines << QStringLiteral("  Expected: cfg.nodes[] / cfg.edges[] (or cfg.functions[] with blocks).");
    return lines.join(QLatin1Char('\n'));
  }

  const GraphData data =
      graphDataFrom(artifacts.cfg, {QStringLiteral("nodes"), QStringLiteral("blocks")});
  lines << QStringLiteral("%1 basic blocks  ·  %2 edges")
               .arg(data.ids.size())
               .arg(data.edges.size());
  if (!data.ids.isEmpty()) {
    lines << QString();
    lines << QStringLiteral("BLOCKS");
    lines << QStringLiteral("  ") + fit(QStringLiteral("ID"), 22) + fit(QStringLiteral("KIND"), 12) +
                 fit(QStringLiteral("ADDRESS"), 14) + QStringLiteral("DETAIL");
    const int limit = std::min(260, static_cast<int>(data.ids.size()));
    for (int index = 0; index < limit; ++index) {
      lines << QStringLiteral("  ") + fit(data.ids.at(index), 22) +
                   fit(data.labels.at(index), 12) + fit(data.details.at(index), 14) +
                   data.extras.at(index);
    }
    if (data.ids.size() > limit) {
      lines << QStringLiteral("  NOTE  showing first %1 of %2 blocks.").arg(limit).arg(data.ids.size());
    }
  }
  if (!data.edges.isEmpty()) {
    lines << QString();
    lines << QStringLiteral("EDGES  ") + QString::number(data.edges.size());
    const int limit = std::min(400, static_cast<int>(data.edges.size()));
    for (int index = 0; index < limit; ++index) {
      lines << QStringLiteral("  ") + fit(data.ids.at(data.edges.at(index).first), 22) +
                   QStringLiteral("  →  ") + fit(data.ids.at(data.edges.at(index).second), 22) +
                   data.edgeLabels.value(index);
    }
  }
  if (data.ids.isEmpty()) {
    lines << QString();
    lines << QStringLiteral("  CFG object present but no nodes were recognized.");
    lines << prettyJson(artifacts.cfg, 80);
  }
  return lines.join(QLatin1Char('\n'));
}

QString buildThreadText(const BinaryArtifacts& artifacts) {
  QStringList lines;
  lines << QStringLiteral("THREAD CONTROL");
  lines << QStringLiteral("source  ") + QFileInfo(artifacts.elfPath).fileName();
  lines << rule();
  if (!artifacts.hasThreads || artifacts.threads.isEmpty()) {
    lines << QStringLiteral("  No thread-control artifacts in the parser bundle.");
    lines << QStringLiteral("  Expected: threads.runtime / frontend, threads[] or nodes[], edges[].");
    return lines.join(QLatin1Char('\n'));
  }

  lines << QStringLiteral("runtime   ") +
               field(artifacts.threads, {QStringLiteral("runtime"), QStringLiteral("thread_runtime")});
  lines << QStringLiteral("frontend  ") +
               field(artifacts.threads, {QStringLiteral("frontend"), QStringLiteral("language_frontend")});
  lines << QStringLiteral("mode      ") +
               field(artifacts.threads, {QStringLiteral("mode"), QStringLiteral("model")});

  const GraphData data = graphDataFrom(
      artifacts.threads, {QStringLiteral("threads"), QStringLiteral("nodes"),
                          QStringLiteral("tasks")});
  lines << QStringLiteral("%1 thread/task entries  ·  %2 edges")
               .arg(data.ids.size())
               .arg(data.edges.size());
  if (!data.ids.isEmpty()) {
    lines << QString();
    lines << QStringLiteral("ENTRIES");
    lines << QStringLiteral("  ") + fit(QStringLiteral("ID"), 22) + fit(QStringLiteral("KIND"), 14) +
                 fit(QStringLiteral("ENTRY"), 18) + QStringLiteral("DETAIL");
    const int limit = std::min(160, static_cast<int>(data.ids.size()));
    for (int index = 0; index < limit; ++index) {
      lines << QStringLiteral("  ") + fit(data.ids.at(index), 22) +
                   fit(data.labels.at(index), 14) + fit(data.details.at(index), 18) +
                   data.extras.at(index);
    }
  }
  if (!data.edges.isEmpty()) {
    lines << QString();
    lines << QStringLiteral("EDGES  ") + QString::number(data.edges.size());
    for (int index = 0; index < std::min(300, static_cast<int>(data.edges.size())); ++index) {
      lines << QStringLiteral("  ") + fit(data.ids.at(data.edges.at(index).first), 22) +
                   QStringLiteral("  →  ") + fit(data.ids.at(data.edges.at(index).second), 22) +
                   data.edgeLabels.value(index);
    }
  }
  if (data.ids.isEmpty()) {
    lines << QString();
    lines << prettyJson(artifacts.threads, 100);
  }
  return lines.join(QLatin1Char('\n'));
}

QString buildNetText(const BinaryArtifacts& artifacts) {
  QStringList lines;
  lines << QStringLiteral("GENERATED UNIFIED NET");
  lines << QStringLiteral("source  ") + QFileInfo(artifacts.elfPath).fileName();
  if (!artifacts.bundlePath.isEmpty()) {
    lines << QStringLiteral("bundle  ") + QFileInfo(artifacts.bundlePath).fileName();
  }
  lines << rule();
  if (!artifacts.hasNet || artifacts.net.isEmpty()) {
    lines << QStringLiteral("  No generated net in the parser bundle.");
    lines << QStringLiteral("  Expected: net (or tdg/dag) in the unified format.");
    lines << QStringLiteral("  The analysis backend is intentionally pending in this workflow.");
    return lines.join(QLatin1Char('\n'));
  }

  lines << QStringLiteral("format    ") +
               field(artifacts.net, {QStringLiteral("format"), QStringLiteral("schema"),
                                     QStringLiteral("kind")});
  const QJsonObject configuration = artifacts.net.value(QStringLiteral("configuration")).toObject();
  if (!configuration.isEmpty()) {
    lines << QStringLiteral("policy    ") +
                 field(configuration, {QStringLiteral("policy")}, QStringLiteral("—"));
    lines << QStringLiteral("cores     ") +
                 field(configuration, {QStringLiteral("num_cpus")}, QStringLiteral("—"));
    lines << QStringLiteral("capacity  ") +
                 field(configuration, {QStringLiteral("task_place_capacity")}, QStringLiteral("—"));
  }

  const GraphData data = graphDataFrom(
      artifacts.net, {QStringLiteral("tasks"), QStringLiteral("nodes"),
                      QStringLiteral("places")});
  const QJsonArray tasks = firstArray(artifacts.net, {QStringLiteral("tasks")});
  lines << QStringLiteral("%1 nodes  ·  %2 edges")
               .arg(data.ids.size())
               .arg(data.edges.size());
  if (!tasks.isEmpty()) {
    lines << QString();
    lines << QStringLiteral("TASKS  ") + QString::number(tasks.size());
    lines << QStringLiteral("  ") + fit(QStringLiteral("ID"), 22) + fit(QStringLiteral("TYPE"), 9) +
                 fit(QStringLiteral("CORE"), 9) + fit(QStringLiteral("PRIO"), 7) +
                 fit(QStringLiteral("WCET"), 12) + fit(QStringLiteral("DEADLINE"), 10) +
                 QStringLiteral("PERIOD");
    const int limit = std::min(240, static_cast<int>(tasks.size()));
    for (int index = 0; index < limit; ++index) {
      const QJsonObject task = tasks.at(index).toObject();
      QString wcet = QStringLiteral("—");
      const QJsonArray times = task.value(QStringLiteral("time")).toArray();
      if (!times.isEmpty() && times.first().isArray()) {
        const QJsonArray interval = times.first().toArray();
        if (interval.size() >= 2) {
          wcet = QStringLiteral("[%1, %2]")
                     .arg(number(interval.at(0).toDouble()), number(interval.at(1).toDouble()));
        }
      }
      lines << QStringLiteral("  ") +
                   fit(field(task, {QStringLiteral("id"), QStringLiteral("name")}), 22) +
                   fit(field(task, {QStringLiteral("type")}), 9) +
                   fit(field(task, {QStringLiteral("core")}, QStringLiteral("—")), 9) +
                   fit(field(task, {QStringLiteral("priority")}, QStringLiteral("—")), 7) +
                   fit(wcet, 12) +
                   fit(field(task, {QStringLiteral("deadline")}, QStringLiteral("—")), 10) +
                   field(task, {QStringLiteral("period")}, QStringLiteral("—"));
    }
  } else if (!data.ids.isEmpty()) {
    lines << QString();
    lines << QStringLiteral("NODES");
    lines << QStringLiteral("  ") + fit(QStringLiteral("ID"), 24) + fit(QStringLiteral("KIND"), 12) +
                 fit(QStringLiteral("DETAIL"), 18) + QStringLiteral("EXTRA");
    const int limit = std::min(300, static_cast<int>(data.ids.size()));
    for (int index = 0; index < limit; ++index) {
      lines << QStringLiteral("  ") + fit(data.ids.at(index), 24) +
                   fit(data.labels.at(index), 12) + fit(data.details.at(index), 18) +
                   data.extras.at(index);
    }
  }
  if (!data.edges.isEmpty()) {
    lines << QString();
    lines << QStringLiteral("EDGES  ") + QString::number(data.edges.size());
    for (int index = 0; index < std::min(400, static_cast<int>(data.edges.size())); ++index) {
      lines << QStringLiteral("  ") + fit(data.ids.at(data.edges.at(index).first), 24) +
                   QStringLiteral("  →  ") + fit(data.ids.at(data.edges.at(index).second), 24) +
                   data.edgeLabels.value(index);
    }
  }
  if (data.ids.isEmpty()) {
    lines << QString();
    lines << QStringLiteral("  Net object present but no common node array was recognized.");
    lines << prettyJson(artifacts.net, 120);
  }
  lines << QString();
  lines << QStringLiteral("  NOTE  This is the generated-net preview. The binary analysis backend is pending.");
  return lines.join(QLatin1Char('\n'));
}

QString buildAssumptions(const BinaryArtifacts& artifacts, const BinaryDocs& docs) {
  QStringList lines;
  lines << QStringLiteral("WORKFLOW      BINARY · ELF");
  lines << QStringLiteral("ELF           ") + QFileInfo(artifacts.elfPath).fileName();
  lines << QStringLiteral("BUNDLE        ") +
               (artifacts.bundlePath.isEmpty() ? QStringLiteral("[missing]")
                                                : QFileInfo(artifacts.bundlePath).fileName());
  lines << QStringLiteral("ARCH          ") +
               field(artifacts.elf, {QStringLiteral("architecture"), QStringLiteral("arch"),
                                     QStringLiteral("machine")},
                     QStringLiteral("[unknown]"));
  lines << QStringLiteral("LANGUAGE      ") +
               field(artifacts.elf, {QStringLiteral("language"), QStringLiteral("source_language")},
                     QStringLiteral("[unknown]"));
  lines << QStringLiteral("FRONTEND      ") +
               field(artifacts.elf, {QStringLiteral("frontend"), QStringLiteral("runtime")},
                     field(artifacts.threads, {QStringLiteral("frontend"), QStringLiteral("runtime")},
                           QStringLiteral("[unknown]")));
  lines << QStringLiteral("CFG           ") + docs.cfgSummary;
  lines << QStringLiteral("THREADS       ") + docs.threadsSummary;
  lines << QStringLiteral("NET           ") + docs.netSummary;
  lines << QStringLiteral("ANALYSIS      backend pending");
  if (!artifacts.error.isEmpty()) {
    lines << QStringLiteral("ERROR         ") + artifacts.error;
  }
  return lines.join(QLatin1Char('\n'));
}

}  // namespace

BinaryArtifacts loadBinaryArtifacts(const QString& elfPath) {
  BinaryArtifacts artifacts;
  const QFileInfo elfInfo(elfPath);
  artifacts.elfPath = elfInfo.absoluteFilePath();
  artifacts.fileSize = elfInfo.size();

  const QStringList bundleCandidates = {
      artifacts.elfPath + QStringLiteral(".bundle.json"),
      artifacts.elfPath + QStringLiteral(".ptpn-bundle.json"),
      artifacts.elfPath + QStringLiteral(".ptpn.json"),
  };
  for (const QString& candidate : bundleCandidates) {
    QFile file(candidate);
    if (!file.open(QIODevice::ReadOnly)) {
      continue;
    }
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    if (!document.isObject()) {
      artifacts.error = QStringLiteral("Parser bundle is not a JSON object: ") + candidate;
      continue;
    }
    const QJsonObject root = document.object();
    artifacts.hasBundle = true;
    artifacts.bundlePath = QFileInfo(candidate).absoluteFilePath();
    artifacts.elf = root.value(QStringLiteral("elf")).toObject();
    artifacts.cfg = firstObject(root, {QStringLiteral("cfg"), QStringLiteral("control_flow"),
                                       QStringLiteral("controlFlow")});
    artifacts.threads = firstObject(root, {QStringLiteral("threads"),
                                           QStringLiteral("thread_control"),
                                           QStringLiteral("threadControl")});
    artifacts.net = firstObject(root, {QStringLiteral("net"), QStringLiteral("unified_net"),
                                       QStringLiteral("unifiedNet"), QStringLiteral("tdg"),
                                       QStringLiteral("dag")});
    break;
  }

  if (!artifacts.hasBundle) {
    const QStringList dagCandidates = {
        artifacts.elfPath + QStringLiteral(".dag.json"),
        artifacts.elfPath + QStringLiteral(".tdg.json"),
        elfInfo.absolutePath() + QLatin1Char('/') + elfInfo.completeBaseName() +
            QStringLiteral(".dag.json"),
        elfInfo.absolutePath() + QLatin1Char('/') + elfInfo.completeBaseName() +
            QStringLiteral(".tdg.json"),
    };
    for (const QString& candidate : dagCandidates) {
      QFile file(candidate);
      if (!file.open(QIODevice::ReadOnly)) {
        continue;
      }
      const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
      if (!document.isObject()) {
        continue;
      }
      artifacts.net = document.object();
      artifacts.bundlePath = QFileInfo(candidate).absoluteFilePath();
      break;
    }
  }

  if (artifacts.elf.isEmpty()) {
    artifacts.elf.insert(QStringLiteral("file"), elfInfo.fileName());
    artifacts.elf.insert(QStringLiteral("path"), artifacts.elfPath);
    artifacts.elf.insert(QStringLiteral("size"), static_cast<double>(artifacts.fileSize));
  }
  artifacts.hasCfg = !artifacts.cfg.isEmpty();
  artifacts.hasThreads = !artifacts.threads.isEmpty();
  artifacts.hasNet = !artifacts.net.isEmpty();
  return artifacts;
}

BinaryDocs renderBinaryArtifacts(const BinaryArtifacts& artifacts) {
  BinaryDocs docs;
  docs.workflow = QStringLiteral("BINARY · ELF");
  docs.elfText = buildElfText(artifacts);
  docs.cfgText = buildCfgText(artifacts);
  docs.threadsText = buildThreadText(artifacts);
  docs.netText = buildNetText(artifacts);

  const GraphData cfg =
      graphDataFrom(artifacts.cfg, {QStringLiteral("nodes"), QStringLiteral("blocks")});
  const GraphData threads = graphDataFrom(
      artifacts.threads, {QStringLiteral("threads"), QStringLiteral("nodes"),
                          QStringLiteral("tasks")});
  const GraphData net = graphDataFrom(
      artifacts.net, {QStringLiteral("tasks"), QStringLiteral("nodes"),
                      QStringLiteral("places")});
  docs.cfgGraph = graphToStateGraph(cfg);
  docs.threadGraph = graphToStateGraph(threads);
  docs.netGraph = graphToStateGraph(net);
  docs.cfgGraphValid = docs.cfgGraph.valid;
  docs.threadGraphValid = docs.threadGraph.valid;
  docs.netGraphValid = docs.netGraph.valid;

  docs.elfSummary = QStringLiteral("%1  ·  %2 bytes")
                        .arg(QFileInfo(artifacts.elfPath).fileName())
                        .arg(artifacts.fileSize);
  docs.cfgSummary = QStringLiteral("%1 BLOCKS  ·  %2 EDGES")
                        .arg(cfg.ids.size())
                        .arg(cfg.edges.size());
  docs.threadsSummary = QStringLiteral("%1 ENTRIES  ·  %2 EDGES")
                            .arg(threads.ids.size())
                            .arg(threads.edges.size());
  docs.netSummary = QStringLiteral("%1 NODES  ·  %2 EDGES")
                        .arg(net.ids.size())
                        .arg(net.edges.size());
  docs.assumptions = buildAssumptions(artifacts, docs);
  return docs;
}
