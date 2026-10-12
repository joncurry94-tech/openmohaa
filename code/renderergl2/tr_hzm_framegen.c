/*
===========================================================================
tr_hzm_framegen.c - HZM frame generation prototype (r_hzmFrameGen, default 0)

The game is CPU-bound: the main thread needs a fixed time per real frame however idle the GPU is. This shows MORE
frames than the main thread makes:

  r_hzmFrameGen 1   a present thread shows, for every real frame N+1, first a GENERATED picture for the camera
                    halfway between N and N+1, and the real picture half a frame later (interpolation: half a real
                    frame of added delay, but the camera is never shown anywhere it was not - extrapolating a mouse
                    overshoots on every stop and reversal)
  r_hzmFrameGen 2   the present thread alone (real frames only) - isolates what moving the swap off the main thread buys
  r_hzmFrameGen 3   single-threaded picture test: fg_capture 0 / fg_capture 1 keep two frames, r_hzmFrameGenDebug
                    1 = show the frame generated from them for the CURRENT camera, 2 = for the camera halfway between
                    them, 3 / 4 = the same with every pixel tinted by where it came from

How a picture is generated. There are no per-pixel motion vectors, so only the CAMERA is reprojected: every output
pixel searches the source frame's depth buffer for the surface that lands on it (a few fixed-point steps), in the newer
frame first and in the older one where that fails (screen edge, something uncovered). Things that move by themselves
keep the newer frame's position, so they still update at the real rate. Two kinds of pixel are never warped and are
taken from the newer real frame as they are: the first-person weapon (drawn with glDepthRange 0..0.3, so its depth
gives it away) and everything 2D (any pixel the HUD pass changed after the 3D picture was complete).

A translucent box over the whole picture (damage flash, fade, tint) is a uniform change, final = scene * K + B; the
generated picture gets the same K and B.

A frame is not generated at all - the real one is simply shown - when there was no world scene (menu, loading screen),
when a 2D quad covering a tenth of the screen or more was drawn over the scene (menu, scope, an opaque fade) or
when the camera jumped. The whole feature stays off in stereo and on the legacy r_ext_framebuffer_multisample path at
render scale 1 (there the HUD is drawn into multisample renderbuffers). The display-split MSAA path and render scale
are handled: the scene depth is then smaller than the display, and is sampled by position.

Measured (2026-10-11, one PC only: NVIDIA 16 GB, windowed 1280x720, MSAA 8x, vsync off): Omaha 44-47 real frames a second
are shown as 87, intervals 11.5 ms +- 0.9; the main thread's frame time does not change by more than the map's own
drift (the swap was 0.3 ms of it here). Costs: four extra sets of display-sized textures, three copies per real
frame, and half a real frame of delay on what is shown.

Commands: fg_stats [reset] (present-time record), fg_capture 0|1 (mode 3), fg_record <n> (the next n pictures as the
present thread shows them -> screenshots/fgrec_*.tga).

Threading. The present thread owns a second GL context that shares textures with the main one, and is the only
thread that touches the window's back buffer and swaps. The main thread copies each finished frame into one of four
texture sets, plants a fence and publishes it; it never waits for the swap. Everything GL the present thread does is
raw qgl* on its own context - the renderer's state cache (glState) belongs to the main thread alone.
The renderer DLL is unloaded on every vid_restart: R_FG_Shutdown joins the thread (RE_Shutdown calls it first).
===========================================================================
*/
#include "tr_local.h"
#include "tr_dsa.h"
#include "tr_hzm_framegen.h"

#ifdef USE_LOCAL_HEADERS
#	include "SDL.h"
#else
#	include <SDL.h>
#endif

// tr_fbo.c (not in tr_fbo.h)
FBO_t   *FBO_Create( const char *name, int width, int height );
qboolean R_CheckFBO( const FBO_t *fbo );

#define FG_SETS			4		// held by the present thread: 2, waiting for it: 1, being written: 1
#define FG_GUN_DEPTH	0.30001f	// RF_DEPTHHACK surfaces are drawn with glDepthRange( 0, 0.3 )
#define FG_BIG2D		0.10f		// one 2D quad covering this much of the screen after the scene = an overlay is up (the in-game menu panel is 0.21, HUD pieces are under 0.03)
#define FG_TS			16384

#define FGK_REAL	0
#define FGK_GEN		1

#ifndef GL_SYNC_GPU_COMMANDS_COMPLETE
#define GL_SYNC_GPU_COMMANDS_COMPLETE	0x9117
#endif
#ifndef GL_SYNC_FLUSH_COMMANDS_BIT
#define GL_SYNC_FLUSH_COMMANDS_BIT		0x00000001
#endif

typedef void *( APIENTRY *fgFenceSync_t )( GLenum condition, GLbitfield flags );
typedef GLenum ( APIENTRY *fgClientWaitSync_t )( void *sync, GLbitfield flags, unsigned long long timeout );
typedef void ( APIENTRY *fgDeleteSync_t )( void *sync );

typedef struct {
	image_t		*S, *F, *D;			// scene before the HUD, final picture, scene depth
	FBO_t		*fboS, *fboF;
} fgSet_t;

typedef struct {
	int			set;
	int			seq;
	qboolean	scene;				// S, D and the matrices are valid
	qboolean	bypass;				// a big 2D overlay was drawn over the scene
	double		V[16], P[16];		// world -> eye, eye -> clip (column-major)
	float		fadeK, fadeB[3];	// full-screen translucent boxes drawn over the scene: final = scene * K + B
	void		*sync;
	Uint64		tPub;
} fgFrame_t;

typedef struct {
	// ---- main thread only
	int				mode;			// what is running now (the cvar is what is wanted)
	qboolean		resTried, resOK;
	fgSet_t			sets[FG_SETS];
	int				w;				// the set this frame is written into
	fgFrame_t		build;
	qboolean		latched;
	double			latV[16], latP[16];
	float			orthoW, orthoH, vpShare, max2D;
	char			max2DName[64];
	int				seq;
	int				lastPubSet;
	fgFrame_t		slot[2];		// mode 3
	qboolean		slotValid[2];
	fgFenceSync_t		FenceSync;
	fgClientWaitSync_t	ClientWaitSync;
	fgDeleteSync_t		DeleteSync;
	int				swapInterval;
	int				width, height;

	// ---- the frame-gen context (whoever has it current)
	void			*ctx;
	qboolean		ctxInit;
	GLuint			prog, vao;
	GLint			uMode, uTint, uRes, uGun, uCurFromOut, uOutFromCur, uPrvFromOut, uOutFromPrv, uFadeK, uFadeB, uHudThr;

	// ---- shared, under mx
	SDL_Thread		*thread;
	SDL_mutex		*mx;
	SDL_cond		*cv;
	int				quit;
	int				threadState;	// 0 starting, 1 running, -1 failed
	fgFrame_t		pending;
	qboolean		hasPending;
	int				heldPrev, heldCur;
} fg_t;

static fg_t fg;

// present-time statistics: one writer (the present thread, or the main thread when the feature is off), read by fg_stats
static struct {
	Uint64			t[FG_TS];
	byte			kind[FG_TS];
	volatile int	n;
	volatile int	nReal, nGen, nNoScene, nBypass, nJump, nDropped, nEarlySkip, nGLErr, nSyncTimeout;
	volatile double	syncWaitMs, genMs;
	volatile int	nSyncWait, nGenTimed, nFade;
	char			lastOverlay[96];
} fgs;

cvar_t *r_hzmFrameGen;
cvar_t *r_hzmFrameGenDebug;
cvar_t *r_hzmFrameGenStats;

