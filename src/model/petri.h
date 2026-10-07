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

class PTPN {
 public:
  PTPN() = default;

  PlaceId add_place(const std::string& name, int capacity = 1, bool saturate = false) {
    places.emplace_back(std::to_string(places.size()), name, capacity, saturate);
    Pre.emplace_back(std::vector<int>(transitions.size(), 0));
    M0.push_back(0);
    for (auto& row : Post) {
      row.push_back(0);
    }
    return PlaceId{places.size() - 1};
  }

  TransitionId add_transition(const std::string& name,
                              const TimeInterval& interval = TimeInterval(), int priority = INT_MAX,
                              int core = -1, bool suspendable = false) {
    transitions.emplace_back(std::to_string(transitions.size()), name, interval, priority, core,
                             suspendable);
    for (auto& row : Pre) {
      row.push_back(0);
    }
    Post.emplace_back(std::vector<int>(places.size(), 0));
    pre_arcs.emplace_back();
    post_arcs.emplace_back();
    return TransitionId{transitions.size() - 1};
  }

  void set_pre_arc(PlaceId place, TransitionId transition, int weight = 1) {
    if (place.index() >= Pre.size() || transition.index() >= transitions.size()) {
      throw std::out_of_range("Invalid place or transition index");
    }
    Pre[place.index()][transition.index()] = weight;
    rebuild_sparse_arcs();
  }

  void set_post_arc(TransitionId transition, PlaceId place, int weight = 1) {
    if (transition.index() >= Post.size() || place.index() >= places.size()) {
      throw std::out_of_range("Invalid transition or place index");
    }
    Post[transition.index()][place.index()] = weight;
    rebuild_sparse_arcs();
  }

  void set_initial_marking(const Marking& marking) {
    if (marking.size() != places.size()) {
      throw std::invalid_argument("Marking size must match number of places");
    }
    M0 = marking;
  }

  void set_initial_marking(PlaceId place, int tokens) {
    if (place.index() >= places.size()) {
      throw std::out_of_range("Invalid place index");
    }
    if (tokens < 0) {
      throw std::invalid_argument("Token count cannot be negative");
    }
    M0[place.index()] = tokens;
  }

  [[nodiscard]] size_t num_places() const {
    return places.size();
  }

  [[nodiscard]] size_t num_transitions() const {
    return transitions.size();
  }

  [[nodiscard]] const Place& get_place(PlaceId place) const {
    if (place.index() >= places.size()) {
      throw std::out_of_range("Invalid place index");
    }
    return places[place.index()];
  }

  [[nodiscard]] const Transition& get_transition(TransitionId transition) const {
    if (transition.index() >= transitions.size()) {
      throw std::out_of_range("Invalid transition index");
    }
    return transitions[transition.index()];
  }

  [[nodiscard]] const Marking& get_marking() const {
    return M0;
  }

  [[nodiscard]] const std::vector<std::vector<int>>& get_pre_matrix() const {
    return Pre;
  }

  [[nodiscard]] const std::vector<std::vector<int>>& get_post_matrix() const {
    return Post;
  }

