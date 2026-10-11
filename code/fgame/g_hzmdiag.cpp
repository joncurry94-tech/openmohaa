/*
===========================================================================
HZM coop [bug-3486] - game.dll's part of the in-game bug report.

  G_HzmScriptError   a ring of the newest 50 distinct script errors (text, script file and line when the engine knows
                     them, level time first/last seen, repeat count), kept regardless of `developer`
  G_HzmDiag          exported; the exe finds it with Sys_LoadFunction (an older exe never calls it, an older game.dll
                     does not have it - both cases just leave the section out). Writes plain text into the caller's
                     buffer: the mission state (players, AI, where the asking player is and looks, level.coop_*),
                     the script errors, or four counts for the once-a-second sampler.

Read-only: nothing here changes an entity, a script variable or anything a player can see.
===========================================================================
*/

#include "g_local.h"
#include "g_hzmdiag.h"
#include "level.h"
#include "game.h"
#include "player.h"
#include "actor.h"
#include "sentient.h"
#include "scriptexception.h"
#include "scriptvariable.h"

#define HZSE_MAX     50
#define HZVARS_MAX   200
#define HZVAR_VALUE  80

typedef struct {
    unsigned int hash;
    int          serial; // when it was last seen (order)
    int          repeats;
    float        firstTime, lastTime;
    int          line;
    char         kind[16];
    char         map[24];
    char         file[72];
    char         text[200];
    char         source[96];
} hzScriptErr_t;

static hzScriptErr_t s_se[HZSE_MAX];
static int           s_seUsed, s_seSerial, s_seTotal;
static char          s_posFile[72], s_posSource[96];
static int           s_posLine;
static bool          s_posValid;

// printable ASCII, single spaces, no quotes that could break a token downstream
static void HZ_Clean(const char *in, char *out, size_t size)
{
    size_t o     = 0;
    bool   space = true; // drops leading blanks

    for (; in && *in && o + 1 < size; in++) {
        unsigned char c = (unsigned char)*in;
        if (c < 0x20 || c > 126) {
            c = ' ';
        }
        if (c == ' ') {
            if (space) {
                continue;
            }
            space = true;
        } else {
            space = false;
        }
        out[o++] = (char)c;
    }
    while (o > 0 && out[o - 1] == ' ') {
        o--;
    }
    out[o] = 0;
}

void G_HzmScriptErrorPos(const char *file, int line, const char *sourceLine)
{
    HZ_Clean(file, s_posFile, sizeof(s_posFile));
    HZ_Clean(sourceLine, s_posSource, sizeof(s_posSource));
    s_posLine  = line;
    s_posValid = true;
}

void G_HzmScriptError(const char *kind, const char *text)
{
    hzScriptErr_t  e;
    hzScriptErr_t *slot = NULL;
    unsigned int   h    = 2166136261u;
    const char    *p;
    int            i;

    memset(&e, 0, sizeof(e));
    HZ_Clean(kind, e.kind, sizeof(e.kind));
    HZ_Clean(text, e.text, sizeof(e.text));
    if (s_posValid) {
        Q_strncpyz(e.file, s_posFile, sizeof(e.file));
        Q_strncpyz(e.source, s_posSource, sizeof(e.source));
        e.line     = s_posLine;
        s_posValid = false;
    }
    for (p = e.text; *p; p++) {
        h = (h ^ (unsigned char)*p) * 16777619u;
    }
    for (p = e.file; *p; p++) {
        h = (h ^ (unsigned char)*p) * 16777619u;
    }
    h = (h ^ (unsigned int)e.line) * 16777619u;

    s_seTotal++;
    s_seSerial++;
    for (i = 0; i < s_seUsed; i++) {
        if (s_se[i].hash == h && s_se[i].line == e.line && !strcmp(s_se[i].text, e.text) && !strcmp(s_se[i].file, e.file)) {
            s_se[i].repeats++;
            s_se[i].lastTime = level.time;
            s_se[i].serial   = s_seSerial;
            return;
        }
    }
    if (s_seUsed < HZSE_MAX) {
        slot = &s_se[s_seUsed++];
    } else {
        // full: the one seen longest ago makes room
        slot = &s_se[0];
        for (i = 1; i < HZSE_MAX; i++) {
            if (s_se[i].serial < slot->serial) {
                slot = &s_se[i];
            }
        }
    }
    e.hash      = h;
    e.serial    = s_seSerial;
    e.repeats   = 1;
    e.firstTime = e.lastTime = level.time;
    HZ_Clean(level.mapname.c_str(), e.map, sizeof(e.map));
    *slot = e;
}

