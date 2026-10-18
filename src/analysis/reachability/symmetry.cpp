#include "analysis/reachability/symmetry.h"

#include <algorithm>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <set>
#include <sstream>
#include <unordered_map>
#include <utility>

#include "analysis/dbm/clock_state.h"

namespace state_class {

namespace {

std::string yes_no(bool value) {
  return value ? "1" : "0";
}

std::string bound_text(int value) {
  return value == petri::INF ? std::string("inf") : std::to_string(value);
}

std::string interval_text(const petri::TimeInterval& interval) {
  std::ostringstream oss;
  oss << (interval.left_open ? "(" : "[") << interval.earliest << "," << bound_text(interval.latest)
      << (interval.right_open ? ")" : "]");
  return oss.str();
}

// Core ids are only compared for equality by the scheduler, so symmetry may
// relabel them. The canonical signature records the observable core behaviour
// (control vs real, and real-core parallelism) rather than the numeric id.
std::string core_behavior_text(const petri::PTPN& net, int core) {
  if (core < 0) {
    return "ctl";
  }
  return "real:" + std::to_string(net.parallelism_of_core(core));
}

std::string place_text(const petri::PTPN& net, size_t place_index) {
  const auto& place = net.get_place(petri::PlaceId{place_index});
  std::ostringstream oss;
  oss << bound_text(place.capacity) << "," << yes_no(place.saturate) << ","
      << net.get_marking()[place_index];
  return oss.str();
}

std::string transition_text(const petri::PTPN& net, size_t transition_index) {
  const auto& transition = net.get_transition(petri::TransitionId{transition_index});
  std::ostringstream oss;
  oss << interval_text(transition.time_interval) << ",pr=" << transition.priority
      << ",cb=" << core_behavior_text(net, transition.core)
      << ",su=" << yes_no(transition.suspendable);
  return oss.str();
}

struct TaskDescriptor {
  std::string name;
  std::vector<petri::NodeRef> chain;
  std::vector<size_t> chain_places;
  std::vector<size_t> chain_transitions;
  petri::TaskInfo info;
  std::set<std::string> lock_names;
  std::string chain_signature;
  std::string signature;
};

bool valid_chain(const petri::PTPN& net, const std::vector<petri::NodeRef>& chain) {
  if (chain.size() < 3 || chain.size() % 2 == 0) {
    return false;
  }
  for (size_t i = 0; i < chain.size(); ++i) {
    const bool want_place = (i % 2 == 0);
    if (chain[i].is_place() != want_place) {
      return false;
    }
    if (want_place) {
      if (chain[i].as_place().index() >= net.num_places()) {
        return false;
      }
    } else if (chain[i].as_transition().index() >= net.num_transitions()) {
      return false;
    }
  }
  return true;
}

// Structural context shared by signatures and swap verification.
struct NetContext {
  const petri::PTPN& net;
  std::vector<int> place_owner;       // task index, -1 unowned, -2 conflict
  std::vector<int> transition_owner;  // task index, -1 unowned, -2 conflict
  std::unordered_map<size_t, std::string> lock_name_of_place;
  std::vector<char> is_core_place;

  explicit NetContext(const petri::PTPN& ptpn)
      : net(ptpn),
        place_owner(ptpn.num_places(), -1),
        transition_owner(ptpn.num_transitions(), -1),
        is_core_place(ptpn.num_places(), 0) {
    for (const auto& [lock_name, place] : ptpn.lock_places()) {
      if (place.index() < ptpn.num_places()) {
        lock_name_of_place[place.index()] = lock_name;
      }
    }
    for (const petri::PlaceId place : ptpn.cpu_places()) {
      if (place.index() < is_core_place.size()) {
        is_core_place[place.index()] = 1;
      }
    }
  }

