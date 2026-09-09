/*
===========================================================================
Copyright (C) 2025 the OpenMoHAA team

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
// Functions for doing model animation and attachments

#include "cg_local.h"
#include "tiki.h"

static qboolean cg_forceModelAllowed = qfalse;

// HZM coop [225] - set while dispatching frame commands for the draw-skipped 1P turret viewmodel
// in 3P: CG_ProcessEntityCommands (cg_commands.cpp) then runs only the SOUND-family commands, so
// the hidden gun keeps firing audio but never spawns its muzzle flash at the player's eyes.
qboolean cg_bCoopMuteVisualCmds = qfalse;

/* HZM coop [user 2026-08-21] GUNVIS PROBE. "random times now where my gun will just disappear,
   any gun at all, and then come back."

   There are FOUR independent paths that can blank the weapon or the arms, and guessing between
   them has a poor record on this project. So record WHICH one fired, with the state that decided
   it. Both probes are EDGE-TRIGGERED off a single shared visibility state per subject - one
   variable, not a hidden flag and a shown flag, because two independent latches can never both
   reset and the probe goes silent after its first print. coop_gunVisTrace 1. */
static int s_coopGunVis  = -1;   /* -1 unknown, 0 shown, 1 hidden - WEAPON */
static int s_coopArmsVis = -1;   /* -1 unknown, 0 shown, 1 hidden - ARMS   */
/* [user 2026-08-22, bug-2049] THE BLIND SPOT. The two probes above sit inside the surface-hide
   branches, so they only ever see a model that IS being drawn and is choosing to blank its
   surfaces. They cannot see a model that is never SUBMITTED - and the whole
   R_AddRefEntityToScene call is wrapped in `!(renderfx & RF_DONTDRAW) && !bCoopHideDraw`.
   A model skipped there vanishes with the arms, together, for as long as the flag holds, and
   logs NOTHING.
   That is exactly what the user reports and exactly what the probe kept missing: nine GUNVIS
   edges captured across a session in which the gun visibly vanished several more times than
   that, with the parent-miss DPrintf confirmed at zero under developer 2. Four hypotheses were
   tested and refuted against the code before instrumenting this one - which is the lesson, not
   the hypotheses. */
static int s_coopDrawVis = -1;   /* -1 unknown, 0 submitted, 1 SKIPPED - whole viewmodel */
static cvar_t *s_pGunVis = NULL;

static void CoopGunVisNote(int *pState, int bHidden, const char *why, int bUnarmed)
{
    /* HZM coop [user 2026-08-22, bug-2048] DEFAULT ON. This probe is EDGE-TRIGGERED - it prints
       only when visibility actually changes, so on a normal session that is a handful of lines,
       and zero while nothing is wrong. Left at 0 it was never armed once across two full capture
       sessions, so when the user asked why the gun vanishes there was no evidence at all and I
       spent the investigation guessing between four paths - which is the exact failure this
       probe was written to prevent. A diagnostic that has to be switched on in advance only ever
       catches bugs you already knew about. */
    // HZM coop [bug-2090] DEFAULTED OFF 2026-08-24. bug-2049 defaulted this probe ON so a session
    // could capture HIDE-WEAPON / HIDE-ARMS edges; bug-2066 CLOSED that hunt (the flicker is
    // EF_UNARMED during the weapon give) and nothing turned it back off, so it shipped armed to
    // players in v1.4.4 - the same class the v1.4.0 audit caught for coop_goreDebug/coop_profProbe.
    //
    // The default change alone reaches NOBODY. It is CVAR_ARCHIVE and shipped as 1, so every player
    // who has already launched carries `seta coop_gunVisTrace "1"` and Cvar_Get keeps an existing
    // value while updating only the reset string. Clear a stale 1 ONCE per session - the same fossil
    // fix used for coop_lowAmmoTell and coop_idleBolt (TRAPS T7). The static means a deliberate
    // `coop_gunVisTrace 1` typed at the console afterwards still holds for the rest of the session.
    if (!s_pGunVis) {
        s_pGunVis = cgi.Cvar_Get("coop_gunVisTrace", "0", CVAR_ARCHIVE);
        if (s_pGunVis->integer) { cgi.Cvar_Set("coop_gunVisTrace", "0"); }
    }
    if (!s_pGunVis->integer) { *pState = bHidden; return; }
    if (*pState == bHidden)  { return; }
    *pState = bHidden;
    cgi.Printf("^~^~^ GUNVIS t=%d %s inzoom=%d unarmed=%d drawvm=%d health=%d vmanim=%d pmflags=0x%x\n",
               cg.time, why,
               cg.snap ? cg.snap->ps.stats[STAT_INZOOM] : -1,
               bUnarmed,
               cg_drawviewmodel ? cg_drawviewmodel->integer : -1,
               cg.snap ? cg.snap->ps.stats[STAT_HEALTH] : -1,
               cg.snap ? cg.snap->ps.iViewModelAnim : -1,
               cg.snap ? cg.snap->ps.pm_flags : 0);
}

/*
===============
CG_GetPlayerModelTiki
===============
*/
const char *CG_GetPlayerModelTiki(const char *modelName)
{
    return va("models/player/%s.tik", modelName);
}

/*
===============
CG_GetPlayerLocalModelTiki
===============
*/
const char *CG_GetPlayerLocalModelTiki(const char *modelName)
{
    return va("models/player/%s_fps.tik", modelName);
}

/*
===============
CG_PlayerTeamIcon
===============
*/
void CG_PlayerTeamIcon(refEntity_t *pModel, entityState_t *pPlayerState)
{
    qboolean bInArtillery, bInTeam, bSpecialIcon;

    if (cg_protocol < PROTOCOL_MOHTA_MIN) {
        if (pPlayerState->eFlags & EF_ALLIES) {
            cg.clientinfo[pPlayerState->number].team = TEAM_ALLIES;
        } else if (pPlayerState->eFlags & EF_AXIS) {
            cg.clientinfo[pPlayerState->number].team = TEAM_AXIS;
        } else {
            cg.clientinfo[pPlayerState->number].team = TEAM_NONE;
        }
    }

    if (pPlayerState->number == cg.snap->ps.clientNum) {
        return;
    }

    bInTeam      = qfalse;
    bSpecialIcon = qfalse;
    if (cgs.gametype > GT_FFA
        && (cg.snap->ps.stats[STAT_TEAM] == TEAM_ALLIES && (pPlayerState->eFlags & EF_ALLIES)
            || cg.snap->ps.stats[STAT_TEAM] == TEAM_AXIS && (pPlayerState->eFlags & EF_AXIS)
            || cg.snap->ps.stats[STAT_TEAM] != TEAM_AXIS && cg.snap->ps.stats[STAT_TEAM] != TEAM_ALLIES
                   && (pPlayerState->eFlags & EF_ANY_TEAM) != 0)) {
        bInTeam = qtrue;
    }

    if (cgs.gametype <= GT_FFA) {
        return;
    }

    bInArtillery = qfalse;
    if (pPlayerState->eFlags & EF_PLAYER_ARTILLERY) {
        bInArtillery = qtrue;
    }

    if (bInTeam || (pPlayerState->eFlags & (EF_PLAYER_IN_MENU | EF_PLAYER_TALKING)) || bInArtillery) {
        int         i;
        int         iTag;
        float       fAlpha;
        float       fDist;
        vec3_t      vTmp;
        refEntity_t iconEnt;

        memset(&iconEnt, 0, sizeof(iconEnt));
        if ((pPlayerState->eFlags & EF_PLAYER_TALKING) != 0 && ((cg.time >> 8) & 1) != 0) {
            iconEnt.hModel = cgi.R_RegisterModel("textures/hud/talking_headicon.spr");
            bSpecialIcon   = qtrue;
        } else if ((pPlayerState->eFlags & EF_PLAYER_IN_MENU) != 0) {
            iconEnt.hModel = cgi.R_RegisterModel("textures/hud/inmenu_headicon.spr");
            bSpecialIcon   = qtrue;
        } else {
            if (!bInTeam) {
                return;
            }

            if (bInArtillery) {
                iconEnt.hModel = cgi.R_RegisterModel("textures/hud/inmenu_artilleryicon.spr");
                bSpecialIcon   = qtrue;
            } else if ((pPlayerState->eFlags & 0x80) != 0) {
                iconEnt.hModel = cgi.R_RegisterModel("textures/hud/allies_headicon.spr");
            } else {
                iconEnt.hModel = cgi.R_RegisterModel("textures/hud/axis_headicon.spr");
            }
        }

        memset(vTmp, 0, sizeof(vTmp));
        AnglesToAxis(vTmp, iconEnt.axis);

        iconEnt.scale              = 0.5f;
        iconEnt.renderfx           = 0;
        iconEnt.reType             = RT_SPRITE;
        iconEnt.shaderTime         = 0.0f;
        iconEnt.frameInfo[0].index = 0;
        iconEnt.shaderRGBA[0]      = -1;
        iconEnt.shaderRGBA[1]      = -1;
        iconEnt.shaderRGBA[2]      = -1;
        VectorCopy(pModel->origin, iconEnt.origin);

        iTag = cgi.Tag_NumForName(pModel->tiki, "eyes bone");
        if (iTag == -1) {
            iconEnt.origin[2] = iconEnt.origin[2] + 96.0f;
        } else {
            orientation_t oEyes = cgi.TIKI_Orientation(pModel, iTag);

            for (i = 0; i < 3; ++i) {
                VectorMA(iconEnt.origin, oEyes.origin[i], pModel->axis[i], iconEnt.origin);
            }

            iconEnt.origin[2] = iconEnt.origin[2] + 20.0f;
        }

        VectorSubtract(iconEnt.origin, cg.refdef.vieworg, vTmp);
        fDist = VectorLength(vTmp);

        if (fDist < 256.0f) {
            iconEnt.scale = fDist / 853.0f + 0.2f;
        } else if (fDist > 512.0f) {
            // Make sure to scale so the icon can be seen far away
            iconEnt.scale = (fDist - 512.0f) / 2560.0f + 0.5f;
        }

        if (iconEnt.scale > 1.0f) {
            iconEnt.scale = 1.0f;
        }

        if (fDist > 256.0) {
            fAlpha = 1.0f;
        } else if (fDist >= 72.0f) {
            fAlpha = (fDist - 72.0f) / 184.0f;
        } else {
            fAlpha = 0.0f;
        }

        if (cg.snap->ps.stats[STAT_TEAM] == TEAM_ALLIES || cg.snap->ps.stats[STAT_TEAM] == TEAM_AXIS) {
            fAlpha = fAlpha * 0.65f;
        } else {
            fAlpha = fAlpha * 0.4f;
        }

        if (bSpecialIcon) {
            int value = (int)((fAlpha + 0.6f) * 255.0f);
            if (value > 255) {
                value = 255;
            }
            iconEnt.shaderRGBA[3] = value;
        } else {
            iconEnt.shaderRGBA[3] = (int)(fAlpha * 255.0f);
        }

        if (fAlpha > 0.0 || bSpecialIcon) {
            if (bSpecialIcon) {
                VectorMA(iconEnt.origin, -2.0f, cg.refdef.viewaxis[0], iconEnt.origin);
                iconEnt.scale += 0.05f;
            }

            cgi.R_AddRefSpriteToScene(&iconEnt);

            if (bSpecialIcon && bInTeam && fAlpha > 0.0f) {
                if (pPlayerState->eFlags & EF_ALLIES) {
                    iconEnt.hModel = cgi.R_RegisterModel("textures/hud/allies_headicon.spr");
                } else {
                    iconEnt.hModel = cgi.R_RegisterModel("textures/hud/axis_headicon.spr");
                }
                VectorMA(iconEnt.origin, 4.0f, cg.refdef.viewaxis[0], iconEnt.origin);
                iconEnt.scale         = iconEnt.scale - 0.1;
                iconEnt.shaderRGBA[3] = (int)(fAlpha * 255.0f);
                cgi.R_AddRefSpriteToScene(&iconEnt);
            }
        }
    }
}

/*
===============
CG_ActorOverheadIcon
Draws an authentic MP-style overhead faction marker above an AI actor's head, using
the SAME render path as CG_PlayerTeamIcon: a world-space RT_SPRITE billboard submitted
via cgi.R_AddRefSpriteToScene. The engine sizes/positions it, so it sits centered above
the head and stays a readable size at range (the previous hand-rolled 2D screen
projection drifted off to the side and mis-scaled). bEnemy selects the swastika sprite
(axis) vs the allied star sprite. Anchored to the head tag + 20 units, same as players.
===============
*/
/* iconType: 0 = allied star, 1 = axis swastika, 2 = officer eagle (Reichsadler) */
static void CG_ActorOverheadIcon(refEntity_t *pModel, int iconType)
{
    int         i, iTag;
    float       fAlpha, fDist;
    vec3_t      vTmp;
    refEntity_t iconEnt;
    const char *sprName;

    if (iconType == 2) {
        sprName = "textures/hud/coop_officer_icon.spr";
    } else if (iconType == 1) {
        sprName = "textures/hud/coop_axis_icon.spr";
    } else {
        sprName = "textures/hud/coop_ally_icon.spr";
    }

    memset(&iconEnt, 0, sizeof(iconEnt));
    iconEnt.hModel = cgi.R_RegisterModel(sprName);
    if (!iconEnt.hModel) {
        return;
    }

    memset(vTmp, 0, sizeof(vTmp));
    AnglesToAxis(vTmp, iconEnt.axis);

    iconEnt.scale              = 0.5f;
    iconEnt.renderfx           = 0;
    iconEnt.reType             = RT_SPRITE;
    iconEnt.shaderTime         = 0.0f;
    iconEnt.frameInfo[0].index = 0;
    iconEnt.shaderRGBA[0]      = -1;
    iconEnt.shaderRGBA[1]      = -1;
    iconEnt.shaderRGBA[2]      = -1;
    VectorCopy(pModel->origin, iconEnt.origin);

    iTag = cgi.Tag_NumForName(pModel->tiki, "eyes bone");
    if (iTag == -1) {
        iTag = cgi.Tag_NumForName(pModel->tiki, "Bip01 Head");
    }
    if (iTag == -1) {
        iconEnt.origin[2] = iconEnt.origin[2] + 96.0f;
    } else {
        orientation_t oHead = cgi.TIKI_Orientation(pModel, iTag);
        for (i = 0; i < 3; ++i) {
            VectorMA(iconEnt.origin, oHead.origin[i], pModel->axis[i], iconEnt.origin);
        }
        iconEnt.origin[2] = iconEnt.origin[2] + 20.0f;
    }

    VectorSubtract(iconEnt.origin, cg.refdef.vieworg, vTmp);
    fDist = VectorLength(vTmp);

    if (fDist < 256.0f) {
        iconEnt.scale = fDist / 853.0f + 0.2f;
    } else if (fDist > 512.0f) {
        iconEnt.scale = (fDist - 512.0f) / 2560.0f + 0.5f;
    }
    if (iconEnt.scale > 1.0f) {
        iconEnt.scale = 1.0f;
    }

    if (fDist > 256.0f) {
        fAlpha = 1.0f;
    } else if (fDist >= 72.0f) {
        fAlpha = (fDist - 72.0f) / 184.0f;
    } else {
        fAlpha = 0.0f;
    }
    iconEnt.shaderRGBA[3] = (int)(fAlpha * 255.0f);

    // HZM coop - sprite world size = image pixel dims * scale (tr_model sprite path), so the HD icon
    // upscale (textures/hud/coop_*_icon.tga 32 -> 128) quadrupled the on-screen size ("overhead icons
    // are now MASSIVE"). Normalize back to the 32px-authored world size the distance curve above expects.
    iconEnt.scale *= 32.0f / 128.0f;

    if (fAlpha > 0.0f) {
        cgi.R_AddRefSpriteToScene(&iconEnt);
    }
}

/* Retained as an empty stub so CG_Draw2D's call (cg_drawtools.cpp) still links. The
 * old deferred 2D-stretchpic icon buffer it used to flush is gone, replaced by the
 * RT_SPRITE world sprite in CG_ActorOverheadIcon above. */
void CG_DrawCoopIcons(void)
{
}

/*
===============
CG_InterpolateAnimParms

Interpolate between current and next entity
===============
*/
void CG_InterpolateAnimParms(entityState_t *state, entityState_t *sNext, refEntity_t *model)
{
    static cvar_t *vmEntity = NULL;
    int            i;
    float          t;
    float          animLength;
    float          t1, t2;

    if (!vmEntity) {
        vmEntity = cgi.Cvar_Get("viewmodelanim", "1", 0);
    }

    if (sNext && sNext->usageIndex == state->usageIndex) {
        t1 = cg.time - cg.snap->serverTime;
        t2 = cg.nextSnap->serverTime - cg.snap->serverTime;
        t  = t1 / t2;

        model->actionWeight = (sNext->actionWeight - state->actionWeight) * t + state->actionWeight;

        for (i = 0; i < MAX_FRAMEINFOS; i++) {
            if (sNext->frameInfo[i].weight) {
                model->frameInfo[i].index = sNext->frameInfo[i].index;
                if (sNext->frameInfo[i].index == state->frameInfo[i].index && state->frameInfo[i].weight) {
                    model->frameInfo[i].weight =
                        (sNext->frameInfo[i].weight - state->frameInfo[i].weight) * t + state->frameInfo[i].weight;

                    if (sNext->frameInfo[i].time >= state->frameInfo[i].time) {
                        model->frameInfo[i].time =
                            (sNext->frameInfo[i].time - state->frameInfo[i].time) * t + state->frameInfo[i].time;
                    } else {
                        animLength = cgi.Anim_Time(model->tiki, sNext->frameInfo[i].index);
                        if (!animLength) {
                            t1 = 0.0;
                        } else {
                            t1 = (animLength + sNext->frameInfo[i].time - state->frameInfo[i].time) * t
                               + state->frameInfo[i].time;
                        }

                        t2 = t1;
                        while (t2 > animLength) {
                            t2 -= animLength;

                            if (t2 == t1) {
                                t2 = 1.0;
                                break;
                            }

                            t1 = t2;
                        }

                        model->frameInfo[i].time = t2;
                    }
                } else {
                    animLength = cgi.Anim_Time(model->tiki, sNext->frameInfo[i].index);
                    if (!animLength) {
                        t1 = 0.0;
                    } else {
                        t1 = sNext->frameInfo[i].time - (cg.nextSnap->serverTime - cg.time) / 1000.0;
                    }

                    model->frameInfo[i].time   = Q_max(0, t1);
                    model->frameInfo[i].weight = sNext->frameInfo[i].weight;
                }
            } else if (sNext->frameInfo[i].index == state->frameInfo[i].index) {
                animLength = cgi.Anim_Time(model->tiki, sNext->frameInfo[i].index);
                if (!animLength) {
                    t1 = 0.0;
                } else {
                    t1 = (cg.time - cg.snap->serverTime) / 1000.0 + state->frameInfo[i].time;
                }

                model->frameInfo[i].index  = Q_clamp_int(state->frameInfo[i].index, 0, model->tiki->a->num_anims - 1);
                model->frameInfo[i].time   = Q_min(animLength, t1);
                model->frameInfo[i].weight = (1.0 - t) * state->frameInfo[i].weight;
            } else {
                model->frameInfo[i].index  = -1;
                model->frameInfo[i].weight = 0.0;
            }
        }
    } else {
        // no next state, don't blend anims

        model->actionWeight = state->actionWeight;
        for (i = 0; i < MAX_FRAMEINFOS; i++) {
            if (state->frameInfo[i].weight) {
                model->frameInfo[i].index  = Q_clamp_int(state->frameInfo[i].index, 0, model->tiki->a->num_anims - 1);
                model->frameInfo[i].time   = state->frameInfo[i].time;
                model->frameInfo[i].weight = state->frameInfo[i].weight;
            } else {
                model->frameInfo[i].index  = -1;
                model->frameInfo[i].weight = 0.0;
            }
        }
    }

    if (vmEntity->integer == state->number) {
        static cvar_t *curanim;
        if (!curanim) {
            curanim = cgi.Cvar_Get("viewmodelanimslot", "1", 0);
        }

        cgi.Cvar_Set("viewmodelanimclienttime", va("%0.2f", model->frameInfo[curanim->integer].time));
    }
}

