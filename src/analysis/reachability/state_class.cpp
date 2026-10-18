#include "analysis/reachability/state_class.h"

#include <functional>
#include <sstream>

#include "analysis/dbm/clock_state.h"

namespace state_class {

namespace {
void hash_combine(size_t& seed, size_t value) {
  seed ^= value + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
}
}  // namespace

std::string FiringEdge::to_string() const {
  std::ostringstream oss;
  oss << "T" << transition_id << "@[" << firing_min << ", "
      << (firing_max == INF_TIME ? "inf" : std::to_string(firing_max)) << "]";
  return oss.str();
}

size_t MarkingHash::operator()(const std::vector<int>& marking) const {
  size_t seed = 0;
  std::hash<int> int_hash;
  for (int value : marking) {
    hash_combine(seed, int_hash(value));
  }
  return seed;
}

size_t hash_state_class(const StateClass& state) {
  size_t seed = 0;
  std::hash<int> int_hash;
  std::hash<size_t> size_hash;

  for (int value : state.marking) {
    hash_combine(seed, int_hash(value));
  }
  for (const ClockVar& var : state.clock_vars) {
    hash_combine(seed, size_hash(static_cast<size_t>(var.kind)));
    hash_combine(seed, size_hash(var.transition));
  }
  for (int value : state.zone.raw_matrix()) {
    hash_combine(seed, int_hash(value));
  }
  return seed;
}

}  // namespace state_class
