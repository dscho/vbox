#!/bin/sh

set -e

cd /vagrant

KBUILD_DIR=kbuild-trunk
KMK=$KBUILD_DIR/kBuild/bin/linux.amd64/kmk
KENV=$KBUILD_DIR/kBuild/env.sh

if test ! -x $KMK
then
	test -f $KBUILD_DIR/kBuild ||
	svn co http://svn.netlabs.org/repos/kbuild/trunk $KBUILD_DIR

	(cd $KBUILD_DIR &&
	 kBuild/env.sh --full make -f bootstrap.gmk)
fi

if test ! -d src/libs/kStuff/kStuff
then
	svn co http://www.virtualbox.org/svn/kstuff-mirror/trunk src/libs/kStuff/kStuff
fi

# VirtualBox' build assumes the revision is decimal; use YYYYMMDD
export VBOX_SVN_REV=$(git show -s --format=%ai |
	cut -c 1-4,6-7,9-10)

test -f env.sh ||
./configure --disable-hardening --with-kbuild=$KBUILD_DIR/kBuild
. ./env.sh
kmk all

(cd out/linux.amd64/release/bin/src &&
 make &&
 printf '' |
 sudo checkinstall --nodoc --pkgname=virtualbox-modules)
