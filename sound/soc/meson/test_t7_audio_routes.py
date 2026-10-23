#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Check compiled digital routes and preserve the deployed HDMI RX DT contract.

Requires cpp and dtc. --baseline is the audio-test DTB from the series base.
The fixture describes hypothetical external endpoints, not VIM4 wiring. Graph
reachability means a possible mux path; it cannot prove a running ALSA stream.
"""
import argparse
from collections import defaultdict
from pathlib import Path
import re
import struct
import subprocess
import tempfile


def cells(value):
    return struct.unpack(f'>{len(value) // 4}I', value)


def strings(value):
    return value.rstrip(b'\0').decode().split('\0')


class Tree:
    def __init__(self, filename):
        data = Path(filename).read_bytes()
        magic, _, offset, string_offset = struct.unpack_from('>4I', data)
        assert magic == 0xd00dfeed, 'invalid DTB magic'
        self.nodes = {}
        stack = []
        while True:
            token, = struct.unpack_from('>I', data, offset)
            offset += 4
            if token == 1:
                end = data.index(b'\0', offset)
                stack.append(data[offset:end].decode())
                self.nodes['/'.join(stack) or '/'] = {}
                offset = (end + 4) & ~3
            elif token == 2:
                stack.pop()
            elif token == 3:
                size, name_offset = struct.unpack_from('>2I', data, offset)
                offset += 8
                name_start = string_offset + name_offset
                name_end = data.index(b'\0', name_start)
                name = data[name_start:name_end].decode()
                self.nodes['/'.join(stack) or '/'][name] = data[offset:offset + size]
                offset = (offset + size + 3) & ~3
            elif token == 9:
                break
            else:
                assert token == 4, f'unknown FDT token {token}'
        self.handles = {cells(props['phandle'])[0]: path
                        for path, props in self.nodes.items() if 'phandle' in props}

    def prefix(self, name):
        matches = [path for path, props in self.nodes.items()
                   if props.get('sound-name-prefix') == name.encode() + b'\0']
        assert len(matches) == 1, (name, matches)
        return matches[0]

    def canonical(self, path):
        """Resolve references so newly allocated phandles do not mask changes."""
        providers = {'clocks': '#clock-cells', 'assigned-clocks': '#clock-cells',
                     'resets': '#reset-cells', 'sound-dai': '#sound-dai-cells',
                     'power-domains': '#power-domain-cells',
                     'phys': '#phy-cells', 'iommus': '#iommu-cells'}
        result = {}
        for node, props in self.nodes.items():
            if node != path and not node.startswith(path + '/'):
                continue
            canonical = {}
            for key, value in props.items():
                if key in ('phandle', 'linux,phandle'):
                    continue
                if key in providers:
                    values = list(cells(value))
                    refs = []
                    while values:
                        provider = self.handles[values.pop(0)]
                        count, = cells(self.nodes[provider][providers[key]])
                        refs.append((provider, tuple(values[:count])))
                        del values[:count]
                    canonical[key] = refs
                elif key == 'remote-endpoint':
                    canonical[key] = self.handles[cells(value)[0]]
                else:
                    canonical[key] = value
            result[node] = canonical
        return result


def driver_routes(source, filename, array, prefix):
    text = (source / 'sound/soc/meson' / filename).read_text()
    body = text.split(f'{array}[] = {{', 1)[1].split('\n};', 1)[0]
    routes = re.findall(r'\{\s*"([^"]+)"\s*,\s*(?:NULL|"[^"]*")\s*,\s*"([^"]+)"\s*\}', body)
    assert routes, array
    return [(f'{prefix} {sink}', f'{prefix} {src}') for sink, src in routes]


def check_routes(source, tree):
    card = tree.nodes['/sound-digital']
    names = strings(card['audio-routing'])
    assert len(names) % 2 == 0
    board_routes = list(zip(names[::2], names[1::2]))
    internal = []
    for prefix in ('FRDDR_B', 'FRDDR_C'):
        internal += driver_routes(source, 'axg-frddr.c',
                                  'g12a_frddr_dapm_routes', prefix)
    for prefix in ('TODDR_B', 'TODDR_C'):
        internal += driver_routes(source, 'axg-toddr.c',
                                  'sm1_toddr_dapm_routes', prefix)
    for prefix in ('SPDIFOUT_A', 'SPDIFOUT_B'):
        internal += driver_routes(source, 'axg-spdifout.c',
                                  'axg_spdifout_dapm_routes', prefix)
    widgets = {name for route in internal for name in route}
    widgets.update(('PDM Capture', 'SPDIFIN Capture'))
    for sink, src in board_routes:
        assert sink in widgets and src in widgets, (sink, src)
    graph = defaultdict(set)
    for sink, src in internal + board_routes:
        graph[src].add(sink)

    def reaches(src, sink):
        pending, visited = [src], set()
        while pending:
            vertex = pending.pop()
            if vertex == sink:
                return True
            if vertex not in visited:
                visited.add(vertex)
                pending.extend(graph[vertex])
        return False

    for fifo in ('FRDDR_B', 'FRDDR_C'):
        for out in ('SPDIFOUT_A', 'SPDIFOUT_B'):
            assert reaches(f'{fifo} Playback', f'{out} Playback'), (fifo, out)
    assert reaches('PDM Capture', 'TODDR_B Capture')
    assert reaches('SPDIFIN Capture', 'TODDR_C Capture')
    assert not reaches('PDM Capture', 'TODDR_C Capture')
    assert not reaches('SPDIFIN Capture', 'TODDR_B Capture')

    # A CPU DAI must not be owned by both cards or take the display allocation.
    cpus = []
    for path, props in tree.nodes.items():
        if re.fullmatch(r'/sound-digital/dai-link-\d+', path):
            cpu, = cells(props['sound-dai'])
            cpus.append(tree.handles[cpu])
    assert len(cpus) == len(set(cpus))
    for reserved in ('TODDR_A', 'FRDDR_A', 'TDM_A'):
        assert tree.prefix(reserved) not in cpus, reserved


def check_resources(tree):
    regions = []
    for path, props in tree.nodes.items():
        if 'sound-name-prefix' not in props or 'reg' not in props:
            continue
        values = cells(props['reg'])
        assert len(values) % 4 == 0, path
        for index in range(0, len(values), 4):
            hi, lo, size_hi, size_lo = values[index:index + 4]
            address, size = hi << 32 | lo, size_hi << 32 | size_lo
            assert size > 0, path
            regions.append((path.rsplit('/', 1)[0], address, address + size, path))
    for index, (parent, start, end, path) in enumerate(regions):
        for other_parent, other_start, other_end, other in regions[:index]:
            if parent == other_parent:
                assert end <= other_start or start >= other_end, (path, other)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', required=True, type=Path)
    parser.add_argument('--dtc', default='dtc')
    parser.add_argument('--output', type=Path, help='retain the compiled fixture DTB')
    args = parser.parse_args()
    source = Path(__file__).resolve().parents[3]
    fixture = Path(__file__).with_name('test-data') / 't7-audio-routes.dts'
    with tempfile.TemporaryDirectory(prefix='t7-audio-routes-') as directory:
        preprocessed = subprocess.run(
            ['cpp', '-nostdinc', '-undef', '-D__DTS__', '-x', 'assembler-with-cpp',
             '-I', str(source / 'scripts/dtc/include-prefixes'),
             '-I', str(source / 'arch/arm64/boot/dts/amlogic'), str(fixture)],
            check=True, capture_output=True).stdout
        dtb = args.output or Path(directory) / 'fixture.dtb'
        subprocess.run([args.dtc, '-q', '-I', 'dts', '-O', 'dtb', '-o', str(dtb)],
                       input=preprocessed, check=True)
        baseline, candidate = Tree(args.baseline), Tree(dtb)
        for path in ('/sound', baseline.prefix('TODDR_A')):
            assert baseline.canonical(path) == candidate.canonical(path), path
        rx = [path for path, props in baseline.nodes.items()
              if b'amlogic,t7-hdmirx\0' in props.get('compatible', b'')]
        assert len(rx) == 1, rx
        assert baseline.canonical(rx[0]) == candidate.canonical(rx[0]), 'HDMI RX'
        check_resources(candidate)
        check_routes(source, candidate)
    print('T7 audio DT: RX contract, MMIO ownership and digital route graph passed')


if __name__ == '__main__':
    main()
