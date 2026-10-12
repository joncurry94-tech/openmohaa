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
// tr_shade.c

#include "tr_local.h" 
#include "../renderercommon/hzm_storm.h" // HZM coop [2026-09-28] storm darkness

/*

  THIS ENTIRE FILE IS BACK END

  This file deals with applying shaders to surface data in the tess struct.
*/


/*
==================
R_DrawElements

==================
*/

void R_DrawElements( int numIndexes, int firstIndex )
{
	hzmPerf.glDraws++;	// HZM gl2 [render-perf 0] r_hzmPerfStats
	if (tess.useCacheVao)
	{
		VaoCache_DrawElements(numIndexes, firstIndex);
	}
	else if (R_TessStreamDraw(numIndexes, firstIndex))
	{
		// HZM gl2 [bug-3483] drawn from the tess ring at its base vertex / first index (tr_vbo.c)
	}
	else
	{
		qglDrawElements(GL_TRIANGLES, numIndexes, GL_INDEX_TYPE, BUFFER_OFFSET(firstIndex * sizeof(glIndex_t)));
	}
}


/*
=============================================================

SURFACE SHADERS

=============================================================
*/

shaderCommands_t	tess;


/*
=================
R_BindAnimatedImageToTMU

=================
*/
static void R_BindAnimatedImageToTMU( textureBundle_t *bundle, int tmu ) {
	int64_t index;

	if ( bundle->isVideoMap ) {
		ri.CIN_RunCinematic(bundle->videoMapHandle);
		ri.CIN_UploadCinematic(bundle->videoMapHandle);
		GL_BindToTMU(tr.scratchImage[bundle->videoMapHandle], tmu);
		return;
	}

	if ( bundle->numImageAnimations <= 1 ) {
		// HZM coop - gore tier 4 (UV wounds): entities with a wound-painted
		// copy of this texture get their copy bound instead of the shared base
		// (HZM gl2 re-port bug-gl2-gore, mirrors gl1 tr_shade.c:223)
		GL_BindToTMU( R_GoreOverrideImage(bundle->image[0]), tmu);
		return;
	}

	// it is necessary to do this messy calc to make sure animations line up
	// exactly with waveforms of the same frequency
	index = tess.shaderTime * bundle->imageAnimationSpeed * FUNCTABLE_SIZE;
	index >>= FUNCTABLE_SIZE2;

	if ( index < 0 ) {
		index = 0;	// may happen with shader time offsets
	}

	// Windows x86 doesn't load renderer DLL with 64 bit modulus
	//index %= bundle->numImageAnimations;
	while ( index >= bundle->numImageAnimations ) {
		index -= bundle->numImageAnimations;
	}

	// HZM coop - gore tier 4 (UV wounds): same override for animated bundles
	// (HZM gl2 re-port bug-gl2-gore, mirrors gl1 tr_shade.c:242)
	GL_BindToTMU( R_GoreOverrideImage(bundle->image[ index ]), tmu );
}


/*
================
DrawTris

Draws triangle outlines for debugging
================
*/
static void DrawTris (shaderCommands_t *input) {
	GL_BindToTMU( tr.whiteImage, TB_COLORMAP );

	GL_State( GLS_POLYMODE_LINE | GLS_DEPTHMASK_TRUE );
	qglDepthRange( 0, 0 );

	{
		shaderProgram_t *sp = &tr.textureColorShader;
		vec4_t color;

		GLSL_BindProgram(sp);
		
		GLSL_SetUniformMat4(sp, UNIFORM_MODELVIEWPROJECTIONMATRIX, glState.modelviewProjection);
		VectorSet4(color, 1, 1, 1, 1);
		GLSL_SetUniformVec4(sp, UNIFORM_COLOR, color);
		GLSL_SetUniformInt(sp, UNIFORM_ALPHATEST, 0);

		R_DrawElements(input->numIndexes, input->firstIndex);
	}

	qglDepthRange( 0, 1 );
}


/*
================
DrawNormals

Draws vertex normals for debugging
================
*/
static void DrawNormals (shaderCommands_t *input) {
	//FIXME: implement this
}

/*
==============
RB_BeginSurface

We must set some things up before beginning any tesselation,
because a surface may be forced to perform a RB_End due
to overflow.
==============
*/
void RB_BeginSurface( shader_t *shader, int fogNum, int cubemapIndex ) {

	shader_t *state = (shader->remappedShader) ? shader->remappedShader : shader;

	tess.numIndexes = 0;
	tess.firstIndex = 0;
	tess.numVertexes = 0;
	tess.shader = state;
	tess.fogNum = fogNum;
	tess.cubemapIndex = cubemapIndex;
	tess.dlightBits = 0;		// will be OR'd in by surface functions
	tess.pshadowBits = 0;       // will be OR'd in by surface functions
	tess.xstages = state->stages;
	tess.numPasses = state->numUnfoggedPasses;
	tess.currentStageIteratorFunc = state->optimalStageIteratorFunc;
	tess.useInternalVao = qtrue;
	tess.useCacheVao = qfalse;

	tess.shaderTime = backEnd.refdef.floatTime - tess.shader->timeOffset;
	if (tess.shader->clampTime && tess.shaderTime >= tess.shader->clampTime) {
		tess.shaderTime = tess.shader->clampTime;
	}

	if (backEnd.viewParms.flags & VPF_SHADOWMAP)
	{
		tess.currentStageIteratorFunc = RB_StageIteratorGeneric;
	}
}



extern float EvalWaveForm( const waveForm_t *wf );
extern float EvalWaveFormClamped( const waveForm_t *wf );


static void ComputeTexMods( shaderStage_t *pStage, int bundleNum, vec4_t outMatrix[8])
{
	int tm;
	float matrix[6];
	float tmpmatrix[6];
	float currentmatrix[6];
	float turb[2];
	textureBundle_t *bundle = &pStage->bundle[bundleNum];
	qboolean hasTurb = qfalse;

	currentmatrix[0] = 1.0f; currentmatrix[2] = 0.0f; currentmatrix[4] = 0.0f;
	currentmatrix[1] = 0.0f; currentmatrix[3] = 1.0f; currentmatrix[5] = 0.0f;

	for ( tm = 0; tm < bundle->numTexMods ; tm++ ) {
		switch ( bundle->texMods[tm].type )
		{
			
		case TMOD_NONE:
			matrix[0] = 1.0f; matrix[2] = 0.0f; matrix[4] = 0.0f;
			matrix[1] = 0.0f; matrix[3] = 1.0f; matrix[5] = 0.0f;
			break;

		case TMOD_TURBULENT:
			RB_CalcTurbulentFactors(&bundle->texMods[tm].wave, &turb[0], &turb[1]);
			break;

		case TMOD_ENTITY_TRANSLATE:
			RB_CalcScrollTexMatrix( backEnd.currentEntity->e.shaderTexCoord, matrix );
			break;

		case TMOD_SCROLL:
			RB_CalcScrollTexMatrix( bundle->texMods[tm].scroll,
									 matrix );
			break;

		case TMOD_SCALE:
			RB_CalcScaleTexMatrix( bundle->texMods[tm].scale,
								  matrix );
			break;
		
		case TMOD_STRETCH:
			RB_CalcStretchTexMatrix( &bundle->texMods[tm].wave, 
								   matrix );
			break;

		// HZM gl2 parity (bug-1242). Only this switch needs the cases: the second switch below
		// already routes every matrix-based tcMod through its default branch.
		case TMOD_WAVETRANS:
			RB_CalcTransWaveTexMatrix( &bundle->texMods[tm].wave, matrix );
			break;

		case TMOD_WAVETRANT:
			RB_CalcTransWaveTexMatrixT( &bundle->texMods[tm].wave, matrix );
			break;

		case TMOD_TRANSFORM:
			RB_CalcTransformTexMatrix( &bundle->texMods[tm],
									 matrix );
			break;

		case TMOD_ROTATE:
			RB_CalcRotateTexMatrix( bundle->texMods[tm].rotateSpeed,
									matrix );
			break;

		default:
			ri.Error( ERR_DROP, "ERROR: unknown texmod '%d' in shader '%s'", bundle->texMods[tm].type, tess.shader->name );
			break;
		}

		switch ( bundle->texMods[tm].type )
		{	
		case TMOD_TURBULENT:
			outMatrix[tm*2+0][0] = 1; outMatrix[tm*2+0][1] = 0; outMatrix[tm*2+0][2] = 0;
			outMatrix[tm*2+1][0] = 0; outMatrix[tm*2+1][1] = 1; outMatrix[tm*2+1][2] = 0;

			outMatrix[tm*2+0][3] = turb[0];
			outMatrix[tm*2+1][3] = turb[1];

			hasTurb = qtrue;
			break;

		case TMOD_NONE:
		case TMOD_ENTITY_TRANSLATE:
		case TMOD_SCROLL:
		case TMOD_SCALE:
		case TMOD_STRETCH:
		case TMOD_TRANSFORM:
		case TMOD_ROTATE:
		default:
			outMatrix[tm*2+0][0] = matrix[0]; outMatrix[tm*2+0][1] = matrix[2]; outMatrix[tm*2+0][2] = matrix[4];
			outMatrix[tm*2+1][0] = matrix[1]; outMatrix[tm*2+1][1] = matrix[3]; outMatrix[tm*2+1][2] = matrix[5];

			outMatrix[tm*2+0][3] = 0;
			outMatrix[tm*2+1][3] = 0;

			tmpmatrix[0] = matrix[0] * currentmatrix[0] + matrix[2] * currentmatrix[1];
			tmpmatrix[1] = matrix[1] * currentmatrix[0] + matrix[3] * currentmatrix[1];

			tmpmatrix[2] = matrix[0] * currentmatrix[2] + matrix[2] * currentmatrix[3];
			tmpmatrix[3] = matrix[1] * currentmatrix[2] + matrix[3] * currentmatrix[3];

			tmpmatrix[4] = matrix[0] * currentmatrix[4] + matrix[2] * currentmatrix[5] + matrix[4];
			tmpmatrix[5] = matrix[1] * currentmatrix[4] + matrix[3] * currentmatrix[5] + matrix[5];

			currentmatrix[0] = tmpmatrix[0];
			currentmatrix[1] = tmpmatrix[1];
			currentmatrix[2] = tmpmatrix[2];
			currentmatrix[3] = tmpmatrix[3];
			currentmatrix[4] = tmpmatrix[4];
			currentmatrix[5] = tmpmatrix[5];
			break;
		}
	}

	// if turb isn't used, only one matrix is needed
	if ( !hasTurb ) {
		tm = 0;

		outMatrix[tm*2+0][0] = currentmatrix[0]; outMatrix[tm*2+0][1] = currentmatrix[2]; outMatrix[tm*2+0][2] = currentmatrix[4];
		outMatrix[tm*2+1][0] = currentmatrix[1]; outMatrix[tm*2+1][1] = currentmatrix[3]; outMatrix[tm*2+1][2] = currentmatrix[5];

		outMatrix[tm*2+0][3] = 0;
		outMatrix[tm*2+1][3] = 0;
		tm++;
	}

	for ( ; tm < TR_MAX_TEXMODS ; tm++ ) {
		outMatrix[tm*2+0][0] = 1; outMatrix[tm*2+0][1] = 0; outMatrix[tm*2+0][2] = 0;
		outMatrix[tm*2+1][0] = 0; outMatrix[tm*2+1][1] = 1; outMatrix[tm*2+1][2] = 0;

		outMatrix[tm*2+0][3] = 0;
		outMatrix[tm*2+1][3] = 0;
	}
}


static void ComputeDeformValues(int *deformGen, vec5_t deformParams)
{
	// u_DeformGen
	*deformGen = DGEN_NONE;
	// HZM gl2 P3 (bug-3244): W bakes the REST pose (what the lightmap compiler saw), never a time-dependent deform
	if (backEnd.viewParms.flags & VPF_SUNWORLD)
	{
		return;
	}
	if(!ShaderRequiresCPUDeforms(tess.shader))
	{
		deformStage_t  *ds;

		// only support the first one
		ds = &tess.shader->deforms[0];

		switch (ds->deformation)
		{
			case DEFORM_WAVE:
				*deformGen = ds->deformationWave.func;

				deformParams[0] = ds->deformationWave.base;
				deformParams[1] = ds->deformationWave.amplitude;
				deformParams[2] = ds->deformationWave.phase;
				deformParams[3] = ds->deformationWave.frequency;
				deformParams[4] = ds->deformationSpread;
				break;

			case DEFORM_BULGE:
				*deformGen = DGEN_BULGE;

				deformParams[0] = 0;
				deformParams[1] = ds->bulgeHeight; // amplitude
				deformParams[2] = ds->bulgeWidth;  // phase
				deformParams[3] = ds->bulgeSpeed;  // frequency
				deformParams[4] = 0;
				break;

			default:
				break;
		}
	}
}


static void ProjectDlightTexture( void ) {
	int		l;
	vec3_t	origin;
	float	scale;
	float	radius;
	int deformGen;
	vec5_t deformParams;

	if ( !backEnd.refdef.num_dlights ) {
		return;
	}

	ComputeDeformValues(&deformGen, deformParams);

	for ( l = 0 ; l < backEnd.refdef.num_dlights ; l++ ) {
		dlight_t	*dl;
		shaderProgram_t *sp;
		vec4_t vector;

		if ( !( tess.dlightBits & ( 1 << l ) ) ) {
			continue;	// this surface definitely doesn't have any of this light
		}

		dl = &backEnd.refdef.dlights[l];
		VectorCopy( dl->transformed, origin );
		radius = dl->radius;
		scale = 1.0f / radius;

		sp = &tr.dlightShader[deformGen == DGEN_NONE ? 0 : 1];

		backEnd.pc.c_dlightDraws++;

		GLSL_BindProgram(sp);

		GLSL_SetUniformMat4(sp, UNIFORM_MODELVIEWPROJECTIONMATRIX, glState.modelviewProjection);

		GLSL_SetUniformFloat(sp, UNIFORM_VERTEXLERP, glState.vertexAttribsInterpolation);
		
		GLSL_SetUniformInt(sp, UNIFORM_DEFORMGEN, deformGen);
		if (deformGen != DGEN_NONE)
		{
			GLSL_SetUniformFloat5(sp, UNIFORM_DEFORMPARAMS, deformParams);
			GLSL_SetUniformFloat(sp, UNIFORM_TIME, tess.shaderTime);
		}

		vector[0] = dl->color[0];
		vector[1] = dl->color[1];
		vector[2] = dl->color[2];
		vector[3] = 1.0f;
		GLSL_SetUniformVec4(sp, UNIFORM_COLOR, vector);

		vector[0] = origin[0];
		vector[1] = origin[1];
		vector[2] = origin[2];
		vector[3] = scale;
		GLSL_SetUniformVec4(sp, UNIFORM_DLIGHTINFO, vector);

		// HZM gl2 [2026-09-26] Phase S1: the cone, in the frame of u_DlightInfo (the LOCAL axis R_TransformDlights
		// keeps beside `transformed`), evaluated PER PIXEL in dlight_fp (vet F11: a per-vertex cone on a 4-vertex quay
		// face is blotchy). All zero for every omni light, which dlight_fp treats as "no cone".
		RB_HZM_SpotProjectUniform(sp, dl);
	  
		GL_BindToTMU( tr.dlightImage, TB_COLORMAP );

		// include GLS_DEPTHFUNC_EQUAL so alpha tested surfaces don't add light
		// where they aren't rendered
		if ( dl->additive ) {
			GL_State( GLS_ATEST_GT_0 | GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE | GLS_DEPTHFUNC_EQUAL );
		}
		else {
			GL_State( GLS_ATEST_GT_0 | GLS_SRCBLEND_DST_COLOR | GLS_DSTBLEND_ONE | GLS_DEPTHFUNC_EQUAL );
		}

		GLSL_SetUniformInt(sp, UNIFORM_ALPHATEST, 1);

		R_DrawElements(tess.numIndexes, tess.firstIndex);

		backEnd.pc.c_totalIndexes += tess.numIndexes;
		backEnd.pc.c_dlightIndexes += tess.numIndexes;
		backEnd.pc.c_dlightVertexes += tess.numVertexes;
	}
}


/*
===============
RB_DistFadeIsPerVertex

HZM gl2 parity (bug-1300): the ONE decider for whether this batch's
tess.color[i][3] belongs to the distance fade. RB_FillDistFadeAlpha uses it as its
gate and the AGEN_DIST_FADE case in ComputeShaderColors uses it to choose between a
vertex-alpha pass-through and plain opaque. They must never disagree - if the
uniform half said "pass through" while the fill half declined, the batch would
sample whatever alpha the PREVIOUS batch happened to leave in tess.color.

Declines, and why each one matters:
 - !tess.useInternalVao: a cached-VAO batch, whose colours come from a baked GPU
   buffer RB_UpdateTessVao never touches, so a CPU write would be discarded.
 - backEnd.depthFill: ComputeShaderColors also runs on the depth-fill path, which
   is upstream of the fill call site. Declining keeps both halves agreeing.
 - a shadow / depth-map view: viewParms.or.origin is the light's virtual position,
   not the player camera, so the fade would be measured from the wrong point.
 - the WORLD entity and the 2D entity. Both are zero-initialised (Com_Memset(&tr,..)
   and backEnd likewise), so their axis is the ZERO matrix, the eye term collapses to
   (0,0,0), and "distance from the eye" silently becomes distance from the MAP ORIGIN
   - camera-independent, and wrong everywhere but the origin. r_vaoCache defaults to 0
   (tr_init.c), so world surfaces keep tess.useInternalVao == qtrue and are NOT screened
   out by the first test; this has to reject them explicitly. Shipped content really does
   put alphaGen distFade on brush shaders (general_industrial.shader jh_pipe1_pulse et al,
   referenced by co_lobby8.bsp), so this path is reachable, not theoretical.

   DELIBERATE DIVERGENCE: gl1 fades those brush surfaces by map-origin distance. That is
   a bug in gl1, not a feature, and reproducing it would put a distance-keyed pulse on
   world geometry that moves when the map is re-origined. gl2 leaves them opaque, which
   is also the pre-change behaviour, so this fix cannot regress them.
===============
*/
static qboolean RB_DistFadeIsPerVertex( void )
{
	if ( !tess.useInternalVao || backEnd.depthFill ) {
		return qfalse;
	}

	if ( backEnd.viewParms.flags & (VPF_DEPTHSHADOW | VPF_SHADOWMAP | VPF_PSHADOW) ) {
		return qfalse;
	}

	if ( backEnd.currentStaticModel ) {
		return qtrue;
	}

	return (qboolean)( backEnd.currentEntity
		&& backEnd.currentEntity != &tr.worldEntity
		&& backEnd.currentEntity != &backEnd.entity2D );
}

/*
===============
RB_DistFadeConstAlpha

HZM gl2 parity (bug-1300): alphaGen tikiDistFade / oneMinusTikiDistFade. Unlike the
distFade pair these are CONSTANT per draw call - gl1 ends them in
RB_CalcAlphaFromConstant (gl1 tr_shade.c:1277-1345) - and are measured from the MODEL
ORIGIN, not per vertex. That makes them expressible as a plain uniform, so they need no
CPU vertex pass at all.

gl1's ramp, verbatim: 0 inside fDistNear, 255 beyond fDistNear+fDistRange, linear
between; then tikiDistFade (but NOT oneMinusTikiDistFade) inverts it. So
oneMinusTikiDistFade fades IN with distance - it is the LOD impostor billboard - and
tikiDistFade fades OUT - it is the mesh the impostor replaces.

Returns qfalse when there is no model to measure from, in which case the caller leaves
the stage opaque. gl1 calls ri.Error(ERR_DROP) there; dropping the client out of a live
game over a shader keyword is not a trade worth making in a renderer, and opaque is
exactly the pre-change behaviour.
===============
*/
static qboolean RB_DistFadeConstAlpha( const shaderStage_t *pStage, float *alphaOut )
{
	const float *modelOrigin;
	vec3_t       org;
	float        lenSqr, fNear, fFar, fRange, a;

	if ( backEnd.currentStaticModel ) {
		modelOrigin = backEnd.currentStaticModel->origin;
	} else if ( backEnd.currentEntity
		&& backEnd.currentEntity != &tr.worldEntity
		&& backEnd.currentEntity != &backEnd.entity2D ) {
		modelOrigin = backEnd.currentEntity->e.origin;
	} else {
		return qfalse;
	}

	VectorSubtract( modelOrigin, backEnd.viewParms.or.origin, org );

	lenSqr = VectorLengthSquared( org );
	// HZM gl2 P3: the W bake measures from no camera - it takes the near state (see tr_staticmodels.cpp)
	if ( backEnd.viewParms.flags & VPF_SUNWORLD ) {
		lenSqr = 0.0f;
	}
	fNear  = tess.shader->fDistNear;
	fRange = tess.shader->fDistRange;
	fFar   = tess.shader->fDistNear + tess.shader->fDistRange;

	// HZM gl2 [2026-09-27] r_hzmLodFog (docs/proposals/fog_lod_pop_2026-09-27 plan piece B): a static model the
	// floor governs uses the moved band - the same one tr_staticmodels.cpp's coarse culls use. Measured from the
	// origin already, so no gate is needed: near' >= floor + cull radius puts the whole model past 100% fog.
	// HZM_LODBAND_AUTHORED (the default, r_hzmLodFog 0) leaves fNear / fRange / fFar exactly as above.
	if ( backEnd.currentStaticModel ) {
		const float lodFloor = R_HZM_LodFogFloorForView( &backEnd.viewParms );

		if ( lodFloor > 0.0f ) {
			hzmLodBand_t lodBand;

			if ( R_HZM_LodFogStaticBand( lodFloor, tess.shader, backEnd.currentStaticModel, &lodBand ) != HZM_LODBAND_AUTHORED ) {
				fNear  = lodBand.nearDist;
				fRange = lodBand.range;
				fFar   = fNear + fRange;
			}
		}
	}

	if ( lenSqr <= fNear * fNear ) {
		a = 0.0f;
	} else if ( lenSqr >= fFar * fFar ) {
		a = 1.0f;
	} else {
		// unreachable when fRange == 0 (the two clamps meet), so no divide by zero
		a = ( VectorLength( org ) - fNear ) / fRange;
	}

	if ( pStage->alphaGen == AGEN_TIKI_DIST_FADE ) {
		a = 1.0f - a;
	}

	*alphaOut = a;
	return qtrue;
}

