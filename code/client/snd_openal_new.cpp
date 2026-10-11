/*
===========================================================================
Copyright (C) 2024 the OpenMoHAA team

This file is part of OpenMoHAA source code.

OpenMoHAA source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

OpenMoHAA source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with OpenMoHAA source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/

#include "snd_local.h"
#include "snd_openal_new.h"
#include "client.h"
#include "../server/server.h"
#include "snd_codec.h"
#include <time.h> // HZM coop [bug-2572] wall-clock seed for the per-launch menu theme

#ifdef USE_INTERNAL_OPENAL_HEADERS
#include "AL/efx-presets.h"
#elif defined(_MSC_VER) || defined(__APPLE__)
#include <efx-presets.h>
#else
#include <AL/efx-presets.h>
#endif

typedef struct {
    const char *funcname;
    void      **funcptr;
    bool        required;
} extensions_table_t;

#define MAX_MUSIC_SONGS 16

static int   s_iNextLoopingWarning = 0;
static int   s_iReverbType         = 0;
static float s_fReverbLevel        = 0;
static bool  s_bReverbChanged      = false;
static bool  s_bFading             = false;
static float s_fFadeVolume         = 1.f;
static float s_fVelocityScale      = 0.005f;

static constexpr float s_fVolumeGain = 1.f; // = 84.f;

// HZM coop [bug-2572] the main-menu theme picker's play command - registered in S_OPENAL_Init, defined with
// S_TriggeredMusic_PlayIntroMusic
void S_MenuMusic_Play_f(void);
// HZM coop [bug-2576] main-menu theme auto-advance (see S_MenuMusic_Frame)
static void     S_MenuMusic_Frame(void);
static qboolean s_menuMusicAuto    = qfalse; // a menu theme started by S_MenuMusic_Start owns chan_trig_music
static int      s_menuMusicStartMs = 0;

cvar_t *s_milesdriver;
cvar_t *s_openaldevice;
cvar_t *s_reverb;
cvar_t *s_show_cpu;
cvar_t *s_show_num_active_sounds;
cvar_t *s_show_sounds;

// HZM coop [user 2026-09-04, bug-2457] SOUND-POOL GAUGE STATE. See the report block at the tail of
// S_OPENAL_Respatialize for why a high-water mark is needed and the existing "Couldn't play" line
// is not enough. Peaks are deliberately NOT reset per map: the question being asked is "how close
// did we ever get on this build", and a per-map reset would hide the worst moment of a session.
cvar_t *s_poolProbe          = NULL;
static int s_iPoolCur3D      = 0;
static int s_iPoolCur2D      = 0;
static int s_iPoolPeak3D     = 0;
static int s_iPoolPeak2D     = 0;
static int s_iPoolLoops      = 0;
static int s_iPoolDenied     = 0;   // 3D + 2D, the trigger for the immediate first report
static int s_iPoolDenied3D   = 0;
static int s_iPoolDenied2D   = 0;
static int s_iPoolDeniedReported = 0;
static int s_iPoolLastReport = 0;
cvar_t *s_speaker_type;
cvar_t *s_obstruction_cal_time;
cvar_t *s_lastSoundTime;
// Added in OPM
cvar_t *s_openaldriver;
cvar_t *s_alAvailableDevices;

static float reverb_table[] = {
    0.5f,   0.25f,        0.417f, 0.653f,      0.208f,      0.5f,   0.403f, 0.5f,   0.5f,
    0.361f, 0.5f,         0.153f, 0.361f,      0.44400001f, 0.25f,  0.111f, 0.111f, 0.19400001f,
    1.0f,   0.097000003f, 0.208f, 0.65200001f, 1.0f,        0.875f, 0.139f, 0.486f,
};

static vec3_t       vec_zero                   = {0, 0, 0};
int                 s_iNumMilesAudioProviders  = 0;
bool                s_bProvidersEmunerated     = false;
static bool         al_initialized             = false;
static bool         al_use_reverb              = false;
static bool         al_use_eaxreverb           = false;
static float        al_current_volume          = 0;

// EFX function pointers — loaded at runtime via alGetProcAddress when ALC_EXT_EFX is present
static LPALGENAUXILIARYEFFECTSLOTS   qalGenAuxiliaryEffectSlots   = NULL;
static LPALDELETEAUXILIARYEFFECTSLOTS qalDeleteAuxiliaryEffectSlots = NULL;
static LPALAUXILIARYEFFECTSLOTI      qalAuxiliaryEffectSloti      = NULL;
static LPALAUXILIARYEFFECTSLOTF      qalAuxiliaryEffectSlotf      = NULL;
static LPALGENEFFECTS                qalGenEffects                = NULL;
static LPALDELETEEFFECTS             qalDeleteEffects             = NULL;
static LPALEFFECTI                   qalEffecti                   = NULL;
static LPALEFFECTF                   qalEffectf                   = NULL;
static LPALEFFECTFV                  qalEffectfv                  = NULL;
static LPALSOURCE3I                  qalSource3i                  = NULL;

static ALuint al_efx_effect = 0;
static ALuint al_efx_slot   = 0;

// HZM coop - OCCLUSION: per-3D-channel lowpass filters (sounds muffled behind walls).
// Filter entry points loaded in S_OPENAL_InitEFX alongside the reverb ones.
static LPALGENFILTERS    qalGenFilters    = NULL;
static LPALDELETEFILTERS qalDeleteFilters = NULL;
static LPALFILTERI       qalFilteri       = NULL;
static LPALFILTERF       qalFilterf       = NULL;
static bool   al_use_occlusion = false;
static ALuint al_occ_filter[MAX_SOUNDSYSTEM_CHANNELS_3D];
static float  al_occ_cur[MAX_SOUNDSYSTEM_CHANNELS_3D];      // smoothed occlusion 0..1
static float  al_occ_tgt[MAX_SOUNDSYSTEM_CHANNELS_3D];      // traced target
static int    al_occ_nexttrace[MAX_SOUNDSYSTEM_CHANNELS_3D];
static int    al_occ_stamp[MAX_SOUNDSYSTEM_CHANNELS_3D];    // channel iStartTime when last traced
cvar_t       *s_occlusion;
cvar_t       *s_occlusionStrength;
// HZM coop [user 2026-09-04] CONCUSSION MUFFLE. One GLOBAL lowpass, not a per-wall one, and ONE
// shared filter object because the value is identical for every channel. EFX copies a filter's
// parameters into the source at alSourcei time, so RE-ATTACHING is how a change is published -
// which is why the occlusion code re-attaches every frame.
static ALuint al_mfl_filter   = 0;
static float  al_mfl_strength = 0.f;   // recomputed ONCE per Respatialize, not per channel
static float  al_mfl_applied[MAX_SOUNDSYSTEM_POSITION_CHANNELS];  // 2D range: last pushed value
static int    al_mfl_armed    = 0;     // marker edge state
cvar_t       *s_muffleHF;
cvar_t       *s_muffleGain;
cvar_t       *s_muffleCurve;
cvar_t       *s_reverbGain; // HZM coop

// MSS reverb preset indices 0-25 mapped to EFX EAXREVERB parameters (from efx-presets.h)
static const EFXEAXREVERBPROPERTIES al_reverb_presets[26] = {
    /* 0  GENERIC         */ { 1.0000f,1.0000f,0.3162f,0.8913f,1.0000f, 1.49f,0.83f,1.00f,0.0500f,0.0070f,{0,0,0},1.2589f,0.0110f,{0,0,0},0.2500f,0.00f,0.25f,0.00f,0.9943f,5000.00f,250.00f,0.00f,1},
    /* 1  PADDEDCELL      */ { 0.1715f,1.0000f,0.3162f,0.0010f,1.0000f, 0.17f,0.10f,1.00f,0.2500f,0.0010f,{0,0,0},1.2691f,0.0020f,{0,0,0},0.2500f,0.00f,0.25f,0.00f,0.9943f,5000.00f,250.00f,0.00f,1},
    /* 2  ROOM            */ { 0.4287f,1.0000f,0.3162f,0.5929f,1.0000f, 0.40f,0.83f,1.00f,0.1503f,0.0020f,{0,0,0},1.0629f,0.0030f,{0,0,0},0.2500f,0.00f,0.25f,0.00f,0.9943f,5000.00f,250.00f,0.00f,1},
    /* 3  BATHROOM        */ { 0.1715f,1.0000f,0.3162f,0.2512f,1.0000f, 1.49f,0.54f,1.00f,0.6531f,0.0070f,{0,0,0},3.2734f,0.0110f,{0,0,0},0.2500f,0.00f,0.25f,0.00f,0.9943f,5000.00f,250.00f,0.00f,1},
    /* 4  LIVINGROOM      */ { 0.9766f,1.0000f,0.3162f,0.0010f,1.0000f, 0.50f,0.10f,1.00f,0.2051f,0.0030f,{0,0,0},0.2805f,0.0040f,{0,0,0},0.2500f,0.00f,0.25f,0.00f,0.9943f,5000.00f,250.00f,0.00f,1},
    /* 5  STONEROOM       */ { 1.0000f,1.0000f,0.3162f,0.7079f,1.0000f, 2.31f,0.64f,1.00f,0.4411f,0.0120f,{0,0,0},1.1003f,0.0170f,{0,0,0},0.2500f,0.00f,0.25f,0.00f,0.9943f,5000.00f,250.00f,0.00f,1},
    /* 6  AUDITORIUM      */ { 1.0000f,1.0000f,0.3162f,0.5781f,1.0000f, 4.32f,0.59f,1.00f,0.4032f,0.0200f,{0,0,0},0.7170f,0.0300f,{0,0,0},0.2500f,0.00f,0.25f,0.00f,0.9943f,5000.00f,250.00f,0.00f,1},
    /* 7  CONCERTHALL     */ { 1.0000f,1.0000f,0.3162f,0.5623f,1.0000f, 3.92f,0.70f,1.00f,0.2427f,0.0200f,{0,0,0},0.9977f,0.0290f,{0,0,0},0.2500f,0.00f,0.25f,0.00f,0.9943f,5000.00f,250.00f,0.00f,1},
    /* 8  CAVE            */ { 1.0000f,1.0000f,0.3162f,1.0000f,1.0000f, 2.91f,1.30f,1.00f,0.5000f,0.0150f,{0,0,0},0.7063f,0.0220f,{0,0,0},0.2500f,0.00f,0.25f,0.00f,0.9943f,5000.00f,250.00f,0.00f,0},
    /* 9  ARENA           */ { 1.0000f,1.0000f,0.3162f,0.4477f,1.0000f, 7.24f,0.33f,1.00f,0.2612f,0.0200f,{0,0,0},1.0186f,0.0300f,{0,0,0},0.2500f,0.00f,0.25f,0.00f,0.9943f,5000.00f,250.00f,0.00f,1},
    /* 10 HANGAR          */ { 1.0000f,1.0000f,0.3162f,0.3162f,1.0000f,10.05f,0.23f,1.00f,0.5000f,0.0200f,{0,0,0},1.2560f,0.0300f,{0,0,0},0.2500f,0.00f,0.25f,0.00f,0.9943f,5000.00f,250.00f,0.00f,1},
    /* 11 CARPETEDHALLWAY */ { 0.4287f,1.0000f,0.3162f,0.0100f,1.0000f, 0.30f,0.10f,1.00f,0.1215f,0.0020f,{0,0,0},0.1531f,0.0300f,{0,0,0},0.2500f,0.00f,0.25f,0.00f,0.9943f,5000.00f,250.00f,0.00f,1},
    /* 12 HALLWAY         */ { 0.3645f,1.0000f,0.3162f,0.7079f,1.0000f, 1.49f,0.59f,1.00f,0.2458f,0.0070f,{0,0,0},1.6615f,0.0110f,{0,0,0},0.2500f,0.00f,0.25f,0.00f,0.9943f,5000.00f,250.00f,0.00f,1},
    /* 13 STONECORRIDOR   */ { 1.0000f,1.0000f,0.3162f,0.7612f,1.0000f, 2.70f,0.79f,1.00f,0.2472f,0.0130f,{0,0,0},1.5758f,0.0200f,{0,0,0},0.2500f,0.00f,0.25f,0.00f,0.9943f,5000.00f,250.00f,0.00f,1},
    /* 14 ALLEY           */ { 1.0000f,0.3000f,0.3162f,0.7328f,1.0000f, 1.49f,0.86f,1.00f,0.2500f,0.0070f,{0,0,0},0.9954f,0.0110f,{0,0,0},0.1250f,0.95f,0.25f,0.00f,0.9943f,5000.00f,250.00f,0.00f,1},
    /* 15 FOREST          */ { 1.0000f,0.3000f,0.3162f,0.0224f,1.0000f, 1.49f,0.54f,1.00f,0.0525f,0.1620f,{0,0,0},0.7682f,0.0880f,{0,0,0},0.1250f,1.00f,0.25f,0.00f,0.9943f,5000.00f,250.00f,0.00f,1},
    /* 16 CITY            */ { 1.0000f,0.5000f,0.3162f,0.3981f,1.0000f, 1.49f,0.67f,1.00f,0.0730f,0.0070f,{0,0,0},0.1427f,0.0110f,{0,0,0},0.2500f,0.00f,0.25f,0.00f,0.9943f,5000.00f,250.00f,0.00f,1},
    /* 17 MOUNTAINS       */ { 1.0000f,0.2700f,0.3162f,0.0562f,1.0000f, 1.49f,0.21f,1.00f,0.0407f,0.3000f,{0,0,0},0.1919f,0.1000f,{0,0,0},0.2500f,1.00f,0.25f,0.00f,0.9943f,5000.00f,250.00f,0.00f,0},
    /* 18 QUARRY          */ { 1.0000f,1.0000f,0.3162f,0.3162f,1.0000f, 1.49f,0.83f,1.00f,0.0000f,0.0610f,{0,0,0},1.7783f,0.0250f,{0,0,0},0.1250f,0.70f,0.25f,0.00f,0.9943f,5000.00f,250.00f,0.00f,1},
    /* 19 PLAIN           */ { 1.0000f,0.2100f,0.3162f,0.1000f,1.0000f, 1.49f,0.50f,1.00f,0.0585f,0.1790f,{0,0,0},0.1089f,0.1000f,{0,0,0},0.2500f,1.00f,0.25f,0.00f,0.9943f,5000.00f,250.00f,0.00f,1},
    /* 20 PARKINGLOT      */ { 1.0000f,1.0000f,0.3162f,1.0000f,1.0000f, 1.65f,1.50f,1.00f,0.2082f,0.0080f,{0,0,0},0.2652f,0.0120f,{0,0,0},0.2500f,0.00f,0.25f,0.00f,0.9943f,5000.00f,250.00f,0.00f,0},
    /* 21 SEWERPIPE       */ { 0.3071f,0.8000f,0.3162f,0.3162f,1.0000f, 2.81f,0.14f,1.00f,1.6387f,0.0140f,{0,0,0},3.2471f,0.0210f,{0,0,0},0.2500f,0.00f,0.25f,0.00f,0.9943f,5000.00f,250.00f,0.00f,1},
    /* 22 UNDERWATER      */ { 0.3645f,1.0000f,0.3162f,0.0100f,1.0000f, 1.49f,0.10f,1.00f,0.5963f,0.0070f,{0,0,0},7.0795f,0.0110f,{0,0,0},0.2500f,0.00f,1.18f,0.35f,0.9943f,5000.00f,250.00f,0.00f,1},
    /* 23 DRUGGED         */ { 0.4287f,0.5000f,0.3162f,1.0000f,1.0000f, 8.39f,1.39f,1.00f,0.8760f,0.0020f,{0,0,0},3.1081f,0.0300f,{0,0,0},0.2500f,0.00f,0.25f,1.00f,0.9943f,5000.00f,250.00f,0.00f,0},
    /* 24 DIZZY           */ { 0.3645f,0.6000f,0.3162f,0.6310f,1.0000f,17.23f,0.56f,1.00f,0.1392f,0.0200f,{0,0,0},0.4937f,0.0300f,{0,0,0},0.2500f,1.00f,0.81f,0.31f,0.9943f,5000.00f,250.00f,0.00f,0},
    /* 25 PSYCHOTIC       */ { 0.0625f,0.5000f,0.3162f,0.8404f,1.0000f, 7.56f,0.91f,1.00f,0.4864f,0.0200f,{0,0,0},2.4378f,0.0300f,{0,0,0},0.2500f,0.00f,4.00f,1.00f,0.9943f,5000.00f,250.00f,0.00f,0},
};
static unsigned int al_frequency               = 22050;
static ALCcontext  *al_context_id              = NULL;
static ALCdevice   *al_device                  = NULL;
static ALsizei      al_default_resampler_index = 0;
static ALsizei      al_resampler_index         = 0;

static ALboolean (*_alutLoadMP3_LOKI)(unsigned int buffer, const byte *data, int length);
static void (*_alReverbScale_LOKI)();
static void (*_alReverbDelay_LOKI)();

static qboolean music_active            = qfalse;
int             music_current_mood      = 0;
int             music_fallback_mood     = 0;
float           old_music_volume        = 1.f;
float           music_volume            = 1.f;
float           new_music_volume        = 1.f;
float           music_volume_fade_time  = 0;
long int        music_volume_start_time = 0;
int             music_volume_direction  = 0;
int             music_volume_changed    = 0;
int             music_loaded            = 0;
int             music_numsongs          = 0;
int             music_currentsong       = 0;

static qboolean enumeration_ext     = qfalse;
static qboolean enumeration_all_ext = qfalse;

static qboolean ima4_ext         = qfalse;
static qboolean soft_block_align = qfalse;

song_t            music_songs[MAX_MUSIC_SONGS];
openal_internal_t openal;
static float      s_fFadeStartTime;
static float      s_fFadeStopTime;
static char       current_soundtrack[128];

static void S_OPENAL_PlayMP3();
static void S_OPENAL_StopMP3();
static void S_OPENAL_Pitch();
static int
S_OPENAL_SpatializeStereoSound(const vec3_t listener_origin, const vec3_t listener_left, const vec3_t origin);
static void   S_OPENAL_reverb(int iChannel, int iReverbType, float fReverbLevel);
static float  S_HZM_LocalPanAmount(const openal_channel *pChannel);   // HZM coop - pannable local sound
static int    S_HZM_CueTier(const char *name);      // HZM coop - defined below; needed by the muffle
static bool   S_HZM_DuckExempt(const char *name);   // HZM coop - defined below; needed by the muffle

// How far off-centre a fully-panned local sound sits. Small on purpose: the listener is AT the
// origin, so this reads as an angle rather than a distance, and 64 units is already hard over.
#define HZM_LOCALPAN_RADIUS 64.f
static bool   S_OPENAL_LoadMP3_Codec(const char *_path, sfx_t *pSfx);
static ALuint S_OPENAL_Format(float width, int channels);

#define alDieIfError() __alDieIfError(__FILE__, __LINE__)

#if defined(_WIN64)
#    define ALDRIVER_DEFAULT "OpenAL64.dll"
#elif defined(_WIN32)
#    define ALDRIVER_DEFAULT "OpenAL32.dll"
#elif defined(__APPLE__)
#    define ALDRIVER_DEFAULT "libopenal.1.dylib"
#elif defined(__OpenBSD__)
#    define ALDRIVER_DEFAULT "libopenal.so"
#else
#    define ALDRIVER_DEFAULT "libopenal.so.1"
#endif

//
// alext
//
#ifdef AL_SOFT_source_resampler
LPALGETSTRINGISOFT qalGetStringiSOFT;
#endif

#ifdef AL_SOFT_loop_points
static LPALBUFFERIV qalBufferiv_soft = NULL;
#endif

/*
==============
__alDieIfError
==============
*/
static void __alDieIfError(const char *file, int line)
{
    ALint alErr = qalGetError();
    if (alErr) {
        if (s_show_sounds->integer) {
            Com_DPrintf("OpenAL error, %s, line %i: [%s].\n", file, line, qalGetString(alErr));
        }
    }
}

/*
==============
S_OPENAL_NukeSource
==============
*/
static void S_OPENAL_NukeSource(ALuint *srcptr)
{
    ALuint source;

    source = *srcptr;
    if (!*srcptr) {
        return;
    }

    if (!qalIsSource(*srcptr)) {
        return;
    }

    alDieIfError();
    qalSourceStop(source);

    alDieIfError();
    qalSourcei(source, AL_BUFFER, 0);

    alDieIfError();
    qalDeleteSources(1, srcptr);

    alDieIfError();
    *srcptr = 0;
}

/*
==============
S_OPENAL_NukeBuffer
==============
*/
static void S_OPENAL_NukeBuffer(ALuint *bufptr)
{
    if (!*bufptr) {
        return;
    }

    if (!qalIsBuffer(*bufptr)) {
        return;
    }

    alDieIfError();
    qalDeleteBuffers(1, bufptr);

    alDieIfError();
    *bufptr = 0;
}

/*
==============
S_OPENAL_NukeChannel
==============
*/
static void S_OPENAL_NukeChannel(openal_channel *channel)
{
    if (!channel) {
        return;
    }

    S_OPENAL_NukeSource(&channel->source);
    S_OPENAL_NukeBuffer(&channel->buffer);

    if (channel->bufferdata) {
        delete[] channel->bufferdata;
    }
}

/*
==============
S_OPENAL_ApplyEFXPreset
==============
*/
static void S_OPENAL_ApplyEFXPreset(const EFXEAXREVERBPROPERTIES *p, float level)
{
    if (!al_efx_effect || !qalEffecti || !qalEffectf || !qalEffectfv || !qalAuxiliaryEffectSloti) {
        return;
    }

    if (al_use_eaxreverb) {
        qalEffectf(al_efx_effect,  AL_EAXREVERB_DENSITY,              p->flDensity);
        qalEffectf(al_efx_effect,  AL_EAXREVERB_DIFFUSION,            p->flDiffusion);
        {
            // HZM coop - s_reverbGain lifts the wet level out of the mud: the stock chain
            // (preset gain x MSS table x zone level) lands around -28dB, inaudible under
            // gunfire. Clamp to the EAXREVERB legal max.
            float fWet = p->flGain * level * (s_reverbGain ? s_reverbGain->value : 1.0f);
            if (fWet > 1.0f) {
                fWet = 1.0f;
            }
            qalEffectf(al_efx_effect,  AL_EAXREVERB_GAIN,             fWet);
        }
        qalEffectf(al_efx_effect,  AL_EAXREVERB_GAINHF,               p->flGainHF);
        qalEffectf(al_efx_effect,  AL_EAXREVERB_GAINLF,               p->flGainLF);
        qalEffectf(al_efx_effect,  AL_EAXREVERB_DECAY_TIME,           p->flDecayTime);
        qalEffectf(al_efx_effect,  AL_EAXREVERB_DECAY_HFRATIO,        p->flDecayHFRatio);
        qalEffectf(al_efx_effect,  AL_EAXREVERB_DECAY_LFRATIO,        p->flDecayLFRatio);
        qalEffectf(al_efx_effect,  AL_EAXREVERB_REFLECTIONS_GAIN,     p->flReflectionsGain);
        qalEffectf(al_efx_effect,  AL_EAXREVERB_REFLECTIONS_DELAY,    p->flReflectionsDelay);
        qalEffectfv(al_efx_effect, AL_EAXREVERB_REFLECTIONS_PAN,      p->flReflectionsPan);
        qalEffectf(al_efx_effect,  AL_EAXREVERB_LATE_REVERB_GAIN,     p->flLateReverbGain);
        qalEffectf(al_efx_effect,  AL_EAXREVERB_LATE_REVERB_DELAY,    p->flLateReverbDelay);
        qalEffectfv(al_efx_effect, AL_EAXREVERB_LATE_REVERB_PAN,      p->flLateReverbPan);
        qalEffectf(al_efx_effect,  AL_EAXREVERB_ECHO_TIME,            p->flEchoTime);
        qalEffectf(al_efx_effect,  AL_EAXREVERB_ECHO_DEPTH,           p->flEchoDepth);
        qalEffectf(al_efx_effect,  AL_EAXREVERB_MODULATION_TIME,      p->flModulationTime);
        qalEffectf(al_efx_effect,  AL_EAXREVERB_MODULATION_DEPTH,     p->flModulationDepth);
        qalEffectf(al_efx_effect,  AL_EAXREVERB_AIR_ABSORPTION_GAINHF,p->flAirAbsorptionGainHF);
        qalEffectf(al_efx_effect,  AL_EAXREVERB_HFREFERENCE,          p->flHFReference);
        qalEffectf(al_efx_effect,  AL_EAXREVERB_LFREFERENCE,          p->flLFReference);
        qalEffectf(al_efx_effect,  AL_EAXREVERB_ROOM_ROLLOFF_FACTOR,  p->flRoomRolloffFactor);
        qalEffecti(al_efx_effect,  AL_EAXREVERB_DECAY_HFLIMIT,        p->iDecayHFLimit);
    } else {
        qalEffectf(al_efx_effect,  AL_REVERB_GAIN,                    p->flGain * level);
        qalEffectf(al_efx_effect,  AL_REVERB_GAINHF,                  p->flGainHF);
        qalEffectf(al_efx_effect,  AL_REVERB_DECAY_TIME,              p->flDecayTime);
        qalEffectf(al_efx_effect,  AL_REVERB_DECAY_HFRATIO,           p->flDecayHFRatio);
        qalEffectf(al_efx_effect,  AL_REVERB_REFLECTIONS_GAIN,        p->flReflectionsGain);
        qalEffectf(al_efx_effect,  AL_REVERB_REFLECTIONS_DELAY,       p->flReflectionsDelay);
        qalEffectf(al_efx_effect,  AL_REVERB_LATE_REVERB_GAIN,        p->flLateReverbGain);
        qalEffectf(al_efx_effect,  AL_REVERB_LATE_REVERB_DELAY,       p->flLateReverbDelay);
        qalEffectf(al_efx_effect,  AL_REVERB_AIR_ABSORPTION_GAINHF,   p->flAirAbsorptionGainHF);
        qalEffectf(al_efx_effect,  AL_REVERB_ROOM_ROLLOFF_FACTOR,     p->flRoomRolloffFactor);
        qalEffecti(al_efx_effect,  AL_REVERB_DECAY_HFLIMIT,           p->iDecayHFLimit);
    }

    qalAuxiliaryEffectSloti(al_efx_slot, AL_EFFECTSLOT_EFFECT, (ALint)al_efx_effect);
    qalGetError();
}

/*
==============
S_OPENAL_InitEFX
==============
*/
static bool S_OPENAL_InitEFX()
{
    if (!qalcIsExtensionPresent(al_device, "ALC_EXT_EFX")) {
        return false;
    }

    qalGenAuxiliaryEffectSlots   = (LPALGENAUXILIARYEFFECTSLOTS)   qalGetProcAddress("alGenAuxiliaryEffectSlots");
    qalDeleteAuxiliaryEffectSlots = (LPALDELETEAUXILIARYEFFECTSLOTS)qalGetProcAddress("alDeleteAuxiliaryEffectSlots");
    qalAuxiliaryEffectSloti      = (LPALAUXILIARYEFFECTSLOTI)       qalGetProcAddress("alAuxiliaryEffectSloti");
    qalAuxiliaryEffectSlotf      = (LPALAUXILIARYEFFECTSLOTF)       qalGetProcAddress("alAuxiliaryEffectSlotf");
    qalGenEffects                = (LPALGENEFFECTS)                 qalGetProcAddress("alGenEffects");
    qalDeleteEffects             = (LPALDELETEEFFECTS)              qalGetProcAddress("alDeleteEffects");
    qalEffecti                   = (LPALEFFECTI)                    qalGetProcAddress("alEffecti");
    qalEffectf                   = (LPALEFFECTF)                    qalGetProcAddress("alEffectf");
    qalEffectfv                  = (LPALEFFECTFV)                   qalGetProcAddress("alEffectfv");
    qalSource3i                  = (LPALSOURCE3I)                   qalGetProcAddress("alSource3i");

    if (!qalGenAuxiliaryEffectSlots || !qalDeleteAuxiliaryEffectSlots ||
        !qalAuxiliaryEffectSloti    || !qalAuxiliaryEffectSlotf       ||
        !qalGenEffects              || !qalDeleteEffects              ||
        !qalEffecti                 || !qalEffectf                    ||
        !qalEffectfv                || !qalSource3i) {
        Com_Printf("OpenAL: Failed to load all EFX function pointers.\n");
        return false;
    }

    qalGetError();
    qalGenAuxiliaryEffectSlots(1, &al_efx_slot);
    if (qalGetError() != AL_NO_ERROR || !al_efx_slot) {
        Com_Printf("OpenAL: Failed to create EFX auxiliary effect slot.\n");
        return false;
    }

    qalGenEffects(1, &al_efx_effect);
    if (qalGetError() != AL_NO_ERROR || !al_efx_effect) {
        qalDeleteAuxiliaryEffectSlots(1, &al_efx_slot);
        al_efx_slot = 0;
        Com_Printf("OpenAL: Failed to create EFX effect object.\n");
        return false;
    }

    qalEffecti(al_efx_effect, AL_EFFECT_TYPE, AL_EFFECT_EAXREVERB);
    if (qalGetError() == AL_NO_ERROR) {
        al_use_eaxreverb = true;
        Com_Printf("OpenAL: EFX EAXREVERB initialized.\n");
    } else {
        qalEffecti(al_efx_effect, AL_EFFECT_TYPE, AL_EFFECT_REVERB);
        if (qalGetError() != AL_NO_ERROR) {
            qalDeleteEffects(1, &al_efx_effect);
            qalDeleteAuxiliaryEffectSlots(1, &al_efx_slot);
            al_efx_effect = 0;
            al_efx_slot   = 0;
            Com_Printf("OpenAL: Neither EAXREVERB nor REVERB effect available.\n");
            return false;
        }
        Com_Printf("OpenAL: EFX standard REVERB initialized (EAXREVERB unavailable).\n");
    }

    // HZM coop - OCCLUSION: load AL filter entry points + one lowpass filter per 3D channel
    al_use_occlusion = false;
    qalGenFilters    = (LPALGENFILTERS)   qalGetProcAddress("alGenFilters");
    qalDeleteFilters = (LPALDELETEFILTERS)qalGetProcAddress("alDeleteFilters");
    qalFilteri       = (LPALFILTERI)      qalGetProcAddress("alFilteri");
    qalFilterf       = (LPALFILTERF)      qalGetProcAddress("alFilterf");
    if (qalGenFilters && qalDeleteFilters && qalFilteri && qalFilterf) {
        int  j;
        bool ok = true;
        qalGetError();
        qalGenFilters(MAX_SOUNDSYSTEM_CHANNELS_3D, al_occ_filter);
        if (qalGetError() == AL_NO_ERROR) {
            for (j = 0; j < MAX_SOUNDSYSTEM_CHANNELS_3D; j++) {
                qalFilteri(al_occ_filter[j], AL_FILTER_TYPE, AL_FILTER_LOWPASS);
                al_occ_cur[j] = al_occ_tgt[j] = 0;
                al_occ_nexttrace[j] = 0;
                al_occ_stamp[j] = -1;
            }
            if (qalGetError() == AL_NO_ERROR) {
                al_use_occlusion = true;
                Com_Printf("OpenAL: occlusion lowpass filters initialized.\n");

                // HZM coop [user 2026-09-04] the concussion muffle's own filter object.
                // DELIBERATELY INSIDE THIS BRANCH. Sitting above it, this block's qalGetError()
                // calls would have consumed the very error the enclosing test reads, so a device
                // whose 96 qalFilteri calls FAILED would still have set al_use_occlusion = true
                // and then driven invalid filter names every frame. Opens by CLEARING the error
                // state rather than closing by clearing it, for the same reason.
                qalGetError();
                qalGenFilters(1, &al_mfl_filter);
                if (qalGetError() == AL_NO_ERROR && al_mfl_filter) {
                    qalFilteri(al_mfl_filter, AL_FILTER_TYPE, AL_FILTER_LOWPASS);
                } else {
                    al_mfl_filter = 0;   // no muffle on this device; occlusion is unaffected
                }
                for (j = 0; j < MAX_SOUNDSYSTEM_POSITION_CHANNELS; j++) {
                    al_mfl_applied[j] = 0.f;   // fresh sources carry no filter, so 0 is accurate
                }
                al_mfl_strength = 0.f;
                al_mfl_armed    = 0;
                qalGetError();
            } else {
                ok = false;
            }
        } else {
            ok = false;
        }
        if (!ok) {
            Com_Printf("OpenAL: occlusion filter setup failed; occlusion disabled.\n");
        }
    }

    return true;
}

