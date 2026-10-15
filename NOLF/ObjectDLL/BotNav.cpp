// ----------------------------------------------------------------------- //
//
// MODULE  : BotNav.cpp
//
// PURPOSE : Waypoint graph the bots plan routes on (see BotNav.h)
//
// ----------------------------------------------------------------------- //

#include "stdafx.h"
#include "BotNav.h"
#include "BotWorld.h"
#include "ServerUtilities.h"
#include "ContainerCodes.h"
#include <stdio.h>
#include <direct.h>
#include <algorithm>
#include <queue>

#define BOTNAV_VERSION			8

static const LTFLOAT kCellSize		= 64.0f;	// Spatial hash
static const int	 kWalkSpacing	= 4;		// Cells between waypoints on foot (128u)
static const int	 kSwimSpacing	= 7;		// ...and in open water

CBotNav::CBotNav()
{
	m_pWorld = LTNULL;
	Clear();
}

void CBotNav::Clear()
{
	m_HazardUntil.clear();
	m_Nodes.clear();
	m_Incoming.clear();
	m_Playable.clear();
	m_Cells.clear();
	m_nChanges = 0;
	for (int t = 0; t < 3; t++) m_aDanger[t].clear();
}

// ----------------------------------------------------------------------- //
//	Geometry helpers (world geometry and world models; doors don't count,
//	bots open them)
// ----------------------------------------------------------------------- //

static LTBOOL NavFilterFn(HOBJECT hObj, void *pUserData)
{
	if (IsMainWorld(hObj)) return LTTRUE;
	if (OT_WORLDMODEL != g_pLTServer->GetObjectType(hObj)) return LTFALSE;

	static HCLASS hDoor = g_pLTServer->GetClass("Door");
	if (hDoor && g_pLTServer->IsKindOf(g_pLTServer->GetObjectClass(hObj), hDoor)) return LTFALSE;

	return LTTRUE;
}

LTBOOL CBotNav::IsVisible(const LTVector& vFrom, const LTVector& vTo)
{
	IntersectQuery IQuery;
	IntersectInfo IInfo;

	IQuery.m_From		= vFrom;
	IQuery.m_To			= vTo;
	IQuery.m_Flags		= INTERSECT_OBJECTS | IGNORE_NONSOLID;
	IQuery.m_FilterFn	= NavFilterFn;

	return !g_pLTServer->IntersectSegment(&IQuery, &IInfo);
}

static LTBOOL IsInContainer(const LTVector& vPos, LTBOOL bLiquid, uint16 nWant)
{
	HOBJECT objList[MAX_TRACKED_CONTAINERS];
	LTVector vPoint = vPos;
	uint32 nCount = g_pLTServer->GetPointContainers(&vPoint, objList, MAX_TRACKED_CONTAINERS);

	for (uint32 i = 0; i < nCount; i++)
	{
		uint16 nCode;
		if (!g_pLTServer->GetContainerCode(objList[i], &nCode)) continue;
		if (bLiquid ? IsLiquid((ContainerCode)nCode) : nCode == nWant) return LTTRUE;
	}
	return LTFALSE;
}

LTBOOL CBotNav::IsInLiquid(const LTVector& vPos)	{ return IsInContainer(vPos, LTTRUE, 0); }
LTBOOL CBotNav::IsInLadder(const LTVector& vPos)	{ return IsInContainer(vPos, LTFALSE, CC_LADDER); }

LTBOOL CBotNav::FindZipHook(const LTVector& vNear, LTFLOAT fMaxDist, LTVector& vHook)
{
	static HCLASS hZip = g_pLTServer->GetClass("ZipHook");
	if (!hZip) return LTFALSE;

	LTFLOAT fBest = fMaxDist;
	LTBOOL bFound = LTFALSE;

	for (int nPass = 0; nPass < 2; nPass++)
	{
		HOBJECT hObj = LTNULL;
		while ((hObj = (nPass == 0 ? g_pLTServer->GetNextObject(hObj) : g_pLTServer->GetNextInactiveObject(hObj))) != LTNULL)
		{
			if (!g_pLTServer->IsKindOf(g_pLTServer->GetObjectClass(hObj), hZip)) continue;

			LTVector vPos;
			g_pLTServer->GetObjectPos(hObj, &vPos);
			LTFLOAT fDist = vPos.Dist(vNear);
			if (fDist < fBest)
			{
				fBest = fDist;
				vHook = vPos;
				bFound = LTTRUE;
			}
		}
	}

	return bFound;
}

// ----------------------------------------------------------------------- //
//	Spatial hash
// ----------------------------------------------------------------------- //

long long CBotNav::CellKey(int x, int y, int z)
{
	return ((long long)(x & 0x1FFFFF) << 42) | ((long long)(y & 0x1FFFFF) << 21) | (long long)(z & 0x1FFFFF);
}

static int CellCoord(LTFLOAT f)
{
	return (int)floor(f / kCellSize);
}

void CBotNav::AddToCells(int nNode)
{
	const LTVector& v = m_Nodes[nNode].vPos;
	m_Cells[CellKey(CellCoord(v.x), CellCoord(v.y), CellCoord(v.z))].push_back(nNode);
}

