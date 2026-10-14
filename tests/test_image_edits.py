"""One edit phase on both filesystems, including hostile image namespaces."""
import hashlib
import os
from pathlib import Path
import stat
import tempfile
import unittest
from unittest import mock

from diskos_installer import bundle, imagebuild as ib
from diskos_installer.buildfs import ArchiveFS, TreeFS
from diskos_installer.pseudofs import Archive, MARK

CLOCK = 1791417600
BOOT = b'usr/project/fiio_init.sh'
RAW = ib._LAUNCH.encode('utf-8')
CFG = {'rootpub': b'offline-test-key\n', 'epochs': b'MIN_KEY_EPOCH=1\nMIN_UI_EPOCH=1\n'}


class EditTests(unittest.TestCase):
    def setUp(self):
        # Minimal script, explicitly test-pinned; the full comparison uses the real pin.
        self.pin = mock.patch.object(ib, 'KNOWN_FIIO_INIT_SHA256', {hashlib.sha256(RAW).hexdigest()})
        self.pin.start()
        self.addCleanup(self.pin.stop)
        if os.name != 'nt':
            mask = os.umask(0o022)
            self.addCleanup(os.umask, mask)

    def backends(self, busybox=True):
        temp = tempfile.TemporaryDirectory(prefix='s3-edits-', dir=os.environ.get('DISKOS_WIN_SCRATCH'))
        self.addCleanup(temp.cleanup)
        root = Path(temp.name) / 'tree'
        root.mkdir()
        archive = Archive(b'/ D 1 755 0 0\n' + MARK)
        self.addCleanup(archive.close)
        systems = [ArchiveFS(archive, clock=CLOCK)]
        if os.name != 'nt':
            systems.append(TreeFS(root, clock=CLOCK))
        for fs in systems:
            fs.replace_file(BOOT, RAW, 0o751)
            if busybox:
                fs.replace_file(b'bin/busybox', b'busybox', 0o755)
        return systems

    def link(self, fs, path, target, hard=False):
        if isinstance(fs, ArchiveFS):
            if hard:
                fs.archive.entries[path] = [b'L', target]
            else:
                fs.archive.symlink(path, target)
        elif hard:
            os.link(fs._path(target), fs._path(path))
        else:
            fs._path(path).symlink_to(os.fsdecode(target))

    def run_edit(self, fs, variant='public', cfg=None):
        return ib.edit_image(fs, bundle.data('mq_ui'), variant, cfg)

    def error(self, fs, code, pattern=None, **kw):
        with self.assertRaises(ib.BuildError) as caught:
            self.run_edit(fs, **kw)
        self.assertEqual(caught.exception.code, code)
        if pattern:
            self.assertRegex(str(caught.exception), pattern)

    def test_variants_and_ota(self):
        for variant in ('public', 'dev'):
            for cfg in (None, CFG):
                for fs in self.backends():
                    with self.subTest(backend=type(fs).__name__, variant=variant, ota=bool(cfg)):
                        # Case collisions and a contained symlink parent remain distinct/safe.
                        fs.replace_file(b'Case', b'upper')
                        fs.replace_file(b'case', b'lower')
                        fs.mkdir(b'hooks')
                        fs.mkdir(b'etc')
                        self.link(fs, b'etc/init.d', b'../hooks')
                        fs.replace_file(b'etc/init.d/S96diskos_select', b'old', 0o4777)
                        self.link(fs, b'bin/rm', b'/outside')
                        patched, bb, sha, size = self.run_edit(fs, variant, cfg)
                        self.assertEqual(fs.read_bytes(BOOT), patched)
                        self.assertEqual(stat.S_IMODE(fs.stat(BOOT).st_mode), 0o751)
                        self.assertEqual(bb, hashlib.sha256(b'busybox').hexdigest())
                        copies = (*ib._BOOT_DECISION_FILES, *ib._SUPPORT_FILES,
                                  ('bin/rm', 'diskos-rmguard'), ('opt/diskos/bin/rm', 'diskos-rmguard'),
                                  ('opt/diskos/mq_ui', 'mq_ui'), ('usr/project/dropbearmulti', 'dropbearmulti'),
                                  ('usr/project/diskos-debug.sh', 'diskos-debug.sh'))
                        for rel, name in copies:
                            self.assertEqual(fs.read_bytes(rel.encode()), Path(bundle.data(name)).read_bytes())
                            self.assertEqual(stat.S_IMODE(fs.lstat(rel.encode()).st_mode), 0o755)
                        self.assertEqual(sha, hashlib.sha256(fs.read_bytes(b'opt/diskos/mq_ui')).hexdigest())
                        self.assertEqual(fs.read_bytes(b'etc/diskos_manifest'),
                                         f'SHA256={sha}\nSIZE={size}\nARCH=mips-le\nVARIANT={variant}\nBUILT=2026-10-08T00:00:00Z\n'.encode())
                        if cfg:
                            for name, mode in ib._OTA_FILES:
                                path = b'etc/diskos-ota/' + name.encode()
                                want = Path(bundle.data(name)).read_bytes() if name == 'diskos-verify.sh' else cfg['rootpub' if name == 'root.pub.pem' else 'epochs']
                                self.assertEqual(fs.read_bytes(path), want)
                                self.assertEqual(stat.S_IMODE(fs.lstat(path).st_mode), mode)
                        else:
                            with self.assertRaises(FileNotFoundError):
                                fs.lstat(b'etc/diskos-ota')
                        if variant == 'dev':
                            self.assertEqual(fs.read_bytes(b'etc/init.d/S99usbserial'), Path(bundle.data('S99usbserial')).read_bytes())
                        else:
                            with self.assertRaises(FileNotFoundError):
                                fs.lstat(b'etc/init.d/S99usbserial')
                        self.assertEqual((fs.read_bytes(b'Case'), fs.read_bytes(b'case')), (b'upper', b'lower'))

    def test_error_codes_and_boot_refusals(self):
        for raw, pattern in ((b'unknown\r\n', 'not the stock'),
                             (RAW + b'diskOS override', 'already contains'),
                             (b'no anchor', 'not found'), (RAW * 2, 'ambiguous')):
            for fs in self.backends():
                fs.replace_file(BOOT, raw)
                pins = set() if pattern == 'not the stock' else {hashlib.sha256(raw).hexdigest()}
                with mock.patch.object(ib, 'KNOWN_FIIO_INIT_SHA256', pins):
                    self.error(fs, 'E223', pattern)
                self.assertEqual(fs.read_bytes(BOOT), raw)
        for fs in self.backends():
            self.error(fs, 'E250', variant='other')
            self.assertEqual(fs.read_bytes(BOOT), RAW)

    def test_missing_payload_is_preflight(self):
        # A controlled payload folder with one file removed: the real bundle.data
        # lookup must raise its E102 unchanged, not a build error.
        from diskos_installer.errors import PreflightError
        ui = bundle.data('mq_ui')
        source = bundle.payload_dir()
        for missing, variant in (('S96diskos_select', 'public'), ('S99usbserial', 'dev')):
            temp = tempfile.TemporaryDirectory(prefix='s3-payload-', dir=os.environ.get('DISKOS_WIN_SCRATCH'))
            self.addCleanup(temp.cleanup)
            for name in os.listdir(source):
                if name != missing and os.path.isfile(os.path.join(source, name)):
                    Path(temp.name, name).write_bytes(Path(source, name).read_bytes())
            with mock.patch.object(bundle, 'payload_dir', return_value=temp.name):
                for fs in self.backends():
                    with self.subTest(backend=type(fs).__name__, missing=missing):
                        with self.assertRaises(PreflightError) as caught:
                            ib.edit_image(fs, ui, variant, None)
                        self.assertNotIsInstance(caught.exception, ib.BuildError)
                        self.assertEqual(caught.exception.code, 'E102')
                        self.assertEqual(caught.exception.message, f"bundled payload '{missing}' not found")
                        self.assertEqual(caught.exception.action,
                                         'the install is incomplete - re-download the installer')

    def test_hostile_paths(self):
        for path in (BOOT, b'etc/init.d', b'etc/diskos_manifest', b'opt/diskos',
                     b'etc/diskos-ota', b'bin'):
            for target in (b'../../../../outside', b'/outside', path.rpartition(b'/')[2]):
                for fs in self.backends(busybox=path != b'bin'):
                    with self.subTest(backend=type(fs).__name__, path=path, target=target):
                        if path == BOOT:
                            fs.unlink(path)
                        fs.mkdir(path.rpartition(b'/')[0] or b'/')
                        self.link(fs, path, target)
                        self.error(fs, 'E233', cfg=CFG)
        for path in (BOOT, b'etc/diskos_manifest', b'bin/busybox', b'bin/rm'):
            for fs in self.backends():
                if path in (BOOT, b'bin/busybox'):
                    fs.unlink(path)
                fs.mkdir(path)
                self.error(fs, 'E233')
        for path in (BOOT, b'etc/diskos_manifest', b'opt/diskos/mq_ui'):
            for fs in self.backends():
                if path != BOOT:
                    fs.replace_file(path, b'sentinel')
                self.link(fs, b'alias', path, hard=True)
                before = fs.read_bytes(b'alias')
                self.error(fs, 'E233')
                self.assertEqual(fs.read_bytes(b'alias'), before)

    def test_rm_exception_and_busybox_identity(self):
        for fs in self.backends():
            self.link(fs, b'bin/rm', b'bin/busybox', hard=True)
            self.run_edit(fs)
            self.assertEqual(fs.read_bytes(b'bin/busybox'), b'busybox')
        for fs in self.backends():
            real = fs.replace_file
            def corrupt(path, source, mode=None):
                real(path, source, mode)
                if path == b'opt/diskos/bin/rm':
                    real(b'bin/busybox', b'changed')
            with mock.patch.object(fs, 'replace_file', side_effect=corrupt):
                self.error(fs, 'E233', 'BusyBox changed')
        for fs in self.backends():
            self.link(fs, b'escape', b'../outside')
            with self.assertRaises(ib.BuildError) as caught:
                ib._edit_within(fs, b'escape/file', code='E232')
            self.assertEqual(caught.exception.code, 'E232')
