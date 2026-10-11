/*
===========================================================================
HZM coop - in-game "Report a Bug", parity with the desktop reporter (bugreport_parity_2026-09-29, bug-3278).

Flow (docs/proposals/bugreport_parity_2026-09-29/DESIGN.md):
  coop_reportopen      REPORT A BUG button (after its own `pushmenu coop_report`): remembers when the menu opened;
                       inside a map, CL_BugReportFrame closes the menus (retrying until the menu manager lets go),
                       waits two rendered frames, takes `screenshotJPEG coop_report_shot` of the game view and
                       re-opens coop_report. Outside a map it only resets the page.
  coop_reportprepare   REVIEW button: builds <homepath>/<gamedir>/coop_report/current/ from scratch - payload.json
                       (the exact Discord JSON), files/ (only the ticked attachments, already scrubbed), zipname.txt,
                       preview.txt - fingerprints all of it (SHA-256), fills the preview cvars and pushes
                       coop_report_review. What the review page shows IS the upload.
  coop_sendreport      SEND button: refuses if a byte changed since the preview, if rate-limited or already sending;
                       writes the embedded uploader (cl_bugreport_uploader.h, generated from uploader.ps1) next to the
                       files and runs it with a private environment block. CL_BugReportFrame polls status.txt.
  coop_reportshowfiles OPEN FOLDER button: Explorer on current/, so the player can read every byte first.

Privacy: every attached text and every message field goes through BR_Scrub (home paths, Windows user and PC name in
ANSI and UTF-8, IPs, webhook URLs, cd keys, password/rcon/token/guid values, userinfo blobs, join codes, resolved host
names); settings.cfg is an ALLOWLIST (engine prefixes + coop_* the engine registers or the shipped cfgs seed) with a
denylist on top. The cd-key files (qkey/q3key), the unlock/save folder and crash-dump memory are never read into a
report. The webhook comes only from FILES (updater.ini, else the loose coop_reportwebhook.cfg), never from the cvar a
server could rewrite, only at send time, only if it is a Discord webhook URL, and reaches the child through its
environment block, never argv. Servers cannot name any coop_report* cvar, command, menu or widget (cmd_filter.c).

Diagnostics [bug-3486]: report_info.txt also carries where the player is, the mission state (objectives from the
configstrings; players, AI and level.coop_* from the host's game - a joiner asks the host once with the reliable command
`hzmdiag` when the menu opens and gets a few KB back), the script errors game.dll keeps regardless of `developer`, where
the frame time goes, a hitch log, a per-map frame-rate history, a connection timeline and the newest freeze-watchdog
files. The collectors are qcommon/hzm_diag.c (+ sv_hzmdiag.c, fgame/g_hzmdiag.cpp, cgame/cg_hzmdiag.cpp,
renderergl2/tr_hzm_perf.c); the DLLs' parts are found by exported symbol, so any mix of old and new binaries just leaves
a section saying "not available". All of it goes through BR_Scrub; server-supplied text is cleaned line by line, bounded
and written behind a fixed prefix.

BUGREPORT_SELFTEST builds only the pure helpers (scrubber, JSON, settings rule) with a main() for
docs/proposals/bugreport_parity_2026-09-29/tools/selftest_scrub.py.
===========================================================================
*/

#ifdef BUGREPORT_SELFTEST
#	include <stdio.h>
#	include <stdlib.h>
#	include <string.h>
#	include <stdarg.h>
#	include <ctype.h>
#	include <io.h>
#	include <fcntl.h>
#	define Q_stricmp _stricmp
#	define Q_stricmpn _strnicmp
#	define Q_strncpyz( d, s, n ) ( strncpy( ( d ), ( s ), ( n ) - 1 ), ( d )[( n ) - 1] = 0 )
#else
#	include "client.h"
#	include "cl_ui.h"
#	include "../server/server.h"
#	include "../sys/sys_local.h"
#	include "../qcommon/hzm_diag.h" // HZM coop [bug-3486]
#endif

#ifdef _WIN32
#	ifndef WIN32_LEAN_AND_MEAN
#		define WIN32_LEAN_AND_MEAN
#	endif
#	ifndef NOMINMAX
#		define NOMINMAX
#	endif
#	include <windows.h>
#	include <bcrypt.h>
#	include <shellapi.h>
#	pragma comment( lib, "bcrypt.lib" )
#	pragma comment( lib, "shell32.lib" )
#	pragma comment( lib, "advapi32.lib" )
#endif

/*
====================================================================================================================
Growable byte buffer (length-based: qconsole.log can hold NULs, TRAPS T14a)
====================================================================================================================
*/
typedef struct {
	char   *p;
	size_t  n;
	size_t  cap;
} brbuf_t;

static void BR_Reserve( brbuf_t *b, size_t add ) {
	if ( b->n + add + 1 <= b->cap ) {
		return;
	}
	size_t nc = b->cap ? b->cap : 1024;
	while ( nc < b->n + add + 1 ) {
		nc *= 2;
	}
	char *np = (char *)realloc( b->p, nc );
	if ( !np ) {
		return;
	}
	b->p   = np;
	b->cap = nc;
}

static void BR_Put( brbuf_t *b, const char *s, size_t len ) {
	BR_Reserve( b, len );
	if ( b->n + len + 1 > b->cap ) {
		return;
	}
	memcpy( b->p + b->n, s, len );
	b->n += len;
	b->p[b->n] = 0;
}

static void BR_Puts( brbuf_t *b, const char *s ) {
	BR_Put( b, s, strlen( s ) );
}

static void BR_Printf( brbuf_t *b, const char *fmt, ... ) {
	char    tmp[4096];
	va_list ap;
	va_start( ap, fmt );
	int n = vsnprintf( tmp, sizeof( tmp ), fmt, ap );
	va_end( ap );
	if ( n < 0 ) {
		return;
	}
	if ( n >= (int)sizeof( tmp ) ) {
		n = (int)sizeof( tmp ) - 1;
	}
	BR_Put( b, tmp, (size_t)n );
}

static void BR_Free( brbuf_t *b ) {
	free( b->p );
	b->p = NULL;
	b->n = b->cap = 0;
}

static const char *BR_P( const brbuf_t *b ) {
	return b->p ? b->p : "";
}

/*
====================================================================================================================
Scrubber (pure). Rules and order match installer/report_problem.ps1 Scrub-Text; tools/selftest_scrub.py runs both on
one corpus and requires identical output.
====================================================================================================================
*/
#define BR_ID_VARIANTS 2
#define BR_MAX_NAMES   160 // MAX_CLIENTS (64) x {full name, "Name#tag" stem, "Name ,marker" stem} would be 192; 160 covers real games
#define BR_NAME_LEN    48
typedef struct {
	char profile[BR_ID_VARIANTS][512]; // %USERPROFILE%   [0] ANSI, [1] UTF-8
	char user[BR_ID_VARIANTS][128];    // %USERNAME%
	char pc[BR_ID_VARIANTS][128];      // %COMPUTERNAME%
	char cdkey[40];                    // the live cd key (never printed), blank when none
	char names[BR_MAX_NAMES][BR_NAME_LEN]; // player names to remove (your own + every one the server has told us of)
	int  nNames;
} brscrub_t;

static int BR_Lower( int c ) {
	return ( c >= 'A' && c <= 'Z' ) ? c + 32 : c;
}

static int BR_IsDigit( int c ) {
	return c >= '0' && c <= '9';
}

static int BR_IsHex( int c ) {
	return BR_IsDigit( c ) || ( c >= 'a' && c <= 'f' ) || ( c >= 'A' && c <= 'F' );
}

static int BR_IsWordChar( int c ) {
	return ( c >= 'a' && c <= 'z' ) || ( c >= 'A' && c <= 'Z' ) || BR_IsDigit( c ) || c == '_' || ( c & 0x80 );
}

static int BR_IsSep( int c ) {
	return c == '\\' || c == '/';
}

static int BR_MatchCI( const char *s, size_t avail, const char *lit ) {
	size_t l = strlen( lit );
	if ( l > avail ) {
		return 0;
	}
	for ( size_t i = 0; i < l; i++ ) {
		if ( BR_Lower( (unsigned char)s[i] ) != BR_Lower( (unsigned char)lit[i] ) ) {
			return 0;
		}
	}
	return 1;
}

static void BR_Truncate( brbuf_t *b, size_t n ) {
	b->n = n;
	if ( b->p ) {
		b->p[n] = 0;
	}
}

// 1. any webhook URL (".../api/webhooks/<id>/<token>", whatever the host) -> <webhook>
static void BR_PassWebhook( const char *in, size_t n, brbuf_t *out ) {
	size_t i = 0;
	while ( i < n ) {
		if ( BR_MatchCI( in + i, n - i, "api/webhooks/" ) ) {
			size_t k = out->n;
			while ( k > 0 ) {
				int c = (unsigned char)out->p[k - 1];
				if ( BR_IsWordChar( c ) || c == '.' || c == ':' || c == '/' || c == '-' ) {
					k--;
				} else {
					break;
				}
			}
			BR_Truncate( out, k );
			i += 13;
			while ( i < n && ( BR_IsWordChar( (unsigned char)in[i] ) || in[i] == '/' || in[i] == '-' ) ) {
				i++;
			}
			BR_Puts( out, "<webhook>" );
			continue;
		}
		BR_Put( out, in + i, 1 );
		i++;
	}
}

// 2. SVC_DirectConnect's ">>>userinfo<<<" (other players' cl_guid, name, rate...) -> ">>><userinfo><<<"
static void BR_PassUserinfo( const char *in, size_t n, brbuf_t *out ) {
	size_t i = 0;
	while ( i < n ) {
		if ( i + 3 <= n && !memcmp( in + i, ">>>", 3 ) ) {
			size_t e = i + 3;
			while ( e + 3 <= n && memcmp( in + e, "<<<", 3 ) && in[e] != '\n' ) {
				e++;
			}
			if ( e + 3 <= n && !memcmp( in + e, "<<<", 3 ) ) {
				BR_Puts( out, ">>><userinfo><<<" );
				i = e + 3;
				continue;
			}
		}
		BR_Put( out, in + i, 1 );
		i++;
	}
}

static int BR_SensitiveWord( const char *s, size_t len ) {
	// substrings (anywhere in the word) and whole words. "key" / "auth" are whole-word only, so "keyboard" and
	// "author" survive; "apikey" / "api_key" / "q3key" / "bearer" ("Authorization: Bearer <token>") are substrings.
	static const char *const subs[] = {"password", "passwd", "rcon", "token", "secret", "cdkey", "qkey", "q3key", "apikey",
									   "api_key", "bearer", "webhook", "guid", "rdv"};
	static const char *const whole[] = {"pass", "pw", "coop_join", "key", "auth"};
	if ( len == 0 ) {
		return 0;
	}
	for ( size_t k = 0; k < sizeof( whole ) / sizeof( whole[0] ); k++ ) {
		if ( len == strlen( whole[k] ) && BR_MatchCI( s, len, whole[k] ) ) {
			return 1;
		}
	}
	for ( size_t k = 0; k < sizeof( subs ) / sizeof( subs[0] ); k++ ) {
		size_t l = strlen( subs[k] );
		for ( size_t i = 0; i + l <= len; i++ ) {
			if ( BR_MatchCI( s + i, len - i, subs[k] ) ) {
				return 1;
			}
		}
	}
	return 0;
}

// 3. "<sensitive word><sep><value>" -> "<sensitive word><sep><redacted>". sep = spaces, tabs, '=', ':', '\', at most
// one '"' or '\''. value = to whitespace (unless quoted), '\', a quote, ';', '<', '>' (never an earlier
// placeholder) or end of line.
static void BR_PassKeyValue( const char *in, size_t n, brbuf_t *out ) {
	size_t i = 0;
	while ( i < n ) {
		if ( BR_IsWordChar( (unsigned char)in[i] ) && ( i == 0 || !BR_IsWordChar( (unsigned char)in[i - 1] ) ) ) {
			size_t e = i;
			while ( e < n && BR_IsWordChar( (unsigned char)in[e] ) ) {
				e++;
			}
			BR_Put( out, in + i, e - i );
			if ( BR_SensitiveWord( in + i, e - i ) ) {
				size_t k = e;
				char   q = 0;
				while ( k < n && ( in[k] == ' ' || in[k] == '\t' || in[k] == '=' || in[k] == ':' || in[k] == '\\'
								   || ( ( in[k] == '"' || in[k] == '\'' ) && !q ) ) ) {
					if ( in[k] == '"' || in[k] == '\'' ) {
						q = in[k];
					}
					k++;
				}
				size_t v = k;
				while ( v < n && in[v] != '\r' && in[v] != '\n' && in[v] != '"' && in[v] != '\'' && in[v] != '\\'
						&& in[v] != ';' && in[v] != '<' && in[v] != '>' && ( q || ( in[v] != ' ' && in[v] != '\t' ) ) ) {
					v++;
				}
				if ( v > k ) {
					BR_Put( out, in + e, k - e );
					BR_Puts( out, "<redacted>" );
					i = v;
					continue;
				}
			}
			i = e;
			continue;
		}
		BR_Put( out, in + i, 1 );
		i++;
	}
}

// 4. "<host> resolved to" -> "<host> resolved to" with the typed server name replaced by <host>; lines mentioning
// "rendezvous" / "coop_join": every '...' quoted token -> '<code>' (join codes)
static void BR_PassHostsAndCodes( const char *in, size_t n, brbuf_t *out ) {
	size_t i = 0;
	while ( i < n ) {
		size_t e = i;
		while ( e < n && in[e] != '\n' ) {
			e++;
		}
		if ( e < n ) {
			e++;
		}
		// one line [i, e)
		const char *line = in + i;
		size_t      len  = e - i;
		size_t      rt   = (size_t)-1;
		for ( size_t k = 0; k + 13 <= len; k++ ) {
			if ( !memcmp( line + k, " resolved to ", 13 ) ) {
				rt = k;
				break;
			}
		}
		int codes = 0;
		for ( size_t k = 0; k + 9 <= len && !codes; k++ ) {
			if ( BR_MatchCI( line + k, len - k, "rendezvous" ) || BR_MatchCI( line + k, len - k, "coop_join" ) ) {
				codes = 1;
			}
		}
		if ( rt != (size_t)-1 ) {
			size_t s = rt;
			while ( s > 0 && line[s - 1] != ' ' && line[s - 1] != ']' ) {
				s--;
			}
			BR_Put( out, line, s );
			BR_Puts( out, "<host>" );
			BR_Put( out, line + rt, len - rt );
		} else if ( codes ) {
			for ( size_t k = 0; k < len; k++ ) {
				if ( line[k] == '\'' ) {
					size_t z = k + 1;
					while ( z < len && line[z] != '\'' && line[z] != '\n' ) {
						z++;
					}
					if ( z < len && line[z] == '\'' ) {
						BR_Puts( out, "'<code>'" );
						k = z;
						continue;
					}
				}
				BR_Put( out, line + k, 1 );
			}
		} else {
			BR_Put( out, line, len );
		}
		i = e;
	}
}

// literal, case-insensitive, optionally whole-word replace
static void BR_PassLiteral( const char *in, size_t n, brbuf_t *out, const char *lit, const char *rep, int wholeWord ) {
	size_t l = lit ? strlen( lit ) : 0;
	size_t i = 0;
	while ( i < n ) {
		if ( l >= 2 && BR_MatchCI( in + i, n - i, lit )
			 && ( !wholeWord
				  || ( ( i == 0 || !BR_IsWordChar( (unsigned char)in[i - 1] ) )
					   && ( i + l >= n || !BR_IsWordChar( (unsigned char)in[i + l] ) ) ) ) ) {
			BR_Puts( out, rep );
			i += l;
			continue;
		}
		BR_Put( out, in + i, 1 );
		i++;
	}
}

static void BR_Swap( brbuf_t *a, brbuf_t *b ) {
	brbuf_t t = *a;
	*a        = *b;
	*b        = t;
}

// one pass of BR_PassLiteral in place on *cur
static void BR_ReplaceIn( brbuf_t *cur, const char *lit, const char *rep, int wholeWord ) {
	brbuf_t t = {0};
	BR_PassLiteral( BR_P( cur ), cur->n, &t, lit, rep, wholeWord );
	BR_Swap( cur, &t );
	BR_Free( &t );
}

// 5. the profile path in its spellings: C:\Users\x, C:/Users/x, C:\\Users\\x (JSON), /c/Users/x (MSYS)
static void BR_PassProfile( brbuf_t *cur, const char *profile ) {
	size_t pl = strlen( profile );
	if ( pl < 4 || pl >= 500 ) {
		return;
	}
	char v[4][1100];
	size_t a = 0, b = 0, c = 0, d = 0;
	for ( size_t i = 0; i < pl; i++ ) {
		char ch = profile[i];
		if ( BR_IsSep( (unsigned char)ch ) ) {
			v[0][a++] = '\\';
			v[1][b++] = '/';
			v[2][c++] = '\\';
			v[2][c++] = '\\';
			v[3][d++] = '/';
		} else if ( i == 1 && ch == ':' ) {
			v[0][a++] = v[1][b++] = v[2][c++] = ':';
		} else {
			v[0][a++] = v[1][b++] = v[2][c++] = ch;
			if ( i == 0 ) {
				v[3][d++] = '/';
				v[3][d++] = (char)BR_Lower( (unsigned char)ch );
			} else {
				v[3][d++] = ch;
			}
		}
	}
	v[0][a] = v[1][b] = v[2][c] = v[3][d] = 0;
	BR_ReplaceIn( cur, v[2], "<HOME>", 0 );
	BR_ReplaceIn( cur, v[0], "<HOME>", 0 );
	BR_ReplaceIn( cur, v[1], "<HOME>", 0 );
	BR_ReplaceIn( cur, v[3], "<HOME>", 0 );
}

// 6. generic: [X:]<sep>+(Users|Documents and Settings)<sep>+<segment> -> <HOME> (after a drive letter) else
// ...Users<sep><USER>; "OneDrive - <organisation>" -> "OneDrive - <org>"
static void BR_PassUsersDir( const char *in, size_t n, brbuf_t *out ) {
	size_t i = 0;
	while ( i < n ) {
		if ( BR_MatchCI( in + i, n - i, "OneDrive - " ) ) {
			size_t e = i + 11;
			while ( e < n && !BR_IsSep( (unsigned char)in[e] ) && in[e] != '"' && in[e] != '\'' && in[e] != '\r'
					&& in[e] != '\n' ) {
				e++;
			}
			BR_Puts( out, "OneDrive - <org>" );
			i = e;
			continue;
		}
		if ( BR_IsSep( (unsigned char)in[i] ) ) {
			size_t j = i;
			while ( j < n && BR_IsSep( (unsigned char)in[j] ) ) {
				j++;
			}
			size_t kw = 0;
			if ( BR_MatchCI( in + j, n - j, "users" ) ) {
				kw = 5;
			} else if ( BR_MatchCI( in + j, n - j, "documents and settings" ) ) {
				kw = 22;
			}
			if ( kw ) {
				size_t s  = j + kw;
				size_t s2 = s;
				while ( s2 < n && BR_IsSep( (unsigned char)in[s2] ) ) {
					s2++;
				}
				if ( s2 > s && s2 < n ) {
					size_t e = s2;
					while ( e < n && !BR_IsSep( (unsigned char)in[e] ) && in[e] != '"' && in[e] != '\'' && in[e] != '<'
							&& in[e] != '\r' && in[e] != '\n' ) {
						e++;
					}
					if ( e > s2 && !BR_MatchCI( in + s2, e - s2, "<USER>" ) ) {
						if ( out->n >= 2 && out->p[out->n - 1] == ':' && isalpha( (unsigned char)out->p[out->n - 2] )
							 && ( out->n == 2 || !BR_IsWordChar( (unsigned char)out->p[out->n - 3] ) ) ) {
							BR_Truncate( out, out->n - 2 );
							BR_Puts( out, "<HOME>" );
						} else {
							BR_Put( out, in + i, s2 - i );
							BR_Puts( out, "<USER>" );
						}
						i = e;
						continue;
					}
				}
			}
			// no match from this separator: none can match from the rest of the run either (same j), so copy the whole
			// run at once (a hostile server flooding '/' would otherwise cost O(n^2))
			BR_Put( out, in + i, j - i );
			i = j;
			continue;
		}
		BR_Put( out, in + i, 1 );
		i++;
	}
}

