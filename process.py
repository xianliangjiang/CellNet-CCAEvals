import os
import sys

if len(sys.argv) != 3:
	print 'Usage: python log-process.py <filename> <output_tsv>'
	sys.exit(1)

results = []
infile = open(sys.argv[1], 'r')
protocol = None
downlink = None
uplink = None
throughput = None
capacity = None
ave_delay = None
delay_95th = None
queue_delay = None
for line in infile:
	l = line.strip()
	if l in ['RENO', 'NV', 'CUBIC', 'VEGAS', 'VENO', 'WESTWOOD', 'YEAH', 'ILLINOIS', 'HTCP', 'HYBLA', 'LP', 'VERUS', 'SPROUT']:
		if not protocol is None:
			results.append((protocol, downlink, uplink, rtt, plr, queue_alg, buffer_size, throughput, capacity, ave_delay, delay_95th, queue_delay))
			protocol = None
			downlink = None
			uplink = None
			rtt = None
			plr = None
			queue_alg = None
			buffer_size = None
			throughput = None
			capacity = None
			ave_delay = None
			delay_95th = None
			queue_delay = None
		protocol = l
	elif l.startswith('Down Linkfile:'):
		downlink = l.split(':')[1]
	elif l.startswith('Up Linkfile:'):
		uplink = l.split(':')[1]
	elif l.startswith('Round Trip Time:'):
		rtt = l.split(':')[1].strip().split(' ')[0]
	elif l.startswith('Packet Loss Rate:'):
		plr = l.split(':')[1].strip().split(' ')[0]
	elif l.startswith('Queue Algorithm:'):
		queue_alg = l.split(':')[1].strip().split(' ')[0]
	elif l.startswith('Buffer Size:'):
		buffer_size = l.split(':')[1].strip().split(' ')[0]
	elif l.startswith('Average throughput:'):
		throughput = l.split(':')[1].strip().split(' ')[0]
	elif l.startswith('Average capacity:'):
		capacity = l.split(':')[1].strip().split(' ')[0]
	elif l.startswith('Average per packet delay:'):
		ave_delay = l.split(':')[1].strip().split(' ')[0]
	elif l.startswith('95th percentile signal delay:'):
		delay_95th = l.split(':')[1].strip().split(' ')[0]
	elif l.startswith('95th percentile per-packet queueing delay:'):
		queue_delay = l.split(':')[1].strip().split(' ')[0]


if not protocol is None:
	results.append((protocol, downlink, uplink, rtt, plr, queue_alg, buffer_size, throughput, capacity, ave_delay, delay_95th, queue_delay))

# now write it out nicely formatted
outfile = open(sys.argv[2], 'w')
outfile.write('protocol\tdownlink\tuplink\trtt\tplr\tqueue_alg\tbuffer_size\tthroughput\tcapacity\tave_delay\tdelay_95th\tqueue_delay\n')
for result in results:
	print result
	outfile.write('\t'.join(result))
	outfile.write('\n')
outfile.close()
