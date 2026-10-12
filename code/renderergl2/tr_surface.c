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
// tr_surf.c
#include "tr_local.h"

/*

  THIS ENTIRE FILE IS BACK END

backEnd.currentEntity will be valid.

Tess_Begin has already been called for the surface's shader.

The modelview matrix will be set.

It is safe to actually issue drawing commands here if you don't want to
use the shader system.
*/


//============================================================================


/*
==============
RB_CheckOverflow
==============
*/
void RB_CheckOverflow( int verts, int indexes ) {
	if (tess.numVertexes + verts < SHADER_MAX_VERTEXES
		&& tess.numIndexes + indexes < SHADER_MAX_INDEXES) {
		return;
	}

	RB_EndSurface();

	if ( verts >= SHADER_MAX_VERTEXES ) {
		ri.Error(ERR_DROP, "RB_CheckOverflow: verts > MAX (%d > %d)", verts, SHADER_MAX_VERTEXES );
	}
	if ( indexes >= SHADER_MAX_INDEXES ) {
		ri.Error(ERR_DROP, "RB_CheckOverflow: indices > MAX (%d > %d)", indexes, SHADER_MAX_INDEXES );
	}

	RB_BeginSurface(tess.shader, tess.fogNum, tess.cubemapIndex );
}

void RB_CheckVao(vao_t *vao)
{
	if (vao != glState.currentVao)
	{
		RB_EndSurface();
		RB_BeginSurface(tess.shader, tess.fogNum, tess.cubemapIndex);

		R_BindVao(vao);
	}

	if (vao != tess.vao)
		tess.useInternalVao = qfalse;
}


/*
==============
RB_AddQuadStampExt
==============
*/
void RB_AddQuadStampExt( vec3_t origin, vec3_t left, vec3_t up, float color[4], float s1, float t1, float s2, float t2 ) {
	vec3_t		normal;
	int16_t     iNormal[4];
	uint16_t    iColor[4];
	int			ndx;

	RB_CheckVao(tess.vao);

	RB_CHECKOVERFLOW( 4, 6 );

	ndx = tess.numVertexes;

	// triangle indexes for a simple quad
	tess.indexes[ tess.numIndexes ] = ndx;
	tess.indexes[ tess.numIndexes + 1 ] = ndx + 1;
	tess.indexes[ tess.numIndexes + 2 ] = ndx + 3;

	tess.indexes[ tess.numIndexes + 3 ] = ndx + 3;
	tess.indexes[ tess.numIndexes + 4 ] = ndx + 1;
	tess.indexes[ tess.numIndexes + 5 ] = ndx + 2;

	tess.xyz[ndx][0] = origin[0] + left[0] + up[0];
	tess.xyz[ndx][1] = origin[1] + left[1] + up[1];
	tess.xyz[ndx][2] = origin[2] + left[2] + up[2];

	tess.xyz[ndx+1][0] = origin[0] - left[0] + up[0];
	tess.xyz[ndx+1][1] = origin[1] - left[1] + up[1];
	tess.xyz[ndx+1][2] = origin[2] - left[2] + up[2];

	tess.xyz[ndx+2][0] = origin[0] - left[0] - up[0];
	tess.xyz[ndx+2][1] = origin[1] - left[1] - up[1];
	tess.xyz[ndx+2][2] = origin[2] - left[2] - up[2];

	tess.xyz[ndx+3][0] = origin[0] + left[0] - up[0];
	tess.xyz[ndx+3][1] = origin[1] + left[1] - up[1];
	tess.xyz[ndx+3][2] = origin[2] + left[2] - up[2];


	// constant normal all the way around
	VectorSubtract( vec3_origin, backEnd.viewParms.or.axis[0], normal );

	R_VaoPackNormal(iNormal, normal);

	VectorCopy4(iNormal, tess.normal[ndx]);
	VectorCopy4(iNormal, tess.normal[ndx + 1]);
	VectorCopy4(iNormal, tess.normal[ndx + 2]);
	VectorCopy4(iNormal, tess.normal[ndx + 3]);

	// standard square texture coordinates
	VectorSet2(tess.texCoords[ndx], s1, t1);
	VectorSet2(tess.lightCoords[ndx], s1, t1);

	VectorSet2(tess.texCoords[ndx+1], s2, t1);
	VectorSet2(tess.lightCoords[ndx+1], s2, t1);

	VectorSet2(tess.texCoords[ndx+2], s2, t2);
	VectorSet2(tess.lightCoords[ndx+2], s2, t2);

	VectorSet2(tess.texCoords[ndx+3], s1, t2);
	VectorSet2(tess.lightCoords[ndx+3], s1, t2);

	// constant color all the way around
	// should this be identity and let the shader specify from entity?

	R_VaoPackColor(iColor, color);

	VectorCopy4(iColor, tess.color[ndx]);
	VectorCopy4(iColor, tess.color[ndx + 1]);
	VectorCopy4(iColor, tess.color[ndx + 2]);
	VectorCopy4(iColor, tess.color[ndx + 3]);

	tess.numVertexes += 4;
	tess.numIndexes += 6;
}

/*
==============
RB_AddQuadStamp
==============
*/
void RB_AddQuadStamp( vec3_t origin, vec3_t left, vec3_t up, float color[4] ) {
	RB_AddQuadStampExt( origin, left, up, color, 0, 0, 1, 1 );
}


/*
==============
RB_InstantQuad

based on Tess_InstantQuad from xreal
==============
*/
void RB_InstantQuad2(vec4_t quadVerts[4], vec2_t texCoords[4])
{
	GLimp_LogComment("--- RB_InstantQuad2 ---\n");

	tess.numVertexes = 0;
	tess.numIndexes = 0;
	tess.firstIndex = 0;

	VectorCopy4(quadVerts[0], tess.xyz[tess.numVertexes]);
	VectorCopy2(texCoords[0], tess.texCoords[tess.numVertexes]);
	tess.numVertexes++;

	VectorCopy4(quadVerts[1], tess.xyz[tess.numVertexes]);
	VectorCopy2(texCoords[1], tess.texCoords[tess.numVertexes]);
	tess.numVertexes++;

	VectorCopy4(quadVerts[2], tess.xyz[tess.numVertexes]);
	VectorCopy2(texCoords[2], tess.texCoords[tess.numVertexes]);
	tess.numVertexes++;

	VectorCopy4(quadVerts[3], tess.xyz[tess.numVertexes]);
	VectorCopy2(texCoords[3], tess.texCoords[tess.numVertexes]);
	tess.numVertexes++;

	tess.indexes[tess.numIndexes++] = 0;
	tess.indexes[tess.numIndexes++] = 1;
	tess.indexes[tess.numIndexes++] = 2;
	tess.indexes[tess.numIndexes++] = 0;
	tess.indexes[tess.numIndexes++] = 2;
	tess.indexes[tess.numIndexes++] = 3;

	RB_UpdateTessVao(ATTR_POSITION | ATTR_TEXCOORD);

	R_DrawElements(tess.numIndexes, tess.firstIndex);

	tess.numIndexes = 0;
	tess.numVertexes = 0;
	tess.firstIndex = 0;
}


void RB_InstantQuad(vec4_t quadVerts[4])
{
	vec2_t texCoords[4];

	VectorSet2(texCoords[0], 0.0f, 0.0f);
	VectorSet2(texCoords[1], 1.0f, 0.0f);
	VectorSet2(texCoords[2], 1.0f, 1.0f);
	VectorSet2(texCoords[3], 0.0f, 1.0f);

	GLSL_BindProgram(&tr.textureColorShader);
	
	GLSL_SetUniformMat4(&tr.textureColorShader, UNIFORM_MODELVIEWPROJECTIONMATRIX, glState.modelviewProjection);
	GLSL_SetUniformVec4(&tr.textureColorShader, UNIFORM_COLOR, colorWhite);

	RB_InstantQuad2(quadVerts, texCoords);
}


/*
==============
RB_SurfaceSprite
==============
*/
static void RB_SurfaceSprite( void ) {
	vec3_t		left, up;
	float		radius;
	float			colors[4];
	trRefEntity_t	*ent = backEnd.currentEntity;

	// calculate the xyz locations for the four corners
	radius = ent->e.radius;
	if ( ent->e.rotation == 0 ) {
		VectorScale( backEnd.viewParms.or.axis[1], radius, left );
		VectorScale( backEnd.viewParms.or.axis[2], radius, up );
	} else {
		float	s, c;
		float	ang;
		
		ang = M_PI * ent->e.rotation / 180;
		s = sin( ang );
		c = cos( ang );

		VectorScale( backEnd.viewParms.or.axis[1], c * radius, left );
		VectorMA( left, -s * radius, backEnd.viewParms.or.axis[2], left );

		VectorScale( backEnd.viewParms.or.axis[2], c * radius, up );
		VectorMA( up, s * radius, backEnd.viewParms.or.axis[1], up );
	}
	if ( backEnd.viewParms.isMirror ) {
		VectorSubtract( vec3_origin, left, left );
	}

	VectorScale4(ent->e.shaderRGBA, 1.0f / 255.0f, colors);

	RB_AddQuadStamp( ent->e.origin, left, up, colors );
}


/*
=============
RB_SurfacePolychain
=============
*/
static void RB_SurfacePolychain( srfPoly_t *p ) {
	int		i;
	int		numv;

	RB_CheckVao(tess.vao);

	RB_CHECKOVERFLOW( p->numVerts, 3*(p->numVerts - 2) );

	// fan triangles into the tess array
	numv = tess.numVertexes;
	for ( i = 0; i < p->numVerts; i++ ) {
		VectorCopy( p->verts[i].xyz, tess.xyz[numv] );
		tess.texCoords[numv][0] = p->verts[i].st[0];
		tess.texCoords[numv][1] = p->verts[i].st[1];
		tess.color[numv][0] = (int)p->verts[i].modulate[0] * 257;
		tess.color[numv][1] = (int)p->verts[i].modulate[1] * 257;
		tess.color[numv][2] = (int)p->verts[i].modulate[2] * 257;
		tess.color[numv][3] = (int)p->verts[i].modulate[3] * 257;

		numv++;
	}

	// generate fan indexes into the tess array
	for ( i = 0; i < p->numVerts-2; i++ ) {
		tess.indexes[tess.numIndexes + 0] = tess.numVertexes;
		tess.indexes[tess.numIndexes + 1] = tess.numVertexes + i + 1;
		tess.indexes[tess.numIndexes + 2] = tess.numVertexes + i + 2;
		tess.numIndexes += 3;
	}

	tess.numVertexes = numv;
}

