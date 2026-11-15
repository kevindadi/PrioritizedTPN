#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QProcess>
#include <QTemporaryDir>

#include "app_model.h"

class AnalysisService : public QObject {
  Q_OBJECT

 public:
  explicit AnalysisService(QObject* parent = nullptr);

  void setCliPath(const QString& path);
  QString cliPath() const;
  bool busy() const;

  void analyze(const QString& filePath);

 signals:
  void started(const QString& filePath);
  void finished(const AnalysisResult& result);

 private slots:
  void handleFinished(int exitCode, QProcess::ExitStatus status);
  void handleError(QProcess::ProcessError error);

 private:
  QString subcommandFor(const QString& filePath) const;
  QString makeArtifactPath(const QString& prefix, const QString& suffix) const;
  static QString readTextFile(const QString& path);

  QProcess* process_ = nullptr;
  QTemporaryDir tempDir_;
  QString cliPath_;
  QString currentPath_;
  QString outputPath_;
  QString statePath_;
  QString metricsPath_;
  QString wcetPath_;
  QString tdgPath_;
  QElapsedTimer timer_;
  bool failureReported_ = false;
};