// 7. IPv4 (not 127.0.0.1 / 0.0.0.0 / 255.255.255.255) -> <ip>. IPv6 -> <ip6>: a run of hex/':' (plus '.' and a
// '%zone'), not glued to a word, holding a hex digit and either "::" or >= 5 colons (so HH:MM:SS never matches).
static void BR_PassIp( const char *in, size_t n, brbuf_t *out ) {
	size_t i = 0;
	while ( i < n ) {
		int c      = (unsigned char)in[i];
		int prevOk = ( i == 0 ) || !( BR_IsWordChar( (unsigned char)in[i - 1] ) || in[i - 1] == '.' || in[i - 1] == ':' );
		if ( BR_IsDigit( c ) && prevOk ) {
			int    oct[4], k = 0;
			size_t j = i;
			while ( k < 4 ) {
				size_t s = j;
				int    v = 0;
				while ( j < n && BR_IsDigit( (unsigned char)in[j] ) && j - s < 4 ) {
					v = v * 10 + ( in[j] - '0' );
					j++;
				}
				if ( j == s || j - s > 3 || v > 255 ) {
					break;
				}
				oct[k++] = v;
				if ( k < 4 ) {
					if ( j < n && in[j] == '.' ) {
						j++;
					} else {
						break;
					}
				}
			}
			int endOk = ( j >= n ) || !( BR_IsWordChar( (unsigned char)in[j] )
										 || ( in[j] == '.' && j + 1 < n && BR_IsDigit( (unsigned char)in[j + 1] ) ) );
			if ( k == 4 && endOk ) {
				int keep = ( oct[0] == 127 && !oct[1] && !oct[2] && oct[3] == 1 )
						|| ( !oct[0] && !oct[1] && !oct[2] && !oct[3] )
						|| ( oct[0] == 255 && oct[1] == 255 && oct[2] == 255 && oct[3] == 255 );
				if ( keep ) {
					BR_Put( out, in + i, j - i );
				} else {
					BR_Puts( out, "<ip>" );
				}
				i = j;
				continue;
			}
		}
		if ( ( BR_IsHex( c ) || c == ':' ) && prevOk ) {
			size_t j      = i;
			int    colons = 0, hex = 0, dbl = 0;
			while ( j < n && ( BR_IsHex( (unsigned char)in[j] ) || in[j] == ':' || in[j] == '.' ) ) {
				if ( in[j] == ':' ) {
					colons++;
					if ( j + 1 < n && in[j + 1] == ':' ) {
						dbl = 1;
					}
				} else if ( in[j] != '.' ) {
					hex = 1;
				}
				j++;
			}
			while ( j > i && in[j - 1] == '.' ) {
				j--;
			}
			if ( hex && colons >= 2 && ( dbl || colons >= 5 ) ) {
				if ( j < n && in[j] == '%' ) { // zone id
					j++;
					while ( j < n && BR_IsWordChar( (unsigned char)in[j] ) ) {
						j++;
					}
				}
				if ( j >= n || !BR_IsWordChar( (unsigned char)in[j] ) ) {
					BR_Puts( out, "<ip6>" );
					i = j;
					continue;
				}
			}
		}
		BR_Put( out, in + i, 1 );
		i++;
	}
}

// 8. XXXX-XXXX-XXXX-XXXX (upper-case letters/digits, not glued to a word) -> <cdkey>
static void BR_PassKeyShape( const char *in, size_t n, brbuf_t *out ) {
	size_t i = 0;
	while ( i < n ) {
		if ( i + 19 <= n && ( i == 0 || ( !BR_IsWordChar( (unsigned char)in[i - 1] ) && in[i - 1] != '-' ) ) ) {
			int ok = 1;
			for ( int k = 0; k < 19 && ok; k++ ) {
				int c = (unsigned char)in[i + k];
				ok    = ( k % 5 == 4 ) ? c == '-' : ( ( c >= 'A' && c <= 'Z' ) || BR_IsDigit( c ) );
			}
			if ( ok && ( i + 19 == n || ( !BR_IsWordChar( (unsigned char)in[i + 19] ) && in[i + 19] != '-' ) ) ) {
				BR_Puts( out, "<cdkey>" );
				i += 19;
				continue;
			}
		}
		BR_Put( out, in + i, 1 );
		i++;
	}
}

// 0. MOHAA colour codes ("^1password^7 hunter2") would split a keyword from its value or reassemble a secret after the
// fact: they go first
static void BR_PassColors( const char *in, size_t n, brbuf_t *out ) {
	size_t i = 0;
	while ( i < n ) {
		if ( in[i] == '^' && i + 1 < n && BR_IsDigit( (unsigned char)in[i + 1] ) ) {
			i += 2;
			continue;
		}
		BR_Put( out, in + i, 1 );
		i++;
	}
}

// 2b. a bare "\name\<value>" info-string key (outside the ">>>...<<<" blob already replaced): the value is a player name
static void BR_PassInfoName( const char *in, size_t n, brbuf_t *out ) {
	size_t i = 0;
	while ( i < n ) {
		if ( BR_MatchCI( in + i, n - i, "\\name\\" ) ) {
			size_t e = i + 6;
			while ( e < n && in[e] != '\\' && in[e] != '\r' && in[e] != '\n' && in[e] != '"' ) {
				e++;
			}
			BR_Put( out, in + i, 6 );
			if ( e > i + 6 ) {
				BR_Puts( out, "<NAME>" );
			}
			i = e;
			continue;
		}
		BR_Put( out, in + i, 1 );
		i++;
	}
}

typedef void ( *brpass_t )( const char *, size_t, brbuf_t * );

static void BR_Run( brbuf_t *cur, brpass_t f ) {
	brbuf_t t = {0};
	f( BR_P( cur ), cur->n, &t );
	BR_Swap( cur, &t );
	BR_Free( &t );
}

// the whole pipeline; out is appended to
static void BR_Scrub( const char *in, size_t n, const brscrub_t *ctx, brbuf_t *out ) {
	brbuf_t cur = {0};
	BR_Put( &cur, in, n );
	BR_Run( &cur, BR_PassColors );
	BR_Run( &cur, BR_PassWebhook );
	BR_Run( &cur, BR_PassUserinfo );
	BR_Run( &cur, BR_PassInfoName );
	BR_Run( &cur, BR_PassKeyValue );
	BR_Run( &cur, BR_PassHostsAndCodes );
	if ( ctx->cdkey[0] ) {
		BR_ReplaceIn( &cur, ctx->cdkey, "<cdkey>", 0 );
	}
	BR_Run( &cur, BR_PassKeyShape );
	for ( int v = 0; v < BR_ID_VARIANTS; v++ ) {
		BR_PassProfile( &cur, ctx->profile[v] );
	}
	BR_Run( &cur, BR_PassUsersDir );
	for ( int v = 0; v < BR_ID_VARIANTS; v++ ) {
		BR_ReplaceIn( &cur, ctx->user[v], "<USER>", 1 );
		BR_ReplaceIn( &cur, ctx->pc[v], "<PC>", 1 );
	}
	// player names (yours and everyone the server told us about): "Name#12 has entered the battle", chat, kill feed
	for ( int k = 0; k < ctx->nNames && k < BR_MAX_NAMES; k++ ) {
		BR_ReplaceIn( &cur, ctx->names[k], "<NAME>", 1 );
	}
	BR_Run( &cur, BR_PassIp );
	BR_Put( out, BR_P( &cur ), cur.n );
	BR_Free( &cur );
}

/*
settings.cfg rule. WITHHELD (value never read into the report) when the name contains pass/key/rcon/token/secret/
webhook/auth/guid/cdkey/socks/connect/rdv, ends in ip/ip6, is the player name, or is progression/unlock/save state.
The caller then applies the ALLOWLIST (BR_SettingAllowed) to whatever survives.
*/
static int BR_CvarWithheld( const char *name ) {
	static const char *const subs[] = {"pass", "key", "rcon", "token", "secret", "webhook", "auth", "guid", "cdkey",
									   "socks", "connect", "rdv", "hostname"};
	// HZM-MP-BEGIN(mp_report_withhold) - read-only: MP progress cvars are WITHHELD from bug-report settings.cfg
	static const char *const prefixes[] = {"sv_location", "sv_dlurl", "net_mcast", "coop_xp", "coop_sbrank", "coop_chal", "coop_cp", "coop_pend_", "coop_pin",
										   "coop_unlock", "coop_mpcnt_", "coop_mpprogblob", "coop_mprank", "coop_mps_",
										   "coop_mptotal", "coop_mpuw_", "coop_mpa_cos", "coop_mpx_cos", "coop_prestige",
										   "coop_medal", "coop_rank", "coop_career", "coop_servicerec", "g_medal",
										   "g_eogmedal", "g_lastsave", "g_mission", "coop_report", "coop_lastserver"};
	// HZM-MP-END(mp_report_withhold)
	char   w[128];
	size_t l = strlen( name );
	if ( l == 0 || l >= sizeof( w ) ) {
		return 1;
	}
	for ( size_t i = 0; i <= l; i++ ) {
		w[i] = (char)BR_Lower( (unsigned char)name[i] );
	}
	if ( !strcmp( w, "name" ) || !strcmp( w, "cl_playername" ) ) {
		return 1;
	}
	// the substring rules run on the name with every "compass" removed: coop_compassBar is a HUD setting, not a
	// password (the only "pass" in 20+ compass cvars)
	char   w2[128];
	size_t o = 0;
	for ( size_t i = 0; i < l; ) {
		if ( !strncmp( w + i, "compass", 7 ) ) {
			i += 7;
			continue;
		}
		w2[o++] = w[i++];
	}
	w2[o] = 0;
	for ( size_t i = 0; i < sizeof( subs ) / sizeof( subs[0] ); i++ ) {
		if ( strstr( w2, subs[i] ) ) {
			return 1;
		}
	}
	for ( size_t i = 0; i < sizeof( prefixes ) / sizeof( prefixes[0] ); i++ ) {
		if ( !strncmp( w, prefixes[i], strlen( prefixes[i] ) ) ) {
			return 1;
		}
	}
	// coop_uiB/D/N/P<digit>: challenge/pin state (challenges.scr); coop_lo??Lk*: cosmetic unlock locks;
	// g_m1l1..g_t3l1 / g_e1l1..: campaign progress (fgame/gamecvars.cpp)
	if ( !strncmp( w, "coop_ui", 7 ) && w[7] && strchr( "bdnp", w[7] ) && BR_IsDigit( (unsigned char)w[8] ) ) {
		return 1;
	}
	if ( !strncmp( w, "coop_lo", 7 ) && strstr( w + 7, "lk" ) ) {
		return 1;
	}
	if ( w[0] == 'g' && w[1] == '_' && strchr( "met", w[2] ) && BR_IsDigit( (unsigned char)w[3] ) ) {
		return 1;
	}
	if ( ( l >= 2 && !strcmp( w + l - 2, "ip" ) ) || ( l >= 3 && !strcmp( w + l - 3, "ip6" ) ) ) {
		return 1;
	}
	return 0;
}

// ALLOWLIST: engine/renderer/sound/client prefixes, or a coop_* the engine registers (engineRegistered) or a shipped
// cfg seeds (seeded). Everything else - notably every script-created coop_* (progress, cosmetics, loadout picks) -
// stays out.
static int BR_SettingAllowed( const char *name, int engineRegistered, int seeded ) {
	static const char *const pre[] = {"r_", "gl_", "cg_", "cl_", "com_", "s_", "snd_", "vid_", "in_", "j_", "m_",
									  "sv_", "net_", "fs_", "sys_", "ui_", "bot_", "g_", "dm_", "con_", "scr_"};
	static const char *const exact[] = {"version", "developer", "logfile", "sensitivity", "rate", "snaps", "dedicated",
										"timescale", "fraglimit", "timelimit", "mapname", "skill"};
	if ( BR_CvarWithheld( name ) ) {
		return 0;
	}
	if ( !Q_stricmpn( name, "coop_", 5 ) ) {
		return engineRegistered || seeded;
	}
	for ( size_t i = 0; i < sizeof( pre ) / sizeof( pre[0] ); i++ ) {
		if ( !Q_stricmpn( name, pre[i], (int)strlen( pre[i] ) ) ) {
			return 1;
		}
	}
	for ( size_t i = 0; i < sizeof( exact ) / sizeof( exact[0] ); i++ ) {
		if ( !Q_stricmp( name, exact[i] ) ) {
			return 1;
		}
	}
	return 0;
}

// JSON string body (no quotes). Bytes that are not valid UTF-8 are taken as Windows-1252/Latin-1 and re-encoded, so a
// typed accent never makes Discord answer 400 (the 2026-08-14 publish_release.ps1 mojibake).
static void BR_JsonEscape( const char *s, brbuf_t *out ) {
	const unsigned char *p = (const unsigned char *)s;
	while ( *p ) {
		unsigned char c = *p;
		if ( c == '"' || c == '\\' ) {
			char e[2] = {'\\', (char)c};
			BR_Put( out, e, 2 );
			p++;
		} else if ( c == '\n' ) {
			BR_Puts( out, "\\n" );
			p++;
		} else if ( c == '\t' ) {
			BR_Puts( out, "\\t" );
			p++;
		} else if ( c < 0x20 || c == 0x7f ) {
			p++;
		} else if ( c < 0x80 ) {
			BR_Put( out, (const char *)p, 1 );
			p++;
		} else {
			int len = ( c >= 0xf0 && c < 0xf5 ) ? 4 : ( c >= 0xe0 && c < 0xf0 ) ? 3 : ( c >= 0xc2 && c < 0xe0 ) ? 2 : 0;
			int ok  = len > 0;
			for ( int k = 1; ok && k < len; k++ ) {
				ok = ( p[k] & 0xc0 ) == 0x80;
			}
			if ( ok ) {
				BR_Put( out, (const char *)p, (size_t)len );
				p += len;
			} else {
				char u[2] = {(char)( 0xc0 | ( c >> 6 ) ), (char)( 0x80 | ( c & 0x3f ) )};
				BR_Put( out, u, 2 );
				p++;
			}
		}
	}
}

// one-line field: no control bytes, no MOHAA colour codes, no backticks (the field is shown in a code span), capped
static void BR_CleanLine( const char *in, char *out, size_t outsz, size_t max ) {
	size_t o = 0;
	if ( max >= outsz ) {
		max = outsz - 1;
	}
	for ( const unsigned char *p = (const unsigned char *)in; *p && o < max; p++ ) {
		if ( *p == '^' && BR_IsDigit( p[1] ) ) {
			p++;
			continue;
		}
		if ( *p < 0x20 || *p == 0x7f ) {
			out[o++] = ' ';
			continue;
		}
		out[o++] = *p == '`' ? '\'' : (char)*p;
	}
	while ( o > 0 && out[o - 1] == ' ' ) {
		o--;
	}
	out[o] = 0;
}

// player names to scrub: your own `name` and every one the server has told this client about (CS_PLAYERS entries survive
// a disconnect, so people who already left are covered). Colour codes stripped; a "Name#1234" form also adds "Name".
static void BR_AddName( brscrub_t *ctx, const char *raw ) {
	char full[BR_NAME_LEN], n[BR_NAME_LEN];
	BR_CleanLine( raw, full, sizeof( full ), sizeof( full ) - 1 );
	// three spellings: the whole value, the part before '#' (the "Name#1234" the game prints), and the part before the
	// mod's " ,marker" suffix (an armory / class marker like " ,w0o" that can still be on the cvar)
	for ( int pass = 0; pass < 3; pass++ ) {
		Q_strncpyz( n, full, sizeof( n ) );
		if ( pass == 1 || pass == 2 ) {
			char *h = pass == 1 ? strchr( n, '#' ) : strstr( n, " ," );
			if ( !h ) {
				continue;
			}
			*h = 0;
		}
		size_t l = strlen( n );
		while ( l > 0 && n[l - 1] == ' ' ) {
			n[--l] = 0;
		}
		if ( l < 3 ) { // one or two characters would garble the log; the whole-word rule keeps longer ones sane
			continue;
		}
		int dup = 0;
		for ( int k = 0; k < ctx->nNames && !dup; k++ ) {
			dup = !Q_stricmp( ctx->names[k], n );
		}
		if ( !dup && ctx->nNames < BR_MAX_NAMES ) {
			Q_strncpyz( ctx->names[ctx->nNames++], n, BR_NAME_LEN );
		}
	}
}

#ifdef BUGREPORT_SELFTEST
/*
selftest: argv[1] corpus, argv[2] profile, argv[3] user, argv[4] pc, argv[5] cdkey ("-" = none), argv[6] a file of
"<name> <engineRegistered 0|1> <seeded 0|1>" lines. Prints the scrubbed corpus, "=== settings ===" and the names the
allowlist admits, then "=== json ===" and one escaped sample.
*/
int main( int argc, char **argv ) {
	if ( argc < 7 ) {
		return 2;
	}
	_setmode( _fileno( stdout ), _O_BINARY );
	brscrub_t ctx;
	memset( &ctx, 0, sizeof( ctx ) );
	Q_strncpyz( ctx.profile[0], argv[2], sizeof( ctx.profile[0] ) );
	Q_strncpyz( ctx.user[0], argv[3], sizeof( ctx.user[0] ) );
	Q_strncpyz( ctx.pc[0], argv[4], sizeof( ctx.pc[0] ) );
	if ( strcmp( argv[5], "-" ) ) {
		Q_strncpyz( ctx.cdkey, argv[5], sizeof( ctx.cdkey ) );
	}
	if ( argc > 7 ) { // argv[7]: player names as the engine would see them, '|' separated (BR_AddName)
		char  nl[1024];
		char *tok;
		Q_strncpyz( nl, argv[7], sizeof( nl ) );
		for ( tok = strtok( nl, "|" ); tok; tok = strtok( NULL, "|" ) ) {
			BR_AddName( &ctx, tok );
		}
	}
	FILE *f = fopen( argv[1], "rb" );
	if ( !f ) {
		return 3;
	}
	brbuf_t in = {0}, out = {0};
	char    tmp[8192];
	size_t  r;
	while ( ( r = fread( tmp, 1, sizeof( tmp ), f ) ) > 0 ) {
		BR_Put( &in, tmp, r );
	}
	fclose( f );
	BR_Scrub( BR_P( &in ), in.n, &ctx, &out );
	fwrite( BR_P( &out ), 1, out.n, stdout );
	printf( "=== settings ===\n" );
	f = fopen( argv[6], "rb" );
	if ( f ) {
		char line[256], nm[200];
		int  eng, seed;
		while ( fgets( line, sizeof( line ), f ) ) {
			if ( sscanf( line, "%199s %d %d", nm, &eng, &seed ) == 3 && BR_SettingAllowed( nm, eng, seed ) ) {
				printf( "%s\n", nm );
			}
		}
		fclose( f );
	}
	printf( "=== json ===\n" );
	brbuf_t j = {0};
	BR_JsonEscape( "a\"b\\c\nd\te\x01\xe9\xc3\xa9", &j );
	printf( "%s\n", BR_P( &j ) );
	return 0;
}
#else /* !BUGREPORT_SELFTEST */

/*
====================================================================================================================
Engine side
====================================================================================================================
*/

#include "cl_bugreport_uploader.h" // BR_UPLOADER_PS1: generated from uploader.ps1 by apply_engine.py
extern "C" void Com_HzmServerFrameStats( int *out ); // qcommon/common.c: server-frame cost + interval, 2 x 60 s

#define BR_LOG_TAIL       ( 256 * 1024 )
#define BR_PREV_TAIL      ( 128 * 1024 )
#define BR_FATAL_TAIL     ( 4 * 1024 )
#define BR_MSG_MAX        1990
#define BR_PREVIEW_LINES  16
#define BR_PREVIEW_WIDTH  72
#define BR_ATTACH_LINES   8
#define BR_COOLDOWN_SEC   60
#define BR_DRY_COOLDOWN   5
#define BR_SESSION_MAX    5
#define BR_DAY_MAX        20

static cvar_t *br_text, *br_steps, *br_result, *br_dryrun;
static cvar_t *br_incLog, *br_incCfg, *br_incShot, *br_incCrash, *br_incServer, *br_incSys;

static int      s_openMs       = -1; // Sys_Milliseconds when the menu was opened
static time_t   s_openTime     = 0;  // wall clock of the same moment (screenshot freshness)
static int      s_prepMs       = 0;  // when the preview was built
static char     s_fingerprint[1300]; // toggles + texts the preview was built from
static char     s_contentSha[72];    // SHA-256 of every byte the preview shows
static qboolean s_prepared     = qfalse;
static qboolean s_pending      = qfalse;
static int      s_pendingSince = 0;
static int      s_nextPoll     = 0;
static int      s_sessionSends = 0;
static int      s_lastDryMs    = -100000; // dry runs never touch the shared state file
static char     s_stamp[32];
static char     s_dir[MAX_OSPATH];  // <homepath>/<gamedir>/coop_report/current
static char     s_root[MAX_OSPATH]; // <homepath>/<gamedir>/coop_report
static float    s_fpsMsAvg     = 0;
static int      s_fpsSamples   = 0;
static int      s_shotState    = 0; // 1 closing menus, 2 settling, 3 waiting for the file
static int      s_shotDeadline = 0;
static int      s_shotFrame    = 0;
// client frame timing in a map, two 60 s windows (the host "server slow" reports: a listen host's server runs inside
// these frames, so a hitch here is a hitch for every joiner)
typedef struct {
	int frames, sumMs, maxMs, h50, h100;
} brwin_t;
static brwin_t  s_clCur, s_clLast;
static int      s_clWinStart   = 0;
static cvar_t  *br_category, *br_incBinds;
static const char *const s_categories[] = {"Choose one", "Crash / freeze", "Performance / lag / server slow",
										   "Graphics / HUD / crosshair", "Gameplay / AI / objectives", "Weapons / aiming",
										   "Map / mission / stuck", "Sound", "Menus / settings / controls",
										   "Multiplayer / connection", "Other"};
#define BR_NCAT ( (int)( sizeof( s_categories ) / sizeof( s_categories[0] ) ) )
#ifdef _WIN32
static HANDLE   s_proc         = NULL;
#endif

static const char *BR_Str( const char *name ) {
	return Cvar_VariableString( name );
}

