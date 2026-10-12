/*
===========================================================================
Copyright (C) 2006-2009 Robert Beckebans <trebor_7@users.sourceforge.net>

This file is part of XreaL source code.

XreaL source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

XreaL source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with XreaL source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/
// tr_glsl.c
#include "tr_local.h"

#include "tr_dsa.h"

extern const char *fallbackShader_bokeh_vp;
extern const char *fallbackShader_bokeh_fp;
extern const char *fallbackShader_calclevels4x_vp;
extern const char *fallbackShader_calclevels4x_fp;
extern const char *fallbackShader_depthblur_vp;
extern const char *fallbackShader_depthblur_fp;
extern const char *fallbackShader_dlight_vp;
extern const char *fallbackShader_dlight_fp;
extern const char *fallbackShader_down4x_vp;
extern const char *fallbackShader_down4x_fp;
extern const char *fallbackShader_fogpass_vp;
extern const char *fallbackShader_fogpass_fp;
extern const char *fallbackShader_generic_vp;
extern const char *fallbackShader_generic_fp;
extern const char *fallbackShader_lightall_vp;
extern const char *fallbackShader_lightall_fp;
extern const char *fallbackShader_pshadow_vp;
extern const char *fallbackShader_pshadow_fp;
extern const char *fallbackShader_shadowfill_vp;
extern const char *fallbackShader_shadowfill_fp;
extern const char *fallbackShader_shadowmask_vp;
extern const char *fallbackShader_shadowmask_fp;
extern const char *fallbackShader_ssao_vp;
extern const char *fallbackShader_ssao_fp;
extern const char *fallbackShader_texturecolor_vp;
extern const char *fallbackShader_texturecolor_fp;
extern const char *fallbackShader_tonemap_vp;
extern const char *fallbackShader_tonemap_fp;
extern const char *fallbackShader_tonemap_hzm_fp;
extern const char *fallbackShader_bloom_bright_fp;
extern const char *fallbackShader_bloom_blur_fp;
extern const char *fallbackShader_fxaa_fp;
extern const char *fallbackShader_sharpen_fp;
extern const char *fallbackShader_raindrops_fp;
extern const char *fallbackShader_hzmwater_vp;      // HZM water pass (tr_hzm_water.c)
extern const char *fallbackShader_hzmwater_fp;
extern const char *fallbackShader_lowhealth_fp;
extern const char *fallbackShader_suppression_fp;
extern const char *fallbackShader_hitblood_fp;
extern const char *fallbackShader_heathaze_fp;
extern const char *fallbackShader_dof_fp;
extern const char *fallbackShader_underwater_fp;
// HZM coop [2026-09-27] realistic lightning sky pass (tr_hzm_lightning.c; compiled lazily on the first lit frame)
extern const char *fallbackShader_hzmlt_sky_vp;
extern const char *fallbackShader_hzmlt_sky_fp;
extern const char *fallbackShader_bloodspatter_fp;
extern const char *fallbackShader_chromab_fp;
extern const char *fallbackShader_motionblur_fp;
extern const char *fallbackShader_filmgrain_fp;
extern const char *fallbackShader_frost_fp;
extern const char *fallbackShader_globalfog_vp;
extern const char *fallbackShader_globalfog_fp;
extern const char *fallbackShader_fsr_easu_fp;      // HZM render scale: AMD FSR 1 EASU
extern const char *fallbackShader_fsr_rcas_fp;      // HZM render scale: AMD FSR 1 RCAS
extern const char *fallbackShader_fsr_downscale_fp; // HZM render scale: SSAA tent downsample
extern const char *fallbackShader_msaa_depthresolve_fp; // HZM MSAA P2a: min/max depth resolve (P2b draws it)
extern const char *fallbackShader_msaa_colorresolve_fp; // HZM MSAA P2b: tone-exact MRT colour resolve

typedef struct uniformInfo_s
{
	char *name;
	int type;
}
uniformInfo_t;

// These must be in the same order as in uniform_t in tr_local.h.
static uniformInfo_t uniformsInfo[] =
{
	{ "u_DiffuseMap",  GLSL_INT },
	{ "u_LightMap",    GLSL_INT },
	{ "u_NormalMap",   GLSL_INT },
	{ "u_DeluxeMap",   GLSL_INT },
	{ "u_SpecularMap", GLSL_INT },

	{ "u_TextureMap", GLSL_INT },
	{ "u_LevelsMap",  GLSL_INT },
	{ "u_CubeMap",    GLSL_INT },

	{ "u_ScreenImageMap", GLSL_INT },
	{ "u_ScreenDepthMap", GLSL_INT },

	{ "u_ShadowMap",  GLSL_INT },
	{ "u_ShadowMap2", GLSL_INT },
	{ "u_ShadowMap3", GLSL_INT },
	{ "u_ShadowMap4", GLSL_INT },

	{ "u_ShadowMvp",  GLSL_MAT16 },
	{ "u_ShadowMvp2", GLSL_MAT16 },
	{ "u_ShadowMvp3", GLSL_MAT16 },
	{ "u_ShadowMvp4", GLSL_MAT16 },

	{ "u_EnableTextures", GLSL_VEC4 },

	{ "u_DiffuseTexMatrix0",  GLSL_VEC4 },
	{ "u_DiffuseTexMatrix1",  GLSL_VEC4 },
	{ "u_DiffuseTexMatrix2",  GLSL_VEC4 },
	{ "u_DiffuseTexMatrix3",  GLSL_VEC4 },
	{ "u_DiffuseTexMatrix4",  GLSL_VEC4 },
	{ "u_DiffuseTexMatrix5",  GLSL_VEC4 },
	{ "u_DiffuseTexMatrix6",  GLSL_VEC4 },
	{ "u_DiffuseTexMatrix7",  GLSL_VEC4 },

	{ "u_TCGen0",        GLSL_INT },
	{ "u_TCGen0Vector0", GLSL_VEC3 },
	{ "u_TCGen0Vector1", GLSL_VEC3 },

	{ "u_DeformGen",    GLSL_INT },
	{ "u_DeformParams", GLSL_FLOAT5 },

	{ "u_ColorGen",  GLSL_INT },
	{ "u_AlphaGen",  GLSL_INT },
	{ "u_Color",     GLSL_VEC4 },
	{ "u_BaseColor", GLSL_VEC4 },
	{ "u_VertColor", GLSL_VEC4 },

	{ "u_DlightInfo",    GLSL_VEC4 },
	{ "u_LightForward",  GLSL_VEC3 },
	{ "u_LightUp",       GLSL_VEC3 },
	{ "u_LightRight",    GLSL_VEC3 },
	{ "u_LightOrigin",   GLSL_VEC4 },
	{ "u_ModelLightDir", GLSL_VEC3 },
	{ "u_LightRadius",   GLSL_FLOAT },
	{ "u_AmbientLight",  GLSL_VEC3 },
	{ "u_DirectedLight", GLSL_VEC3 },

	{ "u_PortalRange", GLSL_FLOAT },

	{ "u_FogDistance",  GLSL_VEC4 },
	{ "u_FogDepth",     GLSL_VEC4 },
	{ "u_FogEyeT",      GLSL_FLOAT },
	{ "u_FogColorMask", GLSL_VEC4 },

	{ "u_ModelMatrix",               GLSL_MAT16 },
	{ "u_ModelViewProjectionMatrix", GLSL_MAT16 },

	{ "u_Time",          GLSL_FLOAT },
	{ "u_VertexLerp" ,   GLSL_FLOAT },
	{ "u_NormalScale",   GLSL_VEC4 },
	{ "u_SpecularScale", GLSL_VEC4 },

	{ "u_ViewInfo",        GLSL_VEC4 },
	{ "u_ViewOrigin",      GLSL_VEC3 },
	{ "u_LocalViewOrigin", GLSL_VEC3 },
	{ "u_ViewForward",     GLSL_VEC3 },
	{ "u_ViewLeft",        GLSL_VEC3 },
	{ "u_ViewUp",          GLSL_VEC3 },

	{ "u_InvTexRes",           GLSL_VEC2 },
	{ "u_HzmParams",           GLSL_VEC4 },   // HZM gl2 post-FX spare (bug-1151)
	{ "u_AutoExposureMinMax",  GLSL_VEC2 },
	{ "u_ToneMinAvgMaxLinear", GLSL_VEC3 },

	{ "u_PrimaryLightOrigin",  GLSL_VEC4  },
	{ "u_PrimaryLightColor",   GLSL_VEC3  },
	{ "u_PrimaryLightAmbient", GLSL_VEC3  },
	{ "u_PrimaryLightRadius",  GLSL_FLOAT },

	{ "u_CubeMapInfo", GLSL_VEC4 },

	{ "u_AlphaTest", GLSL_INT },

	{ "u_BoneMatrix", GLSL_MAT16_BONEMATRIX },

	// HZM gl2 re-port (bug-gl2-nextbundle2): generic second bundle
	// (must stay in sync with uniform_t in tr_local.h)
	{ "u_Texture1Env",     GLSL_INT },
	{ "u_Texture1TCGen",   GLSL_INT },
	{ "u_Texture1Matrix0", GLSL_VEC4 },
	{ "u_Texture1Matrix1", GLSL_VEC4 },
	{ "u_Texture1Matrix2", GLSL_VEC4 },
	{ "u_Texture1Matrix3", GLSL_VEC4 },
	{ "u_Texture1Matrix4", GLSL_VEC4 },
	{ "u_Texture1Matrix5", GLSL_VEC4 },
	{ "u_Texture1Matrix6", GLSL_VEC4 },
	{ "u_Texture1Matrix7", GLSL_VEC4 },

	// HZM gl2 parity (bug-1249) - see UNIFORM_ALPHAGENPARAMS; new rows go AFTER this one, in
	// exactly the order they were appended to uniform_t.
	{ "u_AlphaGenParams",  GLSL_VEC4 },

	// HZM gl2 FORWARD GLOBAL FOG (r_globalFogForward, bug-1306)
	{ "u_GlobalFogColor",  GLSL_VEC4 },
	{ "u_GlobalFogParams", GLSL_VEC4 },

	// HZM coop (bug-2508): alphaGen lightingSpecular light, u_LightOrigin convention (w 0 = dir)
	{ "u_HzmSpecLight",    GLSL_VEC4 },

	// HZM render-scale supersampling + AMD FSR 1 - see UNIFORM_FSRCON0. Kept LAST, in enum order.
	{ "u_FsrCon0",         GLSL_VEC4 },
	{ "u_FsrCon1",         GLSL_VEC4 },
	{ "u_FsrCon2",         GLSL_VEC4 },
	{ "u_FsrCon3",         GLSL_VEC4 },

	// HZM gl2 soft particles - see UNIFORM_SOFTPARTICLE. Kept LAST, in enum order.
	{ "u_SoftParticle",    GLSL_VEC4 },

	// HZM gl2 [2026-09-26] Phase S1 spot cone - see UNIFORM_HZMLIGHTSPOT. Kept LAST, in enum order.
	{ "u_HzmLightSpot",    GLSL_VEC4 },

	// HZM gl2 [2026-09-26] S4 spot shadows - see UNIFORM_HZMPSHADOWSPOT. Kept LAST, in enum order.
	{ "u_HzmPshadowSpot",  GLSL_VEC4 },
	// HZM rain wetness - see UNIFORM_HZMWET. Kept LAST, in enum order.
	{ "u_HzmWet",          GLSL_VEC4 },
	{ "u_HzmWetMat",       GLSL_VEC4 },
	{ "u_HzmWetOcc",       GLSL_VEC4 },
	{ "u_HzmWetOccZ",      GLSL_VEC4 },
	{ "u_HzmWetSkyZ",      GLSL_VEC4 },
	{ "u_HzmWetSkyH",      GLSL_VEC4 },
	{ "u_HzmWetSun",       GLSL_VEC4 },
	{ "u_HzmWetSunCol",    GLSL_VEC4 },
	{ "u_HzmOccMap",       GLSL_INT },
	{ "u_HzmWetNoise",     GLSL_INT },

	// HZM water pass - see UNIFORM_HZMWATER. Kept LAST, in enum order.
	{ "u_HzmWater",        GLSL_VEC4 },
	{ "u_HzmRippleMap",    GLSL_INT },
	// HZM ground variety - see UNIFORM_HZMGROUNDVAR. In enum order (after the water rows, not at the end).
	{ "u_HzmGroundVar",    GLSL_VEC4 },
	{ "u_HzmGroundVar2",   GLSL_VEC4 },
	// HZM coop [2026-09-27] realistic lightning - see UNIFORM_HZMLTFOG. Kept LAST, in enum order.
	{ "u_HzmLtFog",        GLSL_VEC4 },
	{ "u_HzmLtView",       GLSL_VEC4 },
	{ "u_HzmLtProj",       GLSL_VEC4 },
	{ "u_HzmLtWorld",      GLSL_VEC4 },
	{ "u_HzmLtBolt",       GLSL_VEC4 },
	{ "u_HzmLtParams",     GLSL_VEC4 },
	// HZM gl2 stable sun shadows + B0 (plan P3) - see UNIFORM_HZMSUNWORLDMAP. Kept LAST, in enum order.
	{ "u_HzmSunWorldMap",  GLSL_INT },
	{ "u_HzmSunWorldMvp",  GLSL_MAT16 },
	{ "u_HzmSunMaskOnly",  GLSL_VEC4 },
	{ "u_HzmShadowSplits", GLSL_VEC4 },
	{ "u_HzmShadowKernel", GLSL_VEC4 },
	{ "u_HzmShadowBias",   GLSL_VEC4 },
	{ "u_HzmShadowMatch",  GLSL_VEC4 },   // HZM gl2 MSAA P4c
	// HZM coop [2026-10-05] sky fog model - UNIFORM_HZMSKYFOG..UNIFORM_HZMSKYTAN, same order
	{ "u_HzmSkyFog",       GLSL_VEC4 },
	{ "u_HzmSkyVp",        GLSL_VEC4 },
	{ "u_HzmSkyTan",       GLSL_VEC4 },
};

// HZM gl2 [2026-09-26] vet_phaseS F10: this table is indexed by uniform_t and GLSL_InitUniforms walks it to
// UNIFORM_COUNT unguarded, and Phase S and the gfx P2/P4 work all append to both. A row count that disagrees with the
// enum is now a COMPILE error, instead of every later uniform silently resolving to the wrong name at run time.
// (docs/tools/glsl_variant_lint.py also checks the rows against the enum names, in order.)
typedef char hzm_uniformsInfo_rows_match_uniform_t[ ( sizeof( uniformsInfo ) / sizeof( uniformsInfo[0] ) == UNIFORM_COUNT ) ? 1 : -1 ];

typedef enum
{
	GLSL_PRINTLOG_PROGRAM_INFO,
	GLSL_PRINTLOG_SHADER_INFO,
	GLSL_PRINTLOG_SHADER_SOURCE
}
glslPrintLog_t;

static void GLSL_PrintLog(GLuint programOrShader, glslPrintLog_t type, qboolean developerOnly)
{
	char           *msg;
	static char     msgPart[1024];
	int             maxLength = 0;
	int             i;
	int             printLevel = developerOnly ? PRINT_DEVELOPER : PRINT_ALL;

	switch (type)
	{
		case GLSL_PRINTLOG_PROGRAM_INFO:
			ri.Printf(printLevel, "Program info log:\n");
			qglGetProgramiv(programOrShader, GL_INFO_LOG_LENGTH, &maxLength);
			break;

		case GLSL_PRINTLOG_SHADER_INFO:
			ri.Printf(printLevel, "Shader info log:\n");
			qglGetShaderiv(programOrShader, GL_INFO_LOG_LENGTH, &maxLength);
			break;

		case GLSL_PRINTLOG_SHADER_SOURCE:
			ri.Printf(printLevel, "Shader source:\n");
			qglGetShaderiv(programOrShader, GL_SHADER_SOURCE_LENGTH, &maxLength);
			break;
	}

	if (maxLength <= 0)
	{
		ri.Printf(printLevel, "None.\n");
		return;
	}

	if (maxLength < 1023)
		msg = msgPart;
	else
		msg = ri.Malloc(maxLength);

	switch (type)
	{
		case GLSL_PRINTLOG_PROGRAM_INFO:
			qglGetProgramInfoLog(programOrShader, maxLength, &maxLength, msg);
			break;

		case GLSL_PRINTLOG_SHADER_INFO:
			qglGetShaderInfoLog(programOrShader, maxLength, &maxLength, msg);
			break;

		case GLSL_PRINTLOG_SHADER_SOURCE:
			qglGetShaderSource(programOrShader, maxLength, &maxLength, msg);
			break;
	}

	if (maxLength < 1023)
	{
		msgPart[maxLength + 1] = '\0';

		ri.Printf(printLevel, "%s\n", msgPart);
	}
	else
	{
		for(i = 0; i < maxLength; i += 1023)
		{
			Q_strncpyz(msgPart, msg + i, sizeof(msgPart));

			ri.Printf(printLevel, "%s", msgPart);
		}

		ri.Printf(printLevel, "\n");

		ri.Free(msg);
	}

}

static void GLSL_GetShaderHeader( GLenum shaderType, const GLchar *extra, char *dest, int size )
{
	float fbufWidthScale, fbufHeightScale;

	dest[0] = '\0';

	// HACK: abuse the GLSL preprocessor to turn GLSL 1.20 shaders into 1.30 ones
	if(glRefConfig.glslMajorVersion > 1 || (glRefConfig.glslMajorVersion == 1 && glRefConfig.glslMinorVersion >= 30))
	{
		if (qglesMajorVersion >= 3 && glRefConfig.glslMajorVersion >= 3)
			Q_strcat(dest, size, "#version 300 es\n");
		else if (glRefConfig.glslMajorVersion > 1 || (glRefConfig.glslMajorVersion == 1 && glRefConfig.glslMinorVersion >= 50))
			Q_strcat(dest, size, "#version 150\n");
		else
			Q_strcat(dest, size, "#version 130\n");

		// `extra' may contain #extension which must be directly after #version
		if (extra)
		{
			Q_strcat(dest, size, extra);
		}

		if (qglesMajorVersion >= 2)
		{
			Q_strcat(dest, size, "precision mediump float;\n");
			Q_strcat(dest, size, "precision mediump sampler2DShadow;\n");
		}

		if(shaderType == GL_VERTEX_SHADER)
		{
			Q_strcat(dest, size, "#define attribute in\n");
			Q_strcat(dest, size, "#define varying out\n");
		}
		else
		{
			Q_strcat(dest, size, "#define varying in\n");

			Q_strcat(dest, size, "out vec4 out_Color;\n");
			Q_strcat(dest, size, "#define gl_FragColor out_Color\n");
			Q_strcat(dest, size, "#define texture2D texture\n");
			Q_strcat(dest, size, "#define textureCubeLod textureLod\n");
			Q_strcat(dest, size, "#define shadow2D texture\n");
		}
	}
	else
	{
		if (qglesMajorVersion >= 2)
		{
			Q_strcat(dest, size, "#version 100\n");

			if (extra)
			{
				Q_strcat(dest, size, extra);
			}

			Q_strcat(dest, size, "precision mediump float;\n");

			if (glRefConfig.shadowSamplers)
			{
				Q_strcat(dest, size, "precision mediump sampler2DShadow;\n");
				Q_strcat(dest, size, "#define shadow2D(a,b) shadow2DEXT(a,b)\n");
			}
		}
		else
		{
			Q_strcat(dest, size, "#version 120\n");

			if (extra)
			{
				Q_strcat(dest, size, extra);
			}

			Q_strcat(dest, size, "#define shadow2D(a,b) shadow2D(a,b).r\n");
		}
	}

	// HACK: add some macros to avoid extra uniforms and save speed and code maintenance
	//Q_strcat(dest, size,
	//		 va("#ifndef r_SpecularExponent\n#define r_SpecularExponent %f\n#endif\n", r_specularExponent->value));
	//Q_strcat(dest, size,
	//		 va("#ifndef r_SpecularScale\n#define r_SpecularScale %f\n#endif\n", r_specularScale->value));
	//Q_strcat(dest, size,
	//       va("#ifndef r_NormalScale\n#define r_NormalScale %f\n#endif\n", r_normalScale->value));


	Q_strcat(dest, size, "#ifndef M_PI\n#define M_PI 3.14159265358979323846\n#endif\n");

	//Q_strcat(dest, size, va("#ifndef MAX_SHADOWMAPS\n#define MAX_SHADOWMAPS %i\n#endif\n", MAX_SHADOWMAPS));

	Q_strcat(dest, size,
					 va("#ifndef deformGen_t\n"
						"#define deformGen_t\n"
						"#define DGEN_WAVE_SIN %i\n"
						"#define DGEN_WAVE_SQUARE %i\n"
						"#define DGEN_WAVE_TRIANGLE %i\n"
						"#define DGEN_WAVE_SAWTOOTH %i\n"
						"#define DGEN_WAVE_INVERSE_SAWTOOTH %i\n"
						"#define DGEN_BULGE %i\n"
						"#define DGEN_MOVE %i\n"
						"#endif\n",
						DGEN_WAVE_SIN,
						DGEN_WAVE_SQUARE,
						DGEN_WAVE_TRIANGLE,
						DGEN_WAVE_SAWTOOTH,
						DGEN_WAVE_INVERSE_SAWTOOTH,
						DGEN_BULGE,
						DGEN_MOVE));

	Q_strcat(dest, size,
					 va("#ifndef tcGen_t\n"
						"#define tcGen_t\n"
						"#define TCGEN_LIGHTMAP %i\n"
						"#define TCGEN_TEXTURE %i\n"
						"#define TCGEN_ENVIRONMENT_MAPPED %i\n"
						"#define TCGEN_ENVIRONMENT_MAPPED2 %i\n"
						"#define TCGEN_FOG %i\n"
						"#define TCGEN_VECTOR %i\n"
						"#endif\n",
						TCGEN_LIGHTMAP,
						TCGEN_TEXTURE,
						TCGEN_ENVIRONMENT_MAPPED,
						TCGEN_ENVIRONMENT_MAPPED2,
						TCGEN_FOG,
						TCGEN_VECTOR));

	// HZM gl2 [2026-09-25] Phase R2: CGEN_DOT / CGEN_ONE_MINUS_DOT for generic_vp CalcColor (rgbGen dot port)
	Q_strcat(dest, size,
					 va("#ifndef colorGen_t\n"
						"#define colorGen_t\n"
						"#define CGEN_LIGHTING_DIFFUSE %i\n"
						"#define CGEN_DOT %i\n"
						"#define CGEN_ONE_MINUS_DOT %i\n"
						"#endif\n",
						CGEN_LIGHTING_DIFFUSE,
						CGEN_DOT,
						CGEN_ONE_MINUS_DOT));

	Q_strcat(dest, size,
							 va("#ifndef alphaGen_t\n"
								"#define alphaGen_t\n"
								"#define AGEN_LIGHTING_SPECULAR %i\n"
								"#define AGEN_PORTAL %i\n"
								"#define AGEN_SCOORD %i\n"
								"#define AGEN_TCOORD %i\n"
								"#define AGEN_DOT %i\n"
								"#define AGEN_ONE_MINUS_DOT %i\n"
								"#endif\n",
								AGEN_LIGHTING_SPECULAR,
								AGEN_PORTAL,
								AGEN_SCOORD,
								AGEN_TCOORD,
								AGEN_DOT,
								AGEN_ONE_MINUS_DOT));

	fbufWidthScale = 1.0f / ((float)glConfig.vidWidth);
	fbufHeightScale = 1.0f / ((float)glConfig.vidHeight);
	// HZM gl2 shadow hardening (r_shadowHarden 1 at R_Init, plan ME-F12): lightall reads the SCENE-sized
	// shadow mask with gl_FragCoord * r_FBufScale, and the scene renders at r_renderScale, so normalise by the
	// scene size (R_InitImages set it before GLSL init). At scale 1.0 the two are identical.
	if (r_shadowHarden && r_shadowHarden->integer && tr.sceneWidth > 0 && tr.sceneHeight > 0)
	{
		fbufWidthScale = 1.0f / ((float)tr.sceneWidth);
		fbufHeightScale = 1.0f / ((float)tr.sceneHeight);
	}
	Q_strcat(dest, size,
			 va("#ifndef r_FBufScale\n#define r_FBufScale vec2(%f, %f)\n#endif\n", fbufWidthScale, fbufHeightScale));

	if (r_pbr->integer)
		Q_strcat(dest, size, "#define USE_PBR\n");

	if (r_cubeMapping->integer)
	{
		int cubeMipSize = r_cubemapSize->integer;
		int numRoughnessMips = 0;

		while (cubeMipSize)
		{
			cubeMipSize >>= 1;
			numRoughnessMips++;
		}
		numRoughnessMips = MAX(1, numRoughnessMips - 2);
		Q_strcat(dest, size, va("#define ROUGHNESS_MIPS float(%d)\n", numRoughnessMips));
	}

	// OK we added a lot of stuff but if we do something bad in the GLSL shaders then we want the proper line
	// so we have to reset the line counting
	Q_strcat(dest, size, "#line 0\n");
}

// HZM gl2 MSAA (plan P2a, ME-F3): while set, a compile or link failure returns 0 instead of ri.Error(ERR_DROP).
// Only GLSL_TryInitGPUShader sets it, for programs the renderer can run without (the MSAA ones).
static qboolean s_glslTry = qfalse;
// HZM gl2 MSAA (plan P2b): when set, the program has a second fragment output of this name; it is bound to colour 1
// (and out_Color to colour 0) before the link. NULL for every other program, so nothing else changes.
static const char *s_glslFragData1 = NULL;
void GLSL_DeleteGPUShader(shaderProgram_t *program);

static int GLSL_CompileGPUShader(GLuint program, GLuint *prevShader, const GLchar *buffer, int size, GLenum shaderType)
{
	GLint           compiled;
	GLuint          shader;

	shader = qglCreateShader(shaderType);

	qglShaderSource(shader, 1, (const GLchar **)&buffer, &size);

	// compile shader
	qglCompileShader(shader);

	// check if shader compiled
	qglGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
	if(!compiled)
	{
		GLSL_PrintLog(shader, GLSL_PRINTLOG_SHADER_SOURCE, qfalse);
		GLSL_PrintLog(shader, GLSL_PRINTLOG_SHADER_INFO, qfalse);
		if (s_glslTry)
		{
			qglDeleteShader(shader);   // HZM MSAA P2a: a TRY build reports the failure to its caller
			return 0;
		}
		ri.Error(ERR_DROP, "Couldn't compile shader");
		return 0;
	}

	if (*prevShader)
	{
		qglDetachShader(program, *prevShader);
		qglDeleteShader(*prevShader);
	}

	// attach shader to program
	qglAttachShader(program, shader);

	*prevShader = shader;

	return 1;
}

// HZM [2026-09-27] console quiet (docs/proposals/console_quiet_2026-09-27): the upstream GLSL compile dump - one
// '------- GPU shader -------' header, two '...loading built-in' lines and every active uniform, per
// program - printed ~6,700 lines per R_Init pair into every session run with developer 1 (which the
// launcher sets and the script-error gate needs). It now also needs r_glslVerbose 1. flags 0, never
// archived. The renderer-reinit harness hashes these lines: it passes +set r_glslVerbose 1.
static qboolean GLSL_VerboseDump(void)
{
	static cvar_t *r_glslVerbose = NULL;

	if (!r_glslVerbose) {
		r_glslVerbose = ri.Cvar_Get("r_glslVerbose", "0", 0);
	}
	return (qboolean)(r_glslVerbose->integer != 0);
}

// HZM [2026-09-28, bug-3179] the per-stage shader text buffer of GLSL_InitGPUShader (header + source + NUL must fit).
// glsl_variant_lint.py parses this define: keep it a plain decimal literal.
#define HZM_GLSL_TEXT_MAX 65536

static int GLSL_LoadGPUShaderText(const char *name, const char *fallback,
	GLenum shaderType, char *dest, int destSize)
{
	char            filename[MAX_QPATH];
	GLchar      *buffer = NULL;
	const GLchar *shaderText = NULL;
	int             size;
	int             result;

	if(shaderType == GL_VERTEX_SHADER)
	{
		Com_sprintf(filename, sizeof(filename), "glsl/%s_vp.glsl", name);
	}
	else
	{
		Com_sprintf(filename, sizeof(filename), "glsl/%s_fp.glsl", name);
	}

	if ( r_externalGLSL->integer ) {
		size = ri.FS_ReadFile(filename, (void **)&buffer);
	} else {
		size = 0;
		buffer = NULL;
	}

	if(!buffer)
	{
		if (fallback)
		{
			if (GLSL_VerboseDump()) {
				ri.Printf(PRINT_DEVELOPER, "...loading built-in '%s'\n", filename);
			}
			shaderText = fallback;
			size = strlen(shaderText);
		}
		else
		{
			ri.Printf(PRINT_DEVELOPER, "couldn't load '%s'\n", filename);
			return 0;
		}
	}
	else
	{
		ri.Printf(PRINT_DEVELOPER, "...loading '%s'\n", filename);
		shaderText = buffer;
	}

	// HZM [2026-09-28, bug-3179] '>=': the copy below writes size + 1 bytes (with the NUL), so size == destSize wrote
	// one byte past the buffer. And no longer silent: the caller's only message was "Could not load <name> shader!".
	if (size >= destSize)
	{
		ri.Printf(PRINT_ALL, "^1GLSL_LoadGPUShaderText: '%s' is %d bytes but only %d fit after the header "
			"(buffer %d, HZM_GLSL_TEXT_MAX in tr_glsl.c)\n", filename, size, destSize - 1, HZM_GLSL_TEXT_MAX);
		result = 0;
	}
	else
	{
		Q_strncpyz(dest, shaderText, size + 1);
		result = 1;
	}

	if (buffer)
	{
		ri.FS_FreeFile(buffer);
	}
	
	return result;
}

static int GLSL_LinkProgram(GLuint program)
{
	GLint           linked;

	qglLinkProgram(program);

	qglGetProgramiv(program, GL_LINK_STATUS, &linked);
	if(!linked)
	{
		GLSL_PrintLog(program, GLSL_PRINTLOG_PROGRAM_INFO, qfalse);
		if (s_glslTry)
			return 0;   // HZM MSAA P2a: a TRY build reports the failure to its caller
		ri.Error(ERR_DROP, "shaders failed to link");
	}
	return 1;
}

static void GLSL_ShowProgramUniforms(GLuint program)
{
	int             i, count, size;
	GLenum			type;
	char            uniformName[1000];

	if (!GLSL_VerboseDump()) {
		return;
	}

	// query the number of active uniforms
	qglGetProgramiv(program, GL_ACTIVE_UNIFORMS, &count);

	// Loop over each of the active uniforms, and set their value
	for(i = 0; i < count; i++)
	{
		qglGetActiveUniform(program, i, sizeof(uniformName), NULL, &size, &type, uniformName);

		ri.Printf(PRINT_DEVELOPER, "active uniform: '%s'\n", uniformName);
	}
}

static int GLSL_InitGPUShader2(shaderProgram_t * program, const char *name, int attribs, const char *vpCode, const char *fpCode)
{
	if (GLSL_VerboseDump()) {
		ri.Printf(PRINT_DEVELOPER, "------- GPU shader -------\n");
	}

	if(strlen(name) >= MAX_QPATH)
	{
		ri.Error(ERR_DROP, "GLSL_InitGPUShader2: \"%s\" is too long", name);
	}

	Q_strncpyz(program->name, name, sizeof(program->name));

	program->program = qglCreateProgram();
	program->attribs = attribs;

	if (!(GLSL_CompileGPUShader(program->program, &program->vertexShader, vpCode, strlen(vpCode), GL_VERTEX_SHADER)))
	{
		ri.Printf(PRINT_ALL, "GLSL_InitGPUShader2: Unable to load \"%s\" as GL_VERTEX_SHADER\n", name);
		if (s_glslTry)
		{
			GLSL_DeleteGPUShader(program);   // HZM MSAA P2a: zero the struct too, so no stale program id survives
			return 0;
		}
		qglDeleteProgram(program->program);
		return 0;
	}

	if(fpCode)
	{
		if(!(GLSL_CompileGPUShader(program->program, &program->fragmentShader, fpCode, strlen(fpCode), GL_FRAGMENT_SHADER)))
		{
			ri.Printf(PRINT_ALL, "GLSL_InitGPUShader2: Unable to load \"%s\" as GL_FRAGMENT_SHADER\n", name);
			if (s_glslTry)
			{
				GLSL_DeleteGPUShader(program);   // HZM MSAA P2a: also frees the vertex shader already attached
				return 0;
			}
			qglDeleteProgram(program->program);
			return 0;
		}
	}

	if(attribs & ATTR_POSITION)
		qglBindAttribLocation(program->program, ATTR_INDEX_POSITION, "attr_Position");

	if(attribs & ATTR_TEXCOORD)
		qglBindAttribLocation(program->program, ATTR_INDEX_TEXCOORD, "attr_TexCoord0");

	if(attribs & ATTR_LIGHTCOORD)
		qglBindAttribLocation(program->program, ATTR_INDEX_LIGHTCOORD, "attr_TexCoord1");

//  if(attribs & ATTR_TEXCOORD2)
//      qglBindAttribLocation(program->program, ATTR_INDEX_TEXCOORD2, "attr_TexCoord2");

//  if(attribs & ATTR_TEXCOORD3)
//      qglBindAttribLocation(program->program, ATTR_INDEX_TEXCOORD3, "attr_TexCoord3");

	if(attribs & ATTR_TANGENT)
		qglBindAttribLocation(program->program, ATTR_INDEX_TANGENT, "attr_Tangent");

	if(attribs & ATTR_NORMAL)
		qglBindAttribLocation(program->program, ATTR_INDEX_NORMAL, "attr_Normal");

	if(attribs & ATTR_COLOR)
		qglBindAttribLocation(program->program, ATTR_INDEX_COLOR, "attr_Color");

	if(attribs & ATTR_PAINTCOLOR)
		qglBindAttribLocation(program->program, ATTR_INDEX_PAINTCOLOR, "attr_PaintColor");

	if(attribs & ATTR_LIGHTDIRECTION)
		qglBindAttribLocation(program->program, ATTR_INDEX_LIGHTDIRECTION, "attr_LightDirection");

	if(attribs & ATTR_BONE_INDEXES)
		qglBindAttribLocation(program->program, ATTR_INDEX_BONE_INDEXES, "attr_BoneIndexes");

	if(attribs & ATTR_BONE_WEIGHTS)
		qglBindAttribLocation(program->program, ATTR_INDEX_BONE_WEIGHTS, "attr_BoneWeights");

	if(attribs & ATTR_POSITION2)
		qglBindAttribLocation(program->program, ATTR_INDEX_POSITION2, "attr_Position2");

	if(attribs & ATTR_NORMAL2)
		qglBindAttribLocation(program->program, ATTR_INDEX_NORMAL2, "attr_Normal2");

	if(attribs & ATTR_TANGENT2)
		qglBindAttribLocation(program->program, ATTR_INDEX_TANGENT2, "attr_Tangent2");

	if (s_glslFragData1)
	{
		if (!qglBindFragDataLocation)
		{
			GLSL_DeleteGPUShader(program);   // HZM MSAA P2b: an MRT program without the entry point is a TRY failure
			return 0;
		}
		qglBindFragDataLocation(program->program, 0, "out_Color");
		qglBindFragDataLocation(program->program, 1, s_glslFragData1);
	}

	if (!GLSL_LinkProgram(program->program))
	{
		GLSL_DeleteGPUShader(program);   // HZM MSAA P2a: only a TRY build gets here (otherwise ri.Error)
		return 0;
	}

	return 1;
}

static int GLSL_InitGPUShader(shaderProgram_t * program, const char *name,
	int attribs, qboolean fragmentShader, const GLchar *extra, qboolean addHeader,
	const char *fallback_vp, const char *fallback_fp)
{
	// HZM [2026-09-28, bug-3179] were 32000-byte STACK arrays. The HZM additions to lightall_fp (lightning, wetness,
	// fog) grew every assembled variant (header + source) to 32145-32744 bytes, GLSL_LoadGPUShaderText refused the
	// text without a word, and every gl2 client died with "Could not load lightall shader!" (v1103c). 64 KB each,
	// static (init is single-threaded and never re-enters). docs/tools/glsl_variant_lint.py reads HZM_GLSL_TEXT_MAX
	// from this file and fails the build if any variant no longer fits (warns at 95%).
	static char vpCode[HZM_GLSL_TEXT_MAX];
	static char fpCode[HZM_GLSL_TEXT_MAX];
	char *postHeader;
	int size;
	int result;

	size = sizeof(vpCode);
	if (addHeader)
	{
		GLSL_GetShaderHeader(GL_VERTEX_SHADER, extra, vpCode, size);
		postHeader = &vpCode[strlen(vpCode)];
		size -= strlen(vpCode);
	}
	else
	{
		postHeader = &vpCode[0];
	}

	if (!GLSL_LoadGPUShaderText(name, fallback_vp, GL_VERTEX_SHADER, postHeader, size))
	{
		return 0;
	}

	if (fragmentShader)
	{
		size = sizeof(fpCode);
		if (addHeader)
		{
			GLSL_GetShaderHeader(GL_FRAGMENT_SHADER, extra, fpCode, size);
			postHeader = &fpCode[strlen(fpCode)];
			size -= strlen(fpCode);
		}
		else
		{
			postHeader = &fpCode[0];
		}

		if (!GLSL_LoadGPUShaderText(name, fallback_fp, GL_FRAGMENT_SHADER, postHeader, size))
		{
			return 0;
		}
	}

	result = GLSL_InitGPUShader2(program, name, attribs, vpCode, fragmentShader ? fpCode : NULL);

	return result;
}

// HZM gl2 MSAA (plan P2a, ME-F3): GLSL_InitGPUShader that returns 0 on a compile or link failure instead of
// ri.Error - for programs the renderer can run without, so a driver that rejects them costs MSAA, not the session.
static int GLSL_TryInitGPUShader(shaderProgram_t * program, const char *name,
	int attribs, qboolean fragmentShader, const GLchar *extra, qboolean addHeader,
	const char *fallback_vp, const char *fallback_fp)
{
	int result;

	s_glslTry = qtrue;
	result = GLSL_InitGPUShader(program, name, attribs, fragmentShader, extra, addHeader, fallback_vp, fallback_fp);
	s_glslTry = qfalse;
	return result;
}

// HZM gl2 MSAA (plan P2b): the multisample (min, max) depth readers - soft particles (generic, lightall), underwater
// and the screen fog. Their MSAA code sits behind MSAA_DEPTH_MINMAX, which is added (to a COPY of the caller's
// defines) only on the new path at >= 2 samples, so MSAA-off sources are unchanged. A program built with it is TRY-built:
// 1 = built, 2 = failed with the define (tr.msaaCompileFailed; R_Msaa_AfterGLSL re-initialises at 0, so the caller only
// skips its uniform setup), 0 = the ordinary failure (the caller's ri.Error, as today).
static int GLSL_InitGPUShaderMsaaDepth(shaderProgram_t * program, const char *name,
	int attribs, qboolean fragmentShader, const GLchar *extra, qboolean addHeader,
	const char *fallback_vp, const char *fallback_fp)
{
	char defs[1024];

	if (!(tr.msaaNewPath && tr.msaaSamples >= 2))
	{
		return GLSL_InitGPUShader(program, name, attribs, fragmentShader, extra, addHeader, fallback_vp, fallback_fp);
	}

	Q_strncpyz(defs, extra ? extra : "", sizeof(defs));
	Q_strcat(defs, sizeof(defs), "#define MSAA_DEPTH_MINMAX\n");
	// HZM gl2 MSAA P4b (ME-F16, r_msaaCentroid, read at R_Init): centroid lightmap/deluxe coordinates. Only lightall
	// reads the define; the diffuse UVs stay centre-sampled (centroid breaks their derivatives, i.e. mip selection)
	if (r_msaaCentroid && r_msaaCentroid->integer)
	{
		Q_strcat(defs, sizeof(defs), "#define USE_MSAA_CENTROID\n");
	}
	if (GLSL_TryInitGPUShader(program, name, attribs, fragmentShader, defs, addHeader, fallback_vp, fallback_fp))
	{
		return 1;
	}
	tr.msaaCompileFailed = qtrue;
	ri.Printf(PRINT_WARNING, "^~^~^ MSAA shader %s failed to build with MSAA_DEPTH_MINMAX at %d samples\n", name, tr.msaaSamples);
	return 2;
}

void GLSL_InitUniforms(shaderProgram_t *program)
{
	int i, size;

	GLint *uniforms = program->uniforms;

	size = 0;
	for (i = 0; i < UNIFORM_COUNT; i++)
	{
		uniforms[i] = qglGetUniformLocation(program->program, uniformsInfo[i].name);

		if (uniforms[i] == -1)
			continue;
		 
		program->uniformBufferOffsets[i] = size;

		switch(uniformsInfo[i].type)
		{
			case GLSL_INT:
				size += sizeof(GLint);
				break;
			case GLSL_FLOAT:
				size += sizeof(GLfloat);
				break;
			case GLSL_FLOAT5:
				size += sizeof(vec_t) * 5;
				break;
			case GLSL_VEC2:
				size += sizeof(vec_t) * 2;
				break;
			case GLSL_VEC3:
				size += sizeof(vec_t) * 3;
				break;
			case GLSL_VEC4:
				size += sizeof(vec_t) * 4;
				break;
			case GLSL_MAT16:
				size += sizeof(vec_t) * 16;
				break;
			case GLSL_MAT16_BONEMATRIX:
				size += sizeof(vec_t) * 16 * glRefConfig.glslMaxAnimatedBones;
				break;
			default:
				break;
		}
	}

	program->uniformBuffer = ri.Malloc(size);

	// HZM [UNDERWATER VOLUME v3, 2026-09-03] ZERO IT. This is not tidiness, it is a correctness
	// fix, and it is the reason a depth sampler can be assigned and still not arrive.
	//
	// ri.Malloc is CL_RefMalloc (cl_main.cpp) -> Z_TagMalloc, which is a bare malloc() with no
	// clear (qcommon/memory.c), so this buffer arrives holding recycled heap bytes. Every
	// GLSL_SetUniform* below uses it as a REDUNDANCY CACHE and SKIPS the GL upload when the new
	// value already matches what is stored. An uninitialised slot that happens to hold the value
	// being set therefore makes that first - and for a sampler, only - upload disappear, while
	// every read-back of our own state cheerfully reports success.
	//
	// For a sampler that is fatal and invisible: the uniform keeps GL default of 0, i.e.
	// texture unit 0 = the scene COLOUR, while the engine believes it points at TB_LEVELSMAP.
	// The pass then reconstructs eye distance from scene brightness - a plausible-looking
	// wrong answer, which is the single class of failure this project keeps mis-diagnosing.
	// Nondeterministic per launch, which is worse than a hard failure, not better.
	//
	// Zeroing makes the cache honest and is safe for every uniform type, because GL itself
	// defaults uniforms to 0 - so a first set-to-zero that the honest cache now skips was
	// already a no-op.
	if (size > 0) {
		Com_Memset(program->uniformBuffer, 0, size);
	}
}

void GLSL_FinishGPUShader(shaderProgram_t *program)
{
	GLSL_ShowProgramUniforms(program->program);
	GL_CheckErrors();
}

void GLSL_SetUniformInt(shaderProgram_t *program, int uniformNum, GLint value)
{
	GLint *uniforms = program->uniforms;
	GLint *compare = (GLint *)(program->uniformBuffer + program->uniformBufferOffsets[uniformNum]);

	if (uniforms[uniformNum] == -1)
		return;

	if (uniformsInfo[uniformNum].type != GLSL_INT)
	{
		ri.Printf( PRINT_WARNING, "GLSL_SetUniformInt: wrong type for uniform %i in program %s\n", uniformNum, program->name);
		return;
	}

	if (value == *compare)
	{
		return;
	}

	*compare = value;

	hzmPerf.glUniforms++;	// HZM gl2 [render-perf 0] r_hzmPerfStats
	qglProgramUniform1iEXT(program->program, uniforms[uniformNum], value);
}

/*
=============
GLSL_GetUniformIntValue

HZM [UNDERWATER VOLUME v3, 2026-09-03] Read back the texture unit an int uniform - in practice a
SAMPLER - was last set to, straight out of the shadow copy GLSL_SetUniformInt maintains. The
shadow copy is trustworthy only because GLSL_InitUniforms now zeroes it; see the comment there.

Returns -1 when the shader did not declare that uniform at all (location -1), which is a
DIFFERENT failure from declared-but-pointing-at-the-wrong-unit and has to stay distinguishable
in a log: the first needs a shader edit, the second needs a GLSL_SetUniformInt call.

This exists so a depth-reading post pass can PROVE in one printed integer that its sampler is on
the depth image. qglGetUniformiv is not declared in this fork (renderercommon/qgl.h has
GetUniformLocation and nothing else), so asking GL directly is not an option here.
=============
*/
int GLSL_GetUniformIntValue(shaderProgram_t *program, int uniformNum)
{
	if (!program || !program->uniformBuffer)
		return -1;

	if (program->uniforms[uniformNum] == -1)
		return -1;

	if (uniformsInfo[uniformNum].type != GLSL_INT)
		return -1;

	return (int)*(GLint *)(program->uniformBuffer + program->uniformBufferOffsets[uniformNum]);
}

