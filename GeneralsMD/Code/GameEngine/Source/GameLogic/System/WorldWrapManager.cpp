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

// FILE: WorldWrapManager.cpp /////////////////////////////////////////////////////////////////////
// TheSuperHackers @feature mohamed 18/11/2026 Crossing the 180th meridian. See WorldWrapManager.h.
///////////////////////////////////////////////////////////////////////////////////////////////////

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Common/DataChunk.h"
#include "Common/GlobalData.h"
#include "Common/MapObject.h"
#include "Common/MapReaderWriterInfo.h"
#include "Common/Player.h"
#include "Common/PlayerList.h"
#include "Common/Team.h"
#include "Common/ThingFactory.h"
#include "Common/ThingTemplate.h"

#include "GameClient/Drawable.h"
#include "GameClient/InGameUI.h"
#include "GameClient/View.h"

#include "GameLogic/AI.h"
#include "GameLogic/AIStateMachine.h"
#include "GameLogic/GameLogic.h"
#include "GameLogic/Locomotor.h"
#include "GameLogic/LocomotorSet.h"
#include "GameLogic/Module/AIUpdate.h"
#include "GameLogic/Object.h"
#include "GameLogic/TerrainLogic.h"
#include "GameLogic/WorldWrapManager.h"

//-------------------------------------------------------------------------------------------------
WorldWrapManager *TheWorldWrapManager = nullptr;

enum
{
	WORLD_WRAP_CHUNK_VERSION = 1,
	WORLD_WRAP_CHECK_INTERVAL = 3,					///< logic frames between two looks at the ships and aircraft
	WORLD_WRAP_MIN_SAVING = 300,						///< world units a trip through the meridian must save to be taken
	WORLD_WRAP_ROUTE_TOLERANCE = 25					///< how far a goal may be from the planned jump point and still be ours
};

static const Real WORLD_WRAP_SAVING_RATIO = 0.8f;	///< ... and the share of the direct distance the detour must stay below
static const Real WORLD_WRAP_JUMP_OVERSHOOT = 12.0f;	///< world units beyond the trigger line the jump point lies
static const Real WORLD_WRAP_CAMERA_REACH = 1500.0f;	///< the camera follows a selected unit that jumps if it was this close

//-------------------------------------------------------------------------------------------------
/** With GENZH_WORLD_LOG set, one line per jump, redirect and self-test sample goes to WorldLog.txt
	* in the game folder (it stops after 4000 lines). Reads nothing of the simulation, changes nothing. */
//-------------------------------------------------------------------------------------------------
static void worldLog( const char *format, ... )
{
	static Int lines = 0;
	if( getenv( "GENZH_WORLD_LOG" ) == nullptr || lines >= 4000 )
		return;

	FILE *file = fopen( "WorldLog.txt", "a" );
	if( file == nullptr )
		return;

	++lines;
	va_list args;
	va_start( args, format );
	vfprintf( file, format, args );
	va_end( args );
	fputc( '\n', file );
	fclose( file );
}

//-------------------------------------------------------------------------------------------------
static Real distance2D( const Coord3D &a, const Coord3D &b )
{
	const Real dx = a.x - b.x;
	const Real dy = a.y - b.y;
	return (Real)sqrt( dx * dx + dy * dy );
}

//-------------------------------------------------------------------------------------------------
WorldWrapManager::WorldWrapManager()
{
	reset();
}

//-------------------------------------------------------------------------------------------------
WorldWrapManager::~WorldWrapManager()
{
}

//-------------------------------------------------------------------------------------------------
void WorldWrapManager::reset()
{
	m_active = FALSE;
	m_width = 0;
	m_height = 0;
	m_border = 0;
	m_inset = 0.0f;
	m_west.clear();
	m_east.clear();
	m_routes.clear();
}

//-------------------------------------------------------------------------------------------------
/** Chunk layout (tools/worldgen/mapio.py, WorldWrap): width, height, border, inset (world units), row
	* count, then the west x of the meridian for every row and the east x for every row (world units,
	* rows in height-map order, south first). A row that does not wrap has both values 0. */