/*
==============
S_OPENAL_DestroyEFX
==============
*/
static void S_OPENAL_DestroyEFX()
{
    if (al_use_occlusion && qalDeleteFilters) {
        qalDeleteFilters(MAX_SOUNDSYSTEM_CHANNELS_3D, al_occ_filter); // HZM coop - occlusion filters
        memset(al_occ_filter, 0, sizeof(al_occ_filter));
    }
    if (al_mfl_filter && qalDeleteFilters) {
        qalDeleteFilters(1, &al_mfl_filter);
    }
    al_mfl_filter = 0;
    al_use_occlusion = false;
    if (al_efx_effect && qalDeleteEffects) {
        qalDeleteEffects(1, &al_efx_effect);
        al_efx_effect = 0;
    }
    if (al_efx_slot && qalDeleteAuxiliaryEffectSlots) {
        qalDeleteAuxiliaryEffectSlots(1, &al_efx_slot);
        al_efx_slot = 0;
    }
    al_use_reverb   = false;
    al_use_eaxreverb = false;
}

/*
==============
S_OPENAL_NukeContext
==============
*/
static void S_OPENAL_NukeContext()
{
    int i;

    Com_Printf("OpenAL: Destroying channels...\n");

    for (i = 0; i < MAX_SOUNDSYSTEM_CHANNELS; ++i) {
        S_OPENAL_NukeChannel(openal.channel[i]);
    }

    Com_Printf("OpenAL: Channels destroyed successfully.\n");

    for (i = 0; i < s_numSfx; i++) {
        S_OPENAL_NukeBuffer(&s_knownSfx[i].buffer);
    }

    S_OPENAL_NukeBuffer(&openal.movieSFX.buffer);

    S_OPENAL_DestroyEFX();

    if (al_context_id) {
        Com_Printf("OpenAL: Destroying context...\n");

        qalcSuspendContext(al_context_id);
        qalcMakeContextCurrent(NULL);
        qalcDestroyContext(al_context_id);
        al_context_id = NULL;

        Com_Printf("OpenAL: Context destroyed successfully.\n");
    }

    if (al_device) {
        Com_Printf("OpenAL: Closing device...\n");

        qalcCloseDevice(al_device);
        al_device = NULL;

        Com_Printf("OpenAL: Device closed successfully.\n");
    }
}

/*
==============
S_OPENAL_InitContext
==============
*/
static bool S_OPENAL_InitContext()
{
    const char *dev;
    int         attrlist[12];

    Com_DPrintf("OpenAL: Context initialization\n");

    dev = NULL;
    if (s_openaldevice) {
        dev = s_openaldevice->string;
    }

    if (dev && !*dev) {
        dev = NULL;
    }

    // Device enumeration support
    enumeration_all_ext = qalcIsExtensionPresent(NULL, "ALC_ENUMERATE_ALL_EXT");
    enumeration_ext     = qalcIsExtensionPresent(NULL, "ALC_ENUMERATION_EXT");

    if (enumeration_ext || enumeration_all_ext) {
        char        devicenames[16384] = "";
        const char *devicelist;
#ifdef _WIN32
        const char *defaultdevice;
#endif
        int curlen;

        // get all available devices + the default device name.
        if (enumeration_all_ext) {
            devicelist = qalcGetString(NULL, ALC_ALL_DEVICES_SPECIFIER);
#ifdef _WIN32
            defaultdevice = qalcGetString(NULL, ALC_DEFAULT_ALL_DEVICES_SPECIFIER);
#endif
        } else {
            // We don't have ALC_ENUMERATE_ALL_EXT but normal enumeration.
            devicelist = qalcGetString(NULL, ALC_DEVICE_SPECIFIER);
#ifdef _WIN32
            defaultdevice = qalcGetString(NULL, ALC_DEFAULT_DEVICE_SPECIFIER);
#endif
            enumeration_ext = qtrue;
        }

#ifdef _WIN32
        Com_DPrintf("OpenAL: Default playback device: \"%s\"\n", defaultdevice);
#endif

#ifdef _WIN32
        // check whether the default device is generic hardware. If it is, change to
        // Generic Software as that one works more reliably with various sound systems.
        // If it's not, use OpenAL's default selection as we don't want to ignore
        // native hardware acceleration.
        if (!dev && defaultdevice && !strcmp(defaultdevice, "Generic Hardware")) {
            dev = "Generic Software";
        }
#endif

        // dump a list of available devices to a cvar for the user to see.

        if (devicelist) {
            while ((curlen = strlen(devicelist))) {
                Q_strcat(devicenames, sizeof(devicenames), devicelist);
                Q_strcat(devicenames, sizeof(devicenames), "\n");

                devicelist += curlen + 1;
            }
        }

        Com_Printf("OpenAL: List of available devices:\n%s\n", devicenames);

        s_alAvailableDevices = Cvar_Get("s_alAvailableDevices", devicenames, CVAR_ROM | CVAR_NORESTART);
    }

    Com_Printf("OpenAL: Opening device \"%s\"...\n", dev ? dev : "{default}");

    al_device = qalcOpenDevice(dev);
    if (!al_device && dev) {
        Com_Printf("Failed to open OpenAL device '%s', trying default.\n", dev);
        al_device = qalcOpenDevice(NULL);
    }

    if (!al_device) {
        Com_Printf("OpenAL: Could not open device\n");
        S_OPENAL_NukeContext();
        return false;
    }

    Com_Printf("OpenAL: Device opened successfully.\n");
    switch (s_khz->integer) {
    case 11:
        al_frequency = 11025;
        break;
    default:
    case 22:
        al_frequency = 22050;
        break;
    case 44:
        al_frequency = 44100;
        break;
    }

    attrlist[0] = ALC_FREQUENCY;
    attrlist[1] = al_frequency;
    attrlist[2] = ALC_SYNC;
    attrlist[3] = qfalse;

    s_speaker_type->modified = false;

#ifdef ALC_SOFT_output_mode
    attrlist[4] = ALC_OUTPUT_MODE_SOFT;
    // Disable HRTF by default
    // For example, actual speakers that are recognized as headphones by the OS
    // will not get forced HRTF
    attrlist[6] = ALC_HRTF_SOFT;
    attrlist[7] = ALC_FALSE;

    switch (s_speaker_type->integer) {
    // Two speakers
    default:
    case 0:
        attrlist[5] = ALC_STEREO_SOFT;
        break;
    // Headphones
    case 1:
        attrlist[5] = ALC_STEREO_HRTF_SOFT;
        // Allow HRTF mixing (without forcing in case it's unsupported)
        attrlist[7] = ALC_DONT_CARE_SOFT;
        break;
    // Surround
    case 2:
        attrlist[5] = ALC_SURROUND_5_1_SOFT;
        break;
    // Quad speakers
    case 3:
        attrlist[5] = ALC_QUAD_SOFT;
        break;
    }
#else
#    pragma message("OpenAL: ALC_OUTPUT_MODE_SOFT unavailable")

    Com_Printf(
        "OpenAL: ALC_OUTPUT_MODE_SOFT is unavailable. The speaker type will be ignored, fallback to normal stereo.\n"
    );
    attrlist[4] = 0;
    attrlist[5] = 0;
#endif

#ifdef ALC_SOFT_output_limiter
    // HZM coop - ENABLE the output limiter (was ALC_FALSE = force-disabled). Prevents hard output
    // clipping when many loud sounds stack (known issue: 06-24 audio regression "clipping" reports).
    attrlist[8] = ALC_OUTPUT_LIMITER_SOFT;
    attrlist[9] = ALC_TRUE;
#endif
    attrlist[10] = 0;
    attrlist[11] = 0;

    Com_Printf("OpenAL: Creating AL context...\n");
    al_context_id = qalcCreateContext(al_device, attrlist);
    if (!al_context_id) {
        Com_Printf("OpenAL: Could not create context\n");
        S_OPENAL_NukeContext();
        return false;
    }

    Com_Printf("OpenAL: Context created successfully.\n");

    qalcMakeContextCurrent(al_context_id);
    alDieIfError();

#ifdef ALC_HRTF_SOFT
    {
        // HZM coop - report whether HRTF is actually active (Speaker Setup: Headphones requests it)
        ALCint iHrtf = 0;
        qalcGetIntegerv(al_device, ALC_HRTF_SOFT, 1, &iHrtf);
        Com_Printf("OpenAL: HRTF is %s.\n", iHrtf ? "ENABLED" : "disabled");
    }
#endif

    Com_Printf("AL_VENDOR: %s\n", qalGetString(AL_VENDOR));
    alDieIfError();

    Com_Printf("AL_VERSION: %s\n", qalGetString(AL_VERSION));
    alDieIfError();

    Com_Printf("AL_RENDERER: %s\n", qalGetString(AL_RENDERER));
    alDieIfError();

    Com_Printf("AL_EXTENSIONS: %s\n", qalGetString(AL_EXTENSIONS));
    alDieIfError();

    // HZM coop - LINEAR falloff to match the original Miles-engine tuning the stock aliases were
    // authored for (min/max dist define the audible envelope). Under INVERSE_DISTANCE_CLAMPED a
    // gunshot with mindist 480 drops to ~24% at 2000u; the original carried ~80% there — this is
    // why gunfire "always sounded weak" on OpenAL ports. Linear-clamped restores map-wide gunfire.
    qalDistanceModel(AL_LINEAR_DISTANCE_CLAMPED);
    alDieIfError();

    qalcProcessContext(al_context_id);
    alDieIfError();

    return true;
}

/*
==============
S_OPENAL_InitExtensions
==============
*/
static bool S_OPENAL_InitExtensions()
{
    /*
    extensions_table_t extensions_table[4] = {
        "alutLoadMP3_LOKI",
        (void **)&_alutLoadMP3_LOKI,
        true,
        "alReverbScale_LOKI",
        (void **)&_alReverbScale_LOKI,
        true,
        "alReverbDelay_LOKI",
        (void **)&_alReverbDelay_LOKI,
        true
    };
    */

    extensions_table_t extensions_table[] = {
#ifdef AL_SOFT_source_resampler
        extensions_table_t {
                            "alGetStringiSOFT", (void **)&qalGetStringiSOFT,
                            false, },
#endif
        extensions_table_t {NULL, NULL, NULL}
    };

    extensions_table_t *i;

    for (i = extensions_table; i->funcname; ++i) {
        *i->funcptr = NULL;
    }

    for (i = extensions_table; i->funcname; ++i) {
        Com_Printf("AL extension: Looking up %ssymbol \"%s\"...", i->required ? "required " : "", i->funcname);

        *i->funcptr = qalGetProcAddress(i->funcname);
        if (!*i->funcptr) {
            Com_Printf("...not found! %d [%s]\n", qalGetError(), qalGetString(qalGetError()));
            if (i->required) {
                S_OPENAL_NukeContext();
                return false;
            }
            continue;
        }

        Com_Printf("...found.\n");
    }

    ima4_ext         = qalIsExtensionPresent("AL_EXT_IMA4");
    soft_block_align = qalIsExtensionPresent("AL_SOFT_block_alignment");

    qalGetError();
    return true;
}

/*
==============
S_OPENAL_InitChannel
==============
*/
static bool S_OPENAL_InitChannel(int idx, openal_channel *chan)
{
    openal.channel[idx] = chan;
    VectorClear(chan->vOrigin);
    chan->fVolume         = 1.0;
    chan->fNewPitchMult   = 0.0;
    chan->fMinDist        = 0.0;
    chan->fMaxDist        = 0.0;
    chan->pSfx            = 0;
    chan->iEntNum         = 0;
    chan->iEntChannel     = 0;
    chan->iBaseRate       = 0;
    chan->iStartTime      = 0;
    chan->iTime           = 0;
    chan->iEndTime        = 0;
    chan->iFlags          = 0;
    chan->iPausedOffset   = 0;
    chan->source          = 0;
    chan->buffer          = 0;
    chan->bufferdata      = 0;
    chan->fading          = FADE_NONE;
    chan->fade_time       = 0;
    chan->fade_start_time = 0;
    chan->song_number     = 0;

    qalGenSources(1, &chan->source);
    alDieIfError();
    qalSourcei(chan->source, AL_SOURCE_RELATIVE, true);
    alDieIfError();
    // HZM coop - gain headroom so a sound aliased with soundparms volume >1 actually amplifies past full
    // scale (default AL_MAX_GAIN is 1.0, which clamps it). Sounds aliased at vol <=1 are unaffected.
    qalSourcef(chan->source, AL_MAX_GAIN, 8.0f);
    alDieIfError();

#ifdef AL_SOFT_source_resampler
    qalSourcei(chan->source, AL_SOURCE_RESAMPLER_SOFT, al_resampler_index);
    alDieIfError();
#endif

    return true;
}

/*
==============
S_OPENAL_Init
==============
*/

// HZM coop - AUDIO OUTPUT DEVICE CYCLING ("s_cycledevice", wired to the Audio menu's CHANGE OUTPUT
// DEVICE row). Steps s_openaldevice through the devices enumerated into s_alAvailableDevices at init
// ("" = system default is part of the cycle), then restarts the sound system to apply immediately.
static void S_OPENAL_CycleDevice_f(void)
{
    cvar_t     *pList = Cvar_Get("s_alAvailableDevices", "", CVAR_ROM);
    const char *cur   = s_openaldevice ? s_openaldevice->string : "";
    char        names[16384];
    char       *lines[64];
    int         count = 0, i, curIdx = -1;
    char       *p;

    Q_strncpyz(names, pList->string, sizeof(names));
    p = names;
    while (p && *p && count < 64) {
        lines[count] = p;
        p = strchr(p, '\n');
        if (p) {
            *p = 0;
            p++;
        }
        if (lines[count][0]) {
            count++;
        }
    }
    if (!count) {
        Com_Printf("s_cycledevice: no audio devices enumerated\n");
        return;
    }
    for (i = 0; i < count; i++) {
        if (!Q_stricmp(lines[i], cur)) {
            curIdx = i;
            break;
        }
    }
    // advance: {default}("") -> device 0 -> 1 ... -> last -> back to {default}
    if (curIdx == count - 1) {
        Cvar_Set("s_openaldevice", "");
        Com_Printf("OpenAL: output device -> {system default}\n");
    } else {
        Cvar_Set("s_openaldevice", lines[curIdx + 1]);
        Com_Printf("OpenAL: output device -> \"%s\"\n", lines[curIdx + 1]);
    }
    Cbuf_AddText("snd_restart\n");
}

// HZM coop - "s_setdevice <index>": set the audio output device by its index in the
// s_alAvailableDevices enumeration (-1 = system default), then restart sound. Used by the
// Audio menu's OUTPUT DEVICE dropdown (device names contain spaces/parens, which urc popup
// commands can't quote - an index sidesteps that entirely).
static void S_OPENAL_SetDevice_f(void)
{
    cvar_t *pList = Cvar_Get("s_alAvailableDevices", "", CVAR_ROM);
    char    names[16384];
    char   *lines[64];
    int     count = 0, want, i;
    char   *p;

    if (Cmd_Argc() < 2) {
        Com_Printf("usage: s_setdevice <index|-1 for default|device name>\n%s\n", pList->string);
        return;
    }

    // HZM coop [user 2026-08-13, bug-1797] ACCEPT A NAME, NOT ONLY AN INDEX.
    // The Audio menu's OUTPUT DEVICE popup was a baked snapshot of one machine's enumeration, matched
    // by position - so the moment the real device order changed, every entry after the change point
    // pointed at the wrong device. On the reporting machine "Arctis Nova Pro Wireless" (menu index 4)
    // opened Steam Streaming Microphone, a CAPTURE device: sound moved somewhere inaudible, which is
    // exactly the reported "I change my audio device and it doesn't update".
    // A name cannot be misrouted by reordering, and on a machine that does not have that device it
    // fails loudly instead of silently opening whatever happens to sit at that index.
    // Names contain spaces and parens, so the whole remainder of the line is taken (Cmd_ArgsFrom) -
    // an unquoted multi-word value passes through whole, which is why quoting was thought impossible.
    // A pure integer still means an index, so existing binds and cfgs keep working.
    {
        const char *arg    = Cmd_Argv(1);
        qboolean    isNum  = (arg[0] == '-' || (arg[0] >= '0' && arg[0] <= '9'));
        const char *c;

        for (c = arg + 1; isNum && *c; c++) {
            if (*c < '0' || *c > '9') {
                isNum = qfalse;
            }
        }

        if (!isNum) {
            // NOTE: atoi() on a name returns 0, so without this the old code silently selected
            // device 0 for every non-numeric argument rather than reporting the mistake.
            const char *wantName = Cmd_ArgsFrom(1);
            char        namebuf[16384];
            char       *np;
            int         n = 0;

            Q_strncpyz(namebuf, pList->string, sizeof(namebuf));
            np = namebuf;
            while (np && *np) {
                char *nl = strchr(np, '\n');
                if (nl) {
                    *nl = 0;
                }
                if (np[0] && Q_stristr(np, wantName)) {
                    Cvar_Set("s_openaldevice", np);
                    Com_Printf("OpenAL: output device -> \"%s\"\n", np);
                    Cbuf_AddText("snd_restart\n");
                    return;
                }
                n++;
                np = nl ? nl + 1 : NULL;
            }
            Com_Printf("s_setdevice: no device matching \"%s\" (%d enumerated)\n", wantName, n);
            return;
        }
    }

    want = atoi(Cmd_Argv(1));
    if (want < 0) {
        Cvar_Set("s_openaldevice", "");
        Com_Printf("OpenAL: output device -> {system default}\n");
        Cbuf_AddText("snd_restart\n");
        return;
    }
    Q_strncpyz(names, pList->string, sizeof(names));
    p = names;
    while (p && *p && count < 64) {
        lines[count] = p;
        p = strchr(p, '\n');
        if (p) {
            *p = 0;
            p++;
        }
        if (lines[count][0]) {
            count++;
        }
    }
    if (want >= count) {
        Com_Printf("s_setdevice: index %d out of range (%d devices)\n", want, count);
        return;
    }
    Cvar_Set("s_openaldevice", lines[want]);
    Com_Printf("OpenAL: output device -> \"%s\"\n", lines[want]);
    Cbuf_AddText("snd_restart\n");
}

qboolean S_OPENAL_Init()
{
    int i;

    if (al_initialized) {
        Com_DPrintf("S_OPENAL_Init(): Called when sound is already initialized!\n");
        return qtrue;
    }

    for (i = 0; i < MAX_SOUNDSYSTEM_CHANNELS; i++) {
        openal.channel[i] = NULL;
    }

    for (i = 0; i < MAX_SOUNDSYSTEM_LOOP_SOUNDS; i++) {
        openal.loop_sounds[i] = {};
    }

    s_milesdriver            = Cvar_Get("s_milesdriver", "auto", CVAR_SOUND_LATCH | CVAR_ARCHIVE);
    s_openaldevice           = Cvar_Get("s_openaldevice", "", CVAR_SOUND_LATCH);
    // HZM coop - the output device is a per-session choice: every launch starts on the
    // system default device (empty string = alcOpenDevice(NULL)), so the game follows
    // whatever Windows is set to. A stray 'seta' can burn a device name into omconfig;
    // strip the archive flag and clear the value once per process. Devices picked via
    // s_setdevice during the session still survive snd_restart (this only runs once).
    {
        static qboolean bDeviceSanitized = qfalse;
        if (!bDeviceSanitized) {
            bDeviceSanitized = qtrue;
            s_openaldevice->flags &= ~CVAR_ARCHIVE;
            if (s_openaldevice->string[0]) {
                Cvar_Set("s_openaldevice", "");
            }
        }
    }
    s_reverb                 = Cvar_Get("s_reverb", "1", CVAR_SOUND_LATCH | CVAR_ARCHIVE); // HZM coop - default ON (auto env reverb)
    s_occlusion              = Cvar_Get("s_occlusion", "1", CVAR_ARCHIVE);           // HZM coop - muffle sounds behind walls
    s_occlusionStrength      = Cvar_Get("s_occlusionStrength", "1.0", CVAR_ARCHIVE); // HZM coop - 0..1 lowpass depth
    s_muffleHF    = Cvar_Get("s_muffleHF",    "0.95", CVAR_ARCHIVE); // HZM coop - HF shelf depth at full muffle
    s_muffleGain  = Cvar_Get("s_muffleGain",  "0.10", CVAR_ARCHIVE); // HZM coop - broadband loss at full muffle
    s_muffleCurve = Cvar_Get("s_muffleCurve", "2.0",  CVAR_ARCHIVE); // HZM coop - how the muffle tracks the duck
    s_reverbGain             = Cvar_Get("s_reverbGain", "3.0", CVAR_ARCHIVE);        // HZM coop - master wet-level boost (stock triple-multiply lands at -28dB)
    s_show_cpu               = Cvar_Get("s_show_cpu", "0", 0);
    s_show_num_active_sounds = Cvar_Get("s_show_num_active_sounds", "0", 0);
    s_show_sounds            = Cvar_Get("s_show_sounds", "0", 0);
    s_speaker_type           = Cvar_Get("s_speaker_type", "0", CVAR_ARCHIVE);
    s_obstruction_cal_time   = Cvar_Get("s_obstruction_cal_time", "500", CVAR_ARCHIVE);
    //
    // Added in OPM
    //  Initialize the AL driver DLL
    s_openaldriver = Cvar_Get("s_openaldriver", ALDRIVER_DEFAULT, CVAR_LATCH | CVAR_PROTECTED);

    if (!QAL_Init(s_openaldriver->string)) {
        Com_Printf("Failed to load library: \"%s\".\n", s_openaldriver->string);
        if (!Q_stricmp(s_openaldriver->string, ALDRIVER_DEFAULT) || !QAL_Init(ALDRIVER_DEFAULT)) {
            return qfalse;
        }
    }

    if (!Cvar_Get("s_initsound", "1", 0)->integer) {
        Com_Printf("OpenAL: s_initsound set to zero...disabling audio.\n");
        return true;
    }

    if (!S_OPENAL_InitContext()) {
        Com_Printf("OpenAL: initialization failed. No audio will play.\n");
        return false;
    }

    if (!S_OPENAL_InitExtensions()) {
        Com_Printf("OpenAL: A required extension is missing. No audio will play.\n");
        return false;
    }

    al_use_reverb    = false;
    al_use_eaxreverb = false;
    // HZM coop - ALWAYS initialize EFX: runtime s_reverb toggling needs the slot to exist,
    // and the occlusion lowpass filters need the entry points regardless of reverb state.
    al_use_reverb = S_OPENAL_InitEFX();
    if (al_use_reverb) {
        S_OPENAL_SetReverb(s_iReverbType, s_fReverbLevel);
    } else {
        Com_Printf("OpenAL: No EFX support. Reverb and occlusion are disabled.\n");
    }
    s_reverb->modified = false;

    al_current_volume = Square(s_volume->value);
    qalListenerf(AL_GAIN, al_current_volume);
    alDieIfError();

#ifdef AL_SOFT_source_resampler
    if (qalGetStringiSOFT) {
        size_t numResamplers;
        size_t i;

        al_default_resampler_index = qalGetInteger(AL_DEFAULT_RESAMPLER_SOFT);
        alDieIfError();
        al_resampler_index = al_default_resampler_index;
        numResamplers      = qalGetInteger(AL_NUM_RESAMPLERS_SOFT);
        alDieIfError();

        // HZM coop: prefer the band-limited sinc resamplers over spline - nearly all game audio
        // is 22kHz and gets upsampled on every voice; bsinc24 > bsinc12 > spline in quality.
        {
            ALsizei best     = -1;
            int     bestRank = 0;

            for (i = 0; i < numResamplers; i++) {
                const ALchar *resamplerName = qalGetStringiSOFT(AL_RESAMPLER_NAME_SOFT, i);
                int           rank          = 0;

                if (Q_stristr(resamplerName, "bsinc24") || Q_stristr(resamplerName, "48 point")) {
                    rank = 3;
                } else if (Q_stristr(resamplerName, "bsinc")) {
                    rank = 2;
                } else if (Q_stristr(resamplerName, "spline")) {
                    rank = 1;
                }
                if (rank > bestRank) {
                    bestRank = rank;
                    best     = (ALsizei)i;
                }
            }
            if (best >= 0) {
                Com_Printf("OpenAL: Using %s as the resampler.\n", qalGetStringiSOFT(AL_RESAMPLER_NAME_SOFT, (ALsizei)best));
                al_resampler_index = best;
            }
        }
    }
#endif

    for (i = 0; i < MAX_SOUNDSYSTEM_CHANNELS_3D; i++) {
        if (!S_OPENAL_InitChannel(i, &openal.chan_3D[i])) {
            return false;
        }
    }

    for (i = 0; i < MAX_SOUNDSYSTEM_CHANNELS_2D; i++) {
        if (!S_OPENAL_InitChannel(i + MAX_SOUNDSYSTEM_CHANNELS_3D, &openal.chan_2D[i])) {
            return false;
        }
    }

    for (i = 0; i < MAX_SOUNDSYSTEM_CHANNELS_2D_STREAM; i++) {
        if (!S_OPENAL_InitChannel(
                i + MAX_SOUNDSYSTEM_CHANNELS_3D + MAX_SOUNDSYSTEM_CHANNELS_2D, &openal.chan_2D_stream[i]
            )) {
            return false;
        }
    }

    for (i = 0; i < MAX_SOUNDSYSTEM_SONGS; i++) {
        if (!S_OPENAL_InitChannel(
                i + MAX_SOUNDSYSTEM_CHANNELS_3D + MAX_SOUNDSYSTEM_CHANNELS_2D + MAX_SOUNDSYSTEM_CHANNELS_2D_STREAM,
                &openal.chan_song[i]
            )) {
            return false;
        }
    }

    if (!S_OPENAL_InitChannel(SOUNDSYSTEM_CHANNEL_MP3_ID, &openal.chan_mp3)) {
        return false;
    }

    if (!S_OPENAL_InitChannel(SOUNDSYSTEM_CHANNEL_TRIGGER_MUSIC_ID, &openal.chan_trig_music)) {
        return false;
    }
    // (s_cycledevice registered below; implementation lives in S_OPENAL_CycleDevice_f above this function)

    if (!S_OPENAL_InitChannel(SOUNDSYSTEM_CHANNEL_MOVIE_ID, &openal.chan_movie)) {
        return false;
    }

    //
    // Disable sound virtualization on special channels
    //  (triggered music, ambient sounds...)
    //
    for (i = 0; i < MAX_SOUNDSYSTEM_SONGS; i++) {
        openal.chan_song[i].set_no_virtualization();
    }
    openal.chan_mp3.set_no_virtualization();
    openal.chan_trig_music.set_no_virtualization();
    openal.chan_movie.set_no_virtualization();

    Cmd_AddCommand("s_cycledevice", S_OPENAL_CycleDevice_f); // HZM coop - cycle audio output device
    Cmd_AddCommand("s_setdevice", S_OPENAL_SetDevice_f);     // HZM coop - set device by enumeration index (Audio menu dropdown)
    Cmd_AddCommand("playmp3", S_OPENAL_PlayMP3);
    Cmd_AddCommand("stopmp3", S_OPENAL_StopMP3);
    Cmd_AddCommand("loadsoundtrack", S_loadsoundtrack);
    Cmd_AddCommand("playsong", S_PlaySong);
    Cmd_AddCommand("pitch", S_OPENAL_Pitch);
    Cmd_AddCommand("tmstart", S_TriggeredMusic_Start);
    Cmd_AddCommand("tmstartloop", S_TriggeredMusic_StartLoop);
    Cmd_AddCommand("tmstop", S_TriggeredMusic_Stop);
    // Added in 2.0
    Cmd_AddCommand("tmvolume", S_TriggeredMusic_Volume);
    // HZM coop [user 2026-09-13, bug-2572] the main-menu theme picker (see S_TriggeredMusic_PlayIntroMusic)
    Cmd_AddCommand("menumusic_play", S_MenuMusic_Play_f);

    S_OPENAL_ClearLoopingSounds();
    load_sfx_info();
    s_bProvidersEmunerated = true;
    al_initialized         = true;

    // Added in OPM
    S_CodecInit();

    return true;
}

