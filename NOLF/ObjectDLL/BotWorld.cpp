// ----------------------------------------------------------------------- //
//
// MODULE  : BotWorld.cpp
//
// PURPOSE : Geometric map scan for the bots (see BotWorld.h)
//
// ----------------------------------------------------------------------- //

#include "stdafx.h"
#include "BotWorld.h"
#include "ServerUtilities.h"
#include "ContainerCodes.h"
#include "PlayerButes.h"
#include "ServerButeMgr.h"
#include <stdio.h>
#include <direct.h>
#include <algorithm>
#include <queue>
#include <map>
#include <set>

const float CBotWorld::kCell = 32.0f;

#define BOTWORLD_CACHE_VERSION	10

static const LTFLOAT kMinFloorNormal	= 0.6f;		// Steeper than ~53 degrees is a wall
static const LTFLOAT kHeadScan			= 480.0f;	// How far up we measure headroom on dry land
static const LTFLOAT kZipRange			= 2200.0f;
static const int	 kMaxCells			= 900;		// Per axis (28800u)
static const int	 kMaxJumpLinks		= 3000;

static const int s_aDX[8] = { 1, 1, 0, -1, -1, -1,  0,  1 };
static const int s_aDZ[8] = { 0, 1, 1,  1,  0, -1, -1, -1 };

// ----------------------------------------------------------------------- //
//	Rays: the world and world models (crates, walls, terrain pieces), but
//	not doors (bots open them) and not characters / props.
// ----------------------------------------------------------------------- //

static LTBOOL BotWorldFilterFn(HOBJECT hObj, void *pUserData)
{
	if (IsMainWorld(hObj)) return LTTRUE;
	if (OT_WORLDMODEL != g_pLTServer->GetObjectType(hObj)) return LTFALSE;

	static HCLASS hDoor = g_pLTServer->GetClass("Door");
	if (hDoor && g_pLTServer->IsKindOf(g_pLTServer->GetObjectClass(hObj), hDoor)) return LTFALSE;

	return LTTRUE;
}

static LTBOOL Ray(const LTVector& vFrom, const LTVector& vTo, IntersectInfo* pInfo = LTNULL)
{
	IntersectQuery IQuery;
	IntersectInfo IInfo;
	IQuery.m_From		= vFrom;
	IQuery.m_To			= vTo;
	IQuery.m_Flags		= INTERSECT_OBJECTS | IGNORE_NONSOLID;
	IQuery.m_FilterFn	= BotWorldFilterFn;
	return g_pLTServer->IntersectSegment(&IQuery, pInfo ? pInfo : &IInfo);
}

static LTBOOL RayClear(const LTVector& vFrom, const LTVector& vTo)
{
	return !Ray(vFrom, vTo);
}

// Same, but doors count too: you can't ride a zipcord through a door
// (open or shut, the panel and its frame stop you dead)
static LTBOOL SolidFilterFn(HOBJECT hObj, void *pUserData)
{
	if (IsMainWorld(hObj)) return LTTRUE;
	return (OT_WORLDMODEL == g_pLTServer->GetObjectType(hObj));
}

static LTBOOL RayClearSolid(const LTVector& vFrom, const LTVector& vTo)
{
	IntersectQuery IQuery;
	IntersectInfo IInfo;
	IQuery.m_From		= vFrom;
	IQuery.m_To			= vTo;
	IQuery.m_Flags		= INTERSECT_OBJECTS | IGNORE_NONSOLID;
	IQuery.m_FilterFn	= SolidFilterFn;
	return !g_pLTServer->IntersectSegment(&IQuery, &IInfo);
}

// The whole player box along a zipcord ride: center, head, feet and both
// sides, so a ride doesn't clip a door frame or the edge of an opening
static LTBOOL ZipRideClear(const LTVector& vCenter, const LTVector& vHook, LTFLOAT fHalfHeight, LTFLOAT fRadius)
{
	LTVector vUp(0.0f, fHalfHeight - 8.0f, 0.0f);
	if (!RayClearSolid(vCenter, vHook)) return LTFALSE;
	if (!RayClearSolid(vCenter + vUp, vHook)) return LTFALSE;
	if (!RayClearSolid(vCenter - vUp, vHook - vUp)) return LTFALSE;

	LTVector vDir = vHook - vCenter;
	LTVector vSide(-vDir.z, 0.0f, vDir.x);
	LTFLOAT fMag = vSide.Mag();
	if (fMag < 1.0f) return LTTRUE;		// hook straight overhead
	vSide *= (fRadius + 4.0f) / fMag;
	if (!RayClearSolid(vCenter + vSide, vHook + vSide)) return LTFALSE;
	if (!RayClearSolid(vCenter - vSide, vHook - vSide)) return LTFALSE;
	return LTTRUE;
}

LTBOOL CBotWorld::IsZipRideClear(const LTVector& vCenter, const LTVector& vHook, float fHalfHeight, float fRadius)
{
	return ZipRideClear(vCenter, vHook, fHalfHeight, fRadius);
}

// ----------------------------------------------------------------------- //

CBotWorld::CBotWorld()
{
	Clear();
}

void CBotWorld::Clear()
{
	m_HazardUntil.clear();
	m_eState = eIdle;
	m_szWorld[0] = '\0';
	m_Spans.clear();
	m_ColStart.clear();
	m_Links.clear();
	m_LinkStart.clear();
	m_LinkIndex.clear();
	m_Volumes.clear();
	m_Seeds.clear();
	m_Hooks.clear();
	m_nCellsX = m_nCellsZ = 0;
	m_nScanCol = 0;
	m_nMoveSpan = 0;
	m_nJumpSpan = -1;
	m_nZipLinks = m_nLadderLinks = 0;
	m_JumpTaken.clear();
	m_fScanSeconds = 0.0f;
	m_nIslands = 0;
	m_nGeoKey = 0;
	m_vMin.Init(); m_vMax.Init();

	m_Body.fRadius		= 24.0f;
	m_Body.fHalfHeight	= 53.0f;
	m_Body.fStep		= 24.0f;
	m_Body.fClimb		= 70.0f;
	m_Body.fJumpVel		= 550.0f;
	m_Body.fGravity		= 2000.0f;
	m_Body.fRunVel		= 285.0f;
	m_Body.fZipVel		= 600.0f;
}

const char* CBotWorld::GetStateName() const
{
	switch (m_eState)
	{
		case eIdle:			return "idle";
		case eCollect:		return "collecting";
		case eColumns:		return "scanning floors";
		case eMoves:		return "linking moves";
		case eSpecial:		return "zipcords/ladders/jumps";
		case eReach:		return "reachability";
		case eClearance:	return "clearance";
		case eReady:		return "ready";
	}
	return "?";
}

int CBotWorld::GetProgress() const
{
	int nCols = m_nCellsX * m_nCellsZ;
	switch (m_eState)
	{
		case eColumns:	return nCols ? 5 + 70 * m_nScanCol / nCols : 5;
		case eMoves:	return m_Spans.empty() ? 75 : 75 + 20 * m_nMoveSpan / (2 * (int)m_Spans.size());
		case eSpecial:	return (m_nJumpSpan < 0 || m_Spans.empty()) ? 95 : 95 + 3 * m_nJumpSpan / (int)m_Spans.size();
		case eReach:	return 98;
		case eClearance: return 99;
		case eReady:	return 100;
		default:		return 0;
	}
}

int CBotWorld::CellX(LTFLOAT f) const { return (int)floor((f - m_vMin.x) / kCell); }
int CBotWorld::CellZ(LTFLOAT f) const { return (int)floor((f - m_vMin.z) / kCell); }

void CBotWorld::DirVector(int d, int& dx, int& dz)
{
	dx = s_aDX[d & 7];
	dz = s_aDZ[d & 7];
}

int CBotWorld::DirIndex(const LTVector& vDir) const
{
	LTFLOAT fAngle = (LTFLOAT)atan2(vDir.z, vDir.x);	// 0 = +x, pi/2 = +z
	int d = (int)floor(fAngle / (MATH_PI / 4.0f) + 0.5f);
	return (d + 8) & 7;
}

// ----------------------------------------------------------------------- //
//	Volumes (water, ladders) straight from the map's VolumeBrush objects
// ----------------------------------------------------------------------- //

LTBOOL CBotWorld::PointInLiquid(const LTVector& vPos) const
{
	// Quick reject with the boxes, then ask the engine (brushes needn't be boxes)
	LTBOOL bMaybe = LTFALSE;
	for (size_t i = 0; i < m_Volumes.size(); i++)
	{
		const Volume& v = m_Volumes[i];
		if (!IsLiquid((ContainerCode)v.nCode)) continue;
		if (vPos.x < v.vMin.x - 1.0f || vPos.y < v.vMin.y - 1.0f || vPos.z < v.vMin.z - 1.0f) continue;
		if (vPos.x > v.vMax.x + 1.0f || vPos.y > v.vMax.y + 1.0f || vPos.z > v.vMax.z + 1.0f) continue;
		bMaybe = LTTRUE;
		break;
	}
	if (!bMaybe && !m_Volumes.empty()) return LTFALSE;

	HOBJECT objList[MAX_TRACKED_CONTAINERS];
	LTVector vPoint = vPos;
	uint32 nCount = g_pLTServer->GetPointContainers(&vPoint, objList, MAX_TRACKED_CONTAINERS);
	for (uint32 i = 0; i < nCount; i++)
	{
		uint16 nCode;
		if (g_pLTServer->GetContainerCode(objList[i], &nCode) && IsLiquid((ContainerCode)nCode)) return LTTRUE;
	}
	return LTFALSE;
}

