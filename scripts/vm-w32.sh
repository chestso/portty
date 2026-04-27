#!/bin/bash
# vm-w32.sh - Windows VM lifecycle (QEMU/KVM).
#
# Subcommands:
#   setup    - download ISOs, create disk images, UEFI vars, autounattend floppy
#   install  - boot VM from ISO to install Windows
#   run      - boot the VM normally (transfer disk attached)
#   deploy   - cross-compile and write exe + DLLs to transfer disk

set -eu

cd "$(dirname "$0")/.."

W32_VM_DIR="vm-w32"
VM_DISK="$W32_VM_DIR/win11.qcow2"
VM_ISO="$W32_VM_DIR/win11-ltsc-eval.iso"
VM_VIRTIO="$W32_VM_DIR/virtio-win.iso"
VM_OVMF_VARS="$W32_VM_DIR/OVMF_VARS.fd"
VM_TPM_DIR="$W32_VM_DIR/tpm"
VM_TRANSFER="$W32_VM_DIR/transfer.img"
VM_AUTOUNATTEND="$W32_VM_DIR/autounattend.img"
OVMF_CODE="/usr/share/edk2/ovmf/OVMF_CODE.secboot.fd"

cmd_setup() {
	for cmd in qemu-system-x86_64 swtpm qemu-img mcopy mkfs.fat; do
		if ! command -v "$cmd" >/dev/null 2>&1; then
			echo "ERROR: $cmd not found" >&2
			echo "Install with: sudo dnf install qemu-system-x86 swtpm qemu-img mtools dosfstools" >&2
			exit 1
		fi
	done
	if [ ! -f "$OVMF_CODE" ]; then
		echo "ERROR: OVMF Secure Boot firmware not found: $OVMF_CODE" >&2
		echo "Install with: sudo dnf install edk2-ovmf" >&2
		exit 1
	fi

	mkdir -p "$W32_VM_DIR" "$VM_TPM_DIR"

	if [ ! -f "$VM_ISO" ]; then
		echo "Downloading Windows 11 LTSC evaluation ISO (~4.7 GB)..."
		curl -L -o "$VM_ISO" \
			'https://go.microsoft.com/fwlink/?linkid=2289029&clcid=0x409&culture=en-us&country=us'
	fi

	if [ ! -f "$VM_VIRTIO" ]; then
		echo "Downloading virtio-win drivers ISO..."
		curl -L -o "$VM_VIRTIO" \
			'https://fedorapeople.org/groups/virt/virtio-win/direct-downloads/stable-virtio/virtio-win.iso'
	fi

	if [ ! -f "$VM_DISK" ]; then
		echo "Creating 40GB disk image..."
		qemu-img create -f qcow2 "$VM_DISK" 40G
	fi

	if [ ! -f "$VM_OVMF_VARS" ]; then
		cp /usr/share/edk2/ovmf/OVMF_VARS.secboot.fd "$VM_OVMF_VARS"
	fi

	if [ ! -f "$VM_AUTOUNATTEND" ]; then
		echo "Creating autounattend floppy image..."
		XML_FILE="$W32_VM_DIR/autounattend.xml"
		cat >"$XML_FILE" <<'XML'
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
		dd if=/dev/zero of="$VM_AUTOUNATTEND" bs=1440k count=1 2>/dev/null
		mkfs.fat "$VM_AUTOUNATTEND" >/dev/null
		mcopy -i "$VM_AUTOUNATTEND" "$XML_FILE" ::/autounattend.xml
		rm -f "$XML_FILE"
	fi

	if [ ! -f "$VM_TRANSFER" ]; then
		echo "Creating 256MB transfer disk image..."
		dd if=/dev/zero of="$VM_TRANSFER" bs=1M count=256 2>/dev/null
		printf 'o\nn\np\n1\n\n\nt\nc\nw\n' | fdisk "$VM_TRANSFER" >/dev/null 2>&1
		mkfs.fat -F 32 --offset 2048 "$VM_TRANSFER" >/dev/null
	fi

	echo "VM setup complete. Next: ./scripts/vm-w32.sh install"
}

