// ----------------------------------------------------------------------- //
//
// MODULE  : BotGenome.h
//
// PURPOSE : The bots' tunable fighting parameters, trained by self-play on
//			 Hydro (linux/hydro_evolve.py).  Each gene is a multiplier or a
//			 probability with bounds.  Human-like floors (reaction time, aim
//			 error) are applied where the genes are used, so training can make
//			 them smarter fighters but not aimbots.
//
// ----------------------------------------------------------------------- //

#ifndef __BOT_GENOME_H__
#define __BOT_GENOME_H__

enum BotGene
{
	eGeneReact = 0,		// x reaction time to a new enemy / being shot
	eGeneAimErr,		// x aim error
	eGeneAimMove,		// x extra aim error while we move
	eGeneTurn,			// x aim turn speed
	eGeneStrafe,		// x time between strafe direction changes
	eGeneJump,			// x bunny-hop rate while running
	eGeneRange,			// x preferred fighting distance
	eGeneRetreat,		// x health fraction at which we break off to heal
	eGeneChase,			// x how long we chase someone we lost sight of
	eGeneAggro,			// x how hard we close in / go after gunfire
	eGeneBurst,			// x automatic-fire burst length
	eGeneHead,			// x head-shot attempt chance
	eGeneEffRange,		// x "effective range" we're willing to shoot at
	eGeneGreed,			// x value of pickups
	eGeneHeal,			// x value of health / armor
	eGeneRole,			// chance an attacker picks the team objective
	eGeneCover,			// chance to duck into cover while reloading / hurt
	eGeneFov,			// x field-of-view awareness
	eGeneDodge,			// x dodge-jump rate in fights
	eGeneLead,			// x how far ahead we lead moving targets (projectiles)
	eGeneWaves,			// team behaviours, on (>= 0.5) / off
	eGeneDanger,
	eGeneCallouts,
	eGeneDiscipline,
	eGeneFocus,
	eGeneRecall,
	kNumGenes
};

struct BotGeneInfo
{
	const char*	szName;
	float		fDefault, fMin, fMax;
};

extern const BotGeneInfo g_aGeneInfo[kNumGenes];

struct BotGenome
{
	float	v[kNumGenes];
	char	szId[32];

	void	SetDefaults();
	bool	Load(const char* szFile);		// "name value" lines; missing genes keep their defaults
	float	operator[](int i) const { return v[i]; }
	bool	On(int i) const { return v[i] >= 0.5f; }
};

#endif // __BOT_GENOME_H__
