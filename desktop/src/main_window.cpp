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
#include <QLinearGradient>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QPair>
#include <QRadialGradient>
#include <QScrollArea>
#include <QScrollBar>
#include <QShowEvent>
#include <QSizePolicy>
#include <QStatusBar>
#include <QStringList>
#include <QTabWidget>
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
#include "metrics_panel.h"
#include "petri_view.h"
#include "state_graph_view.h"
#include "theme.h"

namespace {

constexpr double kPi = 3.14159265358979323846;

class CompactTabWidget final : public QTabWidget {
 public:
  explicit CompactTabWidget(QWidget* parent = nullptr) : QTabWidget(parent) {}
  QSize minimumSizeHint() const override { return QSize(220, 180); }
  QSize sizeHint() const override { return QSize(760, 520); }
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
  frame->setFixedHeight(62);
  auto* layout = new QVBoxLayout(frame);
  layout->setContentsMargins(10, 7, 10, 7);
  layout->setSpacing(1);
  auto* label = new QLabel(title.toUpper());
  label->setStyleSheet(QStringLiteral("color:#64748B; font-size:8pt; font-weight:600;"));
  auto* number = new QLabel(QStringLiteral("—"));
  number->setStyleSheet(QStringLiteral("color:%1; font-size:14pt; font-weight:700;")
                            .arg(accent.name()));
  layout->addWidget(label);
  layout->addWidget(number);
  *value = number;
  return frame;
}

QWidget* legendRow(const QColor& color, const QString& text) {
  auto* row = new QWidget;
  auto* layout = new QHBoxLayout(row);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(8);
  auto* dot = new QLabel;
  dot->setFixedSize(8, 8);
  dot->setStyleSheet(QStringLiteral("background:%1; border-radius:4px;").arg(color.name()));
  auto* label = new QLabel(text);
  label->setStyleSheet(QStringLiteral("color:#7C8CA8; font-size:8pt;"));
  layout->addWidget(dot);
  layout->addWidget(label);
  layout->addStretch();
  return row;
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
    const QString demo = sourcePath(QStringLiteral("example/motivating-examples/hw.json"));
    if (QFileInfo::exists(demo)) {
      loadModel(demo);
    }
  });
}

MainWindow::~MainWindow() = default;

void MainWindow::buildUi() {
  setWindowTitle(QStringLiteral("PTPN  //  Timed Petri Lab"));
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
  tabs_->setMinimumSize(220, 180);
  tabs_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
  tabs_->addTab(buildPetriTab(), QStringLiteral("PETRI NET"));
  tabs_->addTab(buildStateTab(), QStringLiteral("STATE SPACE"));
  tabs_->addTab(buildMetricsTab(), QStringLiteral("TELEMETRY"));
  bodyLayout->addWidget(tabs_, 1);
  bodyLayout->addWidget(buildInspector());
  root->addWidget(body, 1);
  setCentralWidget(central);

  statusBar()->setSizeGripEnabled(true);
  statusBar()->showMessage(QStringLiteral("READY  ·  waiting for a model"));
  elapsedLabel_ = new QLabel(QStringLiteral("—"));
  elapsedLabel_->setStyleSheet(QStringLiteral("color:#64748B; font-family:'Cascadia Mono';"));
  statusBar()->addPermanentWidget(elapsedLabel_);
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
  auto* subtitle = new QLabel(QStringLiteral("TIMED PETRI NET LAB"));
  subtitle->setObjectName(QStringLiteral("brandSub"));
  subtitle->setFont(theme::mono(7, QFont::DemiBold));
  titleColumn->addWidget(title);
  titleColumn->addWidget(subtitle);
  layout->addLayout(titleColumn);
  layout->addSpacing(18);

  pathChip_ = new QLabel(QStringLiteral("NO MODEL"));
  pathChip_->setObjectName(QStringLiteral("pathChip"));
  pathChip_->setFont(theme::mono(8));
  pathChip_->setFixedWidth(330);
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
  layout->addWidget(runButton_);

  statusPill_ = new QLabel(QStringLiteral("BOOT"));
  statusPill_->setAlignment(Qt::AlignCenter);
  statusPill_->setFixedWidth(116);
  layout->addWidget(statusPill_);
  setStatusPill(QStringLiteral("BOOT"), theme::c("#7C8CA8"));

  return header;
}