  void register_task(size_t task_index, const TaskDescriptor& task) {
    for (size_t place : task.chain_places) {
      if (place_owner[place] == -1) {
        place_owner[place] = static_cast<int>(task_index);
      } else if (place_owner[place] != static_cast<int>(task_index)) {
        place_owner[place] = -2;
      }
    }
    for (size_t transition : task.chain_transitions) {
      if (transition_owner[transition] == -1) {
        transition_owner[transition] = static_cast<int>(task_index);
      } else if (transition_owner[transition] != static_cast<int>(task_index)) {
        transition_owner[transition] = -2;
      }
    }
  }
};

std::string chain_signature_of(const petri::PTPN& net, const TaskDescriptor& task) {
  const auto periodic_it = net.periodic_transitions().find(task.name);
  std::ostringstream oss;
  oss << "cb=" << core_behavior_text(net, task.info.core) << ";pr=" << task.info.priority
      << ";pe=" << task.info.period << ";dl=" << task.info.deadline << ";chain=";
  for (const petri::NodeRef& node : task.chain) {
    if (node.is_place()) {
      oss << "P(" << place_text(net, node.as_place().index()) << ");";
      continue;
    }
    const size_t transition = node.as_transition().index();
    oss << "T(" << transition_text(net, transition);
    if (periodic_it != net.periodic_transitions().end() &&
        periodic_it->second.index() == transition) {
      oss << ",periodic";
    }
    oss << ");";
  }
  return oss.str();
}

std::string external_place_text(const NetContext& context, const std::vector<TaskDescriptor>& tasks,
                                size_t task_index, size_t place) {
  const auto lock_it = context.lock_name_of_place.find(place);
  if (lock_it != context.lock_name_of_place.end()) {
    return "lock(" + lock_it->second + ")";
  }
  if (context.is_core_place[place]) {
    return "coreplace(" + place_text(context.net, place) + ")";
  }
  const int owner = place < context.place_owner.size() ? context.place_owner[place] : -1;
  if (owner >= 0 && static_cast<size_t>(owner) != task_index) {
    const TaskDescriptor& owner_task = tasks[static_cast<size_t>(owner)];
    std::string role = "chain";
    if (place == owner_task.chain_places.front()) {
      role = "entry";
    } else if (place == owner_task.chain_places.back()) {
      role = "exit";
    }
    return "taskplace(" + role + "," + owner_task.chain_signature + ")";
  }
  return "auxplace(" + place_text(context.net, place) + ")";
}

std::string external_transition_text(const NetContext& context,
                                     const std::vector<TaskDescriptor>& tasks, size_t task_index,
                                     size_t transition) {
  const TaskDescriptor& task = tasks[task_index];
  auto in_chain = [&](size_t place) {
    return std::find(task.chain_places.begin(), task.chain_places.end(), place) !=
           task.chain_places.end();
  };

  std::vector<std::string> attached;
  for (const auto& [place, weight] : context.net.pre_arcs()[transition]) {
    if (in_chain(place)) {
      continue;
    }
    attached.push_back("pre:" + std::to_string(weight) + ":" +
                       external_place_text(context, tasks, task_index, place));
  }
  for (const auto& [place, weight] : context.net.post_arcs()[transition]) {
    if (in_chain(place)) {
      continue;
    }
    attached.push_back("post:" + std::to_string(weight) + ":" +
                       external_place_text(context, tasks, task_index, place));
  }
  std::sort(attached.begin(), attached.end());

  std::ostringstream oss;
  oss << "T(" << transition_text(context.net, transition) << "){";
  for (const std::string& item : attached) {
    oss << item << "|";
  }
  oss << "}";
  return oss.str();
}

std::string build_task_signature(const NetContext& context,
                                 const std::vector<TaskDescriptor>& tasks, size_t task_index) {
  const TaskDescriptor& task = tasks[task_index];
  auto in_chain_transition = [&](size_t transition) {
    return std::find(task.chain_transitions.begin(), task.chain_transitions.end(), transition) !=
           task.chain_transitions.end();
  };
  auto in_chain_place = [&](size_t place) {
    return std::find(task.chain_places.begin(), task.chain_places.end(), place) !=
           task.chain_places.end();
  };

  std::vector<std::string> external;
  for (const petri::NodeRef& node : task.chain) {
    if (!node.is_place()) {
      const size_t transition = node.as_transition().index();
      for (const auto& [place, weight] : context.net.pre_arcs()[transition]) {
        if (!in_chain_place(place)) {
          external.push_back("in:" + std::to_string(weight) + ":" +
                             external_place_text(context, tasks, task_index, place));
        }
      }
      for (const auto& [place, weight] : context.net.post_arcs()[transition]) {
        if (!in_chain_place(place)) {
          external.push_back("out:" + std::to_string(weight) + ":" +
                             external_place_text(context, tasks, task_index, place));
        }
      }
      continue;
    }

    const size_t place = node.as_place().index();
    for (size_t transition = 0; transition < context.net.num_transitions(); ++transition) {
      if (in_chain_transition(transition)) {
        continue;
      }
      const int pre_weight = context.net.get_pre_matrix()[place][transition];
      if (pre_weight > 0) {
        external.push_back("outT:" + std::to_string(pre_weight) + ":" +
                           external_transition_text(context, tasks, task_index, transition));
      }
      const int post_weight = context.net.get_post_matrix()[transition][place];
      if (post_weight > 0) {
        external.push_back("inT:" + std::to_string(post_weight) + ":" +
                           external_transition_text(context, tasks, task_index, transition));
      }
    }
  }
  std::sort(external.begin(), external.end());

  std::ostringstream oss;
  oss << task.chain_signature << ";locks=";
  for (const std::string& lock : task.lock_names) {
    oss << lock << ",";
  }
  oss << ";ext=";
  for (const std::string& item : external) {
    oss << item << "|";
  }
  return oss.str();
}

std::vector<TaskDescriptor> build_task_descriptors(const petri::PTPN& net, NetContext& context) {
  std::vector<TaskDescriptor> tasks;
  std::vector<std::string> names;
  for (const auto& entry : net.node_pn_map()) {
    if (net.task_info().find(entry.first) == net.task_info().end()) {
      continue;
    }
    if (!valid_chain(net, entry.second)) {
      continue;
    }
    names.push_back(entry.first);
  }
  std::sort(names.begin(), names.end());

  for (const std::string& name : names) {
    TaskDescriptor task;
    task.name = name;
    task.chain = net.node_pn_map().at(name);
    task.info = net.task_info().at(name);
    for (const petri::NodeRef& node : task.chain) {
      if (node.is_place()) {
        task.chain_places.push_back(node.as_place().index());
      } else {
        task.chain_transitions.push_back(node.as_transition().index());
      }
    }
    for (const std::string& lock : task.info.locks) {
      task.lock_names.insert(lock);
    }
    // Lock resources bound to the chain are part of the task's lock set even
    // when the lowering metadata is absent.
    for (size_t transition : task.chain_transitions) {
      for (const auto& [place, weight] : net.pre_arcs()[transition]) {
        (void)weight;
        const auto lock_it = context.lock_name_of_place.find(place);
        if (lock_it != context.lock_name_of_place.end()) {
          task.lock_names.insert(lock_it->second);
        }
      }
      for (const auto& [place, weight] : net.post_arcs()[transition]) {
        (void)weight;
        const auto lock_it = context.lock_name_of_place.find(place);
        if (lock_it != context.lock_name_of_place.end()) {
          task.lock_names.insert(lock_it->second);
        }
      }
    }
    tasks.push_back(std::move(task));
  }

  for (size_t i = 0; i < tasks.size(); ++i) {
    tasks[i].chain_signature = chain_signature_of(net, tasks[i]);
    context.register_task(i, tasks[i]);
  }
  for (size_t i = 0; i < tasks.size(); ++i) {
    tasks[i].signature = build_task_signature(context, tasks, i);
  }
  return tasks;
}

// True when no lock name used by the group is also used by a task outside it.
bool locks_are_isolated(const std::vector<TaskDescriptor>& tasks,
                        const std::vector<size_t>& members) {
  std::set<std::string> group_locks;
  for (size_t member : members) {
    group_locks.insert(tasks[member].lock_names.begin(), tasks[member].lock_names.end());
  }
  if (group_locks.empty()) {
    return true;
  }
  for (size_t i = 0; i < tasks.size(); ++i) {
    if (std::find(members.begin(), members.end(), i) != members.end()) {
      continue;
    }
    for (const std::string& lock : tasks[i].lock_names) {
      if (group_locks.count(lock) > 0) {
        return false;
      }
    }
  }
  return true;
}

bool same_place_attributes(const petri::PTPN& net, size_t left, size_t right) {
  const auto& a = net.get_place(petri::PlaceId{left});
  const auto& b = net.get_place(petri::PlaceId{right});
  return a.capacity == b.capacity && a.saturate == b.saturate &&
         net.get_marking()[left] == net.get_marking()[right];
}

bool same_transition_attributes(const petri::PTPN& net, size_t left, size_t right) {
  const auto& a = net.get_transition(petri::TransitionId{left});
  const auto& b = net.get_transition(petri::TransitionId{right});
  return a.time_interval.earliest == b.time_interval.earliest &&
         a.time_interval.latest == b.time_interval.latest &&
         a.time_interval.left_open == b.time_interval.left_open &&
         a.time_interval.right_open == b.time_interval.right_open && a.priority == b.priority &&
         a.suspendable == b.suspendable;
}

// Builds the full node permutation induced by swapping two task chains. The
// chain nodes are mapped directly; auxiliary nodes (consumers, period cells,
// core resources, mirrored dependency edges) are matched structurally. The
// result is accepted only if verify_permutation() proves it is an automorphism.
class SwapBuilder {
 public:
  SwapBuilder(const petri::PTPN& net, const NetContext& context, size_t left, size_t right)
      : net_(net),
        context_(context),
        left_(left),
        right_(right),
        num_places_(net.num_places()),
        num_transitions_(net.num_transitions()),
        place_map_(num_places_),
        transition_map_(num_transitions_),
        place_fixed_(num_places_, 0),
        transition_fixed_(num_transitions_, 0) {
    std::iota(place_map_.begin(), place_map_.end(), 0);
    std::iota(transition_map_.begin(), transition_map_.end(), 0);
  }

