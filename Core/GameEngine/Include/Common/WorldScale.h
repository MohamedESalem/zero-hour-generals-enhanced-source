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

// FILE: WorldScale.h /////////////////////////////////////////////////////////////////////////////
// TheSuperHackers @feature mohamed 14/11/2026
// Desc:   "True miniature" world scaling for ultra-large maps. Every scale is applied ONCE, at INI
//         parse time, to the numbers the game reads from its data files. No simulation code is
//         changed, so a scale of exactly 1.0 (the default) is bit-for-bit retail behaviour.
//
//   -collisionScale <f>  Geometry radii/height of every object + a handful of footprint-bound
//                        fields (build exits, fences, dynamic geometry, production exit points).
//   -speedScale     <f>  INI::parseVelocityReal / INI::parseAccelerationReal (locomotors,
//                        projectiles, gravity...). Angular rates are deliberately NOT scaled.
//   -rangeScale     <f>  Weapon / vision / ability distance fields in the table in WorldScale.cpp.
//   -worldScale     <f>  Shortcut: fills whichever of the three was not given explicitly.
//   -worldScaleLog       Write WorldScale.log with the effective values and sample template values.
//
// Values <= 0 (or unparsable) are ignored. Flag order on the command line does not matter.
///////////////////////////////////////////////////////////////////////////////////////////////////

#pragma once

#include "Common/INI.h"

class WorldScale
{
public:

	/// Turn the raw command line values in TheGlobalData into the effective scales (never <= 0).
	/// Idempotent. Called from CommandLine::parseCommandLineForStartup, i.e. before any INI.
	static void resolveFromGlobalData();

	/// Effective scales. 1.0 until resolveFromGlobalData() has seen something else.
	static Real getCollisionScale() { return s_collision; }
	static Real getSpeedScale()     { return s_speed; }
	static Real getRangeScale()     { return s_range; }

	// ---- INI parse context ------------------------------------------------------------------
	// INI::load tells us which top-level block ("Weapon", "Object", ...) is being read, the field
	// dispatcher tells us which field is being parsed, and the module factory tells us which
	// module ("AutoHealBehavior", ...) owns the fields of a nested module block. The table uses
	// this to tell apart field names that mean different things in different places.
	static void setBlock( const char *blockName );
	static void setField( const char *fieldName );
	/// Enter / leave a nested module block (balanced by the module factory; reset by setBlock).
	static void pushModule( const char *moduleName );
	static void popModule();

	/// For INI::parseVelocityReal / parseAccelerationReal: the factor for the field being parsed
	/// (1.0 for purely cosmetic fields that must not follow gameplay speed).
	static Real getSpeedScaleForCurrentField();

	/// Called by INI::initFromINIMulti right after a field was parsed. Multiplies the freshly
	/// stored value if (current context, field) is in the collision / range table.
	static void postParseField( const char *fieldName, INIFieldParseProc parse, void *store );

	// ---- diagnostics ------------------------------------------------------------------------
	/// Logs the effective scales (WorldScale.log when -worldScaleLog or any scale != 1.0).
	static void logEffective();
	/// Logs a few template values (Overlord geometry / vision / locomotor / weapon) so the effect
	/// of the scales can be checked in a release build. Call after all object INIs are loaded.
	static void logSamples();

private:

	static Real s_collision;
	static Real s_speed;
	static Real s_range;
};