//-------------------------------------------------------------------------------------------------
Bool WorldWrapManager::parseWorldWrapChunk( DataChunkInput &file, DataChunkInfo *info, void *userData )
{
	WorldWrapManager *self = (WorldWrapManager *)userData;

	if( info->version != WORLD_WRAP_CHUNK_VERSION )
	{
		DEBUG_LOG(( "WorldWrapManager: WorldWrap version %d is not supported, wrapping is off", info->version ));
		return TRUE;
	}

	self->m_width = file.readInt();
	self->m_height = file.readInt();
	self->m_border = file.readInt();
	self->m_inset = file.readReal();

	const Int rows = file.readInt();
	if( self->m_height <= 0 || rows != self->m_height )
	{
		DEBUG_LOG(( "WorldWrapManager: bad row table (%d rows for a height of %d), wrapping is off", rows, self->m_height ));
		return TRUE;
	}

	self->m_west.resize( rows );
	self->m_east.resize( rows );
	file.readArrayOfBytes( (char *)&self->m_west[ 0 ], rows * (Int)sizeof( Real ) );
	file.readArrayOfBytes( (char *)&self->m_east[ 0 ], rows * (Int)sizeof( Real ) );
	self->m_active = TRUE;
	return TRUE;
}

//-------------------------------------------------------------------------------------------------
void WorldWrapManager::newMap( Bool loadingSaveGame )
{
	reset();

	if( getenv( "GENZH_NO_WRAP" ) != nullptr )		// switch the feature off without touching the map
		return;

	CachedFileInputStream theInputStream;
	if( !theInputStream.open( TheGlobalData->m_mapName ) )
		return;

	ChunkInputStream *pStrm = &theInputStream;
	DataChunkInput file( pStrm );
	file.registerParser( "WorldWrap", AsciiString::TheEmptyString, parseWorldWrapChunk );
	if( !file.parse( this ) || !m_active )
	{
		reset();
		return;
	}

	DEBUG_LOG(( "WorldWrapManager: the map wraps at the 180th meridian, %d rows", m_height ));
}

//-------------------------------------------------------------------------------------------------
Bool WorldWrapManager::getEdges( Real y, Real *west, Real *east ) const
{
	if( !m_active )
		return FALSE;

	const Int row = REAL_TO_INT_FLOOR( y / MAP_XY_FACTOR + 0.5f ) + m_border;
	if( row < 0 || row >= m_height )
		return FALSE;

	const Real w = m_west[ row ] + m_inset;
	const Real e = m_east[ row ] - m_inset;
	if( e - w < 400.0f )
		return FALSE;

	*west = w;
	*east = e;
	return TRUE;
}

//-------------------------------------------------------------------------------------------------
Int WorldWrapManager::findRoute( ObjectID id ) const
{
	for( Int i = 0; i < (Int)m_routes.size(); ++i )
	{
		if( m_routes[ i ].id == id )
			return i;
	}

	return -1;
}

//-------------------------------------------------------------------------------------------------
void WorldWrapManager::removeRoute( Int index )
{
	if( index >= 0 && index < (Int)m_routes.size() )
		m_routes.erase( m_routes.begin() + index );
}

//-------------------------------------------------------------------------------------------------
/** Put an object, and what it carries, at the same latitude on the other side of the map. */
//-------------------------------------------------------------------------------------------------
void WorldWrapManager::jump( Object *obj, Real newX )
{
	const Coord3D oldPos = *obj->getPosition();
	worldLog( "frame %u: %s (%u) crosses the meridian at y %.0f: x %.0f -> %.0f", TheGameLogic->getFrame(), obj->getTemplate()->getName().str(), (UnsignedInt)obj->getID(), oldPos.y, oldPos.x, newX );
	Coord3D pos = oldPos;
	pos.x = newX;
	obj->setPosition( &pos );

	// The player who was looking at the unit follows it.
	Drawable *drawable = obj->getDrawable();
	if( drawable != nullptr && drawable->isSelected() && TheTacticalView != nullptr )
	{
		const Coord3D &camera = TheTacticalView->getPosition();
		const Real dx = camera.x - oldPos.x;
		const Real dy = camera.y - oldPos.y;
		if( dx * dx + dy * dy < WORLD_WRAP_CAMERA_REACH * WORLD_WRAP_CAMERA_REACH )
			TheTacticalView->lookAt( &pos );
	}
}

//-------------------------------------------------------------------------------------------------
/** Is a world position inside the playable area of the map? */
//-------------------------------------------------------------------------------------------------
static Bool isOnTheMap( const Coord3D &pos, Int width, Int height, Int border )
{
	const Real maxX = ( width - 2 * border ) * MAP_XY_FACTOR;
	const Real maxY = ( height - 2 * border ) * MAP_XY_FACTOR;
	return pos.x >= 0.0f && pos.y >= 0.0f && pos.x <= maxX && pos.y <= maxY;
}

//-------------------------------------------------------------------------------------------------
/** Only a unit that was sent through the meridian jumps. A player's order to a place that is nearer through
	* the meridian than across the map makes a Route; that unit sails or flies to the jump point, crosses, and goes
	* on to where it was sent. Nothing else is moved: aircraft that come in from or leave over the edge of the map
	* (reinforcement and supply planes, which are created and removed beyond it) must never be touched. */