static void RB_SurfaceVertsAndIndexes( int numVerts, srfVert_t *verts, int numIndexes, glIndex_t *indexes, int dlightBits, int pshadowBits)
{
	int             i;
	glIndex_t      *inIndex;
	srfVert_t      *dv;
	float          *xyz, *texCoords, *lightCoords;
	int16_t        *lightdir;
	int16_t        *normal;
	int16_t        *tangent;
	glIndex_t      *outIndex;
	uint16_t       *color;

	RB_CheckVao(tess.vao);

	RB_CHECKOVERFLOW( numVerts, numIndexes );

	inIndex = indexes;
	outIndex = &tess.indexes[ tess.numIndexes ];
	for ( i = 0 ; i < numIndexes ; i++ ) {
		*outIndex++ = tess.numVertexes + *inIndex++;
	}
	tess.numIndexes += numIndexes;

	if ( tess.shader->vertexAttribs & ATTR_POSITION )
	{
		dv = verts;
		xyz = tess.xyz[ tess.numVertexes ];
		for ( i = 0 ; i < numVerts ; i++, dv++, xyz+=4 )
			VectorCopy(dv->xyz, xyz);
	}

	if ( tess.shader->vertexAttribs & ATTR_NORMAL )
	{
		dv = verts;
		normal = tess.normal[ tess.numVertexes ];
		for ( i = 0 ; i < numVerts ; i++, dv++, normal+=4 )
			VectorCopy4(dv->normal, normal);
	}

	if ( tess.shader->vertexAttribs & ATTR_TANGENT )
	{
		dv = verts;
		tangent = tess.tangent[ tess.numVertexes ];
		for ( i = 0 ; i < numVerts ; i++, dv++, tangent+=4 )
			VectorCopy4(dv->tangent, tangent);
	}

	if ( tess.shader->vertexAttribs & ATTR_TEXCOORD )
	{
		dv = verts;
		texCoords = tess.texCoords[tess.numVertexes];
		for ( i = 0 ; i < numVerts ; i++, dv++, texCoords+=2 )
			VectorCopy2(dv->st, texCoords);
	}

	if ( tess.shader->vertexAttribs & ATTR_LIGHTCOORD )
	{
		dv = verts;
		lightCoords = tess.lightCoords[ tess.numVertexes ];
		for ( i = 0 ; i < numVerts ; i++, dv++, lightCoords+=2 )
			VectorCopy2(dv->lightmap, lightCoords);
	}

	if ( tess.shader->vertexAttribs & ATTR_COLOR )
	{
		dv = verts;
		color = tess.color[ tess.numVertexes ];
		for ( i = 0 ; i < numVerts ; i++, dv++, color+=4 )
			VectorCopy4(dv->color, color);
	}

	if ( tess.shader->vertexAttribs & ATTR_LIGHTDIRECTION )
	{
		dv = verts;
		lightdir = tess.lightdir[ tess.numVertexes ];
		for ( i = 0 ; i < numVerts ; i++, dv++, lightdir+=4 )
			VectorCopy4(dv->lightdir, lightdir);
	}

#if 0  // nothing even uses vertex dlightbits
	for ( i = 0 ; i < numVerts ; i++ ) {
		tess.vertexDlightBits[ tess.numVertexes + i ] = dlightBits;
	}
#endif

	tess.dlightBits |= dlightBits;
	tess.pshadowBits |= pshadowBits;

	tess.numVertexes += numVerts;
}

static qboolean RB_SurfaceVaoCached(int numVerts, srfVert_t *verts, int numIndexes, glIndex_t *indexes, int dlightBits, int pshadowBits)
{
	qboolean recycleVertexBuffer = qfalse;
	qboolean recycleIndexBuffer = qfalse;
	qboolean endSurface = qfalse;

	if (!r_vaoCache->integer)
		return qfalse;

	if (!(!ShaderRequiresCPUDeforms(tess.shader) && !tess.shader->isSky && !tess.shader->isPortal))
		return qfalse;

	if (!numIndexes || !numVerts)
		return qfalse;

	VaoCache_BindVao();

	tess.dlightBits |= dlightBits;
	tess.pshadowBits |= pshadowBits;

	VaoCache_CheckAdd(&endSurface, &recycleVertexBuffer, &recycleIndexBuffer, numVerts, numIndexes);

	if (endSurface)
	{
		RB_EndSurface();
		RB_BeginSurface(tess.shader, tess.fogNum, tess.cubemapIndex);
	}

	if (recycleVertexBuffer)
		VaoCache_RecycleVertexBuffer();

	if (recycleIndexBuffer)
		VaoCache_RecycleIndexBuffer();

	if (!tess.numVertexes)
		VaoCache_InitQueue();

	VaoCache_AddSurface(verts, numVerts, indexes, numIndexes);

	tess.numIndexes += numIndexes;
	tess.numVertexes += numVerts;
	tess.useInternalVao = qfalse;
	tess.useCacheVao = qtrue;

	return qtrue;
}


/*
=============
RB_SurfaceTriangles
=============
*/
static void RB_SurfaceTriangles( srfBspSurface_t *srf ) {
	if (RB_SurfaceVaoCached(srf->numVerts, srf->verts, srf->numIndexes,
		srf->indexes, srf->dlightBits, srf->pshadowBits))
	{
		return;
	}

	RB_SurfaceVertsAndIndexes(srf->numVerts, srf->verts, srf->numIndexes,
			srf->indexes, srf->dlightBits, srf->pshadowBits);
}



/*
==============
RB_SurfaceBeam
==============
*/
static void RB_SurfaceBeam( void )
{
#define NUM_BEAM_SEGS 6
	refEntity_t *e;
	shaderProgram_t *sp = &tr.textureColorShader;
	int	i;
	vec3_t perpvec;
	vec3_t direction, normalized_direction;
	vec3_t	start_points[NUM_BEAM_SEGS], end_points[NUM_BEAM_SEGS];
	vec3_t oldorigin, origin;

	e = &backEnd.currentEntity->e;

	oldorigin[0] = e->oldorigin[0];
	oldorigin[1] = e->oldorigin[1];
	oldorigin[2] = e->oldorigin[2];

	origin[0] = e->origin[0];
	origin[1] = e->origin[1];
	origin[2] = e->origin[2];

	normalized_direction[0] = direction[0] = oldorigin[0] - origin[0];
	normalized_direction[1] = direction[1] = oldorigin[1] - origin[1];
	normalized_direction[2] = direction[2] = oldorigin[2] - origin[2];

	if ( VectorNormalize( normalized_direction ) == 0 )
		return;

	PerpendicularVector( perpvec, normalized_direction );

	VectorScale( perpvec, 4, perpvec );

	for ( i = 0; i < NUM_BEAM_SEGS ; i++ )
	{
		RotatePointAroundVector( start_points[i], normalized_direction, perpvec, (360.0/NUM_BEAM_SEGS)*i );
//		VectorAdd( start_points[i], origin, start_points[i] );
		VectorAdd( start_points[i], direction, end_points[i] );
	}

	GL_BindToTMU( tr.whiteImage, TB_COLORMAP );

	GL_State( GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE );

	// FIXME: Quake3 doesn't use this, so I never tested it
	tess.numVertexes = 0;
	tess.numIndexes = 0;
	tess.firstIndex = 0;

	for ( i = 0; i <= NUM_BEAM_SEGS; i++ ) {
		VectorCopy(start_points[ i % NUM_BEAM_SEGS ], tess.xyz[tess.numVertexes++]);
		VectorCopy(end_points  [ i % NUM_BEAM_SEGS ], tess.xyz[tess.numVertexes++]);
	}

	for ( i = 0; i < NUM_BEAM_SEGS; i++ ) {
		tess.indexes[tess.numIndexes++] =       i      * 2;
		tess.indexes[tess.numIndexes++] =      (i + 1) * 2;
		tess.indexes[tess.numIndexes++] = 1  +  i      * 2;

		tess.indexes[tess.numIndexes++] = 1  +  i      * 2;
		tess.indexes[tess.numIndexes++] =      (i + 1) * 2;
		tess.indexes[tess.numIndexes++] = 1  + (i + 1) * 2;
	}

	// FIXME: A lot of this can probably be removed for speed, and refactored into a more convenient function
	RB_UpdateTessVao(ATTR_POSITION);
	
	GLSL_BindProgram(sp);
		
	GLSL_SetUniformMat4(sp, UNIFORM_MODELVIEWPROJECTIONMATRIX, glState.modelviewProjection);
					
	GLSL_SetUniformVec4(sp, UNIFORM_COLOR, colorRed);

	GLSL_SetUniformInt(sp, UNIFORM_ALPHATEST, 0);

	R_DrawElements(tess.numIndexes, tess.firstIndex);

	tess.numIndexes = 0;
	tess.numVertexes = 0;
	tess.firstIndex = 0;
}

//================================================================================

static void DoRailCore( const vec3_t start, const vec3_t end, const vec3_t up, float len, float spanWidth )
{
	float		spanWidth2;
	int			vbase;
	float		t = len / 256.0f;

	RB_CheckVao(tess.vao);

	RB_CHECKOVERFLOW( 4, 6 );

	vbase = tess.numVertexes;

	spanWidth2 = -spanWidth;

	// FIXME: use quad stamp?
	VectorMA( start, spanWidth, up, tess.xyz[tess.numVertexes] );
	tess.texCoords[tess.numVertexes][0] = 0;
	tess.texCoords[tess.numVertexes][1] = 0;
	tess.color[tess.numVertexes][0] = backEnd.currentEntity->e.shaderRGBA[0] * 0.25f * 257.0f;
	tess.color[tess.numVertexes][1] = backEnd.currentEntity->e.shaderRGBA[1] * 0.25f * 257.0f;
	tess.color[tess.numVertexes][2] = backEnd.currentEntity->e.shaderRGBA[2] * 0.25f * 257.0f;
	tess.numVertexes++;

	VectorMA( start, spanWidth2, up, tess.xyz[tess.numVertexes] );
	tess.texCoords[tess.numVertexes][0] = 0;
	tess.texCoords[tess.numVertexes][1] = 1;
	tess.color[tess.numVertexes][0] = backEnd.currentEntity->e.shaderRGBA[0] * 257;
	tess.color[tess.numVertexes][1] = backEnd.currentEntity->e.shaderRGBA[1] * 257;
	tess.color[tess.numVertexes][2] = backEnd.currentEntity->e.shaderRGBA[2] * 257;
	tess.numVertexes++;

	VectorMA( end, spanWidth, up, tess.xyz[tess.numVertexes] );

	tess.texCoords[tess.numVertexes][0] = t;
	tess.texCoords[tess.numVertexes][1] = 0;
	tess.color[tess.numVertexes][0] = backEnd.currentEntity->e.shaderRGBA[0] * 257;
	tess.color[tess.numVertexes][1] = backEnd.currentEntity->e.shaderRGBA[1] * 257;
	tess.color[tess.numVertexes][2] = backEnd.currentEntity->e.shaderRGBA[2] * 257;
	tess.numVertexes++;

	VectorMA( end, spanWidth2, up, tess.xyz[tess.numVertexes] );
	tess.texCoords[tess.numVertexes][0] = t;
	tess.texCoords[tess.numVertexes][1] = 1;
	tess.color[tess.numVertexes][0] = backEnd.currentEntity->e.shaderRGBA[0] * 257;
	tess.color[tess.numVertexes][1] = backEnd.currentEntity->e.shaderRGBA[1] * 257;
	tess.color[tess.numVertexes][2] = backEnd.currentEntity->e.shaderRGBA[2] * 257;
	tess.numVertexes++;

	tess.indexes[tess.numIndexes++] = vbase;
	tess.indexes[tess.numIndexes++] = vbase + 1;
	tess.indexes[tess.numIndexes++] = vbase + 2;

	tess.indexes[tess.numIndexes++] = vbase + 2;
	tess.indexes[tess.numIndexes++] = vbase + 1;
	tess.indexes[tess.numIndexes++] = vbase + 3;
}

