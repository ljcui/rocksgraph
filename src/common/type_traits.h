#pragma once

#define DISABLE_COPY(class_name)           \
  class_name(const class_name &) = delete; \
  class_name &operator=(const class_name &) = delete;

#define DISABLE_MOVE(class_name)      \
  class_name(class_name &&) = delete; \
  class_name &operator=(class_name &&) = delete;

#define LIKELY(x) __builtin_expect(!!(x), 1)