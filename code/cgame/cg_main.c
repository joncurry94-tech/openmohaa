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
// Init functions for the cgame

#include "cg_local.h"
void CG_CoopFeedInit(void); // HZM coop [user 2026-10-05] event feed (cg_coopfeed.c)
#include <stddef.h> // HZM coop [SEC2] offsetof
#include "cg_parsemsg.h"
#include "cg_archive.h"
#include "cg_radar.h"
#include "../renderercommon/hzm_storm.h" // HZM coop [2026-09-28] storm darkness (cg_hzmstorm.c)

#ifdef _WIN32
#    include <windows.h>
#endif

clientGameImport_t        cgi;
static clientGameExport_t cge;

cvar_t       *paused;
cvar_t       *developer;
cg_t          cg;
cgs_t         cgs;
target_game_e cg_target_game = TG_INVALID;
int           cg_protocol;
centity_t     cg_entities[MAX_GENTITIES];

cvar_t *cg_animSpeed;
cvar_t *cg_debugAnim;
cvar_t *cg_debugAnimWatch;
cvar_t *cg_errorDecay;
cvar_t *cg_nopredict;
cvar_t *cg_showmiss;
cvar_t *cg_addMarks;
cvar_t *cg_maxMarks;
cvar_t *cg_viewsize;
cvar_t *cg_3rd_person;
cvar_t *cg_drawviewmodel;
cvar_t *cg_synchronousClients;
cvar_t *cg_stereoSeparation;
cvar_t *cg_stats;
cvar_t *cg_lagometer;
cvar_t *r_lerpmodels;
cvar_t *cg_cameraheight;
cvar_t *cg_cameradist;
cvar_t *cg_camerasideoffset; // HZM coop - third-person lateral camera offset (+ = right shoulder)
cvar_t *cg_cameraverticaldisplacement;
cvar_t *cg_camerascale;
cvar_t *cg_shadows;
cvar_t *cg_hidetempmodels;
cvar_t *cg_traceinfo;
cvar_t *cg_debugFootsteps;
cvar_t *cg_smoothClients;
cvar_t *cg_smoothClientsTime;
cvar_t *pmove_fixed;
cvar_t *pmove_msec;
cvar_t *cg_pmove_msec;
cvar_t *dm_playermodel;
cvar_t *dm_playergermanmodel;
cvar_t *cg_forceModel;
cvar_t *cg_animationviewmodel;
cvar_t *cg_hitmessages;
cvar_t *cg_acidtrip;
cvar_t *cg_hud;
cvar_t *cg_huddraw_force;
cvar_t *cg_drawsvlag;
cvar_t *cg_crosshair;
cvar_t *cg_crosshair_friend;
cvar_t *ui_crosshair;
cvar_t *vm_offset_max;
cvar_t *vm_offset_speed;
cvar_t *vm_sway_front;
cvar_t *vm_sway_side;
cvar_t *vm_sway_up;
cvar_t *vm_offset_air_front;
cvar_t *vm_offset_air_side;
cvar_t *vm_offset_air_up;
cvar_t *vm_offset_crouch_front;
cvar_t *vm_offset_crouch_side;
cvar_t *vm_offset_crouch_up;
cvar_t *vm_offset_rocketcrouch_front;
cvar_t *vm_offset_rocketcrouch_side;
cvar_t *vm_offset_rocketcrouch_up;
cvar_t *vm_offset_shotguncrouch_front;
cvar_t *vm_offset_shotguncrouch_side;
cvar_t *vm_offset_shotguncrouch_up;
cvar_t *vm_offset_vel_base;
cvar_t *vm_offset_vel_front;
cvar_t *vm_offset_vel_side;
cvar_t *vm_offset_vel_up;
cvar_t *vm_offset_upvel;
cvar_t *vm_lean_lower;
cvar_t *voiceChat;
cvar_t *cg_shadowscount;
cvar_t *cg_shadowdebug;
cvar_t *ui_timemessage;

//
// Added in OPM
//
cvar_t *cg_fov;
cvar_t *cg_cheats;
cvar_t *cg_adsZoom; // HZM coop - ADS: view-fov multiplier while RMB (secondary attack) is held
cvar_t *cg_adsGunZoom; // HZM coop - ADS: how much the view weapon zooms WITH the world (0=gun stays normal size, 1=gun zooms fully with the world but its rear can clip)
cvar_t *cg_adsPitch;   // HZM coop - ADS: pitch the view weapon (deg) so the rear aperture lines up with the front sight
cvar_t *cg_adsYaw;     // HZM coop - ADS: yaw the view weapon (deg) to angle the gun left/right for horizontal sight alignment
cvar_t *cg_adsRoll;    // HZM coop - ADS: roll the view weapon (deg) about the barrel axis to un-tilt the gun (standing)
cvar_t *cg_adsCrouchPitch; // HZM coop - ADS: EXTRA pitch (deg) applied only while crouched (crouch pose shifts the sights)
cvar_t *cg_adsCrouchYaw;   // HZM coop - ADS: EXTRA yaw (deg) applied only while crouched
cvar_t *cg_adsCrouchRoll;  // HZM coop - ADS: EXTRA roll (deg) applied only while crouched (un-tilt the crouch gun)
cvar_t *cg_adsShiftX;      // HZM coop - ADS: STANDING horizontal screen shift of the whole weapon view (+ right / - left); fed to renderer r_weaponshiftx
cvar_t *cg_adsShiftY;      // HZM coop - ADS: STANDING vertical screen shift (- up / + down); fed to renderer r_weaponshifty
cvar_t *cg_adsCrouchShiftX; // HZM coop - ADS: EXTRA horizontal screen shift added only while crouched (slide hands+gun toward centre)
cvar_t *cg_adsCrouchShiftY; // HZM coop - ADS: EXTRA vertical screen shift added only while crouched
cvar_t *cg_adsTune;        // HZM coop - ADS tuning workbench: 1 = dial the held gun via the global cg_ads* cvars + show a centre reticle and a live value readout
cvar_t *cg_adsMode;        // HZM coop - ADS workbench active param: 0=pitch (up/down) 1=yaw (left/right) 2=shift (slide hands+gun) 3=roll (crouch un-tilt)
cvar_t *cg_adsUp;      // HZM coop - ADS: raise the view weapon up toward the sights (units) while RMB held
cvar_t *cg_adsForward; // HZM coop - ADS: move the view weapon forward(+)/back(-) while RMB held
cvar_t *cg_adsRight;   // HZM coop - ADS: move the view weapon right(+)/left(-) while RMB held

