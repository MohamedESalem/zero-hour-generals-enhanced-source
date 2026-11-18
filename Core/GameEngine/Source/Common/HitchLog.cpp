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

// FILE: HitchLog.cpp /////////////////////////////////////////////////////////////////////////////
// TheSuperHackers @feature mohamed 17/11/2026 See HitchLog.h.
///////////////////////////////////////////////////////////////////////////////////////////////////

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "Common/HitchLog.h"

#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace
{
	enum { MAX_SCOPES = 48, MAX_LINES = 2000 };

	struct ScopeTime
	{
		const char *name;
		double ms;
	};

	ScopeTime g_scopes[MAX_SCOPES];
	int g_scopeCount = 0;
	int g_linesWritten = 0;
	int g_mode = -1;					// -1 = not decided, 0 = off, 1 = on
	double g_thresholdMs = 100.0;
	double g_secondsPerTick = 0.0;
}

double HitchLog::now()
{
	__int64 counter = 0;
	if( g_secondsPerTick == 0.0 )
	{
		__int64 freq = 0;
		QueryPerformanceFrequency( (LARGE_INTEGER *)&freq );
		g_secondsPerTick = ( freq > 0 ) ? 1.0 / (double)freq : 1.0;
	}
	QueryPerformanceCounter( (LARGE_INTEGER *)&counter );
	return (double)counter * g_secondsPerTick * 1000.0;
}

void HitchLog::add( const char *name, double ms )
{
	// Scope names are string literals: the same name is the same pointer within a build, but compare the text
	// anyway so two translation units that merge or split literals still end up in one slot.
	int i;
	for( i = 0; i < g_scopeCount; i++ )
	{
		if( g_scopes[i].name == name || strcmp( g_scopes[i].name, name ) == 0 )
		{
			g_scopes[i].ms += ms;
			return;
		}
	}
	if( g_scopeCount < MAX_SCOPES )
	{
		g_scopes[g_scopeCount].name = name;
		g_scopes[g_scopeCount].ms = ms;
		g_scopeCount++;
	}
}

void HitchLog::endUpdate( unsigned int frame, double totalMs )
{
	if( g_mode < 0 )
	{
		g_mode = ( getenv( "GENZH_NO_HITCHLOG" ) != nullptr ) ? 0 : 1;
		const char *threshold = getenv( "GENZH_HITCH_MS" );
		if( threshold != nullptr && atof( threshold ) > 0.0 )
			g_thresholdMs = atof( threshold );
	}

	if( g_mode == 1 && totalMs >= g_thresholdMs && g_linesWritten < MAX_LINES )
	{
		FILE *fp = fopen( "Hitch.log", "a" );
		if( fp )
		{
			fprintf( fp, "frame %u  total %.1f ms |", frame, totalMs );
			int i;
			for( i = 0; i < g_scopeCount; i++ )
			{
				if( g_scopes[i].ms >= 1.0 )
					fprintf( fp, " %s %.1f |", g_scopes[i].name, g_scopes[i].ms );
			}
			fprintf( fp, "\n" );
			fclose( fp );
			g_linesWritten++;
		}
	}

	g_scopeCount = 0;
}