// ----------------------------------------------------------------------- //
//	Graph editing
// ----------------------------------------------------------------------- //

int CBotNav::AddNode(const LTVector& vPos, int nSpan, uint8 nFlags)
{
	BotNavNode node;
	memset(&node, 0, sizeof(node));
	node.vPos	= vPos;
	node.nSpan	= nSpan;
	node.nFlags	= nFlags;
	node.nLinks = 0;
	m_Nodes.push_back(node);

	int n = (int)m_Nodes.size() - 1;
	AddToCells(n);
	return n;
}

const BotNavLink* CBotNav::GetLink(int nFrom, int nTo) const
{
	if (nFrom < 0 || nFrom >= GetNumNodes()) return LTNULL;
	const BotNavNode& node = m_Nodes[nFrom];
	for (int i = 0; i < node.nLinks; i++)
	{
		if (node.aLinks[i].nTo == nTo) return &node.aLinks[i];
	}
	return LTNULL;
}

LTBOOL CBotNav::HasLink(int nFrom, int nTo) const
{
	return GetLink(nFrom, nTo) != LTNULL;
}

// How hard a kind of link is (lower = easier); a waypoint pair keeps the easiest
static int LinkDifficulty(uint8 nFlags)
{
	if (nFlags & BOTLINK_ZIP)		return 5;
	if (nFlags & BOTLINK_GAP)		return 4;
	if (nFlags & BOTLINK_LADDER)	return 3;
	if (nFlags & BOTLINK_JUMP)		return 2;
	if (nFlags & BOTLINK_DROP)		return 1;
	return 0;
}

void CBotNav::AddLink(int nFrom, int nTo, uint8 nFlags, int nWorldLink)
{
	if (nFrom == nTo) return;
	if (nFrom < 0 || nFrom >= GetNumNodes()) return;
	if (nTo < 0 || nTo >= GetNumNodes()) return;

	BotNavNode& node = m_Nodes[nFrom];
	for (int i = 0; i < node.nLinks; i++)
	{
		if (node.aLinks[i].nTo != nTo) continue;
		if (LinkDifficulty(nFlags) < LinkDifficulty(node.aLinks[i].nFlags))
		{
			node.aLinks[i].nFlags = nFlags;
			node.aLinks[i].nWorldLink = nWorldLink;
		}
		return;
	}
	if (node.nLinks >= BOTNAV_MAX_LINKS) return;

	node.aLinks[node.nLinks].nTo		= nTo;
	node.aLinks[node.nLinks].nFlags		= nFlags;
	node.aLinks[node.nLinks].nWorldLink	= nWorldLink;
	node.nLinks++;
}

// ----------------------------------------------------------------------- //
//	Building from the map scan
// ----------------------------------------------------------------------- //

