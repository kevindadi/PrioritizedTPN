#include "main_window.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontMetrics>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QLinearGradient>
#include <QMessageBox>
#include <QMenu>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRadialGradient>
#include <QScrollArea>
#include <QScrollBar>
#include <QShortcut>
#include <QShowEvent>
#include <QSizePolicy>
#include <QStatusBar>
#include <QStringList>
#include <QTabWidget>
#include <QTime>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QWindow>

#include <algorithm>
#include <cmath>

#if defined(Q_OS_WIN)
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <dwmapi.h>
#  include <windows.h>
#endif

#include "analysis_service.h"
#include "preview_dialog.h"
#include "text_view.h"
#include "theme.h"
#include "workbench_text.h"
#include "workflow_dialog.h"

namespace {

constexpr double kPi = 3.14159265358979323846;

class CompactTabWidget final : public QTabWidget {
 public:
  explicit CompactTabWidget(QWidget* parent = nullptr) : QTabWidget(parent) {}
  QSize minimumSizeHint() const override { return QSize(280, 180); }
  QSize sizeHint() const override { return QSize(900, 560); }
};

class LogoWidget final : public QWidget {
 public:
  explicit LogoWidget(QWidget* parent = nullptr) : QWidget(parent) {
    setFixedSize(48, 48);
    setAttribute(Qt::WA_TranslucentBackground, true);
  }

 protected:
  void paintEvent(QPaintEvent*) override {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QPointF center(width() / 2.0, height() / 2.0);
    const double radius = 17.0;

    QRadialGradient glow(center, 27.0);
    glow.setColorAt(0.0, theme::alpha(theme::c("#22D3EE"), 80));
    glow.setColorAt(1.0, Qt::transparent);
    painter.setPen(Qt::NoPen);
    painter.setBrush(glow);
    painter.drawEllipse(center, 27.0, 27.0);

    QPainterPath hexagon;
    for (int i = 0; i < 6; ++i) {
      const double angle = -kPi / 2.0 + i * kPi / 3.0;
      const QPointF point(center.x() + std::cos(angle) * radius,
                          center.y() + std::sin(angle) * radius);
      if (i == 0) {
        hexagon.moveTo(point);
      } else {
        hexagon.lineTo(point);
      }
    }
    hexagon.closeSubpath();

    QLinearGradient fill(QPointF(0, 0), QPointF(width(), height()));
    fill.setColorAt(0.0, theme::alpha(theme::c("#0E7490"), 230));
    fill.setColorAt(1.0, theme::alpha(theme::c("#4C1D95"), 210));
    painter.setBrush(fill);
    painter.setPen(QPen(theme::c("#67E8F9"), 1.6));
    painter.drawPath(hexagon);

    const QPointF nodes[] = {center + QPointF(0, -11), center + QPointF(-10, 7),
                             center + QPointF(10, 7)};
    painter.setPen(QPen(theme::alpha(theme::c("#A5F3FC"), 200), 1.2));
    painter.drawLine(nodes[0], nodes[1]);
    painter.drawLine(nodes[1], nodes[2]);
    painter.drawLine(nodes[2], nodes[0]);
    for (int i = 0; i < 3; ++i) {
      painter.setPen(Qt::NoPen);
      painter.setBrush(i == 0 ? theme::c("#F0FDFF") : theme::c("#67E8F9"));
      painter.drawEllipse(nodes[i], i == 0 ? 3.2 : 2.5, i == 0 ? 3.2 : 2.5);
    }
  }
};

QLabel* sectionLabel(const QString& text) {
  auto* label = new QLabel(text.toUpper());
  label->setObjectName(QStringLiteral("sectionTitle"));
  return label;
}

QFrame* divider() {
  auto* line = new QFrame;
  line->setFrameShape(QFrame::HLine);
  line->setStyleSheet(QStringLiteral("color: #152238; background: #152238; max-height: 1px;"));
  return line;
}

QWidget* statBlock(const QString& title, QLabel** value, const QColor& accent) {
  auto* frame = new QFrame;
  frame->setObjectName(QStringLiteral("softPanel"));
  frame->setFixedHeight(66);
  auto* layout = new QVBoxLayout(frame);
  layout->setContentsMargins(10, 7, 10, 6);
  layout->setSpacing(0);
  auto* label = new QLabel(title.toUpper());
  label->setStyleSheet(QStringLiteral("color:#64748B; font-size:8pt; font-weight:600;"));
  auto* number = new QLabel(QStringLiteral("—"));
  number->setStyleSheet(QStringLiteral("color:%1; font-size:13pt; font-weight:700;")
                            .arg(accent.name()));
  layout->addWidget(label);
  layout->addWidget(number);
  *value = number;
  return frame;
}

QString formattedValue(const QJsonObject& metrics, const char* key, const QString& fallback) {
  const QJsonValue value = metrics.value(QLatin1String(key));
  if (value.isDouble()) {
    return QString::number(value.toDouble(), 'f', 0);
  }
  return fallback;
}

}  // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
  service_ = new AnalysisService(this);
  buildUi();
  wire();

  const QString cli = resolveCliPath();
  service_->setCliPath(cli);
  if (cliLabel_ != nullptr) {
    cliLabel_->setText(cli.isEmpty() ? QStringLiteral("ENGINE NOT FOUND · set PTPN_BIN")
                                    : QStringLiteral("ENGINE  ") + QDir::toNativeSeparators(cli));
  }

  QPixmap iconPixmap(64, 64);
  iconPixmap.fill(Qt::transparent);
  QPainter iconPainter(&iconPixmap);
  iconPainter.setRenderHint(QPainter::Antialiasing, true);
  QPainterPath iconPath;
  for (int i = 0; i < 6; ++i) {
    const double angle = -kPi / 2.0 + i * kPi / 3.0;
    const QPointF point(32 + std::cos(angle) * 25, 32 + std::sin(angle) * 25);
    if (i == 0) {
      iconPath.moveTo(point);
    } else {
      iconPath.lineTo(point);
    }
  }
  iconPath.closeSubpath();
  QLinearGradient iconFill(0, 0, 64, 64);
  iconFill.setColorAt(0.0, theme::c("#22D3EE"));
  iconFill.setColorAt(1.0, theme::c("#7C3AED"));
  iconPainter.setBrush(iconFill);
  iconPainter.setPen(Qt::NoPen);
  iconPainter.drawPath(iconPath);
  iconPainter.end();
  setWindowIcon(QIcon(iconPixmap));

  QTimer::singleShot(240, this, [this] {
    QString startupFile;
    QString startupElf;
    for (const QString& argument : QCoreApplication::arguments().mid(1)) {
      const QFileInfo info(argument);
      const QString suffix = info.suffix().toLower();
      if (!info.isFile()) {
        continue;
      }
      if (suffix == QStringLiteral("elf")) {
        startupElf = info.absoluteFilePath();
      } else if (suffix == QStringLiteral("rtsl") || suffix == QStringLiteral("json") ||
                 suffix == QStringLiteral("ptpn")) {
        startupFile = info.absoluteFilePath();
      }
    }
    if (!startupElf.isEmpty()) {
      importBinaryPath(startupElf, false);
    } else if (!startupFile.isEmpty()) {
      loadModel(startupFile);
    } else {
      QTimer::singleShot(120, this, &MainWindow::showWorkflowHub);
    }
  });
}

