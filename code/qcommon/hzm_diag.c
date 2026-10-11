/*
===========================================================================
HZM coop [bug-3486] - always-on diagnostics for the in-game bug report. See hzm_diag.h.

Main-thread state only, except the freeze watchdog at the bottom (two threads that sleep until the main loop stops).
===========================================================================
*/

#include "q_shared.h"
#include "qcommon.h"
#include "hzm_diag.h"
#include <time.h>

#ifdef _WIN32
#	ifndef WIN32_LEAN_AND_MEAN
#		define WIN32_LEAN_AND_MEAN
#	endif
#	include <windows.h>
#	include <tlhelp32.h>
#endif

#define HZ_WINDOW_US     60000000LL
#define HZ_HITCH_US      100000LL
#define HZ_HITCHES       64
#define HZ_HITCH_NAMES   4
#define HZ_HITCH_NAMELEN 44
#define HZ_HITCH_AGE_SEC 600
#define HZ_FPS_MINUTES   120
#define HZ_EVENTS        100
#define HZ_EVENT_LEN     150
#define HZ_MAPLEN        32

// file kinds, by extension (what a hitch frame loaded)
enum { HZK_MODEL, HZK_ANIM, HZK_IMAGE, HZK_SOUND, HZK_SHADER, HZK_SCRIPT, HZK_MAP, HZK_CFG, HZK_OTHER, HZK_NUM };
static const char *const s_kindName[HZK_NUM] = {"model", "anim", "image", "sound", "shader", "script", "map", "cfg/menu", "other"};
static const char *const s_phaseName[HZP_NUM] = {"idle/fps-cap wait", "events/other", "server", "game/AI", "client+cgame",
												 "renderer front end", "renderer back end", "stalled in the wait"};
static const char *const s_phaseShort[HZP_NUM] = {"idle", "ev", "sv", "game", "cl", "fe", "be", "stall"};
static const char *const s_countName[HZC_NUM]  = {"snapshot entities", "skeletal models (busiest view)",
												  "skeletal surfaces (all views)", "draw surfaces (all views)", "views",
												  "dynamic (tess) batches", "temp models alive", "emitters alive",
												  "sound channels playing", "server entities in use", "living AI enemy",
												  "living AI friendly", "living players"};

typedef struct {
	int       frames;
	long long us[HZP_NUM];
	long long wallUs;
	long long worstWorkUs;
	long long cntSum[HZC_NUM];
	int       cntN[HZC_NUM];
} hzWindow_t;

typedef struct {
	time_t wall;
	int    workMs;
	int    phaseMs[HZP_NUM];
	int    flags;
	int    files, named;
	short  kinds[HZK_NUM];
	char   names[HZ_HITCH_NAMES][HZ_HITCH_NAMELEN];
	char   map[HZ_MAPLEN];
	char   what[24]; // the long operation's label, when there was one
} hzHitch_t;

typedef struct {
	char      map[HZ_MAPLEN];
	int       mapSerial;
	time_t    startWall;
	int       minute; // 1-based minute in this map
	int       frames;
	long long wallUs;
	long long worstUs;
	int       h50, h100;
	long long cntSum[HZC_NUM];
	int       cntN[HZC_NUM];
} hzFpsMin_t;

typedef struct {
	time_t wall;
	int    repeats;
	char   text[HZ_EVENT_LEN];
} hzEvent_t;

static qboolean   s_inited;
static long long  s_frameStart, s_phaseStart;
static int        s_phase = HZP_EVENTS;
static long long  s_acc[HZP_NUM];
static long long  s_idleBudgetUs;
static int        s_frameFlags;
static char       s_frameWhat[24];
static int        s_frameFiles, s_frameNamed;
static short      s_frameKinds[HZK_NUM];
static char       s_frameNames[HZ_HITCH_NAMES][HZ_HITCH_NAMELEN];
static int        s_clActive;

static hzWindow_t s_winCur, s_winLast;
static long long  s_winStart;

static hzHitch_t  s_hitches[HZ_HITCHES];
static int        s_hitchCount; // total ever; the ring holds the newest HZ_HITCHES

static char       s_map[HZ_MAPLEN];
static int        s_mapSerial;
static time_t     s_mapStart;
static hzFpsMin_t s_fps[HZ_FPS_MINUTES];
static int        s_fpsCount; // total ever closed
static hzFpsMin_t s_fpsCur;

static hzEvent_t  s_events[HZ_EVENTS];
static int        s_eventCount;

static int        s_cntNow[HZC_NUM];
static qboolean   s_cntHave;

static void HZ_WatchInit( void );
static void HZ_WatchBeat( void );
static void HZ_WatchLongOp( int begin, const char *what );
static void HZ_WatchPhase( int phase );
static void HZ_WatchModules( void );

/*
====================
HzmDiag_Micros
====================
*/
unsigned long long HzmDiag_Micros( void ) {
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
	return (unsigned long long)Sys_Milliseconds() * 1000ULL;
#endif
}

static void HZ_CleanName( const char *in, char *out, int size ) {
	int o = 0;
	for ( ; in && *in && o < size - 1; in++ ) {
		unsigned char c = (unsigned char)*in;
		out[o++] = ( ( c >= 'a' && c <= 'z' ) || ( c >= 'A' && c <= 'Z' ) || ( c >= '0' && c <= '9' ) || c == '_' || c == '-'
					 || c == '/' || c == '.' ) ? (char)c : '_';
	}
	out[o] = 0;
}