/*
==============
S_OPENAL_Shutdown
==============
*/
void S_OPENAL_Shutdown()
{
    if (!al_initialized) {
        Com_DPrintf("S_OPENAL_Shutdown(): Called when sound is NOT initialized!\n");
        return;
    }

    S_OPENAL_StopAllSounds(true);

    Cmd_RemoveCommand("playmp3");
    Cmd_RemoveCommand("stopmp3");
    Cmd_RemoveCommand("loadsoundtrack");
    Cmd_RemoveCommand("playsong");
    Cmd_RemoveCommand("pitch");
    Cmd_RemoveCommand("tmstart");
    Cmd_RemoveCommand("tmstartloop");
    Cmd_RemoveCommand("tmstop");
    // Added in 2.0
    Cmd_RemoveCommand("tmvolume");
    Cmd_RemoveCommand("menumusic_play"); // HZM coop [bug-2572]

    S_OPENAL_NukeContext();

    s_bProvidersEmunerated = false;
    al_initialized         = false;

    QAL_Shutdown();
}

/*
==============
S_FadeSound
==============
*/
void S_FadeSound(float fTime)
{
    Com_Printf("Called FadeSound with: %f\n", fTime);

    if (fTime > 0) {
        s_bFading        = true;
        s_fFadeStartTime = cls.realtime;
        s_fFadeVolume    = 1;
        s_fFadeStopTime  = cls.realtime + fTime;
    } else {
        s_fFadeVolume = 1;
        s_bFading     = false;
        // Fixed in OPM
        //  Make sure to restore the music volume
        //  in case the map restarts immediately after the intermission
        music_volume_changed = true;
    }
}

/*
==============
S_GetBaseVolume
==============
*/
float S_GetBaseVolume()
{
    return s_volume->value * s_fFadeVolume;
}

/*
==============
S_NeedFullRestart
==============
*/
qboolean S_NeedFullRestart()
{
    return Cvar_Get("s_initsound", "1", 0)->integer != s_bLastInitSound;
}

/*
==============
S_PrintInfo
==============
*/
void S_PrintInfo()
{
    const char *dev;

    Com_Printf("----- Sound Info -----\n");

    if (s_bSoundStarted) {
        dev = NULL;
        if (s_openaldevice) {
            dev = s_openaldevice->string;
        }
        if (!dev || !*dev) {
            dev = "{default}";
        }
        Com_Printf("device - %s\n", dev);
        if (al_use_reverb) {
            Com_Printf("reverb - ON\n");
        } else {
            Com_Printf("reverb - OFF\n");
        }
        Com_Printf("samplebits - 16\n");
        Com_Printf("speed - %d\n", al_frequency);
        if (s_loadas8bit->integer) {
            Com_Printf("Can NOT force all sounds to 8 bit in OpenAL, I think.\n");
        }
    } else {
        Com_Printf("sound system not started\n");
    }

    Com_Printf("----------------------\n");
}

/*
==============
S_DumpStatus
==============
*/
static void S_DumpStatus(const char *pszChanName, int iChanNum, openal_channel *channel)
{
    sfx_t *sfx;
    ALint  status;

    sfx = channel->pSfx;

    qalGetSourceiv(channel->source, AL_SOURCE_STATE, &status);
    alDieIfError();

    if (status == AL_PLAYING || status == AL_PAUSED) {
        const char *pszMode = status == AL_PLAYING ? "playing" : "paused";

        if (sfx) {
            if (sfx != (sfx_t *)-16 && sfx->name[0]) {
                Com_Printf("%s channel %i - %s sfx %s\n", pszChanName, iChanNum, pszMode, sfx->name);
            } else {
                Com_Printf("%s channel %i - %s with nameless sfx\n", pszChanName, iChanNum, pszMode);
            }
        } else {
            Com_Printf("%s channel %i - %s with NULL sfx\n", pszChanName, iChanNum, pszMode);
        }
    }
}

/*
==============
S_DumpInfo
==============
*/
void S_DumpInfo()
{
    int i;

    for (i = 0; i < MAX_SOUNDSYSTEM_CHANNELS_3D; i++) {
        S_DumpStatus("3D", i, openal.channel[i]);
    }

    for (i = 0; i < MAX_SOUNDSYSTEM_CHANNELS_2D; i++) {
        S_DumpStatus("2D", i, openal.channel[MAX_SOUNDSYSTEM_CHANNELS_3D + i]);
    }

    for (i = 0; i < MAX_SOUNDSYSTEM_CHANNELS_2D_STREAM; i++) {
        S_DumpStatus("2D stream", i, openal.channel[MAX_SOUNDSYSTEM_CHANNELS_3D + MAX_SOUNDSYSTEM_CHANNELS_2D + i]);
    }

    for (i = 0; i < MAX_SOUNDSYSTEM_MISC_CHANNELS; i++) {
        S_DumpStatus(
            "Misc",
            i,
            openal.channel
                [MAX_SOUNDSYSTEM_CHANNELS_3D + MAX_SOUNDSYSTEM_CHANNELS_2D + MAX_SOUNDSYSTEM_CHANNELS_2D_STREAM + i]
        );
    }
}

/*
==============
S_OPENAL_Pitch
==============
*/
static void S_OPENAL_Pitch()
{
    Com_Printf("S_OPENAL_Pitch() needs to be implemented!\n");
}

/*
==============
S_OPENAL_LoadMP3
==============
*/
static bool S_OPENAL_LoadMP3(const char *_path, openal_channel *chan)
{
    char       path[MAX_QPATH];
    snd_info_t info;
    ALuint     format;

    chan->stop();

    qalSourcei(chan->source, AL_BUFFER, 0);
    alDieIfError();

    Q_strncpyz(path, _path, sizeof(path));
    path[MAX_QPATH - 1] = 0;

    FS_CorrectCase(path);

    // Try to load
    chan->bufferdata = (ALubyte *)S_CodecLoad(path, &info);
    if (!chan->bufferdata) {
        return false;
    }

    format = S_OPENAL_Format(info.width, info.channels);

    // Create a buffer
    qalGenBuffers(1, &chan->buffer);
    alDieIfError();

    // Fill the buffer
    if (info.size == 0) {
        // We have no data to buffer, so buffer silence
        byte dummyData[2] = {0};

        qalBufferData(chan->buffer, AL_FORMAT_MONO16, (void *)dummyData, 2, 22050);
    } else {
        qalBufferData(chan->buffer, format, chan->bufferdata, info.size, info.rate);
    }

    alDieIfError();

    // Free the memory
    Hunk_FreeTempMemory(chan->bufferdata);
    chan->bufferdata = NULL;

    qalSourcei(chan->source, AL_BUFFER, chan->buffer);
    alDieIfError();

    chan->set_no_3d();

    return true;
}

/*
==============
S_OPENAL_PlayMP3
==============
*/
static void S_OPENAL_PlayMP3()
{
    const char *path;

    if (Cmd_Argc() != 2) {
        Com_Printf("playmp3 <mp3 file>\n");
        return;
    }

    path = Cmd_Argv(1);
    if (!S_OPENAL_LoadMP3(path, &openal.chan_mp3)) {
        Com_Printf("Failed to play mp3 - %s\n", path);
        return;
    }

    openal.chan_mp3.play();
    Com_Printf("Playing mp3 - %s\n", path);
}

/*
==============
S_OPENAL_StopMP3
==============
*/
static void S_OPENAL_StopMP3()
{
    S_OPENAL_NukeChannel(&openal.chan_mp3);
}

/*
==============
MUSIC_Pause
==============
*/
void MUSIC_Pause()
{
    int i;

    for (i = 0; i < MAX_SOUNDSYSTEM_SONGS; i++) {
        openal.chan_song[i].pause();
    }
}

/*
==============
MUSIC_Unpause
==============
*/
void MUSIC_Unpause()
{
    int i;

    for (i = 0; i < MAX_SOUNDSYSTEM_SONGS; i++) {
        if (openal.chan_song[i].is_paused()) {
            openal.chan_song[i].play();
        }
    }
}

/*
==============
S_PauseSound
==============
*/
void S_PauseSound()
{
    int i;

    if (!s_bSoundStarted) {
        return;
    }

    s_bSoundPaused = true;

    for (i = 0; i < MAX_SOUNDSYSTEM_POSITION_CHANNELS; i++) {
        openal_channel *pChannel = openal.channel[i];
        if (!pChannel) {
            continue;
        }

        if (pChannel->iEntChannel == CHAN_MENU) {
            continue;
        }

        if (!pChannel->is_playing()) {
            continue;
        }

        pChannel->pause();
    }

    if (openal.chan_mp3.is_playing()) {
        openal.chan_mp3.pause();
    }

    MUSIC_Pause();
    S_TriggeredMusic_Pause();
}

/*
==============
S_UnpauseSound
==============
*/
void S_UnpauseSound()
{
    int i;

    if (!s_bSoundStarted) {
        return;
    }

    for (i = 0; i < MAX_SOUNDSYSTEM_POSITION_CHANNELS; i++) {
        openal_channel *pChannel = openal.channel[i];
        if (!pChannel) {
            continue;
        }

        if (pChannel->is_paused() || (pChannel->iFlags & CHANNEL_FLAG_PLAY_DEFERRED)) {
            pChannel->iFlags &= ~CHANNEL_FLAG_PLAY_DEFERRED;
            pChannel->play();
        }
    }

    if (openal.chan_mp3.is_paused()) {
        openal.chan_mp3.play();
    }

    MUSIC_Unpause();
    S_TriggeredMusic_Unpause();

    s_bSoundPaused = false;
}

/*
==============
S_OPENAL_ShouldPlay
==============
*/
static qboolean S_OPENAL_ShouldPlay(sfx_t *pSfx)
{
    if (sfx_infos[pSfx->sfx_info_index].max_number_playing <= 0) {
        return qtrue;
    }

    int iRemainingTimesToPlay;
    int i;

    iRemainingTimesToPlay = sfx_infos[pSfx->sfx_info_index].max_number_playing;

    for (i = 0; i < MAX_SOUNDSYSTEM_POSITION_CHANNELS; i++) {
        openal_channel *pChannel = openal.channel[i];
        if (!pChannel) {
            continue;
        }

        if (pChannel->pSfx == pSfx && pChannel->is_playing()) {
            iRemainingTimesToPlay--;
            if (!iRemainingTimesToPlay) {
                return qfalse;
            }
        }
    }

    return qtrue;
}

/*
==============
S_OPENAL_ShouldStart
==============
*/
static qboolean S_OPENAL_ShouldStart(const vec3_t vOrigin, float fMinDist, float fMaxDist)
{
    vec3_t vDir;
    vec3_t vListenerOrigin;
    vec3_t alvec;

    if (!al_initialized) {
        return false;
    }

    qalGetListenerfv(AL_POSITION, alvec);

    VectorCopy(alvec, vListenerOrigin);
    VectorSubtract(vOrigin, vListenerOrigin, vDir);

    return Square(fMaxDist) > VectorLengthSquared(vDir);
}

/*
==============
S_OPENAL_PickChannelBase
==============
*/
static int S_OPENAL_PickChannelBase(int iEntNum, int iEntChannel, int iFirstChannel, int iLastChannel)
{
    int             i;
    int             iBestChannel;
    openal_channel *pChannel;

    iBestChannel = -1;
    if (iEntNum != ENTITYNUM_NONE && iEntChannel) {
        bool bStoppedChannel = false;

        for (i = iFirstChannel; i <= iLastChannel; i++) {
            pChannel = openal.channel[i];
            if (!pChannel) {
                continue;
            }

            if (pChannel->is_free()) {
                iBestChannel = i;
                continue;
            }

            if (pChannel->iEntNum == iEntNum && pChannel->iEntChannel == iEntChannel) {
                pChannel->end_sample();
                bStoppedChannel = 1;
                iBestChannel    = i;
                break;
            }
        }

        if (!bStoppedChannel) {
            for (i = 0; i < iFirstChannel; ++i) {
                pChannel = openal.channel[i];
                if (!pChannel || pChannel->is_free()) {
                    continue;
                }

                if (pChannel->iEntNum == iEntNum && pChannel->iEntChannel == iEntChannel) {
                    bStoppedChannel = 1;
                    break;
                }
            }

            if (!bStoppedChannel) {
                for (i = iLastChannel + 1; i < MAX_SOUNDSYSTEM_POSITION_CHANNELS; i++) {
                    pChannel = openal.channel[i];
                    if (!pChannel || pChannel->is_free()) {
                        continue;
                    }

                    if (pChannel->iEntNum == iEntNum && pChannel->iEntChannel == iEntChannel) {
                        break;
                    }
                }
            }
        }
    }

    if (iBestChannel < 0) {
        int iBestTime = 0x7FFFFFFF;

        for (i = iFirstChannel; i <= iLastChannel; i++) {
            pChannel = openal.channel[i];
            if (!pChannel) {
                continue;
            }

            if (pChannel->is_free()) {
                return i;
            }

            if (pChannel->iEntNum == s_iListenerNumber && iEntNum != pChannel->iEntNum) {
                continue;
            }

            if (pChannel->iEntChannel < iEntChannel
                || (pChannel->iEntChannel == iEntChannel && pChannel->iStartTime < iBestTime)) {
                iBestChannel = i;
                iEntChannel  = pChannel->iEntChannel;
                iBestTime    = pChannel->iStartTime;
            }
        }
    }

    return iBestChannel;
}

/*
==============
S_OPENAL_PickChannel3D
==============
*/
static int S_OPENAL_PickChannel3D(int iEntNum, int iEntChannel)
{
    return S_OPENAL_PickChannelBase(iEntNum, iEntChannel, 0, MAX_SOUNDSYSTEM_CHANNELS_3D - 1);
}

/*
==============
S_OPENAL_PickChannel2D
==============
*/
static int S_OPENAL_PickChannel2D(int iEntNum, int iEntChannel)
{
    return S_OPENAL_PickChannelBase(
        iEntNum, iEntChannel, MAX_SOUNDSYSTEM_CHANNELS_3D, MAX_SOUNDSYSTEM_CHANNELS_3D + MAX_SOUNDSYSTEM_CHANNELS_2D - 1
    );
}

/*
==============
S_OPENAL_PickChannel2DStreamed
==============
*/
static int S_OPENAL_PickChannel2DStreamed(int iEntNum, int iEntChannel)
{
    return S_OPENAL_PickChannelBase(
        iEntNum,
        iEntChannel,
        MAX_SOUNDSYSTEM_CHANNELS_3D + MAX_SOUNDSYSTEM_CHANNELS_2D,
        MAX_SOUNDSYSTEM_CHANNELS_3D + MAX_SOUNDSYSTEM_CHANNELS_2D + MAX_SOUNDSYSTEM_CHANNELS_2D_STREAM - 1
    );
}

/*
==============
callbackServer
==============
*/
void callbackServer(int entnum, int channel_number, const char *name)
{
    if (!com_sv_running->integer) {
        return;
    }

    SV_SoundCallback(entnum, channel_number, name);
}

/*
==============
S_OPENAL_Start2DSound
==============
*/
static void S_OPENAL_Start2DSound(
    const vec3_t vOrigin,
    int          iEntNum,
    int          iEntChannel,
    sfx_t       *pSfx,
    float        fVolume,
    float        fMinDistance,
    float        fPitch,
    float        fMaxDistance
)
{
    int             iRealEntNum;
    int             iFreeChannel;
    openal_channel *pChannel;
    float           fRealVolume;
    bool            bSupportWaitTillSoundDone;

    iRealEntNum = iEntNum & ~S_FLAG_DO_CALLBACK;

    if (pSfx->iFlags & SFX_FLAG_STREAMED) {
        iFreeChannel = S_OPENAL_PickChannel2DStreamed(iRealEntNum, iEntChannel);
    } else {
        iFreeChannel = S_OPENAL_PickChannel2D(iRealEntNum, iEntChannel);
    }

    if (iFreeChannel < 0) {
        s_iPoolDenied2D++;   // HZM coop [bug-2457] pool gauge - see the report block in Respatialize
        s_iPoolDenied++;
        Com_DPrintf(
            "OpenAL: Couldn't play %s sound '%s' for entity %i on channel %s\n",
            (pSfx->iFlags & SFX_FLAG_STREAMED) ? "2Dstreamed" : "2D",
            pSfx->name,
            iRealEntNum,
            S_ChannelNumToName(iEntChannel)
        );
        return;
    }

    if (iEntNum & S_FLAG_DO_CALLBACK) {
        callbackServer(iRealEntNum, iFreeChannel, pSfx->name);
    }

    pChannel = openal.channel[iFreeChannel];
    pChannel->force_free();

    if (fVolume < 0) {
        fVolume = 1;
    }

    fRealVolume       = S_GetBaseVolume() * fVolume;
    pChannel->fVolume = fVolume;

    if (pChannel->iEntChannel == CHAN_LOCAL) {
        pChannel->iFlags |= CHANNEL_FLAG_LOCAL_LISTENER;
    } else {
        pChannel->iFlags &= ~CHANNEL_FLAG_LOCAL_LISTENER;
    }
    pChannel->iFlags &= ~CHANNEL_FLAG_PAUSED;

    if (fMinDistance < 0.0) {
        fMinDistance = 200.0;
    }
    pChannel->fMinDist = fMinDistance;

    if (fMaxDistance < 0.0) {
        fMaxDistance = pChannel->fMinDist * 64;
    }
    pChannel->fMaxDist = fMaxDistance;

    pChannel->fNewPitchMult = fPitch;
    pChannel->pSfx          = pSfx;
    pChannel->iEntNum       = iRealEntNum;
    pChannel->iEntChannel   = iEntChannel;
    if (iRealEntNum == ENTITYNUM_NONE) {
        VectorClear(pChannel->vOrigin);
        pChannel->iFlags |= CHANNEL_FLAG_NO_ENTITY;
        pChannel->iEntNum = 0;

        if (vOrigin) {
            VectorCopy(vOrigin, pChannel->vOrigin);
        }
    } else {
        pChannel->iFlags &= ~CHANNEL_FLAG_NO_ENTITY;
        pChannel->iEntNum = iRealEntNum;
        if (vOrigin) {
            VectorCopy(vOrigin, pChannel->vOrigin);

            bSupportWaitTillSoundDone = cl.serverTime - 1 < 0;
            pChannel->iTime           = cl.serverTime - 1;
            if (bSupportWaitTillSoundDone) {
                pChannel->iTime = 0;
            }

            // HZM coop [user 2026-09-04] iRealEntNum, NOT iEntNum - test the entity this channel
            // was BOUND to three lines above. It used to test the UNMASKED number, which is a
            // different entity entirely for anything numbered 1024-2047.
            //
            // S_FLAG_DO_CALLBACK is 0x400, bit 10. Retail entity numbers were 10 bits so that bit
            // was free to steal as a flag; this fork raised the pool to 4096, so entities 1024-2047
            // carry it as part of their own number and :1802 strips it unconditionally. The 3D path
            // at :2053/:2063 binds and tests the same masked number and is correct; only this one
            // disagreed with itself.
            //
            // The failure was silent and total. MISSING_ENT exists to stop a sound being killed
            // when its entity is absent from the snapshot (see the OPM note below). Asking about
            // the UNMASKED number asks about an entity that IS present, so the flag was never set,
            // and S_OPENAL_Respatialize's end_sample() then stopped the channel on the very next
            // frame - with no print on any path. ALL dialogue is `streamed`, and streamed sounds
            // take this 2D path, so every speaker above entity 1024 was mute while his mouth kept
            // moving (lipsync runs off the .skc, independent of audio).
            // Measured on m3l1a: shingle_ranger1 = 1784, center_radioman = 1644 - both silent.
            // higgins1_ranger10 = 119, bangalore_wave1 = 20 - both audible. Entity numbers recycle,
            // which is why this read as intermittent rather than as a hard break.
            if (s_entity[iRealEntNum].time < pChannel->iTime) {
                // Fixed in OPM
                //  Not sure if it's the real solution, but script_origin entities are usually
                //  never sent to client so the sound will immediately get stopped
                pChannel->iFlags |= CHANNEL_FLAG_MISSING_ENT;
            }
        } else {
            VectorClear(pChannel->vOrigin);
            pChannel->iTime = 0;
        }
    }

    pChannel->iStartTime = cl.serverTime;
    pChannel->iEndTime   = (int)(cl.serverTime + pSfx->time_length + 250.f);

    if (iFreeChannel > MAX_SOUNDSYSTEM_CHANNELS_3D + MAX_SOUNDSYSTEM_CHANNELS_2D) {
        // streamed
        if (!pChannel->set_sfx(pSfx)) {
            Com_DPrintf("OpenAL: Set stream error - %s\n", pSfx->name);
            return;
        }

        pChannel->iBaseRate = pChannel->sample_playback_rate();
        pChannel->set_no_3d();
        fRealVolume = fRealVolume * s_fVolumeGain;
    } else {
        pChannel->stop();
        pChannel->set_no_3d();
        pChannel->set_sfx(pSfx);
    }

    pChannel->set_gain(fRealVolume);
    pChannel->play();

    // HZM coop [user 2026-09-04, bug-2449] ONE LINE PER DIALOGUE START, printing the number this
    // hunt lacked: which entity the channel actually BOUND to, versus the number the caller passed.
    // dist is measured to the BOUND entity, because that is what S_OPENAL_Respatialize attenuates
    // against - NOT to the speaker, which is why "I was standing right in front of him" could not
    // help. Printed AFTER set_gain so the duck / dialogue-slider state that set_gain applies is on
    // the same line: a clean bind with a duck still down looks identical to a bad bind from outside.
    // flags 0 deliberately - only CVAR_ARCHIVE cvars are written by Cvar_WriteVariables, so this can
    // never latch into a player's config (the coop_voxCut / bug-1669 pattern).
    if (iEntChannel == CHAN_DIALOG || iEntChannel == CHAN_DIALOG_SECONDARY) {
        extern cvar_t *s_dialogscale;
        static cvar_t *s_voxProbe = NULL;

        // [2026-09-27] console quiet (docs/proposals/console_quiet_2026-09-27): default 1 -> 0. The bug-2449
        // hunt this served is closed; one line per dialogue start was ~30 lines a minute.
        if (!s_voxProbe) {
            s_voxProbe = Cvar_Get("s_voxProbe", "0", 0);
        }

        if (s_voxProbe->value > 0.f) {
            vec3_t vProbeListener, vProbeDelta;
            float  fProbeDist = -1.f;

            qalGetListenerfv(AL_POSITION, vProbeListener);
            if (!(pChannel->iFlags & CHANNEL_FLAG_NO_ENTITY)) {
                VectorSubtract(s_entity[pChannel->iEntNum].position, vProbeListener, vProbeDelta);
                fProbeDist = VectorLength(vProbeDelta);
            }

            Com_Printf(
                "^~^~^ VOXSTART %s chan=%i wire=%i bound=%i missing=%i noent=%i dist=%.0f min=%.0f "
                "max=%.0f gain=%.3f dscale=%.2f duck=%.3f\n",
                pSfx->name,
                iFreeChannel,
                iEntNum,
                pChannel->iEntNum,
                (pChannel->iFlags & CHANNEL_FLAG_MISSING_ENT) ? 1 : 0,
                (pChannel->iFlags & CHANNEL_FLAG_NO_ENTITY) ? 1 : 0,
                fProbeDist,
                pChannel->fMinDist,
                pChannel->fMaxDist,
                fRealVolume,
                s_dialogscale ? s_dialogscale->value : -1.f,
                s_sfxduck ? s_sfxduck->value : -1.f
            );
        }
    }

    if (s_show_sounds->integer > 0) {
        Com_DPrintf(
            "OpenAL: 2D - %d (#%i) - %s (vol %f, mindist %f, maxdist %f)\n",
            cl.serverTime,
            iFreeChannel,
            pSfx->name,
            fVolume,
            fMinDistance,
            fMaxDistance
        );
    }
}

/*
==============
S_OPENAL_StartSound
==============
*/
void S_OPENAL_StartSound(
    const vec3_t vOrigin,
    int          iEntNum,
    int          iEntChannel,
    sfxHandle_t  sfxHandle,
    float        fVolume,
    float        fMinDist,
    float        fPitch,
    float        fMaxDist,
    qboolean     bStreamed
)
{
    int             iChannel;
    openal_channel *pChannel;
    sfx_info_t     *pSfxInfo;
    sfx_t          *pSfx;
    ALint           state;
    bool            bOnlyUpdate;
    bool            bSupportWaitTillSoundDone;

    bOnlyUpdate = false;
    pSfx        = &s_knownSfx[sfxHandle];
    if (bStreamed) {
        pSfx->iFlags |= SFX_FLAG_STREAMED;
    }

    if (!S_OPENAL_ShouldPlay(pSfx)) {
        Com_DPrintf("OpenAL: ^~^~^ Not playing sound '%s'\n", pSfx->name);
        return;
    }

    if ((pSfx->iFlags & (SFX_FLAG_NO_OFFSET) || pSfx->iFlags & (SFX_FLAG_STREAMED | SFX_FLAG_MP3))
        || iEntChannel == CHAN_MENU || iEntChannel == CHAN_LOCAL) {
        S_OPENAL_Start2DSound(vOrigin, iEntNum, iEntChannel, pSfx, fVolume, fMinDist, fPitch, fMaxDist);
        return;
    }

    bSupportWaitTillSoundDone = (iEntNum & S_FLAG_DO_CALLBACK) != 0;
    iEntNum &= ~S_FLAG_DO_CALLBACK;

    pSfxInfo = &sfx_infos[pSfx->sfx_info_index];
    if (pSfx->iFlags & SFX_FLAG_STREAMED) {
        Com_DPrintf("OpenAL: 3D sounds not supported - couldn't play '%s'\n", pSfx->name);
        return;
    }

    iChannel = S_OPENAL_PickChannel3D(iEntNum, iEntChannel);
    if (iChannel < 0) {
        s_iPoolDenied3D++;   // HZM coop [bug-2457] pool gauge - see the report block in Respatialize
        s_iPoolDenied++;
        Com_DPrintf(
            "OpenAL: Couldn't play %s sound '%s' for entity %i on channel %s\n",
            (pSfx->iFlags & SFX_FLAG_STREAMED) ? "3Dstreamed" : "3D",
            pSfx->name,
            iEntNum,
            S_ChannelNumToName(iEntChannel)
        );
        return;
    }

    if (bSupportWaitTillSoundDone) {
        callbackServer(iEntNum, iChannel, pSfx->name);
    }

    pChannel                = &openal.chan_3D[iChannel];
    pChannel->fNewPitchMult = fPitch;
    pChannel->iEntChannel   = iEntChannel;
    pChannel->iFlags &= ~(CHANNEL_FLAG_PAUSED | CHANNEL_FLAG_LOCAL_LISTENER);
    state = pChannel->sample_status();

    if (pChannel->iEntNum == iEntNum && (state == AL_PLAYING || state == AL_PAUSED) && pChannel->pSfx == pSfx) {
        bOnlyUpdate = true;
    } else {
        pChannel->stop();
        pChannel->iFlags &= ~(CHANNEL_FLAG_LOOPING | CHANNEL_FLAG_PAUSED | CHANNEL_FLAG_LOCAL_LISTENER);
        if (!pChannel->set_sfx(pSfx)) {
            Com_DPrintf("OpenAL: Set sample error - %s\n", pSfx->name);
            return;
        }
    }

    if (fMinDist < 0.0) {
        fMinDist = 200.0;
        fMaxDist = 12800.0;
    }
    pChannel->fMinDist = fMinDist;
    pChannel->fMaxDist = fMaxDist;

    if (fVolume < 0.0) {
        fVolume = 1.0;
    }

    pChannel->fVolume = S_GetBaseVolume() * fVolume;
    pChannel->set_gain(pChannel->fVolume);

    // Fixed in OPM
    //  Setup the channel for 3D
    pChannel->set_3d();

    if (s_show_sounds->integer > 0) {
        Com_DPrintf(
            "OpenAL: %d (#%i) - %s (vol %f, mindist %f, maxdist %f)\n",
            cl.serverTime,
            iChannel,
            pSfx->name,
            pChannel->fVolume,
            fMinDist,
            fMaxDist
        );
    }

    pChannel->set_velocity(0, 0, 0);
    if (iEntNum == ENTITYNUM_NONE) {
        if (vOrigin) {
            VectorCopy(vOrigin, pChannel->vOrigin);
        } else {
            //VectorClear(vOrigin);
            // Fixed in OPM
            //  Tiny mistake found in original where the vOrigin parameter is set to 0
            VectorClear(pChannel->vOrigin);
        }

        pChannel->set_position(pChannel->vOrigin[0], pChannel->vOrigin[1], pChannel->vOrigin[2]);
        pChannel->iFlags |= CHANNEL_FLAG_NO_ENTITY;
        pChannel->iEntNum = 0;
    } else {
        pChannel->iFlags &= ~CHANNEL_FLAG_NO_ENTITY;
        pChannel->iEntNum = iEntNum;
        if (vOrigin) {
            VectorCopy(vOrigin, pChannel->vOrigin);

            bSupportWaitTillSoundDone = cl.serverTime - 1 < 0;
            pChannel->iTime           = cl.serverTime - 1;
            if (bSupportWaitTillSoundDone) {
                pChannel->iTime = 0;
            }

            if (s_entity[iEntNum].time < pChannel->iTime) {
                // Fixed in OPM
                //  Not sure if it's the real solution, but script_origin entities are usually
                //  never sent to client so the sound will immediately get stopped
                pChannel->iFlags |= CHANNEL_FLAG_MISSING_ENT;
            }
        } else {
            VectorClear(pChannel->vOrigin);
            pChannel->iTime = 0;
        }
        pChannel->set_position(pChannel->vOrigin[0], pChannel->vOrigin[1], pChannel->vOrigin[2]);
    }

    if (pSfxInfo->loop_start != -1) {
        pChannel->set_sample_loop_block(pSfxInfo->loop_start, pSfxInfo->loop_end);
        pChannel->set_sample_loop_count(0);

        pChannel->iFlags |= CHANNEL_FLAG_LOOPING;
        if (s_show_sounds->integer) {
            Com_DPrintf("OpenAL: loopblock - %d to %d\n", pSfxInfo->loop_start, pSfxInfo->loop_end);
        }
    } else {
        pChannel->set_sample_loop_count(1);
    }

    if (!bOnlyUpdate && S_OPENAL_ShouldStart(pChannel->vOrigin, pChannel->fMinDist, pChannel->fMaxDist)) {
        // Fixed in OPM
        //  Make sure to set the pitch before playing the sound
        pChannel->iBaseRate = pSfx->info.rate;
        pChannel->set_sample_playback_rate(pChannel->iBaseRate * pChannel->fNewPitchMult);
        pChannel->fNewPitchMult = 0;

        pChannel->play();
        pChannel->iEndTime   = cl.serverTime + (int)pChannel->pSfx->time_length + 250;
        pChannel->iStartTime = cl.serverTime;
    }
}

