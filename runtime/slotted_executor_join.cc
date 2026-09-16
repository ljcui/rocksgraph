#include <algorithm>
#include <unordered_map>
#include <unordered_set>

#include "runtime/expression_evaluator.h"
#include "runtime/slotted_executor_internal.h"

namespace rg::slotted {

class LeftOuterHashJoinOperator final : public PullOperator {
 public:
  LeftOuterHashJoinOperator(const PhysicalPlanNode &node, RuntimeState &state,
                            std::unique_ptr<PullOperator> lhs,
                            std::unique_ptr<PullOperator> rhs)
      : node_(&node),
        data_(&OperatorData<LeftOuterHashJoinOp>(node)),
        state_(&state),
        lhs_(std::move(lhs)),
        rhs_(std::move(rhs)) {}
  ~LeftOuterHashJoinOperator() override { Close(); }

  bool Next(SlottedRow *row) override {
    RG_CHECK(row != nullptr, common::InvalidArgumentError,
             "output row is null");
    state_->CheckCancelled();
    if (closed_) {
      return false;
    }
    if (!initialized_) {
      Initialize();
    }
    while (true) {
      state_->CheckCancelled();
      if (!left_.has_value()) {
        SlottedRow left(node_->children[0]->output_slots);
        if (!lhs_->Next(&left)) {
          Close();
          return false;
        }
        left_.emplace(std::move(left));
        matches_ = nullptr;
        index_ = 0;
        matched_ = false;
        if (auto key = NodeJoinKey(*left_, data_->left_key_slots);
            key.has_value()) {
          const auto found = buckets_.find(*key);
          if (found != buckets_.end()) {
            matches_ = &found->second;
          }
        }
      }
      while (matches_ != nullptr && index_ < matches_->size()) {
        SlottedRow output(node_->output_slots);
        CopyMappings(*left_, &output, node_->child_mappings[0]);
        if (MergeMappings(build_rows_[(*matches_)[index_++]], &output,
                          node_->child_mappings[1])) {
          matched_ = true;
          *row = std::move(output);
          return true;
        }
      }
      if (!matched_) {
        SlottedRow output(node_->output_slots);
        CopyMappings(*left_, &output, node_->child_mappings[0]);
        for (const std::size_t slot : data_->nullable_output_slots) {
          if (!output.IsInitialized(slot)) {
            output.SetNull(slot);
          }
        }
        left_.reset();
        *row = std::move(output);
        return true;
      }
      left_.reset();
    }
  }

  void Close() noexcept override {
    if (closed_) {
      return;
    }
    if (lhs_ != nullptr) {
      lhs_->Close();
    }
    if (rhs_ != nullptr) {
      rhs_->Close();
    }
    buckets_.clear();
    build_rows_.clear();
    left_.reset();
    matches_ = nullptr;
    state_->memory_tracker.Release(reserved_bytes_);
    reserved_bytes_ = 0;
    closed_ = true;
  }

 private:
  using Bucket = std::vector<std::size_t>;

  void Initialize() {
    initialized_ = true;
    while (true) {
      SlottedRow right(node_->children[1]->output_slots);
      if (!rhs_->Next(&right)) {
        rhs_->Close();
        return;
      }
      state_->CheckCancelled();
      std::optional<CompositeValueKey> key =
          NodeJoinKey(right, data_->right_key_slots);
      if (!key.has_value()) {
        continue;
      }

      const std::size_t row_bytes = right.EstimatedHeapUsage();
      state_->memory_tracker.Reserve(row_bytes);
      reserved_bytes_ += row_bytes;
      const std::size_t row_index = build_rows_.size();
      build_rows_.push_back(std::move(right));

      auto [found, inserted] = buckets_.try_emplace(std::move(*key));
      if (inserted) {
        const std::size_t key_bytes = EstimatedKeyHeapUsage(found->first);
        state_->memory_tracker.Reserve(key_bytes);
        reserved_bytes_ += key_bytes;
      }
      const std::size_t old_capacity = found->second.capacity();
      found->second.push_back(row_index);
      const std::size_t new_capacity = found->second.capacity();
      if (new_capacity > old_capacity) {
        const std::size_t bucket_bytes =
            (new_capacity - old_capacity) * sizeof(std::size_t);
        state_->memory_tracker.Reserve(bucket_bytes);
        reserved_bytes_ += bucket_bytes;
      }
    }
  }

  const PhysicalPlanNode *node_;
  const LeftOuterHashJoinOp *data_ = nullptr;
  RuntimeState *state_;
  std::unique_ptr<PullOperator> lhs_;
  std::unique_ptr<PullOperator> rhs_;
  std::unordered_map<CompositeValueKey, Bucket, ValueHash, ValueEqual> buckets_;
  std::vector<SlottedRow> build_rows_;
  std::optional<SlottedRow> left_;
  const Bucket *matches_ = nullptr;
  std::size_t index_ = 0;
  std::size_t reserved_bytes_ = 0;
  bool matched_ = false;
  bool initialized_ = false;
  bool closed_ = false;
};

class CachedNestedLoopState final {
 public:
  CachedNestedLoopState(const PhysicalPlanNode &node, RuntimeState &state,
                        std::unique_ptr<PullOperator> lhs,
                        std::unique_ptr<PullOperator> rhs,
                        std::size_t cached_child)
      : node_(&node),
        state_(&state),
        lhs_(std::move(lhs)),
        rhs_(std::move(rhs)),
        cached_child_(cached_child) {}

  ~CachedNestedLoopState() { Close(); }

