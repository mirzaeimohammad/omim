#include "routing/route.hpp"

#include "routing/turns_generator.hpp"

#include "traffic/speed_groups.hpp"

#include "indexer/feature_altitude.hpp"

#include "geometry/mercator.hpp"

#include "platform/location.hpp"

#include "geometry/angles.hpp"
#include "geometry/point2d.hpp"
#include "geometry/simplification.hpp"

#include "base/logging.hpp"

#include "std/numeric.hpp"

#include <algorithm>
#include <utility>

using namespace traffic;
using namespace routing::turns;

namespace routing
{
namespace
{
double constexpr kLocationTimeThreshold = 60.0 * 1.0;
double constexpr kOnEndToleranceM = 10.0;
double constexpr kSteetNameLinkMeters = 400.;
}  //  namespace

Route::Route(string const & router, vector<m2::PointD> const & points, string const & name)
  : m_router(router), m_routingSettings(GetCarRoutingSettings()),
    m_name(name), m_poly(points.begin(), points.end())
{
  Update();
}

void Route::Swap(Route & rhs)
{
  m_router.swap(rhs.m_router);
  swap(m_routingSettings, rhs.m_routingSettings);
  m_poly.Swap(rhs.m_poly);
  m_simplifiedPoly.Swap(rhs.m_simplifiedPoly);
  m_name.swap(rhs.m_name);
  swap(m_currentTime, rhs.m_currentTime);
  swap(m_turns, rhs.m_turns);
  swap(m_times, rhs.m_times);
  swap(m_streets, rhs.m_streets);
  m_absentCountries.swap(rhs.m_absentCountries);
  m_altitudes.swap(rhs.m_altitudes);
  m_traffic.swap(rhs.m_traffic);
  m_colors.swap(rhs.m_colors);
}

void Route::AddAbsentCountry(string const & name)
{
  if (!name.empty()) m_absentCountries.insert(name);
}

double Route::GetTotalDistanceMeters() const
{
  if (!m_poly.IsValid())
    return 0.0;
  return m_poly.GetTotalDistanceM();
}

double Route::GetCurrentDistanceFromBeginMeters() const
{
  if (!m_poly.IsValid())
    return 0.0;
  return m_poly.GetDistanceFromBeginM();
}

void Route::GetTurnsDistances(vector<double> & distances) const
{
  distances.clear();
  if (!m_poly.IsValid())
    return;

  double mercatorDistance = 0.0;
  auto const & polyline = m_poly.GetPolyline();
  for (auto currentTurn = m_turns.begin(); currentTurn != m_turns.end(); ++currentTurn)
  {
    // Skip turns at side points of the polyline geometry. We can't display them properly.
    if (currentTurn->m_index == 0 || currentTurn->m_index == (polyline.GetSize() - 1))
      continue;

    uint32_t formerTurnIndex = 0;
    if (currentTurn != m_turns.begin())
      formerTurnIndex = (currentTurn - 1)->m_index;

    //TODO (ldragunov) Extract CalculateMercatorDistance higher to avoid including turns generator.
    double const mercatorDistanceBetweenTurns =
      CalculateMercatorDistanceAlongPath(formerTurnIndex,  currentTurn->m_index, polyline.GetPoints());
    mercatorDistance += mercatorDistanceBetweenTurns;

    distances.push_back(mercatorDistance);
   }
}

double Route::GetCurrentDistanceToEndMeters() const
{
  if (!m_poly.IsValid())
    return 0.0;
  return m_poly.GetDistanceToEndM();
}

double Route::GetMercatorDistanceFromBegin() const
{
  //TODO Maybe better to return FollowedRoute and user will call GetMercatorDistance etc. by itself
  return m_poly.GetMercatorDistanceFromBegin();
}

uint32_t Route::GetTotalTimeSec() const
{
  return m_times.empty() ? 0 : m_times.back().second;
}

uint32_t Route::GetCurrentTimeToEndSec() const
{
  size_t const polySz = m_poly.GetPolyline().GetSize();
  if (m_times.empty() || polySz == 0)
  {
    ASSERT(!m_times.empty(), ());
    ASSERT(polySz != 0, ());
    return 0;
  }

  TTimes::const_iterator it = upper_bound(m_times.begin(), m_times.end(), m_poly.GetCurrentIter().m_ind,
                                         [](size_t v, Route::TTimeItem const & item) { return v < item.first; });

  if (it == m_times.end())
    return 0;

  size_t idx = distance(m_times.begin(), it);
  double time = (*it).second;
  if (idx > 0)
    time -= m_times[idx - 1].second;

  auto distFn = [&](size_t start, size_t end)
  {
    return m_poly.GetDistanceM(m_poly.GetIterToIndex(start), m_poly.GetIterToIndex(end));
  };

  ASSERT_LESS(m_times[idx].first, polySz, ());
  double const dist = distFn(idx > 0 ? m_times[idx - 1].first : 0, m_times[idx].first);

  if (!my::AlmostEqualULPs(dist, 0.))
  {
    double const distRemain = distFn(m_poly.GetCurrentIter().m_ind, m_times[idx].first) -
                                     MercatorBounds::DistanceOnEarth(m_poly.GetCurrentIter().m_pt,
                                     m_poly.GetPolyline().GetPoint(m_poly.GetCurrentIter().m_ind));
    return (uint32_t)((GetTotalTimeSec() - (*it).second) + (double)time * (distRemain / dist));
  }
  else
    return (uint32_t)((GetTotalTimeSec() - (*it).second));
}

Route::TTurns::const_iterator Route::GetCurrentTurn() const
{
  ASSERT(!m_turns.empty(), ());

  TurnItem t;
  t.m_index = static_cast<uint32_t>(m_poly.GetCurrentIter().m_ind);
  return upper_bound(m_turns.cbegin(), m_turns.cend(), t,
         [](TurnItem const & lhs, TurnItem const & rhs)
         {
           return lhs.m_index < rhs.m_index;
         });
}

void Route::GetCurrentStreetName(string & name) const
{
  auto it = GetCurrentStreetNameIterAfter(m_poly.GetCurrentIter());
  if (it == m_streets.cend())
    name.clear();
  else
    name = it->second;
}

void Route::GetStreetNameAfterIdx(uint32_t idx, string & name) const
{
  name.clear();
  auto polyIter = m_poly.GetIterToIndex(idx);
  auto it = GetCurrentStreetNameIterAfter(polyIter);
  if (it == m_streets.cend())
    return;
  for (;it != m_streets.cend(); ++it)
    if (!it->second.empty())
    {
      if (m_poly.GetDistanceM(polyIter, m_poly.GetIterToIndex(max(it->first, static_cast<uint32_t>(polyIter.m_ind)))) < kSteetNameLinkMeters)
        name = it->second;
      return;
    }
}

Route::TStreets::const_iterator Route::GetCurrentStreetNameIterAfter(FollowedPolyline::Iter iter) const
{
  // m_streets empty for pedestrian router.
  if (m_streets.empty())
  {
    return m_streets.cend();
  }

  TStreets::const_iterator curIter = m_streets.cbegin();
  TStreets::const_iterator prevIter = curIter;
  curIter++;

  while (curIter->first < iter.m_ind)
  {
    ++prevIter;
    ++curIter;
    if (curIter == m_streets.cend())
      return curIter;
  }
  return curIter->first == iter.m_ind ? curIter : prevIter;
}

bool Route::GetCurrentTurn(double & distanceToTurnMeters, TurnItem & turn) const
{
  auto it = GetCurrentTurn();
  if (it == m_turns.end())
  {
    ASSERT(false, ());
    return false;
  }

  size_t const segIdx = (*it).m_index;
  turn = (*it);
  distanceToTurnMeters = m_poly.GetDistanceM(m_poly.GetCurrentIter(),
                                             m_poly.GetIterToIndex(segIdx));
  return true;
}

bool Route::GetNextTurn(double & distanceToTurnMeters, TurnItem & turn) const
{
  auto it = GetCurrentTurn();
  auto const turnsEnd = m_turns.end();
  ASSERT(it != turnsEnd, ());

  if (it == turnsEnd || (it + 1) == turnsEnd)
  {
    turn = TurnItem();
    distanceToTurnMeters = 0;
    return false;
  }

  it += 1;
  turn = *it;
  distanceToTurnMeters = m_poly.GetDistanceM(m_poly.GetCurrentIter(),
                                             m_poly.GetIterToIndex(it->m_index));
  return true;
}

bool Route::GetNextTurns(vector<TurnItemDist> & turns) const
{
  TurnItemDist currentTurn;
  if (!GetCurrentTurn(currentTurn.m_distMeters, currentTurn.m_turnItem))
    return false;

  turns.clear();
  turns.emplace_back(move(currentTurn));

  TurnItemDist nextTurn;
  if (GetNextTurn(nextTurn.m_distMeters, nextTurn.m_turnItem))
    turns.emplace_back(move(nextTurn));
  return true;
}

void Route::GetCurrentDirectionPoint(m2::PointD & pt) const
{
  if (m_routingSettings.m_keepPedestrianInfo && m_simplifiedPoly.IsValid())
    m_simplifiedPoly.GetCurrentDirectionPoint(pt, kOnEndToleranceM);
  else
    m_poly.GetCurrentDirectionPoint(pt, kOnEndToleranceM);
}

bool Route::MoveIterator(location::GpsInfo const & info) const
{
  double predictDistance = -1.0;
  if (m_currentTime > 0.0 && info.HasSpeed())
  {
    /// @todo Need to distinguish GPS and WiFi locations.
    /// They may have different time metrics in case of incorrect system time on a device.
    double const deltaT = info.m_timestamp - m_currentTime;
    if (deltaT > 0.0 && deltaT < kLocationTimeThreshold)
      predictDistance = info.m_speed * deltaT;
  }

  m2::RectD const rect = MercatorBounds::MetresToXY(
        info.m_longitude, info.m_latitude,
        max(m_routingSettings.m_matchingThresholdM, info.m_horizontalAccuracy));
  FollowedPolyline::Iter const res = m_poly.UpdateProjectionByPrediction(rect, predictDistance);
  if (m_simplifiedPoly.IsValid())
    m_simplifiedPoly.UpdateProjectionByPrediction(rect, predictDistance);
  return res.IsValid();
}

double Route::GetPolySegAngle(size_t ind) const
{
  size_t const polySz = m_poly.GetPolyline().GetSize();

  if (ind + 1 >= polySz)
  {
    ASSERT(false, ());
    return 0;
  }

  m2::PointD const p1 = m_poly.GetPolyline().GetPoint(ind);
  m2::PointD p2;
  size_t i = ind + 1;
  do
  {
    p2 = m_poly.GetPolyline().GetPoint(i);
  }
  while (m2::AlmostEqualULPs(p1, p2) && ++i < polySz);
  return (i == polySz) ? 0 : my::RadToDeg(ang::AngleTo(p1, p2));
}

void Route::MatchLocationToRoute(location::GpsInfo & location, location::RouteMatchingInfo & routeMatchingInfo) const
{
  if (m_poly.IsValid())
  {
    auto const & iter = m_poly.GetCurrentIter();
    m2::PointD const locationMerc = MercatorBounds::FromLatLon(location.m_latitude, location.m_longitude);
    double const distFromRouteM = MercatorBounds::DistanceOnEarth(iter.m_pt, locationMerc);
    if (distFromRouteM < m_routingSettings.m_matchingThresholdM)
    {
      location.m_latitude = MercatorBounds::YToLat(iter.m_pt.y);
      location.m_longitude = MercatorBounds::XToLon(iter.m_pt.x);
      if (m_routingSettings.m_matchRoute)
        location.m_bearing = location::AngleToBearing(GetPolySegAngle(iter.m_ind));

      routeMatchingInfo.Set(iter.m_pt, iter.m_ind, GetMercatorDistanceFromBegin());
    }
  }
}

bool Route::IsCurrentOnEnd() const
{
  return (m_poly.GetDistanceToEndM() < kOnEndToleranceM);
}

void Route::Update()
{
  if (!m_poly.IsValid())
    return;
  if (m_routingSettings.m_keepPedestrianInfo)
  {
    vector<m2::PointD> points;
    auto distFn = m2::DistanceToLineSquare<m2::PointD>();
    // TODO (ldargunov) Rewrite dist f to distance in meters and avoid 0.00000 constants.
    SimplifyNearOptimal(20, m_poly.GetPolyline().Begin(), m_poly.GetPolyline().End(), 0.00000001, distFn,
                        MakeBackInsertFunctor(points));
    FollowedPolyline(points.begin(), points.end()).Swap(m_simplifiedPoly);
  }
  else
  {
    // Free memory if we don't need simplified geometry.
    FollowedPolyline().Swap(m_simplifiedPoly);
  }
  m_currentTime = 0.0;
}

void Route::AppendTraffic(Route const & route)
{
  CHECK(route.IsValid(), ());

  if (GetTraffic().empty() && route.GetTraffic().empty())
    return;

  if (!IsValid())
  {
    m_traffic = route.GetTraffic();
    return;
  }

  // Note. At this point the last item of |m_poly| should be removed.
  // So the size of |m_traffic| should be equal to size of |m_poly|.
  if (GetTraffic().empty())
    m_traffic.resize(m_poly.GetPolyline().GetSize(), SpeedGroup::Unknown);

  CHECK_EQUAL(GetTraffic().size(), m_poly.GetPolyline().GetSize(), ());

  if (route.GetTraffic().empty())
  {
    CHECK_GREATER_OR_EQUAL(route.m_poly.GetPolyline().GetSize(), 1, ());
    // Note. It's necessary to deduct 1 because size of |route.m_poly|
    // is one less then number of segments of |route.m_poly|. And if |route.m_traffic|
    // were not empty it would had had route.m_poly.GetPolyline().GetSize() - 1 items.
    m_traffic.insert(m_traffic.end(),
                     route.m_poly.GetPolyline().GetSize() - 1 /* number of segments is less by one */,
                     SpeedGroup::Unknown);
  }
  else
  {
    m_traffic.insert(m_traffic.end(), route.GetTraffic().cbegin(), route.GetTraffic().cend());
  }
}

void Route::AppendRoute(Route const & route)
{
  if (!route.IsValid())
    return;

  double const estimatedTime = m_times.empty() ? 0.0 : m_times.back().second;
  if (m_poly.GetPolyline().GetSize() != 0)
  {
    ASSERT(!m_turns.empty(), ());
    ASSERT(!m_times.empty(), ());
    if (!m_streets.empty())
      ASSERT_LESS(m_streets.back().first + 1, m_poly.GetPolyline().GetSize(), ());

    // Remove road end point and turn instruction.
    ASSERT_LESS(MercatorBounds::DistanceOnEarth(m_poly.End().m_pt, route.m_poly.Begin().m_pt),
                2 /* meters */, ());
    m_poly.PopBack();
    CHECK(!m_turns.empty(), ());
    ASSERT_EQUAL(m_turns.back().m_turn, TurnDirection::ReachedYourDestination, ());
    m_turns.pop_back();
    CHECK(!m_times.empty(), ());
    m_times.pop_back();
  }

  size_t const indexOffset = m_poly.GetPolyline().GetSize();

  // Appending turns.
  for (auto t : route.m_turns)
  {
    if (t.m_index == 0)
      continue;
    t.m_index += indexOffset;
    m_turns.push_back(move(t));
  }

  // Appending street names.
  for (auto s : route.m_streets)
  {
    if (s.first == 0)
      continue;
    s.first += indexOffset;
    m_streets.push_back(move(s));
  }

  // Appending times.
  for (auto t : route.m_times)
  {
    if (t.first == 0)
      continue;
    t.first += indexOffset;
    t.second += estimatedTime;
    m_times.push_back(move(t));
  }

  AppendTraffic(route);

  m_poly.Append(route.m_poly);
  if (!GetTraffic().empty())
  {
    CHECK_EQUAL(GetTraffic().size() + 1, m_poly.GetPolyline().GetSize(), ());
  }
  Update();
}

// Subroute interface fake implementation ---------------------------------------------------------
// This implementation is valid for one subroute which is equal to the route.
size_t Route::GetSubrouteCount() const { return IsValid() ? 1 : 0; }

void Route::GetSubrouteInfo(size_t segmentIdx, vector<Route::SegmentInfo> & info) const
{
  CHECK_LESS(segmentIdx, GetSubrouteCount(), ());
  CHECK(IsValid(), ());
  info.clear();

  vector<m2::PointD> const & points = m_poly.GetPolyline().GetPoints();
  size_t const polySz = m_poly.GetPolyline().GetSize();

  CHECK(!m_turns.empty(), ());
  CHECK_LESS(m_turns.back().m_index, polySz, ());
  CHECK(std::is_sorted(m_turns.cbegin(), m_turns.cend(),
            [](TurnItem const & lhs, TurnItem const & rhs) { return lhs.m_index < rhs.m_index; }),
        ());

  if (!m_altitudes.empty())
    CHECK_EQUAL(m_altitudes.size(), polySz, ());

  CHECK(!m_times.empty(), ());
  CHECK_LESS(m_times.back().first, polySz, ());
  CHECK(std::is_sorted(m_times.cbegin(), m_times.cend(),
            [](TTimeItem const & lhs, TTimeItem const & rhs) { return lhs.first < rhs.first; }),
        ());

  if (!m_traffic.empty())
    CHECK_EQUAL(m_traffic.size() + 1, polySz, ());

  // |m_index| of the first turn may be equal to zero. If there's a turn at very beginning of the route.
  // The turn should be skipped in case of segement oriented route which is filled by the method.
  size_t turnItemIdx = (m_turns[0].m_index == 0 ? 1 : 0);
  size_t timeIdx = (m_times[0].first ? 1 : 0);
  double distFromBeginningMeters = 0.0;
  double distFromBeginningMerc = 0.0;
  info.reserve(polySz - 1);
  for (size_t i = 1; i < points.size(); ++i)
  {
    TurnItem turn;
    if (m_turns[turnItemIdx].m_index == i)
    {
      turn = m_turns[turnItemIdx];
      ++turnItemIdx;
    }
    CHECK_LESS_OR_EQUAL(turnItemIdx, m_turns.size(), ());

    if (m_times[timeIdx].first == i)
      ++timeIdx;

    distFromBeginningMeters += MercatorBounds::DistanceOnEarth(points[i - 1], points[i]);
    distFromBeginningMerc += points[i - 1].Length(points[i]);

    info.emplace_back(
        Segment(), turn,
        Junction(points[i], m_altitudes.empty() ? feature::kInvalidAltitude : m_altitudes[i]),
        string(), distFromBeginningMeters,
        distFromBeginningMerc, m_times[timeIdx - 1].second,
        m_traffic.empty() ? SpeedGroup::Unknown : m_traffic[i - 1]);
  }
}

Route::SubrouteSettings const Route::GetSubrouteSettings(size_t segmentIdx) const
{
  CHECK_LESS(segmentIdx, GetSubrouteCount(), ());
  return SubrouteSettings(m_routingSettings, m_router, m_subrouteUid);
}

void Route::SetSubrouteUid(size_t segmentIdx, SubrouteUid subrouteUid)
{
  CHECK_LESS(segmentIdx, GetSubrouteCount(), ());
  m_subrouteUid = subrouteUid;
}

string DebugPrint(Route const & r)
{
  return DebugPrint(r.m_poly.GetPolyline());
}
} // namespace routing
