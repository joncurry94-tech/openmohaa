/*
===========================================================================
Copyright (C) 2023 the OpenMoHAA team

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
// Nature effects

#include "cg_local.h"
#include "cg_commands.h"

cvar_t *cg_rain;
cvar_t *cg_rain_drawcoverage;

void RainTouch(ctempmodel_t *ct, trace_t *trace)
{
    Vector norm, neworg;

    ct->ent.hModel    = cgi.R_RegisterModel("splash_z.spr");
    ct->cgd.velocity  = vec_zero;
    ct->cgd.accel     = vec_zero;
    ct->killTime      = cg.time + 400;
    norm              = trace->plane.normal;
    norm.x            = -norm.x;
    norm.y            = -norm.y;
    ct->cgd.angles    = norm.toAngles();
    ct->ent.scale     = 0.3f;
    ct->cgd.scaleRate = 4.0;
    ct->cgd.flags |= T_FADE;

    neworg = trace->endpos + norm * 0.2f;
    VectorCopy(neworg, ct->ent.origin);
}

void ClientGameCommandManager::RainTouch(Event *ev)
{
    // Nothing to do
}

void ClientGameCommandManager::InitializeRainCvars()
{
    int i;

    cg_rain = cgi.Cvar_Get("cg_rain", "1", CVAR_ARCHIVE);
    cg_rain_drawcoverage =
        cgi.Cvar_Get("cg_rain_drawcoverage", "0", CVAR_SAVEGAME | CVAR_RESETSTRING | CVAR_SYSTEMINFO);

    cg.rain.density    = 0.0;
    cg.rain.speed      = 2048.0f;
    cg.rain.length     = 90.0f;
    cg.rain.min_dist   = 512.0f;
    cg.rain.width      = 1.0f;
    cg.rain.speed_vary = 512;
    cg.rain.slant      = 50;

    for (i = 0; i < MAX_RAIN_SHADERS; i++) {
        cg.rain.shader[i][0] = 0;
    }

    cg.rain.numshaders = 0;
}

// HZM coop - set when the legacy entity-based rain (a map rain brush) renders, so CG_RainGlobal below stands
// down and we never double up rain on maps that already author it.
int g_lastRainEntityTime = 0;

/*
====================
CG_RainBrushDropVisible

HZM coop [tester report, bug-3466] "rain goes through roofs". Every roof test the coop rain has (the per-drop sky
gate, the wall clamp) lives in CG_RainGlobal, the path for maps WITHOUT rain brushes. A map that ships its own
func_rain brushes is drawn by the retail CG_Rain below, which tested nothing: a drop starts anywhere in the brush
box - roofs, upper floors and the street are all inside it - and its streak runs to the brush bottom through
whatever is there (57 of 368 roofed standing spots on m5l1b are inside a rain brush or its drift).

Same two tests as the brushless path, per drop: straight up from the drop's start must reach sky
(coop_rainSkyStrict 1 = must land on a SURF_SKY surface), and the streak is cut at the first solid it meets.
coop_rainBrushRoof 0 = the retail behaviour. e3l1 / e3l2 are left exactly as shipped (the user's standing rule
for the Omaha-family maps is that their look is not touched without asking).
====================
*/
static qboolean CG_RainBrushDropVisible(const vec3_t vStart, vec3_t vEnd, int *piLife)
{
    static cvar_t *pOn = NULL, *pStrict = NULL;
    vec3_t         vZero = {0.0f, 0.0f, 0.0f};
    vec3_t         vSkyEnd;
    trace_t        tr;

    if (!pOn) {
        pOn     = cgi.Cvar_Get("coop_rainBrushRoof", "1", CVAR_ARCHIVE);
        pStrict = cgi.Cvar_Get("coop_rainSkyStrict", "1", CVAR_ARCHIVE);
    }
    if (!pOn->integer) {
        return qtrue;
    }
    // not cached: a static could outlive the map it was worked out for, and this is nothing next to two traces
    if (strstr(cgs.mapname, "e3l1") || strstr(cgs.mapname, "e3l2")) {
        return qtrue;
    }

    // SKY GATE: straight up from the drop's own start
    VectorCopy(vStart, vSkyEnd);
    vSkyEnd[2] += 16384.0f;
    cgi.CM_BoxTrace(&tr, vStart, vSkyEnd, vZero, vZero, 0, MASK_SOLID, qfalse);
    if (pStrict->integer) {
        if (!(tr.surfaceFlags & SURF_SKY)) {
            return qfalse;
        }
    } else if (!(tr.surfaceFlags & SURF_SKY) && tr.fraction < 0.999f) {
        return qfalse;
    }

    // WALL CLAMP: cut the streak at the first solid surface (roof, wall, ground)
    cgi.CM_BoxTrace(&tr, vStart, vEnd, vZero, vZero, 0, MASK_SOLID, qfalse);
    if (tr.fraction < 1.0f) {
        if (tr.fraction < 0.1f) {
            return qfalse;
        }
        VectorCopy(tr.endpos, vEnd);
        *piLife = (int)(*piLife * tr.fraction);
        if (*piLife < 50) {
            return qfalse;
        }
    }
    return qtrue;
}

