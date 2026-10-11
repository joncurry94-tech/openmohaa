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
// cl_main.c  -- client main loop

#include "client.h"
#include "../server/server.h"
#include "cl_ui.h"
#include "../qcommon/net_rendezvous.h" // HZM coop [238]
#include "../qcommon/tiki.h"
#include "../qcommon/cm_terrain.h"
#include "../qcommon/localization.h"
#include "../qcommon/bg_compat.h"
#include "../sys/sys_local.h"
// HZM coop [bugreport_parity bug-3278] client/cl_bugreport.cpp
void CL_BugReportInit( void );
// HZM coop [bug-3486] bug-report diagnostics (qcommon/hzm_diag.h, cl_bugreport.cpp)
#include "../qcommon/hzm_diag.h"
extern "C" void CL_HzmNet( int what, int value );
int cl_hzmRefGen; // bumped whenever the renderer DLL is loaded or unloaded: a cached renderer symbol is stale after that
void CL_BugReportFrame( int msec );
// HZM coop [dedicated_launch 2026-10-04] the coop start screen's Dedicated option (cl_dedicated.cpp)
void CL_DedicatedInit( void );
void CL_DedicatedFrame( void );
void CL_DedicatedInfoResponse( netadr_t from );
void CL_DedicatedUserDisconnect( netadr_t was );
void CL_DedicatedShutdown( void );
void CL_SendReport_f( void );
#include "../sys/sys_update_checker.h"
#include "../uilib/uimessage.h"

extern "C" {
	#include "../sys/sys_loadlib.h"
}

#include "../gamespy/gcdkey/gcdkeyc.h"

#include <exception>
#include <new>

#include <climits>

#ifdef USE_RENDERER_DLOPEN
cvar_t* cl_renderer;
#endif

cvar_t	*cl_nodelta;
cvar_t	*cl_debugMove;

cvar_t	*cl_noprint;
cvar_t	*cl_motd;

cvar_t	*rcon_client_password;
cvar_t	*rconAddress;

cvar_t	*cl_timeout;
cvar_t	*cl_connect_timeout;
cvar_t	*cl_maxpackets;
cvar_t	*cl_packetdup;
cvar_t	*cl_master;
cvar_t	*cl_timeNudge;
cvar_t	*cl_showTimeDelta;
cvar_t	*cl_freezeDemo;

cvar_t	*cl_shownet;
cvar_t	*cl_netprofile;
cvar_t	*cl_netprofileoverlay;
cvar_t	*cl_showSend;
cvar_t	*cl_timedemo;
cvar_t	*cl_timedemoLog;
cvar_t	*cl_autoRecordDemo;
cvar_t	*cl_aviFrameRate;
cvar_t	*cl_aviMotionJpeg;
cvar_t	*cl_forceavidemo;

cvar_t	*cl_freelook;
cvar_t	*cl_sensitivity;

cvar_t	*cl_mouseAccel;
cvar_t	*cl_mouseAccelOffset;
cvar_t	*cl_mouseAccelStyle;
cvar_t	*cl_showMouseRate;

cvar_t	*cl_altbindings;
cvar_t	*cl_ctrlbindings;

cvar_t	*m_pitch;
cvar_t	*m_yaw;
cvar_t	*m_forward;
cvar_t	*m_side;
cvar_t	*m_filter;

cvar_t	*j_pitch;
cvar_t	*j_yaw;
cvar_t	*j_forward;
cvar_t	*j_side;
cvar_t	*j_up;
cvar_t	*j_pitch_axis;
cvar_t	*j_yaw_axis;
cvar_t	*j_forward_axis;
cvar_t	*j_side_axis;
cvar_t	*j_up_axis;

// [controller feel] analog LOOK tuning (right stick). Applied in CL_JoystickMove; neutral defaults = the
// old linear behaviour, so a keyboard player is unaffected and a controller player can dial these in.
cvar_t	*joy_response;        // look response-curve exponent (1 = linear, higher = finer near-centre aim)
cvar_t	*joy_outerThreshold;  // outer dead-zone: saturate to full past (1 - this)
cvar_t	*coop_padLookScale;   // one master look-sensitivity multiplier for the sticks

cvar_t	*cl_activeAction;

cvar_t	*cl_motdString;

cvar_t	*cl_allowDownload;
cvar_t	*cg_gametype;
cvar_t	*cl_conXOffset;
cvar_t	*cl_inGameVideo;

cvar_t	*cl_serverStatusResendTime;
cvar_t	*cl_trn;
cvar_t	*cl_langamerefreshstatus;
cvar_t	*cl_radar_icon_size;
cvar_t	*cl_radar_speak_time;
cvar_t	*cl_radar_blink_time;

cvar_t	*cl_lanForcePackets;

cvar_t	*cl_guidServerUniq;

cvar_t	*wombat;

cvar_t	*cl_r_fullscreen;

cvar_t	*cl_consoleKeys;
cvar_t	*name;
cvar_t	*cl_rate;

clientActive_t		cl;
clientConnection_t	clc;
clientStatic_t		cls;
clientGameExport_t	*cge;
//vm_t				*cgvm;

// Structure containing functions exported from refresh DLL
refexport_t	re;
#ifdef USE_RENDERER_DLOPEN
static void	*rendererLib = NULL;
#endif

qboolean camera_reset;
qboolean camera_active;
vec3_t camera_offset;

ping_t	cl_pinglist[MAX_PINGREQUESTS];

typedef struct serverStatus_s
{
	char string[BIG_INFO_STRING];
	netadr_t address;
	int time, startTime;
	qboolean pending;
	qboolean print;
	qboolean retrieved;
} serverStatus_t;

serverStatus_t cl_serverStatusList[MAX_SERVERSTATUSREQUESTS];
int serverStatusCount;

#if defined __USEA3D && defined __A3D_GEOM
	void hA3Dg_ExportRenderGeom (refexport_t *incoming_re);
#endif

static int noGameRestart = qfalse;

extern void SV_BotFrame( int time );
void CL_CheckForResend( void );
void CL_ShowIP_f(void);
void CL_ServerStatus_f(void);
void CL_ServerStatusResponse( netadr_t from, msg_t *msg );

static qboolean cl_bCLSystemStarted = qfalse;
static qboolean cl_updateNotified = qfalse;

/*
===============
CL_UseLargeLightmap

Added in 2.0
Returns true if the standard BSP file should be used, false if the smaller lightmap BSP file should be used
===============
*/
qboolean CL_UseLargeLightmap(const char* mapName) {
	char buffer[MAX_QPATH];

	Com_sprintf(buffer, sizeof(buffer), "maps/%s_sml.bsp", mapName);

	if (FS_ReadFileEx(buffer, NULL, qtrue) == -1) {
		return qtrue;
	}

	return Cvar_Get("r_largemap", "0", 0)->integer;
}

/*
===============
CL_CDDialog

Called by Com_Error when a cd is needed
===============
*/
void CL_CDDialog( void ) {
	cls.cddialog = qtrue;	// start it next frame
}

/*
======================
get_camera_offset
======================
*/
float *get_camera_offset( qboolean *lookactive, qboolean *resetview )
{
	*resetview = camera_reset;
	*lookactive = camera_active;
	return camera_offset;
}


/*
=======================================================================

CLIENT RELIABLE COMMAND COMMUNICATION

=======================================================================
*/

/*
======================
CL_AddReliableCommand

The given command will be transmitted to the server, and is gauranteed to
not have future usercmd_t executed before it is executed
======================
*/
void CL_AddReliableCommand( const char *cmd, qboolean isDisconnectCmd ) {
	int		index;

	// if we would be losing an old command that hasn't been acknowledged,
	// we must drop the connection
	if ( clc.reliableSequence - clc.reliableAcknowledge > MAX_RELIABLE_COMMANDS ) {
		Com_Error( ERR_DROP, "Client command overflow" );
	}
	clc.reliableSequence++;
	index = clc.reliableSequence & ( MAX_RELIABLE_COMMANDS - 1 );
	Q_strncpyz( clc.reliableCommands[ index ], cmd, sizeof( clc.reliableCommands[ index ] ) );
}

/*
======================
CL_ChangeReliableCommand
======================
*/
void CL_ChangeReliableCommand( void ) {
	size_t r, index, l;

	r = clc.reliableSequence - (random() * 5);
	index = clc.reliableSequence & ( MAX_RELIABLE_COMMANDS - 1 );
	l = strlen(clc.reliableCommands[ index ]);
	if ( l >= MAX_STRING_CHARS - 1 ) {
		l = MAX_STRING_CHARS - 2;
	}
	clc.reliableCommands[ index ][ l ] = '\n';
	clc.reliableCommands[ index ][ l+1 ] = '\0';
}

/*
=======================================================================

CLIENT SIDE DEMO RECORDING

=======================================================================
*/

/*
====================
CL_WriteDemoMessage

Dumps the current net message, prefixed by the length
====================
*/
void CL_WriteDemoMessage ( msg_t *msg, int headerBytes ) {
	size_t		len, swlen;

	// write the packet sequence
	len = clc.serverMessageSequence;
	swlen = LittleLong( len );
	FS_Write (&swlen, 4, clc.demofile);

	// skip the packet sequencing information
	len = msg->cursize - headerBytes;
	swlen = LittleLong(len);
	FS_Write (&swlen, 4, clc.demofile);
	FS_Write ( msg->data + headerBytes, len, clc.demofile );
}


/*
====================
CL_StopRecording_f

stop recording a demo
====================
*/
void CL_StopRecord_f( void ) {
	int		len;

	if ( !clc.demorecording ) {
		Com_Printf ("Not recording a demo.\n");
		return;
	}

	// finish up
	len = -1;
	FS_Write (&len, 4, clc.demofile);
	FS_Write (&len, 4, clc.demofile);
	FS_FCloseFile (clc.demofile);
	clc.demofile = 0;
	clc.demorecording = qfalse;
	clc.spDemoRecording = qfalse;
	Com_Printf ("Stopped demo.\n");
}

/*
==================
CL_DemoFilename
==================
*/
void CL_DemoFilename( int number, char *fileName ) {
	int		a,b,c,d;

	if(number < 0 || number > 9999)
		number = 9999;

	a = number / 1000;
	number -= a*1000;
	b = number / 100;
	number -= b*100;
	c = number / 10;
	number -= c*10;
	d = number;

	Com_sprintf( fileName, MAX_OSPATH, "demo%i%i%i%i"
		, a, b, c, d );
}

/*
====================
CL_Record_f

record <demoname>

Begins recording a demo from the current position
====================
*/
static char		demoName[MAX_QPATH];	// compiler bug workaround
void CL_Record_f( void ) {
	char		name[MAX_OSPATH];
	byte		bufData[MAX_MSGLEN];
	msg_t	buf;
	int			i;
	size_t			len;
	entityState_t	*ent;
	entityState_t	nullstate;
	char		*s;

	if ( Cmd_Argc() > 2 ) {
		Com_Printf ("record <demoname>\n");
		return;
	}

	if ( clc.demorecording ) {
		if (!clc.spDemoRecording) {
			Com_Printf ("Already recording.\n");
		}
		return;
	}

	if ( clc.state != CA_ACTIVE ) {
		Com_Printf ("You must be in a level to record.\n");
		return;
	}

  // sync 0 doesn't prevent recording, so not forcing it off .. everyone does g_sync 1 ; record ; g_sync 0 ..
	if ( NET_IsLocalAddress( clc.serverAddress ) && !Cvar_VariableValue( "g_synchronousClients" ) ) {
		Com_Printf (S_COLOR_YELLOW "WARNING: You should set 'g_synchronousClients 1' for smoother demo recording\n");
	}

	if ( Cmd_Argc() == 2 ) {
		s = Cmd_Argv(1);
		Q_strncpyz( demoName, s, sizeof( demoName ) );
		Com_sprintf (name, sizeof(name), "demos/%s.dm_%d", demoName, com_protocol->integer );
	} else {
		int		number;

		// scan for a free demo name
		for ( number = 0 ; number <= 9999 ; number++ ) {
			CL_DemoFilename( number, demoName );
			Com_sprintf (name, sizeof(name), "demos/%s.dm_%d", demoName, com_protocol->integer );

			if (!FS_FileExists(name))
				break;	// file doesn't exist
		}
	}

	// open the demo file

	Com_Printf ("recording to %s.\n", name);
	clc.demofile = FS_FOpenFileWrite( name );
	if ( !clc.demofile ) {
		Com_Printf ("ERROR: couldn't open.\n");
		return;
	}
	clc.demorecording = qtrue;
	if (Cvar_VariableValue("ui_recordSPDemo")) {
	  clc.spDemoRecording = qtrue;
	} else {
	  clc.spDemoRecording = qfalse;
	}


	Q_strncpyz( clc.demoName, demoName, sizeof( clc.demoName ) );

	// don't start saving messages until a non-delta compressed message is received
	clc.demowaiting = qtrue;

	// write out the gamestate message
	MSG_Init (&buf, bufData, sizeof(bufData));
	MSG_Bitstream(&buf);

	// NOTE, MRE: all server->client messages now acknowledge
	MSG_WriteLong( &buf, clc.reliableSequence );

	MSG_WriteByte ( &buf, svc_gamestate );
	MSG_WriteLong ( &buf, clc.serverCommandSequence );

	// configstrings
	for ( i = 0 ; i < MAX_CONFIGSTRINGS ; i++ ) {
		if ( !cl.gameState.stringOffsets[i] ) {
			continue;
		}
		s = cl.gameState.stringData + cl.gameState.stringOffsets[i];
		MSG_WriteByte ( &buf, svc_configstring );
		MSG_WriteShort ( &buf, CPT_DenormalizeConfigstring( i ) );
		MSG_WriteScrambledBigString ( &buf, s );
	}

	// baselines
	MSG_GetNullEntityState(&nullstate);
	for ( i = 0; i < MAX_GENTITIES ; i++ ) {
		ent = &cl.entityBaselines[i];
		if ( !ent->number ) {
			continue;
		}
		MSG_WriteByte ( &buf, svc_baseline );
		MSG_WriteDeltaEntity ( &buf, &nullstate, ent, qtrue, cls.serverFrameTime );
	}

	MSG_WriteByte( &buf, svc_EOF );

	// finished writing the gamestate stuff

	// write the client num
	MSG_WriteLong(&buf, clc.clientNum);
	// write the checksum feed
    MSG_WriteLong(&buf, clc.checksumFeed);

    // write the server frametime to the client (only on TA/TT)
    MSG_WriteServerFrameTime( &buf, cls.serverFrameTime );

	// finished writing the client packet
	MSG_WriteByte( &buf, svc_EOF );

	// write it to the demo file
	len = LittleLong( clc.serverMessageSequence - 1 );
	FS_Write (&len, 4, clc.demofile);

	len = LittleLong (buf.cursize);
	FS_Write (&len, 4, clc.demofile);
	FS_Write (buf.data, buf.cursize, clc.demofile);

	// the rest of the demo file will be copied from net messages
}

/*
=======================================================================

CLIENT SIDE DEMO PLAYBACK

=======================================================================
*/

/*
=================
CL_DemoFrameDurationSDev
=================
*/
static float CL_DemoFrameDurationSDev( void )
{
	int i;
	int numFrames;
	float mean = 0.0f;
	float variance = 0.0f;

	if( ( clc.timeDemoFrames - 1 ) > MAX_TIMEDEMO_DURATIONS )
		numFrames = MAX_TIMEDEMO_DURATIONS;
	else
		numFrames = clc.timeDemoFrames - 1;

	for( i = 0; i < numFrames; i++ )
		mean += clc.timeDemoDurations[ i ];
	mean /= numFrames;

	for( i = 0; i < numFrames; i++ )
	{
		float x = clc.timeDemoDurations[ i ];

		variance += ( ( x - mean ) * ( x - mean ) );
	}
	variance /= numFrames;

	return sqrt( variance );
}

/*
=================
CL_DemoCompleted
=================
*/
void CL_DemoCompleted( void )
{
	char buffer[ MAX_STRING_CHARS ];

	if( cl_timedemo && cl_timedemo->integer )
	{
		int	time;

		time = Sys_Milliseconds() - clc.timeDemoStart;
		if( time > 0 )
		{
			// Millisecond times are frame durations:
			// minimum/average/maximum/std deviation
			Com_sprintf( buffer, sizeof( buffer ),
					"%i frames %3.1f seconds %3.1f fps %d.0/%.1f/%d.0/%.1f ms\n",
					clc.timeDemoFrames,
					time/1000.0,
					clc.timeDemoFrames*1000.0 / time,
					clc.timeDemoMinDuration,
					time / (float)clc.timeDemoFrames,
					clc.timeDemoMaxDuration,
					CL_DemoFrameDurationSDev( ) );
			Com_Printf( "%s", buffer );

			// Write a log of all the frame durations
			if( cl_timedemoLog && strlen( cl_timedemoLog->string ) > 0 )
			{
				int i;
				int numFrames;
				fileHandle_t f;

				if( ( clc.timeDemoFrames - 1 ) > MAX_TIMEDEMO_DURATIONS )
					numFrames = MAX_TIMEDEMO_DURATIONS;
				else
					numFrames = clc.timeDemoFrames - 1;

				f = FS_FOpenFileWrite( cl_timedemoLog->string );
				if( f )
				{
					FS_Printf( f, "# %s", buffer );

					for( i = 0; i < numFrames; i++ )
						FS_Printf( f, "%d\n", clc.timeDemoDurations[ i ] );

					FS_FCloseFile( f );
					Com_Printf( "%s written\n", cl_timedemoLog->string );
				}
				else
				{
					Com_Printf( "Couldn't open %s for writing\n",
							cl_timedemoLog->string );
				}
			}
		}
	}

	CL_Disconnect();
	CL_NextDemo();
}

/*
=================
CL_ReadDemoMessage
=================
*/
void CL_ReadDemoMessage( void ) {
	size_t		r;
	msg_t		buf;
	byte		bufData[ MAX_MSGLEN ];
	int			s;

	if ( !clc.demofile ) {
		CL_DemoCompleted ();
		return;
	}

	// get the sequence number
	r = FS_Read( &s, 4, clc.demofile);
	if ( r != 4 ) {
		CL_DemoCompleted ();
		return;
	}
	clc.serverMessageSequence = LittleLong( s );

	// init the message
	MSG_Init( &buf, bufData, sizeof( bufData ) );

	// get the length
	r = FS_Read (&buf.cursize, 4, clc.demofile);
	if ( r != 4 ) {
		CL_DemoCompleted ();
		return;
	}
	buf.cursize = LittleLong( buf.cursize );
	if ( buf.cursize == (uint32_t)-1) {
		CL_DemoCompleted ();
		return;
	}
	if ( buf.cursize > buf.maxsize ) {
		Com_Error (ERR_DROP, "CL_ReadDemoMessage: demoMsglen > MAX_MSGLEN");
	}
	r = FS_Read( buf.data, buf.cursize, clc.demofile );
	if ( r != buf.cursize ) {
		Com_Printf( "Demo file was truncated.\n");
		CL_DemoCompleted ();
		return;
	}

	clc.lastPacketTime = cls.realtime;
	buf.readcount = 0;
	CL_ParseServerMessage( &buf );
}

/*
====================
CL_WalkDemoExt
====================
*/
static void CL_WalkDemoExt(char *arg, char *name, int *demofile)
{
	int i = 0;
	*demofile = 0;
	while(demo_protocols[i])
	{
		Com_sprintf (name, MAX_OSPATH, "demos/%s.dm_%d", arg, demo_protocols[i]);
		FS_FOpenFileRead( name, demofile, qtrue, qtrue );
		if (*demofile)
		{
			Com_Printf("Demo file: %s\n", name);
			break;
		}
		else
			Com_Printf("Not found: %s\n", name);
		i++;
	}
}

/*
====================
CL_PlayDemo_f

demo <demoname>

====================
*/
void CL_PlayDemo_f( void ) {
	char		name[MAX_OSPATH];
	char		*arg, *ext_test;
	int			protocol, i;
	char		retry[MAX_OSPATH];

	if (Cmd_Argc() != 2) {
		Com_Printf ("playdemo <demoname>\n");
		return;
	}

	// make sure a local server is killed
	// 2 means don't force disconnect of local client
	Cvar_Set( "sv_killserver", "2" );

	CL_Disconnect();

	// open the demo file
	arg = Cmd_Argv(1);

	// check for an extension .dm_?? (?? is protocol)
	ext_test = arg + strlen(arg) - 6;
	if ((strlen(arg) > 6) && (ext_test[0] == '.') && ((ext_test[1] == 'd') || (ext_test[1] == 'D')) && ((ext_test[2] == 'm') || (ext_test[2] == 'M')) && (ext_test[3] == '_'))
	{
		protocol = atoi(ext_test+4);
		i=0;
		while(demo_protocols[i])
		{
			if (demo_protocols[i] == protocol)
				break;
			i++;
		}
		if (demo_protocols[i])
		{
			Com_sprintf (name, sizeof(name), "demos/%s", arg);
			FS_FOpenFileRead( name, &clc.demofile, qtrue, qtrue );
		} else {
			Com_Printf("Protocol %d not supported for demos\n", protocol);
			Q_strncpyz(retry, arg, sizeof(retry));
			retry[strlen(retry)-6] = 0;
			CL_WalkDemoExt( retry, name, &clc.demofile );
		}
	} else {
		CL_WalkDemoExt( arg, name, &clc.demofile );
	}

	if (!clc.demofile) {
		Com_Error( ERR_DROP, "couldn't open %s", name);
		return;
	}
	Q_strncpyz( clc.demoName, Cmd_Argv(1), sizeof( clc.demoName ) );

    UI_CloseConsole();

	clc.state = CA_CONNECTED;
	clc.demoplaying = qtrue;
	Q_strncpyz( clc.servername, Cmd_Argv(1), sizeof( clc.servername ) );

	// read demo messages until connected
	while ( clc.state >= CA_CONNECTED && clc.state < CA_PRIMED ) {
		CL_ReadDemoMessage();
	}
	// don't get the first snapshot this frame, to prevent the long
	// time from the gamestate load from messing causing a time skip
	clc.firstDemoFrameSkipped = qfalse;
}


/*
====================
CL_StartDemoLoop

Closing the main menu will restart the demo loop
====================
*/
void CL_StartDemoLoop( void ) {
	// start the demo loop again
	Cbuf_AddText ("d1\n");
	Key_SetCatcher( 0 );
}

/*
==================
CL_NextDemo

Called when a demo or cinematic finishes
If the "nextdemo" cvar is set, that command will be issued
==================
*/
void CL_NextDemo( void ) {
	char	v[MAX_STRING_CHARS];

	Q_strncpyz( v, Cvar_VariableString ("nextdemo"), sizeof(v) );
	v[MAX_STRING_CHARS-1] = 0;
	Com_DPrintf("CL_NextDemo: %s\n", v );
	if (!v[0]) {
		return;
	}

	Cvar_Set ("nextdemo","");
	Cbuf_AddText (v);
	Cbuf_AddText ("\n");
	Cbuf_Execute(0);
}


//======================================================================

/*
=====================
CL_ShutdownAll
=====================
*/
void CL_ShutdownAll(qboolean shutdownRef) {

#ifdef USE_CURL
	CL_cURL_Shutdown();
#endif
	// clear sounds
#if defined(NO_MODERN_DMA) && NO_MODERN_DMA
	S_DisableSounds();
#endif
	// shutdown CGame
	CL_ShutdownCGame();
	// shutdown UI
	CL_ShutdownUI();

	// su44: shutdown TIKI system, because it's dependent on renderer
	TIKI_FreeAll();

	// shutdown the renderer
	if(shutdownRef)
		CL_ShutdownRef();
	else if(re.Shutdown)
		re.Shutdown(qfalse);		// don't destroy window or context

	cls.uiStarted = qfalse;
	cls.cgameStarted = qfalse;
	cls.rendererRegistered = qfalse;
	cls.rendererReady = qfalse;	// HZM bug-2981: re.Shutdown ran - not usable until the next BeginRegistration
}