static void ComputeShaderColors( shaderStage_t *pStage, vec4_t baseColor, vec4_t vertColor, int blend )
{
	qboolean isBlend = ((blend & GLS_SRCBLEND_BITS) == GLS_SRCBLEND_DST_COLOR)
		|| ((blend & GLS_SRCBLEND_BITS) == GLS_SRCBLEND_ONE_MINUS_DST_COLOR)
		|| ((blend & GLS_DSTBLEND_BITS) == GLS_DSTBLEND_SRC_COLOR)
		|| ((blend & GLS_DSTBLEND_BITS) == GLS_DSTBLEND_ONE_MINUS_SRC_COLOR);

	qboolean is2DDraw = backEnd.currentEntity == &backEnd.entity2D;

	float overbright = (isBlend || is2DDraw) ? 1.0f : (float)(1 << tr.overbrightBits);

	fog_t *fog;

	baseColor[0] = 
	baseColor[1] =
	baseColor[2] =
	baseColor[3] = 1.0f;

	vertColor[0] =
	vertColor[1] =
	vertColor[2] =
	vertColor[3] = 0.0f;

	//
	// rgbGen
	//
	switch ( pStage->rgbGen )
	{
		case CGEN_EXACT_VERTEX:
		case CGEN_EXACT_VERTEX_LIT:
			baseColor[0] = 
			baseColor[1] =
			baseColor[2] = 
			baseColor[3] = 0.0f;

			vertColor[0] =
			vertColor[1] =
			vertColor[2] = overbright;
			vertColor[3] = 1.0f;
			break;
		case CGEN_CONST:
			baseColor[0] = pStage->constantColor[0] / 255.0f;
			baseColor[1] = pStage->constantColor[1] / 255.0f;
			baseColor[2] = pStage->constantColor[2] / 255.0f;
			baseColor[3] = pStage->constantColor[3] / 255.0f;
			break;
		case CGEN_GLOBAL_COLOR:
			// HZM gl2 re-port Fix 2: MOHAA 'rgbGen global' = the current 2D tint
			// (backEnd.color2D, set by RE_SetColor). Was falling through to white,
			// dropping HUD/menu/bar tints. Mirrors gl1 RB_CalcColorFromConstant.
			baseColor[0] = backEnd.color2D[0] / 255.0f;
			baseColor[1] = backEnd.color2D[1] / 255.0f;
			baseColor[2] = backEnd.color2D[2] / 255.0f;
			break;
		case CGEN_VERTEX:
		case CGEN_VERTEX_LIT:
			baseColor[0] =
			baseColor[1] =
			baseColor[2] =
			baseColor[3] = 0.0f;

			vertColor[0] =
			vertColor[1] =
			vertColor[2] =
			vertColor[3] = 1.0f;
			break;
		case CGEN_ONE_MINUS_VERTEX:
			baseColor[0] = 
			baseColor[1] =
			baseColor[2] = 1.0f;

			vertColor[0] =
			vertColor[1] =
			vertColor[2] = -1.0f;
			break;
		case CGEN_FOG:
			fog = tr.world->fogs + tess.fogNum;

			baseColor[0] = ((unsigned char *)(&fog->colorInt))[0] / 255.0f;
			baseColor[1] = ((unsigned char *)(&fog->colorInt))[1] / 255.0f;
			baseColor[2] = ((unsigned char *)(&fog->colorInt))[2] / 255.0f;
			baseColor[3] = ((unsigned char *)(&fog->colorInt))[3] / 255.0f;
			break;
		case CGEN_WAVEFORM:
			baseColor[0] = 
			baseColor[1] = 
			baseColor[2] = RB_CalcWaveColorSingle( &pStage->rgbWave );
			break;
		case CGEN_ENTITY:
			if (backEnd.currentEntity)
			{
				baseColor[0] = ((unsigned char *)backEnd.currentEntity->e.shaderRGBA)[0] / 255.0f;
				baseColor[1] = ((unsigned char *)backEnd.currentEntity->e.shaderRGBA)[1] / 255.0f;
				baseColor[2] = ((unsigned char *)backEnd.currentEntity->e.shaderRGBA)[2] / 255.0f;
				baseColor[3] = ((unsigned char *)backEnd.currentEntity->e.shaderRGBA)[3] / 255.0f;
			}
			break;
		case CGEN_ONE_MINUS_ENTITY:
			if (backEnd.currentEntity)
			{
				baseColor[0] = 1.0f - ((unsigned char *)backEnd.currentEntity->e.shaderRGBA)[0] / 255.0f;
				baseColor[1] = 1.0f - ((unsigned char *)backEnd.currentEntity->e.shaderRGBA)[1] / 255.0f;
				baseColor[2] = 1.0f - ((unsigned char *)backEnd.currentEntity->e.shaderRGBA)[2] / 255.0f;
				baseColor[3] = 1.0f - ((unsigned char *)backEnd.currentEntity->e.shaderRGBA)[3] / 255.0f;
			}
			break;
		case CGEN_IDENTITY:
		case CGEN_LIGHTING_DIFFUSE:
			baseColor[0] =
			baseColor[1] =
			baseColor[2] = overbright;
			break;
		case CGEN_IDENTITY_LIGHTING:
		case CGEN_BAD:
			break;
		//
		// OPENMOHAA-specific stuff
		//=========================
		case CGEN_STATIC:
		// HZM gl2 re-port (bug-gl2-modellight): grid/spherical model lighting
		// is computed on the CPU into tess.color (RB_FillModelLightingColors),
		// so the shader must pass the vertex color through unscaled - same
		// treatment as CGEN_STATIC's baked colors.
		case CGEN_LIGHTING_GRID:
		case CGEN_LIGHTING_SPHERICAL:
			baseColor[0] =
			baseColor[1] =
			baseColor[2] =
			baseColor[3] = 0.0f;

			vertColor[0] =
			vertColor[1] =
			vertColor[2] =
			vertColor[3] = 1.0f;
			break;
		//=========================
	}

	//
	// alphaGen
	//
	switch ( pStage->alphaGen )
	{
		case AGEN_SKIP:
			break;
		case AGEN_CONST:
			baseColor[3] = pStage->constantColor[3] / 255.0f;
			vertColor[3] = 0.0f;
			break;
		case AGEN_GLOBAL_ALPHA:
			// HZM gl2 re-port Fix 2: MOHAA 'alphaGen globalalpha' = current 2D tint alpha
			baseColor[3] = backEnd.color2D[3] / 255.0f;
			vertColor[3] = 0.0f;
			break;
		case AGEN_WAVEFORM:
			baseColor[3] = RB_CalcWaveAlphaSingle( &pStage->alphaWave );
			vertColor[3] = 0.0f;
			break;
		case AGEN_ENTITY:
			if (backEnd.currentEntity)
			{
				baseColor[3] = ((unsigned char *)backEnd.currentEntity->e.shaderRGBA)[3] / 255.0f;
			}
			vertColor[3] = 0.0f;
			break;
		case AGEN_ONE_MINUS_ENTITY:
			if (backEnd.currentEntity)
			{
				baseColor[3] = 1.0f - ((unsigned char *)backEnd.currentEntity->e.shaderRGBA)[3] / 255.0f;
			}
			vertColor[3] = 0.0f;
			break;
		case AGEN_VERTEX:
			baseColor[3] = 0.0f;
			vertColor[3] = 1.0f;
			break;
		case AGEN_ONE_MINUS_VERTEX:
			baseColor[3] = 1.0f;
			vertColor[3] = -1.0f;
			break;
		case AGEN_IDENTITY:
		case AGEN_LIGHTING_SPECULAR:
		case AGEN_PORTAL:
		// HZM gl2 parity (bug-1249): computed per-vertex in generic_vp CalcColor - they need
		// attr_TexCoord0 and no uniform can carry a per-vertex value. Same result as the old
		// fall-through (baseColor[3] was already 1.0f), so this is inert with the feature off.
		case AGEN_SCOORD:
		case AGEN_TCOORD:
			// Done entirely in vertex program
			baseColor[3] = 1.0f;
			vertColor[3] = 0.0f;
			break;

		// HZM gl2 parity (bug-1300): MOHAA's four distance-fade modes. gl2 parsed all
		// four (tr_shader.c) and coarse-culled on them (tr_staticmodels.cpp) but never
		// computed the alpha, and this switch had no default, so they rendered at alpha
		// 1.0 at EVERY distance. On e2l1 that made the oak LOD impostor - a flat
		// camera-facing card, ~2x the luminance of the canopy mesh it covers - snap on at
		// full opacity at the 900u cull boundary instead of fading in from 1352u to 1800u
		// as gl1 does, which is the reported "distant trees are white".
		case AGEN_DIST_FADE:
		case AGEN_ONE_MINUS_DIST_FADE:
			// PER-VERTEX: RB_FillDistFadeAlpha wrote the ramp into tess.color's alpha
			// before the attribute upload, so just pass it through. The decider must be
			// the same one the fill used, or we would sample a stale batch's alpha.
			if ( RB_DistFadeIsPerVertex() ) {
				baseColor[3] = 0.0f;
				vertColor[3] = 1.0f;
			} else {
				baseColor[3] = 1.0f;
				vertColor[3] = 0.0f;
			}
			break;

		case AGEN_TIKI_DIST_FADE:
		case AGEN_ONE_MINUS_TIKI_DIST_FADE:
			// CONSTANT per draw, measured from the model origin - a uniform, no CPU pass.
			{
				float a;

				if ( RB_DistFadeConstAlpha( pStage, &a ) ) {
					baseColor[3] = a;
				} else {
					baseColor[3] = 1.0f;
				}
				vertColor[3] = 0.0f;
			}
			break;

		// HZM gl2 (bug-1300): never let an unimplemented alphaGen silently render opaque
		// again. Behaviour is UNCHANGED - fall out carrying whatever the rgbGen switch
		// above left in baseColor[3]/vertColor[3]. Do NOT assign here: CGEN_VERTEX,
		// CGEN_EXACT_VERTEX, CGEN_STATIC, CGEN_LIGHTING_GRID and CGEN_LIGHTING_SPHERICAL
		// have already written 0.0/1.0 (vertex-alpha pass-through) and CGEN_CONST /
		// CGEN_FOG / CGEN_ENTITY / CGEN_ONE_MINUS_ENTITY a constant. Forcing opaque would
		// change 9 shipped stages - alphaGen dot + rgbGen lightingSpherical (trees.shader)
		// and + lightingGrid (coop_1936_imports.shader, alphaFunc GE128 foliage).
		// Still unimplemented in gl2: AGEN_NOISE, AGEN_SKYALPHA, AGEN_ONE_MINUS_SKYALPHA,
		// AGEN_HEIGHT_FADE. (AGEN_DOT / AGEN_ONE_MINUS_DOT: ported, bug-2508, case above default.)
		// Cost of adding a default at all: -Wswitch no longer flags a newly added
		// alphaGen_t value as unhandled; this runtime warning replaces that net.

		// HZM coop (bug-2508): alphaGen dot / oneMinusDot - computed per-vertex in generic_vp
		// CalcColor from the vertex normal and the local view origin, so like SCOORD/TCOORD the
		// CPU side only hands the GPU a neutral base. With r_hzmAlphaGenDot 0 this falls through
		// into the legacy warn-and-leave default below, unchanged from before the port.
		case AGEN_DOT:
		case AGEN_ONE_MINUS_DOT:
			if ( GLSL_HzmAlphaGenDotEnabled() ) {
				baseColor[3] = 1.0f;
				vertColor[3] = 0.0f;
				break;
			}
			// fall through
		default:
			if ( !tess.shader->alphaGenWarned ) {
				tess.shader->alphaGenWarned = qtrue;
				ri.Printf( PRINT_DEVELOPER,
					"RENDERER: shader '%s' uses alphaGen %d, which renderergl2 does not "
					"implement - leaving its rgbGen's alpha in place\n",
					tess.shader->name, (int)pStage->alphaGen );
			}
			break;
	}

	// FIXME: find some way to implement this.
#if 0
	// if in greyscale rendering mode turn all color values into greyscale.
	if(r_greyscale->integer)
	{
		int scale;
		
		for(i = 0; i < tess.numVertexes; i++)
		{
			scale = (tess.svars.colors[i][0] + tess.svars.colors[i][1] + tess.svars.colors[i][2]) / 3;
			tess.svars.colors[i][0] = tess.svars.colors[i][1] = tess.svars.colors[i][2] = scale;
		}
	}
#endif
}


static void ComputeFogValues(vec4_t fogDistanceVector, vec4_t fogDepthVector, float *eyeT)
{
	// from RB_CalcFogTexCoords()
	fog_t  *fog;
	vec3_t  local;

	if (!tess.fogNum)
		return;

	fog = tr.world->fogs + tess.fogNum;

	VectorSubtract( backEnd.or.origin, backEnd.viewParms.or.origin, local );
	fogDistanceVector[0] = -backEnd.or.modelMatrix[2];
	fogDistanceVector[1] = -backEnd.or.modelMatrix[6];
	fogDistanceVector[2] = -backEnd.or.modelMatrix[10];
	fogDistanceVector[3] = DotProduct( local, backEnd.viewParms.or.axis[0] );

	// scale the fog vectors based on the fog's thickness
	VectorScale4(fogDistanceVector, fog->tcScale, fogDistanceVector);

	// rotate the gradient vector for this orientation
	if ( fog->hasSurface ) {
		fogDepthVector[0] = fog->surface[0] * backEnd.or.axis[0][0] + 
			fog->surface[1] * backEnd.or.axis[0][1] + fog->surface[2] * backEnd.or.axis[0][2];
		fogDepthVector[1] = fog->surface[0] * backEnd.or.axis[1][0] + 
			fog->surface[1] * backEnd.or.axis[1][1] + fog->surface[2] * backEnd.or.axis[1][2];
		fogDepthVector[2] = fog->surface[0] * backEnd.or.axis[2][0] + 
			fog->surface[1] * backEnd.or.axis[2][1] + fog->surface[2] * backEnd.or.axis[2][2];
		fogDepthVector[3] = -fog->surface[3] + DotProduct( backEnd.or.origin, fog->surface );

		*eyeT = DotProduct( backEnd.or.viewOrigin, fogDepthVector ) + fogDepthVector[3];
	} else {
		*eyeT = 1;	// non-surface fog always has eye inside
	}
}


static void ComputeFogColorMask( shaderStage_t *pStage, vec4_t fogColorMask )
{
	switch(pStage->adjustColorsForFog)
	{
		case ACFF_MODULATE_RGB:
			fogColorMask[0] =
			fogColorMask[1] =
			fogColorMask[2] = 1.0f;
			fogColorMask[3] = 0.0f;
			break;
		case ACFF_MODULATE_ALPHA:
			fogColorMask[0] =
			fogColorMask[1] =
			fogColorMask[2] = 0.0f;
			fogColorMask[3] = 1.0f;
			break;
		case ACFF_MODULATE_RGBA:
			fogColorMask[0] =
			fogColorMask[1] =
			fogColorMask[2] =
			fogColorMask[3] = 1.0f;
			break;
		default:
			fogColorMask[0] =
			fogColorMask[1] =
			fogColorMask[2] =
			fogColorMask[3] = 0.0f;
			break;
	}
}


/*
===============
RB_HZMStageMaterial

HZM gl2 (r_hzmGenNormals / r_hzmSpecular): resolve the normal and specular uniforms for a
stage, applying the live overrides for stages whose TB_NORMALMAP is a map we SYNTHESISED
from the diffuse (shaderStage_t::hzmGenNormal, set in CollapseStagesToLightall).

Two callers - RB_IterateStagesGeneric and ForwardDlight - because a stage drawn through
both must not disagree about how deep its own relief is.

For any stage that is not marked, both outputs are copies of the stage's own values, so
this is a no-op by construction. For a marked stage with the master OFF, normalScale.xy
goes to zero: lightall_fp builds N.xy from (tex.rg - 0.5) * u_NormalScale.xy and recovers
N.z by normalisation, so zero XY collapses N to the interpolated surface normal, i.e.
exactly the shading the surface had before any normal map existed. That matters because
the generated image stays BOUND until the next map load - switching the master off at
runtime has to neutralise it, not merely stop generating new ones.
===============
*/
static void RB_HZMStageMaterial( const shaderStage_t *pStage, vec4_t normalScaleOut, vec4_t specularScaleOut )
{
	Vector4Copy( pStage->normalScale, normalScaleOut );
	Vector4Copy( pStage->specularScale, specularScaleOut );

	// [user 2026-08-28] PARALLAX APPLIES TO EVERY NORMAL-MAPPED STAGE, NOT JUST SYNTHESISED ONES - so
	// this sits ABOVE the generated-only early-return below. The CC0 terrain pack ships AUTHORED
	// <name>_nh maps, which are not marked hzmGenNormal; they were therefore falling through with
	// .a = the latched r_baseParallax and .z = the constant 1.0 that means NO DISTANCE FADE. Two
	// symptoms followed: authored ground bulged up close (measured displacement has far more range than
	// the luminance guess 0.04 was tuned against) and it kept the distance shimmer the fade exists to fix.
	//
	// .a is the depth; r_baseParallax feeds it at collapse time but is CVAR_LATCH, so overriding here is
	// what makes depth tunable in play at all. .z carries the fade distance - it was written as a
	// constant 1.0 and read by no shader, so the channel was free; lightall_fp treats <= 1.0 as 'no fade'.
	if ( r_hzmParallaxDepth->value >= 0.0f )
	{
		normalScaleOut[3] = CLAMP( r_hzmParallaxDepth->value, 0.0f, 0.5f );
	}
	normalScaleOut[2] = ( r_hzmParallaxFade->value > 1.0f ) ? r_hzmParallaxFade->value : 1.0f;

	// [user 2026-08-28] AUTHORED normal maps get their own strength. The synthesised ones below are
	// multiplied by r_hzmGenNormalStrength (1.5 in the shipped cfg), so when the CC0 pack replaced them
	// with real measured normals at a raw 1.0 the ground read FLATTER - not because the data is worse,
	// but because it is honest where the old one was exaggerated. Same knob, separate value, so tuning
	// real art cannot drag the synthesised fallback with it.
	if ( !pStage->hzmGenNormal )
	{
		float fAuth = CLAMP( r_hzmNormalStrength->value, 0.0f, 4.0f );
		normalScaleOut[0] = pStage->normalScale[0] * fAuth;
		normalScaleOut[1] = pStage->normalScale[1] * fAuth;
		return;
	}

	{
		float strength = r_hzmGenNormals->integer ? r_hzmGenNormalStrength->value : 0.0f;

		strength = CLAMP( strength, 0.0f, 4.0f );

		normalScaleOut[0] = pStage->normalScale[0] * strength;
		normalScaleOut[1] = pStage->normalScale[1] * strength;
	}


	// Specular reflectance, confined to exactly these stages. Deliberately NOT r_baseSpecular:
	// that one is global and latched, and was defaulted to 0 in this fork because it puts a
	// view-dependent white sheen on every lit world surface. Specular is only interesting where
	// there is relief to catch it. Needs nothing from r_specularMapping - with USE_SPECULARMAP
	// absent, lightall_fp takes `specular = vec4(1.0)` and multiplies by u_SpecularScale, so
	// this uniform IS the material (rgb = F0, a = gloss). Skipped under r_pbr, where
	// specularScale carries gloss/metalness instead.
	if ( r_hzmGenNormals->integer && !r_pbr->integer && r_hzmSpecular->value > 0.0f )
	{
		float f0    = CLAMP( r_hzmSpecular->value, 0.0f, 1.0f );
		float gloss = CLAMP( r_hzmSpecularGloss->value, 0.0f, 1.0f );

		specularScaleOut[0] = f0;
		specularScaleOut[1] = f0;
		specularScaleOut[2] = f0;
		specularScaleOut[3] = gloss;
	}
}


