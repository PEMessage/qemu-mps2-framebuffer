# Ubuntu requirement


```bash
# TODO:
# Thnask: https://askubuntu.com/questions/1347421/enable-gtk-for-qemu-configure-gives-me-install-gtk3-devel-message-on-ubu
# libavahi-ui-gtk3-dev may not be exact one we need
# This worked by accident via further dependencies, you'd really need/want libgtk-3-dev (and probably for --enable-vte also libvte-2.91-dev)
sudo apt install \
    python3-venv \
    python3-sphinx \
    python3-sphinx-rtd-theme \
    ninja-build \
    pkgconf \
    libavahi-ui-gtk3-dev \
    libgtk-3-dev
```
