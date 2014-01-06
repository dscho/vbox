# -*- mode: ruby -*-
# vi: set ft=ruby :

# Vagrantfile API/syntax version. Don't touch unless you know what you're doing!
VAGRANTFILE_API_VERSION = "2"

$provision = <<PROVISION
# From https://www.virtualbox.org/wiki/Linux%20build%20instructions

apt-get update
apt-get install -y gcc g++ bcc iasl xsltproc uuid-dev zlib1g-dev libidl-dev \
                libsdl1.2-dev libxcursor-dev libasound2-dev libstdc++5 \
                libhal-dev libpulse-dev libxml2-dev libxslt1-dev \
                python-dev libqt4-dev qt4-dev-tools libcap-dev \
                libxmu-dev mesa-common-dev libglu1-mesa-dev \
                linux-kernel-headers libcurl4-openssl-dev libpam0g-dev \
                libxrandr-dev libxinerama-dev libqt4-opengl-dev makeself \
                libdevmapper-dev default-jdk python-central \
                texlive-latex-base \
                texlive-latex-extra texlive-latex-recommended \
                texlive-fonts-extra texlive-fonts-recommended

# for 64-bit:
apt-get install -y ia32-libs libc6-dev-i386 lib32gcc1 gcc-multilib \
                lib32stdc++6 g++-multilib
if test ! -h /usr/lib32/libX11.so
then
	ln -s libX11.so.6    /usr/lib32/libX11.so
	ln -s libXTrap.so.6  /usr/lib32/libXTrap.so
	ln -s libXt.so.6     /usr/lib32/libXt.so
	ln -s libXtst.so.6   /usr/lib32/libXtst.so
	ln -s libXmu.so.6    /usr/lib32/libXmu.so
	ln -s libXext.so.6   /usr/lib32/libXext.so
fi

# Requirements forgotten on the website
apt-get install -y mkisofs libvpx-dev make autoconf automake autopoint bison flex subversion liblzma-dev

# Kernel sources
apt-get source linux-image-$(uname -r)
test -h /usr/src/linux ||
ln -s /usr/src/linux-headers-*-generic /usr/src/linux

# For convenience: checkinstall
apt-get install -y checkinstall

# Mention it upon startup
sed -i '/^cd .vagrant/q' /home/vagrant/.profile
cat >> /home/vagrant/.profile << \EOF

cd /vagrant
printf '\\n%s\\n\\n%s\\n\\n\\t%s\\n' \
	'Congratulations! VirtualBox sources are set up in /vagrant/.' \
	'Build VirtualBox using' \
	'./build-vbox.sh'
EOF

set -x
: All set up! Now connect with 'vagrant ssh'!
PROVISION

Vagrant.configure(VAGRANTFILE_API_VERSION) do |config|
  config.vm.box = "ubuntu"
  config.vm.box_url = "http://files.vagrantup.com/precise64.box"

  config.vm.provision :shell, :inline => $provision
end
