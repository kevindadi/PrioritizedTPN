#include <QApplication>
#include <QPalette>
#include <QStyle>
#include <QStyleFactory>

#include "main_window.h"
#include "theme.h"

int main(int argc, char* argv[]) {
  QApplication application(argc, argv);
  application.setApplicationName(QStringLiteral("PTPN Timed Petri Lab"));
  application.setApplicationDisplayName(QStringLiteral("PTPN  //  Timed Petri Lab"));
  application.setOrganizationName(QStringLiteral("PTPN"));

  if (QStyle* fusion = QStyleFactory::create(QStringLiteral("Fusion"))) {
    application.setStyle(fusion);
  }

  QPalette palette;
  palette.setColor(QPalette::Window, theme::c("#070B14"));
  palette.setColor(QPalette::WindowText, theme::c("#DCE9F8"));
  palette.setColor(QPalette::Base, theme::c("#060C16"));
  palette.setColor(QPalette::AlternateBase, theme::c("#0B1220"));
  palette.setColor(QPalette::Text, theme::c("#DCE9F8"));
  palette.setColor(QPalette::Button, theme::c("#111B2E"));
  palette.setColor(QPalette::ButtonText, theme::c("#DCE9F8"));
  palette.setColor(QPalette::Highlight, theme::c("#0E7490"));
  palette.setColor(QPalette::HighlightedText, theme::c("#E6FFFF"));
  palette.setColor(QPalette::ToolTipBase, theme::c("#0B1B2B"));
  palette.setColor(QPalette::ToolTipText, theme::c("#E6F7FF"));
  palette.setColor(QPalette::Disabled, QPalette::Text, theme::c("#52627A"));
  application.setPalette(palette);
  application.setFont(theme::ui(9));
  application.setStyleSheet(theme::globalStyleSheet());

  MainWindow window;
  window.show();
  return application.exec();
}
