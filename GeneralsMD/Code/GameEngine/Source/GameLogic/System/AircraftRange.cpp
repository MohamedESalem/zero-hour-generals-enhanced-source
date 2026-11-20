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
// FILE: AircraftRange.cpp ////////////////////////////////////////////////////////////////////////
// TheSuperHackers @feature mohamed 20/11/2026 Realistic combat range for aircraft. See AircraftRange.h.
///////////////////////////////////////////////////////////////////////////////////////////////////

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>

#include "Common/AudioEventRTS.h"
#include "Common/FileSystem.h"
#include "Common/GameState.h"
#include "Common/GameAudio.h"
#include "Common/GlobalData.h"
#include "Common/Player.h"
#include "Common/PlayerList.h"
#include "Common/SpecialPower.h"
#include "Common/ThingFactory.h"
#include "Common/ThingTemplate.h"
#include "Common/Xfer.h"
#include "Common/file.h"

#include "GameClient/InGameUI.h"

#include "GameLogic/AI.h"
#include "GameLogic/AircraftRange.h"
#include "GameLogic/GameLogic.h"
#include "GameLogic/Module/AIUpdate.h"
#include "GameLogic/Module/BehaviorModule.h"
#include "GameLogic/Module/JetAIUpdate.h"
#include "GameLogic/Module/ProductionUpdate.h"
#include "GameLogic/Object.h"
#include "GameLogic/TerrainLogic.h"
#include "GameLogic/WorldWrapManager.h"

//-------------------------------------------------------------------------------------------------
AircraftRangeManager *TheAircraftRange = nullptr;

enum
{
	AIRCRAFT_RANGE_CHECK_INTERVAL = 5,				///< logic frames between two looks at the aircraft
	AIRCRAFT_RANGE_XFER_VERSION = 1
};

static const char *AIRCRAFT_RANGE_FILE = "Data\\INI\\AircraftRange.ini";

//-------------------------------------------------------------------------------------------------
/** With GENZH_WORLD_LOG set, one line per decision goes to WorldLog.txt in the game folder (it stops
	* after 4000 lines). Reads nothing of the simulation, changes nothing. */
//-------------------------------------------------------------------------------------------------
static void rangeLog( const char *format, ... )
{
	static Int lines = 0;
	if( getenv( "GENZH_WORLD_LOG" ) == nullptr || lines >= 4000 )
		return;

	FILE *file = fopen( "WorldLog.txt", "a" );
	if( file == nullptr )
		return;

	char buffer[ 512 ];
	va_list args;
	va_start( args, format );
	vsnprintf( buffer, sizeof( buffer ), format, args );
	va_end( args );
	buffer[ sizeof( buffer ) - 1 ] = 0;

	fprintf( file, "[%u] range: %s\n", TheGameLogic ? TheGameLogic->getFrame() : 0, buffer );
	fclose( file );
	++lines;
}

//-------------------------------------------------------------------------------------------------
static Real dist2D( const Coord3D *a, const Coord3D *b )
{
	const Real dx = a->x - b->x;
	const Real dy = a->y - b->y;
	return sqrtf( dx * dx + dy * dy );
}

//-------------------------------------------------------------------------------------------------
static UnicodeString nameOf( const Object *obj )
{
	UnicodeString name = obj->getTemplate()->getDisplayName();
	if( name.isEmpty() )
		name.translate( obj->getTemplate()->getName() );
	return name;
}

//-------------------------------------------------------------------------------------------------
static ParkingPlaceBehaviorInterface *findParkingPlace( const Object *obj )
{
	for( BehaviorModule **m = const_cast<Object *>( obj )->getBehaviorModules(); *m; ++m )
	{
		ParkingPlaceBehaviorInterface *pp = ( *m )->getParkingPlaceBehaviorInterface();
		if( pp != nullptr )
			return pp;
	}
	return nullptr;
}

//-------------------------------------------------------------------------------------------------
AircraftRangeManager::AircraftRangeManager() :
	m_loaded( FALSE ),
	m_active( FALSE ),
	m_kmPerUnit( 1.19f ),
	m_leashSlack( 100.0f ),
	m_oneWayFrames( 30 * LOGICFRAMES_PER_SECOND ),
	m_refuelDistance( 20.0f ),
	m_lastMessageFrame( 0 ),
	m_basesValid( FALSE ),
	m_basesFrame( 0 )
{
}

//-------------------------------------------------------------------------------------------------
AircraftRangeManager::~AircraftRangeManager()
{
}

//-------------------------------------------------------------------------------------------------
void AircraftRangeManager::reset()
{
	m_crafts.clear();
	m_active = FALSE;
	m_messagesThisFrame.clear();
	m_lastMessageFrame = 0;
	m_basesValid = FALSE;
	m_bases.clear();
}

//-------------------------------------------------------------------------------------------------
/** Data/INI/AircraftRange.ini, a plain list (not an INI block file, so the stock INI loader never sees it):
	*
	*   KmPerUnit = 1.19                   ground km per world unit of the world map
	*   LeashSlack = 100                   world units past the white ring before a chasing aircraft turns home
	*   OneWaySeconds = 30                 how long a one-way or stranded aircraft keeps flying past its white ring
	*   RefuelDistance = 20                world units from the edge of an airfield / helipad a helicopter refuels at
	*   Aircraft AmericaJetF16 = 550       combat radius in km of an aircraft template
	*   Power SuperweaponDaisyCutter = 7000   range in km of an aircraft special power
	*
	* ';' starts a comment. */