static const char *fg_vs =
	"out vec2 vUV;\n"
	"void main() {\n"
	"	vec2 p = vec2( float( ( gl_VertexID << 1 ) & 2 ), float( gl_VertexID & 2 ) );\n"
	"	vUV = p;\n"
	"	gl_Position = vec4( p * 2.0 - 1.0, 0.0, 1.0 );\n"
	"}\n";

static const char *fg_fs =
	"uniform sampler2D uCurS, uCurD, uPrvS, uPrvD, uHudF, uHudS, uHudD;\n"
	"uniform mat4 uCurFromOut, uOutFromCur, uPrvFromOut, uOutFromPrv;\n"
	"uniform vec2 uRes;\n"
	"uniform int uMode;\n"		// 0 = copy the final picture, 1 = generate
	"uniform int uTint;\n"
	"uniform float uGun;\n"
	"uniform float uFadeK;\n"
	"uniform vec4 uFadeB;\n"
	"uniform float uHudThr;\n"
	"in vec2 vUV;\n"
	"out vec4 oColor;\n"
	"float depthAt( sampler2D t, vec2 uv ) {\n"
	"	ivec2 sz = textureSize( t, 0 );\n"
	"	return texelFetch( t, clamp( ivec2( uv * vec2( sz ) ), ivec2( 0 ), sz - ivec2( 1 ) ), 0 ).r;\n"
	"}\n"
	// Catmull-Rom through 9 bilinear fetches: a warped pixel almost never lands on a texel centre
	"vec3 sharp( sampler2D t, vec2 uv ) {\n"
	"	vec2 sz = vec2( textureSize( t, 0 ) );\n"
	"	vec2 sp = uv * sz;\n"
	"	vec2 p1 = floor( sp - 0.5 ) + 0.5;\n"
	"	vec2 f = sp - p1;\n"
	"	vec2 w0 = f * ( -0.5 + f * ( 1.0 - 0.5 * f ) );\n"
	"	vec2 w1 = 1.0 + f * f * ( -2.5 + 1.5 * f );\n"
	"	vec2 w2 = f * ( 0.5 + f * ( 2.0 - 1.5 * f ) );\n"
	"	vec2 w3 = f * f * ( -0.5 + 0.5 * f );\n"
	"	vec2 w12 = w1 + w2;\n"
	"	vec2 t0 = ( p1 - 1.0 ) / sz;\n"
	"	vec2 t3 = ( p1 + 2.0 ) / sz;\n"
	"	vec2 t12 = ( p1 + w2 / w12 ) / sz;\n"
	"	vec3 c = textureLod( t, vec2( t0.x, t0.y ), 0.0 ).rgb * w0.x * w0.y\n"
	"		+ textureLod( t, vec2( t12.x, t0.y ), 0.0 ).rgb * w12.x * w0.y\n"
	"		+ textureLod( t, vec2( t3.x, t0.y ), 0.0 ).rgb * w3.x * w0.y\n"
	"		+ textureLod( t, vec2( t0.x, t12.y ), 0.0 ).rgb * w0.x * w12.y\n"
	"		+ textureLod( t, vec2( t12.x, t12.y ), 0.0 ).rgb * w12.x * w12.y\n"
	"		+ textureLod( t, vec2( t3.x, t12.y ), 0.0 ).rgb * w3.x * w12.y\n"
	"		+ textureLod( t, vec2( t0.x, t3.y ), 0.0 ).rgb * w0.x * w3.y\n"
	"		+ textureLod( t, vec2( t12.x, t3.y ), 0.0 ).rgb * w12.x * w3.y\n"
	"		+ textureLod( t, vec2( t3.x, t3.y ), 0.0 ).rgb * w3.x * w3.y;\n"
	"	return clamp( c, 0.0, 1.0 );\n"
	"}\n"
	"vec2 proj( mat4 m, vec2 uv, float d ) {\n"
	"	vec4 c = m * vec4( uv * 2.0 - 1.0, d * 2.0 - 1.0, 1.0 );\n"
	"	if ( c.w < 0.00001 ) return vec2( -10.0 );\n"
	"	return c.xy / c.w * 0.5 + 0.5;\n"
	"}\n"
	// xy = where in the source picture the surface seen at output pixel p is, z = how far off that answer is (pixels)
	"vec3 find( sampler2D dep, mat4 srcFromOut, mat4 outFromSrc, vec2 p ) {\n"
	"	float d = depthAt( dep, p );\n"
	"	if ( d < uGun ) d = 1.0;\n"
	"	vec2 q = proj( srcFromOut, p, d );\n"
	"	for ( int i = 0; i < 4; i++ ) {\n"
	"		if ( any( lessThan( q, vec2( 0.0 ) ) ) || any( greaterThan( q, vec2( 1.0 ) ) ) ) return vec3( q, 10000.0 );\n"
	"		d = depthAt( dep, q );\n"
	"		if ( d < uGun ) return vec3( q, 10000.0 );\n"
	"		q += p - proj( outFromSrc, q, d );\n"
	"	}\n"
	"	if ( any( lessThan( q, vec2( 0.0 ) ) ) || any( greaterThan( q, vec2( 1.0 ) ) ) ) return vec3( q, 10000.0 );\n"
	"	d = depthAt( dep, q );\n"
	"	if ( d < uGun ) return vec3( q, 10000.0 );\n"
	"	return vec3( q, length( ( proj( outFromSrc, q, d ) - p ) * uRes ) );\n"
	"}\n"
	"void main() {\n"
	"	ivec2 ip = ivec2( gl_FragCoord.xy );\n"
	"	vec3 f = texelFetch( uHudF, ip, 0 ).rgb;\n"
	"	if ( uMode == 0 ) { oColor = vec4( f, 1.0 ); return; }\n"
	"	vec3 s = texelFetch( uHudS, ip, 0 ).rgb;\n"
	"	vec3 df = abs( f - clamp( s * uFadeK + uFadeB.rgb, 0.0, 1.0 ) );\n"
	"	bool hud = max( df.r, max( df.g, df.b ) ) > uHudThr;\n"
	"	bool gun = depthAt( uHudD, vUV ) < uGun;\n"
	"	vec3 c;\n"
	"	vec3 tint = vec3( 1.0 );\n"
	"	if ( hud || gun ) {\n"
	"		c = f;\n"
	"		tint = hud ? vec3( 1.0, 0.45, 1.0 ) : vec3( 0.45, 0.45, 1.0 );\n"
	"	} else {\n"
	"		vec3 a = find( uCurD, uCurFromOut, uOutFromCur, vUV );\n"
	"		if ( a.z <= 1.0 ) {\n"
	"			c = sharp( uCurS, a.xy );\n"
	"		} else {\n"
	"			vec3 b = find( uPrvD, uPrvFromOut, uOutFromPrv, vUV );\n"
	"			if ( b.z <= 1.0 ) {\n"
	"				c = sharp( uPrvS, b.xy );\n"
	"				tint = vec3( 0.45, 1.0, 0.45 );\n"
	"			} else if ( min( a.z, b.z ) < 64.0 ) {\n"
	"				c = a.z <= b.z ? texture( uCurS, a.xy ).rgb : texture( uPrvS, b.xy ).rgb;\n"
	"				tint = vec3( 1.0, 0.3, 0.3 );\n"
	"			} else {\n"
	"				c = s;\n"
	"				tint = vec3( 1.0, 1.0, 0.3 );\n"
	"			}\n"
	"		}\n"
	"	}\n"
	"	if ( !( hud || gun ) ) c = clamp( c * uFadeK + uFadeB.rgb, 0.0, 1.0 );\n"
	"	oColor = vec4( uTint != 0 ? c * tint : c, 1.0 );\n"
	"}\n";

