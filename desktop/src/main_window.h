#pragma once

#include <QColor>
#include <QMainWindow>

#include "app_model.h"

class QLabel;
class QPlainTextEdit;
class QPushButton;
class QTabWidget;
class QShowEvent;
class QDragEnterEvent;
class QDropEvent;

class AnalysisService;
class MetricsPanel;
class PetriView;
class StateGraphView;

class MainWindow : public QMainWindow {
  Q_OBJECT

 public:
  explicit MainWindow(QWidget* parent = nullptr);
  ~MainWindow() override;

 protected:
  void showEvent(QShowEvent* event) override;
  void dragEnterEvent(QDragEnterEvent* event) override;
  void dropEvent(QDropEvent* event) override;

 private:
  void buildUi();
  QWidget* buildHeader();
  QWidget* buildSidebar();
  QWidget* buildInspector();
  QWidget* buildPetriTab();
  QWidget* buildStateTab();
  QWidget* buildMetricsTab();
  void wire();
  void openFileDialog();
  void loadModel(const QString& path);
  void runAnalysis();
  void setBusy(bool busy);
  void handleResult(const AnalysisResult& result);
  void updateSummary();
  void showPlaceInfo(int index);
  void showTransitionInfo(int index);
  void showStateInfo(int index);
  void setStatusPill(const QString& text, const QColor& color);
  QString resolveCliPath() const;
  static QString sourcePath(const QString& relative);

  AnalysisResult result_;
  AnalysisService* service_ = nullptr;
  PetriView* petriView_ = nullptr;
  StateGraphView* stateView_ = nullptr;
  MetricsPanel* metricsPanel_ = nullptr;
  QTabWidget* tabs_ = nullptr;
  QLabel* pathChip_ = nullptr;
  QLabel* modelName_ = nullptr;
  QLabel* statusPill_ = nullptr;
  QLabel* verdictValue_ = nullptr;
  QLabel* verdictSub_ = nullptr;
  QLabel* statesValue_ = nullptr;
  QLabel* hyperValue_ = nullptr;
  QLabel* wcrtValue_ = nullptr;
  QLabel* coresValue_ = nullptr;
  QLabel* inspectorTitle_ = nullptr;
  QLabel* inspectorBody_ = nullptr;
  QLabel* elapsedLabel_ = nullptr;
  QLabel* cliLabel_ = nullptr;
  QPlainTextEdit* logView_ = nullptr;
  QPushButton* runButton_ = nullptr;
  QPushButton* openButton_ = nullptr;
  QPushButton* autoButton_ = nullptr;
  QPushButton* speedButton_ = nullptr;
  QString currentFile_;
  bool busy_ = false;
  bool darkTitleApplied_ = false;
  double speed_ = 1.0;
};
