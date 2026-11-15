#pragma once

#include <QColor>
#include <QMainWindow>

#include "app_model.h"
#include "binary_artifacts.h"
#include "workbench_text.h"

class QLabel;
class QPlainTextEdit;
class QPushButton;
class QTabWidget;
class QShowEvent;
class QDragEnterEvent;
class QDropEvent;

class AnalysisService;
class TextView;

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
  QWidget* buildStagePage(const QString& title, const QString& subtitle, TextView** view,
                          QLabel** summary, QPushButton** previewButton, bool previewable);
  QWidget* buildDagTab();
  QWidget* buildPtpnTab();
  QWidget* buildStateTab();
  QWidget* buildTimelineTab();
  QWidget* buildReportTab();
  QWidget* buildElfTab();
  QWidget* buildCfgTab();
  QWidget* buildThreadsTab();
  QWidget* buildNetTab();
  void wire();
  void showWorkflowHub();
  void openFileDialog();
  void importBinary();
  bool importBinaryPath(const QString& elfPath, bool interactive);
  void showBinaryMode(bool binary);
  void applyBinaryDocs(const BinaryDocs& docs);
  void openGraphPreview(const QString& title, const StateGraph& graph, bool valid,
                        const QString& footer);
  void loadModel(const QString& path, const QString& workflowLabel = QString());
  void runAnalysis();
  void setBusy(bool busy);
  void handleResult(const AnalysisResult& result);
  void applyDocs(const WorkbenchDocs& docs);
  void updateSummary();
  void updateRunHistory();
  void openPetriPreview();
  void openStatePreview();
  void showPlaceInfo(int index);
  void showTransitionInfo(int index);
  void showStateInfo(int index);
  void setStatusPill(const QString& text, const QColor& color);
  QString resolveCliPath() const;
  static QString sourcePath(const QString& relative);
  static QString workflowShortName(const QString& path);

  struct RunRecord {
    QString time;
    QString verdict;
    QString detail;
  };

  AnalysisResult result_;
  WorkbenchDocs docs_;
  AnalysisService* service_ = nullptr;
  QTabWidget* tabs_ = nullptr;

  TextView* dagView_ = nullptr;
  TextView* ptpnView_ = nullptr;
  TextView* stateView_ = nullptr;
  TextView* timelineView_ = nullptr;
  TextView* reportView_ = nullptr;
  TextView* elfView_ = nullptr;
  TextView* cfgView_ = nullptr;
  TextView* threadsView_ = nullptr;
  TextView* netView_ = nullptr;

  QLabel* dagSummary_ = nullptr;
  QLabel* ptpnSummary_ = nullptr;
  QLabel* stateSummary_ = nullptr;
  QLabel* timelineSummary_ = nullptr;
  QLabel* reportSummary_ = nullptr;
  QLabel* elfSummary_ = nullptr;
  QLabel* cfgSummary_ = nullptr;
  QLabel* threadsSummary_ = nullptr;
  QLabel* netSummary_ = nullptr;

  QPushButton* ptpnPreview_ = nullptr;
  QPushButton* statePreview_ = nullptr;
  QPushButton* cfgPreview_ = nullptr;
  QPushButton* threadsPreview_ = nullptr;
  QPushButton* netPreview_ = nullptr;

  QLabel* workflowChip_ = nullptr;
  QLabel* workflowSidebarValue_ = nullptr;
  QLabel* workflowSidebarHint_ = nullptr;
  QLabel* artifactsLabel_ = nullptr;
  QLabel* runHistoryLabel_ = nullptr;
  QLabel* pathChip_ = nullptr;
  QLabel* modelName_ = nullptr;
  QLabel* statusPill_ = nullptr;
  QLabel* verdictValue_ = nullptr;
  QLabel* verdictSub_ = nullptr;
  QLabel* statesValue_ = nullptr;
  QLabel* hyperValue_ = nullptr;
  QLabel* wcrtValue_ = nullptr;
  QLabel* coresValue_ = nullptr;
  QLabel* detailTitle_ = nullptr;
  QLabel* detailBody_ = nullptr;
  QLabel* elapsedLabel_ = nullptr;
  QLabel* cliLabel_ = nullptr;
  QPlainTextEdit* assumptionsView_ = nullptr;
  QPlainTextEdit* logView_ = nullptr;
  QPushButton* runButton_ = nullptr;
  QPushButton* openButton_ = nullptr;
  QString currentFile_;
  QString workflowOverride_;
  QString binaryElf_;
  QString binaryBundlePath_;
  BinaryArtifacts binary_;
  BinaryDocs binaryDocs_;
  QVector<RunRecord> runHistory_;
  bool binaryMode_ = false;
  bool busy_ = false;
  bool darkTitleApplied_ = false;
};
