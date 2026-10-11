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

// tr_models.cpp -- model loading and caching

#include "tr_local.h"
#include "tiki.h"
#include <vector.h>

#define LL(x) x = LittleLong(x)

qboolean   g_bInfoworldtris = qfalse;
// HZM (bug-gl2-invisible-friendly-actor 2nd-cause audit): was [MAX_ENTITIES] (1023) but this is
// indexed in RB_SkelMesh by (backEnd.currentEntity - backEnd.refdef.entities), the refdef SLOT,
// which ranges 0..MAX_REFENTITIES-1 (4095) because gl2's refdef.entities[] is [MAX_REFENTITIES].
// gl1 kept refdef.entities[] at [MAX_ENTITIES] so it never overran; gl2 did not, so any skeletal
// actor in a refdef slot >= 1023 OOB-wrote the adjacent staticModelNumIndexes[]. Sized to match
// the refdef array. (All readers use ARRAY_LEN so this is safe.)
static int entityNumIndexes[MAX_REFENTITIES];
static int staticModelNumIndexes[4095];

static int R_CullSkelModel(dtiki_t *tiki, refEntity_t *e, skelAnimFrame_t *newFrame, float fScale, float *vLocalOrg);

// ============================================================================
// ^~^~^ SKELDIAG/SKELDRAW/SKELREG - TEMPORARY skeletal-actor render trace
// (bug-gl2-invisible-friendly-actor, 2nd independent cause). A single e2l2 boot reveals exactly
// where a character actor drops out of the gl2 skeletal path. DETERMINISTIC - NO in-game input:
//   ^~^~^ SKELREG  (R_RegisterModelInternal): one line per .tik model at load - did it register?
//   ^~^~^ SKELDIAG (R_AddSkelSurfaces): first time each model reaches the skeletal ADD path.
//   ^~^~^ SKELDRAW (RB_SkelMesh): first time each model reaches the backend DRAW (ext~0 = collapsed).
// Each of SKELDIAG/SKELDRAW auto-fires ONCE per model handle (dedup) with no cvar. The r_skeldiag
// cvar still exists as an OPTIONAL verbose per-frame mode (set r_skeldiag N) but is NOT required.
// A model that appears in SKELREG (registered) but never in SKELDIAG => it never reaches the
// renderer's skeletal submission (drop is upstream: cgame/dispatch, see SKELDISP in tr_main.c).
// REMOVE this block once the 2nd cause is identified and fixed.
// ============================================================================
static cvar_t *r_skeldiag           = NULL;

// HZM gl2 (bug-1153): the SKEL* forensics were written for the Phillips hunt (bug-1135, CLOSED) and
// several of them fire once per model handle with NO cvar gate, so every session spams the console
// and qconsole.log with SKELREG / SKELDIAG / SKELDRAW / SKELDISP / SKELAGG lines. They are still
// useful, so they are now gated behind the r_skeldiag cvar that already existed for the verbose
// per-frame mode - default 0, i.e. silent. `r_skeldiag 1` in a boot config brings them all back.
// (CVAR_TEMP, not CVAR_CHEAT: a listen server runs sv_cheats 0, which would clamp a cheat cvar
// straight back to 0 and make the diagnostic unusable - see bug-1148.)
static qboolean R_SkelDiagOn(void)
{
    if (!r_skeldiag) {
        r_skeldiag = ri.Cvar_Get("r_skeldiag", "0", CVAR_TEMP);
    }
    return (qboolean)(r_skeldiag->integer != 0);
}

// HZM [2026-09-27] console quiet (docs/proposals/console_quiet_2026-09-27): the deterministic one-shot-per-model
// SKELDRAW (and SKELDISP in tr_main.c) printed with no gate. r_skeltrace 1 restores them; the
// r_skeldiag N verbose window below is unchanged.
static qboolean R_SkelTraceOn(void)
{
    static cvar_t *r_skeltrace = NULL;

    if (!r_skeltrace) {
        r_skeltrace = ri.Cvar_Get("r_skeltrace", "0", CVAR_TEMP);
    }
    return (qboolean)(r_skeltrace->integer != 0);
}

static int     g_skeldiagFrameTag   = -1;   // tr.frame_skel_index of the frame currently bookkept
static int     g_skeldiagFramesLeft = 0;    // frames still to trace (armed from r_skeldiag, OPTIONAL)
static int     g_skeldiagFrontLines = 0;    // SKELDIAG verbose lines emitted this frame
static int     g_skeldiagBackLines  = 0;    // SKELDRAW verbose lines emitted this frame
static qboolean g_skeldiagInit      = qfalse;
// pose-decision handoff: R_UpdatePoseInternal records, R_AddSkelSurfaces reads
static int     g_skeldiagPoseRan     = 0;   // 1=posed fresh, 0=skip(already posed this frame), 3=forced over a reused pre-pass, -1=no entnum
static int     g_skeldiagPoseSkelIdx = 0;   // tr.skel_index[entnum] value read
static int     g_skeldiagPoseFrameIdx = 0;  // tr.frame_skel_index at that moment
static int     g_skeldiagPrePosed    = 0;   // 1 = a gl2 pre-pass (shadow/sun) already posed this entity this frame before the main add
static int     g_skeldiagMainPose    = 0;   // main-add pose decision (0=reused pre-pass, 1=fresh, 3=forced) - not clobbered by R_GetFrame
// backend dedup: last frame tag a given refdef slot was logged (verbose mode, one SKELDRAW per entity/frame)
static int     g_skeldrawSlotSeen[MAX_REFENTITIES];

// ============================================================================
// ^~^~^ SKELTEST - TEMPORARY live per-frame bisect toggles (default 0 = EXACT current behavior).
// The user flips these in the console while staring at an invisible LIVE enemy; whichever makes it
// POP INTO VIEW pinpoints the culprit code path. Each is a plain registered cvar (not latched), read
// every frame in the relevant path, and prints a one-line confirmation when its value changes.
//   r_test_noprepass 1  - skip char skeletal add in gl2 shadow/sun pre-passes (main view poses fresh, gl1-like)
//   r_test_twosided  1  - force char skeletal surfaces to CT_TWO_SIDED (disable backface cull)  [in tr_shade.c]
//   r_test_forcepose 1  - RE_ForceUpdatePose char models at the main add (never reuse a pre-pass pose)
//   r_test_forcelod0 1  - force full detail / no LOD reduction for char skeletal models
//   r_test_maskrfx   N  - strip suspect renderfx bits on char models: 1=RF_SHADOW_PRECISE(0x1000000),
//                         2=RF_SHADOW(0x800), 3=both shadow bits, 4=RF_FRAMELERP(0x10), 5=all three,
//                         other=literal bitmask
// REMOVE this block once the culprit is identified.
// ============================================================================
static cvar_t *r_test_noprepass = NULL;
static cvar_t *r_test_twosided  = NULL;
static cvar_t *r_test_forcepose = NULL;
static cvar_t *r_test_forcelod0 = NULL;
static cvar_t *r_test_maskrfx   = NULL;

static void R_SkelTest_Poll(void)
{
    static int lp[5] = {-999, -999, -999, -999, -999};
    cvar_t    *cv[5];
    const char *nm[5] = {"r_test_noprepass", "r_test_twosided", "r_test_forcepose", "r_test_forcelod0", "r_test_maskrfx"};
    int        i;

    if (!r_test_noprepass) {
        r_test_noprepass = ri.Cvar_Get("r_test_noprepass", "0", 0);
        r_test_twosided  = ri.Cvar_Get("r_test_twosided", "0", 0);
        r_test_forcepose = ri.Cvar_Get("r_test_forcepose", "1", 0); // PERMANENT FIX default-ON; set 0 for A/B only
        r_test_forcelod0 = ri.Cvar_Get("r_test_forcelod0", "0", 0);
        r_test_maskrfx   = ri.Cvar_Get("r_test_maskrfx", "0", 0);
    }
    cv[0] = r_test_noprepass; cv[1] = r_test_twosided; cv[2] = r_test_forcepose;
    cv[3] = r_test_forcelod0; cv[4] = r_test_maskrfx;
    for (i = 0; i < 5; i++) {
        if (cv[i]->integer != lp[i]) {
            lp[i] = cv[i]->integer;
            if (R_SkelDiagOn())
            ri.Printf(PRINT_ALL, "^~^~^ SKELTEST %s = %d (applied)\n", nm[i], lp[i]);
        }
    }
}

// Resolve the r_test_maskrfx preset into a renderfx bitmask to clear.
static int R_SkelTest_RfxMask(void)
{
    int v = r_test_maskrfx ? r_test_maskrfx->integer : 0;
    switch (v) {
    case 0:  return 0;
    case 1:  return 0x1000000;  // RF_SHADOW_PRECISE
    case 2:  return 0x800;      // RF_SHADOW
    case 3:  return 0x1000800;  // both shadow bits
    case 4:  return 0x10;       // RF_FRAMELERP
    case 5:  return 0x1000810;  // shadows + framelerp
    default: return v;          // literal bitmask
    }
}

// ============================================================================
// ^~^~^ SKELAGG - per-model AGGREGATE of ALL emitted skinned-vertex positions across every mesh/
// surface of a char model in a frame (model space AND world space). Resolves the long-standing ext
// ambiguity: a TINY aggregate extent => the skin genuinely COLLAPSES (skinning bug); a FULL-SIZE
// aggregate => the verts are fine and the actor is discarded DOWNSTREAM (depth/scissor/blend/alpha
// /view). Accumulated per hModel across the frame's surfaces and FLUSHED on the next frame's first
// surface (so the flushed value is a complete frame). Bounded to the first few frames per model.
// Keyed by hModel (0..MAX_MOD_KNOWN-1). REMOVE with the rest of the SKEL* scaffolding.
// ============================================================================
static int    g_aggFrame[MAX_MOD_KNOWN];
static int    g_aggFlushed[MAX_MOD_KNOWN];
static int    g_aggEnt[MAX_MOD_KNOWN];
static int    g_aggVerts[MAX_MOD_KNOWN];
static int    g_aggSurfs[MAX_MOD_KNOWN];
static vec3_t g_aggMMin[MAX_MOD_KNOWN], g_aggMMax[MAX_MOD_KNOWN]; // model space
static vec3_t g_aggWMin[MAX_MOD_KNOWN], g_aggWMax[MAX_MOD_KNOWN]; // world space
static int    g_skelVertsLines = 0;                              // bound per-surface SKELVERTS spam

static void R_SkelAgg_Add(int render_count, const vec3_t mmin, const vec3_t mmax, const vec3_t wmin, const vec3_t wmax)
{
    trRefEntity_t *e  = backEnd.currentEntity;
    int            hm = e->e.hModel;
    int            f  = g_skeldiagFrameTag;
    int            k;

    if (hm <= 0 || hm >= MAX_MOD_KNOWN) {
        return;
    }
    if (g_aggFrame[hm] != f) {
        // flush the just-completed previous frame's aggregate
        if (g_aggSurfs[hm] > 0 && g_aggFlushed[hm] < 6) {
            g_aggFlushed[hm]++;
            if (R_SkelDiagOn())
            ri.Printf(PRINT_ALL,
                "^~^~^ SKELAGG ent=%d hModel=%d model=%s surfs=%d verts=%d mext=[%.1f %.1f %.1f] "
                "mbnds=[%.1f %.1f %.1f]-[%.1f %.1f %.1f] wext=[%.1f %.1f %.1f] wctr=[%.0f %.0f %.0f]\n",
                g_aggEnt[hm], hm, (e->e.tiki && e->e.tiki->a) ? e->e.tiki->a->name : "?",
                g_aggSurfs[hm], g_aggVerts[hm],
                g_aggMMax[hm][0] - g_aggMMin[hm][0], g_aggMMax[hm][1] - g_aggMMin[hm][1], g_aggMMax[hm][2] - g_aggMMin[hm][2],
                g_aggMMin[hm][0], g_aggMMin[hm][1], g_aggMMin[hm][2], g_aggMMax[hm][0], g_aggMMax[hm][1], g_aggMMax[hm][2],
                g_aggWMax[hm][0] - g_aggWMin[hm][0], g_aggWMax[hm][1] - g_aggWMin[hm][1], g_aggWMax[hm][2] - g_aggWMin[hm][2],
                (g_aggWMin[hm][0] + g_aggWMax[hm][0]) * 0.5f, (g_aggWMin[hm][1] + g_aggWMax[hm][1]) * 0.5f,
                (g_aggWMin[hm][2] + g_aggWMax[hm][2]) * 0.5f);
        }
        // reset for the new frame
        g_aggFrame[hm] = f;
        g_aggEnt[hm]   = e->e.entityNumber;
        g_aggVerts[hm] = 0;
        g_aggSurfs[hm] = 0;
        for (k = 0; k < 3; k++) {
            g_aggMMin[hm][k] = g_aggWMin[hm][k] = 1e9f;
            g_aggMMax[hm][k] = g_aggWMax[hm][k] = -1e9f;
        }
    }
    for (k = 0; k < 3; k++) {
        if (mmin[k] < g_aggMMin[hm][k]) g_aggMMin[hm][k] = mmin[k];
        if (mmax[k] > g_aggMMax[hm][k]) g_aggMMax[hm][k] = mmax[k];
        if (wmin[k] < g_aggWMin[hm][k]) g_aggWMin[hm][k] = wmin[k];
        if (wmax[k] > g_aggWMax[hm][k]) g_aggWMax[hm][k] = wmax[k];
    }
    g_aggVerts[hm] += render_count;
    g_aggSurfs[hm]++;
}
// DETERMINISTIC one-shot-per-model dedup (keyed by refEntity hModel, 0..MAX_MOD_KNOWN-1)
static unsigned char g_skelSeenAdd[MAX_MOD_KNOWN];   // logged a SKELDIAG for this model handle
static unsigned char g_skelSeenDraw[MAX_MOD_KNOWN];  // logged a SKELDRAW for this model handle
// SKELREG running totals (register-time)
static int     g_skelregTotal = 0;
static int     g_skelregChar  = 0;
static int     g_skelregNull  = 0;

// Advance the trace window. Called once at the top of R_AddSkelSurfaces (frontend runs before the
// backend within a frame, so the decrement happens exactly once per frame). Returns whether the
// OPTIONAL verbose per-frame trace is active this frame.
static qboolean R_SkelDiag_FrameTick(void)
{
    if (!r_skeldiag) {
        r_skeldiag = ri.Cvar_Get("r_skeldiag", "0", CVAR_TEMP);
    }
    if (!g_skeldiagInit) {
        int i;
        for (i = 0; i < MAX_REFENTITIES; i++) {
            g_skeldrawSlotSeen[i] = -1;
        }
        g_skeldiagInit = qtrue;
    }
    if (tr.frame_skel_index != g_skeldiagFrameTag) {
        g_skeldiagFrameTag   = tr.frame_skel_index;
        g_skeldiagFrontLines = 0;
        g_skeldiagBackLines  = 0;
        if (r_skeldiag->integer > 0) {
            g_skeldiagFramesLeft = r_skeldiag->integer;  // (re)arm
            ri.Cvar_Set("r_skeldiag", "0");
        } else if (g_skeldiagFramesLeft > 0) {
            g_skeldiagFramesLeft--;
        }
    }
    return (qboolean)(g_skeldiagFramesLeft > 0);
}

// Should RB_SkelMesh emit a SKELDRAW for the current entity? True if the verbose trace is armed OR
// this model handle has not yet been draw-logged (deterministic one-shot). Does NOT mark seen.
static qboolean R_SkelDiag_DrawWanted(void)
{
    int hm;
    if (g_skeldiagFramesLeft > 0) {
        return qtrue;
    }
    hm = backEnd.currentEntity->e.hModel;
    return (qboolean)(R_SkelTraceOn() && hm > 0 && hm < MAX_MOD_KNOWN && !g_skelSeenDraw[hm]);
}

