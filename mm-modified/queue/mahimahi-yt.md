<h2 align="center">YouTube Analysis Reproduction Instructions</h2>
Our Youtube analysis requires a forked version of Chromium browser which logs render calls, a youtube video download script, an SSIM analysis tool, and a forked version of the mahimahi network emulation toolkit which supports youtube playback in mm-webrecord.
Tested on Ubuntu 15.04 and 15.10

#####Dependencies
<pre>
sudo apt-get install python autoconf g++-4.9 unzip python-matplotlib gcc pkg-config zlib1g-dev debhelper autotools-dev dh-autoreconf iptables protobuf-compiler libprotobuf-dev pkg-config libssl-dev dnsmasq-base ssl-cert libxcb-present-dev libcairo2-dev libpango1.0-dev iproute2 apache2-dev apache2-bin gcc-arm-linux-gnueabihf g++-4.9-multilib-arm-linux-gnueabihf gcc-4.9-multilib-arm-linux-gnueabihf git g++-5-multilib-arm-linux-gnueabihf
</pre>

#####youtube-dl
<pre>
sudo wget https://yt-dl.org/downloads/2015.08.09/youtube-dl -O /usr/local/bin/youtube-dl
sudo chmod a+rx /usr/local/bin/youtube-dl
</pre>

#####SSIM
//TODO: fix warnings?
//https://github.com/alfalfa/SSIM_analysis
<pre>
cd SSIM_analysis
autoreconf -fi
sudo apt-get install libavutil-dev libavformat-dev libswscale-dev libx264-dev
./configure
make -j
cd ..
</pre>

#####mahimahi-fork
<pre>
git clone https://github.com/alfalfa/mahimahi_youtube_record_replay_fork.git
cd mahimahi_youtube_record_replay_fork
./autogen.sh
./configure
make -j
sudo make install
cd ..
</pre>

#####Forked Chromium setup
<pre>
mkdir Chromium_Fork
cd Chromium_Fork
git clone https://chromium.googlesource.com/chromium/tools/depot_tools.git
cd depot_tools
export PATH=$PATH:`pwd`
cd ..
fetch chromium
./src/build/install-build-deps.sh
gclient sync
cd src
</pre>

http://alfalfa.github.io/youtube_replication/
