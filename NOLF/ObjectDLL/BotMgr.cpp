// ----------------------------------------------------------------------- //
//
// MODULE  : BotMgr.cpp
//
// PURPOSE : Adds/removes bots, owns the waypoint graph, and gives bots a
//			 per-frame view of the players and pickups in the world
//
//	Commands: the host types them in chat with a "!" in front (e.g. "!addbot"),
//	or puts them in Bots\bots.cfg (run once when hosting starts).
//
//		addbot [name] [skill 1-5] [team 1|2]
//		removebot [name|all]
//		bot_skill <1-5>			default skill for new bots (and all current ones)
//		bot_minplayers <n>		keep at least n players (humans + bots)
//		bot_record <0|1>		drop waypoints where human players walk
//		bot_save				save the waypoints for this world
//		bot_autogen				rebuild waypoints from spawn points/items
//		bot_clearnav			delete all waypoints (in memory)
//		bot_info				show bot/waypoint status
//
// ----------------------------------------------------------------------- //

#include "stdafx.h"
#include "BotMgr.h"
#include "BotPlayer.h"
#include "GameServerShell.h"
#include "ServerUtilities.h"
#include "PickupItem.h"
#include "AmmoBox.h"
#include "Attachments.h"
#include "Weapon.h"
#include "WeaponItems.h"
#include "WeaponMgr.h"
#include "MsgIDs.h"
#include "TeamMgr.h"
#include "Intelligence.h"
#include "GameStartPoint.h"
#include "GearItems.h"
#include "PlayerVehicle.h"
#include "Body.h"
#include <algorithm>
#include <string>
#include <map>
#include "CVarTrack.h"

extern CVarTrack g_RespawnScaleTrack;
#include <stdarg.h>
#include <ctype.h>
#include <stdio.h>
#include <direct.h>
#include <io.h>
#include <time.h>

CBotMgr g_BotMgr;

static const char* s_aBotNames[] =
{
	"Archer", "Magnus", "Inga", "Volkov", "Dmitrij", "Baroness", "Tom",
	"Bruno", "Santa", "Isako", "Smith", "Jones", "Cate", "Armstrong",
	"Lindsay", "Goldie", "Fritz", "Mister Mime", "Dr. Schenker", "Pierre",
	"Candy", "Hamish", "Bubba", "Ivan",
};
static const int s_nBotNames = sizeof(s_aBotNames) / sizeof(s_aBotNames[0]);

static LTFLOAT SkillFromLevel(int nLevel)
{
	if (nLevel < 1) nLevel = 1;
	if (nLevel > 5) nLevel = 5;
	return (LTFLOAT)(nLevel - 1) / 4.0f;
}

static void ApplySelfPlaySpeed();
static int s_nSpeedHooks = 0;		// self-play clock hooks installed (for the log)

// ----------------------------------------------------------------------- //
//	Genome: the trainable fighting parameters (see BotGenome.h)
// ----------------------------------------------------------------------- //

const BotGeneInfo g_aGeneInfo[kNumGenes] =
{
	//  name			default	min		max
	{ "react",			1.0f,	0.6f,	1.6f },
	{ "aimerr",			1.0f,	0.6f,	1.6f },
	{ "aimmove",		1.0f,	0.5f,	1.6f },
	{ "turn",			1.0f,	0.7f,	2.0f },
	{ "strafe",			1.0f,	0.4f,	2.5f },
	{ "jump",			1.0f,	0.0f,	2.0f },
	{ "range",			1.0f,	0.5f,	2.0f },
	{ "retreat",		1.0f,	0.3f,	2.0f },
	{ "chase",			1.0f,	0.2f,	2.5f },
	{ "aggro",			1.0f,	0.3f,	2.0f },
	{ "burst",			1.0f,	0.5f,	2.5f },
	{ "head",			1.0f,	0.0f,	2.0f },
	{ "effrange",		1.0f,	0.6f,	2.5f },
	{ "greed",			1.0f,	0.3f,	2.5f },
	{ "heal",			1.0f,	0.3f,	3.0f },
	{ "role",			0.92f,	0.3f,	1.0f },
	{ "cover",			1.0f,	0.0f,	1.0f },
	{ "fov",			1.0f,	0.6f,	1.4f },
	{ "dodge",			1.0f,	0.0f,	2.5f },
	{ "lead",			1.0f,	0.0f,	1.5f },
	{ "waves",			1.0f,	0.0f,	1.0f },
	{ "danger",			1.0f,	0.0f,	1.0f },
	{ "callouts",		1.0f,	0.0f,	1.0f },
	{ "discipline",		1.0f,	0.0f,	1.0f },
	{ "focus",			1.0f,	0.0f,	1.0f },
	{ "recall",			1.0f,	0.0f,	1.0f },
};

void BotGenome::SetDefaults()
{
	for (int i = 0; i < kNumGenes; i++) v[i] = g_aGeneInfo[i].fDefault;
	SAFE_STRCPY(szId, "default");
}

bool BotGenome::Load(const char* szFile)
{
	FILE* fp = fopen(szFile, "rt");
	if (!fp) return false;

	SetDefaults();
	const char* pSlash = strrchr(szFile, '\\');
	SAFE_STRCPY(szId, pSlash ? pSlash + 1 : szFile);

	char szLine[128], szName[64];
	float f;
	while (fgets(szLine, sizeof(szLine), fp))
	{
		if (sscanf(szLine, "%63s %f", szName, &f) != 2) continue;
		for (int i = 0; i < kNumGenes; i++)
		{
			if (_stricmp(szName, g_aGeneInfo[i].szName) != 0) continue;
			v[i] = LTCLAMP(f, g_aGeneInfo[i].fMin, g_aGeneInfo[i].fMax);
			break;
		}
	}
	fclose(fp);
	return true;
}

CBotMgr::CBotMgr()
{
	m_szNavWorld[0]		= '\0';
	m_fPlayersTime		= -1.0f;
	m_fNextItemRefresh	= 0.0f;
	m_fSpawnBotsTime	= -1.0f;
	m_bWorldLoaded		= LTFALSE;
	m_fDefaultSkill		= SkillFromLevel(3);
	m_nMinPlayers		= 0;
	m_bRecording		= LTFALSE;
	m_bConfigRun		= LTFALSE;
	m_nCampScoreNext	= 0;
	m_bSettingsApplied	= LTFALSE;
	m_fNextJoin			= 0.0f;
	m_bAutoSkill		= LTTRUE;
	m_fSkillOffset		= 0.0f;
	m_fNextAutoSkill	= 0.0f;
	m_fNextChat			= 0.0f;
	m_fNextLog			= 0.0f;
	m_fNextBodyCleanup	= 0.0f;
	m_nBodiesRemoved	= 0;
	m_nIntelTries		= 0;
	m_nModelVariety		= 3;
	m_nSneakNavSize		= -1;
	m_fNextRecall		= 0.0f;
	m_bLearn			= LTTRUE;
	m_anRouteGen[0] = m_anRouteGen[1] = m_anRouteGen[2] = 0;
	m_fpActions		= LTNULL;
	m_bActionsOpened = LTFALSE;
	m_fWorldStart	= 0.0f;
	m_fNextLearn		= 0.0f;
	m_fNextNavSave		= 0.0f;
	m_fNextDangerDecay	= 0.0f;
	m_nRound			= 0;
	m_fRoundStart		= 0.0f;

	// Training servers that don't save the map shouldn't spend CPU learning it
	const char* szNoSave = getenv("NOLFBOT_NOSAVE");
	if (szNoSave && szNoSave[0] == '1') m_bLearn = LTFALSE;
	m_bWaitingAssign	= LTFALSE;
	m_fNextAssignCheck	= 0.0f;
	m_nFrames			= 0;
	m_fFramesSince		= 0.0f;
	for (int t = 0; t < 3; t++)
	{
		m_aGenome[t].SetDefaults();
		m_anRoundKills[t] = 0; m_anRoundPhotos[t] = 0;
		m_afAlertTime[t] = -100.0f; m_anRoleFail[t] = 0;
		memset(&m_aPlan[t], 0, sizeof(m_aPlan[t]));
		m_anNextSighting[t] = 0;
		for (int s = 0; s < kSightings; s++) m_afSightingTime[t][s] = -100.0f;
	}

	// Feature switches (self-play A/B tests only; all on otherwise)
	static const struct { const char* szName; int nBit; } s_aFeat[] =
	{
		{ "waves", eFeatWaves }, { "danger", eFeatDanger }, { "callouts", eFeatCallouts },
		{ "discipline", eFeatDiscipline }, { "focus", eFeatFocus }, { "recall", eFeatRecall },
	};
	for (int t = 0; t < 3; t++)
	{
		m_anFeatures[t] = eFeatAll;
		const char* szOff = getenv(t == TEAM_2 ? "NOLFBOT_OFF_T2" : "NOLFBOT_OFF_T1");
		if (!szOff) continue;
		for (size_t f = 0; f < sizeof(s_aFeat)/sizeof(s_aFeat[0]); f++)
		{
			if (strstr(szOff, s_aFeat[f].szName)) m_anFeatures[t] &= ~s_aFeat[f].nBit;
		}
	}
}

LTBOOL CBotMgr::HasFeature(uint32 dwTeam, int nFeature) const
{
	if (dwTeam > 2) dwTeam = 1;
	if (dwTeam == 0) dwTeam = 1;
	if (!(m_anFeatures[dwTeam] & nFeature)) return LTFALSE;

	// The trained genome can switch team behaviours off too
	const BotGenome& g = GetGenome(dwTeam);
	switch (nFeature)
	{
		case eFeatWaves:		return g.On(eGeneWaves);
		case eFeatDanger:		return g.On(eGeneDanger);
		case eFeatCallouts:		return g.On(eGeneCallouts);
		case eFeatDiscipline:	return g.On(eGeneDiscipline);
		case eFeatFocus:		return g.On(eGeneFocus);
		case eFeatRecall:		return g.On(eGeneRecall);
	}
	return LTTRUE;
}

// ----------------------------------------------------------------------- //
//	Self-play training rounds (only with NOLFBOT_TRAIN_DIR set).  The server
//	plays on and on; every round it takes the two genomes the trainer
//	assigned (TRAIN_DIR\assign.txt), counts what each team achieves, and
//	appends a line to TRAIN_DIR\results.txt.  hydro_evolve.py reads those.
// ----------------------------------------------------------------------- //

static const char* GetTrainDir()
{
	const char* sz = getenv("NOLFBOT_TRAIN_DIR");
	return (sz && sz[0]) ? sz : LTNULL;
}

// Loads the genomes if assign.txt is for round nRound; returns whether it was.
LTBOOL CBotMgr::LoadAssignment(int nRound)
{
	const char* szDir = GetTrainDir();
	if (!szDir) return LTFALSE;

	char szFile[MAX_PATH];
	sprintf(szFile, "%s\\assign.txt", szDir);
	FILE* fp = fopen(szFile, "rt");
	if (!fp) return LTFALSE;

	char szLine[MAX_PATH], szKey[16], szVal[MAX_PATH], szT1[MAX_PATH] = "", szT2[MAX_PATH] = "";
	int nFor = -1;
	while (fgets(szLine, sizeof(szLine), fp))
	{
		if (sscanf(szLine, "%15s %259s", szKey, szVal) != 2) continue;
		if (_stricmp(szKey, "round") == 0)	{ nFor = atoi(szVal); }
		else if (_stricmp(szKey, "t1") == 0) { SAFE_STRCPY(szT1, szVal); }
		else if (_stricmp(szKey, "t2") == 0) { SAFE_STRCPY(szT2, szVal); }
	}
	fclose(fp);

	if (nFor != nRound) return LTFALSE;
	if (szT1[0]) m_aGenome[TEAM_1].Load(szT1);
	if (szT2[0]) m_aGenome[TEAM_2].Load(szT2);
	return LTTRUE;
}

void CBotMgr::UpdateTraining(LTFLOAT fTime)
{
	const char* szDir = GetTrainDir();
	if (!szDir) return;

	const char* szRound = getenv("NOLFBOT_ROUND");
	LTFLOAT fRoundLen = szRound ? (LTFLOAT)atof(szRound) : 120.0f;
	if (fRoundLen < 20.0f) fRoundLen = 20.0f;

	if (m_nRound == 0)
	{
		// The first round waits until the bots are in
		if (m_Bots.size() < 2) return;
		m_nRound = 1;
		m_bWaitingAssign = LTTRUE;
		m_fRoundStart = fTime;
	}

	// Between rounds: wait for the trainer's assignment for this round (it
	// answers within a second or two; go on anyway after 15 game seconds)
	if (m_bWaitingAssign)
	{
		if (fTime < m_fNextAssignCheck) return;
		m_fNextAssignCheck = fTime + 0.5f;
		if (!LoadAssignment(m_nRound) && fTime - m_fRoundStart < 15.0f) return;

		m_bWaitingAssign = LTFALSE;
		m_fRoundStart = fTime;
		for (int t = 0; t < 3; t++) { m_anRoundKills[t] = 0; m_anRoundPhotos[t] = 0; }
		return;
	}

	if (fTime - m_fRoundStart < fRoundLen) return;

	char szFile[MAX_PATH];
	sprintf(szFile, "%s\\results.txt", szDir);
	FILE* fp = fopen(szFile, "at");
	if (fp)
	{
		fprintf(fp, "R %d t1 %s t2 %s kills %d %d photos %d %d secs %.0f\n", m_nRound,
			m_aGenome[TEAM_1].szId, m_aGenome[TEAM_2].szId,
			m_anRoundKills[TEAM_1], m_anRoundKills[TEAM_2], m_anRoundPhotos[TEAM_1], m_anRoundPhotos[TEAM_2],
			fTime - m_fRoundStart);
		fclose(fp);
	}

	m_nRound++;
	m_bWaitingAssign = LTTRUE;
	m_fRoundStart	 = fTime;
}

void CBotMgr::ReportSighting(uint32 dwTeam, const LTVector& vEnemyPos)
{
	if (dwTeam != TEAM_1 && dwTeam != TEAM_2) return;
	LTFLOAT fTime = g_pLTServer->GetTime();

	// Same enemy spot reported again: just refresh it
	for (int s = 0; s < kSightings; s++)
	{
		if (fTime - m_afSightingTime[dwTeam][s] < 3.0f && m_avSighting[dwTeam][s].DistSqr(vEnemyPos) < 200.0f*200.0f)
		{
			m_avSighting[dwTeam][s]	   = vEnemyPos;
			m_afSightingTime[dwTeam][s] = fTime;
			return;
		}
	}

	int& n = m_anNextSighting[dwTeam];
	m_avSighting[dwTeam][n]		= vEnemyPos;
	m_afSightingTime[dwTeam][n] = fTime;
	n = (n + 1) % kSightings;
}

