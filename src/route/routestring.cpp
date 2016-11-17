/*****************************************************************************
* Copyright 2015-2016 Alexander Barthel albar965@mailbox.org
*
* This program is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program.  If not, see <http://www.gnu.org/licenses/>.
*****************************************************************************/

#include "route/routestring.h"

#include "route/routemapobjectlist.h"
#include "mapgui/mapquery.h"
#include "route/flightplanentrybuilder.h"
#include "common/maptools.h"

#include <QRegularExpression>

using atools::fs::pln::Flightplan;
using atools::fs::pln::FlightplanEntry;

const float MAX_WAYPOINT_DISTANCE_NM = 1000.f;

RouteString::RouteString(MapQuery *mapQuery)
  : query(mapQuery)
{
  entryBuilder = new FlightplanEntryBuilder(query);
}

RouteString::~RouteString()
{
  delete entryBuilder;
}

QString RouteString::createStringForRoute(const atools::fs::pln::Flightplan& flightplan)
{
  QStringList retval;
  QString lastAw, lastId;
  for(int i = 0; i < flightplan.getEntries().size(); i++)
  {
    const FlightplanEntry& entry = flightplan.at(i);
    const QString& airway = entry.getAirway();
    const QString& ident = entry.getIcaoIdent();

    if(airway.isEmpty())
    {
      if(!lastId.isEmpty())
        retval.append(lastId);
      if(i > 0)
        // Add direct if not start airport
        retval.append("DCT");
    }
    else
    {
      if(airway != lastAw)
      {
        // Airways change add last ident of the last airway
        retval.append(lastId);
        retval.append(airway);
      }
      // else Airway is the same skip waypoint
    }

    lastId = ident;
    lastAw = airway;
  }

  // Add destination
  retval.append(lastId);

  return retval.join(" ");
}

