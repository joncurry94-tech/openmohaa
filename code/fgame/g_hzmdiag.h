/*
===========================================================================
HZM coop [bug-3486] - game.dll's part of the in-game bug report (see qcommon/hzm_diag.h). g_hzmdiag.cpp.

Script errors are remembered here whether or not `developer` is set: the console prints are developer-gated and stay
exactly as they were, so a normal player's report used to arrive with no script errors at all.
===========================================================================
*/

#pragma once

// the source position the script engine is about to print for an error (gamescript.cpp PrintSourcePos)
void G_HzmScriptErrorPos(const char *file, int line, const char *sourceLine);
// one script error / compile failure; `kind` is a short label ("Script Error", "compile", "thread", ...)
void G_HzmScriptError(const char *kind, const char *text);
