// ----------------------------------------------------------------------- //
//
// MODULE  : BotPlayer.h
//
// PURPOSE : Server controlled deathmatch / H.A.R.M. vs UNITY player
//
// ----------------------------------------------------------------------- //

#ifndef __BOT_PLAYER_H__
#define __BOT_PLAYER_H__

#include "PlayerObj.h"
#include <vector>
#include <set>
#include "BotNav.h"
#include "BotWorld.h"

class CWeapon;
struct WEAPON;

// Play styles.  Skill (aim, reactions) is separate from style (what the
// bot likes to do), so you can have a sloppy camper or a deadly rusher.

enum BotStyle
{
	eBotRusher = 0,		// Runs at you, jumps a lot, chases
	eBotCamper,			// Holds a good spot, long range weapons
	eBotTactician,		// The smart one: leads shots, listens, retreats to heal
	eBotScavenger,		// Item control, times respawns
	eBotTeamPlayer,		// Objectives and sticking with teammates
	eBotCasual,			// An average joe
	kNumBotStyles
};

struct BotPersonality
{
	const char*	szName;
	LTFLOAT		fAggression;	// 0..1: closes distance, chases, hunts
	LTFLOAT		fJumpiness;		// 0..1: hops and dodge jumps
	LTFLOAT		fCamp;			// Chance to go camping when idle
	LTFLOAT		fRangePref;		// Multiplier on preferred fighting distance
	LTFLOAT		fRetreatHealth;	// Health fraction where we break off to heal
	LTFLOAT		fItemGreed;		// Multiplier on item values
	LTFLOAT		fTeamwork;		// Objectives / following teammates
	LTFLOAT		fAwareness;		// Field of view, hearing, reaction time
	LTFLOAT		fChaseTime;		// Seconds we chase someone we lost sight of
	LTBOOL		bSmart;			// Leads shots, aims splash at feet, times items, hides while reloading
	int			nWeight;		// How common this style is
};

class CBotPlayer : public CPlayerObj
{
	public :

		CBotPlayer();

		void	SetSkill(LTFLOAT fSkill) { m_fSkill = fSkill; }
		LTFLOAT	GetSkill() const { return m_fSkill; }

		void	SetStyle(int nStyle);
		int		GetStyle() const { return m_nStyle; }

		static const BotPersonality& GetPersonality(int nStyle);
		static int	FindStyle(const char* szName);		// -1 if unknown
		static int	PickRandomStyle(LTBOOL bTeamGame);

		void	Describe(char* szBuf, int nLen);	// One line of state for Bots\botlog.txt
		void	Act(const char* szFormat, ...);		// One line in the action log (Bots\actions.txt)
		LTBOOL	IsWaveMember();						// Walks in with the team's attack wave?
		void	ForgetNav();						// The map graph is being rebuilt: drop paths, goals and lane

	protected :

		virtual void UpdateBot();

	private :

		// Thinking
		void	UpdateDead(LTFLOAT fTime);
		void	UpdateEnemy(LTFLOAT fTime);
		void	UpdateHearing(LTFLOAT fTime);
		void	UpdateHurt(LTFLOAT fTime);
		void	UpdateGoal(LTFLOAT fTime);
		LTBOOL	PickItemGoal(LTFLOAT fTime, LTFLOAT fHealth, LTFLOAT fArmor);
		LTBOOL	PickCampGoal(LTFLOAT fTime);
		LTBOOL	PickBuddyGoal(LTFLOAT fTime);
		LTBOOL	PickTeamRoleGoal(LTFLOAT fTime);
		LTBOOL	PickScubaGoal(LTFLOAT fTime);
		LTBOOL	PickLootGoal(LTFLOAT fTime);			// Gear up after spawning
		LTBOOL	PickOverwatchGoal(LTFLOAT fTime);		// Snipers/campers: hold a forward perch
		LTFLOAT	GetLoadoutPower(LTFLOAT fDist);			// Best gun+ammo we carry
		LTFLOAT	GetItemGain(const struct BotItem& item);	// What picking it up adds
		void	StartLooting(LTFLOAT fTime);
		LTFLOAT	m_fLootUntil;							// Gearing up until then (0 = done)
		LTBOOL	m_bFirstSpawn;
		char	m_szLootWhy[48];						// "loot:Rocket_Launcher" (log)
		static const LTFLOAT kWellArmed;
		LTBOOL	PickFrontierGoal(LTFLOAT fTime, const LTVector& vTarget);	// No known path: go as far as we know, then explore
		LTBOOL	m_bGoalIsSneak;
		LTBOOL	m_bGoalIsObjective;		// Team game: on the way to documents / enemy base
		LTBOOL	IsObjectiveRun() const;
		LTBOOL	IsClosingOnDocuments();		// Last stretch of a documents run: keep running
		LTFLOAT	Gene(int nGene);				// Trained fighting parameter for our team
		void	TryPhotograph(LTFLOAT fTime);	// Snap any reachable enemy documents
		LTBOOL	m_bSneakDone;			// Been through the water this life
		LTFLOAT	GetMoveSpeed();
		void	ChooseWeapon(LTFLOAT fTime, LTFLOAT fDist);
		void	UpdateAim(LTFLOAT fTime, LTFLOAT fDelta);
		void	UpdateFire(LTFLOAT fTime);
		void	UpdateWater();