/*
=================
CG_RegisterCvars
=================
*/
void CG_RegisterCvars(void)
{
    cvar_t *temp;

    cgi.Cvar_Get("g_subtitle", "0", CVAR_ARCHIVE);
    cg_viewsize                   = cgi.Cvar_Get("viewsize", "100", CVAR_ARCHIVE);
    cg_addMarks                   = cgi.Cvar_Get("cg_marks_add", "0", CVAR_ARCHIVE);
    cg_maxMarks                   = cgi.Cvar_Get("cg_marks_max", "256", CVAR_ARCHIVE | CVAR_LATCH);
    cg_animSpeed                  = cgi.Cvar_Get("cg_animspeed", "1", CVAR_CHEAT);
    cg_debugAnim                  = cgi.Cvar_Get("cg_debuganim", "0", CVAR_CHEAT);
    cg_debugAnimWatch             = cgi.Cvar_Get("cg_debuganimwatch", "0", CVAR_CHEAT);
    cg_errorDecay                 = cgi.Cvar_Get("cg_errordecay", "100", 0);
    cg_nopredict                  = cgi.Cvar_Get("cg_nopredict", "0", 0);
    cg_showmiss                   = cgi.Cvar_Get("cg_showmiss", "0", 0);
    cg_stats                      = cgi.Cvar_Get("cg_stats", "0", 0);
    cg_hidetempmodels             = cgi.Cvar_Get("cg_hidetempmodels", "0", 0);
    cg_synchronousClients         = cgi.Cvar_Get("g_synchronousClients", "0", 0);
    cg_stereoSeparation           = cgi.Cvar_Get("cg_stereosep", "0.4", CVAR_ARCHIVE);
    cg_lagometer                  = cgi.Cvar_Get("cg_lagometer", "0", 0);
    paused                        = cgi.Cvar_Get("paused", "0", 0);
    r_lerpmodels                  = cgi.Cvar_Get("r_lerpmodels", "1", 0);
    cg_3rd_person                 = cgi.Cvar_Get("cg_3rd_person", "0", 0);
    cg_drawviewmodel              = cgi.Cvar_Get("cg_drawviewmodel", "2", CVAR_ARCHIVE);
    cg_cameraheight               = cgi.Cvar_Get("cg_cameraheight", "18", CVAR_ARCHIVE);
    cg_cameradist                 = cgi.Cvar_Get("cg_cameradist", "120", CVAR_ARCHIVE);
    // HZM coop - third-person camera lateral offset: positive = over the RIGHT shoulder. Pair with a
    // smaller cg_cameradist for a tight over-the-shoulder feel. Default 18 = right shoulder.
    cg_camerasideoffset           = cgi.Cvar_Get("cg_camerasideoffset", "18", CVAR_ARCHIVE);
    cg_cameraverticaldisplacement = cgi.Cvar_Get("cg_cameraverticaldisplacement", "-2", CVAR_ARCHIVE);
    cg_camerascale                = cgi.Cvar_Get("cg_camerascale", "0.3", CVAR_ARCHIVE);
    cg_traceinfo                  = cgi.Cvar_Get("cg_traceinfo", "0", CVAR_ARCHIVE);
    cg_debugFootsteps             = cgi.Cvar_Get("cg_debugfootsteps", "0", CVAR_CHEAT);
    cg_smoothClients              = cgi.Cvar_Get("cg_smoothClients", "1", CVAR_ARCHIVE);
    cg_smoothClientsTime          = cgi.Cvar_Get("cg_smoothClientsTime", "100", CVAR_ARCHIVE);
    pmove_fixed                   = cgi.Cvar_Get("pmove_fixed", "0", 0);
    pmove_msec                    = cgi.Cvar_Get("pmove_msec", "8", 0);
    cg_pmove_msec                 = cgi.Cvar_Get("cg_pmove_msec", "8", 0);
    cg_shadows                    = cgi.Cvar_Get("cg_shadows", "0", CVAR_ARCHIVE);
    cg_shadowscount               = cgi.Cvar_Get("cg_shadowscount", "8", 0);
    cg_shadowdebug                = cgi.Cvar_Get("cg_shadowdebug", "0", 0);
    developer                     = cgi.Cvar_Get("developer", "0", 0);
    dm_playermodel                = cgi.Cvar_Get("dm_playermodel", "american_army", 3);
    dm_playergermanmodel          = cgi.Cvar_Get("dm_playergermanmodel", "german_wehrmacht_soldier", CVAR_ARCHIVE | CVAR_USERINFO);
    // HZM coop [user 08-06] bug-1508 - "Wuss.pk3" challenge marker. USERINFO only (no ARCHIVE - this
    // is a live derived value re-synced fresh every session by CG_SyncWussPk3Count, not something to
    // persist to disk). Registered here so it's already USERINFO-flagged before CG_SyncWussPk3Count's
    // first Cvar_Set, rather than relying on that call to implicitly create it with the right flags.
    cgi.Cvar_Get("coop_wussCount", "0", CVAR_USERINFO);
    cg_forceModel                 = cgi.Cvar_Get("cg_forceModel", "0", CVAR_ARCHIVE);
    cg_animationviewmodel         = cgi.Cvar_Get("cg_animationviewmodel", "0", CVAR_SYSTEMINFO);
    cg_hitmessages                = cgi.Cvar_Get("cg_hitmessages", "1", CVAR_ARCHIVE);
    cg_acidtrip                   = cgi.Cvar_Get("cg_acidtrip", "0", CVAR_CHEAT);
    cg_hud                        = cgi.Cvar_Get("cg_hud", "0", 0);
    cg_huddraw_force              = cgi.Cvar_Get("cg_huddraw_force", "0", CVAR_SAVEGAME);
    cg_drawsvlag                  = cgi.Cvar_Get("cg_drawsvlag", "1", CVAR_ARCHIVE);
    cg_crosshair                  = cgi.Cvar_Get("cg_crosshair", "textures/hud/crosshair", CVAR_ARCHIVE);
    if (cg_protocol >= PROTOCOL_MOHTA_MIN) {
        cg_crosshair_friend = cgi.Cvar_Get("cg_crosshair_friend", "textures/hud/crosshair_friend", CVAR_ARCHIVE);
    } else {
        // on 1.11 and below, fallback to standard crosshair
        // as it doesn't have crosshair_friend texture
        cg_crosshair_friend = cgi.Cvar_Get("cg_crosshair_friend", "textures/hud/crosshair", CVAR_ARCHIVE);
    }
    ui_crosshair                  = cgi.Cvar_Get("ui_crosshair", "0", CVAR_ARCHIVE); // HZM coop - default OFF (was autoexec seta, which stomped the archived user choice every launch)
    // HZM coop [user 2026-09-03] CINEMATIC CROSSHAIR HOLD. Registered EAGERLY and with flags 0.
    // Eagerly, because a server stufftext of "coop_cineHud 5" goes through Cvar_Command, which
    // only sets a cvar that ALREADY EXISTS - the same trap that made coop_voxCut a no-op on the
    // first frame it was needed (bug-2318, snd_dma_new.cpp:110). flags 0, because the two cvars
    // directly above are CVAR_ARCHIVE - the player's own saved preference - and nothing the
    // server can drive may ever be able to reach his config.
    cgi.Cvar_Get("coop_cineHud", "0", 0);
    vm_offset_max                 = cgi.Cvar_Get("vm_offset_max", "8.0", 0);
    vm_offset_speed               = cgi.Cvar_Get("vm_offset_speed", "8.0", 0);
    vm_sway_front                 = cgi.Cvar_Get("vm_sway_front", "0.1", 0);
    vm_sway_side                  = cgi.Cvar_Get("vm_sway_side", "0.005", 0);
    vm_sway_up                    = cgi.Cvar_Get("vm_sway_up", "0.003", 0);
    vm_offset_air_front           = cgi.Cvar_Get("vm_offset_air_front", "-3.0", 0);
    vm_offset_air_side            = cgi.Cvar_Get("vm_offset_air_side", "1.5", 0);
    vm_offset_air_up              = cgi.Cvar_Get("vm_offset_air_up", "-6.0", 0);
    vm_offset_crouch_front        = cgi.Cvar_Get("vm_offset_crouch_front", "-0.5", 0);
    vm_offset_crouch_side         = cgi.Cvar_Get("vm_offset_crouch_side", "2.25", 0);
    vm_offset_crouch_up           = cgi.Cvar_Get("vm_offset_crouch_up", "0.2", 0);
    vm_offset_rocketcrouch_front  = cgi.Cvar_Get("vm_offset_rocketcrouch_front", "0", 0);
    vm_offset_rocketcrouch_side   = cgi.Cvar_Get("vm_offset_rocketcrouch_side", "0", 0);
    vm_offset_rocketcrouch_up     = cgi.Cvar_Get("vm_offset_rocketcrouch_up", "0", 0);
    vm_offset_shotguncrouch_front = cgi.Cvar_Get("vm_offset_shotguncrouch_front", "-1", 0);
    vm_offset_shotguncrouch_side  = cgi.Cvar_Get("vm_offset_shotguncrouch_side", "2.5", 0);
    vm_offset_shotguncrouch_up    = cgi.Cvar_Get("vm_offset_shotguncrouch_up", "-1.1", 0);
    vm_offset_vel_base            = cgi.Cvar_Get("vm_offset_vel_base", "100", 0);
    vm_offset_vel_front           = cgi.Cvar_Get("vm_offset_vel_front", "-2.0", 0);
    vm_offset_vel_side            = cgi.Cvar_Get("vm_offset_vel_side", "1.5", 0);
    vm_offset_vel_up              = cgi.Cvar_Get("vm_offset_vel_up", "-4.0", 0);
    vm_offset_upvel               = cgi.Cvar_Get("vm_offset_upvel", "0.0025", 0);
    vm_lean_lower                 = cgi.Cvar_Get("vm_lean_lower", "0.1", 0);
    voiceChat                     = cgi.Cvar_Get("cg_voicechat", "1", 0);

    ui_timemessage = cgi.Cvar_Get("ui_timemessage", "", 0);

    // see if we are also running the server on this machine
    temp            = cgi.Cvar_Get("sv_running", "0", 0);
    cgs.localServer = temp->integer;

    //
    // Added in OPM
    //

    cg_fov = cgi.Cvar_Get("cg_fov", "80", CVAR_ARCHIVE);
    cg_cheats = cgi.Cvar_Get("cheats", "0", CVAR_USERINFO | CVAR_SERVERINFO | CVAR_LATCH);
    // HZM coop - ADS zoom factor (1.0 = off; 0.70 = ~30% zoom-in while RMB held). Applied in CG_CalcFov.
    cg_adsZoom = cgi.Cvar_Get("cg_adsZoom", "0.70", CVAR_ARCHIVE);
    // HZM coop - how much the view weapon zooms with the world during ADS (0..1). 0 keeps the gun a
    // constant size (rear never clips); 1 zooms the gun fully with the world (sights align but the rear
    // can fall off-screen). ~0.5 = a closer/bigger gun with the sights mostly aligned. Used by CG_CalcFov.
    cg_adsGunZoom = cgi.Cvar_Get("cg_adsGunZoom", "0.5", CVAR_ARCHIVE);
    // HZM coop - ADS iron-sight pitch (degrees). A screen shift (r_weaponshifty) moves the whole gun
    // uniformly so it cannot align the rear aperture with the front post; a small pitch about the grip
    // does. 0 = off; dial +/- a few degrees while aiming to line the sights up. Live-tunable.
    cg_adsPitch = cgi.Cvar_Get("cg_adsPitch", "0", CVAR_ARCHIVE);
    // HZM coop - ADS iron-sight YAW (degrees). Same idea as cg_adsPitch but horizontal: angles the gun
    // left/right so the front post centres in the rear ring sideways. 0 = off. Live-tunable.
    cg_adsYaw = cgi.Cvar_Get("cg_adsYaw", "0", CVAR_ARCHIVE);
    // HZM coop - ADS iron-sight ROLL (degrees). Rotates the gun about its barrel axis to un-tilt it
    // (standing). 0 = off. Crouch has its own EXTRA roll below (cg_adsCrouchRoll). Live-tunable.
    cg_adsRoll = cgi.Cvar_Get("cg_adsRoll", "0", CVAR_ARCHIVE);
    // HZM coop - crouch-only ADS correction. The crouch pose hunches the upper body and carries the gun
    // down/left/tilted off the standing sight line; these add EXTRA rotation about the grip (so the hands
    // stay attached) ONLY while ducked, to bring the crouched sight picture back onto the standing one.
    cg_adsCrouchPitch = cgi.Cvar_Get("cg_adsCrouchPitch", "0", CVAR_ARCHIVE);
    cg_adsCrouchYaw   = cgi.Cvar_Get("cg_adsCrouchYaw", "0", CVAR_ARCHIVE);
    cg_adsCrouchRoll  = cgi.Cvar_Get("cg_adsCrouchRoll", "0", CVAR_ARCHIVE);
    // HZM coop - ADS screen shift (slides the whole weapon view: hands + gun). Standing = cg_adsShiftX/Y;
    // crouch ADDS cg_adsCrouchShiftX/Y. cg_view feeds the combined result into the renderer's r_weaponshiftx/y
    // each frame (only consumed during ADS), so the renderer needs no change. + right/- left ; - up/+ down.
    cg_adsShiftX       = cgi.Cvar_Get("cg_adsShiftX", "0", CVAR_ARCHIVE);
    cg_adsShiftY       = cgi.Cvar_Get("cg_adsShiftY", "0", CVAR_ARCHIVE);
    cg_adsCrouchShiftX = cgi.Cvar_Get("cg_adsCrouchShiftX", "0", CVAR_ARCHIVE);
    cg_adsCrouchShiftY = cgi.Cvar_Get("cg_adsCrouchShiftY", "0", CVAR_ARCHIVE);
    // HZM coop - ADS tuning workbench. cg_adsTune 1: the gun you're holding is tuned LIVE by the global
    // cg_adsPitch/cg_adsYaw (+ r_weaponshiftx/y, + cg_adsCrouch* while crouched) instead of its baked
    // per-gun values, and a centre reticle + value readout are drawn. Dial it in, then bind a key to
    // "adssave" to dump the gun's stand+crouch values to the console.
    cg_adsTune = cgi.Cvar_Get("cg_adsTune", "0", CVAR_ARCHIVE);
    cg_adsMode = cgi.Cvar_Get("cg_adsMode", "0", CVAR_ARCHIVE);
    // HZM coop - ADS weapon position offset (raise the gun to the sights). Applied to the first-person
    // model every frame while RMB is held, so it works for ALL animations incl. full-auto (recoil plays
    // on top). Units are view-space (~1 unit = 1 inch). Live-tunable to dial in the sight picture.
    cg_adsUp      = cgi.Cvar_Get("cg_adsUp", "6", CVAR_ARCHIVE);
    cg_adsForward = cgi.Cvar_Get("cg_adsForward", "0", CVAR_ARCHIVE);
    cg_adsRight   = cgi.Cvar_Get("cg_adsRight", "-2", CVAR_ARCHIVE);
}
/*
===============
CG_UseLargeLightmaps

Added in 2.0
Returns true if the standard BSP file should be used, false if the smaller lightmap BSP file should be used
===============
*/
qboolean CG_UseLargeLightmaps(const char* mapName) {
	char buffer[MAX_QPATH];

	Com_sprintf(buffer, sizeof(buffer), "maps/%s_sml.bsp", mapName);

	if (cgi.FS_ReadFile(buffer, NULL, qtrue) == -1) {
		return qtrue;
	}

	return cgi.Cvar_Get("r_largemap", "0", 0)->integer;
}

