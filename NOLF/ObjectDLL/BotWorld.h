// ----------------------------------------------------------------------- //
//
// MODULE  : BotWorld.h
//
// PURPOSE : What the bots know about the 3D map, measured from the world
//			 geometry itself (not guessed from spawn points or recorded
//			 walks).
//
//			 The playable area is cut into 32u columns.  Vertical rays find
//			 every floor in every column (several per column: bridges,
//			 tunnels, rooftops, the bottom of the reservoir), with its
//			 headroom and any water above it.  Moves between neighbouring
//			 floors are then classified like a player would make them: walk
//			 (stairs, ramps), drop off a ledge, jump up onto a ledge, swim.
//			 On top of that come the long moves found from the map's own
//			 objects: zipcords (ZipHook), ladders (Ladder volumes) and
//			 running jumps over gaps, all checked against the player's real
//			 size, jump speed and gravity.
//
//			 The bots' waypoint graph (CBotNav) is built from this, every
//			 floor cell belongs to one waypoint, and paths are followed cell
//			 by cell, so a bot only ever runs where its body really fits.
//
//			 Scanning a big map takes a while, so it runs a slice per server
//			 frame and the result is cached in Bots\<map>.bwm.
//
// ----------------------------------------------------------------------- //

#ifndef __BOT_WORLD_H__
#define __BOT_WORLD_H__

#include "ltbasedefs.h"
#include <vector>

#define BOTW_INVALID			(-1)

// Span (one floor in one column) flags
#define BOTSPAN_WATER			0x01	// Liquid above the floor
#define BOTSPAN_SWIM			0x02	// Too deep to stand: you swim here
#define BOTSPAN_NOAIR			0x04	// Water all the way up to the ceiling (needs SCUBA / holding breath)
#define BOTSPAN_LADDER			0x08	// Inside a ladder volume
#define BOTSPAN_TIGHT			0x10	// A wall or ledge right next to it (body would scrape)
#define BOTSPAN_REACH			0x20	// Reachable from where players spawn / items lie
#define BOTSPAN_LEDGE			0x40	// Next to a drop

// How you get from a span to its neighbour in one direction
enum BotMove
{
	eBotMoveNone	= 0,
	eBotMoveWalk	= 1,	// Walk / step / swim
	eBotMoveDrop	= 2,	// Walk off a ledge and fall
	eBotMoveClimb	= 3,	// Jump up onto a ledge
};

// Long moves
enum BotWorldLinkType
{
	eBotLinkJump	= 1,	// Running jump over a gap
	eBotLinkZip		= 2,	// Zipcord up to a hook, then step onto the ledge
	eBotLinkLadder	= 3,	// Climb a ladder
};

struct BotSpan
{
	float	fFloor;			// Floor height
	float	fCeil;			// First solid above the floor (capped)
	float	fWater;			// Liquid surface (or the ceiling) when BOTSPAN_WATER
	int		aNbr[8];		// Neighbour span per direction (BOTW_INVALID = none)
	uint16	nMoves;			// 2 bits per direction (BotMove)
	uint8	nFlags;
	uint8	nClear;			// Cells to the nearest wall / ledge (0 = touching)
	int		nNode;			// Waypoint this span belongs to

	int		GetMove(int d) const { return (nMoves >> (d*2)) & 3; }
	void	SetMove(int d, int m) { nMoves = (uint16)((nMoves & ~(3 << (d*2))) | (m << (d*2))); }
};

struct BotWorldLink
{
	int		nFrom, nTo;		// Spans
	uint8	nType;			// BotWorldLinkType
	LTVector vAux;			// Zip: the hook.  Ladder: the ladder's center.  Jump: take-off point.
	float	fCost;			// In run-distance units (time x run speed)
	int		nFails;			// Bots that tried and didn't make it (this session)
};

// Fixed physical facts the scan was made with (stored in the cache)
struct BotBody
{
	float	fRadius;		// Half width of the player box
	float	fHalfHeight;	// Center to floor
	float	fStep;			// Highest step you just walk up
	float	fClimb;			// Highest ledge you can jump onto
	float	fJumpVel;
	float	fGravity;		// Positive (units / s^2)
	float	fRunVel;
	float	fZipVel;
};

class CBotWorld
{
	public :

		CBotWorld();

		void	Clear();

