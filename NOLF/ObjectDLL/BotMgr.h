// ----------------------------------------------------------------------- //
//
// MODULE  : BotMgr.h
//
// PURPOSE : Adds/removes bots, owns the waypoint graph, and gives bots a
//			 per-frame view of the players and pickups in the world
//
// ----------------------------------------------------------------------- //

#ifndef __BOT_MGR_H__
#define __BOT_MGR_H__

#include "BotNav.h"
#include "BotWorld.h"
#include "BotMapInfo.h"
#include "BotGenome.h"
#include <vector>
#include <string>

class CPlayerObj;
class CBotPlayer;

// Bots need player IDs that never clash with real client IDs (those are
// small numbers).  The IDs go into uint8 fields in some messages.
#define BOT_ID_BASE		200
#define BOT_MAX_BOTS	32

struct BotItem
{
	enum Type { eWeapon, eAmmo, eGear, eMod, eIntel, eVehicle, eOther };

	LTVector	vPos;
	Type		eType;
	int			nWeaponId;		// eWeapon only
	LTBOOL		bAvailable;		// Visible (not waiting to respawn)
	uint8		nTeam;			// eIntel: team that can collect it (0 = anybody)
	LTBOOL		bPhotoOnly;		// eIntel: needs the camera gadget
	LTFLOAT		fRespawnDelay;	// Seconds an item stays gone after pickup
	LTFLOAT		fRespawnAt;		// Predicted time it comes back (0 = unknown / available)
	LTBOOL		bScuba;			// eGear: SCUBA gear (breathe under water)
	LTBOOL		bArmor;			// eGear: body armor
	char		szName[32];		// What it is ("Rocket Launcher", "Phosphorus", "Armor"...), for logs and callouts
	int			nAmmoTypes;		// eWeapon / eAmmo: ammo it gives
	uint8		anAmmoId[4];
};

class CBotMgr
{
	public :

		CBotMgr();

		// Server shell hooks
		void	PreStartWorld();
		void	PostStartWorld();
		void	Update();
		void	OnPlayerRemoved();		// A client left: drop cached player pointers

		// Console: returns LTTRUE if the command was a bot command
		LTBOOL	HandleCommand(const char* szCmd);

		// Scoreboard info for a client that just joined
		void	SendBotInfos(HCLIENT hClient);
		void	WriteBotPings(HMESSAGEWRITE hWrite);	// Scoreboard pings (MID_PINGTIMES)

		// Chat and lobby events
		void	OnPlayerKilled(CPlayerObj* pVictim, CPlayerObj* pKiller);
		void	BotSay(CPlayerObj* pBot, const char* szText, LTBOOL bTeamOnly);
		void	LogAction(CPlayerObj* pBot, const char* szText);	// "[m:ss] Name: text" in Bots\\actions.txt

		// Queries used by the bots
		const std::vector<CPlayerObj*>&	GetPlayers();
		const std::vector<BotItem>&		GetItems() const { return m_Items; }
		CPlayerObj*	FindPlayerByID(uint32 nID);
		LTBOOL		IsItemAvailable(const LTVector& vPos) const;
		LTBOOL		IsItemReachable(const LTVector& vPos);
		// "I'm going for that": teammates leave it to us (unless much closer)
		void		ClaimItem(const LTVector& vPos, CPlayerObj* pBot, LTFLOAT fUntil);
		LTBOOL		IsItemClaimed(const LTVector& vPos, CPlayerObj* pAsker);
		LTBOOL		IsItemUnderwater(const LTVector& vPos);	// Deep down or in a flooded tunnel (needs SCUBA)
		LTFLOAT		GetItemRespawnIn(const LTVector& vPos) const;	// 0 = there now, <0 = unknown/gone
		int			PickCampNode(const LTVector& vNear, LTVector& vLook, LTBOOL bHighOnly = LTFALSE);
		LTBOOL		GetCampLook(int nNode, LTVector& vLook) const;
		LTFLOAT		GetCampScore(int nNode) const { return (nNode >= 0 && nNode < (int)m_CampScore.size()) ? m_CampScore[nNode] : -1.0f; }
		LTBOOL		ActivateIntel(const LTVector& vPos, CPlayerObj* pPlayer);	// Like pressing "use" on it
		void		OpenDoorsAhead(CPlayerObj* pPlayer, const LTVector& vPos, const LTVector& vDir);
		LTBOOL		MountVehicle(const LTVector& vPos, CPlayerObj* pPlayer);	// "Use" a vehicle there
		LTBOOL		GetBasePos(uint32 dwTeam, LTVector& vPos) const;	// A spawn point of that team
		LTBOOL		GetPushTarget(uint32 dwTeam, LTVector& vPos);		// Where this team's attackers go (shared)
		LTBOOL		GetGuardTarget(uint32 dwTeam, LTVector& vPos);		// What this team's defenders protect
		LTBOOL		GetSneakTarget(uint32 dwTeam, LTVector& vPos);		// Underwater way into the enemy base
		void		SetTeamAlert(uint32 dwTeam, const LTVector& vPos);	// Enemy spotted in our base
		LTBOOL		GetTeamAlert(uint32 dwTeam, LTVector& vPos, LTFLOAT fMaxAge);
		LTBOOL		IsRecalled(uint32 nPlayerID, uint32 dwTeam) const;	// Told to fall back and defend?

