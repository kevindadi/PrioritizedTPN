#ifndef ANALYSIS_REACHABILITY_CANONICALIZATION_H
#define ANALYSIS_REACHABILITY_CANONICALIZATION_H

namespace state_class {

// How freshly computed state classes are matched against already discovered
// ones. EQUALITY keeps the graph exact; the other two apply the zone-inclusion
// abstraction from the formal document (a smaller zone is merged into a larger
// one with the same marking).
enum class CanonicalizationMode {
  EQUALITY,         // CheckEquality: identical marking, layout and DBM matrix
  MAX_LOWER_BOUND,  // CheckInclusion: zone-inclusion abstraction
  INTERSECTION,     // CheckInclusion: zone-inclusion abstraction
};

struct StateClass;

// Exact match: same marking, same variable layout, identical DBM matrix.
bool check_equality(const StateClass& a, const StateClass& b);

// Inclusion: same marking and layout, and a's zone is a subset of b's zone.
bool check_inclusion(const StateClass& a, const StateClass& b);

// True when `candidate` may be merged into the already-discovered `existing`
// under `mode` (exact match for EQUALITY, zone inclusion otherwise).
bool can_merge_into(const StateClass& candidate, const StateClass& existing,
                    CanonicalizationMode mode);

}  // namespace state_class

#endif  // ANALYSIS_REACHABILITY_CANONICALIZATION_H
