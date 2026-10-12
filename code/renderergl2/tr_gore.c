/*
===========================================================================
Copyright (C) 2026 HaZardModding

HZM coop - gore tier 4 (UV wounds) - gl2 port ("HZM gl2 re-port", bug-gl2-gore)

Per-entity UV-space wound painting for CPU-skinned TIKI characters.

This is the gl1 tr_gore.c ported to renderergl2. The wound logic is
byte-identical; only the renderer bridge points differ:
 - tess.texCoords is a single-bundle vec2_t array in gl2 (gl1 has a
   per-bundle [2] dimension), so UV reads drop the trailing [0].
 - shader stages live in shader->stages (gl1: shader->unfoggedStages).
 - GL texture updates go through the tr_dsa.c wrappers
   (qglTextureImage2DEXT / qglTextureSubImage2DEXT / qglTextureParameterfEXT,
   the RE_UploadCinematic pattern) so the DSA binding cache stays coherent;
   the glGetTexImage readback rebinds raw on unit 0 and then resets the whole
   cache with GL_BindNullTextures.
 - the instance image slot is created with gl2's R_CreateImage.
 - r_goreGpu (default 1): step 3 is done on the GPU - the instance texture is an FBO
   target, the base is drawn into it and each stamp is a quad whose fragment shader is the
   CPU compositor's loop body. Nothing is read back. See "GPU COMPOSITOR" below; the CPU
   path (r_goreGpu 0) is kept whole, as the reference and as the fallback.

How it works, end to end:
 1. cgame forwards every server-authoritative bullet segment (trace start ->
    trace end) through the RE_GoreImpact export.  Segments are queued
    here as "pending impacts" (small ring, ~quarter-second lifetime).
 2. RB_SkelMesh already skins every visible character surface on the CPU
    each frame (tess.xyz / tess.texCoords).  Right after a surface is
    skinned, R_GoreSkelSurfaceCheck() ray-tests the pending impact segments
    against those freshly skinned triangles (in model space).  A triangle
    intersection close to the segment END (= where the server stopped the
    bullet) wins; the barycentric-interpolated UV of the hit is recorded.
 3. At end of frame (all surfaces of all entities tested -> global best
    hit is known), R_GoreCommitPending() composites a small wound stamp
    (dark hole core + #150200 blood halo - the mod's blood color authority)
    into a per-entity RGBA copy of that surface's diffuse texture and
    uploads the touched rows.
 4. R_BindAnimatedImageToTMU asks R_GoreOverrideImage() before binding a
    stage texture; entities that own a wound instance get their copy bound
    in place of the shared base diffuse.  Everyone else keeps the base.

Bounds: at most GORE_MAX_INSTANCES per-entity texture copies exist at once
(LRU-stolen), each capped at GORE_MAX_TEXBYTES.  Instances are reset when
the entity comes back to life (cgame calls RE_GoreReset on the EF_DEAD
falling edge / teleport), on map change, and on renderer shutdown.

Everything is client-side; no server or protocol changes.  Late joiners
simply accumulate wounds from impacts they observe (accepted divergence,
same as the existing client-side drip bouncedecals).
===========================================================================
*/

#include "tr_local.h"
#include "tr_dsa.h"

// may be absent from minimal GL headers (same guard as the gl1 module)
#ifndef GL_RGBA8
#define GL_RGBA8 0x8058
#endif

// bug-780: was 16. A coop firefight demands an instance for EVERY bleeding character hit
// by ANY shooter (cg_parsemsg forwards every volley - players AND both AI sides), times
// 1-4 wounded surfaces each; 16 saturated within one m5l1a skirmish and the LRU steal
// began eating LIVE instances (see the steal-priority fix in R_GoreAcquireInstance).
// Cost is per-WOUNDED-surface only (typ. 2-8 MB CPU+GL each, LRU-bounded).
#define GORE_MAX_INSTANCES   48      // simultaneous per-entity texture copies
#define GORE_MAX_PENDING     32      // queued bullet segments
#define GORE_PENDING_LIFE_MS 250     // pending impact expires after this
#define GORE_MAX_ENTNUM      MAX_GENTITIES // HZM bug-930: follows the protocol constant (2048 since the GENTITYNUM_BITS 11 op)
#define GORE_NEAR_END_DIST   20.0f   // tri hit must be this close to the bullet stop point
#define GORE_SEGMENT_EXTEND  16.0f   // test slightly past the reported end (LBD spheres vs mesh)
// HZM coop (skin-snap fallback): compile-default tolerance for the moving-enemy skin snap - the
// nearest EXPOSED-SKIN vertex within this model-space distance of the bullet stop point is stamped
// when the exact ray finds nothing.  Overridden at use by coop_goreSkinSnapDist (clamped 8-64).
// bug-905: was 18. On a LIVING enemy the bullet STOP point (server pose + inflated LBD head/hand
// hitbox) sits farther from the client's animated skin verts than 18u - face/head/hand shots kept
// finding neither an exact ray hit nor a snap and expired with no wound (corpses match poses, so
// they hit exactly and were unaffected).  26u catches the common mover offset; still skin-only, so
// cloth/uniform wounds are untouched and the exact-hit path (torso) is unchanged.
#define GORE_SKIN_SNAP_DIST  26.0f
// bug-754: was 1024x1024. The HD packs ship 2048x1024 character diffuses (e.g.
// textures/models/human/heer/wh_soldat.dds - the wehrmacht TUNIC) and the old cap silently
// refused an instance for them -> wounds appeared on pants/hands/faces (<=1024 textures) but
// NEVER on the torso. 16 instances x 16MB worst case is acceptable; LRU keeps it honest.
#define GORE_MAX_TEXBYTES    (2048 * 2048 * 4) // refuse instances above 2048x2048 RGBA
#define GORE_MAX_STAMPS      32      // wound history replayed when a tier skin-flip swaps the diffuse
#define GORE_STAMP_FALLBACK  48      // procedural stamp size if the TGA is missing
// bug-735: was 36/512 (~7% of texture width) - at gameplay distance the wound vanished. 56/512 reads.
#define GORE_STAMP_BASEFRAC  (56.0f / 512.0f)  // stamp diameter as fraction of a 512 texture

// bug-780 (more blood): every wound stamp now lays a LARGER soft blood-splash halo under
// the hole (irregular rim, alpha falloff, #150200 family with a brighter wet core - same
// palette authority as the hole art), and the KILLING blow adds 2-3 blood-only splashes
// at random body UVs so corpses read bloodied even where the tier skins fail (surfaces
// with numskins==1 fall back to the clean shader - tr_model.cpp iShaderNum >= numskins).
#define GORE_STAMP_MAXPX     128     // per-stamp hole clamp in texels (pre-existing value)
#define GORE_SPLASH_SCALE    2.4f    // splash halo diameter as a multiple of the hole stamp
// bug-817 (user "dial the blood back"): 448 -> 320 (pre-round-4 value). The bug-791 crank
// to 448 made kill splashes read as oversized red plates; restored to the bug-780 clamp.
#define GORE_SPLASH_MAXPX    320     // per-stamp splash clamp in texels
#define GORE_KILL_LIFE_MS    750     // killing-blow flag waits this long for a first instance

// the mod's blood palette: #150200 base with a slightly lifted wet core so the wound READS on dark
// cloth (bug-735 - pure #150200 was invisible on field-gray), drying toward (7,1,0) at the rim.
// bug-828 (user: "some bullet holes appear lighter red than others"): the old HOT core was 110,12,5
// = a BRIGHT arterial red, so wound halos / kill splashes read light-red while the hole centres read
// dark = inconsistent. bug-829: brought fully into the dark #150200 family (max R ~26-33, matching the
// pool + the coordinator's gen_blood_sprites drips/wound-prop at R26) - a near-#150200 wet sheen only.
// Same value used by the authored stamp art (gen_woundstamp.py) so procedural + authored match.
#define GORE_BLOOD_R 21
#define GORE_BLOOD_G 2
#define GORE_BLOOD_B 0
#define GORE_HOT_R   30
#define GORE_HOT_G   3
#define GORE_HOT_B   0
#define GORE_DRIED_R 7
#define GORE_DRIED_G 1
#define GORE_DRIED_B 0

// one recorded wound, dimension-independent so it can be replayed onto a different-size
// diffuse (bug-754: the tier-1 skin system rebinds a _blood1/_blood2 variant image when a
// sentient gets bloody - wounds must migrate onto the new texture, not vanish)
typedef struct goreStampRec_s {
    float u, v;      // impact UV
    float angle;     // stamp rotation
    float sizeFrac;  // stamp diameter as a fraction of the texture width
    int   kind;      // bug-780: 0 = bullet wound (splash halo + hole), 1 = blood-only splash
    // HZM coop (bloodier skin): 1 if this wound landed on EXPOSED SKIN (face/head/hands).
    // The skin factor is already baked into sizeFrac at record time; this flag only lets the
    // compositor lift the hole/splash texel clamps by the same factor so the enlargement is
    // not eaten on HD (2048) skin diffuses.  Stored in the record so tier-skin replays keep it.
    qboolean skin;
} goreStampRec_t;

typedef struct goreInstance_s {
    qboolean inuse;
    int      entityNumber;
    image_t *baseImage;     // shared diffuse this instance replaces
    image_t *instImage;     // reusable "*goreN" image slot (r_sequence -1)
    byte    *pixels;        // CPU RGBA working copy (ri.Malloc)
    int      width, height; // dimensions of pixels / instImage upload
    int      fullWidth;     // [bug-3493] width of the base image itself; width may be a smaller mip of it
    // GPU compositor (r_goreGpu): qtrue = instImage is painted through an FBO and `pixels` is NULL (or, with
    // r_goreGpu 2, a CPU reference copy that every pass is compared against). qfalse = the CPU path.
    qboolean gpu;
    int      texW, texH;    // storage instImage's level 0 was last given (a property of the SLOT: survives a free)
    int      lastUseFrame;  // LRU (tr.frameCount at last bind/stamp)
    // bug-754: skin-flip migration
    char           family[MAX_QPATH];         // baseImage name minus extension + _bloodN suffix
    image_t       *rebaseImage;               // tier-skin variant seen at bind time; applied end of frame
    goreStampRec_t stamps[GORE_MAX_STAMPS];   // wound history (ring)
    int            numStamps;
    int            stampHead;
} goreInstance_t;

typedef struct gorePending_s {
    qboolean inuse;
    int      msec;          // ri.Milliseconds() when queued
    vec3_t   vStart;        // world-space bullet segment
    vec3_t   vEnd;
    // best hit found so far this frame (surface-agnostic, global best)
    qboolean hasHit;
    float    bestS;         // param along segment (smaller = closer to shooter)
    float    bestDistToEnd;
    float    bestU, bestV;
    int      bestEntityNumber;
    image_t *bestImage;     // diffuse of the surface that was hit
    // bug-829 ("more leaks where the bullet holes are"): the direction of world-DOWN projected
    // onto the hit surface, expressed as a stamp rotation so the baked trickle tail runs down-body.
    qboolean bestHasDown;   // true if bestDownAngle is valid (non-degenerate triangle mapping)
    float    bestDownAngle; // atan2(gx,gy) so R_GoreStampLayer aligns art +Y (the trickle) to gravity
    // HZM coop (bloodier skin): the best-hit surface is EXPOSED SKIN (face/head/hands),
    // resolved from the hit diffuse's image name in R_GoreSkelSurfaceCheck.
    qboolean bestSkin;
    // HZM coop (headshot feedback): the best-hit surface is WORN HEADGEAR (helmet/cap/hat).
    // The engine hides those surfaces when it pops the helmet on a killing head hit, taking the
    // stamp along - so a headgear hit ADDITIONALLY stamps the nearest skin vertex (see commit).
    qboolean bestHeadgear;
    // HZM coop (skin-snap fallback): on a MOVING enemy the exact ray (server segment vs the
    // client's interpolated/animated pose) misses the small skin surfaces that slid off the
    // segment; only the torso still catches it.  When the exact ray finds nothing, remember the
    // EXPOSED-SKIN vertex nearest the bullet stop point and stamp that instead.  These mirror the
    // best* exact-hit fields so R_GoreCommitPending can consume a snap exactly like an exact hit.
    // Consumed ONLY when hasHit is false (exact hits are always preferred); skin surfaces only.
    qboolean hasSnap;
    float    snapDist;          // model-space distance vertex->localEnd (smaller wins)
    float    snapU, snapV;      // the vertex's own diffuse UV (no barycentric needed)
    int      snapEntityNumber;
    image_t *snapImage;         // diffuse of the skin surface that owns the vertex
    qboolean snapHasDown;       // false = trickle falls back to a random roll
    float    snapDownAngle;
} gorePending_t;

static goreInstance_t s_goreInstances[GORE_MAX_INSTANCES];
static gorePending_t  s_gorePending[GORE_MAX_PENDING];
static int            s_goreActiveCount;                 // fast-out for the bind hook
static int            s_gorePendingCount;                // fast-out for the skin hook
static qboolean       s_goreRebasePending;               // some instance saw a skin-flipped diffuse
static byte           s_goreEntityHas[GORE_MAX_ENTNUM];  // fast-out per entity

// wound stamp (RGBA, loaded from the mod pk3 or procedurally generated)
static byte    *s_stampPix;
static int      s_stampW, s_stampH;
static qboolean s_stampTried;

// bug-780: blood-splash art (soft irregular blot, no hole) - halo layer under every
// wound + the killing-blow corpse splashes. Same load-or-fallback pattern as the stamp.
static byte *s_splashPix;
static int   s_splashW, s_splashH;

// GPU compositor (r_goreGpu): the two art layers also live in GL textures; set whenever the CPU art was (re)loaded
static qboolean s_goreArtDirty;
static cvar_t  *r_goreGpu;
static int      s_goreMode = -1; // r_goreGpu value the live instances were built under (-1 = none built yet)

// bug-780: killing-blow splash flags per entity (ri.Milliseconds when flagged, 0 = none).
// Set by cgame on the EF_DEAD rising edge, consumed end-of-frame in R_GoreCommitPending.
static int s_goreKillMs[GORE_MAX_ENTNUM];
static int s_goreKillCount;

static unsigned int s_goreSeed = 0x9e3779b9u;

/*
================
R_GoreRand01
================
*/
static float R_GoreRand01(void)
{
    s_goreSeed = s_goreSeed * 1664525u + 1013904223u;
    return (float)((s_goreSeed >> 8) & 0xffffff) / 16777215.0f;
}

/*
================
R_GoreReadTexImage

HZM gl2 re-port (bug-gl2-gore): gl2 tracks texture bindings through the DSA
cache (tr_dsa.c), so the classic bind + glGetTexImage readback must not leave
that cache lying about real GL state.  Bind raw on unit 0, read, then reset
EVERY cached binding via GL_BindNullTextures (which re-syncs cache and GL in
both the real-DSA and the fallback path).  Only runs on wound-instance
creation / tier-skin rebase, so the full rebind cost afterwards is
irrelevant.
================
*/
static void R_GoreReadTexImage(image_t *img, int level, byte *out)
{
    qglActiveTexture(GL_TEXTURE0);
    qglBindTexture(GL_TEXTURE_2D, img->texnum);
    qglGetTexImage(GL_TEXTURE_2D, level, GL_RGBA, GL_UNSIGNED_BYTE, out);
    GL_BindNullTextures();
}

