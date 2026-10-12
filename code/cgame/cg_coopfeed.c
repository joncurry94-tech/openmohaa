/*
===========================================================================
HZM coop [user 2026-10-05] EVENT FEED - replaces the engine game-message box (gmbox) for coop chatter.

Why (docs/proposals/hud_declutter_2026-10-05/PLAN.md): the gmbox holds 5 lines, decays them ONE AT A TIME
(cl_uigmbox.cpp PostDecayEvent), holds a bold line 10 s and beeps "objective_text" for every bold line
(cl_ui.cpp UI_PrintConsole). A five-line officer burst stayed on screen for most of a minute. This feed:
  - max 3 lines, bottom-left above the health/stamina cluster, newest at the bottom;
  - every line has its OWN timer (parallel fade, never a queue), no sound;
  - a repeat of the same key restarts the hold and shows a counter ("x3") instead of a new line;
  - drawn procedurally like the stamina arc (no ihuddraw slot - the fade-exempt band is full - and no
    s_hudFadeAlpha, so a stationary player still sees it).

WIRE: an ordinary script print (Player iprint / ScriptThread iprintln*), so it is a RELIABLE server command,
whose text starts with a tag:   ~f<cat>[:<key>[:<icon>]]~<text>
  cat 0 alert (amber - the only category drawn in Hardcore), 1 team, 2 self, 3 pickup (icon), 4 hint (once
  per profile: coop_hintSeen/coop_hintSeen2), 5 armory status (sets coop_armoryStatus, not drawn),
  6 last-mission line (key 0-9 -> archived coop_lm<key>, read by ui/coop_lastmission.urc, not drawn).
The server only tags for a client that advertises cg_hzmFeed 1 in its userinfo (coop_mod/feed.scr); an older
cgame gets the plain text. The text is echoed to the console WITHOUT a colour byte, so it stays in the console
and qconsole.log but never reaches the gmbox (UI_PrintConsole routes only prefixed text there).

Client cvars: coop_feed 0 (no feed lines) / 1 (default) / 2 (feed + mirror into the old box, non-bold),
coop_hints 0/1, coop_feedX (hi-res HUD units from the left) / coop_feedYv (640x480 VIRTUAL units from the bottom,
the script huddraw space the item counters live in),
coop_feedDebug 1 (one ^~^~^ FEED line per accepted message, for the harness).
===========================================================================
*/

#include "cg_local.h"

#define FEED_SLOTS     6
#define FEED_VISIBLE   3
#define FEED_MAXROWS   5   // hints (category 4) may use all 5 rows; everything else 3
#define FEED_TEXT      200
#define FEED_KEY       40
#define FEED_ICON      64
#define FEED_FADE_IN   150
#define FEED_FADE_OUT  600
#define FEED_FORCE_OUT 200
#define FEED_BUMP      260
#define FEED_ROW_H     13.0f
#define FEED_GAP       3.0f
#define FEED_WRAP_W    300.0f
#define FEED_ICON_SZ   16.0f

typedef struct {
    int       active;
    int       cat;
    char      key[FEED_KEY];
    char      text[FEED_TEXT];
    char      icon[FEED_ICON];
    qhandle_t hIcon;
    int       count;
    int       born;
    int       refresh;
    int       hold;
    int       dieAt;  // 0 = natural expiry; else a forced fast fade started at this time
    int       nrows;
    int       rowStart[FEED_MAXROWS];
    int       rowLen[FEED_MAXROWS];
    int       trunc;      // the text did not fit: the last row ends in "..."
    int       hintMark;   // category 4: mark the key seen on its first visible frame
    float     y;      // current bottom offset (units, animated)
    int       placed;
} feedLine_t;

static feedLine_t    s_feed[FEED_SLOTS];
static fontheader_t *s_feedFont   = NULL;
static int           s_feedInit   = 0;
static int           s_feedLastT  = 0;
static cvar_t       *s_cvFeed     = NULL;
static cvar_t       *s_cvHints    = NULL;
static cvar_t       *s_cvX        = NULL;
static cvar_t       *s_cvY        = NULL;
static cvar_t       *s_cvDebug    = NULL;

