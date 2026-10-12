/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.

This file is part of Quake III Arena source code.

Quake III Arena source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Quake III Arena source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Quake III Arena source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/

#include "tr_local.h"
#include "../renderercommon/hzm_storm.h" // HZM coop [2026-09-28] storm darkness

int			r_firstSceneDrawSurf;

int			r_numdlights;
int			r_firstSceneDlight;

int			r_numentities;
int			r_firstSceneEntity;

int			r_numpolys;
int			r_firstScenePoly;

int			r_numpolyverts;

//
// OPENMOHAA-specific stuff
//=========================

int			r_firstSceneSpriteSurf;

int			r_numsprites;
int			r_firstSceneSprite;

int			r_numtermarks;
int			r_firstSceneTerMark;

//=========================

/*
====================
R_InitNextFrame

====================
*/
void R_InitNextFrame( void ) {
	backEndData->commands.used = 0;

	r_firstSceneDrawSurf = 0;

	r_numdlights = 0;
	r_firstSceneDlight = 0;

	r_numentities = 0;
	r_firstSceneEntity = 0;

	r_numpolys = 0;
	r_firstScenePoly = 0;

	r_numpolyverts = 0;

    //
    // OPENMOHAA-specific stuff
    //

    r_firstSceneSpriteSurf = 0;

	r_numsprites = 0;
	r_firstSceneSprite = 0;

	r_numtermarks = 0;
	r_firstSceneTerMark = 0;

	R_HZM_SpotNextFrame();	// HZM gl2 [2026-09-26] Phase S2: this frame's lamp-flare list starts empty
}


/*
====================
RE_ClearScene

====================
*/
void RE_ClearScene( void ) {
	r_firstSceneDlight = r_numdlights;
	r_firstSceneEntity = r_numentities;
	r_firstScenePoly = r_numpolys;
	R_HZM_SpotClearScene();	// HZM gl2 [2026-09-26] Phase S2: the flare list is scene-relative like the dlights
}

/*
===========================================================================

DISCRETE POLYS

===========================================================================
*/

/*
=====================
R_AddPolygonSurfaces

Adds all the scene's polys into this view's drawsurf list
=====================
*/
void R_AddPolygonSurfaces( void ) {
	int			i;
	shader_t	*sh;
	srfPoly_t	*poly;
	int		fogMask;

	tr.currentEntityNum = REFENTITYNUM_WORLD;
	tr.shiftedEntityNum = tr.currentEntityNum << QSORT_REFENTITYNUM_SHIFT;
	fogMask = -((tr.refdef.rdflags & RDF_NOFOG) == 0);

	for ( i = 0, poly = tr.refdef.polys; i < tr.refdef.numPolys ; i++, poly++ ) {
		sh = R_GetShaderByHandle( poly->hShader );
		R_AddDrawSurf( ( void * )poly, sh, poly->fogIndex & fogMask, qfalse, qfalse, 0 /*cubeMap*/  );
	}
}

/*
=====================
RE_AddPolyToScene

=====================
*/
void RE_AddPolyToScene( qhandle_t hShader, int numVerts, const polyVert_t *verts, int numPolys ) {
	srfPoly_t	*poly;
	int			i, j;
	int			fogIndex;
	fog_t		*fog;
	vec3_t		bounds[2];

	if ( !tr.registered ) {
		return;
	}

	if ( !hShader ) {
		// This isn't a useful warning, and an hShader of zero isn't a null shader, it's
		// the default shader.
		//ri.Printf( PRINT_WARNING, "WARNING: RE_AddPolyToScene: NULL poly shader\n");
		//return;
	}

	for ( j = 0; j < numPolys; j++ ) {
		if ( r_numpolyverts + numVerts > max_polyverts || r_numpolys >= max_polys ) {
      /*
      NOTE TTimo this was initially a PRINT_WARNING
      but it happens a lot with high fighting scenes and particles
      since we don't plan on changing the const and making for room for those effects
      simply cut this message to developer only
      */
			ri.Printf( PRINT_DEVELOPER, "WARNING: RE_AddPolyToScene: r_max_polys or r_max_polyverts reached\n");
			return;
		}

		poly = &backEndData->polys[r_numpolys];
		poly->surfaceType = SF_POLY;
		poly->hShader = hShader;
		poly->numVerts = numVerts;
		poly->verts = &backEndData->polyVerts[r_numpolyverts];
		
		Com_Memcpy( poly->verts, &verts[numVerts*j], numVerts * sizeof( *verts ) );

		if ( glConfig.hardwareType == GLHW_RAGEPRO ) {
			poly->verts->modulate[0] = 255;
			poly->verts->modulate[1] = 255;
			poly->verts->modulate[2] = 255;
			poly->verts->modulate[3] = 255;
		}
		// done.
		r_numpolys++;
		r_numpolyverts += numVerts;

		// if no world is loaded
		if ( tr.world == NULL ) {
			fogIndex = 0;
		}
		// see if it is in a fog volume
		else if ( tr.world->numfogs == 1 ) {
			fogIndex = 0;
		} else {
			// find which fog volume the poly is in
			VectorCopy( poly->verts[0].xyz, bounds[0] );
			VectorCopy( poly->verts[0].xyz, bounds[1] );
			for ( i = 1 ; i < poly->numVerts ; i++ ) {
				AddPointToBounds( poly->verts[i].xyz, bounds[0], bounds[1] );
			}
			for ( fogIndex = 1 ; fogIndex < tr.world->numfogs ; fogIndex++ ) {
				fog = &tr.world->fogs[fogIndex]; 
				if ( bounds[1][0] >= fog->bounds[0][0]
					&& bounds[1][1] >= fog->bounds[0][1]
					&& bounds[1][2] >= fog->bounds[0][2]
					&& bounds[0][0] <= fog->bounds[1][0]
					&& bounds[0][1] <= fog->bounds[1][1]
					&& bounds[0][2] <= fog->bounds[1][2] ) {
					break;
				}
			}
			if ( fogIndex == tr.world->numfogs ) {
				fogIndex = 0;
			}
		}
		poly->fogIndex = fogIndex;
	}
}


//=================================================================================


