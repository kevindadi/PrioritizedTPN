#pragma once

#include <QColor>
#include <QFont>
#include <QString>

namespace theme {

inline QColor c(const char* hex) { return QColor(QLatin1String(hex)); }
inline QColor alpha(QColor color, int alpha) {
  color.setAlpha(alpha);
  return color;
}

inline QFont ui(int pointSize = 10, QFont::Weight weight = QFont::Normal) {
  QFont font(QStringLiteral("Segoe UI Variable Text"));
  font.setFamilies({QStringLiteral("Segoe UI Variable Text"), QStringLiteral("Segoe UI"),
                    QStringLiteral("Noto Sans"), QStringLiteral("Arial")});
  font.setPointSize(pointSize);
  font.setWeight(weight);
  return font;
}

inline QFont display(int pointSize = 18, QFont::Weight weight = QFont::Bold) {
  QFont font(QStringLiteral("Bahnschrift"));
  font.setFamilies({QStringLiteral("Bahnschrift SemiBold"), QStringLiteral("Segoe UI Variable Display"),
                    QStringLiteral("Segoe UI"), QStringLiteral("Arial")});
  font.setPointSize(pointSize);
  font.setWeight(weight);
  font.setLetterSpacing(QFont::AbsoluteSpacing, 0.6);
  return font;
}

inline QFont mono(int pointSize = 9, QFont::Weight weight = QFont::Normal) {
  QFont font(QStringLiteral("Cascadia Mono"));
  font.setFamilies({QStringLiteral("Cascadia Mono"), QStringLiteral("Consolas"),
                    QStringLiteral("Courier New")});
  font.setPointSize(pointSize);
  font.setWeight(weight);
  return font;
}

inline QString globalStyleSheet() {
  return QStringLiteral(R"(
    QWidget {
      color: #DCE9F8;
      selection-background-color: #0E7490;
      selection-color: #E6FFFF;
    }
    QMainWindow, QDialog {
      background: #070B14;
    }
    QFrame#panel {
      background: #0B1220;
      border: 1px solid #17253C;
      border-radius: 16px;
    }
    QFrame#softPanel {
      background: #0D1626;
      border: 1px solid #16243A;
      border-radius: 14px;
    }
    QFrame#header {
      background: #080E1A;
      border-bottom: 1px solid #152238;
    }
    QLabel#brand {
      color: #E8F7FF;
    }
    QLabel#brandSub {
      color: #64748B;
      letter-spacing: 2px;
    }
    QLabel#sectionTitle {
      color: #7DD3FC;
      font-size: 9pt;
      font-weight: 600;
      letter-spacing: 2px;
    }
    QLabel#muted {
      color: #7C8CA8;
    }
    QLabel#faint {
      color: #52627A;
    }
    QLabel#value {
      color: #F1F5F9;
      font-size: 20pt;
      font-weight: 700;
    }
    QLabel#pathChip {
      color: #A5F3FC;
      background: #0B1B2B;
      border: 1px solid #164E63;
      border-radius: 9px;
      padding: 5px 12px;
    }
    QLabel#workflowChip {
      color: #67E8F9;
      background: #0A1B28;
      border: 1px solid #155E75;
      border-radius: 8px;
      padding: 5px 10px;
      letter-spacing: 1px;
    }
    QLineEdit {
      color: #DCE9F8;
      background: #0B1422;
      border: 1px solid #1E2F49;
      border-radius: 8px;
      padding: 5px 8px;
      selection-background-color: #0E7490;
    }
    QLineEdit:focus {
      border-color: #22D3EE;
    }
    QLineEdit#findBox {
      color: #AFC4DC;
      background: #091321;
      border-color: #17253C;
    }
    QPushButton#previewButton {
      color: #A5F3FC;
      background: #0B1B2B;
      border: 1px solid #164E63;
      font-size: 8pt;
      letter-spacing: 1px;
    }
    QPushButton#previewButton:hover {
      background: #0E2B3D;
      border-color: #22D3EE;
    }
    QPushButton#previewButton:disabled {
      color: #40516A;
      border-color: #17253C;
      background: transparent;
    }
    QPushButton {
      color: #DCE9F8;
      background: #111B2E;
      border: 1px solid #223250;
      border-radius: 9px;
      padding: 7px 13px;
      font-weight: 600;
    }
    QPushButton:hover {
      background: #17253B;
      border-color: #2E4A70;
    }
    QPushButton:pressed {
      background: #0D1626;
    }
    QPushButton#primary {
      color: #04131B;
      background: #22D3EE;
      border: 1px solid #67E8F9;
      padding: 8px 18px;
      font-weight: 700;
    }
    QPushButton#primary:hover {
      background: #67E8F9;
    }
    QPushButton#primary:disabled {
      background: #164E63;
      border-color: #164E63;
      color: #6B8FA3;
    }
    QPushButton#ghost {
      background: transparent;
      border: 1px solid #1E2F49;
      color: #93A8C5;
    }
    QPushButton#ghost:hover {
      border-color: #22D3EE;
      color: #A5F3FC;
    }
    QPushButton#chip {
      background: #0D1828;
      border: 1px solid #1B2C45;
      color: #8FA6C4;
      font-weight: 500;
      padding: 5px 10px;
      text-align: left;
    }
    QPushButton#chip:hover {
      background: #13243A;
      border-color: #2B4B70;
      color: #CFFAFE;
    }
    QPushButton#windowButton {
      background: transparent;
      border: none;
      color: #8FA6C4;
      padding: 4px 8px;
    }
    QPushButton#windowButton:hover {
      background: #1E293B;
      color: #F8FAFC;
    }
    QTabWidget::pane {
      border: 1px solid #16243A;
      border-radius: 14px;
      background: #080F1B;
      top: -1px;
    }
    QTabBar::tab {
      background: transparent;
      color: #6F86A3;
      padding: 9px 18px;
      margin-right: 4px;
      border: 1px solid transparent;
      border-radius: 8px;
      font-weight: 600;
      letter-spacing: 1px;
    }
    QTabBar::tab:selected {
      color: #A5F3FC;
      background: #0B1B2B;
      border: 1px solid #164E63;
    }
    QTabBar::tab:hover:!selected {
      color: #C3D6EE;
      background: #0C1626;
    }
    QPlainTextEdit, QTextEdit {
      background: #060C16;
      border: 1px solid #16243A;
      border-radius: 10px;
      color: #8FA6C4;
      padding: 8px;
    }
    QPlainTextEdit#stageText {
      background: #060C16;
      border: none;
      border-radius: 10px;
      padding: 0px;
      color: #AFC4DC;
    }
    QPlainTextEdit#assumptionText {
      background: #08111D;
      border: 1px solid #16243A;
      border-radius: 9px;
      padding: 8px;
      color: #AFC4DC;
    }
    QPlainTextEdit#logView {
      background: #060C16;
      border: 1px solid #16243A;
      border-radius: 9px;
      padding: 7px;
      color: #8FA6C4;
    }
    QScrollArea {
      border: none;
      background: transparent;
    }
    QScrollBar:vertical {
      background: transparent;
      width: 9px;
      margin: 2px;
    }
    QScrollBar::handle:vertical {
      background: #1E3A52;
      min-height: 28px;
      border-radius: 4px;
    }
    QScrollBar::handle:vertical:hover {
      background: #22D3EE;
    }
    QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {
      height: 0;
    }
    QScrollBar:horizontal {
      background: transparent;
      height: 9px;
      margin: 2px;
    }
    QScrollBar::handle:horizontal {
      background: #1E3A52;
      min-width: 28px;
      border-radius: 4px;
    }
    QToolTip {
      background: #0B1B2B;
      color: #E6F7FF;
      border: 1px solid #164E63;
      padding: 6px;
    }
    QMenu {
      background: #0B1422;
      color: #DCE9F8;
      border: 1px solid #1E2F49;
      padding: 4px;
    }
    QMenu::item {
      padding: 6px 18px;
      border-radius: 5px;
    }
    QMenu::item:selected {
      background: #13243A;
      color: #A5F3FC;
    }
    QMenu::separator {
      height: 1px;
      background: #17253C;
      margin: 4px 8px;
    }
    QStatusBar {
      background: #070C15;
      color: #64748B;
      border-top: 1px solid #111C2D;
    }
    QSplitter::handle {
      background: transparent;
      width: 6px;
    }
  )");
}

}  // namespace theme
