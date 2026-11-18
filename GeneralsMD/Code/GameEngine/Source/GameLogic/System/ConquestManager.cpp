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

// FILE: ConquestManager.cpp //////////////////////////////////////////////////////////////////////
// TheSuperHackers @feature mohamed 15/11/2026 Global conquest mode. See ConquestManager.h.
///////////////////////////////////////////////////////////////////////////////////////////////////

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "Common/DataChunk.h"
#include "Common/GlobalData.h"
#include "Common/MapObject.h"
#include "Common/MapReaderWriterInfo.h"
#include "Common/NameKeyGenerator.h"
#include "Common/Player.h"
#include "Common/PlayerList.h"
#include "Common/PlayerTemplate.h"
#include "Common/Radar.h"
#include "Common/Recorder.h"
#include "Common/Team.h"

#include "GameClient/InGameUI.h"

#include "GameLogic/ConquestManager.h"
#include "GameLogic/GameLogic.h"
#include "GameLogic/AI.h"
#include "GameLogic/Module/AIUpdate.h"
#include "GameLogic/Module/BodyModule.h"
#include "GameLogic/Object.h"
#include "GameLogic/PartitionManager.h"
#include "GameLogic/ScriptActions.h"
#include "GameLogic/ScriptEngine.h"
#include "GameLogic/Scripts.h"
#include "GameLogic/TerrainLogic.h"
#include "GameLogic/VictoryConditions.h"

#include "GameNetwork/GameInfo.h"
#include "GameNetwork/NetworkDefs.h"

//-------------------------------------------------------------------------------------------------
ConquestManager *TheConquestManager = nullptr;

static const Int CONQUEST_CHUNK_VERSION = 1;
static const Int CONQUEST_CHECK_INTERVAL = 5;		///< logic frames between two looks at the hospitals
static const Int CELL_TERRITORY_SLOTS = 4;			///< territories one partition cell can belong to

//-------------------------------------------------------------------------------------------------
/** With GENZH_WORLD_LOG set, the defenders and the hand-overs of armies are noted in WorldLog.txt in the
	* game folder (4000 lines at most). Changes nothing in the game. */
//-------------------------------------------------------------------------------------------------
static void conquestLog( const char *format, ... )
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
static Bool areAllies( const Player *p1, const Player *p2 )
{
	return p1 != p2 &&
		p1->getRelationship( p2->getDefaultTeam() ) == ALLIES &&
		p2->getRelationship( p1->getDefaultTeam() ) == ALLIES;
}

//-------------------------------------------------------------------------------------------------
/** A player that takes part in the conquest: not the neutral or civilian side, not an observer. */
//-------------------------------------------------------------------------------------------------
static Bool isContender( const Player *player )
{
	if( player == nullptr || player == ThePlayerList->getNeutralPlayer() || player->isPlayerObserver() )
		return FALSE;

	const PlayerTemplate *playerTemplate = player->getPlayerTemplate();
	if( playerTemplate == nullptr )
		return FALSE;

	return playerTemplate != ThePlayerTemplateStore->findPlayerTemplate( NAMEKEY( "FactionCivilian" ) );
}

//-------------------------------------------------------------------------------------------------
static const char *DEFENDER_SIDE_NAME = "PlyrDefenders";

//-------------------------------------------------------------------------------------------------
Bool ConquestManager::isDefenderPlayer( const Player *player )
{
	return player != nullptr && player->getPlayerNameKey() == NAMEKEY( DEFENDER_SIDE_NAME );
}

//-------------------------------------------------------------------------------------------------
Player *ConquestManager::findDefenders() const
{
	return ThePlayerList->findPlayerWithNameKey( NAMEKEY( DEFENDER_SIDE_NAME ) );
}

//-------------------------------------------------------------------------------------------------
/** The defender side is an enemy of every player (and every player of it), and every defending unit
	* guards the ground it was placed on, so an army only reacts to what comes into its country. */