//-------------------------------------------------------------------------------------------------
void WorldWrapManager::checkObject( Object *obj )
{
	if( obj->isEffectivelyDead() || obj->isContained() || obj->isKindOf( KINDOF_STRUCTURE ) || obj->isKindOf( KINDOF_PROJECTILE ) )
		return;

	AIUpdateInterface *ai = obj->getAIUpdateInterface();
	if( ai == nullptr )
		return;

	const Locomotor *locomotor = ai->getCurLocomotor();
	if( locomotor == nullptr )
		return;

	// Only what is built for water or air: a ship, an aircraft. Amphibious vehicles stay on their side.
	const LocomotorSurfaceTypeMask surfaces = locomotor->getLegalSurfaces();
	const Bool flies = ( surfaces & LOCOMOTORSURFACE_AIR ) != 0;
	const Bool sails = ( surfaces & LOCOMOTORSURFACE_WATER ) != 0 && ( surfaces & LOCOMOTORSURFACE_GROUND ) == 0;
	if( !flies && !sails )
		return;

	const ObjectID id = obj->getID();
	const Int routeIndex = findRoute( id );
	const Coord3D *pos = obj->getPosition();

	// ---- a unit on its way through the meridian ----------------------------------------------
	if( routeIndex >= 0 )
	{
		const Route route = m_routes[ routeIndex ];		// a copy: the list changes below
		const Bool newOrder = ai->getLastCommandSource() == CMD_FROM_PLAYER;
		if( newOrder || ai->getCurrentStateID() != AI_MOVE_TO )
		{
			// the player sent it somewhere else, or it stopped: it is no longer ours
			removeRoute( routeIndex );
		}
		else
		{
			Real west, east;
			if( !getEdges( pos->y, &west, &east ) )
			{
				removeRoute( routeIndex );
				return;
			}

			const Bool across = route.viaWest ? ( pos->x < west ) : ( pos->x > east );
			if( !across )
				return;

			Real newX = route.viaWest ? ( east - ( west - pos->x ) - 1.0f ) : ( west + ( pos->x - east ) + 1.0f );

			// A ship needs water where it lands: at a few latitudes (the Bering Strait, Antarctica) the other
			// side of the meridian is land. It stays where it is and gives up the trip.
			if( !flies && !TheTerrainLogic->isUnderwater( newX, pos->y ) )
			{
				Coord3D back = *pos;
				back.x = route.viaWest ? west + 2.0f : east - 2.0f;
				obj->setPosition( &back );
				removeRoute( routeIndex );
				return;
			}

			Coord3D target = route.finalTarget;
			removeRoute( routeIndex );
			jump( obj, newX );
			ai->aiMoveToPosition( &target, CMD_FROM_SCRIPT );		// a fresh path from where it stands now
			return;
		}
	}

	// ---- a player's order: is the way through the meridian shorter? ----------------------------
	if( ai->getLastCommandSource() != CMD_FROM_PLAYER || ai->getCurrentStateID() != AI_MOVE_TO || ai->getGoalPosition() == nullptr )
		return;

	const Coord3D goal = *ai->getGoalPosition();
	if( !isOnTheMap( *pos, m_width, m_height, m_border ) || !isOnTheMap( goal, m_width, m_height, m_border ) )
		return;

	Real west, east;
	if( !getEdges( pos->y, &west, &east ) )
		return;

	const Real direct = distance2D( *pos, goal );
	if( direct < (Real)( 2 * WORLD_WRAP_MIN_SAVING ) )
		return;

	Coord3D crossWest = *pos;		crossWest.x = west - WORLD_WRAP_JUMP_OVERSHOOT;
	Coord3D crossEast = *pos;		crossEast.x = east + WORLD_WRAP_JUMP_OVERSHOOT;
	Coord3D landEast = *pos;		landEast.x = east - WORLD_WRAP_JUMP_OVERSHOOT - 1.0f;		// where a unit that left west appears
	Coord3D landWest = *pos;		landWest.x = west + WORLD_WRAP_JUMP_OVERSHOOT + 1.0f;

	// The unit lands on its current row and sails on from there, so only the current row matters.
	const Real viaWest = distance2D( *pos, crossWest ) + distance2D( landEast, goal );
	const Real viaEast = distance2D( *pos, crossEast ) + distance2D( landWest, goal );
	const Bool westIsBetter = viaWest < viaEast;
	const Real best = westIsBetter ? viaWest : viaEast;
	if( best >= direct * WORLD_WRAP_SAVING_RATIO || direct - best < (Real)WORLD_WRAP_MIN_SAVING )
		return;

	Route route;
	route.id = id;
	route.finalTarget = goal;
	route.jumpPoint = westIsBetter ? crossWest : crossEast;
	route.viaWest = westIsBetter;
	m_routes.push_back( route );
	worldLog( "frame %u: %s (%u) at x %.0f y %.0f is sent via the %s side to x %.0f y %.0f (direct %.0f, via the meridian %.0f)", TheGameLogic->getFrame(), obj->getTemplate()->getName().str(), (UnsignedInt)id, pos->x, pos->y, westIsBetter ? "west" : "east", goal.x, goal.y, direct, best );

	Coord3D jumpPoint = route.jumpPoint;
	ai->aiMoveToPosition( &jumpPoint, CMD_FROM_SCRIPT );
}