void CBotNav::BuildFromWorld(const CBotWorld& world)
{
	Clear();
	m_pWorld = &world;

	CBotWorld& w = const_cast<CBotWorld&>(world);
	int nSpans = world.GetNumSpans();
	for (int i = 0; i < nSpans; i++) w.GetSpanRW(i).nNode = BOTW_INVALID;
	if (nSpans == 0) return;

	// 1) Waypoints.  First where long moves start and end, where players
	//    spawn and items lie; then spread over the floor, room centers first
	//    (most clearance), so links run through the middle of rooms and
	//    corridors instead of along the walls.
	std::vector<char> forced(nSpans, 0);
	for (int l = 0; l < world.GetLinkCount(); l++)
	{
		const BotWorldLink& link = world.GetLinks()[l];
		forced[link.nFrom] = forced[link.nTo] = 1;
	}
	const std::vector<LTVector>& seeds = world.GetSeeds();
	for (size_t s = 0; s < seeds.size(); s++)
	{
		int n = world.FindSpan(seeds[s] + LTVector(0.0f, world.GetBody().fHalfHeight - 20.0f, 0.0f));
		if (n >= 0) forced[n] = 1;
	}

	std::vector< std::pair<int, int> > order;	// (-priority, span)
	order.reserve(nSpans);
	for (int i = 0; i < nSpans; i++)
	{
		const BotSpan& s = world.GetSpan(i);
		if (!(s.nFlags & BOTSPAN_REACH)) continue;
		int nPri = forced[i] ? 100 : (int)s.nClear;
		order.push_back(std::make_pair(-nPri, i));
	}
	std::sort(order.begin(), order.end());

	// Occupancy grid of placed waypoints, per cell (several layers per cell)
	std::map<long long, std::vector<int> > placed;
	for (size_t o = 0; o < order.size(); o++)
	{
		int n = order[o].second;
		const BotSpan& s = world.GetSpan(n);
		int x, z;
		world.GetSpanCell(n, x, z);

		int R = (s.nFlags & BOTSPAN_SWIM) ? kSwimSpacing : kWalkSpacing;
		if (forced[n]) R = 1;
		LTFLOAT fY = world.GetSpanPos(n).y;

		LTBOOL bNear = LTFALSE;
		for (int zz = z - R + 1; zz <= z + R - 1 && !bNear; zz++)
		for (int xx = x - R + 1; xx <= x + R - 1 && !bNear; xx++)
		{
			std::map<long long, std::vector<int> >::const_iterator it = placed.find(((long long)xx << 32) | (unsigned int)zz);
			if (it == placed.end()) continue;
			for (size_t k = 0; k < it->second.size(); k++)
			{
				if ((LTFLOAT)fabs(m_Nodes[it->second[k]].vPos.y - fY) < 56.0f) { bNear = LTTRUE; break; }
			}
		}
		if (bNear) continue;

		uint8 nFlags = 0;
		if (s.nFlags & BOTSPAN_SWIM)	nFlags |= BOTNODE_WATER;
		if (s.nFlags & BOTSPAN_NOAIR)	nFlags |= BOTNODE_NOAIR;
		if (s.nFlags & BOTSPAN_LADDER)	nFlags |= BOTNODE_LADDER;

		int nNode = AddNode(world.GetSpanPos(n), n, nFlags);
		w.GetSpanRW(n).nNode = nNode;
		placed[((long long)x << 32) | (unsigned int)z].push_back(nNode);
	}

	// 2) Every floor cell belongs to the nearest waypoint, by walking there
	//    AND back (so a waypoint's cells are all mutually reachable, and a
	//    link between two waypoints is a move a bot can really make).  Cells
	//    you can only drop into or climb out of get waypoints of their own.
	{
		typedef std::pair<LTFLOAT, int> Entry;
		std::vector<LTFLOAT> dist(nSpans, 1.0e30f);

		struct Grow
		{
			static LTBOOL TwoWay(const CBotWorld& w, int n, int d)
			{
				const BotSpan& s = w.GetSpan(n);
				int m = s.aNbr[d];
				if (m < 0 || s.GetMove(d) != eBotMoveWalk) return LTFALSE;
				const BotSpan& t = w.GetSpan(m);
				int b = (d + 4) & 7;
				return t.GetMove(b) == eBotMoveWalk && t.aNbr[b] == n;
			}

			static void Run(CBotWorld& w, std::vector<LTFLOAT>& dist, std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry> >& open, LTFLOAT fMax)
			{
				while (!open.empty())
				{
					LTFLOAT f = open.top().first;
					int n = open.top().second;
					open.pop();
					if (f > dist[n]) continue;
					for (int d = 0; d < 8; d++)
					{
						if (!TwoWay(w, n, d)) continue;
						int m = w.GetSpan(n).aNbr[d];
						if (!(w.GetSpan(m).nFlags & BOTSPAN_REACH)) continue;
						LTFLOAT fNew = f + ((d & 1) ? 1.414f : 1.0f);
						if (fNew > fMax || fNew >= dist[m]) continue;
						dist[m] = fNew;
						w.GetSpanRW(m).nNode = w.GetSpan(n).nNode;
						open.push(Entry(fNew, m));
					}
				}
			}
		};

		std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry> > open;
		for (int i = 0; i < GetNumNodes(); i++)
		{
			dist[m_Nodes[i].nSpan] = 0.0f;
			open.push(Entry(0.0f, m_Nodes[i].nSpan));
		}
		Grow::Run(w, dist, open, 1.0e29f);

		// Left over: one-way pockets.  A waypoint each (best spots first),
		// owning what it can walk to and back nearby.
		for (size_t o = 0; o < order.size(); o++)
		{
			int n = order[o].second;
			if (world.GetSpan(n).nNode >= 0) continue;
			const BotSpan& s = world.GetSpan(n);
			uint8 nFlags = 0;
			if (s.nFlags & BOTSPAN_SWIM)	nFlags |= BOTNODE_WATER;
			if (s.nFlags & BOTSPAN_NOAIR)	nFlags |= BOTNODE_NOAIR;
			if (s.nFlags & BOTSPAN_LADDER)	nFlags |= BOTNODE_LADDER;
			int nNode = AddNode(world.GetSpanPos(n), n, nFlags);
			w.GetSpanRW(n).nNode = nNode;
			dist[n] = 0.0f;
			open.push(Entry(0.0f, n));
			Grow::Run(w, dist, open, (LTFLOAT)((s.nFlags & BOTSPAN_SWIM) ? kSwimSpacing : kWalkSpacing));
		}
	}

	// 3) Links where cells of two waypoints touch
	for (int n = 0; n < nSpans; n++)
	{
		const BotSpan& s = world.GetSpan(n);
		if (s.nNode < 0) continue;
		for (int d = 0; d < 8; d++)
		{
			int m = s.aNbr[d];
			int nMove = s.GetMove(d);
			if (m < 0 || nMove == eBotMoveNone) continue;
			int nTo = world.GetSpan(m).nNode;
			if (nTo < 0 || nTo == s.nNode) continue;

			uint8 nFlags = 0;
			if (nMove == eBotMoveClimb) nFlags = BOTLINK_JUMP;
			else if (nMove == eBotMoveDrop) nFlags = BOTLINK_DROP;
			AddLink(s.nNode, nTo, nFlags);
		}
	}
	for (int l = 0; l < world.GetLinkCount(); l++)
	{
		const BotWorldLink& link = world.GetLinks()[l];
		int a = world.GetSpan(link.nFrom).nNode, b = world.GetSpan(link.nTo).nNode;
		if (a < 0 || b < 0 || a == b) continue;
		uint8 nFlags = (link.nType == eBotLinkZip) ? BOTLINK_ZIP : (link.nType == eBotLinkLadder) ? BOTLINK_LADDER : (BOTLINK_GAP | BOTLINK_JUMP);
		AddLink(a, b, nFlags, l);
	}

	// 4) Perches: well above the floors around them, or only reachable by
	//    zipcord / ladder / climbing.  Good spots to hold.
	m_Incoming.assign(GetNumNodes(), 0);
	std::vector<char> walkIn(GetNumNodes(), 0), specialIn(GetNumNodes(), 0), dropOut(GetNumNodes(), 0);
	for (int i = 0; i < GetNumNodes(); i++)
	{
		for (int k = 0; k < m_Nodes[i].nLinks; k++)
		{
			const BotNavLink& link = m_Nodes[i].aLinks[k];
			m_Incoming[link.nTo]++;
			if (m_Nodes[link.nTo].vPos.y < m_Nodes[i].vPos.y - 80.0f) dropOut[i] = 1;
			LTBOOL bUp = m_Nodes[link.nTo].vPos.y > m_Nodes[i].vPos.y + 60.0f;
			if (link.nFlags & (BOTLINK_ZIP | BOTLINK_LADDER | BOTLINK_JUMP)) { if (bUp) specialIn[link.nTo] = 1; }
			else if (bUp || (LTFLOAT)fabs(m_Nodes[link.nTo].vPos.y - m_Nodes[i].vPos.y) < 60.0f) walkIn[link.nTo] = 1;
		}
	}
	for (int i = 0; i < GetNumNodes(); i++)
	{
		if (m_Nodes[i].nFlags & BOTNODE_WATER) continue;
		// Compared with the ground right around it (not the bottom of the
		// reservoir 2000u away), and it has to be an edge you look down from
		std::vector<int> nearby;
		GetNodesNear(m_Nodes[i].vPos, 500.0f, 600.0f, nearby);
		LTFLOAT fSum = 0.0f;
		int nCount = 0;
		for (size_t k = 0; k < nearby.size(); k++)
		{
			fSum += m_Nodes[nearby[k]].vPos.y;		// (water surface counts as the ground around)
			nCount++;
		}
		LTFLOAT fAbove = nCount ? m_Nodes[i].vPos.y - fSum / nCount : 0.0f;
		if ((fAbove > 120.0f && (dropOut[i] || specialIn[i])) || (specialIn[i] && fAbove > 60.0f))
		{
			m_Nodes[i].nFlags |= BOTNODE_HIGH;
		}
	}

	// 5) Playable: reachable from a spawn point (following the links' directions)
	m_Playable.assign(GetNumNodes(), 0);
	{
		std::vector<int> queue;
		const std::vector<LTVector>& spawns = world.GetSpawns();
		for (size_t s = 0; s < spawns.size(); s++)
		{
			int nSpan = world.FindSpan(spawns[s] + LTVector(0.0f, world.GetBody().fHalfHeight - 20.0f, 0.0f));
			int n = (nSpan >= 0) ? world.GetSpan(nSpan).nNode : BOTNAV_INVALID;
			if (n >= 0 && !m_Playable[n]) { m_Playable[n] = 1; queue.push_back(n); }
		}
		if (queue.empty()) m_Playable.assign(GetNumNodes(), 1);
		for (size_t q = 0; q < queue.size(); q++)
		{
			const BotNavNode& node = m_Nodes[queue[q]];
			for (int l = 0; l < node.nLinks; l++)
			{
				int m = node.aLinks[l].nTo;
				if (!m_Playable[m]) { m_Playable[m] = 1; queue.push_back(m); }
			}
		}
	}
	int nPlayable = 0;
	for (size_t i = 0; i < m_Playable.size(); i++) nPlayable += m_Playable[i];

	for (int t = 0; t < 3; t++) m_aDanger[t].assign(GetNumNodes(), 0.0f);

	g_pLTServer->CPrint("Bots: %d of %d waypoints reachable from the spawns", nPlayable, GetNumNodes());
	g_pLTServer->CPrint("Bots: %d waypoints (%d water, %d high), %d zipcord / %d ladder / %d jump links",
		GetNumNodes(), CountNodes(BOTNODE_WATER), CountNodes(BOTNODE_HIGH),
		CountLinks(BOTLINK_ZIP), CountLinks(BOTLINK_LADDER), CountLinks(BOTLINK_JUMP));
}

