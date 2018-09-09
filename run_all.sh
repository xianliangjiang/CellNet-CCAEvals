#!/bin/bash
#set -x # Enable logging of executed commands.
set -e # Stop if any error occurs.

if [ -d "results" ]; then
	rm -rf results
	mkdir results
else
	mkdir results
fi

if [ -d "figures" ]; then
	rm -rf figures
	mkdir figures
else
	mkdir figures
fi

chmod a+x main.sh
chmod a+x mm-metric
chmod a+x mm-tcp
chmod a+x mm-verus
chmod a+x mm-sprout

./main.sh T-Delay All mit All 2>results/t-delay-mit.d
./main.sh T-Delay All nyu All 2>results/t-delay-nyu.d
./main.sh T-Delay All nus All 2>results/t-delay-nus.d

#########################Post-Processing############################
python process.py results/t-delay-mit.d results/t-delay-mit.csv
python process.py results/t-delay-nyu.d results/t-delay-nyu.csv
python process.py results/t-delay-nus.d results/t-delay-nus.csv
#python figures.py results/$simtype.csv figures/fig

#echo "Open a browser and navigate to http://<ip_address>/figures/ to view the figures."
#sudo python -m SimpleHTTPServer 80