// Backend SKELDRAW emitter for RB_SkelMesh. Fires deterministically once per model handle, plus
// (in the optional verbose mode) once per entity per traced frame.
static void R_SkelDiag_Draw(int render_count, unsigned int dV, const char *bail, const vec3_t xyzMin, const vec3_t xyzMax)
{
    trRefEntity_t *e     = backEnd.currentEntity;
    int            slot  = (int)(e - backEnd.refdef.entities);
    int            hm    = e->e.hModel;
    qboolean       armed = (qboolean)(g_skeldiagFramesLeft > 0);
    qboolean       first = (qboolean)(R_SkelTraceOn() && hm > 0 && hm < MAX_MOD_KNOWN && !g_skelSeenDraw[hm]);
    float          ex, ey, ez;

    if (!armed && !first) {
        return;
    }
    if (!first) {
        // optional verbose mode: per-frame cap + one line per refdef slot per frame
        if (g_skeldiagBackLines >= 40) {
            return;
        }
        if (slot >= 0 && slot < MAX_REFENTITIES) {
            if (g_skeldrawSlotSeen[slot] == g_skeldiagFrameTag) {
                return;
            }
            g_skeldrawSlotSeen[slot] = g_skeldiagFrameTag;
        }
        g_skeldiagBackLines++;
    } else {
        g_skelSeenDraw[hm] = 1; // deterministic one-shot per model
    }

    ex = xyzMax ? (xyzMax[0] - xyzMin[0]) : 0.0f;
    ey = xyzMax ? (xyzMax[1] - xyzMin[1]) : 0.0f;
    ez = xyzMax ? (xyzMax[2] - xyzMin[2]) : 0.0f;

    ri.Printf(
        PRINT_ALL,
        "^~^~^ SKELDRAW ent=%d slot=%d hModel=%d model=%s rc=%d dV=%u ext=[%.1f %.1f %.1f] bail=%s%s\n",
        e->e.entityNumber,
        slot,
        hm,
        (e->e.tiki && e->e.tiki->a) ? e->e.tiki->a->name : "?",
        render_count,
        dV,
        ex, ey, ez,
        bail,
        first ? " FIRST" : "");
}

/*
** R_GetModelByHandle
*/
model_t *R_GetModelByHandle(qhandle_t hModel)
{
    model_t *mod;

    // out of range gets the default model
    if (hModel < 1 || hModel >= tr.numModels) {
        return tr.models[0];
    }

    mod = tr.models[hModel];

    return mod;
}

/*
** R_Model_GetHandle
*/
dtiki_t *R_Model_GetHandle(qhandle_t handle)
{
    model_t *model = R_GetModelByHandle(handle);

    if (model->type == MOD_TIKI) {
        return model->d.tiki;
    }

    return NULL;
}

//===============================================================================

/*
** R_FreeModel
*/
extern "C" void R_SkelCacheFlush(void);

void R_FreeModel(model_t *mod)
{
    R_SkelCacheFlush();     // HZM gl2 [bug-3483] the skin cache is keyed by surface pointers
    if (mod->type == MOD_TIKI) {
        ri.CG_EndTiki(mod->d.tiki);
    }

    memset(mod, 0, sizeof(*mod));
}

/*
** R_AllocModel
*/
model_t *R_AllocModel(void)
{
	model_t *mod;
    int i;

    for (i = 0; i < tr.numModels; i++) {
        mod = tr.models[i];
        if (!mod->name[0]) {
            break;
        }
    }

    if (i == tr.numModels) {
        if (i == MAX_MOD_KNOWN) {
            return NULL;
        }

        mod = (model_t*)ri.Hunk_Alloc(sizeof(*tr.models[tr.numModels]), h_low);
        mod->index = tr.numModels;
        tr.models[tr.numModels] = mod;
        tr.numModels++;
    } else {
        mod = tr.models[i];
        // HZM gl2 (bug-1131 ROOT CAUSE, invisible-actor coin flip): R_FreeModel memsets the
        // whole model_t including .index, and this reuse branch never re-stamped it - so the
        // next model registered into a freed slot loaded fine but returned handle 0
        // ("registration failed") to its caller and could never be drawn for the rest of the
        // session. gl1 re-stamps index on EVERY alloc (gl1 tr_model.cpp:102); match it.
        mod->index = i;
    }

    return mod;
}

/*
** RE_FreeModels
*/
void RE_FreeModels(void)
{
    int hModel;

    for (hModel = 0; hModel < tr.numModels; hModel++) {
        if (!tr.models[hModel]->name[0]) {
            continue;
        }

        R_FreeModel(tr.models[hModel]);
    }
}

/*
** R_RegisterShaders
*/
void R_RegisterShaders(model_t *mod)
{
    dtiki_t        *tiki;
    int             i, j;
    dtikisurface_t *psurface;
    shader_t       *sh;

    tiki = mod->d.tiki;

    for (i = 0; i < tiki->num_surfaces; i++) {
        psurface = &tiki->surfaces[i];

        assert(psurface->numskins <= MAX_TIKI_SHADER);
        for (j = 0; j < psurface->numskins; j++) {
            if (psurface->shader[j][0]) {
                sh                   = R_FindShader(psurface->shader[j], LIGHTMAP_NONE, qtrue);
                psurface->hShader[j] = sh->index;
            } else {
                psurface->hShader[j] = 0;
            }
        }
    }
}

/*
** RE_UnregisterServerModel
*/
void RE_UnregisterServerModel(qhandle_t hModel)
{
    if (hModel < 0 || hModel >= MAX_MOD_KNOWN) {
        return;
    }

    if (tr.models[hModel]->serveronly) {
        R_FreeModel(tr.models[hModel]);
    }
}

/*
** R_RegisterModelInternal
*/
static qhandle_t R_RegisterModelInternal(const char *name, qboolean bBeginTiki, qboolean use)
{
    model_t    *mod;
    qhandle_t   hModel;
    const char *ptr;

    if (!name || !*name) {
        ri.Printf(PRINT_ALL, "RE_RegisterModel: NULL name\n");
        return 0;
    }

    if (strlen(name) >= 128) {
        Com_Printf("Model name exceeds MAX_MODEL_NAME\n");
        return 0;
    }

    //
    // search the currently loaded models
    //
    for (hModel = 1; hModel < tr.numModels; hModel++) {
        mod = tr.models[hModel];
        if (!Q_stricmp(mod->name, name)) {
            if (mod->type == MOD_BAD) {
                return 0;
            }
            return hModel;
        }
    }

    // allocate a new model_t

    if ((mod = R_AllocModel()) == NULL) {
        ri.Printf(PRINT_WARNING, "RE_RegisterModel: R_AllocModel() failed for '%s'\n", name);
        return 0;
    }

    // only set the name after the model has been successfully loaded
    Q_strncpyz(mod->name, name, sizeof(mod->name));

    // make sure the render thread is stopped
    R_IssuePendingRenderCommands();

    mod->serveronly = qtrue;

    //
    // load the files
    //
    ptr = strrchr(name, '.');

    if (ptr) {
        ptr++;

        if (!stricmp(ptr, "spr")) {
            mod->d.sprite = SPR_RegisterSprite(name);
            Q_strncpyz(mod->name, name, sizeof(mod->name));

            if (mod->d.sprite) {
                mod->type = MOD_SPRITE;
                return mod->index;
            }
        } else if (!stricmp(ptr, "tik")) {
            mod->d.tiki = ri.TIKI_RegisterTikiFlags(name, use);
            Q_strncpyz(mod->name, name, sizeof(mod->name));

            if (mod->d.tiki) {
                mod->type = MOD_TIKI;
                R_RegisterShaders(mod);

                if (bBeginTiki) {
                    ri.CG_ProcessInitCommands(mod->d.tiki, NULL);
                }

                // ^~^~^ SKELREG (temporary, deterministic, load-time): one line per registered .tik.
                {
                    dtiki_t *dt      = mod->d.tiki;
                    int      isChar  = (dt->a) ? (dt->a->bIsCharacter ? 1 : 0) : -1;
                    int      m0surfs = 0, m0boxes = 0, m0bones = 0;
                    if (dt->numMeshes > 0) {
                        skelHeaderGame_t *sk = ri.TIKI_GetSkel(dt->mesh[0]);
                        if (sk) {
                            m0surfs = sk->numSurfaces;
                            m0boxes = sk->numBoxes;
                            m0bones = sk->numBones;
                        }
                    }
                    g_skelregTotal++;
                    if (isChar == 1) {
                        g_skelregChar++;
                    }
                    if (R_SkelDiagOn())
                    ri.Printf(PRINT_ALL,
                        "^~^~^ SKELREG model=%s handle=%d type=TIKI char=%d tikisurfs=%d meshes=%d m0surfs=%d m0boxes=%d m0bones=%d result=ok [tot=%d char1=%d null=%d]\n",
                        name, mod->index, isChar, dt->num_surfaces, dt->numMeshes, m0surfs, m0boxes, m0bones,
                        g_skelregTotal, g_skelregChar, g_skelregNull);
                }

                return mod->index;
            }

            // ^~^~^ SKELREG: a .tik that FAILED to load (tiki NULL) - this is exactly the "gl2 nulled
            // registration" case the hypothesis predicts. If allied character .tiks appear here, the
            // drop is at load; if they appear with result=ok above, the drop is downstream.
            g_skelregTotal++;
            g_skelregNull++;
            if (R_SkelDiagOn())
            ri.Printf(PRINT_ALL,
                "^~^~^ SKELREG model=%s handle=0 type=BAD result=nulled reason=tikiload [tot=%d char1=%d null=%d]\n",
                name, g_skelregTotal, g_skelregChar, g_skelregNull);
        }
    }

    ri.Printf(PRINT_ERROR, "RE_RegisterModel: Registration failed for '%s'\n", name);
    mod->type = MOD_BAD;

    return 0;
}

/*
** RE_RegisterServerModel
*/
qhandle_t RE_RegisterServerModel(const char *name)
{
    return R_RegisterModelInternal(name, qtrue, qfalse);
}

/*
** RE_SpawnEffectModel
*/
qhandle_t RE_SpawnEffectModel(const char *szModel, vec3_t vPos, vec3_t *axis)
{
    refEntity_t new_entity;

    memset(&new_entity, 0, sizeof(refEntity_t));
    memset(&new_entity.shaderRGBA, 255, sizeof(byte) * 4);

    VectorCopy(vPos, new_entity.origin);
    new_entity.scale = 1.0;

    if (axis) {
        AxisCopy(axis, new_entity.axis);
    }

    new_entity.hModel = R_RegisterModelInternal(szModel, qfalse, qtrue);

    if (new_entity.hModel) {
        tr.models[new_entity.hModel]->serveronly = qfalse;
        ri.CG_ProcessInitCommands(tr.models[new_entity.hModel]->d.tiki, &new_entity);
    }

    return new_entity.hModel;
}

/*
** RE_RegisterModel
*/
qhandle_t RE_RegisterModel(const char *name)
{
    qhandle_t handle;

    handle = R_RegisterModelInternal(name, qtrue, qtrue);

    if (handle) {
        tr.models[handle]->serveronly = qfalse;
    }
    return handle;
}

//=============================================================================

/*
===============
R_ModelInit
===============
*/
void R_ModelInit(void)
{
    model_t *mod;
    int      i;

    // leave a space for NULL model
    tr.numModels = 0;

    mod = R_AllocModel();
    Q_strncpyz(mod->name, "** BAD MODEL **", sizeof(mod->name));
    mod->type = MOD_BAD;

    for (i = 0; i < ARRAY_LEN(tr.skel_index); i++) {
        tr.skel_index[i] = -1;
    }
}

/*
================
R_Modellist_f
================
*/
void R_Modellist_f(void)
{
    int i;

    for (i = 1; i < tr.numModels; i++) {
        ri.Printf(PRINT_ALL, "%s\n", tr.models[i]->name);
    }
}

/*
====================
R_ModelRadius
====================
*/
float R_ModelRadius(qhandle_t handle)
{
    int      j;
    vec3_t   bounds[2];
    model_t *model;
    float    radius, maxRadius;
    vec3_t   tmpVec;
    float    w;

    model = R_GetModelByHandle(handle);

    switch (model->type) {
    case MOD_BRUSH:
        maxRadius = 0.0;

        VectorCopy(model->bmodel->bounds[0], bounds[0]);
        VectorCopy(model->bmodel->bounds[1], bounds[1]);

        for (j = 0; j < 8; j++) {
            tmpVec[0] = bounds[j & 1 ? 1 : 0][0];
            tmpVec[1] = bounds[j & 2 ? 1 : 0][1];
            tmpVec[2] = bounds[j & 4 ? 1 : 0][2];

            radius = VectorLength(tmpVec);

            if (maxRadius < radius) {
                maxRadius = radius;
            }
        }
        break;
    case MOD_TIKI:
        return ri.TIKI_GlobalRadius(model->d.tiki);
    case MOD_SPRITE:
        maxRadius = model->d.sprite->width * model->d.sprite->scale * 0.5;
        w         = model->d.sprite->height * model->d.sprite->scale * 0.5;

        if (maxRadius <= w) {
            maxRadius = w;
        }
        break;
    default:
        maxRadius = 0.0;
    }

    return maxRadius;
}

/*
====================
R_ModelBounds
====================
*/
void R_ModelBounds(qhandle_t handle, vec3_t mins, vec3_t maxs)
{
    model_t *model;

    model = R_GetModelByHandle(handle);

    switch (model->type) {
    default:
    case MOD_BAD:
        VectorClear(mins);
        VectorClear(maxs);
        break;
    case MOD_BRUSH:
        VectorCopy(model->bmodel->bounds[0], mins);
        VectorCopy(model->bmodel->bounds[1], maxs);
        break;
    case MOD_TIKI:
        ri.TIKI_CalculateBounds(model->d.tiki, 1.0, mins, maxs);
        break;
    case MOD_SPRITE:
        mins[0] = -model->d.sprite->width * model->d.sprite->scale * 0.5;
        mins[1] = -model->d.sprite->height * model->d.sprite->scale * 0.5;
        mins[2] = -0.0;
        maxs[0] = model->d.sprite->width * model->d.sprite->scale * 0.5;
        maxs[1] = model->d.sprite->height * model->d.sprite->scale * 0.5;
        maxs[2] = 0.0;
        break;
    }
}

#if 0
// Replaced by TIKI_FindSkelByHeader
/*
====================
GetModelPath
====================
*/
const char *GetModelPath( skelHeaderGame_t *skelmodel ) {
	int			i;
	int			num;
	skelcache_t	*cache;

	num = cache_numskel;

	for( i = 0; i < TIKI_MAX_SKELCACHE; i++ )
	{
		cache = &skelcache[ i ];

		if( cache->skel )
		{
			if( cache->skel == skelmodel ) {
				return cache->path;
			}

			num--;
			if( num < 0 ) {
				break;
			}
		}
	}

	return NULL;
}
#endif

/*
====================
GetLodCutoff
====================
*/
int GetLodCutoff(skelHeaderGame_t *skelmodel, float lod_val, int renderfx)
{
    lodControl_t *LOD;
    float         f;
    float         fLODCap;

    LOD = skelmodel->pLOD;

    if (renderfx & RF_DEPTHHACK) {
        fLODCap = LOD->maxMetric + (LOD->minMetric - LOD->maxMetric) * r_lodviewmodelcap->value;
    } else {
        f       = (LOD->minMetric - LOD->maxMetric) * r_lodcap->value + LOD->maxMetric;
        fLODCap = (lod_val - LOD->maxMetric) * r_lodscale->value + LOD->maxMetric;

        if (fLODCap > f) {
            fLODCap = f;
        }
    }

    if (fLODCap >= LOD->minMetric || !r_uselod->integer) {
        return LOD->curve[0].val;
    } else if (fLODCap <= LOD->maxMetric) {
        return LOD->curve[4].val;
    } else if (fLODCap <= LOD->consts[3].cutoff) {
        return fLODCap * LOD->consts[3].scale + LOD->consts[3].base;
    } else if (fLODCap <= LOD->consts[2].cutoff) {
        return fLODCap * LOD->consts[2].scale + LOD->consts[2].base;
    } else if (fLODCap <= LOD->consts[1].cutoff) {
        return fLODCap * LOD->consts[1].scale + LOD->consts[1].base;
    } else {
        return fLODCap * LOD->consts[0].scale + LOD->consts[0].base;
    }
}

/*
===============
R_SaveLODFile
===============
*/
static void R_SaveLODFile(const char *path, lodControl_t *LOD)
{
    fileHandle_t file = ri.FS_OpenFileWrite(path);
    if (!file) {
        ri.Printf(PRINT_WARNING, "SaveLODFile: Failed to open file %s\n", path);
        return;
    }

    ri.FS_Write(LOD, sizeof(lodControl_t), file);
}

