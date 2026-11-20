#!/usr/bin/env bash
set -euo pipefail
case_dir=$1
media="$case_dir/boot-media"
mkdir -p "$media" "$case_dir/evidence"
curl --fail --location --retry 2 --max-time 600 --silent --show-error \
  https://software-download.microsoft.com/download/sg/17763.107.101029-1455.rs5_release_svc_refresh_CLIENT_LTSC_EVAL_x64FRE_en-us.iso \
  --output "$case_dir/windows.iso"
test "$(stat -c %s "$case_dir/windows.iso")" = 4056193024
(cd "$case_dir" && printf "%s  windows.iso\n" 668fe1af70c2f7416328aee3a0bb066b12dc6bbd2576f40f812b95741e18bc3a | sha256sum -c && sha256sum windows.iso) > "$case_dir/evidence/microsoft-iso.sha256"
cp experiments/windows-client-candidate/guest.ps1 "$media/"
python3 experiments/windows-client-candidate/unattend-1809.py "$media/Autounattend.xml"
truncate -s 8M "$case_dir/answers.img"
mkfs.vfat -n XDNOTE_ANS "$case_dir/answers.img" >/dev/null
mcopy -i "$case_dir/answers.img" "$media/"* ::/
qemu-img create -f qcow2 "$case_dir/windows.qcow2" 46G
touch "$case_dir/serial.log"
sudo qemu-system-x86_64 -daemonize -machine pc,accel=kvm -cpu host -smp 4 -m 6144 \
  -vga std -display none -nic none -boot order=c,once=d \
  -drive "file=$case_dir/windows.qcow2,format=qcow2,if=ide,index=0" \
  -drive "file=$case_dir/windows.iso,format=raw,media=cdrom,if=ide,index=2" \
  -drive "if=ide,index=3,media=cdrom,id=transfer" \
  -device qemu-xhci -drive "if=none,format=raw,file=$case_dir/answers.img,id=answers" \
  -device usb-storage,drive=answers,removable=on \
  -serial "file:$case_dir/serial.log" -monitor "unix:$case_dir/monitor,server=on,wait=off" \
  -pidfile "$case_dir/qemu.pid"
