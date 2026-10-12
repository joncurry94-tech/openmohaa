/*
===========================================================================
Copyright (C) 2007-2009 Robert Beckebans <trebor_7@users.sourceforge.net>

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
// tr_vbo.c
// HZM gl2 [render-perf 3] SDL_GL_GetProcAddress / SDL_GL_ExtensionSupported for the mapped tess stream
#ifdef USE_INTERNAL_SDL_HEADERS
#	include "SDL.h"
#else
#	include <SDL.h>
#endif

#include "tr_local.h"


void R_VaoPackTangent(int16_t *out, vec4_t v)
{
	out[0] = v[0] * 32767.0f + (v[0] > 0.0f ? 0.5f : -0.5f);
	out[1] = v[1] * 32767.0f + (v[1] > 0.0f ? 0.5f : -0.5f);
	out[2] = v[2] * 32767.0f + (v[2] > 0.0f ? 0.5f : -0.5f);
	out[3] = v[3] * 32767.0f + (v[3] > 0.0f ? 0.5f : -0.5f);
}

void R_VaoPackNormal(int16_t *out, vec3_t v)
{
	out[0] = v[0] * 32767.0f + (v[0] > 0.0f ? 0.5f : -0.5f);
	out[1] = v[1] * 32767.0f + (v[1] > 0.0f ? 0.5f : -0.5f);
	out[2] = v[2] * 32767.0f + (v[2] > 0.0f ? 0.5f : -0.5f);
	out[3] = 0;
}

void R_VaoPackColor(uint16_t *out, vec4_t c)
{
	out[0] = c[0] * 65535.0f + 0.5f;
	out[1] = c[1] * 65535.0f + 0.5f;
	out[2] = c[2] * 65535.0f + 0.5f;
	out[3] = c[3] * 65535.0f + 0.5f;
}

void R_VaoUnpackTangent(vec4_t v, int16_t *pack)
{
	v[0] = pack[0] / 32767.0f;
	v[1] = pack[1] / 32767.0f;
	v[2] = pack[2] / 32767.0f;
	v[3] = pack[3] / 32767.0f;
}

void R_VaoUnpackNormal(vec3_t v, int16_t *pack)
{
	v[0] = pack[0] / 32767.0f;
	v[1] = pack[1] / 32767.0f;
	v[2] = pack[2] / 32767.0f;
}

void Vao_SetVertexPointers(vao_t *vao)
{
	int attribIndex;

	// set vertex pointers
	for (attribIndex = 0; attribIndex < ATTR_INDEX_COUNT; attribIndex++)
	{
		uint32_t attribBit = 1 << attribIndex;
		vaoAttrib_t *vAtb = &vao->attribs[attribIndex];

		if (vAtb->enabled)
		{
			qglVertexAttribPointer(attribIndex, vAtb->count, vAtb->type, vAtb->normalized, vAtb->stride, BUFFER_OFFSET(vAtb->offset));
			if (glRefConfig.vertexArrayObject || !(glState.vertexAttribsEnabled & attribBit))
				qglEnableVertexAttribArray(attribIndex);

			if (!glRefConfig.vertexArrayObject || vao == tess.vao)
				glState.vertexAttribsEnabled |= attribBit;
		}
		else
		{
			// don't disable vertex attribs when using vertex array objects
			// Vao_SetVertexPointers is only called during init when using VAOs, and vertex attribs start disabled anyway
			if (!glRefConfig.vertexArrayObject && (glState.vertexAttribsEnabled & attribBit))
				qglDisableVertexAttribArray(attribIndex);

			if (!glRefConfig.vertexArrayObject || vao == tess.vao)
				glState.vertexAttribsEnabled &= ~attribBit;
		}
	}
}

/*
============
R_CreateVao
============
*/
vao_t *R_CreateVao(const char *name, byte *vertexes, int vertexesSize, byte *indexes, int indexesSize, vaoUsage_t usage)
{
	vao_t          *vao;
	int				glUsage;

	switch (usage)
	{
		case VAO_USAGE_STATIC:
			glUsage = GL_STATIC_DRAW;
			break;

		case VAO_USAGE_DYNAMIC:
			glUsage = GL_DYNAMIC_DRAW;
			break;

		default:
			ri.Error(ERR_FATAL, "bad vaoUsage_t given: %i", usage);
			return NULL;
	}

	if(strlen(name) >= MAX_QPATH)
	{
		ri.Error(ERR_DROP, "R_CreateVao: \"%s\" is too long", name);
	}

	if ( tr.numVaos == MAX_VAOS ) {
		ri.Error( ERR_DROP, "R_CreateVao: MAX_VAOS hit");
	}

	R_IssuePendingRenderCommands();

	vao = tr.vaos[tr.numVaos] = ri.Hunk_Alloc(sizeof(*vao), h_low);
	tr.numVaos++;

	memset(vao, 0, sizeof(*vao));

	Q_strncpyz(vao->name, name, sizeof(vao->name));


	if (glRefConfig.vertexArrayObject)
	{
		qglGenVertexArrays(1, &vao->vao);
		qglBindVertexArray(vao->vao);
	}


	vao->vertexesSize = vertexesSize;

	qglGenBuffers(1, &vao->vertexesVBO);

	qglBindBuffer(GL_ARRAY_BUFFER, vao->vertexesVBO);
	qglBufferData(GL_ARRAY_BUFFER, vertexesSize, vertexes, glUsage);


	vao->indexesSize = indexesSize;

	qglGenBuffers(1, &vao->indexesIBO);

	qglBindBuffer(GL_ELEMENT_ARRAY_BUFFER, vao->indexesIBO);
	qglBufferData(GL_ELEMENT_ARRAY_BUFFER, indexesSize, indexes, glUsage);


	glState.currentVao = vao;

	GL_CheckErrors();

	return vao;
}