/*
=====================
RE_AddRefEntityToScene

=====================
*/
void RE_AddRefEntityToScene( const refEntity_t *ent ) {
	vec3_t cross;

	if ( !tr.registered ) {
		return;
	}
	if ( r_numentities >= MAX_REFENTITIES ) {
		ri.Printf(PRINT_DEVELOPER, "RE_AddRefEntityToScene: Dropping refEntity, reached MAX_REFENTITIES\n");
		return;
	}
	if ( Q_isnan(ent->origin[0]) || Q_isnan(ent->origin[1]) || Q_isnan(ent->origin[2]) ) {
		static qboolean firstTime = qtrue;
		if (firstTime) {
			firstTime = qfalse;
			ri.Printf( PRINT_WARNING, "RE_AddRefEntityToScene passed a refEntity which has an origin with a NaN component\n");
		}
		return;
	}
	if ( (int)ent->reType < 0 || ent->reType >= RT_MAX_REF_ENTITY_TYPE ) {
		ri.Error( ERR_DROP, "RE_AddRefEntityToScene: bad reType %i", ent->reType );
	}

	backEndData->entities[r_numentities].e = *ent;
	backEndData->entities[r_numentities].lightingCalculated = qfalse;
	// HZM gl2 re-port (bug-gl2-modellight): reset the per-frame model lighting
	// caches, mirrors gl1 tr_scene.c:240-241
	backEndData->entities[r_numentities].bLightGridCalculated = qfalse;
	backEndData->entities[r_numentities].sphereCalculated = qfalse;
	// HZM gl2 (bug-gl2-sphereslot-alias): keep the slot's owning-list stamp paired with the
	// flag that validates it. backEnd.sphereListId is free-running and only ever increases
	// within a frame, so a stale stamp could not match anyway - this is hygiene, not load
	// bearing, and it is inert while r_sphereCacheScope is 0.
	backEndData->entities[r_numentities].sphereList = 0u;
	// HZM gl2 (bug-1131, #67 invisible-actor coin flip): cgame memsets its refEntity_t, so
	// e.parentEntity arrives as 0 - NOT ENTITYNUM_NONE. gl1 translates the parent via the
	// two-arg API; gl2's port dropped that, so RB_SetupEntityGridLighting's parent walk chased
	// refdef slot 0's NEVER-INITIALIZED iGridLighting (raw hunk bytes, re-stamped every frame =
	// per-boot latched garbage vertex color -> black/invisible actors on unlucky boots).
	// Sanitize here; RE_AddRefEntityToScene2 re-links real parents below.
	backEndData->entities[r_numentities].e.parentEntity = ENTITYNUM_NONE;

	CrossProduct(ent->axis[0], ent->axis[1], cross);
	backEndData->entities[r_numentities].mirrored = (DotProduct(ent->axis[2], cross) < 0.f);

	// ^~^~^ SKELTRACK (bug-1131): continuous low-rate tracker - proves whether CGAME keeps
	// SUBMITTING the e2l2 briefing ally after a script teleport. Pair with the addskel probe:
	// submit firing while addskel is silent = dropped between scene add and the skel add path.
	// REMOVE with the rest of the SKEL* scaffolding.
	if (0 && ent->tiki && ent->tiki->a && strstr(ent->tiki->a->name, "brit_cmd")) {   // HZM 07-28: SKELTRACK off (per-frame strstr on every refent)
		static int trkSub = 0;
		trkSub++;
		if ((trkSub & 31) == 1) {
			ri.Printf(PRINT_ALL,
				"^~^~^ SKELTRACK submit n=%d ent=%d org=[%d %d %d] hModel=%d rfx=0x%x reType=%d\n",
				trkSub, ent->entityNumber,
				(int)ent->origin[0], (int)ent->origin[1], (int)ent->origin[2],
				ent->hModel, ent->renderfx, (int)ent->reType);
		}
	}

	r_numentities++;
}


/*
=====================
RE_AddDynamicLightToScene

=====================
*/
/*
=====================
R_AddDynamicLightTyped

HZM coop [user 2026-09-09, bug-2553] The real body, taking the FULL dlighttype_t bitmask
(tr_types_new.h:82-86 - lensflare 1, viewlensflare 2, additive 4) instead of a bare boolean.

gl2 used to write only dl->additive, and never dl->type - so tr_sun_flare.cpp:592-593, which tests
`type & lensflare` and `type & additive`, was reading a field nothing had ever written. Both are now
set from one source, so they cannot disagree.
=====================
*/
static void R_AddDynamicLightTyped( const vec3_t org, float intensity, float r, float g, float b, int type ) {
	dlight_t	*dl;
	hzmAdd_t	hzmVerdict;

	if ( !tr.registered ) {
		return;
	}
	// HZM gl2 [2026-09-26] PHASE S intake (tr_hzm_spot_rb.c), deliberately BEFORE the 32-slot cap and the intensity
	// test: a carrier (intensity < 0, HZM_DLIGHT_CARRIER) never takes a slot - it fills the pending spot with its tag,
	// or becomes a lamp flare, or is discarded - and a spot light while r_hzmSpot is off (always on Omaha) is DROPPED
	// rather than lit as an omni light behind the truck. Every other light pays one bit test and carries on below.
	if ( R_HZM_SpotIntake( org, intensity, r, g, b, type, &hzmVerdict ) ) {
		return;
	}
	if ( r_numdlights >= MAX_DLIGHTS ) {
		return;
	}
	if ( intensity <= 0 ) {
		return;
	}
	// these cards don't have the correct blend mode
	if ( glConfig.hardwareType == GLHW_RIVA128 || glConfig.hardwareType == GLHW_PERMEDIA2 ) {
		return;
	}
	dl = &backEndData->dlights[r_numdlights++];
	VectorCopy (org, dl->origin);
	dl->radius = intensity;
	dl->color[0] = r;
	dl->color[1] = g;
	dl->color[2] = b;
	// `additive` here is the ENUMERATOR (value 4), not a parameter - which is why this function's
	// parameter is called `type` and the boolean wrapper's is called `bAdditive`.
	dl->additive = ( type & additive ) ? qtrue : qfalse;
	dl->type = (dlighttype_t)type;
	// HZM gl2 [2026-09-26] Phase S: zero every spot field on EVERY add (the slots are reused each frame), and mark a
	// spot light pending until its carrier arrives (spot-or-nothing, R_HZM_SpotFinishScene)
	R_HZM_SpotBeginLight( dl, type, hzmVerdict == HZM_ADD_SPOT ? qtrue : qfalse );
}