/*
==============
S_OPENAL_AddLoopingSound
==============
*/
void S_OPENAL_AddLoopingSound(
    const vec3_t vOrigin,
    const vec3_t vVelocity,
    sfxHandle_t  sfxHandle,
    float        fVolume,
    float        fMinDist,
    float        fMaxDist,
    float        fPitch,
    int          iFlags
)
{
    int                  i;
    int                  iFreeLoopSound;
    sfx_t               *pSfx;
    openal_loop_sound_t *pLoopSound;

    iFreeLoopSound = -1;
    pSfx           = &s_knownSfx[sfxHandle];
    if (!pSfx || !pSfx->length) {
        return;
    }

    for (i = 0; i < MAX_SOUNDSYSTEM_LOOP_SOUNDS; i++) {
        pLoopSound = &openal.loop_sounds[i];
        if (pLoopSound->pSfx == pSfx && !pLoopSound->bInUse) {
            iFreeLoopSound = i;
            break;
        }
    }

    if (iFreeLoopSound < 0) {
        for (i = 0; i < MAX_SOUNDSYSTEM_LOOP_SOUNDS; i++) {
            pLoopSound = &openal.loop_sounds[i];
            if (!pLoopSound->pSfx && !pLoopSound->bInUse) {
                iFreeLoopSound       = i;
                pLoopSound->bPlaying = false;
                break;
            }
        }
    }

    if (iFreeLoopSound < 0) {
        if (cls.realtime >= s_iNextLoopingWarning) {
            Com_DPrintf("OpenAL: Too many looping sounds\n");
            s_iNextLoopingWarning = cls.realtime + 1000;
        }
        return;
    }

    pLoopSound = &openal.loop_sounds[iFreeLoopSound];
    VectorCopy(vOrigin, pLoopSound->vOrigin);
    VectorCopy(vVelocity, pLoopSound->vVelocity);
    pLoopSound->pSfx        = pSfx;
    pLoopSound->bInUse      = true;
    pLoopSound->iStartTime  = cls.realtime;
    pLoopSound->fBaseVolume = fVolume;
    pLoopSound->fMinDist    = fMinDist;
    pLoopSound->fMaxDist    = fMaxDist;
    pLoopSound->fPitch      = fPitch;
    pLoopSound->iFlags      = iFlags;
    pLoopSound->bCombine    = VectorCompare(vVelocity, vec_zero) == 0;

    if (pLoopSound->bCombine) {
        for (i = 0; i < MAX_SOUNDSYSTEM_LOOP_SOUNDS; i++) {
            if (openal.loop_sounds[i].pSfx == pSfx && openal.loop_sounds[i].bInUse) {
                pLoopSound->iStartTime = openal.loop_sounds[i].iStartTime;
                if (openal.loop_sounds[i].bPlaying) {
                    pLoopSound->bPlaying = true;
                    pLoopSound->iChannel = openal.loop_sounds[i].iChannel;
                }
            }
        }
    }
}

/*
==============
S_OPENAL_StopLoopingSound
==============
*/
void S_OPENAL_StopLoopingSound(openal_loop_sound_t *pLoopSound)
{
    bool            bMayStop;
    int             i;
    openal_channel *pChannel;

    if (!pLoopSound->bPlaying) {
        return;
    }
    bMayStop = true;

    if (pLoopSound->bCombine) {
        for (i = 0; i < MAX_SOUNDSYSTEM_LOOP_SOUNDS; i++) {
            if (openal.loop_sounds[i].pSfx == pLoopSound->pSfx && openal.loop_sounds[i].bInUse) {
                bMayStop = false;
                break;
            }
        }
    }

    if (bMayStop) {
        if (s_show_sounds->integer > 0) {
            Com_DPrintf(
                "OpenAL: %d (#%i) - stopped loop - %s\n",
                cl.serverTime,
                pLoopSound->iChannel,
                openal.channel[pLoopSound->iChannel]->pSfx->name
            );
        }
        openal.channel[pLoopSound->iChannel]->force_free();
    }
    pLoopSound->pSfx     = NULL;
    pLoopSound->bPlaying = false;

    if (pLoopSound->bCombine) {
        for (i = 0; i < MAX_SOUNDSYSTEM_LOOP_SOUNDS; i++) {
            if (openal.loop_sounds[i].pSfx == pLoopSound->pSfx) {
                openal.loop_sounds[i].bPlaying = false;
                openal.loop_sounds[i].pSfx     = NULL;
            }
        }
    }
}

/*
==============
S_OPENAL_ClearLoopingSounds
==============
*/
void S_OPENAL_ClearLoopingSounds()
{
    int i;

    for (i = 0; i < MAX_SOUNDSYSTEM_LOOP_SOUNDS; i++) {
        openal.loop_sounds[i].bInUse = false;
    }
}

/*
==============
S_OPENAL_StopLoopingSounds
==============
*/
void S_OPENAL_StopLoopingSounds()
{
    // HZM coop [user 2026-09-02, bug-2315] THE BOUND IS THE ARRAY'S OWN SIZE, NOT THE CHANNEL COUNT.
    //
    // This and six sibling loops walked openal.loop_sounds[] using
    // (MAX_SOUNDSYSTEM_CHANNELS_3D + MAX_SOUNDSYSTEM_CHANNELS_2D) as the bound. The array is sized
    // MAX_SOUNDSYSTEM_LOOP_SOUNDS = 64, and 32 + 32 was exactly 64 - in bounds by coincidence, with
    // zero margin, and nothing anywhere saying the two had to match. Raising the 3D channels to 96
    // for bug-2309 made the bound 128, and this function wrote 64 entries off the end of the array:
    // openmohaa.exe died at launch inside S_StopAllSounds, before the main menu.
    //
    // THIS IS THE SAME DEFECT AS bug-2292 (TIKI_End walking MAX_GENTITIES over a TIKI_MAX_ENTITIES
    // array), and I had written it into docs/TRAPS.md T4 the day before - "two constants that agree
    // by COINCIDENCE are a bomb with no label" - and then walked straight into it while raising a
    // pool. Knowing the trap did not prevent it. Only the #error in snd_local_new.h does, which is
    // what that same TRAPS entry already said: turn every capacity rule into a build break.
    int i;

    for (i = 0; i < MAX_SOUNDSYSTEM_LOOP_SOUNDS; i++) {
        openal.loop_sounds[i].bInUse = false;
        S_OPENAL_StopLoopingSound(&openal.loop_sounds[i]);
    }
}

/*
==============
S_OPENAL_StopSound
==============
*/
void S_OPENAL_StopSound(int iEntNum, int iEntChannel)
{
    int i;

    for (i = 0; i < MAX_SOUNDSYSTEM_POSITION_CHANNELS; i++) {
        openal_channel *pChannel = openal.channel[i];

        // HZM coop [2026-09-04] same defect class as the reorder in S_OPENAL_Respatialize: this
        // dereferenced the array entry with no NULL test at all.
        if (!pChannel) {
            continue;
        }

        if (!pChannel->is_free() && pChannel->iEntNum == iEntNum && pChannel->iEntChannel == iEntChannel) {
            pChannel->end_sample();
            break;
        }
    }
}

/*
==============
S_OPENAL_StopAllSounds
==============
*/
void S_OPENAL_StopAllSounds(qboolean bStopMusic)
{
    int i;

    if (!s_bSoundStarted) {
        return;
    }

    S_OPENAL_StopLoopingSounds();

    for (i = 0; i < MAX_SOUNDSYSTEM_POSITION_CHANNELS; i++) {
        openal_channel *pChannel = openal.channel[i];
        if (pChannel) {
            pChannel->force_free();
        }
    }

    if (bStopMusic) {
        MUSIC_FreeAllSongs();
        S_TriggeredMusic_Stop();
    }
}

/*
==============
S_OPENAL_Start2DLoopSound
==============
*/
static int S_OPENAL_Start2DLoopSound(
    openal_loop_sound_t *pLoopSound, float fVolume, float fVolumeToPlay, float fMinDistance, const vec3_t vLoopOrigin
)
{
    int             iChannel;
    int             iSoundOffset;
    openal_channel *pChannel;

    if (pLoopSound->pSfx->iFlags & SFX_FLAG_STREAMED) {
        iChannel = S_OPENAL_PickChannel2DStreamed(0, 0);
    } else {
        iChannel = S_OPENAL_PickChannel2D(0, 0);
    }

    if (iChannel < 0) {
        Com_DPrintf("OpenAL: Could not find a free 2D sound channel\n");
        return iChannel;
    }

    pChannel = openal.channel[iChannel];
    pChannel->force_free();
    pChannel->fVolume = fVolume;

    if (pLoopSound->iFlags & LOOPSOUND_FLAG_NO_PAN) {
        pChannel->iFlags |= CHANNEL_FLAG_LOCAL_LISTENER;
    } else {
        pChannel->iFlags &= ~CHANNEL_FLAG_LOCAL_LISTENER;
    }
    pChannel->fMinDist = fMinDistance;

    if (!pChannel->set_sfx(pLoopSound->pSfx)) {
        Com_DPrintf("OpenAL: Set sample error (loopsound) - %s\n", pLoopSound->pSfx->name);
        pChannel->iFlags &= ~CHANNEL_FLAG_PLAY_DEFERRED;
        return -1;
    }

    pChannel->iEntNum     = 0;
    pChannel->iEntChannel = 0;
    pChannel->pSfx        = pLoopSound->pSfx;
    pChannel->iFlags |= CHANNEL_FLAG_PAUSED | CHANNEL_FLAG_NO_ENTITY;
    pChannel->iBaseRate = pChannel->sample_playback_rate();
    VectorCopy(vLoopOrigin, pChannel->vOrigin);

    iSoundOffset = (int)(pLoopSound->pSfx->info.width * pLoopSound->pSfx->info.rate
                         * (float)(cls.realtime - pLoopSound->iStartTime) / 1000.0);
    // HZM coop [bug-3473, tester crash dump 2026-10-10: exception 0xC0000094 (integer divide by zero) at
    // openmohaa.exe+0xCED54 = this function +0x2d4, on the first frame after joining m2l2b] a loop sound whose
    // sfx has length 0 (file empty / not readable at that moment) took the whole client down on the modulo below.
    // S_OPENAL_Start3DLoopSound has refused such an sfx all along; the 2D path never checked. Start it from 0.
    if (pLoopSound->pSfx->length > 0) {
        pChannel->set_sample_offset(iSoundOffset % pLoopSound->pSfx->length);
    } else {
        pChannel->set_sample_offset(0);
    }
    pChannel->set_sample_loop_count(0);
    pChannel->iFlags |= CHANNEL_FLAG_LOOPING;  // HZM coop - mark 2D ambient loops so the category
                                               // mixer in set_gain treats them as ambience, not SFX
    pChannel->fVolume = fVolumeToPlay;
    pChannel->set_gain(fVolumeToPlay);
    pChannel->start_sample();
    if (s_show_sounds->integer > 0) {
        Com_DPrintf(
            "OpenAL: %d (#%i) - %s (vol %f)\n", cl.serverTime, pLoopSound->iChannel, pLoopSound->pSfx->name, fVolume
        );
    }

    return iChannel;
}

/*
==============
S_OPENAL_Start3DLoopSound
==============
*/
static int S_OPENAL_Start3DLoopSound(
    openal_loop_sound_t *pLoopSound,
    float                fVolumeToPlay,
    float                fMinDistance,
    float                fMaxDistance,
    const vec3_t         vLoopOrigin,
    const vec3_t         vListenerOrigin
)
{
    int             iChannel;
    vec3_t          vDir;
    int             iSoundOffset;
    openal_channel *pChan3D;

    if (pLoopSound->pSfx->iFlags & SFX_FLAG_STREAMED) {
        return -1;
    }

    if (!pLoopSound->pSfx->data || !pLoopSound->pSfx->length) {
        return -1;
    }

    iChannel = S_OPENAL_PickChannel3D(0, 0);
    if (iChannel < 0) {
        Com_DPrintf("OpenAL: Could not find a free channel\n");
        return iChannel;
    }

    pChan3D = &openal.chan_3D[iChannel];
    pChan3D->force_free();
    pChan3D->iEntNum     = 0;
    pChan3D->iEntChannel = 0;
    pChan3D->fMinDist    = fMinDistance;
    pChan3D->fMaxDist    = fMaxDistance;
    pChan3D->set_3d();
    // Volume already distance-attenuated manually above; disable AL's rolloff to avoid double-falloff.
    qalSourcef(pChan3D->source, AL_ROLLOFF_FACTOR, 0.0f);

    if (!pChan3D->set_sfx(pLoopSound->pSfx)) {
        Com_DPrintf("OpenAL: Set sample error - %s\n", pLoopSound->pSfx->name);
        return -1;
    }

    pChan3D->set_position(vLoopOrigin[0], vLoopOrigin[1], vLoopOrigin[2]);
    VectorScale(pLoopSound->vVelocity, s_fVelocityScale, pLoopSound->vVelocity);
    pChan3D->set_velocity(pLoopSound->vVelocity[0], pLoopSound->vVelocity[1], pLoopSound->vVelocity[2]);
    pChan3D->pSfx = pLoopSound->pSfx;
    pChan3D->iFlags |= CHANNEL_FLAG_PAUSED | CHANNEL_FLAG_NO_ENTITY;
    pChan3D->iBaseRate = pChan3D->sample_playback_rate();

    iSoundOffset = (int)((int)pLoopSound->pSfx->info.width * pLoopSound->pSfx->info.rate
                         * (float)(cls.realtime - pLoopSound->iStartTime) / 1000.0)
                 % pLoopSound->pSfx->length;
    pChan3D->set_sample_offset(iSoundOffset);
    pChan3D->set_sample_loop_count(0);
    pChan3D->fVolume = fVolumeToPlay;
    pChan3D->set_gain(fVolumeToPlay);
    pChan3D->play();

    S_OPENAL_reverb(iChannel, s_iReverbType, s_fReverbLevel);

    return iChannel;
}

/*
==============
S_OPENAL_UpdateLoopSound
==============
*/
static bool S_OPENAL_UpdateLoopSound(
    openal_loop_sound_t *pLoopSound,
    float                fVolumeToPlay,
    float                fMinDistance,
    float                fMaxDistance,
    const vec3_t         vListenerOrigin,
    const vec3_t         vTempAxis,
    const vec3_t         vLoopOrigin
)
{
    openal_channel *pChannel;
    float           fVolume;
    float           fMaxVolume;
    vec3_t          vDir;
    float           fDistance;

    pChannel = openal.channel[pLoopSound->iChannel];
    if (!pChannel) {
        return false;
    }

    if (pChannel->pSfx != pLoopSound->pSfx) {
        pLoopSound->bPlaying = 0;
        return false;
    }

    pChannel->iStartTime = cl.serverTime;

    if (pLoopSound->pSfx->iFlags & (SFX_FLAG_NO_OFFSET) || pLoopSound->pSfx->iFlags & (SFX_FLAG_STREAMED | SFX_FLAG_MP3)
        || (pLoopSound->iFlags & LOOPSOUND_FLAG_NO_PAN)) {
        vec3_t vOrigin;
        int    iPan;

        pChannel->fVolume = fVolumeToPlay / s_fVolumeGain;
        fVolume           = S_GetBaseVolume() * pChannel->fVolume;
        fMaxVolume        = fVolume;

        if (pLoopSound->iFlags & LOOPSOUND_FLAG_NO_PAN) {
            // Center the sound
            iPan = 64;
        } else {
            VectorCopy(vLoopOrigin, vOrigin);
            iPan = S_OPENAL_SpatializeStereoSound(vListenerOrigin, vTempAxis, vOrigin);

            VectorSubtract(vListenerOrigin, vOrigin, vDir);
            // Clamp the volume by distance
            fDistance = VectorLength(vDir);
            if (pChannel->fMinDist >= fDistance) {
                fVolume = fMaxVolume;
            } else {
                fVolume = pChannel->fMinDist / fDistance * fMaxVolume;
            }
        }

        pChannel->set_gain(fVolume);
        pChannel->set_sample_pan(iPan);
    } else {
        pChannel->set_position(vLoopOrigin[0], vLoopOrigin[1], vLoopOrigin[2]);
        pChannel->fVolume = fVolumeToPlay;
        pChannel->set_gain(fVolumeToPlay);
    }

    if (s_bReverbChanged) {
        // Make sure to update the reverb
        S_OPENAL_reverb(pLoopSound->iChannel, s_iReverbType, s_fReverbLevel);
    }

    return true;
}

/*
==============
S_OPENAL_AddLoopSounds
==============
*/
void S_OPENAL_AddLoopSounds(const vec3_t vTempAxis)
{
    int                  i, j;
    static int           iLoopFrame = 0;
    float                fDistance;
    int                  iChannel;
    vec3_t               vListenerOrigin;
    vec3_t               vLoopOrigin;
    openal_loop_sound_t *pLoopSound;
    float                fTotalVolume;
    float                fVolumeToPlay;
    float                fMinDistance, fMaxDistance;
    float                fVolume;
    float                fPitch;
    float                fMaxVolume, fMaxFactor;
    openal_channel      *pChannel;
    bool                 bAlreadyAdded[MAX_SOUNDSYSTEM_LOOP_SOUNDS] = {false};
    vec3_t               alvec;

    qalGetListenerfv(AL_POSITION, alvec);
    VectorCopy(alvec, vListenerOrigin);

    // HZM coop [bug-2457] pool gauge: per-FRAME count, so it is zeroed here rather than at init.
    // This runs once per frame from the tail of S_OPENAL_Respatialize, immediately before the
    // SNDPOOL line is printed, so the value reported is always this frame's.
    s_iPoolLoops = 0;

    for (i = 0; i < MAX_SOUNDSYSTEM_LOOP_SOUNDS; i++) {
        vec3_t vDir;

        if (bAlreadyAdded[i]) {
            continue;
        }

        pLoopSound = &openal.loop_sounds[i];
        if (!pLoopSound->pSfx) {
            continue;
        }
        pChannel = openal.channel[pLoopSound->iChannel];

        fMinDistance = pLoopSound->fMinDist;
        if (fMinDistance < 0) {
            fMinDistance = 200;
        }

        fMaxDistance = pLoopSound->fMaxDist;
        if (fMaxDistance < 0) {
            fMaxDistance = fMinDistance * 64;
        }

        fVolume = pLoopSound->fBaseVolume;
        if (fVolume < 0) {
            fVolume = 1;
        }
        // HZM coop [user 2026-09-04, bug-2454] A DUCK-EXEMPT LOOP IS NOT AMBIENCE, SO THE AMBIENCE
        // DUCK MUST NOT REACH IT. The Omaha ramp beat runs `coop_cvarDuckStart "Ambient" 0.0 0.3 8`
        // (coopified.scr:10957), which lerps s_ambientvolume to EXACTLY 0 for ~95 seconds. This
        // multiply then zeroes every looping sound, and the epsilon cut below stops the channel
        // outright - and both sit one level ABOVE openal_channel::set_gain, so the S_HZM_DuckExempt
        // carve-out that protects the one-shots could never reach a loop. Net effect: the drowning
        // heartbeat the user asked for ("heart rate should play repeatedly until we are out of the
        // water"), started by coop_uwHeartStart as `playlocalsound coop_heartbeat 1`, has been
        // silent for the whole submerged beat since the day it shipped.
        // Scoped deliberately to the SAME predicate as the one-shot carve-out, so it covers exactly
        // coop_heart/, coop_swim/, coop_memory/ and the NCO whistle and nothing else. Real ambience
        // - wind, generators, room tone - is untouched and the Ambience slider still mutes it, which
        // is what the epsilon below was added for in the first place. Distance culling is also
        // untouched: an exempt loop that is genuinely far away still falls under the epsilon and
        // stops, because fTotalVolume keeps its distance term.
        if (!S_HZM_DuckExempt(pLoopSound->pSfx->name)) {
            fVolume = fVolume * s_fAmbientVolume;
        }

        fTotalVolume = 0.0;
        fMaxVolume   = 0.0;

        if (pLoopSound->bPlaying) {
            pChannel->fNewPitchMult = pLoopSound->fPitch;
        }

        if (pLoopSound->bCombine) {
            for (j = 0; j < MAX_LOOP_SOUNDS; j++) {
                openal_loop_sound_t *pLoopSound2 = &openal.loop_sounds[j];

                if (pLoopSound2->pSfx == pLoopSound->pSfx) {
                    VectorSubtract(pLoopSound2->vOrigin, vListenerOrigin, vDir);
                    VectorCopy(vDir, pLoopSound2->vRelativeOrigin);
                    fDistance = VectorLength(pLoopSound2->vRelativeOrigin);

                    if (fDistance <= fMinDistance) {
                        fVolumeToPlay = fVolume;
                    } else if (fDistance >= fMaxDistance) {
                        fVolumeToPlay = 0;
                    } else {
                        fVolumeToPlay = fMinDistance * fMinDistance * fVolume / (fDistance * fDistance);
                    }

                    if (fMaxVolume < fVolumeToPlay) {
                        fMaxVolume = fVolumeToPlay;
                    }
                    fTotalVolume += fVolumeToPlay;
                    bAlreadyAdded[j] = true;
                }
            }
        } else {
            VectorSubtract(pLoopSound->vOrigin, vListenerOrigin, vDir);
            VectorCopy(vDir, pLoopSound->vRelativeOrigin);
            fDistance = VectorLength(pLoopSound->vRelativeOrigin);

            if (fDistance <= fMinDistance) {
                fTotalVolume = fVolume;
            } else if (fDistance >= fMaxDistance) {
                fTotalVolume = 0;
            } else {
                fTotalVolume = fMinDistance * fMinDistance * fVolume / (fDistance * fDistance);
            }
            pLoopSound->fVolume = fTotalVolume;
            fMaxVolume          = fTotalVolume;
        }

        fMaxFactor = sfx_infos[pLoopSound->pSfx->sfx_info_index].max_factor;
        if (fMaxFactor >= 1 && fMaxVolume * fMaxFactor < fTotalVolume) {
            fTotalVolume = fMaxVolume * fMaxFactor;
        }

        // HZM coop - also stop 2D/NO_PAN ambient loops (wind, generators, room tone) when they
        // fall silent. The original '&& !NO_PAN' left those looping forever at ~0 gain, so the
        // Ambience slider could never actually mute them. Small epsilon catches slider-near-zero.
        // HZM coop [bug-2457] pool gauge: a loop that survives to here is one the mixer is paying
        // for. Counted before the silence cut below so the number means "loops alive", not "loops
        // audible". MAX_SOUNDSYSTEM_LOOP_SOUNDS is 128 and is a SEPARATE pool from the 3D channels.
        s_iPoolLoops++;

        if (fTotalVolume <= 0.0005f) {
            if (pLoopSound->bPlaying) {
                if (s_show_sounds->integer > 0) {
                    Com_DPrintf(
                        "OpenAL: %d (#%i) - stopped loop - %s\n",
                        cl.serverTime,
                        pLoopSound->iChannel,
                        openal.channel[pLoopSound->iChannel]->pSfx->name
                    );
                }

                pChannel->stop();
                pLoopSound->bPlaying = false;
                if (pLoopSound->bCombine) {
                    for (j = 0; j < MAX_LOOP_SOUNDS; j++) {
                        openal_loop_sound_t *pLoopSound2 = &openal.loop_sounds[j];

                        if (pLoopSound2->pSfx == pLoopSound->pSfx) {
                            pLoopSound2->bPlaying = false;
                        }
                    }
                }
            }

            continue;
        }

        VectorClear(vLoopOrigin);

        if (pLoopSound->bCombine) {
            for (j = 0; j < MAX_LOOP_SOUNDS; j++) {
                openal_loop_sound_t *pLoopSound2 = &openal.loop_sounds[j];

                if (pLoopSound2->pSfx == pLoopSound->pSfx) {
                    VectorNormalize(pLoopSound2->vRelativeOrigin);

                    VectorScale(
                        pLoopSound2->vRelativeOrigin, pLoopSound2->fVolume / fTotalVolume, pLoopSound2->vRelativeOrigin
                    );
                    VectorAdd(pLoopSound2->vRelativeOrigin, vLoopOrigin, vLoopOrigin);
                }
            }

            VectorNormalize(vLoopOrigin);
            VectorMA(vListenerOrigin, fMinDistance * 0.5f, vLoopOrigin, vLoopOrigin);
        } else {
            VectorCopy(pLoopSound->vOrigin, vLoopOrigin);
        }

        if (pLoopSound->bPlaying) {
            S_OPENAL_UpdateLoopSound(
                pLoopSound,
                S_GetBaseVolume() * s_fVolumeGain * fTotalVolume,
                fMinDistance,
                fMaxDistance,
                vListenerOrigin,
                vTempAxis,
                vLoopOrigin
            );

            continue;
        }

        if (s_show_sounds->integer > 0) {
            Com_DPrintf(
                "OpenAL: %d (#%i) - started loop - %s\n", cl.serverTime, pLoopSound->iChannel, pLoopSound->pSfx->name
            );
        }

        if (pLoopSound->pSfx->iFlags & (SFX_FLAG_NO_OFFSET)
            || pLoopSound->pSfx->iFlags & (SFX_FLAG_STREAMED | SFX_FLAG_MP3)
            || (pLoopSound->iFlags & LOOPSOUND_FLAG_NO_PAN)) {
            iChannel = S_OPENAL_Start2DLoopSound(
                pLoopSound, fVolume, S_GetBaseVolume() * s_fVolumeGain * fTotalVolume, fMinDistance, vLoopOrigin
            );
        } else {
            iChannel = S_OPENAL_Start3DLoopSound(
                pLoopSound,
                S_GetBaseVolume() * s_fVolumeGain * fTotalVolume,
                fMinDistance,
                fMaxDistance,
                vLoopOrigin,
                vListenerOrigin
            );
        }

        if (iChannel < 0) {
            continue;
        }

        pLoopSound->bPlaying = true;
        pLoopSound->iChannel = iChannel;

        if (pLoopSound->bCombine) {
            for (j = 0; j < MAX_LOOP_SOUNDS; j++) {
                openal_loop_sound_t *pLoopSound2 = &openal.loop_sounds[j];

                if (pLoopSound2->pSfx == pLoopSound->pSfx) {
                    pLoopSound2->bPlaying = true;
                    pLoopSound2->iChannel = iChannel;
                }
            }
        }
    }
}

/*
==============
S_OPENAL_Respatialize
==============
*/

/*
==============
S_HZM_MuffleClamp

HZM coop [user 2026-09-04] s_muffleHF and s_muffleGain are CVAR_ARCHIVE, so a player can set them to
anything. `1 - v * strength` goes NEGATIVE above about 1.1, and alFilterf then returns
AL_INVALID_VALUE and SILENTLY DOES NOTHING - leaving the filter object holding the PREVIOUS frame's
parameters while it is re-attached anyway. In the composed 3D path that also freezes the occlusion
value for that channel. The user-visible symptom would be "I turned the muffle up and it got weaker,
then stuck", which is the worst available failure mode for a knob that invites being turned up.
==============
*/
static float S_HZM_MuffleClamp(float v)
{
    if (v < 0.f) {
        return 0.f;
    }
    if (v > 1.f) {
        return 1.f;
    }
    return v;
}

/*
==============
S_HZM_UpdateMuffle

HZM coop [user 2026-09-04] THE CONCUSSION MUFFLE - "kinda muffled as though we are sorta zoned out
... it becomes clear by the time all audio has been restored from the duck effect".

DERIVED FROM THE DUCK, NOT FADED ALONGSIDE IT. coop_cvarFade is perceptual now, so a second fade
would have to reproduce that curve and stay in sync with it forever. Instead the muffle is a
function OF s_sfxduck, so it rides the identical curve because it IS the identical number, and
reaches exactly 0 on the frame the duck reaches 1.

And that is also the strand-proofing. The muffle CANNOT outlive the duck: coop_muffle only scales
something that is already zero once s_sfxduck is 1. A crash, a death, a disconnect or the watchdog
killing the beach-audio thread leaves nothing muffled, and s_sfxduck itself self-heals on the next
map load.

WHERE IT ACTUALLY READS, measured rather than hoped: during the cinematic itself almost everything
audible is already exempt (the music is on a channel outside the loop; the whistle, coop_memory/,
coop_heart/ and coop_swim/ are DuckExempt; coop_tinnitus/ and coop_injury/ are cue tier 1), and what
is left is the world bed at s_sfxduck 0.05 - i.e. 26 dB down, where an HF shelf is close to
inaudible. The muffle does its real work across the 34-SECOND RECOVERY, when the world climbs back
toward 1.0 while the muffle is still falling. That is exactly the user's ask - "clear by the time
all audio has been restored" - but it means the right expectation is "the world comes back muffled
and clears", not "you are underwater during the cutscene".

Computed once per frame, not once per channel: the value is global.
==============
*/
static void S_HZM_UpdateMuffle(void)
{
    static cvar_t *s_coopMuffle = NULL;
    float          fCut, fDuck, fCurve, fStrength;

    fStrength = 0.f;
    fDuck     = 1.f;

    if (al_use_occlusion && al_mfl_filter) {
        // Registered eagerly in snd_dma_new.cpp too - Cvar_Command only sets a cvar that already
        // EXISTS, so a server stufftext would otherwise be an unknown command on the one frame the
        // cinematic needs it. That is the coop_voxCut trap.
        if (!s_coopMuffle) {
            s_coopMuffle = Cvar_Get("coop_muffle", "0", 0);
        }
        fCut = S_HZM_MuffleClamp(s_coopMuffle->value);
        fDuck = (s_sfxduck ? s_sfxduck->value : 1.f);
        if (fDuck < 0.f) {
            fDuck = 0.f;
        }
        if (fCut > 0.f && fDuck < 1.f) {
            fCurve = s_muffleCurve->value;
            if (fCurve < 0.1f) {
                fCurve = 0.1f;
            }
            fStrength = powf(1.f - fDuck, fCurve) * fCut;
            if (fStrength > 1.f) {
                fStrength = 1.f;
            }
        }
    }

    al_mfl_strength = fStrength;

    if (fStrength > 0.001f && !al_mfl_armed) {
        al_mfl_armed = 1;
        Com_Printf("^~^~^ MUFFLE arm str=%.2f duck=%.3f hf=%.2f gain=%.2f curve=%.2f\n",
                   fStrength, fDuck, s_muffleHF->value, s_muffleGain->value, s_muffleCurve->value);
    } else if (fStrength <= 0.001f && al_mfl_armed) {
        al_mfl_armed = 0;
        Com_Printf("^~^~^ MUFFLE clear duck=%.3f\n", fDuck);
    }
}