QWidget* MainWindow::buildSidebar() {
  auto* panel = new QFrame;
  panel->setObjectName(QStringLiteral("panel"));
  panel->setFixedWidth(276);
  auto* layout = new QVBoxLayout(panel);
  layout->setContentsMargins(16, 16, 16, 16);
  layout->setSpacing(10);

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

  layout->addSpacing(4);
  auto* samples = new QLabel(QStringLiteral("SAMPLES"));
  samples->setStyleSheet(QStringLiteral("color:#52627A; font-size:7pt; letter-spacing:2px;"));
  layout->addWidget(samples);

  const QVector<QPair<QString, QString>> examples = {
      {QStringLiteral("HARDWARE  /  HW"), QStringLiteral("example/motivating-examples/hw.json")},
      {QStringLiteral("S-BENCH  /  A"), QStringLiteral("example/s-bench/a.json")},
      {QStringLiteral("P-BENCH  /  INITIAL"), QStringLiteral("example/p-bench/initial.json")},
      {QStringLiteral("RTSL  /  DEMO"), QStringLiteral("example/introduction/demo.rtsl")},
  };
  for (const auto& example : examples) {
    const QString path = sourcePath(example.second);
    if (!QFileInfo::exists(path)) {
      continue;
    }
    auto* chip = new QPushButton(example.first);
    chip->setObjectName(QStringLiteral("chip"));
    chip->setCursor(Qt::PointingHandCursor);
    layout->addWidget(chip);
    connect(chip, &QPushButton::clicked, this, [this, path] { loadModel(path); });
  }

  layout->addSpacing(2);
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

  layout->addWidget(divider());
  layout->addWidget(sectionLabel(QStringLiteral("Legend")));
  layout->addWidget(legendRow(theme::c("#22D3EE"), QStringLiteral("place / token")));
  layout->addWidget(legendRow(theme::c("#F59E0B"), QStringLiteral("timed transition")));
  layout->addWidget(legendRow(theme::c("#F472B6"), QStringLiteral("enabled / firing")));
  layout->addWidget(legendRow(theme::c("#8B5CF6"), QStringLiteral("state class")));

  layout->addStretch();
  cliLabel_ = new QLabel(QStringLiteral("ENGINE  —"));
  cliLabel_->setObjectName(QStringLiteral("faint"));
  cliLabel_->setWordWrap(true);
  cliLabel_->setFont(theme::mono(6.5));
  layout->addWidget(cliLabel_);
  auto* footer = new QLabel(QStringLiteral("PTPN  ·  DBM STATE-CLASS ENGINE"));
  footer->setStyleSheet(QStringLiteral("color:#334155; font-size:7pt; letter-spacing:1px;"));
  layout->addWidget(footer);

  return panel;
}

QWidget* MainWindow::buildInspector() {
  auto* panel = new QFrame;
  panel->setObjectName(QStringLiteral("panel"));
  panel->setFixedWidth(312);
  auto* layout = new QVBoxLayout(panel);
  layout->setContentsMargins(16, 16, 16, 16);
  layout->setSpacing(9);

  layout->addWidget(sectionLabel(QStringLiteral("Inspector")));
  inspectorTitle_ = new QLabel(QStringLiteral("Nothing selected"));
  inspectorTitle_->setFont(theme::display(13, QFont::Bold));
  inspectorTitle_->setStyleSheet(QStringLiteral("color:#E0F2FE;"));
  layout->addWidget(inspectorTitle_);

  inspectorBody_ = new QLabel(
      QStringLiteral("Click a place, transition or state class to inspect it."));
  inspectorBody_->setObjectName(QStringLiteral("muted"));
  inspectorBody_->setWordWrap(true);
  inspectorBody_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  inspectorBody_->setFont(theme::mono(8));
  inspectorBody_->setMinimumHeight(168);
  inspectorBody_->setAlignment(Qt::AlignTop | Qt::AlignLeft);
  layout->addWidget(inspectorBody_);

  layout->addWidget(divider());
  layout->addWidget(sectionLabel(QStringLiteral("Run log")));
  logView_ = new QPlainTextEdit;
  logView_->setReadOnly(true);
  logView_->setMaximumBlockCount(1200);
  logView_->setFont(theme::mono(7.5));
  logView_->setPlaceholderText(QStringLiteral("Analyzer output will appear here…"));
  logView_->setMinimumHeight(210);
  layout->addWidget(logView_, 1);

  return panel;
}

