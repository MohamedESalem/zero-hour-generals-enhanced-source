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

// FILE: WorldOverlay.cpp /////////////////////////////////////////////////////////////////////////
// TheSuperHackers @feature mohamed 17/11/2026 Country and province borders and city names over the
// tactical view of a world map. See WorldOverlay.h.
///////////////////////////////////////////////////////////////////////////////////////////////////

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "Common/DataChunk.h"
#include "Common/GlobalData.h"
#include "Common/MapObject.h"
#include "Common/MapReaderWriterInfo.h"

#include "GameClient/Color.h"
#include "GameClient/Display.h"
#include "GameClient/DisplayString.h"
#include "GameClient/DisplayStringManager.h"
#include "GameClient/GameFont.h"
#include "GameClient/GlobalLanguage.h"
#include "GameClient/View.h"
#include "GameClient/WorldOverlay.h"

#include "GameLogic/GameLogic.h"
#include "GameLogic/TerrainLogic.h"

//-------------------------------------------------------------------------------------------------
static const Int OVERLAY_CHUNK_VERSION = 2;			///< version 1 is still read: it has no step field and always used 5 world units
static const Real OVERLAY_QUANT_V1 = 5.0f;			///< world units per stored coordinate step of a version 1 chunk

// TheSuperHackers @tweak mohamed 18/11/2026 The three border detail levels simplify to 0.04 / 0.25 / 0.9 cells (about 0.5 / 3.5 / 12 km),
// so each is still finer than a pixel at the camera heights it serves.
static const Real LOD0_MAX_HEIGHT = 600.0f;			///< finest border detail below this camera height ...
static const Real LOD1_MAX_HEIGHT = 4500.0f;		///< ... medium up to this one, coarsest above
// TheSuperHackers @tweak mohamed 18/11/2026 One rule for all the detail: the city names and the province borders
// are shown together, from the ground up to the camera height of "WorldOverlayFadeEndHeight" (a country filling the
// screen), and go away together above it. The province borders fade out between FadeStart and FadeEnd; the labels
// are cut at FadeEnd (the first tier) and the smaller places appear as the camera comes lower.
// Country borders stay at every height, and nothing fades near the ground any more: the lines stay visible in battle.
static const Real TIER_SCALE[ 4 ] = { 1.0f, 0.72f, 0.48f, 0.32f };	///< label tier t is shown below this share of FadeEnd

static const Int MAX_COUNTRY_SEGMENTS = 20000;		///< per frame; 2D draws are flushed in blocks, so this is a time budget
static const Int MAX_PROVINCE_SEGMENTS = 60000;		///< (real borders are detailed: a screen of Europe is tens of thousands of segments)
static const Int MAX_QUADS_PER_FLUSH = 7000;		///< Render2D indices are 16 bit: stay well below 65,535 / 6
static const Int MAX_LABELS_PER_FRAME = 160;
static const Int LABEL_STRING_LIFETIME = 240;		///< frames a label may go unused before its string is freed

//-------------------------------------------------------------------------------------------------
static Real smoothStep( Real a, Real b, Real x )
{
	if( b <= a )
		return x >= b ? 1.0f : 0.0f;
	Real t = ( x - a ) / ( b - a );
	t = t < 0.0f ? 0.0f : ( t > 1.0f ? 1.0f : t );
	return t * t * ( 3.0f - 2.0f * t );
}

//-------------------------------------------------------------------------------------------------
WorldOverlay::WorldOverlay()
{
	m_active = FALSE;
	m_border = 0;
	m_mapMinX = m_mapMinY = m_mapMaxX = m_mapMaxY = 0.0f;
	m_quadsSinceFlush = 0;
	m_frame = 0;
	m_quant = OVERLAY_QUANT_V1;
	m_font = nullptr;
}

//-------------------------------------------------------------------------------------------------
WorldOverlay::~WorldOverlay()
{
	reset();
}

//-------------------------------------------------------------------------------------------------
void WorldOverlay::clearLabelStrings()
{
	for( std::vector<Label>::iterator it = m_labels.begin(); it != m_labels.end(); ++it )
	{
		if( it->string )
			TheDisplayStringManager->freeDisplayString( it->string );
		it->string = nullptr;
	}
}