/*
=================
CL_FlushMemory

Called by CL_MapLoading, CL_Connect_f, CL_PlayDemo_f, and CL_ParseGamestate the only
ways a client gets into a game
Also called by Com_Error
=================
*/
void CL_FlushMemory( void ) {

	// shutdown all the client stuff
	CL_ShutdownCGame();
	S_StopAllSounds2( qtrue );
	UI_ClearState();
	cls.rendererRegistered = qfalse;
}

/*
=====================
CL_MapLoading

A local server is starting to load a map, so update the
screen to let the user know about it, then dump all client
memory on the hunk from cgame, ui, and renderer
=====================
*/
void CL_MapLoading( qboolean flush, const char *pszMapName ) {
	if ( !com_cl_running->integer ) {
		return;
	}

	UI_ClearState();
	UI_ForceMenuOff(false);

	if (!flush) {
		// Don't do anything if it's not flushingb
		return;
	}

	if (pszMapName) {
		// if we are already connected to the local host, stay connected
		if (clc.state >= CA_CONNECTED && !Q_stricmp(clc.servername, "localhost")) {
			clc.state = CA_CONNECTED;		// so the connect screen is drawn
			Com_Memset(cls.updateInfoString, 0, sizeof(cls.updateInfoString));
			Com_Memset(clc.serverMessage, 0, sizeof(clc.serverMessage));
			Com_Memset(&cl.gameState, 0, sizeof(cl.gameState));
			clc.lastPacketSentTime = -9999;
		}
		else {
			// clear nextmap so the cinematic shutdown doesn't execute it
			Cvar_Set("nextmap", "");
			CL_Disconnect();
			Q_strncpyz(clc.servername, "localhost", sizeof(clc.servername));
			clc.state = CA_CHALLENGING;		// so the connect screen is drawn
            clc.connectStartTime = cls.realtime;
			clc.connectTime = -RETRANSMIT_TIMEOUT;
			NET_StringToAdr(clc.servername, &clc.serverAddress, NA_UNSPEC);
			// we don't need a challenge on the localhost

			CL_CheckForResend();
		}

		CL_FlushMemory();
		UI_BeginLoad(pszMapName);
	}
}

/*
=====================
CL_CleanupUserInfo

Remove user-created userinfo variables
=====================
*/
void CL_CleanupUserInfo()
{
    cvar_t *cv;
    cvar_t *next;

    for(cv = Cvar_Next(NULL), next = cv->next; cv; cv = next, next = Cvar_Next(cv)) {
        if ((cv->flags & CVAR_USER_CREATED) && (cv->flags & CVAR_USERINFO)) {
            Cvar_Unset(cv);
        }
    }
}

/*
=====================
CL_HZM_ResetTransientAudio

HZM coop [user 2026-09-13, bug-2573] "ANY time we leave the omaha beach map, whether its crashed, quit, closed,
disconnected, switched maps, restarted map, etc. that the audio ducking that we put in there gets reset to what it
should play on other maps. It still seems to carry sometimes (like if I disconnect during the cinematic sequence and
then load a new map)."

WHAT CARRIED: coop_duckMusicTrigger / coop_duckAmbientTrigger. The server stuffs them to 1 for Omaha's takeover and
back to 0 ninety-five seconds later. They are flags 0, so they never reach a config - but they outlive a disconnect
inside one process, and nothing reset them. The next map's cgame is a freshly loaded DLL (CL_ShutdownCGame unloads
it): its first frame finds the trigger already at 1, takes that for a new duck, captures a base and ducks again -
music swollen, every ambient loop at zero - and a map that never ducks never sends the 0 that releases it.
S_BeginRegistration's self-heals (bugs 2298, 2318, 2369) cover s_sfxduck and the cut cvars at a map load, but not
the triggers or coop_shellPan, and nothing runs at all on a return to the menu, where there is no registration.

So every transient value the server can stuff into the mix goes back to rest here, from the places every exit passes
through: CL_ClearState (launch, disconnect, every new gamestate - a map change, `map` on the same map, a server
dying) and CL_ServerRestarted (`restart`, which keeps the gamestate and the cgame). A crash cannot run anything, so
the archived saves below make the next launch's CL_Init do it instead.

THE VOLUMES are the player's own slider values, and only saved copies can put them back:
  - s_musicvolume / s_ambientvolume from coop_duckSave*, which cgame writes while a duck is in flight and sets to
    -1 when it completes (bug-2551).
  - s_volume from coop_duckSaveVolume, which cgame captures before the first server write of it
    (cg_servercmds.c CG_CoopNoteServerVolume). tinnitus, xp, dbno and medkit all stuff Master volume as a literal
    fade, and leaving mid-fade left it wherever that fade was. Restored only if s_volume still holds the last value
    the server wrote: a player who moved the slider since keeps their own.
Cvar_FindVar, never Cvar_VariableValue: at CL_Init a save that is not in the config does not exist, and reading it as
0 would restore every volume to silence.
=====================
*/
static void CL_HZM_ResetTransientAudio( const char *why, int state ) {
	static const struct {
		const char	*name;
		const char	*rest;
	} transient[] = {
		{ "coop_duckMusicTrigger",		"0" },
		{ "coop_duckAmbientTrigger",	"0" },
		{ "s_sfxduck",					"1" },
		{ "coop_voxCut",				"1" },
		{ "coop_cueCut",				"1" },
		{ "coop_muffle",				"0" },
		{ "coop_shellPan",				"0" },
	};
	static const struct {
		const char	*save;
		const char	*vol;
	} saved[] = {
		{ "coop_duckSaveMusic",		"s_musicvolume" },
		{ "coop_duckSaveAmbient",	"s_ambientvolume" },
	};
	char	changed[ MAX_STRING_CHARS ];
	cvar_t	*cv, *save, *srv;
	int		i;

	changed[ 0 ] = 0;

	for ( i = 0; i < (int)( sizeof( transient ) / sizeof( transient[ 0 ] ) ); i++ ) {
		cv = Cvar_FindVar( transient[ i ].name );
		if ( cv && fabs( cv->value - atof( transient[ i ].rest ) ) > 0.0001f ) {
			Q_strcat( changed, sizeof( changed ), va( " %s=%s->%s", transient[ i ].name, cv->string, transient[ i ].rest ) );
			Cvar_Set( transient[ i ].name, transient[ i ].rest );
		}
	}

	for ( i = 0; i < (int)( sizeof( saved ) / sizeof( saved[ 0 ] ) ); i++ ) {
		save = Cvar_FindVar( saved[ i ].save );
		if ( save && save->string[ 0 ] && save->value >= 0.0f ) {
			Q_strcat( changed, sizeof( changed ), va( " %s=%s->%s", saved[ i ].vol, Cvar_VariableString( saved[ i ].vol ), save->string ) );
			Cvar_Set( saved[ i ].vol, save->string );
			Cvar_Set( saved[ i ].save, "-1" );
		}
	}

	save = Cvar_FindVar( "coop_duckSaveVolume" );
	if ( save && save->string[ 0 ] && save->value >= 0.0f ) {
		cv  = Cvar_FindVar( "s_volume" );
		srv = Cvar_FindVar( "coop_duckSrvVolume" );
		if ( !cv || !srv || !srv->string[ 0 ] || srv->value < 0.0f || fabs( cv->value - srv->value ) < 0.001f ) {
			Q_strcat( changed, sizeof( changed ), va( " s_volume=%s->%s", cv ? cv->string : "", save->string ) );
			Cvar_Set( "s_volume", save->string );
		} else {
			Q_strcat( changed, sizeof( changed ), va( " s_volume=%s(kept:slider moved since the server set %s)", cv->string, srv->string ) );
		}
		Cvar_Set( "coop_duckSaveVolume", "-1" );
		Cvar_Set( "coop_duckSrvVolume", "-1" );
	}

	if ( changed[ 0 ] ) {
		Com_Printf( "^~^~^ AUDIORESET why=%s state=%d%s\n", why, state, changed );
	}
}

/*
=====================
CL_ClearState

Called before parsing a gamestate
=====================
*/
void CL_ClearState( void )
{
	CL_ShutdownCGame();

	// HZM coop [bug-2573] after the cgame is gone, so nothing can write the mix again for this map
	CL_HZM_ResetTransientAudio( "clearstate", clc.state );

	if( !com_sv_running->integer ) {
		S_StopAllSounds2( qtrue );
	}

	UI_ClearState();

	Com_Memset( &cl, 0, sizeof( clientActive_t ) );

    // Added in OPM
    CL_CleanupUserInfo();
}

/*
====================
CL_UpdateGUID

update cl_guid using QKEY_FILE and optional prefix
====================
*/
static void CL_UpdateGUID( const char *prefix, int prefix_len )
{
	fileHandle_t f;
	int len;

	len = FS_SV_FOpenFileRead( QKEY_FILE, &f );
	FS_FCloseFile( f );

	if( len != QKEY_SIZE )
		Cvar_Set( "cl_guid", "" );
	else
		Cvar_Set( "cl_guid", Com_MD5File( QKEY_FILE, QKEY_SIZE,
			prefix, prefix_len ) );
}


/*
=====================
CL_Disconnect

Called when a connection, demo, or cinematic is being terminated.
Goes from a connected state to either a menu state or a console state
Sends a disconnect message to the server
This is also called on Com_Error and Com_Quit, so it shouldn't cause any errors
=====================
*/
void CL_Disconnect() {
	int mouseX, mouseY;

	if ( !com_cl_running || !com_cl_running->integer ) {
		return;
	}

	// HZM coop [SEC2] leaving a server other than our own listen server: drop every command-buffer byte it
	// queued (a wait-deferred stufftext remainder), keeping local bytes in order, so none of it can run
	// against the next server - in particular a local one, where the listen-host allow-lists apply.
	// Leaving localhost keeps them: "disconnect; pushmenu briefingroom" (training.scr) needs its tail.
	if ( Q_stricmp( clc.servername, "localhost" ) ) {
		int removed = Cbuf_RemoveServerText();

		if ( removed && Cvar_VariableIntegerValue( "coop_covtrace" ) ) {
			Com_Printf( "^~^~^ COVX STRIP disconnect bytes=%i\n", removed );
		}
	}

	// HZM coop [bug-3486] connection timeline (CL_Disconnect also runs when nothing is connected: only real ones count)
	if ( clc.state >= CA_CONNECTING ) {
		HzmDiag_Event( "client: disconnect (was %s%s)", clc.state == CA_ACTIVE ? "in game" : clc.state >= CA_LOADING ? "loading"
						: "connecting", !Q_stricmp( clc.servername, "localhost" ) ? ", own server" : "" );
	}

	// shutting down the client so enter full screen ui mode
	Cvar_Set("r_uiFullScreen", "1");

	if ( clc.demorecording ) {
		CL_StopRecord_f ();
	}

	if (clc.download) {
		FS_FCloseFile( clc.download );
		clc.download = 0;
	}
	*clc.downloadTempName = *clc.downloadName = 0;
	Cvar_Set( "cl_downloadName", "" );

	if ( clc.demofile ) {
		FS_FCloseFile( clc.demofile );
		clc.demofile = 0;
	}

	SCR_StopCinematic ();
	S_ClearSoundBuffer();

	// send a disconnect message to the server
	// send it a few times in case one is dropped
	if ( clc.state >= CA_CONNECTED ) {
		CL_AddReliableCommand( "disconnect", qtrue );
		CL_WritePacket();
		CL_WritePacket();
		CL_WritePacket();
	}

	// Fixed in OPM
	//  Don't reset the mouse every time the client disconnects
	mouseX = cl.mousex;
	mouseY = cl.mousey;

	CL_ClearState ();

	cl.mousex = mouseX;
	cl.mousey = mouseY;

	// wipe the client connection
	Com_Memset( &clc, 0, sizeof( clientConnection_t ) );

	clc.state = CA_DISCONNECTED;

	// allow cheats locally
	Cvar_Set( "sv_cheats", "1" );

	// not connected to a pure server anymore
	cl_connectedToPureServer = qfalse;

	// Stop recording any video
	if( CL_VideoRecording( ) ) {
		// Finish rendering current frame
		SCR_UpdateScreen( );
		CL_CloseAVI( );
	}
	CL_UpdateGUID( NULL, 0 );
}

/*
===================
CL_AbnormalDisconnect
===================
*/
void CL_AbnormalDisconnect() {
    CL_Disconnect();
    UI_ServerLoaded();

	if (developer->integer) {
		UI_OpenConsole();
	}
}

/*
===================
CL_ForwardCommandToServer

adds the current command line as a clientCommand
things like godmode, noclip, etc, are commands directed to the server,
so when they are typed in at the console, they will need to be forwarded.
===================
*/
void CL_ForwardCommandToServer( const char *string ) {
	char	*cmd;

	cmd = Cmd_Argv(0);

	// ignore key up commands
	if ( cmd[0] == '-' ) {
		return;
	}

	if ( clc.demoplaying || clc.state < CA_CONNECTED || cmd[0] == '+' ) {
		Com_Printf ("Unknown command \"%s" S_COLOR_WHITE "\"\n", cmd);
		return;
	}

	if ( Cmd_Argc() > 1 ) {
		CL_AddReliableCommand( string, qfalse );
	} else {
		CL_AddReliableCommand( cmd, qfalse );
	}
}

/*
===================
CL_RequestMotd

===================
*/
void CL_RequestMotd( void ) {
	char		info[MAX_INFO_STRING];

	if ( !cl_motd->integer ) {
		return;
	}
	Com_Printf( "Resolving %s\n", UPDATE_SERVER_NAME );
	if ( !NET_StringToAdr( UPDATE_SERVER_NAME, &cls.updateServer, NA_IP ) ) {
		Com_Printf( "Couldn't resolve address\n" );
		return;
	}
	cls.updateServer.port = BigShort( PORT_UPDATE );
	Com_Printf( "%s resolved to %i.%i.%i.%i:%i\n", UPDATE_SERVER_NAME,
		cls.updateServer.ip[0], cls.updateServer.ip[1],
		cls.updateServer.ip[2], cls.updateServer.ip[3],
		BigShort( cls.updateServer.port ) );

	info[0] = 0;
  // NOTE TTimo xoring against Com_Milliseconds, otherwise we may not have a true randomization
  // only srand I could catch before here is tr_noise.c l:26 srand(1001)
  // https://zerowing.idsoftware.com/bugzilla/show_bug.cgi?id=382
  // NOTE: the Com_Milliseconds xoring only affects the lower 16-bit word,
  //   but I decided it was enough randomization
	Com_sprintf( cls.updateChallenge, sizeof( cls.updateChallenge ), "%i", ((rand() << 16) ^ rand()) ^ Com_Milliseconds());

	Info_SetValueForKey( info, "challenge", cls.updateChallenge );
	Info_SetValueForKey( info, "renderer", cls.glconfig.renderer_string );
	Info_SetValueForKey( info, "version", com_version->string );

	CL_NET_OutOfBandPrint( cls.updateServer, "getmotd \"%s\"\n", info );
}

/*
===================
CL_RequestAuthorization

Authorization server protocol
-----------------------------

All commands are text in Q3 out of band packets (leading 0xff 0xff 0xff 0xff).

Whenever the client tries to get a challenge from the server it wants to
connect to, it also blindly fires off a packet to the authorize server:

getKeyAuthorize <challenge> <cdkey>

cdkey may be "demo"


#OLD The authorize server returns a:
#OLD
#OLD keyAthorize <challenge> <accept | deny>
#OLD
#OLD A client will be accepted if the cdkey is valid and it has not been used by any other IP
#OLD address in the last 15 minutes.


The server sends a:

getIpAuthorize <challenge> <ip>

The authorize server returns a:

ipAuthorize <challenge> <accept | deny | demo | unknown >

A client will be accepted if a valid cdkey was sent by that ip (only) in the last 15 minutes.
If no response is received from the authorize server after two tries, the client will be let
in anyway.
===================
*/
void CL_RequestAuthorization( void ) {
    // MOHAA doesn't support this
#if 0
	char	nums[64];
	int		i, j;
	size_t	l;
	cvar_t	*fs;

	if ( !cls.authorizeServer.port ) {
		Com_Printf( "Resolving %s\n", AUTHORIZE_SERVER_NAME );
		if ( !NET_StringToAdr( AUTHORIZE_SERVER_NAME, &cls.authorizeServer, NA_IP ) ) {
			Com_Printf( "Couldn't resolve address\n" );
			return;
		}

		cls.authorizeServer.port = BigShort( PORT_AUTHORIZE );
		Com_Printf( "%s resolved to %i.%i.%i.%i:%i\n", AUTHORIZE_SERVER_NAME,
			cls.authorizeServer.ip[0], cls.authorizeServer.ip[1],
			cls.authorizeServer.ip[2], cls.authorizeServer.ip[3],
			BigShort( cls.authorizeServer.port ) );
	}
	if ( cls.authorizeServer.type == NA_BAD ) {
		return;
	}

	// only grab the alphanumeric values from the cdkey, to avoid any dashes or spaces
	j = 0;
	l = strlen( cl_cdkey );
	if ( l > 32 ) {
		l = 32;
	}
	for ( i = 0 ; i < l ; i++ ) {
		if ( ( cl_cdkey[i] >= '0' && cl_cdkey[i] <= '9' )
				|| ( cl_cdkey[i] >= 'a' && cl_cdkey[i] <= 'z' )
				|| ( cl_cdkey[i] >= 'A' && cl_cdkey[i] <= 'Z' )
			 ) {
			nums[j] = cl_cdkey[i];
			j++;
		}
	}
	nums[j] = 0;

	fs = Cvar_Get ("cl_anonymous", "0", CVAR_INIT|CVAR_SYSTEMINFO );

	CL_NET_OutOfBandPrint(cls.authorizeServer, "getKeyAuthorize %i %s", fs->integer, nums );
#endif
}

/*
======================================================================

CONSOLE COMMANDS

======================================================================
*/

/*
==================
CL_ForwardToServer_f
==================
*/
void CL_ForwardToServer_f( void ) {
	if ( clc.state != CA_ACTIVE || clc.demoplaying ) {
		Com_Printf ("Not connected to a server.\n");
		return;
	}

	// don't forward the first argument
	if ( Cmd_Argc() > 1 ) {
		CL_AddReliableCommand( Cmd_Args(), qfalse );
	}
}

/*
==================
CL_Setenv_f

Mostly for controlling voodoo environment variables
==================
*/
void CL_Setenv_f( void ) {
	int argc = Cmd_Argc();

	if ( argc > 2 ) {
		char buffer[1024];
		int i;

		Q_strncpyz( buffer, Cmd_Argv(1), sizeof( buffer ) );
		Q_strcat( buffer, sizeof( buffer ), "=" );

		for ( i = 2; i < argc; i++ ) {
			Q_strcat( buffer, sizeof( buffer ), Cmd_Argv( i ) );
			Q_strcat( buffer, sizeof( buffer ), " " );
		}

		putenv( buffer );
	} else if ( argc == 2 ) {
		char *env = getenv( Cmd_Argv(1) );

		if ( env ) {
			Com_Printf( "%s=%s\n", Cmd_Argv(1), env );
		} else {
			Com_Printf( "%s undefined\n", Cmd_Argv(1));
		}
	}
}


/*
==================
CL_Disconnect_f
==================
*/
void CL_Disconnect_f( void ) {
	qboolean bConsoleState;
	netadr_t was; // HZM coop [dedicated_launch]

	if ( clc.state == CA_DISCONNECTED ) {
		return;
	}
	was = clc.serverAddress;

	bConsoleState = UI_ConsoleIsOpen();

	Com_Printf("\nDisconnected from server\n");
	SV_Shutdown("Disconnected from server\n");

	if (com_cl_running && com_cl_running->integer)
	{
		CL_AbnormalDisconnect();
		CL_FlushMemory();
		CL_StartHunkUsers(qfalse);
		S_StopAllSounds2(1);
		S_TriggeredMusic_PlayIntroMusic();
	}

	UI_FocusMenuIfExists();

	if (bConsoleState) {
		UI_OpenConsole();
	} else {
		UI_CloseConsole();
	}

	// HZM coop [dedicated_launch] leaving the dedicated server this game started stops it (unless Keep Running)
	CL_DedicatedUserDisconnect( was );
}


/*
================
CL_Reconnect_f

================
*/
void CL_Reconnect_f( void ) {
	if ( !strlen( clc.servername ) || !strcmp( clc.servername, "localhost" ) ) {
		Com_Printf( "Can't reconnect to localhost.\n" );
		return;
	}
	Cvar_Set("ui_singlePlayerActive", "0");
	HzmDiag_Event( "client: reconnect requested" ); // HZM coop [bug-3486]
	Cbuf_AddText( va("connect %s\n", clc.servername ) );
}

/*
================
CL_Connect

================
*/
void CL_Connect( const char *server, netadrtype_t family ) {
	const char *serverString;

	Cvar_Set( "ui_singlePlayerActive", "0" );

	// fire a message off to the motd server
	CL_RequestMotd();

	// clear any previous "server full" type messages
	clc.serverMessage[ 0 ] = 0;

	if( com_sv_running->integer && !strcmp( server, "localhost" ) ) {
		// if running a local server, kill it
		SV_Shutdown( "Server quit" );
	}

	// make sure a local server is killed
	Cvar_Set( "sv_killserver", "1" );
	SV_Frame( 0 );

	CL_Disconnect();
	UI_CloseConsole();

	/* MrE: 2000-09-13: now called in CL_DownloadsComplete
	CL_FlushMemory( );
	*/

	Q_strncpyz( clc.servername, server, sizeof( clc.servername ) );

	// HZM coop [bug-3486] connection timeline; the address itself is never recorded
	HzmDiag_Flag( HZF_CONNECT );
	HzmDiag_Event( "client: connecting to %s", !Q_stricmp( server, "localhost" ) ? "its own server" : "a server" );
	CL_HzmNet( 4, 0 );

	if( !NET_StringToAdr( clc.servername, &clc.serverAddress, family ) ) {
		Com_Printf( "Bad server address\n" );
		clc.state = CA_DISCONNECTED;
		UI_PushMenu("badserveraddy");
		return;
	}
	if( clc.serverAddress.port == 0 ) {
		clc.serverAddress.port = BigShort( PORT_SERVER );
	}
	serverString = NET_AdrToStringwPort(clc.serverAddress);

	Com_Printf( "%s resolved to %s\n", clc.servername, serverString );

	if (cl_guidServerUniq->integer)
		CL_UpdateGUID(serverString, strlen(serverString));
	else
		CL_UpdateGUID(NULL, 0);

	// if we aren't playing on a lan, we need to authenticate
	// with the cd key
	// wombat: no authorization in mohaa. need to send challenge to server though
	if( NET_IsLocalAddress( clc.serverAddress ) ) {
		clc.state = CA_CHALLENGING;
	}
	else {
		clc.state = CA_CONNECTING;

		// Set a client challenge number that ideally is mirrored back by the server.
		clc.challenge = (((unsigned int)rand() << 16) ^ (unsigned int)rand()) ^ Com_Milliseconds();
	}

	clc.connectTime = -99999;	// CL_CheckForResend() will fire immediately
	clc.connectPacketCount = 0;
	clc.connectStartTime = cls.realtime;

	// server connection string
	Cvar_Set( "cl_currentServerAddress", server );
}

