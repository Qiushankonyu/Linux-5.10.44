#!/bin/bash
set -e

# Config
KERNEL_DIR_NAME="linux-5.10.44"
ROOTFS_IMG="debian.ext4"
KERNEL_RELEASE="5.10.44-vpu+"

# 1) Resolve paths
SCRIPT_DIR=$(pwd)
if [ -d "$SCRIPT_DIR/$KERNEL_DIR_NAME" ]; then
    KDIR="$SCRIPT_DIR/$KERNEL_DIR_NAME"
elif [ -f "$SCRIPT_DIR/Kbuild" ] || [ -f "$SCRIPT_DIR/Makefile" ]; then
    KDIR="$SCRIPT_DIR"
else
    echo "ERROR: kernel source directory not found"
    exit 1
fi

ROOTFS_PATH="$KDIR/$ROOTFS_IMG"
KERNEL_IMAGE="$KDIR/arch/arm64/boot/Image"

echo "Kernel Dir: $KDIR"

if [ ! -f "$ROOTFS_PATH" ]; then
    echo "ERROR: rootfs not found: $ROOTFS_PATH"
    echo "Run: $KDIR/debian_rootfs.sh"
    exit 1
fi

if [ ! -f "$KDIR/.config" ]; then
    echo "INFO: .config missing, generating defconfig..."
    make -C "$KDIR" ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- defconfig
fi

if [ ! -f "$KDIR/include/generated/autoconf.h" ]; then
    echo "INFO: running modules_prepare..."
    make -C "$KDIR" ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- modules_prepare
fi

# USE_GL=1 enables virgl (MMIO variant)
USE_GL="${USE_GL:-0}"
if [ "$USE_GL" = "1" ]; then
    GPU_DEVICE="virtio-gpu-gl-device,iommu_platform=on,edid=on,max_outputs=2,xres=1024,yres=768"
    DISPLAY_ARGS=( -display gtk,show-cursor=on,gl=on )
else
    GPU_DEVICE="virtio-gpu-device,iommu_platform=on,edid=on,max_outputs=2,xres=1024,yres=768"
    DISPLAY_ARGS=( -display gtk,show-cursor=on )
fi

echo "Launching with MMIO GPU: $GPU_DEVICE"

qemu-system-aarch64 \
    -M virt \
    -global virtio-mmio.force-legacy=false \
    -cpu cortex-a57 \
    -m 256M \
    -vga none \
    -kernel "$KERNEL_IMAGE" \
    -drive file="$ROOTFS_PATH",format=raw,id=hd0,if=none \
    -device virtio-blk-device,drive=hd0 \
    -append "root=/dev/vda rootfstype=ext4 rootwait rw console=ttyAMA0 earlycon ignore_loglevel modules-load=virtio_dma_buf,virtio_gpu,virtio_net ip=dhcp video=Virtual-1:1024x768@60e video=Virtual-2:1024x768@60e" \
    -device "$GPU_DEVICE" \
    -device qemu-xhci \
    -device usb-kbd \
    -device usb-mouse \
    "${DISPLAY_ARGS[@]}" \
    -netdev user,id=net0 \
    -device virtio-net-device,netdev=net0 \
    -serial stdio \
    -dtb "$KDIR/qemu_final.dtb"