		// Team callouts ("enemy over there!"): what teammates have seen lately
		void		ReportSighting(uint32 dwTeam, const LTVector& vEnemyPos);
		LTBOOL		GetSighting(uint32 dwTeam, const LTVector& vNear, LTFLOAT fMaxDist, LTFLOAT fMaxAge, LTVector& vPos);
		void		NoteRoleFail(uint32 dwTeam);	// An attacker found no path to the objective

		// Per-team feature switches for head-to-head testing in self-play
		// (env NOLFBOT_OFF_T1 / NOLFBOT_OFF_T2 = comma list of names below).
		// Everything is on in normal play.  Deathmatch uses team 1's set.
		enum
		{
			eFeatWaves		= 1 << 0,	// "waves"		rally at a staging point
			eFeatDanger		= 1 << 1,	// "danger"		route around learned killing zones
			eFeatCallouts	= 1 << 2,	// "callouts"	share enemy sightings
			eFeatDiscipline	= 1 << 3,	// "discipline"	don't shoot beyond effective range
			eFeatFocus		= 1 << 4,	// "focus"		keep running for the documents under fire
			eFeatRecall		= 1 << 5,	// "recall"		fall back when the base is attacked
			eFeatAll		= 0x3F
		};
		LTBOOL		HasFeature(uint32 dwTeam, int nFeature) const;

		// Trained fighting parameters per team (Bots\genome.txt in normal play;
		// assigned per round by the trainer in self-play training)
		const BotGenome& GetGenome(uint32 dwTeam) const { return m_aGenome[(dwTeam == 2) ? 2 : 1]; }

		// Wave attacks: gather at a staging point, then push together
		enum { eWaveGather = 0, eWavePush = 1 };
		int			GetWaveState(uint32 dwTeam) const;
		int			GetWaveAtStage(uint32 dwTeam) const { return (dwTeam == 1 || dwTeam == 2) ? m_aPlan[dwTeam].nAtStage : 0; }
		LTBOOL		GetStage(uint32 dwTeam, LTVector& vStage) const;
		CBotNav&	GetNav() { return m_Nav; }
		CBotWorld&	GetWorld() { return m_World; }
		CBotMapInfo& GetMapInfo() { return m_MapInfo; }
		void	DropNav();						// Drop the graph and everything built on it (bots' paths, areas, routes)
		void	RestartNav(LTBOOL bRescan);		// DropNav, then measure the map again
		int			GetRouteGen(uint32 dwTeam) const { return (dwTeam < 3) ? m_anRouteGen[dwTeam] : 0; }
		LTBOOL		IsNavBusy() const { return !m_World.IsReady() || m_Nav.GetNumNodes() == 0; }

		int			GetNumBots() const { return (int)m_Bots.size(); }
		CPlayerObj*	GetBotPlayer(int i);

	private :

		struct BotSlot
		{
			char		szName[32];
			char		szModel[64];
			LTFLOAT		fSkill;
			int			nStyle;
			uint32		dwTeam;
			uint32		nID;
			CBotPlayer*	pBot;		// LTNULL between worlds (or before joining)
			uint16		nPing;		// Fake ping for the scoreboard
		};

		LTBOOL	AddBot(const char* szName, LTFLOAT fSkill, uint32 dwTeam, int nStyle=-1);
		LTBOOL	SpawnBot(BotSlot& slot);
		void	RemoveBot(int nIndex);
		void	RemoveAllBots();
		int		FindBot(const char* szName);
		uint32	AllocBotID();
		uint32	PickTeam();
		void	PickModel(char* szModel, int nLen);
		void	PickName(char* szName, int nLen);
		int		CountHumans();

		void	RefreshPlayers();
		void	RefreshItems();
		void	UpdateRecording();
		void	UpdateRecall(LTFLOAT fTime);
		void	UpdatePlans(LTFLOAT fTime);
		LTBOOL	ComputeStage(uint32 dwTeam, const LTVector& vTarget, LTVector& vStage);

		struct TeamPlan
		{
			int			nState;			// eWaveGather / eWavePush
			LTFLOAT		fStateTime;		// when we entered that state
			LTVector	vTarget;		// the documents (push target) the stage was made for
			LTVector	vStage;			// where the wave gathers
			LTBOOL		bStageOk;
			LTFLOAT		fNextStage;		// recompute the stage now and then
			int			nMembers, nAtStage, nNear, nWaves;
		};
		TeamPlan	m_aPlan[3];

