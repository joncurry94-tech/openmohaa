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
// tr_init.c -- functions that are not called every frame

#include "tr_local.h"

#include "tr_dsa.h"

glconfig_t  glConfig;
glRefConfig_t glRefConfig;
qboolean    textureFilterAnisotropic = qfalse;
int         maxAnisotropy = 0;
float       displayAspect = 0.0f;
qboolean    haveClampToEdge = qfalse;

glstate_t	glState;

static void GfxInfo_f( void );
static void GfxMemInfo_f( void );

#ifdef USE_RENDERER_DLOPEN
cvar_t  *com_altivec;
#endif

cvar_t	*r_flareSize;
cvar_t	*r_flareFade;
cvar_t	*r_flareCoeff;

cvar_t	*r_railWidth;
cvar_t	*r_railCoreWidth;
cvar_t	*r_railSegmentLength;

cvar_t	*r_verbose;
cvar_t	*r_ignore;

cvar_t  *r_displayRefresh;

cvar_t	*r_detailTextures;

cvar_t	*r_znear;
cvar_t	*r_zproj;
cvar_t	*r_stereoSeparation;

cvar_t	*r_skipBackEnd;

cvar_t	*r_stereoEnabled;
cvar_t	*r_anaglyphMode;

cvar_t	*r_greyscale;

cvar_t	*r_ignorehwgamma;
cvar_t	*r_measureOverdraw;

cvar_t	*r_inGameVideo;
cvar_t	*r_fastsky;
cvar_t	*r_drawSun;
cvar_t	*r_dynamiclight;
cvar_t	*r_dlightBacks;

cvar_t	*r_lodbias;
cvar_t	*r_lodscale;

cvar_t	*r_norefresh;
cvar_t	*r_drawentities;
cvar_t	*r_drawworld;
cvar_t	*r_speeds;
cvar_t	*r_fullbright;
cvar_t	*r_novis;
cvar_t	*r_nocull;
cvar_t	*r_facePlaneCull;
cvar_t	*r_showcluster;
cvar_t	*r_nocurves;

cvar_t	*r_allowExtensions;

cvar_t	*r_ext_compressed_textures;
cvar_t	*r_ext_multitexture;
cvar_t	*r_ext_compiled_vertex_array;
cvar_t	*r_ext_texture_env_add;
cvar_t	*r_ext_texture_filter_anisotropic;
cvar_t	*r_ext_max_anisotropy;

cvar_t  *r_ext_framebuffer_object;
cvar_t  *r_ext_texture_float;
cvar_t  *r_ext_framebuffer_multisample;
cvar_t  *r_ppDoF;   // HZM gl2 (bug-1157): gl1-parity depth of field, same lever name as renderergl1
cvar_t  *r_ppSSAO;            // HZM gl2 (bug-1177): gl1-parity SSAO master (same lever name as renderergl1)
cvar_t  *r_ppSSAORadius;      // world-scaled sample radius
cvar_t  *r_ppSSAOIntensity;   // darkening strength
cvar_t  *r_ppSSAOBias;        // depth bias (reduces self-occlusion)
cvar_t  *r_ppSSAODepthAware;  // edge-preserving (bilateral) AO blur
cvar_t  *r_arb_seamless_cube_map;
cvar_t  *r_arb_vertex_array_object;
cvar_t  *r_ext_direct_state_access;

cvar_t  *r_renderScale;      // HZM render-scale supersampling (0.5-2.0, latched); AMD FSR 1 filters
cvar_t  *r_upscaleFilter;
cvar_t  *r_fsrSharpness;
cvar_t  *r_renderScaleDebug;
cvar_t  *r_softParticles;      // HZM gl2 soft particles: sprites depth-fade near geometry
cvar_t  *r_softParticleDistance;
cvar_t  *r_softParticlesDebug;

cvar_t  *r_cameraExposure;

cvar_t  *r_externalGLSL;

cvar_t  *r_hdr;
cvar_t  *r_floatLightmap;
cvar_t  *r_postProcess;

cvar_t  *r_toneMap;
cvar_t  *r_forceToneMap;
cvar_t  *r_forceToneMapMin;
cvar_t  *r_forceToneMapAvg;
cvar_t  *r_forceToneMapMax;

cvar_t  *r_autoExposure;
cvar_t  *r_forceAutoExposure;
cvar_t  *r_forceAutoExposureMin;
cvar_t  *r_forceAutoExposureMax;

cvar_t  *r_depthPrepass;
cvar_t  *r_ssao;

cvar_t  *r_normalMapping;
cvar_t  *r_specularMapping;
cvar_t  *r_deluxeMapping;
cvar_t  *r_parallaxMapping;
cvar_t  *r_parallaxMapOffset;
cvar_t  *r_parallaxMapShadows;
cvar_t  *r_cubeMapping;
cvar_t  *r_cubemapSize;
cvar_t  *r_hzmAlphaGenCoord;
cvar_t  *r_hzmFlapDeform;
cvar_t  *r_hzmLightGlow;          // HZM gl2 [2026-09-25] Phase R - see tr_local.h
cvar_t  *r_hzmLightGlowNear;
cvar_t  *r_hzmLightGlowNearRange;
cvar_t  *r_hzmRgbGenDot;
cvar_t  *r_cubemapAuto;
cvar_t  *r_cubemapAutoRadius;
cvar_t  *r_deluxeSpecular;
cvar_t  *r_pbr;
cvar_t  *r_baseNormalX;
cvar_t  *r_baseNormalY;
cvar_t  *r_baseParallax;
cvar_t  *r_baseSpecular;
cvar_t  *r_baseGloss;
cvar_t  *r_glossType;
cvar_t  *r_mergeLightmaps;
cvar_t  *r_dlightMode;
cvar_t  *r_pshadowDist;
cvar_t  *r_imageUpsample;
cvar_t  *r_imageUpsampleMaxSize;
cvar_t  *r_imageUpsampleType;
cvar_t  *r_genNormalMaps;
cvar_t  *r_hzmGenNormals;			// HZM gl2 - targeted generated normal maps
cvar_t  *r_hzmGenNormalStrength;
cvar_t  *r_hzmGenNormalMaxSize;
cvar_t  *r_hzmGenNormalBlur;
cvar_t  *r_hzmGenNormalBrighten;
cvar_t  *r_hzmGenNormalInclude;
cvar_t  *r_hzmGenNormalExclude;
cvar_t  *r_hzmGenNormalDebug;
cvar_t  *r_hzmSpecular;
cvar_t  *r_hzmSpecularGloss;
cvar_t  *r_hzmParallaxDepth;
cvar_t  *r_hzmParallaxFade;
cvar_t  *r_hzmNormalStrength;
cvar_t  *r_hzmTerrainLightGrid;   // HZM [bug-3007]
cvar_t  *r_forceSun;
cvar_t  *r_forceSunLightScale;
cvar_t  *r_forceSunAmbientScale;
cvar_t  *r_sunlightMode;
cvar_t  *r_drawSunRays;
cvar_t  *r_sunShadows;
cvar_t  *r_shadowFilter;
cvar_t  *r_shadowBlur;
cvar_t  *r_shadowMapSize;
cvar_t  *r_shadowCascadeZNear;
cvar_t  *r_shadowCascadeZFar;
cvar_t  *r_shadowCascadeZBias;

// HZM gl2 real character shadows (see tr_local.h). r_charShadows is the master, default 0.
cvar_t  *r_charShadows;
cvar_t  *r_charShadowCascade;
cvar_t  *r_charShadowDist;
cvar_t  *r_charShadowLod;
cvar_t  *r_charShadowBiasFactor;
cvar_t  *r_charShadowBiasUnits;
cvar_t  *r_sphereCacheScope;
cvar_t  *r_charShadowBlob;
cvar_t  *r_shadowCastFoliage;
cvar_t  *r_shadowDebug;
cvar_t  *r_coopSunPublish;

// HZM gl2 character lighting (see tr_local.h). r_charLighting is the master, default 0.
cvar_t  *r_charLighting;
cvar_t  *r_charLightWrap;
cvar_t  *r_charLightShadow;
cvar_t  *r_charLightDebug;

// HZM gl2 [user 2026-09-09, bug-2554] entity light smoothing (see tr_local.h)
cvar_t  *r_entLightSmooth;
// HZM gl2 [user 2026-09-09, bug-2556] static-lamp admission fade (see tr_local.h)
cvar_t  *r_entLightFade;

// HZM gl2 dynamic-light cast shadows (see tr_local.h). r_hzmDlightShadows is the master, default 0.
cvar_t  *r_hzmDlightShadows;
cvar_t  *r_hzmDlightShadowLights;
cvar_t  *r_hzmDlightShadowMax;
cvar_t  *r_hzmDlightShadowDist;
cvar_t  *r_hzmDlightShadowMinRadius;
cvar_t  *r_hzmDlightShadowCasters;
cvar_t  *r_hzmDlightShadowChars;
cvar_t  *r_hzmDlightShadowDebug;

cvar_t  *r_ignoreDstAlpha;

cvar_t	*r_ignoreGLErrors;
cvar_t	*r_logFile;

cvar_t	*r_stencilbits;
cvar_t	*r_depthbits;
cvar_t	*r_colorbits;
cvar_t	*r_texturebits;
cvar_t  *r_ext_multisample;

cvar_t	*r_drawBuffer;
cvar_t	*r_lightmap;
cvar_t	*r_vertexLight;
cvar_t	*r_uiFullScreen;
cvar_t	*r_shadows;
cvar_t	*r_flares;
cvar_t	*r_mode;
cvar_t	*r_nobind;
cvar_t	*r_singleShader;
cvar_t	*r_roundImagesDown;
cvar_t	*r_colorMipLevels;
cvar_t	*r_picmip;
cvar_t	*r_showtris;
cvar_t	*r_showsky;
cvar_t	*r_shownormals;
cvar_t	*r_finish;
cvar_t	*r_clear;
cvar_t	*r_swapInterval;
cvar_t	*r_textureMode;
cvar_t	*r_offsetFactor;
cvar_t	*r_offsetUnits;
cvar_t	*r_shadowMapBiasFactor;
cvar_t	*r_shadowMapBiasUnits;
cvar_t	*r_gamma;
cvar_t	*r_intensity;
cvar_t	*r_lockpvs;
cvar_t	*r_noportals;
cvar_t	*r_portalOnly;

cvar_t	*r_subdivisions;
cvar_t	*r_lodCurveError;

cvar_t	*r_fullscreen;
cvar_t  *r_noborder;

cvar_t	*r_customwidth;
cvar_t	*r_customheight;
cvar_t	*r_customPixelAspect;

cvar_t	*r_overBrightBits;
cvar_t	*r_mapOverBrightBits;
cvar_t	*r_mapOverBrightScale;   // HZM gl2 [bug-2559] float trim on the above

cvar_t	*r_debugSurface;
cvar_t	*r_simpleMipMaps;

cvar_t	*r_showImages;

cvar_t	*r_ambientScale;
cvar_t	*r_directedScale;
cvar_t	*r_debugLight;
cvar_t	*r_debugSort;
cvar_t	*r_printShaders;
cvar_t	*r_saveFontData;

cvar_t	*r_marksOnTriangleMeshes;

cvar_t	*r_vaoCache;

cvar_t	*r_aviMotionJpegQuality;
cvar_t	*r_screenshotJpegQuality;

cvar_t	*r_maxpolys;
int		max_polys;
cvar_t	*r_maxpolyverts;
int		max_polyverts;

//
// OPENMOHAA-specific stuff
//=========================

int r_sequencenumber;

// DRAWING

cvar_t *r_drawentitypoly;
cvar_t *r_drawstaticmodels;
cvar_t *r_drawstaticmodelpoly;
cvar_t *r_drawstaticdecals;
cvar_t *r_drawterrain;
cvar_t *r_drawsprites;
cvar_t *r_drawspherelights;
// HZM gl2 re-port (bug-gl2-modellight)
cvar_t *r_fastentlight;

// HZM coop - gore tier 4 (UV wounds) - HZM gl2 re-port (bug-gl2-gore), gl1 tr_init.c:107-111
cvar_t *r_goreUV;			// HZM coop - gore tier 4 (UV wounds)
cvar_t *r_goreDebug;		// HZM coop - gore tier 4 dev counters (bug-735)
cvar_t *coop_goreSkinWoundScale;	// HZM coop - EXPOSED-SKIN wound size multiplier (1.0 = off)
cvar_t *coop_goreSkinSnap;			// HZM coop - skin-snap fallback for moving enemies (0 = off)
cvar_t *coop_goreSkinSnapDist;		// HZM coop - skin-snap vertex tolerance (model units, clamped 8-64)

cvar_t *r_numdebuglines;
cvar_t *r_stipplelines;
cvar_t *r_debuglines_depthmask;

cvar_t	*r_maxpolys;
int		max_polys;
cvar_t	*r_maxpolyverts;
int		max_polyverts;
cvar_t* r_maxtermarks;
int		max_termarks;

cvar_t* r_skyportal;
cvar_t* r_skyportal_origin;
// HZM gl2 re-port Fix 3: ADS view-weapon projection cvars (names must match gl1/cgame exactly)
cvar_t* r_weaponfovx;
cvar_t* r_weaponznear;
cvar_t* r_weaponshifty;
cvar_t* r_weaponshiftx;
cvar_t* r_farplane;
cvar_t* r_farplane_bias;
cvar_t* r_farplane_color;
cvar_t* r_farplane_nocull;
cvar_t* r_farplane_nofog;
cvar_t* r_skybox_farplane;
cvar_t* r_farclip;

// HZM gl2 fog parity - see tr_local.h / tr_postprocess.c RB_GlobalFog
cvar_t* r_globalFog;
cvar_t* r_globalFogScale;
cvar_t* r_globalFogStartScale;
cvar_t* r_globalFogEndScale;
cvar_t* r_globalFogSky;
cvar_t* r_hzmSkyFog;
cvar_t* r_hzmSkyFogHeight;
cvar_t* r_hzmSkyFogPortal;
cvar_t* r_skyHD;
cvar_t* r_skyHDCompare;
cvar_t* r_globalFogRadial;
cvar_t* r_globalFogIdentityLight;
cvar_t* r_globalFogDebug;
cvar_t* r_globalFogForward;

// Lighting

cvar_t* r_lightcoronasize;
cvar_t *r_entlight_scale;
cvar_t *r_entlight_errbound;
cvar_t *r_entlight_cubelevel;
cvar_t *r_entlight_cubefraction;
cvar_t *r_entlight_maxcalc;
cvar_t *r_light_lines;
cvar_t *r_light_sun_line;
cvar_t *r_light_int_scale;
cvar_t *r_light_nolight;
cvar_t *r_light_showgrid;

// LOD

cvar_t* r_staticlod;
cvar_t* r_lodscale;
cvar_t* r_lodcap;
cvar_t* r_lodviewmodelcap;

cvar_t* r_uselod;
cvar_t* lod_LOD;
cvar_t* lod_minLOD;
cvar_t* lod_maxLOD;
cvar_t* lod_LOD_slider;
cvar_t* lod_curve_0_val;
cvar_t* lod_curve_1_val;
cvar_t* lod_curve_2_val;
cvar_t* lod_curve_3_val;
cvar_t* lod_curve_4_val;
cvar_t* lod_edit_0;
cvar_t* lod_edit_1;
cvar_t* lod_edit_2;
cvar_t* lod_edit_3;
cvar_t* lod_edit_4;
cvar_t* lod_curve_0_slider;
cvar_t* lod_curve_1_slider;
cvar_t* lod_curve_2_slider;
cvar_t* lod_curve_3_slider;
cvar_t* lod_curve_4_slider;
cvar_t* lod_pitch_val;
cvar_t* lod_zee_val;
cvar_t* lod_mesh;
cvar_t* lod_meshname;
cvar_t* lod_tikiname;
cvar_t* lod_metric;
cvar_t* lod_tris;
cvar_t* lod_position;
cvar_t* lod_save;
cvar_t* lod_tool;

// Utils

cvar_t* r_developer;
cvar_t* r_fps;
cvar_t* r_showstaticbboxes;
cvar_t* r_showcull;
cvar_t* r_showlod;
cvar_t* r_showstaticlod;
cvar_t* r_showportal;

//=========================

/*
** InitOpenGL
**
** This function is responsible for initializing a valid OpenGL subsystem.  This
** is done by calling GLimp_Init (which gives us a working OGL subsystem) then
** setting variables, checking GL constants, and reporting the gfx system config
** to the user.
*/
static void InitOpenGL( void )
{
	//
	// initialize OS specific portions of the renderer
	//
	// GLimp_Init directly or indirectly references the following cvars:
	//		- r_fullscreen
	//		- r_mode
	//		- r_(color|depth|stencil)bits
	//		- r_ignorehwgamma
	//		- r_gamma
	//
	
	if ( glConfig.vidWidth == 0 )
	{
		GLint		temp;
		
		// HZM gl2 MSAA (plan P2a, ME-F15): the new path asks for a single-sample window. Decided from the cvars
		// alone (there is no context yet); the legacy path keeps r_ext_multisample, as today.
		glimp_windowSamplesOverride = R_MsaaNewPathRequested() ? 0 : -1;

		GLimp_Init( qfalse );
		GLimp_InitExtraExtensions();

		glConfig.textureEnvAddAvailable = qtrue;

		// OpenGL driver constants
		qglGetIntegerv( GL_MAX_TEXTURE_SIZE, &temp );
		glConfig.maxTextureSize = temp;

		// stubbed or broken drivers may have reported 0...
		if ( glConfig.maxTextureSize <= 0 ) 
		{
			glConfig.maxTextureSize = 0;
		}

		qglGetIntegerv( GL_MAX_TEXTURE_IMAGE_UNITS, &temp );
		glConfig.numTextureUnits = temp;

		qglGetIntegerv( GL_MAX_VERTEX_ATTRIBS, &temp );
		glRefConfig.maxVertexAttribs = temp;

		// reserve 160 components for other uniforms
		if ( qglesMajorVersion ) {
			qglGetIntegerv( GL_MAX_VERTEX_UNIFORM_VECTORS, &temp );
			temp *= 4;
		} else {
			qglGetIntegerv( GL_MAX_VERTEX_UNIFORM_COMPONENTS, &temp );
		}
		glRefConfig.glslMaxAnimatedBones = Com_Clamp( 0, IQM_MAX_JOINTS, ( temp - 160 ) / 16 );
		if ( glRefConfig.glslMaxAnimatedBones < 12 ) {
			glRefConfig.glslMaxAnimatedBones = 0;
		}
	}

	// check for GLSL function textureCubeLod()
	if ( r_cubeMapping->integer && !QGL_VERSION_ATLEAST( 3, 0 ) ) {
		ri.Printf( PRINT_WARNING, "WARNING: Disabled r_cubeMapping because it requires OpenGL 3.0\n" );
		ri.Cvar_Set( "r_cubeMapping", "0" );
	}

	// set default state
	GL_SetDefaultState();
}

