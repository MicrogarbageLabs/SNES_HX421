/* hx421_gamert.c — the game side's one piece of state: the stashed table.
 * One instance per game binary; every sys_* wrapper reads it. */
#include "hx421_gamert.h"

const Hx421Sys *g_sys = 0;