LTBOOL CBotMgr::GetSighting(uint32 dwTeam, const LTVector& vNear, LTFLOAT fMaxDist, LTFLOAT fMaxAge, LTVector& vPos)
{
	if (dwTeam != TEAM_1 && dwTeam != TEAM_2) return LTFALSE;
	LTFLOAT fTime = g_pLTServer->GetTime();

	LTFLOAT fBest = fMaxDist * fMaxDist;
	LTBOOL bFound = LTFALSE;
	for (int s = 0; s < kSightings; s++)
	{
		if (fTime - m_afSightingTime[dwTeam][s] > fMaxAge) continue;
		LTFLOAT d = m_avSighting[dwTeam][s].DistSqr(vNear);
		if (d < fBest) { fBest = d; vPos = m_avSighting[dwTeam][s]; bFound = LTTRUE; }
	}
	return bFound;
}

// ----------------------------------------------------------------------- //
//	Server settings ("set RunSpeed 1.5" in bots.cfg or chat).  Applied after
//	the host menu options, so they win, and again on every world.
// ----------------------------------------------------------------------- //

void CBotMgr::SetServerVar(const char* szVar, const char* szValue)
{
	std::string sEntry = std::string(szVar) + " " + szValue;

	for (size_t i = 0; i < m_Settings.size(); i++)
	{
		if (_strnicmp(m_Settings[i].c_str(), szVar, strlen(szVar)) == 0 && m_Settings[i][strlen(szVar)] == ' ')
		{
			m_Settings.erase(m_Settings.begin() + i);
			break;
		}
	}
	m_Settings.push_back(sEntry);

	g_pLTServer->RunGameConString((char*)sEntry.c_str());
	g_pLTServer->CPrint("Bots: set %s", sEntry.c_str());

	// Clients get run speed / respawn scale through the game data
	HCLIENT hClient = LTNULL;
	while ((hClient = g_pLTServer->GetNextClient(hClient)) != LTNULL)
	{
		g_pGameServerShell->SendGameDataToClient(hClient);
	}
}

void CBotMgr::ApplySettings()
{
	std::vector<std::string> settings = m_Settings;
	for (size_t i = 0; i < settings.size(); i++)
	{
		g_pLTServer->RunGameConString((char*)settings[i].c_str());
	}

	HCLIENT hClient = LTNULL;
	while ((hClient = g_pLTServer->GetNextClient(hClient)) != LTNULL)
	{
		g_pGameServerShell->SendGameDataToClient(hClient);
	}
}

// What a new install plays with.  Written to Bots\bots.cfg the first time
// someone hosts, so players can find it and change it.
static const char s_szDefaultConfig[] =
	"# NOLF Bots settings, read each time you start hosting a game.\r\n"
	"# Change a value, save, and host again.  Delete this file to get the defaults back.\r\n"
	"# Lines starting with # are ignored.  While hosting you can also type any of\r\n"
	"# these in the chat with a ! in front (!bot_skill 3).  !bots lists the commands.\r\n"
	"\r\n"
	"# --- Game settings (these replace the Host menu's values) ---\r\n"
	"set RunSpeed 1.5              # How fast everyone runs: 1 = original game, 1.5 = 50% faster\r\n"
	"set RespawnScale 3            # Weapons and items come back 3x faster (1 = original game)\r\n"
	"set NetFallDamageScale 0      # No falling damage (the bots drop down ledges freely)\r\n"
	"default_weapon Walther SMG    # What everybody spawns with (or: default_weapon none)\r\n"
	"\r\n"
	"# --- Bots ---\r\n"
	"bot_minplayers 12             # Bots fill the game up to this many players, and leave as people join\r\n"
	"bot_skill 4                   # 1 = easy ... 5 = hardest\r\n"
	"bot_autoskill 1               # Bots get a little easier or harder to keep the score close (0 = off)\r\n"
	"\r\n"
	"# Add bots with a name, skill and style of their own:\r\n"
	"# addbot Volkov 5 rusher\r\n"
	"# addbot Inga 3 camper\r\n"
	"# (styles: rusher camper tactician scavenger teamplayer casual)\r\n";

void CBotMgr::RunConfig()
{
	FILE* fp = fopen("Bots\\bots.cfg", "rt");
	if (!fp)
	{
		_mkdir("Bots");
		FILE* fpNew = fopen("Bots\\bots.cfg", "wb");
		if (fpNew)
		{
			fputs(s_szDefaultConfig, fpNew);
			fclose(fpNew);
			fp = fopen("Bots\\bots.cfg", "rt");
		}
	}

	char szLine[256];
	if (fp)
	{
		while (fgets(szLine, sizeof(szLine), fp)) RunConfigLine(szLine);
		fclose(fp);
		return;
	}

	// Can't write to the game folder: play with the defaults anyway
	for (const char* p = s_szDefaultConfig; *p; )
	{
		size_t nLen = strcspn(p, "\n");
		size_t nCopy = LTMIN(nLen, sizeof(szLine) - 1);
		memcpy(szLine, p, nCopy);
		szLine[nCopy] = '\0';
		RunConfigLine(szLine);
		p += nLen;
		if (*p) p++;
	}
}

void CBotMgr::RunConfigLine(char* szLine)
{
	char* p = szLine;
	while (*p == ' ' || *p == '\t' || *p == '!') p++;
	p[strcspn(p, "\r\n#")] = '\0';	// End of line, or an inline comment
	for (char* e = p + strlen(p); e > p && (e[-1] == ' ' || e[-1] == '\t'); e--) e[-1] = '\0';
	if (!*p || *p == '#' || (p[0] == '/' && p[1] == '/')) return;

	HandleCommand(p);
}

// ----------------------------------------------------------------------- //
//	Messages to the host (shown as chat lines on every client)
// ----------------------------------------------------------------------- //

void CBotMgr::Message(const char* szFormat, ...)
{
	char szMsg[256];
	va_list args;
	va_start(args, szFormat);
	_vsnprintf(szMsg, sizeof(szMsg)-1, szFormat, args);
	va_end(args);
	szMsg[sizeof(szMsg)-1] = '\0';

	g_pLTServer->CPrint("%s", szMsg);

	char szFull[300];
	sprintf(szFull, "BOTS: %s", szMsg);

	HMESSAGEWRITE hMessage = g_pLTServer->StartMessage(LTNULL, MID_PLAYER_MESSAGE);
	g_pLTServer->WriteToMessageString(hMessage, szFull);
	g_pLTServer->EndMessage2(hMessage, MESSAGE_NAGGLE);
}

// ----------------------------------------------------------------------- //
//	World start/end
// ----------------------------------------------------------------------- //

void CBotMgr::PreStartWorld()
{
	// Keep the danger learned on the map we're leaving
	if (m_Nav.GetChanges() > 0 && m_szNavWorld[0] && !IsNavBusy())
	{
		m_Nav.Save(m_szNavWorld);
	}

	// All objects (including our bots) go away with the old world...

	for (size_t i = 0; i < m_Bots.size(); i++)
	{
		m_Bots[i].pBot = LTNULL;
	}

	m_Players.clear();
	m_Items.clear();
	m_fPlayersTime	= -1.0f;
	m_bWorldLoaded	= LTFALSE;
	m_bRecording	= LTFALSE;
}

void CBotMgr::PostStartWorld()
{
	m_bWorldLoaded		= LTTRUE;
	m_szNavWorld[0]		= '\0';		// Load the graph on the next update (world name is set then)
	m_fNextItemRefresh	= 0.0f;
	m_fSpawnBotsTime	= g_pLTServer->GetTime() + 1.0f;
	m_Models.clear();
	m_bSettingsApplied	= LTFALSE;
	m_fNextLog			= 0.0f;
	m_fWorldStart		= g_pLTServer->GetTime();

	// A new action log per map, opened at the first line (the level name
	// isn't known yet here)
	if (m_fpActions) { fclose(m_fpActions); m_fpActions = LTNULL; }
	m_bActionsOpened = LTFALSE;
}

// Each game gets its own file (date, time and map in the name) so earlier
// games are kept: Bots\\games\\<date>-<time>-<map>.actions.txt
static const size_t kKeepGameLogs = 30;

// Only the newest nKeep game logs stay, so the folder doesn't grow forever
static void PruneGameLogs(size_t nKeep)
{
	std::vector<std::string> files;
	struct _finddata_t fd;
	intptr_t hFind = _findfirst("Bots\\games\\*.actions.txt", &fd);
	if (hFind == -1) return;
	do
	{
		if (!(fd.attrib & _A_SUBDIR)) files.push_back(fd.name);
	}
	while (_findnext(hFind, &fd) == 0);
	_findclose(hFind);

	if (files.size() <= nKeep) return;
	std::sort(files.begin(), files.end());	// Names start with the date and time
	for (size_t i = 0; i + nKeep < files.size(); i++)
	{
		remove(("Bots\\games\\" + files[i]).c_str());
	}
}

void CBotMgr::OpenActionLog()
{
	// A new action log per map.  Each game gets its own file (date, time and
	// map in the name) so earlier games are kept: Bots\games\<date>-<time>-<map>.actions.txt
	const char* szLog = getenv("NOLFBOT_LOG");
	char szFile[MAX_PATH];
	_mkdir("Bots");
	if (szLog && szLog[0]) _snprintf(szFile, sizeof(szFile) - 1, "%s.actions.txt", szLog);
	else
	{
		const char* szLevel = g_pGameServerShell ? g_pGameServerShell->GetCurLevel() : LTNULL;
		if (!szLevel || !szLevel[0]) szLevel = m_szNavWorld;
		const char* szMap = "game";
		if (szLevel && szLevel[0])
		{
			szMap = szLevel;
			for (const char* p = szLevel; *p; p++) if (*p == '\\' || *p == '/') szMap = p + 1;
		}
		time_t t = time(LTNULL);
		struct tm* pTm = localtime(&t);
		char szStamp[32] = "00000000-000000";
		if (pTm) strftime(szStamp, sizeof(szStamp), "%Y%m%d-%H%M%S", pTm);
		_mkdir("Bots\\games");
		PruneGameLogs(kKeepGameLogs - 1);
		_snprintf(szFile, sizeof(szFile) - 1, "Bots\\games\\%s-%s.actions.txt", szStamp, szMap);
	}
	szFile[sizeof(szFile) - 1] = '\0';
	m_fpActions = fopen(szFile, (szLog && szLog[0]) ? "at" : "wt");
}

void CBotMgr::LogAction(CPlayerObj* pBot, const char* szText)
{
	if (!m_bActionsOpened) { m_bActionsOpened = LTTRUE; OpenActionLog(); }
	if (!m_fpActions || !pBot) return;
	int nSecs = (int)(g_pLTServer->GetTime() - m_fWorldStart);
	if (nSecs < 0) nSecs = 0;
	fprintf(m_fpActions, "[%d:%02d] %s (team %d): %s\n", nSecs / 60, nSecs % 60, pBot->GetNetName(), (int)pBot->GetTeamID(), szText);
	fflush(m_fpActions);
}

// In multiplayer, dead bodies (with the weapon models etc. they carry) are
// never removed.  With a server full of bots dying all the time they pile
// up until the client can't draw models any more (invisible players and
// weapons), so keep only the recent ones.
void CBotMgr::CleanupBodies(LTFLOAT fTime)
{
	if (fTime < m_fNextBodyCleanup) return;
	m_fNextBodyCleanup = fTime + 2.0f;

	static HCLASS hBody = g_pLTServer->GetClass("Body");
	if (!hBody) return;

	std::vector< std::pair<LTFLOAT, Body*> > bodies;
	for (int nPass = 0; nPass < 2; nPass++)
	{
		HOBJECT hObj = LTNULL;
		while ((hObj = (nPass == 0 ? g_pLTServer->GetNextObject(hObj) : g_pLTServer->GetNextInactiveObject(hObj))) != LTNULL)
		{
			if (!g_pLTServer->IsKindOf(g_pLTServer->GetObjectClass(hObj), hBody)) continue;
			Body* pBody = (Body*)g_pLTServer->HandleToObject(hObj);
			if (!pBody) continue;

			LTFLOAT fStart = pBody->GetStarttime();
			if (fStart <= 0.0f) fStart = fTime;		// Not updated yet: brand new
			bodies.push_back(std::make_pair(fStart, pBody));
		}
	}

	std::sort(bodies.begin(), bodies.end());	// Oldest first

	const size_t kMaxBodies = 16;
	for (size_t i = 0; i < bodies.size(); i++)
	{
		LTBOOL bTooOld  = (fTime - bodies[i].first > 30.0f);
		LTBOOL bTooMany = (bodies.size() - i > kMaxBodies);
		if (!bTooOld && !bTooMany) break;

		bodies[i].second->RemoveObject();
		m_nBodiesRemoved++;
	}
}