LTBOOL CBotWorld::PointInVolume(const LTVector& vPos, int nCode) const
{
	HOBJECT objList[MAX_TRACKED_CONTAINERS];
	LTVector vPoint = vPos;
	uint32 nCount = g_pLTServer->GetPointContainers(&vPoint, objList, MAX_TRACKED_CONTAINERS);
	for (uint32 i = 0; i < nCount; i++)
	{
		uint16 nC;
		if (g_pLTServer->GetContainerCode(objList[i], &nC) && nC == nCode) return LTTRUE;
	}
	return LTFALSE;
}

// Liquid surface above (x, fFrom, z), or fCeil if the water goes up to it
LTFLOAT CBotWorld::FindLiquidTop(LTFLOAT x, LTFLOAT z, LTFLOAT fFrom, LTFLOAT fCeil) const
{
	LTFLOAT fLo = fFrom;			// known wet
	LTFLOAT fHi = fCeil;

	// Coarse climb (fast in the big reservoir: 64u steps), then bisect
	LTFLOAT y = fFrom;
	while (y + 64.0f < fCeil)
	{
		if (!PointInLiquid(LTVector(x, y + 64.0f, z))) { fHi = y + 64.0f; break; }
		y += 64.0f;
		fLo = y;
	}
	if (fHi >= fCeil && PointInLiquid(LTVector(x, fCeil - 1.0f, z))) return fCeil;

	for (int i = 0; i < 5; i++)
	{
		LTFLOAT fMid = (fLo + fHi) * 0.5f;
		if (PointInLiquid(LTVector(x, fMid, z))) fLo = fMid; else fHi = fMid;
	}
	return fLo;
}

// ----------------------------------------------------------------------- //
//	Phase 1: what's in the map
// ----------------------------------------------------------------------- //

void CBotWorld::Collect()
{
	m_Seeds.clear();
	m_Spawns.clear();
	m_Hooks.clear();
	m_Volumes.clear();

	HCLASS hStartPt = g_pLTServer->GetClass("GameStartPoint");
	HCLASS hPickup	= g_pLTServer->GetClass("PickupItem");
	HCLASS hIntel	= g_pLTServer->GetClass("Intelligence");
	HCLASS hZip		= g_pLTServer->GetClass("ZipHook");
	HCLASS hVolume	= g_pLTServer->GetClass("VolumeBrush");

	for (int nPass = 0; nPass < 2; nPass++)
	{
		HOBJECT hObj = LTNULL;
		while ((hObj = (nPass == 0 ? g_pLTServer->GetNextObject(hObj) : g_pLTServer->GetNextInactiveObject(hObj))) != LTNULL)
		{
			HCLASS hClass = g_pLTServer->GetObjectClass(hObj);
			LTVector vPos, vDims;
			g_pLTServer->GetObjectPos(hObj, &vPos);

			if ((hStartPt && g_pLTServer->IsKindOf(hClass, hStartPt)) ||
				(hPickup && g_pLTServer->IsKindOf(hClass, hPickup)) ||
				(hIntel && g_pLTServer->IsKindOf(hClass, hIntel)))
			{
				m_Seeds.push_back(vPos);
				if (hStartPt && g_pLTServer->IsKindOf(hClass, hStartPt)) m_Spawns.push_back(vPos);
			}
			else if (hZip && g_pLTServer->IsKindOf(hClass, hZip))
			{
				Hook h;
				h.vPos = vPos;
				g_pLTServer->GetObjectDims(hObj, &h.vDims);
				m_Hooks.push_back(h);
			}
			else if (hVolume && g_pLTServer->IsKindOf(hClass, hVolume))
			{
				uint16 nCode = CC_NO_CONTAINER;
				if (!g_pLTServer->GetContainerCode(hObj, &nCode)) continue;
				if (!IsLiquid((ContainerCode)nCode) && nCode != CC_LADDER) continue;
				g_pLTServer->GetObjectDims(hObj, &vDims);
				Volume v;
				v.vMin	= vPos - vDims;
				v.vMax	= vPos + vDims;
				v.nCode	= nCode;
				v.hObj	= hObj;
				m_Volumes.push_back(v);
			}
		}
	}

	// Body and movement, from the same attributes players get
	if (g_pServerButeMgr)
	{
		LTFLOAT fJump = g_pServerButeMgr->GetPlayerAttributeFloat(PLAYER_BUTE_JUMPSPEED);
		LTFLOAT fRun  = g_pServerButeMgr->GetPlayerAttributeFloat(PLAYER_BUTE_RUNSPEED);
		if (fJump > 50.0f) m_Body.fJumpVel = fJump;
		if (fRun > 50.0f)  m_Body.fRunVel = fRun;
	}
	LTVector vGravity;
	g_pLTServer->GetGlobalForce(&vGravity);
	if (vGravity.y < -100.0f) m_Body.fGravity = -vGravity.y;

	LTFLOAT fStair = 0.0f;
	if (g_pLTServer->Physics()->GetStairHeight(fStair) == LT_OK && fStair > 4.0f) m_Body.fStep = fStair;

	// Highest ledge: the jump's apex, minus a margin to get the feet over the edge
	LTFLOAT fApex = m_Body.fJumpVel * m_Body.fJumpVel / (2.0f * m_Body.fGravity);
	m_Body.fClimb = LTMAX(m_Body.fStep + 8.0f, fApex - 6.0f);

	// Scan box: around everything players use, inside the world
	g_pLTServer->GetWorldBox(m_vWorldMin, m_vWorldMax);

	LTVector vMin(1.0e9f, 1.0e9f, 1.0e9f), vMax(-1.0e9f, -1.0e9f, -1.0e9f);
	for (size_t i = 0; i < m_Seeds.size(); i++)
	{
		const LTVector& v = m_Seeds[i];
		vMin.x = LTMIN(vMin.x, v.x); vMin.y = LTMIN(vMin.y, v.y); vMin.z = LTMIN(vMin.z, v.z);
		vMax.x = LTMAX(vMax.x, v.x); vMax.y = LTMAX(vMax.y, v.y); vMax.z = LTMAX(vMax.z, v.z);
	}
	for (size_t i = 0; i < m_Hooks.size(); i++)
	{
		const LTVector& v = m_Hooks[i].vPos;
		vMin.x = LTMIN(vMin.x, v.x); vMin.z = LTMIN(vMin.z, v.z);
		vMax.x = LTMAX(vMax.x, v.x); vMax.z = LTMAX(vMax.z, v.z);
	}
	if (m_Seeds.empty())
	{
		vMin = m_vWorldMin;
		vMax = m_vWorldMax;
	}

	m_vMin.x = LTMAX(vMin.x - 1600.0f, m_vWorldMin.x);
	m_vMin.z = LTMAX(vMin.z - 1600.0f, m_vWorldMin.z);
	m_vMax.x = LTMIN(vMax.x + 1600.0f, m_vWorldMax.x);
	m_vMax.z = LTMIN(vMax.z + 1600.0f, m_vWorldMax.z);
	m_vMin.y = m_vWorldMin.y;
	m_vMax.y = m_vWorldMax.y;

	m_nCellsX = LTCLAMP((int)ceil((m_vMax.x - m_vMin.x) / kCell), 1, kMaxCells);
	m_nCellsZ = LTCLAMP((int)ceil((m_vMax.z - m_vMin.z) / kCell), 1, kMaxCells);
	m_vMax.x = m_vMin.x + m_nCellsX * kCell;
	m_vMax.z = m_vMin.z + m_nCellsZ * kCell;

	m_Spans.clear();
	m_ColStart.assign(1, 0);
	m_nScanCol = 0;

	g_pLTServer->CPrint("Bots: scanning map %dx%d cells (%d seeds, %d zip hooks, %d volumes), jump %.0f grav %.0f step %.0f climb %.0f",
		m_nCellsX, m_nCellsZ, (int)m_Seeds.size(), (int)m_Hooks.size(), (int)m_Volumes.size(),
		m_Body.fJumpVel, m_Body.fGravity, m_Body.fStep, m_Body.fClimb);
}

// ----------------------------------------------------------------------- //
//	Phase 2: floors, column by column
// ----------------------------------------------------------------------- //

void CBotWorld::ScanColumn(int x, int z, std::vector<BotSpan>& out)
{
	const LTFLOAT cx = CellCenterX(x), cz = CellCenterZ(z);
	const LTFLOAT fStand = m_Body.fHalfHeight * 2.0f + 2.0f;

	LTFLOAT y = m_vMax.y - 1.0f;
	LTFLOAT fLastFloor = 1.0e9f;
	LTFLOAT fPrevHit = 1.0e9f;		// last surface the down ray hit
	LTFLOAT fSolidAbove = 1.0e9f;	// lowest surface above the current one

	for (int nIter = 0; nIter < 48 && y > m_vMin.y + 1.0f; nIter++)
	{
		IntersectInfo info;
		if (!Ray(LTVector(cx, y, cz), LTVector(cx, m_vMin.y, cz), &info)) break;

		LTFLOAT fHit = info.m_Point.y;
		y = LTMIN(fHit, y) - 2.0f;		// always moves down

		// Whatever the down ray hit above this surface is solid, facing up or
		// not.  Rocks and terrain are one-sided: a ray going up from the floor
		// under them passes straight through, and the floor inside the rock
		// looked like open air (bots routed through it and got wedged on top).
		if (fPrevHit > fHit + 4.0f) fSolidAbove = fPrevHit;
		fPrevHit = fHit;

		if (info.m_Plane.m_Normal.y < kMinFloorNormal) continue;	// wall, slope, underside
		if (fLastFloor - fHit < 4.0f) continue;						// same surface twice

		// Headroom
		LTVector vFloor(cx, fHit + 1.0f, cz);
		IntersectInfo up;
		LTFLOAT fCeil = Ray(vFloor, vFloor + LTVector(0.0f, kHeadScan, 0.0f), &up) ? up.m_Point.y : fHit + kHeadScan;

		LTBOOL bWater = PointInLiquid(LTVector(cx, fHit + 8.0f, cz));
		if (bWater && fCeil >= fHit + kHeadScan - 1.0f)
		{
			// Open water can be deep: find the real ceiling for the surface search
			fCeil = Ray(vFloor, LTVector(cx, m_vMax.y, cz), &up) ? up.m_Point.y : m_vMax.y;
		}

		fCeil = LTMIN(fCeil, fSolidAbove);
		if (fCeil - fHit < fStand) continue;	// can't stand (or swim) here

		BotSpan s;
		memset(&s, 0, sizeof(s));
		s.fFloor	= fHit;
		s.fCeil		= fCeil;
		s.fWater	= -1.0e9f;
		s.nNode		= BOTW_INVALID;
		for (int d = 0; d < 8; d++) s.aNbr[d] = BOTW_INVALID;

		if (bWater)
		{
			s.nFlags |= BOTSPAN_WATER;
			s.fWater = FindLiquidTop(cx, cz, fHit + 8.0f, fCeil);
			if (s.fWater - fHit > m_Body.fHalfHeight * 1.4f) s.nFlags |= BOTSPAN_SWIM;
			if (s.fWater >= fCeil - 1.0f) s.nFlags |= BOTSPAN_NOAIR;
			if (!(s.nFlags & BOTSPAN_SWIM)) s.fCeil = LTMIN(s.fCeil, fHit + kHeadScan);
		}

		if (PointInVolume(LTVector(cx, fHit + m_Body.fHalfHeight, cz), CC_LADDER)) s.nFlags |= BOTSPAN_LADDER;

		out.push_back(s);
		fLastFloor = fHit;
	}
}

