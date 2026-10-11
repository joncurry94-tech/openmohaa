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
#include "tr_fbo.h"
#include "tr_dsa.h"
#include "../renderercommon/hzm_storm.h" // HZM coop [2026-09-28] storm darkness

backEndData_t	*backEndData;
backEndState_t	backEnd;


static float	s_flipMatrix[16] = {
	// convert from our coordinate system (looking down X)
	// to OpenGL's coordinate system (looking down -Z)
	0, 0, -1, 0,
	-1, 0, 0, 0,
	0, 1, 0, 0,
	0, 0, 0, 1
};

//
// OPENMOHAA-specific stuff
//=========================

const void* RB_SpriteSurfs(const void* data);

//=========================

/*
** GL_BindToTMU
*/
void GL_BindToTMU( image_t *image, int tmu )
{
	GLuint texture = (tmu == TB_COLORMAP) ? tr.defaultImage->texnum : 0;
	GLenum target = GL_TEXTURE_2D;

	if (image)
	{
		if (image->flags & IMGFLAG_CUBEMAP)
			target = GL_TEXTURE_CUBE_MAP;
		else if (image->flags & IMGFLAG_MULTISAMPLE)
			target = GL_TEXTURE_2D_MULTISAMPLE;   // HZM gl2 MSAA (plan P2a): sampler2DMS in the P2b resolves

		image->frameUsed = tr.frameCount;
		texture = image->texnum;
	}
	else
	{
		ri.Printf(PRINT_WARNING, "GL_BindToTMU: NULL image\n");
	}

	GL_BindMultiTexture(GL_TEXTURE0 + tmu, target, texture);
}


/*
** GL_Cull
*/
void GL_Cull( int cullType ) {
	if ( glState.faceCulling == cullType ) {
		return;
	}

	if ( cullType == CT_TWO_SIDED ) 
	{
		qglDisable( GL_CULL_FACE );
	} 
	else 
	{
		qboolean cullFront = (cullType == CT_FRONT_SIDED);

		if ( glState.faceCulling == CT_TWO_SIDED )
			qglEnable( GL_CULL_FACE );

		if ( glState.faceCullFront != cullFront )
			qglCullFace( cullFront ? GL_FRONT : GL_BACK );

		glState.faceCullFront = cullFront;
	}

	glState.faceCulling = cullType;
}

/*
** GL_State
**
** This routine is responsible for setting the most commonly changed state
** in Q3.
*/
void GL_State( unsigned long stateBits )
{
	unsigned long diff = stateBits ^ glState.glStateBits;

	if ( !diff )
	{
		return;
	}

	//
	// check depthFunc bits
	//
	if ( diff & ( GLS_DEPTHFUNC_BITS | GLS_DEPTHFUNC_ALWAYS ) )
	{
		if ( stateBits & GLS_DEPTHFUNC_ALWAYS )
		{
			qglDepthFunc( GL_ALWAYS );   // HZM gl2 MSAA (plan P2a, ME-F5): its own bit; 0x60000 decodes as EQUAL
		}
		else if ( stateBits & GLS_DEPTHFUNC_EQUAL )
		{
			qglDepthFunc( GL_EQUAL );
		}
		else if ( stateBits & GLS_DEPTHFUNC_GREATER)
		{
			qglDepthFunc( GL_GREATER );
		}
		else
		{
			qglDepthFunc( GL_LEQUAL );
		}
	}

	//
	// check blend bits
	//
	if ( diff & ( GLS_SRCBLEND_BITS | GLS_DSTBLEND_BITS ) )
	{
		uint32_t oldState = glState.glStateBits & ( GLS_SRCBLEND_BITS | GLS_DSTBLEND_BITS );
		uint32_t newState = stateBits & ( GLS_SRCBLEND_BITS | GLS_DSTBLEND_BITS );
		uint32_t storedState = glState.storedGlState & ( GLS_SRCBLEND_BITS | GLS_DSTBLEND_BITS );

		if (oldState == 0)
		{
			qglEnable( GL_BLEND );
		}
		else if (newState == 0)
		{
			qglDisable( GL_BLEND );
		}

		if (newState != 0 && storedState != newState)
		{
			GLenum srcFactor = GL_ONE, dstFactor = GL_ONE;

			glState.storedGlState &= ~( GLS_SRCBLEND_BITS | GLS_DSTBLEND_BITS );
			glState.storedGlState |= newState;

			switch ( stateBits & GLS_SRCBLEND_BITS )
			{
			case GLS_SRCBLEND_ZERO:
				srcFactor = GL_ZERO;
				break;
			case GLS_SRCBLEND_ONE:
				srcFactor = GL_ONE;
				break;
			case GLS_SRCBLEND_DST_COLOR:
				srcFactor = GL_DST_COLOR;
				break;
			case GLS_SRCBLEND_ONE_MINUS_DST_COLOR:
				srcFactor = GL_ONE_MINUS_DST_COLOR;
				break;
			case GLS_SRCBLEND_SRC_ALPHA:
				srcFactor = GL_SRC_ALPHA;
				break;
			case GLS_SRCBLEND_ONE_MINUS_SRC_ALPHA:
				srcFactor = GL_ONE_MINUS_SRC_ALPHA;
				break;
			case GLS_SRCBLEND_DST_ALPHA:
				srcFactor = GL_DST_ALPHA;
				break;
			case GLS_SRCBLEND_ONE_MINUS_DST_ALPHA:
				srcFactor = GL_ONE_MINUS_DST_ALPHA;
				break;
			case GLS_SRCBLEND_ALPHA_SATURATE:
				srcFactor = GL_SRC_ALPHA_SATURATE;
				break;
			default:
				ri.Error( ERR_DROP, "GL_State: invalid src blend state bits" );
				break;
			}

			switch ( stateBits & GLS_DSTBLEND_BITS )
			{
			case GLS_DSTBLEND_ZERO:
				dstFactor = GL_ZERO;
				break;
			case GLS_DSTBLEND_ONE:
				dstFactor = GL_ONE;
				break;
			case GLS_DSTBLEND_SRC_COLOR:
				dstFactor = GL_SRC_COLOR;
				break;
			case GLS_DSTBLEND_ONE_MINUS_SRC_COLOR:
				dstFactor = GL_ONE_MINUS_SRC_COLOR;
				break;
			case GLS_DSTBLEND_SRC_ALPHA:
				dstFactor = GL_SRC_ALPHA;
				break;
			case GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA:
				dstFactor = GL_ONE_MINUS_SRC_ALPHA;
				break;
			case GLS_DSTBLEND_DST_ALPHA:
				dstFactor = GL_DST_ALPHA;
				break;
			case GLS_DSTBLEND_ONE_MINUS_DST_ALPHA:
				dstFactor = GL_ONE_MINUS_DST_ALPHA;
				break;
			default:
				ri.Error( ERR_DROP, "GL_State: invalid dst blend state bits" );
				break;
			}

			qglBlendFunc( srcFactor, dstFactor );
		}
	}

	//
	// check depthmask
	//
	if ( diff & GLS_DEPTHMASK_TRUE )
	{
		if ( stateBits & GLS_DEPTHMASK_TRUE )
		{
			qglDepthMask( GL_TRUE );
		}
		else
		{
			qglDepthMask( GL_FALSE );
		}
	}

	//
	// fill/line mode
	//
	if ( diff & GLS_POLYMODE_LINE )
	{
		if ( stateBits & GLS_POLYMODE_LINE )
		{
			qglPolygonMode( GL_FRONT_AND_BACK, GL_LINE );
		}
		else
		{
			qglPolygonMode( GL_FRONT_AND_BACK, GL_FILL );
		}
	}

	//
	// depthtest
	//
	if ( diff & GLS_DEPTHTEST_DISABLE )
	{
		if ( stateBits & GLS_DEPTHTEST_DISABLE )
		{
			qglDisable( GL_DEPTH_TEST );
		}
		else
		{
			qglEnable( GL_DEPTH_TEST );
		}
	}

	//
	// HZM gl2 MSAA (plan P2a infrastructure; only P4a sets it): alpha-to-coverage
	//
	if ( diff & GLS_ALPHA_TO_COVERAGE )
	{
		// P4a: alpha-to-one with it, so the colour buffer's alpha stays 1 and a later DST_ALPHA blend never sees
		// coverage values (research step 6)
		if ( stateBits & GLS_ALPHA_TO_COVERAGE )
		{
			qglEnable( GL_SAMPLE_ALPHA_TO_COVERAGE );
			if ( !qglesMajorVersion )
				qglEnable( GL_SAMPLE_ALPHA_TO_ONE );
		}
		else
		{
			qglDisable( GL_SAMPLE_ALPHA_TO_COVERAGE );
			if ( !qglesMajorVersion )
				qglDisable( GL_SAMPLE_ALPHA_TO_ONE );
		}
	}

	glState.glStateBits = stateBits;
}


void GL_SetProjectionMatrix(mat4_t matrix)
{
	Mat4Copy(matrix, glState.projection);
	Mat4Multiply(glState.projection, glState.modelview, glState.modelviewProjection);	
}


void GL_SetModelviewMatrix(mat4_t matrix)
{
	Mat4Copy(matrix, glState.modelview);
	Mat4Multiply(glState.projection, glState.modelview, glState.modelviewProjection);	
}


/*
================
RB_Hyperspace

A player has predicted a teleport, but hasn't arrived yet
================
*/
static void RB_Hyperspace( void ) {
	float		c;

	if ( !backEnd.isHyperspace ) {
		// do initialization shit
	}

	c = ( backEnd.refdef.time & 255 ) / 255.0f;
	qglClearColor( c, c, c, 1 );
	qglClear( GL_COLOR_BUFFER_BIT );
	qglClearColor(0.0f, 0.0f, 0.0f, 1.0f);

	backEnd.isHyperspace = qtrue;
}


static void SetViewportAndScissor( void ) {
	GL_SetProjectionMatrix( backEnd.viewParms.projectionMatrix );

	// set the window clipping
	qglViewport( backEnd.viewParms.viewportX, backEnd.viewParms.viewportY, 
		backEnd.viewParms.viewportWidth, backEnd.viewParms.viewportHeight );
	qglScissor( backEnd.viewParms.viewportX, backEnd.viewParms.viewportY, 
		backEnd.viewParms.viewportWidth, backEnd.viewParms.viewportHeight );
}

/*
=================
RB_BeginDrawingView

Any mirrored or portaled views have already been drawn, so prepare
to actually render the visible surfaces for this view
=================
*/
void RB_BeginDrawingView (void) {
	int clearBits = 0;

	// sync with gl if needed
	if ( r_finish->integer == 1 && !glState.finishCalled ) {
		qglFinish ();
		glState.finishCalled = qtrue;
	}
	if ( r_finish->integer == 0 ) {
		glState.finishCalled = qtrue;
	}

	// we will need to change the projection matrix before drawing
	// 2D images again
	backEnd.projection2D = qfalse;

	if (glRefConfig.framebufferObject)
	{
		FBO_t *fbo = backEnd.viewParms.targetFbo;

		if (fbo == NULL)
		{
			// HZM render scale: the WORLD scene renders into sceneFbo (SCENE size); RDF_NOWORLDMODEL
			// views (UI / HUD 3D widgets) keep the DISPLAY-size renderFbo. At scale 1.0 sceneFbo IS
			// renderFbo, so both branches pick the same buffer.
			if (backEnd.refdef.rdflags & RDF_NOWORLDMODEL)
				fbo = tr.renderFbo;
			else
				fbo = tr.sceneFbo ? tr.sceneFbo : tr.renderFbo;
		}

		if (tr.renderCubeFbo && fbo == tr.renderCubeFbo)
		{
			cubemap_t *cubemap = &tr.cubemaps[backEnd.viewParms.targetFboCubemapIndex];
			FBO_AttachImage(fbo, cubemap->image, GL_COLOR_ATTACHMENT0_EXT, backEnd.viewParms.targetFboLayer);
		}

		FBO_Bind(fbo);
	}

	//
	// set the modelview matrix for the viewer
	//
	SetViewportAndScissor();

	// ensures that depth writes are enabled for the depth clear
	GL_State( GLS_DEFAULT );
	// clear relevant buffers
	clearBits = GL_DEPTH_BUFFER_BIT;

	if ( r_measureOverdraw->integer || r_shadows->integer == 2 )
	{
		clearBits |= GL_STENCIL_BUFFER_BIT;
	}

	//
	// HZM gl2 fog parity: gl1 clears the colour buffer to the FOG COLOUR whenever the
	// global farplane fog is active and no sky/portal covers the view (renderergl1/
	// tr_backend.c:636). Without this, anything the farplane frustum plane culls away
	// (which is everything past farplane_distance - i.e. exactly the region gl1 renders
	// as solid haze) is left showing the black clear instead of the fog colour.
	//
	if ( !( backEnd.refdef.rdflags & RDF_NOWORLDMODEL ) )
	{
		if ( rb_globalFog.active && ( ( !tr.skyRendered && !tr.portalRendered ) || tr.farclip ) )
		{
			clearBits |= GL_COLOR_BUFFER_BIT;
			qglClearColor( rb_globalFog.color[0], rb_globalFog.color[1], rb_globalFog.color[2], 1.0f );
		}
		else if ( r_fastsky->integer )
		{
			clearBits |= GL_COLOR_BUFFER_BIT;	// FIXME: only if sky shaders have been used
			qglClearColor( 0.5f, 0.5f, 1.0f, 1.0f );
		}
	}

	// clear to black for cube maps
	if (tr.renderCubeFbo && backEnd.viewParms.targetFbo == tr.renderCubeFbo)
	{
		clearBits |= GL_COLOR_BUFFER_BIT;
	}

	qglClear( clearBits );

	if ( ( backEnd.refdef.rdflags & RDF_HYPERSPACE ) )
	{
		RB_Hyperspace();
		return;
	}
	else
	{
		backEnd.isHyperspace = qfalse;
	}

	// we will only draw a sun if there was sky rendered in this view
	backEnd.skyRenderedThisView = qfalse;

	// clip to the plane of the portal
	if ( backEnd.viewParms.isPortal ) {
#if 0
		float	plane[4];
		GLdouble	plane2[4];

		plane[0] = backEnd.viewParms.portalPlane.normal[0];
		plane[1] = backEnd.viewParms.portalPlane.normal[1];
		plane[2] = backEnd.viewParms.portalPlane.normal[2];
		plane[3] = backEnd.viewParms.portalPlane.dist;

		plane2[0] = DotProduct (backEnd.viewParms.or.axis[0], plane);
		plane2[1] = DotProduct (backEnd.viewParms.or.axis[1], plane);
		plane2[2] = DotProduct (backEnd.viewParms.or.axis[2], plane);
		plane2[3] = DotProduct (plane, backEnd.viewParms.or.origin) - plane[3];
#endif
		GL_SetModelviewMatrix( s_flipMatrix );
	}
}


// FIX (bug-gl2-invisible-live-char-depthprepass): is this refentity a bIsCharacter skeletal actor?
// Character (animated) skeletal models are EXCLUDED from the gl2 depth prepass (see below) because the
// prepass skins/writes depth from the pose current when the prepass runs, while the force-posed main
// color pass skins from the fresh main-view pose; for a live-animating actor those two poses differ,
// so the color-pass fragments land BEHIND the prepass depth and fail LEQUAL (samplesPassed=0 =>
// invisible). Excluding them (they draw only in the main opaque pass, LEQUAL vs the world/other-opaque
// depth) removes any prepass/color depth mismatch. Non-character skeletal props stay in the prepass.
static qboolean RB_IsCharacterSkelEntity(int entityNum)
{
	trRefEntity_t *ce;
	if (entityNum < 0 || entityNum >= MAX_REFENTITIES) {
		return qfalse; // world / out-of-range -> not a character
	}
	ce = &backEnd.refdef.entities[entityNum];
	return (qboolean)(ce->e.tiki && ce->e.tiki->a && ce->e.tiki->a->bIsCharacter);
}