/*
==================
GL_CheckErrors
==================
*/
void GL_CheckErrs( char *file, int line ) {
	int		err;
	char	s[64];

	err = qglGetError();
	if ( err == GL_NO_ERROR ) {
		return;
	}
	if ( r_ignoreGLErrors->integer ) {
		return;
	}
	switch( err ) {
		case GL_INVALID_ENUM:
			strcpy( s, "GL_INVALID_ENUM" );
			break;
		case GL_INVALID_VALUE:
			strcpy( s, "GL_INVALID_VALUE" );
			break;
		case GL_INVALID_OPERATION:
			strcpy( s, "GL_INVALID_OPERATION" );
			break;
		case GL_STACK_OVERFLOW:
			strcpy( s, "GL_STACK_OVERFLOW" );
			break;
		case GL_STACK_UNDERFLOW:
			strcpy( s, "GL_STACK_UNDERFLOW" );
			break;
		case GL_OUT_OF_MEMORY:
			strcpy( s, "GL_OUT_OF_MEMORY" );
			break;
		default:
			Com_sprintf( s, sizeof(s), "%i", err);
			break;
	}

	ri.Error( ERR_FATAL, "GL_CheckErrors: %s in %s at line %d", s , file, line);
}


/*
** R_GetModeInfo
*/
typedef struct vidmode_s
{
	const char *description;
	int width, height;
	float pixelAspect;		// pixel width / height
} vidmode_t;

vidmode_t r_vidModes[] =
{
	{ "Mode  0: 320x240",		320,	240,	1 },
	{ "Mode  1: 400x300",		400,	300,	1 },
	{ "Mode  2: 512x384",		512,	384,	1 },
	{ "Mode  3: 640x480",		640,	480,	1 },
	{ "Mode  4: 800x600",		800,	600,	1 },
	{ "Mode  5: 960x720",		960,	720,	1 },
	{ "Mode  6: 1024x768",		1024,	768,	1 },
	{ "Mode  7: 1152x864",		1152,	864,	1 },
	{ "Mode  8: 1280x1024",		1280,	1024,	1 },
	{ "Mode  9: 1600x1200",		1600,	1200,	1 },
	{ "Mode 10: 2048x1536",		2048,	1536,	1 },
	{ "Mode 11: 856x480 (wide)",856,	480,	1 }
};
static int	s_numVidModes = ARRAY_LEN( r_vidModes );

qboolean R_GetModeInfo( int *width, int *height, float *windowAspect, int mode ) {
	vidmode_t	*vm;
	float			pixelAspect;

	if ( mode < -1 ) {
		return qfalse;
	}
	if ( mode >= s_numVidModes ) {
		return qfalse;
	}

	if ( mode == -1 ) {
		*width = r_customwidth->integer;
		*height = r_customheight->integer;
		pixelAspect = r_customPixelAspect->value;
	} else {
		vm = &r_vidModes[mode];

		*width  = vm->width;
		*height = vm->height;
		pixelAspect = vm->pixelAspect;
	}

	*windowAspect = (float)*width / ( *height * pixelAspect );

	return qtrue;
}

/*
** R_ModeList_f
*/
static void R_ModeList_f( void )
{
	int i;

	ri.Printf( PRINT_ALL, "\n" );
	for ( i = 0; i < s_numVidModes; i++ )
	{
		ri.Printf( PRINT_ALL, "%s\n", r_vidModes[i].description );
	}
	ri.Printf( PRINT_ALL, "\n" );
}


/* 
============================================================================== 
 
						SCREEN SHOTS 

NOTE TTimo
some thoughts about the screenshots system:
screenshots get written in fs_homepath + fs_gamedir
vanilla q3 .. baseq3/screenshots/ *.tga
team arena .. missionpack/screenshots/ *.tga

two commands: "screenshot" and "screenshotJPEG"
we use statics to store a count and start writing the first screenshot/screenshot????.tga (.jpg) available
(with FS_FileExists / FS_FOpenFileWrite calls)
FIXME: the statics don't get a reinit between fs_game changes

============================================================================== 
*/ 

/* 
================== 
RB_ReadPixels

Reads an image but takes care of alignment issues for reading RGB images.

Reads a minimum offset for where the RGB data starts in the image from
integer stored at pointer offset. When the function has returned the actual
offset was written back to address offset. This address will always have an
alignment of packAlign to ensure efficient copying.

Stores the length of padding after a line of pixels to address padlen

Return value must be freed with ri.Hunk_FreeTempMemory()
================== 
*/  

byte *RB_ReadPixels(int x, int y, int width, int height, size_t *offset, int *padlen)
{
	byte *buffer, *bufstart;
	int padwidth, linelen, bytesPerPixel;
	int yin, xin, xout;
	GLint packAlign, format;

	// OpenGL ES is only required to support reading GL_RGBA
	if (qglesMajorVersion >= 1) {
		format = GL_RGBA;
		bytesPerPixel = 4;
	} else {
		format = GL_RGB;
		bytesPerPixel = 3;
	}

	qglGetIntegerv(GL_PACK_ALIGNMENT, &packAlign);

	linelen = width * bytesPerPixel;
	padwidth = PAD(linelen, packAlign);

	// Allocate a few more bytes so that we can choose an alignment we like
	buffer = ri.Hunk_AllocateTempMemory(padwidth * height + *offset + packAlign - 1);

	bufstart = PADP((intptr_t) buffer + *offset, packAlign);
	qglReadPixels(x, y, width, height, format, GL_UNSIGNED_BYTE, bufstart);

	linelen = width * 3;

	// Convert RGBA to RGB, in place, line by line
	if (format == GL_RGBA) {
		for (yin = 0; yin < height; yin++) {
			for (xin = 0, xout = 0; xout < linelen; xin += 4, xout += 3) {
				bufstart[yin*padwidth + xout + 0] = bufstart[yin*padwidth + xin + 0];
				bufstart[yin*padwidth + xout + 1] = bufstart[yin*padwidth + xin + 1];
				bufstart[yin*padwidth + xout + 2] = bufstart[yin*padwidth + xin + 2];
			}
		}
	}

	*offset = bufstart - buffer;
	*padlen = padwidth - linelen;
	
	return buffer;
}

/* 
================== 
RB_TakeScreenshot
================== 
*/  
void RB_TakeScreenshot(int x, int y, int width, int height, char *fileName)
{
	byte *allbuf, *buffer;
	byte *srcptr, *destptr;
	byte *endline, *endmem;
	byte temp;
	
	int linelen, padlen;
	size_t offset = 18, memcount;
		
	allbuf = RB_ReadPixels(x, y, width, height, &offset, &padlen);
	buffer = allbuf + offset - 18;
	
	Com_Memset (buffer, 0, 18);
	buffer[2] = 2;		// uncompressed type
	buffer[12] = width & 255;
	buffer[13] = width >> 8;
	buffer[14] = height & 255;
	buffer[15] = height >> 8;
	buffer[16] = 24;	// pixel size

	// swap rgb to bgr and remove padding from line endings
	linelen = width * 3;
	
	srcptr = destptr = allbuf + offset;
	endmem = srcptr + (linelen + padlen) * height;
	
	while(srcptr < endmem)
	{
		endline = srcptr + linelen;

		while(srcptr < endline)
		{
			temp = srcptr[0];
			*destptr++ = srcptr[2];
			*destptr++ = srcptr[1];
			*destptr++ = temp;
			
			srcptr += 3;
		}
		
		// Skip the pad
		srcptr += padlen;
	}

	memcount = linelen * height;

	// gamma correct
	if(glConfig.deviceSupportsGamma)
		R_GammaCorrect(allbuf + offset, memcount);

	ri.FS_WriteFile(fileName, buffer, memcount + 18);

	ri.Hunk_FreeTempMemory(allbuf);
}

/* 
================== 
RB_TakeScreenshotJPEG
================== 
*/

void RB_TakeScreenshotJPEG(int x, int y, int width, int height, char *fileName)
{
	byte *buffer;
	size_t offset = 0, memcount;
	int padlen;

	buffer = RB_ReadPixels(x, y, width, height, &offset, &padlen);
	memcount = (width * 3 + padlen) * height;

	// gamma correct
	if(glConfig.deviceSupportsGamma)
		R_GammaCorrect(buffer + offset, memcount);

	RE_SaveJPG(fileName, r_screenshotJpegQuality->integer, width, height, buffer + offset, padlen);
	ri.Hunk_FreeTempMemory(buffer);
}

/*
==================
RB_TakeScreenshotCmd
==================
*/
const void *RB_TakeScreenshotCmd( const void *data ) {
	const screenshotCommand_t	*cmd;
	
	cmd = (const screenshotCommand_t *)data;

	// finish any 2D drawing if needed
	if(tess.numIndexes)
		RB_EndSurface();

	if (cmd->jpeg)
		RB_TakeScreenshotJPEG( cmd->x, cmd->y, cmd->width, cmd->height, cmd->fileName);
	else
		RB_TakeScreenshot( cmd->x, cmd->y, cmd->width, cmd->height, cmd->fileName);
	
	return (const void *)(cmd + 1);	
}

/*
==================
R_TakeScreenshot
==================
*/
void R_TakeScreenshot( int x, int y, int width, int height, char *name, qboolean jpeg ) {
	static char	fileName[MAX_OSPATH]; // bad things if two screenshots per frame?
	screenshotCommand_t	*cmd;

	cmd = R_GetCommandBuffer( sizeof( *cmd ) );
	if ( !cmd ) {
		return;
	}
	cmd->commandId = RC_SCREENSHOT;

	cmd->x = x;
	cmd->y = y;
	cmd->width = width;
	cmd->height = height;
	Q_strncpyz( fileName, name, sizeof(fileName) );
	cmd->fileName = fileName;
	cmd->jpeg = jpeg;
}

/* 
================== 
R_ScreenshotFilename
================== 
*/  
void R_ScreenshotFilename( int lastNumber, char *fileName ) {
	int		a,b,c,d;

	if ( lastNumber < 0 || lastNumber > 9999 ) {
		Com_sprintf( fileName, MAX_OSPATH, "screenshots/shot9999.tga" );
		return;
	}

	a = lastNumber / 1000;
	lastNumber -= a*1000;
	b = lastNumber / 100;
	lastNumber -= b*100;
	c = lastNumber / 10;
	lastNumber -= c*10;
	d = lastNumber;

	Com_sprintf( fileName, MAX_OSPATH, "screenshots/shot%i%i%i%i.tga"
		, a, b, c, d );
}

/* 
================== 
R_ScreenshotFilename
================== 
*/  
void R_ScreenshotFilenameJPEG( int lastNumber, char *fileName ) {
	int		a,b,c,d;

	if ( lastNumber < 0 || lastNumber > 9999 ) {
		Com_sprintf( fileName, MAX_OSPATH, "screenshots/shot9999.jpg" );
		return;
	}

	a = lastNumber / 1000;
	lastNumber -= a*1000;
	b = lastNumber / 100;
	lastNumber -= b*100;
	c = lastNumber / 10;
	lastNumber -= c*10;
	d = lastNumber;

	Com_sprintf( fileName, MAX_OSPATH, "screenshots/shot%i%i%i%i.jpg"
		, a, b, c, d );
}

/*
====================
R_LevelShot

levelshots are specialized 128*128 thumbnails for
the menu system, sampled down from full screen distorted images
====================
*/
void R_LevelShot( void ) {
	char		checkname[MAX_OSPATH];
	byte		*buffer;
	byte		*source, *allsource;
	byte		*src, *dst;
	size_t			offset = 0;
	int			padlen;
	int			x, y;
	int			r, g, b;
	float		xScale, yScale;
	int			xx, yy;

	Com_sprintf(checkname, sizeof(checkname), "levelshots/%s.tga", tr.world->baseName);

	allsource = RB_ReadPixels(0, 0, glConfig.vidWidth, glConfig.vidHeight, &offset, &padlen);
	source = allsource + offset;

	buffer = ri.Hunk_AllocateTempMemory(128 * 128*3 + 18);
	Com_Memset (buffer, 0, 18);
	buffer[2] = 2;		// uncompressed type
	buffer[12] = 128;
	buffer[14] = 128;
	buffer[16] = 24;	// pixel size

	// resample from source
	xScale = glConfig.vidWidth / 512.0f;
	yScale = glConfig.vidHeight / 384.0f;
	for ( y = 0 ; y < 128 ; y++ ) {
		for ( x = 0 ; x < 128 ; x++ ) {
			r = g = b = 0;
			for ( yy = 0 ; yy < 3 ; yy++ ) {
				for ( xx = 0 ; xx < 4 ; xx++ ) {
					src = source + (3 * glConfig.vidWidth + padlen) * (int)((y*3 + yy) * yScale) +
						3 * (int) ((x*4 + xx) * xScale);
					r += src[0];
					g += src[1];
					b += src[2];
				}
			}
			dst = buffer + 18 + 3 * ( y * 128 + x );
			dst[0] = b / 12;
			dst[1] = g / 12;
			dst[2] = r / 12;
		}
	}

	// gamma correct
	if ( glConfig.deviceSupportsGamma ) {
		R_GammaCorrect( buffer + 18, 128 * 128 * 3 );
	}

	ri.FS_WriteFile( checkname, buffer, 128 * 128*3 + 18 );

	ri.Hunk_FreeTempMemory(buffer);
	ri.Hunk_FreeTempMemory(allsource);

	ri.Printf( PRINT_ALL, "Wrote %s\n", checkname );
}

/* 
================== 
R_ScreenShot_f

screenshot
screenshot [silent]
screenshot [levelshot]
screenshot [filename]

Doesn't print the pacifier message if there is a second arg
================== 
*/  
void R_ScreenShot_f (void) {
	char	checkname[MAX_OSPATH];
	static	int	lastNumber = -1;
	qboolean	silent;

	if ( !strcmp( ri.Cmd_Argv(1), "levelshot" ) ) {
		R_LevelShot();
		return;
	}

	if ( !strcmp( ri.Cmd_Argv(1), "silent" ) ) {
		silent = qtrue;
	} else {
		silent = qfalse;
	}

	if ( ri.Cmd_Argc() == 2 && !silent ) {
		// explicit filename
		Com_sprintf( checkname, MAX_OSPATH, "screenshots/%s.tga", ri.Cmd_Argv( 1 ) );
	} else {
		// scan for a free filename

		// if we have saved a previous screenshot, don't scan
		// again, because recording demo avis can involve
		// thousands of shots
		if ( lastNumber == -1 ) {
			lastNumber = 0;
		}
		// scan for a free number
		for ( ; lastNumber <= 9999 ; lastNumber++ ) {
			R_ScreenshotFilename( lastNumber, checkname );

      if (!ri.FS_FileExists( checkname ))
      {
        break; // file doesn't exist
      }
		}

		if ( lastNumber >= 9999 ) {
			ri.Printf (PRINT_ALL, "ScreenShot: Couldn't create a file\n"); 
			return;
 		}

		lastNumber++;
	}

	R_TakeScreenshot( 0, 0, glConfig.vidWidth, glConfig.vidHeight, checkname, qfalse );

	if ( !silent ) {
		ri.Printf (PRINT_ALL, "Wrote %s\n", checkname);
	}
} 

void R_ScreenShotJPEG_f (void) {
	char		checkname[MAX_OSPATH];
	static	int	lastNumber = -1;
	qboolean	silent;

	if ( !strcmp( ri.Cmd_Argv(1), "levelshot" ) ) {
		R_LevelShot();
		return;
	}

	if ( !strcmp( ri.Cmd_Argv(1), "silent" ) ) {
		silent = qtrue;
	} else {
		silent = qfalse;
	}

	if ( ri.Cmd_Argc() == 2 && !silent ) {
		// explicit filename
		Com_sprintf( checkname, MAX_OSPATH, "screenshots/%s.jpg", ri.Cmd_Argv( 1 ) );
	} else {
		// scan for a free filename

		// if we have saved a previous screenshot, don't scan
		// again, because recording demo avis can involve
		// thousands of shots
		if ( lastNumber == -1 ) {
			lastNumber = 0;
		}
		// scan for a free number
		for ( ; lastNumber <= 9999 ; lastNumber++ ) {
			R_ScreenshotFilenameJPEG( lastNumber, checkname );

      if (!ri.FS_FileExists( checkname ))
      {
        break; // file doesn't exist
      }
		}

		if ( lastNumber == 10000 ) {
			ri.Printf (PRINT_ALL, "ScreenShot: Couldn't create a file\n"); 
			return;
 		}

		lastNumber++;
	}

	R_TakeScreenshot( 0, 0, glConfig.vidWidth, glConfig.vidHeight, checkname, qtrue );

	if ( !silent ) {
		ri.Printf (PRINT_ALL, "Wrote %s\n", checkname);
	}
} 

//============================================================================

/*
==================
R_ExportCubemaps
==================
*/
void R_ExportCubemaps(void)
{
	exportCubemapsCommand_t	*cmd;

	cmd = R_GetCommandBuffer(sizeof(*cmd));
	if (!cmd) {
		return;
	}
	cmd->commandId = RC_EXPORT_CUBEMAPS;
}


/*
==================
R_ExportCubemaps_f
==================
*/
void R_ExportCubemaps_f(void)
{
	R_ExportCubemaps();
}

//============================================================================