//-------------------------------------------------------------------------------------------------
void ConquestManager::startDefenders()
{
	Player *defenders = findDefenders();
	if( defenders == nullptr )
		return;

	for( Int i = 0; i < ThePlayerList->getPlayerCount(); ++i )
	{
		Player *other = ThePlayerList->getNthPlayer( i );
		if( other == nullptr || other == defenders || !isContender( other ) )
			continue;

		defenders->setPlayerRelationship( other, ENEMIES );
		other->setPlayerRelationship( defenders, ENEMIES );
	}

	Int guards = 0;
	for( Object *obj = TheGameLogic->getFirstObject(); obj != nullptr; obj = obj->getNextObject() )
	{
		if( obj->getControllingPlayer() != defenders )
			continue;

		AIUpdateInterface *ai = obj->getAIUpdateInterface();
		if( ai == nullptr || !obj->isMobile() )
			continue;

		ai->aiGuardPosition( obj->getPosition(), GUARDMODE_NORMAL, CMD_FROM_AI );
		++guards;
	}

	DEBUG_LOG(( "ConquestManager: defender side is hostile to all players, %d units on guard", guards ));
	conquestLog( "frame %u: defender side %s: hostile to every player, %d units on guard", TheGameLogic->getFrame(), DEFENDER_SIDE_NAME, guards );
}

//-------------------------------------------------------------------------------------------------
/** Hand the army and the capital tech a country still has to the player who holds its hospital. What
	* has been destroyed or captured on its own is left alone: only the defenders' objects change owner. */
//-------------------------------------------------------------------------------------------------
void ConquestManager::giveForcesTo( const Territory &territory, Player *player )
{
	Player *defenders = findDefenders();
	if( defenders == nullptr || player == nullptr || player == defenders )
		return;

	AsciiString armyPrefix, artillery, pad;
	armyPrefix.format( "Army_%s_", territory.key.str() );
	artillery.format( "CapArty_%s", territory.key.str() );
	pad.format( "CapPad_%s", territory.key.str() );

	Int given = 0;
	for( Object *obj = TheGameLogic->getFirstObject(); obj != nullptr; obj = obj->getNextObject() )
	{
		if( obj->getControllingPlayer() != defenders )
			continue;

		const AsciiString &name = obj->getName();
		if( name.isEmpty() || !( name.startsWith( armyPrefix ) || name == artillery || name == pad ) )
			continue;

		obj->setTeam( player->getDefaultTeam() );
		++given;
	}

	if( given > 0 )
		DEBUG_LOG(( "ConquestManager: %d objects of %s go to %ls", given, territory.key.str(), player->getPlayerDisplayName().str() ));
	conquestLog( "frame %u: %d objects of %s go to %ls", TheGameLogic->getFrame(), given, territory.key.str(), player->getPlayerDisplayName().str() );
}

//-------------------------------------------------------------------------------------------------
ConquestManager::ConquestManager()
{
	reset();
}

//-------------------------------------------------------------------------------------------------
ConquestManager::~ConquestManager()
{
}

//-------------------------------------------------------------------------------------------------
void ConquestManager::reset()
{
	m_active = FALSE;
	m_started = FALSE;
	m_fromSave = FALSE;
	m_victoryDeclared = FALSE;
	m_width = 0;
	m_height = 0;
	m_border = 0;
	m_victoryPercent = 75;
	m_grid.clear();
	m_territories.clear();
}

//-------------------------------------------------------------------------------------------------
/** Chunk layout (tools/worldgen/mapio.py, ConquestData): width, height, border, victory percent,
	* territory count, then per territory key, name, hospital, label x/y/radius, land cells, and
	* finally the grid size and the grid, one byte per height sample, south row first. */
