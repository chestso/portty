# Testing bloom-terminal on Windows with QEMU/KVM

For full interactive testing (ConPTY shell sessions), use a Windows VM with QEMU/KVM. This requires a Windows 11 ISO — Microsoft provides free [90-day LTSC evaluation editions](https://www.microsoft.com/en-us/evalcenter/evaluate-windows-11-enterprise).

## VM setup

```bash
# Install QEMU, UEFI firmware, and software TPM
sudo dnf install qemu-system-x86 edk2-ovmf swtpm

# Create directories for VM files
mkdir -p ~/vm/shared ~/vm/tpm

# Download virtio drivers (fast paravirtualized disk/network)
wget -O ~/vm/virtio-win.iso \
  'https://fedorapeople.org/groups/virt/virtio-win/direct-downloads/stable-virtio/virtio-win.iso'

# Create a 40GB thin-provisioned disk image
qemu-img create -f qcow2 ~/vm/win11.qcow2 40G

# Copy writable UEFI variables (Secure Boot variant)
cp /usr/share/edk2/ovmf/OVMF_VARS.secboot.fd ~/vm/OVMF_VARS.fd
```

## Bypass Windows 11 hardware checks

Windows 11 requires TPM 2.0 and 64GB storage, which a VM may not satisfy. Create an `autounattend.xml` on a floppy image to bypass these checks automatically:

```bash
cat > ~/vm/autounattend.xml << 'XML'
<?xml version="1.0" encoding="utf-8"?>
<unattend xmlns="urn:schemas-microsoft-com:unattend"
          xmlns:wcm="http://schemas.microsoft.com/WMIConfig/2002/State">
    <settings pass="windowsPE">
        <component name="Microsoft-Windows-Setup" processorArchitecture="amd64"
                   publicKeyToken="31bf3856ad364e35" language="neutral"
                   versionScope="nonSxS">
            <RunSynchronous>
                <RunSynchronousCommand wcm:action="add">
                    <Order>1</Order>
                    <Path>reg add HKLM\SYSTEM\Setup\LabConfig /v BypassTPMCheck /t REG_DWORD /d 1 /f</Path>
                </RunSynchronousCommand>
                <RunSynchronousCommand wcm:action="add">
                    <Order>2</Order>
                    <Path>reg add HKLM\SYSTEM\Setup\LabConfig /v BypassSecureBootCheck /t REG_DWORD /d 1 /f</Path>
                </RunSynchronousCommand>
                <RunSynchronousCommand wcm:action="add">
                    <Order>3</Order>
                    <Path>reg add HKLM\SYSTEM\Setup\LabConfig /v BypassStorageCheck /t REG_DWORD /d 1 /f</Path>
                </RunSynchronousCommand>
            </RunSynchronous>
        </component>
    </settings>
</unattend>
XML

dd if=/dev/zero of=~/vm/autounattend.img bs=1440k count=1
mkfs.fat ~/vm/autounattend.img
mcopy -i ~/vm/autounattend.img ~/vm/autounattend.xml ::/autounattend.xml
```

## Install Windows

ISOs are attached as USB storage (more reliable than IDE CD-ROM with UEFI). The floppy image provides the autounattend bypass. A software TPM is started for Secure Boot support.

```bash
# Start software TPM
swtpm socket --tpmstate dir=~/vm/tpm \
    --ctrl type=unixio,path=~/vm/tpm/swtpm-sock --tpm2 --daemon

qemu-system-x86_64 \
    -enable-kvm -cpu host -m 4G -smp 4 \
    -machine q35,smm=on \
    -global driver=cfi.pflash01,property=secure,value=on \
    -drive if=pflash,format=raw,readonly=on,file=/usr/share/edk2/ovmf/OVMF_CODE.secboot.fd \
    -drive if=pflash,format=raw,file=~/vm/OVMF_VARS.fd \
    -chardev socket,id=chrtpm,path=~/vm/tpm/swtpm-sock \
    -tpmdev emulator,id=tpm0,chardev=chrtpm \
    -device tpm-tis,tpmdev=tpm0 \
    -drive file=~/vm/win11.qcow2,format=qcow2,if=virtio \
    -device qemu-xhci \
    -device usb-storage,drive=install \
    -drive id=install,file=~/vm/win11-ltsc-eval.iso,media=cdrom,readonly=on,if=none \
    -device usb-storage,drive=virtio \
    -drive id=virtio,file=~/vm/virtio-win.iso,media=cdrom,readonly=on,if=none \
    -drive file=~/vm/autounattend.img,format=raw,index=0,if=floppy \
    -device virtio-net-pci,netdev=net0 -netdev user,id=net0 \
    -vga virtio -display sdl,gl=on -boot order=d
```

When the installer asks for a disk driver: click **Load driver** → **Browse** → drive `F:` (virtio-win) → `viostor` → `w11` → `amd64` → **OK**. Skip the product key.

## Boot the VM

After installation, boot without the install media:

```bash
swtpm socket --tpmstate dir=~/vm/tpm \
    --ctrl type=unixio,path=~/vm/tpm/swtpm-sock --tpm2 --daemon

qemu-system-x86_64 \
    -enable-kvm -cpu host -m 4G -smp 4 \
    -machine q35,smm=on \
    -global driver=cfi.pflash01,property=secure,value=on \
    -drive if=pflash,format=raw,readonly=on,file=/usr/share/edk2/ovmf/OVMF_CODE.secboot.fd \
    -drive if=pflash,format=raw,file=~/vm/OVMF_VARS.fd \
    -chardev socket,id=chrtpm,path=~/vm/tpm/swtpm-sock \
    -tpmdev emulator,id=tpm0,chardev=chrtpm \
    -device tpm-tis,tpmdev=tpm0 \
    -drive file=~/vm/win11.qcow2,format=qcow2,if=virtio \
    -virtfs local,path=~/vm/shared,mount_tag=shared,security_model=mapped-xattr \
    -device virtio-net-pci,netdev=net0 -netdev user,id=net0 \
    -vga virtio -display sdl,gl=on
```

## Deploy and test

```bash
# Cross-compile
./build.sh --mingw64

# Copy binary and DLLs to the shared folder
cp build-mingw64/src/.libs/bloom-terminal.exe build-mingw64/src/.libs/*.dll ~/vm/shared/
```

Run `bloom-terminal.exe` from the shared folder inside Windows.