static void ForwardDlight( void ) {
	int		l;
	//vec3_t	origin;
	//float	scale;
	float	radius;

	int deformGen;
	vec5_t deformParams;
	
	vec4_t fogDistanceVector, fogDepthVector = {0, 0, 0, 0};
	float eyeT = 0;

	shaderCommands_t *input = &tess;
	shaderStage_t *pStage = tess.xstages[0];

	if ( !backEnd.refdef.num_dlights ) {
		return;
	}
	
	ComputeDeformValues(&deformGen, deformParams);

	ComputeFogValues(fogDistanceVector, fogDepthVector, &eyeT);

	for ( l = 0 ; l < backEnd.refdef.num_dlights ; l++ ) {
		dlight_t	*dl;
		shaderProgram_t *sp;
		vec4_t vector;
		vec4_t texMatrix[8];

		if ( !( tess.dlightBits & ( 1 << l ) ) ) {
			continue;	// this surface definitely doesn't have any of this light
		}

		dl = &backEnd.refdef.dlights[l];
		//VectorCopy( dl->transformed, origin );
		radius = dl->radius;
		//scale = 1.0f / radius;

		//if (pStage->glslShaderGroup == tr.lightallShader)
		{
			int index = pStage->glslShaderIndex;

			index &= ~LIGHTDEF_LIGHTTYPE_MASK;
			index |= LIGHTDEF_USE_LIGHT_VECTOR;

			sp = &tr.lightallShader[index];
		}

		backEnd.pc.c_lightallDraws++;

		GLSL_BindProgram(sp);

		// HZM gl2 FORWARD GLOBAL FOG (bug-1306, D-1): gl1 hard-disables GL_FOG on the dlight
		// pass (no GLS_FOG_ENABLED bit -> renderergl1/tr_backend.c:518). Uniforms are
		// per-program persistent state and this permutation is also written by
		// RB_IterateStagesGeneric, so they MUST be zeroed explicitly here or this pure-additive
		// pass inherits a stale fog colour and ADDS it onto distant geometry.
		// HZM gl2 [2026-09-26] Phase S: a spot flagged HZM_DLIGHT_FOGGED (the cgame headlights) IS fogged - as an
		// additive stage, i.e. toward black: exactly its fog transmittance (plan finding 9; RB_SetGlobalFogUniforms
		// maps ONE/ONE to a black fog target), so a pool 900 u away no longer glows through fog that hides the truck.
		// Every other light keeps the zeroing below, byte for byte.
		// HZM coop [2026-09-27] lightning: this pure-additive dlight pass never takes the flash (and the uniforms are
		// per-program persistent state shared with RB_IterateStagesGeneric, so they are zeroed explicitly here)
		RB_HzmLt_ZeroUniforms( sp );
		if ( ( dl->type & HZM_DLIGHT_FOGGED ) && dl->hzmSpot == HZM_SPOT_READY )
		{
			RB_SetGlobalFogUniforms( sp, GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE, qfalse );
		}
		else
		{
			vec4_t off = { 0.0f, 0.0f, 0.0f, 0.0f };
			GLSL_SetUniformVec4( sp, UNIFORM_GLOBALFOGCOLOR,  off );
			GLSL_SetUniformVec4( sp, UNIFORM_GLOBALFOGPARAMS, off );
		}

		// HZM rain wetness: this additive pass shares the lightall programs; a stale u_HzmWet from the main pass would
		// add the wet reflection once per dlight (vet.md B2). Always zero here.
		RB_HZMWetOff( sp );
		// HZM ground variety: the dlight pass lights the SAME varied texture as the lit pass (never the plain repeat)
		RB_HZM_GroundVarUniforms( sp, input, pStage, 0, qfalse );

		GLSL_SetUniformMat4(sp, UNIFORM_MODELVIEWPROJECTIONMATRIX, glState.modelviewProjection);
		GLSL_SetUniformVec3(sp, UNIFORM_VIEWORIGIN, backEnd.viewParms.or.origin);
		GLSL_SetUniformVec3(sp, UNIFORM_LOCALVIEWORIGIN, backEnd.or.viewOrigin);

		GLSL_SetUniformFloat(sp, UNIFORM_VERTEXLERP, glState.vertexAttribsInterpolation);

		GLSL_SetUniformInt(sp, UNIFORM_DEFORMGEN, deformGen);
		if (deformGen != DGEN_NONE)
		{
			GLSL_SetUniformFloat5(sp, UNIFORM_DEFORMPARAMS, deformParams);
			GLSL_SetUniformFloat(sp, UNIFORM_TIME, tess.shaderTime);
		}

		if ( input->fogNum ) {
			vec4_t fogColorMask;

			GLSL_SetUniformVec4(sp, UNIFORM_FOGDISTANCE, fogDistanceVector);
			GLSL_SetUniformVec4(sp, UNIFORM_FOGDEPTH, fogDepthVector);
			GLSL_SetUniformFloat(sp, UNIFORM_FOGEYET, eyeT);

			ComputeFogColorMask(pStage, fogColorMask);

			GLSL_SetUniformVec4(sp, UNIFORM_FOGCOLORMASK, fogColorMask);
		}

		{
			vec4_t baseColor;
			vec4_t vertColor;

			ComputeShaderColors(pStage, baseColor, vertColor, GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE);

			GLSL_SetUniformVec4(sp, UNIFORM_BASECOLOR, baseColor);
			GLSL_SetUniformVec4(sp, UNIFORM_VERTCOLOR, vertColor);
		}

		if (pStage->alphaGen == AGEN_PORTAL)
		{
			GLSL_SetUniformFloat(sp, UNIFORM_PORTALRANGE, tess.shader->portalRange);
		}

		GLSL_SetUniformInt(sp, UNIFORM_COLORGEN, pStage->rgbGen);
		GLSL_SetUniformInt(sp, UNIFORM_ALPHAGEN, pStage->alphaGen);

		GLSL_SetUniformVec3(sp, UNIFORM_DIRECTEDLIGHT, dl->color);

		VectorSet(vector, 0, 0, 0);
		GLSL_SetUniformVec3(sp, UNIFORM_AMBIENTLIGHT, vector);

		VectorCopy(dl->origin, vector);
		vector[3] = 1.0f;
		GLSL_SetUniformVec4(sp, UNIFORM_LIGHTORIGIN, vector);

		GLSL_SetUniformFloat(sp, UNIFORM_LIGHTRADIUS, radius);

		// HZM gl2 [2026-09-26] Phase S1: the cone for EVERY light - all zero (= omni) for every light that is not a
		// ready spot - so this permutation never carries the previous light's cone (plan R1)
		RB_HZM_SpotForwardUniform(sp, dl);

		// HZM gl2 (r_hzmGenNormals): same resolution as the main pass, so a surface drawn
		// through both cannot disagree about its own relief depth. No-op unless the stage
		// carries a generated normal map.
		{
			vec4_t dlNormalScale, dlSpecularScale;

			RB_HZMStageMaterial(pStage, dlNormalScale, dlSpecularScale);
			GLSL_SetUniformVec4(sp, UNIFORM_NORMALSCALE, dlNormalScale);
			GLSL_SetUniformVec4(sp, UNIFORM_SPECULARSCALE, dlSpecularScale);
		}

		// include GLS_DEPTHFUNC_EQUAL so alpha tested surfaces don't add light
		// where they aren't rendered
		GL_State( GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE | GLS_DEPTHFUNC_EQUAL );
		GLSL_SetUniformInt(sp, UNIFORM_ALPHATEST, 0);

		GLSL_SetUniformMat4(sp, UNIFORM_MODELMATRIX, backEnd.or.transformMatrix);

		if (pStage->bundle[TB_DIFFUSEMAP].image[0])
			R_BindAnimatedImageToTMU( &pStage->bundle[TB_DIFFUSEMAP], TB_DIFFUSEMAP);

		// bind textures that are sampled and used in the glsl shader, and
		// bind whiteImage to textures that are sampled but zeroed in the glsl shader
		//
		// alternatives:
		//  - use the last bound texture
		//     -> costs more to sample a higher res texture then throw out the result
		//  - disable texture sampling in glsl shader with #ifdefs, as before
		//     -> increases the number of shaders that must be compiled
		//

		if (pStage->bundle[TB_NORMALMAP].image[0])
		{
			R_BindAnimatedImageToTMU( &pStage->bundle[TB_NORMALMAP], TB_NORMALMAP);
		}
		else if (r_normalMapping->integer)
			GL_BindToTMU( tr.whiteImage, TB_NORMALMAP );

		if (pStage->bundle[TB_SPECULARMAP].image[0])
		{
			R_BindAnimatedImageToTMU( &pStage->bundle[TB_SPECULARMAP], TB_SPECULARMAP);
		}
		else if (r_specularMapping->integer)
			GL_BindToTMU( tr.whiteImage, TB_SPECULARMAP );

		{
			vec4_t enableTextures;

			VectorSet4(enableTextures, 0.0f, 0.0f, 0.0f, 0.0f);
			GLSL_SetUniformVec4(sp, UNIFORM_ENABLETEXTURES, enableTextures);
		}

		if (r_dlightMode->integer >= 2)
			GL_BindToTMU(tr.shadowCubemaps[l], TB_SHADOWMAP);

		ComputeTexMods( pStage, TB_DIFFUSEMAP, texMatrix );
		GLSL_SetUniformVec4(sp, UNIFORM_DIFFUSETEXMATRIX0, texMatrix[0]);
		GLSL_SetUniformVec4(sp, UNIFORM_DIFFUSETEXMATRIX1, texMatrix[1]);
		GLSL_SetUniformVec4(sp, UNIFORM_DIFFUSETEXMATRIX2, texMatrix[2]);
		GLSL_SetUniformVec4(sp, UNIFORM_DIFFUSETEXMATRIX3, texMatrix[3]);
		GLSL_SetUniformVec4(sp, UNIFORM_DIFFUSETEXMATRIX4, texMatrix[4]);
		GLSL_SetUniformVec4(sp, UNIFORM_DIFFUSETEXMATRIX5, texMatrix[5]);
		GLSL_SetUniformVec4(sp, UNIFORM_DIFFUSETEXMATRIX6, texMatrix[6]);
		GLSL_SetUniformVec4(sp, UNIFORM_DIFFUSETEXMATRIX7, texMatrix[7]);

		GLSL_SetUniformInt(sp, UNIFORM_TCGEN0, pStage->bundle[0].tcGen);

		//
		// draw
		//

		R_DrawElements(input->numIndexes, input->firstIndex);

		backEnd.pc.c_totalIndexes += tess.numIndexes;
		backEnd.pc.c_dlightIndexes += tess.numIndexes;
		backEnd.pc.c_dlightVertexes += tess.numVertexes;
	}
}


static void ProjectPshadowVBOGLSL( void ) {
	int		l;
	vec3_t	origin;
	float	radius;

	int deformGen;
	vec5_t deformParams;

	shaderCommands_t *input = &tess;

	if ( !backEnd.refdef.num_pshadows ) {
		return;
	}
	
	ComputeDeformValues(&deformGen, deformParams);

	for ( l = 0 ; l < backEnd.refdef.num_pshadows ; l++ ) {
		pshadow_t	*ps;
		shaderProgram_t *sp;
		vec4_t vector;

		if ( !( tess.pshadowBits & ( 1 << l ) ) ) {
			continue;	// this surface definitely doesn't have any of this shadow
		}

		ps = &backEnd.refdef.pshadows[l];
		VectorCopy( ps->lightOrigin, origin );
		radius = ps->lightRadius;

		sp = &tr.pshadowShader;

		GLSL_BindProgram(sp);

		GLSL_SetUniformMat4(sp, UNIFORM_MODELVIEWPROJECTIONMATRIX, glState.modelviewProjection);

		VectorCopy(origin, vector);
		vector[3] = 1.0f;
		GLSL_SetUniformVec4(sp, UNIFORM_LIGHTORIGIN, vector);

		VectorScale(ps->lightViewAxis[0], 1.0f / ps->viewRadius, vector);
		GLSL_SetUniformVec3(sp, UNIFORM_LIGHTFORWARD, vector);

		VectorScale(ps->lightViewAxis[1], 1.0f / ps->viewRadius, vector);
		GLSL_SetUniformVec3(sp, UNIFORM_LIGHTRIGHT, vector);

		VectorScale(ps->lightViewAxis[2], 1.0f / ps->viewRadius, vector);
		GLSL_SetUniformVec3(sp, UNIFORM_LIGHTUP, vector);

		GLSL_SetUniformFloat(sp, UNIFORM_LIGHTRADIUS, radius);

		// HZM gl2 [2026-09-26] S4: the spot cone mask - all zero for every non-spot shadow, where pshadow_fp skips it
		GLSL_SetUniformVec4(sp, UNIFORM_HZMLIGHTSPOT, ps->hzmSpotCone);
		GLSL_SetUniformVec4(sp, UNIFORM_HZMPSHADOWSPOT, ps->hzmSpotApex);
	  
		// include GLS_DEPTHFUNC_EQUAL so alpha tested surfaces don't add light
		// where they aren't rendered
		GL_State( GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA | GLS_DEPTHFUNC_EQUAL );
		GLSL_SetUniformInt(sp, UNIFORM_ALPHATEST, 0);

		GL_BindToTMU( tr.pshadowMaps[l], TB_DIFFUSEMAP );

		//
		// draw
		//

		R_DrawElements(input->numIndexes, input->firstIndex);

		backEnd.pc.c_totalIndexes += tess.numIndexes;
		//backEnd.pc.c_dlightIndexes += tess.numIndexes;
	}
}



/*
===================
RB_FogPass

Blends a fog texture on top of everything else
===================
*/
static void RB_FogPass( void ) {
	fog_t		*fog;
	vec4_t  color;
	vec4_t	fogDistanceVector, fogDepthVector = {0, 0, 0, 0};
	float	eyeT = 0;
	shaderProgram_t *sp;

	int deformGen;
	vec5_t deformParams;

	ComputeDeformValues(&deformGen, deformParams);

	{
		int index = 0;

		if (deformGen != DGEN_NONE)
			index |= FOGDEF_USE_DEFORM_VERTEXES;

		if (glState.vertexAnimation)
			index |= FOGDEF_USE_VERTEX_ANIMATION;
		else if (glState.boneAnimation)
			index |= FOGDEF_USE_BONE_ANIMATION;
		
		sp = &tr.fogShader[index];
	}

	backEnd.pc.c_fogDraws++;

	GLSL_BindProgram(sp);

	fog = tr.world->fogs + tess.fogNum;

	GLSL_SetUniformMat4(sp, UNIFORM_MODELVIEWPROJECTIONMATRIX, glState.modelviewProjection);

	GLSL_SetUniformFloat(sp, UNIFORM_VERTEXLERP, glState.vertexAttribsInterpolation);

	if (glState.boneAnimation)
	{
		GLSL_SetUniformMat4BoneMatrix(sp, UNIFORM_BONEMATRIX, glState.boneMatrix, glState.boneAnimation);
	}
	
	GLSL_SetUniformInt(sp, UNIFORM_DEFORMGEN, deformGen);
	if (deformGen != DGEN_NONE)
	{
		GLSL_SetUniformFloat5(sp, UNIFORM_DEFORMPARAMS, deformParams);
		GLSL_SetUniformFloat(sp, UNIFORM_TIME, tess.shaderTime);
	}

	color[0] = ((unsigned char *)(&fog->colorInt))[0] / 255.0f;
	color[1] = ((unsigned char *)(&fog->colorInt))[1] / 255.0f;
	color[2] = ((unsigned char *)(&fog->colorInt))[2] / 255.0f;
	color[3] = ((unsigned char *)(&fog->colorInt))[3] / 255.0f;
	GLSL_SetUniformVec4(sp, UNIFORM_COLOR, color);

	ComputeFogValues(fogDistanceVector, fogDepthVector, &eyeT);

	GLSL_SetUniformVec4(sp, UNIFORM_FOGDISTANCE, fogDistanceVector);
	GLSL_SetUniformVec4(sp, UNIFORM_FOGDEPTH, fogDepthVector);
	GLSL_SetUniformFloat(sp, UNIFORM_FOGEYET, eyeT);

	if ( tess.shader->fogPass == FP_EQUAL ) {
		GL_State( GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA | GLS_DEPTHFUNC_EQUAL );
	} else {
		GL_State( GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA );
	}
	GLSL_SetUniformInt(sp, UNIFORM_ALPHATEST, 0);

	R_DrawElements(tess.numIndexes, tess.firstIndex);
}


static unsigned int RB_CalcShaderVertexAttribs( shaderCommands_t *input )
{
	unsigned int vertexAttribs = input->shader->vertexAttribs;

	if(glState.vertexAnimation)
	{
		vertexAttribs |= ATTR_POSITION2;
		if (vertexAttribs & ATTR_NORMAL)
		{
			vertexAttribs |= ATTR_NORMAL2;
			vertexAttribs |= ATTR_TANGENT2;
		}
	}

	return vertexAttribs;
}

/*
=================
R_UseForwardGlobalFog

HZM gl2 FORWARD GLOBAL FOG (bug-1306). qtrue when the forward per-fragment path owns the global
fog this frame. r_globalFogDebug (the ^~^~^ GLOBALFOG log, the fraction/distance visualisers)
and r_globalFogRadial are implemented ONLY by the screen-space pass (RB_GlobalFog /
globalfog_fp.glsl), so either being set hands the frame back to the legacy path instead of
silently disabling the diagnostic tooling that was built for this exact workstream.
=================
*/
qboolean R_UseForwardGlobalFog( void )
{
	if ( !r_globalFogForward || !r_globalFogForward->integer ) {
		return qfalse;
	}
	if ( r_globalFogDebug && r_globalFogDebug->integer ) {
		return qfalse;
	}
	if ( r_globalFogRadial && r_globalFogRadial->integer ) {
		return qfalse;
	}
	return qtrue;
}

/*
=================
RB_SetGlobalFogUniforms

HZM gl2 FORWARD GLOBAL FOG (r_globalFogForward, bug-1306). Uploads the two fog uniforms for one
draw.

Everything is driven off rb_globalFog, which RB_SetupGlobalFog latches from the MAIN world view
only (it early-returns without touching the latch for portal / sky-portal / shadow sub-views).
Those sub-views are therefore suppressed here too, so a sub-view can never be fogged with a
projection it was not rasterised with.

D4 - RB_DrawSun (tr_sky.c) is a separate world-space geometry draw whose shader is NOT isSky.
Two things follow:
  - it is tied to r_globalFogSky via fogAsSky at the call site, so the sun and the sky shell it
    sits on are always fogged together. Fogging one and not the other gives an extinguished sun
    against an unfogged sky, or a bright sun in a fogged one.
  - when r_drawSunRays is on, RB_DrawSun is re-drawn into tr.sunRaysFbo as the god-ray occlusion
    MASK. Fogging that mask blacks it out and the god rays vanish entirely, so fog is hard-off
    whenever the bound FBO is tr.sunRaysFbo.

Per-stage fog target mirrors gl1 (renderergl1/tr_shader.c:3227-3278): additive blends fog toward
BLACK, modulate toward WHITE, alpha-blend and opaque toward the fog colour. (gl1 also exempts
GLS_MULTITEXTURE_ENV stages; gl2 defines the bit but never sets it - multitexture goes through
bundle[1]/UNIFORM_TEXTURE1ENV instead - so the test below is defensive only.)
=================
*/
/*
=================
HZM coop [2026-10-05] SKY FOG MODEL (docs/proposals/volumetric_clouds_2026-09-26/v11.md, part C).

The sky shell is drawn at depth range (1,1), so the forward fog scored it at zFar - and zFar is R_SetFarClip's
farthest VISIBLE world corner, not the fog's end. On a fogged map it shrinks whenever the view is enclosed (a street,
a room, a trench), the sky's fog fraction drops with it, and a clear painted sky shows through every gap above a
solid fog wall; it also changed as the camera turned. A portal-sky view (a 3D skybox) was never fogged at all.

The model: a sky draw takes the path through a fog LAYER of height H (r_hzmSkyFogHeight) toward its own elevation,
  dist = H / sin(elevation)   (below the horizon: fully fogged)
so the zenith is fogged as geometry at H, the horizon fully (seamless with the fogged terrain under it), and nothing
depends on where the camera stands. A thickly fogged map (fog end <= H) hides its sky completely; a light haze keeps
a clear zenith. The same fraction drives the volumetric clouds' composites and gate (RB_HZM_SkyFogAt), the wet film's
and the water's sky reflection.
Off - the legacy zFar behaviour, exactly - on Omaha / dday2 / m3l2 / m3l3 (R_HZM_SkyFogMapLoaded), r_hzmSkyFog 0,
r_globalFogSky 0. A portal-sky view is fogged only with r_hzmSkyFogPortal 1 (a 3D skybox is authored scenery: t1l1).
=================
*/
qboolean R_HZM_SkyFogModelOn( void )
{
	int v;

	if ( tr.hzmSkyFogDeny ) {
		return qfalse;
	}
	if ( !r_hzmSkyFog || !r_hzmSkyFog->string[0] || r_hzmSkyFog->integer == -1 ) {
		v = HZM_SKYFOG_AUTO;
	} else {
		v = r_hzmSkyFog->integer;
	}
	if ( !v || !r_hzmSkyFogHeight || r_hzmSkyFogHeight->value <= 0.0f ) {
		return qfalse;
	}
	return qtrue;
}

// u_HzmSkyFog (world up in eye space, H) on every fogged draw while the model is on (the wet film and the water read
// H for their sky reflection); u_HzmSkyVp / u_HzmSkyTan only on a sky draw, whose z flag switches ApplyGlobalFog over
void RB_HZM_SkyFogUniforms( shaderProgram_t *sp, qboolean isSky )
{
	vec4_t v;
	const float *m = backEnd.viewParms.world.modelMatrix;
	const float *p = backEnd.viewParms.projectionMatrix;

	if ( !R_HZM_SkyFogModelOn() || p[0] == 0.0f || p[5] == 0.0f ) {
		VectorSet4( v, 0.0f, 0.0f, 0.0f, 0.0f );
		GLSL_SetUniformVec4( sp, UNIFORM_HZMSKYFOG, v );
		GLSL_SetUniformVec4( sp, UNIFORM_HZMSKYTAN, v );
		return;
	}
	VectorSet4( v, m[8], m[9], m[10], r_hzmSkyFogHeight->value );
	GLSL_SetUniformVec4( sp, UNIFORM_HZMSKYFOG, v );
	if ( !isSky ) {
		VectorSet4( v, 0.0f, 0.0f, 0.0f, 0.0f );
		GLSL_SetUniformVec4( sp, UNIFORM_HZMSKYTAN, v );
		return;
	}
	VectorSet4( v, (float)backEnd.viewParms.viewportX, (float)backEnd.viewParms.viewportY,
		1.0f / (float)backEnd.viewParms.viewportWidth, 1.0f / (float)backEnd.viewParms.viewportHeight );
	GLSL_SetUniformVec4( sp, UNIFORM_HZMSKYVP, v );
	VectorSet4( v, 1.0f / p[0], 1.0f / p[5], 1.0f, 0.0f );
	GLSL_SetUniformVec4( sp, UNIFORM_HZMSKYTAN, v );
}