/*
====================================================================================================================
Text
====================================================================================================================
*/
typedef struct {
    char *p;
    int   size, len;
} hzOut_t;

static void HZ_Out(hzOut_t *o, const char *fmt, ...)
{
    va_list ap;
    int     n, room = o->size - o->len;

    if (room <= 1) {
        return;
    }
    va_start(ap, fmt);
    n = Q_vsnprintf(o->p + o->len, room, fmt, ap);
    va_end(ap);
    if (n < 0 || n >= room) {
        n = room - 1;
    }
    o->len += n;
    o->p[o->len] = 0;
}

static int HZ_CmpSerial(const void *a, const void *b)
{
    return (*(const hzScriptErr_t *const *)a)->serial - (*(const hzScriptErr_t *const *)b)->serial;
}

// newest last; `max` = how many of the newest to print (0 = all)
static void HZ_TextScriptErrors(hzOut_t *o, int max, bool withSource)
{
    const hzScriptErr_t *order[HZSE_MAX];
    int                  i, first;

    for (i = 0; i < s_seUsed; i++) {
        order[i] = &s_se[i];
    }
    qsort(order, s_seUsed, sizeof(order[0]), HZ_CmpSerial);
    HZ_Out(o, "script errors this game session: %d in all, %d distinct kept (newest %d, oldest first; developer=%d)\n", s_seTotal,
           s_seUsed, HZSE_MAX, developer ? developer->integer : 0);
    first = (max > 0 && s_seUsed > max) ? s_seUsed - max : 0;
    for (i = first; i < s_seUsed; i++) {
        const hzScriptErr_t *e = order[i];
        HZ_Out(o, "x%d  map %s  level time %.1f", e->repeats, e->map[0] ? e->map : "?", e->firstTime);
        if (e->repeats > 1) {
            HZ_Out(o, "..%.1f", e->lastTime);
        }
        if (e->file[0]) {
            HZ_Out(o, "  (%s, %d)", e->file, e->line);
        }
        HZ_Out(o, "  %s: %s\n", e->kind, e->text);
        if (withSource && e->source[0]) {
            HZ_Out(o, "      > %s\n", e->source);
        }
    }
}

static void HZ_Counts(int *out)
{
    int i, n;

    out[0] = out[1] = out[2] = out[3] = 0;
    for (i = 0; i < globals.num_entities; i++) {
        if (g_entities[i].inuse) {
            out[0]++;
        }
    }
    n = SentientList.NumObjects();
    for (i = 1; i <= n; i++) {
        Sentient *s = SentientList.ObjectAt(i);
        if (!s || s->deadflag != DEAD_NO || s->health <= 0) {
            continue;
        }
        if (s->IsSubclassOfActor()) {
            if (s->m_Team == TEAM_AMERICAN) {
                out[2]++;
            } else {
                out[1]++;
            }
        } else if (s->IsSubclassOfPlayer()) {
            if (!((Player *)s)->IsSpectator()) {
                out[3]++;
            }
        }
    }
}

typedef struct {
    char name[48];
    char value[HZVAR_VALUE + 4];
} hzVar_t;

static hzVar_t s_vars[HZVARS_MAX];
static int     s_varCount, s_varSeen;

