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

// FILE: ZoneMerge.h //////////////////////////////////////////////////////////////////////////////
// TheSuperHackers @performance mohamed 17/11/2026
// Desc:   Near-linear replacements for the pathfinder's resolveZones(), which rewrote the WHOLE
//         zone equivalence table on every merge (O(zones) per merge, O(zones^2) per recompute) and
//         froze the game for seconds every time a structure was placed, finished or destroyed on a
//         big map. The results are the same as the old code; tools/_test_zonemerge.cpp checks that
//         against the old algorithm on random data.
//
//         Plain C++ only (no engine headers) so the standalone test can include it.
///////////////////////////////////////////////////////////////////////////////////////////////////

#pragma once

#include <vector>

/// The old operation, kept for the self-test and for the ZONE_VERIFY comparison: every entry equal to
/// the zone of src or of target becomes arr[min(thoseTwo)].
template<class T>
inline void zoneMergeReference( int srcZone, int targetZone, T *arr, int size )
{
	srcZone = (int)arr[srcZone];
	targetZone = (int)arr[targetZone];
	T finalZone = ( targetZone < srcZone ) ? arr[targetZone] : arr[srcZone];
	for( int i = 0; i < size; i++ )
	{
		T ze = arr[i];
		if( (int)ze == targetZone || (int)ze == srcZone )
			arr[i] = finalZone;
	}
}

//-------------------------------------------------------------------------------------------------
// Identity-initialised table (arr[i] == i at the start): every class is labelled by its smallest
// member, so the old table is a plain union-find with "smaller index wins". Reads must go through
// zoneFind() until zoneFlatten() has been run.
//-------------------------------------------------------------------------------------------------
template<class T>
inline T zoneFind( T *p, T x )
{
	T r = x;
	while( p[r] != r )
		r = p[r];
	while( p[x] != r )		// path compression
	{
		T next = p[x];
		p[x] = r;
		x = next;
	}
	return r;
}

/// Same effect as zoneMergeReference() on an identity-initialised table.
template<class T>
inline void zoneMergeIdentity( int srcZone, int targetZone, T *p )
{
	T a = zoneFind( p, (T)srcZone );
	T b = zoneFind( p, (T)targetZone );
	if( a < b )
		p[b] = a;
	else if( b < a )
		p[a] = b;
}

/// Turn the union-find forest back into the flat table the rest of the pathfinder reads.
template<class T>
inline void zoneFlatten( T *p, int size )
{
	for( int i = 0; i < size; i++ )
		p[i] = zoneFind( p, (T)i );
}

//-------------------------------------------------------------------------------------------------
// Arbitrary table (flattenZones works on tables that are NOT identity-initialised). Classes are the
// groups of entries that currently hold the same value; a merge joins the classes of the two values
// and of the value stored at index min(A,B), and labels the result with that last value.
//-------------------------------------------------------------------------------------------------
template<class T>
class ZoneValueMerger
{
public:

	ZoneValueMerger() : m_n( 0 ) {}

	/// arr has n entries, each a value < n.
	void init( const T *arr, int n )
	{
		m_n = n;
		m_node.resize( n );
		m_parent.resize( n );
		m_size.resize( n );
		m_label.resize( n );
		m_nodeOfLabel.assign( n, -1 );
		int i;
		for( i = 0; i < n; i++ )
		{
			m_node[i] = (int)arr[i];
			m_parent[i] = i;
			m_size[i] = 1;
			m_label[i] = i;
		}
		for( i = 0; i < n; i++ )
			m_nodeOfLabel[ m_node[i] ] = m_node[i];
	}

	/// The value arr[i] would hold after all merges so far.
	T value( int i )
	{
		return (T)m_label[ findNode( m_node[i] ) ];
	}

	/// Same effect as zoneMergeReference( a, b, arr, n ).
	void merge( int a, int b )
	{
		int A = (int)value( a );
		int B = (int)value( b );
		int C = (int)value( A < B ? A : B );
		int root = findNode( m_nodeOfLabel[A] );
		root = join( root, m_nodeOfLabel[B] );
		root = join( root, m_nodeOfLabel[C] );
		m_label[root] = C;
		m_nodeOfLabel[C] = root;
		if( A != C )
			m_nodeOfLabel[A] = -1;
		if( B != C )
			m_nodeOfLabel[B] = -1;
	}

	void store( T *arr )
	{
		for( int i = 0; i < m_n; i++ )
			arr[i] = value( i );
	}

private:

	int findNode( int x )
	{
		int r = x;
		while( m_parent[r] != r )
			r = m_parent[r];
		while( m_parent[x] != r )
		{
			int next = m_parent[x];
			m_parent[x] = r;
			x = next;
		}
		return r;
	}

	int join( int x, int y )
	{
		x = findNode( x );
		y = findNode( y );
		if( x == y )
			return x;
		if( m_size[x] < m_size[y] )
		{
			int t = x;
			x = y;
			y = t;
		}
		m_parent[y] = x;
		m_size[x] += m_size[y];
		return x;
	}

	int m_n;
	std::vector<int> m_node;					///< entry -> the node of the value it started with
	std::vector<int> m_parent;
	std::vector<int> m_size;
	std::vector<int> m_label;					///< per root: the value its entries currently hold
	std::vector<int> m_nodeOfLabel;		///< value -> a node of the class that holds it (-1 = nobody does)
};