// Snapshot of what every bot is doing, for tracking down stuck spots
void CBotMgr::WriteLog(LTBOOL bNewWorld)
{
	_mkdir("Bots");
	// Self-play runs log elsewhere, and keep every world of the run in one file
	const char* szLog = getenv("NOLFBOT_LOG");
	LTBOOL bSelfPlayLog = (szLog && szLog[0]);
	FILE* fp = fopen(bSelfPlayLog ? szLog : "Bots\\botlog.txt", (bNewWorld && !bSelfPlayLog) ? "wt" : "at");
	if (!fp) return;

	if (bNewWorld)
	{
		fprintf(fp, "World %s, %d waypoints%s\n", m_szNavWorld, m_Nav.GetNumNodes(), m_World.IsScanning() ? " (scanning the map)" : "");

		// The weapon table as the bots see it (to calibrate ranges / ammo use)
		for (int i = 0; g_pWeaponMgr && i < g_pWeaponMgr->GetNumWeapons(); i++)
		{
			WEAPON* pW = g_pWeaponMgr->GetWeapon(i);
			if (!pW || pW->IsAGadget()) continue;
			AMMO* pA = g_pWeaponMgr->GetAmmo(pW->nDefaultAmmoType);
			fprintf(fp, "WEAPON %2d %-24s range %6d perturb %d-%d delay %4u clip %3d vectors %d effective %.0f | ammo %s dmg %d area %d/%d spawn %d select %d\n",
				i, pW->szName, pW->nRange, pW->nMinPerturb, pW->nMaxPerturb, (unsigned)pW->m_nFireDelay, pW->nShotsPerClip,
				pW->nVectorsPerRound, CBotPlayer::GetEffectiveRange(pW, pA),
				pA ? pA->szName : "-", pA ? pA->nInstDamage : 0, pA ? pA->nAreaDamage : 0, pA ? pA->nAreaDamageRadius : 0,
				pA ? pA->nSpawnedAmount : 0, pA ? pA->nSelectionAmount : 0);
			for (int a = 0; a < pW->nNumAmmoTypes; a++)
			{
				AMMO* pX = g_pWeaponMgr->GetAmmo(pW->aAmmoTypes[a]);
				if (!pX) continue;
				fprintf(fp, "  AMMO %2d %-24s dmg %d area %d/%d burn %.0f x %.0fs | rated at 800u: %.2f\n", pX->nId, pX->szName,
					pX->nInstDamage, pX->nAreaDamage, pX->nAreaDamageRadius, pX->fProgDamage, pX->fProgDamageDuration,
					CBotPlayer::RateShot(pW, pX, 800.0f));
			}
		}
	}

	// Object counts (to spot leaks)
	int nActive = 0, nInactive = 0;
	for (HOBJECT h = g_pLTServer->GetNextObject(LTNULL); h; h = g_pLTServer->GetNextObject(h)) nActive++;
	for (HOBJECT h = g_pLTServer->GetNextInactiveObject(LTNULL); h; h = g_pLTServer->GetNextInactiveObject(h)) nInactive++;

	fprintf(fp, "--- t=%.0f objects %d active %d inactive, bodies removed %d | nav %d nodes (%d water, %d ladder) %d zip %d jump%s\n",
		g_pLTServer->GetTime(), nActive, nInactive, m_nBodiesRemoved,
		m_Nav.GetNumNodes(), m_Nav.CountNodes(BOTNODE_WATER), m_Nav.CountNodes(BOTNODE_LADDER),
		m_Nav.CountLinks(BOTLINK_ZIP), m_Nav.CountLinks(BOTLINK_JUMP), m_World.IsScanning() ? " (scanning the map)" : "");
	// Team-AI state.  Logged EVERY snapshot regardless of game type, so one run
	// shows whether the server sees this as an assault game and whether the
	// bases/documents were actually detected (the #1 thing that silently turns
	// the whole team brain off and makes bots play deathmatch).
	GameType eGT = g_pGameServerShell->GetGameType();
	fprintf(fp, "TEAMAI gametype=%d assault=%d bases t1=%d t2=%d features t1=%02x t2=%02x\n",
		(int)eGT, (eGT == COOPERATIVE_ASSAULT) ? 1 : 0,
		(int)m_aBases[TEAM_1].size(), (int)m_aBases[TEAM_2].size(), m_anFeatures[TEAM_1], m_anFeatures[TEAM_2]);
	{
		extern int g_anBotPathStats[6];
		extern int g_anBotZipStats[4];
		extern LTVector g_avBotPathFail[16][2];
		extern int g_nBotPathFail;
		for (int k = 0; k < 16 && k < g_nBotPathFail; k++)
		{
			const LTVector* v = g_avBotPathFail[(g_nBotPathFail - 1 - k) % 16];
			fprintf(fp, "NOROUTE (%.0f %.0f %.0f) -> (%.0f %.0f %.3f)\n", v[0].x, v[0].y, v[0].z, v[1].x, v[1].y, v[1].z);
		}
		fprintf(fp, "ZIPS rides %d landed %d fell %d held-high-ground %d\n", g_anBotZipStats[0], g_anBotZipStats[1], g_anBotZipStats[2], g_anBotZipStats[3]);
		fprintf(fp, "PATHS ok %d | failed: not-ready %d no-floor-here %d no-floor-there %d no-route %d cells %d\n",
			g_anBotPathStats[5], g_anBotPathStats[0], g_anBotPathStats[1], g_anBotPathStats[2], g_anBotPathStats[3], g_anBotPathStats[4]);
		int nFailed = 0;
		for (int l = 0; l < m_World.GetLinkCount(); l++) if (m_World.GetLinks()[l].nFails >= 3) nFailed++;
		fprintf(fp, "NAV map %s %d%%: %d floor cells (%d reachable, %d swim, %d flooded), islands %d, links zip %d ladder %d jump %d (failed %d), high nodes %d, scan %.0fs, danger nodes t0 %d t1 %d t2 %d\n",
			m_World.GetStateName(), m_World.GetProgress(), m_World.GetNumSpans(), m_World.CountSpans(BOTSPAN_REACH),
			m_World.CountSpans(BOTSPAN_SWIM), m_World.CountSpans(BOTSPAN_NOAIR), m_World.GetIslands(),
			m_World.CountLinks(eBotLinkZip), m_World.CountLinks(eBotLinkLadder), m_World.CountLinks(eBotLinkJump), nFailed,
			m_Nav.CountNodes(BOTNODE_HIGH), m_World.GetScanSeconds(),
			m_Nav.CountDangerNodes(0, 1.0f), m_Nav.CountDangerNodes(1, 1.0f), m_Nav.CountDangerNodes(2, 1.0f));
	}
	{
		LTFLOAT fNow = g_pLTServer->GetTime();
		if (m_fFramesSince > 0.0f && fNow > m_fFramesSince)
		{
			fprintf(fp, "FRAMES %d in %.0f game s = %.0f per game second, genomes t1 %s t2 %s\n", m_nFrames,
				fNow - m_fFramesSince, m_nFrames / (fNow - m_fFramesSince), m_aGenome[TEAM_1].szId, m_aGenome[TEAM_2].szId);
		}
		m_nFrames = 0;
		m_fFramesSince = fNow;
		const char* szScale = getenv("NOLFBOT_TIMESCALE");
		if (szScale && atof(szScale) > 1.0) fprintf(fp, "SELFPLAY speed x%s (%d clock hooks)\n", szScale, s_nSpeedHooks);
	}

	if (eGT == COOPERATIVE_ASSAULT)
	{
		CTeamMgr* pTM = g_pGameServerShell->GetTeamMgr();
		CTeam* pT1 = pTM ? pTM->GetTeam(TEAM_1) : LTNULL;
		CTeam* pT2 = pTM ? pTM->GetTeam(TEAM_2) : LTNULL;
		fprintf(fp, "SCORE team1 %d team2 %d, bot photo attempts %d\n", pT1 ? pT1->GetScore() : 0, pT2 ? pT2->GetScore() : 0, m_nIntelTries);

		// Shared per-team targets (guard/sneak are read-only; push is read from
		// the cache so logging doesn't disturb the attackers' current pick).
		for (int t = TEAM_1; t <= TEAM_2; t++)
		{
			char szP[48], szG[48], szS[48];
			LTVector v;
			if (m_afPushUntil[t] > 0.0f) _snprintf(szP, sizeof(szP)-1, "(%.0f %.0f %.0f)", m_avPush[t].x, m_avPush[t].y, m_avPush[t].z);
			else SAFE_STRCPY(szP, "-");
			if (GetGuardTarget(t, v)) _snprintf(szG, sizeof(szG)-1, "(%.0f %.0f %.0f)", v.x, v.y, v.z); else SAFE_STRCPY(szG, "-");
			if (GetSneakTarget(t, v)) _snprintf(szS, sizeof(szS)-1, "(%.0f %.0f %.0f)", v.x, v.y, v.z); else SAFE_STRCPY(szS, "-");
			szP[sizeof(szP)-1] = szG[sizeof(szG)-1] = szS[sizeof(szS)-1] = '\0';
			LTVector vAl;
			fprintf(fp, "TEAM%d push%s guard%s sneak%s | alert%s recalled %d, no-path-to-objective %d\n", t, szP, szG, szS,
				GetTeamAlert(t, vAl, 12.0f) ? "ON" : "-", (int)m_aRecalled[t].size(), m_anRoleFail[t]);
			const TeamPlan& p = m_aPlan[t];
			for (int r = 0; r < m_MapInfo.GetNumRoutes(t); r++)
			{
				BotRoute& route = m_MapInfo.GetRoute(t, r);
				static const char* s_aKind[] = { "main", "flank", "high", "water" };
				fprintf(fp, "LANE%d #%d %s len %.0f chokes %d bots %d\n", t, r, s_aKind[route.nKind], route.fLength, (int)route.chokes.size(), route.nUsers);
			}
			if (p.bStageOk)
			{
				fprintf(fp, "WAVE%d %s %.0fs, at stage %d/%d, near objective %d, waves %d, stage(%.0f %.0f %.0f)\n", t,
					p.nState == eWavePush ? "PUSH" : "GATHER", g_pLTServer->GetTime() - p.fStateTime,
					p.nAtStage, p.nMembers, p.nNear, p.nWaves, p.vStage.x, p.vStage.y, p.vStage.z);
			}
			else
			{
				fprintf(fp, "WAVE%d off (no staging point)\n", t);
			}
		}

		for (size_t i = 0; i < m_Items.size(); i++)
		{
			const BotItem& item = m_Items[i];
			if (item.eType != BotItem::eIntel) continue;
			fprintf(fp, "INTEL for team%d %s %s pos(%.0f %.0f %.0f)\n", (int)item.nTeam, item.bPhotoOnly ? "photo" : "pickup",
				item.bAvailable ? "available" : "taken", item.vPos.x, item.vPos.y, item.vPos.z);
		}
	}

	for (size_t i = 0; i < m_Bots.size(); i++)
	{
		if (!m_Bots[i].pBot) continue;
		char szLine[768];
		m_Bots[i].pBot->Describe(szLine, sizeof(szLine));
		fprintf(fp, "%s\n", szLine);
	}

	const std::vector<CPlayerObj*>& players = GetPlayers();
	for (size_t i = 0; i < players.size(); i++)
	{
		if (players[i]->IsBot()) continue;
		LTVector vPos;
		g_pLTServer->GetObjectPos(players[i]->m_hObject, &vPos);
		fprintf(fp, "HUMAN %-12s team%d pos(%.0f %.0f %.0f) frags%d\n", players[i]->GetNetName(),
			(int)players[i]->GetTeamID(), vPos.x, vPos.y, vPos.z, players[i]->GetFragCount());
	}

	fclose(fp);
}

void CBotMgr::LoadNav()
{
	const char* szWorld = g_pGameServerShell->GetCurLevel();
	if (!szWorld || !szWorld[0]) return;

	SAFE_STRCPY(m_szNavWorld, szWorld);

	// What the map looks like: measured once (then cached), the waypoint
	// graph is made from that
	RestartNav(LTFALSE);
	m_fNextScanMsg = g_pLTServer->GetTime() + 5.0f;
	if (m_World.IsReady()) FinishNav();
	else Message("Bots are studying the map (first time only)...");

	// Team bases, for attacking/defending in H.A.R.M. vs UNITY
	for (int t = 0; t < 3; t++)
	{
		m_aBases[t].clear(); m_afPushUntil[t] = 0.0f; m_afAlertTime[t] = -100.0f;
		m_aRecalled[t].clear(); m_anRoleFail[t] = 0;

		for (int s = 0; s < kSightings; s++) m_afSightingTime[t][s] = -100.0f;

		// New map: start by gathering the first wave
		memset(&m_aPlan[t], 0, sizeof(m_aPlan[t]));
		m_aPlan[t].nState	  = eWaveGather;
		m_aPlan[t].fStateTime = g_pLTServer->GetTime();
	}
	m_fNextNavSave	= g_pLTServer->GetTime() + 120.0f;
	m_nSneakNavSize	= -1;		// new map: find its underwater approach again

	// The trained fighting parameters (self-play training assigns its own)
	if (!GetTrainDir())
	{
		for (int t = 0; t < 3; t++)
		{
			if (!m_aGenome[t].Load("Bots\\genome.txt")) m_aGenome[t].SetDefaults();
		}
	}
	m_nRound = 0;
	HCLASS hStartPt = g_pLTServer->GetClass("GameStartPoint");
	for (int nPass = 0; nPass < 2; nPass++)
	{
		HOBJECT hObj = LTNULL;
		while ((hObj = (nPass == 0 ? g_pLTServer->GetNextObject(hObj) : g_pLTServer->GetNextInactiveObject(hObj))) != LTNULL)
		{
			if (!g_pLTServer->IsKindOf(g_pLTServer->GetObjectClass(hObj), hStartPt)) continue;
			GameStartPoint* pStart = (GameStartPoint*)g_pLTServer->HandleToObject(hObj);
			if (!pStart || pStart->GetGameType() != COOPERATIVE_ASSAULT) continue;

			uint32 nTeam = pStart->GetTeam();
			if (nTeam != TEAM_1 && nTeam != TEAM_2) continue;

			LTVector vPos;
			g_pLTServer->GetObjectPos(hObj, &vPos);
			m_aBases[nTeam].push_back(vPos);
		}
	}

	m_CampScore.assign(m_Nav.GetNumNodes(), -1.0f);
	m_CampLook.assign(m_Nav.GetNumNodes(), LTVector(0.0f, 0.0f, 1.0f));
	m_nCampScoreNext = 0;
}

// Everything in the map the bots know about, by what it is and where: every
// pickup, spawn, document, zipcord hook, door, vehicle, volume and prop.
void CBotMgr::WriteCatalog()
{
	if (!m_szNavWorld[0]) return;
	const char* pBase = m_szNavWorld;
	for (const char* p = m_szNavWorld; *p; p++) if (*p == '\\' || *p == '/') pBase = p + 1;
	char szFile[MAX_PATH];
	_snprintf(szFile, sizeof(szFile) - 1, "Bots\\%s-objects.txt", pBase);
	szFile[sizeof(szFile) - 1] = '\0';
	FILE* fp = fopen(szFile, "wt");
	if (!fp) return;

	static const char* s_aszType[] = { "weapon", "ammo", "gear", "mod", "intel", "vehicle", "other" };
	fprintf(fp, "# Pickups: type name pos area playable respawn(s) [ammo]\n");
	for (size_t i = 0; i < m_Items.size(); i++)
	{
		const BotItem& it = m_Items[i];
		int nSpan = m_World.IsReady() ? m_World.FindSpanNear(it.vPos + LTVector(0.0f, 40.0f, 0.0f), 4) : -1;
		int nNode = (nSpan >= 0) ? m_World.GetSpan(nSpan).nNode : -1;
		fprintf(fp, "ITEM %-7s %-24s %6.0f %6.0f %6.0f area %4d %s %3.0f", s_aszType[it.eType], it.szName[0] ? it.szName : "-",
			it.vPos.x, it.vPos.y, it.vPos.z, m_MapInfo.GetAreaOf(nNode), m_Nav.IsPlayable(nNode) ? "reach" : "UNREACHABLE", it.fRespawnDelay);
		for (int a = 0; a < it.nAmmoTypes; a++)
		{
			AMMO* pA = g_pWeaponMgr->GetAmmo(it.anAmmoId[a]);
			if (pA) fprintf(fp, " [%s]", pA->szName);
		}
		if (it.eType == BotItem::eIntel) fprintf(fp, " for-team %d", (int)it.nTeam);
		fprintf(fp, "\n");
	}

	// Every other object, by class; positions for the ones that matter
	std::map<std::string, int> counts;
	static HCLASS hProp	 = g_pLTServer->GetClass("Prop");
	static HCLASS hStart = g_pLTServer->GetClass("GameStartPoint");
	static HCLASS hZip	 = g_pLTServer->GetClass("ZipHook");
	static HCLASS hDoor	 = g_pLTServer->GetClass("Door");
	static HCLASS hVol	 = g_pLTServer->GetClass("VolumeBrush");
	for (int nPass = 0; nPass < 2; nPass++)
	{
		HOBJECT hObj = LTNULL;
		while ((hObj = (nPass == 0 ? g_pLTServer->GetNextObject(hObj) : g_pLTServer->GetNextInactiveObject(hObj))) != LTNULL)
		{
			HCLASS hClass = g_pLTServer->GetObjectClass(hObj);
			char szClass[64];
			g_pLTServer->GetClassName(hClass, szClass, sizeof(szClass));
			counts[szClass]++;

			LTVector vPos, vDims;
			g_pLTServer->GetObjectPos(hObj, &vPos);
			g_pLTServer->GetObjectDims(hObj, &vDims);
			const char* szKind = LTNULL;
			if (hStart && g_pLTServer->IsKindOf(hClass, hStart))
			{
				GameStartPoint* pStart = (GameStartPoint*)g_pLTServer->HandleToObject(hObj);
				fprintf(fp, "SPAWN team %d gametype %d %6.0f %6.0f %6.0f\n", pStart ? (int)pStart->GetTeam() : -1,
					pStart ? (int)pStart->GetGameType() : -1, vPos.x, vPos.y, vPos.z);
				continue;
			}
			if (hZip && g_pLTServer->IsKindOf(hClass, hZip)) szKind = "ZIPHOOK";
			else if (hDoor && g_pLTServer->IsKindOf(hClass, hDoor)) szKind = "DOOR";
			else if (hVol && g_pLTServer->IsKindOf(hClass, hVol))
			{
				uint16 nCode = 0;
				g_pLTServer->GetContainerCode(hObj, &nCode);
				fprintf(fp, "VOLUME %-16s code %2d %6.0f %6.0f %6.0f size %4.0f %4.0f %4.0f\n", szClass, (int)nCode, vPos.x, vPos.y, vPos.z, vDims.x * 2.0f, vDims.y * 2.0f, vDims.z * 2.0f);
				continue;
			}
			else if (hProp && g_pLTServer->IsKindOf(hClass, hProp)) szKind = "PROP";

			// Anything else solid that isn't level geometry: the map scan
			// doesn't see it (rocks / clip boxes bots got wedged on)
			uint32 dwFlags = g_pLTServer->GetObjectFlags(hObj);
			uint32 nType = g_pLTServer->GetObjectType(hObj);
			if (!szKind && (dwFlags & FLAG_SOLID) && nType != OT_WORLDMODEL && !IsMainWorld(hObj))
			{
				fprintf(fp, "SOLID  %-20s type %u %6.0f %6.0f %6.0f size %4.0f %4.0f %4.0f flags %08x%s\n", szClass, nType,
					vPos.x, vPos.y, vPos.z, vDims.x * 2.0f, vDims.y * 2.0f, vDims.z * 2.0f, dwFlags, nPass ? " inactive" : "");
			}
			if (szKind)
			{
				fprintf(fp, "%-7s %-20s %6.0f %6.0f %6.0f size %4.0f %4.0f %4.0f\n", szKind, szClass, vPos.x, vPos.y, vPos.z, vDims.x * 2.0f, vDims.y * 2.0f, vDims.z * 2.0f);
			}
		}
	}
	fprintf(fp, "# Object classes in the map\n");
	for (std::map<std::string, int>::const_iterator it = counts.begin(); it != counts.end(); ++it)
	{
		fprintf(fp, "CLASS %-28s %d\n", it->first.c_str(), it->second);
	}
	fclose(fp);
}

