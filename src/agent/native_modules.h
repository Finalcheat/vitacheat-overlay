#ifndef VCO_GAME_NATIVE_MODULES_H
#define VCO_GAME_NATIVE_MODULES_H
/* Eight resident hooks on the public SceModulemgr syscall exports. No hook on
 * private process destruction or UID callbacks; no per-game import registry. */
int vco_agent_modules_start(void);
#endif
