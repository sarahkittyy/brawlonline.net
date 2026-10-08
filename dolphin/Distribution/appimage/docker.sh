#!/bin/bash -ex

branch=`echo ${GITHUB_REF##*/}`

QT_BASE_DIR=/opt/qt514
export QTDIR=$QT_BASE_DIR
export PATH=$QT_BASE_DIR/bin:$PATH
export LD_LIBRARY_PATH=$QT_BASE_DIR/lib/x86_64-linux-gnu:$QT_BASE_DIR/lib:$LD_LIBRARY_PATH
export PKG_CONFIG_PATH=$QT_BASE_DIR/lib/pkgconfig:$PKG_CONFIG_PATH
export GCCVER=10
export GCC_BINARY=gcc-${GCCVER}
export GXX_BINARY=g++-${GCCVER}
export CC=$GCC_BINARY
export CXX=$GXX_BINARY

add-apt-repository -y ppa:ubuntu-toolchain-r/test
apt install -y $GCC_BINARY $GXX_BINARY

cd /dolphin

git clone https://github.com/dolphin-emu/dolphin.git
cd dolphin/
git submodule update --init --recursive

ninja -C /tmp/zstd/build/cmake/build uninstall

### GET BUILD Number
export LASTCOMMIT=$(git log --pretty=format:%H -1)
export DOLPHINVER=$(wget -qO- https://dolphin-emu.org/download/dev/${LASTCOMMIT} | grep '<title>' | awk '{print $NF}' | cut -d '<' -f 1)
echo "DOLPHIN Build $DOLPHINVER"
###

mkdir build
cd build
cmake .. -G Ninja -DLINUX_LOCAL_DEV=true -DCMAKE_C_COMPILER=/usr/lib/ccache/$GCC_BINARY -DCMAKE_CXX_COMPILER=/usr/lib/ccache/$GXX_BINARY -DENABLE_TESTS=OFF
ninja
#ln -s ../../Data/Sys Binaries/

#cat /dolphin/build/CMakeFiles/CMakeError.log | curl -F 'f:1=<-' ix.io

cd /tmp
curl -sLO "https://raw.githubusercontent.com/Project-Plus-Development-Team/Project-Plus-Dolphin/$branch/Distribution/appimage/appimage.sh"
chmod a+x appimage.sh
./appimage.sh
#ls -al /dolphin
#ls -al /dolphin/build
#ls -al /dolphin/build/Binaries