MainWindow::~MainWindow() = default;

void MainWindow::buildUi() {
  setWindowTitle(QStringLiteral("PTPN  //  Analysis Workbench"));
  resize(1520, 940);
  setMinimumSize(1120, 720);
  setAcceptDrops(true);

  auto* central = new QWidget;
  auto* root = new QVBoxLayout(central);
  root->setContentsMargins(0, 0, 0, 0);
  root->setSpacing(0);
  root->addWidget(buildHeader());

  auto* body = new QWidget;
  auto* bodyLayout = new QHBoxLayout(body);
  bodyLayout->setContentsMargins(16, 14, 16, 16);
  bodyLayout->setSpacing(14);
  bodyLayout->addWidget(buildSidebar());

  tabs_ = new CompactTabWidget;
  tabs_->setDocumentMode(true);
  tabs_->setMinimumSize(280, 180);
  tabs_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
  tabs_->addTab(buildDagTab(), QStringLiteral("DAG"));
  tabs_->addTab(buildPtpnTab(), QStringLiteral("PTPN"));
  tabs_->addTab(buildStateTab(), QStringLiteral("STATE SPACE"));
  tabs_->addTab(buildTimelineTab(), QStringLiteral("TIMELINE"));
  tabs_->addTab(buildReportTab(), QStringLiteral("REPORT"));
  bodyLayout->addWidget(tabs_, 1);
  bodyLayout->addWidget(buildInspector());
  root->addWidget(body, 1);
  setCentralWidget(central);

  statusBar()->setSizeGripEnabled(true);
  statusBar()->showMessage(QStringLiteral("READY  ·  text-first workbench"));
  elapsedLabel_ = new QLabel(QStringLiteral("—"));
  elapsedLabel_->setStyleSheet(QStringLiteral("color:#64748B; font-family:'Cascadia Mono';"));
  statusBar()->addPermanentWidget(elapsedLabel_);

  // Keyboard-first navigation: the workbench is designed for dense text and
  // large models, so tab switching and preview should not require the mouse.
  for (int index = 0; index < 5; ++index) {
    auto* shortcut = new QShortcut(QKeySequence(QStringLiteral("Ctrl+%1").arg(index + 1)), this);
    connect(shortcut, &QShortcut::activated, this,
            [this, index] { tabs_->setCurrentIndex(index); });
  }
  auto* previewShortcut = new QShortcut(QKeySequence(QStringLiteral("Ctrl+P")), this);
  connect(previewShortcut, &QShortcut::activated, this, [this] {
    if (tabs_->currentIndex() == 1) {
      openPetriPreview();
    } else if (tabs_->currentIndex() == 2) {
      openStatePreview();
    }
  });
}

QWidget* MainWindow::buildHeader() {
  auto* header = new QFrame;
  header->setObjectName(QStringLiteral("header"));
  header->setFixedHeight(82);
  auto* layout = new QHBoxLayout(header);
  layout->setContentsMargins(20, 10, 20, 10);
  layout->setSpacing(12);

  layout->addWidget(new LogoWidget);

  auto* titleColumn = new QVBoxLayout;
  titleColumn->setSpacing(0);
  auto* title = new QLabel(QStringLiteral("PTPN"));
  title->setObjectName(QStringLiteral("brand"));
  title->setFont(theme::display(20, QFont::Bold));
  auto* subtitle = new QLabel(QStringLiteral("ANALYSIS WORKBENCH"));
  subtitle->setObjectName(QStringLiteral("brandSub"));
  subtitle->setFont(theme::mono(7, QFont::DemiBold));
  titleColumn->addWidget(title);
  titleColumn->addWidget(subtitle);
  layout->addLayout(titleColumn);
  layout->addSpacing(10);

  workflowChip_ = new QLabel(QStringLiteral("NO WORKFLOW"));
  workflowChip_->setObjectName(QStringLiteral("workflowChip"));
  workflowChip_->setAlignment(Qt::AlignCenter);
  workflowChip_->setFixedWidth(142);
  workflowChip_->setFont(theme::mono(7.4, QFont::DemiBold));
  layout->addWidget(workflowChip_);
  layout->addSpacing(6);

  pathChip_ = new QLabel(QStringLiteral("NO MODEL"));
  pathChip_->setObjectName(QStringLiteral("pathChip"));
  pathChip_->setFont(theme::mono(8));
  pathChip_->setFixedWidth(300);
  pathChip_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
  layout->addWidget(pathChip_);
  layout->addStretch(1);

  openButton_ = new QPushButton(QStringLiteral("OPEN"));
  openButton_->setObjectName(QStringLiteral("ghost"));
  openButton_->setCursor(Qt::PointingHandCursor);
  openButton_->setFixedWidth(78);
  layout->addWidget(openButton_);

  runButton_ = new QPushButton(QStringLiteral("RUN ANALYSIS"));
  runButton_->setObjectName(QStringLiteral("primary"));
  runButton_->setCursor(Qt::PointingHandCursor);
  runButton_->setFixedWidth(128);
  runButton_->setEnabled(false);
  layout->addWidget(runButton_);

  statusPill_ = new QLabel(QStringLiteral("BOOT"));
  statusPill_->setAlignment(Qt::AlignCenter);
  statusPill_->setFixedWidth(116);
  layout->addWidget(statusPill_);
  setStatusPill(QStringLiteral("BOOT"), theme::c("#7C8CA8"));

  return header;
}