static void BR_Result( const char *code ) {
	Cvar_Set( "coop_reportResult", code );
}

static void BR_PreviewClear( void ) {
	char n[32];
	for ( int i = 1; i <= BR_PREVIEW_LINES; i++ ) {
		Com_sprintf( n, sizeof( n ), "coop_reportPv%02d", i );
		Cvar_Set( n, "" );
	}
	for ( int i = 1; i <= BR_ATTACH_LINES; i++ ) {
		Com_sprintf( n, sizeof( n ), "coop_reportAt%d", i );
		Cvar_Set( n, "" );
	}
	Cvar_Set( "coop_reportPvTotal", "" );
}

static void BR_ShotPath( char *out, size_t sz ) {
	Q_strncpyz( out, FS_BuildOSPath( Cvar_VariableString( "fs_homepath" ), FS_GetCurrentGameDir(),
									 "screenshots/coop_report_shot.jpg" ), (int)sz );
}

#ifdef _WIN32

static void BR_EnvBoth( const wchar_t *name, char ansi[], char utf8[], size_t sz ) {
	wchar_t w[512] = L"";
	ansi[0] = utf8[0] = 0;
	if ( !GetEnvironmentVariableW( name, w, 512 ) ) {
		return;
	}
	WideCharToMultiByte( CP_ACP, 0, w, -1, ansi, (int)sz, NULL, NULL );
	WideCharToMultiByte( CP_UTF8, 0, w, -1, utf8, (int)sz, NULL, NULL );
}

static void BR_ScrubCtx( brscrub_t *ctx ) {
	memset( ctx, 0, sizeof( *ctx ) );
	BR_EnvBoth( L"USERPROFILE", ctx->profile[0], ctx->profile[1], sizeof( ctx->profile[0] ) );
	BR_EnvBoth( L"USERNAME", ctx->user[0], ctx->user[1], sizeof( ctx->user[0] ) );
	BR_EnvBoth( L"COMPUTERNAME", ctx->pc[0], ctx->pc[1], sizeof( ctx->pc[0] ) );
	// the cd key the engine holds in memory (qcommon/common.c): used ONLY to redact it, never written anywhere
	int blank = 1;
	for ( int i = 0; i < 16 && cl_cdkey[i]; i++ ) {
		if ( cl_cdkey[i] != ' ' ) {
			blank = 0;
		}
	}
	if ( !blank && cl_cdkey[0] ) {
		memcpy( ctx->cdkey, cl_cdkey, 16 );
		ctx->cdkey[16] = 0;
	}
}

static void BR_ScrubStr( const char *in, const brscrub_t *ctx, char *out, size_t outsz ) {
	brbuf_t b = {0};
	BR_Scrub( in, strlen( in ), ctx, &b );
	Q_strncpyz( out, BR_P( &b ), (int)outsz );
	BR_Free( &b );
}

static qboolean BR_WriteFile( const char *path, const char *data, size_t n ) {
	FILE *f = fopen( path, "wb" );
	if ( !f ) {
		return qfalse;
	}
	size_t w = n ? fwrite( data, 1, n, f ) : 0;
	fclose( f );
	return w == n ? qtrue : qfalse;
}

static time_t BR_FileTimeToUnix( FILETIME ft ) {
	ULARGE_INTEGER t;
	t.LowPart  = ft.dwLowDateTime;
	t.HighPart = ft.dwHighDateTime;
	return (time_t)( ( t.QuadPart - 116444736000000000ULL ) / 10000000ULL );
}

// the last maxBytes of a file (starting at a line boundary when cut), shared-read so an open log is fine
static qboolean BR_ReadTail( const char *path, size_t maxBytes, brbuf_t *out, time_t *mtime, long long *fullSize ) {
	WIN32_FILE_ATTRIBUTE_DATA fa;
	if ( !GetFileAttributesExA( path, GetFileExInfoStandard, &fa ) ) {
		return qfalse;
	}
	unsigned long long sz = ( (unsigned long long)fa.nFileSizeHigh << 32 ) | fa.nFileSizeLow;
	if ( fullSize ) {
		*fullSize = (long long)sz;
	}
	if ( mtime ) {
		*mtime = BR_FileTimeToUnix( fa.ftLastWriteTime );
	}
	HANDLE h = CreateFileA( path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
							OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL );
	if ( h == INVALID_HANDLE_VALUE ) {
		return qfalse;
	}
	unsigned long long start = sz > maxBytes ? sz - maxBytes : 0;
	LARGE_INTEGER      li;
	li.QuadPart = (LONGLONG)start;
	SetFilePointerEx( h, li, NULL, FILE_BEGIN );
	char   tmp[65536];
	DWORD  r;
	size_t got = 0;
	while ( got < maxBytes && ReadFile( h, tmp, sizeof( tmp ), &r, NULL ) && r > 0 ) {
		size_t take = r;
		if ( got + take > maxBytes ) {
			take = maxBytes - got;
		}
		BR_Put( out, tmp, take );
		got += take;
	}
	CloseHandle( h );
	if ( start > 0 && out->p ) {
		char *nl = (char *)memchr( out->p, '\n', out->n );
		if ( nl ) {
			size_t skip = (size_t)( nl + 1 - out->p );
			memmove( out->p, out->p + skip, out->n - skip );
			BR_Truncate( out, out->n - skip );
		}
	}
	return qtrue;
}

static long long BR_FileSize( const char *path, time_t *mtime ) {
	WIN32_FILE_ATTRIBUTE_DATA fa;
	if ( !GetFileAttributesExA( path, GetFileExInfoStandard, &fa ) ) {
		return -1;
	}
	if ( mtime ) {
		*mtime = BR_FileTimeToUnix( fa.ftLastWriteTime );
	}
	return (long long)( ( (unsigned long long)fa.nFileSizeHigh << 32 ) | fa.nFileSizeLow );
}

static void BR_DeleteFilesIn( const char *dir ) {
	char             pat[MAX_OSPATH];
	WIN32_FIND_DATAA fd;
	Com_sprintf( pat, sizeof( pat ), "%s\\*", dir );
	HANDLE h = FindFirstFileA( pat, &fd );
	if ( h == INVALID_HANDLE_VALUE ) {
		return;
	}
	do {
		if ( !( fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ) ) {
			char p[MAX_OSPATH];
			Com_sprintf( p, sizeof( p ), "%s\\%s", dir, fd.cFileName );
			DeleteFileA( p );
		}
	} while ( FindNextFileA( h, &fd ) );
	FindClose( h );
}

// incremental CNG hash
typedef struct {
	BCRYPT_ALG_HANDLE  a;
	BCRYPT_HASH_HANDLE h;
	DWORD              len;
} brhash_t;

static qboolean BR_HashBegin( brhash_t *x, LPCWSTR alg ) {
	DWORD cb = 0;
	memset( x, 0, sizeof( *x ) );
	if ( BCryptOpenAlgorithmProvider( &x->a, alg, NULL, 0 ) != 0 ) {
		return qfalse;
	}
	if ( BCryptGetProperty( x->a, BCRYPT_HASH_LENGTH, (PUCHAR)&x->len, sizeof( x->len ), &cb, 0 ) != 0
		 || x->len > 64 || BCryptCreateHash( x->a, &x->h, NULL, 0, NULL, 0, 0 ) != 0 ) {
		BCryptCloseAlgorithmProvider( x->a, 0 );
		x->a = NULL;
		return qfalse;
	}
	return qtrue;
}

static void BR_HashAdd( brhash_t *x, const void *p, size_t n ) {
	if ( x->h && n ) {
		BCryptHashData( x->h, (PUCHAR)p, (ULONG)n, 0 );
	}
}

static void BR_HashAddFile( brhash_t *x, const char *path ) {
	FILE *f = fopen( path, "rb" );
	if ( !f ) {
		return;
	}
	unsigned char buf[65536];
	size_t        r;
	while ( ( r = fread( buf, 1, sizeof( buf ), f ) ) > 0 ) {
		BR_HashAdd( x, buf, r );
	}
	fclose( f );
}

static void BR_HashEnd( brhash_t *x, char *hex, size_t hexsz, int upper ) {
	unsigned char dig[64];
	hex[0] = 0;
	if ( !x->h ) {
		return;
	}
	if ( BCryptFinishHash( x->h, dig, x->len, 0 ) == 0 && hexsz > x->len * 2 ) {
		for ( DWORD i = 0; i < x->len; i++ ) {
			Com_sprintf( hex + i * 2, 3, upper ? "%02X" : "%02x", dig[i] );
		}
	}
	BCryptDestroyHash( x->h );
	BCryptCloseAlgorithmProvider( x->a, 0 );
	x->h = NULL;
	x->a = NULL;
}

static void BR_Md5File( const char *path, char *hex, size_t hexsz ) {
	brhash_t x;
	hex[0] = 0;
	if ( BR_HashBegin( &x, BCRYPT_MD5_ALGORITHM ) ) {
		BR_HashAddFile( &x, path );
		BR_HashEnd( &x, hex, hexsz, 0 );
	}
}

static int BR_CmpStr( const void *a, const void *b ) {
	return strcmp( *(const char *const *)a, *(const char *const *)b );
}

/*
The preview fingerprint: SHA-256 over, for each file in files\ in ordinal name order, name + NUL + bytes, then
"payload.json" + NUL + bytes, then "zipname.txt" + NUL + bytes. uploader.ps1 recomputes the same value
(HZMREP_SHA) and refuses to post if it differs, so nothing can change between the review page and Discord.
*/
static void BR_ContentSha( char *hex, size_t hexsz ) {
	brhash_t x;
	hex[0] = 0;
	if ( !BR_HashBegin( &x, BCRYPT_SHA256_ALGORITHM ) ) {
		return;
	}
	char             pat[MAX_OSPATH], path[MAX_OSPATH];
	char             names[32][MAX_PATH];
	const char      *order[32];
	int              n = 0;
	WIN32_FIND_DATAA fd;
	Com_sprintf( pat, sizeof( pat ), "%s\\files\\*", s_dir );
	HANDLE h = FindFirstFileA( pat, &fd );
	if ( h != INVALID_HANDLE_VALUE ) {
		do {
			if ( !( fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ) && n < 32 ) {
				Q_strncpyz( names[n], fd.cFileName, MAX_PATH );
				order[n] = names[n];
				n++;
			}
		} while ( FindNextFileA( h, &fd ) );
		FindClose( h );
	}
	qsort( order, n, sizeof( order[0] ), BR_CmpStr );
	for ( int i = 0; i < n; i++ ) {
		BR_HashAdd( &x, order[i], strlen( order[i] ) + 1 );
		Com_sprintf( path, sizeof( path ), "%s\\files\\%s", s_dir, order[i] );
		BR_HashAddFile( &x, path );
	}
	const char *tail[] = {"payload.json", "zipname.txt"};
	for ( int i = 0; i < 2; i++ ) {
		BR_HashAdd( &x, tail[i], strlen( tail[i] ) + 1 );
		Com_sprintf( path, sizeof( path ), "%s\\%s", s_dir, tail[i] );
		BR_HashAddFile( &x, path );
	}
	BR_HashEnd( &x, hex, hexsz, 0 );
}

// the anonymous #tag: first 6 upper-case hex of SHA-256("MOHCoopReporter|<PC>|<USER>") in UTF-8 - byte-identical to
// installer/report_problem.ps1 (bug-1799), so one player's in-game and desktop reports group together
static void BR_Tag( char *tag, size_t sz ) {
	wchar_t pcw[128] = L"", uw[128] = L"", src[300];
	GetEnvironmentVariableW( L"COMPUTERNAME", pcw, 128 );
	GetEnvironmentVariableW( L"USERNAME", uw, 128 );
	_snwprintf( src, 300, L"MOHCoopReporter|%s|%s", pcw, uw );
	src[299] = 0;
	char     utf8[1024], hex[130];
	int      n = WideCharToMultiByte( CP_UTF8, 0, src, -1, utf8, sizeof( utf8 ), NULL, NULL );
	brhash_t x;
	hex[0] = 0;
	if ( BR_HashBegin( &x, BCRYPT_SHA256_ALGORITHM ) ) {
		BR_HashAdd( &x, utf8, n > 0 ? (size_t)( n - 1 ) : 0 );
		BR_HashEnd( &x, hex, sizeof( hex ), 1 );
	}
	Q_strncpyz( tag, hex[0] ? hex : "000000", (int)( sz < 7 ? sz : 7 ) );
}

static void BR_RegStr( const char *key, const char *val, char *out, DWORD sz ) {
	DWORD cb = sz;
	out[0]   = 0;
	if ( RegGetValueA( HKEY_LOCAL_MACHINE, key, val, RRF_RT_REG_SZ, NULL, out, &cb ) != ERROR_SUCCESS ) {
		out[0] = 0;
	}
}

static void BR_OsString( char *out, size_t sz ) {
	const char *k = "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
	char        prod[128], disp[64], build[32];
	BR_RegStr( k, "ProductName", prod, sizeof( prod ) );
	BR_RegStr( k, "DisplayVersion", disp, sizeof( disp ) );
	BR_RegStr( k, "CurrentBuildNumber", build, sizeof( build ) );
	if ( atoi( build ) >= 22000 && !strncmp( prod, "Windows 10", 10 ) ) { // the registry says 10 on 11
		prod[9] = '1';
	}
	Com_sprintf( out, (int)sz, "%s%s%s (build %s)", prod[0] ? prod : "Windows", disp[0] ? " " : "", disp,
				 build[0] ? build : "?" );
}

static void BR_CpuString( char *out, size_t sz ) {
	BR_RegStr( "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", "ProcessorNameString", out, (DWORD)sz );
	char *s = out;
	while ( *s == ' ' ) {
		s++;
	}
	memmove( out, s, strlen( s ) + 1 );
	if ( !out[0] ) {
		Q_strncpyz( out, "unknown", (int)sz );
	}
}

static unsigned long long BR_RamMb( void ) {
	MEMORYSTATUSEX ms;
	ms.dwLength = sizeof( ms );
	return GlobalMemoryStatusEx( &ms ) ? ms.ullTotalPhys / ( 1024ULL * 1024ULL ) : 0;
}

static void BR_ExeDir( char *out, size_t sz ) {
	GetModuleFileNameA( NULL, out, (DWORD)sz );
	char *s = strrchr( out, '\\' );
	if ( s ) {
		*s = 0;
	}
}

// installed_manifest.json {"version": "x"} next to the exe (updater installs); "" when absent
static void BR_ManifestVersion( char *out, size_t sz ) {
	char dir[MAX_OSPATH], path[MAX_OSPATH];
	out[0] = 0;
	BR_ExeDir( dir, sizeof( dir ) );
	Com_sprintf( path, sizeof( path ), "%s\\installed_manifest.json", dir );
	brbuf_t b = {0};
	if ( BR_ReadTail( path, 1024 * 1024, &b, NULL, NULL ) && b.p ) {
		const char *k = strstr( b.p, "\"version\"" );
		k             = k ? strchr( k + 9, '"' ) : NULL;
		const char *e = k ? strchr( k + 1, '"' ) : NULL;
		if ( e && e - k - 1 > 0 && e - k - 1 < 40 ) {
			char tmp[48];
			memcpy( tmp, k + 1, e - k - 1 );
			tmp[e - k - 1] = 0;
			BR_CleanLine( tmp, out, sz, 40 );
		}
	}
	BR_Free( &b );
}

// ^https://(ptb\.|canary\.)?discord(app)?\.com/api/webhooks/[0-9]+/[A-Za-z0-9_-]+$
static qboolean BR_IsDiscordWebhook( const char *u ) {
	const char *hosts[] = {"https://discord.com/api/webhooks/", "https://discordapp.com/api/webhooks/",
						   "https://ptb.discord.com/api/webhooks/", "https://canary.discord.com/api/webhooks/",
						   "https://ptb.discordapp.com/api/webhooks/", "https://canary.discordapp.com/api/webhooks/"};
	const char *p       = NULL;
	for ( size_t i = 0; i < sizeof( hosts ) / sizeof( hosts[0] ); i++ ) {
		if ( !strncmp( u, hosts[i], strlen( hosts[i] ) ) ) {
			p = u + strlen( hosts[i] );
			break;
		}
	}
	if ( !p || !BR_IsDigit( (unsigned char)*p ) ) {
		return qfalse;
	}
	while ( BR_IsDigit( (unsigned char)*p ) ) {
		p++;
	}
	if ( *p++ != '/' || !*p ) {
		return qfalse;
	}
	for ( ; *p; p++ ) {
		if ( !( isalnum( (unsigned char)*p ) || *p == '_' || *p == '-' ) ) {
			return qfalse;
		}
	}
	return qtrue;
}

// ^http://127\.0\.0\.1:[0-9]{1,5}/[A-Za-z0-9/_-]*$
static qboolean BR_IsLoopbackUrl( const char *u ) {
	const char *pre = "http://127.0.0.1:";
	if ( strncmp( u, pre, strlen( pre ) ) ) {
		return qfalse;
	}
	const char *p = u + strlen( pre );
	int         d = 0;
	while ( BR_IsDigit( (unsigned char)*p ) ) {
		p++;
		d++;
	}
	if ( d < 1 || d > 5 || *p != '/' ) {
		return qfalse;
	}
	for ( ; *p; p++ ) {
		if ( !( isalnum( (unsigned char)*p ) || *p == '/' || *p == '_' || *p == '-' ) ) {
			return qfalse;
		}
	}
	return qtrue;
}

/*
The webhook, from FILES only - a server can rewrite any coop_* cvar on an older exe (and could inject into the old
PowerShell command line through it), so the cvar is never trusted for a real send. Sources in order: updater.ini
"ReportWebhook=" next to the exe (updater installs), then the loose <homepath>/<gamedir>/coop_reportwebhook.cfg
the updater writes (`seta coop_reportWebhook "<url>"`). Never logged; the caller wipes the buffer.
*/
static const char *BR_FileWebhook( char *out, size_t sz ) {
	char dir[MAX_OSPATH], path[MAX_OSPATH];
	out[0] = 0;
	BR_ExeDir( dir, sizeof( dir ) );
	Com_sprintf( path, sizeof( path ), "%s\\updater.ini", dir );
	for ( int src = 0; src < 2; src++ ) {
		if ( src == 1 ) {
			Q_strncpyz( path, FS_BuildOSPath( Cvar_VariableString( "fs_homepath" ), FS_GetCurrentGameDir(),
											  "coop_reportwebhook.cfg" ), sizeof( path ) );
		}
		brbuf_t b = {0};
		if ( BR_ReadTail( path, 64 * 1024, &b, NULL, NULL ) && b.p ) {
			for ( char *line = strtok( b.p, "\r\n" ); line; line = strtok( NULL, "\r\n" ) ) {
				while ( *line == ' ' || *line == '\t' ) {
					line++;
				}
				char *v = NULL;
				if ( src == 0 && !Q_stricmpn( line, "ReportWebhook=", 14 ) ) {
					v = line + 14;
				} else if ( src == 1 && !Q_stricmpn( line, "seta coop_reportWebhook ", 24 ) ) {
					v = line + 24;
				} else if ( src == 1 && !Q_stricmpn( line, "set coop_reportWebhook ", 23 ) ) {
					v = line + 23;
				}
				if ( !v ) {
					continue;
				}
				while ( *v == ' ' || *v == '"' ) {
					v++;
				}
				Q_strncpyz( out, v, (int)sz );
				for ( int i = (int)strlen( out ) - 1; i >= 0 && ( out[i] == ' ' || out[i] == '"' || out[i] == '\t' ); i-- ) {
					out[i] = 0;
				}
				break;
			}
			SecureZeroMemory( b.p, b.n );
		}
		BR_Free( &b );
		if ( BR_IsDiscordWebhook( out ) ) {
			return src == 0 ? "updater.ini" : "coop_reportwebhook.cfg";
		}
		SecureZeroMemory( out, sz );
	}
	return NULL;
}

