#include "dot_parser.h"

#include <QHash>
#include <QMap>
#include <QRegularExpression>
#include <QStringList>

namespace dot {
namespace {

QString decodeEscapes(QString value) {
  value.replace(QStringLiteral("\\\\n"), QStringLiteral("\n"));
  value.replace(QStringLiteral("\\n"), QStringLiteral("\n"));
  value.replace(QStringLiteral("\\\\\""), QStringLiteral("\""));
  value.replace(QStringLiteral("\\\""), QStringLiteral("\""));
  value.replace(QStringLiteral("\\\\"), QStringLiteral("\\"));
  value.replace(QStringLiteral("\\t"), QStringLiteral("\t"));
  return value;
}

QMap<QString, QString> parseAttributes(const QString& block) {
  QMap<QString, QString> attributes;
  static const QRegularExpression attributePattern(
      QStringLiteral(R"dot(([A-Za-z_][A-Za-z0-9_]*)\s*=\s*"((?:[^"\\]|\\.)*)")dot"));
  QRegularExpressionMatchIterator it = attributePattern.globalMatch(block);
  while (it.hasNext()) {
    const QRegularExpressionMatch match = it.next();
    attributes.insert(match.captured(1), decodeEscapes(match.captured(2)));
  }
  return attributes;
}

QString decodeId(QString id) {
  if (id.size() >= 2 && id.startsWith(QLatin1Char('"')) && id.endsWith(QLatin1Char('"'))) {
    id = id.mid(1, id.size() - 2);
  }
  return id;
}

int intAttribute(const QMap<QString, QString>& attributes, const QString& name, int fallback) {
  bool ok = false;
  const int value = attributes.value(name).toInt(&ok);
  return ok ? value : fallback;
}

struct RawNode {
  QString id;
  QMap<QString, QString> attributes;
};

struct RawEdge {
  QString from;
  QString to;
  QString label;
};

struct DotDocument {
  QVector<RawNode> nodes;
  QVector<RawEdge> edges;
};

DotDocument parseDocument(const QString& text) {
  DotDocument document;
  static const QRegularExpression edgePattern(
      QStringLiteral(R"dot(^\s*("?[^"\s;]+"?)\s*->\s*("?[^"\s;]+"?)(?:\s*\[(.*)\])?\s*;?\s*$)dot"));
  static const QRegularExpression nodePattern(
      QStringLiteral(R"dot(^\s*("?[^"\s;]+"?)\s*\[(.*)\]\s*;?\s*$)dot"));

  const QStringList lines = text.split(QLatin1Char('\n'));
  for (const QString& rawLine : lines) {
    const QString line = rawLine.trimmed();
    if (line.isEmpty() || line.startsWith(QStringLiteral("digraph")) ||
        line.startsWith(QStringLiteral("graph")) || line.startsWith(QStringLiteral("node ")) ||
        line.startsWith(QStringLiteral("edge ")) || line == QStringLiteral("{") ||
        line == QStringLiteral("}")) {
      continue;
    }

    const QRegularExpressionMatch edge = edgePattern.match(line);
    if (edge.hasMatch()) {
      RawEdge parsed;
      parsed.from = decodeId(edge.captured(1));
      parsed.to = decodeId(edge.captured(2));
      parsed.label = parseAttributes(edge.captured(3)).value(QStringLiteral("label"));
      document.edges.push_back(parsed);
      continue;
    }

    const QRegularExpressionMatch node = nodePattern.match(line);
    if (node.hasMatch()) {
      RawNode parsed;
      parsed.id = decodeId(node.captured(1));
      parsed.attributes = parseAttributes(node.captured(2));
      if (!parsed.id.isEmpty()) {
        document.nodes.push_back(parsed);
      }
    }
  }
  return document;
}

bool looksLikePlace(const RawNode& node) {
  const QString shape = node.attributes.value(QStringLiteral("shape")).toLower();
  if (shape == QStringLiteral("circle") || shape == QStringLiteral("ellipse") ||
      shape == QStringLiteral("doublecircle")) {
    return true;
  }
  const QString label = node.attributes.value(QStringLiteral("label"));
  return label.contains(QStringLiteral("M=")) && !label.contains(QStringLiteral("core="));
}

void parsePlaceLabel(const QString& label, Place& place) {
  static const QRegularExpression tokens(QStringLiteral(R"(M\s*=\s*(\d+))"));
  static const QRegularExpression capacity(QStringLiteral(R"(C\s*=\s*(\d+))"));
  const QRegularExpressionMatch tokenMatch = tokens.match(label);
  if (tokenMatch.hasMatch()) {
    place.tokens = tokenMatch.captured(1).toInt();
  }
  const QRegularExpressionMatch capacityMatch = capacity.match(label);
  if (capacityMatch.hasMatch()) {
    place.capacity = capacityMatch.captured(1).toInt();
  }
  place.label = label;
}

void parseTransitionLabel(const QString& label, Transition& transition) {
  static const QRegularExpression core(QStringLiteral(R"(core\s*=\s*(-?\d+))"));
  static const QRegularExpression priority(
      QStringLiteral(R"((?:π|priority|prio)\s*=\s*(-?\d+))"));
  static const QRegularExpression interval(
      QStringLiteral(R"(I\s*=\s*\[\[?\s*(-?\d+(?:\.\d+)?)\s*,\s*(-?\d+(?:\.\d+)?))"));

  const QRegularExpressionMatch coreMatch = core.match(label);
  if (coreMatch.hasMatch()) {
    transition.core = coreMatch.captured(1).toInt();
  }
  const QRegularExpressionMatch priorityMatch = priority.match(label);
  if (priorityMatch.hasMatch()) {
    transition.priority = priorityMatch.captured(1).toInt();
  }
  const QRegularExpressionMatch intervalMatch = interval.match(label);
  if (intervalMatch.hasMatch()) {
    transition.hasInterval = true;
    transition.intervalMin = intervalMatch.captured(1).toDouble();
    transition.intervalMax = intervalMatch.captured(2).toDouble();
  }
  transition.suspendable = label.contains(QStringLiteral("suspendable"));
  transition.label = label;
}

void parseTooltip(const QString& tooltip, StateNode& node) {
  const QStringList lines = tooltip.split(QLatin1Char('\n'));
  for (const QString& rawLine : lines) {
    const QString line = rawLine.trimmed();
    if (line.startsWith(QStringLiteral("Marking:"))) {
      node.marking = line.mid(QStringLiteral("Marking:").size()).trimmed();
    } else if (line.startsWith(QStringLiteral("E_pri (active):"))) {
      node.active = line.mid(QStringLiteral("E_pri (active):").size()).trimmed();
    } else if (line.startsWith(QStringLiteral("Suspended:"))) {
      node.suspended = line.mid(QStringLiteral("Suspended:").size()).trimmed();
    } else if (line.startsWith(QStringLiteral("Elapsed time:"))) {
      node.elapsed = line.mid(QStringLiteral("Elapsed time:").size()).trimmed();
    }
  }
}

}  // namespace

PetriNet parsePetri(const QString& text) {
  PetriNet net;
  const DotDocument document = parseDocument(text);

  QHash<QString, int> placeIndex;
  QHash<QString, int> transitionIndex;

  for (const RawNode& raw : document.nodes) {
    const QString label = raw.attributes.value(QStringLiteral("label"));
    if (looksLikePlace(raw)) {
      Place place;
      place.id = raw.id;
      parsePlaceLabel(label, place);
      placeIndex.insert(place.id, net.places.size());
      net.places.push_back(place);
    } else {
      Transition transition;
      transition.id = raw.id;
      parseTransitionLabel(label, transition);
      transitionIndex.insert(transition.id, net.transitions.size());
      net.transitions.push_back(transition);
    }
  }

  for (const RawEdge& raw : document.edges) {
    PetriArc arc;
    const bool fromPlace = placeIndex.contains(raw.from);
    const bool toPlace = placeIndex.contains(raw.to);
    if (fromPlace && transitionIndex.contains(raw.to)) {
      arc.from = placeIndex.value(raw.from);
      arc.to = transitionIndex.value(raw.to);
      arc.fromPlace = true;
    } else if (transitionIndex.contains(raw.from) && toPlace) {
      arc.from = transitionIndex.value(raw.from);
      arc.to = placeIndex.value(raw.to);
      arc.fromPlace = false;
    } else {
      continue;
    }
    net.arcs.push_back(arc);
  }

  net.initialMarking.reserve(net.places.size());
  for (const Place& place : net.places) {
    net.initialMarking.push_back(place.tokens);
  }
  net.valid = !net.places.isEmpty() && !net.transitions.isEmpty();
  return net;
}

StateGraph parseStateGraph(const QString& text) {
  StateGraph graph;
  const DotDocument document = parseDocument(text);

  QHash<QString, int> indexById;
  for (const RawNode& raw : document.nodes) {
    StateNode node;
    node.id = raw.id;
    const QString tooltip = raw.attributes.value(QStringLiteral("tooltip"));
    if (!tooltip.isEmpty()) {
      parseTooltip(tooltip, node);
    }
    static const QRegularExpression stateNumber(QStringLiteral(R"(s?(\d+))"));
    const QRegularExpressionMatch numberMatch = stateNumber.match(raw.id);
    node.index = numberMatch.hasMatch() ? numberMatch.captured(1).toInt() : graph.nodes.size();
    indexById.insert(node.id, graph.nodes.size());
    graph.nodes.push_back(node);
  }

  for (const RawEdge& raw : document.edges) {
    if (!indexById.contains(raw.from) || !indexById.contains(raw.to)) {
      continue;
    }
    StateEdge edge;
    edge.from = indexById.value(raw.from);
    edge.to = indexById.value(raw.to);
    edge.label = raw.label.section(QLatin1Char('\n'), 0, 0).trimmed();
    graph.edges.push_back(edge);
  }

  graph.valid = !graph.nodes.isEmpty();
  return graph;
}

}  // namespace dot
