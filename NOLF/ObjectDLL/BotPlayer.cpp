// ----------------------------------------------------------------------- //
//
// MODULE  : BotPlayer.cpp
//
// PURPOSE : Server controlled deathmatch / H.A.R.M. vs UNITY player
//
//			 A bot is a CPlayerObj without a client.  Instead of reading
//			 client commands and positions it decides what to do itself
//			 (UpdateBot), moves with server physics, and fires its weapon
//			 directly.  Everything else (damage, pickups, frags, respawn,
//			 scoreboard) is the regular player code.
//
//			 Each bot has a play style (BotPersonality) and a skill level.
//			 Style decides what it likes to do; skill decides how well.
//
// ----------------------------------------------------------------------- //

#include "stdafx.h"
#include "BotPlayer.h"
#include "BotMgr.h"
#include "BotNav.h"
#include "GameServerShell.h"
#include "ServerUtilities.h"
#include "Attachments.h"
#include "Weapon.h"
#include "WeaponMgr.h"
#include "FXButeMgr.h"
#include "CharacterHitBox.h"
#include "CommandIDs.h"
#include "MsgIDs.h"
#include <algorithm>
#include <map>
#include <stdarg.h>
#include "SFXMsgIds.h"
#include "ContainerCodes.h"
#include "Destructible.h"
#include "ServerSoundMgr.h"
#include <math.h>

BEGIN_CLASS(CBotPlayer)
END_CLASS_DEFAULT_FLAGS(CBotPlayer, CPlayerObj, NULL, NULL, CF_HIDDEN)

extern CVarTrack g_NetUseSpawnLimit;
extern CVarTrack g_NetSpawnLimit;

#define NO_ENEMY				0xFFFFFFFF
#define BOT_RESPAWN_DELAY		2.5f
#define BOT_SIGHT_DIST			6500.0f		// How far bots can spot an enemy (big maps)
#define BOT_AWARE_DIST			450.0f		// Notice enemies behind us this close
#define BOT_HEARING_DIST		1800.0f		// Gunfire
#define BOT_ARRIVE_DIST			40.0f
#define BOT_ITEM_REACH_DIST		24.0f

static LTBOOL Chance(LTFLOAT fProb);
static LTFLOAT HorizDist(const LTVector& a, const LTVector& b);
static LTFLOAT RandomFloat(LTFLOAT fMin, LTFLOAT fMax);
static LTVector Horiz(const LTVector& v);

// ----------------------------------------------------------------------- //
//	Play styles
// ----------------------------------------------------------------------- //

static const BotPersonality s_aStyles[kNumBotStyles] =
{
	//  name			aggr  jump  camp  range retreat greed team  aware chase smart  weight
	{ "rusher",			1.0f, 0.9f, 0.0f, 0.5f, 0.15f, 0.8f, 0.3f, 0.9f, 10.0f, LTFALSE, 22 },
	{ "camper",			0.2f, 0.1f, 0.85f,2.0f, 0.40f, 0.6f, 0.2f, 1.2f, 1.5f,  LTTRUE,  15 },
	{ "tactician",		0.6f, 0.5f, 0.15f,1.0f, 0.45f, 1.2f, 0.6f, 1.3f, 6.0f,  LTTRUE,  18 },
	{ "scavenger",		0.4f, 0.4f, 0.05f,1.0f, 0.50f, 2.0f, 0.3f, 1.0f, 3.0f,  LTTRUE,  15 },
	{ "teamplayer",		0.5f, 0.4f, 0.10f,1.0f, 0.35f, 1.0f, 1.0f, 1.1f, 5.0f,  LTTRUE,  15 },
	{ "casual",			0.5f, 0.25f,0.10f,1.0f, 0.20f, 0.7f, 0.3f, 0.7f, 3.0f,  LTFALSE, 15 },
};

const BotPersonality& CBotPlayer::GetPersonality(int nStyle)
{
	if (nStyle < 0 || nStyle >= kNumBotStyles) nStyle = eBotCasual;
	return s_aStyles[nStyle];
}

int CBotPlayer::FindStyle(const char* szName)
{
	if (!szName) return -1;
	for (int i = 0; i < kNumBotStyles; i++)
	{
		if (_strnicmp(szName, s_aStyles[i].szName, 4) == 0) return i;
	}
	return -1;
}

int CBotPlayer::PickRandomStyle(LTBOOL bTeamGame)
{
	int nTotal = 0;
	int aWeights[kNumBotStyles];
	for (int i = 0; i < kNumBotStyles; i++)
	{
		aWeights[i] = s_aStyles[i].nWeight;
		if (bTeamGame && i == eBotTeamPlayer) aWeights[i] += 10;
		nTotal += aWeights[i];
	}

	int nPick = GetRandom(0, nTotal - 1);
	for (int i = 0; i < kNumBotStyles; i++)
	{
		if (nPick < aWeights[i]) return i;
		nPick -= aWeights[i];
	}
	return eBotCasual;
}

void CBotPlayer::SetStyle(int nStyle)
{
	if (nStyle < 0 || nStyle >= kNumBotStyles) nStyle = eBotCasual;
	m_nStyle = nStyle;
	m_pP = &s_aStyles[nStyle];

	// How much this personality avoids places where its team keeps dying
	switch (nStyle)
	{
		case eBotRusher:		m_Route.fDangerMul = 0.15f; break;	// charges in anyway
		case eBotCasual:		m_Route.fDangerMul = 0.3f;	break;
		case eBotScavenger:		m_Route.fDangerMul = 0.5f;	break;
		case eBotTeamPlayer:	m_Route.fDangerMul = 0.6f;	break;
		case eBotCamper:		m_Route.fDangerMul = 0.8f;	break;
		case eBotTactician:		m_Route.fDangerMul = 1.0f;	break;	// finds the other way round
		default:				m_Route.fDangerMul = 0.5f;	break;
	}

	// Team role: everybody pushes forward.  When our base is under attack
	// BotMgr::UpdateRecall tells the nearest few to fall back (m_bDefender
	// is switched on for them in UpdateBot), and they go forward again after.
	m_bDefender = LTFALSE;

	// Route taste
	int nTaste = GetRandom(0, 99);

	// On assault maps the underwater sneak route is far safer than the overland
	// gauntlet, so push more attackers (not defenders) into the water.
	int nDiverCut = 25, nZipCut = 55;
	if (!m_bDefender && g_pGameServerShell->GetGameType() == COOPERATIVE_ASSAULT)
	{
		// A bit more than a third of attackers go for scuba and flank through
		// the water; the rest pressure the overland route.
		nDiverCut = 35;
		nZipCut   = 60;
	}

	if (nStyle == eBotRusher) nTaste = (nDiverCut + nZipCut) / 2;	// Rushers love zipcords

	if (nTaste < nDiverCut)
	{
		m_fBaseWaterMul = 0.5f;		// Diver: sneaks through the water
		m_Route.fZipMul = 1.2f;
	}
	else if (nTaste < nZipCut)
	{
		m_fBaseWaterMul = 1.8f;		// Zipcord user
		m_Route.fZipMul = 0.25f;
	}
	else
	{
		m_fBaseWaterMul = 2.5f;		// Stays dry, walks the top
		m_Route.fZipMul = 1.5f;
	}
	// Until a diver actually PICKS UP scuba, avoid deep water (approach the
	// gear from the nearest surface and dive only the last leg, rather than
	// taking a long submerged route and drowning).  Water gets cheap only once
	// scuba is in hand.  Non-divers keep their normal preference.
	m_Route.fWaterMul = (m_fBaseWaterMul < 1.0f) ? 2.0f : m_fBaseWaterMul;
}

void CBotPlayer::Describe(char* szBuf, int nLen)
{
	LTVector vPos;
	g_pLTServer->GetObjectPos(m_hObject, &vPos);

	const char* szTaste = (m_fBaseWaterMul < 1.0f) ? "diver" : (m_Route.fZipMul < 0.5f ? "zipper" : "walker");
	const char* szGoal = !m_bHasGoal ? (m_bCamping ? (m_bWaitingWave ? "waiting" : "camping") : "none") :
		m_bExploring ? "explore" : m_bGoalIsFrontier ? "frontier" : m_bGoalIsStage ? "stage" :
		(m_bGoalIsCamp ? "camp-spot" : (m_bGoalIsItem ? (m_bGoalIsScuba ? "scuba" : (m_bGoalIsObjective ? "documents" : "item")) : (m_bGoalIsSneak ? "sneak" : (m_bGoalIsObjective ? "objective" : "move"))));

	_snprintf(szBuf, nLen-1,
		"%-12s %-10s %-8s %-6s team%d pos(%.0f %.0f %.0f) state%d hp%.0f guns%d goal=%s(%.0f %.0f %.0f) path%d/%d cells%d/%d stuck%d enemy%s water%d zip%d scuba%d frags%d",
		GetNetName(), m_pP->szName, m_bDefender ? "defend" : "attack", szTaste, (int)GetTeamID(),
		vPos.x, vPos.y, vPos.z, (int)GetState(), GetDestructible()->GetHitPoints(), CountGuns(),
		szGoal, m_vGoal.x, m_vGoal.y, m_vGoal.z, m_nPathIndex, (int)m_Path.size(), m_nSpanIdx, (int)m_SpanPath.size(), m_nStuckCount,
		(m_nEnemyID == NO_ENEMY) ? "-" : (m_bEnemyVisible ? "seen" : "lost"),
		m_bInWater ? 1 : 0, m_bZipping ? 1 : 0, m_bHasScuba ? 1 : 0, GetFragCount());
	szBuf[nLen-1] = '\0';

	// Movement details (swimming/ladders/vehicles)
	LTVector vVel;
	g_pLTServer->GetVelocity(m_hObject, &vVel);
	int nUsed = (int)strlen(szBuf);
	_snprintf(szBuf + nUsed, nLen - 1 - nUsed, " vel(%.0f %.0f %.0f) target%s(%.0f %.0f %.0f) head%d ladder%d bike%d avoid%d why=%s armed%d lane%d",
		vVel.x, vVel.y, vVel.z, m_bHasMoveTarget ? "" : "-", m_vMoveTarget.x, m_vMoveTarget.y, m_vMoveTarget.z,
		m_bHeadInWater ? 1 : 0, m_bOnLadder ? 1 : 0, IsRidingVehicle() ? 1 : 0, (int)m_AvoidLinks.size(),
		m_szWhy, HasRangedWeapon() ? 1 : 0, m_nLane);
	szBuf[nLen-1] = '\0';

	// Stuck: what are we pushing against?  (Anything solid, incl. doors)
	if (m_nStuckCount > 0 && m_bHasMoveTarget)
	{
		LTVector vDir = Horiz(m_vMoveTarget - vPos);
		IntersectQuery IQuery;
		IntersectInfo IInfo;
		IQuery.m_Flags	= INTERSECT_OBJECTS | IGNORE_NONSOLID;
		IQuery.m_FilterFn = LTNULL;
		const char* aszLevel[3] = { "feet", "knee", "head" };
		LTFLOAT afY[3] = { -45.0f, -20.0f, 40.0f };
		for (int k = 0; k < 3; k++)
		{
			IQuery.m_From = vPos + LTVector(0.0f, afY[k], 0.0f);
			IQuery.m_To	  = IQuery.m_From + vDir * 60.0f;
			if (!g_pLTServer->IntersectSegment(&IQuery, &IInfo)) continue;
			char szClass[64] = "world";
			if (IInfo.m_hObject && !IsMainWorld(IInfo.m_hObject))
			{
				if (IInfo.m_hObject == m_hObject || IInfo.m_hObject == m_hHitBox) continue;
				g_pLTServer->GetClassName(g_pLTServer->GetObjectClass(IInfo.m_hObject), szClass, sizeof(szClass));
			}
			nUsed = (int)strlen(szBuf);
			_snprintf(szBuf + nUsed, nLen - 1 - nUsed, " blocked-%s:%s", aszLevel[k], szClass);
			szBuf[nLen-1] = '\0';
		}
	}
}

// ----------------------------------------------------------------------- //
//	Motorcycles / snowmobiles
// ----------------------------------------------------------------------- //

LTFLOAT CBotPlayer::PathLength() const
{
	CBotNav& nav = g_BotMgr.GetNav();
	LTFLOAT fLen = 0.0f;
	for (size_t i = (size_t)m_nPathIndex; i + 1 < m_Path.size(); i++)
	{
		fLen += nav.GetNode(m_Path[i]).vPos.Dist(nav.GetNode(m_Path[i+1]).vPos);
	}
	return fLen;
}

// Called when picking a new goal: if a ride is close and we're going far, go
// get the bike first.  (The real destination is picked again after mounting.)
LTBOOL CBotPlayer::PickVehicleGoal(LTFLOAT fTime)
{
	m_fNextVehicleTry = fTime + 15.0f;
	if (IsRidingVehicle() || m_bInWater || !Chance(0.3f + m_pP->fAggression * 0.4f)) return LTFALSE;

	LTVector vPos;
	g_pLTServer->GetObjectPos(m_hObject, &vPos);

	const std::vector<BotItem>& items = g_BotMgr.GetItems();
	for (size_t i = 0; i < items.size(); i++)
	{
		if (items[i].eType != BotItem::eVehicle || !items[i].bAvailable) continue;
		if (vPos.Dist(items[i].vPos) > 700.0f) continue;
		if (!PathTo(items[i].vPos)) continue;

		SetGoal(items[i].vPos, LTFALSE, fTime + 12.0f);
		m_bGoalIsVehicle = LTTRUE;
		return LTTRUE;
	}

	return LTFALSE;
}

void CBotPlayer::UpdateVehicle(LTFLOAT fTime, CPlayerObj* pEnemy, LTFLOAT fEnemyDist)
{
	LTVector vPos;
	g_pLTServer->GetObjectPos(m_hObject, &vPos);

	// Reached the bike: hop on, then head for the far side of the map
	if (m_bGoalIsVehicle && m_bHasGoal && HorizDist(vPos, m_vGoal) < 90.0f)
	{
		m_bGoalIsVehicle = LTFALSE;
		m_bHasGoal = LTFALSE;
		if (g_BotMgr.MountVehicle(m_vGoal, this))
		{
			// Dry, long route: avoid water and zipcords while riding
			m_Route.fWaterMul = 50.0f;
			LTFLOAT fOldZip = m_Route.fZipMul;
			m_Route.fZipMul = 50.0f;

			LTVector vTarget;
			uint32 dwEnemy = (GetTeamID() == TEAM_1) ? TEAM_2 : TEAM_1;
			if (!(g_pGameServerShell->GetGameType() == COOPERATIVE_ASSAULT && g_BotMgr.GetBasePos(dwEnemy, vTarget)))
			{
				int nNode = g_BotMgr.GetNav().GetRandomNode();
				vTarget = (nNode != BOTNAV_INVALID) ? g_BotMgr.GetNav().GetNode(nNode).vPos : vPos;
			}
			if (PathTo(vTarget)) SetGoal(vTarget, LTFALSE, fTime + 30.0f);

			m_Route.fZipMul = fOldZip;
		}
		return;
	}

	if (!IsRidingVehicle()) return;

	// Get off to fight, near the destination, or before water/zipcords/ladders
	LTBOOL bOff = (pEnemy && m_bEnemyVisible && fEnemyDist < 900.0f) || !m_bHasGoal || m_bInWater || m_bOnLadder;
	if (!bOff && HorizDist(vPos, m_vGoal) < 400.0f) bOff = LTTRUE;
	if (!bOff && m_nPathIndex < (int)m_Path.size())
	{
		const BotNavNode& next = g_BotMgr.GetNav().GetNode(m_Path[m_nPathIndex]);
		if (next.nFlags & (BOTNODE_WATER | BOTNODE_LADDER)) bOff = LTTRUE;
	}

	if (bOff)
	{
		DismountVehicle();
		m_Route.fWaterMul = m_bHasScuba ? m_fBaseWaterMul * 0.4f : ((m_fBaseWaterMul < 1.0f) ? 2.0f : m_fBaseWaterMul);
	}
}

// Last stretch to the documents: a human would sprint for them and snap the
// photo while shooting, not stop and strafe with the defenders.
LTBOOL CBotPlayer::IsClosingOnDocuments()
{
	if (!IsObjectiveRun() || !m_bGoalIsItem || !g_BotMgr.HasFeature(GetTeamID(), CBotMgr::eFeatFocus)) return LTFALSE;
	LTVector vPos;
	g_pLTServer->GetObjectPos(m_hObject, &vPos);
	return HorizDist(vPos, m_vGoal) < 700.0f;
}

// This team's trained value for a fighting parameter (BotGenome.h)
LTFLOAT CBotPlayer::Gene(int nGene)
{
	return g_BotMgr.GetGenome(GetTeamID())[nGene];
}

LTBOOL CBotPlayer::IsWaveMember()
{
	if (GetState() != PS_ALIVE || m_bDefender) return LTFALSE;
	if (m_fBaseWaterMul < 1.0f && m_bHasScuba && !m_bSneakDone) return LTFALSE;	// diver: flanks through the water
	return LTTRUE;
}

// ----------------------------------------------------------------------- //
//	Action log: what each bot decides and why, in plain words, one line
//	per action (Bots\actions.txt).
// ----------------------------------------------------------------------- //

void CBotPlayer::Act(const char* szFormat, ...)
{
	char szText[512];
	va_list args;
	va_start(args, szFormat);
	_vsnprintf(szText, sizeof(szText) - 1, szFormat, args);
	va_end(args);
	szText[sizeof(szText) - 1] = '\0';

	// The same thing again right away is noise
	LTFLOAT fTime = g_pLTServer->GetTime();
	if (strcmp(szText, m_szLastAct) == 0 && fTime - m_fLastActTime < 8.0f) return;
	SAFE_STRCPY(m_szLastAct, szText);
	m_fLastActTime = fTime;

	g_BotMgr.LogAction(this, szText);
}

static const char* CurrentWeaponName(CPlayerObj* pPlayer)
{
	CPlayerAttachments* pAttach = pPlayer ? pPlayer->GetAttachmentsForBots() : LTNULL;
	CWeapon* pWeapon = pAttach ? pAttach->GetWeapon() : LTNULL;
	WEAPON* pData = (pWeapon && pWeapon->Have()) ? g_pWeaponMgr->GetWeapon(pWeapon->GetId()) : LTNULL;
	return pData ? pData->szName : "no weapon";
}

// "Walther SMG (power 0.8)": the best gun we carry
void CBotPlayer::DescribeLoadout(char* szBuf, int nLen)
{
	CPlayerAttachments* pAttach = GetPlayerAttachments();
	const char* szBest = "nothing";
	LTFLOAT fBest = 0.0f;
	for (int i = 0; pAttach && i < g_pWeaponMgr->GetNumWeapons(); i++)
	{
		CWeapon* pWeapon = pAttach->GetWeapon((uint8)i);
		if (!pWeapon || !pWeapon->Have()) continue;
		WEAPON* pW = g_pWeaponMgr->GetWeapon(i);
		if (!pW || pW->IsAGadget()) continue;
		for (int a = 0; a < pW->nNumAmmoTypes; a++)
		{
			AMMO* pA = g_pWeaponMgr->GetAmmo(pW->aAmmoTypes[a]);
			if (!pA || (!pW->bInfiniteAmmo && pAttach->GetAmmoCount(pA->nId) <= 0)) continue;
			LTFLOAT f = RateShot(pW, pA, 800.0f);
			if (f > fBest) { fBest = f; szBest = pW->szName; }
		}
	}
	_snprintf(szBuf, nLen - 1, "%s (power %.1f, %s)", szBest, fBest, fBest >= kWellArmed ? "strong" : (fBest >= 1.2f ? "OK" : "weak"));
	szBuf[nLen - 1] = '\0';
}

static int CountTeammatesNear(CPlayerObj* pMe, const LTVector& vPos, LTFLOAT fRadius)
{
	int n = 0;
	const std::vector<CPlayerObj*>& players = g_BotMgr.GetPlayers();
	for (size_t i = 0; i < players.size(); i++)
	{
		CPlayerObj* p = players[i];
		if (p == pMe || p->GetState() != PS_ALIVE || p->GetTeamID() != pMe->GetTeamID()) continue;
		LTVector v;
		g_pLTServer->GetObjectPos(p->m_hObject, &v);
		if (v.Dist(vPos) < fRadius) n++;
	}
	return n;
}

// A new goal (or camping) since last frame: say what and why
void CBotPlayer::LogGoalChange(LTFLOAT fTime)
{
	LTBOOL bChanged = (m_bHasGoal != m_bLoggedHasGoal) || (m_bCamping != m_bLoggedCamping) ||
					  (m_bHasGoal && m_vGoal.DistSqr(m_vLoggedGoal) > 64.0f*64.0f) || (m_bHasGoal && m_szWhy != m_szLoggedWhy);
	m_bLoggedHasGoal = m_bHasGoal;
	m_bLoggedCamping = m_bCamping;
	m_vLoggedGoal	 = m_vGoal;
	m_szLoggedWhy	 = m_szWhy;
	if (!bChanged) return;

	LTVector vPos;
	g_pLTServer->GetObjectPos(m_hObject, &vPos);

	if (m_bCamping)
	{
		LTVector vLook = vPos + m_vCampLook * 1000.0f;
		LTFLOAT fFor = m_fCampUntil - fTime;
		if (m_bPerched)
			Act("Holding the high ground at (%.0f %.0f %.0f) for %.0fs, watching toward (%.0f %.0f).", vPos.x, vPos.y, vPos.z, fFor, vLook.x, vLook.z);
		else if (m_bWaitingWave)
			Act("At the staging point (%.0f %.0f %.0f), waiting for the team to push.", vPos.x, vPos.y, vPos.z);
		else
			Act("Camping at (%.0f %.0f %.0f) for %.0fs, watching toward (%.0f %.0f).", vPos.x, vPos.y, vPos.z, fFor, vLook.x, vLook.z);
		return;
	}
	if (!m_bHasGoal) return;

	// What it is
	char szWhat[200];
	const char* szItem = LTNULL;
	const BotItem* pItem = LTNULL;
	if (m_bGoalIsItem)
	{
		const std::vector<BotItem>& items = g_BotMgr.GetItems();
		for (size_t i = 0; i < items.size(); i++)
		{
			if (items[i].vPos.DistSqr(m_vGoal) < 16.0f*16.0f) { pItem = &items[i]; szItem = items[i].szName[0] ? items[i].szName : LTNULL; break; }
		}
	}
	char szGun[96];
	DescribeLoadout(szGun, sizeof(szGun));
	CPlayerObj* pEnemy = GetEnemy();

	if (!strncmp(m_szWhy, "loot", 4))
		_snprintf(szWhat, sizeof(szWhat) - 1, "My best gun is the %s - going for the %s", szGun, szItem ? szItem : "pickup");
	else if (m_bGoalIsScuba)			strcpy(szWhat, "Fetching SCUBA gear for the underwater route");
	else if (m_bGoalIsItem && m_bGoalIsObjective) strcpy(szWhat, "Going for the enemy documents");
	else if (m_bGoalIsItem)
	{
		const char* szNeed = "useful";
		if (pItem && pItem->eType == BotItem::eGear && pItem->bArmor)
			szNeed = (!strcmp(m_szWhy, "retreat") || !strcmp(m_szWhy, "item-hurt")) ? "need health" : "armor";
		else if (pItem && (pItem->eType == BotItem::eWeapon || pItem->eType == BotItem::eAmmo))
			szNeed = !strcmp(m_szWhy, "item-noammo") ? "out of ammo" : (pItem->eType == BotItem::eWeapon ? "better gun" : "ammo");
		_snprintf(szWhat, sizeof(szWhat) - 1, "Going for the %s (%s)", szItem ? szItem : "pickup", szNeed);
	}
	else if (m_bGoalIsStage)			strcpy(szWhat, "Moving up to the staging point for the attack");
	else if (m_bGoalIsSneak)			strcpy(szWhat, "Swimming the underwater route toward their base");
	else if (m_bGoalIsCamp)				strcpy(szWhat, m_bDefender ? "Taking a defensive spot" : "Heading to a good spot to hold");
	else if (m_bGoalIsVehicle)			strcpy(szWhat, "Getting on the motorcycle");
	else if (m_bGoalIsFrontier)			strcpy(szWhat, "No known way to the objective - going as far as I can");
	else if (m_bGoalIsObjective)		strcpy(szWhat, "Pushing toward the enemy base");
	else if (!strcmp(m_szWhy, "chase"))
		_snprintf(szWhat, sizeof(szWhat) - 1, "Chasing %s where I last saw them", pEnemy ? pEnemy->GetNetName() : "the enemy");
	else if (!strcmp(m_szWhy, "intercept"))	strcpy(szWhat, "Enemy in our base - going to intercept");
	else if (!strcmp(m_szWhy, "leash"))		strcpy(szWhat, "Back to guard our documents");
	else if (!strcmp(m_szWhy, "buddy"))		strcpy(szWhat, "Joining up with a teammate");
	else if (!strcmp(m_szWhy, "heard"))		strcpy(szWhat, "Heard gunfire - checking it out");
	else if (!strcmp(m_szWhy, "out-of-water")) strcpy(szWhat, "Stuck in the water - getting out to dry land");
	else if (!strcmp(m_szWhy, "wander"))	strcpy(szWhat, "Nothing to do - roaming");
	else _snprintf(szWhat, sizeof(szWhat) - 1, "Moving (%s)", m_szWhy);
	szWhat[sizeof(szWhat) - 1] = '\0';

	// How we get there
	char szRoute[200] = "";
	if (!m_Path.empty())
	{
		CBotNav& nav = g_BotMgr.GetNav();
		LTFLOAT fLen = 0.0f;
		int nZips = 0, nJumps = 0, nWater = 0;
		for (size_t i = 0; i + 1 < m_Path.size(); i++)
		{
			fLen += nav.GetNode(m_Path[i]).vPos.Dist(nav.GetNode(m_Path[i + 1]).vPos);
			const BotNavLink* pLink = nav.GetLink(m_Path[i], m_Path[i + 1]);
			if (pLink && (pLink->nFlags & BOTLINK_ZIP)) nZips++;
			if (pLink && (pLink->nFlags & (BOTLINK_GAP | BOTLINK_JUMP))) nJumps++;
			if (nav.GetNode(m_Path[i + 1]).nFlags & BOTNODE_WATER) nWater++;
		}
		static const char* s_aszLane[] = { "main road", "flank", "high route", "water route" };
		const char* szLane = LTNULL;
		if (m_Route.pLane && m_nLane >= 0 && m_nLane < g_BotMgr.GetMapInfo().GetNumRoutes(GetTeamID()))
		{
			szLane = s_aszLane[g_BotMgr.GetMapInfo().GetRoute(GetTeamID(), m_nLane).nKind];
		}
		_snprintf(szRoute, sizeof(szRoute) - 1, ", route %.0fu over %d waypoints%s%s%s%s",
			fLen, (int)m_Path.size(), szLane ? " along the " : "", szLane ? szLane : "",
			nZips ? " (zipcord)" : "", nWater * 3 > (int)m_Path.size() ? " (swimming)" : "");
		szRoute[sizeof(szRoute) - 1] = '\0';
	}

	Act("%s at (%.0f %.0f %.0f), %.0fu away%s.", szWhat, m_vGoal.x, m_vGoal.y, m_vGoal.z, vPos.Dist(m_vGoal), szRoute);
}