void CBotMgr::DropNav()
{
	// Node, span and area ids all belong to the old graph
	m_Nav.Clear();
	m_MapInfo.Clear();
	for (size_t i = 0; i < m_Bots.size(); i++)
	{
		if (m_Bots[i].pBot) m_Bots[i].pBot->ForgetNav();
	}
	for (int t = 0; t < 3; t++) m_anRouteGen[t]++;
}

void CBotMgr::RestartNav(LTBOOL bRescan)
{
	DropNav();
	m_World.Begin(g_pGameServerShell->GetCurLevel(), bRescan);
}

void CBotMgr::FinishNav()
{
	m_Nav.BuildFromWorld(m_World);
	m_MapInfo.Build(m_Nav, m_World);
	m_MapInfo.DumpFile(m_szNavWorld, m_Nav);
	RefreshItems();
	WriteCatalog();
	for (int t = 0; t < 3; t++)
	{
		m_anRouteGen[t]++;
		m_aPlan[t].fNextStage = 0.0f;		// work out the routes now
	}
	if (!m_Nav.LoadDanger(m_szNavWorld)) m_Nav.Save(m_szNavWorld);	// new graph: start a fresh file

	m_CampScore.assign(m_Nav.GetNumNodes(), -1.0f);
	m_CampLook.assign(m_Nav.GetNumNodes(), LTVector(0.0f, 0.0f, 1.0f));
	m_nCampScoreNext = 0;
	m_nSneakNavSize	 = -1;

	Message("Bots know the map: %d waypoints, %d zipcords, %d ladders, %d jumps, %d perches",
		m_Nav.GetNumNodes(), m_World.CountLinks(eBotLinkZip), m_World.CountLinks(eBotLinkLadder),
		m_World.CountLinks(eBotLinkJump), m_Nav.CountNodes(BOTNODE_HIGH));
}

// ----------------------------------------------------------------------- //
//	Camp spots: waypoints that overlook a lot of other waypoints.  Computed
//	a few per frame so it doesn't hitch.
// ----------------------------------------------------------------------- //

void CBotMgr::UpdateCampScores()
{
	int nNodes = m_Nav.GetNumNodes();
	if ((int)m_CampScore.size() > nNodes)
	{
		// The graph was replaced (cleared / regenerated): start over
		m_CampScore.assign(nNodes, -1.0f);
		m_CampLook.assign(nNodes, LTVector(0.0f, 0.0f, 1.0f));
		m_nCampScoreNext = 0;
	}
	else if ((int)m_CampScore.size() < nNodes)
	{
		// Learned a few new nodes: keep the scores we have, score the new ones
		int nOld = (int)m_CampScore.size();
		m_CampScore.resize(nNodes, -1.0f);
		m_CampLook.resize(nNodes, LTVector(0.0f, 0.0f, 1.0f));
		if (m_nCampScoreNext > nOld) m_nCampScoreNext = nOld;
	}

	for (int nWork = 0; nWork < 4 && m_nCampScoreNext < nNodes; nWork++, m_nCampScoreNext++)
	{
		int n = m_nCampScoreNext;
		const LTVector& vPos = m_Nav.GetNode(n).vPos;
		LTVector vEye = vPos + LTVector(0.0f, 30.0f, 0.0f);

		// Sample up to ~48 other waypoints
		int nStep = nNodes / 48 + 1;
		int nSeen = 0;
		LTFLOAT fSeenY = 0.0f;
		LTVector vSum(0.0f, 0.0f, 0.0f);

		for (int o = (n % nStep); o < nNodes; o += nStep)
		{
			if (o == n) continue;
			const LTVector& vOther = m_Nav.GetNode(o).vPos;
			LTFLOAT fDist = vPos.Dist(vOther);
			if (fDist < 200.0f || fDist > 3000.0f) continue;

			if (CBotNav::IsVisible(vEye, vOther + LTVector(0.0f, 20.0f, 0.0f)))
			{
				nSeen++;
				fSeenY += vOther.y;
				LTVector vDir = vOther - vPos;
				vDir.y = 0.0f;
				vDir.Norm();
				vSum += vDir;
			}
		}

		// Few ways to be approached from (nodes with few links, i.e. corners and
		// ledges) and a bit of height make a spot better.
		LTFLOAT fScore = (LTFLOAT)nSeen;
		if (m_Nav.GetIncoming(n) <= 2) fScore *= 1.3f;

		// High ground: looking down on the area we cover
		if (nSeen > 0)
		{
			LTFLOAT fAbove = vPos.y - fSeenY / (LTFLOAT)nSeen;
			fScore *= 1.0f + LTCLAMP(fAbove / 300.0f, 0.0f, 1.5f);
		}

		// Perches (only up by zipcord / ladder / climbing, or high above the
		// rest) are what high ground is for
		if (m_Nav.GetNode(n).nFlags & BOTNODE_HIGH) fScore *= 1.8f;

		// No camping in the water
		if (m_Nav.GetNode(n).nFlags & BOTNODE_WATER) fScore = 0.0f;

		m_CampScore[n] = fScore;
		if (vSum.MagSqr() > 0.01f)
		{
			vSum.Norm();
			m_CampLook[n] = vSum;
		}
	}
}

LTBOOL CBotMgr::GetCampLook(int nNode, LTVector& vLook) const
{
	if (nNode < 0 || nNode >= (int)m_CampScore.size() || m_CampScore[nNode] < 0.0f) return LTFALSE;
	vLook = m_CampLook[nNode];
	return LTTRUE;
}

int CBotMgr::PickCampNode(const LTVector& vNear, LTVector& vLook, LTBOOL bHighOnly)
{
	int nNodes = m_Nav.GetNumNodes();
	if (nNodes == 0 || (int)m_CampScore.size() != nNodes) return BOTNAV_INVALID;

	// Best few spots within reach, pick one of them at random...
	int aBest[5] = { -1, -1, -1, -1, -1 };
	LTFLOAT aScore[5] = { 0, 0, 0, 0, 0 };

	for (int n = 0; n < nNodes; n++)
	{
		if (m_CampScore[n] < 3.0f) continue;
		if (bHighOnly && !(m_Nav.GetNode(n).nFlags & BOTNODE_HIGH)) continue;
		if (!m_Nav.IsPlayable(n)) continue;

		LTFLOAT fDist = vNear.Dist(m_Nav.GetNode(n).vPos);
		LTFLOAT fScore = m_CampScore[n] / (1.0f + fDist / 2000.0f);

		// Don't stack campers on the same spot
		LTBOOL bTaken = LTFALSE;
		const std::vector<CPlayerObj*>& players = GetPlayers();
		for (size_t b = 0; b < players.size(); b++)
		{
			LTVector vOther;
			g_pLTServer->GetObjectPos(players[b]->m_hObject, &vOther);
			if (vOther.DistSqr(m_Nav.GetNode(n).vPos) < 150.0f*150.0f && vOther.DistSqr(vNear) > 1.0f) bTaken = LTTRUE;
		}
		if (bTaken) continue;

		for (int k = 0; k < 5; k++)
		{
			if (fScore > aScore[k])
			{
				for (int j = 4; j > k; j--) { aScore[j] = aScore[j-1]; aBest[j] = aBest[j-1]; }
				aScore[k] = fScore;
				aBest[k] = n;
				break;
			}
		}
	}

	int nCount = 0;
	while (nCount < 5 && aBest[nCount] >= 0) nCount++;
	if (nCount == 0) return BOTNAV_INVALID;

	int nPick = aBest[GetRandom(0, nCount-1)];
	vLook = m_CampLook[nPick];
	return nPick;
}

// ----------------------------------------------------------------------- //
//	Update
// ----------------------------------------------------------------------- //

void CBotMgr::Update()
{
	if (!m_bWorldLoaded || !g_pGameServerShell) return;
	if (g_pGameServerShell->GetGameType() == SINGLE) return;

	LTFLOAT fTime = g_pLTServer->GetTime();

	if (!m_szNavWorld[0])
	{
		LoadNav();
	}

	// bots.cfg once per hosting session (after the host options have been
	// applied on the first server frame), settings again on every world
	if (!m_bConfigRun)
	{
		m_bConfigRun = LTTRUE;
		m_bSettingsApplied = LTTRUE;
		RunConfig();
		ApplySelfPlaySpeed();
	}
	else if (!m_bSettingsApplied)
	{
		m_bSettingsApplied = LTTRUE;
		ApplySettings();
		ApplySelfPlaySpeed();
	}

	if (m_World.IsScanning())
	{
		static LTFLOAT s_fBudget = -1.0f;
		if (s_fBudget < 0.0f)
		{
			const char* szMs = getenv("NOLFBOT_SCANMS");
			s_fBudget = (szMs && atof(szMs) > 0.0) ? (LTFLOAT)atof(szMs) : 30.0f;
		}
		if (m_World.Step(s_fBudget))
		{
			FinishNav();
		}
		else if (fTime > m_fNextScanMsg)
		{
			m_fNextScanMsg = fTime + 10.0f;
			Message("Bots are studying the map: %s %d%%", m_World.GetStateName(), m_World.GetProgress());
		}
	}
	else if (!IsNavBusy())
	{
		UpdateCampScores();
	}

	if (fTime > m_fNextItemRefresh)
	{
		RefreshItems();
		m_fNextItemRefresh = fTime + 0.5f;
	}

	// (Re)create bots after a world change...

	if (m_fSpawnBotsTime >= 0.0f && fTime > m_fSpawnBotsTime)
	{
		m_fSpawnBotsTime = -1.0f;
	}

	if (m_fSpawnBotsTime < 0.0f)
	{
		UpdateJoining(fTime);
		UpdateMinPlayers();
		UpdateAutoSkill(fTime);
		CleanupBodies(fTime);

		if (fTime > m_fNextLog)
		{
			WriteLog(m_fNextLog == 0.0f);
			m_fNextLog = fTime + 20.0f;
		}
	}

	// Keep the learned danger (also saved on map change)
	if (m_Nav.GetChanges() >= 10 && fTime > m_fNextNavSave && m_szNavWorld[0] && !IsNavBusy())
	{
		m_Nav.Save(m_szNavWorld);
		m_fNextNavSave = fTime + 120.0f;
	}

	if (fTime > m_fNextRecall)
	{
		m_fNextRecall = fTime + 0.5f;
		UpdateRecall(fTime);
		UpdatePlans(fTime);
	}

	m_nFrames++;
	UpdateTraining(fTime);

	// Old killing zones fade (the fight moves around): ~halves in 10 minutes
	if (fTime > m_fNextDangerDecay)
	{
		if (m_fNextDangerDecay > 0.0f) m_Nav.DecayDanger(0.935f);
		m_fNextDangerDecay = fTime + 60.0f;
	}
}

void CBotMgr::OnPlayerRemoved()
{
	m_fPlayersTime = -1.0f;
}

const std::vector<CPlayerObj*>& CBotMgr::GetPlayers()
{
	LTFLOAT fTime = g_pLTServer->GetTime();
	if (fTime != m_fPlayersTime)
	{
		RefreshPlayers();
		m_fPlayersTime = fTime;
	}
	return m_Players;
}

void CBotMgr::RefreshPlayers()
{
	m_Players.clear();

	HOBJECT hObj = LTNULL;
	while ((hObj = g_pLTServer->GetNextObject(hObj)) != LTNULL)
	{
		if (IsPlayer(hObj))
		{
			CPlayerObj* pPlayer = (CPlayerObj*)g_pLTServer->HandleToObject(hObj);
			if (pPlayer && (pPlayer->GetClient() || pPlayer->IsBot()))
			{
				m_Players.push_back(pPlayer);
			}
		}
	}
}

CPlayerObj* CBotMgr::FindPlayerByID(uint32 nID)
{
	const std::vector<CPlayerObj*>& players = GetPlayers();
	for (size_t i = 0; i < players.size(); i++)
	{
		if (players[i]->GetPlayerID() == nID) return players[i];
	}
	return LTNULL;
}

CPlayerObj* CBotMgr::GetBotPlayer(int i)
{
	if (i < 0 || i >= (int)m_Bots.size()) return LTNULL;
	return m_Bots[i].pBot;
}

