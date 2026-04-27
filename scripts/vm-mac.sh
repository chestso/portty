#!/bin/bash
# vm-mac.sh - macOS VM lifecycle (QEMU/KVM + OSX-KVM).
#
# Subcommands:
#   setup    - download recovery image, create disk images, OpenCore + OVMF
#   install  - boot VM from recovery for macOS installation
#   run      - boot the VM normally (transfer disk attached)
#   deploy   - cross-compile and write binary + dylibs to transfer disk

set -eu

cd "$(dirname "$0")/.."

MAC_VM_DIR="vm-macos"
MAC_VM_DISK="$MAC_VM_DIR/macos.qcow2"
MAC_VM_TRANSFER="$MAC_VM_DIR/transfer.img"
MAC_VM_OPENCORE="$MAC_VM_DIR/OpenCore.qcow2"
MAC_VM_BASEIMG="$MAC_VM_DIR/BaseSystem.img"
MAC_VM_OVMF_CODE="$MAC_VM_DIR/OVMF_CODE_4M.fd"
MAC_VM_OVMF_VARS="$MAC_VM_DIR/OVMF_VARS.fd"
MAC_VM_OSX_KVM="$MAC_VM_DIR/OSX-KVM"

cmd_setup() {
	for cmd in qemu-system-x86_64 qemu-img mcopy mkfs.fat; do
		if ! command -v "$cmd" >/dev/null 2>&1; then
			echo "ERROR: $cmd not found" >&2
			echo "Install with: sudo dnf install qemu-system-x86 qemu-img mtools dosfstools" >&2
			exit 1
		fi
	done

	mkdir -p "$MAC_VM_DIR"

	if [ ! -d "$MAC_VM_OSX_KVM" ]; then
		echo "Cloning OSX-KVM..."
		git clone --depth 1 https://github.com/kholia/OSX-KVM.git "$MAC_VM_OSX_KVM"
	fi

	if [ ! -f "$MAC_VM_BASEIMG" ]; then
		echo "Downloading macOS recovery image..."
		osx_kvm_abs=$(cd "$MAC_VM_OSX_KVM" && pwd)
		(cd "$osx_kvm_abs" && python3 fetch-macOS-v2.py --action download)
		dmg=$(find "$osx_kvm_abs" -name "BaseSystem.dmg" -print -quit 2>/dev/null)
		if [ -z "$dmg" ]; then
			echo "ERROR: BaseSystem.dmg not found after download" >&2
			exit 1
		fi
		echo "Converting BaseSystem.dmg to raw image..."
		qemu-img convert "$dmg" -O raw "$MAC_VM_BASEIMG"
	fi

	if [ ! -f "$MAC_VM_OPENCORE" ]; then
		oc_src="${MAC_VM_OSX_KVM}/OpenCore/OpenCore.qcow2"
		if [ ! -f "$oc_src" ]; then
			echo "ERROR: OpenCore.qcow2 not found in OSX-KVM" >&2
			exit 1
		fi
		cp "$oc_src" "$MAC_VM_OPENCORE"
	fi

	if [ ! -f "$MAC_VM_OVMF_CODE" ]; then
		cp "${MAC_VM_OSX_KVM}/OVMF_CODE_4M.fd" "$MAC_VM_OVMF_CODE"
	fi
	if [ ! -f "$MAC_VM_OVMF_VARS" ]; then
		cp "${MAC_VM_OSX_KVM}/OVMF_VARS-1920x1080.fd" "$MAC_VM_OVMF_VARS"
	fi

	if [ ! -f "$MAC_VM_DISK" ]; then
		echo "Creating 64GB disk image..."
		qemu-img create -f qcow2 "$MAC_VM_DISK" 64G
	fi

	if [ ! -f "$MAC_VM_TRANSFER" ]; then
		echo "Creating 256MB transfer disk image..."
		dd if=/dev/zero of="$MAC_VM_TRANSFER" bs=1M count=256 2>/dev/null
		printf 'o\nn\np\n1\n\n\nt\nc\nw\n' | fdisk "$MAC_VM_TRANSFER" >/dev/null 2>&1
		mkfs.fat -F 32 --offset 2048 "$MAC_VM_TRANSFER" >/dev/null
	fi

	echo "VM setup complete. Next: ./scripts/vm-mac.sh install"
}

