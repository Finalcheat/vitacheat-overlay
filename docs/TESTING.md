# Testing

Host tests validate software contracts. VitaSDK builds validate import/link/layout contracts. Hardware tests validate a particular plugin set on an actual device. These forms of evidence are complementary.

## Host suite

```sh
sh scripts/test.sh
```

The suite uses a normal C/C++ compiler and Python 3.10+, with AddressSanitizer and UndefinedBehaviorSanitizer enabled by default. Fake SDK/PAF interfaces test failure and concurrency protocols without a Vita. No firmware dump, original VitaCheat binary, or captured device directory is required.

Coverage includes code parsing/execution, GBK decoding against Python's CP936 codec, catalog and SFO metadata, input neutrality, regional controls, language tables, settings migration/storage, copied protocols, worker state, patch ownership/restoration, public module changes, and stale-build/install/rollback rejection.

To select tests after configuring/building:

```sh
ctest --test-dir build/host -R 'cheats_|game_memory_|settings_' --output-on-failure
```

`.github/workflows/ci.yml` runs the host suite on Linux and macOS. It does not install plugins, connect to a device, or qualify a binary release.

## VitaSDK build checks

```sh
export VITASDK=/path/to/vitasdk
sh scripts/setup.sh
sh scripts/build.sh
```

The build checks the generated CP936 table, UI resource generation, palette ABI, the Game SELF's file-backed RW segment, and the exported ARM syscall argument contract. A successful build records hashes; it does not establish that the plugin will load on every firmware.

Optional link probes compile older platform adapters and extra SDK import paths, without producing additional deployable plugins:

```sh
cmake -S . -B build/sdk-checks -DCMAKE_BUILD_TYPE=Release -DVCO_BUILD_SDK_CHECKS=ON
cmake --build build/sdk-checks --parallel 4
```

## Device harness

`tools/device-harness` is an independent homebrew application, Title ID `VCOCH0001`, version `00.95`. It allocates controlled test memory and includes two unloadable fixture modules. Generated addresses belong only to that application.

```sh
sh scripts/build-harness.sh
python3 scripts/package.py --with-harness
```

Install `build/harness/VcoDeviceHarness.vpk` in VitaShell. The build requires VitaSDK samples/common; if absent, provide that source directory with `-DVCO_SDK_COMMON=/path/to/samples/common` when configuring `build/harness`, then run the build script again.

The application generates `ux0:vitacheat/db/VCOCH0001.psv`. Keep the original VitaCheat's entries OFF during VCO checks, since the first generated entry deliberately uses `_V1`. Test one overlapping data/patch case at a time.

| Cases | Observe after enabling, then disabling |
| --- | --- |
| 01–03 | 32/16/8-bit writes: 100, 25, 7; ordinary OFF stops future writes |
| 04–05 | Copy source 1234; repeated values 10, 11, 12 |
| 06–08 | Pointer write 55, copy 1234, repeated values 20, 21, 22 |
| 09–10 | Conditional write 66; R-only button condition writes 77 with overlay hidden |
| 11 | A200 data patch 5 → 88 → 5 |
| 12–13 | Absolute and B-relative A100: function 7 → 9 → 7, tested separately |
| 14 | Module-relative data write 99 |
| 15 | A200 at a two-byte boundary: raw instruction and CPU return 7 → 9 → 7 |
| 16 | 117 patch targets: stable OFF 117/0/0, ON 0/117/0, then restore |

With the overlay hidden, Cross resets data, Square sets Direct32 to 12, Triangle switches the pointer target, and Circle regenerates the ordinary code list. START switches the harness to its module page; the VCO UI itself uses Triangle for settings.

On the module page: Cross loads the next fixture, Square unloads it, Triangle generates its three code cases, and Circle starts/stops up to 100 load/unload cycles. Refresh the overlay after changing the generated file. Old module generations must not retain callable pointers or patch a new instance.

Also verify separate exits with an ordinary data entry enabled and with an A patch enabled. Relaunch without rebooting: list entries must be OFF, the function must return its initial value, and input must remain usable.

Press Down in the harness and release it to save a bounded observation. Analyze the downloaded log:

```sh
python3 scripts/harness_observations.py vco-harness-observations.jsonl --output observations.json
```

The log reports observed values, not who enabled an entry. Manual ON/OFF and actual effect observations remain part of acceptance.

## Optional resource tools

```sh
python3 scripts/resource_budget.py --package dist/VitaCheatOverlay-0.8.50.zip
sh scripts/build-resource-profile.sh
python3 scripts/resource_profile.py --help
```

The allocation probe consumes the packaged build stamp and current ARM ABI. Its segment/stack/arena totals exclude PAF objects, fonts, textures, allocator/system overhead, and taiHEN storage. The profile build is separate, developer-only, and not admitted by the ordinary packager.