//-------------------------------------------------------------------------------------------------
void WorldOverlay::reset()
{
	clearLabelStrings();
	m_labels.clear();
	m_lines.clear();
	m_points.clear();
	m_countrySegments.clear();
	m_provinceSegments.clear();
	m_labelRects.clear();
	m_mapName.clear();
	m_active = FALSE;
	m_font = nullptr;
	m_fontName.clear();
}

//-------------------------------------------------------------------------------------------------
/** Chunk layout (tools/worldgen/mapio.py, WorldOverlay): width, height, border, detail level
	* count, the tolerance of each level, line count, then per line its level and flags, the point
	* count of each detail level and the points (two 16 bit values each, in half cells, offset by
	* the border); finally the label count and per label the name, two 16 bit coordinates, tier,
	* flags and priority. */
//-------------------------------------------------------------------------------------------------
Bool WorldOverlay::parseChunk( DataChunkInput &file, DataChunkInfo *info, void *userData )
{
	WorldOverlay *self = (WorldOverlay *)userData;

	if( info->version != OVERLAY_CHUNK_VERSION && info->version != 1 )
	{
		DEBUG_LOG(( "WorldOverlay: chunk version %d is not supported, the overlay is off", info->version ));
		return TRUE;
	}

	const Int width = file.readInt();
	const Int height = file.readInt();
	const Int border = file.readInt();
	const Int lodCount = file.readInt();
	if( lodCount != OVERLAY_LOD_COUNT || width <= 0 || height <= 0 )
		return TRUE;

	for( Int iTol = 0; iTol < lodCount; ++iTol )
		file.readReal();								// tolerances: informational

	// TheSuperHackers @tweak mohamed 18/11/2026 Version 2 stores the world units per coordinate step (0.5 for the
	// world map, so borders keep the data's own detail); version 1 always used 5.
	const Real quant = ( info->version >= 2 ) ? file.readReal() : OVERLAY_QUANT_V1;
	if( quant <= 0.0f )
		return TRUE;
	self->m_quant = quant;

	const Real shift = 10.0f * (Real)border / quant;		// stored steps in the border ring
	self->m_border = border;
	self->m_mapMinX = 0.0f;
	self->m_mapMinY = 0.0f;
	self->m_mapMaxX = (Real)( width - 2 * border ) * MAP_XY_FACTOR;
	self->m_mapMaxY = (Real)( height - 2 * border ) * MAP_XY_FACTOR;

	const Int lineCount = file.readInt();
	if( lineCount < 0 || lineCount > 500000 )
		return TRUE;

	self->m_lines.reserve( lineCount );
	std::vector<UnsignedShort> raw;
	for( Int iLine = 0; iLine < lineCount; ++iLine )
	{
		Line line;
		line.level = (UnsignedByte)file.readByte();
		line.flags = (UnsignedByte)file.readByte();
		Int counts[ OVERLAY_LOD_COUNT ];
		for( Int lodRead = 0; lodRead < OVERLAY_LOD_COUNT; ++lodRead )
			counts[ lodRead ] = file.readInt();

		line.minX = line.minY = 1.0e30f;
		line.maxX = line.maxY = -1.0e30f;
		for( Int lod = 0; lod < OVERLAY_LOD_COUNT; ++lod )
		{
			line.first[ lod ] = (UnsignedInt)self->m_points.size();
			line.count[ lod ] = 0;
			if( counts[ lod ] <= 0 )
				continue;

			raw.resize( (size_t)counts[ lod ] * 2 );
			file.readArrayOfBytes( (char *)&raw[ 0 ], counts[ lod ] * 4 );		// little endian, like the host
			line.count[ lod ] = (UnsignedInt)counts[ lod ];
			for( Int p = 0; p < counts[ lod ]; ++p )
			{
				Coord3D point;
				point.x = ( (Real)raw[ p * 2 ] - shift ) * quant;
				point.y = ( (Real)raw[ p * 2 + 1 ] - shift ) * quant;
				point.z = 0.0f;
				self->m_points.push_back( point );
				if( lod == 0 )
				{
					if( point.x < line.minX ) line.minX = point.x;
					if( point.x > line.maxX ) line.maxX = point.x;
					if( point.y < line.minY ) line.minY = point.y;
					if( point.y > line.maxY ) line.maxY = point.y;
				}
			}
		}

		if( line.count[ 0 ] >= 2 )
			self->m_lines.push_back( line );
	}

	const Int labelCount = file.readInt();
	if( labelCount < 0 || labelCount > 100000 )
		return TRUE;

	self->m_labels.reserve( labelCount );
	for( Int iLabel = 0; iLabel < labelCount; ++iLabel )
	{
		Label label;
		label.name = file.readUnicodeString();
		unsigned char record[ 8 ];
		file.readArrayOfBytes( (char *)record, 8 );
		const Int qx = record[ 0 ] | ( record[ 1 ] << 8 );
		const Int qy = record[ 2 ] | ( record[ 3 ] << 8 );
		label.pos.x = ( (Real)qx - shift ) * quant;
		label.pos.y = ( (Real)qy - shift ) * quant;
		label.pos.z = 0.0f;
		label.tier = record[ 4 ] > 3 ? 3 : record[ 4 ];
		label.flags = record[ 5 ];
		label.priority = (UnsignedShort)( record[ 6 ] | ( record[ 7 ] << 8 ) );
		label.string = nullptr;
		label.lastUsedFrame = 0;
		self->m_labels.push_back( label );
	}

	self->m_active = !self->m_lines.empty() || !self->m_labels.empty();
	return TRUE;
}

