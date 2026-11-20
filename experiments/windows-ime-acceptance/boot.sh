#!/usr/bin/env bash
set -euo pipefail
case_dir=$1
media="$case_dir/boot-media"
mkdir -p "$media" "$case_dir/evidence"
curl --fail --location --retry 2 --max-time 600 --silent --show-error \
  https://software-static.download.prss.microsoft.com/dbazure/888969d5-f34g-4e03-ac9d-1f9786c66749/26100.1742.240906-0331.ge_release_svc_refresh_CLIENT_LTSC_EVAL_x64FRE_en-us.iso \
  --output "$case_dir/windows.iso"
test "$(stat -c %s "$case_dir/windows.iso")" = 5112850432
(cd "$case_dir" && echo '67cec5865eaa037a72ddc633a717a10a2bed50778862267223ddb9c60ef5da68  windows.iso' | sha256sum --check)
(cd "$case_dir" && sha256sum windows.iso) > "$case_dir/evidence/microsoft-iso.sha256"
cp experiments/windows-ime-acceptance/guest.ps1 "$media/"
python3 experiments/windows-ime-acceptance/make_unattend.py "$media/Autounattend.xml"
truncate -s 8M "$case_dir/answers.img"
mkfs.vfat -n XDNOTE_ANS "$case_dir/answers.img" >/dev/null
mcopy -i "$case_dir/answers.img" "$media/"* ::/
qemu-img create -f qcow2 "$case_dir/windows.qcow2" 72G
cp /usr/share/OVMF/OVMF_VARS_4M.ms.fd "$case_dir/uefi-vars.fd"
mkdir "$case_dir/tpm"
swtpm socket --tpm2 --tpmstate dir="$case_dir/tpm" \
  --ctrl "type=unixio,path=$case_dir/tpm.sock" --pid "file=$case_dir/tpm.pid" --daemon
touch "$case_dir/serial.log"
sudo qemu-system-x86_64 -daemonize -machine q35,accel=kvm,smm=on -cpu host -smp 4 -m 6144 \
  -global driver=cfi.pflash01,property=secure,value=on \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.secboot.fd \
  -drive "if=pflash,format=raw,file=$case_dir/uefi-vars.fd" \
  -chardev "socket,id=tpm,path=$case_dir/tpm.sock" -tpmdev emulator,id=emulated-tpm,chardev=tpm \
  -device tpm-tis,tpmdev=emulated-tpm \
  -vga std -display none -nic none -boot order=c,once=d \
  -drive "file=$case_dir/windows.qcow2,format=qcow2,if=ide,index=0" \
  -drive "file=$case_dir/windows.iso,format=raw,media=cdrom,if=ide,index=1" \
  -drive "if=ide,index=2,media=cdrom,id=transfer" \
  -device qemu-xhci -device usb-tablet -drive "if=none,format=raw,file=$case_dir/answers.img,id=answers" \
  -device usb-storage,drive=answers,removable=on \
  -serial "file:$case_dir/serial.log" -monitor "unix:$case_dir/monitor,server=on,wait=off" \
  -qmp "unix:$case_dir/qmp,server=on,wait=off" \
  -pidfile "$case_dir/qemu.pid"
# The official UEFI installer prompts for a key on its first optical boot.
for pause in 5 2 2 2 2; do
  sleep "$pause"
  sudo python3 experiments/windows-ime-acceptance/collect.py "$case_dir" boot
done
