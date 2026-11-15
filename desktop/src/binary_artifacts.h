#pragma once

#include <QJsonObject>
#include <QString>

#include "app_model.h"

// Artifacts emitted by the external ELF / language-frontend parser. The
// desktop reads a single bundle so ELF metadata, CFG, thread-control and the
// unified generated net stay traceable to one parse.
struct BinaryArtifacts {
  QString elfPath;
  QString bundlePath;
  QString error;
  QJsonObject elf;
  QJsonObject cfg;
  QJsonObject threads;
  QJsonObject net;
  bool hasBundle = false;
  bool hasCfg = false;
  bool hasThreads = false;
  bool hasNet = false;
  qint64 fileSize = 0;
};

struct BinaryDocs {
  QString elfText;
  QString cfgText;
  QString threadsText;
  QString netText;

  QString elfSummary;
  QString cfgSummary;
  QString threadsSummary;
  QString netSummary;
  QString assumptions;
  QString workflow;

  StateGraph cfgGraph;
  StateGraph threadGraph;
  StateGraph netGraph;
  bool cfgGraphValid = false;
  bool threadGraphValid = false;
  bool netGraphValid = false;
};

BinaryArtifacts loadBinaryArtifacts(const QString& elfPath);
BinaryDocs renderBinaryArtifacts(const BinaryArtifacts& artifacts);