  std::optional<NetPermutation> build(const std::vector<TaskDescriptor>& tasks) {
    if (tasks[left_].chain.size() != tasks[right_].chain.size()) {
      return std::nullopt;
    }

    for (size_t i = 0; i < tasks[left_].chain.size(); ++i) {
      const petri::NodeRef& a = tasks[left_].chain[i];
      const petri::NodeRef& b = tasks[right_].chain[i];
      if (a.kind != b.kind) {
        return std::nullopt;
      }
      const bool ok = a.is_place() ? assign_place(a.index, b.index)
                                   : assign_transition(a.index, b.index);
      if (!ok) {
        return std::nullopt;
      }
    }

    // Resolve auxiliary nodes round by round. Transitions are matched first so
    // that their incident consumer/period places become determined, then
    // places, until a fixed point. Ambiguous nodes are left for the final
    // self-assignment; verification rejects the swap if they really had to
    // move.
    for (size_t round = 0; round < num_places_ + num_transitions_ + 1; ++round) {
      const bool progress_transitions = match_transitions(tasks);
      const bool progress_places = match_places(tasks);
      if (!progress_transitions && !progress_places) {
        break;
      }
    }

    for (size_t place = 0; place < num_places_; ++place) {
      if (!place_fixed_[place]) {
        place_map_[place] = place;
        place_fixed_[place] = 1;
      }
    }
    for (size_t transition = 0; transition < num_transitions_; ++transition) {
      if (!transition_fixed_[transition]) {
        transition_map_[transition] = transition;
        transition_fixed_[transition] = 1;
      }
    }

    NetPermutation permutation{place_map_, transition_map_};
    if (!verify_permutation(net_, permutation)) {
      return std::nullopt;
    }
    return permutation;
  }

