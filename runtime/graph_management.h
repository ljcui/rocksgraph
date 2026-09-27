#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "proto/graph_replication.pb.h"

namespace rg {

struct ManagedGraphInfo {
  std::uint64_t id = 0;
  std::string name;
};

class GraphManagement {
 public:
  GraphManagement() = default;
  GraphManagement(const GraphManagement&) = delete;
  GraphManagement& operator=(const GraphManagement&) = delete;
  virtual ~GraphManagement() = default;

  virtual void CreateManagedGraph(std::string_view name) = 0;
  virtual void CreateManagedRaftGraph(
      std::string_view name, const meta::RaftNodeInfos& node_infos) = 0;
  virtual void ClearManagedGraph(std::string_view name) = 0;
  virtual void DeleteManagedGraph(std::string_view name) = 0;
  [[nodiscard]] virtual std::vector<ManagedGraphInfo> ListManagedGraphs()
      const = 0;
  [[nodiscard]] virtual meta::RaftNodeInfos ManagedGraphRaftNodeInfos(
      std::string_view name) = 0;
};

}  // namespace rg
