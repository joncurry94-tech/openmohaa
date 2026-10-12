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

// tr_shader.c -- this file deals with the parsing and definition of shaders

static char *s_shaderText;

// the shader is parsed into these global variables, then copied into
// dynamically allocated memory if it is valid.
static	shaderStage_t	stages[MAX_SHADER_STAGES];		
static	shader_t		shader;
// HZM gl2 re-port (bug-gl2-texmods-null): per-BUNDLE texMod backing storage.
// The bundle-aware ParseTexMod writes through stage->bundle[cntBundle].texMods,
// but InitShaderEx only wired bundle[0] - the first tcMod after a 'nextbundle'
// (e.g. central_europe_winter/snowfield1 'tcMod scale 2 2') wrote through a
// NULL texMods pointer and took the whole boot down during map shader parsing.
// gl1 points EVERY bundle of stage i at the same texMods[i] array (gl1
// tr_shader.c:3424), which makes bundle[1] mods stomp bundle[0]'s slots; give
// each bundle its own array instead so both bundles keep their authored mods.
static	texModInfo_t	texMods[MAX_SHADER_STAGES][NUM_TEXTURE_BUNDLES][TR_MAX_TEXMODS];
static	int				shader_realLightmapIndex;

#define FILE_HASH_SIZE		1024
static	shader_t*		hashTable[FILE_HASH_SIZE];

#define MAX_SHADERTEXT_HASH		2048
static char **shaderTextHashTable[MAX_SHADERTEXT_HASH];

//
// OPENMOHAA-specific stuff
//=========================
static void CreateMultistageFromBundle();
//=========================

/*
================
return a hash value for the filename
================
*/
#ifdef __GNUCC__
  #warning TODO: check if long is ok here 
#endif
static long generateHashValue( const char *fname, const int size ) {
	int		i;
	long	hash;
	char	letter;

	hash = 0;
	i = 0;
	while (fname[i] != '\0') {
		letter = tolower(fname[i]);
		if (letter =='.') break;				// don't include extension
		if (letter =='\\') letter = '/';		// damn path names
		if (letter == PATH_SEP) letter = '/';		// damn path names
		hash+=(long)(letter)*(i+119);
		i++;
	}
	hash = (hash ^ (hash >> 10) ^ (hash >> 20));
	hash &= (size-1);
	return hash;
}

void R_RemapShader(const char *shaderName, const char *newShaderName, const char *timeOffset) {
	char		strippedName[MAX_QPATH];
	int			hash;
	shader_t	*sh, *sh2;
	qhandle_t	h;

	sh = R_FindShaderByName( shaderName );
	if (sh == NULL || sh == tr.defaultShader) {
		h = RE_RegisterShaderLightMap(shaderName, 0);
		sh = R_GetShaderByHandle(h);
	}
	if (sh == NULL || sh == tr.defaultShader) {
		ri.Printf( PRINT_WARNING, "WARNING: R_RemapShader: shader %s not found\n", shaderName );
		return;
	}

	sh2 = R_FindShaderByName( newShaderName );
	if (sh2 == NULL || sh2 == tr.defaultShader) {
		h = RE_RegisterShaderLightMap(newShaderName, 0);
		sh2 = R_GetShaderByHandle(h);
	}

	if (sh2 == NULL || sh2 == tr.defaultShader) {
		ri.Printf( PRINT_WARNING, "WARNING: R_RemapShader: new shader %s not found\n", newShaderName );
		return;
	}

	// remap all the shaders with the given name
	// even tho they might have different lightmaps
	COM_StripExtension(shaderName, strippedName, sizeof(strippedName));
	hash = generateHashValue(strippedName, FILE_HASH_SIZE);
	for (sh = hashTable[hash]; sh; sh = sh->next) {
		if (Q_stricmp(sh->name, strippedName) == 0) {
			if (sh != sh2) {
				sh->remappedShader = sh2;
			} else {
				sh->remappedShader = NULL;
			}
		}
	}
	if (timeOffset) {
		sh2->timeOffset = atof(timeOffset);
	}
}

/*
===============
ParseVector
===============
*/
static qboolean ParseVector( char **text, int count, float *v ) {
#if 0
	char	*token;
	int		i;

	// FIXME: spaces are currently required after parens, should change parseext...
	token = COM_ParseExt( text, qfalse );
	if ( strcmp( token, "(" ) ) {
		ri.Printf( PRINT_WARNING, "WARNING: missing parenthesis in shader '%s'\n", shader.name );
		return qfalse;
	}

	for ( i = 0 ; i < count ; i++ ) {
		token = COM_ParseExt( text, qfalse );
		if ( !token[0] ) {
			ri.Printf( PRINT_WARNING, "WARNING: missing vector element in shader '%s'\n", shader.name );
			return qfalse;
		}
		v[i] = atof( token );
	}

	token = COM_ParseExt( text, qfalse );
	if ( strcmp( token, ")" ) ) {
		ri.Printf( PRINT_WARNING, "WARNING: missing parenthesis in shader '%s'\n", shader.name );
		return qfalse;
	}

	return qtrue;
#endif

	//
	// OPENMOHAA-specific stuff
	//
	char	*token;
	int		i;

	for ( i = 0 ; i < count ; i++ ) {
		token = COM_ParseExt( text, qfalse );
		if ( !token[0] ) {
			ri.Printf( PRINT_WARNING, "WARNING: missing vector element in shader '%s'\n", shader.name );
			return qfalse;
		}
		v[i] = atof( token );
	}

	return qtrue;

}


/*
===============
NameToAFunc
===============
*/
static unsigned NameToAFunc( const char *funcname )
{	
	if ( !Q_stricmp( funcname, "GT0" ) )
	{
		return GLS_ATEST_GT_0;
	}
	else if ( !Q_stricmp( funcname, "LT128" ) )
	{
		return GLS_ATEST_LT_80;
	}
	else if ( !Q_stricmp( funcname, "GE128" ) )
	{
		return GLS_ATEST_GE_80;
	}
	// HZM gl2 re-port (bug-gl2-foliage): MOHAA foliage alpha-test modes, matching
	// gl1 tr_shader.c NameToAFunc. Used by the r_blendtrees/r_blendbushes foliage
	// stages (e.g. TAwintershrub); without these the stage got atest 0 (no test).
	else if ( !Q_stricmp( funcname, "LT_FOLIAGE1" ) )
	{
		return GLS_ATEST_LT_FOLIAGE1;
	}
	else if ( !Q_stricmp( funcname, "GE_FOLIAGE1" ) )
	{
		return GLS_ATEST_GE_FOLIAGE1;
	}
	else if ( !Q_stricmp( funcname, "LT_FOLIAGE2" ) )
	{
		return GLS_ATEST_LT_FOLIAGE2;
	}
	else if ( !Q_stricmp( funcname, "GE_FOLIAGE2" ) )
	{
		return GLS_ATEST_GE_FOLIAGE2;
	}

	ri.Printf( PRINT_WARNING, "WARNING: invalid alphaFunc name '%s' in shader '%s'\n", funcname, shader.name );
	return 0;
}


/*
===============
NameToSrcBlendMode
===============
*/
static int NameToSrcBlendMode( const char *name )
{
	if ( !Q_stricmp( name, "GL_ONE" ) )
	{
		return GLS_SRCBLEND_ONE;
	}
	else if ( !Q_stricmp( name, "GL_ZERO" ) )
	{
		return GLS_SRCBLEND_ZERO;
	}
	else if ( !Q_stricmp( name, "GL_DST_COLOR" ) )
	{
		return GLS_SRCBLEND_DST_COLOR;
	}
	else if ( !Q_stricmp( name, "GL_ONE_MINUS_DST_COLOR" ) )
	{
		return GLS_SRCBLEND_ONE_MINUS_DST_COLOR;
	}
	else if ( !Q_stricmp( name, "GL_SRC_ALPHA" ) )
	{
		return GLS_SRCBLEND_SRC_ALPHA;
	}
	else if ( !Q_stricmp( name, "GL_ONE_MINUS_SRC_ALPHA" ) )
	{
		return GLS_SRCBLEND_ONE_MINUS_SRC_ALPHA;
	}
	else if ( !Q_stricmp( name, "GL_DST_ALPHA" ) )
	{
		if (r_ignoreDstAlpha->integer)
			return GLS_SRCBLEND_ONE;

		return GLS_SRCBLEND_DST_ALPHA;
	}
	else if ( !Q_stricmp( name, "GL_ONE_MINUS_DST_ALPHA" ) )
	{
		if (r_ignoreDstAlpha->integer)
			return GLS_SRCBLEND_ZERO;

		return GLS_SRCBLEND_ONE_MINUS_DST_ALPHA;
	}
	else if ( !Q_stricmp( name, "GL_SRC_ALPHA_SATURATE" ) )
	{
		return GLS_SRCBLEND_ALPHA_SATURATE;
	}

	ri.Printf( PRINT_WARNING, "WARNING: unknown blend mode '%s' in shader '%s', substituting GL_ONE\n", name, shader.name );
	return GLS_SRCBLEND_ONE;
}

/*
===============
NameToDstBlendMode
===============
*/
static int NameToDstBlendMode( const char *name )
{
	if ( !Q_stricmp( name, "GL_ONE" ) )
	{
		return GLS_DSTBLEND_ONE;
	}
	else if ( !Q_stricmp( name, "GL_ZERO" ) )
	{
		return GLS_DSTBLEND_ZERO;
	}
	else if ( !Q_stricmp( name, "GL_SRC_ALPHA" ) )
	{
		return GLS_DSTBLEND_SRC_ALPHA;
	}
	else if ( !Q_stricmp( name, "GL_ONE_MINUS_SRC_ALPHA" ) )
	{
		return GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA;
	}
	else if ( !Q_stricmp( name, "GL_DST_ALPHA" ) )
	{
		if (r_ignoreDstAlpha->integer)
			return GLS_DSTBLEND_ONE;

		return GLS_DSTBLEND_DST_ALPHA;
	}
	else if ( !Q_stricmp( name, "GL_ONE_MINUS_DST_ALPHA" ) )
	{
		if (r_ignoreDstAlpha->integer)
			return GLS_DSTBLEND_ZERO;

		return GLS_DSTBLEND_ONE_MINUS_DST_ALPHA;
	}
	else if ( !Q_stricmp( name, "GL_SRC_COLOR" ) )
	{
		return GLS_DSTBLEND_SRC_COLOR;
	}
	else if ( !Q_stricmp( name, "GL_ONE_MINUS_SRC_COLOR" ) )
	{
		return GLS_DSTBLEND_ONE_MINUS_SRC_COLOR;
	}

	ri.Printf( PRINT_WARNING, "WARNING: unknown blend mode '%s' in shader '%s', substituting GL_ONE\n", name, shader.name );
	return GLS_DSTBLEND_ONE;
}

/*
===============
NameToGenFunc
===============
*/
static genFunc_t NameToGenFunc( const char *funcname )
{
	if ( !Q_stricmp( funcname, "sin" ) )
	{
		return GF_SIN;
	}
	else if ( !Q_stricmp( funcname, "square" ) )
	{
		return GF_SQUARE;
	}
	else if ( !Q_stricmp( funcname, "triangle" ) )
	{
		return GF_TRIANGLE;
	}
	else if ( !Q_stricmp( funcname, "sawtooth" ) )
	{
		return GF_SAWTOOTH;
	}
	else if ( !Q_stricmp( funcname, "inversesawtooth" ) )
	{
		return GF_INVERSE_SAWTOOTH;
	}
	else if ( !Q_stricmp( funcname, "noise" ) )
	{
		return GF_NOISE;
	}

	ri.Printf( PRINT_WARNING, "WARNING: invalid genfunc name '%s' in shader '%s'\n", funcname, shader.name );
	return GF_SIN;
}


/*
===================
ParseWaveForm
===================
*/
static void ParseWaveForm( char **text, waveForm_t *wave )
{
	char *token;

	token = COM_ParseExt( text, qfalse );
	if ( token[0] == 0 )
	{
		ri.Printf( PRINT_WARNING, "WARNING: missing waveform parm in shader '%s'\n", shader.name );
		return;
	}
	wave->func = NameToGenFunc( token );

	// BASE, AMP, PHASE, FREQ
	token = COM_ParseExt( text, qfalse );
	if ( token[0] == 0 )
	{
		ri.Printf( PRINT_WARNING, "WARNING: missing waveform parm in shader '%s'\n", shader.name );
		return;
	}
	wave->base = atof( token );

	token = COM_ParseExt( text, qfalse );
	if ( token[0] == 0 )
	{
		ri.Printf( PRINT_WARNING, "WARNING: missing waveform parm in shader '%s'\n", shader.name );
		return;
	}
	wave->amplitude = atof( token );

	token = COM_ParseExt( text, qfalse );
	if ( token[0] == 0 )
	{
		ri.Printf( PRINT_WARNING, "WARNING: missing waveform parm in shader '%s'\n", shader.name );
		return;
	}
	wave->phase = atof( token );

	token = COM_ParseExt( text, qfalse );
	if ( token[0] == 0 )
	{
		ri.Printf( PRINT_WARNING, "WARNING: missing waveform parm in shader '%s'\n", shader.name );
		return;
	}
	wave->frequency = atof( token );
}


/*
===================
ParseTexMod
===================
*/
static void ParseTexMod( char *_text, shaderStage_t *stage, int cntBundle ) // HZM gl2 re-port: bundle-aware (gl1 parity) - multi-bundle water/ocean shaders overflowed bundle[0]
{
	const char *token;
	char **text = &_text;
	texModInfo_t *tmi;

	if ( stage->bundle[cntBundle].numTexMods == TR_MAX_TEXMODS ) {
		ri.Error( ERR_DROP, "ERROR: too many tcMod stages in shader '%s'", shader.name );
		return;
	}

	tmi = &stage->bundle[cntBundle].texMods[stage->bundle[cntBundle].numTexMods];
	stage->bundle[cntBundle].numTexMods++;

	token = COM_ParseExt( text, qfalse );

	//
	// turb
	//
	if ( !Q_stricmp( token, "turb" ) )
	{
		token = COM_ParseExt( text, qfalse );
		if ( token[0] == 0 )
		{
			ri.Printf( PRINT_WARNING, "WARNING: missing tcMod turb parms in shader '%s'\n", shader.name );
			return;
		}
		tmi->wave.base = atof( token );
		token = COM_ParseExt( text, qfalse );
		if ( token[0] == 0 )
		{
			ri.Printf( PRINT_WARNING, "WARNING: missing tcMod turb in shader '%s'\n", shader.name );
			return;
		}
		tmi->wave.amplitude = atof( token );
		token = COM_ParseExt( text, qfalse );
		if ( token[0] == 0 )
		{
			ri.Printf( PRINT_WARNING, "WARNING: missing tcMod turb in shader '%s'\n", shader.name );
			return;
		}
		tmi->wave.phase = atof( token );
		token = COM_ParseExt( text, qfalse );
		if ( token[0] == 0 )
		{
			ri.Printf( PRINT_WARNING, "WARNING: missing tcMod turb in shader '%s'\n", shader.name );
			return;
		}
		tmi->wave.frequency = atof( token );

		tmi->type = TMOD_TURBULENT;
	}
	//
	// scale
	//
	else if ( !Q_stricmp( token, "scale" ) )
	{
		token = COM_ParseExt( text, qfalse );
		if ( token[0] == 0 )
		{
			ri.Printf( PRINT_WARNING, "WARNING: missing scale parms in shader '%s'\n", shader.name );
			return;
		}
		tmi->scale[0] = atof( token );

		token = COM_ParseExt( text, qfalse );
		if ( token[0] == 0 )
		{
			ri.Printf( PRINT_WARNING, "WARNING: missing scale parms in shader '%s'\n", shader.name );
			return;
		}
		tmi->scale[1] = atof( token );
		tmi->type = TMOD_SCALE;
	}
	//
	// scroll
	//
	else if ( !Q_stricmp( token, "scroll" ) )
	{
		token = COM_ParseExt( text, qfalse );
		if ( token[0] == 0 )
		{
			ri.Printf( PRINT_WARNING, "WARNING: missing scale scroll parms in shader '%s'\n", shader.name );
			return;
		}
		tmi->scroll[0] = atof( token );
		token = COM_ParseExt( text, qfalse );
		if ( token[0] == 0 )
		{
			ri.Printf( PRINT_WARNING, "WARNING: missing scale scroll parms in shader '%s'\n", shader.name );
			return;
		}
		tmi->scroll[1] = atof( token );
		tmi->type = TMOD_SCROLL;
	}
	//
	// stretch
	//
	else if ( !Q_stricmp( token, "stretch" ) )
	{
		token = COM_ParseExt( text, qfalse );
		if ( token[0] == 0 )
		{
			ri.Printf( PRINT_WARNING, "WARNING: missing stretch parms in shader '%s'\n", shader.name );
			return;
		}
		tmi->wave.func = NameToGenFunc( token );

		token = COM_ParseExt( text, qfalse );
		if ( token[0] == 0 )
		{
			ri.Printf( PRINT_WARNING, "WARNING: missing stretch parms in shader '%s'\n", shader.name );
			return;
		}
		tmi->wave.base = atof( token );

		token = COM_ParseExt( text, qfalse );
		if ( token[0] == 0 )
		{
			ri.Printf( PRINT_WARNING, "WARNING: missing stretch parms in shader '%s'\n", shader.name );
			return;
		}
		tmi->wave.amplitude = atof( token );

		token = COM_ParseExt( text, qfalse );
		if ( token[0] == 0 )
		{
			ri.Printf( PRINT_WARNING, "WARNING: missing stretch parms in shader '%s'\n", shader.name );
			return;
		}
		tmi->wave.phase = atof( token );

		token = COM_ParseExt( text, qfalse );
		if ( token[0] == 0 )
		{
			ri.Printf( PRINT_WARNING, "WARNING: missing stretch parms in shader '%s'\n", shader.name );
			return;
		}
		tmi->wave.frequency = atof( token );
		
		tmi->type = TMOD_STRETCH;
	}
	//
	// transform
	//
	else if ( !Q_stricmp( token, "transform" ) )
	{
		token = COM_ParseExt( text, qfalse );
		if ( token[0] == 0 )
		{
			ri.Printf( PRINT_WARNING, "WARNING: missing transform parms in shader '%s'\n", shader.name );
			return;
		}
		tmi->matrix[0][0] = atof( token );

		token = COM_ParseExt( text, qfalse );
		if ( token[0] == 0 )
		{
			ri.Printf( PRINT_WARNING, "WARNING: missing transform parms in shader '%s'\n", shader.name );
			return;
		}
		tmi->matrix[0][1] = atof( token );

		token = COM_ParseExt( text, qfalse );
		if ( token[0] == 0 )
		{
			ri.Printf( PRINT_WARNING, "WARNING: missing transform parms in shader '%s'\n", shader.name );
			return;
		}
		tmi->matrix[1][0] = atof( token );

		token = COM_ParseExt( text, qfalse );
		if ( token[0] == 0 )
		{
			ri.Printf( PRINT_WARNING, "WARNING: missing transform parms in shader '%s'\n", shader.name );
			return;
		}
		tmi->matrix[1][1] = atof( token );

		token = COM_ParseExt( text, qfalse );
		if ( token[0] == 0 )
		{
			ri.Printf( PRINT_WARNING, "WARNING: missing transform parms in shader '%s'\n", shader.name );
			return;
		}
		tmi->translate[0] = atof( token );

		token = COM_ParseExt( text, qfalse );
		if ( token[0] == 0 )
		{
			ri.Printf( PRINT_WARNING, "WARNING: missing transform parms in shader '%s'\n", shader.name );
			return;
		}
		tmi->translate[1] = atof( token );

		tmi->type = TMOD_TRANSFORM;
	}
	//
	// rotate
	//
	else if ( !Q_stricmp( token, "rotate" ) )
	{
		token = COM_ParseExt( text, qfalse );
		if ( token[0] == 0 )
		{
			ri.Printf( PRINT_WARNING, "WARNING: missing tcMod rotate parms in shader '%s'\n", shader.name );
			return;
		}
		tmi->rotateSpeed = atof( token );
		tmi->type = TMOD_ROTATE;
	}
	//
	// wavetrans / wavetrant - HZM gl2 parity (bug-1242), mirrors renderergl1 tr_shader.c:595-604.
	// Without these two branches the token falls through to the unknown-tcMod warning below and the
	// texMod is left as TMOD_NONE: no crash, but the animation silently never happens. That is
	// exactly what makes gl2 shorelines look dead next to gl1.
	//
	else if ( !Q_stricmp( token, "wavetrans" ) )
	{
		ParseWaveForm( text, &tmi->wave );
		tmi->type = TMOD_WAVETRANS;
	}
	else if ( !Q_stricmp( token, "wavetrant" ) )
	{
		ParseWaveForm( text, &tmi->wave );
		tmi->type = TMOD_WAVETRANT;
	}
	//
	// entityTranslate
	//
	else if ( !Q_stricmp( token, "entityTranslate" ) )
	{
		tmi->type = TMOD_ENTITY_TRANSLATE;
	}
	else
	{
		ri.Printf( PRINT_WARNING, "WARNING: unknown tcMod '%s' in shader '%s'\n", token, shader.name );
	}
}