/*
================
CG_RegisterSoundsForFile

Register the specified ubersound source file
================
*/
void CG_RegisterSoundsForFile(const char *name)
{
    int startTime;
    int endTime;

    Com_Printf("\n\n-----------PARSING '%s'------------\n", name);
    Com_Printf(
        "Any SetCurrentTiki errors means that tiki wasn't prefetched and tiki-specific sounds for it won't work. To "
        "fix prefetch the tiki. Ignore if you don't use that tiki on this level.\n"
    );

    startTime = cgi.Milliseconds();
    CG_Command_ProcessFile(name, qfalse, NULL);
    endTime = cgi.Milliseconds();

    Com_Printf("Parse/Load time: %f seconds.\n", (float)(endTime - startTime) / 1000.0);
    Com_Printf("-------------PARSING '%s' DONE---------------\n\n", name);
}

/*
=================
qsort_compare_strings

perform case-insensitive sorting
=================
*/
int qsort_compare_strings(const void *s1, const void *s2)
{
    return Q_stricmp(*(const char **)s1, *(const char **)s2);
}

/*
=================
CG_RegisterSounds

Called during a precache command
=================
*/
void CG_RegisterSounds(void)
{
    char **fileList;
    int    numFiles;
    int    i;

    fileList = cgi.FS_ListFilteredFiles("ubersound/", "scr", "*.scr", qfalse, &numFiles, qtrue);
    if (cg_target_game >= TG_MOHTA) {
        // Fixed in 2.0
        //  The behavior has changed, all aliases get cleared
        if (cgs.gametype != GT_SINGLE_PLAYER) {
            cgi.Alias_Clear();
        }
    } else {
        if (!cgs.localServer) {
            cgi.Alias_Clear();
        }
    }
    
    qsort(fileList, numFiles, sizeof(char *), &qsort_compare_strings);

    for (i = 0; i < numFiles; i++) {
        // Added in 2.0
        //  Since 2.0, all files in the ubersound folder
        //  are parsed
        CG_RegisterSoundsForFile(va("ubersound/%s", fileList[i]));
    }

    cgi.FS_FreeFileList(fileList);

    // HZM bug-3097: cg_view.c plays these by FILE NAME (cgi.S_StartLocalSound), so
    // they were first loaded from disk the moment they played - each one a "S_LoadSound" console
    // line and a disk hitch mid-fight (the ADS breath-hold and the brace toggle). Register them
    // here, while the client is still loading.
    cgi.S_RegisterSound("sound/coop_breath/breath_in.wav", qfalse);
    cgi.S_RegisterSound("sound/coop_breath/breath_out.wav", qfalse);
    cgi.S_RegisterSound("sound/coop_brace/brace_on.wav", qfalse);
    cgi.S_RegisterSound("sound/coop_brace/brace_off.wav", qfalse);
}

