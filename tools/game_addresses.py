"""Read the game-address tables also included by the C/C++ runtime.

runtime/game_addresses/versions.h lists the supported game versions by the main program's build ID; each has its own
patches_<key>.h (used by the patch generator) and runtime_<key>.h (runtime only, so adding one does not invalidate
generated code). The headers are the authoritative data; no generated copy or Python address list needs updating
alongside them.

ADDRESS is empty until a version is selected (select_version / select_build_id): the patch tables in patches.py are
built from it on import, so select first. An unknown build ID is an error: there is no fallback table.
"""
from pathlib import Path
import re
import sys
from types import SimpleNamespace


class UnknownGameVersion(Exception):
    pass


def load_table(path):
    values = {}
    for line in Path(path).read_text(encoding='utf-8').splitlines():
        if not line.startswith('GA_ADDR'):
            continue
        match = re.fullmatch(r'GA_ADDR\(([A-Z][A-Z0-9_]*),\s+(0x[0-9a-f]+)\)', line)
        if not match or match[1] in values:
            raise ValueError(f'Invalid or duplicate game-address definition: {line}')
        values[match[1]] = int(match[2], 16)
    # NOTE(SMRPG): an empty table is allowed (e.g. a game version with no patches yet);
    # a wrong include path still fails loudly at the consumer, which indexes ADDRESS by name.
    return values


def load_versions(path):
    """{key: (label, build ID as 40 upper-case hex digits)} from versions.h, in file order."""
    versions = {}
    for line in Path(path).read_text(encoding='utf-8').splitlines():
        if not line.startswith('GA_VERSION'):
            continue
        match = re.fullmatch(r'GA_VERSION\((v\d+_\d+_\d+),\s+"([^"]+)",\s+"([0-9A-Fa-f]{32,40})"\)', line)
        if not match or match[1] in versions:
            raise ValueError(f'Invalid or duplicate game version: {line}')
        versions[match[1]] = (match[2], match[3])
    if not versions:
        raise ValueError('No game versions')
    return versions


_root = Path(sys._MEIPASS) if hasattr(sys, '_MEIPASS') else Path(__file__).resolve().parent.parent
TABLE_DIR = _root / 'runtime/game_addresses'
VERSIONS = load_versions(TABLE_DIR / 'versions.h')
GAME = 'smrpg'  # NOTE(SMRPG): this fork lifts SMRPG; 'las' enables LAS's hand-written patches/hooks


def table_path(key, kind='patches'):
    return TABLE_DIR / f'{kind}_{key}.h'


def version_for_build_id(build_id):
    """The version key for a main build ID (hex, any case; Nso.build_id strips trailing zero bytes)."""
    want = build_id.upper()
    padded = want.ljust(64, '0')  # (the header's 32-byte field: a 20-byte ID, zero-filled)
    for key, (label, bid) in VERSIONS.items():
        if padded == bid.upper().ljust(64, '0') and not padded[40:].strip('0'):
            return key
    supported = ', '.join(f'{label} ({bid[:16]})' for label, bid in VERSIONS.values())
    raise UnknownGameVersion(f'These game files are an unsupported version of the game (main build ID '
                             f'{want[:16] or "missing"}). Supported: {supported}.')


class _AddrNS(SimpleNamespace):
    """ADDRESS with a default of 0 for names absent from the selected table.

    NOTE(SMRPG): tools/patches.py reads dozens of LAS-specific addresses eagerly at import;
    with a minimal (patches-free) table those names are absent. Missing names read as 0 so
    the import succeeds and PATCHES registers only at address 0 (never a real function).
    The generated C is unaffected: it uses the GA_ADDR tables (.h), which stay strict.
    """
    def __getattr__(self, name):
        if name.isupper():
            return 0
        raise AttributeError(name)


ADDRESS = _AddrNS()
SELECTED = None


def select_version(key):
    """Fill ADDRESS from patches_<key>.h (in place: modules that imported it see the new values)."""
    global SELECTED
    if key not in VERSIONS:
        raise UnknownGameVersion(f'Unknown game version {key}')
    ADDRESS.__dict__.clear()
    ADDRESS.__dict__.update(load_table(table_path(key)))
    SELECTED = key
    return key


def select_build_id(build_id):
    return select_version(version_for_build_id(build_id))


def addr_text(code):
    """Expand @NAME@ as hex digits inside generated-C addresses, symbols or labels."""
    return re.sub(r'@([A-Z][A-Z0-9_]*)@', lambda m: f'{getattr(ADDRESS, m[1]):x}', code)