		// Load Bots\<map>.bwm or start scanning.  Step() continues the
		// scan with a time budget (milliseconds); LTTRUE when ready.
		void	Begin(const char* szWorld, LTBOOL bRescan = LTFALSE);
		LTBOOL	Step(LTFLOAT fBudgetMs);
		LTBOOL	IsReady() const		{ return m_eState == eReady; }
		LTBOOL	IsScanning() const	{ return m_eState != eReady && m_eState != eIdle; }
		int		GetProgress() const;	// 0..100 for messages
		const char* GetStateName() const;

		// Geometry
		static const float kCell;
		const BotBody& GetBody() const { return m_Body; }
		// Can a player box ride a cord from vCenter to vHook without hitting
		// anything (doors and door frames included)?
		static LTBOOL IsZipRideClear(const LTVector& vCenter, const LTVector& vHook, float fHalfHeight, float fRadius);
		int		GetNumSpans() const { return (int)m_Spans.size(); }
		const BotSpan& GetSpan(int n) const { return m_Spans[n]; }
		BotSpan& GetSpanRW(int n) { return m_Spans[n]; }
		LTVector GetSpanPos(int n) const;		// Where a player's center is on that span (in the water for swim spans)
		LTVector GetSpanFloor(int n) const;		// Floor point under the column center
		void	GetSpanCell(int n, int& x, int& z) const;
		int		GetSpanAt(int x, int z, float fY, float fMaxBelow, float fMaxAbove) const;

		// The span a player at vPos (center) is on / over.  BOTW_INVALID if
		// outside the scanned area.
		int		FindSpan(const LTVector& vPos) const;
		int		FindSpanNear(const LTVector& vPos, int nRadiusCells) const;	// Falls back to a nearby reachable span

		// Can a player walk in a straight line from span A to span B over
		// connected floor (no walls, no drops, not scraping walls)?  Used for
		// cutting corners on a path.
		LTBOOL	CanWalkStraight(int nFrom, int nTo, LTBOOL bAllowTight) const;
		// Same, between two positions (for local steering)
		LTBOOL	CanWalkStraight(const LTVector& vFrom, const LTVector& vTo) const;

		// Where to dive (or come up) between two depths: the nearest spot whose
		// water column, and the columns around it (body width), is open all
		// the way from yLo to yHi.  LTFALSE if none within nRadius cells.
		LTBOOL	FindDiveColumn(const LTVector& vNear, LTFLOAT yLo, LTFLOAT yHi, int nRadius, LTVector& vDive) const;

		// The move from span n in the (closest) 8-direction to vDir
		int		DirIndex(const LTVector& vDir) const;
		static void	DirVector(int d, int& dx, int& dz);

		// Long moves starting at a span
		const std::vector<BotWorldLink>& GetLinks() const { return m_Links; }
		BotWorldLink& GetLinkRW(int n) { return m_Links[n]; }
		int		GetLinkCount() const { return (int)m_Links.size(); }
		void	GetLinksFrom(int nSpan, std::vector<int>& links) const;
		int		FindLink(int nFrom, int nTo) const;
		int		CountLinks(int nType) const;

		// Span-level A*, restricted to spans whose waypoint is in pAllowed
		// (NULL = anywhere).  The path excludes the start.  Long links are
		// returned as consecutive spans; IsLinkStep tells.
		LTBOOL	FindSpanPath(int nFrom, int nTo, const std::vector<char>* pAllowedNodes, std::vector<int>& path, int nMaxVisit) const;

		// Trouble spots learned this game (a bot got pinned there: a body, a
		// crowd, an object the scan doesn't see): routes avoid them a while
		void	MarkHazard(int nSpan, LTFLOAT fUntil);
		LTBOOL	IsHazard(int nSpan) const;

		// Liquid / ladder volumes from the map (boxes)
		struct Volume { LTVector vMin, vMax; int nCode; HOBJECT hObj; };
		const std::vector<Volume>& GetVolumes() const { return m_Volumes; }

		// Stats
		int		CountSpans(uint8 nFlag) const;
		int		GetIslands() const { return m_nIslands; }
		LTFLOAT	GetScanSeconds() const { return m_fScanSeconds; }
		void	GetBounds(LTVector& vMin, LTVector& vMax) const { vMin = m_vMin; vMax = m_vMax; }

		// Seeds = places players certainly get to (spawns, pickups, intel)
		const std::vector<LTVector>& GetSeeds() const { return m_Seeds; }
		const std::vector<LTVector>& GetSpawns() const { return m_Spawns; }

	private :