/*
==================
RB_TakeVideoFrameCmd
==================
*/
const void *RB_TakeVideoFrameCmd( const void *data )
{
	const videoFrameCommand_t	*cmd;
	byte				*cBuf;
	size_t				memcount, bytesPerPixel, linelen, avilinelen;
	int				padwidth, avipadwidth, padlen, avipadlen;
	int				yin, xin, xout;
	GLint packAlign, format;

	// finish any 2D drawing if needed
	if(tess.numIndexes)
		RB_EndSurface();

	cmd = (const videoFrameCommand_t *)data;
	
	// OpenGL ES is only required to support reading GL_RGBA
	if (qglesMajorVersion >= 1) {
		format = GL_RGBA;
		bytesPerPixel = 4;
	} else {
		format = GL_RGB;
		bytesPerPixel = 3;
	}

	qglGetIntegerv(GL_PACK_ALIGNMENT, &packAlign);

	linelen = cmd->width * bytesPerPixel;

	// Alignment stuff for glReadPixels
	padwidth = PAD(linelen, packAlign);
	padlen = padwidth - linelen;

	avilinelen = cmd->width * 3;

	// AVI line padding
	avipadwidth = PAD(avilinelen, AVI_LINE_PADDING);
	avipadlen = avipadwidth - avilinelen;

	cBuf = PADP(cmd->captureBuffer, packAlign);
		
	qglReadPixels(0, 0, cmd->width, cmd->height, format,
		GL_UNSIGNED_BYTE, cBuf);

	memcount = padwidth * cmd->height;

	// gamma correct
	if(glConfig.deviceSupportsGamma)
		R_GammaCorrect(cBuf, memcount);

	if(cmd->motionJpeg)
	{
		// Convert RGBA to RGB, in place, line by line
		if (format == GL_RGBA) {
			linelen = cmd->width * 3;
			padlen = padwidth - linelen;

			for (yin = 0; yin < cmd->height; yin++) {
				for (xin = 0, xout = 0; xout < linelen; xin += 4, xout += 3) {
					cBuf[yin*padwidth + xout + 0] = cBuf[yin*padwidth + xin + 0];
					cBuf[yin*padwidth + xout + 1] = cBuf[yin*padwidth + xin + 1];
					cBuf[yin*padwidth + xout + 2] = cBuf[yin*padwidth + xin + 2];
				}
			}
		}

		memcount = RE_SaveJPGToBuffer(cmd->encodeBuffer, avilinelen * cmd->height,
			r_aviMotionJpegQuality->integer,
			cmd->width, cmd->height, cBuf, padlen);
		ri.CL_WriteAVIVideoFrame(cmd->encodeBuffer, memcount);
	}
	else
	{
		byte *lineend, *memend;
		byte *srcptr, *destptr;
	
		srcptr = cBuf;
		destptr = cmd->encodeBuffer;
		memend = srcptr + memcount;
		
		// swap R and B and remove line paddings
		while(srcptr < memend)
		{
			lineend = srcptr + linelen;
			while(srcptr < lineend)
			{
				*destptr++ = srcptr[2];
				*destptr++ = srcptr[1];
				*destptr++ = srcptr[0];
				srcptr += bytesPerPixel;
			}
			
			Com_Memset(destptr, '\0', avipadlen);
			destptr += avipadlen;
			
			srcptr += padlen;
		}
		
		ri.CL_WriteAVIVideoFrame(cmd->encodeBuffer, avipadwidth * cmd->height);
	}

	return (const void *)(cmd + 1);	
}

//============================================================================

/*
** GL_SetDefaultState
*/
void GL_SetDefaultState( void )
{
	qglClearDepth( 1.0f );

	qglCullFace(GL_FRONT);

	GL_BindNullTextures();

	if (glRefConfig.framebufferObject)
		GL_BindNullFramebuffers();

	GL_TextureMode( r_textureMode->string );

	//qglShadeModel( GL_SMOOTH );
	qglDepthFunc( GL_LEQUAL );

	//
	// make sure our GL state vector is set correctly
	//
	glState.glStateBits = GLS_DEPTHTEST_DISABLE | GLS_DEPTHMASK_TRUE;
	glState.storedGlState = 0;
	glState.faceCulling = CT_TWO_SIDED;
	glState.faceCullFront = qtrue;

	GL_BindNullProgram();

	if (glRefConfig.vertexArrayObject)
		qglBindVertexArray(0);

	qglBindBuffer(GL_ARRAY_BUFFER, 0);
	qglBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
	glState.currentVao = NULL;
	glState.vertexAttribsEnabled = 0;

	qglPolygonMode (GL_FRONT_AND_BACK, GL_FILL);
	qglDepthMask( GL_TRUE );
	qglDisable( GL_DEPTH_TEST );
	qglEnable( GL_SCISSOR_TEST );
	qglDisable( GL_CULL_FACE );
	qglDisable( GL_BLEND );

	// HZM gl2 MSAA (plan P2a, ME-F10): alpha-to-coverage / alpha-to-one are raw GL state that survives R_Init on a
	// map load (the context is kept), so force both off here; GL_State's GLS_ALPHA_TO_COVERAGE bit starts clear.
	qglDisable( GL_SAMPLE_ALPHA_TO_COVERAGE );
	if ( !qglesMajorVersion )
		qglDisable( GL_SAMPLE_ALPHA_TO_ONE );
	// HZM gl2 MSAA P4a: the NV driver dithers alpha-to-coverage by default (a crawling screen-door on foliage); off
	if ( qglAlphaToCoverageDitherControlNV )
		qglAlphaToCoverageDitherControlNV( 0x934F );   // GL_ALPHA_TO_COVERAGE_DITHER_DISABLE_NV

	if (glRefConfig.seamlessCubeMap)
		qglEnable(GL_TEXTURE_CUBE_MAP_SEAMLESS);

	// GL_POLYGON_OFFSET_FILL will be glEnable()d when this is used
	qglPolygonOffset( r_offsetFactor->value, r_offsetUnits->value );

	qglClearColor( 0.0f, 0.0f, 0.0f, 1.0f );	// FIXME: get color of sky
}

/*
================
R_PrintLongString

Workaround for ri.Printf's 1024 characters buffer limit.
================
*/
void R_PrintLongString(const char *string) {
	char buffer[1024];
	const char *p;
	int size = strlen(string);

	p = string;
	while(size > 0)
	{
		Q_strncpyz(buffer, p, sizeof (buffer) );
		ri.Printf( PRINT_ALL, "%s", buffer );
		p += 1023;
		size -= 1023;
	}
}

/*
================
GfxInfo_f
================
*/
void GfxInfo_f( void ) 
{
	const char *enablestrings[] =
	{
		"disabled",
		"enabled"
	};
	const char *fsstrings[] =
	{
		"windowed",
		"fullscreen"
	};

	ri.Printf( PRINT_ALL, "\nGL_VENDOR: %s\n", glConfig.vendor_string );
	ri.Printf( PRINT_ALL, "GL_RENDERER: %s\n", glConfig.renderer_string );
	ri.Printf( PRINT_ALL, "GL_VERSION: %s\n", glConfig.version_string );
	ri.Printf( PRINT_ALL, "GL_EXTENSIONS: " );
	// glConfig.extensions_string is a limited length so get the full list directly
	if ( qglGetStringi )
	{
		GLint numExtensions;
		int i;

		qglGetIntegerv( GL_NUM_EXTENSIONS, &numExtensions );
		for ( i = 0; i < numExtensions; i++ )
		{
			ri.Printf( PRINT_ALL, "%s ", qglGetStringi( GL_EXTENSIONS, i ) );
		}
	}
	else
	{
		R_PrintLongString( (char *) qglGetString( GL_EXTENSIONS ) );
	}
	ri.Printf( PRINT_ALL, "\n" );
	ri.Printf( PRINT_ALL, "GL_MAX_TEXTURE_SIZE: %d\n", glConfig.maxTextureSize );
	ri.Printf( PRINT_ALL, "GL_MAX_TEXTURE_IMAGE_UNITS: %d\n", glConfig.numTextureUnits );
	ri.Printf( PRINT_ALL, "\nPIXELFORMAT: color(%d-bits) Z(%d-bit) stencil(%d-bits)\n", glConfig.colorBits, glConfig.depthBits, glConfig.stencilBits );
	ri.Printf( PRINT_ALL, "MODE: %d, %d x %d %s hz:", r_mode->integer, glConfig.vidWidth, glConfig.vidHeight, fsstrings[r_fullscreen->integer == 1] );
	if ( glConfig.displayFrequency )
	{
		ri.Printf( PRINT_ALL, "%d\n", glConfig.displayFrequency );
	}
	else
	{
		ri.Printf( PRINT_ALL, "N/A\n" );
	}
	if ( glConfig.deviceSupportsGamma )
	{
		ri.Printf( PRINT_ALL, "GAMMA: hardware w/ %d overbright bits\n", tr.overbrightBits );
	}
	else
	{
		ri.Printf( PRINT_ALL, "GAMMA: software w/ %d overbright bits\n", tr.overbrightBits );
	}

	ri.Printf( PRINT_ALL, "texturemode: %s\n", r_textureMode->string );
	ri.Printf( PRINT_ALL, "picmip: %d\n", r_picmip->integer );
	ri.Printf( PRINT_ALL, "texture bits: %d\n", r_texturebits->integer );
	ri.Printf( PRINT_ALL, "compiled vertex arrays: %s\n", enablestrings[qglLockArraysEXT != 0 ] );
	ri.Printf( PRINT_ALL, "texenv add: %s\n", enablestrings[glConfig.textureEnvAddAvailable != 0] );
	ri.Printf( PRINT_ALL, "compressed textures: %s\n", enablestrings[glConfig.textureCompression!=TC_NONE] );
	if ( r_vertexLight->integer || glConfig.hardwareType == GLHW_PERMEDIA2 )
	{
		ri.Printf( PRINT_ALL, "HACK: using vertex lightmap approximation\n" );
	}
	if ( glConfig.hardwareType == GLHW_RAGEPRO )
	{
		ri.Printf( PRINT_ALL, "HACK: ragePro approximations\n" );
	}
	if ( glConfig.hardwareType == GLHW_RIVA128 )
	{
		ri.Printf( PRINT_ALL, "HACK: riva128 approximations\n" );
	}
	if ( r_finish->integer ) {
		ri.Printf( PRINT_ALL, "Forcing glFinish\n" );
	}
}

/*
================
GfxMemInfo_f
================
*/
void GfxMemInfo_f( void ) 
{
	switch (glRefConfig.memInfo)
	{
		case MI_NONE:
		{
			ri.Printf(PRINT_ALL, "No extension found for GPU memory info.\n");
		}
		break;
		case MI_NVX:
		{
			int value;

			qglGetIntegerv(GL_GPU_MEMORY_INFO_DEDICATED_VIDMEM_NVX, &value);
			ri.Printf(PRINT_ALL, "GPU_MEMORY_INFO_DEDICATED_VIDMEM_NVX: %ikb\n", value);

			qglGetIntegerv(GL_GPU_MEMORY_INFO_TOTAL_AVAILABLE_MEMORY_NVX, &value);
			ri.Printf(PRINT_ALL, "GPU_MEMORY_INFO_TOTAL_AVAILABLE_MEMORY_NVX: %ikb\n", value);

			qglGetIntegerv(GL_GPU_MEMORY_INFO_CURRENT_AVAILABLE_VIDMEM_NVX, &value);
			ri.Printf(PRINT_ALL, "GPU_MEMORY_INFO_CURRENT_AVAILABLE_VIDMEM_NVX: %ikb\n", value);

			qglGetIntegerv(GL_GPU_MEMORY_INFO_EVICTION_COUNT_NVX, &value);
			ri.Printf(PRINT_ALL, "GPU_MEMORY_INFO_EVICTION_COUNT_NVX: %i\n", value);

			qglGetIntegerv(GL_GPU_MEMORY_INFO_EVICTED_MEMORY_NVX, &value);
			ri.Printf(PRINT_ALL, "GPU_MEMORY_INFO_EVICTED_MEMORY_NVX: %ikb\n", value);
		}
		break;
		case MI_ATI:
		{
			// GL_ATI_meminfo
			int value[4];

			qglGetIntegerv(GL_VBO_FREE_MEMORY_ATI, &value[0]);
			ri.Printf(PRINT_ALL, "VBO_FREE_MEMORY_ATI: %ikb total %ikb largest aux: %ikb total %ikb largest\n", value[0], value[1], value[2], value[3]);

			qglGetIntegerv(GL_TEXTURE_FREE_MEMORY_ATI, &value[0]);
			ri.Printf(PRINT_ALL, "TEXTURE_FREE_MEMORY_ATI: %ikb total %ikb largest aux: %ikb total %ikb largest\n", value[0], value[1], value[2], value[3]);

			qglGetIntegerv(GL_RENDERBUFFER_FREE_MEMORY_ATI, &value[0]);
			ri.Printf(PRINT_ALL, "RENDERBUFFER_FREE_MEMORY_ATI: %ikb total %ikb largest aux: %ikb total %ikb largest\n", value[0], value[1], value[2], value[3]);
		}
		break;
	}
}