//-------------------------------------------------------------------------------------------------
/** GENZH_WRAP_SELFTEST="x1,y1,x2,y2,Template[,local]": a few seconds after the game starts, put one object of that
	* template at (x1, y1) for the neutral player (the local player with a sixth field "local") and order it to (x2, y2). With GENZH_WORLD_LOG the log shows
	* whether it goes through the meridian and arrives. A test aid for this feature; nothing else uses it. */
//-------------------------------------------------------------------------------------------------
static void runSelfTest()
{
	static ObjectID testId = INVALID_ID;
	static Bool started = FALSE;
	static Coord3D target;
	static Bool disabled = FALSE;

	const char *spec = getenv( "GENZH_WRAP_SELFTEST" );
	if( spec == nullptr || disabled )
		return;

	const UnsignedInt frame = TheGameLogic->getFrame();
	if( !started )
	{
		if( frame < 90 )
			return;

		started = TRUE;
		float x1, y1, x2, y2;
		char name[ 128 ];
		char owner[ 16 ];
		owner[ 0 ] = 0;
		if( sscanf( spec, "%f,%f,%f,%f,%127[^,],%15s", &x1, &y1, &x2, &y2, name, owner ) < 5 )
		{
			disabled = TRUE;
			return;
		}

		const ThingTemplate *thing = TheThingFactory->findTemplate( AsciiString( name ) );
		if( thing == nullptr )
		{
			worldLog( "selftest: no template %s", name );
			disabled = TRUE;
			return;
		}

		Player *player = strcmp( owner, "local" ) == 0 ? ThePlayerList->getLocalPlayer() : ThePlayerList->getNeutralPlayer();
		Object *obj = TheThingFactory->newObject( thing, player->getDefaultTeam() );
		Coord3D pos;
		pos.x = x1;
		pos.y = y1;
		pos.z = TheTerrainLogic->getGroundHeight( x1, y1 );
		obj->setPosition( &pos );
		target.x = x2;
		target.y = y2;
		target.z = 0.0f;
		testId = obj->getID();
		AIUpdateInterface *ai = obj->getAIUpdateInterface();
		if( ai != nullptr )
			ai->aiMoveToPosition( &target, CMD_FROM_PLAYER );		// like a player's order
		worldLog( "frame %u: selftest %s (%u) placed at %.0f,%.0f, ordered to %.0f,%.0f%s", frame, name, (UnsignedInt)testId, x1, y1, x2, y2, ai ? "" : " (no AI)" );
		return;
	}

	if( testId == INVALID_ID || frame % 150 != 0 )
		return;

	Object *obj = TheGameLogic->findObjectByID( testId );
	if( obj == nullptr )
	{
		worldLog( "frame %u: selftest object is gone", frame );
		testId = INVALID_ID;
		return;
	}

	const Coord3D *pos = obj->getPosition();
	worldLog( "frame %u: selftest at %.0f,%.0f, %.0f from the target", frame, pos->x, pos->y, distance2D( *pos, target ) );
}

//-------------------------------------------------------------------------------------------------
void WorldWrapManager::update()
{
	if( !m_active || TheGameLogic == nullptr )
		return;

	runSelfTest();

	if( TheGameLogic->getFrame() % WORLD_WRAP_CHECK_INTERVAL != 0 )
		return;

	for( Object *obj = TheGameLogic->getFirstObject(); obj != nullptr; )
	{
		Object *next = obj->getNextObject();
		checkObject( obj );
		obj = next;
	}

	// Forget routes of objects that are gone.
	for( Int i = (Int)m_routes.size() - 1; i >= 0; --i )
	{
		if( TheGameLogic->findObjectByID( m_routes[ i ].id ) == nullptr )
			removeRoute( i );
	}
}