void CBotMgr::RefreshItems()
{
	// Remember the previous state so we can tell when something got picked up
	std::vector<BotItem> prev;
	prev.swap(m_Items);
	LTFLOAT fTime = g_pLTServer->GetTime();
	LTFLOAT fScale = g_RespawnScaleTrack.IsInitted() ? g_RespawnScaleTrack.GetFloat(1.0f) : 1.0f;
	if (fScale <= 0.0f) fScale = 1.0f;

	static HCLASS hPickup	= g_pLTServer->GetClass("PickupItem");
	static HCLASS hWeapon	= g_pLTServer->GetClass("WeaponItem");
	static HCLASS hAmmo		= g_pLTServer->GetClass("AmmoBox");
	static HCLASS hGear		= g_pLTServer->GetClass("GearItem");
	static HCLASS hMod		= g_pLTServer->GetClass("ModItem");
	static HCLASS hIntel	= g_pLTServer->GetClass("Intelligence");
	static HCLASS hVehicle	= g_pLTServer->GetClass("PlayerVehicle");

	for (int nPass = 0; nPass < 2; nPass++)
	{
		HOBJECT hObj = LTNULL;
		while ((hObj = (nPass == 0 ? g_pLTServer->GetNextObject(hObj) : g_pLTServer->GetNextInactiveObject(hObj))) != LTNULL)
		{
			HCLASS hClass = g_pLTServer->GetObjectClass(hObj);

			BotItem item;
			item.nWeaponId		= -1;
			item.nTeam			= 0;
			item.bPhotoOnly		= LTFALSE;
			item.fRespawnDelay	= 30.0f;
			item.fRespawnAt		= 0.0f;
			item.bScuba			= LTFALSE;
			item.bArmor			= LTFALSE;
			item.nAmmoTypes		= 0;
			item.szName[0]		= '\0';

			if (g_pLTServer->IsKindOf(hClass, hWeapon))
			{
				item.eType = BotItem::eWeapon;
				WeaponItem* pWeapon = (WeaponItem*)g_pLTServer->HandleToObject(hObj);
				if (pWeapon)
				{
					item.nWeaponId	 = pWeapon->GetWeaponId();
					item.anAmmoId[0] = pWeapon->GetAmmoId();
					item.nAmmoTypes	 = 1;
					WEAPON* pW = g_pWeaponMgr->GetWeapon(item.nWeaponId);
					if (pW) SAFE_STRCPY(item.szName, pW->szName);
				}
			}
			else if (g_pLTServer->IsKindOf(hClass, hAmmo))
			{
				item.eType = BotItem::eAmmo;
				AmmoBox* pBox = (AmmoBox*)g_pLTServer->HandleToObject(hObj);
				if (pBox) item.nAmmoTypes = pBox->GetAmmoTypes(item.anAmmoId, 4);
				AMMO* pA = item.nAmmoTypes ? g_pWeaponMgr->GetAmmo(item.anAmmoId[0]) : LTNULL;
				if (pA) SAFE_STRCPY(item.szName, pA->szName);
			}
			else if (g_pLTServer->IsKindOf(hClass, hGear))
			{
				item.eType = BotItem::eGear;
				GearItem* pGearItem = (GearItem*)g_pLTServer->HandleToObject(hObj);
				GEAR* pGear = pGearItem ? g_pWeaponMgr->GetGear(pGearItem->GetGearId()) : LTNULL;
				if (pGear)
				{
					item.bScuba = (pGear->eProtectionType == DT_CHOKE);
					item.bArmor = (pGear->fArmor > 0.0f);
					SAFE_STRCPY(item.szName, pGear->szName);
				}
			}
			else if (g_pLTServer->IsKindOf(hClass, hMod))		item.eType = BotItem::eMod;
			else if (hIntel && g_pLTServer->IsKindOf(hClass, hIntel))
			{
				item.eType = BotItem::eIntel;
				Intelligence* pIntel = (Intelligence*)g_pLTServer->HandleToObject(hObj);
				if (pIntel)
				{
					item.nTeam		= pIntel->GetTeamFilter();
					item.bPhotoOnly	= pIntel->IsPhotoOnly();
				}
			}
			else if (g_pLTServer->IsKindOf(hClass, hPickup))	item.eType = BotItem::eOther;
			else if (hVehicle && g_pLTServer->IsKindOf(hClass, hVehicle))
			{
				// Free (not being ridden, not waiting to respawn)?
				item.eType = BotItem::eVehicle;
				uint32 dwFlags = g_pLTServer->GetObjectFlags(hObj);
				uint32 dwUsr   = g_pLTServer->GetObjectUserFlags(hObj);
				item.bAvailable = (dwFlags & FLAG_VISIBLE) && !(dwUsr & USRFLG_ATTACH_HIDE1SHOW3);
				g_pLTServer->GetObjectPos(hObj, &item.vPos);
				m_Items.push_back(item);
				continue;
			}
			else continue;

			uint32 dwFlags = g_pLTServer->GetObjectFlags(hObj);
			if (item.eType == BotItem::eIntel)
			{
				item.bAvailable = (dwFlags & FLAG_VISIBLE) ? LTTRUE : LTFALSE;	// Activated, not touched
			}
			else
			{
				item.bAvailable = (dwFlags & FLAG_VISIBLE) && (dwFlags & FLAG_TOUCH_NOTIFY) ? LTTRUE : LTFALSE;
			}
			g_pLTServer->GetObjectPos(hObj, &item.vPos);

			if (g_pLTServer->IsKindOf(hClass, hPickup))
			{
				PickupItem* pPickup = (PickupItem*)g_pLTServer->HandleToObject(hObj);
				if (pPickup && pPickup->GetRespawnDelay() > 0.0f)
				{
					item.fRespawnDelay = pPickup->GetRespawnDelay() / fScale;
				}
			}

			// Respawn prediction: when did it disappear?
			if (!item.bAvailable)
			{
				for (size_t p = 0; p < prev.size(); p++)
				{
					if (prev[p].vPos.DistSqr(item.vPos) > 4.0f) continue;
					item.fRespawnAt = prev[p].bAvailable ? fTime + item.fRespawnDelay : prev[p].fRespawnAt;

					// Our documents just got photographed (everyone sees the
					// score message): the enemy is in our base — fall back.
					if (item.eType == BotItem::eIntel && prev[p].bAvailable &&
						(item.nTeam == TEAM_1 || item.nTeam == TEAM_2))
					{
						uint32 dwOwner = (item.nTeam == TEAM_1) ? TEAM_2 : TEAM_1;
						SetTeamAlert(dwOwner, item.vPos);

						m_anRoundPhotos[item.nTeam]++;

						// ...and the team that scored pulls back and regroups
						TeamPlan& plan = m_aPlan[item.nTeam];
						if (plan.nState == eWavePush) { plan.nState = eWaveGather; plan.fStateTime = fTime; }
					}
					break;
				}
			}

			m_Items.push_back(item);
		}
	}
}

LTFLOAT CBotMgr::GetItemRespawnIn(const LTVector& vPos) const
{
	for (size_t i = 0; i < m_Items.size(); i++)
	{
		if (m_Items[i].vPos.DistSqr(vPos) < 16.0f*16.0f)
		{
			if (m_Items[i].bAvailable) return 0.0f;
			if (m_Items[i].fRespawnAt <= 0.0f) return -1.0f;
			LTFLOAT fIn = m_Items[i].fRespawnAt - g_pLTServer->GetTime();
			return (fIn > 0.01f) ? fIn : 0.01f;
		}
	}
	return -1.0f;
}

// Can players get to this item from the spawns at all?
LTBOOL CBotMgr::IsItemReachable(const LTVector& vPos)
{
	if (IsNavBusy()) return LTTRUE;
	int nSpan = m_World.FindSpanNear(vPos + LTVector(0.0f, m_World.GetBody().fHalfHeight, 0.0f), 3);
	if (nSpan < 0) return LTFALSE;
	return m_Nav.IsPlayable(m_World.GetSpan(nSpan).nNode);
}

void CBotMgr::ClaimItem(const LTVector& vPos, CPlayerObj* pBot, LTFLOAT fUntil)
{
	if (!pBot) return;
	for (size_t i = 0; i < m_Claims.size(); i++)
	{
		if (m_Claims[i].nBot == pBot->GetPlayerID()) { m_Claims.erase(m_Claims.begin() + i); break; }	// one claim per bot
	}
	Claim c;
	c.vPos	= vPos;
	c.nBot	= pBot->GetPlayerID();
	c.nTeam	= pBot->GetTeamID();
	c.fUntil = fUntil;
	m_Claims.push_back(c);
}

LTBOOL CBotMgr::IsItemClaimed(const LTVector& vPos, CPlayerObj* pAsker)
{
	if (!pAsker) return LTFALSE;
	LTFLOAT fTime = g_pLTServer->GetTime();
	LTVector vMe;
	g_pLTServer->GetObjectPos(pAsker->m_hObject, &vMe);
	LTBOOL bTeams = (g_pGameServerShell->GetGameType() == COOPERATIVE_ASSAULT);

	for (size_t i = 0; i < m_Claims.size(); i++)
	{
		const Claim& c = m_Claims[i];
		if (c.nBot == pAsker->GetPlayerID() || fTime > c.fUntil) continue;
		if (bTeams && c.nTeam != pAsker->GetTeamID()) continue;		// the other team doesn't tell us
		if (!bTeams) continue;										// deathmatch: everybody for themselves
		if (c.vPos.DistSqr(vPos) > 32.0f*32.0f) continue;

		// Still on its way?  Then it's theirs, unless we're much closer.
		CPlayerObj* pOwner = FindPlayerByID(c.nBot);
		if (!pOwner || pOwner->GetState() != PS_ALIVE) continue;
		LTVector vOwner;
		g_pLTServer->GetObjectPos(pOwner->m_hObject, &vOwner);
		if (vMe.Dist(vPos) < vOwner.Dist(vPos) * 0.5f) continue;
		return LTTRUE;
	}
	return LTFALSE;
}

LTBOOL CBotMgr::IsItemUnderwater(const LTVector& vPos)
{
	if (IsNavBusy()) return LTFALSE;
	int nSpan = m_World.FindSpanNear(vPos + LTVector(0.0f, 40.0f, 0.0f), 2);
	if (nSpan < 0) return LTFALSE;
	const BotSpan& s = m_World.GetSpan(nSpan);
	if (!(s.nFlags & BOTSPAN_SWIM)) return LTFALSE;
	return (s.nFlags & BOTSPAN_NOAIR) || (s.fWater - vPos.y > 250.0f);
}

LTBOOL CBotMgr::IsItemAvailable(const LTVector& vPos) const
{
	for (size_t i = 0; i < m_Items.size(); i++)
	{
		if (m_Items[i].vPos.DistSqr(vPos) < 16.0f*16.0f)
		{
			return m_Items[i].bAvailable;
		}
	}
	return LTFALSE;
}

// Intelligence the given team collects (it sits in the other team's base).
// Neutral documents (team 0) count for everybody.  bAvailOnly skips the ones
// already photographed / waiting to respawn.
static LTBOOL FindIntelFor(const std::vector<BotItem>& items, uint32 dwTeam, LTVector& vPos, LTBOOL bAvailOnly)
{
	std::vector<LTVector> found;
	for (size_t i = 0; i < items.size(); i++)
	{
		const BotItem& it = items[i];
		if (it.eType != BotItem::eIntel) continue;
		if (it.nTeam != 0 && it.nTeam != dwTeam) continue;
		if (bAvailOnly && !it.bAvailable) continue;
		found.push_back(it.vPos);
	}
	if (found.empty()) return LTFALSE;
	vPos = found[GetRandom(0, (int)found.size()-1)];
	return LTTRUE;
}

LTBOOL CBotMgr::GetPushTarget(uint32 dwTeam, LTVector& vPos)
{
	if (dwTeam != TEAM_1 && dwTeam != TEAM_2) return LTFALSE;

	LTFLOAT fTime = g_pLTServer->GetTime();

	// Re-pick when the lock runs out, or when our current target is a document
	// that just got photographed while another one is still up.
	LTBOOL bStale = (fTime > m_afPushUntil[dwTeam]);
	if (!bStale && m_afPushUntil[dwTeam] > 0.0f)
	{
		for (size_t i = 0; i < m_Items.size(); i++)
		{
			const BotItem& it = m_Items[i];
			if (it.eType == BotItem::eIntel && !it.bAvailable && it.vPos.DistSqr(m_avPush[dwTeam]) < 16.0f*16.0f)
			{
				LTVector vScratch;
				if (FindIntelFor(m_Items, dwTeam, vScratch, LTTRUE)) bStale = LTTRUE;
				break;
			}
		}
	}

	if (bStale)
	{
		// Head for a document we can still photograph; if they're all down,
		// stage on a known document spot (they respawn there); only storm the
		// enemy spawn when the map has no documents for us at all.
		uint32 dwEnemy = (dwTeam == TEAM_1) ? TEAM_2 : TEAM_1;
		LTVector vTarget;
		LTBOOL bOk = FindIntelFor(m_Items, dwTeam, vTarget, LTTRUE);
		if (!bOk) bOk = FindIntelFor(m_Items, dwTeam, vTarget, LTFALSE);
		if (!bOk) bOk = GetBasePos(dwEnemy, vTarget);
		if (!bOk) return LTFALSE;

		m_avPush[dwTeam]	  = vTarget;
		m_afPushUntil[dwTeam] = fTime + 25.0f;
	}

	vPos = m_avPush[dwTeam];
	return LTTRUE;
}

// Divers: the underwater spot closest to the enemy base (well below the
// surface), to come up where nobody is looking.
LTBOOL CBotMgr::GetSneakTarget(uint32 dwTeam, LTVector& vPos)
{
	if (dwTeam != TEAM_1 && dwTeam != TEAM_2) return LTFALSE;
	if (IsNavBusy()) return LTFALSE;

	// Recompute for a new map, or once the graph has grown a lot from learning
	// (not for every learned node: it scans every water waypoint)
	if (m_nSneakNavSize < 0 || abs(m_Nav.GetNumNodes() - m_nSneakNavSize) > 500)
	{
		m_nSneakNavSize = m_Nav.GetNumNodes();
		for (int t = 0; t < 3; t++)
		{
			m_anSneakNode[t] = BOTNAV_INVALID;
			if (t != TEAM_1 && t != TEAM_2) continue;

			uint32 dwEnemy = (t == TEAM_1) ? TEAM_2 : TEAM_1;
			const std::vector<LTVector>& bases = m_aBases[dwEnemy];
			if (bases.empty()) continue;

			LTVector vBase(0.0f, 0.0f, 0.0f);
			for (size_t b = 0; b < bases.size(); b++) vBase += bases[b];
			vBase /= (LTFLOAT)bases.size();

			LTFLOAT fBest = 1.0e30f;
			for (int n = 0; n < m_Nav.GetNumNodes(); n++)
			{
				const BotNavNode& node = m_Nav.GetNode(n);
				if (!(node.nFlags & BOTNODE_WATER)) continue;
				// Under water, not swimming along the top
				if (!CBotNav::IsInLiquid(node.vPos + LTVector(0.0f, 120.0f, 0.0f))) continue;

				LTFLOAT dx = node.vPos.x - vBase.x, dz = node.vPos.z - vBase.z;
				LTFLOAT fDist = dx*dx + dz*dz;
				if (fDist < fBest) { fBest = fDist; m_anSneakNode[t] = n; }
			}
		}
	}

	if (m_anSneakNode[dwTeam] == BOTNAV_INVALID) return LTFALSE;
	vPos = m_Nav.GetNode(m_anSneakNode[dwTeam]).vPos;
	return LTTRUE;
}

