/*
===========================================================================
HZM coop [bug-3486] - renderer-side numbers for the bug report (see qcommon/hzm_diag.h). tr_hzm_perf.c.
===========================================================================
*/

#ifndef TR_HZM_PERF_H
#define TR_HZM_PERF_H

void R_HzmPerf_BackendBegin( void ); // tr_cmds.c R_IssueRenderCommands, around RB_ExecuteRenderCommands
void R_HzmPerf_BackendEnd( void );
void R_HzmPerf_PresentBegin( void ); // tr_backend.c RB_SwapBuffers, around qglFinish + GLimp_EndFrame
void R_HzmPerf_PresentEnd( void );
void R_HzmPerf_EndFrame( void );     // tr_cmds.c R_IssueRenderCommands, before R_PerformanceCounters clears backEnd.pc
void R_HzmPerf_DrawSurfs( const struct drawSurf_s *drawSurfs, int numDrawSurfs ); // tr_cmds.c R_AddDrawSurfCmd

#endif