// ----------------------------------------------------------------------- //
//	Queries
// ----------------------------------------------------------------------- //

int CBotNav::FindNearest(const LTVector& vPos, LTFLOAT fMaxDist, LTBOOL bVisible) const
{
	// The waypoint owning the floor we're on: right by construction (it's
	// reachable on foot from here, not just close through a wall)
	if (m_pWorld && m_pWorld->IsReady())
	{
		int nSpan = m_pWorld->FindSpanNear(vPos, (int)(LTMIN(fMaxDist, 400.0f) / CBotWorld::kCell));
		if (nSpan >= 0)
		{
			int n = m_pWorld->GetSpan(nSpan).nNode;
			if (n >= 0 && n < GetNumNodes()) return n;
		}
	}

	LTFLOAT aRadius[3] = { 150.0f, 400.0f, fMaxDist };
	for (int r = 0; r < 3; r++)
	{
		LTFLOAT fRadius = (aRadius[r] < fMaxDist) ? aRadius[r] : fMaxDist;

		std::vector< std::pair<LTFLOAT, int> > cands;
		int nR = (int)(fRadius / kCellSize) + 1;
		int cx = CellCoord(vPos.x), cy = CellCoord(vPos.y), cz = CellCoord(vPos.z);
		int nYR = (int)(200.0f / kCellSize) + 1;

		for (int x = cx - nR; x <= cx + nR; x++)
		for (int y = cy - nYR; y <= cy + nYR; y++)
		for (int z = cz - nR; z <= cz + nR; z++)
		{
			CellMap::const_iterator it = m_Cells.find(CellKey(x, y, z));
			if (it == m_Cells.end()) continue;
			for (size_t i = 0; i < it->second.size(); i++)
			{
				int n = it->second[i];
				LTFLOAT fDist = vPos.Dist(m_Nodes[n].vPos);
				if (fDist <= fRadius) cands.push_back(std::make_pair(fDist, n));
			}
		}

		if (!cands.empty())
		{
			std::sort(cands.begin(), cands.end());
			if (!bVisible) return cands[0].second;

			for (size_t j = 0; j < cands.size() && j < 8; j++)
			{
				if (IsVisible(vPos, m_Nodes[cands[j].second].vPos)) return cands[j].second;
			}
		}

		if (fRadius >= fMaxDist) break;
	}

	return BOTNAV_INVALID;
}

