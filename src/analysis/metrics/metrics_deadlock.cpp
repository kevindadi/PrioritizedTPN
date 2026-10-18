#include <algorithm>
#include <string>
#include <unordered_set>
#include <vector>

#include "analysis/metrics/metrics.h"
#include "model/petri.h"

namespace state_class {

namespace {

bool has_suffix(const std::string& value, const std::string& suffix) {
  return value.size() >= suffix.size() &&
         value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// Iterative Tarjan SCC over a masked subgraph. Returns the component id of
// every vertex (-1 when the vertex is outside the subgraph).
std::vector<long long> tarjan_components(const std::vector<std::vector<size_t>>& adjacency,
                                         const std::vector<char>& in_subgraph) {
  const size_t n = adjacency.size();
  std::vector<long long> index(n, -1);
  std::vector<long long> lowlink(n, 0);
  std::vector<long long> component(n, -1);
  std::vector<char> on_stack(n, 0);
  std::vector<size_t> tarjan_stack;
  long long next_index = 0;
  long long component_count = 0;

  struct Frame {
    size_t vertex;
    size_t next_edge;
  };

  std::vector<Frame> call_stack;

  for (size_t root = 0; root < n; ++root) {
    if (!in_subgraph[root] || index[root] >= 0) {
      continue;
    }

    call_stack.push_back({root, 0});
    index[root] = lowlink[root] = next_index++;
    tarjan_stack.push_back(root);
    on_stack[root] = 1;

    while (!call_stack.empty()) {
      Frame& frame = call_stack.back();
      const size_t v = frame.vertex;

      if (frame.next_edge < adjacency[v].size()) {
        const size_t w = adjacency[v][frame.next_edge++];
        if (index[w] < 0) {
          index[w] = lowlink[w] = next_index++;
          tarjan_stack.push_back(w);
          on_stack[w] = 1;
          call_stack.push_back({w, 0});
        } else if (on_stack[w]) {
          lowlink[v] = std::min(lowlink[v], index[w]);
        }
        continue;
      }

      call_stack.pop_back();
      if (!call_stack.empty()) {
        const size_t parent = call_stack.back().vertex;
        lowlink[parent] = std::min(lowlink[parent], lowlink[v]);
      }
      if (lowlink[v] == index[v]) {
        while (true) {
          const size_t w = tarjan_stack.back();
          tarjan_stack.pop_back();
          on_stack[w] = 0;
          component[w] = component_count;
          if (w == v) {
            break;
          }
        }
        ++component_count;
      }
    }
  }

  return component;
}

}  // namespace

void MetricsAnalyzer::compute_local_deadlocks(MetricsReport& report) {
  // Periodic-release transitions. TDG lowering records them explicitly; a
  // hand-written .ptpn has no metadata, so fall back to the lowering naming
  // convention (<task>_fire).
  std::unordered_set<size_t> all_periodic;
  for (const auto& [task, transition] : net_.periodic_transitions()) {
    (void)task;
    all_periodic.insert(transition.index());
  }
  if (all_periodic.empty()) {
    for (size_t t = 0; t < net_.num_transitions(); ++t) {
      const auto& transition = net_.get_transition(petri::TransitionId{t});
      if (has_suffix(transition.name, "_fire")) {
        all_periodic.insert(t);
      }
    }
  }

  for (size_t task_index = 0; task_index < report.tasks.size(); ++task_index) {
    TaskMetrics& metrics = report.tasks[task_index];
    if (task_index >= tasks_.size()) {
      continue;
    }
    const TaskTopology& task = tasks_[task_index];

    // T_p(a): periodic transitions that release a new instance of this task.
    std::unordered_set<size_t> periodic_of_task;
    const auto metadata_it = net_.periodic_transitions().find(task.name);
    if (metadata_it != net_.periodic_transitions().end()) {
      periodic_of_task.insert(metadata_it->second.index());
    } else {
      for (size_t t : all_periodic) {
        if (net_.get_transition(petri::TransitionId{t}).name == task.name + "_fire") {
          periodic_of_task.insert(t);
        }
      }
    }

    // Omega_a^pend: the current instance is still before its exit place.
    std::vector<char> pending(num_vertices_, 0);
    bool any_pending = false;
    for (size_t v = 0; v < num_vertices_; ++v) {
      pending[v] = task_pending(task, v) ? 1 : 0;
      any_pending = any_pending || pending[v];
    }
    if (!any_pending) {
      continue;
    }

    // Delta_a^pend: keep internal edges, except those labelled by T_p(a)
    // (they release the next instance and end the current analysis window).
    std::vector<std::vector<size_t>> adjacency(num_vertices_);
    std::vector<std::vector<std::pair<size_t, int>>> edges_with_labels(num_vertices_);
    for (size_t v = 0; v < num_vertices_; ++v) {
      if (!pending[v]) {
        continue;
      }
      for (const Edge& e : out_edges_[v]) {
        if (!pending[e.target]) {
          continue;
        }
        if (periodic_of_task.count(static_cast<size_t>(e.transition_id)) > 0) {
          continue;
        }
        adjacency[v].push_back(e.target);
        edges_with_labels[v].push_back({e.target, e.transition_id});
      }
    }

    const std::vector<long long> component = tarjan_components(adjacency, pending);

    // Component sizes and cyclic flags.
    std::vector<size_t> component_size;
    std::vector<char> component_cyclic;
    for (size_t v = 0; v < num_vertices_; ++v) {
      if (component[v] < 0) {
        continue;
      }
      const size_t c = static_cast<size_t>(component[v]);
      if (component_size.size() <= c) {
        component_size.resize(c + 1, 0);
        component_cyclic.resize(c + 1, 0);
      }
      component_size[c]++;
    }
    for (size_t v = 0; v < num_vertices_; ++v) {
      if (component[v] < 0) {
        continue;
      }
      const size_t c = static_cast<size_t>(component[v]);
      for (const auto& [target, transition] : edges_with_labels[v]) {
        (void)transition;
        if (component[target] == component[v]) {
          component_cyclic[c] = 1;  // self-loop or internal edge
          break;
        }
      }
    }

    // A cyclic SCC is a periodic local deadlock when it contains an internal
    // edge labelled by a positive-periodic transition of *another* task.
    std::vector<char> progress_component(component_size.size(), 0);
    for (size_t v = 0; v < num_vertices_; ++v) {
      if (component[v] < 0) {
        continue;
      }
      const size_t c = static_cast<size_t>(component[v]);
      for (const auto& [target, transition_id] : edges_with_labels[v]) {
        if (component[target] != component[v]) {
          continue;
        }
        const size_t t = static_cast<size_t>(transition_id);
        if (t >= net_.num_transitions() || all_periodic.count(t) == 0 ||
            periodic_of_task.count(t) > 0) {
          continue;
        }
        if (net_.get_transition(petri::TransitionId{t}).time_interval.effective_earliest() > 0) {
          progress_component[c] = 1;
          break;
        }
      }
    }

    for (size_t c = 0; c < component_size.size(); ++c) {
      const bool cyclic = component_size[c] > 1 || component_cyclic[c] != 0;
      if (cyclic && progress_component[c] != 0) {
        metrics.local_deadlock = true;
        metrics.local_deadlock_scc_size =
            std::max(metrics.local_deadlock_scc_size, component_size[c]);
      }
    }
  }
}

}  // namespace state_class
