#ifndef PETRI_H
#define PETRI_H

#include <climits>
#include <iostream>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace petri {

constexpr int INF = std::numeric_limits<int>::max();
constexpr int kControlTransitionCore = -1;

// Strongly-typed handles into PTPN::places / PTPN::transitions. They are
// created by the net's add_* functions; `index()` exposes the underlying vector
// offset for the index-oriented analysis layer (markings, clock arrays).
struct PlaceId {
  size_t value = 0;

  [[nodiscard]] constexpr size_t index() const {
    return value;
  }
};

struct TransitionId {
  size_t value = 0;

  [[nodiscard]] constexpr size_t index() const {
    return value;
  }
};

inline bool operator==(PlaceId left, PlaceId right) {
  return left.value == right.value;
}

inline bool operator!=(PlaceId left, PlaceId right) {
  return !(left == right);
}

inline bool operator<(PlaceId left, PlaceId right) {
  return left.value < right.value;
}

inline bool operator==(TransitionId left, TransitionId right) {
  return left.value == right.value;
}

inline bool operator!=(TransitionId left, TransitionId right) {
  return !(left == right);
}

inline bool operator<(TransitionId left, TransitionId right) {
  return left.value < right.value;
}

// A tagged reference to either a place or a transition. The lowering layer
// deliberately mixes both in node_start_end_map / node_pn_map; NodeRef replaces
// the old "bare index plus size check" convention and turns a wrong-kind access
// into a loud runtime error instead of a silent matrix mix-up.
struct NodeRef {
  enum class Kind { Place, Transition };

  Kind kind = Kind::Place;
  size_t index = 0;

  static NodeRef of(PlaceId id) {
    return NodeRef{Kind::Place, id.value};
  }

  static NodeRef of(TransitionId id) {
    return NodeRef{Kind::Transition, id.value};
  }

  [[nodiscard]] bool is_place() const {
    return kind == Kind::Place;
  }

  [[nodiscard]] bool is_transition() const {
    return kind == Kind::Transition;
  }

  [[nodiscard]] PlaceId as_place() const {
    if (!is_place()) {
      throw std::runtime_error("NodeRef does not reference a place");
    }
    return PlaceId{index};
  }

  [[nodiscard]] TransitionId as_transition() const {
    if (!is_transition()) {
      throw std::runtime_error("NodeRef does not reference a transition");
    }
    return TransitionId{index};
  }
};

inline bool operator==(const NodeRef& left, const NodeRef& right) {
  return left.kind == right.kind && left.index == right.index;
}

inline bool operator!=(const NodeRef& left, const NodeRef& right) {
  return !(left == right);
}

// Overflow recording: `fire` clamps every overflowing place to capacity, but a
// NON-saturating place being clamped is an invalid behavior and is recorded so
// the metrics layer can report it. Saturating places clamp silently (expected
// single-server merges). Reset at the start of each reachability build.
void reset_overflow_recording();
void record_overflow(size_t place_idx);
[[nodiscard]] const std::vector<size_t>& overflowed_places();

struct TimeInterval {
  int earliest;
  int latest;
  bool left_open;
  bool right_open;

  TimeInterval(int e = 0, int l = INF, bool left_open = false, bool right_open = false)
      : earliest(e), latest(l), left_open(left_open), right_open(right_open) {
    if (earliest < 0) {
      throw std::invalid_argument("earliest time must be non-negative");
    }
    if (latest != INF && latest < earliest) {
      throw std::invalid_argument("latest time must be >= earliest time");
    }
  }

  [[nodiscard]] int effective_earliest() const {
    return left_open ? earliest + 1 : earliest;
  }

  [[nodiscard]] int effective_latest() const {
    if (latest == INF) {
      return INF;
    }
    return right_open ? latest - 1 : latest;
  }

  [[nodiscard]] bool has_non_empty_integer_domain() const {
    return effective_latest() == INF || effective_earliest() <= effective_latest();
  }