 private:
  bool assign_place(size_t left, size_t right) {
    if (left == right) {
      if (place_fixed_[left]) {
        return place_map_[left] == left;
      }
      place_fixed_[left] = 1;
      return true;
    }
    if (place_fixed_[left]) {
      return place_map_[left] == right;
    }
    if (place_fixed_[right]) {
      return place_map_[right] == left;
    }
    place_map_[left] = right;
    place_map_[right] = left;
    place_fixed_[left] = 1;
    place_fixed_[right] = 1;
    return true;
  }

  bool assign_transition(size_t left, size_t right) {
    if (left == right) {
      if (transition_fixed_[left]) {
        return transition_map_[left] == left;
      }
      transition_fixed_[left] = 1;
      return true;
    }
    if (transition_fixed_[left]) {
      return transition_map_[left] == right;
    }
    if (transition_fixed_[right]) {
      return transition_map_[right] == left;
    }
    transition_map_[left] = right;
    transition_map_[right] = left;
    transition_fixed_[left] = 1;
    transition_fixed_[right] = 1;
    return true;
  }

  [[nodiscard]] bool adjacent_places_are_mapped(size_t transition) const {
    for (const auto& [place, weight] : net_.pre_arcs()[transition]) {
      (void)weight;
      if (!place_fixed_[place]) {
        return false;
      }
    }
    for (const auto& [place, weight] : net_.post_arcs()[transition]) {
      (void)weight;
      if (!place_fixed_[place]) {
        return false;
      }
    }
    return true;
  }

  // True when the node is adjacent to a node that has already been mapped.
  // Matching is restricted to such nodes: a candidate pair in a disconnected
  // component that contains neither swapped task would otherwise pair two
  // *foreign* tasks with each other, turning the intended independent swaps of
  // two symmetric groups into a single simultaneous permutation. Leaving
  // foreign components fixed keeps each generator local; the final
  // verify_permutation() call still proves the whole mapping is correct.
  [[nodiscard]] bool transition_has_fixed_neighbor(size_t transition) const {
    for (const auto& [place, weight] : net_.pre_arcs()[transition]) {
      (void)weight;
      if (place_fixed_[place]) {
        return true;
      }
    }
    for (const auto& [place, weight] : net_.post_arcs()[transition]) {
      (void)weight;
      if (place_fixed_[place]) {
        return true;
      }
    }
    return false;
  }

  [[nodiscard]] bool place_has_fixed_neighbor(size_t place) const {
    for (size_t transition = 0; transition < num_transitions_; ++transition) {
      if (transition_fixed_[transition] &&
          (net_.get_pre_matrix()[place][transition] > 0 ||
           net_.get_post_matrix()[transition][place] > 0)) {
        return true;
      }
    }
    return false;
  }

  [[nodiscard]] bool adjacent_transitions_are_mapped(size_t place) const {
    for (size_t transition = 0; transition < num_transitions_; ++transition) {
      if ((net_.get_pre_matrix()[place][transition] > 0 ||
           net_.get_post_matrix()[transition][place] > 0) &&
          !transition_fixed_[transition]) {
        return false;
      }
    }
    return true;
  }

  [[nodiscard]] std::vector<std::string> unmapped_place_descriptors(
      size_t transition) const {
    std::vector<std::string> descriptors;
    for (const auto& [place, weight] : net_.pre_arcs()[transition]) {
      if (!place_fixed_[place]) {
        descriptors.push_back("pre:" + std::to_string(weight) + ":" + place_text(net_, place));
      }
    }
    for (const auto& [place, weight] : net_.post_arcs()[transition]) {
      if (!place_fixed_[place]) {
        descriptors.push_back("post:" + std::to_string(weight) + ":" + place_text(net_, place));
      }
    }
    std::sort(descriptors.begin(), descriptors.end());
    return descriptors;
  }

  [[nodiscard]] std::vector<std::string> unmapped_transition_descriptors(size_t place) const {
    std::vector<std::string> descriptors;
    for (size_t transition = 0; transition < num_transitions_; ++transition) {
      if (transition_fixed_[transition]) {
        continue;
      }
      const int pre_weight = net_.get_pre_matrix()[place][transition];
      if (pre_weight > 0) {
        descriptors.push_back("out:" + std::to_string(pre_weight) + ":" +
                              transition_text(net_, transition));
      }
      const int post_weight = net_.get_post_matrix()[transition][place];
      if (post_weight > 0) {
        descriptors.push_back("in:" + std::to_string(post_weight) + ":" +
                              transition_text(net_, transition));
      }
    }
    std::sort(descriptors.begin(), descriptors.end());
    return descriptors;
  }

  [[nodiscard]] bool transition_pair_ok(size_t left, size_t right) const {
    if (!same_transition_attributes(net_, left, right)) {
      return false;
    }
    const auto& pre = net_.get_pre_matrix();
    const auto& post = net_.get_post_matrix();
    for (size_t place = 0; place < num_places_; ++place) {
      if (!place_fixed_[place]) {
        continue;
      }
      const size_t image = place_map_[place];
      if (pre[place][left] != pre[image][right] || post[left][place] != post[right][image]) {
        return false;
      }
    }
    return unmapped_place_descriptors(left) == unmapped_place_descriptors(right);
  }

