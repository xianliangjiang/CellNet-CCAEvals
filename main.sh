#!/bin/bash
#set -x # Enable logging of executed commands.
set -e # Stop if any error occurs.
########################Getopt Function#############################
# Simulation types
simtype=$1
# TCP algorithm
selectedtcp=$2
# Cellular trace set: support MIT, NYU and NUS
traceset=$3
# Trace number in trace set, ALL means the whole trace set
traceid=$4

if [ $# != 4 ]
then
	echo "USAGE: $0 <Simulation Type> <TCP Algorithm> <Trace Set> <Trace ID>"
	echo "Algorithms: CUBIC Hybla Illinois Vegas Veno Westwood LP NV BBR CDG Verus Sprout"
	echo "Simulation Type includes T(hroughput)-Delay, T-Loss, T-RTT, T-Buffer, etc."
	echo "If TCP Algorithm is [All], all algorithms will be evaluated."
	echo "If Trace ID is [All], all traces in the trace set will be used."
	exit 1;
fi
######################General Environment###########################
export SPROUT_MODEL_IN="./protocols/sprout/src/examples/sprout.model"
export SPROUT_BT2="./protocols/sprout/src/examples/sproutbt2"
export VERUS_SERVER="./protocols/verus/src/verus_server"
export VERUS_CLIENT="./protocols/verus/src/verus_client"

# Manually extensible part: uplink and downlink trace file, congestion control algorithms
if [[ $traceset == "mit" ]]; then
	DOWNLINKS=("ATT-LTE-driving.down" "TMobile-LTE-driving.down" "TMobile-UMTS-driving.down" "Verizon-EVDO-driving.down" "Verizon-LTE-driving.down")
	UPLINKS=("ATT-LTE-driving.up" "TMobile-LTE-driving.up" "TMobile-UMTS-driving.up" "Verizon-EVDO-driving.up" "Verizon-LTE-driving.up")
	duration=("750" "470" "920" "1000" "1350") #less than the duration of trace files 
elif [[ $traceset == "nyu" ]]; then
	DOWNLINKS=("4G-no-Cross-Subway.down" "4G-with-Cross-Subway.down" "4G-no-Cross-Times.down" "4G-with-Cross-Times.down")
	UPLINKS=("4G-no-Cross-Subway.up" "4G-with-Cross-Subway.up" "4G-no-Cross-Times.up" "4G-with-Cross-Times.up")
	duration=("670" "690" "940" "900") #less than the duration of trace files 
elif [[ $traceset == "nus" ]]; then
	DOWNLINKS=("M1-Bus.down" "M1-Lab.down" "Singtel-Bus.down" "Singtel-Lab.down")
	UPLINKS=("M1-Bus.up" "M1-Lab.up" "Singtel-Bus.up" "Singtel-Lab.up")
	duration=("110" "110" "110" "110") #less than the duration of trace files
else
	echo "The trace set is wrong."
	exit 1
fi

TCPCCA=("hybla" "illinois" "lp" "nv" "vegas" "veno" "westwood" "bbr" "cdg" "verus" "sprout")
RTT=("10" "20" "40" "80" "120" "160" "200") # ms
QUEUE_ALG=("droptail" "codel" "pie" "red" "abc")
BUFFER_SIZE=("50" "100" "200" "300" "400" "500")
LOSS_RATE=("0.00001" "0.0001" "0.001" "0.01" "0.1")

DEFAULT_RTT=20
DEFAULT_PORT=50001
DEFAULT_QUEUE_ALG="droptail"
DEFAULT_LOSS_RATE=0
DEFAULT_BUFFER_SIZE=300

######################Kernel Configuration###########################
echo "===================================================="
echo " Enabling Linux Kernel Congestion Control Algorithms"
echo "===================================================="
# Need to be changed when you want to add new algorithms
CCAs="hybla illinois lp nv vegas veno westwood bbr cdg"
for cc in $CCAs; do
	echo "Loading [$cc] module......"
	sudo modprobe tcp_$cc
done

echo "==================================================="
echo " Reconfiguring Linux Kernel Parameters"
echo "==================================================="
sudo sysctl -w net.ipv4.ip_forward=1
sudo sysctl -w net.core.rmem_max=6553600
sudo sysctl -w net.core.wmem_max=6553600
sudo sysctl -w net.core.rmem_default=65536
sudo sysctl -w net.core.wmem_default=65536
sudo sysctl -w net.ipv4.tcp_rmem='4096 87380 6553600'
sudo sysctl -w net.ipv4.tcp_wmem='4096 65536 6553600'
sudo sysctl -w net.ipv4.tcp_mem='6553600 6553600 6553600'
sudo sysctl -w net.ipv4.route.flush=1
sudo sysctl -w net.ipv4.tcp_low_latency=1
sudo sysctl -w net.ipv4.tcp_autocorking=0
sudo sysctl -w net.ipv4.tcp_no_metrics_save=1

##########################Main Function############################
if [[ $simtype == "T-Delay" ]]
then
	if [[ $traceid == "All" ]]
	then
		# All trace in selected trace set.
		total_traces=${#DOWNLINKS[@]}
		for (( i = 0; i < $total_traces; i++ ))
		do
			# All TCPs
			if [[ $selectedtcp == "All" ]]
			then
				total_tcps=${#TCPCCA[@]}
				for (( j = 0; j < $total_tcps; j++ ))
				do
					algUC=$(echo ${TCPCCA[$j]} | tr "a-z" "A-Z")
					echo "${algUC}: ${DOWNLINKS[$i]} ${UPLINKS[$i]}"
					echo "${algUC}" 1>&2
					echo "Down Linkfile: ${DOWNLINKS[$i]}" 1>&2
					echo "Up Linkfile: ${UPLINKS[$i]}" 1>&2
					echo "Round Trip Time: $DEFAULT_RTT ms" 1>&2
					echo "Packet Loss Rate: $DEFAULT_LOSS_RATE" 1>&2
					echo "Queue Algorithm: $DEFAULT_QUEUE_ALG" 1>&2
					echo "Buffer Size: $DEFAULT_BUFFER_SIZE packets" 1>&2
					if [[ ${TCPCCA[$j]} != "verus" ]] && [[ ${TCPCCA[$j]} != "sprout" ]]
					then
						sudo su <<EOF
						echo ${TCPCCA[$j]} > /proc/sys/net/ipv4/tcp_congestion_control
EOF
						./mm-tcp ${DOWNLINKS[$i]} ${UPLINKS[$i]} ${TCPCCA[$j]} $((DEFAULT_PORT+10*j)) $DEFAULT_RTT $DEFAULT_LOSS_RATE $DEFAULT_QUEUE_ALG $DEFAULT_BUFFER_SIZE $traceset ${duration[$i]}
						./mm-metric 500 up-${TCPCCA[$j]}-$DEFAULT_RTT 1>/dev/null
					elif [[ ${TCPCCA[$j]} == "verus" ]]
					then
						./mm-verus ${DOWNLINKS[$i]} ${UPLINKS[$i]} ${TCPCCA[$j]} $((DEFAULT_PORT+10*j)) $DEFAULT_RTT $DEFAULT_LOSS_RATE $DEFAULT_QUEUE_ALG $DEFAULT_BUFFER_SIZE $traceset ${duration[$i]}
						./mm-metric 500 down-${TCPCCA[$j]}-$DEFAULT_RTT 1>/dev/null
					elif [[ ${TCPCCA[$j]} == "sprout" ]]
					then
						./mm-sprout ${DOWNLINKS[$i]} ${UPLINKS[$i]} ${TCPCCA[$j]} $((DEFAULT_PORT+10*j)) $DEFAULT_RTT $DEFAULT_LOSS_RATE $DEFAULT_QUEUE_ALG $DEFAULT_BUFFER_SIZE $traceset ${duration[$i]}
						./mm-metric 500 up-${TCPCCA[$j]}-$DEFAULT_RTT 1>/dev/null
					fi
					rm up-${TCPCCA[$j]}-$DEFAULT_RTT
					rm down-${TCPCCA[$j]}-$DEFAULT_RTT
					echo "-------------------------------------------------------------" >&2
				done
			else
				# only the selected TCP
				algUC=$(echo ${TCPCCA[$selectedtcp]} | tr "a-z" "A-Z")
				echo "${algUC}: ${DOWNLINKS[$i]} ${UPLINKS[$i]}"
				echo "${algUC}" 1>&2
				echo "Down Linkfile: ${DOWNLINKS[$i]}" 1>&2
				echo "Up Linkfile: ${UPLINKS[$i]}" 1>&2
				echo "Round Trip Time: $DEFAULT_RTT ms" 1>&2
				echo "Packet Loss Rate: $DEFAULT_LOSS_RATE" 1>&2
				echo "Queue Algorithm: $DEFAULT_QUEUE_ALG" 1>&2
				echo "Buffer Size: $DEFAULT_BUFFER_SIZE packets" 1>&2
				if [[ ${TCPCCA[$selectedtcp]} != "verus" ]] && [[ ${TCPCCA[$selectedtcp]} != "sprout" ]]
				then
					sudo su <<EOF
					echo ${TCPCCA[$selectedtcp]} > /proc/sys/net/ipv4/tcp_congestion_control
EOF
					./mm-tcp ${DOWNLINKS[$i]} ${UPLINKS[$i]} ${TCPCCA[$selectedtcp]} $((DEFAULT_PORT+10*selectedtcp)) $DEFAULT_RTT $DEFAULT_LOSS_RATE $DEFAULT_QUEUE_ALG $DEFAULT_BUFFER_SIZE $traceset ${duration[$i]}
					./mm-metric 500 up-${TCPCCA[$selectedtcp]}-$DEFAULT_RTT 1>/dev/null
				elif [[ ${TCPCCA[$selectedtcp]} == "verus" ]]
				then
					./mm-verus ${DOWNLINKS[$i]} ${UPLINKS[$i]} ${TCPCCA[$selectedtcp]} $((DEFAULT_PORT+10*selectedtcp)) $DEFAULT_RTT $DEFAULT_LOSS_RATE $DEFAULT_QUEUE_ALG $DEFAULT_BUFFER_SIZE $traceset ${duration[$i]}
					./mm-metric 500 down-${TCPCCA[$selectedtcp]}-$DEFAULT_RTT 1>/dev/null
				elif [[ ${TCPCCA[$selectedtcp]} == "sprout" ]]
				then
					./mm-sprout ${DOWNLINKS[$i]} ${UPLINKS[$i]} ${TCPCCA[$selectedtcp]} $((DEFAULT_PORT+10*selectedtcp)) $DEFAULT_RTT $DEFAULT_LOSS_RATE $DEFAULT_QUEUE_ALG $DEFAULT_BUFFER_SIZE $traceset ${duration[$i]}
					./mm-metric 500 up-${TCPCCA[$selectedtcp]}-$DEFAULT_RTT 1>/dev/null
				fi
				rm up-${TCPCCA[$selectedtcp]}-$DEFAULT_RTT
				rm down-${TCPCCA[$selectedtcp]}-$DEFAULT_RTT
				echo "-------------------------------------------------------------" >&2
			fi
		done
	else
		# All TCPs
		if [[ $selectedtcp == "All" ]]
		then
			total_tcps=${#TCPCCA[@]}
			for (( j = 0; j < $total_tcps; j++ ))
			do
				algUC=$(echo ${TCPCCA[$j]} | tr "a-z" "A-Z")
				echo "${algUC}: ${DOWNLINKS[$traceid]} ${UPLINKS[$traceid]}"
				echo "${algUC}" 1>&2
				echo "Down Linkfile: ${DOWNLINKS[$traceid]}" 1>&2
				echo "Up Linkfile: ${UPLINKS[$traceid]}" 1>&2
				echo "Round Trip Time: $DEFAULT_RTT ms" 1>&2
				echo "Packet Loss Rate: $DEFAULT_LOSS_RATE" 1>&2
				echo "Queue Algorithm: $DEFAULT_QUEUE_ALG" 1>&2
				echo "Buffer Size: $DEFAULT_BUFFER_SIZE packets" 1>&2
				if [[ ${TCPCCA[$j]} != "verus" ]] && [[ ${TCPCCA[$j]} != "sprout" ]]
				then
					sudo su <<EOF
					echo ${TCPCCA[$j]} > /proc/sys/net/ipv4/tcp_congestion_control
EOF
					./mm-tcp ${DOWNLINKS[$traceid]} ${UPLINKS[$traceid]} ${TCPCCA[$j]} $((DEFAULT_PORT+10*j)) $DEFAULT_RTT $DEFAULT_LOSS_RATE $DEFAULT_QUEUE_ALG $DEFAULT_BUFFER_SIZE $traceset ${duration[$traceid]}
					./mm-metric 500 up-${TCPCCA[$j]}-$DEFAULT_RTT 1>/dev/null
				elif [[ ${TCPCCA[$j]} == "verus" ]]
				then
					./mm-verus ${DOWNLINKS[$traceid]} ${UPLINKS[$traceid]} ${TCPCCA[$j]} $((DEFAULT_PORT+10*j)) $DEFAULT_RTT $DEFAULT_LOSS_RATE $DEFAULT_QUEUE_ALG $DEFAULT_BUFFER_SIZE $traceset ${duration[$traceid]}
					./mm-metric 500 down-${TCPCCA[$j]}-$DEFAULT_RTT 1>/dev/null
				elif [[ ${TCPCCA[$j]} == "sprout" ]]
				then
					./mm-sprout ${DOWNLINKS[$traceid]} ${UPLINKS[$traceid]} ${TCPCCA[$j]} $((DEFAULT_PORT+10*j)) $DEFAULT_RTT $DEFAULT_LOSS_RATE $DEFAULT_QUEUE_ALG $DEFAULT_BUFFER_SIZE $traceset ${duration[$traceid]}
					./mm-metric 500 up-${TCPCCA[$j]}-$DEFAULT_RTT 1>/dev/null
				fi
				rm up-${TCPCCA[$j]}-$DEFAULT_RTT
				rm down-${TCPCCA[$j]}-$DEFAULT_RTT
				echo "-------------------------------------------------------------" >&2
			done
		else
			algUC=$(echo ${TCPCCA[$selectedtcp]} | tr "a-z" "A-Z")
			echo "${algUC}: ${DOWNLINKS[$traceid]} ${UPLINKS[$traceid]}"
			echo "${algUC}" 1>&2
			echo "Down Linkfile: ${DOWNLINKS[$traceid]}" 1>&2
			echo "Up Linkfile: ${UPLINKS[$traceid]}" 1>&2
			echo "Round Trip Time: $DEFAULT_RTT ms" 1>&2
			echo "Packet Loss Rate: $DEFAULT_LOSS_RATE" 1>&2
			echo "Queue Algorithm: $DEFAULT_QUEUE_ALG" 1>&2
			echo "Buffer Size: $DEFAULT_BUFFER_SIZE packets" 1>&2
			if [[ ${TCPCCA[$selectedtcp]} != "verus" ]] && [[ ${TCPCCA[$selectedtcp]} != "sprout" ]]
			then
				sudo su <<EOF
				echo ${TCPCCA[$selectedtcp]} > /proc/sys/net/ipv4/tcp_congestion_control
EOF
				./mm-tcp ${DOWNLINKS[$traceid]} ${UPLINKS[$traceid]} ${TCPCCA[$selectedtcp]} $((DEFAULT_PORT+10*selectedtcp)) $DEFAULT_RTT $DEFAULT_LOSS_RATE $DEFAULT_QUEUE_ALG $DEFAULT_BUFFER_SIZE $traceset ${duration[$traceid]}
				./mm-metric 500 up-${TCPCCA[$selectedtcp]}-$DEFAULT_RTT 1>/dev/null
			elif [[ ${TCPCCA[$selectedtcp]} == "verus" ]]
			then
				./mm-verus ${DOWNLINKS[$traceid]} ${UPLINKS[$traceid]} ${TCPCCA[$selectedtcp]} $((DEFAULT_PORT+10*selectedtcp)) $DEFAULT_RTT $DEFAULT_LOSS_RATE $DEFAULT_QUEUE_ALG $DEFAULT_BUFFER_SIZE $traceset ${duration[$traceid]}
				./mm-metric 500 down-${TCPCCA[$selectedtcp]}-$DEFAULT_RTT 1>/dev/null
			elif [[ ${TCPCCA[$selectedtcp]} == "sprout" ]]
			then
				./mm-sprout ${DOWNLINKS[$traceid]} ${UPLINKS[$traceid]} ${TCPCCA[$selectedtcp]} $((DEFAULT_PORT+10*selectedtcp)) $DEFAULT_RTT $DEFAULT_LOSS_RATE $DEFAULT_QUEUE_ALG $DEFAULT_BUFFER_SIZE $traceset ${duration[$traceid]}
				./mm-metric 500 up-${TCPCCA[$selectedtcp]}-$DEFAULT_RTT 1>/dev/null
			fi
			rm up-${TCPCCA[$selectedtcp]}-$DEFAULT_RTT
			rm down-${TCPCCA[$selectedtcp]}-$DEFAULT_RTT
			echo "-------------------------------------------------------------" >&2
		fi
	fi
elif [[ $simtype == "T-Loss" ]]
then
	echo "T-Loss is under construct..."
	# T-Loss, TODO
elif [[ $simtype == "T-RTT" ]]
then
	echo "T-RTT is under construct..."
	# T-RTT, TODO
elif [[ $simtype == "T-Buffer" ]]
then
	echo "T-Buffer is under construct..."
	# T-Buffer, TODO
else
	echo "The simulation type is wrong!!"	
	exit 1
fi

echo "===========Finish Simulation!!============="
