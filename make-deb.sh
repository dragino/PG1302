#!/bin/bash

VER=$(date -u '+%Y-%m-%d')

echo "make deb version: draginofwd-${VER}"

rm -rf pi_pkg
rm -rf draginofwd-{VER}.deb
sed -i "s/^.*Version:.*/Version: ${VER}/" DEBIAN/control
mkdir -p pi_pkg/lib/systemd/system
mkdir -p pi_pkg/etc/lora
cp -f config/global_conf.json pi_pkg/etc/lora
cp -f config/local_conf.json pi_pkg/etc/lora
cp -rf DEBIAN pi_pkg/
cp -rf draginofwd.service pi_pkg/lib/systemd/system
install -d pi_pkg/usr/lib
install -m 755 libmqtt/libpahomqtt3c.so pi_pkg/usr/lib/libpahomqtt3c.so
install -m 755 sx1302hal/libsx1302hal.so pi_pkg/usr/lib/libsx1302hal.so
install -d pi_pkg/usr/bin
install -m 755 sx1302hal/test_loragw_hal_rx pi_pkg/usr/bin/rx_test
install -m 755 sx1302hal/test_loragw_hal_tx pi_pkg/usr/bin/tx_test
install -m 755 sx1302hal/test_loragw_toa pi_pkg/usr/bin/toa_test
install -m 755 sx1302hal/test_loragw_com pi_pkg/usr/bin/com_test
install -m 755 sx1302hal/test_loragw_com_sx1250 pi_pkg/usr/bin/sx1250_test
install -m 755 build_fwd_sx1302/fwd_sx1302 pi_pkg/usr/bin
install -m 755 build_station_sx1302/build-mips-openwrt-dragino/bin/station_sx1302 pi_pkg/usr/bin
install -m 755 tools/reset_lgw.sh pi_pkg/usr/bin
ln -sf /usr/bin/fwd_sx1302 pi_pkg/usr/bin/fwd
dpkg-deb -Zgzip -b pi_pkg draginofwd-${VER}.deb
rm -rf pi_pkg

