// ----------------------------------------------------------------------- //
//
// MODULE  : BotMapInfo.cpp
//
// PURPOSE : Map structure for the bots (see BotMapInfo.h)
//
// ----------------------------------------------------------------------- //

#include "stdafx.h"
#include "BotMapInfo.h"
#include "BotNav.h"
#include "BotWorld.h"
#include <stdio.h>
#include <algorithm>
#include <set>

static const char* s_aAreaKind[]  = { "ground", "high", "water", "flooded" };
static const char* s_aRouteKind[] = { "main", "flank", "high", "water" };

CBotMapInfo::CBotMapInfo()
{
	Clear();
}

void CBotMapInfo::Clear()
{
	m_Areas.clear();
	m_NodeArea.clear();
	for (int t = 0; t < 3; t++)
	{
		m_Routes[t].clear();
		m_avRouteTo[t].Init();
	}
}

static int NodeKind(const BotNavNode& node)
{
	if (node.nFlags & BOTNODE_NOAIR) return eAreaFlooded;
	if (node.nFlags & BOTNODE_WATER) return eAreaWater;
	if (node.nFlags & BOTNODE_HIGH)  return eAreaHigh;
	return eAreaGround;
}

static LTFLOAT HDist(const LTVector& a, const LTVector& b)
{
	LTFLOAT dx = a.x - b.x, dz = a.z - b.z;
	return (LTFLOAT)sqrt(dx*dx + dz*dz);
}

// ----------------------------------------------------------------------- //
//	Areas: grow regions over the waypoints, from the most open spots out,
//	keeping one kind of ground (dry / high / water) and one level each.
// ----------------------------------------------------------------------- //

void CBotMapInfo::Build(const CBotNav& nav, const CBotWorld& world)
{
	Clear();
	int N = nav.GetNumNodes();
	if (N == 0) return;

	// Walking links both ways (areas are about where you can be together,
	// long moves like zipcords always separate areas)
	std::vector< std::vector<int> > adj(N);
	for (int i = 0; i < N; i++)
	{
		const BotNavNode& node = nav.GetNode(i);
		for (int l = 0; l < node.nLinks; l++)
		{
			const BotNavLink& link = node.aLinks[l];
			if (link.nFlags & (BOTLINK_ZIP | BOTLINK_LADDER | BOTLINK_GAP)) continue;
			adj[i].push_back(link.nTo);
			adj[link.nTo].push_back(i);
		}
	}

	std::vector< std::pair<int, int> > order;
	for (int i = 0; i < N; i++)
	{
		int nClear = world.IsReady() && nav.GetNode(i).nSpan >= 0 ? world.GetSpan(nav.GetNode(i).nSpan).nClear : 0;
		order.push_back(std::make_pair(-nClear, i));
	}
	std::sort(order.begin(), order.end());

	m_NodeArea.assign(N, -1);
	for (size_t o = 0; o < order.size(); o++)
	{
		int nSeed = order[o].second;
		if (m_NodeArea[nSeed] >= 0) continue;

		const BotNavNode& seed = nav.GetNode(nSeed);
		int nKind = NodeKind(seed);
		LTBOOL bWet = (nKind == eAreaWater || nKind == eAreaFlooded);
		LTFLOAT fMaxR  = bWet ? 1100.0f : 560.0f;
		LTFLOAT fMaxDY = bWet ? 500.0f : 140.0f;
		size_t nMax	   = bWet ? 80 : 36;

		int nArea = (int)m_Areas.size();
		m_Areas.push_back(BotArea());
		BotArea& area = m_Areas.back();
		area.nKind = nKind;

		std::vector<int> queue;
		queue.push_back(nSeed);
		m_NodeArea[nSeed] = nArea;
		for (size_t q = 0; q < queue.size() && queue.size() < nMax; q++)
		{
			int u = queue[q];
			for (size_t k = 0; k < adj[u].size() && queue.size() < nMax; k++)
			{
				int v = adj[u][k];
				if (m_NodeArea[v] >= 0) continue;
				const BotNavNode& nv = nav.GetNode(v);
				if (NodeKind(nv) != nKind) continue;
				if ((LTFLOAT)fabs(nv.vPos.y - seed.vPos.y) > fMaxDY) continue;
				if (HDist(nv.vPos, seed.vPos) > fMaxR) continue;
				m_NodeArea[v] = nArea;
				queue.push_back(v);
			}
		}

		area.nodes = queue;
		LTVector vSum(0.0f, 0.0f, 0.0f);
		for (size_t k = 0; k < queue.size(); k++) vSum += nav.GetNode(queue[k]).vPos;
		area.vCenter = vSum / (LTFLOAT)queue.size();
		area.fRadius = 0.0f;
		for (size_t k = 0; k < queue.size(); k++) area.fRadius = LTMAX(area.fRadius, HDist(area.vCenter, nav.GetNode(queue[k]).vPos));
	}

	// Openings between areas, and how wide
	for (int i = 0; i < N; i++)
	{
		int a = m_NodeArea[i];
		const BotNavNode& node = nav.GetNode(i);
		for (int l = 0; l < node.nLinks; l++)
		{
			int b = m_NodeArea[node.aLinks[l].nTo];
			if (b < 0 || b == a) continue;
			BotArea& area = m_Areas[a];
			std::vector<int>::iterator it = std::find(area.neighbours.begin(), area.neighbours.end(), b);
			if (it == area.neighbours.end())
			{
				area.neighbours.push_back(b);
				area.widths.push_back(1);
			}
			else
			{
				area.widths[it - area.neighbours.begin()]++;
			}
		}
	}

	int aKinds[4] = { 0, 0, 0, 0 };
	for (size_t a = 0; a < m_Areas.size(); a++) aKinds[m_Areas[a].nKind]++;
	g_pLTServer->CPrint("Bots: map structure: %d areas (%d ground, %d high, %d water, %d flooded)",
		(int)m_Areas.size(), aKinds[0], aKinds[1], aKinds[2], aKinds[3]);
}

