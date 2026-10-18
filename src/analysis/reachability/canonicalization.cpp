#include "analysis/reachability/canonicalization.h"

#include "analysis/reachability/state_class.h"

namespace state_class {

namespace {
// Two classes share a variable layout iff they have the same marking (the
// layout is fully determined by the marking) and the same selected active set
// (which decides which clocks run and which suspension clocks exist). The
// suspended set is derivable from those two and the net, but it is compared as
// well so that the identity stays explicit and robust.
bool same_layout(const StateClass& a, const StateClass& b) {
  return a.marking == b.marking && a.clock_vars == b.clock_vars &&
         a.release_clock_of_task == b.release_clock_of_task &&
         a.laxity_clock_of_task == b.laxity_clock_of_task &&
         a.priority_enabled == b.priority_enabled && a.suspended == b.suspended;
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