/*
=====================================================================================================================
small matrix helpers (double, column-major like GL)
=====================================================================================================================
*/
static void FG_M4Mul( const double *a, const double *b, double *o )
{
	int c, r, k;
	for ( c = 0; c < 4; c++ ) {
		for ( r = 0; r < 4; r++ ) {
			double s = 0.0;
			for ( k = 0; k < 4; k++ ) {
				s += a[k * 4 + r] * b[c * 4 + k];
			}
			o[c * 4 + r] = s;
		}
	}
}

static qboolean FG_M4Inv( const double *m, double *o )
{
	double inv[16], det;
	int i;

	inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
	inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
	inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
	inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
	inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
	inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
	inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
	inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
	inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
	inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
	inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
	inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
	inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
	inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
	inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
	inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];

	det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
	if ( det == 0.0 ) {
		return qfalse;
	}
	det = 1.0 / det;
	for ( i = 0; i < 16; i++ ) {
		o[i] = inv[i] * det;
	}
	return qtrue;
}

// the camera origin of a world -> eye matrix
static void FG_ViewOrigin( const double *V, double *o )
{
	int i;
	for ( i = 0; i < 3; i++ ) {
		o[i] = -( V[i * 4 + 0] * V[12] + V[i * 4 + 1] * V[13] + V[i * 4 + 2] * V[14] );
	}
}

// the view a fraction t of the way from V0 to V1: the rotation along the shortest arc, the origin on the straight line
static void FG_MidView( const double *V0, const double *V1, double t, double *Vm )
{
	double R0[3][3], R1[3][3], Rr[3][3], Rh[3][3], Rm[3][3], o0[3], o1[3], om[3];
	double tr, c, th, ax[3], s, cc, ss, k;
	int r, cidx, i;

	for ( r = 0; r < 3; r++ ) {
		for ( cidx = 0; cidx < 3; cidx++ ) {
			R0[r][cidx] = V0[cidx * 4 + r];
			R1[r][cidx] = V1[cidx * 4 + r];
		}
	}
	for ( r = 0; r < 3; r++ ) {
		for ( cidx = 0; cidx < 3; cidx++ ) {
			Rr[r][cidx] = R1[r][0] * R0[cidx][0] + R1[r][1] * R0[cidx][1] + R1[r][2] * R0[cidx][2];
		}
	}
	tr = Rr[0][0] + Rr[1][1] + Rr[2][2];
	c = ( tr - 1.0 ) * 0.5;
	if ( c > 1.0 ) c = 1.0;
	if ( c < -1.0 ) c = -1.0;
	th = acos( c );
	s = sin( th );
	if ( th < 1e-9 || fabs( s ) < 1e-9 ) {
		memset( Rh, 0, sizeof( Rh ) );
		Rh[0][0] = Rh[1][1] = Rh[2][2] = 1.0;
	} else {
		ax[0] = ( Rr[2][1] - Rr[1][2] ) / ( 2.0 * s );
		ax[1] = ( Rr[0][2] - Rr[2][0] ) / ( 2.0 * s );
		ax[2] = ( Rr[1][0] - Rr[0][1] ) / ( 2.0 * s );
		k = sqrt( ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2] );
		if ( k > 0.0 ) {
			ax[0] /= k; ax[1] /= k; ax[2] /= k;
		}
		cc = cos( th * t );
		ss = sin( th * t );
		k = 1.0 - cc;
		Rh[0][0] = cc + ax[0] * ax[0] * k;
		Rh[0][1] = ax[0] * ax[1] * k - ax[2] * ss;
		Rh[0][2] = ax[0] * ax[2] * k + ax[1] * ss;
		Rh[1][0] = ax[1] * ax[0] * k + ax[2] * ss;
		Rh[1][1] = cc + ax[1] * ax[1] * k;
		Rh[1][2] = ax[1] * ax[2] * k - ax[0] * ss;
		Rh[2][0] = ax[2] * ax[0] * k - ax[1] * ss;
		Rh[2][1] = ax[2] * ax[1] * k + ax[0] * ss;
		Rh[2][2] = cc + ax[2] * ax[2] * k;
	}
	for ( r = 0; r < 3; r++ ) {
		for ( cidx = 0; cidx < 3; cidx++ ) {
			Rm[r][cidx] = Rh[r][0] * R0[0][cidx] + Rh[r][1] * R0[1][cidx] + Rh[r][2] * R0[2][cidx];
		}
	}
	FG_ViewOrigin( V0, o0 );
	FG_ViewOrigin( V1, o1 );
	for ( i = 0; i < 3; i++ ) {
		om[i] = o0[i] + ( o1[i] - o0[i] ) * t;
	}
	memset( Vm, 0, 16 * sizeof( double ) );
	for ( r = 0; r < 3; r++ ) {
		for ( cidx = 0; cidx < 3; cidx++ ) {
			Vm[cidx * 4 + r] = Rm[r][cidx];
		}
		Vm[12 + r] = -( Rm[r][0] * om[0] + Rm[r][1] * om[1] + Rm[r][2] * om[2] );
	}
	Vm[15] = 1.0;
}

// did the camera cut (respawn, teleport, cinematic cut)? Then there is nothing sensible in between.
static qboolean FG_CameraJumped( const fgFrame_t *a, const fgFrame_t *b )
{
	double oa[3], ob[3], d, dot;

	FG_ViewOrigin( a->V, oa );
	FG_ViewOrigin( b->V, ob );
	d = ( oa[0] - ob[0] ) * ( oa[0] - ob[0] ) + ( oa[1] - ob[1] ) * ( oa[1] - ob[1] ) + ( oa[2] - ob[2] ) * ( oa[2] - ob[2] );
	// the eye's z axis in world space is row 2 of the rotation
	dot = a->V[2] * b->V[2] + a->V[6] * b->V[6] + a->V[10] * b->V[10];
	return ( d > 96.0 * 96.0 || dot < 0.90 ) ? qtrue : qfalse;
}

/*
=====================================================================================================================
the frame-gen context: raw GL, no renderer state cache
=====================================================================================================================
*/
static GLuint FG_Compile( GLenum type, const char *header, const char *src )
{
	const char *parts[2];
	GLuint sh;
	GLint ok = 0;

	parts[0] = header;
	parts[1] = src;
	sh = qglCreateShader( type );
	qglShaderSource( sh, 2, parts, NULL );
	qglCompileShader( sh );
	qglGetShaderiv( sh, GL_COMPILE_STATUS, &ok );
	if ( !ok ) {
		qglDeleteShader( sh );
		return 0;
	}
	return sh;
}