LTBOOL CBotMgr::GetGuardTarget(uint32 dwTeam, LTVector& vPos)
{
	if (dwTeam != TEAM_1 && dwTeam != TEAM_2) return LTFALSE;

	// The documents the enemy wants (in our base), else our spawn.  Guard the
	// spot whether or not it is currently photographed (it respawns there).
	uint32 dwEnemy = (dwTeam == TEAM_1) ? TEAM_2 : TEAM_1;
	if (FindIntelFor(m_Items, dwEnemy, vPos, LTFALSE)) return LTTRUE;
	return GetBasePos(dwTeam, vPos);
}

LTBOOL CBotMgr::GetBasePos(uint32 dwTeam, LTVector& vPos) const
{
	if (dwTeam != TEAM_1 && dwTeam != TEAM_2) return LTFALSE;
	const std::vector<LTVector>& bases = m_aBases[dwTeam];
	if (bases.empty()) return LTFALSE;
	vPos = bases[GetRandom(0, (int)bases.size()-1)];
	return LTTRUE;
}

// A teammate spotted an enemy inside our own base: share it so idle defenders
// (even campers) go intercept instead of standing on the documents.
void CBotMgr::SetTeamAlert(uint32 dwTeam, const LTVector& vPos)
{
	if (dwTeam != TEAM_1 && dwTeam != TEAM_2) return;
	m_avAlert[dwTeam]	 = vPos;
	m_afAlertTime[dwTeam] = g_pLTServer->GetTime();
}

LTBOOL CBotMgr::GetTeamAlert(uint32 dwTeam, LTVector& vPos, LTFLOAT fMaxAge)
{
	if (dwTeam != TEAM_1 && dwTeam != TEAM_2) return LTFALSE;
	if (g_pLTServer->GetTime() - m_afAlertTime[dwTeam] > fMaxAge) return LTFALSE;
	vPos = m_avAlert[dwTeam];
	return LTTRUE;
}

// Team play: everybody pushes forward by default.  While our base is under
// attack (a teammate saw an enemy there, or our documents just got
// photographed) the nearest third of the team falls back to defend, then goes
// forward again once it's quiet.
void CBotMgr::UpdateRecall(LTFLOAT fTime)
{
	(void)fTime;
	for (uint32 t = TEAM_1; t <= TEAM_2; t++)
	{
		std::vector<uint32> old;
		old.swap(m_aRecalled[t]);

		if (g_pGameServerShell->GetGameType() != COOPERATIVE_ASSAULT) continue;
		if (!HasFeature(t, eFeatRecall)) continue;

		LTVector vAlert;
		if (!GetTeamAlert(t, vAlert, 12.0f)) continue;

		std::vector< std::pair<LTFLOAT, uint32> > cands;
		for (size_t i = 0; i < m_Bots.size(); i++)
		{
			CBotPlayer* pBot = m_Bots[i].pBot;
			if (!pBot || pBot->GetState() != PS_ALIVE || pBot->GetTeamID() != t) continue;
			LTVector vPos;
			g_pLTServer->GetObjectPos(pBot->m_hObject, &vPos);
			cands.push_back(std::make_pair(vPos.Dist(vAlert), pBot->GetPlayerID()));
		}
		if (cands.empty()) continue;
		std::sort(cands.begin(), cands.end());

		size_t nWant = (cands.size() + 2) / 3;		// a third of the team, at least one

		// Whoever is already falling back keeps doing it (no flip-flopping)...
		for (size_t i = 0; i < cands.size() && m_aRecalled[t].size() < nWant; i++)
		{
			for (size_t o = 0; o < old.size(); o++)
			{
				if (old[o] == cands[i].second) { m_aRecalled[t].push_back(cands[i].second); break; }
			}
		}
		// ...then the closest others fill up the rest
		for (size_t i = 0; i < cands.size() && m_aRecalled[t].size() < nWant; i++)
		{
			if (!IsRecalled(cands[i].second, t)) m_aRecalled[t].push_back(cands[i].second);
		}
	}
}

LTBOOL CBotMgr::IsRecalled(uint32 nPlayerID, uint32 dwTeam) const
{
	if (dwTeam != TEAM_1 && dwTeam != TEAM_2) return LTFALSE;
	const std::vector<uint32>& v = m_aRecalled[dwTeam];
	for (size_t i = 0; i < v.size(); i++) if (v[i] == nPlayerID) return LTTRUE;
	return LTFALSE;
}

void CBotMgr::NoteRoleFail(uint32 dwTeam)
{
	if (dwTeam == TEAM_1 || dwTeam == TEAM_2) m_anRoleFail[dwTeam]++;
}

// ----------------------------------------------------------------------- //
//	Wave attacks.  Attackers trickling in one by one just feed the defence.
//	Like a human team, they gather out of sight of the objective, then go in
//	together; after a photo (or if the wave is wiped) they regroup.
// ----------------------------------------------------------------------- //

static LTFLOAT HorizDistMgr(const LTVector& a, const LTVector& b)
{
	LTFLOAT dx = a.x - b.x, dz = a.z - b.z;
	return (LTFLOAT)sqrt(dx*dx + dz*dz);
}

// The staging point: on the route from our spawn to the documents, about
// 1600-3500 units out, and preferably where the documents can't see it.
LTBOOL CBotMgr::ComputeStage(uint32 dwTeam, const LTVector& vTarget, LTVector& vStage)
{
	const std::vector<LTVector>& bases = m_aBases[dwTeam];
	if (bases.empty() || IsNavBusy()) return LTFALSE;

	LTVector vBase(0.0f, 0.0f, 0.0f);
	for (size_t b = 0; b < bases.size(); b++) vBase += bases[b];
	vBase /= (LTFLOAT)bases.size();

	int nFrom = m_Nav.FindNearest(vBase, 800.0f, LTFALSE);
	int nTo	  = m_Nav.FindNearest(vTarget, 800.0f, LTFALSE);
	if (nFrom == BOTNAV_INVALID || nTo == BOTNAV_INVALID) return LTFALSE;

	std::vector<int> path;
	if (!m_Nav.FindPath(nFrom, nTo, path)) return LTFALSE;
	path.insert(path.begin(), nFrom);
	if (path.size() < 3) return LTFALSE;

	// Measured in a straight line from the documents (routes wind; a point
	// 1600 units "along the path" can be right under the defenders' noses)
	LTFLOAT fBaseToTarget = HorizDistMgr(vBase, vTarget);
	LTFLOAT fMinD	= LTMIN(1800.0f, fBaseToTarget * 0.45f);
	LTFLOAT fMaxD	= LTMIN(3500.0f, fBaseToTarget * 0.75f);
	LTFLOAT fIdealD	= (fMinD + fMaxD) * 0.5f;
	LTVector vEye	= vTarget + LTVector(0.0f, 50.0f, 0.0f);
	const std::vector<LTFLOAT>* pDanger = m_Nav.GetDanger((int)dwTeam);

	// Candidates: our route through that band, and anything within ~800u of
	// it (the route itself may be the killing zone -- Hydro's central bridge)
	std::vector<int> cands;
	std::vector<LTFLOAT> offRoute;
	for (size_t i = 0; i < path.size(); i++)
	{
		const LTVector& vP = m_Nav.GetNode(path[i]).vPos;
		LTFLOAT d = HorizDistMgr(vP, vTarget);
		if (d < fMinD - 800.0f || d > fMaxD + 800.0f) continue;
		std::vector<int> nearby;
		m_Nav.GetNodesNear(vP, 800.0f, 400.0f, nearby);
		for (size_t k = 0; k < nearby.size(); k++)
		{
			cands.push_back(nearby[k]);
			offRoute.push_back(m_Nav.GetNode(nearby[k]).vPos.Dist(vP));
		}
	}

	// Best: in the band, out of the documents' sight, NOT where we keep dying,
	// not too far off our way
	int nBest = BOTNAV_INVALID;
	LTFLOAT fBestScore = 1.0e30f;
	for (size_t c = 0; c < cands.size(); c++)
	{
		int n = cands[c];
		const BotNavNode& node = m_Nav.GetNode(n);
		if (node.nFlags & (BOTNODE_WATER | BOTNODE_LADDER)) continue;

		LTFLOAT d = HorizDistMgr(node.vPos, vTarget);
		if (d < fMinD || d > fMaxD) continue;

		LTFLOAT fScore = (LTFLOAT)fabs(d - fIdealD) + offRoute[c] * 0.5f;
		if (pDanger && n < (int)pDanger->size()) fScore += 1500.0f * (*pDanger)[n];
		if (fScore >= fBestScore) continue;
		if (CBotNav::IsVisible(vEye, node.vPos + LTVector(0.0f, 40.0f, 0.0f))) fScore += 1000.0f;
		if (fScore < fBestScore) { fBestScore = fScore; nBest = n; }
	}

	if (nBest == BOTNAV_INVALID) return LTFALSE;
	vStage = m_Nav.GetNode(nBest).vPos;
	return LTTRUE;
}

void CBotMgr::UpdatePlans(LTFLOAT fTime)
{
	if (g_pGameServerShell->GetGameType() != COOPERATIVE_ASSAULT) return;

	for (uint32 t = TEAM_1; t <= TEAM_2; t++)
	{
		TeamPlan& p = m_aPlan[t];

		LTVector vTarget;
		if (!GetPushTarget(t, vTarget)) { p.bStageOk = LTFALSE; continue; }

		if (fTime > p.fNextStage || vTarget.DistSqr(p.vTarget) > 32.0f*32.0f)
		{
			LTBOOL bNewTarget = vTarget.DistSqr(p.vTarget) > 32.0f*32.0f || m_MapInfo.GetNumRoutes(t) == 0;
			p.vTarget	 = vTarget;
			p.bStageOk	 = ComputeStage(t, vTarget, p.vStage);
			p.fNextStage = fTime + 90.0f;

			// The ways from our base to it (lanes for the attackers)
			const std::vector<LTVector>& bases = m_aBases[t];
			if (bNewTarget && !bases.empty() && m_MapInfo.IsBuilt())
			{
				LTVector vBase(0.0f, 0.0f, 0.0f);
				for (size_t b = 0; b < bases.size(); b++) vBase += bases[b];
				vBase /= (LTFLOAT)bases.size();
				m_MapInfo.ComputeRoutes(t, m_Nav, vBase, vTarget);
				m_anRouteGen[t]++;
			}
		}

		// Who's walking in with the wave (not defenders, not scuba divers)
		p.nMembers = p.nAtStage = p.nNear = 0;
		for (size_t i = 0; i < m_Bots.size(); i++)
		{
			CBotPlayer* pBot = m_Bots[i].pBot;
			if (!pBot || pBot->GetState() != PS_ALIVE || pBot->GetTeamID() != t || !pBot->IsWaveMember()) continue;
			p.nMembers++;

			LTVector vPos;
			g_pLTServer->GetObjectPos(pBot->m_hObject, &vPos);
			if (p.bStageOk && HorizDistMgr(vPos, p.vStage) < 600.0f) p.nAtStage++;
			if (HorizDistMgr(vPos, vTarget) < 2000.0f) p.nNear++;
		}

		if (!p.bStageOk) continue;

		LTFLOAT fIn = fTime - p.fStateTime;
		if (p.nState == eWaveGather)
		{
			// Go when a good part of the wave is there (or we've waited enough;
			// some are always respawning, healing or fighting on the way)
			int nNeed = LTMAX(2, (p.nMembers * 4 + 9) / 10);
			if (p.nMembers <= 1 || p.nAtStage >= nNeed || (fIn > 25.0f && p.nAtStage >= 1) || fIn > 45.0f)
			{
				p.nState	 = eWavePush;
				p.fStateTime = fTime;
				p.nWaves++;
			}
		}
		else
		{
			// Regroup if the push stalls or the wave got wiped out (give it
			// time to cross the map first)
			if (fIn > 90.0f || (fIn > 40.0f && p.nNear == 0))
			{
				p.nState	 = eWaveGather;
				p.fStateTime = fTime;
			}
		}
	}
}

int CBotMgr::GetWaveState(uint32 dwTeam) const
{
	if (dwTeam != TEAM_1 && dwTeam != TEAM_2) return eWavePush;
	if (!m_aPlan[dwTeam].bStageOk) return eWavePush;
	return m_aPlan[dwTeam].nState;
}

LTBOOL CBotMgr::GetStage(uint32 dwTeam, LTVector& vStage) const
{
	if (dwTeam != TEAM_1 && dwTeam != TEAM_2) return LTFALSE;
	if (!m_aPlan[dwTeam].bStageOk) return LTFALSE;
	vStage = m_aPlan[dwTeam].vStage;
	return LTTRUE;
}

// Bots "use" doors in front of them, like a player pressing the use key.
void CBotMgr::OpenDoorsAhead(CPlayerObj* pPlayer, const LTVector& vPos, const LTVector& vDir)
{
	static HCLASS hDoor = g_pLTServer->GetClass("Door");
	if (!hDoor || !pPlayer) return;

	for (int nPass = 0; nPass < 2; nPass++)
	{
		HOBJECT hObj = LTNULL;
		while ((hObj = (nPass == 0 ? g_pLTServer->GetNextObject(hObj) : g_pLTServer->GetNextInactiveObject(hObj))) != LTNULL)
		{
			if (!g_pLTServer->IsKindOf(g_pLTServer->GetObjectClass(hObj), hDoor)) continue;

			// Doors are big; use the closest point of their box
			LTVector vDoorPos, vDims;
			g_pLTServer->GetObjectPos(hObj, &vDoorPos);
			g_pLTServer->GetObjectDims(hObj, &vDims);

			LTVector vClosest;
			vClosest.x = LTCLAMP(vPos.x, vDoorPos.x - vDims.x, vDoorPos.x + vDims.x);
			vClosest.y = LTCLAMP(vPos.y, vDoorPos.y - vDims.y, vDoorPos.y + vDims.y);
			vClosest.z = LTCLAMP(vPos.z, vDoorPos.z - vDims.z, vDoorPos.z + vDims.z);

			LTVector vTo = vClosest - vPos;
			if (vTo.Mag() > 120.0f) continue;
			vTo.y = 0.0f;
			if (vTo.MagSqr() > 1.0f && vTo.Dot(vDir) < 0.0f) continue;	// Behind us

			SendTriggerMsgToObject(pPlayer, hObj, LTFALSE, "ACTIVATE");
		}
	}
}