int CBotNav::FindCover(const LTVector& vPos, const LTVector& vThreat, LTFLOAT fRadius) const
{
	int nStart = FindNearest(vPos, 300.0f, LTFALSE);
	if (nStart < 0) return BOTNAV_INVALID;

	// Nearby waypoints we can run to (graph distance), hidden from the threat
	std::vector< std::pair<LTFLOAT, int> > queue;
	std::set<int> seen;
	queue.push_back(std::make_pair(0.0f, nStart));
	seen.insert(nStart);

	for (size_t q = 0; q < queue.size() && q < 64; q++)
	{
		int n = queue[q].second;
		LTFLOAT fLen = queue[q].first;
		const BotNavNode& node = m_Nodes[n];

		if (fLen > 40.0f && !(node.nFlags & BOTNODE_WATER) &&
			!IsVisible(vThreat, node.vPos + LTVector(0.0f, 30.0f, 0.0f)) &&
			!IsVisible(vThreat, node.vPos - LTVector(0.0f, 20.0f, 0.0f)))
		{
			return n;
		}

		for (int l = 0; l < node.nLinks; l++)
		{
			const BotNavLink& link = node.aLinks[l];
			if (link.nFlags & (BOTLINK_ZIP | BOTLINK_LADDER | BOTLINK_GAP)) continue;
			LTFLOAT fNext = fLen + node.vPos.Dist(m_Nodes[link.nTo].vPos);
			if (fNext > fRadius || !seen.insert(link.nTo).second) continue;
			queue.push_back(std::make_pair(fNext, link.nTo));
		}
		std::sort(queue.begin() + q + 1, queue.end());
	}

	return BOTNAV_INVALID;
}

int CBotNav::CountNodes(uint8 nFlag) const
{
	int nCount = 0;
	for (int i = 0; i < GetNumNodes(); i++) if (m_Nodes[i].nFlags & nFlag) nCount++;
	return nCount;
}

int CBotNav::CountLinks(uint8 nFlag) const
{
	int nCount = 0;
	for (int i = 0; i < GetNumNodes(); i++)
		for (int l = 0; l < m_Nodes[i].nLinks; l++)
			if (m_Nodes[i].aLinks[l].nFlags & nFlag) nCount++;
	return nCount;
}

int CBotNav::GetRandomNode() const
{
	if (m_Nodes.empty()) return BOTNAV_INVALID;
	for (int nTry = 0; nTry < 32; nTry++)
	{
		int n = GetRandom(0, GetNumNodes()-1);
		if (IsPlayable(n)) return n;
	}
	return GetRandom(0, GetNumNodes()-1);
}