// Our lane's cost table (picks a lane when the team's routes change)
const std::vector<LTFLOAT>* CBotPlayer::GetLaneCost()
{
	if (g_pGameServerShell->GetGameType() != COOPERATIVE_ASSAULT || m_bDefender) return LTNULL;
	CBotMapInfo& info = g_BotMgr.GetMapInfo();
	int nTeam = (int)GetTeamID();
	if (info.GetNumRoutes(nTeam) == 0) return LTNULL;

	if (m_nLaneGen != g_BotMgr.GetRouteGen(GetTeamID()) || m_nLane < 0 || m_nLane >= info.GetNumRoutes(nTeam))
	{
		m_nLaneGen = g_BotMgr.GetRouteGen(GetTeamID());
		m_nLane = info.PickRoute(nTeam, m_fBaseWaterMul < 1.0f, m_Route.fZipMul < 0.5f, m_bHasScuba);
	}
	if (m_nLane < 0) return LTNULL;
	return &info.GetRoute(nTeam, m_nLane).laneCost;
}

// Only plain walking along the path for the next few cells (no ledge to
// jump onto, no gap, no drop, no zipcord): safe to hop.
LTBOOL CBotPlayer::IsPlainAhead(int nSteps)
{
	if (m_SpanPath.empty()) return !m_bInWater && !m_bOnLadder;
	CBotWorld& world = g_BotMgr.GetWorld();
	for (int i = m_nSpanIdx; i < m_nSpanIdx + nSteps && i < (int)m_SpanPath.size(); i++)
	{
		int nLink;
		if (SpanStepKind(i, nLink) != eStepWalk) return LTFALSE;
		if (world.GetSpan(m_SpanPath[i]).nFlags & (BOTSPAN_LEDGE | BOTSPAN_SWIM | BOTSPAN_LADDER)) return LTFALSE;
	}
	return LTTRUE;
}

static const char* s_szOverwatch = "overwatch";

// Does picking up armor heal on this server?  (CDestructible::AddGear heals by
// NetArmorHealthPercent x armor; the host menu can set it to 0, and then there
// is no healing in multiplayer at all - running off to find some is useless.)
extern CVarTrack g_NetArmorHealthPercent;
static LTBOOL ArmorHeals()
{
	return g_NetArmorHealthPercent.IsInitted() && g_NetArmorHealthPercent.GetFloat() > 0.0f;
}

// Snipers, campers and some tacticians: take a perch forward of our base that
// overlooks where the enemy comes through, go up there and cover the team.
LTBOOL CBotPlayer::PickOverwatchGoal(LTFLOAT fTime)
{
	if (m_bInWater) return LTFALSE;
	LTFLOAT fWant = (m_nStyle == eBotCamper) ? 0.7f : (m_nStyle == eBotTactician ? 0.35f : 0.1f);
	if (GetPreferredRange() > 900.0f) fWant += 0.5f;		// carrying a long gun
	if (!Chance(fWant)) return LTFALSE;

	LTVector vOurBase, vTarget;
	uint32 dwEnemy = (GetTeamID() == TEAM_1) ? TEAM_2 : TEAM_1;
	if (!g_BotMgr.GetBasePos(GetTeamID(), vOurBase) || !g_BotMgr.GetPushTarget(GetTeamID(), vTarget)) return LTFALSE;

	// Somewhere 35-65% of the way over, high, overlooking a lot
	LTVector vNear = vOurBase + (vTarget - vOurBase) * RandomFloat(0.35f, 0.65f);
	LTVector vLook;
	int nNode = g_BotMgr.PickCampNode(vNear, vLook, LTTRUE);
	if (nNode == BOTNAV_INVALID) return LTFALSE;

	const LTVector& vSpot = g_BotMgr.GetNav().GetNode(nNode).vPos;
	if (!PathTo(vSpot)) return LTFALSE;
	SetGoal(vSpot, LTFALSE, fTime + 60.0f);
	m_bGoalIsCamp = LTTRUE;
	m_vCampLook	  = vLook;
	m_szWhy		  = s_szOverwatch;
	return LTTRUE;
}

// We got up somewhere high (zipcord, ladder, stairs to a roof).  The point of
// going up there is the view: snipers, campers and defenders hold it and
// cover the ground below instead of jumping straight back down.
void CBotPlayer::OnReachedHighGround(LTFLOAT fTime)
{
	if (!HasRangedWeapon() || m_bRetreating || m_bInWater) return;

	LTVector vPos;
	g_pLTServer->GetObjectPos(m_hObject, &vPos);

	CBotNav& nav = g_BotMgr.GetNav();
	int n = nav.FindNearest(vPos, 200.0f, LTFALSE);
	if (n < 0 || !(nav.GetNode(n).nFlags & BOTNODE_HIGH)) return;

	// Only worth it if it overlooks a lot (BotMgr's overwatch score), and is
	// where the fight is: forward of our base when attacking, by the
	// documents we guard when defending
	LTFLOAT fScore = g_BotMgr.GetCampScore(n);
	if (fScore >= 0.0f && fScore < 6.0f) { Act("Up high at (%.0f %.0f %.0f), but it doesn't overlook much - moving on.", vPos.x, vPos.y, vPos.z); return; }
	if (g_pGameServerShell->GetGameType() == COOPERATIVE_ASSAULT)
	{
		LTVector vOurBase, vGuard;
		uint32 dwEnemy = (GetTeamID() == TEAM_1) ? TEAM_2 : TEAM_1;
		LTVector vEnemyBase;
		if (m_bDefender)
		{
			if (g_BotMgr.GetGuardTarget(GetTeamID(), vGuard) && HorizDist(vPos, vGuard) > 2000.0f) { Act("Up high, but too far from what I guard - moving on."); return; }
		}
		else if (g_BotMgr.GetBasePos(GetTeamID(), vOurBase) && g_BotMgr.GetBasePos(dwEnemy, vEnemyBase))
		{
			if (HorizDist(vPos, vOurBase) < HorizDist(vPos, vEnemyBase) * 0.6f) { Act("Up on our own base - no use attacking from here, moving on."); return; }	// still at home
		}
	}

	LTFLOAT fHold;
	switch (m_nStyle)
	{
		case eBotCamper:		fHold = 0.95f; break;
		case eBotTactician:		fHold = 0.7f;  break;
		case eBotTeamPlayer:	fHold = 0.4f;  break;
		case eBotRusher:		fHold = 0.1f;  break;
		default:				fHold = 0.35f; break;
	}
	// Long guns love it up there
	if (GetPreferredRange() > 900.0f) fHold += 0.25f;
	if (m_bDefender) fHold = LTMAX(fHold, 0.6f);
	else if (IsObjectiveRun()) fHold *= 0.4f;	// on the way to the documents
	if (m_bGoalIsCamp || m_szWhy == s_szOverwatch) fHold = 1.0f;	// came here to hold it
	if (!Chance(fHold)) { Act("Up high, but I'd rather keep moving."); return; }

	LTVector vLook;
	if (!g_BotMgr.GetCampLook(n, vLook))
	{
		// Toward where the enemy comes from
		LTVector vTarget;
		uint32 dwEnemy = (GetTeamID() == TEAM_1) ? TEAM_2 : TEAM_1;
		vLook = g_BotMgr.GetBasePos(dwEnemy, vTarget) ? Horiz(vTarget - vPos) : LTVector(LTFLOAT(sin(m_fYaw)), 0.0f, LTFLOAT(cos(m_fYaw)));
	}

	m_bHasGoal	 = LTFALSE;
	ClearPath();
	m_bCamping	 = LTTRUE;
	m_bPerched	 = LTTRUE;
	m_vCampLook	 = vLook;
	m_vCampSpot	 = vPos;
	m_fCampUntil = fTime + RandomFloat(20.0f, 45.0f) * (m_nStyle == eBotCamper ? 1.5f : 1.0f);
	m_szWhy		 = "perch";
}

LTBOOL CBotPlayer::IsObjectiveRun() const
{
	return m_bHasGoal && m_bGoalIsObjective && g_pGameServerShell->GetGameType() == COOPERATIVE_ASSAULT;
}

// Photograph any of our team's documents we happen to be standing next to,
// no matter what our current goal is (fighting right beside them still counts).
// Scoring only needs the "GADGET" trigger, not the camera gadget in hand.
void CBotPlayer::TryPhotograph(LTFLOAT fTime)
{
	if (g_pGameServerShell->GetGameType() != COOPERATIVE_ASSAULT) return;
	if (fTime < m_fNextIntelTry) return;

	LTVector vPos;
	g_pLTServer->GetObjectPos(m_hObject, &vPos);

	const std::vector<BotItem>& items = g_BotMgr.GetItems();
	for (size_t i = 0; i < items.size(); i++)
	{
		const BotItem& it = items[i];
		if (it.eType != BotItem::eIntel || !it.bAvailable) continue;
		if (it.nTeam != 0 && it.nTeam != GetTeamID()) continue;	// not ours to score
		if (HorizDist(vPos, it.vPos) > 110.0f || (LTFLOAT)fabs(vPos.y - it.vPos.y) > 80.0f) continue;

		if (g_BotMgr.ActivateIntel(it.vPos, this))
		{
			m_fNextIntelTry = fTime + 1.5f;
			if (Chance(0.6f)) g_BotMgr.BotSay(this, Chance(0.5f) ? "Got the documents!" : "Intel photographed", LTTRUE);
		}
		return;
	}
}

int CBotPlayer::CountGuns()
{
	CPlayerAttachments* pAttach = GetPlayerAttachments();
	if (!pAttach || !g_pWeaponMgr) return 0;

	int nGuns = 0;
	for (int i = 0; i < g_pWeaponMgr->GetNumWeapons(); i++)
	{
		CWeapon* pWeapon = pAttach->GetWeapon((uint8)i);
		WEAPON* pData = g_pWeaponMgr->GetWeapon(i);
		if (pWeapon && pWeapon->Have() && pData && !pData->IsAGadget() && !IsMelee(pData)) nGuns++;
	}
	return nGuns;
}

// Run speed, including the server's run speed option (fast servers)
LTFLOAT CBotPlayer::GetMoveSpeed()
{
	static CVarTrack s_vtRunSpeed;
	if (!s_vtRunSpeed.IsInitted())
	{
		s_vtRunSpeed.Init(g_pLTServer, "RunSpeed", LTNULL, 1.0f);
	}

	LTFLOAT fMul = s_vtRunSpeed.GetFloat(1.0f);
	if (fMul < 0.1f) fMul = 1.0f;
	return GetRunVel() * fMul;
}

// ----------------------------------------------------------------------- //
//	Helpers
// ----------------------------------------------------------------------- //

static LTFLOAT RandomFloat(LTFLOAT fMin, LTFLOAT fMax)
{
	return fMin + (fMax - fMin) * ((LTFLOAT)GetRandom(0, 10000) / 10000.0f);
}

static LTBOOL Chance(LTFLOAT fProb)
{
	return RandomFloat(0.0f, 1.0f) < fProb;
}

static LTFLOAT HorizDist(const LTVector& a, const LTVector& b)
{
	LTFLOAT dx = a.x - b.x;
	LTFLOAT dz = a.z - b.z;
	return (LTFLOAT)sqrt(dx*dx + dz*dz);
}

static LTVector Horiz(const LTVector& v)
{
	LTVector r(v.x, 0.0f, v.z);
	LTFLOAT fMag = r.Mag();
	if (fMag > 0.001f) r /= fMag;
	else r.Init();
	return r;
}

static LTVector RotateYaw(const LTVector& v, LTFLOAT fAngle)
{
	LTFLOAT c = (LTFLOAT)cos(fAngle), s = (LTFLOAT)sin(fAngle);
	return LTVector(v.x * c + v.z * s, v.y, -v.x * s + v.z * c);
}

// ----------------------------------------------------------------------- //

CBotPlayer::CBotPlayer() : CPlayerObj()
{
	m_nStyle			= eBotCasual;
	m_pP				= &s_aStyles[eBotCasual];
	m_fSkill			= 0.5f;

	m_nEnemyID			= NO_ENEMY;
	m_bEnemyVisible		= LTFALSE;
	m_vEnemyLastPos.Init();
	m_vEnemyVel.Init();
	m_vEnemyPrevPos.Init();
	m_fEnemyPrevTime	= -1.0f;
	m_fEnemyLastSeen	= -100.0f;
	m_fEnemyAcquired	= 0.0f;
	m_fNextEnemyCheck	= 0.0f;

	m_vHeardPos.Init();
	m_fHeardTime		= -100.0f;

	m_fYaw				= 0.0f;
	m_vAimDir.Init(0.0f, 0.0f, 1.0f);
	m_vAimError.Init();
	m_fNextAimError		= 0.0f;
	m_bAimHead			= LTFALSE;

	m_fNextWeaponCheck	= 0.0f;
	m_nShotsInClip		= 0;
	m_fReloadDone		= 0.0f;
	memset(m_anClipUsed, 0, sizeof(m_anClipUsed));
	m_fLastFire			= -100.0f;
	m_fNextShot			= 0.0f;
	m_nBurstShots		= 0;
	m_nBurstLen			= 0;

	m_vGoal.Init();
	m_bHasGoal			= LTFALSE;
	m_bGoalIsItem		= LTFALSE;
	m_bGoalIsCamp		= LTFALSE;
	m_fGoalTimeout		= 0.0f;
	m_nPathIndex		= 0;
	m_vMoveTarget.Init();
	m_bHasMoveTarget	= LTFALSE;
	m_fNextIntelTry		= 0.0f;

	m_bCamping			= LTFALSE;
	m_vCampLook.Init(0.0f, 0.0f, 1.0f);
	m_fCampUntil		= 0.0f;
	m_bRetreating		= LTFALSE;
	m_bDefender			= LTFALSE;
	m_fNextDoorCheck	= 0.0f;
	m_bZipping			= LTFALSE;
	m_nZipLink			= BOTW_INVALID;
	m_nZipSpanIdx		= 0;
	m_bZipLanding		= LTFALSE;
	m_vZipLand.Init();
	m_fZipLandStart		= 0.0f;
	m_nSpanIdx			= 0;
	m_nCorridorEnd		= 0;
	m_nGoalSpan			= BOTW_INVALID;
	m_nLookIdx			= 0;
	m_fNextLook			= 0.0f;
	m_fOffPathSince		= -1.0f;
	m_nReplans			= 0;
	m_nJumpLink			= BOTW_INVALID;
	m_fJumpTime			= 0.0f;
	m_vJumpFrom.Init();
	m_vCampSpot.Init();
	m_bWasCamping		= LTFALSE;
	m_bPerched			= LTFALSE;
	m_fGoalSetTime		= 0.0f;
	m_szLastAct[0]		= '\0';
	m_fLastActTime		= -100.0f;
	m_bLoggedHasGoal	= LTFALSE;
	m_bLoggedCamping	= LTFALSE;
	m_bWasRetreating	= LTFALSE;
	m_vLoggedGoal.Init();
	m_szLoggedWhy		= "";
	m_nLoggedEnemy		= 0xFFFFFFFF;
	m_fLoggedEnemyTime	= -100.0f;
	m_fLootUntil		= 0.0f;
	m_bFirstSpawn		= LTTRUE;
	strcpy(m_szLootWhy, "loot");
	m_nLane				= -1;
	m_nLaneGen			= -1;
	m_bLaneNext			= LTFALSE;
	m_vZipHook.Init();
	m_fZipUntil			= 0.0f;
	m_fZipLineup		= -1.0f;
	m_vZipCheckPos.Init();
	m_fZipCheckTime		= 0.0f;
	m_bForceWeaponSelect = LTTRUE;
	m_vCover.Init();
	m_fCoverUntil		= 0.0f;
	m_fNextCallout		= 0.0f;

	m_Route.fWaterMul	= 1.5f;
	m_Route.fZipMul		= 1.0f;
	m_Route.fNoAirMul	= 8.0f;
	m_Route.pLane		= LTNULL;
	m_Route.pAvoid		= &m_AvoidLinks;
	m_Route.pDanger		= LTNULL;
	m_Route.fDangerMul	= 0.5f;
	m_fBaseWaterMul		= 1.5f;
	m_fAvoidClear		= 0.0f;
	m_bHasScuba			= LTFALSE;
	m_bGoalIsScuba		= LTFALSE;
	m_bOnLadder			= LTFALSE;
	m_bGoalIsSneak		= LTFALSE;
	m_bSneakDone		= LTFALSE;
	m_bGoalIsObjective	= LTFALSE;
	m_bGoalIsVehicle	= LTFALSE;
	m_fNextVehicleTry	= 0.0f;
	m_bGoalIsFrontier	= LTFALSE;
	m_bExploring		= LTFALSE;
	m_fNextExplore		= 0.0f;
	m_bGoalIsStage		= LTFALSE;
	m_bWaitingWave		= LTFALSE;
	m_fRallySince		= 0.0f;
	m_szWhy				= "-";
	m_fBestTargetDist	= 1.0e30f;
	m_nProgressSpan		= 0;
	m_fProgressCheck	= 0.0f;

	m_fStrafeDir		= 1.0f;
	m_fNextStrafeChange	= 0.0f;
	m_fNextJump			= 0.0f;
	m_fNextHop			= 0.0f;
	m_fNextDodge		= 0.0f;
	m_bWasOnGround		= LTTRUE;
	m_fGlanceYaw		= 0.0f;
	m_fNextGlance		= 0.0f;
	m_bInWater			= LTFALSE;
	m_bHeadInWater		= LTFALSE;

	m_vStuckPos.Init();
	m_fStuckCheck		= 0.0f;
	m_nStuckCount		= 0;
	m_fUnstickUntil		= 0.0f;
	m_vUnstickDir.Init();

	m_fLastHealth		= -1.0f;
	m_fHurtTime			= -100.0f;

	for (int i = 0; i < kVisitedItems; i++)
	{
		m_avVisited[i].Init();
		m_afVisitedTime[i] = -100.0f;
	}
	m_nNextVisited		= 0;
	m_nGoalZipLink		= -1;
	m_fNextHealTry		= 0.0f;
	m_fHurtSince		= -1.0f;
	m_fHealGiveUpUntil	= 0.0f;
	m_fShortLookUntil	= 0.0f;
	m_vOffPathPos.Init();
	m_fOffPathTime		= -100.0f;
	m_nOffPathRepeats	= 0;
	m_fGoalZipSet		= -1.0f;

	m_fDeadTime			= -1.0f;
	m_fLastUpdate		= 0.0f;
}

LTBOOL CBotPlayer::NeedsHealing(LTFLOAT fHealth)
{
	LTFLOAT fTime = g_pLTServer->GetTime();
	if (!ArmorHeals()) return LTFALSE;		// nothing heals here: fight on
	LTBOOL bHurt = fHealth <= 0.35f || fHealth < m_pP->fRetreatHealth * Gene(eGeneRetreat);
	if (!bHurt) { m_fHurtSince = -1.0f; return LTFALSE; }
	if (m_fHurtSince < 0.0f) m_fHurtSince = fTime;

	// No armor to be had for a while (others keep taking it): like a player,
	// give up on it for now and get back in the fight
	if (fTime < m_fHealGiveUpUntil) return LTFALSE;
	if (fTime - m_fHurtSince > 30.0f)
	{
		m_fHealGiveUpUntil = fTime + 45.0f;
		m_fHurtSince = fTime + 45.0f;	// (then another 30 s of trying)
		Act("No armor to be had - fighting on hurt (%.0f hp).", GetDestructible()->GetHitPoints());
		return LTFALSE;
	}
	return LTTRUE;
}

// Heading for armor because we're hurt
LTBOOL CBotPlayer::IsHealGoal() const
{
	return m_bHasGoal && m_bGoalIsItem && (!strcmp(m_szWhy, "retreat") || !strcmp(m_szWhy, "item-hurt"));
}

LTBOOL CBotPlayer::RecentlyVisited(const LTVector& vPos, LTFLOAT fTime) const
{
	for (int i = 0; i < kVisitedItems; i++)
	{
		if (fTime - m_afVisitedTime[i] < 15.0f && m_avVisited[i].DistSqr(vPos) < 32.0f*32.0f)
		{
			return LTTRUE;
		}
	}
	return LTFALSE;
}

LTFLOAT CBotPlayer::GetHealthFraction()
{
	CDestructible* pDamage = GetDestructible();
	LTFLOAT fMax = pDamage->GetMaxHitPoints();
	return (fMax > 0.0f) ? pDamage->GetHitPoints() / fMax : 1.0f;
}

LTBOOL CBotPlayer::IsEnemyOf(CPlayerObj* pPlayer)
{
	if (!pPlayer || pPlayer == this) return LTFALSE;
	if (g_pGameServerShell->GetGameType() == COOPERATIVE_ASSAULT && pPlayer->GetTeamID() == GetTeamID()) return LTFALSE;
	return LTTRUE;
}

// ----------------------------------------------------------------------- //
//
//	ROUTINE:	CBotPlayer::UpdateBot
//
//	PURPOSE:	Called every update (instead of reading client input)
//
// ----------------------------------------------------------------------- //

