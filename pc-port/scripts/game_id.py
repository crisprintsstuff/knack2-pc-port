# SPDX-License-Identifier: GPL-2.0-or-later
"""The one game build this port targets. Other builds are refused until reviewed.

Knack II is a two-stage boot, unlike the Ghost of Tsushima build this port derives from:
`eboot.bin` is a 37 KB SELF whose whole job is to load `/app0/game.elf`, and `game.elf` is the
real 20 MB executable (its build path, `D:/knack2/bin/MGame_Master_ORBIS.elf`, is still inside it).
So there are two hashes to check, not one, and the loader has to link the stub before it can reach
the game's own entry point.
"""
TITLE_ID = 'CUSA08014'
TITLE = 'Knack 2'
# The param.sfo spells the title without the numeral: TITLE = "KNACK 2".
SFO_TITLE = 'KNACK 2'
CONTENT_ID = 'UP9000-CUSA08014_00-KNACK20000000000'
APP_VER = '01.00'

# sha256 of the decrypted retail eboot.bin, the loader stub. Read from the user's own dump on
# 2026-10-28; it is 37,006 bytes and declares only libSceSystemService, libkernel and libc.
EXPECTED_EBOOT = 'e65b5fd039833ada7dddd8c197b8b47c283a3f46320cd13ece5ff3973a4041db'
# sha256 of the decrypted /app0/game.elf the stub loads. 20,205,618 bytes, 778 imports across 43
# libraries. Both hashes are checked: the stub alone proves nothing about which game runs.
EXPECTED_GAME = '80ccc22819fa4eeb5e1824e585f3ac5d5168e9ebc82f629e44cdb63fd573fb20'

# The game's own modules, linked when the dump has them; otherwise the runtime's libc
# (src/libc*.c) stands in for the libc imports. This dump carries all three. libSceJobManager is
# new against the earlier ports: Knack II's job system goes through it, and game.elf lists
# libSceJobManager.prx in DT_NEEDED.
BUNDLED_MODULES = ('libc.prx', 'libSceFios2.prx', 'libSceJobManager.prx')


def bundled_modules(game):
    present = [name for name in BUNDLED_MODULES if (game / 'sce_module' / name).is_file()]
    if present and len(present) != len(BUNDLED_MODULES):
        raise ValueError('sce_module has only part of ' + ', '.join(BUNDLED_MODULES))
    return present