QWidget* MainWindow::buildSidebar() {
  auto* scroll = new QScrollArea;
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
  scroll->setFixedWidth(286);
  scroll->setStyleSheet(QStringLiteral("QScrollArea { background: transparent; border: none; }"));

  auto* panel = new QFrame;
  panel->setObjectName(QStringLiteral("panel"));
  auto* layout = new QVBoxLayout(panel);
  layout->setContentsMargins(16, 16, 16, 16);
  layout->setSpacing(9);

  layout->addWidget(sectionLabel(QStringLiteral("Workflow")));
  workflowSidebarValue_ = new QLabel(QStringLiteral("NO PROJECT"));
  workflowSidebarValue_->setFont(theme::display(15, QFont::Bold));
  workflowSidebarValue_->setStyleSheet(QStringLiteral("color:#67E8F9;"));
  layout->addWidget(workflowSidebarValue_);
  workflowSidebarHint_ = new QLabel(QStringLiteral("Choose a workflow to begin"));
  workflowSidebarHint_->setObjectName(QStringLiteral("muted"));
  workflowSidebarHint_->setWordWrap(true);
  workflowSidebarHint_->setFont(theme::mono(7.2));
  layout->addWidget(workflowSidebarHint_);

  auto* binary = new QLabel(QStringLiteral("BINARY-FIRST  ·  ELF / static analysis"));
  binary->setStyleSheet(QStringLiteral("color:#40516A; font-size:7pt; padding-top:4px;"));
  binary->setWordWrap(true);
  layout->addWidget(binary);
  auto* importBinary = new QPushButton(QStringLiteral("IMPORT ELF"));
  importBinary->setObjectName(QStringLiteral("ghost"));
  importBinary->setCursor(Qt::PointingHandCursor);
  importBinary->setFixedWidth(92);
  layout->addWidget(importBinary);
  connect(importBinary, &QPushButton::clicked, this, &MainWindow::importBinary);

  auto* newWorkspace = new QPushButton(QStringLiteral("NEW WORKSPACE"));
  newWorkspace->setObjectName(QStringLiteral("ghost"));
  newWorkspace->setCursor(Qt::PointingHandCursor);
  layout->addWidget(newWorkspace);
  connect(newWorkspace, &QPushButton::clicked, this, &MainWindow::showWorkflowHub);

  layout->addWidget(divider());
  layout->addWidget(sectionLabel(QStringLiteral("Model source")));
  modelName_ = new QLabel(QStringLiteral("No model loaded"));
  modelName_->setObjectName(QStringLiteral("muted"));
  modelName_->setWordWrap(true);
  modelName_->setFont(theme::mono(8));
  layout->addWidget(modelName_);

  auto* open = new QPushButton(QStringLiteral("OPEN MODEL"));
  open->setObjectName(QStringLiteral("ghost"));
  open->setCursor(Qt::PointingHandCursor);
  layout->addWidget(open);
  connect(open, &QPushButton::clicked, this, &MainWindow::openFileDialog);

  auto* samples = new QLabel(QStringLiteral("SAMPLES"));
  samples->setStyleSheet(QStringLiteral("color:#52627A; font-size:7pt; letter-spacing:2px;"));
  layout->addWidget(samples);

  auto* examplesButton = new QPushButton(QStringLiteral("EXAMPLES"));
  examplesButton->setObjectName(QStringLiteral("chip"));
  examplesButton->setCursor(Qt::PointingHandCursor);
  auto* examplesMenu = new QMenu(examplesButton);
  const QVector<QPair<QString, QString>> examples = {
      {QStringLiteral("HARDWARE  /  HW"), QStringLiteral("example/motivating-examples/hw.json")},
      {QStringLiteral("S-BENCH  /  A"), QStringLiteral("example/s-bench/a.json")},
      {QStringLiteral("P-BENCH  /  INITIAL"), QStringLiteral("example/p-bench/initial.json")},
      {QStringLiteral("RTSL  /  DEMO"), QStringLiteral("example/introduction/demo.rtsl")},
      {QStringLiteral("RTSL  /  FILLED"), QStringLiteral("example/introduction/demo-filled.rtsl")},
  };
  for (const auto& example : examples) {
    const QString path = sourcePath(example.second);
    if (!QFileInfo::exists(path)) {
      continue;
    }
    QAction* action = examplesMenu->addAction(example.first);
    connect(action, &QAction::triggered, this, [this, path] { loadModel(path); });
  }
  examplesButton->setMenu(examplesMenu);
  layout->addWidget(examplesButton);

  layout->addSpacing(2);
  layout->addWidget(divider());
  layout->addWidget(sectionLabel(QStringLiteral("Artifacts")));
  artifactsLabel_ = new QLabel(QStringLiteral("no run yet"));
  artifactsLabel_->setObjectName(QStringLiteral("muted"));
  artifactsLabel_->setFont(theme::mono(7.2));
  artifactsLabel_->setWordWrap(true);
  layout->addWidget(artifactsLabel_);

  layout->addWidget(divider());
  layout->addWidget(sectionLabel(QStringLiteral("Run history")));
  runHistoryLabel_ = new QLabel(QStringLiteral("no runs in this session"));
  runHistoryLabel_->setObjectName(QStringLiteral("muted"));
  runHistoryLabel_->setFont(theme::mono(7.1));
  runHistoryLabel_->setWordWrap(true);
  layout->addWidget(runHistoryLabel_);

  layout->addWidget(divider());
  layout->addWidget(sectionLabel(QStringLiteral("Verdict")));
  verdictValue_ = new QLabel(QStringLiteral("NO DATA"));
  verdictValue_->setFont(theme::display(18, QFont::Bold));
  verdictValue_->setStyleSheet(QStringLiteral("color:#7C8CA8;"));
  layout->addWidget(verdictValue_);
  verdictSub_ = new QLabel(QStringLiteral("run an analysis to populate"));
  verdictSub_->setObjectName(QStringLiteral("muted"));
  verdictSub_->setWordWrap(true);
  verdictSub_->setFont(theme::mono(7.5));
  layout->addWidget(verdictSub_);

  auto* grid = new QGridLayout;
  grid->setSpacing(7);
  grid->addWidget(statBlock(QStringLiteral("States"), &statesValue_, theme::c("#22D3EE")), 0, 0);
  grid->addWidget(statBlock(QStringLiteral("Hyperperiod"), &hyperValue_, theme::c("#8B5CF6")), 0, 1);
  grid->addWidget(statBlock(QStringLiteral("Max WCRT"), &wcrtValue_, theme::c("#F472B6")), 1, 0);
  grid->addWidget(statBlock(QStringLiteral("Cores"), &coresValue_, theme::c("#FBBF24")), 1, 1);
  layout->addLayout(grid);

  layout->addStretch();
  cliLabel_ = new QLabel(QStringLiteral("ENGINE  —"));
  cliLabel_->setObjectName(QStringLiteral("faint"));
  cliLabel_->setWordWrap(true);
  cliLabel_->setFont(theme::mono(6.5));
  layout->addWidget(cliLabel_);
  auto* footer = new QLabel(QStringLiteral("PTPN  ·  TEXT-FIRST WORKBENCH"));
  footer->setStyleSheet(QStringLiteral("color:#334155; font-size:7pt; letter-spacing:1px;"));
  layout->addWidget(footer);

  scroll->setWidget(panel);
  return scroll;
}