  [[nodiscard]] bool Next(const SlottedRow **lhs, const SlottedRow **rhs) {
    RG_CHECK(lhs != nullptr && rhs != nullptr, common::InvalidArgumentError,
             "nested-loop output rows are null");
    state_->CheckCancelled();
    if (closed_) {
      return false;
    }
    if (!initialized_) {
      Initialize();
    }
    if (cached_rows_.empty()) {
      Close();
      return false;
    }

    while (true) {
      if (!probe_row_.has_value()) {
        SlottedRow probe(node_->children[ProbeChild()]->output_slots);
        if (!Source(ProbeChild())->Next(&probe)) {
          Close();
          return false;
        }
        probe_row_.emplace(std::move(probe));
        cached_offset_ = 0;
      }
      if (cached_offset_ < cached_rows_.size()) {
        const SlottedRow &cached = cached_rows_[cached_offset_++];
        *lhs = CachedChild() == 0 ? &cached : &*probe_row_;
        *rhs = CachedChild() == 0 ? &*probe_row_ : &cached;
        return true;
      }
      probe_row_.reset();
    }
  }

  void Close() noexcept {
    if (closed_) {
      return;
    }
    if (lhs_ != nullptr) {
      lhs_->Close();
    }
    if (rhs_ != nullptr) {
      rhs_->Close();
    }
    probe_row_.reset();
    cached_rows_.clear();
    state_->memory_tracker.Release(reserved_bytes_);
    reserved_bytes_ = 0;
    closed_ = true;
  }

 private:
  [[nodiscard]] std::size_t CachedChild() const { return cached_child_; }

  [[nodiscard]] std::size_t ProbeChild() const { return 1U - CachedChild(); }

  PullOperator *Source(std::size_t child) const {
    return child == 0 ? lhs_.get() : rhs_.get();
  }

  void Initialize() {
    initialized_ = true;
    RG_CHECK(CachedChild() < 2, common::InternalError,
             "invalid nested-loop cached child");
    PullOperator *source = Source(CachedChild());
    while (true) {
      SlottedRow input(node_->children[CachedChild()]->output_slots);
      if (!source->Next(&input)) {
        source->Close();
        return;
      }
      state_->CheckCancelled();
      const std::size_t bytes = input.EstimatedHeapUsage();
      state_->memory_tracker.Reserve(bytes);
      reserved_bytes_ += bytes;
      cached_rows_.push_back(std::move(input));
    }
  }

  const PhysicalPlanNode *node_ = nullptr;
  RuntimeState *state_ = nullptr;
  std::unique_ptr<PullOperator> lhs_;
  std::unique_ptr<PullOperator> rhs_;
  std::vector<SlottedRow> cached_rows_;
  std::optional<SlottedRow> probe_row_;
  std::size_t cached_child_ = 1;
  std::size_t cached_offset_ = 0;
  std::size_t reserved_bytes_ = 0;
  bool initialized_ = false;
  bool closed_ = false;
};

class CartesianProductOperator final : public PullOperator {
 public:
  CartesianProductOperator(const PhysicalPlanNode &node, RuntimeState &state,
                           std::unique_ptr<PullOperator> lhs,
                           std::unique_ptr<PullOperator> rhs)
      : node_(&node),
        data_(&OperatorData<CartesianProductOp>(node)),
        state_(&state),
        loop_(node, state, std::move(lhs), std::move(rhs),
              data_->cached_child) {}

  ~CartesianProductOperator() override { Close(); }

  [[nodiscard]] bool Next(SlottedRow *row) override {
    RG_CHECK(row != nullptr, common::InvalidArgumentError,
             "output row is null");
    const SlottedRow *lhs = nullptr;
    const SlottedRow *rhs = nullptr;
    while (loop_.Next(&lhs, &rhs)) {
      SlottedRow output(node_->output_slots);
      if (!MergeMappings(*lhs, &output, node_->child_mappings[0]) ||
          !MergeMappings(*rhs, &output, node_->child_mappings[1])) {
        continue;
      }
      *row = std::move(output);
      return true;
    }
    return false;
  }

  void Close() noexcept override { loop_.Close(); }

 private:
  const PhysicalPlanNode *node_ = nullptr;
  const CartesianProductOp *data_ = nullptr;
  RuntimeState *state_ = nullptr;
  CachedNestedLoopState loop_;
};

class PredicateJoinOperator final : public PullOperator {
 public:
  PredicateJoinOperator(const PhysicalPlanNode &node, RuntimeState &state,
                        std::unique_ptr<PullOperator> lhs,
                        std::unique_ptr<PullOperator> rhs)
      : node_(&node),
        data_(&OperatorData<PredicateJoinOp>(node)),
        state_(&state),
        loop_(node, state, std::move(lhs), std::move(rhs),
              data_->cached_child) {}

  ~PredicateJoinOperator() override { Close(); }

  [[nodiscard]] bool Next(SlottedRow *row) override {
    RG_CHECK(row != nullptr, common::InvalidArgumentError,
             "output row is null");
    const SlottedRow *lhs = nullptr;
    const SlottedRow *rhs = nullptr;
    while (loop_.Next(&lhs, &rhs)) {
      SlottedRow output(node_->output_slots);
      if (!MergeMappings(*lhs, &output, node_->child_mappings[0]) ||
          !MergeMappings(*rhs, &output, node_->child_mappings[1])) {
        continue;
      }
      bool matches = true;
      for (const auto &predicate : data_->predicates) {
        if (!PredicateIsTrue(Evaluate(predicate, output, *state_))) {
          matches = false;
          break;
        }
      }
      if (!matches) {
        continue;
      }
      *row = std::move(output);
      return true;
    }
    return false;
  }

  void Close() noexcept override { loop_.Close(); }

 private:
  const PhysicalPlanNode *node_ = nullptr;
  const PredicateJoinOp *data_ = nullptr;
  RuntimeState *state_ = nullptr;
  CachedNestedLoopState loop_;
};

class NodeHashJoinOperator final : public PullOperator {
 public:
  NodeHashJoinOperator(const PhysicalPlanNode &node, RuntimeState &state,
                       std::unique_ptr<PullOperator> lhs,
                       std::unique_ptr<PullOperator> rhs)
      : node_(&node),
        data_(&OperatorData<NodeHashJoinOp>(node)),
        state_(&state),
        lhs_(std::move(lhs)),
        rhs_(std::move(rhs)) {}