void RB_SetGlobalFogUniforms( shaderProgram_t *sp, int stateBits, qboolean fogAsSky )
{
	vec4_t	fogColor;
	vec4_t	fogParams;
	int		blendSrcBits, blendDstBits;
	const float	*fogRgb;
	float	fogStart, fogEnd;
	qboolean	portalSky = qfalse;

	VectorSet4( fogColor,  0.0f, 0.0f, 0.0f, 0.0f );	// alpha 0 = fog off for this draw
	VectorSet4( fogParams, 0.0f, 0.0f, 0.0f, 0.0f );

	if ( !R_UseForwardGlobalFog() ) {
		goto upload;
	}
	// HZM sky fog model: a PORTAL-SKY view is sky as a whole - its 3D skybox takes the main view's fog (latched last
	// frame; the portal view renders first) as a sky draw. Without the model it stays unfogged, as it always was.
	// [2026-10-05, in engine] OFF by default (r_hzmSkyFogPortal 0): a 3D skybox is authored content - t1l1's C-47 door
	// shows its night sky and flak through it, and the main view's ground fog blacked it out
	if ( backEnd.viewParms.isPortalSky && !backEnd.viewParms.isPortal && r_hzmSkyFogPortal && r_hzmSkyFogPortal->integer
		&& !( backEnd.viewParms.flags & (VPF_SHADOWMAP | VPF_DEPTHSHADOW) ) && R_HZM_SkyFogModelOn()
		&& rb_hzmSkyFogLatch.active && tr.frameCount - rb_hzmSkyFogLatch.frame <= 1
		&& rb_hzmSkyFogLatch.end > rb_hzmSkyFogLatch.start ) {
		portalSky = qtrue;
		fogAsSky  = qtrue;
	} else if ( !rb_globalFog.active ) {
		goto upload;
	}
	// 2D, depth-fill, cubemap capture and the god-ray occlusion mask are never fogged
	if ( backEnd.projection2D || backEnd.depthFill ) {
		goto upload;
	}
	if ( tr.sunRaysFbo && glState.currentFBO == tr.sunRaysFbo ) {
		goto upload;
	}
	/* [HZM 2026-08-31] The shader asked for "nofog", or the sun flare set the flag by hand
	   around its own draws (tr_sun_flare.cpp:539/603).
	   Read shader->noGlobalFog here rather than assigning it into tess in RB_BeginSurface:
	   the flare raises tess.no_global_fog and then issues ordinary surface draws inside that
	   window, so an assignment in RB_BeginSurface would clobber the flare's flag back off. */
	if ( tess.no_global_fog || ( tess.shader && tess.shader->noGlobalFog ) ) {
		goto upload;
	}
	if ( tr.renderCubeFbo && glState.currentFBO == tr.renderCubeFbo ) {
		goto upload;
	}
	// same view policy as the latch in RB_SetupGlobalFog (a portal-sky view only through the sky fog model above)
	if ( !portalSky && ( backEnd.viewParms.isPortal || backEnd.viewParms.isPortalSky
		|| ( backEnd.viewParms.flags & (VPF_SHADOWMAP | VPF_DEPTHSHADOW) ) ) ) {
		goto upload;
	}
	// r_globalFogSky 0 leaves the sky shell (and the sun that sits on it) alone
	if ( fogAsSky && r_globalFogSky && !r_globalFogSky->integer ) {
		goto upload;
	}
	// gl1 turns fog off on a multitexture-env stage
	if ( stateBits & GLS_MULTITEXTURE_ENV ) {
		goto upload;
	}

	fogRgb   = portalSky ? rb_hzmSkyFogLatch.color : rb_globalFog.color;
	fogStart = portalSky ? rb_hzmSkyFogLatch.start : rb_globalFog.start;
	fogEnd   = portalSky ? rb_hzmSkyFogLatch.end   : rb_globalFog.end;
	VectorCopy( fogRgb, fogColor );
	fogColor[3] = r_globalFogScale ? r_globalFogScale->value : 1.0f;

	blendSrcBits = stateBits & GLS_SRCBLEND_BITS;
	blendDstBits = stateBits & GLS_DSTBLEND_BITS;

	if ( blendSrcBits || blendDstBits ) {
		if ( ( blendSrcBits == GLS_SRCBLEND_ONE                   && blendDstBits == GLS_DSTBLEND_ONE )
			|| ( blendSrcBits == GLS_SRCBLEND_ZERO                && blendDstBits == GLS_DSTBLEND_ONE_MINUS_SRC_COLOR )
			|| ( blendSrcBits == GLS_SRCBLEND_SRC_ALPHA           && blendDstBits == GLS_DSTBLEND_ONE )
			|| ( blendSrcBits == GLS_SRCBLEND_DST_COLOR           && blendDstBits == GLS_DSTBLEND_ONE )
			|| ( blendSrcBits == GLS_SRCBLEND_ONE_MINUS_DST_COLOR && blendDstBits == GLS_DSTBLEND_ONE ) ) {
			// additive -> fog toward black, i.e. fade the contribution out
			fogColor[0] = fogColor[1] = fogColor[2] = 0.0f;
		} else if ( ( blendSrcBits == GLS_SRCBLEND_DST_COLOR && blendDstBits == GLS_DSTBLEND_ZERO )
			|| ( blendSrcBits == GLS_SRCBLEND_ZERO           && blendDstBits == GLS_DSTBLEND_SRC_COLOR ) ) {
			// modulate -> fog toward white, i.e. fade the darkening out
			fogColor[0] = fogColor[1] = fogColor[2] = 1.0f;
		} else if ( !( blendSrcBits == GLS_SRCBLEND_SRC_ALPHA && blendDstBits == GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA )
			&& !( blendSrcBits == GLS_SRCBLEND_ONE_MINUS_SRC_ALPHA && blendDstBits == GLS_DSTBLEND_SRC_ALPHA ) ) {
			// gl1 calls this combination unfoggable and leaves fog off
			fogColor[3] = 0.0f;
		}
	}

	fogParams[0] = rb_globalFog.projMat10;
	fogParams[1] = rb_globalFog.projMat14;
	fogParams[2] = fogStart;
	fogParams[3] = 1.0f / ( fogEnd - fogStart );

upload:
	GLSL_SetUniformVec4( sp, UNIFORM_GLOBALFOGCOLOR,  fogColor );
	GLSL_SetUniformVec4( sp, UNIFORM_GLOBALFOGPARAMS, fogParams );
	// HZM sky fog model: the sky flag only where this draw is fogged as sky (fogColor.a > 0)
	RB_HZM_SkyFogUniforms( sp, (qboolean)( fogAsSky && fogColor[3] > 0.0f ) );
}

// HZM sky fog model: the worlds that keep the legacy sky fog - the Omaha set (never touched: hzm_waterwet.h's list, the
// same tr.hzmOmahaWorld the water / wet passes refuse), the user's m3l2 / m3l3, and any sky naming an Omaha box
void R_HZM_SkyFogMapLoaded( void )
{
	static const char *const names[] = { "dday2", "e3l1sky", "e3l2sky", "omaha", "inteldim", 0 };
	int i, k;

	tr.hzmSkyFogDeny = qfalse;
	if ( !tr.world ) {
		return;
	}
	if ( tr.hzmOmahaWorld || !Q_stricmp( tr.world->baseName, "m3l2" ) || !Q_stricmp( tr.world->baseName, "m3l3" )
		|| !Q_stricmpn( tr.world->baseName, "m3l1", 4 ) || !Q_stricmpn( tr.world->baseName, "e3l1", 4 )
		|| !Q_stricmpn( tr.world->baseName, "e3l2", 4 ) ) {
		tr.hzmSkyFogDeny = qtrue;
		return;
	}
	for ( i = 0; i < tr.world->numsurfaces; i++ ) {
		const shader_t *sh = tr.world->surfaces[i].shader;
		if ( !sh || !sh->isSky ) {
			continue;
		}
		for ( k = 0; names[k]; k++ ) {
			if ( Q_stristr( sh->name, names[k] )
				|| ( sh->sky.outerbox[0] && Q_stristr( sh->sky.outerbox[0]->imgName, names[k] ) ) ) {
				tr.hzmSkyFogDeny = qtrue;
				return;
			}
		}
	}
}

/*
=================
RB_SetSoftParticleUniforms

HZM gl2 SOFT PARTICLES (r_softParticles, design section 5). Uploads u_SoftParticle for one draw
through the generic or lightall program. Mode 0 (inert) is uploaded for everything that is not a
qualifying emitter sprite, so a sprite that set a live mode can never leak it into the next world
draw of the same program. With r_softParticles off this early-returns and touches no uniform, so
the fragment path is byte-identical to today.

  u_SoftParticle = (1/fadeDistance, projMat[10], projMat[14], mode)
  mode 0 off, 1 fade alpha, 2 fade rgb (additive), 3 lerp toward white (modulate), 4 debug

Gated to emitter sprites (backEnd.inSpriteList) once the sprite-list depth snapshot exists
(softDepthValid), never on RF_DEPTHHACK sprites (muzzle flashes) whose depth range would mismatch
the snapshot, never 2D, only on blended sprites that do not write depth, and never on a shader that
carried the "nosoftparticles" keyword.
=================
*/
void RB_SetSoftParticleUniforms( shaderProgram_t *sp, int stateBits )
{
	vec4_t sp4;
	vec2_t invTexRes;
	int    blendSrcBits, blendDstBits;
	int    mode;
	const float *m;
	float  softEdge;

	if ( !r_softParticles || !r_softParticles->integer ) {
		return;
	}

	VectorSet4( sp4, 0.0f, 0.0f, 0.0f, 0.0f );	// mode 0 = off

	// HZM gl2 [2026-09-26] S3b: a qer_hzmSoftEdge surface in the MAIN list (the headlight fog beam) is served exactly
	// like an emitter sprite, at its own distance, once RB_HZM_SoftEdgeSnapshotAt has taken this view's snapshot.
	// 0 whenever r_hzmSoftEdge resolves off - then this is the unchanged sprite-only gate.
	softEdge = RB_HZM_SoftEdgeDistance();
	if ( softEdge > 0.0f ) {
		RB_HZM_BindSceneDepth();
	} else if ( !backEnd.inSpriteList || !backEnd.softDepthValid || backEnd.spriteDepthHack
		|| backEnd.projection2D || !tr.hdrDepthImage ) {
		goto upload;
	}
	if ( stateBits & GLS_DEPTHMASK_TRUE ) {
		goto upload;
	}
	if ( tess.shader && tess.shader->noSoftParticles ) {
		goto upload;
	}

	blendSrcBits = stateBits & GLS_SRCBLEND_BITS;
	blendDstBits = stateBits & GLS_DSTBLEND_BITS;
	if ( !blendSrcBits && !blendDstBits ) {
		goto upload;	// opaque sprite: nothing to fade
	}

	// classify the blend like RB_SetGlobalFogUniforms and pick the matching fade
	if ( ( blendSrcBits == GLS_SRCBLEND_ONE                   && blendDstBits == GLS_DSTBLEND_ONE )
		|| ( blendSrcBits == GLS_SRCBLEND_SRC_ALPHA           && blendDstBits == GLS_DSTBLEND_ONE )
		|| ( blendSrcBits == GLS_SRCBLEND_DST_COLOR           && blendDstBits == GLS_DSTBLEND_ONE )
		|| ( blendSrcBits == GLS_SRCBLEND_ONE_MINUS_DST_COLOR && blendDstBits == GLS_DSTBLEND_ONE ) ) {
		mode = 2;	// additive -> fade rgb toward black
	} else if ( ( blendSrcBits == GLS_SRCBLEND_DST_COLOR && blendDstBits == GLS_DSTBLEND_ZERO )
		|| ( blendSrcBits == GLS_SRCBLEND_ZERO           && blendDstBits == GLS_DSTBLEND_SRC_COLOR ) ) {
		mode = 3;	// modulate -> fade toward white
	} else if ( ( blendSrcBits == GLS_SRCBLEND_SRC_ALPHA          && blendDstBits == GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA )
		|| ( blendSrcBits == GLS_SRCBLEND_ONE_MINUS_SRC_ALPHA && blendDstBits == GLS_DSTBLEND_SRC_ALPHA ) ) {
		mode = 1;	// alpha blend -> fade alpha
	} else {
		goto upload;	// unknown combination: leave it alone
	}

	// a well-formed perspective projection has [10] < -1 and [14] < 0 (same guard as the fog latch)
	m = backEnd.viewParms.projectionMatrix;
	if ( m[14] >= 0.0f || m[10] >= -1.0f ) {
		goto upload;
	}

	if ( r_softParticlesDebug && r_softParticlesDebug->integer ) {
		mode = 4;
	}

	sp4[0] = 1.0f / ( ( softEdge > 0.0f ) ? softEdge : ( ( r_softParticleDistance && r_softParticleDistance->value > 0.0f )
		? r_softParticleDistance->value : 24.0f ) );
	sp4[1] = m[10];
	sp4[2] = m[14];
	sp4[3] = (float)mode;

	// the depth snapshot fills the whole scene FBO, so uv = gl_FragCoord.xy / scene size
	invTexRes[0] = 1.0f / (float)tr.sceneWidth;
	invTexRes[1] = 1.0f / (float)tr.sceneHeight;
	GLSL_SetUniformVec2( sp, UNIFORM_INVTEXRES, invTexRes );

upload:
	GLSL_SetUniformVec4( sp, UNIFORM_SOFTPARTICLE, sp4 );
}

