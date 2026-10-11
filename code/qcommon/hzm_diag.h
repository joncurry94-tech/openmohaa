/*
===========================================================================
HZM coop [bug-3486] - always-on diagnostics that the in-game bug report prints.

Everything here is bookkeeping for a report: nothing changes what the game does, what is sent on the wire or what a
player sees. The collectors are a handful of timer reads and counter bumps per frame; anything that walks a list is
sampled once a second from the client (client/cl_bugreport.cpp), never per frame.

  frame phases    where one Com_Frame's time went (idle/fps-cap wait, events, server, game/AI, client+cgame, renderer
                  front end, renderer back end), two 60 s windows
  hitch log       every frame with >= 100 ms of work: when, how long, the phase split, and what the engine did in that
                  frame (files opened by kind + the first few names, a map load, a renderer restart, ...). Ring of 64
  fps history     per map of the session, one record per minute in the map. Capped at 120 records
  timeline        connects, drops (with the reason), timeouts, overflows, kicks, map changes, "entered the game",
                  SVCMDQ/SVCMDPARK. Ring of 100
  freeze watchdog a thread that notices the main loop has not finished a frame for 5 s (120 s inside a map load or a
                  renderer restart) and writes <homepath>/<gamedir>/coop_freeze/freeze_<stamp>.txt: wall clock, how long
                  so far, the main thread's call stack as raw addresses and the module bases to symbolicate it offline.
                  It never kills or "recovers" anything. Windows x64 only; a stub elsewhere. com_freezeWatch 0 = off
===========================================================================
*/

#ifndef HZM_DIAG_H
#define HZM_DIAG_H

#ifdef __cplusplus
extern "C" {
#endif

// phases of one Com_Frame; also what the watchdog reports as "the main thread was in"
enum {
	HZP_IDLE,     // the fps-cap / NET_Sleep wait at the top of the frame
	HZP_EVENTS,   // event loops, command buffer, everything not named below
	HZP_SERVER,   // SV_Frame without the game
	HZP_GAME,     // ge->RunFrame (game logic, AI, scripts)
	HZP_CLIENT,   // CL_Frame without the renderer: cgame, UI, sound, input
	HZP_FRONTEND, // re.RenderScene: culling, sorting, building the draw lists
	HZP_BACKEND,  // RB_ExecuteRenderCommands: GL calls, and the buffer swap
	HZP_STALL,    // the fps-cap wait ran far past what was asked for: the process was not running (counted as work)
	HZP_NUM
};

// things that happened in the running frame / legitimately long operations
#define HZF_MAPLOAD_SV   0x0001 // SV_SpawnServer
#define HZF_MAPLOAD_CL   0x0002 // CL_InitCGame (client map load)
#define HZF_VID_RESTART  0x0004
#define HZF_SND_RESTART  0x0008
#define HZF_ERROR_DROP   0x0010 // a Com_Error( ERR_DROP / ERR_DISCONNECT ) ended the frame
#define HZF_CONNECT      0x0020 // this client started connecting
#define HZF_STARTUP      0x0040 // engine start

// the once-a-second samples (client/cl_bugreport.cpp fills them; -1 = not known on this machine / with these DLLs)
enum {
	HZC_SNAP_ENTS,      // entities in the snapshot this client is drawing from
	HZC_SKEL_MODELS,    // skeletal models in the busiest view of the sampled frame
	HZC_SKEL_SURFS,     // skeletal surfaces submitted, all views (main + shadow passes)
	HZC_DRAWSURFS,      // draw surfaces submitted, all views
	HZC_VIEWS,          // views rendered in the sampled frame
	HZC_TESS_BATCHES,   // dynamic (tess) batches flushed by the back end
	HZC_TEMPMODELS,     // cgame temp models alive
	HZC_EMITTERS,       // cgame emitters alive
	HZC_SND_CHANNELS,   // sound channels playing
	HZC_SV_ENTS,        // server entities in use (host only)
	HZC_SV_AI_ENEMY,    // living enemy actors (host only)
	HZC_SV_AI_FRIENDLY, // living friendly actors (host only)
	HZC_SV_PLAYERS,     // living, non-spectating players (host only)
	HZC_NUM
};

// report sections for HzmDiag_Text
enum {
	HZT_FRAME,    // where the frame time goes + the count samples
	HZT_HITCHES,  // hitch log
	HZT_FPS,      // frame-rate history
	HZT_TIMELINE, // connection timeline
	HZT_NUM
};

void HzmDiag_Init( void );     // end of Com_Init: clocks, the freeze directory, the watchdog
void HzmDiag_Shutdown( void );
unsigned long long HzmDiag_Micros( void );

void HzmDiag_Frame( void );                                    // top of Com_Frame: closes the previous frame
void HzmDiag_Mark( int phase );                                // the main timeline switches to `phase`
void HzmDiag_IdleBudget( int msec );                           // how long this frame's fps-cap wait is meant to take
void HzmDiag_Push( int phase );                                // a nested stretch of `phase` begins (packet handling
void HzmDiag_Pop( void );                                      // inside the wait, the game inside the server, ...)
void HzmDiag_Nested( int phase, unsigned long long micros );   // `micros` of the running phase belong to `phase`
void HzmDiag_Flag( int flags );                                // something notable happened in this frame
void HzmDiag_LongOp( int flag, int begin, const char *what );  // a legitimately long operation starts / ends
void HzmDiag_NoteFile( const char *qpath );                    // a file was opened for reading
void HzmDiag_SetMap( const char *map );                        // the map this machine is on ("" = none)
void HzmDiag_ClientActive( int active );                       // this frame is drawn in a map (CA_ACTIVE)
void HzmDiag_CountSample( const int *counts );                 // HZC_NUM values, about once a second
void HzmDiag_LastCounts( int *now, float *avg );               // newest sample + last-minute average (-1 = unknown)
void HzmDiag_Event( const char *fmt, ... );                    // one line for the connection timeline
int  HzmDiag_Text( int section, char *buf, int size );         // plain text for the report; returns the length

// the newest freeze_*.txt files, newest first: fills paths[n][pathSize], returns how many (0 on other platforms)
int  HzmDiag_FreezeFiles( char *paths, int pathSize, int max );

#ifdef __cplusplus
}
#endif

#endif
