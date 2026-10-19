#ifndef APP_PIPELINE_H
#define APP_PIPELINE_H

#include <cstddef>
#include <string>

#include "app/common.h"
#include "app/exports.h"

namespace ptpn {
class Diagnostics;
}

namespace app {

int run_tdg_pipeline(const std::string& input_file, PipelineOptions opts, size_t initial_memory,
                     ptpn::Diagnostics* diagnostics = nullptr);
// RTSL front-end: lex -> parse -> validate -> lower, then the same analysis
// pipeline as `run_tdg_pipeline`.
int run_rtsl_pipeline(const std::string& input_file, PipelineOptions opts, size_t initial_memory,
                      ptpn::Diagnostics* diagnostics = nullptr);
int run_ptpn_pipeline(const std::string& input_file, const PipelineOptions& opts,
                      size_t initial_memory, ptpn::Diagnostics* diagnostics = nullptr);

}  // namespace app

#endif  // APP_PIPELINE_H
