/* Scalable Increase Adaptive Decrease (SIAD) Congestion Control Algorithm
   Author: Mirja Kühlewind, Uni Stuttgart
*/

#include <linux/module.h>
#include <net/tcp.h>
#include <linux/types.h>

#define OFFSET 1     // in pkts (because of rounding)
#define MIN_CWND 2U  // minimum cwnd
#define NUM_RTT 20   // default number of RTT per congestion epoch
                     // between two congestion events
#define MIN_RTT 2    // minimum number of RTTs for one congestion epoch

static int num_rtt = NUM_RTT;
static int num_ms  = 0;

module_param(num_rtt, int, 0644);
MODULE_PARM_DESC(num_rtt, "desired number of RTTs between two congestion events (if resulting time interval is larger than configured number of milliseconds)");
module_param(num_ms, int, 0644);
MODULE_PARM_DESC(num_ms, "desired milliseconds between two congestion events (if larger than resulting time interval for the configured number of RTTs)");

//64 Byte=16*32bit
struct siad {
  int config_num_rtt;     // configured Num_RTT value 
                          // by Socket Option TCP_SIAD_NUM_RTT
                          // (must be first variable in siad struct)
  u32 default_num_rtt;    // default Num_RTT value 
                          // from module parameter or sysctl 
                          // (will be set at connection start)
  u32 default_num_ms;     // default Num_ms value 
                          // from module parameter or sysctl 
                          // (will be set at connection start)
  u32 curr_num_rtt;       // current calculated Num_RTT 
                          // based on minimum of num_rtt and num_ms 
                          // or config_num_rtt or sysctl_tcp_siad_num_rtt

  u32 increase;           // = alpha * curr_num_rtt 
                          // (provides sufficient resolution as minimum 
                          // increase rate of 1 pkt/congestion epoch needed)
  u32 prev_max_cwnd;      // estimated maximum cwnd 
                          // at previous congestion event
  u32 incthresh;          // Linear Increment threshold 
                          // to enter Fast Increase phase 
                          // (target value after decrease based on max. cwnd)

  u32 prior_snd_una;      // ACK number of the previously received ACK

  u32 prev_delay;         // delay value of previous sample
                          // (to filter out single outliers)
  u32 curr_delay;         // filtered current delay value
  u32 min_delay;          // absolute minimum delay
  u32 curr_min_delay;     // minimum delay since last congestion event
  u32 dec_cnt;            // number of additional decreases 
                          // (for current congestion epoch)
  u8  min_delay_seen;     // state variable if the minimum delay was seen
                          // after a regular window reduction
  u8  increase_performed; // state variable if at least one increase 
                          // was performed before new decrease
  u16 prev_min_delay1,    // previous min_delay values if
      prev_min_delay2,    // monotonously increasing values
      prev_min_delay3;    // due to measurement errors
};

static void tcp_siad_init(struct sock *sk){
	struct siad *siad = inet_csk_ca(sk);
	struct tcp_sock *tp = tcp_sk(sk);

	siad->config_num_rtt = 0;
	// Set sysctl only at connection start
	if (sysctl_tcp_siad_num_rtt) {
		siad->default_num_rtt = max(MIN_RTT, sysctl_tcp_siad_num_rtt);
	} else {
		siad->default_num_rtt = num_rtt;
	}
	if (sysctl_tcp_siad_num_ms) {
		siad->default_num_ms = sysctl_tcp_siad_num_ms;
	} else {
		siad->default_num_ms = num_ms;
	}
	siad->curr_num_rtt = siad->default_num_rtt;

	siad->increase = tp->snd_cwnd*siad->curr_num_rtt;
	siad->prev_max_cwnd = tp->snd_cwnd;
	siad->incthresh = tp->snd_cwnd;

	siad->prior_snd_una=tp->snd_una;

	siad->curr_delay = 0;
	siad->min_delay = INT_MAX;
	siad->curr_min_delay = INT_MAX;
	siad->prev_delay = INT_MAX;
	siad->dec_cnt = 0;
	siad->min_delay_seen=1;
	siad->increase_performed=0;
	siad->prev_min_delay1=0;
	siad->prev_min_delay2=0;
	siad->prev_min_delay3=0;
}
EXPORT_SYMBOL_GPL(tcp_siad_init);