void GLSL_SetUniformFloat(shaderProgram_t *program, int uniformNum, GLfloat value)
{
	GLint *uniforms = program->uniforms;
	GLfloat *compare = (GLfloat *)(program->uniformBuffer + program->uniformBufferOffsets[uniformNum]);

	if (uniforms[uniformNum] == -1)
		return;

	if (uniformsInfo[uniformNum].type != GLSL_FLOAT)
	{
		ri.Printf( PRINT_WARNING, "GLSL_SetUniformFloat: wrong type for uniform %i in program %s\n", uniformNum, program->name);
		return;
	}

	if (value == *compare)
	{
		return;
	}

	*compare = value;
	
	hzmPerf.glUniforms++;	// HZM gl2 [render-perf 0] r_hzmPerfStats
	qglProgramUniform1fEXT(program->program, uniforms[uniformNum], value);
}

void GLSL_SetUniformVec2(shaderProgram_t *program, int uniformNum, const vec2_t v)
{
	GLint *uniforms = program->uniforms;
	vec_t *compare = (float *)(program->uniformBuffer + program->uniformBufferOffsets[uniformNum]);

	if (uniforms[uniformNum] == -1)
		return;

	if (uniformsInfo[uniformNum].type != GLSL_VEC2)
	{
		ri.Printf( PRINT_WARNING, "GLSL_SetUniformVec2: wrong type for uniform %i in program %s\n", uniformNum, program->name);
		return;
	}

	if (v[0] == compare[0] && v[1] == compare[1])
	{
		return;
	}

	compare[0] = v[0];
	compare[1] = v[1];

	hzmPerf.glUniforms++;	// HZM gl2 [render-perf 0] r_hzmPerfStats
	qglProgramUniform2fEXT(program->program, uniforms[uniformNum], v[0], v[1]);
}