void CG_Rain(centity_t *cent)
{
    int         iLife;
    vec3_t      mins, maxs;

    g_lastRainEntityTime = cg.time; // a rain brush exists on this map
    vec3_t      vOmins, vOmaxs, vOe;
    float       fcolor[4];
    vec3_t      vStart, vEnd;
    int         iNumSpawn;
    int         i;
    int         iRandom;
    float       fDensity;
    vec3_t      vLength;
    const char *shadername;

    fcolor[0] = 1.0;
    fcolor[1] = 1.0;
    fcolor[2] = 1.0;
    fcolor[3] = 1.0;

    if (!cg_rain->integer || paused->integer) {
        return;
    }

    cgi.R_ModelBounds(cgs.inlineDrawModel[cent->currentState.modelindex], mins, maxs);

    // Fixed in 2.0
    //  Use cg.refdef.vieworg instead of cg.snap.ps.origin

    vOmins[0] = mins[0] + cent->lerpOrigin[0];
    if (vOmins[0] < cg.refdef.vieworg[0] - cg.rain.min_dist) {
        vOmins[0] = cg.refdef.vieworg[0] - cg.rain.min_dist;
    }

    vOmins[1] = mins[1] + cent->lerpOrigin[1];
    if (vOmins[1] < cg.refdef.vieworg[1] - cg.rain.min_dist) {
        vOmins[1] = cg.refdef.vieworg[1] - cg.rain.min_dist;
    }

    vOmins[2] = mins[2] + cent->lerpOrigin[2];

    vOmaxs[0] = maxs[0] + cent->lerpOrigin[0];
    if (vOmaxs[0] > cg.refdef.vieworg[0] + cg.rain.min_dist) {
        vOmaxs[0] = cg.refdef.vieworg[0] + cg.rain.min_dist;
    }

    vOmaxs[1] = maxs[1] + cent->lerpOrigin[1];
    if (vOmaxs[1] > cg.refdef.vieworg[1] + cg.rain.min_dist) {
        vOmaxs[1] = cg.refdef.vieworg[1] + cg.rain.min_dist;
    }

    vOmaxs[2] = maxs[2] + cent->lerpOrigin[2];

    if (vOmins[0] > vOmaxs[0]) {
        return;
    }

    if (vOmins[1] > vOmaxs[1]) {
        return;
    }

    if (cg_rain_drawcoverage->integer) {
        cgi.R_DebugLine(
            Vector(vOmins[0], vOmins[1], vOmins[2]), Vector(vOmaxs[0], vOmins[1], vOmins[2]), 1.0, 0.0, 0.0, 1.0
        );

        cgi.R_DebugLine(
            Vector(vOmaxs[0], vOmins[1], vOmins[2]), Vector(vOmaxs[0], vOmaxs[1], vOmins[2]), 1.0, 0.0, 0.0, 1.0
        );

        cgi.R_DebugLine(
            Vector(vOmaxs[0], vOmaxs[1], vOmins[2]), Vector(vOmins[0], vOmaxs[1], vOmins[2]), 1.0, 0.0, 0.0, 1.0
        );

        cgi.R_DebugLine(
            Vector(vOmins[0], vOmaxs[1], vOmins[2]), Vector(vOmins[0], vOmins[1], vOmins[2]), 1.0, 0.0, 0.0, 1.0
        );
    }

    VectorSubtract(vOmaxs, vOmins, vOe);
    fDensity  = cg.rain.density / 200.0;
    iNumSpawn = (int)(sqrt(vOe[0] * vOe[1]) * fDensity);
    if (iNumSpawn > MAX_BEAMS) {
        iNumSpawn = MAX_BEAMS;
    }

    iRandom = rand();

    if (cg.rain.numshaders) {
        shadername = cg.rain.shader[iRandom % cg.rain.numshaders];
    } else {
        shadername = cg.rain.shader[0];
    }

    for (i = 0; i < iNumSpawn; ++i) {
        vStart[0] = (float)(iRandom % (int)(vOe[0] + 1.0)) + vOmins[0];
        iRandom   = ((214013 * iRandom + 2531011) >> 16) & 0x7FFF;
        vStart[1] = (float)(iRandom % (int)(vOe[1] + 1.0)) + vOmins[1];
        iRandom   = ((214013 * iRandom + 2531011) >> 16) & 0x7FFF;
        // Fixed in 2.0
        //  Use random height instead of the max height to avoid waiting for it to fall down
        vStart[2] = (float)(iRandom % (int)(vOe[2] + 1.0)) + vOmins[2];

        VectorSubtract(cg.refdef.vieworg, vStart, vLength);
        vLength[2] = 0;

        if (VectorLengthSquared(vLength) > Square(cg.rain.min_dist)) {
            continue;
        }

        iRandom = ((214013 * iRandom + 2531011) >> 16) & 0x7FFF;

        vEnd[0] = (float)(iRandom % cg.rain.slant) + vStart[0] + vss_wind_x->value;
        iRandom = ((214013 * iRandom + 2531011) >> 16) & 0x7FFF;
        vEnd[1] = (float)(iRandom % cg.rain.slant) + vStart[1] + vss_wind_y->value;
        vEnd[2] = vOmins[2];

        // Fixed in 2.0
        //  Use individual particle's origin to determine the life rather than the bounding box
        iLife = (int)((vStart[2] - vOmins[2]) / ((float)(iRandom % cg.rain.speed_vary) + cg.rain.speed) * 1000.0);
        if (iLife > 10000) {
            // Fixed in 2.0
            //  Rain particles must not last longer than 10 seconds
            iLife = 10000;
        }

        // HZM coop [bug-3466] the brush path had no world test at all
        if (!CG_RainBrushDropVisible(vStart, vEnd, &iLife)) {
            continue;
        }

        CG_CreateBeam(
            vStart,
            vec_zero,
            0,
            1,
            1.0,
            cg.rain.width,
            BEAM_INVERTED_FAST,
            1000.0,
            iLife,
            qtrue,
            vEnd,
            0,
            0,
            0,
            1,
            0,
            shadername,
            fcolor,
            0,
            0.0,
            cg.rain.length,
            1.0,
            0,
            "raineffect"
        );
    }
}

