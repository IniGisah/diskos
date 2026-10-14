# Default-off onedir build. Run with native Windows Python on Windows x64.
# Populate vendor/windows-x86_64 using build/build-windows-tools.sh first.
# Use -Wheelhouse <directory> for a fully offline, hash-locked install.
# Supply the official VS2022/MS64 DLL with -LibusbDll; no system search or download.
param([switch]$EnableWindowsFlash, [ValidateNotNullOrEmpty()][string]$Wheelhouse,
      [Parameter(Mandatory=$true)][ValidateNotNullOrEmpty()][string]$LibusbDll,
      [ValidatePattern('^[0-9a-fA-F]{64}$')][string]$NativeManifestSha256)
$ErrorActionPreference = 'Stop'
# Resolve caller-relative paths before changing to installer/.
$libusbPath = (Resolve-Path -LiteralPath $LibusbDll).ProviderPath
if (-not (Test-Path -LiteralPath $libusbPath -PathType Leaf)) {
    throw 'LibusbDll must be an existing file'
}
$libusbPin = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'windows-libusb.json') -Raw | ConvertFrom-Json
if ((Get-Item -LiteralPath $libusbPath).Length -ne $libusbPin.dll_size -or
    (Get-FileHash -LiteralPath $libusbPath -Algorithm SHA256).Hash.ToLowerInvariant() -ne $libusbPin.dll_sha256) {
    throw 'LibusbDll does not match the pinned official libusb 1.0.27 VS2022/MS64 DLL'
}
$wheelhousePath = $null
if ($PSBoundParameters.ContainsKey('Wheelhouse')) {
    $wheelhousePath = (Resolve-Path -LiteralPath $Wheelhouse).ProviderPath
    if (-not (Test-Path -LiteralPath $wheelhousePath -PathType Container)) {
        throw 'Wheelhouse must be an existing directory'
    }
}
Push-Location (Join-Path $PSScriptRoot '..')
$savedPythonEnv = @{}
try {
    foreach ($name in @('PYTHONPATH', 'PYTHONHOME', 'PYTHONSTARTUP')) {
        $savedPythonEnv[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
        [Environment]::SetEnvironmentVariable($name, $null, 'Process')
    }
    python -I -c "import platform, sys; sys.exit(0 if platform.system() == 'Windows' and platform.machine().lower() in ('amd64', 'x86_64') and sys.maxsize > 2**32 else 'Requires native Windows x64 Python')"
    if ($LASTEXITCODE -ne 0) { throw 'Unsupported build Python' }
    $capabilityArgs = @()
    if ($EnableWindowsFlash) {
        $revision = & git rev-parse --verify HEAD
        if ($LASTEXITCODE -ne 0 -or $revision -notmatch '^[0-9a-f]{40}$') {
            throw 'Windows flash capability requires a full installer commit SHA'
        }
        $dirty = & git status --porcelain --untracked-files=all
        if ($LASTEXITCODE -ne 0 -or $dirty) {
            throw 'Windows flash capability requires a clean installer revision'
        }
        $capabilityArgs = @('--', '--enable-windows-flash', '--source-commit', $revision)
    }
    if ($EnableWindowsFlash -and -not $PSBoundParameters.ContainsKey('NativeManifestSha256')) {
        throw 'Windows flash capability requires the retained tools-build NativeManifestSha256'
    }
    $sourceRoot = (Get-Location).Path
    $guardRevision = & python -I build/verify_git_blobs.py $sourceRoot $sourceRoot windows-x86_64
    if ($LASTEXITCODE -ne 0 -or $guardRevision -notmatch '^[0-9a-f]{40}$') {
        throw 'Source device inputs differ from committed blobs'
    }
    if ($EnableWindowsFlash -and $guardRevision -cne $revision) {
        throw 'Source revision changed during verification'
    }
    if (-not $EnableWindowsFlash) {
        $capabilityArgs = @('--', '--source-commit', $guardRevision)
    }
    $nativeVendor = Join-Path $sourceRoot 'vendor/windows-x86_64'
    $nativeHash = (Get-FileHash -LiteralPath (Join-Path $nativeVendor 'MANIFEST.txt') -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($PSBoundParameters.ContainsKey('NativeManifestSha256') -and
        $nativeHash -ne $NativeManifestSha256.ToLowerInvariant()) {
        throw 'Native manifest differs from the retained tools-build hash'
    }
    $nativeCode = @'
import hashlib
from pathlib import Path
import re
import sys

root, vendor = map(Path, sys.argv[1:3])
sys.path.insert(0, str(root / 'build'))
from deterministic_zip import checked_stat

def content(path):
    for ancestor in (path, *path.parents):
        checked_stat(ancestor)
    return path.read_bytes()

raw = content(vendor / 'MANIFEST.txt')
if hashlib.sha256(raw).hexdigest() != sys.argv[3].lower():
    raise ValueError('Native manifest differs from the retained tools-build hash')
inputs, outputs = raw.decode('ascii').split('\nOutput SHA256:\n')
_, inputs = inputs.split('\nInput SHA256 (paths relative to installer):\n')

def verify(section, base, required):
    records = {}
    for line in section.splitlines():
        match = re.fullmatch(r'([0-9a-f]{64}) [ *](.+)', line)
        if not match or match[2] not in required or match[2] in records:
            raise ValueError('Unexpected, duplicate or malformed native hash record')
        records[match[2]] = match[1]
    if set(records) != required:
        raise ValueError('Incomplete native manifest inventory')
    for name, digest in records.items():
        if hashlib.sha256(content(base / name)).hexdigest() != digest:
            raise ValueError('Native manifest hash mismatch: ' + name)

verify(inputs, root, {
    'corresponding-source/squashfs-tools_4.6.1.orig.tar.gz',
    'corresponding-source/libusb-1.0_1.0.27.orig.tar.bz2',
    'corresponding-source/lzo2_2.10.orig.tar.gz', 'corresponding-source/msys2-squashfs.patch',
    'src/usbboot/usbboot.c', 'build/build-windows-tools.sh',
    'src/windows-driver/bind_winusb.c', 'src/windows-driver/bind_winusb.manifest',
    'src/windows-driver/bind_winusb.rc', 'build/build-windows-driver.sh',
})
verify(outputs, vendor, {
    'mksquashfs.exe', 'unsquashfs.exe', 'usbboot.exe', 'bind_winusb.exe', 'msys-2.0.dll',
})
for name in ('my_write6_dram.bin', 'disc_spl_lpddr3.bin'):
    if content(vendor / name) != content(root / 'flash' / name):
        raise ValueError('Device blob differs from tracked flash input: ' + name)
'@
    python -I -c $nativeCode $sourceRoot $nativeVendor $nativeHash
    if ($LASTEXITCODE -ne 0) { throw 'Windows native input verification failed' }
    $verifyRuntimeSources = {
        param([string]$directory)
        $savedKeyring = [Environment]::GetEnvironmentVariable('MSYS2_GNUPGHOME', 'Process')
        try {
            [Environment]::SetEnvironmentVariable('MSYS2_GNUPGHOME', '/etc/pacman.d/gnupg', 'Process')
            & 'C:\msys64\usr\bin\env.exe' 'PATH=/usr/bin:/bin' /usr/bin/bash --noprofile --norc -p build/fetch-windows-sources.sh --verify ($directory.Replace('\', '/'))
            if ($LASTEXITCODE -ne 0) { throw 'Windows runtime source authentication failed' }
        } finally {
            [Environment]::SetEnvironmentVariable('MSYS2_GNUPGHOME', $savedKeyring, 'Process')
        }
    }
    & $verifyRuntimeSources 'corresponding-source/windows'
    # Never reuse an environment, freeze output, stage or ZIP from a prior run.
    # Retain failed runs for inspection; no automatic deletion.
    $run = Join-Path 'build/dist/windows' ([guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $run | Out-Null
    $venv = Join-Path $run 'venv'
    python -I -m venv $venv
    if ($LASTEXITCODE -ne 0) { throw 'Could not create build venv' }
    $buildPython = Join-Path $venv 'Scripts/python.exe'
    $installArgs = @('--require-hashes', '--only-binary=:all:', '-r', 'build/windows-build.lock')
    if ($wheelhousePath) {
        $installArgs += @('--no-index', '--find-links', $wheelhousePath)
    }
    $savedPipEnv = @{}
    try {
        foreach ($entry in [Environment]::GetEnvironmentVariables('Process').GetEnumerator()) {
            if ($entry.Key -like 'PIP_*') {
                $savedPipEnv[$entry.Key] = $entry.Value
                [Environment]::SetEnvironmentVariable($entry.Key, $null, 'Process')
            }
        }
        # NUL disables all pip config files on Windows, including global/site config.
        [Environment]::SetEnvironmentVariable('PIP_CONFIG_FILE', 'NUL', 'Process')
        & $buildPython -I -m pip --isolated --disable-pip-version-check install --no-cache-dir @installArgs
        if ($LASTEXITCODE -ne 0) { throw 'Could not install build requirements' }
    } finally {
        [Environment]::SetEnvironmentVariable('PIP_CONFIG_FILE', $null, 'Process')
        foreach ($name in $savedPipEnv.Keys) {
            [Environment]::SetEnvironmentVariable($name, $savedPipEnv[$name], 'Process')
        }
    }
    $dist = Join-Path $run 'dist'
    $work = Join-Path $run 'work'
    $libusbInput = Join-Path $work 'libusb'
    New-Item -ItemType Directory -Path $libusbInput | Out-Null
    Copy-Item -LiteralPath $libusbPath -Destination (Join-Path $libusbInput 'libusb-1.0.dll')
    & $buildPython -I -m PyInstaller --clean --noconfirm --distpath $dist --workpath $work build/diskos-installer.spec @capabilityArgs
    if ($LASTEXITCODE -ne 0) { throw 'Onedir build failed' }
    # Stage both entry points and their shared dependency collection together.
    $onedir = Join-Path $dist 'diskos-installer'
    & $buildPython -I -c $nativeCode $sourceRoot (Join-Path $onedir '_internal/vendor/windows-x86_64') $nativeHash
    if ($LASTEXITCODE -ne 0) { throw 'Frozen Windows native verification failed' }
    & $buildPython -I build/verify_git_blobs.py $sourceRoot (Join-Path $onedir '_internal') windows-x86_64 --ref $guardRevision --frozen
    if ($LASTEXITCODE -ne 0) { throw 'Frozen device inputs differ from committed blobs' }
    & $verifyRuntimeSources (Join-Path $onedir '_internal/corresponding-source/windows')
    $stage = Join-Path $run 'diskos-installer-windows-x86_64'
    $sourceRevision = 'unrecorded-default-off'
    if ($EnableWindowsFlash) { $sourceRevision = $revision }
    $stageCode = @'
import hashlib
import importlib.metadata
import os
from pathlib import Path
import platform
import shutil
import sys

sys.path.insert(0, str(Path('build').resolve()))
from deterministic_zip import checked_stat

onedir, stage = map(Path, sys.argv[1:3])
def fail(error):
    raise error

# Check the original collection before copying; never dereference a link.
checked_stat(onedir)
for folder, dirs, files in os.walk(onedir, followlinks=False, onerror=fail):
    for name in dirs + files:
        checked_stat(Path(folder) / name)
executables = ('diskos-installer.exe', 'diskos-installer-cli.exe')
if not all((onedir / name).is_file() for name in executables) or not (onedir / '_internal').is_dir():
    raise ValueError('Expected the complete Windows onedir collection')
shutil.copytree(onedir, stage, symlinks=True)
# Promote only sidecars already selected by the spec; keep runtime paths intact.
for name in ('LICENSE', 'NOTICE.md', 'README.md', 'licenses', 'src', 'build', 'corresponding-source'):
    source, target = stage / '_internal' / name, stage / name
    if source.is_dir():
        shutil.copytree(source, target, symlinks=True)
    else:
        shutil.copyfile(source, target)
for name in ('RECOVERY.txt', 'build/windows-build.lock', 'build/WINDOWS_FROZEN_DEPENDENCIES.md'):
    checked_stat(Path(name))
    shutil.copyfile(name, stage / name)
metadata = '\n'.join((
    'CPython=' + platform.python_version(),
    'pip=' + importlib.metadata.version('pip'),
    'PyInstaller=' + importlib.metadata.version('PyInstaller'),
    'WindowsFlash=' + sys.argv[3],
    'SourceCommit=' + sys.argv[4],
    'NativeManifestSHA256=' + hashlib.sha256(
        (stage / '_internal/vendor/windows-x86_64/MANIFEST.txt').read_bytes()).hexdigest(),
    'FreezeOptions=--clean --noconfirm; Windows onedir; upx=False; strip=False',
)) + '\n'
(stage / 'BUILD-METADATA.txt').write_bytes(metadata.encode('utf-8'))
records = []
for folder, dirs, files in os.walk(stage, followlinks=False, onerror=fail):
    for name in dirs + files:
        checked_stat(Path(folder) / name)
    for name in files:
        path = Path(folder) / name
        relative = path.relative_to(stage).as_posix()
        if relative == 'SHA256SUMS':
            raise ValueError('Unexpected pre-existing package inventory')
        with path.open('rb') as stream:
            digest = hashlib.file_digest(stream, 'sha256').hexdigest()
        records.append((relative, digest))
if not set(executables).issubset(name for name, digest in records):
    raise ValueError('Expected both Windows executables in the package inventory')
inventory = ''.join(digest + '  ' + name + '\n' for name, digest in sorted(records))
(stage / 'SHA256SUMS').write_bytes(inventory.encode('utf-8'))
'@
    & $buildPython -I -c $stageCode $onedir $stage "$EnableWindowsFlash" $sourceRevision
    if ($LASTEXITCODE -ne 0) { throw 'Could not stage onedir and SHA256 inventory' }
    & $buildPython -I build/verify_git_blobs.py $sourceRoot (Join-Path $stage '_internal') windows-x86_64 --ref $guardRevision --frozen
    if ($LASTEXITCODE -ne 0) { throw 'Staged device inputs differ from committed blobs' }
    # Two archives of the same immutable stage establish ZIP determinism only.
    $zip = Join-Path $run 'diskos-installer-windows-x86_64.zip'
    $checkZip = Join-Path $run 'diskos-installer-windows-x86_64.check.zip'
    & $buildPython -I build/deterministic_zip.py $stage $zip
    if ($LASTEXITCODE -ne 0) { throw 'Could not build final ZIP' }
    & $buildPython -I build/deterministic_zip.py $stage $checkZip
    if ($LASTEXITCODE -ne 0) { throw 'Could not build verification ZIP' }
    $zipHash = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLowerInvariant()
    $checkHash = (Get-FileHash -LiteralPath $checkZip -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($zipHash -ne $checkHash) { throw 'ZIP SHA256 mismatch: archive is not deterministic' }
    Write-Host "ZIP: $((Resolve-Path -LiteralPath $zip).ProviderPath)"
    Write-Host "SHA256: $zipHash (Windows flash capability: $EnableWindowsFlash)"
} finally {
    foreach ($name in $savedPythonEnv.Keys) {
        [Environment]::SetEnvironmentVariable($name, $savedPythonEnv[$name], 'Process')
    }
    Pop-Location
}