void CBotPlayer::UpdateBot()
{
	LTFLOAT fTime  = g_pLTServer->GetTime();
	LTFLOAT fDelta = fTime - m_fLastUpdate;
	if (fDelta > 0.25f || fDelta < 0.0f) fDelta = 0.25f;
	m_fLastUpdate = fTime;

	ClearBotCommands();

	if (GetState() != PS_ALIVE || GetDestructible()->IsDead())
	{
		UpdateDead(fTime);
		return;
	}
	m_fDeadTime = -1.0f;
	if (m_bFirstSpawn)
	{
		m_bFirstSpawn = LTFALSE;
		StartLooting(fTime);
		char szGun[96];
		DescribeLoadout(szGun, sizeof(szGun));
		Act("Spawned: %s, likes the %s, team %d. Carrying the %s.", m_pP->szName,
			(m_fBaseWaterMul < 1.0f) ? "water" : (m_Route.fZipMul < 0.5f ? "zipcords" : "dry routes"), (int)GetTeamID(), szGun);
	}

	// Ground state (normally reported by the client)...

	CollisionInfo info;
	g_pLTServer->GetStandingOn(m_hObject, &info);
	LTBOOL bOnGround = (info.m_hObject != LTNULL);
	SetOnGround(bOnGround);

	if (bOnGround && !m_bWasOnGround && GetMotionStatus() != MS_NONE)
	{
		SetMotionStatus(MS_LANDED);
	}
	m_bWasOnGround = bOnGround;

	UpdateWater();

	// Knocked out by sleeping gas / stunned: like a player whose client
	// blocks all input until it wears off.
	uint32 dwUsrFlags = g_pLTServer->GetObjectUserFlags(m_hObject);
	if (dwUsrFlags & (USRFLG_CHAR_SLEEPING | USRFLG_CHAR_STUNNED))
	{
		if (m_bZipping) StopZip();
		if (IsRidingVehicle()) DismountVehicle();

		LTVector vVel;
		g_pLTServer->GetVelocity(m_hObject, &vVel);
		vVel.x = vVel.z = 0.0f;
		if (m_bInWater && !m_bHasScuba) vVel.y = 30.0f;	// Float up, don't drown asleep
		g_pLTServer->SetVelocity(m_hObject, &vVel);

		if (dwUsrFlags & USRFLG_CHAR_SLEEPING) SetBotCommand(COMMAND_ID_DUCK, LTTRUE);
		m_fEnemyAcquired = fTime + 0.8f;	// Groggy when we come to
		return;
	}

	// Think...

	UpdateEnemy(fTime);
	UpdateHurt(fTime);

	CPlayerObj* pEnemy = GetEnemy();
	LTFLOAT fEnemyDist = 0.0f;
	if (pEnemy)
	{
		LTVector vPos, vEnemyPos;
		g_pLTServer->GetObjectPos(m_hObject, &vPos);
		g_pLTServer->GetObjectPos(pEnemy->m_hObject, &vEnemyPos);
		fEnemyDist = vPos.Dist(vEnemyPos);
	}

	// Hurt badly?  Break off and go heal (we still shoot back while running).
	m_bRetreating = (pEnemy && GetHealthFraction() < m_pP->fRetreatHealth * Gene(eGeneRetreat) && m_fSkill > 0.2f && ArmorHeals());
	if (m_bRetreating && !m_bWasRetreating) Act("Hurt (%.0f hp) - breaking off to find health.", GetDestructible()->GetHitPoints());
	m_bWasRetreating = m_bRetreating;

	// Team play: fall back when the team calls us home (enemy in our base),
	// push forward again once it's quiet.
	if (g_pGameServerShell->GetGameType() == COOPERATIVE_ASSAULT)
	{
		LTBOOL bRecall = g_BotMgr.IsRecalled(GetPlayerID(), GetTeamID());
		if (bRecall != m_bDefender)
		{
			Act(bRecall ? "Enemy in our base - falling back to defend." : "Base is quiet again - pushing forward.");
			m_bDefender	 = bRecall;
			if (!IsHealGoal())	// (finish a health run first)
			{
				m_bHasGoal	 = LTFALSE;
				m_bCamping	 = LTFALSE;
				m_bExploring = LTFALSE;
				ClearPath();
			}
			if (bRecall && Chance(0.35f))
			{
				g_BotMgr.BotSay(this, Chance(0.5f) ? "Falling back to defend!" : "On my way back to base!", LTTRUE);
			}
		}
	}

	ChooseWeapon(fTime, pEnemy ? fEnemyDist : (m_bCamping ? 1500.0f : 1000.0f));
	UpdateGoal(fTime);
	LogGoalChange(fTime);
	TryPhotograph(fTime);	// Snap documents whenever we're next to them
	UpdateVehicle(fTime, pEnemy, fEnemyDist);

	// Riding a zipcord: no walking, but we can still look and shoot
	if (m_bZipping && UpdateZip(fTime))
	{
		UpdateAim(fTime, fDelta);	// (No shooting on a zipcord, same as players)
		return;
	}
	if (m_bZipLanding && UpdateZipLanding(fTime))
	{
		UpdateAim(fTime, fDelta);
		return;
	}

	// Remember where we set up camp (side steps never leave it)
	if (m_bCamping && !m_bWasCamping) g_pLTServer->GetObjectPos(m_hObject, &m_vCampSpot);
	m_bWasCamping = m_bCamping;

	// Move...

	LTBOOL bJump = LTFALSE;
	LTVector vMoveDir;
	m_bHasMoveTarget = LTFALSE;

	if (fTime < m_fUnstickUntil)
	{
		vMoveDir = m_vUnstickDir;
	}
	else if (pEnemy && m_bEnemyVisible && !m_bRetreating && !IsRidingVehicle() && (fEnemyDist < 400.0f || HasRangedWeapon()) &&
		!(IsObjectiveRun() && fEnemyDist > 600.0f && fTime - m_fHurtTime > 2.0f) && !IsClosingOnDocuments())
	{
		vMoveDir = GetCombatMoveDir(fTime, bJump);
	}
	else
	{
		vMoveDir = GetNavMoveDir(fTime, bJump);

		// Bunny hops while running around (some people just do that)...
		if (fTime > m_fNextHop && vMoveDir.MagSqr() > 0.01f && !m_bCamping && !IsRidingVehicle() && !m_bOnLadder)
		{
			if (Chance(m_pP->fJumpiness * 0.8f * Gene(eGeneJump)) && IsPlainAhead(5) && IsHopSafe(vMoveDir)) bJump = LTTRUE;
			m_fNextHop = fTime + RandomFloat(1.0f, 4.0f) / (0.3f + m_pP->fJumpiness);
		}
	}

	// Open doors in our way...
	if (vMoveDir.MagSqr() > 0.01f && fTime > m_fNextDoorCheck)
	{
		LTVector vPos;
		g_pLTServer->GetObjectPos(m_hObject, &vPos);
		g_BotMgr.OpenDoorsAhead(this, vPos, vMoveDir);
		m_fNextDoorCheck = fTime + 0.75f;
	}

	UpdateStuck(fTime, vMoveDir.MagSqr() > 0.01f, bJump);
	ApplyMovement(fTime, vMoveDir, bJump);

	// Look / shoot...

	UpdateAim(fTime, fDelta);
	UpdateFire(fTime);

	SetMoveCommands(vMoveDir);
}

// ----------------------------------------------------------------------- //
//	Water (normally the client tracks containers for players)
// ----------------------------------------------------------------------- //

static LTBOOL IsPointInLiquid(const LTVector& vPoint, ContainerCode& eCode)
{
	HOBJECT objList[MAX_TRACKED_CONTAINERS];
	LTVector vPos = vPoint;
	uint32 nCount = g_pLTServer->GetPointContainers(&vPos, objList, MAX_TRACKED_CONTAINERS);

	for (uint32 i = 0; i < nCount; i++)
	{
		uint16 nCode;
		if (g_pLTServer->GetContainerCode(objList[i], &nCode) && IsLiquid((ContainerCode)nCode))
		{
			eCode = (ContainerCode)nCode;
			return LTTRUE;
		}
	}

	eCode = CC_NO_CONTAINER;
	return LTFALSE;
}

void CBotPlayer::UpdateWater()
{
	LTVector vPos;
	g_pLTServer->GetObjectPos(m_hObject, &vPos);

	ContainerCode eHead, eBody;
	m_bHeadInWater	= IsPointInLiquid(vPos + LTVector(0.0f, 35.0f, 0.0f), eHead);
	m_bInWater		= IsPointInLiquid(vPos, eBody);

	// Drives the swim animation and drowning, like for real players
	m_eContainerCode = m_bHeadInWater ? eHead : CC_NO_CONTAINER;
	m_bBodyInLiquid	 = m_bInWater;

	m_bOnLadder		 = CBotNav::IsInLadder(vPos) || CBotNav::IsInLadder(vPos - LTVector(0.0f, 40.0f, 0.0f));
	m_bBodyOnLadder	 = m_bOnLadder;
}

// ----------------------------------------------------------------------- //
//	Death / respawn
// ----------------------------------------------------------------------- //

void CBotPlayer::ForgetNav()
{
	ClearPath();
	m_bHasGoal	 = LTFALSE;
	m_nGoalSpan	 = BOTW_INVALID;
	m_nLane		 = -1;
	m_nLaneGen	 = -1;
	m_bLaneNext	 = LTFALSE;
}

void CBotPlayer::UpdateDead(LTFLOAT fTime)
{
	m_nEnemyID		= NO_ENEMY;
	m_bHasGoal		= LTFALSE;
	m_bCamping		= LTFALSE;
	m_bRetreating	= LTFALSE;
	ClearPath();
	if (m_bZipping) StopZip();
	m_bZipLanding = LTFALSE;

	if (GetState() != PS_DEAD) return;

	if (m_fDeadTime < 0.0f)
	{
		m_fDeadTime = fTime;
		return;
	}

	// People take a moment before clicking respawn...
	if (fTime - m_fDeadTime < BOT_RESPAWN_DELAY + (1.0f - m_pP->fAggression) * 1.5f) return;

	// Same rules as MID_PLAYER_RESPAWN from a client...

	if (g_pGameServerShell->GetGameType() == COOPERATIVE_ASSAULT && g_NetUseSpawnLimit.GetFloat() > 0.0f)
	{
		IncRespawnCount();
		if (GetRespawnCount() > (int)g_NetSpawnLimit.GetFloat())
		{
			ChangeState(PS_GHOST);
			return;
		}
	}

	m_fDeadTime		= -1.0f;
	m_nShotsInClip	= 0;
	memset(m_anClipUsed, 0, sizeof(m_anClipUsed));	// fresh guns
	m_fLastHealth	= -1.0f;
	Respawn();
	{
		char szGun[96];
		DescribeLoadout(szGun, sizeof(szGun));
		LTVector vSpawn;
		g_pLTServer->GetObjectPos(m_hObject, &vSpawn);
		Act("Respawned at (%.0f %.0f %.0f). Carrying the %s.", vSpawn.x, vSpawn.y, vSpawn.z, szGun);
	}

	// Respawning resets the inventory; actually select a weapon again
	// (a human's client does that), or the one in our hands is invisible.
	m_bForceWeaponSelect = LTTRUE;
	m_fNextWeaponCheck	 = 0.0f;

	m_bHasScuba		  = LTFALSE;
	m_Route.fWaterMul = (m_fBaseWaterMul < 1.0f) ? 2.0f : m_fBaseWaterMul;	// No scuba after respawn: avoid deep water
	m_bSneakDone	  = LTFALSE;
	m_bGoalIsSneak	  = LTFALSE;
	m_fRallySince	  = 0.0f;
	StartLooting(fTime);

	LTRotation rRot;
	g_pLTServer->GetObjectRotation(m_hObject, &rRot);
	LTVector vR, vU, vF;
	g_pMathLT->GetRotationVectors(rRot, vR, vU, vF);
	m_fYaw = (LTFLOAT)atan2(vF.x, vF.z);
	m_vAimDir = vF;
}

// ----------------------------------------------------------------------- //
//	Perception
// ----------------------------------------------------------------------- //

LTVector CBotPlayer::GetEyePos()
{
	LTVector vPos;
	g_pLTServer->GetObjectPos(m_hObject, &vPos);
	vPos.y += 30.0f;
	return vPos;
}

LTVector CBotPlayer::GetAimPoint(CPlayerObj* pTarget)
{
	LTVector vPos;
	g_pLTServer->GetObjectPos(pTarget->m_hObject, &vPos);
	vPos.y += 15.0f;	// Chest
	return vPos;
}

CPlayerObj* CBotPlayer::GetEnemy()
{
	if (m_nEnemyID == NO_ENEMY) return LTNULL;

	CPlayerObj* pEnemy = g_BotMgr.FindPlayerByID(m_nEnemyID);
	if (!pEnemy || pEnemy->GetState() != PS_ALIVE)
	{
		m_nEnemyID = NO_ENEMY;
		return LTNULL;
	}

	return pEnemy;
}

void CBotPlayer::UpdateEnemy(LTFLOAT fTime)
{
	if (fTime < m_fNextEnemyCheck) return;
	m_fNextEnemyCheck = fTime + 0.15f;

	LTVector vEye = GetEyePos();
	LTVector vForward(LTFLOAT(sin(m_fYaw)), 0.0f, LTFLOAT(cos(m_fYaw)));

	// Better awareness = wider field of view.  Campers looking down a
	// corridor still see what's in front of them.
	LTFLOAT fFovDot = 0.35f - m_pP->fAwareness * Gene(eGeneFov) * 0.5f;	// ~-0.3 (aware) .. 0 (casual)
	if (fFovDot < -0.4f) fFovDot = -0.4f;	// still no eyes in the back of the head

	CPlayerObj* pBest = LTNULL;
	LTFLOAT fBestScore = 1.0e30f;

	const std::vector<CPlayerObj*>& players = g_BotMgr.GetPlayers();
	for (size_t i = 0; i < players.size(); i++)
	{
		CPlayerObj* pPlayer = players[i];
		if (!IsEnemyOf(pPlayer) || pPlayer->GetState() != PS_ALIVE) continue;

		LTVector vTarget = GetAimPoint(pPlayer);
		LTVector vTo = vTarget - vEye;
		LTFLOAT fDist = vTo.Mag();
		if (fDist > BOT_SIGHT_DIST * (0.6f + m_pP->fAwareness * 0.4f)) continue;

		if (fDist > BOT_AWARE_DIST * m_pP->fAwareness && Horiz(vTo).Dot(vForward) < fFovDot) continue;

		if (!CBotNav::IsVisible(vEye, vTarget)) continue;

		// Closest, but smart bots prefer the one that's hurt or already
		// fighting someone else...
		LTFLOAT fScore = fDist;
		if (pPlayer->GetPlayerID() == m_nEnemyID) fScore *= 0.6f;
		if (m_pP->bSmart)
		{
			CDestructible* pDamage = pPlayer->GetDestructible();
			if (pDamage->GetMaxHitPoints() > 0.0f)
			{
				fScore *= 0.5f + 0.5f * (pDamage->GetHitPoints() / pDamage->GetMaxHitPoints());
			}
		}

		if (fScore < fBestScore)
		{
			fBestScore = fScore;
			pBest = pPlayer;
		}
	}

	if (pBest)
	{
		LTVector vPos;
		g_pLTServer->GetObjectPos(pBest->m_hObject, &vPos);

		if ((pBest->GetPlayerID() != m_nLoggedEnemy || fTime - m_fLoggedEnemyTime > 15.0f) && (pBest->GetPlayerID() != m_nEnemyID || !m_bEnemyVisible))
		{
			LTVector vMe;
			g_pLTServer->GetObjectPos(m_hObject, &vMe);
			int nMates = CountTeammatesNear(this, vMe, 1200.0f);
			Act("Enemy spotted: %s with the %s, %.0fu away%s. %d teammate%s near me. I have the %s.",
				pBest->GetNetName(), CurrentWeaponName(pBest), vMe.Dist(vPos),
				vPos.y > vMe.y + 150.0f ? ", above me" : (vPos.y < vMe.y - 150.0f ? ", below me" : ""),
				nMates, nMates == 1 ? "" : "s", CurrentWeaponName(this));
			m_nLoggedEnemy = pBest->GetPlayerID();
			m_fLoggedEnemyTime = fTime;
		}
		if (pBest->GetPlayerID() != m_nEnemyID || !m_bEnemyVisible)
		{
			// Reaction time before we start shooting at somebody new...
			LTFLOAT fReact = RandomFloat(0.15f, 0.6f) * (1.2f - m_fSkill) / m_pP->fAwareness * Gene(eGeneReact);
			if (fReact < 0.12f) fReact = 0.12f;		// a fast human, never faster
			m_fEnemyAcquired = fTime + fReact;
			m_vEnemyVel.Init();
			m_fEnemyPrevTime = -1.0f;
		}

		// Track their velocity (players move on their client; the server
		// only sees positions)...
		if (pBest->GetPlayerID() == m_nEnemyID && m_fEnemyPrevTime > 0.0f && fTime > m_fEnemyPrevTime)
		{
			LTVector vVel = (vPos - m_vEnemyPrevPos) / (fTime - m_fEnemyPrevTime);
			if (vVel.Mag() < 1500.0f) m_vEnemyVel = m_vEnemyVel * 0.5f + vVel * 0.5f;
		}
		m_vEnemyPrevPos	 = vPos;
		m_fEnemyPrevTime = fTime;

		m_nEnemyID			= pBest->GetPlayerID();
		m_bEnemyVisible		= LTTRUE;
		m_fEnemyLastSeen	= fTime;
		m_vEnemyLastPos		= vPos;

		// Call it out to the team (like voice comms)
		if (g_pGameServerShell->GetGameType() == COOPERATIVE_ASSAULT && g_BotMgr.HasFeature(GetTeamID(), CBotMgr::eFeatCallouts))
		{
			g_BotMgr.ReportSighting(GetTeamID(), vPos);
		}

		// Enemy inside our own base: share it so defenders rotate to intercept,
		// and call it out in chat (throttled so bots don't spam).
		if (g_pGameServerShell->GetGameType() == COOPERATIVE_ASSAULT)
		{
			LTVector vBase;
			if (g_BotMgr.GetBasePos(GetTeamID(), vBase) && vPos.Dist(vBase) < 1600.0f)
			{
				g_BotMgr.SetTeamAlert(GetTeamID(), vPos);
				if (fTime > m_fNextCallout && Chance(m_pP->fTeamwork * 0.6f))
				{
					g_BotMgr.BotSay(this, Chance(0.5f) ? "Enemy in our base!" : "They're at our base, help!", LTTRUE);
					m_fNextCallout = fTime + 20.0f;
				}
			}
		}
	}
	else
	{
		m_bEnemyVisible = LTFALSE;

		// Forget about them after a while...
		if (fTime - m_fEnemyLastSeen > m_pP->fChaseTime * Gene(eGeneChase) + 2.0f)
		{
			m_nEnemyID = NO_ENEMY;
		}

		UpdateHearing(fTime);
	}
}

// Gunfire nearby: we know roughly where somebody is.
void CBotPlayer::UpdateHearing(LTFLOAT fTime)
{
	LTVector vPos;
	g_pLTServer->GetObjectPos(m_hObject, &vPos);

	LTFLOAT fRange = BOT_HEARING_DIST * m_pP->fAwareness;

	const std::vector<CPlayerObj*>& players = g_BotMgr.GetPlayers();
	for (size_t i = 0; i < players.size(); i++)
	{
		CPlayerObj* pPlayer = players[i];
		if (!IsEnemyOf(pPlayer) || pPlayer->GetState() != PS_ALIVE) continue;
		if (fTime - pPlayer->GetLastFireTime() > 0.3f) continue;

		LTVector vOther;
		g_pLTServer->GetObjectPos(pPlayer->m_hObject, &vOther);
		if (vPos.Dist(vOther) > fRange) continue;

		m_vHeardPos	 = vOther;
		m_fHeardTime = fTime;

		// Aggressive/aware bots treat it as a target to go after...
		if (m_nEnemyID == NO_ENEMY && Chance(0.3f + m_pP->fAggression * Gene(eGeneAggro) * 0.5f))
		{
			m_nEnemyID			= pPlayer->GetPlayerID();
			m_vEnemyLastPos		= vOther;
			m_fEnemyLastSeen	= fTime;
			m_fEnemyAcquired	= fTime + 0.3f;
		}
		return;
	}

	// Nothing heard ourselves, but a teammate nearby just called out an enemy:
	// face that way (and hunters may go and help), as if we'd heard it
	if (g_pGameServerShell->GetGameType() == COOPERATIVE_ASSAULT && g_BotMgr.HasFeature(GetTeamID(), CBotMgr::eFeatCallouts))
	{
		LTVector vCall;
		if (g_BotMgr.GetSighting(GetTeamID(), vPos, fRange * 1.2f, 2.0f, vCall))
		{
			m_vHeardPos	 = vCall;
			m_fHeardTime = fTime;
		}
	}
}

// ----------------------------------------------------------------------- //
//	Getting shot: turn around and fight back (or at least dodge)
// ----------------------------------------------------------------------- //

void CBotPlayer::UpdateHurt(LTFLOAT fTime)
{
	CDestructible* pDamage = GetDestructible();
	LTFLOAT fHealth = pDamage->GetHitPoints() + pDamage->GetArmorPoints();

	LTBOOL bHurt = (m_fLastHealth >= 0.0f && fHealth < m_fLastHealth - 0.5f);
	m_fLastHealth = fHealth;
	if (!bHurt) return;

	m_fHurtTime = fTime;

	// Dodge...
	m_fStrafeDir = (GetRandom(0, 1) == 0) ? -1.0f : 1.0f;
	m_fNextStrafeChange = fTime + RandomFloat(0.3f, 0.8f);

	if (m_bEnemyVisible) return;

	// Shot by somebody we can't see: a camper's spot is blown, move.
	if (m_bCamping)
	{
		m_bCamping = LTFALSE;
		m_bHasGoal = LTFALSE;
	}

	// Who did it?  Compare against live players only (the damager handle
	// may be stale).
	HOBJECT hDamager = pDamage->GetLastDamager();
	if (!hDamager || hDamager == m_hObject) return;

	const std::vector<CPlayerObj*>& players = g_BotMgr.GetPlayers();
	for (size_t i = 0; i < players.size(); i++)
	{
		CPlayerObj* pPlayer = players[i];
		if (pPlayer->m_hObject != hDamager) continue;
		if (!IsEnemyOf(pPlayer)) return;

		m_nEnemyID		 = pPlayer->GetPlayerID();
		m_fEnemyLastSeen = fTime;
		m_fEnemyAcquired = fTime + LTMAX(0.12f, RandomFloat(0.2f, 0.6f) * (1.2f - m_fSkill) * Gene(eGeneReact));
		g_pLTServer->GetObjectPos(pPlayer->m_hObject, &m_vEnemyLastPos);
		m_fNextEnemyCheck = 0.0f;	// Look for them right away
		m_bHasGoal = LTFALSE;
		return;
	}
}

// ----------------------------------------------------------------------- //
//	Weapons
// ----------------------------------------------------------------------- //

LTBOOL CBotPlayer::IsMelee(const WEAPON* pData)
{
	return pData && pData->nRange > 0 && pData->nRange <= 150;
}

// Roughly how far a weapon actually hits a running person.  Past this, a
// human doesn't empty the magazine (it just runs them dry and gives away
// their position); they close in, switch weapons or hold fire.
LTFLOAT CBotPlayer::GetEffectiveRange(const WEAPON* pData, const AMMO* pAmmo)
{
	if (!pData) return 1000.0f;
	if (IsMelee(pData)) return (LTFLOAT)pData->nRange;

	// From the weapon table's spread (perturb): Walther SMG 15-50 -> ~1000,
	// AK47 5-30 -> ~1500, Luger 5-25 -> ~1700, Revolver 10-60 -> ~850
	LTFLOAT fRange;
	if (pData->nRange > 20000)							fRange = 9000.0f;	// sniper rifle
	else if (pAmmo && pAmmo->nAreaDamageRadius > 0)		fRange = 2500.0f;	// explosives
	else if (pData->nVectorsPerRound > 1)				fRange = 700.0f;	// shotgun spread
	else fRange = LTCLAMP(60000.0f / (LTFLOAT)(pData->nMaxPerturb + 10), 700.0f, 4000.0f);

	if (pData->nRange > 0 && fRange > (LTFLOAT)pData->nRange) fRange = (LTFLOAT)pData->nRange;
	return fRange;
}

// Do we have anything better than our fists?
LTBOOL CBotPlayer::HasRangedWeapon()
{
	CPlayerAttachments* pAttach = GetPlayerAttachments();
	if (!pAttach || !g_pWeaponMgr) return LTFALSE;

	int nNumWeapons = g_pWeaponMgr->GetNumWeapons();
	for (int i = 0; i < nNumWeapons; i++)
	{
		CWeapon* pWeapon = pAttach->GetWeapon((uint8)i);
		if (!pWeapon || !pWeapon->Have()) continue;

		WEAPON* pData = g_pWeaponMgr->GetWeapon(i);
		if (!pData || pData->IsAGadget() || IsMelee(pData)) continue;

		AMMO* pAmmo = g_pWeaponMgr->GetAmmo(pData->nDefaultAmmoType);
		if (!pAmmo || pAmmo->eType == GADGET) continue;

		if (pData->bInfiniteAmmo || pAttach->GetAmmoCount(pAmmo->nId) > 0) return LTTRUE;
	}

	return LTFALSE;
}

// How far from the enemy we like to be with the current weapon
LTFLOAT CBotPlayer::GetPreferredRange()
{
	LTFLOAT fWant = 500.0f;

	CPlayerAttachments* pAttach = GetPlayerAttachments();
	CWeapon* pWeapon = pAttach ? pAttach->GetWeapon() : LTNULL;
	WEAPON* pData = pWeapon ? g_pWeaponMgr->GetWeapon(pWeapon->GetId()) : LTNULL;

	if (IsMelee(pData)) return 40.0f;	// Get in their face

	if (pWeapon)
	{
		AMMO* pAmmo = g_pWeaponMgr->GetAmmo(pWeapon->GetAmmoId());
		if (pAmmo && pAmmo->nAreaDamageRadius > 0)	fWant = 700.0f;
		else if (pData && pData->nRange > 20000)	fWant = 1200.0f;	// Sniper rifle
		else if (pData && pData->m_nFireDelay <= 100) fWant = 400.0f;	// SMG / assault rifle
	}

	return fWant * m_pP->fRangePref * Gene(eGeneRange);
}

// ----------------------------------------------------------------------- //
//	Weapon knowledge.  What every gun/ammo pair is worth comes from the
//	game's own numbers (damage, splash, burning, fire rate, clip, accuracy):
//	roughly how fast it kills someone at a given range.  On top of that the
//	"everybody knows" ranking of the MP arsenal: the Bacalov Collector's CHE
//	rounds, rockets and grenades are what you want, phosphorus rounds beat
//	plain ones, the spawn SMG is what you start with, not what you keep.
// ----------------------------------------------------------------------- //

static LTFLOAT NameTier(const char* szName, const char* const* aszKeys, const LTFLOAT* afMul, int nKeys)
{
	char szLow[64];
	SAFE_STRCPY(szLow, szName ? szName : "");
	_strlwr(szLow);
	for (int i = 0; i < nKeys; i++)
	{
		if (strstr(szLow, aszKeys[i])) return afMul[i];
	}
	return 1.0f;
}

// About how long the reload animation takes for a player (the client plays
// it; the server never sees it).  Single-shot guns reload after every shot.
LTFLOAT CBotPlayer::ReloadTime(const WEAPON* pW)
{
	if (!pW) return 2.0f;
	static const char* const s_aszKeys[] = { "rocket", "contender", "collector", "bacalov", "grenade launcher", "crossbow", "spear", "dragunov", "delisle", "ak47", "revolver" };
	static const LTFLOAT s_afTime[]		 = { 3.0f,     2.4f,        2.4f,        2.4f,      2.6f,               2.6f,       2.4f,    2.8f,       2.6f,     2.4f,   2.2f };
	LTFLOAT f = NameTier(pW->szName, s_aszKeys, s_afTime, sizeof(s_afTime) / sizeof(s_afTime[0]));
	if (f != 1.0f) return f;
	return (pW->nShotsPerClip <= 1) ? 2.4f : 1.9f;
}