/*
===================
ParseStage
===================
*/
static qboolean ParseStage( shaderStage_t *stage, char **text )
{
	char *token;
	int depthMaskBits = GLS_DEPTHMASK_TRUE, blendSrcBits = 0, blendDstBits = 0, atestBits = 0, depthFuncBits = 0;
	qboolean depthMaskExplicit = qfalse;
	//
	// OPENMOHAA-specific stuff
	//=========================
	int colorBits;
	int cntBundle = 0;
	int depthTestBits = 0;
	int fogBits = 0;
	qboolean shouldProcess = qtrue;
	// HZM gl2 re-port (bug-gl2-nopicmip): per-stage image flags, seeded from the
	// shader-level flags like gl1 ParseStage (gl1 tr_shader.c:851-852)
	qboolean stageNoMipMaps = shader.noMipMaps;
	qboolean stageNoPicMip = shader.noPicMip;
	//=========================

	stage->active = qtrue;

	while ( 1 )
	{
		token = COM_ParseExt( text, qtrue );
		if ( !token[0] )
		{
			ri.Printf( PRINT_WARNING, "WARNING: no matching '}' found\n" );
			return qfalse;
		}

		if ( token[0] == '}' )
		{
			break;
		}
		// HZM gl2 re-port (bug-gl2-nopicmip): stage-level nomipmaps/nopicmip used
		// to fall into the unknown-parameter branch below and kill the WHOLE
		// shader -> default texture (misc_outside.shader deepbluesea* animated
		// ocean on the D-Day/N-Africa beaches, Tanks.shader US_V_Chains). gl1
		// treats them as per-stage flags (gl1 tr_shader.c:869-878).
		else if ( !Q_stricmp( token, "nomipmaps" ) )
		{
			stageNoMipMaps = qtrue;
			continue;
		}
		else if ( !Q_stricmp( token, "nopicmip" ) )
		{
			stageNoPicMip = qtrue;
			continue;
		}
		//
		// map <name>
		//
		else if ( !Q_stricmp( token, "map" ) )
		{
			token = COM_ParseExt( text, qfalse );
			if ( !token[0] )
			{
				ri.Printf( PRINT_WARNING, "WARNING: missing parameter for 'map' keyword in shader '%s'\n", shader.name );
				return qfalse;
			}

			if ( !Q_stricmp( token, "$whiteimage" ) )
			{
				stage->bundle[cntBundle].image[0] = tr.whiteImage;
				continue;
			}
			else if ( !Q_stricmp( token, "$lightmap" ) )
			{
				stage->bundle[cntBundle].isLightmap = qtrue;
				if ( shader.lightmapIndex < 0 || !tr.lightmaps ) {
					stage->bundle[cntBundle].image[0] = tr.whiteImage;
				} else {
					stage->bundle[cntBundle].image[0] = tr.lightmaps[shader.lightmapIndex];
				}
				continue;
			}
			else if ( !Q_stricmp( token, "$deluxemap" ) )
			{
				if (!tr.worldDeluxeMapping)
				{
					ri.Printf( PRINT_WARNING, "WARNING: shader '%s' wants a deluxe map in a map compiled without them\n", shader.name );
					return qfalse;
				}

				stage->bundle[cntBundle].isLightmap = qtrue;
				if ( shader.lightmapIndex < 0 ) {
					stage->bundle[cntBundle].image[0] = tr.whiteImage;
				} else {
					stage->bundle[cntBundle].image[0] = tr.deluxemaps[shader.lightmapIndex];
				}
				continue;
			}
			else
			{
				imgType_t type = IMGTYPE_COLORALPHA;
				imgFlags_t flags = IMGFLAG_NONE;

				// HZM gl2 re-port (bug-gl2-nopicmip): per-stage flags
				if (!stageNoMipMaps)
					flags |= IMGFLAG_MIPMAP;

				if (!stageNoPicMip)
					flags |= IMGFLAG_PICMIP;

				if (stage->type == ST_NORMALMAP || stage->type == ST_NORMALPARALLAXMAP)
				{
					type = IMGTYPE_NORMAL;
					flags |= IMGFLAG_NOLIGHTSCALE;

					if (stage->type == ST_NORMALPARALLAXMAP)
						type = IMGTYPE_NORMALHEIGHT;
				}
				else
				{
					// HZM gl2 (r_hzmGenNormals): the path filter is a pure function of the
					// image path, so the same texture reached through two different shaders
					// always gets the same answer - which matters, because R_FindImageFile
					// caches by name and warns on mixed flags.
					if (r_genNormalMaps->integer || R_HZM_GenNormalsWanted( token ))
						flags |= IMGFLAG_GENNORMALMAP;
				}

				stage->bundle[cntBundle].image[0] = R_FindImageFile( token, type, flags );

				if ( !stage->bundle[cntBundle].image[0] )
				{
					ri.Printf( PRINT_WARNING, "WARNING: R_FindImageFile could not find '%s' in shader '%s'\n", token, shader.name );
					return qfalse;
				}
			}
		}
		//
		// clampmap <name>
		//
		//else if ( !Q_stricmp( token, "clampmap" ) )
        //
        // OPENMOHAA-specific stuff
        //=========================
		else if ( !Q_stricmpn( token, "clampmap", 8 ) )
        //=========================
		{
			imgType_t type = IMGTYPE_COLORALPHA;
			imgFlags_t flags;

			// HZM gl2 (bug-2227) - the FIXME that used to sit here is now done.
			//
			// This branch matches "clampmap" as an 8-CHARACTER PREFIX, so clampmapx and clampmapy
			// landed in it too and were given the both-axis IMGFLAG_CLAMPTOEDGE. gl1 has always
			// resolved the two axes separately (renderergl1/tr_shader.c:938-978).
			//
			// It matters on exactly one surface in the shipped game, and it is a surface players
			// stare at: textures/mohtest/omaha_set4_shoreline, the wet-sand strip at Omaha's
			// waterline, is the ONLY clampmapy in main/mainta/maintt (the two others are in the
			// unused scripts/test.shader). It is authored to REPEAT along the beach and CLAMP across
			// the strip, so the one wet->dry gradient reads once from waterline to dry sand. Clamping
			// S as well pinned the S coordinate at the edge texel and stretched a single texel column
			// along the whole 15,000-unit beach - which is precisely the "blurred texture where the
			// water meets the sand" that has been reported since gl2 became the renderer we ship.
			if (!token[8]) {
				flags = IMGFLAG_CLAMPTOEDGE;                       // clampmap  - both axes
			} else if (token[8] == 'x' && !token[9]) {
				flags = IMGFLAG_CLAMPTOEDGE_X;                     // clampmapx - S only
			} else if (token[8] == 'y' && !token[9]) {
				flags = IMGFLAG_CLAMPTOEDGE_Y;                     // clampmapy - T only
			} else {
				// gl1 prints this and then skips the keyword entirely (:974). Matching it: fall back
				// to the both-axis behaviour rather than dropping the image, so an unknown suffix
				// degrades to what gl2 did before instead of leaving the stage with no texture.
				ri.Printf( PRINT_WARNING, "WARNING: Converting unknown clampmap type '%s' to clampmap in shader '%s'\n", token, shader.name );
				flags = IMGFLAG_CLAMPTOEDGE;
			}

			token = COM_ParseExt( text, qfalse );
			if ( !token[0] )
			{
				ri.Printf( PRINT_WARNING, "WARNING: missing parameter for 'clampmap' keyword in shader '%s'\n", shader.name );
				return qfalse;
			}

			// HZM gl2 re-port (bug-gl2-nopicmip): per-stage flags
			if (!stageNoMipMaps)
				flags |= IMGFLAG_MIPMAP;

			if (!stageNoPicMip)
				flags |= IMGFLAG_PICMIP;

			if (stage->type == ST_NORMALMAP || stage->type == ST_NORMALPARALLAXMAP)
			{
				type = IMGTYPE_NORMAL;
				flags |= IMGFLAG_NOLIGHTSCALE;

				if (stage->type == ST_NORMALPARALLAXMAP)
					type = IMGTYPE_NORMALHEIGHT;
			}
			else
			{
				// HZM gl2 (r_hzmGenNormals) - see the 'map' keyword above
				if (r_genNormalMaps->integer || R_HZM_GenNormalsWanted( token ))
					flags |= IMGFLAG_GENNORMALMAP;
			}


			stage->bundle[cntBundle].image[0] = R_FindImageFile( token, type, flags );
			if ( !stage->bundle[cntBundle].image[0] )
			{
				ri.Printf( PRINT_WARNING, "WARNING: R_FindImageFile could not find '%s' in shader '%s'\n", token, shader.name );
				return qfalse;
			}
		}
		//
		// animMap <frequency> <image1> .... <imageN>
		//
		else if ( !Q_stricmp( token, "animMap" ) )
		{
			int	totalImages = 0;

			token = COM_ParseExt( text, qfalse );
			if ( !token[0] )
			{
				ri.Printf( PRINT_WARNING, "WARNING: missing parameter for 'animMap' keyword in shader '%s'\n", shader.name );
				return qfalse;
			}
			stage->bundle[cntBundle].imageAnimationSpeed = atof( token );

			// parse up to MAX_IMAGE_ANIMATIONS animations
			while ( 1 ) {
				int		num;

				token = COM_ParseExt( text, qfalse );
				if ( !token[0] ) {
					break;
				}
				num = stage->bundle[cntBundle].numImageAnimations;
				if ( num < MAX_IMAGE_ANIMATIONS ) {
					imgFlags_t flags = IMGFLAG_NONE;

					// HZM gl2 re-port (bug-gl2-nopicmip): per-stage flags
					if (!stageNoMipMaps)
						flags |= IMGFLAG_MIPMAP;

					if (!stageNoPicMip)
						flags |= IMGFLAG_PICMIP;

					stage->bundle[cntBundle].image[num] = R_FindImageFile( token, IMGTYPE_COLORALPHA, flags );
					if ( !stage->bundle[cntBundle].image[num] )
					{
						ri.Printf( PRINT_WARNING, "WARNING: R_FindImageFile could not find '%s' in shader '%s'\n", token, shader.name );
						return qfalse;
					}
					stage->bundle[cntBundle].numImageAnimations++;
				}
				totalImages++;
			}

			if ( totalImages > MAX_IMAGE_ANIMATIONS ) {
				ri.Printf( PRINT_WARNING, "WARNING: ignoring excess images for 'animMap' (found %d, max is %d) in shader '%s'\n",
						totalImages, MAX_IMAGE_ANIMATIONS, shader.name );
			}
		}
		else if ( !Q_stricmp( token, "videoMap" ) )
		{
			token = COM_ParseExt( text, qfalse );
			if ( !token[0] )
			{
				ri.Printf( PRINT_WARNING, "WARNING: missing parameter for 'videoMap' keyword in shader '%s'\n", shader.name );
				return qfalse;
			}
			stage->bundle[cntBundle].videoMapHandle = ri.CIN_PlayCinematic( token, 0, 0, 256, 256, (CIN_loop | CIN_silent | CIN_shader));
			if (stage->bundle[cntBundle].videoMapHandle != -1) {
				stage->bundle[cntBundle].isVideoMap = qtrue;
				stage->bundle[cntBundle].image[0] = tr.scratchImage[stage->bundle[cntBundle].videoMapHandle];
			} else {
				ri.Printf( PRINT_WARNING, "WARNING: could not load '%s' for 'videoMap' keyword in shader '%s'\n", token, shader.name );
			}
		}
		//
		// alphafunc <func>
		//
		else if ( !Q_stricmp( token, "alphaFunc" ) )
		{
			token = COM_ParseExt( text, qfalse );
			if ( !token[0] )
			{
				ri.Printf( PRINT_WARNING, "WARNING: missing parameter for 'alphaFunc' keyword in shader '%s'\n", shader.name );
				return qfalse;
			}

			atestBits = NameToAFunc( token );
		}
		//
		// depthFunc <func>
		//
		else if ( !Q_stricmp( token, "depthfunc" ) )
		{
			token = COM_ParseExt( text, qfalse );

			if ( !token[0] )
			{
				ri.Printf( PRINT_WARNING, "WARNING: missing parameter for 'depthfunc' keyword in shader '%s'\n", shader.name );
				return qfalse;
			}

			if ( !Q_stricmp( token, "lequal" ) )
			{
				depthFuncBits = 0;
			}
			else if ( !Q_stricmp( token, "equal" ) )
			{
				depthFuncBits = GLS_DEPTHFUNC_EQUAL;
			}
			else
			{
				ri.Printf( PRINT_WARNING, "WARNING: unknown depthfunc '%s' in shader '%s'\n", token, shader.name );
				continue;
			}
		}
		//
		// detail
		//
		else if ( !Q_stricmp( token, "detail" ) )
		{
			stage->isDetail = qtrue;
		}
		//
		// blendfunc <srcFactor> <dstFactor>
		// or blendfunc <add|filter|blend>
		//
		else if ( !Q_stricmp( token, "blendfunc" ) )
		{
			token = COM_ParseExt( text, qfalse );
			if ( token[0] == 0 )
			{
				ri.Printf( PRINT_WARNING, "WARNING: missing parm for blendFunc in shader '%s'\n", shader.name );
				continue;
			}
			// check for "simple" blends first
			if ( !Q_stricmp( token, "add" ) ) {
				blendSrcBits = GLS_SRCBLEND_ONE;
				blendDstBits = GLS_DSTBLEND_ONE;
			} else if ( !Q_stricmp( token, "filter" ) ) {
				blendSrcBits = GLS_SRCBLEND_DST_COLOR;
				blendDstBits = GLS_DSTBLEND_ZERO;
			} else if ( !Q_stricmp( token, "blend" ) ) {
				blendSrcBits = GLS_SRCBLEND_SRC_ALPHA;
				blendDstBits = GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA;
			//
			// OPENMOHAA-specific stuff
			//=========================
			} else if ( !Q_stricmp( token, "alphaadd" ) ) {
				blendSrcBits = GLS_SRCBLEND_SRC_ALPHA;
                blendDstBits = GLS_DSTBLEND_ONE;
			//=========================
			} else {
				// complex double blends
				blendSrcBits = NameToSrcBlendMode( token );

				token = COM_ParseExt( text, qfalse );
				if ( token[0] == 0 )
				{
					ri.Printf( PRINT_WARNING, "WARNING: missing parm for blendFunc in shader '%s'\n", shader.name );
					continue;
				}
				blendDstBits = NameToDstBlendMode( token );
			}

			// clear depth mask for blended surfaces
			if ( !depthMaskExplicit )
			{
				depthMaskBits = 0;
			}
		}
		//
		// stage <type>
		//
		else if(!Q_stricmp(token, "stage"))
		{
			token = COM_ParseExt(text, qfalse);
			if(token[0] == 0)
			{
				ri.Printf(PRINT_WARNING, "WARNING: missing parameters for stage in shader '%s'\n", shader.name);
				continue;
			}

			if(!Q_stricmp(token, "diffuseMap"))
			{
				stage->type = ST_DIFFUSEMAP;
			}
			else if(!Q_stricmp(token, "normalMap") || !Q_stricmp(token, "bumpMap"))
			{
				stage->type = ST_NORMALMAP;
				VectorSet4(stage->normalScale, r_baseNormalX->value, r_baseNormalY->value, 1.0f, r_baseParallax->value);
			}
			else if(!Q_stricmp(token, "normalParallaxMap") || !Q_stricmp(token, "bumpParallaxMap"))
			{
				if (r_parallaxMapping->integer)
					stage->type = ST_NORMALPARALLAXMAP;
				else
					stage->type = ST_NORMALMAP;
				VectorSet4(stage->normalScale, r_baseNormalX->value, r_baseNormalY->value, 1.0f, r_baseParallax->value);
			}
			else if(!Q_stricmp(token, "specularMap"))
			{
				stage->type = ST_SPECULARMAP;
				VectorSet4(stage->specularScale, 1.0f, 1.0f, 1.0f, 1.0f);
			}
			else
			{
				ri.Printf(PRINT_WARNING, "WARNING: unknown stage parameter '%s' in shader '%s'\n", token, shader.name);
				continue;
			}
		}
		//
		// specularReflectance <value>
		//
		else if (!Q_stricmp(token, "specularreflectance"))
		{
			token = COM_ParseExt(text, qfalse);
			if ( token[0] == 0 )
			{
				ri.Printf( PRINT_WARNING, "WARNING: missing parameter for specular reflectance in shader '%s'\n", shader.name );
				continue;
			}

			if (r_pbr->integer)
			{
				// interpret specularReflectance < 0.5 as nonmetal
				stage->specularScale[1] = (atof(token) < 0.5f) ? 0.0f : 1.0f;
			}
			else
			{
				stage->specularScale[0] =
				stage->specularScale[1] =
				stage->specularScale[2] = atof( token );
			}
		}
		//
		// specularExponent <value>
		//
		else if (!Q_stricmp(token, "specularexponent"))
		{
			float exponent;

			token = COM_ParseExt(text, qfalse);
			if ( token[0] == 0 )
			{
				ri.Printf( PRINT_WARNING, "WARNING: missing parameter for specular exponent in shader '%s'\n", shader.name );
				continue;
			}

			exponent = atof( token );

			if (r_pbr->integer)
				stage->specularScale[0] = 1.0f - powf(2.0f / (exponent + 2.0), 0.25);
			else
			{
				// Change shininess to gloss
				// Assumes max exponent of 8190 and min of 0, must change here if altered in lightall_fp.glsl
				exponent = CLAMP(exponent, 0.0f, 8190.0f);
				stage->specularScale[3] = (log2f(exponent + 2.0f) - 1.0f) / 12.0f;
			}
		}
		//
		// gloss <value>
		//
		else if (!Q_stricmp(token, "gloss"))
		{
			float gloss;

			token = COM_ParseExt(text, qfalse);
			if ( token[0] == 0 )
			{
				ri.Printf( PRINT_WARNING, "WARNING: missing parameter for gloss in shader '%s'\n", shader.name );
				continue;
			}

			gloss = atof(token);

			if (r_pbr->integer)
				stage->specularScale[0] = 1.0f - exp2f(-3.0f * gloss);
			else
				stage->specularScale[3] = gloss;
		}
		//
		// roughness <value>
		//
		else if (!Q_stricmp(token, "roughness"))
		{
			float roughness;

			token = COM_ParseExt(text, qfalse);
			if (token[0] == 0)
			{
				ri.Printf(PRINT_WARNING, "WARNING: missing parameter for roughness in shader '%s'\n", shader.name);
				continue;
			}

			roughness = atof(token);

			if (r_pbr->integer)
				stage->specularScale[0] = 1.0 - roughness;
			else
			{
				if (roughness >= 0.125)
					stage->specularScale[3] = log2f(1.0f / roughness) / 3.0f;
				else
					stage->specularScale[3] = 1.0f;
			}
		}
		//
		// parallaxDepth <value>
		//
		else if (!Q_stricmp(token, "parallaxdepth"))
		{
			token = COM_ParseExt(text, qfalse);
			if ( token[0] == 0 )
			{
				ri.Printf( PRINT_WARNING, "WARNING: missing parameter for parallaxDepth in shader '%s'\n", shader.name );
				continue;
			}

			stage->normalScale[3] = atof( token );
		}
		//
		// normalScale <xy>
		// or normalScale <x> <y>
		// or normalScale <x> <y> <height>
		//
		else if (!Q_stricmp(token, "normalscale"))
		{
			token = COM_ParseExt(text, qfalse);
			if ( token[0] == 0 )
			{
				ri.Printf( PRINT_WARNING, "WARNING: missing parameter for normalScale in shader '%s'\n", shader.name );
				continue;
			}

			stage->normalScale[0] = atof( token );

			token = COM_ParseExt(text, qfalse);
			if ( token[0] == 0 )
			{
				// one value, applies to X/Y
				stage->normalScale[1] = stage->normalScale[0];
				continue;
			}

			stage->normalScale[1] = atof( token );

			token = COM_ParseExt(text, qfalse);
			if ( token[0] == 0 )
			{
				// two values, no height
				continue;
			}

			stage->normalScale[3] = atof( token );
		}
		//
		// specularScale <rgb> <gloss>
		// or specularScale <metallic> <smoothness> with r_pbr 1
		// or specularScale <r> <g> <b>
		// or specularScale <r> <g> <b> <gloss>
		//
		else if (!Q_stricmp(token, "specularscale"))
		{
			token = COM_ParseExt(text, qfalse);
			if ( token[0] == 0 )
			{
				ri.Printf( PRINT_WARNING, "WARNING: missing parameter for specularScale in shader '%s'\n", shader.name );
				continue;
			}

			stage->specularScale[0] = atof( token );

			token = COM_ParseExt(text, qfalse);
			if ( token[0] == 0 )
			{
				ri.Printf( PRINT_WARNING, "WARNING: missing parameter for specularScale in shader '%s'\n", shader.name );
				continue;
			}

			stage->specularScale[1] = atof( token );

			token = COM_ParseExt(text, qfalse);
			if ( token[0] == 0 )
			{
				if (r_pbr->integer)
				{
					// two values, metallic then smoothness
					float smoothness = stage->specularScale[1];
					stage->specularScale[1] = (stage->specularScale[0] < 0.5f) ? 0.0f : 1.0f;
					stage->specularScale[0] = smoothness;
				}
				else
				{
					// two values, rgb then gloss
					stage->specularScale[3] = stage->specularScale[1];
					stage->specularScale[1] =
					stage->specularScale[2] = stage->specularScale[0];
				}
				continue;
			}

			stage->specularScale[2] = atof( token );

			token = COM_ParseExt(text, qfalse);
			if ( token[0] == 0 )
			{
				// three values, rgb
				continue;
			}

			stage->specularScale[3] = atof( token );

		}
		//
		// rgbGen
		//
		else if ( !Q_stricmp( token, "rgbGen" ) )
		{
			token = COM_ParseExt( text, qfalse );
			if ( token[0] == 0 )
			{
				ri.Printf( PRINT_WARNING, "WARNING: missing parameters for rgbGen in shader '%s'\n", shader.name );
				continue;
			}

			if ( !Q_stricmp( token, "wave" ) )
			{
				ParseWaveForm( text, &stage->rgbWave );
				stage->rgbGen = CGEN_WAVEFORM;
			}
			else if ( !Q_stricmp( token, "const" ) )
			{
				vec3_t	color;

				VectorClear( color );

				ParseVector( text, 3, color );
				stage->constantColor[0] = 255 * color[0];
				stage->constantColor[1] = 255 * color[1];
				stage->constantColor[2] = 255 * color[2];

				stage->rgbGen = CGEN_CONST;
			}
			else if ( !Q_stricmp( token, "identity" ) )
			{
				stage->rgbGen = CGEN_IDENTITY;
			}
			else if ( !Q_stricmp( token, "identityLighting" ) )
			{
				stage->rgbGen = CGEN_IDENTITY_LIGHTING;
			}
			else if ( !Q_stricmp( token, "entity" ) )
			{
				stage->rgbGen = CGEN_ENTITY;
			}
			else if ( !Q_stricmp( token, "oneMinusEntity" ) )
			{
				stage->rgbGen = CGEN_ONE_MINUS_ENTITY;
			}
			else if ( !Q_stricmp( token, "vertex" ) )
			{
				stage->rgbGen = CGEN_VERTEX;
				if ( stage->alphaGen == 0 ) {
					stage->alphaGen = AGEN_VERTEX;
				}
			}
			else if ( !Q_stricmp( token, "exactVertex" ) )
			{
				stage->rgbGen = CGEN_EXACT_VERTEX;
			}
			else if ( !Q_stricmp( token, "vertexLit" ) )
			{
				stage->rgbGen = CGEN_VERTEX_LIT;
				if ( stage->alphaGen == 0 ) {
					stage->alphaGen = AGEN_VERTEX;
				}
			}
			else if ( !Q_stricmp( token, "exactVertexLit" ) )
			{
				stage->rgbGen = CGEN_EXACT_VERTEX_LIT;
			}
			else if ( !Q_stricmp( token, "lightingDiffuse" ) )
			{
				stage->rgbGen = CGEN_LIGHTING_DIFFUSE;
			}
			else if ( !Q_stricmp( token, "oneMinusVertex" ) )
			{
				stage->rgbGen = CGEN_ONE_MINUS_VERTEX;
			}
			//
			// OPENMOHAA-specific stuff
			//=========================
			else if (!Q_stricmp(token, "colorwave"))
			{
				vec3_t v;

				ParseVector(text, 3, v);
				stage->colorConst[0] = v[0] * 255.0;
				stage->colorConst[1] = v[1] * 255.0;
				stage->colorConst[2] = v[2] * 255.0;
				stage->rgbGen = CGEN_MULTIPLY_BY_WAVEFORM;
			}
			else if (!Q_stricmp(token, "global"))
			{
				stage->rgbGen = CGEN_GLOBAL_COLOR;
            }
            else if (!Q_stricmp(token, "fromentity"))
            {
                stage->rgbGen = CGEN_ENTITY;
            }
			else if (!Q_stricmp(token, "fromclient"))
			{
				stage->rgbGen = CGEN_VERTEX;
				if (stage->alphaGen == 0) {
					stage->alphaGen = AGEN_VERTEX;
				}
			}
			else if (!Q_stricmp(token, "lightingGrid"))
			{
				stage->rgbGen = CGEN_LIGHTING_GRID;
			}
			else if (!Q_stricmp(token, "lightingSpherical"))
			{
				stage->rgbGen = CGEN_LIGHTING_SPHERICAL;
			}
			else if (!Q_stricmp(token, "constant"))
			{
				vec3_t	color;

				VectorClear( color );

				ParseVector( text, 3, color );
				stage->constantColor[0] = 255 * color[0];
				stage->constantColor[1] = 255 * color[1];
				stage->constantColor[2] = 255 * color[2];

				stage->rgbGen = CGEN_CONST;
			}
			else if (!Q_stricmp(token, "static"))
			{
				stage->rgbGen = CGEN_STATIC;
				if (stage->alphaGen == AGEN_IDENTITY) {
					stage->alphaGen = AGEN_VERTEX;
				}
			}
			else if (!Q_stricmp(token, "sCoord") || !Q_stricmp(token, "tCoord"))
			{
				if (!Q_stricmp(token, "sCoord")) {
					stage->rgbGen = CGEN_SCOORD;
				}
				else {
					stage->rgbGen = CGEN_TCOORD;
				}

				stage->alphaMin = 0.0;
				stage->alphaMax = 1.0;
				stage->alphaConstMin = 0;
				stage->alphaConst = -1;

				token = COM_ParseExt(text, qfalse);
				if (token[0] == 0) {
					continue;
				}

				stage->alphaMin = atof(token);

				token = COM_ParseExt(text, qfalse);
				if (token[0] == 0) {
					continue;
				}

				stage->alphaMax = atof(token);

				token = COM_ParseExt(text, qfalse);
				if (token[0] == 0) {
					continue;
				}

				stage->alphaConstMin = atof(token) * 255.0;

				token = COM_ParseExt(text, qfalse);
				if (token[0] == 0) {
					ri.Printf(PRINT_WARNING, "WARNING: missing rgbGen sCoord or tCoord parm 'max' in shader '%s'\n", shader.name);
					continue;
				}

				stage->alphaConst = atof(token) * 255.0;
			}
			else if (!Q_stricmp(token, "dot"))
			{
				stage->rgbGen = CGEN_DOT;
				stage->alphaMin = 0.0;
				stage->alphaMax = 1.0;

				token = COM_ParseExt(text, qfalse);
				if (token[0] == 0) {
					continue;
				}
				stage->alphaMin = atof(token);

				token = COM_ParseExt(text, qfalse);
				if (token[0] == 0) {
					continue;
				}
				stage->alphaMax = atof(token);
			}
			else if (!Q_stricmp(token, "oneminusdot"))
			{
				stage->rgbGen = CGEN_ONE_MINUS_DOT;
				stage->alphaMin = 0.0;
				stage->alphaMax = 1.0;

				token = COM_ParseExt(text, qfalse);
				if (token[0] == 0) {
					continue;
				}
				stage->alphaMin = atof(token);

				token = COM_ParseExt(text, qfalse);
				if (token[0] == 0) {
					continue;
				}
				stage->alphaMax = atof(token);
            }
            //=========================
			else
			{
				ri.Printf( PRINT_WARNING, "WARNING: unknown rgbGen parameter '%s' in shader '%s'\n", token, shader.name );
				continue;
			}
		}
		//
		// alphaGen 
		//
		else if ( !Q_stricmp( token, "alphaGen" ) )
		{
			token = COM_ParseExt( text, qfalse );
			if ( token[0] == 0 )
			{
				ri.Printf( PRINT_WARNING, "WARNING: missing parameters for alphaGen in shader '%s'\n", shader.name );
				continue;
			}

			if ( !Q_stricmp( token, "wave" ) )
			{
				ParseWaveForm( text, &stage->alphaWave );
				stage->alphaGen = AGEN_WAVEFORM;
			}
			else if ( !Q_stricmp( token, "const" ) )
			{
				token = COM_ParseExt( text, qfalse );
				stage->constantColor[3] = 255 * atof( token );
				stage->alphaGen = AGEN_CONST;
			}
			else if ( !Q_stricmp( token, "identity" ) )
			{
				stage->alphaGen = AGEN_IDENTITY;
			}
			else if ( !Q_stricmp( token, "entity" ) )
			{
				stage->alphaGen = AGEN_ENTITY;
			}
			else if ( !Q_stricmp( token, "oneMinusEntity" ) )
			{
				stage->alphaGen = AGEN_ONE_MINUS_ENTITY;
			}
			else if ( !Q_stricmp( token, "vertex" ) )
			{
				stage->alphaGen = AGEN_VERTEX;
			}
			else if ( !Q_stricmp( token, "lightingSpecular" ) )
			{
                stage->alphaGen = AGEN_LIGHTING_SPECULAR;
                //
                // OPENMOHAA-specific stuff
                //=========================
				stage->alphaMax = 255;
				stage->specOrigin[0] = -960;
				stage->specOrigin[1] = 1980;
				stage->specOrigin[2] = 96;

				token = COM_ParseExt(text, qfalse);
				if (token[0]) {
					stage->alphaMax = atof(token) * 255.0;
					ParseVector(text, 3, stage->specOrigin);
                }
                //=========================
			}
			else if ( !Q_stricmp( token, "oneMinusVertex" ) )
			{
				stage->alphaGen = AGEN_ONE_MINUS_VERTEX;
			}
			else if ( !Q_stricmp( token, "portal" ) )
			{
				stage->alphaGen = AGEN_PORTAL;
				token = COM_ParseExt( text, qfalse );
				if ( token[0] == 0 )
				{
					shader.portalRange = 256;
					ri.Printf( PRINT_WARNING, "WARNING: missing range parameter for alphaGen portal in shader '%s', defaulting to 256\n", shader.name );
				}
				else
				{
					shader.portalRange = atof( token );
				}
			}
			//
			// OPENMOHAA-specific stuff
			//=========================
			else if (!Q_stricmp(token, "global"))
			{
				stage->alphaGen = AGEN_GLOBAL_ALPHA;
			}
			else if (!Q_stricmp(token, "fromentity"))
			{
				stage->alphaGen = AGEN_ENTITY;
			}
			else if (!Q_stricmp(token, "fromclient"))
			{
				stage->alphaGen = AGEN_VERTEX;
			}
			else if (!Q_stricmp(token, "oneMinusFromClient"))
			{
				stage->alphaGen = AGEN_ONE_MINUS_VERTEX;
			}
			else if (!Q_stricmp(token, "distFade") || !Q_stricmp(token, "oneMinusDistFade")
				|| !Q_stricmp(token, "tikiDistFade") || !Q_stricmp(token, "oneMinusTikiDistFade"))
			{
				if (!Q_stricmp(token, "distFade")) {
					stage->alphaGen = AGEN_DIST_FADE;
				} else if (!Q_stricmp(token, "oneMinusDistFade")) {
					stage->alphaGen = AGEN_ONE_MINUS_DIST_FADE;
				} else if (!Q_stricmp(token, "tikiDistFade")) {
                    stage->alphaGen = AGEN_TIKI_DIST_FADE;
                } else {
                    stage->alphaGen = AGEN_ONE_MINUS_TIKI_DIST_FADE;
                }

				shader.fDistNear = 256.0;
				shader.fDistRange = 256.0;

				token = COM_ParseExt(text, qfalse);
				if (token[0] == 0) {
					continue;
				}
				shader.fDistNear = atof(token);

				token = COM_ParseExt(text, qfalse);
				if (token[0] == 0) {
					continue;
				}
				shader.fDistRange = atof(token);
			}
			else if (!Q_stricmp(token, "dot"))
			{
				stage->alphaMin = 0.0;
				stage->alphaMax = 1.0;
				stage->alphaGen = AGEN_DOT;

				token = COM_ParseExt(text, qfalse);
				if (token[0] == 0) {
					continue;
				}
				stage->alphaMin = atof(token);

				token = COM_ParseExt(text, qfalse);
				if (token[0] == 0) {
					continue;
				}
				stage->alphaMax = atof(token);
			}
			else if (!Q_stricmp(token, "heightFade"))
            {
                stage->alphaMin = 256.0f;
                stage->alphaMax = 512.0f;
                stage->alphaGen = AGEN_HEIGHT_FADE;

                token = COM_ParseExt(text, qfalse);
                if (token[0] == 0) {
                    continue;
                }
                stage->alphaMin = atof(token);

                token = COM_ParseExt(text, qfalse);
                if (token[0] == 0) {
                    continue;
                }
                stage->alphaMax = atof(token);
			}
			else if (!Q_stricmp(token, "oneMinusDot"))
			{
				stage->alphaMin = 0.0;
				stage->alphaMax = 1.0;
				stage->alphaGen = AGEN_ONE_MINUS_DOT;

				token = COM_ParseExt(text, qfalse);
				if (token[0] == 0) {
					continue;
				}
				stage->alphaMin = atof(token);

				token = COM_ParseExt(text, qfalse);
				if (token[0] == 0) {
					continue;
				}
				stage->alphaMax = atof(token);
			}
			else if (!Q_stricmp(token, "dotView"))
			{
				stage->alphaMin = 0.0;
				stage->alphaMax = 1.0;
				stage->alphaGen = AGEN_DOT_VIEW;

				token = COM_ParseExt(text, qfalse);
				if (token[0] == 0) {
					continue;
				}
				stage->alphaMin = atof(token);

				token = COM_ParseExt(text, qfalse);
				if (token[0] == 0) {
					continue;
				}
				stage->alphaMax = atof(token);
			}
			else if (!Q_stricmp(token, "oneMinusDotView"))
			{
				stage->alphaMin = 0.0;
				stage->alphaMax = 1.0;
				stage->alphaGen = AGEN_ONE_MINUS_DOT_VIEW;

				token = COM_ParseExt(text, qfalse);
				if (token[0] == 0) {
					continue;
				}
				stage->alphaMin = atof(token);

				token = COM_ParseExt(text, qfalse);
				if (token[0] == 0) {
					continue;
				}
				stage->alphaMax = atof(token);
			}
			else if (!Q_stricmp(token, "constant"))
			{
				token = COM_ParseExt(text, qfalse);
				stage->constantColor[3] = 255 * atof(token);
				stage->alphaGen = AGEN_CONST;
			}
			else if (!Q_stricmp(token, "skyAlpha"))
			{
				stage->alphaGen = AGEN_SKYALPHA;
			}
			else if (!Q_stricmp(token, "oneMinusSkyAlpha"))
			{
				stage->alphaGen = AGEN_ONE_MINUS_SKYALPHA;
			}
			else if (!Q_stricmp(token, "sCoord") || !Q_stricmp(token, "tCoord"))
			{
				if (!Q_stricmp(token, "sCoord")) {
					stage->alphaGen = AGEN_SCOORD;
				} else {
					stage->alphaGen = AGEN_TCOORD;
				}

				stage->alphaMin = 0.0;
				stage->alphaMax = 1.0;
				stage->alphaConstMin = 0;
				stage->alphaConst = -1;

				token = COM_ParseExt(text, qfalse);
				if (token[0] == 0) {
					continue;
				}
				stage->alphaMin = atof(token);

				token = COM_ParseExt(text, qfalse);
				if (token[0] == 0) {
					continue;
				}
				stage->alphaMax = atof(token);

				token = COM_ParseExt(text, qfalse);
				if (token[0] == 0) {
					continue;
				}
				stage->alphaConstMin = atof(token) * 255.0;

				token = COM_ParseExt(text, qfalse);
				if (token[0] == 0) {
					ri.Printf(PRINT_WARNING, "WARNING: missing alphaGen s or tCoord parm 'max' in shader '%s'\n", shader.name);
					continue;
				}
				stage->alphaConst = atof(token) * 255.0;
			}
			//=========================
			else
			{
				ri.Printf( PRINT_WARNING, "WARNING: unknown alphaGen parameter '%s' in shader '%s'\n", token, shader.name );
				continue;
			}
		}
		//
		// tcGen <function>
		//
		else if ( !Q_stricmp(token, "texgen") || !Q_stricmp( token, "tcGen" ) ) 
		{
			token = COM_ParseExt( text, qfalse );
			if ( token[0] == 0 )
			{
				ri.Printf( PRINT_WARNING, "WARNING: missing texgen parm in shader '%s'\n", shader.name );
				continue;
			}

			if ( !Q_stricmp( token, "environment" ) )
			{
				stage->bundle[cntBundle].tcGen = TCGEN_ENVIRONMENT_MAPPED;
			}
			else if ( !Q_stricmp( token, "environmentmodel" ) )
			{
				// HZM gl2 re-port: MOHAA model-space env map (scope lenses, glasses, facewrap,
				// gloves, leather coats). Was falling to the unknown-parm warning = base texcoords.
				stage->bundle[cntBundle].tcGen = TCGEN_ENVIRONMENT_MAPPED2;
			}
			else if ( !Q_stricmp( token, "lightmap" ) )
			{
				stage->bundle[cntBundle].tcGen = TCGEN_LIGHTMAP;
			}
			else if ( !Q_stricmp( token, "texture" ) || !Q_stricmp( token, "base" ) )
			{
				stage->bundle[cntBundle].tcGen = TCGEN_TEXTURE;
			}
			else if ( !Q_stricmp( token, "vector" ) )
			{
				ParseVector( text, 3, stage->bundle[cntBundle].tcGenVectors[0] );
				ParseVector( text, 3, stage->bundle[cntBundle].tcGenVectors[1] );

				stage->bundle[cntBundle].tcGen = TCGEN_VECTOR;
			}
			else 
			{
				ri.Printf( PRINT_WARNING, "WARNING: unknown texgen parm in shader '%s'\n", shader.name );
			}
		}
		//
		// tcMod <type> <...>
		//
		else if ( !Q_stricmp( token, "tcMod" ) )
		{
			char buffer[1024] = "";

			while ( 1 )
			{
				token = COM_ParseExt( text, qfalse );
				if ( token[0] == 0 )
					break;
				Q_strcat( buffer, sizeof (buffer), token );
				Q_strcat( buffer, sizeof (buffer), " " );
			}

			ParseTexMod( buffer, stage, cntBundle );

			continue;
		}
		//
		// depthmask
		//
		else if ( !Q_stricmp( token, "depthwrite" ) )
		{
			depthMaskBits = GLS_DEPTHMASK_TRUE;
			depthMaskExplicit = qtrue;

			continue;
		}
		//
		// OPENMOHAA-specific stuff
		//=========================
		else if ( !Q_stricmp(token, "animMapOnce") || !Q_stricmp(token, "animMapPhase"))
		{
			qboolean phased;

			if (!Q_stricmp(token, "animMapOnce")) {
				stage->bundle[cntBundle].flags |= BUNDLE_ANIMATE_ONCE;
			}

			phased = !Q_stricmp(token, "animMapPhase");

			token = COM_ParseExt( text, qfalse );
			if ( !token[0] )
			{
				ri.Printf( PRINT_WARNING, "WARNING: missing parameter for 'animMmap' keyword in shader '%s'\n", shader.name );
				return qfalse;
			}
			stage->bundle[cntBundle].imageAnimationSpeed = atof( token );
			stage->bundle[cntBundle].imageAnimationPhase = 0;

			if (phased)
            {
                token = COM_ParseExt(text, qfalse);
                if (!token[0])
                {
                    ri.Printf(PRINT_WARNING, "WARNING: missing phase for 'animMapPhase' keyword in shader '%s'\n", shader.name);
                    return qfalse;
                }

				stage->bundle[cntBundle].imageAnimationPhase = atof(token);
			}
			// parse up to MAX_IMAGE_ANIMATIONS animations
			while ( 1 ) {
				int		num;

				token = COM_ParseExt( text, qfalse );
				if ( !token[0] ) {
					break;
				}
				num = stage->bundle[cntBundle].numImageAnimations;
				if ( num < MAX_IMAGE_ANIMATIONS ) {
					imgFlags_t flags = IMGFLAG_NONE;

					// HZM gl2 re-port (bug-gl2-nopicmip): per-stage flags
					if (!stageNoMipMaps)
						flags |= IMGFLAG_MIPMAP;

					if (!stageNoPicMip)
						flags |= IMGFLAG_PICMIP;

					stage->bundle[cntBundle].image[num] = R_FindImageFile( token, IMGTYPE_COLORALPHA, flags );
					if ( !stage->bundle[cntBundle].image[num] )
					{
						ri.Printf( PRINT_WARNING, "WARNING: R_FindImageFile could not find '%s' in shader '%s'\n", token, shader.name );
						return qfalse;
					}
					stage->bundle[cntBundle].numImageAnimations++;
				}
			}
		}
		else if (!Q_stricmp(token, "nofog"))
		{
			// [HZM 2026-08-31] Was "FIXME: unimplemented". gl1 implements this
			// (renderergl1/tr_shader.c:881). Consumed by RB_SetGlobalFogUniforms via
			// tess.no_global_fog, which already existed for the sun flare.
			shader.noGlobalFog = qtrue;
			continue;
		}
		else if (!Q_stricmp(token, "nosoftparticles"))
		{
			// [HZM soft particles] opt this shader out of the r_softParticles depth fade
			// (RB_SetSoftParticleUniforms). For emitters that must stay hard-edged, e.g. a
			// first-person smoke wisp spawned in front of the viewmodel.
			shader.noSoftParticles = qtrue;
			continue;
		}
		else if (!Q_stricmp(token, "depthmask"))
		{
			depthMaskBits = GLS_DEPTHMASK_TRUE;
			depthMaskExplicit = qtrue;

			continue;
        }
        else if (!Q_stricmp(token, "nodepthwrite") || !Q_stricmp(token, "nodepthmask"))
        {
            depthMaskBits = 0;
            depthMaskExplicit = qtrue;

            continue;
        }
        else if (!Q_stricmp(token, "nocolorwrite") || !Q_stricmp(token, "nocolormask"))
        {
            colorBits = GLS_COLOR_NOMASK;

            continue;
        }
		else if (!Q_stricmp(token, "noDepthTest"))
		{
			depthFuncBits = GLS_DEPTHTEST_DISABLE;
			continue;
        }
        else if (!Q_stricmp(token, "nextBundle"))
        {
			//if (!qglActiveTextureARB) {
			//	ri.Printf(PRINT_ALL, "WARNING: " PRODUCT_NAME " requires a video card with multitexturing capability\n");
			//	return qfalse;
			//}

			// HZM gl2 re-port (bug-gl2-nextbundle2): store the combine mode like
			// gl1 (tr_shader.c:1682-1687); consumed by CreateMultistageFromBundle
			// and the generic-program second bundle (u_Texture1Env).
			token = COM_ParseExt(text, qfalse);
			if (token[0] && !Q_stricmp(token, "add")) {
				stage->multitextureEnv = GL_ADD;
			} else {
				stage->multitextureEnv = GL_MODULATE;
			}

			cntBundle++;
			// HZM gl2 re-port (bug-gl2-texmods-null): was '>', which let
			// cntBundle reach NUM_TEXTURE_BUNDLES and index bundle[] out of
			// bounds (latent in gl1 too; retail never exceeds 2 bundles)
			if (cntBundle >= NUM_TEXTURE_BUNDLES) {
				ri.Printf(PRINT_WARNING, "WARNING: too many nextBundle commands in shader '%s'\n", shader.name);
				return qfalse;
			}
            continue;
        }
		//
		// Added in 2.0
		//
		else if (!Q_stricmp(token, "ifCvar") || !Q_stricmp(token, "ifCvarnot"))
		{
			cvar_t* var;
			qboolean isNot = token[6] != 0;
			qboolean evaluatedValue;

			token = COM_ParseExt(text, qfalse);
			if (!token[0]) {
				ri.Printf(PRINT_WARNING, "WARNING: missing cvar name in %s", isNot ? "ifCvarnot" : "ifCvar");
				return qfalse;
			}

			var = ri.Cvar_Get(token, "0", 0);

			token = COM_ParseExt(text, qfalse);
			if (!token[0]) {
				ri.Printf(PRINT_WARNING, "WARNING: missing cvar value in %s", isNot ? "ifCvarnot" : "ifCvar");
				return qfalse;
			}

			if (atof(token) != var->value) {
				evaluatedValue = qfalse;
			} else if (var->value) {
				evaluatedValue = qtrue;
			} else if (token[0] == '0' || (token[0] == '.' && token[1] == '0')) {
				evaluatedValue = qtrue;
			} else if (!Q_stricmp(var->string, token)) {
				evaluatedValue = qtrue;
			} else {
				return qfalse;
			}

			shouldProcess &= evaluatedValue ^ isNot;
        }
        //=========================
		else
		{
			ri.Printf( PRINT_WARNING, "WARNING: unknown parameter '%s' in shader '%s'\n", token, shader.name );
			return qfalse;
		}
	}

	//
	// OPENMOHAA-specific stuff
	//=========================
	// HZM gl2 re-port (bug-gl2-foliage): drop stages whose ifCvar / ifCvarnot
	// condition failed, matching gl1 tr_shader.c. shouldProcess was computed by
	// the ifCvar handler above but never applied, so ALL conditional stages
	// rendered at once - e.g. TAwintershrub's r_blendtrees depth-prime stage
	// (nocolorwrite + alphaFunc GE_FOLIAGE1) drew the whole quad opaque ->
	// snowy bushes rendered as solid black rectangles on t2l1.
	if (!shouldProcess) {
		stage->active = qfalse;
		stage->rgbGen = CGEN_BAD;
		return qtrue;
	}
	//=========================

	//
	// if cgen isn't explicitly specified, use either identity or identitylighting
	//
	if ( stage->rgbGen == CGEN_BAD ) {
		// HZM gl2 re-port Fix 1: multi-bundle / $lightmap stages with no explicit rgbGen must
		// default to CGEN_IDENTITY (texture-as-authored), matching gl1 tr_shader.c. Without this
		// leading branch decal/mark/bullethole stages render black/red under gl2.
		if ( cntBundle || stage->bundle[0].isLightmap ) {
			stage->rgbGen = CGEN_IDENTITY;
		} else if ( blendSrcBits == 0 ||
			blendSrcBits == GLS_SRCBLEND_ONE ||
			blendSrcBits == GLS_SRCBLEND_SRC_ALPHA ) {
			stage->rgbGen = CGEN_IDENTITY_LIGHTING;
		} else {
			stage->rgbGen = CGEN_IDENTITY;
		}
	}


	//
	// implicitly assume that a GL_ONE GL_ZERO blend mask disables blending
	//
	if ( ( blendSrcBits == GLS_SRCBLEND_ONE ) && 
		 ( blendDstBits == GLS_DSTBLEND_ZERO ) )
	{
		blendDstBits = blendSrcBits = 0;
		depthMaskBits = GLS_DEPTHMASK_TRUE;
	}

	// decide which agens we can skip
	if ( stage->alphaGen == AGEN_IDENTITY ) {
		if ( stage->rgbGen == CGEN_IDENTITY
			|| stage->rgbGen == CGEN_LIGHTING_DIFFUSE ) {
			stage->alphaGen = AGEN_SKIP;
		}
	}

	//
	// compute state bits
	//
	stage->stateBits = depthMaskBits | 
		               blendSrcBits | blendDstBits | 
					   atestBits | 
					   depthFuncBits;

	return qtrue;
}