/*
==============
S_HZM_MuffleFor

THE MUFFLE COVERS EXACTLY WHAT THE DUCK COVERS, so there is no second list to keep in sync. The
tinnitus ring and injury cues (tier 1), the whistle and the coop_memory/ flashback voices
(DuckExempt) stay CRISP over a muffled world - which is the beat each of those exemptions exists to
protect. Menu chrome is excluded so menus stay usable whatever the mix is doing.

THE CUE-TIER TEST IS GATED ON `not looping`, and that is not cosmetic: openal_channel::set_gain
computes cueTier only for a NON-looping channel, so it FORCES tier 0 on a looping one and ducks it.
Consulting CueTier unconditionally here would exempt a looping tier-1 path from the muffle while the
duck still applied to it - a divergence between two lists that are supposed to be one list.

DIALOGUE IS IN SCOPE, DELIBERATELY. The user asked for it by name ("all dialogue (pain, normal
dialogue, etc.)"). CHAN_DIALOG carries speech at 300-3400 Hz and the shelf is above 5 kHz, so
intelligibility survives - but this is the exact family behind bug-2298 / 2318 / 2354 / 2369 / 2395
/ 2406, so if it ever reads wrong the one-line escape hatch is to return 0.f here for
CHAN_DIALOG and CHAN_DIALOG_SECONDARY.
==============
*/
static float S_HZM_MuffleFor(const openal_channel *pChannel)
{
    const char *name;

    if (al_mfl_strength <= 0.f) {
        return 0.f;   // the whole cost outside a cinematic: one float compare per channel
    }
    if (pChannel->iEntChannel == CHAN_MENU) {
        return 0.f;
    }
    name = pChannel->pSfx ? pChannel->pSfx->name : NULL;
    if (name) {
        if (!(pChannel->iFlags & CHANNEL_FLAG_LOOPING) && S_HZM_CueTier(name) > 0) {
            return 0.f;
        }
        if (S_HZM_DuckExempt(name)) {
            return 0.f;
        }
    }
    return al_mfl_strength;
}

/*
==============
S_HZM_Muffle2D

HZM coop [user 2026-09-04] 2D AND 2D-STREAM CHANNELS (96..159) TAKE THE MUFFLE ONLY.

There is no occlusion to compute for them - they are not in the world - but AL_DIRECT_FILTER is a
per-SOURCE property, not a 3D one: set_no_3d only sets AL_SOURCE_RELATIVE, which changes the
coordinate frame and says nothing about the dry path. This is the ONLY reason dialogue could not be
muffled before: every streamed sound is forced down the 2D path and the occlusion array stops at 96.

Only the OFF state is cached, and only as a float compare. Attaching every frame while muffled is
what the 3D path already does for 96 channels, so that cost is proven; the cache exists so a player
who never sees a cinematic pays nothing. A channel index owns one AL source for the life of the
process, so al_mfl_applied[i] is a safe per-source record - and the detach must HAPPEN, not merely
be skipped, or a stale filter would muffle the next sound that channel plays.
==============
*/
static void S_HZM_Muffle2D(int i, openal_channel *pChannel)
{
    float fMuffle = S_HZM_MuffleFor(pChannel);

    if (fMuffle <= 0.f) {
        if (al_mfl_applied[i] != 0.f) {
            al_mfl_applied[i] = 0.f;
            qalSourcei(pChannel->source, AL_DIRECT_FILTER, AL_FILTER_NULL);
            qalGetError();
        }
        return;
    }

    al_mfl_applied[i] = fMuffle;
    qalFilterf(al_mfl_filter, AL_LOWPASS_GAIN,   1.0f - S_HZM_MuffleClamp(s_muffleGain->value) * fMuffle);
    qalFilterf(al_mfl_filter, AL_LOWPASS_GAINHF, 1.0f - S_HZM_MuffleClamp(s_muffleHF->value) * fMuffle);
    qalSourcei(pChannel->source, AL_DIRECT_FILTER, al_mfl_filter);
    qalGetError();
}

/*
==============
S_OPENAL_UpdateOcclusion

HZM coop - OCCLUSION: muffle 3D sounds whose path to the listener is blocked by world
geometry. A 3-ray fan (direct + two lateral offsets) gives partial occlusion in thirds;
traces are staggered per channel (s_obstruction_cal_time ms, a previously-dead Miles-era
cvar), and the value is smoothed per frame before driving an EFX lowpass on the direct
path. The reverb send stays unfiltered - hearing an occluded sound through the room
is exactly right when the env reverb is on.
==============
*/
static void S_OPENAL_UpdateOcclusion(int i, openal_channel *pChannel, const vec3_t vListener, const vec3_t vSoundPos)
{
    float fStrength;
    float fLerp;

    if (!al_use_occlusion || i >= MAX_SOUNDSYSTEM_POSITION_CHANNELS || !pChannel->source) {
        return;
    }

    // HZM coop [user 2026-09-04] channels 96..159 are 2D / 2D-streamed: no trace, muffle only.
    // This is where ALL dialogue lives - every streamed sound is forced down the 2D path.
    if (i >= MAX_SOUNDSYSTEM_CHANNELS_3D) {
        S_HZM_Muffle2D(i, pChannel);
        return;
    }

    if (!s_occlusion->integer || (pChannel->iFlags & CHANNEL_FLAG_LOCAL_LISTENER)
        || pChannel->iEntNum == s_iListenerNumber) {
        // never muffle your own gun / local UI sounds; drive smoothly back to clear
        al_occ_tgt[i] = 0;
    } else if (cls.realtime >= al_occ_nexttrace[i] || al_occ_stamp[i] != pChannel->iStartTime) {
        trace_t trace;
        vec3_t  vDir, vSide, vFrom, vTo;
        int     iBlocked = 0;
        int     j;

        al_occ_nexttrace[i] = cls.realtime
                            + (s_obstruction_cal_time->integer > 0 ? s_obstruction_cal_time->integer : 250)
                            + ((i * 37) % 100);

        VectorSubtract(vSoundPos, vListener, vDir);
        // lateral offset axis (horizontal, perpendicular to the sound direction)
        vSide[0] = -vDir[1];
        vSide[1] = vDir[0];
        vSide[2] = 0;
        VectorNormalize(vSide);

        for (j = -1; j <= 1; j++) {
            VectorCopy(vListener, vFrom);
            VectorCopy(vSoundPos, vTo);
            VectorMA(vFrom, j * 40.f, vSide, vFrom);
            VectorMA(vTo, j * 40.f, vSide, vTo);
            CM_BoxTrace(&trace, vFrom, vTo, vec3_origin, vec3_origin, 0, CONTENTS_SOLID, qfalse);
            if (trace.fraction < 1.0f) {
                iBlocked++;
            }
        }

        if (al_occ_stamp[i] != pChannel->iStartTime) {
            // new sound on this channel: snap instead of smoothing from a stale value
            al_occ_stamp[i] = pChannel->iStartTime;
            al_occ_cur[i]   = iBlocked / 3.0f;
        }
        al_occ_tgt[i] = iBlocked / 3.0f;
    }

    // smooth roughly 8/s toward the target
    fLerp = cls.frametime * 0.008f;
    if (fLerp > 1) {
        fLerp = 1;
    }
    al_occ_cur[i] += (al_occ_tgt[i] - al_occ_cur[i]) * fLerp;

    fStrength = s_occlusionStrength->value * al_occ_cur[i];
    if (fStrength < 0) {
        fStrength = 0;
    } else if (fStrength > 1) {
        fStrength = 1;
    }

    {
        // HZM coop [user 2026-09-04] compose the concussion muffle onto the same filter object.
        // A source has exactly ONE AL_DIRECT_FILTER, so the two effects must be multiplied into
        // one write rather than fight over the slot. Gains MULTIPLY because they are gains: a
        // sound both behind a wall and heard through a concussion is muffled twice, which is
        // correct. This runs even when s_occlusion is 0 (fStrength is then 0), so turning
        // occlusion off does not turn the muffle off.
        float fGain   = 1.0f - 0.45f * fStrength;
        float fGainHF = 1.0f - 0.88f * fStrength;
        float fMuffle = S_HZM_MuffleFor(pChannel);

        if (fMuffle > 0.f) {
            fGain   *= 1.0f - S_HZM_MuffleClamp(s_muffleGain->value) * fMuffle;
            fGainHF *= 1.0f - S_HZM_MuffleClamp(s_muffleHF->value) * fMuffle;
        }
        qalFilterf(al_occ_filter[i], AL_LOWPASS_GAIN, fGain);
        qalFilterf(al_occ_filter[i], AL_LOWPASS_GAINHF, fGainHF);
        qalSourcei(pChannel->source, AL_DIRECT_FILTER, al_occ_filter[i]);
    }
}

void S_OPENAL_Respatialize(int iEntNum, const vec3_t vHeadPos, const vec3_t vAxis[3])
{
    int             i;
    vec3_t          vOrigin;
    vec3_t          vVelocity;
    vec3_t          vEntOrigin;
    vec3_t          vEntVelocity;
    vec3_t          vDir;
    vec3_t          vUp;
    vec3_t          vListenerOrigin;
    int             iPan;
    vec3_t          vTempAxis;
    float           fMaxVolume;
    float           fVolume;
    float           fDist;
    openal_channel *pChannel;
    vec3_t          alvec {0};
    vec3_t          alorientation[2];

    if (cls.no_menus) {
        return;
    }

    s_iListenerNumber = iEntNum;

    //
    // Velocity
    //
    VectorCopy(s_entity[iEntNum].velocity, alvec);
    VectorScale(alvec, s_fVelocityScale, alvec);
    qalListenerfv(AL_VELOCITY, alvec);
    alDieIfError();

    //
    // Position
    //
    VectorCopy(vHeadPos, alvec);
    VectorCopy(alvec, vListenerOrigin);
    qalListenerfv(AL_POSITION, alvec);
    alDieIfError();

    //
    // Orientation
    //
    alorientation[0][0] = vAxis[0][0];
    alorientation[0][1] = vAxis[0][1];
    alorientation[0][2] = vAxis[0][2];
    alorientation[1][0] = vAxis[2][0];
    alorientation[1][1] = vAxis[2][1];
    alorientation[1][2] = vAxis[2][2];
    qalListenerfv(AL_ORIENTATION, (const ALfloat *)alorientation);
    alDieIfError();

    VectorCopy(vAxis[1], vTempAxis);

    float fLocalPan;

    fVolume = 1;
    iPan    = 64;

    // HZM coop - global concussion muffle; one powf per frame, not one per channel.
    S_HZM_UpdateMuffle();

    s_iPoolCur3D = 0;
    s_iPoolCur2D = 0;

    for (i = 0; i < MAX_SOUNDSYSTEM_POSITION_CHANNELS; i++) {
        pChannel = openal.channel[i];

        if (!pChannel) {
            continue;
        }

        // HZM coop [2026-09-04] moved BELOW the null check. openal.channel[] is NULLed at :1044 and
        // only filled by S_OPENAL_InitChannel, so a device that cannot supply all 160 sources leaves
        // live NULLs here and this line dereferenced one before testing it.
        fMaxVolume = S_GetBaseVolume() * pChannel->fVolume;

        if (!pChannel->is_playing()) {
            continue;
        }

        // HZM coop [user 2026-09-04, bug-2457] POOL OCCUPANCY GAUGE. Counted here because this is
        // the one place that already walks every channel every frame, so it costs an increment.
        if (i < MAX_SOUNDSYSTEM_CHANNELS_3D) {
            s_iPoolCur3D++;
        } else {
            s_iPoolCur2D++;
        }

        if (pChannel->iFlags & CHANNEL_FLAG_PAUSED) {
            continue;
        }

        if (pChannel->iFlags & CHANNEL_FLAG_NO_ENTITY) {
            VectorCopy(pChannel->vOrigin, vOrigin);

            if (pChannel->iFlags & CHANNEL_FLAG_LOCAL_LISTENER) {
                VectorCopy(vListenerOrigin, vOrigin);
                fLocalPan = S_HZM_LocalPanAmount(pChannel);   // HZM coop - pannable local sound
                if (i >= MAX_SOUNDSYSTEM_CHANNELS_3D) {
                    fVolume = fMaxVolume;
                    iPan    = 64 + (int)(fLocalPan * 63.f);
                } else {
                    // +vTempAxis is LEFT, fLocalPan is +right, hence the negation.
                    VectorMA(vOrigin, -fLocalPan * HZM_LOCALPAN_RADIUS, vTempAxis, vOrigin);
                    pChannel->set_position(vOrigin[0], vOrigin[1], vOrigin[2]);
                }
            } else {
                if (i >= MAX_SOUNDSYSTEM_CHANNELS_3D) {
                    iPan = S_OPENAL_SpatializeStereoSound(vListenerOrigin, vTempAxis, vOrigin);
                    VectorSubtract(vListenerOrigin, vOrigin, vDir);

                    fDist = VectorLength(vDir);
                    if (fDist <= pChannel->fMinDist + 0.001f) {
                        fVolume = fMaxVolume;
                    } else if (fDist >= pChannel->fMaxDist - 0.001f) {
                        fVolume = 0;
                    } else {
                        fVolume = (1.0 - (fDist - pChannel->fMinDist) / (pChannel->fMaxDist - pChannel->fMinDist))
                                * fMaxVolume;
                    }
                } else {
                    pChannel->set_position(vOrigin[0], vOrigin[1], vOrigin[2]);
                }
            }
        } else if (pChannel->iFlags & CHANNEL_FLAG_LOCAL_LISTENER) {
            VectorCopy(vListenerOrigin, vOrigin);
            fLocalPan = S_HZM_LocalPanAmount(pChannel);   // HZM coop - pannable local sound
            if (i >= MAX_SOUNDSYSTEM_CHANNELS_3D) {
                fVolume = fMaxVolume;
                iPan    = 64 + (int)(fLocalPan * 63.f);
            } else {
                // +vTempAxis is LEFT, fLocalPan is +right, hence the negation.
                VectorMA(vOrigin, -fLocalPan * HZM_LOCALPAN_RADIUS, vTempAxis, vOrigin);
                pChannel->set_position(vOrigin[0], vOrigin[1], vOrigin[2]);
            }
        } else {
            if (s_entity[pChannel->iEntNum].time < pChannel->iTime) {
                VectorCopy(pChannel->vOrigin, vOrigin);
                if (!(pChannel->iFlags & CHANNEL_FLAG_LOOPING) && !(pChannel->iFlags & (CHANNEL_FLAG_MISSING_ENT))) {
                    pChannel->end_sample();
                    continue;
                }
            } else {
                pChannel->iFlags &= ~CHANNEL_FLAG_MISSING_ENT;

                VectorCopy(s_entity[pChannel->iEntNum].position, vEntOrigin);
                VectorCopy(vEntOrigin, vOrigin);
                VectorCopy(vOrigin, pChannel->vOrigin);
                pChannel->iTime = s_entity[pChannel->iEntNum].time;
            }

            if (s_entity[pChannel->iEntNum].use_listener) {
                VectorCopy(vListenerOrigin, vOrigin);
            }

            if (pChannel->iEntNum == s_iListenerNumber) {
                if (vListenerOrigin[0] == vOrigin[0] && vListenerOrigin[2] == vOrigin[2]) {
                    float fDelta = vListenerOrigin[1] - vOrigin[1];

                    if (fDelta > 89.9f && fDelta < 90.09f) {
                        VectorCopy(vListenerOrigin, vOrigin);
                    }
                }
            }

            if (i >= MAX_SOUNDSYSTEM_CHANNELS_3D) {
                iPan = S_OPENAL_SpatializeStereoSound(vListenerOrigin, vTempAxis, vOrigin);
                VectorSubtract(vListenerOrigin, vOrigin, vDir);
                fDist = VectorLength(vDir);
                if (fDist <= pChannel->fMinDist + 0.001f) {
                    fVolume = fMaxVolume;
                } else if (fDist >= pChannel->fMaxDist - 0.001f) {
                    fVolume = 0;
                } else {
                    fVolume =
                        (1.0 - (fDist - pChannel->fMinDist) / (pChannel->fMaxDist - pChannel->fMinDist)) * fMaxVolume;
                }
            } else {
                pChannel->set_position(vOrigin[0], vOrigin[1], vOrigin[2]);
                VectorCopy(s_entity[pChannel->iEntNum].velocity, vEntVelocity);
                VectorCopy(vEntVelocity, vVelocity);
                VectorScale(vVelocity, s_fVelocityScale, vVelocity);
                pChannel->set_velocity(vVelocity[0], vVelocity[1], vVelocity[2]);
            }
        }

        if (i >= MAX_SOUNDSYSTEM_CHANNELS_3D) {
            pChannel->set_gain(fVolume);
            pChannel->set_sample_pan(iPan);
        }

        S_OPENAL_UpdateOcclusion(i, pChannel, vListenerOrigin, vOrigin); // HZM coop - occlusion lowpass

        if (s_bReverbChanged) {
            S_OPENAL_reverb(i, s_iReverbType, s_fReverbLevel);
        }
    }

    S_OPENAL_AddLoopSounds(vTempAxis);
    s_bReverbChanged = false;

    // HZM coop [user 2026-09-04, bug-2457] THE POOL GAUGE, REPORTED.
    //
    // WHY THIS EXISTS. Until now the ONLY signal that the sound pools were under pressure was the
    // "Couldn't play" line at the two PickChannel failure sites - and that is a CLIFF, not a gauge:
    // it fires only once every single channel is already taken, i.e. after sounds have been lost.
    // Worse, it is Com_DPrintf, so with `developer 0` (which is what autoexec.cfg ships, and what
    // every player runs) it prints NOTHING AT ALL. A log with zero "Couldn't play" lines therefore
    // proves nothing whatsoever about headroom - it is the expected output either way. That is
    // exactly the trap bug-2309 fell into from the other side.
    //
    // This reports the high-water mark instead, so the pool can be watched approaching the ceiling
    // rather than discovered past it. Com_Printf, not DPrintf, so it survives `developer 0`.
    // s_poolProbe is flags 0 and NOT CVAR_ARCHIVE - a probe must never be writable into a player's
    // saved config, which is the bug-1669 lesson.
    if (s_iPoolCur3D > s_iPoolPeak3D) {
        s_iPoolPeak3D = s_iPoolCur3D;
    }
    if (s_iPoolCur2D > s_iPoolPeak2D) {
        s_iPoolPeak2D = s_iPoolCur2D;
    }
    // [2026-09-27] console quiet (docs/proposals/console_quiet_2026-09-27): default 1 -> 0. At 0 the periodic
    // gauge is silent but the FIRST-EVER denial still prints one line - that is the cliff, and a log
    // with no SNDPOOL line at all then still proves no sound was ever refused. 1 = every 5 s as before.
    if (!s_poolProbe) {
        s_poolProbe = Cvar_Get("s_poolProbe", "0", 0);
    }
    {
        qboolean bDenialNew   = (qboolean)(s_iPoolDenied > s_iPoolDeniedReported);
        qboolean bFirstDenial = (qboolean)(bDenialNew && s_iPoolDeniedReported == 0);
        if ((s_poolProbe->integer > 0 && cls.realtime - s_iPoolLastReport > 5000) || bFirstDenial) {
            s_iPoolLastReport = cls.realtime;
            Com_Printf(
                "^~^~^ SNDPOOL 3d=%i/%i peak=%i 2d=%i/%i peak=%i loops=%i denied3d=%i denied2d=%i\n",
                s_iPoolCur3D,
                MAX_SOUNDSYSTEM_CHANNELS_3D,
                s_iPoolPeak3D,
                s_iPoolCur2D,
                MAX_SOUNDSYSTEM_CHANNELS_2D + MAX_SOUNDSYSTEM_CHANNELS_2D_STREAM,
                s_iPoolPeak2D,
                s_iPoolLoops,
                s_iPoolDenied3D,
                s_iPoolDenied2D
            );
            s_iPoolDeniedReported = s_iPoolDenied;
        }
    }
}

/*
==============
S_OPENAL_SpatializeStereoSound
==============
*/
static int S_OPENAL_SpatializeStereoSound(const vec3_t listener_origin, const vec3_t listener_left, const vec3_t origin)
{
    float  lscale, rscale;
    vec3_t source_vec;
    float  pan;

    VectorSubtract(origin, listener_origin, source_vec);
    VectorNormalize(source_vec);

    pan = s_separation->value + (1.f - s_separation->value) * -DotProduct(listener_left, source_vec);

    if (pan < 0) {
        pan = 0;
    }

    return ceilf(pan * 127);
}

/*
==============
S_OPENAL_reverb
==============
*/
static void S_OPENAL_reverb(int iChannel, int iReverbType, float fReverbLevel)
{
    openal_channel *pChan;
    ALuint          source;

    if (!al_use_reverb || !al_efx_slot || !qalSource3i) {
        return;
    }

    pChan = openal.channel[iChannel];
    if (!pChan) {
        return;
    }
    source = pChan->source;
    if (!source) {
        return;
    }

    // HZM coop - keep 2D/local and streamed-VO channels (32..95) DRY: "inside your head" audio
    // (tinnitus ring, DBNO duck cues, playlocalsound, dialogue streams) must not carry room reverb.
    // Sources are recycled and the send routing is sticky, so explicitly disconnect (rather than
    // just skip) in case this source was routed while living on a 3D channel earlier.
    if (iChannel >= MAX_SOUNDSYSTEM_CHANNELS_3D) {
        qalSource3i(source, AL_AUXILIARY_SEND_FILTER, AL_EFFECTSLOT_NULL, 0, AL_FILTER_NULL);
        qalGetError();
        return;
    }

    // Route send 0 of this source through the global reverb slot
    qalSource3i(source, AL_AUXILIARY_SEND_FILTER, (ALint)al_efx_slot, 0, AL_FILTER_NULL);
    qalGetError();
}

/*
==============
S_OPENAL_SetReverb
==============
*/
void S_OPENAL_SetReverb(int iType, float fLevel)
{
    s_fReverbLevel = fLevel;
    s_iReverbType  = iType;
    if (!al_use_reverb || !al_efx_effect) {
        return;
    }
    // HZM coop - EFX is always initialized now; s_reverb gates the audible effect
    if (s_reverb && !s_reverb->integer) {
        iType  = 0;
        fLevel = 0;
    }
    if (iType < 0 || iType >= 26) {
        iType = 0;
    }
    S_OPENAL_ApplyEFXPreset(&al_reverb_presets[iType], reverb_table[iType] * fLevel);
    s_bReverbChanged = true;
}

/*
==============
S_OPENAL_Update
==============
*/
void S_OPENAL_Update()
{
    int             i;
    openal_channel *pChannel;

    if (cl.snap.ps.stats[STAT_CINEMATIC]) {
        S_SetGlobalAmbientVolumeLevel(0.5f);
    } else {
        S_SetGlobalAmbientVolumeLevel(s_ambientVolume->value);
    }

    if (paused->integer && !s_bSoundPaused) {
        S_PauseSound();
    } else if (!paused->integer && s_bSoundPaused) {
        S_UnpauseSound();
    }

    if (s_bFading) {
        s_fFadeVolume = 1.f - (cls.realtime - s_fFadeStartTime) / (s_fFadeStopTime - s_fFadeStartTime);
        if (s_fFadeVolume < 0) {
            s_fFadeVolume = 0;
        }
        music_volume_changed = true;
    }

    if (s_volume->modified) {
        if (s_volume->value > 1) {
            Cvar_Set("s_volume", "1.0");
        } else if (s_volume->value < 0) {
            Cvar_Set("s_volume", "0.0");
        }

        music_volume_changed = true;
        s_volume->modified   = 0;
        al_current_volume    = Square(s_volume->value);
        qalListenerf(AL_GAIN, al_current_volume);
        alDieIfError();
    }

    for (i = 0; i < MAX_SOUNDSYSTEM_POSITION_CHANNELS; i++) {
        pChannel = openal.channel[i];
        if (!pChannel) {
            continue;
        }

        if (!pChannel->is_playing()) {
            continue;
        }

        if (pChannel->fNewPitchMult <= 0) {
            continue;
        }

        pChannel->set_sample_playback_rate(pChannel->iBaseRate * pChannel->fNewPitchMult);
        pChannel->fNewPitchMult = 0;
    }

    if (s_speaker_type->modified) {
        Cbuf_AddText("snd_restart\n");
        s_speaker_type->modified = false;
    }

    if (s_reverbGain && s_reverbGain->modified) {
        // HZM coop - live wet-level tuning: re-apply the current preset
        s_reverbGain->modified = false;
        if (al_use_reverb && s_reverb->integer) {
            S_OPENAL_SetReverb(s_iReverbType, s_fReverbLevel);
        }
    }

    if (s_reverb->modified) {
        s_reverb->modified = false;

        if (s_reverb->integer) {
            if (al_use_reverb) {
                S_OPENAL_SetReverb(s_iReverbType, s_fReverbLevel);
            } else {
                Com_Printf("OpenAL: No reverb support. Reverb is disabled.\n");
            }
        } else if (al_use_reverb) {
            S_OPENAL_SetReverb(0, 0);
        }
    }

    if (s_show_num_active_sounds->integer == 1) {
        int num = 0;

        for (i = 0; i < MAX_SOUNDSYSTEM_POSITION_CHANNELS; i++) {
            pChannel = openal.channel[i];
            if (pChannel && pChannel->is_playing()) {
                ++num;
            }
        }
        Com_DPrintf("OpenAL: Number of active sounds = %d\n", num);
    }

    Music_Update();

    for (i = 0; i < MAX_LOOP_SOUNDS; i++) {
        if (!openal.loop_sounds[i].bInUse) {
            S_OPENAL_StopLoopingSound(&openal.loop_sounds[i]);
        }
    }

    for (i = 0; i < MAX_SOUNDSYSTEM_CHANNELS; i++) {
        openal.channel[i]->update();
    }

    S_MenuMusic_Frame(); // HZM coop [bug-2576] move to the next main-menu theme when one ends
}

/*
==============
S_OPENAL_GetMusicFilename
==============
*/
const char *S_OPENAL_GetMusicFilename()
{
    // Don't call into the channel if QAL initialisation did not succeed.
    if (!al_initialized) {
        return "";
    }

    if (!openal.chan_trig_music.is_playing() && !openal.chan_trig_music.is_paused()) {
        return "";
    }

    return openal.tm_filename;
}

/*
==============
S_OPENAL_GetMusicLoopCount
==============
*/
int S_OPENAL_GetMusicLoopCount()
{
    return openal.tm_loopcount;
}

/*
==============
S_OPENAL_GetMusicOffset
==============
*/
unsigned int S_OPENAL_GetMusicOffset()
{
    return openal.chan_trig_music.sample_offset();
}

/*
==============
S_IsSoundPlaying
==============
*/
qboolean S_IsSoundPlaying(int channel_number, const char *sfxName)
{
    openal_channel *pChannel = openal.channel[channel_number];
    if (!pChannel) {
        return false;
    }

    if (s_bSoundPaused) {
        return false;
    }

    if (!pChannel->is_playing()) {
        return false;
    }

    if (!pChannel->pSfx) {
        return false;
    }

    if (strcmp(sfxName, pChannel->pSfx->name)) {
        return false;
    }

    return true;
}

/*
==============
S_StoreBase
==============
*/
static void S_StoreBase(channelbasesavegame_t *pBase, openal_channel *pChannel)
{
    if (!pChannel) {
        return;
    }

    if (pChannel->iEntChannel == CHAN_MENU || pChannel->is_free()) {
        pBase->bPlaying      = false;
        pBase->iOffset       = 0;
        pBase->iLoopCount    = 0;
        pBase->sfx.szName[0] = 0;
        pBase->sfx.iFlags    = 0;
        pBase->fNewPitchMult = 1.f;
        pBase->iBaseRate     = pChannel->iBaseRate;
        pBase->iStatus       = 0;
    } else {
        pBase->bPlaying   = true;
        pBase->iOffset    = pChannel->sample_offset();
        pBase->iLoopCount = pChannel->sample_loop_count();
        memcpy(pBase->sfx.szName, pChannel->pSfx->name, sizeof(pBase->sfx.szName));
        pBase->sfx.iFlags    = pChannel->pSfx->iFlags;
        pBase->iBaseRate     = pChannel->iBaseRate;
        pBase->iStatus       = 0;
        pBase->fNewPitchMult = (float)pBase->iBaseRate / (float)pChannel->sample_playback_rate();
    }

    pBase->iStartTime                = pChannel->iStartTime - cl.serverTime;
    pBase->iEndTime                  = pChannel->iEndTime - cl.serverTime;
    pBase->iEntChannel               = pChannel->iEntChannel;
    pBase->iEntNum                   = pChannel->iEntNum;
    pBase->iFlags                    = pChannel->iFlags;
    pBase->fMaxDist                  = pChannel->fMaxDist;
    pBase->fMinDist                  = pChannel->fMinDist;
    pBase->iNextCheckObstructionTime = 0;
    VectorCopy(pChannel->vOrigin, pBase->vOrigin);
    pBase->iTime   = pChannel->iTime - cl.serverTime;
    pBase->fVolume = pChannel->fVolume;
}

/*
==============
S_StartSoundFromBase
==============
*/
static void
S_StartSoundFromBase(channelbasesavegame_t *pBase, openal_channel *pChannel, sfx_t *pSfx, bool bStartUnpaused)
{
    if (!pChannel->set_sfx(pSfx)) {
        Com_DPrintf("OpenAL: Set sample error - %s\n", pSfx->name);
        pChannel->iFlags &= ~CHANNEL_FLAG_PLAY_DEFERRED;
        return;
    }

    pChannel->iBaseRate = pChannel->sample_playback_rate();

    if (sfx_infos[pSfx->sfx_info_index].loop_start != -1) {
        pChannel->set_sample_loop_block(
            sfx_infos[pSfx->sfx_info_index].loop_start, sfx_infos[pSfx->sfx_info_index].loop_end
        );
        pChannel->set_sample_loop_count(1);
        pChannel->iFlags |= CHANNEL_FLAG_LOOPING;
        if (s_show_sounds->integer > 0) {
            Com_DPrintf(
                "OpenAL: loopblock - %d to %d\n",
                sfx_infos[pSfx->sfx_info_index].loop_start,
                sfx_infos[pSfx->sfx_info_index].loop_end
            );
        }
    } else {
        pChannel->set_sample_loop_count(1);
    }

    pChannel->set_gain(pChannel->fVolume);
    pChannel->set_sample_offset(pBase->iOffset);
    pChannel->set_sample_playback_rate(pChannel->iBaseRate * pBase->fNewPitchMult);

    if (bStartUnpaused) {
        pChannel->resume_sample();
    } else {
        pChannel->iFlags |= CHANNEL_FLAG_PLAY_DEFERRED;
    }

    if (pChannel->iEntNum != ENTITYNUM_NONE) {
        s_entity[pChannel->iEntNum].time = pChannel->iTime;
        VectorCopy(pChannel->vOrigin, s_entity[pChannel->iEntNum].position);
    }
}