/*
================
CG_IsHandleUnique

Check if the model handle is unique
================
*/
static qboolean CG_IsHandleUnique(qhandle_t handle) {
    int i;
    int numRef;

    numRef = 0;
    for (i = 0; i < MAX_MODELS; i++) {
        if (cgs.model_draw[i] == handle) {
            numRef++;
            if (numRef >= 2) {
                return qfalse;
            }
        }
    }

    return qtrue;
}

/*
================
CG_ProcessConfigString
================
*/
void CG_ProcessConfigString(int num, qboolean modelOnly)
{
    const char* str;
    int i;

    str = CG_ConfigString(num);

    if (num >= CS_MODELS && num < CS_MODELS + MAX_MODELS) {
        qhandle_t hOldModel = cgs.model_draw[num - CS_MODELS];

        if (str && str[0] && !modelOnly) {
            qhandle_t hModel = cgi.R_RegisterServerModel(str);
            dtiki_t  *tiki;

            if (hModel != hOldModel) {
                if (hOldModel) {
                    assert(CG_IsHandleUnique(hOldModel));
                    cgi.R_UnregisterServerModel(hOldModel);
                }

                cgs.model_draw[num - CS_MODELS] = hModel;
                assert(CG_IsHandleUnique(hModel));
            }
            tiki = cgi.R_Model_GetHandle(hModel);
            if (tiki) {
                CG_ProcessCacheInitCommands(tiki);
            }

            CG_ServerModelLoaded(str, hModel);
        } else {
            // clear out the model
            if (hOldModel) {
                assert(CG_IsHandleUnique(hOldModel));
                cgi.R_UnregisterServerModel(hOldModel);
            }

            cgs.model_draw[num - CS_MODELS] = 0;

            if (!str || !str[0]) {
                CG_ServerModelUnloaded(hOldModel);
            }
        }
    }

    if (!modelOnly) {
        switch (num) {
        case CS_RAIN_DENSITY:
            cg.rain.density = atof(str);
            return;
        case CS_RAIN_SPEED:
            cg.rain.speed = atof(str);
            return;
        case CS_RAIN_SPEEDVARY:
            cg.rain.speed_vary = atoi(str);
            return;
        case CS_RAIN_SLANT:
            cg.rain.slant = atoi(str);
            return;
        case CS_RAIN_LENGTH:
            cg.rain.length = atof(str);
            return;
        case CS_RAIN_MINDIST:
            cg.rain.min_dist = atof(str);
            return;
        case CS_RAIN_WIDTH:
            cg.rain.width = atof(str);
            return;
        case CS_RAIN_SHADER:
            Q_strncpyz(cg.rain.currentShader, str, sizeof(cg.rain.currentShader));
            if (cg.rain.numshaders) {
                // Fixed in OPM
                //  not sure why some maps set a digit at the end...
                size_t len = strlen(cg.rain.currentShader);
                if (isdigit(cg.rain.currentShader[len - 1])) {
                    cg.rain.currentShader[len - 1] = 0;
                }
            }
            for (i = 0; i < cg.rain.numshaders; ++i) {
                Com_sprintf(cg.rain.shader[i], sizeof(cg.rain.shader[i]), "%s%i", cg.rain.currentShader, i);
            }
            if (!cg.rain.numshaders) {
                Q_strncpyz(cg.rain.shader[0], cg.rain.currentShader, sizeof(cg.rain.shader[0]));
            }
            return;
        case CS_RAIN_NUMSHADERS:
            cg.rain.numshaders = atoi(str);
            if (cg.rain.numshaders) {
                for (i = 0; i < cg.rain.numshaders; i++) {
                    Com_sprintf(cg.rain.shader[i], sizeof(cg.rain.shader[i]), "%s%i", cg.rain.currentShader, i);
                }
            }
            return;
        case CS_CURRENT_OBJECTIVE:
            // HZM coop [user 2026-09-13] top compass bar: a NEW current objective moves the bar's marker, so
            // it wakes the HUD fade - only while the bar is live, see CG_CompassBarObjectiveChanged. A re-push
            // of the same index is not activity.
            if (atoi(str) != cg.ObjectivesCurrentIndex) {
                CG_CompassBarObjectiveChanged();
            }
            cg.ObjectivesCurrentIndex = atoi(str);
            return;
        }

        if (num >= CS_OBJECTIVES && num < CS_OBJECTIVES + MAX_OBJECTIVES) {
            cobjective_t *objective = &cg.Objectives[num - CS_OBJECTIVES];
            int           newFlags  = atoi(Info_ValueForKey(str, "flags"));
            const char   *newText   = Info_ValueForKey(str, "text");
            // HZM coop - objective ACTUALLY changed -> bring the HUD chrome back (scripts re-push
            // identical objective strings; those must not count as activity for the HUD fade)
            if (objective->flags != newFlags || Q_stricmp(objective->text, newText)) {
                CG_HudFadeTouch();
            }
            objective->flags = newFlags;
            Q_strncpyz(objective->text, newText, sizeof(objective->text));
            // HZM coop [user 2026-09-13] top compass bar: keep the objective's "loc". It is STALE on maps that
            // steer the compass with set_objective_pos (that writes only level.m_vObjectiveLocation,
            // scriptthread.cpp), so the bar trusts it only for an exact distance, and only while its bearing
            // agrees with the live STAT_OBJECTIVECENTER. Read after the text is copied, so the static buffer
            // Info_ValueForKey hands back cannot matter.
            VectorClear(objective->loc);
            objective->hasLoc = qfalse;
            if (sscanf(Info_ValueForKey(str, "loc"), "%f %f %f", &objective->loc[0], &objective->loc[1], &objective->loc[2]) == 3
                && (objective->loc[0] || objective->loc[1] || objective->loc[2])) {
                objective->hasLoc = qtrue;
            }
        }

        switch (num) {
        case CS_MUSIC:
            cgi.MUSIC_NewSoundtrack(str);
            return;
        case CS_WARMUP:
            cg.matchStartTime = atoi(str);
            return;
        case CS_FOGINFO:
            cg.farclipOverride = 0;
            cg.farplaneColorOverride[0] = -1;
            cg.farplaneColorOverride[1] = -1;
            cg.farplaneColorOverride[2] = -1;
            CG_ParseFogInfo(str);
            CG_HzmLt_FogFilter(); // HZM coop [2026-09-27] lightning: hides the fallback fog pulses from a client that renders the strike
            return;
        case CS_SKYINFO:
            sscanf(str, "%f %d", &cg.sky_alpha, &cg.sky_portal);
            return;
        case CS_HZM_LIGHTNING: // HZM coop [2026-09-27] realistic lightning (cg_hzmlightning.c)
            CG_HzmLt_ConfigString(str);
            return;
        case CS_SERVERINFO:
            CG_ParseServerinfo();
            return;
        case CS_LEVEL_START_TIME:
            cgs.levelStartTime = atoi(str);
            return;
        case CS_VOTE_TIME:
            cgs.voteTime = atoi(str);
            cgs.voteRefreshed = qtrue;
            break;
        case CS_VOTE_STRING:
            Q_strncpyz(cgs.voteString, str, sizeof(cgs.voteString));
            break;
        case CS_VOTE_YES:
            cgs.numVotesYes = atoi(str);
            cgs.voteRefreshed = qtrue;
            break;
        case CS_VOTE_NO:
            cgs.numVotesNo = atoi(str);
            cgs.voteRefreshed = qtrue;
            break;
        case CS_VOTE_UNDECIDED:
            cgs.numUndecidedVotes = atoi(str);
            cgs.voteRefreshed = qtrue;
            break;
        case CS_MATCHEND:
            cgs.matchEndTime = atoi(str);
            return;
        }

        if (num >= CS_SOUNDS && num < CS_SOUNDS + MAX_SOUNDS) {
            size_t len = strlen(str);
            if (len) {
                qboolean streamed;
                char     buf[1024];
                Q_strncpyz(buf, str, sizeof(buf));
        
                streamed     = buf[len - 1] != '0';
                buf[len - 1] = 0;
                if (buf[0] != '*') {
                    cgs.sound_precache[num - CS_SOUNDS] = cgi.S_RegisterSound(buf, streamed);
                }
            }
        } else if (num >= CS_LIGHTSTYLES && num < CS_LIGHTSTYLES + MAX_LIGHTSTYLES) {
            CG_SetLightStyle(num - CS_LIGHTSTYLES, str);
        } else if (num >= CS_PLAYERS && num < CS_PLAYERS + MAX_CLIENTS) {
            const char *value;
        
            value = Info_ValueForKey(str, "name");
            if (value) {
                strncpy(cg.clientinfo[num - CS_PLAYERS].name, value, sizeof(cg.clientinfo[num - CS_PLAYERS].name));
            } else {
                strncpy(
                    cg.clientinfo[num - CS_PLAYERS].name, "UnnamedSoldier", sizeof(cg.clientinfo[num - CS_PLAYERS].name)
                );
            }
        
            value = Info_ValueForKey(str, "team");
            if (value) {
                cg.clientinfo[num - CS_PLAYERS].team = atoi(value);
            } else {
                cg.clientinfo[num - CS_PLAYERS].team = TEAM_NONE;
            }
        }
    }
}