/*
===============
CG_CastFootShadow

Cast complex foot shadow using lights
===============
*/
void CG_CastFootShadow(const vec_t *vLightPos, vec_t *vLightIntensity, int iTag, refEntity_t *model)
{
    int           i;
    float         fAlpha;
    float         fLength;
    float         fWidth;
    float         fAlphaOfs;
    float         fOfs;
    float         fPitchCos;
    vec3_t        vPos;
    vec3_t        vEnd;
    vec3_t        vDelta;
    vec3_t        vLightAngles;
    trace_t       trace;
    orientation_t oFoot;

    VectorCopy(model->origin, vPos);
    oFoot = cgi.TIKI_Orientation(model, iTag);
    VectorMA(oFoot.origin, 2, oFoot.axis[1], vEnd);
    for (i = 0; i < 3; i++) {
        VectorMA(vPos, vEnd[i], model->axis[i], vPos);
    }

    if (cg_shadowdebug->integer) {
        vec3_t vDir;

        //
        // show debug lines
        //
        memset(vDir, 0, sizeof(vDir));
        for (i = 0; i < 3; ++i) {
            VectorMA(vDir, oFoot.axis[0][i], model->axis[i], vDir);
        }
        VectorMA(vPos, 32.0, vDir, vEnd);
        cgi.R_DebugLine(vPos, vEnd, 1.0, 0.0, 0.0, 1.0);

        memset(vDir, 0, sizeof(vDir));
        for (i = 0; i < 3; ++i) {
            VectorMA(vDir, oFoot.axis[1][i], model->axis[i], vDir);
        }
        VectorMA(vPos, 32.0, vDir, vEnd);
        cgi.R_DebugLine(vPos, vEnd, 0.0, 1.0, 0.0, 1.0);

        memset(vDir, 0, sizeof(vDir));
        for (i = 0; i < 3; ++i) {
            VectorMA(vDir, oFoot.axis[2][i], model->axis[i], vDir);
        }
        VectorMA(vPos, 32.0, vDir, vEnd);
        cgi.R_DebugLine(vPos, vEnd, 0.0, 0.0, 1.0, 1.0);
    }

    // calculate the direction
    VectorSubtract(vLightPos, vPos, vDelta);
    VectorNormalizeFast(vDelta);
    vectoangles(vDelta, vLightAngles);

    // normalize to 180 degrees
    if (vLightAngles[0] > 180) {
        vLightAngles[0] -= 360;
    }

    if (vLightAngles[0] > -5.7319679) {
        // FIXME: what is -5.7319679?
        return;
    }

    fPitchCos = cos(DEG2RAD(vLightAngles[0]));
    if (fPitchCos > 0.955) {
        fAlpha = 1.0 - (fPitchCos - 0.955) * 25;
    } else {
        fAlpha = 1.0;
    }

    fLength = fPitchCos * fPitchCos * 32.0 + fPitchCos * 8.0 + 10.0;
    fOfs    = 0.5 - (-4.1 / tan(DEG2RAD(vLightAngles[0])) + 4.0 - fLength) / fLength * 0.5;
    VectorMA(vPos, -96.0, vDelta, vEnd);
    CG_Trace(&trace, vPos, vec3_origin, vec3_origin, vEnd, 0, MASK_FOOTSHADOW, qfalse, qtrue, "CG_CastFootShadow");

    if (cg_shadowdebug->integer) {
        cgi.R_DebugLine(vPos, vLightPos, 0.75, 0.75, 0.5, 1.0);
        cgi.R_DebugLine(vPos, vEnd, 1.0, 1.0, 1.0, 1.0);
    }

    if (trace.fraction == 1.0) {
        return;
    }

    trace.fraction -= 0.0427f;
    if (trace.fraction < 0) {
        trace.fraction = 0;
    }

    fWidth    = 10.f - (1.f - trace.fraction) * 6.f;
    fAlphaOfs = (1.f - trace.fraction) * fAlpha;

    fAlpha = Q_max(vLightIntensity[0], Q_max(vLightIntensity[1], vLightIntensity[2]));

    if (fAlpha < 0.1) {
        vLightIntensity[0] *= 0.1 / fAlpha * fAlphaOfs;
        vLightIntensity[1] *= 0.1 / fAlpha * fAlphaOfs;
        vLightIntensity[2] *= 0.1 / fAlpha * fAlphaOfs;
    } else {
        vLightIntensity[0] *= fAlphaOfs;
        vLightIntensity[1] *= fAlphaOfs;
        vLightIntensity[2] *= fAlphaOfs;
    }

    fAlpha = Q_max(vLightIntensity[0], Q_max(vLightIntensity[1], vLightIntensity[2]));
    if (fAlpha > 0.6) {
        vLightIntensity[0] *= 0.6 / fAlpha;
        vLightIntensity[1] *= 0.6 / fAlpha;
        vLightIntensity[2] *= 0.6 / fAlpha;
    }

    if (vLightIntensity[0] <= 0.01 && vLightIntensity[1] <= 0.01 && vLightIntensity[2] <= 0.01) {
        return;
    }

    CG_ImpactMark(
        cgs.media.footShadowMarkShader,
        trace.endpos,
        trace.plane.normal,
        vLightAngles[1],
        fWidth,
        fLength,
        vLightIntensity[0],
        vLightIntensity[1],
        vLightIntensity[2],
        1.0,
        qfalse,
        qtrue,
        qfalse,
        qfalse,
        0.5,
        fOfs
    );
}

/*
===============
CG_CastSimpleFeetShadow

Cast basic feet shadow
===============
*/
void CG_CastSimpleFeetShadow(
    const trace_t *pTrace,
    float          fWidth,
    float          fAlpha,
    int            iRightTag,
    int            iLeftTag,
    const dtiki_t *tiki,
    refEntity_t   *model
)
{
    int           i;
    float         fShadowYaw;
    float         fLength;
    vec3_t        vPos, vRightPos, vLeftPos;
    vec3_t        vDelta;
    orientation_t oFoot;

    //
    // right foot
    //
    VectorCopy(pTrace->endpos, vRightPos);
    oFoot = cgi.TIKI_Orientation(model, iRightTag);
    VectorMA(oFoot.origin, 3, oFoot.axis[1], vPos);

    for (i = 0; i < 3; i++) {
        VectorMA(vRightPos, vPos[i], model->axis[i], vRightPos);
    }

    VectorMA(vRightPos, -2, oFoot.axis[1], vRightPos);

    //
    // left foot
    //
    VectorCopy(pTrace->endpos, vLeftPos);
    oFoot = cgi.TIKI_Orientation(model, iLeftTag);
    VectorMA(oFoot.origin, 3, oFoot.axis[1], vPos);

    for (i = 0; i < 3; i++) {
        VectorMA(vLeftPos, vPos[i], model->axis[i], vLeftPos);
    }

    VectorAdd(vRightPos, vLeftPos, vPos);
    VectorScale(vPos, 0.5, vPos);
    VectorSubtract(vRightPos, vLeftPos, vDelta);
    VectorMA(vLeftPos, 0.5, vDelta, vPos);

    // get the facing yaw
    fShadowYaw = vectoyaw(vDelta);
    fLength    = VectorNormalize(vDelta) * 0.5 + 12;
    if (fLength < fWidth * 0.7) {
        fLength = fWidth * 0.7;
    }

    // add the mark
    CG_ImpactMark(
        cgs.media.shadowMarkShader,
        vPos,
        pTrace->plane.normal,
        fShadowYaw,
        fWidth * 0.7,
        fLength,
        fAlpha,
        fAlpha,
        fAlpha,
        1.0,
        qfalse,
        qtrue,
        qfalse,
        qfalse,
        0.5,
        0.5
    );
}

/*
===============
CG_EntityShadow

Returns the Z component of the surface being shadowed

  should it return a full plane instead of a Z?
===============
*/
#define SHADOW_DISTANCE 96

qboolean CG_EntityShadow(centity_t *cent, refEntity_t *model)
{
    int     iTagL, iTagR;
    float   alpha;
    float   fWidth;
    vec3_t  end;
    vec3_t  vMins, vMaxs;
    vec3_t  vSize;
    trace_t trace;

    iTagR = -1;

    if (cg_shadows->integer == 0) {
        return qfalse;
    }

    if (model->renderfx & RF_SKYENTITY) {
        // no shadows on sky entities
        return qfalse;
    }

    // HZM gl2 REAL CHARACTER SHADOWS. renderergl2 publishes r_coopRealShadows = 1 only while
    // it is actually writing skeletal characters into the sun cascade shadow maps
    // (r_charShadows 1 and the sun chain live). Suppress BOTH the HZM Phase-A directional
    // decal immediately below AND every vanilla foot/blob path after it, so the player sees
    // exactly one shadow per actor instead of a real cast shadow with a decal painted on top.
    //
    // GL1-SAFE BY CONSTRUCTION: renderergl1 never sets this cvar, so under gl1 it stays at the
    // "0" default registered right here and this branch is never taken - gl1 behaviour is
    // bit-identical with no reasoning required. This mirrors the existing r_coopSunValid
    // pattern (published by renderergl1/tr_scene.c, consumed just below).
    //
    // NOT cg_shadows: that is the SAME cvar the renderers register their internal r_shadows
    // under, it gates the rend2 pshadow pass, it also gates CG_Splash water marks, and its
    // registration defaults disagree between cgame (0) and both renderers (1).
    //
    // To force the decal back on while real shadows are running: r_charShadowBlob 1
    // (renderer side - it makes the renderer publish 0 here).
    {
        static cvar_t *sRealShadows = NULL;
        if (!sRealShadows) {
            sRealShadows = cgi.Cvar_Get("r_coopRealShadows", "0", 0);
        }
        if (sRealShadows && sRealShadows->integer) {
            return qfalse;
        }
    }

    // HZM coop - PHASE A directional shadow: when coop_shadowDir is on, draw a single elongated,
    // sun-oriented ground decal per model (overrides the straight-down foot/blob shadow). Uses fixed
    // sun-angle cvars (coop_shadowAz/El) so it needs no renderer sun-direction bridge. Pure cgame.
    {
        static cvar_t *sDir, *sAz, *sEl, *sLen, *sAuto, *sSunAz, *sSunEl, *sSunValid;
        if (!sDir) {
            sDir = cgi.Cvar_Get("coop_shadowDir", "1",   CVAR_ARCHIVE);
            sAz  = cgi.Cvar_Get("coop_shadowAz",  "45",  CVAR_ARCHIVE);   // MANUAL sun azimuth (deg)
            sEl  = cgi.Cvar_Get("coop_shadowEl",  "45",  CVAR_ARCHIVE);   // MANUAL sun elevation (deg); lower = longer
            sLen = cgi.Cvar_Get("coop_shadowLen", "1.5", CVAR_ARCHIVE);   // extra length multiplier
            // AUTO: follow the map's REAL sun (published each frame by the renderer, RE_RenderScene) when it has
            // one; otherwise fall back to the manual coop_shadowAz/El above. coop_shadowAuto 0 forces manual.
            sAuto     = cgi.Cvar_Get("coop_shadowAuto", "1",  CVAR_ARCHIVE);
            sSunAz    = cgi.Cvar_Get("r_coopSunAz",     "45", 0);
            sSunEl    = cgi.Cvar_Get("r_coopSunEl",     "45", 0);
            sSunValid = cgi.Cvar_Get("r_coopSunValid",  "0",  0);
        }
        if (sDir->integer) {
            float azDeg = sAz->value;
            float elDeg = sEl->value;
            float w;
            if (sAuto->integer && sSunValid->integer) {
                azDeg = sSunAz->value;   // the map's real sun
                elDeg = sSunEl->value;
            }
            w = model->scale * cgi.R_ModelRadius(model->hModel);
            if (w < 1) {
                return qfalse;
            }
            VectorCopy(model->origin, end);
            end[2] -= SHADOW_DISTANCE;
            cgi.CM_BoxTrace(&trace, model->origin, end, vec3_origin, vec3_origin, 0, MASK_PLAYERSOLID, qfalse);
            if (trace.fraction == 1.0 || trace.startsolid || trace.allsolid) {
                return qfalse;
            }
            alpha = (1.0 - trace.fraction) * 0.65f;
            {
                float elr = elDeg * ((float)M_PI / 180.0f);
                float azr = azDeg * ((float)M_PI / 180.0f);
                // [user 2026-08-20] "shadows are looking gigantic again". The length is
                // 1 + len/tan(elevation), and tan collapses as the sun drops: at the 0.17rad
                // (9.7deg) floor this reaches 9.5x the model radius, which on a low-sun map is a
                // shadow the size of a truck. Clamp the RESULT rather than the elevation, so a
                // low sun still gives a long shadow but never an absurd one, and fade it as it
                // stretches - a real grazing shadow is long AND faint, not long and black.
                float stretch = 1.0f + sLen->value / tan(elr < 0.17f ? 0.17f : elr);
                {
                    static cvar_t *sMax = NULL;
                    float          cap;
                    if (!sMax) {
                        sMax = cgi.Cvar_Get("coop_shadowStretchMax", "3.0", CVAR_ARCHIVE);
                    }
                    cap = sMax->value > 1.0f ? sMax->value : 1.0f;
                    if (stretch > cap) {
                        alpha  *= cap / stretch; // longer than we allow => proportionally fainter
                        stretch = cap;
                    }
                }
                vec3_t sunH, pos;
                sunH[0] = (float)cos(azr);
                sunH[1] = (float)sin(azr);
                sunH[2] = 0.0f;
                // trail the shadow centre away from the sun so it reads as cast behind the model
                VectorMA(trace.endpos, -w * (stretch - 1.0f) * 0.5f, sunH, pos);
                CG_ImpactMark(
                    cgs.media.shadowMarkShader, pos, trace.plane.normal,
                    azDeg, w * stretch, w, alpha, alpha, alpha, 1,
                    qfalse, qtrue, qfalse, qfalse, 0.5f, 0.5f);
            }
            return qtrue;
        }
    }

    if (cg_shadows->integer == 2 && (model->renderfx & RF_SHADOW_PRECISE)) {
        iTagL = cgi.Tag_NumForName(model->tiki, "Bip01 L Foot");
        if (iTagL != -1) {
            iTagR = cgi.Tag_NumForName(model->tiki, "Bip01 R Foot");
        }

        if (iTagR != -1) {
            int    iNumLights, iCurrLight;
            vec3_t avLightPos[16], avLightIntensity[16];

            iNumLights = Q_clamp(cg_shadowscount->integer, 1, 8);
            iNumLights = cgi.R_GatherLightSources(model->origin, avLightPos, avLightIntensity, iNumLights);
            if (iNumLights) {
                for (iCurrLight = 0; iCurrLight < iNumLights; iCurrLight++) {
                    CG_CastFootShadow(avLightPos[iCurrLight], avLightIntensity[iCurrLight], iTagL, model);
                    CG_CastFootShadow(avLightPos[iCurrLight], avLightIntensity[iCurrLight], iTagR, model);
                }

                // shadow was casted properly
                return qtrue;
            }
        }
    }

    // send a trace down from the player to the ground
    VectorCopy(model->origin, end);
    end[2] -= SHADOW_DISTANCE;

    cgi.CM_BoxTrace(&trace, model->origin, end, vec3_origin, vec3_origin, 0, MASK_PLAYERSOLID, qfalse);

    // no shadow if too high
    if (trace.fraction == 1.0) {
        return qfalse;
    }

    // since 2.0: no shadow if solid
    if (trace.startsolid || trace.allsolid) {
        return qfalse;
    }

    if ((cg_shadows->integer == 3) && (model->renderfx & RF_SHADOW_PRECISE)) {
        return qtrue;
    }

    //
    // get the bounds of the current frame
    //
    fWidth = model->scale * cgi.R_ModelRadius(model->hModel);
    if (fWidth < 1) {
        return qfalse;
    }

    // fade the shadow out with height
    alpha = (1.0 - trace.fraction) * 0.65f;

    if (model->renderfx & RF_SHADOW_PRECISE) {
        iTagL = cgi.Tag_NumForName(model->tiki, "Bip01 L Foot");
        if (iTagL != -1) {
            iTagR = cgi.Tag_NumForName(model->tiki, "Bip01 R Foot");
        }

        if (iTagR != -1) {
            if (cg_shadows->integer == 2) {
                alpha *= 0.6f;
            }

            CG_CastSimpleFeetShadow(&trace, fWidth, alpha, iTagR, iTagL, model->tiki, model);
            return qtrue;
        }
    }

    cgi.R_ModelBounds(model->hModel, vMins, vMaxs);
    VectorSubtract(vMaxs, vMins, vSize);
    VectorScale(vSize, 0.6f, vSize);

    // add the mark as a temporary, so it goes directly to the renderer
    // without taking a spot in the cg_marks array
    CG_ImpactMark(
        cgs.media.shadowMarkShader,
        trace.endpos,
        trace.plane.normal,
        cent->lerpAngles[YAW],
        vSize[1],
        vSize[0],
        alpha,
        alpha,
        alpha,
        1,
        qfalse,
        qtrue,
        qfalse,
        qfalse,
        0.5f,
        0.5f
    );

    return qtrue;
}

//
//
// NEW ANIMATION AND THREE PART MODEL SYSTEM
//
//

//=================
//CG_AnimationDebugMessage
//=================
void CG_AnimationDebugMessage(int number, const char *fmt, ...)
{
#ifndef NDEBUG
    if (cg_debugAnim->integer) {
        va_list argptr;
        char    msg[1024];

        va_start(argptr, fmt);
        Q_vsnprintf(msg, sizeof(msg), fmt, argptr);
        va_end(argptr);

        if ((!cg_debugAnimWatch->integer) || ((cg_debugAnimWatch->integer - 1) == number)) {
            if (cg_debugAnim->integer == 2) {
                cgi.DebugPrintf(msg);
            } else {
                cgi.Printf(msg);
            }
        }
    }
#endif
}

/*
======================
CG_AttachEntity

Modifies the entities position and axis by the given
tag location
======================
*/
void CG_AttachEntity(
    refEntity_t *entity, refEntity_t *parent, dtiki_t *tiki, int tagnum, qboolean use_angles, vec3_t attach_offset
)
{
    int i;
    orientation_t or ;
    vec3_t tempAxis[3];
    vec3_t vOrigin;
    vec3_t vDeltaLightOrg;

    or = cgi.TIKI_Orientation(parent, tagnum);
    //cgi.Printf( "th = %d %.2f %.2f %.2f\n", tikihandle, or.origin[ 0 ], or.origin[ 1 ], or.origin[ 2 ] );

    VectorSubtract(entity->lightingOrigin, entity->origin, vDeltaLightOrg);
    VectorCopy(parent->origin, entity->origin);

    for (i = 0; i < 3; i++) {
        VectorMA(entity->origin, or.origin[i], parent->axis[i], entity->origin);
    }

    if (attach_offset[0] || attach_offset[1] || attach_offset[2]) {
        MatrixMultiply(or.axis, parent->axis, tempAxis);

        for (i = 0; i < 3; i++) {
            VectorMA(entity->origin, attach_offset[i], tempAxis[i], entity->origin);
        }
    }

    VectorCopy(entity->origin, entity->oldorigin);

    if (use_angles) {
        MatrixMultiply(entity->axis, or.axis, tempAxis);
        MatrixMultiply(tempAxis, parent->axis, entity->axis);
    }

    entity->scale *= parent->scale;
    entity->renderfx |= (parent->renderfx & ~(RF_FLAGS_NOT_INHERITED | RF_LIGHTING_ORIGIN));

    MatrixTransformVectorRight(entity->axis, vDeltaLightOrg, vOrigin);
    VectorAdd(entity->origin, vOrigin, entity->lightingOrigin);
}

/*
===============
CG_AttachEyeEntity
===============
*/
void CG_AttachEyeEntity(
    refEntity_t *entity, refEntity_t *parent, dtiki_t *tiki, int tagnum, qboolean use_angles, vec_t *attach_offset
)
{
    int i;

    VectorCopy(cg.refdef.vieworg, entity->origin);

    if (use_angles) {
        AnglesToAxis(cg.refdefViewAngles, entity->axis);
    }

    if (attach_offset[0] || attach_offset[1] || attach_offset[2]) {
        for (i = 0; i < 3; i++) {
            VectorMA(entity->origin, attach_offset[i], entity->axis[i], entity->origin);
        }
    }

    VectorCopy(entity->origin, entity->oldorigin);
    entity->scale *= parent->scale;
    entity->renderfx |= (parent->renderfx & ~(RF_FLAGS_NOT_INHERITED | RF_LIGHTING_ORIGIN));
    VectorCopy(parent->lightingOrigin, entity->lightingOrigin);
}

/*
===============
CG_IsValidServerModel
===============
*/
qboolean CG_IsValidServerModel(const char *modelpath)
{
    const char *str;
    int         i;

    for (i = 1; i < MAX_MODELS; i++) {
        str = CG_ConfigString(CS_MODELS + i);
        if (!Q_stricmp(str, modelpath)) {
            return qtrue;
        }
    }

    return qfalse;
}

