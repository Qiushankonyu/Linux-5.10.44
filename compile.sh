#!/bin/bash
set -e # 遇到错误立即停止

# ================= 配置区 =================
# 内核源码目录名称 (请根据实际情况修改)
KERNEL_DIR_NAME="linux-5.10.44"
# 根文件系统镜像名称
ROOTFS_IMG="rootfs.ext4"
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
    ROOTFS_PATH="$SCRIPT_DIR/$ROOTFS_IMG"
elif [ -f "$SCRIPT_DIR/Kbuild" ] || [ -f "$SCRIPT_DIR/Makefile" ]; then
    # 情况B: 已经在 kernel 目录里 (比如 ~/Codes/linux-5.10.44)
    KDIR="$SCRIPT_DIR"
    ROOTFS_PATH="$SCRIPT_DIR/../$ROOTFS_IMG"
else
    echo "❌ 错误: 找不到内核源码目录，请确认脚本位置！"
    exit 1
fi

echo "📂 Kernel Dir: $KDIR"

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

# ... (在 "3. 编译模块" 之前插入) ...

echo "🔧 正在重新编译设备树..."
# 先清理旧的 DTB
rm -f "$KDIR/qemu_final.dtb"

# 编译新的 DTB
dtc -I dts -O dtb -o "$KDIR/qemu_final.dtb" "$KDIR/qemu_origin.dts"
if [ $? -ne 0 ]; then
    echo "❌ 设备树编译失败!"
    exit 1
fi

echo "✅ 设备树编译成功"
# 验证设备树内容
echo "📋 验证设备树内容:"
dtc -I dtb -O dts "$KDIR/qemu_final.dtb" | grep -A 5 "virtio_gpu_binding"


echo "⚙️  正在清理 virtio_ring 以确保重编..."
# 强制删除目标文件，保证 make 一定会重新编译它
rm -f "$KDIR/drivers/virtio/virtio_ring.o"

echo "🏗️  正在重新编译内核核心 (Image)..."
make -C "$KDIR" ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- Image -j$(nproc)

# ... (后面接原来的 "3. 编译模块") ...

# 3. 编译模块
echo "🔨 正在编译 Virtio-GPU 模块..."
# 强制 touch 确保重新编译
touch "$KDIR/$MODULE_REL_PATH"/*.c
make -C "$KDIR" ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- M=$MODULE_REL_PATH modules -j$(nproc)

# 4. 安装模块到 RootFS
echo "💾 正在安装模块..."
MOUNT_POINT="/tmp/qemu_mnt_$(date +%s)"
mkdir -p $MOUNT_POINT

sudo mount "$ROOTFS_PATH" $MOUNT_POINT

# 创建目标目录 (防止目录不存在报错)
TARGET_DIR="$MOUNT_POINT/lib/modules/$KERNEL_RELEASE/kernel/$MODULE_REL_PATH"
sudo mkdir -p "$TARGET_DIR"

# 拷贝 .ko 文件
sudo cp "$KDIR/$MODULE_REL_PATH/virtio-gpu.ko" "$TARGET_DIR/"
echo "✅ 已拷贝: virtio-gpu.ko -> $TARGET_DIR"

sudo umount $MOUNT_POINT
rm -rf $MOUNT_POINT

# 5. 启动 QEMU
echo "🚀 启动 QEMU..."

KERNEL_IMAGE="$KDIR/arch/arm64/boot/Image"


qemu-system-aarch64 \
    -M virt \
    -cpu cortex-a57 \
    -m 4G \
    -kernel "$KERNEL_IMAGE" \
    -drive file="$ROOTFS_PATH",format=raw,id=hd0,if=none \
    -device virtio-blk-device,drive=hd0 \
    -append "root=/dev/vda rw console=ttyAMA0 earlycon ignore_loglevel" \
    -device virtio-gpu-pci,iommu_platform=on,disable-legacy=on,addr=02.0 \
    -serial stdio \
    -dtb "$KDIR/qemu_final.dtb"

# qemu-system-aarch64 \
#     -M virt \
#     -cpu cortex-a57 \
#     -m 4G \
#     -kernel "$KERNEL_IMAGE" \
#     -drive file="$ROOTFS_PATH",format=raw,id=hd0,if=none \
#     -device virtio-blk-device,drive=hd0 \
#     -append "root=/dev/vda rw console=ttyAMA0 earlycon ignore_loglevel" \
#     -device virtio-gpu-pci,addr=02.0 \
#     -serial stdio \
#     -dtb "$KDIR/qemu_final.dtb"