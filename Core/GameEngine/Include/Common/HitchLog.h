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

// FILE: HitchLog.h ///////////////////////////////////////////////////////////////////////////////
// TheSuperHackers @feature mohamed 17/11/2026
// Desc:   Finds out WHERE a freeze comes from. Named scopes add up the time they take within one
//         GameEngine::update(); when the whole update took longer than 100 ms (HITCH_THRESHOLD_MS,
//         override with the environment variable GENZH_HITCH_MS), one line is appended to Hitch.log:
//
//           frame 4821  total 2314.5 ms | GameLogic::UPDATE 2290.1 | calculateZones 2281.7 | ...
//
//         It only reads the clock and writes a text file, never touches game state, so it cannot change
//         the simulation or the CRC. GENZH_NO_HITCHLOG turns it off. The file stops growing after 2000 lines.
///////////////////////////////////////////////////////////////////////////////////////////////////

#pragma once

class HitchLog
{
public:

	/// RAII timer: adds the time between construction and destruction to the scope called "name"
	/// (a string literal) for the update in progress.
	class Scope
	{
	public:
		Scope( const char *name ) : m_name( name ), m_start( HitchLog::now() ) {}
		~Scope() { HitchLog::add( m_name, HitchLog::now() - m_start ); }
	private:
		const char *m_name;
		double m_start;
	};

	/// Milliseconds on a monotonic clock.
	static double now();

	/// Add elapsed milliseconds to a named scope of the current update.
	static void add( const char *name, double ms );

	/// Call once at the end of every update; writes the log line when it was a hitch, then clears the scopes.
	static void endUpdate( unsigned int frame, double totalMs );
};