// HZM gl2 REAL CHARACTER SHADOWS (r_charShadows).
//
// backEnd.depthFill is set for TWO structurally different passes by one shared block in
// RB_DrawSurfs:
//   (a) the MAIN-VIEW z-prepass - a pure optimisation; anything skipped here still draws
//       normally in the colour pass that follows.
//   (b) every VPF_DEPTHSHADOW sun-cascade view - which has NO other geometry pass at all
//       (the colour RB_RenderDrawSurfList is guarded by `if (!isShadowView)`).
//       Anything skipped in (b) casts NO SHADOW, permanently.
//
// The two historical exclusions below (bug-gl2-foliage-white for alpha-tested cutouts,
// bug-gl2-invisible-live-char-depthprepass for bIsCharacter skeletal actors) were both
// written for (a). Silently applying them to (b) is why every AI in the game casts nothing
// into tr.sunShadowDepthImage[0..3] and the player only ever sees the cgame blob decal.
//
// THE SAFETY PROPERTY, and the thing to review hardest: allowChars / allowCutout are
// computed ONCE per RB_RenderDrawSurfList from backEnd.viewParms.flags & VPF_DEPTHSHADOW.
// They can NEVER be true for the main-view prepass. If that ever loosens, live AI go
// INVISIBLE again - strictly worse than blob shadows. Never key this on backEnd.depthFill
// alone. With r_charShadows 0 both are qfalse and this predicate is byte-equivalent to the
// two hand-written conditions it replaces.
//
// Extracted into one helper deliberately: the two former call sites (the same-sort fast
// path and the slow path) were ~50 lines apart, and changing only one of them yields
// shadows that appear and vanish with draw-surf batching order - an intermittent bug that
// is close to impossible to attribute later. Now they cannot diverge.
// HZM gl2 shadow hardening (plan P1a): the body now lives in R_DepthViewSkips (tr_main.c), shared with the
// frontend so shadow views stop ADDING what this pass skips. Same tests, same order.
static qboolean RB_DepthFillSkip(const shader_t *shader, int entityNum,
                                 qboolean allowChars, qboolean allowCutout)
{
	return R_DepthViewSkips(shader, backEnd.refdef.entities, entityNum, allowChars, allowCutout);
}

// r_shadowDebug 3 accounting: how many character surfaces actually reached the cascade
// depth maps. Prediction before this feature is enabled: chars = 0 on every cascade.
static int rb_shadowCharsDrawn;
static int rb_shadowCharsSkipped;

/*
================================================================================
HZM gl2 CHARACTER LIGHTING (r_charLighting) - backend half.

RB_CharLightingWanted  - may THIS surface take the character-lighting path at all?
RB_SetupCharLighting   - restate backEnd.currentSphere as (flat colour, ambient /
                         directed split, one world-space direction) for the GPU.

Both are no-ops with r_charLighting 0: Wanted() returns qfalse on its first line and
SetupCharLighting is never called, leaving backEnd.charLight.active clear, which is
the single flag every consumer in tr_shade.c tests.
================================================================================
*/
static qboolean RB_CharLightingWanted(const shader_t *shader)
{
	if (!r_charLighting || !r_charLighting->integer) {
		return qfalse;
	}
	// Depth-only passes (main z-prepass and every sun cascade) do no shading.
	if (backEnd.depthFill) {
		return qfalse;
	}
	// No world = no lightgrid and no light spheres: the armory/menu model preview
	// (RDF_HUD) and every RDF_NOWORLDMODEL scene keep their existing fullbright path.
	if (backEnd.refdef.rdflags & (RDF_NOWORLDMODEL | RDF_HUD)) {
		return qfalse;
	}
	if (!backEnd.currentEntity || backEnd.currentEntity == &tr.worldEntity) {
		return qfalse;
	}
	// The view model is drawn into the near depth slice with its own projection; it is
	// not a character anyway, but exclude it explicitly so a future model change cannot
	// quietly drag it onto a path whose light direction assumes world depth.
	if (backEnd.currentEntity->e.renderfx & RF_DEPTHHACK) {
		return qfalse;
	}
	if (!backEnd.currentEntity->e.tiki || !backEnd.currentEntity->e.tiki->a
	    || !backEnd.currentEntity->e.tiki->a->bIsCharacter) {
		return qfalse;
	}
	// Only the spherical-lit stages. rgbGen lightingGrid resolves to ONE flat colour with
	// no direction anywhere in the data, so there is nothing to hand a light vector; those
	// stages keep the existing CPU path untouched.
	if (!shader || !shader->needsLSpherical || shader->needsLGrid) {
		return qfalse;
	}
	// The sphere builder itself is gated on this (it is CVAR_CHEAT with default 1, so a
	// listen server clamps it back to 1, but respect it anyway).
	if (!r_drawspherelights->integer) {
		return qfalse;
	}
	return qtrue;
}

static void RB_SetupCharLighting(void)
{
	sphereor_t *sph = backEnd.currentSphere;
	vec3_t      sumDir, dirColor, dirLocal, total;
	float       wrap, len;
	int         i;

	backEnd.charLight.active = qfalse;

	// RB_Sphere_SetupEntity has three outcomes; only one of them leaves real light data
	// behind. r_light_nolight -> RB_Light_Fullbright, a non-TIKI/failed sphere ->
	// RB_CalcLightGridColor (flat, no direction), success -> RB_Light_Real. In either of the
	// first two the CPU path below in RB_FillModelLightingColors still runs unchanged.
	// bUsesCubeMap is currently unreachable - RB_OptimizeLights has the whole cube-map
	// spill-over block behind `#if 0` ("re-enable when cubemap light is fixed"), so
	// nCubeMapLights is always 0 and every collected light stays direct - but the guard is
	// here because if it is ever re-enabled the surplus lights get folded into a 26-entry
	// cube with no single direction, which this restatement cannot represent.
	if (!sph || sph->TessFunction != RB_Light_Real || sph->bUsesCubeMap) {
		return;
	}

	VectorClear(sumDir);
	VectorClear(dirColor);

	// Fold every light the sphere collected into ONE directional light. Colours here are
	// in the same 0..255 space as sph->ambient.level, and each light's contribution is
	// reduced to the coefficient that RB_Light_Real multiplies its cosine term by, so the
	// sum below is exactly the colour a surface facing the lights would receive.
	for (i = 0; i < sph->numRealLights && i < MAX_REAL_LIGHTS; i++) {
		const reallightinfo_t *pl = &sph->light[i];
		vec3_t                 eff, v;
		float                  d, lum;

		switch (pl->eType) {
		case LIGHT_DIRECTIONAL:
			// The sun, and only when RB_Sphere_Light_Sun's trace actually reached sky -
			// this is what makes an actor under a roof stop being sun-lit.
			VectorCopy(pl->vDirection, dirLocal);
			VectorCopy(pl->color, eff);
			break;

		case LIGHT_SPOT:
		case LIGHT_SPOT_FAST:
			// RB_Light_Real: colour * dot(vDirection,N) / distSquared. The cone factor is
			// ~1 inside the cone and is dropped.
			VectorCopy(pl->vDirection, dirLocal);
			d = pl->fDist * pl->fDist;
			if (d < 1.0f) {
				d = 1.0f;
			}
			VectorScale(pl->color, 1.0f / d, eff);
			break;

		case LIGHT_POINT:
		default:
			// RB_Light_Real: colour * dot(v,N) / |v|^2, and v is NOT normalised, so the
			// cosine coefficient is colour / |v|.
			VectorSubtract(pl->vOrigin, sph->origin, v);
			d = pl->fDist;
			if (d < 1.0f) {
				d = 1.0f;
			}
			if (VectorNormalize2(v, dirLocal) == 0.0f) {
				continue;
			}
			VectorScale(pl->color, 1.0f / d, eff);
			break;
		}

		lum = eff[0] * 0.299f + eff[1] * 0.587f + eff[2] * 0.114f;
		VectorMA(sumDir, lum, dirLocal, sumDir);
		VectorAdd(dirColor, eff, dirColor);
	}

	len = VectorLength(sumDir);
	if (len < 0.0001f) {
		// Ambient only (indoors with no reachable light). There is no direction to give,
		// and forcing one would invent shading the map does not have.
		return;
	}
	VectorScale(sumDir, 1.0f / len, dirLocal);

	// The sphere works in ENTITY-LOCAL space (RB_Sphere_AddLight runs every light origin
	// through MatrixTransformVectorRight with the entity axis). lightall_vp puts the vertex
	// and its normal in WORLD space via u_ModelMatrix, so undo that rotation here.
	// MatrixTransformVector is the transpose of MatrixTransformVectorRight; a scaled entity
	// axis denormalises the result, which is harmless because lightall_fp normalises L.
	MatrixTransformVector(dirLocal, backEnd.currentEntity->e.axis, backEnd.charLight.lightDirWorld);

	// flat = ambient + directed = the "fully lit" colour, which is what tess.color now
	// carries. The fractions below re-derive the two halves from it on the GPU.
	total[0] = (float)sph->ambient.level[0] + dirColor[0];
	total[1] = (float)sph->ambient.level[1] + dirColor[1];
	total[2] = (float)sph->ambient.level[2] + dirColor[2];

	// HZM gl2 [user 2026-09-09, bug-2554] Ease the colour AND the light direction toward this
	// entity's previous frame. Placed here on purpose - before the ambient/directed fractions are
	// derived below - so the split always describes the colour it was derived from. The direction
	// matters as much as the colour: when the dominant light changes, an un-eased direction swings
	// the shading across the model in one frame, which reads as a flicker even when the brightness
	// barely moves. R_CoopSmoothEntityLight itself advances at most once per entity per frame, which
	// is what makes this safe to call from a per-DRAW-BATCH function.
	R_CoopSmoothEntityLight(backEnd.currentEntity->e.entityNumber, total, backEnd.charLight.lightDirWorld);

	wrap = r_charLightWrap ? r_charLightWrap->value : 0.0f;
	if (wrap < 0.0f) {
		wrap = 0.0f;
	} else if (wrap > 1.0f) {
		wrap = 1.0f;
	}

	for (i = 0; i < 3; i++) {
		float a, d;

		if (total[i] < 0.0001f) {
			backEnd.charLight.ambientFrac[i]  = 1.0f;
			backEnd.charLight.directedFrac[i] = 0.0f;
			backEnd.charLight.flatColor[i]    = 0;
			continue;
		}

		a = (float)sph->ambient.level[i] / total[i];
		d = dirColor[i] / total[i];

		// Wrap: hand `wrap` of the directed share to the unconditional half. Sum at
		// N.L == 1 is still a + d, so the lit side is untouched and only the dark side
		// lifts - it can never brighten a character past what MOHAA already gave him.
		a += d * wrap;
		d -= d * wrap;

		backEnd.charLight.ambientFrac[i]  = a;
		backEnd.charLight.directedFrac[i] = d;
		backEnd.charLight.flatColor[i]    = (byte)(total[i] > 255.0f ? 255 : (int)total[i]);
	}
	backEnd.charLight.flatColor[3] = 0xff;   // RB_Light_Real always writes opaque

	backEnd.charLight.active = qtrue;

	if (r_charLightDebug && r_charLightDebug->integer) {
		static int lastPrint;
		if (backEnd.refdef.time - lastPrint > 1000 || backEnd.refdef.time < lastPrint) {
			lastPrint = backEnd.refdef.time;
			ri.Printf(PRINT_ALL,
				"^~^~^ CHARLIGHT lights=%d dir=%.2f %.2f %.2f flat=%d %d %d amb=%.2f dir=%.2f\n",
				sph->numRealLights,
				backEnd.charLight.lightDirWorld[0], backEnd.charLight.lightDirWorld[1],
				backEnd.charLight.lightDirWorld[2],
				backEnd.charLight.flatColor[0], backEnd.charLight.flatColor[1],
				backEnd.charLight.flatColor[2],
				backEnd.charLight.ambientFrac[1], backEnd.charLight.directedFrac[1]);
		}
	}
}