/*
====================
HzmDiag_NoteFile - a file was opened for reading (qcommon/files.cpp). A counter, a kind and the first few names.
====================
*/
void HzmDiag_NoteFile( const char *qpath ) {
	const char *ext;
	int         kind = HZK_OTHER;
	size_t      l;

	if ( !qpath ) {
		return;
	}
	l   = strlen( qpath );
	ext = l >= 4 ? qpath + l - 4 : "";
	if ( !Q_stricmp( ext, ".tik" ) || !Q_stricmp( ext, ".skd" ) || !Q_stricmp( ext, ".skb" ) ) {
		kind = HZK_MODEL;
	} else if ( !Q_stricmp( ext, ".skc" ) ) {
		kind = HZK_ANIM;
	} else if ( !Q_stricmp( ext, ".tga" ) || !Q_stricmp( ext, ".jpg" ) || !Q_stricmp( ext, ".dds" ) || !Q_stricmp( ext, ".png" ) ) {
		kind = HZK_IMAGE;
	} else if ( !Q_stricmp( ext, ".wav" ) || !Q_stricmp( ext, ".mp3" ) ) {
		kind = HZK_SOUND;
	} else if ( !Q_stricmp( ext, ".scr" ) || !Q_stricmp( ext, ".st" ) ) {
		kind = HZK_SCRIPT;
	} else if ( !Q_stricmp( ext, ".bsp" ) || !Q_stricmp( ext, ".min" ) ) {
		kind = HZK_MAP;
	} else if ( !Q_stricmp( ext, ".cfg" ) || !Q_stricmp( ext, ".urc" ) ) {
		kind = HZK_CFG;
	} else if ( l >= 7 && ( !Q_stricmp( qpath + l - 7, ".shader" ) || !Q_stricmp( qpath + l - 5, ".glsl" ) ) ) {
		kind = HZK_SHADER;
	}
	s_frameKinds[kind]++;
	s_frameFiles++;
	// Names are kept for game content only (models, images, sounds, scripts, maps, cfgs). Anything else is counted and
	// never named: a save or unlock file carries a per-player id in its NAME, and that must not reach a report.
	if ( kind != HZK_OTHER && s_frameNamed < HZ_HITCH_NAMES && !strstr( qpath, "save" ) ) {
		// keep the tail: it carries the file name and the extension
		HZ_CleanName( l >= HZ_HITCH_NAMELEN ? qpath + l - ( HZ_HITCH_NAMELEN - 1 ) : qpath, s_frameNames[s_frameNamed],
					  HZ_HITCH_NAMELEN );
		s_frameNamed++;
	}
}

void HzmDiag_Flag( int flags ) {
	s_frameFlags |= flags;
}

/*
====================
HzmDiag_LongOp - a map load / renderer restart begins or ends. The frame it happens in is expected to be long: the
hitch log names it, and the freeze watchdog waits 120 s instead of 5 s before it calls the frame stuck.
====================
*/
void HzmDiag_LongOp( int flag, int begin, const char *what ) {
	if ( begin ) {
		s_frameFlags |= flag;
		if ( what && !s_frameWhat[0] ) {
			Q_strncpyz( s_frameWhat, what, sizeof( s_frameWhat ) );
		}
	}
	HZ_WatchLongOp( begin, what );
	if ( !begin ) {
		HZ_WatchModules(); // DLLs were loaded / unloaded: refresh the module bases the watchdog prints
	}
}

void HzmDiag_SetMap( const char *map ) {
	HZ_CleanName( map ? map : "", s_map, sizeof( s_map ) );
	s_mapSerial++;
	s_mapStart = time( NULL );
}

void HzmDiag_ClientActive( int active ) {
	s_clActive = active;
}

void HzmDiag_Mark( int phase ) {
	long long now = (long long)HzmDiag_Micros();
	s_acc[s_phase] += now - s_phaseStart;
	s_phaseStart = now;
	s_phase      = phase;
	HZ_WatchPhase( phase );
}

/*
====================
HzmDiag_Push / HzmDiag_Pop - a stretch of another phase inside the running one. Network packets are handled inside the
fps-cap wait (NET_Sleep -> NET_Event), and a client's moves run game code from there: without these, a joiner's map
load and every remote player's ClientThink would be booked as "idle".
====================
*/
#define HZ_STACK 8
static int s_stack[HZ_STACK], s_depth;

void HzmDiag_Push( int phase ) {
	if ( s_depth < HZ_STACK ) {
		s_stack[s_depth] = s_phase;
	}
	s_depth++;
	HzmDiag_Mark( phase );
}

void HzmDiag_Pop( void ) {
	if ( s_depth <= 0 ) {
		return;
	}
	s_depth--;
	if ( s_depth < HZ_STACK ) {
		HzmDiag_Mark( s_stack[s_depth] );
	}
}

// the wait at the top of a frame is not work - unless it ran far past what was asked for (see HzmDiag_Frame)
void HzmDiag_IdleBudget( int msec ) {
	s_idleBudgetUs = msec > 0 ? (long long)msec * 1000 : 1000;
}

void HzmDiag_Nested( int phase, unsigned long long micros ) {
	if ( phase < 0 || phase >= HZP_NUM || phase == s_phase ) {
		return;
	}
	s_acc[phase] += (long long)micros;
	s_acc[s_phase] -= (long long)micros; // goes negative until the running phase is closed, which adds its wall time
}

static void HZ_FpsClose( void ) {
	if ( s_fpsCur.frames > 0 ) {
		s_fps[s_fpsCount % HZ_FPS_MINUTES] = s_fpsCur;
		s_fpsCount++;
	}
}

static void HZ_FpsFrame( long long wallUs, long long workUs ) {
	if ( s_fpsCur.mapSerial != s_mapSerial || s_fpsCur.wallUs >= HZ_WINDOW_US ) {
		int nextMinute = ( s_fpsCur.mapSerial == s_mapSerial ) ? s_fpsCur.minute + 1 : 1;
		HZ_FpsClose();
		memset( &s_fpsCur, 0, sizeof( s_fpsCur ) );
		Q_strncpyz( s_fpsCur.map, s_map, sizeof( s_fpsCur.map ) );
		s_fpsCur.mapSerial = s_mapSerial;
		s_fpsCur.startWall = time( NULL );
		s_fpsCur.minute    = nextMinute;
	}
	s_fpsCur.frames++;
	s_fpsCur.wallUs += wallUs;
	if ( workUs > s_fpsCur.worstUs ) {
		s_fpsCur.worstUs = workUs;
	}
	s_fpsCur.h50 += workUs >= 50000;
	s_fpsCur.h100 += workUs >= 100000;
}

