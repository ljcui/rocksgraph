#pragma once
#include <string>

namespace bolt {
// Point2D represents a two dimensional point in a particular coordinate
// reference system.
struct Point2D {
  double x = 0;
  double y = 0;
  uint32_t spatialRefId = 0;  // id of coordinate reference system.
};

// Point3D represents a three dimensional point in a particular coordinate
// reference system.
struct Point3D {
  double x = 0;
  double y = 0;
  double z = 0;
  uint32_t spatialRefId = 0;  // id of coordinate reference system.
};

}  // namespace bolt