/*
Minidump (WER LocalDumps) METADATA only: exception code/address, the module it is in, known overlay/capture DLLs and
the non-Microsoft modules. The dump itself is never attached from in-game: with this installer's DumpType it holds
every module's globals (the in-memory cd key) and the process environment.
*/
static void BR_DumpSummary( const char *path, const brscrub_t *ctx, brbuf_t *out ) {
	FILE *f = fopen( path, "rb" );
	if ( !f ) {
		return;
	}
	unsigned int hdr[4];
	if ( fread( hdr, 4, 4, f ) != 4 || hdr[0] != 0x504d444d ) { // "MDMP"
		fclose( f );
		BR_Puts( out, "  (not a minidump)\r\n" );
		return;
	}
	unsigned int nStreams = hdr[2], dirRva = hdr[3], modRva = 0, excRva = 0;
	for ( unsigned int i = 0; i < nStreams && i < 64; i++ ) {
		unsigned int e[3];
		fseek( f, (long)( dirRva + i * 12 ), SEEK_SET );
		if ( fread( e, 4, 3, f ) != 3 ) {
			break;
		}
		if ( e[0] == 4 ) {
			modRva = e[2];
		} else if ( e[0] == 6 ) {
			excRva = e[2];
		}
	}
	unsigned int       code = 0;
	unsigned long long addr = 0;
	if ( excRva ) {
		fseek( f, (long)( excRva + 8 ), SEEK_SET );
		fread( &code, 4, 1, f );
		fseek( f, (long)( excRva + 24 ), SEEK_SET );
		fread( &addr, 8, 1, f );
		BR_Printf( out, "  exception 0x%08X at 0x%llX\r\n", code, addr );
	}
	static const char *const overlays[] = {"nvspcap", "nvsp", "overlay", "gameoverlayrenderer", "discordhook", "rtss",
										   "rivatuner", "obs-", "obs64", "fraps", "xsplit", "easyanticheat", "beclient",
										   "battleye", "nahimic", "parsec", "medal", "outplayed", "afterburner",
										   "reshade", "specialk", "d3dhook", "gfsdk"};
	if ( modRva ) {
		unsigned int nMod = 0;
		fseek( f, (long)modRva, SEEK_SET );
		fread( &nMod, 4, 1, f );
		brbuf_t nonMs = {0}, ovl = {0};
		for ( unsigned int m = 0; m < nMod && m < 1024; m++ ) {
			unsigned char mod[108];
			fseek( f, (long)( modRva + 4 + m * 108 ), SEEK_SET );
			if ( fread( mod, 1, 108, f ) != 108 ) {
				break;
			}
			unsigned long long base;
			unsigned int       size, nameRva, nameLen = 0;
			memcpy( &base, mod, 8 );
			memcpy( &size, mod + 8, 4 );
			memcpy( &nameRva, mod + 20, 4 );
			wchar_t wname[520];
			fseek( f, (long)nameRva, SEEK_SET );
			fread( &nameLen, 4, 1, f );
			if ( nameLen == 0 || nameLen >= sizeof( wname ) - 2 || fread( wname, 1, nameLen, f ) != nameLen ) {
				continue;
			}
			wname[nameLen / 2] = 0;
			char full[1100], lc[1100];
			full[0] = 0; // WideCharToMultiByte leaves it untouched when it fails (a long non-ASCII module name)
			if ( !WideCharToMultiByte( CP_UTF8, 0, wname, -1, full, sizeof( full ) - 1, NULL, NULL ) ) {
				full[0] = 0;
			}
			full[sizeof( full ) - 1] = 0;
			if ( !full[0] ) {
				continue;
			}
			const char *leaf = strrchr( full, '\\' );
			leaf             = leaf ? leaf + 1 : full;
			if ( addr >= base && addr < base + size ) {
				BR_Printf( out, "  faulting module: %s+0x%llX\r\n", leaf, addr - base );
			}
			Q_strncpyz( lc, leaf, sizeof( lc ) );
			Q_strlwr( lc );
			for ( size_t k = 0; k < sizeof( overlays ) / sizeof( overlays[0] ); k++ ) {
				if ( strstr( lc, overlays[k] ) ) {
					BR_Printf( &ovl, "   >> %s\r\n", leaf );
					break;
				}
			}
			Q_strncpyz( lc, full, sizeof( lc ) );
			Q_strlwr( lc );
			if ( !strstr( lc, "\\windows\\" ) ) {
				BR_Printf( &nonMs, "   %s\r\n", full );
			}
		}
		if ( ovl.n ) {
			BR_Puts( out, "  !! OVERLAY / CAPTURE / INJECTED SOFTWARE loaded at crash (a frequent crash cause):\r\n" );
			BR_Put( out, ovl.p, ovl.n );
		}
		if ( nonMs.n ) {
			BR_Puts( out, "  non-Microsoft modules at crash:\r\n" );
			BR_Scrub( nonMs.p, nonMs.n, ctx, out );
		}
		BR_Free( &nonMs );
		BR_Free( &ovl );
	}
	fclose( f );
}

// newest %LOCALAPPDATA%\CrashDumps\openmohaa.exe.*.dmp modified within 48 h
static qboolean BR_FindDump( char *path, size_t sz, time_t *when, long long *size ) {
	char dir[MAX_OSPATH], pat[MAX_OSPATH];
	if ( !GetEnvironmentVariableA( "LOCALAPPDATA", dir, sizeof( dir ) ) ) {
		return qfalse;
	}
	Com_sprintf( pat, sizeof( pat ), "%s\\CrashDumps\\openmohaa.exe.*.dmp", dir );
	WIN32_FIND_DATAA fd;
	HANDLE           h = FindFirstFileA( pat, &fd );
	if ( h == INVALID_HANDLE_VALUE ) {
		return qfalse;
	}
	ULONGLONG best = 0;
	do {
		ULARGE_INTEGER t;
		t.LowPart  = fd.ftLastWriteTime.dwLowDateTime;
		t.HighPart = fd.ftLastWriteTime.dwHighDateTime;
		if ( t.QuadPart > best ) {
			best = t.QuadPart;
			Com_sprintf( path, (int)sz, "%s\\CrashDumps\\%s", dir, fd.cFileName );
			*size = (long long)( ( (unsigned long long)fd.nFileSizeHigh << 32 ) | fd.nFileSizeLow );
		}
	} while ( FindNextFileA( h, &fd ) );
	FindClose( h );
	*when = (time_t)( ( best - 116444736000000000ULL ) / 10000000ULL );
	return ( time( NULL ) - *when ) < 48 * 3600 ? qtrue : qfalse;
}

#endif /* _WIN32 */

/*
====================================================================================================================
Report content
====================================================================================================================
*/
typedef struct {
	char tag[8];
	char build[48];
	char modver[48];
	char engine[160];
	char game[48];
	char state[16];
	char role[32];
	char map[72];
	char mapTitle[72];
	char lastMap[72];
	char mode[96];
	char players[48];
	char server[72];
	char uptime[32];
	char onMap[32];
	char fps[48];
	char renderer[16];
	char glVendor[96];
	char glRenderer[160];
	char glVersion[160];
	char display[64];
	char msaa[24];
	char os[160];
	char cpu[128];
	unsigned long long ramMb;
	int  scriptErrors;
} brinfo_t;

static const char *BR_ServerInfo( const char *key ) {
	if ( clc.state < CA_CONNECTED || !cl.gameState.stringOffsets[CS_SERVERINFO] ) {
		return "";
	}
	return Info_ValueForKey( cl.gameState.stringData + cl.gameState.stringOffsets[CS_SERVERINFO], key );
}

static const char *BR_ConfigString( int idx ) {
	if ( idx < 0 || idx >= MAX_CONFIGSTRINGS || !cl.gameState.stringOffsets[idx] ) {
		return "";
	}
	return cl.gameState.stringData + cl.gameState.stringOffsets[idx];
}

static void BR_ScrubNames( brscrub_t *ctx ) {
	ctx->nNames = 0;
	BR_AddName( ctx, Cvar_VariableString( "name" ) );
	for ( int i = 0; i < MAX_CLIENTS; i++ ) {
		const char *cs = BR_ConfigString( CS_PLAYERS + i );
		if ( cs[0] ) {
			BR_AddName( ctx, Info_ValueForKey( cs, "name" ) );
		}
	}
	if ( com_sv_running && com_sv_running->integer && svs.clients && sv_maxclients ) {
		for ( int i = 0; i < sv_maxclients->integer && i < MAX_CLIENTS; i++ ) {
			if ( svs.clients[i].state >= CS_CONNECTED && svs.clients[i].name[0] ) {
				BR_AddName( ctx, svs.clients[i].name );
			}
		}
	}
}

// a server-derived one-line field: clean it, then run it through the scrubber (a hostname can hold an IP or a name)
static void BR_CleanScrub( const char *in, const brscrub_t *ctx, char *out, size_t outsz, size_t max ) {
	char tmp[512], big[1024];
	BR_CleanLine( in, tmp, sizeof( tmp ), sizeof( tmp ) - 1 ); // control bytes and colour codes out FIRST
	BR_ScrubStr( tmp, ctx, big, sizeof( big ) );               // then scrub the whole value...
	BR_CleanLine( big, out, outsz, max );                      // ...and only then cap it (a cut must not split a secret)
}

static void BR_Duration( int sec, char *out, size_t sz ) {
	if ( sec < 0 ) {
		Q_strncpyz( out, "?", (int)sz );
	} else if ( sec >= 3600 ) {
		Com_sprintf( out, (int)sz, "%dh%02dm", sec / 3600, ( sec / 60 ) % 60 );
	} else if ( sec >= 60 ) {
		Com_sprintf( out, (int)sz, "%dm%02ds", sec / 60, sec % 60 );
	} else {
		Com_sprintf( out, (int)sz, "%ds", sec );
	}
}

static void BR_Collect( brinfo_t *in, const brscrub_t *ctx ) {
	memset( in, 0, sizeof( *in ) );
#ifdef _WIN32
	BR_Tag( in->tag, sizeof( in->tag ) );
	BR_ManifestVersion( in->build, sizeof( in->build ) );
	BR_OsString( in->os, sizeof( in->os ) );
	BR_CpuString( in->cpu, sizeof( in->cpu ) );
	in->ramMb = BR_RamMb();
#endif
	if ( !in->build[0] ) {
		Q_strncpyz( in->build, "unknown (no installed_manifest.json)", sizeof( in->build ) );
	}
	BR_CleanScrub( BR_Str( "coop_modVersion" ), ctx, in->modver, sizeof( in->modver ), 40 );
	if ( !in->modver[0] ) {
		Q_strncpyz( in->modver, "unknown", sizeof( in->modver ) );
	}
	BR_CleanLine( BR_Str( "version" ), in->engine, sizeof( in->engine ), 150 );
	int tg = Cvar_VariableIntegerValue( "com_target_game" );
	Com_sprintf( in->game, sizeof( in->game ), "%s (com_target_game %d)",
				 tg == 0 ? "AA" : tg == 1 ? "SH" : tg == 2 ? "BT" : "?", tg );

	qboolean inMap  = clc.state == CA_ACTIVE ? qtrue : qfalse;
	qboolean isHost = ( com_sv_running && com_sv_running->integer ) ? qtrue : qfalse;
	Q_strncpyz( in->state, inMap ? "in a map" : "main menu", sizeof( in->state ) );
	Q_strncpyz( in->role, clc.state < CA_CONNECTED ? "not connected" : isHost ? "host" : "joiner", sizeof( in->role ) );
	BR_CleanScrub( BR_Str( "mapname" ), ctx, in->lastMap, sizeof( in->lastMap ), 64 );
	if ( inMap ) {
		BR_CleanScrub( cl.mapname, ctx, in->map, sizeof( in->map ), 64 );
		BR_CleanScrub( BR_ConfigString( CS_MESSAGE ), ctx, in->mapTitle, sizeof( in->mapTitle ), 64 );
		int  gt = atoi( isHost ? BR_Str( "g_gametype" ) : BR_ServerInfo( "g_gametype" ) );
		// HZM-MP-BEGIN(mp_report_mode) - read-only: says 'MP session / mode' in a bug report header
		int  mp = Cvar_VariableIntegerValue( "coop_mp_session" );
		int  hc = atoi( BR_ServerInfo( "g_coopHardcore" ) );
		char mode[96];
		if ( mp ) {
			char mm[40] = "";
			if ( isHost ) {
				BR_CleanLine( BR_Str( "coop_mpMode" ), mm, sizeof( mm ), 30 );
			}
		// HZM-MP-END(mp_report_mode)
			qboolean named = ( mm[0] && Q_stricmp( mm, "none" ) ) ? qtrue : qfalse;
			Com_sprintf( mode, sizeof( mode ), "MP%s%s", named ? " " : "", named ? mm : "" );
		} else if ( gt >= 2 ) {
			Q_strncpyz( mode, "coop", sizeof( mode ) );
		} else if ( gt == 0 ) {
			Q_strncpyz( mode, "single player", sizeof( mode ) );
		} else {
			Com_sprintf( mode, sizeof( mode ), "gametype %d", gt );
		}
		if ( hc ) {
			Q_strcat( mode, sizeof( mode ), ", hardcore" );
		}
		if ( isHost && !mp ) {
			int lms = Cvar_VariableIntegerValue( "coop_lmsLives" );
			if ( lms > 0 ) {
				Q_strcat( mode, sizeof( mode ), va( ", LMS %d", lms ) );
			}
			if ( Cvar_VariableIntegerValue( "coop_dbno" ) ) {
				Q_strcat( mode, sizeof( mode ), ", DBNO" );
			}
		}
		Com_sprintf( in->mode, sizeof( in->mode ), "%s (g_gametype %d)", mode, gt );

		// players: a host counts its real client slots (CS_PLAYERS entries survive a disconnect - the clear in
		// G_ClientDisconnect is #if 0'd); a joiner only has the configstrings, so its count is approximate
		if ( isHost && sv_maxclients ) {
			int humans = 0, bots = 0;
			for ( int i = 0; i < sv_maxclients->integer; i++ ) {
				if ( svs.clients && svs.clients[i].state >= CS_CONNECTED ) {
					if ( svs.clients[i].netchan.remoteAddress.type == NA_BOT ) {
						bots++;
					} else {
						humans++;
					}
				}
			}
			Com_sprintf( in->players, sizeof( in->players ), "%d players + %d bots / %d", humans, bots,
						 sv_maxclients->integer );
		} else {
			int n = 0;
			for ( int i = 0; i < MAX_CLIENTS; i++ ) {
				if ( BR_ConfigString( CS_PLAYERS + i )[0] ) {
					n++;
				}
			}
			const char *mx = BR_ServerInfo( "sv_maxclients" );
			Com_sprintf( in->players, sizeof( in->players ), "~%d/%s players", n, mx[0] ? mx : "?" );
		}
		if ( br_incServer->integer ) {
			BR_CleanScrub( BR_ServerInfo( "sv_hostname" ), ctx, in->server, sizeof( in->server ), 64 );
		}
		int lst = atoi( BR_ConfigString( CS_LEVEL_START_TIME ) );
		int on  = ( cl.serverTime - lst ) / 1000;
		BR_Duration( ( lst > 0 && on >= 0 && on < 7 * 86400 ) ? on : -1, in->onMap, sizeof( in->onMap ) );
	}
	BR_Duration( Sys_Milliseconds() / 1000, in->uptime, sizeof( in->uptime ) );
	if ( s_fpsSamples >= 30 && s_fpsMsAvg > 0.01f ) {
		Com_sprintf( in->fps, sizeof( in->fps ), "%d fps", (int)( 1000.0f / s_fpsMsAvg + 0.5f ) );
	} else {
		Q_strncpyz( in->fps, "fps n/a", sizeof( in->fps ) );
	}

	const char *rn = BR_Str( "cl_renderer" );
	Q_strncpyz( in->renderer, !Q_stricmp( rn, "opengl2" ) ? "gl2" : !Q_stricmp( rn, "opengl1" ) ? "gl1" : rn,
				sizeof( in->renderer ) );
	char tmp[512];
	BR_CleanLine( cls.glconfig.vendor_string, tmp, sizeof( tmp ), 90 );
	BR_ScrubStr( tmp, ctx, in->glVendor, sizeof( in->glVendor ) );
	BR_CleanLine( cls.glconfig.renderer_string, tmp, sizeof( tmp ), 150 );
	BR_ScrubStr( tmp, ctx, in->glRenderer, sizeof( in->glRenderer ) );
	BR_CleanLine( cls.glconfig.version_string, tmp, sizeof( tmp ), 150 );
	BR_ScrubStr( tmp, ctx, in->glVersion, sizeof( in->glVersion ) );
	const char *dm = Cvar_VariableIntegerValue( "r_fullscreen" )
					   ? ( Cvar_VariableIntegerValue( "r_desktopfullscreen" ) ? "borderless" : "fullscreen" )
					   : "windowed";
	Com_sprintf( in->display, sizeof( in->display ), "%dx%d %s", cls.glconfig.vidWidth, cls.glconfig.vidHeight, dm );
	int ms = Cvar_VariableIntegerValue( !Q_stricmp( in->renderer, "gl2" ) ? "r_ext_framebuffer_multisample"
																		  : "r_ext_multisample" );
	if ( ms > 1 ) {
		Com_sprintf( in->msaa, sizeof( in->msaa ), "MSAA %dx", ms );
	} else {
		Q_strncpyz( in->msaa, "MSAA off", sizeof( in->msaa ) );
	}
}

// Script Error family lines in the log: total count + the last `keep` distinct ones (timestamp prefix stripped)
static int BR_ScriptErrors( const char *log, size_t n, char lines[][200], int keep, int *kept ) {
	static const char *const marks[] = {"Script Error", "Couldn't compile", "not properly loaded", "parse error"};
	int    total = 0;
	size_t i     = 0;
	*kept        = 0;
	while ( i < n ) {
		size_t e = i;
		while ( e < n && log[e] != '\n' ) {
			e++;
		}
		int hit = 0;
		for ( size_t k = 0; k < sizeof( marks ) / sizeof( marks[0] ) && !hit; k++ ) {
			size_t ml = strlen( marks[k] );
			for ( size_t p = i; p + ml <= e && !hit; p++ ) {
				hit = !memcmp( log + p, marks[k], ml );
			}
		}
		if ( hit ) {
			total++;
			const char *s = log + i;
			size_t      l = e - i;
			if ( l && s[0] == '[' ) {
				const char *rb = (const char *)memchr( s, ']', l > 48 ? 48 : l );
				if ( rb ) {
					l -= (size_t)( rb + 1 - s );
					s = rb + 1;
					while ( l && *s == ' ' ) {
						s++;
						l--;
					}
				}
			}
			char   one[200];
			size_t cl = l < sizeof( one ) - 1 ? l : sizeof( one ) - 1;
			memcpy( one, s, cl );
			one[cl] = 0;
			for ( size_t z = 0; z < cl; z++ ) {
				if ( (unsigned char)one[z] < 0x20 ) {
					one[z] = ' ';
				}
			}
			int dup = 0;
			for ( int q = 0; q < *kept; q++ ) {
				dup |= !strcmp( lines[q], one );
			}
			if ( !dup ) {
				if ( *kept == keep ) {
					memmove( lines[0], lines[1], sizeof( lines[0] ) * ( keep - 1 ) );
					( *kept )--;
				}
				strcpy( lines[*kept], one );
				( *kept )++;
			}
		}
		i = e + 1;
	}
	return total;
}

// the live cvar names (Cvar_CommandCompletion), NUL-separated
static brbuf_t s_names;
static int     s_nameCount;

static void BR_NameCb( const char *s ) {
	BR_Put( &s_names, s, strlen( s ) + 1 ); // keeps the NUL
	s_nameCount++;
}

static void BR_WinNote( brwin_t *cur, brwin_t *last, int *start, int ms ) {
	int now = Sys_Milliseconds();
	if ( now - *start >= 60000 || now < *start ) {
		*last = *cur;
		memset( cur, 0, sizeof( *cur ) );
		*start = now;
	}
	cur->frames++;
	cur->sumMs += ms;
	if ( ms > cur->maxMs ) {
		cur->maxMs = ms;
	}
	cur->h50 += ms >= 50;
	cur->h100 += ms >= 100;
}

/*
====================================================================================================================
HZM coop [bug-3486] diagnostics the report prints (collectors: qcommon/hzm_diag.c; this file feeds and reads them).

Everything in this block runs for every player, so it is deliberately tiny: a few timer reads per frame, and once a
second a handful of counters. Nothing here is sent anywhere - it only becomes text when a report is prepared.
====================================================================================================================
*/
extern int cl_hzmRefGen;                               // cl_main.cpp: changes when the renderer DLL is (un)loaded
void      *CL_HzmRendererSymbol( const char *name );   // cl_main.cpp
int        S_HzmChannelsPlaying( void );               // snd_openal_new.cpp
extern "C" void *Sys_CGameSymbol( const char *name );  // sys/new/sys_main_new.c
extern "C" int   SV_HzmGameText( int what, int clientNum, char *buf, int size ); // server/sv_hzmdiag.c
extern "C" const char *SV_HzmPrevScriptErrors( void );                           // server/sv_hzmdiag.c

typedef int ( *brRPerf_t )( long long *out, int n, int requestSample );
typedef int ( *brCgDiag_t )( int *out, int n );

// the renderer's optional R_HzmPerf (renderergl2/tr_hzm_perf.c); NULL with a renderer that has none
static brRPerf_t BR_RPerf( void ) {
	static brRPerf_t fn;
	static int       gen = -1;
	if ( gen != cl_hzmRefGen ) {
		gen = cl_hzmRefGen;
		fn  = (brRPerf_t)CL_HzmRendererSymbol( "R_HzmPerf" );
	}
	return fn;
}

// renderer back end: the DLL keeps running totals; the exe takes this frame's share
typedef struct {
	int       frames;
	long long backendUs, presentUs, flushes;
} brBeWin_t;
static brBeWin_t s_beCur, s_beLast;
static int       s_beWinStart;
static long long s_beSeen[3], s_beInScene;
static int       s_beGen = -1;
static qboolean  s_beMeasured;

/*
====================
CL_HzmRenderScene - cgame's R_RenderScene: re.RenderScene with a stopwatch. Whatever the back end flushed inside it is
taken off again, so "front end" is the front end.
====================
*/
void CL_HzmRenderScene( const refdef_t *fd ) {
	brRPerf_t fn = BR_RPerf();
	long long b0 = 0, b1 = 0;

	if ( fn ) {
		fn( &b0, 1, 0 );
	}
	HzmDiag_Push( HZP_FRONTEND );
	re.RenderScene( fd );
	if ( fn ) {
		fn( &b1, 1, 0 );
		if ( b1 > b0 ) {
			HzmDiag_Nested( HZP_BACKEND, (unsigned long long)( b1 - b0 ) );
			s_beInScene += b1 - b0; // already booked: CL_HzmDiagFrameEnd takes it off the frame's total
		}
	}
	HzmDiag_Pop();
}