/*
===============
CG_CheckValidModels

This verifies the allied player model and the german player model:
- If they don't exist on the client, reset to the default allied player model
- If they don't exist on the server, don't allow forceModel so the client explicitly know the skin isn't supported
===============
*/
void CG_CheckValidModels()
{
    const char *modelpath;
    qboolean    isDirty = qfalse;

    if (dm_playermodel->modified) {
        // Check for allied model
        modelpath = va("models/player/%s.tik", dm_playermodel->string);
        if (!cgi.R_RegisterModel(modelpath)) {
            cgi.Printf(
                "Allied model '%s' is invalid, resetting to '%s'\n", dm_playermodel->string, dm_playermodel->resetString
            );

            cgi.Cvar_Set("dm_playermodel", dm_playermodel->resetString);
            modelpath = va("models/player/%s.tik", dm_playermodel->string);
        }

        cg.serverAlliedModelValid = CG_IsValidServerModel(modelpath);
    }

    if (dm_playergermanmodel->modified) {
        // Check for axis model
        modelpath = va("models/player/%s.tik", dm_playergermanmodel->string);
        if (!cgi.R_RegisterModel(modelpath)) {
            cgi.Printf(
                "Allied model '%s' is invalid, resetting to '%s'\n",
                dm_playergermanmodel->string,
                dm_playergermanmodel->resetString
            );

            cgi.Cvar_Set("dm_playergermanmodel", dm_playergermanmodel->resetString);
            modelpath = va("models/player/%s.tik", dm_playergermanmodel->string);
        }

        cg.serverAxisModelValid = CG_IsValidServerModel(modelpath);
    }

    if (dm_playermodel->modified || dm_playergermanmodel->modified) {
        cg_forceModelAllowed = cg.serverAlliedModelValid && cg.serverAxisModelValid;
    }
}

/*
===============
CG_ServerModelLoaded
===============
*/
void CG_ServerModelLoaded(const char *name, qhandle_t handle)
{
    if (!Q_stricmpn(name, "models/player/", 14) && (!cg.serverAlliedModelValid || !cg.serverAxisModelValid)) {
        char modelName[MAX_QPATH];
        COM_StripExtension(name + 14, modelName, sizeof(modelName));

        //
        // The player model has been loaded on the server
        // so try again parsing
        //
        if (!Q_stricmp(modelName, dm_playermodel->string)) {
            dm_playermodel->modified = qtrue;
        }
        if (!Q_stricmp(modelName, dm_playergermanmodel->string)) {
            dm_playergermanmodel->modified = qtrue;
        }
    }
}

/*
===============
CG_ServerModelUnloaded
===============
*/
void CG_ServerModelUnloaded(qhandle_t handle)
{
#if 0
    if (cg.serverAlliedModelValid && handle == cg.hAlliedPlayerModelHandle) {
        dm_playermodel->modified = qtrue;
    }
    if (cg.serverAxisModelValid && handle == cg.hAxisPlayerModelHandle) {
        dm_playergermanmodel->modified = qtrue;
    }
#endif
}

/*
===============
CG_UpdateForceModels
===============
*/
void CG_UpdateForceModels()
{
    qhandle_t hModel;
    char     *pszAlliesPartial;
    char     *pszAxisPartial;
    char      szAlliesModel[256];
    char      szAxisModel[256];
    qboolean  isDirty;

    isDirty = dm_playermodel->modified || dm_playergermanmodel->modified || cg_forceModel->modified;

    if (!cg_forceModelAllowed) {
        if (isDirty) {
            cgi.Printf(
                "One or more of the selected players model don't exist on the server or are not loaded, using the "
                "default skin\n"
            );
        }

        return;
    }

    if (cg.pAlliedPlayerModel && cg.pAxisPlayerModel && !isDirty) {
        return;
    }

    pszAlliesPartial = dm_playermodel->string;
    pszAxisPartial   = dm_playergermanmodel->string;

    Com_sprintf(szAlliesModel, sizeof(szAlliesModel), "models/player/%s.tik", pszAlliesPartial);
    Com_sprintf(szAxisModel, sizeof(szAxisModel), "models/player/%s.tik", pszAxisPartial);

    hModel = cg.serverAlliedModelValid ? cgi.R_RegisterModel(szAlliesModel) : 0;
    if (!hModel) {
        Com_sprintf(szAlliesModel, sizeof(szAlliesModel), "models/player/%s.tik", dm_playermodel->resetString);
        hModel = cgi.R_RegisterModel(szAlliesModel);
    }

    if (hModel) {
        cg.hAlliedPlayerModelHandle = hModel;
        cg.pAlliedPlayerModel       = cgi.R_Model_GetHandle(hModel);
        if (!cg.pAlliedPlayerModel) {
            cg.hAlliedPlayerModelHandle = 0;
        }
    } else {
        cg.hAlliedPlayerModelHandle = 0;
        cg.pAlliedPlayerModel       = NULL;
    }

    hModel = cg.serverAxisModelValid ? cgi.R_RegisterModel(szAxisModel) : 0;
    if (!hModel) {
        Com_sprintf(szAxisModel, sizeof(szAxisModel), "models/player/%s.tik", dm_playergermanmodel->resetString);
        hModel = cgi.R_RegisterModel(szAxisModel);
    }

    if (hModel) {
        cg.hAxisPlayerModelHandle = hModel;
        cg.pAxisPlayerModel       = cgi.R_Model_GetHandle(hModel);
        if (!cg.pAxisPlayerModel) {
            cg.hAxisPlayerModelHandle = 0;
        }
    } else {
        cg.hAxisPlayerModelHandle = 0;
        cg.pAxisPlayerModel       = 0;
    }

    // Clear modified flag
    //dm_playermodel->modified       = qfalse;
    //dm_playergermanmodel->modified = qfalse;
}

/*
===============
CG_ProcessPlayerModel

Checks player models, and update force models
===============
*/
void CG_ProcessPlayerModel()
{
    CG_CheckValidModels();
    if (cg_forceModel->integer) {
        CG_UpdateForceModels();
    }

    // Clear modified flag
    dm_playermodel->modified       = qfalse;
    dm_playergermanmodel->modified = qfalse;
    cg_forceModel->modified        = qfalse;
}

// HZM coop - BAKED per-gun ADS iron-sight tune, dialled in-game with the tuning workbench (numpad pad) and
// captured via adssave. name = weapon configstring (CS_WEAPONS). Standing pitch/yaw/roll + screen shift x/y
// are absolute; crouch values are EXTRA, added on top of the standing values while ducked. Guns NOT listed
// fall back to the global cg_ads* cvars. cg_modelanim applies the rotations; cg_view applies the shift.
static const adsGunTune_t s_adsGunTune[] = {
    //  name                     sP     sY     sR    sSx    sSy      cP     cY    cR    cSx    cSy
    { "Colt 45", -2.5f, -2.0f,  1.5f, -0.02f, -0.02f,   1.5f, -8.5f,  4.0f,  -0.14f,  0.04f },
    { "Walther P38", -1.5f, -0.5f,  1.5f,   0.0f, -0.02f,   0.5f, -9.5f,  1.0f, -0.145f,  0.02f },
    { "Webley Revolver",        2.5f, -1.5f,  0.0f, -0.02f, 0.12f,   0.5f,-10.0f, 1.0f,-0.16f, 0.08f },
    { "Nagant Revolver",        2.5f, -1.5f,  0.0f, -0.02f, 0.08f,   0.5f,-10.0f, 1.0f,-0.16f, 0.02f },
    { "Beretta",                0.0f, -1.0f, -2.0f,  0.005f, 0.075f, -1.0f, -8.5f, 1.0f,-0.14f, 0.02f },
    { "Hi-Standard Silenced",   0.0f, -1.0f, -2.0f, -0.02f, 0.02f,   1.0f, -8.0f, 1.0f,-0.12f, 0.02f },
    { "M1 Garand", -7.5f, -1.0f, -1.0f, -0.02f, -0.22f,   3.5f,-38.5f,  3.5f,  -0.75f,  0.08f },
    { "Mauser KAR 98K", -7.5f, -1.0f, -1.0f, -0.02f, -0.22f,   4.5f,-34.5f,  6.5f, -0.625f,  0.08f },
    { "Lee-Enfield", -7.0f, -1.0f,  1.0f, -0.02f, -0.26f,   2.0f,-34.5f,  0.0f, -0.625f, 0.045f },
    { "Mosin Nagant Rifle", -6.5f, -1.5f,  0.5f, -0.02f, -0.24f,   4.0f,-33.5f,  3.0f,  -0.61f, 0.105f },
    { "Carcano", -6.0f, -1.0f,  0.5f, -0.02f, -0.22f,   4.0f,-41.0f,  0.5f, -0.805f, 0.095f },
    { "DeLisle",                1.0f, -3.0f,  2.0f, -0.04f, 0.02f,   4.0f,-34.0f, 3.0f,-0.56f, 0.14f },
    { "Thompson",  1.0f,  2.0f,  0.0f,  0.02f,   0.0f,   1.0f,-10.5f,  1.5f, -0.165f,  0.02f },
    { "MP40",  0.5f,  2.0f, -1.5f,  0.02f,   0.0f,   2.5f,-20.0f,  1.5f, -0.335f,  0.05f },
    { "Sten Mark II",           1.0f,  1.5f, -1.5f,  0.02f,-0.04f,   1.0f,-20.0f, 1.5f,-0.28f, 0.02f },
    { "PPSH SMG", -2.5f, 11.0f, -2.0f,  0.14f, -0.08f,   1.5f,-18.0f,  2.5f,  -0.26f,  0.04f },
    { "Moschetto", -8.0f,  6.0f, -2.0f,  0.115f,-0.325f,   2.0f,-18.5f,  1.5f, -0.285f,-0.005f },
    { "BAR",  1.0f, -0.5f,  0.0f,   0.0f,  0.04f,   2.0f,-23.0f,  1.5f,  -0.38f, 0.155f },
    { "StG 44",  0.0f,  2.5f,  0.0f,  0.04f,  0.02f,   2.5f,-20.5f,  2.5f, -0.325f,  0.06f },
    { "Vickers-Berthier",  7.0f,-11.5f, -1.0f, -0.12f,  0.28f,   1.5f,-26.5f,  1.5f, -0.505f,  0.18f },
    { "Bazooka", 11.0f,  7.5f, -1.0f,  0.20f,  0.16f,   1.5f, -7.0f,  3.5f,  -0.06f, 0.055f },
    { "Panzerschreck",         11.0f,  7.5f, -1.0f,  0.08f, 0.30f,   1.5f, -7.0f, 1.0f,-0.06f, 0.04f },
    { "PIAT",                 -12.5f, 21.0f,  2.0f,  0.24f,-0.32f,   1.5f, -7.0f, 1.0f,-0.10f, 0.02f },
    { "shotgun",-12.5f, 12.0f,  2.0f,  0.12f, -0.42f,  -3.0f,-43.0f,  1.0f, -0.715f, -0.02f },
    // [user 07-18] grease guns: dialled on the SILENCED variant; regular M3 copied to match (same gun).
    { "Silenced Grease Gun",    3.0f,  0.0f,  0.0f,   0.0f, 0.095f,   1.0f,-10.5f,  1.5f, -0.165f,  0.02f },
    { "M3 Grease Gun",          3.0f,  0.0f,  0.0f,   0.0f, 0.095f,   1.0f,-10.5f,  1.5f, -0.165f,  0.02f },
    // [user 07-18 session 2] STANDING-ONLY tune pass (pistols + silenced variants, rifles, SMGs). The STAND
    // fields are dialled; the CROUCH fields on these NEW rows are the inherited default state the adssave
    // printed (crouch was NOT deliberately tuned this session) - fine as a starting point, refine later.
    // [user 2026-08-18] crouch re-synced to each gun's correct FAMILY donor ("i know i missed some for
    // crouch ads"): silenced pistols match their own base gun, PPS-43 the PPSH, Beretta M38 the Moschetto.
    // Still family guesses, not eyeballed - the standing fields remain the hand-dialled truth.
    { "Silenced Colt .45",      0.0f, -1.0f,  1.5f, -0.015f,-0.005f,   1.5f, -8.5f,  4.0f, -0.14f,  0.04f },
    { "Silenced Walther P38",  -2.0f,  0.0f,  1.5f, -0.005f,-0.025f,   0.5f, -9.5f,  1.0f, -0.145f,  0.02f },
    { "Silenced TT-33",        -2.0f, -1.0f,  1.5f, -0.01f, -0.015f,    1.0f, -8.0f,  1.0f, -0.12f,  0.02f },
    { "Silenced Beretta",      -1.0f, -1.5f,  1.5f, -0.005f, 0.01f, -1.0f, -8.5f, 1.0f,-0.14f, 0.02f },
    { "Silenced Luger P08",    -1.5f, -1.5f,  1.5f, -0.01f, -0.025f,   1.0f, -8.0f,  1.0f, -0.12f,  0.02f },
    { "Walther PPK",           -3.5f, -0.5f,  3.0f, -0.01f, -0.11f,    1.0f, -8.0f,  1.0f, -0.12f,  0.02f },
    { "Luger P08",              0.0f, -1.5f,  0.0f, -0.01f, -0.035f,   1.0f, -8.0f,  1.0f, -0.12f,  0.02f },
    { "Nambu Type 14",          1.5f, -1.5f,  0.0f, -0.015f, 0.015f,   1.0f, -8.0f,  1.0f, -0.12f,  0.02f },
    { "TT-33 Tokarev",         -1.0f, -0.5f,  0.0f, -0.015f,-0.01f,    1.0f, -8.0f,  1.0f, -0.12f,  0.02f },
    { "Welrod",                -1.0f, -2.0f,  0.0f, -0.015f,-0.01f,    1.0f, -8.0f,  1.0f, -0.12f,  0.02f },
    { "M1 Carbine",           -10.5f, -0.5f, -1.0f, -0.015f,-0.385f,   4.5f,-34.5f,  6.5f, -0.625f, 0.08f },
    { "Arisaka Type 99",       -7.0f, -1.0f,  1.0f, -0.015f,-0.29f,    2.0f,-34.5f,  0.0f, -0.625f, 0.045f },
    { "Springfield M1903",    -18.0f,  0.5f,  1.0f,  0.01f, -0.73f,    2.0f,-34.5f,  0.0f, -0.625f, 0.045f },
    { "Thompson 50rd",          4.5f,  2.0f,  0.0f,  0.02f,  0.08f,    1.0f,-10.5f,  1.5f, -0.165f, 0.02f },
    { "Silenced MP40",          4.5f,  4.0f,  0.0f,  0.045f, 0.095f,   2.5f,-20.0f,  1.5f, -0.335f,  0.05f },
    { "Type 100 SMG",          18.0f, -2.0f,  0.0f,  0.0f,   0.325f,   1.0f,-10.5f,  1.5f, -0.165f, 0.02f },
    { "Silenced PPS-43",       -2.0f,  4.0f,  0.0f,  0.065f, 0.005f,   1.5f,-18.0f,  2.5f,  -0.26f,  0.04f },
    { "Beretta M38",           -8.0f,  6.5f, -1.5f,  0.125f,-0.335f,   2.0f,-18.5f,  1.5f, -0.285f,-0.005f },
    { "Breda",                  1.5f,  2.0f, -3.0f,  0.045f, 0.035f,   2.0f,-23.0f,  1.5f, -0.38f,  0.155f },
};

/*
====================
CoopStripSkinSuffix

HZM coop [user 2026-08-17]. A skin variant is named "<Base Gun> (<Finish>)" - "Thompson (Gold)".
Strips the trailing parenthesised part so a cosmetic variant resolves to its base gun. Returns
qtrue only when something was actually stripped.
====================
*/
qboolean CoopStripSkinSuffix(const char *in, char *out, int outSize)
{
    const char *paren;
    int         len;

    if (!in || !*in || !out || outSize <= 0) {
        return qfalse;
    }
    paren = strstr(in, " (");
    if (!paren) {
        return qfalse;
    }
    len = (int)(paren - in);
    if (len <= 0 || len >= outSize) {
        return qfalse;
    }
    memcpy(out, in, len);
    out[len] = 0;
    return qtrue;
}

// [user 2026-08-18] "I wish there was a simpler way to get these new guns ads done and accurate
// without having to manually do it." DONOR ALIASES: a new gun that is mechanically the same
// family as a hand-dialled one borrows that gun's tune by name - one line here instead of a
// numpad session. Exact rows and the "(Finish)" strip still win, so any alias can be replaced
// by a real dialled row later without touching this list. Scoped guns are absent on purpose:
// their ADS is the scope overlay, not iron sights. Audit tool: docs/tools/ads_audit.py lists
// every shipped weapon name that resolves to no row.
static const char *const s_adsDonor[][2] = {
    {"DP-28",         "Vickers-Berthier"}, // top-magazine LMG, offset irons like the VB
    {"FG 42",         "StG 44"          }, // shoulder-fired automatic rifle
    {"G 43",          "M1 Garand"       }, // semi-auto battle rifle
    {"SVT 40",        "M1 Garand"       }, // semi-auto battle rifle
    {"Johnson M1941", "M1 Garand"       }, // semi-auto battle rifle
    {"Gewehrgranate", "Mauser KAR 98K"  }, // kar98 body with a launcher cup
    {"Mauser C96",    "Luger P08"       }, // German pistol
    {"S&W M10 .38",   "Webley Revolver" }, // top-break-style revolver sight picture
};

static float s_fAdsPose = 0.0f; // eased ADS pose factor (0 = hip, 1 = full sight alignment)

// HZM coop [user 2026-09-05] QUICK-DRAW: THE PARKED PRIMARY, PLACED IN VIEW SPACE.
//
// "with the quick swap to pistol, the thompson appears to be put away totally."
//
// It was never put away and it was never hidden. Player::CoopQDrawPose (player.cpp) calls
// Weapon::CoopQDrawPark, which attaches - it does not hideModel unless coop_qdrawShowPrimary is 0;
// the guard below only ZEROES s_fAdsPose; and the whitelist directly above admits tag_weapon_left
// to the first-person attach. The gun was drawn every single frame. It was drawn UNDER THE FLOOR OF
// THE FRUSTUM.
//
// In first person this branch resolves the tag on <skin>_fps.tik, not on the world model. EVERY
// player fps tik in the mod uses one rig - models/player/US_Army/USarmyplyr.skd, 103 of 103 - and
// on that rig tag_weapon_left is a child of "Bip01 L Hand", whose pose comes from whichever
// VIEWMODEL clip is playing. During a quick draw that is always a PISTOL clip, because
// CoopQDrawEnter ends in ViewModelAnim("pullout") and the client resolves the prefix from
// activeItems[1] == the sidearm. Measured by forward kinematics over the retail skc set, offsets
// from the "eyes bone" in game units (tik scale 0.52), and the screen fraction of the half-frame at
// cg_fov 80 on 4:3 where |x|,|y| <= 1 means on screen:
//
//   viewmodel/pistol/coltpose          fwd  +0.63  left +14.99  up -38.19    x=-28.5  y=-96.7
//   viewmodel/pistol/pullout_colt      fwd  +0.63  left +14.99  up -38.19    x=-28.5  y=-96.7
//   viewmodel/pistol/fire_colt   f2    fwd  -2.11                            BEHIND THE EYE
//   viewmodel/pistol/walk_colt         fwd  -0.25                            BEHIND THE EYE
//   viewmodel/pistol/crouch_coltpose   fwd  -0.25                            BEHIND THE EYE
//   viewmodel/pistol/crouch_colt_fire  fwd -35.60                            BEHIND THE EYE
//   viewmodel/pistol/run_colt          fwd  +3.82  left +20.59  up -38.26    x= -6.4  y=-15.9
//
// - i.e. the parked rifle sat at the character's left hip, ~38 units below the eye and level with
// or behind the near plane, in every stance the draw can be taken from. coop_qdrawParkOfs "-6 5 -4"
// then pushed it a further 6 units BACKWARD along that tag's own forward axis.
//
// THE RETAIL REFERENCE DOES NOT TRANSFER. `weaponcommand mainhand attachtohand offhand` at frame 1
// of every rifle reload really is visible in first person - but only because the RIFLE viewmodel
// clips author tag_weapon_left ON TOP OF tag_weapon_right. Measured, same method:
// viewmodel/rifle/idle_rifle and viewmodel/rifle/vm_riflereload have the two tags IDENTICAL to
// 0.01 units at every keyframe. That is a coincident-tag trick inside the rifle clips, not a
// general "the left tag is on screen" facility, and the quick draw does not play rifle clips.
//
// So the first-person placement is taken away from the skeleton entirely and expressed against the
// VIEW, exactly the way CG_AttachEyeEntity (:1029) already does for the "eyes bone" - cg.refdef and
// cg.refdefViewAngles are final by the time entities are added (CG_CalcViewValues runs to
// completion, CG_CalcFov included, before CG_AddPacketEntities; cg_view.c:4051 relies on the same
// ordering). coop_qdrawVOfs is (forward, left, up) in units FROM THE EYE; coop_qdrawVAng is
// (pitch, yaw, roll) composed in the view's own frame, not Euler-added to the view angles.
//
// WHY 20 12 -9. Half-frame at cg_fov 80 / 4:3 is tan(40) = 0.839 horizontally and 0.629 vertically
// per unit of depth, so at 20 units out the half-width is 16.78 and the half-height is 12.59:
//     x = -12 / 16.78 = -0.72      y = -9 / 12.59 = -0.72
// The grip lands ~14% across and ~86% down - the lower-left corner, comfortably inside the frame -
// while the measured PISTOL grip sits at x=+0.44 y=-0.56. Two guns, opposite sides of the frame,
// both on screen, which is the acceptance test. 20 rather than 18 so a butt-stock ~14 units behind
// the grip still clears r_znear. On 16:9 the engine widens the horizontal fov, which only moves the
// parked gun further INSIDE the frame; the vertical is unchanged.
//
// THIRD PERSON IS UNTOUCHED. This is the else arm of
// `s1->parent != cg.snap->ps.clientNum || bThirdPerson` (:1999), so a teammate still sees the rifle
// on the WORLD model's tag_weapon_left with the server-side coop_qdrawParkOfs/Ang pose, which is
// correct there - the world skeleton's left hand is where the left hand actually is.
//
// IDENTIFIED BY ENTITY NUMBER, NOT BY TAG. coop_qdrawOn now carries the parked weapon's entnum + 1
// (Player::TickCoopSidearm), because the left tag is shared: the 14 left-tag reload magazine props
// (bug-2241) ride the same tag on the same parent, and re-placing one of those into the corner of
// the frame would be a new bug of exactly the shape this one is.
static qboolean CG_CoopQDrawIsParked(const entityState_t *s1)
{
    static cvar_t *pOn = NULL;

    if (!pOn) {
        pOn = cgi.Cvar_Get("coop_qdrawOn", "0", 0);
    }
    return (pOn->integer > 0 && s1->number == pOn->integer - 1) ? qtrue : qfalse;
}