// ----------------------------------------------------------------------- //
//	Routes
// ----------------------------------------------------------------------- //

void CBotMapInfo::ComputeRoutes(int nTeam, const CBotNav& nav, const LTVector& vFrom, const LTVector& vTo)
{
	if (nTeam < 0 || nTeam > 2 || m_Areas.empty()) return;
	std::vector<BotRoute>& routes = m_Routes[nTeam];
	routes.clear();
	m_avRouteTo[nTeam] = vTo;

	int nFrom = nav.FindNearest(vFrom, 800.0f, LTFALSE);
	int nTo	  = nav.FindNearest(vTo, 800.0f, LTFALSE);
	if (nFrom < 0 || nTo < 0 || nFrom == nTo) return;

	int N = nav.GetNumNodes();
	std::vector<LTFLOAT> penalty(N, 0.0f);

	// Ways to go, each found with a different taste:
	//   main  - the plain best way on foot
	//   high  - zipcords welcome
	//   water - swimming welcome (SCUBA divers)
	//   flank - like main, but staying out of the areas the others use
	struct Profile { int nKind; LTFLOAT fWater, fZip, fNoAir; LTBOOL bAvoidOthers; };
	static const Profile s_aProfiles[] =
	{
		{ eRouteMain,	3.0f, 1.5f, 10.0f, LTFALSE },
		{ eRouteHigh,	3.0f, 0.2f, 10.0f, LTFALSE },
		{ eRouteWater,	0.35f, 3.0f, 1.2f, LTFALSE },
		{ eRouteFlank,	3.0f, 1.5f, 10.0f, LTTRUE },
		{ eRouteFlank,	3.0f, 1.0f, 10.0f, LTTRUE },
	};

	for (size_t p = 0; p < sizeof(s_aProfiles) / sizeof(s_aProfiles[0]); p++)
	{
		const Profile& prof = s_aProfiles[p];

		BotRouteCost cost;
		memset(&cost, 0, sizeof(cost));
		cost.fWaterMul	= prof.fWater;
		cost.fZipMul	= prof.fZip;
		cost.fNoAirMul	= prof.fNoAir;
		cost.pDanger	= prof.bAvoidOthers ? &penalty : LTNULL;
		cost.fDangerMul	= 1.0f;

		std::vector<int> path;
		if (!nav.FindPath(nFrom, nTo, path, &cost)) continue;
		path.insert(path.begin(), nFrom);

		BotRoute r;
		r.nodes	 = path;
		r.nUsers = 0;
		r.fLength = 0.0f;
		int nWater = 0, nZip = 0, nHigh = 0;
		for (size_t i = 0; i < path.size(); i++)
		{
			const BotNavNode& node = nav.GetNode(path[i]);
			if (i > 0)
			{
				r.fLength += nav.GetNode(path[i-1]).vPos.Dist(node.vPos);
				const BotNavLink* pLink = nav.GetLink(path[i-1], path[i]);
				if (pLink && (pLink->nFlags & BOTLINK_ZIP)) nZip++;
			}
			if (node.nFlags & BOTNODE_WATER) nWater++;
			if (node.nFlags & BOTNODE_HIGH) nHigh++;
			int a = m_NodeArea[path[i]];
			if (a >= 0 && (r.areas.empty() || r.areas.back() != a)) r.areas.push_back(a);
		}

		// What it really is (not what we hoped to find)
		if (nWater * 3 > (int)path.size())		r.nKind = eRouteWater;
		else if (nZip > 0 || nHigh * 5 > (int)path.size()) r.nKind = eRouteHigh;
		else r.nKind = (routes.empty() ? eRouteMain : eRouteFlank);

		// Genuinely different from what we have?
		std::set<int> mine(r.areas.begin(), r.areas.end());
		LTBOOL bSame = LTFALSE;
		for (size_t k = 0; k < routes.size() && !bSame; k++)
		{
			std::set<int> theirs(routes[k].areas.begin(), routes[k].areas.end());
			int nCommon = 0;
			for (std::set<int>::const_iterator it = mine.begin(); it != mine.end(); ++it) if (theirs.count(*it)) nCommon++;
			int nUnion = (int)(mine.size() + theirs.size()) - nCommon;
			if (nUnion > 0 && nCommon * 10 > nUnion * 6) bSame = LTTRUE;
		}
		if (bSame) continue;

		// Chokepoints: ground areas on the way that open onto the next area
		// through at most two waypoint links (doorways, bridges, stairs)
		for (size_t i = 1; i + 1 < r.areas.size(); i++)
		{
			const BotArea& area = m_Areas[r.areas[i]];
			if (area.nKind != eAreaGround) continue;
			int nNext = r.areas[i + 1], nPrev = r.areas[i - 1];
			int nOut = 99, nIn = 99;
			for (size_t k = 0; k < area.neighbours.size(); k++)
			{
				if (area.neighbours[k] == nNext) nOut = area.widths[k];
				if (area.neighbours[k] == nPrev) nIn = area.widths[k];
			}
			if (LTMIN(nOut, nIn) <= 2 && area.neighbours.size() <= 4) r.chokes.push_back(r.areas[i]);
		}

		// Lane: waypoints in this route's areas (and right next to them) cost
		// normal, everywhere else more, so a bot on this lane sticks to it
		// but can still step around trouble
		r.laneCost.assign(N, 1.7f);
		std::set<int> laneAreas(r.areas.begin(), r.areas.end());
		for (size_t i = 0; i < r.areas.size(); i++)
		{
			const BotArea& area = m_Areas[r.areas[i]];
			for (size_t k = 0; k < area.neighbours.size(); k++) laneAreas.insert(area.neighbours[k]);
		}
		for (int n = 0; n < N; n++)
		{
			int a = m_NodeArea[n];
			if (a >= 0 && laneAreas.count(a)) r.laneCost[n] = 1.0f;
		}

		routes.push_back(r);

		// The next flank should avoid everything used so far
		for (size_t i = 0; i < r.areas.size(); i++)
		{
			const BotArea& area = m_Areas[r.areas[i]];
			for (size_t k = 0; k < area.nodes.size(); k++) penalty[area.nodes[k]] = 4.0f;
		}
		// ...except right at both ends (everybody starts and arrives there)
		std::vector<int> ends;
		nav.GetNodesNear(nav.GetNode(nFrom).vPos, 700.0f, 400.0f, ends);
		nav.GetNodesNear(nav.GetNode(nTo).vPos, 700.0f, 400.0f, ends);
		for (size_t k = 0; k < ends.size(); k++) penalty[ends[k]] = 0.0f;
	}
}

