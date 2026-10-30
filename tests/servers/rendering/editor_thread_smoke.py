#!/usr/bin/env python3
"""Opt-in Windows/Vulkan editor render-thread smoke runner (no added dependencies).

Runtime acceptance tool; this source-only implementation pass does not invoke it.
The editor must not be run headlessly: swapchain/window ownership is being tested.
"""

import argparse
import json
import pathlib
import subprocess
import sys
import time


def run(editor, project, mode, timeout_seconds, output_dir):
    command = [str(editor), '--path', str(project), '--editor',
               '--rendering-driver', 'vulkan', '--render-thread', mode,
               '--verbose', '--quit-after', '120']
    if mode == 'separate':
        command.insert(-3, '--editor-render-thread-experimental')
    started = time.monotonic()
    result = {'mode': mode, 'command': command, 'timeout_seconds': timeout_seconds}
    try:
        completed = subprocess.run(command, capture_output=True, text=True,
                                   errors='replace', timeout=timeout_seconds, check=False)
        result.update({'status': 'EXITED' if completed.returncode == 0 else 'FAILED',
                       'exit_code': completed.returncode})
        stdout, stderr = completed.stdout, completed.stderr
    except subprocess.TimeoutExpired as exc:
        result.update({'status': 'TIMED_OUT', 'exit_code': None})
        stdout = (exc.stdout or b'').decode(errors='replace') if isinstance(exc.stdout, bytes) else exc.stdout or ''
        stderr = (exc.stderr or b'').decode(errors='replace') if isinstance(exc.stderr, bytes) else exc.stderr or ''
    result['duration_seconds'] = round(time.monotonic() - started, 3)
    (output_dir / (mode + '.stdout.log')).write_text(stdout, encoding='utf-8')
    (output_dir / (mode + '.stderr.log')).write_text(stderr, encoding='utf-8')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--editor', type=pathlib.Path, required=True)
    parser.add_argument('--project', type=pathlib.Path, required=True)
    parser.add_argument('--output', type=pathlib.Path, required=True)
    parser.add_argument('--timeout', type=int, default=120)
    args = parser.parse_args()
    if not args.editor.is_file() or not (args.project / 'project.godot').is_file():
        parser.error('A built Windows editor and a valid GUI test project are required.')
    args.output.mkdir(parents=True, exist_ok=True)
    results = [run(args.editor, args.project, mode, args.timeout, args.output)
               for mode in ('safe', 'separate')]
    (args.output / 'results.json').write_text(json.dumps(results, indent=2) + '\n', encoding='utf-8')
    return 0 if all(r['status'] == 'EXITED' for r in results) else 1


if __name__ == '__main__':
    sys.exit(main())