  [[nodiscard]] bool place_pair_ok(size_t left, size_t right) const {
    if (!same_place_attributes(net_, left, right)) {
      return false;
    }
    const auto& pre = net_.get_pre_matrix();
    const auto& post = net_.get_post_matrix();
    for (size_t transition = 0; transition < num_transitions_; ++transition) {
      if (!transition_fixed_[transition]) {
        continue;
      }
      const size_t image = transition_map_[transition];
      if (pre[left][transition] != pre[right][image] ||
          post[transition][left] != post[image][right]) {
        return false;
      }
    }
    return unmapped_transition_descriptors(left) == unmapped_transition_descriptors(right);
  }

  bool match_transitions(const std::vector<TaskDescriptor>& tasks) {
    (void)tasks;
    std::vector<std::vector<size_t>> candidates(num_transitions_);
    std::vector<char> self_candidate(num_transitions_, 0);
    for (size_t transition = 0; transition < num_transitions_; ++transition) {
      if (transition_fixed_[transition]) {
        continue;
      }
      for (size_t other = 0; other < num_transitions_; ++other) {
        if (other == transition || transition_fixed_[other]) {
          continue;
        }
        if (transition_pair_ok(transition, other)) {
          candidates[transition].push_back(other);
        }
      }
      if (adjacent_places_are_mapped(transition) && transition_pair_ok(transition, transition)) {
        self_candidate[transition] = 1;
      }
    }

    bool progress = false;
    for (size_t transition = 0; transition < num_transitions_; ++transition) {
      if (transition_fixed_[transition] || candidates[transition].size() != 1) {
        continue;
      }
      const size_t other = candidates[transition].front();
      if (transition_fixed_[other] || candidates[other].size() != 1 ||
          candidates[other].front() != transition) {
        continue;
      }
      if (!transition_has_fixed_neighbor(transition) &&
          !transition_has_fixed_neighbor(other)) {
        continue;
      }
      progress = assign_transition(transition, other) || progress;
    }
    for (size_t transition = 0; transition < num_transitions_; ++transition) {
      if (transition_fixed_[transition] || !candidates[transition].empty() ||
          !self_candidate[transition]) {
        continue;
      }
      if (adjacent_places_are_mapped(transition) && transition_pair_ok(transition, transition)) {
        progress = assign_transition(transition, transition) || progress;
      }
    }
    return progress;
  }

  bool match_places(const std::vector<TaskDescriptor>& tasks) {
    (void)tasks;
    std::vector<std::vector<size_t>> candidates(num_places_);
    std::vector<char> self_candidate(num_places_, 0);
    for (size_t place = 0; place < num_places_; ++place) {
      if (place_fixed_[place]) {
        continue;
      }
      for (size_t other = 0; other < num_places_; ++other) {
        if (other == place || place_fixed_[other]) {
          continue;
        }
        if (place_pair_ok(place, other)) {
          candidates[place].push_back(other);
        }
      }
      if (adjacent_transitions_are_mapped(place) && place_pair_ok(place, place)) {
        self_candidate[place] = 1;
      }
    }

    bool progress = false;
    for (size_t place = 0; place < num_places_; ++place) {
      if (place_fixed_[place] || candidates[place].size() != 1) {
        continue;
      }
      const size_t other = candidates[place].front();
      if (place_fixed_[other] || candidates[other].size() != 1 ||
          candidates[other].front() != place) {
        continue;
      }
      if (!place_has_fixed_neighbor(place) && !place_has_fixed_neighbor(other)) {
        continue;
      }
      progress = assign_place(place, other) || progress;
    }
    for (size_t place = 0; place < num_places_; ++place) {
      if (place_fixed_[place] || !candidates[place].empty() || !self_candidate[place]) {
        continue;
      }
      if (adjacent_transitions_are_mapped(place) && place_pair_ok(place, place)) {
        progress = assign_place(place, place) || progress;
      }
    }
    return progress;
  }

