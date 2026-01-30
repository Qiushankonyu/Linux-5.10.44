make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- M=drivers/gpu/drm/virtio modules -j$(nproc)

cd ..

sudo mount rootfs.ext4 mnt

sudo cp linux-5.10.44/drivers/gpu/drm/virtio/virtio-gpu.ko mnt/lib/modules/5.10.44-vpu+/kernel/drivers/gpu/drm/virtio/

sudo umount mnt

qemu-system-aarch64     -M virt     -cpu cortex-a57     -m 4G     -kernel Image_standard     -drive file=rootfs.ext4,format=raw,id=hd0,if=none     -device virtio-blk-device,drive=hd0     -append "root=/dev/vda rw console=ttyAMA0 earlycon"     -device virtio-gpu-pci     -serial stdio     -display default,show-cursor=on     -dtb linux-5.10.44/qemu_final.dtb
