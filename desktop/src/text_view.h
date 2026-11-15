#pragma once

#include <QPlainTextEdit>

class QPaintEvent;
class QResizeEvent;
class LineNumberArea;

// A read-only, line-numbered text surface used by every workbench stage.
// The workbench is text-first: this widget is the primary representation, and
// graph widgets are only shown from an explicit preview action.
class TextView : public QPlainTextEdit {
  Q_OBJECT

 public:
  explicit TextView(QWidget* parent = nullptr);

  void setStageText(const QString& text);
  void copyAll();

  int lineNumberAreaWidth() const;
  void lineNumberAreaPaintEvent(QPaintEvent* event);

 protected:
  void resizeEvent(QResizeEvent* event) override;

 private slots:
  void updateLineNumberAreaWidth(int newBlockCount);
  void updateLineNumberArea(const QRect& rect, int dy);
  void highlightCurrentLine();

 private:
  LineNumberArea* lineNumberArea_ = nullptr;
  friend class LineNumberArea;
};