/*
===============
R_Register
===============
*/
void R_Register( void ) 
{
	#ifdef USE_RENDERER_DLOPEN
	com_altivec = ri.Cvar_Get("com_altivec", "1", CVAR_ARCHIVE);
	#endif	

	//
	// latched and archived variables
	//
	r_allowExtensions = ri.Cvar_Get( "r_allowExtensions", "1", CVAR_ARCHIVE | CVAR_LATCH );
	r_ext_compressed_textures = ri.Cvar_Get( "r_ext_compressed_textures", "0", CVAR_ARCHIVE | CVAR_LATCH );
	r_ext_multitexture = ri.Cvar_Get( "r_ext_multitexture", "1", CVAR_ARCHIVE | CVAR_LATCH );
	r_ext_compiled_vertex_array = ri.Cvar_Get( "r_ext_compiled_vertex_array", "1", CVAR_ARCHIVE | CVAR_LATCH);
	r_ext_texture_env_add = ri.Cvar_Get( "r_ext_texture_env_add", "1", CVAR_ARCHIVE | CVAR_LATCH);

	r_ext_framebuffer_object = ri.Cvar_Get( "r_ext_framebuffer_object", "1", CVAR_ARCHIVE | CVAR_LATCH);
	r_ext_texture_float = ri.Cvar_Get( "r_ext_texture_float", "1", CVAR_ARCHIVE | CVAR_LATCH);
	r_ext_framebuffer_multisample = ri.Cvar_Get( "r_ext_framebuffer_multisample", "0", CVAR_ARCHIVE | CVAR_LATCH);

	// HZM render-scale supersampling + AMD FSR 1 (r_renderScale). r_renderScale resizes the scene
	// FBOs, so it is CVAR_ARCHIVE|CVAR_LATCH and applies at LAUNCH ONLY - a menu vid_restart while
	// a session is live crashes this renderer (bug-1181), so the Video menu option carries a
	// "(needs restart)" label, like AO. The other three allocate nothing and are live-editable.
	r_renderScale = ri.Cvar_Get( "r_renderScale", "1.0", CVAR_ARCHIVE | CVAR_LATCH );
	ri.Cvar_CheckRange( r_renderScale, 0.5f, 2.0f, qfalse );
	r_upscaleFilter = ri.Cvar_Get( "r_upscaleFilter", "1", CVAR_ARCHIVE );   // 0 bilinear, 1 FSR EASU (<1) / SSAA tent (>1)
	ri.Cvar_CheckRange( r_upscaleFilter, 0, 1, qtrue );
	r_fsrSharpness = ri.Cvar_Get( "r_fsrSharpness", "0.25", CVAR_ARCHIVE );  // 0..1 RCAS strength; 0 = off, replaces r_ppSharpen while > 0
	ri.Cvar_CheckRange( r_fsrSharpness, 0.0f, 1.0f, qfalse );
	r_renderScaleDebug = ri.Cvar_Get( "r_renderScaleDebug", "0", CVAR_TEMP );

	// HZM gl2 soft particles (r_softParticles, design section 5; user decision D8: ON by default,
	// distance 24, restart applies). r_softParticles is CVAR_LATCH because it widens the
	// hdrDepthImage allocation gate (tr_image.c) - a live vid_restart crashes gl2 (bug-1181), so
	// the Video menu row carries a "(needs restart)" label like AO. The distance is live.
	r_softParticles = ri.Cvar_Get( "r_softParticles", "1", CVAR_ARCHIVE | CVAR_LATCH );
	r_softParticleDistance = ri.Cvar_Get( "r_softParticleDistance", "24", CVAR_ARCHIVE );
	ri.Cvar_CheckRange( r_softParticleDistance, 1.0f, 256.0f, qfalse );
	r_softParticlesDebug = ri.Cvar_Get( "r_softParticlesDebug", "0", CVAR_TEMP );
	r_arb_seamless_cube_map = ri.Cvar_Get( "r_arb_seamless_cube_map", "0", CVAR_ARCHIVE | CVAR_LATCH);
	r_arb_vertex_array_object = ri.Cvar_Get( "r_arb_vertex_array_object", "1", CVAR_ARCHIVE | CVAR_LATCH);
	r_ext_direct_state_access = ri.Cvar_Get("r_ext_direct_state_access", "1", CVAR_ARCHIVE | CVAR_LATCH);

	r_ext_texture_filter_anisotropic = ri.Cvar_Get( "r_ext_texture_filter_anisotropic",
			"0", CVAR_ARCHIVE | CVAR_LATCH );
	r_ext_max_anisotropy = ri.Cvar_Get( "r_ext_max_anisotropy", "2", CVAR_ARCHIVE | CVAR_LATCH );

	r_picmip = ri.Cvar_Get ("r_picmip", "1", CVAR_ARCHIVE | CVAR_LATCH );
	r_roundImagesDown = ri.Cvar_Get ("r_roundImagesDown", "1", CVAR_ARCHIVE | CVAR_LATCH );
	r_colorMipLevels = ri.Cvar_Get ("r_colorMipLevels", "0", CVAR_LATCH );
	ri.Cvar_CheckRange( r_picmip, 0, 16, qtrue );
	r_detailTextures = ri.Cvar_Get( "r_detailtextures", "1", CVAR_ARCHIVE | CVAR_LATCH );
	r_texturebits = ri.Cvar_Get( "r_texturebits", "0", CVAR_ARCHIVE | CVAR_LATCH );
	r_colorbits = ri.Cvar_Get( "r_colorbits", "0", CVAR_ARCHIVE | CVAR_LATCH );
	r_stencilbits = ri.Cvar_Get( "r_stencilbits", "8", CVAR_ARCHIVE | CVAR_LATCH );
	r_depthbits = ri.Cvar_Get( "r_depthbits", "0", CVAR_ARCHIVE | CVAR_LATCH );
	r_ext_multisample = ri.Cvar_Get( "r_ext_multisample", "0", CVAR_ARCHIVE | CVAR_LATCH );
	ri.Cvar_CheckRange( r_ext_multisample, 0, 4, qtrue );
	r_overBrightBits = ri.Cvar_Get ("r_overBrightBits", "1", CVAR_ARCHIVE | CVAR_LATCH );
	r_ignorehwgamma = ri.Cvar_Get( "r_ignorehwgamma", "0", CVAR_ARCHIVE | CVAR_LATCH);
	r_mode = ri.Cvar_Get( "r_mode", "-2", CVAR_ARCHIVE | CVAR_LATCH );
	r_fullscreen = ri.Cvar_Get( "r_fullscreen", "1", CVAR_ARCHIVE );
	r_noborder = ri.Cvar_Get("r_noborder", "0", CVAR_ARCHIVE | CVAR_LATCH);
	// HZM [bug-1795] r_desktopfullscreen (0 = Exclusive Fullscreen, 1 = Borderless Window) was
	// registered by renderergl1 ONLY, and gl2 is the renderer that actually ships - so under the live
	// build it survived merely as an archived user cvar with no CVAR_LATCH. sdl_glimp.c still read it
	// by name, which is why borderless worked at all, but the Video Options pulldown's hidden watcher
	// keys off the latch to fire its automatic vid_restart, so changing display mode from the menu
	// could silently not apply until a manual one. Registered here to match gl1 exactly.
	ri.Cvar_Get("r_desktopfullscreen", "0", CVAR_ARCHIVE | CVAR_LATCH);
	r_customwidth = ri.Cvar_Get( "r_customwidth", "1600", CVAR_ARCHIVE | CVAR_LATCH );
	r_customheight = ri.Cvar_Get( "r_customheight", "1024", CVAR_ARCHIVE | CVAR_LATCH );
	r_customPixelAspect = ri.Cvar_Get( "r_customPixelAspect", "1", CVAR_ARCHIVE | CVAR_LATCH );
	r_simpleMipMaps = ri.Cvar_Get( "r_simpleMipMaps", "1", CVAR_ARCHIVE | CVAR_LATCH );
	r_vertexLight = ri.Cvar_Get( "r_vertexLight", "0", CVAR_ARCHIVE | CVAR_LATCH );
	r_uiFullScreen = ri.Cvar_Get( "r_uifullscreen", "0", 0);
	r_subdivisions = ri.Cvar_Get ("r_subdivisions", "4", CVAR_ARCHIVE | CVAR_LATCH);
	r_stereoEnabled = ri.Cvar_Get( "r_stereoEnabled", "0", CVAR_ARCHIVE | CVAR_LATCH);
	r_greyscale = ri.Cvar_Get("r_greyscale", "0", CVAR_ARCHIVE | CVAR_LATCH);
	ri.Cvar_CheckRange(r_greyscale, 0, 1, qfalse);

	r_externalGLSL = ri.Cvar_Get( "r_externalGLSL", "0", CVAR_LATCH );

	r_hdr = ri.Cvar_Get( "r_hdr", "1", CVAR_ARCHIVE | CVAR_LATCH );
	r_floatLightmap = ri.Cvar_Get( "r_floatLightmap", "0", CVAR_ARCHIVE | CVAR_LATCH );
	r_postProcess = ri.Cvar_Get( "r_postProcess", "1", CVAR_ARCHIVE );

	r_toneMap = ri.Cvar_Get( "r_toneMap", "1", CVAR_ARCHIVE );
	r_forceToneMap = ri.Cvar_Get( "r_forceToneMap", "0", CVAR_CHEAT );
	r_forceToneMapMin = ri.Cvar_Get( "r_forceToneMapMin", "-8.0", CVAR_CHEAT );
	r_forceToneMapAvg = ri.Cvar_Get( "r_forceToneMapAvg", "-2.0", CVAR_CHEAT );
	r_forceToneMapMax = ri.Cvar_Get( "r_forceToneMapMax", "0.0", CVAR_CHEAT );

	r_autoExposure = ri.Cvar_Get( "r_autoExposure", "1", CVAR_ARCHIVE );
	r_forceAutoExposure = ri.Cvar_Get( "r_forceAutoExposure", "0", CVAR_CHEAT );
	r_forceAutoExposureMin = ri.Cvar_Get( "r_forceAutoExposureMin", "-2.0", CVAR_CHEAT );
	r_forceAutoExposureMax = ri.Cvar_Get( "r_forceAutoExposureMax", "2.0", CVAR_CHEAT );

	r_cameraExposure = ri.Cvar_Get( "r_cameraExposure", "1", CVAR_CHEAT );

	r_depthPrepass = ri.Cvar_Get( "r_depthPrepass", "1", CVAR_ARCHIVE );
	r_ssao = ri.Cvar_Get( "r_ssao", "0", CVAR_LATCH | CVAR_ARCHIVE );

	r_normalMapping = ri.Cvar_Get( "r_normalMapping", "1", CVAR_ARCHIVE | CVAR_LATCH );
	// HZM gl2 (bug-1155): DEFAULT OFF for MOHAA. rend2 auto-probes for a "<name>_s" texture on any
	// lit stage that has no explicit specularMap (tr_shader.c). MOHAA ships no specular maps, but it
	// DOES ship 39 files whose names end in _s that are ordinary diffuse textures - 34 of them with a
	// matching base name - so the probe binds a diffuse texture as a specular map and lights the
	// surface with it. That is an active (mild) wrongness in the gl2 image today, with no upside on
	// an asset set that has no real specular data. Set 1 only if genuine _s maps are ever authored.
	r_specularMapping = ri.Cvar_Get( "r_specularMapping", "0", CVAR_ARCHIVE | CVAR_LATCH );

	// HZM gl2 (bug-1157): same name and default as renderergl1 so ONE lever drives both renderers.
	// Registered here in R_Register because R_InitImages - which allocates tr.hdrDepthImage, the
	// depth copy the DoF pass samples - runs later in R_Init and has to read it.
	r_ppDoF = ri.Cvar_Get( "r_ppDoF", "0", CVAR_ARCHIVE | CVAR_LATCH );

	// HZM gl2 (bug-1177): same names/defaults as renderergl1 tr_init.c so ONE lever set drives both
	// renderers and the shipped coop_postfx.urc AO controls work on gl2 too (they were silent no-ops
	// here - gl2 only had stock rend2's bare on/off r_ssao, with radius/bias hardcoded in the GLSL).
	// r_ppSSAO is LATCHED on gl2 (gl1 has it live) because it gates buffer allocation in R_InitImages.
	// The latch is what makes this crash-proof: a latched cvar's ->integer returns the ACTIVE value,
	// i.e. exactly what R_InitImages read, so the render-pass gate and the allocation cannot desync.
	// The four TUNING controls stay live (no restart) - they only feed shader uniforms / a program index.
	// DEFAULT 0, NOT gl1's 1 (bug-1178): defaulting this on made gl2 allocate tr.hdrDepthImage
	// (full-res GL_R32F - ~20MB at 3440x1440) + tr.screenSsaoImage AND run an SSAO pass that had
	// never once executed on gl2 in this fork, on every existing install, with no opt-in. That
	// combination crashed on the first map load after deploy. gl2's own stock r_ssao also defaults
	// 0; matching it keeps a fresh install byte-identical to previous behaviour and makes the whole
	// new path strictly opt-in until it has been proven on real hardware.
	r_ppSSAO           = ri.Cvar_Get( "r_ppSSAO",           "0",   CVAR_ARCHIVE | CVAR_LATCH );
	r_ppSSAORadius     = ri.Cvar_Get( "r_ppSSAORadius",     "16",  CVAR_ARCHIVE );
	r_ppSSAOIntensity  = ri.Cvar_Get( "r_ppSSAOIntensity",  "1.0", CVAR_ARCHIVE );
	r_ppSSAOBias       = ri.Cvar_Get( "r_ppSSAOBias",       "0.5", CVAR_ARCHIVE );
	r_ppSSAODepthAware = ri.Cvar_Get( "r_ppSSAODepthAware", "1",   CVAR_ARCHIVE );

	r_deluxeMapping = ri.Cvar_Get( "r_deluxeMapping", "1", CVAR_ARCHIVE | CVAR_LATCH );
	r_parallaxMapping = ri.Cvar_Get( "r_parallaxMapping", "0", CVAR_ARCHIVE | CVAR_LATCH );
	r_parallaxMapOffset = ri.Cvar_Get( "r_parallaxMapOffset", "0", CVAR_ARCHIVE | CVAR_LATCH );
	r_parallaxMapShadows = ri.Cvar_Get( "r_parallaxMapShadows", "0", CVAR_ARCHIVE | CVAR_LATCH );
	r_cubeMapping = ri.Cvar_Get( "r_cubeMapping", "0", CVAR_ARCHIVE | CVAR_LATCH );
	r_cubemapSize = ri.Cvar_Get( "r_cubemapSize", "128", CVAR_ARCHIVE | CVAR_LATCH );
	// HZM gl2 parity (bug-1249) alphaGen sCoord/tCoord. ON by default: this is a PARITY RESTORATION
	// - gl1 has always done this and gl2 silently dropped it, so 0 is the broken state, not the safe
	// one. Kept as a cvar purely as a mid-playtest kill switch. Plain ARCHIVE, never LATCH: the
	// permutations are all pre-built at init so nothing allocates, and vid_restart from an open menu
	// is a known crash here (bug-1181).
	r_hzmAlphaGenCoord = ri.Cvar_Get( "r_hzmAlphaGenCoord", "1", CVAR_ARCHIVE );
	r_hzmFlapDeform    = ri.Cvar_Get( "r_hzmFlapDeform",    "1", CVAR_ARCHIVE );
	// HZM gl2 [2026-09-25] RETAIL LIGHT-GLOW PARITY (vehicle headlights, Phase R). Flags 0 on purpose: never saved,
	// so a later HZM_*_AUTO flip reaches every player (TRAPS T7); not CHEAT (a listen server clamps cheat cvars,
	// bug-1156); not LATCH (read per draw, and vid_restart is a gl2 crash, bug-1181). "-1" = auto (tr_local.h).
	// Test binds: coop_mod/cfg/hltest.cfg (M = OLD/NEW). Omaha BSPs are always off (hzm_light_restore.h).
	r_hzmLightGlow          = ri.Cvar_Get( "r_hzmLightGlow",          "-1", 0 );
	r_hzmLightGlowNear      = ri.Cvar_Get( "r_hzmLightGlowNear",      "-1", 0 );
	r_hzmLightGlowNearRange = ri.Cvar_Get( "r_hzmLightGlowNearRange", "2",  0 );
	r_hzmRgbGenDot          = ri.Cvar_Get( "r_hzmRgbGenDot",          "-1", 0 );
	// HZM gl2 [2026-09-26] PHASE S spot cones + lamp flares: r_hzmSpot / r_hzmFlares (flags 0, "-1" = auto) and their
	// tuning, plus the r_hzmSpotProtocol handshake, forced here on every R_Init (tr_hzm_spot_rb.c)
	R_HZM_SpotRegister();
	// HZM gl2 [2026-09-27] fog-owned LOD (r_hzmLodFog*) + fog-faded SSAO (r_hzmFogAO): flags 0, default OFF
	// (tr_hzm_lodfog_rb.c; docs/proposals/fog_lod_pop_2026-09-27)
	R_HZM_LodFogRegister();
	R_HzmLt_Register();   // HZM coop [2026-09-27] realistic lightning (tr_hzm_lightning.c)

	// HZM (bug-1237) automatic probe placement from info_pathnode - see R_PlaceCubemapsAuto.
	// NOT latched on purpose: both are read at map load, so a change applies on the next map
	// with no vid_restart (which is a known crash from an open menu here, bug-1181).
	r_cubemapAuto = ri.Cvar_Get( "r_cubemapAuto", "16", CVAR_ARCHIVE );
	r_cubemapAutoRadius = ri.Cvar_Get( "r_cubemapAutoRadius", "1200", CVAR_ARCHIVE );
	r_deluxeSpecular = ri.Cvar_Get("r_deluxeSpecular", "0.3", CVAR_ARCHIVE | CVAR_LATCH);
	r_pbr = ri.Cvar_Get("r_pbr", "0", CVAR_ARCHIVE | CVAR_LATCH);
	r_baseNormalX = ri.Cvar_Get( "r_baseNormalX", "1.0", CVAR_ARCHIVE | CVAR_LATCH );
	r_baseNormalY = ri.Cvar_Get( "r_baseNormalY", "1.0", CVAR_ARCHIVE | CVAR_LATCH );
	r_baseParallax = ri.Cvar_Get( "r_baseParallax", "0.05", CVAR_ARCHIVE | CVAR_LATCH );
	// HZM coop - default specular OFF. gl1 (the other shipping renderer) is fixed-function with NO specular;
	// gl2's lightall shader otherwise applies a 4% white sun specular to every lit world surface (no spec map
	// -> specular defaults to vec4(1.0)), producing a view-dependent "white sheen" that sweeps across terrain
	// and floors as the camera pans. 0 matches the gl1 look and removes the only view-dependent term.
	r_baseSpecular = ri.Cvar_Get( "r_baseSpecular", "0", CVAR_ARCHIVE | CVAR_LATCH );
	r_baseGloss = ri.Cvar_Get( "r_baseGloss", "0.3", CVAR_ARCHIVE | CVAR_LATCH );
	r_glossType = ri.Cvar_Get("r_glossType", "1", CVAR_ARCHIVE | CVAR_LATCH);
	r_dlightMode = ri.Cvar_Get( "r_dlightMode", "0", CVAR_ARCHIVE | CVAR_LATCH );
	r_pshadowDist = ri.Cvar_Get( "r_pshadowDist", "128", CVAR_ARCHIVE );
	// HZM coop - default lightmap atlas merging OFF. The merged-atlas lightmap coords come out wrong for
	// MOHAA terrain/world surfaces in this fork, rendering them black (streaky). 0 keeps per-surface lightmaps.
	r_mergeLightmaps = ri.Cvar_Get( "r_mergeLightmaps", "0", CVAR_ARCHIVE | CVAR_LATCH );
	r_imageUpsample = ri.Cvar_Get( "r_imageUpsample", "0", CVAR_ARCHIVE | CVAR_LATCH );
	r_imageUpsampleMaxSize = ri.Cvar_Get( "r_imageUpsampleMaxSize", "1024", CVAR_ARCHIVE | CVAR_LATCH );
	r_imageUpsampleType = ri.Cvar_Get( "r_imageUpsampleType", "1", CVAR_ARCHIVE | CVAR_LATCH );
	r_genNormalMaps = ri.Cvar_Get( "r_genNormalMaps", "0", CVAR_ARCHIVE | CVAR_LATCH );

	//
	// HZM gl2 - TARGETED GENERATED NORMAL MAPS + CONFINED SPECULAR
	//
	// Rationale for not simply defaulting r_genNormalMaps to 1 is in the big comment block
	// above R_FindImageFile in tr_image.c. Short version: that switch is a global flip that
	// synthesises a FULL-RESOLUTION normal map for every mipmapped image the shader parser
	// touches (decals, effect sprites, blend-only stages included), and destructively
	// re-brightens each diffuse. On 2002 hand-painted art carrying a 2.8GB ESRGAN HD pack that
	// is noisy, wasteful and in places actively wrong. These cvars are the narrow version.
	//
	// None of these is CVAR_LATCH (bug-1181: vid_restart crashes gl2) or CVAR_CHEAT (bug-1156:
	// a listen server silently clamps cheat cvars). The generation ones are read at image-load
	// time, so they take effect on the NEXT map load - no vid_restart needed, because
	// RE_BeginRegistration already tears down and rebuilds the whole image/shader system on
	// every map change. Strength, specular and the master itself are additionally read per
	// draw batch, so they can be A/B'd live without leaving the map.
	//
	// 0 = off. No IMGFLAG_GENNORMALMAP is set by this path, nothing is generated, no stage is
	//     marked, and every consumer in tr_shade.c reduces to its previous expression.
	// 1 = allow-list. Only paths matching r_hzmGenNormalInclude are eligible. This is what
	//     keeps models/ out: RB_SkelMesh writes no tangents, so a normal map on a skeletal
	//     character would be rotated by a constant garbage basis (see r_charLighting, which
	//     force-zeroes u_NormalScale for exactly that reason).
	// 2 = broad. Everything except the built-in and user exclusions.
	r_hzmGenNormals = ri.Cvar_Get( "r_hzmGenNormals", "0", CVAR_ARCHIVE );

	// Live. Scales the tangent-space XY of the sampled normal (u_NormalScale.xy in
	// lightall_fp), so 0 collapses N back to the interpolated surface normal - i.e. visually
	// identical to no normal map at all, without a reload. Start low: the Sobel produces its
	// STRONGEST relief where the source texture is darkest, which on this art is exactly where
	// the artist painted dirt and contact shadow.
	r_hzmGenNormalStrength = ri.Cvar_Get( "r_hzmGenNormalStrength", "0.6", CVAR_ARCHIVE );

	// Long-edge cap for the GENERATED map (the diffuse is untouched). This is the single most
	// important dial: it is simultaneously the noise filter, the VRAM budget and the load-time
	// budget, because the height field is box-downsampled to this size BEFORE the Sobel runs.
	// At 512 a 2048x2048 HD texture generates in 1/16 the time and costs 1/16 the VRAM, and the
	// 2002 dither / ESRGAN speckle that would otherwise sparkle through the mip chain is
	// averaged away at source. 0 = full resolution (upstream's behaviour).
	r_hzmGenNormalMaxSize = ri.Cvar_Get( "r_hzmGenNormalMaxSize", "512", CVAR_ARCHIVE );

	// Extra 1-2-1 binomial passes over the height field after the downsample, 0-4. Raise if
	// relief still reads as noise rather than surface.
	r_hzmGenNormalBlur = ri.Cvar_Get( "r_hzmGenNormalBlur", "1", CVAR_ARCHIVE );

	// Upstream's destructive diffuse re-brighten (divide luma by the generated normal's z, up
	// to 8x). OFF: on hand-painted art the painted shading IS the art direction, and this blows
	// out grime, mortar and baked contact shadow. Also expensive - it needs a second
	// full-resolution normal generation.
	r_hzmGenNormalBrighten = ri.Cvar_Get( "r_hzmGenNormalBrighten", "0", CVAR_ARCHIVE );

	// Substring lists, space/comma/semicolon separated, matched case-insensitively against the
	// image path. Include is only consulted in mode 1. Exclude always wins and is applied on
	// top of the built-in list (decals, fx, hud, menu, sky, sprites, ui, fonts - see
	// hzm_genNormalExcludeBuiltin).
	r_hzmGenNormalInclude = ri.Cvar_Get( "r_hzmGenNormalInclude", "textures/", CVAR_ARCHIVE );
	r_hzmGenNormalExclude = ri.Cvar_Get( "r_hzmGenNormalExclude", "", CVAR_ARCHIVE );

	r_hzmGenNormalDebug = ri.Cvar_Get( "r_hzmGenNormalDebug", "0", CVAR_ARCHIVE );

	// Specular reflectance at normal incidence (F0) for stages that carry GENERATED relief, and
	// only those. This is deliberately NOT r_baseSpecular: that one is global, latched, and was
	// defaulted to 0 in this fork precisely because it puts a view-dependent white sheen on
	// every lit world surface. Specular is only interesting where there is relief to catch it,
	// so it is confined to the same opt-in set.
	//
	// Note this needs NOTHING from r_specularMapping, and should not be used with it.
	// r_specularMapping's only contribution on an asset set with no authored specular maps is
	// CollapseStagesToLightall's automatic "<diffuse>_s" probe, and MOHAA ships 39 ordinary
	// diffuse textures whose names end in _s (bug-1155) - the probe binds those as specular
	// data. With r_specularMapping 0 lightall_fp takes `specular = vec4(1.0)` and multiplies by
	// u_SpecularScale, which is exactly the uniform these two cvars drive.
	//
	// Live. 0.04 is the standard dielectric F0 (stone, concrete, painted wood); 0.02 is subtle,
	// 0.08 starts to read as wet or waxed. Ignored when r_pbr is 1, where specularScale carries
	// gloss/metalness instead.
	r_hzmSpecular = ri.Cvar_Get( "r_hzmSpecular", "0", CVAR_ARCHIVE );

	// Live. Meaning depends on r_glossType (default 1 = smoothness, so roughness = 1 - gloss).
	// Low = broad dull sheen, high = tight glint.
	r_hzmSpecularGloss = ri.Cvar_Get( "r_hzmSpecularGloss", "0.3", CVAR_ARCHIVE );

	// [user 2026-08-28] "looks kinda like static as you walk around and look at further distance".
	// Stock rend2 parallax has NO distance or mip falloff at all: RayIntersectDisplaceMap runs a fixed
	// 16-step raymarch at full strength however far away the surface is, so at distance adjacent pixels
	// land on wildly different offsets and the ground boils. Fade it out with range instead.
	//
	// Both live, and both deliberately NOT latched: r_baseParallax is CVAR_LATCH, which is why an A/B of
	// it appeared to do nothing. These ride u_NormalScale, which RB_HZMStageMaterial already rewrites per
	// draw - .a is the depth and .z was set to a constant 1.0 and never read by any shader.
	// 0.04 was tuned against a height GUESSED from diffuse luminance, which is shallow and smooth.
	// The CC0 pack ships MEASURED displacement using the full range, so the same number bulges.
	r_hzmParallaxDepth = ri.Cvar_Get( "r_hzmParallaxDepth", "0.015", CVAR_ARCHIVE );
	r_hzmParallaxFade  = ri.Cvar_Get( "r_hzmParallaxFade",  "800", CVAR_ARCHIVE );

	// Authored relief multiplier. Defaults slightly above 1 because the synthesised maps this replaces
	// were being run at 1.5, so honest normals at 1.0 read as a regression even though they are better data.
	r_hzmNormalStrength = ri.Cvar_Get( "r_hzmNormalStrength", "1.25", CVAR_ARCHIVE );

	// HZM [bug-3007] terrain relief light direction per heightmap vertex (tr_surface.c). flags 0 - never saved, so
	// the HZM_TERRAINLIGHTGRID_AUTO define in tr_local.h is what a player gets until they force 0/1 (TRAPS T7). Live.
	r_hzmTerrainLightGrid = ri.Cvar_Get( "r_hzmTerrainLightGrid", "-1", 0 );

	// HZM (bug-1222): these three were CVAR_CHEAT, inherited from upstream rend2. A listen server
	// clamps cheat cvars (bug-1156), so the host could not set them - which left NO lever for
	// "the scene got darker when r_charShadows switched the sun-shadow chain on". They are purely
	// visual tuning knobs in a co-op PvE mod; there is nothing to cheat at. CVAR_ARCHIVE so the
	// value persists like every other look setting.
	r_forceSun = ri.Cvar_Get( "r_forceSun", "0", CVAR_ARCHIVE );
	r_forceSunLightScale = ri.Cvar_Get( "r_forceSunLightScale", "1.0", CVAR_ARCHIVE );
	r_forceSunAmbientScale = ri.Cvar_Get( "r_forceSunAmbientScale", "0.5", CVAR_ARCHIVE );
	r_drawSunRays = ri.Cvar_Get( "r_drawSunRays", "0", CVAR_ARCHIVE | CVAR_LATCH );
	r_sunlightMode = ri.Cvar_Get( "r_sunlightMode", "1", CVAR_ARCHIVE | CVAR_LATCH );

	r_sunShadows = ri.Cvar_Get( "r_sunShadows", "1", CVAR_ARCHIVE | CVAR_LATCH );
	r_shadowFilter = ri.Cvar_Get( "r_shadowFilter", "1", CVAR_ARCHIVE | CVAR_LATCH );
	r_shadowBlur = ri.Cvar_Get("r_shadowBlur", "0", CVAR_ARCHIVE | CVAR_LATCH);
	r_shadowMapSize = ri.Cvar_Get("r_shadowMapSize", "1024", CVAR_ARCHIVE | CVAR_LATCH);
	r_shadowCascadeZNear = ri.Cvar_Get( "r_shadowCascadeZNear", "8", CVAR_ARCHIVE | CVAR_LATCH );
	r_shadowCascadeZFar = ri.Cvar_Get( "r_shadowCascadeZFar", "1024", CVAR_ARCHIVE | CVAR_LATCH );
	r_shadowCascadeZBias = ri.Cvar_Get( "r_shadowCascadeZBias", "0", CVAR_ARCHIVE | CVAR_LATCH );

	// ------------------------------------------------------------------------------------
	// HZM gl2 REAL CHARACTER SHADOWS
	//
	// Upstream rend2's cascaded sun shadow mapping survived the port intact (4 cascade FBOs,
	// sampler2DShadow depth images, R_RenderSunShadowMaps, the shadowmask resolve) and the
	// bug-1154 worldspawn sun bridge already feeds it a real per-map direction. TWO things
	// stopped it producing character shadows:
	//   1) r_depthPrepass is archived to 0 in the live gl2 sandbox, and the shadowmask +
	//      sun colour are both gated on it (tr_scene.c sunCol, VPF_USESUNLIGHT, tr_backend.c),
	//      so 3 cascade passes render every frame and are DISCARDED.
	//   2) bIsCharacter skeletal actors are dropped from every backEnd.depthFill pass
	//      (tr_backend.c) - an exclusion written for the MAIN-VIEW z-prepass
	//      (bug-gl2-invisible-live-char-depthprepass) that also, silently, applies to the
	//      sun cascade views, which have NO other geometry pass.
	//
	// r_charShadows is the master. At 0 (the default) EVERY guarded path below is skipped and
	// the renderer is bit-identical to today. `r_charShadows 0` is the single revert switch.
	// None of these are CVAR_LATCH (vid_restart crashes under gl2) or CVAR_CHEAT (bug-1156:
	// a listen server clamps CVAR_CHEAT to the default, which is why r_forceSun is unusable).
	r_charShadows = ri.Cvar_Get( "r_charShadows", "0", CVAR_ARCHIVE );

	// Highest sun cascade characters are allowed to cast into (0..3). CalcSplit with the
	// shipped ZNear 8 / ZFar 1024 splits at 8-190 / 190-440 / 440-1024, giving roughly
	// 0.41 / 0.89 / 2.2 world units per texel at r_shadowMapSize 1024 - i.e. a 72-unit
	// actor is ~58 / ~27 / ~11 texels tall. Cascade 3 is the WHOLE-MAP map (tens of units
	// per texel: a sub-texel sparkle, not a shadow) AND it is rendered once and CACHED
	// while the sun direction is static, so admitting characters there would bake frozen
	// silhouettes at their map-load positions. Do not default this to 3.
	r_charShadowCascade = ri.Cvar_Get( "r_charShadowCascade", "1", CVAR_ARCHIVE );

	// Caster distance cap from the viewer, in world units. 0 = no cap.
	r_charShadowDist = ri.Cvar_Get( "r_charShadowDist", "512", CVAR_ARCHIVE );

	// LOD percentage substituted for lodpercentage[0] inside RB_SkelMesh in shadow views
	// only. 0 = no override. PLAY-GL2.bat forces r_uselod 0, so this is the ONLY LOD lever.
	r_charShadowLod = ri.Cvar_Get( "r_charShadowLod", "0.25", CVAR_ARCHIVE );

	// Character casters get their own slope-scaled bias: the global 4/4 is tuned for thin
	// world trim (bug-1164 acne) and visibly detaches a 72-unit actor's shadow from his
	// feet. Only consulted for bIsCharacter surfaces in a VPF_DEPTHSHADOW pass.
	r_charShadowBiasFactor = ri.Cvar_Get( "r_charShadowBiasFactor", "2", CVAR_ARCHIVE );
	r_charShadowBiasUnits  = ri.Cvar_Get( "r_charShadowBiasUnits",  "2", CVAR_ARCHIVE );

	// 1 = keep the cgame blob/Phase-A decal drawing even while real shadows are on (A/B).
	r_charShadowBlob = ri.Cvar_Get( "r_charShadowBlob", "0", CVAR_ARCHIVE );

	// Alpha-tested world cutouts (foliage, fences, camo nets) cast shadows. SEPARATE visual
	// risk from the character fix - deliberately its own switch, default off.
	r_shadowCastFoliage = ri.Cvar_Get( "r_shadowCastFoliage", "0", CVAR_ARCHIVE );

	// HZM gl2 LIGHT-SPHERE SLOT ALIASING (bug-gl2-sphereslot-alias).
	//
	// trRefEntity_t::sphereCalculated is cleared once per FRAME (tr_scene.c, at scene add),
	// but the index it validates - trRefEntity_t::lightingSphere - is handed out from
	// backEnd.numSpheresUsed, which is cleared once per DRAW-SURF LIST (tr_backend.c, top of
	// RB_RenderDrawSurfList). gl1 calls RB_RenderDrawSurfList exactly ONCE per frame, so the
	// two scopes coincide and the port from gl1 was correct there. gl2 calls it up to FIVE
	// times per frame (3 sun cascades + the main-view depth-fill + the main-view colour pass),
	// so slot k is re-issued to a different entity in every later list while the first
	// entity's sphereCalculated flag still says "slot k is mine". The colour pass then shades
	// that entity from a FOREIGN entity's light sphere.
	//
	// Only reachable when a frame contains more than one list, i.e. r_charShadows 1 or
	// r_depthPrepass 1, AND r_fastentlight 0 (with the default 1 a character in a depth-only
	// pass takes the flat grid branch and never allocates a slot at all).
	//
	// 1 = an entity may reuse its cached slot only inside the list that built it, and
	//     recomputes on entering a new list. That is what the cache was always for: the same
	//     entity split across several batches WITHIN one list.
	// 0 = today's behaviour, byte for byte.
	// CVAR_ARCHIVE only - not CVAR_LATCH (vid_restart crashes gl2, bug-1181) and not
	// CVAR_CHEAT (a listen server clamps those, bug-1156), so it is live-togglable.
	// HZM (bug-1230): default flipped 0 -> 1 after the fix was CONFIRMED IN PLAY. This is a bug
	// fix, not a preference - gl2 calls RB_RenderDrawSurfList five times per frame while
	// sphereCalculated is only reset once per frame, so without this a character allocates a
	// sphere slot in a cascade and a later list hands that same slot to another entity, which
	// shades it from a foreign light sphere. Set to 0 to restore the old aliasing behaviour.
	r_sphereCacheScope = ri.Cvar_Get( "r_sphereCacheScope", "1", CVAR_ARCHIVE );

	// 1 = blit the 4 cascade depth maps top-left; 2 = + the screen shadowmask; 3 = + a
	// per-cascade count of character surfaces admitted vs skipped.
	r_shadowDebug = ri.Cvar_Get( "r_shadowDebug", "0", CVAR_ARCHIVE );

	// INDEPENDENT of the shadow work: publish r_coopSunAz/El/Valid the way renderergl1 does.
	// gl2 never had this publisher, so cgame's Phase-A decal runs in MANUAL mode at the
	// hardcoded coop_shadowAz/El 45/45 on every gl2 map.
	// Default 0 = today's behaviour; 1 = the existing blob follows the map's real sun.
	r_coopSunPublish = ri.Cvar_Get( "r_coopSunPublish", "0", CVAR_ARCHIVE );
	// ------------------------------------------------------------------------------------

	// ------------------------------------------------------------------------------------
	// HZM gl2 CHARACTER LIGHTING
	//
	// THE PROBLEM. A bIsCharacter skin's stages carry rgbGen lightingSpherical /
	// lightingGrid. CollapseStagesToGLSL does put them on tr.lightallShader, but with
	// glslShaderIndex & LIGHTDEF_LIGHTTYPE_MASK == 0, and lightall with no light type is
	// literally `gl_FragColor.rgb = diffuse.rgb * var_Color.rgb` - no normal, no light
	// vector, no shadowmap (tr_glsl.c refuses to even build a USE_SHADOWMAP permutation
	// without a light type). All the lighting happens on the CPU in
	// RB_FillModelLightingColors -> sphereor_t::TessFunction, i.e. per VERTEX, and with
	// r_fastentlight 1 (the shipped default) not even that: characters get ONE flat colour
	// sampled from the lightgrid at their feet, identical on every polygon, which is
	// exactly why an AI reads as "lit by full sun" no matter what he is standing in.
	//
	// THE FIX. Characters keep MOHAA's own light solution - the entity light sphere, which
	// already traces to the sun and collects the real map lights in the leaf - but that
	// solution is restated as (flat colour, ambient/directed split, dominant direction) and
	// handed to lightall's USE_LIGHT_VECTOR permutation, so N.L is evaluated per PIXEL from
	// the interpolated surface normal. See charLighting_t in tr_local.h for the algebra.
	// Because the light type is then non-zero, LIGHTDEF_USE_SHADOWMAP finally becomes
	// reachable for characters too - see r_charLightShadow below for why that is NOT free.
	//
	// r_charLighting is the master. At 0 backEnd.charLight.active is never set, so the
	// backend lighting-source selection, RB_FillModelLightingColors and the lightall stage
	// setup all take exactly the paths they take today.
	// None of these are CVAR_LATCH (vid_restart crashes under gl2, bug-1181) or CVAR_CHEAT
	// (a listen server clamps CVAR_CHEAT to the default, bug-1156).
	r_charLighting = ri.Cvar_Get( "r_charLighting", "0", CVAR_ARCHIVE );

	// Wrap term, 0..1. Moves this fraction of the DIRECTED energy into the ambient half, so
	// the side of an actor facing away from the light keeps some of it instead of falling to
	// pure ambient. Total energy at N.L == 1 is unchanged, so this can only lighten, never
	// blow out. 0 = textbook Lambert (hard, sometimes silhouette-black faces on a bright
	// map); 1 = no directionality at all, i.e. today's flat look.
	r_charLightWrap = ri.Cvar_Get( "r_charLightWrap", "0.35", CVAR_ARCHIVE );

	// HZM gl2 [user 2026-09-09, bug-2554] Time constant, in SECONDS, for easing a moving entity's
	// light toward its newly sampled value. Nothing in either renderer damped this before, so
	// walking or driving under a lamp switched the model's light in one frame. 0.10 is roughly two
	// frames at 20 Hz and six at 60: fast enough that it still reads as "I stepped into the light",
	// slow enough that the step is a fade. 0 restores the old behaviour exactly.
	r_entLightSmooth = ri.Cvar_Get( "r_entLightSmooth", "0.10", CVAR_ARCHIVE );

	// HZM gl2 [user 2026-09-09, bug-2556] A static lamp used to be admitted at FULL weight the frame
	// its falloff crossed the hard `>= 5.0` gate, and dropped whole the next - one step, no taper,
	// which is the "flashes bright" the user reported. This is the upper end of a smoothstep over
	// that admission, in the same falloff units. 15 -> full strength at 0.577 x reach, the ramp over
	// the outer 42%, matching gl2's own world dlight attenuation curve. <= 5 restores the hard gate.
	r_entLightFade = ri.Cvar_Get( "r_entLightFade", "15", CVAR_ARCHIVE );

	// Let characters SAMPLE the sun shadowmask. Default 0, and deliberately so: the mask in
	// tr.screenShadowImage is resolved from tr.renderDepthImage, which is filled by the
	// main-view z-prepass - and bIsCharacter surfaces are excluded from that prepass
	// (RB_DepthFillSkip, bug-gl2-invisible-live-char-depthprepass). So at a character's
	// pixels the mask holds the shadow state of whatever geometry is BEHIND him. With
	// r_charShadows 1 that geometry is usually the ground he is standing on, which is inside
	// his OWN cast shadow, so every actor standing in sunlight would darken himself.
	// Correct per-pixel receive needs the cascades sampled directly in lightall from the
	// fragment's own world position (new u_ShadowMap2/3/4 samplers + u_ShadowMvp uniforms +
	// a new permutation bit) - that is the follow-up, not this change. Exposed at 0 so the
	// screen-space approximation can still be A/B'd. Only has any effect at all when the
	// sun-shadow chain is already live (VPF_USESUNLIGHT, i.e. r_charShadows 1 or
	// r_depthPrepass 1).
	r_charLightShadow = ri.Cvar_Get( "r_charLightShadow", "0", CVAR_ARCHIVE );

	// 1 = print the computed per-entity light (direction, ambient/directed split, source
	// light count) about once a second, so the split can be tuned without a debugger.
	r_charLightDebug = ri.Cvar_Get( "r_charLightDebug", "0", CVAR_ARCHIVE );
	// ------------------------------------------------------------------------------------

	// ------------------------------------------------------------------------------------
	// HZM gl2 DYNAMIC-LIGHT CAST SHADOWS (r_hzmDlightShadows).
	//
	// rend2 already contains a complete projected-shadow ("pshadow") chain - the depth
	// targets (tr.pshadowMaps/tr.pshadowFbos, allocated unconditionally in
	// R_CreateBuiltinImages/R_InitFBOs), the world-surface culling (R_PshadowSurface),
	// the receive pass (ProjectPshadowVBOGLSL) and tr.pshadowShader itself. Upstream
	// drives it from R_LightForPoint, i.e. the STATIC lightgrid direction, and dispatches
	// it only on `r_shadows 4` - a value MOHAA's cg_shadows never takes, so in this fork
	// the whole chain has always been dead code.
	//
	// This feature re-aims that chain at the scene's DYNAMIC lights. In a WW2 game those
	// are plentiful and all default-on already: cgame pushes a 55 ms r=160 flash for
	// every bullet fired by anyone (coop_muzzleLight, cg_parsemsg.cpp) and a 260 ms r=420
	// flash for every explosion (coop_explLight), and fgame publishes constantLight on
	// explosion/projectile entities (explosion.cpp, weaputils.cpp) plus anything a map
	// script lights via setLight/lightRadius (entity.cpp) - all of which land in
	// tr.refdef.dlights via CG_EntityEffects / CG_AddCoopDynamicLights.
	//
	// r_hzmDlightShadows is the MASTER. At 0, R_DlightShadowsActive() returns qfalse,
	// R_RenderDlightShadowMaps is never dispatched, tr.refdef.num_pshadows stays 0 (which
	// is what it already is today), so R_PshadowSurface hands out no bits,
	// ProjectPshadowVBOGLSL is never reached and no view ever carries VPF_PSHADOW. The
	// renderer is byte-identical to today.
	// Not CVAR_LATCH (vid_restart crashes gl2, bug-1181), not CVAR_CHEAT (a listen server
	// clamps CVAR_CHEAT back to the default, bug-1156). Live-togglable, no vid_restart.
	r_hzmDlightShadows = ri.Cvar_Get( "r_hzmDlightShadows", "0", CVAR_ARCHIVE );

	// How many dlights may cast in one frame. Each one costs a scan of the entity list and
	// up to r_hzmDlightShadowMax depth views. 2 is deliberately low: at 3440x1440 with 80
	// enemies the scene can hold MAX_DLIGHTS (32) muzzle flashes at once and the cost is
	// linear in this number. Clamped 0..8.
	r_hzmDlightShadowLights = ri.Cvar_Get( "r_hzmDlightShadowLights", "2", CVAR_ARCHIVE );

	// Hard cap on shadow maps rendered per frame, across all lights. Each is one 512x512
	// depth-only view plus one extra full-geometry re-draw of every world surface it
	// touches, so this is the real performance dial. Clamped 0..MAX_DRAWN_PSHADOWS (16).
	r_hzmDlightShadowMax = ri.Cvar_Get( "r_hzmDlightShadowMax", "4", CVAR_ARCHIVE );

	// Ignore lights further than this from the viewer (world units). A muzzle flash across
	// the map is a couple of pixels of shadow nobody will ever see.
	r_hzmDlightShadowDist = ri.Cvar_Get( "r_hzmDlightShadowDist", "1400", CVAR_ARCHIVE );

	// Ignore lights smaller than this radius. Transient cgame lights SHRINK as they fade
	// (radius * f, cg_view.c), so this doubles as a "only while the flash is still bright"
	// gate: at 120 a 160-unit muzzle flash qualifies for roughly its first 25% of life -
	// one or two frames - which reads as a shadow flick rather than a strobe. Raise to
	// ~300 to leave only explosions and standing fires casting.
	r_hzmDlightShadowMinRadius = ri.Cvar_Get( "r_hzmDlightShadowMinRadius", "120", CVAR_ARCHIVE );

	// Max casters considered per light before merging. Clamped 1..8 (pshadow_t holds 8
	// entity slots and the merge step packs neighbours into one map).
	r_hzmDlightShadowCasters = ri.Cvar_Get( "r_hzmDlightShadowCasters", "3", CVAR_ARCHIVE );

	// 1 = let bIsCharacter skeletal actors cast. They are the whole point of the feature,
	// but they are ALSO the entities RB_DepthFillSkip excludes from depth passes
	// (bug-gl2-invisible-live-char-depthprepass), so the exclusion has to be lifted for
	// VPF_PSHADOW views specifically. 0 restores the exclusion and leaves only props
	// (script_models, vehicles, dropped weapons) casting - the diagnostic setting if a
	// character ever renders wrong while this is on.
	r_hzmDlightShadowChars = ri.Cvar_Get( "r_hzmDlightShadowChars", "1", CVAR_ARCHIVE );

	// 1 = print '^~^~^ DLSHADOW ...' about once a second: how many dlights the scene held,
	// how many passed the filters, how many maps were rendered and the winning light's
	// radius/distance. The fastest way to tell "the feature is off" from "no light in this
	// scene qualified".
	r_hzmDlightShadowDebug = ri.Cvar_Get( "r_hzmDlightShadowDebug", "0", CVAR_ARCHIVE );
	// ------------------------------------------------------------------------------------

	r_ignoreDstAlpha = ri.Cvar_Get( "r_ignoreDstAlpha", "1", CVAR_ARCHIVE | CVAR_LATCH );

	//
	// temporary latched variables that can only change over a restart
	//
	r_displayRefresh = ri.Cvar_Get( "r_displayRefresh", "0", CVAR_LATCH );
	ri.Cvar_CheckRange( r_displayRefresh, 0, 200, qtrue );
	r_fullbright = ri.Cvar_Get ("r_fullbright", "0", CVAR_LATCH|CVAR_CHEAT );
	r_mapOverBrightBits = ri.Cvar_Get ("r_mapOverBrightBits", "2", CVAR_LATCH );
	// HZM gl2 [user 2026-09-09, bug-2559] "a smidge too bright... maybe drop it to 1.5 if possible".
	// The bits above are a SHIFT, so 1.5 is not expressible; this trims the resulting scale instead.
	// 1.0 = exactly the bits value; 0.71 with bits 2 gives 2.83x, i.e. 2^1.5. Latched with its parent.
	r_mapOverBrightScale = ri.Cvar_Get ("r_mapOverBrightScale", "1", CVAR_ARCHIVE | CVAR_LATCH );
	r_intensity = ri.Cvar_Get ("r_intensity", "1", CVAR_LATCH );
	r_singleShader = ri.Cvar_Get ("r_singleShader", "0", CVAR_CHEAT | CVAR_LATCH );

	//
	// archived variables that can change at any time
	//
	r_lodCurveError = ri.Cvar_Get( "r_lodCurveError", "250", CVAR_ARCHIVE|CVAR_CHEAT );
	r_lodbias = ri.Cvar_Get( "r_lodbias", "0", CVAR_ARCHIVE );
	r_flares = ri.Cvar_Get ("r_flares", "0", CVAR_ARCHIVE );
	r_znear = ri.Cvar_Get( "r_znear", "4", CVAR_CHEAT );
	ri.Cvar_CheckRange( r_znear, 0.001f, 200, qfalse );
	r_zproj = ri.Cvar_Get( "r_zproj", "64", CVAR_ARCHIVE );
	r_stereoSeparation = ri.Cvar_Get( "r_stereoSeparation", "64", CVAR_ARCHIVE );
	r_ignoreGLErrors = ri.Cvar_Get( "r_ignoreGLErrors", "1", CVAR_ARCHIVE );
	r_fastsky = ri.Cvar_Get( "r_fastsky", "0", CVAR_ARCHIVE );
	r_inGameVideo = ri.Cvar_Get( "r_inGameVideo", "1", CVAR_ARCHIVE );
	r_drawSun = ri.Cvar_Get( "r_drawSun", "0", CVAR_ARCHIVE );
	r_dynamiclight = ri.Cvar_Get( "r_dynamiclight", "1", CVAR_ARCHIVE );
	r_dlightBacks = ri.Cvar_Get( "r_dlightBacks", "1", CVAR_ARCHIVE );
	r_finish = ri.Cvar_Get ("r_finish", "0", CVAR_ARCHIVE);
	r_textureMode = ri.Cvar_Get( "r_textureMode", "GL_LINEAR_MIPMAP_LINEAR", CVAR_ARCHIVE );
#ifdef __EMSCRIPTEN__
	// Under Emscripten we don't throttle framerate with com_maxfps by default, so enable
	// vsync by default instead.
	r_swapInterval = ri.Cvar_Get( "r_swapInterval", "1",
					CVAR_ARCHIVE | CVAR_LATCH );
#else
	r_swapInterval = ri.Cvar_Get( "r_swapInterval", "0",
					CVAR_ARCHIVE | CVAR_LATCH );
#endif
	r_gamma = ri.Cvar_Get( "r_gamma", "1", CVAR_ARCHIVE );
	r_facePlaneCull = ri.Cvar_Get ("r_facePlaneCull", "1", CVAR_ARCHIVE );

	r_railWidth = ri.Cvar_Get( "r_railWidth", "16", CVAR_ARCHIVE );
	r_railCoreWidth = ri.Cvar_Get( "r_railCoreWidth", "6", CVAR_ARCHIVE );
	r_railSegmentLength = ri.Cvar_Get( "r_railSegmentLength", "32", CVAR_ARCHIVE );

	r_ambientScale = ri.Cvar_Get( "r_ambientScale", "0.6", CVAR_CHEAT );
	r_directedScale = ri.Cvar_Get( "r_directedScale", "1", CVAR_CHEAT );

	r_anaglyphMode = ri.Cvar_Get("r_anaglyphMode", "0", CVAR_ARCHIVE);

	//
	// temporary variables that can change at any time
	//
	r_showImages = ri.Cvar_Get( "r_showImages", "0", CVAR_TEMP );

	r_debugLight = ri.Cvar_Get( "r_debuglight", "0", CVAR_TEMP );
	r_debugSort = ri.Cvar_Get( "r_debugSort", "0", CVAR_CHEAT );
	r_printShaders = ri.Cvar_Get( "r_printShaders", "0", 0 );
	r_saveFontData = ri.Cvar_Get( "r_saveFontData", "0", 0 );

	r_nocurves = ri.Cvar_Get ("r_nocurves", "0", CVAR_CHEAT );
	r_drawworld = ri.Cvar_Get ("r_drawworld", "1", CVAR_CHEAT );
	r_lightmap = ri.Cvar_Get ("r_lightmap", "0", 0 );
	r_portalOnly = ri.Cvar_Get ("r_portalOnly", "0", CVAR_CHEAT );

	r_flareSize = ri.Cvar_Get ("r_flareSize", "40", CVAR_CHEAT);
	r_flareFade = ri.Cvar_Get ("r_flareFade", "7", CVAR_CHEAT);
	r_flareCoeff = ri.Cvar_Get ("r_flareCoeff", FLARE_STDCOEFF, CVAR_CHEAT);

	r_skipBackEnd = ri.Cvar_Get ("r_skipBackEnd", "0", CVAR_CHEAT);

	r_measureOverdraw = ri.Cvar_Get( "r_measureOverdraw", "0", CVAR_CHEAT );
	r_lodscale = ri.Cvar_Get( "r_lodscale", "5", CVAR_ARCHIVE );	// HZM gl2 (#77): was CVAR_CHEAT here while also registered CVAR_ARCHIVE below; Cvar_Get OR-combines flags (cvar.c) -> r_lodscale became cheat-protected -> the Advanced "Draw Distance" slider reverted under sv_cheats 0. Register ARCHIVE in both places.
	r_norefresh = ri.Cvar_Get ("r_norefresh", "0", CVAR_CHEAT);
	r_drawentities = ri.Cvar_Get ("r_drawentities", "1", CVAR_CHEAT );
	r_ignore = ri.Cvar_Get( "r_ignore", "1", CVAR_CHEAT );
	r_nocull = ri.Cvar_Get ("r_nocull", "0", CVAR_CHEAT);
	// HZM coop [user 2026-08-16] bug-1849: CVAR_ARCHIVE, not CVAR_CHEAT - see the gl1 copy.
	// r_novis disables PVS CULLING only; solid geometry still draws and still occludes, so it is
	// not an information cheat. Coop players reach places the level was never vis-compiled for
	// (crossing m3l3's churchyard wall makes terrain chunks swap in and out), and with no .map
	// source there is no way to re-vis - bypassing the stale PVS is the only lever.
	// NOTE: this build loads renderer_opengl2.dll, so THIS is the copy that matters.
	// HZM coop [user 2026-08-17] NOT ARCHIVED. CVAR_ARCHIVE let the value persist into the
	// player's omconfig.cfg, so after m3l3 pushed it the game started EVERY session with PVS
	// culling already off - before any world exists - and crashed at startup: two minidumps,
	// both 0xC00000FF in ntdll dispatch with only openmohaa.exe + renderer_opengl2.dll loaded
	// (no game.dll), and the log ending at "finished R_Init". Setting it per session is fine -
	// a whole session was played that way - so it stays settable, it just must not be sticky.
	r_novis = ri.Cvar_Get ("r_novis", "0", 0);
	r_showcluster = ri.Cvar_Get ("r_showcluster", "0", CVAR_CHEAT);
	r_speeds = ri.Cvar_Get ("r_speeds", "0", CVAR_CHEAT);
	r_verbose = ri.Cvar_Get( "r_verbose", "0", CVAR_CHEAT );
	r_logFile = ri.Cvar_Get( "r_logFile", "0", CVAR_CHEAT );
	r_debugSurface = ri.Cvar_Get ("r_debugSurface", "0", CVAR_CHEAT);
	r_nobind = ri.Cvar_Get ("r_nobind", "0", CVAR_CHEAT);
	r_showtris = ri.Cvar_Get ("r_showtris", "0", CVAR_CHEAT);
	r_showsky = ri.Cvar_Get ("r_showsky", "0", CVAR_CHEAT);
	r_shownormals = ri.Cvar_Get ("r_shownormals", "0", CVAR_CHEAT);
	r_clear = ri.Cvar_Get ("r_clear", "0", CVAR_CHEAT);
	r_offsetFactor = ri.Cvar_Get( "r_offsetfactor", "-1", CVAR_CHEAT );
	r_offsetUnits = ri.Cvar_Get( "r_offsetunits", "-2", CVAR_CHEAT );
	// HZM gl2 cascade sun shadows (bug-1156): slope-scaled polygon offset applied only while
	// rendering INTO the shadow depth maps (VPF_DEPTHSHADOW), not CVAR_CHEAT because a listen
	// server clamps CVAR_CHEAT cvars to their default via sv_cheats 0, silently defeating any
	// user tuning. Thin/grazing-angle geometry (e.g. decorative trim spikes) has almost no slope-
	// scale headroom against the fixed receiver-side bias in shadowmask_fp.glsl, so it shows
	// shimmering shadow acne that gl1 (no shadow maps at all) never had.
	r_shadowMapBiasFactor = ri.Cvar_Get( "r_shadowMapBiasFactor", "4", CVAR_ARCHIVE );
	r_shadowMapBiasUnits = ri.Cvar_Get( "r_shadowMapBiasUnits", "4", CVAR_ARCHIVE );
	r_drawBuffer = ri.Cvar_Get( "r_drawBuffer", "GL_BACK", CVAR_CHEAT );
	r_lockpvs = ri.Cvar_Get ("r_lockpvs", "0", CVAR_CHEAT);
	r_noportals = ri.Cvar_Get ("r_noportals", "0", CVAR_CHEAT);
	r_shadows = ri.Cvar_Get( "cg_shadows", "1", 0 );

	r_marksOnTriangleMeshes = ri.Cvar_Get("r_marksOnTriangleMeshes", "0", CVAR_ARCHIVE);

	r_vaoCache = ri.Cvar_Get("r_vaoCache", "0", CVAR_ARCHIVE);

	r_aviMotionJpegQuality = ri.Cvar_Get("r_aviMotionJpegQuality", "90", CVAR_ARCHIVE);
	r_screenshotJpegQuality = ri.Cvar_Get("r_screenshotJpegQuality", "90", CVAR_ARCHIVE);

	r_maxpolys = ri.Cvar_Get( "r_maxpolys", va("%d", MAX_POLYS), 0);
	r_maxpolyverts = ri.Cvar_Get( "r_maxpolyverts", va("%d", MAX_POLYVERTS), 0);

	// make sure all the commands added here are also
	// removed in R_Shutdown
	ri.Cmd_AddCommand( "imagelist", R_ImageList_f );
	ri.Cmd_AddCommand( "shaderlist", R_ShaderList_f );
	ri.Cmd_AddCommand( "skinlist", R_SkinList_f );
	ri.Cmd_AddCommand( "modellist", R_Modellist_f );
	ri.Cmd_AddCommand( "modelist", R_ModeList_f );
	ri.Cmd_AddCommand( "screenshot", R_ScreenShot_f );
	ri.Cmd_AddCommand( "screenshotJPEG", R_ScreenShotJPEG_f );
	ri.Cmd_AddCommand( "gfxinfo", GfxInfo_f );
	ri.Cmd_AddCommand( "minimize", GLimp_Minimize );
	ri.Cmd_AddCommand( "gfxmeminfo", GfxMemInfo_f );
	ri.Cmd_AddCommand( "exportCubemaps", R_ExportCubemaps_f );
	// HZM gl2 graphics probes (tr_gfxprobe.c, MSAA/shadow plan P0): r_gfxProbe, r_glDebug, r_gfxLabel,
	// r_shadowFitYaw/Offset, ROM r_displayHz/r_vsyncActive, and the gfxprobe/gfxwcrc/gfxresettiming commands.
	R_GfxProbe_Register();
	// HZM gl2 stable sun shadows + B0 + E1 (tr_hzm_sunstable.c, plan P3): r_shadowStable and its knobs,
	// r_hzmSunStaticCasters, r_hzmPshadowCharGate. Resets the per-map state (every R_Init).
	R_SunStable_Register();
	// HZM gl2 MSAA core (tr_msaa.c, plan P2a): r_msaaOverride, r_msaaBypass, the dev-only Auto/fallback cvars,
	// ROM r_msaaActive/r_gpuVramMB/r_msaaForcedOff, and the V3-C1 archived-switch check.
	R_Msaa_Register();
	ri.Cmd_AddCommand( "hzmtlprobe", R_HZM_TerrainLProbe_f ); // HZM [bug-3064] diagnostic, inert unless typed

	//
	// OPENMOHAA-specific stuff
	//

	ri.Cvar_CheckRange(r_subdivisions, 2, 24, qtrue);

	// Draw

    r_drawentitypoly = ri.Cvar_Get("r_drawentitypoly", "1", CVAR_CHEAT);
    r_drawstaticmodels = ri.Cvar_Get("r_drawstaticmodels", "1", CVAR_CHEAT);
    r_drawstaticmodelpoly = ri.Cvar_Get("r_drawstaticmodelpoly", "1", CVAR_CHEAT);
    r_drawstaticdecals = ri.Cvar_Get("r_drawstaticdecals", "0", 0);
    r_drawterrain = ri.Cvar_Get("r_drawterrain", "1", CVAR_CHEAT);
    r_drawsprites = ri.Cvar_Get("r_drawsprites", "1", CVAR_CHEAT);
    r_drawspherelights = ri.Cvar_Get("r_drawspherelights", "1", CVAR_CHEAT);
    // HZM gl2 re-port (bug-gl2-modellight): mirrors gl1 tr_init.c:1615 -
    // default 1 routes lightingSpherical models through the flat grid path,
    // exactly like gl1; set 0 for the full per-vertex sphere lighting
    r_fastentlight = ri.Cvar_Get("r_fastentlight", "1", CVAR_ARCHIVE);

    // HZM coop - gore tier 4 (UV wounds) - HZM gl2 re-port (bug-gl2-gore),
    // identical registrations to gl1 tr_init.c:1568-1581
    r_goreUV = ri.Cvar_Get("r_goreUV", "1", CVAR_ARCHIVE);	// HZM coop - gore tier 4 (UV wounds)
    r_goreDebug = ri.Cvar_Get("r_goreDebug", "0", 0);	// HZM coop - gore tier 4 dev counters: 1 = print queue/hit/miss per impact
    // HZM coop - bloodier wounds on EXPOSED SKIN (face/head/hands) only. 1.0 = no change; clamped to 1.0-2.5 at use. Default 1.4 = "a bit" more.
    coop_goreSkinWoundScale = ri.Cvar_Get("coop_goreSkinWoundScale", "1.4", CVAR_ARCHIVE);
    // HZM coop - skin-snap fallback: on a MOVING enemy the exact bullet ray misses the small
    // exposed-skin surfaces (head/hands slid off the server segment vs the client's animated pose);
    // when it does, stamp the nearest skin vertex within coop_goreSkinSnapDist of the bullet stop
    // point as a skin wound.  bug-905: also relocates a wound off a GRAZED cloth surface onto a
    // closer skin vertex.  Only skin surfaces are ever snapped. 1=on, 0=off.
    coop_goreSkinSnap = ri.Cvar_Get("coop_goreSkinSnap", "1", CVAR_ARCHIVE);
    // bug-905: default 18 -> 26 (clamped 8-64 at use). 18u was too tight for living face/head/hand
    // shots - the server stop point (inflated LBD hitbox + interp/anim pose offset) sat beyond it.
    // NOTE: CVAR_ARCHIVE - a config that already stored "18" keeps it; reset or set 26 to pick up.
    coop_goreSkinSnapDist = ri.Cvar_Get("coop_goreSkinSnapDist", "26", CVAR_ARCHIVE);

    r_debuglines_depthmask = ri.Cvar_Get("r_debuglines_depthmask", "0", CVAR_ARCHIVE);
    r_stipplelines = ri.Cvar_Get("r_stipplelines", "1", CVAR_ARCHIVE);
    r_numdebuglines = ri.Cvar_Get("g_numdebuglines", "4096", CVAR_LATCH);

	r_maxtermarks = ri.Cvar_Get("r_maxtermarks", va("%d", MAX_TERMARKS), 0);
	
    r_skyportal = ri.Cvar_Get("r_skyportal", "0", 0);
    r_skyportal_origin = ri.Cvar_Get("r_skyportal_origin", "0 0 0", 0);
	// HZM gl2 re-port Fix 3: ADS view-weapon projection (cgame sets r_weaponfovx/shift each frame)
	r_weaponfovx = ri.Cvar_Get( "r_weaponfovx", "0", 0 );
	r_weaponznear = ri.Cvar_Get( "r_weaponznear", "1", CVAR_ARCHIVE );
	// HZM [2026-08-20] NOT CVAR_ARCHIVE - cgame rewrites these every frame and they now carry an
	// EASED value, so leaving them archived meant a full Com_WriteConfiguration() per frame while
	// aiming. Mirrors renderergl1/tr_init.c. r_weaponfovx uses flags 0 for the same reason.
	r_weaponshifty = ri.Cvar_Get( "r_weaponshifty", "-0.05", 0 );
	r_weaponshiftx = ri.Cvar_Get( "r_weaponshiftx", "0", 0 );
	// [2026-08-21] Cvar_Get ORs flags and NEVER clears them, and any profile that already ran
	// a config containing `seta r_weaponshiftx` has the archive bit set before we get here -
	// so simply passing 0 above does nothing. Strip it explicitly, or Com_WriteConfiguration()
	// rewrites the whole config every frame while aiming (these now carry an EASED value).
	if ( r_weaponshifty ) { r_weaponshifty->flags &= ~CVAR_ARCHIVE; }
	if ( r_weaponshiftx ) { r_weaponshiftx->flags &= ~CVAR_ARCHIVE; }
	r_farplane = ri.Cvar_Get("r_farplane", "0", CVAR_CHEAT);
	r_farplane_bias = ri.Cvar_Get("r_farplane_bias", "0", CVAR_CHEAT);
	r_farplane_color = ri.Cvar_Get("r_farplane_color", ".5 .5 .5", CVAR_CHEAT);
	r_farplane_nocull = ri.Cvar_Get("r_farplane_nocull", "0", CVAR_CHEAT);
	r_farplane_nofog = ri.Cvar_Get("r_farplane_nofog", "0", CVAR_CHEAT);
	r_skybox_farplane = ri.Cvar_Get("r_skybox_farplane", "0", CVAR_CHEAT);
	r_farclip = ri.Cvar_Get("r_farclip", "0", CVAR_CHEAT);

	// HZM gl2 fog parity. Defaults reproduce gl1's fixed-function GL_FOG_LINEAR exactly;
	// the scales exist so the A/B harness can bisect a residual mismatch live.
	r_globalFog              = ri.Cvar_Get("r_globalFog",              "1", CVAR_ARCHIVE);
	r_globalFogScale         = ri.Cvar_Get("r_globalFogScale",         "1", CVAR_ARCHIVE);
	r_globalFogStartScale    = ri.Cvar_Get("r_globalFogStartScale",    "1", CVAR_ARCHIVE);
	r_globalFogEndScale      = ri.Cvar_Get("r_globalFogEndScale",      "1", CVAR_ARCHIVE);
	r_globalFogSky           = ri.Cvar_Get("r_globalFogSky",           "1", CVAR_ARCHIVE);
	// HZM coop [2026-10-05] the sky fog model (tr_shade.c): flags 0, never archived (TRAPS T7)
	r_hzmSkyFog              = ri.Cvar_Get("r_hzmSkyFog",              "-1", 0);
	r_hzmSkyFogHeight        = ri.Cvar_Get("r_hzmSkyFogHeight",        "3000", 0);
	r_hzmSkyFogPortal        = ri.Cvar_Get("r_hzmSkyFogPortal",        "0", 0);
	// HZM gl2 [2026-09-25] HD SKY BOXES + SKY LAYERS (the moving cloud dome's stage images, env/hzmhd/clouds/).
	// Read by ParseSkyParms / R_HZM_SkyHDLayers when the map's shaders are parsed, so a change
	// takes effect on the NEXT map - RE_BeginRegistration rebuilds every shader per map, no vid_restart. Not
	// CVAR_LATCH (bug-1181: vid_restart crashes gl2), not CVAR_CHEAT (bug-1156: a listen server clamps cheat
	// cvars). Archived as "-1" = auto (HZM_SKYHD_AUTO, tr_local.h) so the build default can still be flipped.
	// r_skyHDCompare is test-only and never saved: set before a map loads, it keeps BOTH sets resident for
	// that map and DrawSkyBox flips between them live (1 = HD, 2 = original). coop_mod/cfg/skytest.cfg.
	r_skyHD                  = ri.Cvar_Get("r_skyHD",                  "-1", CVAR_ARCHIVE);
	r_skyHDCompare           = ri.Cvar_Get("r_skyHDCompare",           "0", CVAR_TEMP);
	r_globalFogRadial        = ri.Cvar_Get("r_globalFogRadial",        "0", CVAR_ARCHIVE);
	r_globalFogIdentityLight = ri.Cvar_Get("r_globalFogIdentityLight", "0", CVAR_ARCHIVE);
	// HZM gl2: CVAR_TEMP, not CVAR_CHEAT. A listen server runs with sv_cheats 0, so a
	r_globalFogDebug         = ri.Cvar_Get("r_globalFogDebug",         "0", CVAR_CHEAT);

	// HZM gl2 fog parity (forward port, bug-1306): mix the global fog INSIDE the surface
	// shaders, before the tone stage, which is the order gl1 uses (fixed-function fog during
	// rasterisation, then RB_PostFxApply's grade). gl2's tone stage IS gl1's grade -
	// tonemap_hzm_fp.glsl is a port of renderergl1 TONEMAP_FS, reached unconditionally because
	// r_ppTonemap ships 1 - so TONE(mix(scene, farplane_color)) reproduces gl1 exactly with the
	// RAW farplane colour. It also reaches surfaces the old depth-based screen pass cannot:
	// sky, the sun, and any blendfunc surface that writes no depth (explosion sprites, the
	// glider windscreen). 0 restores the screen-space pass with no rebuild.
	// r_globalFogDebug / r_globalFogRadial force the legacy path (R_UseForwardGlobalFog).
	r_globalFogForward       = ri.Cvar_Get("r_globalFogForward",       "1", CVAR_ARCHIVE);

	// Lighting

    r_lightcoronasize = ri.Cvar_Get("r_lightcoronasize", ".1", CVAR_ARCHIVE);
    r_light_lines = ri.Cvar_Get("r_light_lines", "0", CVAR_CHEAT);
    r_light_sun_line = ri.Cvar_Get("r_light_sun_line", "0", CVAR_CHEAT);
    r_light_int_scale = ri.Cvar_Get("r_light_int_scale", "0.05", CVAR_ARCHIVE);
    r_light_nolight = ri.Cvar_Get("r_light_nolight", "0", CVAR_CHEAT | CVAR_ARCHIVE);
    r_light_showgrid = ri.Cvar_Get("r_light_showgrid", "0", CVAR_CHEAT);
    r_entlight_scale = ri.Cvar_Get("r_entlight_scale", "1.3", CVAR_CHEAT);
    r_entlight_errbound = ri.Cvar_Get("r_entlight_errbound", "6", CVAR_ARCHIVE);
    r_entlight_cubelevel = ri.Cvar_Get("r_entlight_cubelevel", "0", CVAR_ARCHIVE);
    r_entlight_cubefraction = ri.Cvar_Get("r_entlight_cubefraction", "0.5", CVAR_ARCHIVE);
    r_entlight_maxcalc = ri.Cvar_Get("r_entlight_maxcalc", "2", CVAR_ARCHIVE);

	// LOD

	r_staticlod = ri.Cvar_Get("r_staticlod", "1", CVAR_CHEAT);
	r_lodscale = ri.Cvar_Get("r_lodscale", "5", CVAR_ARCHIVE);
	r_lodcap = ri.Cvar_Get("r_lodcap", "0.35", CVAR_ARCHIVE);
	r_lodviewmodelcap = ri.Cvar_Get("r_lodviewmodelcap", "0.25", CVAR_ARCHIVE);
	
	r_uselod = ri.Cvar_Get("r_uselod", "1", CVAR_TEMP);
	lod_LOD = ri.Cvar_Get("lod_LOD", "0", CVAR_TEMP);
	lod_minLOD = ri.Cvar_Get("lod_minLOD", "1.0", CVAR_TEMP);
	lod_maxLOD = ri.Cvar_Get("lod_maxLOD", "0.3", CVAR_TEMP);
	lod_LOD_slider = ri.Cvar_Get("lod_LOD_slider", "0.5", CVAR_TEMP);
	lod_edit_0 = ri.Cvar_Get("lod_edit_0", "0", CVAR_TEMP);
	lod_edit_1 = ri.Cvar_Get("lod_edit_1", "0", CVAR_TEMP);
	lod_edit_2 = ri.Cvar_Get("lod_edit_2", "0", CVAR_TEMP);
	lod_edit_3 = ri.Cvar_Get("lod_edit_3", "0", CVAR_TEMP);
	lod_edit_4 = ri.Cvar_Get("lod_edit_4", "0", CVAR_TEMP);
	lod_curve_0_val = ri.Cvar_Get("lod_curve_0_val", "0", CVAR_TEMP);
	lod_curve_1_val = ri.Cvar_Get("lod_curve_1_val", "0", CVAR_TEMP);
	lod_curve_2_val = ri.Cvar_Get("lod_curve_2_val", "0", CVAR_TEMP);
	lod_curve_3_val = ri.Cvar_Get("lod_curve_3_val", "0", CVAR_TEMP);
	lod_curve_4_val = ri.Cvar_Get("lod_curve_4_val", "0", CVAR_TEMP);
	lod_curve_0_slider = ri.Cvar_Get("lod_curve_0_slider", "0", CVAR_TEMP);
	lod_curve_1_slider = ri.Cvar_Get("lod_curve_1_slider", "0", CVAR_TEMP);
	lod_curve_2_slider = ri.Cvar_Get("lod_curve_2_slider", "0", CVAR_TEMP);
	lod_curve_3_slider = ri.Cvar_Get("lod_curve_3_slider", "0", CVAR_TEMP);
	lod_curve_4_slider = ri.Cvar_Get("lod_curve_4_slider", "0", CVAR_TEMP);
	lod_pitch_val = ri.Cvar_Get("lod_pitch_val", "0", CVAR_TEMP);
	lod_zee_val = ri.Cvar_Get("lod_zee_val", "0", CVAR_TEMP);
	lod_mesh = ri.Cvar_Get("lod_mesh", "0", CVAR_TEMP);
	lod_meshname = ri.Cvar_Get("lod_meshname", "", CVAR_TEMP);
	lod_tikiname = ri.Cvar_Get("lod_tikiname", "", CVAR_TEMP);
	lod_metric = ri.Cvar_Get("lod_metric", "0.0", CVAR_TEMP);
	lod_tris = ri.Cvar_Get("lod_tris", "", CVAR_TEMP);
	lod_save = ri.Cvar_Get("lod_save", "0", CVAR_TEMP);
	lod_position = ri.Cvar_Get("lod_position", "0 0 0", CVAR_TEMP);
	lod_tool = ri.Cvar_Get("lod_tool", "0", CVAR_TEMP);

	// Utils

    r_fps = ri.Cvar_Get("fps", "0", 0);
    r_developer = ri.Cvar_Get("developer", "", 0);
    r_showstaticbboxes = ri.Cvar_Get("r_showstaticbboxes", "0", CVAR_CHEAT);
    r_showcull = ri.Cvar_Get("r_showcull", "0", CVAR_CHEAT);
    r_showlod = ri.Cvar_Get("r_showlod", "0", CVAR_TEMP);
    r_showstaticlod = ri.Cvar_Get("r_showstaticlod", "0", CVAR_TEMP);
    r_showportal = ri.Cvar_Get("r_showportal", "0", 0);
}