QWidget* MainWindow::buildInspector() {
  auto* panel = new QFrame;
  panel->setObjectName(QStringLiteral("panel"));
  panel->setFixedWidth(330);
  auto* layout = new QVBoxLayout(panel);
  layout->setContentsMargins(15, 16, 15, 14);
  layout->setSpacing(8);

  layout->addWidget(sectionLabel(QStringLiteral("Assumptions & evidence")));
  assumptionsView_ = new QPlainTextEdit;
  assumptionsView_->setObjectName(QStringLiteral("assumptionText"));
  assumptionsView_->setReadOnly(true);
  assumptionsView_->setFrameShape(QFrame::NoFrame);
  assumptionsView_->setFont(theme::mono(7.3));
  assumptionsView_->setLineWrapMode(QPlainTextEdit::WidgetWidth);
  assumptionsView_->setMinimumHeight(205);
  assumptionsView_->setMaximumHeight(300);
  assumptionsView_->setPlainText(QStringLiteral("Run an analysis to populate the assumptions."));
  layout->addWidget(assumptionsView_);

  layout->addWidget(divider());
  layout->addWidget(sectionLabel(QStringLiteral("Details")));
  detailTitle_ = new QLabel(QStringLiteral("NOTHING SELECTED"));
  detailTitle_->setFont(theme::display(11, QFont::Bold));
  detailTitle_->setStyleSheet(QStringLiteral("color:#E0F2FE;"));
  layout->addWidget(detailTitle_);
  detailBody_ = new QLabel(
      QStringLiteral("The workbench is text-first. Use PREVIEW on PTPN or STATE SPACE to inspect "
                     "a graph, then select a node for details."));
  detailBody_->setObjectName(QStringLiteral("muted"));
  detailBody_->setWordWrap(true);
  detailBody_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  detailBody_->setFont(theme::mono(7.8));
  detailBody_->setMinimumHeight(105);
  detailBody_->setAlignment(Qt::AlignTop | Qt::AlignLeft);
  layout->addWidget(detailBody_);

  layout->addWidget(divider());
  layout->addWidget(sectionLabel(QStringLiteral("Run log")));
  logView_ = new QPlainTextEdit;
  logView_->setObjectName(QStringLiteral("logView"));
  logView_->setReadOnly(true);
  logView_->setMaximumBlockCount(1400);
  logView_->setFont(theme::mono(7.1));
  logView_->setPlaceholderText(QStringLiteral("Analyzer output will appear here…"));
  logView_->setMinimumHeight(120);
  layout->addWidget(logView_, 1);

  return panel;
}

QWidget* MainWindow::buildStagePage(const QString& title, const QString& subtitle, TextView** view,
                                    QLabel** summary, QPushButton** previewButton,
                                    bool previewable) {
  auto* page = new QWidget;
  auto* layout = new QVBoxLayout(page);
  layout->setContentsMargins(14, 12, 14, 14);
  layout->setSpacing(6);

  auto* header = new QHBoxLayout;
  header->setSpacing(9);
  auto* titleLabel = new QLabel(title);
  titleLabel->setFont(theme::display(15, QFont::Bold));
  titleLabel->setStyleSheet(QStringLiteral("color:#E0F2FE;"));
  header->addWidget(titleLabel);

  auto* summaryLabel = new QLabel(QStringLiteral("—"));
  summaryLabel->setFont(theme::mono(7.2));
  summaryLabel->setStyleSheet(QStringLiteral("color:#52627A;"));
  header->addWidget(summaryLabel);
  header->addStretch();

  auto* search = new QLineEdit;
  search->setObjectName(QStringLiteral("findBox"));
  search->setPlaceholderText(QStringLiteral("FIND"));
  search->setClearButtonEnabled(true);
  search->setFixedWidth(142);
  search->setFont(theme::mono(7.4));
  header->addWidget(search);

  auto* copy = new QPushButton(QStringLiteral("COPY"));
  copy->setObjectName(QStringLiteral("ghost"));
  copy->setCursor(Qt::PointingHandCursor);
  copy->setFixedWidth(56);
  header->addWidget(copy);

  auto* preview = new QPushButton(QStringLiteral("PREVIEW"));
  preview->setObjectName(QStringLiteral("previewButton"));
  preview->setCursor(Qt::PointingHandCursor);
  preview->setFixedWidth(82);
  preview->setVisible(previewable);
  header->addWidget(preview);
  layout->addLayout(header);

  auto* subtitleLabel = new QLabel(subtitle);
  subtitleLabel->setObjectName(QStringLiteral("faint"));
  subtitleLabel->setFont(theme::mono(7.1));
  layout->addWidget(subtitleLabel);

  auto* line = new QFrame;
  line->setFrameShape(QFrame::HLine);
  line->setStyleSheet(QStringLiteral("color:#152238; background:#152238; max-height:1px;"));
  layout->addWidget(line);

  auto* text = new TextView;
  text->setPlaceholderText(QStringLiteral("Run an analysis to populate this stage."));
  layout->addWidget(text, 1);

  connect(search, &QLineEdit::returnPressed, this, [this, text, search] {
    const QString query = search->text();
    if (query.isEmpty()) {
      return;
    }
    if (!text->find(query)) {
      statusBar()->showMessage(QStringLiteral("No match in this stage"), 3500);
    }
  });
  connect(copy, &QPushButton::clicked, this, [this, text] {
    text->copyAll();
    statusBar()->showMessage(QStringLiteral("Stage text copied"), 2500);
  });

  *view = text;
  *summary = summaryLabel;
  *previewButton = preview;
  return page;
}

QWidget* MainWindow::buildDagTab() {
  QPushButton* unused = nullptr;
  return buildStagePage(
      QStringLiteral("TASK GRAPH"),
      QStringLiteral("Text-first DAG / task view  ·  explicit timing provenance  ·  no graph by default"),
      &dagView_, &dagSummary_, &unused, false);
}

QWidget* MainWindow::buildPtpnTab() {
  QWidget* page = buildStagePage(
      QStringLiteral("PTPN STRUCTURE"),
      QStringLiteral("Places, transitions and arcs as dense text  ·  graph only on demand"),
      &ptpnView_, &ptpnSummary_, &ptpnPreview_, true);
  return page;
}

QWidget* MainWindow::buildStateTab() {
  return buildStagePage(
      QStringLiteral("STATE-CLASS SPACE"),
      QStringLiteral("Reachability classes and transitions as text  ·  inspect the graph on demand"),
      &stateView_, &stateSummary_, &statePreview_, true);
}

