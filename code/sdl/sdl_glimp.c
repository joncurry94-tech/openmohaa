/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.

This file is part of Quake III Arena source code.

Quake III Arena source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Quake III Arena source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Quake III Arena source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/

#ifdef USE_INTERNAL_SDL_HEADERS
#	include "SDL.h"
#	include "SDL_syswm.h"
#else
#	include <SDL.h>
#	include <SDL_syswm.h>
#endif

#ifdef _WIN32
#	include <windows.h>
#endif

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#include "../renderercommon/tr_common.h"
#include "../sys/sys_local.h"
#include "sdl_icon.h"

typedef enum
{
	RSERR_OK,

	RSERR_INVALID_FULLSCREEN,
	RSERR_INVALID_MODE,

	RSERR_UNKNOWN
} rserr_t;

SDL_Window *SDL_window = NULL;
static SDL_GLContext SDL_glContext = NULL;

cvar_t *r_allowSoftwareGL; // Don't abort out if a hardware visual can't be obtained
cvar_t *r_allowResize; // make window resizable
cvar_t *r_centerWindow;
// HZM test_window_policy_2026-09-27: explicit window placement for BACKGROUND TEST CLIENTS. Flags 0 (never archived,
// never in a player's config); the defaults ("-1", "", "") change nothing. SDL_WINDOWPOS_UNDEFINED centres a new
// window on display 0 (the primary), and GLimp_Shutdown destroys the window before every vid_restart, so without
// these a harness client is created on the player's screen at boot AND at every renderer restart.
cvar_t *r_windowDisplay;   // 0-based SDL display index: create the window centred on that display
cvar_t *vid_xpos;          // absolute top-left in desktop coordinates (both must be set; wins over r_windowDisplay)
cvar_t *vid_ypos;
cvar_t *r_sdlDriver;
cvar_t *r_preferOpenGLES;

int qglMajorVersion, qglMinorVersion;
int qglesMajorVersion, qglesMinorVersion;

// HZM post-FX: set true in GLimp_GetProcAddresses (fixed-function/gl1 path) when the GL 2.0 + FBO
// procs were loaded, so the gl1 GLSL post-process layer can enable. False => post-FX disabled.
qboolean glPostFxProcsLoaded = qfalse;

// HZM gl2 MSAA (plan P2a, ME-F15): set by the gl2 renderer just before GLimp_Init; -1 = use r_ext_multisample
int glimp_windowSamplesOverride = -1;

void (APIENTRYP qglActiveTextureARB) (GLenum texture);
void (APIENTRYP qglClientActiveTextureARB) (GLenum texture);
void (APIENTRYP qglMultiTexCoord2fARB) (GLenum target, GLfloat s, GLfloat t);

void (APIENTRYP qglLockArraysEXT) (GLint first, GLsizei count);
void (APIENTRYP qglUnlockArraysEXT) (void);

#define GLE(ret, name, ...) name##proc * qgl##name = NULL;
QGL_1_1_PROCS;
QGL_1_1_FIXED_FUNCTION_PROCS;
QGL_DESKTOP_1_1_PROCS;
QGL_DESKTOP_1_1_FIXED_FUNCTION_PROCS;
QGL_ES_1_1_PROCS;
QGL_ES_1_1_FIXED_FUNCTION_PROCS;
QGL_1_3_PROCS;
QGL_1_5_PROCS;
QGL_2_0_PROCS;
QGL_3_0_PROCS;
QGL_ARB_occlusion_query_PROCS;
QGL_ARB_framebuffer_object_PROCS;
QGL_ARB_vertex_array_object_PROCS;
QGL_EXT_direct_state_access_PROCS;
#undef GLE

/*
===============
GLimp_Shutdown
===============
*/
void GLimp_Shutdown( void )
{
	ri.IN_Shutdown();

	SDL_QuitSubSystem( SDL_INIT_VIDEO );
}

/*
===============
GLimp_Minimize

Minimize the game so that user is back at the desktop
===============
*/
void GLimp_Minimize( void )
{
	SDL_MinimizeWindow( SDL_window );
}


/*
===============
GLimp_LogComment
===============
*/
void GLimp_LogComment( char *comment )
{
}

/*
===============
GLimp_CompareModes
===============
*/
static int GLimp_CompareModes( const void *a, const void *b )
{
	const float ASPECT_EPSILON = 0.001f;
	SDL_Rect *modeA = (SDL_Rect *)a;
	SDL_Rect *modeB = (SDL_Rect *)b;
	float aspectA = (float)modeA->w / (float)modeA->h;
	float aspectB = (float)modeB->w / (float)modeB->h;
	int areaA = modeA->w * modeA->h;
	int areaB = modeB->w * modeB->h;
	float aspectDiffA = fabs( aspectA - displayAspect );
	float aspectDiffB = fabs( aspectB - displayAspect );
	float aspectDiffsDiff = aspectDiffA - aspectDiffB;

	if( aspectDiffsDiff > ASPECT_EPSILON )
		return 1;
	else if( aspectDiffsDiff < -ASPECT_EPSILON )
		return -1;
	else
		return areaA - areaB;
}


/*
===============
GLimp_DetectAvailableModes
===============
*/
static void GLimp_DetectAvailableModes(void)
{
	int i, j;
	char buf[ MAX_STRING_CHARS ] = { 0 };
	int numSDLModes;
	SDL_Rect *modes;
	int numModes = 0;

	SDL_DisplayMode windowMode;
	int display = SDL_GetWindowDisplayIndex( SDL_window );
	if( display < 0 )
	{
		ri.Printf( PRINT_WARNING, "Couldn't get window display index, no resolutions detected: %s\n", SDL_GetError() );
		return;
	}
	numSDLModes = SDL_GetNumDisplayModes( display );

	if( SDL_GetWindowDisplayMode( SDL_window, &windowMode ) < 0 || numSDLModes <= 0 )
	{
		ri.Printf( PRINT_WARNING, "Couldn't get window display mode, no resolutions detected: %s\n", SDL_GetError() );
		return;
	}

	modes = SDL_calloc( (size_t)numSDLModes, sizeof( SDL_Rect ) );
	if ( !modes )
	{
		ri.Error( ERR_FATAL, "Out of memory" );
	}

	for( i = 0; i < numSDLModes; i++ )
	{
		SDL_DisplayMode mode;

		if( SDL_GetDisplayMode( display, i, &mode ) < 0 )
			continue;

		if( !mode.w || !mode.h )
		{
			ri.Printf( PRINT_ALL, "Display supports any resolution\n" );
			SDL_free( modes );
			return;
		}

		if( windowMode.format != mode.format )
			continue;

		// SDL can give the same resolution with different refresh rates.
		// Only list resolution once.
		for( j = 0; j < numModes; j++ )
		{
			if( mode.w == modes[ j ].w && mode.h == modes[ j ].h )
				break;
		}

		if( j != numModes )
			continue;

		modes[ numModes ].w = mode.w;
		modes[ numModes ].h = mode.h;
		numModes++;
	}

	if( numModes > 1 )
		qsort( modes, numModes, sizeof( SDL_Rect ), GLimp_CompareModes );

	for( i = 0; i < numModes; i++ )
	{
		const char *newModeString = va( "%ux%u ", modes[ i ].w, modes[ i ].h );

		if( strlen( newModeString ) < (int)sizeof( buf ) - strlen( buf ) )
			Q_strcat( buf, sizeof( buf ), newModeString );
		else
			ri.Printf( PRINT_WARNING, "Skipping mode %ux%u, buffer too small\n", modes[ i ].w, modes[ i ].h );
	}

	if( *buf )
	{
		buf[ strlen( buf ) - 1 ] = 0;
		ri.Printf( PRINT_ALL, "Available modes: '%s'\n", buf );
		ri.Cvar_Set( "r_availableModes", buf );
	}
	SDL_free( modes );
}

