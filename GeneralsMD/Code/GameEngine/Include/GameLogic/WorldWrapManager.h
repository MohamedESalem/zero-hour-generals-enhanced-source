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

// FILE: WorldWrapManager.h ///////////////////////////////////////////////////////////////////////
// TheSuperHackers @feature mohamed 18/11/2026 Crossing the 180th meridian.
//
// The Equal Earth world map ends at the 180th meridian on both sides: eastern Japan is near the right
// edge and the western United States near the left edge, so the Pacific is split in two. A map made by
// tools/decorate_map.py (stage "wrap") carries a "WorldWrap" chunk with the x position of that meridian
// on both sides for every row of the map. With it:
//
//   smart orders a ship or aircraft that a player sends somewhere that is nearer through the meridian
//                than across the map first goes to the meridian
//   edge jump    ...and there reappears at the same latitude on the other side, with everything it carries,
//                and carries on to where it was sent
//
// Only such a unit is ever moved. Aircraft that come in from or leave over the edge of the map (reinforcement
//	and supply planes) are created and removed beyond it and are left alone.
//
// Ground units are never moved: only vehicles whose locomotor is made for water or air.
// A map without the chunk leaves the manager inactive.
///////////////////////////////////////////////////////////////////////////////////////////////////

#pragma once

#include "Common/GameCommon.h"
#include "Common/GameType.h"

#include <vector>

class DataChunkInput;
class Object;
struct DataChunkInfo;

//-------------------------------------------------------------------------------------------------
class WorldWrapManager
{
public:

	WorldWrapManager();
	~WorldWrapManager();

	void reset();																			///< forget the map
	void newMap( Bool loadingSaveGame );							///< read the chunk of the map being started
	void update();																		///< once per logic frame

	Bool isActive() const { return m_active; }

	/// x of the meridian on the west and east side at the row of world position y (already moved inwards
	/// by the trigger distance). False when the map does not wrap there (the poles).
	Bool getEdges( Real y, Real *west, Real *east ) const;

private:

	struct Route
	{
		ObjectID	id;
		Coord3D		finalTarget;										///< where the player sent the unit
		Coord3D		jumpPoint;											///< the point at the meridian it is heading for
		Bool			viaWest;												///< true: it leaves over the west side and lands on the east side
	};

	static Bool parseWorldWrapChunk( DataChunkInput &file, DataChunkInfo *info, void *userData );

	void checkObject( Object *obj );
	void jump( Object *obj, Real newX );
	Int findRoute( ObjectID id ) const;
	void removeRoute( Int index );

	Bool									m_active;
	Int										m_width;						///< size of the map's height grid
	Int										m_height;
	Int										m_border;
	Real									m_inset;						///< world units the trigger lines lie inside the meridian
	std::vector<Real>			m_west;							///< per grid row: world x of the meridian, west side
	std::vector<Real>			m_east;							///< ... and east side
	std::vector<Route>		m_routes;
};

extern WorldWrapManager *TheWorldWrapManager;