int CBotMapInfo::PickRoute(int nTeam, LTBOOL bDiver, LTBOOL bZipper, LTBOOL bHasScuba)
{
	if (nTeam < 0 || nTeam > 2 || m_Routes[nTeam].empty()) return -1;
	std::vector<BotRoute>& routes = m_Routes[nTeam];

	int nWant = (bDiver && bHasScuba) ? eRouteWater : (bZipper ? eRouteHigh : -1);
	int nBest = -1;
	for (size_t i = 0; i < routes.size(); i++)
	{
		if (nWant >= 0 && routes[i].nKind != nWant) continue;
		if (nWant < 0 && (routes[i].nKind == eRouteWater && !bHasScuba)) continue;
		if (nBest < 0 || routes[i].nUsers < routes[nBest].nUsers) nBest = (int)i;
	}
	if (nBest < 0)
	{
		// Nothing of our taste: least used dry route
		for (size_t i = 0; i < routes.size(); i++)
		{
			if (routes[i].nKind == eRouteWater && !bHasScuba) continue;
			if (nBest < 0 || routes[i].nUsers < routes[nBest].nUsers) nBest = (int)i;
		}
	}
	if (nBest >= 0) routes[nBest].nUsers++;
	return nBest;
}

LTBOOL CBotMapInfo::GetDefenseChoke(int nTeam, const LTVector& vGuard, LTVector& vChoke)
{
	// The enemy's routes lead to what we guard: hold one of their chokepoints
	// between 400 and 2500 units out
	int nEnemy = (nTeam == 1) ? 2 : 1;
	std::vector<int> cands;
	for (size_t r = 0; r < m_Routes[nEnemy].size(); r++)
	{
		const BotRoute& route = m_Routes[nEnemy][r];
		for (size_t c = 0; c < route.chokes.size(); c++)
		{
			LTFLOAT d = HDist(m_Areas[route.chokes[c]].vCenter, vGuard);
			if (d > 400.0f && d < 2500.0f) cands.push_back(route.chokes[c]);
		}
	}
	if (cands.empty()) return LTFALSE;
	vChoke = m_Areas[cands[GetRandom(0, (int)cands.size() - 1)]].vCenter;
	return LTTRUE;
}