void CG_CoopFeedInit(void)
{
    if (s_feedInit) {
        return;
    }
    s_feedInit = 1;
    memset(s_feed, 0, sizeof(s_feed));
    s_cvFeed  = cgi.Cvar_Get("coop_feed", "1", CVAR_ARCHIVE);
    s_cvHints = cgi.Cvar_Get("coop_hints", "1", CVAR_ARCHIVE);
    s_cvX     = cgi.Cvar_Get("coop_feedX", "22", CVAR_ARCHIVE);
    // [user 2026-10-06 / 2026-10-11, bug-3464] "your new small text for intel etc overlaps with the medkit, ammo and
    // sandbag icons". The feed sat coop_feedY (116) HI-RES units above the bottom, but the counters it has to clear are
    // script huddraw elements in the 640x480 VIRTUAL screen (medkit.scr: rect 5 -62 16 16, i.e. their top edge is
    // 62 * vidHeight / 480 above the bottom). Hi-res units are 1 px up to 1920x1080 and vidHeight / 1080 above it, so
    // the two only agreed at the 720p the harness ran: counters top 93 px vs feed 116 px there, but 139 px vs 116 px
    // at 1080p and 186 px vs 155 px at 1440p (16:9 or ultrawide alike) - the feed sat ON the counters everywhere real.
    // The anchor is now in the SAME virtual-480 units as the counters: 70 = 8 units above their top at every size.
    // New cvar name on purpose: an archived coop_feedY 116 from an old config would mean something else here.
    s_cvY     = cgi.Cvar_Get("coop_feedYv", "70", CVAR_ARCHIVE);
    s_cvDebug = cgi.Cvar_Get("coop_feedDebug", "0", 0);
    cgi.Cvar_Get("coop_hintSeen", "", CVAR_ARCHIVE);
    cgi.Cvar_Get("coop_hintSeen2", "", CVAR_ARCHIVE);
    cgi.Cvar_Get("coop_armoryStatus", "", 0);
    // CAPABILITY in the userinfo (same pattern as cg_hzmLt): this cgame understands the ~f tags, so the server
    // may send them. ROM: the player cannot change what the binary can do.
    cgi.Cvar_Get("cg_hzmFeed", "1", CVAR_USERINFO | CVAR_ROM);
}

static fontheader_t *CG_FeedFont(void)
{
    if (!s_feedFont) {
        s_feedFont = cgi.R_LoadFont("verdana-12");
    }
    return s_feedFont;
}

// strip ^-colour codes and line breaks; collapse to one line of plain text
static void CG_FeedClean(const char *in, char *out, int outSize)
{
    int n = 0;
    while (*in && n < outSize - 1) {
        if (*in == '^' && in[1]) {
            in += 2;
            continue;
        }
        if (*in == '\n' || *in == '\r') {
            in++;
            continue;
        }
        out[n++] = *in++;
    }
    while (n > 0 && out[n - 1] == ' ') {
        n--;
    }
    out[n] = 0;
}

static int CG_FeedHoldMs(int cat)
{
    switch (cat) {
    case 0:
        return 6000;
    case 1:
        return 4000;
    case 2:
        return 3000;
    case 3:
        return 2500;
    case 4:
        return 8000;
    default:
        return 3000;
    }
}

static float CG_FeedTextW(const char *s, int len)
{
    fontheader_t *f = CG_FeedFont();
    if (!f) {
        return (float)len * 6.0f;
    }
    return (float)cgi.UI_FontStringWidth(f, s, len);
}

// word wrap into at most FEED_MAXROWS rows of FEED_WRAP_W units (the last row may be cut with "...")
static void CG_FeedWrap(feedLine_t *e)
{
    const char *s   = e->text;
    int         len = (int)strlen(s);
    int         pos = 0;
    float       maxW;

    int         maxRows = (e->cat == 4) ? FEED_MAXROWS : 3;

    maxW      = FEED_WRAP_W - ((e->cat == 3) ? (FEED_ICON_SZ + 4.0f) : 0.0f);
    e->nrows  = 0;
    e->trunc  = 0;
    while (pos < len && e->nrows < maxRows) {
        int lastSpace = -1;
        int end       = pos;
        while (end < len) {
            if (s[end] == ' ') {
                lastSpace = end;
            }
            if (CG_FeedTextW(s + pos, end - pos + 1) > maxW) {
                break;
            }
            end++;
        }
        if (end >= len) {
            e->rowStart[e->nrows] = pos;
            e->rowLen[e->nrows]   = len - pos;
            e->nrows++;
            return;
        }
        if (lastSpace > pos) {
            end = lastSpace;
        }
        if (end <= pos) {
            end = pos + 1;
        }
        e->rowStart[e->nrows] = pos;
        e->rowLen[e->nrows]   = end - pos;
        e->nrows++;
        pos = end;
        while (pos < len && s[pos] == ' ') {
            pos++;
        }
    }
    if (pos < len) {
        e->trunc = 1;
    }
    if (e->nrows <= 0) {
        e->nrows       = 1;
        e->rowStart[0] = 0;
        e->rowLen[0]   = 0;
    }
}

// hint memory: two archived cvars of space-separated keys
static qboolean CG_FeedHintSeen(const char *key)
{
    char        pat[FEED_KEY + 4];
    const char *a = cgi.Cvar_Get("coop_hintSeen", "", CVAR_ARCHIVE)->string;
    const char *b = cgi.Cvar_Get("coop_hintSeen2", "", CVAR_ARCHIVE)->string;
    char        buf[600];

    Com_sprintf(pat, sizeof(pat), " %s ", key);
    Com_sprintf(buf, sizeof(buf), " %s %s ", a, b);
    return strstr(buf, pat) ? qtrue : qfalse;
}

static void CG_FeedHintMark(const char *key)
{
    const char *a = cgi.Cvar_Get("coop_hintSeen", "", CVAR_ARCHIVE)->string;
    const char *b = cgi.Cvar_Get("coop_hintSeen2", "", CVAR_ARCHIVE)->string;
    char        buf[300];

    if (strlen(a) + strlen(key) + 2 < 240) {
        Com_sprintf(buf, sizeof(buf), "%s%s%s", a, a[0] ? " " : "", key);
        cgi.Cvar_Set("coop_hintSeen", buf);
    } else if (strlen(b) + strlen(key) + 2 < 240) {
        Com_sprintf(buf, sizeof(buf), "%s%s%s", b, b[0] ? " " : "", key);
        cgi.Cvar_Set("coop_hintSeen2", buf);
    }
    // both full: the hint keeps showing - better than silently swallowing a new one
}

