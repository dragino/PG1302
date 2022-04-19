#!/bin/bash

cd /tmp;
#Download the package of draginofwd
	if [ -f "/tmp/dragino_fwd_for_pi.tar.bz2" ]; then
		if [ ! -f "/tmp/dragino_fwd_for_pi" ]; then
			sudo tar -xjf /tmp/dragino_fwd_for_pi.tar.bz2
		fi
	else
		sudo wget  -q https://www.dragino.com/downloads/downloads/LoRa_Gateway/PG1302/software/dragino_fwd_for_pi.tar.bz2
		if [ "$?" != "0" ];then
			echo "---------------------------------------------"
			echo "Download failure, Please try again" && exit 0
		else
			echo "---------------------------------------------"
			echo "Download done"
		sudo tar -xjf /tmp/dragino_fwd_for_pi.tar.bz2
		fi
	fi
#Install sqlite3

	if [ -f /usr/local/bin/sqlite3 ]; then
		echo "---------------------------------------------"
		echo "Sqlite3 had been installed"
	else
		echo "---------------------------------------------"
		echo "Start to install sqlite3"
		cd /tmp/dragino_fwd_for_pi/sqlite-autoconf-3360000/;
		./configure &> /dev/null;
		sudo make &> /dev/null &&  sudo make install &> /dev/null

		if [ "$?" = "0" ]; then
			echo "---------------------------------------------"
			echo "Sqlite3 install done";
		else
			sudo make clean &> /dev/null
			echo "---------------------------------------------"
			echo "Install failure, Please try again" && exit 0
		fi
	fi

#Install ftdi
	if [ -f /usr/include/ftdi.h ]; then
		echo "---------------------------------------------"
		echo "Ftdi had been installed"
	else
		echo "---------------------------------------------"
		echo "Start install ftdi, installation speed depends on network status "
		sudo apt-get update &> /dev/null;
		sudo apt-get -y --force-yes install libftdi* &> /dev/null
		if [ "$?" = "0" ]; then
			echo "---------------------------------------------"
			echo "Intall ftdi done";
		else
			echo "---------------------------------------------"
			echo "Install failure, Please try again" && exit 0
		fi
	fi
#Install libmpsse
	if [ -f /usr/local/lib/libmpsse.so ]; then
		echo "---------------------------------------------"
		echo "Libmpsse had been installed"
	else
		echo "---------------------------------------------"
		echo "Start install libmpess."
		cd /tmp/dragino_fwd_for_pi/libmpsse/src/;
		sudo make -s clean && sudo make &> /dev/null ;
		sudo make install &> /dev/null
		if [ "$?" = "0" ]; then
			echo "---------------------------------------------"
			echo "Install libmpess done"
		else
			sudo make clean &> /dev/null
			echo "---------------------------------------------"
			echo "Install failure, Please try again" && exit 0
		fi
	fi
#Install fwd
	if [ -f /usr/bin/fwd ]; then
		echo "---------------------------------------------"
		echo "FWD had been installed"
	else
		echo "---------------------------------------------"
		echo "Start install Dragino FWD."
		cd /tmp/dragino_fwd_for_pi/;
		sudo make &> /dev/null

		if [ "$?" = "0" ]; then
			sudo make install &> /dev/null
			if [ "$?" = "0" ]; then
				echo "Install FWD done"
			else
			echo "---------------------------------------------"
			echo "Install failure, Please try again" && exit 0
			fi
		else
			sudo make clean &> /dev/null
			echo "---------------------------------------------"
			echo "Install failure, Please try again" && exit 0
		fi
	fi