/*
====================
GetToolLodCutoff
====================
*/
int GetToolLodCutoff(skelHeaderGame_t *skelmodel, float lod_val)
{
    lodControl_t *LOD;
    float         totalRange;
    int           i;
    char          lodPath[256];
    char         *ext;

    LOD        = skelmodel->pLOD;
    totalRange = 0.0;
    for (i = 0; i < 10; i++) {
        if (skelmodel->lodIndex[i] > 0) {
            totalRange = skelmodel->lodIndex[i];
            break;
        }
    }

    if (lod_save->integer == 1) {
        ri.Cvar_Set("lod_save", "0");
        Q_strncpyz(lodPath, ri.TIKI_FindSkelByHeader(skelmodel)->path, sizeof(lodPath));
        ext = strstr(lodPath, "skd");
        strcpy(ext, "lod");
        R_SaveLODFile(lodPath, LOD);
    }

    if (lod_mesh->modified) {
        lod_mesh->modified = qfalse;
        ri.Cvar_Set("lod_minLOD", va("%f", LOD->minMetric));
        ri.Cvar_Set("lod_maxLOD", va("%f", LOD->maxMetric));
        ri.Cvar_Set("lod_LOD_slider", va("%f", 0.5));
        ri.Cvar_Set("lod_curve_0_slider", va("%f", LOD->curve[0].val / totalRange));
        ri.Cvar_Set("lod_curve_1_slider", va("%f", LOD->curve[1].val / totalRange));
        ri.Cvar_Set("lod_curve_2_slider", va("%f", LOD->curve[2].val / totalRange));
        ri.Cvar_Set("lod_curve_3_slider", va("%f", LOD->curve[3].val / totalRange));
        ri.Cvar_Set("lod_curve_4_slider", va("%f", LOD->curve[4].val / totalRange));
    }

    ri.Cvar_Set("lod_curve_0_val", va("%f", lod_curve_0_slider->value * totalRange));
    ri.Cvar_Set("lod_curve_1_val", va("%f", lod_curve_1_slider->value * totalRange));
    ri.Cvar_Set("lod_curve_2_val", va("%f", lod_curve_2_slider->value * totalRange));
    ri.Cvar_Set("lod_curve_3_val", va("%f", lod_curve_3_slider->value * totalRange));
    ri.Cvar_Set("lod_curve_4_val", va("%f", lod_curve_4_slider->value * totalRange));

    LOD->minMetric    = lod_minLOD->value;
    LOD->maxMetric    = lod_maxLOD->value;
    LOD->curve[0].val = lod_curve_0_val->value;
    LOD->curve[1].val = lod_curve_1_val->value;
    LOD->curve[2].val = lod_curve_2_val->value;
    LOD->curve[3].val = lod_curve_3_val->value;
    LOD->curve[4].val = lod_curve_4_val->value;

    ri.TIKI_CalcLodConsts(LOD);
    return GetLodCutoff(skelmodel, lod_val, 0);
}

/*
==============
R_GetTagPositionAndOrientation
==============
*/
orientation_t R_GetTagPositionAndOrientation(refEntity_t *ent, int tagnum)
{
    int           i;
    orientation_t tag_or, new_or;

    tag_or = RE_TIKI_Orientation(ent, tagnum);

    VectorCopy(ent->origin, new_or.origin);

    for (i = 0; i < 3; i++) {
        VectorMA(new_or.origin, tag_or.origin[i], ent->axis[i], new_or.origin);
    }

    MatrixMultiply(tag_or.axis, ent->axis, new_or.axis);
    return new_or;
}

/*
==============
RB_DrawSkeletor
==============
*/
void RB_DrawSkeletor(trRefEntity_t *ent)
{
    // FIXME: Unimplemented (GL2)
}

surfaceType_t skelSurface = SF_TIKI_SKEL;