static float CG_FeedAlpha(const feedLine_t *e, int now)
{
    float a = 1.0f;
    int   t;

    if (!e->active) {
        return 0.0f;
    }
    t = now - e->born;
    if (t < FEED_FADE_IN) {
        a = (t <= 0) ? 0.0f : (float)t / (float)FEED_FADE_IN;
    }
    if (e->dieAt) {
        t = now - e->dieAt;
        if (t >= FEED_FORCE_OUT) {
            return 0.0f;
        }
        if (t > 0) {
            a *= 1.0f - (float)t / (float)FEED_FORCE_OUT;
        }
        return a;
    }
    t = now - (e->refresh + e->hold);
    if (t >= FEED_FADE_OUT) {
        return 0.0f;
    }
    if (t > 0) {
        a *= 1.0f - (float)t / (float)FEED_FADE_OUT;
    }
    return a;
}

static qboolean CG_FeedExpired(const feedLine_t *e, int now)
{
    if (!e->active) {
        return qtrue;
    }
    if (e->dieAt) {
        return (now - e->dieAt >= FEED_FORCE_OUT) ? qtrue : qfalse;
    }
    return (now - (e->refresh + e->hold) >= FEED_FADE_OUT) ? qtrue : qfalse;
}

static qboolean CG_FeedLive(const feedLine_t *e, int now)
{
    return (e->active && !e->dieAt && now < e->refresh + e->hold) ? qtrue : qfalse;
}

static void CG_FeedPush(int cat, const char *key, const char *icon, const char *text)
{
    int         i, now = cg.time, live = 0, slot = -1, oldest;
    feedLine_t *e;

    if (!key[0]) {
        key = text;
    }
    // same key still on screen: restart its hold, bump the counter, show the newest wording
    for (i = 0; i < FEED_SLOTS; i++) {
        e = &s_feed[i];
        if (e->active && !e->dieAt && !Q_stricmpn(e->key, key, FEED_KEY - 1) && !CG_FeedExpired(e, now)) {
            if (!Q_stricmp(e->text, text)) {
                e->count++;
            } else {
                e->count = 1; // same key, new wording (e.g. a different squad): no misleading "x2"
            }
            e->refresh = now;
            if (e->born > now - FEED_FADE_IN) {
                e->born = now - FEED_FADE_IN;
            }
            Q_strncpyz(e->text, text, sizeof(e->text));
            CG_FeedWrap(e);
            return;
        }
    }
    // at most FEED_VISIBLE live lines: the oldest makes room with a fast fade
    for (;;) {
        live   = 0;
        oldest = -1;
        for (i = 0; i < FEED_SLOTS; i++) {
            if (CG_FeedLive(&s_feed[i], now)) {
                live++;
                if (oldest < 0 || s_feed[i].born < s_feed[oldest].born) {
                    oldest = i;
                }
            }
        }
        if (live < FEED_VISIBLE || oldest < 0) {
            break;
        }
        s_feed[oldest].dieAt = now;
    }
    for (i = 0; i < FEED_SLOTS; i++) {
        if (CG_FeedExpired(&s_feed[i], now)) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        // every slot still fading: take the oldest
        slot = 0;
        for (i = 1; i < FEED_SLOTS; i++) {
            if (s_feed[i].born < s_feed[slot].born) {
                slot = i;
            }
        }
    }
    e = &s_feed[slot];
    memset(e, 0, sizeof(*e));
    e->active  = 1;
    e->cat     = cat;
    e->count   = 1;
    e->born    = now;
    e->refresh = now;
    e->hold    = CG_FeedHoldMs(cat);
    Q_strncpyz(e->key, key, sizeof(e->key));
    Q_strncpyz(e->text, text, sizeof(e->text));
    Q_strncpyz(e->icon, icon, sizeof(e->icon));
    e->hintMark = (cat == 4) ? 1 : 0;
    if (cat == 3 && icon[0]) {
        e->hIcon = cgi.R_RegisterShaderNoMip(icon);
    }
    CG_FeedWrap(e);
}

