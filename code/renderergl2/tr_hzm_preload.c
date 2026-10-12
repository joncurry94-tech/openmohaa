/*
===========================================================================
HZM coop [user 2026-10-11, bug-3493] LEARNED SHADER PRELOAD

"Fix the enemy spawns on one frame." The slow frames of a firefight, profiled (frames >= 28 ms only, m3l1b), were
not spawns. After the wound-copy cost was dealt with, the largest thing left in them was R_FindShaderEx ->
ParseShader -> R_FindImageFile -> file read, JPG/TGA decode, texture upload: 28 % of slow-frame time. A file-open
log of the same four minutes names them - the *_blood3 face skins (13 of them), the wound stamp and blood splash,
blood pool / splat / wound sprites, blast and bullet-hole decals, rank patches - every one a texture the map did
not know it would need until somebody was shot.

They cannot be listed by hand: which ones a map needs depends on who is in it. So the renderer LEARNS them. Any
shader that has to be created after the map finished loading is written to hzm_preload/<map>.txt in the player's
game folder, and the next time that map loads the list is registered during the loading screen. The first play of
a map hitches as it always did; every later one does not. hzm_preload/_common.txt, shipped in the mod pak, holds
the ones every map needs (HUD icons, blood and bullet-hole decals, impact puffs), so nobody has to play a map once
to be rid of those; a per-map list can be shipped the same way.

Bounded (HZP_MAX records), text, one shader per line: "<lightmapIndex> <mip> <realLightmapIndex> <name>".
Only lightmap-less shaders are kept - models, sprites, decals, effects, HUD; a world surface is never created late.
r_hzmPreload 0 = neither record nor preload.
===========================================================================
*/

#include "tr_local.h"

#define HZP_MAX 768

typedef struct {
    char     name[MAX_QPATH];
    int      lm, mip, rlm;
    qboolean common; // from hzm_preload/_common.txt (shipped, every map): registered, never written to a map file
} hzpRec_t;

static hzpRec_t s_rec[HZP_MAX];
static int      s_num;
static int      s_fromFile;       // how many of them came from the file (the rest were learned this session)
static qboolean s_active;         // the map has finished loading: a shader created now is a LATE one
static qboolean s_dirty;
static qboolean s_loading;        // inside our own preload: do not record what we are registering
static char     s_map[MAX_QPATH]; // base name of the map the list belongs to
static cvar_t  *r_hzmPreload;

static qboolean HZP_On(void)
{
    if (!r_hzmPreload) {
        r_hzmPreload = ri.Cvar_Get("r_hzmPreload", "1", CVAR_ARCHIVE);
    }
    return r_hzmPreload->integer ? qtrue : qfalse;
}

static void HZP_Write(void);

static int HZP_Find(const char *name, int lm)
{
    int i;

    for (i = 0; i < s_num; i++) {
        if (s_rec[i].lm == lm && !Q_stricmp(s_rec[i].name, name)) {
            return i;
        }
    }
    return -1;
}

/*
====================
R_HzmPreloadNote

Called by R_FindShaderEx when a shader is about to be CREATED (it was not in the table).
====================
*/
void R_HzmPreloadNote(const char *name, int lightmapIndex, qboolean mipRawImage, int realLightmapIndex)
{
    hzpRec_t *r;

    if (!s_active || s_loading || lightmapIndex >= 0 || !name || !name[0] || name[0] == '*' || !HZP_On()) {
        return;
    }
    if (strchr(name, ' ') || strchr(name, '\n') || strchr(name, '"')) {
        return; // would not survive the one-line text format
    }
    if (s_num >= HZP_MAX || HZP_Find(name, lightmapIndex) >= 0) {
        return;
    }
    r = &s_rec[s_num++];
    Q_strncpyz(r->name, name, sizeof(r->name));
    r->lm   = lightmapIndex;
    r->mip  = mipRawImage ? 1 : 0;
    r->rlm    = realLightmapIndex;
    r->common = qfalse;
    s_dirty   = qtrue;

    // a game that is killed or crashes never reaches the flush: write as we go, at most every 15 s. This frame
    // is already paying for a texture load, a few KB of text more is nothing.
    {
        static int lastWrite = 0;
        int        now       = ri.Milliseconds();

        if (now - lastWrite > 15000 || now < lastWrite) {
            lastWrite = now;
            HZP_Write();
        }
    }
}