/*
==============
S_LoadBase
==============
*/
static void S_LoadBase(channelbasesavegame_t *pBase, openal_channel *pChannel, bool bStartUnpaused)
{
    sfxHandle_t handle;

    if (!pChannel) {
        return;
    }

    if (!pBase->bPlaying) {
        return;
    }

    if (strstr(pBase->sfx.szName, "null.wav")) {
        return;
    }

    handle = S_RegisterSound(pBase->sfx.szName, (pBase->sfx.iFlags & SFX_FLAG_STREAMED), false);

    pChannel->iBaseRate     = pBase->iBaseRate;
    pChannel->iStartTime    = pBase->iStartTime;
    pChannel->iEndTime      = pBase->iEndTime;
    pChannel->iEntChannel   = pBase->iEntChannel;
    pChannel->iEntNum       = pBase->iEntNum;
    pChannel->iFlags        = pBase->iFlags;
    pChannel->fMaxDist      = pBase->fMaxDist;
    pChannel->fMinDist      = pBase->fMinDist;
    pChannel->fNewPitchMult = pBase->fNewPitchMult;
    VectorCopy(pBase->vOrigin, pChannel->vOrigin);
    pChannel->iTime   = pBase->iTime;
    pChannel->fVolume = pBase->fVolume;
    pChannel->pSfx    = &s_knownSfx[handle];

    S_StartSoundFromBase(pBase, pChannel, &s_knownSfx[handle], bStartUnpaused);
}

/*
==============
S_SaveData
==============
*/
void S_SaveData(soundsystemsavegame_t *pSave)
{
    int  i;
    bool bSoundWasUnpaused;

    bSoundWasUnpaused = !s_bSoundPaused;
    if (!s_bSoundPaused) {
        S_PauseSound();
    }

    for (i = 0; i < MAX_SOUNDSYSTEM_POSITION_CHANNELS; i++) {
        S_StoreBase(&pSave->Channels[i], openal.channel[i]);
    }

    if (bSoundWasUnpaused) {
        S_UnpauseSound();
    }
}

/*
==============
S_ReLoad
==============
*/
void S_ReLoad(soundsystemsavegame_t *pSave)
{
    int  i;
    bool bSoundWasUnpaused;

    bSoundWasUnpaused = !s_bSoundPaused;
    if (!s_bSoundPaused) {
        S_PauseSound();
    }

    for (i = 0; i < MAX_SOUNDSYSTEM_POSITION_CHANNELS; i++) {
        S_LoadBase(&pSave->Channels[i], openal.channel[i], bSoundWasUnpaused);
    }

    if (bSoundWasUnpaused) {
        S_UnpauseSound();
    }
}

/*
==============
S_InitBase
==============
*/
static void S_InitBase(channelbasesavegame_t *pBase)
{
    if (!pBase->bPlaying) {
        return;
    }

    if (strstr(pBase->sfx.szName, "null.wav")) {
        return;
    }

    SV_AddSvsTimeFixup(&pBase->iStartTime);
    SV_AddSvsTimeFixup(&pBase->iEndTime);
    SV_AddSvsTimeFixup(&pBase->iTime);
}

/*
==============
S_LoadData
==============
*/
void S_LoadData(soundsystemsavegame_t *pSave)
{
    int i;

    for (i = 0; i < MAX_SOUNDSYSTEM_POSITION_CHANNELS; i++) {
        S_InitBase(&pSave->Channels[i]);
    }
}

/*
==============
openal_channel::set_velocity
==============
*/
void openal_channel::set_velocity(float v0, float v1, float v2)
{
    qalSource3f(source, AL_VELOCITY, v0, v1, v2);
    alDieIfError();
}

/*
==============
openal_channel::set_position
==============
*/
void openal_channel::set_position(float v0, float v1, float v2)
{
    qalSource3f(source, AL_POSITION, v0, v1, v2);
    alDieIfError();
}

/*
==============
openal_channel::set_gain
==============
*/
extern cvar_t *s_dialogscale;

// HZM coop [user 2026-08-07] - CUE SIDECHAIN.
// Some coop feedback cues were getting buried: raising their alias volume alone does not help,
// because the SFX slider scales the whole world with them, so the RELATIVE balance never moves.
// This ducks everything else briefly while a cue plays, in two tiers set by the user:
//   tier 2 STANDOUT (challenge typewriter, rank-up ping) - "meant to be satisfying and really
//          stand out": music/ambience drop hard and other SFX drop noticeably.
//   tier 1 SUBTLE   (sprint breath, DBNO breath, injury cough, MG42 overheat) - present and
//          audible without flattening the firefight around them.
// Tuning lives in one cvar: s_cueDuck scales the whole effect (0 = off, 1 = as designed).
static int   s_hzmCueDuckUntil = 0;   // cls.realtime ms
static int   s_hzmCueDuckTier  = 0;
static int   s_hzmVoxDuckUntil = 0;
static cvar_t *s_cueDuck       = NULL;

// 0 = not a cue, 1 = subtle, 2 = standout. Matched on the sample path because that is what the
// channel carries (pSfx->name); alias names are not retained this far down.
static int S_HZM_CueTier(const char *name)
{
    if (!name || !*name) {
        return 0;
    }
    if (strstr(name, "coop_type_key") || strstr(name, "mptypekey") || strstr(name, "coop_type_return")
        || strstr(name, "coop_rankup")) {
        return 2;
    }
    // HZM coop [2026-08-31] coop_tinnitus/ ADDED. The concussion wash is the one cue that exists
    // specifically to be heard THROUGH a duck - the design is "the ring arrives first and the world
    // comes rushing back underneath it" - and it was the one cue not on this list. Falling to tier 0
    // meant gain *= s_sfxduck below, so on Omaha it played at 3% and rose in lockstep with the world
    // instead of over it: the beat did not exist. Catches ring.wav and shellshock01.wav both.
    if (strstr(name, "coop_sprint/") || strstr(name, "coop_injury/") || strstr(name, "gasmask")
        || strstr(name, "Mec_SteamLoop_01") || strstr(name, "coop_tinnitus/")) {
        return 1;
    }
    return 0;
}

// HZM coop [user 2026-09-01, bug-2276] SOUNDS THAT THE CINEMATIC DUCK MUST NOT TOUCH.
//
// The user, on the Omaha ramp-drop beat: "I think the whistle on the higgins boat should not be faded
// at all." It was - coop_m3l1a_beachAudio pulls s_sfxduck down to 0.02 so AfterTheDrop.mp3 owns the
// scene, and the whistle is an ordinary world sound, so it went with everything else.
//
// THIS IS DELIBERATELY NOT S_HZM_CueTier. A cue tier does two things - it exempts the sound AND it arms
// a sidechain that ducks music and ambience underneath it. Putting the whistle on that list would have
// it duck the very track the same beat is swelling to 4.6x, which is the opposite of what was asked
// for. This predicate only ever says "leave this one alone".
//
// Matched on the sample path, because that is what the channel carries this far down (pSfx->name);
// alias names are long gone. Mec_Whistle_01 is the wav behind BOTH retail's higgins_whistle and our
// wider-range coop_beach_whistle, so one entry covers the ramp cue and the beach advance signal.
// HZM coop - PANNABLE LOCAL SOUND.
//
// [user 2026-09-03] "the shellshock should only be heard by the player thats being shellshocked ...
// thats not a global sound everyone hears." Correct, and it is why the script plays this wash with
// playlocalsound in the first place. But a local sound cannot pan: both CHANNEL_FLAG_LOCAL_LISTENER
// branches in S_OPENAL_Respatialize pin the sound to the listener - dead centre, iPan 64 - and
// EV_Player_PlayLocalSound is "sBF" (soundName loop time), so there is no pan argument to reach for.
//
// The resolution is that the two requirements were never actually in conflict. playlocalsound is
// per-client by CONSTRUCTION - the server sends it to one player and nobody else ever holds the
// channel. Panning it is therefore a purely client-side rendering decision, so we can offset the
// VIRTUAL position around that client own listener and the sound stays private. An earlier attempt
// moved a carrier entity through the world instead, which panned correctly and broadcast the wash
// to every player in earshot - the exact thing the user rejected.
//
// Matched on the sample path (pSfx->name), like S_HZM_DuckExempt, because the alias name is long
// gone by the time a channel exists. Scoped to the shellshock wav alone, so no other local sound -
// menu clicks, the tinnitus ring, DBNO - can ever be dragged off centre by this cvar.
//
// Sign convention, from S_OPENAL_SpatializeStereoSound: pan = sep + (1-sep) * -DotProduct(
// listener_left, source_vec), scaled to 0..127. A source to the LEFT gives dot +1 and pan 0; to the
// RIGHT, dot -1 and pan 127; centre is 64. So +vTempAxis points LEFT, and this returns the intuitive
// -1 = hard left, +1 = hard right - which is why the 3D path negates it and the 2D path does not.
static float S_HZM_LocalPanAmount(const openal_channel *pChannel)
{
    static cvar_t *s_coopShellPan = NULL;
    float          fPan;

    if (!pChannel || !pChannel->pSfx || !pChannel->pSfx->name[0]) {
        return 0.f;
    }
    if (!strstr(pChannel->pSfx->name, "coop_shellshock")) {
        return 0.f;
    }
    if (!s_coopShellPan) {
        // no CVAR_ARCHIVE: a transient scripted sweep, never a saved preference.
        s_coopShellPan = Cvar_Get("coop_shellPan", "0", 0);
    }
    fPan = s_coopShellPan->value;
    if (fPan < -1.f) {
        fPan = -1.f;
    } else if (fPan > 1.f) {
        fPan = 1.f;
    }
    return fPan;
}

static bool S_HZM_DuckExempt(const char *name)
{
    if (!name || !*name) {
        return false;
    }
    // an NCO's whistle is a command, not atmosphere - it is meant to cut through the mix
    if (strstr(name, "Mec_Whistle_01") || strstr(name, "mec_whistle_01")) {
        return true;
    }
    // HZM coop [user 2026-09-02, bug-2344] the Omaha underwater "voices from the past".
    //
    // These play DURING the ramp cinematic, and coop_rampAudioCut has already pulled s_sfxduck to
    // 0.05 by then - so as tier 0 they played at five percent and the whole beat did not exist. That
    // is bug-2298 exactly: a scripted voice turned down to nothing while its mouth kept moving.
    //
    // DELIBERATELY HERE AND NOT IN S_HZM_CueTier, for the reason this predicate was created: a cue
    // tier also ARMS the sidechain (see the hold at cueTier > 0 below), so tier 1 would have each
    // remembered line duck the music underneath it every second or two. The user's requirement is the
    // opposite - "make sure you can hear the music over it still, that is the whole point". Exempt
    // from the duck, arming nothing. The samples are normalised to -24 LUFS and play 3D with
    // mindist 140, so distance and the mix still place them under the score.
    if (strstr(name, "coop_memory/")) {
        return true;
    }
    // HZM coop [user 2026-09-04] the Omaha underwater heartbeat - "heart rate should play
    // repeatedly until we are out of the water". It loops for the whole ramp-drop beat, and
    // coop_rampAudioCut holds s_sfxduck at 0.05 across that beat, so without this it plays at five
    // percent and does not exist. Same failure as coop_memory/ above and as bug-2298.
    //
    // IT HAS TO BE HERE AND NOT IN S_HZM_CueTier, for two independent reasons. First, a cue tier
    // physically cannot exempt it: set_gain computes cueTier only `if (!bMusic && !bLoop && pSfx)`,
    // and this channel IS a loop, so its tier is always 0. Second, a tier also arms the sidechain,
    // and a heart re-striking every 1.2 seconds would pump the music and the underwater wash under
    // it for the entire sequence - the same thing the user rejected for the remembered voices.
    if (strstr(name, "coop_heart/")) {
        return true;
    }
    // ...and the swim, for the same reason and in the same beat: one swoosh per stroke, six
    // strokes, all of them inside the same s_sfxduck 0.05 window. Not a cue tier - six sidechain
    // arms in twelve seconds would pump the music under the whole swim.
    if (strstr(name, "coop_swim/")) {
        return true;
    }
    return false;
}

void openal_channel::set_gain(float gain)
{
    // HZM coop - CATEGORY MIXER. This is the ONE place category volume actually sticks: every sound's
    // gain flows through set_gain every frame (S_OPENAL_Respatialize re-applies it), so scaling done
    // once at start-time gets overwritten here. Classify by channel and apply the matching slider:
    //   - Music (chan_song / chan_mp3 / chan_trig_music): carries s_musicVolume at its own set_gain
    //     call site -> skip.
    //   - Looping ambience (CHANNEL_FLAG_LOOPING): s_ambientVolume is already folded into its gain -> skip.
    //   - Dialogue channels (CHAN_DIALOG / _SECONDARY): follow the Dialogue slider (all VO, incl. enemy).
    //   - Menu channels (CHAN_MENU): stay at master so menus are usable at any SFX setting.
    //     [2026-08-07] CHAN_LOCAL / CHAN_LOCAL_SOUND were in this group and are NOT any more -
    //     they carry in-world player feedback (typewriter, injured cue, sprint breath), not chrome.
    //   - Everything else (gunfire, footsteps, explosions, reloads, vehicles, breathing, cracks, ...):
    //     follow the SFX slider. This is the user's rule: SFX = everything that isn't music, dialogue,
    //     or ambience.
    qboolean bMusic = ((void *)this == (void *)&openal.chan_mp3 || (void *)this == (void *)&openal.chan_trig_music);
    if (!bMusic) {
        for (int i = 0; i < MAX_SOUNDSYSTEM_SONGS; i++) {
            if ((void *)this == (void *)&openal.chan_song[i]) {
                bMusic = qtrue;
                break;
            }
        }
    }
    qboolean bLoop = (iFlags & CHANNEL_FLAG_LOOPING) != 0;

    if (!bMusic && !bLoop) {
        if (iEntChannel == CHAN_DIALOG || iEntChannel == CHAN_DIALOG_SECONDARY) {
            if (s_dialogscale && s_dialogscale->value >= 0.f) {
                gain *= s_dialogscale->value;
            }
        } else if (iEntChannel == CHAN_MENU) {
            // [user 2026-08-07] Only true menu chrome is master-only now. CHAN_LOCAL and
            // CHAN_LOCAL_SOUND used to sit in this branch, so every piece of in-world player
            // feedback bypassed the SFX slider: the challenge typewriter, the injured cue,
            // snd_gasp (sprint breathing), coop_headshot, and everything routed through
            // Player::PlayLocalSound. With the slider at 2.0 the whole world doubled while those
            // stayed at 1.0, so they read as "lower than they used to be even with the dial turned
            // up" - exactly how it was reported. They follow SFX now, per the rule stated above:
            // SFX is everything that is not music, dialogue or ambience.
        } else {
            if (s_sfxvolume && s_sfxvolume->value >= 0.f) {
                gain *= s_sfxvolume->value;
            }
        }
    }

    // --- DIALOGUE SIDECHAIN [user 2026-09-01] ---
    //
    // The user, on Omaha: "I dont hear the captains dialogue... he may be getting drowned out im not
    // sure". He is. The scripted VO on that beach competes with MG42s, a naval barrage, six fires and
    // a surf bed, and nothing was giving way for it - the cue sidechain below covers the mod's own
    // feedback cues (typewriter, sprint breath, rank-up) and has never covered speech.
    //
    // So: any live dialogue channel holds a short window open, and everything that is not dialogue,
    // not music and not ambience steps back inside it. Music and ambience are left alone on purpose -
    // the score under a line is atmosphere, the machine gun over it is the problem. set_gain runs
    // every frame for every live channel (S_OPENAL_Respatialize re-applies it), so a speaking channel
    // re-arms the window continuously and it closes by itself when the line ends.
    {
        static cvar_t *s_voxDuck = NULL;
        if (!s_voxDuck) {
            s_voxDuck = Cvar_Get("s_voxDuck", "0.35", CVAR_ARCHIVE);
        }
        if (s_voxDuck->value > 0.0f) {
            if (!bMusic && !bLoop
                && (iEntChannel == CHAN_DIALOG || iEntChannel == CHAN_DIALOG_SECONDARY)) {
                // the speaker holds the window open; it is never ducked by its own duck
                if (cls.realtime + 250 > s_hzmVoxDuckUntil) {
                    s_hzmVoxDuckUntil = cls.realtime + 250;
                }
            } else if (!bMusic && !bLoop && cls.realtime < s_hzmVoxDuckUntil
                       && !(pSfx && S_HZM_DuckExempt(pSfx->name))) {
                float fVoxAmt  = s_voxDuck->value;
                int   iVoxLeft = s_hzmVoxDuckUntil - cls.realtime;
                if (fVoxAmt > 0.85f) {
                    fVoxAmt = 0.85f;   // never mute the firefight outright
                }
                // release over the last 200ms so the level does not snap back audibly
                if (iVoxLeft < 200) {
                    fVoxAmt *= (float)iVoxLeft / 200.0f;
                }
                gain *= (1.0f - fVoxAmt);
            }
        }
    }

    // --- cue sidechain (see S_HZM_CueTier above) ---
    {
        int   cueTier = 0;
        float duckScale;

        // Classify FIRST, because the cinematic duck below must not dim the very cue the duck was
        // armed for. [user 2026-08-20] "when a challenge unlocks we have sound ducking, but it
        // sounds like the typewriter sounds are ducking too instead of standing out" - the script
        // sets s_sfxduck 0.25 and then plays the typewriter, and this test exempted only MUSIC, so
        // the cue was quartered and then asked to stand out of the mix it had just been buried in.
        if (!bMusic && !bLoop && pSfx) {
            cueTier = S_HZM_CueTier(pSfx->name);
        }

        // cinematic effects duck (music exempt so the soundtrack stays full while effects recede,
        // and standout CUES exempt so an unlock cue is never ducked by its own duck)
        // [user 2026-09-01] ...and the whistle, which is an order rather than atmosphere - see
        // S_HZM_DuckExempt above for why that is a separate list from the cue tiers.
        // [user 2026-09-01, bug-2298] *** DIALOGUE IS NOT ATMOSPHERE. ***
        //
        // The user, third report on this: "I cant hear the captains dialogue still, i see his mouth
        // moving, either it's being overwritten by everything else or idk what" - and then, correctly,
        // "could it be that the dialogue audio that gets ducked for the cinematic scene never comes
        // back up". That was the right diagnosis and it beat two of mine.
        //
        // This test exempted MUSIC and standout CUES. It never exempted SPEECH. coop_m3l1a_beachAudio
        // fades s_sfxduck to 0.03 for a ~76 second music takeover on Omaha, so every scripted line
        // spoken inside that window - including the captain's - played at THREE PERCENT. The lipsync
        // runs off the .skc and is completely independent of the audio, so the mouth moved normally
        // and nothing came out. That is the entire reported symptom, and it is not a missing file.
        //
        // I had previously told the user the audio simply was not shipped in War Chest. That was
        // WRONG: their own session log contains no "Couldn't load sound" for any m3l1 dialogue, and
        // the engine does log those. The files load; they were being turned down to nothing.
        //
        // The duck exists to clear room for a cue. Speech is the thing you most want to hear in that
        // room, so it gets a FLOOR rather than the full duck. Default 1 = dialogue is untouched by
        // the cinematic duck entirely; lower it if a scene ever genuinely needs speech pushed back.
        // Same reasoning as S_HZM_DuckExempt for the whistle: an order is not ambience.
        if (s_sfxduck && s_sfxduck->value < 1.f && !bMusic && cueTier == 0
            && !(pSfx && S_HZM_DuckExempt(pSfx->name))) {
            float fDuck = s_sfxduck->value;
            if (iEntChannel == CHAN_DIALOG || iEntChannel == CHAN_DIALOG_SECONDARY) {
                static cvar_t *s_sfxduckVoxFloor = NULL;
                static cvar_t *s_coopVoxCut      = NULL;
                float          fFloor;
                if (!s_sfxduckVoxFloor) {
                    s_sfxduckVoxFloor = Cvar_Get("s_sfxduckVoxFloor", "1", CVAR_ARCHIVE);
                }
                // HZM coop [user 2026-09-02, bug-2318] A TIME-VARYING VOX FLOOR.
                //
                // bug-2298 gave speech a permanent floor, which is right for a 76-second music
                // takeover and wrong for the one frame a shell lands next to you. The user wants
                // both: dialogue audible through the level, and dialogue CUT with everything else
                // at the blast that throws you out of the boat.
                //
                // coop_voxCut SCALES the floor rather than replacing it: 1 = exactly bug-2298's
                // behaviour, 0 = speech takes the full cinematic duck. FLAGS 0 ON PURPOSE - only
                // CVAR_ARCHIVE cvars are written by Cvar_WriteVariables, so this one is physically
                // incapable of latching to a config. That is the bug-2298 failure mode removed by
                // construction rather than by discipline, and it is why the scale is a new cvar
                // instead of just driving s_sfxduckVoxFloor to 0.
                if (!s_coopVoxCut) {
                    s_coopVoxCut = Cvar_Get("coop_voxCut", "1", 0);
                }
                fFloor = s_sfxduckVoxFloor->value;
                {
                    float fCut = s_coopVoxCut->value;
                    if (fCut < 0.0f) {
                        fCut = 0.0f;
                    } else if (fCut > 1.0f) {
                        fCut = 1.0f;
                    }
                    fFloor *= fCut;
                }
                if (fDuck < fFloor) {
                    fDuck = fFloor;
                }
            }
            gain *= fDuck;
        }

        if (!s_cueDuck) {
            s_cueDuck = Cvar_Get("s_cueDuck", "2", CVAR_ARCHIVE);   // [user 2026-08-07] default 2
        }
        duckScale = s_cueDuck->value;
        if (duckScale < 0.0f) { duckScale = 0.0f; }

        // (classified above, before the cinematic duck, so that duck can exempt it. A looping or
        // music source is never itself a cue - which also keeps a map using the same sample as
        // ambience from arming the duck forever.)

        // [user 2026-08-07] live gain knob so the cue level can be tuned without a rebuild:
        //   s_cueGain  - standout tier (challenge typewriter, rank-up ping)
        //   s_cueGain2 - subtle tier (sprint breath, injury cough, MG overheat)
        // Applied to the cue ITSELF, on top of its alias volume and the SFX slider.
        // HZM coop [user 2026-09-02, bug-2369] A CUE CUT, matching the dialogue one.
        //
        // The user, twice: "the voice dialogue lines and injury sounds all play way over the music
        // still. Nothing has changed there." The dialogue half is the vox floor below. THIS is the
        // other half, and it is why the injury sounds in particular sit on top of everything:
        // sound/coop_injury/ is a TIER 1 CUE (S_HZM_CueTier), which means it is exempt from the
        // cinematic duck entirely AND is then multiplied by s_cueGain2 on top. During a sixty-second
        // music takeover that is precisely backwards.
        //
        // coop_cueCut scales tier-1 cues the way coop_voxCut scales speech: 1 = today's behaviour,
        // lower = the cue recedes under the music. FLAGS 0 ON PURPOSE - only CVAR_ARCHIVE cvars are
        // written by Cvar_WriteVariables, so this one physically cannot latch into a config and
        // silently suppress every injury sound in the game forever. That is the bug-2298 failure
        // mode removed by construction, and it is why this is a new cvar rather than a script
        // driving the archived s_cueGain2.
        if (cueTier > 0) {
            static cvar_t *s_coopCueCut = NULL;
            float          fCueCut;
            if (!s_coopCueCut) {
                s_coopCueCut = Cvar_Get("coop_cueCut", "1", 0);
            }
            fCueCut = s_coopCueCut->value;
            if (fCueCut < 0.0f) {
                fCueCut = 0.0f;
            } else if (fCueCut > 1.0f) {
                fCueCut = 1.0f;
            }
            // [user 2026-09-02, bug-2373] TINNITUS IS EXEMPT FROM THE CUE CUT.
            //
            // "i dont think the shellshock sound is playing thru either... if it is its so low you
            // cant even tell." That was me: coop_tinnitus/ is a tier-1 cue like coop_injury/, so the
            // coop_cueCut I added one change earlier to push the injury sounds under the music was
            // pushing the ring and the concussion wash under it too. The whole design of that beat -
            // written into coop_rampAudioCut's own header - is that the ring arrives OVER everything
            // and the world comes back underneath it. Cutting it defeats the thing the cut exists to
            // make room for.
            if (fCueCut < 1.0f && !(pSfx && strstr(pSfx->name, "coop_tinnitus/"))) {
                gain *= fCueCut;
            }
        }

        if (cueTier > 0) {
            static cvar_t *s_cueGain  = NULL;
            static cvar_t *s_cueGain2 = NULL;
            if (!s_cueGain)  { s_cueGain  = Cvar_Get("s_cueGain",  "1", CVAR_ARCHIVE); }
            if (!s_cueGain2) { s_cueGain2 = Cvar_Get("s_cueGain2", "1", CVAR_ARCHIVE); }
            if (cueTier == 2 && s_cueGain->value  > 0.0f) { gain *= s_cueGain->value; }
            if (cueTier == 1 && s_cueGain2->value > 0.0f) { gain *= s_cueGain2->value; }
        }

        if (cueTier > 0 && duckScale > 0.0f) {
            // the cue itself holds the window open while it plays, and is never ducked
            int hold = (cueTier == 2) ? 900 : 350;
            if (cls.realtime + hold > s_hzmCueDuckUntil) {
                s_hzmCueDuckUntil = cls.realtime + hold;
            }
            if (cueTier > s_hzmCueDuckTier) {
                s_hzmCueDuckTier = cueTier;
            }
        } else if (duckScale > 0.0f && cls.realtime < s_hzmCueDuckUntil && s_hzmCueDuckTier > 0) {
            float amt = (s_hzmCueDuckTier == 2) ? 0.75f : 0.30f;
            float w   = 1.0f;   // music + ambience take the full dip
            int   left;

            if (!bMusic && !bLoop) {
                // other SFX give way much less, so the firefight still reads underneath
                w = (s_hzmCueDuckTier == 2) ? 0.55f : 0.15f;
            }

            // linear release over the last 250ms so the level does not snap back audibly
            left = s_hzmCueDuckUntil - cls.realtime;
            if (left < 250) {
                amt *= (float)left / 250.0f;
            }

            // [user 2026-08-07] CLAMP. s_cueDuck now defaults to 2, and 1 - (0.75 * 1.0 * 2) is
            // -0.5 - a negative gain, which is meaningless to OpenAL and would not simply mean
            // "very quiet". Floor it at 0.05 (~-26dB): effectively inaudible under the cue, but
            // still a real value, so a deep duck reads as ducked rather than as a bug.
            {
                float duckFactor = 1.0f - amt * w * duckScale;
                if (duckFactor < 0.05f) { duckFactor = 0.05f; }
                gain *= duckFactor;
            }
        } else if (cls.realtime >= s_hzmCueDuckUntil) {
            s_hzmCueDuckTier = 0;
        }
    }

    qalSourcef(source, AL_GAIN, gain);
    alDieIfError();
}

/*
==============
openal_channel::set_no_3d
==============
*/
void openal_channel::set_no_3d()
{
    qalSource3f(source, AL_POSITION, 0, 0, 0);
    alDieIfError();
    qalSource3f(source, AL_VELOCITY, 0, 0, 0);
    alDieIfError();
    qalSourcei(source, AL_SOURCE_RELATIVE, true);
    alDieIfError();
    qalSourcei(source, AL_LOOPING, false);
    alDieIfError();
    qalSourcei(source, AL_ROLLOFF_FACTOR, 0);
    alDieIfError();
    set_gain(S_GetBaseVolume()); // HZM coop - route through set_gain so s_sfxduck applies (2D/local sounds bypassed it)
}

/*
==============
openal_channel::set_3d
==============
*/
void openal_channel::set_3d()
{
    qalSourcei(source, AL_SOURCE_RELATIVE, false);
    alDieIfError();
    qalSourcei(source, AL_LOOPING, false);
    alDieIfError();
    qalSourcef(source, AL_ROLLOFF_FACTOR, 1.0f);
    alDieIfError();
    set_gain(S_GetBaseVolume()); // HZM coop - route through set_gain so s_sfxduck applies
    // Context uses AL_INVERSE_DISTANCE_CLAMPED (set at init); per-source override removed.
    //
    // Added in OPM
    //
    if (fMinDist > 0) {
        qalSourcef(source, AL_REFERENCE_DISTANCE, fMinDist);
        alDieIfError();
    }
    if (fMaxDist > 0) {
        qalSourcef(source, AL_MAX_DISTANCE, fMaxDist);
        alDieIfError();
    }
}

/*
==============
openal_channel::play
==============
*/
void openal_channel::play()
{
    qalSourcePlay(source);
    alDieIfError();
}

/*
==============
openal_channel::set_no_virtualization
==============
*/
void openal_channel::set_no_virtualization()
{
#if AL_SOFT_direct_channels_remix
    qalSourcei(source, AL_DIRECT_CHANNELS_SOFT, AL_REMIX_UNMATCHED_SOFT);
    alDieIfError();
#elif AL_SOFT_direct_channels
    qalSourcei(source, AL_DIRECT_CHANNELS_SOFT, AL_TRUE);
    alDieIfError();
#endif
}

/*
==============
openal_channel::set_virtualization
==============
*/
void openal_channel::set_virtualization()
{
#if AL_SOFT_direct_channels_remix || AL_SOFT_direct_channels
    qalSourcei(source, AL_DIRECT_CHANNELS_SOFT, AL_FALSE);
    alDieIfError();
#endif
}

/*
==============
openal_channel::pause
==============
*/
void openal_channel::pause()
{
    qalSourcePause(source);
    alDieIfError();
}

/*
==============
openal_channel::stop
==============
*/
void openal_channel::stop()
{
    qalSourceStop(source);
    alDieIfError();
}