void R_InitQueries(void)
{
	// HZM gl2 [2026-09-26] Phase S2 lamp-flare queries: created here UNCONDITIONALLY (never behind a cvar that changes
	// live - vet F17), names in tr (tr_hzm_spot_rb.c)
	R_HZM_FlareInitQueries();

	if (!glRefConfig.occlusionQuery)
		return;

	if (r_drawSunRays->integer)
		qglGenQueries(ARRAY_LEN(tr.sunFlareQuery), tr.sunFlareQuery);
}

void R_ShutDownQueries(void)
{
	// ^~^~^ SKEL* diag scaffold: forget the cached occlusion-query name - the context is
	// going away and reusing it after a resolution-change vid_restart crashed (07-27 17:58).
	{
		extern void RB_SkelProbeShutdown(void);
		RB_SkelProbeShutdown();
	}

	R_HZM_FlareShutdownQueries();	// HZM gl2 [2026-09-26] Phase S2 (self-checks occlusionQuery)

	if (!glRefConfig.occlusionQuery)
		return;

	if (r_drawSunRays->integer)
		qglDeleteQueries(ARRAY_LEN(tr.sunFlareQuery), tr.sunFlareQuery);
}

/*
===============
R_Init
===============
*/
void R_Init( void ) {	
	int	err;
	int i;
	byte *ptr;

	ri.Printf( PRINT_ALL, "----- R_Init -----\n" );

	// clear all our internal state
	Com_Memset( &tr, 0, sizeof( tr ) );
	Com_Memset( &backEnd, 0, sizeof( backEnd ) );
	Com_Memset( &tess, 0, sizeof( tess ) );

	//if(sizeof(glconfig_t) != 11332)
	//	ri.Error( ERR_FATAL, "Mod ABI incompatible: sizeof(glconfig_t) == %u != 11332", (unsigned int) sizeof(glconfig_t));

//	Swap_Init();

	if ( (intptr_t)tess.xyz & 15 ) {
		ri.Printf( PRINT_WARNING, "tess.xyz not 16 byte aligned\n" );
	}
	//Com_Memset( tess.constantColor255, 255, sizeof( tess.constantColor255 ) );

	//
	// init function tables
	//
	for ( i = 0; i < FUNCTABLE_SIZE; i++ )
	{
		tr.sinTable[i]		= sin( DEG2RAD( i * 360.0f / ( ( float ) ( FUNCTABLE_SIZE - 1 ) ) ) );
		tr.squareTable[i]	= ( i < FUNCTABLE_SIZE/2 ) ? 1.0f : -1.0f;
		tr.sawToothTable[i] = (float)i / FUNCTABLE_SIZE;
		tr.inverseSawToothTable[i] = 1.0f - tr.sawToothTable[i];

		if ( i < FUNCTABLE_SIZE / 2 )
		{
			if ( i < FUNCTABLE_SIZE / 4 )
			{
				tr.triangleTable[i] = ( float ) i / ( FUNCTABLE_SIZE / 4 );
			}
			else
			{
				tr.triangleTable[i] = 1.0f - tr.triangleTable[i-FUNCTABLE_SIZE / 4];
			}
		}
		else
		{
			tr.triangleTable[i] = -tr.triangleTable[i-FUNCTABLE_SIZE/2];
		}
	}

	R_InitFogTable();

	R_NoiseInit();

	R_Register();

	max_polys = r_maxpolys->integer;
	if (max_polys < MAX_POLYS)
		max_polys = MAX_POLYS;

	max_polyverts = r_maxpolyverts->integer;
	if (max_polyverts < MAX_POLYVERTS)
		max_polyverts = MAX_POLYVERTS;

	// HZM bug-3089: allocate terMarks like gl1 (renderergl1/tr_init.c:1844-1848).
	// gl2 used to leave backEndData->terMarks NULL (Hunk_Alloc zeroes) while
	// RE_AddTerrainMarkToScene writes through it. Unreachable today only because gl2's
	// R_MarkFragments never returns a terrain fragment (iIndex > 0), so cgame never calls it.
	max_termarks = r_maxtermarks->integer;
	if (max_termarks < MAX_TERMARKS)
		max_termarks = MAX_TERMARKS;

	ptr = ri.Hunk_Alloc( sizeof( *backEndData ) + sizeof(srfPoly_t) * max_polys + sizeof(polyVert_t) * max_polyverts + sizeof(srfMarkFragment_t) * max_termarks, h_low);
	backEndData = (backEndData_t *) ptr;
	backEndData->polys = (srfPoly_t *) ((char *) ptr + sizeof( *backEndData ));
	backEndData->polyVerts = (polyVert_t *) ((char *) ptr + sizeof( *backEndData ) + sizeof(srfPoly_t) * max_polys);
	backEndData->terMarks = (srfMarkFragment_t *) ((char *) ptr + sizeof( *backEndData ) + sizeof(srfPoly_t) * max_polys + sizeof(polyVert_t) * max_polyverts);
	R_InitNextFrame();

	InitOpenGL();

	// HZM gl2 graphics probes: registration bookkeeping, GPU timer queries, the KHR_debug callback when the
	// context was created with r_glDebug 1, and ROM r_vsyncActive.
	R_GfxProbe_InitGL();

	// HZM gl2 MSAA (plan P2a, ME-F7): decide the path and the sample count before any image, FBO or GLSL program
	// exists - the images are sized for it, the FBOs are built for it, and MSAA-only GLSL defines depend on it.
	R_DecideMsaa();

	R_InitImages();

	if (glRefConfig.framebufferObject)
		FBO_Init();

	GLSL_InitGPUShaders();

	// HZM gl2 MSAA (plan P2a, ME-F3): an MSAA program that failed to build re-initialises in place at 0 (no
	// ri.Error, no drop to the menu); then ROM r_msaaActive is published and the ^~^~^ MSAA line printed.
	R_Msaa_AfterGLSL();

	R_InitVaos();

	R_InitShaders();

	R_InitSkins();

	R_ModelInit();

	R_InitFreeType();

	R_InitQueries();

	//
	// OPENMOHAA-specific stuff
    //=========================

    R_Sky_Init();

    // max_termarks is now computed before the backEndData allocation above.

    R_LevelMarksInit();

    tr.pFontDebugStrings = R_LoadFont("verdana-14");
    g_bInfoworldtris = qfalse;

	//=========================

	err = qglGetError();
	if ( err != GL_NO_ERROR )
		ri.Printf (PRINT_ALL, "glGetError() = 0x%x\n", err);

	// print info
	GfxInfo_f();
	// HZM gl2: ^~^~^ GFXBUILD - which renderer build actually loaded (a failed renderer_<name>.dll load
	// silently falls back to opengl1, so every test run checks for this line).
	R_GfxProbe_AfterInit();
	ri.Printf( PRINT_ALL, "----- finished R_Init -----\n" );
}

