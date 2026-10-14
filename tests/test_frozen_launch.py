# SPDX-License-Identifier: MIT
"""Private child authorization and Win32 supervision; no USB operations."""
import ctypes
from contextlib import ExitStack
import hashlib
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import types
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from diskos_installer import basegate, bundle, flash_child, flasher, gui_entry, platform_probe
from diskos_installer import __main__ as cli
from test_windows_host import API, Process, pe

NONCE = 'a' * 64


def capability(cmd=None):
    return dict(nonce=NONCE, parent=123, dev_requested=True,
                cmd=cmd or [os.path.abspath('usbboot.exe'), '--help'], target='257')


class ChildContractTests(unittest.TestCase):
    def invoke(self, request=None, gate=None, argv=None, supervision=None):
        raw = b'G' + json.dumps(request or capability()).encode('ascii') if gate is None else gate
        with mock.patch.object(sys, 'platform', 'win32'), mock.patch.object(
                sys, 'stdin', mock.Mock(buffer=io.BytesIO(raw))), mock.patch.object(
                sys, 'stdout', io.StringIO()), mock.patch.object(sys, 'stderr', io.StringIO()), \
                mock.patch.object(flash_child, 'verify_supervision', side_effect=supervision) as check, \
                mock.patch.object(flash_child, 'authorized_command', return_value=(['bundled'], {})) as auth, \
                mock.patch.object(subprocess, 'call', return_value=17) as tool:
            rc = flash_child.main(argv if argv is not None else [flash_child.CHILD_MODE, NONCE])
            return rc, check.call_count, auth.call_count, tool.call_count

    def test_malformed_tool_arguments_rejected(self):
        exe = os.path.abspath('usbboot.exe')
        for cmd in (None, {}, 'command', [], [exe], [exe, 1], [exe, ''],
                    ['usbboot.exe', '--help'], [exe, '-c', 'code'],
                    [exe, '--cpu', 'other'], [exe, '--wait'], [exe, '--wait', '-1'],
                    [exe, '--wait', 'NaN'], [exe, '--upload', 'relative'],
                    [exe, '--help\x00'], [exe, '--help\n']):
            with self.subTest(cmd=cmd), self.assertRaises(ValueError): flash_child.validate_command(cmd)

    def test_legacy_named_job_and_caller_command_line_rejected(self):
        for argv in ([], [flash_child.CHILD_MODE], [flash_child.CHILD_MODE, 'bad'],
                     [flash_child.CHILD_MODE, 'Local\\diskos-flash-' + 'b' * 32, '[]'],
                     [flash_child.CHILD_MODE, NONCE, json.dumps(capability()['cmd'])]):
            self.assertEqual(self.invoke(argv=argv), (2, 0, 0, 0))

    def test_eof_bad_gate_and_missing_or_forged_capability(self):
        for raw in (b'', b'X', b'G', b'G{}', b'G' + b'x' * 65537):
            self.assertEqual(self.invoke(gate=raw), (1, 0, 0, 0))
        for field, value in (('nonce', 'b' * 64), ('parent', True), ('parent', 0),
                             ('dev_requested', 1), ('dev_requested', 'true'), ('dev_requested', None)):
            request = capability(); request[field] = value
            self.assertEqual(self.invoke(request), (1, 0, 0, 0))
        self.assertEqual(self.invoke(supervision=OSError('forged job')), (1, 1, 0, 0))
        self.assertEqual(self.invoke(), (17, 1, 1, 1))

    def test_public_false_development_boolean_reaches_authorization(self):
        request = capability(); request['dev_requested'] = False
        self.assertEqual(self.invoke(request), (17, 1, 1, 1))

    def test_private_dispatch_uses_all_child_checks_without_gui_or_lock(self):
        with mock.patch.object(flash_child, 'main', return_value=2) as child, \
                mock.patch.object(cli, 'build_parser') as parser, mock.patch.object(cli, '_Lock') as lock:
            self.assertEqual(cli.main([flash_child.CHILD_MODE, 'bad']), 2)
            child.assert_called_once_with([flash_child.CHILD_MODE, 'bad'])
            parser.assert_not_called(); lock.assert_not_called()

    def test_source_and_frozen_launch_carry_no_job_name_or_tool_argv(self):
        for frozen in (False, True):
            with mock.patch.object(sys, 'frozen', frozen, create=True), \
                    mock.patch.object(sys, 'executable', os.path.abspath('installer space.exe')):
                argv = flash_child.launch_argv(NONCE)
                prefix = [sys.executable] if frozen else [sys.executable, '-I', os.path.abspath(flash_child.__file__)]
                self.assertEqual(argv, prefix + [flash_child.CHILD_MODE, NONCE])

    def test_linux_and_macos_launch_unchanged(self):
        for host in ('linux', 'macos'):
            with mock.patch.object(platform_probe, 'host', return_value=(host, 'x86_64')), \
                    mock.patch.object(flasher.subprocess, 'Popen') as spawn:
                flasher._start_child(['original', '--arg'], stdout=123)
                spawn.assert_called_once_with(['original', '--arg'], start_new_session=True, stdout=123)

    def test_both_frozen_executables_spawn_their_own_image(self):
        for name in ('diskos-installer.exe', 'diskos-installer-cli.exe'):
            image = os.path.abspath('package space/' + name)
            with mock.patch.object(sys, 'frozen', True, create=True), \
                    mock.patch.object(sys, 'executable', image):
                self.assertEqual(flash_child.launch_argv(NONCE), [image, flash_child.CHILD_MODE, NONCE])

    def test_windowed_dispatch_preserves_binary_gate_and_supervision(self):
        self.assertEqual(gui_entry.CHILD_MODE, flash_child.CHILD_MODE)
        for gate, supervision, expected in ((b'', None, 1), (b'G{}', None, 1),
                (b'G' + json.dumps(capability()).encode('ascii'), OSError('wrong parent image'), 1),
                (b'G' + json.dumps(capability()).encode('ascii'), None, 17)):
            streams = [io.TextIOWrapper(io.BytesIO(gate)), io.StringIO(), io.StringIO()]
            with mock.patch.object(sys, 'platform', 'win32'), \
                    mock.patch.object(gui_entry, '_child_stream', side_effect=streams), \
                    mock.patch.object(gui_entry, '_open_log') as log, \
                    mock.patch.object(gui_entry, '_show_error') as dialog, \
                    mock.patch.object(cli, '_Lock') as lock, \
                    mock.patch.object(flash_child, 'verify_supervision', side_effect=supervision) as check, \
                    mock.patch.object(flash_child, 'authorized_command', return_value=(['bundled'], {})) as auth, \
                    mock.patch.object(subprocess, 'call', return_value=17) as tool:
                self.assertEqual(gui_entry.main([flash_child.CHILD_MODE, NONCE]), expected)
                self.assertEqual(tool.call_count, int(expected == 17))
                self.assertEqual(auth.call_count, int(expected == 17))
                self.assertEqual(check.call_count, int(gate not in (b'', b'G{}')))
                log.assert_not_called(); dialog.assert_not_called(); lock.assert_not_called()

    def test_job_limits_membership_and_parent_pipe_are_required(self):
        for member, flags, query, pipe_pid, alive, image in (
                (True, 0x2000, True, 123, 258, sys.executable),
                (False, 0x2000, True, 123, 258, sys.executable),
                (True, 0, True, 123, 258, sys.executable),
                (True, 0x2800, True, 123, 258, sys.executable),
                (True, 0x3000, True, 123, 258, sys.executable),
                (True, 0x2000, False, 123, 258, sys.executable),
                (True, 0x2000, True, 124, 258, sys.executable),
                (True, 0x2000, True, 123, 0, sys.executable),
                (True, 0x2000, True, 123, 258, os.path.abspath('other.exe'))):
            api = mock.Mock()
            def membership(process, job, ptr):
                self.assertIsNone(job)
                ptr._obj.value = member
                return True
            def limits(job, kind, ptr, size, length):
                self.assertIsNone(job)
                ptr._obj.basic.flags = flags
                return query
            def pipe(handle, ptr): ptr._obj.value = pipe_pid; return True
            api.GetCurrentProcess.return_value = -1
            def parent_image(process, flags, output, length):
                output.value = sys.executable if process == -1 else image
                return True
            api.IsProcessInJob.side_effect = membership
            api.QueryInformationJobObject.side_effect = limits
            api.GetNamedPipeServerProcessId.side_effect = pipe
            api.QueryFullProcessImageNameW.side_effect = parent_image
            api.OpenProcess.return_value = 11
            api.WaitForSingleObject.return_value = alive
            with self.subTest(flags=flags, member=member, query=query, pipe_pid=pipe_pid, alive=alive, image=image):
                if member and flags == 0x2000 and query and pipe_pid == 123 and alive == 258 and image == sys.executable:
                    flash_child.verify_supervision(123, api)
                    api.CloseHandle.assert_called_once_with(11)
                else:
                    with self.assertRaises(OSError): flash_child.verify_supervision(123, api)
                api.OpenJobObjectW.assert_not_called()
                api.AssignProcessToJobObject.assert_not_called()

    def test_unnamed_noninherited_suspended_assign_resume_gate_and_failure(self):
        for fail in ('none', 'inherit', 'limit', 'spawn', 'assign', 'resume', 'gate', 'pipe-close', 'kill'):
            api = API(assign=fail not in ('assign', 'kill'))
            process = Process(api.events)
            def spawn(argv, **kw):
                self.assertEqual(kw['creationflags'], 0x204)
                api.events.append('suspended')
                if fail == 'spawn': raise OSError('spawn')
                return process
            with mock.patch.object(flasher, '_WINDOWS_DEV_FLASH', mock.Mock(get=mock.Mock(return_value=True))), \
                    mock.patch.dict(os.environ, DISKOS_WINDOWS_DEV_FLASH='1'), \
                    mock.patch.object(platform_probe, 'windows_dev_runtime', return_value=True), ExitStack() as stack:
                if fail == 'inherit': stack.enter_context(mock.patch.object(api, 'SetHandleInformation', return_value=0))
                if fail == 'limit': stack.enter_context(mock.patch.object(api, 'SetInformationJobObject', return_value=0))
                if fail == 'resume': stack.enter_context(mock.patch.object(api, 'ResumeThread', return_value=0xffffffff))
                if fail == 'gate': stack.enter_context(mock.patch.object(process, 'write', side_effect=BrokenPipeError()))
                if fail == 'pipe-close': stack.enter_context(mock.patch.object(process, 'close', side_effect=BrokenPipeError()))
                if fail == 'kill': stack.enter_context(mock.patch.object(process, 'kill', side_effect=OSError('kill failed')))
                if fail == 'none':
                    child = flasher._WindowsChild(capability()['cmd'], target_ver='257', api=api, popen=spawn)
                    order = [api.events.index(item) for item in ('suspended', 'assign', 'resume', 'gate')]
                    self.assertEqual(order, sorted(order))
                    self.assertIn(('inherit', 11, 1, 0), api.events)
                    flasher._kill_child(child); child.close(); child.close()
                    self.assertIn('terminate', api.events)
                else:
                    with self.assertRaises(OSError):
                        flasher._WindowsChild(capability()['cmd'], api=api, popen=spawn)
                    if fail not in ('inherit', 'limit', 'spawn', 'kill'):
                        self.assertIn('kill', api.events)
                self.assertIn(('job', None, None), api.events)
                self.assertEqual(api.events.count(('close', 11)), 1)

    def test_parent_launch_requires_existing_development_opt_ins(self):
        for flag, env, runtime in ((False, '1', True), (True, '0', True), (True, '1', False)):
            with mock.patch.object(platform_probe, 'host', return_value=('windows', 'x86_64')), \
                    mock.patch.object(flasher, '_WINDOWS_DEV_FLASH', mock.Mock(get=mock.Mock(return_value=flag))), \
                    mock.patch.dict(os.environ, DISKOS_WINDOWS_DEV_FLASH=env), \
                    mock.patch.object(platform_probe, 'windows_dev_runtime', return_value=runtime), \
                    mock.patch.object(flash_child, 'windows_api') as api:
                with self.assertRaises(flasher.FlashError): flasher._WindowsChild(capability()['cmd'])
                api.assert_not_called()

    def test_public_parent_sends_actual_false_development_request(self):
        api = API(); process = Process(api.events)
        with mock.patch.object(platform_probe, 'host', return_value=('windows', 'x86_64')), \
                mock.patch.object(platform_probe, 'windows_native_runtime', return_value=True), \
                mock.patch.object(sys, 'frozen', True, create=True), \
                mock.patch.object(flasher._build_capabilities, 'WINDOWS_FLASH', True), \
                mock.patch.dict(os.environ, DISKOS_WINDOWS_DEV_FLASH=''), \
                mock.patch.object(process, 'write', wraps=process.write) as gate:
            child = flasher._WindowsChild(capability()['cmd'], api=api, popen=lambda *a, **kw: process)
            self.assertIs(json.loads(gate.call_args.args[0][1:])['dev_requested'], False)
            child.close()
            with mock.patch.object(flasher._build_capabilities, 'WINDOWS_FLASH', False):
                with self.assertRaises(flasher.FlashError):
                    flasher._WindowsChild(capability()['cmd'], api=api, popen=lambda *a, **kw: process)

    def test_parent_death_closes_only_job_handle_and_kills_descendants_model(self):
        api = API()
        process = Process(api.events)
        tree = {'launcher': True, 'python': True, 'usbboot': True}
        def close(handle):
            api.events.append(('close', handle))
            if handle == 11:
                for pid in tree: tree[pid] = False
            return 1
        api.CloseHandle = close
        with mock.patch.object(flasher, '_WINDOWS_DEV_FLASH', mock.Mock(get=mock.Mock(return_value=True))), \
                mock.patch.dict(os.environ, DISKOS_WINDOWS_DEV_FLASH='1'), \
                mock.patch.object(platform_probe, 'windows_dev_runtime', return_value=True):
            child = flasher._WindowsChild(capability()['cmd'], api=api, popen=lambda *a, **kw: process)
        self.assertTrue(all(tree.values()))
        # Model the OS closing the sole, non-inherited job handle on parent exit.
        api.CloseHandle(child.job)
        self.assertFalse(any(tree.values()))
        self.assertEqual(api.events.count(('job', None, None)), 1)
        self.assertIn(('inherit', 11, 1, 0), api.events)

    def test_raw_createprocess_retains_thread_and_inherits_only_stdio(self):
        read_fd, write_fd = os.pipe()
        self.addCleanup(os.close, read_fd)
        win = mock.Mock(INFINITE=0xffffffff, WAIT_TIMEOUT=258, DUPLICATE_SAME_ACCESS=2, STARTF_USESTDHANDLES=256)
        win.CreatePipe.return_value = (801, 802)
        win.DuplicateHandle.side_effect = (901, 902, 903)
        win.CreateProcess.return_value = (701, 702, 703, 704)
        win.WaitForSingleObject.return_value = 0
        win.GetExitCodeProcess.return_value = 17
        crt = mock.Mock(); crt.open_osfhandle.return_value = write_fd
        startup = types.SimpleNamespace()
        with mock.patch.dict(sys.modules, _winapi=win, msvcrt=crt), \
                mock.patch.object(os, 'O_BINARY', 0, create=True), \
                mock.patch.object(subprocess, 'STARTUPINFO', return_value=startup, create=True):
            proc = flasher._SuspendedProcess([sys.executable, 'path space.py'], stdin=subprocess.PIPE,
                                            creationflags=0x204, stdout=mock.Mock(), stderr=subprocess.STDOUT)
            self.assertEqual(win.CreateProcess.call_args.args[5], 0x204)
            self.assertEqual(set(startup.lpAttributeList['handle_list']), {901, 902})
            self.assertEqual(proc._thread, 702)
            self.assertNotIn(mock.call(702), win.CloseHandle.call_args_list)
            proc.stdin.write(b'G'); proc.stdin.close()
            self.assertEqual(proc.wait(.1), 17)
            proc.close_handles()
            self.assertIn(mock.call(702), win.CloseHandle.call_args_list)
            self.assertIn(mock.call(701), win.CloseHandle.call_args_list)


