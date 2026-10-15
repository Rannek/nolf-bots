// ----------------------------------------------------------------------- //
//
// MODULE  : BotMapInfo.h
//
// PURPOSE : The map's structure as the bots understand it, worked out
//			 from the waypoint graph:
//
//			 - areas: rooms, yards, walkways, rooftops, bodies of water
//			 - how the areas connect (and how wide the openings are)
//			 - routes: for each team, the genuinely different ways from its
//			   base to what it attacks (main road, flank, high route by
//			   zipcord, swim route), each with its character
//			 - chokepoints: narrow openings those routes squeeze through
//
//			 Attackers are spread over the routes (lanes), defenders hold the
//			 chokepoints into their base.
//
// ----------------------------------------------------------------------- //

#ifndef __BOT_MAP_INFO_H__
#define __BOT_MAP_INFO_H__

#include "ltbasedefs.h"
#include <vector>
#include <stdio.h>

class CBotNav;
class CBotWorld;

enum BotAreaKind
{
	eAreaGround = 0,
	eAreaHigh,			// Perch / rooftop / up by zipcord or ladder
	eAreaWater,			// Swimming
	eAreaFlooded,		// Under water, no air
};

struct BotArea
{
	LTVector	vCenter;
	LTFLOAT		fRadius;
	int			nKind;
	std::vector<int> nodes;
	std::vector<int> neighbours;	// Areas we can move into
	std::vector<int> widths;		// ...through how many waypoint links
};

enum BotRouteKind
{
	eRouteMain = 0,
	eRouteFlank,
	eRouteHigh,			// Uses zipcords / perches
	eRouteWater,		// Mostly swimming
};

struct BotRoute
{
	int			nKind;
	LTFLOAT		fLength;			// Path length (units)
	std::vector<int> nodes;			// Waypoints base -> target
	std::vector<int> areas;			// Areas it passes, in order
	std::vector<int> chokes;		// Areas on it that are chokepoints
	std::vector<LTFLOAT> laneCost;	// Per waypoint cost multiplier that keeps a bot on this lane
	int			nUsers;				// Bots assigned to it
};

class CBotMapInfo
{
	public :

		CBotMapInfo();
		void	Clear();

		// After the waypoint graph is built
		void	Build(const CBotNav& nav, const CBotWorld& world);
		LTBOOL	IsBuilt() const { return !m_Areas.empty(); }

		int		GetAreaOf(int nNode) const { return (nNode >= 0 && nNode < (int)m_NodeArea.size()) ? m_NodeArea[nNode] : -1; }
		int		GetNumAreas() const { return (int)m_Areas.size(); }
		const BotArea& GetArea(int n) const { return m_Areas[n]; }

		// Team routes from vFrom (its base) to vTo (what it attacks).  Cheap
		// enough to recompute when the target changes.
		void	ComputeRoutes(int nTeam, const CBotNav& nav, const LTVector& vFrom, const LTVector& vTo);
		int		GetNumRoutes(int nTeam) const { return (nTeam >= 0 && nTeam < 3) ? (int)m_Routes[nTeam].size() : 0; }
		BotRoute& GetRoute(int nTeam, int n) { return m_Routes[nTeam][n]; }
		const LTVector& GetRouteTarget(int nTeam) const { return m_avRouteTo[nTeam]; }

		// Pick a lane for a bot by its taste (diver / zipper / walker)
		int		PickRoute(int nTeam, LTBOOL bDiver, LTBOOL bZipper, LTBOOL bHasScuba);

		// A chokepoint on the way into this team's base (from the enemy's routes)
		LTBOOL	GetDefenseChoke(int nTeam, const LTVector& vGuard, LTVector& vChoke);

		void	Describe(FILE* fp, const CBotNav& nav);
		void	DumpFile(const char* szWorld, const CBotNav& nav);

	private :

		std::vector<BotArea>	m_Areas;
		std::vector<int>		m_NodeArea;
		std::vector<BotRoute>	m_Routes[3];
		LTVector				m_avRouteTo[3];
};

#endif // __BOT_MAP_INFO_H__