static void tcp_siad_cwnd_event(struct sock *sk, enum tcp_ca_event event)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct siad *siad = inet_csk_ca(sk);

	switch (event) {
	case CA_EVENT_COMPLETE_CWR:
		siad->prior_snd_una=tp->snd_una;
		siad->curr_min_delay = INT_MAX;
		siad->dec_cnt = 0;
		siad->min_delay_seen = 0;
		siad->increase_performed=0;
		break;
	default:
		break;
	}
}
EXPORT_SYMBOL_GPL(tcp_siad_cwnd_event);

void tcp_siad_cong_avoid(struct sock *sk, u32 ack, u32 in_flight) {
	struct tcp_sock *tp = tcp_sk(sk);
	struct siad *siad = inet_csk_ca(sk);

	// Estimate current RTT
	u32 delay;
	if (tp->rx_opt.saw_tstamp && tp->rx_opt.rcv_tsecr) {
		// current measurement sample of rtt based on TSopt
		delay = tcp_time_stamp - tp->rx_opt.rcv_tsecr;
	} else {
		//smoothed RTT based on sampled RTT measurements
		delay = tp->srtt>>3;
	}
	// filter out single outliers
	siad->curr_delay = min(delay, siad->prev_delay);
	siad->prev_delay = delay;

	// minimum delay
	if (siad->min_delay == INT_MAX || delay <= siad->min_delay ) {
		// initialize total min delay or set to smaller value
		siad->min_delay = delay;
		siad->min_delay_seen=1;
		siad->curr_min_delay = delay;
	} else if (delay <= siad->curr_min_delay) {
		// update current minimum
		siad->curr_min_delay = delay;
		if (tp->snd_cwnd > tp->snd_ssthresh+(siad->increase/siad->curr_num_rtt)+1) {
			// reset total minimum as same minimum was seen over several RTTs
			siad->min_delay = delay;
			siad->min_delay_seen=1;
		}
	}
	// Do not perform additional decreases in Fast Increase or Slow Start
	if (tp->snd_cwnd > siad->incthresh || tp->snd_cwnd < tp->snd_ssthresh)
		siad->min_delay_seen=1;

	// Estimate unack'ed bytes since last ACK
	u32 bytes_acked = ack-siad->prior_snd_una;
	siad->prior_snd_una = ack;

	// Do not increase or decrease if application limited
	if (!tcp_is_cwnd_limited(sk, in_flight))
		return;

	// Perform (additional) decrease or increase of congestion window
	if (tp->snd_cwnd > tp->snd_ssthresh+(siad->increase/siad->curr_num_rtt)+2
			&& siad->min_delay_seen==0 && siad->dec_cnt<(siad->curr_num_rtt-1) ) {
		// minimum delay not seen in the first RTT -> Additional Decrease
		// (perform at maximum Num_RTT-1 additional decreases)

		// count number of additional decrease
		siad->dec_cnt++;

		// Reset congestion counter at decrease
		tp->snd_cwnd_cnt=0;

		// reduce estimated cwnd from one RTT ago (=ssthresh)
		tp->snd_cwnd = (siad->min_delay * tp->snd_ssthresh / siad->curr_delay);

		if (tp->snd_cwnd > MIN_CWND+OFFSET) {
			// decrease further if large enough

			// 1. decrease by additional offset
			tp->snd_cwnd = tp->snd_cwnd-OFFSET;

			// 2. reduce at least by new alpha (=increase/Num_RTT) 
            // or reach to 0 (or MIN_CWND) after Num_RTT-1 reductions

			// recalculate increase and alpha
			// (assuming already another reduction of the new alpha 
            // -> siad->curr_num_rtt-siad->dec_cnt-1)
			// minimum increase rate of 1 pkt/RTT
			siad->increase = max(1*siad->curr_num_rtt, 
					(siad->incthresh - tp->snd_cwnd) * 
					siad->curr_num_rtt / (siad->curr_num_rtt-siad->dec_cnt-1));
			u32 alpha = siad->increase/siad->curr_num_rtt;
			// calculate reduction to reach 0 after Num_RTT-1 reductions
			u32 reduce = tp->snd_cwnd/(siad->curr_num_rtt-siad->dec_cnt);
			if (reduce < alpha) {
				// reduce at least by alpha
				if (alpha+MIN_CWND < tp->snd_cwnd) {
					tp->snd_cwnd -= alpha;
				} else {
					// set to MIN_CWND
					tp->snd_cwnd = MIN_CWND;
					// don't do any further decreases
					siad->min_delay_seen=1;
				}
			} else {
				// reduce by 'reduce' if larger than alpha
				if (reduce+MIN_CWND < tp->snd_cwnd) {
					tp->snd_cwnd -= reduce;
				} else {
					// set to MIN_CWND
					tp->snd_cwnd = MIN_CWND;
					// don't do any further decreases
					siad->min_delay_seen=1;
				}
				// recalculate increase as cwnd was reduced again
				// minimum increase rate of 1 pkt/RTT
				siad->increase = max(1*siad->curr_num_rtt, 
						(siad->incthresh - tp->snd_cwnd) * 
						siad->curr_num_rtt / (siad->curr_num_rtt-siad->dec_cnt));
			}
		} else {
			// set to MIN_CWND
			tp->snd_cwnd = MIN_CWND;
			// don't do any further decreases
			siad->min_delay_seen=1;
			// recalculate increase
			// minimum increase rate of 1 pkt/RTT
			siad->increase = max(1*siad->curr_num_rtt, 
						(siad->incthresh - tp->snd_cwnd) * 
						siad->curr_num_rtt / (siad->curr_num_rtt-siad->dec_cnt));
		}

		// reset ssthresh
		tp->snd_ssthresh = tp->snd_cwnd-1;

		// don't do any further decreases 
		// as increase rate would need to be larger than doubling per RTT
		if (siad->increase > tp->snd_cwnd*siad->curr_num_rtt) {
			siad->min_delay_seen=1;
		}

	} else {
		// regular increase

		// reset num_rtt during one congestion epoch via socket option
		if (siad->config_num_rtt!=0 &&
				siad->config_num_rtt!=siad->curr_num_rtt) {
			siad->curr_num_rtt = siad->config_num_rtt;
		}

		// compensate for delayed ACK by calculation acked_pkts
		u32 acked_pkts = bytes_acked/tp->mss_cache;
		if (bytes_acked%tp->mss_cache || acked_pkts==0)
			acked_pkts++;
		tp->snd_cwnd_cnt += acked_pkts;

		// same logic as in tcp_cong_avoid_ai() 
		// but also adapts increase rate (and therefore includes SS)

		// increase by more than one (N) packets 
        // if several packets ack'ed and snd_cwnd_cnt>=N*next
		u32 next = max(1, tp->snd_cwnd*siad->curr_num_rtt/siad->increase);
		if (tp->snd_cwnd_cnt >= next) {
			int n = tp->snd_cwnd_cnt/next;
			if (tp->snd_cwnd < tp->snd_cwnd_clamp) {
				// actual number of increased packets
				int inc = min(acked_pkts, min(n, tp->snd_cwnd_clamp-tp->snd_cwnd));
				tp->snd_cwnd+=inc;
				siad->increase_performed=1;

				// adapt increase rate at thresholds 
				// or in Slow Start/Fast Increase
				if (tp->snd_cwnd >= tp->snd_ssthresh && 
						(tp->snd_cwnd-inc) < tp->snd_ssthresh && 
						siad->incthresh > tp->snd_ssthresh)
					//recalculate increase when entering CA from SS
					siad->increase = max(1*siad->curr_num_rtt, 
							siad->incthresh - tp->snd_ssthresh);
				else if ((tp->snd_cwnd >= tp->snd_ssthresh &&
						(tp->snd_cwnd-inc)<tp->snd_ssthresh &&
						siad->incthresh <= tp->snd_ssthresh) || 
						(tp->snd_cwnd >= siad->incthresh &&
						(tp->snd_cwnd-inc)<siad->incthresh))
					// reset increase rate to 1 pkt/RTT
					// 1) if we passed ssthresh 
					//    but don't have information on incthresh or
					// 2) passed/reached incthresh
					siad->increase = 1*siad->curr_num_rtt;
				else if (tp->snd_cwnd > siad->incthresh && 
						siad->increase < ((tp->snd_cwnd>>1)*siad->curr_num_rtt))
					// Slow Start (below ssthresh) 
					// or Fast Increase (above incthresh):
					// double increase rate per RTT
					// but limit maximum increase rate to 1.5*cwnd per RTT
					siad->increase += (inc*siad->curr_num_rtt);
				else if (tp->snd_cwnd < tp->snd_ssthresh)
					// always set alpha to cwnd in Slow Start
					siad->increase = tp->snd_cwnd*siad->curr_num_rtt;
			}
			// decrease counter (not by inc but n)
			tp->snd_cwnd_cnt -= n*next;
		}
    }
}
EXPORT_SYMBOL_GPL(tcp_siad_cong_avoid);