/*
==============
R_AddSkelSurfaces
==============
*/
void R_AddSkelSurfaces(trRefEntity_t *ent)
{
    dtiki_t         *tiki;
    qboolean         personalModel;
    float            tiki_scale;
    vec3_t           tiki_localorigin;
    vec3_t           tiki_worldorigin;
    skelBoneCache_t *outbones;
    //int tikiSurfNumOffset;
    static cvar_t   *vmEntity = NULL;
    skeletor_c      *skeletor;
    float            radius;
    SkelVec3         centroid;
    skelAnimFrame_t *newFrame;
    int              added;
    shader_t        *shader;
    dtikisurface_t  *dsurf;
    byte            *bsurf;
    //float range;
    //int render_count, total_tris;
    skelSurfaceGame_t *surface;
    //int skinnum;
    //float target;
    Vector newDistance;
    //vec3_t org;
    int i;
    int mesh;
    int iRadiusCull = 0;
    int num_tags;

    tiki = ent->e.tiki;

    // ^~^~^ SKELTRACK (bug-1131): continuous low-rate tracker for the e2l2 briefing ally -
    // proves whether the entity keeps REACHING this add path after a script teleport, and how
    // many draw surfs each visit contributes. REMOVE with the rest of the SKEL* scaffolding.
    int trkIsTarget  = 0;   // HZM 07-28: SKELTRACK off (was strstr per skeletal add, every frame)
    int trkDrawBefore = tr.refdef.numDrawSurfs;
    if (trkIsTarget) {
        static int trkAdd = 0;
        trkAdd++;
        if ((trkAdd & 31) == 1) {
            if (R_SkelDiagOn())
            ri.Printf(PRINT_ALL,
                "^~^~^ SKELTRACK addskel n=%d ent=%d org=[%d %d %d] vpFlags=0x%x portal=%d hModel=%d\n",
                trkAdd, ent->e.entityNumber,
                (int)ent->e.origin[0], (int)ent->e.origin[1], (int)ent->e.origin[2],
                (unsigned)tr.viewParms.flags, tr.viewParms.isPortal ? 1 : 0, ent->e.hModel);
        }
    }

    // ^~^~^ SKELDIAG (temporary): advance the OPTIONAL verbose window; compute the DETERMINISTIC
    // one-shot-per-model gate (fires with no in-game input); per-actor capture locals.
    qboolean diagOn      = R_SkelDiag_FrameTick();
    qboolean diagFirst   = (qboolean)(ent->e.hModel > 0 && ent->e.hModel < MAX_MOD_KNOWN && !g_skelSeenAdd[ent->e.hModel]);
    qboolean diagCapture = (qboolean)(diagOn || diagFirst);
    vec3_t   diagFMin, diagFMax;
    int      diagCullBox = -99;
    qboolean diagDegen   = qfalse;
    VectorClear(diagFMin);
    VectorClear(diagFMax);
    if (diagFirst) {
        g_skelSeenAdd[ent->e.hModel] = 1;
    }

    if (!vmEntity) {
        vmEntity = ri.Cvar_Get("viewmodelentity", "", 0);
    }

    // ^~^~^ SKELTEST: register/poll the live bisect toggles (prints on change). No-op at default 0.
    R_SkelTest_Poll();

    {
        qboolean isChar = (qboolean)(tiki->a && tiki->a->bIsCharacter);

        // r_test_noprepass: keep char models OUT of gl2's shadow / sun pre-passes (gl1 has none), so
        // the main view poses fresh and no pre-pass side effect touches the shared skeletor.
        if (isChar && r_test_noprepass && r_test_noprepass->integer
            && (tr.viewParms.flags & (VPF_DEPTHSHADOW | VPF_SHADOWMAP))) {
            return;
        }

        // ------------------------------------------------------------------------------
        // HZM gl2 REAL CHARACTER SHADOWS (r_charShadows) - frontend caster budget.
        //
        // Runs ONLY when the master cvar is on. With r_charShadows 0 this whole block is
        // skipped and the frontend behaves exactly as it does today (characters are still
        // added to every cascade and still dropped in the backend) - so the feature cannot
        // perturb the tr.skel_index pose-cache ordering that caused three separate
        // invisible-actor bugs in this fork.
        //
        // Placed BEFORE the RE_ForceUpdatePose call below on purpose: a rejection here also
        // saves the pose, the bone-pool allocation and the drawsurf emission, so cascades
        // above r_charShadowCascade become CHEAPER than they are today, not more expensive.
        //
        // WHY cascade 3 is excluded by default: it is the WHOLE-MAP cascade (tens of world
        // units per texel - a human is sub-texel sparkle) AND it is rendered once and cached
        // for as long as the sun direction is unchanged (tr_scene.c). MOHAA's sun is static
        // per map, so admitting characters there would bake frozen silhouettes at their
        // map-load positions that persist after the actors move or die.
        if (isChar && r_charShadows && r_charShadows->integer
            && (tr.viewParms.flags & VPF_DEPTHSHADOW)) {
            int lvl = tr.viewParms.shadowCascade - 1;   // 0 means "not a sun cascade view"

            // honour the engine's existing per-entity opt-out (see tr_main.c pshadows);
            // gives the fgame/script layer a free way to silence a specific caster.
            if (ent->e.renderfx & RF_NOSHADOW) {
                return;
            }
            // HZM gl2 bug-3009 (E1 / C0, r_hzmPshadowCharGate, plan_P3_addendum_B0 section 2): the cascade-level and
            // distance tests are for SUN cascades only. With the gate on, every other depth view admits characters -
            // the dlight pshadows (VPF_PSHADOW) and r_shadows 4 (VPF_NONE extra, tr_main.c) alike; keyed on "not a
            // sun cascade", not on the VPF_PSHADOW flag, which r_shadows 4 never sets. RF_NOSHADOW above still holds.
            // Gate off (-1 = auto 0): exactly as before.
            if (lvl >= 0 || !R_PshadowCharGate()) {
                // P3: the stable cascades are RADIAL - admission by distance only (r_charShadowDist); the level cap
                // would drop every soldier past cascade 1's radius
                if (lvl < 0 || (!(tr.viewParms.flags & VPF_SUNSTABLE) && lvl > r_charShadowCascade->integer)) {
                    return;
                }
                if (r_charShadowDist->value > 0.0f) {
                    vec3_t d;
                    VectorSubtract(ent->e.origin, tr.refdef.vieworg, d);
                    if (DotProduct(d, d) > r_charShadowDist->value * r_charShadowDist->value) {
                        return;
                    }
                }
            }
        }
        // ------------------------------------------------------------------------------

        // r_test_maskrfx: strip suspect renderfx bits (RF_SHADOW_PRECISE 0x1000000 / RF_SHADOW 0x800 /
        // RF_FRAMELERP 0x10) on char models. Transient: refents are rebuilt each frame.
        if (isChar) {
            int rfxMask = R_SkelTest_RfxMask();
            if (rfxMask) {
                ent->e.renderfx &= ~rfxMask;
            }
        }
    }

    // ^~^~^ pose capture + r_test_forcepose. gl2 runs shadow / sun pre-passes (VPF_DEPTHSHADOW/SHADOWMAP)
    // that gl1 does NOT have; the first such pass this frame poses the entity's skeletor and stamps
    // skel_index[entnum]=frame, so the main view reuses that pose (prePosed=1). r_test_forcepose makes
    // the main view re-pose char models regardless (RE_ForceUpdatePose). Default 0 = current behavior.
    {
        int en = ent->e.entityNumber;
        g_skeldiagPrePosed =
            (en >= 0 && en < MAX_GENTITIES && tr.skel_index[en] == tr.frame_skel_index) ? 1 : 0;
        g_skeldiagPoseFrameIdx = tr.frame_skel_index;
        g_skeldiagPoseSkelIdx  = (en >= 0 && en < MAX_GENTITIES) ? tr.skel_index[en] : -1;
    }
    // PERMANENT FIX (bug-gl2-invisible-friendly-actor / bug-gl2-invisible-live-char-depthprepass).
    // Always recompute the pose for EVERY skeletal model at the MAIN-view add so it never skins from a
    // STALE pose left by gl2's shadow/sun PRE-PASS (which gl1 does not have - the pre-pass poses the
    // shared skeletor first and stamps skel_index[entnum]=frame, so the default R_UpdatePoseInternal
    // would skip and reuse it -> wrong skinned depth -> LEQUAL kill -> invisible).
    //
    // The gate was previously `tiki->a->bIsCharacter`, but that FAILS for the e2l2 briefing squadmate:
    // he is dispatched as a COMPOSITE tiki ("weapon|m1 garand|...|sc_al_brit_cmd", hModel 961) whose
    // MERGED dtikianim does NOT carry bIsCharacter=true (even though the base sc_al_brit_cmd.tik is a
    // character via its include of new_generic_human.tik / `ischaracter`). So the force skipped him and
    // he reused a stale pre-pass pose (pose=SKIP) while the m3l2 Ranger composite - which DID carry
    // bIsCharacter - forced and rendered. Dropping the flag gate makes the force apply to the composite
    // ally too. RE_ForceUpdatePose is idempotent (re-poses from the entity's own frameInfo), so this
    // cannot regress the Ranger/enemies (still forced+visible) or non-character props/viewmodels (a
    // redundant re-pose to their own current pose). r_test_forcepose (default 1) can disable for A/B.
    // g_skeldiagMainPose is captured here because R_GetFrame's redundant R_UpdatePoseInternal call
    // clobbers g_skeldiagPoseRan afterwards.
    if (!r_test_forcepose || r_test_forcepose->integer) {
        RE_ForceUpdatePose(&ent->e);
        g_skeldiagMainPose = 3; // FORCED
    } else {
        R_UpdatePoseInternal(&ent->e);
        g_skeldiagMainPose = g_skeldiagPrePosed ? 0 /*reused pre-pass pose*/ : 1 /*posed fresh*/;
    }

    // don't add third_person objects if in a portal
    //
    // HZM gl2 real character shadows: the LOCAL PLAYER's own body carries RF_THIRD_PERSON in
    // first person precisely so it is not DRAWN through the eyes - but it must still CAST.
    // Sun cascade views set isPortal = qfalse, so without this the one actor the player looks
    // at most is the only one on screen with no shadow.
    // Safe: a shadow view is depth-only (the colour draw-surf list is skipped for shadow
    // views in RB_DrawSurfs), so no first-person geometry can reach the screen; and the
    // first-person WEAPON is excluded separately by RF_FIRST_PERSON + VPF_NOVIEWMODEL in
    // tr_main.c, which sun views set.
    personalModel = (ent->e.renderfx & RF_THIRD_PERSON) && !tr.viewParms.isPortal;
    if (personalModel && r_charShadows && r_charShadows->integer
        && (tr.viewParms.flags & VPF_DEPTHSHADOW)) {
        personalModel = qfalse;
    }

    outbones = &TIKI_Skel_Bones[TIKI_Skel_Bones_Index];

    num_tags = ri.TIKI_GetNumChannels(tiki);

    if (num_tags + TIKI_Skel_Bones_Index > MAX_SKELBONES) {
        if (diagCapture) {
            if (R_SkelDiagOn())
            ri.Printf(PRINT_ALL,
                "^~^~^ SKELDIAG-DROP ent=%d slot=%d model=%s reason=bonebufferfull tags=%d idx=%d\n",
                ent->e.entityNumber, (int)(ent - tr.refdef.entities), tiki->a->name, num_tags, TIKI_Skel_Bones_Index);
        }
        ri.Printf(PRINT_DEVELOPER, "R_AddSkelSurfaces: too many skeleton models visible on '%s'\n", tiki->a->name);
        return;
    }

    tiki_scale = tiki->load_scale * ent->e.scale;
    VectorScale(tiki->load_origin, tiki_scale, tiki_localorigin);
    R_LocalPointToWorld(tiki_localorigin, tiki_worldorigin);

    radius = R_GetRadius(&ent->e);

    if (!lod_tool->integer) {
        iRadiusCull = R_CullPointAndRadius(tiki_worldorigin, radius);
        if (r_showcull->integer & 2) {
            switch (iRadiusCull) {
            case CULL_IN:
                R_DebugCircle(tiki_worldorigin, radius * 1.2f, 0, 1, 0, 0.5f, qfalse);
                break;
            case CULL_CLIP:
                R_DebugCircle(tiki_worldorigin, radius * 1.2f, 0, 1, 0, 0.5f, qfalse);
                break;
            case CULL_OUT:
                R_DebugCircle(tiki_worldorigin, radius * 1.2f + 32.f, 1, 0.2f, 0.2f, 0.5f, qfalse);
                break;
            }
        }

        switch (iRadiusCull) {
        case CULL_IN:
            tr.pc.c_sphere_cull_md3_in++;
            break;
        case CULL_CLIP:
            tr.pc.c_sphere_cull_md3_clip++;
            break;
        case CULL_OUT:
            tr.pc.c_sphere_cull_md3_out++;
            break;
        }

        // HZM gl2 real character shadows: the tightest possible caster cull, for free.
        // In a sun cascade tr.viewParms IS the light view, so the iRadiusCull just computed
        // above already tested this entity against the cascade's own ortho box. Note
        // R_SetupProjectionOrtho sets VPF_FARPLANEFRUSTUM, so R_CullPointAndRadius used 5
        // planes: the four LATERAL planes plus the FAR plane, and there is deliberately NO
        // near plane - a caster sitting between the light and the box can therefore never be
        // wrongly discarded.
        // Today this result only gates the bone copy further down; the mesh loop emits
        // drawsurfs unconditionally. Returning here is what keeps each actor in ~1 cascade
        // instead of all of them.
        if (iRadiusCull == CULL_OUT && r_charShadows && r_charShadows->integer
            && (tr.viewParms.flags & VPF_DEPTHSHADOW)) {
            return;
        }

        // HZM gl2 [bug-3488] A SKELETAL MODEL OUTSIDE THE VIEW IS NOT DRAWN.
        // As the comment above says, the cull result only ever gated the bone copy: in the main view the mesh
        // loop below emitted every surface of every skeletal model in the PVS whether or not it was on screen
        // (the box test, R_CullSkelModel, is stubbed to CULL_IN - "FIXME: not working properly"). The back end
        // then skinned, uploaded and drew all of them for the GPU to clip away. Measured on Omaha that is the
        // whole frame: the same frame time looking at the battle or at a wall (tester report 2026-10-10).
        // The test is the sphere the function already has, enlarged well past the model (2x its radius plus
        // 96 units) so nothing with a long animation, a held weapon or an attachment can drop out at the edge
        // of the screen. Colour views and mirrors only (each view carries its own frustum); every shadow-map
        // view keeps exactly the behaviour it had. First-person and depth-hack models are never culled.
        // r_hzmSkelCull 0 = off (live).
        {
            static cvar_t *r_hzmSkelCull = NULL;
            if (!r_hzmSkelCull) {
                r_hzmSkelCull = ri.Cvar_Get("r_hzmSkelCull", "1", 0);
            }
            if (iRadiusCull == CULL_OUT && r_hzmSkelCull->integer
                && !(tr.viewParms.flags & (VPF_SHADOWMAP | VPF_DEPTHSHADOW | VPF_PSHADOW | VPF_SUNWORLD | VPF_SUNSTABLE))
                && !(ent->e.renderfx & (RF_FIRST_PERSON | RF_DEPTHHACK))
                && R_CullPointAndRadius(tiki_worldorigin, radius * 2.0f + 96.0f) == CULL_OUT) {
                return;
            }
        }
    }

    if (tiki->a->bIsCharacter) {
        if (tr.viewParms.isPortal) {
            ent->lodpercentage[1] = R_CalcLod(tiki_worldorigin, 92.f / ent->e.scale);
        } else {
            ent->lodpercentage[0] = R_CalcLod(tiki_worldorigin, 92.f / ent->e.scale);
        }
    } else {
        if (tr.viewParms.isPortal) {
            ent->lodpercentage[1] = R_CalcLod(tiki_worldorigin, radius / ent->e.scale);
        } else {
            ent->lodpercentage[0] = R_CalcLod(tiki_worldorigin, radius / ent->e.scale);
        }
    }

    newFrame = (skelAnimFrame_t *)ri.Hunk_AllocateTempMemory(
        sizeof(skelAnimFrame_t) + ri.TIKI_GetNumChannels(tiki) * sizeof(SkelMat4)
    );
    R_GetFrame(&ent->e, newFrame);

    // ^~^~^ SKELDIAG: capture the anim-frame bounds and a would-be box-cull result. R_CullSkelModel
    // is stubbed to CULL_IN (nothing here is actually culled), so this only reveals whether the skel
    // bounds are degenerate/empty for this actor.
    if (diagCapture) {
        vec3_t db[2];
        int    dk;
        for (dk = 0; dk < 3; dk++) {
            db[0][dk] = newFrame->bounds[0][dk] * tiki_scale + tiki_localorigin[dk];
            db[1][dk] = newFrame->bounds[1][dk] * tiki_scale + tiki_localorigin[dk];
        }
        VectorCopy(newFrame->bounds[0], diagFMin);
        VectorCopy(newFrame->bounds[1], diagFMax);
        diagCullBox = R_CullLocalBox(db);
        diagDegen   = (qboolean)((diagFMax[0] - diagFMin[0] < 0.5f) && (diagFMax[1] - diagFMin[1] < 0.5f)
                                 && (diagFMax[2] - diagFMin[2] < 0.5f));
    }

    skelBoneCache_t *coopCacheStart = outbones; // HZM coop ragdoll - Hook A writes this region

    if (lod_tool->integer || iRadiusCull != CULL_CLIP
        || R_CullSkelModel(tiki, &ent->e, newFrame, tiki_scale, tiki_localorigin) != CULL_OUT) {
        //
        // copy bones position and axis
        //
        for (i = 0; i < num_tags; i++) {
            VectorCopy(newFrame->bones[i][3], outbones->offset);
            outbones->matrix[0][0] = newFrame->bones[i][0][0];
            outbones->matrix[0][1] = newFrame->bones[i][0][1];
            outbones->matrix[0][2] = newFrame->bones[i][0][2];
            outbones->matrix[0][3] = 0;
            outbones->matrix[1][0] = newFrame->bones[i][1][0];
            outbones->matrix[1][1] = newFrame->bones[i][1][1];
            outbones->matrix[1][2] = newFrame->bones[i][1][2];
            outbones->matrix[1][3] = 0;
            outbones->matrix[2][0] = newFrame->bones[i][2][0];
            outbones->matrix[2][1] = newFrame->bones[i][2][1];
            outbones->matrix[2][2] = newFrame->bones[i][2][2];
            outbones->matrix[2][3] = 0;
            outbones++;
        }
    }

    // HZM coop - RAGDOLL Hook A (gl2 lockstep of the gl1 hook; see renderergl1/tr_model.cpp):
    // for flagged entities write EVERY channel from the override table unconditionally and
    // stash the vanilla frame as the anim-pose block. Pure reader; per-view safe.
    {
        struct ragdollSlot_s *coopSlot = R_RagdollSlotFor(ent->e.entityNumber, tiki);
        if (coopSlot) {
            R_RagdollApplyToCache(coopSlot, coopCacheStart, num_tags, newFrame);
        }
    }

    ri.Hunk_FreeTempMemory(newFrame);

    ent->e.bonestart = TIKI_Skel_Bones_Index;
    TIKI_Skel_Bones_Index += num_tags;

    ent->e.hasMorph = qfalse;

    //
    // get the skeletor
    //
    skeletor = (skeletor_c *)ri.TIKI_GetSkeletor(tiki, ent->e.entityNumber);

    //
    // add morphs
    //
    added = ri.SKEL_GetMorphWeightFrame(
        skeletor, ent->e.frameInfo[0].index, ent->e.frameInfo[0].time, &skeletorMorphCache[skeletorMorphCacheIndex]
    );
    ent->e.morphstart = skeletorMorphCacheIndex;

    if (added) {
        // found morphs
        skeletorMorphCacheIndex += added;
        // HZM gl2 [bug-3483] the COUNT, not just "yes": every reader tests it for non-zero, and the skin cache
        // needs to know how many morph weights belong to this entity to hash them
        ent->e.hasMorph = (qboolean)added;
    }

    //
    // draw all meshes
    //
    dsurf = tiki->surfaces;
    bsurf = &ent->e.surfaces[0];
    for (mesh = 0; mesh < tiki->numMeshes; mesh++) {
        skelHeaderGame_t *skelmodel = ri.TIKI_GetSkel(tiki->mesh[mesh]);

        if (!skelmodel) {
            if (diagCapture) {
                if (R_SkelDiagOn())
                ri.Printf(PRINT_ALL,
                    "^~^~^ SKELDIAG-DROP ent=%d slot=%d model=%s reason=noskelmodel mesh=%d\n",
                    ent->e.entityNumber, (int)(ent - tr.refdef.entities), tiki->a->name, mesh);
            }
            ri.Printf(PRINT_DEVELOPER, "R_AddSkelSurfaces: couldn't get skel model in '%s'\n", tiki->a->name);
            return;
        }

        if (lod_tool->integer && !stricmp(ent->e.tiki->a->name, lod_tikiname->string)) {
            if (lod_mesh->integer > tiki->numMeshes - 1) {
                ri.Cvar_Set("lod_mesh", va("%d", tiki->numMeshes - 1));
            }

            if (mesh == lod_mesh->integer) {
                if (skelmodel->pLOD) {
                    break;
                }
            }
        }

        //
        // draw all surfaces
        //
        surface = skelmodel->pSurfaces;
        for (i = 0; i < skelmodel->numSurfaces; i++, dsurf++, bsurf++, surface = surface->pNext) {
            if (*bsurf & 4) {
                continue;
            }

            shader         = NULL;
            surface->ident = SF_TIKI_SKEL;

            // use a custom shader if specified
            if (!(ent->e.customShader) || (ent->e.renderfx & RF_CUSTOMSHADERPASS)) {
                // [bug-2080] 3-bit per-surface index - see MDL_SURFACE_SKININDEX in q_shared.h
                int iShaderNum = ent->e.skinNum + MDL_SURFACE_SKININDEX(*bsurf);

                if (iShaderNum >= dsurf->numskins) {
                    iShaderNum = 0;
                }
                shader = tr.shaders[dsurf->hShader[iShaderNum]];
            } else {
                shader = R_GetShaderByHandle(ent->e.customShader);
            }

            // ^~^~^ SKELSHADER (temporary, deterministic): the resolved shader for each drawn surface
            // of a char=1 model, logged once per model. Answers "draws but invisible": passes=0 or
            // st0img=- (stage image failed/disabled), def=1 (fell to default), a nodraw/transparent
            // sort/blend, or an unexpected rgbGen/alphaGen. Correlate with SKELDRAW ext (geometry ok).
            if (diagFirst && tiki->a && tiki->a->bIsCharacter && shader) {
                int            lsn = ent->e.skinNum + MDL_SURFACE_SKININDEX(*bsurf);
                shaderStage_t *s0;
                const char    *img;
                if (lsn >= dsurf->numskins) {
                    lsn = 0;
                }
                s0  = (shader->numUnfoggedPasses > 0) ? shader->stages[0] : NULL;
                img = (s0 && s0->bundle[0].image[0]) ? s0->bundle[0].image[0]->imgName : "-";
                if (R_SkelDiagOn())
                ri.Printf(PRINT_ALL,
                    "^~^~^ SKELSHADER model=%s surf=%d bsurf=0x%x skin=%d/%d hShader=%d shader=%s def=%d passes=%d sort=%.1f sfc=0x%x cull=%d st0img=%s st0rgb=%d st0alpha=%d st0blend=0x%x\n",
                    tiki->a->name, i, (unsigned)*bsurf, lsn, dsurf->numskins, dsurf->hShader[lsn],
                    shader->name, shader->defaultShader ? 1 : 0, shader->numUnfoggedPasses,
                    shader->sort, (unsigned)shader->surfaceFlags, (int)shader->cullType,
                    img, s0 ? (int)s0->rgbGen : -1, s0 ? (int)s0->alphaGen : -1,
                    s0 ? (unsigned)(s0->stateBits & (GLS_SRCBLEND_BITS | GLS_DSTBLEND_BITS)) : 0u);
            }

            if (!personalModel) {
                // [bug-2080] The crossfade double-draw that stood here is GONE with the bit that
                // drove it. It fired on (*bsurf & 0x40), which is now skin-offset bit 2, so every
                // glove index 4-7 would have drawn the surface twice with two adjacent shaders.
                // Nothing is lost: zero TIKIs in AA/SH/BT ask for crossfade and no script sets it,
                // so this branch has never executed in this game. The out-of-bounds guard it
                // carried (bug from 07-31) is preserved by the single clamped path above.
                R_AddDrawSurf((surfaceType_t *)surface, shader, 0, 0, 0, 0);
            }

            if ((ent->e.customShader) && (ent->e.renderfx & RF_CUSTOMSHADERPASS)) {
                shader = R_GetShaderByHandle(ent->e.customShader);
                R_AddDrawSurf((surfaceType_t *)surface, shader, 0, 0, 0, 0);
            }
        }
    }

    // ^~^~^ SKELTRACK: end-of-add marker, same cadence as the entry print - drawSurfsAdded=0
    // with the entry firing means the mesh loop contributed NOTHING (personalModel/skin bits).
    if (trkIsTarget) {
        static int trkEnd = 0;
        trkEnd++;
        if ((trkEnd & 31) == 1) {
            if (R_SkelDiagOn())
            ri.Printf(PRINT_ALL,
                "^~^~^ SKELTRACK addskel-end n=%d ent=%d drawSurfsAdded=%d personal=%d\n",
                trkEnd, ent->e.entityNumber, tr.refdef.numDrawSurfs - trkDrawBefore,
                personalModel ? 1 : 0);
        }
    }

    // ^~^~^ SKELDIAG: one line the FIRST time each model reaches this add path (deterministic, no
    // input), plus once per actor per frame in the optional verbose mode. Correlate with the backend
    // ^~^~^ SKELDRAW by ent/slot/hModel. A model in SKELREG but never here = dropped upstream.
    if (diagFirst || (diagOn && g_skeldiagFrontLines < 40)) {
        const char *rc = (iRadiusCull == CULL_IN) ? "IN" : (iRadiusCull == CULL_CLIP) ? "CLIP" : (iRadiusCull == CULL_OUT) ? "OUT" : "na";
        const char *cb = (diagCullBox == CULL_IN) ? "IN" : (diagCullBox == CULL_CLIP) ? "CLIP" : (diagCullBox == CULL_OUT) ? "OUT" : "na";
        const char *ps = (g_skeldiagMainPose == 3) ? "FORCED" : (g_skeldiagMainPose == 1) ? "RAN" : "REUSED";
        model_t    *diagMdl = R_GetModelByHandle(ent->e.hModel);
        const char *diagMdlName = (diagMdl && diagMdl->name[0]) ? diagMdl->name : "?";
        // composite/attach path: the dispatched model name is "weapon|<wpn>|<base>" for a merged model
        int         diagComposite = (diagMdlName[0] && strchr(diagMdlName, '|')) ? 1 : 0;
        if (diagOn) {
            g_skeldiagFrontLines++;
        }
        if (R_SkelDiagOn())
        ri.Printf(PRINT_ALL,
            "^~^~^ SKELDIAG ent=%d slot=%d hModel=%d model=%s char=%d rfx=0x%x scale=%.2f escale=%.2f rad=%.1f "
            "bones=%d actw=%.2f frame0=%d/%.2f/%.2f composite=%d mirrored=%d isMirror=%d radcull=%s cullbox=%s "
            "degen=%d prePosed=%d pose=%s(si=%d fi=%d) fbnds=[%.0f %.0f %.0f]-[%.0f %.0f %.0f] meshes=%d mdl=%s%s\n",
            ent->e.entityNumber,
            (int)(ent - tr.refdef.entities),
            ent->e.hModel,
            tiki->a->name,
            tiki->a->bIsCharacter ? 1 : 0,
            ent->e.renderfx,
            tiki_scale,
            ent->e.scale,
            radius,
            num_tags,
            ent->e.actionWeight,
            ent->e.frameInfo[0].index, ent->e.frameInfo[0].time, ent->e.frameInfo[0].weight,
            diagComposite,
            ent->mirrored ? 1 : 0,
            tr.viewParms.isMirror ? 1 : 0,
            rc, cb,
            diagDegen ? 1 : 0,
            g_skeldiagPrePosed,
            ps, g_skeldiagPoseSkelIdx, g_skeldiagPoseFrameIdx,
            diagFMin[0], diagFMin[1], diagFMin[2],
            diagFMax[0], diagFMax[1], diagFMax[2],
            tiki->numMeshes,
            diagMdlName,
            diagFirst ? " FIRST" : "");
    }

    // FIXME: setup LOD
}

/*
=============
SkelVertGetNormal
=============
*/
inline static void SkelVertGetNormal(skeletorVertex_t *vert, skelBoneCache_t *bone, vec3_t out)
{
    out[0] = vert->normal[0] * bone->matrix[0][0] + vert->normal[1] * bone->matrix[1][0]
           + vert->normal[2] * bone->matrix[2][0];

    out[1] = vert->normal[0] * bone->matrix[0][1] + vert->normal[1] * bone->matrix[1][1]
           + vert->normal[2] * bone->matrix[2][1];

    out[2] = vert->normal[0] * bone->matrix[0][2] + vert->normal[1] * bone->matrix[1][2]
           + vert->normal[2] * bone->matrix[2][2];
}

