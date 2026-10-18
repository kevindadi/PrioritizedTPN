#include "analysis/reachability/canonicalization.h"

#include "analysis/reachability/state_class.h"

namespace state_class {

namespace {
// Two classes share a variable layout iff they have the same marking (the
// layout is fully determined by the marking), so comparing markings and the
// clock-variable vector is enough to know the DBMs are dimension-compatible.
bool same_layout(const StateClass& a, const StateClass& b) {
  return a.marking == b.marking && a.clock_vars == b.clock_vars;
}
}  // namespace

bool check_equality(const StateClass& a, const StateClass& b) {
  if (!same_layout(a, b)) {
    return false;
  }
  return a.zone.raw_matrix() == b.zone.raw_matrix();
}

bool check_inclusion(const StateClass& a, const StateClass& b) {
  if (!same_layout(a, b)) {
    return false;
  }
  return a.zone.included_in(b.zone);
}

bool can_merge_into(const StateClass& candidate, const StateClass& existing,
                    CanonicalizationMode mode) {
  if (mode == CanonicalizationMode::EQUALITY) {
    return check_equality(candidate, existing);
  }
  return check_inclusion(candidate, existing);
}

}  // namespace state_class