/*
====================
HzmDiag_Frame - top of Com_Frame. Closes the frame that just ended (so a frame a Com_Error longjmp'd out of is still
counted) and starts the next one.
====================
*/
void HzmDiag_Frame( void ) {
	long long now = (long long)HzmDiag_Micros();
	int       p;

	if ( !s_inited ) {
		return;
	}
	HZ_WatchBeat();
	if ( s_frameStart ) {
		long long wall, work;

		s_acc[s_phase] += now - s_phaseStart;
		// A wait that overran its budget by more than 50 ms was not a wait: the process was stopped or starved (a
		// system-wide stall, a suspended thread). Without this such a freeze would hide inside "idle".
		if ( s_idleBudgetUs > 0 && s_acc[HZP_IDLE] > s_idleBudgetUs + 50000 ) {
			long long over = s_acc[HZP_IDLE] - s_idleBudgetUs;
			s_acc[HZP_IDLE] -= over;
			s_acc[HZP_STALL] += over;
		}
		wall = now - s_frameStart;
		work = wall - s_acc[HZP_IDLE];

		if ( now - s_winStart >= HZ_WINDOW_US || now < s_winStart ) {
			s_winLast = s_winCur;
			memset( &s_winCur, 0, sizeof( s_winCur ) );
			s_winStart = now;
		}
		s_winCur.frames++;
		s_winCur.wallUs += wall;
		for ( p = 0; p < HZP_NUM; p++ ) {
			s_winCur.us[p] += s_acc[p];
		}
		if ( work > s_winCur.worstWorkUs ) {
			s_winCur.worstWorkUs = work;
		}

		if ( work >= HZ_HITCH_US ) {
			hzHitch_t *h = &s_hitches[s_hitchCount % HZ_HITCHES];
			h->wall   = time( NULL );
			h->workMs = (int)( work / 1000 );
			for ( p = 0; p < HZP_NUM; p++ ) {
				// a flush or a game frame measured outside its usual phase can leave a bucket slightly negative
				h->phaseMs[p] = s_acc[p] > 0 ? (int)( s_acc[p] / 1000 ) : 0;
			}
			h->flags = s_frameFlags;
			h->files = s_frameFiles;
			h->named = s_frameNamed;
			memcpy( h->kinds, s_frameKinds, sizeof( h->kinds ) );
			memcpy( h->names, s_frameNames, sizeof( h->names ) );
			Q_strncpyz( h->map, s_map, sizeof( h->map ) );
			Q_strncpyz( h->what, s_frameWhat, sizeof( h->what ) );
			s_hitchCount++;
		}
		if ( s_clActive ) {
			HZ_FpsFrame( wall, work );
		}
	}
	memset( s_acc, 0, sizeof( s_acc ) );
	memset( s_frameKinds, 0, sizeof( s_frameKinds ) );
	s_frameFlags   = s_frameStart ? 0 : HZF_STARTUP; // the first frame runs the startup commands
	s_frameFiles   = 0;
	s_frameNamed   = 0;
	s_frameWhat[0] = 0;
	s_clActive     = 0;
	s_frameStart = s_phaseStart = now;
	s_phase      = HZP_EVENTS;
	s_depth      = 0; // a Com_Error may have jumped out of a pushed phase
}

/*
====================
HzmDiag_CountSample - about once a second, from the client
====================
*/
void HzmDiag_CountSample( const int *counts ) {
	int c;
	for ( c = 0; c < HZC_NUM; c++ ) {
		s_cntNow[c] = counts[c];
		if ( counts[c] >= 0 ) {
			s_winCur.cntSum[c] += counts[c];
			s_winCur.cntN[c]++;
			s_fpsCur.cntSum[c] += counts[c];
			s_fpsCur.cntN[c]++;
		}
	}
	s_cntHave = qtrue;
}

void HzmDiag_LastCounts( int *now, float *avg ) {
	const hzWindow_t *w = s_winLast.frames ? &s_winLast : &s_winCur;
	int               c;
	for ( c = 0; c < HZC_NUM; c++ ) {
		now[c] = s_cntHave ? s_cntNow[c] : -1;
		avg[c] = w->cntN[c] ? (float)w->cntSum[c] / w->cntN[c] : -1.0f;
	}
}

/*
====================
HzmDiag_Event - one line for the connection timeline. A repeat of the newest line only bumps its counter.
====================
*/
void HzmDiag_Event( const char *fmt, ... ) {
	va_list    ap;
	char       text[HZ_EVENT_LEN];
	hzEvent_t *e;
	int        i;

	va_start( ap, fmt );
	Q_vsnprintf( text, sizeof( text ), fmt, ap );
	va_end( ap );
	text[sizeof( text ) - 1] = 0;
	for ( i = 0; text[i]; i++ ) {
		if ( (unsigned char)text[i] < 0x20 || text[i] == 0x7f ) {
			text[i] = ' ';
		}
	}
	while ( i > 0 && text[i - 1] == ' ' ) {
		text[--i] = 0;
	}
	if ( s_eventCount > 0 ) {
		e = &s_events[( s_eventCount - 1 ) % HZ_EVENTS];
		if ( !strcmp( e->text, text ) ) {
			e->repeats++;
			e->wall = time( NULL );
			return;
		}
	}
	e          = &s_events[s_eventCount % HZ_EVENTS];
	e->wall    = time( NULL );
	e->repeats = 1;
	Q_strncpyz( e->text, text, sizeof( e->text ) );
	s_eventCount++;
}

/*
====================================================================================================================
Report text
====================================================================================================================
*/
typedef struct {
	char *p;
	int   size, len;
} hzOut_t;

static void HZ_Out( hzOut_t *o, const char *fmt, ... ) {
	va_list ap;
	int     n, room = o->size - o->len;
	if ( room <= 1 ) {
		return;
	}
	va_start( ap, fmt );
	n = Q_vsnprintf( o->p + o->len, room, fmt, ap );
	va_end( ap );
	if ( n < 0 || n >= room ) {
		n = room - 1;
	}
	o->len += n;
	o->p[o->len] = 0;
}

static const char *HZ_Clock( time_t t, char *buf, int size ) {
	struct tm *l = localtime( &t );
	if ( !l || !strftime( buf, size, "%H:%M:%S", l ) ) {
		Q_strncpyz( buf, "??:??:??", size );
	}
	return buf;
}