bool RouteString::createRouteFromString(const QString& routeString, atools::fs::pln::Flightplan& flightplan)
{
  errors.clear();
  QStringList list = cleanRouteString(routeString).split(" ");
  qDebug() << "parsing" << list;

  if(list.size() < 2)
  {
    appendError(tr("Flight plan string is too small. Need at least departure and destination."));
    return false;
  }

  if(list.size() > 2 && (list.at(1) == "SID" || list.at(1) == "STAR" ||
                         list.at(list.size() - 2) == "SID" || list.at(list.size() - 2) == "STAR"))
    appendError(tr("SID and STAR are not supported. Replacing with direct (DCT)."));

  bool direct = false;
  maptypes::MapAirway currentAirway;
  maptypes::MapSearchResult lastResult;
  atools::geo::Pos lastPos;

  if(!addDeparture(flightplan, list.first()))
    return false;

  if(!addDestination(flightplan, list.last()))
    return false;

  float maxDistance = std::max(MAX_WAYPOINT_DISTANCE_NM, flightplan.getDistanceNm() + 1.5f);

  lastPos = flightplan.getDeparturePosition();

  for(int i = 1; i < list.size() - 1; i++)
  {
    const QString& strItem = list.at(i);

    bool waypoint = (i % 2) == 0;
    qDebug() << "waypoint" << waypoint << "direct" << direct << currentAirway.name << "->" << strItem;

    if(waypoint)
    {
      // Reading navaid string now ==================

      // Waypoint / VOR / NDB ==================
      maptypes::MapSearchResult result;
      QList<maptypes::MapWaypoint> airwayWaypoints;
      QList<maptypes::MapAirwayWaypoint> allAirwayWaypoints;

      if(!direct)
      {
        // Airway routing - only waypoints ======================
        if(!lastResult.waypoints.isEmpty())
        {
          // Get all airways that match name and waypoint - normally only one
          query->getWaypointsForAirway(result.waypoints, currentAirway.name, strItem);
          maptools::removeFarthest(lastPos, result.waypoints);

          if(!result.waypoints.isEmpty())
          {
            // Get all waypoints for the airway sorted by fragment and sequence
            query->getWaypointListForAirwayName(allAirwayWaypoints, currentAirway.name);

            if(!allAirwayWaypoints.isEmpty())
            {
              int startIndex = -1, endIndex = -1;
              // Find the waypoint indexes by id in the list of all waypoints for this airway
              findIndexesInAirway(allAirwayWaypoints, result.waypoints.first().id,
                                  lastResult.waypoints.first().id, startIndex, endIndex);

              qDebug() << "startidx" << startIndex << "endidx" << endIndex;
              if(startIndex != -1 && endIndex != -1)
                // Get the list of waypoints from start to end index
                extractWaypoints(allAirwayWaypoints, startIndex, endIndex, airwayWaypoints);
              else
              {
                if(startIndex == -1)
                  appendError(tr("Waypoint %1 not found in airway %2. Ignoring flight plan segment.").
                              arg(lastResult.waypoints.first().ident).
                              arg(currentAirway.name));

                if(endIndex == -1)
                  appendError(tr("Waypoint %1 not found in airway %2. Ignoring flight plan segment.").
                              arg(result.waypoints.first().ident).
                              arg(currentAirway.name));

                direct = true;
              }
            }
            else
            {
              appendError(tr("No waypoints found for airway %1. Ignoring flight plan segment.").
                          arg(currentAirway.name));
              direct = true;
            }
          }
          else
          {
            appendError(tr("No waypoint %1 found at airway %2. Ignoring flight plan segment.").
                        arg(strItem).arg(currentAirway.name));
            direct = true;
          }
        }
        else
          direct = true;
      }

      if(direct)
      {
        // Direct connection as requested by DCT or by missing airway/waypoint
        airwayWaypoints.clear();

        // Find a waypoint, VOR or NDB that are not too far away
        findBestNavaid(strItem, lastPos, maxDistance, result);
      }

      // Add all airway waypoints if any were found
      FlightplanEntry entry;
      for(const maptypes::MapWaypoint& wp : airwayWaypoints)
      {
        // Reset but keep the last one
        entry = FlightplanEntry();

        // Will fetch objects in order: airport, waypoint, vor, ndb
        entryBuilder->entryFromWaypoint(wp, entry, true);
        if(entry.getPosition().isValid())
        {
          entry.setAirway(currentAirway.name);
          flightplan.getEntries().insert(flightplan.getEntries().size() - 1, entry);
        }
        else
          qDebug() << "No navaid found for" << strItem << "on airway" << currentAirway.name;
      }

      // Replace last result only if something was found
      if(!result.waypoints.isEmpty() || !result.vors.isEmpty() || !result.ndbs.isEmpty())
        lastResult = result;

      if(airwayWaypoints.isEmpty())
      {
        // Add a single waypoint from direct
        entry = FlightplanEntry();
        entryBuilder->buildFlightplanEntry(result, entry, true);
        if(entry.getPosition().isValid())
          flightplan.getEntries().insert(flightplan.getEntries().size() - 1, entry);
        else
          appendError(tr("No navaid found for %1. Ignoring.").arg(strItem));
      }

      if(entry.getPosition().isValid())
        // Remember position for distance calculation
        lastPos = entry.getPosition();
      direct = false;
    }
    else
    {
      // Reading airway string now ==================
      if(strItem == "SID" || strItem == "STAR" || strItem == "DCT" || strItem == "DIRECT")
        // Direct connection ==================
        direct = true;
      else
      {
        // Airway connection ==================
        maptypes::MapSearchResult result;
        query->getMapObjectByIdent(result, maptypes::AIRWAY, strItem);

        if(result.airways.isEmpty())
        {
          appendError(tr("Airway %1 not found. Using direct.").arg(strItem));
          direct = true;
        }
        else
        {
          currentAirway = result.airways.first();
          direct = false;
        }
      }
    }
  }
  return true;
}

QString RouteString::cleanRouteString(const QString& string)
{
  QString retval(string);
  retval.replace(QRegularExpression("[^A-Za-z0-9]"), " ");
  return retval.simplified().toUpper();
}

void RouteString::appendError(const QString& err)
{
  errors.append(err);
  qInfo() << "Error:" << err;
}