void GLSL_SetUniformVec3(shaderProgram_t *program, int uniformNum, const vec3_t v)
{
	GLint *uniforms = program->uniforms;
	vec_t *compare = (float *)(program->uniformBuffer + program->uniformBufferOffsets[uniformNum]);

	if (uniforms[uniformNum] == -1)
		return;

	if (uniformsInfo[uniformNum].type != GLSL_VEC3)
	{
		ri.Printf( PRINT_WARNING, "GLSL_SetUniformVec3: wrong type for uniform %i in program %s\n", uniformNum, program->name);
		return;
	}

	if (VectorCompare(v, compare))
	{
		return;
	}

	VectorCopy(v, compare);

	hzmPerf.glUniforms++;	// HZM gl2 [render-perf 0] r_hzmPerfStats
	qglProgramUniform3fEXT(program->program, uniforms[uniformNum], v[0], v[1], v[2]);
}

void GLSL_SetUniformVec4(shaderProgram_t *program, int uniformNum, const vec4_t v)
{
	GLint *uniforms = program->uniforms;
	vec_t *compare = (float *)(program->uniformBuffer + program->uniformBufferOffsets[uniformNum]);

	if (uniforms[uniformNum] == -1)
		return;

	if (uniformsInfo[uniformNum].type != GLSL_VEC4)
	{
		ri.Printf( PRINT_WARNING, "GLSL_SetUniformVec4: wrong type for uniform %i in program %s\n", uniformNum, program->name);
		return;
	}

	if (VectorCompare4(v, compare))
	{
		return;
	}

	VectorCopy4(v, compare);

	hzmPerf.glUniforms++;	// HZM gl2 [render-perf 0] r_hzmPerfStats
	qglProgramUniform4fEXT(program->program, uniforms[uniformNum], v[0], v[1], v[2], v[3]);
}