/*
================
R_GoreInstanceDims

HZM coop [bug-3493, part 2] the size a wound instance of `img` is built at, and the mip level it is read from.
After the base-pixel cache EVERY first wound was still a slow frame, cache hit or not: the readback scales with the
pixels (measured 31 ms for 2048x2048, 7 ms for 1024x1024) and so does everything after it - a 16 MB allocation, a
16 MB copy and a 16 MB upload for each wounded body part of an HD skin. The cost is the size, so the instance is
made from the first mip level that fits r_goreMaxSize: a quarter of the bytes per step down. DEFAULT 4096 = NO
reduction - the user's decision (2026-10-11: "keep the full gore and do the full fix"); 1024 is the measured
quick fix (readbacks 3-9 ms instead of ~31) and stays available as a switch until the GPU compositor lands. The
stamp clamps scale with it (R_GoreStampPixels), so a wound covers the same part of the body as before.
Images without mipmaps are never reduced.
================
*/
static void R_GoreInstanceDims(const image_t *img, int *w, int *h, int *level)
{
    static cvar_t *maxSize = NULL;
    int            lim;

    if (!maxSize) {
        maxSize = ri.Cvar_Get("r_goreMaxSize", "4096", CVAR_ARCHIVE);
    }
    lim = maxSize->integer;
    if (lim < 64) {
        lim = 64;
    }
    *w     = img->uploadWidth;
    *h     = img->uploadHeight;
    *level = 0;
    if (!(img->flags & IMGFLAG_MIPMAP)) {
        return;
    }
    while ((*w > lim || *h > lim) && *w >= 16 && *h >= 16) {
        *w >>= 1;
        *h >>= 1;
        (*level)++;
    }
}

/*
================
R_GoreBasePixels

HZM coop [user 2026-10-11, bug-3493] "fix the enemy spawns on one frame". The slow frames of a firefight, profiled
on m3l1b (frames >= 28 ms only): 52 % of their time was RE_EndFrame -> R_GoreCommitPending -> R_GoreAcquireInstance
-> glGetTexImage. The FIRST wound on a soldier copies his whole skin texture back from the GPU, synchronously - a
compressed 2048x2048 skin is decompressed by the driver and the pipeline is flushed - about 55 ms, on every first
hit of every soldier, for a picture that is the same for every man wearing that uniform.

So the readback is kept: a small cache of base pixels keyed by the image's NAME and size (not its pointer or GL
id - those are recycled across maps, the name is what the pixels are a function of; a changed pak set or picmip
goes through a renderer restart, which unloads this DLL and the cache with it). The first wound on the first man
in a given uniform still reads back; every later one is a memcpy. Kept across maps on purpose, bounded by
r_goreBaseCacheMB (192; 0 = off, the old behaviour) with least-recently-used eviction.
================
*/
#define GORE_BASECACHE_SLOTS 32

typedef struct {
    char  name[MAX_QPATH];
    int   w, h;
    byte *pixels;
    int   lastUse;
} goreBase_t;

static goreBase_t s_goreBase[GORE_BASECACHE_SLOTS];
static size_t     s_goreBaseBytes;
static int        s_goreBaseClock;
static int        s_goreBaseHits, s_goreBaseReads;

static void R_GoreBaseFree(goreBase_t *b)
{
    if (b->pixels) {
        s_goreBaseBytes -= (size_t)b->w * b->h * 4;
        ri.Free(b->pixels);
    }
    Com_Memset(b, 0, sizeof(*b));
}

static void R_GoreBasePixels(image_t *img, int level, int w, int h, byte *out)
{
    static cvar_t *capMB = NULL;
    size_t         need  = (size_t)w * h * 4, cap;
    goreBase_t    *b, *slot = NULL;
    int            i;

    if (!capMB) {
        capMB = ri.Cvar_Get("r_goreBaseCacheMB", "192", CVAR_ARCHIVE);
    }
    cap = capMB->integer > 0 ? (size_t)capMB->integer * 1024 * 1024 : 0;

    if (cap) {
        for (i = 0; i < GORE_BASECACHE_SLOTS; i++) {
            b = &s_goreBase[i];
            if (b->pixels && b->w == w && b->h == h && !Q_stricmp(b->name, img->imgName)) {
                Com_Memcpy(out, b->pixels, need);
                b->lastUse = ++s_goreBaseClock;
                s_goreBaseHits++;
                return;
            }
        }
    }

    {
        int t0 = ri.Milliseconds();

        R_GoreReadTexImage(img, level, out);
        s_goreBaseReads++;
        if (r_goreDebug && r_goreDebug->integer) {
            ri.Printf(PRINT_ALL, "^~^~^ GORE base readback %s %dx%d (mip %d) took %d ms (readbacks %d, cache hits %d, cache %d MB)\n",
                      img->imgName, w, h, level, ri.Milliseconds() - t0, s_goreBaseReads, s_goreBaseHits,
                      (int)(s_goreBaseBytes >> 20));
        }
    }
    if (!cap || need > cap) {
        return;
    }

    // make room: an empty slot, else the least recently used; then evict until the bytes fit
    for (i = 0; i < GORE_BASECACHE_SLOTS; i++) {
        b = &s_goreBase[i];
        if (!b->pixels) {
            slot = b;
            break;
        }
        if (!slot || b->lastUse < slot->lastUse) {
            slot = b;
        }
    }
    R_GoreBaseFree(slot);
    while (s_goreBaseBytes + need > cap) {
        goreBase_t *old = NULL;

        for (i = 0; i < GORE_BASECACHE_SLOTS; i++) {
            b = &s_goreBase[i];
            if (b->pixels && (!old || b->lastUse < old->lastUse)) {
                old = b;
            }
        }
        if (!old) {
            break;
        }
        R_GoreBaseFree(old);
    }
    slot->pixels = ri.Malloc(need);
    if (!slot->pixels) {
        return;
    }
    Com_Memcpy(slot->pixels, out, need);
    Q_strncpyz(slot->name, img->imgName, sizeof(slot->name));
    slot->w       = w;
    slot->h       = h;
    slot->lastUse = ++s_goreBaseClock;
    s_goreBaseBytes += need;
}

/*
================
R_GoreFreeInstance

Frees the CPU copy and clears the slot.  The GL image slot (instImage) is
kept for reuse; actual GL deletion happens via R_DeleteTextures at
shutdown/restart like every other image.
================
*/
static void R_GoreFreeInstance(goreInstance_t *inst)
{
    if (inst->pixels) {
        ri.Free(inst->pixels);
        inst->pixels = NULL;
    }
    if (inst->inuse) {
        inst->inuse = qfalse;
        s_goreActiveCount--;
    }
    inst->gpu          = qfalse;
    inst->entityNumber = -1;
    inst->baseImage    = NULL;
    inst->rebaseImage  = NULL;
    inst->family[0]    = 0;
    inst->numStamps    = 0;
    inst->stampHead    = 0;
    inst->width = inst->height = 0;
}

/*
================
R_GoreImageFamily

bug-754: the tier-1 gore skins bind a DIFFERENT image per damage tier
(diffuse.jpg -> diffuse_blood1.jpg -> diffuse_blood2[holes].tga, generated by
gen_gore_skins.py with mirrored paths).  Wounds are keyed per-entity per-image,
so a tier flip used to orphan every stamp.  This derives the tier-agnostic
family key: image name minus extension minus a trailing _bloodN suffix.
================
*/
static void R_GoreImageFamily(const char *imgName, char *out, int outSize)
{
    char  *dot;
    char  *slash;
    size_t len;

    Q_strncpyz(out, imgName, outSize);

    // strip the extension (last '.' after the last path separator)
    dot   = strrchr(out, '.');
    slash = strrchr(out, '/');
    if (dot && (!slash || dot > slash)) {
        *dot = 0;
    }

    len = strlen(out);
    if (len > 12 && !Q_stricmp(out + len - 12, "_blood2holes")) {
        out[len - 12] = 0;
    } else if (len > 7 && (!Q_stricmp(out + len - 7, "_blood1") || !Q_stricmp(out + len - 7, "_blood2"))) {
        out[len - 7] = 0;
    }
}

/*
================
R_GoreRebuildEntityFlags
================
*/
static void R_GoreRebuildEntityFlags(void)
{
    int i;

    Com_Memset(s_goreEntityHas, 0, sizeof(s_goreEntityHas));
    for (i = 0; i < GORE_MAX_INSTANCES; i++) {
        if (s_goreInstances[i].inuse && s_goreInstances[i].entityNumber >= 0
            && s_goreInstances[i].entityNumber < GORE_MAX_ENTNUM) {
            s_goreEntityHas[s_goreInstances[i].entityNumber] = 1;
        }
    }
}

/*
================
RE_GoreImpact

Exported to cgame: queue one server-authoritative bullet segment.  The end
point is where the server's bullet stopped; if that was on a character we
will find skinned triangles there next frame.  Cheap enough to call for
every bullet of every volley.
================
*/
void RE_GoreImpact(const vec3_t vStart, const vec3_t vEnd)
{
    int            i;
    gorePending_t *p;

    if (!r_goreUV || !r_goreUV->integer) {
        return;
    }
    if (!tr.registered) {
        return;
    }

    for (i = 0; i < GORE_MAX_PENDING; i++) {
        if (!s_gorePending[i].inuse) {
            break;
        }
    }
    if (i == GORE_MAX_PENDING) {
        if (r_goreDebug && r_goreDebug->integer) {
            ri.Printf(PRINT_ALL, "^~^~^ GORE queue FULL - impact dropped\n");
        }
        return; // queue full - drop (bounded by design)
    }
    if (r_goreDebug && r_goreDebug->integer) {
        ri.Printf(
            PRINT_ALL, "^~^~^ GORE queue[%d] seg (%.0f %.0f %.0f)->(%.0f %.0f %.0f)\n", i, vStart[0], vStart[1],
            vStart[2], vEnd[0], vEnd[1], vEnd[2]
        );
    }

    p = &s_gorePending[i];
    Com_Memset(p, 0, sizeof(*p));
    p->inuse = qtrue;
    p->msec  = ri.Milliseconds();
    VectorCopy(vStart, p->vStart);
    VectorCopy(vEnd, p->vEnd);
    p->bestEntityNumber = -1;
    s_gorePendingCount++;
}

/*
================
RE_GoreReset

Exported to cgame: entity came back to life (respawn / slot reuse /
revive) or otherwise needs a clean uniform - drop its wound instance(s).
================
*/
void RE_GoreReset(int entityNumber)
{
    int i;

    if (entityNumber < 0 || entityNumber >= GORE_MAX_ENTNUM) {
        return;
    }

    // bug-780: a respawn / slot reuse also cancels any pending killing-blow splash
    // (the flag can exist without instances, so clear it before the fast-out below)
    if (s_goreKillMs[entityNumber]) {
        s_goreKillMs[entityNumber] = 0;
        s_goreKillCount--;
    }

    if (!s_goreEntityHas[entityNumber]) {
        return;
    }

    for (i = 0; i < GORE_MAX_INSTANCES; i++) {
        if (s_goreInstances[i].inuse && s_goreInstances[i].entityNumber == entityNumber) {
            R_GoreFreeInstance(&s_goreInstances[i]);
        }
    }
    R_GoreRebuildEntityFlags();
}

// [user 2026-09-21] MP BULLET HOLES ON PLAYERS. The "no holes on players" gates below (entityNumber <
// MAX_CLIENTS) are a coop-only rule (bug-785): holes on your OWN body read wrong. In an MP session the user
// wants coop-style bullet holes on ENEMY players, living AND dead, so those gates are lifted when this is NOT
// a coop session. coop_isCoopSession is the cgame-published flag (1 in coop, 0 in MP/vanilla) - the client
// analog of the MP-session level var that CoopMpGoreSession keys the fgame wound-prop gate on. Returns qtrue
// when player holes should be allowed (i.e. not a coop session).
static qboolean R_GoreHolesOnPlayers(void)
{
    static cvar_t *s_coopSess = NULL;
    if (!s_coopSess) {
        s_coopSess = ri.Cvar_Get("coop_isCoopSession", "0", 0);
    }
    return (s_coopSess && !s_coopSess->integer) ? qtrue : qfalse;
}

/*
================
RE_GoreKillSplash

bug-780: exported to cgame - this entity just took its KILLING blow (EF_DEAD rising
edge; actors set the flag in Actor::HandleKilled, players always did).  Flag it; at end
of frame R_GoreCommitPending throws 2-3 blood-only splashes at random body UVs onto the
corpse's wound textures, so bodies read properly bloodied even on surfaces whose tier
skins never fire.  The flag waits up to GORE_KILL_LIFE_MS for the killing volley's own
impact to create a first instance, then expires silently.
================
*/
void RE_GoreKillSplash(int entityNumber)
{
    if (!r_goreUV || !r_goreUV->integer) {
        return;
    }
    if (!tr.registered) {
        return;
    }
    if (entityNumber < 0 || entityNumber >= GORE_MAX_ENTNUM) {
        return;
    }
    // HZM coop - no holes on players: the kill splash writes into the same UV wound
    // textures.  Players never own an instance (see R_GoreSkelSurfaceCheck gate), so
    // the flag would only idle for GORE_KILL_LIFE_MS and expire - skip it cleanly.
    // [user 2026-09-21] ...unless MP, where enemy players DO get holes (R_GoreHolesOnPlayers).
    if (entityNumber < MAX_CLIENTS && !R_GoreHolesOnPlayers()) {
        return;
    }

    if (!s_goreKillMs[entityNumber]) {
        s_goreKillCount++;
    }
    s_goreKillMs[entityNumber] = ri.Milliseconds();
    if (!s_goreKillMs[entityNumber]) {
        s_goreKillMs[entityNumber] = 1; // 0 means "no flag" - dodge a boot-time collision
    }
}

/*
================
R_GoreOverrideImage

Called by R_BindAnimatedImageToTMU just before binding a stage texture.  If
the entity being drawn owns a wound copy of exactly this image, bind the copy
instead.  Must be extremely cheap in the common (no gore anywhere) case.
================
*/
image_t *R_GoreOverrideImage(image_t *image)
{
    int i;
    int entityNumber;

    if (!s_goreActiveCount || !image) {
        return image;
    }
    if (!r_goreUV || !r_goreUV->integer) {
        return image;
    }
    if (!backEnd.currentEntity || backEnd.currentEntity == &tr.worldEntity) {
        return image;
    }
    if (backEnd.refdef.rdflags & RDF_NOWORLDMODEL) {
        return image; // UI / armory preview scenes keep clean textures
    }

    entityNumber = backEnd.currentEntity->e.entityNumber;
    if (entityNumber < 0 || entityNumber >= GORE_MAX_ENTNUM || !s_goreEntityHas[entityNumber]) {
        return image;
    }

    for (i = 0; i < GORE_MAX_INSTANCES; i++) {
        goreInstance_t *inst = &s_goreInstances[i];

        if (inst->inuse && inst->entityNumber == entityNumber && inst->baseImage == image
            && inst->instImage) {
            inst->lastUseFrame = tr.frameCount;
            return inst->instImage;
        }
    }

    // bug-754: no exact match - the surface may have TIER-SKIN-FLIPPED to a _bloodN variant
    // of an image we already carry wounds for (or flipped back).  Schedule an end-of-frame
    // rebase (readback of the new diffuse + stamp replay); this frame draws the clean variant.
    {
        char family[MAX_QPATH];

        R_GoreImageFamily(image->imgName, family, sizeof(family));
        for (i = 0; i < GORE_MAX_INSTANCES; i++) {
            goreInstance_t *inst = &s_goreInstances[i];

            if (inst->inuse && inst->entityNumber == entityNumber && inst->instImage
                && inst->numStamps && !Q_stricmp(inst->family, family)) {
                inst->rebaseImage  = image;
                s_goreRebasePending = qtrue;
                break;
            }
        }
    }
    return image;
}

/*
================
R_GoreShaderDiffuse

First non-lightmap, non-white stage image of the shader currently being
tessellated - the diffuse we would clone for this surface.
(gl2: stages live in shader->stages, gl1 calls them unfoggedStages)
================
*/
static image_t *R_GoreShaderDiffuse(shader_t *shader)
{
    int i;

    if (!shader) {
        return NULL;
    }
    for (i = 0; i < shader->numUnfoggedPasses && i < MAX_SHADER_STAGES; i++) {
        shaderStage_t *stage = shader->stages[i];
        image_t       *img;

        if (!stage) {
            break;
        }
        img = stage->bundle[0].image[0];
        if (img && !stage->bundle[0].isLightmap && img != tr.whiteImage && img != tr.dlightImage) {
            return img;
        }
    }
    return NULL;
}