QWidget* MainWindow::buildPetriTab() {
  auto* page = new QWidget;
  auto* layout = new QVBoxLayout(page);
  layout->setContentsMargins(10, 9, 10, 10);
  layout->setSpacing(8);

  auto* toolbar = new QHBoxLayout;
  toolbar->setSpacing(7);
  toolbar->addWidget(sectionLabel(QStringLiteral("Petri net")));
  auto* hint = new QLabel(QStringLiteral("drag  ·  wheel  ·  click an enabled transition"));
  hint->setObjectName(QStringLiteral("faint"));
  hint->setFont(theme::mono(7));
  toolbar->addWidget(hint);
  toolbar->addStretch();

  auto* fit = new QPushButton(QStringLiteral("FIT"));
  fit->setObjectName(QStringLiteral("ghost"));
  auto* zoomOut = new QPushButton(QStringLiteral("−"));
  zoomOut->setObjectName(QStringLiteral("ghost"));
  auto* zoomIn = new QPushButton(QStringLiteral("+"));
  zoomIn->setObjectName(QStringLiteral("ghost"));
  auto* reset = new QPushButton(QStringLiteral("RESET"));
  reset->setObjectName(QStringLiteral("ghost"));
  autoButton_ = new QPushButton(QStringLiteral("AUTO"));
  autoButton_->setObjectName(QStringLiteral("ghost"));
  autoButton_->setCheckable(true);
  speedButton_ = new QPushButton(QStringLiteral("1.0×"));
  speedButton_->setObjectName(QStringLiteral("ghost"));
  for (QPushButton* button : {fit, zoomOut, zoomIn, reset, autoButton_, speedButton_}) {
    button->setCursor(Qt::PointingHandCursor);
    toolbar->addWidget(button);
  }
  layout->addLayout(toolbar);

  auto* frame = new QFrame;
  frame->setObjectName(QStringLiteral("softPanel"));
  auto* frameLayout = new QVBoxLayout(frame);
  frameLayout->setContentsMargins(1, 1, 1, 1);
  petriView_ = new PetriView;
  petriView_->setMinimumSize(240, 180);
  frameLayout->addWidget(petriView_);
  layout->addWidget(frame, 1);

  connect(fit, &QPushButton::clicked, petriView_, &PetriView::fitView);
  connect(zoomOut, &QPushButton::clicked, this, [this] { petriView_->zoomBy(0.85); });
  connect(zoomIn, &QPushButton::clicked, this, [this] { petriView_->zoomBy(1.18); });
  connect(reset, &QPushButton::clicked, petriView_, &PetriView::resetSimulation);
  connect(autoButton_, &QPushButton::toggled, this, [this](bool enabled) {
    petriView_->setAutoFire(enabled);
    autoButton_->setText(enabled ? QStringLiteral("AUTO ON") : QStringLiteral("AUTO"));
  });
  connect(speedButton_, &QPushButton::clicked, this, [this] {
    static const double speeds[] = {0.5, 1.0, 2.0, 4.0};
    int index = 0;
    for (int i = 0; i < 4; ++i) {
      if (std::abs(speeds[i] - speed_) < 0.01) {
        index = i;
        break;
      }
    }
    speed_ = speeds[(index + 1) % 4];
    petriView_->setSpeed(speed_);
    speedButton_->setText(QString::number(speed_, 'f', 1) + QStringLiteral("×"));
  });

  return page;
}

QWidget* MainWindow::buildStateTab() {
  auto* page = new QWidget;
  auto* layout = new QVBoxLayout(page);
  layout->setContentsMargins(10, 9, 10, 10);
  layout->setSpacing(8);

  auto* toolbar = new QHBoxLayout;
  toolbar->setSpacing(7);
  toolbar->addWidget(sectionLabel(QStringLiteral("State-class space")));
  auto* hint = new QLabel(QStringLiteral("reachability graph  ·  DBM clock zones"));
  hint->setObjectName(QStringLiteral("faint"));
  hint->setFont(theme::mono(7));
  toolbar->addWidget(hint);
  toolbar->addStretch();
  auto* fit = new QPushButton(QStringLiteral("FIT"));
  fit->setObjectName(QStringLiteral("ghost"));
  auto* zoomOut = new QPushButton(QStringLiteral("−"));
  zoomOut->setObjectName(QStringLiteral("ghost"));
  auto* zoomIn = new QPushButton(QStringLiteral("+"));
  zoomIn->setObjectName(QStringLiteral("ghost"));
  for (QPushButton* button : {fit, zoomOut, zoomIn}) {
    button->setCursor(Qt::PointingHandCursor);
    toolbar->addWidget(button);
  }
  layout->addLayout(toolbar);

  auto* frame = new QFrame;
  frame->setObjectName(QStringLiteral("softPanel"));
  auto* frameLayout = new QVBoxLayout(frame);
  frameLayout->setContentsMargins(1, 1, 1, 1);
  stateView_ = new StateGraphView;
  stateView_->setMinimumSize(240, 180);
  frameLayout->addWidget(stateView_);
  layout->addWidget(frame, 1);

  connect(fit, &QPushButton::clicked, stateView_, &StateGraphView::fitView);
  connect(zoomOut, &QPushButton::clicked, this, [this] { stateView_->zoomBy(0.85); });
  connect(zoomIn, &QPushButton::clicked, this, [this] { stateView_->zoomBy(1.18); });
  return page;
}