QWidget* MainWindow::buildTimelineTab() {
  QPushButton* unused = nullptr;
  return buildStagePage(
      QStringLiteral("TIMELINE / ENVELOPES"),
      QStringLiteral("Text response envelopes  ·  not a concrete execution schedule"),
      &timelineView_, &timelineSummary_, &unused, false);
}

QWidget* MainWindow::buildReportTab() {
  QPushButton* unused = nullptr;
  return buildStagePage(
      QStringLiteral("REPORT"),
      QStringLiteral("Copyable analysis report  ·  assumptions and evidence included"),
      &reportView_, &reportSummary_, &unused, false);
}

void MainWindow::wire() {
  connect(openButton_, &QPushButton::clicked, this, &MainWindow::openFileDialog);
  connect(runButton_, &QPushButton::clicked, this, &MainWindow::runAnalysis);
  connect(ptpnPreview_, &QPushButton::clicked, this, &MainWindow::openPetriPreview);
  connect(statePreview_, &QPushButton::clicked, this, &MainWindow::openStatePreview);
  if (ptpnPreview_ != nullptr) {
    ptpnPreview_->setEnabled(false);
  }
  if (statePreview_ != nullptr) {
    statePreview_->setEnabled(false);
  }

  connect(service_, &AnalysisService::started, this, [this](const QString&) {
    setBusy(true);
    setStatusPill(QStringLiteral("ANALYZING"), theme::c("#FBBF24"));
    statusBar()->showMessage(QStringLiteral("ANALYZING  ·  exploring state classes…"));
  });
  connect(service_, &AnalysisService::finished, this, &MainWindow::handleResult);
}

void MainWindow::showWorkflowHub() {
  WorkflowDialog dialog(this);
  if (dialog.exec() != QDialog::Accepted) {
    return;
  }
  switch (dialog.choice()) {
    case WorkflowDialog::Choice::Specification:
      openFileDialog();
      break;
    case WorkflowDialog::Choice::Binary:
      importBinary();
      break;
    case WorkflowDialog::Choice::Example:
      loadModel(sourcePath(QStringLiteral("example/motivating-examples/hw.json")));
      break;
    case WorkflowDialog::Choice::None:
      break;
  }
}

void MainWindow::importBinary() {
  const QString start = currentFile_.isEmpty()
                            ? QDir::homePath()
                            : QFileInfo(currentFile_).absolutePath();
  const QString elf = QFileDialog::getOpenFileName(
      this, QStringLiteral("Import an ELF artifact"), start,
      QStringLiteral("ELF binaries (*.elf *.out *.axf);;All files (*.*)"));
  if (!elf.isEmpty()) {
    importBinaryPath(elf, true);
  }
}

bool MainWindow::importBinaryPath(const QString& elfPath, bool interactive) {
  const QFileInfo elfInfo(elfPath);
  if (!elfInfo.exists() || !elfInfo.isFile()) {
    const QString message = QStringLiteral("ELF file does not exist: ") + elfPath;
    if (interactive) {
      QMessageBox::warning(this, QStringLiteral("Binary-first import"), message);
    } else {
      statusBar()->showMessage(message, 7000);
    }
    return false;
  }

  const QString dag = findBinaryDag(elfInfo.absoluteFilePath());
  if (dag.isEmpty()) {
    const QString message =
        QStringLiteral("No DAG sidecar found for %1.\n\nExpected:\n  %1.dag.json\n\n"
                       "Binary-first expects an external static analyzer to emit the TDG JSON; "
                       "the ELF → DAG analyzer is not bundled with the desktop app yet.")
            .arg(QDir::toNativeSeparators(elfInfo.absoluteFilePath()));
    if (interactive) {
      QMessageBox::information(this, QStringLiteral("Binary-first import"), message);
    } else {
      statusBar()->showMessage(QStringLiteral("No DAG sidecar for this ELF"), 7000);
    }
    return false;
  }

  binaryElf_ = elfInfo.absoluteFilePath();
  loadModel(dag, QStringLiteral("BINARY · ELF"));
  return true;
}

QString MainWindow::findBinaryDag(const QString& elfPath) {
  const QFileInfo info(elfPath);
  const QStringList candidates = {
      elfPath + QStringLiteral(".dag.json"),
      elfPath + QStringLiteral(".tdg.json"),
      info.absolutePath() + QLatin1Char('/') + info.completeBaseName() + QStringLiteral(".dag.json"),
      info.absolutePath() + QLatin1Char('/') + info.completeBaseName() + QStringLiteral(".tdg.json"),
      info.absolutePath() + QLatin1Char('/') + info.completeBaseName() + QStringLiteral(".json"),
  };
  for (const QString& candidate : candidates) {
    const QFileInfo dag(candidate);
    if (dag.exists() && dag.isFile()) {
      return dag.absoluteFilePath();
    }
  }
  return {};
}

void MainWindow::openFileDialog() {
  const QString path = QFileDialog::getOpenFileName(
      this, QStringLiteral("Open a PTPN model"), sourcePath(QStringLiteral("example")),
      QStringLiteral("PTPN models (*.rtsl *.json *.ptpn);;All files (*.*)"));
  if (!path.isEmpty()) {
    loadModel(path);
  }
}

void MainWindow::loadModel(const QString& path, const QString& workflowLabel) {
  const QFileInfo info(path);
  if (!info.exists() || !info.isFile()) {
    statusBar()->showMessage(QStringLiteral("Cannot open %1").arg(path), 6000);
    return;
  }
  workflowOverride_ = workflowLabel;
  if (workflowLabel.isEmpty()) {
    binaryElf_.clear();
  }
  if (info.absoluteFilePath() != currentFile_) {
    runHistory_.clear();
    if (runHistoryLabel_ != nullptr) {
      runHistoryLabel_->setText(QStringLiteral("no runs in this session"));
    }
  }
  currentFile_ = info.absoluteFilePath();
  const QString nativePath = QDir::toNativeSeparators(currentFile_);
  pathChip_->setText(pathChip_->fontMetrics().elidedText(nativePath, Qt::ElideLeft, 272));
  pathChip_->setToolTip(nativePath);
  if (!binaryElf_.isEmpty()) {
    modelName_->setText(QFileInfo(binaryElf_).fileName() + QStringLiteral("\nDAG: ") +
                        info.fileName());
  } else {
    modelName_->setText(info.fileName() + QStringLiteral("\n") + info.absolutePath());
  }
  const QString workflow =
      workflowLabel.isEmpty() ? workflowShortName(currentFile_) : workflowLabel;
  workflowChip_->setText(workflow);
  workflowSidebarValue_->setText(workflow);
  const QString suffix = info.suffix().toLower();
  if (!binaryElf_.isEmpty()) {
    workflowSidebarHint_->setText(QStringLiteral("ELF  →  DAG  →  PTPN  →  SCG"));
  } else if (suffix == QStringLiteral("rtsl")) {
    workflowSidebarHint_->setText(QStringLiteral("RTSL  →  TDG  →  PTPN  →  SCG"));
  } else if (suffix == QStringLiteral("json")) {
    workflowSidebarHint_->setText(QStringLiteral("TDG JSON  →  PTPN  →  SCG"));
  } else {
    workflowSidebarHint_->setText(QStringLiteral("PTPN  →  SCG  ·  structure input"));
  }
  setWindowTitle(QStringLiteral("PTPN  //  ") +
                 (binaryElf_.isEmpty() ? info.fileName() : QFileInfo(binaryElf_).fileName()));
  runButton_->setEnabled(true);
  runAnalysis();
}