static void DoRailDiscs( int numSegs, const vec3_t start, const vec3_t dir, const vec3_t right, const vec3_t up )
{
	int i;
	vec3_t	pos[4];
	vec3_t	v;
	int		spanWidth = r_railWidth->integer;
	float c, s;
	float		scale;

	if ( numSegs > 1 )
		numSegs--;
	if ( !numSegs )
		return;

	scale = 0.25;

	for ( i = 0; i < 4; i++ )
	{
		c = cos( DEG2RAD( 45 + i * 90 ) );
		s = sin( DEG2RAD( 45 + i * 90 ) );
		v[0] = ( right[0] * c + up[0] * s ) * scale * spanWidth;
		v[1] = ( right[1] * c + up[1] * s ) * scale * spanWidth;
		v[2] = ( right[2] * c + up[2] * s ) * scale * spanWidth;
		VectorAdd( start, v, pos[i] );

		if ( numSegs > 1 )
		{
			// offset by 1 segment if we're doing a long distance shot
			VectorAdd( pos[i], dir, pos[i] );
		}
	}

	RB_CheckVao(tess.vao);

	for ( i = 0; i < numSegs; i++ )
	{
		int j;

		RB_CHECKOVERFLOW( 4, 6 );

		for ( j = 0; j < 4; j++ )
		{
			VectorCopy( pos[j], tess.xyz[tess.numVertexes] );
			tess.texCoords[tess.numVertexes][0] = (j < 2);
			tess.texCoords[tess.numVertexes][1] = (j && j != 3);
			tess.color[tess.numVertexes][0] = backEnd.currentEntity->e.shaderRGBA[0] * 257;
			tess.color[tess.numVertexes][1] = backEnd.currentEntity->e.shaderRGBA[1] * 257;
			tess.color[tess.numVertexes][2] = backEnd.currentEntity->e.shaderRGBA[2] * 257;
			tess.numVertexes++;

			VectorAdd( pos[j], dir, pos[j] );
		}

		tess.indexes[tess.numIndexes++] = tess.numVertexes - 4 + 0;
		tess.indexes[tess.numIndexes++] = tess.numVertexes - 4 + 1;
		tess.indexes[tess.numIndexes++] = tess.numVertexes - 4 + 3;
		tess.indexes[tess.numIndexes++] = tess.numVertexes - 4 + 3;
		tess.indexes[tess.numIndexes++] = tess.numVertexes - 4 + 1;
		tess.indexes[tess.numIndexes++] = tess.numVertexes - 4 + 2;
	}
}

/*
** RB_SurfaceRailRinges
*/
static void RB_SurfaceRailRings( void ) {
	refEntity_t *e;
	int			numSegs;
	int			len;
	vec3_t		vec;
	vec3_t		right, up;
	vec3_t		start, end;

	e = &backEnd.currentEntity->e;

	VectorCopy( e->oldorigin, start );
	VectorCopy( e->origin, end );

	// compute variables
	VectorSubtract( end, start, vec );
	len = VectorNormalize( vec );
	MakeNormalVectors( vec, right, up );
	numSegs = ( len ) / r_railSegmentLength->value;
	if ( numSegs <= 0 ) {
		numSegs = 1;
	}

	VectorScale( vec, r_railSegmentLength->value, vec );

	DoRailDiscs( numSegs, start, vec, right, up );
}

/*
** RB_SurfaceRailCore
*/
static void RB_SurfaceRailCore( void ) {
	refEntity_t *e;
	int			len;
	vec3_t		right;
	vec3_t		vec;
	vec3_t		start, end;
	vec3_t		v1, v2;

	e = &backEnd.currentEntity->e;

	VectorCopy( e->oldorigin, start );
	VectorCopy( e->origin, end );

	VectorSubtract( end, start, vec );
	len = VectorNormalize( vec );

	// compute side vector
	VectorSubtract( start, backEnd.viewParms.or.origin, v1 );
	VectorNormalize( v1 );
	VectorSubtract( end, backEnd.viewParms.or.origin, v2 );
	VectorNormalize( v2 );
	CrossProduct( v1, v2, right );
	VectorNormalize( right );

	DoRailCore( start, end, right, len, r_railCoreWidth->integer );
}

/*
** RB_SurfaceLightningBolt
*/
static void RB_SurfaceLightningBolt( void ) {
	refEntity_t *e;
	int			len;
	vec3_t		right;
	vec3_t		vec;
	vec3_t		start, end;
	vec3_t		v1, v2;
	int			i;

	e = &backEnd.currentEntity->e;

	VectorCopy( e->oldorigin, end );
	VectorCopy( e->origin, start );

	// compute variables
	VectorSubtract( end, start, vec );
	len = VectorNormalize( vec );

	// compute side vector
	VectorSubtract( start, backEnd.viewParms.or.origin, v1 );
	VectorNormalize( v1 );
	VectorSubtract( end, backEnd.viewParms.or.origin, v2 );
	VectorNormalize( v2 );
	CrossProduct( v1, v2, right );
	VectorNormalize( right );

	for ( i = 0 ; i < 4 ; i++ ) {
		vec3_t	temp;

		DoRailCore( start, end, right, len, 8 );
		RotatePointAroundVector( temp, vec, right, 45 );
		VectorCopy( temp, right );
	}
}


static void LerpMeshVertexes(mdvSurface_t *surf, float backlerp)
{
	float *outXyz;
	int16_t *outNormal, *outTangent;
	mdvVertex_t *newVerts;
	int		vertNum;

	newVerts = surf->verts + backEnd.currentEntity->e.frame * surf->numVerts;

	outXyz =     tess.xyz[tess.numVertexes];
	outNormal =  tess.normal[tess.numVertexes];
	outTangent = tess.tangent[tess.numVertexes];

	if (backlerp == 0)
	{
		//
		// just copy the vertexes
		//

		for (vertNum=0 ; vertNum < surf->numVerts ; vertNum++)
		{
			VectorCopy(newVerts->xyz,    outXyz);
			VectorCopy4(newVerts->normal, outNormal);
			VectorCopy4(newVerts->tangent, outTangent);

			newVerts++;
			outXyz += 4;
			outNormal += 4;
			outTangent += 4;
		}
	}
	else
	{
		//
		// interpolate and copy the vertex and normal
		//

		mdvVertex_t *oldVerts;

		oldVerts = surf->verts + backEnd.currentEntity->e.oldframe * surf->numVerts;

		for (vertNum=0 ; vertNum < surf->numVerts ; vertNum++)
		{
			VectorLerp(newVerts->xyz,    oldVerts->xyz,    backlerp, outXyz);

			outNormal[0] = (int16_t)(newVerts->normal[0] * (1.0f - backlerp) + oldVerts->normal[0] * backlerp);
			outNormal[1] = (int16_t)(newVerts->normal[1] * (1.0f - backlerp) + oldVerts->normal[1] * backlerp);
			outNormal[2] = (int16_t)(newVerts->normal[2] * (1.0f - backlerp) + oldVerts->normal[2] * backlerp);
			outNormal[3] = 0;

			outTangent[0] = (int16_t)(newVerts->tangent[0] * (1.0f - backlerp) + oldVerts->tangent[0] * backlerp);
			outTangent[1] = (int16_t)(newVerts->tangent[1] * (1.0f - backlerp) + oldVerts->tangent[1] * backlerp);
			outTangent[2] = (int16_t)(newVerts->tangent[2] * (1.0f - backlerp) + oldVerts->tangent[2] * backlerp);
			outTangent[3] = newVerts->tangent[3];

			newVerts++;
			oldVerts++;
			outXyz += 4;
			outNormal += 4;
			outTangent += 4;
		}
	}

}


/*
=============
RB_SurfaceMesh
=============
*/
static void RB_SurfaceMesh(mdvSurface_t *surface) {
	int				j;
	float			backlerp;
	mdvSt_t			*texCoords;
	int				Bob, Doug;
	int				numVerts;

	if (  backEnd.currentEntity->e.oldframe == backEnd.currentEntity->e.frame ) {
		backlerp = 0;
	} else  {
		backlerp = backEnd.currentEntity->e.backlerp;
	}

	RB_CheckVao(tess.vao);

	RB_CHECKOVERFLOW( surface->numVerts, surface->numIndexes );

	LerpMeshVertexes (surface, backlerp);

	Bob = tess.numIndexes;
	Doug = tess.numVertexes;
	for (j = 0 ; j < surface->numIndexes ; j++) {
		tess.indexes[Bob + j] = Doug + surface->indexes[j];
	}
	tess.numIndexes += surface->numIndexes;

	texCoords = surface->st;

	numVerts = surface->numVerts;
	for ( j = 0; j < numVerts; j++ ) {
		tess.texCoords[Doug + j][0] = texCoords[j].st[0];
		tess.texCoords[Doug + j][1] = texCoords[j].st[1];
		// FIXME: fill in lightmapST for completeness?
	}

	tess.numVertexes += surface->numVerts;

}


/*
==============
RB_SurfaceFace
==============
*/
static void RB_SurfaceFace( srfBspSurface_t *srf ) {
	if (RB_SurfaceVaoCached(srf->numVerts, srf->verts, srf->numIndexes,
		srf->indexes, srf->dlightBits, srf->pshadowBits))
	{
		return;
	}

	RB_SurfaceVertsAndIndexes(srf->numVerts, srf->verts, srf->numIndexes,
			srf->indexes, srf->dlightBits, srf->pshadowBits);
}


static float	LodErrorForVolume( vec3_t local, float radius ) {
	vec3_t		world;
	float		d;

	// never let it go negative
	if ( r_lodCurveError->value < 0 ) {
		return 0;
	}

	world[0] = local[0] * backEnd.or.axis[0][0] + local[1] * backEnd.or.axis[1][0] + 
		local[2] * backEnd.or.axis[2][0] + backEnd.or.origin[0];
	world[1] = local[0] * backEnd.or.axis[0][1] + local[1] * backEnd.or.axis[1][1] + 
		local[2] * backEnd.or.axis[2][1] + backEnd.or.origin[1];
	world[2] = local[0] * backEnd.or.axis[0][2] + local[1] * backEnd.or.axis[1][2] + 
		local[2] * backEnd.or.axis[2][2] + backEnd.or.origin[2];

	VectorSubtract( world, backEnd.viewParms.or.origin, world );
	d = DotProduct( world, backEnd.viewParms.or.axis[0] );

	if ( d < 0 ) {
		d = -d;
	}
	d -= radius;
	if ( d < 1 ) {
		d = 1;
	}

	return r_lodCurveError->value / d;
}

