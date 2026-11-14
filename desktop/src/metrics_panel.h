#pragma once

#include <QJsonObject>
#include <QWidget>

#include "app_model.h"

class MetricsPanel : public QWidget {
  Q_OBJECT

 public:
  explicit MetricsPanel(QWidget* parent = nullptr);

  void setResult(const AnalysisResult& result);
  QSize sizeHint() const override;

  struct TaskRow {
    QString name;
    int core = -1;
    int priority = 0;
    double wcet = 0.0;
    double wcrt = 0.0;
    double deadline = 0.0;
    double slack = 0.0;
    bool hasSlack = false;
    bool missed = false;
    bool capped = false;
    bool observed = false;
    QString verdict;
  };

  struct CoreRow {
    int core = 0;
    double utilMin = 0.0;
    double utilMax = 0.0;
    double busy = 0.0;
  };

 protected:
  void paintEvent(QPaintEvent* event) override;

 private:
  static QVector<TaskRow> parseTasks(const QJsonObject& metrics);
  static QVector<CoreRow> parseCores(const QJsonObject& metrics);
  void drawSectionTitle(QPainter& painter, int x, int y, const QString& title,
                        const QString& subtitle) const;
  void drawStatCard(QPainter& painter, const QRectF& rect, const QString& label,
                    const QString& value, const QString& detail, const QColor& accent) const;
  void drawTaskTable(QPainter& painter, int x, int y, int width,
                     const QVector<TaskRow>& tasks) const;
  void drawCores(QPainter& painter, int x, int y, int width, const QVector<CoreRow>& cores) const;
  void drawPills(QPainter& painter, int x, int y, const QJsonObject& metrics) const;

  QJsonObject metrics_;
  PetriNet net_;
  QVector<TaskRow> tasks_;
  QVector<CoreRow> cores_;
  bool hasResult_ = false;
};