/*
================
CL_Connect_f

================
*/
// we have our own server provided to us by mohaaaa.co.uk
void CL_Connect_f(void) {
    const char *server;
    int argc = Cmd_Argc();
    netadrtype_t family = NA_UNSPEC;

	if ( Cmd_Argc() != 2 ) {
		Com_Printf( "usage: connect [server]\n");
		return;
	}

	if (argc != 2 && argc != 3) {
		Com_Printf("usage: connect [-4|-6] server\n");
		return;
	}

	if (argc == 2)
		server = Cmd_Argv(1);
	else
	{
		if (!strcmp(Cmd_Argv(1), "-4"))
			family = NA_IP;
		else if (!strcmp(Cmd_Argv(1), "-6"))
			family = NA_IP6;
		else
			Com_Printf("warning: only -4 or -6 as address type understood.\n");

		server = Cmd_Argv(2);
	}

	CL_Connect( server, family );
}

/*
================
CL_MenuConnect_f

================
*/
void CL_MenuConnect_f( void ) {
	char	*server;

	if( Cmd_Argc() != 2 ) {
		return;
	}

	server = Cmd_Argv( 1 );

	CL_Connect( Cvar_VariableString( server ), NA_UNSPEC );
}

/*
================
CL_FastConnect_f
================
*/
void CL_FastConnect_f( void ) {
	static int	endTime;
	int			i;

	if( cls.numlocalservers )
	{
		Com_Printf( "Done.\n Connecting To Server\n" );

		for( i = 0; i < cls.numlocalservers; i++ ) {
			Com_Printf( "Server %i - '%s'\n", i, NET_AdrToString( cls.localServers[ i ].adr ) );
		}

		CL_Connect( cls.localServers[ 0 ].hostName, NA_UNSPEC );
		Cbuf_AddText( "popmenu 0 ; wait 500 ; popmenu 0 ; wait 500 ; popmenu 0\n" );
		endTime = 123456789;
	}
	else if( cls.realtime <= endTime || endTime == 123456789 )
	{
		if( endTime == 123456789 )
		{
			endTime = cls.realtime + 10000;
			CL_LocalServers_f();
			Com_Printf( "Searching..." );
		}

		Com_Printf( "." );
		Cbuf_AddText( "wait 500 ; fastconnect\n" );
	}
	else
	{
		Com_Printf( "Done. !Server Not Found!\n" );
		endTime = 123456789;
	}
}

#define MAX_RCON_MESSAGE 1024

/*
=====================
CL_Rcon_f

  Send the rest of the command line over as
  an unconnected command.
=====================
*/
void CL_Rcon_f( void ) {
	char	message[MAX_RCON_MESSAGE];
	netadr_t	to;

	if ( !rcon_client_password->string ) {
		Com_Printf ("You must set 'rconpassword' before\n"
					"issuing an rcon command.\n");
		return;
	}

	message[0] = -1;
	message[1] = -1;
	message[2] = -1;
	message[3] = -1;
	message[4] = 2; // direction
	message[5] = 0;

	Q_strcat (message, MAX_RCON_MESSAGE, "rcon ");

	Q_strcat (message, MAX_RCON_MESSAGE, rcon_client_password->string);
	Q_strcat (message, MAX_RCON_MESSAGE, " ");

	// https://zerowing.idsoftware.com/bugzilla/show_bug.cgi?id=543
	Q_strcat (message, MAX_RCON_MESSAGE, Cmd_Cmd()+5);

	if ( clc.state >= CA_CONNECTED ) {
		to = clc.netchan.remoteAddress;
	} else {
		if (!strlen(rconAddress->string)) {
			Com_Printf ("You must either be connected,\n"
						"or set the 'rconAddress' cvar\n"
						"to issue rcon commands\n");

			return;
		}
		NET_StringToAdr (rconAddress->string, &to, NA_IP);
		if (to.port == 0) {
			to.port = BigShort (PORT_SERVER);
		}
	}

	NET_SendPacket (NS_CLIENT, strlen(message) + 1, message, to);

	if (cl_netprofile->integer) {
		NetProfileAddPacket(&cls.netprofile.inPackets, strlen(message) + 1, NETPROF_PACKET_MESSAGE);
	}
}

/*
=================
CL_SendPureChecksums
=================
*/
void CL_SendPureChecksums( void ) {
	const char *pChecksums;
	char cMsg[MAX_INFO_VALUE];
	int i;

	// if we are pure we need to send back a command with our referenced pk3 checksums
	pChecksums = FS_ReferencedPakPureChecksums();

	// "cp"
	// "Yf"
	Com_sprintf(cMsg, sizeof(cMsg), "Yf ");
	Q_strcat(cMsg, sizeof(cMsg), pChecksums);
	for (i = 0; i < 2; i++) {
		cMsg[i] += '\n';
	}
	CL_AddReliableCommand( cMsg, qfalse );
}

/*
=================
CL_ResetPureClientAtServer
=================
*/
void CL_ResetPureClientAtServer( void ) {
	CL_AddReliableCommand( va("vdr"), qfalse );
}

qboolean CL_Allowed_Vid_Restart() {
	return clc.state == CA_DISCONNECTED || clc.state == CA_ACTIVE;
}

/*
=================
CL_Vid_Restart_f

Restart the video subsystem

we also have to reload the UI, CGame and TIKI system
because the renderer doesn't know what graphics to reload
=================
*/
void CL_Vid_Restart_f( void ) {
    soundsystemsavegame_t save;
    char tm_filename[MAX_RES_NAME];
    int tm_loopcount;
    int tm_offset;
    char current_soundtrack[128];

	if (!CL_Allowed_Vid_Restart()) {
		return;
	}

	Com_Printf("-------- CL_Vid_Restart_f --------\n");
	cls.vid_restart = qtrue;
	HzmDiag_LongOp( HZF_VID_RESTART, 1, "renderer restart" ); // HZM coop [bug-3486] hitch log + freeze watchdog

    //
    // Save the current music
    //
    Q_strncpyz(tm_filename, S_GetMusicFilename(), sizeof(tm_filename));
    tm_loopcount = S_GetMusicLoopCount();
    tm_offset = S_GetMusicOffset();

	SV_ClearSvsTimeFixups();

    S_SaveData(&save);
    Q_strncpyz(current_soundtrack, S_CurrentSoundtrack(), sizeof(current_soundtrack));

	S_BeginRegistration();

	// shutdown the UI
	//CL_ShutdownUI();
	// shutdown the renderer and clear the renderer interface
    CL_ShutdownRef();

    cls.rendererRegistered = qfalse;

	// shutdown the CGame
	CL_ShutdownCGame();
	// initialize the renderer interface
	CL_InitRef();
	// initialize the UI
	//CL_InitializeUI();
	// HZM (bug-1145 "advanced settings apply crashes gl2"): the stock UI_ResolutionChange()
	// call used to sit HERE, between CL_InitRef() and CL_StartHunkUsers(). That is too early.
	// CL_ShutdownRef/CL_InitRef UNLOAD and RELOAD the renderer DLL, so every renderer global -
	// including every cvar_t* - is NULL again, and UI_ResolutionChange walks the UI's
	// regged-material list re-registering every menu shader against that dead renderer.
	// renderergl1 survives it because gl1 runs R_Init() from GetRefAPI (DLL load); renderergl2
	// runs R_Init() from RE_BeginRegistration, which does not happen until CL_StartHunkUsers
	// below -> NULL cvar deref (InitShaderEx r_pbr->integer, then R_LoadImage; both verified
	// from minidumps). The call is also REDUNDANT: CL_StartHunkUsers calls UI_ResolutionChange()
	// itself, right after CL_BeginRegistration, i.e. against a fully initialised renderer -
	// and cls.rendererRegistered was just cleared above, so that call is guaranteed to run.
	// clear aliases
	Alias_Clear();

	// unpause so the cgame definately gets a snapshot and renders a frame
	Com_Unpause();

	CL_StartHunkUsers(qfalse);

#if !defined(NO_MODERN_DMA) || !NO_MODERN_DMA
    s_bSoundPaused = true;
    S_LoadData(&save);
#else
    S_Init();
#endif

	SV_FinishSvsTimeFixups();

    S_ReLoad(&save);

    if (tm_filename[0]) {
        S_TriggeredMusic_SetupHandle(tm_filename, tm_loopcount, tm_offset, 0);
    }

    MUSIC_NewSoundtrack(current_soundtrack);

	if (clc.state > CA_CONNECTED && clc.state != CA_CINEMATIC)
	{
		// send pure checksums
		CL_SendPureChecksums();
	}

	cls.vid_restart = qfalse;
	Com_Printf("----- finished CL_Vid_Restart_f ----\n");
	HzmDiag_LongOp( HZF_VID_RESTART, 0, NULL ); // HZM coop [bug-3486]
}

/*
=================
CL_Snd_Restart_f

Restart the sound subsystem
The cgame and game must also be forced to restart because
handles will be invalid
=================
*/
void CL_Snd_Restart_f( void ) {
	HzmDiag_Flag( HZF_SND_RESTART ); // HZM coop [bug-3486]
#if defined(NO_MODERN_DMA) && NO_MODERN_DMA
	S_Shutdown();
	S_Init();
    CL_Vid_Restart_f();
#else
	qboolean full;
	soundsystemsavegame_t save;
    char tm_filename[MAX_RES_NAME];
	int tm_loopcount;
	int tm_offset;
	char current_soundtrack[128];

	full = S_NeedFullRestart();

    //
    // Save the current music
    //
    Q_strncpyz(tm_filename, S_GetMusicFilename(), sizeof(tm_filename));
    tm_loopcount = S_GetMusicLoopCount();
    tm_offset = S_GetMusicOffset();

	SV_ClearSvsTimeFixups();

	S_SaveData(&save);
	Q_strncpyz(current_soundtrack, S_CurrentSoundtrack(), sizeof(current_soundtrack));

	S_Shutdown(qfalse);
	S_Init(qfalse);

	s_bSoundPaused = true;

	S_LoadData(&save);

	SV_FinishSvsTimeFixups();
	S_ReLoad(&save);

	if (tm_filename[0]) {
		S_TriggeredMusic_SetupHandle(tm_filename, tm_loopcount, tm_offset, 0);
	}

	MUSIC_NewSoundtrack(current_soundtrack);

	if (full) {
		CL_Vid_Restart_f();
	}
#endif
}


/*
==================
CL_PK3List_f
==================
*/
void CL_OpenedPK3List_f( void ) {
	Com_Printf("Opened PK3 Names: %s\n", FS_LoadedPakNames());
}

/*
==================
CL_PureList_f
==================
*/
void CL_ReferencedPK3List_f( void ) {
	Com_Printf("Referenced PK3 Names: %s\n", FS_ReferencedPakNames());
}

/*
==================
CL_ConfigString
==================
*/
const char *CL_ConfigString( int index ) {
	// HZM (bug-1180): was `index > MAX_CONFIGSTRINGS`, an off-by-one that let index ==
	// MAX_CONFIGSTRINGS read one past the end of stringOffsets[]. Also guard negatives.
	if( index < 0 || index >= MAX_CONFIGSTRINGS ) {
		Com_Error( ERR_DROP, "CL_ConfigString: bad index: %i", index );
	}
	return &cl.gameState.stringData[ cl.gameState.stringOffsets[ index ] ];
}

/*
==================
CL_Configstrings_f
==================
*/
void CL_Configstrings_f( void ) {
	int		i;
	int		ofs;

	if ( clc.state != CA_ACTIVE ) {
		Com_Printf( "Not connected to a server.\n");
		return;
	}

	for ( i = 0 ; i < MAX_CONFIGSTRINGS ; i++ ) {
		ofs = cl.gameState.stringOffsets[ i ];
		if ( !ofs ) {
			continue;
		}
		Com_Printf( "%4i: %s\n", i, cl.gameState.stringData + ofs );
	}
}

/*
==============
CL_Clientinfo_f
==============
*/
void CL_Clientinfo_f( void ) {
	Com_Printf( "--------- Client Information ---------\n" );
	Com_Printf( "state: %i\n", clc.state );
	Com_Printf( "Server: %s\n", clc.servername );
	Com_Printf ("User info settings:\n");
	Info_Print( Cvar_InfoString( CVAR_USERINFO ) );
	Com_Printf( "--------------------------------------\n" );
}


//====================================================================

/*
=================
CL_DownloadsComplete

Called when all downloading has been completed
=================
*/
void CL_DownloadsComplete( void ) {

#ifdef USE_CURL
	// if we downloaded with cURL
	if(clc.cURLUsed) {
		clc.cURLUsed = qfalse;
		CL_cURL_Shutdown();
		if( clc.cURLDisconnected ) {
			if(clc.downloadRestart) {
				FS_Restart(clc.checksumFeed);
				clc.downloadRestart = qfalse;
			}
			clc.cURLDisconnected = qfalse;
			CL_Reconnect_f();
			return;
		}
	}
#endif

	// if we downloaded files we need to restart the file system
	if (clc.downloadRestart) {
		clc.downloadRestart = qfalse;

		FS_Restart(clc.checksumFeed); // We possibly downloaded a pak, restart the file system to load it

		// inform the server so we get new gamestate info
		CL_AddReliableCommand( "donedl", qfalse );

		// by sending the donedl command we request a new gamestate
		// so we don't want to load stuff yet
		return;
	}

	// let the client game init and load data
	clc.state = CA_LOADING;

    CL_StartHunkUsers(qfalse);
    // Pump the loop, this may change gamestate!
    Com_EventLoop();

    CL_StartHunkUsers(qfalse);

	// set pure checksums
	CL_SendPureChecksums();

	CL_WritePacket();
	CL_WritePacket();
	CL_WritePacket();
}

/*
=================
CL_BeginDownload

Requests a file to download from the server.  Stores it in the current
game directory.
=================
*/
void CL_BeginDownload( const char *localName, const char *remoteName ) {

	Com_DPrintf("***** CL_BeginDownload *****\n"
				"Localname: %s\n"
				"Remotename: %s\n"
				"****************************\n", localName, remoteName);

	Q_strncpyz ( clc.downloadName, localName, sizeof(clc.downloadName) );
	Com_sprintf( clc.downloadTempName, sizeof(clc.downloadTempName), "%s.tmp", localName );

	// Set so UI gets access to it
	Cvar_Set( "cl_downloadName", remoteName );
	Cvar_Set( "cl_downloadSize", "0" );
	Cvar_Set( "cl_downloadCount", "0" );
	Cvar_SetValue( "cl_downloadTime", cls.realtime );

	clc.downloadBlock = 0; // Starting new file
	clc.downloadCount = 0;

	CL_AddReliableCommand( va("download %s", remoteName), qfalse );
}

/*
=================
CL_NextDownload

A download completed or failed
=================
*/
void CL_NextDownload(void) {
	char *s;
	char *remoteName, *localName;
	qboolean useCURL = qfalse;

	// We are looking to start a download here
	if (*clc.downloadList) {
		s = clc.downloadList;

		// format is:
		//  @remotename@localname@remotename@localname, etc.

		if (*s == '@')
			s++;
		remoteName = s;

		if ( (s = strchr(s, '@')) == NULL ) {
			CL_DownloadsComplete();
			return;
		}

		*s++ = 0;
		localName = s;
		if ( (s = strchr(s, '@')) != NULL )
			*s++ = 0;
		else
			s = localName + strlen(localName); // point at the nul byte
#ifdef USE_CURL
		if(!(cl_allowDownload->integer & DLF_NO_REDIRECT)) {
			if(clc.sv_allowDownload & DLF_NO_REDIRECT) {
				Com_Printf("WARNING: server does not "
					"allow download redirection "
					"(sv_allowDownload is %d)\n",
					clc.sv_allowDownload);
			}
			else if(!*clc.sv_dlURL) {
				Com_Printf("WARNING: server allows "
					"download redirection, but does not "
					"have sv_dlURL set\n");
			}
			else if(!CL_cURL_Init()) {
				Com_Printf("WARNING: could not load "
					"cURL library\n");
			}
			else {
				CL_cURL_BeginDownload(localName, va("%s/%s",
					clc.sv_dlURL, remoteName));
				useCURL = qtrue;
			}
		}
		else if(!(clc.sv_allowDownload & DLF_NO_REDIRECT)) {
			Com_Printf("WARNING: server allows download "
				"redirection, but it disabled by client "
				"configuration (cl_allowDownload is %d)\n",
				cl_allowDownload->integer);
		}
#endif /* USE_CURL */
		if(!useCURL) {
			if((cl_allowDownload->integer & DLF_NO_UDP)) {
				Com_Error(ERR_DROP, "UDP Downloads are "
					"disabled on your client. "
					"(cl_allowDownload is %d)",
					cl_allowDownload->integer);
				return;
			}
			else {
				CL_BeginDownload( localName, remoteName );
			}
		}
		clc.downloadRestart = qtrue;

		// move over the rest
		memmove( clc.downloadList, s, strlen(s) + 1);

		return;
	}

	CL_DownloadsComplete();
}

/*
=================
CL_InitDownloads

After receiving a valid game state, we valid the cgame and local zip files here
and determine if we need to download them
=================
*/
void CL_InitDownloads(void) {
  char missingfiles[1024];

  if ( !(cl_allowDownload->integer & DLF_ENABLE) )
  {
    // autodownload is disabled on the client
    // but it's possible that some referenced files on the server are missing
    if (FS_ComparePaks( missingfiles, sizeof( missingfiles ), qfalse ) )
    {
      // NOTE TTimo I would rather have that printed as a modal message box
      //   but at this point while joining the game we don't know whether we will successfully join or not
      Com_Printf( "\nWARNING: You are missing some files referenced by the server:\n%s"
                  "You might not be able to join the game\n"
                  "Go to the setting menu to turn on autodownload, or get the file elsewhere\n\n", missingfiles );
    }
  }
  else if ( FS_ComparePaks( clc.downloadList, sizeof( clc.downloadList ) , qtrue ) ) {

    Com_Printf("Need paks: %s\n", clc.downloadList );

		if ( *clc.downloadList ) {
			// if autodownloading is not enabled on the server
			clc.state = CA_CONNECTED;
			CL_NextDownload();
			return;
		}

	}

	CL_DownloadsComplete();
}

/*
=================
CL_CheckForResend

Resend a connect message if the last one has timed out
=================
*/
void CL_CheckForResend( void ) {
	int		port, i;
	char	info[MAX_INFO_STRING];
	char	data[MAX_INFO_STRING];

	// don't send anything if playing back a demo
	if ( clc.demoplaying ) {
		return;
	}

	// resend if we haven't gotten a reply yet
	if ( clc.state != CA_CONNECTING && clc.state != CA_CHALLENGING ) {
		return;
	}

	if ( cls.realtime - clc.connectTime < RETRANSMIT_TIMEOUT ) {
		return;
	}

	clc.connectTime = cls.realtime;	// for retransmit requests
	clc.connectPacketCount++;


	switch ( clc.state ) {
	case CA_CONNECTING:
#ifndef STANDALONE
		if (!com_standalone->integer && clc.serverAddress.type == NA_IP && !Sys_IsLANAddress( clc.serverAddress ) )
			CL_RequestAuthorization();
#endif
		CL_NET_OutOfBandPrint(clc.serverAddress, "getchallenge");
		break;
	case CA_AUTHORIZING:
		// resend the cd key authorization
		gcd_compute_response(cl_cdkey, Cmd_Argv(1), cls.gcdResponse, CDResponseMethod_REAUTH);
		CL_NET_OutOfBandPrint(clc.serverAddress, "authorizeThis %s", cls.gcdResponse);
		break;
	case CA_CHALLENGING:
/*
wombat: sending conect here: an example connect string from MOHAA looks like this:
"connect "\challenge\-1629263210\qport\9683\protocol\8\name\wombat\rate\25000\dm_playermodel\american_ranger\snaps\20\dm_playergermanmodel\german_wehrmacht_soldier""
*/

		// sending back the challenge
		port = Cvar_VariableIntegerValue ("net_qport");

        // sanitize the name before sending it
	    char szSanitizedName[MAX_NAME_LENGTH];
		if (Com_SanitizeName(name->string, szSanitizedName, sizeof(szSanitizedName))) {
			Cvar_Set("name", szSanitizedName);
		}

		Q_strncpyz( info, Cvar_InfoString( CVAR_USERINFO ), sizeof( info ) );
		
#ifdef LEGACY_PROTOCOL
		if(com_legacyprotocol->integer == com_protocol->integer)
			clc.compat = qtrue;

		if(clc.compat)
			Info_SetValueForKey(info, "protocol", va("%i", com_legacyprotocol->integer));
		else
#endif
			Info_SetValueForKey(info, "protocol", va("%i", com_protocol->integer));
		Info_SetValueForKey(info, "qport", va("%i", port));
		Info_SetValueForKey(info, "challenge", va("%i", clc.challenge));
		Info_SetValueForKey(info, "version", com_target_shortversion->string);
		if (com_target_game->integer == target_game_e::TG_MOHTT) {
			// only send if maintt is loaded
			Info_SetValueForKey(info, "clientType", "Breakthrough");
		}

		Q_strncpyz(data, "connect ", sizeof(data));
    // TTimo adding " " around the userinfo string to avoid truncated userinfo on the server
    //   (Com_TokenizeString tokenizes around spaces)
    data[8] = '"';

		for(i=0;i<strlen(info);i++) {
			data[9+i] = info[i];	// + (clc.challenge)&0x3;
		}
    data[9+i] = '"';
		data[10+i] = 0;

    // NOTE TTimo don't forget to set the right data length!
		NET_OutOfBandData( NS_CLIENT, clc.serverAddress, (byte *) &data[0], i+10 );
		// the most current userinfo has been sent, so watch for any
		// newer changes to userinfo variables
		cvar_modifiedFlags &= ~CVAR_USERINFO;
		break;

	default:
		Com_Error( ERR_FATAL, "CL_CheckForResend: bad clc.state" );
	}
}

/*
===================
CL_DisconnectPacket

Sometimes the server can drop the client and the netchan based
disconnect can be lost.  If the client continues to send packets
to the server, the server will send out of band disconnect packets
to the client so it doesn't have to wait for the full timeout period.
===================
*/
void CL_DisconnectPacket( netadr_t from ) {
	if ( clc.state < CA_AUTHORIZING ) {
		return;
	}

	// if not from our server, ignore it
	if ( !NET_CompareAdr( from, clc.netchan.remoteAddress ) ) {
		return;
	}

	// drop the connection
    Com_Printf("Server disconnected for unknown reason\n");
    CL_Disconnect_f();
    UI_ForceMenuOff(qtrue);
    UI_PushMenu("serverdisconnected");
}


/*
===================
CL_MotdPacket

===================
*/
void CL_MotdPacket( netadr_t from ) {
	char	*challenge;
	char	*info;

	// if not from our server, ignore it
	if ( !NET_CompareAdr( from, cls.updateServer ) ) {
		return;
	}

	info = Cmd_Argv(1);

	// check challenge
	challenge = Info_ValueForKey( info, "challenge" );
	if ( strcmp( challenge, cls.updateChallenge ) ) {
		return;
	}

	challenge = Info_ValueForKey( info, "motd" );

	Q_strncpyz( cls.updateInfoString, info, sizeof( cls.updateInfoString ) );
	Cvar_Set( "cl_motdString", challenge );
}

/*
===================
CL_InitServerInfo
===================
*/
void CL_InitServerInfo( serverInfo_t *server, netadr_t *address ) {
	server->adr = *address;
	server->clients = 0;
	server->hostName[0] = '\0';
	server->mapName[0] = '\0';
	server->maxClients = 0;
	server->maxPing = 0;
	server->minPing = 0;
	server->ping = -1;
	server->game[0] = '\0';
	server->gameType = 0;
	server->netType = 0;
}

#define MAX_SERVERSPERPACKET	256