//-------------------------------------------------------------------------------------------------
Bool ConquestManager::parseConquestDataChunk( DataChunkInput &file, DataChunkInfo *info, void *userData )
{
	ConquestManager *self = (ConquestManager *)userData;

	if( info->version != CONQUEST_CHUNK_VERSION )
	{
		DEBUG_LOG(( "ConquestManager: ConquestData version %d is not supported, conquest mode is off", info->version ));
		return TRUE;
	}

	self->m_width = file.readInt();
	self->m_height = file.readInt();
	self->m_border = file.readInt();
	self->m_victoryPercent = file.readInt();

	const Int count = file.readInt();
	if( count < 1 || count > 254 )
		return TRUE;

	self->m_territories.resize( count );
	for( Int i = 0; i < count; ++i )
	{
		Territory &territory = self->m_territories[ i ];
		territory.key = file.readAsciiString();
		territory.name = file.readUnicodeString();
		territory.hospitalName = file.readAsciiString();
		territory.labelPos.x = file.readReal();
		territory.labelPos.y = file.readReal();
		territory.labelPos.z = 0.0f;
		territory.labelRadius = file.readReal();
		territory.landCells = file.readInt();
		territory.hospitalID = INVALID_ID;
		territory.ownerIndex = -1;
		territory.viewerMask = 0;
	}

	const Int size = file.readInt();
	if( self->m_width <= 0 || self->m_height <= 0 || size != self->m_width * self->m_height )
	{
		DEBUG_LOG(( "ConquestManager: bad territory grid (%d x %d, %d bytes), conquest mode is off", self->m_width, self->m_height, size ));
		self->m_territories.clear();
		return TRUE;
	}

	self->m_grid.resize( size );
	file.readArrayOfBytes( (char *)&self->m_grid[ 0 ], size );
	self->m_active = TRUE;
	return TRUE;
}

//-------------------------------------------------------------------------------------------------
/** Called from GameLogic::startNewGame once the map's objects exist. Only reads the data: the
	* hospitals are looked up on the first update, because a saved game restores its objects (and
	* who owns them) after this point. */
//-------------------------------------------------------------------------------------------------
void ConquestManager::newMap( Bool loadingSaveGame )
{
	reset();

	if( TheGlobalData->m_conquestDisabled )
		return;

	CachedFileInputStream theInputStream;
	if( !theInputStream.open( TheGlobalData->m_mapName ) )
		return;

	ChunkInputStream *pStrm = &theInputStream;
	DataChunkInput file( pStrm );
	file.registerParser( "ConquestData", AsciiString::TheEmptyString, parseConquestDataChunk );
	if( !file.parse( this ) || !m_active )
	{
		reset();
		return;
	}

	m_fromSave = loadingSaveGame;

	for( std::vector<Territory>::iterator it = m_territories.begin(); it != m_territories.end(); ++it )
		it->labelPos.z = TheTerrainLogic->getGroundHeight( it->labelPos.x, it->labelPos.y );

	buildCells();

	DEBUG_LOG(( "ConquestManager: %d territories, %d needed for victory", getTerritoryCount(), getVictoryCount() ));
	conquestLog( "map started: %d territories, %d needed for victory", getTerritoryCount(), getVictoryCount() );
}

//-------------------------------------------------------------------------------------------------
/** List, for every territory, the partition cells it touches. A cell on a border belongs to each
	* territory that has a sample in it, so both neighbours see the border itself. */
//-------------------------------------------------------------------------------------------------
void ConquestManager::buildCells()
{
	const Int cellsX = ThePartitionManager->getCellCountX();
	const Int cellsY = ThePartitionManager->getCellCountY();
	std::vector<UnsignedByte> seen;
	seen.resize( cellsX * cellsY * CELL_TERRITORY_SLOTS, (UnsignedByte)0 );

	for( Int row = 0; row < m_height; ++row )
	{
		const UnsignedByte *line = &m_grid[ row * m_width ];
		const Real worldY = ( row - m_border ) * MAP_XY_FACTOR;

		for( Int col = 0; col < m_width; ++col )
		{
			const UnsignedByte number = line[ col ];
			if( number == 0 || number > m_territories.size() )
				continue;

			Int cellX, cellY;
			ThePartitionManager->worldToCell( ( col - m_border ) * MAP_XY_FACTOR, worldY, &cellX, &cellY );
			if( cellX < 0 || cellY < 0 || cellX >= cellsX || cellY >= cellsY )
				continue;

			const Int cellIndex = cellY * cellsX + cellX;
			UnsignedByte *slots = &seen[ cellIndex * CELL_TERRITORY_SLOTS ];
			for( Int k = 0; k < CELL_TERRITORY_SLOTS; ++k )
			{
				if( slots[ k ] == number )
					break;

				if( slots[ k ] == 0 )
				{
					slots[ k ] = number;
					m_territories[ number - 1 ].cells.push_back( cellIndex );
					break;
				}
			}
		}
	}
}