//===================================================================================

/*
=================
CG_PrepRefresh

Call before entering a new level, or after changing renderers
This function may execute for a couple of minutes with a slow disk.
=================
*/
void CG_PrepRefresh(void)
{
    int i;

    memset(&cg.refdef, 0, sizeof(cg.refdef));

    cgi.R_LoadWorldMap(cgs.mapname);

    // register the inline models
    cgs.numInlineModels = cgi.CM_NumInlineModels();

    for (i = 1; i < cgs.numInlineModels; i++) {
        char   name[10];
        vec3_t mins, maxs;
        int    j;

        Com_sprintf(name, sizeof(name), "*%i", i);
        cgs.inlineDrawModel[i] = cgi.R_RegisterModel(name);
        cgi.R_ModelBounds(cgs.inlineDrawModel[i], mins, maxs);

        for (j = 0; j < 3; j++) {
            cgs.inlineModelMidpoints[i][j] = mins[j] + 0.5 * (maxs[j] - mins[j]);
        }
    }
    // register media shaders
    cgs.media.shadowMarkShader         = cgi.R_RegisterShader("markShadow");
    cgs.media.footShadowMarkShader     = cgi.R_RegisterShader("footShadow");
    cgs.media.wakeMarkShader           = cgi.R_RegisterShader("ripple.spr");
    cgs.media.lagometerShader          = cgi.R_RegisterShaderNoMip("gfx/2d/blank");
    cgs.media.levelExitShader          = cgi.R_RegisterShaderNoMip("textures/menu/exit");
    cgs.media.pausedShader             = cgi.R_RegisterShaderNoMip("textures/menu/paused");
    cgs.media.backTileShader           = cgi.R_RegisterShader("gfx/2d/backtile");
    cgs.media.zoomOverlayShader        = cgi.R_RegisterShaderNoMip("textures/hud/zoomoverlay");
    cgs.media.kar98TopOverlayShader    = cgi.R_RegisterShaderNoMip("textures/hud/kartop.tga");
    cgs.media.kar98BottomOverlayShader = cgi.R_RegisterShaderNoMip("textures/hud/karbottom.tga");
    cgs.media.binocularsOverlayShader  = cgi.R_RegisterShaderNoMip("textures/hud/binocularsoverlay");
    cgs.media.hudDrawFont              = cgi.R_LoadFont("verdana-14");
    cgs.media.attackerFont             = cgi.R_LoadFont("facfont-20");
    cgs.media.objectiveFont            = cgi.R_LoadFont("facfont-20"); // was courier-16 before 2.0
    cgs.media.objectivesBackShader     = cgi.R_RegisterShaderNoMip("textures/hud/objectives_backdrop");
    cgs.media.checkedBoxShader         = cgi.R_RegisterShaderNoMip("textures/objectives/filledbox");
    cgs.media.uncheckedBoxShader       = cgi.R_RegisterShaderNoMip("textures/objectives/emptybox");

    // go through all the configstrings and process them
    for (i = CS_SYSTEMINFO + 1; i < MAX_CONFIGSTRINGS; i++) {
        CG_ProcessConfigString(i, qfalse);
    }
}

//===========================================================================

/*
=================
CG_ConfigString
=================
*/
const char *CG_ConfigString(int index)
{
    if (index < 0 || index >= MAX_CONFIGSTRINGS) {
        cgi.Error(ERR_DROP, "CG_ConfigString: bad index: %i", index);
    }
    return cgs.gameState.stringData + cgs.gameState.stringOffsets[index];
}

//==================================================================

void CG_GetRendererConfig(void)
{
    // get the rendering configuration from the client system
    cgi.GetGlconfig(&cgs.glconfig);
    cgs.screenXScale = cgs.glconfig.vidWidth / 640.0;
    cgs.screenYScale = cgs.glconfig.vidHeight / 480.0;
    cgi.UI_GetHighResolutionScale(cgs.uiHiResScale);
}

/*
======================
CG_GameStateReceived

Displays the info screen while loading media
======================
*/
void CG_GameStateReceived(void)
{
    const char *s;
    int checksum;

    // clear everything
    memset(&cg, 0, sizeof(cg));
    memset(cg_entities, 0, sizeof(cg_entities));

    // clear the light styles
    CG_ClearLightStyles();

    // get the rendering configuration from the client system
    CG_GetRendererConfig();

    // get the gamestate from the client system
    cgi.GetGameState(&cgs.gameState);

    // check version
    s = CG_ConfigString(CS_GAME_VERSION);
    if (strcmp(s, GAME_VERSION)) {
        cgi.Error(ERR_DROP, "Client/Server game mismatch: %s/%s", GAME_VERSION, s);
    }

    s                  = CG_ConfigString(CS_LEVEL_START_TIME);
    cgs.levelStartTime = atoi(s);

    CG_ParseServerinfo();

    // load the new map
    cgi.CM_LoadMap(cgs.mapname, &checksum);
    if (cgs.useMapChecksum && checksum != cgs.mapChecksum && cgs.gametype != GT_SINGLE_PLAYER) {
        cgi.Error(ERR_DROP, "Client/Server map checksum mismatch: %x/%x", checksum, cgs.mapChecksum);
    }

    CG_InitMarks();
    CG_InitFootprints(); // HZM coop footprints

    CG_RegisterSounds();

    CG_PrepRefresh();

    CG_InitializeSpecialEffectsManager();

    CG_InitializeObjectives();
}

/*
======================
CG_ServerRestarted

The server has beeen restarted, adjust our cgame data accordingly
======================
*/
void CG_ServerRestarted(void)
{
    const char *s;

    s                  = CG_ConfigString(CS_LEVEL_START_TIME);
    cgs.levelStartTime = atoi(s);

    CG_ParseServerinfo();

    cg.thisFrameTeleport = qtrue;
    // free up any temp models currently spawned
    CG_RestartCommandManager();
    // get rid of left over decals from the last game
    CG_InitMarks();
    CG_InitFootprints(); // HZM coop footprints
    // clear all the swipes
    CG_ClearSwipes();
    // Reset tempmodels
    CG_ResetTempModels();
    // Reset resources
    CG_ResetVSSSources();
    // Reset objectives
    CG_InitializeObjectives();
}

/*
=================
HZM coop [bug-3048] THE PER-MAP GRADE SURVIVES A CGAME RELOAD THAT KEEPS THE LEVEL

vid_restart - and the graphics "Apply now" button, which runs one - unloads and reloads this DLL while the client
keeps its gamestate and stays on the same level. CG_Shutdown and CG_Init both clear coop_mapGrade, which is right
for a level change or an MP server and wrong here, and the coop server sends the grade ONCE per player
(coop_mod/mapgrade.scr coop_mapgrade_publish, flags["coop_mgSent"]). So after every vid_restart the layer sat at
identity until the next map: contrast 1.08 -> 1, saturation 0.88 -> 1, lifted shadows (gfx harness, bug-3048).

Fix, client-only, nothing on the wire changes: CG_Shutdown parks the live grade in cg_hzmGradeKept, keyed on the
LEVEL INSTANCE "<sv_serverid>.<level start time>@<serverinfo mapname>". CG_Init, once the gamestate is parsed, hands
it back only if the key still matches. sv_serverid (the server's frame time at SV_SpawnServer) is re-rolled by every
map load and map restart and is untouched by a vid_restart (cl.gameState survives it), so a parked grade does not
cross into another level; reaching another server's level would need the same map AND the same millisecond ids, and
even then it is only a clamped colour grade for that one level. The serverinfo mapname is used rather than
cgs.mapname, which carries the client's own _sml / r_largemap choice. The first CG_CoopMapGradeThink after the reload
re-applies it: its statics start at "\x01" in the freshly loaded DLL (CL_ShutdownCGame -> Sys_UnloadCGame; where a
platform kept the DLL mapped, the restore would simply not show - the old behaviour, never a wrong grade). The park is
one-shot - the next CG_Init consumes it whether it matches or not.

Switch: cg_hzmGradeKeep, flags 0 (never saved - TRAPS T7). -1 or "" = HZM_GRADEKEEP_AUTO, 0 = the old behaviour
(cleared on every reload), 1 = keep. Both cvars are registered in CG_Init before any server command is processed,
so the SEC2 set filter refuses a server set of either (they are registered, non-coop_, not user-created).
=================
*/
#define HZM_GRADEKEEP_AUTO 1