/*
==================
RB_RenderDrawSurfList
==================
*/
void RB_RenderDrawSurfList( drawSurf_t *drawSurfs, int numDrawSurfs ) {
	shader_t		*shader = NULL, *oldShader;
	int				fogNum, oldFogNum;
	int				entityNum, oldEntityNum;
	int				dlighted, oldDlighted;
	int				pshadowed, oldPshadowed;
	int             cubemapIndex, oldCubemapIndex;
	qboolean		depthRange, oldDepthRange, isCrosshair, wasCrosshair;
	int				i;
	drawSurf_t		*drawSurf;
	int				oldSort;
	double			originalTime;
	FBO_t*			fbo = NULL;
	qboolean		bStaticModel, oldbStaticModel;
	qboolean		isShadowView;
	qboolean		allowChars;
	qboolean		allowCutout;

	// save original time for entity shader offsets
	originalTime = backEnd.refdef.floatTime;

	fbo = glState.currentFBO;

	// draw everything
	oldEntityNum = -1;
	backEnd.currentEntity = &tr.worldEntity;
	oldShader = NULL;
	oldFogNum = -1;
	oldDepthRange = qfalse;
	wasCrosshair = qfalse;
	oldDlighted = qfalse;
	oldPshadowed = qfalse;
	oldCubemapIndex = -1;
	oldSort = -1;
	// OPENMOHAA-specific stuff
	//=========================
    oldbStaticModel = -1;
    //=========================

	// HZM gl2 real character shadows: decide ONCE, from the view flags, whether this
	// depth-fill pass is a sun cascade (which is a caster's only chance) or the main-view
	// z-prepass (whose exclusions must stay exactly as they are). r_charShadows 0 -> both
	// qfalse -> RB_DepthFillSkip reproduces the previous behaviour byte for byte.
	isShadowView = (qboolean)((backEnd.viewParms.flags & VPF_DEPTHSHADOW) != 0);
	// HZM gl2 dynamic-light cast shadows: a VPF_PSHADOW view is the second kind of
	// "caster's only chance" depth pass (case (b) above) - it renders one entity group into
	// a 512x512 map and has no colour pass at all, so a character skipped here casts
	// nothing, permanently. VPF_PSHADOW is set ONLY by R_RenderDlightShadowMaps, i.e. only
	// while r_hzmDlightShadows is 1, so with the feature off this term is identically
	// false and allowChars is byte-for-byte the r_charShadows expression it was.
	// The main-view z-prepass can still never set it - that view carries no VPF_ flags at
	// all - which is the invariant the comment above demands.
	// (the expressions live in R_DepthViewAllows, shared with the frontend - plan P1a)
	R_DepthViewAllows(backEnd.viewParms.flags, backEnd.viewParms.shadowCascade, &allowChars, &allowCutout);

	if (backEnd.depthFill && isShadowView) {
		rb_shadowCharsDrawn = 0;
		rb_shadowCharsSkipped = 0;
	}

	backEnd.pc.c_surfaces += numDrawSurfs;
	// HZM gl2 re-port (bug-gl2-modellight): mirrors gl1 tr_backend.c:723
	backEnd.numSpheresUsed = 0;
	// HZM gl2 (bug-gl2-sphereslot-alias): the line above restarts slot numbering, but
	// trRefEntity_t::sphereCalculated - which says "my slot index is still valid" - is only
	// cleared once per FRAME (tr_scene.c). In gl1 that is consistent because gl1 runs one
	// list per frame; gl2 runs up to five, so every list after the first re-issues indices
	// that earlier-list entities still believe they own. Bump a list id alongside the reset
	// so the reuse test below can tell "same list" from "same frame".
	backEnd.sphereListId++;
	// HZM gl2 character lighting: never let a previous view's (or a previous frame's)
	// character batch leak into this list. It is cleared again per draw surf below; this is
	// the entry guard so nothing between two lists - 2D, sky, post - can observe a stale flag.
	backEnd.charLight.active = qfalse;

	// HZM gl2 [bug-3487] skin this list's skeletal surfaces on worker threads before the loop needs them
	// (tr_model.cpp R_SkelPreskin). Colour passes only: a depth-only pass skips most characters.
	if ( !backEnd.depthFill ) {
		R_SkelPreskin( drawSurfs, numDrawSurfs );
	}

	for (i = 0, drawSurf = drawSurfs ; i < numDrawSurfs ; i++, drawSurf++) {
		if ( drawSurf->sort == oldSort && drawSurf->cubemapIndex == oldCubemapIndex) {
			// see RB_DepthFillSkip: bug-gl2-foliage-white + bug-gl2-invisible-live-char-depthprepass
			// stay in force for the MAIN-view prepass; a VPF_DEPTHSHADOW cascade lets casters through.
			if (backEnd.depthFill && RB_DepthFillSkip(shader, entityNum, allowChars, allowCutout)) {
				if (isShadowView && RB_IsCharacterSkelEntity(entityNum)) {
					rb_shadowCharsSkipped++;
				}
				continue;
			}
			if (backEnd.depthFill && isShadowView && RB_IsCharacterSkelEntity(entityNum)) {
				rb_shadowCharsDrawn++;
			}

			// fast path, same as previous sort
			rb_surfaceTable[ *drawSurf->surface ]( drawSurf->surface );
			continue;
		}
		oldSort = drawSurf->sort;
		//
		// OPENMOHAA-specific stuff
		//=========================
		if (*drawSurf->surface != SF_SPRITE) {
			R_DecomposeSort( drawSurf->sort, &entityNum, &shader, &fogNum, &dlighted, &pshadowed,
				&bStaticModel );
		} else {
			shader = tr.sortedShaders[((refSprite_t*)drawSurf->surface)->shaderNum];
			entityNum = ENTITYNUM_WORLD;
			dlighted = 0;
			bStaticModel = qfalse;
        }
        //=========================
		cubemapIndex = drawSurf->cubemapIndex;

		//
		// change the tess parameters if needed
		// a "entityMergable" shader is a shader that can have surfaces from separate
		// entities merged into a single batch, like smoke and blood puff sprites
		if ( shader != NULL && ( shader != oldShader || fogNum != oldFogNum || dlighted != oldDlighted || pshadowed != oldPshadowed || cubemapIndex != oldCubemapIndex
			|| ( entityNum != oldEntityNum && !shader->entityMergable )
			|| ( bStaticModel != oldbStaticModel && !shader->entityMergable )
			) ) {
			if (oldShader != NULL) {
				RB_EndSurface();
			}
			// HZM gl2 [2026-09-26] S3b: the first qer_hzmSoftEdge surface of this view takes the scene-depth snapshot
			// HERE - tess is empty, and every opaque surface (characters and cutout foliage included) sorts before a
			// blended one, so the snapshot is the finished opaque scene. The sprite list takes its own again later.
			if ( shader->hzmSoftEdge > 0.0f && !backEnd.softDepthValid && !backEnd.depthFill ) {
				RB_HZM_SoftEdgeSnapshotAt( entityNum, bStaticModel );
			}
			RB_BeginSurface( shader, fogNum, cubemapIndex );
			backEnd.pc.c_surfBatches++;
			oldShader = shader;
			oldFogNum = fogNum;
			oldDlighted = dlighted;
			oldPshadowed = pshadowed;
			oldCubemapIndex = cubemapIndex;
		}

		// IDENTICAL predicate to the fast path above - see RB_DepthFillSkip. These two sites
		// must never diverge; that is the entire reason the test lives in one function.
		if (backEnd.depthFill && RB_DepthFillSkip(shader, entityNum, allowChars, allowCutout)) {
			if (isShadowView && RB_IsCharacterSkelEntity(entityNum)) {
				rb_shadowCharsSkipped++;
			}
			continue;
		}
		if (backEnd.depthFill && isShadowView && RB_IsCharacterSkelEntity(entityNum)) {
			rb_shadowCharsDrawn++;
		}

		// HZM gl2 re-port (bug-gl2-modellight): mirrors gl1 tr_backend.c:756
		backEnd.currentSphere = &backEnd.spareSphere;
		// HZM gl2 character lighting: cleared here, next to the sphere it is derived from,
		// so it can only ever be set by the block below and can never survive into a batch
		// that is not a character skin. The same-sort fast path above deliberately does not
		// reach either reset: it is by definition the same entity and shader, so both the
		// sphere and this stay valid for it.
		backEnd.charLight.active = qfalse;

		//
		// change the modelview matrix if needed
		//
		if ( entityNum != oldEntityNum
			|| bStaticModel != oldbStaticModel ) {
			depthRange = isCrosshair = qfalse;
			
			//
			// OPENMOHAA-specific stuff
			//=========================
			if (bStaticModel) {
				backEnd.shaderStartTime = 0.0;
				backEnd.currentEntity = 0;
				backEnd.spareSphere.TessFunction = 0;
				backEnd.currentStaticModel = &backEnd.refdef.staticModels[entityNum];
				R_RotateForStaticModel(backEnd.currentStaticModel, &backEnd.viewParms, &backEnd.or);
			}
			else {
                backEnd.currentStaticModel = NULL;
			//=========================

				if ( entityNum != REFENTITYNUM_WORLD ) {
					backEnd.currentEntity = &backEnd.refdef.entities[entityNum];

					// FIXME: e.shaderTime must be passed as int to avoid fp-precision loss issues
					backEnd.refdef.floatTime = originalTime - (double)backEnd.currentEntity->e.shaderTime;

					// we have to reset the shaderTime as well otherwise image animations start
					// from the wrong frame
					tess.shaderTime = backEnd.refdef.floatTime - tess.shader->timeOffset;

					// set up the transformation matrix
					R_RotateForEntity( backEnd.currentEntity, &backEnd.viewParms, &backEnd.or );

					// set up the dynamic lighting if needed
					if ( backEnd.currentEntity->needDlights ) {
						R_TransformDlights( backEnd.refdef.num_dlights, backEnd.refdef.dlights, &backEnd.or );
					}

					if(backEnd.currentEntity->e.renderfx & RF_DEPTHHACK)
					{
						// hack the depth range to prevent view model from poking into walls
						depthRange = qtrue;
					
						if(backEnd.currentEntity->e.renderfx & RF_CROSSHAIR)
							isCrosshair = qtrue;
					}
				} else {
					backEnd.currentEntity = &tr.worldEntity;
					backEnd.refdef.floatTime = originalTime;
					backEnd.or = backEnd.viewParms.world;
					// we have to reset the shaderTime as well otherwise image animations on
					// the world (like water) continue with the wrong frame
					tess.shaderTime = backEnd.refdef.floatTime - tess.shader->timeOffset;
					R_TransformDlights( backEnd.refdef.num_dlights, backEnd.refdef.dlights, &backEnd.or );
				}
			}

			GL_SetModelviewMatrix( backEnd.or.modelMatrix );

			//
			// change depthrange. Also change projection matrix so first person weapon does not look like coming
			// out of the screen.
			//
			if (oldDepthRange != depthRange || wasCrosshair != isCrosshair)
			{
				if (depthRange)
				{
					if(backEnd.viewParms.stereoFrame != STEREO_CENTER)
					{
						if(isCrosshair)
						{
							if(oldDepthRange)
							{
								// was not a crosshair but now is, change back proj matrix
								GL_SetProjectionMatrix( backEnd.viewParms.projectionMatrix );
							}
						}
						else
						{
							viewParms_t temp = backEnd.viewParms;

							R_SetupProjection(&temp, r_znear->value, 0, qfalse);

							GL_SetProjectionMatrix( temp.projectionMatrix );
						}
					}
					// HZM gl2 re-port Fix 3: mono path - depth-hacked view weapon renders with its
					// own un-zoomed projection so ADS zoom doesn't magnify the gun off screen.
					// Crosshair surfaces stay on the world projection.
					else if (backEnd.viewParms.weaponFovActive)
					{
						if (isCrosshair)
						{
							if (oldDepthRange)
							{
								GL_SetProjectionMatrix( backEnd.viewParms.projectionMatrix );
							}
						}
						else
						{
							GL_SetProjectionMatrix( backEnd.viewParms.weaponProjectionMatrix );
						}
					}

					if(!oldDepthRange)
						qglDepthRange (0, 0.3);
				}
				else
				{
					if(!wasCrosshair && backEnd.viewParms.stereoFrame != STEREO_CENTER)
					{
						GL_SetProjectionMatrix( backEnd.viewParms.projectionMatrix );
					}
					// HZM gl2 re-port Fix 3: mono restore
					else if (!wasCrosshair && backEnd.viewParms.weaponFovActive)
					{
						GL_SetProjectionMatrix( backEnd.viewParms.projectionMatrix );
					}

					qglDepthRange (0, 1);
				}

				oldDepthRange = depthRange;
				wasCrosshair = isCrosshair;
			}

            oldEntityNum = entityNum;
			//
			// OPENMOHAA-specific stuff
			//=========================
            oldbStaticModel = bStaticModel;
            //=========================
		}

		//
		// HZM gl2 re-port (bug-gl2-modellight): per-surface model lighting
		// setup, mirrors gl1 tr_backend.c:833-882. Selects and primes the
		// lighting source (grid color or spherical light set) that the CPU
		// color path reads when filling tess.color for rgbGen
		// lightingGrid / lightingSpherical / static stages.
		//
		if (bStaticModel)
		{
			if (r_drawspherelights->integer) {
				RB_Static_BuildDLights();
			}

			if (!backEnd.currentStaticModel->bLightGridCalculated) {
				RB_Grid_SetupStaticModel();
			}
		}
		else if (backEnd.currentEntity && backEnd.currentEntity->e.tiki && shader)
		{
			// HZM gl2 character lighting: a character skin has to take the SPHERE branch,
			// because the sphere is the only thing in the engine that knows which way the
			// light is coming from (RB_Sphere_Light_Sun's sky trace + the real map lights in
			// the leaf). r_fastentlight - which is 1 by default - would otherwise route it to
			// the flat one-colour grid path, which is precisely the "lit as if in full sun"
			// look this feature exists to remove. Characters only; every other TIKI entity
			// still obeys r_fastentlight exactly as before.
			qboolean charLit = RB_CharLightingWanted(shader);

			if (!charLit
				&& (shader->needsLGrid
				|| (shader->needsLSpherical && r_fastentlight->integer)
				|| !r_drawspherelights->integer))
			{
				backEnd.currentSphere->TessFunction = RB_CalcLightGridColor;
				RB_Grid_SetupEntity();
			}
			else if (shader->needsLSpherical)
			{
				if (tr.refdef.rdflags & RDF_HUD)
				{
					backEnd.currentSphere = &backEnd.hudSphere;
					backEnd.hudSphere.TessFunction = 0;
					RB_Sphere_SetupEntity();
				}
				// HZM gl2 (bug-gl2-sphereslot-alias): the cached index is only meaningful
				// inside the draw-surf list that issued it - see the sphereListId bump at the
				// top of this function. With r_sphereCacheScope 0 this is the original
				// frame-scoped test, byte for byte.
				else if (backEnd.currentEntity->sphereCalculated
					&& (!r_sphereCacheScope || !r_sphereCacheScope->integer
						|| backEnd.currentEntity->sphereList == backEnd.sphereListId))
				{
					backEnd.currentSphere = &backEnd.spheres[backEnd.currentEntity->lightingSphere];
				}
				else
				{
					if (backEnd.numSpheresUsed == MAX_SPHERE_LIGHTS)
					{
						ri.Printf(PRINT_DEVELOPER, "Spherical lighting: Ran out of space in the sphere array!\n");
						backEnd.currentSphere = &backEnd.spareSphere;
					}
					else
					{
						backEnd.currentSphere = &backEnd.spheres[backEnd.numSpheresUsed];
						backEnd.currentEntity->lightingSphere = backEnd.numSpheresUsed++;
						backEnd.currentEntity->sphereCalculated = qtrue;
						// HZM gl2 (bug-gl2-sphereslot-alias): record WHICH list issued the
						// index, so a later list in the same frame cannot inherit it.
						backEnd.currentEntity->sphereList = backEnd.sphereListId;
					}

					backEnd.currentSphere->TessFunction = NULL;
					RB_Sphere_SetupEntity();
				}

				// HZM gl2 character lighting: the sphere for this entity is now built (or
				// was reused from an earlier batch this frame, which is why this sits
				// outside the three branches above). Restate it for lightall. Inert unless
				// r_charLighting is on AND the sphere really produced light data.
				if (charLit)
				{
					RB_SetupCharLighting();
				}
			}
		}

        if (*drawSurf->surface == SF_SPRITE) {
            backEnd.shaderStartTime = ((refSprite_t*)drawSurf->surface)->shaderTime;
        }

		// add the triangles for this surface
		rb_surfaceTable[ *drawSurf->surface ]( drawSurf->surface );
	}

	// HZM gl2 real character shadows: r_shadowDebug 3 accounting. Also prints the cumulative
	// drawsurf count, because R_AddDrawSurf masks and WRAPS silently on overflow and - since
	// shadow views run before the main view - an overflow makes the MAIN view sort zero
	// surfaces and draw nothing.
	if (backEnd.depthFill && isShadowView && r_shadowDebug->integer == 3) {
		ri.Printf(PRINT_ALL,
			"^~^~^ SHADOWCAST cascade=%d surfs=%d chars=%d skipped=%d drawSurfTotal=%d\n",
			backEnd.viewParms.shadowCascade - 1, numDrawSurfs,
			rb_shadowCharsDrawn, rb_shadowCharsSkipped, backEnd.refdef.numDrawSurfs);
	}

	backEnd.refdef.floatTime = originalTime;

	// draw the contents of the last shader batch
	if (oldShader != NULL) {
		RB_EndSurface();
	}

	// HZM gl2 character lighting: the flush above legitimately needs the last surface's
	// charLight, so this comes AFTER it. Everything drawn from here on (2D/HUD, sky, the
	// post chain) must see a clear flag.
	backEnd.charLight.active = qfalse;

	if (glRefConfig.framebufferObject)
		FBO_Bind(fbo);

	// go back to the world modelview matrix

	GL_SetModelviewMatrix( backEnd.viewParms.world.modelMatrix );

	qglDepthRange (0, 1);
}


/*
============================================================================

RENDER BACK END FUNCTIONS

============================================================================
*/

/*
================
RB_SetGL2D

================
*/
void	RB_SetGL2D (void) {
	mat4_t matrix;
	int width, height;

	if (backEnd.projection2D && backEnd.last2DFBO == glState.currentFBO)
		return;

	backEnd.projection2D = qtrue;
	backEnd.last2DFBO = glState.currentFBO;

	if (glState.currentFBO)
	{
		width = glState.currentFBO->width;
		height = glState.currentFBO->height;
	}
	else
	{
		width = glConfig.vidWidth;
		height = glConfig.vidHeight;
	}

	// set 2D virtual screen size
	qglViewport( 0, 0, width, height );
	qglScissor( 0, 0, width, height );

	Mat4Ortho(0, width, height, 0, 0, 1, matrix);
	GL_SetProjectionMatrix(matrix);
	Mat4Identity(matrix);
	GL_SetModelviewMatrix(matrix);

	GL_State( GLS_DEPTHTEST_DISABLE |
			  GLS_SRCBLEND_SRC_ALPHA |
			  GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA );

	GL_Cull( CT_TWO_SIDED );

	// set time for 2D shaders
	backEnd.refdef.time = ri.Milliseconds();
	backEnd.refdef.floatTime = backEnd.refdef.time * 0.001;
}


