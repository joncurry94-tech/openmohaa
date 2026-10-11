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


#ifndef TR_LOCAL_H
#define TR_LOCAL_H

#include "../qcommon/q_shared.h"
#include "../qcommon/qfiles.h"
#include "../qcommon/qcommon.h"
#include "../renderercommon/tr_public.h"
#include "../renderercommon/tr_common.h"
#include "tr_extratypes.h"
#include "tr_extramath.h"
#include "tr_fbo.h"
#include "tr_postprocess.h"
#include "../renderercommon/iqm.h"
#include "../renderercommon/qgl.h"

#ifdef __cplusplus
#define GLE(ret, name, ...) extern "C" name##proc * qgl##name;
#else
#define GLE(ret, name, ...) extern name##proc * qgl##name;
#endif
QGL_1_1_PROCS;
QGL_DESKTOP_1_1_PROCS;
QGL_1_3_PROCS;
QGL_1_5_PROCS;
QGL_2_0_PROCS;
QGL_3_0_PROCS;
QGL_ARB_occlusion_query_PROCS;
QGL_ARB_framebuffer_object_PROCS;
QGL_ARB_vertex_array_object_PROCS;
QGL_EXT_direct_state_access_PROCS;
#undef GLE

#ifdef __cplusplus
extern "C" {
#endif

#define GL_INDEX_TYPE		GL_UNSIGNED_SHORT
typedef unsigned short glIndex_t;

typedef unsigned int vaoCacheGlIndex_t;

#define BUFFER_OFFSET(i) ((char *)NULL + (i))

// 14 bits
// can't be increased without changing bit packing for drawsurfs
// see QSORT_SHADERNUM_SHIFT
#define SHADERNUM_BITS	14
#define MAX_SHADERS		(1<<SHADERNUM_BITS)

#define	MAX_FBOS      64
#define MAX_VISCOUNTS 5
#define MAX_VAOS      4096

#define MAX_CALC_PSHADOWS    64
#define MAX_DRAWN_PSHADOWS    16 // do not increase past 32, because bit flags are used on surfaces
#define PSHADOW_MAP_SIZE      512

//
// OPENMOHAA-specific stuff
//=========================

// HZM coop [user 2026-09-01, bug-2283] 512 -> 2048. Nine "Ran out of space in the sphere array"
// lines fired in the seconds before the Omaha edict crash. This is the per-frame list of
// spherical lightgrid samples the backend keeps for animated models, so it scales with how many
// SKELETAL models are on screen at once - and that beach now has ~140 actors plus boats, crates
// and gore. Running out is not fatal, it silently drops the lighting sphere for the models past
// the cap, which is a visible pop as men light differently from the man beside them.
#define MAX_SPHERE_LIGHTS		2048
#define	MAX_SPRITESURFS			0x8000

// HZM (engine-limits audit): one sprite surf is emitted per visible refSprite PER VIEW
// (R_AddSpriteSurfaces), and tr.refdef.numSpriteSurfs accumulates across every view in the
// frame (portal sky, mirrors, sun-cascade shadow views). MAX_SPRITESURFS is therefore the real
// ceiling and must never be smaller than MAX_SPRITES; R_AddSpriteSurf indexes spriteSurfs[]
// with it, not with MAX_SPRITES.
#if MAX_SPRITESURFS < MAX_SPRITES
	#error "MAX_SPRITESURFS must be >= MAX_SPRITES (one sprite surf per sprite, per view)"
#endif

#define MAX_SPRITE_DIST				16384.0f
#define MAX_SPRITE_DIST_SQUARED		(MAX_SPRITE_DIST * MAX_SPRITE_DIST)

typedef enum {
    USE_S_COORDS,
    USE_T_COORDS
} texDirection_t;

#define BUNDLE_ANIMATE_ONCE		1

//=========================

typedef struct cubemap_s {
	char name[MAX_QPATH];
	vec3_t origin;
	float parallaxRadius;
	image_t *image;
} cubemap_t;

// HZM gl2 [2026-09-26] Phase S (tr_hzm_spot.c / tr_hzm_spot_rb.c) needs the carrier bit layout here already; the
// header is include-guarded, so the Phase R include further down stays a no-op.
#include "../renderercommon/hzm_light_restore.h"

#define HZM_SPOT_NONE			0		// an omni light - every light but a Phase S headlight/searchlight cone
#define HZM_SPOT_PENDING		1		// a spot whose carrier has not arrived (removed at RE_BeginScene if it never does)
#define HZM_SPOT_READY			2		// a cone
#define HZM_SPOT_MIN_COS_OUTER	0.02f	// outer half-angle < ~89 deg: the cone is exactly 0 behind the lamp
#define HZM_SPOT_DEBUG_OFFSET	4.0f	// u_HzmLightSpot.w += this = r_hzmSpotDebug 2's magenta tint

typedef struct dlight_s {
	vec3_t	origin;
	vec3_t	color;				// range from 0.0 to 1.0, should be color normalized
	float	radius;

	vec3_t	transformed;		// origin in local coordinate system
	int		additive;			// texture detail is lost tho when the lightmap is dark

	//
	// OPENMOHAA-specific stuff
	//

    dlighttype_t type;

	// HZM gl2 [2026-09-26] Phase S1 spot cone (tr_hzm_spot.c). ZEROED ON EVERY ADD (R_HZM_SpotBeginLight): the 32
	// slots are reused every frame, and a muzzle flash must never inherit last frame's cone.
	int		hzmSpot;			// HZM_SPOT_NONE / _PENDING / _READY
	int		hzmTag;				// the carrier tag this light waits for (HZM_SPOT_PENDING)
	vec3_t	hzmAxis;			// world, unit
	vec3_t	hzmAxisLocal;		// in R_TransformDlights' frame - the frame of `transformed`
	float	hzmCosOuter;
	float	hzmConeK;			// 1 / (cosInner - cosOuter)
	int		hzmOwner;			// the entity the sphere pass skips (the vehicle carrying the lamp), -1 = none
} dlight_t;

// HZM gl2 [2026-09-26] Phase S2: one lens flare cgame asked for this scene (decoded from a carrier)
typedef struct hzmFlare_s {
	vec3_t	origin;
	vec3_t	facing;				// unit, the lamp's forward
	float	brightness;			// > 0; cgame folds its day/dark gate in here
	int		id;					// HZM_FlareIdPack(entnum, lamp)
	int		flareClass;			// HZM_FLARE_CLASS_*
} hzmFlare_t;

// HZM gl2 [2026-09-26] Phase S: what one R_AddLightToScene call is (R_HZM_SpotClassify)
typedef enum {
	HZM_ADD_LIGHT,				// an ordinary light (the Phase S fields are zeroed)
	HZM_ADD_SPOT,				// a spot light: takes a slot, pending until its carrier arrives
	HZM_ADD_DROP_SPOT,			// a spot light while spots are off / on Omaha: dropped, never an omni light
	HZM_ADD_SPOT_CARRIER,		// the parameters of a pending spot
	HZM_ADD_FLARE,				// a lens flare
	HZM_ADD_DISCARD				// a carrier that cannot be used (feature off, malformed)
} hzmAdd_t;

// HZM gl2 [2026-09-26] Phase S: what the intake had to throw away (the ^~^~^ HZMSPOT line)
typedef struct {
	int		spotOff;			// a spot light while spots are off / on Omaha: dropped (spot-or-nothing)
	int		orphan;				// a spot carrier that matched no pending light (its light hit the 32-slot cap)
	int		discarded;			// a carrier that could not be used (feature off, malformed)
	int		flareFull;			// more flares than the list holds
} hzmSpotStats_t;

// HZM gl2 [2026-09-26] Phase S2: per-flare occlusion + fade state, kept across frames (in tr - vet F17)
#define HZM_FLARE_SLOTS		32		// == HZM_FLARE_MAX (checked below)
#define HZM_FLARE_RING		3		// query pairs in flight per flare (vet F19)
#if HZM_FLARE_SLOTS != HZM_FLARE_MAX
#error "tr_local.h: HZM_FLARE_SLOTS must equal HZM_FLARE_MAX (renderercommon/hzm_light_restore.h)"
#endif
typedef struct {
	qboolean	inUse;
	hzmFlare_t	req;				// the latest request for this id
	int			lastSeen;			// backEnd.refdef.time of that request
	int			lastUpdate;			// backEnd.refdef.time of the last fade step
	float		target;				// the latest measured visible fraction (a / b)
	float		vis;				// smoothed visibility 0..1
	float		presence;			// smoothed presence 0..1 (fades in / out as the id comes and goes)
	qboolean	measured;			// a result has been read
	int			nextRing;			// the ring slot the next query pair goes into
	qboolean	pending[HZM_FLARE_RING];	// that pair was issued and its result is not read yet
} hzmFlareState_t;


// a trRefEntity_t has all the information passed in by
// the client game, as well as some locally derived info
typedef struct {
	refEntity_t	e;

	float		axisLength;		// compensate for non-normalized axis

	qboolean	needDlights;	// true for bmodels that touch a dlight
	qboolean	lightingCalculated;
	qboolean	mirrored;		// mirrored matrix, needs reversed culling
	vec3_t		lightDir;		// normalized direction towards light, in world space
	vec3_t      modelLightDir;  // normalized direction towards light, in model space
	vec3_t		ambientLight;	// color normalized to 0-255
	int			ambientLightInt;	// 32 bit rgba packed
	vec3_t		directedLight;

	//
	// OPENMOHAA-specific stuff
	//

    int			iGridLighting;
    float		lodpercentage[2];
    // HZM gl2 re-port (bug-gl2-modellight): per-frame model lighting caches,
    // mirrors gl1 trRefEntity_t (gl1 tr_local.h:187-191)
    qboolean	bLightGridCalculated;
    qboolean	sphereCalculated;
    int			lightingSphere;
    // HZM gl2 (bug-gl2-sphereslot-alias): which draw-surf LIST built lightingSphere.
    // sphereCalculated is cleared once per FRAME (tr_scene.c), but the index it validates
    // comes from backEnd.numSpheresUsed, which is cleared once per LIST (tr_backend.c).
    // gl1 has exactly one list per frame so the two scopes agree there; gl2 runs up to five
    // (3 sun cascades + main depth-fill + main colour), so an entity that allocated a slot in
    // a cascade later "reuses" an index another entity has since been given. Stamped and
    // tested only when r_sphereCacheScope is 1. Unsigned: it is compared for equality against
    // a free-running counter, so wrap must be defined behaviour rather than signed UB.
    unsigned int	sphereList;
} trRefEntity_t;


typedef struct {
	vec3_t		origin;			// in world coordinates
	vec3_t		axis[3];		// orientation in world
	vec3_t		viewOrigin;		// viewParms->or.origin in local coordinates
	float		modelMatrix[16];
	float		transformMatrix[16];
} orientationr_t;

// Ensure this is >= the ATTR_INDEX_COUNT enum below
#define VAO_MAX_ATTRIBS 16

typedef enum
{
	VAO_USAGE_STATIC,
	VAO_USAGE_DYNAMIC
} vaoUsage_t;

typedef struct vaoAttrib_s
{
	uint32_t enabled;
	uint32_t count;
	uint32_t type;
	uint32_t normalized;
	uint32_t stride;
	uint32_t offset;
}
vaoAttrib_t;

typedef struct vao_s
{
	char            name[MAX_QPATH];

	uint32_t        vao;

	uint32_t        vertexesVBO;
	int             vertexesSize;	// amount of memory data allocated for all vertices in bytes
	vaoAttrib_t     attribs[VAO_MAX_ATTRIBS];

	uint32_t        frameSize;      // bytes to skip per frame when doing vertex animation

	uint32_t        indexesIBO;
	int             indexesSize;	// amount of memory data allocated for all triangles in bytes
} vao_t;

//===============================================================================

typedef enum {
	SS_BAD,
	SS_PORTAL,			// mirrors, portals, viewscreens
	SS_PORTALSKY,		// HZM gl2 re-port (bug-gl2-portalsky): 3D skybox portal, matches gl1 ordering
	SS_ENVIRONMENT,		// sky box
	SS_OPAQUE,			// opaque

	SS_DECAL,			// scorch marks, etc.
	SS_SEE_THROUGH,		// ladders, grates, grills that may have small blended edges
						// in addition to alpha test
	SS_BANNER,

	SS_FOG,

	SS_UNDERWATER,		// for items that should be drawn in front of the water plane

	SS_BLEND0,			// regular transparency and filters
	SS_BLEND1,			// generally only used for additive type effects
	SS_BLEND2,
	SS_BLEND3,

	SS_BLEND6,
	SS_STENCIL_SHADOW,
	SS_ALMOST_NEAREST,	// gun smoke puffs

	SS_NEAREST			// blood blobs
} shaderSort_t;


#define MAX_SHADER_STAGES 8

typedef enum {
	GF_NONE,

	GF_SIN,
	GF_SQUARE,
	GF_TRIANGLE,
	GF_SAWTOOTH, 
	GF_INVERSE_SAWTOOTH, 

	GF_NOISE

} genFunc_t;


typedef enum {
	DEFORM_NONE,
	DEFORM_WAVE,
	DEFORM_NORMALS,
	DEFORM_BULGE,
	DEFORM_MOVE,
	DEFORM_PROJECTION_SHADOW,
	DEFORM_AUTOSPRITE,
	DEFORM_AUTOSPRITE2,
	DEFORM_TEXT0,
	DEFORM_TEXT1,
	DEFORM_TEXT2,
	DEFORM_TEXT3,
	DEFORM_TEXT4,
	DEFORM_TEXT5,
	DEFORM_TEXT6,
	DEFORM_TEXT7,
	//
	// OPENMOHAA-specific stuff
	//=========================
	DEFORM_LIGHTGLOW,
	DEFORM_FLAP_S,
	DEFORM_FLAP_T,
	//=========================
} deform_t;

// deformVertexes types that can be handled by the GPU
typedef enum
{
	// do not edit: same as genFunc_t

	DGEN_NONE,
	DGEN_WAVE_SIN,
	DGEN_WAVE_SQUARE,
	DGEN_WAVE_TRIANGLE,
	DGEN_WAVE_SAWTOOTH,
	DGEN_WAVE_INVERSE_SAWTOOTH,
	DGEN_WAVE_NOISE,

	// do not edit until this line

	DGEN_BULGE,
	DGEN_MOVE
} deformGen_t;

typedef enum {
	AGEN_IDENTITY,
	AGEN_SKIP,
	AGEN_ENTITY,
	AGEN_ONE_MINUS_ENTITY,
	AGEN_VERTEX,
	AGEN_ONE_MINUS_VERTEX,
	AGEN_LIGHTING_SPECULAR,
	AGEN_WAVEFORM,
	AGEN_PORTAL,
    AGEN_CONST,

    //
    // OPENMOHAA-specific stuff
    //
	
	AGEN_NOISE,
	AGEN_DOT,
	AGEN_ONE_MINUS_DOT,
	AGEN_CONSTANT,
	AGEN_GLOBAL_ALPHA,
	AGEN_SKYALPHA,
	AGEN_ONE_MINUS_SKYALPHA,
	AGEN_SCOORD,
	AGEN_TCOORD,
	AGEN_DIST_FADE,
    AGEN_ONE_MINUS_DIST_FADE,
    AGEN_TIKI_DIST_FADE,
    AGEN_ONE_MINUS_TIKI_DIST_FADE,
	AGEN_DOT_VIEW,
	AGEN_ONE_MINUS_DOT_VIEW,
	AGEN_HEIGHT_FADE,
} alphaGen_t;

typedef enum {
	CGEN_BAD,
	CGEN_IDENTITY_LIGHTING,	// tr.identityLight
	CGEN_IDENTITY,			// always (1,1,1,1)
	CGEN_ENTITY,			// grabbed from entity's modulate field
	CGEN_ONE_MINUS_ENTITY,	// grabbed from 1 - entity.modulate
	CGEN_EXACT_VERTEX,		// tess.vertexColors
	CGEN_VERTEX,			// tess.vertexColors * tr.identityLight
	CGEN_EXACT_VERTEX_LIT,	// like CGEN_EXACT_VERTEX but takes a light direction from the lightgrid
	CGEN_VERTEX_LIT,		// like CGEN_VERTEX but takes a light direction from the lightgrid
	CGEN_ONE_MINUS_VERTEX,
	CGEN_WAVEFORM,			// programmatically generated
	CGEN_LIGHTING_DIFFUSE,
	CGEN_FOG,				// standard fog
	CGEN_CONST,				// fixed color

	//
	// OPENMOHAA-specific stuff
	//
	
    CGEN_MULTIPLY_BY_WAVEFORM,
    CGEN_LIGHTING_GRID,
    CGEN_LIGHTING_SPHERICAL,
    CGEN_NOISE,
    CGEN_GLOBAL_COLOR,
    CGEN_STATIC,
    CGEN_SCOORD,
    CGEN_TCOORD,
    CGEN_DOT,
    CGEN_ONE_MINUS_DOT,
} colorGen_t;

typedef enum {
	TCGEN_BAD,
	TCGEN_IDENTITY,			// clear to 0,0
	TCGEN_LIGHTMAP,
	TCGEN_TEXTURE,
	TCGEN_ENVIRONMENT_MAPPED,
	TCGEN_FOG,
	TCGEN_VECTOR,			// S and T from world coordinates
	TCGEN_ENVIRONMENT_MAPPED2	// HZM gl2 re-port: MOHAA 'texgen environmentmodel' (model-space env map)
} texCoordGen_t;

typedef enum {
	ACFF_NONE,
	ACFF_MODULATE_RGB,
	ACFF_MODULATE_RGBA,
	ACFF_MODULATE_ALPHA
} acff_t;

typedef struct {
	genFunc_t	func;

	float base;
	float amplitude;
	float phase;
	float frequency;
} waveForm_t;

#define TR_MAX_TEXMODS 4

typedef enum {
	TMOD_NONE,
	TMOD_TRANSFORM,
	TMOD_TURBULENT,
	TMOD_SCROLL,
	TMOD_SCALE,
	TMOD_STRETCH,
	TMOD_ROTATE,
	TMOD_ENTITY_TRANSLATE,
	// HZM gl2 parity (bug-1242): MOHAA-specific tcMods that gl1 has and gl2 never got. Appended at
	// the END deliberately - inserting mid-enum would renumber every value above it and silently
	// repoint any texMod already parsed. 36 live `tcMod wavetrant` lines drive the animated surf
	// wash on the D-Day and both Africa shorelines (misc_outside.shader); under gl2 they hit the
	// unknown-tcMod warning in ParseTexMod, the type stays TMOD_NONE, and the wash renders STATIC.
	TMOD_WAVETRANS,
	TMOD_WAVETRANT
} texMod_t;

#define	MAX_SHADER_DEFORMS	3
typedef struct {
	deform_t	deformation;			// vertex coordinate modification type

	vec3_t		moveVector;
	waveForm_t	deformationWave;
	float		deformationSpread;

	float		bulgeWidth;
	float		bulgeHeight;
	float		bulgeSpeed;
} deformStage_t;


typedef struct {
	texMod_t		type;

	// used for TMOD_TURBULENT and TMOD_STRETCH
	waveForm_t		wave;

	// used for TMOD_TRANSFORM
	float			matrix[2][2];		// s' = s * m[0][0] + t * m[1][0] + trans[0]
	float			translate[2];		// t' = s * m[0][1] + t * m[0][1] + trans[1]

	// used for TMOD_SCALE
	float			scale[2];			// s *= scale[0]
	                                    // t *= scale[1]

	// used for TMOD_SCROLL
	float			scroll[2];			// s' = s + scroll[0] * time
										// t' = t + scroll[1] * time

	// + = clockwise
	// - = counterclockwise
	float			rotateSpeed;

} texModInfo_t;


//#define	MAX_IMAGE_ANIMATIONS	8
//
// OPENMOHAA-specific stuff
//=========================
#define	MAX_IMAGE_ANIMATIONS	64
//=========================

typedef struct {
	image_t			*image[MAX_IMAGE_ANIMATIONS];
	int				numImageAnimations;
	float			imageAnimationSpeed;

	texCoordGen_t	tcGen;
	vec3_t			tcGenVectors[2];

	int				numTexMods;
	texModInfo_t	*texMods;

	int				videoMapHandle;
	qboolean		isLightmap;
	qboolean		isVideoMap;

	//
	// OPENMOHAA-specific stuff
    //=========================
    float imageAnimationPhase;
    int flags;
    //=========================
} textureBundle_t;

enum
{
	TB_COLORMAP    = 0,
	TB_DIFFUSEMAP  = 0,
	TB_LIGHTMAP    = 1,
	TB_LEVELSMAP   = 1,
	TB_SHADOWMAP3  = 1,
	TB_NORMALMAP   = 2,
	TB_DELUXEMAP   = 3,
	TB_SHADOWMAP2  = 3,
	TB_SPECULARMAP = 4,
	TB_SHADOWMAP   = 5,
	TB_CUBEMAP     = 6,
	TB_SHADOWMAP4  = 6,
	NUM_TEXTURE_BUNDLES = 7,
	// HZM gl2 soft particles (r_softParticles): the scene-depth snapshot binds on a NINTH TMU,
	// past the 0-6 bundle range (lightall already uses all seven). NUM_TEXTURE_BUNDLES still
	// sizes the per-stage bundle[] and tess texcoords[] arrays and MUST stay 7; NUM_TEXTURE_UNITS
	// only sizes the DSA texture bind cache (tr_dsa.c), so widening that to 8 is safe.
	TB_SCREENDEPTH = 7,
	// HZM gl2 stable sun shadows + B0 (plan P3, shadows vet section 7 D1): W, the static world cascade, is sampled per
	// pixel in lightall on unit 8. B0 resolves to legacy below 9 units (R_SunStable_Resolve).
	TB_SUNWORLD    = 8,
	// HZM rain wetness (tr_hzm_wet.c, water_wetness_2026-09-27 plan W4). Unit 8 is RESERVED for the shadows plan's
	// TB_SUNWORLD (docs/proposals/shadows_2026-09-26/vet.md) - do not take it. NUM_TEXTURE_UNITS only sizes the DSA
	// bind cache (tr_dsa.c); GL 3.x guarantees 16 fragment units.
	TB_HZMRAINOCC  = 9,
	TB_HZMWETNOISE = 10,
	NUM_TEXTURE_UNITS = 11
};

typedef enum
{
	// material shader stage types
	ST_COLORMAP = 0,			// vanilla Q3A style shader treatening
	ST_DIFFUSEMAP = 0,          // treat color and diffusemap the same
	ST_NORMALMAP,
	ST_NORMALPARALLAXMAP,
	ST_SPECULARMAP,
	ST_GLSL
} stageType_t;

typedef struct {
	qboolean		active;
	
	textureBundle_t	bundle[NUM_TEXTURE_BUNDLES];

	waveForm_t		rgbWave;
	colorGen_t		rgbGen;

	waveForm_t		alphaWave;
	alphaGen_t		alphaGen;

	byte			constantColor[4];			// for CGEN_CONST and AGEN_CONST

	unsigned		stateBits;					// GLS_xxxx mask

	acff_t			adjustColorsForFog;

	qboolean		isDetail;

	stageType_t     type;
	struct shaderProgram_s *glslShaderGroup;
	int glslShaderIndex;

	vec4_t normalScale;
	vec4_t specularScale;

	//
	// OPENMOHAA-SPECIFIC stuff
	//

	float			alphaMin;
	float			alphaMax;
	vec3_t			specOrigin;

    byte			colorConst[4];			// for CGEN_CONST and AGEN_CONST
	byte			alphaConst;
	byte			alphaConstMin;

	// HZM gl2 re-port (bug-gl2-nextbundle2): MOHAA 'nextbundle [add]' combine mode
	// (GL_MODULATE / GL_ADD, 0 = single bundle) - gl1 tr_shader.c:1675-1694
	int				multitextureEnv;

	// HZM gl2 (r_hzmGenNormals): this stage's TB_NORMALMAP is a map we SYNTHESISED from the
	// diffuse, not authored art. Set in CollapseStagesToLightall, which is the first point at
	// which the stage is known to have resolved to a real light type. Consumed by
	// RB_IterateStagesGeneric to make the relief strength live and to confine r_hzmSpecular to
	// exactly these stages - a global r_baseSpecular is the bug-801 "white sheen on everything"
	// class of regression, which is why that cvar defaults to 0 in this fork.
	qboolean		hzmGenNormal;

	// HZM gl2 [2026-09-25] r_skyHD sky LAYERS: in r_skyHDCompare mode the HD twin of this sky stage's image
	// (env/hzmhd/clouds/...), swapped in by RB_StageIteratorSky while compare is 1. NULL otherwise - in normal
	// HD mode the twin simply replaces bundle[0].image[0] (tr_shader.c R_HZM_SkyHDLayers).
	image_t			*hzmSkyHDAlt;
} shaderStage_t;

struct shaderCommands_s;

typedef enum {
	CT_FRONT_SIDED,
	CT_BACK_SIDED,
	CT_TWO_SIDED
} cullType_t;

typedef enum {
	FP_NONE,		// surface is translucent and will just be adjusted properly
	FP_EQUAL,		// surface is opaque but possibly alpha tested
	FP_LE			// surface is trnaslucent, but still needs a fog pass (fog surface)
} fogPass_t;

typedef struct {
	float		cloudHeight;
	image_t		*outerbox[6], *innerbox[6];
	// HZM gl2 [2026-09-25] r_skyHD: the HD box (env/hzmhd/...), kept here ONLY in r_skyHDCompare mode, where
	// outerbox holds the original and DrawSkyBox picks per frame. Otherwise NULL: in normal HD mode the HD set
	// replaces outerbox outright and the original is never loaded (no doubled VRAM).
	image_t		*outerboxAlt[6];
} skyParms_t;

typedef struct {
	vec3_t	color;
	float	depthForOpaque;
} fogParms_t;

//
// OPENMOHAA-specific stuff
//
//=============================

typedef enum {
	SPRITE_PARALLEL,
	SPRITE_PARALLEL_ORIENTED,
	SPRITE_ORIENTED,
	SPRITE_PARALLEL_UPRIGHT
} spriteType_t;

typedef struct {
  spriteType_t type;
  float scale;
} spriteParms_t;

//=============================

typedef struct shader_s {
	char		name[MAX_QPATH];		// game path, including extension
	int			lightmapIndex;			// for a shader to match, both name and lightmapIndex must match

	int			index;					// this shader == tr.shaders[index]
	int			sortedIndex;			// this shader == tr.sortedShaders[sortedIndex]

	float		sort;					// lower numbered shaders draw before higher numbered

	qboolean	defaultShader;			// we want to return index 0 if the shader failed to
										// load for some reason, but R_FindShader should
										// still keep a name allocated for it, so if
										// something calls RE_RegisterShader again with
										// the same name, we don't try looking for it again

	qboolean	explicitlyDefined;		// found in a .shader file

	int			surfaceFlags;			// if explicitlyDefined, this will have SURF_* flags
	int			contentFlags;
	int			hzmWetBits;				// HZM rain wetness: BSP surface-type bits OR-ed at load (tr_hzm_wet.c)
	int			hzmWater;				// HZM water pass: hzm_waterwet.h allowlist slot + 1, 0 = never (tr_hzm_water.c)
	float		hzmWaterLmRef;			// HZM water pass: p90 lightmap luminance of this shader name, 0 = no glint gate
	int			hzmGvClass;				// HZM ground variety: 0 none, 1 macro, 2 macro + hex (tr_hzm_groundvar.c)
	float		hzmGvPeriod;			// HZM ground variety: world units per texture repeat (generated list)
	int			hzmGvWorld;				// HZM ground variety: the world load that tagged it (stale tags never count)

	qboolean	entityMergable;			// merge across entites optimizable (smoke, blood)

	qboolean	isSky;
	skyParms_t	sky;
	fogParms_t	fogParms;

	float		portalRange;			// distance to fog out at
	qboolean	isPortal;
	qboolean	isPortalSky;			// HZM gl2 re-port (bug-gl2-portalsky): shader is textures/common/skyportal-style portal into the 3D sky room

	cullType_t	cullType;				// CT_FRONT_SIDED, CT_BACK_SIDED, or CT_TWO_SIDED
	qboolean	polygonOffset;			// set for decals and other items that must be offset
	qboolean	noMipMaps;				// for console fonts, 2D elements, etc.
	qboolean	noPicMip;				// for images that must always be full resolution
	// HZM gl2 re-port (bug-gl2-foliage-white): shader has an alpha-tested (cutout)
	// stage. gl1 has no depth prepass; gl2's prepass (r_depthPrepass 1) must NOT
	// write full-quad depth for these or the discarded transparent texels occlude
	// the background and reveal the light sky/fog (opaque-white foliage billboards).
	qboolean	hasAlphaTest;

	fogPass_t	fogPass;				// draw a blended pass, possibly with depth test equals
	qboolean	noGlobalFog;			// [HZM 2026-08-31] shader asked for "nofog". gl1 honours this
										// (renderergl1/tr_shader.c:881 sets fogBits = GLS_FOG); gl2
										// parsed the keyword and threw it away with a FIXME, so every
										// nofog surface got fogged anyway. Invisible until the forward
										// global fog landed on 2026-08-03 - after which retail's own
										// nofog tracers and muzzle flashes fogged toward BLACK (the
										// correct target for an additive stage) and rendered as dark
										// bands in heavy fog. User-reported on e1l1, which is thick dust.
	qboolean	noSoftParticles;		// [HZM soft particles] shader asked for "nosoftparticles": never
										// depth-fade this shader's sprites (first-person smoke wisps in front
										// of the viewmodel, or any emitter that must stay hard-edged).
	float		hzmSoftEdge;			// [HZM S3b] "qer_hzmSoftEdge <dist>": a MODEL or world surface that fades
										// out within <dist> u of the scene depth behind it, like a soft particle
										// (the headlight fog beam). 0 = off. gl1 and every older gl2 skip qer*.

	int         vertexAttribs;          // not all shaders will need all data to be gathered

	int			numDeforms;
	deformStage_t	deforms[MAX_SHADER_DEFORMS];

	int			numUnfoggedPasses;
	shaderStage_t	*stages[MAX_SHADER_STAGES];		

	void		(*optimalStageIteratorFunc)( void );

  double clampTime;                                  // time this shader is clamped to
  double timeOffset;                                 // current time offset for this shader

  struct shader_s *remappedShader;                  // current shader this one is remapped too

	struct	shader_s	*next;

	//
	// OPENMOHAA-specific stuff
	//
	qboolean force32bit;
    float fDistRange;
    float fDistNear;
    spriteParms_t sprite;
    // HZM gl2 re-port (bug-gl2-modellight): set in FinishShader when a stage
    // uses rgbGen lightingGrid / lightingSpherical / static, consumed by the
    // backend model-lighting setup (mirrors gl1 tr_local.h:599-600)
    int needsLGrid;
    int needsLSpherical;
    // HZM gl2 parity (bug-1300): set in FinishShader when a stage uses alphaGen
    // distFade / oneMinusDistFade - the two PER-VERTEX distance-fade modes, whose
    // alpha RB_FillDistFadeAlpha has to write into tess.color before the attribute
    // upload. Deliberately NOT set for tikiDistFade / oneMinusTikiDistFade: those
    // are constant per draw and are handled entirely in ComputeShaderColors.
    int needsDistFade;
    // Latch so the unimplemented-alphaGen warning in ComputeShaderColors prints
    // once per shader instead of once per batch per frame.
    qboolean alphaGenWarned;
    // HZM gl2 [2026-09-26] searchlights S1: latch for the one-per-shader ^~^~^ SEARCHLIGHT line (tr_shade.c).
    qboolean hzmDotToRgbNoted;
} shader_t;

enum
{
	ATTR_INDEX_POSITION       = 0,
	ATTR_INDEX_TEXCOORD       = 1,
	ATTR_INDEX_LIGHTCOORD     = 2,
	ATTR_INDEX_TANGENT        = 3,
	ATTR_INDEX_NORMAL         = 4,
	ATTR_INDEX_COLOR          = 5,
	ATTR_INDEX_PAINTCOLOR     = 6,
	ATTR_INDEX_LIGHTDIRECTION = 7,
	ATTR_INDEX_BONE_INDEXES   = 8,
	ATTR_INDEX_BONE_WEIGHTS   = 9,

	// GPU vertex animations
	ATTR_INDEX_POSITION2      = 10,
	ATTR_INDEX_TANGENT2       = 11,
	ATTR_INDEX_NORMAL2        = 12,
	
	ATTR_INDEX_COUNT          = 13
};

enum
{
	ATTR_POSITION =       1 << ATTR_INDEX_POSITION,
	ATTR_TEXCOORD =       1 << ATTR_INDEX_TEXCOORD,
	ATTR_LIGHTCOORD =     1 << ATTR_INDEX_LIGHTCOORD,
	ATTR_TANGENT =        1 << ATTR_INDEX_TANGENT,
	ATTR_NORMAL =         1 << ATTR_INDEX_NORMAL,
	ATTR_COLOR =          1 << ATTR_INDEX_COLOR,
	ATTR_PAINTCOLOR =     1 << ATTR_INDEX_PAINTCOLOR,
	ATTR_LIGHTDIRECTION = 1 << ATTR_INDEX_LIGHTDIRECTION,
	ATTR_BONE_INDEXES =   1 << ATTR_INDEX_BONE_INDEXES,
	ATTR_BONE_WEIGHTS =   1 << ATTR_INDEX_BONE_WEIGHTS,

	// for .md3 interpolation
	ATTR_POSITION2 =      1 << ATTR_INDEX_POSITION2,
	ATTR_TANGENT2 =       1 << ATTR_INDEX_TANGENT2,
	ATTR_NORMAL2 =        1 << ATTR_INDEX_NORMAL2,

	ATTR_DEFAULT = ATTR_POSITION,
	ATTR_BITS =	ATTR_POSITION |
				ATTR_TEXCOORD |
				ATTR_LIGHTCOORD |
				ATTR_TANGENT |
				ATTR_NORMAL |
				ATTR_COLOR |
				ATTR_PAINTCOLOR |
				ATTR_LIGHTDIRECTION |
				ATTR_BONE_INDEXES |
				ATTR_BONE_WEIGHTS |
				ATTR_POSITION2 |
				ATTR_TANGENT2 |
				ATTR_NORMAL2
};

enum
{
	GENERICDEF_USE_DEFORM_VERTEXES  = 0x0001,
	GENERICDEF_USE_TCGEN_AND_TCMOD  = 0x0002,
	GENERICDEF_USE_VERTEX_ANIMATION = 0x0004,
	GENERICDEF_USE_FOG              = 0x0008,
	GENERICDEF_USE_RGBAGEN          = 0x0010,
	GENERICDEF_USE_BONE_ANIMATION   = 0x0020,
	GENERICDEF_ALL                  = 0x003F,
	GENERICDEF_COUNT                = 0x0040,
};

enum
{
	FOGDEF_USE_DEFORM_VERTEXES  = 0x0001,
	FOGDEF_USE_VERTEX_ANIMATION = 0x0002,
	FOGDEF_USE_BONE_ANIMATION   = 0x0004,
	FOGDEF_ALL                  = 0x0007,
	FOGDEF_COUNT                = 0x0008,
};

enum
{
	DLIGHTDEF_USE_DEFORM_VERTEXES  = 0x0001,
	DLIGHTDEF_ALL                  = 0x0001,
	DLIGHTDEF_COUNT                = 0x0002,
};

enum
{
	LIGHTDEF_USE_LIGHTMAP        = 0x0001,
	LIGHTDEF_USE_LIGHT_VECTOR    = 0x0002,
	LIGHTDEF_USE_LIGHT_VERTEX    = 0x0003,
	LIGHTDEF_LIGHTTYPE_MASK      = 0x0003,
	LIGHTDEF_ENTITY_VERTEX_ANIMATION = 0x0004,
	LIGHTDEF_USE_TCGEN_AND_TCMOD = 0x0008,
	LIGHTDEF_USE_PARALLAXMAP     = 0x0010,
	LIGHTDEF_USE_SHADOWMAP       = 0x0020,
	LIGHTDEF_ENTITY_BONE_ANIMATION = 0x0040,
	LIGHTDEF_ALL                 = 0x007F,
	LIGHTDEF_COUNT               = 0x0080
};

enum
{
	SHADOWMAPDEF_USE_VERTEX_ANIMATION = 0x0001,
	SHADOWMAPDEF_USE_BONE_ANIMATION   = 0x0002,
	SHADOWMAPDEF_ALL                  = 0x0003,
	SHADOWMAPDEF_COUNT                = 0x0004
};

enum
{
	GLSL_INT,
	GLSL_FLOAT,
	GLSL_FLOAT5,
	GLSL_VEC2,
	GLSL_VEC3,
	GLSL_VEC4,
	GLSL_MAT16,
	GLSL_MAT16_BONEMATRIX
};

typedef enum
{
	UNIFORM_DIFFUSEMAP = 0,
	UNIFORM_LIGHTMAP,
	UNIFORM_NORMALMAP,
	UNIFORM_DELUXEMAP,
	UNIFORM_SPECULARMAP,

	UNIFORM_TEXTUREMAP,
	UNIFORM_LEVELSMAP,
	UNIFORM_CUBEMAP,

	UNIFORM_SCREENIMAGEMAP,
	UNIFORM_SCREENDEPTHMAP,

	UNIFORM_SHADOWMAP,
	UNIFORM_SHADOWMAP2,
	UNIFORM_SHADOWMAP3,
	UNIFORM_SHADOWMAP4,

	UNIFORM_SHADOWMVP,
	UNIFORM_SHADOWMVP2,
	UNIFORM_SHADOWMVP3,
	UNIFORM_SHADOWMVP4,

	UNIFORM_ENABLETEXTURES,

	UNIFORM_DIFFUSETEXMATRIX0,
	UNIFORM_DIFFUSETEXMATRIX1,
	UNIFORM_DIFFUSETEXMATRIX2,
	UNIFORM_DIFFUSETEXMATRIX3,
	UNIFORM_DIFFUSETEXMATRIX4,
	UNIFORM_DIFFUSETEXMATRIX5,
	UNIFORM_DIFFUSETEXMATRIX6,
	UNIFORM_DIFFUSETEXMATRIX7,

	UNIFORM_TCGEN0,
	UNIFORM_TCGEN0VECTOR0,
	UNIFORM_TCGEN0VECTOR1,

	UNIFORM_DEFORMGEN,
	UNIFORM_DEFORMPARAMS,

	UNIFORM_COLORGEN,
	UNIFORM_ALPHAGEN,
	UNIFORM_COLOR,
	UNIFORM_BASECOLOR,
	UNIFORM_VERTCOLOR,

	UNIFORM_DLIGHTINFO,
	UNIFORM_LIGHTFORWARD,
	UNIFORM_LIGHTUP,
	UNIFORM_LIGHTRIGHT,
	UNIFORM_LIGHTORIGIN,
	UNIFORM_MODELLIGHTDIR,
	UNIFORM_LIGHTRADIUS,
	UNIFORM_AMBIENTLIGHT,
	UNIFORM_DIRECTEDLIGHT,

	UNIFORM_PORTALRANGE,

	UNIFORM_FOGDISTANCE,
	UNIFORM_FOGDEPTH,
	UNIFORM_FOGEYET,
	UNIFORM_FOGCOLORMASK,

	UNIFORM_MODELMATRIX,
	UNIFORM_MODELVIEWPROJECTIONMATRIX,

	UNIFORM_TIME,
	UNIFORM_VERTEXLERP,
	UNIFORM_NORMALSCALE,
	UNIFORM_SPECULARSCALE,

	UNIFORM_VIEWINFO, // znear, zfar, width/2, height/2
	UNIFORM_VIEWORIGIN,
	UNIFORM_LOCALVIEWORIGIN,
	UNIFORM_VIEWFORWARD,
	UNIFORM_VIEWLEFT,
	UNIFORM_VIEWUP,

	UNIFORM_INVTEXRES,
	UNIFORM_HZMPARAMS,   // HZM gl2 post-FX: spare vec4 for the ported gl1 stages (bug-1151)
	UNIFORM_AUTOEXPOSUREMINMAX,
	UNIFORM_TONEMINAVGMAXLINEAR,

	UNIFORM_PRIMARYLIGHTORIGIN,
	UNIFORM_PRIMARYLIGHTCOLOR,
	UNIFORM_PRIMARYLIGHTAMBIENT,
	UNIFORM_PRIMARYLIGHTRADIUS,

	UNIFORM_CUBEMAPINFO,

	UNIFORM_ALPHATEST,

	UNIFORM_BONEMATRIX,

	//
	// OPENMOHAA-specific stuff
	//=========================
	// HZM gl2 re-port (bug-gl2-nextbundle2): second texture bundle in the
	// generic program (MOHAA 'nextbundle' single-pass multitexture)
	UNIFORM_TEXTURE1ENV,     // 0 = off, 1 = GL_MODULATE, 2 = GL_ADD
	UNIFORM_TEXTURE1TCGEN,   // tcGen of bundle[1] (TCGEN_LIGHTMAP -> attr_TexCoord1)
	UNIFORM_TEXTURE1MATRIX0, // bundle[1] tcMod matrix slots (same layout as DIFFUSETEXMATRIX)
	UNIFORM_TEXTURE1MATRIX1,
	UNIFORM_TEXTURE1MATRIX2,
	UNIFORM_TEXTURE1MATRIX3,
	UNIFORM_TEXTURE1MATRIX4,
	UNIFORM_TEXTURE1MATRIX5,
	UNIFORM_TEXTURE1MATRIX6,
	UNIFORM_TEXTURE1MATRIX7,
	//=========================

	// HZM gl2 parity (bug-1249): (alphaMin, alphaMax, loClamp, hiClamp) for the MOHAA per-vertex
	// alphaGens sCoord/tCoord. Appended LAST on purpose - uniformsInfo[] is indexed by this enum and
	// GLSL_InitUniforms walks i < UNIFORM_COUNT unguarded, so inserting mid-enum silently resolves
	// every later uniform to the wrong name. Keep the matching uniformsInfo[] row last too.
	UNIFORM_ALPHAGENPARAMS,

	// HZM gl2 FORWARD GLOBAL FOG (r_globalFogForward). Appended last, per the rule above.
	//   u_GlobalFogColor  = (fogTarget.rgb, fracScale)   fracScale 0 = no fog on this draw
	//   u_GlobalFogParams = (projMat[10], projMat[14], fogStart, 1/(fogEnd-fogStart))
	UNIFORM_GLOBALFOGCOLOR,
	UNIFORM_GLOBALFOGPARAMS,

	// HZM coop (bug-2508): alphaGen lightingSpecular light for generic_vp. Appended last, per the
	// rule above. u_LightOrigin convention: w == 0 direction toward the light, w == 1 point.
	UNIFORM_HZMSPECLIGHT,

	// HZM render-scale supersampling + AMD FSR 1 (r_renderScale). Appended LAST, per the rule
	// above: uniformsInfo[] is indexed by this enum, so a mid-enum insert resolves every later
	// uniform to the wrong name. EASU uses all four; RCAS and the SSAA downsample use u_FsrCon0.
	UNIFORM_FSRCON0,
	UNIFORM_FSRCON1,
	UNIFORM_FSRCON2,
	UNIFORM_FSRCON3,

	// HZM gl2 soft particles (r_softParticles). Appended LAST, per the rule above:
	// uniformsInfo[] is indexed by this enum, so a mid-enum insert resolves every later
	// uniform to the wrong name.
	//   u_SoftParticle = (1/fadeDistance, projMat[10], projMat[14], mode)
	//   mode 0 off, 1 fade alpha, 2 fade rgb (additive), 3 lerp toward white (modulate), 4 debug
	UNIFORM_SOFTPARTICLE,

	// HZM gl2 [2026-09-26] Phase S1 spot cone (tr_hzm_spot.c). Appended LAST, per the rule above; the gfx P2/P4
	// uniforms append AFTER this one (plan_phaseS.md section 14), and tr_glsl.c checks the table length at compile time.
	//   u_HzmLightSpot = (cone axis * k, cosOuter [+4 = the r_hzmSpotDebug 2 tint]); ALL ZERO = an omni light. GPU
	//   uniforms start at 0 and GLSL_SetUniformVec4 caches from 0, so zero MUST be the inert value (plan finding 5).
	UNIFORM_HZMLIGHTSPOT,

	// HZM gl2 [2026-09-26] S4 spot shadows (gfx tree). Appended LAST, per the rule above (P2/P4 append after it).
	//   u_HzmPshadowSpot = (spot apex, light radius) for the pshadow receive pass; w 0 = not a spot = no mask.
	UNIFORM_HZMPSHADOWSPOT,
	// HZM rain wetness (tr_hzm_wet.c). Appended LAST, per the rule above; rows in the same order in uniformsInfo[].
	//   u_HzmWet = (film, puddle, time mod 3600, on). on == 0 = the lightall wet branch is skipped: ALL ZERO is inert.
	UNIFORM_HZMWET,
	UNIFORM_HZMWETMAT,      // (darken, wet roughness, puddles allowed, sealed)
	UNIFORM_HZMWETOCC,      // rain-occlusion map world->uv: (x0, y0, 1/width, 1/height)
	UNIFORM_HZMWETOCCZ,     // (zmin, zrange, puddle threshold, lightmap sheen)
	UNIFORM_HZMWETSKYZ,     // sky zenith rgb
	UNIFORM_HZMWETSKYH,     // sky horizon rgb
	UNIFORM_HZMWETSUN,      // toward-sun xyz, sun present
	UNIFORM_HZMWETSUNCOL,   // sun rgb, max component <= 1
	UNIFORM_HZMOCCMAP,      // sampler, TB_HZMRAINOCC
	UNIFORM_HZMWETNOISE,    // sampler, TB_HZMWETNOISE

	// HZM water pass (tr_hzm_water.c). Appended LAST, rows in the same order in uniformsInfo[].
	UNIFORM_HZMWATER,       // (fade, time mod 1000, lightmap reference or 0, glint on)
	UNIFORM_HZMRIPPLEMAP,   // sampler, TB_HZMWETNOISE (the water program has no puddle noise)
	// HZM ground variety (tr_hzm_groundvar.c). Inserted here, rows in the same place in uniformsInfo[] (the table
	// length is checked at compile time); kept off the enum's end, where the gfx tree appends. ALL ZERO = inert.
	//   u_HzmGroundVar  = (mode 0/1/2, macro strength, 1 / hex cell in world units, blend sharpness)
	//   u_HzmGroundVar2 = (luminance weighting, steep-face gate lo, gate hi, debug)
	UNIFORM_HZMGROUNDVAR,
	UNIFORM_HZMGROUNDVAR2,
	// HZM coop [2026-09-27] realistic lightning (tr_hzm_lightning.c, renderercommon/hzm_lightning.h). Appended LAST,
	// per the rule above (the gfx branch appends after UNIFORM_HZMLIGHTSPOT too: a trivial append conflict at merge).
	// ALL ZERO = inert (nothing added anywhere), which is also what every program starts with.
	//   u_HzmLtFog    = (strike direction in EYE space, haze energy)          lightall/generic ApplyGlobalFog
	//   u_HzmLtView   = (viewport x, y, 1/width, 1/height)                     lightall/generic ApplyGlobalFog
	//   u_HzmLtProj   = (1/projMat[0], 1/projMat[5], 0, 0)                     lightall/generic ApplyGlobalFog
	//   u_HzmLtWorld  = (strike direction in WORLD space, surface/sky energy)  lightall surface light, sky pass
	//   u_HzmLtBolt   = (bolt centre direction WORLD, bolt energy)             sky pass
	//   u_HzmLtParams = (tan bolt half-width, tan bolt half-height, sin lowest bolt elevation, 0)   sky pass
	UNIFORM_HZMLTFOG,
	UNIFORM_HZMLTVIEW,
	UNIFORM_HZMLTPROJ,
	UNIFORM_HZMLTWORLD,
	UNIFORM_HZMLTBOLT,
	UNIFORM_HZMLTPARAMS,
	// HZM gl2 stable sun shadows + B0 (plan P3, tr_hzm_sunstable.c). Appended LAST, rows in the same order in
	// uniformsInfo[]. u_HzmSunMaskOnly.x == 0 (every program's start value, and what every draw uploads while
	// r_shadowStable resolves to legacy) keeps lightall exactly today's.
	UNIFORM_HZMSUNWORLDMAP,   // sampler2DShadow, TB_SUNWORLD (lightall)
	UNIFORM_HZMSUNWORLDMVP,   // W's world -> clip (lightall)
	UNIFORM_HZMSUNMASKONLY,   // (mode, normal offset u, W bias, half a W texel uv) (lightall)
	UNIFORM_HZMSHADOWSPLITS,  // (R0, R1, R2, blend band) (stable mask)
	UNIFORM_HZMSHADOWKERNEL,  // (PCF radius uv c0, c1, c2, debug mode) (stable mask)
	UNIFORM_HZMSHADOWBIAS,    // (bias c0, c1, c2, W bias) in depth units (stable mask)
	// HZM gl2 MSAA P4c (r_msaaShadowMatch): (on, depth-slope factor, 0, 0) - lightall, zero-neutral
	UNIFORM_HZMSHADOWMATCH,
	// HZM coop [2026-10-05] SKY FOG MODEL (tr_shade.c RB_HZM_SkyFogUniforms): u_HzmSkyFog = (world up in EYE space, fog-layer
	// height H; w 0 = off), u_HzmSkyVp = (viewport x, y, 1/w, 1/h), u_HzmSkyTan = (tan(fovX/2), tan(fovY/2), 1 = sky draw, 0)
	UNIFORM_HZMSKYFOG,
	UNIFORM_HZMSKYVP,
	UNIFORM_HZMSKYTAN,

	UNIFORM_COUNT
} uniform_t;

// shaderProgram_t represents a pair of one
// GLSL vertex and one GLSL fragment shader
typedef struct shaderProgram_s
{
	char            name[MAX_QPATH];

	GLuint          program;
	GLuint          vertexShader;
	GLuint          fragmentShader;
	uint32_t        attribs;	// vertex array attributes

	// uniform parameters
	GLint uniforms[UNIFORM_COUNT];
	short uniformBufferOffsets[UNIFORM_COUNT]; // max 32767/64=511 uniforms
	char  *uniformBuffer;
} shaderProgram_t;

// HZM coop [2026-09-27] realistic lightning: the parsed r_hzmLtState of this frame (tr_hzm_lightning.c)
typedef struct {
	qboolean on;
	float    eSky, eWorld, eHaze, eBolt;
	vec3_t   dir;         // unit, world space, toward the lit cloud region
	int      boltSeed;
	float    boltYaw;     // degrees
	float    boltTopEl;   // degrees
	int      builtSeed;   // the seed tr.hzmLtBoltImage currently holds
} hzmLtState_t;

// trRefdef_t holds everything that comes in refdef_t,
// as well as the locally generated scene information
typedef struct {
	int			x, y, width, height;
	float		fov_x, fov_y;
	vec3_t		vieworg;
	vec3_t		viewaxis[3];		// transformation matrix

	stereoFrame_t	stereoFrame;

	int			time;				// time in milliseconds for shader effects and other time dependent rendering issues
	int			rdflags;			// RDF_NOWORLDMODEL, etc

	// 1 bits will prevent the associated area from rendering at all
	byte		areamask[MAX_MAP_AREA_BYTES];
	qboolean	areamaskModified;	// qtrue if areamask changed since last scene

	double		floatTime;			// tr.refdef.time / 1000.0

	float		blurFactor;

	// text messages for deform text shaders
	char		text[MAX_RENDER_STRINGS][MAX_RENDER_STRING_LENGTH];

	int			num_entities;
	trRefEntity_t	*entities;

	int			num_dlights;
	struct dlight_s	*dlights;

	int			numPolys;
	struct srfPoly_s	*polys;

	int			numDrawSurfs;
	struct drawSurf_s	*drawSurfs;

	unsigned int dlightMask;
	int         num_pshadows;
	struct pshadow_s *pshadows;

	float       sunShadowMvp[4][16];
	float       sunDir[4];
	float       sunCol[4];
	float       sunAmbCol[4];

	float       autoExposureMinMax[2];
	float       toneMinAvgMaxLinear[3];

	
	//
	// OPENMOHAA-specific stuff
	//

    int num_sprites;
	struct refSprite_s *sprites;

	int numTerMarks;
	struct srfMarkFragment_s *terMarks;

	int numSpriteSurfs;
	struct drawSurf_s *spriteSurfs;

    int numStaticModels;
    struct cStaticModelUnpacked_s *staticModels;

    int numStaticModelData;
    unsigned char *staticModelData;

    qboolean sky_portal;
    float sky_alpha;
    vec3_t sky_origin;
    vec3_t sky_axis[3];
    qboolean skybox_farplane;
    qboolean render_terrain;

    // HZM gl2 [2026-09-26] Phase S2: this scene's lamp flares (tr_hzm_spot_rb.c R_HZM_SpotFinishScene)
    int numHzmFlares;
    struct hzmFlare_s *hzmFlares;

    // HZM gl2 stable sun shadows + B0 (plan P3, tr_hzm_sunstable.c). hzmSunActive is cleared by RE_BeginScene for
    // EVERY scene and set only by R_SunStable_Frame, so HUD/menu scenes and the legacy path never see the rest.
    int     hzmSunActive;
    vec4_t  hzmSunSplits;       // R0, R1, R2, blend band
    vec4_t  hzmSunKernel;       // PCF radius (uv) for cascades 0..2, w = debug mode (5 = V_world)
    vec4_t  hzmSunBias;         // compare bias (depth units) for cascades 0..2, w = W bias
    vec4_t  hzmSunMaskOnly;     // lightall: on, normal offset (u), W bias, half a W texel (uv)
    float   hzmSunDepthRange[3];
} trRefdef_t;


//=================================================================================

// max surfaces per-skin
// This is an arbitry limit. Vanilla Q3 only supported 32 surfaces in skins but failed to
// enforce the maximum limit when reading skin files. It was possile to use more than 32
// surfaces which accessed out of bounds memory past end of skin->surfaces hunk block.
#define MAX_SKIN_SURFACES	256

// skins allow models to be retextured without modifying the model file
typedef struct {
	char		name[MAX_QPATH];
	shader_t	*shader;
} skinSurface_t;

typedef struct skin_s {
	char		name[MAX_QPATH];		// game path, including extension
	int			numSurfaces;
	skinSurface_t	*surfaces;			// dynamically allocated array of surfaces
} skin_t;


typedef struct {
	int			originalBrushNumber;
	vec3_t		bounds[2];

	unsigned	colorInt;				// in packed byte format
	float		tcScale;				// texture coordinate vector scales
	fogParms_t	parms;

	// for clipping distance in fog when outside
	qboolean	hasSurface;
	float		surface[4];
} fog_t;

//
// OPENMOHAA-specific stuff
// 
//=========================
typedef struct depthfog_s {
	float len;
	float oolen;
	int enabled;
	int extrafrustums;
} depthfog_t;

//=========================

typedef enum {
	VPF_NONE            = 0x00,
	VPF_NOVIEWMODEL     = 0x01,
	VPF_SHADOWMAP       = 0x02,
	VPF_DEPTHSHADOW     = 0x04,
	VPF_DEPTHCLAMP      = 0x08,
	VPF_ORTHOGRAPHIC    = 0x10,
	VPF_USESUNLIGHT     = 0x20,
	VPF_FARPLANEFRUSTUM = 0x40,
	VPF_NOCUBEMAPS      = 0x80,
	// HZM gl2 DYNAMIC-LIGHT CAST SHADOWS (r_hzmDlightShadows): marks a projected-shadow
	// (pshadow) depth view whose light is a scene DLIGHT rather than the lightgrid. Set
	// ONLY by R_RenderDlightShadowMaps, i.e. only while that master cvar is 1, so with
	// the feature off no view ever carries this bit and every test on it is dead.
	// Needed because a pshadow view and a sun cascade are both VPF_DEPTHSHADOW with
	// shadowCascade 0 vs >0, and the caster-admission rule differs between them.
	VPF_PSHADOW         = 0x100,
	// HZM gl2 stable sun shadows (plan P3, tr_hzm_sunstable.c). Set only while r_shadowStable resolves on, so with
	// the switch at 0 no view carries either bit and every test on them is dead.
	VPF_SUNWORLD        = 0x200,    // the W bake (static world cascade, own arrays)
	VPF_SUNSTABLE       = 0x400     // a per-frame radial entity-only cascade
} viewParmFlags_t;

typedef struct {
	orientationr_t	or;
	orientationr_t	world;
	vec3_t		pvsOrigin;			// may be different than or.origin for portals
	qboolean	isPortal;			// true if this view is through a portal
	qboolean	isMirror;			// the portal is a mirror, invert the face culling
	viewParmFlags_t flags;
	int			frameSceneNum;		// copied from tr.frameSceneNum
	int			frameCount;			// copied from tr.frameCount
	cplane_t	portalPlane;		// clip anything behind this if mirroring
	int			viewportX, viewportY, viewportWidth, viewportHeight;
	FBO_t		*targetFbo;
	int         targetFboLayer;
	int         targetFboCubemapIndex;
	float		fovX, fovY;
	float		projectionMatrix[16];
	float		weaponProjectionMatrix[16];	// HZM gl2 re-port Fix 3: un-zoomed ADS view-weapon projection
	qboolean	weaponFovActive;
	cplane_t	frustum[5];
	vec3_t		visBounds[2];
	float		zFar;
	float       zNear;
	stereoFrame_t	stereoFrame;

	// HZM gl2 real character shadows (r_charShadows): which sun cascade this view is.
	// 0 = not a sun-cascade shadow view, 1..4 = R_RenderSunShadowMaps level+1. Every
	// other view setup does Com_Memset(&parms,0,...) so 0 is the reliable default.
	int			shadowCascade;

	// HZM gl2 stable sun shadows (P3): the W bake's face mode (1 = back faces, 2 = + sun-facing pushed back) and its
	// push-back glPolygonOffset (factor, units). 0 in every other view.
	int			hzmWFaces;
	float		hzmWPush[2];

	//
	// OPENMOHAA-specific stuff
	//

    qboolean	isPortalSky;		// since 2.0 whether or not this view is a portal sky
    depthfog_t	fog;
    float		farplane_distance;
    float		farplane_bias; // added in 2.0
    float		farplane_color[3];
    qboolean	farplane_cull;
    qboolean	renderTerrain; // added in 2.0
} viewParms_t;


/*
==============================================================================

SURFACES

==============================================================================
*/
typedef byte color4ub_t[4];

// any changes in surfaceType must be mirrored in rb_surfaceTable[]
typedef enum {
	SF_BAD,
	SF_SKIP,				// ignore
	SF_FACE,
	SF_GRID,
	SF_TRIANGLES,
	SF_POLY,
	SF_MDV,
	SF_MDR,
	SF_IQM,
	SF_FLARE,
	SF_ENTITY,				// beams, rails, lightning, etc that can be determined by entity
	SF_VAO_MDVMESH,
	SF_VAO_IQM,

	//
	// OPENMOHAA-specific stuff
	//

    SF_MARK_FRAG,
    SF_TIKI_SKEL,
    SF_TIKI_STATIC,
    SF_SWIPE,
    SF_SPRITE,
    SF_TERRAIN_PATCH,
	SF_NUM_SURFACE_TYPES,
	SF_MAX = 0x7fffffff			// ensures that sizeof( surfaceType_t ) == sizeof( int )
} surfaceType_t;

typedef struct drawSurf_s {
	unsigned int		sort;			// bit combination for fast compares
	int                 cubemapIndex;
	surfaceType_t		*surface;		// any of surface*_t
} drawSurf_t;

#define	MAX_FACE_POINTS		64

#define	MAX_PATCH_SIZE		32			// max dimensions of a patch mesh in map file
#define	MAX_GRID_SIZE		65			// max dimensions of a grid mesh in memory

// when cgame directly specifies a polygon, it becomes a srfPoly_t
// as soon as it is called
typedef struct srfPoly_s {
	surfaceType_t	surfaceType;
	qhandle_t		hShader;
	int				fogIndex;
	int				numVerts;
	polyVert_t		*verts;

	//
	// OPENMOHAA-specific stuff
	//

    int renderfx;
} srfPoly_t;


typedef struct srfFlare_s {
	surfaceType_t	surfaceType;
	vec3_t			origin;
	vec3_t			normal;
	vec3_t			color;
} srfFlare_t;

typedef struct
{
	vec3_t          xyz;
	vec2_t          st;
	vec2_t          lightmap;
	int16_t         normal[4];
	int16_t         tangent[4];
	int16_t         lightdir[4];
	uint16_t        color[4];

#if DEBUG_OPTIMIZEVERTICES
	unsigned int    id;
#endif
} srfVert_t;

#define srfVert_t_cleared(x) srfVert_t (x) = {{0, 0, 0}, {0, 0}, {0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}}

// srfBspSurface_t covers SF_GRID, SF_TRIANGLES, and SF_POLY
typedef struct srfBspSurface_s
{
	surfaceType_t   surfaceType;

	// dynamic lighting information
	int				dlightBits;
	int             pshadowBits;

	// culling information
	vec3_t			cullBounds[2];
	vec3_t			cullOrigin;
	float			cullRadius;
	cplane_t        cullPlane;

	// indexes
	int             numIndexes;
	glIndex_t      *indexes;

	// vertexes
	int             numVerts;
	srfVert_t      *verts;
	
	// SF_GRID specific variables after here

	// lod information, which may be different
	// than the culling information to allow for
	// groups of curves that LOD as a unit
	vec3_t			lodOrigin;
	float			lodRadius;
	int				lodFixed;
	int				lodStitched;

	// vertexes
	int				width, height;
	float			*widthLodError;
	float			*heightLodError;
} srfBspSurface_t;

typedef struct {
	vec3_t translate;
	quat_t rotate;
	vec3_t scale;
} iqmTransform_t;

// inter-quake-model
typedef struct {
	int		num_vertexes;
	int		num_triangles;
	int		num_frames;
	int		num_surfaces;
	int		num_joints;
	int		num_poses;
	struct srfIQModel_s	*surfaces;

	int		*triangles;

	// vertex arrays
	float		*positions;
	float		*texcoords;
	float		*normals;
	float		*tangents;
	byte		*colors;
	int		*influences; // [num_vertexes] indexes into influenceBlendVertexes

	// unique list of vertex blend indexes/weights for faster CPU vertex skinning
	byte		*influenceBlendIndexes; // [num_influences]
	union {
		float	*f;
		byte	*b;
	} influenceBlendWeights; // [num_influences]

	// depending upon the exporter, blend indices and weights might be int/float
	// as opposed to the recommended byte/byte, for example Noesis exports
	// int/float whereas the official IQM tool exports byte/byte
	int		blendWeightsType; // IQM_UBYTE or IQM_FLOAT

	char		*jointNames;
	int		*jointParents;
	float		*bindJoints; // [num_joints * 12]
	float		*invBindJoints; // [num_joints * 12]
	iqmTransform_t	*poses; // [num_frames * num_poses]
	float		*bounds;

	int		numVaoSurfaces;
	struct srfVaoIQModel_s	*vaoSurfaces;
} iqmData_t;

// inter-quake-model surface
typedef struct srfIQModel_s {
	surfaceType_t	surfaceType;
	char		name[MAX_QPATH];
	shader_t	*shader;
	iqmData_t	*data;
	int		first_vertex, num_vertexes;
	int		first_triangle, num_triangles;
	int		first_influence, num_influences;
} srfIQModel_t;

typedef struct srfVaoIQModel_s
{
	surfaceType_t   surfaceType;

	iqmData_t *iqmData;
	struct srfIQModel_s *iqmSurface;

	// backEnd stats
	int             numIndexes;
	int             numVerts;

	// static render data
	vao_t          *vao;
} srfVaoIQModel_t;

typedef struct srfVaoMdvMesh_s
{
	surfaceType_t   surfaceType;

	struct mdvModel_s *mdvModel;
	struct mdvSurface_s *mdvSurface;

	// backEnd stats
	int             numIndexes;
	int             numVerts;

	// static render data
	vao_t          *vao;
} srfVaoMdvMesh_t;

extern	void (*rb_surfaceTable[SF_NUM_SURFACE_TYPES])(void *);

//
// OPENMOHAA-specific stuff
//=========================

typedef struct {
    struct mnode_s* cntNode;
    struct msurface_s* skySurfs[32];
    int numSurfs;
    vec3_t offset;
    vec3_t mins;
    vec3_t maxs;
} portalsky_t;

typedef struct {
    vec3_t transformed;
    int index;
} sphere_dlight_t;

typedef enum {
    LIGHT_POINT,
    LIGHT_DIRECTIONAL,
    LIGHT_SPOT,
    LIGHT_SPOT_FAST
} lighttype_t;

typedef struct reallightinfo_s {
    vec3_t color;
    lighttype_t eType;
    float fIntensity;
    float fDist;
    float fSpotSlope;
    float fSpotConst;
    float fSpotScale;
    vec3_t vOrigin;
    vec3_t vDirection;
} reallightinfo_t;

typedef float cube_entry_t[3][4];

typedef struct {
    vec3_t origin;
    vec3_t worldOrigin;
    vec3_t traceOrigin;
    float radius;
    struct mnode_s* leaves[8];
    void(*TessFunction) (unsigned char* dstColors);
    union {
        unsigned char level[4];
        int value;
    } ambient;
    int numRealLights;
    reallightinfo_t light[MAX_REAL_LIGHTS];
    int bUsesCubeMap;
    float cubemap[24][3][4];
} sphereor_t;

// HZM gl2 CHARACTER LIGHTING (r_charLighting). Per-DRAW-BATCH restatement of the
// MOHAA entity light sphere as the two things tr.lightallShader's USE_LIGHT_VECTOR
// permutation wants: one dominant light direction, and an ambient/directed split.
//
// RB_Light_Real computes, per vertex:   colour = ambient + SUM(light_j * max(dot(L_j,N),0))
// This restates it as:                  colour = flat * (ambientFrac + directedFrac * dot(L,N))
// with flat = ambient + SUM(light_j) and the fractions taken per channel, so the
// GPU reproduces the same value per PIXEL instead of Gouraud-interpolating it, and
// the fragment normal (not the vertex normal) drives the falloff.
//
// active is false for every batch that is not a character skin under r_charLighting;
// when it is false nothing in tr_shade.c reads any other member.
typedef struct {
    qboolean active;          // this batch is a bIsCharacter skin, char lighting is live
    vec3_t   lightDirWorld;   // unit vector towards the dominant light, WORLD space
    vec3_t   ambientFrac;     // per-channel ambient share of flatColor   (0..1)
    vec3_t   directedFrac;    // per-channel directed share of flatColor  (0..1)
    byte     flatColor[4];    // ambient + directed, clamped: what tess.color is filled with
} charLighting_t;

typedef struct spherel_s {
    vec3_t origin;
	vec3_t color;
    float intensity;
    struct mnode_s* leaf;
    int needs_trace;
    int spot_light;
    float spot_radiusbydistance;
	vec3_t spot_dir;
    int reference_count;
} spherel_t;

typedef struct suninfo_s {
	vec3_t color;
	vec3_t direction;
	vec3_t flaredirection;
	char szFlareName[64];
	qboolean exists;
} suninfo_t;

typedef union varnodeUnpacked_u {
    float fVariance;
    int flags;
} varnodeUnpacked_t;

typedef unsigned short terraInt;

typedef struct terrainVert_s {
    vec3_t xyz;
    vec2_t texCoords[2];
    float fVariance;
    float fHgtAvg;
    float fHgtAdd;
    unsigned int uiDistRecalc;
    terraInt nRef;
    terraInt iVertArray;
    byte* pHgt;
    terraInt iNext;
    terraInt iPrev;
    // HZM gl2 [bug-3007] light-direction GEOMORPH (tr_surface.c R_HZM_TerrainVertLightDir): heightmap indices
    // (gy * 9 + gx, 0xFF = none) of the two hypotenuse ends this vertex was split from, and the geomorph factor
    // R_CalcVertMorphHeight gave its height (0 = at the ends' average, 1 = its own value).
    byte hzmLPar[2];
    float fHzmMorph;
} terrainVert_t;

typedef struct terraTri_s {
    unsigned short iPt[3];
    terraInt nSplit;
    unsigned int uiDistRecalc;
    struct cTerraPatchUnpacked_s* patch;
    varnodeUnpacked_t* varnode;
    terraInt index;
    byte lod;
    byte byConstChecks;
    terraInt iLeft;
    terraInt iRight;
    terraInt iBase;
    terraInt iLeftChild;
    terraInt iRightChild;
    terraInt iParent;
    terraInt iPrev;
    terraInt iNext;
} terraTri_t;

typedef struct srfTerrain_s {
    surfaceType_t surfaceType;
    terraInt iVertHead;
    terraInt iTriHead;
    terraInt iTriTail;
    terraInt iMergeHead;
    int nVerts;
    int nTris;
    int lmapSize;
    int dlightBits[2];
    float lmapStep;
    int dlightMap[2];
    byte* lmData;
    float lmapX;
    float lmapY;
} srfTerrain_t;

typedef struct cTerraPatchUnpacked_s {
    srfTerrain_t drawinfo;
    int viewCount;
    int visCountCheck;
    int visCountDraw;
    int frameCount;
    unsigned int uiDistRecalc;
    float s;
    float t;
    vec2_t texCoord[2][2];
    float x0;
    float y0;
    float z0;
    float zmax;
    shader_t* shader;
    short int iNorth;
    short int iEast;
    short int iSouth;
    short int iWest;
    struct cTerraPatchUnpacked_s* pNextActive;
    varnodeUnpacked_t varTree[2][63];
    unsigned char heightmap[81];
    byte flags;
    byte byDirty;
} cTerraPatchUnpacked_t;

typedef struct srfStaticModel_s {
    surfaceType_t surfaceType;
    struct cStaticModelUnpacked_s* parent;
} srfStaticModel_t;

typedef struct srfMarkFragment_s {
	surfaceType_t surfaceType;
	int iIndex;
	int numVerts;
	polyVert_t* verts;
} srfMarkFragment_t;

typedef struct cStaticModelUnpacked_s {
    qboolean useSpecialLighting;
    qboolean bLightGridCalculated;
    qboolean bRendered;
    char model[128];
    vec3_t origin;
    vec3_t angles;
    vec3_t axis[3];
    float scale;
    int firstVertexData;
    int numVertexData;
    int visCount;
    dtiki_t* tiki;
    sphere_dlight_t dlights[MAX_DLIGHTS];
    int numdlights;
    float radius;
    float cull_radius;
    int iGridLighting;
    float lodpercentage[2];
    qboolean hzmFoliage;   // HZM: its shadow is baked by maps/<map>.hzmlm - never cast per frame
} cStaticModelUnpacked_t;

#define HZM_FOLIAGE_MAX_BAKED 4096   // m6l1a bakes 2603
#define HZM_FOLIAGE_MAX_ENTS 256     // m3l3 bakes 50 build-mode bushes

typedef struct refSprite_s {
    surfaceType_t surftype;
    int hModel;
    int shaderNum;
    float origin[3];
    float scale;
    float axis[3][3];
    byte shaderRGBA[4];
    int renderfx;
    float shaderTime;
} refSprite_t;

//=========================

/*
==============================================================================

SHADOWS

==============================================================================
*/

typedef struct pshadow_s
{
	float sort;
	
	int    numEntities;
	int    entityNums[8];
	vec3_t entityOrigins[8];
	float  entityRadiuses[8];

	float viewRadius;
	vec3_t viewOrigin;

	vec3_t lightViewAxis[3];
	vec3_t lightOrigin;
	float  lightRadius;
	cplane_t cullPlane;

	// HZM gl2 [2026-09-26] S4 spot shadows (R_FinalizeDlightPshadow): the cone mask for pshadow_fp. ALL ZERO for every
	// other pshadow - both builders memset the struct - which is the inert value (no mask, the receive pass unchanged).
	vec4_t hzmSpotApex;		// the spot's apex (world), its light radius
	vec4_t hzmSpotCone;		// cone axis * k (world), cosOuter - R_HZM_SpotUniformVec
} pshadow_t;


/*
==============================================================================

BRUSH MODELS

==============================================================================
*/


//
// in memory representation
//

#define	SIDE_FRONT	0
#define	SIDE_BACK	1
#define	SIDE_ON		2

#define CULLINFO_NONE   0
#define CULLINFO_BOX    1
#define CULLINFO_SPHERE 2
#define CULLINFO_PLANE  4

typedef struct cullinfo_s {
	int             type;
	vec3_t          bounds[2];
	vec3_t			localOrigin;
	float			radius;
	cplane_t        plane;
} cullinfo_t;

typedef struct msurface_s {
	//int					viewCount;		// if == tr.viewCount, already added
	struct shader_s		*shader;
	int					fogIndex;
	int                 cubemapIndex;
	cullinfo_t          cullinfo;

	surfaceType_t		*data;			// any of srf*_t
} msurface_t;


#define	CONTENTS_NODE		-1
typedef struct mnode_s {
	// common with leaf and node
	int			contents;		// -1 for nodes, to differentiate from leafs
	int             visCounts[MAX_VISCOUNTS];	// node needs to be traversed if current
	vec3_t		mins, maxs;		// for bounding box culling
	struct mnode_s	*parent;

	// node specific
	cplane_t	*plane;
	struct mnode_s	*children[2];	

	// leaf specific
	int			cluster;
	int			area;

	int         firstmarksurface;
	int			nummarksurfaces;

	//
	// OPENMOHAA-specific stuff
	//

    spherel_t**	lights;
    int			numlights;

    int			firstTerraPatch;
    int			numTerraPatches;
    int			firstStaticModel;
    int			numStaticModels;
    void**		pFirstMarkFragment;
    int			iNumMarkFragment;
} mnode_t;

typedef struct {
	vec3_t		bounds[2];		// for culling
	int	        firstSurface;
	int			numSurfaces;

	//
	// OPENMOHAA-specific stuff
	//

    void** pFirstMarkFragment;
    int iNumMarkFragment;
    int frameCount;
    qboolean hasLightmap;
} bmodel_t;

typedef struct {
	char		name[MAX_QPATH];		// ie: maps/tim_dm2.bsp
	char		baseName[MAX_QPATH];	// ie: tim_dm2

	int			dataSize;

	int			numShaders;
	dshader_t	*shaders;

	int			numBModels;
	bmodel_t	*bmodels;

	int			numplanes;
	cplane_t	*planes;

	int			numnodes;		// includes leafs
	int			numDecisionNodes;
	mnode_t		*nodes;

	int         numWorldSurfaces;

	int			numsurfaces;
	msurface_t	*surfaces;
	int         *surfacesViewCount;
	int         *surfacesDlightBits;
	int			*surfacesPshadowBits;

	int			nummarksurfaces;
	int         *marksurfaces;

	int			numfogs;
	fog_t		*fogs;

	vec3_t		lightGridOrigin;
	vec3_t		lightGridSize;
	vec3_t		lightGridInverseSize;
	int			lightGridBounds[3];
	byte		*lightGridData;
	uint16_t	*lightGrid16;


	int			numClusters;
	int			clusterBytes;
	const byte	*vis;			// may be passed in by CM_LoadMap to save space

	char		*entityString;
	char		*entityParsePoint;
	
	//
	// OPENMOHAA-specific stuff
	//

	unsigned short* lightGridOffsets;
	byte        lightGridPalette[768];

    int numTerraPatches;
    cTerraPatchUnpacked_t* terraPatches;
    cTerraPatchUnpacked_t* activeTerraPatches;

    int numVisTerraPatches;
    cTerraPatchUnpacked_t** visTerraPatches;

    int numStaticModelData;
    byte* staticModelData;

    int numStaticModels;
    cStaticModelUnpacked_t* staticModels;

    int numVisStaticModels;
    cStaticModelUnpacked_t** visStaticModels;

    // HZM gl2 [bug-3007] terrain light direction per HEIGHTMAP VERTEX, packed exactly like tess.lightdir, indexed
    // [patch index * 81 + gy * 9 + gx]. Hunk-allocated at load on every terrain map, filled by
    // R_HZM_TerrainLightGridBuild (tr_surface.c): at load when r_hzmTerrainLightGrid resolves on, else on first use.
    int16_t (*hzmTerraLightDir)[4];
    qboolean hzmTerraLightDirBuilt;
} world_t;

//
// OPENMOHAA-specific stuff
//=========================
typedef struct {
    float width;
    float height;
    float origin_x;
    float origin_y;
    float scale;
    shader_t* shader;
} sprite_t;
//=========================

/*
==============================================================================
MDV MODELS - meta format for vertex animation models like .md2, .md3, .mdc
==============================================================================
*/
typedef struct
{
	float           bounds[2][3];
	float           localOrigin[3];
	float           radius;
} mdvFrame_t;

typedef struct
{
	float           origin[3];
	float           axis[3][3];
} mdvTag_t;

typedef struct
{
	char            name[MAX_QPATH];	// tag name
} mdvTagName_t;

typedef struct
{
	vec3_t          xyz;
	int16_t         normal[4];
	int16_t         tangent[4];
} mdvVertex_t;

typedef struct
{
	float           st[2];
} mdvSt_t;

typedef struct mdvSurface_s
{
	surfaceType_t   surfaceType;

	char            name[MAX_QPATH];	// polyset name

	int             numShaderIndexes;
	int				*shaderIndexes;

	int             numVerts;
	mdvVertex_t    *verts;
	mdvSt_t        *st;

	int             numIndexes;
	glIndex_t      *indexes;

	struct mdvModel_s *model;
} mdvSurface_t;

typedef struct mdvModel_s
{
	int             numFrames;
	mdvFrame_t     *frames;

	int             numTags;
	mdvTag_t       *tags;
	mdvTagName_t   *tagNames;

	int             numSurfaces;
	mdvSurface_t   *surfaces;

	int             numVaoSurfaces;
	srfVaoMdvMesh_t  *vaoSurfaces;

	int             numSkins;
} mdvModel_t;


//======================================================================

typedef enum {
	MOD_BAD,
	MOD_BRUSH,
	MOD_MESH,
	MOD_MDR,
	MOD_IQM,

	//
	// OPENMOHAA-specific stuff
	//
    MOD_TIKI,
    MOD_SPRITE
} modtype_t;

typedef struct model_s {
	char		name[MAX_QPATH];
	modtype_t	type;
	int			index;		// model = tr.models[model->index]

	int			dataSize;	// just for listing purposes
	bmodel_t	*bmodel;		// only if type == MOD_BRUSH
	mdvModel_t	*mdv[MD3_MAX_LODS];	// only if type == MOD_MESH
	void	*modelData;			// only if type == (MOD_MDR | MOD_IQM)

	int			 numLods;

	// OPENMOHAA-specific stuff
    union {
        dtiki_t* tiki;
        sprite_t* sprite;
    } d;
    qboolean serveronly;
} model_t;


#define	MAX_MOD_KNOWN	2048

void		R_ModelInit (void);
model_t		*R_GetModelByHandle( qhandle_t hModel );
int			R_LerpTag( orientation_t *tag, qhandle_t handle, int startFrame, int endFrame, 
					 float frac, const char *tagName );
void		R_ModelBounds( qhandle_t handle, vec3_t mins, vec3_t maxs );

void		R_Modellist_f (void);

//====================================================

// HZM gl2 [2026-09-26, bug-2997] 2048 -> 4096 (gl1 parity, renderergl1/tr_local.h). e1l4 alone registers 1,923
// images and the shipped r_hzmGenNormals 1 adds generated normal maps on top, so the map ERR_DROPped to the menu
// ("R_CreateImage: MAX_DRAWIMAGES hit"). Only pointer arrays are sized by it (tr.images, hzm_genNormalImages in
// tr_image.c); no image index is packed into a sort key or any other bit field. R_CreateImage warns at 90%.
#define	MAX_DRAWIMAGES			4096
#define	MAX_SKINS				1024


// HZM gl2 (shadow plan P1a, SE#13/V3-C6.4): 0x10000 -> 0x20000. The list is shared by EVERY view of a frame
// (dlight shadows, 3 cascades, the far cascade, then the main view), and with r_shadowHarden 1 the views before
// the main view may fill only HALF of it - so at 0x20000 that half is exactly the whole of today's list, and no
// view can lose a surface that fits today. PROVISIONAL: the plan sizes this as the P0 per-frame high-water after
// a plain map change + 25%, rounded up to a power of two; confirm with ^~^~^ SHADOWBUDGET (frame_hw ds < 50%).
// Derived, grep the VALUE too (T4): DRAWSURF_MASK, backEndData_t::drawSurfs, R_RadixSort's static scratch.
#define	MAX_DRAWSURFS			0x20000
#define	DRAWSURF_MASK			(MAX_DRAWSURFS-1)
#if (MAX_DRAWSURFS & (MAX_DRAWSURFS - 1)) != 0
	#error "MAX_DRAWSURFS must be a power of two: R_AddDrawSurf wraps with DRAWSURF_MASK when r_shadowHarden is 0"
#endif

/*

the drawsurf sort data is packed into a single 32 bit value so it can be
compared quickly during the qsorting process

the bits are allocated as follows:

0 - 1	: dlightmap index
//2		: used to be clipped flag REMOVED - 03.21.00 rad
2 - 6	: fog index
11 - 20	: entity index
21 - 31	: sorted shader index

	TTimo - 1.32
0-1   : dlightmap index
2-6   : fog index
7-16  : entity index
17-30 : sorted shader index

    SmileTheory - for pshadows
17-31 : sorted shader index
7-16  : entity index
2-6   : fog index
1     : pshadow flag
0     : dlight flag
*/
/*
#define	QSORT_FOGNUM_SHIFT	2
#define	QSORT_REFENTITYNUM_SHIFT	7
#define	QSORT_SHADERNUM_SHIFT	(QSORT_REFENTITYNUM_SHIFT+REFENTITYNUM_BITS)
*/
#define	QSORT_FOGNUM_SHIFT	2
#define	QSORT_REFENTITYNUM_SHIFT	4
#define	QSORT_STATICMODEL_SHIFT	(QSORT_REFENTITYNUM_SHIFT+REFENTITYNUM_BITS)
#define	QSORT_SHADERNUM_SHIFT	(QSORT_STATICMODEL_SHIFT+1)
#if (QSORT_SHADERNUM_SHIFT+SHADERNUM_BITS) > 32
	#error "Need to update sorting, too many bits."
#endif
#define QSORT_PSHADOW_SHIFT     1

// HZM (engine-limits audit) - the real, usable shader ceiling for gl2.
//
// Unlike renderergl1 (shift 21, only 11 bits left => a hard 2048 ceiling under a MAX_SHADERS of
// 16384) this layout does fit: shift 17 + SHADERNUM_BITS 14 = 31 bits of an unsigned 32-bit
// key, so all 16384 shaders are encodable today. MAX_SORTED_SHADERS makes that relationship
// explicit instead of coincidental, and GeneratePermanentShader warns loudly if a future
// layout change ever makes tr.numShaders outrun what the key can carry.
#define	QSORT_SHADERNUM_BITS	(32 - QSORT_SHADERNUM_SHIFT)
#define	MAX_SORTED_SHADERS		(MAX_SHADERS < (1 << QSORT_SHADERNUM_BITS) ? MAX_SHADERS : (1 << QSORT_SHADERNUM_BITS))

// The fields must not overlap. entityNum is decoded with REFENTITYNUM_MASK
// (REFENTITYNUM_BITS wide) and dlight/pshadow occupy bits 0 and 1.
#if QSORT_REFENTITYNUM_SHIFT < 2
	#error "QSORT_REFENTITYNUM_SHIFT overlaps the dlight/pshadow flags at bits 0-1"
#endif
#if (QSORT_STATICMODEL_SHIFT + 1) > QSORT_SHADERNUM_SHIFT
	#error "the staticmodel flag overlaps the shader field"
#endif

extern	int			gl_filter_min, gl_filter_max;

/*
** performanceCounters_t
*/
typedef struct {
	int		c_sphere_cull_patch_in, c_sphere_cull_patch_clip, c_sphere_cull_patch_out;
	int		c_box_cull_patch_in, c_box_cull_patch_clip, c_box_cull_patch_out;
	int		c_sphere_cull_md3_in, c_sphere_cull_md3_clip, c_sphere_cull_md3_out;
	int		c_box_cull_md3_in, c_box_cull_md3_clip, c_box_cull_md3_out;

	int		c_leafs;
	int		c_dlightSurfaces;
	int		c_dlightSurfacesCulled;
} frontEndCounters_t;

#define	FOG_TABLE_SIZE		256
#define FUNCTABLE_SIZE		1024
#define FUNCTABLE_SIZE2		10
#define FUNCTABLE_MASK		(FUNCTABLE_SIZE-1)


// the renderer front end should never modify glstate_t
typedef struct {
	qboolean	finishCalled;
	int			texEnv[2];
	int			faceCulling;
	int         faceCullFront;
	uint32_t    glStateBits;
	uint32_t    storedGlState;
	float           vertexAttribsInterpolation;
	qboolean        vertexAnimation;
	int             boneAnimation; // number of bones
	mat4_t          boneMatrix[IQM_MAX_JOINTS];
	uint32_t        vertexAttribsEnabled;  // global if no VAOs, tess only otherwise
	FBO_t          *currentFBO;
	vao_t          *currentVao;
	mat4_t        modelview;
	mat4_t        projection;
	mat4_t		modelviewProjection;
} glstate_t;

typedef enum {
	MI_NONE,
	MI_NVX,
	MI_ATI
} memInfo_t;

typedef enum {
	TCR_NONE = 0x0000,
	TCR_RGTC = 0x0001,
	TCR_BPTC = 0x0002,
} textureCompressionRef_t;

// We can't change glConfig_t without breaking DLL/vms compatibility, so
// store extensions we have here.
typedef struct {
	qboolean    intelGraphics;

	qboolean	occlusionQuery;
	GLenum		occlusionQueryTarget;

	int glslMajorVersion;
	int glslMinorVersion;
	int glslMaxAnimatedBones;

	memInfo_t   memInfo;

	qboolean framebufferObject;
	int maxRenderbufferSize;
	int maxColorAttachments;

	qboolean textureFloat;
	textureCompressionRef_t textureCompression;
	qboolean swizzleNormalmap;
	
	qboolean framebufferMultisample;
	qboolean framebufferBlit;
	qboolean textureMultisample;   // HZM MSAA P2a: GL 3.2 / ARB_texture_multisample, qglTexImage2DMultisample loaded

	qboolean depthClamp;
	qboolean seamlessCubeMap;

	qboolean vertexArrayObject;
	qboolean directStateAccess;

	int maxVertexAttribs;
	qboolean gpuVertexAnimation;

	GLenum vaoCacheGlIndexType; // GL_UNSIGNED_INT or GL_UNSIGNED_SHORT
	size_t vaoCacheGlIndexSize; // must be <= sizeof( vaoCacheGlIndex_t )

	// OpenGL ES extensions
	qboolean readDepth;
	qboolean readStencil;
	qboolean shadowSamplers;
	qboolean standardDerivatives;
} glRefConfig_t;


typedef struct {
	int		c_surfaces, c_shaders, c_vertexes, c_indexes, c_totalIndexes;
	int     c_surfBatches;
	float	c_overDraw;
	
	int		c_vaoBinds;
	int		c_vaoVertexes;
	int		c_vaoIndexes;

	int     c_staticVaoDraws;
	int     c_dynamicVaoDraws;

	int		c_dlightVertexes;
	int		c_dlightIndexes;

	int		c_flareAdds;
	int		c_flareTests;
	int		c_flareRenders;

	int     c_glslShaderBinds;
	int     c_genericDraws;
	int     c_lightallDraws;
	int     c_fogDraws;
	int     c_dlightDraws;

	int		msec;			// total msec for backend run

	//
	// OPENMOHAA-specific stuff
	//

	int		c_characterlights;
} backEndCounters_t;

// all state modified by the back end is separated
// from the front end state
typedef struct {
	trRefdef_t	refdef;
	viewParms_t	viewParms;
	orientationr_t	or;
	backEndCounters_t	pc;
	qboolean	isHyperspace;
	trRefEntity_t	*currentEntity;
	qboolean	skyRenderedThisView;	// flag for drawing sun

	qboolean	projection2D;	// if qtrue, drawstretchpic doesn't need to change modes
	byte		color2D[4];
	qboolean	vertexes2D;		// shader needs to be finished
	trRefEntity_t	entity2D;	// currentEntity will point at this when doing 2D rendering

	FBO_t *last2DFBO;
	qboolean    colorMask[4];
	qboolean    depthFill;

	// HZM gl2 SSAO BLACK-SCREEN FIX (bug-1177 follow-up): was tr.screenSsaoFbo actually PRODUCED
	// for this view, this frame? The AO composite is a multiply (dst *= src), and the AO image is
	// created with pic=NULL, i.e. driver zero-fill = pure BLACK. Compositing a never-written AO
	// buffer therefore multiplies the whole frame by 0 and presents a black screen with no crash
	// and no GL error. Cleared at the top of every non-shadow RB_DrawSurfs and set only by the
	// pass that writes screenSsaoFbo, so the composite can never run against stale or virgin AO
	// (covers the first frame after vid_restart and any RDF_NOWORLDMODEL scene too).
	qboolean    ssaoValid;

	// HZM gl2 soft particles (r_softParticles). softDepthValid mirrors ssaoValid: cleared at the
	// top of every non-shadow RB_DrawSurfs and set only once the sprite-list depth snapshot has
	// been taken for this view, so a sprite fragment never samples a stale or virgin depth copy.
	// inSpriteList is raised only around RB_RenderSpriteSurfList so the per-fragment fade is
	// gated to emitter sprites; spriteDepthHack tracks the current sprite's RF_DEPTHHACK (muzzle
	// flashes) so they are excluded - their compressed depth range would compare wrong against
	// the snapshot.
	qboolean    softDepthValid;
	qboolean    inSpriteList;
	qboolean    spriteDepthHack;

	// HZM gl2 MSAA (plan P2b, tr_msaa.c). msaaLinearValid: this post chain's MRT colour resolve wrote
	// tr.sceneLinearImage, so bloom / DoF / the exposure measure read it (R_MsaaLinearSource); cleared at the head and
	// the end of RB_PostProcess. msaaMinMaxFrame: tr.frameCount + 1 of the last shader depth resolve, so the (min, max)
	// readers (soft particles, underwater, fog) never bind a stale tr.msaaDepthMinMaxImage.
	qboolean    msaaLinearValid;
	int         msaaMinMaxFrame;

	//
	// OPENMOHAA-specific stuff
	//
    sphereor_t spheres[MAX_SPHERE_LIGHTS];
    unsigned short numSpheresUsed;
    // HZM gl2 (bug-gl2-sphereslot-alias): monotonic id of the current draw-surf list,
    // bumped wherever numSpheresUsed is reset. Never reset itself, so it cannot alias
    // across frames either.
    unsigned int sphereListId;
    sphereor_t* currentSphere;
    sphereor_t spareSphere;
    sphereor_t hudSphere;
    // HZM gl2 character lighting (r_charLighting). Recomputed per draw batch in
    // RB_RenderDrawSurfList, immediately after the light sphere it is derived from,
    // and cleared for every non-character batch. .active is qfalse whenever
    // r_charLighting is 0, so every consumer is inert by default.
    charLighting_t charLight;
    cStaticModelUnpacked_t* currentStaticModel;
    float shaderStartTime;
    int dsStreamVert;
    int hzmWPass;   // HZM gl2 stable shadows: 1/2 = the W bake's first/second (sun-facing) depth pass, 0 otherwise
} backEndState_t;

/*
** trGlobals_t 
**
** Most renderer globals are defined here.
** backend functions should never modify any of these fields,
** but may read fields that aren't dynamically modified
** by the frontend.
*/
typedef struct {
	qboolean				registered;		// cleared at shutdown, set at beginRegistration

	int						visIndex;
	int						visClusters[MAX_VISCOUNTS];
	int						visCounts[MAX_VISCOUNTS];	// incremented every time a new vis cluster is entered

	int						frameCount;		// incremented every frame
	int						sceneCount;		// incremented every scene
	int						viewCount;		// incremented every view (twice a scene if portaled)
											// and every R_MarkFragments call

	int						frameSceneNum;	// zeroed at RE_BeginFrame

	qboolean				worldMapLoaded;
	qboolean				worldDeluxeMapping;
	qboolean				hzmFoliageLmApplied;	// HZM: maps/<map>.hzmlm re-baked this world's foliage shadows (hzm_lmpatch.h)
	int				hzmFoliageBaked[HZM_FOLIAGE_MAX_BAKED];	// static-model lump indices it baked (kept out of the per-frame shadow maps)
	int				hzmFoliageNumBaked;
	float				hzmFoliageBakedEnt[HZM_FOLIAGE_MAX_ENTS][3];	// origins of the scripted (build-mode) foliage entities it baked
	int				hzmFoliageNumEnts;
	vec2_t                  autoExposureMinMax;
	vec3_t                  toneMinAvgMaxLevel;
	world_t					*world;

	const byte				*externalVisData;	// from RE_SetWorldVisData, shared with CM_Load

	image_t					*defaultImage;
	image_t					*scratchImage[32];
	image_t					*fogImage;
	image_t					*dlightImage;	// inverse-quare highlight for projective adding
	image_t					*flareImage;
	image_t					*whiteImage;			// full of 0xff
	image_t					*identityLightImage;	// full of tr.identityLightByte

	image_t                 *shadowCubemaps[MAX_DLIGHTS];
	

	image_t					*renderImage;
	image_t					*sunRaysImage;
	image_t					*renderDepthImage;
	image_t					*pshadowMaps[MAX_DRAWN_PSHADOWS];
	image_t					*screenScratchImage;
	image_t					*textureScratchImage[2];
	image_t                 *quarterImage[2];
	image_t                 *bloomImage[2];   // HZM exposure-aware bloom (r_ppBloomMode 1): RGBA16F, DISPLAY/2
	image_t                 *displayImage;    // HZM render scale: RGBA8 at DISPLAY size, backs renderFbo when scaled
	image_t                 *displayScratchImage; // HZM render scale: RGBA8 at DISPLAY size, RCAS ping target
	image_t                 *sceneColorMSImage;    // HZM MSAA P2a: GL_TEXTURE_2D_MULTISAMPLE scene colour (new path)
	image_t                 *sceneDepthMSImage;    // HZM MSAA P2a: GL_TEXTURE_2D_MULTISAMPLE scene depth (new path)
	image_t                 *sceneLinearImage;     // HZM MSAA P2a alloc, P2b use: RGBA16F linear colour resolve (bloom/DoF/levels)
	image_t                 *msaaDepthMinMaxImage; // HZM MSAA P2a alloc, P2b use: RG32F (min,max) depth resolve
	image_t					*calcLevelsImage;
	image_t					*targetLevelsImage;
	image_t					*fixedLevelsImage;
	image_t					*sunShadowDepthImage[4];
	image_t                 *screenShadowImage;
	image_t                 *screenSsaoImage;
	image_t					*hdrDepthImage;
	image_t                 *renderCubeImage;
	
	image_t					*textureDepthImage;

	FBO_t					*renderFbo;
	// HZM render scale (r_renderScale): the 3D world scene + post chain render into sceneFbo at
	// SCENE size (round(vid*scale)); renderFbo stays at DISPLAY size and carries the 2D/HUD,
	// present, screenshots and levelshots. When scale == 1.0 (and no MSAA) sceneFbo IS renderFbo
	// (same pointer), so the pipeline is byte-identical to today. displayScratchFbo is the ping
	// target for RCAS at display size (only allocated when scaled).
	FBO_t					*sceneFbo;
	FBO_t					*displayScratchFbo;
	FBO_t					*msaaResolveFbo;
	// HZM gl2 MSAA (plan P2a, tr_msaa.c). msaaSceneFbo / msaaResolveSSFbo are the new path's canonical multisample
	// scene target and its single-sample resolve target; sceneFbo / msaaResolveFbo point at them, or - while
	// r_msaaBypass is on - at (msaaResolveSSFbo, NULL), which is today's MSAA-0 wiring. msaaDepthResolveFbo is the
	// P2b depth-resolve target (colour msaaDepthMinMaxImage, depth renderDepthImage).
	FBO_t					*msaaSceneFbo;
	FBO_t					*msaaResolveSSFbo;
	FBO_t					*msaaDepthResolveFbo;
	FBO_t					*msaaColorResolveFbo;  // HZM MSAA P2b: MRT target - colour0 renderImage, colour1 sceneLinearImage
	FBO_t					*msaaLinearFbo;        // HZM MSAA P2b: sceneLinearImage alone, as a blit SOURCE for bloom/DoF/levels
	FBO_t					*sunRaysFbo;
	FBO_t					*depthFbo;
	FBO_t					*pshadowFbos[MAX_DRAWN_PSHADOWS];
	FBO_t					*screenScratchFbo;
	// HZM gl2 fog parity: a colour-only alias of screenScratchImage. screenScratchFbo has
	// tr.renderDepthImage bound as its GL_DEPTH_ATTACHMENT (tr_fbo.c), so rendering into it
	// while SAMPLING that same depth image (which is exactly what the global-fog pass does)
	// is a rendering feedback loop. This FBO writes the same colour image with no depth
	// attachment at all, so the depth fetch is always well defined.
	FBO_t					*globalFogFbo;
	FBO_t					*textureScratchFbo[2];
	FBO_t                   *quarterFbo[2];
	FBO_t                   *bloomFbo[2];     // HZM exposure-aware bloom (r_ppBloomMode 1)
	FBO_t					*calcLevelsFbo;
	FBO_t					*targetLevelsFbo;
	FBO_t					*sunShadowFbo[4];
	FBO_t					*screenShadowFbo;
	FBO_t					*screenSsaoFbo;
	FBO_t					*hdrDepthFbo;
	FBO_t                   *renderCubeFbo;

	shader_t				*defaultShader;
	shader_t				*shadowShader;
	shader_t				*projectionShadowShader;

	shader_t				*flareShader;
	shader_t				*sunShader;
	shader_t				*sunFlareShader;

	int						numLightmaps;
	int						lightmapSize;
	image_t					**lightmaps;
	image_t					**deluxemaps;

	int						fatLightmapCols;
	int						fatLightmapRows;

	int                     numCubemaps;
	cubemap_t               *cubemaps;

	trRefEntity_t			*currentEntity;
	trRefEntity_t			worldEntity;		// point currentEntity at this when rendering world
	int						currentEntityNum;
	int						shiftedEntityNum;	// currentEntityNum << QSORT_REFENTITYNUM_SHIFT
	model_t					*currentModel;

	//
	// GPU shader programs
	//
	shaderProgram_t genericShader[GENERICDEF_COUNT];
	shaderProgram_t textureColorShader;
	shaderProgram_t fogShader[FOGDEF_COUNT];
	shaderProgram_t dlightShader[DLIGHTDEF_COUNT];
	shaderProgram_t lightallShader[LIGHTDEF_COUNT];
	shaderProgram_t shadowmapShader[SHADOWMAPDEF_COUNT];
	shaderProgram_t pshadowShader;
	shaderProgram_t down4xShader;
	shaderProgram_t bokehShader;
	shaderProgram_t tonemapShader;
	shaderProgram_t tonemapHzmShader;   // HZM gl1-parity ACES grade (r_tonemapMode 1)
	shaderProgram_t bloomBrightShader;  // HZM gl1-parity bloom bright-pass (r_ppBloom)
	shaderProgram_t bloomBlurShader;    // HZM gl1-parity bloom separable Gaussian
	shaderProgram_t fxaaShader;         // HZM gl1-parity FXAA (r_ppFXAA)
	shaderProgram_t sharpenShader;      // HZM gl1-parity CAS sharpen (r_ppSharpen)
	shaderProgram_t fsrEasuShader;      // HZM render scale: AMD FSR 1 EASU upscale (scale < 1.0)
	shaderProgram_t fsrRcasShader;      // HZM render scale: AMD FSR 1 RCAS sharpen (r_fsrSharpness)
	shaderProgram_t fsrDownscaleShader; // HZM render scale: SSAA tent downsample (scale > 1.0)
	shaderProgram_t rainDropsShader;    // HZM gl1-parity rain-on-lens (r_ppRainDrops)
	shaderProgram_t lowHealthShader;    // HZM gl1-parity low-health desat + red vignette + heartbeat
	shaderProgram_t suppressionShader;  // HZM gl1-parity suppression tunnel-vignette
	shaderProgram_t hitBloodShader;     // HZM coop [user 08-02] on-hit blood splatter
	shaderProgram_t heatHazeShader;     // HZM gl1-parity heat haze + localized muzzle shimmer
	shaderProgram_t dofShader;          // HZM gl1-parity depth of field (r_ppDoF)
	shaderProgram_t underwaterShader;   // HZM NEW: underwater/slime/lava screen distortion (r_ppUnderwaterFx)
	shaderProgram_t bloodSpatterShader; // HZM NEW [bug-2360]: blood on the lens (r_ppBloodFx)
	shaderProgram_t chromabShader;      // HZM NEW: chromatic aberration (r_ppChromaticAberration)
	shaderProgram_t motionBlurShader;   // HZM NEW: camera motion blur (r_ppMotionBlur)
	shaderProgram_t filmgrainShader;    // HZM NEW: film grain (r_ppFilmGrain)
	shaderProgram_t frostShader;        // HZM NEW: frost/ice crystals while it snows (r_ppFrost)
	shaderProgram_t globalFogShader;
	shaderProgram_t msaaResolveDepthShader;   // HZM MSAA P2a: built on the new path at >= 2 samples only (P2b draws it)
	shaderProgram_t msaaResolveColorShader;   // HZM MSAA P2b: the tone-exact MRT colour resolve (same condition)
	shaderProgram_t calclevels4xShader[2];
	shaderProgram_t shadowmaskShader;
	shaderProgram_t shadowmaskStableShader;   // HZM gl2 P3: USE_SHADOW_STABLE, TRY-built (0 = stable resolves to legacy)
	shaderProgram_t ssaoShader;
	shaderProgram_t depthBlurShader[4];
	shaderProgram_t testcubeShader;

	// HZM render scale (r_renderScale, latched): resolved once at R_CreateBuiltinImages.
	// sceneWidth/Height = round(vid * renderScale), clamped; renderScaleActive is set when the
	// scene size differs from the display size (guards the byte-identical scale==1.0 path).
	// fsrEasuAvailable is false if the EASU program failed to build (falls back to a bilinear
	// blit). rcasActive is recomputed per world frame in RB_PostProcess and read by RB_HZMScreenFx
	// so it can skip r_ppSharpen while RCAS runs.
	float					renderScale;
	int						sceneWidth;
	int						sceneHeight;
	qboolean				renderScaleActive;
	qboolean				fsrEasuAvailable;
	qboolean				rcasActive;

	// HZM gl2 MSAA (plan P2a, tr_msaa.c; decided by R_DecideMsaa before any image exists, reset with tr at R_Init).
	// msaaNewPath: r_msaaOverride chose the new path. msaaSamples: its resolved count (always 0 on the legacy path,
	// whose count stays r_ext_framebuffer_multisample). displaySplit: renderFbo is a separate single-sample DISPLAY
	// FBO because of MSAA (the render-scale split has renderScaleActive). msaaBypassActive: r_msaaBypass as applied
	// at the last RE_BeginFrame. msaaCompileFailed: an MSAA program failed to build (R_Msaa_AfterGLSL re-inits at 0).
	qboolean				msaaNewPath;
	int						msaaSamples;
	qboolean				displaySplit;
	qboolean				msaaBypassActive;
	qboolean				msaaCompileFailed;


	// -----------------------------------------

	viewParms_t				viewParms;

	float					identityLight;		// 1.0 / ( 1 << overbrightBits )
	int						identityLightByte;	// identityLight * 255
	int						overbrightBits;		// r_overbrightBits->integer, but set to 0 if no hw gamma

	orientationr_t			or;					// for current entity

	trRefdef_t				refdef;

	int						viewCluster;

	float                   sunShadowScale;

	qboolean                sunShadows;
	vec3_t					sunLight;			// from the sky shader for this level
	vec3_t					sunDirection;
	vec3_t                  lastCascadeSunDirection;
	float                   lastCascadeSunMvp[16];

	frontEndCounters_t		pc;
	int						frontEndMsec;		// not in pc due to clearing issue

	//
	// put large tables at the end, so most elements will be
	// within the +/32K indexed range on risc processors
	//
	model_t					*models[MAX_MOD_KNOWN];
	int						numModels;

	int						numImages;
	image_t					*images[MAX_DRAWIMAGES];

	int						numFBOs;
	FBO_t					*fbos[MAX_FBOS];

	int						numVaos;
	vao_t					*vaos[MAX_VAOS];

	// shader indexes from other modules will be looked up in tr.shaders[]
	// shader indexes from drawsurfs will be looked up in sortedShaders[]
	// lower indexed sortedShaders must be rendered first (opaque surfaces before translucent)
	int						numShaders;
	shader_t				*shaders[MAX_SHADERS];
	shader_t				*sortedShaders[MAX_SHADERS];

	int						numSkins;
	skin_t					*skins[MAX_SKINS];

	GLuint					sunFlareQuery[2];
	int						sunFlareQueryIndex;
	qboolean				sunFlareQueryActive[2];

	float					sinTable[FUNCTABLE_SIZE];
	float					squareTable[FUNCTABLE_SIZE];
	float					triangleTable[FUNCTABLE_SIZE];
	float					sawToothTable[FUNCTABLE_SIZE];
	float					inverseSawToothTable[FUNCTABLE_SIZE];
	float					fogTable[FOG_TABLE_SIZE];

	//
	// OPENMOHAA-specific stuff
    //

    int currentSpriteNum;
    int shiftedIsStatic;

    int overbrightShift;
    float overbrightMult;

    portalsky_t portalsky;
    qboolean skyRendered;
    qboolean portalRendered;

    spherel_t sSunLight;
    spherel_t sLights[1532];
    int numSLights;
    int rendererhandle;
    qboolean shadersParsed;
    int frame_skel_index;
    int skel_index[MAX_GENTITIES]; // HZM (bug-gl2-invisible-friendly-actor): ported gl1 bug-932 fix - was a bare [1024] indexed by model->entityNumber. Since the GENTITYNUM_BITS 11 op (2048-entity pool) any skeletal actor on entityNumber >= 1024 OOB-accessed adjacent globals here; in gl2 the OOB read in R_UpdatePoseInternal can spuriously equal frame_skel_index and skip TIKI_SetPoseInternal, so a high-entnum character (allied squadmates/escort NPCs) never gets posed = invisible (enemies on low entnums render fine). Sizing to MAX_GENTITIES matches gl1.
    fontheader_t* pFontDebugStrings;
    int farclip;
    int skyHDCompareBoxes;	// HZM r_skyHD: sky boxes loaded with BOTH sets this map (compare mode); R_Init zeroes it
    int skyHDCompareLayers;	// HZM r_skyHD: sky layers (moving clouds) loaded with BOTH images this map (compare mode)

    // HZM gl2 [2026-09-26] Phase S2 lamp flares (tr_hzm_spot_rb.c). In tr ON PURPOSE (vet F17): R_Init's memset clears
    // it, and R_InitQueries / R_ShutDownQueries own the query names unconditionally - a query name cached OUTSIDE tr
    // crashed the first draw after a resolution-change vid_restart on 07-27.
    shader_t *hzmLampFlareShader;		// scripts/coop_headlights.shader; the default shader = draw no flares (vet F7)
    GLuint hzmFlareQuery[HZM_FLARE_SLOTS][HZM_FLARE_RING][2];	// [flare][ring][0 depth-tested, 1 total]
    qboolean hzmFlareQueriesValid;
    hzmFlareState_t hzmFlareState[HZM_FLARE_SLOTS];
    int hzmFlareFrame;					// the viewParms.frameCount the flare pass last ran for (once per frame)

    // HZM rain wetness (tr_hzm_wet.c, water_wetness_2026-09-27). In tr ON PURPOSE: R_Init's memset clears it with
    // the images it points at; R_HZMWetBuild rebuilds it on every world load.
    qboolean hzmWetReady;
    qboolean hzmSkyFogDeny;				// HZM sky fog model: Omaha / dday2 / m3l2 / m3l3 world - the legacy sky fog (R_HZM_SkyFogMapLoaded)
    qboolean hzmOmahaWorld;				// the loaded BSP is on hzm_waterwet.h's Omaha list: every water/wet path off
    image_t *hzmRainOccImage;
    image_t *hzmWetNoiseImage;
    vec4_t hzmRainOccXform;
    vec2_t hzmRainOccZ;
    float hzmPuddleQ;
    vec4_t hzmSkyZenith;
    vec4_t hzmSkyHorizon;
    vec4_t hzmWetSun;
    vec4_t hzmWetSunCol;

    // HZM water pass (tr_hzm_water.c). Same lifetime rules as the wetness block above.
    qboolean hzmWaterReady;
    int hzmWaterWorld;					// allowlisted water surfaces in the loaded BSP (0 = build nothing)
    image_t *hzmRippleImage;
    shaderProgram_t hzmWaterShader[2];	// [1] = USE_DEFORM_VERTEXES, the fogShader split
    // HZM coop [2026-09-27] realistic lightning (tr_hzm_lightning.c). In tr so R_Init's memset clears all of it: the
    // program and the bolt image are created on the first lit frame and GLSL_ShutdownGPUShaders / the image shutdown
    // free them, after which the memset re-arms the lazy creation.
    hzmLtState_t    hzmLt;
    shaderProgram_t hzmLtSkyShader;
    qboolean        hzmLtSkyShaderTried;
    qboolean        hzmLtSkyShaderOk;
    image_t        *hzmLtBoltImage;
} trGlobals_t;

extern backEndState_t	backEnd;
extern trGlobals_t	tr;
extern glstate_t	glState;		// outside of TR since it shouldn't be cleared during ref re-init
extern glRefConfig_t glRefConfig;

//
// cvars
//
extern cvar_t	*r_flareSize;
extern cvar_t	*r_flareFade;
// coefficient for the flare intensity falloff function.
#define FLARE_STDCOEFF "150"
extern cvar_t	*r_flareCoeff;

extern cvar_t	*r_railWidth;
extern cvar_t	*r_railCoreWidth;
extern cvar_t	*r_railSegmentLength;

extern cvar_t	*r_ignore;				// used for debugging anything
extern cvar_t	*r_verbose;				// used for verbose debug spew

extern cvar_t	*r_znear;				// near Z clip plane
// HZM gl2 re-port Fix 3: ADS view-weapon projection (cgame sets these by name each frame)
extern cvar_t	*r_weaponfovx;			// un-zoomed view-weapon fov_x (0 = disabled)
extern cvar_t	*r_weaponznear;			// weapon near clip (lower = more of the gun's back end)
extern cvar_t	*r_weaponshifty;		// ADS sight vertical screen shift (-up/+down)
extern cvar_t	*r_weaponshiftx;		// ADS sight horizontal screen shift (+right/-left)
extern cvar_t	*r_zproj;				// z distance of projection plane
extern cvar_t	*r_stereoSeparation;			// separation of cameras for stereo rendering

extern cvar_t	*r_measureOverdraw;		// enables stencil buffer overdraw measurement

extern cvar_t	*r_lodbias;				// push/pull LOD transitions
extern cvar_t	*r_lodscale;

extern cvar_t	*r_inGameVideo;				// controls whether in game video should be draw
extern cvar_t	*r_fastsky;				// controls whether sky should be cleared or drawn
extern cvar_t	*r_drawSun;				// controls drawing of sun quad
extern cvar_t	*r_dynamiclight;		// dynamic lights enabled/disabled
extern cvar_t	*r_dlightBacks;			// dlight non-facing surfaces for continuity

extern	cvar_t	*r_norefresh;			// bypasses the ref rendering
extern	cvar_t	*r_drawentities;		// disable/enable entity rendering
extern	cvar_t	*r_drawworld;			// disable/enable world rendering
extern	cvar_t	*r_speeds;				// various levels of information display
extern  cvar_t	*r_detailTextures;		// enables/disables detail texturing stages
extern	cvar_t	*r_novis;				// disable/enable usage of PVS
extern	cvar_t	*r_nocull;
extern	cvar_t	*r_facePlaneCull;		// enables culling of planar surfaces with back side test
extern	cvar_t	*r_nocurves;
extern	cvar_t	*r_showcluster;

extern cvar_t	*r_gamma;
extern cvar_t	*r_displayRefresh;		// optional display refresh option

extern  cvar_t  *r_ext_framebuffer_object;
extern  cvar_t  *r_ext_texture_float;
extern  cvar_t  *r_ext_framebuffer_multisample;
extern  cvar_t  *r_arb_seamless_cube_map;
extern  cvar_t  *r_arb_vertex_array_object;
extern  cvar_t  *r_ext_direct_state_access;

extern	cvar_t	*r_nobind;						// turns off binding to appropriate textures
extern	cvar_t	*r_singleShader;				// make most world faces use default shader
extern	cvar_t	*r_roundImagesDown;
extern	cvar_t	*r_colorMipLevels;				// development aid to see texture mip usage
extern	cvar_t	*r_picmip;						// controls picmip values
extern	cvar_t	*r_finish;
extern	cvar_t	*r_textureMode;
extern	cvar_t	*r_offsetFactor;
extern	cvar_t	*r_offsetUnits;
extern	cvar_t	*r_shadowMapBiasFactor;
extern	cvar_t	*r_shadowMapBiasUnits;

extern	cvar_t	*r_fullbright;					// avoid lightmap pass
extern	cvar_t	*r_lightmap;					// render lightmaps only
extern	cvar_t	*r_vertexLight;					// vertex lighting mode for better performance
extern	cvar_t	*r_uiFullScreen;				// ui is running fullscreen

extern	cvar_t	*r_logFile;						// number of frames to emit GL logs
extern	cvar_t	*r_showtris;					// enables wireframe rendering of the world
extern	cvar_t	*r_showsky;						// forces sky in front of all surfaces
extern	cvar_t	*r_shownormals;					// draws wireframe normals
extern	cvar_t	*r_clear;						// force screen clear every frame

extern	cvar_t	*r_shadows;						// controls shadows: 0 = none, 1 = blur, 2 = stencil, 3 = black planar projection
extern	cvar_t	*r_flares;						// light flares

extern	cvar_t	*r_intensity;

extern	cvar_t	*r_lockpvs;
extern	cvar_t	*r_noportals;
extern	cvar_t	*r_portalOnly;

extern	cvar_t	*r_subdivisions;
extern	cvar_t	*r_lodCurveError;
extern	cvar_t	*r_skipBackEnd;

extern	cvar_t	*r_anaglyphMode;

extern  cvar_t  *r_externalGLSL;

extern  cvar_t  *r_hdr;
extern  cvar_t  *r_floatLightmap;
extern  cvar_t  *r_postProcess;

extern  cvar_t  *r_toneMap;
extern  cvar_t  *r_forceToneMap;
extern  cvar_t  *r_forceToneMapMin;
extern  cvar_t  *r_forceToneMapAvg;
extern  cvar_t  *r_forceToneMapMax;

extern  cvar_t  *r_autoExposure;
extern  cvar_t  *r_forceAutoExposure;
extern  cvar_t  *r_forceAutoExposureMin;
extern  cvar_t  *r_forceAutoExposureMax;

extern  cvar_t  *r_cameraExposure;

extern  cvar_t  *r_depthPrepass;
extern  cvar_t  *r_ssao;

extern  cvar_t  *r_normalMapping;
extern  cvar_t  *r_specularMapping;
extern  cvar_t  *r_deluxeMapping;
extern  cvar_t  *r_parallaxMapping;
extern  cvar_t  *r_parallaxMapOffset;
extern  cvar_t  *r_parallaxMapShadows;
extern  cvar_t  *r_cubeMapping;
extern  cvar_t  *r_cubemapSize;
extern  cvar_t  *r_hzmAlphaGenCoord;   // HZM bug-1249: alphaGen sCoord/tCoord parity
extern  cvar_t  *r_hzmFlapDeform;      // HZM diagnostic: kill switch for deformVertexes flap
// HZM gl2 [2026-09-25] RETAIL LIGHT-GLOW PARITY (vehicle-headlight plan, Phase R). gl2 parsed `deformVertexes
// lightglow` and `rgbGen dot` and then drew neither. All four switches are flags 0 (never saved - TRAPS T7) and
// are read per draw, so they toggle live. -1 = auto = the HZM_*_AUTO define in the shared header below; 0 / 1
// force it. On the Omaha BSPs both restores are OFF whatever these say. The AUTO defaults AND the Omaha list
// live in ONE header shared with cgame (the headlight manager), because the two DLLs ship as a pair.
#include "../renderercommon/hzm_light_restore.h"
extern  cvar_t  *r_hzmLightGlow;          // deformVertexes lightglow: 0 = today (flat quad), 1 = gl1 billboard + pull
extern  cvar_t  *r_hzmLightGlowNear;      // 0 = gl1 verbatim, 1 = shrink a glow the eye is right at (no white-out)
extern  cvar_t  *r_hzmLightGlowNearRange; // tuning: the near-shrink starts inside this many glow radii (default 2)
extern  cvar_t  *r_hzmRgbGenDot;          // rgbGen dot / oneMinusDot: 0 = today (flat 1.0), 1 = gl1 (N.V)^2 fade
// What -1 means: HZM_LIGHTGLOW_AUTO / HZM_LIGHTGLOWNEAR_AUTO / HZM_RGBGENDOT_AUTO in hzm_light_restore.h.
qboolean R_HZM_LightGlowOn( void );                  // r_hzmLightGlow resolved, Omaha exclusion applied
qboolean R_HZM_LightGlowNearOn( void );              // r_hzmLightGlowNear resolved
qboolean R_HZM_RgbGenDotOn( void );                  // r_hzmRgbGenDot resolved, Omaha exclusion applied
qboolean R_HZM_AlphaDotToRgb( const shaderStage_t *pStage ); // searchlights S1: additive alphaGen dot -> RGB (gl1)
qboolean R_HZM_LightRestoreProtectedWorld( void );   // the loaded BSP is on the Omaha protection list
// HZM coop [2026-09-27] realistic lightning (tr_hzm_lightning.c)
void     R_HzmLt_Register( void );
void     R_HzmLt_FrontEnd( int refdefTime );
void     RB_HzmLt_SetUniforms( shaderProgram_t *sp, int stateBits, qboolean fogAsSky );
void     RB_HzmLt_ZeroUniforms( shaderProgram_t *sp );
void     RB_HzmLt_SkyPass( void );
qboolean GLSL_HzmLtInitSkyShader( void );
// HZM gl2 [2026-09-25] Phase R3 EDGEFADE window: 1 at the light, 0 at `radius`, smooth (saturate(1 - t^4)^2).
// Used ONLY for lights carrying hzm_dlight_edgefade (tr_types_new.h), so every other light is byte-identical.
static ID_INLINE float R_HZM_DlightEdgeWindow( float dist, float radius ) {
	float t, w;

	if ( radius <= 0.0f ) {
		return 0.0f;
	}
	t = dist / radius;
	t *= t;
	t *= t;
	w = 1.0f - t;
	if ( w <= 0.0f ) {
		return 0.0f;
	}
	return w * w;
}
// HZM gl2 [2026-09-26] PHASE S spot cones + lamp flares (docs/proposals/headlights_2026-09-25/plan_phaseS.md and
// vet_phaseS.md). Pure maths in tr_hzm_spot.c (compiled unchanged into docs/tools/hzm_spot_selftest), renderer glue in
// tr_hzm_spot_rb.c. Carrier protocol, bit layout, AUTO defines and the Omaha list: renderercommon/hzm_light_restore.h.
extern  cvar_t  *r_hzmSpot;             // -1 auto (HZM_SPOT_AUTO), 0/1: spot lights + carriers honoured
extern  cvar_t  *r_hzmFlares;           // -1 auto (HZM_FLARES_AUTO), 0/1: lamp flares (also r_flares)
extern  cvar_t  *r_hzmSpotProtocol;     // CVAR_ROM handshake, forced to HZM_SPOT_PROTOCOL at R_Init, 0 at RE_Shutdown
extern  cvar_t  *r_hzmSpotDebug;        // 1 = ^~^~^ HZMSPOT line a second, 2 = + magenta tint on spot-lit pixels
extern  cvar_t  *r_hzmSpotEntScale;     // soldiers / props / statics: the one knob on the one law
extern  cvar_t  *r_hzmFlareSize, *r_hzmFlareIntensity, *r_hzmFlareSize1, *r_hzmFlareIntensity1;
extern  cvar_t  *r_hzmFlareNear, *r_hzmFlareFade, *r_hzmFlareBudget;
// tr_hzm_spot.c - pure
void     R_HZM_SpotClearLight( dlight_t *dl );
hzmAdd_t R_HZM_SpotClassify( float intensity, int type, qboolean spotsOn, qboolean flaresOn );
void     R_HZM_SpotBeginLight( dlight_t *dl, int type, qboolean isSpot );
int      R_HZM_SpotAttachCarrier( dlight_t *dlights, int first, int num, int tag, const vec3_t axis, float cosInner,
                                  float cosOuter, float owner );
int      R_HZM_SpotCompact( dlight_t *dlights, int first, int num, int *dropped );
qboolean R_HZM_SpotIntakeCore( dlight_t *dlights, int first, int num, hzmFlare_t *flares, int *numFlares, int maxFlares,
                               const vec3_t org, float intensity, float r, float g, float b, int type,
                               qboolean spotsOn, qboolean flaresOn, hzmSpotStats_t *stats, hzmAdd_t *verdict );
float    R_HZM_SpotAttenuation( float distSq, float radius );
float    R_HZM_SpotConeDir( const vec3_t axis, float cosOuter, float coneK, const vec3_t toPoint );
float    R_HZM_SpotConeSphere( const vec3_t apex, const vec3_t axis, float cosOuter, float coneK, const vec3_t center,
                               float radius );
qboolean R_HZM_SpotSphereTouches( const vec3_t apex, const vec3_t axis, float cosOuter, const vec3_t center,
                                  float radius );
float    R_HZM_DlightFactorAt( const dlight_t *dl, const vec3_t point, float radius );   // exactly 1.0f unless a cone
void     R_HZM_SpotUniformVec( const dlight_t *dl, const vec3_t axis, qboolean debugTint, vec4_t out );
qboolean R_HZM_FlareFromCarrier( const vec3_t org, float intensity, float r, float g, float b, int type,
                                 hzmFlare_t *out );
void     R_HZM_FlareShape( const hzmFlare_t *f, const vec3_t eye, float viewportHeight, float size, float intensity,
                           float nearDist, float lobe, float *outHalfPx, float *outIntensity, float *outDist );
// tr_hzm_spot_rb.c - glue
void     R_HZM_SpotRegister( void );
void     R_HZM_SpotShutdown( void );
qboolean R_HZM_SpotsOn( void );
qboolean R_HZM_FlaresOn( void );
float    R_HZM_SpotEntityK( void );
qboolean R_HZM_SpotIntake( const vec3_t org, float intensity, float r, float g, float b, int type, hzmAdd_t *verdict );
void     R_HZM_SpotNextFrame( void );
void     R_HZM_SpotClearScene( void );
void     R_HZM_SpotEndScene( void );
void     R_HZM_SpotFinishScene( void );
void     RB_HZM_SpotForwardUniform( shaderProgram_t *sp, const dlight_t *dl );
void     RB_HZM_SpotProjectUniform( shaderProgram_t *sp, const dlight_t *dl );
void     RB_HZM_SpotZeroUniform( shaderProgram_t *sp );
qboolean RB_HZM_SpotSphereLight( const dlight_t *dl, const sphereor_t *sph, const trRefEntity_t *ent,
                                 reallightinfo_t *out );
void     R_HZM_FlareInitQueries( void );
void     R_HZM_FlareShutdownQueries( void );
void     RB_HZM_SpotFlares( void );
// HZM gl2 [2026-09-26] PHASE S-b (gfx tree; renderercommon/hzm_light_restore.h section 4)
extern  cvar_t  *r_hzmSoftEdge;         // S3b: -1 auto (HZM_SOFTEDGE_AUTO), 0/1; also needs r_softParticles
extern  cvar_t  *r_hzmSoftEdgeScale;    // S3b: x every shader's qer_hzmSoftEdge distance (tuning, default 1)
qboolean R_HZM_SoftEdgeOn( void );                          // tr_hzm_spot_rb.c
float    RB_HZM_SoftEdgeDistance( void );                   // > 0: THIS draw fades within that many u
qboolean RB_HZM_SceneDepthSnapshot( void );                 // tr_backend.c: the one scene-depth snapshot
void     RB_HZM_BindSceneDepth( void );                     // tr_backend.c: ... bound on TB_SCREENDEPTH
void     RB_HZM_SoftEdgeSnapshotAt( int entityNum, qboolean bStaticModel ); // tr_backend.c: the S3b hoist
extern  cvar_t  *r_hzmSpotShadows;      // S4: -1 auto (HZM_SPOTSHADOW_AUTO), 0/1; also needs r_hzmDlightShadows + spots
extern  cvar_t  *r_hzmSpotShadowCasters;// S4: casters the spot's shadow may take (of r_hzmDlightShadowCasters)
extern  cvar_t  *r_hzmSpotShadowReach;  // S4: cap on a spot shadow's length, u
qboolean R_HZM_SpotShadowsOn( void );                       // tr_hzm_spot_rb.c

// HZM gl2 [2026-09-27] FOG-OWNED LOD + fog-faded SSAO (docs/proposals/fog_lod_pop_2026-09-27, plan pieces B + A).
// tr_hzm_lodfog.c is pure (arguments only; docs/tools/hzm_lodfog_selftest), tr_hzm_lodfog_rb.c is the glue.
typedef struct {
	float		nearDist;		// fade start to use
	float		range;			// fade range to use (always the shader's own)
	float		distScale;		// multiply the UNSCALED model-space tess.xyz by this before measuring a vertex distance
	float		gateDist;		// > 0: per-vertex fades hold their near-side value while the ORIGIN is closer than this
	qboolean	trueDistance;	// coarse culls use the true 3-D distance (not R_DistanceCullPointAndRadius's doubled dz)
} hzmLodBand_t;
#define HZM_LODBAND_AUTHORED	0	// retail band, retail distances, retail culls
#define HZM_LODBAND_GOVERNED	1	// the whole model is past 100% fog before any fade starts
#define HZM_LODBAND_HANDBACK	2	// above the cap: sliding back to the authored band
float    R_HZM_LodFogFloor( float farplane, float endScale, float fovXdeg, float fovYdeg );
int      R_HZM_LodFogBand( float floorDist, float cap, float instRadius, float nearIn, float rangeIn, float modelScale,
                           hzmLodBand_t *out );
// the Omaha-list decision, cached per LOADED BSP NAME (bug: a tr.world POINTER key never changes - tr.world is
// always &s_worldData - so the first map of the DLL's life decided every later map). The cache is the caller's.
typedef struct {
	char		name[MAX_QPATH];	// tr.world->baseName the decision was made for
	qboolean	valid;
	qboolean	excluded;
} hzmLodFogMapCache_t;
qboolean R_HZM_LodFogMapNameExcluded( const char *worldBaseName );
qboolean R_HZM_LodFogExcludedCached( const char *worldBaseName, hzmLodFogMapCache_t *cache, qboolean *changed );
// tr_hzm_lodfog_rb.c - glue
extern cvar_t *r_hzmLodFog;			// 0 (default) = retail; 1 = piece B
extern cvar_t *r_hzmLodFogCap;		// 3500: B governs a model only while floor + radius <= this
extern cvar_t *r_hzmLodFogOmaha;	// 0 (default) = m3l1a m3l1b e3l1 e3l2 obj_team3 stay retail
extern cvar_t *r_hzmFogAO;			// 0 (default) = retail; 1 = piece A (tr_postprocess.c RB_HZMSsao)
extern cvar_t *r_hzmLodFogDebug;	// 0 (default); 2 = no-LOD REFERENCE for the headless dolly (plan.md 5.3)
void     R_HZM_LodFogRegister( void );
float    R_HZM_LodFogFloorForView( const viewParms_t *vp );
int      R_HZM_LodFogStaticBand( float floorDist, const shader_t *sh, const cStaticModelUnpacked_t *SM,
                                 hzmLodBand_t *out );
extern  cvar_t  *r_cubemapAuto;        // HZM bug-1237: auto probe budget (info_pathnode placement)
extern  cvar_t  *r_cubemapAutoRadius;  // HZM bug-1237: parallax radius for auto-placed probes

// HZM bug-1237: ceiling on auto-placed probes. Each costs SIX full world renders at map load
// plus a cubemap texture, so this is a load-time and VRAM guard, not a quality knob.
#define MAX_AUTO_CUBEMAPS 64
extern  cvar_t  *r_deluxeSpecular;
extern  cvar_t  *r_pbr;
extern  cvar_t  *r_baseNormalX;
extern  cvar_t  *r_baseNormalY;
extern  cvar_t  *r_baseParallax;
extern  cvar_t  *r_baseSpecular;
extern  cvar_t  *r_baseGloss;
extern  cvar_t  *r_glossType;
extern  cvar_t  *r_dlightMode;
extern  cvar_t  *r_pshadowDist;
extern  cvar_t  *r_mergeLightmaps;
extern  cvar_t  *r_imageUpsample;
extern  cvar_t  *r_imageUpsampleMaxSize;
extern  cvar_t  *r_imageUpsampleType;
extern  cvar_t  *r_genNormalMaps;

// HZM gl2 - targeted generated normal maps + confined specular (tr_image.c header block).
// All CVAR_ARCHIVE, none CVAR_LATCH (bug-1181: vid_restart crashes gl2) and none CVAR_CHEAT
// (bug-1156: a listen server silently clamps cheat cvars).
extern  cvar_t  *r_hzmGenNormals;			// MASTER 0=off 1=allow-list 2=broad
extern  cvar_t  *r_hzmGenNormalStrength;	// live tangent-space XY scale
extern  cvar_t  *r_hzmGenNormalMaxSize;		// cap on the generated map's long edge
extern  cvar_t  *r_hzmGenNormalBlur;		// binomial blur passes on the height field
extern  cvar_t  *r_hzmGenNormalBrighten;	// upstream's destructive albedo re-brighten
extern  cvar_t  *r_hzmGenNormalInclude;		// substring allow-list (mode 1)
extern  cvar_t  *r_hzmGenNormalExclude;		// extra substring deny-list
extern  cvar_t  *r_hzmGenNormalDebug;
extern  cvar_t  *r_hzmSpecular;				// F0 applied ONLY to generated-relief stages
extern  cvar_t  *r_hzmSpecularGloss;
extern  cvar_t  *r_hzmParallaxDepth;   // LIVE parallax depth (r_baseParallax is CVAR_LATCH)
extern  cvar_t  *r_hzmParallaxFade;    // world units at which parallax has faded to nothing
extern  cvar_t  *r_hzmNormalStrength;  // relief multiplier for AUTHORED _n/_nh maps (not synthesised)
// HZM gl2 [bug-3007] terrain relief light direction per heightmap vertex instead of one per 512u patch (tr_surface.c
// R_HZM_TerrainLightGridBuild). flags 0, live: -1 or "" = HZM_TERRAINLIGHTGRID_AUTO, 0 = the bug-2905 per-patch
// centre, 1 = per vertex. The Omaha ground set always keeps the per-patch path (user rule). Flipping the define here
// reaches every player who never touched the cvar (TRAPS T7); a saved value never could.
extern  cvar_t  *r_hzmTerrainLightGrid;
#define HZM_TERRAINLIGHTGRID_AUTO 0

extern  cvar_t  *r_forceSun;
extern  cvar_t  *r_forceSunLightScale;
extern  cvar_t  *r_forceSunAmbientScale;
extern  cvar_t  *r_sunlightMode;
extern  cvar_t  *r_drawSunRays;
extern  cvar_t  *r_sunShadows;
extern  cvar_t  *r_shadowFilter;
extern  cvar_t  *r_shadowBlur;
extern  cvar_t  *r_shadowMapSize;
extern  cvar_t  *r_shadowCascadeZNear;
extern  cvar_t  *r_shadowCascadeZFar;
extern  cvar_t  *r_shadowCascadeZBias;

// HZM gl2 REAL CHARACTER SHADOWS. r_charShadows is the MASTER and defaults to 0:
// with it off, every code path guarded below is skipped and the renderer behaves
// exactly as it does today (blob decal only). The rest are inert until it is 1.
extern  cvar_t  *r_charShadows;
extern  cvar_t  *r_charShadowCascade;
extern  cvar_t  *r_charShadowDist;
extern  cvar_t  *r_charShadowLod;
extern  cvar_t  *r_charShadowBiasFactor;
extern  cvar_t  *r_charShadowBiasUnits;
extern  cvar_t  *r_charShadowBlob;
extern  cvar_t  *r_shadowCastFoliage;

// HZM gl2 (bug-gl2-sphereslot-alias): 1 = scope the per-entity light-sphere cache to the
// draw-surf LIST that built it, matching the scope of the index it validates. Default 0 =
// today's behaviour exactly.
extern  cvar_t  *r_sphereCacheScope;
extern  cvar_t  *r_shadowDebug;
extern  cvar_t  *r_coopSunPublish;

// HZM gl2 CHARACTER LIGHTING. r_charLighting is the MASTER and defaults to 0: with it
// off, backEnd.charLight.active is never set, every guarded path is skipped, and the
// renderer behaves exactly as it does today. The rest are inert until it is 1.
extern  cvar_t  *r_charLighting;
extern  cvar_t  *r_charLightWrap;
extern  cvar_t  *r_charLightShadow;
extern  cvar_t  *r_charLightDebug;

// HZM gl2 [user 2026-09-09, bug-2554] ENTITY LIGHT SMOOTHING. Seconds; 0 disables and restores the
// previous frame-independent behaviour exactly. Neither renderer had any temporal damping, so a
// change in which lights the sphere selected landed in a single frame - "it seems like an on/off
// switch". See R_CoopSmoothEntityLight in tr_light.c.
extern  cvar_t  *r_entLightSmooth;

// HZM gl2 [user 2026-09-09, bug-2556] Upper end of the static-lamp admission fade, in the same
// `falloff` units as the hard `>= 5.0` gate in tr_sphere_shade.cpp. 15 puts full strength at 0.577 x
// a lamp's reach; 5 or below disables the taper and restores the hard gate exactly.
extern  cvar_t  *r_entLightFade;

// HZM gl2 [user 2026-09-09, bug-2559] r_mapOverBrightBits is a BIT SHIFT, so it can only ever be 1
// (2x) or 2 (4x) - there is no value between. This float trims the same lightmap scale so a half
// step is reachable: 4x * 0.71 = 2.83x = 2^1.5. CVAR_LATCH like its parent, because the scale is
// baked into the lightmap texture at map load.
extern  cvar_t  *r_mapOverBrightScale;

// Ease one entity's light toward a new value. rgb is required; dir may be NULL. Keyed on the GAME
// entity number, advances at most once per entity per frame, and SNAPS across a discontinuity.
void R_CoopSmoothEntityLight( int entityNumber, vec3_t rgb, vec3_t dir );
void R_CoopResetEntityLightSmoothing( void );

// HZM gl2 DYNAMIC-LIGHT CAST SHADOWS. r_hzmDlightShadows is the MASTER and defaults to
// 0: with it off R_DlightShadowsActive() returns qfalse on its first line, no dlight
// pshadow is ever built, tr.refdef.num_pshadows stays 0 exactly as it is today, and
// every path guarded below is skipped. The rest are inert until it is 1.
// None are CVAR_LATCH (vid_restart crashes gl2, bug-1181) or CVAR_CHEAT (a listen
// server clamps CVAR_CHEAT back to the default, bug-1156).
extern  cvar_t  *r_hzmDlightShadows;
extern  cvar_t  *r_hzmDlightShadowLights;
extern  cvar_t  *r_hzmDlightShadowMax;
extern  cvar_t  *r_hzmDlightShadowDist;
extern  cvar_t  *r_hzmDlightShadowMinRadius;
extern  cvar_t  *r_hzmDlightShadowCasters;
extern  cvar_t  *r_hzmDlightShadowChars;
extern  cvar_t  *r_hzmDlightShadowDebug;

extern  cvar_t  *r_ignoreDstAlpha;

extern	cvar_t	*r_greyscale;

extern	cvar_t	*r_ignoreGLErrors;

extern	cvar_t	*r_overBrightBits;
extern	cvar_t	*r_mapOverBrightBits;

extern	cvar_t	*r_debugSurface;
extern	cvar_t	*r_simpleMipMaps;

extern	cvar_t	*r_showImages;
extern	cvar_t	*r_debugSort;

extern	cvar_t	*r_printShaders;

extern cvar_t	*r_marksOnTriangleMeshes;

//
// OPENMOHAA-specific stuff
//=========================

extern int r_sequencenumber;

// DRAWING

extern cvar_t	*r_drawentitypoly;
extern cvar_t	*r_drawstaticmodels;
extern cvar_t	*r_drawstaticmodelpoly;
extern cvar_t	*r_drawstaticdecals;
extern cvar_t	*r_drawterrain;
extern cvar_t	*r_drawsprites;
extern cvar_t	*r_drawspherelights;
// HZM gl2 re-port (bug-gl2-modellight): gl1 tr_local.h:1590
extern cvar_t	*r_fastentlight;

// HZM coop - gore tier 4 (UV wounds) - HZM gl2 re-port (bug-gl2-gore), gl1 tr_local.h:1696-1701
extern  cvar_t* r_goreUV;
extern  cvar_t* r_goreDebug;
extern  cvar_t* coop_goreSkinWoundScale;	// HZM coop - bloodier wounds on EXPOSED SKIN only (face/head/hands)
extern  cvar_t* coop_goreSkinSnap;		// HZM coop - skin-snap fallback for moving enemies (0 = off)
extern  cvar_t* coop_goreSkinSnapDist;	// HZM coop - skin-snap vertex tolerance in model units (clamped 8-64)

extern cvar_t	*r_numdebuglines;
extern cvar_t	*r_stipplelines;
extern cvar_t	*r_debuglines_depthmask;

extern cvar_t	*r_maxpolys;
extern int		max_polys;
extern cvar_t	*r_maxpolyverts;
extern int		max_polyverts;
extern cvar_t* r_maxtermarks;
extern int		max_termarks;

extern cvar_t* r_skyportal;
extern cvar_t* r_skyportal_origin;
extern cvar_t* r_farplane;
extern cvar_t* r_farplane_bias;
extern cvar_t* r_farplane_color;
extern cvar_t* r_farplane_nocull;
extern cvar_t* r_farplane_nofog;
extern cvar_t* r_skybox_farplane;
extern cvar_t* r_farclip;

//
// HZM gl2 fog parity: the screen-space port of gl1's fixed-function global farplane fog.
// r_globalFog* are tuning/diagnostic knobs; the shipped defaults reproduce gl1 1:1.
//
extern cvar_t* r_globalFog;				// 0 = off entirely (A/B kill switch)
extern cvar_t* r_globalFogScale;		// multiplies the computed fog fraction
extern cvar_t* r_globalFogStartScale;	// multiplies farplane_bias  (fog START)
extern cvar_t* r_globalFogEndScale;		// multiplies farplane_distance (fog END)
extern cvar_t* r_globalFogSky;
extern cvar_t* r_hzmSkyFog;			// sky fog model: -1/"" = HZM_SKYFOG_AUTO, 0 = legacy (sky fogged at zFar), 1 = model
extern cvar_t* r_hzmSkyFogHeight;		// the fog-layer height H (world units): the zenith is fogged as geometry at H
extern cvar_t* r_hzmSkyFogPortal;		// 1 = a portal-sky view (3D skybox) is fogged as sky too; 0 (default) = never, as before
#define HZM_SKYFOG_AUTO 1
qboolean R_HZM_SkyFogModelOn( void );
void     RB_HZM_SkyFogUniforms( shaderProgram_t *sp, qboolean isSky );
void     R_HZM_SkyFogMapLoaded( void );
typedef struct {
	qboolean	active;
	int			frame;
	float		start, end;
	vec3_t		color;
} hzmSkyFogLatch_t;
extern hzmSkyFogLatch_t rb_hzmSkyFogLatch;	// the main view's fog, for a portal-sky view of the next frame			// 1 = fog sky pixels too (gl1 "nofog" sky = 0)
// HZM gl2 [2026-09-25] HD sky boxes (docs/tools/gen_sky_hd.py -> zzzzzzzzzz_coop_hd_skies.pk3, env/hzmhd/).
extern cvar_t* r_skyHD;				// -1 auto (boxes HZM_SKYHD_AUTO, layers HZM_SKYHD_LAYERS_AUTO), 0 original, 1 HD - both. Next map.
extern cvar_t* r_skyHDCompare;		// TEST ONLY, CVAR_TEMP: 0 normal, 1 show HD, 2 show original (live A/B, boxes + layers)
// What r_skyHD -1 means for the sky BOXES. ON since 2026-09-26: the user liked the HD boxes ("they look way better")
// on the condition that the faint face seams be hidden - the bug-2987 seam pass removed the shared dark edge row.
// Flipping here reaches every player who never touched the cvar (saved value "-1"), unlike an archived default (T7).
#define HZM_SKYHD_AUTO 1
// The same, for the sky LAYERS (moving clouds, env/hzmhd/clouds/). User A/B on m1l1 2026-09-25: "new clouds look
// way better go with them" -> auto ON (bug-2942). The boxes above stay auto OFF until their own A/B (sky1-sky3).
#define HZM_SKYHD_LAYERS_AUTO 1
extern cvar_t* r_globalFogRadial;		// 0 = planar eye Z (gl1), 1 = radial distance
extern cvar_t* r_globalFogIdentityLight;// 1 = scale the fog colour by tr.identityLight
extern cvar_t* r_globalFogDebug;		// 1 = log values, 2 = show fraction, 3 = show distance
extern cvar_t* r_globalFogForward;		// 1 = mix fog IN the surface shaders (gl1 order), 0 = old screen-space pass
extern cvar_t  *r_ppDoF;            // HZM gl2 (bug-1157) gl1-parity depth of field
extern cvar_t  *r_ppSSAO;           // HZM gl2 (bug-1177) gl1-parity SSAO master (CVAR_LATCH - gates buffer alloc)
extern cvar_t  *r_ppSSAORadius;
extern cvar_t  *r_ppSSAOIntensity;
extern cvar_t  *r_ppSSAOBias;
extern cvar_t  *r_ppSSAODepthAware;

// HZM render-scale supersampling + AMD FSR 1 (r_renderScale)
extern cvar_t  *r_renderScale;      // CVAR_ARCHIVE|CVAR_LATCH, 0.5-2.0, default 1.0 (launch-only, bug-1181)
extern cvar_t  *r_upscaleFilter;    // CVAR_ARCHIVE, 0 = bilinear blit, 1 = FSR EASU (<1) / SSAA tent (>1)
extern cvar_t  *r_fsrSharpness;     // CVAR_ARCHIVE, 0..1 RCAS strength (0 = off); replaces r_ppSharpen while >0
extern cvar_t  *r_renderScaleDebug; // CVAR_TEMP, 1 = print the resolved scene/display size once per map

// HZM gl2 soft particles (r_softParticles)
extern cvar_t  *r_softParticles;        // CVAR_ARCHIVE|CVAR_LATCH, default 1 (launch-only: widens the hdrDepth alloc gate)
extern cvar_t  *r_softParticleDistance; // CVAR_ARCHIVE, default 24 world units, live
extern cvar_t  *r_softParticlesDebug;   // CVAR_TEMP, 1 = draw the fade factor k as greyscale on particles

// Global farplane fog state, latched from the MAIN world view in RB_DrawSurfs (which is
// where gl1 calls RB_SetupFog) so the post pass can never inherit a portal / sky-portal /
// shadow sub-view's parameters or a projection matrix it was not rasterised with.
typedef struct {
	qboolean	active;
	float		start;			// gl1 GL_FOG_START = viewParms.farplane_bias
	float		end;			// gl1 GL_FOG_END   = viewParms.farplane_distance
	vec3_t		color;			// gl1 fog colour (farplane_color [* tr.identityLight])
	float		projMat10;		// projectionMatrix[10] actually used for this view
	float		projMat14;		// projectionMatrix[14] actually used for this view
	float		projMat0;		// projectionMatrix[0]  (for the radial option)
	float		projMat5;		// projectionMatrix[5]
	float		zNear;			// derived from the matrix, for logging only
	float		zFar;			// derived from the matrix, for logging only
} globalFogState_t;

extern globalFogState_t	rb_globalFog;

// HZM [UNDERWATER VOLUME v3, 2026-09-03] The projection terms a depth-reading POST pass needs,
// latched UNCONDITIONALLY. rb_globalFog above carries the same two numbers, but only assigns
// them when farplane fog is ACTIVE - SIX early returns sit above that assignment in
// RB_SetupGlobalFog (no world model, a portal/shadow sub-view, no farplane distance,
// r_farplane_nofog, r_globalFog 0, a degenerate matrix). On a map with no farplane fog it
// therefore still holds the LAST FOGGED MAP's matrix, so any pass that trusted it would
// reconstruct eye distances from a projection that never rasterised this view. Same latch point
// and same view policy as the fog, none of its preconditions. It is a file-scope global, so
// valid is qfalse before the first frame without an explicit initialiser.
// Read by RB_HZMExtraFx (tr_postprocess.c).
typedef struct {
	qboolean	valid;
	float		projMat10;		// projectionMatrix[10] actually used for this view
	float		projMat14;		// projectionMatrix[14] actually used for this view
	float		projMat0;		// projectionMatrix[0]
	float		projMat5;		// projectionMatrix[5]
	float		zNear;			// derived from the matrix, for logging only
	float		zFar;			// derived from the matrix, for logging only
} viewProjLatch_t;

extern viewProjLatch_t	rb_viewProj;

void RB_SetupGlobalFog( void );

// HZM gl2 FORWARD GLOBAL FOG: upload u_GlobalFogColor / u_GlobalFogParams for one draw.
// stateBits selects gl1's per-stage fog target (black for additive, white for modulate,
// fog colour otherwise); fogAsSky ties the draw to r_globalFogSky. Lives in tr_shade.c and is
// also called from tr_sky.c, so it must have external linkage.
void RB_SetGlobalFogUniforms( shaderProgram_t *sp, int stateBits, qboolean fogAsSky );
// qtrue when the forward path owns the fog this frame. r_globalFogDebug and r_globalFogRadial
// are implemented only by the screen-space pass, so either being set forces the legacy path -
// otherwise flipping the forward default would silently kill the diagnostic tooling built for
// this exact workstream (and an archived r_globalFogRadial 1 would become a silent no-op).
qboolean R_UseForwardGlobalFog( void );

// Lighting

extern cvar_t* r_lightcoronasize;
extern cvar_t *r_entlight_scale;
extern cvar_t *r_entlight_errbound;
extern cvar_t *r_entlight_cubelevel;
extern cvar_t *r_entlight_cubefraction;
extern cvar_t *r_entlight_maxcalc;
extern cvar_t *r_light_lines;
extern cvar_t *r_light_sun_line;
extern cvar_t *r_light_int_scale;
extern cvar_t *r_light_nolight;
extern cvar_t *r_light_showgrid;

// LOD

extern cvar_t* r_staticlod;
extern cvar_t* r_lodscale;
extern cvar_t* r_lodcap;
extern cvar_t* r_lodviewmodelcap;

extern cvar_t* r_uselod;
extern cvar_t* lod_LOD;
extern cvar_t* lod_minLOD;
extern cvar_t* lod_maxLOD;
extern cvar_t* lod_LOD_slider;
extern cvar_t* lod_curve_0_val;
extern cvar_t* lod_curve_1_val;
extern cvar_t* lod_curve_2_val;
extern cvar_t* lod_curve_3_val;
extern cvar_t* lod_curve_4_val;
extern cvar_t* lod_edit_0;
extern cvar_t* lod_edit_1;
extern cvar_t* lod_edit_2;
extern cvar_t* lod_edit_3;
extern cvar_t* lod_edit_4;
extern cvar_t* lod_curve_0_slider;
extern cvar_t* lod_curve_1_slider;
extern cvar_t* lod_curve_2_slider;
extern cvar_t* lod_curve_3_slider;
extern cvar_t* lod_curve_4_slider;
extern cvar_t* lod_pitch_val;
extern cvar_t* lod_zee_val;
extern cvar_t* lod_mesh;
extern cvar_t* lod_meshname;
extern cvar_t* lod_tikiname;
extern cvar_t* lod_metric;
extern cvar_t* lod_tris;
extern cvar_t* lod_position;
extern cvar_t* lod_save;
extern cvar_t* lod_tool;

// UTILS

extern cvar_t* r_developer;
extern cvar_t* r_fps;
extern cvar_t* r_showstaticbboxes;
extern cvar_t* r_showcull;
extern cvar_t* r_showlod;
extern cvar_t* r_showstaticlod;
extern cvar_t* r_showportal;

//=========================

extern cvar_t *r_vaoCache;

//====================================================================

static ID_INLINE qboolean ShaderRequiresCPUDeforms(const shader_t * shader)
{
	if(shader->numDeforms)
	{
		const deformStage_t *ds = &shader->deforms[0];

		if (shader->numDeforms > 1)
			return qtrue;

		switch (ds->deformation)
		{
			case DEFORM_WAVE:
			case DEFORM_BULGE:
				// need CPU deforms at high level-times to avoid floating point percision loss
				return ( backEnd.refdef.floatTime != (float)backEnd.refdef.floatTime );

			default:
				return qtrue;
		}
	}

	return qfalse;
}

//====================================================================

void R_SwapBuffers( int );

void R_RenderView( viewParms_t *parms );
void R_RenderDlightCubemaps(const refdef_t *fd);
void R_RenderPshadowMaps(const refdef_t *fd);
void R_RenderSunShadowMaps(const refdef_t *fd, int level);
void R_RenderCubemapSide( int cubemapIndex, int cubemapSide, qboolean subscene );

void R_AddMD3Surfaces( trRefEntity_t *e );
void R_AddNullModelSurfaces( trRefEntity_t *e );
void R_AddBeamSurfaces( trRefEntity_t *e );
void R_AddRailSurfaces( trRefEntity_t *e, qboolean isUnderwater );
void R_AddLightningBoltSurfaces( trRefEntity_t *e );

void R_AddPolygonSurfaces( void );

void R_DecomposeSort( unsigned sort, int *entityNum, shader_t **shader, 
					 int *fogNum, int *dlightMap, int *pshadowMap, qboolean *bStaticModel );

void R_AddDrawSurf( surfaceType_t *surface, shader_t *shader, 
				   int fogIndex, int dlightMap, int pshadowMap, int cubemap );

void R_CalcTexDirs(vec3_t sdir, vec3_t tdir, const vec3_t v1, const vec3_t v2,
				   const vec3_t v3, const vec2_t w1, const vec2_t w2, const vec2_t w3);
vec_t R_CalcTangentSpace(vec3_t tangent, vec3_t bitangent, const vec3_t normal, const vec3_t sdir, const vec3_t tdir);
qboolean R_CalcTangentVectors(srfVert_t * dv[3]);

#define	CULL_IN		0		// completely unclipped
#define	CULL_CLIP	1		// clipped by one or more planes
#define	CULL_OUT	2		// completely outside the clipping planes
void R_LocalNormalToWorld (const vec3_t local, vec3_t world);
void R_LocalPointToWorld (const vec3_t local, vec3_t world);
int R_CullBox (vec3_t bounds[2]);
int R_CullLocalBox (vec3_t bounds[2]);
int R_CullPointAndRadiusEx( const vec3_t origin, float radius, const cplane_t* frustum, int numPlanes );
int R_CullPointAndRadius( const vec3_t origin, float radius );
int R_CullLocalPointAndRadius( const vec3_t origin, float radius );

// HZM gl2 real character shadows: single predicate for "the r_charShadows feature is
// live this frame". Used to force the sun-shadow chain on without depending on the
// user's archived r_depthPrepass, and to gate every behavioural change in the feature.
// Returns qfalse whenever r_charShadows is 0, which is the default.
qboolean R_CharShadowsActive( void );

// HZM gl2 dynamic-light cast shadows: single predicate for "the r_hzmDlightShadows
// feature is live this frame". Returns qfalse whenever r_hzmDlightShadows is 0, which
// is the default, and whenever the FBO path (which the pshadow depth targets need) is
// unavailable. Every guard for the feature goes through this one function.
qboolean R_DlightShadowsActive( void );

// HZM gl2 dynamic-light cast shadows: front-end pass. Picks the few most important
// scene dlights, finds the entities they can cast, fills tr.refdef.pshadows[] and
// renders one 512x512 depth map per shadow. Called from RE_RenderScene BEFORE the main
// R_RenderView, so tr_world.c can hand out the pshadow bits for this frame.
void R_RenderDlightShadowMaps(const refdef_t *fd);

void R_SetupProjection(viewParms_t *dest, float zProj, float zFar, qboolean computeFrustum);
void R_RotateForEntity( const trRefEntity_t *ent, const viewParms_t *viewParms, orientationr_t *or );

/*
** GL wrapper/helper functions
*/
void	GL_BindToTMU( image_t *image, int tmu );
void	GL_SetDefaultState (void);
void	GL_TextureMode( const char *string );
void	GL_CheckErrs( char *file, int line );
#define GL_CheckErrors(...) GL_CheckErrs(__FILE__, __LINE__)
void	GL_State( unsigned long stateVector );
void    GL_SetProjectionMatrix(mat4_t matrix);
void    GL_SetModelviewMatrix(mat4_t matrix);
void	GL_Cull( int cullType );

#define GLS_SRCBLEND_ZERO						0x00000001
#define GLS_SRCBLEND_ONE						0x00000002
#define GLS_SRCBLEND_DST_COLOR					0x00000003
#define GLS_SRCBLEND_ONE_MINUS_DST_COLOR		0x00000004
#define GLS_SRCBLEND_SRC_ALPHA					0x00000005
#define GLS_SRCBLEND_ONE_MINUS_SRC_ALPHA		0x00000006
#define GLS_SRCBLEND_DST_ALPHA					0x00000007
#define GLS_SRCBLEND_ONE_MINUS_DST_ALPHA		0x00000008
#define GLS_SRCBLEND_ALPHA_SATURATE				0x00000009
#define		GLS_SRCBLEND_BITS					0x0000000f

#define GLS_DSTBLEND_ZERO						0x00000010
#define GLS_DSTBLEND_ONE						0x00000020
#define GLS_DSTBLEND_SRC_COLOR					0x00000030
#define GLS_DSTBLEND_ONE_MINUS_SRC_COLOR		0x00000040
#define GLS_DSTBLEND_SRC_ALPHA					0x00000050
#define GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA		0x00000060
#define GLS_DSTBLEND_DST_ALPHA					0x00000070
#define GLS_DSTBLEND_ONE_MINUS_DST_ALPHA		0x00000080
#define		GLS_DSTBLEND_BITS					0x000000f0

#define GLS_DEPTHMASK_TRUE						0x00000100

#define GLS_POLYMODE_LINE						0x00001000

#define GLS_DEPTHTEST_DISABLE					0x00010000
#define GLS_DEPTHFUNC_EQUAL						0x00020000
#define GLS_DEPTHFUNC_GREATER                   0x00040000
#define GLS_DEPTHFUNC_BITS                      0x00060000

// HZM gl2 MSAA (plan P2a). Two free bits (between DEPTHMASK 0x100 and POLYMODE 0x1000), neither part of any
// existing mask, so nothing that builds or compares state words today can see them; GL_State handles both.
// ALWAYS is a bit of its own because both DEPTHFUNC bits together (0x60000) decode as EQUAL in GL_State (ME-F5).
// Set only by the MSAA code (P2b depth resolve, P4a alpha-to-coverage).
#define GLS_ALPHA_TO_COVERAGE                   0x00000200
#define GLS_DEPTHFUNC_ALWAYS                    0x00000400

#define GLS_ATEST_GT_0							0x10000000
#define GLS_ATEST_LT_80							0x20000000
#define GLS_ATEST_GE_80							0x40000000
// HZM gl2 re-port (bug-gl2-foliage): MOHAA foliage alpha-test modes (gl1 parity).
// gl2's ATEST field is compared with == against the full GLS_ATEST_BITS mask, so
// the remaining enumerated values of the 3-bit field are free for these.
#define GLS_ATEST_LT_FOLIAGE1					0x30000000
#define GLS_ATEST_GE_FOLIAGE1					0x50000000
#define GLS_ATEST_LT_FOLIAGE2					0x60000000
#define GLS_ATEST_GE_FOLIAGE2					0x70000000
#define		GLS_ATEST_BITS						0x70000000

#define GLS_DEFAULT			GLS_DEPTHMASK_TRUE

void	RE_StretchRaw (int x, int y, int w, int h, int cols, int rows, const byte *data, int client, qboolean dirty);
void	RE_UploadCinematic (int w, int h, int cols, int rows, const byte *data, int client, qboolean dirty);

void		RE_BeginFrame( stereoFrame_t stereoFrame );
void		RE_BeginRegistration( glconfig_t *glconfig );
void		RE_LoadWorldMap( const char *mapname );
void		RE_SetWorldVisData( const byte *vis );
qhandle_t	RE_RegisterModel( const char *name );
qhandle_t	RE_RegisterSkin( const char *name );
void		RE_Shutdown( qboolean destroyWindow );

qboolean	R_GetEntityToken( char *buffer, int size );

model_t		*R_AllocModel( void );

void    	R_Init( void );
void		R_UpdateSubImage( image_t *image, byte *pic, int x, int y, int width, int height, GLenum picFormat );

void		R_SetColorMappings( void );
void		R_GammaCorrect( byte *buffer, int bufSize );

void	R_ImageList_f( void );
void	R_SkinList_f( void );
// https://zerowing.idsoftware.com/bugzilla/show_bug.cgi?id=516
const void *RB_TakeScreenshotCmd( const void *data );
void	R_ScreenShot_f( void );

void	R_InitFogTable( void );
float	R_FogFactor( float s, float t );
void	R_InitImages( void );
void	R_DeleteTextures( void );

// HZM gl2 - targeted generated normal maps (tr_image.c)
qboolean	R_HZM_GenNormalsWanted( const char *name );
qboolean	R_HZM_IsGeneratedNormal( const image_t *img );
void		R_HZM_GenNormalsReset( void );

int		R_SumOfUsedImages( void );
void	R_InitSkins( void );
skin_t	*R_GetSkinByHandle( qhandle_t hSkin );

int R_ComputeLOD( trRefEntity_t *ent );

const void *RB_TakeVideoFrameCmd( const void *data );

//
// tr_shader.c
//
shader_t	*R_FindShader( const char *name, int lightmapIndex, qboolean mipRawImage );
shader_t	*R_FindShaderEx( const char *name, int lightmapIndex, qboolean mipRawImage, int realLightmapIndex );
shader_t	*R_GetShaderByHandle( qhandle_t hShader );
shader_t	*R_GetShaderByState( int index, long *cycleTime );
shader_t *R_FindShaderByName( const char *name );
void		R_InitShaders( void );
void		R_ShaderList_f( void );
void    R_RemapShader(const char *oldShader, const char *newShader, const char *timeOffset);

//
// OPENMOHAA-specific stuff
//=========================

#define GLS_MULTITEXTURE						0x00004000
#define GLS_MULTITEXTURE_ENV					0x00008000
#define GLS_FOG									0x00080000

#define GLS_FOG_ENABLED							0x00100000
#define GLS_FOG_BLACK							0x00200000
#define GLS_FOG_WHITE							0x00400000
#define GLS_FOG_COLOR							(GLS_FOG_BLACK | GLS_FOG_WHITE)
#define GLS_FOG_BITS							(GLS_FOG_WHITE|GLS_FOG_BLACK|GLS_FOG_ENABLED)
#define GLS_COLOR_NOMASK						0x00800000

void R_RotateForStaticModel(cStaticModelUnpacked_t* SM, const viewParms_t* viewParms, orientationr_t* ori);
void R_RotateForViewer(void);
void R_SetupFrustum(void);
int R_DistanceCullLocalPointAndRadius(float fDist, const vec3_t pt, float radius);
int R_DistanceCullPointAndRadius(float fDist, const vec3_t pt, float radius);
qboolean R_ImageExists(const char* name);
int R_CountTextureMemory();
qboolean R_LoadRawImage(const char *name, byte **pic, int *width, int *height);
void R_FreeRawImage(byte *pic);

// HZM coop - gore tier 4 (UV wounds) - tr_gore.c
// HZM gl2 re-port (bug-gl2-gore), mirrors gl1 tr_local.h:2294-2304
void RE_GoreImpact(const vec3_t vStart, const vec3_t vEnd); // exported to cgame (bullet segment)
void RE_GoreReset(int entityNumber);                        // exported to cgame (entity fresh again)
// HZM coop - ragdoll (tr_ragdoll.cpp, gl2 lockstep)
struct ragdollSlot_s *R_RagdollSlotFor(int entityNumber, dtiki_t *tiki);
void R_RagdollApplyToCache(struct ragdollSlot_s *slot, skelBoneCache_t *cache, int num_tags, struct skelAnimFrame_s *newFrame);
qboolean R_RagdollGetOrientation(int entityNumber, dtiki_t *tiki, int tagnum, float scale, orientation_t *out);
void RE_SetRagdollPose(int entityNumber, dtiki_t *tiki, int count, const float *mat34, const vec3_t mins, const vec3_t maxs);
void RE_ClearRagdoll(int entityNumber);
void RE_ClearAllRagdolls(void);
void RE_GoreKillSplash(int entityNumber);                   // exported to cgame (killing blow - bug-780)
void R_GoreSkelSurfaceCheck(int baseVertex, int baseIndex); // RB_SkelMesh tail: ray-test skinned tris
image_t *R_GoreOverrideImage(image_t *image);               // R_BindAnimatedImageToTMU: swap in wound copy
void R_GoreCommitPending(void);                             // RE_EndFrame: stamp + upload
void R_GoreLevelReset(void);                                // RE_BeginRegistration
void R_GoreShutdown(void);                                  // RE_Shutdown (before R_DeleteTextures)

//
// tr_bsp.c
//
void		RE_PrintBSPFileSizes(void);
int			RE_MapVersion(void);

//
// tr_cmds.c
//
void R_SavePerformanceCounters(void);

//
// tr_main.c
//

void R_AddSpriteSurfaces();
qboolean SurfIsOffscreen2(const srfBspSurface_t* surface, shader_t* shader, int entityNum);

//
// tr_shader.c
//
qhandle_t		RE_RefreshShaderNoMip(const char* name);
//=========================


/*
====================================================================

IMPLEMENTATION SPECIFIC FUNCTIONS

====================================================================
*/

void		GLimp_InitExtraExtensions( void );

/*
====================================================================

TESSELATOR/SHADER DECLARATIONS

====================================================================
*/

typedef struct stageVars
{
	color4ub_t	colors[SHADER_MAX_VERTEXES];
	vec2_t		texcoords[NUM_TEXTURE_BUNDLES][SHADER_MAX_VERTEXES];
} stageVars_t;

typedef struct shaderCommands_s 
{
	glIndex_t	indexes[SHADER_MAX_INDEXES] QALIGN(16);
	vec4_t		xyz[SHADER_MAX_VERTEXES] QALIGN(16);
	int16_t		normal[SHADER_MAX_VERTEXES][4] QALIGN(16);
	int16_t		tangent[SHADER_MAX_VERTEXES][4] QALIGN(16);
	vec2_t		texCoords[SHADER_MAX_VERTEXES] QALIGN(16);
	vec2_t		lightCoords[SHADER_MAX_VERTEXES] QALIGN(16);
	uint16_t	color[SHADER_MAX_VERTEXES][4] QALIGN(16);
	int16_t		lightdir[SHADER_MAX_VERTEXES][4] QALIGN(16);
	//int			vertexDlightBits[SHADER_MAX_VERTEXES] QALIGN(16);

	void *attribPointers[ATTR_INDEX_COUNT];
	vao_t       *vao;
	qboolean    useInternalVao;
	qboolean    useCacheVao;

	stageVars_t	svars QALIGN(16);

	//color4ub_t	constantColor255[SHADER_MAX_VERTEXES] QALIGN(16);

	shader_t	*shader;
	double		shaderTime;
	int			fogNum;
	int         cubemapIndex;

	int			dlightBits;	// or together of all vertexDlightBits
	int         pshadowBits;

	int			firstIndex;
	int			numIndexes;
	int			numVertexes;

	// info extracted from current shader
	int			numPasses;
	void		(*currentStageIteratorFunc)( void );
	shaderStage_t	**xstages;

	//
	// OPENMOHAA-specific stuff
	//

    qboolean no_global_fog;
} shaderCommands_t;

extern	shaderCommands_t	tess;

void RB_BeginSurface(shader_t *shader, int fogNum, int cubemapIndex );
void RB_EndSurface(void);
void RB_CheckOverflow( int verts, int indexes );
#define RB_CHECKOVERFLOW(v,i) if (tess.numVertexes + (v) >= SHADER_MAX_VERTEXES || tess.numIndexes + (i) >= SHADER_MAX_INDEXES ) {RB_CheckOverflow(v,i);}

void R_DrawElements( int numIndexes, int firstIndex );
void RB_StageIteratorGeneric( void );
void RB_StageIteratorSky( void );
void RB_StageIteratorVertexLitTexture( void );
void RB_StageIteratorLightmappedMultitexture( void );

void RB_AddQuadStamp( vec3_t origin, vec3_t left, vec3_t up, float color[4] );
void RB_AddQuadStampExt( vec3_t origin, vec3_t left, vec3_t up, float color[4], float s1, float t1, float s2, float t2 );
void RB_InstantQuad( vec4_t quadVerts[4] );
//void RB_InstantQuad2(vec4_t quadVerts[4], vec2_t texCoords[4], vec4_t color, shaderProgram_t *sp, vec2_t invTexRes);
void RB_InstantQuad2(vec4_t quadVerts[4], vec2_t texCoords[4]);

void RB_ShowImages( void );


/*
============================================================

WORLD MAP

============================================================
*/

void R_AddBrushModelSurfaces(trRefEntity_t* e);
void R_AddWorldSurfaces(void);
qboolean R_inPVS( const vec3_t p1, const vec3_t p2 );
mnode_t* R_PointInLeaf(const vec3_t p);


/*
============================================================

FLARES

============================================================
*/

void R_ClearFlares( void );

void RB_AddFlare( void *surface, int fogNum, vec3_t point, vec3_t color, vec3_t normal );
void RB_AddDlightFlares( void );
void RB_RenderFlares (void);

/*
============================================================

LIGHTS

============================================================
*/

void R_DlightBmodel( bmodel_t *bmodel );
void R_SetupEntityLighting( const trRefdef_t *refdef, trRefEntity_t *ent );
void R_TransformDlights( int count, dlight_t *dl, orientationr_t *or );
int R_LightForPoint( vec3_t point, vec3_t ambientLight, vec3_t directedLight, vec3_t lightDir );
int R_LightDirForPoint( vec3_t point, vec3_t lightDir, vec3_t normal, world_t *world );
// HZM [bug-3064] what R_LightDirForPoint summed (tr_light.c), for the hzmtlprobe console command
typedef struct { int leafOk, sunList, sunHit, numLights; float sunWeight; vec3_t lampSum; } hzmLDirInfo_t;
int R_LightDirForPointInfo( vec3_t point, vec3_t lightDir, vec3_t normal, world_t *world, qboolean useAreaMask, hzmLDirInfo_t *info );
void R_HZM_TerrainLProbe_f( void ); // HZM [bug-3064] tr_surface.c: `hzmtlprobe <tag>`
int R_LightDirForPointStatic( vec3_t point, vec3_t lightDir, vec3_t normal, world_t *world ); // HZM [bug-3007] no areamask gate
int R_CubemapForPoint( vec3_t point );

/*
============================================================

SHADOWS

============================================================
*/

void RB_ShadowTessEnd( void );
void RB_ShadowFinish( void );
void RB_ProjectionShadowDeform( void );

/*
============================================================

SKIES

============================================================
*/

void R_BuildCloudData( shaderCommands_t *shader );
void R_InitSkyTexCoords( float cloudLayerHeight );
void R_DrawSkyBox( shaderCommands_t *shader );
void RB_DrawSun( float scale, shader_t *shader );
void RB_ClipSkyPolygons( shaderCommands_t *shader );

/*
============================================================

CURVE TESSELATION

============================================================
*/

#define PATCH_STITCHING

void R_SubdividePatchToGrid( srfBspSurface_t *grid, int width, int height,
								srfVert_t points[MAX_PATCH_SIZE*MAX_PATCH_SIZE] );
void R_GridInsertColumn( srfBspSurface_t *grid, int column, int row, vec3_t point, float loderror );
void R_GridInsertRow( srfBspSurface_t *grid, int row, int column, vec3_t point, float loderror );

/*
============================================================

MARKERS, POLYGON PROJECTION ON WORLD POLYGONS

============================================================
*/

int R_MarkFragments(int numPoints, const vec3_t *points, const vec3_t projection,
				   int maxPoints, vec3_t pointBuffer, int maxFragments, markFragment_t *fragmentBuffer, float fRadiusSquared);

int R_MarkFragmentsForInlineModel(clipHandle_t bmodel, const vec3_t vAngles, const vec3_t vOrigin, int numPoints,
	const vec3_t* points, const vec3_t projection, int maxPoints, vec3_t pointBuffer,
	int maxFragments, markFragment_t* fragmentBuffer, float fRadiusSquared);


/*
============================================================

VERTEX BUFFER OBJECTS

============================================================
*/

void R_VaoPackTangent(int16_t *out, vec4_t v);
void R_VaoPackNormal(int16_t *out, vec3_t v);
void R_VaoPackColor(uint16_t *out, vec4_t c);
void R_VaoUnpackTangent(vec4_t v, int16_t *pack);
void R_VaoUnpackNormal(vec3_t v, int16_t *pack);

vao_t          *R_CreateVao(const char *name, byte *vertexes, int vertexesSize, byte *indexes, int indexesSize, vaoUsage_t usage);
vao_t          *R_CreateVao2(const char *name, int numVertexes, srfVert_t *verts, int numIndexes, glIndex_t *inIndexes);

void            R_BindVao(vao_t *vao);
void            R_BindNullVao(void);

void Vao_SetVertexPointers(vao_t *vao);

void            R_InitVaos(void);
void            R_ShutdownVaos(void);
void            R_VaoList_f(void);

void            RB_UpdateTessVao(unsigned int attribBits);

void VaoCache_Commit(void);
void VaoCache_DrawElements(int numIndexes, int firstIndex);
void VaoCache_Init(void);
void VaoCache_BindVao(void);
void VaoCache_CheckAdd(qboolean *endSurface, qboolean *recycleVertexBuffer, qboolean *recycleIndexBuffer, int numVerts, int numIndexes);
void VaoCache_RecycleVertexBuffer(void);
void VaoCache_RecycleIndexBuffer(void);
void VaoCache_InitQueue(void);
void VaoCache_AddSurface(srfVert_t *verts, int numVerts, glIndex_t *indexes, int numIndexes);

/*
============================================================

GLSL

============================================================
*/

void GLSL_InitGPUShaders(void);
void GLSL_ShutdownGPUShaders(void);
void GLSL_VertexAttribPointers(uint32_t attribBits);
void GLSL_BindProgram(shaderProgram_t * program);

void GLSL_SetUniformInt(shaderProgram_t *program, int uniformNum, GLint value);
// HZM [UNDERWATER VOLUME v3] Read back the value an int uniform (in practice: a SAMPLER UNIT)
// was last set to. Returns -1 when the shader never declared that uniform. See the comment on
// the definition in tr_glsl.c - this exists so a pass can PROVE in one printed integer that its
// depth sampler is not silently pointing at texture unit 0.
int  GLSL_GetUniformIntValue(shaderProgram_t *program, int uniformNum);
void GLSL_SetUniformFloat(shaderProgram_t *program, int uniformNum, GLfloat value);
void GLSL_SetUniformFloat5(shaderProgram_t *program, int uniformNum, const vec5_t v);
void GLSL_SetUniformVec2(shaderProgram_t *program, int uniformNum, const vec2_t v);
void GLSL_SetUniformVec3(shaderProgram_t *program, int uniformNum, const vec3_t v);
void GLSL_SetUniformVec4(shaderProgram_t *program, int uniformNum, const vec4_t v);
void GLSL_SetUniformMat4(shaderProgram_t *program, int uniformNum, const mat4_t matrix);
void GLSL_SetUniformMat4BoneMatrix(shaderProgram_t *program, int uniformNum, /*const*/ mat4_t *matrix, int numMatricies);

shaderProgram_t *GLSL_GetGenericShaderProgram(int stage);
qboolean GLSL_HzmAlphaGenDotEnabled(void);   // HZM coop (bug-2508): r_hzmAlphaGenDot, default 1

/*
============================================================

SCENE GENERATION

============================================================
*/

void R_InitNextFrame( void );

void RE_ClearScene( void );
void RE_AddRefEntityToScene( const refEntity_t *ent );
void RE_AddPolyToScene( qhandle_t hShader , int numVerts, const polyVert_t *verts, int num );
void RE_AddLightToScene( const vec3_t org, float intensity, float r, float g, float b );
void RE_AddAdditiveLightToScene( const vec3_t org, float intensity, float r, float g, float b );
void RE_BeginScene( const refdef_t *fd );
void RE_RenderScene( const refdef_t *fd );
void RE_EndScene(void);
void RE_AddRefEntityToScene2(const refEntity_t* ent, int parentEntityNumber);

/*
=============================================================

UNCOMPRESSING BONES

=============================================================
*/

#define MC_BITS_X (16)
#define MC_BITS_Y (16)
#define MC_BITS_Z (16)
#define MC_BITS_VECT (16)

#define MC_SCALE_X (1.0f/64)
#define MC_SCALE_Y (1.0f/64)
#define MC_SCALE_Z (1.0f/64)

void MC_UnCompress(float mat[3][4],const unsigned char * comp);

/*
=============================================================

ANIMATED MODELS

=============================================================
*/

void R_MDRAddAnimSurfaces( trRefEntity_t *ent );
void RB_MDRSurfaceAnim( mdrSurface_t *surface );
qboolean R_LoadIQM (model_t *mod, void *buffer, int filesize, const char *name );
void R_AddIQMSurfaces( trRefEntity_t *ent );
void RB_IQMSurfaceAnim( surfaceType_t *surface );
void RB_IQMSurfaceAnimVao( srfVaoIQModel_t *surface );
int R_IQMLerpTag( orientation_t *tag, iqmData_t *data,
                  int startFrame, int endFrame,
                  float frac, const char *tagName );

//
// OPENMOHAA-specific stuff
//=========================

/*
=============================================================

DRAWING

=============================================================
*/

void Draw_SetColor(const vec4_t rgba);
void Draw_StretchPic(float x, float y, float w, float h, float s1, float t1, float s2, float t2, qhandle_t hShader);
void Draw_StretchPic2(float x, float y, float w, float h, float s1, float t1, float s2, float t2, float sx, float sy, qhandle_t hShader);
void Draw_TilePic(float x, float y, float w, float h, qhandle_t hShader);
void Draw_TilePicOffset(float x, float y, float w, float h, qhandle_t hShader, int offsetX, int offsetY);
// HZM gl2 (bug #73 ghost-gun-over-menus): sceneless-frame stale-FBO clear; flag set by
// RB_DrawSurfs, reset by RB_SwapBuffers, consumed by every immediate 2D entry point.
void R_Ensure2DClear(void);
void R_BindRenderFbo(void);
extern int g_sceneThisFrame;
void Draw_TrianglePic(const vec2_t vPoints[3], const vec2_t vTexCoords[3], qhandle_t hShader);
void DrawBox(float x, float y, float w, float h);
void AddBox(float x, float y, float w, float h);
void Set2DWindow(int x, int y, int w, int h, float left, float right, float bottom, float top, float n, float f);
void RE_Scissor(int x, int y, int width, int height);
void DrawLineLoop(const vec2_t* points, int count, int stipple_factor, int stipple_mask);
void RE_StretchRaw2(int x, int y, int w, int h, int cols, int rows, int components, const byte* data);

/*
=============================================================

FONT

=============================================================
*/
void R_ShutdownFont();
fontheader_t* R_LoadFont(const char* name);
void R_LoadFontShader(fontheader_sgl_t* font);
void R_DrawString(fontheader_t* font, const char* text, float x, float y, int maxlen, const float *pvVirtualScreen);
void R_DrawFloatingString(fontheader_t* font, const char* text, const vec3_t org, const vec4_t color, float scale, int maxlen);
float R_GetFontHeight(const fontheader_t* font);
float R_GetFontStringWidth(const fontheader_t* font, const char* s);

/*
=============================================================

GHOST

=============================================================
*/

void R_UpdateGhostTextures();
void R_SetGhostImage(const char* name, image_t* image);
void LoadGHOST(const char* name, byte** pic, int* width, int* height);

/*
============================================================

LIGHTS

============================================================
*/

void R_GetLightingForDecal(vec3_t vLight, const vec3_t vFacing, const vec3_t vOrigin);
void R_GetLightingForSmoke(vec3_t vLight, const vec3_t vOrigin);
void R_GetLightingGridValue(world_t* world, const vec3_t vPos, vec3_t vAmbientLight, vec3_t vDirectedLight);
// HZM gl2 re-port (bug-gl2-modellight): gl1's single-color grid sampler
// (gl1 tr_light.c R_GetLightingGridValue) - ambient+directed folded into one
// value, used by entity/static-model grid lighting and sphere ambient setup
void R_GetLightingGridValueSingle(world_t* world, const vec3_t vPos, vec3_t vLight);
void RB_SetupEntityGridLighting();
void RB_SetupStaticModelGridLighting(trRefdef_t* refdef, cStaticModelUnpacked_t* ent, const vec3_t lightOrigin);

void RB_Light_Real(unsigned char* colors);
void RB_Sphere_BuildDLights();
void RB_Sphere_SetupEntity();
void RB_Grid_SetupEntity();
void RB_Grid_SetupStaticModel();
void RB_Light_Fullbright(unsigned char* colors);
void R_Sphere_InitLights();
int R_GatherLightSources(const vec3_t vPos, vec3_t* pvLightPos, vec3_t* pvLightIntensity, int iMaxLights);

extern suninfo_t s_sun;

/*
=============================================================

MARKS

=============================================================
*/
void R_LevelMarksLoad(const char* szBSPName);
void R_LevelMarksInit();
void R_LevelMarksFree();
void R_UpdateLevelMarksSystem();
void R_AddPermanentMarkFragmentSurfaces(void** pFirstMarkFragment, int iNumMarkFragment);

/*
============================================================

SCENE

============================================================
*/

void RE_AddRefSpriteToScene(const refEntity_t* ent);
void RE_AddTerrainMarkToScene(int iTerrainIndex, qhandle_t hShader, int numVerts, const polyVert_t* verts, int renderfx);
refEntity_t* RE_GetRenderEntity(int entityNumber);
qboolean RE_AddPolyToScene2(qhandle_t hShader, int numVerts, const polyVert_t* verts, int renderfx);
void RE_AddLightToScene2(const vec3_t org, float intensity, float r, float g, float b, int type);

/*
============================================================

SHADE CALC

============================================================
*/

void RB_CalcLightGridColor(unsigned char* colors);

/*
=============================================================

SKY PORTALS

=============================================================
*/
void R_Sky_Init();
void R_Sky_Reset();
void R_Sky_AddSurf(msurface_t* surf);
void R_Sky_Render();

/*
=============================================================

SPRITE

=============================================================
*/
sprite_t* SPR_RegisterSprite(const char* name);
void RB_DrawSprite(const refSprite_t* spr);


/*
=============================================================

SUN FLARE

=============================================================
*/
void R_InitLensFlare();
void R_DrawLensFlares();

/*
=============================================================

SWIPE

=============================================================
*/
void RB_DrawSwipeSurface(surfaceType_t* pswipe);
void RE_SwipeBegin(float thistime, float life, qhandle_t shader);
void RE_SwipeEnd();
void R_AddSwipeSurfaces();

/*
=============================================================

TERRAIN

=============================================================
*/

extern terraTri_t* g_pTris;
extern terrainVert_t* g_pVert;

void R_MarkTerrainPatch(cTerraPatchUnpacked_t* pPatch);
void R_AddTerrainSurfaces();
void R_AddTerrainMarkSurfaces();
void R_InitTerrain();
void R_HZM_TerrainLightGridLoad( world_t *w ); // HZM [bug-3007] tr_surface.c: reserve (+ build) the terrain light grid
qboolean R_HZM_TerrainLightGridActive( void ); // HZM [bug-3007] tr_surface.c: per-vertex terrain L drawn now (never builds)
void R_ShutdownTerrain();
void R_TerrainFree();
void R_TerrainPrepareFrame();
qboolean R_TerrainHeightForPoly(cTerraPatchUnpacked_t* pPatch, polyVert_t* pVerts, int nVerts);
void R_SwapTerraPatch(cTerraPatch_t* pPatch);

void R_TerrainCrater_f(void);

/*
=============================================================

TIKI

=============================================================
*/
struct skelHeaderGame_s;
struct skelAnimFrame_s;

void R_InitStaticModels(void);
void RE_FreeModels(void);
qhandle_t RE_SpawnEffectModel(const char* szModel, vec3_t vPos, vec3_t* axis);
qhandle_t RE_RegisterServerModel(const char* name);
void RE_UnregisterServerModel(qhandle_t hModel);
orientation_t RE_TIKI_Orientation(refEntity_t* model, int tagnum);
qboolean RE_TIKI_IsOnGround(refEntity_t* model, int tagnum, float threshold);
float R_ModelRadius(qhandle_t handle);
void R_ModelBounds(qhandle_t handle, vec3_t mins, vec3_t maxs);
dtiki_t* R_Model_GetHandle(qhandle_t handle);

float R_GetRadius(refEntity_t* model);
void R_GetFrame(refEntity_t* model, struct skelAnimFrame_s* newFrame);
void RE_ForceUpdatePose(refEntity_t* model);
void RE_SetFrameNumber(int frameNumber);
void R_UpdatePoseInternal(refEntity_t* model);
void RB_SkelMesh(skelSurfaceGame_t* sf);
void RB_StaticMesh(staticSurface_t* staticSurf);
void RB_Static_BuildDLights();
void R_InfoStaticModels_f(void);
void R_PrintInfoStaticModels();
void R_AddSkelSurfaces(trRefEntity_t* ent);
void R_AddStaticModelSurfaces(void);
void R_CountTikiLodTris(dtiki_t* tiki, float lodpercentage, int* render_tris, int* total_tris);
float R_CalcLod(const vec3_t origin, float radius);
int GetLodCutoff(struct skelHeaderGame_s* skelmodel, float lod_val, int renderfx);
int GetToolLodCutoff(struct skelHeaderGame_s* skelmodel, float lod_val);
void R_InfoWorldTris_f(void);
void R_PrintInfoWorldtris(void);
void R_DebugSkeleton(void);

extern int g_nStaticSurfaces;
extern qboolean g_bInfostaticmodels;
extern qboolean g_bInfoworldtris;

/*
=============================================================

UTIL

=============================================================
*/

void RB_StreamBegin(shader_t* shader);
void RB_StreamEnd(void);
void RB_StreamBeginDrawSurf(void);
void RB_StreamEndDrawSurf(void);
static void addTriangle(void);
void RB_Vertex3fv(vec3_t v);
void RB_Vertex3f(vec_t x, vec_t y, vec_t z);
void RB_Vertex2f(vec_t x, vec_t y);
void RB_Color4f(vec_t r, vec_t g, vec_t b, vec_t a);
void RB_Color3f(vec_t r, vec_t g, vec_t b);
void RB_Color3fv(vec3_t col);
void RB_Color4bv(unsigned char* colors);
void RB_Texcoord2f(float s, float t);
void RB_Texcoord2fv(vec2_t st);
void R_DrawDebugNumber(const vec3_t org, float number, float scale, float r, float g, float b, int precision);
void R_DebugRotatedBBox(const vec3_t org, const vec3_t ang, const vec3_t mins, const vec3_t maxs, float r, float g, float b, float alpha);
void R_DebugCircle(const vec3_t org, float radius, float r, float g, float b, float alpha, qboolean horizontal);
void R_DebugLine(const vec3_t start, const vec3_t end, float r, float g, float b, float alpha);
int RE_GetShaderWidth(qhandle_t hShader);
int RE_GetShaderHeight(qhandle_t hShader);
const char* RE_GetShaderName(qhandle_t hShader);
const char* RE_GetModelName(qhandle_t hModel);

/*
============================================================

WORLD MAP

============================================================
*/
void R_GetInlineModelBounds(int iIndex, vec3_t vMins, vec3_t vMaxs);
int R_SphereInLeafs(const vec3_t p, float r, mnode_t** nodes, int nMaxNodes);
mnode_t* R_PointInLeaf(const vec3_t p);
int R_CheckDlightTerrain(cTerraPatchUnpacked_t* surf, int dlightBits);

void R_AddSpriteSurfCmd(drawSurf_t* drawSurfs, int numDrawSurfs);

//=========================

/*
=============================================================
=============================================================
*/
void	R_TransformModelToClip( const vec3_t src, const float *modelMatrix, const float *projectionMatrix,
							vec4_t eye, vec4_t dst );
void	R_TransformClipToWindow( const vec4_t clip, const viewParms_t *view, vec4_t normalized, vec4_t window );

void	RB_DeformTessGeometry( void );

void	RB_CalcFogTexCoords( float *dstTexCoords );

void	RB_CalcScaleTexMatrix( const float scale[2], float *matrix );
void	RB_CalcScrollTexMatrix( const float scrollSpeed[2], float *matrix );
void	RB_CalcRotateTexMatrix( float degsPerSecond, float *matrix );
void RB_CalcTurbulentFactors( const waveForm_t *wf, float *amplitude, float *now );
void	RB_CalcTransformTexMatrix( const texModInfo_t *tmi, float *matrix  );
void	RB_CalcStretchTexMatrix( const waveForm_t *wf, float *matrix );
void	RB_CalcTransWaveTexMatrix( const waveForm_t *wf, float *matrix );   // HZM gl2 parity (bug-1242)
void	RB_CalcTransWaveTexMatrixT( const waveForm_t *wf, float *matrix );  // HZM gl2 parity (bug-1242)

void	RB_CalcModulateColorsByFog( unsigned char *dstColors );
float	RB_CalcWaveAlphaSingle( const waveForm_t *wf );
float	RB_CalcWaveColorSingle( const waveForm_t *wf );

/*
=============================================================

RENDERER BACK END FUNCTIONS

=============================================================
*/

void RB_ExecuteRenderCommands( const void *data );

/*
=============================================================

RENDERER BACK END COMMAND QUEUE

=============================================================
*/

#define	MAX_RENDER_COMMANDS	0x40000

typedef struct {
	byte	cmds[MAX_RENDER_COMMANDS];
	int		used;
} renderCommandList_t;

typedef struct {
	int		commandId;
	float	color[4];
} setColorCommand_t;

typedef struct {
	int		commandId;
	int		buffer;
} drawBufferCommand_t;

typedef struct {
	int		commandId;
	image_t	*image;
	int		width;
	int		height;
	void	*data;
} subImageCommand_t;

typedef struct {
	int		commandId;
} swapBuffersCommand_t;

typedef struct {
	int		commandId;
	int		buffer;
} endFrameCommand_t;

typedef struct {
	int		commandId;
	shader_t	*shader;
	float	x, y;
	float	w, h;
	float	s1, t1;
	float	s2, t2;
} stretchPicCommand_t;

typedef struct {
	int		commandId;
	trRefdef_t	refdef;
	viewParms_t	viewParms;
	drawSurf_t *drawSurfs;
	int		numDrawSurfs;
} drawSurfsCommand_t;

typedef struct {
	int commandId;
	int x;
	int y;
	int width;
	int height;
	char *fileName;
	qboolean jpeg;
} screenshotCommand_t;

typedef struct {
	int						commandId;
	int						width;
	int						height;
	byte					*captureBuffer;
	byte					*encodeBuffer;
	qboolean			motionJpeg;
} videoFrameCommand_t;

typedef struct
{
	int commandId;

	GLboolean rgba[4];
} colorMaskCommand_t;

typedef struct
{
	int commandId;
} clearDepthCommand_t;

typedef struct {
	int commandId;
	int map;
	int cubeSide;
} capShadowmapCommand_t;

typedef struct {
	int		commandId;
	trRefdef_t	refdef;
	viewParms_t	viewParms;
} postProcessCommand_t;

typedef struct {
	int commandId;
} exportCubemapsCommand_t;

typedef enum {
	RC_END_OF_LIST,
	RC_SET_COLOR,
	RC_STRETCH_PIC,
	RC_DRAW_SURFS,
	RC_DRAW_BUFFER,
	RC_SWAP_BUFFERS,
	RC_SCREENSHOT,
	RC_VIDEOFRAME,
	RC_COLORMASK,
	RC_CLEARDEPTH,
	RC_CAPSHADOWMAP,
	RC_POSTPROCESS,
	RC_EXPORT_CUBEMAPS,

	//
	// OPENMOHAA-specific stuff
	//

	RC_SPRITE_SURFS,
} renderCommand_t;


// these are sort of arbitrary limits.
// the limits apply to the sum of all scenes in a frame --
// the main view, all the 3D icons, etc
// HZM gl2 (bug-gl2-maxpolys, #maxpolys): gl2 was still at the STOCK Q3 values (600 /
// 3000) while gl1 was long ago raised to 32768 / 131072. The coop mod's dynamic-poly
// producers (bullet-hole MARKS + blood/decal polys, submitted every frame by the
// renderer-agnostic cgame via RE_AddPolyToScene2) are IDENTICAL for both renderers and
// bounded by cg_maxMarks + the 10s mark lifetime - gl1's larger buffer simply holds the
// sustained-combat load that gl2's tiny buffer dropped, spamming "Exceeded MAX POLYS"
// and thinning the decals after a lot of shooting. The counts reset per frame
// (R_InitNextFrame), so this is headroom, not a leak. Raised PAST gl1 (4x) for comfort;
// the user wants MORE blood, not less. r_maxpolys/r_maxpolyverts default + floor clamp
// to these, so the backEndData allocation (R_Init) grows to match (~17 MB, fine on
// modern HW). Gore/blood-skin wounds are a separate texture-paint system (tr_gore.c,
// GORE_MAX_INSTANCES/PENDING bounded) and submit ZERO scene polys, so they are unaffected.
#define	MAX_POLYS		131072
#define	MAX_POLYVERTS	524288
#define	MAX_TERMARKS	1024

// all of the information needed by the back end must be
// contained in a backEndData_t
typedef struct {
	drawSurf_t	drawSurfs[MAX_DRAWSURFS];
	dlight_t	dlights[MAX_DLIGHTS];
	trRefEntity_t	entities[MAX_REFENTITIES];
	srfPoly_t	*polys;//[MAX_POLYS];
	polyVert_t	*polyVerts;//[MAX_POLYVERTS];
	pshadow_t pshadows[MAX_CALC_PSHADOWS];
	renderCommandList_t	commands;

	//
	// OPENMOHAA-specific stuff
    //
    drawSurf_t  spriteSurfs[MAX_SPRITESURFS];
	srfMarkFragment_t* terMarks;
    // HZM (engine-limits audit): was the literal 2048, so raising MAX_SPRITES (the value
    // RE_AddRefSpriteToScene bounds-checks against) would have overrun this array silently.
    refSprite_t sprites[MAX_SPRITES];
    cStaticModelUnpacked_t* staticModels;
    byte* staticModelData;
} backEndData_t;

extern	int		max_polys;
extern	int		max_polyverts;

extern	backEndData_t	*backEndData;	// the second one may not be allocated


void *R_GetCommandBuffer( int bytes );
void RB_ExecuteRenderCommands( const void *data );

void R_IssuePendingRenderCommands( void );

void R_AddDrawSurfCmd( drawSurf_t *drawSurfs, int numDrawSurfs );
void R_AddCapShadowmapCmd( int dlight, int cubeSide );
void R_AddPostProcessCmd (void);

void RE_SetColor( const float *rgba );
void RE_StretchPic ( float x, float y, float w, float h, 
					  float s1, float t1, float s2, float t2, qhandle_t hShader );
void RE_BeginFrame( stereoFrame_t stereoFrame );
void RE_EndFrame( int *frontEndMsec, int *backEndMsec );
void RE_SaveJPG(char * filename, int quality, int image_width, int image_height,
                unsigned char *image_buffer, int padding);
size_t RE_SaveJPGToBuffer(byte *buffer, size_t bufSize, int quality,
		          int image_width, int image_height, byte *image_buffer, int padding);
void RE_TakeVideoFrame( int width, int height,
		byte *captureBuffer, byte *encodeBuffer, qboolean motionJpeg );

void R_ConvertTextureFormat( const byte *in, int width, int height, GLenum format, GLenum type, byte *out );

// HZM gl2 graphics probes (tr_gfxprobe.c) - MSAA / stable-shadow plan, phase P0. Inert at default:
// r_gfxProbe 0, r_glDebug 0, r_gfxLabel 0, r_shadowFitYaw 0, r_shadowFitOffset 0. Only the one-line
// ^~^~^ GFXBUILD stamp prints unconditionally, at every R_Init.
extern cvar_t *r_gfxProbe;
extern cvar_t *r_glDebug;
extern cvar_t *r_gfxLabel;
extern cvar_t *r_shadowFitYaw;
extern cvar_t *r_shadowFitOffset;
extern const char hzmGfxDefaultsMarker[];
enum { GFXVIEW_C0, GFXVIEW_C1, GFXVIEW_C2, GFXVIEW_C3, GFXVIEW_MAIN, GFXVIEW_DLS };
enum { GPUMARK_FRAME_BEGIN, GPUMARK_SUN_B, GPUMARK_SUN_E, GPUMARK_SHOTH_B, GPUMARK_SHOTH_E,
       GPUMARK_MAIN_B, GPUMARK_MAIN_E, GPUMARK_POST_B, GPUMARK_POST_E, GPUMARK_FRAME_END, GPUMARK_COUNT };
void R_GfxProbe_Register(void);
void R_GfxProbe_Reregister(void);
void R_GfxProbe_InitGL(void);
void R_GfxProbe_AfterInit(void);
void R_GfxProbe_Shutdown(void);
void R_GfxProbe_WorldLoaded(const char *name);
void R_GfxProbe_StaticOverflow(const char *model);
void R_GfxProbe_ViewBegin(int kind);
void R_GfxProbe_ViewEnd(int kind);
const refdef_t *R_GfxProbe_FitRefdef(const refdef_t *fd, refdef_t *scratch);
int  R_GfxProbe_FreezeTime(int t);
void R_GfxProbe_EndFrame(void);
void R_GfxProbe_GpuMark(int kind);
void R_GfxProbe_BackendFrameEnd(void);
void R_GfxLabelDraw(void);
int  R_StaticModelSurfCapacity(void);
void RB_SetGL2D(void);

// HZM gl2 shadow hardening (plan P1a, r_shadowHarden, flags 0, 0 before the flip). With it 0 every path
// below is byte-for-byte today's; with it 1:
//   - the drawsurf list stops at its capacity instead of wrapping over views already issued (drops are
//     counted into ^~^~^ SHADOWBUDGET), and views that run before the main view may fill only half of it;
//   - depth-only (VPF_DEPTHSHADOW) views stop ADDING surfaces their only pass would skip anyway, through the
//     same predicate RB_DepthFillSkip uses, so they cost no drawsurf and no static-model slot;
//   - the legacy far cascade is re-baked when a cvar that changes its casters changes (SE#4);
//   - FBO_Init clears the shadow maps and mask; r_FBufScale is scene-normalised (ME-F12).
extern cvar_t *r_shadowHarden;

// HZM gl2 stable sun shadows + B0 + E1 (plan P3, tr_hzm_sunstable.c - its header comment is the reference)
#define HZM_PSHADOWCHARGATE_AUTO 0   // r_hzmPshadowCharGate -1 means this (bug-3009 / C0 / E1)
extern cvar_t *r_shadowStable;
extern cvar_t *r_shadowSlopeBias;
extern cvar_t *r_hzmPshadowCharGate;
void     R_SunStable_Register(void);
void     R_SunStable_WorldLoaded(const char *name);
qboolean R_SunStable_Resolve(void);
qboolean R_SunStable_Frame(const refdef_t *fitFd);
qboolean R_SunStable_LegacyNeedsFar(void);
qboolean R_SunStable_StaticInWorld(void);
qboolean R_SunStable_FarIsW(void);
qboolean R_PshadowCharGate(void);
void     R_SunWorld_AddTerrainProxy(void);
staticSurface_t *R_SunWorld_StaticSlot(void);
qboolean R_SunWorld_StaticWanted(int staticIndex);
qboolean R_SunStable_IsFoliageStatic(int staticIndex);
void     RB_SunStable_Mask(vec4_t quadVerts[4], vec2_t texCoords[4], const vec4_t viewInfo);
void     RB_SunStable_Lightall(shaderProgram_t *sp, const shaderCommands_t *input);
void     R_GfxProbe_WBaked(void);
extern cvar_t *r_shadowFboDummy;
extern qboolean r_drawSurfNoWrap;   // latched from r_shadowHarden at the top of every RE_RenderScene
extern int      r_drawSurfCap;      // current add limit (MAX_DRAWSURFS, or half of it before the main view)
extern int      r_drawSurfDrops;    // surfaces refused this frame
void     R_DepthViewAllows(int viewFlags, int shadowCascade, qboolean *allowChars, qboolean *allowCutout);
qboolean R_DepthViewSkips(const shader_t *shader, const trRefEntity_t *entities, int entityNum,
                          qboolean allowChars, qboolean allowCutout);
qboolean R_DepthViewSkipsFrontend(const shader_t *shader);
qboolean R_ShadowFarInputsChanged(void);
void     R_GfxReadBuffer(GLenum mode);   // glReadBuffer, loaded on first use (not in the qgl table)

// HZM gl2 MSAA core (plan P2a, tr_msaa.c - the header comment there is the reference)
#ifndef GL_TEXTURE_2D_MULTISAMPLE
#define GL_TEXTURE_2D_MULTISAMPLE           0x9100
#endif
#ifndef GL_SAMPLE_ALPHA_TO_COVERAGE
#define GL_SAMPLE_ALPHA_TO_COVERAGE         0x809E
#endif
#ifndef GL_SAMPLE_ALPHA_TO_ONE
#define GL_SAMPLE_ALPHA_TO_ONE              0x809F
#endif
typedef void (APIENTRY *hzmTexImage2DMultisample_t)(GLenum target, GLsizei samples, GLenum internalformat,
                                                     GLsizei width, GLsizei height, GLboolean fixedsamplelocations);
typedef void (APIENTRY *hzmAlphaToCoverageDitherControlNV_t)(GLenum mode);
extern hzmTexImage2DMultisample_t          qglTexImage2DMultisample;          // NULL = the new MSAA path is unavailable
extern hzmAlphaToCoverageDitherControlNV_t qglAlphaToCoverageDitherControlNV; // NULL = no NV dither control (P4a)
extern cvar_t *r_msaaOverride;
extern cvar_t *r_msaaBypass;
// HZM gl2 MSAA P4 shader features (flags 0, default 1, NEVER archived - V3-C2): read per draw / at R_Init
extern cvar_t *r_alphaToCoverage;   // P4a, live
extern cvar_t *r_msaaCentroid;      // P4b, read at R_Init (GLSL defines)
extern cvar_t *r_msaaShadowMatch;   // P4c, live
int  R_HzmLightMatte(const byte *pic, int width, int height);
int  RB_MsaaA2CStageMode(const shaderStage_t *pStage);
void RB_MsaaShadowMatch(shaderProgram_t *sp);
extern cvar_t *r_msaaDebugFailCompile;
void        R_Msaa_Register(void);
qboolean    R_MsaaNewPathRequested(void);
void        R_DecideMsaa(void);
qboolean    R_MsaaInitSceneFbos(int hdrFormat);
void        R_MsaaBeginFrame(void);
qboolean    R_MsaaSceneIsMultisampled(void);
const char *R_MsaaLabel(void);
void        R_Msaa_AfterGLSL(void);
image_t    *R_CreateImageMS(const char *name, int width, int height, int internalFormat);   // tr_image.c
// HZM gl2 MSAA (plan P2b) shader resolves. All inert unless R_MsaaResolvesActive(): the new path at >= 2 samples, the
// HOME bypass off, and both resolve programs and targets present. Otherwise every call site keeps its blit / today.
typedef void (APIENTRY *hzmDrawBuffers_t)(GLsizei n, const GLenum *bufs);
typedef void (APIENTRY *hzmBindFragDataLocation_t)(GLuint program, GLuint color, const GLchar *name);
// HZM gl2 [bug-3483] tess stream: GL 3.2 glDrawElementsBaseVertex. NULL = the stream stays off (old path).
typedef void (APIENTRY *hzmDrawElementsBaseVertex_t)(GLenum mode, GLsizei count, GLenum type, const void *indices, GLint basevertex);
extern hzmDrawElementsBaseVertex_t qglDrawElementsBaseVertex;
qboolean R_TessStreamDraw(int numIndexes, int firstIndex);
extern hzmDrawBuffers_t          qglDrawBuffers;            // GL 2.0; NULL = no MRT colour resolve (blit fallback)
extern hzmBindFragDataLocation_t qglBindFragDataLocation;   // GL 3.0; NULL = no MRT colour resolve (blit fallback)
qboolean    R_MsaaResolvesActive(void);
void        RB_MSAAResolveDepth(void);
void        RB_MSAAResolveColor(qboolean withAO, int mode, const int *viewRect);
int         R_MsaaToneResolveMode(void);
FBO_t      *R_MsaaLinearSource(FBO_t *src);
image_t    *R_MsaaDepthMinMaxOr(image_t *fallback);
// renderer_reinit plan 6, rule 1: the flags-0 cvars tr_msaa.c reads ONLY at R_Init (NULL-terminated). A renderer
// kept across map loads must snapshot their modificationCount. The ROM outputs (r_msaaActive, r_gpuVramMB,
// r_msaaForcedOff) are deliberately absent; r_msaaBypass is live (read per frame).
extern const char *const hzmMsaaInitReadCvars[];
// HZM rain wetness - tr_hzm_wet.c (docs/proposals/water_wetness_2026-09-27)
void R_HZMWetBuild( dheader_t *header );
void R_HZMWetTagShader( shader_t *sh, int surfaceFlags );
void RB_HZMWetUniforms( shaderProgram_t *sp, const shaderCommands_t *input, const shaderStage_t *pStage, int stage,
                        qboolean charLit );
void RB_HZMWetOff( shaderProgram_t *sp );

// HZM ground variety - tr_hzm_groundvar.c (docs/proposals/ground_variety_2026-09-29)
void R_HZM_GroundVarWorldBegin( const char *baseName );
void R_HZM_GroundVarTagShader( shader_t *sh );
void RB_HZM_GroundVarUniforms( shaderProgram_t *sp, const shaderCommands_t *input, const shaderStage_t *pStage,
                               int stage, qboolean charLit );

// HZM water pass - tr_hzm_water.c
void R_HZMWaterBuild( void );
void RB_HZMWaterPass( int deformGen, const vec5_t deformParams );

#ifdef __cplusplus
}
#endif

#endif //TR_LOCAL_H
