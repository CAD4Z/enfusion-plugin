"""Materialize owned HDR and BC6H seeds; also useful for manual Workbench/DayZ smoke."""
import json
import pathlib
import subprocess
import sys

cli = pathlib.Path(sys.argv[1]).resolve()
folder = pathlib.Path(sys.argv[2]).resolve()
folder.mkdir(parents=True, exist_ok=True)
goldens = json.loads((pathlib.Path(__file__).parents[1] / 'workbench/hdr-goldens.json').read_text())
for index, row in enumerate(goldens['decoder']):
    (folder / f'source-{index}.hdr').write_bytes(bytes.fromhex(row['source']))
for cube in [False, True]:
    for conversion in ['none', 'hdr-compression']:
        output = folder / f'hdr-{conversion}-{"cube" if cube else "2d"}.edds'
        subprocess.run([str(cli), 'edds', 'convert', '--machine', '--protocol', '1',
            '--input', str(folder / 'source-0.hdr'), '--output', str(output),
            '--conversion', conversion, '--generate-cubemap', str(cube).lower(),
            '--format-compress', 'copy'], check=True, capture_output=True)
block_source = folder / 'bc6-block.hdr'
block_source.write_bytes(b'#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 4 +X 4\n' + bytes([128,64,32,132])*16)
output = folder / 'bc6-block.edds'
subprocess.run([str(cli), 'edds', 'convert', '--machine', '--protocol', '1', '--input', str(block_source),
    '--output', str(output), '--conversion', 'hdr-compression', '--format-compress', 'copy',
    '--generate-mips', 'false'], check=True, capture_output=True)
header = output.read_bytes()[:156]
rows = json.loads(pathlib.Path(__file__).with_name('bc6-goldens.json').read_text())['rows']
for row in rows[::12]:
    (folder / f'bc6-mode-{row["mode"]}.edds').write_bytes(header + bytes.fromhex(row['block']))