void GLSL_SetUniformFloat5(shaderProgram_t *program, int uniformNum, const vec5_t v)
{
	GLint *uniforms = program->uniforms;
	vec_t *compare = (float *)(program->uniformBuffer + program->uniformBufferOffsets[uniformNum]);

	if (uniforms[uniformNum] == -1)
		return;

	if (uniformsInfo[uniformNum].type != GLSL_FLOAT5)
	{
		ri.Printf( PRINT_WARNING, "GLSL_SetUniformFloat5: wrong type for uniform %i in program %s\n", uniformNum, program->name);
		return;
	}

	if (VectorCompare5(v, compare))
	{
		return;
	}

	VectorCopy5(v, compare);

	hzmPerf.glUniforms++;	// HZM gl2 [render-perf 0] r_hzmPerfStats
	qglProgramUniform1fvEXT(program->program, uniforms[uniformNum], 5, v);
}

void GLSL_SetUniformMat4(shaderProgram_t *program, int uniformNum, const mat4_t matrix)
{
	GLint *uniforms = program->uniforms;
	vec_t *compare = (float *)(program->uniformBuffer + program->uniformBufferOffsets[uniformNum]);

	if (uniforms[uniformNum] == -1)
		return;

	if (uniformsInfo[uniformNum].type != GLSL_MAT16)
	{
		ri.Printf( PRINT_WARNING, "GLSL_SetUniformMat4: wrong type for uniform %i in program %s\n", uniformNum, program->name);
		return;
	}

	if (Mat4Compare(matrix, compare))
	{
		return;
	}

	Mat4Copy(matrix, compare);

	hzmPerf.glUniformMats++;	// HZM gl2 [render-perf 0] r_hzmPerfStats
	qglProgramUniformMatrix4fvEXT(program->program, uniforms[uniformNum], 1, GL_FALSE, matrix);
}

void GLSL_SetUniformMat4BoneMatrix(shaderProgram_t *program, int uniformNum, /*const*/ mat4_t *matrix, int numMatricies)
{
	GLint *uniforms = program->uniforms;
	vec_t *compare = (float *)(program->uniformBuffer + program->uniformBufferOffsets[uniformNum]);

	if (uniforms[uniformNum] == -1) {
		return;
	}

	if (uniformsInfo[uniformNum].type != GLSL_MAT16_BONEMATRIX)
	{
		ri.Printf( PRINT_WARNING, "GLSL_SetUniformMat4BoneMatrix: wrong type for uniform %i in program %s\n", uniformNum, program->name);
		return;
	}

	if (numMatricies > glRefConfig.glslMaxAnimatedBones)
	{
		ri.Printf( PRINT_WARNING, "GLSL_SetUniformMat4BoneMatrix: too many matricies (%d/%d) for uniform %i in program %s\n",
				numMatricies, glRefConfig.glslMaxAnimatedBones, uniformNum, program->name);
		return;
	}

	if (!memcmp(matrix, compare, numMatricies * sizeof(mat4_t)))
	{
		return;
	}

	Com_Memcpy(compare, matrix, numMatricies * sizeof(mat4_t));

	hzmPerf.glUniformMats++;	// HZM gl2 [render-perf 0] r_hzmPerfStats
	qglProgramUniformMatrix4fvEXT(program->program, uniforms[uniformNum], numMatricies, GL_FALSE, &matrix[0][0]);
}

void GLSL_DeleteGPUShader(shaderProgram_t *program)
{
	if(program->program)
	{
		if (program->vertexShader)
		{
			qglDetachShader(program->program, program->vertexShader);
			qglDeleteShader(program->vertexShader);
		}

		if (program->fragmentShader)
		{
			qglDetachShader(program->program, program->fragmentShader);
			qglDeleteShader(program->fragmentShader);
		}

		qglDeleteProgram(program->program);

		if (program->uniformBuffer)
		{
			ri.Free(program->uniformBuffer);
		}

		Com_Memset(program, 0, sizeof(*program));
	}
}