class BundleAuthorizationTests(unittest.TestCase):
    def setUp(self):
        self.stack = ExitStack(); self.addCleanup(self.stack.close)
        self.root = Path(self.stack.enter_context(tempfile.TemporaryDirectory()))
        self.vendor = self.root / 'vendor/windows-x86_64'; self.vendor.mkdir(parents=True)
        self.source = self.root / 'src/usbboot/usbboot.c'; self.source.parent.mkdir(parents=True)
        self.source.write_bytes(b'source')
        self.tool = self.vendor / 'usbboot.exe'; self.tool.write_bytes(pe(['kernel32.dll']))
        self.manifest = self.vendor / 'MANIFEST.txt'
        self.manifest.write_text('\nInput SHA256 (paths relative to installer):\n' +
            hashlib.sha256(self.source.read_bytes()).hexdigest() + '  src/usbboot/usbboot.c\n' +
            '\nOutput SHA256:\n' + hashlib.sha256(self.tool.read_bytes()).hexdigest() + '  usbboot.exe\n', encoding='ascii')
        self.writer = self.vendor / flasher.WRITER_NAME; self.writer.write_bytes(b'writer')
        self.spl = self.vendor / 'disc_spl_lpddr3.bin'; self.spl.write_bytes(b'spl')
        self.work = self.root / 'flash'; self.work.mkdir()
        self.image = self.work / 'image.bin'; self.image.write_bytes(b'hsqs' + bytes(4092))
        self.plan = self.work / 'plan.bin'; self.plan.write_bytes(basegate.build_plan('257', str(self.image)))
        self.poison = self.work / 'poison.bin'; self.poison.write_bytes(b'\xee' * 128)
        self.cmd = flasher._flash_command(*map(str, (self.tool, self.spl, self.writer, self.image,
                                                    self.plan, self.poison, self.work / 'dbg.bin')))
        self.stack.enter_context(mock.patch.object(bundle, 'resource_root', return_value=str(self.root)))
        self.stack.enter_context(mock.patch.object(platform_probe, 'host', return_value=('windows', 'x86_64')))
        self.stack.enter_context(mock.patch.object(platform_probe, 'windows_dev_runtime', return_value=True))
        self.stack.enter_context(mock.patch.dict(os.environ, DISKOS_WINDOWS_DEV_FLASH='1'))
        self.stack.enter_context(mock.patch.object(flasher, 'IMG_SIZE', 4096))
        self.desc = self.stack.enter_context(mock.patch.object(flasher, '_writer_descriptor', return_value=(0, basegate.BLOB_SCHEMA)))
        self.probes = self.stack.enter_context(mock.patch.object(flasher, '_probe_helpers'))
        self.stack.enter_context(mock.patch.object(flasher.windows_usb, 'inventory', return_value={}))
        self.stack.enter_context(mock.patch.object(flasher.windows_usb, 'require_target'))
        self.stack.enter_context(mock.patch.object(flasher.windows_usb, 'diagnostic', return_value={'length': 5, 'hex': '5832303030'}))

    def test_bundle_manifest_and_dll_closure_verified_in_child(self):
        cmd, env = flash_child.authorized_command(capability(self.cmd))
        self.assertEqual(cmd, self.cmd); self.assertEqual(env['PATH'], str(self.vendor))
        self.probes.assert_called_once()
        self.tool.write_bytes(b'unverified')
        with self.assertRaises(flasher.FlashError): flash_child.bundled_tool()

    def test_private_dispatch_cannot_bypass_validation_with_a_good_gate(self):
        for valid in (False, True):
            request = capability(list(self.cmd))
            if not valid: request['cmd'][0] = str(self.root / 'usbboot.exe')
            raw = b'G' + json.dumps(request).encode('ascii')
            with mock.patch.object(sys, 'platform', 'win32'), \
                    mock.patch.object(sys, 'stdin', mock.Mock(buffer=io.BytesIO(raw))), \
                    mock.patch.object(sys, 'stdout', io.StringIO()), \
                    mock.patch.object(sys, 'stderr', io.StringIO()), \
                    mock.patch.object(flash_child, 'verify_supervision') as verified, \
                    mock.patch.object(subprocess, 'call', return_value=17) as native:
                self.assertEqual(cli.main([flash_child.CHILD_MODE, NONCE]), 17 if valid else 1)
                verified.assert_called_once_with(123)
                if valid:
                    self.assertEqual(native.call_args.args[0][0], str(self.tool))
                    self.assertEqual(native.call_args.kwargs['env']['PATH'], str(self.vendor))
                else:
                    native.assert_not_called()
                    self.probes.assert_not_called()

    def test_private_dll_closure_and_stale_manifest_rejected(self):
        self.tool.write_bytes(pe(['private.dll']))
        self.manifest.write_text(self.manifest.read_text().replace(
            hashlib.sha256(pe(['kernel32.dll'])).hexdigest(), hashlib.sha256(self.tool.read_bytes()).hexdigest()), encoding='ascii')
        with self.assertRaises(FileNotFoundError): flash_child.bundled_tool()
        (self.vendor / 'private.dll').write_bytes(pe(['kernel32.dll']))
        flash_child.bundled_tool()
        self.source.write_bytes(b'changed')
        with self.assertRaises(flasher.FlashError): flash_child.bundled_tool()

    def test_arbitrary_executable_and_raw_usb_operations_rejected(self):
        for index, value in ((0, str(self.root / 'usbboot.exe')), (5, str(self.image)),
                             (11, str(self.image)), (25, '0'), (31, '4096')):
            cmd = list(self.cmd); cmd[index] = value
            with self.subTest(index=index), self.assertRaises((ValueError, OSError)):
                flash_child.authorized_command(capability(cmd))
        with self.assertRaises(ValueError):
            flash_child.authorized_command(capability([str(self.tool), '--start1', '0']))
        self.probes.assert_not_called()

    def test_symlink_directory_junction_and_canonical_escape_rejected(self):
        link = self.vendor / 'linked.exe'
        if sys.platform != 'win32':
            link.symlink_to(self.tool)
            with self.assertRaises(ValueError): flash_child.checked_path(str(link), str(self.root))
        else:
            # A real symlink requires privileges on some Windows test boxes.
            with mock.patch.object(os, 'lstat', return_value=types.SimpleNamespace(st_mode=0o120777)):
                with self.assertRaises(ValueError): flash_child.checked_path(str(self.tool), str(self.root))
        with self.assertRaises(ValueError): flash_child.checked_path(str(self.root.parent), str(self.root))
        original = os.lstat
        def junction(path):
            st = original(path)
            if path == str(self.vendor): return types.SimpleNamespace(st_mode=st.st_mode, st_file_attributes=0x400)
            return st
        with mock.patch.object(os, 'lstat', side_effect=junction):
            with self.assertRaises(ValueError): flash_child.bundled_tool()
        with mock.patch.object(os.path, 'realpath', return_value=str(self.root.parent)):
            with self.assertRaises(ValueError): flash_child.checked_path(str(self.tool), str(self.root))

    def test_modified_plan_poison_writer_and_existing_readback_rejected(self):
        for path, raw in ((self.plan, b'forged'), (self.poison, b'bad')):
            original = path.read_bytes(); path.write_bytes(raw)
            with self.assertRaises(ValueError): flash_child.authorized_command(capability(self.cmd))
            path.write_bytes(original)
        with mock.patch.object(flasher, '_writer_descriptor', return_value=None):
            with self.assertRaises(ValueError): flash_child.authorized_command(capability(self.cmd))
        (self.work / 'dbg.bin').write_bytes(b'existing')
        with self.assertRaises(ValueError): flash_child.authorized_command(capability(self.cmd))
        self.probes.assert_not_called()

    def test_child_image_runtime_and_environment_gates_repeated(self):
        for runtime, env in ((False, '1'), (True, '0')):
            with mock.patch.object(platform_probe, 'windows_dev_runtime', return_value=runtime), \
                    mock.patch.dict(os.environ, DISKOS_WINDOWS_DEV_FLASH=env):
                with self.assertRaises(flasher.FlashError): flash_child.authorized_command(capability(self.cmd))
        self.image.write_bytes(b'hsqs')
        self.plan.write_bytes(basegate.build_plan('257', str(self.image)))
        with self.assertRaises(flasher.FlashError) as caught:
            flash_child.authorized_command(capability(self.cmd))
        self.assertEqual(caught.exception.code, 'E121')
        self.probes.assert_not_called()

    def test_public_child_rechecks_capability_and_existing_image_gate(self):
        request = capability(self.cmd); request['dev_requested'] = False
        with mock.patch.object(sys, 'frozen', True, create=True), \
                mock.patch.object(platform_probe, 'windows_native_runtime', return_value=True), \
                mock.patch.dict(os.environ, DISKOS_WINDOWS_DEV_FLASH=''):
            for enabled in (False, True):
                with mock.patch.object(flasher._build_capabilities, 'WINDOWS_FLASH', enabled):
                    if enabled:
                        self.assertEqual(flash_child.authorized_command(request)[0], self.cmd)
                        self.image.write_bytes(b'hsqs')
                        self.plan.write_bytes(basegate.build_plan('257', str(self.image)))
                    with self.assertRaises(flasher.FlashError) as caught:
                        flash_child.authorized_command(request)
                    self.assertEqual(caught.exception.code, 'E121' if enabled else 'E101')
        with self.assertRaises(flasher.FlashError): flash_child.authorized_command(request)


if __name__ == '__main__':
    unittest.main()