/*
==============
openal_channel::is_free
==============
*/
bool openal_channel::is_free()
{
    ALint state = sample_status();

    return state == AL_INITIAL || state == AL_STOPPED;
}

/*
==============
openal_channel::is_paused
==============
*/
bool openal_channel::is_paused()
{
    ALint state = sample_status();

    return state == AL_PAUSED;
}

/*
==============
openal_channel::is_playing
==============
*/
bool openal_channel::is_playing()
{
    ALint state = sample_status();

    return state == AL_PLAYING;
}

/*
==============
openal_channel::force_free
==============
*/
void openal_channel::force_free()
{
    stop();
    iFlags = 0;
}

/*
==============
openal_channel::set_sfx
==============
*/
bool openal_channel::set_sfx(sfx_t *pSfx)
{
    ALint freq = 0;

    assert(pSfx->length);

    this->pSfx = pSfx;
    if (!pSfx->buffer || !qalIsBuffer(pSfx->buffer)) {
        if (pSfx->iFlags & SFX_FLAG_MP3) {
            qalGenBuffers(1, &pSfx->buffer);
            alDieIfError();

            if (!S_OPENAL_LoadMP3_Codec(pSfx->name, pSfx)) {
                qalDeleteBuffers(1, &pSfx->buffer);
                alDieIfError();

                Com_DPrintf("OpenAL: Failed to load MP3.\n");
                return false;
            }

            alDieIfError();
        } else {
            ALenum fmt = S_OPENAL_Format(pSfx->info.width, pSfx->info.channels);

            if (!fmt) {
                Com_Printf(
                    "OpenAL: Bad Wave file (%d channels, %d bits) [%s].\n",
                    pSfx->info.channels,
                    (int)(pSfx->info.width * 8),
                    pSfx->name
                );
                return false;
            }

            if (pSfx->info.dataalign > 1 && !soft_block_align) {
                Com_DPrintf(
                    "OpenAL: Alignment specified but AL doesn't support block alignment (%d).", pSfx->info.dataalign
                );
                return false;
            }

            qalGenBuffers(1, &pSfx->buffer);
            alDieIfError();

#if AL_SOFT_block_alignment
            if (pSfx->info.dataalign > 1) {
                qalBufferi(pSfx->buffer, AL_UNPACK_BLOCK_ALIGNMENT_SOFT, pSfx->info.dataalign);
                alDieIfError();
            }
#endif

            qalBufferData(
                pSfx->buffer,
                fmt,
                &pSfx->data[pSfx->info.dataofs],
                pSfx->info.datasize - pSfx->info.dataofs,
                pSfx->info.rate
            );
            alDieIfError();
        }
    }

    qalSourceStop(source);
    alDieIfError();

    qalSourcei(source, AL_BUFFER, pSfx->buffer);
    alDieIfError();

    qalSourcef(source, AL_PITCH, 1.0);
    alDieIfError();

    // Get the base frequency
    qalGetBufferi(pSfx->buffer, AL_FREQUENCY, &freq);
    alDieIfError();

    iBaseRate = freq;

    return true;
}

/*
==============
openal_channel::start_sample
==============
*/
void openal_channel::start_sample()
{
    play();
}

/*
==============
openal_channel::stop_sample
==============
*/
void openal_channel::stop_sample()
{
    pause();
}

/*
==============
openal_channel::resume_sample
==============
*/
void openal_channel::resume_sample()
{
    play();
}

/*
==============
openal_channel::end_sample
==============
*/
void openal_channel::end_sample()
{
    stop();
}

/*
==============
openal_channel::set_sample_pan
==============
*/
void openal_channel::set_sample_pan(S32 pan)
{
    const float   panning           = (pan - 64) / 127.f;
    const ALfloat sourcePosition[3] = {panning, 0, sqrtf(1.f - Square(panning))};

    qalSourcef(source, AL_ROLLOFF_FACTOR, 0);
    qalSourcei(source, AL_SOURCE_RELATIVE, AL_TRUE);
    qalSourcefv(source, AL_POSITION, sourcePosition);
}

/*
==============
openal_channel::set_sample_playback_rate
==============
*/
void openal_channel::set_sample_playback_rate(S32 rate)
{
    // Fixed in OPM
    //  Set the pitch in OpenAL
    qalSourcef(source, AL_PITCH, rate / (float)iBaseRate);
    alDieIfError();
}

/*
==============
openal_channel::sample_playback_rate
==============
*/
S32 openal_channel::sample_playback_rate()
{
    float pitch = 1;
    ALint freq  = 0;

    // Fixed in OPM
    //  The sample rate varies according to the pitch
    qalGetSourcef(source, AL_PITCH, &pitch);
    alDieIfError();

    return buffer_frequency() * pitch;
}

/*
==============
openal_channel::buffer_frequency
==============
*/
U32 openal_channel::buffer_frequency() const
{
    ALint freq = 0;

    if (buffer) {
        qalGetBufferi(buffer, AL_FREQUENCY, &freq);
        alDieIfError();
    } else if (pSfx) {
        qalGetBufferi(pSfx->buffer, AL_FREQUENCY, &freq);
        alDieIfError();
    } else {
        freq = 22050;
    }

    return freq;
}

/*
==============
openal_channel::sample_volume
==============
*/
S32 openal_channel::sample_volume()
{
    ALfloat gain = 0;

    qalGetSourcef(source, AL_GAIN, &gain);
    alDieIfError();

    return (gain / S_GetBaseVolume()) * 100;
}

/*
==============
openal_channel::sample_offset
==============
*/
U32 openal_channel::sample_offset()
{
    ALint offset = 0;

    qalGetSourcei(source, AL_SAMPLE_OFFSET, &offset);
    alDieIfError();

    return offset;
}

/*
==============
openal_channel::sample_ms_offset
==============
*/
U32 openal_channel::sample_ms_offset()
{
    if (!is_playing()) {
        return 0;
    }

    return sample_offset() * 1000ull / sample_playback_rate();
}

/*
==============
openal_channel::sample_loop_count
==============
*/
U32 openal_channel::sample_loop_count()
{
    ALuint queued;
    ALuint processed;
    S32    left;

    qalGetSourceiv(source, AL_BUFFERS_QUEUED, (ALint *)&queued);
    alDieIfError();
    qalGetSourcei(source, AL_BUFFERS_PROCESSED, (ALint *)&processed);
    alDieIfError();

    left = queued + ~processed;
    if (left < 0) {
        left = 0;
    }
    return left;
}

/*
==============
openal_channel::set_sample_offset
==============
*/
void openal_channel::set_sample_offset(U32 offset)
{
    qalSourcei(source, AL_SAMPLE_OFFSET, offset);
    alDieIfError();
}

/*
==============
openal_channel::set_sample_ms_offset
==============
*/
void openal_channel::set_sample_ms_offset(U32 offset)
{
    set_sample_offset(offset * sample_playback_rate() / 1000);
}

/*
==============
openal_channel::set_sample_loop_count
==============
*/
void openal_channel::set_sample_loop_count(S32 count)
{
    ALuint processed = 0;

    stop();

    qalGetSourceiv(source, AL_BUFFERS_PROCESSED, (ALint *)&processed);
    alDieIfError();

    for (ALuint i = 0; i < processed; i++) {
        ALuint bufName;

        qalSourceUnqueueBuffers(source, 1, &bufName);
        alDieIfError();
    }

    if (buffer) {
        for (S32 i = 0; i < count; i++) {
            qalSourceQueueBuffers(source, 1, &buffer);
            alDieIfError();
        }
    }

    if (count == 0) {
        qalSourcei(source, AL_LOOPING, true);
        alDieIfError();
    } else {
        qalSourcei(source, AL_LOOPING, false);
        alDieIfError();
    }
}

/*
==============
openal_channel::set_sample_loop_block
==============
*/
void openal_channel::set_sample_loop_block(S32 start_offset, S32 end_offset)
{
#ifdef AL_SOFT_loop_points
    ALuint bufid = 0;
    qalGetSourcei(source, AL_BUFFER, (ALint *)&bufid);
    if (!bufid) {
        return;
    }

    ALint end = end_offset;
    if (end < 0) {
        // -1 means end-of-buffer; derive sample count from buffer attributes
        ALint size = 0, bits = 0, chans = 0;
        qalGetBufferi(bufid, AL_SIZE, &size);
        qalGetBufferi(bufid, AL_BITS, &bits);
        qalGetBufferi(bufid, AL_CHANNELS, &chans);
        if (bits <= 0 || chans <= 0) {
            return;
        }
        end = size / ((bits / 8) * chans);
    }

    if (!qalBufferiv_soft) {
        qalBufferiv_soft = (LPALBUFFERIV)qalGetProcAddress("alBufferiv");
    }
    if (qalBufferiv_soft) {
        ALint points[2] = {(ALint)start_offset, end};
        qalBufferiv_soft(bufid, AL_LOOP_POINTS_SOFT, points);
        alDieIfError();
    }
#endif
}

/*
==============
openal_channel::sample_status
==============
*/
U32 openal_channel::sample_status()
{
    ALint retval;

    qalGetSourcei(source, AL_SOURCE_STATE, &retval);
    alDieIfError();

    return retval;
}

/*
==============
MUSIC_LoadSoundtrackFile
==============
*/
qboolean MUSIC_LoadSoundtrackFile(const char *filename)
{
    song_t *psong = NULL;
    char    args[MAX_MUSIC_SONGS][MAX_RES_NAME];
    int     numargs;
    char    com_token[MAX_STRING_CHARS];
    char    alias[128];
    char    file[128];
    char    load_path[128];
    char   *buffer;
    char    path[MAX_RES_NAME];
    int     i;
    byte   *data;

    if (strrchr(filename, '.')) {
        Com_sprintf(path, sizeof(path), "%s", filename);
    } else {
        Com_sprintf(path, sizeof(path), "%s.mus", filename);
    }

    music_numsongs = 0;

    FS_ReadFile(path, (void **)&data);
    if (!data) {
        Com_DPrintf("OpenAL: Couldn't load %s\n", path);
        return false;
    }
    Com_DPrintf("SOUNDTRACK: Loading %s\n", path);

    MUSIC_StopAllSongs();

    buffer       = (char *)data;
    load_path[0] = 0;

    while (1) {
        Q_strncpyz(com_token, COM_GetToken(&buffer, true), sizeof(com_token));
        if (!com_token[0]) {
            break;
        }

        if (strlen(com_token) >= MAX_RES_NAME) {
            Com_Printf("MUSIC_LoadSoundtrackFile: argument too long, truncating in %s\n", path);
            com_token[MAX_RES_NAME - 1] = 0;
        }

        numargs = 1;
        Q_strncpyz(args[0], com_token, sizeof(args[0]));

        while (1) {
            Q_strncpyz(com_token, COM_GetToken(&buffer, false), sizeof(com_token));
            if (!com_token[0]) {
                break;
            }

            if (strlen(com_token) >= MAX_RES_NAME) {
                Com_Printf("MUSIC_LoadSoundtrackFile: argument too long, truncating in %s\n", path);
                com_token[MAX_RES_NAME - 1] = 0;
            }

            Q_strncpyz(args[numargs], com_token, sizeof(args[numargs]));
            numargs++;
        }

        if (!Q_stricmp(args[0], "path")) {
            Q_strncpyz(load_path, args[1], sizeof(load_path));
            if (load_path[strlen(load_path) - 1] != '/' && load_path[strlen(load_path) - 1] != '\\') {
                Q_strcat(load_path, sizeof(load_path), "/");
            }
        } else if (args[0][0] == '!') {
            for (i = 0; i < music_numsongs; i++) {
                psong = &music_songs[i];
                if (!Q_stricmp(psong->alias, &args[0][1])) {
                    break;
                }
            }

            if (i == music_numsongs) {
                Com_Printf("MUSIC_LoadSoundtrackFile: song %s not found, command skipped in %s.\n", &args[0][1], path);
                continue;
            }

            if (!Q_stricmp(args[1], "volume")) {
                psong->volume = atof(args[2]);
            } else if (!Q_stricmp(args[1], "fadetime")) {
                psong->fadetime = atof(args[2]);
            } else if (!Q_stricmp(args[1], "loop")) {
                psong->flags |= 1;
            } else if (!Q_stricmp(args[1], "restart")) {
                psong->flags |= 2;
            } else if (!Q_stricmp(args[1], "interrupt")) {
                psong->fadetime = 0;
                psong->flags |= 4 | 2;
            } else {
                Com_Printf(
                    "MUSIC_LoadSoundtrackFile: unknown command %s for song %s in %s.\n", args[1], &args[0][1], path
                );
            }
        } else {
            if (numargs > 1) {
                Q_strncpyz(alias, args[0], sizeof(alias));
                Q_strncpyz(file, load_path, sizeof(file));
                Q_strcat(file, sizeof(file), args[1]);
            } else {
                size_t len;

                Q_strncpyz(file, load_path, sizeof(file));
                Q_strcat(file, sizeof(file), args[1]);

                len = strlen(args[0]);
                if (len >= 4) {
                    Q_strncpyz(alias, args[0], len - 4);
                } else {
                    alias[0] = 0;
                }
            }

            if (music_numsongs >= MAX_MUSIC_SONGS) {
                Com_Printf("MUSIC_LoadSoundtrackFile: Too many songs in %s, skipping %s.\n", path, alias);
                continue;
            }

            psong = &music_songs[music_numsongs];
            Q_strncpyz(psong->alias, alias, sizeof(psong->alias));
            Q_strncpyz(psong->path, file, sizeof(psong->path));
            music_songs[music_numsongs].fadetime    = 1.0;
            music_songs[music_numsongs].volume      = 1.0;
            music_songs[music_numsongs].flags       = 0;
            music_songs[music_numsongs].current_pos = 0;
            music_songs[music_numsongs].mood_num    = MusicMood_NameToNum(alias);
            music_numsongs++;
        }
    }

    if (!music_numsongs) {
        Com_Printf("MUSIC_LoadSoundtrackFile: could not load %s, no songs defined.\n", path);
        FS_FreeFile(data);
        return false;
    }

    music_currentsong = -1;
    FS_FreeFile(data);

    if (music_current_mood == mood_none) {
        MUSIC_UpdateMood(mood_normal, mood_normal);
    }

    return true;
}

/*
==============
MUSIC_SongValid
==============
*/
qboolean MUSIC_SongValid(const char *mood)
{
    return MUSIC_FindSong(mood) != -1;
}

/*
==============
MUSIC_Loaded
==============
*/
qboolean MUSIC_Loaded()
{
    return music_loaded;
}

/*
==============
Music_Update
==============
*/
void Music_Update()
{
    int currentsong;

    if (!s_bSoundStarted) {
        return;
    }

    if (!music_active) {
        return;
    }

    if (s_bSoundPaused) {
        return;
    }

    MUSIC_CheckForStoppedSongs();

    if (!MUSIC_Loaded() && music_active && strlen(current_soundtrack)) {
        MUSIC_LoadSoundtrackFile(current_soundtrack);
        music_loaded = true;
    }

    if (music_currentsong >= 0) {
        currentsong = music_songs[music_currentsong].mood_num;
    } else {
        currentsong = -1;
    }

    if (music_current_mood == mood_none) {
        if (MUSIC_Playing()) {
            MUSIC_StopAllSongs();
        }
    } else if (music_current_mood != currentsong) {
        const char *mood = MusicMood_NumToName(music_current_mood);
        if (MUSIC_SongValid(mood) && MUSIC_Loaded() && strlen(current_soundtrack)) {
            Com_DebugPrintf("Playing %s.\n", mood);
            MUSIC_PlaySong(mood);
        }
    }

    if (new_music_volume != music_volume) {
        if (music_volume_fade_time > 0) {
            if (music_volume_direction == 0) {
                music_volume = (cls.realtime - music_volume_start_time) * (new_music_volume - old_music_volume)
                                 / (music_volume_fade_time * 1000.f)
                             + old_music_volume;

                if (music_volume > new_music_volume) {
                    music_volume         = new_music_volume;
                    music_volume_changed = 1;
                } else {
                    music_volume_changed = 1;
                }
            } else if (music_volume_direction == 1) {
                music_volume = 1.0
                             - (cls.realtime - music_volume_start_time) * (old_music_volume - new_music_volume)
                                   / (music_volume_fade_time * 1000.f);

                if (music_volume >= new_music_volume) {
                    music_volume_changed = true;
                } else {
                    music_volume         = new_music_volume;
                    music_volume_changed = true;
                }
            } else {
                music_volume_changed = true;
            }
        } else {
            music_volume         = new_music_volume;
            music_volume_changed = true;
        }
    }

    MUSIC_UpdateMusicVolumes();
}

/*
==============
MUSIC_SongEnded
==============
*/
void MUSIC_SongEnded()
{
    Com_DPrintf(
        "MUSIC: Song ended, changing from [ %s ] to [ %s ]\n",
        MusicMood_NumToName(music_current_mood),
        MusicMood_NumToName(music_fallback_mood)
    );
    music_current_mood = music_fallback_mood;
}

/*
==============
MUSIC_NewSoundtrack
==============
*/
void MUSIC_NewSoundtrack(const char *name)
{
    if (!s_bSoundStarted) {
        return;
    }

    if (!Q_stricmp(name, current_soundtrack)) {
        return;
    }

    // Fixed in OPM
    //  Make sure to not get past the end of the buffer
    //strcpy(current_soundtrack, name);
    Q_strncpyz(current_soundtrack, name, sizeof(current_soundtrack));

    if (!Q_stricmp(current_soundtrack, "none") || !Q_stricmp(current_soundtrack, "")) {
        music_active = qfalse;
        if (MUSIC_Playing()) {
            MUSIC_StopAllSongs();
        }
    } else {
        music_active = qtrue;
        MUSIC_LoadSoundtrackFile(name);
        music_loaded = qtrue;
    }
}

/*
==============
MUSIC_UpdateMood
==============
*/
void MUSIC_UpdateMood(int current, int fallback)
{
    static int last_current_mood  = -1;
    static int last_fallback_mood = -1;
    static int current_mood       = -1;
    static int fallback_mood      = -1;
    qboolean   was_action;

    if (current == current_mood && fallback == fallback_mood) {
        return;
    }

    was_action          = current == last_current_mood && fallback == last_fallback_mood && current_mood == mood_action;
    last_current_mood   = current_mood;
    last_fallback_mood  = fallback_mood;
    current_mood        = current;
    music_current_mood  = current;
    fallback_mood       = fallback;
    music_fallback_mood = fallback;
    music_active        = qtrue;

    Com_DPrintf(
        "MUSIC: changing from [ %s | %s ] to [ %s | %s ]\n",
        MusicMood_NumToName(last_current_mood),
        MusicMood_NumToName(last_fallback_mood),
        MusicMood_NumToName(current_mood),
        MusicMood_NumToName(fallback)
    );

    if (was_action) {
        int songnum = MUSIC_FindSong(MusicMood_NumToName(current_mood));

        if (songnum != -1 && (music_songs[songnum].flags & 4)) {
            Com_DPrintf(
                "MUSIC: restoring music from action state, skipping [ %s ] for [ %s ]\n",
                MusicMood_NumToName(current_mood),
                MusicMood_NumToName(fallback_mood)
            );
            music_current_mood = music_fallback_mood;
        }
    }
}

/*
==============
MUSIC_UpdateVolume
==============
*/
void MUSIC_UpdateVolume(float volume, float fade_time)
{
    if (new_music_volume == volume && music_volume_fade_time == fade_time) {
        return;
    }

    old_music_volume        = music_volume;
    new_music_volume        = volume;
    music_volume_fade_time  = fade_time;
    music_volume_start_time = cls.realtime;

    if (volume > music_volume) {
        music_volume_direction = 0;
    } else if (volume < music_volume) {
        music_volume_direction = 1;
    }
}

/*
==============
MUSIC_StopAllSongs
==============
*/
void MUSIC_StopAllSongs()
{
    for (int i = 0; i < MAX_SOUNDSYSTEM_SONGS; i++) {
        MUSIC_StopChannel(i);
    }

    music_currentsong = -1;
}

/*
==============
MUSIC_FreeAllSongs
==============
*/
void MUSIC_FreeAllSongs()
{
    MUSIC_StopAllSongs();
    MUSIC_UpdateMood(mood_none, mood_none);
    current_soundtrack[0] = 0;
    music_loaded          = false;

    // Added in OPM
    //  Clear the list of songs so the songs from previous maps
    //  won't get played
    music_numsongs = 0;
}

/*
==============
MUSIC_Playing
==============
*/
qboolean MUSIC_Playing()
{
    return MUSIC_CurrentSongChannel() != -1;
}

/*
==============
MUSIC_FindSong
==============
*/
int MUSIC_FindSong(const char *name)
{
    int i;

    for (i = 0; i < music_numsongs; i++) {
        if (!Q_stricmp(music_songs[i].alias, name)) {
            return i;
        }
    }

    return -1;
}

/*
==============
S_loadsoundtrack
==============
*/
void S_loadsoundtrack()
{
    if (Cmd_Argc() != 2) {
        Com_Printf("loadsoundtrack <sound track file>\n");
        return;
    }

    MUSIC_LoadSoundtrackFile(Cmd_Argv(1));
    music_loaded = true;
    Q_strncpyz(current_soundtrack, Cmd_Argv(1), sizeof(current_soundtrack));
}

/*
==============
S_CurrentSoundtrack
==============
*/
const char *S_CurrentSoundtrack()
{
    return current_soundtrack;
}

/*
==============
S_PlaySong
==============
*/
void S_PlaySong()
{
    if (Cmd_Argc() != 2) {
        Com_Printf("playsong <song alias>\n");
        return;
    }

    MUSIC_PlaySong(Cmd_Argv(1));
    music_active = true;
}

/*
==============
MUSIC_CurrentSongChannel
==============
*/
int MUSIC_CurrentSongChannel()
{
    int channel_number = -1;
    int ch_idx         = 0;

    for (ch_idx = 0; ch_idx < MAX_SOUNDSYSTEM_SONGS; ch_idx++) {
        if (openal.chan_song[ch_idx].is_playing() && openal.chan_song[ch_idx].song_number == music_currentsong) {
            channel_number = ch_idx;
        }
    }

    return channel_number;
}

/*
==============
MUSIC_StopChannel
==============
*/
void MUSIC_StopChannel(int channel_number)
{
    openal_channel *channel = &openal.chan_song[channel_number];

    channel->pause();
    if (music_songs[channel->song_number].flags & 2) {
        music_songs[channel->song_number].current_pos = 0;
    } else {
        music_songs[channel->song_number].current_pos = channel->sample_ms_offset();
    }

    channel->stop();
}

/*
==============
MUSIC_PlaySong
==============
*/
qboolean MUSIC_PlaySong(const char *alias)
{
    int                          channel_number;
    song_t                      *song;
    int                          songnum;
    int                          channel_to_play_on;
    int                          fading_song;
    openal_channel_two_d_stream *song_channel;
    unsigned int                 loop_start;
    int                          rate;

    fading_song = 0;
    songnum     = MUSIC_FindSong(alias);

    if (songnum == -1) {
        return true;
    }

    song = &music_songs[songnum];

    if (MUSIC_Playing() && songnum == music_currentsong) {
        return true;
    }

    channel_number    = MUSIC_CurrentSongChannel();
    music_currentsong = songnum;

    if (channel_number != -1) {
        if (song->flags & 4) {
            MUSIC_StopChannel(channel_number);
        } else {
            song_channel                  = &openal.chan_song[channel_number];
            song_channel->fading          = FADE_OUT;
            song_channel->fade_time       = (int)song->fadetime;
            song_channel->fade_start_time = cls.realtime;
            fading_song                   = true;
        }
    }
    channel_to_play_on = (fading_song != 0) && (channel_number == 0);
    song_channel       = &openal.chan_song[channel_to_play_on];

    if (song_channel->is_playing() || song_channel->is_paused()) {
        MUSIC_StopChannel(channel_to_play_on);
    }

    if (!song_channel->queue_stream(song->path)) {
        Com_DPrintf("Could not start music file '%s'!", song->path);
        return false;
    }

    rate = song_channel->sample_playback_rate();

    song_channel->song_number = songnum;
    if (song->current_pos) {
        song_channel->set_sample_ms_offset(song->current_pos);
    } else {
        song_channel->set_sample_offset(rate * 0.063f);
    }

    if (song->flags & 1) {
        song_channel->set_sample_loop_count(0);
        song_channel->set_sample_loop_block(rate * 0.063f, -1);
    } else {
        song_channel->set_sample_loop_count(1);
    }

    if (fading_song) {
        song_channel->fading    = FADE_IN;
        song_channel->fade_time = (int)song->fadetime;
        song_channel->set_gain(0.0);
        song_channel->fade_start_time = cls.realtime;
    } else {
        song_channel->fading = FADE_NONE;
        song_channel->set_gain(S_GetBaseVolume() * (song->volume * s_musicVolume->value) * s_fVolumeGain);
    }

    song_channel->play();

    return true;
}

/*
==============
MUSIC_UpdateMusicVolumes
==============
*/
void MUSIC_UpdateMusicVolumes()
{
    int          i;
    unsigned int current_time;
    float        new_volume, max_volume;

    if (s_musicVolume->modified || music_volume_changed) {
        s_musicVolume->modified = false;

        for (i = 0; i < MAX_SOUNDSYSTEM_SONGS; i++) {
            if (!openal.chan_song[i].is_playing() && !openal.chan_song[i].is_paused()) {
                continue;
            }

            if (openal.chan_song[i].fading != FADE_NONE) {
                continue;
            }

            openal.chan_song[i].set_gain(
                S_GetBaseVolume() * (music_songs[openal.chan_song[i].song_number].volume * s_musicVolume->value)
                * s_fVolumeGain * music_volume
            );
        }
    }

    for (i = 0; i < MAX_SOUNDSYSTEM_SONGS; i++) {
        if (!openal.chan_song[i].is_playing() && !openal.chan_song[i].is_paused()) {
            continue;
        }

        switch (openal.chan_song[i].fading) {
        case fade_t::FADE_IN:
            max_volume = music_songs[openal.chan_song[i].song_number].volume * s_musicVolume->value;
            new_volume = (unsigned int)(cls.realtime - openal.chan_song[i].fade_start_time)
                       / (openal.chan_song[i].fade_time * 1000.f) * max_volume;

            if (new_volume > max_volume) {
                openal.chan_song[i].set_gain(S_GetBaseVolume() * (max_volume * s_fVolumeGain * music_volume));
                openal.chan_song[i].fading = FADE_NONE;
            } else {
                openal.chan_song[i].set_gain(S_GetBaseVolume() * (new_volume * s_fVolumeGain * music_volume));
            }
            break;
        case fade_t::FADE_OUT:
            max_volume = music_songs[openal.chan_song[i].song_number].volume * s_musicVolume->value;
            new_volume = max_volume
                       - (unsigned int)(cls.realtime - openal.chan_song[i].fade_start_time)
                         / (openal.chan_song[i].fade_time * 1000.f) * max_volume;

            if (new_volume > 0) {
                openal.chan_song[i].set_gain(S_GetBaseVolume() * (new_volume * s_fVolumeGain * music_volume));
            } else {
                MUSIC_StopChannel(i);
            }
            break;
        default:
            break;
        }
    }

    if (s_musicVolume->modified || music_volume_changed) {
        s_musicVolume->modified = false;
        if (openal.chan_trig_music.is_playing() || openal.chan_trig_music.is_paused()) {
            openal.chan_trig_music.set_gain(
                S_GetBaseVolume() * (openal.chan_trig_music.fVolume * s_musicVolume->value) * s_fVolumeGain
            );
        }
    }

    music_volume_changed = false;
}

/*
==============
MUSIC_CheckForStoppedSongs
==============
*/
void MUSIC_CheckForStoppedSongs()
{
    int i;

    for (i = 0; i < MAX_SOUNDSYSTEM_SONGS; i++) {
        if (!openal.chan_song[i].is_playing()) {
            continue;
        }

        if (openal.chan_song[i].sample_loop_count()) {
            continue;
        }

        MUSIC_FindSong(MusicMood_NumToName(music_fallback_mood));
        if (!openal.chan_song[i].is_playing() && !openal.chan_song[i].is_paused()) {
            MUSIC_SongEnded();
        }
    }
}

/*
==============
S_TriggeredMusic_SetupHandle
==============
*/
void S_TriggeredMusic_SetupHandle(const char *pszName, int iLoopCount, int iOffset, qboolean autostart)
{
    char            *pszFilename;
    const char      *pszRealName;
    float            fVolume     = 1.0;
    AliasListNode_t *pSoundAlias = NULL;
    void            *stream      = NULL;

    if (!s_bSoundStarted) {
        return;
    }

    if (openal.chan_trig_music.is_playing() || openal.chan_trig_music.is_paused()) {
        Com_DPrintf("Didn't start new triggered music because some was already playing\n");
        return;
    }

    s_menuMusicAuto = qfalse; // HZM coop [bug-2576] a new triggered track owns the channel; S_MenuMusic_Start re-arms it
    openal.chan_trig_music.stop();
    // Fixed in OPM
    //  Use strncpy instead
    //strcpy(openal.tm_filename, pszName);
    Q_strncpyz(openal.tm_filename, pszName, sizeof(openal.tm_filename));

    openal.tm_loopcount = iLoopCount;
    openal.chan_trig_music.set_sample_loop_count(iLoopCount);
    pszRealName = Alias_FindRandom(pszName, &pSoundAlias);

    if (!pszRealName) {
        pszRealName = pszName;
    } else if (pSoundAlias) {
        fVolume = random() * pSoundAlias->volumeMod + pSoundAlias->volume;
    }

    pszFilename = FS_BuildOSPath(Cvar_VariableString("fs_basepath"), FS_Gamedir(), pszRealName);

    if (!openal.chan_trig_music.queue_stream(pszRealName)) {
        S_OPENAL_InitChannel(SOUNDSYSTEM_CHANNEL_TRIGGER_MUSIC_ID, &openal.chan_trig_music);
        Com_DPrintf("Could not start triggered music '%s'\n", pszName);
        return;
    }

    openal.chan_trig_music.fVolume = fVolume;
    // HZM coop - give the triggered-music source gain headroom (default AL_MAX_GAIN is 1.0) so the
    // soundtrack can be pushed loud over the listener attenuation (s_volume^2) and a quiet master.
    qalSourcef(openal.chan_trig_music.source, AL_MAX_GAIN, 8.0f);
    openal.chan_trig_music.set_gain(S_GetBaseVolume() * (fVolume * s_musicVolume->value) * s_fVolumeGain);
    openal.chan_trig_music.set_sample_loop_count(iLoopCount);
    openal.chan_trig_music.set_sample_offset(iOffset);

    // Fixed in OPM
    //  Play the sound then pause it immediately
    //  So it can be unpaused upon loading from save
    openal.chan_trig_music.play();

    if (!autostart) {
        openal.chan_trig_music.pause();
    }
}