  [[nodiscard]] bool is_valid() const {
    return earliest >= 0 && (latest == INF || latest >= earliest) && has_non_empty_integer_domain();
  }

  [[nodiscard]] bool contains(int time) const {
    return time >= effective_earliest() &&
           (effective_latest() == INF || time <= effective_latest());
  }

  [[nodiscard]] std::string to_string() const {
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
};

struct Place {
  std::string id;
  std::string name;
  int capacity;
  // Saturating places absorb overflow: transitions producing into a full
  // saturating place stay enabled, and the token count is clamped at capacity
  // on firing (used for task places under single-server semantics, so periodic
  // releases are merged instead of blocking the release clock).
  bool saturate;

  Place(const std::string& id = "", const std::string& name = "", int cap = 1,
        bool saturate = false)
      : id(id), name(name), capacity(cap), saturate(saturate) {}
};

struct Transition {
  std::string id;
  std::string name;
  TimeInterval time_interval;
  int priority;
  int core;
  bool suspendable;

  Transition(const std::string& id = "", const std::string& name = "",
             const TimeInterval& interval = TimeInterval(), int priority = INT_MAX, int core = -1,
             bool suspendable = false)
      : id(id),
        name(name),
        time_interval(interval),
        priority(priority),
        core(core),
        suspendable(suspendable) {}
};

using Marking = std::vector<int>;

// Per-task scheduling metadata, attached to the lowered net so the metrics layer
// can reason about deadlines/periods/utilisation without re-reading the TDG. It
// is populated by the TDG->PTPN lowering; a direct `.ptpn` model leaves it empty
// and task-level metrics degrade gracefully.
struct TaskInfo {
  int core = -1;
  int priority = 0;
  int wcet = 0;      // sum of execution-segment upper bounds
  int bcet = 0;      // sum of execution-segment lower bounds
  int period = 0;    // 0 means "not periodic"
  int deadline = 0;  // 0 means "none"; defaults to period when implicit
  std::vector<std::string> locks;
};

// A Priority Timed Petri Net.
//
// Data is private: construction goes through the add_*/set_* methods, and
// readers use the accessors below. The lowering pipeline is the only writer;
// the analysis layer receives `const PTPN&` and can never mutate the net.
class PTPN {
 public:
  PTPN() = default;

  // --- Construction ------------------------------------------------------

  PlaceId add_place(const std::string& name, int capacity = 1, bool saturate = false) {
    places_.emplace_back(std::to_string(places_.size()), name, capacity, saturate);
    pre_matrix_.emplace_back(std::vector<int>(transitions_.size(), 0));
    m0_.push_back(0);
    for (auto& row : post_matrix_) {
      row.push_back(0);
    }
    return PlaceId{places_.size() - 1};
  }

  TransitionId add_transition(const std::string& name,
                              const TimeInterval& interval = TimeInterval(), int priority = INT_MAX,
                              int core = -1, bool suspendable = false) {
    transitions_.emplace_back(std::to_string(transitions_.size()), name, interval, priority, core,
                              suspendable);
    for (auto& row : pre_matrix_) {
      row.push_back(0);
    }
    post_matrix_.emplace_back(std::vector<int>(places_.size(), 0));
    pre_arcs_.emplace_back();
    post_arcs_.emplace_back();
    return TransitionId{transitions_.size() - 1};
  }

  void set_pre_arc(PlaceId place, TransitionId transition, int weight = 1) {
    if (place.index() >= pre_matrix_.size() || transition.index() >= transitions_.size()) {
      throw std::out_of_range("Invalid place or transition index");
    }
    pre_matrix_[place.index()][transition.index()] = weight;
    rebuild_sparse_arcs();
  }

  void set_post_arc(TransitionId transition, PlaceId place, int weight = 1) {
    if (transition.index() >= post_matrix_.size() || place.index() >= places_.size()) {
      throw std::out_of_range("Invalid transition or place index");
    }
    post_matrix_[transition.index()][place.index()] = weight;
    rebuild_sparse_arcs();
  }