void GLSL_InitGPUShaders(void)
{
	int             startTime, endTime;
	int i;
	char extradefines[1024];
	int attribs;
	int numGenShaders = 0, numLightShaders = 0, numEtcShaders = 0;
	int msaaBuilt;   // HZM MSAA P2b: GLSL_InitGPUShaderMsaaDepth's result

	ri.Printf(PRINT_ALL, "------- GLSL_InitGPUShaders -------\n");

	R_IssuePendingRenderCommands();

	startTime = ri.Milliseconds();

	// OpenGL ES may not have enough attributes to fit ones used for vertex animation
	if ( glRefConfig.maxVertexAttribs > ATTR_INDEX_NORMAL2 ) {
		ri.Printf(PRINT_ALL, "Using GPU vertex animation\n");
		glRefConfig.gpuVertexAnimation = qtrue;
	} else {
		ri.Printf(PRINT_ALL, "Using CPU vertex animation\n");
		glRefConfig.gpuVertexAnimation = qfalse;
	}

	for (i = 0; i < GENERICDEF_COUNT; i++)
	{	
		if ((i & GENERICDEF_USE_VERTEX_ANIMATION) && (i & GENERICDEF_USE_BONE_ANIMATION))
			continue;

		if ((i & GENERICDEF_USE_BONE_ANIMATION) && !glRefConfig.glslMaxAnimatedBones)
			continue;

		attribs = ATTR_POSITION | ATTR_TEXCOORD | ATTR_LIGHTCOORD | ATTR_NORMAL | ATTR_COLOR;
		extradefines[0] = '\0';

		if (i & GENERICDEF_USE_DEFORM_VERTEXES)
			Q_strcat(extradefines, 1024, "#define USE_DEFORM_VERTEXES\n");

		if (i & GENERICDEF_USE_TCGEN_AND_TCMOD)
		{
			Q_strcat(extradefines, 1024, "#define USE_TCGEN\n");
			Q_strcat(extradefines, 1024, "#define USE_TCMOD\n");
		}

		if (i & GENERICDEF_USE_VERTEX_ANIMATION)
		{
			if (!glRefConfig.gpuVertexAnimation)
				continue;

			Q_strcat(extradefines, 1024, "#define USE_VERTEX_ANIMATION\n");
			attribs |= ATTR_POSITION2 | ATTR_NORMAL2;
		}
		else if (i & GENERICDEF_USE_BONE_ANIMATION)
		{
			Q_strcat(extradefines, 1024, va("#define USE_BONE_ANIMATION\n#define MAX_GLSL_BONES %d\n", glRefConfig.glslMaxAnimatedBones));
			attribs |= ATTR_BONE_INDEXES | ATTR_BONE_WEIGHTS;
		}

		if (i & GENERICDEF_USE_FOG)
			Q_strcat(extradefines, 1024, "#define USE_FOG\n");

		if (i & GENERICDEF_USE_RGBAGEN)
			Q_strcat(extradefines, 1024, "#define USE_RGBAGEN\n");

		msaaBuilt = GLSL_InitGPUShaderMsaaDepth(&tr.genericShader[i], "generic", attribs, qtrue, extradefines, qtrue, fallbackShader_generic_vp, fallbackShader_generic_fp);
		if (!msaaBuilt)
		{
			ri.Error(ERR_FATAL, "Could not load generic shader!");
		}
		if (msaaBuilt == 2)
		{
			continue;   // HZM MSAA P2b: MSAA-define failure - R_Msaa_AfterGLSL rebuilds everything at 0
		}

		GLSL_InitUniforms(&tr.genericShader[i]);

		GLSL_SetUniformInt(&tr.genericShader[i], UNIFORM_DIFFUSEMAP, TB_DIFFUSEMAP);
		GLSL_SetUniformInt(&tr.genericShader[i], UNIFORM_LIGHTMAP,   TB_LIGHTMAP);
		GLSL_SetUniformInt(&tr.genericShader[i], UNIFORM_SCREENDEPTHMAP, TB_SCREENDEPTH); // HZM soft particles

		GLSL_FinishGPUShader(&tr.genericShader[i]);

		numGenShaders++;
	}


	attribs = ATTR_POSITION | ATTR_TEXCOORD;

	if (!GLSL_InitGPUShader(&tr.textureColorShader, "texturecolor", attribs, qtrue, extradefines, qtrue, fallbackShader_texturecolor_vp, fallbackShader_texturecolor_fp))
	{
		ri.Error(ERR_FATAL, "Could not load texturecolor shader!");
	}
	
	GLSL_InitUniforms(&tr.textureColorShader);

	GLSL_SetUniformInt(&tr.textureColorShader, UNIFORM_TEXTUREMAP, TB_DIFFUSEMAP);

	GLSL_FinishGPUShader(&tr.textureColorShader);

	numEtcShaders++;

	for (i = 0; i < FOGDEF_COUNT; i++)
	{
		if ((i & FOGDEF_USE_VERTEX_ANIMATION) && (i & FOGDEF_USE_BONE_ANIMATION))
			continue;

		if ((i & FOGDEF_USE_VERTEX_ANIMATION) && !glRefConfig.gpuVertexAnimation)
			continue;

		if ((i & FOGDEF_USE_BONE_ANIMATION) && !glRefConfig.glslMaxAnimatedBones)
			continue;

		attribs = ATTR_POSITION | ATTR_NORMAL | ATTR_TEXCOORD;
		extradefines[0] = '\0';

		if (i & FOGDEF_USE_DEFORM_VERTEXES)
			Q_strcat(extradefines, 1024, "#define USE_DEFORM_VERTEXES\n");

		if (i & FOGDEF_USE_VERTEX_ANIMATION)
		{
			Q_strcat(extradefines, 1024, "#define USE_VERTEX_ANIMATION\n");
			attribs |= ATTR_POSITION2 | ATTR_NORMAL2;
		}
		else if (i & FOGDEF_USE_BONE_ANIMATION)
		{
			Q_strcat(extradefines, 1024, va("#define USE_BONE_ANIMATION\n#define MAX_GLSL_BONES %d\n", glRefConfig.glslMaxAnimatedBones));
			attribs |= ATTR_BONE_INDEXES | ATTR_BONE_WEIGHTS;
		}

		if (!GLSL_InitGPUShader(&tr.fogShader[i], "fogpass", attribs, qtrue, extradefines, qtrue, fallbackShader_fogpass_vp, fallbackShader_fogpass_fp))
		{
			ri.Error(ERR_FATAL, "Could not load fogpass shader!");
		}

		GLSL_InitUniforms(&tr.fogShader[i]);
		GLSL_FinishGPUShader(&tr.fogShader[i]);

		numEtcShaders++;
	}


	for (i = 0; i < DLIGHTDEF_COUNT; i++)
	{
		attribs = ATTR_POSITION | ATTR_NORMAL | ATTR_TEXCOORD;
		extradefines[0] = '\0';

		if (i & DLIGHTDEF_USE_DEFORM_VERTEXES)
		{
			Q_strcat(extradefines, 1024, "#define USE_DEFORM_VERTEXES\n");
		}

		if (!GLSL_InitGPUShader(&tr.dlightShader[i], "dlight", attribs, qtrue, extradefines, qtrue, fallbackShader_dlight_vp, fallbackShader_dlight_fp))
		{
			ri.Error(ERR_FATAL, "Could not load dlight shader!");
		}

		GLSL_InitUniforms(&tr.dlightShader[i]);
		
		GLSL_SetUniformInt(&tr.dlightShader[i], UNIFORM_DIFFUSEMAP, TB_DIFFUSEMAP);

		GLSL_FinishGPUShader(&tr.dlightShader[i]);

		numEtcShaders++;
	}


	for (i = 0; i < LIGHTDEF_COUNT; i++)
	{
		int lightType = i & LIGHTDEF_LIGHTTYPE_MASK;
		qboolean fastLight = !(r_normalMapping->integer || r_specularMapping->integer);

		// skip impossible combos
		if ((i & LIGHTDEF_USE_PARALLAXMAP) && !r_parallaxMapping->integer)
			continue;

		if ((i & LIGHTDEF_USE_SHADOWMAP) && (!lightType || !r_sunlightMode->integer))
			continue;

		if ((i & LIGHTDEF_ENTITY_VERTEX_ANIMATION) && (i & LIGHTDEF_ENTITY_BONE_ANIMATION))
			continue;

		if ((i & LIGHTDEF_ENTITY_BONE_ANIMATION) && !glRefConfig.glslMaxAnimatedBones)
			continue;

		attribs = ATTR_POSITION | ATTR_TEXCOORD | ATTR_COLOR | ATTR_NORMAL;

		extradefines[0] = '\0';

		if (r_dlightMode->integer >= 2)
			Q_strcat(extradefines, 1024, "#define USE_SHADOWMAP\n");

		if (glRefConfig.swizzleNormalmap)
			Q_strcat(extradefines, 1024, "#define SWIZZLE_NORMALMAP\n");

		if (lightType)
		{
			Q_strcat(extradefines, 1024, "#define USE_LIGHT\n");

			if (fastLight)
				Q_strcat(extradefines, 1024, "#define USE_FAST_LIGHT\n");

			switch (lightType)
			{
				case LIGHTDEF_USE_LIGHTMAP:
					Q_strcat(extradefines, 1024, "#define USE_LIGHTMAP\n");
					if (r_deluxeMapping->integer && !fastLight)
						Q_strcat(extradefines, 1024, "#define USE_DELUXEMAP\n");
					attribs |= ATTR_LIGHTCOORD | ATTR_LIGHTDIRECTION;
					break;
				case LIGHTDEF_USE_LIGHT_VECTOR:
					Q_strcat(extradefines, 1024, "#define USE_LIGHT_VECTOR\n");
					break;
				case LIGHTDEF_USE_LIGHT_VERTEX:
					Q_strcat(extradefines, 1024, "#define USE_LIGHT_VERTEX\n");
					attribs |= ATTR_LIGHTDIRECTION;
					break;
				default:
					break;
			}

			if (r_normalMapping->integer)
			{
				Q_strcat(extradefines, 1024, "#define USE_NORMALMAP\n");

				attribs |= ATTR_TANGENT;

				if ((i & LIGHTDEF_USE_PARALLAXMAP) && !(i & LIGHTDEF_ENTITY_VERTEX_ANIMATION) && !(i & LIGHTDEF_ENTITY_BONE_ANIMATION) && r_parallaxMapping->integer)
				{
					Q_strcat(extradefines, 1024, "#define USE_PARALLAXMAP\n");
					if (r_parallaxMapping->integer > 1)
						Q_strcat(extradefines, 1024, "#define USE_RELIEFMAP\n");

					if (r_parallaxMapShadows->integer)
						Q_strcat(extradefines, 1024, "#define USE_PARALLAXMAP_SHADOWS\n");

					Q_strcat(extradefines, 1024, va("#define r_parallaxMapOffset %f\n", r_parallaxMapOffset->value));
				}
			}

			if (r_specularMapping->integer)
				Q_strcat(extradefines, 1024, "#define USE_SPECULARMAP\n");

			if (r_cubeMapping->integer)
			{
				Q_strcat(extradefines, 1024, "#define USE_CUBEMAP\n");
				if (r_cubeMapping->integer == 2)
					Q_strcat(extradefines, 1024, "#define USE_BOX_CUBEMAP_PARALLAX\n");
			}
			else if (r_deluxeSpecular->value > 0.000001f)
			{
				Q_strcat(extradefines, 1024, va("#define r_deluxeSpecular %f\n", r_deluxeSpecular->value));
			}

			switch (r_glossType->integer)
			{
				case 0:
				default:
					Q_strcat(extradefines, 1024, "#define GLOSS_IS_GLOSS\n");
					break;
				case 1:
					Q_strcat(extradefines, 1024, "#define GLOSS_IS_SMOOTHNESS\n");
					break;
				case 2:
					Q_strcat(extradefines, 1024, "#define GLOSS_IS_ROUGHNESS\n");
					break;
				case 3:
					Q_strcat(extradefines, 1024, "#define GLOSS_IS_SHININESS\n");
					break;
			}
		}

		if (i & LIGHTDEF_USE_SHADOWMAP)
		{
			Q_strcat(extradefines, 1024, "#define USE_SHADOWMAP\n");

			if (r_sunlightMode->integer == 1)
				Q_strcat(extradefines, 1024, "#define SHADOWMAP_MODULATE\n");
			else if (r_sunlightMode->integer == 2)
				Q_strcat(extradefines, 1024, "#define USE_PRIMARY_LIGHT\n");
		}

		if (i & LIGHTDEF_USE_TCGEN_AND_TCMOD)
		{
			Q_strcat(extradefines, 1024, "#define USE_TCGEN\n");
			Q_strcat(extradefines, 1024, "#define USE_TCMOD\n");
		}

		if (i & LIGHTDEF_ENTITY_VERTEX_ANIMATION)
		{
			Q_strcat(extradefines, 1024, "#define USE_MODELMATRIX\n");

			if (glRefConfig.gpuVertexAnimation)
			{
				Q_strcat(extradefines, 1024, "#define USE_VERTEX_ANIMATION\n");
				attribs |= ATTR_POSITION2 | ATTR_NORMAL2;

				if (r_normalMapping->integer)
				{
					attribs |= ATTR_TANGENT2;
				}
			}
		}
		else if (i & LIGHTDEF_ENTITY_BONE_ANIMATION)
		{
			Q_strcat(extradefines, 1024, "#define USE_MODELMATRIX\n");
			Q_strcat(extradefines, 1024, va("#define USE_BONE_ANIMATION\n#define MAX_GLSL_BONES %d\n", glRefConfig.glslMaxAnimatedBones));
			attribs |= ATTR_BONE_INDEXES | ATTR_BONE_WEIGHTS;
		}

		msaaBuilt = GLSL_InitGPUShaderMsaaDepth(&tr.lightallShader[i], "lightall", attribs, qtrue, extradefines, qtrue, fallbackShader_lightall_vp, fallbackShader_lightall_fp);
		if (!msaaBuilt)
		{
			ri.Error(ERR_FATAL, "Could not load lightall shader!");
		}
		if (msaaBuilt == 2)
		{
			continue;   // HZM MSAA P2b: MSAA-define failure - R_Msaa_AfterGLSL rebuilds everything at 0
		}

		GLSL_InitUniforms(&tr.lightallShader[i]);

		GLSL_SetUniformInt(&tr.lightallShader[i], UNIFORM_DIFFUSEMAP,  TB_DIFFUSEMAP);
		GLSL_SetUniformInt(&tr.lightallShader[i], UNIFORM_LIGHTMAP,    TB_LIGHTMAP);
		GLSL_SetUniformInt(&tr.lightallShader[i], UNIFORM_NORMALMAP,   TB_NORMALMAP);
		GLSL_SetUniformInt(&tr.lightallShader[i], UNIFORM_DELUXEMAP,   TB_DELUXEMAP);
		GLSL_SetUniformInt(&tr.lightallShader[i], UNIFORM_SPECULARMAP, TB_SPECULARMAP);
		GLSL_SetUniformInt(&tr.lightallShader[i], UNIFORM_SHADOWMAP,   TB_SHADOWMAP);
		GLSL_SetUniformInt(&tr.lightallShader[i], UNIFORM_CUBEMAP,     TB_CUBEMAP);
		GLSL_SetUniformInt(&tr.lightallShader[i], UNIFORM_SCREENDEPTHMAP, TB_SCREENDEPTH); // HZM soft particles
		GLSL_SetUniformInt(&tr.lightallShader[i], UNIFORM_HZMOCCMAP,   TB_HZMRAINOCC);  // HZM rain wetness
		GLSL_SetUniformInt(&tr.lightallShader[i], UNIFORM_HZMWETNOISE, TB_HZMWETNOISE); // HZM rain wetness
		GLSL_SetUniformInt(&tr.lightallShader[i], UNIFORM_HZMSUNWORLDMAP, TB_SUNWORLD); // HZM gl2 P3 B0 (D1)

		GLSL_FinishGPUShader(&tr.lightallShader[i]);

		numLightShaders++;
	}

	for (i = 0; i < SHADOWMAPDEF_COUNT; i++)
	{
		if ((i & SHADOWMAPDEF_USE_VERTEX_ANIMATION) && (i & SHADOWMAPDEF_USE_BONE_ANIMATION))
			continue;

		if ((i & SHADOWMAPDEF_USE_VERTEX_ANIMATION) && !glRefConfig.gpuVertexAnimation)
			continue;

		if ((i & SHADOWMAPDEF_USE_BONE_ANIMATION) && !glRefConfig.glslMaxAnimatedBones)
			continue;

		attribs = ATTR_POSITION | ATTR_NORMAL | ATTR_TEXCOORD;

		extradefines[0] = '\0';

		if (i & SHADOWMAPDEF_USE_VERTEX_ANIMATION)
		{
			Q_strcat(extradefines, 1024, "#define USE_VERTEX_ANIMATION\n");
			attribs |= ATTR_POSITION2 | ATTR_NORMAL2;
		}

		if (i & SHADOWMAPDEF_USE_BONE_ANIMATION)
		{
			Q_strcat(extradefines, 1024, va("#define USE_BONE_ANIMATION\n#define MAX_GLSL_BONES %d\n", glRefConfig.glslMaxAnimatedBones));
			attribs |= ATTR_BONE_INDEXES | ATTR_BONE_WEIGHTS;
		}

		if (!GLSL_InitGPUShader(&tr.shadowmapShader[i], "shadowfill", attribs, qtrue, extradefines, qtrue, fallbackShader_shadowfill_vp, fallbackShader_shadowfill_fp))
		{
			ri.Error(ERR_FATAL, "Could not load shadowfill shader!");
		}

		GLSL_InitUniforms(&tr.shadowmapShader[i]);
		GLSL_FinishGPUShader(&tr.shadowmapShader[i]);

		numEtcShaders++;
	}

	attribs = ATTR_POSITION | ATTR_NORMAL;
	extradefines[0] = '\0';

	Q_strcat(extradefines, 1024, "#define USE_PCF\n#define USE_DISCARD\n");

	if (!GLSL_InitGPUShader(&tr.pshadowShader, "pshadow", attribs, qtrue, extradefines, qtrue, fallbackShader_pshadow_vp, fallbackShader_pshadow_fp))
	{
		ri.Error(ERR_FATAL, "Could not load pshadow shader!");
	}
	
	GLSL_InitUniforms(&tr.pshadowShader);

	GLSL_SetUniformInt(&tr.pshadowShader, UNIFORM_SHADOWMAP, TB_DIFFUSEMAP);

	GLSL_FinishGPUShader(&tr.pshadowShader);

	numEtcShaders++;


	attribs = ATTR_POSITION | ATTR_TEXCOORD;
	extradefines[0] = '\0';

	if (!GLSL_InitGPUShader(&tr.down4xShader, "down4x", attribs, qtrue, extradefines, qtrue, fallbackShader_down4x_vp, fallbackShader_down4x_fp))
	{
		ri.Error(ERR_FATAL, "Could not load down4x shader!");
	}
	
	GLSL_InitUniforms(&tr.down4xShader);

	GLSL_SetUniformInt(&tr.down4xShader, UNIFORM_TEXTUREMAP, TB_DIFFUSEMAP);

	GLSL_FinishGPUShader(&tr.down4xShader);

	numEtcShaders++;


	attribs = ATTR_POSITION | ATTR_TEXCOORD;
	extradefines[0] = '\0';

	if (!GLSL_InitGPUShader(&tr.bokehShader, "bokeh", attribs, qtrue, extradefines, qtrue, fallbackShader_bokeh_vp, fallbackShader_bokeh_fp))
	{
		ri.Error(ERR_FATAL, "Could not load bokeh shader!");
	}

	GLSL_InitUniforms(&tr.bokehShader);

	GLSL_SetUniformInt(&tr.bokehShader, UNIFORM_TEXTUREMAP, TB_DIFFUSEMAP);

	GLSL_FinishGPUShader(&tr.bokehShader);

	numEtcShaders++;


	attribs = ATTR_POSITION | ATTR_TEXCOORD;
	extradefines[0] = '\0';

	if (!GLSL_InitGPUShader(&tr.tonemapShader, "tonemap", attribs, qtrue, extradefines, qtrue, fallbackShader_tonemap_vp, fallbackShader_tonemap_fp))
	{
		ri.Error(ERR_FATAL, "Could not load tonemap shader!");
	}

	GLSL_InitUniforms(&tr.tonemapShader);

	GLSL_SetUniformInt(&tr.tonemapShader, UNIFORM_TEXTUREMAP, TB_COLORMAP);
	GLSL_SetUniformInt(&tr.tonemapShader, UNIFORM_LEVELSMAP,  TB_LEVELSMAP);

	GLSL_FinishGPUShader(&tr.tonemapShader);


	// HZM gl2 PARITY GRADE: gl1's ACES tonemap+grade, so gl2 can reproduce the OG look
	// (r_tonemapMode 1). Shares the tonemap vertex shader; UNIFORM_COLOR carries
	// (exposure, contrast, saturation, temp) and the fragment shader MUST therefore declare it
	// as u_Color - GLSL_InitUniforms resolves uniforms by the NAME in uniformsInfo above
	// (bug-1148: it was declared u_Grade, so the location was -1 and the grade never arrived).
	attribs = ATTR_POSITION | ATTR_TEXCOORD;
	extradefines[0] = '\0';

	if (!GLSL_InitGPUShader(&tr.tonemapHzmShader, "tonemap_hzm", attribs, qtrue, extradefines, qtrue, fallbackShader_tonemap_vp, fallbackShader_tonemap_hzm_fp))
	{
		ri.Error(ERR_FATAL, "Could not load tonemap_hzm shader!");
	}

	GLSL_InitUniforms(&tr.tonemapHzmShader);
	GLSL_SetUniformInt(&tr.tonemapHzmShader, UNIFORM_TEXTUREMAP, TB_COLORMAP);
	GLSL_FinishGPUShader(&tr.tonemapHzmShader);

	numEtcShaders++;

	// HZM gl2 POST-FX PORT: bloom (bug-1149). gl1 runs bright-pass -> separable Gaussian -> additive
	// composite every frame (renderergl1 tr_postprocess_gl1.c RB_PostFxApply) under r_ppBloom; gl2 had
	// no bloom at all. Both stages share the TONEMAP vertex shader because its varying is var_TexCoords,
	// the name these fragment shaders read. The additive composite needs no program of its own -
	// FBO_Blit with a NULL program uses tr.textureColorShader, which is exactly gl1's ADD_FS
	// (texture * u_Color).
	attribs = ATTR_POSITION | ATTR_TEXCOORD;
	extradefines[0] = 0;

	if (!GLSL_InitGPUShader(&tr.bloomBrightShader, "bloom_bright", attribs, qtrue, extradefines, qtrue, fallbackShader_tonemap_vp, fallbackShader_bloom_bright_fp))
	{
		ri.Error(ERR_FATAL, "Could not load bloom_bright shader!");
	}

	GLSL_InitUniforms(&tr.bloomBrightShader);
	GLSL_SetUniformInt(&tr.bloomBrightShader, UNIFORM_TEXTUREMAP, TB_COLORMAP);
	GLSL_SetUniformInt(&tr.bloomBrightShader, UNIFORM_LEVELSMAP,  TB_LEVELSMAP);   // HZM exposure-aware bloom: mode 2 (Hable) proxy reads calcLevels here
	GLSL_FinishGPUShader(&tr.bloomBrightShader);

	numEtcShaders++;

	attribs = ATTR_POSITION | ATTR_TEXCOORD;
	extradefines[0] = 0;

	if (!GLSL_InitGPUShader(&tr.bloomBlurShader, "bloom_blur", attribs, qtrue, extradefines, qtrue, fallbackShader_tonemap_vp, fallbackShader_bloom_blur_fp))
	{
		ri.Error(ERR_FATAL, "Could not load bloom_blur shader!");
	}

	GLSL_InitUniforms(&tr.bloomBlurShader);
	GLSL_SetUniformInt(&tr.bloomBlurShader, UNIFORM_TEXTUREMAP, TB_COLORMAP);
	GLSL_FinishGPUShader(&tr.bloomBlurShader);

	numEtcShaders++;

	// HZM gl2 POST-FX PORT (bug-1150): the screen-space tail of gl1's chain - FXAA, then the
	// contrast-adaptive sharpen, then rain-on-lens. Same order gl1 runs them in, same r_pp* levers.
	// All three share the tonemap vertex shader (its varying is var_TexCoords).

	attribs = ATTR_POSITION | ATTR_TEXCOORD;
	extradefines[0] = 0;

	if (!GLSL_InitGPUShader(&tr.fxaaShader, "fxaa", attribs, qtrue, extradefines, qtrue, fallbackShader_tonemap_vp, fallbackShader_fxaa_fp))
	{
		ri.Error(ERR_FATAL, "Could not load fxaa shader!");
	}

	GLSL_InitUniforms(&tr.fxaaShader);
	GLSL_SetUniformInt(&tr.fxaaShader, UNIFORM_TEXTUREMAP, TB_COLORMAP);
	GLSL_FinishGPUShader(&tr.fxaaShader);

	numEtcShaders++;

	attribs = ATTR_POSITION | ATTR_TEXCOORD;
	extradefines[0] = 0;

	if (!GLSL_InitGPUShader(&tr.sharpenShader, "sharpen", attribs, qtrue, extradefines, qtrue, fallbackShader_tonemap_vp, fallbackShader_sharpen_fp))
	{
		ri.Error(ERR_FATAL, "Could not load sharpen shader!");
	}

	GLSL_InitUniforms(&tr.sharpenShader);
	GLSL_SetUniformInt(&tr.sharpenShader, UNIFORM_TEXTUREMAP, TB_COLORMAP);
	GLSL_FinishGPUShader(&tr.sharpenShader);

	numEtcShaders++;

	// HZM render scale + AMD FSR 1 (r_renderScale). All three use the tonemap vertex shader
	// (var_TexCoords). EASU needs textureGather, which is GLSL 4.00 or GL_ARB_gpu_shader5 - put
	// the #extension directly after #version via extradefines (GLSL_GetShaderHeader emits `extra'
	// there). If EASU fails to build (no gpu_shader5) leave tr.fsrEasuAvailable qfalse and fall
	// back to a bilinear upscale; do NOT ERR_FATAL, so the renderer still runs on that GPU.
	attribs = ATTR_POSITION | ATTR_TEXCOORD;
	extradefines[0] = 0;
	Q_strcat(extradefines, sizeof(extradefines), "#extension GL_ARB_gpu_shader5 : enable\n");

	tr.fsrEasuAvailable = qfalse;
	if (GLSL_InitGPUShader(&tr.fsrEasuShader, "fsr_easu", attribs, qtrue, extradefines, qtrue, fallbackShader_tonemap_vp, fallbackShader_fsr_easu_fp))
	{
		GLSL_InitUniforms(&tr.fsrEasuShader);
		GLSL_SetUniformInt(&tr.fsrEasuShader, UNIFORM_TEXTUREMAP, TB_COLORMAP);
		GLSL_FinishGPUShader(&tr.fsrEasuShader);
		tr.fsrEasuAvailable = qtrue;
		numEtcShaders++;
	}
	else
	{
		ri.Printf(PRINT_WARNING, "FSR EASU shader failed to build (GL_ARB_gpu_shader5 missing?); render scale < 1.0 will use a bilinear upscale.\n");
	}

	attribs = ATTR_POSITION | ATTR_TEXCOORD;
	extradefines[0] = 0;

	if (!GLSL_InitGPUShader(&tr.fsrRcasShader, "fsr_rcas", attribs, qtrue, extradefines, qtrue, fallbackShader_tonemap_vp, fallbackShader_fsr_rcas_fp))
	{
		ri.Error(ERR_FATAL, "Could not load fsr_rcas shader!");
	}
	GLSL_InitUniforms(&tr.fsrRcasShader);
	GLSL_SetUniformInt(&tr.fsrRcasShader, UNIFORM_TEXTUREMAP, TB_COLORMAP);
	GLSL_FinishGPUShader(&tr.fsrRcasShader);
	numEtcShaders++;

	attribs = ATTR_POSITION | ATTR_TEXCOORD;
	extradefines[0] = 0;

	if (!GLSL_InitGPUShader(&tr.fsrDownscaleShader, "fsr_downscale", attribs, qtrue, extradefines, qtrue, fallbackShader_tonemap_vp, fallbackShader_fsr_downscale_fp))
	{
		ri.Error(ERR_FATAL, "Could not load fsr_downscale shader!");
	}
	GLSL_InitUniforms(&tr.fsrDownscaleShader);
	GLSL_SetUniformInt(&tr.fsrDownscaleShader, UNIFORM_TEXTUREMAP, TB_COLORMAP);
	GLSL_FinishGPUShader(&tr.fsrDownscaleShader);
	numEtcShaders++;

	attribs = ATTR_POSITION | ATTR_TEXCOORD;
	extradefines[0] = 0;

	if (!GLSL_InitGPUShader(&tr.rainDropsShader, "raindrops", attribs, qtrue, extradefines, qtrue, fallbackShader_tonemap_vp, fallbackShader_raindrops_fp))
	{
		ri.Error(ERR_FATAL, "Could not load raindrops shader!");
	}

	GLSL_InitUniforms(&tr.rainDropsShader);
	GLSL_SetUniformInt(&tr.rainDropsShader, UNIFORM_TEXTUREMAP, TB_COLORMAP);
	GLSL_FinishGPUShader(&tr.rainDropsShader);

	numEtcShaders++;

	// HZM water pass (tr_hzm_water.c, water_wetness_2026-09-27 W3): [0] plain, [1] USE_DEFORM_VERTEXES - the fogShader
	// split. Optional: a failed compile leaves program 0 and the pass never draws, it never takes the renderer down.
	for (i = 0; i < 2; i++)
	{
		attribs = ATTR_POSITION | ATTR_NORMAL | ATTR_TEXCOORD | ATTR_LIGHTCOORD;
		extradefines[0] = '\0';
		if (i)
			Q_strcat(extradefines, 1024, "#define USE_DEFORM_VERTEXES\n");

		if (!GLSL_InitGPUShader(&tr.hzmWaterShader[i], "hzmwater", attribs, qtrue, extradefines, qtrue, fallbackShader_hzmwater_vp, fallbackShader_hzmwater_fp))
		{
			ri.Printf(PRINT_WARNING, "WARNING: hzmwater shader %d failed; the r_hzmWater pass is unavailable\n", i);
			Com_Memset(&tr.hzmWaterShader[i], 0, sizeof(tr.hzmWaterShader[i]));
			continue;
		}
		GLSL_InitUniforms(&tr.hzmWaterShader[i]);
		GLSL_SetUniformInt(&tr.hzmWaterShader[i], UNIFORM_LIGHTMAP,     TB_LIGHTMAP);
		GLSL_SetUniformInt(&tr.hzmWaterShader[i], UNIFORM_HZMOCCMAP,    TB_HZMRAINOCC);
		GLSL_SetUniformInt(&tr.hzmWaterShader[i], UNIFORM_HZMRIPPLEMAP, TB_HZMWETNOISE);
		GLSL_FinishGPUShader(&tr.hzmWaterShader[i]);
		numEtcShaders++;
	}

	// HZM gl2 POST-FX PORT (bug-1151): the GAMEPLAY-DRIVEN stages of gl1's chain - heat haze +
	// muzzle shimmer, low-health desat/red vignette/heartbeat, and the suppression tunnel-vignette.
	// All read the same cgame-published signals gl1 reads (r_ppHeat, r_ppHealthFrac, r_ppSuppress).

	attribs = ATTR_POSITION | ATTR_TEXCOORD;
	extradefines[0] = 0;

	if (!GLSL_InitGPUShader(&tr.heatHazeShader, "heathaze", attribs, qtrue, extradefines, qtrue, fallbackShader_tonemap_vp, fallbackShader_heathaze_fp))
	{
		ri.Error(ERR_FATAL, "Could not load heathaze shader!");
	}

	GLSL_InitUniforms(&tr.heatHazeShader);
	GLSL_SetUniformInt(&tr.heatHazeShader, UNIFORM_TEXTUREMAP, TB_COLORMAP);
	GLSL_FinishGPUShader(&tr.heatHazeShader);

	numEtcShaders++;

	attribs = ATTR_POSITION | ATTR_TEXCOORD;
	extradefines[0] = 0;

	if (!GLSL_InitGPUShader(&tr.lowHealthShader, "lowhealth", attribs, qtrue, extradefines, qtrue, fallbackShader_tonemap_vp, fallbackShader_lowhealth_fp))
	{
		ri.Error(ERR_FATAL, "Could not load lowhealth shader!");
	}

	GLSL_InitUniforms(&tr.lowHealthShader);
	GLSL_SetUniformInt(&tr.lowHealthShader, UNIFORM_TEXTUREMAP, TB_COLORMAP);
	GLSL_FinishGPUShader(&tr.lowHealthShader);

	numEtcShaders++;

	attribs = ATTR_POSITION | ATTR_TEXCOORD;
	extradefines[0] = 0;

	if (!GLSL_InitGPUShader(&tr.suppressionShader, "suppression", attribs, qtrue, extradefines, qtrue, fallbackShader_tonemap_vp, fallbackShader_suppression_fp))
	{
		ri.Error(ERR_FATAL, "Could not load suppression shader!");
	}

	GLSL_InitUniforms(&tr.suppressionShader);
	GLSL_SetUniformInt(&tr.suppressionShader, UNIFORM_TEXTUREMAP, TB_COLORMAP);
	GLSL_FinishGPUShader(&tr.suppressionShader);

	// HZM coop [user 08-02] ON-HIT BLOOD - same wiring as suppression directly above.
	if (!GLSL_InitGPUShader(&tr.hitBloodShader, "hitblood", attribs, qtrue, extradefines, qtrue, fallbackShader_tonemap_vp, fallbackShader_hitblood_fp))
	{
		ri.Error(ERR_FATAL, "Could not load hitblood shader!");
	}
	GLSL_InitUniforms(&tr.hitBloodShader);
	GLSL_SetUniformInt(&tr.hitBloodShader, UNIFORM_TEXTUREMAP, TB_COLORMAP);
	GLSL_FinishGPUShader(&tr.hitBloodShader);

	numEtcShaders++;

	// HZM gl2 POST-FX PORT (bug-1157): depth of field. Two samplers - the blurred colour on
	// TB_COLORMAP (bound by FBO_Blit) and a COPY of scene depth on TB_LIGHTMAP, the same pairing
	// depthBlur already uses. The copy matters: sampling tr.renderDepthImage directly would be a
	// feedback loop, since it is the depth attachment of the very FBO this pass draws into.
	attribs = ATTR_POSITION | ATTR_TEXCOORD;
	extradefines[0] = 0;

	if (!GLSL_InitGPUShader(&tr.dofShader, "dof", attribs, qtrue, extradefines, qtrue, fallbackShader_tonemap_vp, fallbackShader_dof_fp))
	{
		ri.Error(ERR_FATAL, "Could not load dof shader!");
	}

	GLSL_InitUniforms(&tr.dofShader);
	GLSL_SetUniformInt(&tr.dofShader, UNIFORM_TEXTUREMAP, TB_COLORMAP);
	GLSL_SetUniformInt(&tr.dofShader, UNIFORM_SCREENDEPTHMAP, TB_LIGHTMAP);
	GLSL_FinishGPUShader(&tr.dofShader);

	numEtcShaders++;

	// HZM gl2 NEW POST-FX (bug-1158): cinematic-look additions, not gl1 ports - underwater
	// distortion (real cgame signal, r_ppUnderwater), chromatic aberration, film grain. Same
	// tonemap vertex shader and registration pattern as every stage above.

	attribs = ATTR_POSITION | ATTR_TEXCOORD;
	extradefines[0] = 0;

	msaaBuilt = GLSL_InitGPUShaderMsaaDepth(&tr.underwaterShader, "underwater", attribs, qtrue, extradefines, qtrue, fallbackShader_tonemap_vp, fallbackShader_underwater_fp);
	if (!msaaBuilt)
	{
		ri.Error(ERR_FATAL, "Could not load underwater shader!");
	}

	if (msaaBuilt != 2)   // HZM MSAA P2b: an MSAA-define failure leaves the program empty until the re-init at 0
	{
	GLSL_InitUniforms(&tr.underwaterShader);
	GLSL_SetUniformInt(&tr.underwaterShader, UNIFORM_TEXTUREMAP, TB_COLORMAP);
	// [UNDERWATER VOLUME v3] MANDATORY, NOT OPTIONAL. The pass now samples scene DEPTH as well,
	// on the same TMU the global-fog pass uses (tr_glsl.c globalFogShader, and tonemapShader
	// above). Without this line the sampler defaults to unit 0 and the shader reads the COLOUR
	// image as depth - which is not a crash, it is a plausible-looking wrong answer, and every
	// downstream term (absorption, blur radius, shafts, the sky-to-silt bonus) is then driven by
	// scene brightness. RB_HZMExtraFx re-asserts it per submersion and prints smp= so its
	// absence is visible in the log; do not delete this line on the strength of that.
	GLSL_SetUniformInt(&tr.underwaterShader, UNIFORM_LEVELSMAP,  TB_LEVELSMAP);
	GLSL_FinishGPUShader(&tr.underwaterShader);
	}

	// HZM [bug-2360] blood on the lens. Same shape as the underwater pass above: a full-screen blit
	// over the tonemap vertex shader, one sampler, all parameters carried in u_Color.
	if (!GLSL_InitGPUShader(&tr.bloodSpatterShader, "bloodspatter", attribs, qtrue, extradefines, qtrue, fallbackShader_tonemap_vp, fallbackShader_bloodspatter_fp))
	{
		ri.Error(ERR_FATAL, "Could not load bloodspatter shader!");
	}

	GLSL_InitUniforms(&tr.bloodSpatterShader);
	GLSL_SetUniformInt(&tr.bloodSpatterShader, UNIFORM_TEXTUREMAP, TB_COLORMAP);
	GLSL_FinishGPUShader(&tr.bloodSpatterShader);

	numEtcShaders++;

	attribs = ATTR_POSITION | ATTR_TEXCOORD;
	extradefines[0] = 0;

	if (!GLSL_InitGPUShader(&tr.chromabShader, "chromab", attribs, qtrue, extradefines, qtrue, fallbackShader_tonemap_vp, fallbackShader_chromab_fp))
	{
		ri.Error(ERR_FATAL, "Could not load chromab shader!");
	}

	GLSL_InitUniforms(&tr.chromabShader);
	GLSL_SetUniformInt(&tr.chromabShader, UNIFORM_TEXTUREMAP, TB_COLORMAP);
	GLSL_FinishGPUShader(&tr.chromabShader);

	// HZM [user 07-29] CAMERA MOTION BLUR. Shares tonemap_vp like every other full-screen HZM pass.
	// GLSL_InitUniforms is NOT optional here: `tr` is Com_Memset to 0 in R_Init, so an uninitialised
	// program has every uniforms[] entry at 0 - which is a VALID GL location and sails straight past
	// the `== -1` guard in GLSL_SetUniform*, then dereferences into the NULL uniformBuffer. That is a
	// hard crash on the first frame the pass runs, not a silent no-op.
	if (!GLSL_InitGPUShader(&tr.motionBlurShader, "motionblur", attribs, qtrue, extradefines, qtrue, fallbackShader_tonemap_vp, fallbackShader_motionblur_fp))
	{
		ri.Error(ERR_FATAL, "Could not load motionblur shader!");
	}
	GLSL_InitUniforms(&tr.motionBlurShader);
	GLSL_SetUniformInt(&tr.motionBlurShader, UNIFORM_TEXTUREMAP, TB_COLORMAP);
	GLSL_FinishGPUShader(&tr.motionBlurShader);

	numEtcShaders++;

	attribs = ATTR_POSITION | ATTR_TEXCOORD;
	extradefines[0] = 0;

	if (!GLSL_InitGPUShader(&tr.filmgrainShader, "filmgrain", attribs, qtrue, extradefines, qtrue, fallbackShader_tonemap_vp, fallbackShader_filmgrain_fp))
	{
		ri.Error(ERR_FATAL, "Could not load filmgrain shader!");
	}

	GLSL_InitUniforms(&tr.filmgrainShader);
	GLSL_SetUniformInt(&tr.filmgrainShader, UNIFORM_TEXTUREMAP, TB_COLORMAP);
	GLSL_FinishGPUShader(&tr.filmgrainShader);

	numEtcShaders++;

	attribs = ATTR_POSITION | ATTR_TEXCOORD;
	extradefines[0] = 0;

	if (!GLSL_InitGPUShader(&tr.frostShader, "frost", attribs, qtrue, extradefines, qtrue, fallbackShader_tonemap_vp, fallbackShader_frost_fp))
	{
		ri.Error(ERR_FATAL, "Could not load frost shader!");
	}

	GLSL_InitUniforms(&tr.frostShader);
	GLSL_SetUniformInt(&tr.frostShader, UNIFORM_TEXTUREMAP, TB_COLORMAP);
	GLSL_FinishGPUShader(&tr.frostShader);

	numEtcShaders++;

	// HZM gl2 re-port: MOHAA global farplane distance fog (screen-space depth pass)
	attribs = ATTR_POSITION | ATTR_TEXCOORD;
	extradefines[0] = '\0';

	msaaBuilt = GLSL_InitGPUShaderMsaaDepth(&tr.globalFogShader, "globalfog", attribs, qtrue, extradefines, qtrue, fallbackShader_globalfog_vp, fallbackShader_globalfog_fp);
	if (!msaaBuilt)
	{
		ri.Error(ERR_FATAL, "Could not load globalfog shader!");
	}

	if (msaaBuilt != 2)   // HZM MSAA P2b: an MSAA-define failure leaves the program empty until the re-init at 0
	{
	GLSL_InitUniforms(&tr.globalFogShader);

	GLSL_SetUniformInt(&tr.globalFogShader, UNIFORM_TEXTUREMAP, TB_COLORMAP);
	GLSL_SetUniformInt(&tr.globalFogShader, UNIFORM_LEVELSMAP,  TB_LEVELSMAP);

	GLSL_FinishGPUShader(&tr.globalFogShader);
	}

	numEtcShaders++;


	for (i = 0; i < 2; i++)
	{
		attribs = ATTR_POSITION | ATTR_TEXCOORD;
		extradefines[0] = '\0';

		if (!i)
			Q_strcat(extradefines, 1024, "#define FIRST_PASS\n");

		if (!GLSL_InitGPUShader(&tr.calclevels4xShader[i], "calclevels4x", attribs, qtrue, extradefines, qtrue, fallbackShader_calclevels4x_vp, fallbackShader_calclevels4x_fp))
		{
			ri.Error(ERR_FATAL, "Could not load calclevels4x shader!");
		}

		GLSL_InitUniforms(&tr.calclevels4xShader[i]);

		GLSL_SetUniformInt(&tr.calclevels4xShader[i], UNIFORM_TEXTUREMAP, TB_DIFFUSEMAP);

		GLSL_FinishGPUShader(&tr.calclevels4xShader[i]);

		numEtcShaders++;		
	}


	// GLSL 1.10+ or GL_EXT_shadow_samplers extension are required for sampler2DShadow type
	if (glRefConfig.glslMajorVersion > 1 || (glRefConfig.glslMajorVersion == 1 && glRefConfig.glslMinorVersion >= 10)
	    || glRefConfig.shadowSamplers)
	{
		attribs = ATTR_POSITION | ATTR_TEXCOORD;
		extradefines[0] = '\0';

		if (qglesMajorVersion < 3 && glRefConfig.shadowSamplers)
		{
			Q_strcat(extradefines, 1024, "#extension GL_EXT_shadow_samplers : enable\n");
		}

		if (r_shadowFilter->integer >= 1)
			Q_strcat(extradefines, 1024, "#define USE_SHADOW_FILTER\n");

		if (r_shadowFilter->integer >= 2)
			Q_strcat(extradefines, 1024, "#define USE_SHADOW_FILTER2\n");

		if (r_shadowCascadeZFar->integer != 0)
			Q_strcat(extradefines, 1024, "#define USE_SHADOW_CASCADE\n");

		Q_strcat(extradefines, 1024, va("#define r_shadowMapSize %f\n", r_shadowMapSize->value));
		Q_strcat(extradefines, 1024, va("#define r_shadowCascadeZFar %f\n", r_shadowCascadeZFar->value));

		if (!GLSL_InitGPUShader(&tr.shadowmaskShader, "shadowmask", attribs, qtrue, extradefines, qtrue, fallbackShader_shadowmask_vp, fallbackShader_shadowmask_fp))
		{
			ri.Error(ERR_FATAL, "Could not load shadowmask shader!");
		}
	
		GLSL_InitUniforms(&tr.shadowmaskShader);

		GLSL_SetUniformInt(&tr.shadowmaskShader, UNIFORM_SCREENDEPTHMAP, TB_COLORMAP);
		GLSL_SetUniformInt(&tr.shadowmaskShader, UNIFORM_SHADOWMAP,  TB_SHADOWMAP);
		GLSL_SetUniformInt(&tr.shadowmaskShader, UNIFORM_SHADOWMAP2, TB_SHADOWMAP2);
		GLSL_SetUniformInt(&tr.shadowmaskShader, UNIFORM_SHADOWMAP3, TB_SHADOWMAP3);
		GLSL_SetUniformInt(&tr.shadowmaskShader, UNIFORM_SHADOWMAP4, TB_SHADOWMAP4);

		GLSL_FinishGPUShader(&tr.shadowmaskShader);

		numEtcShaders++;

		// HZM gl2 stable sun shadows (plan P3): the radial entity-cascade mask. TRY-built for everyone: a driver that
		// rejects it costs r_shadowStable (it resolves to legacy), never the session. Its knobs are uniforms (live).
		extradefines[0] = '\0';
		if (qglesMajorVersion < 3 && glRefConfig.shadowSamplers)
		{
			Q_strcat(extradefines, 1024, "#extension GL_EXT_shadow_samplers : enable\n");
		}
		Q_strcat(extradefines, 1024, "#define USE_SHADOW_STABLE\n");
		if (GLSL_TryInitGPUShader(&tr.shadowmaskStableShader, "shadowmask_stable", attribs, qtrue, extradefines, qtrue, fallbackShader_shadowmask_vp, fallbackShader_shadowmask_fp))
		{
			GLSL_InitUniforms(&tr.shadowmaskStableShader);
			GLSL_SetUniformInt(&tr.shadowmaskStableShader, UNIFORM_SCREENDEPTHMAP, TB_COLORMAP);
			GLSL_SetUniformInt(&tr.shadowmaskStableShader, UNIFORM_SHADOWMAP,  TB_SHADOWMAP);
			GLSL_SetUniformInt(&tr.shadowmaskStableShader, UNIFORM_SHADOWMAP2, TB_SHADOWMAP2);
			GLSL_SetUniformInt(&tr.shadowmaskStableShader, UNIFORM_SHADOWMAP3, TB_SHADOWMAP3);
			GLSL_SetUniformInt(&tr.shadowmaskStableShader, UNIFORM_SHADOWMAP4, TB_SHADOWMAP4);
			GLSL_FinishGPUShader(&tr.shadowmaskStableShader);
			numEtcShaders++;
		}
		else
		{
			ri.Printf(PRINT_WARNING, "^~^~^ SHADOWSTABLE mask shader failed to build - r_shadowStable resolves to legacy\n");
		}
	}


	// GLSL 1.10+ or GL_OES_standard_derivatives extension are required for dFdx() and dFdy() GLSL functions
	if (glRefConfig.glslMajorVersion > 1 || (glRefConfig.glslMajorVersion == 1 && glRefConfig.glslMinorVersion >= 10)
	    || glRefConfig.standardDerivatives)
	{
		attribs = ATTR_POSITION | ATTR_TEXCOORD;
		extradefines[0] = '\0';

		if (qglesMajorVersion < 3 && glRefConfig.standardDerivatives)
		{
			Q_strcat(extradefines, 1024, "#extension GL_OES_standard_derivatives : enable\n");
		}

		if (!GLSL_InitGPUShader(&tr.ssaoShader, "ssao", attribs, qtrue, extradefines, qtrue, fallbackShader_ssao_vp, fallbackShader_ssao_fp))
		{
			ri.Error(ERR_FATAL, "Could not load ssao shader!");
		}

		GLSL_InitUniforms(&tr.ssaoShader);

		GLSL_SetUniformInt(&tr.ssaoShader, UNIFORM_SCREENDEPTHMAP, TB_COLORMAP);

		GLSL_FinishGPUShader(&tr.ssaoShader);

		numEtcShaders++;


		for (i = 0; i < 4; i++)
		{
			attribs = ATTR_POSITION | ATTR_TEXCOORD;
			extradefines[0] = '\0';

			if (qglesMajorVersion < 3 && glRefConfig.standardDerivatives)
			{
				Q_strcat(extradefines, 1024, "#extension GL_OES_standard_derivatives : enable\n");
			}

			if (i & 1)
				Q_strcat(extradefines, 1024, "#define USE_VERTICAL_BLUR\n");
			else
				Q_strcat(extradefines, 1024, "#define USE_HORIZONTAL_BLUR\n");

			if (!(i & 2))
				Q_strcat(extradefines, 1024, "#define USE_DEPTH\n");


			if (!GLSL_InitGPUShader(&tr.depthBlurShader[i], "depthBlur", attribs, qtrue, extradefines, qtrue, fallbackShader_depthblur_vp, fallbackShader_depthblur_fp))
			{
				ri.Error(ERR_FATAL, "Could not load depthBlur shader!");
			}
		
			GLSL_InitUniforms(&tr.depthBlurShader[i]);

			GLSL_SetUniformInt(&tr.depthBlurShader[i], UNIFORM_SCREENIMAGEMAP, TB_COLORMAP);
			GLSL_SetUniformInt(&tr.depthBlurShader[i], UNIFORM_SCREENDEPTHMAP, TB_LIGHTMAP);

			GLSL_FinishGPUShader(&tr.depthBlurShader[i]);

			numEtcShaders++;
		}
	}

#if 0
	attribs = ATTR_POSITION | ATTR_TEXCOORD;
	extradefines[0] = '\0';

	if (!GLSL_InitGPUShader(&tr.testcubeShader, "testcube", attribs, qtrue, extradefines, qtrue, NULL, NULL))
	{
		ri.Error(ERR_FATAL, "Could not load testcube shader!");
	}

	GLSL_InitUniforms(&tr.testcubeShader);

	GLSL_SetUniformInt(&tr.testcubeShader, UNIFORM_TEXTUREMAP, TB_COLORMAP);

	GLSL_FinishGPUShader(&tr.testcubeShader);

	numEtcShaders++;
#endif


	// HZM gl2 MSAA (plan P2a, ME-F3). MSAA programs exist only on the new path at >= 2 samples, carry the sample
	// count as a define, and are TRY-built: a failure sets tr.msaaCompileFailed and R_Init re-initialises in place
	// at 0 (R_Msaa_AfterGLSL). Every program above is untouched, so MSAA-off sources stay byte-identical.
	// r_msaaDebugFailCompile 1 injects an #error to prove that fallback.
	if (tr.msaaNewPath && tr.msaaSamples >= 2)
	{
		attribs = ATTR_POSITION | ATTR_TEXCOORD;
		extradefines[0] = '\0';
		Q_strcat(extradefines, sizeof(extradefines), va("#define MSAA_SAMPLES %d\n", tr.msaaSamples));
		if (r_msaaDebugFailCompile && r_msaaDebugFailCompile->integer)
			Q_strcat(extradefines, sizeof(extradefines), "#error r_msaaDebugFailCompile\n");

		if (GLSL_TryInitGPUShader(&tr.msaaResolveDepthShader, "msaa_depthresolve", attribs, qtrue, extradefines, qtrue, fallbackShader_texturecolor_vp, fallbackShader_msaa_depthresolve_fp))
		{
			GLSL_InitUniforms(&tr.msaaResolveDepthShader);
			GLSL_SetUniformInt(&tr.msaaResolveDepthShader, UNIFORM_SCREENDEPTHMAP, TB_COLORMAP);
			GLSL_FinishGPUShader(&tr.msaaResolveDepthShader);
			numEtcShaders++;
		}
		else
		{
			tr.msaaCompileFailed = qtrue;
			ri.Printf(PRINT_WARNING, "^~^~^ MSAA shader msaa_depthresolve failed to build at %d samples\n", tr.msaaSamples);
		}

		// P2b: the tone-exact MRT colour resolve (out_Color -> renderImage, out_Linear -> sceneLinearImage)
		s_glslFragData1 = "out_Linear";
		if (GLSL_TryInitGPUShader(&tr.msaaResolveColorShader, "msaa_colorresolve", attribs, qtrue, extradefines, qtrue, fallbackShader_texturecolor_vp, fallbackShader_msaa_colorresolve_fp))
		{
			GLSL_InitUniforms(&tr.msaaResolveColorShader);
			GLSL_SetUniformInt(&tr.msaaResolveColorShader, UNIFORM_TEXTUREMAP, TB_COLORMAP);
			GLSL_SetUniformInt(&tr.msaaResolveColorShader, UNIFORM_SCREENIMAGEMAP, TB_NORMALMAP);
			GLSL_SetUniformInt(&tr.msaaResolveColorShader, UNIFORM_LEVELSMAP, TB_LEVELSMAP);
			GLSL_FinishGPUShader(&tr.msaaResolveColorShader);
			numEtcShaders++;
		}
		else
		{
			tr.msaaCompileFailed = qtrue;
			ri.Printf(PRINT_WARNING, "^~^~^ MSAA shader msaa_colorresolve failed to build at %d samples\n", tr.msaaSamples);
		}
		s_glslFragData1 = NULL;
	}

	endTime = ri.Milliseconds();

	ri.Printf(PRINT_ALL, "loaded %i GLSL shaders (%i gen %i light %i etc) in %5.2f seconds\n", 
		numGenShaders + numLightShaders + numEtcShaders, numGenShaders, numLightShaders, 
		numEtcShaders, (endTime - startTime) / 1000.0);
}

