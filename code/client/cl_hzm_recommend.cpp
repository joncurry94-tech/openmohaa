/*
===========================================================================
HZM coop [bug-3484] RECOMMENDED GRAPHICS PRESET

Options -> VIDEO shows "RECOMMENDED: <preset>" and a knob that applies it. This file works the preset out.

WHAT LIMITS THIS GAME. Measured on Omaha (bug-3483): the engine runs game, effects and drawing on ONE core and
skins every soldier on the CPU, so a crowded map is limited by single-core speed first and by the video card
second. A recommendation from video memory alone would tell an older six-core with a big card to run Ultra and
it would crawl on the beach. So the inputs are, in this order:
  1. single-core speed  - a ~20 ms skinning-shaped float loop at launch, best of five short windows, scaled so
                          the development PC (Ryzen 7 9700X, PassMark single-thread ~4650) reads ~4650.
  2. video memory       - ROM r_gpuVramMB, published by renderergl2 (tr_msaa.c) once it has asked the driver.
                          0 = not asked yet, -1 = unknown: then only the CPU decides, capped at High.

The result is published in two READ-ONLY cvars (a server cannot write them, so the knob's vstr cannot be used
to plant a command):
  coop_gfxRecommended  1 Low, 2 Medium, 3 High, 4 Ultra   (the menu readout)
  coop_gfxRecCmd       "exec coop_mod/cfg/gfx_preset_<name>.cfg"   (the menu knob runs it through vstr)

Nothing is ever applied automatically: the player's settings are the player's.
===========================================================================
*/

#include "client.h"

#include <chrono>
#include <thread>

// the development PC's raw loop score and its published single-thread rating: every other machine is read
// against this pair. Re-measure HZM_REC_DEV_RAW (the GFXREC line prints raw=) if the loop below ever changes.
#define HZM_REC_DEV_RAW    344.0
#define HZM_REC_DEV_RATING 4650.0

static cvar_t *s_recCvar;
static cvar_t *s_recCmdCvar;
static cvar_t *s_recDebug;
static double  s_rawScore;
static int     s_lastVram = -999;
static int     s_nextCheck;

/*
====================
REC_RawScore

Thousands of weighted-vertex transforms per millisecond, best of five ~4 ms windows. Scalar float maths on
data that does not fit a register and does fit the cache - the shape of RB_SkelMesh.
====================
*/
static double REC_RawScore(void)
{
    static float bones[64][12];
    static float verts[1024][4];
    volatile float sink = 0.0f;
    double       best = 0.0;
    int          w, i, b;

    for (b = 0; b < 64; b++) {
        for (i = 0; i < 12; i++) {
            bones[b][i] = 0.01f * (float)((b * 7 + i * 3) % 97);
        }
    }
    for (i = 0; i < 1024; i++) {
        verts[i][0] = (float)(i % 31);
        verts[i][1] = (float)(i % 17);
        verts[i][2] = (float)(i % 13);
        verts[i][3] = 0.5f;
    }

    for (w = 0; w < 5; w++) {
        auto   t0    = std::chrono::steady_clock::now();
        double count = 0.0;
        double ms    = 0.0;

        do {
            float acc = 0.0f;
            for (i = 0; i < 1024; i++) {
                const float *m1 = bones[i & 63];
                const float *m2 = bones[(i * 5 + 11) & 63];
                const float *v  = verts[i];
                float        x  = (m1[0] * v[0] + m1[1] * v[1] + m1[2] * v[2] + m1[3]) * v[3]
                                + (m2[0] * v[0] + m2[1] * v[1] + m2[2] * v[2] + m2[3]) * (1.0f - v[3]);
                float        y  = (m1[4] * v[0] + m1[5] * v[1] + m1[6] * v[2] + m1[7]) * v[3]
                                + (m2[4] * v[0] + m2[5] * v[1] + m2[6] * v[2] + m2[7]) * (1.0f - v[3]);
                float        z  = (m1[8] * v[0] + m1[9] * v[1] + m1[10] * v[2] + m1[11]) * v[3]
                                + (m2[8] * v[0] + m2[9] * v[1] + m2[10] * v[2] + m2[11]) * (1.0f - v[3]);
                acc += x * 0.001f + y * 0.002f + z * 0.003f;
            }
            sink  = sink + acc;
            count += 1.024;
            ms     = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        } while (ms < 4.0);

        if (ms > 0.0 && count / ms > best) {
            best = count / ms;
        }
    }
    (void)sink;
    return best;
}

/*
====================
REC_Decide
====================
*/
static int REC_Decide(double rating, int vramMB)
{
    int byCpu;
    int byVram;

    if (rating >= 4200.0) {
        byCpu = 4;
    } else if (rating >= 3300.0) {
        byCpu = 3;
    } else if (rating >= 2200.0) {
        byCpu = 2;
    } else {
        byCpu = 1;
    }

    if (vramMB <= 0) {
        // not known: the CPU decides, but never promise Ultra on a card we know nothing about
        return byCpu > 3 ? 3 : byCpu;
    }
    if (vramMB >= 11000) {
        byVram = 4;
    } else if (vramMB >= 7000) {
        byVram = 3;
    } else if (vramMB >= 3500) {
        byVram = 2;
    } else {
        byVram = 1;
    }
    return byCpu < byVram ? byCpu : byVram;
}

/*
====================
CL_HzmRecommend_Update

Cheap: at most once a second it looks at r_gpuVramMB and republishes when that changed. The CPU test runs once.
====================
*/
void CL_HzmRecommend_Update(void)
{
    static const char *names[5] = {"low", "low", "medium", "high", "ultra"};
    static const char *label[5] = {"Low", "Low", "Medium", "High", "Ultra"};
    cvar_t            *vram;
    int                vramMB;
    int                rec;
    double             rating;

    if (cls.realtime < s_nextCheck && s_recCvar) {
        return;
    }
    s_nextCheck = cls.realtime + 1000;

    if (!s_recCvar) {
        s_recCvar    = Cvar_Get("coop_gfxRecommended", "0", CVAR_ROM);
        s_recCmdCvar = Cvar_Get("coop_gfxRecCmd", "", CVAR_ROM);
        s_recDebug   = Cvar_Get("coop_gfxRecDebugRating", "0", 0);
        s_rawScore   = REC_RawScore();
    }

    vram   = Cvar_Get("r_gpuVramMB", "0", CVAR_ROM);
    vramMB = vram ? vram->integer : 0;
    if (vramMB == s_lastVram && !s_recDebug->modified) {
        return;
    }
    s_lastVram           = vramMB;
    s_recDebug->modified = qfalse;

    rating = s_rawScore / HZM_REC_DEV_RAW * HZM_REC_DEV_RATING;
    if (s_recDebug->value > 0.0f) {
        rating = s_recDebug->value;     // test hook: pretend to be another CPU
    }
    rec = REC_Decide(rating, vramMB);

    Cvar_Set("coop_gfxRecommended", va("%d", rec));
    Cvar_Set("coop_gfxRecCmd", va("seta coop_gfxPreset %d ; exec coop_mod/cfg/gfx_preset_%s.cfg", rec, names[rec]));
    Com_Printf("^~^~^ GFXREC raw=%.0f rating=%.0f cores=%d vramMB=%d -> %s\n", s_rawScore, rating,
               (int)std::thread::hardware_concurrency(), vramMB, label[rec]);
}