/*
===============
RE_Shutdown
===============
*/
void RE_Shutdown( qboolean destroyWindow ) {	

	// HZM gl2 [bug-3487] the skinning workers live in this DLL, and it is unloaded on every vid_restart
	R_SkelThreadsShutdown();

	ri.Printf( PRINT_ALL, "RE_Shutdown( %i )\n", destroyWindow );

	// HZM bug-3011/bug-3102: cvars outlive the renderer DLL. Withdraw the gl2 -> cgame handshake
	// values here, so a gl2 -> gl1 vid_restart (or the silent opengl1 fallback) does not
	// leave r_coopRealShadows 1 behind, which made cgame hide gl1's decal shadows. The
	// per-frame publisher in RE_RenderScene sets them again on the next gl2 frame.
	ri.Cvar_Set( "r_coopRealShadows", "0" );
	ri.Cvar_Set( "r_coopSunValid", "0" );

	ri.Cmd_RemoveCommand( "imagelist" );
	ri.Cmd_RemoveCommand( "shaderlist" );
	ri.Cmd_RemoveCommand( "skinlist" );
	ri.Cmd_RemoveCommand( "modellist" );
	ri.Cmd_RemoveCommand( "modelist" );
	ri.Cmd_RemoveCommand( "screenshot" );
	ri.Cmd_RemoveCommand( "screenshotJPEG" );
	ri.Cmd_RemoveCommand( "gfxinfo" );
	ri.Cmd_RemoveCommand( "minimize" );
	ri.Cmd_RemoveCommand( "gfxmeminfo" );
	ri.Cmd_RemoveCommand( "exportCubemaps" );
	R_GfxProbe_Shutdown();
	ri.Cmd_RemoveCommand( "hzmtlprobe" );


	if ( tr.registered ) {
		R_IssuePendingRenderCommands();
		R_ShutDownQueries();
		if (glRefConfig.framebufferObject)
			FBO_Shutdown();
		R_GoreShutdown();		// HZM coop - gore tier 4 (UV wounds): forget instance images before deletion
		R_DeleteTextures();
		R_ShutdownVaos();
		GLSL_ShutdownGPUShaders();
	}

	// HZM gl2 [2026-09-26] Phase S handshake (vet F3): this gl2 is going away (vid_restart, a renderer switch, quit),
	// so cgame must stop sending spots/flares until a Phase S gl2 sets it again in R_Init
	R_HZM_SpotShutdown();

	R_DoneFreeType();

	// shut down platform specific OpenGL stuff
	if ( destroyWindow ) {
		GLimp_Shutdown();

		Com_Memset( &glConfig, 0, sizeof( glConfig ) );
		Com_Memset( &glRefConfig, 0, sizeof( glRefConfig ) );
		textureFilterAnisotropic = qfalse;
		maxAnisotropy = 0;
		displayAspect = 0.0f;
		haveClampToEdge = qfalse;

		Com_Memset( &glState, 0, sizeof( glState ) );
	}

	//
	// OPENMOHAA-specific stuff
	//=========================
    R_ShutdownFont();	// HZM gl2 (#77): gl1 calls this in RE_Shutdown (gl1 tr_init.c:1885); gl2 never did, so the persistent font cache kept stale shader_t*/texnums across vid_restart -> OOB/garbage on the first menu text after restart.
    R_ShutdownTerrain();
    //=========================

	tr.registered = qfalse;
}