  ~NodeHashJoinOperator() override { Close(); }

  [[nodiscard]] bool Next(SlottedRow *row) override {
    RG_CHECK(row != nullptr, common::InvalidArgumentError,
             "output row is null");
    state_->CheckCancelled();
    if (closed_) {
      return false;
    }
    if (!initialized_) {
      Initialize();
    }

    while (true) {
      while (bucket_ != nullptr && bucket_offset_ < bucket_->size()) {
        state_->CheckCancelled();
        const SlottedRow &build_row = build_rows_[(*bucket_)[bucket_offset_++]];
        const SlottedRow &lhs = BuildChild() == 0 ? build_row : *probe_row_;
        const SlottedRow &rhs = BuildChild() == 0 ? *probe_row_ : build_row;
        SlottedRow output(node_->output_slots);
        if (!MergeMappings(lhs, &output, node_->child_mappings[0]) ||
            !MergeMappings(rhs, &output, node_->child_mappings[1])) {
          continue;
        }
        *row = std::move(output);
        return true;
      }

      bucket_ = nullptr;
      bucket_offset_ = 0;
      probe_row_.reset();
      SlottedRow probe(node_->children[ProbeChild()]->output_slots);
      if (!Source(ProbeChild())->Next(&probe)) {
        Close();
        return false;
      }
      state_->CheckCancelled();
      std::optional<CompositeValueKey> key =
          NodeJoinKey(probe, KeySlots(ProbeChild()));
      if (!key.has_value()) {
        continue;
      }
      const auto found = buckets_.find(*key);
      if (found == buckets_.end()) {
        continue;
      }
      probe_row_.emplace(std::move(probe));
      bucket_ = &found->second;
    }
  }

  void Close() noexcept override {
    if (closed_) {
      return;
    }
    if (lhs_ != nullptr) {
      lhs_->Close();
    }
    if (rhs_ != nullptr) {
      rhs_->Close();
    }
    probe_row_.reset();
    bucket_ = nullptr;
    buckets_.clear();
    build_rows_.clear();
    state_->memory_tracker.Release(reserved_bytes_);
    reserved_bytes_ = 0;
    closed_ = true;
  }

 private:
  using Bucket = std::vector<std::size_t>;

  [[nodiscard]] std::size_t BuildChild() const { return data_->build_child; }

  [[nodiscard]] std::size_t ProbeChild() const { return 1U - BuildChild(); }

  PullOperator *Source(std::size_t child) const {
    return child == 0 ? lhs_.get() : rhs_.get();
  }

  const std::vector<std::size_t> &KeySlots(std::size_t child) const {
    return child == 0 ? data_->left_key_slots : data_->right_key_slots;
  }

  void Initialize() {
    initialized_ = true;
    RG_CHECK(BuildChild() < 2, common::InternalError,
             "invalid node hash join build child");
    PullOperator *source = Source(BuildChild());
    while (true) {
      SlottedRow input(node_->children[BuildChild()]->output_slots);
      if (!source->Next(&input)) {
        source->Close();
        return;
      }
      state_->CheckCancelled();
      std::optional<CompositeValueKey> key =
          NodeJoinKey(input, KeySlots(BuildChild()));
      if (!key.has_value()) {
        continue;
      }

      const std::size_t row_bytes = input.EstimatedHeapUsage();
      state_->memory_tracker.Reserve(row_bytes);
      reserved_bytes_ += row_bytes;
      const std::size_t row_index = build_rows_.size();
      build_rows_.push_back(std::move(input));

      auto [found, inserted] = buckets_.try_emplace(std::move(*key));
      if (inserted) {
        const std::size_t key_bytes = EstimatedKeyHeapUsage(found->first);
        state_->memory_tracker.Reserve(key_bytes);
        reserved_bytes_ += key_bytes;
      }
      const std::size_t old_capacity = found->second.capacity();
      found->second.push_back(row_index);
      const std::size_t new_capacity = found->second.capacity();
      if (new_capacity > old_capacity) {
        const std::size_t bucket_bytes =
            (new_capacity - old_capacity) * sizeof(std::size_t);
        state_->memory_tracker.Reserve(bucket_bytes);
        reserved_bytes_ += bucket_bytes;
      }
    }
  }

  const PhysicalPlanNode *node_ = nullptr;
  const NodeHashJoinOp *data_ = nullptr;
  RuntimeState *state_ = nullptr;
  std::unique_ptr<PullOperator> lhs_;
  std::unique_ptr<PullOperator> rhs_;
  std::unordered_map<CompositeValueKey, Bucket, ValueHash, ValueEqual> buckets_;
  std::vector<SlottedRow> build_rows_;
  std::optional<SlottedRow> probe_row_;
  const Bucket *bucket_ = nullptr;
  std::size_t bucket_offset_ = 0;
  std::size_t reserved_bytes_ = 0;
  bool initialized_ = false;
  bool closed_ = false;
};

class ValueHashJoinOperator final : public PullOperator {
 public:
  ValueHashJoinOperator(const PhysicalPlanNode &node, RuntimeState &state,
                        std::unique_ptr<PullOperator> lhs,
                        std::unique_ptr<PullOperator> rhs)
      : node_(&node),
        data_(&OperatorData<ValueHashJoinOp>(node)),
        state_(&state),
        lhs_(std::move(lhs)),
        rhs_(std::move(rhs)) {}

  ~ValueHashJoinOperator() override { Close(); }