LTBOOL CBotMgr::MountVehicle(const LTVector& vPos, CPlayerObj* pPlayer)
{
	static HCLASS hVehicle = g_pLTServer->GetClass("PlayerVehicle");
	if (!hVehicle || !pPlayer) return LTFALSE;

	for (int nPass = 0; nPass < 2; nPass++)
	{
		HOBJECT hObj = LTNULL;
		while ((hObj = (nPass == 0 ? g_pLTServer->GetNextObject(hObj) : g_pLTServer->GetNextInactiveObject(hObj))) != LTNULL)
		{
			if (!g_pLTServer->IsKindOf(g_pLTServer->GetObjectClass(hObj), hVehicle)) continue;

			LTVector vObjPos;
			g_pLTServer->GetObjectPos(hObj, &vObjPos);
			if (vObjPos.DistSqr(vPos) > 100.0f*100.0f) continue;

			SendTriggerMsgToObject(pPlayer, hObj, LTFALSE, "ACTIVATE");
			m_fNextItemRefresh = 0.0f;
			return pPlayer->IsRidingVehicle();
		}
	}

	return LTFALSE;
}

LTBOOL CBotMgr::ActivateIntel(const LTVector& vPos, CPlayerObj* pPlayer)
{
	static HCLASS hIntel = g_pLTServer->GetClass("Intelligence");
	if (!hIntel || !pPlayer) return LTFALSE;

	// Look the object up fresh (never keep handles across frames)...

	for (int nPass = 0; nPass < 2; nPass++)
	{
		HOBJECT hObj = LTNULL;
		while ((hObj = (nPass == 0 ? g_pLTServer->GetNextObject(hObj) : g_pLTServer->GetNextInactiveObject(hObj))) != LTNULL)
		{
			if (!g_pLTServer->IsKindOf(g_pLTServer->GetObjectClass(hObj), hIntel)) continue;

			LTVector vObjPos;
			g_pLTServer->GetObjectPos(hObj, &vObjPos);
			if (vObjPos.DistSqr(vPos) > 16.0f*16.0f) continue;

			Intelligence* pIntel = (Intelligence*)g_pLTServer->HandleToObject(hObj);
			SendTriggerMsgToObject(pPlayer, hObj, LTFALSE, (pIntel && pIntel->IsPhotoOnly()) ? "GADGET" : "ACTIVATE");
			m_fNextItemRefresh = 0.0f;
			m_nIntelTries++;
			if (pPlayer->IsBot())
			{
				char szText[128];
				_snprintf(szText, sizeof(szText) - 1, "Photographing the documents at (%.0f %.0f %.0f)!", vObjPos.x, vObjPos.y, vObjPos.z);
				szText[sizeof(szText) - 1] = '\0';
				LogAction(pPlayer, szText);
			}
			return LTTRUE;
		}
	}

	return LTFALSE;
}

void CBotMgr::UpdateRecording()
{
	// Nothing to record any more: the bots measure the map themselves
}

int CBotMgr::CountHumans()
{
	int nCount = 0;
	const std::vector<CPlayerObj*>& players = GetPlayers();
	for (size_t i = 0; i < players.size(); i++)
	{
		if (!players[i]->IsBot()) nCount++;
	}
	return nCount;
}

// Self-play training runs faster than real time: NOLFBOT_TIMESCALE=4 makes
// the engine's clocks run 4x and its sleeps 4x shorter (see BotSpeed.cpp).
// Never active unless that environment variable is set.
int BotInstallSelfPlayClock(double fScale);

static void ApplySelfPlaySpeed()
{
	const char* sz = getenv("NOLFBOT_TIMESCALE");
	if (!sz) return;
	double fScale = atof(sz);
	if (fScale <= 1.0) return;
	if (fScale > 8.0) fScale = 8.0;

	s_nSpeedHooks = BotInstallSelfPlayClock(fScale);
	g_pLTServer->CPrint("Bots: self-play clock x%.1f (%d hooks)", fScale, s_nSpeedHooks);

	// Let the engine run more frames per real second too, so each simulated
	// step stays as short as in a normal game
	char szCmd[64];
	sprintf(szCmd, "MaxFPS %d", (int)(60.0 * fScale));
	g_pLTServer->RunGameConString(szCmd);
}

// Self-play test runs (dedicated server, no humans): NOLFBOT_SELFPLAY=<bots>
// fills the server anyway.  Off unless that environment variable is set.
static int GetSelfPlayBots()
{
	const char* sz = getenv("NOLFBOT_SELFPLAY");
	return sz ? atoi(sz) : 0;
}

void CBotMgr::UpdateMinPlayers()
{
	int nHumans = CountHumans();
	int nSelfPlay = GetSelfPlayBots();

	int nWant;
	if (nSelfPlay > 0 && nHumans == 0)
	{
		nWant = LTMIN(nSelfPlay, BOT_MAX_BOTS);
	}
	else
	{
		if (m_nMinPlayers <= 0) return;
		if (nHumans == 0) return;	// Nobody to play with yet
		nWant = m_nMinPlayers - nHumans;
		if (nWant < 0) nWant = 0;
	}

	if ((int)m_Bots.size() < nWant)
	{
		AddBot(LTNULL, VariedSkill(), 0);
	}
	else if ((int)m_Bots.size() > nWant)
	{
		RemoveBot((int)m_Bots.size() - 1);
	}
}

// ----------------------------------------------------------------------- //
//	Adding / removing bots
// ----------------------------------------------------------------------- //

// ----------------------------------------------------------------------- //
//	Lobby: joining, pings, chat, skill balancing
// ----------------------------------------------------------------------- //

void CBotMgr::UpdateJoining(LTFLOAT fTime)
{
	if (fTime < m_fNextJoin) return;

	for (size_t i = 0; i < m_Bots.size(); i++)
	{
		if (m_Bots[i].pBot) continue;

		BotSlot& slot = m_Bots[i];
		if (SpawnBot(slot))
		{
			Message("%s joined (%s, skill %d)", slot.szName, CBotPlayer::GetPersonality(slot.nStyle).szName,
				(int)(slot.fSkill * 4.0f + 1.5f));
		}

		m_fNextJoin = fTime + (LTFLOAT)GetRandom(5, 20) / 10.0f;
		return;
	}
}

void CBotMgr::WriteBotPings(HMESSAGEWRITE hWrite)
{
	for (size_t i = 0; i < m_Bots.size(); i++)
	{
		if (!m_Bots[i].pBot) continue;

		// Wobble a little like a real connection
		int nPing = (int)m_Bots[i].nPing + GetRandom(-8, 8);
		if (nPing < 10) nPing = 10;

		g_pLTServer->WriteToMessageWord(hWrite, (uint16)m_Bots[i].nID);
		g_pLTServer->WriteToMessageWord(hWrite, (uint16)nPing);
	}
}

void CBotMgr::BotSay(CPlayerObj* pBot, const char* szText, LTBOOL bTeamOnly)
{
	if (!pBot || !szText) return;

	char szMsg[200];
	_snprintf(szMsg, sizeof(szMsg)-1, "%s: %s", pBot->GetNetName(), szText);
	szMsg[sizeof(szMsg)-1] = '\0';

	if (!bTeamOnly)
	{
		HMESSAGEWRITE hMessage = g_pLTServer->StartMessage(LTNULL, MID_PLAYER_MESSAGE);
		g_pLTServer->WriteToMessageString(hMessage, szMsg);
		g_pLTServer->EndMessage2(hMessage, MESSAGE_NAGGLE);
		return;
	}

	// Team chat: only to human teammates
	const std::vector<CPlayerObj*>& players = GetPlayers();
	for (size_t i = 0; i < players.size(); i++)
	{
		CPlayerObj* pPlayer = players[i];
		if (!pPlayer->GetClient() || pPlayer->GetTeamID() != pBot->GetTeamID()) continue;

		HMESSAGEWRITE hMessage = g_pLTServer->StartMessage(pPlayer->GetClient(), MID_PLAYER_TEAMMESSAGE);
		g_pLTServer->WriteToMessageString(hMessage, szMsg);
		g_pLTServer->EndMessage2(hMessage, MESSAGE_NAGGLE);
	}
}

static const char* s_aTaunts[] =
{
	"gg", "nice try", "too slow", "lol", "got you", "boom", "next!", "sit down",
	"ez", "that's what I thought", "need a hand?", ":)",
};
static const char* s_aDeaths[] =
{
	"argh", "lucky shot", "lag!", "nice one", "ugh", "wow", "how?!", "ok ok",
	"camper...", "not again", "nice shot", "grr",
};

void CBotMgr::OnPlayerKilled(CPlayerObj* pVictim, CPlayerObj* pKiller)
{
	// Learn where our side gets killed (any death counts: pits, explosions...)
	if (pVictim && !IsNavBusy() && m_Nav.GetNumNodes() > 0)
	{
		LTVector vPos;
		g_pLTServer->GetObjectPos(pVictim->m_hObject, &vPos);
		int nTeam = (g_pGameServerShell->GetGameType() == COOPERATIVE_ASSAULT) ? (int)pVictim->GetTeamID() : 0;
		m_Nav.AddDanger(nTeam, vPos, 1.0f);
	}

	// Action log: who killed whom with what
	if (pVictim && pVictim->IsBot())
	{
		if (!pKiller || pKiller == pVictim) LogAction(pVictim, "Died.");
		else
		{
			CPlayerAttachments* pA = pKiller->GetAttachmentsForBots();
			CWeapon* pW = pA ? pA->GetWeapon() : LTNULL;
			WEAPON* pD = pW ? g_pWeaponMgr->GetWeapon(pW->GetId()) : LTNULL;
			char szText[160];
			_snprintf(szText, sizeof(szText) - 1, "Killed by %s (%s).", pKiller->GetNetName(), pD ? pD->szName : "?");
			szText[sizeof(szText) - 1] = '\0';
			LogAction(pVictim, szText);
		}
	}
	if (pKiller && pKiller->IsBot() && pVictim && pVictim != pKiller)
	{
		char szText[160];
		_snprintf(szText, sizeof(szText) - 1, "%s %s.", pVictim->GetTeamID() == pKiller->GetTeamID() && g_pGameServerShell->GetGameType() == COOPERATIVE_ASSAULT ? "Team-killed" : "Killed", pVictim->GetNetName());
		szText[sizeof(szText) - 1] = '\0';
		LogAction(pKiller, szText);
	}

	if (!pVictim || !pKiller || pVictim == pKiller) return;

	// Training rounds: kills per team (not team kills)
	if (pKiller->GetTeamID() != pVictim->GetTeamID() || g_pGameServerShell->GetGameType() != COOPERATIVE_ASSAULT)
	{
		uint32 t = pKiller->GetTeamID();
		if (t <= 2) m_anRoundKills[t]++;
	}

	LTFLOAT fTime = g_pLTServer->GetTime();
	if (fTime < m_fNextChat) return;

	// Only now and then, it's a game not a chat room
	if (pKiller->IsBot() && GetRandom(0, 100) < 12)
	{
		BotSay(pKiller, s_aTaunts[GetRandom(0, (int)(sizeof(s_aTaunts)/sizeof(s_aTaunts[0])) - 1)], LTFALSE);
		m_fNextChat = fTime + (LTFLOAT)GetRandom(8, 20);
	}
	else if (pVictim->IsBot() && GetRandom(0, 100) < 10)
	{
		BotSay(pVictim, s_aDeaths[GetRandom(0, (int)(sizeof(s_aDeaths)/sizeof(s_aDeaths[0])) - 1)], LTFALSE);
		m_fNextChat = fTime + (LTFLOAT)GetRandom(8, 20);
	}
}

// Keep games close: if the humans run away with it the bots slowly get
// better, if the humans are getting crushed the bots ease off.
void CBotMgr::UpdateAutoSkill(LTFLOAT fTime)
{
	if (!m_bAutoSkill || fTime < m_fNextAutoSkill) return;
	m_fNextAutoSkill = fTime + 30.0f;

	int nHumans = 0, nHumanFrags = 0, nBots = 0, nBotFrags = 0;
	const std::vector<CPlayerObj*>& players = GetPlayers();
	for (size_t i = 0; i < players.size(); i++)
	{
		if (players[i]->IsBot()) { nBots++; nBotFrags += players[i]->GetFragCount(); }
		else					 { nHumans++; nHumanFrags += players[i]->GetFragCount(); }
	}
	if (!nHumans || !nBots) return;

	LTFLOAT fDiff = (LTFLOAT)nHumanFrags / nHumans - (LTFLOAT)nBotFrags / nBots;

	LTFLOAT fOld = m_fSkillOffset;
	if (fDiff > 5.0f)		m_fSkillOffset += 0.05f;
	else if (fDiff < -5.0f)	m_fSkillOffset -= 0.05f;
	m_fSkillOffset = LTCLAMP(m_fSkillOffset, -0.3f, 0.3f);
	if (m_fSkillOffset == fOld) return;

	for (size_t i = 0; i < m_Bots.size(); i++)
	{
		if (m_Bots[i].pBot)
		{
			m_Bots[i].pBot->SetSkill(LTCLAMP(m_Bots[i].fSkill + m_fSkillOffset, 0.0f, 1.0f));
		}
	}
}

// Bots added without an explicit skill vary a bit around the default, so a
// server full of them doesn't feel like clones.
LTFLOAT CBotMgr::VariedSkill()
{
	LTFLOAT fSkill = m_fDefaultSkill + ((LTFLOAT)GetRandom(-100, 100) / 100.0f) * 0.2f;
	if (fSkill < 0.0f) fSkill = 0.0f;
	if (fSkill > 1.0f) fSkill = 1.0f;
	return fSkill;
}

uint32 CBotMgr::AllocBotID()
{
	for (uint32 nID = BOT_ID_BASE; nID < BOT_ID_BASE + BOT_MAX_BOTS; nID++)
	{
		LTBOOL bUsed = LTFALSE;
		for (size_t i = 0; i < m_Bots.size(); i++)
		{
			if (m_Bots[i].nID == nID) bUsed = LTTRUE;
		}
		if (!bUsed) return nID;
	}
	return 0;
}

int CBotMgr::FindBot(const char* szName)
{
	for (size_t i = 0; i < m_Bots.size(); i++)
	{
		if (_stricmp(m_Bots[i].szName, szName) == 0) return (int)i;
	}
	return -1;
}

void CBotMgr::PickName(char* szName, int nLen)
{
	int nStart = GetRandom(0, s_nBotNames-1);
	for (int i = 0; i < s_nBotNames; i++)
	{
		const char* pName = s_aBotNames[(nStart + i) % s_nBotNames];
		if (FindBot(pName) < 0)
		{
			strncpy(szName, pName, nLen-1);
			szName[nLen-1] = '\0';
			return;
		}
	}

	_snprintf(szName, nLen-1, "Bot%d", (int)m_Bots.size() + 1);
	szName[nLen-1] = '\0';
}

