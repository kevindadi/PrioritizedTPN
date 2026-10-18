#include "scheduling_net.h"

#include "encode_common.h"

namespace romeo {

RomeoModel build_scheduling_net_model(const tdg::TDG& tdg, const RomeoExportOptions& opts) {
  EncodeContext ctx;
  ctx.explicit_core_places = opts.explicit_core_places;

  add_core_places(ctx, tdg);
  add_lock_places(ctx, tdg);

  for (const auto& [name, node_type] : tdg.nodes_type) {
    if (const auto* task = as_task_node(node_type)) {
      (void)name;
      add_task_chain_scheduling_net(ctx, *task);
    }
  }

  add_fork_join_nodes(ctx, tdg);
  wire_edges(ctx, tdg);
  add_start_tokens(ctx, tdg);
  add_periodic_releases(ctx, tdg);
  add_end_consumers(ctx, tdg);

  return ctx.builder.build();
}

}  // namespace romeo