/*
=============
RE_StretchRaw

FIXME: not exactly backend
Stretches a raw 32 bit power of 2 bitmap image over the given screen rectangle.
Used for cinematics.
=============
*/
void RE_StretchRaw (int x, int y, int w, int h, int cols, int rows, const byte *data, int client, qboolean dirty) {
	int			i, j;
	int			start, end;
	vec4_t quadVerts[4];
	vec2_t texCoords[4];

	if ( !tr.registered ) {
		return;
	}
	R_IssuePendingRenderCommands();

	if ( tess.numIndexes ) {
		RB_EndSurface();
	}

	// we definitely want to sync every frame for the cinematics
	qglFinish();

	start = 0;
	if ( r_speeds->integer ) {
		start = ri.Milliseconds();
	}

	// make sure rows and cols are powers of 2
	for ( i = 0 ; ( 1 << i ) < cols ; i++ ) {
	}
	for ( j = 0 ; ( 1 << j ) < rows ; j++ ) {
	}
	if ( ( 1 << i ) != cols || ( 1 << j ) != rows) {
		ri.Error (ERR_DROP, "Draw_StretchRaw: size not a power of 2: %i by %i", cols, rows);
	}

	RE_UploadCinematic (w, h, cols, rows, data, client, dirty);
	GL_BindToTMU(tr.scratchImage[client], TB_COLORMAP);

	if ( r_speeds->integer ) {
		end = ri.Milliseconds();
		ri.Printf( PRINT_ALL, "qglTexSubImage2D %i, %i: %i msec\n", cols, rows, end - start );
	}

	if (glRefConfig.framebufferObject)
	{
		FBO_Bind(tr.renderFbo);
	}

	RB_SetGL2D();

	VectorSet4(quadVerts[0], x,     y,     0.0f, 1.0f);
	VectorSet4(quadVerts[1], x + w, y,     0.0f, 1.0f);
	VectorSet4(quadVerts[2], x + w, y + h, 0.0f, 1.0f);
	VectorSet4(quadVerts[3], x,     y + h, 0.0f, 1.0f);

	VectorSet2(texCoords[0], 0.5f / cols,          0.5f / rows);
	VectorSet2(texCoords[1], (cols - 0.5f) / cols, 0.5f / rows);
	VectorSet2(texCoords[2], (cols - 0.5f) / cols, (rows - 0.5f) / rows);
	VectorSet2(texCoords[3], 0.5f / cols,          (rows - 0.5f) / rows);

	GLSL_BindProgram(&tr.textureColorShader);
	
	GLSL_SetUniformMat4(&tr.textureColorShader, UNIFORM_MODELVIEWPROJECTIONMATRIX, glState.modelviewProjection);
	GLSL_SetUniformVec4(&tr.textureColorShader, UNIFORM_COLOR, colorWhite);

	RB_InstantQuad2(quadVerts, texCoords);
}

