# CP936 mapping data

The readonly GBK decoder table is derived from Unicode's Microsoft CP936 mapping,
table version 2.01 (2000-01-07), downloaded on 2026-10-04:
[CP936.TXT](https://www.unicode.org/Public/MAPPINGS/VENDORS/MICSFT/WINDOWS/CP936.TXT).
The unchanged source is retained in this directory in the source project with SHA256
`b86f601c575e9ab457380b6f7abef03c75499cc6075bdc8b4b27f3f2de74bf6a`.

The data and generated table use the [Unicode License V3](LICENSE.txt), retrieved
from [Unicode](https://www.unicode.org/license.txt). The release contains this
notice and the full license. No Microsoft or Unicode affiliation is implied.

Regenerate with `python3 scripts/generate_cp936.py`; verify with `--check`.
The table contains 21,791 assigned two-byte mappings in 23,940 slots, plus the
CP936 single-byte Euro sign (`80` -> `U+20AC`) handled by the decoder.
Undefined pairs are rejected. GB18030 four-byte sequences are not supported.