LTFLOAT CBotNav::LinkCost(int nFrom, const BotNavLink& link, const BotRouteCost* pCost) const
{
	const BotNavNode& from = m_Nodes[nFrom];
	const BotNavNode& to = m_Nodes[link.nTo];
	LTFLOAT fLen = from.vPos.Dist(to.vPos);

	// Long moves cost what they take (riding time, climbing), not their length
	if (link.nWorldLink >= 0 && m_pWorld && link.nWorldLink < m_pWorld->GetLinkCount())
	{
		const BotWorldLink& wl = m_pWorld->GetLinks()[link.nWorldLink];
		if (wl.nFails >= 3) return -1.0f;		// Doesn't work (this session)
		fLen = wl.fCost + from.vPos.Dist(m_pWorld->GetSpanPos(wl.nFrom)) + m_pWorld->GetSpanPos(wl.nTo).Dist(to.vPos);
		if ((link.nFlags & BOTLINK_ZIP) && pCost) fLen *= pCost->fZipMul;
	}
	else if (link.nFlags & BOTLINK_JUMP)
	{
		fLen += 60.0f;
	}
	else if (link.nFlags & BOTLINK_DROP)
	{
		// Falling is quick, but the height is lost
		LTVector vD = to.vPos - from.vPos;
		vD.y = 0.0f;
		fLen = vD.Mag() + 40.0f;
	}

	if (pCost)
	{
		if (to.nFlags & BOTNODE_WATER) fLen *= pCost->fWaterMul;
		if (to.nFlags & BOTNODE_NOAIR) fLen *= pCost->fNoAirMul;
	}
	return fLen;
}

LTFLOAT CBotNav::PathCost(int nFrom, int nTo, const BotRouteCost* pCost) const
{
	std::vector<int> path;
	if (!FindPath(nFrom, nTo, path, pCost)) return -1.0f;
	LTFLOAT fCost = 0.0f;
	int nPrev = nFrom;
	for (size_t i = 0; i < path.size(); i++)
	{
		const BotNavLink* pLink = GetLink(nPrev, path[i]);
		LTFLOAT f = pLink ? LinkCost(nPrev, *pLink, pCost) : -1.0f;
		fCost += (f > 0.0f) ? f : m_Nodes[nPrev].vPos.Dist(m_Nodes[path[i]].vPos);
		nPrev = path[i];
	}
	return fCost;
}

void CBotNav::MarkHazard(int nNode, LTFLOAT fUntil)
{
	if (nNode < 0 || nNode >= (int)m_Nodes.size()) return;
	if (m_HazardUntil.size() != m_Nodes.size()) m_HazardUntil.assign(m_Nodes.size(), 0.0f);
	m_HazardUntil[nNode] = LTMAX(m_HazardUntil[nNode], fUntil);
}

LTBOOL CBotNav::IsHazard(int nNode) const
{
	return nNode >= 0 && nNode < (int)m_HazardUntil.size() && m_HazardUntil[nNode] > g_pLTServer->GetTime();
}

LTBOOL CBotNav::FindPath(int nFrom, int nTo, std::vector<int>& path, const BotRouteCost* pCost) const
{
	path.clear();

	int nNodes = GetNumNodes();
	if (nFrom < 0 || nFrom >= nNodes || nTo < 0 || nTo >= nNodes) return LTFALSE;
	if (nFrom == nTo) { path.push_back(nTo); return LTTRUE; }

	std::vector<LTFLOAT>	cost(nNodes, 1.0e30f);
	std::vector<int>		parent(nNodes, BOTNAV_INVALID);
	std::vector<char>		closed(nNodes, 0);

	typedef std::pair<LTFLOAT, int> Entry;
	std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry> > open;

	const LTVector& vGoal = m_Nodes[nTo].vPos;

	cost[nFrom] = 0.0f;
	open.push(Entry(m_Nodes[nFrom].vPos.Dist(vGoal), nFrom));

	while (!open.empty())
	{
		int nCur = open.top().second;
		open.pop();
		if (closed[nCur]) continue;
		closed[nCur] = 1;

		if (nCur == nTo)
		{
			for (int n = nTo; n != nFrom; n = parent[n])
			{
				path.push_back(n);
			}
			std::reverse(path.begin(), path.end());
			return LTTRUE;
		}

		const BotNavNode& node = m_Nodes[nCur];
		for (int l = 0; l < node.nLinks; l++)
		{
			int nNext = node.aLinks[l].nTo;
			if (closed[nNext]) continue;

			if (pCost && pCost->pAvoid && pCost->pAvoid->count(LinkKey(nCur, nNext))) continue;

			LTFLOAT fLen = LinkCost(nCur, node.aLinks[l], pCost);
			if (fLen < 0.0f) continue;

			// Learned killing zones cost more (never less, so A* stays exact)
			if (pCost && pCost->pDanger && pCost->fDangerMul > 0.0f && nNext < (int)pCost->pDanger->size())
			{
				LTFLOAT fDanger = (*pCost->pDanger)[nNext];
				if (fDanger > 0.05f) fLen *= 1.0f + pCost->fDangerMul * LTMIN(fDanger, 6.0f);
			}

			if (pCost && pCost->pLane && nNext < (int)pCost->pLane->size()) fLen *= (*pCost->pLane)[nNext];

			// Someone got pinned there lately: go round if there's another way
			if (IsHazard(nNext)) fLen += 1500.0f;

			LTFLOAT fCost = cost[nCur] + fLen;

			if (fCost < cost[nNext])
			{
				cost[nNext]		= fCost;
				parent[nNext]	= nCur;
				open.push(Entry(fCost + m_Nodes[nNext].vPos.Dist(vGoal), nNext));
			}
		}
	}

	return LTFALSE;
}