  [[nodiscard]] bool Next(SlottedRow *row) override {
    RG_CHECK(row != nullptr, common::InvalidArgumentError,
             "output row is null");
    state_->CheckCancelled();
    if (closed_) {
      return false;
    }
    if (!initialized_) {
      Initialize();
    }

    while (true) {
      while (bucket_ != nullptr && bucket_offset_ < bucket_->size()) {
        state_->CheckCancelled();
        const SlottedRow &build_row = build_rows_[(*bucket_)[bucket_offset_++]];
        const SlottedRow &lhs = BuildChild() == 0 ? build_row : *probe_row_;
        const SlottedRow &rhs = BuildChild() == 0 ? *probe_row_ : build_row;
        SlottedRow output(node_->output_slots);
        if (!MergeMappings(lhs, &output, node_->child_mappings[0]) ||
            !MergeMappings(rhs, &output, node_->child_mappings[1])) {
          continue;
        }
        if (!PredicatesMatch(output)) {
          continue;
        }
        *row = std::move(output);
        return true;
      }

      bucket_ = nullptr;
      bucket_offset_ = 0;
      probe_row_.reset();
      SlottedRow probe(node_->children[ProbeChild()]->output_slots);
      if (!Source(ProbeChild())->Next(&probe)) {
        Close();
        return false;
      }
      state_->CheckCancelled();
      std::optional<CompositeValueKey> key = JoinKey(probe, ProbeChild());
      if (!key.has_value()) {
        continue;
      }
      const auto found = buckets_.find(*key);
      if (found == buckets_.end()) {
        continue;
      }
      probe_row_.emplace(std::move(probe));
      bucket_ = &found->second;
    }
  }

  void Close() noexcept override {
    if (closed_) {
      return;
    }
    if (lhs_ != nullptr) {
      lhs_->Close();
    }
    if (rhs_ != nullptr) {
      rhs_->Close();
    }
    probe_row_.reset();
    bucket_ = nullptr;
    buckets_.clear();
    build_rows_.clear();
    state_->memory_tracker.Release(reserved_bytes_);
    reserved_bytes_ = 0;
    closed_ = true;
  }

 private:
  using Bucket = std::vector<std::size_t>;

  [[nodiscard]] std::size_t BuildChild() const { return data_->build_child; }

  [[nodiscard]] std::size_t ProbeChild() const { return 1U - BuildChild(); }

  PullOperator *Source(std::size_t child) const {
    return child == 0 ? lhs_.get() : rhs_.get();
  }

  std::optional<CompositeValueKey> JoinKey(const SlottedRow &row,
                                           std::size_t child) const {
    CompositeValueKey result;
    result.values.reserve(data_->keys.size());
    for (const auto &key : data_->keys) {
      const PhysicalExpression &expression = child == 0 ? key.left : key.right;
      Value value = Evaluate(expression, row, *state_);
      if (value.IsNull()) {
        return std::nullopt;
      }
      result.values.push_back(std::move(value));
    }
    return result;
  }

  bool PredicatesMatch(const SlottedRow &row) const {
    for (const auto &predicate : data_->predicates) {
      if (!PredicateIsTrue(Evaluate(predicate, row, *state_))) {
        return false;
      }
    }
    return true;
  }

  void Initialize() {
    initialized_ = true;
    RG_CHECK(BuildChild() < 2, common::InternalError,
             "invalid value hash join build child");
    PullOperator *source = Source(BuildChild());
    while (true) {
      SlottedRow input(node_->children[BuildChild()]->output_slots);
      if (!source->Next(&input)) {
        source->Close();
        return;
      }
      state_->CheckCancelled();
      std::optional<CompositeValueKey> key = JoinKey(input, BuildChild());
      if (!key.has_value()) {
        continue;
      }

      const std::size_t row_bytes = input.EstimatedHeapUsage();
      state_->memory_tracker.Reserve(row_bytes);
      reserved_bytes_ += row_bytes;
      const std::size_t row_index = build_rows_.size();
      build_rows_.push_back(std::move(input));

      auto [found, inserted] = buckets_.try_emplace(std::move(*key));
      if (inserted) {
        const std::size_t key_bytes = EstimatedKeyHeapUsage(found->first);
        state_->memory_tracker.Reserve(key_bytes);
        reserved_bytes_ += key_bytes;
      }
      const std::size_t old_capacity = found->second.capacity();
      found->second.push_back(row_index);
      const std::size_t new_capacity = found->second.capacity();
      if (new_capacity > old_capacity) {
        const std::size_t bucket_bytes =
            (new_capacity - old_capacity) * sizeof(std::size_t);
        state_->memory_tracker.Reserve(bucket_bytes);
        reserved_bytes_ += bucket_bytes;
      }
    }
  }

  const PhysicalPlanNode *node_ = nullptr;
  const ValueHashJoinOp *data_ = nullptr;
  RuntimeState *state_ = nullptr;
  std::unique_ptr<PullOperator> lhs_;
  std::unique_ptr<PullOperator> rhs_;
  std::unordered_map<CompositeValueKey, Bucket, ValueHash, ValueEqual> buckets_;
  std::vector<SlottedRow> build_rows_;
  std::optional<SlottedRow> probe_row_;
  const Bucket *bucket_ = nullptr;
  std::size_t bucket_offset_ = 0;
  std::size_t reserved_bytes_ = 0;
  bool initialized_ = false;
  bool closed_ = false;
};

class UnionInputState final {
 public:
  UnionInputState(const PhysicalPlanNode &node, RuntimeState &state,
                  std::unique_ptr<PullOperator> lhs,
                  std::unique_ptr<PullOperator> rhs)
      : node_(&node),
        state_(&state),
        lhs_(std::move(lhs)),
        rhs_(std::move(rhs)) {}

  ~UnionInputState() { Close(); }

  [[nodiscard]] bool Next(SlottedRow *row) {
    RG_CHECK(row != nullptr, common::InvalidArgumentError,
             "output row is null");
    state_->CheckCancelled();
    if (closed_) {
      return false;
    }
    while (side_ < 2) {
      PullOperator *source = side_ == 0 ? lhs_.get() : rhs_.get();
      const PhysicalPlanNode &child = *node_->children[side_];
      SlottedRow input(child.output_slots);
      if (!source->Next(&input)) {
        source->Close();
        ++side_;
        continue;
      }
      SlottedRow output(node_->output_slots);
      CopyMappings(input, &output, node_->child_mappings[side_]);
      *row = std::move(output);
      return true;
    }
    Close();
    return false;
  }