		int			m_anFeatures[3];	// per team, eFeat* bits
		BotGenome	m_aGenome[3];		// index 1/2 = team (deathmatch uses 1)

		// Self-play training rounds (env NOLFBOT_TRAIN_DIR)
		void		UpdateTraining(LTFLOAT fTime);
		LTBOOL		LoadAssignment(int nRound);
		LTBOOL		m_bWaitingAssign;
		LTFLOAT		m_fNextAssignCheck;
		LTFLOAT		m_fRoundStart;
		int			m_nRound;
		int			m_anRoundKills[3];
		int			m_anRoundPhotos[3];
		int			m_nFrames;			// server frames since the last log (speed check)
		LTFLOAT		m_fFramesSince;

		enum { kSightings = 16 };
		LTVector	m_avSighting[3][kSightings];
		LTFLOAT		m_afSightingTime[3][kSightings];
		int			m_anNextSighting[3];
		void	UpdateMinPlayers();
		void	UpdateJoining(LTFLOAT fTime);
		void	UpdateAutoSkill(LTFLOAT fTime);
		void	WriteLog(LTBOOL bNewWorld);
		void	CleanupBodies(LTFLOAT fTime);
		void	LoadNav();
		void	UpdateCampScores();
		LTFLOAT	VariedSkill();
		void	RunConfig();
		void	RunConfigLine(char* szLine);
		void	ApplySettings();
		void	SetServerVar(const char* szVar, const char* szValue);

		void	Message(const char* szFormat, ...);

		std::vector<BotSlot>		m_Bots;
		std::vector<CPlayerObj*>	m_Players;
		std::vector<BotItem>		m_Items;
		std::vector<std::string>	m_Models;
		std::vector<std::string>	m_SessionModels;	// The few models bots use this session
		int			m_nModelVariety;					// How many different models (textures!)

		CBotNav		m_Nav;
		CBotWorld	m_World;
		CBotMapInfo	m_MapInfo;
		struct Claim { LTVector vPos; uint32 nBot; uint32 nTeam; LTFLOAT fUntil; };
		std::vector<Claim>	m_Claims;
		FILE*		m_fpActions;
		LTBOOL		m_bActionsOpened;
		void		OpenActionLog();
		LTFLOAT		m_fWorldStart;
		int			m_anRouteGen[3];
		void		FinishNav();
		void		WriteCatalog();		// Bots/<map>-objects.txt: everything the bots know is in the map
		LTFLOAT		m_fNextScanMsg;
		std::vector<LTFLOAT>	m_CampScore;	// Per waypoint: how much it overlooks (-1 = not computed yet)
		std::vector<LTVector>	m_CampLook;		// ...and which way to look
		std::vector<LTVector>	m_aBases[3];	// Team spawn points (index = team id)
		LTVector				m_avPush[3];	// Current attack target per team
		LTFLOAT					m_afPushUntil[3];
		LTVector				m_avAlert[3];	// Last enemy sighting inside our own base
		LTFLOAT					m_afAlertTime[3];
		std::vector<uint32>		m_aRecalled[3];	// Bots told to fall back (player IDs)
		LTFLOAT					m_fNextRecall;
		int						m_anRoleFail[3];
		LTBOOL					m_bLearn;		// Learn the graph from how everybody moves
		LTFLOAT					m_fNextLearn;
		LTFLOAT					m_fNextNavSave;
		LTFLOAT					m_fNextDangerDecay;
		int						m_anSneakNode[3];
		int						m_nSneakNavSize;	// Recompute when the graph changes
		int			m_nCampScoreNext;
		char		m_szNavWorld[256];

		LTFLOAT		m_fPlayersTime;
		LTFLOAT		m_fNextItemRefresh;
		LTFLOAT		m_fSpawnBotsTime;	// When to (re)create bots after a world loads
		LTBOOL		m_bWorldLoaded;
		LTFLOAT		m_fDefaultSkill;
		int			m_nMinPlayers;
		LTBOOL		m_bRecording;
		LTBOOL		m_bConfigRun;		// bots.cfg runs once per hosting session
		LTBOOL		m_bSettingsApplied;	// Server settings re-applied every world
		LTFLOAT		m_fNextJoin;		// Bots join one at a time
		LTBOOL		m_bAutoSkill;		// Adjust bot skill to how the humans are doing
		LTFLOAT		m_fSkillOffset;
		LTFLOAT		m_fNextAutoSkill;
		LTFLOAT		m_fNextChat;		// Don't spam
		LTFLOAT		m_fNextLog;			// Bots\botlog.txt snapshots
		LTFLOAT		m_fNextBodyCleanup;
		int			m_nBodiesRemoved;
		int			m_nIntelTries;
		std::vector<std::string>	m_Settings;	// "Var Value" from the set command
};

extern CBotMgr g_BotMgr;

#endif // __BOT_MGR_H__