launch_vm() {
	local mode="$1"

	if [ ! -f "$VM_DISK" ]; then
		echo "ERROR: VM disk not found. Run: ./scripts/vm-w32.sh setup" >&2
		exit 1
	fi

	mkdir -p "$VM_TPM_DIR"
	swtpm socket \
		--tpmstate dir="$VM_TPM_DIR" \
		--ctrl type=unixio,path="$VM_TPM_DIR/swtpm-sock" \
		--tpm2 \
		--daemon

	QEMU_ARGS=(
		-enable-kvm
		-cpu host
		-m 4G
		-smp 4
		-machine q35,smm=on
		-global driver=cfi.pflash01,property=secure,value=on
		-drive "if=pflash,format=raw,readonly=on,file=$OVMF_CODE"
		-drive "if=pflash,format=raw,file=$VM_OVMF_VARS"
		-drive "file=$VM_DISK,format=qcow2,if=virtio"
		-chardev "socket,id=chrtpm,path=$VM_TPM_DIR/swtpm-sock"
		-tpmdev emulator,id=tpm0,chardev=chrtpm
		-device tpm-tis,tpmdev=tpm0
		-device virtio-net-pci,netdev=net0
		-netdev user,id=net0
		-vga virtio
		-display sdl,gl=on
	)

	if [ "$mode" = "install" ]; then
		if [ ! -f "$VM_ISO" ]; then
			echo "ERROR: Windows ISO not found. Run: ./scripts/vm-w32.sh setup" >&2
			exit 1
		fi
		echo "Booting VM in install mode..."
		echo "When installer asks for disk driver: Load driver -> Browse -> F: -> viostor/w11/amd64"
		cp /usr/share/edk2/ovmf/OVMF_VARS.secboot.fd "$VM_OVMF_VARS"
		QEMU_ARGS+=(
			-device qemu-xhci
			-device usb-storage,drive=install
			-drive "id=install,file=$VM_ISO,media=cdrom,readonly=on,if=none"
			-device usb-storage,drive=virtio
			-drive "id=virtio,file=$VM_VIRTIO,media=cdrom,readonly=on,if=none"
			-drive "file=$VM_AUTOUNATTEND,format=raw,index=0,if=floppy"
			-boot order=d
		)
	else
		echo "Booting VM..."
		QEMU_ARGS+=(
			-device qemu-xhci
			-device usb-storage,drive=transfer
			-drive "id=transfer,file=$VM_TRANSFER,format=raw,if=none,readonly=on"
		)
	fi

	exec qemu-system-x86_64 "${QEMU_ARGS[@]}"
}

cmd_deploy() {
	./scripts/build-mingw64.sh

	EXE_DIR="build-mingw64/src/.libs"

	echo "Writing files to transfer disk..."
	dd if=/dev/zero of="$VM_TRANSFER" bs=1M count=256 2>/dev/null
	printf 'o\nn\np\n1\n\n\nt\nc\nw\n' | fdisk "$VM_TRANSFER" >/dev/null 2>&1
	mkfs.fat -F 32 --offset 2048 "$VM_TRANSFER" >/dev/null

	MTOOLS_OFFSET="@@1048576"
	for f in "${EXE_DIR}"/*.exe "${EXE_DIR}"/*.dll; do
		mcopy -i "${VM_TRANSFER}${MTOOLS_OFFSET}" "$f" ::/
	done

	echo "Transfer disk contents:"
	mdir -i "${VM_TRANSFER}${MTOOLS_OFFSET}" ::/
	echo "Next: ./scripts/vm-w32.sh run"
}

usage() {
	echo "Usage: $0 {setup|install|run|deploy}" >&2
	exit 1
}

[ $# -ge 1 ] || usage
case "$1" in
setup) cmd_setup ;;
install) launch_vm install ;;
run) launch_vm run ;;
deploy) cmd_deploy ;;
*) usage ;;
esac