// ----------------------------------------------------------------------- //
//	Learned danger.  Every death marks the waypoints around it, for the team
//	that died there (0 = deathmatch).  Routes avoid killing zones when there
//	is another way, the values persist in the .nav file across matches, and
//	they fade slowly so the bots can unlearn old habits.
// ----------------------------------------------------------------------- //

const std::vector<LTFLOAT>* CBotNav::GetDanger(int nTeam)
{
	if (nTeam < 0 || nTeam > 2) nTeam = 0;
	std::vector<LTFLOAT>& v = m_aDanger[nTeam];
	if ((int)v.size() < GetNumNodes()) v.resize(GetNumNodes(), 0.0f);
	return &v;
}

void CBotNav::AddDanger(int nTeam, const LTVector& vPos, LTFLOAT fAmount)
{
	if (nTeam < 0 || nTeam > 2) nTeam = 0;
	std::vector<LTFLOAT>& v = m_aDanger[nTeam];
	if ((int)v.size() < GetNumNodes()) v.resize(GetNumNodes(), 0.0f);

	const LTFLOAT fR = 350.0f, fDY = 150.0f;
	std::vector<int> nodes;
	GetNodesNear(vPos, fR, fDY, nodes);
	for (size_t i = 0; i < nodes.size(); i++)
	{
		int n = nodes[i];
		const LTVector& p = m_Nodes[n].vPos;
		LTFLOAT dx = p.x - vPos.x, dz = p.z - vPos.z;
		LTFLOAT d = (LTFLOAT)sqrt(dx*dx + dz*dz);
		if (d > fR) continue;
		v[n] = LTMIN(20.0f, v[n] + fAmount * (1.0f - d / fR));
	}
	m_nChanges++;
}

void CBotNav::DecayDanger(LTFLOAT fFactor)
{
	for (int t = 0; t < 3; t++)
	{
		std::vector<LTFLOAT>& v = m_aDanger[t];
		for (size_t i = 0; i < v.size(); i++) v[i] *= fFactor;
	}
}

int CBotNav::CountDangerNodes(int nTeam, LTFLOAT fMin) const
{
	if (nTeam < 0 || nTeam > 2) return 0;
	int n = 0;
	for (size_t i = 0; i < m_aDanger[nTeam].size(); i++) if (m_aDanger[nTeam][i] >= fMin) n++;
	return n;
}

void CBotNav::GetNodesNear(const LTVector& vPos, LTFLOAT fRadius, LTFLOAT fMaxDY, std::vector<int>& nodes) const
{
	int nR = (int)(fRadius / kCellSize) + 1, nYR = (int)(fMaxDY / kCellSize) + 1;
	int cx = CellCoord(vPos.x), cy = CellCoord(vPos.y), cz = CellCoord(vPos.z);
	for (int x = cx-nR; x <= cx+nR; x++)
	for (int y = cy-nYR; y <= cy+nYR; y++)
	for (int z = cz-nR; z <= cz+nR; z++)
	{
		CellMap::const_iterator it = m_Cells.find(CellKey(x, y, z));
		if (it == m_Cells.end()) continue;
		for (size_t i = 0; i < it->second.size(); i++)
		{
			int n = it->second[i];
			const LTVector& p = m_Nodes[n].vPos;
			if ((LTFLOAT)fabs(p.y - vPos.y) > fMaxDY) continue;
			LTFLOAT dx = p.x - vPos.x, dz = p.z - vPos.z;
			if (dx*dx + dz*dz <= fRadius*fRadius) nodes.push_back(n);
		}
	}
}

// The node we can actually reach (from nStart) that is closest to vTarget:
// where to go when there is no known way all the way there.
int CBotNav::FindFrontier(int nStart, const LTVector& vTarget) const
{
	int N = GetNumNodes();
	if (nStart < 0 || nStart >= N) return BOTNAV_INVALID;

	std::vector<char> seen(N, 0);
	std::vector<int> queue;
	queue.reserve(1024);
	queue.push_back(nStart);
	seen[nStart] = 1;

	int nBest = nStart;
	LTFLOAT fBest = 1.0e30f;
	for (size_t q = 0; q < queue.size(); q++)
	{
		int u = queue[q];
		const LTVector& v = m_Nodes[u].vPos;
		LTFLOAT dx = v.x - vTarget.x, dz = v.z - vTarget.z;
		LTFLOAT d = dx*dx + dz*dz;
		if (d < fBest) { fBest = d; nBest = u; }

		const BotNavNode& node = m_Nodes[u];
		for (int l = 0; l < node.nLinks; l++)
		{
			int w = node.aLinks[l].nTo;
			if (w >= 0 && w < N && !seen[w]) { seen[w] = 1; queue.push_back(w); }
		}
	}
	return nBest;
}

// ----------------------------------------------------------------------- //
//	Files
// ----------------------------------------------------------------------- //

