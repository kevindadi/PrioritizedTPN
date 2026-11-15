#include "workflow_dialog.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

#include "theme.h"

namespace {

QLabel* eyebrow(const QString& text) {
  auto* label = new QLabel(text.toUpper());
  label->setStyleSheet(QStringLiteral(
      "color:#7DD3FC; font-size:9pt; font-weight:700; letter-spacing:2px;"));
  return label;
}

}  // namespace

WorkflowDialog::WorkflowDialog(QWidget* parent) : QDialog(parent) {
  setWindowTitle(QStringLiteral("PTPN  //  New workspace"));
  setModal(true);
  setMinimumSize(780, 470);
  setStyleSheet(theme::globalStyleSheet());

  auto* root = new QVBoxLayout(this);
  root->setContentsMargins(24, 18, 24, 18);
  root->setSpacing(12);

  root->addWidget(eyebrow(QStringLiteral("New workspace")));
  auto* title = new QLabel(QStringLiteral("Choose a workflow"));
  title->setFont(theme::display(22, QFont::Bold));
  title->setStyleSheet(QStringLiteral("color:#E8F7FF;"));
  root->addWidget(title);
  auto* subtitle = new QLabel(
      QStringLiteral("Text is the primary surface. The analysis pipeline is shared; only the "
                     "intake contract differs."));
  subtitle->setObjectName(QStringLiteral("muted"));
  subtitle->setWordWrap(true);
  subtitle->setFont(theme::mono(8));
  root->addWidget(subtitle);

  auto* cards = new QHBoxLayout;
  cards->setSpacing(12);
  cards->addWidget(makeCard(
      QStringLiteral("01  /  MODEL FIRST"), QStringLiteral("SPECIFICATION-FIRST"),
      QStringLiteral("Author or open RTSL, TDG JSON or PTPN. Priorities, cores and timing "
                     "intervals are explicit and editable."),
      QStringLiteral("OPEN SPECIFICATION"), QStringLiteral("#22D3EE"), Choice::Specification));
  cards->addWidget(makeCard(
      QStringLiteral("02  /  CODE FIRST"), QStringLiteral("BINARY-FIRST"),
      QStringLiteral("Import an ELF plus the DAG JSON emitted by the external static analyzer. "
                     "Core and priority are treated as assumptions until resolved."),
      QStringLiteral("IMPORT ELF"), QStringLiteral("#C4B5FD"), Choice::Binary));
  root->addLayout(cards, 1);

  auto* footer = new QHBoxLayout;
  auto* example = new QPushButton(QStringLiteral("LOAD HW EXAMPLE"));
  example->setObjectName(QStringLiteral("ghost"));
  example->setCursor(Qt::PointingHandCursor);
  footer->addWidget(example);
  footer->addStretch();
  auto* cancel = new QPushButton(QStringLiteral("CANCEL"));
  cancel->setObjectName(QStringLiteral("ghost"));
  cancel->setCursor(Qt::PointingHandCursor);
  cancel->setFixedWidth(84);
  footer->addWidget(cancel);
  root->addLayout(footer);

  connect(example, &QPushButton::clicked, this, [this] {
    choice_ = Choice::Example;
    accept();
  });
  connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
}

QWidget* WorkflowDialog::makeCard(const QString& kicker, const QString& title,
                                  const QString& description, const QString& buttonText,
                                  const QString& accent, Choice choice) {
  auto* card = new QFrame;
  card->setObjectName(QStringLiteral("softPanel"));
  auto* layout = new QVBoxLayout(card);
  layout->setContentsMargins(18, 16, 18, 16);
  layout->setSpacing(8);

  auto* kickerLabel = new QLabel(kicker);
  kickerLabel->setStyleSheet(QStringLiteral("color:#52627A; font-size:7pt; letter-spacing:1.5px;"));
  layout->addWidget(kickerLabel);

  auto* titleLabel = new QLabel(title);
  titleLabel->setFont(theme::display(16, QFont::Bold));
  titleLabel->setStyleSheet(QStringLiteral("color:%1;").arg(accent));
  layout->addWidget(titleLabel);

  auto* descriptionLabel = new QLabel(description);
  descriptionLabel->setObjectName(QStringLiteral("muted"));
  descriptionLabel->setWordWrap(true);
  descriptionLabel->setFont(theme::mono(7.8));
  descriptionLabel->setMinimumHeight(60);
  layout->addWidget(descriptionLabel);
  layout->addStretch();

  auto* button = new QPushButton(buttonText);
  button->setObjectName(choice == Choice::Specification ? QStringLiteral("primary")
                                                        : QStringLiteral("previewButton"));
  button->setCursor(Qt::PointingHandCursor);
  button->setMinimumHeight(34);
  layout->addWidget(button);
  connect(button, &QPushButton::clicked, this, [this, choice] {
    choice_ = choice;
    accept();
  });
  return card;
}
