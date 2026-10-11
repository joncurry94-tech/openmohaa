/*
===========================================================================
Copyright (C) 2024 the OpenMoHAA team

This file is part of OpenMoHAA source code.

OpenMoHAA source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

OpenMoHAA source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with OpenMoHAA source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/

// DESCRIPTION:
// CGM buffer parser

#include "cg_local.h"
#include "cg_parsemsg.h"
#include "cg_specialfx.h"
#include "cg_radar.h"

#include "str.h"

extern int current_entity_number;

typedef struct {
    vec3_t   i_vBarrel;
    vec3_t   i_vStart;
    vec3_t  *i_vEnd;
    int      i_iNumBullets;
    qboolean iLarge;
    float    alpha;
    int      iTracerVisible;
    qboolean bIgnoreEntities;
} bullet_tracer_t;

// HZM coop [bug-3477, tester log 2026-10-10]: 32 tracer volleys per CLIENT FRAME was hit 778 times in one minute on
// Omaha (m3l1a) - at 19 fps a frame carries ~50 ms of beach MG fire - and every miss both dropped the tracer and
// printed a console line (a flushed log write each). 128 volleys still fit MAX_BULLET_TRACE_BULLETS easily.
#define MAX_BULLET_TRACERS       128
#define MAX_BULLET_TRACE_BULLETS 1024
#define MAX_IMPACTS              64
static constexpr unsigned int BULLET_TRAVEL_DIST = 9216;

static int             bullet_tracers_count;
static int             bullet_tracer_bullets_count;
static int             wall_impact_count;
static int             flesh_impact_count;
static bullet_tracer_t bullet_tracers[MAX_BULLET_TRACERS];
static vec3_t          bullet_tracer_bullets[MAX_BULLET_TRACE_BULLETS];
static vec3_t          wall_impact_pos[MAX_IMPACTS];
static vec3_t          wall_impact_norm[MAX_IMPACTS];
static int             wall_impact_large[MAX_IMPACTS];
static int             wall_impact_type[MAX_IMPACTS];
static vec3_t          flesh_impact_pos[MAX_IMPACTS];
static vec3_t          flesh_impact_norm[MAX_IMPACTS];
static int             flesh_impact_large[MAX_IMPACTS];

void CG_MakeBulletHoleSound(const vec3_t i_vPos, const vec3_t i_vNorm, int iLarge, trace_t *pPreTrace)
{
    int     iSurfType;
    float   fVolume;
    str     sSoundName;
    vec3_t  vFrom, vDest;
    trace_t trace;

    if (pPreTrace) {
        trace = *pPreTrace;
    } else {
        VectorMA(i_vPos, 2.0f, i_vNorm, vFrom);
        VectorMA(i_vPos, -4.0f, i_vNorm, vDest);
        CG_Trace(
            &trace, vFrom, vec_zero, vec_zero, vDest, ENTITYNUM_NONE, MASK_SHOT, qfalse, qtrue, "CG_MakeBulletHole"
        );
    }

    iSurfType = trace.surfaceFlags & MASK_SURF_TYPE;

    // HZM coop [user 2026-08-19] RICOCHET WHINES "to coincide with the changes we made to
    // zings": hard surfaces (metal/rock/grill) chance-spin a round off with a synthesized
    // descending whine (retail ships ZERO ricochet audio - pak-verified). Positional at the
    // impact, rate-limited so a raking burst spins one round, not ten.
    {
        static cvar_t *pRic = NULL;
        static int s_iNextRicTime = 0;
        if (!pRic) {
            pRic = cgi.Cvar_Get("coop_ricochet", "1", CVAR_ARCHIVE);
        }
        if (pRic->integer && cg.time >= s_iNextRicTime && (rand() % 100) < 30
            && (iSurfType == SURF_METAL || iSurfType == SURF_ROCK || iSurfType == SURF_GRILL)) {
            s_iNextRicTime = cg.time + 260;
            commandManager.PlaySound(
                "snd_b_ricochet", i_vPos, -1, 0.9f, -1.0f, 0.92f + crandom() * 0.12f, 1);
        }
    }
    if (trace.contents & CONTENTS_WATER) {
        iSurfType = SURF_PUDDLE;
    }

    if (trace.fraction == 1) {
        // no reason to make a sound if it nothing was hit
        return;
    }

    if ((trace.surfaceFlags & SURF_SKY) || !CG_CheckMakeMarkOnEntity(trace.entityNum)) {
        return;
    }

    VectorAdd(trace.endpos, trace.plane.normal, vFrom);
    sSoundName = "snd_bh_";

    switch (iSurfType) {
    case SURF_FOLIAGE:
        sSoundName += "foliage";
        break;
    case SURF_SNOW:
        sSoundName += "snow";
        break;
    case SURF_CARPET:
        sSoundName += "carpet";
        break;
    case SURF_SAND:
        sSoundName += "sand";
        break;
    case SURF_PUDDLE:
        sSoundName += "puddle";
        break;
    case SURF_GLASS:
        sSoundName += "glass";
        break;
    case SURF_GRAVEL:
        sSoundName += "gravel";
        break;
    case SURF_MUD:
        sSoundName += "mud";
        break;
    case SURF_DIRT:
        sSoundName += "dirt";
        break;
    case SURF_GRILL:
        sSoundName += "grill";
        break;
    case SURF_GRASS:
        sSoundName += "grass";
        break;
    case SURF_ROCK:
        sSoundName += "stone";
        break;
    case SURF_PAPER:
        sSoundName += "paper";
        break;
    case SURF_WOOD:
        sSoundName += "wood";
        break;
    case SURF_METAL:
        sSoundName += "metal";
        break;
    default:
        sSoundName += "stone";
        break;
    }

    if (iLarge) {
        fVolume = 1.0f;
    } else {
        fVolume = 0.75f;
    }

    commandManager.PlaySound(sSoundName, vFrom, -1, fVolume, -1.f, -1.f, 1);
}

static void CG_MakeBulletHoleType(
    const vec3_t i_vPos, const vec3_t i_vNorm, int iLarge, int iEffectNum, float fRadius, qboolean bMakeSound
)
{
    if (bMakeSound) {
        str   sBulletHole = "snd_bh_";
        float fVolume;

        switch (iEffectNum) {
        case SFX_BHIT_PAPER_LITE:
            sBulletHole += "paper";
            break;
        case SFX_BHIT_WOOD_LITE:
            sBulletHole += "wood";
            break;
        case SFX_BHIT_METAL_LITE:
            sBulletHole += "metal";
            break;
        case SFX_BHIT_STONE_LITE:
            sBulletHole += "stone";
            break;
        case SFX_BHIT_DIRT_LITE:
            sBulletHole += "dirt";
            break;
        case SFX_BHIT_GRILL_LITE:
            sBulletHole += "grill";
            break;
        case SFX_BHIT_GRASS_LITE:
            sBulletHole += "grass";
            break;
        case SFX_BHIT_MUD_LITE:
            sBulletHole += "mud";
            break;
        case SFX_BHIT_PUDDLE_LITE:
            sBulletHole += "puddle";
            break;
        case SFX_BHIT_GLASS_LITE:
            sBulletHole += "glass";
            break;
        case SFX_BHIT_GRAVEL_LITE:
            sBulletHole += "gravel";
            break;
        case SFX_BHIT_SAND_LITE:
            sBulletHole += "sand";
            break;
        case SFX_BHIT_FOLIAGE_LITE:
            sBulletHole += "foliage";
            break;
        case SFX_BHIT_SNOW_LITE:
            sBulletHole += "snow";
            break;
        case SFX_BHIT_CARPET_LITE:
            sBulletHole += "carpet";
            break;
        default:
            sBulletHole += "stone";
            break;
        }

        if (iLarge) {
            fVolume = 1.0f;
        } else {
            fVolume = 0.75f;
        }

        commandManager.PlaySound(sBulletHole, i_vPos, -1, fVolume, -1.f, -1.f, 1);
    }

    if (iLarge) {
        iEffectNum++;
    }

    sfxManager.MakeEffect_Normal(iEffectNum, i_vPos, i_vNorm);
}

static void
CG_MakeBulletHole(const vec3_t i_vPos, const vec3_t i_vNorm, int iLarge, trace_t *pPreTrace, qboolean bMakeSound)
{
    int     iSurfType;
    int     iEffect;
    float   fRadius;
    str     sEffectModel;
    str     sBulletHole;
    vec3_t  vFrom, vDest;
    trace_t trace;

    if (pPreTrace) {
        trace = *pPreTrace;
    } else {
        VectorMA(i_vPos, 2.0f, i_vNorm, vFrom);
        VectorMA(i_vPos, -4.0f, i_vNorm, vDest);
        CG_Trace(
            &trace, vFrom, vec_zero, vec_zero, vDest, ENTITYNUM_NONE, MASK_SHOT, qfalse, qtrue, "CG_MakeBulletHole"
        );
    }

    iSurfType = trace.surfaceFlags & MASK_SURF_TYPE;
    if (trace.contents & CONTENTS_WATER) {
        iSurfType = SURF_PUDDLE;
    }

    if (trace.fraction == 1.0f || trace.startsolid) {
        return;
    }

    if (trace.surfaceFlags & SURF_SKY) {
        // ignore sky surfaces
        return;
    }

    VectorAdd(trace.endpos, trace.plane.normal, vFrom);
    fRadius     = 2.0f;
    sBulletHole = "bhole_";

    switch (iSurfType) {
    case SURF_FOLIAGE:
        fRadius = 0.f;
        iEffect = SFX_BHIT_FOLIAGE_LITE;
        break;
    case SURF_SNOW:
        sBulletHole += "snow";
        iEffect = SFX_BHIT_SNOW_LITE;
        break;
    case SURF_CARPET:
        sBulletHole += "carpet";
        iEffect = SFX_BHIT_CARPET_LITE;
        break;
    case SURF_SAND:
        fRadius = 0.f;
        iEffect = SFX_BHIT_SAND_LITE;
        break;
    case SURF_PUDDLE:
        fRadius = 0.f;
        iEffect = SFX_BHIT_PUDDLE_LITE;
        break;
    case SURF_GLASS:
        sBulletHole += "glass";
        iEffect = SFX_BHIT_GLASS_LITE;
        break;
    case SURF_GRAVEL:
        fRadius = 0.f;
        iEffect = SFX_BHIT_GRAVEL_LITE;
        break;
    case SURF_MUD:
        sBulletHole += "mud";
        iEffect = SFX_BHIT_MUD_LITE;
        break;
    case SURF_DIRT:
        sBulletHole += "dirt";
        iEffect = SFX_BHIT_DIRT_LITE;
        break;
    case SURF_GRILL:
        sBulletHole += "grill";
        iEffect = SFX_BHIT_GRILL_LITE;
        break;
    case SURF_GRASS:
        sBulletHole += "grass";
        iEffect = SFX_BHIT_GRASS_LITE;
        break;
    case SURF_ROCK:
        sBulletHole += "stone";
        iEffect = SFX_BHIT_STONE_LITE;
        break;
    case SURF_PAPER:
        sBulletHole += "paper";
        iEffect = SFX_BHIT_PAPER_LITE;
        break;
    case SURF_WOOD:
        sBulletHole += "wood";
        iEffect = SFX_BHIT_WOOD_LITE;
        break;
    case SURF_METAL:
        fRadius -= 0.25f;
        sBulletHole += "metal";
        iEffect = SFX_BHIT_METAL_LITE;
        break;
    default:
        sBulletHole += "stone";
        iEffect = SFX_BHIT_STONE_LITE;
        break;
    }

    if (fRadius && CG_CheckMakeMarkOnEntity(trace.entityNum)) {
        fRadius *= 1.f + crandom() * 0.2f;

        CG_ImpactMarkSimple(
            cgi.R_RegisterShader(sBulletHole.c_str()),
            trace.endpos,
            trace.plane.normal,
            0.f,
            fRadius,
            1.f,
            1.f,
            1.f,
            1.f,
            qfalse,
            qfalse,
            qtrue,
            qfalse
        );
    }

    CG_MakeBulletHoleType(vFrom, i_vNorm, iLarge, iEffect, fRadius, bMakeSound);
}

static void CG_MakeBubbleTrail(const vec3_t i_vStart, const vec3_t i_vEnd, int iLarge, float alpha = 1.0f)
{
    vec3_t vDir;
    vec3_t vPos;
    float  fDist;
    float  fMove;

    VectorSubtract(i_vEnd, i_vStart, vDir);
    fDist = VectorNormalize(vDir);
    VectorCopy(i_vStart, vPos);

    fMove = random() * 8.f * alpha;
    fDist -= fMove;
    while (fDist > 0.f) {
        VectorMA(vPos, fMove, vDir, vPos);
        sfxManager.MakeEffect_Angles(SFX_WATER_TRAIL_BUBBLE, vPos, vec_zero);

        fMove = 16.f + crandom() * 8.f * alpha;
        fDist -= fMove;
    }
}

static void CG_BulletTracerEffect(const vec3_t i_vStart, const vec3_t i_vEnd, int iLarge, float alpha)
{
    int    iLife;
    int    iTracerLength;
    float  fLength, fDist;
    vec4_t fTracerColor;
    vec3_t vDir, vNewStart;
    float  alphaMult;
    float  scale;

    iTracerLength = 450.0f + random() * 150.f;
    VectorSubtract(i_vEnd, i_vStart, vDir);
    fLength = VectorNormalize(vDir);

    if (iLarge > 1) {
        alphaMult = random() * 0.2f + 0.9f;
        scale     = iLarge * 2;
    } else {
        alphaMult = 1;
        scale     = 1;
    }

    fDist = 450.0f + random() * 150.f;

    if (fLength < fDist + 150.f) {
        float fFrac;

        if (fLength < 150.f) {
            return;
        }

        fFrac         = fLength / (fDist + 150.0);
        iTracerLength = (int)((float)iTracerLength * fFrac + 0.5f);
        fDist *= fFrac;
    }

    iLife = (int)((fLength - fDist) / 12000.f * 1000.f / (alpha * alphaMult));
    if (iLife < 20) {
        iLife = 20;
    }

    VectorMA(i_vStart, fDist, vDir, vNewStart);
    fTracerColor[0] = 1.f;
    fTracerColor[1] = 1.f;
    fTracerColor[2] = 1.f;
    fTracerColor[3] = 1.f;

    // HZM coop - TRACER GLOW: thicken the (white) tracer beam so it reads as a hot glowing round and the
    // bloom post-FX catches it harder. cg_tracerGlow scales the beam width (1 = vanilla, 0 disables boost).
    {
        cvar_t *pGlow = cgi.Cvar_Get("cg_tracerGlow", "1.8", CVAR_ARCHIVE);
        if (pGlow && pGlow->value > 0.0f) { scale *= pGlow->value; }
    }

    CG_CreateBeam(
        vNewStart,
        vec_zero,
        0,
        1,
        1.0,
        scale,
        BEAM_INVERTED,
        1000.0,
        iLife,
        qtrue,
        i_vEnd,
        0,
        0,
        0,
        1,
        0,
        "tracer",
        fTracerColor,
        0,
        0.0,
        iTracerLength,
        1.0,
        0,
        "tracereffect"
    );
}

static void CG_MakeBulletTracerInternal(
    const vec3_t  i_vBarrel,
    const vec3_t  i_vStart,
    const vec3_t *i_vEnd,
    int           i_iNumBullets,
    qboolean      iLarge,
    int           iTracerVisible,
    qboolean      bIgnoreEntities,
    float         alpha
)
{
    vec3_t   vPos;
    int      iBullet;
    int      iContinueCount;
    int      iDist;
    int      iHeadDist;
    int      iTravelDist;
    float    fLen, fDist;
    trace_t  trace;
    qboolean bStartInWater, bInWater;
    qboolean bBulletDone;
    qboolean bMadeTracer;
    vec3_t   vDir;
    vec3_t   vTrailStart;
    vec3_t   vTraceStart;
    vec3_t   vTraceEnd;
    vec3_t   vTmp;
    int      iNumImpacts;
    trace_t  tImpacts[128];
    float    fImpSndDistRA;
    float    fImpSndDistLA;
    float    fImpSndDistRB;
    float    fImpSndDistLB;
    int      iImpSndIndexRA;
    int      iImpSndIndexLA;
    int      iImpSndIndexRB;
    int      iImpSndIndexLB;
    float    fVolume;
    float    fPitch;
    float    fZingDistA, fZingDistB, fZingDistC;
    vec3_t   vZingPosA, vZingPosB, vZingPosC;
    float    swMuzzle = 0.0f, swImpact = 0.0f; // HZM coop - smoke-whip spawn chances (0..1)

    fZingDistB  = 9999.0;
    fZingDistA  = 9999.0;
    fZingDistC  = 9999.0;
    iNumImpacts = 0;

    // [HZM 07-28, bug-1200] muzzle 0.35 -> 0.15 and barrel 0.6 -> 0.25. At the old rates ~72% of
    // shots produced SOME smoke (0.35 muzzle puff + 0.6 back-wisp, rolled independently), which
    // stacked under sustained fire and read as a permanent cloud at the barrel. These are all
    // CVAR_ARCHIVE and re-read every volley, so they can be tuned live from the console.
    // HZM coop - SMOKE WHIPS: read the live cvars once. coop_smokeWhip is the master on/off;
    // coop_smokeWhipMuzzle / coop_smokeWhipImpact are per-event spawn CHANCES for a subtle lingering
    // gun-smoke wisp (SFX_COOP_GUNSMOKE) at the barrel / at bullet impacts (0 = off, 1 = every event).
    {
        cvar_t *pSW  = cgi.Cvar_Get("coop_smokeWhip",       "1",    CVAR_ARCHIVE);
        cvar_t *pSWM = cgi.Cvar_Get("coop_smokeWhipMuzzle", "0.15", CVAR_ARCHIVE);
        // [HZM 07-28, bug-1176 round 2] impact chance 0.35 -> 0.12. Halving the puff's SIZE in
        // coop_gunsmoke.tik was not enough on its own: this fires ON TOP of the engine's own
        // per-surface impact effect, so on a sandy/dirt map every third round stacked a second
        // growing cloud over the vanilla one and read as one huge blast. Cutting the RATE thins
        // it to an occasional wisp. coop_smokeWhipImpact 0 disables ours entirely - which is also
        // the clean A/B for telling our smoke apart from the stock surface effect.
        cvar_t *pSWI = cgi.Cvar_Get("coop_smokeWhipImpact", "0.12", CVAR_ARCHIVE);
        if (pSW->integer) {
            swMuzzle = pSWM->value;
            swImpact = pSWI->value;
        }
    }

    // muzzle smoke: once per bullet volley, at the barrel, drifting down the bullet line
    if (swMuzzle > 0.0f && i_iNumBullets > 0 && random() < swMuzzle) {
        vec3_t vMuzAng, vMuzDir, vMuzPos;
        VectorSubtract(i_vEnd[0], i_vStart, vMuzDir);
        vectoangles(vMuzDir, vMuzAng);
        VectorCopy(i_vBarrel, vMuzPos);
        sfxManager.MakeEffect_Angles(SFX_COOP_GUNSMOKE, vMuzPos, vMuzAng);
    }

    // HZM coop - BARREL WISP: a SHORT wisp of smoke that trails BACK off the barrel (toward the shooter)
    // after a shot - distinct from the forward muzzle puff above. Points down the reverse of the bullet line
    // so it reads as smoke curling off your own gun, not a plume in your face. coop_barrelSmoke = per-volley
    // spawn chance (0 = off). The look (short/thin) is baked in models/fx/coop_barrelsmoke.tik.
    {
        cvar_t *pBS = cgi.Cvar_Get("coop_barrelSmoke", "0.25", CVAR_ARCHIVE);
        if (pBS->value > 0.0f && i_iNumBullets > 0 && random() < pBS->value) {
            vec3_t vBackDir, vBackAng, vBPos;
            VectorSubtract(i_vStart, i_vEnd[0], vBackDir); // reverse of the bullet line = back toward the shooter
            vectoangles(vBackDir, vBackAng);
            VectorCopy(i_vBarrel, vBPos);
            sfxManager.MakeEffect_Angles(SFX_COOP_BARRELSMOKE, vBPos, vBackAng);
        }
    }

    // HZM coop - HEAT HAZE from a hot muzzle: each shot fired close to you (your own gun, or a teammate
    // firing nearby) adds a small screen shimmer scaled by how close the barrel is. Previously only
    // explosions did this; now gunfire does too. coop_heatGun = max per-shot strength (0 = off) - much
    // smaller than an explosion so single shots stay subtle, but rapid/sustained fire stacks into a
    // visible "hot barrel" haze that cg_view.c decays back. coop_heatGunRadius = how close the barrel
    // must be to register (keeps distant gunfire from warping your screen).
    if (i_iNumBullets > 0) {
        cvar_t *pHG = cgi.Cvar_Get("coop_heatGun", "0.18", CVAR_ARCHIVE);
        float   hg  = pHG ? pHG->value : 0.18f;
        if (hg > 0.0f) {
            cvar_t *pHGR = cgi.Cvar_Get("coop_heatGunRadius", "350", CVAR_ARCHIVE);
            float   hgr = (pHGR && pHGR->value > 1.0f) ? pHGR->value : 350.0f;
            vec3_t  vd;
            float   d;
            VectorSubtract(i_vBarrel, cg.refdef.vieworg, vd);
            d = VectorLength(vd);
            if (d < hgr) {
                // LOCALIZED now: gunfire feeds the muzzle-heat channel (tight shimmer at the gun on screen),
                // NOT the fullscreen explosion warp - so a hot barrel reads as heat rising off YOUR gun.
                CG_AddMuzzleHeat((1.0f - (d / hgr)) * hg);
            }
        }
    }

    // HZM coop - DYNAMIC MUZZLE LIGHT: a brief warm flash at the barrel that lights nearby surfaces while
    // firing (your own gun + others'). Very short life so it reads as a flicker, not a lamp. coop_muzzleLight
    // = flash radius (0 = off). The per-frame fade + the coop_dynLights master are handled in cg_view.c.
    if (i_iNumBullets > 0) {
        cvar_t *pML = cgi.Cvar_Get("coop_muzzleLight", "160", CVAR_ARCHIVE);
        float   ml  = pML ? pML->value : 160.0f;
        if (ml > 1.0f) {
            CG_AddCoopDynamicLight(i_vBarrel, 1.0f, 0.82f, 0.45f, ml, 55);
        }
    }

    // HZM coop - DISTANT WEAPON TAILS: when the SHOOTER is far away (> coop_distantFireDist units), layer a
    // soft distant-gunfire tail (muffled crack + rolling rumble) at the muzzle position, ADDITIVE on top of
    // whatever fire sound reaches us - the "battlefield in the distance" feel. This runs for every bullet
    // volley the client hears about (players + AI). Cadence detection: this message carries no entity
    // number, so shooters are keyed by a coarse hash of the muzzle position; a volley arriving < 300 ms
    // after the previous one from the same spot = automatic fire -> burst-rumble variant, else the sharper
    // rifle crack-rumble. Rate-limited per shooter so MG bursts do not stack 20 tails. coop_distantFire 0 = off.
    if (i_iNumBullets > 0) {
        cvar_t *pDF  = cgi.Cvar_Get("coop_distantFire", "1", CVAR_ARCHIVE);
        cvar_t *pDFD = cgi.Cvar_Get("coop_distantFireDist", "2500", CVAR_ARCHIVE);
        if (pDF->integer) {
            vec3_t vToShooter;
            float  fShooterDist;
            VectorSubtract(i_vStart, cg.SoundOrg, vToShooter);
            fShooterDist = VectorLength(vToShooter);
            if (fShooterDist > pDFD->value && fShooterDist < 8000.0f) {
                static int s_iTailNextTime[8];
                static int s_iTailLastShot[8];
                int        iKey =
                    (((int)i_vStart[0] >> 7) ^ ((int)i_vStart[1] >> 7) ^ ((int)i_vStart[2] >> 7)) & 7;
                qboolean bAuto        = (cg.time - s_iTailLastShot[iKey]) < 300;
                s_iTailLastShot[iKey] = cg.time;
                if (cg.time >= s_iTailNextTime[iKey]) {
                    s_iTailNextTime[iKey] = cg.time + 600; // one ~1s tail per shooter per 0.6s
                    commandManager.PlaySound(
                        bAuto ? "coop_gun_tail_auto" : "coop_gun_tail_rifle", i_vStart, -1, -1.0f, -1.0f, -1.0f, 0
                    );
                }
            }
        }
    }

    // HZM coop - gore tier 4 (UV wounds): every bullet segment we hear about is
    // server-authoritative (start = shot origin, end = where the server's trace
    // stopped, i.e. ON the victim for flesh hits). Forward each segment to the
    // renderer, which ray-tests the CPU-skinned triangles of nearby character
    // models (players + allied AND enemy AI - anything ischaracter) and paints
    // an exact bullet wound into that entity's per-entity diffuse copy at the
    // true surface UV. Wall hits simply never find a character triangle near
    // their end point and expire. Cheap call; the renderer gates on r_goreUV.
    if (cgi.R_GoreImpact) {
        int iGoreBullet;

        for (iGoreBullet = 0; iGoreBullet < i_iNumBullets; iGoreBullet++) {
            cgi.R_GoreImpact(i_vStart, i_vEnd[iGoreBullet]);
        }
    }

    // check to see if it starts in water
    bStartInWater = (cgi.CM_PointContents(i_vStart, 0) & CONTENTS_FLUID) != 0;

    for (iBullet = 0; iBullet < i_iNumBullets; iBullet++) {
        bInWater = bStartInWater;
        VectorCopy(i_vBarrel, vTrailStart);
        VectorSubtract(i_vEnd[iBullet], i_vStart, vDir);
        fLen = VectorNormalize(vDir);

        trace.fraction = 0;
        iDist          = (int)fLen + 32;
        bBulletDone    = qfalse;
        iContinueCount = 0;

        if (bStartInWater) {
            bMadeTracer = qtrue;
        } else if (!iTracerVisible) {
            bMadeTracer = qtrue;
        } else {
            bMadeTracer = qfalse;
        }

        iTravelDist = 0;
        VectorCopy(i_vStart, vTraceEnd);

        if (iDist > 0) {
            do {
                if (iDist > BULLET_TRAVEL_DIST) {
                    iTravelDist += BULLET_TRAVEL_DIST;
                    iDist -= BULLET_TRAVEL_DIST;
                } else {
                    iTravelDist += iDist;
                    iDist = 0;
                }

                memset(&trace, 0, sizeof(trace));
                VectorCopy(vTraceEnd, vTraceStart);
                VectorMA(i_vStart, iTravelDist, vDir, vTraceEnd);

                while (trace.fraction < 1) {
                    if (bIgnoreEntities) {
                        cgi.CM_BoxTrace(&trace, vTraceStart, vTraceEnd, vec_zero, vec_zero, 0, MASK_SHOT_FLUID, qfalse);

                        if (trace.fraction == 1.0f) {
                            trace.entityNum = ENTITYNUM_NONE;
                        } else {
                            trace.entityNum = ENTITYNUM_WORLD;
                        }

                        if (trace.startsolid) {
                            trace.entityNum = ENTITYNUM_WORLD;
                        }
                    } else {
                        CG_Trace(
                            &trace,
                            vTraceStart,
                            vec_zero,
                            vec_zero,
                            vTraceStart,
                            ENTITYNUM_NONE,
                            MASK_SHOT_FLUID,
                            qfalse,
                            qfalse,
                            "CG_MakeBulletTracerInternal"
                        );
                    }

                    if (trace.contents & CONTENTS_FLUID) {
                        fDist = DotProduct(vDir, trace.plane.normal) * -2.0f;
                        VectorMA(vDir, fDist, trace.plane.normal, vTmp);
                        VectorMA(trace.plane.normal, 2.0, vTmp, trace.plane.normal);
                        VectorNormalizeFast(trace.plane.normal);
                    }

                    if (!bInWater && trace.fraction < 1.0f && iNumImpacts < ARRAY_LEN(tImpacts)) {
                        memcpy(&tImpacts[iNumImpacts], &trace, sizeof(trace_t));
                        iNumImpacts++;
                    }

                    if (iTracerVisible && !bMadeTracer) {
                        CG_BulletTracerEffect(vTrailStart, trace.endpos, iLarge, alpha);
                        bMadeTracer = qtrue;
                    }

                    if (trace.fraction < 1.0f) {
                        if (((trace.surfaceFlags & (SURF_HINT | SURF_NODLIGHT | SURF_SNOW | SURF_FOLIAGE | SURF_DIRT))
                             || (trace.contents & CONTENTS_WATER))
                            && iContinueCount < 5) {
                            if (bInWater) {
                                VectorMA(trace.endpos, -1.0, vDir, vTmp);

                                if (!(trace.contents & CONTENTS_FLUID) && !(trace.surfaceFlags & SURF_PUDDLE)
                                    && !(cgi.CM_PointContents(vTmp, 0) & CONTENTS_FLUID)) {
                                    CG_MakeBubbleTrail(vTrailStart, trace.endpos, iLarge, alpha);
                                    VectorCopy(trace.endpos, vTrailStart);
                                    bInWater = qfalse;
                                }
                            } else if ((trace.contents & CONTENTS_FLUID) || (trace.surfaceFlags & SURF_PUDDLE)) {
                                VectorCopy(trace.endpos, vTrailStart);
                                bInWater = qtrue;

                                // HZM coop [user 2026-09-01] rounds cracking into the water beside you
                                // put spray on the lens. This is the exact instant the engine already
                                // decided a bullet crossed into fluid, so there is no new trace and no
                                // new detection here - only a distance test and a raise.
                                {
                                    // [user 2026-09-01] "needs to be more frequent when water
                                    // effects play near the player" - 300u only caught rounds
                                    // landing almost at your feet. 650 covers the water you are
                                    // actually wading through.
                                    float fSplashDist = Distance(trace.endpos, cg.refdef.vieworg);
                                    if (fSplashDist < 650.0f) {
                                        // 0.7 at your feet down to nothing at 650u: a real wetting,
                                        // still short of the full soaking a shell column gives.
                                        CG_CoopLensSplash(0.7f * (1.0f - fSplashDist / 650.0f));
                                    }
                                }
                            }

                            VectorMA(trace.endpos, 2.0, vDir, vTraceStart);

                            iContinueCount++;
                        } else {
                            trace.fraction = 1.0f;
                            bBulletDone    = qtrue;
                        }
                    }
                }
            } while (!bBulletDone && iDist > 0);
        }

        if (bInWater) {
            CG_MakeBubbleTrail(vTraceStart, trace.endpos, iLarge, alpha);
        }

        if (iTracerVisible) {
            iTracerVisible--;
        }

        fLen      = ProjectPointOnLine(i_vStart, i_vEnd[iBullet], cg.SoundOrg, vPos);
        iHeadDist = (int)Distance(vPos, cg.SoundOrg);
        if (iHeadDist > 255) {
            continue;
        }

        if (iHeadDist >= fZingDistA && iHeadDist >= fZingDistB && iHeadDist >= fZingDistC) {
            continue;
        }

        fDist = Distance(i_vStart, i_vEnd[iBullet]);
        if (fLen <= 128.0f || fLen >= fDist - 128.0f) {
            continue;
        }

        if (fLen < 0.f) {
            VectorCopy(i_vStart, vPos);
        } else if (fDist >= fLen) {
            VectorCopy(i_vEnd[iBullet], vPos);
        }

        if (iHeadDist < fZingDistA) {
            VectorCopy(vZingPosB, vZingPosC);
            VectorCopy(vZingPosA, vZingPosB);
            fZingDistC = fZingDistB;
            fZingDistB = fZingDistA;
            VectorCopy(vPos, vZingPosA);
            fZingDistA = iHeadDist;
        } else if (iHeadDist < fZingDistB) {
            VectorCopy(vZingPosB, vZingPosC);
            fZingDistC = fZingDistB;
            VectorCopy(vPos, vZingPosB);
            fZingDistB = iHeadDist;
        } else {
            VectorCopy(vPos, vZingPosC);
            fZingDistC = iHeadDist;
        }
    }

    if (iNumImpacts > 2) {
        fImpSndDistRA  = 9999.0f;
        fImpSndDistRB  = 9999.0f;
        fImpSndDistLA  = 9999.0f;
        fImpSndDistLB  = 9999.0f;
        iImpSndIndexRA = 0;
        iImpSndIndexLA = 0;
        iImpSndIndexRB = 0;
        iImpSndIndexLB = 0;

        for (iBullet = 0; iBullet < iNumImpacts; iBullet++) {
            CG_MakeBulletHole(
                tImpacts[iBullet].endpos, tImpacts[iBullet].plane.normal, iLarge, &tImpacts[iBullet], qfalse
            );

            // HZM coop - SMOKE WHIPS: subtle smoke wisp off the impact, drifting along the surface normal
            if (swImpact > 0.0f && random() < swImpact) {
                vec3_t vImpAng;
                vectoangles(tImpacts[iBullet].plane.normal, vImpAng);
                sfxManager.MakeEffect_Angles(SFX_COOP_GUNSMOKE, tImpacts[iBullet].endpos, vImpAng);
            }

            VectorSubtract(tImpacts[iBullet].endpos, cg.SoundOrg, vTmp);
            iHeadDist = VectorLength(vTmp);

            if (DotProduct(vTmp, cg.SoundAxis[1]) <= 0.f) {
                if (iHeadDist < fImpSndDistLA || iHeadDist < fImpSndDistLB) {
                    if (iHeadDist < fImpSndDistLA) {
                        iImpSndIndexLB = iImpSndIndexLA;
                        fImpSndDistLB  = fImpSndDistLA;
                        iImpSndIndexRA = iBullet;
                        fImpSndDistRA  = iHeadDist;
                    } else if (iHeadDist < fImpSndDistLB) {
                        iImpSndIndexLB = iBullet;
                        fImpSndDistLB  = iHeadDist;
                    }
                }
            } else {
                if (iHeadDist < fImpSndDistRA || iHeadDist < fImpSndDistRB) {
                    if (iHeadDist < fImpSndDistRA) {
                        iImpSndIndexRB = iImpSndIndexRA;
                        fImpSndDistRB  = fImpSndDistRA;
                        iImpSndIndexRA = iBullet;
                        fImpSndDistRA  = iHeadDist;
                    } else if (iHeadDist < fImpSndDistRB) {
                        iImpSndIndexRB = iBullet;
                        fImpSndDistRB  = iHeadDist;
                    }
                }
            }
        }

        if (fImpSndDistRA < 9999.0f) {
            CG_MakeBulletHoleSound(
                tImpacts[iImpSndIndexRA].endpos,
                tImpacts[iImpSndIndexRA].plane.normal,
                iLarge,
                &tImpacts[iImpSndIndexRA]
            );

            if (fImpSndDistRB < 9999.0f) {
                CG_MakeBulletHoleSound(
                    tImpacts[iImpSndIndexRB].endpos,
                    tImpacts[iImpSndIndexRB].plane.normal,
                    iLarge,
                    &tImpacts[iImpSndIndexRB]
                );
            }
        }

        if (fImpSndDistLA < 9999.0f) {
            CG_MakeBulletHoleSound(
                tImpacts[iImpSndIndexLA].endpos,
                tImpacts[iImpSndIndexLA].plane.normal,
                iLarge,
                &tImpacts[iImpSndIndexLA]
            );

            if (fImpSndDistLB < 9999.0f) {
                CG_MakeBulletHoleSound(
                    tImpacts[iImpSndIndexLB].endpos,
                    tImpacts[iImpSndIndexLB].plane.normal,
                    iLarge,
                    &tImpacts[iImpSndIndexLB]
                );
            }
        }

        // HZM coop [user 2026-08-19] NEAR-IMPACT SNAP: "plus the impact of the bullet hitting
        // nearby surfaces" - rounds striking within coop_impactSnapDist of the listener layer a
        // sharp positional snap over the stock (quiet) hole sound. Rate-limited per 110ms so a
        // burst peppering your cover reads as a hail, not a machine-gunned sample.
        {
            cvar_t *pSnap  = cgi.Cvar_Get("coop_impactSnap", "1", CVAR_ARCHIVE);
            cvar_t *pSnapD = cgi.Cvar_Get("coop_impactSnapDist", "200", CVAR_ARCHIVE);
            static int s_iNextSnapTime = 0;
            float fBest = fImpSndDistRA;
            int   iBest = iImpSndIndexRA;
            if (fImpSndDistLA < fBest) { fBest = fImpSndDistLA; iBest = iImpSndIndexLA; }
            if (pSnap->integer && fBest < pSnapD->value && cg.time >= s_iNextSnapTime) {
                s_iNextSnapTime = cg.time + 110;
                commandManager.PlaySound(
                    "snd_b_impactsnap", tImpacts[iBest].endpos, -1, 1.0f, -1.0f, 1.0f + crandom() * 0.12f, 1);
            }
        }
    } else {
        for (iBullet = 0; iBullet < iNumImpacts; iBullet++) {
            CG_MakeBulletHole(
                tImpacts[iBullet].endpos, tImpacts[iBullet].plane.normal, iNumImpacts, &tImpacts[iBullet], qtrue
            );

            // HZM coop - SMOKE WHIPS: subtle smoke wisp off the impact (the <=2-impact path)
            if (swImpact > 0.0f && random() < swImpact) {
                vec3_t vImpAng;
                vectoangles(tImpacts[iBullet].plane.normal, vImpAng);
                sfxManager.MakeEffect_Angles(SFX_COOP_GUNSMOKE, tImpacts[iBullet].endpos, vImpAng);
            }
        }
    }

    // HZM coop - DISTANCE GUNFIRE TAIL: shots fired from far away get an additive report
    // layered over the normal fire alias - a crack-slap in the mid band (coop_gunTailDist to
    // 2x that), a rolling rumble-echo beyond. Rate-limited per source area so an MG burst
    // does not stack twenty tails. coop_gunTail 0 disables.
    {
        cvar_t *pGT  = cgi.Cvar_Get("coop_gunTail", "1", CVAR_ARCHIVE);
        cvar_t *pGTD = cgi.Cvar_Get("coop_gunTailDist", "1400", CVAR_ARCHIVE);

        if (pGT->integer && pGTD->value > 0) {
            vec3_t vToShooter;
            float  fShooterDist;

            VectorSubtract(i_vStart, cg.SoundOrg, vToShooter);
            fShooterDist = VectorLength(vToShooter);

            if (fShooterDist > pGTD->value) {
                static int s_iTailNext[16];
                int        iSlot;

                iSlot = ((((int)(i_vStart[0]) >> 9) * 7) ^ ((((int)(i_vStart[1]) >> 9)) * 13)) & 15;
                if (cg.time >= s_iTailNext[iSlot]) {
                    s_iTailNext[iSlot] = cg.time + 150;
                    if (fShooterDist > 2.0f * pGTD->value) {
                        commandManager.PlaySound(
                            va("snd_gun_tail_far%d", (rand() % 2) + 1), i_vStart, -1, -1.0f, -1.0f, -1.0f, 1
                        );
                    } else {
                        commandManager.PlaySound(
                            va("snd_gun_tail_mid%d", (rand() % 2) + 1), i_vStart, -1, -1.0f, -1.0f, -1.0f, 1
                        );
                    }
                }
            }
        }
    }

    if (fZingDistA < 9999.0f) {
        // HZM coop - SUPPRESSION: an enemy round just cracked past the listener (fZingDistA is the closest
        // approach in units, 0..255; own outgoing shots are already excluded by the start/end-segment gate
        // above). Spike the under-fire screen FX, strongest for the closest pass.
        {
            float supp = (1.0f - (fZingDistA / 255.0f)) * 0.75f;
            if (supp > 0.0f) {
                CG_AddSuppression(supp);
            }
        }

        // [user 2026-08-19] "should be way more prominent - we should hear cracking and
        // zipping near us": full volume on every pass (0.8 buried the whizbys under battle
        // din) + per-round pitch jitter so an MG burst reads as many rounds, not a loop.
        if (iLarge) {
            fVolume = 1.0f;
            fPitch  = 0.8f + crandom() * 0.06f;
        } else {
            fVolume = 1.0f;
            fPitch  = 1.0f + crandom() * 0.08f;
        }

        // HZM coop - SUPERSONIC CRACK: a round passing VERY close (< coop_bulletCrackDist units of the head)
        // snaps a sharp 2D local crack (bypasses spatialization = "inside your head", which is exactly how a
        // real supersonic bow wave reads). The 3D zing below still plays for the wider band. Direct-path
        // S_StartLocalSound (same pattern as the sprint breath), random 1-of-3 variant, rate-limited so one
        // MG burst does not machine-gun the crack itself. coop_bulletCrack 0 = off.
        {
            cvar_t *pBC  = cgi.Cvar_Get("coop_bulletCrack", "1", CVAR_ARCHIVE);
            cvar_t *pBCD = cgi.Cvar_Get("coop_bulletCrackDist", "120", CVAR_ARCHIVE); // [user 2026-08-19] 60 was graze-only
            static int s_iNextCrackTime = 0;
            if (pBC->integer && fZingDistA < pBCD->value && cg.time >= s_iNextCrackTime) {
                s_iNextCrackTime = cg.time + 110;
                cgi.S_StartLocalSound(va("sound/coop_crack/crack_%02d.wav", (rand() % 3) + 1), qfalse);
            }
        }

        commandManager.PlaySound("snd_b_zing", vZingPosA, -1, fVolume, -1.0f, fPitch, 1);

        if (fZingDistB != 9999) {
            commandManager.PlaySound("snd_b_zing", vZingPosB, -1, fVolume, -1.0f, fPitch, 1);

            if (fZingDistC != 9999) {
                commandManager.PlaySound("snd_b_zing", vZingPosC, -1, fVolume, -1.0f, fPitch, 1);
            }
        }
    }
}

static void CG_MakeBulletTracer(
    vec3_t   i_vBarrel,
    vec3_t   i_vStart,
    vec3_t  *i_vEnd,
    int      i_iNumBullets,
    qboolean iLarge,
    int      iTracerVisible,
    qboolean bIgnoreEntities,
    float    alpha = 1.0f
)
{
    bullet_tracer_t *bullet_tracer;
    int              i;

    // HZM coop - VISUAL fix: the server fires the LOCAL player's bullets from the EYE, so their tracers look
    // like they shoot out of the camera/face. When a volley starts at our own eye (first person only), move
    // the tracer origin to roughly the view-weapon MUZZLE: eye + a tunable forward/right/down offset in view
    // space. Other players' tracers start at their own guns (far from our eye) and are left alone. The end
    // (impact) points are unchanged, so the tracer now runs barrel -> target. Gated by coop_tracerFromMuzzle.
    {
        static cvar_t *pOn = NULL, *pFwd = NULL, *pRt = NULL, *pUp = NULL;
        if (!pOn) {
            pOn  = cgi.Cvar_Get("coop_tracerFromMuzzle", "1",  CVAR_ARCHIVE);
            pFwd = cgi.Cvar_Get("coop_tracerMuzzleFwd",  "24", CVAR_ARCHIVE);
            pRt  = cgi.Cvar_Get("coop_tracerMuzzleRight", "6", CVAR_ARCHIVE);
            pUp  = cgi.Cvar_Get("coop_tracerMuzzleUp",   "-8", CVAR_ARCHIVE); // negative = down
        }
        if (pOn->integer && !cg.renderingThirdPerson) {
            vec3_t vEyeDelta;
            VectorSubtract(i_vStart, cg.refdef.vieworg, vEyeDelta);
            if (VectorLength(vEyeDelta) < 48.0f) { // this volley originates at our own eye = our first-person shot
                vec3_t vMuzzle;
                VectorCopy(cg.refdef.vieworg, vMuzzle);
                VectorMA(vMuzzle,  pFwd->value, cg.refdef.viewaxis[0], vMuzzle); // forward toward the barrel
                VectorMA(vMuzzle, -pRt->value,  cg.refdef.viewaxis[1], vMuzzle); // right (viewaxis[1] is LEFT)
                VectorMA(vMuzzle,  pUp->value,  cg.refdef.viewaxis[2], vMuzzle); // up (negative drops it down)
                VectorCopy(vMuzzle, i_vBarrel);
                VectorCopy(vMuzzle, i_vStart);
            }
        }
    }

    if (bullet_tracer_bullets_count >= MAX_BULLET_TRACERS) {
        // HZM coop [bug-3477] one developer line per 10 s at most - this used to print on EVERY dropped volley
        static int s_iLastTracerWarn = 0;
        if (cg.time - s_iLastTracerWarn > 10000 || cg.time < s_iLastTracerWarn) {
            s_iLastTracerWarn = cg.time;
            cgi.DPrintf("CG_MakeBulletTracer: MAX_BULLET_TRACERS exceeded\n");
        }
        return;
    }

    if (i_iNumBullets + bullet_tracers_count >= MAX_BULLET_TRACE_BULLETS) {
        Com_Printf("CG_MakeBulletTracerInternal: MAX_BULLET_TRACE_BULLETS exceeded\n");
        return;
    }

    bullet_tracer = &bullet_tracers[bullet_tracer_bullets_count++];
    VectorCopy(i_vBarrel, bullet_tracer->i_vBarrel);
    VectorCopy(i_vStart, bullet_tracer->i_vStart);
    bullet_tracer->i_vEnd        = &bullet_tracer_bullets[bullet_tracers_count];
    bullet_tracer->i_iNumBullets = i_iNumBullets;

    for (i = 0; i < i_iNumBullets; i++, bullet_tracers_count++) {
        bullet_tracer_bullets[bullet_tracers_count][0] = i_vEnd[i][0];
        bullet_tracer_bullets[bullet_tracers_count][1] = i_vEnd[i][1];
        bullet_tracer_bullets[bullet_tracers_count][2] = i_vEnd[i][2];
    }

    bullet_tracer->iLarge          = iLarge;
    bullet_tracer->alpha           = alpha;
    bullet_tracer->iTracerVisible  = iTracerVisible;
    bullet_tracer->bIgnoreEntities = bIgnoreEntities;
}

void CG_AddBulletTracers()
{
    int i;

    for (i = 0; i < bullet_tracer_bullets_count; ++i) {
        CG_MakeBulletTracerInternal(
            bullet_tracers[i].i_vBarrel,
            bullet_tracers[i].i_vStart,
            bullet_tracers[i].i_vEnd,
            bullet_tracers[i].i_iNumBullets,
            bullet_tracers[i].iLarge,
            bullet_tracers[i].iTracerVisible,
            bullet_tracers[i].bIgnoreEntities,
            bullet_tracers[i].alpha
        );
    }

    bullet_tracer_bullets_count = 0;
    bullet_tracers_count        = 0;
}

void CG_AddBulletImpacts()
{
    int          i;
    int          iHeadDist;
    float        fVolume;
    float        fImpSndDistRA;
    static float fImpSndDistLA = 9999.f;
    float        fImpSndDistRB;
    float        fImpSndDistLB;
    int          iImpSndIndexRA;
    int          iImpSndIndexRB;
    int          iImpSndIndexLB;
    vec3_t       vTmp;
    str          sSoundName;

    if (wall_impact_count) {
        if (wall_impact_count > 4) {
            fImpSndDistRA  = fImpSndDistLA;
            fImpSndDistRB  = fImpSndDistLA;
            fImpSndDistLB  = fImpSndDistLA;
            iImpSndIndexRA = 0;
            iImpSndIndexRB = 0;
            iImpSndIndexLB = 0;

            for (i = 0; i < wall_impact_count; i++) {
                VectorSubtract(wall_impact_pos[i], cg.SoundOrg, vTmp);
                iHeadDist = VectorLength(vTmp);

                if (DotProduct(vTmp, cg.SoundAxis[1]) > 0.f) {
                    if (iHeadDist < fImpSndDistRA || iHeadDist < fImpSndDistRB) {
                        if (iHeadDist < fImpSndDistRA) {
                            fImpSndDistRB  = fImpSndDistRA;
                            fImpSndDistRA  = iHeadDist;
                            iImpSndIndexRB = iImpSndIndexRA;
                            iImpSndIndexRA = i;
                        } else if (iHeadDist < fImpSndDistRB) {
                            fImpSndDistRB  = iHeadDist;
                            iImpSndIndexRB = i;
                        }
                    }
                } else {
                    if (iHeadDist < fImpSndDistLA || iHeadDist < fImpSndDistLB) {
                        if (iHeadDist < fImpSndDistLA) {
                            fImpSndDistRA  = iHeadDist;
                            fImpSndDistLB  = fImpSndDistLA;
                            iImpSndIndexLB = 0;
                            iImpSndIndexRA = i;
                        } else if (iHeadDist < fImpSndDistLB) {
                            fImpSndDistLB  = iHeadDist;
                            iImpSndIndexLB = i;
                        }
                    }
                }
            }

            if (fImpSndDistRA < fImpSndDistLA) {
                if (wall_impact_type[iImpSndIndexRA] != SFX_BHIT_PAPER_LITE) {
                    if (wall_impact_large[iImpSndIndexRA]) {
                        fVolume = 1.f;
                    } else {
                        fVolume = 0.75f;
                    }

                    if (wall_impact_type[iImpSndIndexRA] == SFX_BHIT_WOOD_LITE
                        || wall_impact_type[iImpSndIndexRA] == SFX_BHIT_WOOD_HARD) {
                        sSoundName = "snd_bh_metal";
                    } else {
                        sSoundName = "snd_bh_wood";
                    }

                    commandManager.PlaySound(sSoundName, wall_impact_pos[iImpSndIndexRA], -1, fVolume, -1, -1, 1);

                    sfxManager.MakeEffect_Normal(
                        wall_impact_type[iImpSndIndexRA],
                        wall_impact_pos[iImpSndIndexRA],
                        wall_impact_norm[iImpSndIndexRA]
                    );
                } else {
                    CG_MakeBulletHole(
                        wall_impact_pos[iImpSndIndexRA],
                        wall_impact_norm[iImpSndIndexRA],
                        wall_impact_large[iImpSndIndexRA],
                        NULL,
                        qtrue
                    );
                }

                if (fImpSndDistRB < fImpSndDistLA) {
                    if (wall_impact_type[iImpSndIndexRB]) {
                        if (wall_impact_large[iImpSndIndexRB]) {
                            fVolume = 1.f;
                        } else {
                            fVolume = 0.75f;
                        }

                        if (wall_impact_type[iImpSndIndexRB] == SFX_BHIT_WOOD_LITE
                            || wall_impact_type[iImpSndIndexRB] == SFX_BHIT_WOOD_HARD) {
                            sSoundName = "snd_bh_metal";
                        } else {
                            sSoundName = "snd_bh_wood";
                        }

                        commandManager.PlaySound(sSoundName, wall_impact_pos[iImpSndIndexRB], -1, fVolume, -1, -1, 1);

                        sfxManager.MakeEffect_Normal(
                            wall_impact_type[iImpSndIndexRB],
                            wall_impact_pos[iImpSndIndexRB],
                            wall_impact_norm[iImpSndIndexRB]
                        );
                    } else {
                        CG_MakeBulletHole(
                            wall_impact_pos[iImpSndIndexRB],
                            wall_impact_norm[iImpSndIndexRB],
                            wall_impact_large[iImpSndIndexRB],
                            NULL,
                            qtrue
                        );
                    }
                }
            }

            if (fImpSndDistLA > 9999.0f) {
                if (wall_impact_type[0] != SFX_BHIT_PAPER_LITE) {
                    if (wall_impact_large[0]) {
                        fVolume = 1.f;
                    } else {
                        fVolume = 0.75f;
                    }

                    if (wall_impact_type[0] == SFX_BHIT_WOOD_LITE || wall_impact_type[0] == SFX_BHIT_WOOD_HARD) {
                        sSoundName = "snd_bh_metal";
                    } else {
                        sSoundName = "snd_bh_wood";
                    }

                    commandManager.PlaySound(sSoundName, wall_impact_pos[0], -1, fVolume, -1, -1, 1);

                    sfxManager.MakeEffect_Normal(wall_impact_type[0], wall_impact_pos[0], wall_impact_norm[0]);
                } else {
                    CG_MakeBulletHole(wall_impact_pos[0], wall_impact_norm[0], wall_impact_large[0], NULL, qtrue);
                }
            }

            if (fImpSndDistLB < fImpSndDistLA) {
                if (wall_impact_type[iImpSndIndexLB] != SFX_BHIT_PAPER_LITE) {
                    if (wall_impact_large[iImpSndIndexLB]) {
                        fVolume = 1.f;
                    } else {
                        fVolume = 0.75f;
                    }

                    if (wall_impact_type[iImpSndIndexLB] == SFX_BHIT_WOOD_LITE
                        || wall_impact_type[iImpSndIndexLB] == SFX_BHIT_WOOD_HARD) {
                        sSoundName = "snd_bh_metal";
                    } else {
                        sSoundName = "snd_bh_wood";
                    }

                    commandManager.PlaySound(sSoundName, wall_impact_pos[iImpSndIndexLB], -1, fVolume, -1, -1, 1);

                    sfxManager.MakeEffect_Normal(
                        wall_impact_type[iImpSndIndexLB],
                        wall_impact_pos[iImpSndIndexLB],
                        wall_impact_norm[iImpSndIndexLB]
                    );
                } else {
                    CG_MakeBulletHole(
                        wall_impact_pos[iImpSndIndexLB],
                        wall_impact_norm[iImpSndIndexLB],
                        wall_impact_large[iImpSndIndexLB],
                        NULL,
                        qtrue
                    );
                }
            }
        } else {
            for (i = 0; i < wall_impact_count; i++) {
                CG_MakeBulletHole(wall_impact_pos[i], wall_impact_norm[i], wall_impact_large[i], NULL, qtrue);
            }
        }

        wall_impact_count = 0;
    }

    if (flesh_impact_count) {
        if (flesh_impact_count > 1) {
            fImpSndDistRA  = 9999.0;
            fImpSndDistRB  = 9999.0;
            iImpSndIndexRA = 0;
            iImpSndIndexRB = 0;

            for (i = 0; i < flesh_impact_count; i++) {
                VectorSubtract(flesh_impact_pos[i], cg.SoundOrg, vTmp);
                iHeadDist = VectorLength(vTmp);

                if (DotProduct(vTmp, cg.SoundAxis[1]) > 0.f) {
                    if (iHeadDist < fImpSndDistRB) {
                        fImpSndDistRA  = iHeadDist;
                        iImpSndIndexRA = i;
                    }
                } else if (iHeadDist < fImpSndDistLA) {
                    fImpSndDistRA  = iHeadDist;
                    iImpSndIndexRA = i;
                }
            }

            if (fImpSndDistRA < 9999) {
                sfxManager.MakeEffect_Normal(
                    flesh_impact_large[iImpSndIndexRA] ? SFX_BHIT_HUMAN_UNIFORM_HARD : SFX_BHIT_HUMAN_UNIFORM_LITE,
                    flesh_impact_pos[iImpSndIndexRA],
                    flesh_impact_norm[iImpSndIndexRA]
                );
                // HZM coop [user 2026-08-19] "make some of these blood drips shoot upwards when they
                // get hit": chance-layer a second burst aimed skyward with jitter, so hits read as
                // arterial spray instead of only the flat along-the-normal puff. coop_bloodSpurtUp = %.
                {{
                    static cvar_t *pSpurt = NULL;
                    if (!pSpurt) {{ pSpurt = cgi.Cvar_Get("coop_bloodSpurtUp", "40", CVAR_ARCHIVE); }}
                    if (pSpurt->integer > 0 && (rand() % 100) < pSpurt->integer) {{
                        vec3_t vUp;
                        vUp[0] = crandom() * 0.28f;
                        vUp[1] = crandom() * 0.28f;
                        vUp[2] = 1.0f;
                        VectorNormalize(vUp);
                        sfxManager.MakeEffect_Normal(SFX_BHIT_HUMAN_UNIFORM_HARD, flesh_impact_pos[iImpSndIndexRA], vUp);
                    }}
                }}
            }

            if (fImpSndDistRB < 9999) {
                sfxManager.MakeEffect_Normal(
                    flesh_impact_large[iImpSndIndexRB] ? SFX_BHIT_HUMAN_UNIFORM_HARD : SFX_BHIT_HUMAN_UNIFORM_LITE,
                    flesh_impact_pos[iImpSndIndexRB],
                    flesh_impact_norm[iImpSndIndexRB]
                );
                // HZM coop [user 2026-08-19] "make some of these blood drips shoot upwards when they
                // get hit": chance-layer a second burst aimed skyward with jitter, so hits read as
                // arterial spray instead of only the flat along-the-normal puff. coop_bloodSpurtUp = %.
                {{
                    static cvar_t *pSpurt = NULL;
                    if (!pSpurt) {{ pSpurt = cgi.Cvar_Get("coop_bloodSpurtUp", "40", CVAR_ARCHIVE); }}
                    if (pSpurt->integer > 0 && (rand() % 100) < pSpurt->integer) {{
                        vec3_t vUp;
                        vUp[0] = crandom() * 0.28f;
                        vUp[1] = crandom() * 0.28f;
                        vUp[2] = 1.0f;
                        VectorNormalize(vUp);
                        sfxManager.MakeEffect_Normal(SFX_BHIT_HUMAN_UNIFORM_HARD, flesh_impact_pos[iImpSndIndexRB], vUp);
                    }}
                }}
            }
        } else {
            for (i = 0; i < flesh_impact_count; i++) {
                sfxManager.MakeEffect_Normal(
                    flesh_impact_large[i] ? SFX_BHIT_HUMAN_UNIFORM_HARD : SFX_BHIT_HUMAN_UNIFORM_LITE,
                    flesh_impact_pos[i],
                    flesh_impact_norm[i]
                );
                // HZM coop [user 2026-08-19] "make some of these blood drips shoot upwards when they
                // get hit": chance-layer a second burst aimed skyward with jitter, so hits read as
                // arterial spray instead of only the flat along-the-normal puff. coop_bloodSpurtUp = %.
                {{
                    static cvar_t *pSpurt = NULL;
                    if (!pSpurt) {{ pSpurt = cgi.Cvar_Get("coop_bloodSpurtUp", "40", CVAR_ARCHIVE); }}
                    if (pSpurt->integer > 0 && (rand() % 100) < pSpurt->integer) {{
                        vec3_t vUp;
                        vUp[0] = crandom() * 0.28f;
                        vUp[1] = crandom() * 0.28f;
                        vUp[2] = 1.0f;
                        VectorNormalize(vUp);
                        sfxManager.MakeEffect_Normal(SFX_BHIT_HUMAN_UNIFORM_HARD, flesh_impact_pos[i], vUp);
                    }}
                }}
            }
        }

        flesh_impact_count = 0;
    }
}

void CG_MakeExplosionEffect(const vec3_t vPos, int iType)
{
    int       iSurfType;
    int       iBaseEffect;
    int       iSurfEffect;
    float     fRadius;
    vec3_t    vEnd;
    str       sMark;
    trace_t   trace;
    qhandle_t shader;

    // HZM coop - HEAT HAZE: a nearby explosion spikes the screen shimmer (closer = stronger). Done at the
    // top so it fires for every explosion type/path; cg_view.c decays it back. coop_heatRadius = max range.
    {
        cvar_t *pHR = cgi.Cvar_Get("coop_heatRadius", "700", CVAR_ARCHIVE);
        float   hr  = (pHR && pHR->value > 1.0f) ? pHR->value : 700.0f;
        vec3_t  vd;
        float   d;
        VectorSubtract(vPos, cg.refdef.vieworg, vd);
        d = VectorLength(vd);
        if (d < hr) {
            CG_AddHeat((1.0f - (d / hr)) * 0.9f);
        }
    }

    // HZM coop - DYNAMIC EXPLOSION LIGHT: a bright orange flash at the blast that lights the surroundings
    // and fades over a few frames. coop_explLight = flash radius (0 = off). Fires for every explosion path.
    {
        cvar_t *pEL = cgi.Cvar_Get("coop_explLight", "420", CVAR_ARCHIVE);
        float   el  = pEL ? pEL->value : 420.0f;
        if (el > 1.0f) {
            CG_AddCoopDynamicLight(vPos, 1.0f, 0.65f, 0.30f, el, 260);
        }
    }

    vEnd[0] = vPos[0];
    vEnd[1] = vPos[1];
    vEnd[2] = vPos[2] - 64.0;
    fRadius = 64.0;

    if ((CG_PointContents(vPos, 0)) & MASK_WATER) {
        iBaseEffect = SFX_EXP_GREN_PUDDLE;
        sfxManager.MakeEffect_Normal(iBaseEffect, vPos, Vector(0, 0, 1));
        return;
    }

    if (cg_protocol >= protocol_e::PROTOCOL_MOHTA_MIN) {
        switch (iType) {
        case CGM_EXPLOSION_EFFECT_1:
            iBaseEffect = SFX_EXP_GREN_BASE;
            break;
        case CGM_EXPLOSION_EFFECT_2:
            iBaseEffect = SFX_EXP_BAZOOKA_BASE;
            break;
        case CGM_EXPLOSION_EFFECT_3:
            iBaseEffect = SFX_EXP_HEAVYSHELL_BASE;
            break;
        case CGM_EXPLOSION_EFFECT_4:
            iBaseEffect = SFX_EXP_TANK_BASE;
            break;
        default:
            iBaseEffect = SFX_EXP_GREN_BASE;
            break;
        }
    } else {
        switch (iType) {
        case CGM6_EXPLOSION_EFFECT_2:
            iBaseEffect = SFX_EXP_BAZOOKA_BASE;
            break;
        default:
            iBaseEffect = SFX_EXP_GREN_BASE;
            break;
        }
    }

    CG_Trace(
        &trace, vPos, vec_zero, vec_zero, vEnd, ENTITYNUM_NONE, MASK_EXPLOSION, qfalse, qtrue, "CG_MakeExplosionEffect"
    );

    if (trace.fraction == 1.0 || trace.startsolid) {
        // exploded in the air
        sfxManager.MakeEffect_Normal(iBaseEffect, vPos, Vector(0, 0, 1));
        return;
    }

    VectorMA(trace.endpos, 32.0, trace.plane.normal, vEnd);

    iSurfType = trace.surfaceFlags & MASK_SURF_TYPE;
    switch (iSurfType) {
    case SURF_FOLIAGE:
        iSurfEffect = SFX_EXP_GREN_FOLIAGE;
        fRadius     = 0;
        break;
    case SURF_SNOW:
        switch (iBaseEffect) {
        case SFX_EXP_TANK_BASE:
            iSurfEffect = SFX_EXP_TANK_SNOW;
            break;
        case SFX_EXP_BAZOOKA_BASE:
            iSurfEffect = SFX_EXP_BAZOOKA_SNOW;
            if (cg_protocol >= protocol_e::PROTOCOL_MOHTA_MIN) {
                iBaseEffect = -1;
            }
            break;
        case SFX_EXP_HEAVYSHELL_BASE:
            iSurfEffect = SFX_EXP_HEAVYSHELL_SNOW;
            break;
        default:
            iSurfEffect = SFX_EXP_GREN_SNOW;
            if (cg_protocol >= protocol_e::PROTOCOL_MOHTA_MIN) {
                iBaseEffect = -1;
            }
            break;
        }
        fRadius = 0;
        break;
    case SURF_CARPET:
        iSurfEffect = SFX_EXP_GREN_CARPET;
        break;
    case SURF_SAND:
        iSurfEffect = SFX_EXP_GREN_SAND;
        fRadius     = 0;
        break;
    case SURF_PUDDLE:
        iSurfEffect = -1;
        fRadius     = 0;
        break;
    case SURF_GLASS:
        iSurfEffect = -1;
        fRadius     = 0;
        break;
    case SURF_GRAVEL:
        iSurfEffect = SFX_EXP_GREN_GRAVEL;
        break;
    case SURF_MUD:
        iSurfEffect = SFX_EXP_GREN_MUD;
        fRadius     = 0;
        break;
    case SURF_DIRT:
        switch (iBaseEffect) {
        case SFX_EXP_TANK_BASE:
            iSurfEffect = SFX_EXP_TANK_DIRT;
            break;
        case SFX_EXP_BAZOOKA_BASE:
            iSurfEffect = SFX_EXP_BAZOOKA_DIRT;
            if (cg_protocol >= protocol_e::PROTOCOL_MOHTA_MIN) {
                iBaseEffect = -1;
            }
            break;
        case SFX_EXP_HEAVYSHELL_BASE:
            iSurfEffect = SFX_EXP_HEAVYSHELL_DIRT;
            break;
        default:
            iSurfEffect = SFX_EXP_GREN_DIRT;
            if (cg_protocol >= protocol_e::PROTOCOL_MOHTA_MIN) {
                iBaseEffect = -1;
            }
            break;
        }
        fRadius = 0;
        break;
    case SURF_GRILL:
        iSurfEffect = SFX_EXP_GREN_GRILL;
        fRadius     = 0;
        break;
    case SURF_GRASS:
        switch (iBaseEffect) {
        case SFX_EXP_TANK_BASE:
            iSurfEffect = SFX_EXP_TANK_DIRT;
            break;
        case SFX_EXP_BAZOOKA_BASE:
            iSurfEffect = SFX_EXP_BAZOOKA_DIRT;
            break;
        case SFX_EXP_HEAVYSHELL_BASE:
            iSurfEffect = SFX_EXP_HEAVYSHELL_DIRT;
            break;
        default:
            iSurfEffect = SFX_EXP_GREN_GRASS;
            if (cg_protocol >= protocol_e::PROTOCOL_MOHTA_MIN) {
                iBaseEffect = -1;
            }
            break;
        }
        fRadius = 0;
        break;
    case SURF_ROCK:
        switch (iBaseEffect) {
        case SFX_EXP_TANK_BASE:
            iSurfEffect = SFX_EXP_TANK_STONE;
            break;
        case SFX_EXP_BAZOOKA_BASE:
            iSurfEffect = SFX_EXP_BAZOOKA_STONE;
            if (cg_protocol >= protocol_e::PROTOCOL_MOHTA_MIN) {
                iBaseEffect = -1;
            }
            break;
        case SFX_EXP_HEAVYSHELL_BASE:
            iSurfEffect = SFX_EXP_HEAVYSHELL_STONE;
            break;
        default:
            iSurfEffect = SFX_EXP_GREN_STONE;
            if (cg_protocol >= protocol_e::PROTOCOL_MOHTA_MIN) {
                iBaseEffect = -1;
            }
            break;
        }
        break;
    case SURF_PAPER:
        iSurfEffect = SFX_EXP_GREN_PAPER;
        break;
    case SURF_WOOD:
        iSurfEffect = SFX_EXP_GREN_WOOD;
        break;
    case SURF_METAL:
        iSurfEffect = SFX_EXP_GREN_METAL;
        break;
    default:
        iSurfEffect = -1;
        break;
    }

    sMark = "blastmark";
    if (fRadius) {
        fRadius *= 1.f + crandom() * 0.1;
        shader = cgi.R_RegisterShader(sMark.c_str());

        CG_ImpactMarkSimple(
            shader,
            trace.endpos,
            trace.plane.normal,
            random() * 360.0,
            fRadius,
            1,
            1,
            1,
            1,
            qfalse,
            qfalse,
            qtrue,
            qfalse
        );
    }

    VectorMA(vEnd, 1.0, trace.plane.normal, vEnd);

    if (iSurfEffect != -1) {
        sfxManager.MakeEffect_Normal(
            iSurfEffect, Vector(trace.endpos) + Vector(trace.plane.normal), trace.plane.normal
        );
    }

    if (iBaseEffect != -1) {
        sfxManager.MakeEffect_Normal(iBaseEffect, vEnd, trace.plane.normal);
    }
}

void CG_MakeVehicleEffect(vec3_t i_vStart, vec3_t i_vEnd, vec3_t i_vDir)
{
    vec3_t  vDir, vFrom, vDest;
    trace_t trace;

    VectorSubtract(i_vEnd, i_vStart, vDir);
    VectorNormalizeFast(vDir);
    VectorMA(i_vEnd, -16.0, vDir, vFrom);
    VectorMA(i_vEnd, 16.0, vDir, vDest);

    CG_Trace(
        &trace, vFrom, vec_zero, vec_zero, vDest, ENTITYNUM_NONE, MASK_PLAYERSOLID, qfalse, qtrue, "CG_MakeBulletHole"
    );
    cgi.R_DebugLine(vFrom, trace.endpos, 1.0, 1.0, 1.0, 1.0);
}

void CG_ParseCGMessage_ver_15()
{
    int    i;
    int    iType;
    int    iLarge;
    int    iInfo;
    int    iCount;
    char  *szTmp;
    vec3_t vStart, vEnd, vTmp;
    vec3_t vEndArray[MAX_IMPACTS];
    float  alpha;

    qboolean bMoreCGameMessages = qtrue;
    while (bMoreCGameMessages) {
        iType = cgi.MSG_ReadBits(6);

        switch (iType) {
        case CGM_BULLET_1:
        case CGM_BULLET_2:
        case CGM_BULLET_5:
            if (iType == CGM_BULLET_1) {
                vTmp[0] = cgi.MSG_ReadCoord();
                vTmp[1] = cgi.MSG_ReadCoord();
                vTmp[2] = cgi.MSG_ReadCoord();
            }
            vStart[0] = cgi.MSG_ReadCoord();
            vStart[1] = cgi.MSG_ReadCoord();
            vStart[2] = cgi.MSG_ReadCoord();

            if (iType != CGM_BULLET_1) {
                vTmp[0] = vStart[0];
                vTmp[1] = vStart[1];
                vTmp[2] = vStart[2];
            }

            vEndArray[0][0] = cgi.MSG_ReadCoord();
            vEndArray[0][1] = cgi.MSG_ReadCoord();
            vEndArray[0][2] = cgi.MSG_ReadCoord();
            iLarge          = cgi.MSG_ReadBits(2);
            if (cgi.MSG_ReadBits(1)) {
                int iAlpha = cgi.MSG_ReadBits(10);
                alpha      = (float)iAlpha / 512.0;
                if (alpha < 0.002f) {
                    alpha = 0.002f;
                }
            } else {
                alpha = 1.0f;
            }

            if (iType == CGM_BULLET_1) {
                CG_MakeBulletTracer(vTmp, vStart, vEndArray, 1, iLarge, qtrue, qtrue, alpha);
            } else if (iType == CGM_BULLET_2) {
                CG_MakeBulletTracer(vTmp, vStart, vEndArray, 1, iLarge, qfalse, qtrue, alpha);
            } else {
                CG_MakeBubbleTrail(vStart, vEndArray[0], iLarge, alpha);
            }

            break;
        case CGM_BULLET_3:
        case CGM_BULLET_4:
            if (iType == CGM_BULLET_3) {
                vTmp[0] = cgi.MSG_ReadCoord();
                vTmp[1] = cgi.MSG_ReadCoord();
                vTmp[2] = cgi.MSG_ReadCoord();
                iInfo   = cgi.MSG_ReadBits(6);
            } else {
                iInfo = 0;
            }

            vStart[0] = cgi.MSG_ReadCoord();
            vStart[1] = cgi.MSG_ReadCoord();
            vStart[2] = cgi.MSG_ReadCoord();
            iLarge    = cgi.MSG_ReadBits(2);
            if (cgi.MSG_ReadBits(1)) {
                int iAlpha = cgi.MSG_ReadBits(10);
                alpha      = (float)iAlpha / 512.0;
                if (alpha < 0.002f) {
                    alpha = 0.002f;
                }
            } else {
                alpha = 1.0f;
            }

            iCount = cgi.MSG_ReadBits(6);
            for (i = 0; i < iCount; ++i) {
                vEndArray[i][0] = cgi.MSG_ReadCoord();
                vEndArray[i][1] = cgi.MSG_ReadCoord();
                vEndArray[i][2] = cgi.MSG_ReadCoord();
            }

            if (iCount) {
                CG_MakeBulletTracer(vTmp, vStart, vEndArray, iCount, iLarge, iInfo, qtrue, alpha);
            }
            break;
        case CGM_BULLET_6:
        case CGM_BULLET_7:
        case CGM_BULLET_8:
        case CGM_BULLET_9:
        case CGM_BULLET_10:
        case CGM_BULLET_11:
            vStart[0] = cgi.MSG_ReadCoord();
            vStart[1] = cgi.MSG_ReadCoord();
            vStart[2] = cgi.MSG_ReadCoord();
            cgi.MSG_ReadDir(vEnd);
            iLarge = cgi.MSG_ReadBits(2);

            switch (iType) {
            case CGM_BULLET_6:
                if (wall_impact_count < MAX_IMPACTS) {
                    VectorCopy(vStart, wall_impact_pos[wall_impact_count]);
                    VectorCopy(vEnd, wall_impact_norm[wall_impact_count]);
                    wall_impact_large[wall_impact_count] = iLarge;
                    wall_impact_type[wall_impact_count]  = -1;
                    wall_impact_count++;
                }
                break;
            case CGM_BULLET_7:
                if (wall_impact_count < MAX_IMPACTS) {
                    VectorCopy(vStart, wall_impact_pos[wall_impact_count]);
                    VectorCopy(vEnd, wall_impact_norm[wall_impact_count]);
                    wall_impact_large[wall_impact_count] = iLarge;
                    wall_impact_type[wall_impact_count]  = SFX_BHIT_STONE_LITE;
                    wall_impact_count++;
                }
                break;
            case CGM_BULLET_8:
                if (flesh_impact_count < MAX_IMPACTS) {
                    // negative
                    VectorNegate(vEnd, vEnd);
                    VectorCopy(vStart, flesh_impact_pos[flesh_impact_count]);
                    VectorCopy(vEnd, flesh_impact_norm[flesh_impact_count]);
                    flesh_impact_large[flesh_impact_count] = iLarge;
                    CG_NoteFleshImpact(vStart); // HZM coop - blood on the gun at knife range
                    flesh_impact_count++;
                }
                CG_RagdollImpulse(vStart, vEnd, 150.0f + 70.0f * iLarge, 15.0f + 1.5f * iLarge,
                                  600 + 70 * iLarge); // corpse hit: THAT limb moves
                break;
            case CGM_BULLET_9:
                if (flesh_impact_count < MAX_IMPACTS) {
                    // negative
                    VectorNegate(vEnd, vEnd);
                    VectorCopy(vStart, flesh_impact_pos[flesh_impact_count]);
                    VectorCopy(vEnd, flesh_impact_norm[flesh_impact_count]);
                    flesh_impact_large[flesh_impact_count] = iLarge;
                    CG_NoteFleshImpact(vStart); // HZM coop - blood on the gun at knife range
                    flesh_impact_count++;
                }
                CG_RagdollImpulse(vStart, vEnd, 150.0f + 70.0f * iLarge, 15.0f + 1.5f * iLarge,
                                  600 + 70 * iLarge); // corpse hit: THAT limb moves
                break;
            case CGM_BULLET_10:
                if (wall_impact_count < MAX_IMPACTS) {
                    VectorCopy(vStart, wall_impact_pos[wall_impact_count]);
                    VectorCopy(vEnd, wall_impact_norm[wall_impact_count]);
                    wall_impact_large[wall_impact_count] = iLarge;
                    wall_impact_type[wall_impact_count]  = SFX_BHIT_WOOD_LITE;
                    wall_impact_count++;
                }
                break;
            case CGM_BULLET_11:
                if (wall_impact_count < MAX_IMPACTS) {
                    VectorCopy(vStart, wall_impact_pos[wall_impact_count]);
                    VectorCopy(vEnd, wall_impact_norm[wall_impact_count]);
                    wall_impact_large[wall_impact_count] = iLarge;
                    wall_impact_type[wall_impact_count]  = SFX_BHIT_METAL_LITE;
                    wall_impact_count++;
                }
                break;
            default:
                continue;
            }
            break;

        case CGM_MELEE_IMPACT:
            vStart[0] = cgi.MSG_ReadCoord();
            vStart[1] = cgi.MSG_ReadCoord();
            vStart[2] = cgi.MSG_ReadCoord();
            vEnd[0]   = cgi.MSG_ReadCoord();
            vEnd[1]   = cgi.MSG_ReadCoord();
            vEnd[2]   = cgi.MSG_ReadCoord();
            CG_MeleeImpact(vStart, vEnd);
            break;
        case CGM_EXPLOSION_EFFECT_1:
        case CGM_EXPLOSION_EFFECT_2:
        case CGM_EXPLOSION_EFFECT_3:
        case CGM_EXPLOSION_EFFECT_4:
            vStart[0] = cgi.MSG_ReadCoord();
            vStart[1] = cgi.MSG_ReadCoord();
            vStart[2] = cgi.MSG_ReadCoord();
            CG_MakeExplosionEffect(vStart, iType);
            // blast wave onto any nearby corpse: radial (NULL dir), wide radius, long limp so the
            // body tumbles instead of snapping back to its pose mid-flight. The server also tosses
            // the corpse ENTITY and our pose already follows that - this adds the limb motion the
            // toss alone cannot produce (live: "grenades kinda just lift the body up as a whole").
            CG_RagdollImpulse(vStart, NULL, 400.0f + 20.0f * (iType - CGM_EXPLOSION_EFFECT_1),
                              180.0f + 55.0f * (iType - CGM_EXPLOSION_EFFECT_1),
                              1200 + 70 * (iType - CGM_EXPLOSION_EFFECT_1));
            break;
        case CGM_MAKE_EFFECT_1:
        case CGM_MAKE_EFFECT_2:
        case CGM_MAKE_EFFECT_3:
        case CGM_MAKE_EFFECT_4:
        case CGM_MAKE_EFFECT_5:
        case CGM_MAKE_EFFECT_6:
        case CGM_MAKE_EFFECT_7:
        case CGM_MAKE_EFFECT_8:
            vStart[0] = cgi.MSG_ReadCoord();
            vStart[1] = cgi.MSG_ReadCoord();
            vStart[2] = cgi.MSG_ReadCoord();
            cgi.MSG_ReadDir(vEnd);

            sfxManager.MakeEffect_Normal((iType - CGM_MAKE_EFFECT_1) + SFX_OIL_LEAK_BIG, vStart, vEnd);
            break;

        case CGM_MAKE_CRATE_DEBRIS:
        case CGM_MAKE_WINDOW_DEBRIS:
            {
                str    sEffect;
                char   cTmp[8];
                vec3_t axis[3];

                vStart[0] = cgi.MSG_ReadCoord();
                vStart[1] = cgi.MSG_ReadCoord();
                vStart[2] = cgi.MSG_ReadCoord();
                iLarge    = cgi.MSG_ReadByte();
                // get the integer as string
                snprintf(cTmp, sizeof(cTmp), "%d", iLarge);

                if (iType == CGM_MAKE_CRATE_DEBRIS) {
                    sEffect = "models/fx/crates/debris_";
                } else {
                    sEffect = "models/fx/windows/debris_";
                }

                sEffect += cTmp;
                sEffect += ".tik";

                VectorSet(axis[0], 0, 0, 1);
                VectorSet(axis[1], 0, 1, 0);
                VectorSet(axis[2], 1, 0, 0);

                cgi.R_SpawnEffectModel(sEffect.c_str(), vStart, axis);
            }
            break;

        case CGM_BULLET_NO_BARREL_1:
            vTmp[0]         = cgi.MSG_ReadCoord();
            vTmp[1]         = cgi.MSG_ReadCoord();
            vTmp[2]         = cgi.MSG_ReadCoord();
            vStart[0]       = cgi.MSG_ReadCoord();
            vStart[1]       = cgi.MSG_ReadCoord();
            vStart[2]       = cgi.MSG_ReadCoord();
            vEndArray[0][0] = cgi.MSG_ReadCoord();
            vEndArray[0][1] = cgi.MSG_ReadCoord();
            vEndArray[0][2] = cgi.MSG_ReadCoord();
            iLarge          = cgi.MSG_ReadBits(2);

            if (cgi.MSG_ReadBits(1)) {
                int iAlpha = cgi.MSG_ReadBits(10);
                alpha      = (float)iAlpha / 512.0;
                if (alpha < 0.002f) {
                    alpha = 0.002f;
                }
            } else {
                alpha = 1.0f;
            }

            CG_MakeBulletTracer(vTmp, vStart, vEndArray, 1, iLarge, qtrue, qtrue, alpha);
            break;

        case CGM_BULLET_NO_BARREL_2:
            memset(vTmp, 0, sizeof(vTmp));
            vStart[0]       = cgi.MSG_ReadCoord();
            vStart[1]       = cgi.MSG_ReadCoord();
            vStart[2]       = cgi.MSG_ReadCoord();
            vEndArray[0][0] = cgi.MSG_ReadCoord();
            vEndArray[0][1] = cgi.MSG_ReadCoord();
            vEndArray[0][2] = cgi.MSG_ReadCoord();
            iLarge          = cgi.MSG_ReadBits(2);

            if (cgi.MSG_ReadBits(1)) {
                int iAlpha = cgi.MSG_ReadBits(10);
                alpha      = (float)iAlpha / 512.0;
                if (alpha < 0.002f) {
                    alpha = 0.002f;
                }
            } else {
                alpha = 1.0f;
            }

            CG_MakeBulletTracer(vTmp, vStart, vEndArray, 1, iLarge, qfalse, qtrue, alpha);
            break;

        case CGM_HUDDRAW_SHADER:
            iInfo = cgi.MSG_ReadByte();
            Q_strncpyz(
                cgi.HudDrawElements[iInfo].shaderName,
                cgi.MSG_ReadString(),
                sizeof(cgi.HudDrawElements[iInfo].shaderName)
            );
            cgi.HudDrawElements[iInfo].string[0]   = 0;
            cgi.HudDrawElements[iInfo].pFont       = NULL;
            cgi.HudDrawElements[iInfo].fontName[0] = 0;
            // set the shader
            CG_HudDrawShader(iInfo);
            break;

        case CGM_HUDDRAW_ALIGN:
            iInfo                                       = cgi.MSG_ReadByte();
            cgi.HudDrawElements[iInfo].iHorizontalAlign = cgi.MSG_ReadBits(2);
            cgi.HudDrawElements[iInfo].iVerticalAlign   = cgi.MSG_ReadBits(2);
            break;

        case CGM_HUDDRAW_RECT:
            iInfo                              = cgi.MSG_ReadByte();
            cgi.HudDrawElements[iInfo].iX      = cgi.MSG_ReadShort();
            cgi.HudDrawElements[iInfo].iY      = cgi.MSG_ReadShort();
            cgi.HudDrawElements[iInfo].iWidth  = cgi.MSG_ReadShort();
            cgi.HudDrawElements[iInfo].iHeight = cgi.MSG_ReadShort();
            break;

        case CGM_HUDDRAW_VIRTUALSIZE:
            iInfo                                     = cgi.MSG_ReadByte();
            cgi.HudDrawElements[iInfo].bVirtualScreen = cgi.MSG_ReadBits(1);
            break;

        case CGM_HUDDRAW_COLOR:
            iInfo                                = cgi.MSG_ReadByte();
            cgi.HudDrawElements[iInfo].vColor[0] = cgi.MSG_ReadByte() / 255.0;
            cgi.HudDrawElements[iInfo].vColor[1] = cgi.MSG_ReadByte() / 255.0;
            cgi.HudDrawElements[iInfo].vColor[2] = cgi.MSG_ReadByte() / 255.0;
            break;

        case CGM_HUDDRAW_ALPHA:
            iInfo                                = cgi.MSG_ReadByte();
            cgi.HudDrawElements[iInfo].vColor[3] = cgi.MSG_ReadByte() / 255.0;
            break;

        case CGM_HUDDRAW_STRING:
            iInfo                              = cgi.MSG_ReadByte();
            cgi.HudDrawElements[iInfo].hShader = 0;
            Q_strncpyz(
                cgi.HudDrawElements[iInfo].string, cgi.MSG_ReadString(), sizeof(cgi.HudDrawElements[iInfo].string)
            );
            break;

        case CGM_HUDDRAW_FONT:
            iInfo = cgi.MSG_ReadByte();
            Q_strncpyz(
                cgi.HudDrawElements[iInfo].fontName, cgi.MSG_ReadString(), sizeof(cgi.HudDrawElements[iInfo].fontName)
            );
            cgi.HudDrawElements[iInfo].hShader       = 0;
            cgi.HudDrawElements[iInfo].shaderName[0] = 0;
            // load the font
            CG_HudDrawFont(iInfo);
            break;

        case CGM_NOTIFY_HIT:
        case CGM_NOTIFY_KILL:
            if (cg.snap) {
                const char *soundName;
                int         iOldEnt;

                iOldEnt = current_entity_number;

                current_entity_number = cg.snap->ps.clientNum;
                // HZM coop [2026-08-28] the marker rides the SAME message as the cue, so the pip and
                // the tick can never disagree about whether a hit landed.
                extern void CG_CoopHitMark(qboolean bKill);
                CG_CoopHitMark((iType == CGM_NOTIFY_KILL) ? qtrue : qfalse);

                if (iType == CGM_NOTIFY_KILL) {
                    commandManager.PlaySound("dm_kill_notify", NULL, CHAN_LOCAL, 2.0, -1, -1, 1);
                } else {
                    commandManager.PlaySound("dm_hit_notify", NULL, CHAN_LOCAL, 2.0, -1, -1, 1);
                }

                current_entity_number = iOldEnt;
            }
            break;

        case CGM_VOICE_CHAT:
            {
                qboolean bLocal;
                int iOldEnt;

                vStart[0] = cgi.MSG_ReadCoord();
                vStart[1] = cgi.MSG_ReadCoord();
                vStart[2] = cgi.MSG_ReadCoord();
                bLocal    = cgi.MSG_ReadBits(1);
                iInfo     = cgi.MSG_ReadBits(6);
                szTmp     = cgi.MSG_ReadString();

                iOldEnt = current_entity_number;

                if (bLocal) {
                    current_entity_number = iInfo;

                    commandManager.PlaySound(szTmp, vStart, CHAN_AUTO, -1, -1, -1, 0);
                } else if (cg.snap) {
                    current_entity_number = cg.snap->ps.clientNum;

                    commandManager.PlaySound(szTmp, vStart, CHAN_LOCAL, -1, -1, -1, 1);
                }

                CG_RadarClientSpeaks(iInfo);
                current_entity_number = iOldEnt;
            }
            break;
        case CGM_FENCEPOST:
            {
                int    iHalf;
                vec3_t vForward, vRight, vUp;
                float  fLength;

                vStart[0] = cgi.MSG_ReadCoord();
                vStart[1] = cgi.MSG_ReadCoord();
                vStart[2] = cgi.MSG_ReadCoord();
                vEnd[0]   = cgi.MSG_ReadCoord();
                vEnd[1]   = cgi.MSG_ReadCoord();
                vEnd[2]   = cgi.MSG_ReadCoord();
                iHalf     = cgi.MSG_ReadByte();
                cgi.MSG_ReadByte();

                VectorSubtract(vEnd, vStart, vForward);
                fLength = VectorNormalize(vForward);
                iCount  = fLength / 2.0;

                MakeNormalVectors(vForward, vRight, vUp);

                for (i = 0; i < iCount; i++) {
                    vec3_t vOrg;
                    vec3_t vNormal;

                    VectorCopy(vStart, vOrg);

                    VectorMA(vOrg, random(), vForward, vOrg);
                    VectorMA(vOrg, crandom() * iHalf, vRight, vOrg);
                    VectorMA(vOrg, crandom() * iHalf, vUp, vOrg);

                    //
                    // Spawn the fence wood effect
                    //

                    VectorSubtract(vStart, vOrg, vNormal);
                    VectorNormalize(vNormal);

                    sfxManager.MakeEffect_Normal(SFX_FENCE_WOOD, vOrg, vNormal);
                }
            }
            break;
        default:
            cgi.Error(ERR_DROP, "CG_ParseCGMessage: Unknown CGM message type");
            break;
        }

        bMoreCGameMessages = cgi.MSG_ReadBits(1);
    }
}

void CG_ParseCGMessage_ver_6()
{
    int    i;
    int    iType;
    int    iLarge;
    int    iInfo;
    int    iCount;
    char  *szTmp;
    vec3_t vStart, vEnd, vTmp;
    vec3_t vEndArray[MAX_IMPACTS];

    qboolean bMoreCGameMessages = qtrue;
    while (bMoreCGameMessages) {
        iType = cgi.MSG_ReadBits(6);

        switch (iType) {
        case CGM6_BULLET_1:
        case CGM6_BULLET_2:
        case CGM6_BULLET_5:
            if (iType == CGM6_BULLET_1) {
                vTmp[0] = cgi.MSG_ReadCoord();
                vTmp[1] = cgi.MSG_ReadCoord();
                vTmp[2] = cgi.MSG_ReadCoord();
            }
            vStart[0] = cgi.MSG_ReadCoord();
            vStart[1] = cgi.MSG_ReadCoord();
            vStart[2] = cgi.MSG_ReadCoord();

            if (iType != CGM6_BULLET_1) {
                vTmp[0] = vStart[0];
                vTmp[1] = vStart[1];
                vTmp[2] = vStart[2];
            }

            vEndArray[0][0] = cgi.MSG_ReadCoord();
            vEndArray[0][1] = cgi.MSG_ReadCoord();
            vEndArray[0][2] = cgi.MSG_ReadCoord();
            iLarge          = cgi.MSG_ReadBits(1);

            if (iType == CGM6_BULLET_1) {
                CG_MakeBulletTracer(vTmp, vStart, vEndArray, 1, iLarge, qtrue, qtrue);
            } else if (iType == CGM6_BULLET_2) {
                CG_MakeBulletTracer(vTmp, vStart, vEndArray, 1, iLarge, qfalse, qtrue);
            } else {
                CG_MakeBubbleTrail(vStart, vEndArray[0], iLarge);
            }

            break;
        case CGM6_BULLET_3:
        case CGM6_BULLET_4:
            if (iType == CGM6_BULLET_3) {
                vTmp[0] = cgi.MSG_ReadCoord();
                vTmp[1] = cgi.MSG_ReadCoord();
                vTmp[2] = cgi.MSG_ReadCoord();
                iInfo   = cgi.MSG_ReadBits(6);
            } else {
                iInfo = 0;
            }

            vStart[0] = cgi.MSG_ReadCoord();
            vStart[1] = cgi.MSG_ReadCoord();
            vStart[2] = cgi.MSG_ReadCoord();
            iLarge    = cgi.MSG_ReadBits(1);
            iCount    = cgi.MSG_ReadBits(6);
            for (i = 0; i < iCount; ++i) {
                vEndArray[i][0] = cgi.MSG_ReadCoord();
                vEndArray[i][1] = cgi.MSG_ReadCoord();
                vEndArray[i][2] = cgi.MSG_ReadCoord();
            }

            if (iCount) {
                CG_MakeBulletTracer(vTmp, vStart, vEndArray, iCount, iLarge, iInfo, qtrue);
            }
            break;
        case CGM6_BULLET_6:
        case CGM6_BULLET_7:
        case CGM6_BULLET_8:
        case CGM6_BULLET_9:
        case CGM6_BULLET_10:
            vStart[0] = cgi.MSG_ReadCoord();
            vStart[1] = cgi.MSG_ReadCoord();
            vStart[2] = cgi.MSG_ReadCoord();
            cgi.MSG_ReadDir(vEnd);
            iLarge = cgi.MSG_ReadBits(1);

            switch (iType) {
            case CGM6_BULLET_6:
                if (wall_impact_count < MAX_IMPACTS) {
                    VectorCopy(vStart, wall_impact_pos[wall_impact_count]);
                    VectorCopy(vEnd, wall_impact_norm[wall_impact_count]);
                    wall_impact_large[wall_impact_count] = iLarge;
                    wall_impact_type[wall_impact_count]  = SFX_BHIT_PAPER_LITE;
                    wall_impact_count++;
                }
                break;
            case CGM6_BULLET_7:
                if (flesh_impact_count < MAX_IMPACTS) {
                    // negative
                    VectorNegate(vEnd, vEnd);
                    VectorCopy(vStart, flesh_impact_pos[flesh_impact_count]);
                    VectorCopy(vEnd, flesh_impact_norm[flesh_impact_count]);
                    flesh_impact_large[flesh_impact_count] = iLarge;
                    CG_NoteFleshImpact(vStart); // HZM coop - blood on the gun at knife range
                    flesh_impact_count++;
                }
                CG_RagdollImpulse(vStart, vEnd, 150.0f + 70.0f * iLarge, 15.0f + 1.5f * iLarge,
                                  600 + 70 * iLarge); // corpse hit: THAT limb moves
                break;
            case CGM6_BULLET_8:
                if (flesh_impact_count < MAX_IMPACTS) {
                    // negative
                    VectorNegate(vEnd, vEnd);
                    VectorCopy(vStart, flesh_impact_pos[flesh_impact_count]);
                    VectorCopy(vEnd, flesh_impact_norm[flesh_impact_count]);
                    flesh_impact_large[flesh_impact_count] = iLarge;
                    CG_NoteFleshImpact(vStart); // HZM coop - blood on the gun at knife range
                    flesh_impact_count++;
                }
                CG_RagdollImpulse(vStart, vEnd, 150.0f + 70.0f * iLarge, 15.0f + 1.5f * iLarge,
                                  600 + 70 * iLarge); // corpse hit: THAT limb moves
                break;
            case CGM6_BULLET_9:
                if (wall_impact_count < MAX_IMPACTS) {
                    VectorCopy(vStart, wall_impact_pos[wall_impact_count]);
                    VectorCopy(vEnd, wall_impact_norm[wall_impact_count]);
                    wall_impact_large[wall_impact_count] = iLarge;
                    wall_impact_type[wall_impact_count]  = iLarge ? SFX_BHIT_WOOD_HARD : SFX_BHIT_WOOD_LITE;
                    wall_impact_count++;
                }
                break;
            case CGM6_BULLET_10:
                if (wall_impact_count < MAX_IMPACTS) {
                    VectorCopy(vStart, wall_impact_pos[wall_impact_count]);
                    VectorCopy(vEnd, wall_impact_norm[wall_impact_count]);
                    wall_impact_large[wall_impact_count] = iLarge;
                    wall_impact_type[wall_impact_count]  = iLarge ? SFX_BHIT_METAL_HARD : SFX_BHIT_METAL_LITE;
                    wall_impact_count++;
                }
                break;
            default:
                continue;
            }
            break;

        case CGM6_MELEE_IMPACT:
            vStart[0] = cgi.MSG_ReadCoord();
            vStart[1] = cgi.MSG_ReadCoord();
            vStart[2] = cgi.MSG_ReadCoord();
            vEnd[0]   = cgi.MSG_ReadCoord();
            vEnd[1]   = cgi.MSG_ReadCoord();
            vEnd[2]   = cgi.MSG_ReadCoord();
            CG_MeleeImpact(vStart, vEnd);
            break;
        case CGM6_EXPLOSION_EFFECT_1:
        case CGM6_EXPLOSION_EFFECT_2:
            vStart[0] = cgi.MSG_ReadCoord();
            vStart[1] = cgi.MSG_ReadCoord();
            vStart[2] = cgi.MSG_ReadCoord();
            CG_MakeExplosionEffect(vStart, iType);
            break;
        case CGM6_MAKE_EFFECT_1:
        case CGM6_MAKE_EFFECT_2:
        case CGM6_MAKE_EFFECT_3:
        case CGM6_MAKE_EFFECT_4:
        case CGM6_MAKE_EFFECT_5:
        case CGM6_MAKE_EFFECT_6:
        case CGM6_MAKE_EFFECT_7:
        case CGM6_MAKE_EFFECT_8:
            vStart[0] = cgi.MSG_ReadCoord();
            vStart[1] = cgi.MSG_ReadCoord();
            vStart[2] = cgi.MSG_ReadCoord();
            cgi.MSG_ReadDir(vEnd);

            sfxManager.MakeEffect_Normal((iType - CGM6_MAKE_EFFECT_1) + SFX_OIL_LEAK_BIG, vStart, vEnd);
            break;

        case CGM6_MAKE_CRATE_DEBRIS:
        case CGM6_MAKE_WINDOW_DEBRIS:
            {
                str    sEffect;
                char   cTmp[8];
                vec3_t axis[3];

                vStart[0] = cgi.MSG_ReadCoord();
                vStart[1] = cgi.MSG_ReadCoord();
                vStart[2] = cgi.MSG_ReadCoord();
                iLarge    = cgi.MSG_ReadByte();
                // get the integer as string
                snprintf(cTmp, sizeof(cTmp), "%d", iLarge);

                if (iType == CGM6_MAKE_CRATE_DEBRIS) {
                    sEffect = "models/fx/crates/debris_";
                } else {
                    sEffect = "models/fx/windows/debris_";
                }

                sEffect += cTmp;
                sEffect += ".tik";

                VectorSet(axis[0], 0, 0, 1);
                VectorSet(axis[1], 0, 1, 0);
                VectorSet(axis[2], 1, 0, 0);

                cgi.R_SpawnEffectModel(sEffect.c_str(), vStart, axis);
            }
            break;

        case CGM6_BULLET_NO_BARREL_1:
            vTmp[0]         = cgi.MSG_ReadCoord();
            vTmp[1]         = cgi.MSG_ReadCoord();
            vTmp[2]         = cgi.MSG_ReadCoord();
            vStart[0]       = cgi.MSG_ReadCoord();
            vStart[1]       = cgi.MSG_ReadCoord();
            vStart[2]       = cgi.MSG_ReadCoord();
            vEndArray[0][0] = cgi.MSG_ReadCoord();
            vEndArray[0][1] = cgi.MSG_ReadCoord();
            vEndArray[0][2] = cgi.MSG_ReadCoord();
            iLarge          = cgi.MSG_ReadBits(1);

            CG_MakeBulletTracer(vTmp, vStart, vEndArray, 1, iLarge, qtrue, qtrue);
            break;

        case CGM6_BULLET_NO_BARREL_2:
            memset(vTmp, 0, sizeof(vTmp));
            vStart[0]       = cgi.MSG_ReadCoord();
            vStart[1]       = cgi.MSG_ReadCoord();
            vStart[2]       = cgi.MSG_ReadCoord();
            vEndArray[0][0] = cgi.MSG_ReadCoord();
            vEndArray[0][1] = cgi.MSG_ReadCoord();
            vEndArray[0][2] = cgi.MSG_ReadCoord();
            iLarge          = cgi.MSG_ReadBits(1);

            CG_MakeBulletTracer(vTmp, vStart, vEndArray, 1, iLarge, qfalse, qtrue);
            break;

        case CGM6_HUDDRAW_SHADER:
            iInfo = cgi.MSG_ReadByte();
            Q_strncpyz(
                cgi.HudDrawElements[iInfo].shaderName,
                cgi.MSG_ReadString(),
                sizeof(cgi.HudDrawElements[iInfo].shaderName)
            );
            cgi.HudDrawElements[iInfo].string[0]   = 0;
            cgi.HudDrawElements[iInfo].pFont       = NULL;
            cgi.HudDrawElements[iInfo].fontName[0] = 0;
            // set the shader
            CG_HudDrawShader(iInfo);
            break;

        case CGM6_HUDDRAW_ALIGN:
            iInfo                                       = cgi.MSG_ReadByte();
            cgi.HudDrawElements[iInfo].iHorizontalAlign = cgi.MSG_ReadBits(2);
            cgi.HudDrawElements[iInfo].iVerticalAlign   = cgi.MSG_ReadBits(2);
            break;

        case CGM6_HUDDRAW_RECT:
            iInfo                              = cgi.MSG_ReadByte();
            cgi.HudDrawElements[iInfo].iX      = cgi.MSG_ReadShort();
            cgi.HudDrawElements[iInfo].iY      = cgi.MSG_ReadShort();
            cgi.HudDrawElements[iInfo].iWidth  = cgi.MSG_ReadShort();
            cgi.HudDrawElements[iInfo].iHeight = cgi.MSG_ReadShort();
            break;

        case CGM6_HUDDRAW_VIRTUALSIZE:
            iInfo                                     = cgi.MSG_ReadByte();
            cgi.HudDrawElements[iInfo].bVirtualScreen = cgi.MSG_ReadBits(1);
            break;

        case CGM6_HUDDRAW_COLOR:
            iInfo                                = cgi.MSG_ReadByte();
            cgi.HudDrawElements[iInfo].vColor[0] = cgi.MSG_ReadByte() / 255.0;
            cgi.HudDrawElements[iInfo].vColor[1] = cgi.MSG_ReadByte() / 255.0;
            cgi.HudDrawElements[iInfo].vColor[2] = cgi.MSG_ReadByte() / 255.0;
            break;

        case CGM6_HUDDRAW_ALPHA:
            iInfo                                = cgi.MSG_ReadByte();
            cgi.HudDrawElements[iInfo].vColor[3] = cgi.MSG_ReadByte() / 255.0;
            break;

        case CGM6_HUDDRAW_STRING:
            iInfo                              = cgi.MSG_ReadByte();
            cgi.HudDrawElements[iInfo].hShader = 0;
            Q_strncpyz(
                cgi.HudDrawElements[iInfo].string, cgi.MSG_ReadString(), sizeof(cgi.HudDrawElements[iInfo].string)
            );
            break;

        case CGM6_HUDDRAW_FONT:
            iInfo = cgi.MSG_ReadByte();
            Q_strncpyz(
                cgi.HudDrawElements[iInfo].fontName, cgi.MSG_ReadString(), sizeof(cgi.HudDrawElements[iInfo].fontName)
            );
            cgi.HudDrawElements[iInfo].hShader       = 0;
            cgi.HudDrawElements[iInfo].shaderName[0] = 0;
            // load the font
            CG_HudDrawFont(iInfo);
            break;

        case CGM6_NOTIFY_HIT:
        case CGM6_NOTIFY_KILL:
            if (cg.snap) {
                int iOldEnt;

                iOldEnt = current_entity_number;

                current_entity_number = cg.snap->ps.clientNum;
                // HZM coop [2026-08-28] the marker rides the SAME message as the cue, so the pip and
                // the tick can never disagree about whether a hit landed.
                extern void CG_CoopHitMark(qboolean bKill);
                CG_CoopHitMark((iType == CGM6_NOTIFY_KILL) ? qtrue : qfalse);

                if (iType == CGM6_NOTIFY_KILL) {
                    commandManager.PlaySound("dm_kill_notify", NULL, CHAN_LOCAL, 2.0, -1, -1, 1);
                } else {
                    commandManager.PlaySound("dm_hit_notify", NULL, CHAN_LOCAL, 2.0, -1, -1, 1);
                }

                current_entity_number = iOldEnt;
            }
            break;

        case CGM6_VOICE_CHAT:
            {
                int iOldEnt;

                vStart[0] = cgi.MSG_ReadCoord();
                vStart[1] = cgi.MSG_ReadCoord();
                vStart[2] = cgi.MSG_ReadCoord();
                iLarge    = cgi.MSG_ReadBits(1);
                iInfo     = cgi.MSG_ReadBits(6);
                szTmp     = cgi.MSG_ReadString();

                iOldEnt = current_entity_number;

                if (iLarge) {
                    current_entity_number = iInfo;

                    commandManager.PlaySound(szTmp, vStart, CHAN_LOCAL, -1, -1, -1, 0);
                } else if (cg.snap) {
                    current_entity_number = cg.snap->ps.clientNum;

                    commandManager.PlaySound(szTmp, vStart, CHAN_LOCAL, -1, -1, -1, 1);
                }

                current_entity_number = iOldEnt;
            }
            break;
        default:
            cgi.Error(ERR_DROP, "CG_ParseCGMessage: Unknown CGM message type");
            break;
        }

        bMoreCGameMessages = cgi.MSG_ReadBits(1);
    }
}

void CG_InitCGMessageAPI(clientGameExport_t *cge)
{
    if (cg_protocol >= PROTOCOL_MOHTA_MIN) {
        cge->CG_ParseCGMessage = &CG_ParseCGMessage_ver_15;
    } else {
        cge->CG_ParseCGMessage = &CG_ParseCGMessage_ver_6;
    }
}
