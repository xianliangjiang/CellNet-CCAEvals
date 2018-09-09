#!/bin/bash

./main.sh T-Delay All mit All 2>results/t-delay-mit.d
python process.py results/t-delay-mit.d results/t-delay-mit.csv
./main.sh T-Delay All nus All 2>results/t-delay-nus.d
python process.py results/t-delay-nus.d results/t-delay-nus.csv
./main.sh T-Delay All nyu All 2>results/t-delay-nyu.d
python process.py results/t-delay-nyu.d results/t-delay-nyu.csv

#########################Post-Processing############################

#python figures.py results/$simtype.csv figures/fig

#echo "Open a browser and navigate to http://<ip_address>/figures/ to view the figures."
#sudo python -m SimpleHTTPServer 80