//-------------------------------------------------------------------------------------------------
void AircraftRangeManager::loadTable()
{
	m_loaded = TRUE;
	m_aircraftKm.clear();
	m_powerKm.clear();
	m_radiusCache.clear();

	File *file = TheFileSystem ? TheFileSystem->openFile( AIRCRAFT_RANGE_FILE, File::READ | File::BINARY ) : nullptr;
	if( file == nullptr )
		return;

	const Int size = file->size();
	char *buffer = file->readEntireAndClose();
	std::string text( buffer, size > 0 ? size : 0 );
	delete [] buffer;

	size_t start = 0;
	while( start < text.size() )
	{
		size_t end = text.find( '\n', start );
		if( end == std::string::npos )
			end = text.size();
		std::string line = text.substr( start, end - start );
		start = end + 1;

		const size_t comment = line.find( ';' );
		if( comment != std::string::npos )
			line.erase( comment );
		const size_t eq = line.find( '=' );
		if( eq == std::string::npos )
			continue;

		char left[ 256 ];
		char kind[ 64 ];
		char name[ 192 ];
		const std::string lhs = line.substr( 0, eq );
		const Real value = (Real)atof( line.c_str() + eq + 1 );
		strncpy( left, lhs.c_str(), sizeof( left ) - 1 );
		left[ sizeof( left ) - 1 ] = 0;

		const Int words = sscanf( left, "%63s %191s", kind, name );
		if( words == 1 )
		{
			if( stricmp( kind, "KmPerUnit" ) == 0 && value > 0.0f )
				m_kmPerUnit = value;
			else if( stricmp( kind, "LeashSlack" ) == 0 && value >= 0.0f )
				m_leashSlack = value;
			else if( stricmp( kind, "OneWaySeconds" ) == 0 && value > 0.0f )
				m_oneWayFrames = (UnsignedInt)( value * LOGICFRAMES_PER_SECOND );
			else if( stricmp( kind, "RefuelDistance" ) == 0 && value >= 0.0f )
				m_refuelDistance = value;
		}
		else if( words == 2 && value > 0.0f )
		{
			if( stricmp( kind, "Aircraft" ) == 0 )
				m_aircraftKm[ AsciiString( name ) ] = value;
			else if( stricmp( kind, "Power" ) == 0 )
				m_powerKm[ AsciiString( name ) ] = value;
		}
	}

	rangeLog( "table: %d aircraft, %d powers, %.3f km/unit, slack %.0f, one-way %u frames, refuel %.0f",
		(Int)m_aircraftKm.size(), (Int)m_powerKm.size(), m_kmPerUnit, m_leashSlack, m_oneWayFrames, m_refuelDistance );
}

//-------------------------------------------------------------------------------------------------
void AircraftRangeManager::newMap()
{
	if( !m_loaded )
		loadTable();

	m_active = !TheGlobalData->m_aircraftRangeDisabled
		&& TheWorldWrapManager != nullptr && TheWorldWrapManager->isActive()
		&& ( !m_aircraftKm.empty() || !m_powerKm.empty() );

	rangeLog( "new map: %s", m_active ? "active" : "off" );
}

//-------------------------------------------------------------------------------------------------
Real AircraftRangeManager::getRadius( const Object *obj ) const
{
	if( !m_active || obj == nullptr )
		return 0.0f;

	const ThingTemplate *tmpl = obj->getTemplate();
	std::map<const ThingTemplate *, Real>::const_iterator cached = m_radiusCache.find( tmpl );
	if( cached != m_radiusCache.end() )
		return cached->second;

	Real radius = 0.0f;
	std::map<AsciiString, Real>::const_iterator it = m_aircraftKm.find( tmpl->getName() );
	if( it != m_aircraftKm.end() )
		radius = it->second / m_kmPerUnit;
	m_radiusCache[ tmpl ] = radius;
	return radius;
}

//-------------------------------------------------------------------------------------------------
Real AircraftRangeManager::getPowerRange( const SpecialPowerTemplate *power ) const
{
	if( !m_active || power == nullptr )
		return 0.0f;

	std::map<AsciiString, Real>::const_iterator it = m_powerKm.find( power->getName() );
	return it != m_powerKm.end() ? it->second / m_kmPerUnit : 0.0f;
}

//-------------------------------------------------------------------------------------------------
/** Ground distance, the short way round on a map that wraps at the 180th meridian. The meridian is curved on an
	* Equal Earth map, so the way round is measured to the west / east edge at each point's own row. */
//-------------------------------------------------------------------------------------------------
Real AircraftRangeManager::distance( const Coord3D *a, const Coord3D *b ) const
{
	Real best = dist2D( a, b );
	if( TheWorldWrapManager == nullptr || !TheWorldWrapManager->isActive() )
		return best;

	Real westA, eastA, westB, eastB;
	if( !TheWorldWrapManager->getEdges( a->y, &westA, &eastA ) || !TheWorldWrapManager->getEdges( b->y, &westB, &eastB ) )
		return best;

	const Real dy = a->y - b->y;
	const Real viaWest = ( a->x - westA ) + ( eastB - b->x );	// a leaves west, b is reached from the east
	const Real viaEast = ( eastA - a->x ) + ( b->x - westB );
	const Real dx = viaWest < viaEast ? viaWest : viaEast;
	if( dx >= 0.0f )
	{
		const Real round = sqrtf( dx * dx + dy * dy );
		if( round < best )
			best = round;
	}
	return best;
}

//-------------------------------------------------------------------------------------------------
Bool AircraftRangeManager::isBase( const Object *obj, const Player *owner ) const
{
	if( obj == nullptr || obj->isEffectivelyDead() || !obj->isKindOf( KINDOF_STRUCTURE ) )
		return FALSE;
	if( owner != nullptr && obj->getControllingPlayer() != owner )
		return FALSE;
	if( obj->testStatus( OBJECT_STATUS_UNDER_CONSTRUCTION ) || obj->testStatus( OBJECT_STATUS_SOLD ) )
		return FALSE;
	return findParkingPlace( obj ) != nullptr;
}