/*
HZM coop [2026-09-27] realistic lightning: the sky-pass program, built on the FIRST lit frame (RB_HzmLt_SkyPass), never
at start-up - so a player who never turns the feature on never compiles it. Same shape as the post-FX programs.
*/
qboolean GLSL_HzmLtInitSkyShader(void)
{
	int  attribs = ATTR_POSITION | ATTR_TEXCOORD;
	char extradefines[1024];

	extradefines[0] = 0;
	if (!GLSL_InitGPUShader(&tr.hzmLtSkyShader, "hzmlt_sky", attribs, qtrue, extradefines, qtrue, fallbackShader_hzmlt_sky_vp, fallbackShader_hzmlt_sky_fp))
	{
		return qfalse;
	}
	GLSL_InitUniforms(&tr.hzmLtSkyShader);
	GLSL_SetUniformInt(&tr.hzmLtSkyShader, UNIFORM_DIFFUSEMAP, TB_DIFFUSEMAP);
	GLSL_FinishGPUShader(&tr.hzmLtSkyShader);
	return qtrue;
}

void GLSL_ShutdownGPUShaders(void)
{
	int i;

	ri.Printf(PRINT_ALL, "------- GLSL_ShutdownGPUShaders -------\n");

	for (i = 0; i < ATTR_INDEX_COUNT && i < glRefConfig.maxVertexAttribs; i++)
		qglDisableVertexAttribArray(i);

	GL_BindNullProgram();

	for ( i = 0; i < GENERICDEF_COUNT; i++)
		GLSL_DeleteGPUShader(&tr.genericShader[i]);

	GLSL_DeleteGPUShader(&tr.textureColorShader);

	for ( i = 0; i < FOGDEF_COUNT; i++)
		GLSL_DeleteGPUShader(&tr.fogShader[i]);

	for ( i = 0; i < DLIGHTDEF_COUNT; i++)
		GLSL_DeleteGPUShader(&tr.dlightShader[i]);

	for ( i = 0; i < LIGHTDEF_COUNT; i++)
		GLSL_DeleteGPUShader(&tr.lightallShader[i]);

	for ( i = 0; i < SHADOWMAPDEF_COUNT; i++)
		GLSL_DeleteGPUShader(&tr.shadowmapShader[i]);

	GLSL_DeleteGPUShader(&tr.pshadowShader);
	GLSL_DeleteGPUShader(&tr.down4xShader);
	GLSL_DeleteGPUShader(&tr.bokehShader);
	GLSL_DeleteGPUShader(&tr.tonemapShader);
	GLSL_DeleteGPUShader(&tr.tonemapHzmShader);
	GLSL_DeleteGPUShader(&tr.bloomBrightShader);
	GLSL_DeleteGPUShader(&tr.bloomBlurShader);
	GLSL_DeleteGPUShader(&tr.fxaaShader);
	GLSL_DeleteGPUShader(&tr.sharpenShader);
	GLSL_DeleteGPUShader(&tr.fsrEasuShader);      // HZM render scale (r_renderScale)
	GLSL_DeleteGPUShader(&tr.fsrRcasShader);
	GLSL_DeleteGPUShader(&tr.fsrDownscaleShader);
	GLSL_DeleteGPUShader(&tr.rainDropsShader);
	GLSL_DeleteGPUShader(&tr.hzmWaterShader[0]);      // HZM water pass
	GLSL_DeleteGPUShader(&tr.hzmWaterShader[1]);
	GLSL_DeleteGPUShader(&tr.heatHazeShader);
	GLSL_DeleteGPUShader(&tr.lowHealthShader);
	GLSL_DeleteGPUShader(&tr.suppressionShader);
	GLSL_DeleteGPUShader(&tr.hitBloodShader);   // HZM coop [user 08-02]
	GLSL_DeleteGPUShader(&tr.dofShader);
	GLSL_DeleteGPUShader(&tr.underwaterShader);
	if (tr.hzmLtSkyShaderOk) {
		GLSL_DeleteGPUShader(&tr.hzmLtSkyShader);   // HZM coop [2026-09-27] lightning (only if it was ever built)
		tr.hzmLtSkyShaderOk    = qfalse;
		tr.hzmLtSkyShaderTried = qfalse;
	}
	GLSL_DeleteGPUShader(&tr.bloodSpatterShader);
	GLSL_DeleteGPUShader(&tr.chromabShader);
	GLSL_DeleteGPUShader(&tr.motionBlurShader);
	GLSL_DeleteGPUShader(&tr.filmgrainShader);
	GLSL_DeleteGPUShader(&tr.frostShader);
	GLSL_DeleteGPUShader(&tr.globalFogShader);
	GLSL_DeleteGPUShader(&tr.msaaResolveDepthShader);   // HZM MSAA P2a (a no-op when it was never built)
	GLSL_DeleteGPUShader(&tr.msaaResolveColorShader);   // HZM MSAA P2b (same)

	for ( i = 0; i < 2; i++)
		GLSL_DeleteGPUShader(&tr.calclevels4xShader[i]);

	GLSL_DeleteGPUShader(&tr.shadowmaskShader);
	GLSL_DeleteGPUShader(&tr.shadowmaskStableShader);   // HZM gl2 P3
	GLSL_DeleteGPUShader(&tr.ssaoShader);

	for ( i = 0; i < 4; i++)
		GLSL_DeleteGPUShader(&tr.depthBlurShader[i]);
}


