#pragma once

#include <QString>

#include "app_model.h"

struct WorkbenchDocs {
  QString workflow;
  QString inputKind;
  QString dag;
  QString ptpn;
  QString state;
  QString timeline;
  QString report;
  QString assumptions;

  QString dagSummary;
  QString ptpnSummary;
  QString stateSummary;
  QString timelineSummary;
  QString reportSummary;
};

// Builds the text-first workbench surfaces from the analyzer result. The text
// documents are the primary representation; graphs are only secondary previews.
WorkbenchDocs renderWorkbench(const AnalysisResult& result);