LTBOOL CBotWorld::StepColumns(LTFLOAT fBudgetMs)
{
	LTCounter counter;
	g_pLTServer->StartCounter(&counter);

	int nCols = m_nCellsX * m_nCellsZ;
	std::vector<BotSpan> col;

	while (m_nScanCol < nCols)
	{
		int x = m_nScanCol % m_nCellsX;
		int z = m_nScanCol / m_nCellsX;

		col.clear();
		ScanColumn(x, z, col);
		m_Spans.insert(m_Spans.end(), col.begin(), col.end());
		m_ColStart.push_back((int)m_Spans.size());
		m_nScanCol++;

		if ((m_nScanCol & 15) == 0 && g_pLTServer->EndCounter(&counter) > (uint32)(fBudgetMs * 1000.0f)) break;
	}

	return m_nScanCol >= nCols;
}

void CBotWorld::GetSpanCell(int n, int& x, int& z) const
{
	// Binary search the column
	int nCol = (int)(std::upper_bound(m_ColStart.begin(), m_ColStart.end(), n) - m_ColStart.begin()) - 1;
	x = nCol % m_nCellsX;
	z = nCol / m_nCellsX;
}

LTVector CBotWorld::GetSpanFloor(int n) const
{
	int x, z;
	GetSpanCell(n, x, z);
	return LTVector(CellCenterX(x), m_Spans[n].fFloor, CellCenterZ(z));
}

LTVector CBotWorld::GetSpanPos(int n) const
{
	const BotSpan& s = m_Spans[n];
	LTVector v = GetSpanFloor(n);

	if (s.nFlags & BOTSPAN_SWIM)
	{
		if (s.nFlags & BOTSPAN_NOAIR)
		{
			// Flooded tunnel: the middle (the body has to fit)
			v.y = LTMAX(s.fFloor + m_Body.fHalfHeight + 4.0f, (s.fFloor + s.fWater) * 0.5f);
		}
		else
		{
			// Open water: at the surface, head out
			v.y = LTMAX(s.fFloor + m_Body.fHalfHeight + 4.0f, s.fWater - 35.0f);
		}
	}
	else
	{
		v.y = s.fFloor + m_Body.fHalfHeight + 1.0f;
	}
	return v;
}

int CBotWorld::GetSpanAt(int x, int z, float fY, float fMaxBelow, float fMaxAbove) const
{
	if (!CellInside(x, z) || (int)m_ColStart.size() < m_nCellsX * m_nCellsZ + 1) return BOTW_INVALID;
	int nCol = ColumnIndex(x, z);
	int nBest = BOTW_INVALID;
	LTFLOAT fBest = 1.0e9f;
	for (int i = m_ColStart[nCol]; i < m_ColStart[nCol + 1]; i++)
	{
		LTFLOAT dy = m_Spans[i].fFloor - fY;
		if (dy > fMaxAbove || dy < -fMaxBelow) continue;
		LTFLOAT f = (LTFLOAT)fabs(dy);
		if (f < fBest) { fBest = f; nBest = i; }
	}
	return nBest;
}

int CBotWorld::FindSpan(const LTVector& vPos) const
{
	if (m_Spans.empty() || (int)m_ColStart.size() < m_nCellsX * m_nCellsZ + 1) return BOTW_INVALID;

	int x = CellX(vPos.x), z = CellZ(vPos.z);
	if (!CellInside(x, z)) return BOTW_INVALID;

	// The highest floor at or below our feet (spans are stored top down)
	LTFLOAT fFeet = vPos.y - m_Body.fHalfHeight;
	int nCol = ColumnIndex(x, z);
	for (int i = m_ColStart[nCol]; i < m_ColStart[nCol + 1]; i++)
	{
		if (m_Spans[i].fFloor <= fFeet + m_Body.fStep + 8.0f) return i;
	}
	return BOTW_INVALID;
}

int CBotWorld::FindSpanNear(const LTVector& vPos, int nRadiusCells) const
{
	if (m_Spans.empty() || (int)m_ColStart.size() < m_nCellsX * m_nCellsZ + 1) return BOTW_INVALID;

	int n = FindSpan(vPos);
	if (n != BOTW_INVALID && (m_Spans[n].nFlags & BOTSPAN_REACH) &&
		(m_Spans[n].nFlags & BOTSPAN_SWIM || vPos.y - m_Body.fHalfHeight - m_Spans[n].fFloor < 400.0f))
	{
		return n;
	}

	int cx = CellX(vPos.x), cz = CellZ(vPos.z);
	LTFLOAT fFeet = vPos.y - m_Body.fHalfHeight;
	int nBest = BOTW_INVALID;
	LTFLOAT fBest = 1.0e9f;

	for (int r = 0; r <= nRadiusCells; r++)
	{
		for (int z = cz - r; z <= cz + r; z++)
		for (int x = cx - r; x <= cx + r; x++)
		{
			if (abs(x - cx) != r && abs(z - cz) != r) continue;	// ring only
			if (!CellInside(x, z)) continue;
			int nCol = ColumnIndex(x, z);
			for (int i = m_ColStart[nCol]; i < m_ColStart[nCol + 1]; i++)
			{
				const BotSpan& s = m_Spans[i];
				if (!(s.nFlags & BOTSPAN_REACH)) continue;
				LTFLOAT dy = (LTFLOAT)fabs(s.fFloor - fFeet);
				if ((s.nFlags & BOTSPAN_SWIM) && fFeet > s.fFloor && fFeet < s.fWater) dy = 0.0f;
				LTFLOAT f = dy * 2.0f + r * kCell;
				if (f < fBest) { fBest = f; nBest = i; }
			}
		}
		if (nBest != BOTW_INVALID && fBest < (r + 1) * kCell) break;
	}
	return nBest;
}

// ----------------------------------------------------------------------- //
//	Phase 3: moves between neighbouring floors
// ----------------------------------------------------------------------- //

LTBOOL CBotWorld::TestWalk(const BotSpan& a, const LTVector& vA, const BotSpan& b, const LTVector& vB, int nDir) const
{
	const LTFLOAT fStand = m_Body.fHalfHeight * 2.0f;
	LTFLOAT fLow  = LTMAX(a.fFloor, b.fFloor);
	if (LTMIN(a.fCeil, b.fCeil) - fLow < fStand) return LTFALSE;

	// Knee (just above what we step over) and head
	LTFLOAT fKnee = m_Body.fStep + 2.0f;
	if (!RayClear(LTVector(vA.x, a.fFloor + fKnee, vA.z), LTVector(vB.x, b.fFloor + fKnee, vB.z))) return LTFALSE;
	if (!RayClear(LTVector(vA.x, fLow + fStand - 6.0f, vA.z), LTVector(vB.x, fLow + fStand - 6.0f, vB.z))) return LTFALSE;
	if (!RayClear(LTVector(vA.x, fLow + m_Body.fHalfHeight, vA.z), LTVector(vB.x, fLow + m_Body.fHalfHeight, vB.z))) return LTFALSE;
	return LTTRUE;
}

LTBOOL CBotWorld::TestDrop(const BotSpan& a, const LTVector& vA, const BotSpan& b, const LTVector& vB) const
{
	const LTFLOAT fStand = m_Body.fHalfHeight * 2.0f;

	// Walk out over the edge at our own level...
	if (!RayClear(LTVector(vA.x, a.fFloor + m_Body.fStep + 2.0f, vA.z), LTVector(vB.x, a.fFloor + m_Body.fStep + 2.0f, vB.z))) return LTFALSE;
	if (!RayClear(LTVector(vA.x, a.fFloor + fStand - 6.0f, vA.z), LTVector(vB.x, a.fFloor + fStand - 6.0f, vB.z))) return LTFALSE;

	// ...and fall straight down onto b
	if (!RayClear(LTVector(vB.x, a.fFloor + m_Body.fStep, vB.z), LTVector(vB.x, b.fFloor + 2.0f, vB.z))) return LTFALSE;
	return LTTRUE;
}

LTBOOL CBotWorld::TestClimb(const BotSpan& a, const LTVector& vA, const BotSpan& b, const LTVector& vB) const
{
	const LTFLOAT fStand = m_Body.fHalfHeight * 2.0f;
	LTFLOAT dy = b.fFloor - a.fFloor;

	// Room to jump straight up, then move over onto b
	if (a.fCeil - a.fFloor < dy + fStand) return LTFALSE;
	if (b.fCeil - b.fFloor < fStand) return LTFALSE;
	if (!RayClear(LTVector(vA.x, b.fFloor + 4.0f, vA.z), LTVector(vB.x, b.fFloor + 4.0f, vB.z))) return LTFALSE;
	if (!RayClear(LTVector(vA.x, b.fFloor + fStand - 6.0f, vA.z), LTVector(vB.x, b.fFloor + fStand - 6.0f, vB.z))) return LTFALSE;
	return LTTRUE;
}