static void RB_IterateStagesGeneric( shaderCommands_t *input )
{
	int stage;
	
	vec4_t fogDistanceVector, fogDepthVector = {0, 0, 0, 0};
	float eyeT = 0;

	int deformGen;
	vec5_t deformParams;

	qboolean renderToCubemap = tr.renderCubeFbo && glState.currentFBO == tr.renderCubeFbo;

	ComputeDeformValues(&deformGen, deformParams);

	ComputeFogValues(fogDistanceVector, fogDepthVector, &eyeT);

	for ( stage = 0; stage < MAX_SHADER_STAGES; stage++ )
	{
		shaderStage_t *pStage = input->xstages[stage];
		shaderProgram_t *sp;
		vec4_t texMatrix[8];
		qboolean stageCharLit;
		qboolean useSunShadow;
		int      stageA2C;   // HZM gl2 MSAA P4a

		if ( !pStage )
		{
			break;
		}

		// HZM gl2 CHARACTER LIGHTING (r_charLighting). Promote this stage's lightall
		// permutation from "no light type" (which is just diffuse * var_Color) to
		// LIGHTDEF_USE_LIGHT_VECTOR, so lightall_vp/fp evaluate the ambient/directed split
		// RB_SetupCharLighting produced against the per-pixel surface normal.
		//
		// Restricted to the two model-lighting rgbGens on purpose: a character TIKI can also
		// carry fullbright decal / rgbGen entity / const stages (muzzle glow, team tint,
		// gore overlays) and those must stay exactly as they are.
		//
		// Deliberately done HERE, at draw time, and not in CollapseStagesToLightall: doing it
		// at parse time would run rend2's automatic "<diffuse>_n" / "_s" lookups for every
		// character skin, would bake the decision into the shader cache (so the master could
		// not be toggled live), and would change shader->vertexAttribs for everyone.
		stageCharLit = (qboolean)( backEnd.charLight.active
		                           && !backEnd.depthFill
		                           && pStage->glslShaderGroup == tr.lightallShader
		                           && !(pStage->glslShaderIndex & LIGHTDEF_LIGHTTYPE_MASK)
		                           && ( pStage->rgbGen == CGEN_LIGHTING_SPHERICAL
		                             || pStage->rgbGen == CGEN_LIGHTING_GRID ) );
		useSunShadow = qfalse;

		if (backEnd.depthFill)
		{
			if (pStage->glslShaderGroup == tr.lightallShader)
			{
				int index = 0;

				if (backEnd.currentEntity && backEnd.currentEntity != &tr.worldEntity)
				{
					if (glState.boneAnimation)
					{
						index |= LIGHTDEF_ENTITY_BONE_ANIMATION;
					}
					else
					{
						index |= LIGHTDEF_ENTITY_VERTEX_ANIMATION;
					}
				}

				if (pStage->stateBits & GLS_ATEST_BITS)
				{
					index |= LIGHTDEF_USE_TCGEN_AND_TCMOD;
				}

				sp = &pStage->glslShaderGroup[index];
			}
			else
			{
				int shaderAttribs = 0;

				if (tess.shader->numDeforms && !ShaderRequiresCPUDeforms(tess.shader))
				{
					shaderAttribs |= GENERICDEF_USE_DEFORM_VERTEXES;
				}

				if (glState.vertexAnimation)
				{
					shaderAttribs |= GENERICDEF_USE_VERTEX_ANIMATION;
				}
				else if (glState.boneAnimation)
				{
					shaderAttribs |= GENERICDEF_USE_BONE_ANIMATION;
				}

				if (pStage->stateBits & GLS_ATEST_BITS)
				{
					shaderAttribs |= GENERICDEF_USE_TCGEN_AND_TCMOD;
				}

				sp = &tr.genericShader[shaderAttribs];
			}
		}
		else if (pStage->glslShaderGroup == tr.lightallShader)
		{
			int index = pStage->glslShaderIndex;

			// HZM gl2 character lighting: the promotion itself. Everything downstream keys
			// off the light type now being non-zero.
			if (stageCharLit)
			{
				index |= LIGHTDEF_USE_LIGHT_VECTOR;
			}

			if (backEnd.currentEntity && backEnd.currentEntity != &tr.worldEntity)
			{
				if (glState.boneAnimation)
				{
					index |= LIGHTDEF_ENTITY_BONE_ANIMATION;
				}
				else
				{
					index |= LIGHTDEF_ENTITY_VERTEX_ANIMATION;
				}
			}

			// HZM gl2 character lighting: the sun shadowmask is now REACHABLE for characters
			// (it needs a light type, which they finally have) - but it is not correct for
			// them, so it stays behind its own switch. tr.screenShadowImage is resolved from
			// the main-view z-prepass depth, and bIsCharacter surfaces are excluded from that
			// prepass, so at a character's pixels the mask describes the geometry BEHIND him;
			// with r_charShadows on that geometry sits in his own cast shadow and he would
			// darken himself. See r_charLightShadow in tr_init.c. ONE local decides it so the
			// permutation index and the texture/uniform binds further down can never disagree
			// - a program compiled with USE_SHADOWMAP but no bound mask samples garbage.
			useSunShadow = (qboolean)( r_sunlightMode->integer
			                           && (backEnd.viewParms.flags & VPF_USESUNLIGHT)
			                           && (index & LIGHTDEF_LIGHTTYPE_MASK)
			                           && ( !stageCharLit
			                             || (r_charLightShadow && r_charLightShadow->integer) ) );

			if (useSunShadow)
			{
				index |= LIGHTDEF_USE_SHADOWMAP;
			}

			if (r_lightmap->integer && ((index & LIGHTDEF_LIGHTTYPE_MASK) == LIGHTDEF_USE_LIGHTMAP))
			{
				index = LIGHTDEF_USE_TCGEN_AND_TCMOD;
			}

			sp = &pStage->glslShaderGroup[index];

			backEnd.pc.c_lightallDraws++;
		}
		else
		{
			sp = GLSL_GetGenericShaderProgram(stage);

			backEnd.pc.c_genericDraws++;
		}

		GLSL_BindProgram(sp);

		GLSL_SetUniformMat4(sp, UNIFORM_MODELVIEWPROJECTIONMATRIX, glState.modelviewProjection);
		GLSL_SetUniformVec3(sp, UNIFORM_VIEWORIGIN, backEnd.viewParms.or.origin);
		GLSL_SetUniformVec3(sp, UNIFORM_LOCALVIEWORIGIN, backEnd.or.viewOrigin);

		GLSL_SetUniformFloat(sp, UNIFORM_VERTEXLERP, glState.vertexAttribsInterpolation);

		if (glState.boneAnimation)
		{
			GLSL_SetUniformMat4BoneMatrix(sp, UNIFORM_BONEMATRIX, glState.boneMatrix, glState.boneAnimation);
		}
		
		GLSL_SetUniformInt(sp, UNIFORM_DEFORMGEN, deformGen);
		if (deformGen != DGEN_NONE)
		{
			GLSL_SetUniformFloat5(sp, UNIFORM_DEFORMPARAMS, deformParams);
			GLSL_SetUniformFloat(sp, UNIFORM_TIME, tess.shaderTime);
		}

		if ( input->fogNum ) {
			GLSL_SetUniformVec4(sp, UNIFORM_FOGDISTANCE, fogDistanceVector);
			GLSL_SetUniformVec4(sp, UNIFORM_FOGDEPTH, fogDepthVector);
			GLSL_SetUniformFloat(sp, UNIFORM_FOGEYET, eyeT);
		}

		// HZM gl2 FORWARD GLOBAL FOG (bug-1306). tr.sunShader / tr.sunFlareShader are the
		// RB_DrawSun draws (D4): they are not isSky but they live on the same shell, so they
		// follow r_globalFogSky.
		RB_SetGlobalFogUniforms( sp, pStage->stateBits,
			(qboolean)( input->shader->isSky
			            || input->shader == tr.sunShader
			            || input->shader == tr.sunFlareShader ) );
		// HZM coop [2026-09-27] realistic lightning: haze in-scatter + outdoor surface light. All zero (today's inputs)
		// unless a strike is lit; same gates as the fog (main view only) plus opaque/alpha-blend stages only.
		RB_HzmLt_SetUniforms( sp, pStage->stateBits,
			(qboolean)( input->shader->isSky
			            || input->shader == tr.sunShader
			            || input->shader == tr.sunFlareShader ) );

		// HZM gl2 SOFT PARTICLES (r_softParticles): uploads mode 0 for every non-sprite draw and
		// the qualifying fade mode for emitter sprites. No-op when r_softParticles is off.
		RB_SetSoftParticleUniforms( sp, pStage->stateBits );

		// HZM gl2 parity (bug #73 "gun over the menus"): a 2D stage must NEVER depth-test.
		// renderergl1 forces this (tr_shade.c RB_StageIteratorGeneric:
		// `if (backEnd.in2D) GL_State(pStage->stateBits | GLS_DEPTHTEST_DISABLE)`), gl2 did not.
		// Default LIGHTMAP_2D shaders carry GLS_DEPTHTEST_DISABLE in their own stateBits, so
		// plain pics were unaffected - but every SCRIPTED menu/HUD shader (escmenu,
		// menu_button_trans, m_buttonhighlight, the weapon-bar art, ...) keeps the Q3 default of
		// depth-test ON. The view model is rasterised into the near depth slice, so those menu
		// quads were depth-REJECTED exactly in the weapon's silhouette: the ESC board rendered
		// over the (far) world but was punched through by the (near) gun. The same stale near
		// depth survives into UI-only frames - RB_DrawBuffer's ghost clear is COLOR-only - which
		// is the unlit gun-shaped hole on the main menu, and the weapons-bar bleed.
		// backEnd.projection2D is gl2's `in2D`: set by Set2DWindow/RB_SetGL2D, cleared by
		// RB_BeginDrawingView, so 3D (including the armory's RDF_HUD model preview) is untouched.
		// HZM gl2 MSAA P4a (r_alphaToCoverage): 0 unless this cutout draws into the multisampled scene
		stageA2C = RB_MsaaA2CStageMode(pStage);
		if (backEnd.projection2D) {
			GL_State( pStage->stateBits | GLS_DEPTHTEST_DISABLE );
		} else {
			GL_State( pStage->stateBits | ( stageA2C ? GLS_ALPHA_TO_COVERAGE : 0 ) );
		}
		if (stageA2C)
		{
			GLSL_SetUniformInt(sp, UNIFORM_ALPHATEST, stageA2C);
		}
		else if ((pStage->stateBits & GLS_ATEST_BITS) == GLS_ATEST_GT_0)
		{
			GLSL_SetUniformInt(sp, UNIFORM_ALPHATEST, 1);
		}
		else if ((pStage->stateBits & GLS_ATEST_BITS) == GLS_ATEST_LT_80)
		{
			GLSL_SetUniformInt(sp, UNIFORM_ALPHATEST, 2);
		}
		else if ((pStage->stateBits & GLS_ATEST_BITS) == GLS_ATEST_GE_80)
		{
			GLSL_SetUniformInt(sp, UNIFORM_ALPHATEST, 3);
		}
		// HZM gl2 re-port (bug-gl2-foliage): MOHAA foliage alpha-test modes.
		// gl1 runs these through qglAlphaFunc with the r_alpha_foliage1/2 cvar
		// refs (default 0.75); gl2's GLSL alpha test only has fixed 0.5 refs,
		// so map GE_* to the GE test and LT_* to the LT test as the closest
		// gl1-faithful behavior (only reachable with r_blendtrees/r_blendbushes
		// enabled, which are 0 by default).
		else if ((pStage->stateBits & GLS_ATEST_BITS) == GLS_ATEST_GE_FOLIAGE1
			|| (pStage->stateBits & GLS_ATEST_BITS) == GLS_ATEST_GE_FOLIAGE2)
		{
			GLSL_SetUniformInt(sp, UNIFORM_ALPHATEST, 3);
		}
		else if ((pStage->stateBits & GLS_ATEST_BITS) == GLS_ATEST_LT_FOLIAGE1
			|| (pStage->stateBits & GLS_ATEST_BITS) == GLS_ATEST_LT_FOLIAGE2)
		{
			GLSL_SetUniformInt(sp, UNIFORM_ALPHATEST, 2);
		}
		else
		{
			GLSL_SetUniformInt(sp, UNIFORM_ALPHATEST, 0);
		}


		{
			vec4_t baseColor;
			vec4_t vertColor;

			ComputeShaderColors(pStage, baseColor, vertColor, pStage->stateBits);

			// HZM coop [2026-09-28] storm darkness: a sky shader's cloud / HD layers take the fog's storm tint, like the
			// outer box (tr_sky.c DrawSkySide) - additive layers included (their light is what darkens)
			if (input->shader->isSky && !backEnd.projection2D && R_HZM_Storm()->active) {
				int k;
				for (k = 0; k < 3; k++) {
					baseColor[k] *= R_HZM_Storm()->fog[k];
					vertColor[k] *= R_HZM_Storm()->fog[k];
				}
			}

			GLSL_SetUniformVec4(sp, UNIFORM_BASECOLOR, baseColor);
			GLSL_SetUniformVec4(sp, UNIFORM_VERTCOLOR, vertColor);
		}

		if (pStage->rgbGen == CGEN_LIGHTING_DIFFUSE)
		{
			vec4_t vec;

			VectorScale(backEnd.currentEntity->ambientLight, 1.0f / 255.0f, vec);
			GLSL_SetUniformVec3(sp, UNIFORM_AMBIENTLIGHT, vec);

			VectorScale(backEnd.currentEntity->directedLight, 1.0f / 255.0f, vec);
			GLSL_SetUniformVec3(sp, UNIFORM_DIRECTEDLIGHT, vec);
			
			VectorCopy(backEnd.currentEntity->lightDir, vec);
			vec[3] = 0.0f;
			GLSL_SetUniformVec4(sp, UNIFORM_LIGHTORIGIN, vec);
			GLSL_SetUniformVec3(sp, UNIFORM_MODELLIGHTDIR, backEnd.currentEntity->modelLightDir);

			GLSL_SetUniformFloat(sp, UNIFORM_LIGHTRADIUS, 0.0f);
		}

		// HZM gl2 character lighting: feed the promoted USE_LIGHT_VECTOR permutation.
		//
		//   lightall_vp:  L = u_LightOrigin.xyz - position*u_LightOrigin.w
		//                 var_ColorAmbient.rgb = u_AmbientLight  * var_Color.rgb
		//                 var_Color.rgb       *= u_DirectedLight
		//   lightall_fp:  rgb = var_Color*reflectance*(attenuation*N.L) + var_ColorAmbient*diffuse
		//
		// with u_LightOrigin.w = 0 the light is purely directional and u_LightRadius 0 makes
		// CalcLightAttenuation return exactly 1, so the result is
		//     diffuse * var_Color * (ambientFrac + directedFrac * N.L)
		// and var_Color is the flat (ambient+directed) colour RB_FillModelLightingColors just
		// wrote. That is RB_Light_Real's own formula, evaluated per pixel.
		//
		// position and normal are in WORLD space here: the permutation always carries
		// LIGHTDEF_ENTITY_VERTEX_ANIMATION (any non-world entity does), which is what defines
		// USE_MODELMATRIX in lightall_vp. lightDirWorld is world space to match.
		if (stageCharLit)
		{
			vec4_t vec;

			GLSL_SetUniformVec3(sp, UNIFORM_AMBIENTLIGHT,  backEnd.charLight.ambientFrac);
			GLSL_SetUniformVec3(sp, UNIFORM_DIRECTEDLIGHT, backEnd.charLight.directedFrac);

			VectorCopy(backEnd.charLight.lightDirWorld, vec);
			vec[3] = 0.0f;                       // w = 0 -> directional, no position term
			GLSL_SetUniformVec4(sp, UNIFORM_LIGHTORIGIN, vec);
			GLSL_SetUniformFloat(sp, UNIFORM_LIGHTRADIUS, 0.0f);   // -> attenuation == 1
		}

		// HZM gl2 [2026-09-26] Phase S1 (plan R1, vet F8): the USE_LIGHT_VECTOR programs serve ForwardDlight AND
		// character lighting AND rgbGen lightingDiffuse, and ForwardDlight's last light stays set on the program - so
		// every draw here uploads the inert all-zero cone. Programs without the uniform resolve it to -1 (no-op), and
		// an unchanged value is skipped, so this costs a compare.
		RB_HZM_SpotZeroUniform(sp);

		if (pStage->alphaGen == AGEN_PORTAL)
		{
			GLSL_SetUniformFloat(sp, UNIFORM_PORTALRANGE, tess.shader->portalRange);
		}

		// HZM gl2 [2026-09-26] searchlights S1: an ADDITIVE alphaGen dot stage (the retail tower cone `beam`) takes
		// the dot in RGB with an identity alpha - gl1's RB_CalcAlphaFromDot - through R2's CGEN_DOT branch, which
		// reads the same alphaMin/alphaMax from u_AlphaGenParams (uploaded below). R_HZM_AlphaDotToRgb is shared with
		// the permutation select in tr_glsl.c, so the two cannot disagree; it rides r_hzmRgbGenDot and is off on
		// Omaha (tr_shade_calc.c). When it answers false, both uploads are exactly what they were before.
		{
			qboolean dotToRgb = R_HZM_AlphaDotToRgb( pStage );
			int      cgen     = pStage->rgbGen;
			int      agen     = pStage->alphaGen;

			if ( dotToRgb ) {
				cgen = ( agen == AGEN_ONE_MINUS_DOT ) ? CGEN_ONE_MINUS_DOT : CGEN_DOT;
				agen = AGEN_IDENTITY;
				if ( !tess.shader->hzmDotToRgbNoted ) {
					tess.shader->hzmDotToRgbNoted = qtrue;
					ri.Printf( PRINT_ALL, "^~^~^ SEARCHLIGHT dot->rgb shader=%s\n", tess.shader->name );
				}
			} else {
				// HZM gl2 [2026-09-25] Phase R2: while r_hzmRgbGenDot resolves OFF (always on an Omaha BSP)
				// CalcColor must never run the new dot branch - it can still be compiled in for another reason (an
				// alphaGen on the same stage) - so the dot family is uploaded as CGEN_IDENTITY, which CalcColor
				// ignores exactly as it ignored the unported value before.
				if ( ( cgen == CGEN_DOT || cgen == CGEN_ONE_MINUS_DOT ) && !R_HZM_RgbGenDotOn() ) {
					cgen = CGEN_IDENTITY;
				}
				// HZM coop (bug-2508): with r_hzmAlphaGenDot 0 the GPU must see exactly what it saw
				// before the port. CalcColor can still be compiled in by rgbGen lightingDiffuse, so the
				// dot family is uploaded as AGEN_IDENTITY (inert in CalcColor) while the switch is off.
				if ( ( agen == AGEN_DOT || agen == AGEN_ONE_MINUS_DOT ) && !GLSL_HzmAlphaGenDotEnabled() ) {
					agen = AGEN_IDENTITY;
				}
			}
			GLSL_SetUniformInt(sp, UNIFORM_COLORGEN, cgen);
			GLSL_SetUniformInt(sp, UNIFORM_ALPHAGEN, agen);
		}

		// HZM coop (bug-2508): the light for alphaGen lightingSpecular. generic_vp used the
		// ioquake3 placeholder POINT (-960,1980,96) and ignored the origin MOHAA lets a stage
		// author ('alphaGen lightingSpecular a x y z', parsed into specOrigin on both renderers).
		// The parser writes that same triple as its default, so "not authored" is "still the
		// default triple": then use the worldspawn sun bridged in tr_bsp.c (a direction TOWARD
		// the sun, world space) rotated into model space the way tr_light.c builds modelLightDir.
		// An authored origin is passed verbatim as a model-space point, which is what gl1's
		// RB_CalcSpecularAlpha subtracts. Resolves to -1 (no-op) on every non-generic program.
		if (pStage->alphaGen == AGEN_LIGHTING_SPECULAR)
		{
			vec4_t   spec;
			qboolean authored = (qboolean)( pStage->specOrigin[0] != -960.0f
			                             || pStage->specOrigin[1] != 1980.0f
			                             || pStage->specOrigin[2] != 96.0f );

			if ( !authored && VectorLengthSquared( tr.sunDirection ) > 0.25f ) {
				spec[0] = DotProduct( tr.sunDirection, backEnd.or.axis[0] );
				spec[1] = DotProduct( tr.sunDirection, backEnd.or.axis[1] );
				spec[2] = DotProduct( tr.sunDirection, backEnd.or.axis[2] );
				spec[3] = 0.0f;   // direction
			} else {
				VectorCopy( pStage->specOrigin, spec );
				spec[3] = 1.0f;   // point
			}
			GLSL_SetUniformVec4(sp, UNIFORM_HZMSPECLIGHT, spec);
		}

		// HZM gl2 parity (bug-1249): the sCoord/tCoord ramp constants. gl1 computes
		//   f = (alphaMax - alphaMin) * coord + alphaMin,  clamped to [alphaConstMin, alphaConst]
		// in RB_CalcAlphaFromTexCoords (renderergl1/tr_shade_calc.c). The two clamps are BYTE fields,
		// so normalise them here rather than in GLSL. Uploaded ONLY on this path - ForwardDlight uses
		// tr.dlightallShader, which has no CalcColor, so the uniform would resolve to -1 there.
		//
		// HZM coop (bug-2226): THE CLAMP MUST NEVER BE HANDED min > max.
		//
		// gl1's alphaGen sCoord/tCoord takes FOUR parameters - min, max, constMin, const - and
		// the last two are the clamps. Almost nothing supplies the fourth: across every shipped
		// pak, 6 uses give all four and 55 give only two. When it is missing the parser leaves
		// alphaConst at its -1 SENTINEL (renderergl1/tr_shader.c:1533, mirrored in this
		// renderer's tr_shader.c) - and note the 3-parameter path continues WITHOUT a warning,
		// so nothing is ever logged.
		//
		// gl1 then does the clamp in integer 0-255 space, and with the cap at -1 the arithmetic
		// in RB_CalcAlphaFromTexCoords drives alpha to exactly 0 for every input - so the stage
		// draws nothing. Every retail shader was authored against that, which is why the water
		// shaders look right on gl1.
		//
		// Normalising -1 by /255 and passing it straight through gave GLSL
		// clamp(f, 0.0, -0.0039) - MIN GREATER THAN MAX, which the GLSL spec leaves UNDEFINED.
		// A driver is free to compute min(max(x,lo),hi) or max(min(x,hi),lo); those happen to
		// give -0.0039 and 0 respectively, so it lands on invisible here and matches gl1 by luck.
		// It is luck, not correctness: fold it differently and those 55 stages become VISIBLE,
		// which paints an extra ocean layer over the surf - a hard flat waterline and a pale
		// additive band, the exact symptom bug-1249 was originally reported for.
		//
		// So collapse the sentinel HERE, deterministically, and keep GLSL's clamp well defined.
		// clamp(f, 0, 0) is 0 for every f, which is precisely what gl1 produces.
		{
			vec4_t agp;
			float  aLo = pStage->alphaConstMin / 255.0f;
			float  aHi = pStage->alphaConst / 255.0f;

			if (pStage->alphaConst < 0) {
				// the missing-4th-parameter sentinel - gl1 yields alpha 0
				aLo = 0.0f;
				aHi = 0.0f;
			} else if (aHi < aLo) {
				// any other inversion an author could write; pin it rather than leave it undefined
				aHi = aLo;
			}

			VectorSet4(agp, pStage->alphaMin, pStage->alphaMax, aLo, aHi);
			GLSL_SetUniformVec4(sp, UNIFORM_ALPHAGENPARAMS, agp);
		}

		if ( input->fogNum )
		{
			vec4_t fogColorMask;

			ComputeFogColorMask(pStage, fogColorMask);

			GLSL_SetUniformVec4(sp, UNIFORM_FOGCOLORMASK, fogColorMask);
		}

		if (r_lightmap->integer)
		{
			vec4_t st[2];
			VectorSet4(st[0], 1.0f, 0.0f, 0.0f, 0.0f);
			VectorSet4(st[1], 0.0f, 1.0f, 0.0f, 0.0f);
			GLSL_SetUniformVec4(sp, UNIFORM_DIFFUSETEXMATRIX0, st[0]);
			GLSL_SetUniformVec4(sp, UNIFORM_DIFFUSETEXMATRIX1, st[1]);
			GLSL_SetUniformVec4(sp, UNIFORM_DIFFUSETEXMATRIX2, st[0]);
			GLSL_SetUniformVec4(sp, UNIFORM_DIFFUSETEXMATRIX3, st[1]);
			GLSL_SetUniformVec4(sp, UNIFORM_DIFFUSETEXMATRIX4, st[0]);
			GLSL_SetUniformVec4(sp, UNIFORM_DIFFUSETEXMATRIX5, st[1]);
			GLSL_SetUniformVec4(sp, UNIFORM_DIFFUSETEXMATRIX6, st[0]);
			GLSL_SetUniformVec4(sp, UNIFORM_DIFFUSETEXMATRIX7, st[1]);

			GLSL_SetUniformInt(sp, UNIFORM_TCGEN0, TCGEN_LIGHTMAP);
		}
		else
		{
			ComputeTexMods(pStage, TB_DIFFUSEMAP, texMatrix);
			GLSL_SetUniformVec4(sp, UNIFORM_DIFFUSETEXMATRIX0, texMatrix[0]);
			GLSL_SetUniformVec4(sp, UNIFORM_DIFFUSETEXMATRIX1, texMatrix[1]);
			GLSL_SetUniformVec4(sp, UNIFORM_DIFFUSETEXMATRIX2, texMatrix[2]);
			GLSL_SetUniformVec4(sp, UNIFORM_DIFFUSETEXMATRIX3, texMatrix[3]);
			GLSL_SetUniformVec4(sp, UNIFORM_DIFFUSETEXMATRIX4, texMatrix[4]);
			GLSL_SetUniformVec4(sp, UNIFORM_DIFFUSETEXMATRIX5, texMatrix[5]);
			GLSL_SetUniformVec4(sp, UNIFORM_DIFFUSETEXMATRIX6, texMatrix[6]);
			GLSL_SetUniformVec4(sp, UNIFORM_DIFFUSETEXMATRIX7, texMatrix[7]);

			GLSL_SetUniformInt(sp, UNIFORM_TCGEN0, pStage->bundle[0].tcGen);
			if (pStage->bundle[0].tcGen == TCGEN_VECTOR)
			{
				vec3_t vec;

				VectorCopy(pStage->bundle[0].tcGenVectors[0], vec);
				GLSL_SetUniformVec3(sp, UNIFORM_TCGEN0VECTOR0, vec);
				VectorCopy(pStage->bundle[0].tcGenVectors[1], vec);
				GLSL_SetUniformVec3(sp, UNIFORM_TCGEN0VECTOR1, vec);
			}
		}

		GLSL_SetUniformMat4(sp, UNIFORM_MODELMATRIX, backEnd.or.transformMatrix);

		// HZM gl2 character lighting: force the normal-map scale to zero on a promoted
		// character stage. lightall_fp samples u_NormalMap UNCONDITIONALLY under
		// USE_NORMALMAP (which tr_glsl.c defines for every lit permutation while
		// r_normalMapping is 1) and rotates the result by tangentToWorld - but RB_SkelMesh
		// writes xyz / normal / texcoords only, there is no tangent for a skeletal vertex
		// and the attribute resolves to a constant. With normalScale.xy == 0 the sampled
		// N.xy cancels and N collapses to the interpolated surface normal, which is what we
		// want and is also the untouched default for every stock character stage
		// (InitShaderEx zeroes it, and the auto "_n" lookup in CollapseStagesToLightall
		// never ran for these because they had no light type at parse time). Setting it
		// explicitly makes that a property of the code rather than of the content, so a
		// hand-authored or HD-pack normalmap stage cannot smuggle garbage tangents in.
		{
			// HZM gl2 (r_hzmGenNormals / r_hzmSpecular): resolve both material uniforms
			// together. For an unmarked stage this returns the stage's own values, so the
			// expression below is term-for-term what it was.
			vec4_t normalScale, specularScale;

			RB_HZMStageMaterial(pStage, normalScale, specularScale);

			if (stageCharLit)
			{
				// r_charLighting's promotion is draw-time only, so a character stage can never
				// have entered CollapseStagesToLightall's normal-map probe and hzmGenNormal is
				// unreachable here today. Restore both uniforms anyway rather than depend on
				// that: a skeletal vertex has no tangent, so ANY normal map on one would be
				// rotated by a constant garbage basis, and specular without relief is just
				// sheen. Belt and braces against a future change to either feature.
				VectorSet4(normalScale, 0.0f, 0.0f, 0.0f, 0.0f);
				Vector4Copy(pStage->specularScale, specularScale);
			}

			if (renderToCubemap)
			{
				// force specular to nonmetal if rendering cubemaps
				if (r_pbr->integer)
					specularScale[1] = 0.0f;
			}

			GLSL_SetUniformVec4(sp, UNIFORM_NORMALSCALE, normalScale);
			GLSL_SetUniformVec4(sp, UNIFORM_SPECULARSCALE, specularScale);
		}

		//GLSL_SetUniformFloat(sp, UNIFORM_MAPLIGHTSCALE, backEnd.refdef.mapLightScale);

		//
		// do multitexture
		//
		if ( backEnd.depthFill )
		{
			if (!(pStage->stateBits & GLS_ATEST_BITS))
				GL_BindToTMU( tr.whiteImage, TB_COLORMAP );
			else if ( pStage->bundle[TB_COLORMAP].image[0] != 0 )
				R_BindAnimatedImageToTMU( &pStage->bundle[TB_COLORMAP], TB_COLORMAP );

			// HZM gl2 re-port (bug-gl2-nextbundle2): generic programs are shared;
			// make sure a previous dual-bundle draw doesn't leak its combine mode
			// into the depth prepass (no-op on lightall programs).
			GLSL_SetUniformInt(sp, UNIFORM_TEXTURE1ENV, 0);
		}
		else if ( pStage->glslShaderGroup == tr.lightallShader )
		{
			int i;
			vec4_t enableTextures;

			// HZM gl2 character lighting: was an independent re-derivation of the same test
			// that picks the permutation above. It is now the SAME local, so a program built
			// with USE_SHADOWMAP always gets its mask and its sun uniforms, and one built
			// without never has them bound behind its back. (For every non-character stage
			// useSunShadow evaluates to exactly what this line used to test - the runtime
			// index only ever ADDS bits to pStage->glslShaderIndex, so the LIGHTTYPE mask is
			// identical for them.)
			if (useSunShadow)
			{
				// FIXME: screenShadowImage is NULL if no framebuffers
				if (tr.screenShadowImage)
					GL_BindToTMU(tr.screenShadowImage, TB_SHADOWMAP);
				// HZM gl2 P3 B0 (D1): W on TB_SUNWORLD + u_HzmSunMaskOnly; uploads 0 (= today) while legacy
				RB_SunStable_Lightall(sp, input);
				// HZM gl2 MSAA P4c: edge-sample mask matching; uploads 0 (= today) off the new path
				RB_MsaaShadowMatch(sp);
				GLSL_SetUniformVec3(sp, UNIFORM_PRIMARYLIGHTAMBIENT, backEnd.refdef.sunAmbCol);
				if (r_pbr->integer)
				{
					vec3_t color;

					color[0] = backEnd.refdef.sunCol[0] * backEnd.refdef.sunCol[0];
					color[1] = backEnd.refdef.sunCol[1] * backEnd.refdef.sunCol[1];
					color[2] = backEnd.refdef.sunCol[2] * backEnd.refdef.sunCol[2];
					GLSL_SetUniformVec3(sp, UNIFORM_PRIMARYLIGHTCOLOR, color);
				}
				else
				{
					GLSL_SetUniformVec3(sp, UNIFORM_PRIMARYLIGHTCOLOR, backEnd.refdef.sunCol);
				}
				GLSL_SetUniformVec4(sp, UNIFORM_PRIMARYLIGHTORIGIN,  backEnd.refdef.sunDir);
			}

			VectorSet4(enableTextures, 0, 0, 0, 0);
			if ((r_lightmap->integer == 1 || r_lightmap->integer == 2) && pStage->bundle[TB_LIGHTMAP].image[0])
			{
				for (i = 0; i < NUM_TEXTURE_BUNDLES; i++)
				{
					if (i == TB_COLORMAP)
						R_BindAnimatedImageToTMU( &pStage->bundle[TB_LIGHTMAP], i);
					else
						GL_BindToTMU( tr.whiteImage, i );
				}
			}
			else if (r_lightmap->integer == 3 && pStage->bundle[TB_DELUXEMAP].image[0])
			{
				for (i = 0; i < NUM_TEXTURE_BUNDLES; i++)
				{
					if (i == TB_COLORMAP)
						R_BindAnimatedImageToTMU( &pStage->bundle[TB_DELUXEMAP], i);
					else
						GL_BindToTMU( tr.whiteImage, i );
				}
			}
			else
			{
				// HZM gl2 character lighting: a promoted character stage IS lit now, and this
				// flag is what binds tr.whiteImage to TB_NORMALMAP / TB_SPECULARMAP. Those
				// samplers are read unconditionally by the lit permutation, so leaving them
				// on whatever texture the previous draw happened to leave in those TMUs is
				// not an option.
				qboolean light = (pStage->glslShaderIndex & LIGHTDEF_LIGHTTYPE_MASK) != 0 || stageCharLit;
				qboolean fastLight = !(r_normalMapping->integer || r_specularMapping->integer);

				if (pStage->bundle[TB_DIFFUSEMAP].image[0])
					R_BindAnimatedImageToTMU( &pStage->bundle[TB_DIFFUSEMAP], TB_DIFFUSEMAP);

				if (pStage->bundle[TB_LIGHTMAP].image[0])
					R_BindAnimatedImageToTMU( &pStage->bundle[TB_LIGHTMAP], TB_LIGHTMAP);

				// bind textures that are sampled and used in the glsl shader, and
				// bind whiteImage to textures that are sampled but zeroed in the glsl shader
				//
				// alternatives:
				//  - use the last bound texture
				//     -> costs more to sample a higher res texture then throw out the result
				//  - disable texture sampling in glsl shader with #ifdefs, as before
				//     -> increases the number of shaders that must be compiled
				//
				if (light && !fastLight)
				{
					if (pStage->bundle[TB_NORMALMAP].image[0])
					{
						R_BindAnimatedImageToTMU( &pStage->bundle[TB_NORMALMAP], TB_NORMALMAP);
						enableTextures[0] = 1.0f;
					}
					else if (r_normalMapping->integer)
						GL_BindToTMU( tr.whiteImage, TB_NORMALMAP );

					if (pStage->bundle[TB_DELUXEMAP].image[0])
					{
						R_BindAnimatedImageToTMU( &pStage->bundle[TB_DELUXEMAP], TB_DELUXEMAP);
						enableTextures[1] = 1.0f;
					}
					else if (r_deluxeMapping->integer)
						GL_BindToTMU( tr.whiteImage, TB_DELUXEMAP );

					if (pStage->bundle[TB_SPECULARMAP].image[0])
					{
						R_BindAnimatedImageToTMU( &pStage->bundle[TB_SPECULARMAP], TB_SPECULARMAP);
						enableTextures[2] = 1.0f;
					}
					else if (r_specularMapping->integer)
						GL_BindToTMU( tr.whiteImage, TB_SPECULARMAP );
				}

				enableTextures[3] = (r_cubeMapping->integer && !(tr.viewParms.flags & VPF_NOCUBEMAPS) && input->cubemapIndex) ? 1.0f : 0.0f;
			}

			GLSL_SetUniformVec4(sp, UNIFORM_ENABLETEXTURES, enableTextures);

			// HZM rain wetness (tr_hzm_wet.c): uploads the wet uniforms for a qualifying draw and ZERO for every
			// other lightall draw - uniforms are per-program state shared with ForwardDlight and the cube bake.
			RB_HZMWetUniforms(sp, input, pStage, stage, stageCharLit);
			// HZM ground variety (tr_hzm_groundvar.c): the same rule - a tagged world ground stage, else ZERO
			RB_HZM_GroundVarUniforms(sp, input, pStage, stage, stageCharLit);
		}
		else if ( pStage->bundle[1].image[0] != 0 )
		{
			// HZM gl2 re-port (bug-gl2-nextbundle2): MOHAA 'nextbundle' dual-texture
			// stage rendered in ONE generic pass, matching gl1's single-pass
			// multitexture (gl1 DrawMultitextured): bundle[1] goes to TMU 1
			// (u_LightMap), its tcGen/tcMods drive var_Tex2, and u_Texture1Env
			// selects the GL_MODULATE / GL_ADD combine.
			vec4_t tex1Matrix[8];

			R_BindAnimatedImageToTMU( &pStage->bundle[0], 0 );
			R_BindAnimatedImageToTMU( &pStage->bundle[1], 1 );

			ComputeTexMods( pStage, 1, tex1Matrix );
			GLSL_SetUniformVec4(sp, UNIFORM_TEXTURE1MATRIX0, tex1Matrix[0]);
			GLSL_SetUniformVec4(sp, UNIFORM_TEXTURE1MATRIX1, tex1Matrix[1]);
			GLSL_SetUniformVec4(sp, UNIFORM_TEXTURE1MATRIX2, tex1Matrix[2]);
			GLSL_SetUniformVec4(sp, UNIFORM_TEXTURE1MATRIX3, tex1Matrix[3]);
			GLSL_SetUniformVec4(sp, UNIFORM_TEXTURE1MATRIX4, tex1Matrix[4]);
			GLSL_SetUniformVec4(sp, UNIFORM_TEXTURE1MATRIX5, tex1Matrix[5]);
			GLSL_SetUniformVec4(sp, UNIFORM_TEXTURE1MATRIX6, tex1Matrix[6]);
			GLSL_SetUniformVec4(sp, UNIFORM_TEXTURE1MATRIX7, tex1Matrix[7]);
			GLSL_SetUniformInt(sp, UNIFORM_TEXTURE1TCGEN, pStage->bundle[1].tcGen);
			GLSL_SetUniformInt(sp, UNIFORM_TEXTURE1ENV,
				(pStage->multitextureEnv == GL_ADD) ? 2 : 1);
		}
		else
		{
			//
			// set state
			//
			R_BindAnimatedImageToTMU( &pStage->bundle[0], 0 );

			// HZM gl2 re-port (bug-gl2-nextbundle2): switch the shared program's
			// second bundle off for single-texture stages.
			GLSL_SetUniformInt(sp, UNIFORM_TEXTURE1ENV, 0);
		}

		//
		// testing cube map
		//
		if (!(tr.viewParms.flags & VPF_NOCUBEMAPS) && input->cubemapIndex && r_cubeMapping->integer)
		{
			vec4_t vec;
			cubemap_t *cubemap = &tr.cubemaps[input->cubemapIndex - 1];

			// FIXME: cubemap image could be NULL if cubemap isn't renderer or loaded
			if (cubemap->image)
				GL_BindToTMU( cubemap->image, TB_CUBEMAP);

			VectorSubtract(cubemap->origin, backEnd.viewParms.or.origin, vec);
			vec[3] = 1.0f;

			VectorScale4(vec, 1.0f / cubemap->parallaxRadius, vec);

			GLSL_SetUniformVec4(sp, UNIFORM_CUBEMAPINFO, vec);
		}

		//
		// draw
		//
		R_DrawElements(input->numIndexes, input->firstIndex);

		// allow skipping out to show just lightmaps during development
		if ( r_lightmap->integer && ( pStage->bundle[0].isLightmap || pStage->bundle[1].isLightmap ) )
		{
			break;
		}

		if (backEnd.depthFill)
			break;
	}
}