LTFLOAT CBotPlayer::WeaponTier(const WEAPON* pW)
{
	static const char* const s_aszKeys[] =
	{
		"rocket", "contender", "collector", "bacalov", "grenade launcher", "dragunov", "delisle",
		"crossbow", "ak47", "ak-47", "spear", "walther", "smg", "fist", "punch",
	};
	static const LTFLOAT s_afMul[] =
	{
		1.8f,     1.7f,        1.7f,        1.7f,      1.6f,               1.35f,      1.2f,
		1.1f,       1.15f,  1.15f,   0.8f,    0.85f,     0.9f,  0.3f,   0.3f,
	};
	return NameTier(pW ? pW->szName : LTNULL, s_aszKeys, s_afMul, sizeof(s_afMul) / sizeof(s_afMul[0]));
}

LTFLOAT CBotPlayer::AmmoTier(const AMMO* pA)
{
	static const char* const s_aszKeys[] = { "phos", "explos", "che", "dum", "toxic", "poison", "armor", " ap" };
	static const LTFLOAT s_afMul[]		 = { 1.6f,   1.4f,     1.3f,  1.25f, 1.2f,    1.2f,     1.1f,    1.1f };
	return NameTier(pA ? pA->szName : LTNULL, s_aszKeys, s_afMul, sizeof(s_afMul) / sizeof(s_afMul[0]));
}

// How good this gun with this ammo is at fDist: ~ shots-to-kill and time
// between them, how likely we hit at that range.  0 = useless there.
LTFLOAT CBotPlayer::RateShot(const WEAPON* pW, const AMMO* pA, LTFLOAT fDist)
{
	if (!pW || !pA || ((WEAPON*)pW)->IsAGadget() || pA->eType == GADGET) return 0.0f;
	if (pW->nRange > 0 && fDist > (LTFLOAT)pW->nRange) return 0.0f;

	LTFLOAT fHit = (LTFLOAT)pA->nInstDamage * (LTFLOAT)LTMAX(1, pW->nVectorsPerRound) + (LTFLOAT)pA->nAreaDamage;
	if (pA->fProgDamage > 0.0f) fHit += pA->fProgDamage * LTMAX(1.0f, pA->fProgDamageDuration);	// burning, poison
	if (fHit <= 0.0f) return 0.0f;

	// Health plus some armor
	int nShots = (int)ceil(130.0f / fHit);
	if (nShots < 1) nShots = 1;
	if (nShots > 12) nShots = 12;

	LTBOOL bMelee = IsMelee(pW);
	LTFLOAT fDelay = (LTFLOAT)pW->m_nFireDelay / 1000.0f;
	LTFLOAT fInterval = (pW->m_nFireDelay <= 100) ? LTMAX(fDelay, 0.05f) * 1.6f : LTMAX(fDelay, 0.3f) + 0.1f;
	if (pW->nShotsPerClip == 1 && !bMelee) fInterval += ReloadTime(pW);		// reload after every shot
	LTFLOAT fTTK = (nShots - 1) * fInterval;
	if (pW->nShotsPerClip > 1 && nShots > pW->nShotsPerClip) fTTK += ReloadTime(pW);

	LTFLOAT fEff = GetEffectiveRange(pW, pA);
	LTFLOAT fAcc = (fDist <= fEff) ? 1.0f : (fEff / fDist) * (fEff / fDist);
	if (pA->nAreaDamageRadius > 0)
	{
		if (fDist < (LTFLOAT)pA->nAreaDamageRadius + 100.0f) fAcc *= 0.05f;	// we'd blow ourselves up
		else fAcc = LTMAX(fAcc, 0.5f);										// splash forgives a miss
	}
	if (bMelee) fAcc *= 0.3f;

	return fAcc / (fTTK + 0.3f) * WeaponTier(pW) * AmmoTier(pA);
}

// The best thing we carry (with ammo for it), rated at a typical range
LTFLOAT CBotPlayer::GetLoadoutPower(LTFLOAT fDist)
{
	CPlayerAttachments* pAttach = GetPlayerAttachments();
	if (!pAttach || !g_pWeaponMgr) return 0.0f;

	LTFLOAT fBest = 0.0f;
	for (int i = 0; i < g_pWeaponMgr->GetNumWeapons(); i++)
	{
		CWeapon* pWeapon = pAttach->GetWeapon((uint8)i);
		if (!pWeapon || !pWeapon->Have()) continue;
		WEAPON* pW = g_pWeaponMgr->GetWeapon(i);
		if (!pW || pW->IsAGadget()) continue;
		for (int a = 0; a < pW->nNumAmmoTypes; a++)
		{
			AMMO* pA = g_pWeaponMgr->GetAmmo(pW->aAmmoTypes[a]);
			if (!pA || (!pW->bInfiniteAmmo && pAttach->GetAmmoCount(pA->nId) <= 0)) continue;
			fBest = LTMAX(fBest, RateShot(pW, pA, fDist));
		}
	}
	return fBest;
}

// How much picking this up would improve what we carry (same units as
// RateShot; 0 = nothing for us)
LTFLOAT CBotPlayer::GetItemGain(const BotItem& item)
{
	CPlayerAttachments* pAttach = GetPlayerAttachments();
	if (!pAttach || !g_pWeaponMgr) return 0.0f;
	const LTFLOAT fRef = 800.0f;
	LTFLOAT fPower = GetLoadoutPower(fRef);

	if (item.eType == BotItem::eWeapon)
	{
		WEAPON* pW = g_pWeaponMgr->GetWeapon(item.nWeaponId);
		if (!pW || pW->IsAGadget()) return 0.0f;
		CWeapon* pWeapon = pAttach->GetWeapon((uint8)item.nWeaponId);
		LTBOOL bHave = pWeapon && pWeapon->Have();

		// Best ammo it comes with
		LTFLOAT fRate = 0.0f;
		const AMMO* pBestAmmo = LTNULL;
		for (int a = 0; a < item.nAmmoTypes; a++)
		{
			AMMO* pA = g_pWeaponMgr->GetAmmo(item.anAmmoId[a]);
			LTFLOAT f = RateShot(pW, pA, fRef);
			if (f > fRate) { fRate = f; pBestAmmo = pA; }
		}
		if (!pBestAmmo)
		{
			pBestAmmo = g_pWeaponMgr->GetAmmo(pW->nDefaultAmmoType);
			fRate = RateShot(pW, pBestAmmo, fRef);
		}

		if (!bHave) return LTMAX(0.0f, fRate - fPower * 0.8f) + fRate * 0.3f;

		// Already have it: worth it for the ammo
		int nMax = pBestAmmo ? LTMAX(1, ((AMMO*)pBestAmmo)->GetMaxAmount(m_hObject)) : 1;
		LTFLOAT fNeed = 1.0f - (LTFLOAT)(pBestAmmo ? pAttach->GetAmmoCount(pBestAmmo->nId) : nMax) / (LTFLOAT)nMax;
		return fRate * 0.4f * LTCLAMP(fNeed, 0.0f, 1.0f);
	}

	if (item.eType == BotItem::eAmmo)
	{
		LTFLOAT fGain = 0.0f;
		for (int a = 0; a < item.nAmmoTypes; a++)
		{
			AMMO* pA = g_pWeaponMgr->GetAmmo(item.anAmmoId[a]);
			if (!pA) continue;

			// For a gun we carry?
			LTFLOAT fRate = 0.0f;
			for (int i = 0; i < g_pWeaponMgr->GetNumWeapons(); i++)
			{
				CWeapon* pWeapon = pAttach->GetWeapon((uint8)i);
				if (!pWeapon || !pWeapon->Have()) continue;
				WEAPON* pW = g_pWeaponMgr->GetWeapon(i);
				if (!pW) continue;
				for (int k = 0; k < pW->nNumAmmoTypes; k++)
				{
					if (pW->aAmmoTypes[k] == pA->nId) fRate = LTMAX(fRate, RateShot(pW, pA, fRef));
				}
			}
			if (fRate <= 0.0f) continue;

			int nMax = LTMAX(1, pA->GetMaxAmount(m_hObject));
			LTFLOAT fNeed = 1.0f - (LTFLOAT)pAttach->GetAmmoCount(pA->nId) / (LTFLOAT)nMax;
			LTFLOAT f = fRate * 0.5f * LTCLAMP(fNeed, 0.0f, 1.0f);
			if (fRate > fPower) f += (fRate - fPower) * LTCLAMP(fNeed, 0.0f, 1.0f);	// better rounds than we fire now
			fGain += f;
		}
		return fGain;
	}

	return 0.0f;
}

// Fresh from spawning (or badly armed): gear up first.  Walk to the best
// weapons and ammo on our side of the map, then go do the job.
LTBOOL CBotPlayer::PickLootGoal(LTFLOAT fTime)
{
	LTVector vPos;
	g_pLTServer->GetObjectPos(m_hObject, &vPos);

	LTBOOL bTeams = (g_pGameServerShell->GetGameType() == COOPERATIVE_ASSAULT);
	LTVector vOurBase, vTheirBase;
	uint32 dwEnemy = (GetTeamID() == TEAM_1) ? TEAM_2 : TEAM_1;
	LTBOOL bSides = bTeams && g_BotMgr.GetBasePos(GetTeamID(), vOurBase) && g_BotMgr.GetBasePos(dwEnemy, vTheirBase);

	LTFLOAT fRunVel = GetMoveSpeed() > 1.0f ? GetMoveSpeed() : 400.0f;
	LTFLOAT fMaxDist = (m_nStyle == eBotScavenger) ? 5000.0f : (m_nStyle == eBotRusher ? 2200.0f : 3500.0f);

	const std::vector<BotItem>& items = g_BotMgr.GetItems();
	int nBest = -1;
	LTFLOAT fBestScore = 0.0f;
	std::vector< std::pair<LTFLOAT, std::pair<int, LTFLOAT> > > cands;	// (straight-line score, (item, gain))
	for (size_t i = 0; i < items.size(); i++)
	{
		const BotItem& item = items[i];
		if (item.eType != BotItem::eWeapon && item.eType != BotItem::eAmmo && !(item.eType == BotItem::eGear && item.bArmor)) continue;
		if (RecentlyVisited(item.vPos, fTime)) continue;
		if (g_BotMgr.IsItemClaimed(item.vPos, this)) continue;		// a teammate is already going for it

		LTFLOAT fDist = vPos.Dist(item.vPos);
		if (fDist > fMaxDist) continue;

		// There now, or back by the time we get there
		if (!item.bAvailable)
		{
			if (item.fRespawnAt <= 0.0f || item.fRespawnAt - fTime > fDist / fRunVel + 2.0f) continue;
		}

		// Our side of the map (not a loot run into their base)
		if (bSides && vOurBase.Dist(item.vPos) > vTheirBase.Dist(item.vPos) * 1.1f) continue;
		if (!g_BotMgr.IsItemReachable(item.vPos)) continue;
		if (!m_bHasScuba && g_BotMgr.IsItemUnderwater(item.vPos)) continue;	// no diving for it without SCUBA

		LTFLOAT fGain;
		if (item.eType == BotItem::eGear)
		{
			CDestructible* pDamage = GetDestructible();
			LTFLOAT fArmor = pDamage->GetMaxArmorPoints() > 0.0f ? pDamage->GetArmorPoints() / pDamage->GetMaxArmorPoints() : 1.0f;
			fGain = (1.0f - fArmor) * 1.5f;
			// Fresh off the respawn at full health: guns first, armor stays
			// for hurt teammates (it's the only healing)
			if (GetHealthFraction() >= 0.9f && ArmorHeals()) fGain = (1.0f - fArmor) * 0.3f;
		}
		else
		{
			fGain = GetItemGain(item);
		}
		if (fGain < 0.25f) continue;

		cands.push_back(std::make_pair(fGain / (fDist + 500.0f), std::make_pair((int)i, fGain)));
	}
	if (cands.empty()) return LTFALSE;

	// The few best by straight line, then by the real way there (a rocket
	// launcher across the hall may be a long walk round by the stairs)
	std::sort(cands.begin(), cands.end());
	std::reverse(cands.begin(), cands.end());
	CBotNav& nav = g_BotMgr.GetNav();
	CBotWorld& world = g_BotMgr.GetWorld();
	int nFrom = nav.FindNearest(vPos, 400.0f, LTFALSE);
	for (size_t c = 0; c < cands.size() && c < 5; c++)
	{
		const BotItem& item = items[cands[c].second.first];
		int nSpan = world.FindSpanNear(item.vPos + LTVector(0.0f, 40.0f, 0.0f), 4);
		int nTo = (nSpan >= 0) ? world.GetSpan(nSpan).nNode : BOTNAV_INVALID;
		LTFLOAT fCost = (nFrom >= 0 && nTo >= 0) ? nav.PathCost(nFrom, nTo, &m_Route) : -1.0f;
		if (fCost < 0.0f || fCost > fMaxDist * 1.5f) continue;

		LTFLOAT fScore = cands[c].second.second / (fCost + 500.0f);
		if (fScore > fBestScore) { fBestScore = fScore; nBest = cands[c].second.first; }
	}

	if (nBest < 0 || !PathTo(items[nBest].vPos)) return LTFALSE;
	SetGoal(items[nBest].vPos, LTTRUE, fTime + 25.0f);

	// What we're after (log), and tell the team about the big ones
	const BotItem& want = items[nBest];
	_snprintf(m_szLootWhy, sizeof(m_szLootWhy) - 1, "loot:%s", want.szName[0] ? want.szName : "item");
	m_szLootWhy[sizeof(m_szLootWhy) - 1] = '\0';
	for (char* p = m_szLootWhy; *p; p++) if (*p == ' ') *p = '_';
	if (want.eType == BotItem::eWeapon && want.szName[0] && Chance(0.25f))
	{
		WEAPON* pW = g_pWeaponMgr->GetWeapon(want.nWeaponId);
		if (pW && WeaponTier(pW) >= 1.5f)
		{
			char szSay[96];
			_snprintf(szSay, sizeof(szSay) - 1, "Getting the %s", want.szName);
			szSay[sizeof(szSay) - 1] = '\0';
			g_BotMgr.BotSay(this, szSay, LTTRUE);
		}
	}
	return LTTRUE;
}

const LTFLOAT CBotPlayer::kWellArmed = 2.2f;

void CBotPlayer::StartLooting(LTFLOAT fTime)
{
	// How long a personality spends gearing up before going to work
	LTFLOAT fFor;
	switch (m_nStyle)
	{
		case eBotScavenger:	fFor = 60.0f; break;
		case eBotRusher:	fFor = 20.0f; break;
		case eBotCamper:	fFor = 45.0f; break;
		default:			fFor = 35.0f; break;
	}
	m_fLootUntil = fTime + fFor;
	m_bHasGoal = LTFALSE;
}

void CBotPlayer::ChooseWeapon(LTFLOAT fTime, LTFLOAT fDist)
{
	CPlayerAttachments* pAttach = GetPlayerAttachments();
	if (!pAttach || !g_pWeaponMgr) return;

	CWeapon* pCur = pAttach->GetWeapon();
	if (pCur && !pCur->Have()) pCur = LTNULL;	// Lost it (respawn/reset)

	// Out of the rounds we're loaded with: pick again right away
	LTBOOL bEmpty = pCur && !g_pWeaponMgr->GetWeapon(pCur->GetId())->bInfiniteAmmo && pAttach->GetAmmoCount(pCur->GetAmmoId()) <= 0;
	if (fTime < m_fNextWeaponCheck && !bEmpty) return;
	m_fNextWeaponCheck = fTime + 1.0f;

	int nBest = -1, nBestAmmo = -1;
	LTFLOAT fBestScore = 0.0f;

	int nNumWeapons = g_pWeaponMgr->GetNumWeapons();
	for (int i = 0; i < nNumWeapons; i++)
	{
		CWeapon* pWeapon = pAttach->GetWeapon((uint8)i);
		if (!pWeapon || !pWeapon->Have()) continue;

		WEAPON* pData = g_pWeaponMgr->GetWeapon(i);
		if (!pData || pData->IsAGadget()) continue;

		for (int a = 0; a < pData->nNumAmmoTypes; a++)
		{
			AMMO* pAmmo = g_pWeaponMgr->GetAmmo(pData->aAmmoTypes[a]);
			if (!pAmmo || pAmmo->eType == GADGET) continue;
			if (!pData->bInfiniteAmmo && pAttach->GetAmmoCount(pAmmo->nId) <= 0) continue;

			LTFLOAT fDamage = (LTFLOAT)(pAmmo->nInstDamage + pAmmo->nAreaDamage);

			// Perfume gas (sleeping / stun / acid): no direct damage, short range,
			// but an enemy right in front of us goes down for the count -- gas
			// them, then finish them with a gun (they're asleep for that).
			if (fDamage <= 0.0f && pAmmo->fProgDamage <= 0.0f && pData->nRange > 0 && pData->nRange <= 400 && !IsMelee(pData))
			{
				CPlayerObj* pTarget = GetEnemy();
				LTBOOL bOut = pTarget && (g_pLTServer->GetObjectUserFlags(pTarget->m_hObject) & (USRFLG_CHAR_SLEEPING | USRFLG_CHAR_STUNNED));
				if (!pTarget || bOut || fDist > (LTFLOAT)pData->nRange * 0.85f) continue;
				if (100.0f > fBestScore) { fBestScore = 100.0f; nBest = i; nBestAmmo = pAmmo->nId; }
				continue;
			}

			LTFLOAT fScore = RateShot(pData, pAmmo, fDist);
			if (fScore <= 0.0f) continue;

			// Can it actually hit at this distance?
			if (fDist > GetEffectiveRange(pData, pAmmo) && g_BotMgr.HasFeature(GetTeamID(), CBotMgr::eFeatDiscipline)) fScore *= 0.3f;

			if (pData->nRange > 20000)	// Sniper rifle: for range, not for a brawl
			{
				if (fDist < 500.0f)			fScore *= 0.08f;
				else if (fDist < 900.0f)	fScore *= 0.3f;
				else if (fDist > 1500.0f)	fScore *= 1.2f * m_pP->fRangePref;
			}
			if (pData->m_nFireDelay <= 100 && m_nStyle == eBotRusher) fScore *= 1.3f;

			// Big guns are for when it counts: keep a few rockets for a fight
			if (pAmmo->nAreaDamageRadius > 0 && !GetEnemy()) fScore *= 0.8f;

			// Avoid flip-flopping
			if (pCur && pCur->GetId() == i && pCur->GetAmmoId() == pAmmo->nId) fScore *= 1.15f;

			if (fScore > fBestScore)
			{
				fBestScore = fScore;
				nBest = i;
				nBestAmmo = pAmmo->nId;
			}
		}
	}

	if (nBest < 0) return;

	if (!pCur || pCur->GetId() != nBest || m_bForceWeaponSelect)
	{
		m_bForceWeaponSelect = LTFALSE;
		DoWeaponChange((uint8)nBest);
		SetWeaponStatus(WS_SELECT);
		m_nBurstShots = 0;
		m_nBurstLen	  = 0;
		m_fReloadDone = fTime + 0.6f;	// drawing it (its clip is as we left it)
		WEAPON* pNew = g_pWeaponMgr->GetWeapon(nBest);
		AMMO* pNewAmmo = g_pWeaponMgr->GetAmmo(nBestAmmo);
		if (pNew && pCur) Act("Switching to the %s (%s) - %s.", pNew->szName, pNewAmmo ? pNewAmmo->szName : "?",
			GetEnemy() ? "best at this range" : "best for the next fight");
	}

	// Load the best rounds we have for it (phosphorus over plain, ...)
	CWeapon* pWeapon = pAttach->GetWeapon((uint8)nBest);
	if (pWeapon && nBestAmmo >= 0 && pWeapon->GetAmmoId() != nBestAmmo)
	{
		WEAPON* pW = g_pWeaponMgr->GetWeapon(nBest);
		pWeapon->SetAmmoId(nBestAmmo);
		pWeapon->ReloadClip(LTFALSE);
		if (nBest < kMaxClipWeapons) m_anClipUsed[nBest] = 0;
		m_fReloadDone = LTMAX(m_fReloadDone, fTime + ReloadTime(pW));	// loading other rounds is a reload
		SetWeaponStatus(WS_RELOADING);
		AMMO* pNewAmmo = g_pWeaponMgr->GetAmmo(nBestAmmo);
		if (pW && pNewAmmo) Act("Loading %s rounds into the %s.", pNewAmmo->szName, pW->szName);
	}
}

void CBotPlayer::UpdateAim(LTFLOAT fTime, LTFLOAT fDelta)
{
	CPlayerObj* pEnemy = GetEnemy();

	LTVector vPos;
	g_pLTServer->GetObjectPos(m_hObject, &vPos);

	LTVector vWant;
	if (pEnemy && m_bEnemyVisible)
	{
		LTVector vEye = GetEyePos();
		LTVector vTarget = GetAimPoint(pEnemy);

		CPlayerAttachments* pAttach = GetPlayerAttachments();
		CWeapon* pWeapon = pAttach ? pAttach->GetWeapon() : LTNULL;
		AMMO* pAmmo = pWeapon ? g_pWeaponMgr->GetAmmo(pWeapon->GetAmmoId()) : LTNULL;

		if (m_pP->bSmart && pAmmo)
		{
			// Lead moving targets with slow projectiles...
			if (pAmmo->eType == PROJECTILE && pAmmo->pProjectileFX && pAmmo->pProjectileFX->nVelocity > 0)
			{
				LTFLOAT fTravel = vEye.Dist(vTarget) / (LTFLOAT)pAmmo->pProjectileFX->nVelocity;
				if (fTravel > 1.5f) fTravel = 1.5f;
				vTarget += m_vEnemyVel * fTravel * m_fSkill * Gene(eGeneLead);
			}

			// ...splash damage at their feet, headshots with the rest.
			if (pAmmo->nAreaDamageRadius > 0)	vTarget.y -= 45.0f;
			else if (m_bAimHead)				vTarget.y += 18.0f;
		}

		// Aim error wanders; worse bots are sloppier, and nobody hits
		// much while running and jumping...

		if (fTime > m_fNextAimError)
		{
			LTVector vVel;
			g_pLTServer->GetVelocity(m_hObject, &vVel);
			LTFLOAT fMove = (vVel.Mag() > 50.0f) ? 1.3f * Gene(eGeneAimMove) : 0.7f;
			if (!IsOnGround()) fMove *= 1.5f;

			LTFLOAT fErr = (0.015f + (1.0f - m_fSkill) * 0.11f) * fMove * Gene(eGeneAimErr);
			if (fErr < 0.008f) fErr = 0.008f;		// human hands shake a little
			m_vAimError.Init(RandomFloat(-fErr, fErr), RandomFloat(-fErr, fErr) * 0.5f, RandomFloat(-fErr, fErr));
			m_fNextAimError = fTime + RandomFloat(0.2f, 0.5f);
			m_bAimHead = (m_fSkill > 0.5f) && Chance((m_fSkill - 0.5f) * 0.8f * Gene(eGeneHead));
		}

		vWant = vTarget - vEye;
		vWant.Norm();
		vWant += m_vAimError;
	}
	else
	{
		LTVector vVel;
		g_pLTServer->GetVelocity(m_hObject, &vVel);
		vVel.y = 0.0f;

		if (m_nEnemyID != NO_ENEMY)
		{
			vWant = m_vEnemyLastPos - vPos;		// Where we last saw them
		}
		else if (fTime - m_fHeardTime < 2.0f)
		{
			vWant = m_vHeardPos - vPos;			// Where we heard shots
		}
		else if (m_bCamping)
		{
			// Sweep slowly across the area we're watching
			vWant = RotateYaw(m_vCampLook, (LTFLOAT)sin(fTime * 0.6f) * 0.7f);
		}
		else if (vVel.MagSqr() > 100.0f)
		{
			vWant = vVel;
		}
		else
		{
			vWant = m_vAimDir;
		}
		vWant.y = 0.0f;

		// Glance around now and then, like people do...
		if (m_nEnemyID == NO_ENEMY && !m_bCamping)
		{
			if (fTime > m_fNextGlance)
			{
				m_fGlanceYaw  = Chance(0.25f + m_pP->fAwareness * 0.2f) ? RandomFloat(-1.2f, 1.2f) : 0.0f;
				m_fNextGlance = fTime + RandomFloat(0.6f, 2.5f);
			}

			if (m_fGlanceYaw != 0.0f) vWant = RotateYaw(vWant, m_fGlanceYaw);
		}
	}

	if (vWant.MagSqr() < 0.0001f) return;
	vWant.Norm();

	// Turn towards it at a limited rate...

	LTFLOAT fTurn = (3.0f + m_fSkill * 7.0f) * Gene(eGeneTurn) * fDelta;
	if (fTurn > 1.0f) fTurn = 1.0f;

	m_vAimDir = m_vAimDir + (vWant - m_vAimDir) * fTurn;
	if (m_vAimDir.MagSqr() < 0.0001f) m_vAimDir = vWant;
	m_vAimDir.Norm();

	m_fYaw = (LTFLOAT)atan2(m_vAimDir.x, m_vAimDir.z);

	LTRotation rRot;
	g_pMathLT->SetupEuler(rRot, 0.0f, m_fYaw, 0.0f);
	g_pLTServer->SetObjectRotation(m_hObject, &rRot);
}