/*
=============
RB_SurfaceGrid

Just copy the grid of points and triangulate
=============
*/
static void RB_SurfaceGrid( srfBspSurface_t *srf ) {
	int		i, j;
	float	*xyz;
	float	*texCoords, *lightCoords;
	int16_t *normal;
	int16_t *tangent;
	uint16_t *color;
	int16_t *lightdir;
	srfVert_t	*dv;
	int		rows, irows, vrows;
	int		used;
	int		widthTable[MAX_GRID_SIZE];
	int		heightTable[MAX_GRID_SIZE];
	float	lodError;
	int		lodWidth, lodHeight;
	int		numVertexes;
	int		dlightBits;
	int     pshadowBits;
	//int		*vDlightBits;

	if (RB_SurfaceVaoCached(srf->numVerts, srf->verts, srf->numIndexes,
		srf->indexes, srf->dlightBits, srf->pshadowBits))
	{
		return;
	}

	RB_CheckVao(tess.vao);

	dlightBits = srf->dlightBits;
	tess.dlightBits |= dlightBits;

	pshadowBits = srf->pshadowBits;
	tess.pshadowBits |= pshadowBits;

	// determine the allowable discrepance
	lodError = LodErrorForVolume( srf->lodOrigin, srf->lodRadius );

	// determine which rows and columns of the subdivision
	// we are actually going to use
	widthTable[0] = 0;
	lodWidth = 1;
	for ( i = 1 ; i < srf->width-1 ; i++ ) {
		if ( srf->widthLodError[i] <= lodError ) {
			widthTable[lodWidth] = i;
			lodWidth++;
		}
	}
	widthTable[lodWidth] = srf->width-1;
	lodWidth++;

	heightTable[0] = 0;
	lodHeight = 1;
	for ( i = 1 ; i < srf->height-1 ; i++ ) {
		if ( srf->heightLodError[i] <= lodError ) {
			heightTable[lodHeight] = i;
			lodHeight++;
		}
	}
	heightTable[lodHeight] = srf->height-1;
	lodHeight++;


	// very large grids may have more points or indexes than can be fit
	// in the tess structure, so we may have to issue it in multiple passes

	used = 0;
	while ( used < lodHeight - 1 ) {
		// see how many rows of both verts and indexes we can add without overflowing
		do {
			vrows = ( SHADER_MAX_VERTEXES - tess.numVertexes ) / lodWidth;
			irows = ( SHADER_MAX_INDEXES - tess.numIndexes ) / ( lodWidth * 6 );

			// if we don't have enough space for at least one strip, flush the buffer
			if ( vrows < 2 || irows < 1 ) {
				RB_EndSurface();
				RB_BeginSurface(tess.shader, tess.fogNum, tess.cubemapIndex );
			} else {
				break;
			}
		} while ( 1 );
		
		rows = irows;
		if ( vrows < irows + 1 ) {
			rows = vrows - 1;
		}
		if ( used + rows > lodHeight ) {
			rows = lodHeight - used;
		}

		numVertexes = tess.numVertexes;

		xyz = tess.xyz[numVertexes];
		normal = tess.normal[numVertexes];
		tangent = tess.tangent[numVertexes];
		texCoords = tess.texCoords[numVertexes];
		lightCoords = tess.lightCoords[numVertexes];
		color = tess.color[numVertexes];
		lightdir = tess.lightdir[numVertexes];
		//vDlightBits = &tess.vertexDlightBits[numVertexes];

		for ( i = 0 ; i < rows ; i++ ) {
			for ( j = 0 ; j < lodWidth ; j++ ) {
				dv = srf->verts + heightTable[ used + i ] * srf->width
					+ widthTable[ j ];

				if ( tess.shader->vertexAttribs & ATTR_POSITION )
				{
					VectorCopy(dv->xyz, xyz);
					xyz += 4;
				}

				if ( tess.shader->vertexAttribs & ATTR_NORMAL )
				{
					VectorCopy4(dv->normal, normal);
					normal += 4;
				}

				if ( tess.shader->vertexAttribs & ATTR_TANGENT )
				{
					VectorCopy4(dv->tangent, tangent);
					tangent += 4;
				}

				if ( tess.shader->vertexAttribs & ATTR_TEXCOORD )
				{
					VectorCopy2(dv->st, texCoords);
					texCoords += 2;
				}

				if ( tess.shader->vertexAttribs & ATTR_LIGHTCOORD )
				{
					VectorCopy2(dv->lightmap, lightCoords);
					lightCoords += 2;
				}

				if ( tess.shader->vertexAttribs & ATTR_COLOR )
				{
					VectorCopy4(dv->color, color);
					color += 4;
				}

				if ( tess.shader->vertexAttribs & ATTR_LIGHTDIRECTION )
				{
					VectorCopy4(dv->lightdir, lightdir);
					lightdir += 4;
				}

				//*vDlightBits++ = dlightBits;
			}
		}


		// add the indexes
		{
			int		numIndexes;
			int		w, h;

			h = rows - 1;
			w = lodWidth - 1;
			numIndexes = tess.numIndexes;
			for (i = 0 ; i < h ; i++) {
				for (j = 0 ; j < w ; j++) {
					int		v1, v2, v3, v4;
			
					// vertex order to be reckognized as tristrips
					v1 = numVertexes + i*lodWidth + j + 1;
					v2 = v1 - 1;
					v3 = v2 + lodWidth;
					v4 = v3 + 1;

					tess.indexes[numIndexes] = v2;
					tess.indexes[numIndexes+1] = v3;
					tess.indexes[numIndexes+2] = v1;
					
					tess.indexes[numIndexes+3] = v1;
					tess.indexes[numIndexes+4] = v3;
					tess.indexes[numIndexes+5] = v4;
					numIndexes += 6;
				}
			}

			tess.numIndexes = numIndexes;
		}

		tess.numVertexes += rows * lodWidth;

		used += rows - 1;
	}
}


/*
===========================================================================

NULL MODEL

===========================================================================
*/

/*
===================
RB_SurfaceAxis

Draws x/y/z lines from the origin for orientation debugging
===================
*/
static void RB_SurfaceAxis( void ) {
	// FIXME: implement this
#if 0
	GL_BindToTMU( tr.whiteImage, TB_COLORMAP );
	GL_State( GLS_DEFAULT );
	qglLineWidth( 3 );
	qglBegin( GL_LINES );
	qglColor3f( 1,0,0 );
	qglVertex3f( 0,0,0 );
	qglVertex3f( 16,0,0 );
	qglColor3f( 0,1,0 );
	qglVertex3f( 0,0,0 );
	qglVertex3f( 0,16,0 );
	qglColor3f( 0,0,1 );
	qglVertex3f( 0,0,0 );
	qglVertex3f( 0,0,16 );
	qglEnd();
	qglLineWidth( 1 );
#endif
}

//===========================================================================

/*
====================
RB_SurfaceEntity

Entities that have a single procedurally generated surface
====================
*/
static void RB_SurfaceEntity( surfaceType_t *surfType ) {
	switch( backEnd.currentEntity->e.reType ) {
	case RT_SPRITE:
		RB_SurfaceSprite();
		break;
	case RT_BEAM:
		RB_SurfaceBeam();
		break;
	case RT_RAIL_CORE:
		RB_SurfaceRailCore();
		break;
	case RT_RAIL_RINGS:
		RB_SurfaceRailRings();
		break;
	case RT_LIGHTNING:
		RB_SurfaceLightningBolt();
		break;
	default:
		RB_SurfaceAxis();
		break;
	}
}

static void RB_SurfaceBad( surfaceType_t *surfType ) {
	ri.Printf( PRINT_ALL, "Bad surface tesselated.\n" );
}

static void RB_SurfaceFlare(srfFlare_t *surf)
{
	if (r_flares->integer)
		RB_AddFlare(surf, tess.fogNum, surf->origin, surf->color, surf->normal);
}

void RB_SurfaceVaoMdvMesh(srfVaoMdvMesh_t * surface)
{
	//mdvModel_t     *mdvModel;
	//mdvSurface_t   *mdvSurface;
	refEntity_t    *refEnt;

	GLimp_LogComment("--- RB_SurfaceVaoMdvMesh ---\n");

	if (ShaderRequiresCPUDeforms(tess.shader))
	{
		RB_SurfaceMesh(surface->mdvSurface);
		return;
	}

	if(!surface->vao)
		return;

	//RB_CheckVao(surface->vao);
	RB_EndSurface();
	RB_BeginSurface(tess.shader, tess.fogNum, tess.cubemapIndex);

	R_BindVao(surface->vao);

	tess.useInternalVao = qfalse;

	tess.numIndexes = surface->numIndexes;
	tess.numVertexes = surface->numVerts;

	//mdvModel = surface->mdvModel;
	//mdvSurface = surface->mdvSurface;

	refEnt = &backEnd.currentEntity->e;

	glState.vertexAttribsInterpolation = (refEnt->oldframe == refEnt->frame) ? 0.0f : refEnt->backlerp;

	if (surface->mdvModel->numFrames > 1)
	{
		int frameOffset, attribIndex;
		vaoAttrib_t *vAtb;

		glState.vertexAnimation = qtrue;

		if (glRefConfig.vertexArrayObject)
		{
			qglBindBuffer(GL_ARRAY_BUFFER, surface->vao->vertexesVBO);
		}

		frameOffset    = refEnt->frame * surface->vao->frameSize;

		attribIndex = ATTR_INDEX_POSITION;
		vAtb = &surface->vao->attribs[attribIndex];
		qglVertexAttribPointer(attribIndex, vAtb->count, vAtb->type, vAtb->normalized, vAtb->stride, BUFFER_OFFSET(vAtb->offset + frameOffset));

		attribIndex = ATTR_INDEX_NORMAL;
		vAtb = &surface->vao->attribs[attribIndex];
		qglVertexAttribPointer(attribIndex, vAtb->count, vAtb->type, vAtb->normalized, vAtb->stride, BUFFER_OFFSET(vAtb->offset + frameOffset));

		attribIndex = ATTR_INDEX_TANGENT;
		vAtb = &surface->vao->attribs[attribIndex];
		qglVertexAttribPointer(attribIndex, vAtb->count, vAtb->type, vAtb->normalized, vAtb->stride, BUFFER_OFFSET(vAtb->offset + frameOffset));

		frameOffset = refEnt->oldframe * surface->vao->frameSize;

		attribIndex = ATTR_INDEX_POSITION2;
		vAtb = &surface->vao->attribs[attribIndex];
		qglVertexAttribPointer(attribIndex, vAtb->count, vAtb->type, vAtb->normalized, vAtb->stride, BUFFER_OFFSET(vAtb->offset + frameOffset));

		attribIndex = ATTR_INDEX_NORMAL2;
		vAtb = &surface->vao->attribs[attribIndex];
		qglVertexAttribPointer(attribIndex, vAtb->count, vAtb->type, vAtb->normalized, vAtb->stride, BUFFER_OFFSET(vAtb->offset + frameOffset));

		attribIndex = ATTR_INDEX_TANGENT2;
		vAtb = &surface->vao->attribs[attribIndex];
		qglVertexAttribPointer(attribIndex, vAtb->count, vAtb->type, vAtb->normalized, vAtb->stride, BUFFER_OFFSET(vAtb->offset + frameOffset));


		if (!glRefConfig.vertexArrayObject)
		{
			attribIndex = ATTR_INDEX_TEXCOORD;
			vAtb = &surface->vao->attribs[attribIndex];
			qglVertexAttribPointer(attribIndex, vAtb->count, vAtb->type, vAtb->normalized, vAtb->stride, BUFFER_OFFSET(vAtb->offset));
		}
	}

	RB_EndSurface();

	// So we don't lerp surfaces that shouldn't be lerped
	glState.vertexAnimation = qfalse;
}