/*
CG_CoopFeedIntercept - called from CG_ServerCommand for "print"/"hudprint" BEFORE the text reaches the console.
Returns qtrue when the message was a feed tag and has been handled (the caller must not print it again).
*/
qboolean CG_CoopFeedIntercept(const char *msg)
{
    const char *p = msg;
    int         cat, n;
    char        key[FEED_KEY];
    char        icon[FEED_ICON];
    char        text[FEED_TEXT];

    if (!p) {
        return qfalse;
    }
    // ONLY the two colour bytes a server script print uses (Player iprint / iprintln:  yellow,  white). Chat
    // (), team chat and obituaries (/) carry player-typed text and names, so a "~f6:0~" typed by a player
    // can never become a feed line or write a coop_lm cvar.
    if ((unsigned char)*p != 1 && (unsigned char)*p != 3) {
        return qfalse;
    }
    p++;
    if (p[0] != '~' || p[1] != 'f' || p[2] < '0' || p[2] > '9') {
        return qfalse;
    }
    CG_CoopFeedInit();
    cat = p[2] - '0';
    p += 3;
    key[0] = icon[0] = 0;
    if (*p == ':') {
        p++;
        for (n = 0; *p && *p != ':' && *p != '~'; p++) {
            if (n < FEED_KEY - 1) {
                key[n++] = *p;
            }
        }
        key[n] = 0;
        if (*p == ':') {
            p++;
            for (n = 0; *p && *p != '~'; p++) {
                if (n < FEED_ICON - 1) {
                    icon[n++] = *p;
                }
            }
            icon[n] = 0;
        }
    }
    if (*p != '~') {
        return qfalse; // not a well-formed tag: let it print as it is
    }
    p++;
    CG_FeedClean(p, text, sizeof(text));

    if (s_cvDebug && s_cvDebug->integer) {
        cgi.Printf("^~^~^ FEED cat=%d key=%s t=%d %s\n", cat, key, cg.time, text);
    }

    if (cat == 5) {
        cgi.Cvar_Set("coop_armoryStatus", text);
        cgi.Printf("%s\n", text);
        return qtrue;
    }
    if (cat == 6) {
        if (key[0] >= '0' && key[0] <= '9' && !key[1]) {
            char name[16];
            Com_sprintf(name, sizeof(name), "coop_lm%c", key[0]);
            cgi.Cvar_Get(name, "", CVAR_ARCHIVE);
            cgi.Cvar_Set(name, text);
        }
        return qtrue;
    }

    if (!text[0]) {
        return qtrue;
    }
    // filters first, so a filtered line is neither drawn nor mirrored (the console always keeps it)
    if (CG_CoopHardcoreActive() && cat != 0) {
        cgi.Printf("%s\n", text); // Hardcore: only the amber threat alerts reach the screen
        return qtrue;
    }
    if (cat == 4 && (!s_cvHints->integer || !key[0] || CG_FeedHintSeen(key))) {
        cgi.Printf("%s\n", text);
        return qtrue;
    }
    if (s_cvFeed->integer <= 0) {
        // feed off: the threat alerts (a teammate down, an enemy wave) still reach the old box, non-bold
        cgi.Printf((cat == 0) ? "\x01%s\n" : "%s\n", text);
        return qtrue;
    }
    // no colour byte = console only, never the gmbox; coop_feed 2 = also mirror into the old box, non-bold, no beep
    cgi.Printf((s_cvFeed->integer == 2) ? "\x01%s\n" : "%s\n", text);
    CG_FeedPush(cat, key, icon, text);
    return qtrue;
}

static void CG_FeedTick(const float *col)
{
    cgi.R_SetColor(col);
}

/*
CG_DrawCoopFeed - called every frame from the 2D HUD pass (cg_drawtools.cpp, next to the stamina arc).
cineHide = the coop cinematic HUD suppression is active.
*/
/*
====================
CG_CoopScoreHudMirror

HZM coop [bug-3485] ui/hud_score.urc (the retail "Score: n (n)" line) is shown in multiplayer only. The menu file
is shared by coop and MP, and the MP/coop isolation contract (docs/tools/check_mp_isolation.py clause 7) does not
let a shared file name an MP cvar - so the HUD is gated on this neutral cvar, and the one place that knows which
kind of session this is keeps it current. Written only when it changes.
====================
*/
static void CG_CoopScoreHudMirror(void)
{
    static cvar_t *cvMp = NULL, *cvHud = NULL;
    int            want;

    if (!cvMp) {
        cvMp  = cgi.Cvar_Get("coop_mp_session", "0", 0);
        cvHud = cgi.Cvar_Get("cg_hudScore", "0", 0);
    }
    want = cvMp->integer ? 1 : 0;
    if (cvHud->integer != want) {
        cgi.Cvar_Set("cg_hudScore", want ? "1" : "0");
    }
}

/*
====================
CG_DrawCoopPinned

HZM coop [bug-3485, user 2026-10-11: "Make the enemies remaining text smaller"] The enemies-remaining readout
(coop_mod/enemycount.scr) as ONE pinned line at the top right, in the feed's own type: verdana-12 in hi-res units,
i.e. 12 px at 1080p and below and scaled above - the script HUD could only draw that font at virtual-480 scale,
1.5x to 2.25x larger, and has no smaller font. The server sets the text with `set coop_ecLine <text>` and clears
it with "-"; CG_Init clears it on every connect and map load (cg_main.c), so a line can never outlive
the mission that set it.
====================
*/
static void CG_DrawCoopPinned(void)
{
    static cvar_t *cvLine = NULL;
    fontheader_t  *font;
    char           text[96];
    vec4_t         c;
    float          x, y;

    if (!cvLine) {
        cvLine = cgi.Cvar_Get("coop_ecLine", "", 0);
    }
    // "" or "-" = nothing to show ("-" because a script cannot send an empty value: stufftext has no inner quotes)
    if (!cvLine->string[0] || (cvLine->string[0] == '-' && !cvLine->string[1])) {
        return;
    }
    font = CG_FeedFont();
    if (!font) {
        return;
    }
    CG_FeedClean(cvLine->string, text, sizeof(text));
    if (!text[0]) {
        return;
    }
    x = (float)cgs.glconfig.vidWidth / cgs.uiHiResScale[0] - CG_FeedTextW(text, -1) - 16.0f;
    y = 8.0f;
    c[0] = c[1] = c[2] = 0.0f;
    c[3]                = 0.85f;
    cgi.R_SetColor(c);
    cgi.R_DrawString(font, text, x + 1.0f, y + 1.0f, -1, cgs.uiHiResScale);
    c[0] = 1.00f;
    c[1] = 0.86f;
    c[2] = 0.55f;
    c[3] = 0.95f;
    cgi.R_SetColor(c);
    cgi.R_DrawString(font, text, x, y, -1, cgs.uiHiResScale);
    cgi.R_SetColor(NULL);
}