  [[nodiscard]] static bool verify_permutation(const petri::PTPN& net,
                                               const NetPermutation& permutation) {
    const size_t num_places = net.num_places();
    const size_t num_transitions = net.num_transitions();
    if (permutation.places.size() != num_places ||
        permutation.transitions.size() != num_transitions) {
      return false;
    }
    // The generated task swaps are involutions; requiring that keeps the group
    // closure simple and catches malformed auxiliary matchings.
    for (size_t place = 0; place < num_places; ++place) {
      if (permutation.places[place] >= num_places ||
          permutation.places[permutation.places[place]] != place) {
        return false;
      }
    }
    for (size_t transition = 0; transition < num_transitions; ++transition) {
      if (permutation.transitions[transition] >= num_transitions ||
          permutation.transitions[permutation.transitions[transition]] != transition) {
        return false;
      }
    }

    for (size_t place = 0; place < num_places; ++place) {
      if (!same_place_attributes(net, place, permutation.places[place])) {
        return false;
      }
    }
    for (size_t transition = 0; transition < num_transitions; ++transition) {
      if (!same_transition_attributes(net, transition,
                                      permutation.transitions[transition])) {
        return false;
      }
    }

    const auto& pre = net.get_pre_matrix();
    const auto& post = net.get_post_matrix();
    for (size_t place = 0; place < num_places; ++place) {
      const size_t image_place = permutation.places[place];
      for (size_t transition = 0; transition < num_transitions; ++transition) {
        const size_t image_transition = permutation.transitions[transition];
        if (pre[place][transition] != pre[image_place][image_transition] ||
            post[transition][place] != post[image_transition][image_place]) {
          return false;
        }
      }
    }

    // Core ids may be relabelled, but the relabelling must be a bijection and
    // must preserve control-core status and core parallelism.
    std::map<int, int> sigma;
    for (size_t transition = 0; transition < num_transitions; ++transition) {
      const int core = net.get_transition(petri::TransitionId{transition}).core;
      const int image_core =
          net.get_transition(petri::TransitionId{permutation.transitions[transition]}).core;
      const auto it = sigma.find(core);
      if (it != sigma.end() && it->second != image_core) {
        return false;
      }
      sigma[core] = image_core;
    }
    // Periodic metadata is keyed by task name and is not permuted by the
    // quotient, so a periodic release transition must stay in place. In a
    // genuine task swap this normally fails the arc checks above as well; the
    // explicit test keeps the invariant local and obvious.
    for (const auto& [task_name, transition] : net.periodic_transitions()) {
      (void)task_name;
      const size_t index = transition.index();
      if (index >= num_transitions || permutation.transitions[index] != index) {
        return false;
      }
    }

    std::set<int> images;
    for (const auto& [core, image_core] : sigma) {
      if ((core < 0) != (image_core < 0)) {
        return false;
      }
      if (core >= 0 && net.parallelism_of_core(core) != net.parallelism_of_core(image_core)) {
        return false;
      }
      if (!images.insert(image_core).second) {
        return false;
      }
    }
    return true;
  }

  const petri::PTPN& net_;
  const NetContext& context_;
  size_t left_;
  size_t right_;
  size_t num_places_;
  size_t num_transitions_;
  std::vector<size_t> place_map_;
  std::vector<size_t> transition_map_;
  std::vector<char> place_fixed_;
  std::vector<char> transition_fixed_;
};

NetPermutation compose(const NetPermutation& first, const NetPermutation& second) {
  NetPermutation result;
  result.places.resize(first.places.size());
  result.transitions.resize(first.transitions.size());
  for (size_t i = 0; i < first.places.size(); ++i) {
    result.places[i] = second.places[first.places[i]];
  }
  for (size_t i = 0; i < first.transitions.size(); ++i) {
    result.transitions[i] = second.transitions[first.transitions[i]];
  }
  return result;
}

std::vector<size_t> permutation_key(const NetPermutation& permutation) {
  std::vector<size_t> key;
  key.reserve(permutation.places.size() + permutation.transitions.size());
  key.insert(key.end(), permutation.places.begin(), permutation.places.end());
  key.insert(key.end(), permutation.transitions.begin(), permutation.transitions.end());
  return key;
}

SymmetryGroupElement make_group_element(NetPermutation permutation) {
  SymmetryGroupElement element;
  element.place_inverse.resize(permutation.places.size());
  element.transition_inverse.resize(permutation.transitions.size());
  for (size_t i = 0; i < permutation.places.size(); ++i) {
    element.place_inverse[permutation.places[i]] = i;
  }
  for (size_t i = 0; i < permutation.transitions.size(); ++i) {
    element.transition_inverse[permutation.transitions[i]] = i;
  }
  element.forward = std::move(permutation);
  return element;
}

// Computes the closure of `generators`. Every element is multiplied by every
// generator (not only by the one that was added last), which is what makes the
// result a group: with one-generator extensions the word order would be lost
// and a set of star transpositions such as (01),(02),(03) would close to 8
// elements instead of S_4. Returns an empty vector when the closure would
// exceed `TaskSymmetry::kMaxGroupOrder`.
std::vector<SymmetryGroupElement> close_group(
    const NetPermutation& identity, const std::vector<NetPermutation>& generators) {
  std::vector<SymmetryGroupElement> elements{make_group_element(identity)};
  std::set<std::vector<size_t>> seen;
  seen.insert(permutation_key(identity));

  for (size_t index = 0; index < elements.size(); ++index) {
    for (const NetPermutation& generator : generators) {
      NetPermutation candidate = compose(elements[index].forward, generator);
      if (seen.insert(permutation_key(candidate)).second) {
        elements.push_back(make_group_element(std::move(candidate)));
        if (elements.size() > TaskSymmetry::kMaxGroupOrder) {
          return {};
        }
      }
    }
  }
  return elements;
}

std::vector<long long> orbit_key(const StateClass& state) {
  std::vector<long long> key;
  key.push_back(static_cast<long long>(state.marking.size()));
  for (int token : state.marking) {
    key.push_back(token);
  }
  key.push_back(static_cast<long long>(state.clock_vars.size()));
  for (const ClockVar& var : state.clock_vars) {
    key.push_back(static_cast<long long>(var.kind));
    key.push_back(var.transition == NO_TRANSITION ? -1
                                                  : static_cast<long long>(var.transition));
    key.push_back(var.task == NO_TASK ? -1 : static_cast<long long>(var.task));
  }
  key.push_back(static_cast<long long>(state.priority_enabled.size()));
  for (size_t transition : state.priority_enabled) {
    key.push_back(static_cast<long long>(transition));
  }
  key.push_back(static_cast<long long>(state.suspended.size()));
  for (size_t transition : state.suspended) {
    key.push_back(static_cast<long long>(transition));
  }
  key.push_back(static_cast<long long>(state.zone.size()));
  for (int bound : state.zone.raw_matrix()) {
    key.push_back(bound);
  }
  return key;
}

struct DisjointSet {
  std::vector<size_t> parent;

