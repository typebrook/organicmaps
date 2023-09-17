#include "drape_frontend/stylist.hpp"

#include "indexer/classificator.hpp"
#include "indexer/feature.hpp"
#include "indexer/feature_visibility.hpp"
#include "indexer/scales.hpp"

#include <algorithm>
#include <limits>

namespace df
{

IsHatchingTerritoryChecker::IsHatchingTerritoryChecker()
{
  Classificator const & c = classif();

  base::StringIL const arr3[] = {
    {"boundary", "protected_area", "1"},
  };
  for (auto const & sl : arr3)
    m_types.push_back(c.GetTypeByPath(sl));
  m_type3end = m_types.size();

  base::StringIL const arr2[] = {
    {"leisure", "nature_reserve"},
    {"boundary", "national_park"},
    {"landuse", "military"},
  };
  for (auto const & sl : arr2)
    m_types.push_back(c.GetTypeByPath(sl));
}

bool IsHatchingTerritoryChecker::IsMatched(uint32_t type) const
{
  // Matching with subtypes (see Stylist_IsHatching test).

  auto const iEnd3 = m_types.begin() + m_type3end;
  if (std::find(m_types.begin(), iEnd3, PrepareToMatch(type, 3)) != iEnd3)
    return true;
  return std::find(iEnd3, m_types.end(), PrepareToMatch(type, 2)) != m_types.end();
}

void CaptionDescription::Init(FeatureType & f, int8_t deviceLang, int const zoomLevel,
                              feature::GeomType const geomType, bool const auxCaptionExists)
{
  feature::NameParamsOut out;
  // TODO: remove forced secondary text for all lines and set it via styles for major roads and rivers only.
  // ATM even minor paths/streams/etc use secondary which makes their pathtexts take much more space.
  if (zoomLevel > scales::GetUpperWorldScale() && (auxCaptionExists || geomType == feature::GeomType::Line))
  {
    // Get both primary and secondary/aux names.
    f.GetPreferredNames(true /* allowTranslit */, deviceLang, out);
    m_auxText = out.secondary;
  }
  else
  {
    // Returns primary name only.
    f.GetReadableName(true /* allowTranslit */, deviceLang, out);
  }
  m_mainText = out.GetPrimary();
  ASSERT(!m_auxText.empty() && !m_mainText.empty() || m_auxText.empty(), ("auxText without mainText"));

  uint8_t constexpr kLongCaptionsMaxZoom = 4;
  size_t constexpr kLowWorldMaxTextSize = 50;
  if (zoomLevel <= kLongCaptionsMaxZoom && m_mainText.size() > kLowWorldMaxTextSize)
  {
    m_mainText.clear();
    m_auxText.clear();
    return;
  }

  // Set max text size to avoid VB/IB overflow in rendering.
  size_t constexpr kMaxTextSize = 200;
  if (m_mainText.size() > kMaxTextSize)
    m_mainText = m_mainText.substr(0, kMaxTextSize) + "...";

  uint8_t constexpr kHousenumbersMinZoom = 16; // hardcoded for optimization
  if (geomType != feature::GeomType::Line && zoomLevel >= kHousenumbersMinZoom &&
      (auxCaptionExists || m_mainText.empty()))
  {
    // TODO: its not obvious that a housenumber display is dependent on a secondary caption drule existance in styles.
    m_houseNumberText = f.GetHouseNumber();
    if (!m_houseNumberText.empty() && !m_mainText.empty() && m_houseNumberText.find(m_mainText) != std::string::npos)
      m_mainText.clear();
  }
}

void Stylist::ProcessKey(FeatureType & f, drule::Key const & key)
{
  drule::BaseRule const * const dRule = drule::rules().Find(key);

  auto const geomType = f.GetGeomType();
  switch (key.m_type)
  {
  case drule::symbol:
    ASSERT(dRule->GetSymbol() != nullptr && m_symbolRule == nullptr &&
           (geomType == feature::GeomType::Point || geomType == feature::GeomType::Area),
           (m_symbolRule == nullptr, geomType, f.DebugString(0, true)));
    m_symbolRule = dRule;
    break;
  case drule::pathtext:
  case drule::caption:
    ASSERT(dRule->GetCaption(0) != nullptr, (f.DebugString(0, true)));
    m_auxCaptionFound = dRule->GetCaption(1) != nullptr;
    if (key.m_type == drule::caption)
    {
      ASSERT(m_captionRule == nullptr && (geomType == feature::GeomType::Point || geomType == feature::GeomType::Area),
             (geomType, f.DebugString(0, true)));
      m_captionRule = dRule;
    }
    else
    {
      ASSERT(m_pathtextRule == nullptr && geomType == feature::GeomType::Line,
             (geomType, f.DebugString(0, true)));
      m_pathtextRule = dRule;
    }
    break;
  case drule::shield:
    ASSERT(dRule->GetShield() != nullptr && m_shieldRule == nullptr && geomType == feature::GeomType::Line,
           (m_shieldRule == nullptr, geomType, f.DebugString(0, true)));
    m_shieldRule = dRule;
    break;
  case drule::line:
    ASSERT(dRule->GetLine() != nullptr && geomType == feature::GeomType::Line, (geomType, f.DebugString(0, true)));
    m_lineRules.push_back(dRule);
    break;
  case drule::area:
    ASSERT(dRule->GetArea() != nullptr && geomType == feature::GeomType::Area, (geomType, f.DebugString(0, true)));
    if (key.m_hatching)
    {
      ASSERT(m_hatchingRule == nullptr, (f.DebugString(0, true)));
      m_hatchingRule = dRule;
    }
    else
    {
      ASSERT(m_areaRule == nullptr, (f.DebugString(0, true)));
      m_areaRule = dRule;
    }
    break;
  // TODO : check if circle/waymarker support exists still (not used in styles ATM).
  case drule::circle:
  case drule::waymarker:
  default:
    ASSERT(false, (key.m_type, f.DebugString(0, true)));
    return;
  }
}

Stylist::Stylist(FeatureType & f, uint8_t const zoomLevel, int8_t deviceLang)
{
  feature::TypesHolder const types(f);
  Classificator const & cl = classif();

  uint32_t mainOverlayType = 0;
  if (types.Size() == 1)
    mainOverlayType = *types.cbegin();
  else
  {
    // Determine main overlays type by priority. Priorities might be different across zoom levels
    // so a max value across all zooms is used to make sure main type doesn't change.
    int overlaysMaxPriority = std::numeric_limits<int>::min();
    for (uint32_t t : types)
    {
      int const priority = cl.GetObject(t)->GetMaxOverlaysPriority();
      if (priority > overlaysMaxPriority)
      {
        overlaysMaxPriority = priority;
        mainOverlayType = t;
      }
    }
  }

  auto const & hatchingChecker = IsHatchingTerritoryChecker::Instance();
  auto const geomType = types.GetGeomType();

  drule::KeysT keys;
  for (uint32_t t : types)
  {
    drule::KeysT typeKeys;
    cl.GetObject(t)->GetSuitable(zoomLevel, geomType, typeKeys);
    bool const hasHatching = hatchingChecker(t);

    for (auto & k : typeKeys)
    {
      // Take overlay drules from the main type only.
      if (t == mainOverlayType ||
          (k.m_type != drule::caption && k.m_type != drule::symbol &&
           k.m_type != drule::shield && k.m_type != drule::pathtext))
      {
        if (hasHatching && k.m_type == drule::area)
          k.m_hatching = true;
        keys.push_back(k);
      }
    }
  }

  feature::FilterRulesByRuntimeSelector(f, zoomLevel, keys);

  if (keys.empty())
    return;

  // Leave only one area drule and an optional hatching drule.
  drule::MakeUnique(keys);

  for (auto const & key : keys)
    ProcessKey(f, key);

  if (m_captionRule != nullptr || m_pathtextRule != nullptr)
  {
    m_captionDescriptor.Init(f, deviceLang, zoomLevel, geomType, m_auxCaptionFound);

    if (m_captionDescriptor.IsHouseNumberExists())
    {
      bool isGood = true;
      if (zoomLevel < scales::GetUpperStyleScale())
      {
        if (geomType == feature::GeomType::Area)
        {
          // Don't display housenumbers when an area (e.g. a building) is too small.
          m2::RectD const r = f.GetLimitRect(zoomLevel);
          isGood = std::min(r.SizeX(), r.SizeY()) > scales::GetEpsilonForHousenumbers(zoomLevel);
        }
        else
        {
          // Limit point housenumbers display to detailed zooms only (z18-).
          ASSERT_EQUAL(geomType, feature::GeomType::Point, ());
          isGood = zoomLevel >= scales::GetPointHousenumbersScale();
        }
      }

      if (isGood)
      {
        // Use building-address' caption drule to display house numbers.
        static auto const addressType = cl.GetTypeByPath({"building", "address"});
        if (mainOverlayType == addressType)
        {
          // Optimization: just duplicate the drule if the main type is building-address.
          ASSERT(m_captionRule != nullptr, ());
          m_houseNumberRule = m_captionRule;
        }
        else
        {
          drule::KeysT addressKeys;
          cl.GetObject(addressType)->GetSuitable(zoomLevel, geomType, addressKeys);
          if (!addressKeys.empty())
          {
            // A caption drule exists for this zoom level.
            ASSERT(addressKeys.size() == 1 && addressKeys[0].m_type == drule::caption,
                   ("building-address should contain a caption drule only"));
            m_houseNumberRule = drule::rules().Find(addressKeys[0]);
          }
        }
      }
    }

    if (!m_captionDescriptor.IsNameExists())
    {
      m_captionRule = nullptr;
      m_pathtextRule = nullptr;
    }
  }

  m_isCoastline = types.Has(cl.GetCoastType());
}

}  // namespace df