/*
===============
ParseDeform

deformVertexes wave <spread> <waveform> <base> <amplitude> <phase> <frequency>
deformVertexes normal <frequency> <amplitude>
deformVertexes move <vector> <waveform> <base> <amplitude> <phase> <frequency>
deformVertexes bulge <bulgeWidth> <bulgeHeight> <bulgeSpeed>
deformVertexes projectionShadow
deformVertexes autoSprite
deformVertexes autoSprite2
deformVertexes text[0-7]
===============
*/
static void ParseDeform( char **text ) {
	char	*token;
	deformStage_t	*ds;

	token = COM_ParseExt( text, qfalse );
	if ( token[0] == 0 )
	{
		ri.Printf( PRINT_WARNING, "WARNING: missing deform parm in shader '%s'\n", shader.name );
		return;
	}

	if ( shader.numDeforms == MAX_SHADER_DEFORMS ) {
		ri.Printf( PRINT_WARNING, "WARNING: MAX_SHADER_DEFORMS in '%s'\n", shader.name );
		return;
	}

	ds = &shader.deforms[ shader.numDeforms ];
	shader.numDeforms++;

	if ( !Q_stricmp( token, "projectionShadow" ) ) {
		ds->deformation = DEFORM_PROJECTION_SHADOW;
		return;
	}

	if ( !Q_stricmp( token, "autosprite" ) ) {
		ds->deformation = DEFORM_AUTOSPRITE;
		return;
	}

	if ( !Q_stricmp( token, "autosprite2" ) ) {
		ds->deformation = DEFORM_AUTOSPRITE2;
		return;
	}

	if ( !Q_stricmpn( token, "text", 4 ) ) {
		int		n;
		
		n = token[4] - '0';
		if ( n < 0 || n > 7 ) {
			n = 0;
		}
		ds->deformation = DEFORM_TEXT0 + n;
		return;
	}

	if ( !Q_stricmp( token, "bulge" ) )	{
		token = COM_ParseExt( text, qfalse );
		if ( token[0] == 0 )
		{
			ri.Printf( PRINT_WARNING, "WARNING: missing deformVertexes bulge parm in shader '%s'\n", shader.name );
			return;
		}
		ds->bulgeWidth = atof( token );

		token = COM_ParseExt( text, qfalse );
		if ( token[0] == 0 )
		{
			ri.Printf( PRINT_WARNING, "WARNING: missing deformVertexes bulge parm in shader '%s'\n", shader.name );
			return;
		}
		ds->bulgeHeight = atof( token );

		token = COM_ParseExt( text, qfalse );
		if ( token[0] == 0 )
		{
			ri.Printf( PRINT_WARNING, "WARNING: missing deformVertexes bulge parm in shader '%s'\n", shader.name );
			return;
		}
		ds->bulgeSpeed = atof( token );

		ds->deformation = DEFORM_BULGE;
		return;
	}

	if ( !Q_stricmp( token, "wave" ) )
	{
		token = COM_ParseExt( text, qfalse );
		if ( token[0] == 0 )
		{
			ri.Printf( PRINT_WARNING, "WARNING: missing deformVertexes parm in shader '%s'\n", shader.name );
			return;
		}

		if ( atof( token ) != 0 )
		{
			ds->deformationSpread = 1.0f / atof( token );
		}
		else
		{
			ds->deformationSpread = 100.0f;
			ri.Printf( PRINT_WARNING, "WARNING: illegal div value of 0 in deformVertexes command for shader '%s'\n", shader.name );
		}

		ParseWaveForm( text, &ds->deformationWave );
		ds->deformation = DEFORM_WAVE;
		return;
	}

	if ( !Q_stricmp( token, "normal" ) )
	{
		token = COM_ParseExt( text, qfalse );
		if ( token[0] == 0 )
		{
			ri.Printf( PRINT_WARNING, "WARNING: missing deformVertexes parm in shader '%s'\n", shader.name );
			return;
		}
		ds->deformationWave.amplitude = atof( token );

		token = COM_ParseExt( text, qfalse );
		if ( token[0] == 0 )
		{
			ri.Printf( PRINT_WARNING, "WARNING: missing deformVertexes parm in shader '%s'\n", shader.name );
			return;
		}
		ds->deformationWave.frequency = atof( token );

		ds->deformation = DEFORM_NORMALS;
		return;
	}

	if ( !Q_stricmp( token, "move" ) ) {
		int		i;

		for ( i = 0 ; i < 3 ; i++ ) {
			token = COM_ParseExt( text, qfalse );
			if ( token[0] == 0 ) {
				ri.Printf( PRINT_WARNING, "WARNING: missing deformVertexes parm in shader '%s'\n", shader.name );
				return;
			}
			ds->moveVector[i] = atof( token );
		}

		ParseWaveForm( text, &ds->deformationWave );
		ds->deformation = DEFORM_MOVE;
		return;
	}

	//
	// OPENMOHAA-specific stuff
	//=========================
	if (!Q_stricmp(token, "lightglow")) {
		ds->deformation = DEFORM_LIGHTGLOW;
		return;
	}

	if (!Q_stricmp(token, "flap")) {
		texDirection_t coordDirection;

		token = COM_ParseExt(text, qfalse);
		if (token[0] == 's') {
			coordDirection = USE_S_COORDS;
		}
		else if (token[0] == 't') {
			coordDirection = USE_T_COORDS;
		}
		else {
			ri.Printf(PRINT_WARNING, "WARNING: deformVertexes flap requires 's' or 't' in shader '%s'\n", shader.name);
			return;
		}

		token = COM_ParseExt(text, qfalse);
		if (token[0] == 0) {
			ri.Printf(PRINT_WARNING, "WARNING: missing deformVertexes flap parm in shader '%s'\n", shader.name);
			return;
		}

		if (!atof(token)) {
			ds->deformationSpread = 100.0;
			ri.Printf(PRINT_WARNING, "WARNING: illegal div value of 0 in deformVertexes command for shader '%s'\n", shader.name);
		}
		else {
			ds->deformationSpread = 1.0 / atof(token);
		}

		ParseWaveForm(text, &ds->deformationWave);

		if (coordDirection == USE_T_COORDS) {
			ds->deformation = DEFORM_FLAP_T;
		}
		else {
			ds->deformation = DEFORM_FLAP_S;
		}

		token = COM_ParseExt(text, qfalse);
		if (token[0] == 0)
		{
			ds->bulgeWidth = 0.0;
			ds->bulgeHeight = 1.0;
			return;
		}
		ds->bulgeWidth = atof(token);

		token = COM_ParseExt(text, qfalse);
		if (token[0] == 0)
		{
			ri.Printf(PRINT_WARNING, "WARNING: missing deformVertexes parm 'max' in shader '%s'\n\n", shader.name);
			return;
		}
		ds->bulgeHeight = atof(token);
		return;
    }
    //=========================

	ri.Printf( PRINT_WARNING, "WARNING: unknown deformVertexes subtype '%s' found in shader '%s'\n", token, shader.name );
}