		LTFLOAT	GetHealthFraction();
		LTBOOL	HasRangedWeapon();
		LTFLOAT	GetPreferredRange();
		static LTBOOL IsMelee(const WEAPON* pData);
	public :
		static LTFLOAT GetEffectiveRange(const WEAPON* pData, const AMMO* pAmmo);	// Farther than this you mostly miss
		static LTFLOAT RateShot(const WEAPON* pW, const AMMO* pA, LTFLOAT fDist);		// How fast it kills at fDist (0 = useless)
		static LTFLOAT WeaponTier(const WEAPON* pW);	// Known MP ranking (Collector, rockets, grenades...)
		static LTFLOAT AmmoTier(const AMMO* pA);		// Phosphorus > plain, ...
	private :

		// Movement
		LTVector GetNavMoveDir(LTFLOAT fTime, LTBOOL& bJump);
		LTVector GetCombatMoveDir(LTFLOAT fTime, LTBOOL& bJump);
		void	ApplyMovement(LTFLOAT fTime, const LTVector& vDir, LTBOOL bJump);
		void	UpdateStuck(LTFLOAT fTime, LTBOOL bWantMove, LTBOOL& bJump);
		void	SetMoveCommands(const LTVector& vDir);
		LTBOOL	IsBlocked(const LTVector& vDir, LTFLOAT fDist);
		LTBOOL	IsHopSafe(const LTVector& vDir);

		LTBOOL	PathTo(const LTVector& vGoal);
		LTBOOL	FindEscape(int nFrom, std::vector<int>& cells);
		LTBOOL	ExtendSpanPath();
		void	ClearPath() { m_Path.clear(); m_nPathIndex = 0; m_SpanPath.clear(); m_nSpanIdx = 0; m_nCorridorEnd = 0; m_nLookIdx = 0; }
		enum { eStepWalk, eStepClimb, eStepDrop, eStepZip, eStepLadder, eStepGap };
		int		SpanStepKind(int i, int& nLink);
		LTBOOL	IsPlainAhead(int nSteps);		// Only walking for the next few cells (OK to hop)
		void	OnStuck(LTFLOAT fTime, LTBOOL& bJump, LTBOOL bPinned);
		LTBOOL	FindDryLand(const LTVector& vPos, LTVector& vDry);
		void	OnReachedHighGround(LTFLOAT fTime);
		void	SetGoal(const LTVector& vGoal, LTBOOL bItem, LTFLOAT fTimeout);

		CPlayerObj*	GetEnemy();
		LTVector	GetEyePos();
		LTVector	GetAimPoint(CPlayerObj* pTarget);
		LTBOOL		IsEnemyOf(CPlayerObj* pPlayer);

		LTBOOL	RecentlyVisited(const LTVector& vPos, LTFLOAT fTime) const;
		LTBOOL	NeedsHealing(LTFLOAT fHealth);
		LTBOOL	IsHealGoal() const;

		// Personality
		int		m_nStyle;
		const BotPersonality* m_pP;
		LTFLOAT	m_fSkill;				// 0 (dumb) .. 1 (nasty)

		// Enemy
		uint32	m_nEnemyID;				// Player ID, 0xFFFFFFFF = none
		LTBOOL	m_bEnemyVisible;
		LTVector m_vEnemyLastPos;
		LTVector m_vEnemyVel;			// Estimated from position changes
		LTVector m_vEnemyPrevPos;
		LTFLOAT	m_fEnemyPrevTime;
		LTFLOAT	m_fEnemyLastSeen;
		LTFLOAT	m_fEnemyAcquired;		// When we may start shooting at the current enemy
		LTFLOAT	m_fNextEnemyCheck;

