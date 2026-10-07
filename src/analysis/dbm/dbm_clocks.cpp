#include <algorithm>
#include <stdexcept>
#include <vector>

#include "analysis/dbm/clock_state.h"
#include "analysis/dbm/dbm.h"
#include "analysis/dbm/dbm_internal.h"

namespace state_class {

void DBM::elapse_time(int delta) {
  if (delta <= 0 || clock_count_ == 0)
    return;

  bool changed = false;
  for (size_t i = 1; i < clock_count_; ++i) {
    if (is_frozen(i)) {
      continue;
    }

    const int current_upper = matrix_[offset(i, 0)];
    if (current_upper != INF_TIME) {
      matrix_[offset(i, 0)] = detail::safe_add_bound(current_upper, delta);
      changed = true;
    }

    const int current_lower = matrix_[offset(0, i)];
    if (current_lower != INF_TIME) {
      matrix_[offset(0, i)] = detail::safe_add_bound(current_lower, -delta);
      changed = true;
    }
  }

  if (changed) {
    minimize();
  }
}

void DBM::future() {
  if (clock_count_ <= 1) {
    return;
  }

  bool changed = false;
  for (size_t i = 1; i < clock_count_; ++i) {
    if (is_frozen(i)) {
      continue;
    }

    const size_t lower_bound_offset = offset(0, i);
    if (matrix_[lower_bound_offset] != INF_TIME) {
      matrix_[lower_bound_offset] = INF_TIME;
      changed = true;
    }
  }

  if (changed) {
    minimize();
  }
}

void DBM::reset_clock(size_t clock_idx) {
  check_index(clock_idx, clock_idx);

  if (clock_idx == 0) {
    return;
  }

  bool changed = false;

  if (matrix_[offset(0, clock_idx)] != 0) {
    matrix_[offset(0, clock_idx)] = 0;
    changed = true;
  }
  if (matrix_[offset(clock_idx, 0)] != 0) {
    matrix_[offset(clock_idx, 0)] = 0;
    changed = true;
  }

  for (size_t k = 0; k < clock_count_; ++k) {
    const int row0 = matrix_[offset(0, k)];
    if (matrix_[offset(clock_idx, k)] != row0) {
      matrix_[offset(clock_idx, k)] = row0;
      changed = true;
    }

    const int col0 = matrix_[offset(k, 0)];
    if (matrix_[offset(k, clock_idx)] != col0) {
      matrix_[offset(k, clock_idx)] = col0;
      changed = true;
    }
  }

  matrix_[offset(clock_idx, clock_idx)] = 0;

  if (changed) {
    minimize();
  }
}

void DBM::forget_clock(size_t clock_idx) {
  check_index(clock_idx, clock_idx);

  if (clock_idx == 0) {
    return;
  }

  bool changed = false;
  for (size_t i = 0; i < clock_count_; ++i) {
    if (i != clock_idx) {
      if (matrix_[offset(clock_idx, i)] != INF_TIME) {
        matrix_[offset(clock_idx, i)] = INF_TIME;
        changed = true;
      }
      if (matrix_[offset(i, clock_idx)] != INF_TIME) {
        matrix_[offset(i, clock_idx)] = INF_TIME;
        changed = true;
      }
    }
  }

  if (matrix_[offset(clock_idx, 0)] != INF_TIME) {
    matrix_[offset(clock_idx, 0)] = INF_TIME;
    changed = true;
  }
  if (matrix_[offset(0, clock_idx)] != 0) {
    matrix_[offset(0, clock_idx)] = 0;
    changed = true;
  }
  if (matrix_[offset(clock_idx, clock_idx)] != 0) {
    matrix_[offset(clock_idx, clock_idx)] = 0;
    changed = true;
  }

  unfreeze_clock(clock_idx);

  if (changed) {
    minimize();
  }
}

void DBM::remove_clock(size_t clock_idx) {
  if (clock_idx >= clock_count_ || clock_idx == 0) {
    return;
  }

  unfreeze_clock(clock_idx);

  const size_t new_count = clock_count_ - 1;
  std::vector<int> new_matrix(new_count * new_count, INF_TIME);

  for (size_t i = 0; i < clock_count_; ++i) {
    if (i == clock_idx)
      continue;

    size_t new_i = (i < clock_idx) ? i : i - 1;
    for (size_t j = 0; j < clock_count_; ++j) {
      if (j == clock_idx)
        continue;

      size_t new_j = (j < clock_idx) ? j : j - 1;
      new_matrix[new_i * new_count + new_j] = matrix_[offset(i, j)];
    }
  }

  matrix_ = std::move(new_matrix);
  clock_count_ = new_count;

  // Remap indices above the removed clock; iterating the sorted vector in
  // order keeps the result sorted.
  std::vector<size_t> new_frozen;
  new_frozen.reserve(frozen_clocks_.size());
  for (size_t idx : frozen_clocks_) {
    if (idx < clock_idx) {
      new_frozen.push_back(idx);
    } else if (idx > clock_idx) {
      new_frozen.push_back(idx - 1);
    }
  }
  frozen_clocks_ = std::move(new_frozen);
}

DBM DBM::restrict_clock(size_t clock_idx, int alpha, int beta) const {
  if (clock_idx >= clock_count_) {
    return *this;
  }

  DBM result(*this);

  int current_lower = -result.get_constraint(0, clock_idx);
  if (alpha > current_lower) {
    result.set_constraint(0, clock_idx, -alpha);
  }

  int current_upper = result.get_constraint(clock_idx, 0);
  if (beta != INF_TIME && (current_upper == INF_TIME || beta < current_upper)) {
    result.set_constraint(clock_idx, 0, beta);
  }

  result.minimize();

  for (size_t i = 0; i < result.clock_count_; ++i) {
    if (result.matrix_[result.offset(i, i)] < 0) {
      return DBM(0);
    }
  }

  return result;
}

void DBM::constrain_upper_bound(size_t clock_idx, int beta) {
  if (clock_idx >= clock_count_ || beta == INF_TIME) {
    return;
  }

  const size_t upper_bound_offset = offset(clock_idx, 0);
  const int current_upper = matrix_[upper_bound_offset];
  if (current_upper != INF_TIME && beta < current_upper) {
    matrix_[upper_bound_offset] = beta;
    minimize();
  }
}

void DBM::synchronize_clocks(const std::vector<size_t>& clock_indices) {
  if (clock_indices.size() < 2) {
    return;
  }

  bool changed = false;
  for (size_t left = 0; left < clock_indices.size(); ++left) {
    const size_t first = clock_indices[left];
    if (first >= clock_count_) {
      continue;
    }

    for (size_t right = left + 1; right < clock_indices.size(); ++right) {
      const size_t second = clock_indices[right];
      if (second >= clock_count_) {
        continue;
      }

      if (matrix_[offset(first, second)] != 0) {
        matrix_[offset(first, second)] = 0;
        changed = true;
      }
      if (matrix_[offset(second, first)] != 0) {
        matrix_[offset(second, first)] = 0;
        changed = true;
      }
    }
  }

  if (changed) {
    minimize();
  }
}

DBM DBM::restrict_for_firing(size_t transition_id, int alpha, int beta) const {
  return restrict_clock(transition_id + 1, alpha, beta);
}

void DBM::freeze_clock(size_t clock_idx) {
  if (clock_idx >= clock_count_ || clock_idx == 0) {
    return;
  }

  const auto it = std::lower_bound(frozen_clocks_.begin(), frozen_clocks_.end(), clock_idx);
  if (it == frozen_clocks_.end() || *it != clock_idx) {
    frozen_clocks_.insert(it, clock_idx);
  }
}

void DBM::unfreeze_clock(size_t clock_idx) {
  const auto it = std::lower_bound(frozen_clocks_.begin(), frozen_clocks_.end(), clock_idx);
  if (it != frozen_clocks_.end() && *it == clock_idx) {
    frozen_clocks_.erase(it);
  }
}

bool DBM::is_frozen(size_t clock_idx) const {
  return std::binary_search(frozen_clocks_.begin(), frozen_clocks_.end(), clock_idx);
}

void DBM::copy_clock_constraints(size_t clock_idx, DBM& target) const {
  if (clock_idx >= clock_count_) {
    return;
  }

  if (clock_idx >= target.size()) {
    target.resize(clock_idx + 1);
  }

  for (size_t i = 0; i < clock_count_; ++i) {
    if (i < target.size()) {
      target.set_constraint(clock_idx, i, matrix_[offset(clock_idx, i)]);
      target.set_constraint(i, clock_idx, matrix_[offset(i, clock_idx)]);
    }
  }

  if (is_frozen(clock_idx)) {
    target.freeze_clock(clock_idx);
  }
}

}  // namespace state_class