// with the frame-gen context current. No ri.* calls: this also runs on the present thread.
static qboolean FG_CtxInit( void )
{
	const char *header;
	GLuint vs, fs;
	GLint ok = 0;
	static const char *samplers[7] = { "uCurS", "uCurD", "uPrvS", "uPrvD", "uHudF", "uHudS", "uHudD" };
	int i;

	if ( fg.ctxInit ) {
		return qtrue;
	}
	header = ( glRefConfig.glslMajorVersion > 1 || glRefConfig.glslMinorVersion >= 50 ) ? "#version 150\n" : "#version 130\n";
	vs = FG_Compile( GL_VERTEX_SHADER, header, fg_vs );
	fs = FG_Compile( GL_FRAGMENT_SHADER, header, fg_fs );
	if ( !vs || !fs ) {
		return qfalse;
	}
	fg.prog = qglCreateProgram();
	qglAttachShader( fg.prog, vs );
	qglAttachShader( fg.prog, fs );
	qglLinkProgram( fg.prog );
	qglGetProgramiv( fg.prog, GL_LINK_STATUS, &ok );
	qglDeleteShader( vs );
	qglDeleteShader( fs );
	if ( !ok ) {
		return qfalse;
	}
	qglGenVertexArrays( 1, &fg.vao );
	qglBindVertexArray( fg.vao );
	qglUseProgram( fg.prog );
	for ( i = 0; i < 7; i++ ) {
		qglUniform1i( qglGetUniformLocation( fg.prog, samplers[i] ), i );
	}
	fg.uMode = qglGetUniformLocation( fg.prog, "uMode" );
	fg.uTint = qglGetUniformLocation( fg.prog, "uTint" );
	fg.uRes = qglGetUniformLocation( fg.prog, "uRes" );
	fg.uGun = qglGetUniformLocation( fg.prog, "uGun" );
	fg.uFadeK = qglGetUniformLocation( fg.prog, "uFadeK" );
	fg.uFadeB = qglGetUniformLocation( fg.prog, "uFadeB" );
	fg.uHudThr = qglGetUniformLocation( fg.prog, "uHudThr" );
	fg.uCurFromOut = qglGetUniformLocation( fg.prog, "uCurFromOut" );
	fg.uOutFromCur = qglGetUniformLocation( fg.prog, "uOutFromCur" );
	fg.uPrvFromOut = qglGetUniformLocation( fg.prog, "uPrvFromOut" );
	fg.uOutFromPrv = qglGetUniformLocation( fg.prog, "uOutFromPrv" );
	qglDisable( GL_DEPTH_TEST );
	qglDisable( GL_BLEND );
	qglDisable( GL_CULL_FACE );
	qglDisable( GL_SCISSOR_TEST );
	fg.ctxInit = qtrue;
	return qtrue;
}

static void FG_BindTex( int unit, const image_t *img )
{
	qglActiveTexture( GL_TEXTURE0 + unit );
	qglBindTexture( GL_TEXTURE_2D, img->texnum );
}

static void FG_SetMat( GLint loc, const double *m )
{
	float f[16];
	int i;
	for ( i = 0; i < 16; i++ ) {
		f[i] = (float)m[i];
	}
	qglUniformMatrix4fv( loc, 1, GL_FALSE, f );
}

static void FG_DrawBegin( void )
{
	qglViewport( 0, 0, fg.width, fg.height );
	qglUseProgram( fg.prog );
	qglBindVertexArray( fg.vao );
}

// the real picture, as it is
static void FG_DrawCopy( const fgFrame_t *fr )
{
	FG_DrawBegin();
	FG_BindTex( 4, fg.sets[fr->set].F );
	qglUniform1i( fg.uMode, 0 );
	qglDrawArrays( GL_TRIANGLES, 0, 3 );
}

// a picture for the camera ( outV, outP ), made from two real frames; HUD and weapon come from `hud' unwarped
static qboolean FG_DrawGenerated( const fgFrame_t *prv, const fgFrame_t *cur, const fgFrame_t *hud,
	const double *outV, const double *outP, int tint )
{
	double vpO[16], vpC[16], vpP[16], iO[16], iC[16], iP[16], m[16];

	FG_M4Mul( outP, outV, vpO );
	FG_M4Mul( cur->P, cur->V, vpC );
	FG_M4Mul( prv->P, prv->V, vpP );
	if ( !FG_M4Inv( vpO, iO ) || !FG_M4Inv( vpC, iC ) || !FG_M4Inv( vpP, iP ) ) {
		return qfalse;
	}
	FG_DrawBegin();
	FG_BindTex( 0, fg.sets[cur->set].S );
	FG_BindTex( 1, fg.sets[cur->set].D );
	FG_BindTex( 2, fg.sets[prv->set].S );
	FG_BindTex( 3, fg.sets[prv->set].D );
	FG_BindTex( 4, fg.sets[hud->set].F );
	FG_BindTex( 5, fg.sets[hud->set].S );
	FG_BindTex( 6, fg.sets[hud->set].D );
	qglUniform1i( fg.uMode, 1 );
	qglUniform1i( fg.uTint, tint );
	qglUniform2f( fg.uRes, (float)fg.width, (float)fg.height );
	qglUniform1f( fg.uGun, FG_GUN_DEPTH );
	qglUniform1f( fg.uFadeK, hud->fadeK );
	qglUniform4f( fg.uFadeB, hud->fadeB[0], hud->fadeB[1], hud->fadeB[2], 0.0f );
	// with a fade in play the scene copy and the final picture differ by two roundings instead of none
	qglUniform1f( fg.uHudThr, ( hud->fadeK != 1.0f || hud->fadeB[0] + hud->fadeB[1] + hud->fadeB[2] != 0.0f ) ? 0.009f : 0.0025f );
	FG_M4Mul( vpC, iO, m ); FG_SetMat( fg.uCurFromOut, m );
	FG_M4Mul( vpO, iC, m ); FG_SetMat( fg.uOutFromCur, m );
	FG_M4Mul( vpP, iO, m ); FG_SetMat( fg.uPrvFromOut, m );
	FG_M4Mul( vpO, iP, m ); FG_SetMat( fg.uOutFromPrv, m );
	qglDrawArrays( GL_TRIANGLES, 0, 3 );
	return qtrue;
}

static void FG_MidCamera( const fgFrame_t *a, const fgFrame_t *b, double *V, double *P )
{
	int i;
	FG_MidView( a->V, b->V, 0.5, V );
	for ( i = 0; i < 16; i++ ) {
		P[i] = ( a->P[i] + b->P[i] ) * 0.5;
	}
}

// fg_record <n>: the present thread keeps the next n pictures exactly as it shows them (real and generated), the main
// thread writes them out as screenshots/fgrec_<index>_<R|G>_<time in 0.1 ms>.tga. For looking at generated frames
// from live play; reading the back buffer costs the present thread a millisecond or two per picture while it runs.
#define FG_REC_MAX	96
static struct {
	volatile int	want, count, done;
	byte			*pix[FG_REC_MAX];
	byte			kind[FG_REC_MAX];
	Uint64			t[FG_REC_MAX];
} fgRec;

static void FG_RecFlush( void );

static void FG_RecGrab( int kind )
{
	int i = fgRec.count;

	if ( fgRec.done || i >= fgRec.want ) {
		return;
	}
	fgRec.pix[i] = (byte *)malloc( (size_t)fg.width * fg.height * 4 );
	if ( !fgRec.pix[i] ) {
		fgRec.done = 1;
		return;
	}
	// RGBA: rows are 4-aligned whatever the pack alignment is
	qglReadPixels( 0, 0, fg.width, fg.height, GL_RGBA, GL_UNSIGNED_BYTE, fgRec.pix[i] );
	fgRec.kind[i] = (byte)kind;
	fgRec.t[i] = SDL_GetPerformanceCounter();
	fgRec.count = i + 1;
	if ( fgRec.count >= fgRec.want ) {
		fgRec.done = 1;
	}
}

static void FG_Record( int kind )
{
	int i = fgs.n & ( FG_TS - 1 );
	fgs.t[i] = SDL_GetPerformanceCounter();
	fgs.kind[i] = (byte)kind;
	fgs.n++;
	if ( kind == FGK_GEN ) {
		fgs.nGen++;
	} else {
		fgs.nReal++;
	}
}

/*
=====================================================================================================================
the present thread
=====================================================================================================================
*/
static double FG_Ms( Uint64 a, Uint64 b )
{
	return (double)( b - a ) * 1000.0 / (double)SDL_GetPerformanceFrequency();
}