static void RB_RenderShadowmap( shaderCommands_t *input )
{
	int deformGen;
	vec5_t deformParams;

	ComputeDeformValues(&deformGen, deformParams);

	{
		shaderProgram_t *sp = &tr.shadowmapShader[0];

		if (glState.vertexAnimation)
		{
			sp = &tr.shadowmapShader[SHADOWMAPDEF_USE_VERTEX_ANIMATION];
		}
		else if (glState.boneAnimation)
		{
			sp = &tr.shadowmapShader[SHADOWMAPDEF_USE_BONE_ANIMATION];
		}

		vec4_t vector;

		GLSL_BindProgram(sp);

		GLSL_SetUniformMat4(sp, UNIFORM_MODELVIEWPROJECTIONMATRIX, glState.modelviewProjection);

		GLSL_SetUniformMat4(sp, UNIFORM_MODELMATRIX, backEnd.or.transformMatrix);

		GLSL_SetUniformFloat(sp, UNIFORM_VERTEXLERP, glState.vertexAttribsInterpolation);

		if (glState.boneAnimation)
		{
			GLSL_SetUniformMat4BoneMatrix(sp, UNIFORM_BONEMATRIX, glState.boneMatrix, glState.boneAnimation);
		}

		GLSL_SetUniformInt(sp, UNIFORM_DEFORMGEN, deformGen);
		if (deformGen != DGEN_NONE)
		{
			GLSL_SetUniformFloat5(sp, UNIFORM_DEFORMPARAMS, deformParams);
			GLSL_SetUniformFloat(sp, UNIFORM_TIME, tess.shaderTime);
		}

		VectorCopy(backEnd.viewParms.or.origin, vector);
		vector[3] = 1.0f;
		GLSL_SetUniformVec4(sp, UNIFORM_LIGHTORIGIN, vector);
		GLSL_SetUniformFloat(sp, UNIFORM_LIGHTRADIUS, backEnd.viewParms.zFar);

		GL_State( 0 );
		GLSL_SetUniformInt(sp, UNIFORM_ALPHATEST, 0);

		//
		// do multitexture
		//
		//if ( pStage->glslShaderGroup )
		{
			//
			// draw
			//

			R_DrawElements(input->numIndexes, input->firstIndex);
		}
	}
}



/*
===============
RB_FillModelLightingColors

HZM gl2 re-port (bug-gl2-modellight): the CPU model vertex-color lighting
pipeline. gl1 computes these colors per stage in ComputeColors
(gl1 tr_shade.c:911-1010, CGEN_LIGHTING_GRID / CGEN_LIGHTING_SPHERICAL /
CGEN_STATIC) into tess.svars.colors; gl2 uploads vertex colors once per batch
from tess.color, so the equivalent spot is here, right before the attribute
upload in RB_StageIteratorGeneric.

- Animated TIKI refentities: RB_SkelMesh only writes xyz/normals/texcoords, so
  fill tess.color from the lighting source the backend selected in
  RB_RenderDrawSurfList (flat grid color or per-vertex spherical light set).
- Static models (rgbGen static): RB_StaticMesh already filled tess.color with
  the baked radiosity colors; add gl1's per-vertex dlight contribution when a
  dynamic light touches the model (gl1 tr_shade.c:931-994).

Format bridge: the gl1-ported tess functions write byte colors; gl2's
tess.color is uint16 per channel, scaled x257 (0xff -> 0xffff), exactly like
RB_StaticMesh does.
===============
*/
// ^~^~^ SKELCLR (bug-1131 follow-up): the lighting COLOR SOURCE for a char model batch.
// g_clrFillResult is written by RB_FillModelLightingColors on EVERY call (so at the SKELPIX
// wrap it reflects THIS batch): -1=static-model path, 0=sphere path FILLED tess.color,
// 1=no entity/tiki, 2=shader needs no grid/spherical, 3=NO currentSphere/TessFunction
// (tess.color left STALE from the previous batch!). REMOVE with the rest of the SKEL* scaffolding.
static int      g_clrFillResult = -2;
static byte     g_clrFillFirst[4];
static int      g_clrFlushed[MAX_MOD_KNOWN];

static void RB_FillModelLightingColors( void )
{
	static byte lightColors[SHADER_MAX_VERTEXES][4];
	int i;

	g_clrFillResult = -1;   // ^~^~^ SKELCLR: -1 = static-model path / fell through

	if (backEnd.currentStaticModel)
	{
		// HZM gl2 [2026-09-26] Phase S1: each spot's axis in THIS static model's frame (the frame of its `transformed`
		// light origin), rotated once per light per batch, and the one-law scale (R_HZM_SpotEntityK)
		vec3_t	hzmSpotAxis[MAX_DLIGHTS];
		float	hzmSpotK = 0.0f;

		if (!tess.shader->needsLSpherical || !r_drawspherelights->integer
			|| !backEnd.currentStaticModel->useSpecialLighting) {
			return;
		}

		for (i = 0; i < backEnd.currentStaticModel->numdlights && i < MAX_DLIGHTS; i++)
		{
			const dlight_t *sdl = &backEnd.refdef.dlights[backEnd.currentStaticModel->dlights[i].index];

			if (sdl->hzmSpot == HZM_SPOT_READY)
			{
				MatrixTransformVectorRight(backEnd.currentStaticModel->axis, sdl->hzmAxis, hzmSpotAxis[i]);
				VectorNormalize(hzmSpotAxis[i]);
				hzmSpotK = R_HZM_SpotEntityK();
			}
		}

		for (i = 0; i < tess.numVertexes; i++)
		{
			int j;
			vec3_t colorout;
			vec3_t normal;
			int r, g, b;

			colorout[0] = tess.color[i][0] * (1.0f / 257.0f);
			colorout[1] = tess.color[i][1] * (1.0f / 257.0f);
			colorout[2] = tess.color[i][2] * (1.0f / 257.0f);

			R_VaoUnpackNormal(normal, tess.normal[i]);

			for (j = 0; j < backEnd.currentStaticModel->numdlights; j++)
			{
				float ooLightDistSquared;
				float dot;
				vec3_t diff;
				dlight_t* dl;

				dl = &backEnd.refdef.dlights[backEnd.currentStaticModel->dlights[j].index];
				VectorSubtract(backEnd.currentStaticModel->dlights[j].transformed, tess.xyz[i], diff);

				dot = DotProduct(diff, normal);
				if (dot >= 0)
				{
					float ooLen;

					// HZM gl2 [2026-09-26] Phase S1 (vet F14): a spot uses THE law - K x attenuation x cone x cos -
					// instead of the 7500 R cos / d^2 term below, so a prop inside the pool matches the ground at
					// its foot and nothing behind the lamp is lit. Every other light takes the untouched path.
					if (dl->hzmSpot == HZM_SPOT_READY)
					{
						vec3_t	toVert;
						float	distSq = VectorLengthSquared(diff);
						float	spotTerm;

						VectorNegate(diff, toVert);
						spotTerm = hzmSpotK * R_HZM_SpotAttenuation(distSq, dl->radius)
						         * R_HZM_SpotConeDir(hzmSpotAxis[j], dl->hzmCosOuter, dl->hzmConeK, toVert)
						         * dot / (float)sqrt(distSq > 1.0f ? distSq : 1.0f);
						colorout[0] += dl->color[0] * spotTerm;
						colorout[1] += dl->color[1] * spotTerm;
						colorout[2] += dl->color[2] * spotTerm;
						continue;
					}

					ooLen = 1.0 / VectorLengthSquared(diff);
					ooLightDistSquared = dot * (7500.0 * dl->radius * ooLen * sqrt(ooLen));
					// HZM gl2 [2026-09-25] Phase R3 EDGEFADE: this term never reaches 0 at the radius, so a prop
					// pops as a moving light passes. Lights flagged hzm_dlight_edgefade (the cgame headlights -
					// nothing else sets it) fade to 0 at their radius instead.
					if ( dl->type & hzm_dlight_edgefade )
					{
						ooLightDistSquared *= R_HZM_DlightEdgeWindow( (float)( 1.0 / sqrt( ooLen ) ), dl->radius );
					}
					// gl1 overbright-multiplies the whole baked+dlight sum in
					// ComputeColors; gl2 already baked the shift into the
					// static colors at load (R_LoadStaticModelData), so scale
					// only the dlight term - same end result
					if (tr.overbrightShift)
					{
						ooLightDistSquared *= tr.overbrightMult;
					}
					colorout[0] = dl->color[0] * ooLightDistSquared + colorout[0];
					colorout[1] = dl->color[1] * ooLightDistSquared + colorout[1];
					colorout[2] = dl->color[2] * ooLightDistSquared + colorout[2];
				}
			}

			r = colorout[0];
			g = colorout[1];
			b = colorout[2];

			if (r > 0xFF || g > 0xFF || b > 0xFF)
			{
				float t;

				t = 255.0 / (float)Q_max(r, Q_max(g, b));

				r = (int)((float)r * t);
				g = (int)((float)g * t);
				b = (int)((float)b * t);
			}

			tess.color[i][0] = r * 257;
			tess.color[i][1] = g * 257;
			tess.color[i][2] = b * 257;
			// alpha stays as RB_StaticMesh wrote it (gl1 keeps vertex alpha,
			// "Fixed in OPM" note in gl1 tr_shade.c:991-993)
		}

		return;
	}

	if (!backEnd.currentEntity || !backEnd.currentEntity->e.tiki) {
		g_clrFillResult = 1;   // ^~^~^ SKELCLR
		return;
	}

	if (!tess.shader->needsLGrid && !tess.shader->needsLSpherical) {
		g_clrFillResult = 2;   // ^~^~^ SKELCLR
		return;
	}

	// HZM gl2 character lighting (r_charLighting): the GPU now owns the directional half
	// of this batch's lighting, so the CPU must NOT also apply it - otherwise N.L lands
	// twice and the actor is over-shaded. Fill flat with (ambient + directed), the colour
	// a fully-lit surface receives; lightall's USE_LIGHT_VECTOR permutation splits it back
	// into ambient + directed*N.L per pixel using backEnd.charLight's fractions.
	// RB_SetupCharLighting only sets .active when the sphere really produced light data,
	// so anything else still falls through to the TessFunction below.
	if (backEnd.charLight.active) {
		for (i = 0; i < tess.numVertexes; i++) {
			tess.color[i][0] = (uint16_t)((int)backEnd.charLight.flatColor[0] * 257);
			tess.color[i][1] = (uint16_t)((int)backEnd.charLight.flatColor[1] * 257);
			tess.color[i][2] = (uint16_t)((int)backEnd.charLight.flatColor[2] * 257);
			tess.color[i][3] = (uint16_t)((int)backEnd.charLight.flatColor[3] * 257);
		}

		g_clrFillResult   = 0;   // ^~^~^ SKELCLR: filled (char-lighting flat path)
		g_clrFillFirst[0] = backEnd.charLight.flatColor[0];
		g_clrFillFirst[1] = backEnd.charLight.flatColor[1];
		g_clrFillFirst[2] = backEnd.charLight.flatColor[2];
		g_clrFillFirst[3] = backEnd.charLight.flatColor[3];
		return;
	}

	if (!backEnd.currentSphere || !backEnd.currentSphere->TessFunction) {
		g_clrFillResult = 3;   // ^~^~^ SKELCLR: tess.color left STALE for this batch
		return;
	}

	hzmPerf.lightFills++;	// HZM gl2 [render-perf 0] r_hzmPerfStats
	backEnd.currentSphere->TessFunction(&lightColors[0][0]);

	for (i = 0; i < tess.numVertexes; i++) {
		tess.color[i][0] = (uint16_t)((int)lightColors[i][0] * 257);
		tess.color[i][1] = (uint16_t)((int)lightColors[i][1] * 257);
		tess.color[i][2] = (uint16_t)((int)lightColors[i][2] * 257);
		tess.color[i][3] = (uint16_t)((int)lightColors[i][3] * 257);
	}

	// ^~^~^ SKELCLR: sphere path filled - keep the first raw color for the print
	g_clrFillResult  = 0;
	g_clrFillFirst[0] = lightColors[0][0];
	g_clrFillFirst[1] = lightColors[0][1];
	g_clrFillFirst[2] = lightColors[0][2];
	g_clrFillFirst[3] = lightColors[0][3];
}