// Hop out of the water onto b: room over the ledge for the body
LTBOOL CBotWorld::TestClimbOut(const BotSpan& b, const LTVector& vA, const LTVector& vB) const
{
	const LTFLOAT fStand = m_Body.fHalfHeight * 2.0f;
	if (b.fCeil - b.fFloor < fStand) return LTFALSE;
	if (!RayClear(LTVector(vA.x, b.fFloor + 4.0f, vA.z), LTVector(vB.x, b.fFloor + 4.0f, vB.z))) return LTFALSE;
	if (!RayClear(LTVector(vA.x, b.fFloor + fStand - 6.0f, vA.z), LTVector(vB.x, b.fFloor + fStand - 6.0f, vB.z))) return LTFALSE;
	return LTTRUE;
}

LTBOOL CBotWorld::TestSwim(const BotSpan& a, const LTVector& vA, const BotSpan& b, const LTVector& vB) const
{
	// Common stretch of water between the two columns
	LTFLOAT fLo = LTMAX(a.fFloor, b.fFloor) + m_Body.fHalfHeight;
	LTFLOAT fHi = LTMIN(a.fWater, b.fWater) - 20.0f;
	if (fHi < fLo) return LTFALSE;

	if (RayClear(LTVector(vA.x, fHi, vA.z), LTVector(vB.x, fHi, vB.z))) return LTTRUE;
	LTFLOAT fMid = (fLo + fHi) * 0.5f;
	return RayClear(LTVector(vA.x, fMid, vA.z), LTVector(vB.x, fMid, vB.z));
}

void CBotWorld::ClassifyMoves(int nSpan)
{
	// Two passes over all spans: straight directions first, then diagonals,
	// which also need both straight neighbours (no squeezing past corners).
	int nPass = (m_nMoveSpan >= (int)m_Spans.size()) ? 1 : 0;
	BotSpan& a = m_Spans[nSpan];

	int x, z;
	GetSpanCell(nSpan, x, z);
	LTVector vA(CellCenterX(x), 0.0f, CellCenterZ(z));

	const LTBOOL bASwim = (a.nFlags & BOTSPAN_SWIM) != 0;
	// Floating at the surface our "feet" are near the water line
	const LTFLOAT fAFeet = bASwim ? LTMAX(a.fFloor, a.fWater - 35.0f - m_Body.fHalfHeight) : a.fFloor;

	for (int d = nPass; d < 8; d += 2)
	{
		int nx = x + s_aDX[d], nz = z + s_aDZ[d];
		if (!CellInside(nx, nz)) continue;

		if (nPass == 1)
		{
			// Diagonal: both straight moves must exist on the same layer
			int d1 = (d + 7) & 7, d2 = (d + 1) & 7;
			if (a.GetMove(d1) != eBotMoveWalk || a.GetMove(d2) != eBotMoveWalk) continue;
		}

		LTVector vB(CellCenterX(nx), 0.0f, CellCenterZ(nz));
		int nCol = ColumnIndex(nx, nz);

		int nWalk = BOTW_INVALID, nClimb = BOTW_INVALID, nDrop = BOTW_INVALID, nSwim = BOTW_INVALID, nExit = BOTW_INVALID;
		LTFLOAT fWalk = 1.0e9f, fClimb = 1.0e9f, fDrop = -1.0e9f, fExit = 1.0e9f;

		for (int i = m_ColStart[nCol]; i < m_ColStart[nCol + 1]; i++)
		{
			const BotSpan& b = m_Spans[i];
			const LTBOOL bBSwim = (b.nFlags & BOTSPAN_SWIM) != 0;
			LTFLOAT fBFeet = bBSwim ? LTMAX(b.fFloor, b.fWater - 35.0f - m_Body.fHalfHeight) : b.fFloor;
			LTFLOAT dy = fBFeet - fAFeet;

			if (bASwim && bBSwim)
			{
				if (nSwim == BOTW_INVALID && TestSwim(a, vA, b, vB)) nSwim = i;
				continue;
			}

			// Out of the water: from the surface, onto a ledge up to about
			// a jump above the water line (players bob up and hop out)
			if (bASwim && !bBSwim)
			{
				LTFLOAT dyw = b.fFloor - a.fWater;
				// Wading floors go down to the swim threshold (1.4 x half height)
				if (dyw > m_Body.fClimb * 0.9f || dyw < -m_Body.fHalfHeight * 1.5f) continue;
				if (nExit == BOTW_INVALID || (LTFLOAT)fabs(dyw) < fExit) { fExit = (LTFLOAT)fabs(dyw); nExit = i; }
				continue;
			}

			if ((LTFLOAT)fabs(dy) <= m_Body.fStep)
			{
				if ((LTFLOAT)fabs(dy) < fWalk) { fWalk = (LTFLOAT)fabs(dy); nWalk = i; }
			}
			else if (dy > 0.0f && dy <= m_Body.fClimb + (bASwim ? 30.0f : 0.0f))
			{
				if (dy < fClimb) { fClimb = dy; nClimb = i; }
			}
			else if (dy < 0.0f && !bASwim)
			{
				// The highest floor below us (the first one we'd land on)
				if (b.fFloor > fDrop && b.fFloor < a.fFloor) { fDrop = b.fFloor; nDrop = i; }
			}
		}

		int nMove = eBotMoveNone, nTo = BOTW_INVALID;
		if (nSwim != BOTW_INVALID)
		{
			nMove = eBotMoveWalk; nTo = nSwim;
		}
		else if (nExit != BOTW_INVALID && TestClimbOut(m_Spans[nExit], vA, vB))
		{
			LTFLOAT dyw = m_Spans[nExit].fFloor - a.fWater;
			nMove = (dyw <= m_Body.fStep) ? eBotMoveWalk : eBotMoveClimb;
			if (nPass == 1 && nMove != eBotMoveWalk) nMove = eBotMoveNone;
			nTo = (nMove != eBotMoveNone) ? nExit : BOTW_INVALID;
		}
		else if (nWalk != BOTW_INVALID && TestWalk(a, vA, m_Spans[nWalk], vB, d))
		{
			nMove = eBotMoveWalk; nTo = nWalk;
		}
		else if (nPass == 0 && nClimb != BOTW_INVALID && TestClimb(a, vA, m_Spans[nClimb], vB))
		{
			nMove = eBotMoveClimb; nTo = nClimb;
		}
		else if (nPass == 0 && nDrop != BOTW_INVALID && TestDrop(a, vA, m_Spans[nDrop], vB))
		{
			nMove = eBotMoveDrop; nTo = nDrop;
		}

		a.SetMove(d, nMove);
		a.aNbr[d] = nTo;
	}
}

LTBOOL CBotWorld::StepMoves(LTFLOAT fBudgetMs)
{
	LTCounter counter;
	g_pLTServer->StartCounter(&counter);

	int nSpans = (int)m_Spans.size();
	while (m_nMoveSpan < 2 * nSpans)
	{
		ClassifyMoves(m_nMoveSpan % nSpans);
		m_nMoveSpan++;
		if ((m_nMoveSpan & 63) == 0 && g_pLTServer->EndCounter(&counter) > (uint32)(fBudgetMs * 1000.0f)) break;
	}
	return m_nMoveSpan >= 2 * nSpans;
}

// ----------------------------------------------------------------------- //
//	Phase 4: long moves
// ----------------------------------------------------------------------- //

int CBotWorld::AddLink(int nFrom, int nTo, int nType, const LTVector& vAux, LTFLOAT fCost)
{
	BotWorldLink l;
	l.nFrom	= nFrom;
	l.nTo	= nTo;
	l.nType	= (uint8)nType;
	l.vAux	= vAux;
	l.fCost	= fCost;
	l.nFails = 0;
	m_Links.push_back(l);
	return (int)m_Links.size() - 1;
}