// sleep until `target'. Returns qfalse if a newer frame was published first (and early is set).
static qboolean FG_WaitUntil( Uint64 target, qboolean wakeOnPending )
{
	for ( ;; ) {
		Uint64 now = SDL_GetPerformanceCounter();
		double rem;

		if ( now >= target ) {
			return qtrue;
		}
		rem = FG_Ms( now, target );
		if ( rem > 2.0 ) {
			qboolean pend;
			SDL_LockMutex( fg.mx );
			pend = fg.hasPending || fg.quit;
			if ( !pend || !wakeOnPending ) {
				SDL_CondWaitTimeout( fg.cv, fg.mx, (Uint32)( rem - 1.5 ) );
				pend = fg.hasPending || fg.quit;
			}
			SDL_UnlockMutex( fg.mx );
			if ( pend && wakeOnPending ) {
				return qfalse;
			}
			if ( pend ) {
				// the condition is already raised: a timed wait would return at once, so yield instead of spinning hot
				SDL_Delay( 1 );
			}
		}
		// the last stretch is spun: a timed wait is only good to a millisecond
	}
}

static int FG_ThreadMain( void *unused )
{
	fgFrame_t	cur, next;
	qboolean	haveCur = qfalse;
	double		dEst = 0.0;
	int			state;
	int			errTick = 0;

	(void)unused;
	memset( &cur, 0, sizeof( cur ) );

	state = ( GLimp_FG_MakeCurrent( fg.ctx ) && FG_CtxInit() ) ? 1 : -1;
	if ( state == 1 ) {
		GLimp_FG_SetSwapInterval( fg.swapInterval );
	}
	SDL_LockMutex( fg.mx );
	fg.threadState = state;
	SDL_CondBroadcast( fg.cv );
	SDL_UnlockMutex( fg.mx );
	if ( state != 1 ) {
		GLimp_FG_ReleaseCurrent();
		return 0;
	}
	SDL_SetThreadPriority( SDL_THREAD_PRIORITY_HIGH );

	for ( ;; ) {
		qboolean gen;
		Uint64 t0;

		SDL_LockMutex( fg.mx );
		while ( !fg.hasPending && !fg.quit ) {
			SDL_CondWaitTimeout( fg.cv, fg.mx, 200 );
		}
		if ( fg.quit ) {
			SDL_UnlockMutex( fg.mx );
			break;
		}
		next = fg.pending;
		fg.hasPending = qfalse;
		fg.heldPrev = haveCur ? cur.set : -1;
		fg.heldCur = next.set;
		SDL_UnlockMutex( fg.mx );

		// the main thread only flushed: wait until the GPU has really finished this frame's textures
		if ( next.sync ) {
			GLenum r;
			t0 = SDL_GetPerformanceCounter();
			r = fg.ClientWaitSync( next.sync, 0, 200000000ULL );
			if ( r == 0x911B /* GL_TIMEOUT_EXPIRED */ || r == 0x911D /* GL_WAIT_FAILED */ ) {
				fgs.nSyncTimeout++;
			}
			fg.DeleteSync( next.sync );
			next.sync = NULL;
			fgs.syncWaitMs += FG_Ms( t0, SDL_GetPerformanceCounter() );
			fgs.nSyncWait++;
		}

		gen = qfalse;
		if ( fg.mode == 1 ) {
			if ( !next.scene ) {
				fgs.nNoScene++;
			} else if ( next.bypass ) {
				fgs.nBypass++;
			} else if ( haveCur && cur.scene && !cur.bypass ) {
				if ( FG_CameraJumped( &cur, &next ) ) {
					fgs.nJump++;
				} else {
					gen = qtrue;
				}
			}
		}

		if ( haveCur ) {
			double d = FG_Ms( cur.tPub, next.tPub );
			if ( d > 100.0 ) d = 100.0;
			dEst = dEst <= 0.0 ? d : dEst + ( d - dEst ) * 0.2;
		}

		if ( gen ) {
			double V[16], P[16];
			Uint64 tGen;
			qboolean showReal = qtrue;

			t0 = SDL_GetPerformanceCounter();
			FG_MidCamera( &cur, &next, V, P );
			if ( FG_DrawGenerated( &cur, &next, &next, V, P, r_hzmFrameGenDebug->integer >= 3 ) ) {
				FG_RecGrab( FGK_GEN );
				GLimp_FG_Swap();
				tGen = SDL_GetPerformanceCounter();
				FG_Record( FGK_GEN );
				fgs.genMs += FG_Ms( t0, tGen );
				fgs.nGenTimed++;
				// the real picture half a real frame after the generated one
				if ( !FG_WaitUntil( tGen + (Uint64)( dEst * 0.5 / 1000.0 * (double)SDL_GetPerformanceFrequency() ), qtrue ) ) {
					// the next real frame is already here. If this one's turn was still far off, leave it out:
					// the generated pictures alone are an even sequence, two pictures back to back are not.
					if ( dEst * 0.5 - FG_Ms( tGen, SDL_GetPerformanceCounter() ) > dEst * 0.25 ) {
						showReal = qfalse;
						fgs.nEarlySkip++;
					}
				}
			}
			if ( showReal && !fg.quit ) {
				FG_DrawCopy( &next );
				FG_RecGrab( FGK_REAL );
				GLimp_FG_Swap();
				FG_Record( FGK_REAL );
			}
		} else {
			FG_DrawCopy( &next );
			FG_RecGrab( FGK_REAL );
			GLimp_FG_Swap();
			FG_Record( FGK_REAL );
		}

		if ( ( ++errTick & 63 ) == 0 ) {
			while ( qglGetError() != GL_NO_ERROR ) {
				fgs.nGLErr++;
			}
		}

		cur = next;
		haveCur = qtrue;
	}

	qglFinish();
	GLimp_FG_ReleaseCurrent();
	return 0;
}

static void FG_StopThread( void )
{
	if ( fg.thread ) {
		SDL_LockMutex( fg.mx );
		fg.quit = 1;
		SDL_CondBroadcast( fg.cv );
		SDL_UnlockMutex( fg.mx );
		SDL_WaitThread( fg.thread, NULL );
		fg.thread = NULL;
	}
	fg.quit = 0;
	if ( fg.hasPending && fg.pending.sync && fg.DeleteSync ) {
		fg.DeleteSync( fg.pending.sync );
	}
	fg.hasPending = qfalse;
	fg.heldPrev = fg.heldCur = -1;
	fg.lastPubSet = -1;
}

static qboolean FG_StartThread( void )
{
	fg.quit = 0;
	fg.threadState = 0;
	fg.hasPending = qfalse;
	fg.heldPrev = fg.heldCur = -1;
	fg.lastPubSet = -1;
	fg.swapInterval = r_swapInterval ? r_swapInterval->integer : 0;
	fg.thread = SDL_CreateThread( FG_ThreadMain, "hzm_present", NULL );
	if ( !fg.thread ) {
		return qfalse;
	}
	SDL_LockMutex( fg.mx );
	while ( fg.threadState == 0 ) {
		SDL_CondWaitTimeout( fg.cv, fg.mx, 100 );
	}
	SDL_UnlockMutex( fg.mx );
	if ( fg.threadState != 1 ) {
		SDL_WaitThread( fg.thread, NULL );
		fg.thread = NULL;
		return qfalse;
	}
	return qtrue;
}

/*
=====================================================================================================================
main thread
=====================================================================================================================
*/
static const char *FG_Unsupported( void )
{
	if ( !glRefConfig.framebufferObject || !tr.renderFbo || !glRefConfig.framebufferBlit ) {
		return "no framebuffer objects";
	}
	if ( tr.msaaResolveFbo && tr.renderFbo == tr.sceneFbo ) {
		// r_ext_framebuffer_multisample without render scale: the HUD is drawn into multisample renderbuffers
		return "the legacy multisample path (r_ext_framebuffer_multisample)";
	}
	if ( glConfig.stereoEnabled ) {
		return "stereo rendering";
	}
	if ( !( glRefConfig.glslMajorVersion > 1 || glRefConfig.glslMinorVersion >= 30 ) || !glRefConfig.vertexArrayObject ) {
		return "needs GLSL 1.30 and vertex array objects";
	}
	if ( !tr.renderDepthImage ) {
		return "no scene depth texture";
	}
	return NULL;
}

