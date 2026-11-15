#include "text_view.h"

#include <QApplication>
#include <QClipboard>
#include <QHash>
#include <QFontMetrics>
#include <QPainter>
#include <QPaintEvent>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QScrollBar>
#include <QSyntaxHighlighter>
#include <QTextBlock>
#include <QTextBlockFormat>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextEdit>
#include <QTextFormat>

#include "theme.h"

namespace {

const QColor kInk = theme::c("#AFC4DC");
const QColor kHeading = theme::c("#7DD3FC");
const QColor kMuted = theme::c("#52627A");
const QColor kArrow = theme::c("#67E8F9");
const QColor kDanger = theme::c("#FB7185");
const QColor kWarn = theme::c("#FBBF24");
const QColor kGood = theme::c("#34D399");
const QColor kViolet = theme::c("#C4B5FD");
const QColor kNumber = theme::c("#E2E8F0");

class WorkbenchHighlighter final : public QSyntaxHighlighter {
 public:
  explicit WorkbenchHighlighter(QTextDocument* document) : QSyntaxHighlighter(document) {
    heading_.setForeground(kHeading);
    heading_.setFontWeight(QFont::DemiBold);

    muted_.setForeground(kMuted);
    arrow_.setForeground(kArrow);
    arrow_.setFontWeight(QFont::DemiBold);

    danger_.setForeground(kDanger);
    danger_.setFontWeight(QFont::DemiBold);
    warn_.setForeground(kWarn);
    good_.setForeground(kGood);
    violet_.setForeground(kViolet);
    number_.setForeground(kNumber);

    tagFormats_.insert(QStringLiteral("measured"), good_);
    tagFormats_.insert(QStringLiteral("certified"), good_);
    tagFormats_.insert(QStringLiteral("model"), violet_);
    tagFormats_.insert(QStringLiteral("static"), warn_);
    tagFormats_.insert(QStringLiteral("estimated"), warn_);
    tagFormats_.insert(QStringLiteral("unknown"), danger_);
    tagFormats_.insert(QStringLiteral("exact"), good_);
    tagFormats_.insert(QStringLiteral("approximate"), warn_);
    tagFormats_.insert(QStringLiteral("truncated"), danger_);
    tagFormats_.insert(QStringLiteral("back"), violet_);
    tagFormats_.insert(QStringLiteral("deadline miss"), danger_);
    tagFormats_.insert(QStringLiteral("capped"), warn_);
    tagFormats_.insert(QStringLiteral("N/A"), muted_);
  }

 protected:
  void highlightBlock(const QString& text) override {
    // Section headings such as "TASKS  3" or "CORE 0  ·  util ...".
    static const QRegularExpression heading(
        QStringLiteral("^[A-Z][A-Z0-9 /_+.-]{2,}(?:\\s{2,}.*)?$"));
    if (heading.match(text).hasMatch() && !text.startsWith(QLatin1Char(' '))) {
      setFormat(0, text.size(), heading_);
    }

    // Inline provenance / certification tags.
    static const QRegularExpression tag(QStringLiteral("\\[([^\\]]+)\\]"));
    auto it = tag.globalMatch(text);
    while (it.hasNext()) {
      const QRegularExpressionMatch match = it.next();
      const QString value = match.captured(1);
      if (tagFormats_.contains(value)) {
        setFormat(match.capturedStart(), match.capturedLength(), tagFormats_.value(value));
      }
    }

    // Directional arcs are the most important relational token in the text.
    static const QRegularExpression arrow(QStringLiteral("[→▶↔]"));
    auto arrows = arrow.globalMatch(text);
    while (arrows.hasNext()) {
      const QRegularExpressionMatch match = arrows.next();
      setFormat(match.capturedStart(), match.capturedLength(), arrow_);
    }

    if (text.startsWith(QStringLiteral("  !")) || text.contains(QStringLiteral("FAILED"))) {
      setFormat(0, text.size(), danger_);
    } else if (text.startsWith(QStringLiteral("  NOTE"))) {
      setFormat(0, text.size(), warn_);
    } else if (text.startsWith(QLatin1Char('#'))) {
      setFormat(0, text.size(), muted_);
    }

    // Right-aligned numeric-looking markers are deliberately understated.
    static const QRegularExpression numeric(QStringLiteral("\\b\\d+(?:\\.\\d+)?\\b"));
    auto numbers = numeric.globalMatch(text);
    int applied = 0;
    while (numbers.hasNext() && applied < 24) {
      const QRegularExpressionMatch match = numbers.next();
      if (!tagFormats_.contains(text.mid(match.capturedStart(), match.capturedLength()))) {
        setFormat(match.capturedStart(), match.capturedLength(), number_);
      }
      ++applied;
    }
  }

 private:
  QTextCharFormat heading_;
  QTextCharFormat muted_;
  QTextCharFormat arrow_;
  QTextCharFormat danger_;
  QTextCharFormat warn_;
  QTextCharFormat good_;
  QTextCharFormat violet_;
  QTextCharFormat number_;
  QHash<QString, QTextCharFormat> tagFormats_;
};

}  // namespace

