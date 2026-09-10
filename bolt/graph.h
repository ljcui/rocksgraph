#pragma once
#include <any>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace bolt {
struct Node {
  int64_t id;                       // id of this Node.
  std::string elementId;            // elementId of this Node.
  std::vector<std::string> labels;  // labels attached to this Node.
  std::unordered_map<std::string, std::any> props;  // Properties of this Node.
};

struct Relationship {
  int64_t id;             // id of this Relationship.
  std::string elementId;  // elementId of this Relationship.
  int64_t startId;        // id of the start Node of this Relationship.
  std::string
      startElementId;  // elementId of the start Node of this Relationship.
  int64_t endId;       // id of the End Node of this Relationship.
  std::string endElementId;  // elementId of the End Node of this Relationship.
  std::string type;          // type of this Relationship.
  std::unordered_map<std::string, std::any>
      props;  // Properties of this Relationship.
};

// Path represents a directed sequence of relationships between two nodes.
// This generally represents a traversal or walk through a graph and maintains a
// direction separate from that of any relationships traversed. It is allowed to
// be of size 0, meaning there are no relationships in it. In this case, it
// contains only a single node which is both the start and the End of the path.
struct Path {
  std::vector<Node> nodes;  // All the nodes in the path.
  std::vector<Relationship> relationships;
};

}  // namespace bolt