static void GetNavFilename(const char* szWorld, char* szFile, int nLen)
{
	// "Worlds\Multi\DeathMatch\001ThePad_DM" -> "Bots\001ThePad_DM.nav"
	const char* pBase = szWorld;
	for (const char* p = szWorld; *p; p++)
	{
		if (*p == '\\' || *p == '/') pBase = p + 1;
	}

	_snprintf(szFile, nLen, "Bots\\%s.nav", pBase);
	szFile[nLen-1] = '\0';

	char* pExt = strrchr(szFile, '.');
	if (pExt && _stricmp(pExt, ".dat") == 0)
	{
		strcpy(pExt, ".nav");
	}
}

LTBOOL CBotNav::LoadDanger(const char* szWorld)
{
	if (!szWorld || !szWorld[0]) return LTFALSE;

	char szFile[MAX_PATH];
	GetNavFilename(szWorld, szFile, sizeof(szFile));

	FILE* fp = fopen(szFile, "rt");
	if (!fp) return LTFALSE;

	// Only a file made from the same graph lines up node for node: the
	// header's node count and every saved node position must match ours
	// before any danger values are applied
	int nVersion = -1, nFileNodes = -1, nNodeLine = 0;
	LTBOOL bMatch = LTTRUE;
	struct Danger { int nTeam, nNode; LTFLOAT fValue; };
	std::vector<Danger> danger;
	char szLine[256];
	while (bMatch && fgets(szLine, sizeof(szLine), fp))
	{
		int a, b, f;
		float x, y, z;
		if (sscanf(szLine, "NOLFBOTNAV %d %d", &a, &b) >= 1)
		{
			nVersion = a;
			nFileNodes = (sscanf(szLine, "NOLFBOTNAV %d %d", &a, &b) == 2) ? b : -1;
			if (nVersion != BOTNAV_VERSION || nFileNodes != GetNumNodes()) bMatch = LTFALSE;
		}
		else if (sscanf(szLine, "N %f %f %f %d", &x, &y, &z, &f) == 4)
		{
			if (nNodeLine >= GetNumNodes() || m_Nodes[nNodeLine].vPos.DistSqr(LTVector(x, y, z)) > 1.0f) bMatch = LTFALSE;
			nNodeLine++;
		}
		else if (sscanf(szLine, "D %d %d %f", &a, &b, &x) == 3)
		{
			if (a >= 0 && a <= 2 && b >= 0 && b < GetNumNodes())
			{
				Danger d = { a, b, x };
				danger.push_back(d);
			}
		}
	}
	fclose(fp);

	if (!bMatch || nVersion != BOTNAV_VERSION || nFileNodes != GetNumNodes() || nNodeLine != GetNumNodes()) return LTFALSE;

	// Learned danger (fades a little each time the map is loaded)
	for (size_t i = 0; i < danger.size(); i++)
	{
		std::vector<LTFLOAT>& v = m_aDanger[danger[i].nTeam];
		if ((int)v.size() < GetNumNodes()) v.resize(GetNumNodes(), 0.0f);
		v[danger[i].nNode] = danger[i].fValue * 0.85f;
	}
	return LTTRUE;
}

LTBOOL CBotNav::Save(const char* szWorld)
{
	if (!szWorld || !szWorld[0] || m_Nodes.empty()) return LTFALSE;

	// Self-play evaluation runs (NOLFBOT_NOSAVE=1) must not change the maps
	// they're comparing on
	const char* szNoSave = getenv("NOLFBOT_NOSAVE");
	if (szNoSave && szNoSave[0] == '1') return LTTRUE;

	char szFile[MAX_PATH];
	GetNavFilename(szWorld, szFile, sizeof(szFile));

	_mkdir("Bots");

	FILE* fp = fopen(szFile, "wt");
	if (!fp)
	{
		g_pLTServer->CPrint("Bots: couldn't write %s", szFile);
		return LTFALSE;
	}

	fprintf(fp, "NOLFBOTNAV %d %d\n", BOTNAV_VERSION, GetNumNodes());
	for (int i = 0; i < GetNumNodes(); i++)
	{
		const LTVector& v = m_Nodes[i].vPos;
		fprintf(fp, "N %.1f %.1f %.1f %d\n", v.x, v.y, v.z, (int)m_Nodes[i].nFlags);
	}
	for (int i = 0; i < GetNumNodes(); i++)
	{
		for (int l = 0; l < m_Nodes[i].nLinks; l++)
		{
			fprintf(fp, "L %d %d %d\n", i, m_Nodes[i].aLinks[l].nTo, m_Nodes[i].aLinks[l].nFlags);
		}
	}
	for (int t = 0; t < 3; t++)
	{
		const std::vector<LTFLOAT>& v = m_aDanger[t];
		for (size_t i = 0; i < v.size() && (int)i < GetNumNodes(); i++)
		{
			if (v[i] >= 0.05f) fprintf(fp, "D %d %d %.2f\n", t, (int)i, v[i]);
		}
	}

	fclose(fp);
	m_nChanges = 0;
	return LTTRUE;
}