  static bool is_enabled(const Marking& M, const PTPN& net, TransitionId transition) {
    if (transition.index() >= net.transitions.size()) {
      throw std::out_of_range("Invalid transition index");
    }
    if (M.size() != net.places.size()) {
      throw std::invalid_argument("Marking size must match number of places");
    }

    for (const auto& [place_idx, weight] : net.pre_arcs[transition.index()]) {
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

    for (const auto& [place_idx, weight] : net.pre_arcs[transition.index()]) {
      new_marking[place_idx] -= weight;
    }

    for (const auto& [place_idx, weight] : net.post_arcs[transition.index()]) {
      new_marking[place_idx] += weight;
      const auto& place = net.places[place_idx];
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
    oss << "Places (" << places.size() << "):\n";
    for (size_t i = 0; i < places.size(); ++i) {
      oss << "  P" << i << ": " << places[i].name
          << " [capacity=" << (places[i].capacity == INF ? "∞" : std::to_string(places[i].capacity))
          << ", tokens=" << M0[i] << "]\n";
    }

    oss << "\nTransitions (" << transitions.size() << "):\n";
    for (size_t i = 0; i < transitions.size(); ++i) {
      oss << "  T" << i << ": " << transitions[i].name
          << " [time=" << transitions[i].time_interval.to_string()
          << ", priority=" << transitions[i].priority << ", core=" << transitions[i].core
          << ", suspendable=" << (transitions[i].suspendable ? "yes" : "no") << "]\n";
    }

    oss << "\nPre Matrix (" << Pre.size() << "x" << (Pre.empty() ? 0 : Pre[0].size()) << "):\n";
    for (size_t p = 0; p < Pre.size(); ++p) {
      oss << "  P" << p << ": ";
      for (size_t t = 0; t < Pre[p].size(); ++t) {
        oss << Pre[p][t] << " ";
      }
      oss << "\n";
    }

    oss << "\nPost Matrix (" << Post.size() << "x" << (Post.empty() ? 0 : Post[0].size()) << "):\n";
    for (size_t t = 0; t < Post.size(); ++t) {
      oss << "  T" << t << ": ";
      for (size_t p = 0; p < Post[t].size(); ++p) {
        oss << Post[t][p] << " ";
      }
      oss << "\n";
    }

    return oss.str();
  }

  // How many transitions may run simultaneously on `core_id`. Returns 0 to mean
  // "no bound": the control core (-1) is never resource-limited, and cores
  // without a registered parallelism keep the legacy highest-priority behaviour.
  [[nodiscard]] int parallelism_of_core(int core_id) const {
    if (core_id < 0) {
      return 0;
    }
    auto it = core_parallelism.find(core_id);
    return it == core_parallelism.end() ? 0 : it->second;
  }

  void rebuild_sparse_arcs() {
    pre_arcs.assign(transitions.size(), {});
    post_arcs.assign(transitions.size(), {});

    for (size_t p = 0; p < Pre.size(); ++p) {
      for (size_t t = 0; t < Pre[p].size(); ++t) {
        if (Pre[p][t] > 0) {
          pre_arcs[t].push_back({p, Pre[p][t]});
        }
      }
    }

    for (size_t t = 0; t < Post.size(); ++t) {
      for (size_t p = 0; p < Post[t].size(); ++p) {
        if (Post[t][p] > 0) {
          post_arcs[t].push_back({p, Post[t][p]});
        }
      }
    }
  }

  [[nodiscard]] bool verify_structure() const;

  std::vector<Place> places;
  std::vector<Transition> transitions;
  std::vector<std::vector<int>> Pre;
  std::vector<std::vector<int>> Post;
  std::vector<std::vector<std::pair<size_t, int>>> pre_arcs;
  std::vector<std::vector<std::pair<size_t, int>>> post_arcs;
  Marking M0;

  std::map<std::string, std::pair<NodeRef, NodeRef>> node_start_end_map;
  std::unordered_map<std::string, std::vector<NodeRef>> node_pn_map;
  std::vector<PlaceId> cpus_place;
  std::unordered_map<std::string, PlaceId> locks_place;
  // Maximum number of transitions that may run simultaneously on a real core
  // (i.e. the CPU's cores_per_cpu). Used by the scheduler's per-core priority
  // filter to bound parallelism. An absent entry means "no bound" (the legacy
  // behaviour of keeping every highest-priority transition).
  std::unordered_map<int, int> core_parallelism;
  // Per-task scheduling metadata keyed by TDG node name (see TaskInfo). Empty for
  // direct .ptpn input.
  std::unordered_map<std::string, TaskInfo> task_info;
  int node_index = 0;
};

}  // namespace petri

#endif