/*
===================
CL_ServersResponsePacket
===================
*/
void CL_ServersResponsePacket( const netadr_t* from, msg_t *msg, qboolean extended ) {
	int				i, j, count, total;
	netadr_t addresses[MAX_SERVERSPERPACKET];
	int				numservers;
	byte*			buffptr;
	byte*			buffend;
	
	Com_Printf("CL_ServersResponsePacket from %s\n", NET_AdrToStringwPort(*from));

	if (cls.numglobalservers == -1) {
		// state to detect lack of servers or lack of response
		cls.numglobalservers = 0;
		cls.numGlobalServerAddresses = 0;
	}

	// parse through server response string
	numservers = 0;
	buffptr    = msg->data;
	buffend    = buffptr + msg->cursize;

	// advance to initial token
	do
	{
		if(*buffptr == '\\' || (extended && *buffptr == '/'))
			break;
		
		buffptr++;
	} while (buffptr < buffend);

	while (buffptr + 1 < buffend)
	{
		// IPv4 address
		if (*buffptr == '\\')
		{
			buffptr++;

			if (buffend - buffptr < sizeof(addresses[numservers].ip) + sizeof(addresses[numservers].port) + 1)
				break;

			for(i = 0; i < sizeof(addresses[numservers].ip); i++)
				addresses[numservers].ip[i] = *buffptr++;

			addresses[numservers].type = NA_IP;
		}
		// IPv6 address, if it's an extended response
		else if (extended && *buffptr == '/')
		{
			buffptr++;

			if (buffend - buffptr < sizeof(addresses[numservers].ip6) + sizeof(addresses[numservers].port) + 1)
				break;
			
			for(i = 0; i < sizeof(addresses[numservers].ip6); i++)
				addresses[numservers].ip6[i] = *buffptr++;
			
			addresses[numservers].type = NA_IP6;
			addresses[numservers].scope_id = from->scope_id;
		}
		else
			// syntax error!
			break;
			
		// parse out port
		addresses[numservers].port = (*buffptr++) << 8;
		addresses[numservers].port += *buffptr++;
		addresses[numservers].port = BigShort( addresses[numservers].port );

		// syntax check
		if (*buffptr != '\\' && *buffptr != '/')
			break;
	
		numservers++;
		if (numservers >= MAX_SERVERSPERPACKET)
			break;
	}

	count = cls.numglobalservers;

	for (i = 0; i < numservers && count < MAX_GLOBAL_SERVERS; i++) {
		// build net address
		serverInfo_t *server = &cls.globalServers[count];

		// Tequila: It's possible to have sent many master server requests. Then
		// we may receive many times the same addresses from the master server.
		// We just avoid to add a server if it is still in the global servers list.
		for (j = 0; j < count; j++)
		{
			if (NET_CompareAdr(cls.globalServers[j].adr, addresses[i]))
				break;
		}

		if (j < count)
			continue;

		CL_InitServerInfo( server, &addresses[i] );
		// advance to next slot
		count++;
	}

	// if getting the global list
	if ( count >= MAX_GLOBAL_SERVERS && cls.numGlobalServerAddresses < MAX_GLOBAL_SERVERS )
	{
		// if we couldn't store the servers in the main list anymore
		for (; i < numservers && cls.numGlobalServerAddresses < MAX_GLOBAL_SERVERS; i++)
		{
			// just store the addresses in an additional list
			cls.globalServerAddresses[cls.numGlobalServerAddresses++] = addresses[i];
		}
	}

	cls.numglobalservers = count;
	total = count + cls.numGlobalServerAddresses;

	Com_Printf("%d servers parsed (total %d)\n", numservers, total);
}

/*
=================
CL_ConnectionlessPacket

Responses to broadcasts, etc
=================
*/
void CL_ConnectionlessPacket( netadr_t from, msg_t *msg ) {
	char	*s;
	char	*c;
	const char	*reason;
	
	if (cl_netprofile->integer) {
		NetProfileAddPacket(&cls.netprofile.inPackets, msg->cursize, NETPROF_PACKET_MESSAGE);
	}

	MSG_BeginReadingOOB( msg );
	MSG_ReadLong( msg );	// skip the -1

	MSG_ReadByte( msg ); // wombat: skip the direction byte

	s = MSG_ReadStringLine( msg );

	Cmd_TokenizeString( s );

	c = Cmd_Argv(0);

	Com_DPrintf ("CL packet %s: %s\n", NET_AdrToString(from), c);

	// challenge from the server we are connecting to
	if ( !Q_stricmp(c, "challengeResponse") ) {
		if ( clc.state != CA_CONNECTING && clc.state != CA_AUTHORIZING ) {
			Com_Printf( "Unwanted challenge response received.  Ignored.\n" );
		} else {
			// start sending challenge repsonse instead of challenge request packets
			clc.challenge = atoi(Cmd_Argv(1));
			clc.state = CA_CHALLENGING;
			clc.connectPacketCount = 0;
			clc.connectTime = -99999;

			// take this address as the new server address.  This allows
			// a server proxy to hand off connections to multiple servers
			clc.serverAddress = from;
			Com_DPrintf ("challengeResponse: %d\n", clc.challenge);
		}
		return;
	}

	// server connection
	if ( !Q_stricmp(c, "connectResponse") ) {
		if ( clc.state >= CA_CONNECTED ) {
			Com_Printf ("Dup connect received.  Ignored.\n");
			return;
		}
		if ( clc.state != CA_CHALLENGING ) {
			Com_Printf ("connectResponse packet while not connecting.  Ignored.\n");
			return;
		}
		if ( !NET_CompareBaseAdr( from, clc.serverAddress ) ) {
			Com_Printf( "connectResponse from a different address.  Ignored.\n" );
			Com_Printf( "%s should have been %s\n", NET_AdrToString( from ),
				NET_AdrToString( clc.serverAddress ) );
			return;
		}

#ifdef LEGACY_PROTOCOL
		Netchan_Setup(NS_CLIENT, &clc.netchan, from, Cvar_VariableValue("net_qport"),
			      clc.challenge, clc.compat);
#else
		Netchan_Setup(NS_CLIENT, &clc.netchan, from, Cvar_VariableValue("net_qport"),
			      clc.challenge, qfalse);
#endif

		clc.state = CA_CONNECTED;
		clc.lastPacketSentTime = -9999;		// send first packet immediately
		return;
	}

	// server responding to an info broadcast
	if ( !Q_stricmp(c, "infoResponse") ) {
		CL_DedicatedInfoResponse( from ); // HZM coop [dedicated_launch] the server we started is up
		CL_ServerInfoPacket( from, msg );
		return;
	}

	// server responding to a get playerlist
	if ( !Q_stricmp(c, "statusResponse") ) {
		CL_ServerStatusResponse( from, msg );
		return;
	}

	// a disconnect message from the server, which will happen if the server
	// dropped the connection but it is still getting packets from us
	if (!Q_stricmp(c, "disconnect")) {
		CL_DisconnectPacket( from );
		return;
	}

	// echo request from server
	if ( !Q_stricmp(c, "echo") ) {
		CL_NET_OutOfBandPrint(from, "%s", Cmd_Argv(1) );
		return;
	}

	// cd check
	if ( !Q_stricmp(c, "getKey") ) {
		clc.state = CA_AUTHORIZING;
		gcd_compute_response(cl_cdkey, Cmd_Argv(1), cls.gcdResponse, CDResponseMethod_NEWAUTH);
		CL_NET_OutOfBandPrint(from, "authorizeThis %s", cls.gcdResponse);
		return;
	}

	// global MOTD from id
	if ( !Q_stricmp(c, "motd") ) {
		CL_MotdPacket( from );
		return;
	}

	// echo request from server
	if ( !Q_stricmp(c, "print") ) {
		s = MSG_ReadString( msg );
		Q_strncpyz( clc.serverMessage, s, sizeof( clc.serverMessage ) );
		Com_Printf( "%s", s );
		return;
	}

	// echo request from server
	if ( !Q_strncmp(c, "getserversResponse", 18) ) {
		CL_ServersResponsePacket( &from, msg, qfalse );
		return;
	}

	// wombat: mohaa servers send this to reject clients
	if ( !Q_stricmp(c, "droperror") ) {
        reason = MSG_ReadString(msg);
        Com_Printf("Server dropped connection. Reason: %s", reason);
        HzmDiag_Event( "client: the server refused the connection: %s", reason ); // HZM coop [bug-3486]
		Cvar_Set("com_errorMessage", reason);
        CL_Disconnect_f();
        UI_ForceMenuOff(qtrue);
        UI_PushMenu("errormessage");
		return;
    }

    if (!Q_stricmp(c, "wrongver")) {
        reason = MSG_ReadString(msg);
        Cvar_Set("com_errorMessage", va("Server is version %s, you are using %s, from base '%s'", reason, com_target_shortversion->string, Cvar_VariableString("fs_basegame")));
        CL_Disconnect_f();
        UI_ForceMenuOff(qtrue);
        UI_PushMenu("errormessage");
        return;
    }

	// HZM coop [238] - rendezvous replies (peer endpoint / punch hole-openers / errors)
	if (RDV_HandleClientOOB(from, c)) {
		return;
	}

	Com_DPrintf ("Unknown connectionless packet command: \"%s\".\n", c);
}


/*
=================
CL_PacketEvent

A packet has arrived from the main event loop
=================
*/
void CL_PacketEvent( netadr_t from, msg_t *msg ) {
	int		headerBytes;

	clc.lastPacketTime = cls.realtime;

	if ( msg->cursize >= 4 && *(int *)msg->data == -1 ) {
		CL_ConnectionlessPacket( from, msg );
		return;
	}

	if ( clc.state < CA_CONNECTED ) {
		return;		// can't be a valid sequenced packet
	}

	if ( msg->cursize < 4 ) {
		Com_Printf ("%s: Runt packet\n",NET_AdrToString( from ));
		return;
	}

	//
	// packet from server
	//
	if ( !NET_CompareAdr( from, clc.netchan.remoteAddress ) ) {
		Com_DPrintf ("%s:sequenced packet without connection\n"
			,NET_AdrToString( from ) );
		// FIXME: send a client disconnect?
		return;
	}

	// HZM coop [bug-3486] duplicated / out-of-order packet: the same test Netchan_Process makes, counted here because its
	// qfalse also covers "a fragment, more to come"
	if ( ( LittleLong( *(int *)msg->data ) & 0x7fffffff ) <= clc.netchan.incomingSequence ) {
		CL_HzmNet( 1, 1 );
	}

	if (!CL_Netchan_Process( &clc.netchan, msg) ) {
		return;		// out of order, duplicated, etc
	}
	if ( clc.netchan.dropped > 0 ) {
		CL_HzmNet( 0, clc.netchan.dropped ); // HZM coop [bug-3486] packets lost before this one
	}

	// the header is different lengths for reliable and unreliable messages
	headerBytes = msg->readcount;

	// track the last message received so it can be returned in
	// client messages, allowing the server to detect a dropped
	// gamestate
	clc.serverMessageSequence = LittleLong( *(int *)msg->data );

	clc.lastPacketTime = cls.realtime;
	CL_ParseServerMessage( msg );

	//
	// we don't know if it is ok to save a demo message until
	// after we have parsed the frame
	//
	if ( clc.demorecording && !clc.demowaiting ) {
		CL_WriteDemoMessage( msg, headerBytes );
	}
}

/*
==================
CL_CheckTimeout

==================
*/
void CL_CheckTimeout( void ) {
	if (com_sv_running->integer) {
		// Added in OPM
		//  Timeout should not be possible if the client is hosting the game
		//  (like in single-player mode)
		return;
	}

	//
	// check timeout
	//
	if ( ( !CL_CheckPaused() || !paused->integer )
		&& clc.state >= CA_CONNECTED && clc.state != CA_CINEMATIC
	    && cls.realtime - clc.lastPacketTime > cl_timeout->value*1000) {
		if (++cl.timeoutcount > 5) {	// timeoutcount saves debugger
			const char* info;
			const char* maxclients;

			info = &cl.gameState.stringData[cl.gameState.stringOffsets[CS_SERVERINFO]];
			maxclients = Info_ValueForKey(info, "maxclients");
			//
			// Disconnect if the player isn't alone (single-player mode)
			//
			if (!maxclients || atoi(maxclients) != 1) {
				qboolean bConsoleState;

				bConsoleState = UI_ConsoleIsOpen();

				Com_Printf("\nServer connection timed out.\n");
				HzmDiag_Event( "client: server connection timed out (nothing received for %d s)", cl_timeout->integer ); // HZM coop [bug-3486]
				SV_Shutdown("\nServer connection timed out\n");

				if (com_cl_running && com_cl_running->integer) {
					CL_AbnormalDisconnect();
					CL_FlushMemory();
					CL_StartHunkUsers(qfalse);
				}

				UI_ForceMenuOff(qtrue);
				UI_PushMenu("servertimeout");

				if (bConsoleState) {
					UI_OpenConsole();
				} else {
					UI_CloseConsole();
				}
			}
		}
	} else {
		cl.timeoutcount = 0;
	}
}

/*
==================
CL_CheckPaused
Check whether client has been paused.
==================
*/
qboolean CL_CheckPaused(void)
{
	// if paused->modified is set, the cvar has only been changed in
	// this frame. Keep paused in this frame to ensure the server doesn't
	// lag behind.
	if(paused->integer)
		return qtrue;

	return qfalse;
}

//============================================================================

/*
==================
CL_CheckUserinfo

==================
*/
void CL_CheckUserinfo( void ) {
	char szSanitizedName[MAX_NAME_LENGTH];

	// don't add reliable commands when not yet connected
	if(clc.state < CA_CHALLENGING)
		return;

	// don't overflow the reliable command buffer when paused
	if(CL_CheckPaused())
		return;

	// send a reliable userinfo update if needed
	if(cvar_modifiedFlags & CVAR_USERINFO)
	{
		if (Com_SanitizeName(name->string, szSanitizedName, sizeof(szSanitizedName))) {
			Cvar_Set("name", szSanitizedName);
		}

		cvar_modifiedFlags &= ~CVAR_USERINFO;
		CL_AddReliableCommand( va("userinfo \"%s\"", Cvar_InfoString( CVAR_USERINFO ) ), qfalse );
	}
}

void CL_SetFrameNumber(int frameNumber) {
	if (!re.SetFrameNumber) {
		return;
	}

	re.SetFrameNumber(frameNumber);
}

/*
==================
CL_VerifyUpdate

Check for a new version and display a message box
when a new version is available
==================
*/
void CL_VerifyUpdate() {
    if (cl_updateNotified) {
        return;
    }

    int lastMajor, lastMinor, lastPatch;
    if (updateChecker.CheckNewVersion(lastMajor, lastMinor, lastPatch)) {
        cl_updateNotified = true;

        const char *updateText =
            va("A new update is available!\n"
               "The latest version is v%d.%d.%d (you are running v%s).\n"
               "Check https://github.com/openmoh/openmohaa for more.",
               lastMajor,
               lastMinor,
               lastPatch,
               PRODUCT_VERSION_NUMBER_STRING);

        UIMessageDialog::ShowMessageBox("Update available", updateText);
    }
}

/*
==================
CL_Frame

==================
*/
void CL_Frame ( int msec ) {

	if ( !com_cl_running->integer ) {
		return;
	}

	/* HZM coop [bug-3484]: the recommended graphics preset for this PC (cl_hzm_recommend.cpp). Returns at once
	   except once a second, and does real work only when the video-memory reading changes. */
	{
		void CL_HzmRecommend_Update(void);
		CL_HzmRecommend_Update();
	}

	/* HZM coop: mirror "actually playing" into ui_ingame so menus can gate widgets with
	   enabledcvar "ui_ingame" (e.g. the multiplayer room's DISCONNECT FROM GAME button,
	   which vanilla draws even at the pre-game menu). Works for hosts and joining clients
	   alike and can never go stale - it tracks the real connection state every frame. */
	{
		static int s_lastIngame = -1;
		int        ingame       = (clc.state == CA_ACTIVE) ? 1 : 0;
		if ( ingame != s_lastIngame ) {
			Cvar_SetValue( "ui_ingame", ingame );
			s_lastIngame = ingame;
		}
	}

	CL_BugReportFrame( msec ); // HZM coop [bugreport_parity bug-3278] in-map fps, the report screenshot, the upload status
	CL_DedicatedFrame(); // HZM coop [dedicated_launch] watch / poll / stop the dedicated server this game started

#ifdef USE_CURL
	if(clc.downloadCURLM) {
		CL_cURL_PerformDownload();
		// we can't process frames normally when in disconnected
		// download mode since the ui vm expects clc.state to be
		// CA_CONNECTED
		if(clc.cURLDisconnected) {
			cls.realFrametime = msec;
			cls.frametime = msec;
			cls.realtime += cls.frametime;
			SCR_UpdateScreen();
			S_Update();
			cls.framecount++;
			return;
		}
	}
#endif

	if (CL_FinishedIntro()) {
		if (clc.state == CA_DISCONNECTED) {
			if (!UI_MenuActive() && !com_sv_running->integer) {
				// if disconnected, bring up the menu
				S_StopAllSounds2(qtrue);
				S_TriggeredMusic_PlayIntroMusic();
				UI_MenuEscape("main");
			}

            CL_VerifyUpdate();
		} else if (clc.state == CA_CINEMATIC) {
			UI_ForceMenuOff(qtrue);
		}
	}

	// if recording an avi, lock to a fixed fps
	if ( CL_VideoRecording( ) && cl_aviFrameRate->integer && msec) {
		// save the current screen
		if ( clc.state == CA_ACTIVE || cl_forceavidemo->integer) {
			CL_TakeVideoFrame( );

			// fixed time for next frame'
			msec = (int)ceil( (1000.0f / cl_aviFrameRate->value) * com_timescale->value );
			if (msec == 0) {
				msec = 1;
			}
		}
	}

	if( cl_autoRecordDemo->integer ) {
		if( clc.state == CA_ACTIVE && !clc.demorecording && !clc.demoplaying ) {
			// If not recording a demo, and we should be, start one
			qtime_t	now;
			const char	*nowString;
			char		*p;
			char		mapName[ MAX_QPATH ];
			char		serverName[ MAX_OSPATH ];

			Com_RealTime( &now );
			nowString = va( "%04d%02d%02d%02d%02d%02d",
					1900 + now.tm_year,
					1 + now.tm_mon,
					now.tm_mday,
					now.tm_hour,
					now.tm_min,
					now.tm_sec );

			Q_strncpyz( serverName, clc.servername, MAX_OSPATH );
			// Replace the ":" in the address as it is not a valid
			// file name character
			p = strstr( serverName, ":" );
			if( p ) {
				*p = '.';
			}

			Q_strncpyz( mapName, COM_SkipPath( cl.mapname ), sizeof( cl.mapname ) );
			COM_StripExtension(mapName, mapName, sizeof(mapName));

			Cbuf_ExecuteText( EXEC_NOW,
					va( "record %s-%s-%s", nowString, serverName, mapName ) );
		}
		else if( clc.state != CA_ACTIVE && clc.demorecording ) {
			// Recording, but not CA_ACTIVE, so stop recording
			CL_StopRecord_f( );
		}
	}

	// save the msec before checking pause
	cls.realFrametime = msec;

	// decide the simulation time
	cls.frametime = msec;

	cls.realtime += cls.frametime;

	if ( cl_timegraph->integer ) {
		SCR_DebugGraph ( cls.realFrametime * 0.25 );
	}

	cls.timeScaled = com_timescale->integer != 1;

	//
	// Added in 2.0: network profiling
	//
	if (cl_netprofile->integer) {
		if (!cls.netprofile.initialized) {
			memset(&cls.netprofile, 0, sizeof(cls.netprofile));
			cls.netprofile.initialized = qtrue;
		}

		if (cls.netprofile.rate != cl_rate->integer) {
            cls.netprofile.rate = cl_rate->integer;

            if (cls.netprofile.rate < 1000) {
				cls.netprofile.rate = 1000;
            } else if (cls.netprofile.rate > 90000) {
				cls.netprofile.rate = 90000;
			}
		}

		cls.netprofile.outPackets.updateTime = Com_Milliseconds();
		cls.netprofile.inPackets.updateTime = cls.netprofile.outPackets.updateTime;
	} else {
		cls.netprofile.initialized = qfalse;
	}

	// see if we need to update any userinfo
	CL_CheckUserinfo();

	// if we haven't gotten a packet in a long time,
	// drop the connection
	CL_CheckTimeout();

	// HZM coop: hold-to-repeat for build-mode keys (no-op unless coop_build 1)
	CL_CoopBuildKeyRepeat();

	// send intentions now
	CL_SendCmd();

	// resend a connection request if necessary
	CL_CheckForResend();

	// HZM coop [238] - rendezvous join-request resends/timeout
	RDV_ClientFrame();

	// decide on the serverTime to render
	CL_SetCGameTime();

	// set the time if we loaded a save
	if( SV_DoSaveGame() )
	{
		if( cl.serverTime < svs.time ) {
			cl.serverTime = svs.time;
		}
	}

	L_ProcessPendingEvents();

    // Added in OPM
    CL_UpdateMouse();

	// update the screen
	SCR_UpdateScreen();

	// update audio
	S_Update();

	// advance local effects for next frame
	SCR_RunCinematic();

	// HZM coop bug-3234: the networked briefing video (serverinfo coop_briefvideo)
	CL_CoopBriefVideoFrame();

	cls.framecount++;
}


//============================================================================

/*
================
CL_RefPrintf

DLL glue
================
*/
void QDECL CL_RefPrintf( int print_level, const char *fmt, ...) {
	va_list		argptr;
	char		msg[MAXPRINTMSG];

	va_start(argptr, fmt);
	Q_vsnprintf(msg, sizeof(msg), fmt, argptr);
	va_end(argptr);

	if (print_level == PRINT_ALL) {
		Com_Printf("%s", msg);
	}
	else if (print_level == PRINT_WARNING || print_level == PRINT_DEVELOPER) {
		Com_DPrintf("%s", msg);
	}
	else if (print_level == PRINT_DEVELOPER_2 && developer->integer) {
		Com_DPrintf("%s", msg);
	}
}

/*
============
CL_RefSetPerformanceCounters
============
*/
void CL_RefSetPerformanceCounters( int total_tris, int total_verts, int total_texels, int world_tris, int world_verts, int character_lights )
{
	cls.total_tris = total_tris;
	cls.total_verts = total_verts;
	cls.total_texels = total_texels;
	cls.world_tris = world_tris;
	cls.world_verts = world_verts;
	cls.character_lights = character_lights;
}

/*
============
CL_ShutdownRef
============
*/
void CL_ShutdownRef( void ) {
	cls.rendererReady = qfalse;	// HZM bug-2981
	if ( re.Shutdown ) {
		re.Shutdown( qtrue );
	}

	Com_Memset( &re, 0, sizeof( re ) );
	cl_hzmRefGen++; // HZM coop [bug-3486]

#ifdef USE_RENDERER_DLOPEN
	if ( rendererLib ) {
		Sys_UnloadLibrary( rendererLib );
		rendererLib = NULL;
	}
#endif
}

/*
============
CL_HzmRendererSymbol

HZM coop [bug-3486] an optional exported function of the renderer DLL (R_HzmPerf), NULL when this renderer has none.
============
*/
void *CL_HzmRendererSymbol( const char *name ) {
#ifdef USE_RENDERER_DLOPEN
	return rendererLib ? Sys_LoadFunction( rendererLib, name ) : NULL;
#else
	return NULL;
#endif
}

/*
============================
CL_StartHunkUsers

After the server has cleared the hunk, these will need to be restarted
This is the only place that any of these functions are called from
============================
*/
void CL_StartHunkUsers( qboolean rendererOnly ) {
	if( !com_cl_running ) {
		return;
	}

	if ( !com_cl_running->integer ) {
		return;
	}

	if ( !cls.rendererRegistered ) {
		cls.rendererRegistered = qtrue;
		CL_BeginRegistration();
		UI_ResolutionChange();
	}

	if( !cls.cgameStarted ) {
		if( clc.state >= CA_LOADING && clc.state != CA_CINEMATIC ) {
			UI_LoadScreenPrimeMaterials(); // HZM bug-3015: register the loading menu's materials, no draw (cl_ui.cpp)
			CL_InitCGame();
		}
	}

	if ( !cls.uiStarted ) {
		cls.uiStarted = qtrue;
		CL_InitializeUI();
	}
}

