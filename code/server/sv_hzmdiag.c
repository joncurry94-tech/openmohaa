/*
===========================================================================
HZM coop [bug-3486] - the server's side of the bug-report diagnostics (see qcommon/hzm_diag.h).

  SV_HzmGameText   asks game.dll for report text (mission state, script errors, counts) through its exported
                   G_HzmDiag symbol. An older game.dll does not export it: the caller gets 0 and says "not available".
  SV_HzmDiag_f     a JOINER's report cannot read the host's game, so its client sends the reliable command `hzmdiag`
                   when the report menu opens and the host answers with at most SV_HZD_CHUNKS reliable commands
                   (`hzmdiag <n> <total> "<text>"`, a few KB in all). One answer per client per 15 s; nothing is ever
                   sent unasked, so normal play carries no extra traffic.
===========================================================================
*/

#include "server.h"
#include "../qcommon/hzm_diag.h"

#define SV_HZD_CHUNK   700
#define SV_HZD_CHUNKS  6
#define SV_HZD_COOLDOWN 15000

void *Sys_GameSymbol( const char *name ); // sys/new/sys_main_new.c

typedef int ( *hzmGameDiag_t )( int what, int clientNum, char *buf, int size );

/*
====================
SV_HzmGameText - what: 1 mission state (host, full), 2 mission state for one joiner (compact), 3 script errors,
4 counts (buf is int[4]: entities in use, living AI enemy, living AI friendly, living players)
====================
*/
int SV_HzmGameText( int what, int clientNum, char *buf, int size ) {
	hzmGameDiag_t fn;

	if ( size > 0 ) {
		buf[0] = 0;
	}
	if ( !com_sv_running || !com_sv_running->integer || !ge || sv.state != SS_GAME ) {
		return 0;
	}
	fn = (hzmGameDiag_t)Sys_GameSymbol( "G_HzmDiag" );
	if ( !fn ) {
		return 0;
	}
	return fn( what, clientNum, buf, size );
}

/*
====================
SV_HzmKeepScriptErrors - SV_ShutdownGameProgs, just before game.dll is unloaded (every map change): the script errors of
the map that is ending would be gone with the DLL, and a report filed on the next map is exactly the one that wants
them. Kept as text, newest 12, one map back.
====================
*/
static char s_prevScriptErrors[4096];

void SV_HzmKeepScriptErrors( void ) {
	hzmGameDiag_t fn;
	char          buf[sizeof( s_prevScriptErrors )];

	if ( !ge ) {
		return;
	}
	fn = (hzmGameDiag_t)Sys_GameSymbol( "G_HzmDiag" );
	if ( !fn ) {
		return;
	}
	buf[0] = 0;
	if ( fn( 5, -1, buf, sizeof( buf ) ) > 0 && buf[0] ) {
		Q_strncpyz( s_prevScriptErrors, buf, sizeof( s_prevScriptErrors ) );
	}
}

const char *SV_HzmPrevScriptErrors( void ) {
	return s_prevScriptErrors;
}

/*
====================
SV_HzmDiag_f - client command `hzmdiag`
====================
*/
void SV_HzmDiag_f( client_t *cl ) {
	static int  lastAnswer[MAX_CLIENTS];
	static char text[SV_HZD_CHUNK * SV_HZD_CHUNKS + 1];
	char        chunk[SV_HZD_CHUNK + 1];
	int         clientNum = (int)( cl - svs.clients );
	int         now       = Sys_Milliseconds();
	int         len, total, i;

	if ( clientNum < 0 || clientNum >= MAX_CLIENTS || cl->state != CS_ACTIVE ) {
		return;
	}
	if ( lastAnswer[clientNum] && now - lastAnswer[clientNum] < SV_HZD_COOLDOWN && now >= lastAnswer[clientNum] ) {
		return;
	}
	lastAnswer[clientNum] = now ? now : 1;

	len = SV_HzmGameText( 2, clientNum, text, sizeof( text ) );
	if ( len <= 0 ) {
		Q_strncpyz( text, "(this host's game.dll has no mission state to give)", sizeof( text ) );
	}
	text[sizeof( text ) - 1] = 0;
	// one token per command: no quote, no '%' (MSG_WriteString eats it), lines joined with '`'
	for ( i = 0; text[i]; i++ ) {
		unsigned char c = (unsigned char)text[i];
		if ( c == '\n' ) {
			text[i] = '`';
		} else if ( c == '"' || c == '`' ) {
			text[i] = '\'';
		} else if ( c < 0x20 || c > 126 || c == '%' ) {
			text[i] = ' ';
		}
	}
	len   = i;
	total = ( len + SV_HZD_CHUNK - 1 ) / SV_HZD_CHUNK;
	if ( total < 1 ) {
		total = 1;
	}
	for ( i = 0; i < total; i++ ) {
		Q_strncpyz( chunk, text + i * SV_HZD_CHUNK, sizeof( chunk ) );
		SV_SendServerCommand( cl, "hzmdiag %d %d \"%s\"", i, total, chunk );
	}
}