/*
====================
CL_HzmDiagFrameEnd - right after CL_Frame (qcommon/common.c): this frame's back-end time gets its own bucket
====================
*/
extern "C" void CL_HzmDiagFrameEnd( void ) {
	brRPerf_t fn = BR_RPerf();
	long long v[3];
	int       now;

	if ( !fn || fn( v, 3, 0 ) < 3 ) {
		s_beMeasured = qfalse;
		return;
	}
	s_beMeasured = qtrue;
	if ( s_beGen == cl_hzmRefGen && v[0] >= s_beSeen[0] && v[1] >= s_beSeen[1] && v[2] >= s_beSeen[2] ) {
		now = Sys_Milliseconds();
		if ( now - s_beWinStart >= 60000 || now < s_beWinStart ) {
			s_beLast = s_beCur;
			memset( &s_beCur, 0, sizeof( s_beCur ) );
			s_beWinStart = now;
		}
		s_beCur.frames++;
		s_beCur.backendUs += v[0] - s_beSeen[0];
		s_beCur.presentUs += v[1] - s_beSeen[1];
		s_beCur.flushes += v[2] - s_beSeen[2];
		if ( v[0] - s_beSeen[0] > s_beInScene ) {
			HzmDiag_Nested( HZP_BACKEND, (unsigned long long)( v[0] - s_beSeen[0] - s_beInScene ) );
		}
	}
	s_beInScene = 0;
	s_beGen = cl_hzmRefGen;
	memcpy( s_beSeen, v, sizeof( s_beSeen ) );
}

// connection quality as this client sees it (cl_main.cpp CL_PacketEvent, cl_parse.cpp CL_ParseSnapshot)
typedef struct {
	int       since; // Sys_Milliseconds of the connect
	int       lost, outOfOrder, badSnaps;
	int       pings, pingMax;
	long long pingSum;
} brNet_t;
static brNet_t s_net;
static brwin_t s_pingCur, s_pingLast; // frames = samples, sumMs = ping sum, maxMs = worst
static int     s_pingWinStart;

/*
====================
CL_HzmNet - what: 0 packets lost, 1 duplicated / out-of-order packet, 2 unusable snapshot, 3 ping sample, 4 new connection
====================
*/
extern "C" void CL_HzmNet( int what, int value ) {
	switch ( what ) {
	case 0:
		s_net.lost += value;
		break;
	case 1:
		s_net.outOfOrder += value;
		break;
	case 2:
		s_net.badSnaps += value;
		break;
	case 3:
		if ( value >= 0 && value < 999 ) {
			s_net.pings++;
			s_net.pingSum += value;
			if ( value > s_net.pingMax ) {
				s_net.pingMax = value;
			}
			BR_WinNote( &s_pingCur, &s_pingLast, &s_pingWinStart, value );
		}
		break;
	case 4:
		memset( &s_net, 0, sizeof( s_net ) );
		memset( &s_pingCur, 0, sizeof( s_pingCur ) );
		memset( &s_pingLast, 0, sizeof( s_pingLast ) );
		s_net.since = Sys_Milliseconds();
		break;
	}
}

/*
The host's mission state for a JOINER's report. This client asks with the reliable command `hzmdiag` when the report
menu opens (at most once per 20 s); the host answers with a few reliable commands, `hzmdiag <n> <total> "<text>"`.
The answer is hostile text like everything else a server sends: it is only accepted while a request is outstanding,
bounded in pieces and bytes, stored as data, and printed later line by line through the cleaner and the scrubber.
*/
#define BR_STATE_CHUNKS   6
#define BR_STATE_CHUNKLEN 760
static char s_stChunk[BR_STATE_CHUNKS][BR_STATE_CHUNKLEN + 4];
static int  s_stTotal, s_stHave;
static int  s_stReqMs = -100000, s_stGotMs;
static int  s_stServerId;

static void BR_StateRequest( void ) {
	int now = Sys_Milliseconds();
	if ( clc.state != CA_ACTIVE || ( com_sv_running && com_sv_running->integer ) || clc.demoplaying ) {
		return;
	}
	if ( now - s_stReqMs < 20000 && now >= s_stReqMs ) {
		return;
	}
	memset( s_stChunk, 0, sizeof( s_stChunk ) );
	s_stTotal = s_stHave = 0;
	s_stReqMs    = now;
	s_stGotMs    = 0;
	s_stServerId = cl.serverId;
	CL_AddReliableCommand( "hzmdiag", qfalse );
}

// cl_cgame.cpp CL_GetServerCommand, with the command already tokenized
qboolean CL_HzmDiagReply( void ) {
	int         now = Sys_Milliseconds();
	int         seq = atoi( Cmd_Argv( 1 ) ), total = atoi( Cmd_Argv( 2 ) );
	const char *text = Cmd_Argv( 3 );

	if ( now - s_stReqMs > 30000 || now < s_stReqMs ) {
		return qfalse; // never asked, or far too late
	}
	if ( total < 1 || total > BR_STATE_CHUNKS || seq < 0 || seq >= total || strlen( text ) > BR_STATE_CHUNKLEN ) {
		return qfalse;
	}
	if ( s_stTotal && s_stTotal != total ) {
		return qfalse;
	}
	if ( !s_stChunk[seq][0] ) {
		s_stHave++;
	}
	s_stTotal = total;
	Q_strncpyz( s_stChunk[seq], text[0] ? text : " ", sizeof( s_stChunk[seq] ) );
	if ( s_stHave == total ) {
		s_stGotMs = now ? now : 1;
	}
	return qtrue;
}

/*
====================
BR_DiagSample - about once a second while in a map: the counts the frame-time section and the fps history print
====================
*/
static void BR_DiagSample( void ) {
	int        counts[HZC_NUM];
	long long  rp[9];
	brRPerf_t  rfn = BR_RPerf();
	brCgDiag_t cfn;
	static long long s_lastSeq = -1;
	static int       s_lastGen = -1;

	for ( int i = 0; i < HZC_NUM; i++ ) {
		counts[i] = -1;
	}
	counts[HZC_SNAP_ENTS] = cl.snap.valid ? cl.snap.numEntities : -1;
	if ( rfn && rfn( rp, 9, 1 ) == 9 ) {
		// a sample is only new when the renderer drew a frame since the last request
		if ( s_lastGen == cl_hzmRefGen && rp[3] != s_lastSeq ) {
			counts[HZC_VIEWS]        = (int)rp[4];
			counts[HZC_DRAWSURFS]    = (int)rp[5];
			counts[HZC_SKEL_SURFS]   = (int)rp[6];
			counts[HZC_SKEL_MODELS]  = (int)rp[7];
			counts[HZC_TESS_BATCHES] = (int)rp[8];
		}
		s_lastSeq = rp[3];
		s_lastGen = cl_hzmRefGen;
	}
	cfn = cge ? (brCgDiag_t)Sys_CGameSymbol( "CG_HzmDiag" ) : NULL;
	if ( cfn ) {
		int c[2] = {-1, -1};
		if ( cfn( c, 2 ) == 2 ) {
			counts[HZC_TEMPMODELS] = c[0];
			counts[HZC_EMITTERS]   = c[1];
		}
	}
	counts[HZC_SND_CHANNELS] = S_HzmChannelsPlaying();
	if ( com_sv_running && com_sv_running->integer ) {
		int g[4];
		if ( SV_HzmGameText( 4, clc.clientNum, (char *)g, sizeof( g ) ) == 4 ) {
			counts[HZC_SV_ENTS]        = g[0];
			counts[HZC_SV_AI_ENEMY]    = g[1];
			counts[HZC_SV_AI_FRIENDLY] = g[2];
			counts[HZC_SV_PLAYERS]     = g[3];
		}
	}
	HzmDiag_CountSample( counts );
}

// every client frame, from CL_BugReportFrame
static void BR_DiagFrame( void ) {
	static int s_next, s_lastState = -1;
	int        now = Sys_Milliseconds();

	if ( (int)clc.state != s_lastState ) {
		if ( clc.state == CA_ACTIVE ) {
			HzmDiag_Event( "client: entered the game%s", ( com_sv_running && com_sv_running->integer ) ? " (own server)" : "" );
			if ( com_sv_running && com_sv_running->integer ) {
				CL_HzmNet( 4, 0 ); // a host never goes through CL_Connect: its counters start with each map
			}
		}
		s_lastState = (int)clc.state;
	}
	if ( clc.state != CA_ACTIVE ) {
		return;
	}
	HzmDiag_ClientActive( 1 );
	if ( now >= s_next || now < s_next - 2000 ) {
		s_next = now + 1000;
		BR_DiagSample();
	}
}

/*
====================================================================================================================
Release identity (user requirement 2026-09-29: every report states the installed release). The installed manifest
version, coop_modVersion, the running binaries' md5 and whether each equals the manifest's sha256, and whether the
updater started this game (the updater passes +set coop_reportLauncher updater; without it, updater.log's age
relative to this process's start decides "probably" / "NO").
====================================================================================================================
*/
#ifdef _WIN32
typedef struct {
	char manifest[48];
	char exeMd5[12];
	int  exeMatch;   // 1 match, 0 differs, -1 not listed / no manifest
	int  mismatches; // loaded binaries whose sha256 differs from their manifest entry
	char launcher[200];
	char launchShort[64];
	char header[400];
} brrelease_t;

static void BR_HashFile2( const char *path, char *md5, char *sha ) {
	brhash_t a, b;
	md5[0] = sha[0] = 0;
	FILE *f = fopen( path, "rb" );
	if ( !f ) {
		return;
	}
	qboolean okA = BR_HashBegin( &a, BCRYPT_MD5_ALGORITHM ), okB = BR_HashBegin( &b, BCRYPT_SHA256_ALGORITHM );
	unsigned char buf[65536];
	size_t        r;
	while ( ( r = fread( buf, 1, sizeof( buf ), f ) ) > 0 ) {
		if ( okA ) {
			BR_HashAdd( &a, buf, r );
		}
		if ( okB ) {
			BR_HashAdd( &b, buf, r );
		}
	}
	fclose( f );
	if ( okA ) {
		BR_HashEnd( &a, md5, 40, 0 );
	}
	if ( okB ) {
		BR_HashEnd( &b, sha, 72, 0 );
	}
}

// the sha256 the manifest lists for a file whose "path" ends in `leaf`; 0 when not listed
static int BR_ManifestSha( const char *json, const char *leaf, char *out ) {
	const char *p = json;
	out[0]        = 0;
	while ( json && ( p = strstr( p, "\"path\"" ) ) ) {
		const char *q = strchr( p + 6, '"' );
		const char *e = q ? strchr( q + 1, '"' ) : NULL;
		if ( !e ) {
			return 0;
		}
		char v[260];
		size_t l = (size_t)( e - q - 1 ) < sizeof( v ) - 1 ? (size_t)( e - q - 1 ) : sizeof( v ) - 1;
		memcpy( v, q + 1, l );
		v[l] = 0;
		const char *vl = strrchr( v, '/' );
		vl             = vl ? vl + 1 : v;
		const char *vb = strrchr( vl, '\\' );
		vl             = vb ? vb + 1 : vl;
		const char *next = strstr( e, "\"path\"" );
		if ( !Q_stricmp( vl, leaf ) ) {
			const char *h = strstr( e, "\"sha256\"" );
			if ( h && ( !next || h < next ) ) {
				h = strchr( h + 8, '"' );
				if ( h && strlen( h ) > 65 ) {
					memcpy( out, h + 1, 64 );
					out[64] = 0;
					Q_strlwr( out );
					return 1;
				}
			}
			return 0;
		}
		p = e;
	}
	return 0;
}

static void BR_Release( brrelease_t *r, const char *modver, const brscrub_t *ctx, brbuf_t *info ) {
	char    exeDir[MAX_OSPATH], path[MAX_OSPATH];
	brbuf_t man = {0};
	memset( r, 0, sizeof( *r ) );
	r->exeMatch = -1;
	BR_ExeDir( exeDir, sizeof( exeDir ) );
	Com_sprintf( path, sizeof( path ), "%s\\installed_manifest.json", exeDir );
	qboolean haveMan = BR_ReadTail( path, 2 * 1024 * 1024, &man, NULL, NULL ) && man.p;
	BR_ManifestVersion( r->manifest, sizeof( r->manifest ) );

	BR_Puts( info, "\r\n=== binaries (loaded right now): md5, and the sha256 check against installed_manifest.json ===\r\n" );
	const char *mods[] = {NULL, "cgame.dll", "game.dll", "renderer_opengl1.dll", "renderer_opengl2.dll"};
	for ( size_t i = 0; i < sizeof( mods ) / sizeof( mods[0] ); i++ ) {
		const char *name = mods[i] ? mods[i] : "openmohaa.exe";
		HMODULE     h    = GetModuleHandleA( mods[i] );
		const char *how  = "loaded";
		if ( h ) {
			GetModuleFileNameA( h, path, sizeof( path ) );
		} else {
			Com_sprintf( path, sizeof( path ), "%s\\%s", exeDir, name );
			how = "not loaded (file next to the exe)";
		}
		long long sz = BR_FileSize( path, NULL );
		if ( sz < 0 ) {
			BR_Printf( info, "%-22s (not loaded, not present)\r\n", name );
			continue;
		}
		char md5[40], sha[72], want[72], disp[MAX_OSPATH];
		BR_HashFile2( path, md5, sha );
		int listed = haveMan && BR_ManifestSha( man.p, name, want );
		int match  = listed && !strcmp( sha, want );
		if ( !mods[i] ) {
			Q_strncpyz( r->exeMd5, md5, 9 );
			r->exeMatch = listed ? match : -1;
		}
		if ( listed && !match && h ) {
			r->mismatches++;
		}
		BR_ScrubStr( path, ctx, disp, sizeof( disp ) );
		BR_Printf( info, "%-22s %10lld  md5 %s  manifest: %-12s %s  %s\r\n", name, sz, md5,
				   !haveMan ? "none" : !listed ? "not listed" : match ? "MATCH" : "DIFFERS", how, disp );
	}
	BR_Free( &man );

	// launched by the updater?
	const char *lv = Cvar_VariableString( "coop_reportLauncher" );
	char        ulog[MAX_OSPATH];
	time_t      um = 0;
	Com_sprintf( ulog, sizeof( ulog ), "%s\\updater.log", exeDir );
	FILETIME ct, xt, kt, ut;
	time_t   started = time( NULL ) - Sys_Milliseconds() / 1000;
	if ( GetProcessTimes( GetCurrentProcess(), &ct, &xt, &kt, &ut ) ) {
		started = BR_FileTimeToUnix( ct );
	}
	if ( !Q_stricmp( lv, "updater" ) ) {
		Q_strncpyz( r->launcher, "yes - started by the MOH Coop Trilogy shortcut (updater)", sizeof( r->launcher ) );
		Q_strncpyz( r->launchShort, "yes", sizeof( r->launchShort ) );
	} else if ( BR_FileSize( ulog, &um ) >= 0 ) {
		long d = (long)( started - um );
		if ( d >= -30 && d <= 600 ) {
			Com_sprintf( r->launcher, sizeof( r->launcher ), "probably - updater.log was written %lds before this launch "
							 "(an older updater does not pass coop_reportLauncher)", d );
			Q_strncpyz( r->launchShort, "probably", sizeof( r->launchShort ) );
		} else {
			char ago[32];
			BR_Duration( d > 0 ? (int)d : -1, ago, sizeof( ago ) );
			Com_sprintf( r->launcher, sizeof( r->launcher ), "NO - openmohaa.exe was started directly; the updater last "
							 "ran %s before this launch, so the game may not be up to date", ago );
			Com_sprintf( r->launchShort, sizeof( r->launchShort ), "**NO** (updater last ran %s earlier)", ago );
		}
	} else {
		Q_strncpyz( r->launcher, haveMan ? "NO - no updater.log next to the exe"
										 : "no - not an updater install (manual or dev copy)", sizeof( r->launcher ) );
		Q_strncpyz( r->launchShort, haveMan ? "**NO**" : "no updater install", sizeof( r->launchShort ) );
	}

	// the header line: installed release first
	const char *mv = r->manifest[0] == 'v' ? r->manifest + 1 : r->manifest;
	const char *dv = modver[0] == 'v' ? modver + 1 : modver;
	qboolean    verMismatch = r->manifest[0] && Q_stricmp( modver, "unknown" ) && Q_stricmp( mv, dv );
	Com_sprintf( r->header, sizeof( r->header ), "Release: **%s%s** (installed manifest) | mod %s | exe %s %s | via updater: %s%s",
				 r->manifest[0] ? "v" : "", r->manifest[0] ? mv : "no manifest (manual/dev install)", modver,
				 r->exeMd5[0] ? r->exeMd5 : "?",
				 r->exeMatch == 1 ? "= manifest" : r->exeMatch == 0 ? "**DIFFERS from the manifest**" : "(not in a manifest)",
				 r->launchShort,
				 verMismatch ? " | **VERSION MISMATCH (manifest vs mod pak)**"
							 : r->mismatches ? " | **a loaded DLL differs from the manifest**" : "" );
	BR_Printf( info, "\r\n=== release ===\r\nInstalledManifest=%s\r\nModVersion=%s\r\nExe=%s (%s)\r\nLoadedBinariesDifferingFromManifest=%d\r\n"
					 "LaunchedByUpdater=%s\r\n",
			   r->manifest[0] ? r->manifest : "none", modver, r->exeMd5,
			   r->exeMatch == 1 ? "matches manifest" : r->exeMatch == 0 ? "DIFFERS from manifest" : "not in a manifest",
			   r->mismatches, r->launcher );
}

// frame timing for the message + report_info: this client's frames, and (host) the server's own frame cost
static void BR_Perf( char *line, size_t sz, brbuf_t *info ) {
	const brwin_t *w = s_clLast.frames ? &s_clLast : &s_clCur;
	char           cl[160] = "client n/a", sv[200] = "";
	if ( w->frames > 0 && w->sumMs > 0 ) {
		Com_sprintf( cl, sizeof( cl ), "client %d fps avg, worst frame %d ms, %d frames >= 100 ms", (int)( 1000.0 * w->frames / w->sumMs + 0.5 ),
					 w->maxMs, w->h100 );
	}
	if ( com_sv_running && com_sv_running->integer ) {
		int st[14];
		Com_HzmServerFrameStats( st );
		const int *v = st[0] ? st : st + 7; // frames, sumCost, maxCost, sumGap, maxGap, cost50, gap100
		if ( v[0] > 0 ) {
			Com_sprintf( sv, sizeof( sv ), "server frame %.1f ms avg, worst %d ms, %d >= 50 ms; gaps worst %d ms, %d >= 100 ms (sv_fps %s)",
						 (double)v[1] / v[0], v[2], v[5], v[4], v[6], Cvar_VariableString( "sv_fps" ) );
		}
	}
	Com_sprintf( line, (int)sz, "Perf (last minute): %s%s%s", cl, sv[0] ? " | " : "", sv );
	BR_Printf( info, "\r\n=== performance (last full minute, else the current one) ===\r\n%s\r\n", line );
	BR_Printf( info, "client window: %d frames, %d ms, worst %d, >=50 ms %d, >=100 ms %d\r\n", w->frames, w->sumMs,
			   w->maxMs, w->h50, w->h100 );
}

// weather/storm state: a host has the coop_* weather cvars; everyone has the rain/fog/lightning configstrings
static void BR_Weather( char *line, size_t sz, brbuf_t *info, qboolean isHost, const brscrub_t *ctx ) {
	float       rain = (float)atof( BR_ConfigString( CS_RAIN_DENSITY ) );
	const char *lt   = BR_ConfigString( CS_HZM_LIGHTNING );
	const char *fog  = BR_ConfigString( CS_FOGINFO );
	Com_sprintf( line, (int)sz, "Weather: %s%s", rain > 0.001f ? va( "rain %.2f", rain ) : "dry",
				 lt[0] ? ", lightning" : "" );
	char rd[64], fg[200], lg[200];
	BR_CleanScrub( BR_ConfigString( CS_RAIN_DENSITY ), ctx, rd, sizeof( rd ), 40 ); // server-supplied: cleaned + scrubbed
	BR_CleanScrub( fog, ctx, fg, sizeof( fg ), 150 );
	BR_CleanScrub( lt, ctx, lg, sizeof( lg ), 150 );
	BR_Printf( info, "\r\n=== weather ===\r\nrain density=%s\r\nfog info=%s\r\nlightning=%s\r\n", rd, fg, lg[0] ? lg : "(none)" );
	if ( isHost ) {
		// HZM-MP-BEGIN(mp_report_weather) - read-only: the host's weather cvars (incl. MP weather) listed in a bug report
		static const char *const wc[] = {"coop_dynWeather", "coop_weatherForce", "coop_weatherPin", "coop_stormNow",
										 "coop_stormKind", "coop_stormCells", "coop_dynWeatherLightning", "coop_mpWeather",
										 "coop_mpStorm", "coop_fog"};
		// HZM-MP-END(mp_report_weather)
		for ( size_t i = 0; i < sizeof( wc ) / sizeof( wc[0] ); i++ ) {
			cvar_t *v = Cvar_FindVar( wc[i] );
			if ( v ) {
				char c[64];
				BR_CleanLine( v->string, c, sizeof( c ), 60 );
				BR_Printf( info, "%s=%s\r\n", wc[i], c );
				if ( !strcmp( wc[i], "coop_stormNow" ) && atoi( c ) ) {
					Q_strcat( line, (int)sz, ", storm now" );
				}
			}
		}
	}
}

