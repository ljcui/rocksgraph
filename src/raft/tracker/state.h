/**
 * Copyright 2025 AntGroup CO., Ltd.
 * Copyright 2015 The etcd Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 */

// written by botu.wzy, inspired by etcd raft

#pragma once
#include "../util.h"
namespace tracker {

enum class StateType {
  // StateProbe indicates a follower whose last index isn't known. Such a
  // follower is "probed" (i.e. an append sent periodically) to narrow down
  // its last index. In the ideal (and common) case, only one round of probing
  // is necessary as the follower will react with a hint. Followers that are
  // probed over extended periods of time are often offline.
  StateProbe = 0,
  // StateReplicate is the state steady in which a follower eagerly receives
  // log entries to append to its log.
  StateReplicate,
  // StateSnapshot indicates a follower that needs log entries not available
  // from the leader's Raft log. Such a follower needs a full snapshot to
  // return to StateReplicate.
  StateSnapshot
};

inline std::string ToString(StateType s) {
  switch (s) {
    case StateType::StateProbe:
      return "StateProbe";
    case StateType::StateReplicate:
      return "StateReplicate";
    case StateType::StateSnapshot:
      return "StateSnapshot";
    default:
      return "UnknownStateType";
  }
}

}  // namespace tracker