static void RB_SurfaceSkip( void *surf ) {
}

//
// OPENMOHAA-specific stuff
//=========================

/*
=============
RB_SurfaceMarkFragment
=============
*/
void RB_SurfaceMarkFragment(srfMarkFragment_t* p) {
	int i;
	int numv;

	RB_CHECKOVERFLOW( p->numVerts, 3*(p->numVerts - 2) );

	if (p->iIndex <= 0 || R_TerrainHeightForPoly(&tr.world->terraPatches[p->iIndex - 1], p->verts, p->numVerts))
	{
		// FIXME: from here on out, it's mostly the same code as in RB_SurfacePolychain,
		// common part could be extracted into an inline func

		// fan triangles into the tess array
		numv = tess.numVertexes;
		for ( i = 0; i < p->numVerts; i++ )
		{
			VectorCopy( p->verts[i].xyz, tess.xyz[numv] );
            tess.texCoords[numv][0] = p->verts[i].st[0];
            tess.texCoords[numv][1] = p->verts[i].st[1];
			tess.color[numv][0] = p->verts[i].modulate[0] * 65535 / 255;
            tess.color[numv][1] = p->verts[i].modulate[1] * 65535 / 255;
            tess.color[numv][2] = p->verts[i].modulate[2] * 65535 / 255;
            tess.color[numv][3] = p->verts[i].modulate[3] * 65535 / 255;

			numv++;
		}

		// generate fan indexes into the tess array
		for ( i = 0; i < p->numVerts - 2; i++ ) {
			tess.indexes[tess.numIndexes + 0] = tess.numVertexes;
			tess.indexes[tess.numIndexes + 1] = tess.numVertexes + i + 1;
			tess.indexes[tess.numIndexes + 2] = tess.numVertexes + i + 2;
			tess.numIndexes += 3;
		}

		tess.numVertexes = numv;
	}
}

/*
=============
HZM coop [bug-2905] LOD-INDEPENDENT terrain shading inputs.

User: "the ground textures ... go from looking like the generic ground texture to a ground that has more depth
... it basically constantly shifts between the two as you walk". The terrain LOD re-tessellates as the player
moves, and two per-pixel lighting inputs were derived from whatever the tessellation happened to be:
  - the patch LIGHT DIRECTION was sampled at g_pVert[iVertHead] - and R_AllocateVert inserts new vertices at
    the HEAD of that list, so the sample point was whichever vertex split most recently. R_LightDirForPoint
    traces from it to the SUN: sky -> the sun's grazing direction (relief shows), anything in the way - a tree,
    a wall, or the trace starting on the ground it sits on - -> straight up (flat). Each split flipped it.
  - the smooth NORMALS were accumulated from the current LOD triangles, so a coarse mesh shaded flatter than a
    fine one over the same ground.
Both now come from data the LOD never touches: the light direction from the patch centre, lifted clear of the
ground; the normals from the 9x9 heightmap (64u grid, z = h * 2 + z0), stepping into the edge neighbour so the
normals match across patch seams.
=============
*/
static const cTerraPatchUnpacked_t *R_TerraEdgeNeighbour(const cTerraPatchUnpacked_t *patch, float dx, float dy)
{
    const short n[4] = {patch->iNorth, patch->iEast, patch->iSouth, patch->iWest};
    int         i;

    if (!tr.world) {
        return NULL;
    }
    for (i = 0; i < 4; i++) {
        const cTerraPatchUnpacked_t *q;
        if (n[i] < 0 || n[i] >= tr.world->numTerraPatches) {
            continue;
        }
        q = &tr.world->terraPatches[n[i]];
        // match by position rather than trust which link means which side
        if (q->x0 == patch->x0 + dx && q->y0 == patch->y0 + dy) {
            return q;
        }
    }
    return NULL;
}

// world z at heightmap grid point (gx, gy); one step outside the patch reads the edge neighbour, else clamps.
// *pOk = 0 when it had to clamp (the caller then shortens the difference span)
static float R_TerraGridZ(const cTerraPatchUnpacked_t *patch, int gx, int gy, int *pOk)
{
    *pOk = 1;
    if (gx < 0 || gx > 8 || gy < 0 || gy > 8) {
        const float dx = (gx < 0) ? -512.0f : ((gx > 8) ? 512.0f : 0.0f);
        const float dy = (gy < 0) ? -512.0f : ((gy > 8) ? 512.0f : 0.0f);
        const cTerraPatchUnpacked_t *q = (dx == 0.0f || dy == 0.0f) ? R_TerraEdgeNeighbour(patch, dx, dy) : NULL;
        if (q) {
            const int nx = (gx < 0) ? gx + 8 : ((gx > 8) ? gx - 8 : gx);
            const int ny = (gy < 0) ? gy + 8 : ((gy > 8) ? gy - 8 : gy);
            return (float)(q->heightmap[ny * 9 + nx] * 2) + q->z0;
        }
        *pOk = 0;
        gx = (gx < 0) ? 0 : ((gx > 8) ? 8 : gx);
        gy = (gy < 0) ? 0 : ((gy > 8) ? 8 : gy);
    }
    return (float)(patch->heightmap[gy * 9 + gx] * 2) + patch->z0;
}

// unit surface normal at the heightmap grid point nearest (x, y) - central differences, one-sided where clamped
static void R_TerraHeightmapNormal(const cTerraPatchUnpacked_t *patch, float x, float y, vec3_t out)
{
    int   gx = (int)floor((x - patch->x0) / 64.0f + 0.5f);
    int   gy = (int)floor((y - patch->y0) / 64.0f + 0.5f);
    int   okL, okR, okD, okU;
    float zL, zR, zD, zU, spanX, spanY;

    gx = (gx < 0) ? 0 : ((gx > 8) ? 8 : gx);
    gy = (gy < 0) ? 0 : ((gy > 8) ? 8 : gy);

    zL    = R_TerraGridZ(patch, gx - 1, gy, &okL);
    zR    = R_TerraGridZ(patch, gx + 1, gy, &okR);
    zD    = R_TerraGridZ(patch, gx, gy - 1, &okD);
    zU    = R_TerraGridZ(patch, gx, gy + 1, &okU);
    spanX = 64.0f * (float)(okL + okR);
    spanY = 64.0f * (float)(okD + okU);

    out[0] = (spanX > 0.0f) ? -(zR - zL) / spanX : 0.0f;
    out[1] = (spanY > 0.0f) ? -(zU - zD) / spanY : 0.0f;
    out[2] = 1.0f;
    VectorNormalize(out);
}

/*
=============
HZM gl2 [bug-3007] CONTINUOUS terrain light direction: one per HEIGHTMAP VERTEX, not one per 512u patch.

bug-2905 moved the patch light direction to the patch CENTRE, 48u up, so the LOD could no longer flip it. That is
LOD-independent but still ONE L per patch, and R_LightDirForPoint adds the sun only when its trace reaches the sky:
a patch whose centre stands in a shadow gets straight up while its neighbours get the sun or moon. lightall's
lightmap branch re-shades the lightmap by max(N.L,0)/max(n.L,0.25), so on the same _nh the relief contrast is about
+-2 % under L = up and +-19 % under the m4l3 moon - a step that sat exactly on the straight 512u patch borders
(docs/proposals/ground_blend_2026-09-26/diagnosis.md section 3; m4l3 patch 84 against all its moonlit neighbours).

Here L is sampled at every heightmap vertex (64u grid, the same 48u lift) and given to the LOD vertex standing on
it. Every LOD vertex IS a heightmap vertex (MAX_TERRAIN_LOD 6: splits stop at one 64u cell, R_InterpolateVert
averages xy exactly), so:
  - the bug-2905 property is kept: a vertex's L is a function of its ground point, never of which vertex the
    tessellation split last (the bug-2905 heightmap normals, R_TerraHeightmapNormal, work the same way);
  - and it GEOMORPHS like the height (R_HZM_TerrainVertLightDir): a vertex a split has just added starts at the
    average of its two hypotenuse ends - exactly what the unsplit triangle drew at that point - and moves to its own
    value with the factor R_CalcVertMorphHeight gives its height. A per-vertex L that switched at once would pop
    wherever L changes (a shadow edge, a lamp coming into view) each time the LOD front crossed it (review of
    2026-09-27). Morphed, a split or merge moves L no more than it moves the geometry: the start is exact once the
    two ends have finished their own morph, as it is for the height. A cautious merge also waits for L to morph
    back (R_MergeInternalCautious): where a midpoint's height equals its ends' average the height test alone passes
    at any morph and the vertex would vanish mid-blend (~14% of split points on the retail maps). The bug-2905
    heightmap NORMALS still switch per vertex on a split, grid on or off; morphing them from the same parents is
    the natural follow-up if the A/B shows it;
  - neighbours AGREE: a border vertex is one world point. The builder copies an edge-shared row from the neighbour
    it already built; a corner shared only diagonally is traced again from identical inputs, so it agrees wherever
    the border heights agree (0 mismatches over ~60k shared border points in 9 sampled terrain BSPs). The GPU
    interpolates L across each triangle and lightall normalises it per pixel, so the relief term fades over one
    64u cell at a shadow edge instead of stepping on a 512u line.
Built ONCE per map, at load, through R_LightDirForPointStatic: the runtime call's sphere-light + sun-visibility sum
minus its per-view areamask gate, so the cached value cannot depend on where the first view stood. About 64 traced
points per patch (shared border points are copied); the per-frame, per-patch trace of the centre path goes away.
The build logs its cost:  ^~^~^ HZM TERRAIN LGRID <map>: ...

OMAHA (user rule - no ground or texture work on Omaha): the m3l1a / m3l1b / e3l1 / e3l2 BSPs, their _sml copies and
obj_team3 always take the bug-2905 per-patch path, whatever the switch says. This is a look change, not a restore
of retail (gl1 draws no terrain relief at all), so the rule's correctness exception does not apply. Same map set as
docs/tools/gen_terrain_pak_v4.py OMAHA_MAPS.

Switch: r_hzmTerrainLightGrid (tr_init.c), flags 0, live. -1 or "" = HZM_TERRAINLIGHTGRID_AUTO (tr_local.h), 0 = the
bug-2905 per-patch centre (r_hzmTerrainLightCentre still picks centre/head inside it), 1 = per heightmap vertex.
=============
*/
static qboolean R_HZM_TerrainLightMapProtected(const char *baseName)
{
    static const char *const omaha[] = {"m3l1a", "m3l1b", "e3l1", "e3l2", "obj_team3"};
    int                      i;

    if (!baseName || !baseName[0]) {
        return qfalse;
    }
    for (i = 0; i < (int)ARRAY_LEN(omaha); i++) {
        const size_t n = strlen(omaha[i]);
        // the BSP itself or a suffixed copy of it (m3l1a_sml): the gen_terrain_pak_v4.py rule
        if (!Q_stricmpn(baseName, omaha[i], (int)n) && (baseName[n] == 0 || baseName[n] == '_')) {
            return qtrue;
        }
    }
    return qfalse;
}

