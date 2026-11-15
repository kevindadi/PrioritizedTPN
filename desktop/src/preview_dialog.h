#pragma once

#include <QDialog>

#include "app_model.h"

class QLabel;
class PetriView;
class StateGraphView;

// Secondary graph view. The workbench keeps text as the primary surface; this
// dialog is opened only from an explicit PREVIEW action.
class PreviewDialog final : public QDialog {
  Q_OBJECT

 public:
  enum class Kind { Petri, State };

  explicit PreviewDialog(Kind kind, QWidget* parent = nullptr);

  void setPetriNet(const PetriNet& net);
  void setStateGraph(const StateGraph& graph);
  void setGenericGraph(const StateGraph& graph);
  void setContext(const QString& title, const QString& footer);

 signals:
  void placeSelected(int index);
  void transitionSelected(int index);
  void stateSelected(int index);

 private:
  void setFooter(const QString& message);

  Kind kind_;
  PetriView* petri_ = nullptr;
  StateGraphView* state_ = nullptr;
  QLabel* footer_ = nullptr;
  QLabel* eyebrow_ = nullptr;
  QLabel* hint_ = nullptr;
};