/*
====================================================================================================================
HZM coop [bug-3486] the report's diagnostic sections. Every byte that did not originate in this process's own
constants goes through BR_Scrub; anything a server supplied (map and objective text, the host's answer to a joiner,
drop reasons) is also cleaned line by line first, cut to a bounded length and written behind a fixed prefix, so it
cannot forge a section header, name a file or grow the report.
====================================================================================================================
*/
#define BR_DIAG_TEXT 49152

// scrub a block of our own text (LF or CRLF in, CRLF out) and append it
static void BR_PutScrubbed( brbuf_t *out, const char *text, const brscrub_t *ctx ) {
	brbuf_t s = {0};
	BR_Scrub( text, strlen( text ), ctx, &s );
	const char *p = BR_P( &s );
	for ( size_t i = 0; i < s.n; i++ ) {
		if ( p[i] == '\n' && ( i == 0 || p[i - 1] != '\r' ) ) {
			BR_Put( out, "\r\n", 2 );
		} else {
			BR_Put( out, p + i, 1 );
		}
	}
	BR_Free( &s );
}

// untrusted text, `sep`-separated lines: each line cleaned (no control bytes, colour codes, backticks), scrubbed,
// cut at `maxLine` and written after `prefix`; at most `maxLines` lines
static void BR_PutHostile( brbuf_t *out, const char *text, char sep, const char *prefix, const brscrub_t *ctx, int maxLine,
						   int maxLines ) {
	const char *p = text;
	int         n = 0;
	while ( *p && n < maxLines ) {
		const char *e = strchr( p, sep );
		size_t      l = e ? (size_t)( e - p ) : strlen( p );
		char        raw[400], line[400];
		if ( l >= sizeof( raw ) ) {
			l = sizeof( raw ) - 1;
		}
		memcpy( raw, p, l );
		raw[l] = 0;
		BR_CleanScrub( raw, ctx, line, sizeof( line ), (size_t)maxLine );
		if ( line[0] ) {
			BR_Printf( out, "%s%s\r\n", prefix, line );
			n++;
		}
		if ( !e ) {
			break;
		}
		p = e + 1;
	}
}

// the first line of `text` that starts with `key` (lines separated by `sep`), cleaned and scrubbed; "" when absent
static void BR_FindLine( const char *text, char sep, const char *key, const brscrub_t *ctx, char *out, size_t outsz ) {
	size_t      kl = strlen( key );
	const char *p  = text;
	out[0]         = 0;
	while ( p && *p ) {
		const char *e = strchr( p, sep );
		if ( !strncmp( p, key, kl ) ) {
			char   raw[400];
			size_t l = e ? (size_t)( e - p ) : strlen( p );
			if ( l >= sizeof( raw ) ) {
				l = sizeof( raw ) - 1;
			}
			memcpy( raw, p, l );
			raw[l] = 0;
			BR_CleanScrub( raw, ctx, out, outsz, outsz - 1 );
			return;
		}
		p = e ? e + 1 : NULL;
	}
}

// the joiner's copy of the host's answer, pieces joined; qfalse when there is none for this connection
static qboolean BR_StateJoined( char *out, size_t outsz ) {
	out[0] = 0;
	if ( !s_stGotMs || s_stServerId != cl.serverId || s_stTotal < 1 ) {
		return qfalse;
	}
	for ( int i = 0; i < s_stTotal && i < BR_STATE_CHUNKS; i++ ) {
		Q_strcat( out, (int)outsz, s_stChunk[i] );
	}
	return qtrue;
}

static void BR_DiagSections( brbuf_t *info, const brscrub_t *ctx, qboolean inMap, qboolean isHost ) {
	static char text[BR_DIAG_TEXT];
	static char mission[BR_DIAG_TEXT];
	static char joined[BR_STATE_CHUNKS * ( BR_STATE_CHUNKLEN + 4 ) + 8];
	qboolean    haveMission = qfalse, haveJoined = qfalse;
	char        line[400];

	mission[0] = joined[0] = 0;
	if ( inMap && isHost ) {
		haveMission = SV_HzmGameText( 1, clc.clientNum, mission, sizeof( mission ) ) > 0 ? qtrue : qfalse;
	} else if ( inMap ) {
		haveJoined = BR_StateJoined( joined, sizeof( joined ) );
	}

	// ---- 8. where the player is
	if ( inMap ) {
		float       best = -1;
		int         bestIdx = -1;
		const float *o = cl.snap.ps.origin;
		BR_Puts( info, "\r\n=== where the player is ===\r\n" );
		BR_CleanScrub( cl.mapname, ctx, line, sizeof( line ), 64 );
		BR_Printf( info, "map=%s\r\norigin=(%.0f %.0f %.0f)\r\nview angles=(%.0f %.0f %.0f)\r\n", line, o[0], o[1], o[2],
				   cl.snap.ps.viewangles[0], cl.snap.ps.viewangles[1], cl.snap.ps.viewangles[2] );
		for ( int i = 0; i < MAX_OBJECTIVES; i++ ) {
			const char *cs = BR_ConfigString( CS_OBJECTIVES + i );
			float       v[3];
			int         fl;
			if ( !cs[0] ) {
				continue;
			}
			fl = atoi( Info_ValueForKey( cs, "flags" ) );
			if ( ( fl & ( OBJ_FLAG_HIDDEN | OBJ_FLAG_COMPLETED ) ) || sscanf( Info_ValueForKey( cs, "loc" ), "%f %f %f", &v[0], &v[1], &v[2] ) != 3 ) {
				continue;
			}
			if ( v[0] == 0 && v[1] == 0 && v[2] == 0 ) {
				continue; // no location given
			}
			float d = sqrtf( ( v[0] - o[0] ) * ( v[0] - o[0] ) + ( v[1] - o[1] ) * ( v[1] - o[1] ) + ( v[2] - o[2] ) * ( v[2] - o[2] ) );
			if ( bestIdx < 0 || d < best ) {
				best    = d;
				bestIdx = i;
			}
		}
		if ( bestIdx >= 0 ) {
			BR_Printf( info, "nearest open objective=#%d, %.0f units away\r\n", bestIdx + 1, best );
		} else {
			BR_Puts( info, "nearest open objective=none with a location\r\n" );
		}
		if ( haveMission ) {
			BR_FindLine( mission, '\n', "under the crosshair:", ctx, line, sizeof( line ) );
			BR_Printf( info, "%s\r\n", line[0] ? line : "under the crosshair: (not given by the game)" );
		} else if ( haveJoined ) {
			BR_FindLine( joined, '`', "under the crosshair:", ctx, line, sizeof( line ) );
			BR_Printf( info, "%s\r\n", line[0] ? line : "under the crosshair: (not given by the host)" );
		} else {
			BR_Puts( info, "under the crosshair: not available (needs the host's game; see mission state)\r\n" );
		}
	}

	// ---- 4. mission state
	if ( inMap ) {
		int cur = atoi( BR_ConfigString( CS_CURRENT_OBJECTIVE ) ), shown = 0;
		BR_Puts( info, "\r\n=== mission state ===\r\nobjectives (as this client was told; * = the current one):\r\n" );
		for ( int i = 0; i < MAX_OBJECTIVES; i++ ) {
			const char *cs = BR_ConfigString( CS_OBJECTIVES + i );
			float       v[3] = {0, 0, 0};
			char        txt[200];
			int         fl;
			if ( !cs[0] ) {
				continue;
			}
			fl = atoi( Info_ValueForKey( cs, "flags" ) );
			BR_CleanScrub( Info_ValueForKey( cs, "text" ), ctx, txt, sizeof( txt ), 160 );
			sscanf( Info_ValueForKey( cs, "loc" ), "%f %f %f", &v[0], &v[1], &v[2] );
			BR_Printf( info, " %c#%-2d %-9s %s  (at %.0f %.0f %.0f)\r\n", i == cur ? '*' : ' ', i + 1,
					   ( fl & OBJ_FLAG_COMPLETED ) ? "COMPLETED" : ( fl & OBJ_FLAG_HIDDEN ) ? "hidden" : ( fl & OBJ_FLAG_CURRENT ) ? "CURRENT" : "open",
					   txt, v[0], v[1], v[2] );
			shown++;
		}
		if ( !shown ) {
			BR_Puts( info, " (none set)\r\n" );
		}
		if ( haveMission ) {
			BR_Puts( info, "from this machine's game (host):\r\n" );
			BR_PutHostile( info, mission, '\n', "  ", ctx, 300, 300 );
		} else if ( isHost ) {
			BR_Puts( info, "players / AI / level.coop_*: not available (this game.dll does not export G_HzmDiag)\r\n" );
		} else if ( haveJoined ) {
			BR_Printf( info, "answered by the host %d s before this report (a joiner gets a short version, at most ~4 KB):\r\n",
					   ( Sys_Milliseconds() - s_stGotMs ) / 1000 );
			BR_PutHostile( info, joined, '`', "  host> ", ctx, 300, 120 );
		} else {
			BR_Puts( info, "players / AI / level.coop_*: not available on a joiner - the host did not answer the request "
						   "(an older host, or the report was prepared before the answer arrived)\r\n" );
		}
	}

	// ---- 7. script errors, kept by the game whether or not `developer` is set
	BR_Puts( info, "\r\n=== script errors kept by the game (independent of `developer`) ===\r\n" );
	if ( isHost && inMap && SV_HzmGameText( 3, clc.clientNum, text, sizeof( text ) ) > 0 ) {
		BR_PutHostile( info, text, '\n', "", ctx, 380, 120 );
	} else if ( isHost && inMap ) {
		BR_Puts( info, "not available (this game.dll does not export G_HzmDiag)\r\n" );
	} else if ( inMap ) {
		BR_Puts( info, "scripts run on the host: see the host> lines in the mission state above for its count and newest three\r\n" );
	} else {
		BR_Puts( info, "no game running\r\n" );
	}
	if ( SV_HzmPrevScriptErrors()[0] ) {
		// game.dll is unloaded with every map: the server kept the ending map's newest errors as text
		BR_Puts( info, "the map before this one, on this machine's server (kept across the map change, newest 12):\r\n" );
		BR_PutHostile( info, SV_HzmPrevScriptErrors(), '\n', "  ", ctx, 380, 16 );
	}

	// ---- 1. where the frame time goes
	HzmDiag_Text( HZT_FRAME, text, sizeof( text ) );
	BR_PutScrubbed( info, text, ctx );
	{
		const brBeWin_t *w = s_beLast.frames ? &s_beLast : &s_beCur;
		if ( !s_beMeasured ) {
			BR_Puts( info, "renderer back end: NOT measured (this renderer DLL has no R_HzmPerf) - its time is inside client+cgame above\r\n" );
		} else if ( w->frames > 0 ) {
			BR_Printf( info, "renderer back end: measured in the renderer around RB_ExecuteRenderCommands, %.1f flushes per frame; of its "
							 "%.2f ms, %.2f ms is glFinish + the buffer swap (waiting for the GPU / vsync)\r\n",
					   (double)w->flushes / w->frames, w->backendUs / 1000.0 / w->frames, w->presentUs / 1000.0 / w->frames );
		}
	}

	// ---- 2. hitch log, 3. frame-rate history
	HzmDiag_Text( HZT_HITCHES, text, sizeof( text ) );
	BR_PutScrubbed( info, text, ctx );
	HzmDiag_Text( HZT_FPS, text, sizeof( text ) );
	BR_PutScrubbed( info, text, ctx );

	// ---- 5. connection timeline
	HzmDiag_Text( HZT_TIMELINE, text, sizeof( text ) );
	BR_PutScrubbed( info, text, ctx );
	if ( clc.state >= CA_CONNECTED ) {
		const brwin_t *pw = s_pingLast.frames ? &s_pingLast : &s_pingCur;
		BR_Printf( info, "this client's connection%s, %d s old: packets lost %d, duplicated/out-of-order %d, unusable snapshots %d",
				   isHost ? " (to its own server)" : "", ( Sys_Milliseconds() - s_net.since ) / 1000, s_net.lost, s_net.outOfOrder,
				   s_net.badSnaps );
		if ( s_net.pings > 0 ) {
			BR_Printf( info, "; ping avg %d ms, worst %d ms", (int)( s_net.pingSum / s_net.pings ), s_net.pingMax );
		}
		if ( pw->frames > 0 ) {
			BR_Printf( info, " (last minute: avg %d, worst %d)", pw->sumMs / pw->frames, pw->maxMs );
		}
		BR_Puts( info, "\r\n" );
	}

	// ---- 6. freeze watchdog files
	{
		char paths[2][MAX_OSPATH];
		int  n = HzmDiag_FreezeFiles( paths[0], MAX_OSPATH, 2 ), shown = 0;
		BR_Puts( info, "\r\n=== freeze watchdog (main loop stuck > 5 s; newest 2 files of the last 30 days) ===\r\n" );
		for ( int i = 0; i < n; i++ ) {
			brbuf_t fr = {0};
			time_t  fm = 0;
			if ( BR_ReadTail( paths[i], 12 * 1024, &fr, &fm, NULL ) && fr.p && time( NULL ) - fm < 30 * 86400 ) {
				const char *leaf = strrchr( paths[i], '\\' );
				BR_Printf( info, "--- %s (%d h ago) ---\r\n", leaf ? leaf + 1 : "freeze file", (int)( ( time( NULL ) - fm ) / 3600 ) );
				BR_PutScrubbed( info, fr.p, ctx );
				BR_Puts( info, "\r\n" );
				shown++;
			}
			BR_Free( &fr );
		}
		if ( !shown ) {
			BR_Printf( info, "none (com_freezeWatch=%s)\r\n", Cvar_VariableString( "com_freezeWatch" ) );
		}
	}
}

// key binds + HUD / crosshair / view / input cvars (the "green crosshair, KP3 works for my friend" reports)
static void BR_LoadSeeded( void );
static int  BR_IsSeeded( const char *name );

static void BR_ControlsHud( const brscrub_t *ctx, brbuf_t *out, char *hudLine, size_t hsz ) {
	BR_LoadSeeded();
	BR_Puts( out, "// MOH Coop report - your key binds and HUD / crosshair / view / input settings (scrubbed)\r\n\r\n=== key binds ===\r\n" );
	for ( int k = 0; k < MAX_KEYS; k++ ) {
		const char *b = Key_GetBinding( k );
		if ( b && b[0] ) {
			char v[512];
			BR_ScrubStr( b, ctx, v, sizeof( v ) );
			BR_Printf( out, "%-16s %s\r\n", Key_KeynumToString( k ), v );
		}
	}
	BR_Puts( out, "\r\n=== HUD / crosshair / view / input cvars ===\r\n" );
	static const char *const keys[] = {"crosshair", "hud", "compass", "cg_draw", "drawfps", "fov", "sensitivity", "notify",
									   "minicon", "gmbox", "m_pitch", "m_yaw", "m_filter", "in_", "cl_run", "freelook",
									   "viewmodel", "ads", "cg_3rd", "thirdperson", "zoom", "scope", "hitmark", "subtitle"};
	BR_Truncate( &s_names, 0 );
	s_nameCount = 0;
	Cvar_CommandCompletion( BR_NameCb );
	for ( size_t off = 0; s_names.p && off < s_names.n; off += strlen( s_names.p + off ) + 1 ) {
		const char *n = s_names.p + off;
		char        lc[128];
		Q_strncpyz( lc, n, sizeof( lc ) );
		Q_strlwr( lc );
		qboolean hit = qfalse;
		for ( size_t i = 0; i < sizeof( keys ) / sizeof( keys[0] ) && !hit; i++ ) {
			hit = strstr( lc, keys[i] ) != NULL;
		}
		cvar_t *v = hit ? Cvar_FindVar( n ) : NULL;
		// the same ALLOWLIST as settings.cfg: a substring hit ("ads", "hud", "in_") must not admit a script-made cvar
		if ( v && v->string && BR_SettingAllowed( v->name, ( v->flags & CVAR_USER_CREATED ) ? 0 : 1, BR_IsSeeded( v->name ) ) ) {
			char val[256];
			BR_ScrubStr( v->string, ctx, val, sizeof( val ) );
			BR_Printf( out, "%s%s = \"%s\"\r\n", ( v->resetString && strcmp( v->resetString, v->string ) ) ? "*" : " ", v->name, val );
		}
	}
	static const char *const top[] = {"ui_crosshair", "cg_crosshair", "cg_crosshair3p", "cg_crosshairSize",
									  "cg_crosshair_friend", "cg_hud", "ui_hud", "cg_drawviewmodel", "coop_compassBar"};
	hudLine[0] = 0;
	Q_strcat( hudLine, (int)hsz, "HUD:" );
	for ( size_t i = 0; i < sizeof( top ) / sizeof( top[0] ); i++ ) {
		cvar_t *v = Cvar_FindVar( top[i] );
		if ( v ) {
			char c[40];
			BR_CleanLine( v->string, c, sizeof( c ), 24 );
			Q_strcat( hudLine, (int)hsz, va( " %s=%s", top[i], c ) );
		}
	}
}
#endif /* _WIN32 */

typedef struct {
	char      name[48];
	long long size;
	char      note[48];
} brattach_t;

static int        s_nAttach;
static brattach_t s_attach[BR_ATTACH_LINES];

static void BR_AddAttach( const char *name, long long size, const char *note ) {
	if ( s_nAttach >= BR_ATTACH_LINES ) {
		return;
	}
	Q_strncpyz( s_attach[s_nAttach].name, name, sizeof( s_attach[0].name ) );
	s_attach[s_nAttach].size = size;
	Q_strncpyz( s_attach[s_nAttach].note, note, sizeof( s_attach[0].note ) );
	s_nAttach++;
}

static void BR_Fingerprint( char *out, size_t sz ) {
	Com_sprintf( out, (int)sz, "%s|%s|%d%d%d%d%d%d%d|%d|%d|%d", br_text->string, br_steps->string, br_incLog->integer,
				 br_incCfg->integer, br_incShot->integer, br_incCrash->integer, br_incServer->integer,
				 br_incSys->integer, br_incBinds->integer, br_category->integer, br_dryrun->integer, s_openMs );
}

// the coop_* names the SHIPPED cfgs seed (coop_defaults.cfg, autoexec.cfg): "\nname\n" lower-case
static brbuf_t s_seeded;

static void BR_LoadSeeded( void ) {
	const char *cfgs[] = {"coop_defaults.cfg", "autoexec.cfg"};
	if ( s_seeded.n ) {
		return;
	}
	BR_Puts( &s_seeded, "\n" );
	for ( int c = 0; c < 2; c++ ) {
		void *buf = NULL;
		long  len = FS_ReadFile( cfgs[c], &buf );
		if ( len <= 0 || !buf ) {
			continue;
		}
		const char *p = (const char *)buf, *end = p + len;
		while ( p < end ) {
			const char *e = p;
			while ( e < end && *e != '\n' ) {
				e++;
			}
			const char *s = p;
			while ( s < e && ( *s == ' ' || *s == '\t' ) ) {
				s++;
			}
			const char *nm = NULL;
			if ( e - s > 5 && ( !Q_stricmpn( s, "seta ", 5 ) || !Q_stricmpn( s, "sets ", 5 ) || !Q_stricmpn( s, "setu ", 5 ) ) ) {
				nm = s + 5;
			} else if ( e - s > 4 && !Q_stricmpn( s, "set ", 4 ) ) {
				nm = s + 4;
			}
			if ( nm ) {
				while ( nm < e && *nm == ' ' ) {
					nm++;
				}
				const char *ne = nm;
				while ( ne < e && BR_IsWordChar( (unsigned char)*ne ) ) {
					ne++;
				}
				if ( ne - nm > 5 && ne - nm < 100 && !Q_stricmpn( nm, "coop_", 5 ) ) {
					char w[104];
					for ( int k = 0; k < ne - nm; k++ ) {
						w[k] = (char)BR_Lower( (unsigned char)nm[k] );
					}
					w[ne - nm] = 0;
					BR_Puts( &s_seeded, w );
					BR_Puts( &s_seeded, "\n" );
				}
			}
			p = e + 1;
		}
		FS_FreeFile( buf );
	}
}

static int BR_IsSeeded( const char *name ) {
	char w[110];
	size_t l = strlen( name );
	if ( l >= 100 ) {
		return 0;
	}
	w[0] = '\n';
	for ( size_t k = 0; k < l; k++ ) {
		w[k + 1] = (char)BR_Lower( (unsigned char)name[k] );
	}
	w[l + 1] = '\n';
	w[l + 2] = 0;
	return s_seeded.p && strstr( s_seeded.p, w ) ? 1 : 0;
}

// settings.cfg: the live cvar table through the allowlist, sorted, values scrubbed. '*' = differs from default.

static int BR_CmpStrI( const void *a, const void *b ) {
	return Q_stricmp( *(const char *const *)a, *(const char *const *)b );
}