  void set_initial_marking(const Marking& marking) {
    if (marking.size() != places_.size()) {
      throw std::invalid_argument("Marking size must match number of places");
    }
    m0_ = marking;
  }

  void set_initial_marking(PlaceId place, int tokens) {
    if (place.index() >= places_.size()) {
      throw std::out_of_range("Invalid place index");
    }
    if (tokens < 0) {
      throw std::invalid_argument("Token count cannot be negative");
    }
    m0_[place.index()] = tokens;
  }

  // --- Lowering metadata -------------------------------------------------

  // Flips the suspendable flag of an already-created transition (used by the
  // restart preemption strategy and by spin-lock lowering).
  void set_suspendable(TransitionId transition, bool suspendable) {
    if (transition.index() >= transitions_.size()) {
      throw std::out_of_range("Invalid transition index");
    }
    transitions_[transition.index()].suspendable = suspendable;
  }

  // Maximum number of transitions that may run simultaneously on a real core.
  void set_core_parallelism(int core_id, int parallelism) {
    core_parallelism_[core_id] = parallelism;
  }

  // Monotonic counter used to name generated nodes (Fork0, Join1, ...).
  size_t next_node_index() {
    return node_index_++;
  }

  void set_node_span(const std::string& name, NodeRef start, NodeRef end) {
    node_start_end_map_[name] = {start, end};
  }

  void set_task_chain(const std::string& task, std::vector<NodeRef> chain) {
    node_pn_map_[task] = std::move(chain);
  }

  void add_cpu_place(PlaceId place) {
    cpus_place_.push_back(place);
  }

  void set_lock_place(const std::string& lock, PlaceId place) {
    locks_place_[lock] = place;
  }

  void set_task_info(const std::string& task, TaskInfo info) {
    task_info_[task] = std::move(info);
  }

  void clear_lowering_metadata() {
    node_start_end_map_.clear();
    node_pn_map_.clear();
    cpus_place_.clear();
    core_parallelism_.clear();
    locks_place_.clear();
    task_info_.clear();
    node_index_ = 0;
  }

  // --- Read-only access --------------------------------------------------

  [[nodiscard]] size_t num_places() const {
    return places_.size();
  }

  [[nodiscard]] size_t num_transitions() const {
    return transitions_.size();
  }

  [[nodiscard]] const std::vector<Place>& places() const {
    return places_;
  }

  [[nodiscard]] const std::vector<Transition>& transitions() const {
    return transitions_;
  }

  [[nodiscard]] const Place& get_place(PlaceId place) const {
    if (place.index() >= places_.size()) {
      throw std::out_of_range("Invalid place index");
    }
    return places_[place.index()];
  }

  [[nodiscard]] const Transition& get_transition(TransitionId transition) const {
    if (transition.index() >= transitions_.size()) {
      throw std::out_of_range("Invalid transition index");
    }
    return transitions_[transition.index()];
  }

  [[nodiscard]] const Marking& get_marking() const {
    return m0_;
  }

  [[nodiscard]] const std::vector<std::vector<int>>& get_pre_matrix() const {
    return pre_matrix_;
  }

  [[nodiscard]] const std::vector<std::vector<int>>& get_post_matrix() const {
    return post_matrix_;
  }

  [[nodiscard]] const std::vector<std::vector<std::pair<size_t, int>>>& pre_arcs() const {
    return pre_arcs_;
  }

  [[nodiscard]] const std::vector<std::vector<std::pair<size_t, int>>>& post_arcs() const {
    return post_arcs_;
  }

  [[nodiscard]] const std::map<std::string, std::pair<NodeRef, NodeRef>>& node_start_end_map()
      const {
    return node_start_end_map_;
  }

  [[nodiscard]] const std::unordered_map<std::string, std::vector<NodeRef>>& node_pn_map() const {
    return node_pn_map_;
  }

  [[nodiscard]] const std::vector<PlaceId>& cpu_places() const {
    return cpus_place_;
  }

  [[nodiscard]] const std::unordered_map<std::string, PlaceId>& lock_places() const {
    return locks_place_;
  }