QWidget* MainWindow::buildMetricsTab() {
  auto* page = new QWidget;
  auto* layout = new QVBoxLayout(page);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);
  metricsPanel_ = new MetricsPanel;
  auto* scroll = new QScrollArea;
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  scroll->setMinimumSize(0, 0);
  scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  scroll->setWidget(metricsPanel_);
  layout->addWidget(scroll);
  return page;
}

void MainWindow::wire() {
  connect(openButton_, &QPushButton::clicked, this, &MainWindow::openFileDialog);
  connect(runButton_, &QPushButton::clicked, this, &MainWindow::runAnalysis);

  connect(service_, &AnalysisService::started, this, [this](const QString&) {
    setBusy(true);
    setStatusPill(QStringLiteral("ANALYZING"), theme::c("#FBBF24"));
    statusBar()->showMessage(QStringLiteral("ANALYZING  ·  exploring state classes…"));
  });
  connect(service_, &AnalysisService::finished, this, &MainWindow::handleResult);

  connect(petriView_, &PetriView::statusMessage, this,
          [this](const QString& message) { statusBar()->showMessage(message, 5000); });
  connect(petriView_, &PetriView::nodeSelected, this,
          [this](const QString& kind, int index) {
            if (kind == QStringLiteral("place")) {
              showPlaceInfo(index);
            } else {
              showTransitionInfo(index);
            }
          });
  connect(stateView_, &StateGraphView::nodeSelected, this, &MainWindow::showStateInfo);
  connect(stateView_, &StateGraphView::statusMessage, this,
          [this](const QString& message) { statusBar()->showMessage(message, 5000); });
}

void MainWindow::openFileDialog() {
  const QString path = QFileDialog::getOpenFileName(
      this, QStringLiteral("Open a PTPN model"), sourcePath(QStringLiteral("example")),
      QStringLiteral("PTPN models (*.rtsl *.json *.ptpn);;All files (*.*)"));
  if (!path.isEmpty()) {
    loadModel(path);
  }
}

void MainWindow::loadModel(const QString& path) {
  const QFileInfo info(path);
  if (!info.exists() || !info.isFile()) {
    statusBar()->showMessage(QStringLiteral("Cannot open %1").arg(path), 6000);
    return;
  }
  currentFile_ = info.absoluteFilePath();
  const QString nativePath = QDir::toNativeSeparators(currentFile_);
  pathChip_->setText(pathChip_->fontMetrics().elidedText(nativePath, Qt::ElideLeft, 302));
  pathChip_->setToolTip(nativePath);
  modelName_->setText(info.fileName() + QStringLiteral("\n") + info.absolutePath());
  setWindowTitle(QStringLiteral("PTPN  //  ") + info.fileName());
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
  qApp->setOverrideCursor(Qt::WaitCursor);
  logView_->clear();
  logView_->appendPlainText(QStringLiteral("> ptpn ") + QDir::toNativeSeparators(currentFile_));
  tabs_->setCurrentIndex(0);
  service_->analyze(currentFile_);
  qApp->restoreOverrideCursor();
}

void MainWindow::setBusy(bool busy) {
  busy_ = busy;
  runButton_->setEnabled(!busy);
  openButton_->setEnabled(!busy);
  runButton_->setText(busy ? QStringLiteral("ANALYZING…") : QStringLiteral("RUN ANALYSIS"));
}

