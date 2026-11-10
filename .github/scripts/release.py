"""Publish only a tested main commit and the installer pinned in latest.json."""
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import urllib.error
import urllib.parse
import urllib.request

ROOT = Path(__file__).resolve().parents[2]
WORKFLOWS = ('build.yml', 'vector-bundle-tests.yml', 'vector-tests.yml')


def git(*args):
    return subprocess.check_output(['git', *args], cwd=ROOT, text=True).strip()


def api(path, method='GET', data=None, allow_missing=False):
    request = urllib.request.Request(
        'https://api.github.com/repos/' + os.environ['GITHUB_REPOSITORY'] + '/' + path,
        data=None if data is None else json.dumps(data).encode(),
        method=method,
        headers={'Authorization': 'Bearer ' + os.environ['GH_TOKEN'],
                 'Accept': 'application/vnd.github+json',
                 'X-GitHub-Api-Version': '2022-11-28',
                 'Content-Type': 'application/json'},
    )
    try:
        with urllib.request.urlopen(request) as response:
            return json.load(response)
    except urllib.error.HTTPError as error:
        if allow_missing and error.code == 404:
            return None
        raise RuntimeError(f'GitHub API failed: {method} {path}: HTTP {error.code}') from error


def validate():
    metadata = json.loads((ROOT / 'latest.json').read_text(encoding='utf-8'))
    version = metadata['version']
    if not re.fullmatch(r'[0-9]+\.[0-9]+\.[0-9]+', version):
        raise ValueError('Expected a stable version such as 9.8.6')
    installer = ROOT / 'dist' / 'Vizu-Setup.exe'
    digest = hashlib.sha256(installer.read_bytes()).hexdigest()
    if digest != metadata['sha256'].lower():
        raise ValueError('Installer does not match latest.json SHA-256')
    if installer.read_bytes()[:2] != b'MZ':
        raise ValueError('Installer is not a Windows executable')
    resource = (ROOT / 'Vizu.rc').read_text(encoding='utf-8')
    if not re.search(r'VALUE\s+"FileVersion",\s*"' + re.escape(version) + r'"', resource):
        raise ValueError('Installer source version does not match latest.json')
    return version, installer, digest


def verify():
    version, _, _ = validate()
    sha = git('rev-parse', 'HEAD')
    main_sha = api('git/ref/heads/main')['object']['sha']
    ready = sha == main_sha
    ref = api('git/ref/tags/v' + version, allow_missing=True)
    if ref:
        obj = ref['object']
        while obj['type'] == 'tag':
            obj = api('git/tags/' + obj['sha'])['object']
        if obj['sha'] != sha:
            print(f'v{version} already belongs to another commit; no new version to publish')
            ready = False
    for workflow in WORKFLOWS:
        query = urllib.parse.urlencode({'branch': 'main', 'event': 'push', 'head_sha': sha, 'per_page': 100})
        runs = api(f'actions/workflows/{workflow}/runs?{query}')['workflow_runs']
        runs = [run for run in runs if run['head_sha'] == sha and run['head_branch'] == 'main']
        newest = max(runs, key=lambda run: run['run_number'], default=None)
        passed = newest is not None and newest['status'] == 'completed' and newest['conclusion'] == 'success'
        print(f'{workflow}: {"passed" if passed else "waiting or failed"}')
        ready = ready and passed
    with open(os.environ['GITHUB_OUTPUT'], 'a', encoding='utf-8') as output:
        output.write(f'ready={str(ready).lower()}\nsha={sha}\n')


def publish():
    version, installer, digest = validate()
    sha = git('rev-parse', 'HEAD')
    if api('git/ref/heads/main')['object']['sha'] != sha:
        raise ValueError('main changed; wait for checks on the new commit')
    tag = 'v' + version
    ref = api('git/ref/tags/' + tag, allow_missing=True)
    if ref:
        obj = ref['object']
        while obj['type'] == 'tag':
            obj = api('git/tags/' + obj['sha'])['object']
        if obj['sha'] != sha:
            raise ValueError(f'{tag} already points to another commit; bump the version')
    else:
        api('git/refs', 'POST', {'ref': 'refs/tags/' + tag, 'sha': sha})
    release = api('releases/tags/' + tag, allow_missing=True)
    if release is None:
        # A previous interrupted attempt may have left an unpublished draft.
        for page in range(1, 11):
            releases = api(f'releases?per_page=100&page={page}')
            release = next((item for item in releases if item['tag_name'] == tag), None)
            if release is not None or len(releases) < 100:
                break
    if release is None:
        history = (ROOT / 'README.md').read_text(encoding='utf-8')
        entry = next((line for line in history.splitlines() if line.startswith(f'- **v{version}**')), '')
        notes = (entry + '\n\n' if entry else '') + 'Para instalar ou atualizar, baixe e execute **Vizu-Setup.exe**.\n\n'
        notes += f'Compilação e testes aprovados para o commit `{sha[:7]}`.\n\nSHA-256 do instalador: `{digest}`.'
        release = api('releases', 'POST', {'tag_name': tag, 'target_commitish': sha,
                     'name': 'Vizu ' + version, 'body': notes, 'draft': True, 'prerelease': False})
    assets = api(f'releases/{release["id"]}/assets?per_page=100')
    asset = next((item for item in assets if item['name'] == installer.name), None)
    if asset is None:
        subprocess.run(['gh', 'release', 'upload', tag, str(installer), '--repo', os.environ['GITHUB_REPOSITORY']], check=True)
        assets = api(f'releases/{release["id"]}/assets?per_page=100')
        asset = next((item for item in assets if item['name'] == installer.name), None)
    if asset is None or asset.get('digest') != 'sha256:' + digest or asset.get('size') != installer.stat().st_size:
        raise ValueError('Release asset is missing or differs from the verified installer; refusing to overwrite it')
    if release['draft']:
        release = api(f'releases/{release["id"]}', 'PATCH', {'draft': False, 'make_latest': 'true'})
    print('Release ready: ' + release['html_url'])
    summary = os.environ.get('GITHUB_STEP_SUMMARY')
    if summary:
        with open(summary, 'a', encoding='utf-8') as output:
            output.write(f'Published [{tag}]({release["html_url"]}) with verified Vizu-Setup.exe.\n')


if __name__ == '__main__':
    command = sys.argv[1]
    if command == 'check':
        print('Validated installer:', validate()[0])
    elif command == 'verify':
        verify()
    elif command == 'publish':
        publish()
    else:
        raise SystemExit('Usage: release.py check|verify|publish')
