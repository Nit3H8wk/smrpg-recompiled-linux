#!/usr/bin/env python3
# tools/check_lang.py: checks the interface's Spanish texts, runtime/ui/lang_es.inc (the settings menu, tr() in
# runtime/ui/tr.h) and tools/installer/lang_es.inc (the installer). For each one:
#   - every English text of the table is in the code (an edited text leaves its line behind);
#   - both texts have the same printf formats, in the same order;
#   - every literal given straight to tr() has its line;
#   - the menu's texts only have Latin-1 characters (its font has no others).
# Usage: python tools/check_lang.py  (from the repository's folder; exit code 1 if something is wrong)
import glob
import os
import re
import sys

LIT = r'L?"(?:\\.|[^"\\])*"'
FMT = re.compile(r'%(?:%|[-+#0]*[0-9.*]*(?:hh|h|ll|l|z|j|t)?[a-zA-Z])')
MACROS = {'APP_NAME': 'SMRPG Recomp', 'APP_VERSION': '0.0.0'}


def text(expr):
    """The text of a C expression made of literals and macros."""
    out = ''
    for tok in re.findall(LIT + r'|[A-Za-z_]\w*', expr):
        if tok.startswith(('"', 'L"')):
            out += tok[tok.index('"') + 1:-1]
        else:
            out += MACROS.get(tok, '{' + tok + '}')
    return out


def rows(path):
    t = re.sub(r'(?m)^\s*//[^\n]*$', '', open(path, encoding='utf-8').read())
    elem = r'((?:' + LIT + r'|APP_NAME|APP_VERSION|\s)+)'
    for m in re.finditer(r'\{' + elem + ',' + elem + r'\},', t):
        yield text(m.group(1)), text(m.group(2))


def code(files):
    src = ''
    for f in files:
        t = open(f, encoding='utf-8').read()
        t = re.sub(r'(?m)^\s*//[^\n]*$', '', t)
        for k, v in MACROS.items():
            t = re.sub(r'\b' + k + r'\b', '"' + v + '"', t)
        src += re.sub(r'"\s*L?"', '', t) + '\n'  # (adjacent literals, joined)
    return src


def check(table, files, latin1, calls):
    bad = 0
    pairs = list(rows(table))
    src = code(files)
    known = {en for en, _ in pairs}
    for en, es in pairs:
        if not re.match(r'(key|pad|input):', en) and en not in src:
            print(f'{table}: not in the code: {en[:90]}')
            bad += 1
        if FMT.findall(en) != FMT.findall(es):
            print(f'{table}: different formats: {en[:60]} | {es[:60]}')
            bad += 1
        if latin1 and any(ord(c) > 255 for c in en + es):
            print(f'{table}: not Latin-1: {en[:60]} | {es[:60]}')
            bad += 1
    for m in re.finditer(calls + r'\(\s*(' + LIT + r')\s*\)', src):
        en = text(m.group(1))
        if en not in known:
            print(f'{table}: missing: {en[:90]}')
            bad += 1
    print(f'{table}: {len(pairs)} texts')
    return bad


def main():
    bad = check('runtime/ui/lang_es.inc',
                [f for p in ('runtime/*.c', 'runtime/ui/*.mm', 'runtime/nvn/vulkan/*.c', 'runtime/nvn/vulkan/*.cpp') for f in glob.glob(p)],
                True, r'\btr')
    if os.path.exists('tools/installer/lang_es.inc'):
        bad += check('tools/installer/lang_es.inc', ['tools/installer/installer.c', 'tools/installer/game_builder.cpp'], False,
                     r'\b(?:tr|trw|TR)')
    sys.exit(1 if bad else 0)


main()