/*
=============
SkelMorphGetXyz
=============
*/
inline static void SkelMorphGetXyz(skeletorMorph_t *morph, int *morphcache, vec3_t out)
{
    VectorMA(out, *morphcache, morph->offset, out);
}

/*
=============
SkelWeightGetXyz
=============
*/
inline static void SkelWeightGetXyz(skelWeight_t *weight, skelBoneCache_t *bone, vec3_t out)
{
    out[0] += ((weight->offset[0] * bone->matrix[0][0] + weight->offset[1] * bone->matrix[1][0]
                + weight->offset[2] * bone->matrix[2][0])
               + bone->offset[0])
            * weight->boneWeight;

    out[1] += ((weight->offset[0] * bone->matrix[0][1] + weight->offset[1] * bone->matrix[1][1]
                + weight->offset[2] * bone->matrix[2][1])
               + bone->offset[1])
            * weight->boneWeight;

    out[2] += ((weight->offset[0] * bone->matrix[0][2] + weight->offset[1] * bone->matrix[1][2]
                + weight->offset[2] * bone->matrix[2][2])
               + bone->offset[2])
            * weight->boneWeight;
}

/*
=============
SkelWeightMorphGetXyz
=============
*/
inline static void SkelWeightMorphGetXyz(skelWeight_t *weight, skelBoneCache_t *bone, vec3_t totalmorph, vec3_t out)
{
    vec3_t point;

    VectorAdd(totalmorph, weight->offset, point);

    out[0] += ((point[0] * bone->matrix[0][0] + point[1] * bone->matrix[1][0] + point[2] * bone->matrix[2][0])
               + bone->offset[0])
            * weight->boneWeight;

    out[1] += ((point[0] * bone->matrix[0][1] + point[1] * bone->matrix[1][1] + point[2] * bone->matrix[2][1])
               + bone->offset[1])
            * weight->boneWeight;

    out[2] += ((point[0] * bone->matrix[0][2] + point[1] * bone->matrix[1][2] + point[2] * bone->matrix[2][2])
               + bone->offset[2])
            * weight->boneWeight;
}

/*
=============
RB_SkelMesh
=============
*/
/*
=============
HZM gl2 [bug-3483] THE SKIN CACHE

Skeletal models are skinned on the CPU, every surface, every view, every frame: RB_SkelMesh + R_VaoPackNormal were
28 % of the main thread on Omaha (m3l1a), where 128 of the 226 soldiers on the sand are dead and will never move
again. The skinned result of a surface is a pure function of (surface, LOD vertex count, scale, bone matrices) - it
is in MODEL space, the entity's position does not enter into it - so when the bones are bit-for-bit what they were
the last times this surface was drawn, the stored vertices are copied instead.

  * The key is (entity number, surface, vertex count). The entity number only spreads entries out; a wrong
    sharing is impossible because a hit also needs the same 64-bit hash over every bone the entity has.
  * offset[3] of a bone is never written by R_AddSkelSurfaces and is skipped by the hash.
  * Morph weights (faces) are part of the hash: e.hasMorph carries how many the entity has. A soldier whose
    face is moving simply never repeats; a dead one does.
  * A surface is stored on the THIRD consecutive identical sighting, so a moving soldier costs one hash and
    nothing else. Its buffer is kept when the pose changes and reused, not freed.
  * malloc/free, not the engine zone: nothing here can dangle across a vid_restart. Flushed when a model is freed.

r_hzmSkelCache 0 = off (live), 2 = print hit/miss/store counts every ~2 s.
=============
*/
#define SKC_SLOTS        8192
#define SKC_PROBES       4
#define SKC_MAX_BYTES    (96 * 1024 * 1024)
#define SKC_VERT_BYTES   (sizeof(vec4_t) + 4 * sizeof(int16_t) + sizeof(vec2_t))

typedef struct {
    skelSurfaceGame_t *sf;
    int                entityNumber;
    int                renderCount;
    float              scale;
    uint64_t           boneHash;
    int                lastFrame;
    int                same;        // consecutive sightings with this hash
    int                hitFrame;    // last frame its buffer was handed to the frame table (bug-3487)
    qboolean           valid;       // data holds the skin for boneHash
    int                bytes;
    byte              *data;        // xyz[rc] | normal[rc] | texCoords[rc]
} skcEntry_t;

static skcEntry_t s_skc[SKC_SLOTS];
static int        s_skcBytes;
static int        s_skcHit, s_skcMiss, s_skcStore, s_skcStatFrame;
static cvar_t    *r_hzmSkelCache;

extern "C" void R_SkelCacheFlush(void)
{
    int i;

    for (i = 0; i < SKC_SLOTS; i++) {
        if (s_skc[i].data) {
            free(s_skc[i].data);
        }
    }
    memset(s_skc, 0, sizeof(s_skc));
    s_skcBytes = 0;
}

static uint64_t SKC_HashBones(const skelBoneCache_t *bones, int numBones)
{
    uint64_t h = 0x9E3779B97F4A7C15ULL ^ (uint64_t)numBones;
    int      i, k;

    for (i = 0; i < numBones; i++) {
        uint64_t w[8];
        // offset[0..2] (offset[3] is never written), then the 3x4 matrix
        w[0] = 0;
        w[1] = 0;
        memcpy(&w[0], &bones[i].offset[0], 8);
        memcpy(&w[1], &bones[i].offset[2], 4);
        memcpy(&w[2], &bones[i].matrix[0][0], 48);
        for (k = 0; k < 8; k++) {
            h ^= w[k];
            h *= 0x100000001B3ULL;
            h ^= h >> 29;
        }
    }
    return h;
}

static skcEntry_t *SKC_Lookup(int entityNumber, skelSurfaceGame_t *sf, int rc, float scale, uint64_t h)
{
    unsigned int idx = ((unsigned int)entityNumber * 2654435761u) ^ ((unsigned int)((uintptr_t)sf >> 4) * 40503u)
                       ^ ((unsigned int)rc * 97u);
    skcEntry_t  *victim = NULL;
    int          p;

    for (p = 0; p < SKC_PROBES; p++) {
        skcEntry_t *e = &s_skc[(idx + p) & (SKC_SLOTS - 1)];

        if (e->sf == sf && e->entityNumber == entityNumber && e->renderCount == rc) {
            e->lastFrame = tr.frameCount;
            if (e->boneHash == h && e->scale == scale) {
                e->same++;
            } else {
                e->boneHash = h;
                e->scale    = scale;
                e->same     = 0;
                e->valid    = qfalse;
            }
            return e;
        }
        if (!victim || e->lastFrame < victim->lastFrame) {
            victim = e;
        }
    }

    if (victim->hitFrame == tr.frameCount && victim->data) {
        // [bug-3487] every candidate here is holding a buffer the frame table points at: no caching for this one
        return NULL;
    }
    if (victim->data) {
        free(victim->data);
        s_skcBytes -= victim->bytes;
    }
    memset(victim, 0, sizeof(*victim));
    victim->sf           = sf;
    victim->entityNumber = entityNumber;
    victim->renderCount  = rc;
    victim->scale        = scale;
    victim->boneHash     = h;
    victim->lastFrame    = tr.frameCount;
    return victim;
}

static void SKC_StoreFrom(skcEntry_t *e, const byte *src, int rc);

static void SKC_Store(skcEntry_t *e, int baseVertex, int rc)
{
    // tess keeps the three arrays apart; gather them and store
    static byte tmp[SHADER_MAX_VERTEXES * SKC_VERT_BYTES];

    if (rc > SHADER_MAX_VERTEXES) {
        return;
    }
    memcpy(tmp, tess.xyz[baseVertex], rc * sizeof(vec4_t));
    memcpy(tmp + rc * sizeof(vec4_t), tess.normal[baseVertex], rc * 4 * sizeof(int16_t));
    memcpy(tmp + rc * (sizeof(vec4_t) + 4 * sizeof(int16_t)), tess.texCoords[baseVertex], rc * sizeof(vec2_t));
    SKC_StoreFrom(e, tmp, rc);
}

static void SKC_StoreFrom(skcEntry_t *e, const byte *src, int rc)
{
    int bytes = rc * (int)SKC_VERT_BYTES;

    if (e->hitFrame == tr.frameCount) {
        return;     // [bug-3487] its buffer is on loan to the frame table until this frame is drawn
    }

    if (!e->data) {
        if (s_skcBytes + bytes > SKC_MAX_BYTES) {
            // full: let go of everything that has not been drawn for ~10 s, then try once more
            int i;
            for (i = 0; i < SKC_SLOTS; i++) {
                if (s_skc[i].data && tr.frameCount - s_skc[i].lastFrame > 600) {
                    free(s_skc[i].data);
                    s_skcBytes -= s_skc[i].bytes;
                    s_skc[i].data  = NULL;
                    s_skc[i].bytes = 0;
                    s_skc[i].valid = qfalse;
                }
            }
            if (s_skcBytes + bytes > SKC_MAX_BYTES) {
                return;
            }
        }
        e->data = (byte *)malloc(bytes);
        if (!e->data) {
            return;
        }
        e->bytes = bytes;
        s_skcBytes += bytes;
    }
    memcpy(e->data, src, bytes);
    e->valid = qtrue;
    s_skcStore++;
}

/*
=============
SKEL_Skin

HZM gl2 [bug-3487] The four skinning loops of RB_SkelMesh (morph / no morph x base mesh / attached mesh), lifted out
unchanged so a worker thread can run them: this function touches nothing but its arguments, the model data and the
bone / morph pools it is handed, all of which are read-only while the back end is drawing.
=============
*/
static void SKEL_Skin(skelSurfaceGame_t *sf, dtiki_t *tiki, skelHeaderGame_t *skelmodel, int mesh,
                      unsigned int render_count, float scale, skelBoneCache_t *bones, int *morphs, qboolean hasMorph,
                      float *outXyz, int16_t *outNormal, float *outTc)
{
    skeletorVertex_t *newVerts = sf->pVerts;
    skeletorMorph_t  *morph;
    skelWeight_t     *weight;
    skelBoneCache_t  *bone;
    int              *morphcache;
    int               vertNum;
    int               morphNum;
    int               weightNum;

    if (hasMorph) {
        if (mesh > 0) {
            for (vertNum = 0; vertNum < render_count; vertNum++) {
                vec3_t normal;
                vec3_t out;
                vec3_t totalmorph;
                int    channelNum;
                int    boneNum;

                VectorClear(out);
                VectorClear(outXyz);
                VectorClear(totalmorph);

                weight = (skelWeight_t *)((byte *)newVerts + sizeof(skeletorVertex_t)
                                          + sizeof(skeletorMorph_t) * newVerts->numMorphs);
                morph  = (skeletorMorph_t *)((byte *)newVerts + sizeof(skeletorVertex_t));

                for (morphNum = 0; morphNum < newVerts->numMorphs; morphNum++) {
                    morphcache = &morphs[morph->morphIndex];

                    if (*morphcache) {
                        SkelMorphGetXyz(morph, morphcache, totalmorph);
                    }

                    morph++;
                }

                if (newVerts->numMorphs) {
                    channelNum = skelmodel->pBones[morph->morphIndex].channel;
                } else {
                    channelNum = skelmodel->pBones[weight->boneIndex].channel;
                }

                boneNum = ri.TIKI_GetLocalChannel(tiki, channelNum);
                bone    = &bones[boneNum];

                SkelVertGetNormal(newVerts, bone, normal);

                for (weightNum = 0; weightNum < newVerts->numWeights; weightNum++) {
                    channelNum = skelmodel->pBones[weight->boneIndex].channel;
                    boneNum    = ri.TIKI_GetLocalChannel(tiki, channelNum);
                    bone       = &bones[boneNum];

                    if (!weightNum) {
                        SkelWeightMorphGetXyz(weight, bone, totalmorph, out);
                    } else {
                        SkelWeightGetXyz(weight, bone, out);
                    }

                    weight++;
                }

                R_VaoPackNormal(outNormal, normal);
                VectorScale(out, scale, outXyz);

                outTc[vertNum * 2] = newVerts->texCoords[0];
                outTc[vertNum * 2 + 1] = newVerts->texCoords[1];
                // FIXME: fill in lightmapST for completeness?

                newVerts = (skeletorVertex_t *)((byte *)newVerts + sizeof(skeletorVertex_t)
                                                + sizeof(skeletorMorph_t) * newVerts->numMorphs
                                                + sizeof(skelWeight_t) * newVerts->numWeights);
                outXyz += 4;
                outNormal += 4;
            }
        } else {
            for (vertNum = 0; vertNum < render_count; vertNum++) {
                vec3_t normal;
                vec3_t out;
                vec3_t totalmorph;

                VectorClear(out);
                VectorClear(outXyz);
                VectorClear(totalmorph);

                weight = (skelWeight_t *)((byte *)newVerts + sizeof(skeletorVertex_t)
                                          + sizeof(skeletorMorph_t) * newVerts->numMorphs);
                morph  = (skeletorMorph_t *)((byte *)newVerts + sizeof(skeletorVertex_t));

                for (morphNum = 0; morphNum < newVerts->numMorphs; morphNum++) {
                    morphcache = &morphs[morph->morphIndex];

                    if (*morphcache) {
                        SkelMorphGetXyz(morph, morphcache, totalmorph);
                    }

                    morph++;
                }

                if (newVerts->numMorphs) {
                    bone = &bones[morph->morphIndex];
                } else {
                    bone = &bones[weight->boneIndex];
                }

                SkelVertGetNormal(newVerts, bone, normal);

                for (weightNum = 0; weightNum < newVerts->numWeights; weightNum++) {
                    bone = &bones[weight->boneIndex];

                    if (!weightNum) {
                        SkelWeightMorphGetXyz(weight, bone, totalmorph, out);
                    } else {
                        SkelWeightGetXyz(weight, bone, out);
                    }

                    weight++;
                }

                R_VaoPackNormal(outNormal, normal);
                VectorScale(out, scale, outXyz);

                outTc[vertNum * 2] = newVerts->texCoords[0];
                outTc[vertNum * 2 + 1] = newVerts->texCoords[1];
                // FIXME: fill in lightmapST for completeness?

                newVerts = (skeletorVertex_t *)((byte *)newVerts + sizeof(skeletorVertex_t)
                                                + sizeof(skeletorMorph_t) * newVerts->numMorphs
                                                + sizeof(skelWeight_t) * newVerts->numWeights);
                outXyz += 4;
                outNormal += 4;
            }
        }
    } else {
        if (mesh > 0) {
            for (vertNum = 0; vertNum < render_count; vertNum++) {
                vec3_t normal;
                vec3_t out;
                int    channelNum;
                int    boneNum;

                VectorClear(out);
                VectorClear(outXyz);

                weight = (skelWeight_t *)((byte *)newVerts + sizeof(skeletorVertex_t)
                                          + sizeof(skeletorMorph_t) * newVerts->numMorphs);

                channelNum = skelmodel->pBones[weight->boneIndex].channel;
                boneNum    = ri.TIKI_GetLocalChannel(tiki, channelNum);
                bone       = &bones[boneNum];

                SkelVertGetNormal(newVerts, bone, normal);

                for (weightNum = 0; weightNum < newVerts->numWeights; weightNum++) {
                    channelNum = skelmodel->pBones[weight->boneIndex].channel;
                    boneNum    = ri.TIKI_GetLocalChannel(tiki, channelNum);
                    bone       = &bones[boneNum];

                    SkelWeightGetXyz(weight, bone, out);

                    weight++;
                }

                R_VaoPackNormal(outNormal, normal);
                VectorScale(out, scale, outXyz);

                outTc[vertNum * 2] = newVerts->texCoords[0];
                outTc[vertNum * 2 + 1] = newVerts->texCoords[1];
                // FIXME: fill in lightmapST for completeness?

                newVerts = (skeletorVertex_t *)((byte *)newVerts + sizeof(skeletorVertex_t)
                                                + sizeof(skeletorMorph_t) * newVerts->numMorphs
                                                + sizeof(skelWeight_t) * newVerts->numWeights);
                outXyz += 4;
                outNormal += 4;
            }
        } else {
            for (vertNum = 0; vertNum < render_count; vertNum++) {
                vec3_t normal;
                vec3_t out;

                VectorClear(out);
                VectorClear(outXyz);

                weight = (skelWeight_t *)((byte *)newVerts + sizeof(skeletorVertex_t)
                                          + sizeof(skeletorMorph_t) * newVerts->numMorphs);

                bone = &bones[weight->boneIndex];
                SkelVertGetNormal(newVerts, bone, normal);

                for (weightNum = 0; weightNum < newVerts->numWeights; weightNum++) {
                    bone = &bones[weight->boneIndex];

                    SkelWeightGetXyz(weight, bone, out);

                    weight++;
                }

                R_VaoPackNormal(outNormal, normal);
                VectorScale(out, scale, outXyz);

                outTc[vertNum * 2] = newVerts->texCoords[0];
                outTc[vertNum * 2 + 1] = newVerts->texCoords[1];
                // FIXME: fill in lightmapST for completeness?

                newVerts = (skeletorVertex_t *)((byte *)newVerts + sizeof(skeletorVertex_t)
                                                + sizeof(skeletorMorph_t) * newVerts->numMorphs
                                                + sizeof(skelWeight_t) * newVerts->numWeights);
                outXyz += 4;
                outNormal += 4;
            }
        }
    }
}