//-------------------------------------------------------------------------------------------------
/** Every airfield / helipad on the map, gathered at most once per logic frame (ids, so nothing dangles). */
//-------------------------------------------------------------------------------------------------
void AircraftRangeManager::refreshBases() const
{
	const UnsignedInt now = TheGameLogic->getFrame();
	if( m_basesValid && m_basesFrame == now )
		return;
	m_basesValid = TRUE;
	m_basesFrame = now;
	m_bases.clear();
	for( Object *obj = TheGameLogic->getFirstObject(); obj != nullptr; obj = obj->getNextObject() )
	{
		if( obj->isKindOf( KINDOF_STRUCTURE ) && isBase( obj, nullptr ) )
			m_bases.push_back( obj->getID() );
	}
}

//-------------------------------------------------------------------------------------------------
Object *AircraftRangeManager::findNearestBase( const Player *owner, const Coord3D *pos ) const
{
	if( owner == nullptr )
		return nullptr;

	refreshBases();
	Object *best = nullptr;
	Real bestDist = 0.0f;
	for( std::vector<ObjectID>::const_iterator id = m_bases.begin(); id != m_bases.end(); ++id )
	{
		Object *obj = TheGameLogic->findObjectByID( *id );
		if( obj == nullptr || !isBase( obj, owner ) )
			continue;
		const Real d = distance( pos, obj->getPosition() );
		if( best == nullptr || d < bestDist )
		{
			best = obj;
			bestDist = d;
		}
	}
	return best;
}

//-------------------------------------------------------------------------------------------------
/** An airfield or helipad of `owner` the point lies on (or within RefuelDistance of its edge). */
//-------------------------------------------------------------------------------------------------
Object *AircraftRangeManager::baseAtPosition( const Player *owner, const Coord3D *pos ) const
{
	Object *base = findNearestBase( owner, pos );
	if( base == nullptr )
		return nullptr;
	const Real reach = base->getGeometryInfo().getBoundingCircleRadius() + m_refuelDistance;
	return dist2D( pos, base->getPosition() ) <= reach ? base : nullptr;
}

//-------------------------------------------------------------------------------------------------
void AircraftRangeManager::setHome( Craft &craft, const Object *base )
{
	craft.home = base->getID();
	craft.homePos = *base->getPosition();
	craft.virtualHome = FALSE;
}

//-------------------------------------------------------------------------------------------------
AircraftRangeManager::Craft &AircraftRangeManager::getCraft( Object *obj )
{
	std::map<ObjectID, Craft>::iterator it = m_crafts.find( obj->getID() );
	if( it != m_crafts.end() )
		return it->second;

	Craft craft;
	craft.home = INVALID_ID;
	craft.homePos = *obj->getPosition();
	craft.mode = MODE_NORMAL;
	craft.rebaseTarget = INVALID_ID;
	craft.timerStart = 0;
	craft.returning = FALSE;
	craft.doomed = FALSE;
	craft.virtualHome = FALSE;
	craft.lastOrderPos.zero();

	// first home: the airfield / helipad that built it, else the nearest one of its owner
	const Player *owner = obj->getControllingPlayer();
	Object *producer = TheGameLogic->findObjectByID( obj->getProducerID() );
	if( producer != nullptr && isBase( producer, owner ) )
		setHome( craft, producer );
	else
	{
		Object *nearest = findNearestBase( owner, obj->getPosition() );
		if( nearest != nullptr )
			setHome( craft, nearest );
		else
		{
			// placed on the map with no airfield anywhere (the national defender armies): it is based where it starts
			craft.virtualHome = TRUE;
		}
	}

	return m_crafts[ obj->getID() ] = craft;
}

//-------------------------------------------------------------------------------------------------
Bool AircraftRangeManager::getHomePosition( const Object *obj, Coord3D *pos ) const
{
	if( obj == nullptr )
		return FALSE;
	std::map<ObjectID, Craft>::const_iterator it = m_crafts.find( obj->getID() );
	if( it != m_crafts.end() )
	{
		if( !it->second.hasHome() )
			return FALSE;
		*pos = it->second.homePos;
		return TRUE;
	}

	// not looked at yet (just built): where it will be
	const Player *owner = obj->getControllingPlayer();
	const Object *producer = TheGameLogic->findObjectByID( obj->getProducerID() );
	if( producer == nullptr || !isBase( producer, owner ) )
		producer = findNearestBase( owner, obj->getPosition() );
	if( producer == nullptr )
	{
		*pos = *obj->getPosition();	// will be based where it is
		return TRUE;
	}
	*pos = *producer->getPosition();
	return TRUE;
}

//-------------------------------------------------------------------------------------------------
/** A message for the local player, each text once per frame (a group order judges every aircraft). */
//-------------------------------------------------------------------------------------------------
void AircraftRangeManager::tell( const Object *obj, const UnicodeString &text )
{
	if( TheInGameUI == nullptr || ThePlayerList == nullptr || obj->getControllingPlayer() != ThePlayerList->getLocalPlayer() )
		return;

	const UnsignedInt now = TheGameLogic->getFrame();
	if( now != m_lastMessageFrame )
	{
		m_messagesThisFrame.clear();
		m_lastMessageFrame = now;
	}
	for( std::vector<UnicodeString>::const_iterator it = m_messagesThisFrame.begin(); it != m_messagesThisFrame.end(); ++it )
	{
		if( it->compare( text ) == 0 )
			return;
	}
	m_messagesThisFrame.push_back( text );
	TheInGameUI->messageNoFormat( text );
}