class LineNumberArea final : public QWidget {
 public:
  explicit LineNumberArea(TextView* editor) : QWidget(editor), editor_(editor) {}

  QSize sizeHint() const override { return QSize(editor_->lineNumberAreaWidth(), 0); }

 protected:
  void paintEvent(QPaintEvent* event) override { editor_->lineNumberAreaPaintEvent(event); }

 private:
  TextView* editor_;
};

TextView::TextView(QWidget* parent) : QPlainTextEdit(parent) {
  setObjectName(QStringLiteral("stageText"));
  setReadOnly(true);
  setUndoRedoEnabled(false);
  setLineWrapMode(QPlainTextEdit::NoWrap);
  setFrameShape(QFrame::NoFrame);
  setFont(theme::mono(8.6));
  setTabStopDistance(fontMetrics().horizontalAdvance(QLatin1Char(' ')) * 4.0);
  setCursorWidth(1);
  setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
  setWordWrapMode(QTextOption::NoWrap);
  document()->setDocumentMargin(14.0);

  lineNumberArea_ = new LineNumberArea(this);
  connect(this, &QPlainTextEdit::blockCountChanged, this, &TextView::updateLineNumberAreaWidth);
  connect(this, &QPlainTextEdit::updateRequest, this, &TextView::updateLineNumberArea);
  connect(this, &QPlainTextEdit::cursorPositionChanged, this, &TextView::highlightCurrentLine);
  updateLineNumberAreaWidth(0);
  highlightCurrentLine();

  new WorkbenchHighlighter(document());
}

void TextView::setStageText(const QString& text) {
  setPlainText(text);
  QTextCursor cursor(document());
  cursor.select(QTextCursor::Document);
  QTextBlockFormat blockFormat;
  blockFormat.setLineHeight(128, QTextBlockFormat::ProportionalHeight);
  cursor.mergeBlockFormat(blockFormat);
  document()->setDocumentMargin(14.0);
  verticalScrollBar()->setValue(0);
  updateLineNumberAreaWidth(0);
  highlightCurrentLine();
}

void TextView::copyAll() {
  QApplication::clipboard()->setText(toPlainText());
}

int TextView::lineNumberAreaWidth() const {
  int digits = 1;
  int max = qMax(1, blockCount());
  while (max >= 10) {
    max /= 10;
    ++digits;
  }
  return 12 + fontMetrics().horizontalAdvance(QLatin1Char('9')) * digits;
}

void TextView::updateLineNumberAreaWidth(int) {
  setViewportMargins(lineNumberAreaWidth(), 0, 0, 0);
}

void TextView::updateLineNumberArea(const QRect& rect, int dy) {
  if (dy != 0) {
    lineNumberArea_->scroll(0, dy);
  } else {
    lineNumberArea_->update(0, rect.y(), lineNumberArea_->width(), rect.height());
  }
  if (rect.contains(viewport()->rect())) {
    updateLineNumberAreaWidth(0);
  }
}

void TextView::resizeEvent(QResizeEvent* event) {
  QPlainTextEdit::resizeEvent(event);
  const QRect contents = contentsRect();
  lineNumberArea_->setGeometry(
      QRect(contents.left(), contents.top(), lineNumberAreaWidth(), contents.height()));
}

void TextView::highlightCurrentLine() {
  QList<QTextEdit::ExtraSelection> selections;
  if (!isReadOnly()) {
    QTextEdit::ExtraSelection selection;
    selection.format.setBackground(theme::c("#0B1828"));
    selection.format.setProperty(QTextFormat::FullWidthSelection, true);
    selection.cursor = textCursor();
    selection.cursor.clearSelection();
    selections.append(selection);
  }
  setExtraSelections(selections);
}

void TextView::lineNumberAreaPaintEvent(QPaintEvent* event) {
  QPainter painter(lineNumberArea_);
  painter.fillRect(event->rect(), theme::c("#070E19"));

  QTextBlock block = firstVisibleBlock();
  int blockNumber = block.blockNumber();
  int top = qRound(blockBoundingGeometry(block).translated(contentOffset()).top());
  int bottom = top + qRound(blockBoundingRect(block).height());
  const int currentLine = textCursor().blockNumber();

  while (block.isValid() && top <= event->rect().bottom()) {
    if (block.isVisible() && bottom >= event->rect().top()) {
      painter.setFont(theme::mono(7.4));
      painter.setPen(currentLine == blockNumber ? theme::c("#67E8F9") : theme::c("#334155"));
      painter.drawText(0, top, lineNumberArea_->width() - 8,
                       qRound(blockBoundingRect(block).height()), Qt::AlignRight | Qt::AlignTop,
                       QString::number(blockNumber + 1));
    }
    block = block.next();
    top = bottom;
    bottom = top + qRound(blockBoundingRect(block).height());
    ++blockNumber;
  }
}