static qboolean CG_HZM_GradeKeepOn(void)
{
    const cvar_t *cv = cgi.Cvar_Get("cg_hzmGradeKeep", "-1", 0);

    if (!cv->string[0] || cv->integer < 0) {
        return HZM_GRADEKEEP_AUTO ? qtrue : qfalse;
    }
    return cv->integer ? qtrue : qfalse;
}

// the level instance the loaded gamestate belongs to; "" when there is none (no gamestate, no serverid, no map)
static void CG_HZM_GradeKeepKey(char *out, int size)
{
    char sid[64];
    char map[MAX_QPATH];

    // copied out at once: Info_ValueForKey hands back one of two rotating static buffers
    Q_strncpyz(sid, Info_ValueForKey(CG_ConfigString(CS_SYSTEMINFO), "sv_serverid"), sizeof(sid));
    Q_strncpyz(map, Info_ValueForKey(CG_ConfigString(CS_SERVERINFO), "mapname"), sizeof(map));
    out[0] = 0;
    if (sid[0] && map[0]) {
        Com_sprintf(out, size, "%s.%s@%s", sid, CG_ConfigString(CS_LEVEL_START_TIME), map);
    }
}

// CG_Shutdown: park the live grade before the identity reset clears it
static void CG_HZM_GradeKeepPark(void)
{
    char        key[MAX_QPATH + 96];
    const char *grade = cgi.Cvar_Get("coop_mapGrade", "", 0)->string;

    CG_HZM_GradeKeepKey(key, sizeof(key));
    if (CG_HZM_GradeKeepOn() && key[0] && grade[0]) {
        cgi.Cvar_Set("cg_hzmGradeKept", va("%s|%s", key, grade));
    } else {
        cgi.Cvar_Set("cg_hzmGradeKept", "");
    }
}

// CG_Init, after CG_GameStateReceived: hand the parked grade back if this reload kept the level
static void CG_HZM_GradeKeepRestore(void)
{
    char        key[MAX_QPATH + 96];
    char        kept[MAX_STRING_CHARS];
    const char *grade;
    size_t      keyLen;

    Q_strncpyz(kept, cgi.Cvar_Get("cg_hzmGradeKept", "", 0)->string, sizeof(kept));
    if (!kept[0]) {
        return;
    }
    cgi.Cvar_Set("cg_hzmGradeKept", ""); // one-shot

    CG_HZM_GradeKeepKey(key, sizeof(key));
    keyLen = strlen(key);
    // an exact key prefix, then '|', then a non-empty grade (no scan for '|', so a '|' anywhere cannot mis-split it)
    if (!CG_HZM_GradeKeepOn() || !keyLen || strncmp(kept, key, keyLen) || kept[keyLen] != '|' || !kept[keyLen + 1]) {
        cgi.Printf("^~^~^ HZM MAPGRADE keep: parked grade dropped (level changed) now=%s\n", keyLen ? key : "-");
        return;
    }
    grade = kept + keyLen + 1;
    cgi.Cvar_Set("coop_mapGrade", grade);
    cgi.Printf("^~^~^ HZM MAPGRADE keep: restored after cgame reload %s grade=%s\n", key, grade);
}