/*
============
R_CreateVao2
============
*/
vao_t *R_CreateVao2(const char *name, int numVertexes, srfVert_t *verts, int numIndexes, glIndex_t *indexes)
{
	vao_t          *vao;
	int             i;

	byte           *data;
	int             dataSize;
	int             dataOfs;

	int				glUsage = GL_STATIC_DRAW;

	if(!numVertexes || !numIndexes)
		return NULL;

	if(strlen(name) >= MAX_QPATH)
	{
		ri.Error(ERR_DROP, "R_CreateVao2: \"%s\" is too long", name);
	}

	if ( tr.numVaos == MAX_VAOS ) {
		ri.Error( ERR_DROP, "R_CreateVao2: MAX_VAOS hit");
	}

	R_IssuePendingRenderCommands();

	vao = tr.vaos[tr.numVaos] = ri.Hunk_Alloc(sizeof(*vao), h_low);
	tr.numVaos++;

	memset(vao, 0, sizeof(*vao));

	Q_strncpyz(vao->name, name, sizeof(vao->name));

	// since these vertex attributes are never altered, interleave them
	vao->attribs[ATTR_INDEX_POSITION      ].enabled = 1;
	vao->attribs[ATTR_INDEX_NORMAL        ].enabled = 1;
	vao->attribs[ATTR_INDEX_TANGENT       ].enabled = 1;
	vao->attribs[ATTR_INDEX_TEXCOORD      ].enabled = 1;
	vao->attribs[ATTR_INDEX_LIGHTCOORD    ].enabled = 1;
	vao->attribs[ATTR_INDEX_COLOR         ].enabled = 1;
	vao->attribs[ATTR_INDEX_LIGHTDIRECTION].enabled = 1;

	vao->attribs[ATTR_INDEX_POSITION      ].count = 3;
	vao->attribs[ATTR_INDEX_NORMAL        ].count = 4;
	vao->attribs[ATTR_INDEX_TANGENT       ].count = 4;
	vao->attribs[ATTR_INDEX_TEXCOORD      ].count = 2;
	vao->attribs[ATTR_INDEX_LIGHTCOORD    ].count = 2;
	vao->attribs[ATTR_INDEX_COLOR         ].count = 4;
	vao->attribs[ATTR_INDEX_LIGHTDIRECTION].count = 4;

	vao->attribs[ATTR_INDEX_POSITION      ].type = GL_FLOAT;
	vao->attribs[ATTR_INDEX_NORMAL        ].type = GL_SHORT;
	vao->attribs[ATTR_INDEX_TANGENT       ].type = GL_SHORT;
	vao->attribs[ATTR_INDEX_TEXCOORD      ].type = GL_FLOAT;
	vao->attribs[ATTR_INDEX_LIGHTCOORD    ].type = GL_FLOAT;
	vao->attribs[ATTR_INDEX_COLOR         ].type = GL_UNSIGNED_SHORT;
	vao->attribs[ATTR_INDEX_LIGHTDIRECTION].type = GL_SHORT;

	vao->attribs[ATTR_INDEX_POSITION      ].normalized = GL_FALSE;
	vao->attribs[ATTR_INDEX_NORMAL        ].normalized = GL_TRUE;
	vao->attribs[ATTR_INDEX_TANGENT       ].normalized = GL_TRUE;
	vao->attribs[ATTR_INDEX_TEXCOORD      ].normalized = GL_FALSE;
	vao->attribs[ATTR_INDEX_LIGHTCOORD    ].normalized = GL_FALSE;
	vao->attribs[ATTR_INDEX_COLOR         ].normalized = GL_TRUE;
	vao->attribs[ATTR_INDEX_LIGHTDIRECTION].normalized = GL_TRUE;

	vao->attribs[ATTR_INDEX_POSITION      ].offset = 0;        dataSize  = sizeof(verts[0].xyz);
	vao->attribs[ATTR_INDEX_NORMAL        ].offset = dataSize; dataSize += sizeof(verts[0].normal);
	vao->attribs[ATTR_INDEX_TANGENT       ].offset = dataSize; dataSize += sizeof(verts[0].tangent);
	vao->attribs[ATTR_INDEX_TEXCOORD      ].offset = dataSize; dataSize += sizeof(verts[0].st);
	vao->attribs[ATTR_INDEX_LIGHTCOORD    ].offset = dataSize; dataSize += sizeof(verts[0].lightmap);
	vao->attribs[ATTR_INDEX_COLOR         ].offset = dataSize; dataSize += sizeof(verts[0].color);
	vao->attribs[ATTR_INDEX_LIGHTDIRECTION].offset = dataSize; dataSize += sizeof(verts[0].lightdir);

	vao->attribs[ATTR_INDEX_POSITION      ].stride = dataSize;
	vao->attribs[ATTR_INDEX_NORMAL        ].stride = dataSize;
	vao->attribs[ATTR_INDEX_TANGENT       ].stride = dataSize;
	vao->attribs[ATTR_INDEX_TEXCOORD      ].stride = dataSize;
	vao->attribs[ATTR_INDEX_LIGHTCOORD    ].stride = dataSize;
	vao->attribs[ATTR_INDEX_COLOR         ].stride = dataSize;
	vao->attribs[ATTR_INDEX_LIGHTDIRECTION].stride = dataSize;


	if (glRefConfig.vertexArrayObject)
	{
		qglGenVertexArrays(1, &vao->vao);
		qglBindVertexArray(vao->vao);
	}


	// create VBO
	dataSize *= numVertexes;
	data = ri.Hunk_AllocateTempMemory(dataSize);
	dataOfs = 0;

	for (i = 0; i < numVertexes; i++)
	{
		// xyz
		memcpy(data + dataOfs, &verts[i].xyz, sizeof(verts[i].xyz));
		dataOfs += sizeof(verts[i].xyz);

		// normal
		memcpy(data + dataOfs, &verts[i].normal, sizeof(verts[i].normal));
		dataOfs += sizeof(verts[i].normal);

		// tangent
		memcpy(data + dataOfs, &verts[i].tangent, sizeof(verts[i].tangent));
		dataOfs += sizeof(verts[i].tangent);

		// texcoords
		memcpy(data + dataOfs, &verts[i].st, sizeof(verts[i].st));
		dataOfs += sizeof(verts[i].st);

		// lightmap texcoords
		memcpy(data + dataOfs, &verts[i].lightmap, sizeof(verts[i].lightmap));
		dataOfs += sizeof(verts[i].lightmap);

		// colors
		memcpy(data + dataOfs, &verts[i].color, sizeof(verts[i].color));
		dataOfs += sizeof(verts[i].color);

		// light directions
		memcpy(data + dataOfs, &verts[i].lightdir, sizeof(verts[i].lightdir));
		dataOfs += sizeof(verts[i].lightdir);
	}

	vao->vertexesSize = dataSize;

	qglGenBuffers(1, &vao->vertexesVBO);

	qglBindBuffer(GL_ARRAY_BUFFER, vao->vertexesVBO);
	qglBufferData(GL_ARRAY_BUFFER, vao->vertexesSize, data, glUsage);


	// create IBO
	vao->indexesSize = numIndexes * sizeof(glIndex_t);

	qglGenBuffers(1, &vao->indexesIBO);

	qglBindBuffer(GL_ELEMENT_ARRAY_BUFFER, vao->indexesIBO);
	qglBufferData(GL_ELEMENT_ARRAY_BUFFER, vao->indexesSize, indexes, glUsage);


	Vao_SetVertexPointers(vao);


	glState.currentVao = vao;

	GL_CheckErrors();

	ri.Hunk_FreeTempMemory(data);

	return vao;
}


/*
============
R_BindVao
============
*/
void R_BindVao(vao_t * vao)
{
	if(!vao)
	{
		//R_BindNullVao();
		ri.Error(ERR_DROP, "R_BindVao: NULL vao");
		return;
	}

	if(r_logFile->integer)
	{
		// don't just call LogComment, or we will get a call to va() every frame!
		GLimp_LogComment(va("--- R_BindVao( %s ) ---\n", vao->name));
	}

	if(glState.currentVao != vao)
	{
		glState.currentVao = vao;

		glState.vertexAttribsInterpolation = 0;
		glState.vertexAnimation = qfalse;
		backEnd.pc.c_vaoBinds++;

		if (glRefConfig.vertexArrayObject)
		{
			qglBindVertexArray(vao->vao);

			// Intel Graphics doesn't save GL_ELEMENT_ARRAY_BUFFER binding with VAO binding.
			if (glRefConfig.intelGraphics || vao == tess.vao)
				qglBindBuffer(GL_ELEMENT_ARRAY_BUFFER, vao->indexesIBO);

			// tess VAO always has buffers bound
			if (vao == tess.vao)
				qglBindBuffer(GL_ARRAY_BUFFER, vao->vertexesVBO);
		}
		else
		{
			qglBindBuffer(GL_ARRAY_BUFFER, vao->vertexesVBO);
			qglBindBuffer(GL_ELEMENT_ARRAY_BUFFER, vao->indexesIBO);

			// tess VAO doesn't have vertex pointers set until data is uploaded
			if (vao != tess.vao)
				Vao_SetVertexPointers(vao);
		}
	}
}

/*
============
R_BindNullVao
============
*/
void R_BindNullVao(void)
{
	GLimp_LogComment("--- R_BindNullVao ---\n");

	if(glState.currentVao)
	{
		if (glRefConfig.vertexArrayObject)
		{
			qglBindVertexArray(0);

			// why you no save GL_ELEMENT_ARRAY_BUFFER binding, Intel?
			if (1) qglBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
		}
		else
		{
			qglBindBuffer(GL_ARRAY_BUFFER, 0);
			qglBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
		}
		glState.currentVao = NULL;
	}

	GL_CheckErrors();
}


/*
============
HZM gl2 [bug-3483] THE TESS STREAM

Profile of Omaha (m3l1a, ~230 skeletal soldiers): 29 % of the main thread inside RB_UpdateTessVao, nearly all of it in
the GL driver. The function orphaned the whole dynamic buffer (glBufferData NULL, vertex AND index) before every
batch, and a skeletal model is one batch per surface per view - well over a thousand buffer re-allocations a frame.

The buffer is a ring now, HZM_TESS_STREAM_MUL batches long. Each batch is written behind a cursor (the layout is
still one region per attribute, so "behind the cursor" is the same vertex index in every region) and drawn with
glDrawElementsBaseVertex; the buffer is orphaned only when the next batch would not fit, so nothing the GPU may
still be reading is ever overwritten. Every dynamic draw in this renderer goes through R_DrawElements, which is the
only other place that had to change.

r_hzmTessStream 0 = the old behaviour (orphan per batch, base 0), live. Without glDrawElementsBaseVertex (GL < 3.2)
the buffer keeps its old size and the old path runs.
============
*/
#define HZM_TESS_STREAM_MUL 64

