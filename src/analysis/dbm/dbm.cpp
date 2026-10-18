#include "analysis/dbm/dbm.h"

#include <algorithm>
#include <atomic>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>

#include "analysis/dbm/clock_state.h"
#include "analysis/dbm/dbm_internal.h"

namespace state_class {

namespace {
std::atomic<size_t> g_dbm_minimize_calls{0};

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

        const int new_bound = detail::safe_add_bound(matrix_[ik], matrix_[kj]);
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
  if (ji != INF_TIME && detail::safe_add_bound(bound, ji) < 0) {
    matrix_[offset(i, i)] = detail::safe_add_bound(bound, ji);
    return false;
  }

  // Re-close: any pair (a, b) can only improve via a -> i -> j -> b.
  for (size_t a = 0; a < clock_count_; ++a) {
    const int ai = matrix_[offset(a, i)];
    if (ai == INF_TIME) {
      continue;
    }
    const int a_via = detail::safe_add_bound(ai, bound);
    if (a_via == INF_TIME) {
      continue;
    }
    for (size_t b = 0; b < clock_count_; ++b) {
      const int jb = matrix_[offset(j, b)];
      if (jb == INF_TIME) {
        continue;
      }
      const int candidate = detail::safe_add_bound(a_via, jb);
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

bool DBM::is_empty() const {
  return !is_consistent();
}

void DBM::prune() {
  minimize();
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

}  // namespace state_class