/*
===============
OpenGL ES compatibility
===============
*/
static void APIENTRY GLimp_GLES_ClearDepth( GLclampd depth ) {
	qglClearDepthf( depth );
}

static void APIENTRY GLimp_GLES_DepthRange( GLclampd near_val, GLclampd far_val ) {
	qglDepthRangef( near_val, far_val );
}

static void APIENTRY GLimp_GLES_DrawBuffer( GLenum mode ) {
	// unsupported
}

static void APIENTRY GLimp_GLES_PolygonMode( GLenum face, GLenum mode ) {
	// unsupported
}

/*
===============
GLimp_GetProcAddresses

Get addresses for OpenGL functions.
===============
*/
static qboolean GLimp_GetProcAddresses( qboolean fixedFunction ) {
	qboolean success = qtrue;
	const char *version;

#ifdef __SDL_NOGETPROCADDR__
#define GLE( ret, name, ... ) qgl##name = gl#name;
#else
#define GLE( ret, name, ... ) qgl##name = (name##proc *) SDL_GL_GetProcAddress("gl" #name); \
	if ( qgl##name == NULL ) { \
		ri.Printf( PRINT_ALL, "ERROR: Missing OpenGL function %s\n", "gl" #name ); \
		success = qfalse; \
	}
#endif

	// OpenGL 1.0 and OpenGL ES 1.0
	GLE(const GLubyte *, GetString, GLenum name)

	if ( !qglGetString ) {
		Com_Error( ERR_FATAL, "glGetString is NULL" );
	}

	version = (const char *)qglGetString( GL_VERSION );

	if ( !version ) {
		Com_Error( ERR_FATAL, "GL_VERSION is NULL" );
	}

	if ( Q_stricmpn( "OpenGL ES", version, 9 ) == 0 ) {
		char profile[6]; // ES, ES-CM, or ES-CL
		sscanf( version, "OpenGL %5s %d.%d", profile, &qglesMajorVersion, &qglesMinorVersion );
		// common lite profile (no floating point) is not supported
		if ( Q_stricmp( profile, "ES-CL" ) == 0 ) {
			qglesMajorVersion = 0;
			qglesMinorVersion = 0;
		}
	} else {
		sscanf( version, "%d.%d", &qglMajorVersion, &qglMinorVersion );
	}

	if ( fixedFunction ) {
		if ( QGL_VERSION_ATLEAST( 1, 1 ) ) {
			QGL_1_1_PROCS;
			QGL_1_1_FIXED_FUNCTION_PROCS;
			QGL_DESKTOP_1_1_PROCS;
			QGL_DESKTOP_1_1_FIXED_FUNCTION_PROCS;
		} else if ( qglesMajorVersion == 1 && qglesMinorVersion >= 1 ) {
			// OpenGL ES 1.1 (2.0 is not backward compatible)
			QGL_1_1_PROCS;
			QGL_1_1_FIXED_FUNCTION_PROCS;
			QGL_ES_1_1_PROCS;
			QGL_ES_1_1_FIXED_FUNCTION_PROCS;
			// error so this doesn't segfault due to NULL desktop GL functions being used
			Com_Error( ERR_FATAL, "Unsupported OpenGL Version: %s", version );
		} else {
			Com_Error( ERR_FATAL, "Unsupported OpenGL Version (%s), OpenGL 1.1 is required", version );
		}
		// Added in OPM
		//  Add compression-related GL functions for the renderer
		if ( QGL_VERSION_ATLEAST( 1, 3 ) ) {
			QGL_1_3_PROCS;
		}
		// HZM post-FX: load GL 1.5/2.0 + FBO procs on the fixed-function (gl1) path so a GLSL
		// post-process layer can run on top of gl1. Only when the desktop context is >= 2.0;
		// otherwise post-FX auto-disables and gl1 renders exactly as before.
		{
			extern qboolean glPostFxProcsLoaded;
			glPostFxProcsLoaded = qfalse;
			if ( QGL_VERSION_ATLEAST( 2, 0 ) ) {
				QGL_1_5_PROCS;
				QGL_2_0_PROCS;
				QGL_ARB_framebuffer_object_PROCS;
				glPostFxProcsLoaded = ( qglCreateProgram != NULL && qglGenFramebuffers != NULL );
			}
		}
	} else {
		if ( QGL_VERSION_ATLEAST( 2, 0 ) ) {
			QGL_1_1_PROCS;
			QGL_DESKTOP_1_1_PROCS;
			QGL_1_3_PROCS;
			QGL_1_5_PROCS;
			QGL_2_0_PROCS;
		} else if ( QGLES_VERSION_ATLEAST( 2, 0 ) ) {
			QGL_1_1_PROCS;
			QGL_ES_1_1_PROCS;
			QGL_1_3_PROCS;
			QGL_1_5_PROCS;
			QGL_2_0_PROCS;

			qglClearDepth = GLimp_GLES_ClearDepth;
			qglDepthRange = GLimp_GLES_DepthRange;
			qglDrawBuffer = GLimp_GLES_DrawBuffer;
			qglPolygonMode = GLimp_GLES_PolygonMode;
		} else {
			Com_Error( ERR_FATAL, "Unsupported OpenGL Version (%s), OpenGL 2.0 is required", version );
		}
	}

	if ( QGL_VERSION_ATLEAST( 3, 0 ) || QGLES_VERSION_ATLEAST( 3, 0 ) ) {
		QGL_3_0_PROCS;
	}

#undef GLE

	return success;
}

/*
===============
GLimp_ClearProcAddresses

Clear addresses for OpenGL functions.
===============
*/
static void GLimp_ClearProcAddresses( void ) {
#define GLE( ret, name, ... ) qgl##name = NULL;

	qglMajorVersion = 0;
	qglMinorVersion = 0;
	qglesMajorVersion = 0;
	qglesMinorVersion = 0;

	QGL_1_1_PROCS;
	QGL_1_1_FIXED_FUNCTION_PROCS;
	QGL_DESKTOP_1_1_PROCS;
	QGL_DESKTOP_1_1_FIXED_FUNCTION_PROCS;
	QGL_ES_1_1_PROCS;
	QGL_ES_1_1_FIXED_FUNCTION_PROCS;
	QGL_1_3_PROCS;
	QGL_1_5_PROCS;
	QGL_2_0_PROCS;
	QGL_3_0_PROCS;
	QGL_ARB_occlusion_query_PROCS;
	QGL_ARB_framebuffer_object_PROCS;
	QGL_ARB_vertex_array_object_PROCS;
	QGL_EXT_direct_state_access_PROCS;

	qglActiveTextureARB = NULL;
	qglClientActiveTextureARB = NULL;
	qglMultiTexCoord2fARB = NULL;

	qglLockArraysEXT = NULL;
	qglUnlockArraysEXT = NULL;

#undef GLE
}

/*
===============
GLimp_MakeCaptureSafe

HZM [user 2026-08-13, bug-1795]: make a borderless window visible to screen capture.

THE PROBLEM. This engine presents through legacy wglSwapBuffers. When a borderless window
covers the screen exactly and vsync is off, the driver is free to promote it to independent
flip / an overlay plane and scan the buffer out directly, bypassing the desktop compositor.
Everything that captures by reading the composited desktop - OBS display capture, ShadowPlay,
SignalRGB and every other ambient-lighting tool - then samples a surface the game is no longer
drawing into, and records black. The giveaway is that menus and loading screens DO capture:
they present sporadically, so the driver never sustains the promotion, and the moment gameplay
starts presenting continuously it does.

THE FIX. A layered window cannot be promoted to an overlay plane, so DWM has to compose it.
WS_EX_LAYERED with a fully opaque alpha changes nothing the player can see - no resolution
change, no windowed mode, no vsync requirement - and hands the frames back to the compositor.

Deliberately ON by default. Borderless exists so that overlays, alt-tab and capture all work;
a borderless mode that silently defeats capture is not doing its job, and "set an undocumented
cvar" is not an answer a player will ever find. r_captureSafe 0 is the escape hatch if a driver
ever objects.
===============
*/
static void GLimp_MakeCaptureSafe( void )
{
#ifdef _WIN32
	SDL_SysWMinfo	wmInfo;
	HWND			hwnd;
	LONG_PTR		exStyle;

	if( !SDL_window ){ return; }
	if( ri.Cvar_VariableIntegerValue( "r_captureSafe" ) == 0 ){ return; }

	SDL_VERSION( &wmInfo.version );
	if( !SDL_GetWindowWMInfo( SDL_window, &wmInfo ) )
	{
		ri.Printf( PRINT_DEVELOPER, "GLimp_MakeCaptureSafe: SDL_GetWindowWMInfo failed: %s\n", SDL_GetError() );
		return;
	}
	if( wmInfo.subsystem != SDL_SYSWM_WINDOWS ){ return; }

	hwnd = wmInfo.info.win.window;
	exStyle = GetWindowLongPtr( hwnd, GWL_EXSTYLE );
	if( exStyle & WS_EX_LAYERED ){ return; }		// already layered - nothing to do

	SetWindowLongPtr( hwnd, GWL_EXSTYLE, exStyle | WS_EX_LAYERED );

	// Opaque: alpha 255 means the window looks exactly as it did. If this fails the window is
	// left layered-but-unconfigured, which CAN render it invisible - so put the style back.
	if( !SetLayeredWindowAttributes( hwnd, 0, 255, LWA_ALPHA ) )
	{
		SetWindowLongPtr( hwnd, GWL_EXSTYLE, exStyle );
		ri.Printf( PRINT_ALL, "Capture-safe borderless FAILED (SetLayeredWindowAttributes) - reverted\n" );
		return;
	}

	ri.Printf( PRINT_ALL, "Capture-safe borderless enabled (compositor-visible)\n" );
#endif
}

/*
===============
GLimp_SetMode
===============
*/
static int GLimp_SetMode(int mode, qboolean fullscreen, qboolean noborder, qboolean fixedFunction)
{
	struct GLimp_ContextType {
		int profileMask;
		int majorVersion;
		int minorVersion;
	} contexts[4];
	int numContexts, type;
	const char *glstring;
	int perChannelColorBits;
	int colorBits, depthBits, stencilBits;
	int samples;
	int i = 0;
	SDL_Surface *icon = NULL;
	Uint32 flags = SDL_WINDOW_SHOWN | SDL_WINDOW_OPENGL;
	SDL_DisplayMode desktopMode;
	int display = 0;
	int x = SDL_WINDOWPOS_UNDEFINED, y = SDL_WINDOWPOS_UNDEFINED;

	ri.Printf( PRINT_ALL, "Initializing OpenGL display\n");

	if ( r_allowResize->integer )
		flags |= SDL_WINDOW_RESIZABLE;

#ifdef USE_ICON
	icon = SDL_CreateRGBSurfaceFrom(
			(void *)CLIENT_WINDOW_ICON.pixel_data,
			CLIENT_WINDOW_ICON.width,
			CLIENT_WINDOW_ICON.height,
			CLIENT_WINDOW_ICON.bytes_per_pixel * 8,
			CLIENT_WINDOW_ICON.bytes_per_pixel * CLIENT_WINDOW_ICON.width,
#ifdef Q3_LITTLE_ENDIAN
			0x000000FF, 0x0000FF00, 0x00FF0000, 0xFF000000
#else
			0xFF000000, 0x00FF0000, 0x0000FF00, 0x000000FF
#endif
			);
#endif

	// If a window exists, note its display index
	if( SDL_window != NULL )
	{
		display = SDL_GetWindowDisplayIndex( SDL_window );
		if( display < 0 )
		{
			ri.Printf( PRINT_DEVELOPER, "SDL_GetWindowDisplayIndex() failed: %s\n", SDL_GetError() );
			display = 0;
		}
	}
	// HZM test_window_policy: a NEW window's display (for the desktop mode / aspect below) follows the placement cvars
	else if( *vid_xpos->string && *vid_ypos->string )
	{
		SDL_Point p = { vid_xpos->integer, vid_ypos->integer };
		display = SDL_GetPointDisplayIndex( &p );
		if( display < 0 )
			display = 0;
	}
	else if( r_windowDisplay->integer >= 0 && r_windowDisplay->integer < SDL_GetNumVideoDisplays( ) )
	{
		display = r_windowDisplay->integer;
	}

	if( SDL_GetDesktopDisplayMode( display, &desktopMode ) == 0 )
	{
		displayAspect = (float)desktopMode.w / (float)desktopMode.h;

		ri.Printf( PRINT_ALL, "Display aspect: %.3f\n", displayAspect );
	}
	else
	{
		Com_Memset( &desktopMode, 0, sizeof( SDL_DisplayMode ) );

		ri.Printf( PRINT_ALL,
				"Cannot determine display aspect, assuming 1.333\n" );
	}

	// HZM bug-753 addendum: FIRST-LAUNCH NATIVE RESOLUTION AUTO-DETECT. One-shot: if the archived
	// flag has never been set (fresh install, or an existing config from before this feature),
	// adopt the Windows desktop resolution of the current display as the game resolution: archive
	// r_mode -1 + r_customwidth/r_customheight = desktop size (the exact cvar set the Video
	// Options resolution picker writes), then set the flag so this NEVER re-fires and can never
	// fight a later manual pick from the list. r_mode/r_custom* are CVAR_LATCH so those Cvar_Sets
	// only latch - for THIS window we switch the local mode to -2 (= use desktop resolution
	// directly, read above from desktopMode) so the very first window is already native without
	// waiting for a vid_restart; the latched values apply/archive from then on and describe the
	// same size. Multi-monitor: 'display' is the existing window's display, or the primary (0) on
	// a true first launch. If the desktop mode query failed, skip WITHOUT setting the flag so it
	// retries next launch. Registered here (not tr_init.c) so every renderer shares it.
	{
		cvar_t *detected = ri.Cvar_Get( "r_resAutoDetected", "0", CVAR_ARCHIVE );

		if( !detected->integer && desktopMode.w > 0 && desktopMode.h > 0 )
		{
			ri.Printf( PRINT_ALL, "First launch: adopting native desktop resolution %dx%d\n",
					desktopMode.w, desktopMode.h );
			ri.Cvar_Set( "r_mode", "-1" );
			ri.Cvar_Set( "r_customwidth", va( "%d", desktopMode.w ) );
			ri.Cvar_Set( "r_customheight", va( "%d", desktopMode.h ) );
			ri.Cvar_Set( "r_resAutoDetected", "1" );
			mode = -2;
		}
	}

	ri.Printf (PRINT_ALL, "...setting mode %d:", mode );

	if (mode == -2)
	{
		// use desktop video resolution
		if( desktopMode.h > 0 )
		{
			glConfig.vidWidth = desktopMode.w;
			glConfig.vidHeight = desktopMode.h;
		}
		else
		{
			glConfig.vidWidth = 640;
			glConfig.vidHeight = 480;
			ri.Printf( PRINT_ALL,
					"Cannot determine display resolution, assuming 640x480\n" );
		}

		glConfig.windowAspect = (float)glConfig.vidWidth / (float)glConfig.vidHeight;
	}
	else if ( !R_GetModeInfo( &glConfig.vidWidth, &glConfig.vidHeight, &glConfig.windowAspect, mode ) )
	{
		ri.Printf( PRINT_ALL, " invalid mode\n" );
		return RSERR_INVALID_MODE;
	}
	ri.Printf( PRINT_ALL, " %d %d\n", glConfig.vidWidth, glConfig.vidHeight);

	// Center window
	if( r_centerWindow->integer && !fullscreen )
	{
		x = ( desktopMode.w / 2 ) - ( glConfig.vidWidth / 2 );
		y = ( desktopMode.h / 2 ) - ( glConfig.vidHeight / 2 );
	}

	// Destroy existing state if it exists
	if( SDL_glContext != NULL )
	{
		GLimp_ClearProcAddresses();
		SDL_GL_DeleteContext( SDL_glContext );
		SDL_glContext = NULL;
	}

	if( SDL_window != NULL )
	{
		SDL_GetWindowPosition( SDL_window, &x, &y );
		ri.Printf( PRINT_DEVELOPER, "Existing window at %dx%d before being destroyed\n", x, y );
		SDL_DestroyWindow( SDL_window );
		SDL_window = NULL;
	}

	if( fullscreen )
	{
		// HZM: r_desktopfullscreen 1 = "Borderless Window" (desktop-sized, no video mode change, alt-tab
		// friendly); 0 = "Exclusive Fullscreen" (real mode change). Both report isFullscreen so input grab
		// behaves the same. FULLSCREEN_DESKTOP includes the FULLSCREEN bit, so existing flag tests still match.
		if( ri.Cvar_VariableIntegerValue( "r_desktopfullscreen" ) )
		{
			// HZM bug-753: SDL always sizes a FULLSCREEN_DESKTOP window to the DESKTOP and ignores the
			// requested w/h (and SDL_SetWindowDisplayMode), so a resolution picked in the Video Options
			// while in Borderless was silently ignored - worse, glConfig kept the picked size while the
			// real drawable was desktop-sized (corner-rendered picture + misaligned mouse). Borderless now
			// honors the picked size: at the desktop size use true FULLSCREEN_DESKTOP (canonical borderless
			// fullscreen); at any OTHER size open a centered BORDERLESS window of exactly the picked size.
			if( desktopMode.h > 0 &&
				( glConfig.vidWidth != desktopMode.w || glConfig.vidHeight != desktopMode.h ) )
			{
				flags |= SDL_WINDOW_BORDERLESS;
				x = ( desktopMode.w / 2 ) - ( glConfig.vidWidth / 2 );
				y = ( desktopMode.h / 2 ) - ( glConfig.vidHeight / 2 );
				glConfig.isFullscreen = qfalse;
			}
			else
			{
				flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
				glConfig.isFullscreen = qtrue;
			}
		}
		else
		{
			flags |= SDL_WINDOW_FULLSCREEN;
			glConfig.isFullscreen = qtrue;
		}
	}
	else
	{
		if( noborder )
			flags |= SDL_WINDOW_BORDERLESS;

		glConfig.isFullscreen = qfalse;
	}

	// HZM test_window_policy: explicit placement, applied last so it also covers borderless/fullscreen and a
	// vid_restart. Unset (the default) leaves x/y exactly as computed above.
	if( *vid_xpos->string && *vid_ypos->string )
	{
		x = vid_xpos->integer;
		y = vid_ypos->integer;
	}
	else if( r_windowDisplay->integer >= 0 && r_windowDisplay->integer < SDL_GetNumVideoDisplays( ) )
	{
		x = SDL_WINDOWPOS_CENTERED_DISPLAY( r_windowDisplay->integer );
		y = SDL_WINDOWPOS_CENTERED_DISPLAY( r_windowDisplay->integer );
	}

	colorBits = r_colorbits->value;
	if ((!colorBits) || (colorBits >= 32))
		colorBits = 24;

	if (!r_depthbits->value)
		depthBits = 24;
	else
		depthBits = r_depthbits->value;

	stencilBits = r_stencilbits->value;
	samples = r_ext_multisample->value;
	// HZM gl2 MSAA (plan P2a, ME-F15): the new path renders the scene into multisample textures and presents a
	// single-sample image, so it asks for a single-sample window whatever r_ext_multisample says
	if ( glimp_windowSamplesOverride >= 0 )
		samples = glimp_windowSamplesOverride;

	numContexts = 0;

	if ( !fixedFunction ) {
		int profileMask;
		qboolean preferOpenGLES;

		SDL_GL_ResetAttributes();
		SDL_GL_GetAttribute( SDL_GL_CONTEXT_PROFILE_MASK, &profileMask );

		preferOpenGLES = ( r_preferOpenGLES->integer == 1 ||
		                 ( r_preferOpenGLES->integer == -1 && profileMask == SDL_GL_CONTEXT_PROFILE_ES ) );

		if ( preferOpenGLES ) {
#ifdef __EMSCRIPTEN__
			// WebGL 2.0 isn't fully backward compatible so you have to ask for it specifically
			contexts[numContexts].profileMask = SDL_GL_CONTEXT_PROFILE_ES;
			contexts[numContexts].majorVersion = 3;
			contexts[numContexts].minorVersion = 0;
			numContexts++;
#endif

			contexts[numContexts].profileMask = SDL_GL_CONTEXT_PROFILE_ES;
			contexts[numContexts].majorVersion = 2;
			contexts[numContexts].minorVersion = 0;
			numContexts++;
		}

		contexts[numContexts].profileMask = SDL_GL_CONTEXT_PROFILE_CORE;
		contexts[numContexts].majorVersion = 3;
		contexts[numContexts].minorVersion = 2;
		numContexts++;

		contexts[numContexts].profileMask = 0;
		contexts[numContexts].majorVersion = 2;
		contexts[numContexts].minorVersion = 0;
		numContexts++;

		if ( !preferOpenGLES ) {
#ifdef __EMSCRIPTEN__
			contexts[numContexts].profileMask = SDL_GL_CONTEXT_PROFILE_ES;
			contexts[numContexts].majorVersion = 3;
			contexts[numContexts].minorVersion = 0;
			numContexts++;
#endif

			contexts[numContexts].profileMask = SDL_GL_CONTEXT_PROFILE_ES;
			contexts[numContexts].majorVersion = 2;
			contexts[numContexts].minorVersion = 0;
			numContexts++;
		}
	} else {
		contexts[numContexts].profileMask = 0;
		contexts[numContexts].majorVersion = 1;
		contexts[numContexts].minorVersion = 1;
		numContexts++;
	}

	// HZM gl2 graphics probes (MSAA/shadow plan): r_glDebug 1 asks for a DEBUG context so KHR_debug reports
	// errors synchronously (tr_gfxprobe.c installs the callback). Must be set before SDL_CreateWindow and after
	// SDL_GL_ResetAttributes above. Read by name, so this shared file adds no link dependency on either renderer.
	if ( !fixedFunction && ri.Cvar_VariableIntegerValue( "r_glDebug" ) )
	{
		SDL_GL_SetAttribute( SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_DEBUG_FLAG );
	}

	for (i = 0; i < 16; i++)
	{
		int testColorBits, testDepthBits, testStencilBits;
		int realColorBits[3];

		// 0 - default
		// 1 - minus colorBits
		// 2 - minus depthBits
		// 3 - minus stencil
		if ((i % 4) == 0 && i)
		{
			// one pass, reduce
			switch (i / 4)
			{
				case 2 :
					if (colorBits == 24)
						colorBits = 16;
					break;
				case 1 :
					if (depthBits == 24)
						depthBits = 16;
					else if (depthBits == 16)
						depthBits = 8;
				case 3 :
					if (stencilBits == 24)
						stencilBits = 16;
					else if (stencilBits == 16)
						stencilBits = 8;
			}
		}

		testColorBits = colorBits;
		testDepthBits = depthBits;
		testStencilBits = stencilBits;

		if ((i % 4) == 3)
		{ // reduce colorBits
			if (testColorBits == 24)
				testColorBits = 16;
		}

		if ((i % 4) == 2)
		{ // reduce depthBits
			if (testDepthBits == 24)
				testDepthBits = 16;
			else if (testDepthBits == 16)
				testDepthBits = 8;
		}

		if ((i % 4) == 1)
		{ // reduce stencilBits
			if (testStencilBits == 24)
				testStencilBits = 16;
			else if (testStencilBits == 16)
				testStencilBits = 8;
			else
				testStencilBits = 0;
		}

		if (testColorBits == 24)
			perChannelColorBits = 8;
		else
			perChannelColorBits = 4;

#ifdef __sgi /* Fix for SGIs grabbing too many bits of color */
		if (perChannelColorBits == 4)
			perChannelColorBits = 0; /* Use minimum size for 16-bit color */

		/* Need alpha or else SGIs choose 36+ bit RGB mode */
		SDL_GL_SetAttribute( SDL_GL_ALPHA_SIZE, 1);
#endif

		SDL_GL_SetAttribute( SDL_GL_RED_SIZE, perChannelColorBits );
		SDL_GL_SetAttribute( SDL_GL_GREEN_SIZE, perChannelColorBits );
		SDL_GL_SetAttribute( SDL_GL_BLUE_SIZE, perChannelColorBits );
		SDL_GL_SetAttribute( SDL_GL_DEPTH_SIZE, testDepthBits );
		SDL_GL_SetAttribute( SDL_GL_STENCIL_SIZE, testStencilBits );

		SDL_GL_SetAttribute( SDL_GL_MULTISAMPLEBUFFERS, samples ? 1 : 0 );
		SDL_GL_SetAttribute( SDL_GL_MULTISAMPLESAMPLES, samples );

		if(r_stereoEnabled->integer)
		{
			glConfig.stereoEnabled = qtrue;
			SDL_GL_SetAttribute(SDL_GL_STEREO, 1);
		}
		else
		{
			glConfig.stereoEnabled = qfalse;
			SDL_GL_SetAttribute(SDL_GL_STEREO, 0);
		}
		
		SDL_GL_SetAttribute( SDL_GL_DOUBLEBUFFER, 1 );

#if 0 // if multisampling is enabled on X11, this causes create window to fail.
		// If not allowing software GL, demand accelerated
		if( !r_allowSoftwareGL->integer )
			SDL_GL_SetAttribute( SDL_GL_ACCELERATED_VISUAL, 1 );
#endif

		if( ( SDL_window = SDL_CreateWindow( CLIENT_WINDOW_TITLE, x, y,
				glConfig.vidWidth, glConfig.vidHeight, flags ) ) == NULL )
		{
			ri.Printf( PRINT_DEVELOPER, "SDL_CreateWindow failed: %s\n", SDL_GetError( ) );
			continue;
		}

		if( fullscreen )
		{
			SDL_DisplayMode mode;

			switch( testColorBits )
			{
				case 16: mode.format = SDL_PIXELFORMAT_RGB565; break;
				case 24: mode.format = SDL_PIXELFORMAT_RGB24;  break;
				default: ri.Printf( PRINT_DEVELOPER, "testColorBits is %d, can't fullscreen\n", testColorBits ); continue;
			}

			mode.w = glConfig.vidWidth;
			mode.h = glConfig.vidHeight;
			mode.refresh_rate = glConfig.displayFrequency = ri.Cvar_VariableIntegerValue( "r_displayRefresh" );
			mode.driverdata = NULL;

			if( SDL_SetWindowDisplayMode( SDL_window, &mode ) < 0 )
			{
				ri.Printf( PRINT_DEVELOPER, "SDL_SetWindowDisplayMode failed: %s\n", SDL_GetError( ) );
				continue;
			}
		}

		SDL_SetWindowIcon( SDL_window, icon );

		for ( type = 0; type < numContexts; type++ ) {
			char contextName[32];

			switch ( contexts[type].profileMask ) {
				default:
				case 0:
					Com_sprintf( contextName, sizeof( contextName ), "OpenGL %d.%d",
					             contexts[type].majorVersion, contexts[type].minorVersion );
					break;
				case SDL_GL_CONTEXT_PROFILE_CORE:
					Com_sprintf( contextName, sizeof( contextName ), "OpenGL %d.%d Core",
					             contexts[type].majorVersion, contexts[type].minorVersion );
					break;
				case SDL_GL_CONTEXT_PROFILE_ES:
					Com_sprintf( contextName, sizeof( contextName ), "OpenGL ES %d.%d",
					             contexts[type].majorVersion, contexts[type].minorVersion );
					break;
			}

			SDL_GL_SetAttribute( SDL_GL_CONTEXT_PROFILE_MASK, contexts[type].profileMask );
			SDL_GL_SetAttribute( SDL_GL_CONTEXT_MAJOR_VERSION, contexts[type].majorVersion );
			SDL_GL_SetAttribute( SDL_GL_CONTEXT_MINOR_VERSION, contexts[type].minorVersion );

			SDL_glContext = SDL_GL_CreateContext( SDL_window );
			if ( !SDL_glContext )
			{
				ri.Printf( PRINT_ALL, "SDL_GL_CreateContext() for %s context failed: %s\n", contextName, SDL_GetError() );
				continue;
			}

			if ( !GLimp_GetProcAddresses( fixedFunction ) )
			{
				ri.Printf( PRINT_ALL, "GLimp_GetProcAddresses() for %s context failed\n", contextName );
				GLimp_ClearProcAddresses();
				SDL_GL_DeleteContext( SDL_glContext );
				SDL_glContext = NULL;
				continue;
			}

			if ( contexts[type].profileMask == SDL_GL_CONTEXT_PROFILE_CORE ) {
				const char *renderer;

				renderer = (const char *)qglGetString( GL_RENDERER );

				if ( !renderer || strstr( renderer, "Software Renderer" ) || strstr( renderer, "Software Rasterizer" ) )
				{
					ri.Printf( PRINT_ALL, "GL_RENDERER is %s, rejecting %s context\n", renderer, contextName );

					GLimp_ClearProcAddresses();
					SDL_GL_DeleteContext( SDL_glContext );
					SDL_glContext = NULL;
					continue;
				}
			}

			break;
		}

		if ( !SDL_glContext ) {
			SDL_DestroyWindow( SDL_window );
			SDL_window = NULL;
			continue;
		}

		qglClearColor( 0, 0, 0, 1 );
		qglClear( GL_COLOR_BUFFER_BIT );
		SDL_GL_SwapWindow( SDL_window );

		if( SDL_GL_SetSwapInterval( r_swapInterval->integer ) == -1 )
		{
			ri.Printf( PRINT_DEVELOPER, "SDL_GL_SetSwapInterval failed: %s\n", SDL_GetError( ) );
		}

		// HZM gl2 graphics probes (plan V3-B2): publish the display's real refresh and whether vsync is really
		// live. glConfig.displayFrequency is only set fullscreen from r_displayRefresh, and r_swapInterval is a
		// latched request, so neither tells the fps label its true cap. ROM, written at every window creation.
		{
			SDL_DisplayMode hzmMode;
			int hzmHz = 0;
			int hzmDisplay = SDL_GetWindowDisplayIndex( SDL_window );

			if( hzmDisplay >= 0 && SDL_GetCurrentDisplayMode( hzmDisplay, &hzmMode ) == 0 )
				hzmHz = hzmMode.refresh_rate;
			if( hzmHz <= 0 && SDL_GetWindowDisplayMode( SDL_window, &hzmMode ) == 0 )
				hzmHz = hzmMode.refresh_rate;
			if( hzmHz <= 0 && hzmDisplay >= 0 && SDL_GetDesktopDisplayMode( hzmDisplay, &hzmMode ) == 0 )
				hzmHz = hzmMode.refresh_rate;

			ri.Cvar_Get( "r_displayHz", "0", CVAR_ROM );
			ri.Cvar_Set( "r_displayHz", va( "%d", hzmHz ) );
			ri.Cvar_Get( "r_vsyncActive", "0", CVAR_ROM );
			ri.Cvar_Set( "r_vsyncActive", SDL_GL_GetSwapInterval() != 0 ? "1" : "0" );
		}

		SDL_GL_GetAttribute( SDL_GL_RED_SIZE, &realColorBits[0] );
		SDL_GL_GetAttribute( SDL_GL_GREEN_SIZE, &realColorBits[1] );
		SDL_GL_GetAttribute( SDL_GL_BLUE_SIZE, &realColorBits[2] );
		SDL_GL_GetAttribute( SDL_GL_DEPTH_SIZE, &glConfig.depthBits );
		SDL_GL_GetAttribute( SDL_GL_STENCIL_SIZE, &glConfig.stencilBits );

		glConfig.colorBits = realColorBits[0] + realColorBits[1] + realColorBits[2];

		ri.Printf( PRINT_ALL, "Using %d color bits, %d depth, %d stencil display.\n",
				glConfig.colorBits, glConfig.depthBits, glConfig.stencilBits );
		break;
	}

	SDL_FreeSurface( icon );

	if( !SDL_window )
	{
		ri.Printf( PRINT_ALL, "Couldn't get a visual\n" );
		return RSERR_INVALID_MODE;
	}

	// HZM [user 2026-08-13, bug-1795]: borderless is supposed to be the capture-friendly mode - that is
	// the whole reason it exists - so it makes itself capture-safe rather than asking the player to
	// discover a driver setting. Exclusive fullscreen is deliberately skipped: it bypasses the
	// compositor by definition, and layering a window that owns a real display mode is asking for
	// trouble for no gain.
	if( fullscreen && ri.Cvar_VariableIntegerValue( "r_desktopfullscreen" ) )
	{
		GLimp_MakeCaptureSafe();
	}

	GLimp_DetectAvailableModes();

	glstring = (char *) qglGetString (GL_RENDERER);
	ri.Printf( PRINT_ALL, "GL_RENDERER: %s\n", glstring );

	return RSERR_OK;
}

/*
===============
GLimp_StartDriverAndSetMode
===============
*/
static qboolean GLimp_StartDriverAndSetMode(int mode, qboolean fullscreen, qboolean noborder, qboolean gl3Core)
{
	rserr_t err;

	if (!SDL_WasInit(SDL_INIT_VIDEO))
	{
		const char *driverName;

		if (SDL_Init(SDL_INIT_VIDEO) != 0)
		{
			ri.Printf( PRINT_ALL, "SDL_Init( SDL_INIT_VIDEO ) FAILED (%s)\n", SDL_GetError());
			return qfalse;
		}

		driverName = SDL_GetCurrentVideoDriver( );
		ri.Printf( PRINT_ALL, "SDL using driver \"%s\"\n", driverName );
		ri.Cvar_Set( "r_sdlDriver", driverName );
	}

	if (fullscreen && ri.Cvar_VariableIntegerValue( "in_nograb" ) )
	{
		ri.Printf( PRINT_ALL, "Fullscreen not allowed with in_nograb 1\n");
		ri.Cvar_Set( "r_fullscreen", "0" );
		r_fullscreen->modified = qfalse;
		fullscreen = qfalse;
	}
	
	err = GLimp_SetMode(mode, fullscreen, noborder, gl3Core);

	switch ( err )
	{
		case RSERR_INVALID_FULLSCREEN:
			ri.Printf( PRINT_ALL, "...WARNING: fullscreen unavailable in this mode\n" );
			return qfalse;
		case RSERR_INVALID_MODE:
			ri.Printf( PRINT_ALL, "...WARNING: could not set the given mode (%d)\n", mode );
			return qfalse;
		default:
			break;
	}

	return qtrue;
}


/*
===============
GLimp_InitExtensions
===============
*/
static void GLimp_InitExtensions( qboolean fixedFunction )
{
	if ( !r_allowExtensions->integer )
	{
		ri.Printf( PRINT_ALL, "* IGNORING OPENGL EXTENSIONS *\n" );
		return;
	}

	ri.Printf( PRINT_ALL, "Initializing OpenGL extensions\n" );

	glConfig.textureCompression = TC_NONE;

	// GL_EXT_texture_compression_s3tc
	if ( ( QGLES_VERSION_ATLEAST( 2, 0 ) || SDL_GL_ExtensionSupported( "GL_ARB_texture_compression" ) ) &&
	     SDL_GL_ExtensionSupported( "GL_EXT_texture_compression_s3tc" ) )
	{
		if ( r_ext_compressed_textures->value )
		{
			glConfig.textureCompression = TC_S3TC_ARB;
			ri.Printf( PRINT_ALL, "...using GL_EXT_texture_compression_s3tc\n" );
		}
		else
		{
			ri.Printf( PRINT_ALL, "...ignoring GL_EXT_texture_compression_s3tc\n" );
		}
	}
	else
	{
		ri.Printf( PRINT_ALL, "...GL_EXT_texture_compression_s3tc not found\n" );
	}

	// GL_S3_s3tc ... legacy extension before GL_EXT_texture_compression_s3tc.
	if (glConfig.textureCompression == TC_NONE)
	{
		if ( SDL_GL_ExtensionSupported( "GL_S3_s3tc" ) )
		{
			if ( r_ext_compressed_textures->value )
			{
				glConfig.textureCompression = TC_S3TC;
				ri.Printf( PRINT_ALL, "...using GL_S3_s3tc\n" );
			}
			else
			{
				ri.Printf( PRINT_ALL, "...ignoring GL_S3_s3tc\n" );
			}
		}
		else
		{
			ri.Printf( PRINT_ALL, "...GL_S3_s3tc not found\n" );
		}
	}

	// OpenGL 1 fixed function pipeline
	if ( fixedFunction )
	{
		// GL_EXT_texture_env_add
		glConfig.textureEnvAddAvailable = qfalse;
		if ( SDL_GL_ExtensionSupported( "GL_EXT_texture_env_add" ) )
		{
			if ( r_ext_texture_env_add->integer )
			{
				glConfig.textureEnvAddAvailable = qtrue;
				ri.Printf( PRINT_ALL, "...using GL_EXT_texture_env_add\n" );
			}
			else
			{
				glConfig.textureEnvAddAvailable = qfalse;
				ri.Printf( PRINT_ALL, "...ignoring GL_EXT_texture_env_add\n" );
			}
		}
		else
		{
			ri.Printf( PRINT_ALL, "...GL_EXT_texture_env_add not found\n" );
		}

		// GL_ARB_multitexture
		qglMultiTexCoord2fARB = NULL;
		qglActiveTextureARB = NULL;
		qglClientActiveTextureARB = NULL;
		if ( SDL_GL_ExtensionSupported( "GL_ARB_multitexture" ) )
		{
			if ( r_ext_multitexture->value )
			{
				qglMultiTexCoord2fARB = SDL_GL_GetProcAddress( "glMultiTexCoord2fARB" );
				qglActiveTextureARB = SDL_GL_GetProcAddress( "glActiveTextureARB" );
				qglClientActiveTextureARB = SDL_GL_GetProcAddress( "glClientActiveTextureARB" );

				if ( qglActiveTextureARB )
				{
					GLint glint = 0;
					qglGetIntegerv( GL_MAX_TEXTURE_UNITS_ARB, &glint );
					glConfig.numTextureUnits = (int) glint;
					if ( glConfig.numTextureUnits > 1 )
					{
						ri.Printf( PRINT_ALL, "...using GL_ARB_multitexture\n" );
					}
					else
					{
						qglMultiTexCoord2fARB = NULL;
						qglActiveTextureARB = NULL;
						qglClientActiveTextureARB = NULL;
						ri.Printf( PRINT_ALL, "...not using GL_ARB_multitexture, < 2 texture units\n" );
					}
				}
			}
			else
			{
				ri.Printf( PRINT_ALL, "...ignoring GL_ARB_multitexture\n" );
			}
		}
		else
		{
			ri.Printf( PRINT_ALL, "...GL_ARB_multitexture not found\n" );
		}

		// GL_EXT_compiled_vertex_array
		if ( SDL_GL_ExtensionSupported( "GL_EXT_compiled_vertex_array" ) )
		{
			if ( r_ext_compiled_vertex_array->value )
			{
				ri.Printf( PRINT_ALL, "...using GL_EXT_compiled_vertex_array\n" );
				qglLockArraysEXT = ( void ( APIENTRY * )( GLint, GLint ) ) SDL_GL_GetProcAddress( "glLockArraysEXT" );
				qglUnlockArraysEXT = ( void ( APIENTRY * )( void ) ) SDL_GL_GetProcAddress( "glUnlockArraysEXT" );
				if (!qglLockArraysEXT || !qglUnlockArraysEXT)
				{
					ri.Error (ERR_FATAL, "bad getprocaddress");
				}
			}
			else
			{
				ri.Printf( PRINT_ALL, "...ignoring GL_EXT_compiled_vertex_array\n" );
			}
		}
		else
		{
			ri.Printf( PRINT_ALL, "...GL_EXT_compiled_vertex_array not found\n" );
		}
	}

	textureFilterAnisotropic = qfalse;
	if ( SDL_GL_ExtensionSupported( "GL_EXT_texture_filter_anisotropic" ) )
	{
		if ( r_ext_texture_filter_anisotropic->integer ) {
			qglGetIntegerv( GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, (GLint *)&maxAnisotropy );
			if ( maxAnisotropy <= 0 ) {
				ri.Printf( PRINT_ALL, "...GL_EXT_texture_filter_anisotropic not properly supported!\n" );
				maxAnisotropy = 0;
			}
			else
			{
				ri.Printf( PRINT_ALL, "...using GL_EXT_texture_filter_anisotropic (max: %i)\n", maxAnisotropy );
				textureFilterAnisotropic = qtrue;
			}
		}
		else
		{
			ri.Printf( PRINT_ALL, "...ignoring GL_EXT_texture_filter_anisotropic\n" );
		}
	}
	else
	{
		ri.Printf( PRINT_ALL, "...GL_EXT_texture_filter_anisotropic not found\n" );
	}

	haveClampToEdge = qfalse;
	if ( QGL_VERSION_ATLEAST( 1, 2 ) || QGLES_VERSION_ATLEAST( 1, 0 ) || SDL_GL_ExtensionSupported( "GL_SGIS_texture_edge_clamp" ) )
	{
		ri.Printf( PRINT_ALL, "...using GL_SGIS_texture_edge_clamp\n" );
		haveClampToEdge = qtrue;
	}
	else
	{
		ri.Printf( PRINT_ALL, "...GL_SGIS_texture_edge_clamp not found\n" );
	}
}

#define R_MODE_FALLBACK 3 // 640 * 480

/*
===============
GLimp_Init

This routine is responsible for initializing the OS specific portions
of OpenGL
===============
*/
void GLimp_Init( qboolean fixedFunction )
{
	ri.Printf( PRINT_DEVELOPER, "Glimp_Init( )\n" );

	r_allowSoftwareGL = ri.Cvar_Get( "r_allowSoftwareGL", "0", CVAR_LATCH );
	r_sdlDriver = ri.Cvar_Get( "r_sdlDriver", "", CVAR_ROM );
	r_allowResize = ri.Cvar_Get( "r_allowResize", "0", CVAR_ARCHIVE | CVAR_LATCH );
	r_centerWindow = ri.Cvar_Get( "r_centerWindow", "0", CVAR_ARCHIVE | CVAR_LATCH );
	r_windowDisplay = ri.Cvar_Get( "r_windowDisplay", "-1", 0 );   // HZM test_window_policy (see the declaration)
	vid_xpos = ri.Cvar_Get( "vid_xpos", "", 0 );
	vid_ypos = ri.Cvar_Get( "vid_ypos", "", 0 );
	r_preferOpenGLES = ri.Cvar_Get( "r_preferOpenGLES", "-1", CVAR_ARCHIVE | CVAR_LATCH );
	// HZM [bug-1795] registered HERE rather than in a renderer's tr_init.c because this file is the
	// only consumer and it is linked into BOTH renderers - r_desktopfullscreen is registered by gl1
	// alone (renderergl1/tr_init.c), which is exactly how it ended up unregistered under the gl2 build
	// that actually ships. One registration, both renderers, no gap to rediscover later.
	ri.Cvar_Get( "r_captureSafe", "1", CVAR_ARCHIVE | CVAR_LATCH );

	if( ri.Cvar_VariableIntegerValue( "com_abnormalExit" ) )
	{
		ri.Cvar_Set( "r_mode", va( "%d", R_MODE_FALLBACK ) );
		ri.Cvar_Set( "r_fullscreen", "0" );
		ri.Cvar_Set( "r_centerWindow", "0" );
		ri.Cvar_Set( "com_abnormalExit", "0" );
	}

	ri.Sys_GLimpInit( );

	// Create the window and set up the context
	if(GLimp_StartDriverAndSetMode(r_mode->integer, r_fullscreen->integer, r_noborder->integer, fixedFunction))
		goto success;

	// Try again, this time in a platform specific "safe mode"
	ri.Sys_GLimpSafeInit( );

	if(GLimp_StartDriverAndSetMode(r_mode->integer, r_fullscreen->integer, qfalse, fixedFunction))
		goto success;

	// Finally, try the default screen resolution
	if( r_mode->integer != R_MODE_FALLBACK )
	{
		ri.Printf( PRINT_ALL, "Setting r_mode %d failed, falling back on r_mode %d\n",
				r_mode->integer, R_MODE_FALLBACK );

		if(GLimp_StartDriverAndSetMode(R_MODE_FALLBACK, qfalse, qfalse, fixedFunction))
			goto success;
	}

	// Nothing worked, give up
	ri.Error( ERR_FATAL, "GLimp_Init() - could not load OpenGL subsystem" );

success:
	// These values force the UI to disable driver selection
	glConfig.driverType = GLDRV_ICD;
	glConfig.hardwareType = GLHW_GENERIC;

	// Only using SDL_SetWindowBrightness to determine if hardware gamma is supported
	glConfig.deviceSupportsGamma = !r_ignorehwgamma->integer &&
		SDL_SetWindowBrightness( SDL_window, 1.0f ) >= 0;

	// get our config strings
	Q_strncpyz( glConfig.vendor_string, (char *) qglGetString (GL_VENDOR), sizeof( glConfig.vendor_string ) );
	Q_strncpyz( glConfig.renderer_string, (char *) qglGetString (GL_RENDERER), sizeof( glConfig.renderer_string ) );
	if (*glConfig.renderer_string && glConfig.renderer_string[strlen(glConfig.renderer_string) - 1] == '\n')
		glConfig.renderer_string[strlen(glConfig.renderer_string) - 1] = 0;
	Q_strncpyz( glConfig.version_string, (char *) qglGetString (GL_VERSION), sizeof( glConfig.version_string ) );

	// manually create extension list if using OpenGL 3
	if ( qglGetStringi )
	{
		int i, numExtensions, extensionLength, listLength;
		const char *extension;

		qglGetIntegerv( GL_NUM_EXTENSIONS, &numExtensions );
		listLength = 0;

		for ( i = 0; i < numExtensions; i++ )
		{
			extension = (char *) qglGetStringi( GL_EXTENSIONS, i );
			extensionLength = strlen( extension );

			if ( ( listLength + extensionLength + 1 ) >= sizeof( glConfig.extensions_string ) )
				break;

			if ( i > 0 ) {
				Q_strcat( glConfig.extensions_string, sizeof( glConfig.extensions_string ), " " );
				listLength++;
			}

			Q_strcat( glConfig.extensions_string, sizeof( glConfig.extensions_string ), extension );
			listLength += extensionLength;
		}
	}
	else
	{
		Q_strncpyz( glConfig.extensions_string, (char *) qglGetString (GL_EXTENSIONS), sizeof( glConfig.extensions_string ) );
	}

	// initialize extensions
	GLimp_InitExtensions( fixedFunction );

	ri.Cvar_Get( "r_availableModes", "", CVAR_ROM );

	// This depends on SDL_INIT_VIDEO, hence having it here
	ri.IN_Init( SDL_window );
}


/*
===============
GLimp_EndFrame

Responsible for doing a swapbuffers
===============
*/
static void GLimp_EndFrameEx( qboolean doSwap )
{
	// don't flip if drawing to front buffer
	if ( doSwap && Q_stricmp( r_drawBuffer->string, "GL_FRONT" ) != 0 )
	{
		SDL_GL_SwapWindow( SDL_window );
	}

	if( r_fullscreen->modified )
	{
		int         fullscreen;
		qboolean    needToToggle;
		qboolean    sdlToggled = qfalse;

		// Find out the current state
		fullscreen = !!( SDL_GetWindowFlags( SDL_window ) & SDL_WINDOW_FULLSCREEN );

		if( r_fullscreen->integer && ri.Cvar_VariableIntegerValue( "in_nograb" ) )
		{
			ri.Printf( PRINT_ALL, "Fullscreen not allowed with in_nograb 1\n");
			ri.Cvar_Set( "r_fullscreen", "0" );
			r_fullscreen->modified = qfalse;
		}

		// Is the state we want different from the current state?
		needToToggle = !!r_fullscreen->integer != fullscreen;

		// HZM bug-753: Borderless mode at a non-native picked resolution is deliberately a plain
		// BORDERLESS window (no SDL fullscreen flag) sized to the pick - see GLimp_SetMode. Don't
		// "correct" it back to FULLSCREEN_DESKTOP here or the picked size would be desktop-sized again.
		if( r_fullscreen->integer && ri.Cvar_VariableIntegerValue( "r_desktopfullscreen" )
			&& !glConfig.isFullscreen )
		{
			needToToggle = qfalse;
		}

		if( needToToggle )
		{
			{
				Uint32 fsFlag = r_fullscreen->integer
					? ( ri.Cvar_VariableIntegerValue( "r_desktopfullscreen" ) ? SDL_WINDOW_FULLSCREEN_DESKTOP : SDL_WINDOW_FULLSCREEN )
					: 0;
				sdlToggled = SDL_SetWindowFullscreen( SDL_window, fsFlag ) >= 0;
			}

			// SDL_WM_ToggleFullScreen didn't work, so do it the slow way
			if( !sdlToggled )
				ri.Cmd_ExecuteText(EXEC_APPEND, "vid_restart\n");

			ri.IN_Restart( );
		}

		r_fullscreen->modified = qfalse;
	}
}

void GLimp_EndFrame( void )
{
	GLimp_EndFrameEx( qtrue );
}

/*
===============
HZM frame generation (r_hzmFrameGen, renderergl2/tr_hzm_framegen.c)

A second GL context that SHARES objects with the main one, for the present thread. Only these few entry points
know about SDL's window and contexts; everything else lives in the renderer.
===============
*/
// the main thread's end-of-frame housekeeping (the r_fullscreen toggle) without the swap: the present thread swaps
void GLimp_FG_EndFrameNoSwap( void )
{
	GLimp_EndFrameEx( qfalse );
}

// main thread, main context current. Returns the new context (NULL on failure); the main context is current again on return.
void *GLimp_FG_CreateContext( void )
{
	SDL_GLContext ctx;

	if ( !SDL_window || !SDL_glContext ) {
		return NULL;
	}
	SDL_GL_SetAttribute( SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1 );
	ctx = SDL_GL_CreateContext( SDL_window );
	SDL_GL_SetAttribute( SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 0 );
	if ( !ctx ) {
		ri.Printf( PRINT_WARNING, "GLimp_FG_CreateContext: %s\n", SDL_GetError() );
	}
	// creating a context makes it current: hand the thread back to the main one
	if ( SDL_GL_MakeCurrent( SDL_window, SDL_glContext ) != 0 ) {
		ri.Printf( PRINT_WARNING, "GLimp_FG_CreateContext: main context not restored: %s\n", SDL_GetError() );
	}
	return (void *)ctx;
}

void GLimp_FG_DeleteContext( void *ctx )
{
	if ( ctx ) {
		SDL_GL_DeleteContext( (SDL_GLContext)ctx );
	}
}

// ctx NULL = the main context (main thread only). Any thread may make the frame-gen context current on itself,
// one thread at a time. Returns qfalse on failure.
qboolean GLimp_FG_MakeCurrent( void *ctx )
{
	if ( !SDL_window ) {
		return qfalse;
	}
	return SDL_GL_MakeCurrent( SDL_window, ctx ? (SDL_GLContext)ctx : SDL_glContext ) == 0 ? qtrue : qfalse;
}

// the calling thread lets go of whatever context it holds (the present thread, before it exits)
void GLimp_FG_ReleaseCurrent( void )
{
	if ( SDL_window ) {
		SDL_GL_MakeCurrent( SDL_window, NULL );
	}
}

void GLimp_FG_Swap( void )
{
	if ( SDL_window ) {
		SDL_GL_SwapWindow( SDL_window );
	}
}

void GLimp_FG_SetSwapInterval( int interval )
{
	SDL_GL_SetSwapInterval( interval );
}

void *GLimp_FG_GetProc( const char *name )
{
	return SDL_GL_GetProcAddress( name );
}