/*
============
CL_RefMalloc
============
*/
void *CL_RefMalloc(int size) {
	return Z_TagMalloc( size, TAG_RENDERER );
}

/*
============
CL_RefFree
============
*/
void CL_RefFree(void* ptr) {
	Z_Free(ptr);
}

/*
============
CL_RefClear
============
*/
void CL_RefClear( void )
{
	Z_FreeTags( TAG_RENDERER );
}

/*
============
CL_RefStaticMallocDebug
============
*/
void* CL_RefStaticMallocDebug(size_t size, const char* label, const char* file, int line) {
    void* ptr = Z_TagMalloc(size, TAG_STATIC_RENDERER);
    Com_Memset(ptr, 0, size);
    return ptr;
}

/*
============
CL_RefStaticMalloc
============
*/
void *CL_RefStaticMalloc(int size, ha_pref preference ) {
	void *ptr = Z_TagMalloc( size, TAG_STATIC_RENDERER );
	Com_Memset( ptr, 0, size );
	return ptr;
}

/*
============
CL_RefStaticMallocTemp
============
*/
void* CL_RefStaticMallocTemp(int size) {
	return Hunk_AllocateTempMemory(size);
}

/*
============
CL_RefStaticClear
============
*/
void CL_RefStaticClear( void ) {
	Z_FreeTags( TAG_STATIC_RENDERER );
}

/*
============
CL_CG_PermanentMark
============
*/
int CL_CG_PermanentMark(vec3_t origin, vec3_t dir, float orientation,
	float fSScale, float fTScale, float red, float green, float blue, float alpha,
	qboolean dolighting, float fSCenter, float fTCenter,
	markFragment_t *pMarkFragments, void *pPolyVerts )
{
	if( cge ) {
		return cge->CG_PermanentMark( origin, dir, orientation,
			fSScale, fTScale, red, green, blue, alpha,
			dolighting, fSCenter, fTCenter,
			pMarkFragments, pPolyVerts );
	}

	return 0;
}

/*
============
CL_CG_PermanentTreadMarkDecal
============
*/
int CL_CG_PermanentTreadMarkDecal( treadMark_t *pTread, qboolean bStartSegment, qboolean dolighting, markFragment_t *pMarkFragments, void *pVoidPolyVerts ) {
	if( cge ) {
		return cge->CG_PermanentTreadMarkDecal( pTread, bStartSegment, dolighting, pMarkFragments, pVoidPolyVerts );
	}

	return 0;
}

/*
============
CL_CG_PermanentUpdateTreadMark
============
*/
int CL_CG_PermanentUpdateTreadMark( treadMark_t *pTread, float fAlpha, float fMinSegment, float fMaxSegment, float fMaxOffset, float fTexScale ) {
	if( cge ) {
		return cge->CG_PermanentUpdateTreadMark( pTread, fAlpha, fMinSegment, fMaxSegment, fMaxOffset, fTexScale );
	}

	return 0;
}

/*
============
CL_CG_ProcessInitCommands
============
*/
void CL_CG_ProcessInitCommands( dtiki_t *tiki, refEntity_t *ent ) {
	if( cge ) {
		return cge->CG_ProcessInitCommands( tiki, ent );
	}
}

/*
============
CL_CG_EndTiki
============
*/
void CL_CG_EndTiki( dtiki_t *tiki ) {
	if( cge ) {
		return cge->CG_EndTiki( tiki );
	}
}

/*
============
CL_CG_EndTiki
============
*/
extern "C"
int CL_ScaledMilliseconds(void) {
	return Sys_Milliseconds()*com_timescale->value;
}

/*
============
CL_RefFS_WriteFile
============
*/
void CL_RefFS_WriteFile(const char* qpath, const void* buffer, int size) {
	FS_WriteFile(qpath, buffer, size);
}

/*
============
CL_RefFS_ListFiles
============
*/
char** CL_RefFS_ListFiles(const char* name, const char* extension, int* numfilesfound) {
	return FS_ListFiles(name, extension, qtrue, numfilesfound);
}

/*
============
CL_RefCIN_UploadCinematic
============
*/
void CL_RefCIN_UploadCinematic(int handle) {
}

/*
============
CL_RefTIKI_GetNumChannels
============
*/
int CL_RefTIKI_GetNumChannels(dtiki_t* tiki) {
	return tiki->m_boneList.NumChannels();
}

/*
============
CL_RefTIKI_GetLocalChannel
============
*/
int CL_RefTIKI_GetLocalChannel(dtiki_t* tiki, int channel) {
	return tiki->m_boneList.LocalChannel(channel);
}

/*
============
CL_RefTIKI_GetLocalFromGlobal
============
*/
int CL_RefTIKI_GetLocalFromGlobal(dtiki_t* tiki, int channel) {
    return tiki->m_boneList.GetLocalFromGlobal(channel);
}

/*
============
CL_RefSKEL_GetMorphWeightFrame
============
*/
int CL_RefSKEL_GetMorphWeightFrame(void* skeletor, int index, float time, int* data) {
    return ((skeletor_c*)skeletor)->GetMorphWeightFrame(index, time, data);
}

/*
============
CL_RefSKEL_GetBoneParent
============
*/
int CL_RefSKEL_GetBoneParent(void* skeletor, int boneIndex) {
	return ((skeletor_c*)skeletor)->GetBoneParent(boneIndex);
}

/*
============
CL_GetRefSequence
============
*/
int CL_GetRefSequence(void) {
	return cls.refSequence;
}

/*
============
CL_IsRendererLoaded
============
*/
qboolean CL_IsRendererLoaded(void) {
	// HZM bug-2981: 'loaded' means usable. A freshly reloaded renderer DLL (vid_restart) has its exports but has not
	// run R_Init until re.BeginRegistration returns - a UI font loaded in that window got a NULL shader (bug-1181
	// guard) and, with the sequence already bumped, was never reloaded: every label/console line drew as nothing.
	return ( re.Shutdown != NULL && cls.rendererReady ) ? qtrue : qfalse;
}

/*
============
CL_InitRef
============
*/
void CL_FillUIImports( void ); // cl_ui.cpp - bug-2975
void CL_InitRef( void ) {
	refimport_t	ri;
	refexport_t	*ret;
#ifdef USE_RENDERER_DLOPEN
	GetRefAPI_t		GetRefAPI;
	char			dllName[MAX_OSPATH];
#endif

	Com_Printf( "----- Initializing Renderer ----\n" );

#define RENDERER_ARCH_DLL_EXT DLL_SUFFIX DLL_EXT

#ifdef USE_RENDERER_DLOPEN
	cl_renderer = Cvar_Get("cl_renderer", "opengl1", CVAR_ARCHIVE | CVAR_LATCH);

	Com_sprintf(dllName, sizeof(dllName), "renderer_%s" RENDERER_ARCH_DLL_EXT, cl_renderer->string);

	if(!(rendererLib = Sys_LoadDll(dllName, qfalse)) && strcmp(cl_renderer->string, cl_renderer->resetString))
	{
		Com_Printf("failed:\n\"%s\"\n", Sys_LibraryError());
		Cvar_ForceReset("cl_renderer");

		Com_sprintf(dllName, sizeof(dllName), "renderer_%s" RENDERER_ARCH_DLL_EXT, cl_renderer->resetString);
		rendererLib = Sys_LoadDll(dllName, qfalse);
	}

	if(!rendererLib)
	{
		Com_Printf("failed:\n\"%s\"\n", Sys_LibraryError());
		Com_Error(ERR_FATAL, "Failed to load renderer");
	}

	GetRefAPI = (GetRefAPI_t)Sys_LoadFunction(rendererLib, "GetRefAPI");
	if(!GetRefAPI)
	{
		Com_Error(ERR_FATAL, "Can't load symbol GetRefAPI: '%s'",  Sys_LibraryError());
	}
#endif

	ri.Cmd_AddCommand = Cmd_AddCommand;
	ri.Cmd_RemoveCommand = Cmd_RemoveCommand;
	ri.Cmd_Argc = Cmd_Argc;
	ri.Cmd_Argv = Cmd_Argv;
	ri.Cmd_ExecuteText = Cbuf_ExecuteText;
	ri.Printf = CL_RefPrintf;
	ri.Error = Com_Error;
	ri.Milliseconds = CL_ScaledMilliseconds;
	ri.LV_ConvertString = Sys_LV_CL_ConvertString;
	ri.Malloc = CL_RefMalloc;
	ri.Free = CL_RefFree;
	ri.Clear = CL_RefClear;
    ri.Hunk_Clear = CL_RefStaticClear;
#ifdef HUNK_DEBUG
    ri.Hunk_AllocDebug = CL_RefStaticMallocDebug;
#else
    ri.Hunk_Alloc = CL_RefStaticMalloc;
#endif
	ri.Hunk_AllocateTempMemory = CL_RefStaticMallocTemp;
	ri.Hunk_FreeTempMemory = Hunk_FreeTempMemory;
	ri.CM_DrawDebugSurface = CM_DrawDebugSurface;

	ri.FS_OpenFile = FS_FOpenFileRead;
	ri.FS_OpenFileWrite = FS_FOpenFileWrite;
	ri.FS_CloseFile = FS_FCloseFile;
	ri.FS_Read = FS_Read;
	ri.FS_Write = FS_Write;
	ri.FS_Seek = FS_Seek;
	ri.FS_ReadFile = FS_ReadFile;
	ri.FS_ReadFileEx = FS_ReadFileEx;
	ri.FS_FreeFile = FS_FreeFile;
	ri.FS_WriteFile = CL_RefFS_WriteFile;
	ri.FS_FreeFileList = FS_FreeFileList;
	ri.FS_ListFiles = CL_RefFS_ListFiles;
	ri.FS_FileIsInPAK = FS_FileIsInPAK;
	ri.FS_FileExists = FS_FileExists;
	ri.FS_CanonicalFilename = FS_CanonicalFilename;
	ri.Cvar_Get = Cvar_Get;
	ri.Cvar_Set = Cvar_Set;
	ri.Cvar_SetValue = Cvar_SetValue;
	ri.Cvar_SetDefault = Cvar_SetDefault;

    ri.CM_EntityString = CM_EntityString;
    ri.CM_MapTime = CM_MapTime;
    ri.CM_BoxTrace = CM_BoxTrace;
	ri.CG_PermanentMark = CL_CG_PermanentMark;
	ri.CG_PermanentTreadMarkDecal = CL_CG_PermanentTreadMarkDecal;
	ri.CG_PermanentUpdateTreadMark = CL_CG_PermanentUpdateTreadMark;
	ri.CM_TerrainSquareType = CM_TerrainSquareType;
	ri.CG_ProcessInitCommands = CL_CG_ProcessInitCommands;
	ri.CG_EndTiki = CL_CG_EndTiki;
	ri.SetPerformanceCounters = CL_RefSetPerformanceCounters;

    ri.DebugLines = &DebugLines;
    ri.numDebugLines = &numDebugLines;
    ri.DebugStrings = &DebugStrings;
    ri.numDebugStrings = &numDebugStrings;

	ri.TIKI_OrientationInternal = TIKI_OrientationInternal;
	ri.TIKI_IsOnGroundInternal = TIKI_IsOnGroundInternal;
	ri.TIKI_SetPoseInternal = TIKI_SetPoseInternal;
	ri.TIKI_Alloc = TIKI_Alloc;
	ri.GetRadiusInternal = TIKI_GetRadiusInternal;
	ri.GetCentroidRadiusInternal = TIKI_GetCentroidRadiusInternal;
	ri.GetFrameInternal = TIKI_GetFrameInternal;

	//
	// ioq3 stuff
	//
    ri.Cvar_CheckRange = Cvar_CheckRange;
    ri.Cvar_SetDescription = Cvar_SetDescription;
    ri.Cvar_VariableIntegerValue = Cvar_VariableIntegerValue;

    ri.CIN_UploadCinematic = CL_RefCIN_UploadCinematic;
    ri.CIN_PlayCinematic = CIN_PlayCinematic;
    ri.CIN_RunCinematic = CIN_RunCinematic;
    ri.CL_WriteAVIVideoFrame = CL_WriteAVIVideoFrame;

    ri.IN_Init = IN_Init;
    ri.IN_Shutdown = IN_Shutdown;
    ri.IN_Restart = IN_Restart;
    ri.ftol = Q_ftol;

    ri.Sys_SetEnv = Sys_SetEnv;
    ri.Sys_GLimpSafeInit = Sys_GLimpSafeInit;
    ri.Sys_GLimpInit = Sys_GLimpInit;
    ri.Sys_LowPhysicalMemory = Sys_LowPhysicalMemory;

	//
	// Added in OPM
	//
	ri.UI_LoadResource = UI_LoadResource;
	ri.CM_PointLeafnum = CM_PointLeafnum;
	ri.CM_LeafCluster = CM_LeafCluster;

    ri.TIKI_CalcLodConsts = TIKI_CalcLodConsts;
    ri.TIKI_CalculateBounds = TIKI_CalculateBounds;
    ri.TIKI_FindTiki = TIKI_FindTiki;
    ri.TIKI_RegisterTikiFlags = TIKI_RegisterTikiFlags;
    ri.TIKI_GetSkeletor = TIKI_GetSkeletor;
    ri.TIKI_GetSkel = TIKI_GetSkel;
    ri.TIKI_GetSkelAnimFrame = TIKI_GetSkelAnimFrame;
    ri.TIKI_GlobalRadius = TIKI_GlobalRadius;
    ri.TIKI_FindSkelByHeader = TIKI_FindSkelByHeader;
	ri.TIKI_GetNumChannels = CL_RefTIKI_GetNumChannels;
    ri.TIKI_GetLocalChannel = CL_RefTIKI_GetLocalChannel;
    ri.TIKI_GetLocalFromGlobal = CL_RefTIKI_GetLocalFromGlobal;

	ri.SKEL_GetBoneParent = CL_RefSKEL_GetBoneParent;
	ri.SKEL_GetMorphWeightFrame = CL_RefSKEL_GetMorphWeightFrame;

	ret = GetRefAPI( REF_API_VERSION, &ri );
	cl_hzmRefGen++; // HZM coop [bug-3486]

#if defined __USEA3D && defined __A3D_GEOM
	hA3Dg_ExportRenderGeom (ret);
#endif

	Com_Printf( "-------------------------------\n");

	if ( !ret ) {
		Com_Error (ERR_FATAL, "Couldn't initialize refresh" );
	}

	re = *ret;

	// HZM bug-2975: re-point the UI's renderer imports (uii.Rend_*) at the NEW renderer DLL right here. They were
	// refreshed only by UI_ResolutionChange (CL_StartHunkUsers), which runs AFTER re.BeginRegistration - and the UI
	// calls through them in between (UIFont::CheckRefreshFont -> uii.Rend_LoadFont). On a vid_restart that call went
	// into the UNLOADED previous renderer: harmless only while Windows happened to map the new DLL at the same address,
	// an access violation at R_LoadFont+0 in "renderer_opengl2flip.dll_unloaded" otherwise (2026-09-26 AA test).
	// Copying function pointers is safe before R_Init; the functions themselves already guard an unready renderer
	// (bug-1181/1145).
	CL_FillUIImports();

	// unpause so the cgame definately gets a snapshot and renders a frame
	Cvar_Set( "cl_paused", "0" );

	cls.refSequence++;
}


//===========================================================================================

qboolean CL_SetVidMode( int mode ) {
	qboolean ret;

	ret = re.SetMode( mode, &cls.glconfig );

	if( cge ) {
		cge->CG_GetRendererConfig();
	}

	UI_ResolutionChange();
	return ret;
}

void CL_SetFullscreen( qboolean fullscreen ) {
	re.SetFullscreen( fullscreen );

	if( cge ) {
		cge->CG_GetRendererConfig();
	}
}

void CL_VidMode_f( void ) {
	char	text[ 16 ];
	int		mode;

	if( Cmd_Argc() != 2 ) {
		Com_Printf( "Usage: vidmode [modenum]\n" );
		return;
	}

	mode = atoi( Cmd_Argv( 1 ) );

	if( CL_SetVidMode( mode ) ) {
		Com_sprintf( text, sizeof( text ), "%d", mode );
		Cvar_Set( "r_mode", text );
	}
}

void CL_TikiInfoCommand_f( void ) {
	dtiki_t		*tiki;
	qhandle_t	hModel;
	const char	*name;
	char		modelname[ 128 ];

	if( Cmd_Argc() != 2 ) {
		Com_Printf( "usage: tiki tikiname\n" );
		return;
	}

	name = Cmd_Argv( 1 );

	if( strchr( name, '/' ) ) {
		Q_strncpyz( modelname, name, sizeof( modelname ) );
	} else {
		Q_strncpyz( modelname, "models/", sizeof( modelname ) );
		Q_strcat( modelname, sizeof( modelname ), name );
	}

	COM_DefaultExtension( modelname, sizeof( modelname ), ".tik" );
	hModel = re.RegisterModel( modelname );
	tiki = re.R_Model_GetHandle( hModel );

	TIKI_ModelInfo( tiki );
}


//===========================================================================================


/*
===============
CL_Video_f

video
video [filename]
===============
*/
void CL_Video_f( void )
{
  char  filename[ MAX_OSPATH ];
  int   i, last;

  if( !clc.demoplaying )
  {
    Com_Printf( "The video command can only be used when playing back demos\n" );
    return;
  }

  if( Cmd_Argc( ) == 2 )
  {
    // explicit filename
    Com_sprintf( filename, MAX_OSPATH, "videos/%s.avi", Cmd_Argv( 1 ) );
  }
  else
  {
    // scan for a free filename
    for( i = 0; i <= 9999; i++ )
    {
      int a, b, c, d;

      last = i;

      a = last / 1000;
      last -= a * 1000;
      b = last / 100;
      last -= b * 100;
      c = last / 10;
      last -= c * 10;
      d = last;

      Com_sprintf( filename, MAX_OSPATH, "videos/video%d%d%d%d.avi",
          a, b, c, d );

      if( !FS_FileExists( filename ) )
        break; // file doesn't exist
    }

    if( i > 9999 )
    {
      Com_Printf( S_COLOR_RED "ERROR: no free file names to create video\n" );
      return;
    }
  }

  CL_OpenAVIForWriting( filename );
}

/*
===============
CL_StopVideo_f
===============
*/
void CL_StopVideo_f( void )
{
  CL_CloseAVI( );
}

/*
===============
CL_GenerateQKey

test to see if a valid QKEY_FILE exists.  If one does not, try to generate
it by filling it with 2048 bytes of random data.
===============
*/
static void CL_GenerateQKey(void)
{
	int len = 0;
	unsigned char buff[ QKEY_SIZE ];
	fileHandle_t f;

	len = FS_SV_FOpenFileRead( QKEY_FILE, &f );
	FS_FCloseFile( f );
	if( len == QKEY_SIZE ) {
		Com_Printf( "QKEY found.\n" );
		return;
	}
	else {
		if( len > 0 ) {
			Com_Printf( "QKEY file size != %d, regenerating\n",
				QKEY_SIZE );
		}

		Com_Printf( "QKEY building random string\n" );
		Com_RandomBytes( buff, sizeof(buff) );

		f = FS_SV_FOpenFileWrite( QKEY_FILE );
		if( !f ) {
			Com_Printf( "QKEY could not open %s for write\n",
				QKEY_FILE );
			return;
		}
		FS_Write( buff, sizeof(buff), f );
		FS_FCloseFile( f );
		Com_Printf( "QKEY generated\n" );
	}
}

/*
====================
CL_Init
====================
*/
/*
HZM coop: name the killer before dying. An uncaught C++ exception (in the field:
std::bad_alloc when a 16GB machine exhausts commit during HD-asset precache at map load)
previously ended as a bare WER 0xC0000409 fast-fail with no log line, so crash reports
could not say WHY. Log what() to qconsole.log and to hzm_fatal.log (cwd, next to the
updater log where report_problem.ps1 picks it up), then abort() so WER still writes the
minidump. set_new_handler fires BEFORE the bad_alloc unwind, catching the OOM case even
if the terminate path itself cannot allocate.
*/
static void HZM_LogFatal( const char *what ) {
	FILE *f = fopen( "hzm_fatal.log", "ab" );
	if ( f ) {
		fprintf( f, "FATAL: %s (map load/frame; see qconsole.log tail)\n", what );
		fclose( f );
	}
	fprintf( stderr, "FATAL: %s\n", what );
	Com_Printf( "^~^~^ FATAL: %s\n", what );
}

static void HZM_NewHandler() {
	std::set_new_handler( NULL );      // next failed new throws bad_alloc normally
	HZM_LogFatal( "out of memory (operator new failed - close background apps / add RAM or pagefile)" );
}

static void HZM_TerminateHandler() {
	static volatile long entered = 0;
	if ( ++entered == 1 ) {
		const char *what = "terminate() - unknown non-std exception";
		try {
			if ( std::current_exception() ) {
				std::rethrow_exception( std::current_exception() );
			} else {
				what = "terminate() with no active exception";
			}
		}
		catch ( const std::bad_alloc & ) { what = "std::bad_alloc - OUT OF MEMORY during load"; }
		catch ( const std::exception &e ) { what = e.what(); }
		catch ( const char *s ) { what = s; }
		catch ( ... ) {}
		HZM_LogFatal( what );
	}
	abort();
}

/*
====================
CL_PinToggle_f

HZM coop [user 08-06] bug-1503 - "coop_pintoggle <gi>": toggle a challenge pin from ANYWHERE,
including the fully disconnected main menu, where there is no server and no script VM to run the
existing (server-side) chal_pin_toggle logic in coop_mod/challenges.scr. That existing system only
works while connected, because it works by appending a token to the "name" cvar and waiting for a
server-side script (coop_mod/player.scr::manageNamechange) to notice - and there is no server at
all in the disconnected Service Record screen.

Source of truth here is FIVE separate client cvars, coop_pin1..coop_pin5, each holding one
catalogue index (empty string = unused slot) - deliberately NOT one comma-joined list, because
Morpheus script (coop_mod/challenges.scr, which must read this back on connect) has no confirmed
string-split/substring primitives; five clean info_valueforkey lookups avoid needing any parsing at
all server-side. Each is CVAR_ARCHIVE (persists disconnected, like the rest of Service Record's
saved progress) AND CVAR_USERINFO - mirroring dm_playermodel's existing CVAR_ARCHIVE|CVAR_USERINFO
pattern (below, ~3843) for exactly this "client picks something while disconnected, server needs to
find out on connect" problem. A server-side script reads all five back via self.userinfo on connect
and reconciles self.flags["coop_pinN"] (see coop_mod/challenges.scr::chal_pin_importUserinfo).

Also flips coop_uiP<gi> locally so the Service Record marker responds INSTANTLY with no server
round-trip (which may not exist at all right now). If a server connection IS currently active, this
additionally fires the existing name-bus dispatch so the live Mission Objectives HUD and self.flags
update immediately too, instead of waiting for a future reconnect to reconcile.
====================
*/
#define COOP_PIN_MAX 5