/*
===============
R_HZM_SkyHDWanted

HZM gl2 [2026-09-25] the one r_skyHD predicate, shared by the box hook (ParseSkyParms, layers = qfalse) and the
layer hook (R_HZM_SkyHDLayers, layers = qtrue): -1 = auto (HZM_SKYHD_AUTO for boxes, HZM_SKYHD_LAYERS_AUTO for
layers - bug-2942 split them so the user-approved clouds ship without the untested boxes), 0 = original, 1 = HD.
Compare mode loads HD regardless.
===============
*/
static qboolean R_HZM_SkyHDWanted( qboolean layers ) {
	int autoOn = layers ? HZM_SKYHD_LAYERS_AUTO : HZM_SKYHD_AUTO;

	if ( r_skyHDCompare && r_skyHDCompare->integer ) {
		return qtrue;
	}
	return ( r_skyHD && ( r_skyHD->integer > 0 || ( r_skyHD->integer < 0 && autoOn ) ) ) ? qtrue : qfalse;
}

/*
===============
ParseSkyParms

skyParms <outerbox> <cloudheight> <innerbox>
===============
*/
static void ParseSkyParms( char **text ) {
	char		*token;
	static char	*suf[6] = {"rt", "bk", "lf", "ft", "up", "dn"};
	char		pathname[MAX_QPATH];
	int			i;
	// HZM gl2 [user 2026-08-02] bug-1295 - IMGFLAG_NO_COMPRESSION on sky faces, matching gl1.
	// gl1 loads these through R_FindImageFileOld with its shader_force32bit argument
	// (renderergl1/tr_shader.c:2033), i.e. UNCOMPRESSED 32-bit. gl2 omitted the flag, so with
	// r_ext_compressed_textures at its default 1 the six sky faces were handed to DXT1 - and DXT1's
	// 4x4 blocks with RGB565 endpoints are worst exactly where a sky is smoothest, which reads as
	// "low res and pixelated" (user, on t3l1 and e2l1). The map's own 512x512 source is already
	// coarse; compressing it on top is the part that is ours to fix. Costs VRAM, not quality.
	imgFlags_t imgFlags = IMGFLAG_MIPMAP | IMGFLAG_PICMIP | IMGFLAG_NO_COMPRESSION;

	// outerbox
	token = COM_ParseExt( text, qfalse );
	if ( token[0] == 0 ) {
		ri.Printf( PRINT_WARNING, "WARNING: 'skyParms' missing parameter in shader '%s'\n", shader.name );
		return;
	}
	if ( strcmp( token, "-" ) ) {
		// HZM gl2 [2026-09-25] r_skyHD - HD replacement boxes under NEW names env/hzmhd/<box minus "env/">_<suf>.tga
		// (docs/tools/gen_sky_hd.py). New names cannot lose a .dds/.jpg/.tga or pak-order contest (TRAPS T6).
		// ALL SIX OR NONE: existence is checked for the whole set before anything is loaded, so a partial set
		// never mixes into a box. Same flags as the original (bug-1295: uncompressed). One log line per box.
		// r_skyHDCompare (test only): keep BOTH sets for this map - original in outerbox, HD in outerboxAlt.
		int			compare = r_skyHDCompare ? r_skyHDCompare->integer : 0;
		qboolean	wantHD = R_HZM_SkyHDWanted( qfalse );
		char		hdbase[MAX_QPATH];
		image_t		*hd[6];
		int			haveHD = 0;

		Com_Memset( hd, 0, sizeof( hd ) );
		if ( wantHD ) {
			Com_sprintf( hdbase, sizeof( hdbase ), "env/hzmhd/%s", Q_stricmpn( token, "env/", 4 ) ? token : token + 4 );
			for ( i = 0; i < 6; i++ ) {
				Com_sprintf( pathname, sizeof( pathname ), "%s_%s.tga", hdbase, suf[i] );
				if ( ri.FS_ReadFileEx( pathname, NULL, qtrue ) > 0 ) {
					haveHD++;
				}
			}
			if ( haveHD == 6 ) {
				for ( i = 0; i < 6; i++ ) {
					Com_sprintf( pathname, sizeof( pathname ), "%s_%s.tga", hdbase, suf[i] );
					hd[i] = R_FindImageFile( ( char * ) pathname, IMGTYPE_COLORALPHA, imgFlags | IMGFLAG_CLAMPTOEDGE );
					if ( !hd[i] ) {
						haveHD = -1;	// present but undecodable: keep the original, say so
						break;
					}
				}
			}
		}

		if ( haveHD == 6 && !compare ) {
			for ( i = 0; i < 6; i++ ) {
				shader.sky.outerbox[i] = hd[i];
			}
		} else {
			for (i=0 ; i<6 ; i++) {
				Com_sprintf( pathname, sizeof(pathname), "%s_%s.tga"
					, token, suf[i] );
				shader.sky.outerbox[i] = R_FindImageFile( ( char * ) pathname, IMGTYPE_COLORALPHA, imgFlags | IMGFLAG_CLAMPTOEDGE );

				if ( !shader.sky.outerbox[i] ) {
					shader.sky.outerbox[i] = tr.defaultImage;
				}
			}
			if ( haveHD == 6 ) {
				for ( i = 0; i < 6; i++ ) {
					shader.sky.outerboxAlt[i] = hd[i];
				}
				tr.skyHDCompareBoxes++;
			}
		}

		if ( haveHD == 6 ) {
			ri.Printf( PRINT_ALL, "HZM skyHD: %s -> HD %s 6/6 %dx%d%s\n", token, hdbase, hd[0]->uploadWidth, hd[0]->uploadHeight,
				compare ? " + original [compare: r_skyHDCompare 1 = HD, 2 = original]" : "" );
		} else if ( haveHD < 0 ) {
			ri.Printf( PRINT_WARNING, "HZM skyHD: %s -> original (HD set %s present but failed to load)\n", token, hdbase );
		} else if ( wantHD && haveHD ) {
			ri.Printf( PRINT_WARNING, "HZM skyHD: %s -> original (HD set incomplete %d/6 - all six or none)\n", token, haveHD );
		} else if ( wantHD ) {
			ri.Printf( PRINT_ALL, "HZM skyHD: %s -> original (no HD set)\n", token );
		} else {
			ri.Printf( PRINT_ALL, "HZM skyHD: %s -> original (r_skyHD %s)\n", token, r_skyHD ? r_skyHD->string : "?" );
		}
	}

	// cloudheight
	token = COM_ParseExt( text, qfalse );
	if ( token[0] == 0 ) {
		ri.Printf( PRINT_WARNING, "WARNING: 'skyParms' missing parameter in shader '%s'\n", shader.name );
		return;
	}
	shader.sky.cloudHeight = atof( token );
	if ( !shader.sky.cloudHeight ) {
		shader.sky.cloudHeight = 512;
	}
	R_InitSkyTexCoords( shader.sky.cloudHeight );


	// innerbox
	token = COM_ParseExt( text, qfalse );
	if ( token[0] == 0 ) {
		ri.Printf( PRINT_WARNING, "WARNING: 'skyParms' missing parameter in shader '%s'\n", shader.name );
		return;
	}
	if ( strcmp( token, "-" ) ) {
		for (i=0 ; i<6 ; i++) {
			Com_sprintf( pathname, sizeof(pathname), "%s_%s.tga"
				, token, suf[i] );
			shader.sky.innerbox[i] = R_FindImageFile( ( char * ) pathname, IMGTYPE_COLORALPHA, imgFlags );
			if ( !shader.sky.innerbox[i] ) {
				shader.sky.innerbox[i] = tr.defaultImage;
			}
		}
	}

	shader.isSky = qtrue;
}


/*
=================
ParseSort
=================
*/
void ParseSort( char **text ) {
	char	*token;

	token = COM_ParseExt( text, qfalse );
	if ( token[0] == 0 ) {
		ri.Printf( PRINT_WARNING, "WARNING: missing sort parameter in shader '%s'\n", shader.name );
		return;
	}

	if ( !Q_stricmp( token, "portal" ) ) {
		shader.sort = SS_PORTAL;
	} else if ( !Q_stricmp( token, "sky" ) ) {
		shader.sort = SS_ENVIRONMENT;
	} else if ( !Q_stricmp( token, "opaque" ) ) {
		shader.sort = SS_OPAQUE;
	}else if ( !Q_stricmp( token, "decal" ) ) {
		shader.sort = SS_DECAL;
	} else if ( !Q_stricmp( token, "seeThrough" ) ) {
		shader.sort = SS_SEE_THROUGH;
	} else if ( !Q_stricmp( token, "banner" ) ) {
		shader.sort = SS_BANNER;
	} else if ( !Q_stricmp( token, "additive" ) ) {
		shader.sort = SS_BLEND1;
	} else if ( !Q_stricmp( token, "nearest" ) ) {
		shader.sort = SS_NEAREST;
	} else if ( !Q_stricmp( token, "underwater" ) ) {
		shader.sort = SS_UNDERWATER;
	} else {
		shader.sort = atof( token );
	}
}



// this table is also present in q3map

typedef struct {
	char	*name;
	int		clearSolid, surfaceFlags, contents;
} infoParm_t;

infoParm_t	infoParms[] = {
	// server relevant contents
	{"water",		1,	0,	CONTENTS_WATER },
	{"slime",		1,	0,	CONTENTS_SLIME },		// mildly damaging
	{"lava",		1,	0,	CONTENTS_LAVA },		// very damaging
	{"playerclip",	1,	0,	CONTENTS_PLAYERCLIP },
	{"monsterclip",	1,	0,	CONTENTS_MONSTERCLIP },
	{"nodrop",		1,	0,	CONTENTS_NODROP },		// don't drop items or leave bodies (death fog, lava, etc)
	{"nonsolid",	1,	SURF_NONSOLID,	0},						// clears the solid flag

	// utility relevant attributes
	{"origin",		1,	0,	CONTENTS_ORIGIN },		// center of rotating brushes
	{"trans",		0,	0,	CONTENTS_TRANSLUCENT },	// don't eat contained surfaces
	{"detail",		0,	0,	CONTENTS_DETAIL },		// don't include in structural bsp
	{"structural",	0,	0,	CONTENTS_STRUCTURAL },	// force into structural bsp even if trnas
	{"areaportal",	1,	0,	CONTENTS_AREAPORTAL },	// divides areas
	{"clusterportal", 1,0,  CONTENTS_CLUSTERPORTAL },	// for bots
	{"donotenter",  1,  0,  CONTENTS_DONOTENTER },		// for bots

	{"fog",			1,	0,	CONTENTS_FOG},			// carves surfaces entering
	{"sky",			0,	SURF_SKY,		0 },		// emit light from an environment map
	{"lightfilter",	0,	SURF_LIGHTFILTER, 0 },		// filter light going through it
	{"alphashadow",	0,	SURF_ALPHASHADOW, 0 },		// test light on a per-pixel basis
	{"hint",		0,	SURF_HINT,		0 },		// use as a primary splitter

	// server attributes
	{"slick",		0,	SURF_SLICK,		0 },
	{"noimpact",	0,	SURF_NOIMPACT,	0 },		// don't make impact explosions or marks
	{"nomarks",		0,	SURF_NOMARKS,	0 },		// don't make impact marks, but still explode
	{"ladder",		0,	SURF_LADDER,	0 },
	{"nodamage",	0,	SURF_NODAMAGE,	0 },
	{"metalsteps",	0,	SURF_METALSTEPS,0 },
	{"flesh",		0,	SURF_FLESH,		0 },
	{"nosteps",		0,	SURF_NOSTEPS,	0 },

	// drawsurf attributes
	{"nodraw",		0,	SURF_NODRAW,	0 },	// don't generate a drawsurface (or a lightmap)
	{"pointlight",	0,	SURF_POINTLIGHT, 0 },	// sample lighting at vertexes
	{"nolightmap",	0,	SURF_NOLIGHTMAP,0 },	// don't generate a lightmap
	{"nodlight",	0,	SURF_NODLIGHT, 0 },		// don't ever add dynamic lights
	{"dust",		0,	SURF_DUST, 0}			// leave a dust trail when walking on this surface
};


/*
===============
ParseSurfaceParm

surfaceparm <name>
===============
*/
static void ParseSurfaceParm( char **text ) {
	char	*token;
	int		numInfoParms = ARRAY_LEN( infoParms );
	int		i;

	token = COM_ParseExt( text, qfalse );
	for ( i = 0 ; i < numInfoParms ; i++ ) {
		if ( !Q_stricmp( token, infoParms[i].name ) ) {
			shader.surfaceFlags |= infoParms[i].surfaceFlags;
			shader.contentFlags |= infoParms[i].contents;
#if 0
			if ( infoParms[i].clearSolid ) {
				si->contents &= ~CONTENTS_SOLID;
			}
#endif
			break;
		}
	}
}

/*
=================
ParseShader

The current text pointer is at the explicit text definition of the
shader.  Parse it into the global shader variable.  Later functions
will optimize it.
=================
*/
static qboolean ParseShader( char **text )
{
	char *token;
	int s;

	s = 0;

	token = COM_ParseExt( text, qtrue );
	if ( token[0] != '{' )
	{
		ri.Printf( PRINT_WARNING, "WARNING: expecting '{', found '%s' instead in shader '%s'\n", token, shader.name );
		return qfalse;
	}

	while ( 1 )
	{
		token = COM_ParseExt( text, qtrue );
		if ( !token[0] )
		{
			ri.Printf( PRINT_WARNING, "WARNING: no concluding '}' in shader %s\n", shader.name );
			return qfalse;
		}

		// end of shader definition
		if ( token[0] == '}' )
		{
			break;
		}
		// stage definition
		else if ( token[0] == '{' )
		{
			if ( s >= MAX_SHADER_STAGES ) {
				ri.Printf( PRINT_WARNING, "WARNING: too many stages in shader %s (max is %i)\n", shader.name, MAX_SHADER_STAGES );
				return qfalse;
			}

			if ( !ParseStage( &stages[s], text ) )
			{
				return qfalse;
			}
			//
			// OPENMOHAA-specific stuff
			//=========================
			// HZM gl2 re-port (bug-gl2-foliage): a stage skipped by ifCvar /
			// ifCvarnot comes back with rgbGen == CGEN_BAD (gl1 convention).
			// gl1 leaves the slot as an inactive hole; gl2's FinishShader
			// break-on-inactive loops would strand any stages after such a
			// hole, so instead wipe the slot back to its InitShaderEx state
			// and let the next stage reuse it.
			if ( stages[s].rgbGen == CGEN_BAD ) {
				int b;

				Com_Memset( &stages[s], 0, sizeof( stages[s] ) );
				// HZM gl2 re-port (bug-gl2-texmods-null): re-wire EVERY bundle's
				// texMod storage, exactly like InitShaderEx
				for ( b = 0; b < NUM_TEXTURE_BUNDLES; b++ ) {
					stages[s].bundle[b].texMods = texMods[s][b];
				}
				VectorSet4( stages[s].normalScale, 0.0f, 0.0f, 0.0f, 0.0f );
				if ( r_pbr->integer ) {
					stages[s].specularScale[0] = r_baseGloss->value;
				} else {
					stages[s].specularScale[0] =
					stages[s].specularScale[1] =
					stages[s].specularScale[2] = r_baseSpecular->value;
					stages[s].specularScale[3] = r_baseGloss->value;
				}
				continue;
			}
			//=========================
			stages[s].active = qtrue;
			s++;

			continue;
		}
		// HZM gl2 [2026-09-26] S3b: qer_hzmSoftEdge <dist> - see shader_t.hzmSoftEdge. Parsed BEFORE the generic qer
		// skip below, which is exactly what keeps it invisible to gl1 and to every older gl2.
		else if ( !Q_stricmp( token, "qer_hzmSoftEdge" ) ) {
			token = COM_ParseExt( text, qfalse );
			shader.hzmSoftEdge = token[0] ? atof( token ) : 0.0f;
			if ( !( shader.hzmSoftEdge > 0.0f ) ) {
				shader.hzmSoftEdge = 0.0f;
			}
			continue;
		}
		// skip stuff that only the QuakeEdRadient needs
		else if ( !Q_stricmpn( token, "qer", 3 ) ) {
			SkipRestOfLine( text );
			continue;
		}
		// sun parms
		else if ( !Q_stricmp( token, "q3map_sun" ) || !Q_stricmp( token, "q3map_sunExt" ) || !Q_stricmp( token, "q3gl2_sun" ) ) {
			float	a, b;
			qboolean isGL2Sun = qfalse;

			if (!Q_stricmp( token, "q3gl2_sun" ) && r_sunShadows->integer )
			{
				isGL2Sun = qtrue;
				tr.sunShadows = qtrue;
			}

			token = COM_ParseExt( text, qfalse );
			tr.sunLight[0] = atof( token );
			token = COM_ParseExt( text, qfalse );
			tr.sunLight[1] = atof( token );
			token = COM_ParseExt( text, qfalse );
			tr.sunLight[2] = atof( token );
			
			VectorNormalize( tr.sunLight );

			token = COM_ParseExt( text, qfalse );
			a = atof( token );
			VectorScale( tr.sunLight, a, tr.sunLight);

			token = COM_ParseExt( text, qfalse );
			a = atof( token );
			a = a / 180 * M_PI;

			token = COM_ParseExt( text, qfalse );
			b = atof( token );
			b = b / 180 * M_PI;

			tr.sunDirection[0] = cos( a ) * cos( b );
			tr.sunDirection[1] = sin( a ) * cos( b );
			tr.sunDirection[2] = sin( b );

			if (isGL2Sun)
			{
				token = COM_ParseExt( text, qfalse );
				tr.sunShadowScale = atof(token);

				// parse twice, since older shaders may include mapLightScale before sunShadowScale
				if (token[0]) {
					token = COM_ParseExt( text, qfalse );
					if (token[0]) {
						tr.sunShadowScale = atof(token);
					}
				}
			}

			if (token[0]) {
				SkipRestOfLine( text );
			}
			continue;
		}
		// tonemap parms
		else if ( !Q_stricmp( token, "q3gl2_tonemap" ) ) {
			token = COM_ParseExt( text, qfalse );
			tr.toneMinAvgMaxLevel[0] = atof( token );
			token = COM_ParseExt( text, qfalse );
			tr.toneMinAvgMaxLevel[1] = atof( token );
			token = COM_ParseExt( text, qfalse );
			tr.toneMinAvgMaxLevel[2] = atof( token );

			token = COM_ParseExt( text, qfalse );
			tr.autoExposureMinMax[0] = atof( token );
			token = COM_ParseExt( text, qfalse );
			tr.autoExposureMinMax[1] = atof( token );

			SkipRestOfLine( text );
			continue;
		}
		else if ( !Q_stricmp( token, "deformVertexes" ) ) {
			ParseDeform( text );
			continue;
		}
		else if ( !Q_stricmp( token, "tesssize" ) ) {
			SkipRestOfLine( text );
			continue;
		}
		else if ( !Q_stricmp( token, "clampTime" ) ) {
			token = COM_ParseExt( text, qfalse );
			if (token[0]) {
				shader.clampTime = atof(token);
			}
		}
		// skip stuff that only the q3map needs
		else if ( !Q_stricmpn( token, "q3map", 5 ) ) {
			SkipRestOfLine( text );
			continue;
		}
		// skip stuff that only q3map or the server needs
		else if ( !Q_stricmp( token, "surfaceParm" ) ) {
			ParseSurfaceParm( text );
			continue;
		}
		// no mip maps
		else if ( !Q_stricmp( token, "nomipmaps" ) )
		{
			shader.noMipMaps = qtrue;
			shader.noPicMip = qtrue;
			continue;
		}
		// no picmip adjustment
		else if ( !Q_stricmp( token, "nopicmip" ) )
		{
			shader.noPicMip = qtrue;
			continue;
		}
		// polygonOffset
		else if ( !Q_stricmp( token, "polygonOffset" ) )
		{
			shader.polygonOffset = qtrue;
			continue;
		}
		// entityMergable, allowing sprite surfaces from multiple entities
		// to be merged into one batch.  This is a savings for smoke
		// puffs and blood, but can't be used for anything where the
		// shader calcs (not the surface function) reference the entity color or scroll
		else if ( !Q_stricmp( token, "entityMergable" ) )
		{
			shader.entityMergable = qtrue;
			continue;
		}
		// fogParms
		else if ( !Q_stricmp( token, "fogParms" ) ) 
		{
			if ( !ParseVector( text, 3, shader.fogParms.color ) ) {
				return qfalse;
			}

			if ( r_greyscale->integer )
			{
				float luminance;

				luminance = LUMA( shader.fogParms.color[0], shader.fogParms.color[1], shader.fogParms.color[2] );
				VectorSet( shader.fogParms.color, luminance, luminance, luminance );
			}
			else if ( r_greyscale->value )
			{
				float luminance;

				luminance = LUMA( shader.fogParms.color[0], shader.fogParms.color[1], shader.fogParms.color[2] );
				shader.fogParms.color[0] = LERP( shader.fogParms.color[0], luminance, r_greyscale->value );
				shader.fogParms.color[1] = LERP( shader.fogParms.color[1], luminance, r_greyscale->value );
				shader.fogParms.color[2] = LERP( shader.fogParms.color[2], luminance, r_greyscale->value );
			}

			token = COM_ParseExt( text, qfalse );
			if ( !token[0] ) 
			{
				ri.Printf( PRINT_WARNING, "WARNING: missing parm for 'fogParms' keyword in shader '%s'\n", shader.name );
				continue;
			}
			shader.fogParms.depthForOpaque = atof( token );

			// skip any old gradient directions
			SkipRestOfLine( text );
			continue;
		}
		// portal
		else if ( !Q_stricmp(token, "portal") )
		{
			shader.sort = SS_PORTAL;
			shader.isPortal = qtrue;
			continue;
		}
		// HZM gl2 re-port (bug-gl2-portalsky): portal into the 3D skybox room
		// (textures/common/skyportal), mirrors gl1 (gl1 tr_shader.c:2378-2383).
		// Used to fall into the unknown-parameter branch -> default shader.
		else if ( !Q_stricmp(token, "portalsky") )
		{
			shader.sort = SS_PORTALSKY;
			shader.isPortalSky = qtrue;
			continue;
		}
		// skyparms <cloudheight> <outerbox> <innerbox>
		else if ( !Q_stricmp( token, "skyparms" ) )
		{
			ParseSkyParms( text );
			continue;
		}
		// light <value> determines flaring in q3map, not needed here
		else if ( !Q_stricmp(token, "light") ) 
		{
			COM_ParseExt( text, qfalse );
			continue;
		}
		// cull <face>
		else if ( !Q_stricmp( token, "cull") ) 
		{
			token = COM_ParseExt( text, qfalse );
			if ( token[0] == 0 )
			{
				ri.Printf( PRINT_WARNING, "WARNING: missing cull parms in shader '%s'\n", shader.name );
				continue;
			}

			if ( !Q_stricmp( token, "none" ) || !Q_stricmp( token, "twosided" ) || !Q_stricmp( token, "disable" ) )
			{
				shader.cullType = CT_TWO_SIDED;
			}
			else if ( !Q_stricmp( token, "back" ) || !Q_stricmp( token, "backside" ) || !Q_stricmp( token, "backsided" ) )
			{
				shader.cullType = CT_BACK_SIDED;
			}
			// HZM gl2 re-port (bug-gl2-cullfront): accept 'cull front' and friends
			// (retail textures/fx/sandstorm_sky uses it). Front-sided IS the default
			// cull, so this only silences the spurious per-load warning; behavior is
			// identical to gl1 (which also falls through to the warning + default).
			else if ( !Q_stricmp( token, "front" ) || !Q_stricmp( token, "frontside" ) || !Q_stricmp( token, "frontsided" ) )
			{
				shader.cullType = CT_FRONT_SIDED;
			}
			else
			{
				ri.Printf( PRINT_WARNING, "WARNING: invalid cull parm '%s' in shader '%s'\n", token, shader.name );
			}
			continue;
		}
		// sort
		else if ( !Q_stricmp( token, "sort" ) )
		{
			ParseSort( text );
			continue;
		}
		//
		// OPENMOHAA-specific stuff
        //=========================
        else if (!Q_stricmp(token, "noMerge"))
        {
			// FIXME: unimplemented
            continue;
		}
		else if (!Q_stricmp(token, "spritegen"))
		{
			token = COM_ParseExt(text, qfalse);
			if (token[0] == 0) {
				ri.Printf(PRINT_WARNING, "WARNING: missing spritegen parm in shader '%s'\n", shader.name);
				continue;
			}

			if (!Q_stricmp(token, "parallel")) {
				shader.sprite.type = SPRITE_PARALLEL;
			} else if (!Q_stricmp(token, "parallel_oriented")) {
				shader.sprite.type = SPRITE_PARALLEL_ORIENTED;
			} else if (!Q_stricmp(token, "parallel_upright")) {
				shader.sprite.type = SPRITE_PARALLEL_UPRIGHT;
			} else if (!Q_stricmp(token, "oriented")) {
				shader.sprite.type = SPRITE_ORIENTED;
			}

			shader.sprite.scale = 1.0;
			continue;
		}
		else if (!Q_stricmp(token, "spritescale"))
		{
			token = COM_ParseExt(text, qfalse);
			if (token[0] == 0) {
				ri.Printf(PRINT_WARNING, "WARNING: missing spritescale parm in shader '%s'\n", shader.name);
				continue;
			}

			shader.sprite.scale = atof(token);
			continue;
		}
		// force 32 bit images
		if (!Q_stricmp(token, "force32bit"))
		{
			shader.force32bit = qtrue;
			continue;
		}
		//=========================
		else
		{
			ri.Printf( PRINT_WARNING, "WARNING: unknown general shader parameter '%s' in '%s'\n", token, shader.name );
			return qfalse;
		}
	}

	//
	// ignore shaders that don't have any stages, unless it is a sky or fog
	//
	// HZM gl2 re-port (bug-gl2-nodraw): exempt surfaceparm-nodraw shaders like
	// gl1 (gl1 tr_shader.c:2563) - zero-stage utility shaders (e.g.
	// textures/common/modelshader) used to fail parse -> default checkerboard.
	if ( s == 0 && !shader.isSky && !(shader.contentFlags & CONTENTS_FOG ) && !(shader.surfaceFlags & SURF_NODRAW) ) {
		return qfalse;
	}

	shader.explicitlyDefined = qtrue;

	return qtrue;
}