void GLSL_BindProgram(shaderProgram_t * program)
{
	GLuint programObject = program ? program->program : 0;
	char *name = program ? program->name : "NULL";

	if(r_logFile->integer)
	{
		// don't just call LogComment, or we will get a call to va() every frame!
		GLimp_LogComment(va("--- GLSL_BindProgram( %s ) ---\n", name));
	}

	if (GL_UseProgram(programObject))
		backEnd.pc.c_glslShaderBinds++;
}


// HZM coop (bug-2508): kill switch for the gl2 alphaGen dot / oneMinusDot port. Default ON.
// r_hzmAlphaGenDot 0 restores the pre-port behaviour exactly: the permutation select below
// stops asking for USE_RGBAGEN and tr_shade.c uploads AGEN_IDENTITY in place of the dot mode,
// so a CalcColor compiled in for some other reason cannot run the new branch either.
// Fetched lazily (same pattern as r_skeldiag in tr_shade.c) so tr_init.c is not touched.
qboolean GLSL_HzmAlphaGenDotEnabled(void)
{
	static cvar_t *r_hzmAlphaGenDot = NULL;

	if (!r_hzmAlphaGenDot) {
		r_hzmAlphaGenDot = ri.Cvar_Get("r_hzmAlphaGenDot", "1", CVAR_ARCHIVE);
	}
	return (qboolean)(r_hzmAlphaGenDot->integer != 0);
}

