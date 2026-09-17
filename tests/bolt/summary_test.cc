#include "bolt/summary.h"

#include <gtest/gtest.h>

#if defined(NodesCreated) || defined(NodesDeleted) ||                 \
    defined(RelationshipsCreated) || defined(RelationshipsDeleted) || \
    defined(PropertiesSet) || defined(LabelsAdded) ||                 \
    defined(LabelsRemoved) || defined(IndexesAdded) ||                \
    defined(IndexesRemoved) || defined(ConstraintsAdded) ||           \
    defined(ConstraintsRemoved) || defined(SystemUpdates)
#error "Bolt summary keys must not leak preprocessor macros"
#endif

TEST(BoltSummaryTest, ExposesNamespacedCounterKeys) {
  EXPECT_STREQ(bolt::kNodesCreated, "nodes-created");
  EXPECT_STREQ(bolt::kNodesDeleted, "nodes-deleted");
  EXPECT_STREQ(bolt::kRelationshipsCreated, "relationships-created");
  EXPECT_STREQ(bolt::kRelationshipsDeleted, "relationships-deleted");
  EXPECT_STREQ(bolt::kPropertiesSet, "properties-set");
  EXPECT_STREQ(bolt::kLabelsAdded, "labels-added");
  EXPECT_STREQ(bolt::kLabelsRemoved, "labels-removed");
  EXPECT_STREQ(bolt::kIndexesAdded, "indexes-added");
  EXPECT_STREQ(bolt::kIndexesRemoved, "indexes-removed");
  EXPECT_STREQ(bolt::kConstraintsAdded, "constraints-added");
  EXPECT_STREQ(bolt::kConstraintsRemoved, "constraints-removed");
  EXPECT_STREQ(bolt::kSystemUpdates, "system-updates");
  EXPECT_STREQ(bolt::kContainsSystemUpdates, "contains-system-updates");
  EXPECT_STREQ(bolt::kContainsUpdates, "contains-updates");
}