/*
=====================
RE_AddDynamicLightToScene

The BOOLEAN entry point, unchanged in meaning for its existing callers (qfalse/qtrue below and in
RE_AddLightToScene2). Kept separate on purpose: passing a boolean 1 into the typed function would
set the LENSFLARE bit, not the additive one.
=====================
*/
void RE_AddDynamicLightToScene( const vec3_t org, float intensity, float r, float g, float b, int bAdditive ) {
	R_AddDynamicLightTyped( org, intensity, r, g, b, bAdditive ? (int)additive : 0 );
}

/*
=====================
RE_AddLightToScene

=====================
*/
void RE_AddLightToScene( const vec3_t org, float intensity, float r, float g, float b ) {
	RE_AddDynamicLightToScene( org, intensity, r, g, b, qfalse );
}

/*
=====================
RE_AddAdditiveLightToScene

=====================
*/
void RE_AddAdditiveLightToScene( const vec3_t org, float intensity, float r, float g, float b ) {
	RE_AddDynamicLightToScene( org, intensity, r, g, b, qtrue );
}


/*
=================
R_ShadowFarInputsChanged

HZM gl2 shadow hardening (plan SE#4). The legacy far cascade is re-rendered only when the sun direction changes,
so every LIVE cvar that changes what it contains froze beyond ~512 u until the next map. With r_shadowHarden 1
a change to any of them re-bakes it. Called only where the far cascade is decided; the signature is refreshed
on every call, so it also tracks changes made while hardening was off.
=================
*/
qboolean R_ShadowFarInputsChanged(void)
{
	static int lastSig = 0x7fffffff;
	int sig = r_shadowCastFoliage->modificationCount
	        + 7 * r_shadowMapBiasFactor->modificationCount
	        + 31 * r_shadowMapBiasUnits->modificationCount
	        + 127 * r_charShadows->modificationCount
	        + 509 * r_drawstaticmodels->modificationCount;
	qboolean changed = (qboolean)( sig != lastSig );

	lastSig = sig;
	return (qboolean)( changed && r_shadowHarden && r_shadowHarden->integer );
}

