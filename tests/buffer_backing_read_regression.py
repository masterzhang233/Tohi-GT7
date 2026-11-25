"""Generate a CPU-only test of the actual backing-read and SRT cache decisions.

The range sets, BufferCache queries, kernel readers, page cache and indirect
dispatch/sampler fast-path predicates are extracted unchanged from production.
GPU progress/publication and the guest backing allocation are controlled adapters.
"""
from pathlib import Path
import argparse
import hashlib
import json
import re


def block(text, marker):
    start = text.index(marker)
    opening = text.index('{', start)
    tokens = re.compile(r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]')
    depth = 0
    for token in tokens.finditer(text, opening):
        if token.group() == '{':
            depth += 1
        elif token.group() == '}':
            depth -= 1
            if depth == 0:
                return text[start:token.end()]
    raise ValueError(f'unterminated C++ block: {marker}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    root = args.source / 'src'
    inputs = {
        'ranges': root / 'graphics/host_gpu/rangeSet.h',
        'buffers': root / 'graphics/host_gpu/renderer/cache/bufferCache.cpp',
        'memory': root / 'kernel/memory.cpp',
        'pipeline': root / 'graphics/host_gpu/renderer/pipeline/pipelineCache.cpp',
        'compute': root / 'graphics/host_gpu/renderer/renderCompute.cpp',
        'descriptors': root / 'graphics/host_gpu/renderer/pipeline/descriptors.cpp',
    }
    sources = {name: path.read_text(encoding='utf-8-sig') for name, path in inputs.items()}
    queries = '\n\n'.join(block(sources['buffers'], marker) for marker in [
        'bool BufferCache::HasGpuDirtyBytes(', 'bool BufferCache::HasGpuDirtyBytesShared(',
        'bool BufferCache::IsCleanForConcurrentRead('])
    # The pre-optimization snapshot has no dedicated publication method. Keeping
    # that source usable lets the same regression demonstrate the old fallback.
    publication_marker = 'bool BufferCache::FinishPendingBacking('
    if publication_marker in sources['buffers']:
        queries += '\n\n' + block(sources['buffers'], publication_marker)
    readers = '\n\n'.join(block(sources['memory'], marker) for marker in [
        'bool TryReadBufferBacking(', 'bool IsGpuBufferWritten(',
        'static bool MaybeGpuDirtyForClean(', 'const uint8_t* FindGpuCleanBacking(',
        'bool TryReadGpuCleanBacking('])
    compute = sources['compute'].split('if (vk::DispatchIndirectCommand skip_args {};', 1)[1]
    compute = compute.split(') {', 1)[0].strip()
    samplers = block(sources['descriptors'], 'void RenderExecutor::PrepareBindlessSamplers(')
    samplers = samplers.split('if (!records.empty() &&', 1)[1].split(') {', 1)[0]
    samplers = '!records.empty() &&' + samplers
    text = Path(__file__).with_name('BufferBackingReadRegression.cpp.in').read_text(encoding='utf-8')
    for key, value in {
        'RANGES': block(sources['ranges'], 'class RangeSet final {') + ';',
        'QUERIES': queries, 'READERS': readers,
        'PAGE_CACHE': block(sources['pipeline'], 'struct GuestPageReadCache {') + ';',
        'EMPTY_DISPATCH': compute, 'SAMPLER_MIRROR': samplers,
    }.items():
        text = text.replace('@' + key + '@', value)
    if re.search(r'@[A-Z_]+@', text):
        raise ValueError('unexpanded template marker')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(text, encoding='utf-8')
    args.output.with_suffix('.sources.json').write_text(json.dumps({
        'sources': {str(path): hashlib.sha256(path.read_bytes()).hexdigest()
                    for path in inputs.values()},
        'scope': __doc__,
    }, indent=2), encoding='utf-8')


if __name__ == '__main__':
    main()