// A script variable whose NAME says it holds a credential (level.coop_authTokens, the admin login) is never read: this
// text also goes to any joiner who asks, and into reports.
static bool HZ_VarWithheld(const char *name)
{
    static const char *const subs[] = {"auth", "token", "pass", "rcon", "secret", "admin", "login", "guid", "cdkey", "webhook"};
    char                     low[64];
    size_t                   i;

    for (i = 0; name[i] && i + 1 < sizeof(low); i++) {
        low[i] = (char)tolower((unsigned char)name[i]);
    }
    low[i] = 0;
    for (i = 0; i < sizeof(subs) / sizeof(subs[0]); i++) {
        const char *hit = strstr(low, subs[i]);
        if (hit && strncmp(hit, "author", 6)) { // "authored" / "author" is map-making vocabulary, not a login
            return true;
        }
    }
    return false;
}

static void HZ_VarCb(const char *name, ScriptVariable& value, void *ud)
{
    hzVar_t *v;
    char     raw[512];

    (void)ud;
    if (Q_stricmpn(name, "coop_", 5)) {
        return;
    }
    s_varSeen++;
    if (s_varCount >= HZVARS_MAX) {
        return;
    }
    v = &s_vars[s_varCount++];
    HZ_Clean(name, v->name, sizeof(v->name));
    if (HZ_VarWithheld(name)) {
        Q_strncpyz(v->value, "(withheld)", sizeof(v->value));
        return;
    }
    switch (value.GetType()) {
    case VARIABLE_ARRAY:
    case VARIABLE_CONSTARRAY:
    case VARIABLE_CONTAINER:
    case VARIABLE_SAFECONTAINER:
        Com_sprintf(raw, sizeof(raw), "(%s, %d entries)", value.GetTypeName(), value.arraysize());
        break;
    case VARIABLE_REF:
    case VARIABLE_POINTER:
        Com_sprintf(raw, sizeof(raw), "(%s)", value.GetTypeName());
        break;
    default:
        try {
            Q_strncpyz(raw, value.stringValue().c_str(), sizeof(raw));
        } catch (ScriptException&) {
            Com_sprintf(raw, sizeof(raw), "(%s)", value.GetTypeName());
        }
        break;
    }
    HZ_Clean(raw, v->value, sizeof(v->value));
    if (strlen(raw) > HZVAR_VALUE) {
        strcpy(v->value + HZVAR_VALUE - 3, "...");
    }
}

static int HZ_CmpVar(const void *a, const void *b)
{
    return Q_stricmp(((const hzVar_t *)a)->name, ((const hzVar_t *)b)->name);
}

