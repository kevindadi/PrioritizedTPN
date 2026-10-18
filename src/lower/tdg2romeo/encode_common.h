#ifndef TDG2ROMEO_ENCODE_COMMON_H
#define TDG2ROMEO_ENCODE_COMMON_H

#include <string>
#include <unordered_map>

#include "model/tdg.h"
#include "romeo_model.h"
#include "tdg_helpers.h"

namespace romeo {

struct EncodeContext {
  RomeoModelBuilder builder;
  std::unordered_map<std::string, NodeEndpoints> nodes;
  std::unordered_map<std::string, TaskEndpoints> tasks;
  bool explicit_core_places = true;
};

void add_core_places(EncodeContext& ctx, const tdg::TDG& tdg);

void add_lock_places(EncodeContext& ctx, const tdg::TDG& tdg);

void add_task_chain_scheduling_net(EncodeContext& ctx, const TaskNode& task);

void add_task_chain_inhibitor_arc(EncodeContext& ctx, const tdg::TDG& tdg, const TaskNode& task);

void add_fork_join_nodes(EncodeContext& ctx, const tdg::TDG& tdg);

void wire_edges(EncodeContext& ctx, const tdg::TDG& tdg);

void add_start_tokens(EncodeContext& ctx, const tdg::TDG& tdg);

void add_periodic_releases(EncodeContext& ctx, const tdg::TDG& tdg);

void add_end_consumers(EncodeContext& ctx, const tdg::TDG& tdg);

}  // namespace romeo

#endif  // TDG2ROMEO_ENCODE_COMMON_H
