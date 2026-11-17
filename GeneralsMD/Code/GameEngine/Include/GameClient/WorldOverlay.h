/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

// FILE: WorldOverlay.h ///////////////////////////////////////////////////////////////////////////
// TheSuperHackers @feature mohamed 17/11/2026 Country and province borders and city names drawn over
// the tactical view of a world map.
//
// A map made by tools/decorate_map.py (stage "overlay") carries a "WorldOverlay" chunk (layout in
// tools/worldgen/mapio.py, WorldOverlay):
//
//   borders   polylines of real country borders (level 1) and state / province borders (level 0),
//             each stored at three levels of detail so the cheapest one that still looks right at
//             the current camera height is drawn
//   cities    about 2,500 labelled places with a zoom tier (0 = the first to appear as the camera
//             comes down into a country, 3 = only when very close) and a priority that decides which of two overlapping
//             labels survives
//
// Everything is drawn in screen space on top of the 3D view, so lines keep the same crisp width at
// every zoom, like a web map. Country borders are a white line with a soft white glow around it;
// province borders are a thinner, dimmer line. Both fade away as the camera comes down to battle
// height (they would only hide the fighting), province borders also fade out again when the whole
// world is in view. The overlay is purely visual: it reads the map file once and never touches the
// game logic, so it cannot affect replays or multiplayer.
///////////////////////////////////////////////////////////////////////////////////////////////////

#pragma once

#include "Common/AsciiString.h"
#include "Common/GameCommon.h"
#include "Common/GameType.h"
#include "Common/UnicodeString.h"

#include <vector>

// detail levels of a border line (an enum: this compiler rejects in-class static const initialisers and
// cannot use a private enumerator inside a nested struct)
enum { OVERLAY_LOD_COUNT = 3 };

class DataChunkInput;
class DisplayString;
class GameFont;
struct DataChunkInfo;

//-------------------------------------------------------------------------------------------------
class WorldOverlay
{
public:

	WorldOverlay();
	~WorldOverlay();

	/// Forget the map and free every label string (the game is leaving the map).
	void reset();

	/// Draw the borders, then the city names, for the map being played. `fontName` is the UI caption font.
	void draw( const AsciiString &fontName );

private:


	struct Line
	{
		UnsignedByte	level;						///< 1 = country border, 0 = province border
		UnsignedByte	flags;						///< bit 0: disputed
		UnsignedInt		first[ OVERLAY_LOD_COUNT ];			///< index of the first point of each detail level in m_points
		UnsignedInt		count[ OVERLAY_LOD_COUNT ];			///< number of points of each detail level
		Real			minX, minY, maxX, maxY;		///< world bounds of the finest level
	};

	struct Label
	{
		UnicodeString	name;
		Coord3D			pos;
		UnsignedByte	tier;
		UnsignedByte	flags;						///< bit 0: country capital, bit 1: megacity
		UnsignedShort	priority;
		DisplayString *	string;						///< made on first use, freed when unused for a while
		Int				lastUsedFrame;
	};

	struct Segment
	{
		Short x1, y1, x2, y2;						///< screen pixels
	};

	struct Rect
	{
		Int x1, y1, x2, y2;
	};

	static Bool parseChunk( DataChunkInput &file, DataChunkInfo *info, void *userData );

	void load( const AsciiString &mapName );
	void clearLabelStrings();
	Bool visibleGroundRect( Real *minX, Real *minY, Real *maxX, Real *maxY ) const;
	void gatherSegments( const Line &line, Int lod, Int viewX1, Int viewY1, Int viewX2, Int viewY2 );
	void drawSegments( const std::vector<Segment> &segments, Real width, Color color );
	void drawBorders( Real height );
	void drawLabels( const AsciiString &fontName, Real height );

	AsciiString					m_mapName;				///< the map the data below belongs to
	Bool						m_active;				///< the map carries a WorldOverlay chunk
	Int							m_border;
	Real						m_mapMinX, m_mapMinY, m_mapMaxX, m_mapMaxY;	///< playable area, world units
	std::vector<Line>			m_lines;
	std::vector<Coord3D>		m_points;				///< all points of all lines and detail levels
	std::vector<Label>			m_labels;				///< best first

	// scratch, kept between frames so nothing is allocated while drawing
	std::vector<Segment>		m_countrySegments;
	std::vector<Segment>		m_provinceSegments;
	std::vector<Rect>			m_labelRects;
	Int							m_quadsSinceFlush;
	Int							m_frame;
	GameFont *					m_font;
	AsciiString					m_fontName;
};