void CBotPlayer::UpdateFire(LTFLOAT fTime)
{
	CPlayerObj* pEnemy = GetEnemy();
	if (!pEnemy || !m_bEnemyVisible) return;
	if (IsRidingVehicle() || m_bOnLadder) return;
	if (fTime < m_fEnemyAcquired || fTime < m_fReloadDone || fTime < m_fNextShot) return;

	CPlayerAttachments* pAttach = GetPlayerAttachments();
	if (!pAttach) return;

	CWeapon* pWeapon = pAttach->GetWeapon();
	if (!pWeapon) return;

	WEAPON* pData = g_pWeaponMgr->GetWeapon(pWeapon->GetId());
	AMMO* pAmmo = g_pWeaponMgr->GetAmmo(pWeapon->GetAmmoId());
	if (!pData || !pAmmo) return;

	// Only shoot when we're roughly facing them...

	LTVector vEye = GetEyePos();
	LTVector vTo = GetAimPoint(pEnemy) - vEye;
	LTFLOAT fDist = vTo.Mag();
	vTo.Norm();
	if (vTo.Dot(m_vAimDir) < 0.9f) return;

	if (pData->nRange > 0 && fDist > (LTFLOAT)pData->nRange) return;

	// Fire discipline: don't spray at what we can't hit (better shots reach a
	// bit further), and when the ammo is nearly gone only take close shots.
	LTFLOAT fEffective = GetEffectiveRange(pData, pAmmo) * (0.8f + 0.4f * m_fSkill) * Gene(eGeneEffRange);
	LTBOOL bDiscipline = g_BotMgr.HasFeature(GetTeamID(), CBotMgr::eFeatDiscipline);
	if (bDiscipline && fDist > fEffective) return;
	if (bDiscipline && !pData->bInfiniteAmmo && !IsMelee(pData))
	{
		int nAmmoLeft = pAttach->GetAmmoCount(pAmmo->nId);
		if (nAmmoLeft < pData->nShotsPerClip + pData->nShotsPerClip / 2 && fDist > fEffective * 0.6f) return;
	}

	// Never shoot through a teammate
	if (g_pGameServerShell->GetGameType() == COOPERATIVE_ASSAULT && IsTeammateInLineOfFire(vEye, GetAimPoint(pEnemy)))
	{
		return;
	}

	// Smart bots don't fire rockets into a wall in their face
	if (m_pP->bSmart && pAmmo->nAreaDamageRadius > 0 && fDist < (LTFLOAT)pAmmo->nAreaDamageRadius) return;

	WFireInfo fireInfo;
	fireInfo.hFiredFrom	= m_hObject;
	fireInfo.vPath		= m_vAimDir;
	fireInfo.vFirePos	= HandHeldWeaponFirePos(pWeapon);
	fireInfo.vFlashPos	= fireInfo.vFirePos;
	fireInfo.bAltFire	= LTFALSE;
	fireInfo.fPerturbR	= 1.0f - m_fSkill * 0.6f;
	fireInfo.fPerturbU	= fireInfo.fPerturbR;

	// The fire position is in front of us, make sure it isn't through a wall...
	if (!CBotNav::IsVisible(vEye, fireInfo.vFirePos))
	{
		fireInfo.vFirePos  = vEye;
		fireInfo.vFlashPos = vEye;
	}

	uint32 nLastShot = pWeapon->GetLastFireTimestamp();
	pWeapon->UpdateWeapon(fireInfo, LTTRUE);
	if (pWeapon->GetLastFireTimestamp() == nLastShot) return;	// Too soon, didn't fire

	m_fLastFire = fTime;
	NoteFired();
	SetWeaponStatus(WS_NONE);

	// Time the next shot like a person would.  The weapon's FireDelay is only
	// an anti-cheat minimum; real players are limited by the (client side)
	// fire animations, and pull the trigger for each shot of a semi-auto.

	LTFLOAT fDelay	= (LTFLOAT)pData->m_nFireDelay / 1000.0f;
	LTBOOL bMelee	= IsMelee(pData);
	LTBOOL bAuto	= (pData->m_nFireDelay <= 100);

	if (bAuto)
	{
		// Bursts, using the same numbers the single player AI uses...
		if (m_nBurstLen <= 0)
		{
			m_nBurstLen = (int)((GetRandom(pData->nAIMinBurstShots, pData->nAIMaxBurstShots) + (int)(m_fSkill * 4.0f)) * Gene(eGeneBurst));
			if (m_nBurstLen < 1) m_nBurstLen = 1;
		}

		m_fNextShot = fTime + fDelay;
		if (++m_nBurstShots >= m_nBurstLen)
		{
			m_nBurstShots	= 0;
			m_nBurstLen		= 0;
			m_fNextShot		= fTime + RandomFloat(pData->fAIMinBurstInterval, pData->fAIMaxBurstInterval) * (1.3f - m_fSkill * 0.6f);
		}
	}
	else if (bMelee)
	{
		m_fNextShot = fTime + fDelay + RandomFloat(0.0f, 0.2f);
	}
	else
	{
		LTFLOAT fMin = (fDelay > 0.3f) ? fDelay : 0.3f;
		m_fNextShot = fTime + fMin + RandomFloat(0.05f, 0.35f) * (1.2f - m_fSkill);
	}

	// Projectile weapons: clients play the fire sound from this message (vector
	// weapons do it in the weapon fx message)...

	if (pAmmo->eType == PROJECTILE)
	{
		HMESSAGEWRITE hMessage = g_pLTServer->StartInstantSpecialEffectMessage(&fireInfo.vFirePos);
		g_pLTServer->WriteToMessageByte(hMessage, SFX_PLAYERSOUND_ID);
		g_pLTServer->WriteToMessageByte(hMessage, PSI_FIRE);
		g_pLTServer->WriteToMessageByte(hMessage, (uint8)pWeapon->GetId());
		g_pLTServer->WriteToMessageByte(hMessage, (uint8)GetPlayerID());
		g_pLTServer->WriteToMessageCompPosition(hMessage, &fireInfo.vFirePos);
		g_pLTServer->EndMessage2(hMessage, MESSAGE_NAGGLEFAST);
	}

	// Reload like a real player has to (the server refills a player's clip
	// at once; the reload animation is the client's, so we wait it out)

	int nW = pWeapon->GetId();
	if (!bMelee && !pData->bInfiniteAmmo && pData->nShotsPerClip >= 1 && nW < kMaxClipWeapons &&
		++m_anClipUsed[nW] >= pData->nShotsPerClip)
	{
		m_anClipUsed[nW] = 0;
		m_nBurstShots	= 0;
		m_nBurstLen		= 0;
		m_fReloadDone	= fTime + ReloadTime(pData) * RandomFloat(0.95f, 1.15f);
		SetWeaponStatus(WS_RELOADING);
	}
}

// ----------------------------------------------------------------------- //
//	Goals
// ----------------------------------------------------------------------- //

void CBotPlayer::SetGoal(const LTVector& vGoal, LTBOOL bItem, LTFLOAT fTimeout)
{
	m_vGoal			= vGoal;
	m_bGoalIsItem	= bItem;
	m_bGoalIsCamp	= LTFALSE;
	m_bHasGoal		= LTTRUE;
	m_fGoalTimeout	= fTimeout;
	m_bCamping		= LTFALSE;
	m_bGoalIsVehicle = LTFALSE;
	m_bGoalIsSneak	 = LTFALSE;
	m_bGoalIsObjective = LTFALSE;
	m_bGoalIsFrontier  = LTFALSE;
	m_bExploring	   = LTFALSE;
	m_bGoalIsStage	   = LTFALSE;
	m_bGoalIsScuba	   = LTFALSE;
	m_bWaitingWave	   = LTFALSE;
	m_fBestTargetDist = 1.0e30f;
	m_nProgressSpan   = 0;
	m_fProgressCheck  = g_pLTServer->GetTime() + 3.0f;
	m_nReplans		  = 0;
	m_fGoalSetTime	  = g_pLTServer->GetTime();
	if (bItem) g_BotMgr.ClaimItem(vGoal, this, fTimeout);	// teammates pick something else
}

void CBotPlayer::UpdateGoal(LTFLOAT fTime)
{
	LTVector vPos;
	g_pLTServer->GetObjectPos(m_hObject, &vPos);

	LTBOOL bArmed = HasRangedWeapon();
	LTFLOAT fHealth = GetHealthFraction();

	CDestructible* pDamage = GetDestructible();
	LTFLOAT fArmor = pDamage->GetMaxArmorPoints() > 0.0f ? pDamage->GetArmorPoints() / pDamage->GetMaxArmorPoints() : 1.0f;

	// Retreating: go for health right now.

	if (m_bRetreating && !(m_bHasGoal && m_bGoalIsItem))
	{
		m_bCamping = LTFALSE;
		if (PickItemGoal(fTime, fHealth, fArmor)) { m_szWhy = "retreat"; return; }
	}

	// Badly hurt with nobody in sight: still go heal first (armor is the only
	// health in multiplayer).  Without this a hurt bot drifted back into its
	// team role, buddy runs and intercepts the moment the enemy was out of
	// sight, and could limp around at 20 hp for minutes.
	LTBOOL bNeedHeal = NeedsHealing(fHealth);
	LTBOOL bEscapingWater = m_bHasGoal && !strcmp(m_szWhy, "out-of-water");	// (stuck under water: get out first)
	if (bNeedHeal && !m_bRetreating && !IsHealGoal() && !bEscapingWater && fTime > m_fNextHealTry)
	{
		m_fNextHealTry = fTime + 3.0f;	// (none to be had: don't retry every frame)
		if (PickItemGoal(fTime, fHealth, fArmor))
		{
			m_bCamping = LTFALSE;
			m_szWhy = "item-hurt";
			return;
		}
	}

	// Chasing somebody we lost sight of?  (Not with bare fists, not when
	// we're camping, and only for as long as our patience lasts.)

	// Defenders respond to a teammate's "enemy in our base!" callout, breaking
	// camp if they have to, and go intercept the intruder.
	if (g_pGameServerShell->GetGameType() == COOPERATIVE_ASSAULT && m_bDefender && !m_bEnemyVisible && !m_bRetreating && !IsHealGoal())
	{
		LTVector vAlert;
		LTBOOL bOnIt = m_bHasGoal && !strcmp(m_szWhy, "intercept") && (fTime - m_fGoalSetTime < 3.0f || m_vGoal.Dist(vAlert) < 600.0f);
		// Already chasing an enemy we saw: that IS dealing with the intruder.
		// (Intercept and chase used to replace each other every frame: the
		// bot stood still, flipping between the two, while being shot.)
		LTBOOL bChasing = m_bHasGoal && !strcmp(m_szWhy, "chase");
		if (g_BotMgr.GetTeamAlert(GetTeamID(), vAlert, 10.0f) && vPos.Dist(vAlert) > 600.0f && !bOnIt && !bChasing &&
			!(m_bHasGoal && !m_bGoalIsItem && m_vGoal.Dist(vAlert) < 300.0f) && PathTo(vAlert))
		{
			m_bCamping = LTFALSE;
			SetGoal(vAlert, LTFALSE, fTime + 8.0f);
			m_szWhy = "intercept";
			return;
		}
	}

	// Team defenders don't wander off what they guard
	if (g_pGameServerShell->GetGameType() == COOPERATIVE_ASSAULT && m_bDefender && !m_bEnemyVisible && !m_bRetreating && !m_bCamping && !IsHealGoal())
	{
		LTVector vGuard;
		LTVector vAlertNow;
		LTBOOL bIntercepting = (m_bHasGoal && !strcmp(m_szWhy, "intercept")) || g_BotMgr.GetTeamAlert(GetTeamID(), vAlertNow, 10.0f);
		if (g_BotMgr.GetGuardTarget(GetTeamID(), vGuard) && vPos.Dist(vGuard) > 1800.0f && !bIntercepting &&
			fTime - m_fGoalSetTime > 4.0f && !(m_bHasGoal && m_vGoal.Dist(vGuard) < 800.0f))
		{
			if (PickCampGoal(fTime)) { m_szWhy = "leash"; return; }
			if (PathTo(vGuard)) { SetGoal(vGuard, LTFALSE, fTime + 30.0f); m_szWhy = "leash"; return; }
		}
	}

	if (!m_bRetreating && !bNeedHeal && bArmed && !m_bCamping && m_nEnemyID != NO_ENEMY && !m_bEnemyVisible &&
		!IsObjectiveRun() && fTime - m_fEnemyLastSeen < m_pP->fChaseTime * Gene(eGeneChase) &&
		vPos.Dist(m_vEnemyLastPos) < 2500.0f)	// (not across the map after someone seen far away)
	{
		// Smart bots go where they're probably heading, not where they were
		LTVector vChase = m_vEnemyLastPos;
		if (m_pP->bSmart) vChase += m_vEnemyVel * 0.7f;

		if (!m_bHasGoal || m_bGoalIsItem || m_vGoal.Dist(vChase) > 150.0f)
		{
			if (PathTo(vChase))
			{
				SetGoal(vChase, LTFALSE, fTime + 6.0f);
				m_szWhy = "chase";
			}
		}
		return;
	}

	// Camping: hold the spot until bored, out of ammo, or hurt.

	if (m_bCamping)
	{
		// Gunfire somewhere else: go have a look (unless it's a hardcore camper,
		// or we're holding the line for our wave)
		// (High ground is held through it: that's what it's for.)
		if (fTime - m_fHeardTime < 1.0f && m_nStyle != eBotCamper && !m_bWaitingWave && !m_bPerched) m_fCampUntil = 0.0f;

		if (fTime < m_fCampUntil && bArmed && fHealth > m_pP->fRetreatHealth * Gene(eGeneRetreat)) return;
		m_bCamping	   = LTFALSE;
		m_bPerched	   = LTFALSE;
		m_bWaitingWave = LTFALSE;
		m_bHasGoal	   = LTFALSE;
	}

	// Current goal still good?

	if (m_bHasGoal && fTime < m_fGoalTimeout)
	{
		LTFLOAT fReach = m_bGoalIsItem ? BOT_ITEM_REACH_DIST : BOT_ARRIVE_DIST*2.0f;
		LTBOOL bReached = HorizDist(vPos, m_vGoal) < fReach && (LTFLOAT)fabs(vPos.y - m_vGoal.y) < 80.0f;

		// An item that's about to respawn is worth waiting for...
		LTBOOL bGone = LTFALSE;
		if (m_bGoalIsItem)
		{
			LTFLOAT fRespawnIn = g_BotMgr.GetItemRespawnIn(m_vGoal);
			bGone = (fRespawnIn < 0.0f) || (fRespawnIn > 5.0f && HorizDist(vPos, m_vGoal) < 200.0f);
		}

		if (bReached && m_bGoalIsCamp)
		{
			int nHere = g_BotMgr.GetNav().FindNearest(vPos, 200.0f, LTFALSE);
			m_bPerched	 = (nHere >= 0 && (g_BotMgr.GetNav().GetNode(nHere).nFlags & BOTNODE_HIGH)) || m_szWhy == s_szOverwatch;
			m_bCamping	 = LTTRUE;
			m_fCampUntil = fTime + RandomFloat(8.0f, 20.0f) * (m_nStyle == eBotCamper ? 1.5f : 1.0f);
			if (m_bPerched) m_fCampUntil = fTime + RandomFloat(25.0f, 50.0f) * (m_nStyle == eBotCamper ? 1.4f : 1.0f);	// high ground is worth holding
			m_bHasGoal	 = LTFALSE;
			return;
		}

		if (!bReached && !bGone) return;

		// Got somewhere high: maybe hold it
		if (bReached && !m_bGoalIsItem && !m_bGoalIsCamp) OnReachedHighGround(fTime);
		if (m_bCamping) return;

		if (bReached && m_bGoalIsSneak)
		{
			m_bSneakDone = LTTRUE;
			m_bGoalIsSneak = LTFALSE;
		}

		if (bReached && m_bGoalIsItem && m_bGoalIsScuba)
		{
			// Now the water is our friend
			m_bHasScuba = LTTRUE;
			m_Route.fWaterMul = m_fBaseWaterMul * 0.4f;
			m_bGoalIsScuba = LTFALSE;
		}

		if (bReached && m_bGoalIsItem)
		{
			m_avVisited[m_nNextVisited]		= m_vGoal;
			m_afVisitedTime[m_nNextVisited] = fTime;
			m_nNextVisited = (m_nNextVisited + 1) % kVisitedItems;
		}
	}

	// Pick something new...

	m_bHasGoal = LTFALSE;
	ClearPath();

	LTBOOL bTeams = (g_pGameServerShell->GetGameType() == COOPERATIVE_ASSAULT);

	// Gear up first (just spawned with the default gun): the good weapons
	// and ammo on our side of the map, until we're well armed or it's time
	// to go.  Not while the team needs us back home.
	if (m_fLootUntil > 0.0f)
	{
		LTVector vAlert;
		LTBOOL bNeeded = bTeams && m_bDefender && g_BotMgr.GetTeamAlert(GetTeamID(), vAlert, 10.0f);
		if (fTime > m_fLootUntil || bNeeded || GetLoadoutPower(800.0f) >= kWellArmed)
		{
			m_fLootUntil = 0.0f;
		}
		else if (PickLootGoal(fTime))
		{
			m_szWhy = m_szLootWhy;
			return;
		}
		else
		{
			m_fLootUntil = 0.0f;	// nothing worth it nearby
		}
	}

	// Campers (and now and then others) find a spot to hold, if they're armed.
	// In team games only defenders camp much.
	LTFLOAT fCamp = m_pP->fCamp;
	if (bTeams) fCamp *= m_bDefender ? 0.4f : 0.1f;	// Defenders mostly patrol
	if (bArmed && fHealth > 0.6f && Chance(fCamp) && PickCampGoal(fTime)) { m_szWhy = "camp"; return; }

	// Long trip and a motorcycle close by?  Ride.
	if (fTime > m_fNextVehicleTry && PickVehicleGoal(fTime)) { m_szWhy = "vehicle"; return; }

	if (bTeams)
	{
		// Group up with a nearby teammate now and then (loose escorting)...
		if (Chance(0.08f + m_pP->fTeamwork * 0.2f) && PickBuddyGoal(fTime)) { m_szWhy = "buddy"; return; }

		// ...but the team objective is the point of an assault map, so commit
		// to it on most picks: attackers push the documents, defenders hold
		// them.  Item/weapon runs below are the fallback when the role goal
		// can't be reached (no path, etc.) or when we actually need gear.
		LTFLOAT fRoleChance = m_bDefender ? 0.7f : Gene(eGeneRole);
		if (bArmed && fHealth > 0.35f && Chance(fRoleChance) && PickTeamRoleGoal(fTime)) { m_szWhy = "role"; return; }
	}

	if (PickItemGoal(fTime, fHealth, fArmor))
	{
		m_szWhy = !bArmed ? "item-noammo" : (fHealth <= 0.35f ? "item-hurt" : (bTeams ? "item-rolefail" : "item"));
		return;
	}

	// Hunters go where the shooting is...
	if (fTime - m_fHeardTime < 8.0f && Chance(m_pP->fAggression * Gene(eGeneAggro)) && PathTo(m_vHeardPos))
	{
		SetGoal(m_vHeardPos, LTFALSE, fTime + 10.0f);
		m_szWhy = "heard";
		return;
	}

	CBotNav& nav = g_BotMgr.GetNav();
	for (int nTry = 0; nTry < 5; nTry++)
	{
		int nNode = nav.GetRandomNode();
		if (nNode == BOTNAV_INVALID) break;

		if (PathTo(nav.GetNode(nNode).vPos))
		{
			SetGoal(nav.GetNode(nNode).vPos, LTFALSE, fTime + 20.0f);
			m_szWhy = "wander";
			return;
		}
	}

	// No usable graph: just wander.
	LTFLOAT fAngle = RandomFloat(0.0f, MATH_CIRCLE);
	SetGoal(vPos + LTVector(LTFLOAT(sin(fAngle)), 0.0f, LTFLOAT(cos(fAngle))) * 500.0f, LTFALSE, fTime + 3.0f);
}

LTBOOL CBotPlayer::PickItemGoal(LTFLOAT fTime, LTFLOAT fHealth, LTFLOAT fArmor)
{
	LTVector vPos;
	g_pLTServer->GetObjectPos(m_hObject, &vPos);

	CPlayerAttachments* pAttach = GetPlayerAttachments();
	LTBOOL bArmed = HasRangedWeapon();
	LTBOOL bTeams = (g_pGameServerShell->GetGameType() == COOPERATIVE_ASSAULT);
	LTFLOAT fRunVel = GetMoveSpeed() > 1.0f ? GetMoveSpeed() : 400.0f;

	LTBOOL bDiver = (m_fBaseWaterMul < 1.0f);

	// Badly hurt (enemy in sight or not): only armor counts, nearby, on our side
	LTBOOL bHeal = m_bRetreating || NeedsHealing(fHealth);

	const std::vector<BotItem>& items = g_BotMgr.GetItems();
	int nBest = -1;
	LTFLOAT fBestScore = 0.0f;

	for (size_t i = 0; i < items.size(); i++)
	{
		const BotItem& item = items[i];
		if (RecentlyVisited(item.vPos, fTime)) continue;
		if (!g_BotMgr.IsItemReachable(item.vPos)) continue;	// on a ledge nobody gets to
		if (item.eType != BotItem::eIntel && g_BotMgr.IsItemClaimed(item.vPos, this)) continue;	// a teammate is on it
		if (item.eType == BotItem::eGear && item.bScuba && m_bHasScuba) continue;

		LTFLOAT fDist = vPos.Dist(item.vPos);

		// Smart bots also go for items that will be back by the time they
		// get there (they remember when they were taken)...
		LTFLOAT fTiming = 1.0f;
		if (!item.bAvailable)
		{
			if (!m_pP->bSmart || item.fRespawnAt <= 0.0f) continue;
			LTFLOAT fRespawnIn = item.fRespawnAt - fTime;
			if (fRespawnIn > fDist / fRunVel + 3.0f) continue;
			fTiming = 0.8f;
		}

		LTFLOAT fValue = 0.0f;
		switch (item.eType)
		{
			case BotItem::eWeapon:
			{
				// What it adds to what we carry (a rocket launcher when all we
				// have is the spawn SMG is worth a long walk)
				fValue = 30.0f * GetItemGain(item);
				if (!bArmed) fValue = LTMAX(fValue, 40.0f);

				// Campers want something that reaches (sniper rifle) or splashes
				WEAPON* pWData = g_pWeaponMgr->GetWeapon(item.nWeaponId);
				CWeapon* pWeapon = pAttach ? pAttach->GetWeapon((uint8)item.nWeaponId) : LTNULL;
				if (pWData && pWData->nRange > 20000 && !(pWeapon && pWeapon->Have()) && (m_nStyle == eBotCamper || m_bDefender)) fValue *= 1.8f;
			}
			break;

			case BotItem::eAmmo:
				fValue = 30.0f * GetItemGain(item);
				if (!bArmed) fValue = LTMAX(fValue, 25.0f);
			break;

			case BotItem::eGear:
				if (item.bScuba)
				{
					fValue = bDiver ? 70.0f : 15.0f;
				}
				else if (item.bArmor)
				{
					// Armor is the only thing that heals in multiplayer
					// (CDestructible::AddGear: armor + NetArmorHealthPercent)
					fValue = (10.0f + (1.0f - fHealth) * 80.0f + (1.0f - fArmor) * 40.0f) * Gene(eGeneHeal);
					// Healthy (just respawned): armor is a nice-to-have.  Leave
					// it for hurt teammates - it's the only healing there is, and
					// fresh spawns standing next to it took it all (37 grabs by
					// full-health spawns against 90 health runs in one game).
					if (fHealth >= 0.9f && ArmorHeals()) fValue = (8.0f + (1.0f - fArmor) * 10.0f) * Gene(eGeneHeal);
					if (!ArmorHeals()) fValue = (10.0f + (1.0f - fArmor) * 40.0f) * Gene(eGeneHeal);	// protection only
					if (bHeal) fValue *= 5.0f;
				}
				else
				{
					// Fire extinguisher, gas mask...: protection only, no health
					if (bHeal) continue;
					fValue = 5.0f;
				}
			break;

			case BotItem::eIntel:
				// Team objective: only ours (or anybody's) counts
				if (!bTeams || (item.nTeam && item.nTeam != GetTeamID())) continue;
				fValue = 25.0f + 50.0f * m_pP->fTeamwork;
				fDist *= 0.4f;	// Worth a trip across the map
			break;

			default:
				fValue = 5.0f;
			break;
		}

		if (bHeal && !(item.eType == BotItem::eGear && item.bArmor)) continue;

		// Patching up: not by running into their base or across the map
		if (bHeal)
		{
			if (fDist > 2500.0f) continue;
			LTVector vMine, vTheirs;
			if (bTeams && g_BotMgr.GetBasePos(GetTeamID(), vMine) &&
				g_BotMgr.GetBasePos(GetTeamID() == TEAM_1 ? TEAM_2 : TEAM_1, vTheirs) &&
				item.vPos.DistSqr(vTheirs) < item.vPos.DistSqr(vMine) &&
				item.vPos.DistSqr(vTheirs) < vPos.DistSqr(vTheirs)) continue;	// deeper into their half
		}

		// Deep under water or in a flooded tunnel: only with SCUBA (except the
		// SCUBA itself, which divers fetch on purpose)
		if (!m_bHasScuba && !(item.eType == BotItem::eGear && item.bScuba) && g_BotMgr.IsItemUnderwater(item.vPos)) continue;

		// On the way to the objective: only grab what's right there, or what we really need
		if (IsObjectiveRun() && item.eType != BotItem::eIntel && fDist > 500.0f && fHealth > 0.4f && bArmed) continue;

		fValue *= m_pP->fItemGreed * Gene(eGeneGreed) * fTiming;
		LTFLOAT fScore = fValue / (fDist + 300.0f) * RandomFloat(0.7f, 1.3f);

		if (fScore > fBestScore)
		{
			fBestScore = fScore;
			nBest = (int)i;
		}
	}

	if (nBest >= 0 && PathTo(items[nBest].vPos))
	{
		SetGoal(items[nBest].vPos, LTTRUE, fTime + 20.0f);
		m_bGoalIsScuba = (items[nBest].eType == BotItem::eGear && items[nBest].bScuba);
		return LTTRUE;
	}

	return LTFALSE;
}