void CBotWorld::FindZipLinks()
{
	const LTFLOAT fStand = m_Body.fHalfHeight * 2.0f;

	for (size_t h = 0; h < m_Hooks.size(); h++)
	{
		const LTVector vHook = m_Hooks[h].vPos;
		int hx = CellX(vHook.x), hz = CellZ(vHook.z);

		// Landing: a floor up by the hook we can step onto from it (prefer
		// roomy spots a bit below the hook, close to it)
		int nLand = BOTW_INVALID;
		LTFLOAT fBest = 1.0e9f;
		const int R = 9;
		for (int z = hz - R; z <= hz + R; z++)
		for (int x = hx - R; x <= hx + R; x++)
		{
			if (!CellInside(x, z)) continue;
			int nCol = ColumnIndex(x, z);
			for (int i = m_ColStart[nCol]; i < m_ColStart[nCol + 1]; i++)
			{
				const BotSpan& s = m_Spans[i];
				if (s.nFlags & BOTSPAN_SWIM) continue;
				LTFLOAT dy = vHook.y - s.fFloor;
				if (dy < -10.0f || dy > 300.0f) continue;
				LTFLOAT dx = CellCenterX(x) - vHook.x, dz = CellCenterZ(z) - vHook.z;
				LTFLOAT fHoriz = (LTFLOAT)sqrt(dx*dx + dz*dz);
				LTFLOAT fScore = fHoriz + (LTFLOAT)fabs(dy - 90.0f) * 0.5f;
				// Not right at the edge (we'd fall back off)
				int nOpen = 0;
				for (int d = 0; d < 8; d++) if (s.GetMove(d) == eBotMoveWalk) nOpen++;
				fScore += (8 - nOpen) * 25.0f + (s.nClear < 2 ? 90.0f : 0.0f);
				if (fScore >= fBest) continue;

				LTVector vSpot(CellCenterX(x), s.fFloor + m_Body.fHalfHeight, CellCenterZ(z));
				// From the hook: over to the spot at hook height, then down
				LTVector vOver(vSpot.x, LTMAX(vHook.y, vSpot.y), vSpot.z);
				if (!RayClear(vHook, vOver) || !RayClear(vOver, vSpot)) continue;

				fBest = fScore;
				nLand = i;
			}
		}
		if (nLand == BOTW_INVALID) continue;

		// Take-off: floors well below the landing that can see the hook and
		// have a clear line for the whole body
		const BotSpan& land = m_Spans[nLand];
		std::vector< std::pair<LTFLOAT, int> > cands;
		const int RR = (int)(kZipRange / kCell);
		for (int z = hz - RR; z <= hz + RR; z += 2)
		for (int x = hx - RR; x <= hx + RR; x += 2)
		{
			if (!CellInside(x, z)) continue;
			LTFLOAT dx = CellCenterX(x) - vHook.x, dz = CellCenterZ(z) - vHook.z;
			LTFLOAT fHoriz2 = dx*dx + dz*dz;
			int nCol = ColumnIndex(x, z);
			for (int i = m_ColStart[nCol]; i < m_ColStart[nCol + 1]; i++)
			{
				const BotSpan& s = m_Spans[i];
				if (s.nFlags & (BOTSPAN_SWIM | BOTSPAN_LADDER)) continue;
				if (s.fFloor > land.fFloor - 120.0f) continue;
				// Not in a doorway or against a wall: out in the open, where
				// a player stands to fire the cord
				if (s.nClear < 2 || (s.nFlags & BOTSPAN_TIGHT)) continue;
				LTFLOAT dy = vHook.y - s.fFloor;
				LTFLOAT fDist2 = fHoriz2 + dy*dy;
				if (fDist2 > kZipRange * kZipRange) continue;
				cands.push_back(std::make_pair(fDist2, i));
			}
		}
		std::sort(cands.begin(), cands.end());

		std::vector<LTVector> chosen;
		for (size_t c = 0; c < cands.size() && chosen.size() < 6; c++)
		{
			int n = cands[c].second;
			LTVector vFloor = GetSpanFloor(n);

			LTBOOL bNear = LTFALSE;
			for (size_t k = 0; k < chosen.size(); k++) if (chosen[k].DistSqr(vFloor) < 200.0f*200.0f) { bNear = LTTRUE; break; }
			if (bNear) continue;

			// Eye to hook, and the body (feet and head) along the ride
			LTVector vCenter = vFloor + LTVector(0.0f, m_Body.fHalfHeight, 0.0f);
			if (!RayClear(vFloor + LTVector(0.0f, fStand - 8.0f, 0.0f), vHook)) continue;
			if (!ZipRideClear(vCenter, vHook, m_Body.fHalfHeight, m_Body.fRadius)) continue;

			LTFLOAT fRide = vCenter.Dist(vHook) / LTMAX(m_Body.fZipVel, 200.0f);
			AddLink(n, nLand, eBotLinkZip, vHook, (fRide + 1.5f) * m_Body.fRunVel);
			chosen.push_back(vFloor);
		}
	}
}

void CBotWorld::FindLadderLinks()
{
	for (size_t v = 0; v < m_Volumes.size(); v++)
	{
		const Volume& vol = m_Volumes[v];
		if (vol.nCode != CC_LADDER) continue;

		LTVector vC = (vol.vMin + vol.vMax) * 0.5f;
		int x0 = CellX(vol.vMin.x - 96.0f), x1 = CellX(vol.vMax.x + 96.0f);
		int z0 = CellZ(vol.vMin.z - 96.0f), z1 = CellZ(vol.vMax.z + 96.0f);

		int nBottom = BOTW_INVALID, nTop = BOTW_INVALID;
		LTFLOAT fBottom = 1.0e9f, fTop = 1.0e9f;
		for (int z = z0; z <= z1; z++)
		for (int x = x0; x <= x1; x++)
		{
			if (!CellInside(x, z)) continue;
			LTFLOAT dx = CellCenterX(x) - vC.x, dz = CellCenterZ(z) - vC.z;
			LTFLOAT fHoriz = (LTFLOAT)sqrt(dx*dx + dz*dz);
			LTBOOL bInside = CellCenterX(x) >= vol.vMin.x && CellCenterX(x) <= vol.vMax.x &&
							 CellCenterZ(z) >= vol.vMin.z && CellCenterZ(z) <= vol.vMax.z;
			int nCol = ColumnIndex(x, z);
			for (int i = m_ColStart[nCol]; i < m_ColStart[nCol + 1]; i++)
			{
				const BotSpan& s = m_Spans[i];
				if (s.nFlags & BOTSPAN_SWIM) continue;
				if (s.fFloor > vol.vMin.y - 80.0f && s.fFloor < vol.vMin.y + 80.0f && fHoriz < fBottom)
				{
					fBottom = fHoriz; nBottom = i;
				}
				// The top exit is off the ladder, onto the floor up there
				if (s.fFloor > vol.vMax.y - 160.0f && s.fFloor < vol.vMax.y + 40.0f)
				{
					LTFLOAT f = fHoriz + (bInside ? 80.0f : 0.0f);
					if (f < fTop) { fTop = f; nTop = i; }
				}
			}
		}

		if (nBottom == BOTW_INVALID || nTop == BOTW_INVALID || nBottom == nTop) continue;
		if (m_Spans[nTop].fFloor - m_Spans[nBottom].fFloor < m_Body.fClimb) continue;

		LTFLOAT fCost = (m_Spans[nTop].fFloor - m_Spans[nBottom].fFloor) / 150.0f * m_Body.fRunVel + 100.0f;
		AddLink(nBottom, nTop, eBotLinkLadder, vC, fCost);
		AddLink(nTop, nBottom, eBotLinkLadder, vC, fCost * 0.7f);
	}
}

// A running jump from vFrom (floor point) to vTo (floor point): does the body
// fly over everything in between?
LTBOOL CBotWorld::TestJumpArc(const LTVector& vFrom, const LTVector& vTo, LTFLOAT fHorizSpeed) const
{
	LTVector vD = vTo - vFrom;
	LTFLOAT dy = vD.y;
	vD.y = 0.0f;
	LTFLOAT fDist = vD.Mag();
	if (fDist < 1.0f) return LTFALSE;
	LTVector vDir = vD / fDist;

	LTFLOAT T = fDist / fHorizSpeed;
	LTFLOAT g = m_Body.fGravity, v0 = m_Body.fJumpVel;
	if (v0 * T - 0.5f * g * T * T < dy + 8.0f) return LTFALSE;		// can't get that high / far

	const LTFLOAT fStand = m_Body.fHalfHeight * 2.0f;
	LTVector vPrev = vFrom;
	const int nSteps = 8;
	for (int i = 1; i <= nSteps; i++)
	{
		LTFLOAT t = T * (LTFLOAT)i / nSteps;
		LTVector vP = vFrom + vDir * (fHorizSpeed * t);
		vP.y = vFrom.y + v0 * t - 0.5f * g * t * t;
		if (i == nSteps) vP.y = LTMAX(vP.y, vTo.y + 2.0f);

		if (!RayClear(vPrev + LTVector(0.0f, 6.0f, 0.0f), vP + LTVector(0.0f, 6.0f, 0.0f))) return LTFALSE;
		if (!RayClear(vPrev + LTVector(0.0f, fStand - 6.0f, 0.0f), vP + LTVector(0.0f, fStand - 6.0f, 0.0f))) return LTFALSE;
		vPrev = vP;
	}
	return LTTRUE;
}

// Bounded search: is nTo reachable from nFrom by plain moves within fMaxLen?
LTBOOL CBotWorld::IsWalkReachable(int nFrom, int nTo, int nMaxVisit, LTFLOAT fMaxLen) const
{
	std::vector< std::pair<int, LTFLOAT> > queue;
	std::set<int> seen;
	queue.push_back(std::make_pair(nFrom, 0.0f));
	seen.insert(nFrom);

	for (size_t q = 0; q < queue.size() && (int)q < nMaxVisit; q++)
	{
		int n = queue[q].first;
		LTFLOAT fLen = queue[q].second;
		if (n == nTo) return LTTRUE;
		const BotSpan& s = m_Spans[n];
		for (int d = 0; d < 8; d++)
		{
			if (s.GetMove(d) == eBotMoveNone) continue;
			int m = s.aNbr[d];
			if (m < 0) continue;
			LTFLOAT fNext = fLen + ((d & 1) ? kCell * 1.414f : kCell);
			if (fNext > fMaxLen) continue;
			if (!seen.insert(m).second) continue;
			queue.push_back(std::make_pair(m, fNext));
		}
	}
	return LTFALSE;
}