		// Things we heard (gunfire) but didn't see
		LTVector m_vHeardPos;
		LTFLOAT	m_fHeardTime;

		// Aim
		LTFLOAT	m_fYaw;
		LTVector m_vAimDir;
		LTVector m_vAimError;
		LTFLOAT	m_fNextAimError;
		LTBOOL	m_bAimHead;

		// Weapons
		LTFLOAT	m_fNextWeaponCheck;
		int		m_nShotsInClip;		// (unused: see m_anClipUsed)
		enum { kMaxClipWeapons = 64 };
		int		m_anClipUsed[kMaxClipWeapons];	// Shots fired since that weapon's last reload (each gun keeps its own clip)
		static LTFLOAT ReloadTime(const WEAPON* pW);	// How long a person takes to reload it
		LTFLOAT	m_fReloadDone;
		LTFLOAT	m_fLastFire;
		LTFLOAT	m_fNextShot;
		int		m_nBurstShots;
		int		m_nBurstLen;

		// Goals / navigation
		LTVector m_vGoal;
		LTBOOL	m_bHasGoal;
		LTBOOL	m_bGoalIsItem;
		LTBOOL	m_bGoalIsCamp;
		LTFLOAT	m_fGoalTimeout;
		std::vector<int> m_Path;
		int		m_nPathIndex;
		// The exact floor cells to walk over (CBotWorld spans), planned a few
		// waypoints ahead at a time
		std::vector<int> m_SpanPath;
		int		m_nSpanIdx;				// Next cell to reach
		int		m_nCorridorEnd;			// Waypoints (m_Path) covered by m_SpanPath so far
		int		m_nGoalSpan;
		int		m_nLookIdx;				// Farthest cell we can run straight to
		LTFLOAT	m_fNextLook;
		LTFLOAT	m_fOffPathSince;
		int		m_nReplans;
		std::vector<char> m_Allowed;	// Scratch for corridor searches
		int		m_nJumpLink;
		LTVector m_vJumpFrom;
		LTFLOAT	m_fJumpTime;				// When we took off on m_nJumpLink
		LTVector m_vCampSpot;
		LTBOOL	m_bWasCamping;
		LTBOOL	m_bPerched;				// Holding high ground
		LTFLOAT	m_fGoalSetTime;			// When the current goal was picked (no flip-flopping)
		LTVector m_vOffPathPos;			// Where / when we last re-planned for being off the path
		LTFLOAT	m_fOffPathTime;
		int		m_nOffPathRepeats;
		LTFLOAT	m_fShortLookUntil;		// Steer only a cell or two ahead until then (stuck under water)
		LTFLOAT	m_fHurtSince;			// Badly hurt since (-1: not)
		LTFLOAT	m_fHealGiveUpUntil;		// Not chasing armor until then (none to be had)
		LTFLOAT	m_fNextHealTry;			// Next look for armor while hurt
		int		m_nGoalZipLink;			// Last zipcord ridden for the goal picked at m_fGoalZipSet
		LTFLOAT	m_fGoalZipSet;
		// Action log
		void	LogGoalChange(LTFLOAT fTime);
		void	DescribeLoadout(char* szBuf, int nLen);
		char	m_szLastAct[512];
		LTFLOAT	m_fLastActTime;
		LTBOOL	m_bLoggedHasGoal, m_bLoggedCamping, m_bWasRetreating;
		LTVector m_vLoggedGoal;
		const char* m_szLoggedWhy;
		uint32	m_nLoggedEnemy;
		LTFLOAT	m_fLoggedEnemyTime;
		int		m_nLane;				// Route (BotMapInfo) we attack along
		int		m_nLaneGen;
		LTBOOL	m_bLaneNext;			// Next PathTo goes along our lane
		const std::vector<LTFLOAT>* GetLaneCost();
		LTVector m_vMoveTarget;			// 3D, for swimming
		LTBOOL	m_bHasMoveTarget;
		LTFLOAT	m_fNextIntelTry;

		// Camping
		LTBOOL	m_bCamping;
		LTVector m_vCampLook;
		LTFLOAT	m_fCampUntil;

		// Retreating to heal
		LTBOOL	m_bRetreating;