/*
====================
CG_DrawCoopItems

HZM coop [user 2026-10-11, bug-3495] "For the icons for the medkit, ammo box, and sandbags, can we use subscript for
their amounts and underneath of them put the key that is bound to use them? Please keep all of it small so it isn't
obnoxious and make sure it doesn't overlap with anything else on other resolutions."

Was: three script-HUD icons with a verdana-14 number beside each, drawn at virtual-480 scale (31 px tall digits at
1080p), and no way to show a key - a script cannot read the player's bindings. Now, per item that the server has
published a count for (client cvars coop_hudMed / coop_hudCov / coop_hudAmmo; "" or "-" = not shown):

    [icon]          the same art, a little smaller than before
          2         the amount, in the feed's small type, hung off the icon's lower right corner
      ]             the key bound to it, dimmer, centred underneath (nothing when it is unbound)

Everything is laid out in PIXELS from the bottom-left corner: icon size and the row's height above the bottom follow
the screen HEIGHT (so the row sits where the old one did, just under the feed's bottom edge at 70 virtual units, at
every aspect ratio), text is the feed's own verdana-12 in hi-res units (12 px up to 1080p, scaled above), and the
column pitch is whichever is larger - the icon plus a gap, or room for a six-character key name - so a key label
can never run into its neighbour at any resolution; a longer name is cut to fit.
====================
*/
#define COOPITEM_ICON_V   14.0f // icon edge, virtual-480 units
#define COOPITEM_TOP_V    66.0f // top of the icons above the bottom of the screen, virtual-480 units (feed: 70)
#define COOPITEM_LEFT_V   6.0f
#define COOPITEM_GAP_V    12.0f
#define COOPITEM_MIN_ICON 14.0f // pixels
#define COOPITEM_KEY_PX   48.0f // room a key label is guaranteed, in hi-res units

typedef struct {
    const char *cvarName;
    const char *bindCmd;
    const char *shaderName;
    cvar_t     *cv;
    qhandle_t   hShader;
    char        key[16];
} coopItem_t;

static coopItem_t s_coopItems[3] = {
    {"coop_hudMed",  "append name ,8mk", "textures/hud/coop_medkit_icon",  NULL, 0, ""},
    {"coop_hudCov",  "append name ,9cv", "textures/hud/coop_cover_icon",   NULL, 0, ""},
    {"coop_hudAmmo", "append name ,abx", "textures/hud/coop_ammobox_icon", NULL, 0, ""},
};

// A key name as it is shown under an icon: upper case, and the long ones shortened so that what tells two keys
// apart survives ("MOUSE4" was being cut to "MOUSE"). MOUSE4 -> M4, MWHEELUP -> WH-UP, KP_ENTER -> KP ENTER,
// PAD0_DPAD_UP -> D-UP, PAD0_LEFTSTICK_UP -> LS UP. Whatever is still too wide is cut by the caller.
static void CG_CoopItemKeyLabel(const char *nm, char *out, int outSize)
{
    static const struct {
        const char *from, *to;
    } sub[] = {
        {"PAD0_", ""}, {"MWHEELUP", "WH-UP"}, {"MWHEELDOWN", "WH-DN"}, {"MOUSE", "M"}, {"DPAD_", "D-"},
        {"LEFTSTICK", "LS"}, {"RIGHTSTICK", "RS"}, {"SHOULDER", "SH"}, {"TRIGGER", "TR"}, {"LEFTARROW", "LEFT"},
        {"RIGHTARROW", "RIGHT"}, {"UPARROW", "UP"}, {"DOWNARROW", "DOWN"}, {"SEMICOLON", ";"},
    };
    char up[48];
    int  i, n = 0, k;

    for (i = 0; nm[i] && i < (int)sizeof(up) - 1; i++) {
        up[i] = (char)toupper((unsigned char)nm[i]);
    }
    up[i] = 0;
    i     = 0;
    while (up[i] && n < outSize - 1) {
        for (k = 0; k < (int)(sizeof(sub) / sizeof(sub[0])); k++) {
            int len = (int)strlen(sub[k].from);

            if (!strncmp(up + i, sub[k].from, len)) {
                const char *t = sub[k].to;

                while (*t && n < outSize - 1) {
                    out[n++] = *t++;
                }
                i += len;
                break;
            }
        }
        if (k < (int)(sizeof(sub) / sizeof(sub[0]))) {
            continue;
        }
        out[n++] = (up[i] == '_') ? ' ' : up[i];
        i++;
    }
    out[n] = 0;
}