//-------------------------------------------------------------------------------------------------
const ConquestManager::Territory *ConquestManager::getTerritory( Int index ) const
{
	if( index < 0 || index >= getTerritoryCount() )
		return nullptr;

	return &m_territories[ index ];
}

//-------------------------------------------------------------------------------------------------
Int ConquestManager::getTerritoryIndexAt( const Coord3D *pos ) const
{
	if( !m_active || pos == nullptr )
		return -1;

	// Height samples sit on whole multiples of MAP_XY_FACTOR; a position belongs to the nearest one.
	const Int col = REAL_TO_INT_FLOOR( pos->x / MAP_XY_FACTOR + 0.5f ) + m_border;
	const Int row = REAL_TO_INT_FLOOR( pos->y / MAP_XY_FACTOR + 0.5f ) + m_border;
	if( col < 0 || row < 0 || col >= m_width || row >= m_height )
		return -1;

	const Int number = m_grid[ row * m_width + col ];
	return number <= getTerritoryCount() ? number - 1 : -1;
}

//-------------------------------------------------------------------------------------------------
Player *ConquestManager::getTerritoryOwner( Int index ) const
{
	const Territory *territory = getTerritory( index );
	if( territory == nullptr || territory->ownerIndex < 0 )
		return nullptr;

	return ThePlayerList->getNthPlayer( territory->ownerIndex );
}

//-------------------------------------------------------------------------------------------------
/** The build rule: inside a territory held by the player or one of his allies. */
//-------------------------------------------------------------------------------------------------
Bool ConquestManager::canPlayerBuildAt( const Player *player, const Coord3D *pos ) const
{
	// Not our business: no conquest on this map, or somebody who is not playing it (map scripts).
	if( !m_active || !m_started || !isContender( player ) )
		return TRUE;

	const Player *owner = getTerritoryOwner( getTerritoryIndexAt( pos ) );
	return owner != nullptr && ( owner == player || areAllies( owner, player ) );
}

//-------------------------------------------------------------------------------------------------
Int ConquestManager::getVictoryCount() const
{
	Int percent = TheGlobalData->m_conquestVictoryPercent > 0 ? TheGlobalData->m_conquestVictoryPercent : m_victoryPercent;
	if( percent < 1 )
		percent = 1;
	if( percent > 100 )
		percent = 100;

	const Int needed = ( getTerritoryCount() * percent + 99 ) / 100;
	return needed < 1 ? 1 : needed;
}

//-------------------------------------------------------------------------------------------------
Int ConquestManager::getTerritoryCountForAlliance( const Player *player ) const
{
	if( player == nullptr )
		return 0;

	Int count = 0;
	for( Int i = 0; i < getTerritoryCount(); ++i )
	{
		const Player *owner = getTerritoryOwner( i );
		if( owner != nullptr && ( owner == player || areAllies( owner, player ) ) )
			++count;
	}

	return count;
}

//-------------------------------------------------------------------------------------------------
Player *ConquestManager::findOwner( const Territory &territory ) const
{
	Object *hospital = TheGameLogic->findObjectByID( territory.hospitalID );
	if( hospital == nullptr )
		return nullptr;

	Player *player = hospital->getControllingPlayer();
	return isContender( player ) ? player : nullptr;
}

//-------------------------------------------------------------------------------------------------
PlayerMaskType ConquestManager::viewersFor( const Player *owner ) const
{
	if( owner == nullptr )
		return 0;

	PlayerMaskType mask = owner->getPlayerMask();
	for( Int i = 0; i < ThePlayerList->getPlayerCount(); ++i )
	{
		const Player *other = ThePlayerList->getNthPlayer( i );
		if( other != nullptr && isContender( other ) && areAllies( owner, other ) )
			mask |= other->getPlayerMask();
	}

	return mask;
}

