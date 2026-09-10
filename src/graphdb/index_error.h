/**
 * Copyright 2024 AntGroup CO., Ltd.
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

//
// Created by botu.wzy
//

#pragma once

#include <string>

#include "common/exceptions.h"

namespace graphdb {

template <typename IndexPtr>
[[noreturn]] inline void ThrowIfIndexUnavailable(const IndexPtr& index,
                                                 const std::string& index_name,
                                                 const char* index_kind) {
  if (index->state() == meta::IndexBuildState::FAILED) {
    if (index->meta().build_error().empty()) {
      THROW_CODE(IndexNotReady, "{} index [{}] build failed", index_kind,
                 index_name);
    }
    THROW_CODE(IndexNotReady, "{} index [{}] build failed: {}", index_kind,
               index_name, index->meta().build_error());
  }
  THROW_CODE(IndexNotReady, "{} index [{}] is still building", index_kind,
             index_name);
}

}  // namespace graphdb