  explicit DisjointSet(size_t size) : parent(size) {
    std::iota(parent.begin(), parent.end(), 0);
  }

  size_t find(size_t value) {
    while (parent[value] != value) {
      parent[value] = parent[parent[value]];
      value = parent[value];
    }
    return value;
  }

  void unite(size_t left, size_t right) {
    const size_t left_root = find(left);
    const size_t right_root = find(right);
    if (left_root != right_root) {
      parent[right_root] = left_root;
    }
  }
};

}  // namespace

TaskSymmetry::TaskSymmetry(const petri::PTPN& net) : net_(net) {
  // Always install the identity so group_order() is at least one and
  // canonicalize() is well-defined for trivial groups.
  NetPermutation identity;
  identity.places.resize(net.num_places());
  identity.transitions.resize(net.num_transitions());
  std::iota(identity.places.begin(), identity.places.end(), 0);
  std::iota(identity.transitions.begin(), identity.transitions.end(), 0);
  elements_.push_back(make_group_element(identity));

  NetContext context(net);
  std::vector<TaskDescriptor> tasks = build_task_descriptors(net, context);
  if (tasks.size() < 2) {
    return;
  }

  std::map<std::string, std::vector<size_t>> by_signature;
  for (size_t i = 0; i < tasks.size(); ++i) {
    by_signature[tasks[i].signature].push_back(i);
  }

  std::vector<std::vector<size_t>> candidate_groups;
  for (const auto& [signature, members] : by_signature) {
    (void)signature;
    if (members.size() < 2) {
      continue;
    }
    // Conservative scope: only pairwise distinct real cores. Keep one task per
    // core (members are name-sorted, so the choice is deterministic).
    std::map<int, size_t> one_per_core;
    for (size_t member : members) {
      const int core = tasks[member].info.core;
      if (core < 0) {
        continue;
      }
      one_per_core.emplace(core, member);
    }
    if (one_per_core.size() < 2) {
      continue;
    }
    std::vector<size_t> selected;
    selected.reserve(one_per_core.size());
    for (const auto& [core, member] : one_per_core) {
      (void)core;
      selected.push_back(member);
    }
    if (!locks_are_isolated(tasks, selected)) {
      continue;
    }
    candidate_groups.push_back(std::move(selected));
  }
  if (candidate_groups.empty()) {
    return;
  }

  // Only verified swaps may generate the canonicalization group. A spanning
  // tree of the admissible-pair graph yields transpositions whose generated
  // group is S_k on that component, and each generator is an automorphism.
  std::vector<NetPermutation> generators;
  std::vector<std::pair<size_t, size_t>> accepted_edges;
  for (const std::vector<size_t>& group : candidate_groups) {
    const size_t size = group.size();
    std::vector<std::vector<std::optional<NetPermutation>>> admissible(
        size, std::vector<std::optional<NetPermutation>>(size));
    for (size_t i = 0; i < size; ++i) {
      for (size_t j = i + 1; j < size; ++j) {
        SwapBuilder builder(net_, context, group[i], group[j]);
        std::optional<NetPermutation> permutation = builder.build(tasks);
        if (permutation.has_value()) {
          admissible[i][j] = permutation;
          admissible[j][i] = permutation;
        }
      }
    }

    // Connected components of the admissible-pair graph.
    std::vector<char> visited(size, 0);
    for (size_t root = 0; root < size; ++root) {
      if (visited[root]) {
        continue;
      }
      std::vector<size_t> component;
      std::vector<std::pair<size_t, size_t>> tree_edges;
      std::vector<size_t> stack{root};
      visited[root] = 1;
      while (!stack.empty()) {
        const size_t node = stack.back();
        stack.pop_back();
        component.push_back(node);
        for (size_t other = 0; other < size; ++other) {
          if (other == node || !admissible[node][other].has_value()) {
            continue;
          }
          if (!visited[other]) {
            visited[other] = 1;
            stack.push_back(other);
            tree_edges.emplace_back(node, other);
          }
        }
      }
      if (component.size() < 2) {
        continue;
      }
      for (const auto& [left, right] : tree_edges) {
        const NetPermutation& permutation = admissible[left][right].value();
        generators.push_back(permutation);
        accepted_edges.emplace_back(group[left], group[right]);
      }
    }
  }

  if (generators.empty()) {
    return;
  }

  // Add generators one by one and rebuild the closure from scratch, so a
  // generator that would push the group over the cap is simply skipped while
  // the previously accepted subgroup stays in place.
  std::vector<NetPermutation> accepted_generators;
  for (const NetPermutation& generator : generators) {
    accepted_generators.push_back(generator);
    std::vector<SymmetryGroupElement> closed = close_group(identity, accepted_generators);
    if (closed.empty()) {
      accepted_generators.pop_back();
      continue;
    }
    elements_ = std::move(closed);
  }

  // Equivalence classes actually generated by the accepted swaps (used by the
  // metrics layer to pool per-task values).
  DisjointSet classes(tasks.size());
  for (const auto& [left, right] : accepted_edges) {
    classes.unite(left, right);
  }
  std::map<size_t, std::vector<std::string>> group_names;
  for (size_t i = 0; i < tasks.size(); ++i) {
    group_names[classes.find(i)].push_back(tasks[i].name);
  }
  for (auto& [root, names] : group_names) {
    (void)root;
    if (names.size() < 2) {
      continue;
    }
    std::sort(names.begin(), names.end());
    task_groups_.push_back(std::move(names));
  }
  std::sort(task_groups_.begin(), task_groups_.end());
}

StateClass TaskSymmetry::apply(const StateClass& state,
                               const SymmetryGroupElement& element) const {
  StateClass out;
  out.marking.assign(net_.num_places(), 0);
  for (size_t place = 0; place < net_.num_places(); ++place) {
    out.marking[element.forward.places[place]] = state.marking[place];
  }

  auto map_set = [&](const TransitionSet& source, TransitionSet& target) {
    target.clear();
    target.reserve(source.size());
    for (size_t transition : source) {
      target.push_back(element.forward.transitions[transition]);
    }
    std::sort(target.begin(), target.end());
  };
  map_set(state.struct_enabled, out.struct_enabled);
  map_set(state.priority_enabled, out.priority_enabled);
  map_set(state.suspended, out.suspended);

  // Match the layout actually used by the analysis: suspension clocks are
  // present only when they have not been projected out (or, more directly,
  // when the source class still carries one).
  const bool keep_suspension_clocks =
      std::any_of(state.clock_vars.begin(), state.clock_vars.end(),
                  [](const ClockVar& variable) {
                    return variable.kind == ClockKind::Suspension;
                  });
  const bool include_release_clocks =
      std::any_of(state.clock_vars.begin(), state.clock_vars.end(),
                  [](const ClockVar& variable) {
                    return variable.kind == ClockKind::Release;
                  });
  build_clock_layout(out, net_, keep_suspension_clocks, include_release_clocks);

  const size_t clock_count = out.clock_vars.size();
  DBM zone(clock_count);
  std::vector<size_t> source_index(clock_count, 0);
  for (size_t i = 1; i < clock_count; ++i) {
    const ClockVar& variable = out.clock_vars[i];
    if (variable.kind == ClockKind::Release) {
      // Release clocks are keyed by task ordinal, but NetPermutation does not
      // carry a task permutation. The supported EDF class is single-core, so
      // no non-identity task permutation can be generated; if one ever reaches
      // here, returning the original state is the safe (non-reducing) fallback.
      return state;
    }
    const size_t source_transition = element.transition_inverse[variable.transition];
    const int source_clock = variable.kind == ClockKind::Execution
                                 ? state.exec_index(source_transition)
                                 : state.susp_index(source_transition);
    if (source_clock <= 0) {
      // A verified automorphism always maps a required clock; returning the
      // original state is the safe fallback if an invariant is ever violated.
      return state;
    }
    source_index[i] = static_cast<size_t>(source_clock);
  }
  for (size_t i = 0; i < clock_count; ++i) {
    for (size_t j = 0; j < clock_count; ++j) {
      zone.set_constraint(
          i, j, state.zone.get_constraint(source_index[i], source_index[j]));
    }
  }
  out.zone = std::move(zone);
  out.elapsed_time = state.elapsed_time;
  return out;
}

StateClass TaskSymmetry::canonicalize(StateClass state) const {
  if (elements_.size() <= 1) {
    return state;
  }

  // The orbit key starts with the marking, so a group element that does not
  // produce the lexicographically minimal marking can never produce the
  // minimal key. Select those elements first (cheap) and build the full
  // permuted state class only for them; this avoids constructing an entire
  // DBM for every element when the marking alone already discriminates.
  std::vector<int> candidate_marking(state.marking.size(), 0);
  std::vector<int> best_marking;
  std::vector<size_t> candidates;
  candidates.reserve(elements_.size());
  for (size_t index = 0; index < elements_.size(); ++index) {
    const std::vector<size_t>& places = elements_[index].forward.places;
    for (size_t place = 0; place < state.marking.size(); ++place) {
      candidate_marking[places[place]] = state.marking[place];
    }
    if (candidates.empty() || candidate_marking < best_marking) {
      best_marking = candidate_marking;
      candidates.clear();
      candidates.push_back(index);
    } else if (candidate_marking == best_marking) {
      candidates.push_back(index);
    }
  }

  StateClass best = apply(state, elements_[candidates.front()]);
  std::vector<long long> best_key = orbit_key(best);
  for (size_t i = 1; i < candidates.size(); ++i) {
    StateClass candidate = apply(state, elements_[candidates[i]]);
    std::vector<long long> candidate_key = orbit_key(candidate);
    if (candidate_key < best_key) {
      best = std::move(candidate);
      best_key = std::move(candidate_key);
    }
  }
  best.id = 0;
  return best;
}

}  // namespace state_class