/*
=============
HZM gl2 [bug-3487] SKINNING ON WORKER THREADS

After the tess stream and the skin cache (bug-3483) the largest single cost left on a crowded map was still
RB_SkelMesh + R_VaoPackNormal: the soldiers who ARE moving, about 1,400 surface skins a frame on Omaha, all on the
one thread that also runs the game, the effects and every draw call.

R_SkelPreskin runs at the top of each colour pass (never a depth-only pass: those skip most characters). It walks
the draw list once, works out for every skeletal surface what RB_SkelMesh is about to ask for - (entity, surface,
vertex count) - and
  * if the skin cache already holds it, points the frame table at the cached copy;
  * otherwise queues a job that skins it into a frame arena.
The jobs run on r_hzmSkinThreads workers plus this thread, and the pass does not start until all are done, so the
draw loop is exactly as single-threaded as before; RB_SkelMesh simply finds its vertices ready and copies them.
A surface the prepass did not prepare (arena full, a depth pass, LOD tool, the feature off) is skinned inline by
RB_SkelMesh as it always was, and a prepared one is only used when RB_SkelMesh's own vertex count matches the
prepass's, so the two can never disagree about what is drawn.

The table lasts the frame, not the pass: the same entity surface drawn again in a later list (a second pass, a
shadow cascade at the same detail) reuses the skin.

Workers only read: model data, TIKI_Skel_Bones, skeletorMorphCache (all frozen while the back end runs) and
skelChannelList_s::LocalChannel (const). They write only their own job's slice of the arena.
The threads are joined in RE_Shutdown - the renderer DLL is unloaded on every vid_restart.

r_hzmSkinThreads: -1 = automatic (cores - 2, at most 6), 0 = off (inline, as before), N = that many workers. Live.
=============
*/
#include <thread>
#include <mutex>
#include <condition_variable>
#include <vector>

#define PS_SLOTS       16384
#define PS_MAX_JOBS    8192
#define PS_ARENA_BYTES (48 * 1024 * 1024)

typedef struct {
    trRefEntity_t     *ent;
    skelSurfaceGame_t *sf;
    int                rc;
    int                frame;
    const byte        *data;        // xyz[rc] | normal[rc] | texCoords[rc]
} psEntry_t;

typedef struct {
    skelSurfaceGame_t *sf;
    dtiki_t           *tiki;
    skelHeaderGame_t  *skelmodel;
    int                mesh;
    unsigned int       rc;
    float              scale;
    skelBoneCache_t   *bones;
    int               *morphs;
    qboolean           hasMorph;
    byte              *out;
    skcEntry_t        *store;       // third identical sighting: keep the result in the skin cache
} psJob_t;

static psEntry_t s_ps[PS_SLOTS];
static psJob_t   s_psJobs[PS_MAX_JOBS];
static byte     *s_psArena;
static int       s_psArenaUsed;
static int       s_psFrame = -1;
static int       s_psStatJobs, s_psStatHits, s_psStatUsed;
static cvar_t   *r_hzmSkinThreads;

static std::vector<std::thread> *s_psThreads;
static std::mutex               *s_psMx;
static std::condition_variable  *s_psWork, *s_psDone;
static int                       s_psNum, s_psNext, s_psPending;
static bool                      s_psQuit;

static void PS_RunJob(const psJob_t *j)
{
    SKEL_Skin(j->sf, j->tiki, j->skelmodel, j->mesh, j->rc, j->scale, j->bones, j->morphs, j->hasMorph, (float *)j->out,
              (int16_t *)(j->out + j->rc * sizeof(vec4_t)), (float *)(j->out + j->rc * (sizeof(vec4_t) + 4 * sizeof(int16_t))));
}

static void PS_Worker(void)
{
    std::unique_lock<std::mutex> lk(*s_psMx);

    for (;;) {
        s_psWork->wait(lk, [] { return s_psQuit || s_psNext < s_psNum; });
        if (s_psQuit) {
            return;
        }
        int i = s_psNext++;
        lk.unlock();
        PS_RunJob(&s_psJobs[i]);
        lk.lock();
        if (--s_psPending == 0) {
            s_psDone->notify_all();
        }
    }
}

extern "C" void R_SkelThreadsShutdown(void)
{
    if (s_psThreads) {
        {
            std::lock_guard<std::mutex> lk(*s_psMx);
            s_psQuit = true;
        }
        s_psWork->notify_all();
        for (size_t i = 0; i < s_psThreads->size(); i++) {
            (*s_psThreads)[i].join();
        }
        delete s_psThreads;
        delete s_psWork;
        delete s_psDone;
        delete s_psMx;
        s_psThreads = NULL;
        s_psWork = s_psDone = NULL;
        s_psMx   = NULL;
        s_psQuit = false;
    }
    if (s_psArena) {
        free(s_psArena);
        s_psArena = NULL;
    }
    s_psFrame = -1;
    s_psNum = s_psNext = s_psPending = 0;
}

static int PS_WantedWorkers(void)
{
    int n;

    if (!r_hzmSkinThreads) {
        r_hzmSkinThreads = ri.Cvar_Get("r_hzmSkinThreads", "-1", 0);
    }
    n = r_hzmSkinThreads->integer;
    if (n < 0) {
        n = (int)std::thread::hardware_concurrency() - 2;
        if (n > 6) {
            n = 6;
        }
    }
    if (n < 0) {
        n = 0;
    }
    if (n > 16) {
        n = 16;
    }
    return n;
}

static qboolean PS_EnsurePool(int workers)
{
    if (s_psThreads && (int)s_psThreads->size() != workers) {
        R_SkelThreadsShutdown();
    }
    if (!s_psArena) {
        s_psArena = (byte *)malloc(PS_ARENA_BYTES);
        if (!s_psArena) {
            return qfalse;
        }
    }
    if (!s_psThreads) {
        s_psMx      = new std::mutex;
        s_psWork    = new std::condition_variable;
        s_psDone    = new std::condition_variable;
        s_psThreads = new std::vector<std::thread>;
        s_psQuit    = false;
        for (int i = 0; i < workers; i++) {
            s_psThreads->emplace_back(PS_Worker);
        }
    }
    return qtrue;
}

static unsigned int PS_Hash(trRefEntity_t *ent, skelSurfaceGame_t *sf, int rc)
{
    return ((unsigned int)((uintptr_t)ent >> 4) * 2654435761u) ^ ((unsigned int)((uintptr_t)sf >> 4) * 40503u)
           ^ ((unsigned int)rc * 97u);
}

static psEntry_t *PS_Slot(trRefEntity_t *ent, skelSurfaceGame_t *sf, int rc, qboolean create)
{
    unsigned int idx = PS_Hash(ent, sf, rc);
    int          p;

    for (p = 0; p < 12; p++) {
        psEntry_t *e = &s_ps[(idx + p) & (PS_SLOTS - 1)];

        if (e->frame != s_psFrame) {
            if (!create) {
                return NULL;
            }
            e->ent   = ent;
            e->sf    = sf;
            e->rc    = rc;
            e->frame = s_psFrame;
            e->data  = NULL;
            return e;
        }
        if (e->ent == ent && e->sf == sf && e->rc == rc) {
            return e;
        }
    }
    return NULL;
}

/*
=============
SKEL_Prepare

What RB_SkelMesh works out before it skins: which mesh the surface belongs to and how many vertices this level of
detail draws. A plain copy of that logic without the diagnostics. If the two ever drift the only effect is that
RB_SkelMesh does not find the prepared skin and skins inline.
=============
*/
static qboolean SKEL_Prepare(skelSurfaceGame_t *sf, trRefEntity_t *ent, psJob_t *out)
{
    dtiki_t           *tiki = ent->e.tiki;
    skelHeaderGame_t  *skelmodel = NULL;
    skelSurfaceGame_t *psurface;
    qboolean           bFound = qfalse;
    unsigned int       render_count;
    int                mesh, surf;

    if (!tiki) {
        return qfalse;
    }
    for (mesh = 0; mesh < tiki->numMeshes; mesh++) {
        skelmodel = ri.TIKI_GetSkel(tiki->mesh[mesh]);
        psurface  = skelmodel->pSurfaces;
        for (surf = 0; surf < skelmodel->numSurfaces; surf++) {
            if (psurface == sf) {
                bFound = qtrue;
                break;
            }
            psurface = psurface->pNext;
        }
        if (bFound) {
            break;
        }
    }
    if (!bFound) {
        return qfalse;
    }

    qboolean tskForceLod0 = (qboolean)(r_test_forcelod0 && r_test_forcelod0->integer && tiki->a && tiki->a->bIsCharacter);
    if (skelmodel->pLOD && !tskForceLod0) {
        float lod_val  = ent->lodpercentage[0];
        int   renderfx = ent->e.renderfx;

        if (r_charShadows && r_charShadows->integer && (backEnd.viewParms.flags & VPF_DEPTHSHADOW) && r_charShadowLod
            && r_charShadowLod->value > 0.0f && tiki->a && tiki->a->bIsCharacter) {
            lod_val = r_charShadowLod->value;
        }

        if (sf->numVerts > 3) {
            skelIndex_t *collapseIndex = sf->pCollapseIndex;
            int          mid, low, high;
            int          lod_cutoff = GetLodCutoff(skelmodel, lod_val, renderfx);

            if (collapseIndex[2] < lod_cutoff) {
                return qfalse;
            }
            low = mid = 3;
            high      = sf->numVerts;
            while (high >= low) {
                mid = (low + high) >> 1;
                if (collapseIndex[mid] < lod_cutoff) {
                    high = mid - 1;
                    if (collapseIndex[mid - 1] >= lod_cutoff) {
                        break;
                    }
                } else {
                    mid++;
                    low = mid;
                    if (high == mid || collapseIndex[mid] < lod_cutoff) {
                        break;
                    }
                }
            }
            render_count = mid;
        } else {
            render_count = sf->numVerts;
        }
        if (!render_count) {
            return qfalse;
        }
    } else {
        render_count = sf->numVerts;
    }

    out->sf        = sf;
    out->tiki      = tiki;
    out->skelmodel = skelmodel;
    out->mesh      = mesh;
    out->rc        = render_count;
    out->scale     = tiki->load_scale * ent->e.scale;
    return qtrue;
}

/*
=============
R_SkelPreskin
=============
*/
extern "C" void R_SkelPreskin(drawSurf_t *drawSurfs, int numDrawSurfs)
{
    drawSurf_t *drawSurf;
    int         workers = PS_WantedWorkers();
    int         i, numJobs = 0;

    if (!workers || (lod_tool && lod_tool->integer) || !r_drawentitypoly->integer) {
        return;
    }
    if (!PS_EnsurePool(workers)) {
        return;
    }
    if (!r_hzmSkelCache) {
        r_hzmSkelCache = ri.Cvar_Get("r_hzmSkelCache", "1", 0);
    }
    if (s_psFrame != tr.frameCount) {
        s_psFrame     = tr.frameCount;
        s_psArenaUsed = 0;
    }

    for (i = 0, drawSurf = drawSurfs; i < numDrawSurfs; i++, drawSurf++) {
        shader_t          *shader;
        int                entityNum, fogNum, dlighted, pshadowed;
        qboolean           bStaticModel;
        trRefEntity_t     *ent;
        skelSurfaceGame_t *sf;
        psJob_t            job;
        psEntry_t         *slot;
        skcEntry_t        *skc = NULL;
        int                bytes, nMorph;
        uint64_t           hash;

        if (*drawSurf->surface != SF_TIKI_SKEL) {
            continue;
        }
        R_DecomposeSort(drawSurf->sort, &entityNum, &shader, &fogNum, &dlighted, &pshadowed, &bStaticModel);
        if (bStaticModel || entityNum == REFENTITYNUM_WORLD) {
            continue;
        }
        ent = &backEnd.refdef.entities[entityNum];
        sf  = (skelSurfaceGame_t *)drawSurf->surface;
        if (!SKEL_Prepare(sf, ent, &job) || job.rc < 24) {
            continue;
        }
        slot = PS_Slot(ent, sf, (int)job.rc, qtrue);
        if (!slot || slot->data) {
            continue;       // table crowded here (inline will do it), or already prepared this frame
        }

        job.bones    = &TIKI_Skel_Bones[ent->e.bonestart];
        job.morphs   = &skeletorMorphCache[ent->e.morphstart];
        job.hasMorph = (qboolean)(ent->e.hasMorph != 0);
        job.store    = NULL;

        if (r_hzmSkelCache->integer) {
            hash   = SKC_HashBones(job.bones, ri.TIKI_GetNumChannels(job.tiki));
            nMorph = (int)ent->e.hasMorph;
            if (nMorph > 0) {
                int mi;
                for (mi = 0; mi < nMorph; mi++) {
                    hash ^= (uint64_t)(unsigned int)job.morphs[mi] + 0x9E3779B97F4A7C15ULL + (hash << 6) + (hash >> 2);
                }
                hash ^= (uint64_t)nMorph << 48;
            }
            skc = SKC_Lookup(ent->e.entityNumber, sf, (int)job.rc, job.scale, hash);
            if (skc && skc->valid) {
                skc->hitFrame = tr.frameCount;      // its buffer is in use until this frame is drawn
                slot->data    = skc->data;
                s_psStatHits++;
                s_skcHit++;
                continue;
            }
            if (skc) {
                s_skcMiss++;
                if (skc->same >= 2 && skc->hitFrame != tr.frameCount) {
                    job.store = skc;
                }
            }
        }

        bytes = (int)((job.rc * SKC_VERT_BYTES + 63) & ~(size_t)63);
        if (numJobs >= PS_MAX_JOBS || s_psArenaUsed + bytes > PS_ARENA_BYTES) {
            slot->frame = -1;       // give the slot back: RB_SkelMesh skins this one inline
            continue;
        }
        job.out = s_psArena + s_psArenaUsed;
        s_psArenaUsed += bytes;
        slot->data = job.out;
        s_psJobs[numJobs++] = job;
    }

    if (!numJobs) {
        return;
    }
    s_psStatJobs += numJobs;

    {
        std::unique_lock<std::mutex> lk(*s_psMx);
        s_psNum     = numJobs;
        s_psNext    = 0;
        s_psPending = numJobs;
        lk.unlock();
        s_psWork->notify_all();
        lk.lock();
        // this thread works too
        while (s_psNext < s_psNum) {
            int k = s_psNext++;
            lk.unlock();
            PS_RunJob(&s_psJobs[k]);
            lk.lock();
            --s_psPending;
        }
        s_psDone->wait(lk, [] { return s_psPending == 0; });
        s_psNum = s_psNext = 0;
    }

    for (i = 0; i < numJobs; i++) {
        if (s_psJobs[i].store) {
            SKC_StoreFrom(s_psJobs[i].store, s_psJobs[i].out, (int)s_psJobs[i].rc);
        }
    }
}