static qboolean R_HZM_TerrainLightSwitch(void)
{
    if (!r_hzmTerrainLightGrid || !r_hzmTerrainLightGrid->string[0] || r_hzmTerrainLightGrid->integer < 0) {
        return HZM_TERRAINLIGHTGRID_AUTO ? qtrue : qfalse;
    }
    return r_hzmTerrainLightGrid->integer ? qtrue : qfalse;
}

// index of the edge neighbour of `patch` at (x0 + dx, y0 + dy) in w->terraPatches, or -1 (R_TerraEdgeNeighbour's
// position match, on an explicit world: the build runs inside RE_LoadWorldMap, before tr.world is set)
static int R_HZM_TerraEdgeNeighbourIndex(const world_t *w, const cTerraPatchUnpacked_t *patch, float dx, float dy)
{
    const short n[4] = {patch->iNorth, patch->iEast, patch->iSouth, patch->iWest};
    int         i;

    for (i = 0; i < 4; i++) {
        const cTerraPatchUnpacked_t *q;
        if (n[i] < 0 || n[i] >= w->numTerraPatches) {
            continue;
        }
        q = &w->terraPatches[n[i]];
        if (q->x0 == patch->x0 + dx && q->y0 == patch->y0 + dy) {
            return n[i];
        }
    }
    return -1;
}

static void R_HZM_TerrainLightGridBuild(world_t *w)
{
    vec3_t up;
    int    pi, gx, gy, traced = 0, shared = 0, t0;

    if (!w || !w->hzmTerraLightDir || w->hzmTerraLightDirBuilt) {
        return;
    }
    t0 = ri.Milliseconds();
    VectorSet(up, 0.0f, 0.0f, 1.0f);

    for (pi = 0; pi < w->numTerraPatches; pi++) {
        const cTerraPatchUnpacked_t *p   = &w->terraPatches[pi];
        int16_t(*dst)[4]                 = &w->hzmTerraLightDir[pi * 81];
        // edge neighbours this sweep has already built (lower index): their shared row/column is copied verbatim
        const int iw = R_HZM_TerraEdgeNeighbourIndex(w, p, -512.0f, 0.0f);
        const int ie = R_HZM_TerraEdgeNeighbourIndex(w, p, 512.0f, 0.0f);
        const int is = R_HZM_TerraEdgeNeighbourIndex(w, p, 0.0f, -512.0f);
        const int in = R_HZM_TerraEdgeNeighbourIndex(w, p, 0.0f, 512.0f);

        for (gy = 0; gy < 9; gy++) {
            for (gx = 0; gx < 9; gx++) {
                int src = -1;

                if (gx == 0 && iw >= 0 && iw < pi) {
                    src = iw * 81 + gy * 9 + 8;
                } else if (gx == 8 && ie >= 0 && ie < pi) {
                    src = ie * 81 + gy * 9;
                } else if (gy == 0 && is >= 0 && is < pi) {
                    src = is * 81 + 8 * 9 + gx;
                } else if (gy == 8 && in >= 0 && in < pi) {
                    src = in * 81 + gx;
                }

                if (src >= 0) {
                    VectorCopy4(w->hzmTerraLightDir[src], dst[gy * 9 + gx]);
                    shared++;
                } else {
                    vec3_t at, dir;

                    at[0] = p->x0 + 64.0f * gx;
                    at[1] = p->y0 + 64.0f * gy;
                    at[2] = (float)(p->heightmap[gy * 9 + gx] * 2) + p->z0 + 48.0f;
                    VectorCopy(up, dir);
                    R_LightDirForPointStatic(at, dir, up, w);
                    if (VectorLength(dir) < 0.01f) {
                        VectorCopy(up, dir);
                    } else {
                        VectorNormalize(dir);
                    }
                    R_VaoPackNormal(dst[gy * 9 + gx], dir);
                    traced++;
                }
            }
        }
    }

    w->hzmTerraLightDirBuilt = qtrue;
    ri.Printf(PRINT_ALL, "^~^~^ HZM TERRAIN LGRID %s: %d patches, %d vertices traced, %d shared, %d ms\n",
        w->baseName, w->numTerraPatches, traced, shared, ri.Milliseconds() - t0);
}

// RE_LoadWorldMap (tr_bsp.c): reserve the table on every terrain map outside the Omaha set, so a live switch-on never
// allocates; build it now if the switch is on. On Omaha no table exists, so the draw path cannot take it.
void R_HZM_TerrainLightGridLoad(world_t *w)
{
    if (!w || w->numTerraPatches <= 0 || !w->terraPatches) {
        return;
    }
    if (R_HZM_TerrainLightMapProtected(w->baseName)) {
        ri.Printf(PRINT_ALL, "HZM terrain light grid: not on %s (Omaha ground rule, bug-3007)\n", w->baseName);
        return;
    }
    w->hzmTerraLightDir      = ri.Hunk_Alloc(w->numTerraPatches * 81 * (int)sizeof(*w->hzmTerraLightDir), h_low);
    w->hzmTerraLightDirBuilt = qfalse;
    if (R_HZM_TerrainLightSwitch()) {
        R_HZM_TerrainLightGridBuild(w);
    }
}

// per terrain draw: qtrue = take L from tr.world->hzmTerraLightDir; qfalse = the bug-2905 per-patch path
// (the Omaha set never has a table - R_HZM_TerrainLightGridLoad - so no map-name test is needed here)
static qboolean R_HZM_TerrainLightGridOn(void)
{
    if (!tr.world || !tr.world->hzmTerraLightDir || !R_HZM_TerrainLightSwitch()) {
        return qfalse;
    }
    if (!tr.world->hzmTerraLightDirBuilt) {
        R_HZM_TerrainLightGridBuild(tr.world); // switched on mid-map: one build now, cached for the rest of the map
    }
    return tr.world->hzmTerraLightDirBuilt;
}

// tr_terrain.c R_MergeInternalCautious: is per-vertex L being drawn right now? Never builds (the LOD pass runs
// before the draw that would build it; a first frame answering qfalse only merges as before).
qboolean R_HZM_TerrainLightGridActive(void)
{
    return (tr.world && tr.world->hzmTerraLightDir && tr.world->hzmTerraLightDirBuilt && R_HZM_TerrainLightSwitch())
        ? qtrue : qfalse;
}

// one LOD vertex's light direction. Grid off: the patch value (the bug-2905 path, unchanged). Grid on: the value of the
// heightmap vertex it stands on, GEOMORPHED like its height (see the banner): from the average of its two hypotenuse
// ends (hzmLPar, set by R_InterpolateVert) to its own value by fHzmMorph (set by R_CalcVertMorphHeight). A patch
// corner, an unsplit vertex or a split that snaps its height (tr_terrain.c, varnode flag 8) shows its own value.
static void R_HZM_TerrainVertLightDir(qboolean bGrid, const cTerraPatchUnpacked_t *patch, const terrainVert_t *pv,
    const int16_t *patchL, int16_t *out)
{
    const int16_t(*tab)[4];
    int   gx, gy, own, k;
    float m;

    if (!bGrid) {
        VectorCopy4(patchL, out);
        return;
    }
    tab = &tr.world->hzmTerraLightDir[(int)(patch - tr.world->terraPatches) * 81];
    // the grid point this vertex stands on (same rounding as R_TerraHeightmapNormal; exact, every LOD vertex is one)
    gx  = (int)floor((pv->xyz[0] - patch->x0) / 64.0f + 0.5f);
    gy  = (int)floor((pv->xyz[1] - patch->y0) / 64.0f + 0.5f);
    gx  = (gx < 0) ? 0 : ((gx > 8) ? 8 : gx);
    gy  = (gy < 0) ? 0 : ((gy > 8) ? 8 : gy);
    own = gy * 9 + gx;
    m   = pv->fHzmMorph;
    if (pv->hzmLPar[0] > 80 || pv->hzmLPar[1] > 80 || !(m < 1.0f)) {
        VectorCopy4(tab[own], out);
        return;
    }
    if (!(m > 0.0f)) {
        m = 0.0f;
    }
    // R_VaoPackNormal is linear in the vector and the attribute is a normalised GL_SHORT, so blending the packed
    // components is blending the vectors. The blend need not be unit length: lightall normalises L per pixel, and
    // every stored L has z > 0.2 (R_LightDirForPoint), so no blend of them can vanish.
    for (k = 0; k < 3; k++) {
        const float a = 0.5f * ((float)tab[pv->hzmLPar[0]][k] + (float)tab[pv->hzmLPar[1]][k]);
        out[k]        = (int16_t)floor(a + ((float)tab[own][k] - a) * m + 0.5f);
    }
    out[3] = tab[own][3];
}

/*
=============
HZM gl2 [bug-3064] PROBE: the per-patch relief light direction and every input it depends on, printed so that loads
of the same map can be diffed (menu start vs in-game `map` vs vid_restart). A console command, inert unless typed:
    hzmtlprobe <tag>
One header (overbright, sun, sphere-light count, areamask), one line per patch, one footer with hashes.
  L  = what RB_DrawTerrainTris draws today: centre + 48u, R_LightDirForPoint with the LIVE areamask
  Ls = the view-independent variant the bug-3061 grid is built from (no areamask gate)
Per patch: leaf = the point has a lit leaf, sl = its light list starts with the sun, sh = the sun trace reached the sky,
nl = sphere lights added (live / static), sw = the sun's weight |s_sun.color| when added, lw = |sum of lamp terms|.
=============
*/
static unsigned int R_HZM_Fnv(unsigned int h, const void *data, int n)
{
    const byte *b = (const byte *)data;

    while (n-- > 0) {
        h ^= *b++;
        h *= 16777619u;
    }
    return h;
}

