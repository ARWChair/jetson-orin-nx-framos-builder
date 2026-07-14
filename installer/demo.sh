sudo apt update
sudo apt install -y build-essential bc flex bison libssl-dev libelf-dev \
  python3 python3-dev libncurses-dev wget git

cd ~
mkdir jetson-build && cd jetson-build

# Kernel sources für L4T R36.4.4
git clone --depth=1 -b jetson_36.4.4 \
  https://github.com/nvidia-mirror/linux-nv-oot.git

# Tegra kernel sources
git clone --depth=1 -b tegra-l4t-r36.4.4 \
  https://github.com/nvidia-mirror/linux-tegra-5.15.git kernel

cd ~/jetson-build
git clone -b l4t-r36.4.4 \
  https://github.com/framosimaging/framos-jetson-drivers.git

cd ~/jetson-build
rm -rf linux-nv-oot kernel

cd ~/jetson-build

# Kernel Sources direkt von Nvidia (ca. 150-200MB)
wget https://developer.nvidia.com/downloads/embedded/l4t/r36_release_v4.4/sources/public_sources.tbz2

# Entpacken
tar -xjf public_sources.tbz2

# Kernel-Source aus dem Paket extrahieren
cd ~/jetson-build/Linux_for_Tegra/source
tar -xjf kernel_src.tbz2
tar -xjf nvidia-oot/nvidia_kernel_oot.tbz2 2>/dev/null || true

ls -la

cd ~/jetson-build/Linux_for_Tegra/source
tar -xjf kernel_oot_modules_src.tbz2
ls kernel/

cd ~/jetson-build/Linux_for_Tegra/source/kernel/kernel-jammy-src

# Aktuelle laufende Kernel-Config als Basis nehmen
# (das stellt sicher dass ALLES was jetzt funktioniert, auch nachher funktioniert)
zcat /proc/config.gz > .config

# Config anpassen für unsere Kernel-Version
make ARCH=arm64 olddefconfig

cd ~/jetson-build/Linux_for_Tegra/source/kernel/kernel-jammy-src

# Wie viele CPU-Kerne hat dein Orin NX?
nproc

make ARCH=arm64 -j$(nproc) Image modules

cd ~/jetson-build/Linux_for_Tegra/source/kernel/kernel-jammy-src

# Module in temporäres Verzeichnis installieren (noch NICHT ins System)
mkdir -p ~/jetson-build/modules-staging
make ARCH=arm64 -j$(nproc) modules_install INSTALL_MOD_PATH=~/jetson-build/modules-staging

ls ~/jetson-build/modules-staging/lib/modules/

cd ~/jetson-build/Linux_for_Tegra/source/kernel/kernel-jammy-src

# Aktuelle LOCALVERSION in der Config prüfen
grep LOCALVERSION .config

cat include/config/kernel.release 2>/dev/null || echo "nicht vorhanden"

cd ~/jetson-build/Linux_for_Tegra/source/kernel/kernel-jammy-src

# LOCALVERSION auf -tegra setzen
sed -i 's/CONFIG_LOCALVERSION=""/CONFIG_LOCALVERSION="-tegra"/' .config

# Prüfen
grep LOCALVERSION .config

# Altes staging löschen
rm -rf ~/jetson-build/modules-staging

mkdir -p ~/jetson-build/modules-staging
make ARCH=arm64 -j$(nproc) modules_install INSTALL_MOD_PATH=~/jetson-build/modules-staging

# Prüfen ob der Name jetzt stimmt
ls ~/jetson-build/modules-staging/lib/modules/

cd ~/jetson-build/Linux_for_Tegra/source/kernel/kernel-jammy-src

# EXTRAVERSION in der Makefile setzen
sed -i 's/^EXTRAVERSION =$/EXTRAVERSION = -tegra/' Makefile

# Prüfen
grep EXTRAVERSION Makefile | head -2

# utsrelease neu bauen
make ARCH=arm64 -j$(nproc) prepare