LTBOOL CBotWorld::StepJumpLinks(LTFLOAT fBudgetMs)
{
	LTCounter counter;
	g_pLTServer->StartCounter(&counter);

	// How far a running jump carries us, landing at the same height
	LTFLOAT fRun = m_Body.fRunVel;
	LTFLOAT fAir = 2.0f * m_Body.fJumpVel / m_Body.fGravity;
	int K = (int)(fRun * fAir / kCell);
	if (K < 2) return LTTRUE;
	if (K > 10) K = 10;

	std::vector<JumpTaken>& taken = m_JumpTaken;
	const int nOther = m_nZipLinks + m_nLadderLinks;

	int nSpans = (int)m_Spans.size();
	for (; m_nJumpSpan < nSpans && (int)m_Links.size() - nOther < kMaxJumpLinks; m_nJumpSpan++)
	{
		if ((m_nJumpSpan & 63) == 0 && g_pLTServer->EndCounter(&counter) > (uint32)(fBudgetMs * 1000.0f)) return LTFALSE;

		const int n = m_nJumpSpan;
		const BotSpan& a = m_Spans[n];
		if (a.nFlags & (BOTSPAN_SWIM | BOTSPAN_LADDER)) continue;

		int x, z;
		GetSpanCell(n, x, z);

		for (int d = 0; d < 8; d++)
		{
			// Only at the edge of a ledge, and with a run-up behind us
			int nMove = a.GetMove(d);
			if (nMove == eBotMoveWalk || nMove == eBotMoveClimb) continue;
			if (a.GetMove((d + 4) & 7) != eBotMoveWalk) continue;

			// Air in front at our level (not a wall)
			int fx = x + s_aDX[d], fz = z + s_aDZ[d];
			if (!CellInside(fx, fz)) continue;

			for (int k = 2; k <= K; k++)
			{
				int tx = x + s_aDX[d] * k, tz = z + s_aDZ[d] * k;
				if (!CellInside(tx, tz)) break;

				// Nothing to stand on in between at about our level (a real gap)
				LTBOOL bGap = LTTRUE;
				for (int j = 1; j < k && bGap; j++)
				{
					int jc = ColumnIndex(x + s_aDX[d] * j, z + s_aDZ[d] * j);
					for (int i = m_ColStart[jc]; i < m_ColStart[jc + 1]; i++)
					{
						LTFLOAT dy = m_Spans[i].fFloor - a.fFloor;
						if (dy > -m_Body.fStep * 2.0f && dy < m_Body.fHalfHeight * 2.0f) { bGap = LTFALSE; break; }
					}
				}
				if (!bGap) break;

				LTFLOAT fHoriz = k * kCell * ((d & 1) ? 1.414f : 1.0f);
				int tc = ColumnIndex(tx, tz);
				int nLand = BOTW_INVALID;
				for (int i = m_ColStart[tc]; i < m_ColStart[tc + 1]; i++)
				{
					const BotSpan& b = m_Spans[i];
					if (b.nFlags & BOTSPAN_SWIM) continue;
					LTFLOAT dy = b.fFloor - a.fFloor;
					if (dy > m_Body.fClimb || dy < -m_Body.fHalfHeight * 3.0f) continue;
					// Land in the open, not on the lip
					if (b.GetMove(d) == eBotMoveNone && b.GetMove((d + 4) & 7) == eBotMoveNone) continue;
					nLand = i;
					break;
				}
				if (nLand == BOTW_INVALID) continue;

				LTVector vFrom = GetSpanFloor(n), vTo = GetSpanFloor(nLand);
				LTBOOL bDup = LTFALSE;
				for (size_t t = 0; t < taken.size(); t++)
				{
					if (taken[t].vFrom.DistSqr(vFrom) < 96.0f*96.0f && taken[t].vTo.DistSqr(vTo) < 128.0f*128.0f) { bDup = LTTRUE; break; }
				}
				if (bDup) break;

				// Already reachable on foot with a short detour: not worth a jump
				if (IsWalkReachable(n, nLand, 1500, fHoriz * 3.0f + 300.0f)) break;
				if (!TestJumpArc(vFrom, vTo, fRun * 0.9f)) continue;

				AddLink(n, nLand, eBotLinkJump, vFrom, fHoriz + 120.0f);
				JumpTaken tk; tk.vFrom = vFrom; tk.vTo = vTo;
				taken.push_back(tk);
				break;
			}
		}
	}
	return LTTRUE;
}

LTBOOL CBotWorld::StepSpecial(LTFLOAT fBudgetMs)
{
	if (m_nJumpSpan < 0)
	{
		m_Links.clear();
		FindZipLinks();
		m_nZipLinks = (int)m_Links.size();
		FindLadderLinks();
		m_nLadderLinks = (int)m_Links.size() - m_nZipLinks;
		m_JumpTaken.clear();
		m_nJumpSpan = 0;
		return LTFALSE;
	}

	if (!StepJumpLinks(fBudgetMs)) return LTFALSE;

	int nJump = (int)m_Links.size() - m_nZipLinks - m_nLadderLinks;
	m_JumpTaken.clear();
	BuildLinkIndex();

	g_pLTServer->CPrint("Bots: map links: %d zipcord, %d ladder, %d jump", m_nZipLinks, m_nLadderLinks, nJump);
	return LTTRUE;
}

void CBotWorld::BuildLinkIndex()
{
	int nSpans = (int)m_Spans.size();
	m_LinkStart.assign(nSpans + 1, 0);
	for (size_t i = 0; i < m_Links.size(); i++) m_LinkStart[m_Links[i].nFrom + 1]++;
	for (int i = 0; i < nSpans; i++) m_LinkStart[i + 1] += m_LinkStart[i];
	m_LinkIndex.assign(m_Links.size(), 0);
	std::vector<int> fill(m_LinkStart.begin(), m_LinkStart.end() - 1);
	for (size_t i = 0; i < m_Links.size(); i++) m_LinkIndex[fill[m_Links[i].nFrom]++] = (int)i;
}

void CBotWorld::GetLinksFrom(int nSpan, std::vector<int>& links) const
{
	links.clear();
	if (nSpan < 0 || nSpan + 1 >= (int)m_LinkStart.size()) return;
	for (int i = m_LinkStart[nSpan]; i < m_LinkStart[nSpan + 1]; i++) links.push_back(m_LinkIndex[i]);
}

int CBotWorld::FindLink(int nFrom, int nTo) const
{
	if (nFrom < 0 || nFrom + 1 >= (int)m_LinkStart.size()) return BOTW_INVALID;
	for (int i = m_LinkStart[nFrom]; i < m_LinkStart[nFrom + 1]; i++)
	{
		if (m_Links[m_LinkIndex[i]].nTo == nTo) return m_LinkIndex[i];
	}
	return BOTW_INVALID;
}

int CBotWorld::CountLinks(int nType) const
{
	int n = 0;
	for (size_t i = 0; i < m_Links.size(); i++) if (m_Links[i].nType == nType) n++;
	return n;
}

// ----------------------------------------------------------------------- //
//	Phase 5: what players can actually get to
// ----------------------------------------------------------------------- //

void CBotWorld::ComputeReach()
{
	int nSpans = (int)m_Spans.size();
	for (int i = 0; i < nSpans; i++) m_Spans[i].nFlags &= ~BOTSPAN_REACH;

	// Islands: each seed that isn't reached yet starts a new one
	std::vector<int> queue;
	queue.reserve(nSpans);
	m_nIslands = 0;

	for (size_t s = 0; s < m_Seeds.size(); s++)
	{
		int x = CellX(m_Seeds[s].x), z = CellZ(m_Seeds[s].z);
		if (!CellInside(x, z)) continue;

		// The floor under the seed (seeds float a little above it)
		int nSeed = BOTW_INVALID;
		int nCol = ColumnIndex(x, z);
		for (int i = m_ColStart[nCol]; i < m_ColStart[nCol + 1]; i++)
		{
			if (m_Spans[i].fFloor <= m_Seeds[s].y + 40.0f) { nSeed = i; break; }
		}
		if (nSeed == BOTW_INVALID || (m_Spans[nSeed].nFlags & BOTSPAN_REACH)) continue;

		m_nIslands++;
		queue.clear();
		queue.push_back(nSeed);
		m_Spans[nSeed].nFlags |= BOTSPAN_REACH;

		for (size_t q = 0; q < queue.size(); q++)
		{
			int n = queue[q];
			const BotSpan& sp = m_Spans[n];
			for (int d = 0; d < 8; d++)
			{
				int m = sp.aNbr[d];
				if (m < 0 || sp.GetMove(d) == eBotMoveNone) continue;
				if (m_Spans[m].nFlags & BOTSPAN_REACH) continue;
				m_Spans[m].nFlags |= BOTSPAN_REACH;
				queue.push_back(m);
			}
			for (int i = m_LinkStart[n]; i < m_LinkStart[n + 1]; i++)
			{
				int m = m_Links[m_LinkIndex[i]].nTo;
				if (m_Spans[m].nFlags & BOTSPAN_REACH) continue;
				m_Spans[m].nFlags |= BOTSPAN_REACH;
				queue.push_back(m);
			}
		}
	}
}

// Distance (in cells) to the nearest wall or ledge, for keeping paths off them
void CBotWorld::ComputeClearance()
{
	int nSpans = (int)m_Spans.size();
	std::vector<int> queue;
	queue.reserve(nSpans);

	for (int i = 0; i < nSpans; i++)
	{
		BotSpan& s = m_Spans[i];
		s.nFlags &= ~(BOTSPAN_TIGHT | BOTSPAN_LEDGE);
		s.nClear = 7;
		LTBOOL bEdge = LTFALSE;
		for (int d = 0; d < 8; d += 2)
		{
			if (s.GetMove(d) != eBotMoveWalk) bEdge = LTTRUE;
		}
		for (int d = 0; d < 8; d++)
		{
			if (s.GetMove(d) == eBotMoveDrop) s.nFlags |= BOTSPAN_LEDGE;
		}
		if (bEdge)
		{
			s.nClear = 0;
			if (!(s.nFlags & BOTSPAN_SWIM)) s.nFlags |= BOTSPAN_TIGHT;
			queue.push_back(i);
		}
	}

	for (size_t q = 0; q < queue.size(); q++)
	{
		int n = queue[q];
		const BotSpan& s = m_Spans[n];
		if (s.nClear >= 6) continue;
		for (int d = 0; d < 8; d += 2)
		{
			int m = s.aNbr[d];
			if (m < 0 || s.GetMove(d) != eBotMoveWalk) continue;
			if (m_Spans[m].nClear <= s.nClear + 1) continue;
			m_Spans[m].nClear = (uint8)(s.nClear + 1);
			queue.push_back(m);
		}
	}
}

// ----------------------------------------------------------------------- //
//	Driving the phases
// ----------------------------------------------------------------------- //

