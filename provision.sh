# This file handles installing necessary ubuntu packages when
# using Vagrant.

apt-get update
sudo apt-get -y install build-essential nasm libc6-dev-i386 git gdb
sudo apt-get -y build-dep qemu
# This is Jeff's patched qemu, maintaining fixes for accurate SMP timing.
git clone http://git.dyninst.org/qemu.git
# CONFIGURE includes our necessary options for ./configure
cd qemu && ./CONFIGURE && make -j3 install

