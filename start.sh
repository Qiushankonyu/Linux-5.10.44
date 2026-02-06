#!/bin/bash
set -e # 遇到错误立即停止

# ================= 配置区 =================
# 内核源码目录名称 (请根据实际情况修改)
KERNEL_DIR_NAME="linux-5.10.44"
# 根文件系统镜像名称（Debian）
ROOTFS_IMG="debian.ext4"
# 目标模块路径 (内核内部路径)
MODULE_REL_PATH="drivers/gpu/drm/virtio"
# 当前运行内核的版本号 (用于安装路径)
KERNEL_RELEASE="5.10.44-vpu+"
# ==========================================

# 1. 智能定位目录
# 获取当前脚本所在目录
SCRIPT_DIR=$(pwd)

# 判断脚本是在 parent 目录还是在 kernel 目录
if [ -d "$SCRIPT_DIR/$KERNEL_DIR_NAME" ]; then
    # 情况A: 在 parent 目录 (比如 ~/Codes)
    KDIR="$SCRIPT_DIR/$KERNEL_DIR_NAME"
elif [ -f "$SCRIPT_DIR/Kbuild" ] || [ -f "$SCRIPT_DIR/Makefile" ]; then
    # 情况B: 已经在 kernel 目录里 (比如 ~/Codes/linux-5.10.44)
    KDIR="$SCRIPT_DIR"
else
    echo "❌ 错误: 找不到内核源码目录，请确认脚本位置！"
    exit 1
fi

ROOTFS_PATH="$KDIR/$ROOTFS_IMG"

echo "📂 Kernel Dir: $KDIR"

if [ ! -f "$ROOTFS_PATH" ]; then
    echo "❌ 找不到 rootfs: $ROOTFS_PATH"
    echo "   请先运行: $KDIR/debian_rootfs.sh"
    exit 1
fi

# 2. 检查内核是否已配置
if [ ! -f "$KDIR/.config" ]; then
    echo "⚠️ .config 缺失！正在生成默认配置..."
    make -C "$KDIR" ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- defconfig
fi

# 检查是否需要 prepare
if [ ! -f "$KDIR/include/generated/autoconf.h" ]; then
    echo "⚙️ 正在准备编译环境 (modules_prepare)..."
    make -C "$KDIR" ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- modules_prepare
fi

KERNEL_IMAGE="$KDIR/arch/arm64/boot/Image"

# USE_GL=1 启用 virgl；默认 0 使用普通 virtio-gpu
USE_GL="${USE_GL:-0}"
if [ "$USE_GL" = "1" ]; then
    GPU_DEVICE="virtio-gpu-gl-pci,iommu_platform=on,disable-legacy=on,addr=02.0,edid=on"
    DISPLAY_ARGS=( -display gtk,show-cursor=on,gl=on )
else
    GPU_DEVICE="virtio-gpu-pci,iommu_platform=on,disable-legacy=on,addr=02.0,edid=on"
    DISPLAY_ARGS=( -display gtk,show-cursor=on )
fi

qemu-system-aarch64 \
    -M virt \
    -cpu cortex-a57 \
    -m 4G \
    -vga none \
    -kernel "$KERNEL_IMAGE" \
    -drive file="$ROOTFS_PATH",format=raw,id=hd0,if=none \
    -device virtio-blk-device,drive=hd0 \
    -append "root=/dev/vda rootfstype=ext4 rootwait rw console=ttyAMA0 console=tty0 earlycon ignore_loglevel modules-load=virtio_dma_buf,virtio_gpu,virtio_net ip=dhcp video=Virtual-1:800x600@60" \
    -device "$GPU_DEVICE" \
    -device qemu-xhci \
    -device usb-kbd \
    -device usb-mouse \
    "${DISPLAY_ARGS[@]}" \
    -netdev user,id=net0 \
    -device virtio-net-device,netdev=net0 \
    -serial stdio \
    -dtb "$KDIR/qemu_final.dtb"