//-------------------------------------------------------------------------------------------------
void AircraftRangeManager::sendHome( Object *obj, Craft &craft )
{
	AIUpdateInterface *ai = obj->getAI();
	if( ai == nullptr )
		return;

	craft.returning = TRUE;

	if( obj->getControllingPlayer() == ThePlayerList->getLocalPlayer() )
	{
		AudioEventRTS voice = *obj->getTemplate()->getPerUnitSound( "VoiceLowFuel" );
		voice.setObjectID( obj->getID() );
		TheAudio->addAudioEvent( &voice );
	}
	UnicodeString text;
	text.format( L"%s: low fuel, returning to base", nameOf( obj ).str() );
	tell( obj, text );

	JetAIUpdate *jet = ai->getJetAIUpdate();
	if( jet != nullptr )
		jet->friend_returnToBaseForRange();
	else
		ai->aiMoveToPosition( &craft.homePos, CMD_FROM_AI );

	rangeLog( "%s %u sent home (%.0f units away)", obj->getTemplate()->getName().str(), obj->getID(),
		distance( obj->getPosition(), &craft.homePos ) );
}

//-------------------------------------------------------------------------------------------------
void AircraftRangeManager::crash( Object *obj, const char *why )
{
	UnicodeString text;
	text.format( L"%s ran out of fuel and crashed", nameOf( obj ).str() );
	tell( obj, text );
	rangeLog( "%s %u crashed: %s", obj->getTemplate()->getName().str(), obj->getID(), why );
	m_crafts.erase( obj->getID() );
	obj->kill();
}

//-------------------------------------------------------------------------------------------------
void AircraftRangeManager::checkAircraft( Object *obj, Real radius, UnsignedInt now )
{
	AIUpdateInterface *ai = obj->getAI();
	if( ai == nullptr )
		return;

	Craft &craft = getCraft( obj );
	const Player *owner = obj->getControllingPlayer();
	JetAIUpdate *jet = ai->getJetAIUpdate();
	const Bool airborne = obj->isAirborneTarget();

	// refuel: a jet parked at an airfield / helipad (not one about to take off), a helicopter idle at one
	Object *refuelAt = nullptr;
	if( jet != nullptr )
	{
		if( !airborne && !jet->friend_isTakeoffOrLandingInProgress() && ( jet->isIdle() || jet->isReloading() ) )
		{
			Object *producer = TheGameLogic->findObjectByID( obj->getProducerID() );
			if( producer != nullptr && isBase( producer, owner ) )
				refuelAt = producer;
		}
	}
	else if( ai->isIdle() )
	{
		refuelAt = baseAtPosition( owner, obj->getPosition() );
		if( refuelAt == nullptr && craft.virtualHome && distance( obj->getPosition(), &craft.homePos ) <= m_refuelDistance + 30.0f )
		{
			craft.mode = MODE_NORMAL;	// back at its starting point
			craft.timerStart = 0;
			craft.returning = FALSE;
			craft.rebaseTarget = INVALID_ID;
		}
	}

	if( refuelAt != nullptr )
	{
		if( craft.home != refuelAt->getID() || craft.mode != MODE_NORMAL )
			rangeLog( "%s %u refuelled at %s %u", obj->getTemplate()->getName().str(), obj->getID(),
				refuelAt->getTemplate()->getName().str(), refuelAt->getID() );
		setHome( craft, refuelAt );
		craft.mode = MODE_NORMAL;
		craft.timerStart = 0;
		craft.returning = FALSE;
		craft.rebaseTarget = INVALID_ID;
		return;
	}

	// lost its home (destroyed, sold, captured): the nearest other one, else it is stranded
	Object *home = craft.home != INVALID_ID ? TheGameLogic->findObjectByID( craft.home ) : nullptr;
	if( !craft.virtualHome && ( home == nullptr || !isBase( home, owner ) ) )
	{
		Object *other = findNearestBase( owner, obj->getPosition() );
		if( other != nullptr )
		{
			setHome( craft, other );
			if( craft.mode == MODE_STRANDED )
			{
				craft.mode = MODE_NORMAL;
				craft.timerStart = 0;
			}
			craft.returning = FALSE;
		}
		else
		{
			craft.home = INVALID_ID;
			if( airborne && craft.mode != MODE_STRANDED && craft.mode != MODE_ONE_WAY )
			{
				craft.mode = MODE_STRANDED;
				craft.timerStart = now;
				UnicodeString text;
				text.format( L"%s has no airfield or helipad left", nameOf( obj ).str() );
				tell( obj, text );
				rangeLog( "%s %u stranded", obj->getTemplate()->getName().str(), obj->getID() );
			}
		}
	}

	if( craft.doomed )
	{
		crash( obj, "ordered back" );
		return;
	}

	if( !airborne )
		return;

	const Real fromHome = craft.hasHome() ? distance( obj->getPosition(), &craft.homePos ) : 0.0f;

	switch( craft.mode )
	{
		case MODE_NORMAL:
			if( craft.returning )
			{
				if( fromHome <= radius )
					craft.returning = FALSE;
			}
			else if( fromHome > radius + m_leashSlack )
				sendHome( obj, craft );
			break;

		case MODE_ONE_WAY:
		{
			if( craft.hasHome() && fromHome <= radius )
				break;	// not past the white ring yet
			if( craft.timerStart == 0 )
				craft.timerStart = now;

			Bool done = ai->isIdle();
			if( jet != nullptr && ( jet->isOutOfSpecialReloadAmmo() || jet->friend_isReturningToBase() ) )
				done = TRUE;
			if( done )
				crash( obj, "job done" );
			else if( now - craft.timerStart > m_oneWayFrames )
				crash( obj, "one-way time up" );
			break;
		}

		case MODE_REBASE:
		{
			Object *target = TheGameLogic->findObjectByID( craft.rebaseTarget );
			if( target == nullptr || !isBase( target, owner ) )
			{
				// the new base is gone: it is now a one-way flight from the old home
				craft.mode = MODE_ONE_WAY;
				craft.timerStart = 0;
			}
			break;
		}

		case MODE_STRANDED:
			if( craft.timerStart == 0 )
				craft.timerStart = now;
			if( now - craft.timerStart > m_oneWayFrames )
				crash( obj, "stranded" );
			break;
	}
}

