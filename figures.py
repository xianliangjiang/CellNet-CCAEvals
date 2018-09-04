import os
import sys

import matplotlib.pyplot as plt

def make_delay95th_graph(data, title, save):
	markers = ['*', 'o', '^', 'v', 'd', 'p', 'x']
	fig = plt.figure(figsize=(9,6), dpi=2048, facecolor="white")
	ax = fig.add_subplot(111)
	count = 0
	for label, x, y in data:
		ms = 11
		label = label.lower()
		plt.plot(x, y, markers[count % len(markers)], label=label, ms=ms)
		count += 1
		#ax.annotate('%s' % label, xy=(x, y), textcoords='data')
	ax.set_xscale('log')
	font = {'family' : 'sans-serif', 'weight' : 'normal', 'size'   : 14}
	plt.rc('font', **font)
	plt.gca().invert_xaxis()
	plt.title(title)
	ax.set_xlabel('95th Percentile Signal Delay (ms)', fontname='sans-serif', fontsize=16)
	ax.set_ylabel('Throughput (Mb/s)', fontname='sans-serif', fontsize=16)
	plt.legend(bbox_to_anchor=[0.005, 0.2], loc=2, ncol=2, shadow=True, borderaxespad=0., fontsize='medium', numpoints=1)
	plt.ylim(0)
	print 'Saving: %s' % save
	plt.savefig(save, bbox_inches='tight')

def make_delayave_graph(data, title, save):
	markers = ['*', 'o', '^', 'v', 'd', 'p', 'x']
	fig = plt.figure(figsize=(9,6), dpi=2048, facecolor="white")
	ax = fig.add_subplot(111)
	count = 0
	for label, x, y in data:
		ms = 11
		label = label.lower()
		plt.plot(x, y, markers[count % len(markers)], label=label, ms=ms)
		count += 1
		#ax.annotate('%s' % label, xy=(x, y), textcoords='data')
	ax.set_xscale('log')
	font = {'family' : 'sans-serif', 'weight' : 'normal', 'size'   : 14}
	plt.rc('font', **font)
	plt.gca().invert_xaxis()
	plt.title(title)
	ax.set_xlabel('Average Packet Delay (ms)', fontname='sans-serif', fontsize=16)
	ax.set_ylabel('Throughput (Mb/s)', fontname='sans-serif', fontsize=16)
	plt.legend(bbox_to_anchor=[0.005, 0.2], loc=2, ncol=2, shadow=True, borderaxespad=0., fontsize='medium', numpoints=1)
	plt.ylim(0)
	print 'Saving: %s' % save
	plt.savefig(save, bbox_inches='tight')

def get_data_by_downlink(link, values):
	rows = []
	for d in values:
		if d['downlink'] == link:
			rows.append(d)
	return rows

if len(sys.argv) != 3:
	print 'Usage: python make_graphs.py <input.tsv> <output_prefix>'
	sys.exit(1)

infile = sys.argv[1]
output_prefix = sys.argv[2]

headers = None
values = []
links = []
for line in open(infile, 'r'):
	l = line.strip()
	if headers is None:
		headers = line.split('\t')
	else:
		bits = line.split('\t')  
		if len(bits) != len(headers):
			print 'Anger! this tsv file is malformatted'
		entry = {}
		for i in range(0, len(bits)):
			entry[headers[i]] = bits[i] 
			if headers[i] == 'downlink' and not bits[i] in links:
				links.append(bits[i])
		values.append(entry)

# now make graphs for each of the up/down link pairs
#print 'Making graphs for each link: '
#print links

for link in links:
 	rows = get_data_by_downlink(link, values)
	# now make these rows into (labels, xs, ys) tuples
	data1 = [(r['protocol'], r['delay_95th'], r['throughput']) for r in rows]
	data2 = [(r['protocol'], r['ave_delay'], r['throughput']) for r in rows]
	title = link.split('.')[0].strip()
	make_delay95th_graph(data1, 'Throughput vs. Signal Delay for ' + title, output_prefix + '_95th_' + title + '.png')
	#make_delayave_graph(data2, 'Throughput vs. Average Delay for ' + title, output_prefix + '_ave_' + title + '.png')