static void BR_Settings( const brscrub_t *ctx, brbuf_t *out ) {
	BR_LoadSeeded();
	BR_Truncate( &s_names, 0 );
	s_nameCount = 0;
	Cvar_CommandCompletion( BR_NameCb );
	const char **arr = (const char **)malloc( sizeof( char * ) * ( s_nameCount + 1 ) );
	int          k = 0, written = 0;
	for ( size_t off = 0; arr && s_names.p && off < s_names.n && k < s_nameCount; k++ ) {
		arr[k] = s_names.p + off;
		off += strlen( s_names.p + off ) + 1;
	}
	if ( arr ) {
		qsort( arr, k, sizeof( char * ), BR_CmpStrI );
	}
	BR_Puts( out, "// MOH Coop in-game report - live settings, scrubbed. '*' = differs from its default.\r\n"
				  "// Included: engine/renderer/sound/client cvars and coop_* settings the engine registers or the shipped\r\n"
				  "// cfgs seed. Never included: passwords, keys, tokens, webhook, GUIDs, IPs, your name, progress/unlocks.\r\n" );
	for ( int i = 0; arr && i < k; i++ ) {
		cvar_t *v = Cvar_FindVar( arr[i] );
		if ( !v || !v->string
			 || !BR_SettingAllowed( v->name, ( v->flags & CVAR_USER_CREATED ) ? 0 : 1, BR_IsSeeded( v->name ) ) ) {
			continue;
		}
		char val[1024];
		BR_ScrubStr( v->string, ctx, val, sizeof( val ) );
		for ( char *c = val; *c; c++ ) {
			if ( *c == '"' || (unsigned char)*c < 0x20 ) {
				*c = '\'';
			}
		}
		BR_Printf( out, "%sset %s \"%s\"\r\n", ( v->resetString && strcmp( v->resetString, v->string ) ) ? "*" : " ",
				   v->name, val );
		written++;
	}
	BR_Printf( out, "// %d of %d cvars included\r\n", written, k );
	free( arr );
}

// word-wrap into up to maxLines of width chars (hard split for unbroken runs)
static int BR_Wrap( const char *text, int width, char lines[][100], int maxLines ) {
	int         n = 0;
	const char *p = text;
	while ( *p && n < maxLines ) {
		const char *nl  = strchr( p, '\n' );
		int         seg = nl ? (int)( nl - p ) : (int)strlen( p );
		if ( seg <= width ) {
			memcpy( lines[n], p, seg );
			lines[n][seg] = 0;
			n++;
			p += seg + ( nl ? 1 : 0 );
			continue;
		}
		int cut = width;
		while ( cut > width / 2 && p[cut] != ' ' ) {
			cut--;
		}
		if ( p[cut] != ' ' ) {
			cut = width;
		}
		memcpy( lines[n], p, cut );
		lines[n][cut] = 0;
		n++;
		p += cut;
		while ( *p == ' ' ) {
			p++;
		}
	}
	if ( *p && n == maxLines ) {
		Q_strncpyz( lines[n - 1], "... more: OPEN FOLDER shows the whole message (preview.txt)", 100 );
	}
	return n;
}

static void BR_Stamp( char *out, size_t sz ) {
	time_t     t = time( NULL );
	struct tm *l = localtime( &t );
	strftime( out, sz, "%Y%m%d-%H%M%S", l );
}

static void BR_Paths( void ) {
	Q_strncpyz( s_root, FS_BuildOSPath( Cvar_VariableString( "fs_homepath" ), FS_GetCurrentGameDir(), "coop_report" ),
				sizeof( s_root ) );
	for ( char *c = s_root; *c; c++ ) {
		if ( *c == '/' ) {
			*c = '\\';
		}
	}
	Com_sprintf( s_dir, sizeof( s_dir ), "%s\\current", s_root );
}

#ifdef _WIN32
// the rate-limit state shared with report_problem.ps1: coop_report\state.txt "last=<unix> day=<unix> count=<n>"
static void BR_StateRead( long long *last, long long *day, int *count ) {
	char    p[MAX_OSPATH];
	brbuf_t b = {0};
	*last = *day = 0;
	*count       = 0;
	Com_sprintf( p, sizeof( p ), "%s\\state.txt", s_root );
	if ( BR_ReadTail( p, 256, &b, NULL, NULL ) && b.p ) {
		const char *s;
		if ( ( s = strstr( b.p, "last=" ) ) ) {
			*last = _atoi64( s + 5 );
		}
		if ( ( s = strstr( b.p, "day=" ) ) ) {
			*day = _atoi64( s + 4 );
		}
		if ( ( s = strstr( b.p, "count=" ) ) ) {
			*count = atoi( s + 6 );
		}
	}
	BR_Free( &b );
}

static void BR_StateWrite( long long last, long long day, int count ) {
	char p[MAX_OSPATH], buf[128];
	Com_sprintf( p, sizeof( p ), "%s\\state.txt", s_root );
	Com_sprintf( buf, sizeof( buf ), "last=%lld day=%lld count=%d\r\n", last, day, count );
	BR_WriteFile( p, buf, strlen( buf ) );
}

// status.txt of a send still in flight (this or an earlier game process): "starting"/"sending" newer than 120 s
static qboolean BR_UploaderBusy( void ) {
	char    st[MAX_OSPATH];
	time_t  m = 0;
	brbuf_t b = {0};
	Com_sprintf( st, sizeof( st ), "%s\\status.txt", s_dir );
	qboolean busy = BR_ReadTail( st, 64, &b, &m, NULL ) && b.p
				 && ( !strncmp( b.p, "sending", 7 ) || !strncmp( b.p, "starting", 8 ) ) && time( NULL ) - m < 120;
	BR_Free( &b );
	return busy;
}
#endif

/*
====================
CL_BugReportOpen_f - coop_reportopen (the button has already pushed coop_report)
====================
*/
static void CL_BugReportOpen_f( void ) {
	s_openMs   = Sys_Milliseconds();
	s_openTime = time( NULL );
	s_prepared = qfalse;
	if ( !s_pending ) {
		BR_Result( "" );
	}
	BR_PreviewClear();
	BR_StateRequest(); // HZM coop [bug-3486] a joiner asks the host for the mission state now; it is here by REVIEW
	// a game-view screenshot, only from inside a map (CL_BugReportFrame does the steps; the menu manager refuses
	// ClearMenus while a PushMenu is still settling, so the close is retried, not assumed)
	if ( clc.state == CA_ACTIVE && br_incShot->integer ) {
		char shot[MAX_OSPATH];
		BR_ShotPath( shot, sizeof( shot ) );
		remove( shot );
		s_shotState    = 1;
		s_shotDeadline = s_openMs + 1500;
	}
}

/*
====================
CL_BugReportPrepare_f - coop_reportprepare: build the exact upload, then show it
====================
*/
static void CL_BugReportPrepare_f( void ) {
#ifndef _WIN32
	Com_Printf( "coop report: only implemented on Windows in this build\n" );
	BR_Result( "3" );
#else
	BR_Paths();
	if ( s_pending || BR_UploaderBusy() ) {
		BR_Result( "4" );
		return;
	}
	if ( !br_text->string[0] ) {
		BR_Result( "0" );
		return;
	}
	brscrub_t ctx;
	BR_ScrubCtx( &ctx );
	BR_ScrubNames( &ctx );
	BR_PreviewClear();
	s_prepared = qfalse;
	s_nAttach  = 0;

	// a finished dry run is kept: current -> dryrun_<its stamp>
	{
		char    st[MAX_OSPATH];
		brbuf_t sb = {0};
		Com_sprintf( st, sizeof( st ), "%s\\status.txt", s_dir );
		if ( BR_ReadTail( st, 64, &sb, NULL, NULL ) && sb.p && !strncmp( sb.p, "dryrun", 6 ) && s_stamp[0] ) {
			char keep[MAX_OSPATH];
			Com_sprintf( keep, sizeof( keep ), "%s\\dryrun_%s", s_root, s_stamp );
			MoveFileA( s_dir, keep );
		}
		BR_Free( &sb );
	}
	char p[MAX_OSPATH];
	Com_sprintf( p, sizeof( p ), "%s\\files\\x", s_dir );
	FS_CreatePath( p );
	Com_sprintf( p, sizeof( p ), "%s\\files", s_dir );
	BR_DeleteFilesIn( p );
	Com_sprintf( p, sizeof( p ), "%s\\dryrun", s_dir );
	BR_DeleteFilesIn( p );
	BR_DeleteFilesIn( s_dir );
	BR_Stamp( s_stamp, sizeof( s_stamp ) );

	brinfo_t in;
	BR_Collect( &in, &ctx );
	char zipName[64];
	Com_sprintf( zipName, sizeof( zipName ), "MOHCoop-Report-%s.zip", s_stamp );

	// ---- log tail (scrubbed), script errors
	brbuf_t logRaw = {0}, logS = {0};
	char    logPath[MAX_OSPATH];
	Q_strncpyz( logPath, FS_BuildOSPath( Cvar_VariableString( "fs_homepath" ), FS_GetCurrentGameDir(), "qconsole.log" ),
				sizeof( logPath ) );
	qboolean haveLog = BR_ReadTail( logPath, BR_LOG_TAIL, &logRaw, NULL, NULL ) && logRaw.p;
	if ( haveLog ) {
		BR_Scrub( logRaw.p, logRaw.n, &ctx, &logS );
	}
	BR_Free( &logRaw );
	static char errs[20][200];
	int         nErr = 0;
	in.scriptErrors  = haveLog ? BR_ScriptErrors( BR_P( &logS ), logS.n, errs, 20, &nErr ) : 0;

	// ---- report_info.txt
	brbuf_t info = {0};
	BR_Printf( &info, "=== MOH Coop Trilogy report %s (in-game) ===\r\nReport #%s\r\n", s_stamp, in.tag );
	BR_Printf( &info, "CurrentBuild=%s\r\nModVersion=%s\r\nEngine=%s\r\nGame=%s\r\n", in.build, in.modver, in.engine,
			   in.game );
	BR_Printf( &info, "\r\n=== session ===\r\nState=%s\r\nRole=%s\r\n", in.state, in.role );
	if ( in.map[0] ) {
		BR_Printf( &info, "Map=%s  \"%s\"\r\nMode=%s\r\nPlayers=%s\r\nServer=%s\r\nTimeOnMap=%s\r\n", in.map,
				   in.mapTitle, in.mode, in.players,
				   br_incServer->integer ? ( in.server[0] ? in.server : "(none)" ) : "(withheld by the player)",
				   in.onMap );
	} else {
		BR_Printf( &info, "Map=none (last map cvar: %s)\r\n", in.lastMap[0] ? in.lastMap : "none" );
	}
	BR_Printf( &info, "Uptime=%s\r\nFPS=%s (average in a map with no menu open)  com_maxfps=%s\r\n", in.uptime, in.fps,
			   BR_Str( "com_maxfps" ) );

	brrelease_t rel;
	BR_Release( &rel, in.modver, &ctx, &info );
	char perfLine[480] = "", wxLine[160] = "";
	if ( in.map[0] ) {
		BR_Perf( perfLine, sizeof( perfLine ), &info );
		BR_Weather( wxLine, sizeof( wxLine ), &info, ( com_sv_running && com_sv_running->integer ) ? qtrue : qfalse, &ctx );
	}
	const char *cat = s_categories[br_category->integer >= 0 && br_category->integer < BR_NCAT ? br_category->integer : 0];
	BR_Printf( &info, "\r\n=== category ===\r\n%s\r\n", br_category->integer > 0 ? cat : "(not chosen)" );
	BR_Puts( &info, "\r\n=== coop paks (home/base, size, name) ===\r\n" );
	{
		const char *bases[] = {Cvar_VariableString( "fs_homepath" ), Cvar_VariableString( "fs_basepath" )};
		for ( int b = 0; b < 2; b++ ) {
			char pat[MAX_OSPATH];
			Q_strncpyz( pat, FS_BuildOSPath( bases[b], FS_GetCurrentGameDir(), "*.pk3" ), sizeof( pat ) );
			WIN32_FIND_DATAA fd;
			HANDLE           h = FindFirstFileA( pat, &fd );
			if ( h == INVALID_HANDLE_VALUE ) {
				continue;
			}
			do {
				char lc[260];
				Q_strncpyz( lc, fd.cFileName, sizeof( lc ) );
				Q_strlwr( lc );
				if ( strstr( lc, "coop" ) || !strncmp( lc, "zzz", 3 ) ) {
					BR_Printf( &info, "%s %12llu  %s\r\n", b ? "base" : "home",
							   ( (unsigned long long)fd.nFileSizeHigh << 32 ) | fd.nFileSizeLow, fd.cFileName );
				}
			} while ( FindNextFileA( h, &fd ) );
			FindClose( h );
		}
	}
	if ( br_incSys->integer ) {
		BR_Printf( &info, "\r\n=== graphics ===\r\ncl_renderer=%s\r\nGL_VENDOR=%s\r\nGL_RENDERER=%s\r\nGL_VERSION=%s\r\n"
						  "Display=%s\r\n%s\r\n",
				   BR_Str( "cl_renderer" ), in.glVendor, in.glRenderer, in.glVersion, in.display, in.msaa );
		static const char *const gfx[] = {"r_mode", "r_fullscreen", "r_desktopfullscreen", "r_noborder", "r_customwidth",
										  "r_customheight", "r_swapInterval", "com_maxfps", "r_ext_multisample",
										  "r_ext_framebuffer_multisample", "r_ext_texture_filter_anisotropic",
										  "r_picmip", "r_gamma", "r_hdr", "r_postProcess", "r_toneMap",
										  "r_autoExposure", "r_ssao", "r_sunShadows", "r_shadows", "r_dlightMode",
										  "r_pshadowDist", "r_depthPrepass", "r_ext_direct_state_access", "r_pbr",
										  "r_normalMapping", "r_specularMapping", "r_cubeMapping", "s_volume",
										  "s_khz", "rate", "snaps", "cl_maxpackets"};
		for ( size_t i = 0; i < sizeof( gfx ) / sizeof( gfx[0] ); i++ ) {
			cvar_t *v = Cvar_FindVar( gfx[i] );
			if ( v ) {
				char val[256];
				BR_ScrubStr( v->string, &ctx, val, sizeof( val ) );
				BR_Printf( &info, "%s=%s\r\n", gfx[i], val );
			}
		}
		BR_Printf( &info, "\r\n=== system ===\r\nOS=%s\r\nCPU=%s\r\nRAM=%llu MB\r\n", in.os, in.cpu, in.ramMb );
	} else {
		BR_Puts( &info, "\r\n=== graphics / system ===\r\n(withheld by the player)\r\n" );
	}

	// ---- crash info: hzm_fatal.log (cwd), the previous session's log, the newest dump's metadata
	if ( br_incCrash->integer ) {
		BR_Puts( &info, "\r\n=== crash info ===\r\n" );
		char    fatal[MAX_OSPATH];
		time_t  fm = 0;
		brbuf_t fr = {0};
		Com_sprintf( fatal, sizeof( fatal ), "%s\\hzm_fatal.log", Sys_Cwd() );
		if ( BR_ReadTail( fatal, BR_FATAL_TAIL, &fr, &fm, NULL ) && fr.p && time( NULL ) - fm < 7 * 86400 ) {
			brbuf_t fs = {0};
			BR_Scrub( fr.p, fr.n, &ctx, &fs );
			Com_sprintf( p, sizeof( p ), "%s\\files\\hzm_fatal.log", s_dir );
			BR_WriteFile( p, BR_P( &fs ), fs.n );
			BR_AddAttach( "hzm_fatal.log", (long long)fs.n, "last 4 KB, scrubbed" );
			BR_Puts( &info, "hzm_fatal.log: attached (last 4 KB, changed in the last 7 days)\r\n" );
			BR_Free( &fs );
		} else {
			BR_Puts( &info, "hzm_fatal.log: none in the last 7 days\r\n" );
		}
		BR_Free( &fr );
		char    prev[MAX_OSPATH];
		time_t  pm = 0;
		brbuf_t pr = {0};
		Q_strncpyz( prev, FS_BuildOSPath( Cvar_VariableString( "fs_homepath" ), FS_GetCurrentGameDir(),
										  "qconsole_prev.log" ), sizeof( prev ) );
		if ( BR_ReadTail( prev, BR_PREV_TAIL, &pr, &pm, NULL ) && pr.p && time( NULL ) - pm < 48 * 3600 ) {
			brbuf_t ps = {0};
			BR_Scrub( pr.p, pr.n, &ctx, &ps );
			Com_sprintf( p, sizeof( p ), "%s\\files\\qconsole_prev.log", s_dir );
			BR_WriteFile( p, BR_P( &ps ), ps.n );
			BR_AddAttach( "qconsole_prev.log", (long long)ps.n, "previous session, last 128 KB" );
			BR_Puts( &info, "qconsole_prev.log: attached (the session before this one, last 128 KB)\r\n" );
			BR_Free( &ps );
		}
		BR_Free( &pr );
		char      dumpPath[MAX_OSPATH];
		time_t    dw = 0;
		long long dsz = 0;
		if ( BR_FindDump( dumpPath, sizeof( dumpPath ), &dw, &dsz ) ) {
			const char *leaf = strrchr( dumpPath, '\\' );
			BR_Printf( &info, "crash dump: %s (%.1f MB, %d h ago) - summary only; the file is never attached from "
							  "in-game (it holds game memory). The desktop reporter can attach it.\r\n",
					   leaf ? leaf + 1 : dumpPath, dsz / 1048576.0, (int)( ( time( NULL ) - dw ) / 3600 ) );
			BR_DumpSummary( dumpPath, &ctx, &info );
		} else {
			BR_Puts( &info, "crash dump: none from the last 48 h\r\n" );
		}
	}

	BR_Printf( &info, "\r\n=== script errors: %d in the log tail (last %d distinct) ===\r\n", in.scriptErrors, nErr );
	for ( int i = 0; i < nErr; i++ ) {
		BR_Printf( &info, "%s\r\n", errs[i] );
	}
	if ( haveLog && logS.p ) {
		BR_Puts( &info, "\r\n=== last 12 console lines ===\r\n" );
		size_t s = logS.n;
		int    nl = 0;
		while ( s > 0 ) {
			if ( logS.p[s - 1] == '\n' && ++nl == 13 ) {
				break;
			}
			s--;
		}
		BR_Put( &info, logS.p + s, logS.n - s );
	}

	// ---- attachments by toggle
	if ( br_incLog->integer && haveLog ) {
		Com_sprintf( p, sizeof( p ), "%s\\files\\qconsole.log", s_dir );
		BR_WriteFile( p, BR_P( &logS ), logS.n );
		BR_AddAttach( "qconsole.log", (long long)logS.n, "last 256 KB, scrubbed" );
	}
	if ( br_incCfg->integer ) {
		brbuf_t cfg = {0};
		BR_Settings( &ctx, &cfg );
		Com_sprintf( p, sizeof( p ), "%s\\files\\settings.cfg", s_dir );
		BR_WriteFile( p, BR_P( &cfg ), cfg.n );
		BR_AddAttach( "settings.cfg", (long long)cfg.n, "settings only, scrubbed" );
		BR_Free( &cfg );
	}
	char hudLine[400] = "";
	if ( br_incBinds->integer ) {
		brbuf_t ch = {0};
		BR_ControlsHud( &ctx, &ch, hudLine, sizeof( hudLine ) );
		Com_sprintf( p, sizeof( p ), "%s\\files\\controls_hud.txt", s_dir );
		BR_WriteFile( p, BR_P( &ch ), ch.n );
		BR_AddAttach( "controls_hud.txt", (long long)ch.n, "key binds + HUD/crosshair settings" );
		BR_Free( &ch );
	}
	if ( br_incShot->integer ) {
		char      shot[MAX_OSPATH];
		time_t    sm  = 0;
		BR_ShotPath( shot, sizeof( shot ) );
		long long ssz = BR_FileSize( shot, &sm );
		if ( ssz > 0 && sm + 2 >= s_openTime ) {
			Com_sprintf( p, sizeof( p ), "%s\\files\\screenshot.jpg", s_dir );
			if ( CopyFileA( shot, p, FALSE ) ) {
				BR_AddAttach( "screenshot.jpg", ssz, "game view (see OPEN FOLDER)" );
			}
		}
	}
	// ---- [bug-3486] where the player is, mission state, script errors, frame time, hitches, fps history, timeline, freezes
	BR_DiagSections( &info, &ctx, in.map[0] ? qtrue : qfalse, ( com_sv_running && com_sv_running->integer ) ? qtrue : qfalse );
	BR_Puts( &info, "\r\n=== privacy ===\r\nScrubbed: home paths -> <HOME>, Windows user and PC name, player names "
					"(yours and the ones in your game) -> <NAME>, IP addresses, webhook URLs, cd keys, password/rcon/token/key/auth/"
					"bearer values, other players' userinfo, join codes.\r\n"
					"Never read into this report: the cd-key file, unlock/save files, crash-dump memory.\r\n"
					"Diagnostics (frame timing, hitch log, frame-rate history, connection timeline, mission state, script errors, "
					"freeze stacks) are numbers, game file names, map/script text and code addresses; they pass the same scrubber. "
					"Server addresses and save-file names are never recorded.\r\n" );

	// ---- texts, message, payload
	brbuf_t t = {0};
	char    d1[1024], d2[640];
	{
		// control bytes / colour codes first (so "password<0x01>hunter2" is seen by the scrubber as it will be sent),
		// then scrub, then cap
		char raw1[1200], raw2[1200];
		BR_CleanLine( br_text->string, raw1, sizeof( raw1 ), sizeof( raw1 ) - 1 );
		BR_CleanLine( br_steps->string, raw2, sizeof( raw2 ), sizeof( raw2 ) - 1 );
		BR_Scrub( raw1, strlen( raw1 ), &ctx, &t );
		BR_CleanLine( BR_P( &t ), d1, sizeof( d1 ), 900 );
		BR_Free( &t );
		BR_Scrub( raw2, strlen( raw2 ), &ctx, &t );
		BR_CleanLine( BR_P( &t ), d2, sizeof( d2 ), 500 );
		BR_Free( &t );
	}
	brbuf_t desc = {0};
	BR_Printf( &desc, "What happened: %s\r\nSteps to reproduce: %s\r\n", d1, d2[0] ? d2 : "(none given)" );
	Com_sprintf( p, sizeof( p ), "%s\\files\\user_description.txt", s_dir );
	BR_WriteFile( p, desc.p, desc.n );
	BR_AddAttach( "user_description.txt", (long long)desc.n, "your text" );
	Com_sprintf( p, sizeof( p ), "%s\\files\\report_info.txt", s_dir );
	BR_WriteFile( p, info.p, info.n );
	BR_AddAttach( "report_info.txt", (long long)info.n, "everything above, in full" );

	// the Discord message. Server-supplied strings (map title, server name) go in code spans: no markdown, no
	// masked links; allowed_mentions (payload) stops pings, flags 4 stops link previews.
	brbuf_t msg = {0};
	BR_Printf( &msg, "**MOH Coop report** #%s (%s) - in-game%s%s%s\n", in.tag, s_stamp,
			   br_category->integer > 0 ? " - **" : "", br_category->integer > 0 ? cat : "", br_category->integer > 0 ? "**" : "" );
	BR_Printf( &msg, "%s\n", rel.header );
	if ( in.map[0] ) {
		BR_Printf( &msg, "%s | Map: `%s`%s%s%s | %s | %s, %s", in.game, in.map, in.mapTitle[0] ? " `" : "", in.mapTitle,
				   in.mapTitle[0] ? "`" : "", in.mode, in.players, in.role );
		if ( br_incServer->integer && in.server[0] ) {
			BR_Printf( &msg, " on `%s`", in.server );
		}
		BR_Printf( &msg, " | on map %s, up %s\n", in.onMap, in.uptime );
		BR_Printf( &msg, "%s | %s\n", perfLine, wxLine );
	} else {
		BR_Printf( &msg, "%s | Map: none (main menu; last map `%s`) | up %s\n", in.game, in.lastMap[0] ? in.lastMap : "none",
				   in.uptime );
	}
	if ( br_incSys->integer ) {
		BR_Printf( &msg, "System: %s | %s | %s | %s | %s | %s | %s\n", in.renderer, in.glRenderer, in.glVersion, in.display,
				   in.msaa, in.os, in.cpu );
	}
	if ( hudLine[0] && ( br_category->integer == 3 || br_category->integer == 8 ) ) {
		BR_Printf( &msg, "%s (binds: controls_hud.txt)\n", hudLine );
	}
	BR_Printf( &msg, "**What happened:** %s\n", d1 );
	if ( d2[0] ) {
		BR_Printf( &msg, "**Steps:** %s\n", d2 );
	}
	if ( in.scriptErrors > 0 ) {
		brbuf_t eb = {0};
		BR_Printf( &eb, "**Script errors:** %d in log (last %d)\n```\n", in.scriptErrors, nErr < 3 ? nErr : 3 );
		for ( int i = ( nErr > 3 ? nErr - 3 : 0 ); i < nErr; i++ ) {
			char one[200];
			BR_CleanLine( errs[i], one, sizeof( one ), 160 );
			BR_Printf( &eb, "%s\n", one );
		}
		BR_Puts( &eb, "```\n" );
		if ( msg.n + eb.n < BR_MSG_MAX - 250 ) {
			BR_Put( &msg, eb.p, eb.n );
		} else {
			BR_Printf( &msg, "**Script errors:** %d in log (see report_info.txt)\n", in.scriptErrors );
		}
		BR_Free( &eb );
	}
	BR_Printf( &msg, "Attached: %s (", zipName );
	for ( int i = 0; i < s_nAttach; i++ ) {
		BR_Printf( &msg, "%s%s", i ? ", " : "", s_attach[i].name );
	}
	BR_Puts( &msg, ")" );
	if ( br_dryrun->integer ) {
		BR_Puts( &msg, "\n(dry run - not posted)" );
	}
	if ( msg.n > BR_MSG_MAX ) {
		size_t cut = BR_MSG_MAX;
		while ( cut > 0 && ( (unsigned char)msg.p[cut] & 0xc0 ) == 0x80 ) {
			cut--;
		}
		BR_Truncate( &msg, cut );
	}

	brbuf_t js = {0};
	BR_Puts( &js, "{\"content\":\"" );
	BR_JsonEscape( BR_P( &msg ), &js );
	BR_Puts( &js, "\",\"allowed_mentions\":{\"parse\":[]},\"flags\":4}" );
	Com_sprintf( p, sizeof( p ), "%s\\payload.json", s_dir );
	qboolean ok = BR_WriteFile( p, js.p, js.n );
	Com_sprintf( p, sizeof( p ), "%s\\zipname.txt", s_dir );
	ok = BR_WriteFile( p, zipName, strlen( zipName ) ) && ok;
	Com_sprintf( p, sizeof( p ), "%s\\preview.txt", s_dir );
	ok = BR_WriteFile( p, BR_P( &msg ), msg.n ) && ok;
	BR_Free( &js );

	// ---- preview cvars
	static char wl[BR_PREVIEW_LINES][100];
	int         nl = BR_Wrap( BR_P( &msg ), BR_PREVIEW_WIDTH, wl, BR_PREVIEW_LINES );
	for ( int i = 0; i < nl; i++ ) {
		Cvar_Set( va( "coop_reportPv%02d", i + 1 ), wl[i] );
	}
	long long total = 0;
	for ( int i = 0; i < s_nAttach; i++ ) {
		Cvar_Set( va( "coop_reportAt%d", i + 1 ), va( "%s  -  %lld KB  -  %s", s_attach[i].name,
													   ( s_attach[i].size + 1023 ) / 1024, s_attach[i].note ) );
		total += s_attach[i].size;
	}
	Cvar_Set( "coop_reportPvTotal", va( "%s: %d files, %lld KB before zipping", zipName, s_nAttach, ( total + 1023 ) / 1024 ) );
	BR_Free( &msg );
	BR_Free( &desc );
	BR_Free( &info );
	BR_Free( &logS );
	if ( !ok ) {
		BR_Result( "3" );
		return;
	}
	BR_ContentSha( s_contentSha, sizeof( s_contentSha ) );
	BR_Fingerprint( s_fingerprint, sizeof( s_fingerprint ) );
	s_prepMs   = Sys_Milliseconds();
	s_prepared = qtrue;
	BR_Result( br_dryrun->integer ? "12" : "" );
	Com_Printf( "coop report: prepared %s (%d attachments) - review, then SEND\n", zipName, s_nAttach );
	UI_PushMenu( "coop_report_review" );
#endif
}