// [user 2026-09-06] "off to the left but completely upside down ... it needs to practically look like the
// gun is being moved over to the left, out of the way, with the left hand, and it needs to be smooth."
//
// TWO THINGS THE FIRST CUT GOT WRONG, both visible in the user's screenshot. (1) The weapon mesh is not
// authored with its barrel on +X and its sights on +Z the way the pose assumed: parked at (10 -15 -25)
// it drew with the sights underneath - a half-turn about its own X (v2 read it as Y and reversed the
// muzzle; the second screenshot corrected that). The correction is a model-space half-turn whose axis
// coop_qdrawHoldFlip selects (2 = about X: negate the posed Y and Z rows, a proper rotation),
// so the pose cvars below say where the MUZZLE points and which way the SIGHTS face, in the view frame
// (pitch, yaw, roll; negative pitch = muzzle up, positive yaw = left), and read the way a person writes
// them. The defaults were derived from direction vectors and round-tripped through this same math by
// the fixer that wrote them, not typed from feel. (2) It snapped into place. The pose now eases from a
// FROM pose - low and centred, where the gun sat in both hands - to the HOLD pose over coop_qdrawHoldMs
// with a smoothstep, keyed on the parked entity number appearing or changing: no server change and no
// new state on the wire. On release the primary goes back into the hands as the viewmodel, drawn by
// the pullout clip, so there is nothing to ease out.
//
// NOT ARCHIVED, AND RENAMED. The first cut's coop_qdrawVOfs/VAng were CVAR_ARCHIVE, so the user's config
// now carries "10 -15 -25"; Cvar_Get keeps an existing value, and a changed code default under the old
// name would never have run. New names, no archive flag: what is written here is what draws.
static void CG_CoopQDrawParkInView(refEntity_t *ent, int iEntNum)
{
    static cvar_t *pOfs = NULL, *pAng = NULL, *pFromOfs = NULL, *pFromAng = NULL;
    static cvar_t *pMs = NULL, *pFlip = NULL, *pDbg = NULL;
    static int     iNext    = 0;
    static int     iLastEnt = -1, iLastTime = -1, iStart = 0;
    vec3_t         vOfs, vAng, vFromOfs, vFromAng, vO, vA;
    vec3_t         vView[3], vPose[3], vLocal[3];
    float          f, e;
    int            i;

    if (!pOfs) {
        pOfs     = cgi.Cvar_Get("coop_qdrawHoldOfs",     "16 10 -7",  0);
        // [user 2026-09-08, bug-2542] "gun is facing almost straight upwards". PITCH -37 -> 14.
        // AnglesToAxis puts axis[0][2] = -sin(pitch) (q_math.c:775-777), so a pitch of -37 aims the
        // BARREL 37 degrees UP. Screen-flat for this pose offset is pitch +14.09 - the grip is parked
        // forward and left of the eye, so a barrel angled slightly down projects as horizontal - which
        // makes the shipped value 51 degrees past flat and 65 degrees from horizontal on screen.
        // coop_qdrawHoldFlip cannot be the cause: flip 2 negates only rows 1 and 2 of the pose basis
        // (below), so it provably cannot alter barrel elevation.
        // This is a LIVE cvar, so it is dialable without a rebuild: coop_qdrawHoldAng "<pitch> 55 17".
        pAng     = cgi.Cvar_Get("coop_qdrawHoldAng",     "14 55 17",  0);
        pFromOfs = cgi.Cvar_Get("coop_qdrawHoldFromOfs", "14 2 -13",  0);
        pFromAng = cgi.Cvar_Get("coop_qdrawHoldFromAng", "18 3 0",  0);
        pMs      = cgi.Cvar_Get("coop_qdrawHoldMs",      "320",       0);
        pFlip    = cgi.Cvar_Get("coop_qdrawHoldFlip",    "2",         0);
        pDbg     = cgi.Cvar_Get("coop_qdrawVDbg",        "0",         0);
    }

    // A malformed cvar must not silently park the gun at the eye - that is the same invisible
    // failure this whole block exists to end. Fall back to the shipped pose instead.
    if (sscanf(pOfs->string, "%f %f %f", &vOfs[0], &vOfs[1], &vOfs[2]) != 3) {
        VectorSet(vOfs, 16.0f, 10.0f, -7.0f);
    }
    if (sscanf(pAng->string, "%f %f %f", &vAng[0], &vAng[1], &vAng[2]) != 3) {
        // [bug-2542] must match the Cvar_Get default above, or a malformed cvar silently restores the
        // 37-degrees-muzzle-up pose this fix exists to end.
        VectorSet(vAng, 14.0f, 55.0f, 17.0f);
    }
    if (sscanf(pFromOfs->string, "%f %f %f", &vFromOfs[0], &vFromOfs[1], &vFromOfs[2]) != 3) {
        VectorSet(vFromOfs, 14.0f, 2.0f, -13.0f);
    }
    if (sscanf(pFromAng->string, "%f %f %f", &vFromAng[0], &vFromAng[1], &vFromAng[2]) != 3) {
        VectorSet(vFromAng, 18.0f, 3.0f, 0.0f);
    }

    // A fresh park: a different entity, or this one not drawn parked within the last quarter second
    // (released and drawn again). Everything else is a continuing ease.
    if (iEntNum != iLastEnt || cg.time - iLastTime > 250) {
        iStart = cg.time;
    }
    iLastEnt  = iEntNum;
    iLastTime = cg.time;
    f = (pMs->value > 1.0f) ? (float)(cg.time - iStart) / pMs->value : 1.0f;
    if (f < 0.0f) { f = 0.0f; }
    if (f > 1.0f) { f = 1.0f; }
    e = f * f * (3.0f - 2.0f * f);
    for (i = 0; i < 3; i++) {
        vO[i] = vFromOfs[i] + (vOfs[i] - vFromOfs[i]) * e;
        vA[i] = LerpAngle(vFromAng[i], vAng[i], e);
    }

    AnglesToAxis(cg.refdefViewAngles, vView);

    VectorCopy(cg.refdef.vieworg, ent->origin);
    for (i = 0; i < 3; i++) {
        VectorMA(ent->origin, vO[i], vView[i], ent->origin);
    }
    VectorCopy(ent->origin, ent->oldorigin);
    VectorCopy(ent->origin, ent->lightingOrigin);

    // Composed, not Euler-added: adding roll to a pitched view rolls about the world axis and the
    // gun swings out of frame the moment you look up or down. The flip is the model-space half-turn
    // described above, applied to the posed frame (negating two rows keeps the determinant +1).
    AnglesToAxis(vA, vPose);
    // [user 2026-09-06, second screenshot] The v2 half-turn about Y sent the MUZZLE the wrong way: it drew
    // pointing down-right at the shooter with the stock up-left, i.e. the barrel had been on +X all along
    // and only the sights were inverted. The correction is therefore about X (keep X, negate Y and Z),
    // and the axis is now selectable so the next wrong guess costs an rcon line, not a build:
    // coop_qdrawHoldFlip 0 none, 1 about Y, 2 about X, 3 about Z. Each negates two rows: still a rotation.
    VectorCopy(vPose[0], vLocal[0]);
    VectorCopy(vPose[1], vLocal[1]);
    VectorCopy(vPose[2], vLocal[2]);
    switch (pFlip->integer) {
    case 1:
        VectorNegate(vPose[0], vLocal[0]);
        VectorNegate(vPose[2], vLocal[2]);
        break;
    case 2:
        VectorNegate(vPose[1], vLocal[1]);
        VectorNegate(vPose[2], vLocal[2]);
        break;
    case 3:
        VectorNegate(vPose[0], vLocal[0]);
        VectorNegate(vPose[1], vLocal[1]);
        break;
    default:
        break;
    }
    MatrixMultiply(vLocal, vView, ent->axis);

    // THE TUNING PROBE. coop_qdrawVDbg 1 prints the pose and the screen fraction the arithmetic above
    // predicts for the grip point, twice a second, so a tuning pass is a number rather than a squint.
    if (pDbg->integer && cg.time >= iNext) {
        static cvar_t *pWFov = NULL;
        float          fx = 9.99f, fy = 9.99f;
        float          fFovX = cg.refdef.fov_x, fFovY = cg.refdef.fov_y;

        iNext = cg.time + 500;
        // The parked gun inherits RF_DEPTHHACK and is drawn with the WEAPON projection whenever ADS has
        // zoomed the world fov (tr_main.c: r_weaponfovx), so the prediction has to use that fov or it
        // reads OFF-SCREEN for a gun that is on screen while the pistol is aimed.
        if (!pWFov) {
            pWFov = cgi.Cvar_Get("r_weaponfovx", "0", 0);
        }
        if (pWFov->value > 1.0f && fabs(pWFov->value - fFovX) > 0.05f && cg.refdef.width > 0) {
            fFovX = pWFov->value;
            fFovY = 2.0f * RAD2DEG(atan(tan(DEG2RAD(fFovX * 0.5f)) * (float)cg.refdef.height / (float)cg.refdef.width));
        }
        if (vO[0] > 0.01f && fFovX > 1.0f && fFovY > 1.0f) {
            fx = -vO[1] / (vO[0] * tan(DEG2RAD(fFovX * 0.5f)));
            fy = vO[2] / (vO[0] * tan(DEG2RAD(fFovY * 0.5f)));
        }
        cgi.Printf(
            "^~^~^ QDRAWVIS ent=%d ease=%.2f ofs=(%.1f %.1f %.1f) ang=(%.1f %.1f %.1f) flip=%d fov=(%.1f %.1f) screen=(%+.2f %+.2f) %s\n",
            iEntNum, e,
            vO[0], vO[1], vO[2], vA[0], vA[1], vA[2], pFlip->integer,
            fFovX, fFovY,
            fx, fy,
            (fx > -1.0f && fx < 1.0f && fy > -1.0f && fy < 1.0f) ? "IN-FRAME" : "OFF-SCREEN"
        );
    }
}

static const adsGunTune_t *CG_AdsTuneExact(const char *wpn)
{
    int i;

    for (i = 0; i < (int)(sizeof(s_adsGunTune) / sizeof(s_adsGunTune[0])); i++) {
        if (!Q_stricmp(wpn, s_adsGunTune[i].name)) {
            return &s_adsGunTune[i];
        }
    }
    return NULL;
}

const adsGunTune_t *CG_FindAdsTune(const char *wpn)
{
    int                 i;
    char                base[64];
    const char         *name;
    const adsGunTune_t *t;

    if (!wpn || !*wpn) {
        return NULL;
    }
    t = CG_AdsTuneExact(wpn);
    if (t) {
        return t;
    }
    //
    // [user 2026-08-17] No exact hit - fall back to the base gun. Without this a skin variant
    // silently loses every hand-dialled sight value in the table above, because the lookup is an
    // exact Q_stricmp and "Thompson (Gold)" is not "Thompson". Exact still wins, so a variant CAN
    // be given its own tuning later simply by adding a row for it.
    //
    name = wpn;
    if (CoopStripSkinSuffix(wpn, base, sizeof(base))) {
        name = base;
        t    = CG_AdsTuneExact(name);
        if (t) {
            return t;
        }
    }
    // [user 2026-08-18] donor stage: runs on the finish-stripped base name, so
    // "FG 42 (Gold)" -> "FG 42" -> StG 44's dialled values.
    for (i = 0; i < (int)(sizeof(s_adsDonor) / sizeof(s_adsDonor[0])); i++) {
        if (!Q_stricmp(name, s_adsDonor[i][0])) {
            return CG_AdsTuneExact(s_adsDonor[i][1]);
        }
    }
    return NULL;
}

/*
===============
CG_ModelAnim
===============
*/
/*
=================================================================================================
HZM coop [user 2026-08-21] PROCEDURAL FINGER LIFE.

"make the right hand fingers sometimes move to grip stronger, rest off the trigger and just
animate them in general and make them not seem so static all of the time. randomize it."

They are not just static-LOOKING - they are literally static. Measuring every finger channel across
197 base-game viewmodel animations found idle_rifle and fire_rifle_stand at 0.00000 variance on all
30 finger channels: the grip is a frozen baked pose, and even firing only moves the wrist. So there
is no animation here to fight, and nothing to author over.

HOW IT WORKS. refEntity_t::bone_tag / bone_quat are per-bone override slots, applied client-side
through cgi.ForceUpdatePose -> skeletor_c::SetPose. The blend is ADDITIVE - the animation evaluates
first and the controller quaternion post-multiplies (skeletorbones.cpp) - and it PROPAGATES TO
CHILDREN, so one controller on Bip01 R Finger1 curls that whole finger. Rotation is about the bone's
own origin, in MODEL space (same convention as the stock head/torso controllers).

WHY THIS IS SAFE FOR THE ADS WORK. tag_weapon_right is a SIBLING of the fingers - both hang off
Bip01 R Hand - not a descendant. Finger rotation therefore cannot move the weapon and cannot disturb
the per-gun sight alignment in s_adsGunTune. Server hitboxes are untouched too: TIKI_GetSkeletor
caches per (entnum, tiki) and the FPS tiki differs from the world tiki, so the viewmodel has its own
skeletor. (Note: _research/ragdoll_r13_spec.md bans bone_quat writes because a SHARED skeletor would
deflect SV_TraceDeep hitboxes. That reasoning is about world entities; this is viewmodel-only, so it
is a deliberate documented exception rather than an oversight.)

SLOTS ARE SCARCE. NUM_BONE_CONTROLLERS is 5, hardcoded, and raising it means editing the exe and
breaking the protocol. HEAD_TAG/TORSO_TAG/ARMS_TAG are 0/1/2, and ARMS_TAG carries view pitch into
the viewmodel - clobbering it would break arm pitch. So this NEVER takes a slot the engine is
already using: it copies the incoming array and fills only entries whose bone_tag is < 0, in
priority order. If only two are free, the trigger finger and the grip still get them.

INDEX SPACE TRAP. Bone indices must be resolved against the FPS TIKI, not reused from
s1->bone_tag - those were computed on the world tiki, a merged multi-skd model with a completely
different bone table.

garandhand (Garand / Springfield / KAR98 / KAR98 Sniper) is a rigid mesh with NO finger bones, so on
those four the left hand cannot move. This only drives the RIGHT hand, which always can.
=================================================================================================
*/
// HZM coop [user 2026-08-22] viewmodel-only controller budget - see CoopFingerLife.
#define COOP_VM_BONE_CONTROLLERS 8
#define COOP_FINGER_SLOTS 6