/*
================
R_GoreDiffuseIsSkin

HZM coop (bloodier skin): decide, from the hit surface's diffuse image name, whether it is
EXPOSED SKIN (face / head / neck / bare hands) versus cloth / gear / helmet.  Renderer-only:
keys purely off image->imgName, which is the loaded diffuse texture PATH (e.g.
"textures/models/human/faces/dolf.tga").

Signals were VERIFIED against the full stock (AA+SH+BT) + mod + UBER-MODS texture corpus:

  * ".../faces/"  -> every stock head pack (young/old/afrika/winter, US + German) maps its
    head shader (dolf, slick, srg, us_north, ...) into a "faces/" folder that holds ONLY bare
    human faces - no gear, no eye/teeth/hair, no cloth.  This is the workhorse and covers the
    overwhelming majority of characters with zero false positives.

  * basename contains "hand"  -> bare-hand diffuses (handsnew, hand, handview, pt_hands,
    manon_hands).  The GLOVED "hand" surfaces bind knitgloves1 / l_gloves textures whose
    basenames contain no "hand", so a glove is never matched.

  * basename contains "head"  -> the few unique characters that keep the head outside /faces/
    (usmaps/head, frenchmaps/manon/manon_head, germanmaps/Frogman/Frogmanhead).  No gear or
    helmet basename in the corpus contains "head".

  * basename contains "face" but NOT "wrap"  -> *_face skins (germanmaps/wehrmact/wehrmact_face,
    germanmaps/face).  The "wrap" exclusion drops the afrika head WRAP (germanmaps/afrikakorps/
    DAkfacewrap.tga) - that is cloth over the face, i.e. NOT exposed skin.

Cloth / uniform diffuses (heer/wh_soldat, shutzshirt, ranger_jacket2, hbtpants, gear/*,
helmets, holsters, pouches) match none of the above and keep the unchanged wound size.
================
*/
static qboolean R_GoreDiffuseIsSkin(const image_t *diffuse)
{
    const char *name;
    const char *base;

    if (!diffuse) {
        return qfalse;
    }
    name = diffuse->imgName;
    if (!name || !name[0]) {
        return qfalse;
    }

    // face/head skin: the whole "faces/" folder is bare skin (see banner)
    if (Q_stristr(name, "/faces/")) {
        return qtrue;
    }

    // key the remaining tests off the file BASENAME so folder names never leak in
    base = strrchr(name, '/');
    base = base ? base + 1 : name;

    if (Q_stristr(base, "hand")) {
        return qtrue; // bare hands (gloves carry no "hand" in the basename)
    }
    if (Q_stristr(base, "head")) {
        return qtrue; // heads kept outside /faces/ (manon, frogman, usmaps/head)
    }
    if (Q_stristr(base, "face") && !Q_stristr(base, "wrap")) {
        return qtrue; // *_face skins; the afrika head WRAP (cloth) is excluded
    }

    return qfalse;
}

/*
================
R_GoreDiffuseIsHeadgear

HZM coop (headshot feedback): TRUE when the hit surface's diffuse is WORN HEADGEAR - a helmet,
officer/NCO cap or soft hat.  These surfaces are HIDDEN when the engine pops the helmet on a
killing head hit (sethelmet), and any UV stamp painted on them vanishes with the surface - the
"confirmed headshot but no wound on the corpse" case.  A headgear hit therefore additionally
stamps the nearest exposed-skin vertex (the face under the rim), which survives the pop.

Signals verified against the full stock (AA+SH+BT) + mod + HRRTM texture corpus (2026-07-28):
every worn-headgear diffuse basename contains "helm" (heerhelmet, camohelm, ushelmet, tankhelm,
winterhelmet, splinterhelmet, kmarrine_helmet, ...), "hat" (officer_hat, dakhat, ncohat_ss_*,
gestapohat, jager_hat, ...) or "cap" (field_cap, sidecap_*, kmcap, ssblackcap, ...).  This test
is only reached for skinned CHARACTER surfaces, so world textures (joistcap, snowcap) and
vehicle parts (hubcap) can never arrive here.  The US cap-insignia patches (army_ltnt_cap) can
match; at worst a rare insignia graze also paints nearby skin - acceptable.  Ordering: the
caller resolves skin FIRST, so a faces/-folder texture (e.g. faces/adhelm) stays skin.
================
*/
static qboolean R_GoreDiffuseIsHeadgear(const image_t *diffuse)
{
    const char *name;
    const char *base;

    if (!diffuse) {
        return qfalse;
    }
    name = diffuse->imgName;
    if (!name || !name[0]) {
        return qfalse;
    }

    base = strrchr(name, '/');
    base = base ? base + 1 : name;

    if (Q_stristr(base, "helm") || Q_stristr(base, "hat") || Q_stristr(base, "cap")) {
        return qtrue;
    }
    return qfalse;
}

/*
================
R_GoreSkinScale

HZM coop (bloodier skin): the exposed-skin wound size multiplier, read from the
coop_goreSkinWoundScale cvar and clamped to a sane range.  1.0 = feature off (wounds identical
to cloth).  Bigger = a larger hole stamp and a proportionally larger blood-splash halo; the
dark #150200 palette is never touched, so "bloodier" means MORE blood area, not brighter/redder.
================
*/
static float R_GoreSkinScale(void)
{
    float s;

    if (!coop_goreSkinWoundScale) {
        return 1.0f;
    }
    s = coop_goreSkinWoundScale->value;
    if (s < 1.0f) {
        s = 1.0f;
    }
    if (s > 2.5f) {
        s = 2.5f;
    }
    return s;
}

/*
================
R_GoreSkelSurfaceCheck

Called at the tail of RB_SkelMesh with the tess ranges the surface just
filled.  Positions in tess.xyz are MODEL-space (entity rotation/origin are
applied by the GL modelview), so the pending world segments are transformed
into model space and Moller-Trumbore tested against the skinned triangles.
The best (first-along-ray, near the bullet stop point) hit is remembered on
the pending impact; the stamp itself is applied later in
R_GoreCommitPending once every surface of the frame had its chance.
(gl2: tess.texCoords is the single-bundle vec2_t array)
================
*/
void R_GoreSkelSurfaceCheck(int baseVertex, int baseIndex)
{
    trRefEntity_t *ent;
    image_t       *diffuse;
    int            iPending;
    int            entityNumber;
    int            numIndexes;
    vec3_t         localStart, localEnd, localDir;
    vec3_t         tmp;
    float          segLen, sMax;
    qboolean       isSkin;     // HZM coop (bloodier skin): this surface is exposed skin
    qboolean       isHeadgear; // HZM coop (headshot feedback): this surface is worn headgear

    if (!s_gorePendingCount) {
        return;
    }
    if (!r_goreUV || !r_goreUV->integer) {
        return;
    }
    if (backEnd.refdef.rdflags & RDF_NOWORLDMODEL) {
        return; // never wound armory / UI preview models
    }

    ent = backEnd.currentEntity;
    if (!ent || ent == &tr.worldEntity) {
        return;
    }
    // NOTE: the "bleedable humans only" gate (TIKI ischaracter) is applied by
    // the caller in RB_SkelMesh, which has the complete tiki type in scope.
    if (ent->e.customShader) {
        return; // entity drawn with an override/effect shader - do not clone it
    }
    entityNumber = ent->e.entityNumber;
    if (entityNumber < 0 || entityNumber >= GORE_MAX_ENTNUM) {
        return;
    }
    // HZM coop - no holes on players: never UV-stamp wounds onto PLAYER entities
    // (entityNumber < MAX_CLIENTS).  This is the single choke point every stamp
    // rides through (a pending can only pick an entity here), so players never
    // acquire a gore instance at all.  AI / allied-AI stamping, players' blood
    // drips (fgame bouncedecals) and the gore skin tiers are all untouched.
    // [user 2026-09-21] ...unless MP: enemy players get coop-style bullet holes, living and dead. This is
    // the single choke point, so lifting it here (not coop -> R_GoreHolesOnPlayers) is what actually gives
    // MP players wound stamps; the fgame CoopGoreTryWoundProp gate is lifted in parallel for the 3D props.
    if (entityNumber < MAX_CLIENTS && !R_GoreHolesOnPlayers()) {
        return;
    }

    numIndexes = tess.numIndexes;
    if (baseIndex >= numIndexes || baseVertex >= tess.numVertexes) {
        return;
    }

    diffuse = R_GoreShaderDiffuse(tess.shader);
    if (!diffuse) {
        return; // no paintable diffuse -> silently skip (atlas-less/custom shader)
    }
    // HZM coop (bloodier skin): skin-vs-cloth is a property of the surface diffuse, so resolve
    // it once here (constant for every pending segment tested against this surface).
    isSkin = R_GoreDiffuseIsSkin(diffuse);
    // HZM coop (headshot feedback): skin classification wins (faces/adhelm stays skin)
    isHeadgear = !isSkin && R_GoreDiffuseIsHeadgear(diffuse);

    for (iPending = 0; iPending < GORE_MAX_PENDING; iPending++) {
        gorePending_t *p = &s_gorePending[iPending];
        int            i;

        if (!p->inuse) {
            continue;
        }

        // quick world-space reject: bullet stop point must be near this entity
        VectorSubtract(p->vEnd, ent->e.origin, tmp);
        if (VectorLengthSquared(tmp) > 200.0f * 200.0f) {
            continue;
        }

        // world -> model space (entity axis is orthonormal for characters;
        // model scale is already baked into the skinned verts)
        VectorSubtract(p->vStart, ent->e.origin, tmp);
        localStart[0] = DotProduct(tmp, ent->e.axis[0]);
        localStart[1] = DotProduct(tmp, ent->e.axis[1]);
        localStart[2] = DotProduct(tmp, ent->e.axis[2]);

        VectorSubtract(p->vEnd, ent->e.origin, tmp);
        localEnd[0] = DotProduct(tmp, ent->e.axis[0]);
        localEnd[1] = DotProduct(tmp, ent->e.axis[1]);
        localEnd[2] = DotProduct(tmp, ent->e.axis[2]);

        // HZM coop (skin-snap fallback): the exact Moller-Trumbore test below ray-tests the server
        // segment against the CLIENT's interpolated/animated pose.  On a static corpse the two poses
        // match so the ray hits; on a MOVING enemy the rendered pose is offset ~10-25u, so the small
        // exposed-skin surfaces (head/hands ~8-12u) slide out of the ray path and only the big torso
        // still catches it (cloth wound appears, face/hand wound does not).  For SKIN surfaces ONLY,
        // remember the skin VERTEX nearest the bullet stop point (localEnd).  If the exact ray finds
        // nothing on this enemy this frame, R_GoreCommitPending stamps that nearest skin vertex.
        // Cloth is never tracked, so a torso spray (vEnd ~20-40u from the face) can't false-stamp
        // skin; exact hits are handled below and ALWAYS win.  Respects every reject guard already
        // applied above (r_goreUV, entity range, players entityNumber<MAX_CLIENTS, 200u quick reject).
        if (isSkin && coop_goreSkinSnap && coop_goreSkinSnap->integer) {
            float snapTol = GORE_SKIN_SNAP_DIST;
            int   iv;

            if (coop_goreSkinSnapDist) {
                snapTol = coop_goreSkinSnapDist->value;
                if (snapTol < 8.0f) {
                    snapTol = 8.0f;
                }
                // bug-905: ceiling raised 40 -> 64 so fast strafers/turners (whose head can sit
                // >40u from the server stop point) can still be covered by cranking the cvar.
                if (snapTol > 64.0f) {
                    snapTol = 64.0f;
                }
            }

            for (iv = baseVertex; iv < tess.numVertexes; iv++) {
                const float *vp = tess.xyz[iv];
                const float *tc;
                vec3_t       d;
                float        dist, su, sv;

                VectorSubtract(vp, localEnd, d);
                dist = VectorLength(d);
                if (dist > snapTol) {
                    continue;
                }
                if (p->hasSnap && dist >= p->snapDist) {
                    continue; // keep the SMALLEST distance across all skin surfaces this frame
                }

                tc = tess.texCoords[iv];
                su = tc[0];
                sv = tc[1];
                // same UV sanity as the exact path (reject NaN / hugely tiled coords)
                if (su != su || sv != sv || su < -8.0f || su > 8.0f || sv < -8.0f || sv > 8.0f) {
                    continue;
                }

                p->hasSnap          = qtrue;
                p->snapDist         = dist;
                p->snapU            = su;
                p->snapV            = sv;
                p->snapEntityNumber = entityNumber;
                p->snapImage        = diffuse;
                p->snapHasDown      = qfalse; // simple fallback: trickle uses a random roll
                p->snapDownAngle    = 0.0f;
            }
        }

        VectorSubtract(localEnd, localStart, localDir);
        segLen = VectorLength(localDir);
        if (segLen < 1.0f) {
            continue;
        }
        // allow hits slightly past the reported stop point: the server's
        // location-damage spheres are a coarse hull around the visual mesh
        sMax = 1.0f + GORE_SEGMENT_EXTEND / segLen;

        for (i = baseIndex; i + 2 < numIndexes; i += 3) {
            const float *v0 = tess.xyz[tess.indexes[i]];
            const float *v1 = tess.xyz[tess.indexes[i + 1]];
            const float *v2 = tess.xyz[tess.indexes[i + 2]];
            vec3_t       e1, e2, pv, tv, qv;
            float        det, invDet, u, v, s;
            vec3_t       hit;
            float        distToEnd;

            // Moller-Trumbore, double-sided
            VectorSubtract(v1, v0, e1);
            VectorSubtract(v2, v0, e2);
            CrossProduct(localDir, e2, pv);
            det = DotProduct(e1, pv);
            if (det > -0.000001f && det < 0.000001f) {
                continue;
            }
            invDet = 1.0f / det;
            VectorSubtract(localStart, v0, tv);
            u = DotProduct(tv, pv) * invDet;
            if (u < 0.0f || u > 1.0f) {
                continue;
            }
            CrossProduct(tv, e1, qv);
            v = DotProduct(localDir, qv) * invDet;
            if (v < 0.0f || u + v > 1.0f) {
                continue;
            }
            s = DotProduct(e2, qv) * invDet;
            if (s < 0.0f || s > sMax) {
                continue;
            }

            VectorMA(localStart, s, localDir, hit);
            VectorSubtract(hit, localEnd, tmp);
            distToEnd = VectorLength(tmp);
            if (distToEnd > GORE_NEAR_END_DIST) {
                continue; // triangle on the ray but not where the bullet stopped
            }

            // prefer the entry-side (first along the ray) hit
            if (p->hasHit && s >= p->bestS) {
                continue;
            }

            {
                // barycentric-interpolated diffuse UV of the impact
                const float *t0 = tess.texCoords[tess.indexes[i]];
                const float *t1 = tess.texCoords[tess.indexes[i + 1]];
                const float *t2 = tess.texCoords[tess.indexes[i + 2]];
                float        su = t0[0] + u * (t1[0] - t0[0]) + v * (t2[0] - t0[0]);
                float        sv = t0[1] + u * (t1[1] - t0[1]) + v * (t2[1] - t0[1]);

                // sanity: reject degenerate/hugely tiled coords
                if (su != su || sv != sv || su < -8.0f || su > 8.0f || sv < -8.0f || sv > 8.0f) {
                    continue;
                }

                p->hasHit           = qtrue;
                p->bestS            = s;
                p->bestDistToEnd    = distToEnd;
                p->bestU            = su;
                p->bestV            = sv;
                p->bestEntityNumber = entityNumber;
                p->bestImage        = diffuse;
                p->bestSkin         = isSkin;     // HZM coop (bloodier skin)
                p->bestHeadgear     = isHeadgear; // HZM coop (headshot feedback)

                // bug-829: gravity-orient the wound so its trickle tail runs DOWN the body. World-down
                // (0,0,-1) -> model space via the entity axis (same convention as the segment above).
                // Solve gModel ~= a*e1 + b*e2 in the triangle plane (2x2 Gram system), then map (a,b)
                // through the same barycentric weights into UV space; convert to texel space (aspect)
                // and to a stamp angle. R_GoreStampLayer maps art +Y to instance dir (sin,cos), so the
                // trickle (baked pointing +Y) aligns to gravity when angle = atan2(gx, gy).
                {
                    vec3_t gModel;
                    float  g11, g12, g22, ge1, ge2, gdet;

                    gModel[0] = -ent->e.axis[0][2];
                    gModel[1] = -ent->e.axis[1][2];
                    gModel[2] = -ent->e.axis[2][2];

                    g11  = DotProduct(e1, e1);
                    g12  = DotProduct(e1, e2);
                    g22  = DotProduct(e2, e2);
                    ge1  = DotProduct(e1, gModel);
                    ge2  = DotProduct(e2, gModel);
                    gdet = g11 * g22 - g12 * g12;
                    if (gdet > 1e-9f || gdet < -1e-9f) {
                        float a  = (ge1 * g22 - ge2 * g12) / gdet;
                        float b  = (g11 * ge2 - g12 * ge1) / gdet;
                        float du = a * (t1[0] - t0[0]) + b * (t2[0] - t0[0]);
                        float dv = a * (t1[1] - t0[1]) + b * (t2[1] - t0[1]);
                        float gx = du * (float)diffuse->uploadWidth;
                        float gy = dv * (float)diffuse->uploadHeight;
                        if ((gx * gx + gy * gy) > 1e-12f) {
                            p->bestDownAngle = (float)atan2(gx, gy);
                            p->bestHasDown   = qtrue;
                        } else {
                            p->bestHasDown = qfalse;
                        }
                    } else {
                        p->bestHasDown = qfalse;
                    }
                }
            }
        }
    }
}