//
// OPENMOHAA-specific stuff
//=========================

/*
** RE_BeginRegistration
*/
void RE_BeginRegistration(glconfig_t* glconfigOut) {

    // HZM gl2 (#77 vid_restart crash): gl2 never called ri.Hunk_Clear(), so every runtime
    // vid_restart LEAKED the entire TAG_STATIC_RENDERER zone (world, images, shaders, models,
    // backEndData, FBO structs) - the re-init then exhausted the zone -> Com_Error fatal.
    // This is why "most Advanced Graphics settings crash gl2" (they're latched -> force
    // vid_restart) while gl1 is fine. Mirror gl1's RE_BeginRegistration order
    // (gl1 tr_init.c:1897-1925): free the systems holding zone pointers FIRST, then clear
    // the tag, then rebuild via R_Init. (Previously LevelMarksFree/TerrainFree ran AFTER
    // R_Init, which would leave them freeing the NEW allocations once the clear exists.)
    // HZM gl2 (bug-1146): this block used to sit behind a `static qboolean s_bReregistration`
    // first-boot guard (bug-1128). That guard was WRONG in the one case it mattered most:
    // vid_restart UNLOADS and RELOADS this DLL (cl_main.cpp CL_ShutdownRef -> Sys_UnloadLibrary,
    // CL_InitRef -> Sys_LoadDll), so the static was back to qfalse on every runtime restart and
    // ri.Hunk_Clear() NEVER RAN - the exact TAG_STATIC_RENDERER leak bug-1128 was written to stop
    // was still live on every Advanced-Graphics apply.
    // It is now unconditional, matching gl1's RE_BeginRegistration (renderergl1 tr_init.c), which
    // has never needed a guard. The reason gl2 needed one - R_LevelMarksFree dereferencing an
    // unregistered dcl_editmode, because gl2 registers its cvars inside R_Init BELOW - is fixed at
    // the source (tr_marks_permanent.c now checks the pointer). The other three are safe on a fresh
    // image: R_IssuePendingRenderCommands early-outs on !tr.registered, R_TerrainFree frees NULL,
    // R_GoreLevelReset only touches zeroed statics, and ri.Hunk_Clear (Z_FreeTags) is engine-side,
    // so on a reload it correctly reclaims the PREVIOUS image's allocations that nothing points at.
    R_IssuePendingRenderCommands();

    // HZM gl2 graphics probes: the previous registration ends here - print what it saw (probe on only).
    R_GfxProbe_Reregister();

    // HZM [user 08-02]: GPU-OBJECT TEARDOWN on re-registration. ri.Hunk_Clear() below frees
    // the CPU-side structs, but GL objects are driver-side handles - deleting the image_t
    // does not delete the GL texture. gl2 reaches R_Init() from HERE on every map load and
    // every disconnect, while the only calls to the four GL deletors live in RE_Shutdown,
    // which the disconnect path never reaches: CL_Disconnect_f -> CL_FlushMemory ->
    // CL_StartHunkUsers -> CL_BeginRegistration, and CL_ShutdownAll (the sole caller of
    // re.Shutdown) is not on that path. A single session's qconsole.log showed the result
    // plainly: "----- R_Init -----" x4, GLSL_InitGPUShaders x4, GLSL_ShutdownGPUShaders 0,
    // RE_Shutdown 0. Each cycle orphaned ~150-200 GLSL programs plus every texture, VAO/VBO
    // and FBO. gl1 does not have this problem because it calls R_Init() once from GetRefAPI
    // and its RE_BeginRegistration does not re-init.
    // This must run BEFORE Hunk_Clear (the deletors walk tr.images / tr.vaos, which the
    // clear is about to free) and before R_Init (which does Com_Memset(&tr,0,...) and then
    // re-creates all four via R_InitImages / FBO_Init / GLSL_InitGPUShaders / R_InitVaos),
    // so init and shutdown stay symmetric. Order mirrors RE_Shutdown exactly - in particular
    // R_GoreShutdown must precede R_DeleteTextures so gore instances forget their image_t
    // pointers before those images die.
    // Deliberately NOT calling R_DoneFreeType here: R_Init does re-run R_InitFreeType, but
    // the font cache self-heals off r_sequencenumber (bumped just below) and tearing down
    // FreeType mid-connect risks the load-screen text draw for a comparatively tiny leak.
    if ( tr.registered ) {
        R_ShutDownQueries();
        if ( glRefConfig.framebufferObject ) {
            FBO_Shutdown();
        }
        R_GoreShutdown();
        R_DeleteTextures();
        R_ShutdownVaos();
        GLSL_ShutdownGPUShaders();
    }

    R_LevelMarksFree();
    R_TerrainFree();
    R_GoreLevelReset();		// HZM coop - gore tier 4 (UV wounds): new map = clean uniforms

    ri.Hunk_Clear();

    // HZM gl2 (#77): gl1 bumps this every registration (gl1 tr_init.c:1912) and the font
    // system self-heals off it - R_DrawString*/R_LoadFont re-resolve font->shader when
    // font->trhandle != r_sequencenumber (tr_font.cpp:586/807, code identical in gl2).
    // gl2 declared r_sequencenumber but NEVER incremented it, so the cached font shader
    // pointer was reused forever - a use-after-free crash at the first UI text draw
    // (UI_DrawConnect) once the Hunk_Clear above actually frees the old shaders.
    r_sequencenumber++;

    R_Init();

    *glconfigOut = glConfig;

    R_IssuePendingRenderCommands();

    tr.viewCluster = -1;		// force markleafs to regenerate
    R_ClearFlares();
    RE_ClearScene();

	//
	// OPENMOHAA-specific stuff
    //=========================
    R_InitLensFlare();
    R_LevelMarksInit();

    ri.UI_LoadResource("*124");
	//=========================

    tr.registered = qtrue;
}
//=========================