static void CoopFingerLife(refEntity_t *pModel)
{
    static cvar_t *pOn = NULL, *pAmt = NULL, *pAxis = NULL, *pRest = NULL;
    static int     s_iTag[COOP_FINGER_SLOTS];
    static qboolean s_bTags = qfalse;
    static int     s_iTiki  = 0;
    static int     s_iHeadTag = -1;   // the one slot we may borrow while inert
    // HZM coop [user 2026-08-22] COOP_VM_BONE_CONTROLLERS, not NUM_BONE_CONTROLLERS. The
    // networked 5 is a WIRE size (entityState_t sizes three arrays by it, two of them
    // hand-enumerated netfields), so raising THAT is a protocol change costing every entity
    // forever to benefit one local model. The VIEWMODEL is not networked: refEntity_t carries
    // POINTERS, and the skeletor stores controllers per-bone with no structural limit - the
    // old cap was a hardcoded 5 in its read loop, now the caller's count.
    static vec3_t  s_vAng[COOP_VM_BONE_CONTROLLERS];
    static vec4_t  s_qOut[COOP_VM_BONE_CONTROLLERS];
    static int     s_iTagOut[COOP_VM_BONE_CONTROLLERS];
    static float   s_fPhase   = 0.0f;   // integrated, never time*frequency
    static float   s_fGrip    = 0.0f;   // eased 0..1 grip event magnitude
    static float   s_fGripDir = 1.0f;   // +1 = squeeze tighter, -1 = loosen / stretch out
    static int     s_iGripAt  = 0;      // when the next squeeze fires
    static int     s_iLast    = 0;
    static unsigned s_seed    = 2463534242u;
    static float   s_fFade    = 0.0f;   // master, fades out where the anim owns the fingers

    // INTERLEAVED BY HAND. Controller slots are scarce (5 total, ARMS_TAG already holds one), and
    // this fills only the ones the engine left free - so the order decides what survives when there
    // are just two. Trigger finger first because it is the one the eye tracks, then the left index
    // so BOTH hands get life before either gets a second finger.
    //
    // [user 2026-08-21] "Can we do anything with the left hands fingers?" - yes: lefthand (374 verts)
    // is weighted to all 15 left finger bones. No detection needed for the four rifles that swap in
    // garandhand instead, because that mesh has NO finger weights at all - writing the controller is
    // simply a no-op there rather than something that needs gating.
    const char *kNames[COOP_FINGER_SLOTS] = {
        "Bip01 R Finger1",  // right index / trigger - the one that reads
        "Bip01 L Finger1",  // left index            - support-hand life
        "Bip01 R Finger2",  // right middle          - grip
        "Bip01 L Finger2",  // left middle           - grip
        "Bip01 R Finger0",  // right thumb           - slow drift
        "Bip01 L Finger0"   // left thumb            - slow drift, out of phase
    };
    float fDt, fAmt, fTrig, fWantFade;
    int   i, iSlot, iAnim;

    if (!pOn)   { pOn   = cgi.Cvar_Get("coop_fingerLife", "1", CVAR_ARCHIVE); }
    if (!pAmt)  { pAmt  = cgi.Cvar_Get("coop_fingerAmount", "1.0", CVAR_ARCHIVE); }
    if (!pRest) { pRest = cgi.Cvar_Get("coop_fingerTrigRest", "3.5", CVAR_ARCHIVE); }
    // [user 2026-08-21] "fingeraxis2 looks best" - ROLL is the curl axis on this rig. Confirmed by
    // eye, not derived: which model-space axis curls a finger is a property of how the skeleton was
    // authored and there is no way to know it without looking.
    if (!pAxis) { pAxis = cgi.Cvar_Get("coop_fingerAxis", "2", CVAR_ARCHIVE); }

    if (pOn->integer <= 0 || !pModel->tiki || !cg.snap) {
        return;
    }

    // resolve bone indices ONCE per model - and re-resolve if the tiki changed
    if (!s_bTags || s_iTiki != (int)(size_t)pModel->tiki) {
        for (i = 0; i < COOP_FINGER_SLOTS; i++) {
            s_iTag[i] = cgi.Tag_NumForName(pModel->tiki, (char *)kNames[i]);
        }
        s_iHeadTag = cgi.Tag_NumForName(pModel->tiki, "Bip01 Head");
        s_iTiki = (int)(size_t)pModel->tiki;
        s_bTags = qtrue;
    }

    // ---- timing ---------------------------------------------------------------------------
    fDt = (cg.time - s_iLast) / 1000.0f;
    if (s_iLast == 0 || fDt < 0.0f || fDt > 0.25f) {
        fDt = 0.0f;                      // first frame, or a hitch / 3P gap: advance nothing
    }
    s_iLast = cg.time;

    // A reload, pullout or putaway DOES animate the fingers (36 of 36 reload anims do). Fade the
    // override out there so it cannot fight authored motion, and back in when idle owns them again.
    iAnim     = cg.snap->ps.iViewModelAnim;
    fWantFade = (iAnim == VM_ANIM_RELOAD || iAnim == VM_ANIM_RELOAD_SINGLE
                 || iAnim == VM_ANIM_RELOAD_END || iAnim == VM_ANIM_PULLOUT
                 || iAnim == VM_ANIM_PUTAWAY || iAnim == VM_ANIM_RECHAMBER)
                    ? 0.0f : 1.0f;
    s_fFade += (fWantFade - s_fFade) * (fDt * 6.0f > 1.0f ? 1.0f : fDt * 6.0f);

    // integrated phase for the idle drift - never cg.time * frequency (bug-1983/1984/1985)
    s_fPhase += fDt * 1.35f;
    if (s_fPhase > 62831.85f) { s_fPhase -= 62831.85f; }

    // ---- randomised grip re-settle -----------------------------------------------------------
    // Fires every 4-11s, ramps in fast and relaxes slowly, so it reads as adjusting a hold rather
    // than as a pulse. The interval is re-rolled each time, so it never settles into a visible loop.
    if (s_iGripAt == 0) {
        s_iGripAt = cg.time + 3000;
    }
    if (cg.time >= s_iGripAt) {
        s_seed ^= s_seed << 13;
        s_seed ^= s_seed >> 17;
        s_seed ^= s_seed << 5;
        s_iGripAt = cg.time + 4000 + (int)(s_seed % 7000u);
        s_fGrip   = 1.0f;
        // [user 2026-08-21] "sorta squeezing the grip that the hand is holding or
        // loosening/stretching fingers occassionally makes sense" - so the event has a DIRECTION,
        // re-rolled each time. Squeeze is the common case; a stretch is the occasional shake-out.
        // Biased 2:1 toward squeezing, because a hand that keeps splaying open reads as nervous
        // rather than as adjusting a hold.
        s_fGripDir = ((s_seed >> 11) % 3u) ? 1.0f : -1.0f;
    }
    if (s_fGrip > 0.0f) {
        s_fGrip -= fDt * (s_fGripDir > 0.0f ? 1.7f : 1.15f);   // squeeze ~600ms, stretch ~870ms
        if (s_fGrip < 0.0f) { s_fGrip = 0.0f; }
    }

    // ---- trigger discipline ------------------------------------------------------------------
    // At rest the index finger lies OFF the trigger (extended); it curls on as the weapon comes up,
    // driven by the same eased ADS pose factor the sight alignment uses, so finger and gun are one
    // motion rather than two. Firing adds a short extra squeeze.
    fTrig = 1.0f - CG_AdsPoseFactor();    // 1 = resting off the trigger, 0 = on it
    if (iAnim == VM_ANIM_FIRE || iAnim == VM_ANIM_FIRE_SECONDARY) {
        fTrig = -0.35f;                   // past neutral: pulled through
    }

    fAmt = pAmt->value * s_fFade;
    if (fAmt <= 0.001f) {
        return;                           // nothing to add - leave the incoming controllers alone
    }

    // ---- build the output arrays -------------------------------------------------------------
    // Copy what the engine already set, then fill ONLY free entries. This is what keeps ARMS_TAG
    // (view pitch into the viewmodel) intact.
    for (i = 0; i < COOP_VM_BONE_CONTROLLERS; i++) {
        // only the first NUM_BONE_CONTROLLERS can hold anything the engine set up; the extra
        // entries start empty and exist purely for the fingers to claim
        s_iTagOut[i] = (pModel->bone_tag && i < NUM_BONE_CONTROLLERS) ? pModel->bone_tag[i] : -1;
        if (pModel->bone_quat && i < NUM_BONE_CONTROLLERS) {
            s_qOut[i][0] = pModel->bone_quat[i][0];
            s_qOut[i][1] = pModel->bone_quat[i][1];
            s_qOut[i][2] = pModel->bone_quat[i][2];
            s_qOut[i][3] = pModel->bone_quat[i][3];
        } else {
            s_qOut[i][0] = 0.0f; s_qOut[i][1] = 0.0f; s_qOut[i][2] = 0.0f; s_qOut[i][3] = 1.0f;
        }
    }

    iSlot = 0;
    for (i = 0; i < COOP_FINGER_SLOTS; i++) {
        float fDeg;
        int   iAxis;

        if (s_iTag[i] < 0) {
            continue;                     // this rig has no such bone
        }
        // [user 2026-08-21] BORROWING AN INERT SLOT, so the left hand can move at all.
        //
        // A live probe (ADSSLOT) showed all four other controllers hold REAL bones on the FPS rig -
        // Head, Spine2, Spine1 and Pelvis - not the meaningless world-model leftovers I had assumed,
        // so taking one blindly would break a working controller. But two facts narrow it:
        //   * a controller whose quaternion is IDENTITY is applying no rotation - it is doing nothing
        //   * Bip01 Head has NO geometry under it on the first-person model, which draws arms,
        //     sleeves, hands and the weapon; there is no head to mis-rotate even if it were used
        // So borrow the HEAD slot only while it is inert, and yield it back the instant it is not.
        // Spine1 (arm pitch) and Pelvis (skeleton root - rotating it would move everything) are
        // never touched regardless of what their quats say.
        while (iSlot < COOP_VM_BONE_CONTROLLERS) {
            qboolean bFree = (s_iTagOut[iSlot] < 0) ? qtrue : qfalse;

            if (!bFree && s_iHeadTag >= 0 && s_iTagOut[iSlot] == s_iHeadTag) {
                float qw = s_qOut[iSlot][3];
                float qx = s_qOut[iSlot][0], qy = s_qOut[iSlot][1], qz = s_qOut[iSlot][2];

                if (qx > -0.001f && qx < 0.001f && qy > -0.001f && qy < 0.001f
                    && qz > -0.001f && qz < 0.001f && (qw > 0.999f || qw < -0.999f)) {
                    bFree = qtrue;        // head controller is inert this frame - safe to borrow
                }
            }
            if (bFree) {
                break;
            }
            iSlot++;
        }
        if (iSlot >= COOP_VM_BONE_CONTROLLERS) {
            break;                        // out of slots - the remaining fingers simply stay still
        }

        switch (i) {
        case 0:  // RIGHT index / trigger
            // [user 2026-08-21] "might need to even have it further inside the trigger area versus
            // coming out cause it looks just a tad weird with a pistol." The first pass swung 9
            // degrees OUT at rest, which reads as pointing away from the weapon rather than resting
            // inside the guard. coop_fingerTrigRest is that resting angle, and it is deliberately
            // small - NEGATIVE values curl further in, past neutral, if you want it tucked.
            fDeg = fTrig * pRest->value + s_fGrip * 2.0f;
            break;
        case 1:  // LEFT index - the support hand is where a stretch reads naturally
            fDeg = s_fGrip * s_fGripDir * 5.5f + (float)sin(s_fPhase + 0.8f) * 1.0f;
            break;
        case 2:  // right middle - grip squeeze plus a little drift
            fDeg = s_fGrip * 6.0f + (float)sin(s_fPhase) * 0.9f;
            break;
        case 3:  // left middle - trails the left index slightly so the hand rolls through the
                 // gesture finger by finger instead of clenching as one block
            fDeg = s_fGrip * s_fGripDir * 6.0f + (float)sin(s_fPhase - 0.9f) * 0.8f;
            break;
        case 4:  // right thumb - slow, out of phase so a hand never moves as one block
            fDeg = (float)sin(s_fPhase * 0.63f + 1.9f) * 1.4f + s_fGrip * 2.5f;
            break;
        default: // left thumb - follows the hand, at about a third the travel
            fDeg = (float)sin(s_fPhase * 0.55f + 3.4f) * 1.3f + s_fGrip * s_fGripDir * 2.2f;
            break;
        }
        fDeg *= fAmt;

        // Which model-space axis curls a finger is a property of how the rig was built, so it is
        // tunable rather than guessed: 0 = pitch, 1 = yaw, 2 = roll. If fingers splay sideways
        // instead of curling, change coop_fingerAxis.
        iAxis = pAxis->integer;
        if (iAxis < 0 || iAxis > 2) { iAxis = 0; }

        s_vAng[iSlot][0] = 0.0f;
        s_vAng[iSlot][1] = 0.0f;
        s_vAng[iSlot][2] = 0.0f;
        s_vAng[iSlot][iAxis] = fDeg;

        EulerToQuat(s_vAng[iSlot], s_qOut[iSlot]);
        s_iTagOut[iSlot] = s_iTag[i];
        iSlot++;
    }

    pModel->bone_tag   = s_iTagOut;
    pModel->bone_quat  = s_qOut;
    pModel->bone_count = COOP_VM_BONE_CONTROLLERS;
}

int g_iCoopSurfMask = 0;   // HZM coop surface probe: 2 bits per surface (exists, hidden)

