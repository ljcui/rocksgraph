# AST 到 PlannerQuery 后续计划

本文档只记录尚未完成的 AST 到 PlannerQuery 阶段工作。短期目标是把 Cypher 前端到 PlannerQuery IR 的边界做扎实。

## 目标

- 让 PlannerQuery 保留 Cypher 的声明式语义，避免过早做逻辑计划或物理执行决策。
- 对齐 Neo4j 的核心分层思路：AST rewrite / semantic analysis / CreatePlannerQuery / PlannerQuery IR。
- 让后续计划阶段可以基于 QueryGraph、Selections、QueryHorizon 做优化，而不是依赖 opaque AST 表达式。
- 保持当前实现不兼容旧 IR，不为已删除的逻辑计划层保留兼容代码。

## 设计原则

- `PlannerQuery` 是 AST 和后续计划阶段之间的边界，不应该依赖具体逻辑计划节点。
- `QueryGraph` 描述两次 horizon 之间的 MATCH、OPTIONAL MATCH、WHERE 和更新语义。
- 节点 pattern 以变量集合表达。
- 关系 pattern 使用结构化对象表达，包括关系变量、左右节点、方向、类型和长度。
- 节点 label、属性谓词、关系属性谓词等可规划谓词进入 `Selections`，但需要保留结构化信息，不能只降级成普通表达式字符串。
- inline relationship type 应优先保留在 `PatternRelationship::types`；显式 `WHERE r:TYPE` 可以作为 `Selections`，后续再由 planner rewrite 选择是否内联到关系 pattern。
- `WITH`、`RETURN`、`UNWIND`、聚合、DISTINCT、子查询等边界操作进入 `QueryHorizon`。
- `WITH ... WHERE`、`YIELD ... WHERE` 这类 horizon 之后的过滤应作为 projection horizon 自身的 selections，而不是混入前一个 `QueryGraph` 的 MATCH selections。
- AST rewrite 可以继续做语法级规范化，但 PlannerQuery 构建阶段应尽量保留可规划语义。

## 当前限制

- `PatternPredicateNormalizationRewriter` 仍会把 inline label/type/property 拉到 `WHERE`。
- 其中 relationship type 也被清空到 `WHERE`，导致 `PatternRelationship::types` 在默认 pipeline 下为空；这点和 Neo4j 的主线行为不一致。
- PlannerQuery 目前是从 normalized WHERE predicate 中识别 label/type/property，而不是基于结构化 predicate 对象或 pattern 信息构建 selections。
- `ExpressionToString` 仍用于 WHERE predicate 去重，后续应替换为更稳定的结构化 key。
- `QueryHorizon` 仍较弱，projection 里只有 `distinct` 标志，尚未拆成 regular/distinct/aggregating projection。
- `Projection` 仍直接保存 `order_by`，尚未引入类似 Neo4j `InterestingOrder` / required order 的独立结构。
- 聚合表达式、grouping key、`count(*)` 之后的聚合语义还没有结构化进入 horizon。
- `SemanticTable` 尚未引入，变量类型、表达式 dependencies、聚合标记仍分散在现有逻辑中。
- `OPTIONAL MATCH`、`CALL { ... }`、`EXISTS` subquery 的 PlannerQuery 结构化表示尚未完成。
- `CREATE`、`MERGE`、`SET`、`DELETE`、`REMOVE` 等更新语句尚未进入 `mutating_patterns`。
- named path 当前仍不支持。

## 下一阶段 1：调整 inline pattern predicate 策略

当前状态是 AST rewrite 把 inline label/type/property 拉成 WHERE，PlannerQuery 再从 WHERE 中识别结构化 predicate。这个方案可用，但 relationship type 被清空后会丢失 pattern 结构信息。

Neo4j 的主查询路径不是让 `CreatePlannerQuery` 消费未规范化的原始 pattern，而是要求 normalized predicates、semantic info、isolated aggregations 等前置条件。主线 MATCH 通过 `Selections(optWhere.expression.asPredicates)` 接收已规范化 predicate，同时 `PatternRelationship.types` 仍保留普通 inline relationship type。只有 pattern expression / EXISTS / COUNT 这类表达式子查询会在 `CreateIrExpressions` 内部从 pattern 中抽取 inline predicates。

后续目标：

- 保持 PlannerQuery 面向 normalized AST 的主流程，但不能让 rewrite 破坏 pattern 仍需承载的结构信息。
- node labels、node/relationship properties 可以继续规范化为 predicate，再由 PlannerQuery 转为结构化 selections。
- inline relationship types 应留在 `PatternRelationship::types`；若同时生成 selection，必须有结构化 key 去重并明确二者职责。
- AST 层仍可以保留必要规范化，但不应破坏 PlannerQuery 需要的 pattern 结构信息。
- `ExpressionToString` 不再作为主要 predicate 去重依据。