static void HZ_TextFrame( hzOut_t *o ) {
	const hzWindow_t *w    = s_winLast.frames ? &s_winLast : &s_winCur;
	const char       *which = s_winLast.frames ? "last full minute" : "current minute so far";
	int               p, c;
	int               now[HZC_NUM];
	float             avg[HZC_NUM];

	HZ_Out( o, "\r\n=== where the frame time goes (%s; average ms per frame) ===\r\n", which );
	if ( w->frames <= 0 || w->wallUs <= 0 ) {
		HZ_Out( o, "(no frames measured yet)\r\n" );
	} else {
		double f = (double)w->frames;
		HZ_Out( o, "frames=%d in %.1f s = %.1f fps | frame %.2f ms avg, of which work %.2f ms; worst frame %d ms of work\r\n", w->frames,
				w->wallUs / 1e6, f * 1e6 / (double)w->wallUs, w->wallUs / f / 1000.0,
				( w->wallUs - w->us[HZP_IDLE] ) / f / 1000.0, (int)( w->worstWorkUs / 1000 ) );
		for ( p = 0; p < HZP_NUM; p++ ) {
			HZ_Out( o, "  %-20s %7.2f ms  %5.1f%%\r\n", s_phaseName[p], w->us[p] / f / 1000.0,
					100.0 * (double)w->us[p] / (double)w->wallUs );
		}
	}
	HzmDiag_LastCounts( now, avg );
	HZ_Out( o, "counts (at report time / average over that minute, sampled once a second; n/a = not known here):\r\n" );
	for ( c = 0; c < HZC_NUM; c++ ) {
		char a[24], b[24];
		if ( now[c] >= 0 ) {
			Com_sprintf( a, sizeof( a ), "%d", now[c] );
		} else {
			Q_strncpyz( a, "n/a", sizeof( a ) );
		}
		if ( avg[c] >= 0 ) {
			Com_sprintf( b, sizeof( b ), "%.1f", avg[c] );
		} else {
			Q_strncpyz( b, "n/a", sizeof( b ) );
		}
		HZ_Out( o, "  %-32s %6s / %s\r\n", s_countName[c], a, b );
	}
}

static void HZ_FlagText( int flags, const char *what, char *out, int size ) {
	out[0] = 0;
	if ( flags & HZF_STARTUP ) {
		Q_strcat( out, size, " | engine start" );
	}
	if ( flags & HZF_MAPLOAD_SV ) {
		Q_strcat( out, size, " | MAP LOAD (server)" );
	}
	if ( flags & HZF_MAPLOAD_CL ) {
		Q_strcat( out, size, " | MAP LOAD (client)" );
	}
	if ( flags & HZF_VID_RESTART ) {
		Q_strcat( out, size, " | RENDERER RESTART" );
	}
	if ( flags & HZF_SND_RESTART ) {
		Q_strcat( out, size, " | sound restart" );
	}
	if ( flags & HZF_CONNECT ) {
		Q_strcat( out, size, " | connecting" );
	}
	if ( flags & HZF_ERROR_DROP ) {
		Q_strcat( out, size, " | an error dropped the frame" );
	}
	if ( what && what[0] ) {
		Q_strcat( out, size, va( " (%s)", what ) );
	}
}

static void HZ_TextHitches( hzOut_t *o ) {
	time_t now   = time( NULL );
	int    n     = s_hitchCount < HZ_HITCHES ? s_hitchCount : HZ_HITCHES;
	int    i, k, shown = 0, older = 0;

	HZ_Out( o, "\r\n=== hitch log: frames with >= 100 ms of work in the last %d min (newest first; ring of %d, %d this session) ===\r\n",
			HZ_HITCH_AGE_SEC / 60, HZ_HITCHES, s_hitchCount );
	HZ_Out( o, "time     work  map  | ms per phase | files opened in that frame (by kind): first names | what else\r\n" );
	for ( i = 0; i < n; i++ ) {
		const hzHitch_t *h = &s_hitches[( s_hitchCount - 1 - i ) % HZ_HITCHES];
		char             clock[16], flags[160];
		if ( now - h->wall > HZ_HITCH_AGE_SEC ) {
			older++;
			continue;
		}
		HZ_Out( o, "%s %5d ms  %s |", HZ_Clock( h->wall, clock, sizeof( clock ) ), h->workMs, h->map[0] ? h->map : "(no map)" );
		for ( k = HZP_EVENTS; k < HZP_NUM; k++ ) {
			if ( k == HZP_STALL && !h->phaseMs[k] ) {
				continue;
			}
			HZ_Out( o, " %s %d", s_phaseShort[k], h->phaseMs[k] );
		}
		if ( h->files > 0 ) {
			int first = 1;
			HZ_Out( o, " | %d files (", h->files );
			for ( k = 0; k < HZK_NUM; k++ ) {
				if ( h->kinds[k] ) {
					HZ_Out( o, "%s%s %d", first ? "" : ", ", s_kindName[k], h->kinds[k] );
					first = 0;
				}
			}
			HZ_Out( o, "):" );
			for ( k = 0; k < HZ_HITCH_NAMES && k < h->named; k++ ) {
				HZ_Out( o, " %s", h->names[k] );
			}
			if ( h->files > h->named ) {
				HZ_Out( o, " ..." );
			}
		} else {
			HZ_Out( o, " | no files opened" );
		}
		HZ_FlagText( h->flags, h->what, flags, sizeof( flags ) );
		HZ_Out( o, "%s\r\n", flags );
		shown++;
	}
	if ( !shown ) {
		HZ_Out( o, "(none)\r\n" );
	}
	if ( older ) {
		HZ_Out( o, "(%d older ones in the ring not shown)\r\n", older );
	}
}