void MainWindow::handleResult(const AnalysisResult& result) {
  setBusy(false);
  result_ = result;

  metricsPanel_->setResult(result_);
  petriView_->setNet(result.net);
  stateView_->setGraph(result.scg);

  logView_->setPlainText(result.logs.isEmpty()
                             ? QStringLiteral("(no output)\n")
                             : result.logs.join(QLatin1Char('\n')));
  logView_->verticalScrollBar()->setValue(logView_->verticalScrollBar()->maximum());
  elapsedLabel_->setText(QStringLiteral("%1 ms").arg(result.elapsedMs));

  if (!result.ok) {
    setStatusPill(QStringLiteral("ERROR"), theme::c("#FB7185"));
    verdictValue_->setText(QStringLiteral("NO RESULT"));
    verdictValue_->setStyleSheet(QStringLiteral("color:#FB7185;"));
    verdictSub_->setText(result.error.left(160));
    inspectorTitle_->setText(QStringLiteral("ANALYSIS FAILED"));
    inspectorBody_->setText(result.error.isEmpty() ? QStringLiteral("ptpn returned no result.")
                                                   : result.error);
    statusBar()->showMessage(QStringLiteral("ANALYSIS FAILED  ·  check the run log"), 8000);
    return;
  }

  updateSummary();
  tabs_->setCurrentIndex(0);
  const bool schedulable = jsonBool(result.metrics, "schedulable", false);
  setStatusPill(schedulable ? QStringLiteral("SCHEDULABLE") : QStringLiteral("UNSCHEDULABLE"),
                schedulable ? theme::c("#34D399") : theme::c("#FB7185"));
  inspectorTitle_->setText(QStringLiteral("MODEL READY"));
  inspectorBody_->setText(
      QStringLiteral("%1 places · %2 transitions\n%3 state classes · %4 edges\n\n"
                     "Click a place, transition or state class to inspect it.")
          .arg(result.net.places.size())
          .arg(result.net.transitions.size())
          .arg(result.scg.nodes.size())
          .arg(result.scg.edges.size()));
  statusBar()->showMessage(QStringLiteral("READY  ·  %1 places  ·  %2 state classes")
                               .arg(result.net.places.size())
                               .arg(result.scg.nodes.size()),
                           5000);
}

void MainWindow::updateSummary() {
  const QJsonObject& metrics = result_.metrics;
  const bool ok = result_.ok;
  const bool schedulable = jsonBool(metrics, "schedulable", false);
  const bool truncated = jsonBool(metrics, "truncated", false);
  const bool exact = jsonBool(metrics, "exact", true);

  if (!ok) {
    verdictValue_->setText(QStringLiteral("NO DATA"));
    verdictValue_->setStyleSheet(QStringLiteral("color:#7C8CA8;"));
    verdictSub_->setText(QStringLiteral("analysis did not complete"));
    return;
  }

  verdictValue_->setText(schedulable ? QStringLiteral("SCHEDULABLE") : QStringLiteral("UNSCHEDULABLE"));
  verdictValue_->setStyleSheet(schedulable ? QStringLiteral("color:#34D399;")
                                           : QStringLiteral("color:#FB7185;"));
  QString detail = jsonString(metrics, "schedulability",
                              schedulable ? QStringLiteral("schedulable") : QStringLiteral("infeasible"));
  detail += exact ? QStringLiteral("  ·  exact") : QStringLiteral("  ·  approximate");
  if (truncated) {
    detail += QStringLiteral("  ·  truncated");
  }
  verdictSub_->setText(detail);

  statesValue_->setText(formattedValue(metrics, "states", QString::number(result_.net.places.size())));
  const double hyperperiod = jsonNumber(metrics, "hyperperiod", 0.0);
  hyperValue_->setText(hyperperiod > 0.0 ? QString::number(hyperperiod, 'f', 0) : QStringLiteral("—"));

  double maxWcrt = 0.0;
  const QJsonArray tasks = metrics.value(QStringLiteral("tasks")).toArray();
  for (const QJsonValue& value : tasks) {
    maxWcrt = std::max(maxWcrt, value.toObject().value(QStringLiteral("wcrt")).toDouble());
  }
  wcrtValue_->setText(tasks.isEmpty() ? QStringLiteral("—") : QString::number(maxWcrt, 'f', 0));
  coresValue_->setText(QString::number(metrics.value(QStringLiteral("cores")).toArray().size()));
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
  inspectorTitle_->setText(QStringLiteral("PLACE  /  ") + place.id);
  inspectorBody_->setText(
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
  inspectorTitle_->setText(QStringLiteral("TRANSITION  /  ") + transition.id);
  inspectorBody_->setText(
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
  inspectorTitle_->setText(QStringLiteral("STATE CLASS  /  S%1").arg(state.index));
  inspectorBody_->setText(
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
