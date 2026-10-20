#include "model/petri.h"

#include <spdlog/spdlog.h>
#include <sstream>

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

std::string TimeInterval::to_string() const {
  std::ostringstream oss;
  oss << (left_open ? "(" : "[") << earliest << ", ";
  if (latest == INF) {
    oss << "∞";
  } else {
    oss << latest;
  }
  oss << (right_open ? ")" : "]");
  return oss.str();
}

void PTPN::set_arcs(const std::vector<ArcSpec>& pre_arcs, const std::vector<ArcSpec>& post_arcs) {
  for (const ArcSpec& arc : pre_arcs) {
    if (arc.place.index() >= places_.size() || arc.transition.index() >= transitions_.size()) {
      throw std::out_of_range("Invalid place or transition index");
    }
    if (arc.weight <= 0) {
      throw std::invalid_argument("Arc weight must be positive");
    }
    pre_matrix_[arc.place.index()][arc.transition.index()] = arc.weight;
  }
  for (const ArcSpec& arc : post_arcs) {
    if (arc.transition.index() >= post_matrix_.size() || arc.place.index() >= places_.size()) {
      throw std::out_of_range("Invalid transition or place index");
    }
    if (arc.weight <= 0) {
      throw std::invalid_argument("Arc weight must be positive");
    }
    post_matrix_[arc.transition.index()][arc.place.index()] = arc.weight;
  }
  rebuild_sparse_arcs();
}

std::string PTPN::to_string() const {
  std::ostringstream oss;
  oss << "=== PTPN ===\n";
  oss << "Places (" << places_.size() << "):\n";
  for (size_t i = 0; i < places_.size(); ++i) {
    oss << "  P" << i << ": " << places_[i].name << " [capacity="
        << (places_[i].capacity == INF ? "∞" : std::to_string(places_[i].capacity))
        << ", tokens=" << m0_[i] << "]\n";
  }

  oss << "\nTransitions (" << transitions_.size() << "):\n";
  for (size_t i = 0; i < transitions_.size(); ++i) {
    oss << "  T" << i << ": " << transitions_[i].name
        << " [time=" << transitions_[i].time_interval.to_string()
        << ", priority=" << transitions_[i].priority << ", core=" << transitions_[i].core
        << ", suspendable=" << (transitions_[i].suspendable ? "yes" : "no") << "]\n";
  }

  oss << "\nPre Matrix (" << pre_matrix_.size() << "x"
      << (pre_matrix_.empty() ? 0 : pre_matrix_[0].size()) << "):\n";
  for (size_t p = 0; p < pre_matrix_.size(); ++p) {
    oss << "  P" << p << ": ";
    for (size_t t = 0; t < pre_matrix_[p].size(); ++t) {
      oss << pre_matrix_[p][t] << " ";
    }
    oss << "\n";
  }

  oss << "\nPost Matrix (" << post_matrix_.size() << "x"
      << (post_matrix_.empty() ? 0 : post_matrix_[0].size()) << "):\n";
  for (size_t t = 0; t < post_matrix_.size(); ++t) {
    oss << "  T" << t << ": ";
    for (size_t p = 0; p < post_matrix_[t].size(); ++p) {
      oss << post_matrix_[t][p] << " ";
    }
    oss << "\n";
  }

  return oss.str();
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