void CG_ModelAnim(centity_t *cent, qboolean bDoShaderTime)
{
    entityState_t *s1;
    entityState_t *sNext = NULL;
    refEntity_t    model;
    int            i;
    vec3_t         vMins, vMaxs, vTmp;
    const char    *szTagName;
    int            iAnimFlags;
    qboolean       bThirdPerson = qfalse;
    qboolean       bCoopHideDraw = qfalse; // HZM coop - process commands/sounds but do not render [219]

    s1 = &cent->currentState;

    // HZM coop - STAGED ADS: first person is forced only when the ADS system asks for it (instant for
    // first-person players / cg_adsShoulder 0; for third-person players the over-the-shoulder aim stage
    // KEEPS the body drawn, and the wheel-up handoff flips this in LOCKSTEP with cg.renderingThirdPerson
    // in cg_view.c - both sides must use CG_AdsForceFirstPerson or you get the camera-in-body bug).
    bThirdPerson |= (cg_3rd_person->integer && !CG_AdsForceFirstPerson()) ? qtrue : qfalse;
    // HZM coop (bug-1234) - the bug-1217 DBNO LOCKSTEP THAT USED TO SIT HERE IS REVERTED.
    // It forced bThirdPerson = qfalse for the whole downed state on the theory that the body
    // draw had to match cg_view.c's camera force. That reasoning was sound in the abstract and
    // WRONG for this project: the user could go third person while downed, deliberately, and
    // relied on it - so the 'fix' removed a working feature to prevent a problem nobody had.
    // If a camera-in-head artefact ever does appear while downed, fix it on the CAMERA side in
    // cg_view.c, where the player still keeps the choice, not by force-hiding their body here.
    // HZM coop - IN COVER auto-3P: draw the own body whenever the cover view force is active
    // (lockstep with cg_view.c renderingThirdPerson - turret-camera-regression rule 2)
    // [user 2026-08-20] cover must not outrank the ADS first-person handoff - lockstep with the
    // matching change in cg_view.c (turret-camera-regression rule 2: if the camera goes first
    // person and the body draw does not, the camera sits inside the drawn head).
    if ((cg.snap->ps.pm_flags & PMF_COOP_COVER) && !CG_AdsForceFirstPerson()) { bThirdPerson = qtrue; }
    // Fixed in OPM
    //  Draw world model body when in camera
    bThirdPerson |= (cg.snap->ps.pm_flags & PMF_CAMERA_VIEW && !(cg.snap->ps.pm_flags & PMF_TURRET));
    // HZM coop [241] - NATIVE ZOOM lockstep: the camera side (cg_view.c) forces FIRST person while
    // scoped (STAT_INZOOM, turrets exempt) - the body draw must match or the 1P camera sits inside
    // the still-drawn head ("sniper scope looks at the back of the player's head"). This is exactly
    // the lockstep rule from the comment above, applied to the zoom term.
    if (cg.snap->ps.stats[STAT_INZOOM] && !(cg.snap->ps.pm_flags & PMF_TURRET)) {
        bThirdPerson = qfalse;
    }
    // HZM coop - REMOVED the 3rd-person MG42 experiment's `bThirdPerson |= PMF_TURRET` line. It force-drew
    // your own body in 3rd person on EVERY turret (MG42 nest, jeep .30cal, halftrack), overriding the
    // upstream line above (which deliberately excludes turrets so the mounted view stays clean first-person).
    // The cg_view.c half of that experiment was reverted but this half was missed = the "camera stuck in my
    // body" regression. Restored to stock: turrets are first person, own body not drawn.

    /* HZM coop [user 2026-08-22, bug-2049] THE LOCKSTEP PROBE, and it is deliberately placed
       OUTSIDE every bThirdPerson branch. Three probes today were blind because they sat INSIDE
       the condition they needed to observe - SKIP-DRAW was gated on !bThirdPerson, the wall-cover
       probe registered its cvar only inside the wallValid branch. A probe belongs outside the
       branch it measures.
       WHAT THIS TESTS. The whole first-person viewmodel branch is wrapped in `if (!bThirdPerson)`
       (~line 2303), so when this flag flips true the gun AND both hands vanish together and the
       world body is drawn instead - which from a first-person camera is nothing where the gun was.
       That is the reported symptom exactly, and no existing probe can see it.
       The flag is decided here from cg.snap->ps.pm_flags (line ~1720). The CAMERA decides the same
       thing at cg_view.c:4820 from cg.predicted_player_state.pm_flags. Both sites carry comments
       insisting they are in lockstep; they read DIFFERENT SOURCES. cg.snap advances at snapshot
       rate, predicted_player_state every render frame. On any frame they disagree about
       PMF_COOP_COVER the camera stays first person while the viewmodel is not drawn.
       PMF_COOP_COVER started toggling far more often today, because wall cover was re-enabled -
       which is why the flicker started when it did. Recording both sources settles it. */
    {
        static int  s_iLastTP  = -1;
        static int  s_iLastSnap = -1;
        static int  s_iLastPred = -1;
        static cvar_t *s_pTrace = NULL;

        if (s1->number == cg.snap->ps.clientNum) {
            int iSnapCover = (cg.snap->ps.pm_flags & PMF_COOP_COVER) ? 1 : 0;
            int iPredCover = (cg.predicted_player_state.pm_flags & PMF_COOP_COVER) ? 1 : 0;

            if (!s_pTrace) { s_pTrace = cgi.Cvar_Get("coop_gunVisTrace", "0", CVAR_ARCHIVE); }
            if (s_pTrace->integer
                && (s_iLastTP != (int)bThirdPerson || s_iLastSnap != iSnapCover
                    || s_iLastPred != iPredCover)) {
                s_iLastTP   = (int)bThirdPerson;
                s_iLastSnap = iSnapCover;
                s_iLastPred = iPredCover;
                cgi.Printf("^~^~^ GUNVIS t=%d LOCKSTEP 3p=%d snapCover=%d predCover=%d "
                           "agree=%d adsFP=%d snapFlags=0x%x predFlags=0x%x\n",
                           cg.time, (int)bThirdPerson, iSnapCover, iPredCover,
                           (iSnapCover == iPredCover) ? 1 : 0,
                           CG_AdsForceFirstPerson() ? 1 : 0,
                           cg.snap->ps.pm_flags, cg.predicted_player_state.pm_flags);
            }
        }
    }

    if ((cg.snap->ps.pm_flags & PMF_INTERMISSION) && s1->number == cg.snap->ps.clientNum && !bThirdPerson) {
        // Don't render the first-person model during intermission
        return;
    }

    memset(&model, 0, sizeof(model));

    if (cent->interpolate) {
        sNext = &cent->nextState;
    }

    // add loop sound only if it is not attached, and never another player's private 2D one -
    // see CG_LoopSoundIsForeignLocal (cg_ents.c). This is the site the Omaha heartbeat runs
    // through: the body is hidden for the whole beat, and that is fine, because the RF_DONTDRAW
    // gate is further down at the R_AddRefEntityToScene call - a hidden entity still sounds.
    if (s1->loopSound && (s1->parent == ENTITYNUM_NONE) && !CG_LoopSoundIsForeignLocal(s1)) {
        cgi.S_AddLoopingSound(
            cent->lerpOrigin,
            vec3_origin,
            cgs.sound_precache[s1->loopSound],
            s1->loopSoundVolume,
            s1->loopSoundMinDist,
            s1->loopSoundMaxDist,
            s1->loopSoundPitch,
            s1->loopSoundFlags
        );
    }

    if (cent->tikiLoopSound && (s1->parent == ENTITYNUM_NONE)) {
        cgi.S_AddLoopingSound(
            cent->lerpOrigin,
            vec3_origin,
            cent->tikiLoopSound,
            cent->tikiLoopSoundVolume,
            cent->tikiLoopSoundMinDist,
            cent->tikiLoopSoundMaxDist,
            cent->tikiLoopSoundPitch,
            cent->tikiLoopSoundFlags
        );
    }

    if (s1->renderfx & RF_SKYORIGIN) {
        AnglesToAxis(cent->lerpAngles, cg.sky_axis);
        VectorCopy(cent->lerpOrigin, cg.sky_origin);
    }

    // if set to invisible, skip
    if (!s1->modelindex) {
        return;
    }

    // set the entity number
    model.entityNumber = s1->number;

    // take the results of CL_InterpolateEntities
    VectorCopy(cent->lerpOrigin, model.origin);
    VectorCopy(cent->lerpOrigin, model.oldorigin);

    IntegerToBoundingBox(s1->solid, vMins, vMaxs);
    // calculate the light origin
    VectorAdd(vMins, vMaxs, vTmp);
    VectorMA(model.origin, 0.5, vTmp, model.lightingOrigin);
    // calculate the radius
    VectorSubtract(vMins, vMaxs, vTmp);
    model.radius = VectorLength(vTmp) * 0.5;

    if (s1->number == cg.snap->ps.clientNum) {
        if (!bThirdPerson) {
            PmoveAdjustAngleSettings_Client(
                cg.refdefViewAngles, cent->lerpAngles, &cg.predicted_player_state, &cent->currentState
            );
        }

        model.bone_quat = s1->bone_quat;
        model.bone_tag  = s1->bone_tag;
    } else {
        for (i = 0; i < NUM_BONE_CONTROLLERS; i++) {
            if (s1->bone_tag[i] >= 0) {
                if ((cent->interpolate) && (cent->nextState.bone_tag[i] == s1->bone_tag[i])) {
                    SlerpQuaternion(
                        s1->bone_quat[i], cent->nextState.bone_quat[i], cg.frameInterpolation, cent->bone_quat[i]
                    );
                } else {
                    cent->bone_quat[i][0] = s1->bone_quat[i][0];
                    cent->bone_quat[i][1] = s1->bone_quat[i][1];
                    cent->bone_quat[i][2] = s1->bone_quat[i][2];
                    cent->bone_quat[i][3] = s1->bone_quat[i][3];
                }
            }
        }

        model.bone_quat = cent->bone_quat;
        model.bone_tag  = s1->bone_tag;
    }

    // convert angles to axis
    AnglesToAxis(cent->lerpAngles, model.axis);

    // copy shader specific data
    if (s1->shader_data[0]) {
        model.shader_data[0] = s1->shader_data[0];
    } else {
        model.shader_data[0] = s1->tag_num;
    }

    if (s1->shader_data[1]) {
        model.shader_data[1] = s1->shader_data[1];
    } else {
        model.shader_data[1] = s1->skinNum;
    }

    if (bDoShaderTime) {
        if (cent->interpolate) {
            model.shaderTime =
                s1->shader_time + (sNext->shader_time - s1->shader_time) * cg.frameInterpolation + cg.time / 1000.0;
        } else {
            model.shaderTime = cg.time / 1000.0 + s1->shader_time;
        }
    }

    // Interpolated state variables
    if (cent->interpolate) {
        model.scale = s1->scale + cg.frameInterpolation * (cent->nextState.scale - s1->scale);
    } else {
        model.scale = s1->scale;
    }

    model.hOldModel = 0;
    model.tiki      = cgi.R_Model_GetHandle(cgs.model_draw[s1->modelindex]);

    if (s1->number != cg.snap->ps.clientNum && (s1->eType == ET_PLAYER || (s1->eFlags & EF_DEAD))) {
        if (cg_forceModel->integer && cg_forceModelAllowed) {
            //CG_UpdateForceModels();

            if (s1->eFlags & EF_AXIS) {
                model.hModel = cg.hAxisPlayerModelHandle;
                model.tiki   = cg.pAxisPlayerModel;
            } else {
                model.hModel = cg.hAlliedPlayerModelHandle;
                model.tiki   = cg.pAlliedPlayerModel;
            }

            if (model.hModel && model.tiki) {
                model.hOldModel = cgs.model_draw[s1->modelindex];
            } else {
                // fallback to non-forced model
                model.tiki   = cgi.R_Model_GetHandle(cgs.model_draw[s1->modelindex]);
                model.hModel = cgs.model_draw[s1->modelindex];
            }
        } else {
            model.hModel = cgs.model_draw[s1->modelindex];
        }

        if (!model.hModel || !model.tiki) {
            // Use a model in case it still doesn't exist
            if (s1->eFlags & EF_AXIS) {
                model.hModel = cgi.R_RegisterModel(CG_GetPlayerModelTiki(dm_playergermanmodel->resetString));
            } else {
                model.hModel = cgi.R_RegisterModel(CG_GetPlayerModelTiki(dm_playermodel->resetString));
            }
            model.tiki      = cgi.R_Model_GetHandle(model.hModel);
            model.hOldModel = cgs.model_draw[s1->modelindex];
        }
    } else {
        model.hModel = cgs.model_draw[s1->modelindex];
    }

    if (!model.tiki) {
        // still no model
        return;
    }

    // set skin
    model.skinNum = s1->skinNum;
    model.renderfx |= s1->renderfx;
    cgi.TIKI_SetEyeTargetPos(model.tiki, model.entityNumber, s1->eyeVector);

    CG_InterpolateAnimParms(s1, sNext, &model);

    if (cent->currentState.parent != ENTITYNUM_NONE) {
        int          iTagNum;
        refEntity_t *parent;
        dtiki_t     *tiki;

        parent = cgi.R_GetRenderEntity(cent->currentState.parent);
        if (!parent) {
            if (developer->integer > 1) {
                cgi.DPrintf("CG_ModelAnim: Could not find parent entity\n");
            }

            return;
        }

        // HZM coop - a mounted turret's FIRST-PERSON overlay gun (<model>_viewmodel.tik,
        // attached to the local player's "eyes bone" by TurretGun::P_CreateViewModel) must
        // never DRAW in third person - attached to the visible body it renders skewered
        // through the head (jeep .30cal report). DRAW-only skip: the early-return version
        // also killed the viewmodel's client frame commands = the owner's FIRE SOUND went
        // silent in 3P (bug-315). First person keeps it (it IS the gun view).
        if (s1->parent == cg.snap->ps.clientNum && bThirdPerson && model.tiki) {
            const char *szTikiName = cgi.TIKI_Name(model.tiki);
            if (szTikiName && strstr(szTikiName, "_viewmodel")) {
                bCoopHideDraw = qtrue;
            }
        }

        if (s1->parent != cg.snap->ps.clientNum || bThirdPerson) {
            // attach the model to the world model

            // Fixed in OPM
            //  Added checks to make sure the old model's tiki is valid
            if (parent->hOldModel && (tiki = cgi.R_Model_GetHandle(parent->hOldModel))) {
                szTagName = cgi.Tag_NameForNum(tiki, s1->tag_num & TAG_MASK);
                // Fixed in OPM
                //  Added checks to make sure the tag name is valid
                if (szTagName) {
                    tiki    = cgi.R_Model_GetHandle(parent->hModel);
                    iTagNum = cgi.Tag_NumForName(tiki, szTagName);
                } else {
                    iTagNum = 0;
                }
            } else {
                tiki    = cgi.R_Model_GetHandle(parent->hModel);
                iTagNum = s1->tag_num;
            }

            CG_AttachEntity(&model, parent, tiki, iTagNum & TAG_MASK, s1->attach_use_angles, s1->attach_offset);
        } else {
            tiki = cg.pPlayerFPSModel;

            // attach to the first person model
            if (cg.pLastPlayerWorldModel) {
                szTagName = cgi.Tag_NameForNum(cg.pLastPlayerWorldModel, s1->tag_num & TAG_MASK);
            } else {
                szTagName = cgi.Tag_NameForNum(tiki, s1->tag_num & TAG_MASK);
            }

            if (!Q_stricmp(szTagName, "eyes bone")) {
                iTagNum = cgi.Tag_NumForName(tiki, szTagName);
                CG_AttachEyeEntity(&model, parent, tiki, iTagNum & TAG_MASK, s1->attach_use_angles, s1->attach_offset);
            } else if (!Q_stricmp(szTagName, "tag_weapon_right") || !Q_stricmp(szTagName, "tag_weapon_left")) {
                iTagNum = cgi.Tag_NumForName(tiki, szTagName);
                CG_AttachEntity(&model, parent, tiki, iTagNum & TAG_MASK, s1->attach_use_angles, s1->attach_offset);

                // HZM coop: ADS iron-sight aim. A screen shift (r_weaponshift) moves the whole gun
                // uniformly, so it cannot line the REAR aperture up with the FRONT post; rotating the
                // weapon about tag_weapon (near the grip) tilts/angles the barrel so both sights fall on
                // the aim line. cg_adsPitch (up/down) + cg_adsYaw (left/right), live-tunable, ADS only.
                // [user 2026-08-20] "if the animation could be smoother coming out of ads for all
                // guns that would be ideal". The per-gun sight rotation used to be a hard on/off -
                // full alignment the instant ADS engaged, gone the instant it released - so the gun
                // SNAPPED at both ends regardless of how smoothly the zoom eased. Ease a 0..1 pose
                // factor and scale every angle by it: at 1 the sight picture is identical to before,
                // at 0 the block does not run at all, and in between the gun rotates into and out of
                // alignment. Out is deliberately gentler than in - coming down off the sights is a
                // relax, not a snap.
                // [user 2026-08-20] the ease now lives in cg_view.c (CG_AdsFactorAdvance) and is
                // advanced unconditionally once per frame. It used to be advanced HERE, inside the
                // first-person weapon-tag branch - which does not run in third person, in cover, on
                // a cutscene camera, or while dead - so the pose froze and was then re-applied at
                // full strength on the first frame this branch ran again.
                // HZM coop [user 2026-09-04] QUICK-DRAW SIDEARM. Everything from here to the close
                // of this block sits inside the single `tag_weapon_right || tag_weapon_left` arm
                // above with NO per-tag check, and takes its tune from activeItems[1] - the block's
                // own comment says "model.origin was set from tag_weapon_right". It was written when
                // exactly one weapon could ever reach it. With a long gun parked on tag_weapon_left,
                // aiming the PISTOL would swing the parked rifle by the PISTOL's sight rotation (the
                // pistol rows of s_adsGunTune[] carry -8.0 to -10.0 degrees of crouch yaw: Colt 45
                // -8.5, Walther P38 -9.5, Webley -10.0, Nagant -10.0, Beretta -8.5, Hi-Standard
                // -8.0) on a lever arm the length of a Garand. The -34.5/-38.5/-43.0 figures are the
                // RIFLE and shotgun rows, and they belong to the other half of this argument below -
                // do not attribute them to the pistol.
                //
                // GATED ON THE FEATURE BEING ACTIVE, NOT ON THE TAG - AND THAT IS THE WHOLE POINT.
                // "the tag is not tag_weapon_right" is NOT a quick-draw signal and never was:
                //   retail runs `weaponcommand mainhand attachtohand offhand` at frame 1 of
                //     rifle_reload, kar98_reload, springfield_reload_start and all four
                //     kar98/springfield *_rechamber clips, which puts THE PLAYER'S OWN RIFLE on
                //     tag_weapon_left for 1.5-3.3 s of every reload and every bolt cycle; and
                //   the reload MAGAZINE is a separate networked Animate attached by a frame command
                //     (bug-2241; 22 such props, entity.cpp CoopIsMagazineProp), drawn in first
                //     person by this very branch. THE COUNT IS 22, THE PLACEMENT IS NOT UNIFORM -
                //     measured across every models/player/base/anims_*.txt and human_*.tik in the
                //     retail paks: FOURTEEN attach to tag_weapon_left (bar, colt, g43,
                //     it_w_beretta, it_w_moschetto, mp40, mp44, p38, ppsh, silencedpistol, sten,
                //     svt, thompson, uk_w_l42a1), SEVEN to tag_weapon_RIGHT (enfield_clip1,
                //     it_w_breda, it_w_carcano, kar98_clip_reload, nagant_pistol_shell,
                //     springfield_clip_reload, uk_w_vickers) and ONE - enfield_clip2 - to the bone
                //     "Bip01 R Hand" rather than to a weapon tag at all. So a tag-keyed guard would
                //     miss the seven right-tag clips entirely while still stripping the fourteen.
                // CG_AimingDownSights (cg_view.c) keys purely on the ADS button plus the brace mount,
                // with no reload or rechamber test, so "during ADS" is not a narrow window. A
                // tag-keyed guard would therefore delete up to ~38.5 degrees of authored per-gun
                // sight rotation from the player's own rifle through every reload and every bolt
                // cycle taken with the aim button held or while braced, and from the fourteen
                // left-tag magazine props - for a player who has never pressed the key. coop_qdrawOn
                // makes it genuinely inert instead of merely small: at coop_qdraw 0 the server never
                // publishes 1, the cvar stays 0, the guard never fires and no rotation is lost. The
                // residue in this function is one cached Cvar_Get and one integer test - inert, not
                // byte-identical; sidearm.md section 6 enumerates every coop_qdraw 0 delta honestly.
                //
                // coop_qdrawOn is published per player, change-only, by Player::TickCoopSidearm on
                // the stufftext bus - the same wire and the same shape as coop_braceMounted
                // (fgame/player.cpp:15194), read back the same way CG_AimingDownSights (cg_view.c:4659)
                // reads that one - its `cgi.Cvar_Get("coop_braceMounted", "0", 0)` at :4692.
                // `set coop_*` is auto-allowed by CG_IsVariableAllowed
                // (cg_servercmds_filter.cpp:169-172), so it needs no whitelist entry. This branch is
                // the LOCAL player's first-person attachment only - it is the `} else {` arm of
                // `if (s1->parent != cg.snap->ps.clientNum || bThirdPerson)`, i.e. the inverse of the
                // world-model path, reached through the tag whitelist. In cg_modelanim.c TODAY those
                // are :1999 (the gate), :2020 (this arm) and :2033 (the whitelist) - grep the
                // conditions rather than trusting the numbers, bug-2462 moved this file ~50 lines on
                // 2026-09-04. Either way a per-player cvar is exactly the right scope.
                //
                // ZERO THE POSE FACTOR - NEVER `return`. A return here also skips the loop sound,
                // CG_UpdateEntityEmitters, CG_ProcessEntityCommands and the cent->animLast*/
                // usageIndexLast bookkeeping at the tail of this function, so the commands then fire
                // out of step; and cg_adsHideOffHand deliberately SHOWS the left hand during a
                // reload, so the player would watch a bare hand mime seating a magazine that is not
                // there. THAT IS bug-315 EXACTLY, which is why bCoopHideDraw (:1712) exists in this
                // file rather than an early return.
                //
                // THE DROOP (bug-2458), THE LAG (bug-2459) AND THE IDLE-INSPECT ROLL (bug-2462)
                // BELOW ARE NOT AFFECTED, AND MUST NOT BE. THREE blocks sit outside the
                // `if (s_fAdsPose > 0.001f)` gate today, not two - all three landed in this file on
                // 2026-09-04 - and none of them can be reached by zeroing this file-static:
                //   the droop reads CG_CoopDroopAngle(), which applies its own ADS cancel
                //     (`fTgt *= (1.0f - CG_AdsPoseFactor());`, cg_view.c:4650);
                //   the lag reads CG_CoopLagAngles(), which does the same (:4599); and
                //   the inspect roll reads CG_CoopInspectRoll() (cg_view.c:4621), which reads
                //     NEITHER s_fAdsPose NOR CG_AdsPoseFactor() - its body is `return s_inspPubRoll;`
                //     and its own comment says "No cg.time cache and no ADS scale here". It is
                //     untouched for a different reason from the other two: not because it cancels
                //     the pose itself, but because it never consults it at all. The recorded
                //     consequence, the same class as the droop/heft note below: the PARKED rifle
                //     also picks up the idle-inspect roll about its own grip.
                // So zeroing s_fAdsPose cannot reach any of the three: no double-application, and no
                // silent disabling. What DOES change with a gun parked on the left tag is that each
                // attached weapon entity runs this branch once, so the parked gun gets its OWN droop
                // and lag rotation about its own grip. That is correct rather than doubled - the two
                // guns are two entities - but note the magnitudes come from the shared once-per-frame
                // integrators, which are keyed to the EQUIPPED weapon: during a draw CoopGunHeft()
                // reads the PISTOL's published weight (coop_gunHeft, cg_view.c:7013), so the parked
                // rifle droops by a pistol's heft, not a rifle's. It is a small visual understatement
                // on a gun that is deliberately out of the way, and it is not worth a second
                // per-entity heft channel; recorded here so nobody re-derives it in playtest.
                //
                // s_fAdsPose is re-assigned at the top of this block on every call and read only
                // inside the `if (s_fAdsPose > 0.001f)` gate below (the `fAdsPitch *= s_fAdsPose;`
                // trio and the `fCrouchMix` line, :2073-2126 before this insertion), so writing 0
                // cannot leak to another entity or another frame.
                //
                // If hiding the parked gun specifically is ever wanted, gate it on a signal that
                // identifies THE PARKED GUN - e.g. Player::TickCoopSidearm setting RF_DONTDRAW on
                // m_pCoopQDrawPrimary server-side, which the existing `cgi.R_AddRefEntityToScene(&model,
                // s1->parent);` guard (:2847) already honours - never on "the tag is not
                // tag_weapon_right".
                s_fAdsPose = CG_AdsPoseFactor();
                {
                    static cvar_t *pCoopQDrawOn = NULL;

                    if (!pCoopQDrawOn) {
                        pCoopQDrawOn = cgi.Cvar_Get("coop_qdrawOn", "0", 0);
                    }
                    if (pCoopQDrawOn->integer && Q_stricmp(szTagName, "tag_weapon_right")) {
                        s_fAdsPose = 0.0f;
                    }
                }
                if (s_fAdsPose > 0.001f) {
                    vec3_t      vAdsA, vAdsB;
                    const char *adsWpn   = "";
                    const adsGunTune_t *adsT;
                    qboolean    tune   = (cg_adsTune && cg_adsTune->integer) ? qtrue : qfalse;
                    qboolean    ducked = (cg.predicted_player_state.pm_flags & PMF_DUCKED) ? qtrue : qfalse;
                    float       fAdsPitch, fAdsYaw, fAdsRoll;

                    if (cg.snap->ps.activeItems[1] >= 0) {
                        adsWpn = CG_ConfigString(CS_WEAPONS + cg.snap->ps.activeItems[1]);
                    }
                    // Per-gun ADS sight values are BAKED in s_adsGunTune (CG_FindAdsTune). TUNE MODE
                    // (cg_adsTune 1) ignores the table and uses the global cg_ads* cvars so the held gun can
                    // be dialled live; any gun NOT in the table also falls back to those globals.
                    adsT      = tune ? NULL : CG_FindAdsTune(adsWpn);
                    fAdsPitch = adsT ? adsT->sPitch : (cg_adsPitch ? cg_adsPitch->value : 0.0f);
                    fAdsYaw   = adsT ? adsT->sYaw   : (cg_adsYaw   ? cg_adsYaw->value   : 0.0f);
                    fAdsRoll  = adsT ? adsT->sRoll  : (cg_adsRoll  ? cg_adsRoll->value  : 0.0f);
                    fAdsPitch *= s_fAdsPose; // scaled by the eased pose: full at 1, nothing at 0
                    fAdsYaw   *= s_fAdsPose;
                    fAdsRoll  *= s_fAdsPose;

                    // [weight 6] MUZZLE DROOP rides in on the same pitch term. This is the one place
                    // in the pipeline where the rotation pivot is the GRIP rather than the player's
                    // feet - model.origin was set from tag_weapon_right by CG_AttachEntity - so a
                    // heavy barrel can drop while the hands stay put, which is exactly what the cue
                    // is, and it needs no positional compensation at all. Added rather than replacing
                    // so the per-gun ADS tune is untouched; the droop is already zero under ADS.
                    // [vet] NOT folded into fAdsPitch: this whole block is gated on the ADS pose
                    // being non-zero, while the droop is defined as (1 - ads) - the two conditions
                    // are exact complements, so the droop could only ever apply in the state it was
                    // written to stay out of, and never in the hip-fire state it was written for.
                    // Applied separately below, outside that gate.

                    // pitch: rotate forward + up about the left axis (tilt muzzle up/down)
                    if (fAdsPitch != 0.0f) {
                        RotatePointAroundVector(vAdsA, model.axis[1], model.axis[0], fAdsPitch);
                        RotatePointAroundVector(vAdsB, model.axis[1], model.axis[2], fAdsPitch);
                        VectorCopy(vAdsA, model.axis[0]);
                        VectorCopy(vAdsB, model.axis[2]);
                    }
                    // yaw: rotate forward + left about the up axis (angle muzzle left/right)
                    if (fAdsYaw != 0.0f) {
                        RotatePointAroundVector(vAdsA, model.axis[2], model.axis[0], fAdsYaw);
                        RotatePointAroundVector(vAdsB, model.axis[2], model.axis[1], fAdsYaw);
                        VectorCopy(vAdsA, model.axis[0]);
                        VectorCopy(vAdsB, model.axis[1]);
                    }
                    // roll: rotate up + left about the forward axis (un-tilt the gun) - standing
                    if (fAdsRoll != 0.0f) {
                        RotatePointAroundVector(vAdsA, model.axis[0], model.axis[1], fAdsRoll);
                        RotatePointAroundVector(vAdsB, model.axis[0], model.axis[2], fAdsRoll);
                        VectorCopy(vAdsA, model.axis[1]);
                        VectorCopy(vAdsB, model.axis[2]);
                    }

                    // CROUCH-only EXTRA correction (added on top of the standing rotation): the crouch pose
                    // hunches the body and carries the gun off the standing sight line. Per-gun crouch values
                    // come from the table; tune mode / un-tabled guns fall back to the cg_adsCrouch* cvars.
                    // [user 2026-08-20] THE ADS JOLT. These three were applied at FULL STRENGTH,
                    // never multiplied by s_fAdsPose, for the whole ~0.73s ease-out - and then
                    // vanished in a single frame when the enclosing gate (> 0.001f) closed. The
                    // values are not small: cYaw is -38.5 on the M1 Garand, -34.5 on the KAR98,
                    // -43.0 on the shotgun. Crouched, that is the entire per-gun sight correction
                    // snapping off at once, which is exactly the reported "gun may be way to the
                    // right aiming up ... so it snaps back into its non ADS position".
                    // Now scaled by BOTH the ADS factor and the eased crouch blend, so the pose
                    // is continuous when aiming in/out AND when crouching/standing while aimed.
                    // The guard also has to admit the blend-OUT frames after PMF_DUCKED clears,
                    // or standing up while aimed would still cut the correction off at once.
                    if (ducked || CG_AdsCrouchBlend() > 0.001f) {
                        float fCrouchMix = s_fAdsPose * CG_AdsCrouchBlend();
                        float cp = (adsT ? adsT->cPitch : (cg_adsCrouchPitch ? cg_adsCrouchPitch->value : 0.0f)) * fCrouchMix;
                        float cy = (adsT ? adsT->cYaw   : (cg_adsCrouchYaw   ? cg_adsCrouchYaw->value   : 0.0f)) * fCrouchMix;
                        float cr = (adsT ? adsT->cRoll  : (cg_adsCrouchRoll  ? cg_adsCrouchRoll->value  : 0.0f)) * fCrouchMix;
                        if (cp != 0.0f) {
                            RotatePointAroundVector(vAdsA, model.axis[1], model.axis[0], cp);
                            RotatePointAroundVector(vAdsB, model.axis[1], model.axis[2], cp);
                            VectorCopy(vAdsA, model.axis[0]);
                            VectorCopy(vAdsB, model.axis[2]);
                        }
                        if (cy != 0.0f) {
                            RotatePointAroundVector(vAdsA, model.axis[2], model.axis[0], cy);
                            RotatePointAroundVector(vAdsB, model.axis[2], model.axis[1], cy);
                            VectorCopy(vAdsA, model.axis[0]);
                            VectorCopy(vAdsB, model.axis[1]);
                        }
                        if (cr != 0.0f) {
                            RotatePointAroundVector(vAdsA, model.axis[0], model.axis[1], cr);
                            RotatePointAroundVector(vAdsB, model.axis[0], model.axis[2], cr);
                            VectorCopy(vAdsA, model.axis[1]);
                            VectorCopy(vAdsB, model.axis[2]);
                        }
                    }
                }

                // [weight 6, user 2026-09-04, bug-2458] MUZZLE DROOP - NOW GENUINELY OUTSIDE THE ADS GATE.
                //
                // This block used to sit INSIDE `if (s_fAdsPose > 0.001f)` above, while the angle it reads
                // is scaled by `(1.0f - CG_AdsPoseFactor())` in CG_CoopDroopAngle (cg_view.c:4382). Those
                // are exact complements, so it could only ever be non-zero during the brief ADS transition -
                // and was being multiplied toward zero even there. A Panzerschreck and a Luger drooped
                // identically at hip carry because both drooped by nothing. The comment on the block already
                // claimed it lived outside the gate; only the braces were never moved. TRAPS T23.
                //
                // STILL INSIDE THE tag_weapon_right BRANCH, DELIBERATELY. model.origin has been set from
                // tag_weapon_right by CG_AttachEntity, so the pivot is the GRIP. Applying this to pREnt->axis
                // instead is bug-2142: the first-person player entity's origin is at the FEET, so the rotation
                // swings the whole rig on a ~60-unit lever and adds a parasitic forward shove about twice the
                // intended drop - and never touches pREnt->origin, so the feel budget cannot see it.
                {
                    float fDroop = CG_CoopDroopAngle();
                    if (fDroop > 0.01f) {
                        vec3_t vDrA, vDrB;
                        RotatePointAroundVector(vDrA, model.axis[1], model.axis[0], -fDroop);
                        RotatePointAroundVector(vDrB, model.axis[1], model.axis[2], -fDroop);
                        VectorCopy(vDrA, model.axis[0]);
                        VectorCopy(vDrB, model.axis[2]);
                    }
                }

                // [weight 7, user 2026-09-04, bug-2459] ROTATIONAL WEAPON LAG - the swing the spring never had.
                //
                // The lag spring's output was spent entirely on translation, which reads as DRAG. This adds the
                // rotation about the GRIP that reads as MASS ON A LEVER - the muzzle swinging off the view axis
                // on a fast turn and settling back, which is the whole difference between a Panzerschreck and a
                // Luger rather than merely a slower one.
                //
                // SAME PIVOT AS THE DROOP ABOVE, and for the same reason: model.origin came from
                // tag_weapon_right, so this turns the gun about the hand. On pREnt->axis it would instead swing
                // the whole rig about the player's FEET (bug-2142). CG_CoopLagAngles already applies the ADS
                // cancel and the degree clamp, so there is nothing to bound here.
                //
                // Yaw turns forward+left about UP; pitch turns forward+up about LEFT - the same axis the droop
                // uses, so the two compose rather than fight.
                {
                    float          fLagYaw = 0.0f, fLagPitch = 0.0f;
                    static cvar_t *pLagBodyM = NULL;
                    float          fGunShare;
                    CG_CoopLagAngles(&fLagYaw, &fLagPitch);
                    // [user 2026-09-06, bug-2502] the BODY now carries cg_weaponLagBody of the swing about the
                    // same grip (cg_view.c, the lag block), and the gun inherits that through tag_weapon_right;
                    // only the remainder is applied to the gun alone here, so the hands stay on the fore-grip.
                    if (!pLagBodyM) {
                        pLagBodyM = cgi.Cvar_Get("cg_weaponLagBody", "1", CVAR_ARCHIVE);
                    }
                    fGunShare = 1.0f - pLagBodyM->value;
                    if (fGunShare < 0.0f) { fGunShare = 0.0f; }
                    if (fGunShare > 1.0f) { fGunShare = 1.0f; }
                    fLagYaw   *= fGunShare;
                    fLagPitch *= fGunShare;
                    if (fLagYaw > 0.01f || fLagYaw < -0.01f) {
                        vec3_t vLgA, vLgB;
                        RotatePointAroundVector(vLgA, model.axis[2], model.axis[0], fLagYaw);
                        RotatePointAroundVector(vLgB, model.axis[2], model.axis[1], fLagYaw);
                        VectorCopy(vLgA, model.axis[0]);
                        VectorCopy(vLgB, model.axis[1]);
                    }
                    if (fLagPitch > 0.01f || fLagPitch < -0.01f) {
                        vec3_t vLgA, vLgB;
                        RotatePointAroundVector(vLgA, model.axis[1], model.axis[0], fLagPitch);
                        RotatePointAroundVector(vLgB, model.axis[1], model.axis[2], fLagPitch);
                        VectorCopy(vLgA, model.axis[0]);
                        VectorCopy(vLgB, model.axis[2]);
                    }
                }

                // [weight 8, user 2026-09-04, bug-2462] IDLE INSPECT - THE FLANK ROLL, AT THE GRIP.
                //
                //   "the idle inspect weapon animation is still bad, when the gun gets turned to
                //    the left to observe the right side of the gun, you're shoulder clips right
                //    through the camera. happens slightly on the other side too."
                //
                // The inspect used to express the whole flank turn as a roll of pREnt->axis - the
                // FIRST-PERSON PLAYER ENTITY, whose tiki is the <skin>_fps.tik body swapped in at
                // :2478. So up to 52 degrees of roll was applied to the arms and shoulder caps,
                // and cg_view.c's tag-pivot compensation only ever put the GUN back; the body had
                // nothing putting it back. The trigger hand sits ~1.5 units in front of the eye at
                // rest against r_znear 4, so it does not have far to travel.
                //
                // cg_view.c now keeps only ~12 degrees on the body (coop_inspectBodyTurn, soft
                // knee) and publishes the remainder here. THIS is the rotation that actually shows
                // the flank: model.origin came from tag_weapon_right, and model.axis[0] is the
                // weapon's own forward - measured at dot +0.99 with the view forward - so this
                // spins the receiver about the barrel with the muzzle staying put, turning the
                // flank normal 1:1 instead of the ~0.3:1 a body roll about the view axis manages.
                // It also moves nothing toward the lens: a roll about an axis within 8 degrees of
                // the view axis is depth-neutral to second order.
                //
                // Roll only - no yaw. A yaw applied here would have to use the already-rolled up
                // axis and would leak roll*yaw into muzzle pitch, with the sign of the flank. The
                // gesture's small yaw stays on the body, scaled down with the body roll.
                //
                // SAME PIVOT AND SAME BRANCH as the droop and the lag above, which means the same
                // guarantees: this is inside `s1->parent == cg.snap->ps.clientNum && !bThirdPerson`
                // (:1999), so it can never touch a teammate's weapon, and it writes only
                // model.axis - no camera, no aim ray, no feel budget.
                {
                    float fInspRoll = CG_CoopInspectRoll();
                    if (fInspRoll > 0.01f || fInspRoll < -0.01f) {
                        vec3_t vInA, vInB;
                        RotatePointAroundVector(vInA, model.axis[0], model.axis[1], fInspRoll);
                        RotatePointAroundVector(vInB, model.axis[0], model.axis[2], fInspRoll);
                        VectorCopy(vInA, model.axis[1]);
                        VectorCopy(vInB, model.axis[2]);
                    }
                }

                // HZM coop [user 2026-09-05] THE PARKED PRIMARY. Last statement in the branch on
                // purpose: the ADS, droop, lag and inspect blocks above all rotate model.axis about
                // model.origin, and the parked gun is not being aimed, drooped, lagged or inspected -
                // it is being carried. Overriding here means its pose is exactly the two cvars and
                // nothing else, which is what makes it tunable in one pass instead of six.
                // CG_CoopQDrawIsParked keys on the ENTITY NUMBER, so a reload magazine prop riding the
                // same tag on the same parent is never touched.
                // AND the tag: the parked primary is on tag_weapon_left by construction
                // (weapon.cpp attachToTag_offhand), while coop_qdrawOn is never cleared client-side
                // on a map change - so a stale entnum from a draw that was active when the last
                // map ended could otherwise match the HELD rifle on tag_weapon_right for the frames
                // before the server's first publish from the -1 seed.
                if (CG_CoopQDrawIsParked(s1) && !Q_stricmp(szTagName, "tag_weapon_left")) {
                    CG_CoopQDrawParkInView(&model, s1->number);
                }
            } else {
                // Don't show the model at all
                return;
            }
        }

        if (s1->loopSound && !CG_LoopSoundIsForeignLocal(s1)) {
            cgi.S_AddLoopingSound(
                model.origin,
                vec3_origin,
                cgs.sound_precache[s1->loopSound],
                s1->loopSoundVolume,
                s1->loopSoundMinDist,
                s1->loopSoundMaxDist,
                s1->loopSoundPitch,
                s1->loopSoundFlags
            );
        }

        if (cent->tikiLoopSound) {
            cgi.S_AddLoopingSound(
                cent->lerpOrigin,
                vec3_origin,
                cent->tikiLoopSound,
                cent->tikiLoopSoundVolume,
                cent->tikiLoopSoundMinDist,
                cent->tikiLoopSoundMaxDist,
                cent->tikiLoopSoundPitch,
                cent->tikiLoopSoundFlags
            );
        }

        // set the attached model to have the same render FX
        // HZM coop (bug-1217) - RF_THIRD_PERSON was written TWICE in both masks where
        // RF_FIRST_PERSON belongs. The pair means "the child's view-visibility is EXACTLY the
        // parent's": line 1 drops whatever the child was carrying, line 2 takes the parent's.
        // With the typo the first-person bit could never be DROPPED, only inherited. Restores the
        // intended semantics; verified bit-for-bit inert on today's data, so this is a latent-only
        // correctness fix - nothing in fgame ever sets RF_FIRST_PERSON (the renderEffects script
        // token table in entity.cpp has no name for it), and both CG_AttachEntity and
        // CG_AttachEyeEntity already OR the parent's copy in (RF_FIRST_PERSON is deliberately NOT
        // in RF_FLAGS_NOT_INHERITED), so child == parent either way for every entity that exists.
        model.renderfx &= ~(RF_FIRST_PERSON | RF_THIRD_PERSON | RF_DEPTHHACK);
        model.renderfx |= parent->renderfx & (RF_FIRST_PERSON | RF_THIRD_PERSON | RF_DEPTHHACK);
    }

    for (i = 0; i < 3; i++) {
        model.shaderRGBA[i] = cent->color[i] * 255;
    }
    model.shaderRGBA[3] = s1->alpha * 255;

    // set surfaces
    memcpy(model.surfaces, s1->surfaces, MAX_MODEL_SURFACES);

    // HZM coop (bug-1208) - THIS IS A VIEW-MODEL HIDER, so it must not run in third person.
    // The stock parenthesisation was `((!cg_drawviewmodel->integer && !bThirdPerson) || STAT_INZOOM)`:
    // !bThirdPerson guarded ONLY the cg_drawviewmodel clause, so the STAT_INZOOM clause fired
    // unconditionally and nodraw'd EVERY surface of EVERY entity attached to the local player even
    // while the 3rd-person body was on screen - the switcher helmet (attached to "Bip01 Head",
    // coop_mod/helmet.scr), holstered weapons, gear, all of it. Reachable because zoom normally
    // forces first person (see the STAT_INZOOM term where bThirdPerson is computed) EXCEPT on a
    // turret: PMF_TURRET is exempt there, and VehicleTurretGun force-zooms its gunner purely to pin
    // the fov (fgame/player.cpp ToggleZoom), so mounting any MG42 / jeep .30cal / halftrack in 3rd
    // person stripped the player's helmet and every other attached prop.
    // Hoisting !bThirdPerson out is bit-IDENTICAL in first person (it is already true there), so the
    // viewmodel/zoom behaviour this block exists for is unchanged; it simply stops firing in 3P,
    // where there is no viewmodel to hide.
    if (!(s1->renderfx & RF_ALWAYSDRAW) && s1->parent != ENTITYNUM_NONE && s1->parent == cg.snap->ps.clientNum
        && !bThirdPerson && (!cg_drawviewmodel->integer || cg.snap->ps.stats[STAT_INZOOM])) {
        // hide all surfaces while zooming or if the viewmodel shouldn't be shown
        for (i = 0; i < MAX_MODEL_SURFACES; i++) {
            model.surfaces[i] |= MDL_SURFACE_NODRAW;
        }
        CoopGunVisNote(&s_coopGunVis, 1, "HIDE-WEAPON", (s1->eFlags & EF_UNARMED) ? 1 : 0);
    } else if (s1->parent == cg.snap->ps.clientNum && s1->parent != ENTITYNUM_NONE) {
        CoopGunVisNote(&s_coopGunVis, 0, "SHOW-WEAPON", (s1->eFlags & EF_UNARMED) ? 1 : 0);
    }

    if (!(s1->renderfx & RF_DONTDRAW) && !bCoopHideDraw && (model.renderfx & RF_SHADOW)) {
        // add the shadow
        CG_EntityShadow(cent, &model);
    }

    iAnimFlags = 0;

    // combine anim flags from all frame infos
    for (i = 0; i < MAX_FRAMEINFOS; i++) {
        if (model.frameInfo[i].weight && model.frameInfo[i].index >= 0) {
            iAnimFlags |= cgi.Anim_Flags(model.tiki, model.frameInfo[i].index);
        }
    }

    if (iAnimFlags & TAF_AUTOSTEPS) {
        int iTagNum;
        // Automatically calculate the footsteps sounds

        if (cent->bFootOnGround_Right) {
            iTagNum = cgi.Tag_NumForName(model.tiki, "Bip01 R Foot");
            if (iTagNum >= 0) {
                cent->bFootOnGround_Right = cgi.TIKI_IsOnGround(&model, iTagNum, 13.653847f);
            } else {
                cent->bFootOnGround_Right = qtrue;
            }
        } else {
            iTagNum = cgi.Tag_NumForName(model.tiki, "Bip01 R Foot");
            if (iTagNum >= 0) {
                if (cgi.TIKI_IsOnGround(&model, iTagNum, 13.461539f)) {
                    CG_Footstep(
                        "Bip01 R Foot",
                        cent,
                        &model,
                        (iAnimFlags & TAF_AUTOSTEPS_RUNNING),
                        (iAnimFlags & TAF_AUTOSTEPS_EQUIPMENT)
                    );
                    cent->bFootOnGround_Right = qtrue;
                }
            } else {
                cent->bFootOnGround_Right = qtrue;
            }
        }

        if (cent->bFootOnGround_Left) {
            iTagNum = cgi.Tag_NumForName(model.tiki, "Bip01 L Foot");
            if (iTagNum >= 0) {
                cent->bFootOnGround_Left = cgi.TIKI_IsOnGround(&model, iTagNum, 13.653847f);
            } else {
                cent->bFootOnGround_Left = qtrue;
            }
        } else {
            iTagNum = cgi.Tag_NumForName(model.tiki, "Bip01 L Foot");
            if (iTagNum >= 0) {
                if (cgi.TIKI_IsOnGround(&model, iTagNum, 13.461539f)) {
                    CG_Footstep(
                        "Bip01 L Foot",
                        cent,
                        &model,
                        (iAnimFlags & TAF_AUTOSTEPS_RUNNING),
                        (iAnimFlags & TAF_AUTOSTEPS_EQUIPMENT)
                    );

                    cent->bFootOnGround_Left = qtrue;
                }
            } else {
                cent->bFootOnGround_Left = qtrue;
            }
        }
    } else {
        cent->bFootOnGround_Left  = qtrue;
        cent->bFootOnGround_Right = qtrue;
    }

    if (cent->currentState.eType == ET_PLAYER && !(cent->currentState.eFlags & EF_DEAD)) {
        CG_PlayerTeamIcon(&model, &cent->currentState);
    }


    if ((cent->currentState.eType == ET_MODELANIM || cent->currentState.eType == ET_MODELANIM_SKEL)
        && (cent->currentState.renderfx & RF_COOP_BOSS)
        && !(cent->currentState.eFlags & EF_DEAD)) {
        int iconType;
        /* The officer is additionally tagged "+additivedynamiclight" (RF_ADDITIVE_DLIGHT)
         * by the coop script as an officer marker. That bit is NOT set by default on
         * sentients (RF_SHADOW_PRECISE is -> it put the eagle over every actor AND player),
         * has no visual effect without an attached dlight, and the stock game.dll already
         * supports the token (a brand-new renderfx token would need an fgame rebuild this
         * build tree can't produce). It gets the eagle icon. */
        if (cent->currentState.renderfx & RF_ADDITIVE_DLIGHT) {
            iconType = 2; /* officer -> Reichsadler eagle */
        } else if (cent->currentState.renderfx & RF_LIGHTSTYLE_DLIGHT) {
            /* HZM coop [user 2026-08-21] SURRENDERED OVERRIDE: "the allied icon (same as
             * paratroopers) once they surrender". A surrendered german is still team german
             * until converted, so EF_AXIS is still set and the branch below would give him
             * the swastika - the opposite of what the icon must say ("this one is yours").
             * Script signals with +lightstyledynamiclight, the same reuse trick as the
             * officer's additive bit: visually inert without an attached dlight, and only
             * ever applied to map light entities otherwise - which can never carry
             * RF_COOP_BOSS, so this combination is unambiguous. Conversion removes the bit
             * and flips the team, after which the star draws through the normal path. */
            iconType = 0; /* surrendered -> allied star, despite EF_AXIS */
        } else if (cent->currentState.eFlags & EF_AXIS) {
            iconType = 1; /* axis -> swastika */
        } else {
            iconType = 0; /* ally -> star */
        }
        CG_ActorOverheadIcon(&model, iconType);
    }

    if (s1->number == cg.snap->ps.clientNum) {
        if ((!cg.bFPSModelLastFrame && !bThirdPerson) || (cg.bFPSModelLastFrame && bThirdPerson)) {
            // reset the animations when toggling 3rd person
            for (i = 0; i < MAX_FRAMEINFOS; i++) {
                cent->animLast[i] = -1;
            }

            cent->animLastWeight = 0;
            cent->usageIndexLast = 0;

            cg.bFPSModelLastFrame = !bThirdPerson;
        }

        // player footsteps, walking/falling
        if (cg.bFPSOnGround != cg.predicted_player_state.walking) {
            cg.bFPSOnGround = cg.predicted_player_state.walking;
            if (cg.predicted_player_state.walking) {
                {
                    // [2026-08-21] TIERED VOLUME. Was a hardcoded 1.0 for every landing. Reads the
                    // shared severity latch so it agrees with the camera dip and the weapon dip on
                    // the same frame; falls back to a normal-strength landing if the latch is cold
                    // (this hook also runs in third person and while dead, where the first-person
                    // detector does not advance, so it must never go silent just because the latch
                    // has nothing for it).
                    float fSev = CG_GetLandingSeverity();
                    float fVol = (fSev > 0.0f) ? (0.55f + fSev * 0.65f) : 1.0f;
                    CG_LandingSound(cent, &model, fVol, 1);
                }
            } else {
                if (cent->iNextLandTime < cg.time) {
                    CG_Footstep(0, cent, &model, 1, 1);
                }

                cent->iNextLandTime = cg.time + 200;
            }
        }

        if (!bThirdPerson) {
            // first person view

            if (!(cg.predicted_player_state.pm_flags & PMF_CAMERA_VIEW)
                && (cg.snap->ps.stats[STAT_HEALTH] <= 0 || cg_animationviewmodel->integer)) {
                // use world position for this case
                CG_OffsetFirstPersonView(&model, qtrue);
            }

            if (!cg.pLastPlayerWorldModel || cg.pLastPlayerWorldModel != model.tiki) {
                qhandle_t hModel;
                char      fpsname[128];

                COM_StripExtension(model.tiki->a->name, fpsname, sizeof(fpsname));
                Q_strcat(fpsname, sizeof(fpsname), "_fps.tik");

                hModel = cgi.R_RegisterModel(fpsname);
                if (hModel) {
                    cg.hPlayerFPSModelHandle = hModel;
                    cg.pPlayerFPSModel       = cgi.R_Model_GetHandle(hModel);
                    if (!cg.pPlayerFPSModel) {
                        cg.pPlayerFPSModel = model.tiki;
                    }
                } else {
                    if (cg.snap->ps.stats[STAT_TEAM] == TEAM_AXIS) {
                        hModel = cgi.R_RegisterModel(CG_GetPlayerLocalModelTiki(dm_playergermanmodel->resetString));
                    } else {
                        hModel = cgi.R_RegisterModel(CG_GetPlayerLocalModelTiki(dm_playermodel->resetString));
                    }

                    if (hModel) {
                        cg.hPlayerFPSModelHandle = hModel;
                        cg.pPlayerFPSModel       = cgi.R_Model_GetHandle(hModel);

                        if (!cg.pPlayerFPSModel) {
                            cg.pPlayerFPSModel = model.tiki;
                        }
                    } else {
                        cg.hPlayerFPSModelHandle = cgs.model_draw[s1->modelindex];
                        cg.pPlayerFPSModel       = model.tiki;
                    }
                }

                cg.pLastPlayerWorldModel = model.tiki;
            }

            model.tiki   = cg.pPlayerFPSModel;
            model.hModel = cg.hPlayerFPSModelHandle;
            memset(model.surfaces, 0, sizeof(model.surfaces));

            // HZM coop [user 2026-08-23, bug-2080] RE-APPLY THE ARMORY GLOVE IN FIRST PERSON.
            //
            // The memset directly above is why 1P needs its own line at all. The third-person glove
            // rides in the player entity's per-surface bits, but this function swaps to a DIFFERENT
            // tiki (<skin>_fps.tik) whose surface indices do not correspond, so it clears the array
            // wholesale - correctly. Without re-applying here, a player would see gloved hands on
            // their own body in third person and bare hands down the sights, which is worse than
            // having no gloves at all.
            //
            // The index arrives as coop_gloveIdx, a plain integer with no embedded quote, which the
            // server pushes on change. The whole coop_ namespace is prefix-allowed by
            // cg_servercmds_filter.cpp:173, so the wire is the same proven one coop_coverSide uses.
            //
            // Bit layout must match MDL_SURFACE_SKININDEX in q_shared.h exactly: bits 0-1 carry the
            // low two, bit 6 the high one. Writing the composed byte rather than calling the macro
            // in reverse keeps the two definitions adjacent in review.
            {
                static cvar_t *pGloveIdx = NULL;
                int            g;

                if (!pGloveIdx) {
                    pGloveIdx = cgi.Cvar_Get("coop_gloveIdx", "0", 0);
                }
                g = pGloveIdx->integer;
                if (g > 0 && g <= 7 && model.tiki) {
                    static const char *kHandSurfaces[3] = {"triggerhand", "lefthand", "garandhand"};
                    int                bits             = (g & 3) | ((g & 4) << 4);
                    int                i;

                    for (i = 0; i < 3; i++) {
                        int sn = cgi.Surface_NameToNum(model.tiki, kHandSurfaces[i]);
                        if (sn >= 0 && sn < MAX_MODEL_SURFACES) {
                            model.surfaces[sn] = (byte)bits;
                        }
                    }
                }
            }

            CG_ViewModelAnimation(&model);
            model.renderfx |= RF_FRAMELERP;
            // must run BEFORE ForceUpdatePose - that is what applies the bone controllers - and
            // AFTER the tiki has been swapped to the FPS model, so the bone indices resolve against
            // the right skeleton.
            CoopFingerLife(&model);
            cgi.ForceUpdatePose(&model);

            if ((cent->currentState.eFlags & EF_UNARMED) || cg_drawviewmodel->integer <= 1
                || cg.snap->ps.stats[STAT_INZOOM] || cg.snap->ps.stats[STAT_HEALTH] <= 0) {
                // unarmed or zooming, hide the arms
                CoopGunVisNote(&s_coopArmsVis, 1, "HIDE-ARMS", (cent->currentState.eFlags & EF_UNARMED) ? 1 : 0);
                for (i = 0; i < MAX_MODEL_SURFACES; i++) {
                    model.surfaces[i] |= MDL_SURFACE_NODRAW;
                }
            } else {
                CoopGunVisNote(&s_coopArmsVis, 0, "SHOW-ARMS", (cent->currentState.eFlags & EF_UNARMED) ? 1 : 0);
                // show/hide the garand hand depending if it's a rifle or not
                // so the hand can hold the rifle correctly

                const char *weaponstring = "";
                int         iSurfaceNum;

                if (cg.snap->ps.activeItems[1] >= 0) {
                    weaponstring = CG_ConfigString(CS_WEAPONS + cg.snap->ps.activeItems[1]);
                }

                // [user 2026-08-21] VARIANT-SAFE. A skin variant is named "<Base Gun> (<Finish>)",
                // so a whole-string compare against a base name misses all 247 of them. This is the
                // fourth site to need it, after the ADS tune, the viewmodel anim prefix and the
                // third-person magazine - so it is worth stating the rule plainly: ANY comparison
                // against a weapon name must strip the suffix first.
                {
                    char vbase[64];
                    if (CoopStripSkinSuffix(weaponstring, vbase, sizeof(vbase))) {
                        weaponstring = vbase;
                    }
                }
                if (!Q_stricmp(weaponstring, "M1 Garand") || !Q_stricmp(weaponstring, "Springfield '03 Sniper")
                    || !Q_stricmp(weaponstring, "Mauser KAR 98K") || !Q_stricmp(weaponstring, "KAR98 - Sniper")) {
                    // show the garand hands

                    iSurfaceNum = cgi.Surface_NameToNum(model.tiki, "lefthand");
                    if (iSurfaceNum >= 0) {
                        model.surfaces[iSurfaceNum] |= MDL_SURFACE_NODRAW;
                    }

                    iSurfaceNum = cgi.Surface_NameToNum(model.tiki, "garandhand");
                    if (iSurfaceNum >= 0) {
                        model.surfaces[iSurfaceNum] &= ~MDL_SURFACE_NODRAW;
                    }
                } else {
                    // hide the garand hands

                    iSurfaceNum = cgi.Surface_NameToNum(model.tiki, "garandhand");
                    if (iSurfaceNum >= 0) {
                        model.surfaces[iSurfaceNum] |= MDL_SURFACE_NODRAW;
                    }

                    iSurfaceNum = cgi.Surface_NameToNum(model.tiki, "lefthand");
                    if (iSurfaceNum >= 0) {
                        model.surfaces[iSurfaceNum] &= ~MDL_SURFACE_NODRAW;
                    }
                }

                // HZM coop - ADS off-hand HIDE. The raised aim poses bake the support (off) hand for a
                // different gun's foregrip, so on many weapons it hovers off the gun while aiming. Instead
                // of re-posing every weapon, just HIDE the support hand while aiming down the sights (the
                // trigger hand + gun stay, sight alignment + tuning unchanged). Covers both the normal
                // "lefthand" and the rifle "garandhand" support surfaces. Toggle: cg_adsHideOffHand 0.
                // ONLY while the steady aim pose (VM_ANIM_CHARGE) is playing - during a bolt rechamber,
                // reload, or weapon switch the vm anim changes away from charge, so the support hand
                // re-appears to work the bolt / magazine (bolt rifles like the Kar98 need this).
                {
                    // [user 2026-08-21] "my shoulders seem to jump in first person where I am coming
                    // out of ADS and after reloading" - and, almost certainly, the long-running
                    // "leaving ADS is a jolt" report as well.
                    //
                    // THE JOLT WAS NEVER MOTION. A live trace showed the camera perfectly still
                    // (dPos 0.00 on every frame of the transition) and the ADS pose factor decaying
                    // as a clean exponential. Nothing moved. What changed was GEOMETRY: this block
                    // NODRAWs a whole arm and the sleeve, and the gate was CG_AimingDownSights(),
                    // which is the raw BUTTON state. Release the button and the arm reappears in a
                    // single frame while the pose still has ~350ms of travel left - an arm popping
                    // into existence mid-transition, which no amount of camera or blend smoothing
                    // could ever have fixed. That is why nine fixes aimed at motion did nothing.
                    //
                    // Gate on the EASED pose factor instead, with hysteresis: hide once the weapon is
                    // genuinely up (>0.80) and do not restore until it is nearly back down (<0.12).
                    // The pop still exists - MDL_SURFACE_NODRAW is binary and a surface cannot fade -
                    // but it now happens when the arm is back where it belongs and mostly occluded by
                    // the weapon, instead of at the most visible moment of the whole animation.
                    //
                    // The anim condition stays: during a bolt rechamber, reload or switch the vm anim
                    // leaves charge and the support hand MUST return to work the bolt (Kar98 et al).
                    // That pop is motivated by an action on screen, so it reads as intent.
                    // [user 2026-08-21] "my left hand disappears with the thompson when I go down
                    // ADS" - then, importantly: "I had specifically enabled that for specific weapons
                    // when in ADS before specifically... it was waaaay back when we did ads tuning."
                    //
                    // The record backs that up. autoexec.cfg documents cg_adsHideOffHand 1 as a
                    // DELIBERATE global default: "the raised aim poses bake the off-hand for a
                    // different gun's foregrip, so on many weapons it hovers off the gun; hiding it
                    // leaves a clean trigger-hand + gun sight picture."
                    //
                    // A first pass here made it opt-IN with an empty default, which silently disabled
                    // a working feature on every weapon to fix one. Inverted: this is now an opt-OUT.
                    // Hiding stays on by default exactly as tuned, and only the named weapons keep
                    // their support hand. Add one when its hand looks correct without hiding:
                    //     cg_adsHideOffHandSkip "Thompson,Sten Gun"
                    cvar_t *pHideOff  = cgi.Cvar_Get("cg_adsHideOffHand", "1", CVAR_ARCHIVE);
                    cvar_t *pHideSkip = cgi.Cvar_Get("cg_adsHideOffHandSkip", "Thompson", CVAR_ARCHIVE);
                    static qboolean s_bOffHandHidden = qfalse;
                    float           fAdsPose         = CG_AdsPoseFactor();
                    qboolean        bSkipThisGun     = qfalse;
                    qboolean        bHandBusy        = qfalse;

                    if (pHideSkip && pHideSkip->string && pHideSkip->string[0]
                        && cg.snap->ps.activeItems[1] >= 0) {
                        const char *pszWeap = CG_ConfigString(CS_WEAPONS + cg.snap->ps.activeItems[1]);
                        char        szBase[64];

                        // variant-safe: a skin variant is "<Base Gun> (<Finish>)", so a raw compare
                        // would miss every one of them.
                        if (CoopStripSkinSuffix(pszWeap, szBase, sizeof(szBase))) {
                            pszWeap = szBase;
                        }
                        if (pszWeap && pszWeap[0] && strstr(pHideSkip->string, pszWeap)) {
                            bSkipThisGun = qtrue;
                        }
                    }

                    if (fAdsPose > 0.80f) {
                        s_bOffHandHidden = qtrue;
                    } else if (fAdsPose < 0.12f) {
                        s_bOffHandHidden = qfalse;
                    }
                    // [user 2026-08-21, MEASURED] THE SHOULDER POP.
                    //
                    // This used to require iViewModelAnim == VM_ANIM_CHARGE. A live trace killed that:
                    //     t=46275  vmanim=2  pose=0.547   <- aiming
                    //     t=46992  vmanim=1  pose=0.549   <- vmanim ALREADY back to idle
                    // The server flips the anim index to IDLE the instant the aim button is released,
                    // while the eased pose factor still has half its travel left. So the CHARGE term
                    // went false at pose ~0.55 and an entire arm plus the sleeve popped back into
                    // existence mid-transition - which is why the pose-based hysteresis added earlier
                    // did nothing at all: the anim term was overriding it every time. The same term
                    // fires the moment a reload starts, which is the other event the user reported.
                    //
                    // The anim check does earn its place - the support hand MUST come back to work a
                    // bolt or a magazine - so test for THAT rather than for CHARGE. Returning to idle
                    // is not a reason to un-hide; starting a reload is. The pose hysteresis now
                    // actually governs the transition, so the hand reappears at pose < 0.12, with the
                    // weapon back at the hip where the change is least visible.
                    {
                        // [user 2026-08-21] "on one of the loads the hand didnt even pull the
                        // slide/rail back on the stg44" - this read cg.snap->ps.iViewModelAnim, the
                        // SERVER's value. The coop idle flourish injects `rechamber` CLIENT-side, so
                        // the server still says IDLE, bHandBusy stayed false, and the support hand
                        // remained hidden while a bolt-pull animation played: the gesture with no
                        // hand performing it. Read what is actually PLAYING (g_iLastVMAnim, which the
                        // flourish writes) and fall back to the server value if the anim system has
                        // not been initialised yet.
                        int iVA = (cgi.anim && cgi.anim->g_iLastVMAnim >= 0)
                                      ? cgi.anim->g_iLastVMAnim : cg.snap->ps.iViewModelAnim;

                        bHandBusy = (iVA == VM_ANIM_RELOAD || iVA == VM_ANIM_RELOAD_SINGLE
                                     || iVA == VM_ANIM_RELOAD_END || iVA == VM_ANIM_RECHAMBER
                                     || iVA == VM_ANIM_PULLOUT || iVA == VM_ANIM_PUTAWAY)
                                        ? qtrue : qfalse;
                    }
                    if (pHideOff && pHideOff->integer && !bSkipThisGun && s_bOffHandHidden
                        && !bHandBusy) {
                        iSurfaceNum = cgi.Surface_NameToNum(model.tiki, "lefthand");
                        if (iSurfaceNum >= 0) {
                            model.surfaces[iSurfaceNum] |= MDL_SURFACE_NODRAW;
                        }
                        iSurfaceNum = cgi.Surface_NameToNum(model.tiki, "garandhand");
                        if (iSurfaceNum >= 0) {
                            model.surfaces[iSurfaceNum] |= MDL_SURFACE_NODRAW;
                        }
                        // also hide the off-arm: "viewsleeves" is the forearm/sleeve geometry that was
                        // left dangling once the support hand was hidden. Leaves trigger hand + gun only.
                        iSurfaceNum = cgi.Surface_NameToNum(model.tiki, "viewsleeves");
                        if (iSurfaceNum >= 0) {
                            model.surfaces[iSurfaceNum] |= MDL_SURFACE_NODRAW;
                        }
                    }
                }
            }

            if (!(s1->eFlags & EF_CLIMBWALL)) {
                // when the player is not climbing ladders show the entity
                model.renderfx |= RF_DEPTHHACK;
            }

            if (!(cg.predicted_player_state.pm_flags & PMF_CAMERA_VIEW)) {
                if (cg.snap->ps.stats[STAT_HEALTH] > 0 && !cg_animationviewmodel->integer) {
                    CG_OffsetFirstPersonView(&model, qfalse);
                }

                AnglesToAxis(cg.refdefViewAngles, cg.refdef.viewaxis);
            }

            model.renderfx &= ~(RF_FIRST_PERSON | RF_THIRD_PERSON);
            // set the first person render flag
            model.renderfx |= RF_FIRST_PERSON;
        }
    }

    model.reType = RT_MODEL;
    /* [user 2026-08-22, bug-2049] Record whether the LOCAL player's first-person model is
       actually submitted this frame. Edge-triggered like the other two, and scoped to the local
       player in first person so it can never spam on world entities. renderfx is printed raw
       because RF_DONTDRAW is set by the script `hide` command - if that bit is the one set, the
       question becomes WHO hid the player, which is a script search rather than an engine one. */
    if (s1->number == cg.snap->ps.clientNum && !bThirdPerson) {
        int bSkipped = ((s1->renderfx & RF_DONTDRAW) || bCoopHideDraw) ? 1 : 0;

        if (s_coopDrawVis != bSkipped) {
            if (!s_pGunVis) { s_pGunVis = cgi.Cvar_Get("coop_gunVisTrace", "0", CVAR_ARCHIVE); }
            if (s_pGunVis->integer) {
                cgi.Printf("^~^~^ GUNVIS t=%d %s renderfx=0x%x dontdraw=%d hidedraw=%d "
                           "inzoom=%d unarmed=%d health=%d pmflags=0x%x\n",
                           cg.time, bSkipped ? "SKIP-DRAW" : "DO-DRAW",
                           s1->renderfx,
                           (s1->renderfx & RF_DONTDRAW) ? 1 : 0,
                           bCoopHideDraw ? 1 : 0,
                           cg.snap ? cg.snap->ps.stats[STAT_INZOOM] : -1,
                           (s1->eFlags & EF_UNARMED) ? 1 : 0,
                           cg.snap ? cg.snap->ps.stats[STAT_HEALTH] : -1,
                           cg.snap ? cg.snap->ps.pm_flags : 0);
            }
            s_coopDrawVis = bSkipped;
        }
    }
    if (!(s1->renderfx & RF_DONTDRAW) && !bCoopHideDraw) {
        cgi.R_Model_GetHandle(model.hModel);
        if (VectorCompare(model.origin, vec3_origin)) {
            VectorCopy(s1->origin, model.origin);
            AngleVectors(s1->angles, model.axis[0], model.axis[1], model.axis[2]);
        }

        // HZM coop [user 2026-08-20] BLOOD ON THE GUN. Darken the weapon toward a dried red-brown
        // in proportion to how much blood it has picked up at knife range. This is a TINT, not a
        // decal: a decal cannot be projected onto the view weapon, which renders in its own
        // projection, and per-gun bloodied textures would mean authoring art for all 69 weapons.
        // Only the first-person weapon is tinted - the third-person model other players see is
        // untouched, so this is purely local flavour and cannot desync anything.
        // [2026-08-21] MULTIPLY into whatever colour the entity already carries rather than
        // overwriting it - the previous version stomped cent->color, which other systems set.
        // Also excluded on a turret: vehicleturret.cpp sets RF_DEPTHHACK on the turret viewmodel
        // too, and that is not the player's weapon.
        if ((model.renderfx & RF_DEPTHHACK) && CG_GunBlood() > 0.01f && cg.snap
            && !(cg.snap->ps.pm_flags & PMF_TURRET)) {
            float b = CG_GunBlood();
            if (b > 1.0f) { b = 1.0f; }
            model.shaderRGBA[0] = (byte)(model.shaderRGBA[0] * (1.0f - 0.16f * b));
            model.shaderRGBA[1] = (byte)(model.shaderRGBA[1] * (1.0f - 0.59f * b));
            model.shaderRGBA[2] = (byte)(model.shaderRGBA[2] * (1.0f - 0.63f * b));
        }

        // add to refresh list
        // HZM coop [user 2026-08-21] SURFACE PROBE. Publish which of the hand/sleeve surfaces are
        // actually NODRAW at SUBMIT time - after every system that writes them has had its say.
        // Two of them do: the retail garand-hand swap (which SHOWS lefthand or garandhand by weapon)
        // and the coop ADS off-hand hide (which hides lefthand, garandhand AND viewsleeves). The
        // reported "shoulders jump" is not motion - the camera measured dead still and the bones
        // moved ~1 unit - so geometry appearing is the remaining candidate, and this records exactly
        // which surface flips and at what ADS pose.
        {
            static const char *kProbeSurf[5] = {"lefthand", "garandhand", "viewsleeves",
                                                "triggerhand", "sleeves"};
            int p;

            g_iCoopSurfMask = 0;
            for (p = 0; p < 5; p++) {
                int sn = cgi.Surface_NameToNum(model.tiki, kProbeSurf[p]);
                if (sn >= 0) {
                    g_iCoopSurfMask |= (1 << (p * 2));                       /* exists */
                    if (model.surfaces[sn] & MDL_SURFACE_NODRAW) {
                        g_iCoopSurfMask |= (1 << (p * 2 + 1));               /* hidden */
                    }
                }
            }
        }
        cgi.R_AddRefEntityToScene(&model, s1->parent);
    }

    CG_UpdateEntityEmitters(s1->number, &model, cent);

    // HZM coop [225] - the hidden 1P viewmodel still processes its frame commands for the FIRE
    // SOUND (bug-315 kept them), but its VISUAL commands (tagdlight + tagspawnlinked muzzle
    // flash) spawned at the player's eyes-bone gun - "the burst comes from the left" while the
    // real turret flashes at its own barrel (shot0020). Mute non-sound commands for the whole
    // command dispatch below when this entity is the draw-skipped viewmodel.
    cg_bCoopMuteVisualCmds = bCoopHideDraw;

    if (s1->usageIndex == cent->usageIndexLast) {
        // process the exit commands of the last animations
        for (i = 0; i < MAX_FRAMEINFOS; i++) {
            if ((cent->animLastWeight >> i) & 1) {
                if (!model.frameInfo[i].weight || model.frameInfo[i].index != cent->animLast[i]) {
                    CG_ProcessEntityCommands(TIKI_FRAME_EXIT, cent->animLast[i], s1->number, &model, cent);
                }
            }
        }
    }

    for (i = 0; i < MAX_FRAMEINFOS; i++) {
        // process the entry commands of the current anim
        if (model.frameInfo[i].weight) {
            if (!((cent->animLastWeight >> i) & 1) || model.frameInfo[i].index != cent->animLast[i]) {
                CG_ProcessEntityCommands(TIKI_FRAME_ENTRY, model.frameInfo[i].index, s1->number, &model, cent);
                if (cent->animLastTimes[i] == -1) {
                    cent->animLast[i]      = model.frameInfo[i].index;
                    cent->animLastTimes[i] = model.frameInfo[i].time;
                } else {
                    cent->animLastTimes[i] = 0;
                }
            }

            CG_ClientCommands(&model, cent, i);
        }

        cent->animLastTimes[i] = model.frameInfo[i].time;
        cent->animLast[i]      = model.frameInfo[i].index;

        if (model.frameInfo[i].weight) {
            cent->animLastWeight |= 1 << i;
        } else {
            cent->animLastWeight &= ~(1 << i);
        }
    }

    cg_bCoopMuteVisualCmds = qfalse; // HZM coop [225] - never leak the mute past this entity

    cent->usageIndexLast = cent->currentState.usageIndex;
}
