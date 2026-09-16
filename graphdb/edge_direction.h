//
// Created by botu.wzy
//

#pragma once
namespace graphdb {
// Do not change the order
// BOTH enumerates each incident edge once; a self-loop is not duplicated.
enum class EdgeDirection : char { OUTGOING = 0, INCOMING, BOTH };
}  // namespace graphdb
