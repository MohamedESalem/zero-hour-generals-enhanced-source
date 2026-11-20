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
// FILE: AircraftRange.h //////////////////////////////////////////////////////////////////////////
// TheSuperHackers @feature mohamed 20/11/2026 Realistic combat range for aircraft on world maps.
//
// Design: docs/plans/aircraft-range-design.md in the outer repository. In short:
//
//   * Data/INI/AircraftRange.ini (shipped only to the 64-bit game in its own .big) lists a combat radius in km for
//     every aircraft template and a range in km for every aircraft special power. Templates not listed are free.
//   * Every aircraft has a home: the airfield or helipad it last refuelled at (the one that built it at first),
//     else the nearest airfield or helipad of its owner. Two rings around the home are drawn when it is selected:
//     white = radius R (it can fly there and back), red = 2R (one way only).
//   * Player orders are judged by the target's distance from home: <= R normal; <= 2R one-way (a suicide mission,
//     unless the target is another friendly airfield or helipad: then it rebases there); beyond 2R refused.
//     AI and script orders are always accepted and never one-way.
//   * A normal aircraft that ends up beyond R + LeashSlack (a chase) says "low fuel" and flies home.
//   * A one-way aircraft crashes when its job is done (idle, out of ammo, heading home, ordered back) or after
//     OneWaySeconds beyond its white ring. An aircraft with no airfield or helipad left crashes after the same time.
//   * Jets refuel (re-home) when they land; helicopters and drones when idle within RefuelDistance of the edge of
//     a friendly airfield or helipad.
//   * Aircraft special powers (OCLSpecialPower) refuse a target beyond their range without spending the power,
//     and their planes start over the calling building instead of at the far map edge.
//
// Only active on maps that wrap around the world (WorldWrapManager active), when the data file has entries and
// -noAircraftRange was not given. Its state is saved in its own save-game block; older saves simply lack it.
///////////////////////////////////////////////////////////////////////////////////////////////////

#pragma once

#include "Common/AsciiString.h"
#include "Common/GameCommon.h"
#include "Common/GameType.h"
#include "Common/Snapshot.h"

#include <map>
#include <vector>

class Object;
class Player;
class SpecialPowerTemplate;
class ThingTemplate;
struct AICommandParms;

//-------------------------------------------------------------------------------------------------
class AircraftRangeManager : public Snapshot
{
public:

	AircraftRangeManager();
	~AircraftRangeManager();

	void reset();																			///< forget the aircraft of the game being left
	void newMap();																		///< decide whether the rules apply on this map
	void update();																		///< once per logic frame

	Bool isActive() const { return m_active; }

	/// Combat radius of an aircraft in world units; 0 = not limited.
	Real getRadius( const Object *obj ) const;

	/// Range of a special power in world units; 0 = not limited.
	Real getPowerRange( const SpecialPowerTemplate *power ) const;

	/// Where the rings of this aircraft are drawn. False if it has no home.
	Bool getHomePosition( const Object *obj, Coord3D *pos ) const;

	/// Called first thing by AIUpdateInterface::aiDoCommand (and JetAIUpdate's). False = drop the command.
	Bool filterCommand( Object *obj, const AICommandParms *parms );

	/// Called by OCLSpecialPower before anything is spent. False = refuse (the power stays ready).
	Bool allowSpecialPower( const Object *source, const SpecialPowerTemplate *power, const Coord3D *target );

	/// Ground distance between two points, the short way round when the map wraps.
	Real distance( const Coord3D *a, const Coord3D *b ) const;

protected:

	virtual void crc( Xfer *xfer ) override;
	virtual void xfer( Xfer *xfer ) override;
	virtual void loadPostProcess() override;

private:

	enum Mode
	{
		MODE_NORMAL = 0,		///< leashed to its white ring
		MODE_ONE_WAY,				///< suicide mission: crashes when the job is done or the timer runs out
		MODE_REBASE,				///< flying to another friendly airfield / helipad within its red ring
		MODE_STRANDED				///< no airfield or helipad left: crashes when the timer runs out
	};

	struct Craft
	{
		ObjectID		home;								///< INVALID_ID = none
		Coord3D			homePos;
		Int					mode;
		ObjectID		rebaseTarget;
		UnsignedInt	timerStart;					///< frame the one-way / stranded clock started; 0 = not running
		Bool				returning;					///< we sent it home for fuel and it has not yet come back inside its ring
		Bool				virtualHome;				///< no airfield / helipad existed when first seen (a map's defender army): home is its starting point
		Bool				doomed;							///< a one-way aircraft ordered back: it crashes at the next check
		Coord3D			lastOrderPos;
		Bool hasHome() const { return home != INVALID_ID || virtualHome; }				///< target of the last player order judged (so a repeat does not warn twice)
	};

	void loadTable();
	void refreshBases() const;
	Craft &getCraft( Object *obj );
	Bool isBase( const Object *obj, const Player *owner ) const;
	Object *findNearestBase( const Player *owner, const Coord3D *pos ) const;
	Object *baseAtPosition( const Player *owner, const Coord3D *pos ) const;
	void setHome( Craft &craft, const Object *base );
	void checkAircraft( Object *obj, Real radius, UnsignedInt now );
	void sendHome( Object *obj, Craft &craft );
	void crash( Object *obj, const char *why );
	void tell( const Object *obj, const UnicodeString &text );
	void runSelfTest( UnsignedInt now );

	Bool													m_loaded;
	Bool													m_active;
	Real													m_kmPerUnit;
	Real													m_leashSlack;					///< world units
	UnsignedInt										m_oneWayFrames;
	Real													m_refuelDistance;			///< world units
	std::map<AsciiString, Real>		m_aircraftKm;					///< template name -> combat radius, km
	std::map<AsciiString, Real>		m_powerKm;						///< special power name -> range, km
	mutable std::map<const ThingTemplate *, Real>	m_radiusCache;	///< template -> radius in world units (0 = free)
	std::map<ObjectID, Craft>			m_crafts;
	UnsignedInt										m_lastMessageFrame;
	std::vector<UnicodeString>		m_messagesThisFrame;	///< so a group order says each thing once
	mutable Bool									m_basesValid;
	mutable UnsignedInt						m_basesFrame;
	mutable std::vector<ObjectID>	m_bases;							///< every airfield / helipad, gathered once per frame
};

extern AircraftRangeManager *TheAircraftRange;