建议方案：

- 拆分 `PatternPredicateNormalizationRewriter`：node label/property 和 relationship property 可继续抽取到 `WHERE`，普通 relationship type 不应清空到 `WHERE`。
- 给 `Predicate` 增加稳定结构化 key，例如 kind、variable、property、op、literal/表达式结构摘要，而不是使用 `ExpressionToString`。
- `PatternConverter` 直接保留 `RelationshipDetail::types` 到 `PatternRelationship::types`。
- 对 pattern expression / EXISTS / COUNT 等表达式子查询，再单独做 pattern 内 predicate 抽取，避免依赖主查询 rewrite 顺序。

不建议第一版直接把 PlannerQuery 构建移动到 pattern predicate normalization 之前。这样会偏离 Neo4j 的 normalized AST 前置条件，并且会同时影响匿名命名、CNF、聚合隔离、`RETURN *` 展开等已有 rewrite 结果。

## 下一阶段 2：扩展 QueryHorizon

当前 projection/unwind 不足。建议按顺序补：

- `RegularQueryProjection`
- `DistinctQueryProjection`
- `AggregatingQueryProjection`
- `UnwindProjection`
- `PassthroughAllHorizon`
- `CallSubqueryHorizon`

优先实现 aggregation 和 distinct 的结构化表达：

```cpp
struct AggregatingQueryProjection {
  std::vector<ProjectionItem> grouping_items;
  std::vector<ProjectionItem> aggregation_items;
};
```

需要配套 AST rewrite 或 analyzer：

- 识别聚合表达式。
- 区分 grouping key 和 aggregation item。
- 保留 `count(*)` 的聚合语义；不要把它无标记地降级成普通 `count(1)`。
- 校验非法聚合作用域。
- 将 `WITH ... WHERE` 放入 projection horizon 的 selections。
- 将 `SKIP` / `LIMIT` 抽为 pagination 结构。
- 暂时可保留 `order_by`，但后续应引入 required order / interesting order，避免把排序完全当成 projection 普通字段。

## 下一阶段 3：轻量 SemanticTable

PlannerQuery 构建需要比当前 semantic validator 更多的信息。建议新增轻量语义表：

- 变量类型：node、relationship、path、scalar、list、map。
- projection 输出符号。
- 表达式 dependencies。
- 聚合表达式标记。
- 子查询 imported variables。
- procedure/function 签名占位。

第一版可以只覆盖 PlannerQuery 构建所需的变量类型、dependencies 和聚合标记。

这一步应和 QueryHorizon 扩展并行或提前落地。聚合拆分、`SET n.prop` vs `SET r.prop`、`WHERE x:LabelOrType` 的 node/relationship 判定都需要类型信息，单靠当前 scope validator 不够。

## 下一阶段 4：按 clause 扩展转换能力

按优先级：

1. `OPTIONAL MATCH`
2. `EXISTS` subquery
3. `CALL { ... }`
4. named path
5. `CREATE`
6. `MERGE`
7. `SET` / `DELETE` / `REMOVE`

更新语句先只进入 PlannerQuery 的 `mutating_patterns`，不需要支持执行。

`OPTIONAL MATCH` 需要额外的 finalize pass 来修正 argument ids：optional query graph 应从当前已覆盖符号中取依赖交集，而不是简单继承全部可见变量。`CALL { ... }` 和 EXISTS / COUNT / COLLECT 表达式子查询也应复用 `PlannerQuery` 构建入口，作为 nested PlannerQuery 挂到 horizon 或 IR expression 中。

## 推荐落地顺序

1. 替换 `ExpressionToString` 谓词去重为结构化 key，并把 `Selections` 做成结构化 predicate 容器。
2. 拆分 inline pattern predicate normalization，保留 inline relationship type 到 `PatternRelationship::types`。
3. 引入轻量 `SemanticTable` / analyzer，覆盖变量类型、dependencies、聚合标记和 projection 输出符号。
4. 扩展 `QueryHorizon` 到 regular/distinct/aggregating/unwind/passthrough/call-subquery，并保留 `count(*)` 语义。
5. 增加 PlannerQuery finalize pass，统一修正 tail、OPTIONAL MATCH、子查询的 argument ids。
6. 支持 OPTIONAL MATCH。
7. 支持 EXISTS / COUNT / COLLECT expression subquery 和 `CALL { ... }`。
8. 支持 named path projection rewrite。
9. 支持 CREATE / MERGE / SET / DELETE / REMOVE 的 mutating patterns，其中 MERGE 需要按 Neo4j 思路隔离为独立 query segment。