LTBOOL CBotPlayer::PickTeamRoleGoal(LTFLOAT fTime)
{
	LTVector vPos;
	g_pLTServer->GetObjectPos(m_hObject, &vPos);

	uint32 dwMyTeam	   = GetTeamID();
	uint32 dwEnemyTeam = (dwMyTeam == TEAM_1) ? TEAM_2 : TEAM_1;

	LTVector vTarget;
	if (m_bDefender)
	{
		// Hold a good spot near what they're after, or at least patrol it
		if (Chance(0.5f) && PickCampGoal(fTime)) return LTTRUE;
		if (!g_BotMgr.GetGuardTarget(dwMyTeam, vTarget)) return LTFALSE;
	}
	else if (m_fBaseWaterMul < 1.0f && !m_bSneakDone && m_bHasScuba)
	{
		// Have SCUBA: sneak in through the water (the safe deep route).
		LTVector vSneak;
		if (g_BotMgr.GetSneakTarget(dwMyTeam, vSneak) && PathTo(vSneak))
		{
			SetGoal(vSneak, LTFALSE, fTime + 120.0f);	// Long underwater swim; don't give up early
			m_bGoalIsSneak = LTTRUE;
			m_bGoalIsObjective = LTTRUE;
			return LTTRUE;
		}
		if (!g_BotMgr.GetPushTarget(dwMyTeam, vTarget)) return LTFALSE;
	}
	else if (m_fBaseWaterMul < 1.0f && !m_bSneakDone)
	{
		// Diver without SCUBA: grab a set if one's available, otherwise just
		// attack overland with the team instead of loitering or drowning.
		if (PickScubaGoal(fTime)) return LTTRUE;
		if (!g_BotMgr.GetPushTarget(dwMyTeam, vTarget)) return LTFALSE;
	}
	else
	{
		// The team's current push (documents or their spawn).  Everybody
		// goes together, each by their own favourite route.
		if (!g_BotMgr.GetPushTarget(dwMyTeam, vTarget)) return LTFALSE;
	}
	(void)dwEnemyTeam;
	if (!m_bDefender) m_bLaneNext = LTTRUE;

	// Long guns and campers cover the push from high ground
	if (!m_bDefender && !m_bGoalIsSneak && PickOverwatchGoal(fTime)) return LTTRUE;

	// Wave attack: while the team is gathering, go to the staging point (out of
	// sight of the objective) and hold there; everybody goes in together when
	// the wave pushes.  Whoever is already past the stage just keeps going.
	// It's a rally point, not a team-wide freeze: whoever gets there waits for
	// a buddy (a few seconds, never long), then they go in together.  Once
	// the team is pushing, latecomers skip it and head straight in.
	LTVector vStage;
	if (!m_bDefender && IsWaveMember() && g_BotMgr.HasFeature(dwMyTeam, CBotMgr::eFeatWaves) &&
		g_BotMgr.GetWaveState(dwMyTeam) == CBotMgr::eWaveGather &&
		g_BotMgr.GetStage(dwMyTeam, vStage) && HorizDist(vPos, vTarget) > HorizDist(vStage, vTarget) - 200.0f &&
		!(m_fRallySince > 0.0f && fTime - m_fRallySince > 20.0f))
	{
		if (HorizDist(vPos, vStage) > 450.0f)
		{
			m_bLaneNext = LTTRUE;
			if (PathTo(vStage))
			{
				m_fRallySince = 0.0f;
				SetGoal(vStage, LTFALSE, fTime + 45.0f);
				m_bGoalIsObjective = LTTRUE;
				m_bGoalIsStage	   = LTTRUE;
				return LTTRUE;
			}
		}
		else
		{
			if (m_fRallySince <= 0.0f) m_fRallySince = fTime;
			LTFLOAT fWaited = fTime - m_fRallySince;
			LTBOOL bBuddy = (g_BotMgr.GetWaveAtStage(dwMyTeam) >= 2);
			if (!(bBuddy && fWaited > 3.0f) && fWaited < 12.0f)
			{
				// Hold, watching toward the objective, re-checking every couple of seconds
				m_bHasGoal	   = LTFALSE;
				ClearPath();
				m_bCamping	   = LTTRUE;
				m_bWaitingWave = LTTRUE;
				m_vCampLook	   = Horiz(vTarget - vPos);
				m_fCampUntil   = fTime + 2.0f;
				return LTTRUE;
			}
			// Go (with whoever is here) -- on to the documents below
		}
	}

	// Documents: go right to them (they have to be photographed up close)
	if (!m_bDefender)
	{
		const std::vector<BotItem>& items = g_BotMgr.GetItems();
		for (size_t i = 0; i < items.size(); i++)
		{
			if (items[i].eType == BotItem::eIntel && items[i].vPos.DistSqr(vTarget) < 16.0f*16.0f)
			{
				m_bLaneNext = LTTRUE;
				if (!items[i].bAvailable || !PathTo(vTarget)) break;
				SetGoal(vTarget, LTTRUE, fTime + 90.0f);	// The push across the map can be long
				m_bGoalIsObjective = LTTRUE;
				return LTTRUE;
			}
		}
	}

	// Somewhere around the target, not all on the same spot
	LTVector vPush = vTarget;
	CBotNav& nav = g_BotMgr.GetNav();
	int nNode = nav.FindNearest(vTarget + LTVector(RandomFloat(-400.0f, 400.0f), 0.0f, RandomFloat(-400.0f, 400.0f)), 800.0f, LTFALSE);
	if (nNode != BOTNAV_INVALID) vTarget = nav.GetNode(nNode).vPos;

	if (vPos.Dist(vTarget) < 300.0f) return LTFALSE;
	m_bLaneNext = !m_bDefender;
	if (!PathTo(vTarget))
	{
		// No known way there.  Attackers don't give up and loiter at home:
		// go as far toward it as the map is known, then strike out for it.
		if (m_bDefender) return LTFALSE;
		g_BotMgr.NoteRoleFail(dwMyTeam);
		return PickFrontierGoal(fTime, vPush);
	}

	SetGoal(vTarget, LTFALSE, fTime + 75.0f);
	m_bGoalIsObjective = !m_bDefender;
	return LTTRUE;
}

LTBOOL CBotPlayer::PickFrontierGoal(LTFLOAT fTime, const LTVector& vTarget)
{
	LTVector vPos;
	g_pLTServer->GetObjectPos(m_hObject, &vPos);

	CBotNav& nav = g_BotMgr.GetNav();
	int nStart = nav.FindNearest(vPos, 800.0f, LTTRUE);
	if (nStart == BOTNAV_INVALID) return LTFALSE;

	int nFront = nav.FindFrontier(nStart, vTarget);
	if (nFront == BOTNAV_INVALID) return LTFALSE;
	LTVector vFront = nav.GetNode(nFront).vPos;

	// Not there yet: walk to the edge of what we know
	if (HorizDist(vPos, vFront) > 350.0f)
	{
		if (!PathTo(vFront)) return LTFALSE;
		SetGoal(vFront, LTFALSE, fTime + 60.0f);
		m_bGoalIsObjective = LTTRUE;
		m_bGoalIsFrontier  = LTTRUE;
		return LTTRUE;
	}

	// At the edge: head straight for the objective for a while.  Wherever we
	// manage to get across is learned into the waypoints (BotMgr records how
	// everyone moves), so the next push can path through.  Not too often, or
	// a real wall would keep us bumping into it.
	if (fTime < m_fNextExplore) return LTFALSE;
	m_fNextExplore = fTime + 25.0f;

	ClearPath();
	SetGoal(vTarget, LTFALSE, fTime + 12.0f);
	m_bGoalIsObjective = LTTRUE;
	m_bExploring	   = LTTRUE;
	return LTTRUE;
}

LTBOOL CBotPlayer::PickScubaGoal(LTFLOAT fTime)
{
	LTVector vPos;
	g_pLTServer->GetObjectPos(m_hObject, &vPos);

	const std::vector<BotItem>& items = g_BotMgr.GetItems();
	int nBest = -1;
	LTFLOAT fBest = 1.0e30f;
	for (size_t i = 0; i < items.size(); i++)
	{
		if (items[i].eType != BotItem::eGear || !items[i].bScuba || !items[i].bAvailable) continue;
		LTFLOAT fDist = vPos.Dist(items[i].vPos);
		if (fDist < fBest) { fBest = fDist; nBest = (int)i; }
	}

	if (nBest < 0 || !PathTo(items[nBest].vPos)) return LTFALSE;

	SetGoal(items[nBest].vPos, LTTRUE, fTime + 30.0f);
	m_bGoalIsScuba = LTTRUE;
	return LTTRUE;
}

LTBOOL CBotPlayer::PickCampGoal(LTFLOAT fTime)
{
	LTVector vPos;
	g_pLTServer->GetObjectPos(m_hObject, &vPos);

	// Defenders camp overlooking what the enemy is after
	if (m_bDefender && g_pGameServerShell->GetGameType() == COOPERATIVE_ASSAULT)
	{
		LTVector vGuard, vChoke;
		if (g_BotMgr.GetGuardTarget(GetTeamID(), vGuard))
		{
			vPos = vGuard;
			// Half the time: a chokepoint on the way in, not the documents
			if (Chance(0.5f) && g_BotMgr.GetMapInfo().GetDefenseChoke((int)GetTeamID(), vGuard, vChoke)) vPos = vChoke;
		}
	}

	LTVector vLook;
	int nNode = g_BotMgr.PickCampNode(vPos, vLook);
	if (nNode == BOTNAV_INVALID) return LTFALSE;

	const LTVector& vSpot = g_BotMgr.GetNav().GetNode(nNode).vPos;
	if (!PathTo(vSpot)) return LTFALSE;

	SetGoal(vSpot, LTFALSE, fTime + 25.0f);
	m_bGoalIsCamp = LTTRUE;
	m_vCampLook	  = vLook;
	return LTTRUE;
}

LTBOOL CBotPlayer::PickBuddyGoal(LTFLOAT fTime)
{
	LTVector vPos;
	g_pLTServer->GetObjectPos(m_hObject, &vPos);

	CPlayerObj* pBuddy = LTNULL;
	LTFLOAT fBestBuddy = 0.0f;

	const std::vector<CPlayerObj*>& players = g_BotMgr.GetPlayers();
	for (size_t i = 0; i < players.size(); i++)
	{
		CPlayerObj* pPlayer = players[i];
		if (pPlayer == this || pPlayer->GetState() != PS_ALIVE || pPlayer->GetTeamID() != GetTeamID()) continue;

		LTFLOAT fScore = RandomFloat(0.5f, 1.0f) * (pPlayer->IsBot() ? 1.0f : 3.0f);
		if (fScore > fBestBuddy)
		{
			fBestBuddy = fScore;
			pBuddy = pPlayer;
		}
	}

	if (!pBuddy) return LTFALSE;

	LTVector vBuddy;
	g_pLTServer->GetObjectPos(pBuddy->m_hObject, &vBuddy);
	if (vPos.Dist(vBuddy) < 250.0f || !PathTo(vBuddy)) return LTFALSE;

	SetGoal(vBuddy, LTFALSE, fTime + 6.0f);	// They move; re-plan soon
	return LTTRUE;
}

// Why PathTo failed, for the log: not ready, no floor here, no floor there,
// no waypoint route, cells couldn't be joined, ok
int g_anBotPathStats[6] = { 0, 0, 0, 0, 0, 0 };
LTVector g_avBotPathFail[16][2];
int g_nBotPathFail = 0;
// Zipcords: rides, landed on top, fell back down, then held the high ground
int g_anBotZipStats[4] = { 0, 0, 0, 0 };

// Path to vGoal.  The route is planned on the waypoint graph, then turned
// into the exact floor cells to walk over (a few waypoints at a time), so the
// bot never cuts through a wall or off a ledge it didn't mean to take.
LTBOOL CBotPlayer::PathTo(const LTVector& vGoal)
{
	ClearPath();

	CBotWorld& world = g_BotMgr.GetWorld();
	CBotNav& nav = g_BotMgr.GetNav();
	if (!world.IsReady() || nav.GetNumNodes() == 0) { g_anBotPathStats[0]++; return LTFALSE; }

	LTVector vPos;
	g_pLTServer->GetObjectPos(m_hObject, &vPos);

	// Standing somewhere the map scan never reached (pushed up onto a roof
	// at respawn, say): the nearest reachable floor may be right under our
	// feet but not reachable from here.  Walk / drop / jump off to the
	// nearest floor the scan does know first, and plan on from there.
	std::vector<int> escape;
	int nHere = world.FindSpan(vPos);
	if (nHere >= 0 && !(world.GetSpan(nHere).nFlags & (BOTSPAN_REACH | BOTSPAN_SWIM)) &&
		vPos.y - world.GetBody().fHalfHeight - world.GetSpan(nHere).fFloor < 60.0f && FindEscape(nHere, escape))
	{
		LTVector vOut = world.GetSpanPos(escape.back());
		Act("On a spot I can't plan from at (%.0f %.0f %.0f) - getting down to (%.0f %.0f %.0f) first.", vPos.x, vPos.y, vPos.z, vOut.x, vOut.y, vOut.z);
	}

	int nStartSpan = escape.empty() ? world.FindSpanNear(vPos, 6) : escape.back();
	// (Items lie on the floor, waypoints are at body height: lift the goal a
	// bit so both find the floor they're on, not the one under it)
	int nEndSpan   = world.FindSpanNear(vGoal + LTVector(0.0f, 40.0f, 0.0f), 8);
	if (nStartSpan < 0 || world.GetSpan(nStartSpan).nNode < 0) { g_anBotPathStats[1]++; return LTFALSE; }
	if (nEndSpan < 0 || world.GetSpan(nEndSpan).nNode < 0) { g_anBotPathStats[2]++; return LTFALSE; }
	int nStart = world.GetSpan(nStartSpan).nNode;
	int nEnd   = world.GetSpan(nEndSpan).nNode;

	LTFLOAT fTime = g_pLTServer->GetTime();
	if (fTime > m_fAvoidClear)
	{
		m_AvoidLinks.clear();
		m_fAvoidClear = fTime + 90.0f;
	}

	// Steer around where our side keeps getting killed (learned, per team)
	int nDangerTeam = (g_pGameServerShell->GetGameType() == COOPERATIVE_ASSAULT) ? (int)GetTeamID() : 0;
	m_Route.pDanger = g_BotMgr.HasFeature(GetTeamID(), CBotMgr::eFeatDanger) ? nav.GetDanger(nDangerTeam) : LTNULL;
	m_Route.fNoAirMul = m_bHasScuba ? 1.0f : 8.0f;

	// Attacking: along our lane (main road, flank, high route, water)
	LTBOOL bLane = m_bLaneNext || (m_bHasGoal && m_bGoalIsObjective && vGoal.DistSqr(m_vGoal) < 1.0f);
	m_Route.pLane = bLane ? GetLaneCost() : LTNULL;
	m_bLaneNext = LTFALSE;

	if (nStart != nEnd)
	{
		// (Links we got stuck on are only a preference: if they're the only
		// way, take them anyway)
		if (!nav.FindPath(nStart, nEnd, m_Path, &m_Route) && !m_AvoidLinks.empty())
		{
			m_AvoidLinks.clear();
			nav.FindPath(nStart, nEnd, m_Path, &m_Route);
		}
		if (m_Path.empty())
		{
			g_anBotPathStats[3]++;
			g_avBotPathFail[g_nBotPathFail % 16][0] = vPos;
			g_avBotPathFail[g_nBotPathFail % 16][1] = vGoal;
			g_nBotPathFail++;
			return LTFALSE;
		}
	}
	m_Path.insert(m_Path.begin(), nStart);
	m_nPathIndex	= 0;
	m_nCorridorEnd	= 0;
	m_nGoalSpan		= nEndSpan;
	m_SpanPath.clear();
	m_SpanPath.insert(m_SpanPath.end(), escape.begin(), escape.end());	// (ends with nStartSpan)
	if (m_SpanPath.empty()) m_SpanPath.push_back(nStartSpan);
	m_nSpanIdx		= 1;
	m_fOffPathSince	= -1.0f;
	m_fNextLook		= 0.0f;

	if (!ExtendSpanPath())
	{
		g_anBotPathStats[4]++;
		g_avBotPathFail[g_nBotPathFail % 16][0] = vPos;
		g_avBotPathFail[g_nBotPathFail % 16][1] = vGoal + LTVector(0.0f, 0.0f, 0.001f);	// (marks "cells")
		g_nBotPathFail++;
		ClearPath();
		return LTFALSE;
	}
	g_anBotPathStats[5]++;
	return LTTRUE;
}

// Breadth-first from an unreachable span over the scanned moves and links to
// the nearest reachable one (where players normally walk, so the landing is
// safe).  cells = nFrom ... the reachable span.
LTBOOL CBotPlayer::FindEscape(int nFrom, std::vector<int>& cells)
{
	CBotWorld& world = g_BotMgr.GetWorld();
	cells.clear();

	std::vector<int> queue, links;
	std::map<int, int> from;
	queue.push_back(nFrom);
	from[nFrom] = -1;
	int nFound = -1;
	for (size_t q = 0; q < queue.size() && q < 3000 && nFound < 0; q++)
	{
		int n = queue[q];
		const BotSpan& s = world.GetSpan(n);
		int aNext[8 + 16];
		int nNext = 0;
		for (int d = 0; d < 8; d++)
		{
			if (s.aNbr[d] >= 0 && s.GetMove(d) != eBotMoveNone) aNext[nNext++] = s.aNbr[d];
		}
		world.GetLinksFrom(n, links);
		for (size_t l = 0; l < links.size() && nNext < 24; l++) aNext[nNext++] = world.GetLinks()[links[l]].nTo;

		for (int i = 0; i < nNext; i++)
		{
			int m = aNext[i];
			if (from.count(m)) continue;
			from[m] = n;
			if (world.GetSpan(m).nFlags & BOTSPAN_REACH) { nFound = m; break; }
			queue.push_back(m);
		}
	}
	if (nFound < 0) return LTFALSE;

	for (int n = nFound; n >= 0; n = from[n]) cells.push_back(n);
	std::reverse(cells.begin(), cells.end());
	return LTTRUE;
}

// Turn the next stretch of waypoints into floor cells
LTBOOL CBotPlayer::ExtendSpanPath()
{
	CBotWorld& world = g_BotMgr.GetWorld();
	CBotNav& nav = g_BotMgr.GetNav();
	if (m_SpanPath.empty()) return LTFALSE;
	if (m_nCorridorEnd >= (int)m_Path.size() - 1 && m_SpanPath.back() == m_nGoalSpan) return LTTRUE;

	int nLast = (int)m_Path.size() - 1;
	int nTo = LTMIN(m_nCorridorEnd + 10, nLast);
	int nTarget = (nTo == nLast) ? m_nGoalSpan : nav.GetNode(m_Path[nTo]).nSpan;

	// The waypoints on this stretch and their neighbours (a little room to
	// round corners); long links only if the route takes them
	if ((int)m_Allowed.size() != nav.GetNumNodes()) m_Allowed.assign(nav.GetNumNodes(), 0);
	std::vector<int> marked;
	for (int i = LTMAX(m_nCorridorEnd - 1, 0); i <= nTo; i++)
	{
		int n = m_Path[i];
		if (!m_Allowed[n]) { m_Allowed[n] = 1; marked.push_back(n); }
		const BotNavNode& node = nav.GetNode(n);
		for (int l = 0; l < node.nLinks; l++)
		{
			const BotNavLink& link = node.aLinks[l];
			if (link.nFlags & (BOTLINK_ZIP | BOTLINK_LADDER | BOTLINK_GAP | BOTLINK_DROP | BOTLINK_JUMP)) continue;
			if (!m_Allowed[link.nTo]) { m_Allowed[link.nTo] = 1; marked.push_back(link.nTo); }
		}
	}

	std::vector<int> part;
	LTBOOL bOk = world.FindSpanPath(m_SpanPath.back(), nTarget, &m_Allowed, part, 12000);
	if (!bOk) bOk = world.FindSpanPath(m_SpanPath.back(), nTarget, LTNULL, part, 6000);

	for (size_t i = 0; i < marked.size(); i++) m_Allowed[marked[i]] = 0;
	if (!bOk) return LTFALSE;

	m_SpanPath.insert(m_SpanPath.end(), part.begin(), part.end());
	m_nCorridorEnd = nTo;
	return LTTRUE;
}

// How we get from path cell i-1 to cell i
int CBotPlayer::SpanStepKind(int i, int& nLink)
{
	nLink = BOTW_INVALID;
	if (i <= 0 || i >= (int)m_SpanPath.size()) return eStepWalk;

	CBotWorld& world = g_BotMgr.GetWorld();
	int a = m_SpanPath[i - 1], b = m_SpanPath[i];
	const BotSpan& s = world.GetSpan(a);
	for (int d = 0; d < 8; d++)
	{
		if (s.aNbr[d] != b) continue;
		switch (s.GetMove(d))
		{
			case eBotMoveClimb:	return eStepClimb;
			case eBotMoveDrop:	return eStepDrop;
			case eBotMoveWalk:	return eStepWalk;
		}
	}
	nLink = world.FindLink(a, b);
	if (nLink < 0) return eStepWalk;
	switch (world.GetLinks()[nLink].nType)
	{
		case eBotLinkZip:		return eStepZip;
		case eBotLinkLadder:	return eStepLadder;
		default:				return eStepGap;
	}
}

// ----------------------------------------------------------------------- //
//	Movement
// ----------------------------------------------------------------------- //

