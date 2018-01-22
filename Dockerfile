FROM ubuntu:trusty

COPY sources.list /etc/apt/sources.list
RUN apt-get update
RUN apt-get -y upgrade
RUN apt-get -y install build-essential nasm libc6-dev-i386 git gdb x11-utils dos2unix python ruby
RUN apt-get -y build-dep qemu
# This is Jeff's patched qemu, maintaining fixes for accurate SMP timing.
RUN git clone http://git.dyninst.org/qemu.git patched-qemu
# sdl enables the X display, target-list specifics because we need only i386 emulation.
# enable-sdl may be optional / unnecessary on mac osx.
RUN cd patched-qemu && ./configure --enable-sdl --target-list=i386-softmmu && make -j3 && make install