static void HZ_TextFpsLine( hzOut_t *o, const hzFpsMin_t *m, int *lastSerial, qboolean current ) {
	static const int show[] = {HZC_SNAP_ENTS, HZC_SV_ENTS, HZC_SV_AI_ENEMY, HZC_SV_AI_FRIENDLY, HZC_SV_PLAYERS};
	static const char *const label[] = {"snapshot ents", "server ents", "AI enemy", "AI friendly", "players alive"};
	char   clock[16];
	size_t k;

	if ( m->frames <= 0 || m->wallUs <= 0 ) {
		return;
	}
	if ( m->mapSerial != *lastSerial ) {
		HZ_Out( o, "map %s\r\n", m->map[0] ? m->map : "(unknown)" );
		*lastSerial = m->mapSerial;
	}
	HZ_Out( o, "  %s min %3d: %4.0f fps avg, worst frame %4d ms, %3d frames >= 50 ms, %3d >= 100 ms |",
			HZ_Clock( m->startWall, clock, sizeof( clock ) ), m->minute, (double)m->frames * 1e6 / (double)m->wallUs,
			(int)( m->worstUs / 1000 ), m->h50, m->h100 );
	for ( k = 0; k < sizeof( show ) / sizeof( show[0] ); k++ ) {
		if ( m->cntN[show[k]] ) {
			HZ_Out( o, " %s %d", label[k], (int)( m->cntSum[show[k]] / m->cntN[show[k]] ) );
		}
	}
	if ( current ) {
		HZ_Out( o, " (current minute, %d s so far)", (int)( m->wallUs / 1000000 ) );
	}
	HZ_Out( o, "\r\n" );
}

static void HZ_TextFps( hzOut_t *o ) {
	int n = s_fpsCount < HZ_FPS_MINUTES ? s_fpsCount : HZ_FPS_MINUTES;
	int i, lastSerial = -1;

	HZ_Out( o, "\r\n=== frame-rate history: this session, per map, one line per minute in the map (newest %d minutes) ===\r\n",
			HZ_FPS_MINUTES );
	HZ_Out( o, "(worst frame = work time, the fps-cap wait excluded; counts are that minute's average)\r\n" );
	for ( i = n; i > 0; i-- ) {
		HZ_TextFpsLine( o, &s_fps[( s_fpsCount - i ) % HZ_FPS_MINUTES], &lastSerial, qfalse );
	}
	HZ_TextFpsLine( o, &s_fpsCur, &lastSerial, qtrue );
	if ( lastSerial == -1 ) {
		HZ_Out( o, "(no minute in a map yet)\r\n" );
	}
}

static void HZ_TextTimeline( hzOut_t *o ) {
	int n = s_eventCount < HZ_EVENTS ? s_eventCount : HZ_EVENTS;
	int i;

	HZ_Out( o, "\r\n=== connection timeline: this session, oldest first (newest %d of %d events) ===\r\n", n, s_eventCount );
	for ( i = n; i > 0; i-- ) {
		const hzEvent_t *e = &s_events[( s_eventCount - i ) % HZ_EVENTS];
		char             clock[16];
		HZ_Out( o, "%s  %s", HZ_Clock( e->wall, clock, sizeof( clock ) ), e->text );
		if ( e->repeats > 1 ) {
			HZ_Out( o, "  (x%d, last at this time)", e->repeats );
		}
		HZ_Out( o, "\r\n" );
	}
	if ( !n ) {
		HZ_Out( o, "(no events)\r\n" );
	}
}

int HzmDiag_Text( int section, char *buf, int size ) {
	hzOut_t o;
	o.p    = buf;
	o.size = size;
	o.len  = 0;
	if ( size > 0 ) {
		buf[0] = 0;
	}
	switch ( section ) {
	case HZT_FRAME:
		HZ_TextFrame( &o );
		break;
	case HZT_HITCHES:
		HZ_TextHitches( &o );
		break;
	case HZT_FPS:
		HZ_TextFps( &o );
		break;
	case HZT_TIMELINE:
		HZ_TextTimeline( &o );
		break;
	}
	return o.len;
}

/*
====================
hzm_diag - console: print the four sections (a dedicated server has no bug report; this is its view of them)
====================
*/
static void HzmDiag_Print_f( void ) {
	static char buf[32768];
	int         s;
	for ( s = 0; s < HZT_NUM; s++ ) {
		char *line = buf, *nl;
		HzmDiag_Text( s, buf, sizeof( buf ) );
		// Com_Printf cuts at MAXPRINTMSG: one line at a time
		while ( ( nl = strchr( line, '\n' ) ) != NULL ) {
			*nl = 0;
			if ( nl > line && nl[-1] == '\r' ) {
				nl[-1] = 0;
			}
			Com_Printf( "%s\n", line );
			line = nl + 1;
		}
	}
}

/*
====================================================================================================================
Freeze watchdog (Windows x64). Two threads:
  watch   wakes once a second and compares the frame counter. When it has not moved for 5 s (120 s inside a long
          operation) it asks the walker for a sample, and again at 15 s / 45 s (300 s / 600 s).
  walker  suspends the main thread, reads its context, unwinds it with RtlLookupFunctionEntry + RtlVirtualUnwind
          into a static array, resumes it, and only then formats and writes the file.
Nothing here allocates: the main thread may be stopped while it holds the CRT heap lock, and a walker that then
called malloc would turn a stall into a deadlock. The unwinder itself takes the loader's function-table lock for
reading; if the main thread was stopped holding it, the walker blocks - so `watch` waits 3 s for the walker and
resumes the main thread itself when it does not answer. Exactly one of the two resumes (s_wdSuspended).
====================================================================================================================
*/
#if defined( _WIN32 ) && defined( _M_X64 )

#define HZ_WD_FRAMES   64
#define HZ_WD_MODULES  192
#define HZ_WD_MAXFILES 12 // per session
#define HZ_WD_KEEP     10 // files kept on disk across sessions

typedef struct {
	DWORD64 base;
	DWORD   size;
	DWORD   stamp; // PE TimeDateStamp: matches a build to its .map / .pdb
	char    name[40];
} hzModule_t;

