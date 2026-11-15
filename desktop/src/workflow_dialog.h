#pragma once

#include <QDialog>

class QLabel;

// First-run workflow chooser. It deliberately does not pick a model for the
// user: specification-first and binary-first have different intake contracts.
class WorkflowDialog final : public QDialog {
  Q_OBJECT

 public:
  enum class Choice { None, Specification, Binary, Example };

  explicit WorkflowDialog(QWidget* parent = nullptr);
  Choice choice() const { return choice_; }

 private:
  QWidget* makeCard(const QString& kicker, const QString& title, const QString& description,
                    const QString& buttonText, const QString& accent, Choice choice);

  Choice choice_ = Choice::None;
};
