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

echo "🧩 正在更新内核配置 (olddefconfig)..."
make -C "$KDIR" ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- olddefconfig

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

echo "🔨 正在编译 virtio_dma_buf 模块..."
make -C "$KDIR" ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- M=drivers/virtio modules -j$(nproc)

# 4. 安装模块到 RootFS
echo "💾 正在安装模块..."
MOUNT_POINT="/tmp/qemu_mnt_$(date +%s)"
mkdir -p $MOUNT_POINT

sudo mount "$ROOTFS_PATH" $MOUNT_POINT

# 创建目标目录 (防止目录不存在报错)
TARGET_DIR="$MOUNT_POINT/lib/modules/$KERNEL_RELEASE/kernel/$MODULE_REL_PATH"
sudo mkdir -p "$TARGET_DIR"
sudo mkdir -p "$MOUNT_POINT/lib/modules/$KERNEL_RELEASE/kernel/drivers/virtio"

# 拷贝 .ko 文件
sudo cp "$KDIR/$MODULE_REL_PATH/virtio-gpu.ko" "$TARGET_DIR/"
sudo cp "$KDIR/drivers/virtio/virtio_dma_buf.ko" "$MOUNT_POINT/lib/modules/$KERNEL_RELEASE/kernel/drivers/virtio/"
echo "✅ 已拷贝: virtio-gpu.ko -> $TARGET_DIR"

# 生成模块依赖，确保 modprobe 可用
sudo depmod -a -b "$MOUNT_POINT" "$KERNEL_RELEASE"

# 开机自动加载模块
echo -e "virtio_dma_buf\nvirtio-gpu" | sudo tee "$MOUNT_POINT/etc/modules-load.d/virtio-gpu.conf" >/dev/null
echo -e "virtio_dma_buf\nvirtio-gpu" | sudo tee -a "$MOUNT_POINT/etc/modules" >/dev/null

sudo umount $MOUNT_POINT
rm -rf $MOUNT_POINT

echo "✅ 编译与安装完成，启动请运行 start.sh"