static qboolean FG_CreateResources( void )
{
	const char *why = FG_Unsupported();
	int i;

	if ( why ) {
		ri.Printf( PRINT_ALL, "r_hzmFrameGen: not available - %s\n", why );
		return qfalse;
	}
	fg.width = glConfig.vidWidth;
	fg.height = glConfig.vidHeight;
	for ( i = 0; i < FG_SETS; i++ ) {
		fgSet_t *s = &fg.sets[i];
		s->S = R_CreateImage( va( "_hzmfgS%d", i ), NULL, fg.width, fg.height, IMGTYPE_COLORALPHA, IMGFLAG_NO_COMPRESSION | IMGFLAG_CLAMPTOEDGE, GL_RGBA8 );
		s->F = R_CreateImage( va( "_hzmfgF%d", i ), NULL, fg.width, fg.height, IMGTYPE_COLORALPHA, IMGFLAG_NO_COMPRESSION | IMGFLAG_CLAMPTOEDGE, GL_RGBA8 );
		s->D = R_CreateImage( va( "*hzmfgD%d", i ), NULL, tr.renderDepthImage->width, tr.renderDepthImage->height, IMGTYPE_COLORALPHA, IMGFLAG_NO_COMPRESSION | IMGFLAG_CLAMPTOEDGE, GL_DEPTH_COMPONENT24 );
		if ( !s->S || !s->F || !s->D ) {
			ri.Printf( PRINT_ALL, "r_hzmFrameGen: not available - could not create its textures\n" );
			return qfalse;
		}
		s->fboS = FBO_Create( va( "_hzmfgS%d", i ), fg.width, fg.height );
		s->fboF = FBO_Create( va( "_hzmfgF%d", i ), fg.width, fg.height );
		if ( !s->fboS || !s->fboF ) {
			ri.Printf( PRINT_ALL, "r_hzmFrameGen: not available - could not create its framebuffers\n" );
			return qfalse;
		}
		FBO_AttachImage( s->fboS, s->S, GL_COLOR_ATTACHMENT0, 0 );
		FBO_AttachImage( s->fboF, s->F, GL_COLOR_ATTACHMENT0, 0 );
		if ( !R_CheckFBO( s->fboS ) || !R_CheckFBO( s->fboF ) ) {
			ri.Printf( PRINT_ALL, "r_hzmFrameGen: not available - its framebuffers are incomplete\n" );
			return qfalse;
		}
	}
	GL_BindFramebuffer( GL_FRAMEBUFFER, 0 );
	glState.currentFBO = NULL;

	fg.ctx = GLimp_FG_CreateContext();
	if ( !fg.ctx ) {
		ri.Printf( PRINT_ALL, "r_hzmFrameGen: not available - no second GL context\n" );
		return qfalse;
	}
	fg.ctxInit = qfalse;
	fg.FenceSync = (fgFenceSync_t)GLimp_FG_GetProc( "glFenceSync" );
	fg.ClientWaitSync = (fgClientWaitSync_t)GLimp_FG_GetProc( "glClientWaitSync" );
	fg.DeleteSync = (fgDeleteSync_t)GLimp_FG_GetProc( "glDeleteSync" );
	if ( !fg.FenceSync || !fg.ClientWaitSync || !fg.DeleteSync ) {
		fg.FenceSync = NULL;	// the main thread then finishes each frame before publishing it
	}
	ri.Printf( PRINT_ALL, "r_hzmFrameGen: ready (%dx%d, depth %dx%d, %s)\n", fg.width, fg.height,
		tr.renderDepthImage->width, tr.renderDepthImage->height, fg.FenceSync ? "fence sync" : "glFinish" );
	return qtrue;
}

static void FG_ResetBuild( void )
{
	memset( &fg.build, 0, sizeof( fg.build ) );
	fg.build.fadeK = 1.0f;
	fg.latched = qfalse;
	fg.max2D = 0.0f;
}

static void FG_SetMode( void )
{
	int want = r_hzmFrameGen->integer;

	if ( want < 0 || want > 3 ) {
		want = 0;
	}
	if ( fg.mode && fg.thread && fg.threadState != 1 ) {
		want = 0;
	}
	if ( want == fg.mode ) {
		return;
	}
	FG_StopThread();
	fg.mode = 0;
	if ( !want ) {
		return;
	}
	if ( !fg.resTried ) {
		fg.resTried = qtrue;
		fg.resOK = FG_CreateResources();
	}
	if ( !fg.resOK ) {
		ri.Cvar_Set( "r_hzmFrameGen", "0" );
		return;
	}
	FG_ResetBuild();
	fg.slotValid[0] = fg.slotValid[1] = qfalse;
	if ( want == 3 ) {
		fg.w = 2;
	} else {
		fg.w = 0;
		if ( !FG_StartThread() ) {
			ri.Printf( PRINT_ALL, "r_hzmFrameGen: the present thread did not start\n" );
			ri.Cvar_Set( "r_hzmFrameGen", "0" );
			return;
		}
	}
	fg.mode = want;
}

qboolean R_FG_Wanted( void )
{
	return ( fg.mode || ( r_hzmFrameGen && r_hzmFrameGen->integer ) ) ? qtrue : qfalse;
}

void R_FG_LatchView( const void *viewParms )
{
	const viewParms_t *vp = (const viewParms_t *)viewParms;
	int sw, sh, i;

	if ( !fg.mode || fg.build.scene ) {
		return;
	}
	// only a view that fills the picture: anything smaller would need its own rectangle in every pass
	sw = tr.sceneFbo ? tr.sceneFbo->width : glConfig.vidWidth;
	sh = tr.sceneFbo ? tr.sceneFbo->height : glConfig.vidHeight;
	if ( vp->viewportX != 0 || vp->viewportY != 0 || vp->viewportWidth != sw || vp->viewportHeight != sh ) {
		fg.latched = qfalse;
		return;
	}
	for ( i = 0; i < 16; i++ ) {
		fg.latV[i] = vp->world.modelMatrix[i];
		fg.latP[i] = vp->projectionMatrix[i];
	}
	fg.latched = qtrue;
}

void R_FG_SceneDone( qboolean depthValid )
{
	fgSet_t *s;
	FBO_t *depthFbo, *old;

	if ( !fg.mode || !fg.latched || fg.build.scene || !depthValid ) {
		return;
	}
	s = &fg.sets[fg.w];
	old = glState.currentFBO;

	FBO_FastBlit( tr.renderFbo, NULL, s->fboS, NULL, GL_COLOR_BUFFER_BIT, GL_NEAREST );

	// the single-sampled scene depth (tr.renderDepthImage): with MSAA it is the resolve target's, else the scene's own
	depthFbo = tr.msaaResolveFbo ? tr.msaaResolveFbo : ( tr.sceneFbo ? tr.sceneFbo : tr.renderFbo );
	FBO_Bind( depthFbo );
	qglCopyTextureSubImage2DEXT( s->D->texnum, GL_TEXTURE_2D, 0, 0, 0, 0, 0, s->D->width, s->D->height );
	FBO_Bind( old );

	memcpy( fg.build.V, fg.latV, sizeof( fg.build.V ) );
	memcpy( fg.build.P, fg.latP, sizeof( fg.build.P ) );
	fg.build.scene = qtrue;
	fg.latched = qfalse;
	fg.max2D = 0.0f;
}