/*
=================
CG_Init

Called after every level change or subsystem restart
=================
*/
void CG_Init(clientGameImport_t *imported, int serverMessageNum, int serverCommandSequence, int clientNum)
{
    // HZM coop - ragdoll: renderer table must start clean (plan section-5)
    if (imported->R_ClearAllRagdolls) {
        imported->R_ClearAllRagdolls();
    }

    // HZM coop [SEC2] an exe older than CGAME_IMPORT_API_VERSION_STUFFSERVER never stamped apiversion and
    // its clientGameImport_t ends before Cmd_StuffServer: copy only what it has.
    if (imported->apiversion >= CGAME_IMPORT_API_VERSION_STUFFSERVER) {
        cgi = *imported;
    } else {
        memcpy(&cgi, imported, offsetof(clientGameImport_t, Cmd_StuffServer));
        cgi.Cmd_StuffServer = NULL;
    }

    cg_protocol = cgi.Cvar_Get("com_protocol", "", 0)->integer;
    cg_target_game = (target_game_e)cgi.Cvar_Get("com_target_game", "0", 0)->integer;

    // HZM 2026-08-05 - deploy-truth fingerprint (sweep detector rank 4); must agree with the
    // game module's FPRINT line or the deployed pair is mismatched.
    cgi.Printf("^~^~^ FPRINT cgame %s %s ENTBITS=%d MAX_SOUNDS=%d\n", __DATE__, __TIME__, GENTITYNUM_BITS, MAX_SOUNDS);

    CG_InitCGMessageAPI(&cge);
    CG_InitScoresAPI(&cge);

    memset(&cg, 0, sizeof(cg));
    memset(&cgs, 0, sizeof(cgs));
    // clear fog values
    cg.farclipOverride = 0;
    cg.farplaneColorOverride[0] = -1;
    cg.farplaneColorOverride[1] = -1;
    cg.farplaneColorOverride[2] = -1;

    cg.clientNum              = clientNum;
    cgs.processedSnapshotNum  = serverMessageNum;
    cgs.serverCommandSequence = serverCommandSequence;

    // HZM (bug-1202): clear every screen-effect signal cgame PUBLISHES to the renderer.
    // These are one-way per-frame values - cgame writes them, renderergl2's post-process chain
    // reads them, and nothing else ever resets them. They are published from CG_CalcFov(), which
    // is skipped entirely on snapshot loss (cg_view.c), during cinematics, and at the main menu -
    // so whatever value was live when the connection dropped stays live forever, and the effect
    // never goes away. Zeroing them here means a map change, reconnect or fresh connect always
    // starts from a clean screen. coop_dbnoView is included because it forces the downed
    // bleed-out view; it was also CVAR_ARCHIVE until now, so crashing while downed persisted a
    // forced max-injury effect to disk.
    {
        static const char *const hzmClearFx[] = {
            "r_ppHeat", "r_ppSuppress", "r_ppHit", "r_ppRainWet", "coop_dbnoView", "coop_medkitView",
            // [2026-09-27] rain wetness (cg_view.c CG_HZMWorldWetness): the world film + puddle levels
            "r_hzmWetNow", "r_hzmPuddleNow",
            // [2026-09-03] bug-1202 again, with a member nobody enumerated. r_ppBlood is
            // published from CG_CalcFov exactly like every name above it, and coop_lensBlood is
            // the script-poked input it consumes. Without these, dropping out mid-ramp with 0.85
            // on the glass leaves the blood welded to the screen. See the companion change in
            // cg_view.c: zeroing the cvar here is not sufficient on its own.
            "r_ppBlood", "coop_lensBlood",
            // bug-1307: the scripted-suppression floor and one-shot bump. Without these a
            // disconnect mid-set-piece leaves a permanently forced blur - bug-1202 again.
            "coop_suppHold", "coop_suppBump",
            // [2026-08-21] coop_vaultView is a COUNTER, so a stale value is not a stuck view -
            // but zeroing it on a fresh connect keeps the client edge-detector in step with a
            // server that starts its own counter at 0 again.
            "coop_vaultView",
            // [user 2026-09-03] the cinematic crosshair hold. flags 0, so it can never reach a
            // config - but it CAN survive a map change inside one session, and the "0" written
            // here is read by CG_CoopCineHudActive as an explicit release on the first CG_Draw2D
            // of the new map. Same reasoning as coop_voxCut's self-heal in S_BeginRegistration.
            "coop_cineHud"
        };
        int i;
        for (i = 0; i < (int)(sizeof(hzmClearFx) / sizeof(hzmClearFx[0])); i++) {
            cgi.Cvar_Set(hzmClearFx[i], "0");
        }
        // health fraction is inverted: 1.0 = full health = no effect
        cgi.Cvar_Set("r_ppHealthFrac", "1");
        // HZM coop [2026-09-27] realistic lightning: registers its cvars and clears r_hzmLtState (vet V10), before any
        // configstring of the new gamestate is processed
        CG_HzmLt_Init();
        CG_CoopFeedInit(); // HZM coop [user 2026-10-05] event feed: cvars + the cg_hzmFeed userinfo capability
        // [user 2026-09-06, bug-2507] the drowning air ramp is inverted the same way: 1.0 = full
        // air = no effect. coop_uwAir is the server-stuffed input, r_ppUnderwaterAir the
        // cgame-eased publish (cg_view.c CG_CalcFov) - a server dying mid-ramp must not leave
        // the next map's water throbbing at 5% air.
        cgi.Cvar_Set("coop_uwAir", "1");
        cgi.Cvar_Set("r_ppUnderwaterAir", "1");
    }

    // HZM coop [per-map grade] reset the renderer PER-MAP grade LAYER to identity on every cgame
    // load, so a coop server's grade never leaks into the next map or into an MP server (the layer
    // cvars are non-archived and persist within one client session). coop_mapGrade is the packed
    // server input; clearing it means an MP server that never publishes carries no coop grade. The
    // opt-out r_ppMapGradeOn is registered here (archived, default 1) so it exists as a real,
    // non-coop_ cvar BEFORE any server command can try to create it - which is what keeps a hostile
    // server from flipping the player's choice (the SEC2 set-variable filter refuses server sets of a
    // registered non-coop_, non-user-created cvar). See cg_view.c CG_CoopMapGradeThink.
    cgi.Cvar_Set("r_ppMapExposure",   "1");
    cgi.Cvar_Set("r_ppMapContrast",   "1");
    cgi.Cvar_Set("r_ppMapSaturation", "1");
    cgi.Cvar_Set("r_ppMapTemp",       "0");
    cgi.Cvar_Set("coop_mapGrade",     "");
    cgi.Cvar_Get("r_ppMapGradeOn",    "1", CVAR_ARCHIVE);
    // HZM coop [bug-3048] register the grade-keep switch and its park slot (see CG_HZM_GradeKeepOn). Cvar_Get only:
    // cg_hzmGradeKept must NOT be cleared here - it carries a vid_restart's grade into this very CG_Init.
    cgi.Cvar_Get("cg_hzmGradeKeep",   "-1", 0);
    cgi.Cvar_Get("cg_hzmGradeKept",   "", 0);

    // HZM-MP-BEGIN(mp_armory_session)
    // E2 session flag: zeroed on every cgame init so an MP session from a previous connection never
    // bleeds into the next. The server stuffs "set coop_mp_session 1" when mp.scr starts.
    cgi.Cvar_Set("coop_mp_session", "0");
    // [bug-3485] the pinned enemies-remaining line (cg_coopfeed.c) never survives a connect or a map load
    cgi.Cvar_Set("coop_ecLine", "");
    // HZM-MP-END(mp_armory_session)

    // HZM-MP-BEGIN(mp_kit_userinfo)
    // E1 [MP armories slice 2] - register the carried default kit cvars as USERINFO|ARCHIVE so a
    // player's MP weapon picks travel with them across servers via the userinfo string. Six slots
    // per side (Allied k1..k6, Axis k1..k6). The armory commit cfgs seta these; the server reads
    // them back with info_valueforkey on connect to apply the carried kit with no marker round-trip.
    // Precedent: coop_pin1..5 in cl_main.cpp:4198-4202 (same flags, same pattern).
    // About 170 of ~800 free userinfo bytes (progression 2.2).
    {
        static const char *const kKitCvars[12] = {
            "coop_mpa_k1", "coop_mpa_k2", "coop_mpa_k3",
            "coop_mpa_k4", "coop_mpa_k5", "coop_mpa_k6",
            "coop_mpx_k1", "coop_mpx_k2", "coop_mpx_k3",
            "coop_mpx_k4", "coop_mpx_k5", "coop_mpx_k6"
        };
        int ki;
        for (ki = 0; ki < 12; ki++) {
            cgi.Cvar_Get(kKitCvars[ki], "", CVAR_ARCHIVE | CVAR_USERINFO);
        }
    }
    // HZM-MP-END(mp_kit_userinfo)

    // HZM-MP-BEGIN(mp_prog_blob)
    // Slice 3 [MP progression, arch A] - the carried progression blob: a signed, compact snapshot of
    // the player's MP counters. USERINFO|ARCHIVE so it travels between servers and persists across
    // sessions exactly like the kit cvars above; the server reads it with info_valueforkey on connect,
    // verifies its mp_sign tag, and pushes an updated value back with `seta coop_mpProgBlob ...`. ~90
    // bytes, well inside the remaining userinfo budget. Same flags/pattern as coop_pin1..5.
    cgi.Cvar_Get("coop_mpProgBlob", "", CVAR_ARCHIVE | CVAR_USERINFO);
    // HZM-MP-END(mp_prog_blob)

    // HZM-MP-BEGIN(mp_cos_userinfo)
    // MP cosmetics carry: the player's chosen skin/helmet/glove id per side, USERINFO|ARCHIVE so the
    // appearance choices travel between servers and persist across sessions exactly like the kit cvars.
    // The armory option cfgs seta these; the server reads them with info_valueforkey on spawn and applies
    // the cosmetic (mp_cosmetics.scr). Same flags/pattern as coop_mpProgBlob / the kit cvars above.
    {
        static const char *const kCosCvars[6] = {
            "coop_mpa_cosSkin", "coop_mpa_cosHelm", "coop_mpa_cosGlove",
            "coop_mpx_cosSkin", "coop_mpx_cosHelm", "coop_mpx_cosGlove"
        };
        int ci;
        for (ci = 0; ci < 6; ci++) {
            cgi.Cvar_Get(kCosCvars[ci], "", CVAR_ARCHIVE | CVAR_USERINFO);
        }
    }
    // HZM-MP-END(mp_cos_userinfo)

    CG_RegisterCvars();
    CG_CompassBarInit(); // HZM coop [user 2026-09-13] top compass bar: prefs registered, session flag + band reset to 0

    L_InitEvents();

    // init swapping for endian conversion
    Swap_Init();

    CG_InitializeCommandManager();

    CG_GameStateReceived();

    // HZM coop [bug-3048] a vid_restart keeps the level: hand back the grade CG_Shutdown parked (needs the parsed
    // gamestate for its key, hence after CG_GameStateReceived). CG_CoopMapGradeThink applies it on the first frame.
    CG_HZM_GradeKeepRestore();

    // HZM coop [2026-09-28] storm darkness: registers, clears the renderer state and takes back a keyframe a
    // vid_restart parked for this same level (needs the parsed gamestate for its key, like the grade keep above)
    CG_HzmStorm_Init();

    CG_InitConsoleCommands();

    cg.vEyeOffsetMax[0]         = 40.0f;
    cg.vEyeOffsetMax[1]         = 45.0f;
    cg.vEyeOffsetMax[2]         = 60.0f;
    cg.fEyeOffsetFrac           = 0.1f;
    cg.fCurrentViewHeight       = 0.0f;
    cg.fCurrentViewBobPhase     = 0.0f;
    cg.fCurrentViewBobAmp       = 0.0f;
    cg.pLastPlayerWorldModel    = NULL;
    cg.pPlayerFPSModel          = NULL;
    cg.hPlayerFPSModelHandle    = 0;
    cg.pAlliedPlayerModel       = NULL;
    cg.hAlliedPlayerModelHandle = 0;
    cg.pAxisPlayerModel         = NULL;
    cg.hAxisPlayerModelHandle   = 0;
    cg.bFPSOnGround             = qtrue;

    // Pop the stats UI screen menu
    cgi.UI_HideMenu("StatsScreen", 1);

    // Scoreboard setup
    CG_PrepScoreBoardInfo();
    cgi.UI_HideScoreBoard();

    // HUD setup
    CG_RefreshHudDrawElements();
    cgi.Cmd_Execute(EXEC_NOW, "ui_hud 1\n");
}