  void Close() noexcept {
    if (closed_) {
      return;
    }
    if (lhs_ != nullptr) {
      lhs_->Close();
    }
    if (rhs_ != nullptr) {
      rhs_->Close();
    }
    closed_ = true;
  }

 private:
  const PhysicalPlanNode *node_ = nullptr;
  RuntimeState *state_ = nullptr;
  std::unique_ptr<PullOperator> lhs_;
  std::unique_ptr<PullOperator> rhs_;
  std::size_t side_ = 0;
  bool closed_ = false;
};

class UnionAllOperator final : public PullOperator {
 public:
  UnionAllOperator(const PhysicalPlanNode &node, RuntimeState &state,
                   std::unique_ptr<PullOperator> lhs,
                   std::unique_ptr<PullOperator> rhs)
      : input_(node, state, std::move(lhs), std::move(rhs)) {
    (void)OperatorData<UnionAllOp>(node);
  }

  ~UnionAllOperator() override { Close(); }

  [[nodiscard]] bool Next(SlottedRow *row) override { return input_.Next(row); }

  void Close() noexcept override { input_.Close(); }

 private:
  UnionInputState input_;
};

class UnionDistinctOperator final : public PullOperator {
 public:
  UnionDistinctOperator(const PhysicalPlanNode &node, RuntimeState &state,
                        std::unique_ptr<PullOperator> lhs,
                        std::unique_ptr<PullOperator> rhs)
      : node_(&node),
        data_(&OperatorData<UnionDistinctOp>(node)),
        state_(&state),
        input_(node, state, std::move(lhs), std::move(rhs)) {}

  ~UnionDistinctOperator() override { Close(); }

  [[nodiscard]] bool Next(SlottedRow *row) override {
    RG_CHECK(row != nullptr, common::InvalidArgumentError,
             "output row is null");
    if (closed_) {
      return false;
    }
    SlottedRow input(node_->output_slots);
    while (input_.Next(&input)) {
      CompositeValueKey key;
      key.values.reserve(data_->key_slots.size());
      for (const std::size_t offset : data_->key_slots) {
        key.values.push_back(ReadRowValue(input, offset));
      }
      auto [seen, inserted] = seen_.insert(std::move(key));
      if (!inserted) {
        continue;
      }
      const std::size_t key_bytes = EstimatedKeyHeapUsage(*seen);
      state_->memory_tracker.Reserve(key_bytes);
      reserved_bytes_ += key_bytes;
      *row = std::move(input);
      return true;
    }
    Close();
    return false;
  }

  void Close() noexcept override {
    if (closed_) {
      return;
    }
    input_.Close();
    seen_.clear();
    state_->memory_tracker.Release(reserved_bytes_);
    reserved_bytes_ = 0;
    closed_ = true;
  }

 private:
  const PhysicalPlanNode *node_ = nullptr;
  const UnionDistinctOp *data_ = nullptr;
  RuntimeState *state_ = nullptr;
  UnionInputState input_;
  std::unordered_set<CompositeValueKey, ValueHash, ValueEqual> seen_;
  std::size_t reserved_bytes_ = 0;
  bool closed_ = false;
};

class CorrelatedRightState final {
 public:
  CorrelatedRightState(const PhysicalPlanNode &node, RuntimeState &state,
                       OperatorFactory &factory,
                       std::unique_ptr<PullOperator> lhs)
      : node_(&node),
        state_(&state),
        factory_(&factory),
        lhs_(std::move(lhs)) {}

  ~CorrelatedRightState() { Close(); }

  [[nodiscard]] bool NextLeft();
  void OpenRight();
  [[nodiscard]] bool NextRight(SlottedRow *row);
  [[nodiscard]] bool HasLeft() const noexcept {
    return current_left_.has_value();
  }
  [[nodiscard]] const SlottedRow &Left() const;
  void FinishLeft() noexcept;
  void Close() noexcept;

 private:
  const PhysicalPlanNode *node_ = nullptr;
  RuntimeState *state_ = nullptr;
  OperatorFactory *factory_ = nullptr;
  std::unique_ptr<PullOperator> lhs_;
  std::unique_ptr<PullOperator> rhs_;
  std::optional<SlottedRow> current_left_;
  bool closed_ = false;
};

class ApplyOperator final : public PullOperator {
 public:
  ApplyOperator(const PhysicalPlanNode &node, RuntimeState &state,
                OperatorFactory &factory, std::unique_ptr<PullOperator> lhs)
      : node_(&node),
        state_(&state),
        correlated_(node, state, factory, std::move(lhs)) {
    (void)OperatorData<ApplyOp>(node);
  }

  ~ApplyOperator() override { Close(); }

  [[nodiscard]] bool Next(SlottedRow *row) override;
  void Close() noexcept override { correlated_.Close(); }

 private:
  const PhysicalPlanNode *node_ = nullptr;
  RuntimeState *state_ = nullptr;
  CorrelatedRightState correlated_;
};

class OptionalApplyOperator final : public PullOperator {
 public:
  OptionalApplyOperator(const PhysicalPlanNode &node, RuntimeState &state,
                        OperatorFactory &factory,
                        std::unique_ptr<PullOperator> lhs)
      : node_(&node),
        data_(&OperatorData<OptionalApplyOp>(node)),
        state_(&state),
        correlated_(node, state, factory, std::move(lhs)) {}

  ~OptionalApplyOperator() override { Close(); }

  [[nodiscard]] bool Next(SlottedRow *row) override;
  void Close() noexcept override { correlated_.Close(); }

 private:
  const PhysicalPlanNode *node_ = nullptr;
  const OptionalApplyOp *data_ = nullptr;
  RuntimeState *state_ = nullptr;
  CorrelatedRightState correlated_;
  bool rhs_produced_ = false;
};

class SemiApplyOperator final : public PullOperator {
 public:
  SemiApplyOperator(const PhysicalPlanNode &node, RuntimeState &state,
                    OperatorFactory &factory, std::unique_ptr<PullOperator> lhs)
      : node_(&node),
        state_(&state),
        correlated_(node, state, factory, std::move(lhs)) {
    (void)OperatorData<SemiApplyOp>(node);
  }