void RE_BeginScene(const refdef_t *fd)
{
	Com_Memcpy( tr.refdef.text, fd->text, sizeof( tr.refdef.text ) );

	tr.refdef.x = fd->x;
	tr.refdef.y = fd->y;
	tr.refdef.width = fd->width;
	tr.refdef.height = fd->height;
	tr.refdef.fov_x = fd->fov_x;
	tr.refdef.fov_y = fd->fov_y;

	VectorCopy( fd->vieworg, tr.refdef.vieworg );
	VectorCopy( fd->viewaxis[0], tr.refdef.viewaxis[0] );
	VectorCopy( fd->viewaxis[1], tr.refdef.viewaxis[1] );
	VectorCopy( fd->viewaxis[2], tr.refdef.viewaxis[2] );

	// HZM gl2 graphics probes: r_gfxProbe bit 2 pins renderer time, so repeated captures of a static view
	// compare equal (shader animation, tcMods, deforms). Identity at default.
	tr.refdef.time = R_GfxProbe_FreezeTime( fd->time );
	tr.refdef.rdflags = fd->rdflags;

	// copy the areamask data over and note if it has changed, which
	// will force a reset of the visible leafs even if the view hasn't moved
	tr.refdef.areamaskModified = qfalse;
	if ( ! (tr.refdef.rdflags & RDF_NOWORLDMODEL) ) {
		int		areaDiff;
		int		i;

		// compare the area bits
		areaDiff = 0;
		for (i = 0 ; i < MAX_MAP_AREA_BYTES/4 ; i++) {
			areaDiff |= ((int *)tr.refdef.areamask)[i] ^ ((int *)fd->areamask)[i];
			((int *)tr.refdef.areamask)[i] = ((int *)fd->areamask)[i];
		}

		if ( areaDiff ) {
			// a door just opened or something
			tr.refdef.areamaskModified = qtrue;
		}
	}

	tr.refdef.sunDir[3] = 0.0f;
	tr.refdef.sunCol[3] = 1.0f;
	tr.refdef.sunAmbCol[3] = 1.0f;
	tr.refdef.hzmSunActive = 0;   // HZM gl2 P3: only R_SunStable_Frame sets it, for this scene

	VectorCopy(tr.sunDirection, tr.refdef.sunDir);
	// HZM gl2 real character shadows: this is the FIRST of three r_depthPrepass gates on the
	// sun-shadow chain, and the one that actually removes the sun LIGHT TERM - with it taken,
	// the cascades still render but there is nothing for a shadow to subtract.
	// R_CharShadowsActive() returns qfalse whenever r_charShadows is 0, so the default path
	// is unchanged.
	if ( (tr.refdef.rdflags & RDF_NOWORLDMODEL)
	     || !(r_depthPrepass->value || R_CharShadowsActive()) ){
		VectorSet(tr.refdef.sunCol, 0, 0, 0);
		VectorSet(tr.refdef.sunAmbCol, 0, 0, 0);
	}
	else
	{
		float scale = (1 << r_mapOverBrightBits->integer) / 255.0f;

		if (r_forceSun->integer)
			VectorScale(tr.sunLight, scale * r_forceSunLightScale->value, tr.refdef.sunCol);
		else
			VectorScale(tr.sunLight, scale, tr.refdef.sunCol);

		if (r_sunlightMode->integer == 1)
		{
			tr.refdef.sunAmbCol[0] =
			tr.refdef.sunAmbCol[1] =
			tr.refdef.sunAmbCol[2] = r_forceSun->integer ? r_forceSunAmbientScale->value : tr.sunShadowScale;
		}
		else
		{
			if (r_forceSun->integer)
				VectorScale(tr.sunLight, scale * r_forceSunAmbientScale->value, tr.refdef.sunAmbCol);
			else
				VectorScale(tr.sunLight, scale * tr.sunShadowScale, tr.refdef.sunAmbCol);
		}

		// HZM coop [2026-09-28] storm darkness: the sun is behind the storm deck. Mode 1 (the shipped one): lightall's
		// SHADOWMAP_MODULATE reads sunAmbCol.b / .r as the SUN'S VISIBILITY, so only the sun's share of each texel fades -
		// the lit side darkens toward the shadow level, a shadowed texel keeps its ambient, NOTHING brightens (vet F1).
		// sunCol (the sun specular; mode 2's direct term) fades with it. The lightning's sky-visibility test reads the
		// shadow MASK, not these, so it is unchanged.
		{
			const hzmStormState_t *hs = R_HZM_Storm();
			if (hs->active && hs->sunFade > 0.0f) {
				if (r_sunlightMode->integer == 1) {
					tr.refdef.sunAmbCol[2] = tr.refdef.sunAmbCol[0] * (1.0f - hs->sunFade);
				}
				VectorScale(tr.refdef.sunCol, 1.0f - hs->sunFade, tr.refdef.sunCol);
			}
		}
	}

	if (r_forceAutoExposure->integer)
	{
		tr.refdef.autoExposureMinMax[0] = r_forceAutoExposureMin->value;
		tr.refdef.autoExposureMinMax[1] = r_forceAutoExposureMax->value;
	}
	else
	{
		tr.refdef.autoExposureMinMax[0] = tr.autoExposureMinMax[0];
		tr.refdef.autoExposureMinMax[1] = tr.autoExposureMinMax[1];
	}

	if (r_forceToneMap->integer)
	{
		tr.refdef.toneMinAvgMaxLinear[0] = pow(2, r_forceToneMapMin->value);
		tr.refdef.toneMinAvgMaxLinear[1] = pow(2, r_forceToneMapAvg->value);
		tr.refdef.toneMinAvgMaxLinear[2] = pow(2, r_forceToneMapMax->value);
	}
	else
	{
		tr.refdef.toneMinAvgMaxLinear[0] = pow(2, tr.toneMinAvgMaxLevel[0]);
		tr.refdef.toneMinAvgMaxLinear[1] = pow(2, tr.toneMinAvgMaxLevel[1]);
		tr.refdef.toneMinAvgMaxLinear[2] = pow(2, tr.toneMinAvgMaxLevel[2]);
	}

	// Makro - copy exta info if present
	if (fd->rdflags & RDF_EXTRA) {
		const refdefex_t* extra = (const refdefex_t*) (fd+1);

		tr.refdef.blurFactor = extra->blurFactor;

		if (fd->rdflags & RDF_SUNLIGHT)
		{
			VectorCopy(extra->sunDir,    tr.refdef.sunDir);
			VectorCopy(extra->sunCol,    tr.refdef.sunCol);
			VectorCopy(extra->sunAmbCol, tr.refdef.sunAmbCol);
		}
	} 
	else
	{
		tr.refdef.blurFactor = 0.0f;
	}

	// derived info

	tr.refdef.floatTime = tr.refdef.time * 0.001;

	tr.refdef.numDrawSurfs = r_firstSceneDrawSurf;
	tr.refdef.drawSurfs = backEndData->drawSurfs;

	tr.refdef.num_entities = r_numentities - r_firstSceneEntity;
	tr.refdef.entities = &backEndData->entities[r_firstSceneEntity];

	// HZM gl2 [2026-09-26] Phase S: remove every spot whose carrier never came (spot-or-nothing) and hand this
	// scene its lamp flares - BEFORE the count below is taken
	R_HZM_SpotFinishScene();

	tr.refdef.num_dlights = r_numdlights - r_firstSceneDlight;
	tr.refdef.dlights = &backEndData->dlights[r_firstSceneDlight];

	tr.refdef.numPolys = r_numpolys - r_firstScenePoly;
	tr.refdef.polys = &backEndData->polys[r_firstScenePoly];

	tr.refdef.num_pshadows = 0;
	tr.refdef.pshadows = &backEndData->pshadows[0];

	// turn off dynamic lighting globally by clearing all the
	// dlights if it needs to be disabled or if vertex lighting is enabled
	if ( r_dynamiclight->integer == 0 ||
		 r_vertexLight->integer == 1 ||
		 glConfig.hardwareType == GLHW_PERMEDIA2 ) {
		tr.refdef.num_dlights = 0;
	}

	// a single frame may have multiple scenes draw inside it --
	// a 3D game view, 3D status bar renderings, 3D menus, etc.
	// They need to be distinguished by the light flare code, because
	// the visibility state for a given surface may be different in
	// each scene / view.
	tr.frameSceneNum++;
	tr.sceneCount++;

	//
	// OPENMOHAA-specific stuff
    //=========================
    TIKI_Reset_Caches();

    // copy the sky data
    tr.refdef.sky_alpha = fd->sky_alpha;
    tr.refdef.sky_portal = fd->sky_portal;

    VectorCopy(fd->sky_origin, tr.refdef.sky_origin);
    VectorCopy(fd->sky_axis[0], tr.refdef.sky_axis[0]);
    VectorCopy(fd->sky_axis[1], tr.refdef.sky_axis[1]);
    VectorCopy(fd->sky_axis[2], tr.refdef.sky_axis[2]);

    tr.refdef.numSpriteSurfs = r_firstSceneSpriteSurf;
    tr.refdef.spriteSurfs = backEndData->spriteSurfs;

    tr.refdef.num_sprites = r_numsprites - r_firstSceneSprite;
    tr.refdef.sprites = &backEndData->sprites[r_firstSceneSprite];

    tr.refdef.numTerMarks = r_numtermarks - r_firstSceneTerMark;
    tr.refdef.terMarks = &backEndData->terMarks[r_firstSceneTerMark];

    backEndData->staticModelData = tr.refdef.staticModelData;
	
    tr.skyRendered = qfalse;
    tr.portalRendered = qfalse;
	//=========================
}


void RE_EndScene(void)
{
	// the next scene rendered in this frame will tack on after this one
	r_firstSceneDrawSurf = tr.refdef.numDrawSurfs;
	r_firstSceneEntity = r_numentities;
	r_firstSceneDlight = r_numdlights;
	r_firstScenePoly = r_numpolys;

	//
	// OPENMOHAA-specific stuff
    //

    r_firstSceneSpriteSurf = tr.refdef.numSpriteSurfs;
    r_firstSceneSprite = r_numsprites;
    r_firstSceneTerMark = r_numtermarks;
    R_HZM_SpotEndScene();	// HZM gl2 [2026-09-26] Phase S2
}

