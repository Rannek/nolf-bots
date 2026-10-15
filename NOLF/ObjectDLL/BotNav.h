// ----------------------------------------------------------------------- //
//
// MODULE  : BotNav.h
//
// PURPOSE : Waypoint graph the bots plan routes on
//
//			 Built from the measured map (CBotWorld): every floor cell
//			 belongs to the nearest waypoint, and waypoints are linked where
//			 their cells touch, so every link is a move a player can really
//			 make.  Long moves (zipcords, ladders, running jumps) link the
//			 waypoints at their ends.
//
// ----------------------------------------------------------------------- //

#ifndef __BOT_NAV_H__
#define __BOT_NAV_H__

#include "ltbasedefs.h"
#include <vector>
#include <map>
#include <set>

class CBotWorld;

#define BOTNAV_MAX_LINKS		24
#define BOTNAV_INVALID			(-1)

// Link flags
#define BOTLINK_JUMP			0x01	// Jump up onto a ledge / over a gap
#define BOTLINK_ZIP				0x02	// Ride a zipcord (BotNavLink::nWorldLink is the hook)
#define BOTLINK_DROP			0x04	// Drop off a ledge (one way)
#define BOTLINK_LADDER			0x08	// Climb a ladder
#define BOTLINK_GAP				0x10	// Running jump over a gap

struct BotNavLink
{
	int		nTo;
	uint8	nFlags;
	int		nWorldLink;		// CBotWorld long move for ZIP / LADDER / GAP links, else -1
};

// Node flags
#define BOTNODE_WATER			0x01	// Swimming
#define BOTNODE_LADDER			0x02	// On a ladder
#define BOTNODE_NOAIR			0x04	// Under water with no air above
#define BOTNODE_HIGH			0x08	// Perch: only reachable by zipcord / ladder / climbing, or well above its surroundings

// How a particular bot likes its routes
struct BotRouteCost
{
	LTFLOAT		fWaterMul;		// Cost multiplier for swimming
	LTFLOAT		fZipMul;		// Cost multiplier for zipcords
	LTFLOAT		fNoAirMul;		// Cost multiplier for flooded stretches (no SCUBA = high)
	const std::set<long long>* pAvoid;	// Links (from<<32|to) to stay off (got stuck there)
	const std::vector<LTFLOAT>* pDanger;	// Learned per-waypoint danger for our team (may be NULL)
	LTFLOAT		fDangerMul;		// How much this bot cares (cost x (1 + mul*danger))
	const std::vector<LTFLOAT>* pLane;	// Per waypoint cost multiplier keeping us on our lane (may be NULL)
};

struct BotNavNode
{
	LTVector	vPos;		// Position of the player's center (not the floor)
	uint8		nFlags;
	int			nSpan;		// Floor cell in CBotWorld
	int			nLinks;
	BotNavLink	aLinks[BOTNAV_MAX_LINKS];
};

class CBotNav
{
	public :

		CBotNav();

		void	Clear();

		// Make the graph from a finished map scan
		void	BuildFromWorld(const CBotWorld& world);

		// Bots\<WorldName>.nav keeps the learned danger (and the graph, for
		// looking at with scripts).  Load after BuildFromWorld.
		LTBOOL	LoadDanger(const char* szWorld);
		LTBOOL	Save(const char* szWorld);

		int		AddNode(const LTVector& vPos, int nSpan, uint8 nFlags);
		void	AddLink(int nFrom, int nTo, uint8 nFlags, int nWorldLink = -1);
		LTBOOL	HasLink(int nFrom, int nTo) const;
		const BotNavLink* GetLink(int nFrom, int nTo) const;
		int		FindFrontier(int nStart, const LTVector& vTarget) const;	// Reachable node closest to vTarget
		void	GetNodesNear(const LTVector& vPos, LTFLOAT fRadius, LTFLOAT fMaxDY, std::vector<int>& nodes) const;

		// Learned danger: where players of a team (0 = deathmatch) keep dying.
		// Persisted in the .nav file; the pathfinder routes around it.
		void	AddDanger(int nTeam, const LTVector& vPos, LTFLOAT fAmount);
		void	DecayDanger(LTFLOAT fFactor);
		const std::vector<LTFLOAT>* GetDanger(int nTeam);
		int		CountDangerNodes(int nTeam, LTFLOAT fMin) const;
		int		GetChanges() const	{ return m_nChanges; }
		void	ClearChanges()		{ m_nChanges = 0; }

		int		GetNumNodes() const { return (int)m_Nodes.size(); }
		const BotNavNode& GetNode(int n) const { return m_Nodes[n]; }
		// Can players get here from where they spawn?  (Some floors are only
		// known because an item lies there, e.g. on a ledge nobody reaches.)
		LTBOOL	IsPlayable(int n) const { return n >= 0 && n < (int)m_Playable.size() && m_Playable[n]; }
		int		GetIncoming(int n) const { return n >= 0 && n < (int)m_Incoming.size() ? m_Incoming[n] : 0; }

		int		FindNearest(const LTVector& vPos, LTFLOAT fMaxDist, LTBOOL bVisible) const;
		int		GetRandomNode() const;

		// Nearest waypoint we can walk to that vThreat (an enemy's eye) can't see
		int		FindCover(const LTVector& vPos, const LTVector& vThreat, LTFLOAT fRadius) const;

		// Trouble spots learned this game (see CBotWorld::MarkHazard)
		void	MarkHazard(int nNode, LTFLOAT fUntil);
		LTBOOL	IsHazard(int nNode) const;

		// A* from node to node.  Path excludes nFrom, includes nTo.
		LTBOOL	FindPath(int nFrom, int nTo, std::vector<int>& path, const BotRouteCost* pCost=LTNULL) const;
		LTFLOAT	LinkCost(int nFrom, const BotNavLink& link, const BotRouteCost* pCost) const;
		LTFLOAT	PathCost(int nFrom, int nTo, const BotRouteCost* pCost=LTNULL) const;	// Travel cost, <0 = no way
		static long long LinkKey(int nFrom, int nTo) { return ((long long)nFrom << 32) | (unsigned int)nTo; }

		static LTBOOL IsVisible(const LTVector& vFrom, const LTVector& vTo);
		static LTBOOL IsInLiquid(const LTVector& vPos);
		static LTBOOL IsInLadder(const LTVector& vPos);
		static LTBOOL FindZipHook(const LTVector& vNear, LTFLOAT fMaxDist, LTVector& vHook);

		// Stats for the log
		int		CountNodes(uint8 nFlag) const;
		int		CountLinks(uint8 nFlag) const;

	private :

		// Spatial hash so lookups stay fast with thousands of nodes
		typedef std::map<long long, std::vector<int> > CellMap;
		static long long CellKey(int x, int y, int z);
		void	AddToCells(int nNode);

		const CBotWorld*		m_pWorld;
		std::vector<BotNavNode>	m_Nodes;
		std::vector<int>		m_Incoming;		// Links into each node
		std::vector<char>		m_Playable;
		std::vector<float>		m_HazardUntil;	// Per node, game time
		CellMap					m_Cells;
		std::vector<LTFLOAT>	m_aDanger[3];	// Per team (index = team id, 0 = deathmatch)
		int						m_nChanges;		// Danger updates since the last save
};

#endif // __BOT_NAV_H__