void MainWindow::runAnalysis() {
  if (busy_) {
    return;
  }
  if (currentFile_.isEmpty()) {
    openFileDialog();
    return;
  }
  logView_->clear();
  logView_->appendPlainText(QStringLiteral("> ") + QDir::toNativeSeparators(currentFile_));
  tabs_->setCurrentIndex(0);
  service_->analyze(currentFile_);
}

void MainWindow::setBusy(bool busy) {
  busy_ = busy;
  runButton_->setEnabled(!busy && !currentFile_.isEmpty());
  openButton_->setEnabled(!busy);
  runButton_->setText(busy ? QStringLiteral("ANALYZING…") : QStringLiteral("RUN ANALYSIS"));
}

void MainWindow::handleResult(const AnalysisResult& result) {
  setBusy(false);
  result_ = result;
  result_.workflowLabel = workflowOverride_;
  result_.binaryArtifact = binaryElf_;
  docs_ = renderWorkbench(result_);
  applyDocs(docs_);

  RunRecord record;
  record.time = QTime::currentTime().toString(QStringLiteral("HH:mm"));
  if (!result.ok) {
    record.verdict = QStringLiteral("ERROR");
  } else if (docs_.incompleteAssumptions()) {
    record.verdict = QStringLiteral("CONDITIONAL");
  } else {
    record.verdict = jsonBool(result.metrics, "schedulable", false)
                         ? QStringLiteral("SCHEDULABLE")
                         : QStringLiteral("UNSCHEDULABLE");
  }
  record.detail = QStringLiteral("%1T/%2E · %3S")
                      .arg(docs_.taskCount)
                      .arg(docs_.dagEdgeCount)
                      .arg(result.scg.nodes.size());
  runHistory_.prepend(record);
  while (runHistory_.size() > 5) {
    runHistory_.removeLast();
  }
  updateRunHistory();

  logView_->setPlainText(result.logs.isEmpty()
                             ? QStringLiteral("(no output)\n")
                             : result.logs.join(QLatin1Char('\n')));
  logView_->verticalScrollBar()->setValue(logView_->verticalScrollBar()->maximum());
  elapsedLabel_->setText(QStringLiteral("%1 ms").arg(result.elapsedMs));

  if (!result.ok) {
    setStatusPill(QStringLiteral("ERROR"), theme::c("#FB7185"));
    verdictValue_->setText(QStringLiteral("NO RESULT"));
    verdictValue_->setStyleSheet(QStringLiteral("color:#FB7185;"));
    verdictSub_->setText(result.error.left(180));
    detailTitle_->setText(QStringLiteral("ANALYSIS FAILED"));
    detailBody_->setText(result.error.isEmpty() ? QStringLiteral("ptpn returned no result.")
                                                : result.error);
    statusBar()->showMessage(QStringLiteral("ANALYSIS FAILED  ·  read the text error and run log"),
                             8000);
    return;
  }

  updateSummary();
  tabs_->setCurrentIndex(0);
  const bool conditional = docs_.incompleteAssumptions();
  if (conditional) {
    verdictValue_->setText(QStringLiteral("CONDITIONAL"));
    verdictValue_->setStyleSheet(QStringLiteral("color:#FBBF24;"));
    verdictSub_->setText(QStringLiteral("core/priority unresolved  ·  parser defaults used"));
    setStatusPill(QStringLiteral("ASSUMPTIONS"), theme::c("#FBBF24"));
    detailTitle_->setText(QStringLiteral("MODEL READY  ·  CONDITIONAL"));
    detailBody_->setText(
        QStringLiteral("%1 places · %2 transitions\n%3 state classes · %4 edges\n\n"
                       "ASSUMPTIONS INCOMPLETE  ·  core %5/%6 known  ·  priority %7/%8 known.\n"
                       "The analyzer used parser defaults; this is not a certified binary-first "
                       "verdict. Text remains the primary workbench.")
            .arg(result.net.places.size())
            .arg(result.net.transitions.size())
            .arg(result.scg.nodes.size())
            .arg(result.scg.edges.size())
            .arg(docs_.knownCoreCount)
            .arg(docs_.taskCount)
            .arg(docs_.knownPriorityCount)
            .arg(docs_.taskCount));
    statusBar()->showMessage(
        QStringLiteral("ASSUMPTIONS INCOMPLETE  ·  core/priority defaults used  ·  not certified"),
        8000);
    return;
  }

  const bool schedulable = jsonBool(result.metrics, "schedulable", false);
  setStatusPill(schedulable ? QStringLiteral("SCHEDULABLE") : QStringLiteral("UNSCHEDULABLE"),
                schedulable ? theme::c("#34D399") : theme::c("#FB7185"));
  detailTitle_->setText(QStringLiteral("MODEL READY"));
  detailBody_->setText(
      QStringLiteral("%1 places · %2 transitions\n%3 state classes · %4 edges\n\n"
                     "Open PREVIEW on PTPN or STATE SPACE to inspect a graph. Text remains the "
                     "primary workbench.")
          .arg(result.net.places.size())
          .arg(result.net.transitions.size())
          .arg(result.scg.nodes.size())
          .arg(result.scg.edges.size()));
  statusBar()->showMessage(QStringLiteral("READY  ·  %1 state classes  ·  text view")
                               .arg(result.scg.nodes.size()),
                           5000);
}