/*
@@@@@@@@@@@@@@@@@@@@@
RE_RenderScene

Draw a 3D view into a part of the window, then return
to 2D drawing.

Rendering a scene may require multiple views to be rendered
to handle mirrors,
@@@@@@@@@@@@@@@@@@@@@
*/
void RE_RenderScene( const refdef_t *fd ) {
	viewParms_t		parms;
	int				startTime;

	if ( !tr.registered ) {
		return;
	}
	GLimp_LogComment( "====== RE_RenderScene =====\n" );

	if ( r_norefresh->integer ) {
		return;
	}

	startTime = ri.Milliseconds();
	if ( hzmTiming ) {
		hzmFrontUs -= R_HzmMicros();	// HZM gl2 [render-perf 0] r_hzmAB: closed at the end of this function
	}

	if (!tr.world && !( fd->rdflags & RDF_NOWORLDMODEL ) ) {
		ri.Error (ERR_DROP, "R_RenderScene: NULL worldmodel");
	}

	// HZM coop [2026-09-27] realistic lightning: this scene's strike state (tr_hzm_lightning.c). Only a world scene
	// refreshes it, so a HUD 3D icon drawn after the world cannot switch the flash off mid-frame.
	if ( !( fd->rdflags & RDF_NOWORLDMODEL ) ) {
		R_HzmLt_FrontEnd( fd->time );
	}

	// ------------------------------------------------------------------------------------
	// HZM gl2 -> cgame bridges. Both are gl1-SAFE BY CONSTRUCTION: renderergl1 never sets
	// either cvar, so under gl1 they stay at the defaults cgame itself registers and gl1
	// behaviour is bit-identical. This mirrors the existing renderergl1/tr_scene.c
	// r_coopSunAz/El/Valid pattern, which gl2 has never had.
	if ( tr.world ) {
		// (a) r_coopSunPublish (default 0, INDEPENDENT of the shadow work): publish the map's
		//     real sun so cgame's Phase-A directional decal (coop_shadowAuto) can follow it.
		//     Without this the gl2 decal is stuck in MANUAL mode at the hardcoded
		//     coop_shadowAz 45 / coop_shadowEl 45 on EVERY map, because r_coopSunValid never
		//     leaves cgame's "0" default.
		//     tr.sunDirection points TOWARD the sun; the cgame side already trails the decal
		//     away from it - do NOT re-negate here.
		if ( r_coopSunPublish && r_coopSunPublish->integer ) {
			float sunSum = tr.sunLight[0] + tr.sunLight[1] + tr.sunLight[2];
			float sz     = tr.sunDirection[2];
			float azDeg  = (float)atan2( tr.sunDirection[1], tr.sunDirection[0] ) * ( 180.0f / (float)M_PI );
			float elDeg;
			if ( sz < -1.0f ) { sz = -1.0f; } else if ( sz > 1.0f ) { sz = 1.0f; }
			elDeg = (float)asin( sz ) * ( 180.0f / (float)M_PI );
			ri.Cvar_Set( "r_coopSunAz",    va( "%g", azDeg ) );
			ri.Cvar_Set( "r_coopSunEl",    va( "%g", elDeg ) );
			// keep gl1's intensity threshold: measured worldspawn suncolor spans ~400x across
			// shipped maps, and tr.sunShadows has no intensity gate.
			ri.Cvar_Set( "r_coopSunValid", ( sunSum > 0.05f ) ? "1" : "0" );
		} else {
			// HZM bug-3011/bug-3102: publishing off (or turned off mid-session) must not leave the
			// last published sun standing for cgame; Cvar_Set is a no-op when already "0".
			ri.Cvar_Set( "r_coopSunValid", "0" );
		}

		// (b) r_coopRealShadows: capability signal telling cgame that renderergl2 is ACTUALLY
		//     casting characters into the sun cascade shadow maps right now, so CG_EntityShadow
		//     should stop drawing its decal (otherwise the user sees a blob AND a cast shadow
		//     and reasonably still reports "blobs"). Published every frame so toggling
		//     r_charShadows off restores the decal immediately, with no restart.
		//     NOT cg_shadows: that is the SAME cvar as the renderer's r_shadows, it gates the
		//     pshadow pass and cgame's water splash marks, and its registration defaults
		//     conflict.
		ri.Cvar_Set( "r_coopRealShadows",
			( R_CharShadowsActive() && !( r_charShadowBlob && r_charShadowBlob->integer ) ) ? "1" : "0" );
	}
	// ------------------------------------------------------------------------------------

	RE_BeginScene(fd);

	// HZM gl2 shadow hardening (r_shadowHarden, plan P1a): latch no-wrap for this scene, and let the views
	// that run BEFORE the main view (dlight shadows, sun cascades) fill at most half the drawsurf list, so the
	// main view can never be starved. Restored to the full list just before R_RenderView below.
	r_drawSurfNoWrap = (qboolean)( r_shadowHarden && r_shadowHarden->integer );
	r_drawSurfCap = r_drawSurfNoWrap ? MAX_DRAWSURFS / 2 : MAX_DRAWSURFS;

	// SmileTheory: playing with shadow mapping
	if (!( fd->rdflags & RDF_NOWORLDMODEL ) && tr.refdef.num_dlights && r_dlightMode->integer >= 2)
	{
		R_RenderDlightCubemaps(fd);
	}

	/* playing with more shadows */
	if(glRefConfig.framebufferObject && !( fd->rdflags & RDF_NOWORLDMODEL ) && r_shadows->integer == 4)
	{
		R_RenderPshadowMaps(fd);
	}

	// HZM gl2 DYNAMIC-LIGHT CAST SHADOWS (r_hzmDlightShadows, default 0). Same projected-
	// shadow chain as the r_shadows 4 block above, but the shadow list is built from the
	// scene's DLIGHTS (muzzle flashes, explosions, fires, script lights) instead of the
	// static lightgrid direction - see R_RenderDlightShadowMaps in tr_main.c.
	//
	// Placed AFTER the upstream dispatch on purpose: R_RenderDlightShadowMaps appends
	// starting at tr.refdef.num_pshadows, so if a user ever does set cg_shadows 4 the two
	// lists coexist instead of one silently overwriting the other.
	//
	// Both must run BEFORE the main R_RenderView below, because tr_world.c hands out this
	// frame's pshadow bits (R_PshadowSurface) while walking the world for the main view.
	// R_DlightShadowsActive() is qfalse whenever r_hzmDlightShadows is 0, which leaves
	// tr.refdef.num_pshadows at the 0 that RE_BeginScene set - i.e. exactly today.
	if( !( fd->rdflags & RDF_NOWORLDMODEL ) && R_DlightShadowsActive() )
	{
		R_GfxProbe_ViewBegin(GFXVIEW_DLS);
		R_RenderDlightShadowMaps(fd);
		R_GfxProbe_ViewEnd(GFXVIEW_DLS);
	}

	// playing with even more shadows
	//
	// HZM gl2 WASTED-WORK FIX (independent of r_charShadows): this dispatch did NOT check
	// r_depthPrepass, but every consumer of what it produces DOES:
	//   - the shadowmask resolve (tr_backend.c, gated on VPF_USESUNLIGHT)
	//   - VPF_USESUNLIGHT itself (set below, gated on r_depthPrepass)
	//   - the sun colour/ambient term (RE_BeginScene above, gated on r_depthPrepass)
	// The live gl2 sandbox archives r_depthPrepass 0, so this build rendered THREE full
	// cascade depth passes every frame and threw all three away. The extra term below makes
	// the dispatch agree with its consumers:
	//   r_depthPrepass 1                  -> renders, exactly as before (no behaviour change)
	//   r_depthPrepass 0, r_charShadows 0 -> skipped; the maps were being discarded anyway
	//   r_depthPrepass 0, r_charShadows 1 -> renders, because the feature consumes them
	// r_shadowDebug is included so the cascade thumbnail blit still has live content to show
	// while diagnosing, even in the otherwise-skipped state.
	// The last cascade's sun-direction cache (tr.lastCascadeSunDirection, below) stays
	// correct across a live toggle: skipping the dispatch leaves both the cached MVP and the
	// depth image untouched, and turning the chain back on re-renders only if the sun moved.
	if(glRefConfig.framebufferObject && r_sunlightMode->integer && !( fd->rdflags & RDF_NOWORLDMODEL ) && (r_forceSun->integer || tr.sunShadows)
	   && (r_depthPrepass->value || R_CharShadowsActive() || (r_shadowDebug && r_shadowDebug->integer)))
	{
		// HZM gl2 graphics probes: r_shadowFitYaw / r_shadowFitOffset perturb ONLY the view the cascade fit
		// sees (test T1); at 0 this is fd itself. The ViewBegin/End pairs feed ^~^~^ SHADOWBUDGET/SHADOWTIME.
		refdef_t fitScratch;
		const refdef_t *fitFd = R_GfxProbe_FitRefdef(fd, &fitScratch);
		// HZM gl2 shadow hardening (SE#4): evaluated EVERY frame (not inside the short-circuit below) so the
		// input signature is always current; it can only answer qtrue with r_shadowHarden 1.
		const qboolean farInputsChanged = R_ShadowFarInputsChanged();

		// HZM gl2 stable sun shadows + B0 (r_shadowStable, plan P3): three radial ENTITY-ONLY cascades + the static
		// world cascade W in image 3. qfalse whenever the switch is 0 or anything makes it invalid - then everything
		// below runs exactly as before.
		if (!R_SunStable_Frame(fitFd))
		{
		if (r_shadowCascadeZFar->integer != 0)
		{
			R_GfxProbe_ViewBegin(GFXVIEW_C0);
			R_RenderSunShadowMaps(fitFd, 0);
			R_GfxProbe_ViewEnd(GFXVIEW_C0);
			R_GfxProbe_ViewBegin(GFXVIEW_C1);
			R_RenderSunShadowMaps(fitFd, 1);
			R_GfxProbe_ViewEnd(GFXVIEW_C1);
			R_GfxProbe_ViewBegin(GFXVIEW_C2);
			R_RenderSunShadowMaps(fitFd, 2);
			R_GfxProbe_ViewEnd(GFXVIEW_C2);
		}
		else
		{
			Mat4Zero(tr.refdef.sunShadowMvp[0]);
			Mat4Zero(tr.refdef.sunShadowMvp[1]);
			Mat4Zero(tr.refdef.sunShadowMvp[2]);
		}

		// only rerender last cascade if sun has changed position
		// HZM gl2 shadow hardening (SE#4): ...or, with r_shadowHarden 1, when a live cvar that changes what
		// the far cascade CONTAINS was modified - before, those froze beyond ~512 u until the next map.
		// HZM gl2 P3: ...or W occupied the image while stable was on (one-shot, set on the switch back)
		if (r_forceSun->integer == 2 || !VectorCompare(tr.refdef.sunDir, tr.lastCascadeSunDirection)
		    || farInputsChanged || R_SunStable_LegacyNeedsFar())
		{
			VectorCopy(tr.refdef.sunDir, tr.lastCascadeSunDirection);
			R_GfxProbe_ViewBegin(GFXVIEW_C3);
			R_RenderSunShadowMaps(fitFd, 3);
			R_GfxProbe_ViewEnd(GFXVIEW_C3);
			Mat4Copy(tr.refdef.sunShadowMvp[3], tr.lastCascadeSunMvp);
		}
		else
		{
			Mat4Copy(tr.lastCascadeSunMvp, tr.refdef.sunShadowMvp[3]);
		}
		}   // HZM gl2 P3: end of the legacy branch
	}

	// playing with cube maps
	// this is where dynamic cubemaps would be rendered
	if (0) //(glRefConfig.framebufferObject && !( fd->rdflags & RDF_NOWORLDMODEL ))
	{
		int i, j;

		for (i = 0; i < tr.numCubemaps; i++)
		{
			for (j = 0; j < 6; j++)
			{
				R_RenderCubemapSide(i, j, qtrue);
			}
		}
	}

	// setup view parms for the initial view
	//
	// set up viewport
	// The refdef takes 0-at-the-top y coordinates, so
	// convert to GL's 0-at-the-bottom space
	//
	Com_Memset( &parms, 0, sizeof( parms ) );
	parms.viewportX = tr.refdef.x;
	parms.viewportY = glConfig.vidHeight - ( tr.refdef.y + tr.refdef.height );
	parms.viewportWidth = tr.refdef.width;
	parms.viewportHeight = tr.refdef.height;

	// HZM render scale (r_renderScale): the WORLD scene renders into the scaled scene FBO, so
	// scale its viewport from display pixels to scene pixels here. RDF_NOWORLDMODEL views
	// (UI / HUD 3D widgets) keep display pixels and render into renderFbo's own depth. Portal /
	// mirror sub-views inherit this scaled viewport through parms; projection is fov-based and
	// so is scale-independent. At scale 1.0 renderScaleActive is qfalse and this is skipped.
	if (tr.renderScaleActive && !( fd->rdflags & RDF_NOWORLDMODEL ))
	{
		float rscale = tr.renderScale;
		parms.viewportX      = (int)(parms.viewportX      * rscale + 0.5f);
		parms.viewportY      = (int)(parms.viewportY      * rscale + 0.5f);
		parms.viewportWidth  = (int)(parms.viewportWidth  * rscale + 0.5f);
		parms.viewportHeight = (int)(parms.viewportHeight * rscale + 0.5f);
	}

	parms.isPortal = qfalse;

	parms.fovX = tr.refdef.fov_x;
	parms.fovY = tr.refdef.fov_y;
	
	parms.stereoFrame = tr.refdef.stereoFrame;

	VectorCopy( fd->vieworg, parms.or.origin );
	VectorCopy( fd->viewaxis[0], parms.or.axis[0] );
	VectorCopy( fd->viewaxis[1], parms.or.axis[1] );
	VectorCopy( fd->viewaxis[2], parms.or.axis[2] );

	VectorCopy( fd->vieworg, parms.pvsOrigin );

	// HZM gl2 real character shadows: second r_depthPrepass gate. VPF_USESUNLIGHT is what
	// enables the shadowmask resolve in RB_DrawSurfs and LIGHTDEF_USE_SHADOWMAP in
	// tr_shade.c; without it the cascades are rendered and thrown away.
	// KEEP IN SYNC with the cascade DISPATCH gate above: the dispatch must not render
	// cascades this test is going to discard, and must not skip cascades this test is
	// going to consume. Both now read (r_depthPrepass->value || R_CharShadowsActive()).
	if(!( fd->rdflags & RDF_NOWORLDMODEL )
	   && (r_depthPrepass->value || R_CharShadowsActive())
	   && ((r_forceSun->integer) || tr.sunShadows))
	{
		parms.flags = VPF_USESUNLIGHT;
	}
	// HZM coop [2026-09-28] storm darkness: does removing the sun darken anything this frame (hzm_storm.h 4)?
	if (!(fd->rdflags & RDF_NOWORLDMODEL)) {
		R_HZM_StormSetSunLive((parms.flags & VPF_USESUNLIGHT) && r_sunlightMode->integer == 1 ? 1 : 0);
	}

	//
	// OPENMOHAA-specific stuff
	//=========================

	// copy the farplane data
	parms.farplane_distance = fd->farplane_distance;
	parms.farplane_bias = fd->farplane_bias;
	parms.farplane_color[0] = fd->farplane_color[0];
	parms.farplane_color[1] = fd->farplane_color[1];
	parms.farplane_color[2] = fd->farplane_color[2];
	parms.farplane_cull = fd->farplane_cull;
	parms.renderTerrain = fd->renderTerrain;

	tr.refdef.skybox_farplane = fd->skybox_farplane;
	tr.refdef.render_terrain = parms.renderTerrain;

	if (fd->farclipOverride >= 15900 || fd->farclipOverride <= -0.99) {
		tr.farclip = 0;
	} else {
		tr.farclip = r_farclip->integer;
		if (!tr.farclip && (r_picmip->integer > 1 || r_colorbits->integer == 16)) {
			tr.farclip = 2800;
		}
	}

	if (tr.farclip) {
        if (fd->farclipOverride != 0) {
            parms.farplane_distance = fd->farclipOverride;
		} else {
			parms.farplane_distance = tr.farclip;
		}

		if (fd->farplane_color[0] >= 0 && fd->farplane_color[1] >= 0 && fd->farplane_color[2] >= 0) {
            parms.farplane_color[0] = fd->farplane_color[0];
            parms.farplane_color[1] = fd->farplane_color[1];
            parms.farplane_color[2] = fd->farplane_color[2];
		}

		if (fd->farplaneColorOverride[0] >= 0 && fd->farplaneColorOverride[1] >= 0 && fd->farplaneColorOverride[2] >= 0) {
			parms.farplane_color[0] = fd->farplaneColorOverride[0];
			parms.farplane_color[1] = fd->farplaneColorOverride[1];
			parms.farplane_color[2] = fd->farplaneColorOverride[2];
		}

		parms.farplane_cull = qtrue;

        if (fd->farplane_distance > 0 && fd->farplane_distance < parms.farplane_distance) {
            parms.farplane_distance = fd->farplane_distance;
		} else {
			if (fd->farplane_bias == 0) {
				parms.farplane_bias = parms.farplane_distance * 0.18f;
			} else if (fd->farplane_distance <= 500.f) {
				parms.farplane_bias = parms.farplane_distance * 0.18f;
			} else {
				parms.farplane_bias = parms.farplane_distance / fd->farplane_distance;
			}
		}
	} else if (parms.farplane_bias == 0) {
		parms.farplane_bias = parms.farplane_distance * 0.18f;
	}
	//=========================

	r_drawSurfCap = MAX_DRAWSURFS;
	if (!( fd->rdflags & RDF_NOWORLDMODEL )) {
		R_GfxProbe_ViewBegin(GFXVIEW_MAIN);
	}
	R_RenderView( &parms );
	if (!( fd->rdflags & RDF_NOWORLDMODEL )) {
		R_GfxProbe_ViewEnd(GFXVIEW_MAIN);
	}

	if(!( fd->rdflags & RDF_NOWORLDMODEL ))
		R_AddPostProcessCmd();

	RE_EndScene();

	tr.frontEndMsec += ri.Milliseconds() - startTime;
	if ( hzmTiming ) {
		hzmFrontUs += R_HzmMicros();	// HZM gl2 [render-perf 0]
	}
}

