# SPDX-License-Identifier: GPL-2.0-or-later
"""The one game build this port targets. Other builds are refused until reviewed."""
TITLE_ID = 'CUSA08014'
TITLE = 'Knack II'
# sha256 of the decrypted retail eboot.bin for CUSA08014, taken from an actual dump.
# TODO: fill this in from the dump this port is built against, then keep it fixed. Until it is set,
# prepare.py refuses to run: an empty hash would accept any eboot, and every address in our notes
# would silently mean something else. The regional builds CUSA07670, CUSA08095 and CUSA04774 are
# separate executables and are refused even once this hash is known.
EXPECTED_EBOOT = ''
# The game's own libc and the Fios2 it imports, linked when the dump has them; otherwise the
# runtime's libc (src/libc*.c) stands in. Which modules this game ships in sce_module is unknown
# until a dump is inspected, so the list starts as the two every title so far has had.
BUNDLED_MODULES = ('libc.prx', 'libSceFios2.prx')


def bundled_modules(game):
    present = [name for name in BUNDLED_MODULES if (game / 'sce_module' / name).is_file()]
    if present and len(present) != len(BUNDLED_MODULES):
        raise ValueError('sce_module has only part of ' + ', '.join(BUNDLED_MODULES))
    return present