void R_HZM_TerrainLProbe_f(void)
{
    const char   *tag = (ri.Cmd_Argc() > 1) ? ri.Cmd_Argv(1) : "-";
    world_t      *w   = tr.world;
    unsigned int  hRT = 2166136261u, hST = 2166136261u, hG = 2166136261u;
    int           pi, nUp = 0, nSun = 0, nLamp = 0;

    if (!w || !w->terraPatches || w->numTerraPatches <= 0) {
        ri.Printf(PRINT_ALL, "^~^~^ TLPROBE %s no terrain world\n", tag);
        return;
    }
    ri.Printf(PRINT_ALL,
        "^~^~^ TLPROBE %s head map=%s obBits=%d obShift=%d obMult=%g idLight=%g mapOBBits=%d mapOBScale=%g sunExists=%d "
        "sunColor=%g %g %g |sun|=%g sunDir=%.4f %.4f %.4f trSunLight=%g %g %g nSL=%d patches=%d grid=%d/%d amask=%02x%02x%02x%02x "
        "vis=%d\n",
        tag, w->baseName, tr.overbrightBits, tr.overbrightShift, tr.overbrightMult, tr.identityLight,
        r_mapOverBrightBits ? r_mapOverBrightBits->integer : -1, r_mapOverBrightScale ? r_mapOverBrightScale->value : -1.0f,
        (int)s_sun.exists, s_sun.color[0], s_sun.color[1], s_sun.color[2], VectorLength(s_sun.color),
        s_sun.direction[0], s_sun.direction[1], s_sun.direction[2], tr.sunLight[0], tr.sunLight[1], tr.sunLight[2],
        tr.numSLights, w->numTerraPatches, w->hzmTerraLightDir ? 1 : 0, (int)w->hzmTerraLightDirBuilt,
        backEnd.refdef.areamask[0], backEnd.refdef.areamask[1], backEnd.refdef.areamask[2], backEnd.refdef.areamask[3],
        w->vis ? 1 : 0);

    for (pi = 0; pi < w->numTerraPatches; pi++) {
        const cTerraPatchUnpacked_t *p = &w->terraPatches[pi];
        vec3_t        at, up, L, Ls;
        hzmLDirInfo_t iRT, iST;
        int16_t       pk[4];

        VectorSet(up, 0.0f, 0.0f, 1.0f);
        at[0] = p->x0 + 256.0f;   // exactly RB_DrawTerrainTris's bug-2905 sample point
        at[1] = p->y0 + 256.0f;
        at[2] = (float)(p->heightmap[40] * 2) + p->z0 + 48.0f;

        VectorCopy(up, L);
        R_LightDirForPointInfo(at, L, up, w, qtrue, &iRT);
        if (VectorLength(L) < 0.01f) { VectorCopy(up, L); } else { VectorNormalize(L); }
        VectorCopy(up, Ls);
        R_LightDirForPointInfo(at, Ls, up, w, qfalse, &iST);
        if (VectorLength(Ls) < 0.01f) { VectorCopy(up, Ls); } else { VectorNormalize(Ls); }

        R_VaoPackNormal(pk, L);
        hRT = R_HZM_Fnv(hRT, pk, 6);
        R_VaoPackNormal(pk, Ls);
        hST = R_HZM_Fnv(hST, pk, 6);
        if (L[2] > 0.9999f) {
            nUp++;
        } else if (iRT.numLights) {
            nLamp++;
        } else {
            nSun++;
        }
        ri.Printf(PRINT_ALL, "^~^~^ TLP %s %d leaf=%d sl=%d sh=%d nl=%d/%d sw=%.1f lw=%.0f L=%.4f %.4f %.4f Ls=%.4f %.4f %.4f\n",
            tag, pi, iRT.leafOk, iRT.sunList, iRT.sunHit, iRT.numLights, iST.numLights, iRT.sunWeight,
            VectorLength(iRT.lampSum), L[0], L[1], L[2], Ls[0], Ls[1], Ls[2]);
    }
    if (w->hzmTerraLightDir && w->hzmTerraLightDirBuilt) {
        hG = R_HZM_Fnv(hG, w->hzmTerraLightDir, w->numTerraPatches * 81 * (int)sizeof(*w->hzmTerraLightDir));
    }
    ri.Printf(PRINT_ALL, "^~^~^ TLPROBE %s end hashL=%08x hashLs=%08x hashGrid=%08x up=%d sun=%d lamp=%d\n",
        tag, hRT, hST, hG, nUp, nSun, nLamp);
}