hzmDrawElementsBaseVertex_t qglDrawElementsBaseVertex;
cvar_t *r_hzmTessStream;

static int s_tessStreamVerts;		// vertices the tess VBO holds per attribute region
static int s_tessStreamIndexes;
static int s_tessCursorV, s_tessCursorI;	// next free vertex / index
static int s_tessDrawBaseV, s_tessDrawFirstI;	// where the batch being drawn lives

/*
============
HZM gl2 [render-perf 3] THE MAPPED TESS STREAM

After the ring (bug-3483) every dynamic batch still cost one glBufferSubData per vertex attribute plus one for the
indexes - four to eight driver calls a batch, a couple of thousand batches a frame on Omaha, each of them taking the
driver's lock and copying the data into its command queue. With GL 4.4 buffer storage the buffer is mapped ONCE,
persistently, and a batch is written with plain memcpy: no driver call at all until the draw.

A persistent buffer cannot be orphaned, so "is the GPU still reading this?" is answered with fences instead. The
store is r_hzmTessMapSegs segments (64; latched, 16..256) of HZM_TESS_MAP_SEGBATCH full batches each, about 1.2 MB
a segment, filled in a circle. 64 segments hold a little over one Omaha frame (~950,000 vertices). The first batch
of every frame drops ONE fence behind everything drawn so far, and every segment written in the frame before is
recorded as covered by it. Before a segment is written again its fence must have signalled; normally that was two
or more frames ago and the answer is already known, so the whole scheme costs one glFenceSync and about one
zero-timeout glClientWaitSync a frame. If the GPU really is that far behind - or one frame goes all the way round
the store - this waits for it (counted: RPSTAT "stalls"); nothing the GPU may still read is ever overwritten.

It is a SECOND vertex array object beside the classic tess VAO, with the same attribute layout over a larger
buffer, so the two paths can be switched live: tess.vao simply points at one or the other, chosen per batch in
RB_UpdateTessVao (r_hzmTessMap 0 = the classic ring). Each VAO keeps its own record of which arrays are enabled.
No buffer storage / sync objects / VAOs / base-vertex draws: the mapped VAO is never created and nothing changes.
============
*/
#define HZM_TESS_MAP_SEGS_MAX	256
#define HZM_TESS_MAP_SEGBATCH	8
#define HZM_TESS_MAP_FENCES		8
#define HZM_TESS_SEG_FREE		0		// never written, or its fence is known to have signalled
#define HZM_TESS_SEG_PENDING	-1		// written this frame: the next frame's fence will cover it

#ifndef GL_MAP_WRITE_BIT
#define GL_MAP_WRITE_BIT					0x0002
#endif
#ifndef GL_MAP_PERSISTENT_BIT
#define GL_MAP_PERSISTENT_BIT				0x0040
#endif
#ifndef GL_MAP_COHERENT_BIT
#define GL_MAP_COHERENT_BIT					0x0080
#endif
#ifndef GL_SYNC_GPU_COMMANDS_COMPLETE
#define GL_SYNC_GPU_COMMANDS_COMPLETE		0x9117
#endif
#ifndef GL_SYNC_FLUSH_COMMANDS_BIT
#define GL_SYNC_FLUSH_COMMANDS_BIT			0x00000001
#endif
#ifndef GL_TIMEOUT_EXPIRED
#define GL_TIMEOUT_EXPIRED					0x911B
#endif

typedef void (APIENTRY *hzmBufferStorage_t)(GLenum target, ptrdiff_t size, const void *data, GLbitfield flags);
typedef void *(APIENTRY *hzmMapBufferRange_t)(GLenum target, ptrdiff_t offset, ptrdiff_t length, GLbitfield access);
typedef void *(APIENTRY *hzmFenceSync_t)(GLenum condition, GLbitfield flags);
typedef GLenum (APIENTRY *hzmClientWaitSync_t)(void *sync, GLbitfield flags, uint64_t timeout);
typedef void (APIENTRY *hzmDeleteSync_t)(void *sync);

static hzmBufferStorage_t	qglBufferStorageHzm;
static hzmMapBufferRange_t	qglMapBufferRangeHzm;
static hzmFenceSync_t		qglFenceSyncHzm;
static hzmClientWaitSync_t	qglClientWaitSyncHzm;
static hzmDeleteSync_t		qglDeleteSyncHzm;

cvar_t *r_hzmTessMap;

static vao_t	*s_tessClassicVao, *s_tessMapVao;
static uint32_t	s_tessVaoEnabled[2];		// vertexAttribsEnabled of [classic, mapped] while it is not the live one
static byte		*s_tessMapV, *s_tessMapI;	// the mapped vertex / index store
static int		s_tessMapSegVerts, s_tessMapSegIndexes;
static int		s_tessMapSeg;
static int		s_tessMapCurV, s_tessMapCurI;
static int		s_tessMapSegs;				// segments in the store (r_hzmTessMapSegs when it was created)
static int		s_tessMapSegFence[HZM_TESS_MAP_SEGS_MAX];	// FREE, PENDING, or the sequence number of the covering fence
static void		*s_tessMapFence[HZM_TESS_MAP_FENCES];	// fence number n lives in slot n % HZM_TESS_MAP_FENCES
static int		s_tessMapFenceSeq;			// number of the newest fence (they start at 1)
static int		s_tessMapFenceDone;			// every fence up to this number has signalled
static int		s_tessMapFrame;