LTVector CBotPlayer::GetNavMoveDir(LTFLOAT fTime, LTBOOL& bJump)
{
	if (m_bCamping || !m_bHasGoal) return LTVector(0.0f, 0.0f, 0.0f);

	LTVector vPos;
	g_pLTServer->GetObjectPos(m_hObject, &vPos);

	CBotWorld& world = g_BotMgr.GetWorld();

	// A planned jump (gap, ledge): did we make it?  Missed ones (we came
	// down well below where it lands, e.g. off a walkway onto the floor) count
	// against the link, so after a few misses the route stops using it.
	if (m_nJumpLink >= 0 && m_nJumpLink < world.GetLinkCount() && fTime - m_fJumpTime > 0.3f && IsOnGround())
	{
		LTVector vLand = world.GetSpanPos(world.GetLinks()[m_nJumpLink].nTo);
		if (vPos.y < vLand.y - 70.0f || (fTime - m_fJumpTime > 3.0f && vPos.Dist(vLand) > 150.0f))
		{
			world.GetLinkRW(m_nJumpLink).nFails++;
			Act("Missed the jump from (%.0f %.0f %.0f) to (%.0f %.0f %.0f) - ended up at (%.0f %.0f %.0f), finding another way.",
				m_vJumpFrom.x, m_vJumpFrom.y, m_vJumpFrom.z, vLand.x, vLand.y, vLand.z, vPos.x, vPos.y, vPos.z);
			m_nJumpLink = BOTW_INVALID;
			LTVector vGoal = m_vGoal;
			if (m_bHasGoal && !PathTo(vGoal)) m_bHasGoal = LTFALSE;
			return LTVector(0.0f, 0.0f, 0.0f);
		}
		m_nJumpLink = BOTW_INVALID;
	}

	if (!m_SpanPath.empty() && world.IsReady())
	{
		int nCur = world.FindSpan(vPos);
		int nSize = (int)m_SpanPath.size();

		// Where are we on the path?  (Look a bit ahead: we may have skipped
		// cells by cutting a corner, or slid down a slope.)
		if (nCur >= 0)
		{
			LTBOOL bFound = LTFALSE;
			for (int j = LTMIN(m_nSpanIdx + 24, nSize - 1); j >= LTMAX(m_nSpanIdx - 3, 0); j--)
			{
				if (m_SpanPath[j] != nCur) continue;
				if (j + 1 > m_nSpanIdx)
				{
					m_nSpanIdx = j + 1;
					m_fNextLook = 0.0f;
				}
				bFound = LTTRUE;
				break;
			}

			// Cutting a corner crosses cells that aren't on the path: fine as
			// long as the spot we're heading for is still straight ahead
			if (!bFound && m_nLookIdx >= m_nSpanIdx && m_nLookIdx < nSize &&
				world.CanWalkStraight(nCur, m_SpanPath[m_nLookIdx], LTTRUE))
			{
				bFound = LTTRUE;
			}

			// Knocked off the path (pushed, fell, took a wrong drop): plan again
			if (bFound || !IsOnGround() || m_bInWater) m_fOffPathSince = -1.0f;
			else if (m_fOffPathSince < 0.0f) m_fOffPathSince = fTime;
			else if (fTime - m_fOffPathSince > 0.8f)
			{
				// Over and over at the same spot (wedged on a rock lip, sliding
				// back down a slope): that's being stuck, whatever the path
				// index says - unstick (hop, avoid that bit, other goal)
				if (vPos.Dist(m_vOffPathPos) < 120.0f && fTime - m_fOffPathTime < 15.0f) m_nOffPathRepeats++;
				else m_nOffPathRepeats = 0;
				m_vOffPathPos  = vPos;
				m_fOffPathTime = fTime;
				if (m_nOffPathRepeats >= 2)
				{
					m_nOffPathRepeats = 0;
					m_fOffPathSince = -1.0f;
					OnStuck(fTime, bJump, LTTRUE);
					return Horiz(m_bHasMoveTarget ? m_vMoveTarget - vPos : LTVector(0.0f, 0.0f, 0.0f));
				}

				LTVector vGoal = m_vGoal;
				LTBOOL bOk = PathTo(vGoal);
				Act("Off my path at (%.0f %.0f %.0f)%s - planning again: %s.", vPos.x, vPos.y, vPos.z,
					fTime - m_fZipLandStart < 10.0f ? " just after a zipcord ride" : "",
					bOk ? "new route" : "no route, dropping it");
				if (!bOk) { m_bHasGoal = LTFALSE; return LTVector(0.0f, 0.0f, 0.0f); }
				nSize = (int)m_SpanPath.size();
			}
		}

		// Plan the next stretch before we run out of cells
		if (m_nSpanIdx + 12 >= nSize && m_nCorridorEnd < (int)m_Path.size() - 1)
		{
			if (!ExtendSpanPath() && m_nSpanIdx >= nSize)
			{
				LTVector vGoal = m_vGoal;
				if (!PathTo(vGoal)) { m_bHasGoal = LTFALSE; return LTVector(0.0f, 0.0f, 0.0f); }
			}
			nSize = (int)m_SpanPath.size();
		}

		// Keep m_nPathIndex (the next waypoint) roughly in step, for the log
		// and the vehicle logic
		if (nCur >= 0)
		{
			int nNode = world.GetSpan(nCur).nNode;
			for (int k = m_nPathIndex; k < (int)m_Path.size() && k < m_nPathIndex + 6; k++)
			{
				if (m_Path[k] == nNode) { m_nPathIndex = k + 1; break; }
			}
		}

		if (m_nSpanIdx < nSize)
		{
			int nLink;
			int nKind = SpanStepKind(m_nSpanIdx, nLink);
			int nPrev = m_SpanPath[m_nSpanIdx - 1];
			int nNext = m_SpanPath[m_nSpanIdx];
			LTVector vPrev = world.GetSpanPos(nPrev);
			LTVector vNext = world.GetSpanPos(nNext);

			switch (nKind)
			{
				case eStepZip:
				{
					// Fire the cord only from where the whole ride is clear:
					// not from inside a doorway or under a lintel, where we'd
					// fly into the frame and hang there
					const BotWorldLink& link = world.GetLinks()[nLink];
					LTFLOAT fFromTakeoff = HorizDist(vPos, vPrev);
					if (fFromTakeoff < 40.0f || (m_fZipLineup >= 0.0f && fFromTakeoff < 140.0f))
					{
						if (m_fZipLineup < 0.0f || fTime - m_fZipLineup > 6.0f) m_fZipLineup = fTime;

						const BotBody& body = world.GetBody();
						if (IsOnGround() && CBotWorld::IsZipRideClear(vPos, link.vAux, body.fHalfHeight, body.fRadius))
						{
							// Rode this cord for this goal already and we're back
							// at the bottom: the way on from the top doesn't work
							// for us (a drop onto a ledge we keep missing).  Don't
							// go round again.
							if (nLink == m_nGoalZipLink && m_fGoalZipSet == m_fGoalSetTime)
							{
								m_fZipLineup = -1.0f;
								Act("Already went up this zipcord for this and ended up back here - giving up on (%.0f %.0f %.0f).", m_vGoal.x, m_vGoal.y, m_vGoal.z);
								if (m_bGoalIsItem)
								{
									// Skip it for a minute (RecentlyVisited counts 15 s from this)
									m_avVisited[m_nNextVisited]		= m_vGoal;
									m_afVisitedTime[m_nNextVisited] = fTime + 45.0f;
									m_nNextVisited = (m_nNextVisited + 1) % kVisitedItems;
								}
								m_bHasGoal = LTFALSE;
								m_bCamping = LTFALSE;
								ClearPath();
								return LTVector(0.0f, 0.0f, 0.0f);
							}
							m_nGoalZipLink = nLink;
							m_fGoalZipSet  = m_fGoalSetTime;
							StartZip(nLink, fTime);
							return LTVector(0.0f, 0.0f, 0.0f);
						}

						if (fTime - m_fZipLineup > 2.5f)
						{
							// No clear shot at the hook from here (door in the way?)
							m_fZipLineup = -1.0f;
							world.GetLinkRW(nLink).nFails++;
							Act("No clear line to the zipcord hook from (%.0f %.0f %.0f) - finding another way.", vPos.x, vPos.y, vPos.z);
							LTVector vGoal = m_vGoal;
							if (!PathTo(vGoal)) m_bHasGoal = LTFALSE;
							return LTVector(0.0f, 0.0f, 0.0f);
						}

						// Step out toward the hook, into the open
						m_vMoveTarget = link.vAux;
						m_bHasMoveTarget = LTTRUE;
						return Horiz(link.vAux - vPos);
					}
					m_vMoveTarget = vPrev;
					m_bHasMoveTarget = LTTRUE;
					return Horiz(vPrev - vPos);
				}

				case eStepLadder:
				{
					const BotWorldLink& link = world.GetLinks()[nLink];
					m_vMoveTarget = vNext;
					m_bHasMoveTarget = LTTRUE;
					if (!m_bOnLadder)
					{
						// Walk into the ladder
						LTVector vLadder = link.vAux;
						return Horiz(vLadder - vPos);
					}
					return Horiz(vNext - vPos);
				}

				case eStepGap:
				{
					m_vMoveTarget = vNext;
					m_bHasMoveTarget = LTTRUE;
					LTFLOAT fToLaunch = HorizDist(vPos, vPrev);
					if (fToLaunch < 30.0f && IsOnGround())
					{
						// Take off (at a run: we came along the path behind us)
						bJump = LTTRUE;
						m_nJumpLink = nLink;
						m_vJumpFrom = vPos;
						m_fJumpTime = fTime;
						return Horiz(vNext - vPos);
					}
					// Line up: approach the launch cell along the jump direction
					LTVector vDir = Horiz(vNext - vPrev);
					LTVector vRunUp = vPrev - vDir * 40.0f;
					if (fToLaunch > 60.0f && HorizDist(vPos, vRunUp) > 20.0f && world.CanWalkStraight(vPos, vRunUp))
					{
						return Horiz(vRunUp - vPos);
					}
					return Horiz(vPrev - vPos);
				}

				case eStepClimb:
				{
					m_vMoveTarget = vNext;
					m_bHasMoveTarget = LTTRUE;
					if (HorizDist(vPos, vNext) < 60.0f && (IsOnGround() || m_bInWater)) bJump = LTTRUE;
					return Horiz(vNext - vPos);
				}

				default:
				break;
			}

			// Walking (or dropping, or swimming): head for the farthest cell
			// ahead we can reach in a straight line without scraping a wall
			if (fTime > m_fNextLook || m_nLookIdx < m_nSpanIdx)
			{
				m_fNextLook = fTime + 0.15f;
				m_nLookIdx = m_nSpanIdx;

				LTBOOL bSwim = m_bInWater || (world.GetSpan(nNext).nFlags & BOTSPAN_SWIM);
				// (Just stuck under water: only a cell or two ahead for a while.
				// A far target can be in sight through a gap the body doesn't
				// fit; the cells themselves were checked for a whole body.)
				int nAhead = bSwim ? (fTime < m_fShortLookUntil ? 2 : 10) : 14;
				int nMax = LTMIN(m_nSpanIdx + nAhead, nSize - 1);

				// Only as far as plain walking goes (stop before a jump, ladder...)
				int nPlainEnd = m_nSpanIdx;
				for (int j = m_nSpanIdx + 1; j <= nMax; j++)
				{
					int nL;
					int k = SpanStepKind(j, nL);
					if (k != eStepWalk) break;
					nPlainEnd = j;
				}

				if (nCur >= 0)
				{
					for (int j = nPlainEnd; j > m_nSpanIdx; j--)
					{
						LTBOOL bOk;
						if (bSwim)
						{
							LTVector vT = world.GetSpanPos(m_SpanPath[j]);
							bOk = CBotNav::IsVisible(vPos, vT) &&
								  CBotNav::IsVisible(vPos + LTVector(0.0f, 30.0f, 0.0f), vT + LTVector(0.0f, 30.0f, 0.0f));
						}
						else
						{
							bOk = world.CanWalkStraight(nCur, m_SpanPath[j], LTFALSE);
						}
						if (bOk) { m_nLookIdx = j; break; }
					}
				}
			}

			int nTargetSpan = m_SpanPath[LTMIN(m_nLookIdx, nSize - 1)];
			LTVector vTarget = world.GetSpanPos(nTargetSpan);

			// Last cell: the goal itself (items have to be touched)
			if (m_nLookIdx >= nSize - 1 && m_SpanPath.back() == m_nGoalSpan && HorizDist(vTarget, m_vGoal) < 48.0f)
			{
				vTarget.x = m_vGoal.x;
				vTarget.z = m_vGoal.z;
			}

			m_vMoveTarget = vTarget;
			m_bHasMoveTarget = LTTRUE;

			// Diving down / coming up a shaft: the target is (nearly) straight
			// below or above.  Go vertical first, sideways only once we're at
			// its depth (a tunnel can run under the ledge beside the shaft).
			LTFLOAT fH = HorizDist(vPos, vTarget), fDY = vTarget.y - vPos.y;
			// (Steep counts too: 100u across and 1500u up from the reservoir
			// floor is a climb, not a swim across - the bot pinned itself
			// under the overhang pushing at it.)
			if ((fH < 96.0f || (LTFLOAT)fabs(fDY) > fH * 2.5f) && (LTFLOAT)fabs(fDY) > 60.0f && (world.GetSpan(nTargetSpan).nFlags & BOTSPAN_SWIM))
			{
				// Not at the edge of a shaft, where the body catches on the lip:
				// over to where the water is open all the way down (or up),
				// then straight down (up) there
				LTVector vDive;
				LTFLOAT yLo = LTMIN(vPos.y, vTarget.y), yHi = LTMAX(vPos.y, vTarget.y);
				if (world.FindDiveColumn(vPos, yLo, yHi, 6, vDive))
				{
					if (HorizDist(vPos, vDive) > 12.0f)
					{
						m_vMoveTarget = LTVector(vDive.x, vPos.y, vDive.z);	// across at this depth first
						return Horiz(vDive - vPos);
					}
					if (m_bInWater || m_bHeadInWater) return LTVector(0.0f, 0.0f, 0.0f);
				}
				else if (m_bInWater || m_bHeadInWater)
				{
					return LTVector(0.0f, 0.0f, 0.0f);
				}

				// Standing on a ledge beside the water: step back into it first
				// (the cell before, where the water is open)
				for (int k = LTMIN(m_nLookIdx, nSize - 1) - 1; k >= LTMAX(m_nSpanIdx - 2, 0); k--)
				{
					LTVector vEntry = world.GetSpanPos(m_SpanPath[k]);
					if ((world.GetSpan(m_SpanPath[k]).nFlags & BOTSPAN_SWIM) && HorizDist(vPos, vEntry) > 8.0f)
					{
						m_vMoveTarget = vEntry;
						return Horiz(vEntry - vPos);
					}
				}
			}

			if (fH > 4.0f) return Horiz(vTarget - vPos);
		}
	}

	m_vMoveTarget = m_vGoal;
	m_bHasMoveTarget = LTTRUE;

	// Intelligence has to be "used", not touched...
	if (m_bGoalIsItem && fTime > m_fNextIntelTry && vPos.Dist(m_vGoal) < 90.0f)
	{
		const std::vector<BotItem>& items = g_BotMgr.GetItems();
		for (size_t i = 0; i < items.size(); i++)
		{
			if (items[i].eType == BotItem::eIntel && items[i].vPos.DistSqr(m_vGoal) < 16.0f*16.0f)
			{
				if (g_BotMgr.ActivateIntel(m_vGoal, this) && Chance(0.5f))
				{
					g_BotMgr.BotSay(this, Chance(0.5f) ? "Got the documents!" : "Intel photographed", LTTRUE);
				}
				m_fNextIntelTry = fTime + 1.0f;
				m_fGoalTimeout = fTime + 1.0f;	// Move on shortly
				break;
			}
		}
	}

	if (HorizDist(vPos, m_vGoal) < (m_bGoalIsItem ? 8.0f : BOT_ARRIVE_DIST))
	{
		return LTVector(0.0f, 0.0f, 0.0f);
	}

	// No cell path (or past its end): straight at the goal, but not into a
	// wall or off a ledge
	LTVector vDir = Horiz(m_vGoal - vPos);
	if (IsBlocked(vDir, 48.0f))
	{
		static const LTFLOAT s_aTurn[] = { 0.5f, -0.5f, 1.0f, -1.0f, 1.5f, -1.5f };
		for (int t = 0; t < 6; t++)
		{
			LTVector vTry = RotateYaw(vDir, s_aTurn[t]);
			if (!IsBlocked(vTry, 48.0f)) { vDir = vTry; break; }
		}
	}

	return vDir;
}

LTVector CBotPlayer::GetCombatMoveDir(LTFLOAT fTime, LTBOOL& bJump)
{
	CPlayerObj* pEnemy = GetEnemy();
	if (!pEnemy) return LTVector(0.0f, 0.0f, 0.0f);

	LTVector vPos, vEnemyPos;
	g_pLTServer->GetObjectPos(m_hObject, &vPos);
	g_pLTServer->GetObjectPos(pEnemy->m_hObject, &vEnemyPos);

	LTVector vToEnemy = Horiz(vEnemyPos - vPos);
	LTVector vSide(vToEnemy.z, 0.0f, -vToEnemy.x);
	LTFLOAT fDist = HorizDist(vPos, vEnemyPos);
	LTFLOAT fWant = GetPreferredRange();

	// Smart bots duck behind something while reloading or when hurt
	LTBOOL bReloading = (fTime < m_fReloadDone);
	if (m_pP->bSmart && !m_bCamping && (bReloading || GetHealthFraction() < 0.5f))
	{
		if (fTime > m_fCoverUntil)
		{
			LTVector vEnemyEye = vEnemyPos + LTVector(0.0f, 30.0f, 0.0f);
			int nCover = Chance(Gene(eGeneCover)) ? g_BotMgr.GetNav().FindCover(vPos, vEnemyEye, 500.0f) : BOTNAV_INVALID;
			m_vCover = (nCover != BOTNAV_INVALID) ? g_BotMgr.GetNav().GetNode(nCover).vPos : LTVector(0.0f, 0.0f, 0.0f);
			m_fCoverUntil = fTime + 1.5f;
		}

		if (m_vCover.MagSqr() > 0.0f && HorizDist(vPos, m_vCover) > 30.0f)
		{
			// Only if we can run straight there (not through what hides it)
			if (g_BotMgr.GetWorld().CanWalkStraight(vPos, m_vCover)) return Horiz(m_vCover - vPos);
			m_vCover.Init();
		}
	}
	if (bReloading && m_pP->bSmart) fWant *= 1.8f;

	// Campers hold their spot: just a small side step now and then (never
	// off the perch).
	if (m_bCamping)
	{
		if (fTime > m_fNextStrafeChange)
		{
			m_fStrafeDir = Chance(0.6f) ? 0.0f : ((GetRandom(0, 1) == 0) ? -1.0f : 1.0f);
			m_fNextStrafeChange = fTime + RandomFloat(0.5f, 1.5f);
		}
		LTVector vStep = vSide * m_fStrafeDir;
		if (m_fStrafeDir != 0.0f && (IsBlocked(vStep, 64.0f) || HorizDist(vPos + vStep * 64.0f, m_vCampSpot) > 90.0f)) vStep.Init();
		return vStep;
	}

	// Strafe; rushers circle-strafe (change direction less often)...

	if (fTime > m_fNextStrafeChange)
	{
		m_fStrafeDir = (GetRandom(0, 1) == 0) ? -1.0f : 1.0f;
		LTFLOAT fHold = ((m_nStyle == eBotRusher) ? RandomFloat(0.8f, 2.0f) : RandomFloat(0.25f, 1.2f)) * Gene(eGeneStrafe);
		m_fNextStrafeChange = fTime + fHold;
	}

	LTVector vDir = vSide * m_fStrafeDir;
	if (fDist > fWant * 1.3f)		vDir += vToEnemy * (0.5f + m_pP->fAggression * Gene(eGeneAggro));
	else if (fDist < fWant * 0.6f)	vDir -= vToEnemy;

	vDir = Horiz(vDir);

	// Don't strafe into walls or off ledges (the map knows where they are)
	if (IsBlocked(vDir, 64.0f))
	{
		m_fStrafeDir = -m_fStrafeDir;
		vDir = Horiz(vSide * m_fStrafeDir + (fDist > fWant ? vToEnemy : -vToEnemy) * 0.5f);
		if (IsBlocked(vDir, 64.0f))
		{
			vDir = Horiz(vSide * m_fStrafeDir);
			if (IsBlocked(vDir, 64.0f)) vDir = (fDist > fWant && !IsBlocked(vToEnemy, 64.0f)) ? vToEnemy : LTVector(0.0f, 0.0f, 0.0f);
		}
	}

	// Dodge jumps: jumpy styles and good players do it more, and right after
	// getting hit...
	if (fTime > m_fNextDodge)
	{
		LTBOOL bJustHurt = (fTime - m_fHurtTime < 0.5f);
		if ((bJustHurt && Chance((0.3f + m_pP->fJumpiness * 0.6f) * Gene(eGeneDodge))) || Chance(m_pP->fJumpiness * (0.4f + m_fSkill * 0.5f) * Gene(eGeneDodge)))
		{
			if (IsHopSafe(vDir.MagSqr() > 0.01f ? vDir : vSide)) bJump = LTTRUE;
		}
		m_fNextDodge = fTime + RandomFloat(0.6f, 2.0f) * (1.4f - m_pP->fJumpiness * 0.6f);
	}

	return vDir;
}

// A hop (bunny hop, dodge) along vDir lands on the same floor: plain walkable
// floor all the way to where the jump comes down, plus a body width.  Up on a
// platform (zipcord landing, walkway, roof) a hop near the edge otherwise
// sails off it.
LTBOOL CBotPlayer::IsHopSafe(const LTVector& vDir)
{
	CBotWorld& world = g_BotMgr.GetWorld();
	if (!world.IsReady()) return LTFALSE;
	const BotBody& body = world.GetBody();
	LTFLOAT fAir = (body.fGravity > 1.0f) ? 2.0f * body.fJumpVel / body.fGravity : 0.6f;
	LTFLOAT fDist = GetMoveSpeed() * fAir + 2.0f * body.fRadius;

	LTVector vPos;
	g_pLTServer->GetObjectPos(m_hObject, &vPos);
	int nCur = world.FindSpan(vPos);
	LTVector vD = Horiz(vDir);
	if (nCur < 0 || vD.MagSqr() < 0.01f) return !m_bInWater && !m_bOnLadder;

	// Only a long fall is dangerous (no fall damage, but off a walkway onto
	// the floor below, or into the void).  A wall stops the hop; a small step
	// down (off a rock onto the floor beside it) is fine.
	const LTFLOAT kMaxFall = 250.0f;
	int d = world.DirIndex(vD);
	int nSteps = LTMAX(1, (int)(fDist / CBotWorld::kCell + 0.5f));
	LTFLOAT fStart = world.GetSpan(nCur).fFloor;
	for (int i = 0; i < nSteps; i++)
	{
		const BotSpan& s = world.GetSpan(nCur);
		int m = s.GetMove(d);
		if (m == eBotMoveWalk && s.aNbr[d] >= 0) { nCur = s.aNbr[d]; continue; }
		if (m == eBotMoveClimb) return LTTRUE;			// wall / ledge up: the hop just stops
		if (m == eBotMoveDrop && s.aNbr[d] >= 0)
		{
			const BotSpan& b = world.GetSpan(s.aNbr[d]);
			return fStart - b.fFloor <= kMaxFall && (b.nFlags & BOTSPAN_REACH);
		}
		// No move: a wall (some floor above us there) or nothing to land on
		int x, z, dx, dz;
		world.GetSpanCell(nCur, x, z);
		CBotWorld::DirVector(d, dx, dz);
		return world.GetSpanAt(x + dx, z + dz, s.fFloor, kMaxFall, 100000.0f) >= 0;
	}
	return LTTRUE;
}

// Would moving fDist along vDir hit a wall or go off a ledge?  Read from the
// measured map (cheap and exact); rays only where the map has no floor.
LTBOOL CBotPlayer::IsBlocked(const LTVector& vDir, LTFLOAT fDist)
{
	LTVector vPos;
	g_pLTServer->GetObjectPos(m_hObject, &vPos);

	CBotWorld& world = g_BotMgr.GetWorld();
	if (world.IsReady() && !m_bInWater)
	{
		int nSpan = world.FindSpan(vPos);
		if (nSpan >= 0)
		{
			LTVector vD = Horiz(vDir);
			if (vD.MagSqr() < 0.01f) return LTFALSE;
			int nSteps = LTMAX(1, (int)(fDist / CBotWorld::kCell + 0.5f));
			int nCur = nSpan;
			for (int i = 0; i < nSteps; i++)
			{
				const BotSpan& s = world.GetSpan(nCur);
				int d = world.DirIndex(vD);
				if (s.GetMove(d) != eBotMoveWalk || s.aNbr[d] < 0) return LTTRUE;
				nCur = s.aNbr[d];
			}
			return LTFALSE;
		}
	}

	LTVector vTo = vPos + vDir * fDist;

	// Wall in the way (knee height), or a drop ahead?
	if (!CBotNav::IsVisible(vPos - LTVector(0.0f, 20.0f, 0.0f), vTo - LTVector(0.0f, 20.0f, 0.0f))) return LTTRUE;

	IntersectQuery IQuery;
	IntersectInfo IInfo;
	IQuery.m_From	= vTo;
	IQuery.m_To		= vTo - LTVector(0.0f, 140.0f, 0.0f);
	IQuery.m_Flags	= INTERSECT_OBJECTS | IGNORE_NONSOLID;
	IQuery.m_FilterFn = LTNULL;

	return !g_pLTServer->IntersectSegment(&IQuery, &IInfo);
}

void CBotPlayer::UpdateStuck(LTFLOAT fTime, LTBOOL bWantMove, LTBOOL& bJump)
{
	// Not getting any closer to where we're steering for a while (a crowd in
	// a doorway, a door that won't open, a prop the scan didn't see)?
	if (bWantMove && m_bHasMoveTarget && !m_bZipping && !m_bZipLanding)
	{
		LTVector vPos;
		g_pLTServer->GetObjectPos(m_hObject, &vPos);
		LTFLOAT fDist = vPos.Dist(m_vMoveTarget);

		// Progress is getting closer to the move target, or getting further
		// along the path: on a long straight run the target is a point far
		// ahead that moves with us, so its distance hardly changes
		if (m_nSpanIdx < m_nProgressSpan) m_nProgressSpan = m_nSpanIdx;	// new path
		if (fDist < m_fBestTargetDist - 24.0f || m_nSpanIdx > m_nProgressSpan)
		{
			m_fBestTargetDist = fDist;
			m_nProgressSpan	  = m_nSpanIdx;
			m_fProgressCheck  = fTime + 3.0f;
		}
		else if (fTime > m_fProgressCheck)
		{
			OnStuck(fTime, bJump, LTFALSE);
			m_fBestTargetDist = 1.0e30f;
			m_fProgressCheck  = fTime + 3.0f;
			return;
		}
	}
	else
	{
		m_fBestTargetDist = 1.0e30f;
		m_fProgressCheck  = fTime + 3.0f;
	}

	if (fTime < m_fStuckCheck) return;
	m_fStuckCheck = fTime + 0.5f;

	LTVector vPos;
	g_pLTServer->GetObjectPos(m_hObject, &vPos);

	if (!bWantMove || m_bZipping || vPos.Dist(m_vStuckPos) > 24.0f)
	{
		m_nStuckCount = 0;
		m_vStuckPos = vPos;
		return;
	}

	// Pushing but not moving (half a second at a time)
	m_nStuckCount++;
	m_vStuckPos = vPos;

	if (m_nStuckCount == 2)
	{
		// Something in the way: step aside along the floor (left or right of
		// where we're going, whichever is open), with a hop over small stuff
		LTVector vWant = m_bHasMoveTarget ? Horiz(m_vMoveTarget - vPos) : LTVector(LTFLOAT(sin(m_fYaw)), 0.0f, LTFLOAT(cos(m_fYaw)));
		m_vUnstickDir.Init();
		LTFLOAT fSide = Chance(0.5f) ? 1.0f : -1.0f;
		static const LTFLOAT s_aTurn[] = { 1.2f, 2.0f, 0.6f, 2.6f };
		for (int t = 0; t < 8 && m_vUnstickDir.MagSqr() < 0.01f; t++)
		{
			LTVector vTry = RotateYaw(vWant, s_aTurn[t / 2] * ((t & 1) ? -fSide : fSide));
			if (!IsBlocked(vTry, 64.0f)) m_vUnstickDir = vTry;
		}
		m_fUnstickUntil = fTime + 0.5f;
		bJump = IsHopSafe(m_vUnstickDir.MagSqr() > 0.01f ? m_vUnstickDir : vWant);
	}
	else if (m_nStuckCount >= 4)
	{
		OnStuck(fTime, bJump, LTTRUE);
		m_nStuckCount = 0;
	}
}

