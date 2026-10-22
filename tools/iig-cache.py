#!/usr/bin/env python3
"""Reuse Xcode 15 IIG outputs only with byte-identical XNU interface inputs.

The cache is a separate build artifact, never kernel binaries or SDK files.
This keeps xnu-8796's IORPC ABI when building with a newer host Xcode.
"""
import hashlib
import json
import lzma
import os
from pathlib import Path
import sys


def run(argv):
    cache = json.loads(lzma.decompress(Path(os.environ['IIG_CACHE']).read_bytes()))
    if cache['schema'] != 1:
        raise ValueError('unsupported IIG cache schema')
    root = Path(os.environ['IIG_SOURCE_ROOT']).resolve()
    for relative, expected in cache['inputs'].items():
        source = (root / relative).resolve()
        if root not in source.parents:
            raise ValueError('invalid cached input path')
        if hashlib.sha256(source.read_bytes()).hexdigest() != expected:
            raise ValueError(f'IIG input changed; regenerate with Xcode 15: {relative}')
    def option(name):
        return Path(argv[argv.index(name) + 1])
    definition = option('--def').resolve()
    if definition.relative_to(root).as_posix() not in cache['inputs']:
        raise ValueError(f'uncached IIG definition: {definition}')
    outputs = [(option('--header'), definition.stem + '.h'),
               (option('--impl'), definition.stem + '.iig.cpp')]
    for destination, key in outputs:
        if destination.name != key or key not in cache['files']:
            raise ValueError(f'uncached IIG output: {destination}')
    for destination, key in outputs:
        destination.write_text(cache['files'][key], encoding='utf8', newline='\n')
    print(f'IIG cache: {definition.name} (verified XNU inputs, Xcode 15 ABI)')


if __name__ == '__main__':
    try:
        run(sys.argv[1:])
    except (KeyError, ValueError, OSError) as exc:
        sys.exit(f'IIG cache refused: {exc}')