#define COOPITEM_FLOOR_U  48.0f // hi-res units above the bottom that stay clear: the retail team badge
                                 // (cg_drawtools.cpp CG_DrawPlayerTeam) is 24 units tall and ends 46 up

/*
The row's geometry in pixels, shared with the feed. The top follows the screen HEIGHT (66 virtual units up, where
the old script icons were), unless that would put the key labels on the team badge, which is laid out in fixed
hi-res units - below about 610 px of height it does, and then the row is lifted to clear it.
*/
static float CG_CoopItemsTopPx(float *iconPxOut)
{
    float sy = cgs.uiHiResScale[1] > 0.0f ? cgs.uiHiResScale[1] : 1.0f;
    float vs = (float)cgs.glconfig.vidHeight / 480.0f;
    float iconPx, up, need;

    iconPx = COOPITEM_ICON_V * vs;
    if (iconPx < COOPITEM_MIN_ICON) {
        iconPx = COOPITEM_MIN_ICON;
    }
    up   = COOPITEM_TOP_V * vs;                             // where it wants to be
    need = COOPITEM_FLOOR_U * sy + 18.0f * sy + iconPx;     // badge + (gap and one line of label) + icon
    if (up < need) {
        up = need;
    }
    if (iconPxOut) {
        *iconPxOut = iconPx;
    }
    return (float)cgs.glconfig.vidHeight - up;
}

static void CG_CoopItemKeys(void)
{
    static int nextMs = 0;
    int        i, k1, k2;

    if (cg.time < nextMs && cg.time + 2000 > nextMs) {
        return;
    }
    nextMs = cg.time + 1000; // bindings change in a menu, never mid-frame: once a second is plenty
    for (i = 0; i < 3; i++) {
        coopItem_t *it = &s_coopItems[i];
        const char *nm;

        it->key[0] = 0;
        k1 = k2 = -1;
        cgi.Key_GetKeysForCommand(it->bindCmd, &k1, &k2);
        if (k1 < 0) {
            continue;
        }
        nm = cgi.Key_KeynumToBindString(k1);
        if (!nm || !nm[0]) {
            continue;
        }
        CG_CoopItemKeyLabel(nm, it->key, sizeof(it->key));
    }
}

static void CG_DrawCoopItems(void)
{
    fontheader_t *font = CG_FeedFont();
    float         sx   = cgs.uiHiResScale[0], sy = cgs.uiHiResScale[1];
    float         vs   = (float)cgs.glconfig.vidHeight / 480.0f;
    float         iconPx, topPx, pitchPx, xPx, textH;
    vec4_t        c;
    int           i, shown = 0;

    if (!font || sx <= 0.0f || sy <= 0.0f) {
        return;
    }
    CG_CoopItemKeys();

    topPx   = CG_CoopItemsTopPx(&iconPx);
    pitchPx = iconPx + COOPITEM_GAP_V * vs;
    if (pitchPx < COOPITEM_KEY_PX * sx) {
        pitchPx = COOPITEM_KEY_PX * sx;
    }
    textH = 12.0f * sy;
    xPx   = COOPITEM_LEFT_V * vs;

    for (i = 0; i < 3; i++) {
        coopItem_t *it = &s_coopItems[i];
        const char *v;
        char        num[8], key[16];
        float       tw, tx, ty;
        int         n;

        if (!it->cv) {
            it->cv = cgi.Cvar_Get(it->cvarName, "", 0);
        }
        v = it->cv->string;
        if (!v[0] || v[0] == '-') {
            continue;
        }
        if (!it->hShader) {
            it->hShader = cgi.R_RegisterShaderNoMip(it->shaderName);
        }

        // icon
        c[0] = c[1] = c[2] = 1.0f;
        c[3]                = 0.92f;
        cgi.R_SetColor(c);
        cgi.R_DrawStretchPic(xPx, topPx, iconPx, iconPx, 0.0f, 0.0f, 1.0f, 1.0f, it->hShader);

        // amount: a subscript - starts at the icon's right edge, sits half a line below its bottom edge
        Q_strncpyz(num, v, sizeof(num));
        for (n = 0; num[n]; n++) {
            if (num[n] < '0' || num[n] > '9') {
                num[n] = 0; // digits only: this text came from a server
                break;
            }
        }
        if (num[0]) {
            tx = (xPx + iconPx - 1.0f) / sx;
            ty = (topPx + iconPx - textH * 0.55f) / sy;
            c[0] = c[1] = c[2] = 0.0f;
            c[3]                = 0.85f;
            cgi.R_SetColor(c);
            cgi.R_DrawString(font, num, tx + 1.0f, ty + 1.0f, -1, cgs.uiHiResScale);
            c[0] = c[1] = c[2] = (num[0] == '0' && !num[1]) ? 0.62f : 1.0f; // an empty slot reads dimmer
            c[3]                = 0.95f;
            cgi.R_SetColor(c);
            cgi.R_DrawString(font, num, tx, ty, -1, cgs.uiHiResScale);
        }

        // bound key: centred under the icon, cut to the column so it can never touch the next one
        Q_strncpyz(key, it->key, sizeof(key));
        n = (int)strlen(key);
        while (n > 0 && CG_FeedTextW(key, -1) * sx > pitchPx - 4.0f) {
            key[--n] = 0;
        }
        if (key[0]) {
            tw = CG_FeedTextW(key, -1);
            tx = (xPx + iconPx * 0.5f) / sx - tw * 0.5f;
            if (tx * sx < 2.0f) {
                tx = 2.0f / sx;
            }
            ty = (topPx + iconPx + textH * 0.50f) / sy;
            c[0] = c[1] = c[2] = 0.0f;
            c[3]                = 0.70f;
            cgi.R_SetColor(c);
            cgi.R_DrawString(font, key, tx + 1.0f, ty + 1.0f, -1, cgs.uiHiResScale);
            c[0] = 0.80f;
            c[1] = 0.76f;
            c[2] = 0.62f;
            c[3] = 0.78f;
            cgi.R_SetColor(c);
            cgi.R_DrawString(font, key, tx, ty, -1, cgs.uiHiResScale);
        }

        xPx += pitchPx;
        shown++;
    }
    if (shown) {
        cgi.R_SetColor(NULL);
    }
}