void CBotWorld::Begin(const char* szWorld, LTBOOL bRescan)
{
	Clear();
	if (!szWorld || !szWorld[0]) return;
	SAFE_STRCPY(m_szWorld, szWorld);

	m_fScanStart = g_pLTServer->GetTime();
	Collect();
	m_nGeoKey = ProbeGeometry();

	if (!bRescan && LoadCache())
	{
		m_eState = eReady;
		g_pLTServer->CPrint("Bots: map knowledge loaded (%d floor cells, %d links)", (int)m_Spans.size(), (int)m_Links.size());
		return;
	}

	m_eState = eColumns;
}

LTBOOL CBotWorld::Step(LTFLOAT fBudgetMs)
{
	switch (m_eState)
	{
		case eColumns:
			if (StepColumns(fBudgetMs))
			{
				m_nMoveSpan = 0;
				m_eState = eMoves;
				g_pLTServer->CPrint("Bots: %d floor cells found", (int)m_Spans.size());
			}
			return LTFALSE;

		case eMoves:
			if (m_Spans.empty() || StepMoves(fBudgetMs)) m_eState = eClearance;
			return LTFALSE;

		case eClearance:
			ComputeClearance();		// (zipcord landings want room around them)
			m_nJumpSpan = -1;
			m_eState = eSpecial;
			return LTFALSE;

		case eSpecial:
			if (StepSpecial(fBudgetMs)) m_eState = eReach;
			return LTFALSE;

		case eReach:
			ComputeReach();
			m_fScanSeconds = g_pLTServer->GetTime() - m_fScanStart;
			m_eState = eReady;
			SaveCache();
			g_pLTServer->CPrint("Bots: map scan done in %.0fs: %d cells, %d reachable, %d water, %d islands",
				m_fScanSeconds, (int)m_Spans.size(), CountSpans(BOTSPAN_REACH), CountSpans(BOTSPAN_WATER), m_nIslands);
			return LTTRUE;

		case eReady:
			return LTTRUE;

		default:
			return LTFALSE;
	}
}

int CBotWorld::CountSpans(uint8 nFlag) const
{
	int n = 0;
	for (size_t i = 0; i < m_Spans.size(); i++) if (m_Spans[i].nFlags & nFlag) n++;
	return n;
}

// ----------------------------------------------------------------------- //
//	Queries used while playing
// ----------------------------------------------------------------------- //

LTBOOL CBotWorld::CanWalkStraight(int nFrom, int nTo, LTBOOL bAllowTight) const
{
	if (nFrom < 0 || nTo < 0) return LTFALSE;
	if (nFrom == nTo) return LTTRUE;

	int x0, z0, x1, z1;
	GetSpanCell(nFrom, x0, z0);
	GetSpanCell(nTo, x1, z1);

	int dx = x1 - x0, dz = z1 - z0;
	int nSteps = LTMAX(abs(dx), abs(dz));
	if (nSteps > 40) return LTFALSE;

	// Walk the line through the grid (supercover-ish: diagonal steps need
	// both straight moves, which the diagonal move already guarantees)
	int nCur = nFrom;
	int cx = x0, cz = z0;
	LTFLOAT fx = (LTFLOAT)x0, fz = (LTFLOAT)z0;
	LTFLOAT sx = (LTFLOAT)dx / nSteps, sz = (LTFLOAT)dz / nSteps;

	for (int i = 1; i <= nSteps; i++)
	{
		fx += sx; fz += sz;
		int nx = (int)floor(fx + 0.5f), nz = (int)floor(fz + 0.5f);
		int ddx = nx - cx, ddz = nz - cz;
		if (ddx == 0 && ddz == 0) continue;

		int d = -1;
		for (int k = 0; k < 8; k++) if (s_aDX[k] == ddx && s_aDZ[k] == ddz) { d = k; break; }
		if (d < 0) return LTFALSE;

		const BotSpan& s = m_Spans[nCur];
		if (s.GetMove(d) != eBotMoveWalk) return LTFALSE;
		int nNext = s.aNbr[d];
		if (nNext < 0) return LTFALSE;
		if (!bAllowTight && nNext != nTo && (m_Spans[nNext].nFlags & BOTSPAN_TIGHT)) return LTFALSE;

		nCur = nNext;
		cx = nx; cz = nz;
	}

	return nCur == nTo;
}

LTBOOL CBotWorld::FindDiveColumn(const LTVector& vNear, LTFLOAT yLo, LTFLOAT yHi, int nRadius, LTVector& vDive) const
{
	if ((int)m_ColStart.size() < m_nCellsX * m_nCellsZ + 1) return LTFALSE;

	// Is this column water from yLo (body bottom clear of the floor) up to yHi?
	struct Open
	{
		static LTBOOL Col(const CBotWorld& w, int x, int z, LTFLOAT yLo, LTFLOAT yHi)
		{
			if (!w.CellInside(x, z)) return LTFALSE;
			int c = w.ColumnIndex(x, z);
			for (int i = w.m_ColStart[c]; i < w.m_ColStart[c + 1]; i++)
			{
				const BotSpan& s = w.m_Spans[i];
				if (!(s.nFlags & BOTSPAN_SWIM)) continue;
				if (s.fFloor <= yLo - w.m_Body.fHalfHeight - 4.0f && s.fWater >= yHi - 40.0f) return LTTRUE;
			}
			return LTFALSE;
		}
	};

	int cx = CellX(vNear.x), cz = CellZ(vNear.z);
	LTFLOAT fBest = 1.0e9f;
	LTBOOL bFound = LTFALSE;
	for (int z = cz - nRadius; z <= cz + nRadius; z++)
	for (int x = cx - nRadius; x <= cx + nRadius; x++)
	{
		LTFLOAT dx = CellCenterX(x) - vNear.x, dz = CellCenterZ(z) - vNear.z;
		LTFLOAT d = dx*dx + dz*dz;
		if (d >= fBest) continue;
		LTBOOL bOk = Open::Col(*this, x, z, yLo, yHi);
		for (int k = 0; k < 8 && bOk; k++) bOk = Open::Col(*this, x + s_aDX[k], z + s_aDZ[k], yLo, yHi);
		if (!bOk) continue;
		fBest = d;
		vDive = LTVector(CellCenterX(x), vNear.y, CellCenterZ(z));
		bFound = LTTRUE;
	}
	return bFound;
}

LTBOOL CBotWorld::CanWalkStraight(const LTVector& vFrom, const LTVector& vTo) const
{
	int a = FindSpan(vFrom), b = FindSpan(vTo);
	if (a < 0 || b < 0) return LTFALSE;
	return CanWalkStraight(a, b, LTTRUE);
}

void CBotWorld::MarkHazard(int nSpan, LTFLOAT fUntil)
{
	if (nSpan < 0 || nSpan >= (int)m_Spans.size()) return;
	if (m_HazardUntil.size() != m_Spans.size()) m_HazardUntil.assign(m_Spans.size(), 0.0f);
	// The cell and the ones around it (the lip, the gap beside a body)
	m_HazardUntil[nSpan] = LTMAX(m_HazardUntil[nSpan], fUntil);
	for (int d = 0; d < 8; d++)
	{
		int m = m_Spans[nSpan].aNbr[d];
		if (m >= 0) m_HazardUntil[m] = LTMAX(m_HazardUntil[m], fUntil);
	}
}

LTBOOL CBotWorld::IsHazard(int nSpan) const
{
	return nSpan >= 0 && nSpan < (int)m_HazardUntil.size() && m_HazardUntil[nSpan] > g_pLTServer->GetTime();
}

LTBOOL CBotWorld::FindSpanPath(int nFrom, int nTo, const std::vector<char>* pAllowed, std::vector<int>& path, int nMaxVisit) const
{
	path.clear();
	int N = (int)m_Spans.size();
	if (nFrom < 0 || nTo < 0 || nFrom >= N || nTo >= N) return LTFALSE;
	if (nFrom == nTo) { path.push_back(nTo); return LTTRUE; }

	// Sparse bookkeeping: paths only touch a corridor of the map
	struct Rec { LTFLOAT g; int parent; char closed; };
	std::map<int, Rec> rec;
	typedef std::pair<LTFLOAT, int> Entry;
	std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry> > open;

	LTVector vGoal = GetSpanFloor(nTo);
	Rec r0; r0.g = 0.0f; r0.parent = -1; r0.closed = 0;
	rec[nFrom] = r0;
	open.push(Entry(GetSpanFloor(nFrom).Dist(vGoal), nFrom));

	int nVisit = 0;
	while (!open.empty() && nVisit < nMaxVisit)
	{
		int n = open.top().second;
		open.pop();
		Rec& rn = rec[n];
		if (rn.closed) continue;
		rn.closed = 1;
		nVisit++;

		if (n == nTo)
		{
			for (int m = nTo; m != nFrom; m = rec[m].parent) path.push_back(m);
			std::reverse(path.begin(), path.end());
			return LTTRUE;
		}

		const BotSpan& s = m_Spans[n];
		LTFLOAT g = rn.g;

		for (int k = 0; k < 8 + (m_LinkStart[n + 1] - m_LinkStart[n]); k++)
		{
			int m;
			LTFLOAT fCost;
			if (k < 8)
			{
				int nMove = s.GetMove(k);
				m = s.aNbr[k];
				if (nMove == eBotMoveNone || m < 0) continue;
				fCost = (k & 1) ? kCell * 1.414f : kCell;
				if (nMove == eBotMoveClimb) fCost += 60.0f;
				if (nMove == eBotMoveDrop)  fCost += 10.0f;
			}
			else
			{
				const BotWorldLink& l = m_Links[m_LinkIndex[m_LinkStart[n] + k - 8]];
				if (l.nFails >= 3) continue;
				m = l.nTo;
				fCost = l.fCost;
			}

			const BotSpan& t = m_Spans[m];
			if (pAllowed && (t.nNode < 0 || t.nNode >= (int)pAllowed->size() || !(*pAllowed)[t.nNode])) continue;

			// Keep off walls and ledges where there's room; swimming is slow
			if (t.nFlags & BOTSPAN_TIGHT) fCost *= 1.8f;
			else if (t.nClear == 1) fCost *= 1.2f;
			if (t.nFlags & BOTSPAN_SWIM) fCost *= 1.5f;
			if (IsHazard(m)) fCost += 600.0f;		// someone got pinned here lately

			LTFLOAT gNew = g + fCost;
			std::map<int, Rec>::iterator it = rec.find(m);
			if (it != rec.end() && (it->second.closed || it->second.g <= gNew)) continue;

			Rec rm; rm.g = gNew; rm.parent = n; rm.closed = 0;
			rec[m] = rm;
			open.push(Entry(gNew + GetSpanFloor(m).Dist(vGoal), m));
		}
	}

	return LTFALSE;
}