/*
========================================================================================

SHADER OPTIMIZATION AND FOGGING

========================================================================================
*/

/*
===================
ComputeStageIteratorFunc

See if we can use on of the simple fastpath stage functions,
otherwise set to the generic stage function
===================
*/
static void ComputeStageIteratorFunc( void )
{
	shader.optimalStageIteratorFunc = RB_StageIteratorGeneric;

	//
	// see if this should go into the sky path
	//
	if ( shader.isSky )
	{
		shader.optimalStageIteratorFunc = RB_StageIteratorSky;
		return;
	}
}

/*
===================
ComputeVertexAttribs

Check which vertex attributes we only need, so we
don't need to submit/copy all of them.
===================
*/
static void ComputeVertexAttribs(void)
{
	int i, stage;

	// dlights always need ATTR_NORMAL
	shader.vertexAttribs = ATTR_POSITION | ATTR_NORMAL;

	// portals always need normals, for SurfIsOffscreen()
	if (shader.isPortal)
	{
		shader.vertexAttribs |= ATTR_NORMAL;
	}

	if (shader.defaultShader)
	{
		shader.vertexAttribs |= ATTR_TEXCOORD;
		return;
	}

	if(shader.numDeforms)
	{
		for ( i = 0; i < shader.numDeforms; i++)
		{
			deformStage_t  *ds = &shader.deforms[i];

			switch (ds->deformation)
			{
				case DEFORM_BULGE:
					shader.vertexAttribs |= ATTR_NORMAL | ATTR_TEXCOORD;
					break;

				case DEFORM_AUTOSPRITE:
					shader.vertexAttribs |= ATTR_NORMAL | ATTR_COLOR;
					break;

				// HZM gl2 [2026-09-25] Phase R1: LightGlowDeform stamps quads exactly as AutospriteDeform does
				// (normal + the source vertex colour), so it needs the same attributes. Load-time and
				// unconditional; it only widens the upload mask, the image is unchanged while the switch is off.
				case DEFORM_LIGHTGLOW:
					shader.vertexAttribs |= ATTR_NORMAL | ATTR_COLOR;
					break;

				case DEFORM_WAVE:
				case DEFORM_NORMALS:
				case DEFORM_TEXT0:
				case DEFORM_TEXT1:
				case DEFORM_TEXT2:
				case DEFORM_TEXT3:
				case DEFORM_TEXT4:
				case DEFORM_TEXT5:
				case DEFORM_TEXT6:
				case DEFORM_TEXT7:
					shader.vertexAttribs |= ATTR_NORMAL;
					break;

				default:
				case DEFORM_NONE:
				case DEFORM_MOVE:
				case DEFORM_PROJECTION_SHADOW:
				case DEFORM_AUTOSPRITE2:
					break;
			}
		}
	}

	for ( stage = 0; stage < MAX_SHADER_STAGES; stage++ )
	{
		shaderStage_t *pStage = &stages[stage];

		if ( !pStage->active ) 
		{
			break;
		}

		if (pStage->glslShaderGroup == tr.lightallShader)
		{
			shader.vertexAttribs |= ATTR_NORMAL;

			if ((pStage->glslShaderIndex & LIGHTDEF_LIGHTTYPE_MASK) && !(r_normalMapping->integer == 0 && r_specularMapping->integer == 0))
			{
				shader.vertexAttribs |= ATTR_TANGENT;
			}

			switch (pStage->glslShaderIndex & LIGHTDEF_LIGHTTYPE_MASK)
			{
				case LIGHTDEF_USE_LIGHTMAP:
				case LIGHTDEF_USE_LIGHT_VERTEX:
					shader.vertexAttribs |= ATTR_LIGHTDIRECTION;
					break;
				default:
					break;
			}
		}

		for (i = 0; i < NUM_TEXTURE_BUNDLES; i++)
		{
			if ( pStage->bundle[i].image[0] == 0 )
			{
				continue;
			}

			switch(pStage->bundle[i].tcGen)
			{
				case TCGEN_TEXTURE:
					shader.vertexAttribs |= ATTR_TEXCOORD;
					break;
				case TCGEN_LIGHTMAP:
					shader.vertexAttribs |= ATTR_LIGHTCOORD;
					break;
				case TCGEN_ENVIRONMENT_MAPPED:
				case TCGEN_ENVIRONMENT_MAPPED2:
					shader.vertexAttribs |= ATTR_NORMAL;
					break;

				default:
					break;
			}
		}

		switch(pStage->rgbGen)
		{
			case CGEN_EXACT_VERTEX:
			case CGEN_VERTEX:
			case CGEN_EXACT_VERTEX_LIT:
			case CGEN_VERTEX_LIT:
			case CGEN_ONE_MINUS_VERTEX:
				shader.vertexAttribs |= ATTR_COLOR;
				break;

			case CGEN_LIGHTING_DIFFUSE:
				shader.vertexAttribs |= ATTR_NORMAL;
				break;

			// HZM gl2 re-port (bug-gl2-modellight): CPU-computed model lighting
			// is delivered through the color attribute (tess.color), spherical
			// also needs normals for the per-vertex light dot products
			case CGEN_LIGHTING_GRID:
				shader.vertexAttribs |= ATTR_COLOR;
				break;
			case CGEN_LIGHTING_SPHERICAL:
			case CGEN_STATIC:
				shader.vertexAttribs |= ATTR_COLOR | ATTR_NORMAL;
				break;

			default:
				break;
		}

		switch(pStage->alphaGen)
		{
			case AGEN_LIGHTING_SPECULAR:
				shader.vertexAttribs |= ATTR_NORMAL;
				break;

			case AGEN_VERTEX:
			case AGEN_ONE_MINUS_VERTEX:
			// HZM gl2 parity (bug-1300): the per-vertex distance fade is delivered
			// through tess.color's alpha channel, so the colour attribute has to be in
			// the upload set even when the rgbGen would not have asked for it. rgbGen
			// lightingGrid / lightingSpherical / static already imply ATTR_COLOR, which
			// covers the shipped foliage; this is for the rgbGen identity + alphaGen
			// distFade combination, where attr_Color would otherwise resolve to the GL
			// constant (0,0,0,1) and the fade would be silently dropped.
			case AGEN_DIST_FADE:
			case AGEN_ONE_MINUS_DIST_FADE:
				shader.vertexAttribs |= ATTR_COLOR;
				break;

			default:
				break;
		}
	}
}


static void CollapseStagesToLightall(shaderStage_t *diffuse, 
	shaderStage_t *normal, shaderStage_t *specular, shaderStage_t *lightmap, 
	qboolean useLightVector, qboolean useLightVertex, qboolean parallax, qboolean tcgen)
{
	int defs = 0;

	//ri.Printf(PRINT_ALL, "shader %s has diffuse %s", shader.name, diffuse->bundle[0].image[0]->imgName);

	// reuse diffuse, mark others inactive
	diffuse->type = ST_GLSL;

	if (lightmap)
	{
		//ri.Printf(PRINT_ALL, ", lightmap");
		diffuse->bundle[TB_LIGHTMAP] = lightmap->bundle[0];
		defs |= LIGHTDEF_USE_LIGHTMAP;
	}
	else if (useLightVector)
	{
		defs |= LIGHTDEF_USE_LIGHT_VECTOR;
	}
	else if (useLightVertex)
	{
		defs |= LIGHTDEF_USE_LIGHT_VERTEX;
	}

	if (r_deluxeMapping->integer && tr.worldDeluxeMapping && lightmap && shader.lightmapIndex >= 0)
	{
		//ri.Printf(PRINT_ALL, ", deluxemap");
		diffuse->bundle[TB_DELUXEMAP] = lightmap->bundle[0];
		diffuse->bundle[TB_DELUXEMAP].image[0] = tr.deluxemaps[shader.lightmapIndex];
	}

	if (r_normalMapping->integer)
	{
		image_t *diffuseImg;
		if (normal)
		{
			//ri.Printf(PRINT_ALL, ", normalmap %s", normal->bundle[0].image[0]->imgName);
			diffuse->bundle[TB_NORMALMAP] = normal->bundle[0];
			if (parallax && r_parallaxMapping->integer)
				defs |= LIGHTDEF_USE_PARALLAXMAP;

			VectorCopy4(normal->normalScale, diffuse->normalScale);
		}
		else if ((lightmap || useLightVector || useLightVertex) && (diffuseImg = diffuse->bundle[TB_DIFFUSEMAP].image[0]))
		{
			char normalName[MAX_QPATH];
			image_t *normalImg;
			imgFlags_t normalFlags = (diffuseImg->flags & ~IMGFLAG_GENNORMALMAP) | IMGFLAG_NOLIGHTSCALE;

			// try a normalheight image first
			COM_StripExtension(diffuseImg->imgName, normalName, MAX_QPATH);
			Q_strcat(normalName, MAX_QPATH, "_nh");

			normalImg = R_FindImageFile(normalName, IMGTYPE_NORMALHEIGHT, normalFlags);

			if (normalImg)
			{
				parallax = qtrue;
			}
			else
			{
				// try a normal image ("_n" suffix)
				normalName[strlen(normalName) - 1] = '\0';
				normalImg = R_FindImageFile(normalName, IMGTYPE_NORMAL, normalFlags);
			}

			if (normalImg)
			{
				diffuse->bundle[TB_NORMALMAP] = diffuse->bundle[0];
				diffuse->bundle[TB_NORMALMAP].numImageAnimations = 0;
				diffuse->bundle[TB_NORMALMAP].image[0] = normalImg;

				if (parallax && r_parallaxMapping->integer)
					defs |= LIGHTDEF_USE_PARALLAXMAP;

				VectorSet4(diffuse->normalScale, r_baseNormalX->value, r_baseNormalY->value, 1.0f, r_baseParallax->value);

				// HZM gl2 (r_hzmGenNormals): mark the stage IF that normal map is one we
				// synthesised. This is the point where the decision is finally safe to make -
				// the enclosing `else if` already established (lightmap || useLightVector ||
				// useLightVertex), i.e. the stage really did resolve to a light type, so a
				// generated map bound here will actually be sampled. Authored _n / _nh art
				// deliberately does NOT get marked: it keeps r_baseNormalX/Y and gets no
				// r_hzmSpecular, because a real normal map has no need of either.
				if (R_HZM_IsGeneratedNormal(normalImg))
				{
					diffuse->hzmGenNormal = qtrue;
				}
			}
		}
	}

	if (r_specularMapping->integer)
	{
		image_t *diffuseImg;
		if (specular)
		{
			//ri.Printf(PRINT_ALL, ", specularmap %s", specular->bundle[0].image[0]->imgName);
			diffuse->bundle[TB_SPECULARMAP] = specular->bundle[0];
			VectorCopy4(specular->specularScale, diffuse->specularScale);
		}
		else if ((lightmap || useLightVector || useLightVertex) && (diffuseImg = diffuse->bundle[TB_DIFFUSEMAP].image[0]))
		{
			char specularName[MAX_QPATH];
			image_t *specularImg;
			imgFlags_t specularFlags = (diffuseImg->flags & ~IMGFLAG_GENNORMALMAP) | IMGFLAG_NOLIGHTSCALE;

			COM_StripExtension(diffuseImg->imgName, specularName, MAX_QPATH);
			Q_strcat(specularName, MAX_QPATH, "_s");

			specularImg = R_FindImageFile(specularName, IMGTYPE_COLORALPHA, specularFlags);

			if (specularImg)
			{
				diffuse->bundle[TB_SPECULARMAP] = diffuse->bundle[0];
				diffuse->bundle[TB_SPECULARMAP].numImageAnimations = 0;
				diffuse->bundle[TB_SPECULARMAP].image[0] = specularImg;

				VectorSet4(diffuse->specularScale, 1.0f, 1.0f, 1.0f, 1.0f);
			}
		}
	}

	if (tcgen || diffuse->bundle[0].numTexMods)
	{
		defs |= LIGHTDEF_USE_TCGEN_AND_TCMOD;
	}

	//ri.Printf(PRINT_ALL, ".\n");

	diffuse->glslShaderGroup = tr.lightallShader;
	diffuse->glslShaderIndex = defs;
}


static int CollapseStagesToGLSL(void)
{
	int i, j, numStages;
	qboolean skip = qfalse;

	// skip shaders with deforms
	if (shader.numDeforms != 0)
	{
		skip = qtrue;
	}

	if (!skip)
	{
		// if 2+ stages and first stage is lightmap, switch them
		// this makes it easier for the later bits to process
		if (stages[0].active && stages[0].bundle[0].tcGen == TCGEN_LIGHTMAP && stages[1].active)
		{
			int blendBits = stages[1].stateBits & ( GLS_DSTBLEND_BITS | GLS_SRCBLEND_BITS );

			if (blendBits == (GLS_DSTBLEND_SRC_COLOR | GLS_SRCBLEND_ZERO)
				|| blendBits == (GLS_DSTBLEND_ZERO | GLS_SRCBLEND_DST_COLOR))
			{
				int stateBits0 = stages[0].stateBits;
				int stateBits1 = stages[1].stateBits;
				shaderStage_t swapStage;

				swapStage = stages[0];
				stages[0] = stages[1];
				stages[1] = swapStage;

				stages[0].stateBits = stateBits0;
				stages[1].stateBits = stateBits1;
			}
		}
	}

	if (!skip)
	{
		// scan for shaders that aren't supported
		for (i = 0; i < MAX_SHADER_STAGES; i++)
		{
			shaderStage_t *pStage = &stages[i];

			if (!pStage->active)
				continue;

			if (pStage->adjustColorsForFog)
			{
				skip = qtrue;
				break;
			}

			if (pStage->bundle[0].tcGen == TCGEN_LIGHTMAP)
			{
				int blendBits = pStage->stateBits & ( GLS_DSTBLEND_BITS | GLS_SRCBLEND_BITS );
				
				if (blendBits != (GLS_DSTBLEND_SRC_COLOR | GLS_SRCBLEND_ZERO)
					&& blendBits != (GLS_DSTBLEND_ZERO | GLS_SRCBLEND_DST_COLOR))
				{
					skip = qtrue;
					break;
				}
			}

			switch(pStage->bundle[0].tcGen)
			{
				case TCGEN_TEXTURE:
				case TCGEN_LIGHTMAP:
				case TCGEN_ENVIRONMENT_MAPPED:
				case TCGEN_ENVIRONMENT_MAPPED2:
				case TCGEN_VECTOR:
					break;
				default:
					skip = qtrue;
					break;
			}

			switch(pStage->alphaGen)
			{
				case AGEN_LIGHTING_SPECULAR:
				case AGEN_PORTAL:
					skip = qtrue;
					break;
				default:
					break;
			}

			// HZM gl2 [2026-09-25] Phase R2: rgbGen dot / oneMinusDot live only in generic_vp's CalcColor.
			// A single-bundle, deform-free dot stage collapsed into LIGHTALL would silently draw flat (the
			// bug-2486 class). Zero shipped content is affected: the only retail users are opellights, already
			// generic through its nextbundle, and opellightflare, referenced by no tik, BSP or script
			// (swept 2026-09-25). The coop headlight beam (coop_headbeam) is the first single-bundle user.
			switch(pStage->rgbGen)
			{
				case CGEN_DOT:
				case CGEN_ONE_MINUS_DOT:
					skip = qtrue;
					break;
				default:
					break;
			}
		}
	}

	if (!skip)
	{
		qboolean usedLightmap = qfalse;

		for (i = 0; i < MAX_SHADER_STAGES; i++)
		{
			shaderStage_t *pStage = &stages[i];
			shaderStage_t *diffuse, *normal, *specular, *lightmap;
			qboolean parallax, tcgen, diffuselit, vertexlit;

			if (!pStage->active)
				continue;

			// skip normal and specular maps
			if (pStage->type != ST_COLORMAP)
				continue;

			// skip lightmaps
			if (pStage->bundle[0].tcGen == TCGEN_LIGHTMAP)
				continue;

			diffuse  = pStage;
			normal   = NULL;
			parallax = qfalse;
			specular = NULL;
			lightmap = NULL;

			// we have a diffuse map, find matching normal, specular, and lightmap
			for (j = i + 1; j < MAX_SHADER_STAGES; j++)
			{
				shaderStage_t *pStage2 = &stages[j];

				if (!pStage2->active)
					continue;

				switch(pStage2->type)
				{
					case ST_NORMALMAP:
						if (!normal)
						{
							normal = pStage2;
						}
						break;

					case ST_NORMALPARALLAXMAP:
						if (!normal)
						{
							normal = pStage2;
							parallax = qtrue;
						}
						break;

					case ST_SPECULARMAP:
						if (!specular)
						{
							specular = pStage2;
						}
						break;

					case ST_COLORMAP:
						if (pStage2->bundle[0].tcGen == TCGEN_LIGHTMAP)
						{
							int blendBits = pStage->stateBits & ( GLS_DSTBLEND_BITS | GLS_SRCBLEND_BITS );

							// Only add lightmap to blendfunc filter stage if it's the first time lightmap is used
							// otherwise it will cause the shader to be darkened by the lightmap multiple times.
							if (!usedLightmap || (blendBits != (GLS_DSTBLEND_SRC_COLOR | GLS_SRCBLEND_ZERO)
								&& blendBits != (GLS_DSTBLEND_ZERO | GLS_SRCBLEND_DST_COLOR)))
							{
								lightmap = pStage2;
								usedLightmap = qtrue;
							}
						}
						break;

					default:
						break;
				}
			}

			tcgen = qfalse;
			if (diffuse->bundle[0].tcGen == TCGEN_ENVIRONMENT_MAPPED
			    || diffuse->bundle[0].tcGen == TCGEN_LIGHTMAP
			    || diffuse->bundle[0].tcGen == TCGEN_VECTOR)
			{
				tcgen = qtrue;
			}

			diffuselit = qfalse;
			if (diffuse->rgbGen == CGEN_LIGHTING_DIFFUSE)
			{
				diffuselit = qtrue;
			}

			vertexlit = qfalse;
			if (diffuse->rgbGen == CGEN_VERTEX_LIT || diffuse->rgbGen == CGEN_EXACT_VERTEX_LIT)
			{
				vertexlit = qtrue;
			}

			CollapseStagesToLightall(diffuse, normal, specular, lightmap, diffuselit, vertexlit, parallax, tcgen);
		}

		// deactivate lightmap stages
		for (i = 0; i < MAX_SHADER_STAGES; i++)
		{
			shaderStage_t *pStage = &stages[i];

			if (!pStage->active)
				continue;

			if (pStage->bundle[0].tcGen == TCGEN_LIGHTMAP)
			{
				pStage->active = qfalse;
			}
		}
	}

	// deactivate normal and specular stages
	for (i = 0; i < MAX_SHADER_STAGES; i++)
	{
		shaderStage_t *pStage = &stages[i];

		if (!pStage->active)
			continue;

		if (pStage->type == ST_NORMALMAP)
		{
			pStage->active = qfalse;
		}

		if (pStage->type == ST_NORMALPARALLAXMAP)
		{
			pStage->active = qfalse;
		}

		if (pStage->type == ST_SPECULARMAP)
		{
			pStage->active = qfalse;
		}			
	}

	// remove inactive stages
	numStages = 0;
	for (i = 0; i < MAX_SHADER_STAGES; i++)
	{
		if (!stages[i].active)
			continue;

		if (i == numStages)
		{
			numStages++;
			continue;
		}

		stages[numStages] = stages[i];
		stages[i].active = qfalse;
		numStages++;
	}

	// convert any remaining lightmap stages with no blending or blendfunc filter
	// to a lighting pass with a white texture
	// only do this with r_sunlightMode non-zero, as it's only for correct shadows.
	if (r_sunlightMode->integer && shader.numDeforms == 0)
	{
		for (i = 0; i < MAX_SHADER_STAGES; i++)
		{
			shaderStage_t *pStage = &stages[i];
			int blendBits;

			if (!pStage->active)
				continue;

			if (pStage->adjustColorsForFog)
				continue;

			if (pStage->bundle[TB_DIFFUSEMAP].tcGen != TCGEN_LIGHTMAP)
				continue;

			blendBits = pStage->stateBits & (GLS_DSTBLEND_BITS | GLS_SRCBLEND_BITS);

			if (blendBits != 0 &&
				blendBits != (GLS_DSTBLEND_SRC_COLOR | GLS_SRCBLEND_ZERO) &&
				blendBits != (GLS_DSTBLEND_ZERO | GLS_SRCBLEND_DST_COLOR)) {
				continue;
			}

			pStage->glslShaderGroup = tr.lightallShader;
			pStage->glslShaderIndex = LIGHTDEF_USE_LIGHTMAP;
			pStage->bundle[TB_LIGHTMAP] = pStage->bundle[TB_DIFFUSEMAP];
			pStage->bundle[TB_DIFFUSEMAP].image[0] = tr.whiteImage;
			pStage->bundle[TB_DIFFUSEMAP].isLightmap = qfalse;
			pStage->bundle[TB_DIFFUSEMAP].tcGen = TCGEN_TEXTURE;
		}
	}

	// convert any remaining lightingdiffuse stages to a lighting pass
	if (shader.numDeforms == 0)
	{
		for (i = 0; i < MAX_SHADER_STAGES; i++)
		{
			shaderStage_t *pStage = &stages[i];

			if (!pStage->active)
				continue;

			if (pStage->adjustColorsForFog)
				continue;

			if (pStage->rgbGen == CGEN_LIGHTING_DIFFUSE)
			{
				pStage->glslShaderGroup = tr.lightallShader;
				pStage->glslShaderIndex = LIGHTDEF_USE_LIGHT_VECTOR;

				if (pStage->bundle[0].tcGen != TCGEN_TEXTURE || pStage->bundle[0].numTexMods != 0)
					pStage->glslShaderIndex |= LIGHTDEF_USE_TCGEN_AND_TCMOD;
			}
		}
	}

	return numStages;
}

