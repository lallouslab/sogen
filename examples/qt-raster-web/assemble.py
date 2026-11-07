import argparse
import gzip
import hashlib
import json
from pathlib import Path, PurePosixPath
import shutil
from urllib.parse import quote

parser = argparse.ArgumentParser(description='Assemble a local raster example from separately built components and an existing root.')
parser.add_argument('--worker', type=Path, required=True)
parser.add_argument('--display', type=Path, required=True)
parser.add_argument('--root', type=Path, required=True)
parser.add_argument('--files', type=Path, required=True, help='JSON array of relative files selected from the root')
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--application', action='append', required=True, metavar='NAME=GUEST_PATH')
a = parser.parse_args()
a.output.mkdir(parents=True, exist_ok=True)
for name in ['index.html', 'app.js', 'worker.js', 'wire.js']:
    shutil.copy2(Path(__file__).parent / name, a.output / name)
for folder, names in [(a.worker, ['qt-raster-web.js', 'qt-raster-web.wasm']),
                      (a.display, ['raster_display.js', 'raster_display.wasm', 'qtloader.js'])]:
    for name in names:
        shutil.copy2(folder / name, a.output / name)
entries = json.loads(a.files.read_text())
if not isinstance(entries, list) or len(entries) > 512 or len(set(entries)) != len(entries):
    raise ValueError('Expected at most 512 distinct runtime paths')
root = a.root.resolve()
manifest = {'applications': dict(x.split('=', 1) for x in a.application), 'files': []}
total = 0
for name in sorted(entries):
    relative = PurePosixPath(name)
    if relative.is_absolute() or '..' in relative.parts or '\\' in name:
        raise ValueError('Invalid relative runtime path: ' + name)
    source = (root / name).resolve()
    source.relative_to(root)
    content = source.read_bytes()
    total += len(content)
    if len(content) > 256 * 1024 * 1024 or total > 512 * 1024 * 1024:
        raise ValueError('Runtime budget exceeded')
    path = 'runtime/' + name + '.gz'
    target = a.output / path
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(gzip.compress(content, compresslevel=6, mtime=0))
    manifest['files'].append(dict(path=name, bytes=len(content), sha256=hashlib.sha256(content).hexdigest(), url=quote(path), gzip=True))
(a.output / 'runtime.json').write_text(json.dumps(manifest, indent=2) + '\n')
print(len(entries), 'runtime files,', total, 'uncompressed bytes')