//-------------------------------------------------------------------------------------------------
void WorldOverlay::load( const AsciiString &mapName )
{
	reset();
	m_mapName = mapName;

	CachedFileInputStream stream;
	if( !stream.open( mapName ) )
		return;

	ChunkInputStream *pStream = &stream;
	DataChunkInput file( pStream );
	file.registerParser( "WorldOverlay", AsciiString::TheEmptyString, parseChunk );
	if( !file.parse( this ) || !m_active )
	{
		const AsciiString keep = m_mapName;
		reset();
		m_mapName = keep;
		return;
	}

	// lay the points on the ground, so lines and names follow the hills instead of hovering at sea level
	if( TheTerrainLogic )
	{
		for( std::vector<Coord3D>::iterator pit = m_points.begin(); pit != m_points.end(); ++pit )
			pit->z = TheTerrainLogic->getGroundHeight( pit->x, pit->y );
		for( std::vector<Label>::iterator lit = m_labels.begin(); lit != m_labels.end(); ++lit )
			lit->pos.z = TheTerrainLogic->getGroundHeight( lit->pos.x, lit->pos.y );
	}

	DEBUG_LOG(( "WorldOverlay: %d border lines, %d points, %d city labels", (Int)m_lines.size(), (Int)m_points.size(), (Int)m_labels.size() ));
}

//-------------------------------------------------------------------------------------------------
/** The ground area the camera sees, in world units (with a margin), or FALSE when a corner of the
	* screen looks past the horizon. */
//-------------------------------------------------------------------------------------------------
Bool WorldOverlay::visibleGroundRect( Real *minX, Real *minY, Real *maxX, Real *maxY ) const
{
	Int originX, originY;
	TheTacticalView->getOrigin( &originX, &originY );
	const Int width = TheTacticalView->getWidth();
	const Int height = TheTacticalView->getHeight();

	ICoord2D corners[ 4 ];
	corners[ 0 ].x = originX;					corners[ 0 ].y = originY;
	corners[ 1 ].x = originX + width;			corners[ 1 ].y = originY;
	corners[ 2 ].x = originX;					corners[ 2 ].y = originY + height;
	corners[ 3 ].x = originX + width;			corners[ 3 ].y = originY + height;

	Real x1 = 1.0e30f, y1 = 1.0e30f, x2 = -1.0e30f, y2 = -1.0e30f;
	for( Int i = 0; i < 4; ++i )
	{
		Coord3D ground;
		if( TheTacticalView->screenToWorldAtZ( &corners[ i ], &ground, 0.0f ) == PlaneClass::NO_INTERSECTION )
			return FALSE;
		if( ground.x < x1 ) x1 = ground.x;
		if( ground.x > x2 ) x2 = ground.x;
		if( ground.y < y1 ) y1 = ground.y;
		if( ground.y > y2 ) y2 = ground.y;
	}

	const Real margin = 0.15f * ( ( x2 - x1 ) > ( y2 - y1 ) ? ( x2 - x1 ) : ( y2 - y1 ) ) + 200.0f;
	*minX = x1 - margin;
	*minY = y1 - margin;
	*maxX = x2 + margin;
	*maxY = y2 + margin;
	return TRUE;
}