bool RouteString::addDeparture(atools::fs::pln::Flightplan& flightplan, const QString& airportIdent)
{
  maptypes::MapAirport departure;
  query->getAirportByIdent(departure, airportIdent);

  if(departure.position.isValid())
  {
    qDebug() << "found" << departure.ident << "id" << departure.id;

    flightplan.setDepartureAiportName(departure.name);
    flightplan.setDepartureIdent(departure.ident);
    flightplan.setDeparturePosition(departure.position);

    FlightplanEntry entry;
    entryBuilder->buildFlightplanEntry(departure, entry);
    flightplan.getEntries().append(entry);
    return true;
  }
  else
  {
    appendError(tr("Departure airport %1 not found. Reading stopped.").arg(airportIdent));
    return false;
  }
}

bool RouteString::addDestination(atools::fs::pln::Flightplan& flightplan, const QString& airportIdent)
{
  maptypes::MapAirport destination;
  query->getAirportByIdent(destination, airportIdent);
  if(destination.position.isValid())
  {
    qDebug() << "found" << destination.ident << "id" << destination.id;

    flightplan.setDestinationAiportName(destination.name);
    flightplan.setDestinationIdent(destination.ident);
    flightplan.setDestinationPosition(destination.position);

    FlightplanEntry entry;
    entryBuilder->buildFlightplanEntry(destination, entry);
    flightplan.getEntries().append(entry);
    return true;
  }
  else
  {
    appendError(tr("Destination airport %1 not found. Reading stopped.").arg(airportIdent));
    return false;
  }
}

void RouteString::findIndexesInAirway(const QList<maptypes::MapAirwayWaypoint>& allAirwayWaypoints,
                                      int curId, int lastId, int& startIndex, int& endIndex)
{
  // TODO handle fragments properly
  for(int idx = 0; idx < allAirwayWaypoints.size(); idx++)
  {
    // qDebug() << "found idx" << idx << allAirwayWaypoints.at(idx).waypoint.ident
    // << "in" << currentAirway.name;

    if(startIndex == -1 && lastId == allAirwayWaypoints.at(idx).waypoint.id)
      startIndex = idx;
    if(endIndex == -1 && curId == allAirwayWaypoints.at(idx).waypoint.id)
      endIndex = idx;
  }
}

void RouteString::extractWaypoints(const QList<maptypes::MapAirwayWaypoint>& allAirwayWaypoints,
                                   int startIndex, int endIndex,
                                   QList<maptypes::MapWaypoint>& airwayWaypoints)
{
  if(startIndex < endIndex)
  {
    for(int idx = startIndex + 1; idx <= endIndex; idx++)
    {
      // qDebug() << "adding forward idx" << idx << allAirwayWaypoints.at(idx).waypoint.ident
      // << "to" << currentAirway.name;
      airwayWaypoints.append(allAirwayWaypoints.at(idx).waypoint);
    }
  }
  else if(startIndex > endIndex)
  {
    // Reversed order
    for(int idx = startIndex - 1; idx >= endIndex; idx--)
    {
      // qDebug() << "adding reverse idx" << idx << allAirwayWaypoints.at(idx).waypoint.ident
      // << "to" << currentAirway.name;
      airwayWaypoints.append(allAirwayWaypoints.at(idx).waypoint);
    }
  }
}

void RouteString::findBestNavaid(const QString& strItem, const atools::geo::Pos& lastPos, float maxDistance,
                                 maptypes::MapSearchResult& result)
{
  query->getMapObjectByIdent(result, maptypes::WAYPOINT, strItem);
  float waypointDistNm = atools::geo::meterToNm(maptools::removeFarthest(lastPos, result.waypoints));

  if(waypointDistNm > maxDistance)
  {
    result.waypoints.clear();

    // Too far to waypoint - try VOR
    query->getMapObjectByIdent(result, maptypes::VOR, strItem);
    float vorDistNm = atools::geo::meterToNm(maptools::removeFarthest(lastPos, result.vors));

    if(vorDistNm > maxDistance)
    {
      result.vors.clear();

      // Too far to VOR either - try NDB
      query->getMapObjectByIdent(result, maptypes::NDB, strItem);
      float ndbDistNm = atools::geo::meterToNm(maptools::removeFarthest(lastPos, result.ndbs));

      if(ndbDistNm > maxDistance)
        result.ndbs.clear();
    }
  }
}