//-------------------------------------------------------------------------------------------------
void AircraftRangeManager::update()
{
	if( !m_active || TheGameLogic == nullptr )
		return;

	const UnsignedInt now = TheGameLogic->getFrame();
	if( now % AIRCRAFT_RANGE_CHECK_INTERVAL != 0 )
		return;

	runSelfTest( now );

	for( Object *obj = TheGameLogic->getFirstObject(); obj != nullptr; )
	{
		Object *next = obj->getNextObject();
		if( !obj->isEffectivelyDead() && !obj->isKindOf( KINDOF_STRUCTURE ) )
		{
			const Real radius = getRadius( obj );
			if( radius > 0.0f )
				checkAircraft( obj, radius, now );
		}
		obj = next;
	}

	// forget aircraft that are gone
	for( std::map<ObjectID, Craft>::iterator it = m_crafts.begin(); it != m_crafts.end(); )
	{
		Object *obj = TheGameLogic->findObjectByID( it->first );
		if( obj == nullptr || obj->isEffectivelyDead() )
			m_crafts.erase( it++ );
		else
			++it;
	}
}

//-------------------------------------------------------------------------------------------------
/** Judge a command by the distance of its target from the aircraft's home. */
//-------------------------------------------------------------------------------------------------
Bool AircraftRangeManager::filterCommand( Object *obj, const AICommandParms *parms )
{
	if( !m_active || obj == nullptr || parms == nullptr )
		return TRUE;
	const Real radius = getRadius( obj );
	if( radius <= 0.0f )
		return TRUE;

	// where the command sends it
	Coord3D target;
	const Object *targetObj = nullptr;
	switch( parms->m_cmd )
	{
		case AICMD_MOVE_TO_OBJECT:
		case AICMD_ATTACK_OBJECT:
		case AICMD_FORCE_ATTACK_OBJECT:
		case AICMD_GUARD_OBJECT:
		case AICMD_ENTER:
		case AICMD_DOCK:
		case AICMD_GET_REPAIRED:
		case AICMD_COMMANDBUTTON_OBJ:
			if( parms->m_obj == nullptr )
				return TRUE;
			targetObj = parms->m_obj;
			target = *targetObj->getPosition();
			break;

		case AICMD_MOVE_TO_POSITION:
		case AICMD_MOVE_TO_POSITION_EVEN_IF_SLEEPING:
		case AICMD_TIGHTEN_TO_POSITION:
		case AICMD_MOVE_TO_POSITION_AND_EVACUATE:
		case AICMD_MOVE_TO_POSITION_AND_EVACUATE_AND_EXIT:
		case AICMD_ATTACK_POSITION:
		case AICMD_ATTACKMOVE_TO_POSITION:
		case AICMD_GUARD_POSITION:
		case AICMD_COMMANDBUTTON_POS:
		case AICMD_FOLLOW_PATH_APPEND:
			target = parms->m_pos;
			break;

		case AICMD_FOLLOW_PATH:
		case AICMD_FOLLOW_USER_PATH:
		{
			if( parms->m_coords.empty() )
				return TRUE;
			target = parms->m_coords.back();
			break;
		}

		default:
			return TRUE;	// no target of its own (idle, hunt, guard area, ...): the leash looks after it
	}

	// AI and scripts: always obeyed, never one-way; the leash brings the aircraft back
	if( parms->m_cmdSource != CMD_FROM_PLAYER )
	{
		std::map<ObjectID, Craft>::const_iterator it = m_crafts.find( obj->getID() );
		if( it != m_crafts.end() && it->second.hasHome() && distance( &it->second.homePos, &target ) > radius )
			rangeLog( "%s %u (player %d) AI/script order %d beyond its ring: %.0f units (radius %.0f)",
				obj->getTemplate()->getName().str(), obj->getID(),
				obj->getControllingPlayer() ? obj->getControllingPlayer()->getPlayerIndex() : -1, (Int)parms->m_cmd,
				distance( &it->second.homePos, &target ), radius );
		return TRUE;
	}

	Craft &craft = getCraft( obj );
	const Player *owner = obj->getControllingPlayer();
	const Bool repeat = craft.lastOrderPos.x == target.x && craft.lastOrderPos.y == target.y;
	craft.lastOrderPos = target;

	// another airfield / helipad of the same player: a move, if it is within the red ring
	const Object *base = nullptr;
	if( targetObj != nullptr && isBase( targetObj, owner ) )
		base = targetObj;
	else if( targetObj == nullptr )
		base = baseAtPosition( owner, &target );

	if( !craft.hasHome() )
	{
		// stranded: anything goes (the clock runs), and reaching a base saves it
		if( base != nullptr )
		{
			craft.mode = MODE_REBASE;
			craft.rebaseTarget = base->getID();
		}
		return TRUE;
	}

	const Real d = distance( &craft.homePos, &target );
	const UnicodeString &name = obj->getTemplate()->getDisplayName();

	if( base != nullptr && base->getID() != craft.home && d <= 2.0f * radius )
	{
		craft.mode = MODE_REBASE;
		craft.rebaseTarget = base->getID();
		craft.returning = FALSE;
		craft.timerStart = 0;
		if( !repeat )
		{
			UnicodeString text;
			text.format( L"%s: rebasing to another airfield (%.0f km)", name.str(), d * m_kmPerUnit );
			tell( obj, text );
		}
		rangeLog( "%s %u rebase to %u, %.0f units", obj->getTemplate()->getName().str(), obj->getID(), base->getID(), d );
		return TRUE;
	}

	if( d <= radius || ( base != nullptr && base->getID() == craft.home ) )
	{
		// a one-way aircraft past its white ring cannot come back
		if( craft.mode == MODE_ONE_WAY && distance( obj->getPosition(), &craft.homePos ) > radius && obj->isAirborneTarget() )
		{
			craft.doomed = TRUE;	// crashes at the next check (not inside the command)
			return FALSE;
		}
		craft.mode = MODE_NORMAL;
		craft.timerStart = 0;
		craft.returning = FALSE;
		return TRUE;
	}

	if( d <= 2.0f * radius )
	{
		if( craft.mode != MODE_ONE_WAY )
		{
			craft.mode = MODE_ONE_WAY;
			craft.timerStart = 0;
		}
		craft.returning = FALSE;
		if( !repeat )
		{
			UnicodeString text;
			text.format( L"Suicide mission: %s cannot fly %.0f km and back. It will not return.", name.str(), d * m_kmPerUnit );
			tell( obj, text );
		}
		rangeLog( "%s %u one-way, %.0f units (radius %.0f)", obj->getTemplate()->getName().str(), obj->getID(), d, radius );
		return TRUE;
	}

	UnicodeString text;
	text.format( L"Out of range: %s can reach %.0f km one way; the target is %.0f km from its base.", name.str(),
		2.0f * radius * m_kmPerUnit, d * m_kmPerUnit );
	tell( obj, text );
	rangeLog( "%s %u refused, %.0f units (radius %.0f)", obj->getTemplate()->getName().str(), obj->getID(), d, radius );
	return FALSE;
}