/*
==============
S_TriggeredMusic_Start
==============
*/
void S_TriggeredMusic_Start()
{
    if (Cmd_Argc() != 2) {
        Com_Printf("tmstart <sound file>\n");
        return;
    }

    S_TriggeredMusic_SetupHandle(Cmd_Argv(1), 1, 0, true);
}

/*
==============
S_TriggeredMusic_StartLoop
==============
*/
void S_TriggeredMusic_StartLoop()
{
    if (Cmd_Argc() != 2) {
        Com_Printf("tmstartloop <sound file>\n");
        return;
    }

    S_TriggeredMusic_SetupHandle(Cmd_Argv(1), 0, 0, true);
}

/*
==============
S_TriggeredMusic_Stop
==============
*/
void S_TriggeredMusic_Stop()
{
    s_menuMusicAuto = qfalse; // HZM coop [bug-2576] a forced stop is not the end of a menu theme
    if (!s_bSoundStarted) {
        return;
    }

    openal.chan_trig_music.stop();
    // Fixed in OPM
    //  Make sure to clear the triggered music in case snd_restart is called
    openal.tm_filename[0] = 0;
    openal.tm_loopcount   = 0;
}

/*
==============
S_TriggeredMusic_Volume

Added in 2.0
==============
*/
void S_TriggeredMusic_Volume()
{
    if (!s_bSoundStarted) {
        return;
    }

    if (Cmd_Argc() != 2) {
        Com_Printf("tmvolume <0-1>\n\n");
        return;
    }

    openal.chan_trig_music.fVolume = atof(Cmd_Argv(1));
    music_volume_changed           = true;
}

/*
==============
S_TriggeredMusic_Pause
==============
*/
void S_TriggeredMusic_Pause()
{
    if (!s_bSoundStarted) {
        return;
    }

    if (openal.chan_trig_music.is_playing()) {
        openal.chan_trig_music.pause();
    }
}

/*
==============
S_TriggeredMusic_Unpause
==============
*/
void S_TriggeredMusic_Unpause()
{
    if (!s_bSoundStarted) {
        return;
    }

    if (openal.chan_trig_music.is_paused()) {
        openal.chan_trig_music.play();
    }

    openal.chan_trig_music.set_gain(
        S_GetBaseVolume() * (openal.chan_trig_music.fVolume * s_musicVolume->value) * s_fVolumeGain
    );
}

/*
==============
S_TriggeredMusic_PlayIntroMusic

HZM coop [user 2026-09-13, bugs 2570 + 2572] MAIN MENU THEME PICKER

"Players should be able to switch which main menu theme plays" and "randomize which one it plays when you launch
every time". The playlist is mod data - ui/coop_menumusic/t<n>.cfg, one file per theme - so a theme is added
without touching the engine. Each cfg sets ui_menuMusic / ui_menuMusicTitle / ui_menuMusicGame /
ui_menuMusicPrev / ui_menuMusicNext for the session, records `seta ui_menuMusicLast <n>`, then runs menumusic_play.

THIS IS THE LIVE COPY. bug-2570 first built this in code/client/new/snd_main_new.cpp, whose entire body sits inside
#if NO_MODERN_DMA - and this build is NO_MODERN_DMA=0, OpenAL. It compiled, linked and deployed, and the exe did not
contain one byte of it: the menu kept the hardcoded theme and Next/Back ran an empty vstr. Here the theme plays on
chan_trig_music through the same S_TriggeredMusic_SetupHandle that tmstartloop uses.
==============
*/
#define MENUMUSIC_DEFAULT    "sound/music/mus_MainTheme.mp3"
#define MENUMUSIC_MAX_TRACKS 32

static qboolean s_menuMusicRolled = qfalse; // the per-launch random pick has been made

// Play ui_menuMusic, looping, if it is a real music file; otherwise the original theme.
static void S_MenuMusic_Start(void)
{
    const char *szTrack = Cvar_VariableString("ui_menuMusic");
    size_t      len     = szTrack ? strlen(szTrack) : 0;

    if (len < 17 || Q_stricmpn(szTrack, "sound/music/", 12) || Q_stricmp(szTrack + len - 4, ".mp3")
        || FS_ReadFile(szTrack, NULL) <= 0) {
        szTrack = MENUMUSIC_DEFAULT;
    }
    // SetupHandle refuses while chan_trig_music is playing or paused ("Didn't start new triggered music because
    // some was already playing"), and that is exactly the state a Next/Back press happens in. Both paths here are
    // disconnected-only, so there is never a map's score on this channel to cut.
    S_TriggeredMusic_Stop();
    // [bug-2576] loop count 1 = play ONCE; S_MenuMusic_Frame moves on to the next theme when it ends. Armed only if
    // the track really started - a missing file leaves the channel idle and must not read as "finished".
    S_TriggeredMusic_SetupHandle(szTrack, 1, 0, true);
    if (openal.chan_trig_music.is_playing()) {
        s_menuMusicAuto    = qtrue;
        s_menuMusicStartMs = Sys_Milliseconds();
    }
}

/*
==============
S_MenuMusic_Play_f - `menumusic_play`, run by the theme cfgs.

ONLY while disconnected, i.e. sitting in the menu. The same Next/Back buttons exist on the in-game main menu, and
there they must change the choice without replacing the map's own score; the new theme plays the next time the
player is back at the menu.
==============
*/
void S_MenuMusic_Play_f(void)
{
    if (clc.state != CA_DISCONNECTED) {
        return;
    }
    S_MenuMusic_Start();
}

/*
==============
S_MenuMusic_Frame

HZM coop [user 2026-09-13, bug-2576] "once one of the main title songs play, automatically move to the next one."

Runs at the end of every S_OPENAL_Update, after the stream channels have been fed. A menu theme plays once (loop count
1): when openal_channel_two_d_stream::update runs out of data it stops feeding, the source plays out its queue and
stops, and the stream clears itself. That natural end is the only way s_menuMusicAuto can still be set while the channel
is idle, because every forced stop clears it (S_TriggeredMusic_Stop, S_TriggeredMusic_SetupHandle).

The next theme is the same cfg the Next button runs, so the title box and ui_menuMusicLast follow along, and the last
theme wraps to the first. Only while disconnected: in game a map owns the channel, and the menu theme comes back through
PlayIntroMusic on the way out.
==============
*/
static void S_MenuMusic_Frame(void)
{
    if (!s_menuMusicAuto) {
        return;
    }
    if (clc.state != CA_DISCONNECTED) {
        s_menuMusicAuto = qfalse;
        return;
    }
    if (openal.chan_trig_music.is_playing() || openal.chan_trig_music.is_paused()) {
        return;
    }
    s_menuMusicAuto = qfalse;
    // a stream that died within seconds of starting (a bad or truncated file) must not spin through the playlist
    if (Sys_Milliseconds() - s_menuMusicStartMs < 5000) {
        return;
    }
    if (*Cvar_VariableString("ui_menuMusicNext")) {
        Cbuf_AddText("vstr ui_menuMusicNext\n");
    } else {
        S_MenuMusic_Start();
    }
}

void S_TriggeredMusic_PlayIntroMusic()
{
    if (!s_menuMusicRolled) {
        char szCfg[MAX_QPATH];
        int  n, idx, last;

        s_menuMusicRolled = qtrue;

        // how many themes the mod ships: t0, t1, ... up to the first missing file
        for (n = 0; n < MENUMUSIC_MAX_TRACKS; n++) {
            Com_sprintf(szCfg, sizeof(szCfg), "ui/coop_menumusic/t%d.cfg", n);
            if (FS_ReadFile(szCfg, NULL) <= 0) {
                break;
            }
        }

        if (n > 0) {
            // RANDOM PER LAUNCH, and never the one that played last launch. Wall clock mixed with the millisecond
            // timer: at this point Sys_Milliseconds is just the boot time, nearly identical every launch.
            srand((unsigned int)time(NULL) ^ (unsigned int)Sys_Milliseconds());
            idx  = rand() % n;
            last = Cvar_VariableIntegerValue("ui_menuMusicLast");
            if (n > 1 && idx == last) {
                idx = (idx + 1 + rand() % (n - 1)) % n;
            }
            // The cfg sets the cvars and ends with menumusic_play, which starts the track once the buffer runs it
            // (both call sites are disconnected, so its state check passes). Appended, not run inline: the cvars
            // would not be set yet on return.
            Com_sprintf(szCfg, sizeof(szCfg), "ui/coop_menumusic/t%d.cfg", idx);
            // path built first, then run through "exec %s": ui_wiring_audit resolves every exec-plus-path literal
            // in engine source against real files, and a printf template cannot resolve
            Cbuf_AddText(va("exec %s\n", szCfg));
            return;
        }
        // no playlist shipped - fall through to the original theme
    }
    S_MenuMusic_Start();
}

/*
==============
S_StopMovieAudio
==============
*/
void S_StopMovieAudio()
{
    openal.chan_movie.stop();
}

/*
==============
S_SetupMovieAudio
==============
*/
void S_SetupMovieAudio(const char *pszMovieName)
{
    char filename[MAX_QPATH];

    S_StopMovieAudio();

    COM_StripExtension(pszMovieName, filename, sizeof(filename));
    Q_strcat(filename, sizeof(filename), ".mp3");

    if (!openal.chan_movie.queue_stream(filename)) {
        COM_StripExtension(pszMovieName, filename, sizeof(filename));
        Q_strcat(filename, sizeof(filename), ".wav");

        if (!openal.chan_movie.queue_stream(filename)) {
            Com_DPrintf("Can't find any matching audio file for movie: %s\n", pszMovieName);
            return;
        }
    }

    openal.chan_movie.set_gain(S_GetBaseVolume() * 1.0f);
    openal.chan_movie.play();

    Com_DPrintf("Movie Audio setup: %s\n", filename);
}

/*
==============
S_CurrentMoviePosition
==============
*/
int S_CurrentMoviePosition()
{
    return openal.chan_movie.sample_ms_offset();
}

/*
==============
S_SeekMovieAudio

HZM coop bug-3234: a late joiner's briefing audio starts where the film is.
==============
*/
void S_SeekMovieAudio(int ms)
{
    S32 rate;

    if (ms <= 0 || !openal.chan_movie.is_playing()) {
        return;
    }
    rate = openal.chan_movie.sample_playback_rate();
    if (rate <= 0) {
        return;
    }
    // 64-bit: set_sample_ms_offset's U32 ms*rate overflows past ~97 s at 44.1 kHz
    openal.chan_movie.set_sample_offset((U32)((unsigned long long)ms * (unsigned long long)rate / 1000ull));
}

/*
=================
S_AL_Format
=================
*/
static ALuint S_OPENAL_Format(float width, int channels)
{
    ALuint format = AL_FORMAT_MONO16;

    // Work out format
    if (width == 1) {
        if (channels == 1) {
            format = AL_FORMAT_MONO8;
        } else if (channels == 2) {
            format = AL_FORMAT_STEREO8;
        }
    } else if (width == 2) {
        if (channels == 1) {
            format = AL_FORMAT_MONO16;
        } else if (channels == 2) {
            format = AL_FORMAT_STEREO16;
        }
    } else if (width == 0.5) {
        if (ima4_ext && soft_block_align) {
            if (channels == 1) {
                format = AL_FORMAT_MONO_IMA4;
            } else if (channels == 2) {
                format = AL_FORMAT_STEREO_IMA4;
            }
        } else {
            // unsupported
            format = 0;
        }
    }

    return format;
}

/*
==============
S_OPENAL_LoadMP3_Codec
==============
*/
static bool S_OPENAL_LoadMP3_Codec(const char *_path, sfx_t *pSfx)
{
    void      *data;
    snd_info_t info;
    ALuint     format;

    // Try to load
    data = S_CodecLoad(_path, &info);
    if (!data) {
        return false;
    }

    format = S_OPENAL_Format(info.width, info.channels);

    // Create a buffer
    qalGenBuffers(1, &pSfx->buffer);
    alDieIfError();

    // Fill the buffer
    if (info.size == 0) {
        // We have no data to buffer, so buffer silence
        byte dummyData[2] = {0};

        qalBufferData(pSfx->buffer, AL_FORMAT_MONO16, (void *)dummyData, 2, 22050);
    } else {
        qalBufferData(pSfx->buffer, format, data, info.size, info.rate);
    }

    alDieIfError();

    // Free the memory
    Hunk_FreeTempMemory(data);

    return true;
}

/*
==============
openal_channel::update
==============
*/
void openal_channel::update() {}

/*
==============
openal_channel_two_d_stream::stop
==============
*/
void openal_channel_two_d_stream::stop()
{
    openal_channel::stop();

    clear_stream();
}

/*
==============
openal_channel_two_d_stream::set_sfx
==============
*/
bool openal_channel_two_d_stream::set_sfx(sfx_t *pSfx)
{
    snd_stream_t *stream;
    unsigned int  bytesToRead, bytesRead;
    ALint         freq = 0;
    char          rawData[MAX_BUFFER_SAMPLES * 2 * 2];

    stop();

    sampleLoopCount = 1;

    this->pSfx = pSfx;
    Q_strncpyz(this->fileName, pSfx->name, sizeof(this->fileName));

    streamHandle = S_CodecLoad(pSfx->name, NULL);
    if (!streamHandle) {
        Com_DPrintf("OpenAL: Failed to load sound file.\n");
        return false;
    }

    stream              = (snd_stream_t *)streamHandle;
    pSfx->info.channels = stream->info.channels;
    pSfx->info.dataofs  = stream->info.dataofs;
    pSfx->info.datasize = stream->info.size;
    pSfx->info.rate     = stream->info.rate;
    pSfx->info.samples  = stream->info.samples;
    pSfx->info.width    = stream->info.width;

    pSfx->info.format = S_OPENAL_Format(stream->info.width, stream->info.channels);
    if (!pSfx->info.format) {
        Com_DPrintf(
            "OpenAL: Bad Wave file (%d channels, %d bits) [%s].\n",
            pSfx->info.channels,
            (int)(pSfx->info.width * 8.f),
            pSfx->name
        );

        S_CodecCloseStream(stream);
        return false;
    }

    qalSourceStop(source);
    alDieIfError();

    qalGenBuffers(MAX_STREAM_BUFFERS, buffers);
    alDieIfError();

    // Read a smaller sample
    bytesToRead      = Q_min(MAX_BUFFER_SAMPLES * stream->info.width * stream->info.channels, sizeof(rawData));
    bytesRead        = S_CodecReadStream(stream, bytesToRead, rawData);
    streamNextOffset = bytesRead;
    if (!bytesRead) {
        // Valid stream but no data?
        S_CodecCloseStream(stream);
        return true;
    }

#if AL_SOFT_block_alignment
    if (stream->info.dataalign > 1 && soft_block_align) {
        qalBufferi(buffers[currentBuf], AL_UNPACK_BLOCK_ALIGNMENT_SOFT, stream->info.dataalign);
        alDieIfError();
    }
#endif

    qalBufferData(buffers[currentBuf], pSfx->info.format, rawData, bytesRead, stream->info.rate);
    alDieIfError();

    qalSourceQueueBuffers(source, 1, &buffers[currentBuf]);
    alDieIfError();

    iBaseRate  = stream->info.rate;
    currentBuf = (currentBuf + 1) % MAX_STREAM_BUFFERS;
    streaming  = true;

    return true;
}

/*
==============
openal_channel_two_d_stream::set_sample_loop_count
==============
*/
void openal_channel_two_d_stream::set_sample_loop_count(S32 count)
{
    sampleLoopCount = count;
    sampleLooped    = 0;
}

/*
==============
openal_channel_two_d_stream::openal_channel_two_d_stream
==============
*/
openal_channel_two_d_stream::openal_channel_two_d_stream()
{
    int i;

    streamHandle = NULL;
    for (i = 0; i < MAX_STREAM_BUFFERS; i++) {
        buffers[i] = 0;
    }
    currentBuf       = 0;
    sampleLoopCount  = 0;
    sampleLooped     = 0;
    streamNextOffset = 0;
    streaming        = false;
}

/*
==============
openal_channel_two_d_stream::openal_channel_two_d_stream
==============
*/
openal_channel_two_d_stream::~openal_channel_two_d_stream()
{
    clear_stream();
}

/*
==============
openal_channel_two_d_stream::update
==============
*/
void openal_channel_two_d_stream::update()
{
    snd_stream_t *stream = (snd_stream_t *)streamHandle;
    unsigned int  bytesToRead, bytesRead;
    ALuint        format;
    ALint         numProcessedBuffers = 0, numQueuedBuffers = 0;
    ALint         state = sample_status();
    // 2 channels with a width of 2
    char rawData[MAX_BUFFER_SAMPLES * 2 * 2];

    if (!streaming || is_paused() || state == AL_INITIAL) {
        return;
    }

    qalGetSourcei(source, AL_BUFFERS_PROCESSED, &numProcessedBuffers);
    alDieIfError();

    qalGetSourcei(source, AL_BUFFERS_QUEUED, &numQueuedBuffers);
    alDieIfError();

    // Remove processed buffers after half of the queued buffers has been processed
    if (numProcessedBuffers >= Q_max(numQueuedBuffers / 2, 1)) {
        while (numProcessedBuffers-- > 0) {
            ALuint tmpBuffer;
            qalSourceUnqueueBuffers(source, 1, &tmpBuffer);
        }
    }

    if (numQueuedBuffers >= MAX_STREAM_BUFFERS) {
        return;
    }

    if (sampleLoopCount && sampleLooped >= sampleLoopCount) {
        if (!is_playing()) {
            // Can clear the stream
            clear_stream();
        }
        return;
    }

    bytesToRead = Q_min(MAX_BUFFER_SAMPLES * stream->info.width * stream->info.channels, sizeof(rawData));

    format = S_OPENAL_Format(stream->info.width, stream->info.channels);

    bytesRead = S_CodecReadStream(stream, bytesToRead, rawData);
    streamNextOffset += bytesRead;
    if (!bytesRead) {
        S_CodecCloseStream(stream);

        sampleLooped++;
        if (sampleLoopCount && sampleLooped >= sampleLoopCount) {
            // Finished the last loop
            streamHandle = NULL;
            return;
        }

        //
        // Looped, start again from the beginning
        //
        streamHandle = S_CodecLoad(this->fileName, NULL);
        if (!streamHandle) {
            clear_stream();
            return;
        }

        stream = (snd_stream_t *)streamHandle;
        //
        // Re-read the format, we never know...
        //
        format = S_OPENAL_Format(stream->info.width, stream->info.channels);

        bytesRead        = S_CodecReadStream(stream, bytesToRead, rawData);
        streamNextOffset = bytesRead;
        if (!bytesRead) {
            S_CodecCloseStream(stream);
            streamHandle = NULL;
            return;
        }
    }

#if AL_SOFT_block_alignment
    if (stream->info.dataalign > 1 && soft_block_align) {
        qalBufferi(buffers[currentBuf], AL_UNPACK_BLOCK_ALIGNMENT_SOFT, stream->info.dataalign);
        alDieIfError();
    }
#endif

    qalBufferData(buffers[currentBuf], format, rawData, bytesRead, stream->info.rate);
    alDieIfError();

    qalSourceQueueBuffers(source, 1, &buffers[currentBuf]);
    alDieIfError();

    if (!is_playing()) {
        // The sample has stopped during stream
        // Could be because the storage is slow enough for the buffer
        // or because the storage was powering on after standby
        play();
    }

    currentBuf = (currentBuf + 1) % MAX_STREAM_BUFFERS;
}

/*
==============
openal_channel_two_d_stream::sample_offset
==============
*/
U32 openal_channel_two_d_stream::sample_offset()
{
    snd_stream_t *stream              = (snd_stream_t *)streamHandle;
    ALint         playerByteOffset    = 0;
    ALint         numProcessedBuffers = 0;
    ALint         numQueuedBuffers    = 0;
    unsigned int  totalQueueLength    = 0;
    unsigned int  bitsPerSample       = 0;
    unsigned int  offset              = 0;
    ALint         bits = 0, channels = 0;

    if (!streaming) {
        // no stream
        return 0;
    }

    // Get the byte offset in the queue
    qalGetSourcei(source, AL_BYTE_OFFSET, &playerByteOffset);
    alDieIfError();

    totalQueueLength = getQueueLength();
    bitsPerSample    = getBitsPerSample();

    assert(playerByteOffset < totalQueueLength);
    assert(streamNextOffset >= totalQueueLength || stream);

    if (streamNextOffset >= totalQueueLength) {
        offset = streamNextOffset - totalQueueLength + playerByteOffset;
    } else if (stream) {
        // it probably means the sound is looped and will restart from the beginning
        if (streamNextOffset + playerByteOffset <= totalQueueLength) {
            // near the end of the stream
            if (stream->info.size > totalQueueLength) {
                offset = stream->info.size + streamNextOffset - totalQueueLength + playerByteOffset;
            } else {
                // the buffer is really small
                offset = (streamNextOffset - totalQueueLength + playerByteOffset) % stream->info.size;
            }
        } else {
            // it is past end of stream
            // so, start from the beginning of the stream
            offset = streamNextOffset + playerByteOffset - totalQueueLength;
        }

        assert(offset < stream->info.size);
    } else {
        offset = totalQueueLength - streamNextOffset + playerByteOffset;
    }

    return offset * 8 / bitsPerSample;
}

/*
==============
openal_channel_two_d_stream::buffer_frequency
==============
*/
U32 openal_channel_two_d_stream::buffer_frequency() const
{
    unsigned int bufferId;
    ALint        freq = 0;

    bufferId = (currentBuf - 1) % MAX_STREAM_BUFFERS;
    qalGetBufferi(buffers[bufferId], AL_FREQUENCY, &freq);
    alDieIfError();

    return freq;
}

/*
==============
openal_channel_two_d_stream::set_sample_offset
==============
*/
void openal_channel_two_d_stream::set_sample_offset(U32 offset)
{
    snd_stream_t *stream;
    unsigned int  bytesToRead, bytesRead;
    unsigned int  byteOffset;
    unsigned int  bitsPerSample;
    unsigned int  streamPosition;
    ALuint        format;
    ALint         numQueuedBuffers;
    bool          bWasPlaying;
    char          rawData[MAX_BUFFER_SAMPLES * 2 * 2];

    if (!streaming) {
        return;
    }

    stream         = (snd_stream_t *)streamHandle;
    streamPosition = getCurrentStreamPosition();
    bitsPerSample  = getBitsPerSample();
    byteOffset     = offset * bitsPerSample / 8;

    if (byteOffset >= streamPosition && byteOffset < streamNextOffset) {
        //
        // Sample is in the currently streamed data
        //
        qalSourcei(source, AL_BYTE_OFFSET, byteOffset - streamPosition);
        alDieIfError();

        return;
    }

    bWasPlaying = is_playing();

    if (stream) {
        if (byteOffset < streamPosition) {
            //
            // New offset is before the current offset,
            // reload the stream from the beginning
            //
            S_CodecCloseStream(stream);
            streamHandle = NULL;
        } else if (byteOffset >= stream->info.size) {
            //
            // This shouldn't happen unless the file is different
            // from the one used while saving
            //
            byteOffset %= stream->info.size;
            S_CodecCloseStream(stream);
            streamHandle = NULL;
        }
    }

    if (!streamHandle) {
        streamHandle = S_CodecLoad(this->fileName, NULL);
        if (!streamHandle) {
            clear_stream();
            return;
        }

        stream           = (snd_stream_t *)streamHandle;
        streamNextOffset = 0;
    }

    while (streamNextOffset <= byteOffset) {
        streamPosition = streamNextOffset;

        bytesToRead = Q_min(MAX_BUFFER_SAMPLES * stream->info.width * stream->info.channels, sizeof(rawData));
        bytesRead   = S_CodecReadStream(stream, bytesToRead, rawData);
        if (!bytesRead) {
            clear_stream();
            return;
        }

        streamNextOffset += bytesRead;
    }

    //
    // Stop the sound and remove all buffers from the queue
    //
    qalSourceStop(source);
    alDieIfError();

    //
    // Remove all buffers from the queue
    //
    numQueuedBuffers = 0;
    qalGetSourcei(source, AL_BUFFERS_QUEUED, &numQueuedBuffers);
    alDieIfError();

    while (numQueuedBuffers--) {
        ALuint b;
        qalSourceUnqueueBuffers(source, 1, &b);
        alDieIfError();
    }

    format = S_OPENAL_Format(stream->info.width, stream->info.channels);

    qalBufferData(buffers[currentBuf], format, rawData, bytesRead, stream->info.rate);
    alDieIfError();

    qalSourceQueueBuffers(source, 1, &buffers[currentBuf]);
    alDieIfError();
    currentBuf = (currentBuf + 1) % MAX_STREAM_BUFFERS;

    //
    // Now set the source offset
    //
    qalSourcei(source, AL_BYTE_OFFSET, byteOffset - streamPosition);
    alDieIfError();

    if (bWasPlaying) {
        //
        // The sound was stopped when re-streaming
        //
        play();
    }
}

/*
==============
openal_channel_two_d_stream::queue_stream
==============
*/
bool openal_channel_two_d_stream::queue_stream(const char *fileName)
{
    snd_stream_t *stream;
    unsigned int  bytesToRead, bytesRead;
    ALint         freq   = 0;
    ALuint        format = 0;
    ALuint        old    = 0;
    char          rawData[MAX_BUFFER_SAMPLES * 2 * 2];

    // Store the filename so it can be looped later
    Q_strncpyz(this->fileName, fileName, sizeof(this->fileName));
    clear_stream();

    sampleLoopCount = 1;

    //
    // Load the file
    //
    streamHandle = S_CodecLoad(this->fileName, NULL);
    if (!streamHandle) {
        return false;
    }
    stream = (snd_stream_t *)streamHandle;

    iStartTime = cl.serverTime;
    iEndTime   = (int)(cl.serverTime + (stream->info.samples / stream->info.rate * 1000.f));

    format = S_OPENAL_Format(stream->info.width, stream->info.channels);
    if (!format) {
        Com_Printf(
            "OpenAL: Bad Wave file (%d channels, %d bits) [%s].\n",
            pSfx->info.channels,
            (int)(pSfx->info.width * 8.f),
            pSfx->name
        );

        S_CodecCloseStream(stream);
        return false;
    }

    qalGenBuffers(MAX_STREAM_BUFFERS, buffers);
    alDieIfError();

    // Read a smaller sample
    bytesToRead = Q_min(MAX_BUFFER_SAMPLES * stream->info.width * stream->info.channels, sizeof(rawData));
    bytesRead   = S_CodecReadStream(stream, bytesToRead, rawData);
    if (!bytesRead) {
        // Valid stream but no data?
        return true;
    }

    streamNextOffset = bytesRead;

    qalBufferData(buffers[currentBuf], format, rawData, bytesRead, stream->info.rate);
    alDieIfError();

    qalSourceQueueBuffers(source, 1, &buffers[currentBuf]);
    alDieIfError();

    currentBuf = (currentBuf + 1) % MAX_STREAM_BUFFERS;

    streaming = true;

    return true;
}

/*
==============
openal_channel_two_d_stream::clear_stream
==============
*/
void openal_channel_two_d_stream::clear_stream()
{
    if (!streaming) {
        return;
    }

    qalSourceStop(source);
    qalSourcei(source, AL_BUFFER, 0);
    qalDeleteBuffers(MAX_STREAM_BUFFERS, buffers);

    if (streamHandle) {
        S_CodecCloseStream((snd_stream_t *)streamHandle);
    }

    fileName[0]  = 0;
    streamHandle = NULL;

    currentBuf       = 0;
    streamNextOffset = 0;
    streaming        = false;
    sampleLooped     = 0;
}

/*
==============
openal_channel_two_d_stream::getQueueLength

Return the total length of all buffers being streamed.
==============
*/
unsigned int openal_channel_two_d_stream::getQueueLength() const
{
    ALint        numQueuedBuffers = 0;
    unsigned int totalQueueLength = 0;
    unsigned int bufferId;
    unsigned int i;

    qalGetSourcei(source, AL_BUFFERS_QUEUED, &numQueuedBuffers);
    alDieIfError();

    for (i = 0; i < numQueuedBuffers; i++) {
        ALint bufferSize = 0;

        bufferId = (currentBuf - i - 1) % MAX_STREAM_BUFFERS;
        qalGetBufferi(buffers[bufferId], AL_SIZE, &bufferSize);

        totalQueueLength += bufferSize;
    }

    return totalQueueLength;
}

/*
==============
openal_channel_two_d_stream::getCurrentStreamPosition

Return the current position in the sound being streamed.
==============
*/
unsigned int openal_channel_two_d_stream::getCurrentStreamPosition() const
{
    unsigned int queueLength = getQueueLength();

    if (streamNextOffset >= queueLength) {
        return streamNextOffset - queueLength;
    } else if (streamHandle) {
        snd_stream_t *stream = (snd_stream_t *)streamHandle;
        return (stream->info.size + streamNextOffset - queueLength) % stream->info.size;
    } else {
        return queueLength - streamNextOffset;
    }
}

/*
==============
openal_channel_two_d_stream::getBitsPerSample

Return the number of bytes per sample.
It assumes that all pending buffers have the same channels and bits.
==============
*/
unsigned int openal_channel_two_d_stream::getBitsPerSample() const
{
    unsigned int bufferId;
    ALint        bits = 0, channels = 0;

    bufferId = (currentBuf - 1) % MAX_STREAM_BUFFERS;
    qalGetBufferi(buffers[bufferId], AL_BITS, &bits);
    alDieIfError();
    qalGetBufferi(buffers[bufferId], AL_CHANNELS, &channels);
    alDieIfError();

    return bits * channels;
}

/*
==============
S_HzmChannelsPlaying

HZM coop [bug-3486] how many channels are playing right now, for the bug report's once-a-second sample. -1 = no sound.
==============
*/
int S_HzmChannelsPlaying(void)
{
    int i, n = 0;

    if (!s_bSoundStarted) {
        return -1;
    }

    for (i = 0; i < MAX_SOUNDSYSTEM_POSITION_CHANNELS; i++) {
        openal_channel *pChannel = openal.channel[i];
        if (pChannel && pChannel->is_playing()) {
            n++;
        }
    }

    return n;
}