//
// OPENMOHAA-specific stuff
//

/*
=====================
RE_AddLightToScene2

=====================
*/
void RE_AddLightToScene2(const vec3_t org, float intensity, float r, float g, float b, int type) {
    // HZM coop [user 2026-09-09, bug-2553] was `qfalse` - the caller's type bits were DISCARDED, so
    // dl->additive was always 0 and renderergl2/tr_shade.c:434 always chose the DST_COLOR multiply.
    // Against a night lightmap that multiplies by near-black, i.e. a dynamic light lit nothing.
    // gl1 has always passed this through (renderergl1/tr_scene.c:361). The bits originate in
    // cgame/cg_ents.c from RF_LENSFLARE / RF_VIEWLENSFLARE / RF_ADDITIVE_DLIGHT.
    R_AddDynamicLightTyped(org, intensity, r, g, b, type);
}


/*
=====================
RE_AddRefEntityToScene2

=====================
*/
void RE_AddRefEntityToScene2( const refEntity_t *ent, int parentEntityNumber ) {
	int before = r_numentities;

	RE_AddRefEntityToScene(ent);

	// HZM gl2 (bug-1131): mirror gl1 tr_scene.c:241-260 - translate the caller's WORLD entity
	// number into this scene's refdef-relative slot index (refdef.entities starts at
	// r_firstSceneEntity), or ENTITYNUM_NONE. The base add above already defaulted the field.
	if ( r_numentities > before && parentEntityNumber != ENTITYNUM_NONE ) {
		int i;

		for ( i = r_firstSceneEntity; i < before; i++ ) {
			if ( backEndData->entities[i].e.entityNumber == parentEntityNumber ) {
				backEndData->entities[before].e.parentEntity = i - r_firstSceneEntity;
				break;
			}
		}
	}
}