/*
====================
CL_ChallengeIsDone / CL_SyncSR_f

HZM coop [user 08-07] - "is challenge <gi> already complete?", answered CLIENT-SIDE with no server
round trip. Source of truth is coop_uiN<gi>, the archived "N/T" progress string that
challenges.scr::chal_ui_writeOne already maintains for every challenge and that persists across
sessions for the disconnected Service Record. Parsing it here means the done-state is known the
instant the menu opens - the earlier approach read only coop_uiD<gi>, which is pushed on the 30s
chal_flush cycle, so nothing worked until the player had been in-game half a minute (and never at
all while browsing disconnected).

CL_SyncSR_f ("coop_srsync") walks every challenge and republishes coop_uiD<i> from that same
parse, so the Service Record's done-checkmark Labels (enabledcvar coop_uiD<i>, which can only test
an integer and so cannot parse "N/T" themselves) are correct too. The generated menu runs it from
every tab button, so simply opening/switching a tab refreshes the whole page.
====================
*/
static qboolean CL_ChallengeIsDone( int gi )
{
	char        nameBuf[16];
	const char *progress;
	const char *slash;
	int         have, target;

	if ( gi < 0 ) {
		return qfalse;
	}
	// [user 08-07] NO coop_uiD fast path here. Reading the flag we are about to recompute made
	// the result STICKY: once any row was set to 1 it could never go back to 0, so a stale or
	// wrongly-set flag latched permanently and marked unfinished rows as done. coop_uiN (the
	// "N/T" progress string) is the single source of truth; coop_uiD is purely an output.
	Com_sprintf( nameBuf, sizeof( nameBuf ), "coop_uiN%d", gi );
	progress = Cvar_VariableString( nameBuf );
	if ( !progress || !progress[0] ) {
		return qfalse;	// never tracked - treat as not done
	}
	slash = strchr( progress, '/' );
	if ( !slash ) {
		return qfalse;
	}
	have   = atoi( progress );
	target = atoi( slash + 1 );
	return ( target > 0 && have >= target ) ? qtrue : qfalse;
}

void CL_SyncSR_f( void )
{
	char     nameBuf[20];
	char     doneBuf[20];
	int      i;
	qboolean onMedalsPage;

	// [user 08-07] walk the WHOLE range and write every entry - do NOT stop at the first missing
	// coop_uiN. An earlier revision broke out on the first gap, so any coop_uiD left at "1" by the
	// old sticky-flag bug (they are CVAR_ARCHIVE, so they persist across sessions) was never
	// cleared and every row rendered as completed. A row with no progress cvar is simply not done.
	for ( i = 0; i < 1024; i++ ) {
		Com_sprintf( doneBuf, sizeof( doneBuf ), "coop_uiD%d", i );
		Cvar_Get( doneBuf, "0", CVAR_ARCHIVE );
		Cvar_Set( doneBuf, CL_ChallengeIsDone( i ) ? "1" : "0" );
	}

	// [user 08-07] PIN STATE IS DERIVED, NOT TRACKED. The row markers (coop_uiP<i>) and the
	// "n/5" summary (coop_pinCount) were each maintained independently as pins changed, so they
	// drifted apart - the user saw "3/5 pinned" with not one box actually marked. Both are now
	// rebuilt here from the single source of truth, the five coop_pin1..5 slots that
	// CL_PinToggle_f writes: clear every marker, re-mark exactly the slots that are filled, and
	// set the count to the same tally. They cannot disagree because they come from one pass.
	{
		char slotName[16];
		char markName[16];
		int  pinIdx[COOP_PIN_MAX];
		int  pinCount = 0;
		int  k;

		// [user 2026-08-18] slots hold CIDS now (see CL_PinToggle_f) - resolve each back to its
		// CURRENT row through the generated map, so markers stay right through any catalogue
		// renumber. Legacy numeric slots (pre-cid) fall back to the old direct index.
		if ( !Cvar_VariableString( "coop_srCid0" )[0] ) {
			Cbuf_ExecuteText( EXEC_NOW, "exec ui/coop_sr_cids.cfg\n" );
		}
		// [user 2026-08-18] CATALOGUE GENERATION SELF-HEAL. The archived per-row state family
		// (coop_uiN progress, coop_uiD done) is keyed by ROW POSITION, so a catalogue renumber
		// leaves every row showing some OTHER challenge's numbers until the next join. The
		// generated map carries a hash of the cid list; on mismatch with the archived stamp,
		// wipe the positional state - the menu shows blank progress instead of WRONG progress,
		// and the next server join re-exports truth. Pins survive: they are cid-keyed.
		{
			const char *gen  = Cvar_VariableString( "coop_srGen" );
			cvar_t     *seen = Cvar_Get( "coop_srGenSeen", "", CVAR_ARCHIVE );
			if ( gen[0] && strcmp( gen, seen->string ) ) {
				int w;
				for ( w = 0; w < 1024; w++ ) {
					Cvar_Set( va( "coop_uiN%d", w ), "" );
					Cvar_Set( va( "coop_uiD%d", w ), "0" );
				}
				Cvar_Set( "coop_srGenSeen", gen );
				Com_Printf( "SR: catalogue changed (gen %s) - positional row state reset\n", gen );
			}
		}
		for ( k = 0; k < COOP_PIN_MAX; k++ ) {
			const char *v;
			Com_sprintf( slotName, sizeof( slotName ), "coop_pin%d", k + 1 );
			v = Cvar_VariableString( slotName );
			if ( v[0] ) {
				if ( strchr( v, '_' ) ) {
					int m;
					for ( m = 0; m < 1024; m++ ) {
						const char *mc = Cvar_VariableString( va( "coop_srCid%d", m ) );
						if ( !mc[0] ) {
							break;	// the map is dense - first gap is the end
						}
						if ( !strcmp( mc, v ) ) {
							pinIdx[pinCount++] = m;
							break;
						}
					}
				} else {
					pinIdx[pinCount++] = atoi( v );
				}
			}
		}
		for ( i = 0; i < 1024; i++ ) {
			Com_sprintf( markName, sizeof( markName ), "coop_uiP%d", i );
			Cvar_Get( markName, "0", CVAR_ARCHIVE );
			Cvar_Set( markName, "0" );
		}
		for ( k = 0; k < pinCount; k++ ) {
			if ( pinIdx[k] >= 0 && pinIdx[k] < 1024 ) {
				Com_sprintf( markName, sizeof( markName ), "coop_uiP%d", pinIdx[k] );
				Cvar_Set( markName, "1" );
			}
		}
		Cvar_Set( "coop_pinCount", va( "%d/%d", pinCount, COOP_PIN_MAX ) );
	}

	// [user 08-07] Medals lock overlays need BOTH "we are on the medals page" AND "this medal is
	// still locked", but enabledcvar only takes one cvar - so publish the AND of the two as
	// coop_uiMLv<i>. Without this the 12 scrims/padlocks drew over every challenge page.
	// Recomputed here because every tab button runs coop_srsync right after flipping the page
	// cvars, so this always reflects the page the user just switched to.
	// A medal with NO coop_uiML<i> yet (never pushed by medals.scr - e.g. browsing disconnected, or
	// before the first server flush) must default to LOCKED, not skipped: an unearned medal showing
	// as unlocked is the wrong way to fail. An earlier revision `break`-ed on the first missing
	// entry, which meant that when none had ever been pushed NO lock ever rendered at all.
	// The 64 cap is a runaway guard; medals.scr currently defines 12.
	onMedalsPage = !strcmp( Cvar_VariableString( "coop_srMedals" ), "1" ) ? qtrue : qfalse;
	for ( i = 0; i < 64; i++ ) {
		const char *lockedStr;
		Com_sprintf( nameBuf, sizeof( nameBuf ), "coop_uiML%d", i );
		lockedStr = Cvar_VariableString( nameBuf );
		Com_sprintf( doneBuf, sizeof( doneBuf ), "coop_uiMLv%d", i );
		Cvar_Get( doneBuf, "0", 0 );
		// missing/empty => treat as locked
		Cvar_Set( doneBuf,
			( onMedalsPage && ( !lockedStr[0] || !strcmp( lockedStr, "1" ) ) ) ? "1" : "0" );
	}
}

void CL_PinToggle_f( void )
{
	cvar_t     *slot[COOP_PIN_MAX];
	char        pinCid[COOP_PIN_MAX][64];
	char        cid[64];
	int         count;
	int         gi;
	int         i, j;
	qboolean    found;
	char        cvarName[16];

	if ( Cmd_Argc() < 2 ) {
		Com_Printf( "usage: coop_pintoggle <catalogue index>\n" );
		return;
	}
	gi = atoi( Cmd_Argv( 1 ) );
	if ( gi < 0 ) {
		return;
	}

	// [user 2026-08-18] pins persist as CIDS, not catalogue indices. The archived-index store
	// meant every catalogue renumber (a challenge added or removed) silently repointed every
	// player's pins at DIFFERENT challenges - the server half was designed around cids from
	// day one ("pins store cids, not indices", challenges.scr) and the client half betrayed it.
	// The index->cid map is generated with the SR pages (ui/coop_sr_cids.cfg) and lazy-loaded
	// here, so the disconnected main menu needs no server and no menu-flow hook.
	{
		const char *cidProbe = Cvar_VariableString( va( "coop_srCid%d", gi ) );
		if ( !cidProbe[0] ) {
			Cbuf_ExecuteText( EXEC_NOW, "exec ui/coop_sr_cids.cfg\n" );
		}
	}
	Q_strncpyz( cid, Cvar_VariableString( va( "coop_srCid%d", gi ) ), sizeof( cid ) );
	if ( !cid[0] ) {
		Com_Printf( "coop_pintoggle: row %d has no cid (ui/coop_sr_cids.cfg missing?)\n", gi );
		return;
	}

	// read the current 5 slots (skipping any empty ones) into a compact local array.
	// LEGACY MIGRATION: a pre-cid slot holds a bare number - resolve it through the same map
	// once and it is rewritten as a cid below; unresolvable legacy values are dropped.
	count = 0;
	for ( i = 0; i < COOP_PIN_MAX; i++ ) {
		Com_sprintf( cvarName, sizeof( cvarName ), "coop_pin%d", i + 1 );
		slot[i] = Cvar_Get( cvarName, "", CVAR_ARCHIVE | CVAR_USERINFO );
		if ( slot[i]->string[0] ) {
			if ( strchr( slot[i]->string, '_' ) ) {
				Q_strncpyz( pinCid[count], slot[i]->string, sizeof( pinCid[count] ) );
				count++;
			} else {
				const char *mig = Cvar_VariableString( va( "coop_srCid%s", slot[i]->string ) );
				if ( mig[0] ) {
					Q_strncpyz( pinCid[count], mig, sizeof( pinCid[count] ) );
					count++;
				}
			}
		}
	}

	// already pinned? unpin (remove + compact)
	found = qfalse;
	for ( i = 0; i < count; i++ ) {
		if ( !strcmp( pinCid[i], cid ) ) {
			found = qtrue;
			for ( j = i; j < count - 1; j++ ) {
				Q_strncpyz( pinCid[j], pinCid[j + 1], sizeof( pinCid[j] ) );
			}
			count--;
			break;
		}
	}

	if ( !found ) {
		// [user 08-07] failsafe - nothing left to track once a challenge is done. First draft read
		// only coop_uiD<gi>, a flag challenges.scr::chal_ui_writeOne pushes - but that push happens
		// on the 30s chal_flush cycle, so a player who had not yet been in-game 30s since this
		// shipped (or who is browsing the DISCONNECTED menu) saw no block at all. Derive it from
		// coop_uiN<gi> instead: that "N/T" progress string is already archived from previous
		// sessions for every existing player, so this works offline and needs no server push.
		// coop_uiD is still honoured as a fast path when present.
		if ( CL_ChallengeIsDone( gi ) ) {
			Com_Printf( "coop_pintoggle: challenge %d already complete - nothing to pin\n", gi );
			Cvar_Set( "coop_pinResult", "done" );
			return;
		}
		if ( count >= COOP_PIN_MAX ) {
			Com_Printf( "coop_pintoggle: %d challenges pinned already - unpin one first\n", COOP_PIN_MAX );
			Cvar_Set( "coop_pinResult", "full" );
			return;
		}
		Q_strncpyz( pinCid[count], cid, sizeof( pinCid[count] ) );
		count++;
	}

	// rewrite all 5 slots: the first `count` get the compacted indices, the rest are cleared
	for ( i = 0; i < COOP_PIN_MAX; i++ ) {
		if ( i < count ) {
			Cvar_Set( slot[i]->name, pinCid[i] );
		} else {
			Cvar_Set( slot[i]->name, "" );
		}
	}

	// instant local marker feedback - no server needed for this part
	Cvar_Set( va( "coop_uiP%d", gi ), found ? "0" : "1" );
	Cvar_Set( "coop_pinResult", found ? "unpinned" : "pinned" );
	// [user 08-07] single token - an unquoted stufftext value stops at the first space, and the
	// word PINNED is baked into the page art beside this (see gen_service_record.py).
	Cvar_Set( "coop_pinCount", va( "%d/%d", count, COOP_PIN_MAX ) );

	// connected right now? also notify the server immediately via the existing, proven name-bus
	// dispatch (player.scr:614, arrayIndex 47) so the live Mission Objectives HUD and self.flags
	// update in real time rather than waiting for the NEXT connect to reconcile via userinfo.
	if ( clc.state == CA_ACTIVE ) {
		cvar_t *nameCvar = Cvar_Get( "name", "UnnamedSoldier", CVAR_ARCHIVE | CVAR_USERINFO );
		char    nameToken[64];
		Com_sprintf( nameToken, sizeof( nameToken ), "%s ,cp%d", nameCvar->string, gi );
		Cvar_Set( "name", nameToken );
	}
}

/*
==================
CL_CoopDiscord_f

HZM coop [user 2026-09-25] "a discord icon that actually lets you click that will take you to join the discord
server". The invite is FIXED here on purpose - no argument, no cvar: a server can stufftext any console command
to a client, and a url parameter would let it open any page on a player's machine. Rate-limited so a stuffed
loop cannot spam browser tabs. The invite is the one README.md links.
==================
*/
static void CL_CoopDiscord_f( void )
{
    static int s_lastMs = -100000;
    const int  now      = Sys_Milliseconds();

    if ( now - s_lastMs < 3000 ) {
        return;
    }
    s_lastMs = now;
    if ( !Sys_OpenURL( "https://discord.gg/cVjMYPBFZ" ) ) {
        Com_Printf( "coop_discord: could not open a browser - the invite is https://discord.gg/cVjMYPBFZ\n" );
    }
}

// HZM coop [bugreport_parity bug-3278, security bug-3276] CL_SendReport_f moved to cl_bugreport.cpp (in-game report = desktop parity). The old body put the
// coop_reportWebhook cvar - which a server could rewrite - inside a PowerShell command line.