		// H.A.R.M. vs UNITY: push into the enemy base, or hold our own
		LTBOOL	m_bDefender;
		LTFLOAT	m_fNextDoorCheck;

		// Zipcord ride
		void	StartZip(int nLink, LTFLOAT fTime);
		void	StopZip();
		LTBOOL	UpdateZip(LTFLOAT fTime);		// LTTRUE while riding
		LTBOOL	UpdateZipLanding(LTFLOAT fTime);	// LTTRUE while stepping off onto the ledge
		LTBOOL	m_bZipping;
		int		m_nZipLink;
		int		m_nZipSpanIdx;			// Path cell the cord lands on (m_SpanPath index)
		LTBOOL	m_bZipLanding;
		LTVector m_vZipLand;
		LTFLOAT	m_fZipLandStart;
		LTVector m_vZipHook;
		LTFLOAT	m_fZipUntil;
		LTFLOAT	m_fZipLineup;		// Since when we've stood at the take-off waiting for a clear ride (-1 = not)
		LTVector m_vZipCheckPos;	// Riding: where we were at the last progress check
		LTFLOAT	m_fZipCheckTime;

		LTBOOL	m_bForceWeaponSelect;

		// Cover (smart bots hide while reloading or hurt)
		LTVector m_vCover;
		LTFLOAT	m_fCoverUntil;

		LTBOOL	IsTeammateInLineOfFire(const LTVector& vFrom, const LTVector& vTo);
		LTFLOAT	m_fNextCallout;

		// Route taste: some swim, some zip, some walk the top.  Links we got
		// stuck on are avoided for a while.
		BotRouteCost			m_Route;
		LTFLOAT					m_fBaseWaterMul;
		std::set<long long>		m_AvoidLinks;
		LTFLOAT					m_fAvoidClear;
		LTBOOL					m_bHasScuba;
		LTBOOL					m_bGoalIsScuba;
		int		CountGuns();

		// Ladders and vehicles
		LTBOOL	m_bOnLadder;
		LTBOOL	m_bGoalIsVehicle;
		LTFLOAT	m_fNextVehicleTry;
		LTBOOL	m_bGoalIsFrontier;		// Heading to the edge of the known map toward the objective
		LTBOOL	m_bExploring;			// Walking straight at the objective past the known map
		LTFLOAT	m_fNextExplore;
		const char* m_szWhy;			// Why the current goal was picked (for the log)
		LTBOOL	m_bGoalIsStage;			// Heading to the wave's staging point
		LTBOOL	m_bWaitingWave;			// Holding at the staging point for the wave to go
		LTFLOAT	m_fRallySince;			// When we got to the rally point (0 = not there)
		LTBOOL	PickVehicleGoal(LTFLOAT fTime);
		void	UpdateVehicle(LTFLOAT fTime, CPlayerObj* pEnemy, LTFLOAT fEnemyDist);
		LTFLOAT	PathLength() const;

		// Progress towards the current waypoint (catches slow "stuck" too)
		LTFLOAT	m_fBestTargetDist;
		int		m_nProgressSpan;		// Furthest path cell reached since the last progress check
		LTFLOAT	m_fProgressCheck;

		// Movement state
		LTFLOAT	m_fStrafeDir;
		LTFLOAT	m_fNextStrafeChange;
		LTFLOAT	m_fNextJump;
		LTFLOAT	m_fNextHop;
		LTFLOAT	m_fNextDodge;
		LTBOOL	m_bWasOnGround;
		LTFLOAT	m_fGlanceYaw;
		LTFLOAT	m_fNextGlance;
		LTBOOL	m_bInWater;
		LTBOOL	m_bHeadInWater;

		LTVector m_vStuckPos;
		LTFLOAT	m_fStuckCheck;
		int		m_nStuckCount;
		LTFLOAT	m_fUnstickUntil;
		LTVector m_vUnstickDir;

		// Damage
		LTFLOAT	m_fLastHealth;
		LTFLOAT	m_fHurtTime;

		// Items we went for recently (so we don't camp on one)
		enum { kVisitedItems = 8 };
		LTVector m_avVisited[kVisitedItems];
		LTFLOAT	m_afVisitedTime[kVisitedItems];
		int		m_nNextVisited;

		LTFLOAT	m_fDeadTime;
		LTFLOAT	m_fLastUpdate;
};

#endif // __BOT_PLAYER_H__
