"""Download a pinned, complete portable Inkscape for embedding in the installer."""
import hashlib
from pathlib import Path
import urllib.request

URL = 'https://media.inkscape.org/dl/resources/file/inkscape-1.4.2_2025-05-13_f4327f4-x64.7z'
SHA256 = '757e0358512630e65505a4cca89885369087f2908469439bc41b5a346604cbd4'
DEST = Path('build/engine/inkscape.7z')

def main():
    DEST.parent.mkdir(parents=True, exist_ok=True)
    if DEST.is_file() and hashlib.sha256(DEST.read_bytes()).hexdigest() == SHA256:
        print('Verified portable Inkscape already cached.'); return
    staging = DEST.with_suffix('.download')
    try:
        with urllib.request.urlopen(URL, timeout=180) as response, staging.open('wb') as output:
            digest = hashlib.sha256()
            while chunk := response.read(1024 * 1024):
                digest.update(chunk); output.write(chunk)
        if digest.hexdigest() != SHA256:
            raise RuntimeError('Inkscape archive SHA-256 mismatch; refusing to bundle.')
        staging.replace(DEST)
    finally:
        staging.unlink(missing_ok=True)
    print('Downloaded and verified portable Inkscape 1.4.2.')

if __name__ == '__main__':
    main()