void CBotMgr::PickModel(char* szModel, int nLen)
{
	// Same list the player setup menu offers: chars\models\multi\<model>_<style>_multi.abc

	if (m_Models.empty())
	{
		FileEntry* pFiles = g_pLTServer->GetFileList("chars\\models\\multi");
		for (FileEntry* ptr = pFiles; ptr; ptr = ptr->m_pNext)
		{
			if (ptr->m_Type != TYPE_FILE) continue;

			char szTemp[128];
			SAFE_STRCPY(szTemp, ptr->m_pBaseFilename);
			_strupr(szTemp);

			char* pSuffix = strstr(szTemp, "_MULTI.ABC");
			if (!pSuffix) continue;
			*pSuffix = '\0';

			char szTag[128];
			SAFE_STRCPY(szTag, szTemp);
			_strlwr(szTag);
			if (!g_pModelButeMgr->GetButeMgr()->Exist(szTag)) continue;

			// Scuba gear on dry land looks silly (and holds weapons badly)
			if (strstr(szTemp, "SCUBA")) continue;

			char* pSep = strchr(szTemp, '_');
			if (!pSep) continue;
			*pSep = ',';

			m_Models.push_back(szTemp);
		}
		if (pFiles) g_pLTServer->FreeFileList(pFiles);

		if (m_Models.empty()) m_Models.push_back("HERO,ACTION");
	}

	// Only a few different models: every model/skin is more texture memory
	// for the (old) renderer, and running out makes models vanish.
	if ((int)m_SessionModels.size() != m_nModelVariety)
	{
		m_SessionModels.clear();
		std::vector<std::string> pool = m_Models;
		while ((int)m_SessionModels.size() < m_nModelVariety && !pool.empty())
		{
			int nPick = GetRandom(0, (int)pool.size()-1);
			m_SessionModels.push_back(pool[nPick]);
			pool.erase(pool.begin() + nPick);
		}
	}

	const std::string& sModel = m_SessionModels[GetRandom(0, (int)m_SessionModels.size()-1)];
	strncpy(szModel, sModel.c_str(), nLen-1);
	szModel[nLen-1] = '\0';
}

uint32 CBotMgr::PickTeam()
{
	if (g_pGameServerShell->GetGameType() != COOPERATIVE_ASSAULT) return 0;

	CTeam* pTeam = g_pGameServerShell->GetTeamMgr()->GetTeamWithLeastPlayers(TRUE);
	if (pTeam) return pTeam->GetID();

	return (GetRandom(0, 1) == 1) ? TEAM_2 : TEAM_1;
}

LTBOOL CBotMgr::AddBot(const char* szName, LTFLOAT fSkill, uint32 dwTeam, int nStyle)
{
	if (g_pGameServerShell->GetGameType() == SINGLE)
	{
		Message("Bots only work in multiplayer games");
		return LTFALSE;
	}

	if ((int)m_Bots.size() >= BOT_MAX_BOTS)
	{
		Message("Too many bots");
		return LTFALSE;
	}

	BotSlot slot;
	memset(&slot, 0, sizeof(slot));

	if (szName && szName[0])
	{
		SAFE_STRCPY(slot.szName, szName);
	}
	else
	{
		PickName(slot.szName, sizeof(slot.szName));
	}

	PickModel(slot.szModel, sizeof(slot.szModel));
	slot.fSkill = fSkill;
	slot.dwTeam = dwTeam;
	slot.nStyle = (nStyle >= 0) ? nStyle : CBotPlayer::PickRandomStyle(g_pGameServerShell->GetGameType() == COOPERATIVE_ASSAULT);
	slot.nID	= AllocBotID();
	slot.pBot	= LTNULL;
	slot.nPing	= (uint16)GetRandom(35, 120);


	// Joins shortly (UpdateJoining), like somebody connecting
	m_Bots.push_back(slot);
	return LTTRUE;
}

LTBOOL CBotMgr::SpawnBot(BotSlot& slot)
{
	if (!m_bWorldLoaded) return LTFALSE;

	HCLASS hClass = g_pLTServer->GetClass("CBotPlayer");
	if (!hClass) return LTFALSE;

	ObjectCreateStruct theStruct;
	INIT_OBJECTCREATESTRUCT(theStruct);
	theStruct.m_Rotation.Init();
	theStruct.m_Pos.Init();
	theStruct.m_Flags = 0;

	CBotPlayer* pBot = (CBotPlayer*)g_pLTServer->CreateObject(hClass, &theStruct);
	if (!pBot) return LTFALSE;

	// Team (H.A.R.M. vs UNITY only)...

	if (g_pGameServerShell->GetGameType() == COOPERATIVE_ASSAULT)
	{
		if (slot.dwTeam != TEAM_1 && slot.dwTeam != TEAM_2)
		{
			slot.dwTeam = PickTeam();
		}
		g_pGameServerShell->GetTeamMgr()->AddPlayer(slot.dwTeam, slot.nID);
	}
	else
	{
		slot.dwTeam = 0;
	}

	pBot->PreMultiplayerInit();
	pBot->BotInit(slot.nID, slot.szName, slot.szModel, slot.dwTeam);
	pBot->SetSkill(LTCLAMP(slot.fSkill + m_fSkillOffset, 0.0f, 1.0f));
	pBot->SetStyle(slot.nStyle);

	pBot->UpdateTeamID();

	slot.pBot = pBot;

	// Show up on everybody's scoreboard, then jump in...

	g_pGameServerShell->SendPlayerInfoMsgToClients(LTNULL, pBot, LTTRUE);
	pBot->Respawn();

	m_fPlayersTime = -1.0f;
	g_pGameServerShell->SetUpdateGameServ();

	return LTTRUE;
}

void CBotMgr::RemoveBot(int nIndex)
{
	if (nIndex < 0 || nIndex >= (int)m_Bots.size()) return;

	BotSlot& slot = m_Bots[nIndex];

	HMESSAGEWRITE hWrite = g_pLTServer->StartMessage(LTNULL, MID_PLAYER_REMOVED);
	g_pLTServer->WriteToMessageFloat(hWrite, (float)slot.nID);
	g_pLTServer->EndMessage(hWrite);

	if (g_pGameServerShell->GetGameType() == COOPERATIVE_ASSAULT)
	{
		g_pGameServerShell->GetTeamMgr()->RemovePlayer(slot.nID);
	}

	if (slot.pBot)
	{
		g_pLTServer->RemoveObject(slot.pBot->m_hObject);
	}

	m_Bots.erase(m_Bots.begin() + nIndex);
	m_fPlayersTime = -1.0f;
	g_pGameServerShell->SetUpdateGameServ();
}

void CBotMgr::RemoveAllBots()
{
	while (!m_Bots.empty())
	{
		RemoveBot((int)m_Bots.size() - 1);
	}
}

void CBotMgr::SendBotInfos(HCLIENT hClient)
{
	for (size_t i = 0; i < m_Bots.size(); i++)
	{
		if (m_Bots[i].pBot)
		{
			g_pGameServerShell->SendPlayerInfoMsgToClients(hClient, m_Bots[i].pBot, LTFALSE);
		}
	}
}

// ----------------------------------------------------------------------- //
//	Console commands
// ----------------------------------------------------------------------- //

LTBOOL CBotMgr::HandleCommand(const char* szCmd)
{
	if (!szCmd) return LTFALSE;

	char szBuf[256];
	SAFE_STRCPY(szBuf, szCmd);

	char* aArgs[8];
	int nArgs = 0;
	char* pTok = strtok(szBuf, " \t");
	while (pTok && nArgs < 8)
	{
		aArgs[nArgs++] = pTok;
		pTok = strtok(LTNULL, " \t");
	}
	if (nArgs == 0) return LTFALSE;

	const char* szName = aArgs[0];

	if (_stricmp(szName, "addbot") == 0)
	{
		// addbot [name] [skill 1-5] [team 1|2] [style]   (any order after the name;
		// a word that's a style name is the style, "random" means random name)
		const char* pBotName = LTNULL;
		LTFLOAT fSkill = VariedSkill();
		uint32 dwTeam = 0;
		int nStyle = -1;
		LTBOOL bHaveSkill = LTFALSE;

		for (int a = 1; a < nArgs; a++)
		{
			int nStyleArg = CBotPlayer::FindStyle(aArgs[a]);
			if (nStyleArg >= 0)						nStyle = nStyleArg;
			else if (isdigit((unsigned char)aArgs[a][0]))
			{
				if (!bHaveSkill)	{ fSkill = SkillFromLevel(atoi(aArgs[a])); bHaveSkill = LTTRUE; }
				else				dwTeam = (uint32)atoi(aArgs[a]);
			}
			else if (_stricmp(aArgs[a], "random") != 0 && !pBotName) pBotName = aArgs[a];
		}

		if (pBotName && FindBot(pBotName) >= 0)
		{
			Message("There already is a bot called %s", pBotName);
		}
		else
		{
			AddBot(pBotName, fSkill, dwTeam, nStyle);
		}
		return LTTRUE;
	}

	if (_stricmp(szName, "removebot") == 0 || _stricmp(szName, "kickbot") == 0)
	{
		if (nArgs > 1 && _stricmp(aArgs[1], "all") == 0)
		{
			m_nMinPlayers = 0;
			RemoveAllBots();
			Message("All bots removed");
		}
		else if (nArgs > 1)
		{
			int nIndex = FindBot(aArgs[1]);
			if (nIndex >= 0)	RemoveBot(nIndex);
			else				Message("No bot called %s", aArgs[1]);
		}
		else
		{
			RemoveBot((int)m_Bots.size() - 1);
		}
		return LTTRUE;
	}

	if (_stricmp(szName, "bot_skill") == 0)
	{
		if (nArgs > 1)
		{
			m_fDefaultSkill = SkillFromLevel(atoi(aArgs[1]));
			for (size_t i = 0; i < m_Bots.size(); i++)
			{
				m_Bots[i].fSkill = m_fDefaultSkill;
				if (m_Bots[i].pBot) m_Bots[i].pBot->SetSkill(m_fDefaultSkill);
			}
		}
		Message("Bot skill is %d (1-5)", (int)(m_fDefaultSkill * 4.0f + 1.5f));
		return LTTRUE;
	}

	if (_stricmp(szName, "bot_minplayers") == 0)
	{
		if (nArgs > 1) m_nMinPlayers = atoi(aArgs[1]);
		Message("Keeping at least %d players", m_nMinPlayers);
		return LTTRUE;
	}

	if (_stricmp(szName, "bot_record") == 0)
	{
		Message("Not needed any more: bots measure the map from its geometry (%d waypoints). bot_autogen rescans it.", m_Nav.GetNumNodes());
		return LTTRUE;
	}

	if (_stricmp(szName, "bot_learn") == 0)
	{
		Message("Not needed any more: bots measure the map from its geometry");
		return LTTRUE;
	}

	if (_stricmp(szName, "bot_save") == 0)
	{
		if (m_Nav.Save(g_pGameServerShell->GetCurLevel()))
			Message("Saved %d waypoints", m_Nav.GetNumNodes());
		else
			Message("Couldn't save waypoints");
		return LTTRUE;
	}

	if (_stricmp(szName, "bot_autogen") == 0)
	{
		RestartNav(LTTRUE);
		Message("Studying the map again...");
		return LTTRUE;
	}

	if (_stricmp(szName, "bot_clearnav") == 0)
	{
		DropNav();
		Message("Waypoints cleared");
		return LTTRUE;
	}

	if (_stricmp(szName, "bot_help") == 0 || _stricmp(szName, "bots") == 0)
	{
		Message("!addbot [name] [skill 1-5] [team] [rusher|camper|tactician|scavenger|teamplayer|casual]");
		Message("!removebot [name|all]  !bot_skill 1-5  !bot_list");
		Message("!bot_minplayers n  !bot_record 0|1  !bot_save  !bot_autogen  !bot_info");
		return LTTRUE;
	}

	if (_stricmp(szName, "set") == 0)
	{
		if (nArgs >= 3)
		{
			SetServerVar(aArgs[1], aArgs[2]);
			Message("%s = %s", aArgs[1], aArgs[2]);
		}
		else
		{
			Message("Usage: set <variable> <value>  (e.g. set RunSpeed 1.5)");
		}
		return LTTRUE;
	}

	if (_stricmp(szName, "default_weapon") == 0)
	{
		// default_weapon <name|none>: what everybody spawns with
		if (nArgs < 2) { Message("Usage: default_weapon <weapon name|none>"); return LTTRUE; }

		if (_stricmp(aArgs[1], "none") == 0)
		{
			SetServerVar("NetDefaultWeapon", "0");
			Message("No default weapon");
			return LTTRUE;
		}

		// The rest of the line is the name ("Walther SMG"); match the start, ignoring case
		std::string sWant;
		for (int a = 1; a < nArgs; a++) { if (a > 1) sWant += " "; sWant += aArgs[a]; }

		for (int i = 0; i < g_pWeaponMgr->GetNumWeapons(); i++)
		{
			WEAPON* pData = g_pWeaponMgr->GetWeapon(i);
			if (!pData || pData->IsAGadget()) continue;
			if (_strnicmp(pData->szName, sWant.c_str(), sWant.size()) != 0) continue;

			int nCommand = g_pWeaponMgr->GetCommandId(pData->nId);
			if (nCommand < 0) continue;

			char szValue[16];
			sprintf(szValue, "%d", nCommand);
			SetServerVar("NetDefaultWeapon", szValue);
			Message("Default weapon: %s (from your next respawn)", pData->szName);
			return LTTRUE;
		}

		Message("No weapon called %s", sWant.c_str());
		return LTTRUE;
	}

	if (_stricmp(szName, "bot_models") == 0)
	{
		if (nArgs > 1) m_nModelVariety = LTCLAMP(atoi(aArgs[1]), 1, 20);
		m_SessionModels.clear();
		Message("Bots use %d different model(s) (affects newly joining bots)", m_nModelVariety);
		return LTTRUE;
	}

	if (_stricmp(szName, "bot_autoskill") == 0)
	{
		m_bAutoSkill = (nArgs > 1) ? (atoi(aArgs[1]) != 0) : !m_bAutoSkill;
		if (!m_bAutoSkill) m_fSkillOffset = 0.0f;
		Message("Automatic bot skill balancing %s", m_bAutoSkill ? "ON" : "OFF");
		return LTTRUE;
	}

	if (_stricmp(szName, "bot_list") == 0)
	{
		for (size_t i = 0; i < m_Bots.size(); i++)
		{
			const BotSlot& slot = m_Bots[i];
			Message("%s: %s, skill %d%s", slot.szName, CBotPlayer::GetPersonality(slot.nStyle).szName,
				(int)(slot.fSkill * 4.0f + 1.5f), slot.dwTeam ? (slot.dwTeam == 1 ? ", team 1" : ", team 2") : "");
		}
		if (m_Bots.empty()) Message("No bots");
		return LTTRUE;
	}

	if (_stricmp(szName, "bot_info") == 0)
	{
		Message("%d bots, %d waypoints, %d items, recording %s, minplayers %d",
			(int)m_Bots.size(), m_Nav.GetNumNodes(), (int)m_Items.size(),
			m_bRecording ? "on" : "off", m_nMinPlayers);
		return LTTRUE;
	}

	return LTFALSE;
}