		enum State { eIdle, eCollect, eColumns, eMoves, eSpecial, eReach, eClearance, eReady };

		// Phases
		void	Collect();
		LTBOOL	StepColumns(LTFLOAT fBudgetMs);
		LTBOOL	StepMoves(LTFLOAT fBudgetMs);
		LTBOOL	StepSpecial(LTFLOAT fBudgetMs);
		void	ComputeReach();
		void	ComputeClearance();

		void	ScanColumn(int x, int z, std::vector<BotSpan>& out);
		void	ClassifyMoves(int nSpan);
		LTBOOL	TestWalk(const BotSpan& a, const LTVector& vA, const BotSpan& b, const LTVector& vB, int nDir) const;
		LTBOOL	TestDrop(const BotSpan& a, const LTVector& vA, const BotSpan& b, const LTVector& vB) const;
		LTBOOL	TestClimb(const BotSpan& a, const LTVector& vA, const BotSpan& b, const LTVector& vB) const;
		LTBOOL	TestClimbOut(const BotSpan& b, const LTVector& vA, const LTVector& vB) const;
		LTBOOL	TestSwim(const BotSpan& a, const LTVector& vA, const BotSpan& b, const LTVector& vB) const;
		void	FindZipLinks();
		void	FindLadderLinks();
		LTBOOL	StepJumpLinks(LTFLOAT fBudgetMs);
		LTBOOL	TestJumpArc(const LTVector& vFrom, const LTVector& vTo, LTFLOAT fHorizSpeed) const;
		LTBOOL	IsWalkReachable(int nFrom, int nTo, int nMaxVisit, LTFLOAT fMaxLen) const;
		int		AddLink(int nFrom, int nTo, int nType, const LTVector& vAux, LTFLOAT fCost);
		void	BuildLinkIndex();

		LTBOOL	PointInLiquid(const LTVector& vPos) const;
		LTBOOL	PointInVolume(const LTVector& vPos, int nCode) const;
		LTFLOAT	FindLiquidTop(LTFLOAT x, LTFLOAT z, LTFLOAT fFrom, LTFLOAT fCeil) const;

		int		ColumnIndex(int x, int z) const { return z * m_nCellsX + x; }
		LTBOOL	CellInside(int x, int z) const { return x >= 0 && z >= 0 && x < m_nCellsX && z < m_nCellsZ; }
		LTFLOAT	CellCenterX(int x) const { return m_vMin.x + (x + 0.5f) * kCell; }
		LTFLOAT	CellCenterZ(int z) const { return m_vMin.z + (z + 0.5f) * kCell; }
		int		CellX(LTFLOAT f) const;
		int		CellZ(LTFLOAT f) const;

		// Cache
		LTBOOL	LoadCache();
		void	SaveCache();
		void	MakeCacheName(char* szBuf, int nLen) const;
		uint32	WorldKey() const;
		uint32	ProbeGeometry() const;

		State	m_eState;
		char	m_szWorld[256];
		BotBody	m_Body;

		LTVector m_vMin, m_vMax;		// Scanned box
		LTVector m_vWorldMin, m_vWorldMax;
		int		m_nCellsX, m_nCellsZ;

		std::vector<int>		m_ColStart;	// Per column: first span (spans of a column are consecutive, top down)
		std::vector<BotSpan>	m_Spans;
		std::vector<BotWorldLink> m_Links;
		std::vector<int>		m_LinkStart;	// Per span: first entry in m_LinkIndex (size = spans+1)
		std::vector<int>		m_LinkIndex;
		std::vector<float>		m_HazardUntil;	// Per span, game time (not cached)
		std::vector<Volume>		m_Volumes;
		std::vector<LTVector>	m_Seeds;
		std::vector<LTVector>	m_Spawns;		// Where players appear
		struct Hook { LTVector vPos; LTVector vDims; };
		std::vector<Hook>		m_Hooks;

		// Incremental scan state
		int		m_nScanCol;
		int		m_nMoveSpan;
		int		m_nJumpSpan;		// -1 = zipcords/ladders not found yet
		int		m_nZipLinks, m_nLadderLinks;
		struct JumpTaken { LTVector vFrom, vTo; };
		std::vector<JumpTaken>	m_JumpTaken;
		LTFLOAT	m_fScanStart;
		LTFLOAT	m_fScanSeconds;
		int		m_nIslands;
		uint32	m_nGeoKey;			// Hash of a coarse ray grid (catches geometry edits)
};

#endif // __BOT_WORLD_H__
