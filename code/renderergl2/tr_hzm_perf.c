/*
===========================================================================
HZM coop [bug-3486] - renderer-side numbers for the in-game bug report.

The engine's own com_speeds split cannot say how long the back end took: every R_IssuePendingRenderCommands (the UI's
set2D, image uploads, ...) runs RB_ExecuteRenderCommands in the middle of the client frame, so that time lands in the
"client" bucket. Here it is measured where it happens, in microseconds, as two running totals the exe reads:

  backend   all of RB_ExecuteRenderCommands, every flush of the frame
  present   the part of it spent in qglFinish + the buffer swap (waiting for the GPU / vsync)

Counts are taken only on a frame the exe asked for (about once a second): the draw-surface lists of every view are
walked once for skeletal surfaces and the models they belong to, and backEnd.pc gives the dynamic (tess) batches.

The exe finds R_HzmPerf with Sys_LoadFunction, so a renderer without it (gl1, an older gl2) simply reports nothing.
No threads, no allocation, nothing drawn differently.
===========================================================================
*/

#include "tr_local.h"
#include "tr_hzm_perf.h"

#ifdef _WIN32
#	ifndef WIN32_LEAN_AND_MEAN
#		define WIN32_LEAN_AND_MEAN
#	endif
#	include <windows.h>
#endif

enum { HP_VIEWS, HP_DRAWSURFS, HP_SKELSURFS, HP_SKELMODELS, HP_BATCHES, HP_NUM };

static unsigned long long s_backendUs, s_presentUs, s_backendT0, s_presentT0;
static unsigned int       s_flushes, s_sampleSeq;
static qboolean           s_request, s_sampling;
static int                s_cur[HP_NUM], s_sample[HP_NUM];

static unsigned long long R_HzmPerf_Micros( void ) {
#ifdef _WIN32
	static LARGE_INTEGER freq;
	LARGE_INTEGER        now;
	if ( !freq.QuadPart ) {
		QueryPerformanceFrequency( &freq );
	}
	QueryPerformanceCounter( &now );
	return (unsigned long long)( now.QuadPart / freq.QuadPart ) * 1000000ULL
		 + (unsigned long long)( now.QuadPart % freq.QuadPart ) * 1000000ULL / (unsigned long long)freq.QuadPart;
#else
	return (unsigned long long)ri.Milliseconds() * 1000ULL;
#endif
}

void R_HzmPerf_BackendBegin( void ) {
	s_backendT0 = R_HzmPerf_Micros();
}

void R_HzmPerf_BackendEnd( void ) {
	s_backendUs += R_HzmPerf_Micros() - s_backendT0;
	s_flushes++;
}

void R_HzmPerf_PresentBegin( void ) {
	s_presentT0 = R_HzmPerf_Micros();
}

void R_HzmPerf_PresentEnd( void ) {
	s_presentUs += R_HzmPerf_Micros() - s_presentT0;
}

/*
====================
R_HzmPerf_DrawSurfs - one view's draw-surface list was handed to the back end. Only looked at on a sampled frame.
====================
*/
void R_HzmPerf_DrawSurfs( const struct drawSurf_s *drawSurfs, int numDrawSurfs ) {
	byte seen[( REFENTITYNUM_MASK + 8 ) / 8];
	int  i, skelSurfs = 0, skelModels = 0;

	if ( !s_sampling || !drawSurfs ) {
		return;
	}
	Com_Memset( seen, 0, sizeof( seen ) );
	for ( i = 0; i < numDrawSurfs; i++ ) {
		const drawSurf_t *ds = &drawSurfs[i];
		if ( ds->surface && *ds->surface == SF_TIKI_SKEL ) {
			int ent = ( ds->sort >> QSORT_REFENTITYNUM_SHIFT ) & REFENTITYNUM_MASK;
			skelSurfs++;
			if ( !( seen[ent >> 3] & ( 1 << ( ent & 7 ) ) ) ) {
				seen[ent >> 3] |= (byte)( 1 << ( ent & 7 ) );
				skelModels++;
			}
		}
	}
	s_cur[HP_VIEWS]++;
	s_cur[HP_DRAWSURFS] += numDrawSurfs;
	s_cur[HP_SKELSURFS] += skelSurfs;
	if ( skelModels > s_cur[HP_SKELMODELS] ) {
		s_cur[HP_SKELMODELS] = skelModels; // the busiest view: the main one, not a shadow pass
	}
}

/*
====================
R_HzmPerf_EndFrame - RE_EndFrame, just before R_PerformanceCounters zeroes backEnd.pc (which still holds the batches
the back end flushed since the previous frame's end)
====================
*/
void R_HzmPerf_EndFrame( void ) {
	if ( s_sampling ) {
		s_cur[HP_BATCHES] = backEnd.pc.c_surfBatches;
		Com_Memcpy( s_sample, s_cur, sizeof( s_sample ) );
		s_sampleSeq++;
	}
	s_sampling = s_request;
	s_request  = qfalse;
	Com_Memset( s_cur, 0, sizeof( s_cur ) );
}

/*
====================
R_HzmPerf - the exported reader. out[]: 0 backend microseconds (running total), 1 present microseconds (running total),
2 flushes (running total), 3 sample sequence number, 4 views, 5 draw surfaces, 6 skeletal surfaces, 7 skeletal models,
8 dynamic batches. requestSample asks for the counts to be taken on the next frame. Returns how many were written.
====================
*/
Q_EXPORT int QDECL R_HzmPerf( long long *out, int n, int requestSample ) {
	long long v[4 + HP_NUM];
	int       i;

	v[0] = (long long)s_backendUs;
	v[1] = (long long)s_presentUs;
	v[2] = (long long)s_flushes;
	v[3] = (long long)s_sampleSeq;
	for ( i = 0; i < HP_NUM; i++ ) {
		v[4 + i] = s_sample[i];
	}
	if ( requestSample ) {
		s_request = qtrue;
	}
	if ( n > 4 + HP_NUM ) {
		n = 4 + HP_NUM;
	}
	for ( i = 0; i < n; i++ ) {
		out[i] = v[i];
	}
	return n;
}