//-------------------------------------------------------------------------------------------------
Bool AircraftRangeManager::allowSpecialPower( const Object *source, const SpecialPowerTemplate *power, const Coord3D *target )
{
	if( !m_active || source == nullptr || target == nullptr )
		return TRUE;
	const Real range = getPowerRange( power );
	if( range <= 0.0f )
		return TRUE;

	const Real d = distance( source->getPosition(), target );
	if( d <= range )
		return TRUE;

	UnicodeString text;
	text.format( L"Out of range: this strike reaches %.0f km from the building that calls it; the target is %.0f km away.",
		range * m_kmPerUnit, d * m_kmPerUnit );
	tell( source, text );
	rangeLog( "power %s refused, %.0f units (range %.0f)", power->getName().str(), d, range );
	return FALSE;
}

//-------------------------------------------------------------------------------------------------
/** GENZH_RANGE_SELFTEST=1 (log in WorldLog.txt with GENZH_WORLD_LOG): the local player's first airfield / helipad
	* builds a jet and two helicopters (GENZH_RANGE_SELFTEST="Jet,Heli" for others than the F-16 and the Apache), then
	* player and script orders at set distances test refusal, normal flight, the one-way crash and the leash; the Daisy
	* Cutter range is checked once. It changes the game it runs in: for testing only. */