/*
=============

FixRenderCommandList
https://zerowing.idsoftware.com/bugzilla/show_bug.cgi?id=493
Arnout: this is a nasty issue. Shaders can be registered after drawsurfaces are generated
but before the frame is rendered. This will, for the duration of one frame, cause drawsurfaces
to be rendered with bad shaders. To fix this, need to go through all render commands and fix
sortedIndex.
==============
*/
static void FixRenderCommandList( int newShader ) {
	renderCommandList_t	*cmdList;

	// HZM (settings-apply crash, GeneratePermanentShader+0x487 = inlined SortNewShader ->
	// here): during vid_restart / renderer teardown backEndData can be NULL while a late
	// shader registration still runs. The old `if (cmdList)` guarded &backEndData->commands
	// - a near-NULL OFFSET that is never NULL - so the walk dereferenced near-NULL
	// deterministically. Guard the base pointer itself.
	if ( !backEndData ) {
		return;
	}
	cmdList = &backEndData->commands;

	if( cmdList ) {
		const void *curCmd = cmdList->cmds;

		while ( 1 ) {
			curCmd = PADP(curCmd, sizeof(void *));

			switch ( *(const int *)curCmd ) {
			case RC_SET_COLOR:
				{
				const setColorCommand_t *sc_cmd = (const setColorCommand_t *)curCmd;
				curCmd = (const void *)(sc_cmd + 1);
				break;
				}
			case RC_STRETCH_PIC:
				{
				const stretchPicCommand_t *sp_cmd = (const stretchPicCommand_t *)curCmd;
				curCmd = (const void *)(sp_cmd + 1);
				break;
				}
			case RC_DRAW_SURFS:
			case RC_SPRITE_SURFS:
				{
				int i;
				drawSurf_t	*drawSurf;
				int			sortedIndex;
				const drawSurfsCommand_t *ds_cmd =  (const drawSurfsCommand_t *)curCmd;

				for( i = 0, drawSurf = ds_cmd->drawSurfs; i < ds_cmd->numDrawSurfs; i++, drawSurf++ ) {
					sortedIndex = (( drawSurf->sort >> QSORT_SHADERNUM_SHIFT ) & (MAX_SORTED_SHADERS-1));
					if( sortedIndex >= newShader ) {
						sortedIndex++;
						// replace only the shader-index field: the bits below
						// QSORT_SHADERNUM_SHIFT hold entity/fog/pshadow/dlight and
						// the static-model bit for scene surfs, but a z-distance
						// for sprite surfs, so they must be preserved verbatim
						// rather than repacked field-by-field
						drawSurf->sort = ( drawSurf->sort & ( ( 1u << QSORT_SHADERNUM_SHIFT ) - 1 ) )
							| ( sortedIndex << QSORT_SHADERNUM_SHIFT );
					}
				}
				curCmd = (const void *)(ds_cmd + 1);
				break;
				}
			case RC_DRAW_BUFFER:
				{
				const drawBufferCommand_t *db_cmd = (const drawBufferCommand_t *)curCmd;
				curCmd = (const void *)(db_cmd + 1);
				break;
				}
			case RC_SWAP_BUFFERS:
				{
				const swapBuffersCommand_t *sb_cmd = (const swapBuffersCommand_t *)curCmd;
				curCmd = (const void *)(sb_cmd + 1);
				break;
				}
			case RC_SCREENSHOT:
				{
				const screenshotCommand_t *ss_cmd = (const screenshotCommand_t *)curCmd;
				curCmd = (const void *)(ss_cmd + 1);
				break;
				}
			case RC_VIDEOFRAME:
				{
				const videoFrameCommand_t *vf_cmd = (const videoFrameCommand_t *)curCmd;
				curCmd = (const void *)(vf_cmd + 1);
				break;
				}
			case RC_COLORMASK:
				{
				const colorMaskCommand_t *cm_cmd = (const colorMaskCommand_t *)curCmd;
				curCmd = (const void *)(cm_cmd + 1);
				break;
				}
			case RC_CLEARDEPTH:
				{
				const clearDepthCommand_t *cd_cmd = (const clearDepthCommand_t *)curCmd;
				curCmd = (const void *)(cd_cmd + 1);
				break;
				}
			case RC_CAPSHADOWMAP:
				{
				const capShadowmapCommand_t *cs_cmd = (const capShadowmapCommand_t *)curCmd;
				curCmd = (const void *)(cs_cmd + 1);
				break;
				}
			case RC_POSTPROCESS:
				{
				const postProcessCommand_t *pp_cmd = (const postProcessCommand_t *)curCmd;
				curCmd = (const void *)(pp_cmd + 1);
				break;
				}
			case RC_EXPORT_CUBEMAPS:
				{
				const exportCubemapsCommand_t *ec_cmd = (const exportCubemapsCommand_t *)curCmd;
				curCmd = (const void *)(ec_cmd + 1);
				break;
				}
			case RC_END_OF_LIST:
			default:
				return;
			}
		}
	}
}

/*
==============
SortNewShader

Positions the most recently created shader in the tr.sortedShaders[]
array so that the shader->sort key is sorted reletive to the other
shaders.

Sets shader->sortedIndex
==============
*/
static void SortNewShader( void ) {
	int		i;
	float	sort;
	shader_t	*newShader;

	// HZM [user 2026-09-16, bug-2666] settings-apply / vid_restart teardown race. Applying a new
	// resolution tears the renderer down and back up; a late shader registration can reach here while
	// tr.shaders / tr.sortedShaders are in a half-built state, so newShader or a sortedShaders[] entry
	// is NULL and the ->sort / ->sortedIndex read dereferenced ~NULL (crash in GeneratePermanentShader,
	// resolved from the .map). This is the same class the backEndData guard in FixRenderCommandList
	// covers - guard the pointers here too rather than trust the transient state.
	if ( tr.numShaders < 1 ) {
		return;
	}
	newShader = tr.shaders[ tr.numShaders - 1 ];
	if ( !newShader ) {
		return;
	}
	sort = newShader->sort;

	for ( i = tr.numShaders - 2 ; i >= 0 ; i-- ) {
		if ( !tr.sortedShaders[ i ] ) {
			break;
		}
		if ( tr.sortedShaders[ i ]->sort <= sort ) {
			break;
		}
		tr.sortedShaders[i+1] = tr.sortedShaders[i];
		tr.sortedShaders[i+1]->sortedIndex++;
	}

	// Arnout: fix rendercommandlist
	// https://zerowing.idsoftware.com/bugzilla/show_bug.cgi?id=493
	FixRenderCommandList( i+1 );

	newShader->sortedIndex = i+1;
	tr.sortedShaders[i+1] = newShader;
}


/*
====================
GeneratePermanentShader
====================
*/
static shader_t *GeneratePermanentShader( void ) {
	shader_t	*newShader;
	int			i, b;
	int			size, hash;

	if ( tr.numShaders == MAX_SHADERS ) {
		ri.Printf( PRINT_WARNING, "WARNING: GeneratePermanentShader - MAX_SHADERS hit\n");
		return tr.defaultShader;
	}

	// HZM (engine-limits audit): tr.shaders[] can hold MAX_SHADERS entries, but a drawsurf sort
	// key only carries QSORT_SHADERNUM_BITS of sortedIndex (see tr_local.h). Past
	// MAX_SORTED_SHADERS the index silently aliases in R_DecomposeSort and surfaces render with
	// an unrelated shader. gl2's layout currently makes MAX_SORTED_SHADERS == MAX_SHADERS, so
	// this branch is unreachable today - it exists so that a future repack of the sort key
	// cannot reintroduce the silent aliasing unnoticed.
	// "==" makes this fire exactly once, at the moment the ceiling is crossed.
	if ( tr.numShaders == MAX_SORTED_SHADERS ) {
		ri.Printf( PRINT_WARNING,
			"^1WARNING: shader count has passed MAX_SORTED_SHADERS (%d).\n"
			"^1  The drawsurf sort key only encodes %d bits of shader index at "
			"QSORT_SHADERNUM_SHIFT %d, so every shader from here on will be drawn with the WRONG "
			"shader (aliased onto sortedIndex %% %d). Repack the sort key or cut shader count.\n",
			MAX_SORTED_SHADERS, QSORT_SHADERNUM_BITS, QSORT_SHADERNUM_SHIFT, MAX_SORTED_SHADERS );
	}

	newShader = ri.Hunk_Alloc( sizeof( shader_t ), h_low );
	// [bug-2666] never build on a NULL allocation - during a vid_restart teardown the hunk can be
	// transient, and the `*newShader = shader` copy below would fault writing to NULL.
	if ( !newShader ) {
		return tr.defaultShader;
	}

	*newShader = shader;

	if ( shader.sort <= SS_OPAQUE ) {
		newShader->fogPass = FP_EQUAL;
	} else if ( shader.contentFlags & CONTENTS_FOG ) {
		newShader->fogPass = FP_LE;
	}

	tr.shaders[ tr.numShaders ] = newShader;
	newShader->index = tr.numShaders;
	
	tr.sortedShaders[ tr.numShaders ] = newShader;
	newShader->sortedIndex = tr.numShaders;

	tr.numShaders++;

	for ( i = 0 ; i < newShader->numUnfoggedPasses ; i++ ) {
		if ( !stages[i].active ) {
			break;
		}
		newShader->stages[i] = ri.Hunk_Alloc( sizeof( stages[i] ), h_low );
		// [bug-2666] guard a transient-teardown NULL stage alloc (the copy below would fault at NULL).
		if ( !newShader->stages[i] ) {
			newShader->numUnfoggedPasses = i;
			break;
		}
		*newShader->stages[i] = stages[i];

		for ( b = 0 ; b < NUM_TEXTURE_BUNDLES ; b++ ) {
			size = newShader->stages[i]->bundle[b].numTexMods * sizeof( texModInfo_t );
			// HZM gl2 re-port (bug-gl2-ztagmalloc): guard with if(size) like gl1
			// GeneratePermanentShader. ri.Hunk_Alloc routes to Z_TagMalloc, which
			// rejects zero-size allocs - every tcMod-less stage bundle (7 bundles
			// per stage, nearly all empty) spammed "Z_TagMalloc, Negative or zero
			// size 0 tag 12" on each shader registration.
			if ( size ) {
				newShader->stages[i]->bundle[b].texMods = ri.Hunk_Alloc( size, h_low );
				Com_Memcpy( newShader->stages[i]->bundle[b].texMods, stages[i].bundle[b].texMods, size );
			}
		}
	}

	SortNewShader();

	hash = generateHashValue(newShader->name, FILE_HASH_SIZE);
	newShader->next = hashTable[hash];
	hashTable[hash] = newShader;

	return newShader;
}

/*
=================
VertexLightingCollapse

If vertex lighting is enabled, only render a single
pass, trying to guess which is the correct one to best approximate
what it is supposed to look like.
=================
*/
static void VertexLightingCollapse( void ) {
	int		stage;
	shaderStage_t	*bestStage;
	int		bestImageRank;
	int		rank;

	// if we aren't opaque, just use the first pass
	if ( shader.sort == SS_OPAQUE ) {

		// pick the best texture for the single pass
		bestStage = &stages[0];
		bestImageRank = -999999;

		for ( stage = 0; stage < MAX_SHADER_STAGES; stage++ ) {
			shaderStage_t *pStage = &stages[stage];

			if ( !pStage->active ) {
				break;
			}
			rank = 0;

			if ( pStage->bundle[0].isLightmap ) {
				rank -= 100;
			}
			if ( pStage->bundle[0].tcGen != TCGEN_TEXTURE ) {
				rank -= 5;
			}
			if ( pStage->bundle[0].numTexMods ) {
				rank -= 5;
			}
			if ( pStage->rgbGen != CGEN_IDENTITY && pStage->rgbGen != CGEN_IDENTITY_LIGHTING ) {
				rank -= 3;
			}

			if ( rank > bestImageRank  ) {
				bestImageRank = rank;
				bestStage = pStage;
			}
		}

		stages[0].bundle[0] = bestStage->bundle[0];
		stages[0].stateBits &= ~( GLS_DSTBLEND_BITS | GLS_SRCBLEND_BITS );
		stages[0].stateBits |= GLS_DEPTHMASK_TRUE;
		if ( shader.lightmapIndex == LIGHTMAP_NONE ) {
			stages[0].rgbGen = CGEN_LIGHTING_DIFFUSE;
		} else {
			stages[0].rgbGen = CGEN_EXACT_VERTEX;
		}
		stages[0].alphaGen = AGEN_SKIP;		
	} else {
		// don't use a lightmap (tesla coils)
		if ( stages[0].bundle[0].isLightmap ) {
			stages[0] = stages[1];
		}

		// if we were in a cross-fade cgen, hack it to normal
		if ( stages[0].rgbGen == CGEN_ONE_MINUS_ENTITY || stages[1].rgbGen == CGEN_ONE_MINUS_ENTITY ) {
			stages[0].rgbGen = CGEN_IDENTITY_LIGHTING;
		}
		if ( ( stages[0].rgbGen == CGEN_WAVEFORM && stages[0].rgbWave.func == GF_SAWTOOTH )
			&& ( stages[1].rgbGen == CGEN_WAVEFORM && stages[1].rgbWave.func == GF_INVERSE_SAWTOOTH ) ) {
			stages[0].rgbGen = CGEN_IDENTITY_LIGHTING;
		}
		if ( ( stages[0].rgbGen == CGEN_WAVEFORM && stages[0].rgbWave.func == GF_INVERSE_SAWTOOTH )
			&& ( stages[1].rgbGen == CGEN_WAVEFORM && stages[1].rgbWave.func == GF_SAWTOOTH ) ) {
			stages[0].rgbGen = CGEN_IDENTITY_LIGHTING;
		}
	}

	for ( stage = 1; stage < MAX_SHADER_STAGES; stage++ ) {
		shaderStage_t *pStage = &stages[stage];

		if ( !pStage->active ) {
			break;
		}

		Com_Memset( pStage, 0, sizeof( *pStage ) );
	}
}

/*
=================
FixFatLightmapTexCoords

Handle edge cases of altering lightmap texcoords for fat lightmap atlas
=================
*/
static void FixFatLightmapTexCoords(void)
{
	texModInfo_t *tmi;
	int lightmapnum;
	int stage;
	int size;
	int i;

	if ( !r_mergeLightmaps->integer || tr.fatLightmapCols <= 0) {
		return;
	}

	if ( shader.lightmapIndex < 0 ) {
		// no internal lightmap, texcoords were not modified
		return;
	}

	lightmapnum = shader_realLightmapIndex;

	if (tr.worldDeluxeMapping)
		lightmapnum >>= 1;

	lightmapnum %= (tr.fatLightmapCols * tr.fatLightmapRows);

	for ( stage = 0; stage < MAX_SHADER_STAGES; stage++ ) {
		shaderStage_t *pStage = &stages[stage];

		if ( !pStage->active ) {
			break;
		}

		if ( pStage->bundle[0].isLightmap ) {
			// fix tcMod transform for internal lightmaps, it may be used by q3map2 lightstyles
			if ( pStage->bundle[0].tcGen == TCGEN_LIGHTMAP ) {
				for ( i = 0; i < pStage->bundle[0].numTexMods; i++ ) {
					tmi = &pStage->bundle[0].texMods[i];

					if ( tmi->type == TMOD_TRANSFORM ) {
						tmi->translate[0] /= (float)tr.fatLightmapCols;
						tmi->translate[1] /= (float)tr.fatLightmapRows;
					}
				}
			}

			// fix tcGen environment for internal lightmaps to be limited to the sub-image of the atlas
			// this is done last so other tcMods are applied first in the 0.0 to 1.0 space
			if ( pStage->bundle[0].tcGen == TCGEN_ENVIRONMENT_MAPPED ) {
				if ( pStage->bundle[0].numTexMods == TR_MAX_TEXMODS ) {
					ri.Printf( PRINT_DEVELOPER, "WARNING: too many tcmods to fix lightmap texcoords for r_mergeLightmaps in shader '%s'", shader.name );
				} else {
					tmi = &pStage->bundle[0].texMods[pStage->bundle[0].numTexMods];
					pStage->bundle[0].numTexMods++;

					tmi->matrix[0][0] = 1.0f / tr.fatLightmapCols;
					tmi->matrix[0][1] = 0;
					tmi->matrix[1][0] = 0;
					tmi->matrix[1][1] = 1.0f / tr.fatLightmapRows;

					tmi->translate[0] = ( lightmapnum % tr.fatLightmapCols ) / (float)tr.fatLightmapCols;
					tmi->translate[1] = ( lightmapnum / tr.fatLightmapCols ) / (float)tr.fatLightmapRows;

					tmi->type = TMOD_TRANSFORM;
				}
			}
		}
		// add a tcMod transform for external lightmaps to convert back to the original texcoords
		else if ( pStage->bundle[0].tcGen == TCGEN_LIGHTMAP ) {
			if ( pStage->bundle[0].numTexMods == TR_MAX_TEXMODS ) {
				ri.Printf( PRINT_DEVELOPER, "WARNING: too many tcmods to fix lightmap texcoords for r_mergeLightmaps in shader '%s'", shader.name );
			} else {
				size = pStage->bundle[0].numTexMods * sizeof( texModInfo_t );

				if ( size ) {
					memmove( &pStage->bundle[0].texMods[1], &pStage->bundle[0].texMods[0], size );
				}

				tmi = &pStage->bundle[0].texMods[0];
				pStage->bundle[0].numTexMods++;

				tmi->matrix[0][0] = tr.fatLightmapCols;
				tmi->matrix[0][1] = 0;
				tmi->matrix[1][0] = 0;
				tmi->matrix[1][1] = tr.fatLightmapRows;

				tmi->translate[0] = -( lightmapnum % tr.fatLightmapCols );
				tmi->translate[1] = -( lightmapnum / tr.fatLightmapCols );

				tmi->type = TMOD_TRANSFORM;
			}
		}
	}
}

/*
===============
InitShader
===============
*/
static void InitShaderEx( const char *name, int lightmapIndex, int realLightmapIndex ) {
	int i;

	// clear the global shader
	Com_Memset( &shader, 0, sizeof( shader ) );
	Com_Memset( &stages, 0, sizeof( stages ) );

	// HZM gl1-parity fix: gl1 restores the sprite scale immediately after the memsets
	// (renderergl1/tr_shader.c:3439). gl2 never did, so any shader that declares
	// `spritegen ...` WITHOUT an explicit `spritescale` (the scale keyword is optional -
	// see ParseSprite, tr_shader.c:2778 vs :2789) was left at scale 0 instead of 1, i.e.
	// a degenerate zero-size sprite. Must run AFTER the memsets and BEFORE any parsing.
	shader.sprite.scale = 1.0f;

	Q_strncpyz( shader.name, name, sizeof( shader.name ) );
	shader.lightmapIndex = lightmapIndex;
	shader_realLightmapIndex = realLightmapIndex;

	for ( i = 0 ; i < MAX_SHADER_STAGES ; i++ ) {
		int b;

		// HZM gl2 re-port (bug-gl2-texmods-null): every bundle needs its own
		// texMod backing - the bundle-aware ParseTexMod writes bundle[cntBundle]
		for ( b = 0; b < NUM_TEXTURE_BUNDLES; b++ ) {
			stages[i].bundle[b].texMods = texMods[i][b];
		}

		// default normal/specular
		VectorSet4(stages[i].normalScale, 0.0f, 0.0f, 0.0f, 0.0f);
		if (r_pbr->integer)
		{
			stages[i].specularScale[0] = r_baseGloss->value;
		}
		else
		{
			stages[i].specularScale[0] =
			stages[i].specularScale[1] =
			stages[i].specularScale[2] = r_baseSpecular->value;
			stages[i].specularScale[3] = r_baseGloss->value;
		}
	}
}

static void InitShader( const char *name, int lightmapIndex ) {
	InitShaderEx( name, lightmapIndex, lightmapIndex );
}

/*
=========================
FinishShader

Returns a freshly allocated shader with all the needed info
from the current global working shader
=========================
*/
/*
===============
R_HZM_SkyHDLayers

HZM gl2 [2026-09-25] r_skyHD for sky LAYERS - the stage images a sky shader draws over its box (the moving
cloud dome, tr_sky.c FillCloudBox; africanight's talonclouds). bug-1295's NO_COMPRESSION covers the box faces
only, so a stage image is DXT1 (runtime-compressed, or a precompressed .dds - talonclouds.dds from
zzzzzzz_dds_hdmem.pk3 is 13x20 px tinted blocks at the zenith). The twin lives under a NEW name,
env/hzmhd/clouds/<image path minus "textures/">.tga (docs/tools/gen_sky_hd.py LAYERS), so no name contest.
 - Flags are built, never copied: MIPMAP | PICMIP | NO_COMPRESSION, REPEAT wrap. Never CLAMPTOEDGE (a hard
   line every tile); never orig->flags wholesale (a short-mip DDS has MIPMAP stripped, and it may carry
   GENNORMALMAP). A stage whose image is clamped is not a tiling layer and is left alone.
 - r_texturebits 16 would upload NO_COMPRESSION as GL_RGB5 (3-9 levels across this layer): skipped, logged.
 - Skipped: lightmaps, video maps, animMaps, images outside textures/, missing twins (quiet check, no
   "could not find" noise). A twin that exists but fails to load keeps the original and warns.
 - Normal mode: the twin replaces bundle[0].image[0]. Compare mode: the original stays and the twin goes in
   hzmSkyHDAlt for RB_StageIteratorSky to swap while r_skyHDCompare is 1.
One line per twin, and one count line per sky shader that has layers.
===============
*/
static void R_HZM_SkyHDLayers( void ) {
	int			compare = r_skyHDCompare ? r_skyHDCompare->integer : 0;
	qboolean	wanted = R_HZM_SkyHDWanted( qtrue );
	int			i, nLayers = 0, nHD = 0;
	const char	*why = "no HD twin";
	char		stem[MAX_QPATH], twin[MAX_QPATH];

	for ( i = 0; i < MAX_SHADER_STAGES; i++ ) {
		shaderStage_t	*pStage = &stages[i];
		image_t			*orig, *hd;

		if ( !pStage->active ) {
			break;
		}
		orig = pStage->bundle[0].image[0];
		if ( !orig || pStage->bundle[0].isLightmap || pStage->bundle[0].isVideoMap || pStage->bundle[0].numImageAnimations > 1
			|| Q_stricmpn( orig->imgName, "textures/", 9 ) ) {
			continue;
		}
		nLayers++;
		if ( !wanted ) {
			why = "r_skyHD off";
			continue;
		}
		if ( r_texturebits && r_texturebits->integer == 16 ) {
			why = "r_texturebits 16 - an uncompressed twin would be RGB5";
			continue;
		}
		if ( orig->flags & ( IMGFLAG_CLAMPTOEDGE | IMGFLAG_CLAMPTOEDGE_X | IMGFLAG_CLAMPTOEDGE_Y ) ) {
			continue;
		}
		COM_StripExtension( orig->imgName + 9, stem, sizeof( stem ) );
		Com_sprintf( twin, sizeof( twin ), "env/hzmhd/clouds/%s.tga", stem );
		if ( ri.FS_ReadFileEx( twin, NULL, qtrue ) <= 0 ) {
			continue;
		}
		hd = R_FindImageFile( twin, orig->type, IMGFLAG_MIPMAP | IMGFLAG_PICMIP | IMGFLAG_NO_COMPRESSION );
		if ( !hd ) {
			ri.Printf( PRINT_WARNING, "HZM skyHD layer: %s -> %s present but failed to load - original kept\n", orig->imgName, twin );
			continue;
		}
		nHD++;
		if ( compare ) {
			pStage->hzmSkyHDAlt = hd;
			tr.skyHDCompareLayers++;
		} else {
			pStage->bundle[0].image[0] = hd;
		}
		ri.Printf( PRINT_ALL, "HZM skyHD layer: %s -> %s %dx%d fmt 0x%x%s\n", orig->imgName, twin, hd->uploadWidth,
			hd->uploadHeight, hd->internalFormat, compare ? " + original [compare: r_skyHDCompare 1 = HD, 2 = original]" : "" );
	}
	if ( nLayers ) {
		if ( nHD ) {
			ri.Printf( PRINT_ALL, "HZM skyHD: %s layers -> HD %d/%d%s\n", shader.name, nHD, nLayers, compare ? " [compare]" : "" );
		} else {
			ri.Printf( PRINT_ALL, "HZM skyHD: %s layers -> original 0/%d (%s)\n", shader.name, nLayers, why );
		}
	}
}