/*
================
R_GoreLoadStamp

Load the authored wound stamp from the mod pk3
(textures/coop_gore/coop_woundstamp.tga, generated by the PIL pipeline).
If it is missing, synthesize a simple hole-core + #150200 halo so the
feature still works without the asset.
================
*/
static void R_GoreLoadStamp(void)
{
    byte *pic    = NULL;
    int   width  = 0;
    int   height = 0;

    if (s_stampTried) {
        return;
    }
    s_stampTried   = qtrue;
    s_goreArtDirty = qtrue; // the GPU compositor's copies of the two layers are stale now

    // R_LoadImage silently prefers a same-name .dds when S3TC is on and would
    // hand us COMPRESSED bytes (no format info from this API) - if anything
    // shadows the stamp with a .dds, use the procedural fallback instead of
    // overreading a compressed buffer.
    if (!ri.FS_FileExists("textures/coop_gore/coop_woundstamp.dds")
        && R_LoadRawImage("textures/coop_gore/coop_woundstamp.tga", &pic, &width, &height)) {
        if (width > 0 && height > 0 && width <= 256 && height <= 256) {
            s_stampPix = ri.Malloc(width * height * 4);
            Com_Memcpy(s_stampPix, pic, width * height * 4);
            s_stampW = width;
            s_stampH = height;
        }
        R_FreeRawImage(pic);
    }

    if (!s_stampPix) {
        // procedural fallback (bug-735 palette): dark hole core, bright arterial hot ring so the wound
        // reads on dark cloth, #150200 mid halo, dried rim
        int x, y;

        s_stampW = s_stampH = GORE_STAMP_FALLBACK;
        s_stampPix          = ri.Malloc(s_stampW * s_stampH * 4);
        for (y = 0; y < s_stampH; y++) {
            for (x = 0; x < s_stampW; x++) {
                float dx = (x + 0.5f) / s_stampW - 0.5f;
                float dy = (y + 0.5f) / s_stampH - 0.5f;
                float d  = 2.0f * (float)sqrt(dx * dx + dy * dy); // 0 center .. 1 edge
                byte *px = s_stampPix + (y * s_stampW + x) * 4;

                if (d < 0.22f) {
                    px[0] = 8; // hole core - almost black, still warm
                    px[1] = 1;
                    px[2] = 0;
                    px[3] = 255;
                } else if (d < 0.48f) {
                    float f = (d - 0.22f) / 0.26f; // hot wet ring -> base
                    px[0]   = (byte)(GORE_HOT_R + f * (GORE_BLOOD_R - GORE_HOT_R));
                    px[1]   = (byte)(GORE_HOT_G + f * (GORE_BLOOD_G - GORE_HOT_G));
                    px[2]   = (byte)(GORE_HOT_B + f * (GORE_BLOOD_B - GORE_HOT_B));
                    px[3]   = 250;
                } else if (d < 1.0f) {
                    float f = (d - 0.48f) / 0.52f; // base -> dried rim, fading out
                    px[0]   = (byte)(GORE_BLOOD_R + f * (GORE_DRIED_R - GORE_BLOOD_R));
                    px[1]   = (byte)(GORE_BLOOD_G + f * (GORE_DRIED_G - GORE_BLOOD_G));
                    px[2]   = (byte)(GORE_BLOOD_B + f * (GORE_DRIED_B - GORE_BLOOD_B));
                    px[3]   = (byte)(220.0f * (1.0f - f * f));
                } else {
                    px[0] = px[1] = px[2] = px[3] = 0;
                }

                // bug-829: modest downward trickle tail. Art +Y is aligned to gravity by the stamp
                // angle, so a leak baked at +dy runs down-body. Narrow, tapering, fading, #150200.
                {
                    float adx = dx < 0.0f ? -dx : dx;
                    float tl  = 0.46f;                       // length below the hole
                    float tw  = 0.05f;                       // half-width at the top
                    if (dy > 0.0f && dy < tl) {
                        float fl = dy / tl;                  // 0 at hole .. 1 at tail end
                        float wcur = tw * (1.0f - 0.55f * fl);
                        if (adx < wcur) {
                            float ta = 205.0f * (1.0f - fl) * (1.0f - adx / (wcur + 1e-4f));
                            if (ta > (float)px[3]) {
                                px[0] = (byte)(GORE_BLOOD_R + fl * (GORE_DRIED_R - GORE_BLOOD_R));
                                px[1] = (byte)(GORE_BLOOD_G + fl * (GORE_DRIED_G - GORE_BLOOD_G));
                                px[2] = (byte)(GORE_BLOOD_B + fl * (GORE_DRIED_B - GORE_BLOOD_B));
                                px[3] = (byte)ta;
                            }
                        }
                    }
                }
            }
        }
    }

    // bug-780: the blood-splash blot (halo layer + killing-blow spatter). Authored art
    // first (textures/coop_gore/coop_bloodsplash.tga, gen_woundstamp.py), procedural
    // irregular fallback otherwise. Same .dds-shadow guard as the stamp above.
    pic    = NULL;
    width  = 0;
    height = 0;
    if (!ri.FS_FileExists("textures/coop_gore/coop_bloodsplash.dds")
        && R_LoadRawImage("textures/coop_gore/coop_bloodsplash.tga", &pic, &width, &height)) {
        if (width > 0 && height > 0 && width <= 256 && height <= 256) {
            s_splashPix = ri.Malloc(width * height * 4);
            Com_Memcpy(s_splashPix, pic, width * height * 4);
            s_splashW = width;
            s_splashH = height;
        }
        R_FreeRawImage(pic);
    }

    if (!s_splashPix) {
        // procedural fallback: soft irregular blot - #150200 body, brighter wet core,
        // lobed rim (three low-frequency sines) with a long alpha falloff, NO hole.
        int   x, y;
        float lobe0 = R_GoreRand01() * 6.2831853f;
        float lobe1 = R_GoreRand01() * 6.2831853f;
        float lobe2 = R_GoreRand01() * 6.2831853f;

        s_splashW = s_splashH = 96;
        s_splashPix           = ri.Malloc(s_splashW * s_splashH * 4);
        for (y = 0; y < s_splashH; y++) {
            for (x = 0; x < s_splashW; x++) {
                float dx  = (x + 0.5f) / s_splashW - 0.5f;
                float dy  = (y + 0.5f) / s_splashH - 0.5f;
                float d   = 2.0f * (float)sqrt(dx * dx + dy * dy); // 0 center .. 1 edge
                float th  = (float)atan2(dy, dx);
                float rim = 0.62f + 0.14f * (float)sin(2.0f * th + lobe0)
                          + 0.11f * (float)sin(3.0f * th + lobe1)
                          + 0.07f * (float)sin(5.0f * th + lobe2);
                byte *px = s_splashPix + (y * s_splashW + x) * 4;

                if (d < rim) {
                    float f   = d / rim;                                     // 0 center .. 1 rim
                    float wet = (f < 0.30f) ? (1.0f - f / 0.30f) : 0.0f;     // wet-core weight
                    px[0]     = (byte)(GORE_BLOOD_R + wet * (GORE_HOT_R - GORE_BLOOD_R));
                    px[1]     = (byte)(GORE_BLOOD_G + wet * (GORE_HOT_G - GORE_BLOOD_G));
                    px[2]     = (byte)(GORE_BLOOD_B + wet * (GORE_HOT_B - GORE_BLOOD_B));
                    px[3]     = (byte)(185.0f * (1.0f - f * f));             // soft falloff
                } else {
                    px[0] = px[1] = px[2] = px[3] = 0;
                }
            }
        }
    }
}

/*
=============================================================================
GPU COMPOSITOR (r_goreGpu)                      [user 2026-10-11, bug-3493 "the full fix"]

The CPU path gives every wounded body part a private RGBA copy of its diffuse: read the whole texture back from
the GPU, composite into the copy, upload it. On an HD skin (2048x2048 = 16 MB) that readback alone is ~31 ms, and
it was 52 % of every slow frame of a firefight. Here nothing ever leaves the GPU:

  base   the instance texture is an FBO colour attachment; one quad copies the base diffuse into it, texel for
         texel (texelFetch - compressed textures cannot be attached or glCopyImageSubData'd into RGBA8, and
         sampling is exactly what every unwounded soldier's skin goes through anyway).
  stamp  one quad per art layer, covering exactly the texels R_GoreStampLayer's loop visits. The fragment shader
         IS that loop body: the same inverse rotation into art space, the same truncating nearest sample, the same
         integer blend (dst*(255-a) + src*a)/255 with the destination alpha kept. The destination is read from a
         scratch copy of the rectangle (blitted just before the draw), so there is no feedback loop and no
         reliance on fixed-function blending (which rounds where the CPU truncates).
  wrap   a stamp that crosses a texture edge is drawn once per tile it touches, in the CPU loop's order
         (rows outer, columns inner), so even a stamp larger than the texture overlaps itself the same way.
  rebase / replay   redraw the new base, then draw the recorded stamps again.

All of the wound LOGIC (what gets stamped, where, how big, which instance is stolen) is the code below, shared by
both paths; only "write these texels" differs. r_goreGpu: 0 = CPU path (unchanged, and the fallback whenever the
GL features are missing or the base is an sRGB texture), 1 = GPU, 2 = GPU plus a CPU reference copy that every
pass is read back and compared against (a test mode: it brings the readbacks back). Changing the cvar rebuilds
the live instances from their stamp records in the other mode, which is what makes same-frame A/B stills possible.

GL state: R_GoreCommitPending runs from RE_EndFrame after the back end has swapped. Everything goes through the
caches the renderer keeps (tr_dsa.c for textures / program / framebuffers, GL_State, GL_Cull); what has no cache
(viewport, scissor box and enable, dither, colour mask, vertex array) is saved in R_GoreGpuBegin and put back in
R_GoreGpuEnd. The FBOs, programs and the vertex array are this file's own raw GL objects (deleted in
R_GoreShutdown); the textures are ordinary image_t slots so R_DeleteTextures owns them.
=============================================================================
*/
#define GORE_GPU_SCRATCH 1024 // side of the scratch copy; larger stamp rectangles are drawn in chunks

typedef struct {
    qboolean tried, ok;
    qboolean precise;                // the stamp shader carries GLSL 4 `precise` (no fused multiply-add)
    GLuint   fboInst, fboScratch, vao;
    GLuint   progBase, progStamp;
    GLint    uBaseP, uStampOrg, uStampRot, uStampArt;
    image_t *scratch;
    image_t *art[2];                 // 0 = bullet-hole stamp, 1 = blood splash
    GLuint   attached;               // texture currently on fboInst's colour attachment
    // state bracket
    qboolean  active;
    GLint     vp[4], sc[4];
    GLboolean scOn, dither, mask[4];
    uint32_t  stateBits;
    int       cull;
    FBO_t    *fbo;
    vao_t    *oldVao;
    // counters (r_goreDebug)
    int draws, blits, fills, glErrors, preErrors;
    int vChecks, vBadTexels, vMaxDiff, vBaseBadTexels, vBaseMaxDiff;
} goreGpu_t;

static goreGpu_t s_gg;

static const char *s_goreGpuVS =
    "void main()\n"
    "{\n"
    "	vec2 c = vec2(float(gl_VertexID & 1), float((gl_VertexID >> 1) & 1));\n"
    "	gl_Position = vec4(c * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";

// the viewport is the target rectangle, so gl_FragCoord is the destination texel (+0.5)
static const char *s_goreGpuBaseFS =
    "uniform sampler2D u_Tex;\n"
    "uniform vec4 u_P;\n" // x = mip level
    "out vec4 out_Color;\n"
    "void main()\n"
    "{\n"
    "	out_Color = texelFetch(u_Tex, ivec2(gl_FragCoord.xy), int(u_P.x));\n"
    "}\n";

// R_GoreStampLayer's loop body. u_Org.xy = the stamp centre in this tile's texel coordinates, u_Org.zw = the
// origin of the rectangle held in u_Dst; u_Rot = cos, sin, art texels per destination texel; u_ArtSize = w, h.
static const char *s_goreGpuStampFS =
    "uniform sampler2D u_Dst;\n"
    "uniform sampler2D u_Art;\n"
    "uniform vec4 u_Org;\n"
    "uniform vec4 u_Rot;\n"
    "uniform vec4 u_ArtSize;\n"
    "out vec4 out_Color;\n"
    "void main()\n"
    "{\n"
    "	ivec2 t = ivec2(gl_FragCoord.xy);\n"
    "	float dx = float(t.x - int(u_Org.x));\n"
    "	float dy = float(t.y - int(u_Org.y));\n"
    "	GORE_PRECISE float fx = (dx * u_Rot.x - dy * u_Rot.y) * u_Rot.z + u_ArtSize.x * 0.5;\n"
    "	GORE_PRECISE float fy = (dx * u_Rot.y + dy * u_Rot.x) * u_Rot.z + u_ArtSize.y * 0.5;\n"
    "	ivec2 s = ivec2(int(fx), int(fy));\n"
    "	if (s.x < 0 || s.y < 0 || s.x >= int(u_ArtSize.x) || s.y >= int(u_ArtSize.y)) discard;\n"
    "	ivec4 a = ivec4(texelFetch(u_Art, s, 0) * 255.0 + 0.5);\n"
    "	if (a.a == 0) discard;\n"
    "	vec4 d = texelFetch(u_Dst, t - ivec2(int(u_Org.z), int(u_Org.w)), 0);\n"
    "	ivec3 di = ivec3(d.rgb * 255.0 + 0.5);\n"
    "	ivec3 o = (di * (255 - a.a) + a.rgb * a.a) / 255;\n"
    "	out_Color = vec4(vec3(o) / 255.0, d.a);\n"
    "}\n";