//-------------------------------------------------------------------------------------------------
/** Bring the lookers held on a territory's cells in line with the players who should see it. */
//-------------------------------------------------------------------------------------------------
void ConquestManager::applyViewers( Territory &territory, PlayerMaskType newMask )
{
	const PlayerMaskType oldMask = territory.viewerMask;

	for( Int i = 0; i < MAX_PLAYER_COUNT; ++i )
	{
		const PlayerMaskType bit = (PlayerMaskType)( 1 << i );
		if( ( newMask & bit ) != 0 && ( oldMask & bit ) == 0 )
			ThePartitionManager->adjustLookersForCells( territory.cells, i, TRUE );
		else if( ( newMask & bit ) == 0 && ( oldMask & bit ) != 0 )
			ThePartitionManager->adjustLookersForCells( territory.cells, i, FALSE );
	}

	territory.viewerMask = newMask;
}

//-------------------------------------------------------------------------------------------------
/** First update on a map: find the hospitals and set up, or take over, the state of the game. */
//-------------------------------------------------------------------------------------------------
void ConquestManager::start()
{
	for( std::vector<Territory>::iterator it = m_territories.begin(); it != m_territories.end(); ++it )
	{
		Object *hospital = TheScriptEngine->getUnitNamed( it->hospitalName );
		if( hospital == nullptr )
		{
			DEBUG_LOG(( "ConquestManager: hospital '%s' of %s is not on the map", it->hospitalName.str(), it->key.str() ));
			continue;
		}

		it->hospitalID = hospital->getID();

		// The map says so already; this covers a hospital that was edited by hand.
		BodyModuleInterface *body = hospital->getBodyModule();
		if( body != nullptr )
			body->setIndestructible( TRUE );
	}

	if( m_fromSave )
	{
		// The saved partition cells already hold the lookers that were in place: only note them.
		for( std::vector<Territory>::iterator it = m_territories.begin(); it != m_territories.end(); ++it )
		{
			const Player *owner = findOwner( *it );
			it->ownerIndex = owner ? owner->getPlayerIndex() : -1;
			it->viewerMask = viewersFor( owner );
		}
		return;
	}

	// Fog instead of shroud everywhere: terrain and borders are visible, what stands on them is not.
	for( Int i = 0; i < ThePlayerList->getPlayerCount(); ++i )
	{
		const Player *player = ThePlayerList->getNthPlayer( i );
		if( player != nullptr && isContender( player ) )
			ThePartitionManager->revealMapForPlayer( player->getPlayerIndex() );
	}

	startDefenders();

	// Everybody starts as the owner of the territory he starts in.
	if( TheGameInfo )
	{
		for( Int slotIndex = 0; slotIndex < MAX_SLOTS; ++slotIndex )
		{
			const GameSlot *slot = TheGameInfo->getConstSlot( slotIndex );
			if( slot == nullptr || !slot->isOccupied() || slot->getPlayerTemplate() == PLAYERTEMPLATE_OBSERVER )
				continue;

			AsciiString playerName;
			playerName.format( "player%d", slotIndex );
			Player *player = ThePlayerList->findPlayerWithNameKey( TheNameKeyGenerator->nameToKey( playerName ) );
			if( !isContender( player ) )
				continue;

			AsciiString waypointName;
			waypointName.format( "Player_%d_Start", slot->getStartPos() + 1 );	// start pos waypoints are 1-based
			Waypoint *waypoint = TheTerrainLogic->getWaypointByName( waypointName );
			if( waypoint == nullptr )
				continue;

			const Int index = getTerritoryIndexAt( waypoint->getLocation() );
			if( index < 0 )
				continue;

			Object *hospital = TheGameLogic->findObjectByID( m_territories[ index ].hospitalID );
			if( hospital != nullptr && findOwner( m_territories[ index ] ) == nullptr )
				hospital->setTeam( player->getDefaultTeam() );
		}
	}
	else
	{
		// A map started on its own (no lobby): the local player gets the first start position.
		Player *player = ThePlayerList->getLocalPlayer();
		Waypoint *waypoint = TheTerrainLogic->getWaypointByName( AsciiString( "Player_1_Start" ) );
		if( isContender( player ) && waypoint != nullptr )
		{
			const Int index = getTerritoryIndexAt( waypoint->getLocation() );
			Object *hospital = index >= 0 ? TheGameLogic->findObjectByID( m_territories[ index ].hospitalID ) : nullptr;
			if( hospital != nullptr && findOwner( m_territories[ index ] ) == nullptr )
				hospital->setTeam( player->getDefaultTeam() );
		}
	}

	checkOwnership( FALSE );
}