void MainWindow::applyDocs(const WorkbenchDocs& docs) {
  if (dagView_ != nullptr) {
    dagView_->setStageText(docs.dag);
  }
  if (ptpnView_ != nullptr) {
    ptpnView_->setStageText(docs.ptpn);
  }
  if (stateView_ != nullptr) {
    stateView_->setStageText(docs.state);
  }
  if (timelineView_ != nullptr) {
    timelineView_->setStageText(docs.timeline);
  }
  if (reportView_ != nullptr) {
    reportView_->setStageText(docs.report);
  }
  if (assumptionsView_ != nullptr) {
    assumptionsView_->setPlainText(docs.assumptions);
  }
  dagSummary_->setText(docs.dagSummary);
  ptpnSummary_->setText(docs.ptpnSummary);
  stateSummary_->setText(docs.stateSummary);
  timelineSummary_->setText(docs.timelineSummary);
  reportSummary_->setText(docs.reportSummary);
  if (artifactsLabel_ != nullptr) {
    QString artifactText;
    if (!binaryElf_.isEmpty()) {
      artifactText += QStringLiteral("ELF   ") + QFileInfo(binaryElf_).fileName() + QLatin1Char('\n');
    }
    artifactText += QStringLiteral("DAG   %1T · %2E\nPTPN  %3P · %4T · %5A\nSCG   %6S · %7E\nRUN   %8 ms")
                        .arg(docs_.taskCount)
                        .arg(docs_.dagEdgeCount)
                        .arg(result_.net.places.size())
                        .arg(result_.net.transitions.size())
                        .arg(result_.net.arcs.size())
                        .arg(result_.scg.nodes.size())
                        .arg(result_.scg.edges.size())
                        .arg(result_.elapsedMs);
    artifactsLabel_->setText(artifactText);
  }
  const QString workflow =
      workflowOverride_.isEmpty() ? workflowShortName(result_.path) : workflowOverride_;
  workflowChip_->setText(workflow);
  workflowSidebarValue_->setText(workflow);

  if (ptpnPreview_ != nullptr) {
    ptpnPreview_->setEnabled(result_.ok && result_.net.valid);
  }
  if (statePreview_ != nullptr) {
    statePreview_->setEnabled(result_.ok && result_.scg.valid);
  }
}

void MainWindow::updateRunHistory() {
  if (runHistoryLabel_ == nullptr) {
    return;
  }
  if (runHistory_.isEmpty()) {
    runHistoryLabel_->setText(QStringLiteral("no runs in this session"));
    return;
  }
  QStringList lines;
  for (const RunRecord& record : runHistory_) {
    lines << QStringLiteral("%1  %2  %3").arg(record.time, record.verdict, record.detail);
  }
  runHistoryLabel_->setText(lines.join(QLatin1Char('\n')));
}

void MainWindow::updateSummary() {
  const QJsonObject& metrics = result_.metrics;
  if (!result_.ok) {
    verdictValue_->setText(QStringLiteral("NO DATA"));
    verdictValue_->setStyleSheet(QStringLiteral("color:#7C8CA8;"));
    verdictSub_->setText(QStringLiteral("analysis did not complete"));
    statesValue_->setText(QStringLiteral("—"));
    hyperValue_->setText(QStringLiteral("—"));
    wcrtValue_->setText(QStringLiteral("—"));
    coresValue_->setText(QStringLiteral("—"));
    return;
  }

  const bool schedulable = jsonBool(metrics, "schedulable", false);
  const bool truncated = jsonBool(metrics, "truncated", false);
  const bool exact = jsonBool(metrics, "exact", true);
  verdictValue_->setText(schedulable ? QStringLiteral("SCHEDULABLE")
                                     : QStringLiteral("UNSCHEDULABLE"));
  verdictValue_->setStyleSheet(schedulable ? QStringLiteral("color:#34D399;")
                                           : QStringLiteral("color:#FB7185;"));
  QString detail = jsonString(metrics, "schedulability",
                              schedulable ? QStringLiteral("schedulable")
                                          : QStringLiteral("infeasible"));
  detail += exact ? QStringLiteral("  ·  exact") : QStringLiteral("  ·  approximate");
  if (truncated) {
    detail += QStringLiteral("  ·  truncated");
  }
  verdictSub_->setText(detail);

  statesValue_->setText(formattedValue(metrics, "states", QString::number(result_.scg.nodes.size())));
  const double hyperperiod = jsonNumber(metrics, "hyperperiod", 0.0);
  hyperValue_->setText(hyperperiod > 0.0 ? QString::number(hyperperiod, 'f', 0)
                                         : QStringLiteral("—"));

  double maxWcrt = 0.0;
  const QJsonArray tasks = metrics.value(QStringLiteral("tasks")).toArray();
  for (const QJsonValue& value : tasks) {
    maxWcrt = std::max(maxWcrt, value.toObject().value(QStringLiteral("wcrt")).toDouble());
  }
  wcrtValue_->setText(tasks.isEmpty() ? QStringLiteral("—") : QString::number(maxWcrt, 'f', 0));
  coresValue_->setText(QString::number(metrics.value(QStringLiteral("cores")).toArray().size()));
}

void MainWindow::openPetriPreview() {
  if (!result_.net.valid) {
    return;
  }
  PreviewDialog dialog(PreviewDialog::Kind::Petri, this);
  dialog.setPetriNet(result_.net);
  connect(&dialog, &PreviewDialog::placeSelected, this, &MainWindow::showPlaceInfo);
  connect(&dialog, &PreviewDialog::transitionSelected, this, &MainWindow::showTransitionInfo);
  dialog.exec();
}

void MainWindow::openStatePreview() {
  if (!result_.scg.valid) {
    return;
  }
  PreviewDialog dialog(PreviewDialog::Kind::State, this);
  dialog.setStateGraph(result_.scg);
  connect(&dialog, &PreviewDialog::stateSelected, this, &MainWindow::showStateInfo);
  dialog.exec();
}

void MainWindow::showPlaceInfo(int index) {
  if (index < 0 || index >= result_.net.places.size()) {
    return;
  }
  const Place& place = result_.net.places.at(index);
  int incoming = 0;
  int outgoing = 0;
  for (const PetriArc& arc : result_.net.arcs) {
    if (arc.fromPlace && arc.from == index) {
      ++outgoing;
    }
    if (!arc.fromPlace && arc.to == index) {
      ++incoming;
    }
  }
  detailTitle_->setText(QStringLiteral("PLACE  /  ") + place.id);
  detailBody_->setText(
      QStringLiteral("tokens       %1\ncapacity     %2\ninitial      %3\nincoming     %4 arcs\n"
                     "outgoing     %5 arcs")
          .arg(place.tokens)
          .arg(place.capacity > 0 ? QString::number(place.capacity) : QStringLiteral("∞"))
          .arg(result_.net.initialMarking.value(index, place.tokens))
          .arg(incoming)
          .arg(outgoing));
}

