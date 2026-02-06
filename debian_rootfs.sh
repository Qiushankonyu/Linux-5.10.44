#!/bin/bash
set -e

# === Debian rootfs (arm64) build script ===
DEBIAN_RELEASE="bookworm"
DEBIAN_MIRROR="https://mirrors.tuna.tsinghua.edu.cn/debian"
ROOTFS_IMG="debian.ext4"
ROOTFS_SIZE="8G"
MOUNT_POINT="/tmp/debian_rootfs_$(date +%s)"

if ! command -v debootstrap >/dev/null 2>&1; then
  echo "❌ 缺少 debootstrap,请先安装:sudo apt-get install -y debootstrap"
  exit 1
fi

if ! command -v qemu-aarch64-static >/dev/null 2>&1; then
  echo "❌ 缺少 qemu-aarch64-static,请先安装:sudo apt-get install -y qemu-user-static"
  exit 1
fi

if [ -f "$ROOTFS_IMG" ]; then
  echo "⚠️ 发现已有 $ROOTFS_IMG，如需重建请先删除"
  exit 1
fi

qemu-img create -f raw "$ROOTFS_IMG" "$ROOTFS_SIZE"
mkfs.ext4 -F "$ROOTFS_IMG"

mkdir -p "$MOUNT_POINT"
sudo mount -o loop "$ROOTFS_IMG" "$MOUNT_POINT"

sudo debootstrap --arch=arm64 --foreign "$DEBIAN_RELEASE" "$MOUNT_POINT" "$DEBIAN_MIRROR"

sudo cp /usr/bin/qemu-aarch64-static "$MOUNT_POINT/usr/bin/"

sudo chroot "$MOUNT_POINT" /debootstrap/debootstrap --second-stage

# 基础配置
sudo chroot "$MOUNT_POINT" bash -c "echo 'debian' > /etc/hostname"

echo "proc /proc proc defaults 0 0" | sudo tee "$MOUNT_POINT/etc/fstab" >/dev/null

echo "root:root" | sudo chroot "$MOUNT_POINT" chpasswd
sudo chroot "$MOUNT_POINT" useradd -m -s /bin/bash -G sudo konyu

echo "konyu:4" | sudo chroot "$MOUNT_POINT" chpasswd

sudo mkdir -p "$MOUNT_POINT/etc/sudoers.d"
echo "konyu ALL=(ALL) NOPASSWD:ALL" | sudo tee "$MOUNT_POINT/etc/sudoers.d/konyu" >/dev/null
sudo chmod 440 "$MOUNT_POINT/etc/sudoers.d/konyu"

# 安装常用包（含 DRM 工具和 glmark2）
sudo chroot "$MOUNT_POINT" apt-get update
sudo chroot "$MOUNT_POINT" apt-get install -y \
  sudo openssh-server net-tools iproute2 \
  vim less curl ca-certificates \
  mesa-utils glmark2 libdrm-tests

# 启用 tty0 登录（显示在 QEMU 窗口）
sudo chroot "$MOUNT_POINT" systemctl enable getty@tty0

# 允许密码登录
sudo sed -i 's/^#*PasswordAuthentication.*/PasswordAuthentication yes/' "$MOUNT_POINT/etc/ssh/sshd_config"

sudo umount "$MOUNT_POINT"
rm -rf "$MOUNT_POINT"

echo "✅ Debian rootfs 已生成：$ROOTFS_IMG"
