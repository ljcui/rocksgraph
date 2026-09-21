//
// Created by botu.wzy
//

#pragma once

#include <string>

#include "common/exception.h"

namespace graphdb {

template <typename IndexPtr>
[[noreturn]] inline void ThrowIfIndexUnavailable(const IndexPtr& index,
                                                 const std::string& index_name,
                                                 const char* index_kind) {
  if (index->state() == meta::IndexBuildState::FAILED) {
    if (index->meta().build_error().empty()) {
      RG_THROW(common::ErrorCode::IndexNotReady, "{} index [{}] build failed",
               index_kind, index_name);
    }
    RG_THROW(common::ErrorCode::IndexNotReady, "{} index [{}] build failed: {}",
             index_kind, index_name, index->meta().build_error());
  }
  RG_THROW(common::ErrorCode::IndexNotReady, "{} index [{}] is still building",
           index_kind, index_name);
}

}  // namespace graphdb
