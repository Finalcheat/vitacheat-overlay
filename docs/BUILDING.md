# Building

## Dependencies

- VitaSDK with its compiler, import stubs, and SELF tools.
- CMake 3.16+, GNU Make, Git, and Python 3.10+.
- A C99/C++11 compiler for the independent host suite.

PAF declarations are fetched at commit `7b1d3b6aca81131fd90c65b81f317909e4feda1c`. The Linux CI SDK is pinned by URL and SHA-256 in `scripts/ci-setup-vitasdk.sh`; the helper installs only into an unused directory.

## Plugins and package

```sh
export VITASDK=/path/to/vitasdk
export PATH="$VITASDK/bin:$PATH"
sh scripts/setup.sh
sh scripts/build.sh
python3 scripts/package.py
python3 scripts/package.py --verify dist/VitaCheatOverlay-0.8.50.zip
```

The build emits four plugins and `vco-base.rco`. It checks the generated CP936 table, UI palette, Game SELF layout, and raw ARM syscall signatures. Packaging verifies source/artifact hashes and includes the MIT license and third-party notices.

## Host suite

```sh
sh scripts/test.sh
```

VitaSDK and PAF are unnecessary for host tests. AddressSanitizer and UndefinedBehaviorSanitizer are enabled by default. Use `-DVCO_SANITIZE=OFF` only for an environment without their runtime support. CI runs the sanitized suite on Linux and macOS.

## Optional tools

```sh
sh scripts/build-harness.sh
python3 scripts/package.py --with-harness --output dist/VitaCheatOverlay-0.8.50-with-harness.zip
cmake -S . -B build/sdk-checks -DCMAKE_BUILD_TYPE=Release -DVCO_BUILD_SDK_CHECKS=ON
cmake --build build/sdk-checks --parallel 4
```

The harness needs VitaSDK samples/common, configurable through `VCO_SDK_COMMON`. SDK checks emit additional link-probe ELFs rather than deployable plugins. See [Testing](TESTING.md) for the device harness and [Releasing](RELEASING.md) for release workflows.
