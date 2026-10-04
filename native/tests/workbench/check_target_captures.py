"""Independently read successful target captures; optionally exercise a packaged CLI.

Uses only Python's standard library. No converter or capture-oracle code is imported.
"""
import argparse
import base64
import hashlib
import json
import pathlib
import struct
import subprocess
import tempfile


def word(data, offset):
    return struct.unpack_from('<I', data, offset)[0]


def lz4(frame):
    """Read the recorded ENF1 block stream, including references into an earlier block."""
    expected = word(frame, 0)
    cursor = 4
    output = bytearray()
    last = False
    while cursor < len(frame):
        assert not last
        descriptor = word(frame, cursor)
        last = bool(descriptor & 0x80000000)
        size = descriptor & 0x7fffffff
        block = frame[cursor + 4:cursor + 4 + size]
        assert len(block) == size and size > 0
        cursor += 4 + size
        at = 0
        start = len(output)

        def length(initial):
            nonlocal at
            total = initial
            if initial == 15:
                while True:
                    extra = block[at]
                    at += 1
                    total += extra
                    if extra != 255:
                        break
            return total

        while at < len(block):
            token = block[at]
            at += 1
            literal = length(token >> 4)
            assert at + literal <= len(block) and len(output) + literal <= expected
            output.extend(block[at:at + literal])
            at += literal
            if at == len(block):
                break
            distance = struct.unpack_from('<H', block, at)[0]
            at += 2
            count = length(token & 15) + 4
            assert 0 < distance <= len(output) and len(output) + count <= expected
            for _ in range(count):
                output.append(output[-distance])
        assert len(output) - start == (expected - start if last else 65536)
    assert cursor == len(frame) and last and len(output) == expected
    return bytes(output)


def inspect(data, source):
    """Verify header, descending storage order, container boundaries and every planted sample."""
    levels = source['levels']
    first = levels[0]
    assert data[:4] == b'DDS ' and word(data, 4) == 124
    assert data[36:40] == b'ENF1' and word(data, 76) == 32
    assert (word(data, 16), word(data, 12), word(data, 28)) == (
        first['width'], first['height'], len(levels))
    assert word(data, 80) == (0x41 if source['alpha'] else 0x40)
    assert word(data, 88) == 32 and word(data, 20) == first['width'] * 4
    assert [word(data, at) for at in (92, 96, 100, 104)] == [
        0xff0000, 0xff00, 0xff, 0xff000000 if source['alpha'] else 0]
    cursor = 128 + len(levels) * 8
    containers = []
    for index, level in enumerate(reversed(levels)):
        tag = data[128 + index * 8:132 + index * 8]
        size = word(data, 132 + index * 8)
        assert 0 < size <= len(data) - cursor
        stored = data[cursor:cursor + size]
        cursor += size
        assert tag in (b'COPY', b'LZ4 ')
        decoded = stored if tag == b'COPY' else lz4(stored)
        assert decoded == bytes.fromhex(level['bgra'])
        containers.append(tag.decode().strip())
    assert cursor == len(data)
    return list(reversed(containers))


def cli_json(cli, *args):
    result = subprocess.run([str(cli), 'edds', *args, '--machine', '--protocol', '1'],
                            capture_output=True, text=True, check=True)
    return json.loads(result.stdout)


def check(capture, cli=None):
    assert capture['executable_sha256'] == '74b74d1abe1f7f91e989d33dbfdcf97f9517a3a7df4dcb808c38991fd1df5cbe'
    files = {digest: bytes.fromhex(encoded) for digest, encoded in capture['files'].items()}
    for digest, data in files.items():
        assert hashlib.sha256(data).hexdigest() == digest
    written = 0
    faults = 0
    results = {}
    with tempfile.TemporaryDirectory(prefix='enfusion-target-captures-') as directory:
        for index, row in enumerate(capture['rows']):
            source = capture['sources'][row['source']]
            result = row['result']
            key = (row['source'], row['compression'])
            results[(*key, row['target'])] = result
            if result['status'] == 'writer-fault':
                assert row['target'] == 'EnfusionDDS_LZ0' and row['compression'] != 'Copy'
                assert result['fault_rva'] == '0x42a573'
                assert result['length_overlaps_table_pointer'] and result['table_pointer_zeroed']
                assert result['file_bytes_written'] == 0 and 'edds_sha256' not in result
                assert len(bytes.fromhex(result['codec_payload'])) == result['stored_bytes']
                faults += 1
                continue
            assert result['status'] == 'written'
            data = files[result['edds_sha256']]
            containers = inspect(data, source)
            written += 1
            if cli:
                path = pathlib.Path(directory) / f'{index}.edds'
                path.write_bytes(data)
                actual = cli_json(cli, 'inspect', '--input', str(path))
                assert [mip['container'] for mip in actual['mips']] == containers
                for level, expected in enumerate(source['levels']):
                    actual = cli_json(cli, 'preview', '--input', str(path), '--mip', str(level))
                    bgra = bytes.fromhex(expected['bgra'])
                    rgba = bytes(value for at in range(0, len(bgra), 4) for value in
                                 (bgra[at + 2], bgra[at + 1], bgra[at],
                                  bgra[at + 3] if source['alpha'] else 255))
                    assert base64.b64decode(actual['pixelsBase64']) == rgba
    assert (written, faults) == (66, 18)
    for source in range(len(capture['sources'])):
        for compression in ['Copy', 'Fastest', 'Medium', 'Best']:
            key = (source, compression)
            assert results[(*key, 'EnfusionDDS')] == results[(*key, 'EnfusionDDS_LZ4')]
        key = (source, 'Copy')
        assert results[(*key, 'EnfusionDDS')] == results[(*key, 'EnfusionDDS_LZ0')]
    print(f'{written} complete files decoded independently; {faults} LZO writer failures retained as failures.'
          + (' CLI inspect and every mip preview also matched.' if cli else ''))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture', type=pathlib.Path)
    parser.add_argument('--cli', type=pathlib.Path)
    args = parser.parse_args()
    check(json.loads(args.capture.read_text(encoding='utf-8')), args.cli.resolve() if args.cli else None)