launch_vm() {
	local mode="$1"

	if [ ! -f "$MAC_VM_DISK" ]; then
		echo "ERROR: VM disk not found. Run: ./scripts/vm-mac.sh setup" >&2
		exit 1
	fi
	if [ ! -f "$MAC_VM_OPENCORE" ] || [ ! -f "$MAC_VM_OVMF_CODE" ]; then
		echo "ERROR: OpenCore or OVMF firmware missing. Run: ./scripts/vm-mac.sh setup" >&2
		exit 1
	fi

	QEMU_ARGS=(
		-enable-kvm
		-cpu "Skylake-Client,-hle,-rtm,kvm=on,vendor=GenuineIntel,+invtsc,vmware-cpuid-freq=on,+ssse3,+sse4.2,+popcnt,+avx,+aes,+xsave,+xsaveopt"
		-m 4G
		-smp 4,cores=2,sockets=1
		-machine q35
		-drive "if=pflash,format=raw,readonly=on,file=${MAC_VM_OVMF_CODE}"
		-drive "if=pflash,format=raw,file=${MAC_VM_OVMF_VARS}"
		-device "isa-applesmc,osk=ourhardworkbythesewordsguardedpleasedontsteal(c)AppleComputerInc"
		-smbios type=2
		-device ich9-ahci,id=sata
		-drive "id=OpenCoreBoot,if=none,snapshot=on,format=qcow2,file=${MAC_VM_OPENCORE}"
		-device ide-hd,bus=sata.2,drive=OpenCoreBoot
		-drive "id=MacHDD,if=none,file=${MAC_VM_DISK},format=qcow2"
		-device ide-hd,bus=sata.4,drive=MacHDD
		-device qemu-xhci,id=xhci
		-device usb-kbd,bus=xhci.0
		-device usb-tablet,bus=xhci.0
		-device vmware-svga
		-display sdl
		-device virtio-net-pci,netdev=net0
		-netdev user,id=net0
		-device ich9-intel-hda
		-device hda-duplex
	)

	if [ "$mode" = "install" ]; then
		if [ ! -f "$MAC_VM_BASEIMG" ]; then
			echo "ERROR: Recovery image not found. Run: ./scripts/vm-mac.sh setup" >&2
			exit 1
		fi
		echo "Booting macOS VM in install mode..."
		echo "Select 'macOS Base System' in OpenCore, then erase the SATA disk in Disk Utility."
		QEMU_ARGS+=(
			-drive "id=InstallMedia,if=none,file=${MAC_VM_BASEIMG},format=raw"
			-device ide-hd,bus=sata.3,drive=InstallMedia
		)
	else
		echo "Booting macOS VM..."
		QEMU_ARGS+=(
			-device usb-storage,drive=transfer,bus=xhci.0
			-drive "id=transfer,if=none,file=${MAC_VM_TRANSFER},format=raw,readonly=on"
		)
	fi

	exec qemu-system-x86_64 "${QEMU_ARGS[@]}"
}

cmd_deploy() {
	./scripts/build-osxcross.sh

	BIN_DIR="build-osxcross/src"

	echo "Writing files to macOS transfer disk..."
	dd if=/dev/zero of="$MAC_VM_TRANSFER" bs=1M count=256 2>/dev/null
	printf 'o\nn\np\n1\n\n\nt\nc\nw\n' | fdisk "$MAC_VM_TRANSFER" >/dev/null 2>&1
	mkfs.fat -F 32 --offset 2048 "$MAC_VM_TRANSFER" >/dev/null

	MTOOLS_OFFSET="@@1048576"

	if [ -f "${BIN_DIR}/bloom-terminal" ]; then
		mcopy -i "${MAC_VM_TRANSFER}${MTOOLS_OFFSET}" "${BIN_DIR}/bloom-terminal" ::/
	elif [ -f "${BIN_DIR}/.libs/bloom-terminal" ]; then
		mcopy -i "${MAC_VM_TRANSFER}${MTOOLS_OFFSET}" "${BIN_DIR}/.libs/bloom-terminal" ::/
	else
		echo "ERROR: bloom-terminal binary not found in ${BIN_DIR}/" >&2
		exit 1
	fi

	dylibs=$(find "${BIN_DIR}" "${BIN_DIR}/.libs" -name "*.dylib" 2>/dev/null || true)
	for f in $dylibs; do
		mcopy -i "${MAC_VM_TRANSFER}${MTOOLS_OFFSET}" "$f" ::/
	done

	if [ -f "data/bloom-terminal.ti" ]; then
		mcopy -i "${MAC_VM_TRANSFER}${MTOOLS_OFFSET}" "data/bloom-terminal.ti" ::/
		install_script=$(mktemp)
		cat >"$install_script" <<'SCRIPT'
#!/bin/bash
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
tic -x "$SCRIPT_DIR/bloom-terminal.ti"
echo "Installed bloom-terminal-256color terminfo entry"
SCRIPT
		mcopy -i "${MAC_VM_TRANSFER}${MTOOLS_OFFSET}" "$install_script" ::/install-terminfo.sh
		rm -f "$install_script"
	fi

	echo "Transfer disk contents:"
	mdir -i "${MAC_VM_TRANSFER}${MTOOLS_OFFSET}" ::/
	echo "Next: ./scripts/vm-mac.sh run"
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
