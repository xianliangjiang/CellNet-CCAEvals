#!/bin/bash
make
sudo rmmod tcp_lola
sudo insmod tcp_lola.ko
echo cubic reno bbr nv vegas lola | sudo dd of=/proc/sys/net/ipv4/tcp_allowed_congestion_control
sudo sysctl -p ../../AutoExp/setup/tcp-tune/tcp-tune10g.sys
#end 
