#pragma once

#include <QJsonObject>
#include <QPointF>
#include <QString>
#include <QStringList>
#include <QVector>

struct Place {
  QString id;
  QString label;
  int tokens = 0;
  int capacity = 0;
};

struct Transition {
  QString id;
  QString label;
  int core = -1;
  int priority = 0;
  bool suspendable = false;
  bool hasInterval = false;
  double intervalMin = 0.0;
  double intervalMax = 0.0;
};

struct PetriArc {
  int from = -1;
  int to = -1;
  bool fromPlace = false;
};

struct PetriNet {
  bool valid = false;
  QString name;
  QVector<Place> places;
  QVector<Transition> transitions;
  QVector<PetriArc> arcs;
  QVector<int> initialMarking;
};

struct StateNode {
  QString id;
  int index = 0;
  QString marking;
  QString active;
  QString suspended;
  QString elapsed;
  QPointF pos;
  int level = 0;
};

struct StateEdge {
  int from = -1;
  int to = -1;
  QString label;
};

struct StateGraph {
  bool valid = false;
  QVector<StateNode> nodes;
  QVector<StateEdge> edges;
};

struct AnalysisResult {
  bool ok = false;
  QString error;
  QString path;
  QString command;
  QStringList logs;
  qint64 elapsedMs = 0;
  PetriNet net;
  StateGraph scg;
  QJsonObject metrics;
  QJsonObject tdg;
  QString workflowLabel;
  QString binaryArtifact;
};

inline double jsonNumber(const QJsonObject& object, const char* key, double fallback = 0.0) {
  const QJsonValue value = object.value(QLatin1String(key));
  return value.isDouble() ? value.toDouble() : fallback;
}

inline bool jsonBool(const QJsonObject& object, const char* key, bool fallback = false) {
  const QJsonValue value = object.value(QLatin1String(key));
  return value.isBool() ? value.toBool() : fallback;
}

inline QString jsonString(const QJsonObject& object, const char* key,
                          const QString& fallback = QString()) {
  const QJsonValue value = object.value(QLatin1String(key));
  return value.isString() ? value.toString() : fallback;
}