//-------------------------------------------------------------------------------------------------
/** Clip a screen segment to a rectangle (Liang-Barsky). FALSE when nothing of it is inside. */
//-------------------------------------------------------------------------------------------------
static Bool clipSegment( Real &x1, Real &y1, Real &x2, Real &y2, Real left, Real top, Real right, Real bottom )
{
	const Real dx = x2 - x1;
	const Real dy = y2 - y1;
	const Real p[ 4 ] = { -dx, dx, -dy, dy };
	const Real q[ 4 ] = { x1 - left, right - x1, y1 - top, bottom - y1 };
	Real t0 = 0.0f, t1 = 1.0f;
	for( Int i = 0; i < 4; ++i )
	{
		if( p[ i ] == 0.0f )
		{
			if( q[ i ] < 0.0f )
				return FALSE;
			continue;
		}
		const Real t = q[ i ] / p[ i ];
		if( p[ i ] < 0.0f )
		{
			if( t > t1 ) return FALSE;
			if( t > t0 ) t0 = t;
		}
		else
		{
			if( t < t0 ) return FALSE;
			if( t < t1 ) t1 = t;
		}
	}
	const Real nx1 = x1 + t0 * dx, ny1 = y1 + t0 * dy;
	const Real nx2 = x1 + t1 * dx, ny2 = y1 + t1 * dy;
	x1 = nx1; y1 = ny1; x2 = nx2; y2 = ny2;
	return TRUE;
}

//-------------------------------------------------------------------------------------------------
void WorldOverlay::gatherSegments( const Line &line, Int lod, Int viewX1, Int viewY1, Int viewX2, Int viewY2,
								   Real minX, Real minY, Real maxX, Real maxY )
{
	std::vector<Segment> &out = line.level ? m_countrySegments : m_provinceSegments;
	const Int limit = line.level ? MAX_COUNTRY_SEGMENTS : MAX_PROVINCE_SEGMENTS;

	// the coarser levels drop short pieces entirely: fall back to the next finer one
	while( lod > 0 && line.count[ lod ] < 2 )
		--lod;

	const Coord3D *points = &m_points[ line.first[ lod ] ];
	const Int count = (Int)line.count[ lod ];

	// A border chain can be long while only a few of its points are on screen: a point is projected only when it
	// or one of its neighbours lies in the visible ground rectangle (a cheap compare), so the cost follows the screen.
	ICoord2D previous = { 0, 0 };
	Bool havePrevious = FALSE;
	Bool prevIn = FALSE;
	Bool curIn = points[ 0 ].x >= minX && points[ 0 ].x <= maxX && points[ 0 ].y >= minY && points[ 0 ].y <= maxY;
	for( Int i = 0; i < count; ++i )
	{
		Bool nextIn = FALSE;
		if( i + 1 < count )
			nextIn = points[ i + 1 ].x >= minX && points[ i + 1 ].x <= maxX && points[ i + 1 ].y >= minY && points[ i + 1 ].y <= maxY;
		const Bool nearView = prevIn || curIn || nextIn;
		prevIn = curIn;
		curIn = nextIn;
		if( !nearView )
		{
			havePrevious = FALSE;
			continue;
		}

		ICoord2D screen;
		if( TheTacticalView->worldToScreenTriReturn( &points[ i ], &screen ) == View::WTS_INVALID )
		{
			havePrevious = FALSE;
			continue;
		}

		if( havePrevious )
		{
			Real x1 = (Real)previous.x, y1 = (Real)previous.y, x2 = (Real)screen.x, y2 = (Real)screen.y;
			if( clipSegment( x1, y1, x2, y2, (Real)viewX1, (Real)viewY1, (Real)viewX2, (Real)viewY2 ) &&
				( x1 != x2 || y1 != y2 ) )
			{
				if( (Int)out.size() >= limit )
					return;
				Segment segment;
				segment.x1 = (Short)x1;
				segment.y1 = (Short)y1;
				segment.x2 = (Short)x2;
				segment.y2 = (Short)y2;
				out.push_back( segment );
			}
		}

		previous = screen;
		havePrevious = TRUE;
	}
}