void RB_SkelMesh(skelSurfaceGame_t *sf)
{
    skcEntry_t        *skc = NULL;
    unsigned int       baseIndex, baseVertex;
    unsigned int       render_count;
    unsigned int       indexes;
    float             *outXyz;
    int16_t           *outNormal;
    skelIndex_t       *triangles;
    skelIndex_t       *collapse_map;
    skeletorVertex_t  *newVerts;
    skeletorMorph_t   *morph;
    skelWeight_t      *weight;
    int                vertNum;
    int                morphNum;
    int                weightNum;
    skelBoneCache_t   *bones;
    skelBoneCache_t   *bone;
    int               *morphs;
    int               *morphcache;
    float              scale;
    dtiki_t           *tiki;
    int                mesh;
    int                surf;
    int                i;
    skelHeaderGame_t  *skelmodel;
    skelSurfaceGame_t *psurface;
    qboolean           bFound;
    short              collapse[TIKI_MAX_VERTEXES];

    if (!r_drawentitypoly->integer) {
        return;
    }

    tiki = backEnd.currentEntity->e.tiki;

    scale = tiki->load_scale * backEnd.currentEntity->e.scale;

    //
    // get the mesh associated with the surface
    //
    bFound = qfalse;
    for (mesh = 0; mesh < tiki->numMeshes; mesh++) {
        skelmodel = ri.TIKI_GetSkel(tiki->mesh[mesh]);
        psurface  = skelmodel->pSurfaces;

        // find the surface
        for (surf = 0; surf < skelmodel->numSurfaces; surf++) {
            if (psurface == sf) {
                bFound = qtrue;
                break;
            }
            psurface = psurface->pNext;
        }

        if (bFound) {
            break;
        }
    }

    assert(bFound);

    //
    // Process LOD
    //
    // ^~^~^ SKELTEST r_test_forcelod0: for char models, skip all LOD reduction (render every vert) in
    // case the live actor selects a broken/empty LOD level that a frozen/dead actor does not.
    qboolean tskForceLod0 = (qboolean)(r_test_forcelod0 && r_test_forcelod0->integer && tiki
        && tiki->a && tiki->a->bIsCharacter);
    if (skelmodel->pLOD && !tskForceLod0) {
        float lod_val;
        int   renderfx;

        lod_val  = backEnd.currentEntity->lodpercentage[0];
        renderfx = backEnd.currentEntity->e.renderfx;

        // HZM gl2 real character shadows: coarse LOD for shadow casters. PLAY-GL2.bat forces
        // r_uselod 0 / r_lodscale 28 (max detail at all ranges), so this override is the only
        // LOD lever available, and CPU skinning in this function is the dominant cost of the
        // whole feature.
        // CRITICAL: this is a LOCAL substitution. lodpercentage[] lives on the SHARED
        // trRefEntity_t and is written per-entity at add time, i.e. last-write-wins across
        // views. Writing a shadow LOD back into the entity would corrupt the MAIN view's
        // detail. Never assign to backEnd.currentEntity->lodpercentage.
        if (r_charShadows && r_charShadows->integer
            && (backEnd.viewParms.flags & VPF_DEPTHSHADOW)
            && r_charShadowLod && r_charShadowLod->value > 0.0f
            && tiki && tiki->a && tiki->a->bIsCharacter) {
            lod_val = r_charShadowLod->value;
        }

        if (sf->numVerts > 3) {
            skelIndex_t *collapseIndex;
            int          mid, low, high;
            int          lod_cutoff;

            if (lod_tool->integer && !strcmp(backEnd.currentEntity->e.tiki->a->name, lod_tikiname->string)
                && mesh == lod_mesh->integer) {
                lod_cutoff = GetToolLodCutoff(skelmodel, backEnd.currentEntity->lodpercentage[0]);
            } else {
                lod_cutoff = GetLodCutoff(skelmodel, lod_val, renderfx);
            }

            collapseIndex = sf->pCollapseIndex;
            if (collapseIndex[2] < lod_cutoff) {
                if (R_SkelDiag_DrawWanted()) {
                    R_SkelDiag_Draw((int)sf->numVerts, 0, "lodcut", NULL, NULL);
                }
                return;
            }

            low = mid = 3;
            high      = sf->numVerts;
            while (high >= low) {
                mid = (low + high) >> 1;
                if (collapseIndex[mid] < lod_cutoff) {
                    high = mid - 1;
                    if (collapseIndex[mid - 1] >= lod_cutoff) {
                        break;
                    }
                } else {
                    mid++;
                    low = mid;
                    if (high == mid || collapseIndex[mid] < lod_cutoff) {
                        break;
                    }
                }
            }

            render_count = mid;
        } else {
            render_count = sf->numVerts;
        }

        if (!render_count) {
            if (R_SkelDiag_DrawWanted()) {
                R_SkelDiag_Draw(0, 0, "lod0", NULL, NULL);
            }
            return;
        }
    } else {
        render_count = sf->numVerts;
    }

    indexes = sf->numTriangles * 3;
    RB_CHECKOVERFLOW(render_count, indexes);

    collapse_map = sf->pCollapse;
    triangles    = sf->pTriangles;
    baseIndex    = tess.numIndexes;
    baseVertex   = tess.numVertexes;
    tess.numVertexes += render_count;

    outXyz    = tess.xyz[baseVertex];
    outNormal = tess.normal[baseVertex];
    newVerts  = sf->pVerts;

    if (render_count == sf->numVerts) {
        for (i = 0; i < indexes; i++) {
            tess.indexes[baseIndex + i] = baseVertex + triangles[i];
        }

        entityNumIndexes[backEnd.currentEntity - backEnd.refdef.entities] += indexes;
        tess.numIndexes += indexes;
    } else {
        assert(sf->numVerts < TIKI_MAX_VERTEXES);

        for (i = 0; i < render_count; i++) {
            collapse[i] = i;
        }

        for (i = render_count; i < sf->numVerts; i++) {
            collapse[i] = collapse[collapse_map[i]];
        }

        for (i = 0; i < indexes; i += 3) {
            assert(collapse[triangles[i]] < sf->numVerts);
            assert(collapse[triangles[i + 1]] < sf->numVerts);
            assert(collapse[triangles[i + 2]] < sf->numVerts);

            if (collapse[triangles[i]] == collapse[triangles[i + 1]]
                || collapse[triangles[i + 1]] == collapse[triangles[i + 2]]
                || collapse[triangles[i + 2]] == collapse[triangles[i]]) {
                break;
            }

            tess.indexes[baseIndex + i]     = baseVertex + collapse[triangles[i]];
            tess.indexes[baseIndex + i + 1] = baseVertex + collapse[triangles[i + 1]];
            tess.indexes[baseIndex + i + 2] = baseVertex + collapse[triangles[i + 2]];
        }

        entityNumIndexes[backEnd.currentEntity - backEnd.refdef.entities] += indexes;
        tess.numIndexes += i;
    }

    //
    // just copy the vertexes
    //
    bones  = &TIKI_Skel_Bones[backEnd.currentEntity->e.bonestart];
    morphs = &skeletorMorphCache[backEnd.currentEntity->e.morphstart];

    // HZM gl2 [bug-3483] the skin cache (see THE SKIN CACHE above)
    if (!r_hzmSkelCache) {
        r_hzmSkelCache = ri.Cvar_Get("r_hzmSkelCache", "1", 0);
    }
    // HZM gl2 [bug-3487] prepared by R_SkelPreskin (worker threads, or a skin-cache hit found there)?
    if (s_psFrame == tr.frameCount && render_count >= 24) {
        psEntry_t *psE = PS_Slot(backEnd.currentEntity, sf, (int)render_count, qfalse);
        if (psE && psE->data) {
            memcpy(outXyz, psE->data, render_count * sizeof(vec4_t));
            memcpy(outNormal, psE->data + render_count * sizeof(vec4_t), render_count * 4 * sizeof(int16_t));
            memcpy(tess.texCoords[baseVertex], psE->data + render_count * (sizeof(vec4_t) + 4 * sizeof(int16_t)),
                   render_count * sizeof(vec2_t));
            s_psStatUsed++;
            goto skc_skinned;
        }
    }

    if (r_hzmSkelCache->integer && render_count >= 24) {
        uint64_t skcHash = SKC_HashBones(bones, ri.TIKI_GetNumChannels(tiki));
        int      skcMorphs = (int)backEnd.currentEntity->e.hasMorph;

        if (skcMorphs > 0) {
            int mi;
            for (mi = 0; mi < skcMorphs; mi++) {
                skcHash ^= (uint64_t)(unsigned int)morphs[mi] + 0x9E3779B97F4A7C15ULL + (skcHash << 6) + (skcHash >> 2);
            }
            skcHash ^= (uint64_t)skcMorphs << 48;
        }
        skc = SKC_Lookup(backEnd.currentEntity->e.entityNumber, sf, (int)render_count, scale, skcHash);
        if (skc && skc->valid) {
            memcpy(outXyz, skc->data, render_count * sizeof(vec4_t));
            memcpy(outNormal, skc->data + render_count * sizeof(vec4_t), render_count * 4 * sizeof(int16_t));
            memcpy(tess.texCoords[baseVertex], skc->data + render_count * (sizeof(vec4_t) + 4 * sizeof(int16_t)),
                   render_count * sizeof(vec2_t));
            s_skcHit++;
            skc = NULL;
            goto skc_skinned;
        }
        s_skcMiss++;
    }

    SKEL_Skin(sf, tiki, skelmodel, mesh, render_count, scale, bones, morphs,
              (qboolean)(backEnd.currentEntity->e.hasMorph != 0), outXyz, outNormal, tess.texCoords[baseVertex]);

    // HZM gl2 [bug-3483] third identical sighting in a row: keep what was just skinned
    if (skc && skc->same >= 2) {
        SKC_Store(skc, (int)baseVertex, (int)render_count);
    }
skc_skinned:
    if (r_hzmSkelCache->integer == 2 && tr.frameCount - s_skcStatFrame > 120) {
        ri.Printf(PRINT_ALL, "^~^~^ SKELCACHE hit=%d miss=%d store=%d over %d frames, %d KB held | threads=%d jobs=%d "
                  "prepared-hits=%d used=%d arena=%d KB\n", s_skcHit, s_skcMiss, s_skcStore, tr.frameCount - s_skcStatFrame,
                  s_skcBytes / 1024, s_psThreads ? (int)s_psThreads->size() : 0, s_psStatJobs, s_psStatHits, s_psStatUsed,
                  s_psArenaUsed / 1024);
        s_skcHit = s_skcMiss = s_skcStore = 0;
        s_psStatJobs = s_psStatHits = s_psStatUsed = 0;
        s_skcStatFrame = tr.frameCount;
    }

#if 0
	if( backEnd.currentEntity->e.staticModelIndex ) {
		mstaticModel_t *sm;
		color4ub_t *out;
		color3ub_t *in;
		int cdofs;
		skdSurface_t *sf2;

		sm = &tr.world->staticModels[ backEnd.currentEntity->e.staticModelIndex - 1 ];

		tess.useStaticModelVertexColors = qtrue;

		cdofs = 0;
		sf2 = tiki->surfs;
		while( sf2 != sf ) {
			cdofs += sf2->numVerts;
			sf2 = ( skdSurface_t* )( ( ( byte* )sf2 ) + sf2->ofsEnd );
		}

		in = &tr.world->smColors[ sm->firstVert + cdofs ];
		out = tess.vertexColors + baseVertex;
		for( i = 0; i < sf->numVerts; i++, in++, out++ ) {
#    if 1
			( *out )[ 0 ] = ( *in )[ 0 ];
			( *out )[ 1 ] = ( *in )[ 1 ];
			( *out )[ 2 ] = ( *in )[ 2 ];
			( *out )[ 3 ] = 255;
#    elif 0
			( ( int* )out ) = tr.identityLightByte;
#    else		
			// su44: set it to something special so
			// I can debug vertex colors rendering
			( *out )[ 0 ] = 255;
			( *out )[ 1 ] = 0;
			( *out )[ 2 ] = 0;
			( *out )[ 3 ] = 255;
#    endif
		}
	} //else
#endif
    //{
    //	// an attemp to fix bizarre vertex colors bug
    //	color4ub_t *col;
    //
    //	col = &tess.vertexColors[baseVertex];
    //	for(i = 0; i < sf->numVerts; i++,col++) {
    //		(*col)[0] = 255;
    //		(*col)[1] = 255;
    //		(*col)[2] = 255;
    //		(*col)[3] = 255;
    //	}
    //}
    //tess.numVertexes += sf->numVerts;

    // HZM coop - gore tier 4 (UV wounds): this surface's CPU-skinned verts +
    // diffuse UVs are now sitting in tess (model space, current pose). Ray-test
    // any pending bullet impacts against exactly these triangles so a hit can
    // be painted into the entity's wound texture at the true surface UV.
    // Only bleedable humans: the TIKI ischaracter flag (same flag that gates
    // the server's location-damage trace) marks players + allied AND enemy
    // human AI, and excludes vehicles/turrets/props.
    // (HZM gl2 re-port bug-gl2-gore, mirrors gl1 tr_model.cpp:1455-1464)
    // ^~^~^ SKELDRAW / SKELVERTS / SKELAGG: emitted skinned-vertex bounds in MODEL and WORLD space.
    // SKELDRAW = per-model one-shot (legacy). SKELVERTS = per-surface (per mesh/surf). SKELAGG = the
    // per-model AGGREGATE across ALL surfaces this frame (resolves collapse-vs-full-size). For char
    // models the aggregate is accumulated every surface for the first few frames.
    {
        trRefEntity_t *e      = backEnd.currentEntity;
        qboolean       isChar = (qboolean)(e->e.tiki && e->e.tiki->a && e->e.tiki->a->bIsCharacter);
        qboolean       wantAgg =
            (qboolean)(isChar && e->e.hModel > 0 && e->e.hModel < MAX_MOD_KNOWN && g_aggFlushed[e->e.hModel] < 6);

        if (R_SkelDiag_DrawWanted() || wantAgg) {
            vec3_t       mmin, mmax, wmin, wmax;
            unsigned int vn;
            int          k;
            mmin[0] = mmin[1] = mmin[2] = 1e9f;
            mmax[0] = mmax[1] = mmax[2] = -1e9f;
            wmin[0] = wmin[1] = wmin[2] = 1e9f;
            wmax[0] = wmax[1] = wmax[2] = -1e9f;
            for (vn = 0; vn < render_count; vn++) {
                const float *p = tess.xyz[baseVertex + vn];
                vec3_t       w;
                for (k = 0; k < 3; k++) {
                    if (p[k] < mmin[k]) mmin[k] = p[k];
                    if (p[k] > mmax[k]) mmax[k] = p[k];
                }
                // model -> world (rigid): origin + p.x*axis0 + p.y*axis1 + p.z*axis2
                w[0] = e->e.origin[0] + p[0] * e->e.axis[0][0] + p[1] * e->e.axis[1][0] + p[2] * e->e.axis[2][0];
                w[1] = e->e.origin[1] + p[0] * e->e.axis[0][1] + p[1] * e->e.axis[1][1] + p[2] * e->e.axis[2][1];
                w[2] = e->e.origin[2] + p[0] * e->e.axis[0][2] + p[1] * e->e.axis[1][2] + p[2] * e->e.axis[2][2];
                for (k = 0; k < 3; k++) {
                    if (w[k] < wmin[k]) wmin[k] = w[k];
                    if (w[k] > wmax[k]) wmax[k] = w[k];
                }
            }

            if (R_SkelDiag_DrawWanted()) {
                R_SkelDiag_Draw((int)render_count, (unsigned int)render_count, "-", mmin, mmax);
            }
            if (wantAgg) {
                if (g_skelVertsLines < 240) {
                    g_skelVertsLines++;
                    if (R_SkelDiagOn())
                    ri.Printf(PRINT_ALL,
                        "^~^~^ SKELVERTS ent=%d hModel=%d model=%s mesh=%d surf=%d rc=%d "
                        "mbnds=[%.1f %.1f %.1f]-[%.1f %.1f %.1f]\n",
                        e->e.entityNumber, e->e.hModel, e->e.tiki->a->name, mesh, surf, render_count,
                        mmin[0], mmin[1], mmin[2], mmax[0], mmax[1], mmax[2]);
                }
                R_SkelAgg_Add((int)render_count, mmin, mmax, wmin, wmax);
            }
        }
    }

    if (tiki && tiki->a && tiki->a->bIsCharacter) {
        R_GoreSkelSurfaceCheck((int)baseVertex, (int)baseIndex);
    }
}