static cvar_t       *com_freezeWatch;
static HANDLE        s_wdMain, s_wdStop, s_wdWalkEvt, s_wdDoneEvt, s_wdWatchThread, s_wdWalkThread;
static volatile LONG s_wdBeat, s_wdLongOp, s_wdPhase, s_wdSuspended, s_wdBusy;
static char          s_wdLongWhat[24];
static wchar_t       s_wdDirW[MAX_OSPATH];
static char          s_wdDirA[MAX_OSPATH];
static hzModule_t    s_wdMods[HZ_WD_MODULES], s_wdModsCopy[HZ_WD_MODULES];
static volatile LONG s_wdModCount;
static int           s_wdModCountCopy;
static DWORD64       s_wdStack[HZ_WD_FRAMES];
static int           s_wdDepth;
static int           s_wdTimedOut;
// the request the watch thread hands to the walker
static wchar_t       s_wdFileW[MAX_OSPATH];
static DWORD         s_wdStalledMs;
static int           s_wdSample;
static int           s_wdFiles;
static char          s_wdText[16384];

static void HZ_WatchBeat( void ) {
	InterlockedIncrement( &s_wdBeat );
}

static void HZ_WatchPhase( int phase ) {
	s_wdPhase = phase;
}

static void HZ_WatchLongOp( int begin, const char *what ) {
	if ( begin ) {
		if ( what ) {
			Q_strncpyz( s_wdLongWhat, what, sizeof( s_wdLongWhat ) );
		}
		InterlockedIncrement( &s_wdLongOp );
	} else if ( s_wdLongOp > 0 ) {
		InterlockedDecrement( &s_wdLongOp );
	}
}

// main thread: the loaded modules' bases, sizes and link stamps (cheap; at start and after every map load / restart)
static void HZ_WatchModules( void ) {
	HANDLE         snap;
	MODULEENTRY32W me;
	char           leaf[64];
	int            n = 0;

	if ( !s_wdWatchThread ) {
		return;
	}
	snap = CreateToolhelp32Snapshot( TH32CS_SNAPMODULE, GetCurrentProcessId() );
	if ( snap == INVALID_HANDLE_VALUE ) {
		return;
	}
	me.dwSize = sizeof( me );
	if ( Module32FirstW( snap, &me ) ) {
		do {
			hzModule_t             *m   = &s_wdMods[n];
			const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)me.modBaseAddr;
			m->base  = (DWORD64)(ULONG_PTR)me.modBaseAddr;
			m->size  = me.modBaseSize;
			m->stamp = 0;
			if ( dos && dos->e_magic == IMAGE_DOS_SIGNATURE ) {
				const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)( (const BYTE *)dos + dos->e_lfanew );
				if ( nt->Signature == IMAGE_NT_SIGNATURE ) {
					m->stamp = nt->FileHeader.TimeDateStamp;
				}
			}
			leaf[0] = 0;
			WideCharToMultiByte( CP_ACP, 0, me.szModule, -1, leaf, sizeof( leaf ) - 1, NULL, NULL );
			leaf[sizeof( leaf ) - 1] = 0;
			HZ_CleanName( leaf, m->name, sizeof( m->name ) );
			n++;
		} while ( n < HZ_WD_MODULES && Module32NextW( snap, &me ) );
	}
	CloseHandle( snap );
	InterlockedExchange( &s_wdModCount, n );
}

static const hzModule_t *HZ_WdFindModule( DWORD64 addr ) {
	int i;
	for ( i = 0; i < s_wdModCountCopy; i++ ) {
		if ( addr >= s_wdModsCopy[i].base && addr < s_wdModsCopy[i].base + s_wdModsCopy[i].size ) {
			return &s_wdModsCopy[i];
		}
	}
	return NULL;
}

static void HZ_WdAppendFile( const wchar_t *path, const char *text, int len ) {
	HANDLE h = CreateFileW( path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL );
	DWORD  wrote;
	if ( h == INVALID_HANDLE_VALUE ) {
		return;
	}
	WriteFile( h, text, (DWORD)len, &wrote, NULL );
	CloseHandle( h );
}

// the walk itself: no CRT, no heap, nothing that can wait on the stopped thread except the unwinder's table lock
static void HZ_WdWalk( void ) {
	CONTEXT ctx;

	s_wdDepth    = 0;
	s_wdTimedOut = 0;
	if ( SuspendThread( s_wdMain ) == (DWORD)-1 ) {
		return;
	}
	InterlockedExchange( &s_wdSuspended, 1 );
	s_wdModCountCopy = s_wdModCount;
	memcpy( s_wdModsCopy, s_wdMods, sizeof( s_wdModsCopy ) );
	memset( &ctx, 0, sizeof( ctx ) );
	ctx.ContextFlags = CONTEXT_FULL;
	if ( GetThreadContext( s_wdMain, &ctx ) ) {
		__try {
			s_wdStack[s_wdDepth++] = ctx.Rip;
			while ( s_wdDepth < HZ_WD_FRAMES && s_wdSuspended ) {
				DWORD64           imageBase = 0;
				PRUNTIME_FUNCTION fn        = RtlLookupFunctionEntry( ctx.Rip, &imageBase, NULL );
				if ( !fn ) {
					// a leaf: the return address is at the top of the stack
					if ( !ctx.Rsp ) {
						break;
					}
					ctx.Rip = *(DWORD64 *)ctx.Rsp;
					ctx.Rsp += 8;
				} else {
					PVOID   handlerData = NULL;
					DWORD64 establisher = 0;
					RtlVirtualUnwind( UNW_FLAG_NHANDLER, imageBase, ctx.Rip, fn, &ctx, &handlerData, &establisher, NULL );
				}
				if ( !ctx.Rip ) {
					break;
				}
				s_wdStack[s_wdDepth++] = ctx.Rip;
			}
		} __except ( EXCEPTION_EXECUTE_HANDLER ) {
			// a torn stack: keep what was read
		}
	}
	if ( InterlockedExchange( &s_wdSuspended, 0 ) == 1 ) {
		ResumeThread( s_wdMain );
	} else {
		s_wdTimedOut = 1; // the watch thread had to resume it: the frames after the first few are not reliable
	}
}