void RE_AddRefSpriteToScene(const refEntity_t* ent) {
	refSprite_t* spr;
	int i;

	if (!tr.registered) {
		return;
	}

	// HZM (engine-limits audit): this was a completely silent drop - sprites (muzzle flashes,
	// tracers, most of the FX layer) just stopped appearing with no trace anywhere.
	if (r_numsprites >= MAX_SPRITES) {
		static qboolean overflowWarned = qfalse;

		if (!overflowWarned) {
			overflowWarned = qtrue;
			ri.Printf(
				PRINT_WARNING,
				"RE_AddRefSpriteToScene: MAX_SPRITES (%d) exceeded - sprites dropped."
				" Raise MAX_SPRITES in renderercommon/new/tr_types_new.h"
				" (it sizes backEndData_t::sprites; MAX_SPRITESURFS must stay >= it).\n",
				MAX_SPRITES
			);
		}
		return;
	}

	spr = &backEndData->sprites[r_numsprites];
	VectorCopy(ent->origin, spr->origin);
	spr->surftype = SF_SPRITE;
    spr->hModel = ent->hModel;
    spr->scale = ent->scale;
    spr->renderfx = ent->renderfx;
    spr->shaderTime = ent->shaderTime;
	AxisCopy(ent->axis, spr->axis);

	for (i = 0; i < 4; ++i) {
		spr->shaderRGBA[i] = ent->shaderRGBA[i];
	}

    ++r_numsprites;
}