// ----------------------------------------------------------------------- //
//	Reports
// ----------------------------------------------------------------------- //

void CBotMapInfo::Describe(FILE* fp, const CBotNav& nav)
{
	int aKinds[4] = { 0, 0, 0, 0 };
	for (size_t a = 0; a < m_Areas.size(); a++) aKinds[m_Areas[a].nKind]++;
	fprintf(fp, "MAP areas %d (ground %d high %d water %d flooded %d)\n", (int)m_Areas.size(), aKinds[0], aKinds[1], aKinds[2], aKinds[3]);

	for (int t = 1; t <= 2; t++)
	{
		for (size_t r = 0; r < m_Routes[t].size(); r++)
		{
			const BotRoute& route = m_Routes[t][r];
			fprintf(fp, "ROUTE team%d #%d %-5s len %.0f waypoints %d areas %d chokes %d users %d via",
				t, (int)r, s_aRouteKind[route.nKind], route.fLength, (int)route.nodes.size(), (int)route.areas.size(),
				(int)route.chokes.size(), route.nUsers);
			// A few points along it, to recognise it on the map
			for (int k = 1; k <= 3; k++)
			{
				const LTVector& v = nav.GetNode(route.nodes[route.nodes.size() * k / 4]).vPos;
				fprintf(fp, " (%.0f %.0f %.0f)", v.x, v.y, v.z);
			}
			fprintf(fp, "\n");
		}
	}
}

void CBotMapInfo::DumpFile(const char* szWorld, const CBotNav& nav)
{
	if (!szWorld || !szWorld[0]) return;
	const char* pBase = szWorld;
	for (const char* p = szWorld; *p; p++) if (*p == '\\' || *p == '/') pBase = p + 1;
	char szFile[MAX_PATH];
	_snprintf(szFile, sizeof(szFile) - 1, "Bots\\%s-structure.txt", pBase);
	szFile[sizeof(szFile) - 1] = '\0';

	FILE* fp = fopen(szFile, "wt");
	if (!fp) return;
	Describe(fp, nav);
	for (size_t a = 0; a < m_Areas.size(); a++)
	{
		const BotArea& area = m_Areas[a];
		fprintf(fp, "A %d %s center %.0f %.0f %.0f r %.0f nodes %d ->", (int)a, s_aAreaKind[area.nKind],
			area.vCenter.x, area.vCenter.y, area.vCenter.z, area.fRadius, (int)area.nodes.size());
		for (size_t k = 0; k < area.neighbours.size(); k++) fprintf(fp, " %d/%d", area.neighbours[k], area.widths[k]);
		fprintf(fp, "\n");
	}
	// Doors (the scan walks through them: bots open them)
	static HCLASS hDoor = g_pLTServer->GetClass("Door");
	for (int nPass = 0; nPass < 2 && hDoor; nPass++)
	{
		HOBJECT hObj = LTNULL;
		while ((hObj = (nPass == 0 ? g_pLTServer->GetNextObject(hObj) : g_pLTServer->GetNextInactiveObject(hObj))) != LTNULL)
		{
			if (!g_pLTServer->IsKindOf(g_pLTServer->GetObjectClass(hObj), hDoor)) continue;
			LTVector vPos, vDims;
			g_pLTServer->GetObjectPos(hObj, &vPos);
			g_pLTServer->GetObjectDims(hObj, &vDims);
			char szClass[64];
			g_pLTServer->GetClassName(g_pLTServer->GetObjectClass(hObj), szClass, sizeof(szClass));
			fprintf(fp, "DOOR %s at %.0f %.0f %.0f dims %.0f %.0f %.0f\n", szClass, vPos.x, vPos.y, vPos.z, vDims.x, vDims.y, vDims.z);
		}
	}
	for (int t = 1; t <= 2; t++)
	{
		for (size_t r = 0; r < m_Routes[t].size(); r++)
		{
			const BotRoute& route = m_Routes[t][r];
			fprintf(fp, "R %d %d %s areas", t, (int)r, s_aRouteKind[route.nKind]);
			for (size_t k = 0; k < route.areas.size(); k++) fprintf(fp, " %d", route.areas[k]);
			fprintf(fp, " chokes");
			for (size_t k = 0; k < route.chokes.size(); k++) fprintf(fp, " %d", route.chokes[k]);
			fprintf(fp, "\n");
		}
	}
	fclose(fp);
}
