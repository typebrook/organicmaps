#include "indexer/drawing_rule_def.hpp"

#include <algorithm>
#include <iterator>

namespace drule
{
using namespace std;

namespace
{
struct less_key
{
  bool operator() (drule::Key const & r1, drule::Key const & r2) const
  {
    // assume that unique algo leaves the first element (with max priority), others - go away
    if (r1.m_type == r2.m_type)
    {
      if (r1.m_hatching != r2.m_hatching)
        return r1.m_hatching;
      return (r1.m_priority > r2.m_priority);
    }
    else
      return (r1.m_type < r2.m_type);
   }
};

struct equal_key
{
  bool operator() (drule::Key const & r1, drule::Key const & r2) const
  {
    // many line rules - is ok, other rules - one is enough
    if (r1.m_type == drule::line)
      return (r1 == r2);
    else
    {
      if (r1.m_type == r2.m_type)
      {
        // Keep several area styles if bigger one (r1) is hatching and second is not.
        if (r1.m_type == drule::area && r1.m_hatching && !r2.m_hatching)
          return false;

        return true;
      }
      return false;
    }
  }
};
}  // namespace

void MakeUnique(KeysT & keys)
{
  sort(keys.begin(), keys.end(), less_key());
  keys.resize(distance(keys.begin(), unique(keys.begin(), keys.end(), equal_key())));
}

double CalcAreaBySizeDepth(FeatureType & f)
{
  // Calculate depth based on areas' bbox sizes instead of style-set priorities.
  m2::RectD const r = f.GetLimitRectChecked();
  // Raw areas' size range is about (1e-11, 3000).
  double const areaSize = r.SizeX() * r.SizeY();
  // Use log2() to have more precision distinguishing smaller areas.
  double const areaSizeCompact = std::log2(areaSize);
  // Compacted range is approx (-37;13).
  double constexpr minSize = -37,
                   maxSize = 13,
                   stretchFactor = kDepthRangeBgBySize / (maxSize - minSize);
  // Adjust the range to fit into [kBaseDepthBgBySize;kBaseDepthBgTop).
  double const areaDepth = kBaseDepthBgBySize + (maxSize - areaSizeCompact) * stretchFactor;
  ASSERT(kBaseDepthBgBySize <= areaDepth && areaDepth < kBaseDepthBgTop,
         (areaDepth, areaSize, areaSizeCompact, f.GetID()));

  return areaDepth;
}

}