void R_FG_Set2DWindow( float w, float h, int vpW, int vpH )
{
	fg.orthoW = fabs( w );
	fg.orthoH = fabs( h );
	// a HUD element sets a small 2D window of its own: its quad fills that window, not the screen
	fg.vpShare = ( glConfig.vidWidth > 0 && glConfig.vidHeight > 0 )
		? (float)abs( vpW ) * (float)abs( vpH ) / ( (float)glConfig.vidWidth * (float)glConfig.vidHeight ) : 1.0f;
	if ( fg.vpShare > 1.0f ) {
		fg.vpShare = 1.0f;
	}
}

void R_FG_Note2D( float w, float h, const char *what )
{
	float a;

	if ( !fg.mode || !fg.build.scene || fg.orthoW <= 0.0f || fg.orthoH <= 0.0f ) {
		return;
	}
	if ( what && what[0] == '(' && what[1] == 'b' && backEnd.color2D[3] == 0 ) {
		return;		// a box drawn with alpha 0 changes nothing
	}
	a = fabs( w * h ) / ( fg.orthoW * fg.orthoH );
	if ( a > 1.0f ) {
		a = 1.0f;
	}
	a *= fg.vpShare;
	if ( what && what[0] == '(' && a >= 0.98f && ( what[1] == 'a' || backEnd.color2D[3] < 230 ) ) {
		// a translucent box over the whole picture (damage flash, fade, tint): final = scene * K + B. The generated
		// picture gets the same K and B; if the guess is off, its pixels simply fail the "untouched by 2D" test and
		// are shown as the real frame has them.
		float al = backEnd.color2D[3] / 255.0f;
		int i;

		if ( what[1] == 'a' ) {
			for ( i = 0; i < 3; i++ ) {
				fg.build.fadeB[i] += backEnd.color2D[i] / 255.0f;
			}
		} else {
			fg.build.fadeK *= 1.0f - al;
			for ( i = 0; i < 3; i++ ) {
				fg.build.fadeB[i] = fg.build.fadeB[i] * ( 1.0f - al ) + backEnd.color2D[i] / 255.0f * al;
			}
		}
		return;
	}
	if ( a > fg.max2D ) {
		fg.max2D = a;
		Com_sprintf( fg.max2DName, sizeof( fg.max2DName ), "%s alpha %d", what ? what : "", (int)backEnd.color2D[3] );
	}
}

// mode 3: one thread, the generated picture goes where the real one would
static int FG_DebugPresent( void )
{
	int dbg = r_hzmFrameGenDebug->integer;
	fgFrame_t cur = fg.build;
	double V[16], P[16];
	qboolean ok;

	float max2D = fg.max2D;

	FG_ResetBuild();
	if ( fg.w != 2 ) {
		// fg_capture: this frame was written straight into a slot
		fg.slot[fg.w] = cur;
		fg.slotValid[fg.w] = cur.scene;
		{
			double o[3];
			FG_ViewOrigin( cur.V, o );
			ri.Printf( PRINT_ALL, "FGCAP slot %d %s at %.1f %.1f %.1f (biggest 2D quad %.2f of the screen: %s)\n", fg.w,
				cur.scene ? "captured" : "has NO scene", o[0], o[1], o[2], max2D, max2D > 0.0f ? fg.max2DName : "-" );
		}
		fg.w = 2;
		return 0;
	}
	if ( dbg <= 0 || !fg.slotValid[0] || !fg.slotValid[1] || !cur.scene ) {
		return 0;
	}
	if ( dbg == 2 || dbg == 4 ) {
		FG_MidCamera( &fg.slot[0], &fg.slot[1], V, P );
	} else {
		memcpy( V, cur.V, sizeof( V ) );
		memcpy( P, cur.P, sizeof( P ) );
	}
	qglFinish();
	if ( !GLimp_FG_MakeCurrent( fg.ctx ) ) {
		GLimp_FG_MakeCurrent( NULL );
		return 0;
	}
	ok = FG_CtxInit() && FG_DrawGenerated( &fg.slot[0], &fg.slot[1], &cur, V, P, dbg >= 3 );
	qglFinish();
	GLimp_FG_MakeCurrent( NULL );
	return ok ? 2 : 0;
}

int R_FG_Present( void )
{
	fgSet_t *s;
	void *oldSync = NULL;
	int i;

	FG_RecFlush();
	FG_SetMode();
	if ( !fg.mode ) {
		return 0;
	}
	s = &fg.sets[fg.w];
	FBO_FastBlit( tr.renderFbo, NULL, s->fboF, NULL, GL_COLOR_BUFFER_BIT, GL_NEAREST );
	fg.build.set = fg.w;
	fg.build.seq = ++fg.seq;
	fg.build.bypass = ( fg.max2D >= FG_BIG2D ) ? qtrue : qfalse;
	if ( fg.build.fadeK != 1.0f || fg.build.fadeB[0] + fg.build.fadeB[1] + fg.build.fadeB[2] != 0.0f ) {
		fgs.nFade++;
	}
	if ( fg.build.bypass ) {
		Com_sprintf( fgs.lastOverlay, sizeof( fgs.lastOverlay ), "%.2f %s", fg.max2D, fg.max2DName );
	}

	if ( fg.mode == 3 ) {
		return FG_DebugPresent();
	}

	if ( fg.FenceSync ) {
		fg.build.sync = fg.FenceSync( GL_SYNC_GPU_COMMANDS_COMPLETE, 0 );
		qglFlush();
	} else {
		fg.build.sync = NULL;
		qglFinish();
	}
	fg.build.tPub = SDL_GetPerformanceCounter();

	SDL_LockMutex( fg.mx );
	if ( fg.hasPending ) {
		// the present thread never got to the last one: it is dropped, its set is free again
		oldSync = fg.pending.sync;
		fgs.nDropped++;
	}
	fg.pending = fg.build;
	fg.hasPending = qtrue;
	fg.lastPubSet = fg.w;
	for ( i = 0; i < FG_SETS; i++ ) {
		if ( i != fg.heldPrev && i != fg.heldCur && i != fg.pending.set ) {
			fg.w = i;
			break;
		}
	}
	SDL_CondBroadcast( fg.cv );
	SDL_UnlockMutex( fg.mx );

	if ( oldSync ) {
		fg.DeleteSync( oldSync );
	}
	FG_ResetBuild();
	GLimp_FG_EndFrameNoSwap();
	return 1;
}

void R_FG_NotePresentOff( void )
{
	if ( r_hzmFrameGenStats && r_hzmFrameGenStats->integer ) {
		FG_Record( FGK_REAL );
	}
}

qboolean R_FG_ReadSourceBegin( void )
{
	if ( ( fg.mode != 1 && fg.mode != 2 ) || fg.lastPubSet < 0 ) {
		return qfalse;
	}
	FBO_Bind( fg.sets[fg.lastPubSet].fboF );
	return qtrue;
}

void R_FG_ReadSourceEnd( void )
{
	FBO_Bind( NULL );
}

static int FG_CmpDouble( const void *a, const void *b )
{
	double x = *(const double *)a, y = *(const double *)b;
	return x < y ? -1 : ( x > y ? 1 : 0 );
}

