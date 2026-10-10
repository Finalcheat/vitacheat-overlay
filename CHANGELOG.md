# Changelog

## 0.8.50

Initial repository release of VitaCheat Overlay.

- Full-screen cheat-list UI with game title, installed version, Title ID, pagination, selection highlighting, and capsule switches.
- Direct UTF-8/GBK `.psv` loading from the shared VitaCheat database directory.
- Independent implementation of the public VitaCheat code families, including instruction-patch restoration.
- English, Simplified Chinese, Traditional Chinese, Japanese, and Korean menus, with system-language selection.
- Configurable opening shortcut, adjustable opacity, persistent settings, and regional confirm/back conventions.
- New sessions start with all entries OFF; Square remains the fixed cheat toggle.

Settings use schema v6 and accept v1–v5 for migration. When downgrading, restore the matching settings backup or remove only the VCO settings file to reset it.