/*
===============
RB_FillDistFadeAlpha

HZM gl2 parity (bug-1300): alphaGen distFade / oneMinusDistFade, the two PER-VERTEX
distance-fade modes. gl1 computes them per stage straight into tess.svars.colors
(gl1 tr_shade.c:1167-1276). gl2 has no per-vertex colour path at stage time -
ComputeShaderColors runs from RB_IterateStagesGeneric, long after RB_UpdateTessVao has
uploaded the attributes - so, exactly like RB_FillModelLightingColors above, the
equivalent spot is here, immediately before the upload.

This is viable because static-model geometry is NOT a baked VBO: RB_StaticMesh refills
tess.xyz/normal/color per batch per frame, RB_UpdateTessVao re-uploads it, and
ATTR_COLOR is already in the upload set for rgbGen static / lightingGrid /
lightingSpherical (FinishShader adds it for the rgbGen-identity case too). Cost is one
sqrt per vertex on shaders that asked for the fade, and no extra GL work at all.

Only tess.color[i][3] is written - RGB belongs to RB_StaticMesh /
RB_FillModelLightingColors, which is how gl1 splits it too.
===============
*/
static void RB_FillDistFadeAlpha( void )
{
	const shaderStage_t *pStage = NULL;
	vec3_t               eyeLocal;
	vec3_t               v;
	float                fNear, fRange;
	float                distScale = 1.0f;	// HZM r_hzmLodFog: 1 = the retail unscaled measure
	qboolean             holdNear  = qfalse;	// HZM r_hzmLodFog: the gate (see below)
	qboolean             oneMinus;
	int                  i;

	if ( !tess.shader->needsDistFade || !RB_DistFadeIsPerVertex() ) {
		return;
	}

	// alphaGen is per STAGE but fDistNear / fDistRange are per SHADER (the parser stores
	// them on `shader`, not on the stage, so the last distFade-family stage in a shader
	// already wins for all of them). One alpha channel per batch therefore means the
	// first distFade-family stage decides it - a shader mixing distFade with a stage that
	// consumes real vertex alpha is not expressible on gl1 either.
	for ( i = 0; i < MAX_SHADER_STAGES; i++ ) {
		if ( !tess.xstages[i] ) {
			break;
		}
		if ( tess.xstages[i]->alphaGen == AGEN_DIST_FADE
			|| tess.xstages[i]->alphaGen == AGEN_ONE_MINUS_DIST_FADE ) {
			pStage = tess.xstages[i];
			break;
		}
	}

	if ( !pStage ) {
		return;
	}

	oneMinus = (qboolean)( pStage->alphaGen == AGEN_ONE_MINUS_DIST_FADE );
	fNear    = tess.shader->fDistNear;
	fRange   = tess.shader->fDistRange;

	// The eye, expressed in whatever space tess.xyz is in for this draw, so the
	// subtraction below is gl1's org[] term (gl1 tr_shade.c:1176-1180 / 1200-1204).
	// gl1 recomputes this inside the vertex loop; it is loop-invariant, so hoisting it
	// is bit-identical. RB_DistFadeIsPerVertex has already guaranteed one of these two
	// branches is taken - the world / 2D / no-entity cases were rejected there.
	if ( backEnd.currentStaticModel ) {
		VectorSubtract( backEnd.viewParms.or.origin, backEnd.currentStaticModel->origin, v );
		eyeLocal[0] = DotProduct( v, backEnd.currentStaticModel->axis[0] );
		eyeLocal[1] = DotProduct( v, backEnd.currentStaticModel->axis[1] );
		eyeLocal[2] = DotProduct( v, backEnd.currentStaticModel->axis[2] );
	} else {
		VectorSubtract( backEnd.viewParms.or.origin, backEnd.currentEntity->e.origin, v );
		eyeLocal[0] = DotProduct( v, backEnd.currentEntity->e.axis[0] );
		eyeLocal[1] = DotProduct( v, backEnd.currentEntity->e.axis[1] );
		eyeLocal[2] = DotProduct( v, backEnd.currentEntity->e.axis[2] );
	}

	// HZM gl2 [2026-09-27] r_hzmLodFog (docs/proposals/fog_lod_pop_2026-09-27 plan piece B). For a static model the
	// floor governs: (1) the moved band, the same one the coarse culls use; (2) the TRUE vertex distance - tess.xyz is
	// UNSCALED model space here (RB_StaticMesh copies pStaticXyz; the scale lives only in the GL matrix,
	// R_RotateForStaticModel), so the retail measure is off by 1/scale (2x on the 0.52 common tree); (3) the gate:
	// every vertex holds its near-side value until the ORIGIN reaches floor + cull radius, because only then is every
	// fragment of the model past 100% fog (a vertex alone past near' says nothing about its neighbours).
	// HZM_LODBAND_AUTHORED (the default, r_hzmLodFog 0) leaves all three untouched - the retail loop below.
	if ( backEnd.currentStaticModel ) {
		const float lodFloor = R_HZM_LodFogFloorForView( &backEnd.viewParms );

		if ( lodFloor > 0.0f ) {
			hzmLodBand_t lodBand;

			if ( R_HZM_LodFogStaticBand( lodFloor, tess.shader, backEnd.currentStaticModel, &lodBand ) != HZM_LODBAND_AUTHORED ) {
				fNear     = lodBand.nearDist;
				fRange    = lodBand.range;
				distScale = lodBand.distScale;
				if ( lodBand.gateDist > 0.0f ) {
					vec3_t d;

					VectorSubtract( backEnd.currentStaticModel->origin, backEnd.viewParms.or.origin, d );
					holdNear = (qboolean)( VectorLength( d ) < lodBand.gateDist );
				}
			}
		}
	}

	for ( i = 0; i < tess.numVertexes; i++ ) {
		vec3_t org;
		float  len;
		int    alpha;

		if ( holdNear ) {
			// the near-side value: fade-out fully visible, fade-in fully invisible
			tess.color[i][3] = (uint16_t)( ( oneMinus ? 0 : 0xff ) * 257 );
			continue;
		}

		if ( distScale != 1.0f ) {
			org[0] = tess.xyz[i][0] * distScale - eyeLocal[0];
			org[1] = tess.xyz[i][1] * distScale - eyeLocal[1];
			org[2] = tess.xyz[i][2] * distScale - eyeLocal[2];
		} else {
			VectorSubtract( tess.xyz[i], eyeLocal, org );
		}

		if ( fRange != 0.0f ) {
			len = ( VectorLength( org ) - fNear ) / fRange;
		} else {
			// gl1 divides by zero here and lets the +/-inf fall into the clamps below.
			// Shipped content really does contain "alphaGen distFade 2304 0" (19 sites)
			// and "distFade 900 0" (14), so make the same outcome explicit and NaN-free -
			// gl1's d == fNear case casts a NaN to unsigned char, which is UB.
			len = ( VectorLength( org ) < fNear ) ? -1.0f : 2.0f;
		}

		// Ramp and clamps are gl1's (gl1 tr_shade.c:1181-1188 / 1237-1244). The clamps are
		// strict and the boundary values fall through to the else producing the same
		// numbers anyway: len == 0 gives 255 / 0, len == 1 gives 0 / 255.
		if ( len < 0.0f ) {
			alpha = oneMinus ? 0 : 0xff;
		} else if ( len > 1.0f ) {
			alpha = oneMinus ? 0xff : 0;
		} else {
			alpha = oneMinus ? (int)( len * 255.0 ) : (int)( ( 1.0 - len ) * 255.0 );
		}

		// gl2's tess.color is uint16 per channel at x257 scale (0xff -> 0xffff), exactly
		// as RB_StaticMesh and RB_FillModelLightingColors write it.
		tess.color[i][3] = (uint16_t)( alpha * 257 );
	}
}

// ============================================================================
// ^~^~^ SKELPIX - VIEW-INDEPENDENT visibility probe. Wraps each char=1 model's MAIN color-pass base
// draw with a GL_SAMPLES_PASSED occlusion query and captures the GL depth/color/scissor/blend state.
// samplesPassed > 0 => the soldier's fragments ARE passing depth+stencil (with colorMask on, they are
// written = visible); samplesPassed ~ 0 => discarded by depth/stencil. Per model, accumulated across
// its surface batches and FLUSHED on frame change, bounded to the first few frames. No camera aiming
// needed. REMOVE with the rest of the SKEL* scaffolding.
// ============================================================================
static GLuint   g_pixQuery = 0;
static qboolean g_pixQueryInit = qfalse;
static qboolean g_pixQueryActive = qfalse;
static int      g_pixFrame[MAX_MOD_KNOWN];
static int      g_pixFlushed[MAX_MOD_KNOWN];
static int      g_pixEnt[MAX_MOD_KNOWN];
static unsigned g_pixSamples[MAX_MOD_KNOWN];
static int      g_pixBatches[MAX_MOD_KNOWN];
static int      g_pixDepthTest[MAX_MOD_KNOWN];
static int      g_pixDepthFunc[MAX_MOD_KNOWN];
static int      g_pixDepthMask[MAX_MOD_KNOWN];
static int      g_pixColorMask[MAX_MOD_KNOWN];
static int      g_pixBlendOn[MAX_MOD_KNOWN];
static int      g_pixScEnabled[MAX_MOD_KNOWN];
static int      g_pixScissor[MAX_MOD_KNOWN][4];
static char     g_pixModel[MAX_MOD_KNOWN][64];
// ^~^~^ SKELPIX view/FBO identity (#ally): WHICH view and WHICH framebuffer his draw lands in.
static int      g_pixFboGL[MAX_MOD_KNOWN];      // raw GL_DRAW_FRAMEBUFFER_BINDING
static int      g_pixFboKind[MAX_MOD_KNOWN];    // 0=other 1=renderFbo 2=msaaResolve 3=NULL(backbuffer)
static int      g_pixIsPortal[MAX_MOD_KNOWN];
static int      g_pixIsPortalSky[MAX_MOD_KNOWN];
static int      g_pixRdflags[MAX_MOD_KNOWN];
static int      g_pixViewport[MAX_MOD_KNOWN][4];
static int      g_pixDrawCount[MAX_MOD_KNOWN];  // how many separate probe-wrapped draw runs this frame
static int      g_pixDrawBuf0[MAX_MOD_KNOWN];   // GL_DRAW_BUFFER0 at his draw: 0=GL_NONE(!) 0x8CE0=att0
static int      g_pixDepthFill[MAX_MOD_KNOWN];  // 1 = this draw ran during the depth PREPASS
static int      g_pixProgram[MAX_MOD_KNOWN];    // GL_CURRENT_PROGRAM at batch 1 (captured pre-draw)
static char     g_pixShaderName[MAX_MOD_KNOWN][64]; // tess.shader->name at batch 1
// ^~^~^ SKELCOL: what COLOR does his draw actually write? Two pixels (origin-center + 80px up =
// torso), read BEFORE his first batch and AFTER his last batch of the frame. after==before ->
// his color writes never land; after==fog grey -> he's painted as a fog-colored silhouette.
static int      g_colPx[MAX_MOD_KNOWN], g_colPy[MAX_MOD_KNOWN];
static unsigned char g_colBefore[MAX_MOD_KNOWN][2][3];
static unsigned char g_colAfter[MAX_MOD_KNOWN][2][3];
// ^~^~^ SKELNDC: NDC bounds of the actual draw-time tess.xyz vertex stream (per frame)
static float    g_ndcMin[MAX_MOD_KNOWN][2], g_ndcMax[MAX_MOD_KNOWN][2];
static int      g_ndcCount[MAX_MOD_KNOWN], g_ndcBehind[MAX_MOD_KNOWN];
// ^~^~^ SKELROW: 7-pixel row across the CHEST of the model's measured triangle box (previous
// frame's bounds), before/after his batches - catches the written color even if single center
// pixels fall in the legs gap. rowValid gates until bounds exist.
static int      g_rowPx[MAX_MOD_KNOWN][7], g_rowPy[MAX_MOD_KNOWN];
static int      g_rowValid[MAX_MOD_KNOWN];
static unsigned char g_rowBefore[MAX_MOD_KNOWN][7][3], g_rowAfter[MAX_MOD_KNOWN][7][3];
// ^~^~^ SKELZ (measurement only, #ally): one-shot-per-model depth-source probe. Logs the char model's
// screen-center fragment depth vs the depth ALREADY in the buffer there, plus whether gl2 routed this
// draw through the first-person WEAPON projection (weaponProjectionMatrix) instead of the world path.
static int      g_zFlushed[MAX_MOD_KNOWN];

// ^~^~^ called from R_ShutDownQueries (RE_Shutdown): the GL context is going away, so the
// cached occlusion-query name is dead. Without this, the first char draw after a
// resolution-change vid_restart used a stale query name -> crash (user repro 07-27 17:58).
void RB_SkelProbeShutdown(void)
{
	g_pixQueryInit   = qfalse;
	g_pixQueryActive = qfalse;
	Com_Memset(g_rowValid, 0, sizeof(g_rowValid));
	Com_Memset(g_clrFlushed, 0, sizeof(g_clrFlushed));
}

static void RB_SkelPix_Flush(int hm)
{
	if (hm <= 0 || hm >= MAX_MOD_KNOWN) return;
	if (g_pixBatches[hm] > 0 && g_pixFlushed[hm] < 6) {
		g_pixFlushed[hm]++;
		ri.Printf(PRINT_ALL,
			"^~^~^ SKELPIX ent=%d hModel=%d model=%s pass=opaqueMainColor batches=%d samplesPassed=%u "
			"depthTest=%d depthFunc=0x%x depthMask=%d colorMask=0x%x blend=%d scissorEn=%d scissor=[%d %d %d %d]\n",
			g_pixEnt[hm], hm, g_pixModel[hm], g_pixBatches[hm], g_pixSamples[hm],
			g_pixDepthTest[hm], g_pixDepthFunc[hm], g_pixDepthMask[hm], g_pixColorMask[hm], g_pixBlendOn[hm],
			g_pixScEnabled[hm], g_pixScissor[hm][0], g_pixScissor[hm][1], g_pixScissor[hm][2], g_pixScissor[hm][3]);
		ri.Printf(PRINT_ALL,
			"^~^~^ SKELVIEW ent=%d model=%s fboGL=%d fboKind=%s isPortal=%d isPortalSky=%d rdflags=0x%x "
			"viewport=[%d %d %d %d] drawRuns=%d depthFill=%d drawBuf0=0x%x%s\n",
			g_pixEnt[hm], g_pixModel[hm], g_pixFboGL[hm],
			g_pixFboKind[hm] == 1 ? "renderFbo" : g_pixFboKind[hm] == 2 ? "msaaResolve" :
			g_pixFboKind[hm] == 3 ? "backbuffer" : "OTHER",
			g_pixIsPortal[hm], g_pixIsPortalSky[hm], g_pixRdflags[hm],
			g_pixViewport[hm][0], g_pixViewport[hm][1], g_pixViewport[hm][2], g_pixViewport[hm][3],
			g_pixDrawCount[hm], g_pixDepthFill[hm], g_pixDrawBuf0[hm],
			g_pixDrawBuf0[hm] == 0 ? " (GL_NONE - COLOR GOES NOWHERE!)" : "");
		ri.Printf(PRINT_ALL, "^~^~^ SKELPROG ent=%d model=%s program=%d firstStageShader=%s\n",
			g_pixEnt[hm], g_pixModel[hm], g_pixProgram[hm], g_pixShaderName[hm]);
		ri.Printf(PRINT_ALL,
			"^~^~^ SKELNDC ent=%d model=%s verts=%d behindW=%d ndcX=[%.3f .. %.3f] ndcY=[%.3f .. %.3f]\n",
			g_pixEnt[hm], g_pixModel[hm], g_ndcCount[hm], g_ndcBehind[hm],
			g_ndcMin[hm][0], g_ndcMax[hm][0], g_ndcMin[hm][1], g_ndcMax[hm][1]);
		if (g_rowValid[hm]) {
			int r;
			char buf[512];
			buf[0] = 0;
			for (r = 0; r < 7; r++) {
				char one[64];
				Com_sprintf(one, sizeof(one), " [%d,%d,%d->%d,%d,%d]",
					g_rowBefore[hm][r][0], g_rowBefore[hm][r][1], g_rowBefore[hm][r][2],
					g_rowAfter[hm][r][0],  g_rowAfter[hm][r][1],  g_rowAfter[hm][r][2]);
				Q_strcat(buf, sizeof(buf), one);
			}
			ri.Printf(PRINT_ALL, "^~^~^ SKELROW ent=%d py=%d row(before->after):%s\n",
				g_pixEnt[hm], g_rowPy[hm], buf);
		}
		// derive next frame's chest row from this frame's measured triangle box
		if (g_ndcCount[hm] > 0) {
			float cx0 = g_ndcMin[hm][0], cx1 = g_ndcMax[hm][0];
			float chestY = g_ndcMin[hm][1] + 0.72f * (g_ndcMax[hm][1] - g_ndcMin[hm][1]);
			int   r;
			g_rowPy[hm] = (int)((chestY * 0.5f + 0.5f) * 720.0f);   // viewport height (diag: fixed 720 run)
			for (r = 0; r < 7; r++) {
				float fx = cx0 + (cx1 - cx0) * (0.125f + 0.75f * r / 6.0f);
				g_rowPx[hm][r] = (int)((fx * 0.5f + 0.5f) * 1280.0f);
			}
			g_rowValid[hm] = 1;
		}
		g_ndcCount[hm] = 0; g_ndcBehind[hm] = 0;
		ri.Printf(PRINT_ALL,
			"^~^~^ SKELCOL ent=%d px=%d py=%d center before=[%d %d %d] after=[%d %d %d] | torso before=[%d %d %d] after=[%d %d %d]\n",
			g_pixEnt[hm], g_colPx[hm], g_colPy[hm],
			g_colBefore[hm][0][0], g_colBefore[hm][0][1], g_colBefore[hm][0][2],
			g_colAfter[hm][0][0],  g_colAfter[hm][0][1],  g_colAfter[hm][0][2],
			g_colBefore[hm][1][0], g_colBefore[hm][1][1], g_colBefore[hm][1][2],
			g_colAfter[hm][1][0],  g_colAfter[hm][1][1],  g_colAfter[hm][1][2]);
		g_pixDrawCount[hm] = 0;
	}
}