// HZM coop - GLOBAL dynamic precipitation. Vanilla CG_Rain only renders inside a map-placed rain BRUSH
// (most maps don't have one) and never checks for ceilings. This renders rain/snow in a box around the
// player WITHOUT a brush (so dynamic weather works on any map), and SKY-GATES every column with an up-trace
// (SURF_SKY = open sky) so drops never fall indoors / under roofs / overhangs. Driven by the same level.rain_*
// params the coop weather script ramps. Gated by coop_dynRainGlobal so legacy rain-brush maps are untouched.
void CG_RainGlobal(void)
{
    static cvar_t *pGlobal = NULL;
    int            iNumSpawn, i, iRandom, iLife, iSlant, iSpeedVary;
    float          fDensity;
    float          fcolor[4];
    vec3_t         vOmins, vOmaxs, vOe, vStart, vEnd, vLength;
    vec3_t         vZero = {0.0f, 0.0f, 0.0f};
    const char    *shadername;

    if (!pGlobal) {
        pGlobal = cgi.Cvar_Get("coop_dynRainGlobal", "1", CVAR_ARCHIVE); // master toggle (off if it misbehaves)
    }
    if (!pGlobal->integer || !cg_rain->integer || paused->integer) {
        return;
    }
    // Driven purely by the networked cg.rain.density so EVERY coop client sees the weather. 0 = clear.
    if (cg.rain.density <= 0.0f || cg.rain.min_dist <= 1.0f) {
        return;
    }
    // A map with its own rain brush drives the entity-based CG_Rain; stand down so we never double the rain.
    if (g_lastRainEntityTime && (cg.time - g_lastRainEntityTime) < 1000) {
        return;
    }

    // [user 07-11] HARD interior / vehicle cull. The per-drop sky-gate below uses cgi.CM_BoxTrace, which
    // only sees the WORLD (BSP) - so a VEHICLE roof (the truck is an ENTITY) or an entity/script-model roof
    // is invisible to it, and rain leaks into the truck and some interiors. Trace UP from the PLAYER (NOT
    // the camera - in 3rd person the camera sits above the roof and would see open sky) with an
    // entity-INCLUSIVE CG_Trace: if anything solid that isn't sky is above the player, kill the rain
    // entirely. The per-drop world gate still handles partial exposure when the player is genuinely outside.
    {
        trace_t roofTr;
        vec3_t  vpStart, vpEnd;
        vpStart[0] = cg.predicted_player_state.origin[0];
        vpStart[1] = cg.predicted_player_state.origin[1];
        vpStart[2] = cg.predicted_player_state.origin[2] + 40.0f; // ~chest height, off the floor
        vpEnd[0]   = vpStart[0];
        vpEnd[1]   = vpStart[1];
        // HZM coop [user 2026-08-24] RAIN INDOORS. The old test read
        //     if (fraction < 0.999 && !SURF_SKY) -> dry
        // i.e. "if we hit something and it is not sky, you are dry" - so when the trace hit NOTHING it
        // fell through and drew rain. "Hit nothing" was being treated as "open sky", which is why any
        // interior taller than the 4096u trace, or with a gap the trace escaped through, rained inside.
        //
        // Inverted: wet only if the trace ACTUALLY LANDS ON SKY. Lengthened to 16384 so a genuinely
        // tall outdoor map still finds its sky brush instead of going falsely dry.
        // coop_rainSkyStrict 0 restores the old behaviour if a map turns out to need it.
        vpEnd[2]   = vpStart[2] + 16384.0f;
        CG_Trace(&roofTr, vpStart, vZero, vZero, vpEnd, cg.snap->ps.clientNum, MASK_SOLID, qfalse, qtrue, "CG_RainRoof");
        {
            static cvar_t *pStrict = NULL;
            if (!pStrict) { pStrict = cgi.Cvar_Get("coop_rainSkyStrict", "1", CVAR_ARCHIVE); }
            if (pStrict->integer) {
                if (!(roofTr.surfaceFlags & SURF_SKY)) {
                    return; // no sky overhead -> dry, INCLUDING "hit nothing at all"
                }
            } else if (roofTr.fraction < 0.999f && !(roofTr.surfaceFlags & SURF_SKY)) {
                return;
            }
        }
    }

    fcolor[0] = fcolor[1] = fcolor[2] = fcolor[3] = 1.0f;

    // player-centred volume (no map brush needed)
    vOmins[0] = cg.refdef.vieworg[0] - cg.rain.min_dist;
    vOmins[1] = cg.refdef.vieworg[1] - cg.rain.min_dist;
    vOmins[2] = cg.refdef.vieworg[2] - 256.0f;
    vOmaxs[0] = cg.refdef.vieworg[0] + cg.rain.min_dist;
    vOmaxs[1] = cg.refdef.vieworg[1] + cg.rain.min_dist;
    vOmaxs[2] = cg.refdef.vieworg[2] + 640.0f;

    VectorSubtract(vOmaxs, vOmins, vOe);
    if (vOe[0] < 1.0f || vOe[1] < 1.0f || vOe[2] < 1.0f) {
        return;
    }

    fDensity  = cg.rain.density / 200.0f;
    iNumSpawn = (int)(sqrt(vOe[0] * vOe[1]) * fDensity);
    if (iNumSpawn > MAX_BEAMS) {
        iNumSpawn = MAX_BEAMS;
    }

    iSlant     = (cg.rain.slant < 1) ? 1 : cg.rain.slant;            // snow uses slant 1; guard div-by-zero
    iSpeedVary = (cg.rain.speed_vary < 1) ? 1 : cg.rain.speed_vary;  // guard div-by-zero

    iRandom    = rand();
    shadername = cg.rain.numshaders ? cg.rain.shader[iRandom % cg.rain.numshaders] : cg.rain.shader[0];

    for (i = 0; i < iNumSpawn; ++i) {
        trace_t skyTr;
        vec3_t  vSkyStart, vSkyEnd;

        vStart[0] = (float)(iRandom % (int)(vOe[0] + 1.0f)) + vOmins[0];
        iRandom   = ((214013 * iRandom + 2531011) >> 16) & 0x7FFF;
        vStart[1] = (float)(iRandom % (int)(vOe[1] + 1.0f)) + vOmins[1];
        iRandom   = ((214013 * iRandom + 2531011) >> 16) & 0x7FFF;
        vStart[2] = (float)(iRandom % (int)(vOe[2] + 1.0f)) + vOmins[2];

        VectorSubtract(cg.refdef.vieworg, vStart, vLength);
        vLength[2] = 0;
        if (VectorLengthSquared(vLength) > Square(cg.rain.min_dist)) {
            continue;
        }

        // SKY GATE: trace straight up this column from the DROP's own height; only rain where the sky is
        // genuinely open above it (so it stops dead under roofs/awnings and never falls into interiors).
        // HZM fix: was tracing from cg.refdef.vieworg[2] (the CAMERA height). In 3rd person the camera sits
        // above/behind the player, so when it rose above a building's roof the gate saw open sky and rained
        // even though the drops (spawned around the player, camera-256..+640) were UNDER the roof - the
        // "rain sometimes still goes through interiors" leak. Tracing from vStart[2] checks each drop's
        // actual exposure, camera position irrelevant.
        vSkyStart[0] = vStart[0];
        vSkyStart[1] = vStart[1];
        vSkyStart[2] = vStart[2];
        vSkyEnd[0]   = vStart[0];
        vSkyEnd[1]   = vStart[1];
        // same inversion as the per-player roof gate: a drop is drawn only if the column above it
        // actually reaches SKY. Previously a trace that hit nothing still drew the drop.
        vSkyEnd[2]   = vStart[2] + 16384.0f;
        cgi.CM_BoxTrace(&skyTr, vSkyStart, vSkyEnd, vZero, vZero, 0, MASK_SOLID, qfalse);
        {
            static cvar_t *pStrict2 = NULL;
            if (!pStrict2) { pStrict2 = cgi.Cvar_Get("coop_rainSkyStrict", "1", CVAR_ARCHIVE); }
            if (pStrict2->integer) {
                if (!(skyTr.surfaceFlags & SURF_SKY)) {
                    continue; // no sky in this column -> no drop
                }
            } else if (!(skyTr.surfaceFlags & SURF_SKY) && skyTr.fraction < 0.999f) {
                continue;
            }
        }

        iRandom = ((214013 * iRandom + 2531011) >> 16) & 0x7FFF;
        vEnd[0] = (float)(iRandom % iSlant) + vStart[0] + vss_wind_x->value;
        iRandom = ((214013 * iRandom + 2531011) >> 16) & 0x7FFF;
        vEnd[1] = (float)(iRandom % iSlant) + vStart[1] + vss_wind_y->value;
        vEnd[2] = vOmins[2];

        iLife = (int)((vStart[2] - vOmins[2]) / ((float)(iRandom % iSpeedVary) + cg.rain.speed) * 1000.0f);
        if (iLife > 10000) {
            iLife = 10000;
        }

        // HZM coop - WALL CLAMP: the sky gate above only checks the drop's START column, but rain streaks
        // slant up to rain_slant (250) horizontal units as they fall (+ wind) and beams render THROUGH
        // geometry - so drops spawned legitimately outside a building knifed diagonally through the roof /
        // walls into interiors (user screenshot: rain inside a closed room). Trace along the streak and cut
        // it at the first solid surface so rain visibly stops at walls and roofs. Snow (slant 1) barely
        // drifts, so this mostly no-ops there.
        {
            trace_t wallTr;
            cgi.CM_BoxTrace(&wallTr, vStart, vEnd, vZero, vZero, 0, MASK_SOLID, qfalse);
            if (wallTr.fraction < 1.0f) {
                if (wallTr.fraction < 0.1f) {
                    continue; // streak would be a stub - skip the drop entirely
                }
                VectorCopy(wallTr.endpos, vEnd);
                iLife = (int)(iLife * wallTr.fraction); // die on impact so the visible speed stays constant
                if (iLife < 50) {
                    continue;
                }
            }
        }

        CG_CreateBeam(
            vStart, vec_zero, 0, 1, 1.0, cg.rain.width, BEAM_INVERTED_FAST, 1000.0, iLife, qtrue,
            vEnd, 0, 0, 0, 1, 0, shadername, fcolor, 0, 0.0, cg.rain.length, 1.0, 0, "raineffect"
        );
    }
}
