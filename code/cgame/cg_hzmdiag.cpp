/*
===========================================================================
HZM coop [bug-3486] - cgame's numbers for the in-game bug report (see qcommon/hzm_diag.h).

The exe finds CG_HzmDiag with Sys_LoadFunction about once a second while in a map, so an older cgame.dll without it
simply reports nothing. Read-only: two list walks, nothing changed.
===========================================================================
*/

#include "cg_local.h"
#include "cg_commands.h"

/*
====================
ClientGameCommandManager::HzmDiagCounts - out[0] temp models alive, out[1] emitters alive
====================
*/
int ClientGameCommandManager::HzmDiagCounts(int *out, int n)
{
    int           tempModels = 0;
    ctempmodel_t *p;

    // the active list is circular around the m_active_tempmodels sentinel; bounded in case it is ever torn
    for (p = m_active_tempmodels.next; p && p != &m_active_tempmodels && tempModels < MAX_TEMPMODELS; p = p->next) {
        tempModels++;
    }
    if (n > 0) {
        out[0] = tempModels;
    }
    if (n > 1) {
        out[1] = m_emitters.NumObjects();
    }
    return n > 2 ? 2 : n;
}

extern "C" Q_EXPORT int QDECL CG_HzmDiag(int *out, int n)
{
    if (!out || n <= 0) {
        return 0;
    }
    return commandManager.HzmDiagCounts(out, n);
}