/*
=============
RB_StaticMesh
=============
*/
void RB_StaticMesh(staticSurface_t *staticSurf)
{
    int                i, j;
    dtiki_t           *tiki;
    skelSurfaceGame_t *surf;
    int                meshNum;
    skelHeaderGame_t  *skelmodel;
    int                render_count;
    skelIndex_t       *collapse_map;
    skelIndex_t       *triangles;
    int                indexes;
    int                baseIndex, baseVertex;
    short              collapse[1000];

    if (!r_drawstaticmodelpoly->integer) {
        return;
    }

    assert(backEnd.currentStaticModel);
    tiki = backEnd.currentStaticModel->tiki;
    surf = staticSurf->surface;

    assert(surf->pStaticXyz);
    if (!surf->pStaticXyz) {
        return;
    }

    meshNum   = staticSurf->meshNum;
    skelmodel = ri.TIKI_GetSkel(tiki->mesh[meshNum]);

    //
    // Process LOD
    //
    // HZM gl2 P3: W is baked at FULL detail - the LOD here reads the main view's lodpercentage, which would make W's
    // content depend on where the camera was when it was baked
    if (skelmodel->pLOD && r_staticlod->integer && !(backEnd.viewParms.flags & VPF_SUNWORLD)) {
        float lod_val;

        lod_val = backEnd.currentStaticModel->lodpercentage[0];

        if (surf->numVerts > 3) {
            skelIndex_t *collapseIndex;
            int          mid, low, high;
            int          lod_cutoff;

            if (lod_tool->integer && !strcmp(backEnd.currentStaticModel->tiki->a->name, lod_tikiname->string)
                && meshNum == lod_mesh->integer) {
                lod_cutoff = GetToolLodCutoff(skelmodel, backEnd.currentStaticModel->lodpercentage[0]);
            } else {
                lod_cutoff = GetLodCutoff(skelmodel, backEnd.currentStaticModel->lodpercentage[0], 0);
            }

            collapseIndex = surf->pCollapseIndex;
            if (collapseIndex[2] < lod_cutoff) {
                return;
            }

            low = mid = 3;
            high      = surf->numVerts;
            while (high >= low) {
                mid = (low + high) >> 1;
                if (collapseIndex[mid] < lod_cutoff) {
                    high = mid - 1;
                    if (collapseIndex[mid - 1] >= lod_cutoff) {
                        break;
                    }
                } else {
                    mid++;
                    low = mid;
                    if (high == mid || collapseIndex[mid] < lod_cutoff) {
                        break;
                    }
                }
            }

            render_count = mid;
        } else {
            render_count = surf->numVerts;
        }

        if (!render_count) {
            return;
        }
    } else {
        render_count = surf->numVerts;
    }

    indexes = surf->numTriangles * 3;
    RB_CHECKOVERFLOW(render_count, surf->numTriangles);

    collapse_map = surf->pCollapse;
    triangles    = surf->pTriangles;
    baseIndex    = tess.numIndexes;
    baseVertex   = tess.numVertexes;
    tess.numVertexes += render_count;

    if (render_count == surf->numVerts) {
        for (j = 0; j < indexes; j++) {
            tess.indexes[baseIndex + j] = baseVertex + triangles[j];
        }

        staticModelNumIndexes[backEnd.currentStaticModel - backEnd.refdef.staticModels] += indexes;
        tess.numIndexes += indexes;
    } else {
        for (i = 0; i < render_count; i++) {
            collapse[i] = i;
        }
        for (i = render_count; i < surf->numVerts; i++) {
            collapse[i] = collapse[collapse_map[i]];
        }

        for (j = 0; j < indexes; j += 3) {
            if (collapse[triangles[j]] == collapse[triangles[j + 1]]
                || collapse[triangles[j + 1]] == collapse[triangles[j + 2]]
                || collapse[triangles[j + 2]] == collapse[triangles[j]]) {
                break;
            }

            tess.indexes[baseIndex + j]     = baseVertex + collapse[triangles[j]];
            tess.indexes[baseIndex + j + 1] = baseVertex + collapse[triangles[j + 1]];
            tess.indexes[baseIndex + j + 2] = baseVertex + collapse[triangles[j + 2]];
        }

        staticModelNumIndexes[backEnd.currentStaticModel - backEnd.refdef.staticModels] += j;
        tess.numIndexes += j;
    }

    for (j = 0; j < render_count; j++) {
        Vector4Copy(surf->pStaticXyz[j], tess.xyz[baseVertex + j]);
        // HZM gl2 re-port (bug-gl2-modellight): pStaticNormal is float, gl2's
        // tess.normal is int16-packed - the old Vector4Copy truncated unit
        // normals to 0/1, breaking any per-vertex lighting on static models
        R_VaoPackNormal(tess.normal[baseVertex + j], surf->pStaticNormal[j]);
        tess.texCoords[baseVertex + j][0]   = surf->pStaticTexCoords[j][0][0];
        tess.texCoords[baseVertex + j][1]   = surf->pStaticTexCoords[j][0][1];
        tess.lightCoords[baseVertex + j][0] = surf->pStaticTexCoords[j][1][0];
        tess.lightCoords[baseVertex + j][1] = surf->pStaticTexCoords[j][1][1];
    }

    if (backEndData->staticModelData) {
        const size_t offset =
            backEnd.currentStaticModel->firstVertexData + staticSurf->ofsStaticData * sizeof(color4ub_t);
        assert(offset < tr.world->numStaticModelData * sizeof(color4ub_t));
        assert(offset + render_count * sizeof(color4ub_t) <= tr.world->numStaticModelData * sizeof(color4ub_t));

        const color4ub_t *in = (const color4ub_t *)&backEndData->staticModelData[offset];

        for (i = 0; i < render_count; i++) {
            tess.color[baseVertex + i][0] = in[i][0] * 0xffff / 0xff;
            tess.color[baseVertex + i][1] = in[i][1] * 0xffff / 0xff;
            tess.color[baseVertex + i][2] = in[i][2] * 0xffff / 0xff;
            tess.color[baseVertex + i][3] = in[i][3] * 0xffff / 0xff;
        }
    } else {
        for (i = 0; i < render_count; i++) {
            tess.color[baseVertex + i][0] = 0xffff;
            tess.color[baseVertex + i][1] = 0xffff;
            tess.color[baseVertex + i][2] = 0xffff;
            tess.color[baseVertex + i][3] = 0xffff;
        }
    }
}

/*
=============
R_InfoWorldTris_f
=============
*/
void R_InfoWorldTris_f(void)
{
    int i;

    g_bInfoworldtris = qtrue;

    for (i = 0; i < ARRAY_LEN(entityNumIndexes); i++) {
        entityNumIndexes[i] = 0;
    }
    for (i = 0; i < ARRAY_LEN(staticModelNumIndexes); i++) {
        staticModelNumIndexes[i] = 0;
    }
}

/*
=============
R_PrintInfoWorldtris
=============
*/
void R_PrintInfoWorldtris(void)
{
    int               i;
    int               numTris;
    int               totalNumTris;
    dtiki_t          *tiki;
    skelHeaderGame_t *skelmodel;

    totalNumTris = 0;

    for (i = 0; i < ARRAY_LEN(entityNumIndexes); i++) {
        numTris = entityNumIndexes[i] / 3;
        if (!numTris) {
            continue;
        }

        totalNumTris += numTris;
        tiki      = backEnd.refdef.entities[i].e.tiki;
        skelmodel = ri.TIKI_GetSkel(tiki->mesh[0]);
        Com_Printf("ent: %i, tris: %i, %s, version: %i\n", i, numTris, tiki->a->name, skelmodel->version);
    }

    Com_Printf("total entity tris: %i\n\n", totalNumTris);

    totalNumTris = 0;

    for (i = 0; i < ARRAY_LEN(entityNumIndexes); i++) {
        numTris = staticModelNumIndexes[i] / 3;
        if (!numTris) {
            continue;
        }

        totalNumTris += numTris;
        tiki      = backEnd.refdef.staticModels[i].tiki;
        skelmodel = ri.TIKI_GetSkel(tiki->mesh[0]);
        Com_Printf("sm: %i, tris: %i, %s, version: %i\n", i, numTris, tiki->a->name, skelmodel->version);
    }

    Com_Printf("total static model tris: %i\n\n", totalNumTris);
}

/*
=============
RE_SetFrameNumber
=============
*/
void RE_SetFrameNumber(int frameNumber)
{
    tr.frame_skel_index = frameNumber;
}

/*
=============
R_UpdatePoseInternal
=============
*/
void R_UpdatePoseInternal(refEntity_t *model)
{
    if (model->entityNumber != ENTITYNUM_NONE) {
        // ^~^~^ SKELDIAG: record the pose decision for the frontend trace line.
        g_skeldiagPoseFrameIdx = tr.frame_skel_index;
        g_skeldiagPoseSkelIdx  = tr.skel_index[model->entityNumber];
        if (tr.skel_index[model->entityNumber] == tr.frame_skel_index) {
            g_skeldiagPoseRan = 0; // early-return: pose already computed for this entity this frame
            return;
        }
        tr.skel_index[model->entityNumber] = tr.frame_skel_index;
    } else {
        g_skeldiagPoseFrameIdx = tr.frame_skel_index;
        g_skeldiagPoseSkelIdx  = 0;
        g_skeldiagPoseRan      = -1; // no entity number (won't be deduped)
    }

    ri.TIKI_SetPoseInternal(
        ri.TIKI_GetSkeletor(model->tiki, model->entityNumber),
        model->frameInfo,
        model->bone_tag,
        model->bone_quat,
        model->actionWeight,
        model->bone_count
    );
    g_skeldiagPoseRan = 1; // actually (re)computed the pose this frame
}

/*
=============
RE_ForceUpdatePose
=============
*/
void RE_ForceUpdatePose(refEntity_t *model)
{
    if (model->entityNumber != ENTITYNUM_NONE) {
        tr.skel_index[model->entityNumber] = tr.frame_skel_index;
    }

    ri.TIKI_SetPoseInternal(
        ri.TIKI_GetSkeletor(model->tiki, model->entityNumber),
        model->frameInfo,
        model->bone_tag,
        model->bone_quat,
        model->actionWeight,
        model->bone_count
    );
}

/*
=============
RE_TIKI_Orientation
=============
*/
orientation_t RE_TIKI_Orientation(refEntity_t *model, int tagnum)
{
    // HZM coop - RAGDOLL Hook B (gl2 lockstep): serve tag orientations from the override
    orientation_t coopOr;
    if (R_RagdollGetOrientation(model->entityNumber, model->tiki, tagnum, model->scale, &coopOr)) {
        return coopOr;
    }
    R_UpdatePoseInternal(model);
    return ri.TIKI_OrientationInternal(model->tiki, model->entityNumber, tagnum, model->scale);
}

/*
=============
RE_TIKI_IsOnGround
=============
*/
qboolean RE_TIKI_IsOnGround(refEntity_t *model, int tagnum, float threshold)
{
    R_UpdatePoseInternal(model);
    return ri.TIKI_IsOnGroundInternal(model->tiki, model->entityNumber, tagnum, threshold);
}

/*
=============
R_GetRadius
=============
*/
float R_GetRadius(refEntity_t *model)
{
    R_UpdatePoseInternal(model);
    return ri.GetRadiusInternal(model->tiki, model->entityNumber, model->scale);
}

/*
=============
R_GetFrame
=============
*/
void R_GetFrame(refEntity_t *model, struct skelAnimFrame_s *newFrame)
{
    R_UpdatePoseInternal(model);
    ri.GetFrameInternal(model->tiki, model->entityNumber, newFrame);
}

/*
=============
R_DebugSkeleton
=============
*/
void R_DebugSkeleton(void)
{
    // FIXME: unimplemented (GL2)
}

/*
=============
ProjectRadius
=============
*/
static float ProjectRadius(float r, const vec3_t location)
{
    Vector separation;
    float  projectedRadius;

    separation      = Vector(tr.viewParms.or.origin) - Vector(location);
    projectedRadius = separation.length();

    return fabs(r) * (100.0 / tr.viewParms.fovX) / projectedRadius;
}

/*
=============
R_CalcLod
=============
*/
float R_CalcLod(const vec3_t origin, float radius)
{
    return ProjectRadius(radius, origin);
}

/*
=============
R_CullTIKI
=============
*/
static int R_CullSkelModel(dtiki_t *tiki, refEntity_t *e, skelAnimFrame_t *newFrame, float fScale, float *vLocalOrg)
{
    vec3_t bounds[2];
    vec3_t delta;
    int    i;
    int    cull;

    // FIXME: not working properly
    return CULL_IN;

    if (tr.currentEntity->e.renderfx & RF_FRAMELERP) {
        VectorSubtract(e->origin, e->oldorigin, delta);
    } else {
        VectorClear(delta);
    }

    for (i = 0; i < 3; i++) {
        bounds[0][i] = newFrame->bounds[0][i] * fScale + vLocalOrg[i];
        bounds[1][i] = newFrame->bounds[1][i] * fScale + vLocalOrg[i];

        if (delta[i] > 0) {
            bounds[1][i] += delta[i];
        } else {
            bounds[0][i] += delta[i];
        }
    }

    cull = R_CullLocalBox(bounds);

    if (r_showcull->integer & 1) {
        float  fR, fG, fB;
        vec3_t vAngles;

        switch (cull) {
        case CULL_IN:
            fR = 0;
            fG = 1;
            fB = 0;
            break;
        case CULL_CLIP:
            fR = 1;
            fG = 1;
            fB = 0;
            break;
        case CULL_OUT:
            fR = 1;
            fG = 0.2f;
            fB = 0.2f;

            for (i = 0; i < 3; i++) {
                bounds[0][i] -= 16;
                bounds[1][i] += 16;
            }
            break;
        }

        MatrixToEulerAngles(tr.or.axis, vAngles);
        R_DebugRotatedBBox(tr.or.origin, vAngles, bounds[0], bounds[1], fR, fG, fB, 0.5);
    }

    switch (cull) {
    case CULL_IN:
        tr.pc.c_box_cull_md3_in++;
        return CULL_IN;
    case CULL_CLIP:
        tr.pc.c_box_cull_md3_clip++;
        return CULL_CLIP;
    case CULL_OUT:
    default:
        tr.pc.c_box_cull_md3_out++;
        return CULL_OUT;
    }
}

/*
==================
R_CountTikiLodTris

Computes the number of triangles to be rendered for the given TIKI model
based on the given LoD percentage, and passes it back with render_tris.
The total number of triangles in the TIKI model are passed back with total_tris.
Currently only used for debugging purposes.

FIXME: Shares some code with RB_StaticMesh, common parts could be extracted.
According to debug symbols, this was originally in tiki_mesh.cpp
==================
*/
void R_CountTikiLodTris(dtiki_t *tiki, float lodpercentage, int *render_tris, int *total_tris)
{
    *render_tris = 0;
    *total_tris  = 0;
    int numtris = 0, totaltris = 0;

    for (int i = 0; i < tiki->numMeshes; i++) {
        skelHeaderGame_t  *skelmodel = ri.TIKI_GetSkel(tiki->mesh[i]);
        skelSurfaceGame_t *surface   = skelmodel->pSurfaces;

        for (int j = 0; j < skelmodel->numSurfaces; j++) {
            int render_count = 0;
            if (skelmodel->pLOD) {
                int          lod_cutoff    = GetLodCutoff(skelmodel, lodpercentage, 0);
                skelIndex_t *collapseIndex = surface->pCollapseIndex;
                render_count               = surface->numVerts;

                // Determine the number of vertices to be rendered based on the LOD cutoff
                while (render_count > 0 && collapseIndex[render_count - 1] < lod_cutoff) {
                    render_count--;
                }
            } else {
                // The surf mesh doesn't have a LOD model, so all vertices will be rendered
                render_count = surface->numVerts;
            }

            skelIndex_t *triangles    = surface->pTriangles;
            int          indexes      = surface->numTriangles * 3; // 3 vertex/index for each tri
            skelIndex_t *collapse_map = surface->pCollapse;
            totaltris += surface->numTriangles;
            skelIndex_t collapse[4096] {};

            // Initialize array for collapsed indices
            int k;
            for (k = 0; k < render_count; ++k) {
                collapse[k] = k;
            }

            // Map remaining vertices to their collapsed indices using the collapse map
            for (k = render_count; k < surface->numVerts; ++k) {
                collapse[k] = collapse[collapse_map[k]];
            }

            // Check the first two collapsed indices of each triangle
            for (k = 0; k < indexes; k += 3) {
                if (collapse[triangles[k]] == collapse[triangles[k + 1]]) {
                    // The collapsed indices of the two vertices are the same:
                    // this, and any subsequent tris do not need to be rendered
                    // because the collapsed vertices (for this surface) coincide from here.
                    break;
                }
            }

            surface = surface->pNext;
            numtris += k / 3;
        }
    }

    *render_tris = numtris;
    *total_tris  = totaltris;
}

/*
==================
R_LerpTag
==================
*/
int R_LerpTag(orientation_t *tag, qhandle_t handle, int startFrame, int endFrame, float frac, const char *tagName)
{
    // stub
    return 0;
}