/*
=============
RE_EndRegistration

Touch all images to make sure they are resident
=============
*/
void RE_EndRegistration( void ) {
	R_IssuePendingRenderCommands();
	if (!ri.Sys_LowPhysicalMemory()) {
		RB_ShowImages();
	}
}

//
// OPENMOHAA-specific stuff
//=========================

/*
==================
R_SetMode
==================
*/
qboolean R_SetMode(int mode, const glconfig_t* glConfig) {
    // FIXME: unimplemented (GL2)
	ri.Printf(PRINT_WARNING, "R_SetMode is unimplemented. To change the video mode, set the \"r_mode\" variable and execute the \"vid_restart\" command.\n");
	return qfalse;
}

/*
==================
R_SetFullscreen
==================
*/
void R_SetFullscreen(qboolean fullscreen) {
    // FIXME: unimplemented (GL2)
    ri.Printf(PRINT_WARNING, "R_SetFullscreen is unimplemented. To change the fullscreen mode, set the \"r_fullscreen\" variable and execute the \"vid_restart\" command.\n");
}

/*
=============
RE_SetRenderTime
=============
*/
void RE_SetRenderTime(int t) {
    backEnd.refdef.floatTime = (long double)t / 1000.0;
    R_UpdateGhostTextures();
}

/*
==================
RE_GetGraphicsInfo
==================
*/
const char* RE_GetGraphicsInfo() {
	// FIXME: unimplemented (GL2)
	// Looks like it's unused anyway
	return "";
}

//=========================

/*
@@@@@@@@@@@@@@@@@@@@@
GetRefAPI

@@@@@@@@@@@@@@@@@@@@@
*/
#ifdef USE_RENDERER_DLOPEN
Q_EXPORT refexport_t* QDECL GetRefAPI ( int apiVersion, refimport_t *rimp ) {
#else
refexport_t *GetRefAPI ( int apiVersion, refimport_t *rimp ) {
#endif

	static refexport_t	re;

	ri = *rimp;

	Com_Memset( &re, 0, sizeof( re ) );

	if ( apiVersion != REF_API_VERSION ) {
		ri.Printf(PRINT_ALL, "Mismatched REF_API_VERSION: expected %i, got %i\n", 
			REF_API_VERSION, apiVersion );
		return NULL;
	}

	// the RE_ functions are Renderer Entry points

	re.Shutdown = RE_Shutdown;

	re.BeginRegistration = RE_BeginRegistration;
	re.RegisterModel = RE_RegisterModel;
	re.RegisterSkin = RE_RegisterSkin;
	re.RegisterShader = RE_RegisterShader;
	re.RegisterShaderNoMip = RE_RegisterShaderNoMip;
	re.LoadWorld = RE_LoadWorldMap;
	re.SetWorldVisData = RE_SetWorldVisData;
	re.EndRegistration = RE_EndRegistration;

	re.BeginFrame = RE_BeginFrame;
	re.EndFrame = RE_EndFrame;

	re.MarkFragments = R_MarkFragments;
	re.LerpTag = R_LerpTag;
	re.ModelBounds = R_ModelBounds;

	re.ClearScene = RE_ClearScene;
	re.AddRefEntityToScene = RE_AddRefEntityToScene2;
	re.AddPolyToScene = RE_AddPolyToScene2;
	re.LightForPoint = R_LightForPoint;
	re.AddLightToScene = RE_AddLightToScene2;
	re.AddAdditiveLightToScene = RE_AddAdditiveLightToScene;
	re.RenderScene = RE_RenderScene;

	re.SetColor = RE_SetColor;
	re.DrawStretchPic = RE_StretchPic;
	re.DrawStretchRaw = RE_StretchRaw2;
	re.UploadCinematic = RE_UploadCinematic;

	re.RegisterFont = RE_RegisterFont;
	re.RemapShader = R_RemapShader;
	re.GetEntityToken = R_GetEntityToken;
	re.inPVS = R_inPVS;

	re.TakeVideoFrame = RE_TakeVideoFrame;

	//
	// After ioquake3 imports
	//

    re.FreeModels = RE_FreeModels;
	re.EndRegistration = RE_EndRegistration;
    re.SpawnEffectModel = RE_SpawnEffectModel;
    re.RegisterServerModel = RE_RegisterServerModel;
    re.UnregisterServerModel = RE_UnregisterServerModel;
	re.RefreshShaderNoMip = RE_RefreshShaderNoMip;
	re.PrintBSPFileSizes = RE_PrintBSPFileSizes;
	re.MapVersion = RE_MapVersion;
    re.LoadFont = R_LoadFont;

    re.MarkFragmentsForInlineModel = R_MarkFragmentsForInlineModel;
    re.GetInlineModelBounds = R_GetInlineModelBounds;
    re.GetLightingForDecal = R_GetLightingForDecal;
    re.GetLightingForSmoke = R_GetLightingForSmoke;
    re.R_GatherLightSources = R_GatherLightSources;
    re.ModelRadius = R_ModelRadius;
    re.AddPolyToScene = RE_AddPolyToScene2;
    re.AddRefSpriteToScene = RE_AddRefSpriteToScene;
    re.AddTerrainMarkToScene = RE_AddTerrainMarkToScene;

    re.GetRenderEntity = RE_GetRenderEntity;

	re.SavePerformanceCounters = R_SavePerformanceCounters;

	re.R_Model_GetHandle = R_Model_GetHandle;
	re.SetColor = Draw_SetColor;
	re.DrawStretchPic = Draw_StretchPic;
	re.DrawStretchPic2 = Draw_StretchPic2;
	re.DrawStretchRaw = RE_StretchRaw2;
	re.DebugLine = R_DebugLine;
	re.DrawTilePic = Draw_TilePic;
	re.DrawTilePicOffset = Draw_TilePicOffset;
	re.DrawTrianglePic = Draw_TrianglePic;
	re.DrawBox = DrawBox;
	re.AddBox = AddBox;
	re.Set2DWindow = Set2DWindow;
	re.Scissor = RE_Scissor;
	re.DrawLineLoop = DrawLineLoop;
	re.DrawString = R_DrawString;
	re.GetFontHeight = R_GetFontHeight;
	re.GetFontStringWidth = R_GetFontStringWidth;
	re.SwipeBegin = RE_SwipeBegin;
	re.SwipeEnd = RE_SwipeEnd;
	re.SetRenderTime = RE_SetRenderTime;
	re.Noise = R_NoiseGet4f;

	re.SetMode = R_SetMode;
	re.SetFullscreen = R_SetFullscreen;

	re.GetShaderHeight = RE_GetShaderHeight;
	re.GetShaderWidth = RE_GetShaderWidth;
	re.GetShaderName = RE_GetShaderName;
	re.GetModelName = RE_GetModelName;
	re.GetGraphicsInfo = RE_GetGraphicsInfo;
	re.ForceUpdatePose = RE_ForceUpdatePose;
	re.TIKI_Orientation = RE_TIKI_Orientation;
	re.TIKI_IsOnGround = RE_TIKI_IsOnGround;
	re.SetFrameNumber = RE_SetFrameNumber;

	re.ImageExists = R_ImageExists;
	re.CountTextureMemory = R_CountTextureMemory;

    re.LoadRawImage = R_LoadRawImage;
    re.FreeRawImage = R_FreeRawImage;

    // HZM coop - gore tier 4 (UV wounds) - HZM gl2 re-port (bug-gl2-gore),
    // mirrors gl1 tr_init.c:2101-2104
    re.GoreImpact     = RE_GoreImpact;
    re.SetRagdollPose = RE_SetRagdollPose; // HZM coop - ragdoll bridge (gl2 lockstep)
    re.ClearRagdoll   = RE_ClearRagdoll;
    re.ClearAllRagdolls = RE_ClearAllRagdolls;
    re.GoreReset      = RE_GoreReset;
    re.GoreKillSplash = RE_GoreKillSplash; // bug-780

	return &re;
}