static shader_t *FinishShader( void ) {
	int stage;
	qboolean		hasLightmapStage;
	qboolean		vertexLightmap;

	hasLightmapStage = qfalse;
	vertexLightmap = qfalse;

    //
    // OPENMOHAA-specific stuff
    //=========================
    CreateMultistageFromBundle();
    //=========================

	// HZM gl2 [2026-09-25] r_skyHD sky layers - after the bundles are final, before any collapse/copy
	if ( shader.isSky ) {
		R_HZM_SkyHDLayers();
	}

	//
	// set sky stuff appropriate
	//
	// HZM gl2 re-port (bug-gl2-portalsky): mirrors gl1 FinishShader
	// (gl1 tr_shader.c:3117-3119)
	if ( shader.isPortalSky ) {
		shader.sort = SS_PORTALSKY;
	}
	if ( shader.isSky ) {
		shader.sort = SS_ENVIRONMENT;
	}

	//
	// set polygon offset
	//
	if ( shader.polygonOffset && !shader.sort ) {
		shader.sort = SS_DECAL;
	}

	// HZM gl2 re-port (bug-gl2-modellight): mirrors gl1 FinishShader
	// (gl1 tr_shader.c:3131-3132)
	shader.needsLGrid = qfalse;
	shader.needsLSpherical = qfalse;
	// HZM gl2 parity (bug-1300)
	shader.needsDistFade = qfalse;
	shader.alphaGenWarned = qfalse;

	//
	// set appropriate stage information
	//
	for ( stage = 0; stage < MAX_SHADER_STAGES; ) {
		shaderStage_t *pStage = &stages[stage];

		if ( !pStage->active ) {
			break;
		}

    // check for a missing texture
		if ( !pStage->bundle[0].image[0] ) {
			ri.Printf( PRINT_WARNING, "Shader %s has a stage with no image\n", shader.name );
			pStage->active = qfalse;
			stage++;
			continue;
		}

		// HZM gl2 re-port (bug-gl2-modellight): flag grid/spherical-lit shaders
		// for the backend model-lighting setup (gl1 tr_shader.c:3152-3156)
		if (pStage->rgbGen == CGEN_LIGHTING_GRID) {
			shader.needsLGrid = qtrue;
		} else if (pStage->rgbGen == CGEN_LIGHTING_SPHERICAL || pStage->rgbGen == CGEN_STATIC) {
			shader.needsLSpherical = qtrue;
		}

		// HZM gl2 parity (bug-1300): flag the two PER-VERTEX distance-fade modes so
		// RB_FillDistFadeAlpha can skip every shader that never asked for one.
		if (pStage->alphaGen == AGEN_DIST_FADE || pStage->alphaGen == AGEN_ONE_MINUS_DIST_FADE) {
			shader.needsDistFade = qtrue;
		}

		//
		// ditch this stage if it's detail and detail textures are disabled
		//
		if ( pStage->isDetail && !r_detailTextures->integer )
		{
			int index;
			
			for(index = stage + 1; index < MAX_SHADER_STAGES; index++)
			{
				if(!stages[index].active)
					break;
			}
			
			if(index < MAX_SHADER_STAGES)
				memmove(pStage, pStage + 1, sizeof(*pStage) * (index - stage));
			else
			{
				if(stage + 1 < MAX_SHADER_STAGES)
					memmove(pStage, pStage + 1, sizeof(*pStage) * (index - stage - 1));
				
				Com_Memset(&stages[index - 1], 0, sizeof(*stages));
			}
			
			continue;
		}

		//
		// default texture coordinate generation
		//
		if ( pStage->bundle[0].isLightmap ) {
			if ( pStage->bundle[0].tcGen == TCGEN_BAD ) {
				pStage->bundle[0].tcGen = TCGEN_LIGHTMAP;
			}
			hasLightmapStage = qtrue;
		} else {
			if ( pStage->bundle[0].tcGen == TCGEN_BAD ) {
				pStage->bundle[0].tcGen = TCGEN_TEXTURE;
			}
		}


    // not a true lightmap but we want to leave existing 
    // behaviour in place and not print out a warning
    //if (pStage->rgbGen == CGEN_VERTEX) {
    //  vertexLightmap = qtrue;
    //}



		//
		// determine sort order and fog color adjustment
		//
		if ( ( pStage->stateBits & ( GLS_SRCBLEND_BITS | GLS_DSTBLEND_BITS ) ) &&
			 ( stages[0].stateBits & ( GLS_SRCBLEND_BITS | GLS_DSTBLEND_BITS ) ) ) {
			int blendSrcBits = pStage->stateBits & GLS_SRCBLEND_BITS;
			int blendDstBits = pStage->stateBits & GLS_DSTBLEND_BITS;

			// fog color adjustment only works for blend modes that have a contribution
			// that aproaches 0 as the modulate values aproach 0 --
			// GL_ONE, GL_ONE
			// GL_ZERO, GL_ONE_MINUS_SRC_COLOR
			// GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA

			// modulate, additive
			if ( ( ( blendSrcBits == GLS_SRCBLEND_ONE ) && ( blendDstBits == GLS_DSTBLEND_ONE ) ) ||
				( ( blendSrcBits == GLS_SRCBLEND_ZERO ) && ( blendDstBits == GLS_DSTBLEND_ONE_MINUS_SRC_COLOR ) ) ) {
				pStage->adjustColorsForFog = ACFF_MODULATE_RGB;
			}
			// strict blend
			else if ( ( blendSrcBits == GLS_SRCBLEND_SRC_ALPHA ) && ( blendDstBits == GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA ) )
			{
				pStage->adjustColorsForFog = ACFF_MODULATE_ALPHA;
			}
			// premultiplied alpha
			else if ( ( blendSrcBits == GLS_SRCBLEND_ONE ) && ( blendDstBits == GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA ) )
			{
				pStage->adjustColorsForFog = ACFF_MODULATE_RGBA;
			} else {
				// we can't adjust this one correctly, so it won't be exactly correct in fog
			}

			// don't screw with sort order if this is a portal or environment
			if ( !shader.sort ) {
				// see through item, like a grill or grate
				if ( pStage->stateBits & GLS_DEPTHMASK_TRUE ) {
					shader.sort = SS_SEE_THROUGH;
				} else {
					shader.sort = SS_BLEND0;
				}
			}
		}
		
		stage++;
	}

	// there are times when you will need to manually apply a sort to
	// opaque alpha tested shaders that have later blend passes
	if ( !shader.sort ) {
		shader.sort = SS_OPAQUE;
	}

	//
	// if we are in r_vertexLight mode, never use a lightmap texture
	//
	if ( stage > 1 && ( (r_vertexLight->integer && !r_uiFullScreen->integer) || glConfig.hardwareType == GLHW_PERMEDIA2 ) ) {
		VertexLightingCollapse();
		hasLightmapStage = qfalse;
	}

	FixFatLightmapTexCoords();

	//
	// look for multitexture potential
	//
	stage = CollapseStagesToGLSL();

	if ( shader.lightmapIndex >= 0 && !hasLightmapStage ) {
		if (vertexLightmap) {
			ri.Printf( PRINT_DEVELOPER, "WARNING: shader '%s' has VERTEX forced lightmap!\n", shader.name );
		} else {
			ri.Printf( PRINT_DEVELOPER, "WARNING: shader '%s' has lightmap but no lightmap stage!\n", shader.name );
			// Don't set this, it will just add duplicate shaders to the hash
  			//shader.lightmapIndex = LIGHTMAP_NONE;
		}
	}


	//
	// compute number of passes
	//
	shader.numUnfoggedPasses = stage;

	// HZM gl2 re-port (bug-gl2-foliage-white): flag alpha-tested (cutout) shaders
	// so the depth prepass can skip them (gl1 parity - gl1 has no prepass). Any
	// active stage carrying an ATEST bit marks the whole shader.
	shader.hasAlphaTest = qfalse;
	{
		int atsIdx;
		for ( atsIdx = 0; atsIdx < MAX_SHADER_STAGES; atsIdx++ ) {
			if ( !stages[atsIdx].active ) {
				continue;
			}
			if ( stages[atsIdx].stateBits & GLS_ATEST_BITS ) {
				shader.hasAlphaTest = qtrue;
				break;
			}
		}
	}

	// fogonly shaders don't have any normal passes
	if (stage == 0 && !shader.isSky)
		shader.sort = SS_FOG;

	// determine which stage iterator function is appropriate
	ComputeStageIteratorFunc();

	// determine which vertex attributes this shader needs
	ComputeVertexAttribs();

	return GeneratePermanentShader();
}

//========================================================================================

/*
====================
FindShaderInShaderText

Scans the combined text description of all the shader files for
the given shader name.

return NULL if not found

If found, it will return a valid shader
=====================
*/
/*
====================
FindShaderInShaderText

HZM gl2 (bug-2228) - RETURNS THE LAST OCCURRENCE, NOT THE FIRST. Returning the first inverted shader
precedence on gl2 relative to gl1, which silently killed every mod shader override that shadows a
retail name - on the renderer we actually ship.

The two halves have to be read together:

  * ScanAndLoadShaderFiles concatenates the files in REVERSE FS-list order
    (`for (i = numShaderFiles - 1; i >= 0; i--)`), in BOTH renderers. FS_ListFiles returns the
    highest-priority pak's files FIRST, so a LOW FS index means HIGH priority - and reverse
    concatenation therefore puts a low index LATE in s_shaderText.

  * gl1 walks s_shaderText forward and PREPENDS each block to its bucket
    (renderergl1/tr_shader.c:77-89 AddShaderTextToHash, called from FindShadersInShaderText at
    :3856-3885), then FindShaderText returns the chain HEAD (:111-129). Head == last prepended ==
    LAST text occurrence == LOWEST FS index == HIGHEST-priority pak. Correct.

  * gl2 walks s_shaderText forward and APPENDS to its bucket array, then returned the FIRST match -
    the first text occurrence, i.e. the HIGHEST FS index, i.e. the LOWEST-priority pak. Backwards.

Measured on the live install: zz_coop_shoreline.shader is FS index 93 and retail misc_outside.shader
is 212, so gl2 handed textures/misc_outside/deepbluesea_shoreline to Pak0 and discarded the mod's
override entirely - along with coop_water_overrides.shader (index 77) and every other name-shadowing
override. Those overrides were not subtly wrong; they were never parsed. That is why the 2026-08-28
deformVertexes flap and the 2026-08-31 blood stage produced no visible change.

Returning the last match makes gl2 agree with gl1. Note this also changes which RETAIL pak wins where
a name is defined more than once: main/Pak0, mainta/pak1 and maintt/pak1 all ship misc_outside.shader,
and gl2 was picking main's copy even under Breakthrough. gl1 picks maintt's. Matching gl1 is the whole
point - it is the reference implementation for this engine.
====================
*/
static char *FindShaderInShaderText( const char *shadername ) {

	char *token, *p;

	int i, hash;
	char *last = NULL;      // HZM gl2 (bug-2228) - keep the LAST match, see the note above

	hash = generateHashValue(shadername, MAX_SHADERTEXT_HASH);

	if(shaderTextHashTable[hash])
	{
		for (i = 0; shaderTextHashTable[hash][i]; i++)
		{
			p = shaderTextHashTable[hash][i];
			token = COM_ParseExt(&p, qtrue);
		
			// The bucket is filled by a forward walk of s_shaderText, so it is in text order.
			// Run to the end and keep the last hit rather than returning the first.
			if(!Q_stricmp(token, shadername))
				last = p;
		}

		if (last) {
			return last;
		}
	}

	p = s_shaderText;

	if ( !p ) {
		return NULL;
	}

	// look for label
	while ( 1 ) {
		token = COM_ParseExt( &p, qtrue );
		if ( token[0] == 0 ) {
			break;
		}

		if ( !Q_stricmp( token, shadername ) ) {
			// Same rule as the hashed path above, so the fallback can never disagree with the fast
			// path about which definition wins.
			last = p;
			SkipBracedSection( &p, 0 );
		}
		else {
			// skip the definition
			SkipBracedSection( &p, 0 );
		}
	}

	return last;
}


/*
==================
R_FindShaderByName

Will always return a valid shader, but it might be the
default shader if the real one can't be found.
==================
*/
shader_t *R_FindShaderByName( const char *name ) {
	char		strippedName[MAX_QPATH];
	int			hash;
	shader_t	*sh;

	if ( (name==NULL) || (name[0] == 0) ) {
		return tr.defaultShader;
	}

	COM_StripExtension(name, strippedName, sizeof(strippedName));

	hash = generateHashValue(strippedName, FILE_HASH_SIZE);

	//
	// see if the shader is already loaded
	//
	for (sh=hashTable[hash]; sh; sh=sh->next) {
		// NOTE: if there was no shader or image available with the name strippedName
		// then a default shader is created with lightmapIndex == LIGHTMAP_NONE, so we
		// have to check all default shaders otherwise for every call to R_FindShader
		// with that same strippedName a new default shader is created.
		if (Q_stricmp(sh->name, strippedName) == 0) {
			// match found
			return sh;
		}
	}

	return tr.defaultShader;
}


/*
===============
R_FindShader

Will always return a valid shader, but it might be the
default shader if the real one can't be found.

In the interest of not requiring an explicit shader text entry to
be defined for every single image used in the game, three default
shader behaviors can be auto-created for any image:

If lightmapIndex == LIGHTMAP_NONE, then the image will have
dynamic diffuse lighting applied to it, as appropriate for most
entity skin surfaces.

If lightmapIndex == LIGHTMAP_2D, then the image will be used
for 2D rendering unless an explicit shader is found

If lightmapIndex == LIGHTMAP_BY_VERTEX, then the image will use
the vertex rgba modulate values, as appropriate for misc_model
pre-lit surfaces.

Other lightmapIndex values will have a lightmap stage created
and src*dest blending applied with the texture, as appropriate for
most world construction surfaces.

===============
*/
shader_t *R_FindShader( const char *name, int lightmapIndex, qboolean mipRawImage ) {
	return R_FindShaderEx( name, lightmapIndex, mipRawImage, lightmapIndex );
}

/*
===============
R_ShaderSystemReady

HZM gl2 (bug-1145, "advanced settings apply crashes"): true once R_Init has run for the
CURRENT load of this DLL.

vid_restart tears the renderer DLL down and loads it again (cl_main.cpp CL_ShutdownRef ->
Sys_UnloadLibrary, CL_InitRef -> Sys_LoadDll), so every renderer global - including every
cvar_t* - is back to NULL. renderergl1 calls R_Init() from GetRefAPI, i.e. at DLL load, so
its cvars are live the moment CL_InitRef returns. rend2/gl2 calls R_Init() from
RE_BeginRegistration instead, which does not happen until CL_StartHunkUsers.

CL_Vid_Restart_f calls UI_ResolutionChange() BETWEEN those two points, and that walks the
UI's regged-material list re-registering every menu shader. Under gl2 those calls landed in
a completely uninitialized renderer and died on the first cvar read in InitShaderEx
(r_pbr->integer = offset 0x30 off a NULL cvar_t; verified from the minidump).

Refusing the registration is safe and complete: CL_StartHunkUsers calls UI_ResolutionChange()
a second time immediately after CL_BeginRegistration (cl_main.cpp), so every material the
UI asks for here is re-registered for real a few milliseconds later, against a live renderer.
Building shaders here would be pointless anyway - RE_BeginRegistration's ri.Hunk_Clear()
would free them again before the first frame.

Sentinel is tr.defaultShader, not tr.registered: R_Init zeroes tr and CreateInternalShaders
assigns tr.defaultShader, so it is non-NULL from partway through R_Init onwards - whereas
tr.registered only goes true at the END of RE_BeginRegistration and would wrongly refuse
legitimate registrations in between. CreateInternalShaders reaches InitShaderEx directly
(not via R_FindShader), so this never blocks the renderer's own bring-up.
===============
*/
static qboolean R_ShaderSystemReady( void ) {
	return (qboolean)(tr.defaultShader != NULL);
}

shader_t *R_FindShaderEx( const char *name, int lightmapIndex, qboolean mipRawImage, int realLightmapIndex ) {
	char		strippedName[MAX_QPATH];
	int			hash;
	char		*shaderText;
	image_t		*image;
	shader_t	*sh;

	if ( name[0] == 0 ) {
		return tr.defaultShader;
	}

	// use (fullbright) vertex lighting if the bsp file doesn't have
	// lightmaps
	if ( lightmapIndex >= 0 && lightmapIndex >= tr.numLightmaps ) {
		lightmapIndex = LIGHTMAP_BY_VERTEX;
	} else if ( lightmapIndex < LIGHTMAP_2D ) {
		// negative lightmap indexes cause stray pointers (think tr.lightmaps[lightmapIndex])
		ri.Printf( PRINT_WARNING, "WARNING: shader '%s' has invalid lightmap index of %d\n", name, lightmapIndex  );
		lightmapIndex = LIGHTMAP_BY_VERTEX;
	}

	COM_StripExtension(name, strippedName, sizeof(strippedName));

	hash = generateHashValue(strippedName, FILE_HASH_SIZE);

	//
	// see if the shader is already loaded
	//
	for (sh = hashTable[hash]; sh; sh = sh->next) {
		// NOTE: if there was no shader or image available with the name strippedName
		// then a default shader is created with lightmapIndex == LIGHTMAP_NONE, so we
		// have to check all default shaders otherwise for every call to R_FindShader
		// with that same strippedName a new default shader is created.
		if ( (sh->lightmapIndex == lightmapIndex || sh->defaultShader) &&
		     !Q_stricmp(sh->name, strippedName)) {
			// match found
			return sh;
		}
	}

	// HZM coop [bug-3493] a shader that has to be CREATED: if the map finished loading already this is a mid-game
	// load (file read, decode, upload on the frame it is needed) - remember it for this map's next load
	{
		extern void R_HzmPreloadNote( const char *name, int lightmapIndex, qboolean mipRawImage, int realLightmapIndex );
		R_HzmPreloadNote( strippedName, lightmapIndex, mipRawImage, realLightmapIndex );
	}

	InitShaderEx( strippedName, lightmapIndex, realLightmapIndex );

	//
	// attempt to define shader from an explicit parameter file
	//
	shaderText = FindShaderInShaderText( strippedName );
	if ( shaderText ) {
		// enable this when building a pak file to get a global list
		// of all explicit shaders
		if ( r_printShaders->integer ) {
			ri.Printf( PRINT_ALL, "*SHADER* %s\n", name );
		}

		if ( !ParseShader( &shaderText ) ) {
			// had errors, so use default shader
			shader.defaultShader = qtrue;
		}
		sh = FinishShader();
		return sh;
	}


	//
	// if not defined in the in-memory shader descriptions,
	// look for a single supported image file
	//
	{
		imgFlags_t flags;

		flags = IMGFLAG_NONE;

		if (mipRawImage)
		{
			flags |= IMGFLAG_MIPMAP | IMGFLAG_PICMIP;

			// HZM gl2 (r_hzmGenNormals). This is the implicit-shader path and it matters most
			// of the three: the majority of MOHAA BSP surfaces reference a bare texture path
			// with no .shader entry, land here, and get the two-pass lightmap stage pair below
			// - which CollapseStagesToGLSL folds into LIGHTDEF_USE_LIGHTMAP.
			if (r_genNormalMaps->integer || R_HZM_GenNormalsWanted( name ))
				flags |= IMGFLAG_GENNORMALMAP;
		}
		else
		{
			flags |= IMGFLAG_CLAMPTOEDGE;
		}

		image = R_FindImageFile( name, IMGTYPE_COLORALPHA, flags );
		if ( !image ) {
			ri.Printf( PRINT_DEVELOPER, "Couldn't find image file for shader %s\n", name );
			shader.defaultShader = qtrue;
			return FinishShader();
		}
	}

	//
	// create the default shading commands
	//
	if ( shader.lightmapIndex == LIGHTMAP_NONE ) {
		// dynamic colors at vertexes
		stages[0].bundle[0].image[0] = image;
		stages[0].active = qtrue;
		stages[0].rgbGen = CGEN_LIGHTING_DIFFUSE;
		stages[0].stateBits = GLS_DEFAULT;
	} else if ( shader.lightmapIndex == LIGHTMAP_BY_VERTEX ) {
		// explicit colors at vertexes
		stages[0].bundle[0].image[0] = image;
		stages[0].active = qtrue;
		stages[0].rgbGen = CGEN_EXACT_VERTEX;
		stages[0].alphaGen = AGEN_SKIP;
		stages[0].stateBits = GLS_DEFAULT;
	} else if ( shader.lightmapIndex == LIGHTMAP_2D ) {
		// GUI elements
		// HZM gl2 re-port Fix 2: match gl1 - default 2D pics take the current global 2D tint
		// (RE_SetColor / backEnd.color2D), not stock-ioq3 vertex color.
		stages[0].bundle[0].image[0] = image;
		stages[0].active = qtrue;
		stages[0].rgbGen = CGEN_GLOBAL_COLOR;
		stages[0].alphaGen = AGEN_GLOBAL_ALPHA;
		stages[0].stateBits = GLS_DEPTHTEST_DISABLE |
			  GLS_SRCBLEND_SRC_ALPHA |
			  GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA;
	} else if ( shader.lightmapIndex == LIGHTMAP_WHITEIMAGE ) {
		// fullbright level
		stages[0].bundle[0].image[0] = tr.whiteImage;
		stages[0].active = qtrue;
		stages[0].rgbGen = CGEN_IDENTITY_LIGHTING;
		stages[0].stateBits = GLS_DEFAULT;

		stages[1].bundle[0].image[0] = image;
		stages[1].active = qtrue;
		stages[1].rgbGen = CGEN_IDENTITY;
		stages[1].stateBits |= GLS_SRCBLEND_DST_COLOR | GLS_DSTBLEND_ZERO;
	} else {
		// two pass lightmap
		stages[0].bundle[0].image[0] = tr.lightmaps[shader.lightmapIndex];
		stages[0].bundle[0].isLightmap = qtrue;
		stages[0].active = qtrue;
		stages[0].rgbGen = CGEN_IDENTITY;	// lightmaps are scaled on creation
													// for identitylight
		stages[0].stateBits = GLS_DEFAULT;

		stages[1].bundle[0].image[0] = image;
		stages[1].active = qtrue;
		stages[1].rgbGen = CGEN_IDENTITY;
		stages[1].stateBits |= GLS_SRCBLEND_DST_COLOR | GLS_DSTBLEND_ZERO;
	}

	return FinishShader();
}