  ~SemiApplyOperator() override { Close(); }

  [[nodiscard]] bool Next(SlottedRow *row) override;
  void Close() noexcept override { correlated_.Close(); }

 private:
  const PhysicalPlanNode *node_ = nullptr;
  RuntimeState *state_ = nullptr;
  CorrelatedRightState correlated_;
};

class AntiSemiApplyOperator final : public PullOperator {
 public:
  AntiSemiApplyOperator(const PhysicalPlanNode &node, RuntimeState &state,
                        OperatorFactory &factory,
                        std::unique_ptr<PullOperator> lhs)
      : node_(&node),
        state_(&state),
        correlated_(node, state, factory, std::move(lhs)) {
    (void)OperatorData<AntiSemiApplyOp>(node);
  }

  ~AntiSemiApplyOperator() override { Close(); }

  [[nodiscard]] bool Next(SlottedRow *row) override;
  void Close() noexcept override { correlated_.Close(); }

 private:
  const PhysicalPlanNode *node_ = nullptr;
  RuntimeState *state_ = nullptr;
  CorrelatedRightState correlated_;
};

class LetSemiApplyOperator final : public PullOperator {
 public:
  LetSemiApplyOperator(const PhysicalPlanNode &node, RuntimeState &state,
                       OperatorFactory &factory,
                       std::unique_ptr<PullOperator> lhs)
      : node_(&node),
        data_(&OperatorData<LetSemiApplyOp>(node)),
        state_(&state),
        correlated_(node, state, factory, std::move(lhs)) {}

  ~LetSemiApplyOperator() override { Close(); }

  [[nodiscard]] bool Next(SlottedRow *row) override;
  void Close() noexcept override { correlated_.Close(); }

 private:
  const PhysicalPlanNode *node_ = nullptr;
  const LetSemiApplyOp *data_ = nullptr;
  RuntimeState *state_ = nullptr;
  CorrelatedRightState correlated_;
};

class SelectOrSemiApplyOperator final : public PullOperator {
 public:
  SelectOrSemiApplyOperator(const PhysicalPlanNode &node, RuntimeState &state,
                            OperatorFactory &factory,
                            std::unique_ptr<PullOperator> lhs)
      : node_(&node),
        data_(&OperatorData<SelectOrSemiApplyOp>(node)),
        state_(&state),
        correlated_(node, state, factory, std::move(lhs)) {}

  ~SelectOrSemiApplyOperator() override { Close(); }

  [[nodiscard]] bool Next(SlottedRow *row) override;
  void Close() noexcept override { correlated_.Close(); }

 private:
  const PhysicalPlanNode *node_ = nullptr;
  const SelectOrSemiApplyOp *data_ = nullptr;
  RuntimeState *state_ = nullptr;
  CorrelatedRightState correlated_;
};

class RollUpApplyOperator final : public PullOperator {
 public:
  RollUpApplyOperator(const PhysicalPlanNode &node, RuntimeState &state,
                      OperatorFactory &factory,
                      std::unique_ptr<PullOperator> lhs)
      : node_(&node),
        data_(&OperatorData<RollUpApplyOp>(node)),
        state_(&state),
        correlated_(node, state, factory, std::move(lhs)) {}

  ~RollUpApplyOperator() override { Close(); }

  [[nodiscard]] bool Next(SlottedRow *row) override;
  void Close() noexcept override { correlated_.Close(); }

 private:
  const PhysicalPlanNode *node_ = nullptr;
  const RollUpApplyOp *data_ = nullptr;
  RuntimeState *state_ = nullptr;
  CorrelatedRightState correlated_;
};

class MergeOperator final : public PullOperator {
 public:
  MergeOperator(const PhysicalPlanNode &node, RuntimeState &state,
                OperatorFactory &factory, std::unique_ptr<PullOperator> source)
      : node_(&node),
        data_(&OperatorData<MergeOp>(node)),
        state_(&state),
        factory_(&factory),
        source_(std::move(source)) {}

  ~MergeOperator() override { Close(); }

  [[nodiscard]] bool Next(SlottedRow *row) override;
  void Close() noexcept override;

 private:
  void Initialize();
  void BufferRow(SlottedRow row) {
    const std::size_t bytes = row.EstimatedHeapUsage();
    state_->memory_tracker.Reserve(bytes);
    reserved_bytes_ += bytes;
    rows_.push_back(std::move(row));
  }