static void R_TessMapCreate(void)
{
	vao_t	*vao;
	int		attribIndex, offset, totalVerts, totalIndexes, vertexesSize, indexesSize;
	GLbitfield flags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;

	s_tessMapVao = NULL;
	s_tessMapV = s_tessMapI = NULL;
	memset(s_tessMapFence, 0, sizeof(s_tessMapFence));
	memset(s_tessMapSegFence, 0, sizeof(s_tessMapSegFence));
	s_tessMapSeg = 0;
	s_tessMapCurV = s_tessMapCurI = 0;
	s_tessMapFenceSeq = s_tessMapFenceDone = 0;
	s_tessMapFrame = -1;

	qglBufferStorageHzm = (hzmBufferStorage_t)SDL_GL_GetProcAddress("glBufferStorage");
	qglMapBufferRangeHzm = (hzmMapBufferRange_t)SDL_GL_GetProcAddress("glMapBufferRange");
	qglFenceSyncHzm = (hzmFenceSync_t)SDL_GL_GetProcAddress("glFenceSync");
	qglClientWaitSyncHzm = (hzmClientWaitSync_t)SDL_GL_GetProcAddress("glClientWaitSync");
	qglDeleteSyncHzm = (hzmDeleteSync_t)SDL_GL_GetProcAddress("glDeleteSync");

	if (!qglBufferStorageHzm || !qglMapBufferRangeHzm || !qglFenceSyncHzm || !qglClientWaitSyncHzm || !qglDeleteSyncHzm
		|| !qglDrawElementsBaseVertex || !glRefConfig.vertexArrayObject
		|| !SDL_GL_ExtensionSupported("GL_ARB_buffer_storage") || tr.numVaos == MAX_VAOS) {
		ri.Printf(PRINT_ALL, "...mapped tess stream: not available\n");
		return;
	}

	s_tessMapSegVerts = SHADER_MAX_VERTEXES * HZM_TESS_MAP_SEGBATCH;
	s_tessMapSegIndexes = SHADER_MAX_INDEXES * HZM_TESS_MAP_SEGBATCH;
	s_tessMapSegs = ri.Cvar_Get("r_hzmTessMapSegs", "64", CVAR_LATCH)->integer;
	if (s_tessMapSegs < 16) {
		s_tessMapSegs = 16;
	} else if (s_tessMapSegs > HZM_TESS_MAP_SEGS_MAX) {
		s_tessMapSegs = HZM_TESS_MAP_SEGS_MAX;
	}
	totalVerts = s_tessMapSegVerts * s_tessMapSegs;
	totalIndexes = s_tessMapSegIndexes * s_tessMapSegs;

	vao = tr.vaos[tr.numVaos] = ri.Hunk_Alloc(sizeof(*vao), h_low);
	tr.numVaos++;
	memset(vao, 0, sizeof(*vao));
	Q_strncpyz(vao->name, "tessVertexArray_mapped", sizeof(vao->name));

	// the classic VAO's layout - one region per attribute - over the larger vertex count
	offset = 0;
	for (attribIndex = 0; attribIndex < ATTR_INDEX_COUNT; attribIndex++) {
		vao->attribs[attribIndex] = s_tessClassicVao->attribs[attribIndex];
		vao->attribs[attribIndex].offset = offset;
		if (vao->attribs[attribIndex].enabled) {
			offset += vao->attribs[attribIndex].stride * totalVerts;
		}
	}
	vertexesSize = offset;
	indexesSize = sizeof(tess.indexes[0]) * totalIndexes;

	qglGenVertexArrays(1, &vao->vao);
	qglBindVertexArray(vao->vao);

	vao->vertexesSize = vertexesSize;
	qglGenBuffers(1, &vao->vertexesVBO);
	qglBindBuffer(GL_ARRAY_BUFFER, vao->vertexesVBO);
	qglBufferStorageHzm(GL_ARRAY_BUFFER, vertexesSize, NULL, flags);
	s_tessMapV = (byte *)qglMapBufferRangeHzm(GL_ARRAY_BUFFER, 0, vertexesSize, flags);

	vao->indexesSize = indexesSize;
	qglGenBuffers(1, &vao->indexesIBO);
	qglBindBuffer(GL_ELEMENT_ARRAY_BUFFER, vao->indexesIBO);
	qglBufferStorageHzm(GL_ELEMENT_ARRAY_BUFFER, indexesSize, NULL, flags);
	s_tessMapI = (byte *)qglMapBufferRangeHzm(GL_ELEMENT_ARRAY_BUFFER, 0, indexesSize, flags);

	glState.currentVao = vao;

	// pointers + all arrays enabled, without touching the classic VAO's record of its own enables
	for (attribIndex = 0; attribIndex < ATTR_INDEX_COUNT; attribIndex++) {
		vaoAttrib_t *vAtb = &vao->attribs[attribIndex];

		if (vAtb->enabled) {
			qglVertexAttribPointer(attribIndex, vAtb->count, vAtb->type, vAtb->normalized, vAtb->stride, BUFFER_OFFSET(vAtb->offset));
			qglEnableVertexAttribArray(attribIndex);
			s_tessVaoEnabled[1] |= 1u << attribIndex;
		}
	}

	// swallow any error from the calls above: a failure means "no mapped stream", never a broken renderer
	while (qglGetError() != GL_NO_ERROR) {
		s_tessMapV = NULL;
	}
	if (!s_tessMapV || !s_tessMapI) {
		ri.Printf(PRINT_ALL, "...mapped tess stream: buffer storage failed, staying on the classic stream\n");
		s_tessMapV = s_tessMapI = NULL;
		return;		// the VAO stays in tr.vaos so R_ShutdownVaos frees what was made
	}
	s_tessMapVao = vao;
	ri.Printf(PRINT_ALL, "...mapped tess stream: %d segments x %d vertices, %d MB\n", s_tessMapSegs, s_tessMapSegVerts,
		(vertexesSize + indexesSize) / (1024 * 1024));
}

/*
R_TessMapSelect - point tess.vao at the classic or the mapped VAO (between batches only: called from
RB_UpdateTessVao before it binds), carrying each one's enabled-array record across the switch
*/
static qboolean R_TessMapSelect(void)
{
	qboolean wantMap = (qboolean)(s_tessMapVao && r_hzmTessMap->integer && r_hzmTessStream->integer);
	vao_t	*want = wantMap ? s_tessMapVao : s_tessClassicVao;

	if (tess.vao != want) {
		s_tessVaoEnabled[tess.vao == s_tessMapVao ? 1 : 0] = glState.vertexAttribsEnabled;
		tess.vao = want;
		glState.vertexAttribsEnabled = s_tessVaoEnabled[wantMap ? 1 : 0];
	}
	return wantMap;
}

/*
R_TessMapWaitFence - fence number seq (and with it every older one) has signalled when this returns
*/
static void R_TessMapWaitFence(int seq)
{
	void *f;

	if (seq <= s_tessMapFenceSeq - HZM_TESS_MAP_FENCES) {
		// so old that its slot has been reused: the oldest fence still held was dropped after it, so that one
		// having signalled proves this one has
		seq = s_tessMapFenceSeq - HZM_TESS_MAP_FENCES + 1;
	}
	if (seq <= s_tessMapFenceDone) {
		return;
	}
	f = s_tessMapFence[seq % HZM_TESS_MAP_FENCES];
	if (f && qglClientWaitSyncHzm(f, 0, 0) == GL_TIMEOUT_EXPIRED) {
		int tries = 0;

		hzmPerf.tessStalls++;
		while (qglClientWaitSyncHzm(f, GL_SYNC_FLUSH_COMMANDS_BIT, 50000000) == GL_TIMEOUT_EXPIRED && ++tries < 40) {
		}
	}
	s_tessMapFenceDone = seq;
}