static GLuint R_GoreGpuCompile(GLenum type, const char *header, const char *body, qboolean quiet)
{
    const GLchar *src[2];
    GLuint        sh;
    GLint         okay = 0;

    sh = qglCreateShader(type);
    if (!sh) {
        return 0;
    }
    src[0] = header;
    src[1] = body;
    qglShaderSource(sh, 2, src, NULL);
    qglCompileShader(sh);
    qglGetShaderiv(sh, GL_COMPILE_STATUS, &okay);
    if (!okay) {
        char log[1024];

        log[0] = 0;
        qglGetShaderInfoLog(sh, sizeof(log) - 1, NULL, log);
        log[sizeof(log) - 1] = 0;
        if (!quiet) {
            ri.Printf(PRINT_WARNING, "^~^~^ GORE gpu: shader compile failed (%s): %s\n", header, log);
        }
        qglDeleteShader(sh);
        return 0;
    }
    return sh;
}

static GLuint R_GoreGpuLink(const char *header, const char *fsBody, qboolean quiet)
{
    GLuint vs, fs, prog;
    GLint  okay = 0;

    vs = R_GoreGpuCompile(GL_VERTEX_SHADER, header, s_goreGpuVS, quiet);
    if (!vs) {
        return 0;
    }
    fs = R_GoreGpuCompile(GL_FRAGMENT_SHADER, header, fsBody, quiet);
    if (!fs) {
        qglDeleteShader(vs);
        return 0;
    }
    prog = qglCreateProgram();
    qglAttachShader(prog, vs);
    qglAttachShader(prog, fs);
    qglLinkProgram(prog);
    qglGetProgramiv(prog, GL_LINK_STATUS, &okay);
    qglDetachShader(prog, vs);
    qglDetachShader(prog, fs);
    qglDeleteShader(vs);
    qglDeleteShader(fs);
    if (!okay) {
        char log[1024];

        log[0] = 0;
        qglGetProgramInfoLog(prog, sizeof(log) - 1, NULL, log);
        log[sizeof(log) - 1] = 0;
        ri.Printf(PRINT_WARNING, "^~^~^ GORE gpu: program link failed: %s\n", log);
        qglDeleteProgram(prog);
        return 0;
    }
    return prog;
}

/*
================
R_GoreGpuBegin / R_GoreGpuEnd

The state bracket: the first GPU pass of a commit opens it, the end of R_GoreCommitPending closes it.
================
*/
static void R_GoreGpuBegin(void)
{
    if (s_gg.active) {
        return;
    }
    s_gg.active = qtrue;

    if (r_goreDebug && r_goreDebug->integer) {
        // whatever is in the error queue now is not ours - count it apart
        int guard = 0;

        while (qglGetError() != GL_NO_ERROR && guard++ < 16) {
            s_gg.preErrors++;
        }
    }

    qglGetIntegerv(GL_VIEWPORT, s_gg.vp);
    qglGetIntegerv(GL_SCISSOR_BOX, s_gg.sc);
    qglGetBooleanv(GL_SCISSOR_TEST, &s_gg.scOn);
    qglGetBooleanv(GL_DITHER, &s_gg.dither);
    qglGetBooleanv(GL_COLOR_WRITEMASK, s_gg.mask);
    s_gg.stateBits = glState.glStateBits;
    s_gg.cull      = glState.faceCulling;
    s_gg.fbo       = glState.currentFBO;
    s_gg.oldVao    = glState.currentVao;

    qglDisable(GL_SCISSOR_TEST); // it clips both draws and blits
    qglDisable(GL_DITHER);
    qglColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    GL_State(GLS_DEPTHTEST_DISABLE); // no depth test or write, no blend, filled polygons
    GL_Cull(CT_TWO_SIDED);
}

static void R_GoreGpuEnd(void)
{
    if (!s_gg.active) {
        return;
    }
    s_gg.active = qfalse;

    GL_BindFramebuffer(GL_FRAMEBUFFER, s_gg.fbo ? s_gg.fbo->frameBuffer : 0);
    qglViewport(s_gg.vp[0], s_gg.vp[1], s_gg.vp[2], s_gg.vp[3]);
    qglScissor(s_gg.sc[0], s_gg.sc[1], s_gg.sc[2], s_gg.sc[3]);
    if (s_gg.scOn) {
        qglEnable(GL_SCISSOR_TEST);
    }
    if (s_gg.dither) {
        qglEnable(GL_DITHER);
    }
    qglColorMask(s_gg.mask[0], s_gg.mask[1], s_gg.mask[2], s_gg.mask[3]);
    GL_State(s_gg.stateBits);
    GL_Cull(s_gg.cull);
    GL_BindNullProgram();

    // the vertex array: what R_BindVao does for the array the renderer believes is bound
    qglBindVertexArray(s_gg.oldVao ? s_gg.oldVao->vao : 0);
    if (s_gg.oldVao && (glRefConfig.intelGraphics || s_gg.oldVao == tess.vao)) {
        qglBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s_gg.oldVao->indexesIBO);
    }

    if (r_goreDebug && r_goreDebug->integer) {
        int err, guard = 0;

        while ((err = qglGetError()) != GL_NO_ERROR && guard++ < 16) {
            s_gg.glErrors++;
            ri.Printf(PRINT_ALL, "^~^~^ GORE gpu GL ERROR 0x%x after the gore passes\n", err);
        }
        ri.Printf(
            PRINT_ALL, "^~^~^ GORE gpu frame %d: totals fills %d draws %d blits %d, GL errors %d (before our passes %d)\n",
            tr.frameCount, s_gg.fills, s_gg.draws, s_gg.blits, s_gg.glErrors, s_gg.preErrors
        );
    }
}