// vsnprintf into the static text: integer and string formats only, so no allocation
static void HZ_WdCat( int *n, const char *fmt, ... ) {
	va_list ap;
	int     room = (int)sizeof( s_wdText ) - *n, w;
	if ( room <= 1 ) {
		return;
	}
	va_start( ap, fmt );
	w = _vsnprintf( s_wdText + *n, room - 1, fmt, ap );
	va_end( ap );
	if ( w < 0 || w > room - 1 ) {
		w = room - 1;
	}
	*n += w;
	s_wdText[*n] = 0;
}

static void HZ_WdWrite( void ) {
	static const char *const phase[HZP_NUM] = {"idle / fps-cap wait", "events / command buffer", "server frame",
											   "game / AI / scripts", "client + cgame", "renderer front end",
											   "renderer back end", "idle / fps-cap wait"};
	SYSTEMTIME st;
	int        n = 0, i, ph = s_wdPhase;
	int        sz = (int)sizeof( s_wdText );
	static const char *const always[] = {"openmohaa", "omohaaded", "game.", "cgame.", "renderer_"};

	GetLocalTime( &st );
	if ( s_wdSample == 1 ) {
		HZ_WdCat( &n, "MOH Coop freeze report - written by the watchdog while the game was not finishing frames.\r\n"
									   "Raw addresses only: symbolicate module+offset with that build's .map / .pdb.\r\n" );
	}
	HZ_WdCat( &n, "\r\n--- sample %d ---\r\nwhen=%04d-%02d-%02d %02d:%02d:%02d (local)\r\nstalled_so_far=%.1f s\r\n"
								   "frames_completed=%ld\r\nmain_thread_was_in=%s\r\nlong_operation=%s\r\nmap=%s\r\n",
					s_wdSample, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, s_wdStalledMs / 1000.0,
					(long)s_wdBeat, ( ph >= 0 && ph < HZP_NUM ) ? phase[ph] : "?", s_wdLongOp > 0 ? s_wdLongWhat : "none",
					s_map[0] ? s_map : "(none)" );
	if ( s_wdTimedOut ) {
		HZ_WdCat( &n, "note=the stack walk did not finish in 3 s; the main thread was let go and the list below may be torn\r\n" );
	}
	HZ_WdCat( &n, "stack (innermost first; %d frames):\r\n", s_wdDepth );
	for ( i = 0; i < s_wdDepth && n < sz - 200; i++ ) {
		const hzModule_t *m = HZ_WdFindModule( s_wdStack[i] );
		if ( m ) {
			HZ_WdCat( &n, "%2d 0x%016I64x %s+0x%I64x\r\n", i, s_wdStack[i], m->name, s_wdStack[i] - m->base );
		} else {
			HZ_WdCat( &n, "%2d 0x%016I64x ?\r\n", i, s_wdStack[i] );
		}
	}
	HZ_WdCat( &n, "modules (base size link-stamp name; the game's own and every one in the stack):\r\n" );
	for ( i = 0; i < s_wdModCountCopy && n < sz - 200; i++ ) {
		const hzModule_t *m    = &s_wdModsCopy[i];
		int               want = 0, k;
		for ( k = 0; k < (int)( sizeof( always ) / sizeof( always[0] ) ) && !want; k++ ) {
			want = !Q_stricmpn( m->name, always[k], (int)strlen( always[k] ) );
		}
		for ( k = 0; k < s_wdDepth && !want; k++ ) {
			want = s_wdStack[k] >= m->base && s_wdStack[k] < m->base + m->size;
		}
		if ( want ) {
			HZ_WdCat( &n, "0x%016I64x 0x%08lx 0x%08lx %s\r\n", m->base, (unsigned long)m->size,
							(unsigned long)m->stamp, m->name );
		}
	}
	if ( n > 0 ) {
		HZ_WdAppendFile( s_wdFileW, s_wdText, n < sz ? n : sz - 1 );
	}
}

static DWORD WINAPI HZ_WalkThread( LPVOID unused ) {
	HANDLE both[2];
	char   warm[64];
	(void)unused;
	// first use of the CRT's formatter sets up this thread's CRT data (an allocation): do it now, while all is well
	_snprintf( warm, sizeof( warm ), "%d %s 0x%016I64x %.1f", 1, "x", (unsigned __int64)1, 1.0 );
	both[0] = s_wdStop;
	both[1] = s_wdWalkEvt;
	for ( ;; ) {
		if ( WaitForMultipleObjects( 2, both, FALSE, INFINITE ) != WAIT_OBJECT_0 + 1 ) {
			break;
		}
		HZ_WdWalk();
		HZ_WdWrite();
		InterlockedExchange( &s_wdBusy, 0 );
		SetEvent( s_wdDoneEvt );
	}
	return 0;
}

static void HZ_WdRequest( DWORD stalledMs, int sample ) {
	if ( InterlockedExchange( &s_wdBusy, 1 ) ) {
		return; // the walker never came back from the previous sample
	}
	if ( sample == 1 ) {
		SYSTEMTIME st;
		if ( s_wdFiles >= HZ_WD_MAXFILES ) {
			InterlockedExchange( &s_wdBusy, 0 );
			return;
		}
		s_wdFiles++;
		GetLocalTime( &st );
		wsprintfW( s_wdFileW, L"%s\\freeze_%04d%02d%02d-%02d%02d%02d.txt", s_wdDirW, st.wYear, st.wMonth, st.wDay, st.wHour,
				   st.wMinute, st.wSecond );
	}
	s_wdStalledMs = stalledMs;
	s_wdSample    = sample;
	ResetEvent( s_wdDoneEvt );
	SetEvent( s_wdWalkEvt );
	if ( WaitForSingleObject( s_wdDoneEvt, 3000 ) == WAIT_TIMEOUT ) {
		if ( InterlockedExchange( &s_wdSuspended, 0 ) == 1 ) {
			ResumeThread( s_wdMain );
		}
	}
}

