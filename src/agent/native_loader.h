#ifndef VCO_AGENT_NATIVE_LOADER_H
#define VCO_AGENT_NATIVE_LOADER_H
/* Resident kernel module startup only; no runtime unload API. Failure details
 * are retained and reported without changing the game's startup return value. */
int vco_agent_loader_start(void);
#endif