//-------------------------------------------------------------------------------------------------
void AircraftRangeManager::runSelfTest( UnsignedInt now )
{
	const char *env = getenv( "GENZH_RANGE_SELFTEST" );
	if( env == nullptr || ThePlayerList == nullptr || ThePlayerList->getLocalPlayer() == nullptr )
		return;

	static Int stage = 0;
	static ObjectID baseID = INVALID_ID;
	static ObjectID maxOldID = INVALID_ID;
	static ObjectID unitID[ 3 ] = { INVALID_ID, INVALID_ID, INVALID_ID };
	static UnsignedInt stageFrame = 0;
	static UnsignedInt firstFrame = 0;

	if( stage >= 3 )
		return;
	if( firstFrame == 0 )
		firstFrame = now;
	if( now < firstFrame + 90 )
		return;	// let a loaded game settle

	char names[ 2 ][ 64 ] = { "AmericaJetF16", "AmericaVehicleAH64" };
	if( strchr( env, ',' ) != nullptr )
		sscanf( env, "%63[^,],%63s", names[ 0 ], names[ 1 ] );

	Player *local = ThePlayerList->getLocalPlayer();
	Object *base = baseID != INVALID_ID ? TheGameLogic->findObjectByID( baseID ) : nullptr;

	if( stage == 0 )
	{
		refreshBases();
		for( std::vector<ObjectID>::const_iterator id = m_bases.begin(); id != m_bases.end() && base == nullptr; ++id )
		{
			Object *o = TheGameLogic->findObjectByID( *id );
			if( o != nullptr && o->getControllingPlayer() == local && o->getProductionUpdateInterface() != nullptr )
				base = o;
		}
		if( base == nullptr )
		{
			// place one next to the local player's first structure
			const Object *anchor = nullptr;
			for( Object *obj = TheGameLogic->getFirstObject(); obj != nullptr && anchor == nullptr; obj = obj->getNextObject() )
			{
				if( obj->getControllingPlayer() == local && obj->isKindOf( KINDOF_STRUCTURE ) && !obj->isEffectivelyDead() )
					anchor = obj;
			}
			const ThingTemplate *airfield = TheThingFactory->findTemplate( "AmericaAirfield" );
			if( anchor == nullptr || airfield == nullptr || local->getDefaultTeam() == nullptr )
			{
				rangeLog( "selftest: the local player has no airfield and none could be placed" );
				stage = 3;
				return;
			}
			Coord3D pos = *anchor->getPosition();
			pos.x += 120.0f;
			pos.z = TheTerrainLogic->getGroundHeight( pos.x, pos.y );
			base = TheThingFactory->newObject( airfield, local->getDefaultTeam() );
			base->setPosition( &pos );
			m_basesValid = FALSE;
			rangeLog( "selftest: placed an AmericaAirfield next to %s", anchor->getTemplate()->getName().str() );
		}
		baseID = base->getID();
		rangeLog( "selftest: base %s %u at (%.0f, %.0f)", base->getTemplate()->getName().str(), baseID,
			base->getPosition()->x, base->getPosition()->y );

		for( Object *obj = TheGameLogic->getFirstObject(); obj != nullptr; obj = obj->getNextObject() )
		{
			if( obj->getID() > maxOldID )
				maxOldID = obj->getID();
			if( obj->getControllingPlayer() == local && getRadius( obj ) > 0.0f )
				rangeLog( "selftest: own aircraft %s %u, radius %.0f units", obj->getTemplate()->getName().str(), obj->getID(), getRadius( obj ) );
		}

		const SpecialPowerTemplate *power = TheSpecialPowerStore->findSpecialPowerTemplate( "SuperweaponDaisyCutter" );
		const Real kms[ 2 ] = { 1000.0f, 8000.0f };
		for( Int i = 0; i < 2 && power != nullptr; ++i )
		{
			Coord3D target = *base->getPosition();
			target.x += kms[ i ] / m_kmPerUnit;
			const Bool allowed = allowSpecialPower( base, power, &target );
			rangeLog( "selftest: Daisy Cutter at %.0f km east: %s (expected %s)", kms[ i ], allowed ? "allowed" : "refused",
				i == 0 ? "allowed" : "refused" );
		}

		const ThingTemplate *jet = TheThingFactory->findTemplate( names[ 0 ] );
		const ThingTemplate *heli = TheThingFactory->findTemplate( names[ 1 ] );
		if( jet == nullptr || heli == nullptr || local->getDefaultTeam() == nullptr )
		{
			rangeLog( "selftest: unknown template %s or %s", names[ 0 ], names[ 1 ] );
			stage = 3;
			return;
		}

		// the jet appears in the air over its airfield (it has to land there by itself), the helicopters beside it
		for( Int i = 0; i < 3; ++i )
		{
			Object *unit = TheThingFactory->newObject( i == 0 ? jet : heli, local->getDefaultTeam() );
			Coord3D pos = *base->getPosition();
			pos.x += i == 0 ? -150.0f : 0.0f;
			pos.y += i == 1 ? 60.0f : ( i == 2 ? -60.0f : 0.0f );
			pos.z = TheTerrainLogic->getGroundHeight( pos.x, pos.y ) + ( i == 0 ? 120.0f : 0.0f );
			unit->setPosition( &pos );
			unit->setProducer( base );
			unitID[ i ] = unit->getID();
		}
		rangeLog( "selftest: created %s %u (in the air) and 2 x %s %u %u", names[ 0 ], unitID[ 0 ], names[ 1 ], unitID[ 1 ], unitID[ 2 ] );
		stage = 1;
		stageFrame = now;
		return;
	}

	if( base == nullptr )
	{
		rangeLog( "selftest: the base is gone" );
		stage = 3;
		return;
	}

	// find the new units
	for( Object *obj = TheGameLogic->getFirstObject(); obj != nullptr; obj = obj->getNextObject() )
	{
		if( obj->getID() <= maxOldID || obj->getControllingPlayer() != local )
			continue;
		const AsciiString &name = obj->getTemplate()->getName();
		if( unitID[ 0 ] == INVALID_ID && name.compare( names[ 0 ] ) == 0 )
			unitID[ 0 ] = obj->getID();
		else if( name.compare( names[ 1 ] ) == 0 && unitID[ 1 ] != obj->getID() && unitID[ 2 ] != obj->getID() )
		{
			if( unitID[ 1 ] == INVALID_ID )
				unitID[ 1 ] = obj->getID();
			else if( unitID[ 2 ] == INVALID_ID )
				unitID[ 2 ] = obj->getID();
		}
	}

	Object *units[ 3 ];
	for( Int i = 0; i < 3; ++i )
		units[ i ] = unitID[ i ] != INVALID_ID ? TheGameLogic->findObjectByID( unitID[ i ] ) : nullptr;

	if( stage == 1 )
	{
		if( units[ 0 ] && ( now - stageFrame ) % ( 5 * LOGICFRAMES_PER_SECOND ) == 0 )
			rangeLog( "selftest: waiting: jet %s, idle %d, home %u", units[ 0 ]->isAirborneTarget() ? "airborne" : "on ground",
				units[ 0 ]->getAI() ? units[ 0 ]->getAI()->isIdle() : -1, m_crafts.count( unitID[ 0 ] ) ? m_crafts[ unitID[ 0 ] ].home : 0 );
		const Bool jetParked = units[ 0 ] && !units[ 0 ]->isAirborneTarget() && units[ 0 ]->getAI() && units[ 0 ]->getAI()->isIdle();
		const Bool ready = units[ 0 ] && units[ 1 ] && units[ 2 ] && units[ 1 ]->getAI()->isIdle() && units[ 2 ]->getAI()->isIdle()
			&& ( jetParked || now > stageFrame + 60 * LOGICFRAMES_PER_SECOND );
		if( !ready )
		{
			if( now > stageFrame + 30 * LOGICFRAMES_PER_SECOND * 4 )
			{
				rangeLog( "selftest: units not ready after 2 minutes (%u %u %u)", unitID[ 0 ], unitID[ 1 ], unitID[ 2 ] );
				stage = 3;
			}
			return;
		}

		const Coord3D *b = base->getPosition();
		const Real rj = getRadius( units[ 0 ] );
		const Real rh = getRadius( units[ 1 ] );
		Coord3D p;

		p = *b; p.x += 2.6f * rj;
		units[ 0 ]->getAI()->aiMoveToPosition( &p, CMD_FROM_PLAYER );
		rangeLog( "selftest: jet ordered 2.6 R east (expect refused): mode %d", m_crafts[ unitID[ 0 ] ].mode );

		p = *b; p.x += 0.7f * rj;
		units[ 0 ]->getAI()->aiMoveToPosition( &p, CMD_FROM_PLAYER );
		rangeLog( "selftest: jet ordered 0.7 R east (expect normal): mode %d", m_crafts[ unitID[ 0 ] ].mode );

		p = *b; p.y += 1.5f * rh;
		units[ 1 ]->getAI()->aiMoveToPosition( &p, CMD_FROM_PLAYER );
		rangeLog( "selftest: heli 1 ordered 1.5 R north (expect one-way, then crash): mode %d", m_crafts[ unitID[ 1 ] ].mode );

		p = *b; p.y -= 1.8f * rh;
		units[ 2 ]->getAI()->aiMoveToPosition( &p, CMD_FROM_SCRIPT );
		rangeLog( "selftest: heli 2 sent 1.8 R south by script (expect leash: sent home): mode %d", m_crafts[ unitID[ 2 ] ].mode );

		stage = 2;
		stageFrame = now;
		return;
	}

	if( stage == 2 )
	{
		if( ( now - stageFrame ) % LOGICFRAMES_PER_SECOND == 0 )
		{
			for( Int i = 0; i < 3; ++i )
			{
				if( units[ i ] == nullptr )
				{
					rangeLog( "selftest: unit %d (%u) is gone", i, unitID[ i ] );
					continue;
				}
				std::map<ObjectID, Craft>::const_iterator it = m_crafts.find( unitID[ i ] );
				const Coord3D *pos = units[ i ]->getPosition();
				rangeLog( "selftest: unit %d %s mode %d, %.0f units from home, %s, idle %d",
					i, units[ i ]->getTemplate()->getName().str(), it != m_crafts.end() ? it->second.mode : -1,
					it != m_crafts.end() ? distance( pos, &it->second.homePos ) : -1.0f,
					units[ i ]->isAirborneTarget() ? "airborne" : "on ground", units[ i ]->getAI()->isIdle() );
			}
		}
		if( now > stageFrame + 90 * LOGICFRAMES_PER_SECOND )
		{
			rangeLog( "selftest: done" );
			stage = 3;
			if( getenv( "GENZH_RANGE_SAVE" ) != nullptr && TheGameState != nullptr )
			{
				// a save game with this block in it, for a load test (GENZH_RANGE_SAVE=<file name in the Save folder>)
				const SaveResult result = TheGameState->saveGame( AsciiString( getenv( "GENZH_RANGE_SAVE" ) ),
					UnicodeString( L"aircraft range test" ), SAVE_FILE_TYPE_NORMAL );
				rangeLog( "selftest: saved %s (as %s) with %d aircraft states: code %d", getenv( "GENZH_RANGE_SAVE" ), result.filename.str(), (Int)m_crafts.size(), (Int)result.saveCode );
			}
		}
	}
}