/*
** RB_StageIteratorGeneric
*/
void RB_StageIteratorGeneric( void )
{
	shaderCommands_t *input;
	unsigned int vertexAttribs = 0;

	input = &tess;

	if (!input->numVertexes || !input->numIndexes)
	{
		return;
	}

	// HZM gl2 P3 (D2, r_shadowWFaces 2): the W bake's second pass draws the sun-facing side of one-sided surfaces;
	// two-sided members were already drawn, pushed back, in the first pass (S1)
	if (backEnd.hzmWPass == 2 && input->shader->cullType == CT_TWO_SIDED)
	{
		return;
	}

	if (tess.useInternalVao && !(backEnd.viewParms.flags & VPF_SUNWORLD))   // HZM gl2 P3 (bug-3244): W = rest pose
	{
		RB_DeformTessGeometry();
	}

	// HZM gl2 re-port (bug-gl2-modellight): CPU model lighting colors must be
	// in tess.color before the vertex attributes are uploaded below
	if (tess.useInternalVao && !backEnd.depthFill)
	{
		RB_FillModelLightingColors();
	}

	// HZM gl2 parity (bug-1300): the per-vertex distance fade writes tess.color's ALPHA,
	// so it must run after the model-lighting fill (which owns RGB) and, like it, before
	// the attribute upload below. Self-gating on shader->needsDistFade.
	RB_FillDistFadeAlpha();

	// ^~^~^ SKELFLOOD (bug-1131): r_skeldiag >= 7 floods every char model's CPU lighting
	// colors MAGENTA right before the attribute upload - a drawn-but-dark actor turns
	// magenta, a not-drawn actor stays absent. Covers ALL fill outcomes including the
	// stale-color early-outs. REMOVE with the rest of the SKEL* scaffolding.
	if (tess.useInternalVao && !backEnd.depthFill
	    && backEnd.currentEntity && backEnd.currentEntity != &tr.worldEntity
	    && backEnd.currentEntity->e.tiki && backEnd.currentEntity->e.tiki->a
	    && backEnd.currentEntity->e.tiki->a->bIsCharacter) {
		static cvar_t *skdiagFlood = NULL;
		if (!skdiagFlood) skdiagFlood = ri.Cvar_Get("r_skeldiag", "0", 0);
		if (skdiagFlood->integer >= 7) {
			int fv;
			for (fv = 0; fv < tess.numVertexes; fv++) {
				tess.color[fv][0] = 65535; tess.color[fv][1] = 0;
				tess.color[fv][2] = 65535; tess.color[fv][3] = 65535;
			}
		}
	}

	vertexAttribs = RB_CalcShaderVertexAttribs( input );

	// HZM gl2 character lighting (r_charLighting): the promoted lightall permutation reads
	// attr_Normal, and shader->vertexAttribs was computed at PARSE time from the un-promoted
	// rgbGen (ComputeVertexAttribs gives CGEN_LIGHTING_GRID colour only). Without this the
	// normal array would be left disabled and attr_Normal would come back as the constant
	// (0,0,0), collapsing N.L to zero and flattening every actor to his ambient term.
	// RB_SkelMesh already writes tess.normal for every vertex, so this costs one extra
	// attribute upload on character batches and nothing at all when the master is off.
	if (backEnd.charLight.active)
	{
		vertexAttribs |= ATTR_NORMAL;
	}

	if (tess.useInternalVao)
	{
		RB_UpdateTessVao(vertexAttribs);
	}
	else
	{
		backEnd.pc.c_staticVaoDraws++;
	}

	//
	// log this call
	//
	if ( r_logFile->integer ) 
	{
		// don't just call LogComment, or we will get
		// a call to va() every frame!
		GLimp_LogComment( va("--- RB_StageIteratorGeneric( %s ) ---\n", tess.shader->name) );
	}

	//
	// set face culling appropriately
	//
	// ^~^~^ SKELTEST r_test_twosided: force char skeletal surfaces to two-sided (disable backface cull).
	static cvar_t *r_test_twosided_sh = NULL;
	if (!r_test_twosided_sh) r_test_twosided_sh = ri.Cvar_Get("r_test_twosided", "0", 0);
	qboolean tskTwoSided = (qboolean)(r_test_twosided_sh->integer
		&& backEnd.currentEntity && backEnd.currentEntity->e.tiki
		&& backEnd.currentEntity->e.tiki->a && backEnd.currentEntity->e.tiki->a->bIsCharacter);

	if (input->shader->cullType == CT_TWO_SIDED || tskTwoSided)
	{
		GL_Cull( CT_TWO_SIDED );
	}
	else
	{
		qboolean cullFront = (input->shader->cullType == CT_FRONT_SIDED);

		// HZM gl2 P3: ...except the W bake's second (sun-facing) pass
		if ( ( backEnd.viewParms.flags & VPF_DEPTHSHADOW ) && backEnd.hzmWPass != 2 )
			cullFront = !cullFront;

		if ( backEnd.viewParms.isMirror )
			cullFront = !cullFront;

		// HZM gl2 re-port (bug-gl2-invisible-friendly-actor, ROOT cause): do NOT flip the cull face
		// for currentEntity->mirrored. MOHAA's reference renderer (gl1) never applies a per-entity
		// mirrored cull flip (gl1 tr_shade.c GL_Cull(shader->cullType); gl1 GL_Cull handles only
		// isMirror). MOHAA character skins are SINGLE-SIDED shells (only the outer, GL-back-facing
		// surface exists), so flipping their cull discards EVERY face and the model vanishes. The
		// 58-bone "sc_" scripted/cinematic characters (e2l2 briefing squadmate sc_al_brit_cmd,
		// sc_al_us_radio, sc_ax_ital_inf, ...) are placed with a negative-determinant (mirrored)
		// entity axis, so the stock-ioq3 flip made them invisible in gl2 while gl1 drew them. Non-
		// mirrored combat AI (42/50-bone) never had the flip applied, so they are unaffected. Matching
		// gl1 (no per-entity mirrored flip) is correct for all MOHAA content.
		// (was: if ( backEnd.currentEntity && backEnd.currentEntity->mirrored ) cullFront = !cullFront;)

		if (cullFront)
			GL_Cull( CT_FRONT_SIDED );
		else
			GL_Cull( CT_BACK_SIDED );
	}

	// set polygon offset if necessary
	if ( input->shader->polygonOffset )
	{
		qglEnable( GL_POLYGON_OFFSET_FILL );
	}

	//
	// render depth if in depthfill mode
	//
	if (backEnd.depthFill)
	{
		// HZM gl2 shadow acne fix (bug-1156 follow-up): sun cascade shadow depth passes get no
		// slope-scaled bias otherwise, so thin/grazing-angle geometry shimmers between lit and
		// shadowed as the camera moves. Only touches the VPF_DEPTHSHADOW pass, not the normal
		// z-prepass or the main scene draw.
		qboolean shadowBias = (qboolean)((backEnd.viewParms.flags & VPF_DEPTHSHADOW) && !input->shader->polygonOffset);
		if ( shadowBias && ( backEnd.viewParms.flags & VPF_SUNWORLD ) )
		{
			// HZM gl2 P3: W. Two-sided members (S1) and the sun-facing pass (D2) are pushed back ~r_shadowWPush
			// W texels, so a sunlit receiver never cancels its own entity shadow; the back faces keep today's bias.
			qglEnable( GL_POLYGON_OFFSET_FILL );
			if ( backEnd.hzmWPass == 2 || input->shader->cullType == CT_TWO_SIDED )
				qglPolygonOffset( backEnd.viewParms.hzmWPush[0], backEnd.viewParms.hzmWPush[1] );
			else
				qglPolygonOffset( r_shadowMapBiasFactor->value, r_shadowMapBiasUnits->value );
		}
		else if ( shadowBias && ( backEnd.viewParms.flags & VPF_SUNSTABLE ) )
		{
			// HZM gl2 P3: the stable cascades hold entities only (B0) and no world receiver is in them, so one
			// small slope bias for every caster (r_shadowSlopeBias)
			qglEnable( GL_POLYGON_OFFSET_FILL );
			qglPolygonOffset( r_shadowSlopeBias->value, 1.0f );
		}
		else if ( shadowBias )
		{
			// HZM gl2 real character shadows: skinned organic geometry with animated normals
			// is a completely different acne surface from the thin world trim
			// r_shadowMapBiasFactor/Units 4/4 was tuned for. 4/4 on a 72-unit-tall actor
			// visibly detaches the shadow from his feet ("peter-panning"), so characters get
			// their own, smaller bias. Only reachable when r_charShadows is on, because that
			// is the only way a character surface ever gets here.
			if ( r_charShadows && r_charShadows->integer
			     && backEnd.currentEntity && backEnd.currentEntity->e.tiki
			     && backEnd.currentEntity->e.tiki->a
			     && backEnd.currentEntity->e.tiki->a->bIsCharacter )
			{
				qglEnable( GL_POLYGON_OFFSET_FILL );
				qglPolygonOffset( r_charShadowBiasFactor->value, r_charShadowBiasUnits->value );
			}
			else
			{
				qglEnable( GL_POLYGON_OFFSET_FILL );
				qglPolygonOffset( r_shadowMapBiasFactor->value, r_shadowMapBiasUnits->value );
			}
		}

		RB_IterateStagesGeneric( input );

		//
		// reset polygon offset
		//
		if ( input->shader->polygonOffset )
		{
			qglDisable( GL_POLYGON_OFFSET_FILL );
		}
		if ( shadowBias )
		{
			qglDisable( GL_POLYGON_OFFSET_FILL );
			qglPolygonOffset( r_offsetFactor->value, r_offsetUnits->value );
		}

		return;
	}

	//
	// render shadowmap if in shadowmap mode
	//
	if (backEnd.viewParms.flags & VPF_SHADOWMAP)
	{
		if ( input->shader->sort == SS_OPAQUE )
		{
			RB_RenderShadowmap( input );
		}
		//
		// reset polygon offset
		//
		if ( input->shader->polygonOffset )
		{
			qglDisable( GL_POLYGON_OFFSET_FILL );
		}

		return;
	}

	//
	//
	// call shader function
	//
	// ^~^~^ SKELPIX: wrap ONLY the base opaque color draw of a char model with an occlusion query.
	{
		trRefEntity_t *pe    = backEnd.currentEntity;
		int            pixHm = (pe && pe != &tr.worldEntity && pe->e.tiki && pe->e.tiki->a
		                        && pe->e.tiki->a->bIsCharacter) ? pe->e.hModel : -1;
		static cvar_t *skdiagGate = NULL;
			if (!skdiagGate) skdiagGate = ri.Cvar_Get("r_skeldiag", "0", 0);
			// HZM 2026-07-28: hard-gate - these probes do SYNCHRONOUS occlusion-query readbacks
			// and qglReadPixels per character model (pipeline stall = the user's frame spikes)
			qboolean       pixOn = (qboolean)(skdiagGate->integer > 0 && pixHm > 0 && pixHm < MAX_MOD_KNOWN && !backEnd.depthFill
		                        && !(backEnd.viewParms.flags & (VPF_SHADOWMAP | VPF_DEPTHSHADOW))
		                        && g_pixFlushed[pixHm] < 6
		                        && !R_MsaaSceneIsMultisampled());   // ME-F18: glReadPixels on a multisample FBO is illegal

		if (pixOn) {
			int f = tr.frame_skel_index;
			if (g_pixFrame[pixHm] != f) {
				RB_SkelPix_Flush(pixHm); // flush the just-completed previous frame
				g_pixFrame[pixHm]   = f;
				g_pixEnt[pixHm]     = pe->e.entityNumber;
				g_pixSamples[pixHm] = 0;
				g_pixBatches[pixHm] = 0;
				Q_strncpyz(g_pixModel[pixHm], (pe->e.tiki->a) ? pe->e.tiki->a->name : "?", 64);
			}
			// ^~^~^ SKELCLR (bug-1131 follow-up): the color SOURCE for this batch - parent slot,
			// grid flag, packed iGridLighting, which fill path ran, the raw filled color, and the
			// first tess.color actually headed to the GPU (catches NaN/garbage/stale directly).
			if (g_pixBatches[pixHm] == 0 && g_clrFlushed[pixHm] < 6) {
				g_clrFlushed[pixHm]++;
				ri.Printf(PRINT_ALL,
					"^~^~^ SKELCLR ent=%d model=%s org=[%d %d %d] scale=%.2f parentEnt=%d gridCalc=%d "
					"iGrid=[%u %u %u %u] fillResult=%d fillFirst=[%d %d %d %d] tessColor0=[%u %u %u %u] "
					"identByte=%d fastent=%d sphere=%d\n",
					pe->e.entityNumber,
					(pe->e.tiki && pe->e.tiki->a) ? pe->e.tiki->a->name : "?",
					(int)pe->e.origin[0], (int)pe->e.origin[1], (int)pe->e.origin[2],
					pe->e.scale, pe->e.parentEntity, (int)pe->bLightGridCalculated,
					((byte *)&pe->iGridLighting)[0], ((byte *)&pe->iGridLighting)[1],
					((byte *)&pe->iGridLighting)[2], ((byte *)&pe->iGridLighting)[3],
					g_clrFillResult,
					g_clrFillFirst[0], g_clrFillFirst[1], g_clrFillFirst[2], g_clrFillFirst[3],
					(unsigned)tess.color[0][0], (unsigned)tess.color[0][1],
					(unsigned)tess.color[0][2], (unsigned)tess.color[0][3],
					tr.identityLightByte, r_fastentlight ? r_fastentlight->integer : -1,
					backEnd.currentSphere ? 1 : 0);
			}
			// ^~^~^ SKELZ depth-source probe: first surface of this model this frame, bounded.
			// Reads the depth buffer (what is occluding him) at his screen-center BEFORE he draws,
			// against his own fragment depth there, and reports whether the WEAPON projection was used.
			if (g_pixBatches[pixHm] == 0 && g_zFlushed[pixHm] < 6) {
				const float *mvp = glState.modelviewProjection; // local->clip for this entity
				float cw = mvp[15];                              // clip of local origin (0,0,0,1) = 4th column
				if (cw > 0.0f) {
					float ndcx = mvp[12] / cw, ndcy = mvp[13] / cw, ndcz = mvp[14] / cw;
					float z01  = ndcz * 0.5f + 0.5f;
					float drNear = 0.0f, drFar = 1.0f;
					float fragZ, bufZ = -1.0f;
					int   rfx = pe->e.renderfx;
					qboolean usedWeap = (qboolean)(backEnd.viewParms.weaponFovActive
					                    && (rfx & RF_DEPTHHACK) && !(rfx & RF_CROSSHAIR));
					int px = (int)(backEnd.viewParms.viewportX + (ndcx * 0.5f + 0.5f) * backEnd.viewParms.viewportWidth);
					int py = (int)(backEnd.viewParms.viewportY + (ndcy * 0.5f + 0.5f) * backEnd.viewParms.viewportHeight);
					// depth range the backend used: qglDepthRange(0,0.3) for RF_DEPTHHACK, else [0,1]
					if (rfx & RF_DEPTHHACK) { drFar = 0.3f; }
					fragZ = drNear + z01 * (drFar - drNear);   // window-space depth, same range the GPU wrote with
					if (px >= backEnd.viewParms.viewportX
					    && px < backEnd.viewParms.viewportX + backEnd.viewParms.viewportWidth
					    && py >= backEnd.viewParms.viewportY
					    && py < backEnd.viewParms.viewportY + backEnd.viewParms.viewportHeight) {
						qglReadPixels(px, py, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, &bufZ);
					}
					g_zFlushed[pixHm]++;
					ri.Printf(PRINT_ALL,
						"^~^~^ SKELZ ent=%d model=%s fragZ=%f bufZ=%f ndcCenter=[%.4f %.4f] usedWeaponProj=%d projKind=%s renderfx=0x%x\n",
						pe->e.entityNumber,
						(pe->e.tiki && pe->e.tiki->a) ? pe->e.tiki->a->name : "?",
						fragZ, bufZ, ndcx, ndcy, usedWeap ? 1 : 0, usedWeap ? "weapon" : "world", rfx);
				}
			}
			// ^~^~^ SKELNDC: project the ACTUAL draw-time vertex stream (tess.xyz) through the
			// current MVP and accumulate NDC bounds - shows WHERE ON SCREEN his triangles land,
			// independent of the frontend SKELAGG bounds (which proved only frontend skinning).
			{
				const float *m = glState.modelviewProjection;
				int   v;
				for (v = 0; v < tess.numVertexes; v++) {
					float x = tess.xyz[v][0], y = tess.xyz[v][1], zz = tess.xyz[v][2];
					float cx = m[0]*x + m[4]*y + m[8]*zz  + m[12];
					float cy = m[1]*x + m[5]*y + m[9]*zz  + m[13];
					float cw = m[3]*x + m[7]*y + m[11]*zz + m[15];
					if (cw > 0.001f) {
						float nx = cx/cw, ny = cy/cw;
						if (g_ndcCount[pixHm] == 0) {
							g_ndcMin[pixHm][0]=nx; g_ndcMax[pixHm][0]=nx;
							g_ndcMin[pixHm][1]=ny; g_ndcMax[pixHm][1]=ny;
						} else {
							if (nx<g_ndcMin[pixHm][0]) g_ndcMin[pixHm][0]=nx;
							if (nx>g_ndcMax[pixHm][0]) g_ndcMax[pixHm][0]=nx;
							if (ny<g_ndcMin[pixHm][1]) g_ndcMin[pixHm][1]=ny;
							if (ny>g_ndcMax[pixHm][1]) g_ndcMax[pixHm][1]=ny;
						}
						g_ndcCount[pixHm]++;
					} else {
						g_ndcBehind[pixHm]++;
					}
				}
			}
			// ^~^~^ SKELROW: before-colors along the chest row (first batch only)
			if (g_pixBatches[pixHm] == 0 && g_rowValid[pixHm]) {
				int r;
				for (r = 0; r < 7; r++)
					qglReadPixels(g_rowPx[pixHm][r], g_rowPy[pixHm], 1, 1, GL_RGB, GL_UNSIGNED_BYTE, g_rowBefore[pixHm][r]);
			}
			// ^~^~^ SKELCOL: before-color at origin-center + torso pixels (first batch only)
			if (g_pixBatches[pixHm] == 0) {
				const float *mvpC = glState.modelviewProjection;
				float cwC = mvpC[15];
				if (cwC > 0.0f) {
					int cx = (int)(backEnd.viewParms.viewportX
					        + ((mvpC[12] / cwC) * 0.5f + 0.5f) * backEnd.viewParms.viewportWidth);
					int cy = (int)(backEnd.viewParms.viewportY
					        + ((mvpC[13] / cwC) * 0.5f + 0.5f) * backEnd.viewParms.viewportHeight);
					if (cy < backEnd.viewParms.viewportY + 2) cy = backEnd.viewParms.viewportY + 2;
					g_colPx[pixHm] = cx; g_colPy[pixHm] = cy;
					qglReadPixels(cx, cy,      1, 1, GL_RGB, GL_UNSIGNED_BYTE, g_colBefore[pixHm][0]);
					qglReadPixels(cx, cy + 80, 1, 1, GL_RGB, GL_UNSIGNED_BYTE, g_colBefore[pixHm][1]);
				}
			}
			if (!g_pixQueryInit) { qglGenQueries(1, &g_pixQuery); g_pixQueryInit = qtrue; }
			qglBeginQuery(glRefConfig.occlusionQueryTarget, g_pixQuery);
			g_pixQueryActive = qtrue;
		}

		RB_IterateStagesGeneric( input );

		if (pixOn && g_pixQueryActive) {
			GLuint samples = 0;
			qglEndQuery(glRefConfig.occlusionQueryTarget);
			g_pixQueryActive = qfalse;
			qglGetQueryObjectuiv(g_pixQuery, GL_QUERY_RESULT, &samples); // synchronous (diagnostic)
			g_pixSamples[pixHm] += samples;
			g_pixBatches[pixHm]++;
			// ^~^~^ SKELCOL: after-color at the same two pixels (updated after every batch;
			// the last batch's read is what the flush prints)
			qglReadPixels(g_colPx[pixHm], g_colPy[pixHm],      1, 1, GL_RGB, GL_UNSIGNED_BYTE, g_colAfter[pixHm][0]);
			qglReadPixels(g_colPx[pixHm], g_colPy[pixHm] + 80, 1, 1, GL_RGB, GL_UNSIGNED_BYTE, g_colAfter[pixHm][1]);
			// ^~^~^ SKELROW: after-colors along the chest row
			if (g_rowValid[pixHm]) {
				int r;
				for (r = 0; r < 7; r++)
					qglReadPixels(g_rowPx[pixHm][r], g_rowPy[pixHm], 1, 1, GL_RGB, GL_UNSIGNED_BYTE, g_rowAfter[pixHm][r]);
			}
			if (g_pixBatches[pixHm] == 1) {
				GLboolean dt = 0, dm = 0, bl = 0, sc = 0, cm[4] = {0, 0, 0, 0};
				GLint     df = 0, sb[4] = {0, 0, 0, 0};
				qglGetBooleanv(GL_DEPTH_TEST, &dt);
				qglGetIntegerv(GL_DEPTH_FUNC, &df);
				qglGetBooleanv(GL_DEPTH_WRITEMASK, &dm);
				qglGetBooleanv(GL_COLOR_WRITEMASK, cm);
				qglGetBooleanv(GL_BLEND, &bl);
				qglGetBooleanv(GL_SCISSOR_TEST, &sc);
				qglGetIntegerv(GL_SCISSOR_BOX, sb);
				g_pixDepthTest[pixHm] = dt ? 1 : 0;
				g_pixDepthFunc[pixHm] = (int)df;
				g_pixDepthMask[pixHm] = dm ? 1 : 0;
				g_pixColorMask[pixHm] = (cm[0] ? 8 : 0) | (cm[1] ? 4 : 0) | (cm[2] ? 2 : 0) | (cm[3] ? 1 : 0);
				g_pixBlendOn[pixHm]   = bl ? 1 : 0;
				g_pixScEnabled[pixHm] = sc ? 1 : 0;
				g_pixScissor[pixHm][0] = sb[0]; g_pixScissor[pixHm][1] = sb[1];
				g_pixScissor[pixHm][2] = sb[2]; g_pixScissor[pixHm][3] = sb[3];
				// ^~^~^ SKELVIEW: which framebuffer + draw buffer + program + pass this draw targets
				{
					GLint fb = 0, db0 = 0, prog = 0;
					qglGetIntegerv(GL_CURRENT_PROGRAM, &prog);
					g_pixProgram[pixHm] = (int)prog;
					qglGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fb);
					qglGetIntegerv(GL_DRAW_BUFFER0, &db0);
					g_pixDrawBuf0[pixHm]  = (int)db0;   // 0=GL_NONE 0x8CE0=ATTACHMENT0
					g_pixDepthFill[pixHm] = backEnd.depthFill ? 1 : 0;
					g_pixFboGL[pixHm] = (int)fb;
					if (glState.currentFBO == NULL)                 g_pixFboKind[pixHm] = 3;
					else if (glState.currentFBO == tr.renderFbo)    g_pixFboKind[pixHm] = 1;
					else if (glState.currentFBO == tr.msaaResolveFbo) g_pixFboKind[pixHm] = 2;
					else                                            g_pixFboKind[pixHm] = 0;
					Q_strncpyz(g_pixShaderName[pixHm], tess.shader ? tess.shader->name : "?", sizeof(g_pixShaderName[pixHm]));
					g_pixIsPortal[pixHm]    = backEnd.viewParms.isPortal ? 1 : 0;
					g_pixIsPortalSky[pixHm] = backEnd.viewParms.isPortalSky ? 1 : 0;
					g_pixRdflags[pixHm]     = backEnd.refdef.rdflags;
					g_pixViewport[pixHm][0] = backEnd.viewParms.viewportX;
					g_pixViewport[pixHm][1] = backEnd.viewParms.viewportY;
					g_pixViewport[pixHm][2] = backEnd.viewParms.viewportWidth;
					g_pixViewport[pixHm][3] = backEnd.viewParms.viewportHeight;
				}
				g_pixDrawCount[pixHm]++;
			}
		}
	}

	//
	// pshadows!
	//
	// HZM gl2 dynamic-light cast shadows: R_DlightShadowsActive() joins the r_shadows 4
	// test as a second producer of tr.refdef.pshadows. It is PURELY a widening - with
	// r_hzmDlightShadows 0 it returns qfalse and this reduces to the original condition,
	// and tess.pshadowBits would be 0 anyway because nothing built a shadow list.
	if (glRefConfig.framebufferObject && (r_shadows->integer == 4 || R_DlightShadowsActive()) && tess.pshadowBits
		&& tess.shader->sort <= SS_OPAQUE && !(tess.shader->surfaceFlags & (SURF_NODLIGHT | SURF_SKY) ) ) {
		ProjectPshadowVBOGLSL();
	}


	// 
	// now do any dynamic lighting needed
	//
	if ( tess.dlightBits && tess.shader->sort <= SS_OPAQUE && r_lightmap->integer == 0
		&& !(tess.shader->surfaceFlags & (SURF_NODLIGHT | SURF_SKY) ) ) {
		if (tess.shader->numUnfoggedPasses == 1 && tess.xstages[0]->glslShaderGroup == tr.lightallShader
			&& (tess.xstages[0]->glslShaderIndex & LIGHTDEF_LIGHTTYPE_MASK) && r_dlightMode->integer)
		{
			ForwardDlight();
		}
		else
		{
			ProjectDlightTexture();
		}
	}

	//
	// HZM water pass (r_hzmWater, tr_hzm_water.c): allowlisted water only, the fog pass's deform recipe. With the
	// switch off RB_HZMWaterPass returns before touching any GL state.
	//
	if ( tess.shader->hzmWater ) {
		int		hzmDeformGen;
		vec5_t	hzmDeformParams;

		ComputeDeformValues( &hzmDeformGen, hzmDeformParams );
		RB_HZMWaterPass( hzmDeformGen, hzmDeformParams );
	}

	//
	// now do fog
	//
	if ( tess.fogNum && tess.shader->fogPass ) {
		RB_FogPass();
	}

	//
	// reset polygon offset
	//
	if ( input->shader->polygonOffset )
	{
		qglDisable( GL_POLYGON_OFFSET_FILL );
	}
}

/*
** RB_EndSurface
*/
void RB_EndSurface( void ) {
	shaderCommands_t *input;

	input = &tess;

	if (input->numIndexes == 0 || input->numVertexes == 0) {
		return;
	}

	if (input->indexes[SHADER_MAX_INDEXES-1] != 0) {
		ri.Error (ERR_DROP, "RB_EndSurface() - SHADER_MAX_INDEXES hit");
	}	
	if (input->xyz[SHADER_MAX_VERTEXES-1][0] != 0) {
		ri.Error (ERR_DROP, "RB_EndSurface() - SHADER_MAX_VERTEXES hit");
	}

	if ( tess.shader == tr.shadowShader ) {
		RB_ShadowTessEnd();
		return;
	}

	// for debugging of sort order issues, stop rendering after a given sort value
	if ( r_debugSort->integer && r_debugSort->integer < tess.shader->sort ) {
		return;
	}

	if (tess.useCacheVao)
	{
		// upload indexes now
		VaoCache_Commit();
	}

	//
	// update performance counters
	//
	backEnd.pc.c_shaders++;
	backEnd.pc.c_vertexes += tess.numVertexes;
	backEnd.pc.c_indexes += tess.numIndexes;
	backEnd.pc.c_totalIndexes += tess.numIndexes * tess.numPasses;

	//
	// call off to shader specific tess end function
	//
	tess.currentStageIteratorFunc();

	//
	// draw debugging stuff
	//
	if ( r_showtris->integer ) {
		DrawTris (input);
	}
	if ( r_shownormals->integer ) {
		DrawNormals (input);
	}
	// clear shader so we can tell we don't have any unclosed surfaces
	tess.numIndexes = 0;
	tess.numVertexes = 0;
	tess.firstIndex = 0;
	tess.useCacheVao = qfalse;
	tess.useInternalVao = qfalse;

	GLimp_LogComment( "----------\n" );
}