void CG_DrawCoopFeed(qboolean cineHide)
{
    static const float tick[5][3] = {
        {1.00f, 0.66f, 0.20f}, // 0 alert - amber
        {0.80f, 0.68f, 0.40f}, // 1 team - brass
        {0.62f, 0.67f, 0.72f}, // 2 self - steel
        {0.55f, 0.78f, 0.45f}, // 3 pickup - green
        {0.55f, 0.72f, 0.90f}, // 4 hint - pale blue
    };
    int           i, j, k, now, order[FEED_SLOTS], n = 0, dt;
    float         sx, sy, unitsH, x0, bottom, target, a, ex, ey, rowsH, w, maxW;
    vec4_t        c;
    fontheader_t *font;

    CG_CoopFeedInit();
    CG_CoopScoreHudMirror();
    if (!cineHide) {
        CG_DrawCoopItems();         // [bug-3495] medkit / sandbag / ammo box: icon, subscript amount, bound key
        CG_DrawCoopPinned();        // [bug-3485] not part of the feed: coop_feed 0 does not hide it
    }
    now = cg.time;
    dt  = now - s_feedLastT;
    if (dt < 0 || dt > 250) {
        dt = 16;
    }
    s_feedLastT = now;

    // collect visible entries, oldest first
    for (i = 0; i < FEED_SLOTS; i++) {
        if (!s_feed[i].active) {
            continue;
        }
        // expired (the fade-OUT is over - never the alpha 0 at the very start of the fade-IN), or cg.time ran
        // backwards over a map load
        if (s_feed[i].born > now + 1000 || CG_FeedExpired(&s_feed[i], now)) {
            s_feed[i].active = 0;
            continue;
        }
        order[n++] = i;
    }
    for (i = 1; i < n; i++) {
        for (j = i; j > 0 && s_feed[order[j]].born < s_feed[order[j - 1]].born; j--) {
            k            = order[j];
            order[j]     = order[j - 1];
            order[j - 1] = k;
        }
    }
    if (!n) {
        return;
    }
    if (!cg.snap || (cg.snap->ps.pm_flags & (PMF_NO_HUD | PMF_INTERMISSION)) || cineHide) {
        static int s_hideLog = 0;
        if (s_cvDebug && s_cvDebug->integer && (now - s_hideLog > 1000 || now < s_hideLog)) {
            s_hideLog = now;
            cgi.Printf(
                "^~^~^ FEEDHIDE n=%d snap=%d nohud=%d inter=%d cine=%d\n",
                n,
                cg.snap ? 1 : 0,
                (cg.snap && (cg.snap->ps.pm_flags & PMF_NO_HUD)) ? 1 : 0,
                (cg.snap && (cg.snap->ps.pm_flags & PMF_INTERMISSION)) ? 1 : 0,
                cineHide ? 1 : 0
            );
        }
        return; // keep ageing, draw nothing
    }
    font = CG_FeedFont();
    if (s_cvDebug && s_cvDebug->integer) {
        static int s_drawLog = 0;
        if (now - s_drawLog > 1000 || now < s_drawLog) {
            s_drawLog = now;
            cgi.Printf("^~^~^ FEEDDRAW n=%d font=%d vid=%dx%d\n", n, font ? 1 : 0, cgs.glconfig.vidWidth, cgs.glconfig.vidHeight);
        }
    }
    if (!font) {
        return;
    }

    sx     = cgs.uiHiResScale[0];
    sy     = cgs.uiHiResScale[1];
    unitsH = (float)cgs.glconfig.vidHeight / sy;
    x0     = s_cvX->value;
    // bottom edge in feed units: coop_feedYv virtual-480 units above the bottom of the screen (see CG_CoopFeedInit)
    bottom = ((float)cgs.glconfig.vidHeight - s_cvY->value * ((float)cgs.glconfig.vidHeight / 480.0f)) / sy;
    // [bug-3495] on a small screen the item row is lifted clear of the team badge: the feed stays above it
    {
        float itemsTop = (CG_CoopItemsTopPx(NULL) - 4.0f) / sy;

        if (bottom > itemsTop) {
            bottom = itemsTop;
        }
    }

    // newest at the bottom: walk newest -> oldest, stacking upward
    target = 0.0f;
    for (k = n - 1; k >= 0; k--) {
        feedLine_t *e = &s_feed[order[k]];
        float       f;
        rowsH = (float)e->nrows * FEED_ROW_H;
        if (e->cat == 3 && rowsH < FEED_ICON_SZ) {
            rowsH = FEED_ICON_SZ;
        }
        if (!e->placed) {
            e->y      = target;
            e->placed = 1;
        } else {
            f = (float)dt / 90.0f; // ~90 ms slide
            if (f > 1.0f) {
                f = 1.0f;
            }
            e->y += (target - e->y) * f;
        }
        a = CG_FeedAlpha(e, now);
        ey = bottom - e->y - rowsH;
        if (e->hintMark && a > 0.0f) {
            e->hintMark = 0;
            CG_FeedHintMark(e->key); // seen = actually drawn, not merely received (a cinematic may hide it)
        }

        // width of the widest row (+ the counter)
        maxW = 0.0f;
        for (j = 0; j < e->nrows; j++) {
            w = CG_FeedTextW(e->text + e->rowStart[j], e->rowLen[j]);
            if (w > maxW) {
                maxW = w;
            }
        }
        if (e->count > 1) {
            char cnt[16];
            Com_sprintf(cnt, sizeof(cnt), " x%d", e->count);
            maxW += CG_FeedTextW(cnt, -1);
        }
        ex = x0 + 6.0f + ((e->cat == 3 && e->hIcon) ? (FEED_ICON_SZ + 4.0f) : 0.0f);

        // plate
        c[0] = 0.0f;
        c[1] = 0.0f;
        c[2] = 0.0f;
        c[3] = 0.38f * a;
        cgi.R_SetColor(c);
        cgi.R_DrawBox(x0 * sx, (ey - 2.0f) * sy, (ex - x0 + maxW + 6.0f) * sx, (rowsH + 3.0f) * sy);

        // category tick (brightens for a moment when the line is bumped)
        c[0] = tick[e->cat < 5 ? e->cat : 2][0];
        c[1] = tick[e->cat < 5 ? e->cat : 2][1];
        c[2] = tick[e->cat < 5 ? e->cat : 2][2];
        c[3] = a;
        if (now - e->refresh < FEED_BUMP && e->count > 1) {
            c[0] = c[1] = c[2] = 1.0f;
        }
        CG_FeedTick(c);
        cgi.R_DrawBox(x0 * sx, (ey - 2.0f) * sy, 2.0f * sx, (rowsH + 3.0f) * sy);

        // pickup icon: pops in at 1.3x and settles
        if (e->cat == 3 && e->hIcon) {
            float sc = 1.0f, isz;
            int   ti = now - e->born;
            if (ti < 220) {
                sc = 1.3f - 0.3f * ((float)ti / 220.0f);
            }
            isz  = FEED_ICON_SZ * sc;
            c[0] = c[1] = c[2] = 1.0f;
            c[3]                = a;
            cgi.R_SetColor(c);
            cgi.R_DrawStretchPic(
                (x0 + 5.0f + (FEED_ICON_SZ - isz) * 0.5f) * sx,
                (ey + (rowsH - isz) * 0.5f) * sy,
                isz * sx,
                isz * sy,
                0.0f,
                0.0f,
                1.0f,
                1.0f,
                e->hIcon
            );
        }

        // text rows: 1-unit drop shadow, then the line
        for (j = 0; j < e->nrows; j++) {
            char  row[FEED_TEXT];
            float ry = ey + (float)j * FEED_ROW_H;
            if (e->cat == 3 && e->nrows == 1) {
                ry = ey + (rowsH - FEED_ROW_H) * 0.5f;
            }
            Q_strncpyz(row, e->text + e->rowStart[j], e->rowLen[j] + 1 < (int)sizeof(row) ? e->rowLen[j] + 1 : (int)sizeof(row));
            c[0] = c[1] = c[2] = 0.0f;
            c[3]                = 0.85f * a;
            cgi.R_SetColor(c);
            cgi.R_DrawString(font, row, ex + 1.0f, ry + 1.0f, -1, cgs.uiHiResScale);
            c[0] = 0.94f;
            c[1] = 0.92f;
            c[2] = 0.86f;
            c[3] = a;
            cgi.R_SetColor(c);
            cgi.R_DrawString(font, row, ex, ry, -1, cgs.uiHiResScale);
            if (j == e->nrows - 1 && e->trunc) {
                c[0] = 0.94f;
                c[1] = 0.92f;
                c[2] = 0.86f;
                cgi.R_SetColor(c);
                cgi.R_DrawString(font, "...", ex + CG_FeedTextW(row, -1), ry, -1, cgs.uiHiResScale);
            }
            if (j == e->nrows - 1 && e->count > 1) {
                char cnt[16];
                Com_sprintf(cnt, sizeof(cnt), " x%d", e->count);
                c[0] = 0.96f;
                c[1] = 0.78f;
                c[2] = 0.40f;
                cgi.R_SetColor(c);
                cgi.R_DrawString(font, cnt, ex + CG_FeedTextW(row, -1), ry, -1, cgs.uiHiResScale);
            }
        }
        target += rowsH + FEED_GAP + 3.0f;
    }
    cgi.R_SetColor(NULL);
}
