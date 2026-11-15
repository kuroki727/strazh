#!/usr/bin/env python3
"""Exercise publication guards with fake gh/git; never access a remote or tag."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / 'tools/publish-v0.1.0.sh'
SHA = 'a' * 40
TREE = 'b' * 40

STUB = '''#!/usr/bin/env python3
import json, os, sys
from pathlib import Path
args=sys.argv[1:]
name=Path(sys.argv[0]).name
case=os.environ['CASE']
with open(os.environ['CALLS'], 'a') as f: f.write(json.dumps([name,args])+'\\n')
sha='a'*40
tree='b'*40
if name=='gh':
    if args[0]=='api':
        path=args[1]
        if case=='api_error': sys.exit(1)
        if path.endswith('/pulls/1'):
            print('true\\t%s\\t%s\\t%s\\t%s'%(
                'c'*40 if case=='other_merge' else sha, 'd'*40,
                'other' if case=='wrong_base' else 'main',
                'other' if case=='wrong_head' else 'release/v0.1.0'))
        elif '/git/ref/heads/main' in path: print('e'*40 if case=='moved_main' else sha)
        elif '/git/commits/' in path: print('f'*40 if case=='wrong_tree' else tree)
        elif '/git/matching-refs/' in path: print(1 if case=='existing_tag' else 0)
        elif path.endswith('/releases'): print('42' if case=='existing_release' else '')
        else: sys.exit(99)
    elif args[:2]==['release','create']: print('https://example.invalid/release')
    elif args[:2]==['release','view']: print('https://example.invalid/release')
    else: sys.exit(99)
elif name=='git':
    if args[:2]==['rev-parse','HEAD']: print(sha)
    elif args[:2]==['rev-parse','HEAD^{tree}']: print(tree)
    elif args[:2]==['rev-parse','v0.1.0^{}']: print(sha)
    elif args[:2]==['rev-parse','v0.1.0^{tag}']: print('1'*40)
    elif args[0]=='status': print(' M unsafe.cpp' if case=='dirty' else '')
    elif args[0] in ('config','tag'): pass
    elif 'push' in args:
        if case=='push_error': sys.exit(1)
    else: sys.exit(99)
else: sys.exit(99)
'''


class ReleaseGate(unittest.TestCase):
    def invoke(self, case):
        with tempfile.TemporaryDirectory(prefix='strazh-release-gate-') as directory:
            root = Path(directory)
            for tool in ('gh', 'git'):
                path = root / tool
                path.write_text(STUB)
                path.chmod(0o700)
            (root / 'CHANGELOG.md').write_text('## [0.1.0] - 2026-11-15\nExperimental release\n')
            if case == 'missing_notes':
                (root / 'CHANGELOG.md').unlink()
            calls = root / 'calls'
            env = dict(os.environ, CASE=case, CALLS=str(calls),
                       PATH=str(root) + os.pathsep + os.environ['PATH'], RUNNER_TEMP=str(root),
                       GITHUB_REPOSITORY='kuroki727/strazh', GITHUB_REF='refs/heads/main',
                       GITHUB_EVENT_NAME='push', GITHUB_SHA=SHA)
            if case == 'wrong_repository':
                env['GITHUB_REPOSITORY'] = 'someone/else'
            if case == 'wrong_ref':
                env['GITHUB_REF'] = 'refs/heads/release/v0.1.0'
            if case == 'wrong_event':
                env['GITHUB_EVENT_NAME'] = 'pull_request'
            result = subprocess.run(['bash', str(SCRIPT)], cwd=root, env=env,
                                    capture_output=True, text=True, timeout=5)
            recorded = [json.loads(line) for line in calls.read_text().splitlines()] if calls.exists() else []
            return result, recorded

    @staticmethod
    def mutations(calls):
        return [(tool, args) for tool, args in calls
                if (tool == 'git' and (args[0] == 'tag' or 'push' in args)) or
                (tool == 'gh' and args[:2] == ['release', 'create'])]

    def test_rejects_unsafe_preconditions_without_publication(self):
        for case in ('wrong_repository', 'wrong_ref', 'wrong_event', 'dirty', 'moved_main',
                     'wrong_tree', 'wrong_base', 'wrong_head', 'existing_tag',
                     'existing_release', 'missing_notes', 'api_error'):
            with self.subTest(case=case):
                result, calls = self.invoke(case)
                self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(self.mutations(calls), [])

    def test_other_main_commit_does_not_publish(self):
        result, calls = self.invoke('other_merge')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.mutations(calls), [])
        self.assertIn('not the merge of PR #1', result.stdout)

    def test_verified_merge_uses_annotated_tag_and_changelog_only(self):
        result, calls = self.invoke('valid')
        self.assertEqual(result.returncode, 0, result.stderr)
        mutations = self.mutations(calls)
        self.assertEqual(len(mutations), 3)
        self.assertEqual(mutations[0][1], ['tag', '--annotate', 'v0.1.0', '--message',
                                         'Strazh v0.1.0 — experimental Linux init', SHA])
        self.assertIn('refs/tags/v0.1.0', mutations[1][1])
        release = mutations[2][1]
        self.assertIn('--verify-tag', release)
        self.assertEqual(release[release.index('--notes-file') + 1], 'CHANGELOG.md')
        self.assertFalse(any(arg in ('--force', '-f') for _, args in mutations for arg in args))

    def test_failed_tag_push_prevents_release_creation(self):
        result, calls = self.invoke('push_error')
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(any(tool == 'gh' and args[:2] == ['release', 'create']
                             for tool, args in calls))


if __name__ == '__main__':
    unittest.main(verbosity=2)