void CL_Init( void ) {
	int start, end;

	if( cl_bCLSystemStarted ) {
		return;
	}

	cl_bCLSystemStarted = qtrue;

	std::set_terminate( HZM_TerminateHandler );
	std::set_new_handler( HZM_NewHandler );

	Com_Printf( "----- Client Initialization -----\n" );

	start = Sys_Milliseconds();

	CL_ClearState ();
	S_StopAllSounds2( qtrue );

	clc.state = CA_DISCONNECTED;	// no longer CA_UNINITIALIZED
	Key_SetCatcher(KEYCATCH_UI);
	cls.realtime = 0;

	CL_InitInput ();

	if( !L_EventSystemStarted() ) {
		L_InitEvents();
	}

	//
	// register our variables
	//
	wombat = Cvar_Get( "wombat", "0", 0 );
	cl_noprint = Cvar_Get( "cl_noprint", "0", 0 );
	// maybe we can set up our own motd server once :)
	cl_motd = Cvar_Get ("cl_motd", "0", 0);

	cl_timeout = Cvar_Get ("cl_timeout", "200", 0);
	cl_connect_timeout = Cvar_Get( "cl_connect_timeout", "15", 0 );

	cl_master = Cvar_Get ("cl_master", MASTER_SERVER_NAME, CVAR_ARCHIVE);
	cl_timeNudge = Cvar_Get ("cl_timeNudge", "0", CVAR_TEMP );
	cl_shownet = Cvar_Get ("cl_shownet", "0", CVAR_TEMP );
	cl_netprofile = Cvar_Get("cl_netprofile", "0", CVAR_TEMP);
	cl_netprofileoverlay = Cvar_Get("cl_netprofileoverlay", "0", CVAR_TEMP);
	cl_showSend = Cvar_Get ("cl_showSend", "0", CVAR_TEMP );
	cl_showTimeDelta = Cvar_Get ("cl_showTimeDelta", "0", CVAR_TEMP );
	cl_freezeDemo = Cvar_Get ("cl_freezeDemo", "0", CVAR_TEMP );
	rcon_client_password = Cvar_Get ("rconPassword", "", CVAR_TEMP );
	cl_activeAction = Cvar_Get( "activeAction", "", CVAR_TEMP );
	// HZM coop [SEC2] CL_NextDemo executes this cvar's value. Registered (engine-owned) so a server can
	// neither create it through stufftext nor through systeminfo.
	Cvar_Get( "nextdemo", "", CVAR_TEMP );

	cl_timedemo = Cvar_Get ("timedemo", "0", 0);
	cl_timedemoLog = Cvar_Get ("cl_timedemoLog", "", CVAR_ARCHIVE);
	cl_autoRecordDemo = Cvar_Get ("cl_autoRecordDemo", "0", CVAR_ARCHIVE);
	cl_aviFrameRate = Cvar_Get ("cl_aviFrameRate", "25", CVAR_ARCHIVE);
	cl_aviMotionJpeg = Cvar_Get ("cl_aviMotionJpeg", "1", CVAR_ARCHIVE);
	cl_forceavidemo = Cvar_Get ("cl_forceavidemo", "0", 0);

	rconAddress = Cvar_Get ("rconAddress", "", 0);

	cl_yawspeed = Cvar_Get ("cl_yawspeed", "140", CVAR_ARCHIVE);
	cl_pitchspeed = Cvar_Get ("cl_pitchspeed", "140", CVAR_ARCHIVE);
	cl_anglespeedkey = Cvar_Get ("cl_anglespeedkey", "1.5", 0);

	cl_maxpackets = Cvar_Get ("cl_maxpackets", "100", CVAR_ARCHIVE );	// HZM coop [bug-3489] was 30: best connection for everyone
	cl_packetdup = Cvar_Get ("cl_packetdup", "1", CVAR_ARCHIVE );

	cl_run = Cvar_Get( "cl_run", "1", CVAR_ARCHIVE );
	cl_sensitivity = Cvar_Get( "sensitivity", "5", CVAR_ARCHIVE );
    cl_mouseAccel = Cvar_Get("cl_mouseAccel", "0", CVAR_ARCHIVE);
    cl_freelook = Cvar_Get("cl_freelook", "1", CVAR_ARCHIVE);
	// 0: legacy mouse acceleration
	// 1: new implementation
	cl_mouseAccelStyle = Cvar_Get( "cl_mouseAccelStyle", "0", CVAR_ARCHIVE );
	// offset for the power function (for style 1, ignored otherwise)
	// this should be set to the max rate value
	cl_mouseAccelOffset = Cvar_Get( "cl_mouseAccelOffset", "5", CVAR_ARCHIVE );
	Cvar_CheckRange(cl_mouseAccelOffset, 0.001f, 50000.0f, qfalse);

	cl_showMouseRate = Cvar_Get ("cl_showmouserate", "0", 0);

	cl_allowDownload = Cvar_Get ("cl_allowDownload", "0", CVAR_ARCHIVE);
#ifdef USE_CURL
	cl_cURLLib = Cvar_Get("cl_cURLLib", DEFAULT_CURL_LIB, CVAR_ARCHIVE);
#endif
	// HZM coop - in-game Report a Bug. This build has NO libcurl (find_package(CURL) failed -> HAS_LIBCURL
	// undefined, and the legacy USE_CURL path is off too), so the POST is done by spawning PowerShell
	// (CL_SendReport_f) instead of any in-engine HTTP. Registered unconditionally. coop_reportWebhook is
	// seeded on the user's machine (loose maintt/coop_reportwebhook.cfg from updater.ini, kept out of the repo);
	// the UI (coop_report menu) links coop_reportText and runs coop_sendreport.
	Cvar_Get("coop_reportWebhook", "", 0); // HZM coop [bugreport_parity bug-3278] old-exe seed only; the new report reads FILES
	Cvar_Get("coop_reportText", "", 0);
	Cvar_Get("coop_reportResult", "", 0);
	Cmd_AddCommand("coop_sendreport", CL_SendReport_f);
	CL_BugReportInit(); // HZM coop [bugreport_parity bug-3278] the report commands, cvars and preview
	CL_DedicatedInit(); // HZM coop [dedicated_launch] coop_dedStart / coop_dedStop
	Cmd_AddCommand("coop_discord", CL_CoopDiscord_f); // HZM coop - main-menu Discord button

	// HZM coop [user 08-06] bug-1503 - disconnected-capable Service Record challenge pinning.
	// Registered unconditionally (like coop_sendreport above) so it works from the cold main menu.
	// coop_pin1..coop_pin5 also get Cvar_Get'd lazily inside CL_PinToggle_f itself (first click on
	// any row registers whichever slots haven't been touched yet); registering all 5 here too just
	// makes sure they exist (and are in userinfo) even if the player never opens Service Record.
	{
		// [user 08-07] coop_pinCount - "X/5 Pinned" summary for the Service Record. Computed here too
		// (not just in CL_PinToggle_f) so a fresh menu open shows the right count from persisted pins
		// even before the player's first click this session.
		cvar_t *p1 = Cvar_Get("coop_pin1", "", CVAR_ARCHIVE | CVAR_USERINFO);
		cvar_t *p2 = Cvar_Get("coop_pin2", "", CVAR_ARCHIVE | CVAR_USERINFO);
		cvar_t *p3 = Cvar_Get("coop_pin3", "", CVAR_ARCHIVE | CVAR_USERINFO);
		cvar_t *p4 = Cvar_Get("coop_pin4", "", CVAR_ARCHIVE | CVAR_USERINFO);
		cvar_t *p5 = Cvar_Get("coop_pin5", "", CVAR_ARCHIVE | CVAR_USERINFO);
		int     initCount = 0;
		if (p1->string[0]) initCount++;
		if (p2->string[0]) initCount++;
		if (p3->string[0]) initCount++;
		if (p4->string[0]) initCount++;
		if (p5->string[0]) initCount++;
		// [user 08-07] value is ONE token ("3/5"): challenges.scr pushes this via stufftext, and an
		// unquoted stufftext value stops at the first space (quoting truncates on the wire instead -
		// bug-758), so the earlier "3/5 Pinned" silently became "3/5". The word PINNED is baked into
		// the page texture next to this Label instead.
		Cvar_Get("coop_pinCount", va("%d/%d", initCount, COOP_PIN_MAX), 0);
	}
	Cvar_Get("coop_pinResult", "", 0);

	// HZM-MP-BEGIN(mp_persist_userinfo) [2026-09-20 bug-2749]
	// MP progression / kit / cosmetic carry cvars MUST be registered here at CLIENT LAUNCH (CL_Init),
	// CVAR_ARCHIVE|CVAR_USERINFO - exactly like coop_pin1..5 above and dm_playermodel below. They were
	// previously registered only in cgame CG_Init (cg_main.c), which runs at CONNECT time - too late to be
	// userinfo-flagged when the client first builds its userinfo string, so the server's ensureLoaded read
	// an EMPTY coop_mpProgBlob on connect (MPPROG load VERIFIED=0 ever) and progression + armory weapon
	// memory + cosmetics all silently reset every session. Registering at launch means the archived value
	// is in userinfo from the very first connect and the server reads it back with info_valueforkey.
	{
		int i;
		static const char *const mpUserinfoCvars[] = {
			"coop_mpProgBlob",
			"coop_mpa_k1", "coop_mpa_k2", "coop_mpa_k3", "coop_mpa_k4", "coop_mpa_k5", "coop_mpa_k6",
			"coop_mpx_k1", "coop_mpx_k2", "coop_mpx_k3", "coop_mpx_k4", "coop_mpx_k5", "coop_mpx_k6",
			"coop_mpa_cosSkin", "coop_mpa_cosHelm", "coop_mpa_cosGlove",
			"coop_mpx_cosSkin", "coop_mpx_cosHelm", "coop_mpx_cosGlove"
		};
		for (i = 0; i < (int)(sizeof(mpUserinfoCvars) / sizeof(mpUserinfoCvars[0])); i++) {
			Cvar_Get(mpUserinfoCvars[i], "", CVAR_ARCHIVE | CVAR_USERINFO);
		}
	}
	// HZM-MP-END(mp_persist_userinfo)

	// HZM-MP-BEGIN(mp_preview_defaults) [2026-09-20 bug-2777]
	// The MP armory/appearance 3D mannequin reads coop_mp<side>_Char (body) + coop_mp<side>_Helm (head
	// attach). A cosmetic pick now setas these (persist), so on re-open the preview shows the SAVED skin/
	// helmet instead of always reverting to the default body (the "it looked reset" polish item). For a
	// player who has NEVER picked a cosmetic there is no archived value, so register a sensible DEFAULT
	// here at launch: Cvar_Get applies a default only to a cvar that does not yet exist (see the pin-shader
	// note below), so a genuine saved pick on disk is preserved and only a fresh profile gets the default.
	// CVAR_ARCHIVE, NOT userinfo - these are display-only preview paths; the server dresses the real spawn
	// from the carried cosSkin/cosHelm IDS, never from these model paths. Defaults MIRROR gen_mp_armory.py
	// (default_body / nohat_of / SKIN_STD) - keep in sync if the default bodies change (first-open preview
	// only; a stale value here can never affect what actually spawns).
	Cvar_Get("coop_mpa_Char", "models/player/american_ranger_nohat.tik", CVAR_ARCHIVE);
	Cvar_Get("coop_mpa_Helm", "models/coop_helmets/coop_std_us_helmet_private_net.tik", CVAR_ARCHIVE);
	Cvar_Get("coop_mpx_Char", "models/player/german_wehrmacht_soldier_nohat.tik", CVAR_ARCHIVE);
	Cvar_Get("coop_mpx_Helm", "models/coop_helmets/coop_helmet_ger_helmet.tik", CVAR_ARCHIVE);
	// HZM-MP-END(mp_preview_defaults)

	Cmd_AddCommand("coop_pintoggle", CL_PinToggle_f);
	Cmd_AddCommand("coop_srsync", CL_SyncSR_f);

	// [user 08-07] seed the per-row box shader paths. The Service Record box is drawn with
	// linkcvartoshader, so an UNSET cvar means no shader at all - the row rendered with no box
	// until something wrote one (the user had to press CLEAR PINS to make them appear). Cvar_Get
	// only applies a default to a cvar that does not yet exist, so genuine pinned/done state
	// already on disk is preserved.

	cl_altbindings = Cvar_Get( "cl_altbindings", "0", CVAR_ARCHIVE );
	cl_ctrlbindings = Cvar_Get( "cl_ctrlbindings", "0", CVAR_ARCHIVE );   // [fix] was registering "cl_altbindings" twice (copy-paste typo)

	cl_conXOffset = Cvar_Get ("cl_conXOffset", "0", 0);
#ifdef MACOS_X
        // In game video is REALLY slow in Mac OS X right now due to driver slowness
	cl_inGameVideo = Cvar_Get ("r_inGameVideo", "0", CVAR_ARCHIVE);
#else
	cl_inGameVideo = Cvar_Get ("r_inGameVideo", "1", CVAR_ARCHIVE);
#endif

	cl_serverStatusResendTime = Cvar_Get ("cl_serverStatusResendTime", "750", 0);

	cl_r_fullscreen = Cvar_Get( "r_fullscreen", "1", CVAR_ARCHIVE );

	m_pitch = Cvar_Get ("m_pitch", "0.022", CVAR_ARCHIVE);
	m_yaw = Cvar_Get ("m_yaw", "0.022", CVAR_ARCHIVE);
	m_forward = Cvar_Get ("m_forward", "0.25", CVAR_ARCHIVE);
	m_side = Cvar_Get ("m_side", "0.25", CVAR_ARCHIVE);
#ifdef MACOS_X
        // Input is jittery on OS X w/o this
	m_filter = Cvar_Get ("m_filter", "1", CVAR_ARCHIVE);
#else
	m_filter = Cvar_Get ("m_filter", "0", CVAR_ARCHIVE);
#endif

	j_pitch =        Cvar_Get ("j_pitch",        "0.022", CVAR_ARCHIVE);
	j_yaw =          Cvar_Get ("j_yaw",          "-0.022", CVAR_ARCHIVE);
	j_forward =      Cvar_Get ("j_forward",      "-0.25", CVAR_ARCHIVE);
	j_side =         Cvar_Get ("j_side",         "0.25", CVAR_ARCHIVE);
	j_up =           Cvar_Get ("j_up",           "0", CVAR_ARCHIVE);

	j_pitch_axis =   Cvar_Get ("j_pitch_axis",   "3", CVAR_ARCHIVE);
	j_yaw_axis =     Cvar_Get ("j_yaw_axis",     "2", CVAR_ARCHIVE);
	j_forward_axis = Cvar_Get ("j_forward_axis", "1", CVAR_ARCHIVE);
	j_side_axis =    Cvar_Get ("j_side_axis",    "0", CVAR_ARCHIVE);
	j_up_axis =      Cvar_Get ("j_up_axis",      "4", CVAR_ARCHIVE);

	Cvar_CheckRange(j_pitch_axis, 0, MAX_JOYSTICK_AXIS-1, qtrue);
	Cvar_CheckRange(j_yaw_axis, 0, MAX_JOYSTICK_AXIS-1, qtrue);
	Cvar_CheckRange(j_forward_axis, 0, MAX_JOYSTICK_AXIS-1, qtrue);
	Cvar_CheckRange(j_side_axis, 0, MAX_JOYSTICK_AXIS-1, qtrue);
	Cvar_CheckRange(j_up_axis, 0, MAX_JOYSTICK_AXIS-1, qtrue);

	// [controller feel] analog LOOK tuning (see CL_JoystickMove). Neutral defaults reproduce the old linear
	// feel; coop_defaults.cfg seeds nicer starting values that a Controls slider can then adjust.
	joy_response       = Cvar_Get( "joy_response",       "1.0", CVAR_ARCHIVE );
	joy_outerThreshold = Cvar_Get( "joy_outerThreshold", "0.0", CVAR_ARCHIVE );
	coop_padLookScale  = Cvar_Get( "coop_padLookScale",  "1.0", CVAR_ARCHIVE );

	cl_motdString = Cvar_Get( "cl_motdString", "", CVAR_ROM );

	cl_langamerefreshstatus = Cvar_Get( "cl_langamerefreshstatus", "Ready", 0 );
	cl_radar_icon_size = Cvar_Get("cl_radar_icon_size", "10", CVAR_ARCHIVE);
	cl_radar_speak_time = Cvar_Get("cl_radar_speak_time", "3", CVAR_ARCHIVE);
	cl_radar_blink_time = Cvar_Get("cl_radar_blink_time", "0.333", CVAR_ARCHIVE);
	cg_gametype = Cvar_Get( "cg_gametype", "0", 0 );

	cl_lanForcePackets = Cvar_Get ("cl_lanForcePackets", "1", CVAR_ARCHIVE);

	cl_guidServerUniq = Cvar_Get ("cl_guidServerUniq", "1", CVAR_ARCHIVE);

	// init autoswitch so the ui will have it correctly even
	// if the cgame hasn't been started
	Cvar_Get( "cg_autoswitch", "1", CVAR_ARCHIVE );

	Cvar_Get( "cl_maxPing", "800", CVAR_ARCHIVE );
	Cvar_Get( "cl_forceModel", "0", CVAR_ARCHIVE );

	if( m_pitch->value >= 0.0 ) {
		Cvar_Set( "m_invert_pitch", "0" );
	} else {
		Cvar_Set( "m_invert_pitch", "1" );
	}

	// ~ and `, as keys and characters
	cl_consoleKeys = Cvar_Get( "cl_consoleKeys", "~ ` 0x7e 0x60", CVAR_ARCHIVE );

	// userinfo
	name = Cvar_Get ("name", va("UnnamedSoldier#%d", rand() % 100000), CVAR_USERINFO | CVAR_ARCHIVE);
	cl_rate = Cvar_Get ("rate", "90000", CVAR_USERINFO | CVAR_ARCHIVE );	// HZM coop [bug-3489] was 25000 (and CL_ApplyOriginalConfigTweaks resets to these)
	Cvar_Get ("snaps", "40", CVAR_USERINFO | CVAR_ARCHIVE );	// HZM coop [bug-3489] was 20: the mod runs sv_fps 40
	Cvar_Get ("password", "", CVAR_USERINFO);
	Cvar_Get ("dm_playermodel", "american_army", CVAR_USERINFO | CVAR_ARCHIVE );
	Cvar_Get ("dm_playergermanmodel", "german_wehrmacht_soldier", CVAR_USERINFO | CVAR_ARCHIVE );
	// HZM coop - native FOV: the video-options slider links this cvar; the server reads the
	// "fov" userinfo key (G_ClientUserinfoChanged, clamps 80-160) and applies it via EV_Player_Fov
	// on connect AND on every userinfo change (so uniform swaps self-heal). ARCHIVE = persists.
	Cvar_Get ("fov", "80", CVAR_USERINFO | CVAR_ARCHIVE );
	// HZM coop - 3P free-cam toggle + shoulder side: registered HERE (exe startup) with ARCHIVE so
	// flipping them from the MAIN MENU persists. The cgame registers them too, but the cgame only
	// loads once in-game - a menu toggle before that created a flagless cvar that never archived.
	Cvar_Get ("cg_freecam", "0", CVAR_ARCHIVE );
	Cvar_Get ("cg_adsShoulderRight", "1", CVAR_ARCHIVE );
	// HZM coop [238] - NAT rendezvous cvars + the coop_join command
	RDV_Init();

	//
	// register our commands
	//
	Cmd_AddCommand ("cmd", CL_ForwardToServer_f);
	Cmd_AddCommand ("configstrings", CL_Configstrings_f);
	Cmd_AddCommand ("clientinfo", CL_Clientinfo_f);
	Cmd_AddCommand ("snd_restart", CL_Snd_Restart_f);
	Cmd_AddCommand ("vid_restart", CL_Vid_Restart_f);
	Cmd_AddCommand ("disconnect", CL_Disconnect_f);
	Cmd_AddCommand ("record", CL_Record_f);
	Cmd_AddCommand ("demo", CL_PlayDemo_f);
	Cmd_AddCommand ("cinematic", CL_PlayCinematic_f);
	Cmd_AddCommand ("stoprecord", CL_StopRecord_f);
	Cmd_AddCommand ("connect", CL_Connect_f);
	Cmd_AddCommand ("menuconnect", CL_MenuConnect_f);
	Cmd_AddCommand ("reconnect", CL_Reconnect_f);
	Cmd_AddCommand ("localservers", CL_LocalServers_f);
	Cmd_AddCommand ("globalservers", CL_GlobalServers_f);
	Cmd_AddCommand ("rcon", CL_Rcon_f);
	Cmd_AddCommand ("setenv", CL_Setenv_f );
	Cmd_AddCommand ("ping", CL_Ping_f );
	Cmd_AddCommand ("tikianimlist", TIKI_TikiAnimList_f );
	Cmd_AddCommand ("tikilist", TIKI_TikiList_f );
	Cmd_AddCommand ("animlist", TIKI_AnimList_f );
	Cmd_AddCommand ("tiki", CL_TikiInfoCommand_f );
	Cmd_AddCommand ("vidmode", CL_VidMode_f );
	Cmd_AddCommand ("saveshot", CL_SaveShot_f );
	Cmd_AddCommand ("dialog", CL_Dialog_f );
	Cmd_AddCommand ("aliasdump", Alias_Dump );
	Cmd_AddCommand ("fastconnect", CL_FastConnect_f );
	Cmd_AddCommand ("serverstatus", CL_ServerStatus_f );
	Cmd_AddCommand ("showip", CL_ShowIP_f );
	Cmd_AddCommand ("fs_openedList", CL_OpenedPK3List_f );
	Cmd_AddCommand ("fs_referencedList", CL_ReferencedPK3List_f );
	Cmd_AddCommand ("video", CL_Video_f );
	Cmd_AddCommand ("stopvideo", CL_StopVideo_f );
	CL_InitConsoleCommands();
	CL_InitRef();
	CL_StartHunkUsers(qfalse);

	SCR_Init ();

	Cbuf_Execute (0);

	Cvar_Set( "cl_running", "1" );

#if defined(NO_MODERN_DMA) && NO_MODERN_DMA
	S_Init2();
#else
	S_Init(qtrue);
#endif

	CL_GenerateQKey();
	Cvar_Get( "cl_guid", "", CVAR_USERINFO | CVAR_ROM );
	CL_UpdateGUID( NULL, 0 );

	CL_StartHunkUsers(qfalse);

	end = Sys_Milliseconds();

	if (com_gotOriginalConfig) {
		// Added in OPM
		//  Apply config tweaks after loading the original config
		CL_ApplyOriginalConfigTweaks();
	}

	Com_Printf( "----- Client Initialization Complete ----- %i ms\n", start - end );
}


/*
===============
CL_Shutdown

===============
*/
void CL_Shutdown(const char* finalmsg, qboolean disconnect, qboolean quit) {
	static qboolean recursive = qfalse;

	// check whether the client is running at all.
	if(!(com_cl_running && com_cl_running->integer))
		return;
	
	Com_Printf( "----- Client Shutdown (%s) -----\n", finalmsg );

	if ( recursive ) {
		printf ("recursive shutdown\n");
		return;
	}
    recursive = qtrue;

    noGameRestart = quit;

	if(disconnect)
		CL_Disconnect();

	if ( quit ) {
		CL_DedicatedShutdown(); // HZM coop [dedicated_launch] stop (or, Keep Running, leave) the server this game started
	}

#if defined(NO_MODERN_DMA) && NO_MODERN_DMA
	S_Shutdown();
#else
	S_Shutdown(qtrue);
#endif
	CL_ShutdownRef();

	CL_ShutdownUI();

	Cmd_RemoveCommand ("cmd");
	Cmd_RemoveCommand ("configstrings");
	Cmd_RemoveCommand ("userinfo");
	Cmd_RemoveCommand ("snd_restart");
	Cmd_RemoveCommand ("vid_restart");
	Cmd_RemoveCommand ("disconnect");
	Cmd_RemoveCommand ("record");
	Cmd_RemoveCommand ("demo");
	Cmd_RemoveCommand ("cinematic");
	Cmd_RemoveCommand ("stoprecord");
	Cmd_RemoveCommand ("connect");
	Cmd_RemoveCommand ("localservers");
	Cmd_RemoveCommand ("globalservers");
	Cmd_RemoveCommand ("refreshserverlist");
	Cmd_RemoveCommand ("rcon");
	Cmd_RemoveCommand ("setenv");
	Cmd_RemoveCommand ("ping");
	Cmd_RemoveCommand ("serverstatus");
	Cmd_RemoveCommand ("showip");
	Cmd_RemoveCommand ("model");
	Cmd_RemoveCommand ("video");
	Cmd_RemoveCommand ("stopvideo");

	CL_ShutdownInput();

	Cvar_Set( "cl_running", "0" );

	recursive = qfalse;

	Com_Printf( "-----------------------\n" );
	Z_FreeTags(TAG_CLIENT);
	cl_bCLSystemStarted = qfalse;

}

static void CL_SetServerInfo(serverInfo_t *server, const char *info, int ping) {
	if (server) {
		if (info) {
			server->clients = atoi(Info_ValueForKey(info, "clients"));
			Q_strncpyz(server->hostName,Info_ValueForKey(info, "hostname"), MAX_NAME_LENGTH);
			Q_strncpyz(server->mapName, Info_ValueForKey(info, "mapname"), MAX_NAME_LENGTH);
			server->maxClients = atoi(Info_ValueForKey(info, "sv_maxclients"));
			Q_strncpyz(server->game,Info_ValueForKey(info, "game"), MAX_NAME_LENGTH);
			server->gameType = atoi(Info_ValueForKey(info, "gametype"));
			Q_strncpyz(server->gameTypeString, Info_ValueForKey(info, "gametypestring"), sizeof(server->gameTypeString));
			server->netType = atoi(Info_ValueForKey(info, "nettype"));
			server->minPing = atoi(Info_ValueForKey(info, "minping"));
			server->maxPing = atoi(Info_ValueForKey(info, "maxping"));
		}
		server->ping = ping;
	}
}

static void CL_SetServerInfoByAddress(netadr_t from, const char *info, int ping) {
	int i;

	for (i = 0; i < MAX_OTHER_SERVERS; i++) {
		if (NET_CompareAdr(from, cls.localServers[i].adr)) {
			CL_SetServerInfo(&cls.localServers[i], info, ping);
		}
	}

	for (i = 0; i < MAX_OTHER_SERVERS; i++) {
		if (NET_CompareAdr(from, cls.mplayerServers[i].adr)) {
			CL_SetServerInfo(&cls.mplayerServers[i], info, ping);
		}
	}

	for (i = 0; i < MAX_GLOBAL_SERVERS; i++) {
		if (NET_CompareAdr(from, cls.globalServers[i].adr)) {
			CL_SetServerInfo(&cls.globalServers[i], info, ping);
		}
	}

	for (i = 0; i < MAX_OTHER_SERVERS; i++) {
		if (NET_CompareAdr(from, cls.favoriteServers[i].adr)) {
			CL_SetServerInfo(&cls.favoriteServers[i], info, ping);
		}
	}

}

/*
===================
CL_ServerInfoPacket
===================
*/
void CL_ServerInfoPacket( netadr_t from, msg_t *msg ) {
	int		i, type;
	char	info[MAX_INFO_STRING];
	char	*infoString;
	int		prot;
	char	*gamename;
	char	*pszVersion;
	char	*pszServerType;
	qboolean gameMismatch;

	infoString = MSG_ReadString( msg );

	// if this isn't the correct gamename, ignore it
	gamename = Info_ValueForKey( infoString, "gamename" );

#ifdef LEGACY_PROTOCOL
	// gamename is optional for legacy protocol
	if (com_legacyprotocol->integer && !*gamename)
		gameMismatch = qfalse;
	else
#endif
		gameMismatch = !*gamename || strcmp(gamename, com_gamename->string) != 0;

	if (gameMismatch)
	{
		Com_DPrintf( "Game mismatch in info packet: %s\n", infoString );
		return;
	}

	// if this isn't the correct protocol version, ignore it
	prot = atoi( Info_ValueForKey( infoString, "protocol" ) );

	if(prot != com_protocol->integer
#ifdef LEGACY_PROTOCOL
	   && prot != com_legacyprotocol->integer
#endif
		&& (com_target_demo->integer || prot != protocol_version_demo)
	  )
	{
		Com_DPrintf( "Different protocol info packet: %s\n", infoString );
		return;
	}

	if (!com_target_demo->integer && protocol_version_demo != protocol_version_full && prot == protocol_version_demo) {
		Com_DPrintf("Full version found compatible demo protocol version. %s\n", infoString);
	}

    pszVersion = Info_ValueForKey(infoString, "gamever");
    pszServerType = Info_ValueForKey(infoString, "serverType");

    if (*pszVersion) {
        if (*pszVersion == 'd') {
            pszVersion++;
        }

		if (com_target_game->integer >= target_game_e::TG_MOHTT) {
			if (atoi(pszServerType) == target_game_e::TG_MOHTT) {
				if (fabs(atof(pszVersion)) < 2.3f) {
					return;
				}
			} else {
				if (fabs(atof(pszVersion)) < 2.1f) {
					return;
				}
			}
		} else {
			if (fabs(atof(pszVersion) - com_target_shortversion->value) > 0.1f) {
				return;
			}
		}
	}

	// iterate servers waiting for ping response
	for (i=0; i<MAX_PINGREQUESTS; i++)
	{
		if ( cl_pinglist[i].adr.port && !cl_pinglist[i].time && NET_CompareAdr( from, cl_pinglist[i].adr ) )
		{
			// calc ping time
			cl_pinglist[i].time = Sys_Milliseconds() - cl_pinglist[i].start;
			Com_DPrintf( "ping time %dms from %s\n", cl_pinglist[i].time, NET_AdrToString( from ) );

			// save of info
			Q_strncpyz( cl_pinglist[i].info, infoString, sizeof( cl_pinglist[i].info ) );

			// tack on the net type
			// NOTE: make sure these types are in sync with the netnames strings in the UI
			switch (from.type)
			{
				case NA_BROADCAST:
				case NA_IP:
					type = 1;
					break;
				case NA_IP6:
					type = 2;
					break;
				default:
					type = 0;
					break;
			}
			Info_SetValueForKey( cl_pinglist[i].info, "nettype", va("%d", type) );
			CL_SetServerInfoByAddress(from, infoString, cl_pinglist[i].time);

			return;
		}
	}

	// if not just sent a local broadcast or pinging local servers
	if (cls.pingUpdateSource != AS_LOCAL) {
		return;
	}

	for ( i = 0 ; i < MAX_OTHER_SERVERS ; i++ ) {
		// empty slot
		if ( cls.localServers[i].adr.port == 0 ) {
			break;
		}

		// avoid duplicate
		if ( NET_CompareAdr( from, cls.localServers[i].adr ) ) {
			return;
		}
	}

	if ( i == MAX_OTHER_SERVERS ) {
		Com_DPrintf( "MAX_OTHER_SERVERS hit, dropping infoResponse\n" );
		return;
	}

	// add this to the list
	cls.numlocalservers = i+1;
	cls.bNewLocalServerInfo = true;
	CL_InitServerInfo( &cls.localServers[i], &from );
	CL_SetServerInfo( &cls.localServers[i], infoString, 0 );

	Q_strncpyz( info, MSG_ReadString( msg ), MAX_INFO_STRING );
	if (strlen(info)) {
		if (info[strlen(info)-1] != '\n') {
			Q_strcat(info, sizeof(info), "\n");
		}
		Com_Printf( "%s: %s", NET_AdrToStringwPort( from ), info );
	}
}

/*
===================
CL_GetServerStatus
===================
*/
serverStatus_t *CL_GetServerStatus( netadr_t from ) {
	int i, oldest, oldestTime;

	for (i = 0; i < MAX_SERVERSTATUSREQUESTS; i++) {
		if ( NET_CompareAdr( from, cl_serverStatusList[i].address ) ) {
			return &cl_serverStatusList[i];
		}
	}
	for (i = 0; i < MAX_SERVERSTATUSREQUESTS; i++) {
		if ( cl_serverStatusList[i].retrieved ) {
			return &cl_serverStatusList[i];
		}
	}
	oldest = -1;
	oldestTime = 0;
	for (i = 0; i < MAX_SERVERSTATUSREQUESTS; i++) {
		if (oldest == -1 || cl_serverStatusList[i].startTime < oldestTime) {
			oldest = i;
			oldestTime = cl_serverStatusList[i].startTime;
		}
	}
	return &cl_serverStatusList[oldest];
}

