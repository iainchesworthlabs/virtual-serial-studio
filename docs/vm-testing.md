# Testing in the VSS guests

Anything that loads a driver, turns on test signing or needs a disposable kernel runs in a VMware Workstation guest, never on
the development machine. A snapshot is the unit of disposal.

## The guests

`tools/vm/matrix.json` lists the guests, and the scripts in `tools/vm` build them from the install media (see [Building a guest](#building-a-guest)). The harness works on these three and refuses every other VM.

| Guest | System | Used for |
|---|---|---|
| `VSS - Windows 10` | Windows 10 22H2, UEFI, Secure Boot off | The Windows virtual COM driver on a down-level stack, the installed package, a screenshot |
| `VSS - Windows 11` | Windows 11 25H2, UEFI, Secure Boot off, no vTPM | The same on the newest stack |
| `VSS - Ubuntu 26.04` | Ubuntu 26.04.1 LTS | Pseudo-terminal ports, `vport`, what third-party serial programs see |

The Windows guests have test signing on, VMware Tools, auto-logon (so a screenshot shows a desktop) and an empty `C:\vss`.
The Ubuntu guest has `open-vm-tools`, `python3`, `python3-serial`, `socat`, `picocom`, `minicom`, `screen` and `udev`, and
passwordless `sudo`, as the hosted CI runners do. None has a compiler: CI builds the package and the harness copies it in.
Each has a snapshot named `clean`, taken after setup.

## Credentials

The guest user name and password never enter the repository. Set them in your own shell:

```powershell
$env:VSS_VM_USER = 'vss'
$env:VSS_VM_PASSWORD = Read-Host -AsSecureString | ConvertFrom-SecureString -AsPlainText
```

For guests that only exist to test this software, `. ./tools/vm/credentials.ps1 -Create` makes the user `vss` with a random
password in `$HOME/.vss/guest-credentials.json` (readable by your user only, never replaced once it exists) and loads both variables
without printing the password. Use it for these disposable guests and for nothing else.

`vmrun` takes them as arguments, so they are visible to other processes on the host while a guest command runs. The harness
prints `<hidden>` in place of the password. Under WSL, add both names to `WSLENV`.

## Building a guest

The install media are in `E:\ISOs`. `tools/vm/iso-hashes.json` holds the SHA-256 of each, and `new-vm.ps1` checks it.

```powershell
# 1. The answer disc for the unattended install, written outside the repository.
#    It reads VSS_VM_USER and VSS_VM_PASSWORD from your shell and prints neither.
./tools/vm/new-seed.ps1 -Guest windows11

# 2. The disk and the VMX. This does not start the guest, and it never overwrites one.
./tools/vm/new-vm.ps1 -Guest windows11

# 3. Install it, verify it and take the snapshot, with nobody at the console. This needs the credentials
#    in the environment (credentials.ps1) and takes between half an hour and two hours.
./tools/vm/install-guest.ps1 -Guest windows11
```

The guest ids are `windows10`, `windows11` and `ubuntu2604`. For Ubuntu, `new-seed.ps1` hashes the password with `openssl passwd -6`
in WSL Debian, from standard input, and reads your SSH public key from `%USERPROFILE%\.vss`. The Windows answer file skips the TPM,
Secure Boot, RAM and CPU checks of Windows 11 setup, so the guest needs no virtual TPM and no encryption password. After the first
logon, `tools/vm/guest/setup-guest.ps1` installs VMware Tools, turns on test signing, creates `C:\vss` and powers the guest off.
`install-guest.ps1` runs the whole unattended install. Windows Setup restarts the machine once, and the install ISO would then boot again and start over, so the script stops the guest at that moment, disconnects the install ISO and starts it from the disk. When the guest has powered itself off, the script starts it, waits for VMware Tools, runs a check in it with the credentials, and only then takes the `clean` snapshot. Any other result takes no snapshot.

Two installers wait for a key that nobody can press: Subiquity asks "Continue with autoinstall?", and the Windows 10 media ask "Press any key to boot from CD". `new-vm.ps1` boots patched copies of those two ISOs, made by `patch_ubuntu_iso.py` and `patch_windows_iso.py` (they need `pycdlib`) and kept in `_iso` next to the VMs. The originals are never modified. The Windows disks are SATA, because the virtual NVMe disk stalled Setup.

## The loop

```powershell
$vm = 'D:\Virtual Machines\VSS - Windows 10\VSS - Windows 10.vmx'

./tools/vm/vmrun.ps1 revert -Vm $vm            # always start from the prepared state
./tools/vm/vmrun.ps1 start  -Vm $vm
./tools/vm/vmrun.ps1 wait-guest -Vm $vm
./tools/vm/vmrun.ps1 copy-to -Vm $vm -Local build\package.zip -Guest 'C:\vss\package.zip'
./tools/vm/vmrun.ps1 run -Vm $vm -CommandLine 'powershell -File C:\vss\test.ps1'
"guest exit code: $LASTEXITCODE"
./tools/vm/vmrun.ps1 screenshot -Vm $vm -Local out\windows10.png
./tools/vm/vmrun.ps1 stop -Vm $vm
```

`run` returns no output from the guest, so write it to a file in the guest and bring it back with `copy-from`. `wait-guest` is the check that a guest is ready: it asks the guest a question with the credentials until it answers, and it takes about 30 seconds after a revert. `wait-tools` only reads the Tools state, asks without credentials (with them `vmrun` kept answering "installed" while Tools were running), and can report "installed" for minutes after the guest is up, so use `wait-guest` before copying or running anything. `run` exits with the exit code of the guest command, and fails if the guest does not report one. On a Windows guest the command line is PowerShell syntax; on Linux it is bash. `snapshot` creates a
snapshot once and fails if the name exists. `revert` fails if the snapshot is missing. `-DryRun` prints the `vmrun` command
lines without running them. `tools/vm/test-vmrun.ps1` checks all of this against a stand-in for `vmrun`, and `test-vm.ps1`, `test-seed.ps1` and `test-credentials.ps1` check the guest builders and the credentials script with placeholders. They run as the `ctest` suites `vm_harness`, `vm_builder`, `vm_seed` (Windows only) and `vm_credentials`, with `vm_guest_cell` for `test-guest-cell.ps1` (see below), so CI runs them wherever PowerShell 7 is present.

## Running a cell in a guest

The guests have no compiler, so the programs are built outside them and copied in. Two scripts build what a guest needs, and a third runs the cell:

```powershell
# Windows guests: needs Visual Studio, Qt (ICL_QT_ROOT) and Boost (BOOST_ROOT) on this machine.
./tools/vm/make-bundle.ps1 -Repository . -Out D:\bundle-win
./tools/vm/guest-cell.ps1 -Guest windows11 -Bundle D:\bundle-win -Commit (git rev-parse --short HEAD)

# Ubuntu guest: run the first line on Linux or in WSL, with Qt (VSS_QT_ROOT) and Boost (BOOST_ROOT).
tools/vm/make-bundle.sh . /mnt/d/bundle-ubuntu
./tools/vm/guest-cell.ps1 -Guest ubuntu2604 -Bundle D:\bundle-ubuntu -Package virtual-serial-studio-dry-run-linux-x86_64.tar.gz
```

`guest-cell.ps1` reverts to `clean`, starts the guest, copies in the bundle and `tools/vm/guest/win-cell.ps1` or `linux-cell.sh`, runs `ctest` and `tools/matrix/run_cell.py` in the guest, brings back `<Out>\<guest>\cell-out` (the result JSON, the logs and the screenshots) and stops the guest. It exits with 1 when a test failed or did not run. `-Package` is for T9: the archive from the Release workflow's dry run, for that system. The script refuses a guest that is already running, because starting or stopping a guest cuts off whoever is using it; `-Force` overrides that.

The guest scripts install what the program needs on Ubuntu (cmake for `ctest`, the OpenGL, EGL and fontconfig libraries Qt loads, and Xvfb with the xcb libraries for T9). On Windows the bundle carries a Python, PowerShell 7 for the `vm_*` suites, the Visual C++ runtime and the Qt offscreen plugin. `tools/vm/test-guest-cell.ps1` checks the order of the `vmrun` calls with `-DryRun`.

## Running the driver tests: a 32-bit program and a hung program

The Windows guests also run the driver. `drivers/windows/test/test-wow64.ps1` drives it with a small
C client (`drivers/windows/test/client32`), as a 32-bit program and as a native one, and holds a port
the way a hung program does. The programs that matter on Windows are old 32-bit ones, so the first
question is whether one of them works against the driver, and the second, when one hangs, whether
the driver or the program holds the port.

**There is no 32-bit guest.** The driver is x64 and ARM64 only, so a 32-bit Windows cannot load it,
and an x86 driver build does not exist. The Windows 10 and Windows 11 guests are 64-bit, and the 32-bit
client runs in them under WOW64, which is how a 32-bit program meets the driver on any 64-bit Windows.

```powershell
# Build the driver and the packages (drivers/windows/build.ps1), then the bundle. The script builds the
# client with MSVC as Win32 and x64, or takes the CI artifact vss-com-client-x64 with -Client.
./drivers/windows/build.ps1 -Driver all
./tools/vm/make-driver-bundle.ps1 -Repository . -Out D:\bundle-driver\driver-bundle.zip

# From the clean snapshot to the evidence. It signs the packages in the guest with a throwaway
# certificate, installs each driver, runs the tests and uninstalls. Add -DriverSuite for test-driver.ps1.
./tools/vm/guest-cell.ps1 -Guest windows11 -DriverBundle D:\bundle-driver\driver-bundle.zip -Out out\driver
./tools/vm/guest-cell.ps1 -Guest windows10 -DriverBundle D:\bundle-driver\driver-bundle.zip -Out out\driver
```

`-Bundle` and `-DriverBundle` can be given together; the cell runs first and the driver tests after it, in
the same guest. The evidence comes back to `<Out>\<guest>\driver-out\<kmdf|umdf>`: one JSON file per step
(`result-wow64.json` holds W1 to W4 and T1 to T7), a log per step, and the JSON the client printed
(`exercise-x86.json`, `probe-x86-busy.json` and the others). The script exits with 1 when a test failed.
A test that ends `unexpected` is a result to read, not a pass: the detail says what the driver did that the
design did not predict. `skip` carries its reason (T5 is skipped only when the `public.h` shipped in the bundle has
no revoke request, which the driver now has; T7 is for the user-mode driver). The guest side of a driver run is
`tools/vm/guest/win-driver.ps1`, which `guest-cell.ps1` copies in with the bundle.

Run the tests by hand in a guest that is already prepared with the folders `make-driver-bundle.ps1`
makes (`kmdf\` and `umdf\`):

```powershell
./prepare-test-signing.ps1 -ConfirmDisposableGuest
./install-driver.ps1 -ResultPath C:\vss\install.json            # -Flavour umdf -Helper for the user-mode driver
./test-wow64.ps1 -ResultPath C:\vss\wow64.json                  # -Only T2 runs one test
./uninstall-driver.ps1
```

## Safety

- Never load a driver or turn on test signing on the development machine.
- Revert to `clean` before each run. A failed install can leave a guest in a state that is tedious to repair.
- The snapshot, the VM files and the seed ISOs hold the guest password. Keep them out of every repository and every
  artifact upload.
