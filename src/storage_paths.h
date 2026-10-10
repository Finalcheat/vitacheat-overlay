#ifndef VCO_STORAGE_PATHS_H
#define VCO_STORAGE_PATHS_H
/* Share VitaCheat's existing directory and database. VCO-owned files have
 * distinct names; neither loader nor catalog creates a private cheat database. */
#define VCO_STORAGE_DIR "ux0:vitacheat"
#define VCO_CHEAT_DB_DIR VCO_STORAGE_DIR "/db/"
#define VCO_KERNEL_PATH VCO_STORAGE_DIR "/VitaCheatOverlayKernel.skprx"
#define VCO_LOADER_PATH VCO_STORAGE_DIR "/VitaCheatOverlayShell.suprx"
#define VCO_UI_PATH VCO_STORAGE_DIR "/VitaCheatOverlayUI.suprx"
#define VCO_GAME_PATH VCO_STORAGE_DIR "/VitaCheatOverlayGame.suprx"
#define VCO_RESOURCE_PATH VCO_STORAGE_DIR "/vco-base.rco"
#define VCO_SETTINGS_PATH VCO_STORAGE_DIR "/vco-settings.ini"
#define VCO_SETTINGS_TEMP VCO_SETTINGS_PATH ".tmp"
#define VCO_SETTINGS_BACKUP VCO_SETTINGS_PATH ".bak"
#define VCO_KERNEL_LOG VCO_STORAGE_DIR "/vco-kernel.log"
#define VCO_LOADER_LOG VCO_STORAGE_DIR "/vco-loader.log"
#define VCO_SHELL_LOG VCO_STORAGE_DIR "/vco-shell.log"
#define VCO_CATALOG_LOG VCO_STORAGE_DIR "/vco-catalog.log"
#define VCO_NATIVE_LOG VCO_STORAGE_DIR "/vco-native.log"
#endif