void MainWindow::showTransitionInfo(int index) {
  if (index < 0 || index >= result_.net.transitions.size()) {
    return;
  }
  const Transition& transition = result_.net.transitions.at(index);
  QStringList inputs;
  QStringList outputs;
  for (const PetriArc& arc : result_.net.arcs) {
    if (arc.fromPlace && arc.to == index && arc.from < result_.net.places.size()) {
      inputs.push_back(result_.net.places.at(arc.from).id);
    }
    if (!arc.fromPlace && arc.from == index && arc.to < result_.net.places.size()) {
      outputs.push_back(result_.net.places.at(arc.to).id);
    }
  }
  const QString window = transition.hasInterval
                             ? QStringLiteral("[%1, %2]")
                                   .arg(QString::number(transition.intervalMin, 'f', 0),
                                        QString::number(transition.intervalMax, 'f', 0))
                             : QStringLiteral("untimed");
  detailTitle_->setText(QStringLiteral("TRANSITION  /  ") + transition.id);
  detailBody_->setText(
      QStringLiteral("core         %1\npriority     %2\nwindow       %3\nsuspendable  %4\n\n"
                     "inputs       %5\noutputs      %6")
          .arg(transition.core)
          .arg(transition.priority)
          .arg(window)
          .arg(transition.suspendable ? QStringLiteral("yes") : QStringLiteral("no"))
          .arg(inputs.isEmpty() ? QStringLiteral("—") : inputs.join(QStringLiteral(", ")))
          .arg(outputs.isEmpty() ? QStringLiteral("—") : outputs.join(QStringLiteral(", "))));
}

void MainWindow::showStateInfo(int index) {
  if (index < 0 || index >= result_.scg.nodes.size()) {
    return;
  }
  const StateNode& state = result_.scg.nodes.at(index);
  detailTitle_->setText(QStringLiteral("STATE CLASS  /  S%1").arg(state.index));
  detailBody_->setText(
      QStringLiteral("marking      %1\n\nactive       %2\n\nsuspended    %3\n\nelapsed      %4")
          .arg(state.marking.isEmpty() ? QStringLiteral("—") : state.marking,
               state.active.isEmpty() ? QStringLiteral("—") : state.active,
               state.suspended.isEmpty() ? QStringLiteral("—") : state.suspended,
               state.elapsed.isEmpty() ? QStringLiteral("—") : state.elapsed));
}

void MainWindow::setStatusPill(const QString& text, const QColor& color) {
  statusPill_->setText(text);
  statusPill_->setStyleSheet(
      QStringLiteral(
          "QLabel { color:%1; background:rgba(%2,%3,%4,0.12); border:1px solid "
          "rgba(%2,%3,%4,0.55); border-radius:11px; padding:5px 12px; font-weight:700; "
          "letter-spacing:1px; font-size:8pt; }")
          .arg(color.name())
          .arg(color.red())
          .arg(color.green())
          .arg(color.blue()));
}

QString MainWindow::workflowShortName(const QString& path) {
  const QString suffix = QFileInfo(path).suffix().toLower();
  if (suffix == QStringLiteral("rtsl")) {
    return QStringLiteral("SPEC · RTSL");
  }
  if (suffix == QStringLiteral("json")) {
    return QStringLiteral("SPEC · TDG");
  }
  if (suffix == QStringLiteral("ptpn")) {
    return QStringLiteral("MANUAL · PTPN");
  }
  return QStringLiteral("SPEC-FIRST");
}

QString MainWindow::resolveCliPath() const {
  QStringList candidates;
  const QString environment = qEnvironmentVariable("PTPN_BIN");
  if (!environment.isEmpty()) {
    candidates.push_back(environment);
  }
#ifdef PTPN_CLI_PATH
  candidates.push_back(QString::fromUtf8(PTPN_CLI_PATH));
#endif
  const QDir applicationDir(QCoreApplication::applicationDirPath());
  candidates.push_back(applicationDir.filePath(QStringLiteral("ptpn.exe")));
  candidates.push_back(applicationDir.filePath(QStringLiteral("../ptpn.exe")));
  candidates.push_back(applicationDir.filePath(QStringLiteral("../../ptpn.exe")));
#ifdef PTPN_SOURCE_DIR
  const QDir sourceDir(QString::fromUtf8(PTPN_SOURCE_DIR));
  candidates.push_back(sourceDir.filePath(QStringLiteral("cmake-build-debug-mingw/ptpn.exe")));
  candidates.push_back(sourceDir.filePath(QStringLiteral("cmake-build-debug/ptpn.exe")));
  candidates.push_back(sourceDir.filePath(QStringLiteral("build/ptpn.exe")));
#endif
  for (const QString& candidate : candidates) {
    const QFileInfo info(candidate);
    if (info.exists() && info.isFile()) {
      return info.absoluteFilePath();
    }
  }
  const QString path = qEnvironmentVariable("PATH");
  for (const QString& entry : path.split(QLatin1Char(';'))) {
    if (entry.isEmpty()) {
      continue;
    }
    const QFileInfo info(QDir(entry).filePath(QStringLiteral("ptpn.exe")));
    if (info.exists() && info.isFile()) {
      return info.absoluteFilePath();
    }
  }
  return {};
}

QString MainWindow::sourcePath(const QString& relative) {
#ifdef PTPN_SOURCE_DIR
  return QDir(QString::fromUtf8(PTPN_SOURCE_DIR)).filePath(relative);
#else
  return QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("../") + relative);
#endif
}

void MainWindow::showEvent(QShowEvent* event) {
  QMainWindow::showEvent(event);
#if defined(Q_OS_WIN)
  if (!darkTitleApplied_) {
    const HWND handle = reinterpret_cast<HWND>(winId());
    BOOL enabled = TRUE;
    if (SUCCEEDED(DwmSetWindowAttribute(handle, 20, &enabled, sizeof(enabled)))) {
      darkTitleApplied_ = true;
    }
  }
#endif
}

void MainWindow::dragEnterEvent(QDragEnterEvent* event) {
  if (!event->mimeData()->hasUrls()) {
    return;
  }
  for (const QUrl& url : event->mimeData()->urls()) {
    if (url.isLocalFile()) {
      const QString suffix = QFileInfo(url.toLocalFile()).suffix().toLower();
      if (suffix == QStringLiteral("rtsl") || suffix == QStringLiteral("json") ||
          suffix == QStringLiteral("ptpn")) {
        event->acceptProposedAction();
        return;
      }
    }
  }
}

void MainWindow::dropEvent(QDropEvent* event) {
  for (const QUrl& url : event->mimeData()->urls()) {
    if (!url.isLocalFile()) {
      continue;
    }
    const QString path = url.toLocalFile();
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix == QStringLiteral("rtsl") || suffix == QStringLiteral("json") ||
        suffix == QStringLiteral("ptpn")) {
      loadModel(path);
      event->acceptProposedAction();
      return;
    }
  }
}