/*
================
R_GoreGpuInit

Lazily build the programs, the two framebuffers, the vertex array and the scratch / art texture slots.
Returns qfalse (for good, until the renderer restarts) when the GL features are missing - the CPU path then
does the work exactly as before.
================
*/
static qboolean R_GoreGpuInit(void)
{
    static byte dummy[16 * 16 * 4];
    const char *why = NULL;
    int         maj = glRefConfig.glslMajorVersion, min = glRefConfig.glslMinorVersion;
    int         i;

    if (s_gg.tried) {
        return s_gg.ok;
    }
    s_gg.tried = qtrue;
    s_gg.ok    = qfalse;

    if (qglesMajorVersion) {
        why = "OpenGL ES";
    } else if (!glRefConfig.framebufferObject || !glRefConfig.framebufferBlit) {
        why = "no framebuffer objects / blit";
    } else if (!glRefConfig.vertexArrayObject) {
        why = "no vertex array objects";
    } else if (maj < 1 || (maj == 1 && min < 30)) {
        why = "GLSL older than 1.30";
    }
    if (why) {
        ri.Printf(PRINT_ALL, "^~^~^ GORE gpu: unavailable (%s) - using the CPU compositor\n", why);
        return qfalse;
    }

    R_GoreGpuBegin();

    // `precise` (GLSL 4.00, or GL_ARB_gpu_shader5 on a 1.50 context): the art-space coordinates are then computed
    // with the CPU's exact sequence of float operations (no fused multiply-add), so the nearest sample can never
    // land one art texel aside. Without it the shader still compiles; r_goreGpu 2 measures what that costs.
    if (maj >= 4) {
        s_gg.progStamp = R_GoreGpuLink("#version 400\n#define GORE_PRECISE precise\n", s_goreGpuStampFS, qtrue);
    }
    if (!s_gg.progStamp && (maj > 1 || min >= 50)) {
        s_gg.progStamp = R_GoreGpuLink(
            "#version 150\n#extension GL_ARB_gpu_shader5 : require\n#define GORE_PRECISE precise\n", s_goreGpuStampFS,
            qtrue
        );
    }
    s_gg.precise = s_gg.progStamp != 0;
    {
        const char *hdr = (maj > 1 || min >= 50) ? "#version 150\n#define GORE_PRECISE\n" : "#version 130\n#define GORE_PRECISE\n";

        if (!s_gg.progStamp) {
            s_gg.progStamp = R_GoreGpuLink(hdr, s_goreGpuStampFS, qfalse);
        }
        s_gg.progBase = R_GoreGpuLink(hdr, s_goreGpuBaseFS, qfalse);
    }
    if (!s_gg.progStamp || !s_gg.progBase) {
        ri.Printf(PRINT_WARNING, "^~^~^ GORE gpu: unavailable (shaders) - using the CPU compositor\n");
        return qfalse;
    }

    GL_UseProgram(s_gg.progBase);
    qglUniform1i(qglGetUniformLocation(s_gg.progBase, "u_Tex"), 0);
    s_gg.uBaseP = qglGetUniformLocation(s_gg.progBase, "u_P");
    GL_UseProgram(s_gg.progStamp);
    qglUniform1i(qglGetUniformLocation(s_gg.progStamp, "u_Dst"), 0);
    qglUniform1i(qglGetUniformLocation(s_gg.progStamp, "u_Art"), 1);
    s_gg.uStampOrg = qglGetUniformLocation(s_gg.progStamp, "u_Org");
    s_gg.uStampRot = qglGetUniformLocation(s_gg.progStamp, "u_Rot");
    s_gg.uStampArt = qglGetUniformLocation(s_gg.progStamp, "u_ArtSize");

    qglGenVertexArrays(1, &s_gg.vao); // no attributes: the quad comes from gl_VertexID
    qglGenFramebuffers(1, &s_gg.fboInst);
    qglGenFramebuffers(1, &s_gg.fboScratch);

    // texture slots (image_t so R_DeleteTextures owns them; level 0 is respecified below / on art load)
    s_gg.scratch = R_CreateImage("*goreScratch", dummy, 16, 16, IMGTYPE_COLORALPHA, IMGFLAG_NONE, 0);
    s_gg.art[0]  = R_CreateImage("*goreArt0", dummy, 16, 16, IMGTYPE_COLORALPHA, IMGFLAG_NONE, 0);
    s_gg.art[1]  = R_CreateImage("*goreArt1", dummy, 16, 16, IMGTYPE_COLORALPHA, IMGFLAG_NONE, 0);
    if (!s_gg.vao || !s_gg.fboInst || !s_gg.fboScratch || !s_gg.scratch || !s_gg.art[0] || !s_gg.art[1]) {
        ri.Printf(PRINT_WARNING, "^~^~^ GORE gpu: unavailable (GL objects) - using the CPU compositor\n");
        return qfalse;
    }
    s_gg.scratch->r_sequence = -1;
    for (i = 0; i < 2; i++) {
        s_gg.art[i]->r_sequence = -1;
    }
    s_goreArtDirty = qtrue;

    {
        GLuint tex = s_gg.scratch->texnum;

        qglTextureImage2DEXT(
            tex, GL_TEXTURE_2D, 0, GL_RGBA8, GORE_GPU_SCRATCH, GORE_GPU_SCRATCH, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL
        );
        qglTextureParameterfEXT(tex, GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        qglTextureParameterfEXT(tex, GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        qglTextureParameterfEXT(tex, GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        qglTextureParameterfEXT(tex, GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        s_gg.scratch->width = s_gg.scratch->uploadWidth = GORE_GPU_SCRATCH;
        s_gg.scratch->height = s_gg.scratch->uploadHeight = GORE_GPU_SCRATCH;

        GL_BindFramebuffer(GL_FRAMEBUFFER, s_gg.fboScratch);
        qglFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        if (qglCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            ri.Printf(PRINT_WARNING, "^~^~^ GORE gpu: unavailable (scratch framebuffer incomplete) - using the CPU compositor\n");
            return qfalse;
        }
    }

    s_gg.ok = qtrue;
    ri.Printf(
        PRINT_ALL, "^~^~^ GORE gpu: compositor ready (GLSL %d.%02d, stamp maths %s)\n", maj, min,
        s_gg.precise ? "precise" : "not marked precise"
    );
    return qtrue;
}

static qboolean R_GoreImageIsSRGB(const image_t *img)
{
    switch (img->internalFormat) {
    case GL_SRGB_EXT:
    case GL_SRGB8_EXT:
    case GL_SRGB_ALPHA_EXT:
    case GL_SRGB8_ALPHA8_EXT:
    case GL_COMPRESSED_SRGB_S3TC_DXT1_EXT:
    case GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1_EXT:
    case GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT3_EXT:
    case GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT:
    case GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM_ARB:
        return qtrue;
    default:
        return qfalse;
    }
}

/*
================
R_GoreGpuReady

Should (and can) an instance of this base image be composited on the GPU?  An sRGB base stays on the CPU path:
sampling would linearise it, the readback does not, and the picture must not depend on the path.
================
*/
static qboolean R_GoreGpuReady(const image_t *img)
{
    if (!r_goreGpu || r_goreGpu->integer <= 0) {
        return qfalse;
    }
    if (!R_GoreGpuInit()) {
        return qfalse;
    }
    if (!img || R_GoreImageIsSRGB(img)) {
        return qfalse;
    }
    return qtrue;
}

// make the instance texture the render target (fboInst is left bound to GL_FRAMEBUFFER)
static qboolean R_GoreGpuTarget(goreInstance_t *inst)
{
    GLuint tex = inst->instImage->texnum;

    GL_BindFramebuffer(GL_FRAMEBUFFER, s_gg.fboInst);
    if (s_gg.attached != tex) {
        qglFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        s_gg.attached = tex;
        if (qglCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            ri.Printf(PRINT_WARNING, "^~^~^ GORE gpu: instance framebuffer incomplete - back to the CPU compositor\n");
            s_gg.attached = 0;
            s_gg.ok       = qfalse;
            return qfalse;
        }
    }
    return qtrue;
}

/*
================
R_GoreGpuBase

Give the instance texture its storage (only when the size changed - the slot is reused) and copy level `level`
of the base diffuse into it, texel for texel.
================
*/
static qboolean R_GoreGpuBase(goreInstance_t *inst, image_t *img, int w, int h, int level)
{
    GLuint tex   = inst->instImage->texnum;
    float  wrapS = (img->flags & IMGFLAG_CLAMPTOEDGE) ? (float)GL_CLAMP_TO_EDGE : (float)GL_REPEAT;

    R_GoreGpuBegin();

    if (inst->texW != w || inst->texH != h) {
        qglTextureImage2DEXT(tex, GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        inst->texW    = w;
        inst->texH    = h;
        s_gg.attached = 0; // the attachment must be looked at again
    }
    qglTextureParameterfEXT(tex, GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    qglTextureParameterfEXT(tex, GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    qglTextureParameterfEXT(tex, GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrapS);
    qglTextureParameterfEXT(tex, GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrapS);

    if (!R_GoreGpuTarget(inst)) {
        return qfalse;
    }
    qglViewport(0, 0, w, h);
    GL_BindMultiTexture(GL_TEXTURE0, GL_TEXTURE_2D, img->texnum);
    GL_UseProgram(s_gg.progBase);
    qglUniform4f(s_gg.uBaseP, (float)level, 0.0f, 0.0f, 0.0f);
    qglBindVertexArray(s_gg.vao);
    qglDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    s_gg.draws++;
    s_gg.fills++;
    return qtrue;
}

// (re)load the two art layers into their textures, byte for byte
static void R_GoreGpuArt(void)
{
    int i;

    if (!s_goreArtDirty) {
        return;
    }
    s_goreArtDirty = qfalse;

    for (i = 0; i < 2; i++) {
        const byte *pix = i ? s_splashPix : s_stampPix;
        int         w   = i ? s_splashW : s_stampW;
        int         h   = i ? s_splashH : s_stampH;
        GLuint      tex = s_gg.art[i]->texnum;

        if (!pix || w < 1 || h < 1) {
            continue;
        }
        qglTextureImage2DEXT(tex, GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pix);
        qglTextureParameterfEXT(tex, GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        qglTextureParameterfEXT(tex, GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        qglTextureParameterfEXT(tex, GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        qglTextureParameterfEXT(tex, GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        s_gg.art[i]->width = s_gg.art[i]->uploadWidth = w;
        s_gg.art[i]->height = s_gg.art[i]->uploadHeight = h;
    }

    // r_goreGpu 2 self-test of the base pass on an UNCOMPRESSED source: copy the splash art (RGBA8, uploaded byte
    // for byte just above) into the scratch target with the base program and read it back. It has to come back
    // exactly - which pins every "base copy" difference the verify lines report on the driver decoding a
    // COMPRESSED texture differently for glGetTexImage than the GPU does for sampling, not on this pass.
    if (r_goreGpu && r_goreGpu->integer >= 2 && s_splashPix && s_splashW <= GORE_GPU_SCRATCH && s_splashH <= GORE_GPU_SCRATCH) {
        int   n   = s_splashW * s_splashH * 4, bad = 0;
        byte *got = ri.Malloc(n);

        GL_BindFramebuffer(GL_FRAMEBUFFER, s_gg.fboScratch);
        qglViewport(0, 0, s_splashW, s_splashH);
        GL_BindMultiTexture(GL_TEXTURE0, GL_TEXTURE_2D, s_gg.art[1]->texnum);
        GL_UseProgram(s_gg.progBase);
        qglUniform4f(s_gg.uBaseP, 0.0f, 0.0f, 0.0f, 0.0f);
        qglBindVertexArray(s_gg.vao);
        qglDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        qglReadPixels(0, 0, s_splashW, s_splashH, GL_RGBA, GL_UNSIGNED_BYTE, got);
        for (i = 0; i < n; i++) {
            if (got[i] != s_splashPix[i]) {
                bad++;
            }
        }
        ri.Free(got);
        ri.Printf(
            PRINT_ALL, "^~^~^ GORE VERIFY selftest: base pass on an uncompressed %dx%d texture, %d of %d bytes differ\n",
            s_splashW, s_splashH, bad, n
        );
    }
}

static int R_GoreFloorDiv(int a, int b)
{
    int q = a / b;

    if ((a % b) != 0 && ((a < 0) != (b < 0))) {
        q--;
    }
    return q;
}

/*
================
R_GoreGpuStampLayer

The GPU twin of R_GoreStampLayer's loop: the same arguments, the same texels, the same order.
================
*/
static void R_GoreGpuStampLayer(goreInstance_t *inst, int art, int artW, int artH, int px, int py, float angle, int size)
{
    int   w    = inst->width;
    int   h    = inst->height;
    int   half = size / 2;
    int   x0   = px - half, x1 = px + half; // the loop's dx range [-half, half), unwrapped
    int   y0   = py - half, y1 = py + half;
    int   kx, ky, kx0, kx1, ky0, ky1;
    float ca, sa, scale;

    if (half < 1 || w < 1 || h < 1) {
        return;
    }

    ca    = (float)cos(angle);
    sa    = (float)sin(angle);
    scale = (float)artW / (float)size;

    R_GoreGpuBegin();
    R_GoreGpuArt();
    if (!R_GoreGpuTarget(inst)) {
        return;
    }
    GL_BindMultiTexture(GL_TEXTURE0, GL_TEXTURE_2D, s_gg.scratch->texnum);
    GL_BindMultiTexture(GL_TEXTURE1, GL_TEXTURE_2D, s_gg.art[art]->texnum);
    GL_UseProgram(s_gg.progStamp);
    qglUniform4f(s_gg.uStampRot, ca, sa, scale, 0.0f);
    qglUniform4f(s_gg.uStampArt, (float)artW, (float)artH, 0.0f, 0.0f);
    qglBindVertexArray(s_gg.vao);

    // one pass per texture tile the rectangle touches; rows outer / columns inner is the CPU loop's order,
    // which matters only when the stamp is larger than the texture and overlaps itself
    kx0 = R_GoreFloorDiv(x0, w);
    kx1 = R_GoreFloorDiv(x1 - 1, w);
    ky0 = R_GoreFloorDiv(y0, h);
    ky1 = R_GoreFloorDiv(y1 - 1, h);
    for (ky = ky0; ky <= ky1; ky++) {
        for (kx = kx0; kx <= kx1; kx++) {
            int ax0 = x0 > kx * w ? x0 : kx * w;
            int ax1 = x1 < (kx + 1) * w ? x1 : (kx + 1) * w;
            int ay0 = y0 > ky * h ? y0 : ky * h;
            int ay1 = y1 < (ky + 1) * h ? y1 : (ky + 1) * h;
            int lx0 = ax0 - kx * w, lx1 = ax1 - kx * w; // this tile's piece, in texture texels
            int ly0 = ay0 - ky * h, ly1 = ay1 - ky * h;
            int cx, cy;

            for (cy = ly0; cy < ly1; cy += GORE_GPU_SCRATCH) {
                for (cx = lx0; cx < lx1; cx += GORE_GPU_SCRATCH) {
                    int cw = lx1 - cx > GORE_GPU_SCRATCH ? GORE_GPU_SCRATCH : lx1 - cx;
                    int ch = ly1 - cy > GORE_GPU_SCRATCH ? GORE_GPU_SCRATCH : ly1 - cy;

                    // the destination as it is now -> scratch (read it there: no feedback loop)
                    GL_BindFramebuffer(GL_READ_FRAMEBUFFER, s_gg.fboInst);
                    GL_BindFramebuffer(GL_DRAW_FRAMEBUFFER, s_gg.fboScratch);
                    qglBlitFramebuffer(cx, cy, cx + cw, cy + ch, 0, 0, cw, ch, GL_COLOR_BUFFER_BIT, GL_NEAREST);
                    GL_BindFramebuffer(GL_DRAW_FRAMEBUFFER, s_gg.fboInst);

                    qglViewport(cx, cy, cw, ch);
                    qglUniform4f(s_gg.uStampOrg, (float)(px - kx * w), (float)(py - ky * h), (float)cx, (float)cy);
                    qglDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
                    s_gg.blits++;
                    s_gg.draws++;
                }
            }
        }
    }
}

/*
================
R_GoreGpuVerify

r_goreGpu 2: read the GPU-painted instance back and compare it with the CPU reference copy. `base` = this is the
check of the base copy alone (GPU sampling against the glGetTexImage readback - decoding, not compositing); the
reference then ADOPTS the GPU's texels, so that every later check measures the compositing and nothing else.
================
*/
static void R_GoreGpuVerify(goreInstance_t *inst, const char *what, qboolean base, const image_t *img)
{
    int         n = inst->width * inst->height;
    int         i, c, bad = 0, maxd = 0;
    byte       *got;
    const char *name;

    if (!inst->pixels || !qglGetTexImage || n <= 0) {
        return;
    }
    // the base check names the image being copied and its GL internal format (compressed formats are what differ)
    name = img ? va("%s (format 0x%x)", img->imgName, img->internalFormat)
               : (inst->baseImage ? inst->baseImage->imgName : "?");
    got = ri.Malloc(n * 4);
    R_GoreReadTexImage(inst->instImage, 0, got);
    for (i = 0; i < n; i++) {
        const byte *a = inst->pixels + i * 4;
        const byte *b = got + i * 4;
        int         d = 0;

        for (c = 0; c < 4; c++) {
            int e = a[c] > b[c] ? a[c] - b[c] : b[c] - a[c];

            if (e > d) {
                d = e;
            }
        }
        if (d) {
            bad++;
            if (d > maxd) {
                maxd = d;
            }
        }
    }
    if (base) {
        s_gg.vBaseBadTexels += bad;
        if (maxd > s_gg.vBaseMaxDiff) {
            s_gg.vBaseMaxDiff = maxd;
        }
        Com_Memcpy(inst->pixels, got, n * 4);
    } else {
        s_gg.vChecks++;
        s_gg.vBadTexels += bad;
        if (maxd > s_gg.vMaxDiff) {
            s_gg.vMaxDiff = maxd;
        }
    }
    ri.Free(got);
    ri.Printf(
        PRINT_ALL,
        "^~^~^ GORE VERIFY %s ent %d %s %dx%d stamps %d: %d of %d texels differ, max diff %d | run: %d composite checks, %d texels off, max %d; base copies %d texels off, max %d\n",
        what, inst->entityNumber, name, inst->width, inst->height,
        inst->numStamps, bad, n, maxd, s_gg.vChecks, s_gg.vBadTexels, s_gg.vMaxDiff, s_gg.vBaseBadTexels,
        s_gg.vBaseMaxDiff
    );
}

static void R_GoreGpuShutdown(void)
{
    if (s_gg.progBase) {
        qglDeleteProgram(s_gg.progBase);
    }
    if (s_gg.progStamp) {
        qglDeleteProgram(s_gg.progStamp);
    }
    if (s_gg.fboInst) {
        qglDeleteFramebuffers(1, &s_gg.fboInst);
    }
    if (s_gg.fboScratch) {
        qglDeleteFramebuffers(1, &s_gg.fboScratch);
    }
    if (s_gg.vao) {
        qglDeleteVertexArrays(1, &s_gg.vao);
    }
    Com_Memset(&s_gg, 0, sizeof(s_gg)); // the image slots die with R_DeleteTextures
    s_goreArtDirty = qtrue;
}

static void R_GoreStampPixels(goreInstance_t *inst, const goreStampRec_t *st, int *outPy, int *outHalf);

/*
================
R_GoreFillInstance

(Re)build an instance's texture for base image `img`: the base copy, then the recorded stamps on top - the one
routine behind creation (no stamps yet), the bug-754 tier-skin rebase and an r_goreGpu mode switch.  CPU path:
readback (or the base-pixel cache) into `pixels`, composite, upload - what creation and rebase each did inline
before.  GPU path: R_GoreGpuBase and the stamp quads.  Returns qfalse only if neither path can run.
================
*/
static qboolean R_GoreFillInstance(goreInstance_t *inst, image_t *img, int w, int h, int level)
{
    qboolean gpu = R_GoreGpuReady(img);
    qboolean cpuCopy;
    int      dbg = r_goreDebug ? r_goreDebug->integer : 0;
    int      i, py, half, t0 = 0;

    if (gpu) {
        if (dbg >= 2) {
            qglFinish(); // time the GPU's work, not just the submission
        }
        t0 = ri.Milliseconds();
        if (!R_GoreGpuBase(inst, img, w, h, level)) {
            gpu = qfalse;
        }
    }
    if (!gpu && !qglGetTexImage) {
        return qfalse; // loader could not resolve the readback entry point
    }
    cpuCopy = !gpu || (r_goreGpu->integer >= 2 && qglGetTexImage);

    if (!cpuCopy) {
        if (inst->pixels) {
            ri.Free(inst->pixels);
            inst->pixels = NULL;
        }
    } else if (!inst->pixels || w != inst->width || h != inst->height) {
        if (inst->pixels) {
            ri.Free(inst->pixels);
        }
        inst->pixels = ri.Malloc(w * h * 4);
    }
    inst->gpu       = gpu;
    inst->width     = w;
    inst->height    = h;
    inst->fullWidth = img->uploadWidth;
    inst->instImage->width = inst->instImage->uploadWidth = w;
    inst->instImage->height = inst->instImage->uploadHeight = h;

    if (cpuCopy) {
        // read the uploaded base texture back (post-picmip, post-lightscale ->
        // the copy is pixel-identical to what everyone else renders)
        R_GoreBasePixels(img, level, w, h, inst->pixels); // [bug-3493] cached per uniform texture
        if (gpu) {
            R_GoreGpuVerify(inst, "base", qtrue, img);
        }
    }

    for (i = 0; i < inst->numStamps; i++) {
        R_GoreStampPixels(inst, &inst->stamps[i], &py, &half);
    }

    if (!gpu) {
        // respecify our instance slot with the copy (gl2 RE_UploadCinematic pattern:
        // the DSA-wrapped texture entry points keep the tr_dsa binding cache coherent)
        GLuint texture = inst->instImage->texnum;
        float  wrapS   = (img->flags & IMGFLAG_CLAMPTOEDGE) ? (float)GL_CLAMP_TO_EDGE : (float)GL_REPEAT;

        qglTextureImage2DEXT(texture, GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, inst->pixels);
        qglTextureParameterfEXT(texture, GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        qglTextureParameterfEXT(texture, GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        qglTextureParameterfEXT(texture, GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrapS);
        qglTextureParameterfEXT(texture, GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrapS);
        inst->texW = w;
        inst->texH = h;
        if (s_gg.attached == texture) {
            s_gg.attached = 0;
        }
    } else {
        if (cpuCopy && inst->numStamps) {
            R_GoreGpuVerify(inst, "replay", qfalse, NULL);
        }
        if (dbg) {
            if (dbg >= 2) {
                qglFinish();
            }
            ri.Printf(
                PRINT_ALL, "^~^~^ GORE gpu fill %s %dx%d (mip %d), %d stamps replayed, took %d ms%s\n", img->imgName, w,
                h, level, inst->numStamps, ri.Milliseconds() - t0, dbg >= 2 ? " (finished)" : " (submitted)"
            );
        }
    }
    return qtrue;
}

/*
================
R_GoreModeSync

r_goreGpu changed: rebuild every live instance from its base image and stamp records in the other mode, so a
wound never depends on which path painted it first (and so the two paths can be compared on the same frame).
================
*/
static void R_GoreModeSync(void)
{
    int i, mode, n = 0;

    if (!r_goreGpu) {
        // 1 = wounds are composited on the GPU (no texture ever read back); 0 = the CPU path; 2 = GPU + verify
        r_goreGpu = ri.Cvar_Get("r_goreGpu", "1", CVAR_ARCHIVE);
    }
    mode = r_goreGpu->integer;
    if (mode == s_goreMode) {
        return;
    }
    s_goreMode = mode;

    for (i = 0; i < GORE_MAX_INSTANCES; i++) {
        goreInstance_t *inst = &s_goreInstances[i];
        int             w, h, level;

        if (!inst->inuse || !inst->instImage || !inst->baseImage || !inst->baseImage->texnum) {
            continue;
        }
        R_GoreInstanceDims(inst->baseImage, &w, &h, &level);
        if (!R_GoreFillInstance(inst, inst->baseImage, w, h, level)) {
            R_GoreFreeInstance(inst);
        }
        n++;
    }
    if (n) {
        R_GoreRebuildEntityFlags();
    }
    if (r_goreDebug && r_goreDebug->integer) {
        ri.Printf(PRINT_ALL, "^~^~^ GORE mode r_goreGpu %d: %d live instance(s) rebuilt\n", mode, n);
    }
}

/*
================
R_GoreAcquireInstance

Find or create the wound texture copy for (entityNumber, baseImage).
Creation reads the uploaded base texture back from GL (so the copy matches
whatever the engine actually loaded - TGA, JPG or DDS override) and
respecifies a reusable "*goreN" image slot with it.  Returns NULL on any
failure (caller silently skips - crash safety over completeness).
================
*/
static void R_GoreRebaseInstance(goreInstance_t *inst); // bug-754: defined below

static goreInstance_t *R_GoreAcquireInstance(int entityNumber, image_t *baseImage)
{
    goreInstance_t *inst = NULL;
    int             i, w, h, level;

    // existing?
    for (i = 0; i < GORE_MAX_INSTANCES; i++) {
        if (s_goreInstances[i].inuse && s_goreInstances[i].entityNumber == entityNumber
            && s_goreInstances[i].baseImage == baseImage) {
            return &s_goreInstances[i];
        }
    }

    if (!baseImage || baseImage->texnum == 0) {
        return NULL;
    }

    // bug-754: a fresh impact on a surface that already carries an instance keyed to a
    // DIFFERENT tier variant of the same diffuse (skin bits flipped between the two hits).
    // Rebase that instance onto the currently bound image instead of creating a duplicate,
    // so one instance carries the surface's whole wound history.
    {
        char family[MAX_QPATH];

        R_GoreImageFamily(baseImage->imgName, family, sizeof(family));
        for (i = 0; i < GORE_MAX_INSTANCES; i++) {
            inst = &s_goreInstances[i];
            if (inst->inuse && inst->entityNumber == entityNumber && inst->instImage
                && !Q_stricmp(inst->family, family)) {
                inst->rebaseImage = baseImage;
                R_GoreRebaseInstance(inst); // frees the instance on failure
                if (inst->inuse && inst->baseImage == baseImage) {
                    return inst;
                }
                break; // rebase failed - fall through and try a clean create
            }
        }
        inst = NULL;
    }
    if (!qglGetTexImage && !R_GoreGpuReady(baseImage)) {
        return NULL; // loader could not resolve the readback entry point (and no GPU compositor either)
    }
    R_GoreInstanceDims(baseImage, &w, &h, &level); // [bug-3493] a mip of an HD skin, not all 16 MB of it
    if (w < 8 || h < 8 || w * h * 4 > GORE_MAX_TEXBYTES) {
        return NULL;
    }

    // free slot, else steal the least recently used
    for (i = 0; i < GORE_MAX_INSTANCES; i++) {
        if (!s_goreInstances[i].inuse) {
            inst = &s_goreInstances[i];
            break;
        }
    }
    if (!inst) {
        // bug-780: THE "existing holes disappear when I shoot new ones" wiper.  The old
        // strict-min pick degenerated once the table saturated in a firefight: every
        // VISIBLE wounded character re-binds each frame, so all lastUseFrame tie at
        // tr.frameCount and the first minimal slot - slot 0 - got stolen EVERY time
        // (revolving door).  Any new instance (new surface, new victim, AI crossfire the
        // player never aimed) then wiped a live character's wounds - frequently the very
        // character being shot.  Now: NEVER steal from the entity being stamped, prefer
        // the stalest lastUseFrame (off-screen / long-dead corpses), and break remaining
        // ties by fewest stamps (cheapest loss).
        goreInstance_t *victim = NULL;

        for (i = 0; i < GORE_MAX_INSTANCES; i++) {
            goreInstance_t *cand = &s_goreInstances[i];

            if (cand->entityNumber == entityNumber) {
                continue; // protect the character being stamped
            }
            if (!victim || cand->lastUseFrame < victim->lastUseFrame
                || (cand->lastUseFrame == victim->lastUseFrame && cand->numStamps < victim->numStamps)) {
                victim = cand;
            }
        }
        if (!victim) {
            return NULL; // whole table is this entity's own instances - never self-wipe
        }
        inst = victim;
        if (r_goreDebug && r_goreDebug->integer) {
            ri.Printf(
                PRINT_ALL, "^~^~^ GORE steal slot %d (ent %d, %d stamps) for ent %d\n",
                (int)(inst - s_goreInstances), inst->entityNumber, inst->numStamps, entityNumber
            );
        }
        R_GoreFreeInstance(inst);
        R_GoreRebuildEntityFlags();
    }

    // lazily create the reusable GL image slot for this table index
    if (!inst->instImage) {
        static byte dummy[16 * 16 * 4];
        int         slot = (int)(inst - s_goreInstances);

        // HZM gl2 re-port (bug-gl2-gore): gl2's R_CreateImage; no mipmaps
        // (IMGFLAG_NONE), level 0 is respecified below anyway
        inst->instImage = R_CreateImage(
            va("*gore%d", slot), dummy, 16, 16, IMGTYPE_COLORALPHA, IMGFLAG_NONE, 0
        );
        if (!inst->instImage) {
            return NULL;
        }
        inst->instImage->r_sequence = -1; // builtin-style: never purged between levels
    }

    // the base copy (no stamps yet): GPU = one quad into the instance texture, CPU = readback + upload
    inst->numStamps    = 0;
    inst->stampHead    = 0;
    inst->entityNumber = entityNumber; // for the debug / verify lines of the fill
    if (!R_GoreFillInstance(inst, baseImage, w, h, level)) {
        inst->entityNumber = -1;
        return NULL;
    }

    inst->inuse        = qtrue;
    inst->entityNumber = entityNumber;
    inst->baseImage    = baseImage;
    inst->rebaseImage  = NULL;
    R_GoreImageFamily(baseImage->imgName, inst->family, sizeof(inst->family)); // bug-754
    inst->lastUseFrame = tr.frameCount;
    s_goreActiveCount++;
    if (entityNumber >= 0 && entityNumber < GORE_MAX_ENTNUM) {
        s_goreEntityHas[entityNumber] = 1;
    }
    return inst;
}

/*
================
R_GoreStampLayer

Composite one art layer (wound stamp or blood splash) into the CPU copy at
texel (px,py), rotated, scaled to 'size' texels (no GL).  UV wrap is handled
on both axes.  bug-780: factored out of R_GoreStampPixels so a record can
paint two layers (splash halo under, hole core over).
================
*/
static void R_GoreStampLayer(
    goreInstance_t *inst, int art, const byte *artPix, int artW, int artH, int px, int py, float angle, int size)
{
    int   w = inst->width;
    int   h = inst->height;
    int   half = size / 2;
    int   dx, dy;
    float ca, sa, scale;

    if (!artPix || size < 2) {
        return;
    }

    // r_goreGpu: the same texels, painted by a quad into the instance texture (art: 0 = hole, 1 = splash)
    if (inst->gpu) {
        R_GoreGpuStampLayer(inst, art, artW, artH, px, py, angle, size);
    }
    if (!inst->pixels) {
        return; // no CPU copy (it exists beside a GPU instance only as the r_goreGpu 2 reference)
    }

    ca    = (float)cos(angle);
    sa    = (float)sin(angle);
    scale = (float)artW / (float)size;

    for (dy = -half; dy < half; dy++) {
        for (dx = -half; dx < half; dx++) {
            // inverse-rotate into art space, nearest sample
            float fx = (dx * ca - dy * sa) * scale + artW * 0.5f;
            float fy = (dx * sa + dy * ca) * scale + artH * 0.5f;
            int   sx = (int)fx;
            int   sy = (int)fy;
            const byte *src;
            byte       *dst;
            int         a, ia, tx, ty;

            if (sx < 0 || sy < 0 || sx >= artW || sy >= artH) {
                continue;
            }
            src = artPix + (sy * artW + sx) * 4;
            a   = src[3];
            if (!a) {
                continue;
            }

            tx = px + dx;
            ty = py + dy;
            tx = ((tx % w) + w) % w;
            ty = ((ty % h) + h) % h;

            dst = inst->pixels + (ty * w + tx) * 4;
            ia  = 255 - a;
            dst[0] = (byte)((dst[0] * ia + src[0] * a) / 255);
            dst[1] = (byte)((dst[1] * ia + src[1] * a) / 255);
            dst[2] = (byte)((dst[2] * ia + src[2] * a) / 255);
            // keep dst alpha (some skins use alpha for cull tricks)
        }
    }
}

/*
================
R_GoreStampPixels

Composite one recorded wound into the CPU copy (no GL).  Size comes from the
record's width-fraction so a replay onto a different-resolution diffuse
(bug-754 tier-skin rebase) keeps the same on-model wound size.  bug-780: a
record is now two layers - a larger soft blood-splash halo under (all kinds)
and the bullet-hole stamp over (kind 0 only; kind 1 = blood-only splash).
Returns the touched row center/halfspan (of the LARGER layer) for the
caller's upload.
================
*/
static void R_GoreStampPixels(goreInstance_t *inst, const goreStampRec_t *st, int *outPy, int *outHalf)
{
    int w = inst->width;
    int h = inst->height;
    int px, py, rawSize, holeSize, splashSize, half;
    int holeMax, splashMax;

    *outPy   = 0;
    *outHalf = 0;
    if (!s_stampPix && !s_splashPix) {
        return;
    }

    rawSize = (int)(w * st->sizeFrac);
    if (rawSize < 6) {
        rawSize = 6;
    }
    // HZM coop (bloodier skin): the skin factor is already baked into st->sizeFrac (so into
    // rawSize).  Lift the texel clamps by the same factor for a SKIN wound so the enlargement
    // survives on HD (large) skin diffuses; cloth/uniform wounds keep the original clamps.
    holeMax   = GORE_STAMP_MAXPX;
    splashMax = GORE_SPLASH_MAXPX;
    if (st->skin) {
        float sk  = R_GoreSkinScale();
        holeMax   = (int)(GORE_STAMP_MAXPX * sk);
        splashMax = (int)(GORE_SPLASH_MAXPX * sk);
    }
    // [bug-3493] the clamps are in texels of the FULL image; this instance may be a smaller mip of it
    if (inst->fullWidth > w && inst->fullWidth > 0) {
        holeMax   = (int)((float)holeMax * w / inst->fullWidth);
        splashMax = (int)((float)splashMax * w / inst->fullWidth);
        if (holeMax < 6) {
            holeMax = 6;
        }
        if (splashMax < 6) {
            splashMax = 6;
        }
    }
    holeSize = rawSize;
    if (holeSize > holeMax) {
        holeSize = holeMax;
    }
    // bug-817 (user "dial the blood back"): both kinds route the splash through the
    // clamped hole size again (the bug-780 behaviour). The bug-791 unclamped kind-1 path
    // was what let kill splashes balloon on HD diffuses; reverted so the splash stays
    // proportional to the wound.
    splashSize = (int)(holeSize * GORE_SPLASH_SCALE);
    if (splashSize > splashMax) {
        splashSize = splashMax;
    }

    // texel of the impact (wrapped)
    px = (int)floor(st->u * w);
    py = (int)floor(st->v * h);
    px = ((px % w) + w) % w;
    py = ((py % h) + h) % h;

    half = 0;
    if (s_splashPix) {
        // decorrelate the splash rotation from the hole so the pair never reads as one shape
        R_GoreStampLayer(inst, 1, s_splashPix, s_splashW, s_splashH, px, py, st->angle * 1.7f + 1.1f, splashSize);
        half = splashSize / 2;
    }
    if (st->kind == 0 && s_stampPix) {
        R_GoreStampLayer(inst, 0, s_stampPix, s_stampW, s_stampH, px, py, st->angle, holeSize);
        if (holeSize / 2 > half) {
            half = holeSize / 2;
        }
    }

    *outPy   = py;
    *outHalf = half;
}

/*
================
R_GoreStampUploadBand

Upload the touched rows (full-width bands are contiguous memory), wrapping
across the texture edge as needed.  Caller must have the GL context current.
(gl2: DSA-wrapped qglTextureSubImage2DEXT - no explicit bind needed)
================
*/
static void R_GoreStampUploadBand(goreInstance_t *inst, int py, int half)
{
    int    w = inst->width;
    int    h = inst->height;
    int    yMin, yMax;
    GLuint texture = inst->instImage->texnum;

    yMin = py - half;
    yMax = py + half; // exclusive
    if (yMax - yMin >= h) {
        yMin = 0;
        yMax = h;
    }
    if (yMin < 0) {
        // wrapped band at the bottom of the texture
        int rows = -yMin;
        qglTextureSubImage2DEXT(
            texture, GL_TEXTURE_2D, 0, 0, h - rows, w, rows, GL_RGBA, GL_UNSIGNED_BYTE,
            inst->pixels + (h - rows) * w * 4
        );
        yMin = 0;
    }
    if (yMax > h) {
        // wrapped band at the top of the texture
        int rows = yMax - h;
        qglTextureSubImage2DEXT(texture, GL_TEXTURE_2D, 0, 0, 0, w, rows, GL_RGBA, GL_UNSIGNED_BYTE, inst->pixels);
        yMax = h;
    }
    if (yMax > yMin) {
        qglTextureSubImage2DEXT(
            texture, GL_TEXTURE_2D, 0, 0, yMin, w, yMax - yMin, GL_RGBA, GL_UNSIGNED_BYTE,
            inst->pixels + yMin * w * 4
        );
    }
}

/*
================
R_GoreApplyStamp

RECORD one stamp in the instance's replay ring (bug-754: survives tier-skin
diffuse swaps; cap GORE_MAX_STAMPS = drop the OLDEST record only), composite,
upload the touched band.
================
*/
static void R_GoreApplyStamp(goreInstance_t *inst, const goreStampRec_t *rec)
{
    int py, half;

    inst->stamps[inst->stampHead] = *rec;
    inst->stampHead               = (inst->stampHead + 1) % GORE_MAX_STAMPS;
    if (inst->numStamps < GORE_MAX_STAMPS) {
        inst->numStamps++;
    }

    R_GoreStampPixels(inst, rec, &py, &half);
    if (!inst->gpu) {
        R_GoreStampUploadBand(inst, py, half);
    } else if (inst->pixels) {
        R_GoreGpuVerify(inst, "stamp", qfalse, NULL); // r_goreGpu 2
    }
    inst->lastUseFrame = tr.frameCount;
}

/*
================
R_GoreStampInstance

New bullet wound at (u,v): roll rotation + size jitter, record + composite +
upload (splash halo + hole core).
================
*/
static void R_GoreStampInstance(
    goreInstance_t *inst, float u, float v, qboolean hasDown, float downAngle, qboolean skin)
{
    goreStampRec_t rec;

    if (!s_stampPix && !s_splashPix) {
        return;
    }

    rec.u = u;
    rec.v = v;
    // bug-829: if we resolved gravity for this hit, orient the wound so its baked trickle runs
    // down-body; otherwise fall back to a random roll (e.g. degenerate UV mapping).
    rec.angle    = hasDown ? downAngle : (R_GoreRand01() * 6.2831853f);
    rec.sizeFrac = GORE_STAMP_BASEFRAC * (0.80f + 0.40f * R_GoreRand01());
    rec.kind     = 0;
    // HZM coop (bloodier skin): an EXPOSED-SKIN wound (face/head/hands) gets a bigger hole stamp
    // and - via R_GoreStampPixels - a proportionally bigger blood-splash halo.  Cloth/uniform
    // wounds keep skin=0 and the unchanged size.  The dark #150200 palette is untouched either way.
    rec.skin = skin;
    if (skin) {
        rec.sizeFrac *= R_GoreSkinScale();
    }

    R_GoreApplyStamp(inst, &rec);
}

/*
================
R_GoreKillStampInstance

bug-780: one killing-blow blood-only splash (no hole) at a RANDOM body UV,
bigger than a wound halo, so corpses read bloodied even on surfaces whose
tier skins never fired.  Recorded in the same replay ring, so it survives
tier-skin rebases like any wound.
================
*/
static void R_GoreKillStampInstance(goreInstance_t *inst)
{
    goreStampRec_t rec;

    if (!s_splashPix) {
        return;
    }

    rec.u        = R_GoreRand01();
    rec.v        = R_GoreRand01();
    rec.angle    = R_GoreRand01() * 6.2831853f;
    // bug-817 (user "dial the blood back"): restored to the pre-round-4 1.35-2.10x range
    // (bug-791 had cranked this to 1.7-2.7x for the "super messy" pass).
    rec.sizeFrac = GORE_STAMP_BASEFRAC * (1.35f + 0.75f * R_GoreRand01());
    rec.kind     = 1;
    // HZM coop (bloodier skin): killing-blow splashes are random-placement corpse spatter, not
    // tied to a skin surface, and the user dialed corpse blood DOWN (bug-817) - leave them at
    // the cloth size (skin=0), so this feature only enlarges the aimed skin wounds.
    rec.skin = qfalse;

    R_GoreApplyStamp(inst, &rec);
}

/*
================
R_GoreRebaseInstance

bug-754: the surface this instance wounds now binds a DIFFERENT diffuse (a
tier-1 _bloodN skin variant, or a heal flipped it back).  Re-read the newly
bound base texture, replay the recorded wound history onto it, respecify the
instance slot, and re-key the instance to the new image.  On any failure the
instance is freed (stamps lost, correctness kept).  GL context must be
current (called from R_GoreCommitPending / R_GoreAcquireInstance).
================
*/
static void R_GoreRebaseInstance(goreInstance_t *inst)
{
    image_t *newImg = inst->rebaseImage;
    int      w, h, level = 0;

    inst->rebaseImage = NULL;
    if (!newImg || newImg == inst->baseImage) {
        return;
    }

    w = h = 0;
    if (newImg->texnum) {
        R_GoreInstanceDims(newImg, &w, &h, &level); // [bug-3493]
    }
    // the new base, then the recorded wounds replayed onto it (GPU: quads; CPU: readback, composite, upload)
    if (newImg->texnum == 0 || w < 8 || h < 8 || w * h * 4 > GORE_MAX_TEXBYTES
        || !R_GoreFillInstance(inst, newImg, w, h, level)) {
        R_GoreFreeInstance(inst);
        R_GoreRebuildEntityFlags();
        return;
    }

    inst->baseImage = newImg;
    R_GoreImageFamily(newImg->imgName, inst->family, sizeof(inst->family));
    inst->lastUseFrame = tr.frameCount;

    if (r_goreDebug && r_goreDebug->integer) {
        ri.Printf(
            PRINT_ALL, "^~^~^ GORE REBASE ent %d -> %s (%dx%d), %d stamps replayed\n",
            inst->entityNumber, newImg->imgName, w, h, inst->numStamps
        );
    }
}

/*
================
R_GoreDebugCommand

r_goreDebug only: a test hook, so a wound can be placed exactly and repeatably (the QA stills compare the two
compositors on the same wounds). Set the cvar r_goreTest to ONE command; it is consumed at the end of the frame:
  seg x y z x y z    queue a bullet segment (world space), as cgame does for a real bullet
  uv ENT N U V SKIN  stamp a wound at UV (u,v) on entity ENT's N-th wound instance (it must exist already)
  kill ENT           the killing-blow splashes on entity ENT
  seed N             reseed the stamp rotation / size jitter
  list               print the live instances
================
*/
static void R_GoreDebugCommand(void)
{
    static cvar_t *test = NULL;
    char           buf[256];
    float          f[6];
    int            a, b, c, i, k;
    unsigned int   u;

    if (!r_goreDebug || !r_goreDebug->integer) {
        return;
    }
    if (!test) {
        test = ri.Cvar_Get("r_goreTest", "", CVAR_TEMP);
    }
    if (!test->string[0]) {
        return;
    }
    Q_strncpyz(buf, test->string, sizeof(buf));
    ri.Cvar_Set("r_goreTest", "");

    if (sscanf(buf, "seg %f %f %f %f %f %f", &f[0], &f[1], &f[2], &f[3], &f[4], &f[5]) == 6) {
        RE_GoreImpact(f, f + 3);
    } else if (sscanf(buf, "uv %d %d %f %f %d", &a, &b, &f[0], &f[1], &c) == 5) {
        for (i = 0, k = 0; i < GORE_MAX_INSTANCES; i++) {
            goreInstance_t *inst = &s_goreInstances[i];

            if (inst->inuse && inst->entityNumber == a && inst->instImage && k++ == b) {
                R_GoreLoadStamp();
                R_GoreStampInstance(inst, f[0], f[1], qfalse, 0.0f, c ? qtrue : qfalse);
                ri.Printf(
                    PRINT_ALL, "^~^~^ GORE TEST uv stamp ent %d slot %d (%s %dx%d) uv (%.4f %.4f) stamps %d\n", a, i,
                    inst->baseImage->imgName, inst->width, inst->height, f[0], f[1], inst->numStamps
                );
                break;
            }
        }
        if (i == GORE_MAX_INSTANCES) {
            ri.Printf(PRINT_ALL, "^~^~^ GORE TEST uv: ent %d has no instance %d\n", a, b);
        }
    } else if (sscanf(buf, "kill %d", &a) == 1) {
        RE_GoreKillSplash(a);
    } else if (sscanf(buf, "seed %u", &u) == 1) {
        s_goreSeed = u;
    } else if (!Q_stricmpn(buf, "list", 4)) {
        for (i = 0; i < GORE_MAX_INSTANCES; i++) {
            goreInstance_t *inst = &s_goreInstances[i];

            if (inst->inuse) {
                ri.Printf(
                    PRINT_ALL, "^~^~^ GORE LIST slot %d ent %d %s %dx%d stamps %d %s lastUse %d\n", i,
                    inst->entityNumber, inst->baseImage ? inst->baseImage->imgName : "?", inst->width, inst->height,
                    inst->numStamps, inst->gpu ? (inst->pixels ? "gpu+ref" : "gpu") : "cpu", inst->lastUseFrame
                );
            }
        }
        ri.Printf(PRINT_ALL, "^~^~^ GORE LIST end: %d active, r_goreGpu %d\n", s_goreActiveCount, s_goreMode);
    } else {
        ri.Printf(PRINT_ALL, "^~^~^ GORE TEST: did not understand '%s'\n", buf);
    }
}

/*
================
R_GoreCommitPending

End of frame (all skel surfaces skinned and tested, GL work for the frame
issued): apply stamps for pending impacts that found a triangle, expire the
rest.  Runs on the frontend thread with the GL context current.
================
*/
void R_GoreCommitPending(void)
{
    int i;
    int now;

    // r_goreGpu: (register the cvar and) rebuild the live instances if the compositor mode changed
    R_GoreModeSync();
    R_GoreDebugCommand(); // r_goreDebug: the r_goreTest hook

    // bug-754: apply tier-skin rebases first (scheduled by R_GoreOverrideImage when a wounded
    // surface bound a different _bloodN diffuse this frame) so new stamps land on the new copy
    if (s_goreRebasePending) {
        s_goreRebasePending = qfalse;
        for (i = 0; i < GORE_MAX_INSTANCES; i++) {
            if (s_goreInstances[i].inuse && s_goreInstances[i].rebaseImage) {
                R_GoreRebaseInstance(&s_goreInstances[i]);
            }
        }
    }

    if (s_gorePendingCount) {
        now = ri.Milliseconds();

        for (i = 0; i < GORE_MAX_PENDING; i++) {
            gorePending_t *p = &s_gorePending[i];
            qboolean       snapOn;
            qboolean       preferSnapOverCloth;

            if (!p->inuse) {
                continue;
            }

            // bug-905: recover face/head/hand shots that the thin exact ray GRAZED onto a nearby
            // CLOTH surface (helmet brim / collar / shoulder) instead of the skin.  When the exact
            // hit is cloth (!bestSkin) but a skin vertex sat CLOSER to the bullet stop point than
            // that cloth triangle did (snapDist < bestDistToEnd), stamp the skin snap instead.
            // Self-bounding: a dead-on torso/cloth shot has a tiny bestDistToEnd that no skin vertex
            // can beat, so legit cloth wounds are protected; only a grazing cloth clip beside a true
            // skin hit is relocated onto the skin.  Skin-only beneficiary -> never removes a wound.
            snapOn              = (coop_goreSkinSnap && coop_goreSkinSnap->integer);
            preferSnapOverCloth = p->hasHit && !p->bestSkin && p->hasSnap && snapOn
                                  && p->snapDist < p->bestDistToEnd;

            if (p->hasHit && !preferSnapOverCloth) {
                goreInstance_t *inst;

                R_GoreLoadStamp();
                inst = R_GoreAcquireInstance(p->bestEntityNumber, p->bestImage);
                if (r_goreDebug && r_goreDebug->integer) {
                    ri.Printf(
                        PRINT_ALL, "^~^~^ GORE %s ent %d uv (%.3f %.3f) skin=%d img %s (%dx%d)\n",
                        inst ? "STAMP" : "hit-but-NO-INSTANCE", p->bestEntityNumber, p->bestU, p->bestV,
                        p->bestSkin, p->bestImage ? p->bestImage->imgName : "?",
                        p->bestImage ? p->bestImage->uploadWidth : 0,
                        p->bestImage ? p->bestImage->uploadHeight : 0
                    );
                }
                if (inst) {
                    R_GoreStampInstance(
                        inst, p->bestU, p->bestV, p->bestHasDown, p->bestDownAngle, p->bestSkin
                    );
                }
                // HZM coop (headshot feedback): a WORN-HEADGEAR hit also stamps the nearest
                // exposed-skin vertex of the SAME entity (the face under the rim).  On a killing
                // head hit the engine pops the helmet and hides its surfaces, so the exact-hit
                // stamp above vanishes with them - this second stamp is what the corpse actually
                // shows, and it gives the killing-blow splash an instance to land on.  While the
                // helmet stays on (survivor), the helmet stamp remains the visible one.
                if (p->bestHeadgear && p->hasSnap && snapOn
                    && p->snapEntityNumber == p->bestEntityNumber) {
                    goreInstance_t *skinInst;

                    skinInst = R_GoreAcquireInstance(p->snapEntityNumber, p->snapImage);
                    if (r_goreDebug && r_goreDebug->integer) {
                        ri.Printf(
                            PRINT_ALL, "^~^~^ GORE %s ent %d uv (%.3f %.3f) HEADGEAR+SKIN d=%.1f img %s\n",
                            skinInst ? "STAMP" : "skin-but-NO-INSTANCE", p->snapEntityNumber, p->snapU,
                            p->snapV, p->snapDist, p->snapImage ? p->snapImage->imgName : "?"
                        );
                    }
                    if (skinInst) {
                        R_GoreStampInstance(
                            skinInst, p->snapU, p->snapV, p->snapHasDown, p->snapDownAngle, qtrue
                        );
                    }
                }
                p->inuse = qfalse;
                s_gorePendingCount--;
            } else if (p->hasSnap && snapOn) {
                // HZM coop (skin-snap fallback): reached in two cases -
                //   1. the exact ray found NOTHING on this enemy (a mover whose small skin surfaces
                //      slid off the server segment vs the client's animated pose), OR
                //   2. bug-905: the exact ray grazed CLOTH but a skin vertex sat closer to the stop
                //      point (preferSnapOverCloth), so we relocate the wound onto the skin.
                // Either way stamp the nearest skin vertex as a SKIN wound (skin=qtrue -> the
                // coop_goreSkinWoundScale enlargement), routed through the SAME stamp path the exact
                // hit uses.  Only skin surfaces are ever tracked, so cloth/torso can't be false-snapped.
                goreInstance_t *inst;

                R_GoreLoadStamp();
                inst = R_GoreAcquireInstance(p->snapEntityNumber, p->snapImage);
                if (r_goreDebug && r_goreDebug->integer) {
                    ri.Printf(
                        PRINT_ALL, "^~^~^ GORE %s ent %d uv (%.3f %.3f) SNAP d=%.1f img %s (%dx%d)\n",
                        inst ? "SNAP-STAMP" : "snap-but-NO-INSTANCE", p->snapEntityNumber, p->snapU,
                        p->snapV, p->snapDist, p->snapImage ? p->snapImage->imgName : "?",
                        p->snapImage ? p->snapImage->uploadWidth : 0,
                        p->snapImage ? p->snapImage->uploadHeight : 0
                    );
                }
                if (inst) {
                    R_GoreStampInstance(
                        inst, p->snapU, p->snapV, p->snapHasDown, p->snapDownAngle, qtrue
                    );
                }
                p->inuse = qfalse;
                s_gorePendingCount--;
            } else if (now - p->msec > GORE_PENDING_LIFE_MS || now < p->msec) {
                // never met a character triangle (wall hit / target off-screen)
                if (r_goreDebug && r_goreDebug->integer) {
                    ri.Printf(
                        PRINT_ALL, "^~^~^ GORE MISS seg end (%.0f %.0f %.0f) - no character tri in %dms\n",
                        p->vEnd[0], p->vEnd[1], p->vEnd[2], GORE_PENDING_LIFE_MS
                    );
                }
                p->inuse = qfalse;
                s_gorePendingCount--;
            } else {
                // keep for another frame, but drop stale per-frame candidates
                p->hasHit  = qfalse;
                p->hasSnap = qfalse;
            }
        }
    }

    // bug-780: killing blows -> 2-3 blood-only splashes at random body UVs, spread over
    // the corpse's wound instances.  Runs AFTER the pending loop so the killing volley's
    // own stamp has already created its instance this frame; a flag with no instance yet
    // waits up to GORE_KILL_LIFE_MS (the impact ray-test can land a frame later), then
    // expires silently (a corpse that never took a UV wound has nothing to splash on).
    if (s_goreKillCount) {
        int e;

        now = ri.Milliseconds();
        for (e = 0; e < GORE_MAX_ENTNUM && s_goreKillCount; e++) {
            goreInstance_t *owned[GORE_MAX_INSTANCES];
            int             nOwned, nSplash, k;

            if (!s_goreKillMs[e]) {
                continue;
            }

            nOwned = 0;
            if (s_goreEntityHas[e]) {
                for (i = 0; i < GORE_MAX_INSTANCES; i++) {
                    if (s_goreInstances[i].inuse && s_goreInstances[i].entityNumber == e
                        && s_goreInstances[i].instImage) {
                        owned[nOwned++] = &s_goreInstances[i];
                    }
                }
            }

            if (nOwned) {
                R_GoreLoadStamp();
                // bug-817 (user "dial the blood back"): restored to 2-3 killing-blow
                // splashes (bug-791 had raised it to 3-4 for the "super messy" pass).
                nSplash = 2 + ((R_GoreRand01() < 0.5f) ? 0 : 1);
                for (k = 0; k < nSplash; k++) {
                    R_GoreKillStampInstance(owned[((int)(R_GoreRand01() * nOwned)) % nOwned]);
                }
                if (r_goreDebug && r_goreDebug->integer) {
                    ri.Printf(
                        PRINT_ALL, "^~^~^ GORE KILL ent %d: %d splashes over %d instance(s)\n", e, nSplash,
                        nOwned
                    );
                }
                s_goreKillMs[e] = 0;
                s_goreKillCount--;
            } else if (now - s_goreKillMs[e] > GORE_KILL_LIFE_MS || now < s_goreKillMs[e]) {
                s_goreKillMs[e] = 0;
                s_goreKillCount--;
            }
        }
    }

    // r_goreGpu: put back the GL state the instance passes of this commit changed (no-op if there were none)
    R_GoreGpuEnd();
}

/*
================
R_GoreLevelReset

Map change / registration restart: every entity slot means someone new now.
GL image slots stay allocated for reuse (they are r_sequence -1 builtins).
================
*/
void R_GoreLevelReset(void)
{
    int i;

    for (i = 0; i < GORE_MAX_INSTANCES; i++) {
        R_GoreFreeInstance(&s_goreInstances[i]);
    }
    for (i = 0; i < GORE_MAX_PENDING; i++) {
        s_gorePending[i].inuse = qfalse;
    }
    s_gorePendingCount  = 0;
    s_goreActiveCount   = 0;
    s_goreRebasePending = qfalse;
    Com_Memset(s_goreEntityHas, 0, sizeof(s_goreEntityHas));

    // bug-780: forget killing-blow flags (entity slots mean someone new next level)
    Com_Memset(s_goreKillMs, 0, sizeof(s_goreKillMs));
    s_goreKillCount = 0;

    if (s_stampPix) {
        ri.Free(s_stampPix);
        s_stampPix = NULL;
    }
    s_stampW = s_stampH = 0;
    if (s_splashPix) { // bug-780
        ri.Free(s_splashPix);
        s_splashPix = NULL;
    }
    s_splashW = s_splashH = 0;
    s_stampTried = qfalse; // pak set may have changed - reload lazily (covers both stamps)
}

/*
================
R_GoreShutdown

Renderer shutdown (before R_DeleteTextures wipes every GL texture): free
CPU buffers and forget the image slots so a restart re-creates them, and
delete the GPU compositor's own GL objects.
================
*/
void R_GoreShutdown(void)
{
    int i;

    R_GoreLevelReset();
    for (i = 0; i < GORE_MAX_INSTANCES; i++) {
        s_goreInstances[i].instImage    = NULL;
        s_goreInstances[i].lastUseFrame = 0;
    }
    for (i = 0; i < GORE_MAX_INSTANCES; i++) {
        s_goreInstances[i].texW = s_goreInstances[i].texH = 0;
    }
    for (i = 0; i < GORE_BASECACHE_SLOTS; i++) { // [bug-3493] the textures these were read from are about to go
        R_GoreBaseFree(&s_goreBase[i]);
    }
    // r_goreGpu: the framebuffers / programs / vertex array are ours to delete (the context is still current
    // at both call sites); the scratch and art image slots go with R_DeleteTextures right after this
    R_GoreGpuShutdown();
    s_goreMode = -1;
}