shaderProgram_t *GLSL_GetGenericShaderProgram(int stage)
{
	shaderStage_t *pStage = tess.xstages[stage];
	int shaderAttribs = 0;

	if (tess.fogNum && pStage->adjustColorsForFog)
	{
		shaderAttribs |= GENERICDEF_USE_FOG;
	}

	switch (pStage->rgbGen)
	{
		case CGEN_LIGHTING_DIFFUSE:
			shaderAttribs |= GENERICDEF_USE_RGBAGEN;
			break;
		// HZM gl2 [2026-09-25] Phase R2: rgbGen dot / oneMinusDot. The branch lives in CalcColor, which only
		// exists under USE_RGBAGEN, so this select is what makes the port exist at all (the bug-2508 recipe).
		// Off (the default, and always on an Omaha BSP) the permutation is exactly the pre-port one.
		case CGEN_DOT:
		case CGEN_ONE_MINUS_DOT:
			if (R_HZM_RgbGenDotOn()) {
				shaderAttribs |= GENERICDEF_USE_RGBAGEN;
			}
			break;
		default:
			break;
	}

	switch (pStage->alphaGen)
	{
		case AGEN_LIGHTING_SPECULAR:
		case AGEN_PORTAL:
			shaderAttribs |= GENERICDEF_USE_RGBAGEN;
			break;

		// HZM gl2 parity (bug-1249): alphaGen sCoord/tCoord are PARSED at tr_shader.c:1712-1714 and
		// consumed by NOTHING - baseColor[3] kept its 1.0f initialiser and the stage drew fully
		// OPAQUE. gl1 implements both at renderergl1/tr_shade.c:1143,1155 via
		// RB_CalcAlphaFromTexCoords. deepbluesea_shoreline uses them to fade the ocean layers out
		// across the patch, so on gl2 that fade never happened: a hard straight waterline, and a
		// full-strength additive band where the shoreline shader abuts the open ocean.
		//
		// CalcColor lives inside #if defined(USE_RGBAGEN) in generic_vp.glsl, so THIS permutation
		// select is what compiles the new branch - without it the whole change is a silent no-op.
		case AGEN_SCOORD:
		case AGEN_TCOORD:
			if (r_hzmAlphaGenCoord && r_hzmAlphaGenCoord->integer) {
				shaderAttribs |= GENERICDEF_USE_RGBAGEN;
			}
			break;

		// HZM coop (bug-2508): alphaGen dot / oneMinusDot - same story as sCoord above, the
		// branch lives in CalcColor so this select is what makes it exist. Only reachable on
		// the generic path: a deform-free shader still collapses into lightall (tr_shader.c
		// CollapseStagesToGLSL skips only LIGHTING_SPECULAR/PORTAL), the bug-2486 gap.
		case AGEN_DOT:
		case AGEN_ONE_MINUS_DOT:
			// HZM gl2 [2026-09-26] searchlights S1: an additive dot stage takes the dot in RGB (gl1) through the
			// same CalcColor, so it needs this permutation even if r_hzmAlphaGenDot was saved 0. R_HZM_AlphaDotToRgb
			// (tr_shade_calc.c) is the ONE predicate shared with the uniform upload in tr_shade.c.
			if (GLSL_HzmAlphaGenDotEnabled() || R_HZM_AlphaDotToRgb(pStage)) {
				shaderAttribs |= GENERICDEF_USE_RGBAGEN;
			}
			break;

		default:
			break;
	}

	if (pStage->bundle[0].tcGen != TCGEN_TEXTURE)
	{
		shaderAttribs |= GENERICDEF_USE_TCGEN_AND_TCMOD;
	}

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

	if (pStage->bundle[0].numTexMods)
	{
		shaderAttribs |= GENERICDEF_USE_TCGEN_AND_TCMOD;
	}

	return &tr.genericShader[shaderAttribs];
}
