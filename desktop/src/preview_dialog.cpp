#include "preview_dialog.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

#include "petri_view.h"
#include "state_graph_view.h"
#include "theme.h"

namespace {

QLabel* eyebrow(const QString& text) {
  auto* label = new QLabel(text.toUpper());
  label->setStyleSheet(QStringLiteral("color:#7DD3FC; font-size:9pt; font-weight:700; "
                                      "letter-spacing:2px;"));
  return label;
}

QPushButton* action(const QString& text) {
  auto* button = new QPushButton(text);
  button->setObjectName(QStringLiteral("ghost"));
  button->setCursor(Qt::PointingHandCursor);
  return button;
}

}  // namespace

PreviewDialog::PreviewDialog(Kind kind, QWidget* parent) : QDialog(parent), kind_(kind) {
  setWindowTitle(kind == Kind::Petri ? QStringLiteral("PTPN structure preview")
                                     : QStringLiteral("State-class graph preview"));
  setModal(true);
  resize(1220, 790);
  setMinimumSize(860, 580);
  setStyleSheet(theme::globalStyleSheet());

  auto* root = new QVBoxLayout(this);
  root->setContentsMargins(16, 12, 16, 12);
  root->setSpacing(9);

  auto* header = new QHBoxLayout;
  header->setSpacing(9);
  header->addWidget(eyebrow(kind == Kind::Petri ? QStringLiteral("Graph preview")
                                                : QStringLiteral("State preview")));
  auto* hint = new QLabel(kind == Kind::Petri
                              ? QStringLiteral("secondary view  ·  text is the primary model")
                              : QStringLiteral("secondary view  ·  select a state for details"));
  hint->setObjectName(QStringLiteral("faint"));
  hint->setFont(theme::mono(7.2));
  header->addWidget(hint);
  header->addStretch();

  if (kind == Kind::Petri) {
    auto* autoButton = action(QStringLiteral("AUTO"));
    autoButton->setCheckable(true);
    autoButton->setMinimumWidth(64);
    auto* fit = action(QStringLiteral("FIT"));
    auto* zoomOut = action(QStringLiteral("−"));
    auto* zoomIn = action(QStringLiteral("+"));
    auto* reset = action(QStringLiteral("RESET"));
    header->addWidget(fit);
    header->addWidget(zoomOut);
    header->addWidget(zoomIn);
    header->addWidget(reset);
    header->addWidget(autoButton);
  } else {
    auto* fit = action(QStringLiteral("FIT"));
    auto* zoomOut = action(QStringLiteral("−"));
    auto* zoomIn = action(QStringLiteral("+"));
    header->addWidget(fit);
    header->addWidget(zoomOut);
    header->addWidget(zoomIn);
  }

  auto* close = action(QStringLiteral("CLOSE"));
  close->setFixedWidth(72);
  header->addWidget(close);
  root->addLayout(header);

  auto* frame = new QFrame;
  frame->setObjectName(QStringLiteral("softPanel"));
  auto* frameLayout = new QVBoxLayout(frame);
  frameLayout->setContentsMargins(1, 1, 1, 1);

  if (kind == Kind::Petri) {
    petri_ = new PetriView;
    petri_->setMinimumSize(640, 440);
    frameLayout->addWidget(petri_);
    connect(petri_, &PetriView::nodeSelected, this, [this](const QString& kindName, int index) {
      if (kindName == QStringLiteral("place")) {
        emit placeSelected(index);
      } else {
        emit transitionSelected(index);
      }
    });
    connect(petri_, &PetriView::statusMessage, this, &PreviewDialog::setFooter);
  } else {
    state_ = new StateGraphView;
    state_->setMinimumSize(640, 440);
    frameLayout->addWidget(state_);
    connect(state_, &StateGraphView::nodeSelected, this, [this](int index) {
      emit stateSelected(index);
    });
    connect(state_, &StateGraphView::statusMessage, this, &PreviewDialog::setFooter);
  }
  root->addWidget(frame, 1);

  footer_ = new QLabel(kind == Kind::Petri
                           ? QStringLiteral("click a place / transition  ·  wheel to zoom  ·  "
                                            "AUTO is a structural token preview, not analysis")
                           : QStringLiteral("click a state  ·  wheel to zoom  ·  graph is derived "
                                            "from the state-class export"));
  footer_->setObjectName(QStringLiteral("faint"));
  footer_->setFont(theme::mono(7.2));
  root->addWidget(footer_);

  connect(close, &QPushButton::clicked, this, &QDialog::accept);

  if (kind == Kind::Petri) {
    auto* fit = static_cast<QPushButton*>(header->itemAt(3)->widget());
    auto* zoomOut = static_cast<QPushButton*>(header->itemAt(4)->widget());
    auto* zoomIn = static_cast<QPushButton*>(header->itemAt(5)->widget());
    auto* reset = static_cast<QPushButton*>(header->itemAt(6)->widget());
    auto* autoButton = static_cast<QPushButton*>(header->itemAt(7)->widget());
    connect(fit, &QPushButton::clicked, petri_, &PetriView::fitView);
    connect(zoomOut, &QPushButton::clicked, this, [this] { petri_->zoomBy(0.85); });
    connect(zoomIn, &QPushButton::clicked, this, [this] { petri_->zoomBy(1.18); });
    connect(reset, &QPushButton::clicked, petri_, &PetriView::resetSimulation);
    connect(autoButton, &QPushButton::toggled, this, [this, autoButton](bool enabled) {
      petri_->setAutoFire(enabled);
      autoButton->setText(enabled ? QStringLiteral("AUTO ON") : QStringLiteral("AUTO"));
    });
  } else {
    auto* fit = static_cast<QPushButton*>(header->itemAt(3)->widget());
    auto* zoomOut = static_cast<QPushButton*>(header->itemAt(4)->widget());
    auto* zoomIn = static_cast<QPushButton*>(header->itemAt(5)->widget());
    connect(fit, &QPushButton::clicked, state_, &StateGraphView::fitView);
    connect(zoomOut, &QPushButton::clicked, this, [this] { state_->zoomBy(0.85); });
    connect(zoomIn, &QPushButton::clicked, this, [this] { state_->zoomBy(1.18); });
  }
}

void PreviewDialog::setPetriNet(const PetriNet& net) {
  if (petri_ != nullptr) {
    petri_->setNet(net);
    QTimer::singleShot(0, petri_, &PetriView::fitView);
  }
}

void PreviewDialog::setStateGraph(const StateGraph& graph) {
  if (state_ != nullptr) {
    state_->setGraph(graph);
    QTimer::singleShot(0, state_, &StateGraphView::fitView);
  }
}

void PreviewDialog::setFooter(const QString& message) { footer_->setText(message); }