//-------------------------------------------------------------------------------------------------
void ConquestManager::checkOwnership( Bool announce )
{
	for( std::vector<Territory>::iterator it = m_territories.begin(); it != m_territories.end(); ++it )
	{
		Player *owner = findOwner( *it );

		// A defeated player keeps nothing. His other buildings are destroyed; the hospital cannot be,
		// so it is handed back to the neutral side and can be captured again.
		if( owner != nullptr && !owner->isPlayerActive() )
		{
			Object *hospital = TheGameLogic->findObjectByID( it->hospitalID );
			if( hospital != nullptr )
				hospital->setTeam( ThePlayerList->getNeutralPlayer()->getDefaultTeam() );
			owner = nullptr;
		}

		const Int ownerIndex = owner ? owner->getPlayerIndex() : -1;
		const Bool changed = ownerIndex != it->ownerIndex;
		it->ownerIndex = ownerIndex;

		if( changed && owner != nullptr )
			giveForcesTo( *it, owner );		// the army and the capital tech of the country

		applyViewers( *it, viewersFor( owner ) );		// also follows a change of alliances

		if( changed && announce )
		{
			UnicodeString text;
			if( owner != nullptr )
				text.format( L"%ls has taken %ls", owner->getPlayerDisplayName().str(), it->name.str() );
			else
				text.format( L"%ls is neutral again", it->name.str() );
			TheInGameUI->messageNoFormat( text );

			const Object *hospital = TheGameLogic->findObjectByID( it->hospitalID );
			if( hospital != nullptr )
				TheRadar->createEvent( hospital->getPosition(), RADAR_EVENT_INFORMATION );
		}
	}
}

//-------------------------------------------------------------------------------------------------
void ConquestManager::checkVictory()
{
	if( m_victoryDeclared )
		return;

	const Int needed = getVictoryCount();
	for( Int i = 0; i < ThePlayerList->getPlayerCount(); ++i )
	{
		Player *player = ThePlayerList->getNthPlayer( i );
		if( player == nullptr || !isContender( player ) || !player->isPlayerActive() )
			continue;

		if( getTerritoryCountForAlliance( player ) < needed )
			continue;

		m_victoryDeclared = TRUE;

		UnicodeString text;
		text.format( L"%ls holds %d of %d countries and has conquered the world", player->getPlayerDisplayName().str(),
			getTerritoryCountForAlliance( player ), getTerritoryCount() );
		TheInGameUI->messageNoFormat( text );

		if( TheRecorder && TheRecorder->isMultiplayer() )
		{
			// Skirmish and network games: the stock multiplayer scripts watch TheVictoryConditions
			// and put up the victory and defeat screens.
			TheVictoryConditions->declareAllianceVictory( player );
		}
		else
		{
			// A map started on its own has no such scripts: end it the way a map script would.
			const Player *localPlayer = ThePlayerList->getLocalPlayer();
			const Bool localWins = localPlayer == player || areAllies( localPlayer, player );
			ScriptAction *action = newInstance( ScriptAction )( localWins ? ScriptAction::VICTORY : ScriptAction::DEFEAT );
			TheScriptActions->executeAction( action );
			deleteInstance( action );
		}
		return;
	}
}

//-------------------------------------------------------------------------------------------------
void ConquestManager::update()
{
	if( !m_active )
		return;

	if( !m_started )
	{
		m_started = TRUE;
		start();
		return;
	}

	if( TheGameLogic->getFrame() % CONQUEST_CHECK_INTERVAL != 0 )
		return;

	checkOwnership( TRUE );
	checkVictory();
}