void RE_UploadCinematic (int w, int h, int cols, int rows, const byte *data, int client, qboolean dirty) {
	byte *buffer;
	GLuint texture;

	if (!tr.scratchImage[client])
	{
		ri.Printf(PRINT_WARNING, "RE_UploadCinematic: scratch images not initialized\n");
		return;
	}

	texture = tr.scratchImage[client]->texnum;

	// if the scratchImage isn't in the format we want, specify it as a new texture
	if ( cols != tr.scratchImage[client]->width || rows != tr.scratchImage[client]->height ) {
		tr.scratchImage[client]->width = tr.scratchImage[client]->uploadWidth = cols;
		tr.scratchImage[client]->height = tr.scratchImage[client]->uploadHeight = rows;

		if ( qglesMajorVersion >= 1 ) {
			buffer = ri.Hunk_AllocateTempMemory( 3 * cols * rows );

			R_ConvertTextureFormat( data, cols, rows, GL_RGB, GL_UNSIGNED_BYTE, buffer );
			qglTextureImage2DEXT(texture, GL_TEXTURE_2D, 0, GL_RGB, cols, rows, 0, GL_RGB, GL_UNSIGNED_BYTE, buffer);

			ri.Hunk_FreeTempMemory( buffer );
		} else {
			qglTextureImage2DEXT(texture, GL_TEXTURE_2D, 0, GL_RGB8, cols, rows, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
		}

		qglTextureParameterfEXT(texture, GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		qglTextureParameterfEXT(texture, GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
		qglTextureParameterfEXT(texture, GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		qglTextureParameterfEXT(texture, GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	} else {
		if (dirty) {
			// otherwise, just subimage upload it so that drivers can tell we are going to be changing
			// it and don't try and do a texture compression
			if ( qglesMajorVersion >= 1 ) {
				buffer = ri.Hunk_AllocateTempMemory( 3 * cols * rows );

				R_ConvertTextureFormat( data, cols, rows, GL_RGB, GL_UNSIGNED_BYTE, buffer );
				qglTextureSubImage2DEXT(texture, GL_TEXTURE_2D, 0, 0, 0, cols, rows, GL_RGB, GL_UNSIGNED_BYTE, buffer);

				ri.Hunk_FreeTempMemory( buffer );
			} else {
				qglTextureSubImage2DEXT(texture, GL_TEXTURE_2D, 0, 0, 0, cols, rows, GL_RGBA, GL_UNSIGNED_BYTE, data);
			}
		}
	}
}


/*
=============
RB_SetColor

=============
*/
const void	*RB_SetColor( const void *data ) {
	const setColorCommand_t	*cmd;

	cmd = (const setColorCommand_t *)data;

	backEnd.color2D[0] = cmd->color[0] * 255;
	backEnd.color2D[1] = cmd->color[1] * 255;
	backEnd.color2D[2] = cmd->color[2] * 255;
	backEnd.color2D[3] = cmd->color[3] * 255;

	return (const void *)(cmd + 1);
}

/*
=============
RB_StretchPic
=============
*/
const void *RB_StretchPic ( const void *data ) {
	const stretchPicCommand_t	*cmd;
	shader_t *shader;
	int		numVerts, numIndexes;

	cmd = (const stretchPicCommand_t *)data;

	if (glRefConfig.framebufferObject)
		FBO_Bind(tr.renderFbo);

	RB_SetGL2D();

	shader = cmd->shader;
	if ( shader != tess.shader ) {
		if ( tess.numIndexes ) {
			RB_EndSurface();
		}
		backEnd.currentEntity = &backEnd.entity2D;
		RB_BeginSurface( shader, 0, 0 );
	}

	RB_CHECKOVERFLOW( 4, 6 );
	numVerts = tess.numVertexes;
	numIndexes = tess.numIndexes;

	tess.numVertexes += 4;
	tess.numIndexes += 6;

	tess.indexes[ numIndexes ] = numVerts + 3;
	tess.indexes[ numIndexes + 1 ] = numVerts + 0;
	tess.indexes[ numIndexes + 2 ] = numVerts + 2;
	tess.indexes[ numIndexes + 3 ] = numVerts + 2;
	tess.indexes[ numIndexes + 4 ] = numVerts + 0;
	tess.indexes[ numIndexes + 5 ] = numVerts + 1;

	{
		uint16_t color[4];

		VectorScale4(backEnd.color2D, 257, color);

		VectorCopy4(color, tess.color[ numVerts ]);
		VectorCopy4(color, tess.color[ numVerts + 1]);
		VectorCopy4(color, tess.color[ numVerts + 2]);
		VectorCopy4(color, tess.color[ numVerts + 3 ]);
	}

	tess.xyz[ numVerts ][0] = cmd->x;
	tess.xyz[ numVerts ][1] = cmd->y;
	tess.xyz[ numVerts ][2] = 0;

	tess.texCoords[ numVerts ][0] = cmd->s1;
	tess.texCoords[ numVerts ][1] = cmd->t1;

	tess.xyz[ numVerts + 1 ][0] = cmd->x + cmd->w;
	tess.xyz[ numVerts + 1 ][1] = cmd->y;
	tess.xyz[ numVerts + 1 ][2] = 0;

	tess.texCoords[ numVerts + 1 ][0] = cmd->s2;
	tess.texCoords[ numVerts + 1 ][1] = cmd->t1;

	tess.xyz[ numVerts + 2 ][0] = cmd->x + cmd->w;
	tess.xyz[ numVerts + 2 ][1] = cmd->y + cmd->h;
	tess.xyz[ numVerts + 2 ][2] = 0;

	tess.texCoords[ numVerts + 2 ][0] = cmd->s2;
	tess.texCoords[ numVerts + 2 ][1] = cmd->t2;

	tess.xyz[ numVerts + 3 ][0] = cmd->x;
	tess.xyz[ numVerts + 3 ][1] = cmd->y + cmd->h;
	tess.xyz[ numVerts + 3 ][2] = 0;

	tess.texCoords[ numVerts + 3 ][0] = cmd->s1;
	tess.texCoords[ numVerts + 3 ][1] = cmd->t2;

	return (const void *)(cmd + 1);
}


/*
=============
RB_SetupGlobalFog

HZM gl2 fog parity - the counterpart of gl1's RB_SetupFog (renderergl1/tr_backend.c), called
from the same place (RB_DrawSurfs, right after backEnd.viewParms is taken from the command)
for the same reason: this is the ONLY point where the fog parameters and the projection
matrix that will actually rasterise the view are guaranteed to belong to the same view.

gl1 can push the state straight into fixed-function GL here. gl2 has to defer to a
screen-space pass in RB_PostProcess, and that pass runs off a SEPARATE RC_POSTPROCESS
command carrying its own copy of tr.viewParms - which may have been left pointing at a
portal / sky-portal / shadow sub-view, or carry a projection matrix that was rebuilt after
the world was drawn. Latching here removes that entire class of divergence.
=============
*/
globalFogState_t rb_globalFog;
viewProjLatch_t  rb_viewProj;
hzmSkyFogLatch_t rb_hzmSkyFogLatch;	// HZM sky fog model: the main view's fog, for the portal-sky view of the next frame

void RB_SetupGlobalFog( void ) {
	const float	*m;
	float		identityLight;

	// a view with no world (HUD 3D icons, the menu) can never have global fog. These are
	// drawn AFTER the world's RC_POSTPROCESS in the same frame, so clearing here cannot
	// disarm the world pass, and it stops a stale latch leaking into a worldless frame.
	if ( backEnd.refdef.rdflags & RDF_NOWORLDMODEL ) {
		rb_globalFog.active = qfalse;
		rb_viewProj.valid   = qfalse;
		return;
	}

	// portal / sky-portal / shadow sub-views are rendered BEFORE the outer view's
	// RC_DRAWSURFS is queued, so leaving the latch alone here means the main view always
	// gets the last word
	if ( backEnd.viewParms.isPortal || backEnd.viewParms.isPortalSky
		|| ( backEnd.viewParms.flags & (VPF_SHADOWMAP | VPF_DEPTHSHADOW) ) ) {
		return;
	}

	// HZM [UNDERWATER VOLUME v3] Unconditional projection latch. This sits ABOVE every
	// fog-specific test on purpose - see viewProjLatch_t in tr_local.h. It cannot alter fog
	// behaviour: it writes only rb_viewProj and returns nothing. It sits BELOW the portal /
	// sky-portal / shadow guard on purpose too, so a sub-view can never supply the matrix.
	{
		const float *pm = backEnd.viewParms.projectionMatrix;

		// same well-formed-perspective test the fog uses below: anything else (ortho, an
		// oblique portal matrix, an uninitialised one) cannot be inverted the way the shader
		// does, so refuse rather than hand out a wrong distance
		if ( pm[14] < 0.0f && pm[10] < -1.0f ) {
			rb_viewProj.projMat10 = pm[10];
			rb_viewProj.projMat14 = pm[14];
			rb_viewProj.projMat0  = pm[0];
			rb_viewProj.projMat5  = pm[5];
			rb_viewProj.zNear     = pm[14] / ( pm[10] - 1.0f );
			rb_viewProj.zFar      = pm[14] / ( pm[10] + 1.0f );
			rb_viewProj.valid     = qtrue;
		} else {
			rb_viewProj.valid     = qfalse;
		}
	}

	rb_globalFog.active = qfalse;
	// HZM sky fog model (2026-10-05): the latch follows the main view only - fog off here, until set below
	rb_hzmSkyFogLatch.active = qfalse;
	rb_hzmSkyFogLatch.frame  = tr.frameCount;

	// gl1 RB_SetupFog: no farplane -> no fog; r_farplane_nofog -> no fog
	if ( backEnd.viewParms.farplane_distance <= 0.0f ) {
		return;
	}
	if ( r_farplane_nofog && r_farplane_nofog->integer ) {
		return;
	}
	if ( r_globalFog && !r_globalFog->integer ) {
		return;
	}

	m = backEnd.viewParms.projectionMatrix;

	// a well-formed perspective projection has [10] < -1 and [14] < 0. Anything else
	// (ortho, an oblique portal matrix, an uninitialised matrix) cannot be inverted the
	// way the shader does, so bail rather than draw wrong fog.
	if ( m[14] >= 0.0f || m[10] >= -1.0f ) {
		return;
	}

	rb_globalFog.projMat10 = m[10];
	rb_globalFog.projMat14 = m[14];
	rb_globalFog.projMat0  = m[0];
	rb_globalFog.projMat5  = m[5];
	rb_globalFog.zNear     = m[14] / ( m[10] - 1.0f );
	rb_globalFog.zFar      = m[14] / ( m[10] + 1.0f );

	// gl1: GL_FOG_START = farplane_bias, GL_FOG_END = farplane_distance
	rb_globalFog.start = backEnd.viewParms.farplane_bias;
	rb_globalFog.end   = backEnd.viewParms.farplane_distance;

	if ( r_globalFogStartScale ) {
		rb_globalFog.start *= r_globalFogStartScale->value;
	}
	if ( r_globalFogEndScale ) {
		rb_globalFog.end *= r_globalFogEndScale->value;
	}

	if ( rb_globalFog.end <= rb_globalFog.start ) {
		return;
	}

	// gl1 multiplies by tr.identityLight, but gl1 defaults r_overBrightBits to 0 so its
	// identityLight is 1.0 and the on-screen fog colour is the raw farplane_color.
	//
	// [2026-08-03, bug-1306] THE COMMENT THAT USED TO BE HERE WAS WRONG and is deleted. It
	// claimed gl2 mixes fog in the same final DISPLAY space gl1 blends in, so the raw colour
	// was correct only by accident of the post-tone ordering. gl2's tone stage IS gl1's grade -
	// glsl/tonemap_hzm_fp.glsl is a port of renderergl1 TONEMAP_FS (tr_postprocess_gl1.c:280),
	// reached unconditionally at tr_postprocess.c:111 because r_ppTonemap is 1 in the shipped
	// config. gl1's on-screen answer for a fully fogged pixel is GRADE(farplane_color), NOT
	// farplane_color, so a fog mixed BEFORE the tone stage (r_globalFogForward) must still mix
	// toward the RAW value. Do not pre-invert this colour: an inverse grade is
	// exposure-dependent and would make the horizon pump under auto-exposure.
	// r_globalFogIdentityLight 1 restores the literal gl1 expression for A/B.
	identityLight = ( r_globalFogIdentityLight && r_globalFogIdentityLight->integer )
		? tr.identityLight : 1.0f;

	rb_globalFog.color[0] = backEnd.viewParms.farplane_color[0] * identityLight;
	rb_globalFog.color[1] = backEnd.viewParms.farplane_color[1] * identityLight;
	rb_globalFog.color[2] = backEnd.viewParms.farplane_color[2] * identityLight;

	rb_globalFog.active = qtrue;

	// HZM sky fog model (2026-10-05): what a portal-sky view (rendered BEFORE this view, every frame) fogs its 3D skybox with
	rb_hzmSkyFogLatch.active = qtrue;
	rb_hzmSkyFogLatch.frame  = tr.frameCount;
	rb_hzmSkyFogLatch.start  = rb_globalFog.start;
	rb_hzmSkyFogLatch.end    = rb_globalFog.end;
	VectorCopy( rb_globalFog.color, rb_hzmSkyFogLatch.color );
}

/*
=============
RB_DrawSurfs

=============
*/
const void	*RB_DrawSurfs( const void *data ) {
	const drawSurfsCommand_t	*cmd;
	qboolean isShadowView;
	int gpuMarkKind;

	// HZM gl2 (bug #73): a real scene rendered this frame - the sceneless-frame
	// stale-FBO clear (R_Ensure2DClear) must not fire.
	g_sceneThisFrame = 1;

	// finish any 2D drawing if needed
	if ( tess.numIndexes ) {
		RB_EndSurface();
	}

	cmd = (const drawSurfsCommand_t *)data;

	backEnd.refdef = cmd->refdef;
	backEnd.viewParms = cmd->viewParms;

	isShadowView = !!(backEnd.viewParms.flags & VPF_DEPTHSHADOW);

	// HZM gl2 graphics probes: GPU timestamps per view class (r_gfxProbe 1 only).
	gpuMarkKind = isShadowView ? (backEnd.viewParms.shadowCascade > 0 ? GPUMARK_SUN_B : GPUMARK_SHOTH_B)
	            : ((backEnd.refdef.rdflags & RDF_NOWORLDMODEL) ? -1 : GPUMARK_MAIN_B);
	if (gpuMarkKind >= 0) {
		R_GfxProbe_GpuMark(gpuMarkKind);
	}

	// HZM gl2 SSAO black-screen fix: invalidate last frame's AO at the start of every real
	// (non-shadow) view. Only the pass that actually writes tr.screenSsaoFbo may set this back
	// to qtrue, so the multiply composite in RB_PostProcess can never run against an AO buffer
	// that was not produced for this frame - including the virgin, zero-filled (BLACK) one that
	// exists right after allocation. See backEndState_t::ssaoValid.
	if (!isShadowView) {
		backEnd.ssaoValid = qfalse;
		// HZM gl2 SOFT PARTICLES: the sprite-list depth snapshot for this view has not run yet.
		backEnd.softDepthValid = qfalse;
	}

	// HZM gl2 fog parity: latch the global farplane fog for this view, mirroring gl1's
	// RB_SetupFog call site. MUST run before RB_BeginDrawingView, which clears the colour
	// buffer to the fog colour when the farplane fog is active (gl1 tr_backend.c:636).
	RB_SetupGlobalFog();

	// clear the z buffer, set the modelview, etc
	RB_BeginDrawingView ();

	if (glRefConfig.framebufferObject && (backEnd.viewParms.flags & VPF_DEPTHCLAMP) && glRefConfig.depthClamp)
	{
		qglEnable(GL_DEPTH_CLAMP);
	}

	// HZM gl2 real character shadows: third and last r_depthPrepass gate. This block owns both
	// the depth-fill pass AND the shadowmask resolve below; with r_depthPrepass archived to 0
	// the mask is never generated no matter how many cascades were rendered.
	if (glRefConfig.framebufferObject && !(backEnd.refdef.rdflags & RDF_NOWORLDMODEL)
	    && (r_depthPrepass->integer || isShadowView || R_CharShadowsActive()))
	{
		FBO_t *oldFbo = glState.currentFBO;
		vec4_t viewInfo;

		VectorSet4(viewInfo, backEnd.viewParms.zFar / r_znear->value, backEnd.viewParms.zFar, 0.0, 0.0);

		backEnd.depthFill = qtrue;
		qglColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
		// HZM gl2 P3: the W bake is one view with one or two depth passes (r_shadowWFaces, vet section 7 D2)
		backEnd.hzmWPass = (backEnd.viewParms.flags & VPF_SUNWORLD) ? 1 : 0;
		RB_RenderDrawSurfList( cmd->drawSurfs, cmd->numDrawSurfs );
		if ((backEnd.viewParms.flags & VPF_SUNWORLD) && backEnd.viewParms.hzmWFaces == 2)
		{
			backEnd.hzmWPass = 2;
			RB_RenderDrawSurfList( cmd->drawSurfs, cmd->numDrawSurfs );
		}
		backEnd.hzmWPass = 0;
		qglColorMask(!backEnd.colorMask[0], !backEnd.colorMask[1], !backEnd.colorMask[2], !backEnd.colorMask[3]);
		backEnd.depthFill = qfalse;

		if (!isShadowView)
		{
			if (R_MsaaResolvesActive() && backEnd.viewParms.targetFbo == NULL)
			{
				// HZM gl2 MSAA (plan P2b): the shader depth resolve - the NEAREST sample into renderDepthImage (the
				// shadow mask below is right for the foreground), and (min, max) into msaaDepthMinMaxImage
				RB_MSAAResolveDepth();
			}
			else if (tr.msaaResolveFbo)
			{
				// If we're using multisampling, resolve the depth first
				FBO_FastBlit(tr.sceneFbo, NULL, tr.msaaResolveFbo, NULL, GL_DEPTH_BUFFER_BIT, GL_NEAREST);   // HZM render scale: scene is the MSAA target
			}
			else if (tr.renderFbo == NULL && tr.renderDepthImage)
			{
				// If we're rendering directly to the screen, copy the depth to a texture
				// This is incredibly slow on Intel Graphics, so just skip it on there
				if (!glRefConfig.intelGraphics)
					qglCopyTextureSubImage2DEXT(tr.renderDepthImage->texnum, GL_TEXTURE_2D, 0, 0, 0, 0, 0, glConfig.vidWidth, glConfig.vidHeight);
			}

			if (tr.hdrDepthFbo)
			{
				// need the depth in a texture we can do GL_LINEAR sampling on, so copy it to an HDR image
				vec4_t srcTexCoords;

				VectorSet4(srcTexCoords, 0.0f, 0.0f, 1.0f, 1.0f);

				FBO_BlitFromTexture(tr.renderDepthImage, srcTexCoords, NULL, tr.hdrDepthFbo, NULL, NULL, NULL, 0);
			}

			if (r_sunlightMode->integer && backEnd.viewParms.flags & VPF_USESUNLIGHT)
			{
				vec4_t quadVerts[4];
				vec2_t texCoords[4];
				vec4_t box;

				FBO_Bind(tr.screenShadowFbo);

				// HZM render scale: screenShadowFbo and the viewport are both SCENE-sized, so normalise by scene dims.
				box[0] = backEnd.viewParms.viewportX      * tr.screenShadowFbo->width / (float)tr.sceneWidth;
				box[1] = backEnd.viewParms.viewportY      * tr.screenShadowFbo->height / (float)tr.sceneHeight;
				box[2] = backEnd.viewParms.viewportWidth  * tr.screenShadowFbo->width / (float)tr.sceneWidth;
				box[3] = backEnd.viewParms.viewportHeight * tr.screenShadowFbo->height / (float)tr.sceneHeight;

				qglViewport(box[0], box[1], box[2], box[3]);
				qglScissor(box[0], box[1], box[2], box[3]);

				box[0] = backEnd.viewParms.viewportX / (float)tr.sceneWidth;
				box[1] = backEnd.viewParms.viewportY / (float)tr.sceneHeight;
				box[2] = box[0] + backEnd.viewParms.viewportWidth / (float)tr.sceneWidth;
				box[3] = box[1] + backEnd.viewParms.viewportHeight / (float)tr.sceneHeight;

				texCoords[0][0] = box[0]; texCoords[0][1] = box[3];
				texCoords[1][0] = box[2]; texCoords[1][1] = box[3];
				texCoords[2][0] = box[2]; texCoords[2][1] = box[1];
				texCoords[3][0] = box[0]; texCoords[3][1] = box[1];

				box[0] = -1.0f;
				box[1] = -1.0f;
				box[2] = 1.0f;
				box[3] = 1.0f;

				VectorSet4(quadVerts[0], box[0], box[3], 0, 1);
				VectorSet4(quadVerts[1], box[2], box[3], 0, 1);
				VectorSet4(quadVerts[2], box[2], box[1], 0, 1);
				VectorSet4(quadVerts[3], box[0], box[1], 0, 1);

				GL_State(GLS_DEPTHTEST_DISABLE);

				if (backEnd.refdef.hzmSunActive)
				{
					// HZM gl2 stable sun shadows + B0 (plan P3): radial entity cascades only; r_shadowBlur is ignored
					RB_SunStable_Mask(quadVerts, texCoords, viewInfo);
				}
				else
				{
				GLSL_BindProgram(&tr.shadowmaskShader);

				GL_BindToTMU(tr.renderDepthImage, TB_COLORMAP);

				if (r_shadowCascadeZFar->integer != 0)
				{
					GL_BindToTMU(tr.sunShadowDepthImage[0], TB_SHADOWMAP);
					GL_BindToTMU(tr.sunShadowDepthImage[1], TB_SHADOWMAP2);
					GL_BindToTMU(tr.sunShadowDepthImage[2], TB_SHADOWMAP3);
					GL_BindToTMU(tr.sunShadowDepthImage[3], TB_SHADOWMAP4);

					GLSL_SetUniformMat4(&tr.shadowmaskShader, UNIFORM_SHADOWMVP, backEnd.refdef.sunShadowMvp[0]);
					GLSL_SetUniformMat4(&tr.shadowmaskShader, UNIFORM_SHADOWMVP2, backEnd.refdef.sunShadowMvp[1]);
					GLSL_SetUniformMat4(&tr.shadowmaskShader, UNIFORM_SHADOWMVP3, backEnd.refdef.sunShadowMvp[2]);
					GLSL_SetUniformMat4(&tr.shadowmaskShader, UNIFORM_SHADOWMVP4, backEnd.refdef.sunShadowMvp[3]);
				}
				else
				{
					GL_BindToTMU(tr.sunShadowDepthImage[3], TB_SHADOWMAP);
					GLSL_SetUniformMat4(&tr.shadowmaskShader, UNIFORM_SHADOWMVP, backEnd.refdef.sunShadowMvp[3]);
				}

				GLSL_SetUniformVec3(&tr.shadowmaskShader, UNIFORM_VIEWORIGIN, backEnd.refdef.vieworg);
				{
					vec3_t viewVector;

					float zmax = backEnd.viewParms.zFar;
					float ymax = zmax * tan(backEnd.viewParms.fovY * M_PI / 360.0f);
					float xmax = zmax * tan(backEnd.viewParms.fovX * M_PI / 360.0f);

					VectorScale(backEnd.refdef.viewaxis[0], zmax, viewVector);
					GLSL_SetUniformVec3(&tr.shadowmaskShader, UNIFORM_VIEWFORWARD, viewVector);
					VectorScale(backEnd.refdef.viewaxis[1], xmax, viewVector);
					GLSL_SetUniformVec3(&tr.shadowmaskShader, UNIFORM_VIEWLEFT, viewVector);
					VectorScale(backEnd.refdef.viewaxis[2], ymax, viewVector);
					GLSL_SetUniformVec3(&tr.shadowmaskShader, UNIFORM_VIEWUP, viewVector);

					GLSL_SetUniformVec4(&tr.shadowmaskShader, UNIFORM_VIEWINFO, viewInfo);
				}

				RB_InstantQuad2(quadVerts, texCoords); //, color, shaderProgram, invTexRes);

				if (r_shadowBlur->integer)
				{
					viewInfo[2] = 1.0f / (float)(tr.screenScratchFbo->width);
					viewInfo[3] = 1.0f / (float)(tr.screenScratchFbo->height);

					FBO_Bind(tr.screenScratchFbo);

					GLSL_BindProgram(&tr.depthBlurShader[0]);

					GL_BindToTMU(tr.screenShadowImage, TB_COLORMAP);
					GL_BindToTMU(tr.hdrDepthImage, TB_LIGHTMAP);

					GLSL_SetUniformVec4(&tr.depthBlurShader[0], UNIFORM_VIEWINFO, viewInfo);

					RB_InstantQuad2(quadVerts, texCoords);

					FBO_Bind(tr.screenShadowFbo);

					GLSL_BindProgram(&tr.depthBlurShader[1]);

					GL_BindToTMU(tr.screenScratchImage, TB_COLORMAP);
					GL_BindToTMU(tr.hdrDepthImage, TB_LIGHTMAP);

					GLSL_SetUniformVec4(&tr.depthBlurShader[1], UNIFORM_VIEWINFO, viewInfo);

					RB_InstantQuad2(quadVerts, texCoords);
				}
				}   // HZM gl2 P3: end of the legacy mask
			}

			// HZM gl2 SSAO BLACK-SCREEN FIX (bug-1177 follow-up): the SSAO GENERATION pass used to
			// live right here, nested inside this r_depthPrepass-gated block. The AO COMPOSITE in
			// RB_PostProcess was NOT gated the same way, so with r_depthPrepass 0 (which the gl2
			// sandbox archives) generation never ran, and the composite multiplied the frame by an
			// all-zero (never-written) AO image -> total black screen. Generation now runs from
			// RB_HZMSsao() in tr_postprocess.c, which takes its own depth snapshot exactly the way
			// RB_HZMDof does and is therefore independent of r_depthPrepass.
		}

		// reset viewport and scissor
		FBO_Bind(oldFbo);
		SetViewportAndScissor();
	}

	if (glRefConfig.framebufferObject && (backEnd.viewParms.flags & VPF_DEPTHCLAMP) && glRefConfig.depthClamp)
	{
		qglDisable(GL_DEPTH_CLAMP);
	}

	if (!isShadowView)
	{
		RB_RenderDrawSurfList( cmd->drawSurfs, cmd->numDrawSurfs );

		// HZM coop [2026-09-27] realistic lightning: the lit cloud lobe and the far bolt, additive at depth range (1,1)
		// so only sky pixels take them. Returns at once (no GL call) unless a strike is lit on the main view.
		RB_HzmLt_SkyPass();

		if (r_drawSun->integer)
		{
			RB_DrawSun(0.1, tr.sunShader);
		}

		// HZM gl2 MSAA (plan P2b, ME-F13): resolve the FINISHED opaque depth once per view, before the sun-rays mask and
		// the flares read renderDepthImage. The legacy blit path left them the prepass depth, which lacks the
		// alpha-tested world and characters (they never enter the prepass), so a flare behind foliage stayed lit.
		if (R_MsaaResolvesActive() && backEnd.viewParms.targetFbo == NULL
		    && !( backEnd.refdef.rdflags & RDF_NOWORLDMODEL ))
		{
			RB_MSAAResolveDepth();
			SetViewportAndScissor();
		}

		if (glRefConfig.framebufferObject && r_drawSunRays->integer)
		{
			FBO_t *oldFbo = glState.currentFBO;
			FBO_Bind(tr.sunRaysFbo);
			
			qglClearColor( 0.0f, 0.0f, 0.0f, 1.0f );
			qglClear( GL_COLOR_BUFFER_BIT );

			if (glRefConfig.occlusionQuery)
			{
				tr.sunFlareQueryActive[tr.sunFlareQueryIndex] = qtrue;
				qglBeginQuery(glRefConfig.occlusionQueryTarget, tr.sunFlareQuery[tr.sunFlareQueryIndex]);
			}

			RB_DrawSun(0.3, tr.sunFlareShader);

			if (glRefConfig.occlusionQuery)
			{
				qglEndQuery(glRefConfig.occlusionQueryTarget);
			}

			FBO_Bind(oldFbo);
		}

		// darken down any stencil shadows
		RB_ShadowFinish();		

		// add light flares on lights that aren't obscured
		RB_RenderFlares();
	}

	if (glRefConfig.framebufferObject && tr.renderCubeFbo && backEnd.viewParms.targetFbo == tr.renderCubeFbo)
	{
		cubemap_t *cubemap = &tr.cubemaps[backEnd.viewParms.targetFboCubemapIndex];

		FBO_Bind(NULL);
		if (cubemap && cubemap->image)
			qglGenerateTextureMipmapEXT(cubemap->image->texnum, GL_TEXTURE_CUBE_MAP);
	}

	// FIXME? backEnd.viewParms doesn't get properly initialized for 2D drawing.
	// r_cubeMapping 1 generates cubemaps with R_RenderCubemapSide()
	// and sets isMirror = qtrue. Clear it here to prevent it from leaking
	// to 2D drawing and causing the loading screen to be culled.
	backEnd.viewParms.isMirror = qfalse;
	backEnd.viewParms.flags = 0;

	if (gpuMarkKind >= 0) {
		R_GfxProbe_GpuMark(gpuMarkKind + 1);
	}

	return (const void *)(cmd + 1);
}


/*
=============
RB_DrawBuffer

=============
*/
const void	*RB_DrawBuffer( const void *data ) {
	const drawBufferCommand_t	*cmd;

	cmd = (const drawBufferCommand_t *)data;

	// finish any 2D drawing if needed
	if(tess.numIndexes)
		RB_EndSurface();

	if (glRefConfig.framebufferObject)
		FBO_Bind(NULL);

	qglDrawBuffer( cmd->buffer );

	// HZM gl2 (bug #73 ghost-gun): clear the persistent render FBO at the START of the frame.
	// This was previously done at the END of RB_SwapBuffers, which zeroed the very buffer the
	// screenshot path reads afterwards -> in-game screenshots came out PURE BLACK (user report
	// 2026-07-28, shot0011/0012 max luma 0). Clearing here is equally effective against the
	// ghost (nothing of the previous frame survives into this frame's 2D-only draws) and leaves
	// the presented image intact for screenshots/video capture.
	if ( glRefConfig.framebufferObject && tr.renderFbo ) {
		static cvar_t *r_uiFrameClearBegin = NULL;

		if (!r_uiFrameClearBegin) {
			r_uiFrameClearBegin = ri.Cvar_Get("r_uiFrameClear", "1", CVAR_ARCHIVE);
		}

		if (r_uiFrameClearBegin->integer) {
			FBO_Bind(tr.renderFbo);
			qglDisable(GL_SCISSOR_TEST);
			qglClearColor(0.0f, 0.0f, 0.0f, 1.0f);
			qglClear(GL_COLOR_BUFFER_BIT);
			// HZM gl2 MSAA (plan P2a): with the display split the world scene has a buffer of its own. Clear it too,
			// as the MSAA-0 pipeline does (there sceneFbo IS renderFbo), so the bypass and override 0 match even
			// where the world leaves holes. New path only - the render-scale path is left exactly as it is.
			if (tr.msaaNewPath && tr.sceneFbo && tr.sceneFbo != tr.renderFbo) {
				FBO_Bind(tr.sceneFbo);
				qglClear(GL_COLOR_BUFFER_BIT);
			}
			FBO_Bind(NULL);
		}
	}

	// clear screen for debugging
	if ( r_clear->integer ) {
		if (glRefConfig.framebufferObject && tr.renderFbo) {
			FBO_Bind(tr.renderFbo);
		}

		qglClearColor( 1, 0, 0.5, 1 );
		qglClear( GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT );
	}

	return (const void *)(cmd + 1);
}

/*
===============
RB_ShowImages

Draw all the images to the screen, on top of whatever
was there.  This is used to test for texture thrashing.

Also called by RE_EndRegistration
===============
*/
void RB_ShowImages( void ) {
	int		i;
	image_t	*image;
	float	x, y, w, h;
	int		start, end;

	RB_SetGL2D();

	qglClear( GL_COLOR_BUFFER_BIT );

	qglFinish();

	start = ri.Milliseconds();

	for ( i=0 ; i<tr.numImages ; i++ ) {
		image = tr.images[i];

		// HZM gl2 MSAA (plan P2a, ME-F8): a multisample texture cannot be drawn as a 2D image
		if (image->flags & IMGFLAG_MULTISAMPLE) {
			continue;
		}

		w = glConfig.vidWidth / 20;
		h = glConfig.vidHeight / 15;
		x = i % 20 * w;
		y = i / 20 * h;

		// show in proportional size in mode 2
		if ( r_showImages->integer == 2 ) {
			w *= image->uploadWidth / 512.0f;
			h *= image->uploadHeight / 512.0f;
		}

		{
			vec4_t quadVerts[4];

			GL_BindToTMU(image, TB_COLORMAP);

			VectorSet4(quadVerts[0], x, y, 0, 1);
			VectorSet4(quadVerts[1], x + w, y, 0, 1);
			VectorSet4(quadVerts[2], x + w, y + h, 0, 1);
			VectorSet4(quadVerts[3], x, y + h, 0, 1);

			RB_InstantQuad(quadVerts);
		}
	}

	qglFinish();

	end = ri.Milliseconds();
	ri.Printf( PRINT_ALL, "%i msec to draw all images\n", end - start );

}

/*
=============
RB_ColorMask

=============
*/
const void *RB_ColorMask(const void *data)
{
	const colorMaskCommand_t *cmd = data;

	// finish any 2D drawing if needed
	if(tess.numIndexes)
		RB_EndSurface();

	if (glRefConfig.framebufferObject)
	{
		// reverse color mask, so 0 0 0 0 is the default
		backEnd.colorMask[0] = !cmd->rgba[0];
		backEnd.colorMask[1] = !cmd->rgba[1];
		backEnd.colorMask[2] = !cmd->rgba[2];
		backEnd.colorMask[3] = !cmd->rgba[3];
	}

	qglColorMask(cmd->rgba[0], cmd->rgba[1], cmd->rgba[2], cmd->rgba[3]);
	
	return (const void *)(cmd + 1);
}

/*
=============
RB_ClearDepth

=============
*/
const void *RB_ClearDepth(const void *data)
{
	const clearDepthCommand_t *cmd = data;
	
	// finish any 2D drawing if needed
	if(tess.numIndexes)
		RB_EndSurface();

	// texture swapping test
	if (r_showImages->integer)
		RB_ShowImages();

	if (glRefConfig.framebufferObject)
	{
		FBO_Bind(tr.renderFbo);
	}

	qglClear(GL_DEPTH_BUFFER_BIT);

	// if we're doing MSAA, clear the depth texture for the resolve buffer
	if (tr.msaaResolveFbo)
	{
		FBO_Bind(tr.msaaResolveFbo);
		qglClear(GL_DEPTH_BUFFER_BIT);
	}

	
	return (const void *)(cmd + 1);
}


/*
=============
RB_SwapBuffers

=============
*/
const void	*RB_SwapBuffers( const void *data ) {
	const swapBuffersCommand_t	*cmd;

	// HZM gl2 (bug #73): new frame starts after this swap - re-arm the sceneless-frame clear
	g_sceneThisFrame = 0;

	// finish any 2D drawing if needed
	if ( tess.numIndexes ) {
		RB_EndSurface();
	}

	// texture swapping test
	if ( r_showImages->integer ) {
		RB_ShowImages();
	}

	cmd = (const swapBuffersCommand_t *)data;

	// we measure overdraw by reading back the stencil buffer and
	// counting up the number of increments that have happened
	if ( r_measureOverdraw->integer ) {
		int i;
		long sum = 0;
		unsigned char *stencilReadback;

		stencilReadback = ri.Hunk_AllocateTempMemory( glConfig.vidWidth * glConfig.vidHeight );
		qglReadPixels( 0, 0, glConfig.vidWidth, glConfig.vidHeight, GL_STENCIL_INDEX, GL_UNSIGNED_BYTE, stencilReadback );

		for ( i = 0; i < glConfig.vidWidth * glConfig.vidHeight; i++ ) {
			sum += stencilReadback[i];
		}

		backEnd.pc.c_overDraw += sum;
		ri.Hunk_FreeTempMemory( stencilReadback );
	}

	if (glRefConfig.framebufferObject)
	{
		if (tr.msaaResolveFbo && r_hdr->integer && !tr.renderScaleActive && !tr.displaySplit)   // HZM render scale / MSAA display split: renderFbo is then already the single-sampled display
		{
			// Resolving an RGB16F MSAA FBO to the screen messes with the brightness, so resolve to an RGB16F FBO first
			FBO_FastBlit(tr.renderFbo, NULL, tr.msaaResolveFbo, NULL, GL_COLOR_BUFFER_BIT, GL_NEAREST);
			FBO_FastBlit(tr.msaaResolveFbo, NULL, NULL, NULL, GL_COLOR_BUFFER_BIT, GL_NEAREST);
		}
		else if (tr.renderFbo)
		{
			FBO_FastBlit(tr.renderFbo, NULL, NULL, NULL, GL_COLOR_BUFFER_BIT, GL_NEAREST);
		}

		// HZM gl2 (bug #73 ghost-gun-over-menus): the ghost-clear used to live HERE, after the
		// present. That zeroed the very buffer the screenshot path reads next, so in-game
		// screenshots came out pure black (user report 2026-07-28). It now runs at the START
		// of the frame in RB_DrawBuffer, which prevents the ghost just as well and leaves the
		// presented image readable.
	}

	// HZM gl2 graphics probes: frame-end timestamp + readback of older frames, WCRC, glGetError drain.
	R_GfxProbe_BackendFrameEnd();

	if ( !glState.finishCalled ) {
		qglFinish();
	}

	GLimp_LogComment( "***************** RB_SwapBuffers *****************\n\n\n" );

	GLimp_EndFrame();

	backEnd.projection2D = qfalse;

	return (const void *)(cmd + 1);
}

/*
=============
RB_CapShadowMap

=============
*/
const void *RB_CapShadowMap(const void *data)
{
	const capShadowmapCommand_t *cmd = data;

	// finish any 2D drawing if needed
	if(tess.numIndexes)
		RB_EndSurface();

	if (cmd->map != -1)
	{
		if (cmd->cubeSide != -1)
		{
			if (tr.shadowCubemaps[cmd->map])
			{
				qglCopyTextureSubImage2DEXT(tr.shadowCubemaps[cmd->map]->texnum, GL_TEXTURE_CUBE_MAP_POSITIVE_X + cmd->cubeSide, 0, 0, 0, backEnd.refdef.x, glConfig.vidHeight - ( backEnd.refdef.y + PSHADOW_MAP_SIZE ), PSHADOW_MAP_SIZE, PSHADOW_MAP_SIZE);
			}
		}
		else
		{
			if (tr.pshadowMaps[cmd->map])
			{
				qglCopyTextureSubImage2DEXT(tr.pshadowMaps[cmd->map]->texnum, GL_TEXTURE_2D, 0, 0, 0, backEnd.refdef.x, glConfig.vidHeight - (backEnd.refdef.y + PSHADOW_MAP_SIZE), PSHADOW_MAP_SIZE, PSHADOW_MAP_SIZE);
			}
		}
	}

	return (const void *)(cmd + 1);
}


/*
=============
RB_PostProcess

=============
*/
const void *RB_PostProcess(const void *data)
{
	const postProcessCommand_t *cmd = data;
	FBO_t *srcFbo, *dstFbo;
	ivec4_t srcBox, dstBox;
	qboolean autoExposure;
	qboolean bloomPreMeasured = qfalse;   // HZM exposure-aware bloom (bug-1149): measured before bloom in mode 1
	qboolean msaaShaderResolved = qfalse; // HZM gl2 MSAA (plan P2b): this chain began with the shader resolves
	static cvar_t *r_ppBloomModeBE = NULL, *r_ppBloomBE = NULL;

	// finish any 2D drawing if needed
	if(tess.numIndexes)
		RB_EndSurface();

	// HZM render scale: does RCAS run this frame? (scene scaled AND r_fsrSharpness > 0). Set before
	// the early-out and before RB_HZMScreenFx, which reads tr.rcasActive to skip r_ppSharpen.
	tr.rcasActive = (qboolean)(tr.renderScaleActive && tr.displayScratchFbo
	                           && r_fsrSharpness && r_fsrSharpness->value > 0.0f);

	if (!glRefConfig.framebufferObject || !r_postProcess->integer)
	{
		// HZM render scale: even with post-processing off, the scaled scene still has to reach the
		// display FBO, or the HUD / present would draw over an empty native buffer.
		// HZM gl2 MSAA (plan P2a): the same holds for the MSAA display split at scale 1.0 - a plain copy.
		if (glRefConfig.framebufferObject && (tr.renderScaleActive || tr.displaySplit)
		    && tr.sceneFbo && tr.renderFbo && tr.sceneFbo != tr.renderFbo)
		{
			FBO_t *rsSrc = tr.sceneFbo;
			if (R_MsaaResolvesActive())
			{
				// HZM gl2 MSAA (plan P2b): no tone stage runs with post-processing off - the per-sample clamp
				// resolve is what the display shows (and what gl1's clamped multisample buffer gives)
				RB_MSAAResolveColor(qfalse, 1, NULL);
				backEnd.msaaLinearValid = qfalse;
				rsSrc = tr.msaaResolveFbo;
			}
			else if (tr.msaaResolveFbo)
			{
				FBO_FastBlit(tr.sceneFbo, NULL, tr.msaaResolveFbo, NULL, GL_COLOR_BUFFER_BIT, GL_NEAREST);
				rsSrc = tr.msaaResolveFbo;
			}
			if (tr.renderScaleActive)
				RB_RenderScaleResample(rsSrc);
			else
				FBO_FastBlit(rsSrc, NULL, tr.renderFbo, NULL, GL_COLOR_BUFFER_BIT, GL_NEAREST);
		}
		return (const void *)(cmd + 1);
	}

	if (cmd)
	{
		backEnd.refdef = cmd->refdef;
		backEnd.viewParms = cmd->viewParms;
	}

	R_GfxProbe_GpuMark(GPUMARK_POST_B);

	// HZM render scale: the post chain runs on the SCENE buffer (S); the final resample brings it
	// to the DISPLAY buffer (dstFbo). At scale 1.0 sceneFbo == renderFbo, so srcFbo == dstFbo and
	// the pipeline is byte-identical (no resample runs).
	srcFbo = tr.sceneFbo ? tr.sceneFbo : tr.renderFbo;
	dstFbo = tr.renderFbo;

	backEnd.msaaLinearValid = qfalse;
	msaaShaderResolved = qfalse;

	if (R_MsaaResolvesActive())
	{
		// HZM gl2 MSAA (plan P2b, ME-F2): depth resolve -> SSAO generation (depth only) -> ONE MRT colour resolve that
		// applies the AO per sample before the tone-exact clamp and also writes the linear copy bloom, DoF and the
		// exposure measure read. The separate AO composite below is skipped. ao = 1 when AO did not run (bug-1211).
		int     viewRect[4];
		ivec4_t aoBox;
		qboolean aoOn;

		viewRect[0] = aoBox[0] = backEnd.viewParms.viewportX;
		viewRect[1] = aoBox[1] = backEnd.viewParms.viewportY;
		viewRect[2] = aoBox[2] = backEnd.viewParms.viewportWidth;
		viewRect[3] = aoBox[3] = backEnd.viewParms.viewportHeight;

		RB_MSAAResolveDepth();
		RB_HZMSsao(tr.msaaResolveFbo, aoBox);
		aoOn = (qboolean)((r_ssao->integer || (r_ppSSAO && r_ppSSAO->integer)) && backEnd.ssaoValid
		                  && tr.screenSsaoImage && tr.screenSsaoFbo);
		RB_MSAAResolveColor(aoOn, R_MsaaToneResolveMode(), viewRect);
		srcFbo = tr.msaaResolveFbo;
		msaaShaderResolved = qtrue;
	}
	else if (tr.msaaResolveFbo)
	{
		// Resolve the MSAA before anything else
		// Can't resolve just part of the MSAA FBO, so multiple views will suffer a performance hit here
		FBO_FastBlit(srcFbo, NULL, tr.msaaResolveFbo, NULL, GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT, GL_NEAREST);
		srcFbo = tr.msaaResolveFbo;
	}

	dstBox[0] = backEnd.viewParms.viewportX;
	dstBox[1] = backEnd.viewParms.viewportY;
	dstBox[2] = backEnd.viewParms.viewportWidth;
	dstBox[3] = backEnd.viewParms.viewportHeight;

	// HZM gl2 SSAO BLACK-SCREEN FIX (bug-1177 follow-up): GENERATE the AO here, at the head of the
	// post chain - after the MSAA resolve above (so tr.renderDepthImage is resolved depth) and
	// immediately before the composite below that consumes it. It used to be generated inside
	// RB_DrawSurfs' r_depthPrepass-gated block while the composite below was not gated the same
	// way; at r_depthPrepass 0 that multiplied every frame by an all-zero AO image = black screen.
	// (srcBox is first assigned below, after the composite has used it for its own quarter-res
	//  source rect - seed it here so nothing reads it uninitialised)
	srcBox[0] = backEnd.viewParms.viewportX;
	srcBox[1] = backEnd.viewParms.viewportY;
	srcBox[2] = backEnd.viewParms.viewportWidth;
	srcBox[3] = backEnd.viewParms.viewportHeight;

	if (!msaaShaderResolved)   // HZM gl2 MSAA (plan P2b): already generated, before the colour resolve
	{
		RB_HZMSsao(srcFbo, srcBox);
	}

	// HZM gl2 (bug-1177): the NULL tests are MANDATORY, not defensive dressing - the lines below
	// dereference tr.screenSsaoImage->width/height with no check of their own, so this is the exact
	// spot that would crash if the AO gate ever ran without the buffers having been allocated.
	// backEnd.ssaoValid is the liveness half of the same guard: it is qfalse unless the pass above
	// actually wrote tr.screenSsaoFbo this frame, which turns any future failure of the generation
	// side into "AO silently does nothing" instead of "the screen is black".
	if ((r_ssao->integer || (r_ppSSAO && r_ppSSAO->integer)) && backEnd.ssaoValid
	    && tr.screenSsaoImage && tr.screenSsaoFbo
	    && !msaaShaderResolved)   // HZM gl2 MSAA (plan P2b): the MRT resolve already applied the AO per sample
	{
		// HZM render scale: screenSsaoImage is SCENE/2 and the viewport is SCENE-sized, so use scene dims.
		srcBox[0] = backEnd.viewParms.viewportX      * tr.screenSsaoImage->width  / (float)tr.sceneWidth;
		srcBox[1] = backEnd.viewParms.viewportY      * tr.screenSsaoImage->height / (float)tr.sceneHeight;
		srcBox[2] = backEnd.viewParms.viewportWidth  * tr.screenSsaoImage->width  / (float)tr.sceneWidth;
		srcBox[3] = backEnd.viewParms.viewportHeight * tr.screenSsaoImage->height / (float)tr.sceneHeight;

		FBO_Blit(tr.screenSsaoFbo, srcBox, NULL, srcFbo, dstBox, NULL, NULL, GLS_SRCBLEND_DST_COLOR | GLS_DSTBLEND_ZERO);
	}

	srcBox[0] = backEnd.viewParms.viewportX;
	srcBox[1] = backEnd.viewParms.viewportY;
	srcBox[2] = backEnd.viewParms.viewportWidth;
	srcBox[3] = backEnd.viewParms.viewportHeight;

	// HZM gl2 fog parity (2026-07-28, workflow wf_053a935d-2dd): the fog must be applied
	// BEFORE the tone stage, because gl1 is NOT untonemapped as the old comment here
	// claimed. gl1 bakes fixed-function fog during rasterisation (renderergl1
	// tr_backend.c RB_SetupFog) and THEN runs the HZM post-FX grade inline
	// (RB_PostFxApply, renderergl1/tr_backend.c:1302, r_ppTonemap/r_ppExposure/
	// r_ppContrast/r_ppSaturation - all live in the shipped config). So on gl1 a fogged
	// pixel reaches the screen as GRADE(mix(scene, fogColor)), while gl2 was producing
	// mix(TONE(scene), fogColor) - fog applied in the wrong colour space, which is what
	// makes gl2's horizon fail to lift the way gl1's does (measured: t2l1 gl1 far-band
	// 104.8 > near 92.8, gl2 far 82.9 < near 87.5).
	// Applying fog first makes gl2 compute TONE(mix(scene, fogColor)) - structurally
	// identical to gl1 - so the fog colour lands correctly with no per-map tuning.
	// r_globalFogPreTone 0 restores the old post-tone order for A/B.
	{
		static cvar_t *r_globalFogPreTone = NULL;
		qboolean fogFirst;

		if (!r_globalFogPreTone) {
			r_globalFogPreTone = ri.Cvar_Get("r_globalFogPreTone", "0", CVAR_ARCHIVE);   // legacy A/B for the SCREEN-SPACE pass only; ignored when the forward path is active
		}
		fogFirst = (qboolean)(r_globalFogPreTone->integer != 0);

		if (fogFirst && !R_UseForwardGlobalFog() && srcFbo && rb_globalFog.active && tr.globalFogFbo)
		{
			RB_GlobalFog(srcFbo, srcBox, tr.globalFogFbo, srcBox);
			FBO_FastBlit(tr.globalFogFbo, srcBox, srcFbo, srcBox, GL_COLOR_BUFFER_BIT, GL_NEAREST);
			backEnd.msaaLinearValid = qfalse;   // HZM MSAA P2b: the fog went onto the tone input only - bloom reads it
		}

		// HZM gl2 POST-FX PORT (bug-1149): gl1 applies bloom BEFORE its tonemap/grade
		// (renderergl1 tr_postprocess_gl1.c RB_PostFxApply: SSAO -> DoF -> bloom -> god rays ->
		// grade -> FXAA -> sharpen), so it goes here - after the optional pre-tone fog, ahead of
		// the tone stage. Driven by r_ppBloom / r_ppBloomThreshold / r_ppBloomIntensity, the same
		// Advanced-Graphics levers gl1 reads, so one set of sliders controls both renderers.
		// HZM exposure-aware bloom (bug-1149): in r_ppBloomMode 1, measure auto-exposure BEFORE bloom
		// composites its energy - so the glow is not dimmed away and the bright pass reads a
		// current-frame calcLevels texel. Mode 0 leaves the measurement inside RB_ToneMap, unchanged.
		if (!r_ppBloomModeBE) {
			r_ppBloomModeBE = ri.Cvar_Get("r_ppBloomMode", "0", CVAR_ARCHIVE);
			r_ppBloomBE     = ri.Cvar_Get("r_ppBloom",     "1", CVAR_ARCHIVE);
		}
		if (srcFbo && r_ppBloomBE->integer && r_ppBloomModeBE->integer
		    && r_hdr->integer && (r_toneMap->integer || r_forceToneMap->integer)
		    && (r_autoExposure->integer || r_forceAutoExposure->integer))
		{
			RB_ToneMapMeasureLevels(srcFbo, srcBox);
			bloomPreMeasured = qtrue;
		}

		if (srcFbo)
		{
			// gl1 order is SSAO -> DoF -> bloom -> god rays -> grade, so DoF leads
			RB_HZMDof(srcFbo, srcBox);
			RB_HZMBloom(srcFbo, srcBox);
		}

		if (srcFbo)
		{
			if (r_hdr->integer && (r_toneMap->integer || r_forceToneMap->integer))
			{
				autoExposure = r_autoExposure->integer || r_forceAutoExposure->integer;

				// Use an intermediate FBO because it can't blit to the same FBO directly
				// and can't read from an MSAA dstFbo later.
				RB_ToneMap(srcFbo, srcBox, tr.screenScratchFbo, srcBox, autoExposure, bloomPreMeasured);
				FBO_FastBlit(tr.screenScratchFbo, srcBox, srcFbo, srcBox, GL_COLOR_BUFFER_BIT, GL_NEAREST);
			}
			else if (r_cameraExposure->value != 0.0f)
			{
				vec4_t color;

				color[0] =
				color[1] =
				color[2] = pow(2, r_cameraExposure->value); //exp2(r_cameraExposure->value);
				// HZM coop [2026-09-28] storm darkness: the no-tonemap path takes the same exposure gain
				color[0] *= R_HZM_StormExposure(1);
				color[1] *= R_HZM_StormExposure(1);
				color[2] *= R_HZM_StormExposure(1);
				color[3] = 1.0f;

				FBO_BlitFromTexture(tr.whiteImage, NULL, NULL, srcFbo, srcBox, NULL, color, GLS_SRCBLEND_DST_COLOR | GLS_DSTBLEND_ZERO);
			}
		}

		if (!fogFirst && !R_UseForwardGlobalFog() && srcFbo && rb_globalFog.active && tr.globalFogFbo)
		{
			RB_GlobalFog(srcFbo, srcBox, tr.globalFogFbo, srcBox);
			FBO_FastBlit(tr.globalFogFbo, srcBox, srcFbo, srcBox, GL_COLOR_BUFFER_BIT, GL_NEAREST);
		}

		// HZM gl2 POST-FX PORT (bug-1150): gl1 runs FXAA -> sharpen -> rain-on-lens AFTER its
		// grade (renderergl1 tr_postprocess_gl1.c RB_PostFxApply), so they go here, once the tone
		// stage has produced a display-referred image. Same r_pp* levers as gl1.
		if (srcFbo)
		{
			RB_HZMScreenFx(srcFbo, srcBox);
			// HZM gl2 NEW POST-FX (bug-1158): underwater/frost/chromatic-aberration/film-grain.
			// No gl1 equivalent to stay parity-ordered against, so these simply run last - on top
			// of everything, including the vignettes, matching how a real lens/sensor artifact or
			// being submerged would sit over the whole image.
			RB_HZMExtraFx(srcFbo, srcBox);
		}
	}

	if (r_drawSunRays->integer)
		RB_SunRays(srcFbo, srcBox, srcFbo, srcBox);

	if (1)
		RB_BokehBlur(srcFbo, srcBox, srcFbo, srcBox, backEnd.refdef.blurFactor);
	else
		RB_GaussianBlur(srcFbo, srcFbo, backEnd.refdef.blurFactor);

	// HZM render scale: bring the fully post-processed scene (srcFbo @S) to the display FBO (@D)
	// via FSR 1 EASU / SSAA tent / bilinear, then optional RCAS. At scale 1.0 renderScaleActive is
	// qfalse and srcFbo == dstFbo, so this is exactly the old no-op (byte-identical).
	if (tr.renderScaleActive && dstFbo == tr.renderFbo && srcFbo != dstFbo)
	{
		RB_RenderScaleResample(srcFbo);
	}
	else if (srcFbo != dstFbo)
	{
		FBO_FastBlit(srcFbo, srcBox, dstFbo, dstBox, GL_COLOR_BUFFER_BIT, GL_NEAREST);
	}

	backEnd.msaaLinearValid = qfalse;   // HZM gl2 MSAA (plan P2b): the linear copy belongs to this post chain only

#if 0
	if (0)
	{
		vec4_t quadVerts[4];
		vec2_t texCoords[4];
		ivec4_t iQtrBox;
		vec4_t box;
		vec4_t viewInfo;
		static float scale = 5.0f;

		scale -= 0.005f;
		if (scale < 0.01f)
			scale = 5.0f;

		FBO_FastBlit(dstFbo, NULL, tr.quarterFbo[0], NULL, GL_COLOR_BUFFER_BIT, GL_LINEAR);

		iQtrBox[0] = backEnd.viewParms.viewportX      * tr.quarterImage[0]->width / (float)glConfig.vidWidth;
		iQtrBox[1] = backEnd.viewParms.viewportY      * tr.quarterImage[0]->height / (float)glConfig.vidHeight;
		iQtrBox[2] = backEnd.viewParms.viewportWidth  * tr.quarterImage[0]->width / (float)glConfig.vidWidth;
		iQtrBox[3] = backEnd.viewParms.viewportHeight * tr.quarterImage[0]->height / (float)glConfig.vidHeight;

		qglViewport(iQtrBox[0], iQtrBox[1], iQtrBox[2], iQtrBox[3]);
		qglScissor(iQtrBox[0], iQtrBox[1], iQtrBox[2], iQtrBox[3]);

		VectorSet4(box, 0.0f, 0.0f, 1.0f, 1.0f);

		texCoords[0][0] = box[0]; texCoords[0][1] = box[3];
		texCoords[1][0] = box[2]; texCoords[1][1] = box[3];
		texCoords[2][0] = box[2]; texCoords[2][1] = box[1];
		texCoords[3][0] = box[0]; texCoords[3][1] = box[1];

		VectorSet4(box, -1.0f, -1.0f, 1.0f, 1.0f);

		VectorSet4(quadVerts[0], box[0], box[3], 0, 1);
		VectorSet4(quadVerts[1], box[2], box[3], 0, 1);
		VectorSet4(quadVerts[2], box[2], box[1], 0, 1);
		VectorSet4(quadVerts[3], box[0], box[1], 0, 1);

		GL_State(GLS_DEPTHTEST_DISABLE);


		VectorSet4(viewInfo, backEnd.viewParms.zFar / r_znear->value, backEnd.viewParms.zFar, 0.0, 0.0);

		viewInfo[2] = scale / (float)(tr.quarterImage[0]->width);
		viewInfo[3] = scale / (float)(tr.quarterImage[0]->height);

		FBO_Bind(tr.quarterFbo[1]);
		GLSL_BindProgram(&tr.depthBlurShader[2]);
		GL_BindToTMU(tr.quarterImage[0], TB_COLORMAP);
		GLSL_SetUniformVec4(&tr.depthBlurShader[2], UNIFORM_VIEWINFO, viewInfo);
		RB_InstantQuad2(quadVerts, texCoords);

		FBO_Bind(tr.quarterFbo[0]);
		GLSL_BindProgram(&tr.depthBlurShader[3]);
		GL_BindToTMU(tr.quarterImage[1], TB_COLORMAP);
		GLSL_SetUniformVec4(&tr.depthBlurShader[3], UNIFORM_VIEWINFO, viewInfo);
		RB_InstantQuad2(quadVerts, texCoords);

		SetViewportAndScissor();

		FBO_FastBlit(tr.quarterFbo[1], NULL, dstFbo, NULL, GL_COLOR_BUFFER_BIT, GL_LINEAR);
		FBO_Bind(NULL);
	}
#endif

	// HZM gl2 real character shadows: was dead-coded `if (0 && ...)`. r_shadowDebug 1 blits the
	// four cascade depth maps into the top-left corner - the single cheapest way to confirm what
	// is (or is not) being written into them. Character silhouettes appearing in cascades 0/1 is
	// the money shot for this whole feature.
	// HZM gl2 graphics probes: r_shadowDebug 4 = the resolved shadow mask FULL SCREEN, nothing over it
	// (tests T1-T3 diff these captures). 1-3 keep their corner thumbnails.
	// HZM gl2 P3: 5 = the same full-screen mask, carrying V_world (W) while stable is on (vet R8)
	if ((r_shadowDebug->integer == 4 || r_shadowDebug->integer == 5) && r_sunlightMode->integer && tr.screenShadowImage)
	{
		ivec4_t dstBox;
		VectorSet4(dstBox, 0, 0, glConfig.vidWidth, glConfig.vidHeight);
		FBO_BlitFromTexture(tr.screenShadowImage, NULL, NULL, dstFbo, dstBox, NULL, NULL, 0);
	}

	if (r_shadowDebug->integer >= 1 && r_shadowDebug->integer <= 3 && r_sunlightMode->integer)
	{
		ivec4_t dstBox;
		VectorSet4(dstBox, 0, glConfig.vidHeight - 128, 128, 128);
		FBO_BlitFromTexture(tr.sunShadowDepthImage[0], NULL, NULL, dstFbo, dstBox, NULL, NULL, 0);
		VectorSet4(dstBox, 128, glConfig.vidHeight - 128, 128, 128);
		FBO_BlitFromTexture(tr.sunShadowDepthImage[1], NULL, NULL, dstFbo, dstBox, NULL, NULL, 0);
		VectorSet4(dstBox, 256, glConfig.vidHeight - 128, 128, 128);
		FBO_BlitFromTexture(tr.sunShadowDepthImage[2], NULL, NULL, dstFbo, dstBox, NULL, NULL, 0);
		VectorSet4(dstBox, 384, glConfig.vidHeight - 128, 128, 128);
		FBO_BlitFromTexture(tr.sunShadowDepthImage[3], NULL, NULL, dstFbo, dstBox, NULL, NULL, 0);
	}

	// r_shadowDebug 2: the resolved screen-space shadowmask. If this is uniformly white the
	// chain is dead upstream (VPF_USESUNLIGHT / the depth-fill gate), not at the casters.
	if (r_shadowDebug->integer >= 2 && r_shadowDebug->integer <= 3 && r_sunlightMode->integer && tr.screenShadowImage)
	{
		ivec4_t dstBox;
		VectorSet4(dstBox, 0, glConfig.vidHeight - 256, 256, 128);
		FBO_BlitFromTexture(tr.screenShadowImage, NULL, NULL, dstFbo, dstBox, NULL, NULL, 0);
	}

	if (0 && r_shadows->integer == 4)
	{
		ivec4_t dstBox;
		VectorSet4(dstBox, 512 + 0, glConfig.vidHeight - 128, 128, 128);
		FBO_BlitFromTexture(tr.pshadowMaps[0], NULL, NULL, dstFbo, dstBox, NULL, NULL, 0);
		VectorSet4(dstBox, 512 + 128, glConfig.vidHeight - 128, 128, 128);
		FBO_BlitFromTexture(tr.pshadowMaps[1], NULL, NULL, dstFbo, dstBox, NULL, NULL, 0);
		VectorSet4(dstBox, 512 + 256, glConfig.vidHeight - 128, 128, 128);
		FBO_BlitFromTexture(tr.pshadowMaps[2], NULL, NULL, dstFbo, dstBox, NULL, NULL, 0);
		VectorSet4(dstBox, 512 + 384, glConfig.vidHeight - 128, 128, 128);
		FBO_BlitFromTexture(tr.pshadowMaps[3], NULL, NULL, dstFbo, dstBox, NULL, NULL, 0);
	}

	if (0)
	{
		ivec4_t dstBox;
		VectorSet4(dstBox, 256, glConfig.vidHeight - 256, 256, 256);
		FBO_BlitFromTexture(tr.renderDepthImage, NULL, NULL, dstFbo, dstBox, NULL, NULL, 0);
		VectorSet4(dstBox, 512, glConfig.vidHeight - 256, 256, 256);
		FBO_BlitFromTexture(tr.screenShadowImage, NULL, NULL, dstFbo, dstBox, NULL, NULL, 0);
	}

	if (0)
	{
		ivec4_t dstBox;
		VectorSet4(dstBox, 256, glConfig.vidHeight - 256, 256, 256);
		FBO_BlitFromTexture(tr.sunRaysImage, NULL, NULL, dstFbo, dstBox, NULL, NULL, 0);
	}

#if 0
	if (r_cubeMapping->integer && tr.numCubemaps)
	{
		ivec4_t dstBox;
		int cubemapIndex = R_CubemapForPoint( backEnd.viewParms.or.origin );

		if (cubemapIndex)
		{
			VectorSet4(dstBox, 0, glConfig.vidHeight - 256, 256, 256);
			//FBO_BlitFromTexture(tr.renderCubeImage, NULL, NULL, dstFbo, dstBox, &tr.testcubeShader, NULL, 0);
			FBO_BlitFromTexture(tr.cubemaps[cubemapIndex - 1].image, NULL, NULL, dstFbo, dstBox, &tr.testcubeShader, NULL, 0);
		}
	}
#endif

	R_GfxProbe_GpuMark(GPUMARK_POST_E);

	return (const void *)(cmd + 1);
}

// FIXME: put this function declaration elsewhere
void R_SaveDDS(const char *filename, byte *pic, int width, int height, int depth);

/*
=============
RB_ExportCubemaps

=============
*/
const void *RB_ExportCubemaps(const void *data)
{
	const exportCubemapsCommand_t *cmd = data;

	// finish any 2D drawing if needed
	if (tess.numIndexes)
		RB_EndSurface();

	if (!glRefConfig.framebufferObject || !tr.world || tr.numCubemaps == 0)
	{
		// do nothing
		ri.Printf(PRINT_ALL, "Nothing to export!\n");
		return (const void *)(cmd + 1);
	}

	if (cmd)
	{
		FBO_t *oldFbo = glState.currentFBO;
		int sideSize = r_cubemapSize->integer * r_cubemapSize->integer * 4;
		byte *cubemapPixels = ri.Malloc(sideSize * 6);
		int i, j;

		FBO_Bind(tr.renderCubeFbo);

		for (i = 0; i < tr.numCubemaps; i++)
		{
			char filename[MAX_QPATH];
			cubemap_t *cubemap = &tr.cubemaps[i];
			byte *p = cubemapPixels;

			for (j = 0; j < 6; j++)
			{
				FBO_AttachImage(tr.renderCubeFbo, cubemap->image, GL_COLOR_ATTACHMENT0_EXT, j);
				qglReadPixels(0, 0, r_cubemapSize->integer, r_cubemapSize->integer, GL_RGBA, GL_UNSIGNED_BYTE, p);
				p += sideSize;
			}

			if (cubemap->name[0])
			{
				COM_StripExtension(cubemap->name, filename, MAX_QPATH);
				Q_strcat(filename, MAX_QPATH, ".dds");
			}
			else
			{
				Com_sprintf(filename, MAX_QPATH, "cubemaps/%s/%03d.dds", tr.world->baseName, i);
			}

			R_SaveDDS(filename, cubemapPixels, r_cubemapSize->integer, r_cubemapSize->integer, 6);
			ri.Printf(PRINT_ALL, "Saved cubemap %d as %s\n", i, filename);
		}

		FBO_Bind(oldFbo);

		ri.Free(cubemapPixels);
	}

	return (const void *)(cmd + 1);
}


/*
====================
RB_ExecuteRenderCommands
====================
*/
void RB_ExecuteRenderCommands( const void *data ) {
	int		t1, t2;

	t1 = ri.Milliseconds ();

	while ( 1 ) {
		data = PADP(data, sizeof(void *));

		switch ( *(const int *)data ) {
		case RC_SET_COLOR:
			data = RB_SetColor( data );
			break;
		case RC_STRETCH_PIC:
			data = RB_StretchPic( data );
			break;
		case RC_DRAW_SURFS:
			data = RB_DrawSurfs( data );
			break;
		case RC_DRAW_BUFFER:
			data = RB_DrawBuffer( data );
			break;
		case RC_SWAP_BUFFERS:
			data = RB_SwapBuffers( data );
			break;
		case RC_SCREENSHOT:
			data = RB_TakeScreenshotCmd( data );
			break;
		case RC_VIDEOFRAME:
			data = RB_TakeVideoFrameCmd( data );
			break;
		case RC_COLORMASK:
			data = RB_ColorMask(data);
			break;
		case RC_CLEARDEPTH:
			data = RB_ClearDepth(data);
			break;
		case RC_CAPSHADOWMAP:
			data = RB_CapShadowMap(data);
			break;
		case RC_POSTPROCESS:
			data = RB_PostProcess(data);
			break;
		case RC_EXPORT_CUBEMAPS:
			data = RB_ExportCubemaps(data);
			break;
		//
		// OPENMOHAA-specific stuff
		//=========================
		case RC_SPRITE_SURFS:
			data = RB_SpriteSurfs( data );
			break;
		//=========================
		case RC_END_OF_LIST:
		default:
			// finish any 2D drawing if needed
			if(tess.numIndexes)
				RB_EndSurface();

			// stop rendering
			t2 = ri.Milliseconds ();
			backEnd.pc.msec = t2 - t1;
			return;
		}
	}

}

//
// OPENMOHAA-specific stuff
//




/*
=============
RE_StretchRaw2

=============
*/
void RE_StretchRaw2 (int x, int y, int w, int h, int cols, int rows, int components, const byte* data) {
	RE_StretchRaw(x, y, w, h, cols, rows, data, 0, qtrue);
}

/*
==================
RB_HZM_SceneDepthSnapshot

HZM gl2 SOFT PARTICLES + [2026-09-26] S3b: THE scene-depth snapshot - blit renderDepthImage (resolving the MS depth
first when multisampling) into hdrDepthImage and restore the scene target, viewport and scissor. Moved here verbatim
from RB_SpriteSurfs so the sprite list and the soft-edge hoist (RB_HZM_SoftEdgeSnapshotAt) share ONE copy: the gfx P2
MSAA core replaces the body of THIS function with its post-opaque (min, max) depth resolve (plan_graphics_final.md P2b,
MH-B1 two-surface fade), and both callers follow. Sets backEnd.softDepthValid; false = not taken (UI 3D view, or no
hdrDepth target because r_softParticles/SSAO/DoF are all off at launch).
==================
*/
qboolean RB_HZM_SceneDepthSnapshot( void ) {
	FBO_t	*oldFbo;
	vec4_t	srcTexCoords;

	if ( ( backEnd.refdef.rdflags & RDF_NOWORLDMODEL ) || !tr.hdrDepthFbo ) {
		return qfalse;
	}
	oldFbo = glState.currentFBO;

	if (R_MsaaResolvesActive() && backEnd.viewParms.targetFbo == NULL) {
		RB_MSAAResolveDepth();   // HZM gl2 MSAA (plan P2b, MH-B1): (min, max) for the two-surface soft-particle fade
	} else if (tr.msaaResolveFbo) {
		FBO_FastBlit(tr.sceneFbo, NULL, tr.msaaResolveFbo, NULL, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
	}

	VectorSet4(srcTexCoords, 0.0f, 0.0f, 1.0f, 1.0f);
	FBO_BlitFromTexture(tr.renderDepthImage, srcTexCoords, NULL, tr.hdrDepthFbo, NULL, NULL, NULL, 0);

	// restore the scene render target and this view's viewport/scissor for the draws that follow
	FBO_Bind(oldFbo ? oldFbo : (tr.sceneFbo ? tr.sceneFbo : tr.renderFbo));
	SetViewportAndScissor();

	backEnd.softDepthValid = qtrue;
	return qtrue;
}

/*
==================
RB_HZM_BindSceneDepth - the snapshot on TB_SCREENDEPTH (P2 binds its own depth image here)
==================
*/
void RB_HZM_BindSceneDepth( void ) {
	if (backEnd.softDepthValid && tr.hdrDepthImage) {
		// HZM gl2 MSAA (plan P2b): the (min, max) image when a shader depth resolve made this snapshot; its r is the
		// same nearest depth hdrDepthImage holds, and MSAA_DEPTH_MINMAX shaders also read its g
		GL_BindToTMU(R_MsaaDepthMinMaxOr(tr.hdrDepthImage), TB_SCREENDEPTH);
	}
}

/*
==================
RB_HZM_SoftEdgeSnapshotAt

HZM gl2 [2026-09-26] S3b: called from RB_RenderDrawSurfList, between batches, for the first qer_hzmSoftEdge surface of
a view. Only for the scene views a soft particle may fade in (no shadow / pshadow / cubemap target, no 2D, no UI 3D
view) and never for an RF_DEPTHHACK entity (its compressed depth range would compare wrong). The snapshot resets the
projection to the view's (SetViewportAndScissor); the batch loop may be mid weapon-projection, so it is put back.
==================
*/
void RB_HZM_SoftEdgeSnapshotAt( int entityNum, qboolean bStaticModel ) {
	mat4_t	projection;

	if ( !R_HZM_SoftEdgeOn() || backEnd.projection2D || backEnd.viewParms.targetFbo
		|| ( backEnd.viewParms.flags & ( VPF_DEPTHSHADOW | VPF_SHADOWMAP | VPF_PSHADOW ) ) ) {
		return;
	}
	if ( !bStaticModel && entityNum >= 0 && entityNum < backEnd.refdef.num_entities
		&& ( backEnd.refdef.entities[entityNum].e.renderfx & RF_DEPTHHACK ) ) {
		return;
	}
	Mat4Copy( glState.projection, projection );
	RB_HZM_SceneDepthSnapshot();
	GL_SetProjectionMatrix( projection );
}

/*
==================
RB_RenderSpriteSurfList
==================
*/
void RB_RenderSpriteSurfList(drawSurf_t* drawSurfs, int numDrawSurfs) {
	shader_t	*shader;
	shader_t	*oldShader;
	qboolean	depthRange;
	qboolean	oldDepthRange;
	int			i;
	drawSurf_t	*drawSurf;

    backEnd.currentEntity = &tr.worldEntity;
    backEnd.currentStaticModel = NULL;

	backEnd.pc.c_surfaces += numDrawSurfs;
	backEnd.or = backEnd.viewParms.world;

	// HZM gl2 SOFT PARTICLES (r_softParticles): open the per-fragment fade window for the sprite
	// list and bind the scene-depth snapshot on TB_SCREENDEPTH. spriteDepthHack is set per sprite
	// below so RF_DEPTHHACK muzzle flashes are excluded. inSpriteList is cleared at the end.
	backEnd.inSpriteList = qtrue;
	backEnd.spriteDepthHack = qfalse;
	RB_HZM_BindSceneDepth();

	oldShader = NULL;
    depthRange = qfalse;
    oldDepthRange = qfalse;

    for (i = 0, drawSurf = drawSurfs; i < numDrawSurfs; i++, drawSurf++) {
		shader = tr.sortedShaders[((refSprite_t*)drawSurf->surface)->shaderNum];
		depthRange = (((refSprite_t*)drawSurf->surface)->renderfx & RF_DEPTHHACK) != 0;

        if (shader != oldShader)
        {
			if (oldShader) {
				RB_EndSurface();
			}

            RB_BeginSurface(shader, 0, 0);
            oldShader = shader;
        }

		// HZM gl2 SOFT PARTICLES: record this sprite's depth-hack state for RB_SetSoftParticleUniforms.
		backEnd.spriteDepthHack = depthRange;
		
		GL_SetModelviewMatrix( backEnd.or.modelMatrix );

        if (oldDepthRange != depthRange)
        {
			if (depthRange) {
				// HZM gl2 re-port Fix 3: depth-hacked sprites (first-person muzzle flash)
				// follow the weapon projection so the flash stays registered to the shifted gun
				if (backEnd.viewParms.stereoFrame == STEREO_CENTER && backEnd.viewParms.weaponFovActive) {
					GL_SetProjectionMatrix( backEnd.viewParms.weaponProjectionMatrix );
				}
				qglDepthRange(0.0, 0.3);
			} else {
				if (backEnd.viewParms.stereoFrame == STEREO_CENTER && backEnd.viewParms.weaponFovActive) {
					GL_SetProjectionMatrix( backEnd.viewParms.projectionMatrix );
				}
                qglDepthRange(0.0, 1.0);
			}

            oldDepthRange = depthRange;
        }

        backEnd.shaderStartTime = ((refSprite_t*)drawSurf->surface)->shaderTime;

        // add the triangles for this surface
        rb_surfaceTable[*drawSurf->surface](drawSurf->surface);
	}

	if (oldShader) {
		RB_EndSurface();
	}

    // go back to the world modelview matrix
	GL_SetModelviewMatrix( backEnd.viewParms.world.modelMatrix );
	// go back to the previous depth range
	if (depthRange) {
		// HZM gl2 re-port Fix 3: restore the world projection if the last sprite was depth-hacked
		if (backEnd.viewParms.stereoFrame == STEREO_CENTER && backEnd.viewParms.weaponFovActive) {
			GL_SetProjectionMatrix( backEnd.viewParms.projectionMatrix );
		}
		qglDepthRange(0.0, 1.0);
	}

	// HZM gl2 SOFT PARTICLES: close the fade window; world draws in later frames upload mode 0.
	backEnd.inSpriteList = qfalse;
}

/*
=============
RB_SpriteSurfs

=============
*/
const void* RB_SpriteSurfs(const void* data) {
    const drawSurfsCommand_t* cmd;

    // finish any 2D drawing if needed
    if (tess.numIndexes) {
        RB_EndSurface();
    }

    cmd = (const drawSurfsCommand_t*)data;

    backEnd.refdef = cmd->refdef;
    backEnd.viewParms = cmd->viewParms;

    // HZM gl2 SOFT PARTICLES (r_softParticles, design section 5): snapshot the scene depth into
    // hdrDepthImage so the sprite fragment shader can fade emitter particles as they approach
    // geometry. Per view (the portal-sky view gets its own), and gated so it costs nothing with
    // the feature off, on an RDF_NOWORLDMODEL (UI 3D) view, or before any world surface exists.
    // Mirrors the SSAO/DoF snapshot recipe in RB_DrawSurfs (blit renderDepthImage -> hdrDepthFbo,
    // resolving MS depth first when multisampling).
    // (S3b: the snapshot itself lives in RB_HZM_SceneDepthSnapshot, shared with the soft-edge hoist)
    backEnd.softDepthValid = qfalse;
    if (r_softParticles && r_softParticles->integer && cmd->numDrawSurfs > 0) {
        RB_HZM_SceneDepthSnapshot();
    }

	//RB_SetupFog();
    RB_RenderSpriteSurfList(cmd->drawSurfs, cmd->numDrawSurfs);

    return (const void*)(cmd + 1);
}