static DWORD WINAPI HZ_WatchThread( LPVOID unused ) {
	static const DWORD shortAt[3] = {5000, 15000, 45000};
	static const DWORD longAt[3]  = {120000, 300000, 600000};
	LONG  lastBeat = s_wdBeat;
	DWORD stalled = 0, lastTick = GetTickCount();
	int   stage = 0;
	char  warm[32];
	(void)unused;
	_snprintf( warm, sizeof( warm ), "%.0f", 1.0 ); // see HZ_WalkThread

	for ( ;; ) {
		DWORD now, dt;
		LONG  beat;
		if ( WaitForSingleObject( s_wdStop, 1000 ) != WAIT_TIMEOUT ) {
			break;
		}
		now      = GetTickCount();
		dt       = now - lastTick;
		lastTick = now;
		if ( dt > 2000 ) {
			dt = 2000; // the PC slept, or this thread was starved: never count that as a stall of the game
		}
		beat = s_wdBeat;
		if ( beat != lastBeat ) {
			if ( stage > 0 && !s_wdBusy ) {
				char line[96];
				int  n = _snprintf( line, sizeof( line ), "\r\nframes resumed after about %.0f s\r\n", stalled / 1000.0 );
				HZ_WdAppendFile( s_wdFileW, line, n );
			}
			lastBeat = beat;
			stalled  = 0;
			stage    = 0;
			continue;
		}
		stalled += dt;
		if ( stage < 3 && stalled >= ( s_wdLongOp > 0 ? longAt[stage] : shortAt[stage] ) ) {
			stage++;
			HZ_WdRequest( stalled, stage );
		}
	}
	return 0;
}

static int QDECL HZ_CmpNameDesc( const void *a, const void *b ) {
	return -strcmp( (const char *)a, (const char *)b );
}

// newest first (the names are timestamps)
int HzmDiag_FreezeFiles( char *paths, int pathSize, int max ) {
	static char      names[64][40];
	WIN32_FIND_DATAA fd;
	HANDLE           h;
	int              n = 0, i;

	if ( !s_wdDirA[0] ) {
		return 0;
	}
	h = FindFirstFileA( va( "%s\\freeze_*.txt", s_wdDirA ), &fd );
	if ( h == INVALID_HANDLE_VALUE ) {
		return 0;
	}
	do {
		if ( strlen( fd.cFileName ) < sizeof( names[0] ) ) {
			if ( n == 64 ) {
				// more than 64 on disk: keep the newest 64 seen so far
				qsort( names, n, sizeof( names[0] ), HZ_CmpNameDesc );
				n = 63;
			}
			Q_strncpyz( names[n++], fd.cFileName, sizeof( names[0] ) );
		}
	} while ( FindNextFileA( h, &fd ) );
	FindClose( h );
	qsort( names, n, sizeof( names[0] ), HZ_CmpNameDesc );
	for ( i = 0; i < n && i < max; i++ ) {
		Com_sprintf( paths + i * pathSize, pathSize, "%s\\%s", s_wdDirA, names[i] );
	}
	return i;
}

static void HZ_WatchInit( void ) {
	char  paths[HZ_WD_KEEP + 24][MAX_OSPATH];
	char  mk[MAX_OSPATH];
	int   n, i;
	char *c;

	com_freezeWatch = Cvar_Get( "com_freezeWatch", "1", 0 );
	Q_strncpyz( s_wdDirA, FS_BuildOSPath( Cvar_VariableString( "fs_homepath" ), FS_GetCurrentGameDir(), "coop_freeze" ),
				sizeof( s_wdDirA ) );
	for ( c = s_wdDirA; *c; c++ ) {
		if ( *c == '/' ) {
			*c = '\\';
		}
	}
	// old files: keep the newest HZ_WD_KEEP
	n = HzmDiag_FreezeFiles( paths[0], MAX_OSPATH, HZ_WD_KEEP + 24 );
	for ( i = HZ_WD_KEEP; i < n; i++ ) {
		DeleteFileA( paths[i] );
	}
	if ( !com_freezeWatch->integer ) {
		return;
	}
	Com_sprintf( mk, sizeof( mk ), "%s\\x", s_wdDirA );
	FS_CreatePath( mk );
	if ( !MultiByteToWideChar( CP_ACP, 0, s_wdDirA, -1, s_wdDirW, MAX_OSPATH ) ) {
		return;
	}
	if ( !DuplicateHandle( GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &s_wdMain,
						   THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0 ) ) {
		return;
	}
	s_wdStop    = CreateEventA( NULL, TRUE, FALSE, NULL );
	s_wdWalkEvt = CreateEventA( NULL, FALSE, FALSE, NULL );
	s_wdDoneEvt = CreateEventA( NULL, TRUE, FALSE, NULL );
	if ( !s_wdStop || !s_wdWalkEvt || !s_wdDoneEvt ) {
		return;
	}
	s_wdWalkThread  = CreateThread( NULL, 0, HZ_WalkThread, NULL, 0, NULL );
	s_wdWatchThread = CreateThread( NULL, 0, HZ_WatchThread, NULL, 0, NULL );
	HZ_WatchModules();
}

static void HZ_WatchShutdown( void ) {
	if ( s_wdStop ) {
		SetEvent( s_wdStop );
		if ( s_wdWatchThread ) {
			WaitForSingleObject( s_wdWatchThread, 1500 );
		}
	}
}

#else // not Windows x64: no watchdog

static void HZ_WatchInit( void ) {}
static void HZ_WatchShutdown( void ) {}
static void HZ_WatchBeat( void ) {}
static void HZ_WatchLongOp( int begin, const char *what ) { (void)begin; (void)what; }
static void HZ_WatchPhase( int phase ) { (void)phase; }
static void HZ_WatchModules( void ) {}
int HzmDiag_FreezeFiles( char *paths, int pathSize, int max ) { (void)paths; (void)pathSize; (void)max; return 0; }

#endif

/*
====================
HzmDiag_Init / HzmDiag_Shutdown
====================
*/
void HzmDiag_Init( void ) {
	int c;
	if ( s_inited ) {
		return;
	}
	for ( c = 0; c < HZC_NUM; c++ ) {
		s_cntNow[c] = -1;
	}
	s_winStart = (long long)HzmDiag_Micros();
	Cmd_AddCommand( "hzm_diag", HzmDiag_Print_f );
	HZ_WatchInit();
	s_inited = qtrue;
}

void HzmDiag_Shutdown( void ) {
	if ( !s_inited ) {
		return;
	}
	HZ_WatchShutdown();
}