/*
====================
R_HzmPreloadFlush

A map is going away (RE_BeginRegistration) or the renderer is (RE_Shutdown): write what was learned.
====================
*/
void R_HzmPreloadFlush(void)
{
    s_active = qfalse;
    HZP_Write();
}

static void HZP_Write(void)
{
    char *buf;
    int   i, n = 0, cap;

    if (!s_dirty || !s_map[0] || !s_num) {
        return;
    }
    s_dirty = qfalse;
    cap     = s_num * (MAX_QPATH + 40) + 16;
    buf     = ri.Malloc(cap);
    if (!buf) {
        return;
    }
    for (i = 0; i < s_num; i++) {
        if (s_rec[i].common) {
            continue;
        }
        n += Com_sprintf(buf + n, cap - n, "%d %d %d %s\n", s_rec[i].lm, s_rec[i].mip, s_rec[i].rlm, s_rec[i].name);
    }
    ri.FS_WriteFile(va("hzm_preload/%s.txt", s_map), buf, n);
    ri.Printf(PRINT_DEVELOPER, "^~^~^ PRELOAD wrote hzm_preload/%s.txt: %d shaders (%d new this session)\n", s_map, s_num,
              s_num - s_fromFile);
    ri.Free(buf);
}

// one shader per line: "<lightmapIndex> <mip> <realLightmapIndex> <name>"; '/' starts a comment line
static void HZP_ReadList(const char *qpath, qboolean common)
{
    void *data = NULL;
    long  len  = ri.FS_ReadFileEx ? ri.FS_ReadFileEx(qpath, &data, qtrue) : ri.FS_ReadFile(qpath, &data);

    if (len > 0 && data) {
        const char *p = (const char *)data, *end = p + len;

        while (p < end && s_num < HZP_MAX) {
            char line[MAX_QPATH + 64];
            int  k = 0, lm, mip, rlm, used = 0;

            while (p < end && *p != '\n' && k < (int)sizeof(line) - 1) {
                if (*p != '\r') {
                    line[k++] = *p;
                }
                p++;
            }
            while (p < end && *p != '\n') {
                p++; // an over-long line: drop its tail
            }
            if (p < end) {
                p++;
            }
            line[k] = 0;
            if (line[0] != '/' && sscanf(line, "%d %d %d %n", &lm, &mip, &rlm, &used) >= 3 && used > 0 && line[used]
                && lm < 0 && strlen(line + used) < MAX_QPATH && HZP_Find(line + used, lm) < 0) {
                hzpRec_t *r = &s_rec[s_num++];

                Q_strncpyz(r->name, line + used, sizeof(r->name));
                r->lm     = lm;
                r->mip    = mip ? 1 : 0;
                r->rlm    = rlm;
                r->common = common;
            }
        }
    }
    if (data) {
        ri.FS_FreeFile(data);
    }
}

/*
====================
R_HzmPreloadMapLoaded

RE_EndRegistration: the map and everything it asked for are loaded. Register what this map needed mid-play the
last time, then start recording.
====================
*/
void R_HzmPreloadMapLoaded(void)
{
    const char *base = (tr.world && tr.world->baseName[0]) ? tr.world->baseName : "";
    int         t0, done = 0;

    if (!HZP_On() || !base[0]) {
        s_active = qfalse; // menus, or switched off
        return;
    }
    if (Q_stricmp(base, s_map)) {
        R_HzmPreloadFlush(); // normally already done by RE_BeginRegistration
        Q_strncpyz(s_map, base, sizeof(s_map));
        s_num = s_fromFile = 0;
        s_dirty = qfalse;

        HZP_ReadList(va("hzm_preload/%s.txt", s_map), qfalse); // what THIS map needed mid-play before
        HZP_ReadList("hzm_preload/_common.txt", qtrue);         // shipped: what every map needs (HUD, decals, puffs)
        s_fromFile = s_num;
    }

    // (a vid_restart unloads this DLL, so the list is re-read from the file RE_Shutdown just wrote)
    t0        = ri.Milliseconds();
    s_loading = qtrue;
    for (done = 0; done < s_num; done++) {
        R_FindShaderEx(s_rec[done].name, s_rec[done].lm, s_rec[done].mip ? qtrue : qfalse, s_rec[done].rlm);
    }
    s_loading = qfalse;
    if (s_num) {
        ri.Printf(PRINT_ALL, "^~^~^ PRELOAD %s: %d shaders this map needed mid-play before, registered in %d ms\n", s_map,
                  s_num, ri.Milliseconds() - t0);
    }
    s_active = qtrue;
}
