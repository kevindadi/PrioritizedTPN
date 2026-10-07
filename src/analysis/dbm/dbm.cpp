#include "analysis/dbm/dbm.h"

#include <algorithm>
#include <atomic>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>

#include "clock_state.h"

namespace state_class {

namespace {
std::atomic<size_t> g_dbm_minimize_calls{0};

int safe_add_bound(int lhs, int rhs) {
  if (lhs == INF_TIME || rhs == INF_TIME) {
    return INF_TIME;
  }

  if ((rhs > 0 && lhs > std::numeric_limits<int>::max() - rhs) ||
      (rhs < 0 && lhs < std::numeric_limits<int>::min() - rhs)) {
    return rhs > 0 ? INF_TIME : std::numeric_limits<int>::min();
  }

  return lhs + rhs;
}
}  // namespace

void reset_dbm_instrumentation() {
  g_dbm_minimize_calls.store(0, std::memory_order_relaxed);
}

DBMInstrumentation get_dbm_instrumentation() {
  return {
      g_dbm_minimize_calls.load(std::memory_order_relaxed),
  };
}

DBM::DBM(size_t size) : clock_count_(size), matrix_(size * size, INF_TIME) {
  if (size > 0) {
    for (size_t i = 0; i < size; ++i) {
      matrix_[offset(i, i)] = 0;
    }
    if (size > 1) {
      for (size_t i = 1; i < size; ++i) {
        matrix_[offset(i, 0)] = INF_TIME;
        matrix_[offset(0, i)] = 0;
      }
    }
  }
}

DBM::DBM(const DBM& other)
    : matrix_(other.matrix_),
      clock_count_(other.clock_count_),
      frozen_clocks_(other.frozen_clocks_) {}

DBM& DBM::operator=(const DBM& other) {
  if (this != &other) {
    matrix_ = other.matrix_;
    clock_count_ = other.clock_count_;
    frozen_clocks_ = other.frozen_clocks_;
  }
  return *this;
}

size_t DBM::offset(size_t i, size_t j) const {
  return i * clock_count_ + j;
}

void DBM::check_index(size_t i, size_t j) const {
  if (i >= clock_count_ || j >= clock_count_) {
    throw std::out_of_range("DBM index out of range");
  }
}

void DBM::set_constraint(size_t i, size_t j, int bound) {
  check_index(i, j);
  matrix_[offset(i, j)] = bound;
}

int DBM::get_constraint(size_t i, size_t j) const {
  check_index(i, j);
  return matrix_[offset(i, j)];
}

bool DBM::is_consistent() const {
  if (clock_count_ == 0)
    return true;

  for (size_t i = 0; i < clock_count_; ++i) {
    if (matrix_[offset(i, i)] < 0) {
      return false;
    }
  }

  DBM temp(*this);
  temp.minimize();

  for (size_t i = 0; i < clock_count_; ++i) {
    if (temp.matrix_[offset(i, i)] < 0) {
      return false;
    }
  }

  return true;
}

void DBM::minimize() {
  g_dbm_minimize_calls.fetch_add(1, std::memory_order_relaxed);

  if (clock_count_ == 0)
    return;

  for (size_t k = 0; k < clock_count_; ++k) {
    for (size_t i = 0; i < clock_count_; ++i) {
      const size_t ik = offset(i, k);
      if (matrix_[ik] == INF_TIME)
        continue;

      for (size_t j = 0; j < clock_count_; ++j) {
        const size_t kj = offset(k, j);
        if (matrix_[kj] == INF_TIME)
          continue;

        const int new_bound = safe_add_bound(matrix_[ik], matrix_[kj]);
        const size_t ij = offset(i, j);
        if (matrix_[ij] == INF_TIME || new_bound < matrix_[ij]) {
          matrix_[ij] = new_bound;
        }
      }
    }
  }
}

bool DBM::minimize_and_check() {
  minimize();
  for (size_t i = 0; i < clock_count_; ++i) {
    if (matrix_[offset(i, i)] < 0) {
      return false;
    }
  }
  return true;
}

bool DBM::tighten(size_t i, size_t j, int bound) {
  check_index(i, j);

  const size_t ij = offset(i, j);
  if (matrix_[ij] != INF_TIME && matrix_[ij] <= bound) {
    return true;
  }
  matrix_[ij] = bound;

  const int ji = matrix_[offset(j, i)];
  if (ji != INF_TIME && safe_add_bound(bound, ji) < 0) {
    matrix_[offset(i, i)] = safe_add_bound(bound, ji);
    return false;
  }

  // Re-close: any pair (a, b) can only improve via a -> i -> j -> b.
  for (size_t a = 0; a < clock_count_; ++a) {
    const int ai = matrix_[offset(a, i)];
    if (ai == INF_TIME) {
      continue;
    }
    const int a_via = safe_add_bound(ai, bound);
    if (a_via == INF_TIME) {
      continue;
    }
    for (size_t b = 0; b < clock_count_; ++b) {
      const int jb = matrix_[offset(j, b)];
      if (jb == INF_TIME) {
        continue;
      }
      const int candidate = safe_add_bound(a_via, jb);
      const size_t ab = offset(a, b);
      if (matrix_[ab] == INF_TIME || candidate < matrix_[ab]) {
        matrix_[ab] = candidate;
      }
    }
  }

  for (size_t a = 0; a < clock_count_; ++a) {
    if (matrix_[offset(a, a)] < 0) {
      return false;
    }
  }
  return true;
}

size_t DBM::add_clock() {
  size_t new_idx = clock_count_;
  resize(clock_count_ + 1);
  return new_idx;
}

void DBM::resize(size_t new_size) {
  if (new_size == clock_count_)
    return;

  const size_t old_size = clock_count_;
  const std::vector<int> old_matrix = matrix_;

  clock_count_ = new_size;
  matrix_.assign(new_size * new_size, INF_TIME);

  for (size_t i = 0; i < new_size; ++i) {
    matrix_[offset(i, i)] = 0;
  }
  if (new_size > 1) {
    for (size_t i = 1; i < new_size; ++i) {
      matrix_[offset(i, 0)] = INF_TIME;
      matrix_[offset(0, i)] = 0;
    }
  }

  const size_t preserved = std::min(old_size, new_size);
  for (size_t i = 0; i < preserved; ++i) {
    for (size_t j = 0; j < preserved; ++j) {
      matrix_[offset(i, j)] = old_matrix[i * old_size + j];
    }
  }

  for (size_t i = old_size; i < new_size; ++i) {
    initialize_clock(i);
  }
}

void DBM::initialize_clock(size_t clock_idx) {
  if (clock_idx >= clock_count_)
    return;

  matrix_[offset(clock_idx, clock_idx)] = 0;

  if (clock_idx == 0) {
    for (size_t i = 1; i < clock_count_; ++i) {
      matrix_[offset(0, i)] = 0;
      matrix_[offset(i, 0)] = INF_TIME;
    }
  } else {
    matrix_[offset(clock_idx, 0)] = INF_TIME;
    matrix_[offset(0, clock_idx)] = 0;

    for (size_t i = 1; i < clock_count_; ++i) {
      if (i != clock_idx) {
        matrix_[offset(clock_idx, i)] = INF_TIME;
        matrix_[offset(i, clock_idx)] = INF_TIME;
      }
    }
  }
}

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
      matrix_[offset(i, 0)] = safe_add_bound(current_upper, delta);
      changed = true;
    }

    const int current_lower = matrix_[offset(0, i)];
    if (current_lower != INF_TIME) {
      matrix_[offset(0, i)] = safe_add_bound(current_lower, -delta);
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

bool DBM::is_empty() const {
  return !is_consistent();
}

void DBM::prune() {
  minimize();
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

std::string DBM::to_string() const {
  if (clock_count_ == 0) {
    return "DBM(empty)";
  }

  std::ostringstream oss;
  oss << "DBM(size=" << clock_count_ << "):\n";
  oss << "   ";
  for (size_t j = 0; j < clock_count_; ++j) {
    oss << std::setw(8) << "x" << j;
  }
  oss << "\n";

  for (size_t i = 0; i < clock_count_; ++i) {
    oss << "x" << i << " ";
    for (size_t j = 0; j < clock_count_; ++j) {
      int value = matrix_[offset(i, j)];
      if (value == INF_TIME) {
        oss << std::setw(8) << "inf";
      } else {
        oss << std::setw(8) << value;
      }
    }
    oss << "\n";
  }

  return oss.str();
}

bool DBM::operator==(const DBM& other) const {
  if (clock_count_ != other.clock_count_) {
    return false;
  }

  return frozen_clocks_ == other.frozen_clocks_ && matrix_ == other.matrix_;
}

bool DBM::operator<(const DBM& other) const {
  if (clock_count_ != other.clock_count_) {
    return clock_count_ < other.clock_count_;
  }

  for (size_t i = 0; i < matrix_.size(); ++i) {
    if (matrix_[i] != other.matrix_[i]) {
      if (matrix_[i] == INF_TIME)
        return false;
      if (other.matrix_[i] == INF_TIME)
        return true;
      return matrix_[i] < other.matrix_[i];
    }
  }

  return frozen_clocks_ < other.frozen_clocks_;
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