/*
=====================
RE_AddPolyToScene

=====================
*/
qboolean RE_AddPolyToScene2(qhandle_t hShader, int numVerts, const polyVert_t* verts, int renderfx) {
	srfPoly_t	*poly;

	if ( !tr.registered ) {
		return qfalse;
	}

	if (numVerts + r_numpolyverts > max_polyverts || r_numpolys >= max_polys) {
		ri.Printf(PRINT_WARNING, "Exceeded MAX POLYS\n");
		return qfalse;
	}

	poly = &backEndData->polys[r_numpolys];
	poly->surfaceType = SF_POLY;
	poly->hShader = hShader;
	poly->numVerts = numVerts;
	poly->verts = &backEndData->polyVerts[r_numpolyverts];
	poly->renderfx = renderfx;

	Com_Memcpy(poly->verts, verts, sizeof(polyVert_t) * numVerts);
	++r_numpolys;
	r_numpolyverts += numVerts;

	return qtrue;
}

/*
=====================
R_AddTerrainMarkSurfaces
=====================
*/
void R_AddTerrainMarkSurfaces(void) {
    srfMarkFragment_t* terMark;
    int j;
    shader_t* shader;

    for (j = 0; j < tr.refdef.numTerMarks; j++)
    {
        terMark = &tr.refdef.terMarks[j];

        shader = R_GetShaderByHandle(terMark->surfaceType);
        terMark->surfaceType = SF_MARK_FRAG;
        R_AddDrawSurf(&terMark->surfaceType, shader, 0, 0, 0, 0);
    }
}

/*
=====================
RE_AddTerrainMarkToScene
=====================
*/
void RE_AddTerrainMarkToScene(int iTerrainIndex, qhandle_t hShader, int numVerts, const polyVert_t* verts, int renderfx) {
    srfMarkFragment_t* terMark;

    if (!tr.registered || !backEndData || !backEndData->terMarks) {
        return;
    }

    if (numVerts + r_numpolyverts > max_polyverts || r_numtermarks >= max_termarks) {
        ri.Printf(PRINT_WARNING, "Exceeded MAX TERRAIN MARKS\n");
        return;
    }

    terMark = &backEndData->terMarks[r_numtermarks];
    terMark->surfaceType = hShader;
    terMark->iIndex = iTerrainIndex;
    terMark->numVerts = numVerts;
    terMark->verts = &backEndData->polyVerts[r_numpolyverts];
    memcpy(terMark->verts, verts, sizeof(polyVert_t) * numVerts);

    r_numtermarks++;
    r_numpolyverts += numVerts;
}

//=================================================================================

/*
=====================
RE_GetRenderEntity
=====================
*/
refEntity_t* RE_GetRenderEntity(int entityNumber) {
    int i;

    // HZM (bug-1217, gl2 twin of the renderergl1 fix): scan only the CURRENT scene.
    // r_numentities is zeroed ONCE PER FRAME in R_InitNextFrame - RE_ClearScene does NOT reset it,
    // it only moves r_firstSceneEntity up to the current high-water mark - and a single frame draws
    // several scenes (the 3D game view, then the HUD / menu / inventory renders, which go through
    // cl_invrender.cpp's own re.ClearScene). Scanning from 0 could therefore match an entity from an
    // EARLIER scene of the same frame and hand back a stale origin/axis/renderfx; every caller is a
    // cgame attached-model parent lookup, so a stale hit attaches a prop to the previous scene's
    // copy of its parent. RE_AddRefEntityToScene's own parent loop already starts here.
    // NOT fixed by resetting the counter: render commands for already-submitted scenes point into
    // backEndData->entities[] and do not execute until RE_EndFrame, so reusing those slots would
    // corrupt them.
    for (i = r_firstSceneEntity; i < r_numentities; i++) {
        if (backEndData->entities[i].e.entityNumber == entityNumber) {
            return &backEndData->entities[i].e;
        }
    }

    return NULL;
}
