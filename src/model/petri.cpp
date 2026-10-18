#include "model/petri.h"

#include <spdlog/spdlog.h>

namespace petri {

namespace {
std::vector<size_t> g_overflow_places;
}

void reset_overflow_recording() {
  g_overflow_places.clear();
}

void record_overflow(size_t place_idx) {
  g_overflow_places.push_back(place_idx);
}

const std::vector<size_t>& overflowed_places() {
  return g_overflow_places;
}

bool PTPN::verify_structure() const {
  bool is_valid = true;

  if (!pre_matrix_.empty() && !transitions_.empty()) {
    size_t expected_cols = transitions_.size();
    for (size_t p = 0; p < pre_matrix_.size(); ++p) {
      if (pre_matrix_[p].size() != expected_cols) {
        spdlog::error("[PETRI] Pre matrix row {} dimension mismatch", p);
        is_valid = false;
      }
    }
  }

  if (!post_matrix_.empty() && !places_.empty()) {
    size_t expected_cols = places_.size();
    for (size_t t = 0; t < post_matrix_.size(); ++t) {
      if (post_matrix_[t].size() != expected_cols) {
        spdlog::error("[PETRI] Post matrix row {} dimension mismatch", t);
        is_valid = false;
      }
    }
  }

  if (m0_.size() != places_.size()) {
    spdlog::error("[PETRI] Initial marking dimension mismatch with places count");
    is_valid = false;
  }

  for (size_t t = 0; t < transitions_.size(); ++t) {
    if (!transitions_[t].time_interval.is_valid()) {
      spdlog::error("[PETRI] Transition T{} has invalid time interval", t);
      is_valid = false;
    }
  }

  if (is_valid) {
    spdlog::info("[PETRI] Structure verification passed");
  }

  return is_valid;
}

}  // namespace petri