# Prüfen ob es jetzt stimmt
cat include/generated/utsrelease.h
cat include/config/kernel.release

rm -rf ~/jetson-build/modules-staging
mkdir -p ~/jetson-build/modules-staging
make ARCH=arm64 -j$(nproc) modules_install INSTALL_MOD_PATH=~/jetson-build/modules-staging
ls ~/jetson-build/modules-staging/lib/modules/

cd ~/jetson-build/Linux_for_Tegra/source/kernel/kernel-jammy-src

# CONFIG_LOCALVERSION wieder leeren
sed -i 's/CONFIG_LOCALVERSION="-tegra"/CONFIG_LOCALVERSION=""/' .config

# Prüfen
grep LOCALVERSION .config

# Und nochmal prepare
make ARCH=arm64 -j$(nproc) prepare

cat include/generated/utsrelease.h
cat include/config/kernel.release

rm -rf ~/jetson-build/modules-staging
mkdir -p ~/jetson-build/modules-staging
make ARCH=arm64 -j$(nproc) modules_install INSTALL_MOD_PATH=~/jetson-build/modules-staging
ls ~/jetson-build/modules-staging/lib/modules/

sudo cp /boot/Image /boot/Image.backup

cd ~/jetson-build/Linux_for_Tegra/source/kernel/kernel-jammy-src

# Neues Kernel Image installieren
sudo cp arch/arm64/boot/Image /boot/Image

# Module ins System installieren
sudo make ARCH=arm64 modules_install

ls /lib/modules/

# Backup extlinux (nochmal, jetzt nach Kernel-Install)
sudo cp /boot/extlinux/extlinux.conf /boot/extlinux/extlinux.conf.BEFORE_FRAMOS

# Aktuellen Inhalt anzeigen
cat /boot/extlinux/extlinux.conf

cd ~/jetson-build
git clone -b l4t-r36.4.4 https://github.com/framosimaging/framos-jetson-drivers.git

ls framos-jetson-drivers/

# Was ist im build-Verzeichnis?
ls ~/jetson-build/framos-jetson-drivers/build/

# Das Haupt-Installscript anschauen
cat ~/jetson-build/framos-jetson-drivers/build/install_framos_jetson_drivers.sh 2>/dev/null || \
ls ~/jetson-build/framos-jetson-drivers/build/

# Gibt es Scripts die extlinux anfassen?
grep -r "extlinux" ~/jetson-build/framos-jetson-drivers/ --include="*.sh" -l
grep -r "extlinux" ~/jetson-build/framos-jetson-drivers/ --include="*.sh"

cat ~/jetson-build/framos-jetson-drivers/build/target_build.sh

# Python scripts finden
find ~/jetson-build/framos-jetson-drivers/ -name "*.py" | head -20

# Gibt es ein tools-Verzeichnis?
ls ~/jetson-build/framos-jetson-drivers/tools/

cat ~/jetson-build/framos-jetson-drivers/build/target_build.sh

grep -n "extlinux" ~/jetson-build/framos-jetson-drivers/tools/jetson-config-camera-cli.py | head -30
grep -n "extlinux" ~/jetson-build/framos-jetson-drivers/tools/jetson-config-camera.py | head -30

sed -n '90,140p' ~/jetson-build/framos-jetson-drivers/tools/jetson-config-camera-cli.py
sed -n '550,620p' ~/jetson-build/framos-jetson-drivers/tools/jetson-config-camera.py

cd ~/jetson-build/framos-jetson-drivers/build

# Script sourcen (nicht ausführen!)
source target_build.sh

# Erst nur bauen - noch nichts installieren
build_all

cd ~/jetson-build/framos-jetson-drivers/build

install_all

ls /boot/framos/dtbo/ | grep imx900

ls /boot/framos/dtbo/ | grep p3767

# Welches base-DTB liegt in /boot/dtb/?
ls /boot/dtb/