// The nearest dry, reachable waypoint (by the graph from where we are)
LTBOOL CBotPlayer::FindDryLand(const LTVector& vPos, LTVector& vDry)
{
	CBotNav& nav = g_BotMgr.GetNav();
	int nStart = nav.FindNearest(vPos, 400.0f, LTFALSE);
	if (nStart < 0) return LTFALSE;

	std::vector<int> queue;
	std::vector<char> seen(nav.GetNumNodes(), 0);
	queue.push_back(nStart);
	seen[nStart] = 1;
	for (size_t q = 0; q < queue.size() && q < 3000; q++)
	{
		const BotNavNode& node = nav.GetNode(queue[q]);
		if (!(node.nFlags & (BOTNODE_WATER | BOTNODE_NOAIR)) && nav.IsPlayable(queue[q]))
		{
			vDry = node.vPos;
			return LTTRUE;
		}
		for (int l = 0; l < node.nLinks; l++)
		{
			int m = node.aLinks[l].nTo;
			if (!seen[m]) { seen[m] = 1; queue.push_back(m); }
		}
	}
	return LTFALSE;
}

// Can't get on along the path: plan again (around this spot), and give up
// on the goal if that keeps happening.
// What a ray / our feet hit, for the log.  Returns 0 = level geometry,
// 1 = a character (a player, a bot, a body), 2 = some other object.
static int DescribeObject(HOBJECT hObj, char* szOut, int nLen, LTBOOL bDetail)
{
	szOut[0] = '\0';
	if (!hObj || IsMainWorld(hObj)) { strncpy(szOut, "wall", nLen - 1); szOut[nLen - 1] = '\0'; return 0; }

	static HCLASS hHitBox = g_pLTServer->GetClass("CCharacterHitBox");
	static HCLASS hPlayer = g_pLTServer->GetClass("CPlayerObj");
	HCLASS hClass = g_pLTServer->GetObjectClass(hObj);
	HOBJECT hWho = LTNULL;
	if (hHitBox && g_pLTServer->IsKindOf(hClass, hHitBox))
	{
		CCharacterHitBox* pBox = (CCharacterHitBox*)g_pLTServer->HandleToObject(hObj);
		hWho = pBox ? pBox->GetModelObject() : LTNULL;
	}
	else if (hPlayer && g_pLTServer->IsKindOf(hClass, hPlayer))
	{
		hWho = hObj;
	}
	if (hWho)
	{
		if (hPlayer && g_pLTServer->IsKindOf(g_pLTServer->GetObjectClass(hWho), hPlayer))
		{
			CPlayerObj* pWho = (CPlayerObj*)g_pLTServer->HandleToObject(hWho);
			_snprintf(szOut, nLen - 1, "%s", pWho ? pWho->GetNetName() : "a player");
		}
		else
		{
			char szWho[32];
			g_pLTServer->GetClassName(g_pLTServer->GetObjectClass(hWho), szWho, sizeof(szWho));
			_snprintf(szOut, nLen - 1, "a %s", szWho);		// e.g. a dead body
		}
		szOut[nLen - 1] = '\0';
		return 1;
	}

	char szClass[32];
	g_pLTServer->GetClassName(hClass, szClass, sizeof(szClass));
	if (bDetail)
	{
		LTVector vPos, vDims;
		g_pLTServer->GetObjectPos(hObj, &vPos);
		g_pLTServer->GetObjectDims(hObj, &vDims);
		_snprintf(szOut, nLen - 1, "a %s (type %u at %.0f %.0f %.0f, size %.0f %.0f %.0f, flags %x)", szClass,
			(unsigned)g_pLTServer->GetObjectType(hObj), vPos.x, vPos.y, vPos.z, vDims.x * 2.0f, vDims.y * 2.0f, vDims.z * 2.0f,
			(unsigned)g_pLTServer->GetObjectFlags(hObj));
	}
	else _snprintf(szOut, nLen - 1, "a %s", szClass);
	szOut[nLen - 1] = '\0';
	return 2;
}

// bPinned: pushing but not moving at all.  Otherwise moving, but not getting
// any closer to the next step (sliding along a wall, circling, shoved back).
void CBotPlayer::OnStuck(LTFLOAT fTime, LTBOOL& bJump, LTBOOL bPinned)
{
	// Can't get to this item (on a shelf, behind glass...): forget it a while
	if (m_bGoalIsItem && m_nReplans >= 1)
	{
		m_avVisited[m_nNextVisited]		= m_vGoal;
		m_afVisitedTime[m_nNextVisited] = fTime;
		m_nNextVisited = (m_nNextVisited + 1) % kVisitedItems;
		m_bGoalIsScuba = LTFALSE;
	}

	// Stay off the waypoint link that trapped us (this bot, for a while).
	// Not for swimming: slow, but it's not the link's fault.
	if (!m_bInWater && m_nPathIndex > 0 && m_nPathIndex < (int)m_Path.size())
	{
		m_AvoidLinks.insert(CBotNav::LinkKey(m_Path[m_nPathIndex - 1], m_Path[m_nPathIndex]));
	}

	char szOn[160] = "";
	{
		LTVector vHerePos;
		g_pLTServer->GetObjectPos(m_hObject, &vHerePos);
		bJump = !m_bHasMoveTarget || m_bInWater || IsHopSafe(Horiz(m_vMoveTarget - vHerePos));

		// Standing on an object the map scan doesn't know (a dropped
		// motorcycle, a prop): hop off it, whatever's ahead
		CollisionInfo info;
		g_pLTServer->GetStandingOn(m_hObject, &info);
		if (info.m_hObject && !IsMainWorld(info.m_hObject))
		{
			char szWhat[128];
			DescribeObject(info.m_hObject, szWhat, sizeof(szWhat), LTTRUE);
			_snprintf(szOn, sizeof(szOn) - 1, ", standing on %s", szWhat);
			szOn[sizeof(szOn) - 1] = '\0';
			bJump = LTTRUE;
		}
	}
	m_nReplans++;
	{
		LTVector vHere;
		g_pLTServer->GetObjectPos(m_hObject, &vHere);

		// What's in the way of the next step, at foot / knee / head height
		char szBlock[200] = "";
		LTBOOL bByCharacter = LTFALSE;
		if (m_bHasMoveTarget)
		{
			LTVector vDir = Horiz(m_vMoveTarget - vHere);
			IntersectQuery IQuery;
			IntersectInfo IInfo;
			IQuery.m_Flags	  = INTERSECT_OBJECTS | IGNORE_NONSOLID;
			IQuery.m_FilterFn = LTNULL;
			const char* aszLevel[3] = { "feet", "knee", "head" };
			LTFLOAT afY[3] = { -45.0f, -20.0f, 40.0f };
			for (int k = 0; k < 3; k++)
			{
				IQuery.m_From = vHere + LTVector(0.0f, afY[k], 0.0f);
				IQuery.m_To	  = IQuery.m_From + vDir * 60.0f;
				if (!g_pLTServer->IntersectSegment(&IQuery, &IInfo)) continue;
				if (IInfo.m_hObject == m_hObject || IInfo.m_hObject == m_hHitBox) continue;
				char szClass[128];
				int nKind = DescribeObject(IInfo.m_hObject, szClass, sizeof(szClass), k == 0);
				if (nKind == 1) bByCharacter = LTTRUE;
				int nUsed = (int)strlen(szBlock);
				_snprintf(szBlock + nUsed, sizeof(szBlock) - 1 - nUsed, "%s%s at %s", nUsed ? ", " : ", bumping into ", szClass, aszLevel[k]);
				szBlock[sizeof(szBlock)-1] = '\0';
			}
		}
		// Pinned: tell the team.  The spot (and the next step) cost extra in
		// everybody's route planning for a minute, so the others go round
		// instead of piling into the same body / crowd / unscanned rock.
		CBotWorld& world = g_BotMgr.GetWorld();
		LTBOOL bMarked = LTFALSE;
		if (bPinned && world.IsReady())
		{
			int anSpans[2] = { world.FindSpan(vHere), (m_nSpanIdx >= 0 && m_nSpanIdx < (int)m_SpanPath.size()) ? m_SpanPath[m_nSpanIdx] : -1 };
			for (int i = 0; i < 2; i++)
			{
				if (anSpans[i] < 0) continue;
				world.MarkHazard(anSpans[i], fTime + 60.0f);
				g_BotMgr.GetNav().MarkHazard(world.GetSpan(anSpans[i]).nNode, fTime + 60.0f);
				bMarked = LTTRUE;
			}
		}

		// Somebody in the way (a teammate in a narrow tunnel, a body), or
		// pinned under water (resting on a ledge in a shaft, head on the lip
		// of the tunnel mouth): back off a little first - then sink / rise to
		// the next step's depth and go in (see ApplyMovement)
		if ((bByCharacter || (bPinned && (m_bInWater || m_bHeadInWater))) && m_bHasMoveTarget)
		{
			m_vUnstickDir	= Horiz(vHere - m_vMoveTarget);
			m_fUnstickUntil	= fTime + 1.0f;
		}

		Act("Stuck at (%.0f %.0f %.0f)%s, %s (next step (%.0f %.0f %.0f), %.0fu)%s%s%s - %s.", vHere.x, vHere.y, vHere.z,
			(m_bInWater || m_bHeadInWater) ? " in the water" : "",
			bPinned ? "can't move" : "not getting closer",
			m_vMoveTarget.x, m_vMoveTarget.y, m_vMoveTarget.z, m_bHasMoveTarget ? vHere.Dist(m_vMoveTarget) : 0.0f, szBlock, szOn,
			bMarked ? " (marked for the team)" : "",
			m_nReplans <= 2 ? "trying another way" : "giving up on this");
	}

	if (m_bInWater || m_bHeadInWater) m_fShortLookUntil = fTime + 5.0f;

	// Stuck while swimming (a shaft we can't get down, a pool we can't climb
	// out of): drop the goal and swim to the nearest dry floor
	if ((m_bInWater || m_bHeadInWater) && m_nReplans >= 2)
	{
		LTVector vPos;
		g_pLTServer->GetObjectPos(m_hObject, &vPos);
		LTVector vDry;
		if (FindDryLand(vPos, vDry) && PathTo(vDry))
		{
			SetGoal(vDry, LTFALSE, fTime + 15.0f);
			m_nReplans = 3;		// (SetGoal reset it; one more stuck and we give up)
			m_szWhy = "out-of-water";
			return;
		}
	}

	if (m_bHasGoal && m_nReplans <= (m_bGoalIsItem ? 1 : 2))
	{
		LTVector vGoal = m_vGoal;
		if (PathTo(vGoal)) return;
	}

	m_bHasGoal = LTFALSE;
	m_bCamping = LTFALSE;
	ClearPath();
}

void CBotPlayer::ApplyMovement(LTFLOAT fTime, const LTVector& vDir, LTBOOL bJump)
{
	LTVector vVel;
	g_pLTServer->GetVelocity(m_hObject, &vVel);

	LTFLOAT fSpeed = GetMoveSpeed();
	if (IsRidingVehicle()) fSpeed *= 2.2f;
	LTVector vWant = vDir * fSpeed;

	// Ladder: climb towards the target's height (no gravity while on it)
	if (m_bOnLadder && m_bHasMoveTarget)
	{
		LTVector vPos;
		g_pLTServer->GetObjectPos(m_hObject, &vPos);
		LTFLOAT fDY = m_vMoveTarget.y - vPos.y;
		if ((LTFLOAT)fabs(fDY) > 25.0f)
		{
			LTFLOAT fClimb = GetLadderVelocity() > 50.0f ? GetLadderVelocity() : 200.0f;
			vVel = vDir * 60.0f;
			vVel.y = (fDY > 0.0f) ? fClimb : -fClimb;
			g_pLTServer->SetVelocity(m_hObject, &vVel);
			return;
		}
	}

	LTVector vMyPos;
	g_pLTServer->GetObjectPos(m_hObject, &vMyPos);
	LTBOOL bTargetAbove = m_bHasMoveTarget && (m_vMoveTarget.y > vMyPos.y + 40.0f);

	if (m_bHeadInWater || (m_bInWater && (!IsOnGround() || bTargetAbove)))
	{
		// Swim towards the target in 3D; head for the surface for air or
		// when there's nothing to swim to.
		LTVector vPos;
		g_pLTServer->GetObjectPos(m_hObject, &vPos);

		// Swim straight at the target in 3D (down a shaft is straight down,
		// not into its wall); head for the surface for air or when there's
		// nothing to swim to
		LTVector vSwim = vDir + LTVector(0.0f, 0.6f, 0.0f);
		if (m_bHasMoveTarget && !(m_bHeadInWater && GetDestructible()->GetHitPoints() < 30.0f))
		{
			LTVector vTo = m_vMoveTarget - vPos;
			LTFLOAT fH = (LTFLOAT)sqrt(vTo.x*vTo.x + vTo.z*vTo.z);
			if (vDir.MagSqr() < 0.01f) vTo.x = vTo.z = 0.0f;		// (no horizontal wish: just rise/sink)
			else if (fH > 1.0f) { vTo.x = vDir.x * fH; vTo.z = vDir.z * fH; }	// keep the steering's direction
			// Just got stuck under water (head on the lip of a tunnel mouth):
			// get to the next step's depth first, then go in
			if (fTime < m_fShortLookUntil && fTime >= m_fUnstickUntil && (LTFLOAT)fabs(vTo.y) > 16.0f) vTo.x = vTo.z = 0.0f;
			vSwim = vTo;
		}
		if (vSwim.MagSqr() > 1.0f) vSwim.Norm();
		LTFLOAT fSwimSpeed = (m_fSwimVel > 100.0f) ? m_fSwimVel : 250.0f;
		vVel = vSwim * fSwimSpeed;

		// Climb out at the edge (at the surface: kick up and out)
		if (bJump && fTime > m_fNextJump && (!m_bHeadInWater || (m_bHasMoveTarget && m_vMoveTarget.y > vPos.y && m_vMoveTarget.y - vPos.y < 140.0f)))
		{
			vVel.y = GetJumpVel();
			m_fNextJump = fTime + 1.0f;
		}

		g_pLTServer->SetVelocity(m_hObject, &vVel);
		return;
	}

	if (IsOnGround())
	{
		vVel.x = vWant.x;
		vVel.z = vWant.z;

		if (bJump && fTime > m_fNextJump)
		{
			vVel.y = GetJumpVel();
			m_fNextJump = fTime + 0.9f;
			SetMotionStatus(MS_JUMPED);
			SetOnGround(LTFALSE);
		}
	}
	else
	{
		// A little air control...
		vVel.x += (vWant.x - vVel.x) * 0.05f;
		vVel.z += (vWant.z - vVel.z) * 0.05f;
	}

	g_pLTServer->SetVelocity(m_hObject, &vVel);
}

LTBOOL CBotPlayer::IsTeammateInLineOfFire(const LTVector& vFrom, const LTVector& vTo)
{
	LTVector vDir = vTo - vFrom;
	LTFLOAT fLen = vDir.Mag();
	if (fLen < 1.0f) return LTFALSE;
	vDir /= fLen;

	const std::vector<CPlayerObj*>& players = g_BotMgr.GetPlayers();
	for (size_t i = 0; i < players.size(); i++)
	{
		CPlayerObj* pPlayer = players[i];
		if (pPlayer == this || pPlayer->GetState() != PS_ALIVE || pPlayer->GetTeamID() != GetTeamID()) continue;

		LTVector vPos;
		g_pLTServer->GetObjectPos(pPlayer->m_hObject, &vPos);

		LTFLOAT t = (vPos - vFrom).Dot(vDir);
		if (t < 0.0f || t > fLen) continue;

		LTVector vClosest = vFrom + vDir * t;
		if (vClosest.DistSqr(vPos) < 45.0f*45.0f) return LTTRUE;
	}

	return LTFALSE;
}

// ----------------------------------------------------------------------- //
//	Zipcords (the client normally does this for players)
// ----------------------------------------------------------------------- //

void CBotPlayer::StartZip(int nLink, LTFLOAT fTime)
{
	CBotWorld& world = g_BotMgr.GetWorld();
	if (nLink < 0 || nLink >= world.GetLinkCount()) return;
	const BotWorldLink& link = world.GetLinks()[nLink];

	LTVector vPos;
	g_pLTServer->GetObjectPos(m_hObject, &vPos);

	m_nZipLink	= nLink;
	m_nZipSpanIdx = m_nSpanIdx;		// (the zipcord step: cell m_nSpanIdx is its landing)
	m_bZipping	= LTTRUE;
	m_fZipLineup	= -1.0f;
	m_vZipCheckPos	= vPos;
	m_fZipCheckTime	= fTime;
	g_anBotZipStats[0]++;
	Act("Zipcord up from (%.0f %.0f %.0f) to the hook at (%.0f %.0f %.0f).", vPos.x, vPos.y, vPos.z, link.vAux.x, link.vAux.y, link.vAux.z);
	m_vZipHook	= link.vAux;
	LTFLOAT fSpeed = GetZipCordVelocity() > 200.0f ? GetZipCordVelocity() : 600.0f;
	m_fZipUntil	= fTime + vPos.Dist(m_vZipHook) / fSpeed + 2.0f;

	g_pLTServer->SetObjectFlags(m_hObject, g_pLTServer->GetObjectFlags(m_hObject) & ~FLAG_GRAVITY);

	// Show the cord on everybody's screen
	LTVector vHook = m_vZipHook;
	HMESSAGEWRITE hMessage = g_pLTServer->StartMessage(LTNULL, MID_SFX_MESSAGE);
	g_pLTServer->WriteToMessageByte(hMessage, SFX_CHARACTER_ID);
	g_pLTServer->WriteToMessageObject(hMessage, m_hObject);
	g_pLTServer->WriteToMessageByte(hMessage, CFX_ZIPCORD_MSG);
	g_pLTServer->WriteToMessageByte(hMessage, ZC_ON);
	g_pLTServer->WriteToMessageVector(hMessage, &vHook);
	g_pLTServer->EndMessage(hMessage);
}

void CBotPlayer::StopZip()
{
	m_bZipping = LTFALSE;
	g_pLTServer->SetObjectFlags(m_hObject, g_pLTServer->GetObjectFlags(m_hObject) | FLAG_GRAVITY);

	HMESSAGEWRITE hMessage = g_pLTServer->StartMessage(LTNULL, MID_SFX_MESSAGE);
	g_pLTServer->WriteToMessageByte(hMessage, SFX_CHARACTER_ID);
	g_pLTServer->WriteToMessageObject(hMessage, m_hObject);
	g_pLTServer->WriteToMessageByte(hMessage, CFX_ZIPCORD_MSG);
	g_pLTServer->WriteToMessageByte(hMessage, ZC_OFF);
	g_pLTServer->EndMessage(hMessage);
}

// Riding up the cord, like the client does it for players: straight at the
// hook, and let go once we're there.  Then step off onto the ledge we came
// for (UpdateZipLanding).
LTBOOL CBotPlayer::UpdateZip(LTFLOAT fTime)
{
	LTVector vPos;
	g_pLTServer->GetObjectPos(m_hObject, &vPos);

	LTVector vTo = m_vZipHook - vPos;
	LTFLOAT fDist = vTo.Mag();

	LTVector vDims;
	g_pLTServer->GetObjectDims(m_hObject, &vDims);

	if (fDist < vDims.Mag() + 16.0f || fTime > m_fZipUntil)
	{
		StopZip();

		CBotWorld& world = g_BotMgr.GetWorld();
		if (m_nZipLink >= 0 && m_nZipLink < world.GetLinkCount())
		{
			m_bZipLanding	= LTTRUE;
			m_vZipLand		= world.GetSpanPos(world.GetLinks()[m_nZipLink].nTo);
			// Aim a little past it, so we end up well onto the ledge
			LTVector vIn = Horiz(m_vZipLand - vPos);
			m_vZipLand += vIn * 24.0f;
			m_fZipLandStart	= fTime;
		}

		// Let go with a little kick up, to clear the lip
		LTVector vVel(0.0f, GetJumpVel() * 0.5f, 0.0f);
		g_pLTServer->SetVelocity(m_hObject, &vVel);
		return LTFALSE;
	}

	LTFLOAT fSpeed = GetZipCordVelocity();
	if (fSpeed < 200.0f) fSpeed = 600.0f;

	// Hung up on something (a door, a frame)?  Let go and find another way
	// instead of hanging there.
	if (fTime - m_fZipCheckTime > 0.5f)
	{
		LTFLOAT fMoved = vPos.Dist(m_vZipCheckPos);
		m_vZipCheckPos	= vPos;
		m_fZipCheckTime	= fTime;
		if (fMoved < fSpeed * 0.5f * 0.25f)
		{
			StopZip();
			CBotWorld& world = g_BotMgr.GetWorld();
			if (m_nZipLink >= 0 && m_nZipLink < world.GetLinkCount())
			{
				world.GetLinkRW(m_nZipLink).nFails++;
				g_anBotZipStats[2]++;
			}
			Act("Zipcord ride blocked at (%.0f %.0f %.0f) - letting go, finding another way.", vPos.x, vPos.y, vPos.z);
			LTVector vGoal = m_vGoal;
			if (m_bHasGoal && !PathTo(vGoal)) m_bHasGoal = LTFALSE;
			return LTFALSE;
		}
	}

	LTVector vVel = vTo / fDist * fSpeed;
	g_pLTServer->SetVelocity(m_hObject, &vVel);
	SetOnGround(LTFALSE);
	return LTTRUE;
}

// Just let go of a zipcord: move over onto the landing (air control, like a
// player holding forward), and check we really made it up there.
LTBOOL CBotPlayer::UpdateZipLanding(LTFLOAT fTime)
{
	if (!m_bZipLanding) return LTFALSE;

	LTVector vPos;
	g_pLTServer->GetObjectPos(m_hObject, &vPos);

	LTBOOL bLanded = IsOnGround() && fTime - m_fZipLandStart > 0.15f;
	if (bLanded || fTime - m_fZipLandStart > 2.5f)
	{
		m_bZipLanding = LTFALSE;

		// Fell back down instead: that cord's landing doesn't work for us
		CBotWorld& world = g_BotMgr.GetWorld();
		if (vPos.y < m_vZipLand.y - 70.0f && m_nZipLink >= 0 && m_nZipLink < world.GetLinkCount())
		{
			world.GetLinkRW(m_nZipLink).nFails++;
			g_anBotZipStats[2]++;
			Act("Missed the zipcord landing and fell back down - finding another way.");
			LTVector vGoal = m_vGoal;
			if (m_bHasGoal && !PathTo(vGoal)) m_bHasGoal = LTFALSE;
		}
		else if (m_bHasGoal)
		{
			// Up on the high ground.  Worth holding if it overlooks the fight.
			g_anBotZipStats[1]++;
			Act("Made it up the zipcord to (%.0f %.0f %.0f).", vPos.x, vPos.y, vPos.z);

			// Carry on along the path from the landing.  (We rarely come down
			// on the exact landing cell; the path follower then didn't find
			// us on the path, fell back on the cord step - "walk to the
			// take-off" - and walked straight back off the edge.)
			if (m_nZipSpanIdx > 0 && m_nZipSpanIdx < (int)m_SpanPath.size() && m_nZipLink >= 0 &&
				m_SpanPath[m_nZipSpanIdx] == world.GetLinks()[m_nZipLink].nTo)
			{
				m_nSpanIdx		= m_nZipSpanIdx + 1;
				m_nLookIdx		= m_nSpanIdx;
				m_fNextLook		= 0.0f;
				m_fOffPathSince	= -1.0f;
				if (m_nSpanIdx >= (int)m_SpanPath.size())
				{
					LTVector vGoal = m_vGoal;
					if (!PathTo(vGoal)) m_bHasGoal = LTFALSE;
				}
			}
			OnReachedHighGround(fTime);
			if (m_bPerched) g_anBotZipStats[3]++;
		}
		m_nZipLink = BOTW_INVALID;
		return LTFALSE;
	}

	LTVector vVel;
	g_pLTServer->GetVelocity(m_hObject, &vVel);
	LTVector vD = Horiz(m_vZipLand - vPos);
	LTFLOAT fHoriz = HorizDist(vPos, m_vZipLand);
	LTFLOAT fSpeed = LTMIN(GetMoveSpeed(), fHoriz * 5.0f);
	vVel.x = vD.x * fSpeed;
	vVel.z = vD.z * fSpeed;
	g_pLTServer->SetVelocity(m_hObject, &vVel);
	return LTTRUE;
}

void CBotPlayer::SetMoveCommands(const LTVector& vDir)
{
	if (g_pLTServer->GetTime() - m_fLastFire < 0.3f)
	{
		SetBotCommand(COMMAND_ID_FIRING, LTTRUE);
	}

	// Campers crouch to shoot from their spot
	if (m_bCamping && vDir.MagSqr() < 0.01f && m_bEnemyVisible)
	{
		SetBotCommand(COMMAND_ID_DUCK, LTTRUE);
		return;
	}

	if (vDir.MagSqr() < 0.01f) return;

	SetBotCommand(COMMAND_ID_RUN, LTTRUE);

	if (m_bOnLadder)
	{
		SetBotCommand(COMMAND_ID_FORWARD, LTTRUE);	// Climbing animation
		return;
	}

	// Animation direction is relative to where we're facing...

	LTVector vForward(LTFLOAT(sin(m_fYaw)), 0.0f, LTFLOAT(cos(m_fYaw)));
	LTVector vRight(vForward.z, 0.0f, -vForward.x);

	LTFLOAT fFwd	= vDir.Dot(vForward);
	LTFLOAT fRight	= vDir.Dot(vRight);

	if (fFwd > 0.4f)		SetBotCommand(COMMAND_ID_FORWARD, LTTRUE);
	else if (fFwd < -0.4f)	SetBotCommand(COMMAND_ID_REVERSE, LTTRUE);

	if (fRight > 0.4f)		SetBotCommand(COMMAND_ID_STRAFE_RIGHT, LTTRUE);
	else if (fRight < -0.4f) SetBotCommand(COMMAND_ID_STRAFE_LEFT, LTTRUE);
}