qhandle_t RE_RegisterShaderFromImage(const char *name, int lightmapIndex, image_t *image, qboolean mipRawImage) {
	// HZM gl2 (bug-1145): refuse registration before R_Init has built the shader system.
	// See R_ShaderSystemReady - vid_restart reloads this DLL and the UI re-registers its
	// materials before R_Init runs, which used to fault on a NULL cvar inside InitShaderEx.
	if ( !R_ShaderSystemReady() ) {
		return 0;
	}

	int			hash;
	shader_t	*sh;

	hash = generateHashValue(name, FILE_HASH_SIZE);

	// probably not necessary since this function
	// only gets called from tr_font.c with lightmapIndex == LIGHTMAP_2D
	// but better safe than sorry.
	if ( lightmapIndex >= tr.numLightmaps ) {
		lightmapIndex = LIGHTMAP_WHITEIMAGE;
	}

	//
	// see if the shader is already loaded
	//
	for (sh=hashTable[hash]; sh; sh=sh->next) {
		// NOTE: if there was no shader or image available with the name strippedName
		// then a default shader is created with lightmapIndex == LIGHTMAP_NONE, so we
		// have to check all default shaders otherwise for every call to R_FindShader
		// with that same strippedName a new default shader is created.
		if ( (sh->lightmapIndex == lightmapIndex || sh->defaultShader) &&
			// index by name
			!Q_stricmp(sh->name, name)) {
			// match found
			return sh->index;
		}
	}

	InitShader( name, lightmapIndex );

	//
	// create the default shading commands
	//
	if ( shader.lightmapIndex == LIGHTMAP_NONE ) {
		// dynamic colors at vertexes
		stages[0].bundle[0].image[0] = image;
		stages[0].active = qtrue;
		stages[0].rgbGen = CGEN_LIGHTING_DIFFUSE;
		stages[0].stateBits = GLS_DEFAULT;
	} else if ( shader.lightmapIndex == LIGHTMAP_BY_VERTEX ) {
		// explicit colors at vertexes
		stages[0].bundle[0].image[0] = image;
		stages[0].active = qtrue;
		stages[0].rgbGen = CGEN_EXACT_VERTEX;
		stages[0].alphaGen = AGEN_SKIP;
		stages[0].stateBits = GLS_DEFAULT;
	} else if ( shader.lightmapIndex == LIGHTMAP_2D ) {
		// GUI elements
		// HZM gl2 re-port Fix 2: match gl1 - default 2D pics take the current global 2D tint
		// (RE_SetColor / backEnd.color2D), not stock-ioq3 vertex color.
		stages[0].bundle[0].image[0] = image;
		stages[0].active = qtrue;
		stages[0].rgbGen = CGEN_GLOBAL_COLOR;
		stages[0].alphaGen = AGEN_GLOBAL_ALPHA;
		stages[0].stateBits = GLS_DEPTHTEST_DISABLE |
			  GLS_SRCBLEND_SRC_ALPHA |
			  GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA;
	} else if ( shader.lightmapIndex == LIGHTMAP_WHITEIMAGE ) {
		// fullbright level
		stages[0].bundle[0].image[0] = tr.whiteImage;
		stages[0].active = qtrue;
		stages[0].rgbGen = CGEN_IDENTITY_LIGHTING;
		stages[0].stateBits = GLS_DEFAULT;

		stages[1].bundle[0].image[0] = image;
		stages[1].active = qtrue;
		stages[1].rgbGen = CGEN_IDENTITY;
		stages[1].stateBits |= GLS_SRCBLEND_DST_COLOR | GLS_DSTBLEND_ZERO;
	} else {
		// two pass lightmap
		stages[0].bundle[0].image[0] = tr.lightmaps[shader.lightmapIndex];
		stages[0].bundle[0].isLightmap = qtrue;
		stages[0].active = qtrue;
		stages[0].rgbGen = CGEN_IDENTITY;	// lightmaps are scaled on creation
													// for identitylight
		stages[0].stateBits = GLS_DEFAULT;

		stages[1].bundle[0].image[0] = image;
		stages[1].active = qtrue;
		stages[1].rgbGen = CGEN_IDENTITY;
		stages[1].stateBits |= GLS_SRCBLEND_DST_COLOR | GLS_DSTBLEND_ZERO;
	}

	sh = FinishShader();
  return sh->index; 
}


/* 
====================
RE_RegisterShader

This is the exported shader entry point for the rest of the system
It will always return an index that will be valid.

This should really only be used for explicit shaders, because there is no
way to ask for different implicit lighting modes (vertex, lightmap, etc)
====================
*/
qhandle_t RE_RegisterShaderLightMap( const char *name, int lightmapIndex ) {
	// HZM gl2 (bug-1145): refuse registration before R_Init has built the shader system.
	// See R_ShaderSystemReady - vid_restart reloads this DLL and the UI re-registers its
	// materials before R_Init runs, which used to fault on a NULL cvar inside InitShaderEx.
	if ( !R_ShaderSystemReady() ) {
		return 0;
	}

	shader_t	*sh;

	if ( strlen( name ) >= MAX_QPATH ) {
		ri.Printf( PRINT_ALL, "Shader name exceeds MAX_QPATH\n" );
		return 0;
	}

	sh = R_FindShader( name, lightmapIndex, qtrue );

	// we want to return 0 if the shader failed to
	// load for some reason, but R_FindShader should
	// still keep a name allocated for it, so if
	// something calls RE_RegisterShader again with
	// the same name, we don't try looking for it again
	if ( sh->defaultShader ) {
		return 0;
	}

	return sh->index;
}


/* 
====================
RE_RegisterShader

This is the exported shader entry point for the rest of the system
It will always return an index that will be valid.

This should really only be used for explicit shaders, because there is no
way to ask for different implicit lighting modes (vertex, lightmap, etc)
====================
*/
qhandle_t RE_RegisterShader( const char *name ) {
	// HZM gl2 (bug-1145): refuse registration before R_Init has built the shader system.
	// See R_ShaderSystemReady - vid_restart reloads this DLL and the UI re-registers its
	// materials before R_Init runs, which used to fault on a NULL cvar inside InitShaderEx.
	if ( !R_ShaderSystemReady() ) {
		return 0;
	}

	shader_t	*sh;

	if ( strlen( name ) >= MAX_QPATH ) {
		ri.Printf( PRINT_ALL, "Shader name exceeds MAX_QPATH\n" );
		return 0;
	}

	sh = R_FindShader( name, LIGHTMAP_2D, qtrue );

	// we want to return 0 if the shader failed to
	// load for some reason, but R_FindShader should
	// still keep a name allocated for it, so if
	// something calls RE_RegisterShader again with
	// the same name, we don't try looking for it again
	if ( sh->defaultShader ) {
		return 0;
	}

	return sh->index;
}


/*
====================
RE_RegisterShaderNoMip

For menu graphics that should never be picmiped
====================
*/
qhandle_t RE_RegisterShaderNoMip( const char *name ) {
	// HZM gl2 (bug-1145): refuse registration before R_Init has built the shader system.
	// See R_ShaderSystemReady - vid_restart reloads this DLL and the UI re-registers its
	// materials before R_Init runs, which used to fault on a NULL cvar inside InitShaderEx.
	if ( !R_ShaderSystemReady() ) {
		return 0;
	}

	shader_t	*sh;

	if ( strlen( name ) >= MAX_QPATH ) {
		ri.Printf( PRINT_ALL, "Shader name exceeds MAX_QPATH\n" );
		return 0;
	}

	sh = R_FindShader( name, LIGHTMAP_2D, qfalse );

	// we want to return 0 if the shader failed to
	// load for some reason, but R_FindShader should
	// still keep a name allocated for it, so if
	// something calls RE_RegisterShader again with
	// the same name, we don't try looking for it again
	if ( sh->defaultShader ) {
		return 0;
	}

	return sh->index;
}

/*
====================
R_GetShaderByHandle

When a handle is passed in by another module, this range checks
it and returns a valid (possibly default) shader_t to be used internally.
====================
*/
shader_t *R_GetShaderByHandle( qhandle_t hShader ) {
	if ( hShader < 0 ) {
	  ri.Printf( PRINT_WARNING, "R_GetShaderByHandle: out of range hShader '%d'\n", hShader );
		return tr.defaultShader;
	}
	if ( hShader >= tr.numShaders ) {
		ri.Printf( PRINT_WARNING, "R_GetShaderByHandle: out of range hShader '%d'\n", hShader );
		return tr.defaultShader;
	}
	return tr.shaders[hShader];
}

/*
===============
R_ShaderList_f

Dump information on all valid shaders to the console
A second parameter will cause it to print in sorted order
===============
*/
void	R_ShaderList_f (void) {
	int			i;
	int			count;
	shader_t	*shader;

	ri.Printf (PRINT_ALL, "-----------------------\n");

	count = 0;
	for ( i = 0 ; i < tr.numShaders ; i++ ) {
		if ( ri.Cmd_Argc() > 1 ) {
			shader = tr.sortedShaders[i];
		} else {
			shader = tr.shaders[i];
		}

		ri.Printf( PRINT_ALL, "%i ", shader->numUnfoggedPasses );

		if (shader->lightmapIndex >= 0 ) {
			ri.Printf (PRINT_ALL, "L ");
		} else {
			ri.Printf (PRINT_ALL, "  ");
		}
		if ( shader->explicitlyDefined ) {
			ri.Printf( PRINT_ALL, "E " );
		} else {
			ri.Printf( PRINT_ALL, "  " );
		}

		if ( shader->optimalStageIteratorFunc == RB_StageIteratorGeneric ) {
			ri.Printf( PRINT_ALL, "gen " );
		} else if ( shader->optimalStageIteratorFunc == RB_StageIteratorSky ) {
			ri.Printf( PRINT_ALL, "sky " );
		} else {
			ri.Printf( PRINT_ALL, "    " );
		}

		if ( shader->defaultShader ) {
			ri.Printf (PRINT_ALL,  ": %s (DEFAULTED)\n", shader->name);
		} else {
			ri.Printf (PRINT_ALL,  ": %s\n", shader->name);
		}
		count++;
	}
	ri.Printf (PRINT_ALL, "%i total shaders\n", count);
	ri.Printf (PRINT_ALL, "------------------\n");
}

/*
====================
ScanAndLoadShaderFiles

Finds and loads all .shader files, combining them into
a single large text block that can be scanned for shader names
=====================
*/
#define	MAX_SHADER_FILES	4096
static void ScanAndLoadShaderFiles( void )
{
	char **shaderFiles;
	char *buffers[MAX_SHADER_FILES] = {NULL};
	char *p;
	int numShaderFiles;
	int i;
	char *oldp, *token, *hashMem, *textEnd;
	int shaderTextHashTableSizes[MAX_SHADERTEXT_HASH], hash, size;
	char shaderName[MAX_QPATH];
	int shaderLine;

	long sum = 0, summand;
	// scan for shader files
	shaderFiles = ri.FS_ListFiles( "scripts", ".shader", &numShaderFiles );

	if ( !shaderFiles || !numShaderFiles )
	{
		ri.Printf( PRINT_WARNING, "WARNING: no shader files found\n" );
		return;
	}

	if ( numShaderFiles > MAX_SHADER_FILES ) {
		numShaderFiles = MAX_SHADER_FILES;
	}

	// load and parse shader files
	for ( i = 0; i < numShaderFiles; i++ )
	{
		char filename[MAX_QPATH];

		// look for a .mtr file first
		{
			char *ext;
			Com_sprintf( filename, sizeof( filename ), "scripts/%s", shaderFiles[i] );
			if ( (ext = strrchr(filename, '.')) )
			{
				strcpy(ext, ".mtr");
			}

			if ( ri.FS_ReadFile( filename, NULL ) <= 0 )
			{
				Com_sprintf( filename, sizeof( filename ), "scripts/%s", shaderFiles[i] );
			}
		}
		
		ri.Printf( PRINT_DEVELOPER, "...loading '%s'\n", filename );
		summand = ri.FS_ReadFile( filename, (void **)&buffers[i] );
		
		if ( !buffers[i] )
			ri.Error( ERR_DROP, "Couldn't load %s", filename );
		
		// Do a simple check on the shader structure in that file to make sure one bad shader file cannot fuck up all other shaders.
		p = buffers[i];
		COM_BeginParseSession(filename);
		while(1)
		{
			token = COM_ParseExt(&p, qtrue);
			
			if(!*token)
				break;

			Q_strncpyz(shaderName, token, sizeof(shaderName));
			shaderLine = COM_GetCurrentParseLine();

			token = COM_ParseExt(&p, qtrue);
			if(token[0] != '{' || token[1] != '\0')
			{
				ri.Printf(PRINT_WARNING, "WARNING: Ignoring shader file %s. Shader \"%s\" on line %d missing opening brace",
							filename, shaderName, shaderLine);
				if (token[0])
				{
					ri.Printf(PRINT_WARNING, " (found \"%s\" on line %d)", token, COM_GetCurrentParseLine());
				}
				ri.Printf(PRINT_WARNING, ".\n");
				ri.FS_FreeFile(buffers[i]);
				buffers[i] = NULL;
				break;
			}

			if(!SkipBracedSection(&p, 1))
			{
				ri.Printf(PRINT_WARNING, "WARNING: Ignoring shader file %s. Shader \"%s\" on line %d missing closing brace.\n",
							filename, shaderName, shaderLine);
				ri.FS_FreeFile(buffers[i]);
				buffers[i] = NULL;
				break;
			}
		}
			
		
		if (buffers[i])
			sum += summand;		
	}

	// build single large buffer
	s_shaderText = ri.Hunk_Alloc( sum + numShaderFiles*2, h_low );
	s_shaderText[ 0 ] = '\0';
	textEnd = s_shaderText;
 
	// free in reverse order, so the temp files are all dumped
	for ( i = numShaderFiles - 1; i >= 0 ; i-- )
	{
		if ( !buffers[i] )
			continue;

		strcat( textEnd, buffers[i] );
		strcat( textEnd, "\n" );
		textEnd += strlen( textEnd );
		ri.FS_FreeFile( buffers[i] );
	}

	COM_Compress( s_shaderText );

	// free up memory
	ri.FS_FreeFileList( shaderFiles );

	Com_Memset(shaderTextHashTableSizes, 0, sizeof(shaderTextHashTableSizes));
	size = 0;

	p = s_shaderText;
	// look for shader names
	while ( 1 ) {
		token = COM_ParseExt( &p, qtrue );
		if ( token[0] == 0 ) {
			break;
		}

		hash = generateHashValue(token, MAX_SHADERTEXT_HASH);
		shaderTextHashTableSizes[hash]++;
		size++;
		SkipBracedSection(&p, 0);
	}

	size += MAX_SHADERTEXT_HASH;

	hashMem = ri.Hunk_Alloc( size * sizeof(char *), h_low );

	for (i = 0; i < MAX_SHADERTEXT_HASH; i++) {
		shaderTextHashTable[i] = (char **) hashMem;
		hashMem = ((char *) hashMem) + ((shaderTextHashTableSizes[i] + 1) * sizeof(char *));
	}

	Com_Memset(shaderTextHashTableSizes, 0, sizeof(shaderTextHashTableSizes));

	p = s_shaderText;
	// look for shader names
	while ( 1 ) {
		oldp = p;
		token = COM_ParseExt( &p, qtrue );
		if ( token[0] == 0 ) {
			break;
		}

		hash = generateHashValue(token, MAX_SHADERTEXT_HASH);
		shaderTextHashTable[hash][shaderTextHashTableSizes[hash]++] = oldp;

		SkipBracedSection(&p, 0);
	}

	return;

}


/*
====================
CreateInternalShaders
====================
*/
static void CreateInternalShaders( void ) {
	tr.numShaders = 0;

	// init the default shader
	InitShader( "<default>", LIGHTMAP_NONE );
	stages[0].bundle[0].image[0] = tr.defaultImage;
	stages[0].active = qtrue;
	stages[0].stateBits = GLS_DEFAULT;
	tr.defaultShader = FinishShader();

	// shadow shader is just a marker
	Q_strncpyz( shader.name, "<stencil shadow>", sizeof( shader.name ) );
	shader.sort = SS_STENCIL_SHADOW;
	tr.shadowShader = FinishShader();
}

static void CreateExternalShaders( void ) {
	tr.projectionShadowShader = R_FindShader( "projectionShadow", LIGHTMAP_NONE, qtrue );
	tr.flareShader = R_FindShader( "flareShader", LIGHTMAP_NONE, qtrue );

	// Hack to make fogging work correctly on flares. Fog colors are calculated
	// in tr_flare.c already.
	if(!tr.flareShader->defaultShader)
	{
		int index;
		
		for(index = 0; index < tr.flareShader->numUnfoggedPasses; index++)
		{
			tr.flareShader->stages[index]->adjustColorsForFog = ACFF_NONE;
			tr.flareShader->stages[index]->stateBits |= GLS_DEPTHTEST_DISABLE;
		}
	}

	// HZM gl2 [2026-09-26] Phase S2 LAMP FLARES (tr_hzm_spot_rb.c): the mod's own flare shader (scripts/
	// coop_headlights.shader, textures/coop_fx/lampflare.tga) - never flareShader, which the HD FX pack replaced with a
	// 2048 starburst (vet F16/F19). Set up like flareShader above: no depth test (occlusion is measured with queries),
	// no fog colour adjust, and no global fog (it is drawn in 2D; the flare pass multiplies fog transmittance itself).
	// An older pk3 without it leaves the default shader, and the flare pass then draws nothing (vet F7).
	tr.hzmLampFlareShader = R_FindShader( "hzmLampFlare", LIGHTMAP_NONE, qtrue );
	if ( !tr.hzmLampFlareShader->defaultShader )
	{
		int index;

		for ( index = 0; index < tr.hzmLampFlareShader->numUnfoggedPasses; index++ )
		{
			if ( tr.hzmLampFlareShader->stages[index] )
			{
				tr.hzmLampFlareShader->stages[index]->adjustColorsForFog = ACFF_NONE;
				tr.hzmLampFlareShader->stages[index]->stateBits |= GLS_DEPTHTEST_DISABLE;
			}
		}
		tr.hzmLampFlareShader->noGlobalFog = qtrue;
	}

	tr.sunShader = R_FindShader( "sun", LIGHTMAP_NONE, qtrue );

	tr.sunFlareShader = R_FindShader( "gfx/2d/sunflare", LIGHTMAP_NONE, qtrue);

	// HACK: if sunflare is missing, make one using the flare image or dlight image
	if (tr.sunFlareShader->defaultShader)
	{
		image_t *image;

		if (!tr.flareShader->defaultShader && tr.flareShader->stages[0] && tr.flareShader->stages[0]->bundle[0].image[0])
			image = tr.flareShader->stages[0]->bundle[0].image[0];
		else
			image = tr.dlightImage;

		InitShader( "gfx/2d/sunflare", LIGHTMAP_NONE );
		stages[0].bundle[0].image[0] = image;
		stages[0].active = qtrue;
		stages[0].stateBits = GLS_DEFAULT;
		tr.sunFlareShader = FinishShader();
	}

}

/*
==================
R_InitShaders
==================
*/
void R_InitShaders( void ) {
	ri.Printf( PRINT_ALL, "Initializing Shaders\n" );

	Com_Memset(hashTable, 0, sizeof(hashTable));

	CreateInternalShaders();

	ScanAndLoadShaderFiles();

	CreateExternalShaders();
}

//
// OPENMOHAA-specific stuff
//=========================

static void CreateMultistageFromBundle() {
    int stage, bundle;
	int i;

	for (stage = 0; stage < MAX_SHADER_STAGES; stage++ ) {
		shaderStage_t* pStage = &stages[stage];
		int blendBits;

		if (!pStage->active) {
			break;
		}

		if (!pStage->bundle[1].image[0]) {
			continue;	// single-bundle stage
		}

		// HZM gl2 re-port (bug-gl2-nextbundle2): gl1 defaults tcGen for EVERY
		// bundle (gl1 tr_shader.c:3161-3173); gl2's FinishShader loop only
		// touches bundle[0], so default both bundles here before deciding.
		for (bundle = 0; bundle < 2; bundle++) {
			if (pStage->bundle[bundle].tcGen == TCGEN_BAD) {
				pStage->bundle[bundle].tcGen =
					pStage->bundle[bundle].isLightmap ? TCGEN_LIGHTMAP : TCGEN_TEXTURE;
			}
		}

		// HZM gl2 re-port (bug-gl2-nextbundle2): lightmap-FIRST pair
		// ('map $lightmap' + nextbundle texture, mohcommon window32_lightmapalpha /
		// bordered_window_glass) - exchange bundles so the texture is the diffuse
		// (gl1 tr_shader.c:3175-3182); previously these drew the raw lightmap.
		if (pStage->bundle[0].isLightmap) {
			textureBundle_t tmpBundle = pStage->bundle[0];
			pStage->bundle[0] = pStage->bundle[1];
			pStage->bundle[1] = tmpBundle;
		}

		blendBits = pStage->stateBits & (GLS_SRCBLEND_BITS | GLS_DSTBLEND_BITS);

        if (pStage->bundle[TB_LIGHTMAP].isLightmap && !blendBits
			&& pStage->multitextureEnv != GL_ADD) {
			// opaque texture x lightmap: split into a separate DST_COLOR*ZERO
			// filter pass (original gl2 path). CollapseStagesToGLSL re-merges the
			// pair into one lightall draw; even unmerged the two passes composite
			// correctly because the opaque base wrote depth (DEPTHFUNC_EQUAL).
			shaderStage_t* newStage = NULL;

			for (i = stage; i < MAX_SHADER_STAGES; i++) {
				if (!stages[i].active) {
					newStage = &stages[i];
					break;
				}
			}

			if (newStage) {
				*newStage = *pStage;
				newStage->stateBits = GLS_SRCBLEND_DST_COLOR | GLS_DSTBLEND_ZERO | GLS_DEPTHFUNC_EQUAL;

                newStage->bundle[TB_COLORMAP] = pStage->bundle[TB_LIGHTMAP];
                memset(&newStage->bundle[TB_LIGHTMAP], 0, sizeof(textureBundle_t));
				memset(&pStage->bundle[TB_LIGHTMAP], 0, sizeof(textureBundle_t));
				newStage->multitextureEnv = 0;
				pStage->multitextureEnv = 0;
				stage = i;
			}
		} else {
			// HZM gl2 re-port (bug-gl2-nextbundle2): every other dual-bundle stage -
			// two-texture effects (tracers/snow/dust cones/waterfalls, modulate or
			// add) and BLENDED tex+lightmap stages (the dark-decal-rectangle class:
			// a split framebuffer-multiply pass would darken the whole quad outside
			// the decal's alpha shape) - renders in ONE pass through the generic
			// program's second bundle (u_Texture1Env), matching gl1's single-pass
			// multitexture (DrawMultitextured). Mark the stage ST_GLSL so
			// CollapseStagesToGLSL leaves it alone (it would repurpose
			// bundle[TB_LIGHTMAP] as a lightall lightmap and lose the 2nd texture).
			pStage->type = ST_GLSL;
			if (!pStage->multitextureEnv) {
				pStage->multitextureEnv = GL_MODULATE; // nextbundle default combine
			}
		}
	}
}

//=========================

qhandle_t RE_RefreshShaderNoMip(const char* name) {
	// HZM gl2 (bug-1145): refuse registration before R_Init has built the shader system.
	// See R_ShaderSystemReady - vid_restart reloads this DLL and the UI re-registers its
	// materials before R_Init runs, which used to fault on a NULL cvar inside InitShaderEx.
	if ( !R_ShaderSystemReady() ) {
		return 0;
	}

	shader_t* sh;
	char strippedName[64];
	int hash;

	if (!name || !*name) {
		return 0;
	}

    COM_StripExtension(name, strippedName, sizeof(strippedName));

	hash = generateHashValue(strippedName, FILE_HASH_SIZE);
	
	for (sh = hashTable[hash]; sh; sh = sh->next) {
		if (Q_stricmp(sh->name, strippedName) == 0) {
			image_t* image;

			image = sh->stages[0]->bundle[0].image[0];

			if (image) {
				sh->stages[0]->bundle[0].image[0] = R_RefreshImageFile(image->imgName, IMGTYPE_COLORALPHA, image->flags);
			}

			return sh->index;
		}
	}

    sh = R_FindShader(name, -4, qfalse);
	if (sh->defaultShader) {
		return 0;
	}

    return sh->index;
}