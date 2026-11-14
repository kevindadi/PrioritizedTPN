#include "analysis_service.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QStringList>

#include "dot_parser.h"

namespace {
QString makeRunTemplate() {
  QString root = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
  if (root.isEmpty()) {
    root = QDir::tempPath();
  }
  QDir().mkpath(root);
  return QDir(root).filePath(QStringLiteral("run-XXXXXX"));
}
}  // namespace

AnalysisService::AnalysisService(QObject* parent)
    : QObject(parent), tempDir_(makeRunTemplate()) {
  process_ = new QProcess(this);
  process_->setProcessChannelMode(QProcess::SeparateChannels);
  connect(process_, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
          &AnalysisService::handleFinished);
  connect(process_, &QProcess::errorOccurred, this, &AnalysisService::handleError);
}

void AnalysisService::setCliPath(const QString& path) { cliPath_ = path; }

QString AnalysisService::cliPath() const { return cliPath_; }

bool AnalysisService::busy() const {
  return process_->state() != QProcess::NotRunning;
}

QString AnalysisService::subcommandFor(const QString& filePath) const {
  const QString suffix = QFileInfo(filePath).suffix().toLower();
  if (suffix == QStringLiteral("rtsl")) {
    return QStringLiteral("rtsl");
  }
  if (suffix == QStringLiteral("ptpn")) {
    return QStringLiteral("ptpn");
  }
  return QStringLiteral("tdg");
}

QString AnalysisService::makeArtifactPath(const QString& prefix, const QString& suffix) const {
  QString base = QFileInfo(currentPath_).completeBaseName();
  static const QRegularExpression unsafe(QStringLiteral("[^A-Za-z0-9_.-]+"));
  base.replace(unsafe, QStringLiteral("_"));
  if (base.isEmpty()) {
    base = QStringLiteral("model");
  }
  const QString stamp =
      QString::number(QDateTime::currentDateTimeUtc().toMSecsSinceEpoch());
  const QString directory = tempDir_.isValid() ? tempDir_.path() : QDir::tempPath();
  QDir().mkpath(directory);
  return QDir(directory).filePath(prefix + QStringLiteral("_") + base + QStringLiteral("_") + stamp +
                                  suffix);
}

QString AnalysisService::readTextFile(const QString& path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    return {};
  }
  return QString::fromUtf8(file.readAll());
}

void AnalysisService::analyze(const QString& filePath) {
  if (busy()) {
    return;
  }
  if (cliPath_.isEmpty() || !QFileInfo::exists(cliPath_)) {
    AnalysisResult result;
    result.path = filePath;
    result.error = QStringLiteral("Cannot find the ptpn executable. Set PTPN_BIN or rebuild the CLI.");
    emit finished(result);
    return;
  }
  if (!QFileInfo::exists(filePath)) {
    AnalysisResult result;
    result.path = filePath;
    result.error = QStringLiteral("Input file does not exist: ") + filePath;
    emit finished(result);
    return;
  }

  currentPath_ = filePath;
  outputPath_ = makeArtifactPath(QStringLiteral("net"), QStringLiteral(".dot"));
  statePath_ = makeArtifactPath(QStringLiteral("scg"), QStringLiteral(".dot"));
  metricsPath_ = makeArtifactPath(QStringLiteral("metrics"), QStringLiteral(".json"));
  wcetPath_ = makeArtifactPath(QStringLiteral("wcet"), QStringLiteral(".json"));
  failureReported_ = false;

  const QString subcommand = subcommandFor(filePath);
  const QStringList arguments = {
      subcommand,
      QStringLiteral("-f"),
      filePath,
      QStringLiteral("--export-ptpn"),
      outputPath_,
      QStringLiteral("--export-scg"),
      statePath_,
      QStringLiteral("--export-metrics"),
      metricsPath_,
      QStringLiteral("--export-wcet"),
      wcetPath_,
      QStringLiteral("--max-states"),
      QStringLiteral("20000"),
  };

  timer_.start();
  emit started(filePath);
  process_->setProgram(cliPath_);
  process_->setArguments(arguments);
  process_->setWorkingDirectory(QFileInfo(filePath).absolutePath());
  process_->start();
}

void AnalysisService::handleError(QProcess::ProcessError error) {
  if (error != QProcess::FailedToStart || failureReported_) {
    return;
  }
  failureReported_ = true;
  AnalysisResult result;
  result.path = currentPath_;
  result.error = QStringLiteral("Failed to start ptpn: ") + process_->errorString();
  result.logs = {result.error};
  result.elapsedMs = timer_.isValid() ? timer_.elapsed() : 0;
  emit finished(result);
}

void AnalysisService::handleFinished(int exitCode, QProcess::ExitStatus status) {
  if (failureReported_) {
    return;
  }
  const QString standardOutput = QString::fromUtf8(process_->readAllStandardOutput());
  const QString standardError = QString::fromUtf8(process_->readAllStandardError());

  AnalysisResult result;
  result.path = currentPath_;
  result.elapsedMs = timer_.isValid() ? timer_.elapsed() : 0;
  const QString combined = standardOutput + (standardError.isEmpty() ? QString() : standardError);
  result.logs = combined.split(QLatin1Char('\n'), Qt::SkipEmptyParts);

  result.net = dot::parsePetri(readTextFile(outputPath_));
  result.scg = dot::parseStateGraph(readTextFile(statePath_));

  const QByteArray metricsJson = [&]() {
    QFile file(metricsPath_);
    if (!file.open(QIODevice::ReadOnly)) {
      return QByteArray();
    }
    return file.readAll();
  }();
  if (!metricsJson.isEmpty()) {
    const QJsonDocument document = QJsonDocument::fromJson(metricsJson);
    if (document.isObject()) {
      result.metrics = document.object();
    }
  }

  const bool normalExit = status == QProcess::NormalExit;
  if (exitCode != 0 || !normalExit) {
    result.ok = false;
    QStringList interesting;
    for (const QString& line : result.logs) {
      if (line.contains(QStringLiteral("ERROR")) || line.contains(QStringLiteral("error")) ||
          line.contains(QStringLiteral("failed")) || line.contains(QStringLiteral("Unsupported"))) {
        interesting.push_back(line.trimmed());
      }
    }
    if (!interesting.isEmpty()) {
      result.error = interesting.mid(interesting.size() > 4 ? interesting.size() - 4 : 0).join('\n');
    } else if (!result.logs.isEmpty()) {
      result.error = result.logs.mid(result.logs.size() > 4 ? result.logs.size() - 4 : 0).join('\n');
    } else {
      result.error = QStringLiteral("ptpn exited with code ") + QString::number(exitCode);
    }
    emit finished(result);
    return;
  }

  if (!result.net.valid) {
    result.ok = false;
    result.error = QStringLiteral("The analyzer produced no PTPN structure. Check the run log.");
    emit finished(result);
    return;
  }

  result.ok = true;
  emit finished(result);
}