u32 tcp_siad_ssthresh(struct sock *sk) {
	struct siad *siad = inet_csk_ca(sk);
	struct tcp_sock *tp = tcp_sk(sk);

	// Reset congestion counter at decrease
	tp->snd_cwnd_cnt=0;

	// Estimate cwnd when congestion event occurred (about one RTT ago)
	u32 cwnd = tp->snd_cwnd;
	if (siad->increase_performed==1) {
		if (siad->increase >= tp->snd_cwnd*siad->curr_num_rtt ||
				tp->snd_cwnd <= tp->snd_ssthresh) {
			// (simply) halve cwnd
			// if increase is larger than current snd_cwnd or
			// if in Slow Start
			cwnd = tp->snd_cwnd>>1;
		} else if (tp->snd_cwnd > siad->incthresh && 
				siad->increase == (tp->snd_cwnd>>1)*siad->curr_num_rtt) {
			// reduce by 1/3 if in Fast Increase 
			// but increase rate is limited to maximum already
			cwnd -= cwnd/3;
		} else if (tp->snd_cwnd >= siad->incthresh && 
				siad->incthresh > tp->snd_ssthresh && 
				siad->increase == 1*siad->curr_num_rtt) {
			// reduce by (old) alpha if Fast Increase has been just entered
			// and therefore alpha is 1
			cwnd -= (siad->incthresh - tp->snd_ssthresh)/siad->curr_num_rtt;
		} else if (tp->snd_cwnd > siad->incthresh) {
			// minus alpha/2 in Fast Increase
			cwnd -= min(tp->snd_cwnd-MIN_CWND, 
					(siad->increase/siad->curr_num_rtt)>>1);
		} else {
			// minus alpha (= number of increases during last RTT 
			//                since congestion event occurred)
			cwnd -= min(tp->snd_cwnd-MIN_CWND, 
					siad->increase/siad->curr_num_rtt);
		}
	}

	// detect monotonic increasing min delays and reset
	if (siad->min_delay < siad->prev_min_delay1 || 
			siad->min_delay < siad->prev_min_delay2 || 
			siad->min_delay < siad->prev_min_delay3) {
		siad->prev_min_delay1=0;
		siad->prev_min_delay2=0;
		siad->prev_min_delay3=0;
	} else if (siad->min_delay > siad->prev_min_delay1) {
		if (siad->prev_min_delay1 == 0)
			siad->prev_min_delay1 = siad->min_delay;
		else if (siad->prev_min_delay2 == 0)
			siad->prev_min_delay2 = siad->min_delay;
		else if (siad->min_delay > siad->prev_min_delay2) {
			if (siad->prev_min_delay3 == 0)
				siad->prev_min_delay3 = siad->min_delay;
			else if (siad->min_delay > siad->prev_min_delay3) {
				// reset minimum delay and remember as first value
				// (reset other two value to zero)
    			siad->min_delay = siad->prev_min_delay1;
				siad->prev_min_delay2=0;
				siad->prev_min_delay3=0;
    		}
    	}
	}

	// calculate new ssthresh
	u32 ssthresh = cwnd;
	if (siad->min_delay!=INT_MAX && siad->curr_delay!=0) {
		// decrease proportional to delay ratio (see H-TCP)
		ssthresh = (siad->min_delay * cwnd / siad->curr_delay);
    } else {
	    // halve if no information
	    ssthresh = cwnd>>1;
	}
	if (ssthresh > MIN_CWND+OFFSET) {
		// decrease by additional offset
		ssthresh = ssthresh-OFFSET;
	} else {
		// at least MIN_CWND
		ssthresh = MIN_CWND;
	}

	// set current value for num_rtt
	// based on default values or configuration over socket option
	if (siad->config_num_rtt) {
		// use value of socket option TCP_SIAD_NUM_RTT
		siad->curr_num_rtt = siad->config_num_rtt;
	} else if (siad->default_num_ms && siad->min_delay!=INT_MAX &&
			 siad->curr_delay!=0) {
		// calculate a Num_RTT based on current average RTT and num_ms
		// use minimum of default values (either calculated Num_RTT or num_ms)
		u32 tmp = (siad->default_num_ms<<1)/(siad->curr_delay+siad->min_delay);
		siad->curr_num_rtt = max(siad->default_num_rtt, tmp);
	} else {
		// use num_rtt in case no valid RTT measurements are available
		siad->curr_num_rtt = siad->default_num_rtt;
	}

	// calculate increase threshold/target value
	// amplify trend to speed-up convergence (but more oscillation!)
	// trend can be positive or negative
	int trend = cwnd - siad->prev_max_cwnd;
	if (siad->prev_max_cwnd < 2*cwnd)
		// increment threshold at least new cwnd after reduction (=ssthresh)
		siad->incthresh = max(cwnd + trend, ssthresh);
	else
		siad->incthresh = ssthresh;

	// calculate new increase
	// with minimum increase rate of 1 packet per RTT
	siad->increase = max(1*siad->curr_num_rtt, siad->incthresh - ssthresh);

	// remember estimated max value before reduction
	// for next trend calculation
	siad->prev_max_cwnd = cwnd;

	return ssthresh;
}
EXPORT_SYMBOL_GPL(tcp_siad_ssthresh);


u32  tcp_siad_undo_cwnd(struct sock *sk) {
	struct siad *siad = inet_csk_ca(sk);
	struct tcp_sock *tp = tcp_sk(sk);
	u32 cwnd = siad->incthresh;
	siad->incthresh = siad->prev_max_cwnd;
	siad->min_delay_seen = 1;
	return cwnd;
}
EXPORT_SYMBOL_GPL(tcp_siad_undo_cwnd);

static struct tcp_congestion_ops tcp_siad = {
  .init = tcp_siad_init,
  .name = "siad",
  .ssthresh = tcp_siad_ssthresh,
  .cong_avoid = tcp_siad_cong_avoid,
  .cwnd_event = tcp_siad_cwnd_event,
  .undo_cwnd = tcp_siad_undo_cwnd,
};

static int __init tcp_siad_register(void){
  return tcp_register_congestion_control(&tcp_siad);
}

static void __exit tcp_siad_unregister(void){
  tcp_unregister_congestion_control(&tcp_siad);
}

module_init(tcp_siad_register);
module_exit(tcp_siad_unregister);

MODULE_AUTHOR("Mirja Kuehlewind");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("TCP SIAD");
MODULE_VERSION("1.0");
