#include <algorithm>
#include <stdexcept>

#include "analysis/dbm/clock_state.h"
#include "analysis/dbm/dbm.h"
#include "analysis/dbm/dbm_internal.h"

namespace state_class {

DBM DBM::intersection(const DBM& other) const {
  if (clock_count_ != other.clock_count_) {
    throw std::invalid_argument("DBM sizes must match for intersection");
  }

  DBM result(clock_count_);

  for (size_t i = 0; i < clock_count_; ++i) {
    for (size_t j = 0; j < clock_count_; ++j) {
      int bound1 = matrix_[offset(i, j)];
      int bound2 = other.matrix_[other.offset(i, j)];
      size_t ij = result.offset(i, j);

      if (bound1 == INF_TIME) {
        result.matrix_[ij] = bound2;
      } else if (bound2 == INF_TIME) {
        result.matrix_[ij] = bound1;
      } else {
        result.matrix_[ij] = std::min(bound1, bound2);
      }
    }
  }

  result.minimize();

  return result;
}

void DBM::extrapolate(int k) {
  if (clock_count_ == 0 || k < 0) {
    return;
  }

  bool changed = false;
  for (size_t i = 0; i < clock_count_; ++i) {
    if (is_frozen(i)) {
      continue;
    }
    for (size_t j = 0; j < clock_count_; ++j) {
      if (i == j || is_frozen(j)) {
        continue;
      }
      const size_t ij = offset(i, j);
      const int bound = matrix_[ij];
      if (bound == INF_TIME) {
        continue;
      }
      if (bound > k) {
        matrix_[ij] = INF_TIME;
        changed = true;
      } else if (bound < -k) {
        matrix_[ij] = -k;
        changed = true;
      }
    }
  }

  if (changed) {
    minimize();
  }
}

bool DBM::contains(const DBM& other) const {
  if (clock_count_ != other.clock_count_) {
    return false;
  }

  for (size_t i = 0; i < clock_count_; ++i) {
    for (size_t j = 0; j < clock_count_; ++j) {
      int this_bound = matrix_[offset(i, j)];
      int other_bound = other.matrix_[other.offset(i, j)];

      if (other_bound != INF_TIME && (this_bound == INF_TIME || other_bound < this_bound)) {
        return false;
      }
    }
  }

  return frozen_clocks_ == other.frozen_clocks_;
}

bool DBM::included_in(const DBM& other) const {
  if (clock_count_ != other.clock_count_) {
    return false;
  }

  for (size_t i = 0; i < clock_count_; ++i) {
    for (size_t j = 0; j < clock_count_; ++j) {
      const int this_bound = matrix_[offset(i, j)];
      const int other_bound = other.matrix_[other.offset(i, j)];

      if (other_bound == INF_TIME) {
        continue;  // other imposes no bound here; anything is contained
      }
      if (this_bound == INF_TIME || this_bound > other_bound) {
        return false;  // this is looser than other on (i, j)
      }
    }
  }

  return true;
}

}  // namespace state_class