/*
=================
CG_Shutdown

Called before every level change or subsystem restart
=================
*/
void CG_Shutdown(void)
{
    L_ShutdownEvents();
    // Shutdown radar
    cgi.CL_InitRadar(NULL, NULL, -1);

    // HZM coop - free cam: release the mouse capture so the client input layer can never be left
    // orbiting (viewangles frozen) across a level change / cgame reload
    cgi.Cvar_Set("cg_freecamCapture", "0");

    // HZM coop [bug-3048] park the live grade, keyed on this level instance, before the reset below clears it.
    // CG_Init hands it back only when the reload kept the level (vid_restart); any other load discards it.
    // HZM coop [2026-09-28] storm darkness: park the live keyframe (vid_restart), then clear coop_storm and the
    // renderer state so nothing outlives the level
    CG_HzmStorm_Shutdown();

    CG_HZM_GradeKeepPark();

    // HZM coop [per-map grade] the per-map grade LAYER never outlives the level: reset to identity
    // and clear the packed input, so whatever loads next - an MP map included - starts ungraded
    // until a coop server republishes coop_mapGrade. Mirrors the CG_Init reset above.
    cgi.Cvar_Set("r_ppMapExposure",   "1");
    cgi.Cvar_Set("r_ppMapContrast",   "1");
    cgi.Cvar_Set("r_ppMapSaturation", "1");
    cgi.Cvar_Set("r_ppMapTemp",       "0");
    cgi.Cvar_Set("coop_mapGrade",     "");

    // HZM coop [user 2026-09-13] top compass bar: the coop session flag and the published band never outlive
    // the level, so whatever loads next - an MP map included - starts with the stock ring and DM box
    CG_CompassBarShutdown();

    // some mods may need to do cleanup work here,
    // like closing files or archiving session data

    // hide the stats screen
    cgi.UI_HideMenu("StatsScreen", qtrue);

    // reset the scoreboard
    CG_PrepScoreBoardInfo();
    cgi.UI_HideScoreBoard();
}

int CG_GetParent(int entnum)
{
    return cg_entities[entnum].currentState.parent;
}

float CG_GetObjectiveAlpha()
{
    return cg.ObjectivesCurrentAlpha;
}

/*
================
GetCGameAPI

The only exported function from this module
================
*/
clientGameExport_t *GetCGameAPI(void)
{
    cge.CG_Init                     = CG_Init;
    cge.CG_DrawActiveFrame          = CG_DrawActiveFrame;
    cge.CG_Shutdown                 = CG_Shutdown;
    cge.CG_ConsoleCommand           = CG_ConsoleCommand;
    cge.CG_GetRendererConfig        = CG_GetRendererConfig;
    cge.CG_Draw2D                   = CG_Draw2D;
    cge.CG_EyePosition              = CG_EyePosition;
    cge.CG_EyeOffset                = CG_EyeOffset;
    cge.CG_EyeAngles                = CG_EyeAngles;
    cge.CG_SensitivityScale         = CG_SensitivityScale;
    cge.CG_RefreshHudDrawElements   = CG_RefreshHudDrawElements;
    cge.CG_HudDrawShader            = CG_HudDrawShader;
    cge.CG_HudDrawFont              = CG_HudDrawFont;
    cge.CG_PermanentMark            = CG_PermanentMark;
    cge.CG_PermanentTreadMarkDecal  = CG_PermanentTreadMarkDecal;
    cge.CG_PermanentUpdateTreadMark = CG_PermanentUpdateTreadMark;
    cge.CG_Command_ProcessFile      = CG_Command_ProcessFile;
    cge.CG_ProcessInitCommands      = CG_ProcessInitCommands;
    cge.CG_EndTiki                  = CG_EndTiki;
    cge.CG_GetParent                = CG_GetParent;
    cge.CG_GetObjectiveAlpha        = CG_GetObjectiveAlpha;
    cge.CG_WeaponCommandButtonBits  = CG_WeaponCommandButtonBits;
    cge.CG_CheckCaptureKey          = CG_CheckCaptureKey;
    cge.CG_ReadNonPVSClient         = CG_ReadNonPVSClient;
    cge.CG_UpdateRadar              = CG_UpdateRadar;
    cge.CG_SaveStateToBuffer        = CG_SaveStateToBuffer;
    cge.CG_LoadStateToBuffer        = CG_LoadStateToBuffer;
    cge.CG_CleanUpTempModels        = CG_CleanUpTempModels;

    // FIXME
    //cge.profStruct = NULL;

    return &cge;
}

/*
=====================
CG_DrawActive

Perform all drawing needed to completely fill the screen
=====================
*/
void CG_DrawActive(stereoFrame_t stereoView)
{
    float  separation;
    vec3_t baseOrg;

    switch (stereoView) {
    case STEREO_CENTER:
        separation = 0;
        break;
    case STEREO_LEFT:
        separation = -cg_stereoSeparation->value / 2;
        break;
    case STEREO_RIGHT:
        separation = cg_stereoSeparation->value / 2;
        break;
    default:
        separation = 0;
        cgi.Error(ERR_DROP, "CG_DrawActive: Undefined stereoView");
    }

    // clear around the rendered view if sized down
    CG_TileClear();

    // offset vieworg appropriately if we're doing stereo separation
    VectorCopy(cg.refdef.vieworg, baseOrg);
    if (separation != 0) {
        VectorMA(cg.refdef.vieworg, -separation, cg.refdef.viewaxis[1], cg.refdef.vieworg);
    }

    // draw 3D view
    cgi.R_RenderScene(&cg.refdef);

    // restore original viewpoint if running stereo
    if (separation != 0) {
        VectorCopy(baseOrg, cg.refdef.vieworg);
    }
}

#ifndef CGAME_HARD_LINKED
// this is only here so the functions in q_shared.c and bg_*.c can link (FIXME)

void Com_Error(int level, const char *error, ...)
{
    va_list argptr;
    char    text[1024];

    va_start(argptr, error);
    Q_vsnprintf(text, sizeof(text), error, argptr);
    va_end(argptr);

    cgi.Error(level, "%s", text);
}

void Com_Printf(const char *msg, ...)
{
    va_list argptr;
    char    text[1024];

    va_start(argptr, msg);
    Q_vsnprintf(text, sizeof(text), msg, argptr);
    va_end(argptr);

    cgi.Printf("%s", text);
}

#endif

void CG_ParseFogInfo_ver_15(const char *str)
{
    sscanf(
        str,
        "%d %f %f %f %f %f %f %f %d %f %f %f %f",
        &cg.farplane_cull,
        &cg.farplane_distance,
        &cg.farplane_bias,
        &cg.skyboxFarplane,
        &cg.skyboxSpeed,
        &cg.farplane_color[0],
        &cg.farplane_color[1],
        &cg.farplane_color[2],
        &cg.renderTerrain,
        &cg.farclipOverride,
        &cg.farplaneColorOverride[0],
        &cg.farplaneColorOverride[1],
        &cg.farplaneColorOverride[2]
    );
}

void CG_ParseFogInfo_ver_6(const char *str)
{
    //
    // clear all unsupported fields in protocol below version 15
    //

    // don't set the farplane_bias 0, otherwise the renderer will set a minimum value
    cg.farplane_bias            = 0.001f;
    cg.skyboxFarplane           = 0;
    cg.skyboxSpeed              = 0;
    cg.renderTerrain            = qtrue;
    cg.farclipOverride          = -1.0;
    cg.farplaneColorOverride[0] = -1.0;
    cg.farplaneColorOverride[1] = -1.0;
    cg.farplaneColorOverride[2] = -1.0;

    sscanf(
        str,
        "%d %f %f %f %f",
        &cg.farplane_cull,
        &cg.farplane_distance,
        &cg.farplane_color[0],
        &cg.farplane_color[1],
        &cg.farplane_color[2]
    );
}

void CG_ParseFogInfo(const char *str)
{
    if (cg_protocol >= PROTOCOL_MOHTA_MIN) {
        CG_ParseFogInfo_ver_15(str);
    } else {
        CG_ParseFogInfo_ver_6(str);
    }
}