// compact = the answer to a joiner: it must fit a few KB, so the script errors are a summary and the variables stop
// when the room runs out
static void HZ_TextMission(hzOut_t *o, int clientNum, bool compact)
{
    int counts[4];
    int i, connected = 0;

    HZ_Counts(counts);
    HZ_Out(o, "level time=%.1f s  map=%s\n", level.time, level.mapname.c_str());
    HZ_Out(o, "living AI: enemy %d, friendly %d | server entities in use %d of %d\n", counts[1], counts[2], counts[0],
           globals.max_entities);

    for (i = 0; i < game.maxclients; i++) {
        gentity_t *ed = &g_entities[i];
        if (ed->inuse && ed->client && ed->entity) {
            connected++;
        }
    }
    HZ_Out(o, "players: %d living of %d connected\n", counts[3], connected);
    for (i = 0; i < game.maxclients; i++) {
        gentity_t  *ed = &g_entities[i];
        Player     *p;
        const char *state;
        char        name[40];

        if (!ed->inuse || !ed->client || !ed->entity) {
            continue;
        }
        p = (Player *)ed->entity;
        if (p->IsSpectator()) {
            state = "SPECTATING";
        } else if (p->deadflag != DEAD_NO || p->health <= 0) {
            state = "DEAD";
        } else if (p->IsCoopDbno()) {
            state = "DOWN (dbno)";
        } else {
            state = "alive";
        }
        HZ_Clean(ed->client->pers.netname, name, sizeof(name));
        HZ_Out(o, "  client %d %s: %s, health %d/%d, at (%d %d %d)%s\n", i, name, state, (int)p->health, (int)p->max_health,
               (int)p->origin[0], (int)p->origin[1], (int)p->origin[2], i == clientNum ? "  <- the reporting player" : "");
    }

    if (clientNum >= 0 && clientNum < game.maxclients && g_entities[clientNum].inuse && g_entities[clientNum].client
        && g_entities[clientNum].entity) {
        Player *p   = (Player *)g_entities[clientNum].entity;
        Vector  eye = p->EyePosition();
        Vector  ang = p->GetViewAngles();
        Vector  fwd;
        trace_t tr;

        ang.AngleVectors(&fwd);
        tr = G_Trace(eye, vec_zero, vec_zero, eye + fwd * 8192.0f, p, MASK_SHOT, qfalse, "G_HzmDiag");
        HZ_Out(o, "reporting player: origin (%.0f %.0f %.0f) view angles (%.0f %.0f)\n", p->origin[0], p->origin[1], p->origin[2],
               ang[0], ang[1]);
        if (tr.fraction >= 1.0f || !tr.ent || !tr.ent->entity) {
            HZ_Out(o, "under the crosshair: nothing within 8192\n");
        } else if (tr.ent->entity->entnum == ENTITYNUM_WORLD) {
            HZ_Out(o, "under the crosshair: world, %.0f units away\n", tr.fraction * 8192.0f);
        } else {
            Entity *e = tr.ent->entity;
            char    tn[48], md[72];
            HZ_Clean(e->targetname.c_str(), tn, sizeof(tn));
            HZ_Clean(e->model.c_str(), md, sizeof(md));
            HZ_Out(o, "under the crosshair: entity %d class %s targetname '%s' model '%s' health %d, %.0f units away\n", e->entnum,
                   e->getClassname(), tn, md, (int)e->health, tr.fraction * 8192.0f);
        }
    }

    if (compact) {
        HZ_TextScriptErrors(o, 3, false);
    }

    s_varCount = s_varSeen = 0;
    level.Vars()->HzmForEach(HZ_VarCb, NULL);
    qsort(s_vars, s_varCount, sizeof(s_vars[0]), HZ_CmpVar);
    HZ_Out(o, "level.coop_* script variables: %d (showing %s%d, values cut at %d characters)\n", s_varSeen,
           compact ? "as many as fit of " : "", s_varCount, HZVAR_VALUE);
    for (i = 0; i < s_varCount; i++) {
        if (compact && o->size - o->len < 160) {
            HZ_Out(o, "  (... %d more not sent)\n", s_varCount - i);
            break;
        }
        HZ_Out(o, "  %s = %s\n", s_vars[i].name, s_vars[i].value);
    }
}

/*
====================
G_HzmDiag - what: 1 mission state (full), 2 mission state for a joiner (compact), 3 script errors, 4 counts (buf is
int[4]: entities in use, living AI enemy, living AI friendly, living players), 5 script errors, short (newest 12, empty
when there were none). Returns the text length (4 for counts).
====================
*/
extern "C" Q_EXPORT int QDECL G_HzmDiag(int what, int clientNum, char *buf, int size)
{
    hzOut_t o;

    if (!buf || size <= 0) {
        return 0;
    }
    o.p    = buf;
    o.size = size;
    o.len  = 0;
    try {
        switch (what) {
        case 1:
        case 2:
            buf[0] = 0;
            HZ_TextMission(&o, clientNum, what == 2);
            break;
        case 3:
            buf[0] = 0;
            HZ_TextScriptErrors(&o, 0, true);
            break;
        case 5: // the short form the server keeps across a map change (this DLL is unloaded with the map)
            buf[0] = 0;
            if (s_seTotal > 0) {
                HZ_TextScriptErrors(&o, 12, false);
            }
            break;
        case 4:
            if (size < (int)(4 * sizeof(int))) {
                return 0;
            }
            HZ_Counts((int *)buf);
            return 4;
        default:
            buf[0] = 0;
            break;
        }
    } catch (...) {
        HZ_Out(&o, "(the game threw while collecting this)\n");
    }
    return o.len;
}
