#!/usr/bin/env python3
"""Build the native engine libraries on this Mac from the user's own firmware.

Runs the source build's generators on a private copy of the firmware, then
compiles m8aot, m8native-step and m8native-regions with the recipe recorded
from the shipped runtime's build. The output directory receives the three
libraries and, last, engine.json; without a matching engine.json the app
ignores the directory. build.status holds "<percent> <state> <text>".
Needs the Xcode Command Line Tools; capstone comes from the bundle, else a
private venv beside the engines (network once). No PGO.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import ctypes
import fcntl
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import shlex
import shutil
import signal
import subprocess
import sys
import threading
import time

HERE = Path(__file__).resolve().parent
SCHEMA = 'm8-user-engine/1'
CAPSTONE = '5.0.7'
SYMBOLS = {'m8aot': ('m8_aot_lookup', 'm8_aot_entries', 'm8_aot_verify_memory', 'm8_aot_codegen_v2'),
           'm8native-step': ('m8_native_step',),
           'm8native-regions': ('m8_native_region_step', 'm8_native_region_verify_memory',
                                'm8_native_region_chain', 'm8_native_region_event_chain')}
# The recipe keeps the shipped flags; profile data exists only on the build host.
DROPPED = ('-fprofile-', '-fno-profile-', '-ftest-coverage', '-fno-test-coverage', '-Werror=backend-plugin',
           '-Werror=missing-profile', '-Werror=coverage-mismatch')


class Failure(Exception):
    pass


CANCELLED = threading.Event()


def sha(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as file:
        for block in iter(lambda: file.read(1 << 20), b''):
            digest.update(block)
    return digest.hexdigest()


def write_atomic(path, text):
    temporary = path.with_name(path.name + '.tmp')
    temporary.write_text(text)
    os.replace(temporary, path)


class Status:
    """Single compiles take minutes, so a phase ramps toward its end with time."""
    def __init__(self, path):
        self.path, self.percent, self.start, self.lock = path, 0, time.monotonic(), threading.RLock()
        self.text, self.ramp = '', None
        threading.Thread(target=self.tick, daemon=True).start()

    def __call__(self, percent, text, state='running'):
        with self.lock:
            if state != 'running':
                self.ramp = None
            self.percent, self.text = max(self.percent, percent), text
            write_atomic(self.path, f'{self.percent} {state} {text}\n')
            print(f'[{self.percent:3d}% {time.monotonic() - self.start:6.1f}s] {text}', flush=True)

    def phase(self, start, end, seconds, text):
        with self.lock:
            self.ramp = (start, end, seconds, time.monotonic())
            self(start, text)

    def tick(self):
        while True:
            time.sleep(1)
            with self.lock:
                if self.ramp:
                    start, end, seconds, began = self.ramp
                    value = start + (end - start) * (1 - math.exp(-(time.monotonic() - began) / seconds))
                    if int(value) > self.percent:
                        self.percent = int(value)
                        write_atomic(self.path, f'{self.percent} running {self.text}\n')


def capstone_ready(python, path=None):
    env = dict(os.environ, PYTHONPATH=str(path)) if path else os.environ
    # The distribution version: later wheels still report core 5.0.7.
    check = f"import capstone,importlib.metadata as m,sys;sys.exit(m.version('capstone')!='{CAPSTONE}')"
    return subprocess.run([python, '-c', check], env=env, stdout=subprocess.DEVNULL,
                          stderr=subprocess.DEVNULL, timeout=60).returncode == 0


def interpreter(source, root, status):
    """Return (python, PYTHONPATH) with the pinned capstone."""
    bundled = source / 'python'
    if (bundled / 'capstone').is_dir() and capstone_ready(sys.executable, bundled):
        return sys.executable, str(bundled)
    venv = root / '.venv'
    python = venv / 'bin/python3'
    if python.exists() and capstone_ready(str(python)):
        return str(python), None
    status(4, 'INSTALLING CAPSTONE')
    shutil.rmtree(venv, ignore_errors=True)
    for command in ([sys.executable, '-m', 'venv', str(venv)],
                    [str(python), '-m', 'pip', 'install', '--disable-pip-version-check', '--no-input',
                     '--timeout', '45', '--retries', '2', f'capstone=={CAPSTONE}']):
        if subprocess.run(command, timeout=600).returncode:
            raise Failure('CAPSTONE INSTALL FAILED (NETWORK?)')
    if not capstone_ready(str(python)):
        raise Failure(f'CAPSTONE {CAPSTONE} UNAVAILABLE')
    return str(python), None


def run(command, env, cwd):
    if CANCELLED.is_set():
        raise Failure('CANCELLED')
    print('+ ' + shlex.join(map(str, command)), flush=True)
    result = subprocess.run(list(map(str, command)), env=env, cwd=cwd, stdin=subprocess.DEVNULL)
    if result.returncode:
        name = Path(str(command[2] if command[1] == '-B' else command[0])).name
        raise Failure(f'{name.upper()} FAILED: EXIT {result.returncode}')


def generate(recipe, source, firmware, work, python, path, jobs, status):
    """The CMake custom commands of the macos-dev profile, in two pipelines."""
    env = dict(os.environ)
    env.pop('PYTHONHOME', None)
    if path:
        env['PYTHONPATH'] = path
    else:
        env.pop('PYTHONPATH', None)
    aot, src = source / 'aot', str(source)
    corpus = [a.replace(recipe['source_dir'], src) for a in recipe['corpus_args'] if a]
    fragments = ['fm', 'braids', 'mixed', 'coverage', 'macro-all', 'control', 'maintenance']
    translate = [python, '-B', aot / 'translate.py', '--firmware', firmware,
                 '--functions', aot / 'functions.txt', '--hooks', aot / 'enabled-maintenance.txt',
                 '--entry-hooks', aot / 'enabled-maintenance-entries.txt',
                 *[a for f in fragments for a in ('--function-fragment', aot / f'functions-{f}.txt')],
                 *[f for f in recipe['translate_flags'] if f],
                 '--output', work / 'aot/generated.c', '--manifest', work / 'aot/manifest.json']
    aot_steps = [('TRANSLATING DSP FUNCTIONS', translate),
                 ('GUARDING AOT CODE', [python, '-B', aot / 'native_aot_guard.py', '--input',
                                        work / 'aot/generated.c', '--output', work / 'native-aot-guard']),
                 ('SPLITTING HOT ENTRIES', [python, '-B', aot / 'codegen_v2.py', '--input',
                                            work / 'native-aot-guard', '--output', work / 'codegen-v2',
                                            '--replace', '--structural'])]
    corpus_steps = [('GENERATING NATIVE REGIONS', [
        python, '-B', aot / 'prepare-corpus-native.py', '--firmware', firmware,
        '--pc-file', aot / 'catalogs/corpus/playback.txt',
        '--handler-pc-file', aot / 'catalogs/corpus/handlers-complete.txt',
        '--cold-pc-file', aot / 'catalogs/corpus/baseline-cold.txt',
        '--layout-input', aot / 'catalogs/corpus/layout.json',
        '--output', work / 'corpus-native', *corpus])]
    (work / 'aot').mkdir(parents=True, exist_ok=True)
    done = []

    def pipeline(steps):
        for text, command in steps:
            run(command, env, work)
            done.append(text)
            status(status.percent, text)
    status.phase(8, 44, 40, 'GENERATING SOURCES')
    with ThreadPoolExecutor(2 if jobs > 1 else 1) as pool:
        for future in [pool.submit(pipeline, aot_steps), pool.submit(pipeline, corpus_steps)]:
            future.result()


def flags(recipe, sdk):
    def words(text):
        return shlex.split(text or '')
    common = words(recipe['c_flags'])
    for arch in (recipe['architectures'] or '').split(';'):
        if arch:
            common += ['-arch', arch]
    if recipe['sysroot']:
        common += ['-isysroot', sdk]
    if recipe['deployment'].rsplit('=', 1)[-1]:
        common += [recipe['deployment']]
    return common


def kept(options):
    return [o for o in options if o and not o.startswith(DROPPED)]


def translate_path(path, recipe, source, work):
    if path.startswith(recipe['binary_dir'] + '/'):
        return str(work / path[len(recipe['binary_dir']) + 1:])
    if path == recipe['source_dir'] or path.startswith(recipe['source_dir'] + '/'):
        return str(source) + path[len(recipe['source_dir']):]
    raise Failure(f'RECIPE PATH OUTSIDE BUILD: {path}')


def compile_targets(recipe, source, work, staging, cc, jobs, status):
    sdk = ''
    if recipe['sysroot']:
        sdk = subprocess.run(['/usr/bin/xcrun', '--show-sdk-path'], capture_output=True, text=True,
                             timeout=120).stdout.strip()
        if not sdk:
            raise Failure('NO MACOS SDK; REINSTALL COMMAND LINE TOOLS')
    common = flags(recipe, sdk)
    units, links = [], []
    for name, target in recipe['targets'].items():
        definitions = sorted({d for d in [*recipe['directory_definitions'], *target['definitions'],
                                          target['define_symbol']] if d})
        includes = [translate_path(i, recipe, source, work) for i in target['includes'] if i]
        objects = []
        # SOURCES also lists the custom-command .rule placeholders.
        for index, unit in enumerate(s for s in target['sources'] if s.endswith('.c')):
            extra = recipe['source_properties'].get(unit, {}) if name == 'm8native-regions' else {}
            source_file = translate_path(unit, recipe, source, work)
            obj = staging / f'{name}.{index}.o'
            local = sorted(d for d in extra.get('definitions', []) if d)
            command = [cc, *[f'-D{d}' for d in definitions], *[f'-D{d}' for d in local],
                       *[f'-I{i}' for i in includes], *common, recipe['standard'], recipe['pic'],
                       *kept(target['options']), *kept(extra.get('options', [])), '-o', str(obj), '-c', source_file]
            units.append((Path(source_file).stat().st_size, command))
            objects.append(str(obj))
        libraries = [f'-l{lib}' for lib in target['libraries'] if lib]
        output = staging / target['file']
        links.append((name, output, [cc, *common, *shlex.split(recipe['shared_flags']),
                                     *kept(target['link_options']), '-o', str(output),
                                     recipe['soname_flag'].strip(), f'@rpath/{target["file"]}', *objects,
                                     f'{recipe["rpath_flag"]}{recipe["rpath"]}', *libraries]))
    units.sort(key=lambda unit: -unit[0])
    finished = []

    def compile_unit(unit):
        size, command = unit
        if CANCELLED.is_set():
            raise Failure('CANCELLED')
        begin = time.monotonic()
        result = subprocess.run(command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True)
        print(f'+ {shlex.join(command)}\n{result.stdout}compiled {Path(command[-1]).name} '
              f'in {time.monotonic() - begin:.1f}s', flush=True)
        if result.returncode:
            raise Failure(f'COMPILE FAILED: {Path(command[-1]).name.upper()}')
        finished.append(size)
        status(status.percent, f'COMPILING NATIVE CODE {len(finished)}/{len(units)}')
    status.phase(45, 90, 150, 'COMPILING NATIVE CODE')
    with ThreadPoolExecutor(jobs) as pool:
        list(pool.map(compile_unit, units))
    for name, output, command in links:
        status.phase(91, 94, 5, 'LINKING ' + output.name.upper())
        print('+ ' + shlex.join(command), flush=True)
        if subprocess.run(command, stdin=subprocess.DEVNULL).returncode:
            raise Failure('LINK FAILED: ' + output.name.upper())
        if subprocess.run(['/usr/bin/codesign', '--force', '--sign', '-', str(output)],
                          stdin=subprocess.DEVNULL).returncode:
            raise Failure('AD HOC SIGNING FAILED')
    return {name: output for name, output, _ in links}


def verify(outputs):
    for name, path in outputs.items():
        try:
            library = ctypes.CDLL(str(path), mode=os.RTLD_LOCAL)
            missing = [s for s in SYMBOLS[name] if not hasattr(library, s)]
        except OSError as error:
            raise Failure(f'{path.name.upper()} DOES NOT LOAD') from error
        if missing:
            raise Failure(f'{path.name.upper()} LACKS {missing[0].upper()}')


def install(outputs, output, stamp):
    """engine.json is the commit marker: removed first, written last."""
    marker = output / 'engine.json'
    if marker.exists():
        marker.unlink()
    for path in outputs.values():
        os.replace(path, output / path.name)
    stamp['files'] = {path.name: {'size': (output / path.name).stat().st_size, 'sha256': sha(output / path.name)}
                      for path in outputs.values()}
    write_atomic(marker, json.dumps(stamp, indent=2) + '\n')


def build(args, status):
    source = args.source.resolve()
    recipe = json.loads((source / 'recipe.json').read_text())
    engine_id = (source / 'ENGINE_ID').read_text().strip()
    if recipe.get('schema') != 'm8-engine-recipe/1':
        raise Failure('UNSUPPORTED RECIPE')
    output = args.output.resolve()
    staging = output / '.staging'
    shutil.rmtree(staging, ignore_errors=True)
    staging.mkdir()
    try:
        stage(args, status, source, recipe, engine_id, output, staging)
    finally:
        shutil.rmtree(staging, ignore_errors=True)


def stage(args, status, source, recipe, engine_id, output, staging):
    status(1, 'CHECKING FIRMWARE')
    firmware = staging / 'firmware.bin'
    shutil.copyfile(args.firmware, firmware)
    digest = sha(firmware)
    if digest != recipe['firmware_sha256']:
        raise Failure('FIRMWARE IS NOT HEADLESS 6.5.2C')
    if output.name != digest:
        raise Failure('OUTPUT DIRECTORY DOES NOT NAME THE FIRMWARE')
    status(2, 'CHECKING COMPILER')
    probe = subprocess.run([args.cc, '--version'], capture_output=True, text=True, timeout=300)
    if probe.returncode:
        raise Failure('CLANG UNAVAILABLE: ' + (probe.stderr.strip().splitlines() or ['?'])[-1][:60].upper())
    python, path = interpreter(source, output.parent, status)
    start = time.monotonic()
    work = staging / 'build'
    generate(recipe, source, firmware, work, python, path, args.jobs, status)
    outputs = compile_targets(recipe, source, work, staging, args.cc, args.jobs, status)
    status.ramp = None
    status(95, 'VERIFYING LIBRARIES')
    verify(outputs)
    status(98, 'INSTALLING')
    install(outputs, output, {'schema': SCHEMA, 'engine_id': engine_id, 'firmware_sha256': digest,
                              'version': recipe['version'], 'compiler': probe.stdout.splitlines()[0],
                              'python': platform.python_version(), 'seconds': round(time.monotonic() - start, 1),
                              'created': time.strftime('%Y-%m-%dT%H:%M:%S%z')})


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source', type=Path, default=HERE, help='Staged engine-src directory')
    p.add_argument('--firmware', type=Path, required=True, help='Headless 6.5.2C BIN; only read')
    p.add_argument('--output', type=Path, required=True, help='<data>/m8-live/engine/<bin sha256>')
    p.add_argument('--cc', default='/usr/bin/clang')
    p.add_argument('--jobs', type=int, default=os.cpu_count() or 1)
    args = p.parse_args()
    if sys.version_info < (3, 9):
        p.exit(2, 'Python 3.9 or newer is required\n')
    args.output.mkdir(parents=True, exist_ok=True)
    status = Status(args.output / 'build.status')

    def cancel(number, frame):
        CANCELLED.set()
        raise Failure('CANCELLED')
    # Cancel signals the whole process group; compilers stop with it.
    signal.signal(signal.SIGTERM, cancel)
    with open(args.output / '.lock', 'w') as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError:
            status(0, 'ANOTHER BUILD IS RUNNING', 'failed')
            sys.exit(1)
        start = time.monotonic()
        try:
            build(args, status)
        except Failure as error:
            status(status.percent, str(error), 'failed')
            sys.exit(1)
        except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
            print(f'error: {error!r}', flush=True)
            status(status.percent, f'BUILD ERROR: {type(error).__name__.upper()}', 'failed')
            sys.exit(1)
        status(100, f'DONE IN {round(time.monotonic() - start)} S', 'done')


if __name__ == '__main__':
    main()