/*
===================
CL_ServerStatus
===================
*/
int CL_ServerStatus( const char *serverAddress, char *serverStatusString, int maxLen ) {
	int i;
	netadr_t	to;
	serverStatus_t *serverStatus;

	// if no server address then reset all server status requests
	if ( !serverAddress ) {
		for (i = 0; i < MAX_SERVERSTATUSREQUESTS; i++) {
			cl_serverStatusList[i].address.port = 0;
			cl_serverStatusList[i].retrieved = qtrue;
		}
		return qfalse;
	}
	// get the address
	if ( !NET_StringToAdr( serverAddress, &to, NA_UNSPEC) ) {
		return qfalse;
	}
	serverStatus = CL_GetServerStatus( to );
	// if no server status string then reset the server status request for this address
	if ( !serverStatusString ) {
		serverStatus->retrieved = qtrue;
		return qfalse;
	}

	// if this server status request has the same address
	if ( NET_CompareAdr( to, serverStatus->address) ) {
		// if we received a response for this server status request
		if (!serverStatus->pending) {
			Q_strncpyz(serverStatusString, serverStatus->string, maxLen);
			serverStatus->retrieved = qtrue;
			serverStatus->startTime = 0;
			return qtrue;
		}
		// resend the request regularly
		else if ( serverStatus->startTime < Com_Milliseconds() - cl_serverStatusResendTime->integer ) {
			serverStatus->print = qfalse;
			serverStatus->pending = qtrue;
			serverStatus->retrieved = qfalse;
			serverStatus->time = 0;
			serverStatus->startTime = Com_Milliseconds();
			CL_NET_OutOfBandPrint(to, "getstatus" );
			return qfalse;
		}
	}
	// if retrieved
	else if ( serverStatus->retrieved ) {
		serverStatus->address = to;
		serverStatus->print = qfalse;
		serverStatus->pending = qtrue;
		serverStatus->retrieved = qfalse;
		serverStatus->startTime = Com_Milliseconds();
		serverStatus->time = 0;
		CL_NET_OutOfBandPrint(to, "getstatus" );
		return qfalse;
	}
	return qfalse;
}

/*
===================
CL_ServerStatusResponse
===================
*/
void CL_ServerStatusResponse( netadr_t from, msg_t *msg ) {
	const char	*s;
	char		info[MAX_INFO_STRING];
	int			i, l, ping;
	int			len;
	serverStatus_t *serverStatus;

	serverStatus = NULL;
	for (i = 0; i < MAX_SERVERSTATUSREQUESTS; i++) {
		if ( NET_CompareAdr( from, cl_serverStatusList[i].address ) ) {
			serverStatus = &cl_serverStatusList[i];
			break;
		}
	}
	// if we didn't request this server status
	if (!serverStatus) {
		return;
	}

	s = MSG_ReadStringLine( msg );

	len = 0;
	Com_sprintf(&serverStatus->string[len], sizeof(serverStatus->string)-len, "%s", s);

	if (serverStatus->print) {
		Com_Printf("Server settings:\n");
		// print cvars
		while (*s) {
			for (i = 0; i < 2 && *s; i++) {
				if (*s == '\\')
					s++;
				l = 0;
				while (*s) {
					info[l++] = *s;
					if (l >= MAX_INFO_STRING-1)
						break;
					s++;
					if (*s == '\\') {
						break;
					}
				}
				info[l] = '\0';
				if (i) {
					Com_Printf("%s\n", info);
				}
				else {
					Com_Printf("%-24s", info);
				}
			}
		}
	}

	len = strlen(serverStatus->string);
	Com_sprintf(&serverStatus->string[len], sizeof(serverStatus->string)-len, "\\");

	if (serverStatus->print) {
		Com_Printf("\nPlayers:\n");
		Com_Printf("num: ping: name:\n");
	}
	for (i = 0, s = MSG_ReadStringLine( msg ); *s; s = MSG_ReadStringLine( msg ), i++) {

		len = strlen(serverStatus->string);
		Com_sprintf(&serverStatus->string[len], sizeof(serverStatus->string)-len, "\\%s", s);

		if (serverStatus->print) {
			ping = 0;
			sscanf(s, "%d", &ping);
			s = strchr(s, ' ');
			if (s)
				s++;
			else
				s = "unknown";
			Com_Printf("%-2d   %-3d   %s\n", i, ping, s );
		}
	}
	len = strlen(serverStatus->string);
	Com_sprintf(&serverStatus->string[len], sizeof(serverStatus->string)-len, "\\");

	serverStatus->time = Com_Milliseconds();
	serverStatus->address = from;
	serverStatus->pending = qfalse;
	if (serverStatus->print) {
		serverStatus->retrieved = qtrue;
	}
}

/*
==================
CL_LocalServers_f
==================
*/
void CL_LocalServers_f( void ) {
	const char		*message;
	int			i, j;
	netadr_t	to;

	Com_Printf( "Scanning for servers on the local network...\n");
	Cvar_Set( "cl_langamerefreshstatus", "Scanning for servers on the local network...\n" );

	// reset the list, waiting for response
	cls.numlocalservers = 0;
	cls.bNewLocalServerInfo = true;
	cls.pingUpdateSource = AS_LOCAL;

	for (i = 0; i < MAX_OTHER_SERVERS; i++) {
		qboolean b = cls.localServers[i].visible;
		Com_Memset(&cls.localServers[i], 0, sizeof(cls.localServers[i]));
		cls.localServers[i].visible = b;
	}
	Com_Memset( &to, 0, sizeof( to ) );

	// The 'xxx' in the message is a challenge that will be echoed back
	// by the server.  We don't care about that here, but master servers
	// can use that to prevent spoofed server responses from invalid ip
	message = "\377\377\377\377\x02getinfo xxx";

	// send each message twice in case one is dropped
	for ( i = 0 ; i < 2 ; i++ ) {
		// send a broadcast packet on each server port
		// we support multiple server ports so a single machine
		// can nicely run multiple servers
		for ( j = 0 ; j < NUM_SERVER_PORTS ; j++ ) {
			to.port = BigShort( (short)(PORT_SERVER + j) );

			to.type = NA_BROADCAST;
			NET_SendPacket( NS_CLIENT, strlen( message ), message, to );

			if (cl_netprofile->integer) {
				NetProfileAddPacket(&cls.netprofile.inPackets, strlen( message ), NETPROF_PACKET_MESSAGE);
			}
			
			// Added in OPM (from ioquake3)
			to.type = NA_MULTICAST6;
			NET_SendPacket( NS_CLIENT, strlen( message ), message, to );

			if (cl_netprofile->integer) {
				NetProfileAddPacket(&cls.netprofile.inPackets, strlen(message), NETPROF_PACKET_MESSAGE);
			}
		}
	}
}

/*
==================
CL_GlobalServers_f

Originally master 0 was Internet and master 1 was MPlayer.
ioquake3 2008; added support for requesting five separate master servers using 0-4.
ioquake3 2017; made master 0 fetch all master servers and 1-5 request a single master server.
==================
*/
void CL_GlobalServers_f( void ) {
	netadr_t	to;
	int			count, i, masterNum;
	char		command[1024], *masteraddress;
	
	if ((count = Cmd_Argc()) < 3 || (masterNum = atoi(Cmd_Argv(1))) < 0 || masterNum > MAX_MASTER_SERVERS)
	{
		Com_Printf("usage: globalservers <master# 0-%d> <protocol> [keywords]\n", MAX_MASTER_SERVERS);
		return;	
	}

	// request from all master servers
	if ( masterNum == 0 ) {
		int numAddress = 0;

		for ( i = 1; i <= MAX_MASTER_SERVERS; i++ ) {
			Com_sprintf(command, sizeof(command), "sv_master%d", i);
			masteraddress = Cvar_VariableString(command);

			if(!*masteraddress)
				continue;

			numAddress++;

			Com_sprintf(command, sizeof(command), "globalservers %d %s %s\n", i, Cmd_Argv(2), Cmd_ArgsFrom(3));
			Cbuf_AddText(command);
		}

		if ( !numAddress ) {
			Com_Printf( "CL_GlobalServers_f: Error: No master server addresses.\n");
		}
		return;
	}

	Com_sprintf(command, sizeof(command), "sv_master%d", masterNum);
	masteraddress = Cvar_VariableString(command);
	
	if(!*masteraddress)
	{
		Com_Printf( "CL_GlobalServers_f: Error: No master server address given.\n");
		return;	
	}

	// reset the list, waiting for response
	// -1 is used to distinguish a "no response"

	i = NET_StringToAdr(masteraddress, &to, NA_UNSPEC);
	
	if(!i)
	{
		Com_Printf( "CL_GlobalServers_f: Error: could not resolve address of master %s\n", masteraddress);
		return;	
	}
	else if(i == 2)
		to.port = BigShort(PORT_MASTER);

	Com_Printf("Requesting servers from %s (%s)...\n", masteraddress, NET_AdrToStringwPort(to));

	cls.numglobalservers = -1;
	cls.pingUpdateSource = AS_GLOBAL;

	// Use the extended query for IPv6 masters
	if (to.type == NA_IP6 || to.type == NA_MULTICAST6)
	{
		int v4enabled = Cvar_VariableIntegerValue("net_enabled") & NET_ENABLEV4;
		
		if(v4enabled)
		{
			Com_sprintf(command, sizeof(command), "getserversExt %s %s",
				com_gamename->string, Cmd_Argv(2));
		}
		else
		{
			Com_sprintf(command, sizeof(command), "getserversExt %s %s ipv6",
				com_gamename->string, Cmd_Argv(2));
		}
	}
	//else if ( !Q_stricmp( com_gamename->string, LEGACY_MASTER_GAMENAME ) )
	//	Com_sprintf(command, sizeof(command), "getservers %s",
	//		Cmd_Argv(2));
	else
		Com_sprintf(command, sizeof(command), "getservers %s %s",
			com_gamename->string, Cmd_Argv(2));

	for (i=3; i < count; i++)
	{
		Q_strcat(command, sizeof(command), " ");
		Q_strcat(command, sizeof(command), Cmd_Argv(i));
	}

	CL_NET_OutOfBandPrint( to, "%s", command );
}


/*
==================
CL_GetPing
==================
*/
void CL_GetPing( int n, char *buf, int buflen, int *pingtime )
{
	const char	*str;
	int		time;
	int		maxPing;

	if (n < 0 || n >= MAX_PINGREQUESTS || !cl_pinglist[n].adr.port)
	{
		// empty or invalid slot
		buf[0]    = '\0';
		*pingtime = 0;
		return;
	}

	str = NET_AdrToStringwPort( cl_pinglist[n].adr );
	Q_strncpyz( buf, str, buflen );

	time = cl_pinglist[n].time;
	if (!time)
	{
		// check for timeout
		time = Sys_Milliseconds() - cl_pinglist[n].start;
		maxPing = Cvar_VariableIntegerValue( "cl_maxPing" );
		if( maxPing < 100 ) {
			maxPing = 100;
		}
		if (time < maxPing)
		{
			// not timed out yet
			time = 0;
		}
	}

	CL_SetServerInfoByAddress(cl_pinglist[n].adr, cl_pinglist[n].info, cl_pinglist[n].time);

	*pingtime = time;
}

/*
==================
CL_UpdateServerInfo
==================
*/
void CL_UpdateServerInfo( int n )
{
	if (!cl_pinglist[n].adr.port)
	{
		return;
	}

	CL_SetServerInfoByAddress(cl_pinglist[n].adr, cl_pinglist[n].info, cl_pinglist[n].time );
}

/*
==================
CL_GetPingInfo
==================
*/
void CL_GetPingInfo( int n, char *buf, int buflen )
{
	if (n < 0 || n >= MAX_PINGREQUESTS || !cl_pinglist[n].adr.port)
	{
		// empty or invalid slot
		if (buflen)
			buf[0] = '\0';
		return;
	}

	Q_strncpyz( buf, cl_pinglist[n].info, buflen );
}

/*
==================
CL_ClearPing
==================
*/
void CL_ClearPing( int n )
{
	if (n < 0 || n >= MAX_PINGREQUESTS)
		return;

	cl_pinglist[n].adr.port = 0;
}

/*
==================
CL_GetPingQueueCount
==================
*/
int CL_GetPingQueueCount( void )
{
	int		i;
	int		count;
	ping_t*	pingptr;

	count   = 0;
	pingptr = cl_pinglist;

	for (i=0; i<MAX_PINGREQUESTS; i++, pingptr++ ) {
		if (pingptr->adr.port) {
			count++;
		}
	}

	return (count);
}

/*
==================
CL_GetFreePing
==================
*/
ping_t* CL_GetFreePing( void )
{
	ping_t*	pingptr;
	ping_t*	best;	
	int		oldest;
	int		i;
	int		time;

	pingptr = cl_pinglist;
	for (i=0; i<MAX_PINGREQUESTS; i++, pingptr++ )
	{
		// find free ping slot
		if (pingptr->adr.port)
		{
			if (!pingptr->time)
			{
				if (Sys_Milliseconds() - pingptr->start < 500)
				{
					// still waiting for response
					continue;
				}
			}
			else if (pingptr->time < 500)
			{
				// results have not been queried
				continue;
			}
		}

		// clear it
		pingptr->adr.port = 0;
		return (pingptr);
	}

	// use oldest entry
	pingptr = cl_pinglist;
	best    = cl_pinglist;
	oldest  = INT_MIN;
	for (i=0; i<MAX_PINGREQUESTS; i++, pingptr++ )
	{
		// scan for oldest
		time = Sys_Milliseconds() - pingptr->start;
		if (time > oldest)
		{
			oldest = time;
			best   = pingptr;
		}
	}

	return (best);
}

/*
==================
CL_Ping_f
==================
*/
void CL_Ping_f( void ) {
	netadr_t	to;
	ping_t*		pingptr;
	char*		server;
	int			argc;
	netadrtype_t	family = NA_UNSPEC;

	argc = Cmd_Argc();

	if ( argc != 2 && argc != 3 ) {
		Com_Printf( "usage: ping [-4|-6] server\n");
		return;	
	}
	
	if(argc == 2)
		server = Cmd_Argv(1);
	else
	{
		if(!strcmp(Cmd_Argv(1), "-4"))
			family = NA_IP;
		else if(!strcmp(Cmd_Argv(1), "-6"))
			family = NA_IP6;
		else
			Com_Printf( "warning: only -4 or -6 as address type understood.\n");
		
		server = Cmd_Argv(2);
	}

	Com_Memset( &to, 0, sizeof(netadr_t) );

	if ( !NET_StringToAdr( server, &to, family ) ) {
		return;
	}

	pingptr = CL_GetFreePing();

	memcpy( &pingptr->adr, &to, sizeof (netadr_t) );
	pingptr->start = Sys_Milliseconds();
	pingptr->time  = 0;

	CL_SetServerInfoByAddress(pingptr->adr, NULL, 0);
		
	CL_NET_OutOfBandPrint( to, "\x02getinfo xxx" );
}

/*
==================
CL_SaveShot_f
==================
*/
void CL_SaveShot_f( void ) {
	int i;
	char expanded[ 1024 ];

	if( !cls.rendererRegistered ) {
		return;
	}

	memcpy( expanded, "screenshot", 11 );

	for( i = 1; i < Cmd_Argc(); i++ )
	{
		strcat( expanded, " " );
		strcat( expanded, Cmd_Argv( i ) );
	}

	strcat( expanded, "\n" );

	// hide menus
	cls.no_menus = qtrue;
	UpdateStereoSide( STEREO_CENTER );
	Cbuf_ExecuteText( EXEC_NOW, expanded );
	cls.no_menus = qfalse;
}

/*
==================
CL_Dialog_f
==================
*/
void CL_Dialog_f( void ) {
	char		title[ 64 ];
	char		cvar[ 64 ];
	char		command[ 64 ];
	char		cancelCommand[ 64 ];
	char		shader[ 64 ];
	char		okshader[ 64 ];
	char		cancelshader[ 64 ];
	int			width, height;

	if( Cmd_Argc() <= 3 ) {
		Com_Printf( "usage: dialog title cvar_name command [width] [height]\n" );
		return;
	}

	Q_strncpyz( title, Cmd_Argv( 1 ), sizeof( title ) );
	Q_strncpyz( cvar, Cmd_Argv( 2 ), sizeof( cvar ) );
	Q_strncpyz( command, Cmd_Argv( 3 ), sizeof( command ) );
	Q_strncpyz( cancelCommand, Cmd_Argv( 4 ), sizeof( cancelCommand ) );

	if( Cmd_Argc() > 5 ) {
		width = atoi( Cmd_Argv( 5 ) );
	} else {
		width = 300;
	}
	if( Cmd_Argc() > 6 ) {
		height = atoi( Cmd_Argv( 6 ) );
	} else {
		height = 200;
	}

	if( Cmd_Argc() > 7 ) {
		Q_strncpyz( shader, Cmd_Argv( 7 ), sizeof( shader ) );
	} else {
		shader[ 0 ] = 0;
	}
	if( Cmd_Argc() > 8 ) {
		Q_strncpyz( okshader, Cmd_Argv( 8 ), sizeof(okshader) );
	} else {
		okshader[ 0 ] = 0;
	}
	if( Cmd_Argc() > 9 ) {
		Q_strncpyz( cancelshader, Cmd_Argv( 9 ), sizeof(cancelshader) );
	} else {
		cancelshader[ 0 ] = 0;
	}

	UI_CreateDialog( title, cvar, command, cancelCommand, width, height, shader, okshader, cancelshader );
}

/*
==================
CL_ServerRestarted
==================
*/
void CL_ServerRestarted( void ) {
	// HZM coop [bug-2573] `restart` keeps the gamestate and the cgame, so CL_ClearState never runs for it
	CL_HZM_ResetTransientAudio( "restart", clc.state );

	//S_StopAllSounds2( qfalse );
	// Fixed in OPM
	//  Also stop the music
	S_StopAllSounds2( qtrue );

	UI_ServerLoaded();
	UI_ClearState();

	VectorClear( cl.viewangles );
}

/*
==================
CL_UpdateVisiblePings_f
==================
*/
qboolean CL_UpdateVisiblePings_f(int source) {
	int			slots, i;
	char		buff[MAX_STRING_CHARS];
	int			pingTime;
	int			max;
	qboolean status = qfalse;

	if (source < 0 || source > AS_FAVORITES) {
		return qfalse;
	}

	cls.pingUpdateSource = source;

	slots = CL_GetPingQueueCount();
	if (slots < MAX_PINGREQUESTS) {
		serverInfo_t *server = NULL;

		switch (source) {
			case AS_LOCAL :
				server = &cls.localServers[0];
				max = cls.numlocalservers;
			break;
			case AS_GLOBAL :
				server = &cls.globalServers[0];
				max = cls.numglobalservers;
			break;
			case AS_FAVORITES :
				server = &cls.favoriteServers[0];
				max = cls.numfavoriteservers;
			break;
			default:
				return qfalse;
		}
		for (i = 0; i < max; i++) {
			if (server[i].visible) {
				if (server[i].ping == -1) {
					int j;

					if (slots >= MAX_PINGREQUESTS) {
						break;
					}
					for (j = 0; j < MAX_PINGREQUESTS; j++) {
						if (!cl_pinglist[j].adr.port) {
							continue;
						}
						if (NET_CompareAdr( cl_pinglist[j].adr, server[i].adr)) {
							// already on the list
							break;
						}
					}
					if (j >= MAX_PINGREQUESTS) {
						status = qtrue;
						for (j = 0; j < MAX_PINGREQUESTS; j++) {
							if (!cl_pinglist[j].adr.port) {
								break;
							}
						}
						memcpy(&cl_pinglist[j].adr, &server[i].adr, sizeof(netadr_t));
						cl_pinglist[j].start = Sys_Milliseconds();
						cl_pinglist[j].time = 0;
						CL_NET_OutOfBandPrint( cl_pinglist[j].adr, "getinfo xxx" );
						slots++;
					}
				}
				// if the server has a ping higher than cl_maxPing or
				// the ping packet got lost
				else if (server[i].ping == 0) {
					// if we are updating global servers
					if (source == AS_GLOBAL) {
						//
						if ( cls.numGlobalServerAddresses > 0 ) {
							// overwrite this server with one from the additional global servers
							cls.numGlobalServerAddresses--;
							CL_InitServerInfo(&server[i], &cls.globalServerAddresses[cls.numGlobalServerAddresses]);
							// NOTE: the server[i].visible flag stays untouched
						}
					}
				}
			}
		}
	} 

	if (slots) {
		status = qtrue;
	}
	for (i = 0; i < MAX_PINGREQUESTS; i++) {
		if (!cl_pinglist[i].adr.port) {
			continue;
		}
		CL_GetPing( i, buff, MAX_STRING_CHARS, &pingTime );
		if (pingTime != 0) {
			CL_ClearPing(i);
			status = qtrue;
		}
	}

	return status;
}

/*
==================
CL_ServerStatus_f
==================
*/
void CL_ServerStatus_f(void) {
	netadr_t	to, *toptr = NULL;
	char		*server;
	serverStatus_t *serverStatus;
	int			argc;
	netadrtype_t	family = NA_UNSPEC;

	argc = Cmd_Argc();

	if ( argc != 2 && argc != 3 )
	{
		if (clc.state != CA_ACTIVE || clc.demoplaying)
		{
			Com_Printf ("Not connected to a server.\n");
			Com_Printf( "usage: serverstatus [-4|-6] server\n");
			return;
		}

		toptr = &clc.serverAddress;
	}
	
	if(!toptr)
	{
		Com_Memset( &to, 0, sizeof(netadr_t) );
	
		if(argc == 2)
			server = Cmd_Argv(1);
		else
		{
			if(!strcmp(Cmd_Argv(1), "-4"))
				family = NA_IP;
			else if(!strcmp(Cmd_Argv(1), "-6"))
				family = NA_IP6;
			else
				Com_Printf( "warning: only -4 or -6 as address type understood.\n");
		
			server = Cmd_Argv(2);
		}

		toptr = &to;
		if ( !NET_StringToAdr( server, toptr, family ) )
			return;
	}

	CL_NET_OutOfBandPrint( *toptr, "getstatus" );

	serverStatus = CL_GetServerStatus( *toptr );
	serverStatus->address = *toptr;
	serverStatus->print = qtrue;
	serverStatus->pending = qtrue;
}

/*
==================
CL_ShowIP_f
==================
*/
void CL_ShowIP_f(void) {
	Sys_ShowIP();
}

/*
=================
bool CL_CDKeyValidate
=================
*/
qboolean CL_CDKeyValidate( const char *key, const char *checksum ) {
	char	ch;
	byte	sum;
	char	chs[3];
	int i;
	size_t len;

	len = strlen(key);
	if( len != CDKEY_LEN ) {
		return qfalse;
	}

	if( checksum && strlen( checksum ) != CDCHKSUM_LEN ) {
		return qfalse;
	}

	sum = 0;
	// for loop gets rid of conditional assignment warning
	for (i = 0; i < len; i++) {
		ch = *key++;
		if (ch>='a' && ch<='z') {
			ch -= 32;
		}
		switch( ch ) {
		case '2':
		case '3':
		case '7':
		case 'A':
		case 'B':
		case 'C':
		case 'D':
		case 'G':
		case 'H':
		case 'J':
		case 'L':
		case 'P':
		case 'R':
		case 'S':
		case 'T':
		case 'W':
			sum += ch;
			continue;
		default:
			return qfalse;
		}
	}

	Com_sprintf(chs, sizeof(chs), "%02x", sum);

	if (checksum && !Q_stricmp(chs, checksum)) {
		return qtrue;
	}

	if (!checksum) {
		return qtrue;
	}

	return qfalse;
}

void TIKI_CG_Command_ProcessFile(char* filename, qboolean quiet, dtiki_t* curTiki)
{
	if (cge) {
		cge->CG_Command_ProcessFile(filename, quiet, curTiki);
		return;
	}

	Com_Printf("NO CGE \n");
}

void CL_ApplyOriginalConfigTweaks()
{
	cvar_t* snaps = Cvar_Get("snaps", "", 0);

    // Those variables are not editable via UI so reset them
	// snaps/maxpackets can also have wrong values due to them being changed
	// via stufftext

	Cvar_Set("snaps", snaps->resetString);
	Cvar_Set("cl_maxpackets", cl_maxpackets->resetString);
}
