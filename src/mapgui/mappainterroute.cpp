/*****************************************************************************
* Copyright 2015-2017 Alexander Barthel albar965@mailbox.org
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

#include "mapgui/mappainterroute.h"

#include "mapgui/mapwidget.h"
#include "common/symbolpainter.h"
#include "common/unit.h"
#include "mapgui/maplayer.h"
#include "common/mapcolors.h"
#include "geo/calculations.h"
#include "route/routecontroller.h"
#include "mapgui/mapscale.h"
#include "util/paintercontextsaver.h"

#include <QBitArray>
#include <marble/GeoDataLineString.h>
#include <marble/GeoPainter.h>

using namespace Marble;
using namespace atools::geo;
using maptypes::MapApproachLeg;
using maptypes::MapApproachLegList;

MapPainterRoute::MapPainterRoute(MapWidget *mapWidget, MapQuery *mapQuery, MapScale *mapScale,
                                 RouteController *controller)
  : MapPainter(mapWidget, mapQuery, mapScale), routeController(controller)
{
}

MapPainterRoute::~MapPainterRoute()
{

}

void MapPainterRoute::render(PaintContext *context)
{
  if(!context->objectTypes.testFlag(maptypes::ROUTE))
    return;

  setRenderHints(context->painter);

  atools::util::PainterContextSaver saver(context->painter);

  paintRoute(context);
  paintApproachPreview(context, mapWidget->getTransitionHighlight(), mapWidget->getApproachHighlight());
}

void MapPainterRoute::paintRoute(const PaintContext *context)
{
  const RouteMapObjectList& routeMapObjects = routeController->getRouteMapObjects();

  if(routeMapObjects.isEmpty())
    return;

  context->painter->setBrush(Qt::NoBrush);

  // Use a layer that is independent of the declutter factor
  if(!routeMapObjects.isEmpty() && context->mapLayerEffective->isAirportDiagram())
  {
    // Draw start position or parking circle into the airport diagram
    const RouteMapObject& first = routeMapObjects.at(0);
    if(first.getMapObjectType() == maptypes::AIRPORT)
    {
      int size = 100;

      Pos startPos;
      if(routeMapObjects.hasDepartureParking())
      {
        startPos = first.getDepartureParking().position;
        size = first.getDepartureParking().radius;
      }
      else if(routeMapObjects.hasDepartureStart())
        startPos = first.getDepartureStart().position;

      if(startPos.isValid())
      {
        QPoint pt = wToS(startPos);

        if(!pt.isNull())
        {
          // At least 10 pixel size - unaffected by scaling
          int w = std::max(10, scale->getPixelIntForFeet(size, 90));
          int h = std::max(10, scale->getPixelIntForFeet(size, 0));

          context->painter->setPen(QPen(mapcolors::routeOutlineColor, 9,
                                        Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
          context->painter->drawEllipse(pt, w, h);
          context->painter->setPen(QPen(OptionData::instance().getFlightplanColor(), 5,
                                        Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
          context->painter->drawEllipse(pt, w, h);
        }
      }
    }
  }

  // Check if there is any magnetic variance on the route
  bool foundMagvarObject = false;
  for(const RouteMapObject& obj : routeMapObjects)
  {
    // Route contains correct magvar if any of these objects were found
    if(obj.getMapObjectType() == maptypes::AIRPORT || obj.getMapObjectType() == maptypes::VOR ||
       obj.getMapObjectType() == maptypes::NDB || obj.getMapObjectType() == maptypes::WAYPOINT)
      foundMagvarObject = true;
  }

  // Collect coordinates for text placement and lines first
  QList<QPoint> textCoords;
  QList<float> textBearing;
  QStringList texts;
  QList<int> textLineLengths;

  // Collect start and end points of legs and visibility
  QList<QPoint> startPoints;
  QBitArray visibleStartPoints(routeMapObjects.size(), false);
  GeoDataLineString linestring;
  linestring.setTessellate(true);

  for(int i = 0; i < routeMapObjects.size(); i++)
  {
    const RouteMapObject& obj = routeMapObjects.at(i);
    linestring.append(GeoDataCoordinates(obj.getPosition().getLonX(), obj.getPosition().getLatY(), 0, DEG));

    int x, y;
    visibleStartPoints.setBit(i, wToS(obj.getPosition(), x, y));

    if(i > 0 && !context->drawFast)
    {
      int lineLength = simpleDistance(x, y, startPoints.at(i - 1).x(), startPoints.at(i - 1).y());
      if(lineLength > MIN_LENGTH_FOR_TEXT)
      {
        // Build text
        QString text(Unit::distNm(obj.getDistanceTo(), true /*addUnit*/, 10, true /*narrow*/) + tr(" / ") +
                     QString::number(obj.getCourseToRhumb(), 'f', 0) +
                     (foundMagvarObject ? tr("°M") : tr("°T")));

        int textw = context->painter->fontMetrics().width(text);
        if(textw > lineLength)
          // Limit text length to line for elide
          textw = lineLength;

        int xt, yt;
        float brg;
        Pos p1(obj.getPosition()), p2(routeMapObjects.at(i - 1).getPosition());

        if(findTextPos(p1, p2, context->painter,
                       textw, context->painter->fontMetrics().height(), xt, yt, &brg))
        {
          textCoords.append(QPoint(xt, yt));
          textBearing.append(brg);
          texts.append(text);
          textLineLengths.append(lineLength);
        }
      }
      else
      {
        // No text - append all dummy values
        textCoords.append(QPoint());
        textBearing.append(0.f);
        texts.append(QString());
        textLineLengths.append(0);
      }
    }
    startPoints.append(QPoint(x, y));
  }

  float outerlinewidth = context->sz(context->thicknessFlightplan, 6);
  float innerlinewidth = context->sz(context->thicknessFlightplan, 4);

  // Draw lines separately to avoid omission in mercator near anti meridian
  // Draw outer line
  context->painter->setPen(QPen(mapcolors::routeOutlineColor, outerlinewidth, Qt::SolidLine,
                                Qt::RoundCap, Qt::RoundJoin));
  drawLineString(linestring, context);

  const Pos& pos = mapWidget->getUserAircraft().getPosition();

  int leg = -1;
  if(pos.isValid())
  {
    float cross;
    leg = routeMapObjects.getNearestLegIndex(pos, cross);
  }

  // Draw innner line
  context->painter->setPen(QPen(OptionData::instance().getFlightplanColor(), innerlinewidth,
                                Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
  GeoDataLineString ls;
  ls.setTessellate(true);
  for(int i = 1; i < linestring.size(); i++)
  {
    ls.clear();
    ls << linestring.at(i - 1) << linestring.at(i);

    if(leg != -1 && i == leg)
    {
      context->painter->setPen(QPen(OptionData::instance().getFlightplanActiveSegmentColor(), innerlinewidth,
                                    Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
      context->painter->drawPolyline(ls);
      context->painter->setPen(QPen(OptionData::instance().getFlightplanColor(), innerlinewidth,
                                    Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    }
    else
      context->painter->drawPolyline(ls);
  }

  context->szFont(context->textSizeFlightplan);

  if(!context->drawFast)
  {
    // Draw text with direction arrow along lines
    context->painter->setPen(QPen(Qt::black, 2, Qt::SolidLine, Qt::FlatCap));
    int i = 0;
    for(const QPoint& textCoord : textCoords)
    {
      QString text = texts.at(i);
      if(!text.isEmpty())
      {
        // Cut text right or left depending on direction
        Qt::TextElideMode elide = Qt::ElideRight;
        float rotate, brg = textBearing.at(i);
        if(brg < 180.)
        {
          text += tr(" ►");
          elide = Qt::ElideLeft;
          rotate = brg - 90.f;
        }
        else
        {
          text = tr("◄ ") + text;
          elide = Qt::ElideRight;
          rotate = brg + 90.f;
        }

        // Draw text
        QFontMetrics metrics = context->painter->fontMetrics();

        // Both points are visible - cut text for full line length
        if(visibleStartPoints.testBit(i) && visibleStartPoints.testBit(i + 1))
          text = metrics.elidedText(text, elide, textLineLengths.at(i));

        context->painter->translate(textCoord.x(), textCoord.y());
        context->painter->rotate(rotate);
        context->painter->drawText(-metrics.width(text) / 2, -metrics.descent() - outerlinewidth / 2, text);
        context->painter->resetTransform();
      }
      i++;
    }
  }

  // Draw airport and navaid symbols
  drawSymbols(context, routeMapObjects, visibleStartPoints, startPoints);

  // Draw symbol text
  drawSymbolText(context, routeMapObjects, visibleStartPoints, startPoints);

  if(routeMapObjects.size() >= 2)
  {
    // Draw the top of descent circle and text
    QPoint pt = wToS(routeMapObjects.getTopOfDescent());
    if(!pt.isNull())
    {
      float width = context->sz(context->thicknessFlightplan, 3);
      int radius = atools::roundToInt(context->sz(context->thicknessFlightplan, 6));

      context->painter->setPen(QPen(Qt::black, width, Qt::SolidLine, Qt::FlatCap));
      context->painter->drawEllipse(pt, radius, radius);

      QStringList tod;
      tod.append(tr("TOD"));
      if(context->mapLayer->isAirportRouteInfo())
        tod.append(Unit::distNm(routeMapObjects.getTopOfDescentFromDestination()));

      symbolPainter->textBox(context->painter, tod, QPen(Qt::black),
                             pt.x() + radius, pt.y() + radius,
                             textatt::ROUTE_BG_COLOR | textatt::BOLD, 255);
    }
  }
}

/* Draw approaches and transitions selected in the tree view */
void MapPainterRoute::paintApproachPreview(const PaintContext *context,
                                           const maptypes::MapApproachLegList& transition,
                                           const maptypes::MapApproachLegList& approach)
{
  GeoDataLineString linestring, missedLineString;
  linestring.setTessellate(true);
  missedLineString.setTessellate(true);

  for(const MapApproachLeg& leg : transition.legs)
  {
    if(leg.fixPos.isValid())
      linestring.append(GeoDataCoordinates(leg.fixPos.getLonX(), leg.fixPos.getLatY(), 0, DEG));
  }

  for(const MapApproachLeg& leg : approach.legs)
  {
    if(leg.missed)
    {
      if(leg.fixPos.isValid())
        missedLineString.append(GeoDataCoordinates(leg.fixPos.getLonX(), leg.fixPos.getLatY(), 0, DEG));
      break;
    }
    if(leg.fixPos.isValid())
      linestring.append(GeoDataCoordinates(leg.fixPos.getLonX(), leg.fixPos.getLatY(), 0, DEG));
  }

  for(const MapApproachLeg& leg : approach.legs)
  {
    if(leg.missed)
      if(leg.fixPos.isValid())
        missedLineString.append(GeoDataCoordinates(leg.fixPos.getLonX(), leg.fixPos.getLatY(), 0, DEG));
  }

  GeoDataLineString ls;
  ls.setTessellate(true);

  float outerlinewidth = context->sz(context->thicknessFlightplan, 7);
  float innerlinewidth = context->sz(context->thicknessFlightplan, 5);

  context->painter->setPen(QPen(mapcolors::routeApproachPreviewOutlineColor, outerlinewidth, Qt::SolidLine,
                                Qt::RoundCap, Qt::RoundJoin));
  drawLineString(linestring, context);
  context->painter->setPen(QPen(mapcolors::routeApproachPreviewColor, innerlinewidth,
                                Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
  drawLineString(linestring, context);

  context->painter->setPen(QPen(mapcolors::routeApproachPreviewOutlineColor, outerlinewidth, Qt::DotLine,
                                Qt::RoundCap, Qt::RoundJoin));
  drawLineString(missedLineString, context);
  context->painter->setPen(QPen(mapcolors::routeApproachPreviewColor, innerlinewidth,
                                Qt::DotLine, Qt::RoundCap, Qt::RoundJoin));
  drawLineString(missedLineString, context);

  // Do not paint the last point in the transition since it overlaps with the approach
  for(int i = 0; i < transition.legs.size() - 1; ++i)
    paintApproachPoint(context, transition.legs.at(i));

  for(const MapApproachLeg& leg : approach.legs)
    paintApproachPoint(context, leg);
}

void MapPainterRoute::paintApproachPoint(const PaintContext *context, const MapApproachLeg& leg)
{
  int x, y;
  if(leg.waypoint.position.isValid() && wToS(leg.waypoint.position, x, y))
  {
    paintWaypoint(context, QColor(), x, y);
    paintWaypointText(context, x, y, leg.waypoint, &leg.altRestriction);
  }
  else if(leg.ndb.position.isValid() && wToS(leg.ndb.position, x, y))
  {
    paintNdb(context, x, y);
    paintNdbText(context, x, y, leg.ndb, &leg.altRestriction);
  }
  else if(leg.vor.position.isValid() && wToS(leg.vor.position, x, y))
  {
    paintVor(context, x, y, leg.vor);
    paintVorText(context, x, y, leg.vor, &leg.altRestriction);
  }
  else if(leg.ils.position.isValid() && wToS(leg.ils.position, x, y))
  {
    paintUserpoint(context, x, y);
    paintText(context, mapcolors::routeUserPointColor, x, y, leg.ils.ident, &leg.altRestriction);
  }
  else if(wToS(leg.fixPos, x, y))
  {
    paintUserpoint(context, x, y);
    paintText(context, mapcolors::routeUserPointColor, x, y, leg.userpoint.name, &leg.altRestriction);
  }
}

void MapPainterRoute::paintAirport(const PaintContext *context, int x, int y, const maptypes::MapAirport& obj)
{
  int size = context->sz(context->symbolSizeAirport, context->mapLayerEffective->getAirportSymbolSize());
  symbolPainter->drawAirportSymbol(context->painter, obj, x, y, size, false, false);
}

void MapPainterRoute::paintAirportText(const PaintContext *context, int x, int y,
                                       const maptypes::MapAirport& obj)
{
  int size = context->sz(context->symbolSizeAirport, context->mapLayerEffective->getAirportSymbolSize());
  textflags::TextFlags flags = textflags::IDENT | textflags::ROUTE_TEXT;

  // Use more more detailed text for flight plan
  if(context->mapLayer->isAirportRouteInfo())
    flags |= textflags::NAME | textflags::INFO;

  symbolPainter->drawAirportText(context->painter, obj, x, y, context->dispOpts, flags, size,
                                 context->mapLayerEffective->isAirportDiagram());
}

void MapPainterRoute::paintVor(const PaintContext *context, int x, int y, const maptypes::MapVor& obj)
{
  int size = context->sz(context->symbolSizeNavaid, context->mapLayerEffective->getVorSymbolSize());
  symbolPainter->drawVorSymbol(context->painter, obj, x, y,
                               size, false, false,
                               context->mapLayerEffective->isVorLarge() ? size * 5 : 0);
}

void MapPainterRoute::paintVorText(const PaintContext *context, int x, int y, const maptypes::MapVor& obj,
                                   const maptypes::MapAltRestriction *altRestriction)
{
  int size = context->sz(context->symbolSizeNavaid, context->mapLayerEffective->getVorSymbolSize());
  textflags::TextFlags flags = textflags::ROUTE_TEXT;
  // Use more more detailed VOR text for flight plan
  if(context->mapLayer->isVorRouteIdent())
    flags |= textflags::IDENT;

  if(context->mapLayer->isVorRouteInfo())
    flags |= textflags::FREQ | textflags::INFO | textflags::TYPE;

  symbolPainter->drawVorText(context->painter, obj, x, y, flags, size, true, altRestriction);
}

void MapPainterRoute::paintNdb(const PaintContext *context, int x, int y)
{
  int size = context->sz(context->symbolSizeNavaid, context->mapLayerEffective->getNdbSymbolSize());
  symbolPainter->drawNdbSymbol(context->painter, x, y, size, true, false);
}

void MapPainterRoute::paintNdbText(const PaintContext *context, int x, int y, const maptypes::MapNdb& obj,
                                   const maptypes::MapAltRestriction *altRestriction)
{
  int size = context->sz(context->symbolSizeNavaid, context->mapLayerEffective->getNdbSymbolSize());
  textflags::TextFlags flags = textflags::ROUTE_TEXT;
  // Use more more detailed NDB text for flight plan
  if(context->mapLayer->isNdbRouteIdent())
    flags |= textflags::IDENT;

  if(context->mapLayer->isNdbRouteInfo())
    flags |= textflags::FREQ | textflags::INFO | textflags::TYPE;

  symbolPainter->drawNdbText(context->painter, obj, x, y, flags, size, true, altRestriction);
}

void MapPainterRoute::paintWaypoint(const PaintContext *context, const QColor& col, int x, int y)
{
  int size = context->sz(context->symbolSizeNavaid, context->mapLayerEffective->getWaypointSymbolSize());
  symbolPainter->drawWaypointSymbol(context->painter, col, x, y, size, true, false);
}

void MapPainterRoute::paintWaypointText(const PaintContext *context, int x, int y,
                                        const maptypes::MapWaypoint& obj,
                                        const maptypes::MapAltRestriction *altRestriction)
{
  int size = context->sz(context->symbolSizeNavaid, context->mapLayerEffective->getWaypointSymbolSize());
  textflags::TextFlags flags = textflags::ROUTE_TEXT;
  if(context->mapLayer->isWaypointRouteName())
    flags |= textflags::IDENT;

  symbolPainter->drawWaypointText(context->painter, obj, x, y, flags, size, true, altRestriction);
}

/* Paint user defined waypoint */
void MapPainterRoute::paintUserpoint(const PaintContext *context, int x, int y)
{
  int size = context->sz(context->symbolSizeNavaid, context->mapLayerEffective->getWaypointSymbolSize());
  symbolPainter->drawUserpointSymbol(context->painter, x, y, size, true, false);
}

/* Draw text with light yellow background for flight plan */
void MapPainterRoute::paintText(const PaintContext *context, const QColor& color, int x, int y,
                                const QString& text,
                                const maptypes::MapAltRestriction *altRestriction)
{
  int size = context->sz(context->symbolSizeNavaid, context->mapLayerEffective->getWaypointSymbolSize());

  QStringList texts;
  texts.append(text);
  symbolPainter->restrictionText(altRestriction, texts);

  if(!text.isEmpty() && context->mapLayer->isWaypointRouteName())
    symbolPainter->textBox(context->painter, texts, color,
                           x + size / 2 + 2,
                           y, textatt::BOLD | textatt::ROUTE_BG_COLOR, 255);
}

void MapPainterRoute::drawSymbols(const PaintContext *context, const RouteMapObjectList& routeMapObjects,
                                  const QBitArray& visibleStartPoints, const QList<QPoint>& startPoints)
{
  int i = 0;
  for(const QPoint& pt : startPoints)
  {
    if(visibleStartPoints.testBit(i))
    {
      int x = pt.x();
      int y = pt.y();
      const RouteMapObject& obj = routeMapObjects.at(i);
      maptypes::MapObjectTypes type = obj.getMapObjectType();
      switch(type)
      {
        case maptypes::INVALID:
          // name and region not found in database
          paintWaypoint(context, mapcolors::routeInvalidPointColor, x, y);
          break;
        case maptypes::USER:
          paintUserpoint(context, x, y);
          break;
        case maptypes::AIRPORT:
          paintAirport(context, x, y, obj.getAirport());
          break;
        case maptypes::VOR:
          paintVor(context, x, y, obj.getVor());
          break;
        case maptypes::NDB:
          paintNdb(context, x, y);
          break;
        case maptypes::WAYPOINT:
          paintWaypoint(context, QColor(), x, y);
          break;
      }
    }
    i++;
  }
}

void MapPainterRoute::drawSymbolText(const PaintContext *context, const RouteMapObjectList& routeMapObjects,
                                     const QBitArray& visibleStartPoints, const QList<QPoint>& startPoints)
{
  int i = 0;
  for(const QPoint& pt : startPoints)
  {
    if(visibleStartPoints.testBit(i))
    {
      int x = pt.x();
      int y = pt.y();
      const RouteMapObject& obj = routeMapObjects.at(i);
      maptypes::MapObjectTypes type = obj.getMapObjectType();
      switch(type)
      {
        case maptypes::INVALID:
          paintText(context, mapcolors::routeInvalidPointColor, x, y, obj.getIdent());
          break;
        case maptypes::USER:
          paintText(context, mapcolors::routeUserPointColor, x, y, obj.getIdent());
          break;
        case maptypes::AIRPORT:
          paintAirportText(context, x, y, obj.getAirport());
          break;
        case maptypes::VOR:
          paintVorText(context, x, y, obj.getVor());
          break;
        case maptypes::NDB:
          paintNdbText(context, x, y, obj.getNdb());
          break;
        case maptypes::WAYPOINT:
          paintWaypointText(context, x, y, obj.getWaypoint());
          break;
      }
    }
    i++;
  }
}
