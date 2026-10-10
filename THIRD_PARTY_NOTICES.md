# Third-party notices

Project-authored code is licensed under the [MIT License](LICENSE). The terms below apply to the named third-party materials independently.

| Material | Origin | Included notice / status |
| --- | --- | --- |
| taiHEN declarations and import specification | [henkaku/taiHEN](https://github.com/henkaku/taiHEN) | MIT, Copyright © 2016 Yifan Lu; [full notice](vendor/taihen/LICENSE) |
| `resources/base.rco` native UI template | [GrapheneCt/PSVshellPlus](https://github.com/GrapheneCt/PSVshellPlus) | MIT, Copyright © 2022 GrapheneCt; [full notice](resources/PSVshellPlus-LICENSE) |
| Microsoft CP936 mapping and derived decoder table | [Unicode mapping archive](https://www.unicode.org/Public/MAPPINGS/VENDORS/MICSFT/WINDOWS/CP936.TXT) | Unicode License V3; [full notice](vendor/unicode/LICENSE.txt), [mapping provenance](vendor/unicode/README.md) |
| PAF headers and NID stubs | [GrapheneCt/ScePaf-RE](https://github.com/GrapheneCt/ScePaf-RE/tree/7b1d3b6aca81131fd90c65b81f317909e4feda1c) | Fetched externally by setup at the pinned commit; not vendored. That checkout has no standalone license file; no MIT or other license is inferred for it. |
| Optional harness debug screen | [VitaSDK samples](https://github.com/vitasdk/samples) | Linked from the developer's SDK samples/common; upstream sample code declares CC0 1.0; [harness notes](tools/device-harness/THIRD-PARTY.md) |
| Optional harness debug font | [PSPSDK](https://github.com/pspdev/pspsdk) | BSD notice for Marcus R. Brown, James Forshaw, and John Kelley; [full notice](tools/device-harness/PSPSDK-LICENSE.txt) |

VitaSDK tools and libraries are supplied by the developer and retain their respective upstream terms. Dependencies fetched or linked during a build are not re-licensed by this project.

The normal binary package includes the taiHEN, Unicode, and PSVshellPlus notices. A package containing the optional harness also includes its notices. Preserve these when redistributing those files.

No original VitaCheat executable, Sony firmware, or game binary is included. VitaCheat's public format documentation is a reference for the independent implementation; the name does not imply an official fork or affiliation.
