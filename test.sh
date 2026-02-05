# weston 测试 - 在guest中运行 weston 显示服务器
sudo -i
systemctl enable --now dbus systemd-logind
mkdir -p /run/user/0 && chmod 0700 /run/user/0
XDG_RUNTIME_DIR=/run/user/0 weston --tty=1 --backend=drm-backend.so --log=/root/weston.log
#