  [[nodiscard]] const std::unordered_map<std::string, TaskInfo>& task_info() const {
    return task_info_;
  }

  // How many transitions may run simultaneously on `core_id`. Returns 0 to mean
  // "no bound": the control core (-1) is never resource-limited, and cores
  // without a registered parallelism keep the legacy highest-priority behaviour.
  [[nodiscard]] int parallelism_of_core(int core_id) const {
    if (core_id < 0) {
      return 0;
    }
    auto it = core_parallelism_.find(core_id);
    return it == core_parallelism_.end() ? 0 : it->second;
  }

  // --- Semantics ---------------------------------------------------------

  static bool is_enabled(const Marking& M, const PTPN& net, TransitionId transition) {
    if (transition.index() >= net.transitions_.size()) {
      throw std::out_of_range("Invalid transition index");
    }
    if (M.size() != net.places_.size()) {
      throw std::invalid_argument("Marking size must match number of places");
    }

    for (const auto& [place_idx, weight] : net.pre_arcs_[transition.index()]) {
      if (M[place_idx] < weight) {
        return false;
      }
    }

    // NOTE: enabling is input-driven only (classic TPN semantics). Successor
    // places never gate the transition; overflow on non-saturating places is
    // reported by the metrics layer, not by disabling the producer.
    return true;
  }

  static Marking fire(const Marking& M, const PTPN& net, TransitionId transition) {
    if (!is_enabled(M, net, transition)) {
      throw std::runtime_error("Transition is not enabled");
    }

    Marking new_marking = M;

    for (const auto& [place_idx, weight] : net.pre_arcs_[transition.index()]) {
      new_marking[place_idx] -= weight;
    }

    for (const auto& [place_idx, weight] : net.post_arcs_[transition.index()]) {
      new_marking[place_idx] += weight;
      const auto& place = net.places_[place_idx];
      if (place.capacity != INF && new_marking[place_idx] > place.capacity) {
        // Firing always happens (enabling is input-driven). Overflow is clamped
        // to capacity; a NON-saturating place being clamped is recorded as an
        // invalid behavior by the metrics layer.
        if (!place.saturate) {
          record_overflow(place_idx);
        }
        new_marking[place_idx] = place.capacity;
      }
    }

    return new_marking;
  }

  [[nodiscard]] std::string to_string() const {
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

  [[nodiscard]] bool verify_structure() const;

 private:
  void rebuild_sparse_arcs() {
    pre_arcs_.assign(transitions_.size(), {});
    post_arcs_.assign(transitions_.size(), {});

    for (size_t p = 0; p < pre_matrix_.size(); ++p) {
      for (size_t t = 0; t < pre_matrix_[p].size(); ++t) {
        if (pre_matrix_[p][t] > 0) {
          pre_arcs_[t].push_back({p, pre_matrix_[p][t]});
        }
      }
    }

    for (size_t t = 0; t < post_matrix_.size(); ++t) {
      for (size_t p = 0; p < post_matrix_[t].size(); ++p) {
        if (post_matrix_[t][p] > 0) {
          post_arcs_[t].push_back({p, post_matrix_[t][p]});
        }
      }
    }
  }

  std::vector<Place> places_;
  std::vector<Transition> transitions_;
  std::vector<std::vector<int>> pre_matrix_;
  std::vector<std::vector<int>> post_matrix_;
  std::vector<std::vector<std::pair<size_t, int>>> pre_arcs_;
  std::vector<std::vector<std::pair<size_t, int>>> post_arcs_;
  Marking m0_;

  std::map<std::string, std::pair<NodeRef, NodeRef>> node_start_end_map_;
  std::unordered_map<std::string, std::vector<NodeRef>> node_pn_map_;
  std::vector<PlaceId> cpus_place_;
  std::unordered_map<std::string, PlaceId> locks_place_;
  std::unordered_map<int, int> core_parallelism_;
  std::unordered_map<std::string, TaskInfo> task_info_;
  size_t node_index_ = 0;
};

}  // namespace petri

#endif