static void FG_Stats_f( void )
{
	static const double edges[] = { 2, 4, 6, 8, 10, 12, 14, 17, 20, 25, 33, 50 };
	static double iv[FG_TS];
	int hist[13];
	int n, cnt, i, j, first, even;
	double sum, mean, sd, med, total;
	char line[512];

	if ( ri.Cmd_Argc() > 1 && !Q_stricmp( ri.Cmd_Argv( 1 ), "reset" ) ) {
		memset( &fgs, 0, sizeof( fgs ) );
		ri.Printf( PRINT_ALL, "FGSTAT reset\n" );
		return;
	}
	n = fgs.n;
	cnt = n > FG_TS ? FG_TS : n;
	if ( cnt < 3 ) {
		ri.Printf( PRINT_ALL, "FGSTAT mode=%d presents=%d (nothing to report)\n", fg.mode, cnt );
		return;
	}
	first = n - cnt;
	sum = 0.0;
	memset( hist, 0, sizeof( hist ) );
	for ( i = 1; i < cnt; i++ ) {
		double d = FG_Ms( fgs.t[( first + i - 1 ) & ( FG_TS - 1 )], fgs.t[( first + i ) & ( FG_TS - 1 )] );
		iv[i - 1] = d;
		sum += d;
		for ( j = 0; j < 12 && d >= edges[j]; j++ ) {
		}
		hist[j]++;
	}
	cnt--;
	total = sum;
	mean = sum / cnt;
	sd = 0.0;
	for ( i = 0; i < cnt; i++ ) {
		sd += ( iv[i] - mean ) * ( iv[i] - mean );
	}
	sd = sqrt( sd / cnt );
	qsort( iv, cnt, sizeof( double ), FG_CmpDouble );
	med = iv[cnt / 2];
	even = 0;
	for ( i = 0; i < cnt; i++ ) {
		if ( iv[i] >= med * 0.75 && iv[i] <= med * 1.25 ) {
			even++;
		}
	}
	ri.Printf( PRINT_ALL, "FGSTAT mode=%d presents=%d seconds=%.2f shown_fps=%.1f real=%d gen=%d interval_ms mean=%.2f sd=%.2f min=%.2f p01=%.2f p50=%.2f p99=%.2f max=%.2f even25=%.1f%%\n",
		fg.mode, cnt + 1, total / 1000.0, cnt * 1000.0 / total, fgs.nReal, fgs.nGen,
		mean, sd, iv[0], iv[cnt / 100], med, iv[cnt - 1 - cnt / 100], iv[cnt - 1], even * 100.0 / cnt );
	ri.Printf( PRINT_ALL, "FGSTAT2 noscene=%d overlay=%d fade=%d camjump=%d dropped=%d earlyskip=%d glerr=%d synctimeout=%d syncwait_ms=%.3f gen_ms=%.3f lastoverlay=[%s]\n",
		fgs.nNoScene, fgs.nBypass, fgs.nFade, fgs.nJump, fgs.nDropped, fgs.nEarlySkip, fgs.nGLErr, fgs.nSyncTimeout,
		fgs.nSyncWait ? fgs.syncWaitMs / fgs.nSyncWait : 0.0, fgs.nGenTimed ? fgs.genMs / fgs.nGenTimed : 0.0, fgs.lastOverlay );
	line[0] = 0;
	for ( j = 0; j < 13; j++ ) {
		if ( j < 12 ) {
			Q_strcat( line, sizeof( line ), va( " <%g:%.1f%%", edges[j], hist[j] * 100.0 / cnt ) );
		} else {
			Q_strcat( line, sizeof( line ), va( " >=50:%.1f%%", hist[j] * 100.0 / cnt ) );
		}
	}
	ri.Printf( PRINT_ALL, "FGHIST%s\n", line );
}

// main thread: write out what fg_record grabbed
static void FG_RecFlush( void )
{
	int i, n, k, px;
	byte *buf;

	if ( !fgRec.done ) {
		return;
	}
	n = fgRec.count;
	px = fg.width * fg.height;
	buf = (byte *)malloc( 18 + px * 3 );
	for ( i = 0; i < n; i++ ) {
		if ( buf && fgRec.pix[i] ) {
			const byte *src = fgRec.pix[i];
			byte *dst = buf + 18;

			memset( buf, 0, 18 );
			buf[2] = 2;		// uncompressed true colour, origin bottom-left like glReadPixels
			buf[12] = fg.width & 255;
			buf[13] = fg.width >> 8;
			buf[14] = fg.height & 255;
			buf[15] = fg.height >> 8;
			buf[16] = 24;
			for ( k = 0; k < px; k++ ) {
				dst[k * 3] = src[k * 4 + 2];
				dst[k * 3 + 1] = src[k * 4 + 1];
				dst[k * 3 + 2] = src[k * 4];
			}
			ri.FS_WriteFile( va( "screenshots/fgrec_%02d_%c_%05d.tga", i, fgRec.kind[i] == FGK_GEN ? 'G' : 'R',
				(int)( FG_Ms( fgRec.t[0], fgRec.t[i] ) * 10.0 ) ), buf, 18 + px * 3 );
		}
		free( fgRec.pix[i] );
	}
	free( buf );
	memset( &fgRec, 0, sizeof( fgRec ) );
	ri.Printf( PRINT_ALL, "FGREC wrote %d pictures\n", n );
}

static void FG_Rec_f( void )
{
	int n = ri.Cmd_Argc() > 1 ? atoi( ri.Cmd_Argv( 1 ) ) : 32;

	if ( ( fg.mode != 1 && fg.mode != 2 ) || fgRec.want ) {
		ri.Printf( PRINT_ALL, "usage: fg_record <pictures>  (needs r_hzmFrameGen 1 or 2, one recording at a time)\n" );
		return;
	}
	if ( n > FG_REC_MAX ) {
		n = FG_REC_MAX;
	}
	while ( n > 1 && (double)n * fg.width * fg.height * 4.0 > 400.0 * 1024.0 * 1024.0 ) {
		n--;
	}
	fgRec.count = 0;
	fgRec.done = 0;
	fgRec.want = n > 0 ? n : 1;
}

static void FG_Capture_f( void )
{
	int n = ri.Cmd_Argc() > 1 ? atoi( ri.Cmd_Argv( 1 ) ) : -1;

	if ( fg.mode != 3 || n < 0 || n > 1 ) {
		ri.Printf( PRINT_ALL, "usage: fg_capture <0|1>  (needs r_hzmFrameGen 3)\n" );
		return;
	}
	// commands run between frames: the next frame goes straight into the slot
	fg.w = n;
}

void R_FG_Init( void )
{
	memset( &fg, 0, sizeof( fg ) );
	memset( &fgs, 0, sizeof( fgs ) );
	fg.heldPrev = fg.heldCur = fg.lastPubSet = -1;
	r_hzmFrameGen = ri.Cvar_Get( "r_hzmFrameGen", "0", 0 );
	r_hzmFrameGenDebug = ri.Cvar_Get( "r_hzmFrameGenDebug", "0", 0 );
	r_hzmFrameGenStats = ri.Cvar_Get( "r_hzmFrameGenStats", "0", 0 );
	fg.mx = SDL_CreateMutex();
	fg.cv = SDL_CreateCond();
	ri.Cmd_AddCommand( "fg_stats", FG_Stats_f );
	ri.Cmd_AddCommand( "fg_capture", FG_Capture_f );
	ri.Cmd_AddCommand( "fg_record", FG_Rec_f );
}

void R_FG_Shutdown( void )
{
	if ( !fg.mx ) {
		return;		// never initialised
	}
	FG_StopThread();
	if ( fg.ctx ) {
		// its program and vertex array die with it; the textures and framebuffers are the renderer's own
		GLimp_FG_DeleteContext( fg.ctx );
	}
	ri.Cmd_RemoveCommand( "fg_stats" );
	ri.Cmd_RemoveCommand( "fg_capture" );
	ri.Cmd_RemoveCommand( "fg_record" );
	{
		int i;
		for ( i = 0; i < FG_REC_MAX; i++ ) {
			free( fgRec.pix[i] );
		}
		memset( &fgRec, 0, sizeof( fgRec ) );
	}
	SDL_DestroyCond( fg.cv );
	SDL_DestroyMutex( fg.mx );
	memset( &fg, 0, sizeof( fg ) );
}
