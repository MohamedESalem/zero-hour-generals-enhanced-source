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

// FILE: ConquestManager.h ////////////////////////////////////////////////////////////////////////
// TheSuperHackers @feature mohamed 15/11/2026 Global conquest mode.
//
// A map made by tools/decorate_map.py (stage "conquest") carries a "ConquestData" chunk: a table of
// territories and a grid with one territory number per height-map sample. Each territory has a
// hospital in its capital, an ordinary capturable but indestructible map object found by name.
// Whoever controls the hospital owns the territory, and owning it means:
//
//   vision    the owner and his allies permanently see the whole territory; everything else is
//             fogged (explored, but units and new buildings there stay hidden)
//   building  structures can only be placed inside territories of the player or an ally
//             (BuildAssistant::isLocationLegalToBuild)
//   victory   an alliance holding the map's share of all territories (75%) wins
//
// Ownership is polled from the hospitals, so every way of changing a building's owner counts. A map
// without the chunk leaves the manager inactive and every query returns the stock answer.
//
// TheSuperHackers @feature mohamed 18/11/2026 National armies. A map made with the "armies" stage has a
// defender side (PlyrDefenders, a civilian faction, so skirmish games keep it). Its units stand guard in every
// country and the Tech Artillery Platform and Tech Reinforcement Pad in each capital belong to it. The side is
// hostile to every player. Whoever takes a country's hospital (or starts in the country) is handed the army
// and the capital tech of that country: the objects named Army_<key>_<n>, CapArty_<key> and CapPad_<key>.
///////////////////////////////////////////////////////////////////////////////////////////////////

#pragma once

#include "Common/AsciiString.h"
#include "Common/GameCommon.h"
#include "Common/GameType.h"
#include "Common/UnicodeString.h"

#include <vector>

class DataChunkInput;
class Object;
class Player;
struct DataChunkInfo;

//-------------------------------------------------------------------------------------------------
class ConquestManager
{
public:

	struct Territory
	{
		AsciiString				key;							///< short code, e.g. "EGY"
		UnicodeString			name;							///< display name
		AsciiString				hospitalName;			///< script name of the capital's hospital
		Coord3D						labelPos;					///< where the name is drawn
		Real							labelRadius;			///< open land around labelPos, world units
		Int								landCells;
		ObjectID					hospitalID;
		Int								ownerIndex;				///< player index of the owner, -1 = nobody
		PlayerMaskType		viewerMask;				///< players currently holding lookers on 'cells'
		std::vector<Int>	cells;						///< partition cells the territory touches
	};

	ConquestManager();
	~ConquestManager();

	void reset();																			///< forget the map
	void newMap( Bool loadingSaveGame );							///< read the chunk of the map being started
	void update();																		///< once per logic frame

	Bool isActive() const { return m_active; }
	Int getTerritoryCount() const { return (Int)m_territories.size(); }
	const Territory *getTerritory( Int index ) const;
	Int getTerritoryIndexAt( const Coord3D *pos ) const;			///< -1 = open sea or off the map
	Player *getTerritoryOwner( Int index ) const;							///< null = nobody
	Bool canPlayerBuildAt( const Player *player, const Coord3D *pos ) const;
	static Bool isDefenderPlayer( const Player *player );					///< the map's defender side
	Int getVictoryCount() const;															///< territories an alliance needs to win
	Int getTerritoryCountForAlliance( const Player *player ) const;	///< held by the player and his allies

private:

	static Bool parseConquestDataChunk( DataChunkInput &file, DataChunkInfo *info, void *userData );

	void buildCells();
	void start();
	Player *findOwner( const Territory &territory ) const;
	PlayerMaskType viewersFor( const Player *owner ) const;
	void applyViewers( Territory &territory, PlayerMaskType newMask );
	void checkOwnership( Bool announce );
	void checkVictory();
	Player *findDefenders() const;
	void startDefenders();														///< hostile to every player, units on guard
	void giveForcesTo( const Territory &territory, Player *player );	///< army and capital tech of a country

	Bool											m_active;
	Bool											m_started;					///< hospitals resolved and start state applied
	Bool											m_fromSave;					///< the map was started to receive a saved game
	Bool											m_victoryDeclared;
	Int												m_width;						///< grid size in samples
	Int												m_height;
	Int												m_border;
	Int												m_victoryPercent;
	std::vector<UnsignedByte>	m_grid;							///< territory number per sample, 0 = nobody's
	std::vector<Territory>		m_territories;
};

extern ConquestManager *TheConquestManager;