/*
R_TessMapNewFence - one fence behind everything drawn so far; every PENDING segment is covered by it
*/
static void R_TessMapNewFence(void)
{
	int i, slot;

	s_tessMapFenceSeq++;
	slot = s_tessMapFenceSeq % HZM_TESS_MAP_FENCES;
	if (s_tessMapFence[slot]) {
		qglDeleteSyncHzm(s_tessMapFence[slot]);
	}
	s_tessMapFence[slot] = qglFenceSyncHzm(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
	for (i = 0; i < s_tessMapSegs; i++) {
		if (s_tessMapSegFence[i] == HZM_TESS_SEG_PENDING) {
			s_tessMapSegFence[i] = s_tessMapFenceSeq;
		}
	}
}

/*
R_TessMapPlace - where the batch of numVerts / numIndexes goes in the mapped store
*/
static void R_TessMapPlace(int numVerts, int numIndexes, int *baseV, int *baseI)
{
	int seg = s_tessMapSeg;

	if (s_tessMapFrame != tr.frameCount) {
		s_tessMapFrame = tr.frameCount;
		R_TessMapNewFence();	// behind every draw of the frames before this one
	}

	if (s_tessMapCurV + numVerts > (seg + 1) * s_tessMapSegVerts
		|| s_tessMapCurI + numIndexes > (seg + 1) * s_tessMapSegIndexes) {
		seg = (seg + 1) % s_tessMapSegs;
		if (s_tessMapSegFence[seg] == HZM_TESS_SEG_PENDING) {
			// all the way round the store inside one frame: fence what has been drawn and wait for it
			R_TessMapNewFence();
		}
		if (s_tessMapSegFence[seg] > 0) {
			R_TessMapWaitFence(s_tessMapSegFence[seg]);
		}
		s_tessMapSeg = seg;
		s_tessMapCurV = seg * s_tessMapSegVerts;
		s_tessMapCurI = seg * s_tessMapSegIndexes;
	}
	// the part of the current segment behind the cursor was drawn from in this frame (or is covered by this
	// frame's fence); writing past the cursor never touches it
	s_tessMapSegFence[seg] = HZM_TESS_SEG_PENDING;
	*baseV = s_tessMapCurV;
	*baseI = s_tessMapCurI;
	s_tessMapCurV += numVerts;
	s_tessMapCurI += numIndexes;
}

/*
============
R_TessStreamDraw

qtrue = drawn. Only for the tess VAO, and only while the batch in it was written at a non-zero base.
============
*/
qboolean R_TessStreamDraw(int numIndexes, int firstIndex)
{
	if (glState.currentVao != tess.vao || !qglDrawElementsBaseVertex) {
		return qfalse;
	}
	if (!s_tessDrawBaseV && !s_tessDrawFirstI) {
		return qfalse;
	}
	qglDrawElementsBaseVertex(GL_TRIANGLES, numIndexes, GL_INDEX_TYPE,
		BUFFER_OFFSET((firstIndex + s_tessDrawFirstI) * sizeof(glIndex_t)), s_tessDrawBaseV);
	return qtrue;
}

/*
============
R_InitVaos
============
*/
void R_InitVaos(void)
{
	int             vertexesSize, indexesSize;
	int             offset;

	r_hzmTessStream = ri.Cvar_Get("r_hzmTessStream", "1", 0);
	s_tessStreamVerts = SHADER_MAX_VERTEXES;
	s_tessStreamIndexes = SHADER_MAX_INDEXES;
	if (qglDrawElementsBaseVertex) {
		s_tessStreamVerts *= HZM_TESS_STREAM_MUL;
		s_tessStreamIndexes *= HZM_TESS_STREAM_MUL;
	}
	s_tessCursorV = s_tessCursorI = 0;
	s_tessDrawBaseV = s_tessDrawFirstI = 0;

	ri.Printf(PRINT_ALL, "------- R_InitVaos -------\n");

	tr.numVaos = 0;

	vertexesSize  = sizeof(tess.xyz[0]);
	vertexesSize += sizeof(tess.normal[0]);
	vertexesSize += sizeof(tess.tangent[0]);
	vertexesSize += sizeof(tess.color[0]);
	vertexesSize += sizeof(tess.texCoords[0]);
	vertexesSize += sizeof(tess.lightCoords[0]);
	vertexesSize += sizeof(tess.lightdir[0]);
	vertexesSize *= s_tessStreamVerts;

	indexesSize = sizeof(tess.indexes[0]) * s_tessStreamIndexes;

	tess.vao = R_CreateVao("tessVertexArray_VAO", NULL, vertexesSize, NULL, indexesSize, VAO_USAGE_DYNAMIC);

	offset = 0;

	tess.vao->attribs[ATTR_INDEX_POSITION      ].enabled = 1;
	tess.vao->attribs[ATTR_INDEX_NORMAL        ].enabled = 1;
	tess.vao->attribs[ATTR_INDEX_TANGENT       ].enabled = 1;
	tess.vao->attribs[ATTR_INDEX_TEXCOORD      ].enabled = 1;
	tess.vao->attribs[ATTR_INDEX_LIGHTCOORD    ].enabled = 1;
	tess.vao->attribs[ATTR_INDEX_COLOR         ].enabled = 1;
	tess.vao->attribs[ATTR_INDEX_LIGHTDIRECTION].enabled = 1;

	tess.vao->attribs[ATTR_INDEX_POSITION      ].count = 3;
	tess.vao->attribs[ATTR_INDEX_NORMAL        ].count = 4;
	tess.vao->attribs[ATTR_INDEX_TANGENT       ].count = 4;
	tess.vao->attribs[ATTR_INDEX_TEXCOORD      ].count = 2;
	tess.vao->attribs[ATTR_INDEX_LIGHTCOORD    ].count = 2;
	tess.vao->attribs[ATTR_INDEX_COLOR         ].count = 4;
	tess.vao->attribs[ATTR_INDEX_LIGHTDIRECTION].count = 4;

	tess.vao->attribs[ATTR_INDEX_POSITION      ].type = GL_FLOAT;
	tess.vao->attribs[ATTR_INDEX_NORMAL        ].type = GL_SHORT;
	tess.vao->attribs[ATTR_INDEX_TANGENT       ].type = GL_SHORT;
	tess.vao->attribs[ATTR_INDEX_TEXCOORD      ].type = GL_FLOAT;
	tess.vao->attribs[ATTR_INDEX_LIGHTCOORD    ].type = GL_FLOAT;
	tess.vao->attribs[ATTR_INDEX_COLOR         ].type = GL_UNSIGNED_SHORT;
	tess.vao->attribs[ATTR_INDEX_LIGHTDIRECTION].type = GL_SHORT;

	tess.vao->attribs[ATTR_INDEX_POSITION      ].normalized = GL_FALSE;
	tess.vao->attribs[ATTR_INDEX_NORMAL        ].normalized = GL_TRUE;
	tess.vao->attribs[ATTR_INDEX_TANGENT       ].normalized = GL_TRUE;
	tess.vao->attribs[ATTR_INDEX_TEXCOORD      ].normalized = GL_FALSE;
	tess.vao->attribs[ATTR_INDEX_LIGHTCOORD    ].normalized = GL_FALSE;
	tess.vao->attribs[ATTR_INDEX_COLOR         ].normalized = GL_TRUE;
	tess.vao->attribs[ATTR_INDEX_LIGHTDIRECTION].normalized = GL_TRUE;

	tess.vao->attribs[ATTR_INDEX_POSITION      ].offset = offset; offset += sizeof(tess.xyz[0])         * s_tessStreamVerts;
	tess.vao->attribs[ATTR_INDEX_NORMAL        ].offset = offset; offset += sizeof(tess.normal[0])      * s_tessStreamVerts;
	tess.vao->attribs[ATTR_INDEX_TANGENT       ].offset = offset; offset += sizeof(tess.tangent[0])     * s_tessStreamVerts;
	tess.vao->attribs[ATTR_INDEX_TEXCOORD      ].offset = offset; offset += sizeof(tess.texCoords[0])   * s_tessStreamVerts;
	tess.vao->attribs[ATTR_INDEX_LIGHTCOORD    ].offset = offset; offset += sizeof(tess.lightCoords[0]) * s_tessStreamVerts;
	tess.vao->attribs[ATTR_INDEX_COLOR         ].offset = offset; offset += sizeof(tess.color[0])       * s_tessStreamVerts;
	tess.vao->attribs[ATTR_INDEX_LIGHTDIRECTION].offset = offset;

	tess.vao->attribs[ATTR_INDEX_POSITION      ].stride = sizeof(tess.xyz[0]);
	tess.vao->attribs[ATTR_INDEX_NORMAL        ].stride = sizeof(tess.normal[0]);
	tess.vao->attribs[ATTR_INDEX_TANGENT       ].stride = sizeof(tess.tangent[0]);
	tess.vao->attribs[ATTR_INDEX_TEXCOORD      ].stride = sizeof(tess.texCoords[0]);
	tess.vao->attribs[ATTR_INDEX_LIGHTCOORD    ].stride = sizeof(tess.lightCoords[0]);
	tess.vao->attribs[ATTR_INDEX_COLOR         ].stride = sizeof(tess.color[0]);
	tess.vao->attribs[ATTR_INDEX_LIGHTDIRECTION].stride = sizeof(tess.lightdir[0]);

	tess.attribPointers[ATTR_INDEX_POSITION]       = tess.xyz;
	tess.attribPointers[ATTR_INDEX_NORMAL]         = tess.normal;
	tess.attribPointers[ATTR_INDEX_TANGENT]        = tess.tangent;
	tess.attribPointers[ATTR_INDEX_TEXCOORD]       = tess.texCoords;
	tess.attribPointers[ATTR_INDEX_LIGHTCOORD]     = tess.lightCoords;
	tess.attribPointers[ATTR_INDEX_COLOR]          = tess.color;
	tess.attribPointers[ATTR_INDEX_LIGHTDIRECTION] = tess.lightdir;

	Vao_SetVertexPointers(tess.vao);

	// HZM gl2 [render-perf 3] the mapped twin of the tess VAO (see THE MAPPED TESS STREAM)
	r_hzmTessMap = ri.Cvar_Get("r_hzmTessMap", "1", 0);
	s_tessClassicVao = tess.vao;
	s_tessVaoEnabled[0] = glState.vertexAttribsEnabled;
	s_tessVaoEnabled[1] = 0;
	R_TessMapCreate();

	R_BindNullVao();

	VaoCache_Init();

	GL_CheckErrors();
}

/*
============
R_ShutdownVaos
============
*/
void R_ShutdownVaos(void)
{
	int             i;
	vao_t          *vao;

	ri.Printf(PRINT_ALL, "------- R_ShutdownVaos -------\n");

	R_BindNullVao();

	// HZM gl2 [render-perf 3] the mapped stream's fences; its buffers go with the VAO list below (deleting unmaps)
	if (qglDeleteSyncHzm) {
		for (i = 0; i < HZM_TESS_MAP_FENCES; i++) {
			if (s_tessMapFence[i]) {
				qglDeleteSyncHzm(s_tessMapFence[i]);
				s_tessMapFence[i] = NULL;
			}
		}
	}
	if (s_tessClassicVao) {
		tess.vao = s_tessClassicVao;
	}
	s_tessMapVao = NULL;
	s_tessMapV = s_tessMapI = NULL;

	for(i = 0; i < tr.numVaos; i++)
	{
		vao = tr.vaos[i];

		if(vao->vao)
			qglDeleteVertexArrays(1, &vao->vao);

		if(vao->vertexesVBO)
		{
			qglDeleteBuffers(1, &vao->vertexesVBO);
		}

		if(vao->indexesIBO)
		{
			qglDeleteBuffers(1, &vao->indexesIBO);
		}
	}

	tr.numVaos = 0;
}

/*
============
R_VaoList_f
============
*/
void R_VaoList_f(void)
{
	int             i;
	vao_t          *vao;
	int             vertexesSize = 0;
	int             indexesSize = 0;

	ri.Printf(PRINT_ALL, " size          name\n");
	ri.Printf(PRINT_ALL, "----------------------------------------------------------\n");

	for(i = 0; i < tr.numVaos; i++)
	{
		vao = tr.vaos[i];

		ri.Printf(PRINT_ALL, "%d.%02d MB %s\n", vao->vertexesSize / (1024 * 1024),
				  (vao->vertexesSize % (1024 * 1024)) * 100 / (1024 * 1024), vao->name);

		vertexesSize += vao->vertexesSize;
	}

	for(i = 0; i < tr.numVaos; i++)
	{
		vao = tr.vaos[i];

		ri.Printf(PRINT_ALL, "%d.%02d MB %s\n", vao->indexesSize / (1024 * 1024),
				  (vao->indexesSize % (1024 * 1024)) * 100 / (1024 * 1024), vao->name);

		indexesSize += vao->indexesSize;
	}

	ri.Printf(PRINT_ALL, " %i total VAOs\n", tr.numVaos);
	ri.Printf(PRINT_ALL, " %d.%02d MB total vertices memory\n", vertexesSize / (1024 * 1024),
			  (vertexesSize % (1024 * 1024)) * 100 / (1024 * 1024));
	ri.Printf(PRINT_ALL, " %d.%02d MB total triangle indices memory\n", indexesSize / (1024 * 1024),
			  (indexesSize % (1024 * 1024)) * 100 / (1024 * 1024));
}


/*
==============
RB_UpdateTessVao

Adapted from Tess_UpdateVBOs from xreal

Update the default VAO to replace the client side vertex arrays
==============
*/
void RB_UpdateTessVao(unsigned int attribBits)
{
	GLimp_LogComment("--- RB_UpdateTessVao ---\n");

	backEnd.pc.c_dynamicVaoDraws++;

	// update the default VAO
	if(tess.numVertexes > 0 && tess.numVertexes <= SHADER_MAX_VERTEXES && tess.numIndexes > 0 && tess.numIndexes <= SHADER_MAX_INDEXES)
	{
		int attribIndex;
		int attribUpload;
		int baseV = 0, baseI = 0;
		qboolean stream = (qboolean)(s_tessStreamVerts > SHADER_MAX_VERTEXES && r_hzmTessStream->integer);
		qboolean mapped = R_TessMapSelect();	// HZM gl2 [render-perf 3] may repoint tess.vao

		R_BindVao(tess.vao);
		hzmPerf.tessBatches++;
		hzmPerf.tessVerts += tess.numVertexes;

		if (mapped) {
			// HZM gl2 [render-perf 3] written straight into the mapped store: no driver call until the draw
			R_TessMapPlace(tess.numVertexes, tess.numIndexes, &baseV, &baseI);
			s_tessDrawBaseV = baseV;
			s_tessDrawFirstI = baseI;

			if(!(attribBits & ATTR_BITS))
				attribBits = ATTR_BITS;

			for (attribIndex = 0; attribIndex < ATTR_INDEX_COUNT; attribIndex++)
			{
				uint32_t attribBit = 1 << attribIndex;
				vaoAttrib_t *vAtb = &tess.vao->attribs[attribIndex];

				if (attribBits & attribBit)
				{
					if (vAtb->stride && tess.attribPointers[attribIndex])
					{
						memcpy(s_tessMapV + vAtb->offset + (size_t)baseV * vAtb->stride, tess.attribPointers[attribIndex],
							(size_t)tess.numVertexes * vAtb->stride);
						hzmPerf.tessBytes += (double)(tess.numVertexes * vAtb->stride);
					}

					if (!(glState.vertexAttribsEnabled & attribBit))
					{
						qglEnableVertexAttribArray(attribIndex);
						glState.vertexAttribsEnabled |= attribBit;
					}
				}
				else if (glState.vertexAttribsEnabled & attribBit)
				{
					qglDisableVertexAttribArray(attribIndex);
					glState.vertexAttribsEnabled &= ~attribBit;
				}
			}
			memcpy(s_tessMapI + (size_t)baseI * sizeof(tess.indexes[0]), tess.indexes, (size_t)tess.numIndexes * sizeof(tess.indexes[0]));
			return;
		}

		if (stream && s_tessCursorV + tess.numVertexes <= s_tessStreamVerts
			&& s_tessCursorI + tess.numIndexes <= s_tessStreamIndexes) {
			// HZM gl2 [bug-3483] room behind the cursor: append, no orphan
			baseV = s_tessCursorV;
			baseI = s_tessCursorI;
		} else {
			// orphan old vertex buffer so we don't stall on it
			qglBufferData(GL_ARRAY_BUFFER, tess.vao->vertexesSize, NULL, GL_DYNAMIC_DRAW);
			// orphan old index buffer so we don't stall on it
			qglBufferData(GL_ELEMENT_ARRAY_BUFFER, tess.vao->indexesSize, NULL, GL_DYNAMIC_DRAW);
		}
		if (stream) {
			s_tessCursorV = baseV + tess.numVertexes;
			s_tessCursorI = baseI + tess.numIndexes;
		} else {
			// off: every batch starts at 0, and the ring restarts with an orphan when it is switched back on
			s_tessCursorV = s_tessStreamVerts;
			s_tessCursorI = s_tessStreamIndexes;
		}
		s_tessDrawBaseV = baseV;
		s_tessDrawFirstI = baseI;

		// if nothing to set, set everything
		if(!(attribBits & ATTR_BITS))
			attribBits = ATTR_BITS;

		attribUpload = attribBits;

		for (attribIndex = 0; attribIndex < ATTR_INDEX_COUNT; attribIndex++)
		{
			uint32_t attribBit = 1 << attribIndex;
			vaoAttrib_t *vAtb = &tess.vao->attribs[attribIndex];

			if (attribUpload & attribBit)
			{
				// note: tess has a VBO where stride == size
				qglBufferSubData(GL_ARRAY_BUFFER, vAtb->offset + baseV * vAtb->stride, tess.numVertexes * vAtb->stride, tess.attribPointers[attribIndex]);
				hzmPerf.tessUploads++;
				hzmPerf.tessBytes += (double)(tess.numVertexes * vAtb->stride);
			}

			if (attribBits & attribBit)
			{
				if (!glRefConfig.vertexArrayObject)
					qglVertexAttribPointer(attribIndex, vAtb->count, vAtb->type, vAtb->normalized, vAtb->stride, BUFFER_OFFSET(vAtb->offset));

				if (!(glState.vertexAttribsEnabled & attribBit))
				{
					qglEnableVertexAttribArray(attribIndex);
					glState.vertexAttribsEnabled |= attribBit;
				}
			}
			else
			{
				if ((glState.vertexAttribsEnabled & attribBit))
				{
					qglDisableVertexAttribArray(attribIndex);
					glState.vertexAttribsEnabled &= ~attribBit;
				}
			}
		}

		qglBufferSubData(GL_ELEMENT_ARRAY_BUFFER, baseI * sizeof(tess.indexes[0]), tess.numIndexes * sizeof(tess.indexes[0]), tess.indexes);
	}
}

// FIXME: This sets a limit of 65536 verts/262144 indexes per static surface
// This is higher than the old vq3 limits but is worth noting
#define VAOCACHE_QUEUE_MAX_SURFACES (1 << 10)
#define VAOCACHE_QUEUE_MAX_VERTEXES (1 << 16)
#define VAOCACHE_QUEUE_MAX_INDEXES (VAOCACHE_QUEUE_MAX_VERTEXES * 4)

typedef struct queuedSurface_s
{
	srfVert_t *vertexes;
	int numVerts;
	glIndex_t *indexes;
	int numIndexes;
}
queuedSurface_t;

static struct
{
	queuedSurface_t surfaces[VAOCACHE_QUEUE_MAX_SURFACES];
	int numSurfaces;

	srfVert_t vertexes[VAOCACHE_QUEUE_MAX_VERTEXES];
	int vertexCommitSize;

	vaoCacheGlIndex_t indexes[VAOCACHE_QUEUE_MAX_INDEXES];
	int indexCommitSize;
}
vcq;

#define VAOCACHE_MAX_SURFACES (1 << 16)
#define VAOCACHE_MAX_BATCHES (1 << 10)

// srfVert_t is 60 bytes
// assuming each vert is referenced 4 times, need 16 bytes (4 glIndex_t) per vert
// -> need about 4/15ths the space for indexes as vertexes
#define VAOCACHE_VERTEX_BUFFER_SIZE (16 * 1024 * 1024)
#define VAOCACHE_INDEX_BUFFER_SIZE (5 * 1024 * 1024)

typedef struct buffered_s
{
	glIndex_t *indexes;
	int numIndexes;
	int bufferOffset;
}
buffered_t;

static struct
{
	vao_t *vao;
	buffered_t surfaceIndexSets[VAOCACHE_MAX_SURFACES];
	int numSurfaces;

	int batchLengths[VAOCACHE_MAX_BATCHES];
	int numBatches;

	int vertexOffset;
	int indexOffset;
}
vc;

void VaoCache_Commit(void)
{
	buffered_t *indexSet;
	int *batchLength;
	queuedSurface_t *surf, *end = vcq.surfaces + vcq.numSurfaces;

	R_BindVao(vc.vao);

	// Search for a matching batch
	// FIXME: Use faster search
	indexSet = vc.surfaceIndexSets;
	batchLength = vc.batchLengths;
	for (; batchLength < vc.batchLengths + vc.numBatches; batchLength++)
	{
		if (*batchLength == vcq.numSurfaces)
		{
			buffered_t *indexSet2 = indexSet;
			for (surf = vcq.surfaces; surf < end; surf++, indexSet2++)
			{
				if (surf->indexes != indexSet2->indexes || surf->numIndexes != indexSet2->numIndexes)
					break;
			}

			if (surf == end)
				break;
		}

		indexSet += *batchLength;
	}

	// If found, use it
	if (indexSet < vc.surfaceIndexSets + vc.numSurfaces)
	{
		tess.firstIndex = indexSet->bufferOffset / glRefConfig.vaoCacheGlIndexSize;
		//ri.Printf(PRINT_ALL, "firstIndex %d numIndexes %d as %d\n", tess.firstIndex, tess.numIndexes, (int)(batchLength - vc.batchLengths));
		//ri.Printf(PRINT_ALL, "vc.numSurfaces %d vc.numBatches %d\n", vc.numSurfaces, vc.numBatches);
	}
	// If not, rebuffer the batch
	// FIXME: keep track of the vertexes so we don't have to reupload them every time
	else
	{
		srfVert_t *dstVertex = vcq.vertexes;
		vaoCacheGlIndex_t *dstIndex = vcq.indexes;
		unsigned short *dstIndexUshort = (unsigned short *)vcq.indexes;

		batchLength = vc.batchLengths + vc.numBatches;
		*batchLength = vcq.numSurfaces;
		vc.numBatches++;

		tess.firstIndex = vc.indexOffset / glRefConfig.vaoCacheGlIndexSize;
		vcq.vertexCommitSize = 0;
		vcq.indexCommitSize = 0;
		for (surf = vcq.surfaces; surf < end; surf++)
		{
			glIndex_t *srcIndex = surf->indexes;
			int vertexesSize = surf->numVerts * sizeof(srfVert_t);
			int indexesSize = surf->numIndexes * glRefConfig.vaoCacheGlIndexSize;
			int i, indexOffset = (vc.vertexOffset + vcq.vertexCommitSize) / sizeof(srfVert_t);

			Com_Memcpy(dstVertex, surf->vertexes, vertexesSize);
			dstVertex += surf->numVerts;

			vcq.vertexCommitSize += vertexesSize;

			indexSet = vc.surfaceIndexSets + vc.numSurfaces;
			indexSet->indexes = surf->indexes;
			indexSet->numIndexes = surf->numIndexes;
			indexSet->bufferOffset = vc.indexOffset + vcq.indexCommitSize;
			vc.numSurfaces++;

			if (glRefConfig.vaoCacheGlIndexType == GL_UNSIGNED_SHORT)
			{
				for (i = 0; i < surf->numIndexes; i++)
					*dstIndexUshort++ = *srcIndex++ + indexOffset;
			}
			else
			{
				for (i = 0; i < surf->numIndexes; i++)
					*dstIndex++ = *srcIndex++ + indexOffset;
			}

			vcq.indexCommitSize += indexesSize;
		}

		//ri.Printf(PRINT_ALL, "committing %d to %d, %d to %d as %d\n", vcq.vertexCommitSize, vc.vertexOffset, vcq.indexCommitSize, vc.indexOffset, (int)(batchLength - vc.batchLengths));

		if (vcq.vertexCommitSize)
		{
			qglBindBuffer(GL_ARRAY_BUFFER, vc.vao->vertexesVBO);
			qglBufferSubData(GL_ARRAY_BUFFER, vc.vertexOffset, vcq.vertexCommitSize, vcq.vertexes);
			vc.vertexOffset += vcq.vertexCommitSize;
		}

		if (vcq.indexCommitSize)
		{
			qglBindBuffer(GL_ELEMENT_ARRAY_BUFFER, vc.vao->indexesIBO);
			qglBufferSubData(GL_ELEMENT_ARRAY_BUFFER, vc.indexOffset, vcq.indexCommitSize, vcq.indexes);
			vc.indexOffset += vcq.indexCommitSize;
		}
	}
}

void VaoCache_DrawElements(int numIndexes, int firstIndex)
{
	assert( glState.currentVao == vc.vao );

	qglDrawElements(GL_TRIANGLES, numIndexes, glRefConfig.vaoCacheGlIndexType, BUFFER_OFFSET(firstIndex * glRefConfig.vaoCacheGlIndexSize));
}

void VaoCache_Init(void)
{
	int vertexBufferSize;
	int indexBufferSize;

	if (glRefConfig.vaoCacheGlIndexType == GL_UNSIGNED_SHORT)
	{
		vertexBufferSize = sizeof(srfVert_t) * USHRT_MAX;
		indexBufferSize = sizeof(unsigned short) * USHRT_MAX * 4;
	}
	else
	{
		vertexBufferSize = VAOCACHE_VERTEX_BUFFER_SIZE;
		indexBufferSize = VAOCACHE_INDEX_BUFFER_SIZE;
	}

	vc.vao = R_CreateVao("VaoCache", NULL, vertexBufferSize, NULL, indexBufferSize, VAO_USAGE_DYNAMIC);

	vc.vao->attribs[ATTR_INDEX_POSITION].enabled       = 1;
	vc.vao->attribs[ATTR_INDEX_TEXCOORD].enabled       = 1;
	vc.vao->attribs[ATTR_INDEX_LIGHTCOORD].enabled     = 1;
	vc.vao->attribs[ATTR_INDEX_NORMAL].enabled         = 1;
	vc.vao->attribs[ATTR_INDEX_TANGENT].enabled        = 1;
	vc.vao->attribs[ATTR_INDEX_LIGHTDIRECTION].enabled = 1;
	vc.vao->attribs[ATTR_INDEX_COLOR].enabled          = 1;

	vc.vao->attribs[ATTR_INDEX_POSITION].count       = 3;
	vc.vao->attribs[ATTR_INDEX_TEXCOORD].count       = 2;
	vc.vao->attribs[ATTR_INDEX_LIGHTCOORD].count     = 2;
	vc.vao->attribs[ATTR_INDEX_NORMAL].count         = 4;
	vc.vao->attribs[ATTR_INDEX_TANGENT].count        = 4;
	vc.vao->attribs[ATTR_INDEX_LIGHTDIRECTION].count = 4;
	vc.vao->attribs[ATTR_INDEX_COLOR].count          = 4;

	vc.vao->attribs[ATTR_INDEX_POSITION].type             = GL_FLOAT;
	vc.vao->attribs[ATTR_INDEX_TEXCOORD].type             = GL_FLOAT;
	vc.vao->attribs[ATTR_INDEX_LIGHTCOORD].type           = GL_FLOAT;
	vc.vao->attribs[ATTR_INDEX_NORMAL].type               = GL_SHORT;
	vc.vao->attribs[ATTR_INDEX_TANGENT].type              = GL_SHORT;
	vc.vao->attribs[ATTR_INDEX_LIGHTDIRECTION].type       = GL_SHORT;
	vc.vao->attribs[ATTR_INDEX_COLOR].type                = GL_UNSIGNED_SHORT;

	vc.vao->attribs[ATTR_INDEX_POSITION].normalized       = GL_FALSE;
	vc.vao->attribs[ATTR_INDEX_TEXCOORD].normalized       = GL_FALSE;
	vc.vao->attribs[ATTR_INDEX_LIGHTCOORD].normalized     = GL_FALSE;
	vc.vao->attribs[ATTR_INDEX_NORMAL].normalized         = GL_TRUE;
	vc.vao->attribs[ATTR_INDEX_TANGENT].normalized        = GL_TRUE;
	vc.vao->attribs[ATTR_INDEX_LIGHTDIRECTION].normalized = GL_TRUE;
	vc.vao->attribs[ATTR_INDEX_COLOR].normalized          = GL_TRUE;

	vc.vao->attribs[ATTR_INDEX_POSITION].offset       = offsetof(srfVert_t, xyz);
	vc.vao->attribs[ATTR_INDEX_TEXCOORD].offset       = offsetof(srfVert_t, st);
	vc.vao->attribs[ATTR_INDEX_LIGHTCOORD].offset     = offsetof(srfVert_t, lightmap);
	vc.vao->attribs[ATTR_INDEX_NORMAL].offset         = offsetof(srfVert_t, normal);
	vc.vao->attribs[ATTR_INDEX_TANGENT].offset        = offsetof(srfVert_t, tangent);
	vc.vao->attribs[ATTR_INDEX_LIGHTDIRECTION].offset = offsetof(srfVert_t, lightdir);
	vc.vao->attribs[ATTR_INDEX_COLOR].offset          = offsetof(srfVert_t, color);

	vc.vao->attribs[ATTR_INDEX_POSITION].stride       = sizeof(srfVert_t);
	vc.vao->attribs[ATTR_INDEX_TEXCOORD].stride       = sizeof(srfVert_t);
	vc.vao->attribs[ATTR_INDEX_LIGHTCOORD].stride     = sizeof(srfVert_t);
	vc.vao->attribs[ATTR_INDEX_NORMAL].stride         = sizeof(srfVert_t);
	vc.vao->attribs[ATTR_INDEX_TANGENT].stride        = sizeof(srfVert_t);
	vc.vao->attribs[ATTR_INDEX_LIGHTDIRECTION].stride = sizeof(srfVert_t);
	vc.vao->attribs[ATTR_INDEX_COLOR].stride          = sizeof(srfVert_t);

	Vao_SetVertexPointers(vc.vao);

	vc.numSurfaces = 0;
	vc.numBatches = 0;
	vc.vertexOffset = 0;
	vc.indexOffset = 0;
	vcq.vertexCommitSize = 0;
	vcq.indexCommitSize = 0;
	vcq.numSurfaces = 0;
}

void VaoCache_BindVao(void)
{
	R_BindVao(vc.vao);
}

void VaoCache_CheckAdd(qboolean *endSurface, qboolean *recycleVertexBuffer, qboolean *recycleIndexBuffer, int numVerts, int numIndexes)
{
	int vertexesSize = sizeof(srfVert_t) * numVerts;
	int indexesSize = glRefConfig.vaoCacheGlIndexSize * numIndexes;

	if (vc.vao->vertexesSize < vc.vertexOffset + vcq.vertexCommitSize + vertexesSize)
	{
		//ri.Printf(PRINT_ALL, "out of space in vertex cache: %d < %d + %d + %d\n", vc.vao->vertexesSize, vc.vertexOffset, vcq.vertexCommitSize, vertexesSize);
		*recycleVertexBuffer = qtrue;
		*recycleIndexBuffer = qtrue;
		*endSurface = qtrue;
	}

	if (vc.vao->indexesSize < vc.indexOffset + vcq.indexCommitSize + indexesSize)
	{
		//ri.Printf(PRINT_ALL, "out of space in index cache\n");
		*recycleIndexBuffer = qtrue;
		*endSurface = qtrue;
	}

	if (vc.numSurfaces + vcq.numSurfaces >= VAOCACHE_MAX_SURFACES)
	{
		//ri.Printf(PRINT_ALL, "out of surfaces in index cache\n");
		*recycleIndexBuffer = qtrue;
		*endSurface = qtrue;
	}

	if (vc.numBatches >= VAOCACHE_MAX_BATCHES)
	{
		//ri.Printf(PRINT_ALL, "out of batches in index cache\n");
		*recycleIndexBuffer = qtrue;
		*endSurface = qtrue;
	}

	if (vcq.numSurfaces >= VAOCACHE_QUEUE_MAX_SURFACES)
	{
		//ri.Printf(PRINT_ALL, "out of queued surfaces\n");
		*endSurface = qtrue;
	}

	if (VAOCACHE_QUEUE_MAX_VERTEXES * sizeof(srfVert_t) < vcq.vertexCommitSize + vertexesSize)
	{
		//ri.Printf(PRINT_ALL, "out of queued vertexes\n");
		*endSurface = qtrue;
	}

	if (VAOCACHE_QUEUE_MAX_INDEXES * glRefConfig.vaoCacheGlIndexSize < vcq.indexCommitSize + indexesSize)
	{
		//ri.Printf(PRINT_ALL, "out of queued indexes\n");
		*endSurface = qtrue;
	}
}

void VaoCache_RecycleVertexBuffer(void)
{
	qglBindBuffer(GL_ARRAY_BUFFER, vc.vao->vertexesVBO);
	qglBufferData(GL_ARRAY_BUFFER, vc.vao->vertexesSize, NULL, GL_DYNAMIC_DRAW);
	vc.vertexOffset = 0;
}

void VaoCache_RecycleIndexBuffer(void)
{
	qglBindBuffer(GL_ELEMENT_ARRAY_BUFFER, vc.vao->indexesIBO);
	qglBufferData(GL_ELEMENT_ARRAY_BUFFER, vc.vao->indexesSize, NULL, GL_DYNAMIC_DRAW);
	vc.indexOffset = 0;
	vc.numSurfaces = 0;
	vc.numBatches = 0;
}

void VaoCache_InitQueue(void)
{
	vcq.vertexCommitSize = 0;
	vcq.indexCommitSize = 0;
	vcq.numSurfaces = 0;
}

void VaoCache_AddSurface(srfVert_t *verts, int numVerts, glIndex_t *indexes, int numIndexes)
{
	queuedSurface_t *queueEntry = vcq.surfaces + vcq.numSurfaces;
	queueEntry->vertexes = verts;
	queueEntry->numVerts = numVerts;
	queueEntry->indexes = indexes;
	queueEntry->numIndexes = numIndexes;
	vcq.numSurfaces++;

	vcq.vertexCommitSize += sizeof(srfVert_t) * numVerts;
	vcq.indexCommitSize += glRefConfig.vaoCacheGlIndexSize * numIndexes;
}