//-------------------------------------------------------------------------------------------------
void AircraftRangeManager::crc( Xfer *xfer )
{
}

//-------------------------------------------------------------------------------------------------
void AircraftRangeManager::xfer( Xfer *xfer )
{
	XferVersion currentVersion = AIRCRAFT_RANGE_XFER_VERSION;
	XferVersion version = currentVersion;
	xfer->xferVersion( &version, currentVersion );

	UnsignedInt count = (UnsignedInt)m_crafts.size();
	xfer->xferUnsignedInt( &count );

	if( xfer->getXferMode() == XFER_SAVE )
	{
		for( std::map<ObjectID, Craft>::iterator it = m_crafts.begin(); it != m_crafts.end(); ++it )
		{
			ObjectID id = it->first;
			Craft &c = it->second;
			xfer->xferObjectID( &id );
			xfer->xferObjectID( &c.home );
			xfer->xferCoord3D( &c.homePos );
			xfer->xferInt( &c.mode );
			xfer->xferObjectID( &c.rebaseTarget );
			xfer->xferUnsignedInt( &c.timerStart );
			xfer->xferBool( &c.returning );
			xfer->xferCoord3D( &c.lastOrderPos );
			xfer->xferBool( &c.doomed );
			xfer->xferBool( &c.virtualHome );
		}
	}
	else
	{
		m_crafts.clear();
		for( UnsignedInt i = 0; i < count; ++i )
		{
			ObjectID id = INVALID_ID;
			Craft c;
			xfer->xferObjectID( &id );
			xfer->xferObjectID( &c.home );
			xfer->xferCoord3D( &c.homePos );
			xfer->xferInt( &c.mode );
			xfer->xferObjectID( &c.rebaseTarget );
			xfer->xferUnsignedInt( &c.timerStart );
			xfer->xferBool( &c.returning );
			xfer->xferCoord3D( &c.lastOrderPos );
			xfer->xferBool( &c.doomed );
			xfer->xferBool( &c.virtualHome );
			m_crafts[ id ] = c;
		}
		rangeLog( "loaded %u aircraft states from the save game", count );
	}
}

//-------------------------------------------------------------------------------------------------
void AircraftRangeManager::loadPostProcess()
{
}