//-------------------------------------------------------------------------------------------------
void WorldOverlay::drawSegments( const std::vector<Segment> &segments, Real width, Color color )
{
	for( std::vector<Segment>::const_iterator it = segments.begin(); it != segments.end(); ++it )
	{
		TheDisplay->drawLine( it->x1, it->y1, it->x2, it->y2, width, color );
		if( ++m_quadsSinceFlush >= MAX_QUADS_PER_FLUSH )
		{
			TheDisplay->flush();
			m_quadsSinceFlush = 0;
		}
	}
}

//-------------------------------------------------------------------------------------------------
void WorldOverlay::drawBorders( Real height )
{
	const Real countryFade = 1.0f;
	const Real provinceFade = 1.0f - smoothStep( TheGlobalData->m_worldOverlayFadeStartHeight, TheGlobalData->m_worldOverlayFadeEndHeight, height );

	Real minX, minY, maxX, maxY;
	if( !visibleGroundRect( &minX, &minY, &maxX, &maxY ) )
	{
		minX = m_mapMinX; minY = m_mapMinY; maxX = m_mapMaxX; maxY = m_mapMaxY;
	}

	Int originX, originY;
	TheTacticalView->getOrigin( &originX, &originY );
	const Int viewW = TheTacticalView->getWidth();
	const Int viewH = TheTacticalView->getHeight();

	const Int lod = height < LOD0_MAX_HEIGHT ? 0 : ( height < LOD1_MAX_HEIGHT ? 1 : 2 );
	const Bool wantProvinces = provinceFade > 0.02f;

	m_countrySegments.clear();
	m_provinceSegments.clear();
	for( std::vector<Line>::const_iterator it = m_lines.begin(); it != m_lines.end(); ++it )
	{
		if( !it->level && !wantProvinces )
			continue;
		if( it->maxX < minX || it->minX > maxX || it->maxY < minY || it->minY > maxY )
			continue;
		gatherSegments( *it, lod, originX - 16, originY - 16, originX + viewW + 16, originY + viewH + 16, minX, minY, maxX, maxY );
	}

	// thicker on a big screen, so the line looks the same at 4K as at 1080p
	const Real k = viewH > 1080 ? (Real)viewH / 1080.0f : 1.0f;

	if( !m_provinceSegments.empty() )
		drawSegments( m_provinceSegments, 1.2f * k, GameMakeColor( 220, 228, 242, (UnsignedByte)( 105.0f * provinceFade ) ) );

	if( !m_countrySegments.empty() )
	{
		// glow: two soft wide strokes under a thin bright core
		drawSegments( m_countrySegments, 8.0f * k, GameMakeColor( 255, 255, 255, (UnsignedByte)( 13.0f * countryFade ) ) );
		drawSegments( m_countrySegments, 4.0f * k, GameMakeColor( 255, 255, 255, (UnsignedByte)( 32.0f * countryFade ) ) );
		drawSegments( m_countrySegments, 1.6f * k, GameMakeColor( 255, 255, 255, (UnsignedByte)( 175.0f * countryFade ) ) );
	}

	TheDisplay->flush();
	m_quadsSinceFlush = 0;
}