  const PhysicalPlanNode *node_ = nullptr;
  const MergeOp *data_ = nullptr;
  RuntimeState *state_ = nullptr;
  OperatorFactory *factory_ = nullptr;
  std::unique_ptr<PullOperator> source_;
  std::vector<SlottedRow> rows_;
  std::size_t next_ = 0;
  std::size_t reserved_bytes_ = 0;
  bool initialized_ = false;
};

std::unique_ptr<PullOperator> BuildBinaryOperator(
    const PhysicalPlanNode &node, RuntimeState &state,
    std::unique_ptr<PullOperator> lhs, std::unique_ptr<PullOperator> rhs) {
  RG_CHECK(node.children.size() == 2, common::InternalError,
           std::string(ToString(node.kind)) +
               " physical node must have two children");
  switch (node.kind) {
    case PhysicalOperatorKind::kUnionAll:
      return std::make_unique<UnionAllOperator>(node, state, std::move(lhs),
                                                std::move(rhs));
    case PhysicalOperatorKind::kUnionDistinct:
      return std::make_unique<UnionDistinctOperator>(
          node, state, std::move(lhs), std::move(rhs));
    case PhysicalOperatorKind::kValueHashJoin:
      return std::make_unique<ValueHashJoinOperator>(
          node, state, std::move(lhs), std::move(rhs));
    case PhysicalOperatorKind::kLeftOuterHashJoin:
      return std::make_unique<LeftOuterHashJoinOperator>(
          node, state, std::move(lhs), std::move(rhs));
    case PhysicalOperatorKind::kNodeHashJoin:
      return std::make_unique<NodeHashJoinOperator>(node, state, std::move(lhs),
                                                    std::move(rhs));
    case PhysicalOperatorKind::kCartesianProduct:
      return std::make_unique<CartesianProductOperator>(
          node, state, std::move(lhs), std::move(rhs));
    case PhysicalOperatorKind::kPredicateJoin:
      return std::make_unique<PredicateJoinOperator>(
          node, state, std::move(lhs), std::move(rhs));
    default:
      RG_THROW(common::InternalError, "not a binary physical operator: " +
                                          std::string(ToString(node.kind)));
  }
}

std::unique_ptr<PullOperator> BuildCorrelatedOperator(
    const PhysicalPlanNode &node, RuntimeState &state, OperatorFactory &factory,
    std::unique_ptr<PullOperator> lhs) {
  RG_CHECK(node.children.size() == 2, common::InternalError,
           std::string(ToString(node.kind)) +
               " physical node must have two children");
  switch (node.kind) {
    case PhysicalOperatorKind::kApply:
      return std::make_unique<ApplyOperator>(node, state, factory,
                                             std::move(lhs));
    case PhysicalOperatorKind::kOptionalApply:
      return std::make_unique<OptionalApplyOperator>(node, state, factory,
                                                     std::move(lhs));
    case PhysicalOperatorKind::kSemiApply:
      return std::make_unique<SemiApplyOperator>(node, state, factory,
                                                 std::move(lhs));
    case PhysicalOperatorKind::kAntiSemiApply:
      return std::make_unique<AntiSemiApplyOperator>(node, state, factory,
                                                     std::move(lhs));
    case PhysicalOperatorKind::kLetSemiApply:
      return std::make_unique<LetSemiApplyOperator>(node, state, factory,
                                                    std::move(lhs));
    case PhysicalOperatorKind::kSelectOrSemiApply:
      return std::make_unique<SelectOrSemiApplyOperator>(node, state, factory,
                                                         std::move(lhs));
    case PhysicalOperatorKind::kRollUpApply:
      return std::make_unique<RollUpApplyOperator>(node, state, factory,
                                                   std::move(lhs));
    case PhysicalOperatorKind::kMerge:
      return std::make_unique<MergeOperator>(node, state, factory,
                                             std::move(lhs));
    default:
      RG_THROW(common::InternalError, "not a correlated physical operator: " +
                                          std::string(ToString(node.kind)));
  }
}

bool CorrelatedRightState::NextLeft() {
  state_->CheckCancelled();
  if (closed_) {
    return false;
  }
  RG_CHECK(!current_left_.has_value() && rhs_ == nullptr, common::InternalError,
           "correlated right state still has an active left row");
  SlottedRow lhs_row(node_->children[0]->output_slots);
  if (!lhs_->Next(&lhs_row)) {
    Close();
    return false;
  }
  current_left_.emplace(std::move(lhs_row));
  return true;
}

void CorrelatedRightState::OpenRight() {
  state_->CheckCancelled();
  RG_CHECK(!closed_ && current_left_.has_value() && rhs_ == nullptr,
           common::InternalError,
           "correlated right state cannot open the right child");
  rhs_ = factory_->Build(*node_->children[1], *current_left_);
}

bool CorrelatedRightState::NextRight(SlottedRow *row) {
  RG_CHECK(row != nullptr, common::InvalidArgumentError, "right row is null");
  RG_CHECK(current_left_.has_value() && rhs_ != nullptr, common::InternalError,
           "correlated right state has no active left row");
  state_->CheckCancelled();
  return rhs_->Next(row);
}

const SlottedRow &CorrelatedRightState::Left() const {
  RG_CHECK(current_left_.has_value(), common::InternalError,
           "correlated right state has no active left row");
  return *current_left_;
}

void CorrelatedRightState::FinishLeft() noexcept {
  if (rhs_ != nullptr) {
    rhs_->Close();
    rhs_.reset();
  }
  current_left_.reset();
}

void CorrelatedRightState::Close() noexcept {
  if (closed_) {
    return;
  }
  FinishLeft();
  if (lhs_ != nullptr) {
    lhs_->Close();
  }
  closed_ = true;
}

bool ApplyOperator::Next(SlottedRow *row) {
  RG_CHECK(row != nullptr, common::InvalidArgumentError, "output row is null");
  while (true) {
    state_->CheckCancelled();
    if (!correlated_.HasLeft()) {
      if (!correlated_.NextLeft()) {
        return false;
      }
      correlated_.OpenRight();
    }

    SlottedRow rhs(node_->children[1]->output_slots);
    while (correlated_.NextRight(&rhs)) {
      SlottedRow output(node_->output_slots);
      if (!MergeMappings(correlated_.Left(), &output,
                         node_->child_mappings[0]) ||
          !MergeMappings(rhs, &output, node_->child_mappings[1])) {
        continue;
      }
      *row = std::move(output);
      return true;
    }
    correlated_.FinishLeft();
  }
}

bool OptionalApplyOperator::Next(SlottedRow *row) {
  RG_CHECK(row != nullptr, common::InvalidArgumentError, "output row is null");
  while (true) {
    state_->CheckCancelled();
    if (!correlated_.HasLeft()) {
      if (!correlated_.NextLeft()) {
        return false;
      }
      correlated_.OpenRight();
      rhs_produced_ = false;
    }

    SlottedRow rhs(node_->children[1]->output_slots);
    while (correlated_.NextRight(&rhs)) {
      rhs_produced_ = true;
      SlottedRow output(node_->output_slots);
      if (MergeMappings(correlated_.Left(), &output,
                        node_->child_mappings[0]) &&
          MergeMappings(rhs, &output, node_->child_mappings[1])) {
        *row = std::move(output);
        return true;
      }
    }

    if (!rhs_produced_) {
      SlottedRow output(node_->output_slots);
      CopyMappings(correlated_.Left(), &output, node_->child_mappings[0]);
      for (const std::size_t slot : data_->nullable_output_slots) {
        if (!output.IsInitialized(slot)) {
          output.SetNull(slot);
        }
      }
      correlated_.FinishLeft();
      *row = std::move(output);
      return true;
    }
    correlated_.FinishLeft();
  }
}
bool SemiApplyOperator::Next(SlottedRow *row) {
  RG_CHECK(row != nullptr, common::InvalidArgumentError, "output row is null");
  while (correlated_.NextLeft()) {
    correlated_.OpenRight();
    SlottedRow rhs(node_->children[1]->output_slots);
    const bool exists = correlated_.NextRight(&rhs);
    if (exists) {
      SlottedRow output = CopyChildOutput(*node_, 0, correlated_.Left());
      correlated_.FinishLeft();
      *row = std::move(output);
      return true;
    }
    correlated_.FinishLeft();
  }
  return false;
}

bool AntiSemiApplyOperator::Next(SlottedRow *row) {
  RG_CHECK(row != nullptr, common::InvalidArgumentError, "output row is null");
  while (correlated_.NextLeft()) {
    correlated_.OpenRight();
    SlottedRow rhs(node_->children[1]->output_slots);
    const bool exists = correlated_.NextRight(&rhs);
    if (!exists) {
      SlottedRow output = CopyChildOutput(*node_, 0, correlated_.Left());
      correlated_.FinishLeft();
      *row = std::move(output);
      return true;
    }
    correlated_.FinishLeft();
  }
  return false;
}

bool LetSemiApplyOperator::Next(SlottedRow *row) {
  RG_CHECK(row != nullptr, common::InvalidArgumentError, "output row is null");
  if (!correlated_.NextLeft()) {
    return false;
  }
  correlated_.OpenRight();
  SlottedRow rhs(node_->children[1]->output_slots);
  const bool exists = correlated_.NextRight(&rhs);
  SlottedRow output = CopyChildOutput(*node_, 0, correlated_.Left());
  StoreEvaluatedValue(&output, data_->value_slot, Value(exists), *state_);
  correlated_.FinishLeft();
  *row = std::move(output);
  return true;
}

bool SelectOrSemiApplyOperator::Next(SlottedRow *row) {
  RG_CHECK(row != nullptr, common::InvalidArgumentError, "output row is null");
  while (correlated_.NextLeft()) {
    if (PredicateIsTrue(
            Evaluate(data_->predicate, correlated_.Left(), *state_))) {
      *row = CopyChildOutput(*node_, 0, correlated_.Left());
      correlated_.FinishLeft();
      return true;
    }

    correlated_.OpenRight();
    SlottedRow rhs(node_->children[1]->output_slots);
    const bool exists = correlated_.NextRight(&rhs);
    if (exists != data_->anti) {
      SlottedRow output = CopyChildOutput(*node_, 0, correlated_.Left());
      correlated_.FinishLeft();
      *row = std::move(output);
      return true;
    }
    correlated_.FinishLeft();
  }
  return false;
}

bool RollUpApplyOperator::Next(SlottedRow *row) {
  RG_CHECK(row != nullptr, common::InvalidArgumentError, "output row is null");
  if (!correlated_.NextLeft()) {
    return false;
  }
  correlated_.OpenRight();
  Value::List values;
  std::size_t reserved_bytes = 0;
  try {
    SlottedRow rhs(node_->children[1]->output_slots);
    while (correlated_.NextRight(&rhs)) {
      Value value = ReadRowValue(rhs, data_->value_slot);
      const std::size_t bytes = EstimatedValueHeapUsage(value);
      state_->memory_tracker.Reserve(bytes);
      reserved_bytes += bytes;
      values.push_back(std::move(value));
    }
    SlottedRow output = CopyChildOutput(*node_, 0, correlated_.Left());
    StoreEvaluatedValue(&output, data_->collection_slot,
                        Value(std::move(values)), *state_);
    state_->memory_tracker.Release(reserved_bytes);
    correlated_.FinishLeft();
    *row = std::move(output);
    return true;
  } catch (...) {
    state_->memory_tracker.Release(reserved_bytes);
    correlated_.FinishLeft();
    throw;
  }
}

void MergeOperator::Initialize() {
  initialized_ = true;
  SlottedRow input(node_->children[0]->output_slots);
  while (source_->Next(&input)) {
    state_->CheckCancelled();
    std::unique_ptr<PullOperator> rhs =
        factory_->Build(*node_->children[1], input);
    bool matched = false;
    SlottedRow rhs_row(node_->children[1]->output_slots);
    while (rhs->Next(&rhs_row)) {
      SlottedRow output(node_->output_slots);
      if (!MergeMappings(input, &output, node_->child_mappings[0]) ||
          !MergeMappings(rhs_row, &output, node_->child_mappings[1])) {
        continue;
      }
      matched = true;
      ExecuteMergeActions(*data_, true, &output, state_);
      BufferRow(std::move(output));
    }
    rhs->Close();
    if (matched) {
      continue;
    }
    SlottedRow output = CopyChildOutput(*node_, 0, input);
    for (const auto &command : data_->create_commands) {
      state_->CheckCancelled();
      std::visit(
          [this, &output](const auto &create) {
            ExecuteMergeCreate(create, &output, state_);
          },
          command);
    }
    ExecuteMergeActions(*data_, false, &output, state_);
    BufferRow(std::move(output));
  }
}

bool MergeOperator::Next(SlottedRow *row) {
  state_->CheckCancelled();
  if (!initialized_) {
    Initialize();
  }
  if (next_ >= rows_.size()) {
    Close();
    return false;
  }
  *row = std::move(rows_[next_++]);
  return true;
}

void MergeOperator::Close() noexcept {
  if (source_ != nullptr) {
    source_->Close();
  }
  state_->memory_tracker.Release(reserved_bytes_);
  reserved_bytes_ = 0;
}

}  // namespace rg::slotted