two compared bit for bit (counted by r_hzmPerfStats; the differing count has to stay 0).
=============
*/
void RB_DrawTerrainTris(srfTerrain_t* p) {
	int i;
	terraInt numv;
	int dlightBits;
	int16_t iNormal[4];
	int16_t iTangent[4];
	vec3_t  vUp;
	vec4_t  vTangent;
	int     firstVert, v;
	float   fSignAcc;   // patch-wide tangent handedness vote (see the loop below)
	static vec3_t s_terraNormAcc[SHADER_MAX_VERTEXES];
	static vec3_t s_terraTanAcc[SHADER_MAX_VERTEXES];
	static vec3_t s_terraBitanAcc[SHADER_MAX_VERTEXES];

	RB_CHECKOVERFLOW(p->nVerts, p->nTris * 3);
	hzmPerf.terrainPatches++;

	// HZM coop - gl2 tess.normal/tangent are PACKED int16[4] (not float like gl1). The port set neither
	// correctly for terrain: the float "1.0" normal stored as int16 1 = a near-zero normal, and the tangent
	// was never written (stale garbage) - so gl2's per-pixel / specular lighting shaded terrain wrong (the
	// moving white sheen). Pack a proper up-normal + a matching tangent once, copy them per vertex (same as
	// the mesh/sprite paths do via R_VaoPackNormal / R_VaoPackTangent). Tangent (1,0,0), handedness +1 ->
	// bitangent (0,1,0): a valid TBN for a flat-up terrain vertex.
	int16_t iLightDir[4];
	const qboolean bLightGrid = R_HZM_TerrainLightGridOn(); // [bug-3007] L per heightmap vertex, see the banner above

	VectorSet(vUp, 0.0f, 0.0f, 1.0f);
	R_VaoPackNormal(iNormal, vUp);
	// HZM coop [vet 2026-08-28] THIS CONSTANT IS NOW ONLY A FALLBACK - see the real per-vertex
	// tangent solve at the bottom of this function. It was previously the tangent every terrain
	// vertex in the game shipped with, and it is correct for almost none of them: terrain UVs are a
	// planar XY projection whose t axis runs -Y, so the handedness needs to be -1 on 91% of the
	// 31,169 retail patches, and (1,0,0) lies off the surface plane by the slope angle on the 90% of
	// patches that are not flat. Measured across all 160 shipped BSPs, this basis was right for 30
	// patches. That is why generated normal maps read on walls - which get a real tangent from
	// R_CalcTangentSpace - and did nothing on the ground.
	vTangent[0] = 1.0f;
	vTangent[1] = 0.0f;
	vTangent[2] = 0.0f;
	vTangent[3] = 1.0f;
	R_VaoPackTangent(iTangent, vTangent);

	// HZM coop [vet 2026-08-27] ...and the LIGHT DIRECTION, which this path never wrote at all.
	//
	// tess.lightdir is a persistent array: leaving it untouched does not mean 'no light direction',
	// it means every terrain vertex inherits whatever direction the previously batched surface left
	// there - a brush face somewhere else in the map. Terrain is the ground of every outdoor level,
	// so the largest lit surface in the game has been shading against an unrelated wall's light
	// vector. It went unnoticed because nothing sampled a normal map on it until now; with per-pixel
	// relief switched on it becomes the difference between ground that reads as ground and ground
	// that is lit from the wrong side.
	//
	// Sampled once per patch rather than per vertex, matching how the normal and tangent above are
	// done: terrain patches are small and flat, and R_LightDirForPoint is a world query.
	{
		vec3_t vLightDir, vNorm;
		VectorSet(vNorm, 0.0f, 0.0f, 1.0f);
		VectorSet(vLightDir, 0.0f, 0.0f, 1.0f);
		if (tr.world && !bLightGrid) { // [bug-3007] the grid path writes L per vertex below instead
			// [bug-2905] from the patch CENTRE, 48u up - never from g_pVert[iVertHead], which is whichever
			// vertex the LOD split last (see R_TerraEdgeNeighbour's header). drawinfo is the patch's first member.
			const cTerraPatchUnpacked_t *pPatch = (const cTerraPatchUnpacked_t *)p;
			vec3_t                       vAt;
			static cvar_t               *s_terraLightCentre = NULL;
			if (!s_terraLightCentre) {
				// 0 = the old sample point (the LOD head vertex), kept only to A/B this fix live
				s_terraLightCentre = ri.Cvar_Get("r_hzmTerrainLightCentre", "1", CVAR_ARCHIVE);
			}
			vAt[0] = pPatch->x0 + 256.0f;
			vAt[1] = pPatch->y0 + 256.0f;
			vAt[2] = (float)(pPatch->heightmap[40] * 2) + pPatch->z0 + 48.0f;
			if (!s_terraLightCentre->integer && p->iVertHead) {
				VectorCopy(g_pVert[p->iVertHead].xyz, vAt);
			}
			R_LightDirForPoint(vAt, vLightDir, vNorm, tr.world);
			if (VectorLength(vLightDir) < 0.01f) {
				VectorSet(vLightDir, 0.0f, 0.0f, 1.0f);
			} else {
				VectorNormalize(vLightDir);
			}
		}
		R_VaoPackNormal(iLightDir, vLightDir);
	}

	firstVert = tess.numVertexes; // remember where this patch's verts start (for the normal pass below)

	dlightBits = p->dlightBits[0];
	tess.dlightBits |= dlightBits;
	if (p->dlightMap[0])
	{
		float lmScale = (1.0 / LIGHTMAP_SIZE) / p->lmapStep;

		for (i = p->iVertHead; i; i = g_pVert[i].iNext) {
			assert(tess.numVertexes < SHADER_MAX_VERTEXES);

			VectorCopy(g_pVert[i].xyz, tess.xyz[tess.numVertexes]);
            tess.texCoords[tess.numVertexes][0] = g_pVert[i].texCoords[0][0];
            tess.texCoords[tess.numVertexes][1] = g_pVert[i].texCoords[0][1];
            tess.lightCoords[tess.numVertexes][0] = g_pVert[i].xyz[0] * lmScale + p->lmapX;
            tess.lightCoords[tess.numVertexes][1] = g_pVert[i].xyz[1] * lmScale + p->lmapY;
			VectorCopy4(iNormal, tess.normal[tess.numVertexes]);
			VectorCopy4(iTangent, tess.tangent[tess.numVertexes]);
			R_HZM_TerrainVertLightDir(bLightGrid, (const cTerraPatchUnpacked_t *)p, &g_pVert[i], iLightDir, tess.lightdir[tess.numVertexes]);
			tess.color[tess.numVertexes][0] = 0xffff;
			tess.color[tess.numVertexes][1] = 0xffff;
			tess.color[tess.numVertexes][2] = 0xffff;
			tess.color[tess.numVertexes][3] = 0xffff;

			g_pVert[i].iVertArray = tess.numVertexes;
			tess.numVertexes++;
		}
	}
	else
	{
		for (i = p->iVertHead; i; i = g_pVert[i].iNext) {
			assert(tess.numVertexes < SHADER_MAX_VERTEXES);

			VectorCopy(g_pVert[i].xyz, tess.xyz[tess.numVertexes]);
            tess.texCoords[tess.numVertexes][0] = g_pVert[i].texCoords[0][0];
            tess.texCoords[tess.numVertexes][1] = g_pVert[i].texCoords[0][1];
            tess.lightCoords[tess.numVertexes][0] = g_pVert[i].texCoords[1][0];
            tess.lightCoords[tess.numVertexes][1] = g_pVert[i].texCoords[1][1];
			//tess.vertexDlightBits[tess.numVertexes] = dlightBits;
			VectorCopy4(iNormal, tess.normal[tess.numVertexes]);
			VectorCopy4(iTangent, tess.tangent[tess.numVertexes]);
			R_HZM_TerrainVertLightDir(bLightGrid, (const cTerraPatchUnpacked_t *)p, &g_pVert[i], iLightDir, tess.lightdir[tess.numVertexes]);
            tess.color[tess.numVertexes][0] = 0xffff;
            tess.color[tess.numVertexes][1] = 0xffff;
            tess.color[tess.numVertexes][2] = 0xffff;
            tess.color[tess.numVertexes][3] = 0xffff;

			g_pVert[i].iVertArray = tess.numVertexes;
			tess.numVertexes++;
		}
	}

	// HZM coop - zero the per-vertex normal accumulator for this patch's verts.
	for (v = firstVert; v < tess.numVertexes; v++) {
		VectorClear(s_terraNormAcc[v]);
		VectorClear(s_terraTanAcc[v]);
		VectorClear(s_terraBitanAcc[v]);
	}

	// [user 2026-08-28] PATCH-WIDE HANDEDNESS. Deciding the tangent sign per VERTEX made it flip as the
	// terrain LOD morphed: the varnode tessellation changes the triangle set with view distance, so the
	// accumulated bitangent at a vertex changes too, and near a degenerate sum the sign could land either
	// way from frame to frame. A flipped sign inverts the normal map's green channel, which inverts the
	// lighting - seen as whole 512-unit patches flashing as the player moves toward or around them.
	// A patch's UVs are a single planar XY projection, so ONE sign is correct for all of it by
	// construction; summing the evidence over the patch also makes the decision robust where any
	// individual vertex is degenerate.
	fSignAcc = 0.0f;

	for (i = p->iTriHead; i; i = g_pTris[i].iNext)
	{
		assert(tess.numVertexes < SHADER_MAX_INDEXES);

		//
		// Make sure these can be drawn
		//
		if (g_pTris[i].byConstChecks & 4)
		{
			int    ia = g_pVert[g_pTris[i].iPt[0]].iVertArray;
			int    ib = g_pVert[g_pTris[i].iPt[1]].iVertArray;
			int    ic = g_pVert[g_pTris[i].iPt[2]].iVertArray;
			vec3_t e1, e2, fn;

			// HZM coop - area-weighted face normal, accumulated into each vertex -> smooth terrain normals,
			// so gl2's per-pixel sun lighting follows the real slopes instead of a flat sweeping glint.
			VectorSubtract(tess.xyz[ib], tess.xyz[ia], e1);
			VectorSubtract(tess.xyz[ic], tess.xyz[ia], e2);
			CrossProduct(e1, e2, fn);
			VectorAdd(s_terraNormAcc[ia], fn, s_terraNormAcc[ia]);
			VectorAdd(s_terraNormAcc[ib], fn, s_terraNormAcc[ib]);
			VectorAdd(s_terraNormAcc[ic], fn, s_terraNormAcc[ic]);

			// HZM coop [vet 2026-08-28] ...and the tangent frame, solved from this triangle's own UV
			// gradient instead of assumed. Left unnormalized so the accumulation is area-weighted, the
			// same way the face normal above is.
			{
				float du1 = tess.texCoords[ib][0] - tess.texCoords[ia][0];
				float dv1 = tess.texCoords[ib][1] - tess.texCoords[ia][1];
				float du2 = tess.texCoords[ic][0] - tess.texCoords[ia][0];
				float dv2 = tess.texCoords[ic][1] - tess.texCoords[ia][1];
				float det = du1 * dv2 - du2 * dv1;

				if (fabs(det) > 1e-12f) {
					float  f = 1.0f / det;
					vec3_t t, b;
					int    k;

					for (k = 0; k < 3; k++) {
						t[k] = (e1[k] * dv2 - e2[k] * dv1) * f;
						b[k] = (e2[k] * du1 - e1[k] * du2) * f;
					}
					VectorAdd(s_terraTanAcc[ia], t, s_terraTanAcc[ia]);
					VectorAdd(s_terraTanAcc[ib], t, s_terraTanAcc[ib]);
					VectorAdd(s_terraTanAcc[ic], t, s_terraTanAcc[ic]);
					VectorAdd(s_terraBitanAcc[ia], b, s_terraBitanAcc[ia]);
					VectorAdd(s_terraBitanAcc[ib], b, s_terraBitanAcc[ib]);
					VectorAdd(s_terraBitanAcc[ic], b, s_terraBitanAcc[ic]);

					// area-weighted vote: cross(faceNormal, T) . B is positive for one handedness and
					// negative for the other, and weighting by face area lets the big triangles decide
					{
						vec3_t cf;
						CrossProduct(fn, t, cf);
						fSignAcc += DotProduct(cf, b);
					}
				}
			}

			tess.indexes[tess.numIndexes] = ia;
			tess.indexes[tess.numIndexes + 1] = ib;
			tess.indexes[tess.numIndexes + 2] = ic;
			tess.numIndexes += 3;
		}
	}

	// HZM coop - normalize + pack the accumulated normals (terrain faces up, so force +Z).
	// [bug-2905] ...now from the HEIGHTMAP at the vertex's grid point, so a coarser or finer LOD mesh over the same
	// ground shades the same; the triangle accumulation stays as the fallback (and still feeds the tangent below).
	for (v = firstVert; v < tess.numVertexes; v++) {
		vec3_t  n;
		int16_t pn[4];
		static cvar_t *s_terraHmNormals = NULL;
		if (!s_terraHmNormals) {
			s_terraHmNormals = ri.Cvar_Get("r_hzmTerrainHeightNormals", "1", CVAR_ARCHIVE);
		}
		if (s_terraHmNormals->integer) {
			R_TerraHeightmapNormal((const cTerraPatchUnpacked_t *)p, tess.xyz[v][0], tess.xyz[v][1], n);
		} else {
			VectorCopy(s_terraNormAcc[v], n);
			if (VectorNormalize(n) < 0.001f) {
				VectorSet(n, 0.0f, 0.0f, 1.0f);
			} else if (n[2] < 0.0f) {
				VectorInverse(n);
			}
		}
		R_VaoPackNormal(pn, n);
		VectorCopy4(pn, tess.normal[v]);

		// HZM coop [vet 2026-08-28] Gram-Schmidt the accumulated tangent against the SMOOTHED normal -
		// which is why this lives here rather than up top: the real normal does not exist until this
		// loop. Handedness comes from the accumulated bitangent, so it is measured per vertex rather
		// than assumed, and rotated or mirrored UV sets resolve correctly with no special case.
		{
			vec3_t t, c;
			vec4_t t4;
			int16_t pt[4];

			VectorCopy(s_terraTanAcc[v], t);
			VectorMA(t, -DotProduct(n, t), n, t);
			if (VectorNormalize(t) < 0.001f) {
				// degenerate UVs - fall back to any vector in the surface plane
				VectorSet(t, 1.0f, 0.0f, 0.0f);
				VectorMA(t, -DotProduct(n, t), n, t);
				if (VectorNormalize(t) < 0.001f) {
					VectorSet(t, 0.0f, 1.0f, 0.0f);
				}
			}
			CrossProduct(n, t, c);
			VectorCopy(t, t4);
			// one sign for the whole patch, from the area-weighted vote above. Falls back to the
			// per-vertex sum only if the vote is a dead tie, which needs genuinely degenerate UVs.
			if (fSignAcc > 0.0f)      { t4[3] =  1.0f; }
			else if (fSignAcc < 0.0f) { t4[3] = -1.0f; }
			else                      { t4[3] = (DotProduct(c, s_terraBitanAcc[v]) < 0.0f) ? -1.0f : 1.0f; }
			(void)c;
			R_VaoPackTangent(pt, t4);
			VectorCopy4(pt, tess.tangent[v]);
		}
	}
}

//=========================

void (*rb_surfaceTable[SF_NUM_SURFACE_TYPES])( void *) = {
	(void(*)(void*))RB_SurfaceBad,			// SF_BAD, 
	(void(*)(void*))RB_SurfaceSkip,			// SF_SKIP, 
	(void(*)(void*))RB_SurfaceFace,			// SF_FACE,
	(void(*)(void*))RB_SurfaceGrid,			// SF_GRID,
	(void(*)(void*))RB_SurfaceTriangles,		// SF_TRIANGLES,
	(void(*)(void*))RB_SurfacePolychain,		// SF_POLY,
	(void(*)(void*))RB_SurfaceMesh,			// SF_MDV,
	(void(*)(void*))RB_MDRSurfaceAnim,		// SF_MDR,
	(void(*)(void*))RB_IQMSurfaceAnim,		// SF_IQM,
	(void(*)(void*))RB_SurfaceFlare,		// SF_FLARE,
	(void(*)(void*))RB_SurfaceEntity,		// SF_ENTITY
	(void(*)(void*))RB_SurfaceVaoMdvMesh,   // SF_VAO_MDVMESH
    (void(*)(void*))RB_IQMSurfaceAnimVao,   // SF_VAO_IQM
    //
    // OPENMOHAA-specific stuff
    //=========================
    (void(*)(void*))RB_SurfaceMarkFragment, // SF_MARK_FRAG
	(void(*)(void*))RB_SkelMesh,			// SF_TIKI_SKEL
	(void(*)(void*))RB_StaticMesh,			// SF_TIKI_STATIC
	(void(*)(void*))RB_DrawSwipeSurface,	// SF_SWIPE
	(void(*)(void*))RB_DrawSprite,			// SF_SPRITE
    (void(*)(void*))RB_DrawTerrainTris,		// SF_TERRAIN_PATCH
    //=========================
};