//-------------------------------------------------------------------------------------------------
void WorldOverlay::drawLabels( const AsciiString &fontName, Real height )
{
	if( m_labels.empty() )
		return;

	if( m_font == nullptr || m_fontName != fontName )
	{
		clearLabelStrings();
		m_font = TheFontLibrary->getFont( fontName, TheGlobalLanguageData->adjustFontSize( 8 ), FALSE );
		m_fontName = fontName;
	}

	Int originX, originY;
	TheTacticalView->getOrigin( &originX, &originY );
	const Int left = originX - 24;
	const Int top = originY - 12;
	const Int right = originX + TheTacticalView->getWidth() + 24;
	const Int bottom = originY + TheTacticalView->getHeight() + 12;

	const Color white = GameMakeColor( 238, 240, 244, 225 );		// light and slightly see-through: quieter than the country names
	const Color black = GameMakeColor( 0, 0, 0, 110 );
	const Color gold = GameMakeColor( 255, 214, 96, 215 );

	++m_frame;
	m_labelRects.clear();
	Int shown = 0;
	const Real maxLabelHeight = TheGlobalData->m_worldOverlayFadeEndHeight;

	// `m_labels` is sorted best first, so a label that would overlap one already placed is the lesser one
	for( std::vector<Label>::iterator it = m_labels.begin(); it != m_labels.end() && shown < MAX_LABELS_PER_FRAME; ++it )
	{
		if( height > maxLabelHeight * TIER_SCALE[ it->tier ] )
			continue;

		ICoord2D screen;
		if( TheTacticalView->worldToScreenTriReturn( &it->pos, &screen ) == View::WTS_INVALID )
			continue;
		if( screen.x < left || screen.x > right || screen.y < top || screen.y > bottom )
			continue;

		if( it->string == nullptr )
		{
			it->string = TheDisplayStringManager->newDisplayString();
			it->string->reset();
			it->string->setFont( m_font );
			it->string->setText( it->name );
		}
		it->lastUsedFrame = m_frame;

		Int textW, textH;
		it->string->getSize( &textW, &textH );

		Rect rect;
		rect.x1 = screen.x - 5;
		rect.y1 = screen.y - textH / 2 - 1;
		rect.x2 = screen.x + 9 + textW;
		rect.y2 = screen.y + textH / 2 + 1;

		Bool overlaps = FALSE;
		for( std::vector<Rect>::const_iterator r = m_labelRects.begin(); r != m_labelRects.end(); ++r )
		{
			if( rect.x1 < r->x2 && rect.x2 > r->x1 && rect.y1 < r->y2 && rect.y2 > r->y1 )
			{
				overlaps = TRUE;
				break;
			}
		}
		if( overlaps )
			continue;

		m_labelRects.push_back( rect );
		++shown;

		if( it->flags & 1 )			// a country's capital: a larger marker with a ring
		{
			TheDisplay->drawFillRect( screen.x - 1, screen.y - 1, 3, 3, gold );
			TheDisplay->drawOpenRect( screen.x - 4, screen.y - 4, 9, 9, 1.0f, white );
		}
		else
		{
			TheDisplay->drawFillRect( screen.x - 1, screen.y - 1, 3, 3, gold );
		}
		it->string->draw( screen.x + 7, screen.y - textH / 2, white, black );
	}

	// give back the strings of labels that have been off screen for a while
	if( ( m_frame % 120 ) == 0 )
	{
		for( std::vector<Label>::iterator sweep = m_labels.begin(); sweep != m_labels.end(); ++sweep )
		{
			if( sweep->string && m_frame - sweep->lastUsedFrame > LABEL_STRING_LIFETIME )
			{
				TheDisplayStringManager->freeDisplayString( sweep->string );
				sweep->string = nullptr;
			}
		}
	}
}

//-------------------------------------------------------------------------------------------------
void WorldOverlay::draw( const AsciiString &fontName )
{
	if( TheGlobalData->m_worldOverlayDisabled || TheTacticalView == nullptr || TheDisplay == nullptr ||
		TheGameLogic == nullptr || !TheGameLogic->getDrawIconUI() )
		return;

	// the map's file is read once per map; a map without the chunk costs one failed look
	if( m_mapName != TheGlobalData->m_mapName )
		load( TheGlobalData->m_mapName );
	if( !m_active )
		return;

	const Real height = TheTacticalView->getHeightAboveGround();
	drawBorders( height );
	drawLabels( fontName, height );
}