// ----------------------------------------------------------------------- //
//	Cache: Bots\<map>.bwm
// ----------------------------------------------------------------------- //

void CBotWorld::MakeCacheName(char* szBuf, int nLen) const
{
	const char* pBase = m_szWorld;
	for (const char* p = m_szWorld; *p; p++) if (*p == '\\' || *p == '/') pBase = p + 1;
	_snprintf(szBuf, nLen, "Bots\\%s", pBase);
	szBuf[nLen - 1] = '\0';
	char* pExt = strrchr(szBuf, '.');
	if (pExt && _stricmp(pExt, ".dat") == 0) *pExt = '\0';
	strncat(szBuf, ".bwm", nLen - strlen(szBuf) - 1);
}

uint32 CBotWorld::WorldKey() const
{
	// Changes when the world (its box) or what's placed in it changes
	uint32 h = 2166136261u;
	const LTFLOAT af[] = { m_vWorldMin.x, m_vWorldMin.y, m_vWorldMin.z, m_vWorldMax.x, m_vWorldMax.y, m_vWorldMax.z };
	for (int i = 0; i < 6; i++) { h ^= (uint32)(int)af[i]; h *= 16777619u; }
	h ^= (uint32)m_Seeds.size(); h *= 16777619u;
	h ^= (uint32)m_Hooks.size(); h *= 16777619u;
	h ^= (uint32)m_Volumes.size(); h *= 16777619u;
	for (size_t i = 0; i < m_Hooks.size(); i++) { h ^= (uint32)(int)(m_Hooks[i].vPos.x + m_Hooks[i].vPos.y * 7 + m_Hooks[i].vPos.z * 13); h *= 16777619u; }
	// Volumes and seeds summed, so the order objects are found in doesn't matter
	uint32 nSum = 0;
	for (size_t i = 0; i < m_Volumes.size(); i++)
	{
		const Volume& v = m_Volumes[i];
		nSum += (uint32)(int)(v.vMin.x + v.vMin.y * 7 + v.vMin.z * 13 + v.vMax.x * 17 + v.vMax.y * 19 + v.vMax.z * 23) * 31u + (uint32)v.nCode;
	}
	for (size_t i = 0; i < m_Seeds.size(); i++) nSum += (uint32)(int)(m_Seeds[i].x + m_Seeds[i].y * 7 + m_Seeds[i].z * 13);
	h ^= nSum; h *= 16777619u;
	h ^= m_nGeoKey; h *= 16777619u;
	return h;
}

uint32 CBotWorld::ProbeGeometry() const
{
	// A coarse grid of rays through the world box: vertical ones top down and
	// horizontal ones along both axes at a few heights. Where they first hit
	// changes when walls, floors or world models move. (Edits that fall
	// between the rays go unnoticed; NOLFBOT_RESCAN / bot_autogen cover those.)
	const int kGrid = 32, kRows = 16, kLevels = 4;
	LTVector vMin = m_vWorldMin, vMax = m_vWorldMax;
	LTVector vSize = vMax - vMin;
	uint32 h = 2166136261u;

	IntersectInfo info;
	for (int i = 0; i < kGrid; i++)
	for (int j = 0; j < kGrid; j++)
	{
		LTFLOAT x = vMin.x + vSize.x * (i + 0.5f) / kGrid;
		LTFLOAT z = vMin.z + vSize.z * (j + 0.5f) / kGrid;
		int nHit = Ray(LTVector(x, vMax.y, z), LTVector(x, vMin.y, z), &info) ? (int)info.m_Point.y : 0x7FFFFFFF;
		h ^= (uint32)nHit; h *= 16777619u;
	}

	for (int l = 0; l < kLevels; l++)
	{
		LTFLOAT y = vMin.y + vSize.y * (l + 0.5f) / kLevels;
		for (int r = 0; r < kRows; r++)
		{
			LTFLOAT z = vMin.z + vSize.z * (r + 0.5f) / kRows;
			int nHit = Ray(LTVector(vMin.x, y, z), LTVector(vMax.x, y, z), &info) ? (int)info.m_Point.x : 0x7FFFFFFF;
			h ^= (uint32)nHit; h *= 16777619u;

			LTFLOAT x = vMin.x + vSize.x * (r + 0.5f) / kRows;
			nHit = Ray(LTVector(x, y, vMin.z), LTVector(x, y, vMax.z), &info) ? (int)info.m_Point.z : 0x7FFFFFFF;
			h ^= (uint32)nHit; h *= 16777619u;
		}
	}
	return h;
}

void CBotWorld::SaveCache()
{
	char szFile[MAX_PATH];
	MakeCacheName(szFile, sizeof(szFile));
	_mkdir("Bots");

	FILE* fp = fopen(szFile, "wb");
	if (!fp) return;

	uint32 nMagic = 0x314D5742;	// "BWM1"
	uint32 nVersion = BOTWORLD_CACHE_VERSION;
	uint32 nKey = WorldKey();
	fwrite(&nMagic, 4, 1, fp);
	fwrite(&nVersion, 4, 1, fp);
	fwrite(&nKey, 4, 1, fp);
	fwrite(&m_Body, sizeof(m_Body), 1, fp);
	fwrite(&m_vMin, sizeof(LTVector), 1, fp);
	fwrite(&m_vMax, sizeof(LTVector), 1, fp);
	fwrite(&m_nCellsX, 4, 1, fp);
	fwrite(&m_nCellsZ, 4, 1, fp);
	fwrite(&m_nIslands, 4, 1, fp);
	fwrite(&m_fScanSeconds, 4, 1, fp);

	uint32 n = (uint32)m_ColStart.size();
	fwrite(&n, 4, 1, fp);
	if (n) fwrite(&m_ColStart[0], sizeof(int), n, fp);
	n = (uint32)m_Spans.size();
	fwrite(&n, 4, 1, fp);
	if (n) fwrite(&m_Spans[0], sizeof(BotSpan), n, fp);
	n = (uint32)m_Links.size();
	fwrite(&n, 4, 1, fp);
	if (n) fwrite(&m_Links[0], sizeof(BotWorldLink), n, fp);

	fclose(fp);
	g_pLTServer->CPrint("Bots: saved %s", szFile);
}

LTBOOL CBotWorld::LoadCache()
{
	const char* szRescan = getenv("NOLFBOT_RESCAN");
	if (szRescan && szRescan[0] == '1') return LTFALSE;

	char szFile[MAX_PATH];
	MakeCacheName(szFile, sizeof(szFile));

	FILE* fp = fopen(szFile, "rb");
	if (!fp) return LTFALSE;

	LTBOOL bOk = LTFALSE;
	uint32 nMagic = 0, nVersion = 0, nKey = 0, n = 0;
	BotBody body;
	LTVector vMin, vMax;
	int nCX = 0, nCZ = 0, nIslands = 0;
	LTFLOAT fSecs = 0.0f;

	if (fread(&nMagic, 4, 1, fp) == 1 && nMagic == 0x314D5742 &&
		fread(&nVersion, 4, 1, fp) == 1 && nVersion == BOTWORLD_CACHE_VERSION &&
		fread(&nKey, 4, 1, fp) == 1 && nKey == WorldKey() &&
		fread(&body, sizeof(body), 1, fp) == 1 &&
		fread(&vMin, sizeof(vMin), 1, fp) == 1 && fread(&vMax, sizeof(vMax), 1, fp) == 1 &&
		fread(&nCX, 4, 1, fp) == 1 && fread(&nCZ, 4, 1, fp) == 1 &&
		fread(&nIslands, 4, 1, fp) == 1 && fread(&fSecs, 4, 1, fp) == 1)
	{
		std::vector<int> colStart;
		std::vector<BotSpan> spans;
		std::vector<BotWorldLink> links;

		if (fread(&n, 4, 1, fp) == 1 && n == (uint32)(nCX * nCZ + 1))
		{
			colStart.resize(n);
			if (fread(&colStart[0], sizeof(int), n, fp) == n && fread(&n, 4, 1, fp) == 1)
			{
				spans.resize(n);
				if ((n == 0 || fread(&spans[0], sizeof(BotSpan), n, fp) == n) && fread(&n, 4, 1, fp) == 1)
				{
					links.resize(n);
					if (n == 0 || fread(&links[0], sizeof(BotWorldLink), n, fp) == n)
					{
						m_Body = body;
						m_vMin = vMin; m_vMax = vMax;
						m_nCellsX = nCX; m_nCellsZ = nCZ;
						m_nIslands = nIslands;
						m_fScanSeconds = fSecs;
						m_ColStart.swap(colStart);
						m_Spans.swap(spans);
						m_Links.swap(links);
						for (size_t i = 0; i < m_Links.size(); i++) m_Links[i].nFails = 0;
						for (size_t i = 0; i < m_Spans.size(); i++) m_Spans[i].nNode = BOTW_INVALID;
						BuildLinkIndex();
						bOk = LTTRUE;
					}
				}
			}
		}
	}

	fclose(fp);
	return bOk;
}
