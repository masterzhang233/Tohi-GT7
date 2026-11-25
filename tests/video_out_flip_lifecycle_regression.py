"""CPU-only regression of extracted, unmodified VideoOut queue/lifecycle methods.

Only host presentation, event delivery, clocks and platform synchronization are substituted.
The tested Reserve/Cancel/Complete/Flip/Close/SetFlipMaster implementations and queue layout
come from videoOut.cpp at generation time. No Vulkan, game or graphics device is started.
"""
from pathlib import Path
import argparse
import hashlib
import json
import re
import subprocess


def block(text, marker):
    """Read a balanced C++ block, ignoring braces in comments and string literals."""
    start = text.index(marker)
    opening = text.index('{', start)
    tokens = re.compile(r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]')
    depth = 0
    for token in tokens.finditer(text, opening):
        value = token.group()
        if value == '{':
            depth += 1
        elif value == '}':
            depth -= 1
            if depth == 0:
                return text[start:token.end()]
    raise ValueError(f'unterminated C++ block: {marker}')


def replace_once(text, before, after):
    if text.count(before) != 1:
        raise ValueError('negative-control anchor changed; update the regression explicitly')
    return text.replace(before, after, 1)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--compiler', type=Path)
    parser.add_argument('--vcvars', type=Path)
    parser.add_argument('--generate-only', action='store_true')
    args = parser.parse_args()
    source = args.source.resolve() / 'src/graphics/presentation/videoOut.cpp'
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    production = source.read_text(encoding='utf-8-sig')
    template = Path(__file__).with_name('VideoOutFlipLifecycleRegression.cpp.in')
    text = template.read_text(encoding='utf-8')
    declarations = '\n'.join(block(production, marker) + ';' for marker in [
        'struct VideoOutFlipStatus {', 'struct VideoOutVblankStatus {',
        'struct VideoOutBuffer {', 'struct VideoOutConfig {', 'struct FlipDiagnostics {'])
    queue = block(production, 'class FlipQueue {') + ';'
    # Test setup may promote a reserved request to Ready without invoking GPU preparation.
    # All transitions exercised by the assertions below remain the production methods.
    queue = queue.replace('\nprivate:', '\npublic:')
    methods = '\n\n'.join(block(production, marker) for marker in [
        'static bool IsFlipDueLocked(', 'bool FlipQueue::Reserve(',
        'FlipQueue::~FlipQueue()', 'void FlipQueue::Cancel(', 'void FlipQueue::Complete(',
        'void FlipQueue::BeginAsyncTick(', 'void FlipQueue::EndAsyncTick(',
        'uint64_t FlipQueue::AsyncRevision(', 'void FlipQueue::WaitAsyncEvent(',
        'void FlipQueue::WakeAsync(', 'bool FlipQueue::TakeAsyncRefresh(',
        'bool FlipQueue::PublishCompleted(', 'bool FlipQueue::Flip(',
        'int VideoOutDriver::Impl::Open(', 'bool VideoOutDriver::Impl::Close(',
        'int VideoOutDriver::Impl::SetFlipMaster(',
        'void VideoOutDriver::Impl::VblankBegin(', 'void VideoOutDriver::Impl::VblankEnd(',
        'bool VideoOutDriver::Impl::AsyncClockTick(', 'void VideoOutDriver::Impl::AsyncHostThread('])
    text = text.replace('@DECLARATIONS@', declarations).replace('@QUEUE@', queue)
    variants = {'fixed': methods}
    flip = block(methods, 'bool FlipQueue::Flip(')
    unlock = ('\t\tfor (size_t i = count; i != 0; i--) {\n'
              '\t\t\trequests[i - 1].cfg->mutex.Unlock();\n\t\t}\n')
    relock_start = flip.index('\t\tfor (size_t i = 0; i < count; i++) {', flip.index('m_presenter.Present'))
    relock = block(flip[relock_start:], '\t\tfor (size_t i = 0; i < count; i++) {') + '\n'
    locked = replace_once(flip, unlock, '')
    locked = replace_once(locked, relock, '')
    variants['negative_ports_held'] = replace_once(methods, flip, locked)
    stale = replace_once(
        flip, 'if (r.cfg->opened && !r.cfg->closing && r.cfg->generation == r.generation) {',
        'if (true) {')
    variants['negative_stale_completion'] = replace_once(methods, flip, stale)
    duplicate = replace_once(flip, 'r.cfg->async_admitted_epoch != admission_epoch;', 'true;')
    variants['negative_async_double_admission'] = replace_once(methods, flip, duplicate)
    complete = block(methods, 'void FlipQueue::Complete(')
    lost_ready = replace_once(complete, 'if (m_async_enabled) ++m_async_revision;', '')
    variants['negative_async_ready_revision'] = replace_once(methods, complete, lost_ready)
    generated = {}
    for name, body in variants.items():
        path = output / f'{name}.cpp'
        path.write_text(text.replace('@METHODS@', body), encoding='utf-8')
        generated[name] = path
    report = {
        'production_sha256': hashlib.sha256(source.read_bytes()).hexdigest(),
        'test_sha256': {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
                        for p in (Path(__file__), template)},
        'scope': 'Actual queue class and Reserve/Cancel/Complete/Flip/Open/Close/SetFlipMaster, '
                 'async queue methods, VblankBegin/End, AsyncClockTick and AsyncHostThread; '
                 'test-controlled Ready admission, Presenter/frame ownership boundary, event sink, '
                 'clocks and std mutex/condition-variable adapter. No GPU preparation, real '
                 'Presenter/frame-pool implementation, Vulkan or platform mutex implementation.',
        'results': [],
    }
    if args.generate_only:
        report['generated_only'] = True
    else:
        if args.compiler is None or args.vcvars is None:
            parser.error('--compiler and --vcvars are required unless --generate-only')
        for name, cpp in generated.items():
            expected = 0 if name == 'fixed' else 1
            expected_failure = {'negative_ports_held': 'submit_during_present',
                                'negative_stale_completion': 'close_during_present',
                                'negative_async_double_admission': 'async_epoch_and_flip_rate',
                                'negative_async_ready_revision': 'async_ready_wake_and_stop'}.get(name)
            exe = output / f'{name}.exe'
            obj = output / f'{name}.obj'
            cmd = output / f'{name}.cmd'
            cmd.write_text('@echo off\n'
                           f'call "{args.vcvars.resolve()}" >nul\n'
                           f'"{args.compiler.resolve()}" /nologo /std:c++20 /EHsc /Od /W4 '
                           f'/Fe:"{exe}" /Fo:"{obj}" "{cpp}"\nexit /b %errorlevel%\n')
            compiled = subprocess.run(['cmd', '/d', '/c', str(cmd)], capture_output=True,
                                      text=True, timeout=120)
            (output / f'{name}-compile.log').write_text(compiled.stdout + compiled.stderr)
            actual = None
            run_text = ''
            if compiled.returncode == 0:
                try:
                    # Mutants deliberately violate one contract. Exercise only that contract:
                    # unrelated concurrent cases may intentionally deadlock under a different bug.
                    command = [str(exe)]
                    if expected_failure is not None:
                        command += ['--case', expected_failure]
                    ran = subprocess.run(command, capture_output=True, text=True, timeout=30)
                    actual, run_text = ran.returncode, ran.stdout + ran.stderr
                except subprocess.TimeoutExpired as error:
                    run_text = f'TIMEOUT: {error}\n'
                (output / f'{name}-run.log').write_text(run_text)
            passed = compiled.returncode == 0 and actual == expected
            if expected_failure is not None:
                passed &= f'FAIL {expected_failure}:' in run_text
            report['results'].append({'variant': name, 'compile_exit': compiled.returncode,
                                      'actual_exit': actual, 'expected_exit': expected,
                                      'expected_failure': expected_failure, 'passed': passed})
            print(name, report['results'][-1], flush=True)
        report['passed'] = all(result['passed'] for result in report['results'])
    (output / 'results.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    if not args.generate_only and not report['passed']:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