/*
====================
CL_SendReport_f - coop_sendreport (SEND). Same command name as before; cl_main.cpp keeps the registration.
====================
*/
void CL_SendReport_f( void ) {
#ifndef _WIN32
	Com_Printf( "coop report: only implemented on Windows in this build\n" );
	BR_Result( "3" );
#else
	if ( s_pending ) {
		BR_Result( "4" );
		return;
	}
	if ( !br_text->string[0] ) {
		BR_Result( "0" );
		return;
	}
	BR_Paths();
	char fp[sizeof( s_fingerprint )], sha[72];
	BR_Fingerprint( fp, sizeof( fp ) );
	if ( s_prepared ) {
		BR_ContentSha( sha, sizeof( sha ) );
	}
	if ( !s_prepared || strcmp( fp, s_fingerprint ) || strcmp( sha, s_contentSha ) ) {
		// what would go out is not what was shown: rebuild it and show it again
		CL_BugReportPrepare_f();
		if ( s_prepared ) {
			BR_Result( "10" );
		}
		return;
	}
	if ( Sys_Milliseconds() - s_prepMs < 1000 ) { // a person reads the page; a chained command does not
		BR_Result( "10" );
		return;
	}

	// rate limit (accidents, double clicks, loops - not a determined abuser: the webhook is on disk)
	long long now = (long long)time( NULL ), last, day;
	int       count;
	int       dry = br_dryrun->integer;
	BR_StateRead( &last, &day, &count );
	if ( dry == 1 ? ( Sys_Milliseconds() - s_lastDryMs < BR_DRY_COOLDOWN * 1000 )
				  : ( now >= last && now - last < BR_COOLDOWN_SEC ) ) {
		BR_Result( "9" );
		return;
	}
	if ( now - day >= 86400 || now < day ) {
		day   = now;
		count = 0;
	}
	if ( dry != 1 && ( s_sessionSends >= BR_SESSION_MAX || count >= BR_DAY_MAX ) ) {
		BR_Result( "11" );
		return;
	}

	char        url[512] = "";
	const char *mode, *source = "none";
	if ( dry == 1 ) {
		mode = "dry";
	} else if ( dry == 2 ) {
		mode = "local";
		Q_strncpyz( url, BR_Str( "coop_reportTestUrl" ), sizeof( url ) );
		if ( !BR_IsLoopbackUrl( url ) ) {
			Com_Printf( "coop report: coop_reportDryRun 2 needs coop_reportTestUrl http://127.0.0.1:<port>/...\n" );
			BR_Result( "2" );
			return;
		}
		source = "coop_reportTestUrl";
	} else {
		mode   = "send";
		source = BR_FileWebhook( url, sizeof( url ) );
		if ( !source ) {
			Com_Printf( "coop report: no valid webhook in updater.ini or coop_reportwebhook.cfg\n" );
			BR_Result( "2" );
			return;
		}
	}

	// the uploader script goes next to the files (constant text, compiled in), run with -File by absolute path
	char st[MAX_OSPATH], ps1[MAX_OSPATH], sysdir[MAX_PATH], cmd[2 * MAX_OSPATH];
	Com_sprintf( ps1, sizeof( ps1 ), "%s\\upload.ps1", s_dir );
	if ( !BR_WriteFile( ps1, BR_UPLOADER_PS1, strlen( BR_UPLOADER_PS1 ) ) ) {
		SecureZeroMemory( url, sizeof( url ) );
		BR_Result( "3" );
		return;
	}
	Com_sprintf( st, sizeof( st ), "%s\\status.txt", s_dir );
	BR_WriteFile( st, "starting", 8 );
	GetSystemDirectoryA( sysdir, sizeof( sysdir ) );
	Com_sprintf( cmd, sizeof( cmd ),
				 "\"%s\\WindowsPowerShell\\v1.0\\powershell.exe\" -NoProfile -NonInteractive -ExecutionPolicy Bypass "
				 "-WindowStyle Hidden -File \"%s\"",
				 sysdir, ps1 );

	// a private environment block for this one child: ours + HZMREP_*. Nothing is set in the game's own environment.
	brbuf_t env  = {0};
	char   *mine = GetEnvironmentStringsA();
	if ( mine ) {
		const char *e = mine;
		while ( *e ) {
			size_t l = strlen( e );
			if ( Q_stricmpn( e, "HZMREP_", 7 ) ) {
				BR_Put( &env, e, l + 1 );
			}
			e += l + 1;
		}
		FreeEnvironmentStringsA( mine );
	}
	char kv[1200];
	Com_sprintf( kv, sizeof( kv ), "HZMREP_DIR=%s", s_dir );
	BR_Put( &env, kv, strlen( kv ) + 1 );
	Com_sprintf( kv, sizeof( kv ), "HZMREP_MODE=%s", mode );
	BR_Put( &env, kv, strlen( kv ) + 1 );
	Com_sprintf( kv, sizeof( kv ), "HZMREP_SHA=%s", s_contentSha );
	BR_Put( &env, kv, strlen( kv ) + 1 );
	Com_sprintf( kv, sizeof( kv ), "HZMREP_URL=%s", url );
	BR_Put( &env, kv, strlen( kv ) + 1 );
	SecureZeroMemory( kv, sizeof( kv ) );
	SecureZeroMemory( url, sizeof( url ) );
	BR_Put( &env, "", 1 ); // the block ends with an empty string

	STARTUPINFOA        si;
	PROCESS_INFORMATION pi;
	memset( &si, 0, sizeof( si ) );
	si.cb = sizeof( si );
	memset( &pi, 0, sizeof( pi ) );
	BOOL started = CreateProcessA( NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, env.p, s_dir, &si, &pi );
	if ( env.p ) {
		SecureZeroMemory( env.p, env.n );
	}
	BR_Free( &env );
	if ( !started ) {
		Com_Printf( "coop report: could not start the uploader (err %lu)\n", (unsigned long)GetLastError() );
		BR_Result( "5" );
		return;
	}
	CloseHandle( pi.hThread );
	if ( s_proc ) {
		CloseHandle( s_proc );
	}
	s_proc         = pi.hProcess;
	s_pending      = qtrue;
	s_pendingSince = Sys_Milliseconds();
	s_nextPoll     = s_pendingSince + 500;
	if ( dry == 1 ) {
		s_lastDryMs = Sys_Milliseconds();
	} else {
		BR_StateWrite( now, day, count + 1 );
		s_sessionSends++;
	}
	BR_Result( "4" );
	Com_Printf( "coop report: %s (webhook source: %s)\n", dry == 1 ? "dry run started" : "sending", source );
#endif
}

/*
====================
CL_BugReportCategory_f - coop_reportcategory [+1|-1]: the compose page's "What kind of problem?" picker
====================
*/
static void CL_BugReportCategory_f( void ) {
	int d = Cmd_Argc() > 1 ? atoi( Cmd_Argv( 1 ) ) : 1;
	int c = ( ( br_category->integer + d ) % BR_NCAT + BR_NCAT ) % BR_NCAT;
	Cvar_Set( "coop_reportCategory", va( "%d", c ) );
	Cvar_Set( "coop_reportCategoryName", s_categories[c] );
}

/*
====================
CL_BugReportShowFiles_f - coop_reportshowfiles: Explorer on the prepared folder
====================
*/
static void CL_BugReportShowFiles_f( void ) {
#ifdef _WIN32
	BR_Paths();
	if ( GetFileAttributesA( s_dir ) != INVALID_FILE_ATTRIBUTES ) {
		ShellExecuteA( NULL, "open", s_dir, NULL, NULL, SW_SHOWNORMAL );
	}
#endif
}

/*
====================
CL_BugReportFrame - every client frame: the in-map fps average, the screenshot steps, and the uploader's status
====================
*/
void CL_BugReportFrame( int msec ) {
	BR_DiagFrame(); // HZM coop [bug-3486] timeline + the once-a-second count sample
	if ( clc.state == CA_ACTIVE && msec > 0 ) {
		BR_WinNote( &s_clCur, &s_clLast, &s_clWinStart, msec );
	}
	if ( clc.state == CA_ACTIVE && !UI_MenuActive() && !s_shotState && msec > 0 && msec < 1000 ) {
		s_fpsMsAvg = s_fpsSamples ? s_fpsMsAvg * 0.97f + msec * 0.03f : (float)msec;
		s_fpsSamples++;
	}
	int now = Sys_Milliseconds();
	if ( s_shotState ) {
		char shot[MAX_OSPATH];
		if ( clc.state != CA_ACTIVE || now > s_shotDeadline ) {
			s_shotState = 0; // give up without a screenshot, never leave the player without the menu
			if ( !UI_MenuActive() ) {
				UI_PushMenu( "coop_report" );
			}
		} else if ( s_shotState == 1 ) {
			if ( UI_MenuActive() ) {
				UI_ForceMenuOff( false ); // refused while a PushMenu is settling - retried next frame
			} else {
				s_shotState = 2;
				s_shotFrame = cls.framecount;
			}
		} else if ( s_shotState == 2 && cls.framecount >= s_shotFrame + 2 ) {
			// executed NOW, not queued: a `wait` already in the command buffer would hold a queued line past the
			// menu-free frames and the JPEG would show the menu again
			Cmd_ExecuteString( "screenshotJPEG coop_report_shot" );
			s_shotState = 3;
			s_shotFrame = cls.framecount;
		} else if ( s_shotState == 3 && cls.framecount > s_shotFrame + 1 ) {
			BR_ShotPath( shot, sizeof( shot ) );
#ifdef _WIN32
			if ( BR_FileSize( shot, NULL ) > 0 ) {
#else
			if ( 1 ) {
#endif
				s_shotState = 0;
				UI_PushMenu( "coop_report" );
			}
		}
	}
#ifdef _WIN32
	if ( !s_pending || now < s_nextPoll ) {
		return;
	}
	s_nextPoll = now + 500;
	char    st[MAX_OSPATH];
	brbuf_t b = {0};
	Com_sprintf( st, sizeof( st ), "%s\\status.txt", s_dir );
	BR_ReadTail( st, 128, &b, NULL, NULL );
	const char *s   = BR_P( &b );
	const char *res = NULL;
	if ( !strncmp( s, "sent", 4 ) ) {
		res = "1";
	} else if ( !strncmp( s, "dryrun", 6 ) ) {
		res = "8";
	} else if ( !strncmp( s, "toobig", 6 ) ) {
		res = "6";
	} else if ( !strncmp( s, "ratelimited", 11 ) ) {
		res = "7";
	} else if ( !strncmp( s, "failed", 6 ) ) {
		res = "5";
	} else if ( s_proc && WaitForSingleObject( s_proc, 0 ) == WAIT_OBJECT_0 ) {
		res = "5"; // exited without a verdict
	} else if ( now - s_pendingSince > 90000 ) {
		res = "5";
	}
	if ( res ) {
		Com_Printf( "coop report: %s\n", s[0] ? s : "no status" );
		s_pending = qfalse;
		if ( s_proc ) {
			CloseHandle( s_proc );
			s_proc = NULL;
		}
		BR_Result( res );
		if ( !strcmp( res, "1" ) ) {
			Cvar_Set( "coop_reportText", "" );
			Cvar_Set( "coop_reportSteps", "" );
			Cvar_Set( "coop_reportCategory", "0" );
			Cvar_Set( "coop_reportCategoryName", s_categories[0] );
			s_prepared = qfalse;
		}
	} else {
		BR_Result( "4" ); // re-assert: ESC's RestoreCVars can wipe it
	}
	BR_Free( &b );
#endif
}

/*
====================
CL_BugReportInit - from CL_Init (registered unconditionally, so it works from the cold main menu)
====================
*/
void CL_BugReportInit( void ) {
	br_text      = Cvar_Get( "coop_reportText", "", 0 );
	br_steps     = Cvar_Get( "coop_reportSteps", "", 0 );
	br_result    = Cvar_Get( "coop_reportResult", "", 0 );
	br_dryrun    = Cvar_Get( "coop_reportDryRun", "0", 0 ); // 1 = write the exact upload locally, 2 = post to 127.0.0.1
	Cvar_Get( "coop_reportTestUrl", "", 0 );
	br_incLog    = Cvar_Get( "coop_reportIncLog", "1", CVAR_ARCHIVE );
	br_incCfg    = Cvar_Get( "coop_reportIncCfg", "1", CVAR_ARCHIVE );
	br_incShot   = Cvar_Get( "coop_reportIncShot", "1", CVAR_ARCHIVE );
	br_incCrash  = Cvar_Get( "coop_reportIncCrash", "1", CVAR_ARCHIVE );
	br_incServer = Cvar_Get( "coop_reportIncServer", "1", CVAR_ARCHIVE );
	br_incSys    = Cvar_Get( "coop_reportIncSys", "1", CVAR_ARCHIVE );
	br_incBinds  = Cvar_Get( "coop_reportIncBinds", "1", CVAR_ARCHIVE );
	br_category  = Cvar_Get( "coop_reportCategory", "0", 0 );
	Cvar_Get( "coop_reportCategoryName", s_categories[0], 0 );
	Cvar_Get( "coop_reportLauncher", "", 0 ); // +set by updater.ps1 LaunchGame; a server cannot write coop_report*
	Cvar_Get( "coop_reportPvTotal", "", 0 );
	for ( int i = 1; i <= BR_PREVIEW_LINES; i++ ) {
		Cvar_Get( va( "coop_reportPv%02d", i ), "", 0 );
	}
	for ( int i = 1; i <= BR_ATTACH_LINES; i++ ) {
		Cvar_Get( va( "coop_reportAt%d", i ), "", 0 );
	}
	Cmd_AddCommand( "coop_reportopen", CL_BugReportOpen_f );
	Cmd_AddCommand( "coop_reportprepare", CL_BugReportPrepare_f );
	Cmd_AddCommand( "coop_reportshowfiles", CL_BugReportShowFiles_f );
	Cmd_AddCommand( "coop_reportcategory", CL_BugReportCategory_f );
	// coop_sendreport keeps its existing registration in cl_main.cpp (-> CL_SendReport_f, now defined here)
}

#endif /* !BUGREPORT_SELFTEST */
