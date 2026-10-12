/*
tr_hzm_framegen.h - HZM frame generation prototype (r_hzmFrameGen). See tr_hzm_framegen.c.
*/
#ifndef TR_HZM_FRAMEGEN_H
#define TR_HZM_FRAMEGEN_H

#ifdef __cplusplus
extern "C" {
#endif

void     R_FG_Init( void );                 // R_Init: cvars + commands only, no GL
void     R_FG_Shutdown( void );             // RE_Shutdown: joins the present thread, drops its context
qboolean R_FG_Wanted( void );               // cheap: is the cvar set, or is the feature still running
void     R_FG_LatchView( const void *viewParms );   // RB_SetupGlobalFog: the main world view's matrices
void     R_FG_SceneDone( qboolean depthValid );     // RB_PostProcess: the 3D picture is complete, the HUD is not yet drawn
void     R_FG_Set2DWindow( float w, float h, int vpW, int vpH );   // Set2DWindow: extents of the 2D coordinate system and its viewport
void     R_FG_Note2D( float w, float h, const char *what );          // every 2D quad: a big one after the scene = menu / console / overlay
int      R_FG_Present( void );              // RB_SwapBuffers: 0 = not handled, 1 = handed to the present thread, 2 = back buffer filled, swap it
void     R_FG_NotePresentOff( void );       // RB_SwapBuffers with the feature off: present-time statistics only
qboolean R_FG_ReadSourceBegin( void );      // RB_ReadPixels: read the last real frame instead of a back buffer another thread owns
void     R_FG_ReadSourceEnd( void );

// sdl_glimp.c
void     GLimp_FG_EndFrameNoSwap( void );
void    *GLimp_FG_CreateContext( void );
void     GLimp_FG_DeleteContext( void *ctx );
qboolean GLimp_FG_MakeCurrent( void *ctx );
void     GLimp_FG_ReleaseCurrent( void );
void     GLimp_FG_Swap( void );
void     GLimp_FG_SetSwapInterval( int interval );
void    *GLimp_FG_GetProc( const char *name );

#ifdef __cplusplus
}
#endif

#endif
