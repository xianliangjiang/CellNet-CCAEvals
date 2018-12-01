/*
 * TCP LOLA: Low Latancy Congestion Control for TCP v1.5
 *
 * Copyright (C) [2017]  [Felix Neumeister, Mario Hock]
 * 
 * This program is free software; you can redistribute it and/or modify it 
 * under the terms of the GNU General Public License as published by the 
 * Free Software Foundation; only version 2.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; 
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License along with this program; 
 * if not, see <http://www.gnu.org/licenses/>.
 *
 * This code is based on the linux kernel v4.4.52 implementation of TCP CUBIC.
 * Function names were modified after major changes only
 */

#include <linux/mm.h>
#include <linux/module.h>
#include <linux/math64.h>
#include <net/tcp.h>
#include <linux/version.h>

#define BICTCP_BETA_SCALE    1024	/* Scale factor beta calculation
					 * max_cwnd = snd_cwnd * beta
					 */
#define	BICTCP_HZ		10	/* BIC HZ 2^10 = 1024 */

/* Two methods of hybrid slow start */
#define HYSTART_ACK_TRAIN	1
#define HYSTART_DELAY		2
#define LOLA_LOSS_SAMPLING_LOCKED 4
#define LOLA_RED_LOCKED 8
#define LOLA_IN_FAIR_FLOW_BALANCING 16
#define LOLA_IN_CWND_HOLD 32
#define LOLA_BASE_REDUCED 128

#define LOLA_TCP_PARAMETER_SHIFT 10

#define LOLA_DO_PRECAUTIONARY_DECONGESTION 1
#define LOLA_DO_FAIR_FLOW_BALANCING 2
#define LOLA_DO_FAST_CONVERGENCE 4
#define LOLA_DO_CWND_HOLD 8

/* Number of delay samples for detecting the increase of delay */
#define HYSTART_MIN_SAMPLES	16
#define HYSTART_DELAY_MIN	(2U * USEC_PER_MSEC)
#define HYSTART_DELAY_MAX	(4U * USEC_PER_MSEC)
#define HYSTART_DELAY_THRESH(x)	clamp(x, HYSTART_DELAY_MIN, HYSTART_DELAY_MAX)



static int fast_convergence __read_mostly = 1;
static int beta __read_mostly = 717;	/* = 717/1024 (BICTCP_BETA_SCALE) */
static int initial_ssthresh __read_mostly;
static int bic_scale __read_mostly = 41;
static int tcp_friendliness __read_mostly = 1;

static int hystart __read_mostly = 1;
static int hystart_detect __read_mostly = HYSTART_ACK_TRAIN | HYSTART_DELAY;
static int hystart_low_window __read_mostly = 16;
static int hystart_ack_delta __read_mostly = 2;

static u32 cube_rtt_scale __read_mostly;
static u32 beta_scale __read_mostly;
static u64 cube_factor __read_mostly;

static int lola_mode __read_mostly = 11;
static int lola_queue_max __read_mostly = 5000;
static int lola_delta __read_mostly = 900;		/* fast convergence factor */
static int lola_gamma __read_mostly = 927;		/* pro mill of bandwidth to reduce to */
static int lola_cwnd_min __read_mostly = 5;	/* min congestion window to do precautionary decongestion */
static int lola_base_timeout __read_mostly = 10;	/* shift for exponential moving average */
static int lola_base_delay_epsilon = 100;		/* vicinity counting as base delay measurement */
static int lola_fair_flow_balancing_start_delay __read_mostly = 500;
static int lola_fair_flow_balancing_curve_factor = 75;
static int lola_hold_time = 250;
static int lola_min_samples = 20;
static int lola_measurement_time = 40;
static int lola_slow_start_exit = 1000;


/* Note parameters that are used for precomputing scale factors are read-only */
module_param(fast_convergence, int, 0644);
MODULE_PARM_DESC(fast_convergence, "turn on/off fast convergence");
module_param(beta, int, 0644);
MODULE_PARM_DESC(beta, "beta for multiplicative increase");
module_param(initial_ssthresh, int, 0644);
MODULE_PARM_DESC(initial_ssthresh, "initial value of slow start threshold");
module_param(bic_scale, int, 0444);
MODULE_PARM_DESC(bic_scale, "scale (scaled by 1024) value for bic function (bic_scale/1024)");
module_param(tcp_friendliness, int, 0644);
MODULE_PARM_DESC(tcp_friendliness, "turn on/off tcp friendliness");
module_param(hystart, int, 0644);
MODULE_PARM_DESC(hystart, "turn on/off hybrid slow start algorithm");
module_param(hystart_detect, int, 0644);
MODULE_PARM_DESC(hystart_detect, "hyrbrid slow start detection mechanisms"
		 " 1: packet-train 2: delay 3: both packet-train and delay");
module_param(hystart_low_window, int, 0644);
MODULE_PARM_DESC(hystart_low_window, "lower bound cwnd for hybrid slow start");
module_param(hystart_ack_delta, int, 0644);
MODULE_PARM_DESC(hystart_ack_delta, "spacing between ack's indicating train (msecs)");


module_param(lola_mode, int, 0644);
MODULE_PARM_DESC(lola_mode, "Turn features on and off (1-decongestion 2-convBoost 4-fastConv 8-Hold)");
module_param(lola_queue_max, int, 0644);
MODULE_PARM_DESC(lola_queue_max, "Maximum queue length (usec)");
module_param(lola_delta, int, 0644);
MODULE_PARM_DESC(lola_beta, "Reduction factor for fast convergence * 2^-10");
module_param(lola_gamma, int, 0644);
MODULE_PARM_DESC(lola_gamma, "Bandwidth to reduce to in precautionary decongestion * 2^-10");
module_param(lola_cwnd_min, int, 0644);
MODULE_PARM_DESC(lola_cwnd_min, "Minimum cwnd to do precautionary decongestion");
module_param(lola_base_timeout, int, 0644);
MODULE_PARM_DESC(lola_base_timeout, "Timeout for base delay in epochs (0 for disabled)");
module_param(lola_base_delay_epsilon, int, 0644);
MODULE_PARM_DESC(lola_base_timeout, "Epsilon-vacinity counting as a measured value (usec)");
module_param(lola_fair_flow_balancing_start_delay, int, 0644);
MODULE_PARM_DESC(lola_fair_flow_balancing_start_delay, "Start of fair flow balancing (usec)");
module_param(lola_fair_flow_balancing_curve_factor, int, 0644);
MODULE_PARM_DESC(lola_fair_flow_balancing_curve_factor, "Scaling factor for quick convergence curve");
module_param(lola_hold_time, int, 0644);
MODULE_PARM_DESC(lola_hold_time, "hold time (ms)");
module_param(lola_min_samples, int, 0644);
MODULE_PARM_DESC(lola_min_samples, "Minimal numer of rtt samples to make a decision");
module_param(lola_measurement_time, int, 0644);
MODULE_PARM_DESC(lola_measurement_time, "Length of RTT measuremnt intervals (msec)");
module_param(lola_slow_start_exit, int, 0644);
MODULE_PARM_DESC(lola_slow_start_exit, "Slow start exit delay (usec)");


/*
 * Changes to save space:
 *
 * last_cwnd originally saved the cwnd after the last calculation of cnt. It monitors the 
 * whether the cwnd changed and triggers a recalc of cnt if it did. It has been cut by saving the cwnd
 * at the beginning of cong_avoid and setting last_time variable to zero triggering
 * a recalculation if it changed. (see line 741)
 *
 * round_start was replaced with epoch_start by consequently setting epoch_start to 0, whenever slow start ends.
 * This allows epoch_start to be != 0 during slow start and not prevent recalculation.
 */


/* BIC TCP Parameters */
struct lolatcp {
	s32	cnt;		/* increase cwnd by 1 after ACKs */
	u32	last_max_cwnd;	/* last maximum snd_cwnd */
	u32	loss_cwnd;	/* congestion window at last loss */
	u32	last_time;	/* time when updated last_cwnd */
	u32	bic_origin_point;/* origin point of bic function */
	u32	bic_K;		/* time to origin point
				   from the beginning of the current epoch */

	u32	epoch_start;	/* beginning of an epoch */
	u32	ack_cnt;	/* number of acks */
	u32	tcp_cwnd;	/* estimated tcp cwnd */
	u32	sample_cnt;	/* number of samples to decide curr_rtt */
	u32	delay_min;	/* min delay */
	/*u8	limited_rtt_cnt; /* counts the number of */
	u8    id;
	u8	base_invalidation_count;	/* counter for epochs without base measurement*/
	u8	flags;		/* saves current state of the algorithm */
	u32	end_seq;	/* end_seq of the round */
	u32	last_ack;	/* last time when the ACK spacing is close */
	u32	curr_rtt;	/* the minimum rtt of current round */
	u32    end_measurement;	/* timestamp of the end of the measurement */
};


static inline void lolatcp_reset(struct lolatcp *ca)
{
	printk("<%u>lolatcp_reset called\n", ca->id);
	ca->cnt = 0;
	ca->last_max_cwnd = 0;
	ca->last_time = 0;
	ca->bic_origin_point = 0;
	ca->bic_K = 0;
	ca->delay_min = 0;
	ca->epoch_start = 0;
	ca->ack_cnt = 0;
	ca->tcp_cwnd = 0;
	ca->flags = 0;
	ca->sample_cnt = 0;
	ca->curr_rtt = 0;
}

static inline u32 bictcp_clock(void)
{
#if HZ < 1000
	return ktime_to_ms(ktime_get_real());
#else
	return jiffies_to_msecs(jiffies);
#endif
}

static inline void bictcp_hystart_reset(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct lolatcp *ca = inet_csk_ca(sk);

	ca->epoch_start = ca->last_ack = bictcp_clock();
	ca->end_seq = tp->snd_nxt;
	ca->curr_rtt = 0;
	ca->sample_cnt = 0;
	ca->flags &= ~(LOLA_IN_CWND_HOLD | LOLA_IN_FAIR_FLOW_BALANCING);
}

static void lolatcp_init(struct sock *sk)
{
	struct lolatcp *ca = inet_csk_ca(sk);

	lolatcp_reset(ca);
	ca->loss_cwnd = 0;
	get_random_bytes(&(ca->id), sizeof(ca->id));

	if (hystart)
		bictcp_hystart_reset(sk);

	if (initial_ssthresh)
		tcp_sk(sk)->snd_ssthresh = initial_ssthresh;
}

static void bictcp_cwnd_event(struct sock *sk, enum tcp_ca_event event)
{
	if (event == CA_EVENT_TX_START) {
		struct lolatcp *ca = inet_csk_ca(sk);
		u32 now = tcp_time_stamp;
		s32 delta;

		delta = now - tcp_sk(sk)->lsndtime;

		/* We were application limited (idle) for a while.
		 * Shift epoch_start to keep cwnd growth to cubic curve.
		 */
		if (ca->epoch_start && delta > 0) {
			ca->epoch_start += delta;
			if (after(ca->epoch_start, now))
				ca->epoch_start = now;
		}
		return;
	}
}

/* calculate the cubic root of x using a table lookup followed by one
 * Newton-Raphson iteration.
 * Avg err ~= 0.195%
 */
static u32 cubic_root(u64 a)
{
	u32 x, b, shift;
	/*
	 * cbrt(x) MSB values for x MSB values in [0..63].
	 * Precomputed then refined by hand - Willy Tarreau
	 *
	 * For x in [0..63],
	 *   v = cbrt(x << 18) - 1
	 *   cbrt(x) = (v[x] + 10) >> 6
	 */
	static const u8 v[] = {
		/* 0x00 */    0,   54,   54,   54,  118,  118,  118,  118,
		/* 0x08 */  123,  129,  134,  138,  143,  147,  151,  156,
		/* 0x10 */  157,  161,  164,  168,  170,  173,  176,  179,
		/* 0x18 */  181,  185,  187,  190,  192,  194,  197,  199,
		/* 0x20 */  200,  202,  204,  206,  209,  211,  213,  215,
		/* 0x28 */  217,  219,  221,  222,  224,  225,  227,  229,
		/* 0x30 */  231,  232,  234,  236,  237,  239,  240,  242,
		/* 0x38 */  244,  245,  246,  248,  250,  251,  252,  254,
	};

	b = fls64(a);
	if (b < 7) {
		/* a in [0..63] */
		return ((u32)v[(u32)a] + 35) >> 6;
	}

	b = ((b * 84) >> 8) - 1;
	shift = (a >> (b * 3));

	x = ((u32)(((u32)v[shift] + 10) << b)) >> 6;

	/*
	 * Newton-Raphson iteration
	 *                         2
	 * x    = ( 2 * x  +  a / x  ) / 3
	 *  k+1          k         k
	 */
	x = (2 * x + (u32)div64_u64(a, (u64)x * (u64)(x - 1)));
	x = ((x * 341) >> 10);
	return x;
}

/*
 * Calculates the fair flow balancing target value
 *
 * return (t/sigma)^3
 *
 * The sigma used in this function has to fit the granularity of the timer interrupt and the MSS
 * -> curve_factor = sigma * (MSS)^(1/3) * HZ / MSEC_PER_SEC
 *
 * Calculation done in 64 bit -> stable for time < 2^21 = 1000s or 16min
 */

static u32 lola_get_target(u32 time){
	u64 factor = (tcp_time_stamp - time);
	factor *= factor * factor;
	do_div(factor, lola_fair_flow_balancing_curve_factor * lola_fair_flow_balancing_curve_factor * lola_fair_flow_balancing_curve_factor);
	return (u32)factor;
}


/*
 * Do precautionary decongestion reduces cwnd if the measured RTT is bigger than lola_queue_max
 *
 * This function handles decongestion, fair flow balancing and hold interval 
 */
static void lolatcp_precautionary_decongestion(struct sock *sk){
	struct tcp_sock *tp = tcp_sk(sk);
	struct lolatcp *ca = inet_csk_ca(sk);
//	int reenter_slow_start = 0;


	/*
	 * This if statement regulates whether or not to enter the precautionary decongestion block
	 * The block is entered either if there are legitimate RTT-measurements and no hold or the hold interval is up (implies legitimate measurements)
	 *
	 * To allow switching on and off different mechanisms, this statement can not beseperated into different without producing great code duplication
	 *
	 */


	if((lola_mode & LOLA_DO_PRECAUTIONARY_DECONGESTION) && ca->delay_min && ca->curr_rtt 	/* Check if all necessary variables are set */

			&& ((ca->epoch_start && !(ca->flags & LOLA_IN_CWND_HOLD)  		/* only allow entering if the algorithm it is not in hold */
			&&((ca->flags & (LOLA_IN_FAIR_FLOW_BALANCING|HYSTART_DELAY|LOLA_BASE_REDUCED)) || tcp_time_stamp - ca->epoch_start > 2 * ca->curr_rtt  / USEC_PER_MSEC) 	/* earliest two rtts after reduction or directly after slow start or base delay meausrement*/
			/*
			 * check if measurements are valid :
			 * Valid after one RTT of measurements (check at least once per RTT)
			 * or after the end of this measurement interval and if it contained enough samples
			 */
			&& ((0 && ca->sample_cnt >= tp->snd_cwnd)
			|| (((ca->end_measurement <= tcp_time_stamp) || (ca->flags & HYSTART_DELAY)) && (ca->sample_cnt >= lola_min_samples)))

			&& tp->snd_cwnd >= lola_cwnd_min)					/* don't reduce if cwnd is below threshold */

			/*
			 * enter if hold interval is over
			*/

            ||((ca->flags & LOLA_IN_CWND_HOLD) && (tcp_time_stamp - ca->epoch_start) > msecs_to_jiffies(lola_hold_time))))
	{

		u32 queue_delay = ca->curr_rtt - ca->delay_min;

		printk("<%u>reaching first bracket cwnd:%d sample_cnt:%d RTT:%d cnt:%d bic_K:%d flags:%u baseRTT:%u end:%u time:%u, if:%u 10ms:%u\n", ca->id, tp->snd_cwnd, ca->sample_cnt, ca->curr_rtt, ca->cnt, ca->bic_K, ca->flags, ca->delay_min, ca->end_measurement, tcp_time_stamp, (((ca->end_measurement <= tcp_time_stamp) || (ca->flags & HYSTART_DELAY)) && (ca->sample_cnt >= lola_min_samples)), msecs_to_jiffies(10));
		ca->flags &= ~64;

		/*
		 * If the queue delay is bigger than lola_fair_flow_balancing_start_delay it enters the fair flow balancing.
		 * It then sets epoch start to the current time samp to mark the start of the phase
		 */

		if((lola_mode & LOLA_DO_FAIR_FLOW_BALANCING) && !(ca->flags & LOLA_IN_CWND_HOLD) && queue_delay > lola_fair_flow_balancing_start_delay && !(ca->flags & LOLA_IN_FAIR_FLOW_BALANCING)){
			ca->flags |= LOLA_IN_FAIR_FLOW_BALANCING;
			ca->epoch_start = tcp_time_stamp; 
			printk("<%u> going into quick converge with delay:%u",ca->id, queue_delay);

		}

		/*
		 * If the fair flow balancing is started this block is called every time a valid measurement is available.
		 * 
		 * It calculates the number of packets in the queue and uses the get_target function to get the currently
		 * appropriate target value for the number of packets in the queue.
		 * It then sets the cnt value for the paced congestion window increase provided by the kernel - called in gong_avoid
		 * If the congestion window should not be incresased, the cnt variable is set to high value, which constitures to one packet incrase in 100 RTTs
		 */
		if(ca->flags & LOLA_IN_FAIR_FLOW_BALANCING){
			u32 target_queue;
			u64 packets_in_queue;

			target_queue = lola_get_target(ca->epoch_start);
                        
			packets_in_queue = ((u64)tp->snd_cwnd) * queue_delay;
			do_div(packets_in_queue, ca->curr_rtt);
			printk("<%u> in quick-convergence target_queue:%u packets_in_queue:%u \n",ca->id, target_queue, packets_in_queue);


 			/*
                         * Set cnt (number of packets after which the cwnd is increased by 1)
                         * 
                         * since cwnd / cnt = increase_per_RTT (see tcp_cong_avoid_ai())
                         * => cnt = cwnd / increse_per_RTT
                         * increase_per_RTT = increase_per_time_interval * RTT / time_interval
                         * 
                         * => cnt = cwnd / (increase_per_time_interval * RTT / time_interval) = cwnd * time_interval / increase_per_time_interval * RTT
                         * 
                         * increase = difference_to_taget 
			 * 	<= max(4*increase_in_target, 2*increase_during_last_interval) 
			 * 	>= 1
                         */

			if(packets_in_queue < target_queue){
				ca->cnt = tp->snd_cwnd * lola_measurement_time * USEC_PER_MSEC / 
					(max(min(max((target_queue - lola_get_target(ca->epoch_start + ca->delay_min / USEC_PER_MSEC))<<2, tp->snd_cwnd / ca->cnt<<1) ,(target_queue - (u32)packets_in_queue)), 1U)* ca->curr_rtt);
			
			
			/*
			 * if difference < 1, set cnt to 1 packet increase per 100 RTTs
			 */
			}else
				ca->cnt = 100 * tp->snd_cwnd;
			ca->cnt = max(ca->cnt, 4U);
		}
		

		/*
		 * If the queueing delay is more than the specified queueing delay, the algorithm does a precautionary decongestion.
		 * Should the algorithm use the hold mechanism and it reaches this point it means the hold interval is over.
		 * Consequently the fair flow balancing is also ended.
		 */
		if(queue_delay > lola_queue_max || (ca->flags & (LOLA_IN_CWND_HOLD|LOLA_BASE_REDUCED|HYSTART_DELAY))){

			ca->flags &= ~LOLA_IN_FAIR_FLOW_BALANCING;
			if(lola_mode & LOLA_DO_PRECAUTIONARY_DECONGESTION){

				/*
				 * Should the hold mechanism be enabled and a too high queueing delay be detected, the hold mechanism is enabled.
				 * Consequently the hold flag is set, the fair flow balancing is disabled, the paced gaining is halted and
				 * epoch start is set to save the beginning of the hold interval
				 */
				if((lola_mode & LOLA_DO_CWND_HOLD) && !(ca->flags & (LOLA_IN_CWND_HOLD | LOLA_BASE_REDUCED|HYSTART_DELAY))){
					ca->flags |= LOLA_IN_CWND_HOLD;
					ca->flags ^= ca->flags & LOLA_IN_FAIR_FLOW_BALANCING;
					ca->cnt = 100 * tp->snd_cwnd;
					ca->epoch_start = tcp_time_stamp;
					printk("<%u> going into hold at %u\n",ca->id, tcp_time_stamp);
					return;
				}
				/*
				 * If the algorithm reaches this point and the hold flag is set, the holding interval is over and the flag should therefore be removed.
				 */
				if(ca->flags & LOLA_IN_CWND_HOLD){
					ca->flags &= ~ LOLA_BASE_REDUCED; 
					printk("<%u> after hold at %u\n", ca->id, tcp_time_stamp);
				}
				if(queue_delay > lola_queue_max) ca->flags &= ~ LOLA_BASE_REDUCED;
				ca->flags &= ~(LOLA_IN_CWND_HOLD | LOLA_IN_FAIR_FLOW_BALANCING|HYSTART_DELAY);

				/*
				 * The congestion window is reduced by the number of packets in the queue.
				 * This completely empties the queue and allows the correct measurement of the propagation delay.
				 *
				 * Instead of calculating a reduction, the new congestion window is calculated directly
				 * The queue is empty when the congestion window equals base delay * bandwidth.
				 * To make the measurement of the base delay more reliable, the congestion window is reduced
				 * to gamma >> 10 of the empty queue.
				 *
				 * The calculation is done the following way:
				 *
				 * cwnd =  bandwidth * base_rtt * gamma
				 * bandwidth = cwnd / curr_rtt
				 *
				 * The calculation is done in 64 bit to prevent overflow and allows higher precision
				 * Using 32 bit would limit cwnd to 2^(32 - LOLATCP_PARAMETER_SHIFT - (log2(rtt in us))
				 *
				 * Using 64 bit, the calculation is stable for the full 32 bit of cwnd 
				 */
                                
				u64 cwnd;

				cwnd = ((u64)tp->snd_cwnd) * ca->delay_min * lola_gamma;
				do_div(cwnd, (u64)ca->curr_rtt);

                		cwnd >>= LOLA_TCP_PARAMETER_SHIFT;
                		printk("<%u> new_cwnd:%du curr_rtt:%u snd_cwnd:%u queue_delay:%u base_rtt:%u\n", ca->id, (u32)cwnd, ca->curr_rtt, tp->snd_cwnd, queue_delay, ca->delay_min);


                		cwnd = max(lola_cwnd_min, cwnd); /* Check that the new congestion window is not below the minimum congestion window */



				/*
				 * CUBIC's fast convergence:
				 *
				 * Decide whether or not to perform fast convergence:
				 * If the cwnd at the previous reduction was bigger than the current, expect another flow is present
				 *
				 * If this is the case, the plateau of the next CUBIC curve is set lower than it would be by the factor of lola_delta.
				 * This increases the convergence, since a flow with a bigger bandwidth share will yield more bandwidth.
				 *
				 */

				if ((lola_mode & LOLA_DO_FAST_CONVERGENCE) && tp->snd_cwnd < ca->last_max_cwnd){

					ca->last_max_cwnd = (tp->snd_cwnd * lola_delta) >> LOLA_TCP_PARAMETER_SHIFT;

					printk("<%u> fast convergence  new cwnd:%u cwnd: %u\n", ca->id, cwnd, tp->snd_cwnd);
				}else{
					ca->last_max_cwnd = tp->snd_cwnd;
				}
				ca->flags &= ~ LOLA_BASE_REDUCED;
				
				tp->snd_cwnd = cwnd;

				/*
				 * Lola base rtt adjust:
				 *
				 * The base RTT is considered to have changed if it has not been measured for a number of epochs.
				 * Here the base delay invalidation counter is increased,
				 * If it exceeds lola_base_timeout the value for the base delay is set to 0 - this equals an invalidation, so the next
				 * measured value is taken and the update process continues as normal in the lolatcp_acked.
				 */

				if(lola_base_timeout){

					ca->base_invalidation_count ++;
					if(ca->base_invalidation_count > lola_base_timeout){
						ca->delay_min  = 0;
						ca->base_invalidation_count = 0;
					}
				}

			}
			/*
			 * Trigger CUBIC function recalculation and set ssthresh
			 */
			printk("<%u> snd_cwnd after reduction:%u\n", ca->id, tp->snd_cwnd);
			ca->epoch_start = 0;
			tp->snd_ssthresh = min(tp->snd_cwnd, tp->snd_ssthresh);

		}

		/*
		 * Reset measurements and set end of next measurement interval
		 *
		 * (Not called if algorithm went into hold)
		 */
		ca->sample_cnt = 0;
		ca->curr_rtt = 0;
		ca->end_measurement = tcp_time_stamp + msecs_to_jiffies(lola_measurement_time);

	}
}

/*
 * Compute congestion window to use.
 */
static inline void bictcp_update(struct lolatcp *ca, u32 cwnd, u32 acked)
{
	u32 delta, bic_target, max_cnt;
	u64 offs, t;

	ca->ack_cnt += acked;	/* count the number of ACKed packets */

	if (ca->epoch_start && ca->bic_origin_point &&
	    (s32)(tcp_time_stamp - ca->last_time) <= HZ / 32)
		return;

	/* The CUBIC function can update ca->cnt at most once per jiffy.
	 * On all cwnd reduction events, ca->epoch_start is set to 0,
	 * which will force a recalculation of ca->cnt.
	 */
	if (ca->epoch_start && tcp_time_stamp == ca->last_time)
		goto tcp_friendliness;

	ca->last_time = tcp_time_stamp;
	/*
	 * Perform new calculation
	 * if: epoch_start is 0 (loss or reduction)
	 * or: bic_origin_point  is 0 (slow start ran last)
	 */

	if (ca->epoch_start == 0) {
		ca->epoch_start = tcp_time_stamp;	/* record beginning */
		ca->ack_cnt = acked;			/* start counting */
		ca->tcp_cwnd = cwnd;			/* syn with cubic */

		if (ca->last_max_cwnd <= cwnd) {
			ca->bic_K = 0;
			ca->bic_origin_point = cwnd;
		} else {
			/* Compute new K based on
			 * (wmax-cwnd) * (srtt>>3 / HZ) / c * 2^(3*bictcp_HZ)
			 */
			ca->bic_K = cubic_root(cube_factor
					       * (ca->last_max_cwnd - cwnd));
			ca->bic_origin_point = ca->last_max_cwnd;
		}
		printk("<%u> did recalc bic_k::%d origin:%d cwnd:%d\n", ca->id, ca->bic_K, ca->bic_origin_point, cwnd);
	}

	/* cubic function - calc*/
	/* calculate c * time^3 / rtt
	 *  while considering overflow in calculation of time^3
	 * (so time^3 is done by using 64 bit)
	 * and without the support of division of 64bit numbers
	 * (so all divisions are done by using 32 bit)
	 *  also NOTE the unit of those veriables
	 *	  time  = (t - K) / 2^bictcp_HZ
	 *	c = bic_scale >> 10
	 * rtt  = (srtt >> 3) / HZ
	 * !!! The following code does not have overflow problems,
	 * if the cwnd < 1 million packets !!!
	 */

	t = (s32)(tcp_time_stamp - ca->epoch_start);
	t += msecs_to_jiffies(ca->delay_min  / USEC_PER_MSEC);
	/* change the unit from HZ to bictcp_HZ */
	t <<= BICTCP_HZ;
	do_div(t, HZ);

	if (t < ca->bic_K)		/* t - K */
		offs = ca->bic_K - t;
	else
		offs = t - ca->bic_K;

	/* c/rtt * (t-K)^3 */
	delta = (cube_rtt_scale * offs * offs * offs) >> (10+3*BICTCP_HZ);
	if (t < ca->bic_K)                            /* below origin*/
		bic_target = ca->bic_origin_point - delta;
	else                                          /* above origin*/
		bic_target = ca->bic_origin_point + delta;

	/* cubic function - calc bictcp_cnt*/
	if (bic_target > cwnd) {
		ca->cnt = cwnd / (bic_target - cwnd);
	} else {
		ca->cnt = 100 * cwnd;              /* very small increment*/
	}

	/*
	 * The initial growth of cubic function may be too conservative
	 * when the available bandwidth is still unknown.
	 */
	if (ca->last_max_cwnd == 0 && ca->cnt > 20)
		ca->cnt = 20;	/* increase cwnd 5% per RTT */

tcp_friendliness:
	/* TCP Friendly */
	if (tcp_friendliness) {
		u32 scale = beta_scale;

		delta = (cwnd * scale) >> 3;
		while (ca->ack_cnt > delta) {		/* update tcp cwnd */
			ca->ack_cnt -= delta;
			ca->tcp_cwnd++;
		}

		if (ca->tcp_cwnd > cwnd) {	/* if bic is slower than tcp */
			delta = ca->tcp_cwnd - cwnd;
			max_cnt = cwnd / delta;
			if (ca->cnt > max_cnt)
				ca->cnt = max_cnt;
		}
	}

	/*
	 * The maximum rate of cwnd increase CUBIC allows is 1 packet per
	 * 2 packets ACKed, meaning cwnd grows at 1.5x per RTT.
	 */
	ca->cnt = max(ca->cnt, 2U);
}

static void lolatcp_cong_avoid(struct sock *sk, u32 ack, u32 acked)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct lolatcp *ca = inet_csk_ca(sk);
	u32 last_cwnd;

	/*
	 * save the congestion window at the beginning of the method to determine if it has changed
	 * and whether CUBICs next value has to be calculated
	 */
	last_cwnd = tp->snd_cwnd;

	/*
	 * Try Precautionary Decongestion if TCP is not in slow start
	 */

	if (!tcp_in_slow_start(tp)) {
		lolatcp_precautionary_decongestion(sk);
		if(!ca->epoch_start)
			printk("<%u>after precog cwnd: %u flags: %u\n", ca->id, tp->snd_cwnd, ca->flags);
	}

	/*
	 * do not increase the cwnd if flow is not cwnd limited (in flight < cwnd)
	 */
	if (!tcp_is_cwnd_limited(sk) && ca->epoch_start){
		if(!ca->epoch_start || ca->flags & (LOLA_IN_CWND_HOLD | LOLA_IN_FAIR_FLOW_BALANCING))
			printk("<%u>cwnd_limited killed it cwnd:%u packets in flightt:%u RTT:%u\n", ca->id, tp->snd_cwnd, tcp_packets_in_flight(tp), ca->curr_rtt);
		
		/* 
		 * This block exits slow start and sets the cwnd to the current packets in flight.
		 * This part is important if the connection has no bottleneck or the application has not enough data
		 * 
		 * This part is experimental
		 */ 
		if(tcp_in_slow_start(tp) && (tp->snd_cwnd > 10)){	/* exit slow start if not cwnd limited */ 
			tp->snd_ssthresh = tp->snd_cwnd = tp->max_packets_out; 
			ca->epoch_start = 0; 
		}

		return;
	}

	/*
	 * This block performs the slow start
	 * It triggers a recalculation of the cubic curve, if LoLa exited slow start while applieing the acked packets
	 */
	if (tcp_in_slow_start(tp)) {
		printk("<%u>tcp in slowstart sst: %u cwnd %u RTT:%u\n", ca->id, tp->snd_ssthresh, tp->snd_cwnd, ca->curr_rtt, ca->epoch_start);
		if (hystart && after(ack, ca->end_seq))
			bictcp_hystart_reset(sk);
		acked = tcp_slow_start(tp, acked);
		if(!tcp_in_slow_start(tp))
			ca->epoch_start = 0;
		if (!acked)
			return;
	}

	/*
	 * Do cwnd increase by calculating cnt an calling tcp_cong_avoid_ai
	 */
	if(!(ca->epoch_start && (ca->flags & (LOLA_IN_CWND_HOLD | LOLA_IN_FAIR_FLOW_BALANCING)))){
		if(!ca->epoch_start){
			printk("<%u>cong_avoid called cwnd:%u epoch-start: %u flags: %u sample_cnt:%u RTT:%u\n", ca->id, tp->snd_cwnd, ca->epoch_start, ca->flags, ca->sample_cnt, ca->curr_rtt);
			ca->flags &= ~(LOLA_IN_CWND_HOLD|LOLA_IN_FAIR_FLOW_BALANCING);
		}
		bictcp_update(ca, tp->snd_cwnd, acked);
	}
	ca->cnt = max(ca->cnt, 2U);
	tcp_cong_avoid_ai(tp, ca->cnt, acked);

	/*
	 * Trigger cnt recalc in bictcp_update if cwnd changed during this function
	 */
	if(last_cwnd != tp->snd_cwnd)
		ca->last_time = 0;
}

static u32 bictcp_recalc_ssthresh(struct sock *sk)
{
	const struct tcp_sock *tp = tcp_sk(sk);
	struct lolatcp *ca = inet_csk_ca(sk);



	printk("<%u>recalcssthresh called cwnd:%u rtt:%u flags:%u\n",ca->id, tp->snd_cwnd, ca->curr_rtt, ca->flags);
	
	ca->loss_cwnd = tp->snd_cwnd;
	ca->cnt = tp->snd_cwnd *100;


	/* Wmax and fast convergence */
	if (tp->snd_cwnd < ca->last_max_cwnd && fast_convergence)
		ca->last_max_cwnd = (tp->snd_cwnd * (BICTCP_BETA_SCALE + beta))
			/ (2 * BICTCP_BETA_SCALE);
	else
		ca->last_max_cwnd = tp->snd_cwnd;

	ca->flags |= LOLA_LOSS_SAMPLING_LOCKED;
	ca->epoch_start = 0;
        ca->curr_rtt = 0;
        ca->sample_cnt = 0;
	ca->flags &= ~(LOLA_IN_CWND_HOLD | LOLA_IN_FAIR_FLOW_BALANCING);

	printk("<%u>Returning %u", ca->id, max((tp->snd_cwnd * beta) / BICTCP_BETA_SCALE, 2U));

	return max((tp->snd_cwnd * beta) / BICTCP_BETA_SCALE, 2U);
}

static u32 bictcp_undo_cwnd(struct sock *sk)
{
	struct lolatcp *ca = inet_csk_ca(sk);
	const struct tcp_sock *tp = tcp_sk(sk);

	printk("<%u>Undo cwnd called cwnd:%u, loss_window:%u, epoch_start:%u time since:%u delay_min:%u sample_cnt:%u ssthresh%u\n", ca->id, tcp_sk(sk)->snd_cwnd, ca->loss_cwnd, ca->epoch_start, tcp_time_stamp - ca->epoch_start, ca->delay_min, ca->sample_cnt, tcp_sk(sk)->snd_ssthresh);
	if ((!ca->epoch_start) || tcp_time_stamp - ca->epoch_start < (ca->delay_min / USEC_PER_MSEC)>>1){
		printk("<%u>Loss used\n", ca->id);
		ca->epoch_start = 0;
	}
	ca->flags &= ~LOLA_LOSS_SAMPLING_LOCKED;
	return max(tcp_sk(sk)->snd_cwnd, ca->loss_cwnd);
}

static void bictcp_state(struct sock *sk, u8 new_state)
{
	if (new_state == TCP_CA_Loss) {
		if(tcp_in_slow_start(tcp_sk(sk)))
			bictcp_hystart_reset(sk);
	}
}

static void hystart_update(struct sock *sk, u32 delay)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct lolatcp *ca = inet_csk_ca(sk);

	if(!ca->epoch_start) ca->epoch_start = tcp_time_stamp;

	if (ca->flags & hystart_detect)
		return;

	if (hystart_detect & HYSTART_ACK_TRAIN) {
		u32 now = bictcp_clock();

		/* first detection parameter - ack-train detection */
		if ((s32)(now - ca->last_ack) <= hystart_ack_delta) {
			ca->last_ack = now;
			if ((s32)(now - ca->epoch_start) > ca->delay_min / USEC_PER_MSEC >> 1) {
				ca->flags |= HYSTART_ACK_TRAIN;
				NET_INC_STATS(sock_net(sk),
					      LINUX_MIB_TCPHYSTARTTRAINDETECT);
				NET_ADD_STATS(sock_net(sk),
					      LINUX_MIB_TCPHYSTARTTRAINCWND,
					      tp->snd_cwnd);
				tp->snd_ssthresh = tp->snd_cwnd;
				ca->epoch_start = 0;
			}
		}
	}

	if (hystart_detect & HYSTART_DELAY) {
		/* obtain the minimum delay of more than sampling packets */
		if (ca->sample_cnt < HYSTART_MIN_SAMPLES) {
			if (ca->curr_rtt == 0 || ca->curr_rtt > delay)
				ca->curr_rtt = delay;

            ca->sample_cnt++;  // FIXME: This counts the number of acks, received in slow start (which is what we want), but sample_cnt also gets increased during bictcp_acked (we do not want this in slow start)
		} else {
			if (ca->curr_rtt > ca->delay_min + lola_slow_start_exit ) {
				ca->flags &= ~LOLA_IN_FAIR_FLOW_BALANCING;
				if(!(ca->last_max_cwnd)){
					 ca->flags|= HYSTART_DELAY;
					ca->epoch_start = 0;
				}
				NET_INC_STATS(sock_net(sk),
					      LINUX_MIB_TCPHYSTARTDELAYDETECT);
				NET_ADD_STATS(sock_net(sk),
					      LINUX_MIB_TCPHYSTARTDELAYCWND,
					      tp->snd_cwnd);
				tp->snd_ssthresh = tp->snd_cwnd;
			}
		}
	}
}

/* Track delayed acknowledgment ratio using sliding window
 * ratio = (15*ratio + sample) / 16
 */
#if LINUX_VERSION_CODE <= KERNEL_VERSION(4,7,0)
static void lolatcp_acked(struct sock *sk, u32 pkts_acked, s32 rtt_us)
{
#else
static void lolatcp_acked(struct sock *sk, const struct ack_sample *sample)
{
	u32 pkts_acked = sample->pkts_acked;
	s32 rtt_us = sample->rtt_us;
#endif
	const struct tcp_sock *tp = tcp_sk(sk);
	struct lolatcp *ca = inet_csk_ca(sk);
	u32 delay;

	/* Some calls are for duplicates without timetamps */
	//if(ca->flags & 64) printk("<%u> acked called cwnd:%u flags:%u ssth:%u epoch_start%u, delay_min:%u rtt_us:%u pckts:%u\n",ca->id, tp->snd_cwnd, ca->flags, tp->snd_ssthresh, ca->epoch_start, ca->delay_min, rtt_us, pkts_acked);
	if (rtt_us <= 0)
		return;

	/* Discard delay samples right after fast recovery */
	if ((ca->flags & LOLA_LOSS_SAMPLING_LOCKED) && (!tcp_in_slow_start(tp)) && ((!ca->epoch_start) || (s32)(tcp_time_stamp - ca->epoch_start) < ca->delay_min / USEC_PER_MSEC)){
		return;
	}

	if(unlikely(ca->flags & LOLA_LOSS_SAMPLING_LOCKED)){
		ca->flags &= ~LOLA_LOSS_SAMPLING_LOCKED;
		printk("<%u> Locked ended cwnd:%u",ca->id, tp->snd_cwnd);
	}

	delay = rtt_us;


	if(lola_mode & LOLA_DO_PRECAUTIONARY_DECONGESTION){
		if(likely(!(ca->flags & LOLA_IN_CWND_HOLD))){
			if (ca->curr_rtt == 0 || ca->curr_rtt > delay)
				ca->curr_rtt = delay;
			ca->sample_cnt += pkts_acked;
		}


	}
	/* first time call or link delay decreases */
	if ((ca->delay_min == 0 || ca->delay_min > delay)){
		if (ca->delay_min - delay > lola_base_delay_epsilon)
			ca->flags |= LOLA_BASE_REDUCED;
		ca->delay_min = delay;		
        }
        
        /* reset invalidation count if delay measurement in close proximity to measured value */
	if(delay - ca->delay_min < lola_base_delay_epsilon)		
		ca->base_invalidation_count = 0;


	/* hystart triggers when cwnd is larger than some threshold */
	if (hystart && tcp_in_slow_start(tp) &&
			tp->snd_cwnd >= hystart_low_window)
		hystart_update(sk, delay);
}

static struct tcp_congestion_ops lolatcp __read_mostly = {
	.init		= lolatcp_init,
	.ssthresh	= bictcp_recalc_ssthresh,
	.cong_avoid	= lolatcp_cong_avoid,
	.set_state	= bictcp_state,
	.undo_cwnd	= bictcp_undo_cwnd,
	.cwnd_event	= bictcp_cwnd_event,
	.pkts_acked     = lolatcp_acked,
	.owner		= THIS_MODULE,
	.name		= "lola",
};

static int __init lolatcp_register(void)
{
	BUILD_BUG_ON(sizeof(struct lolatcp) > ICSK_CA_PRIV_SIZE);

	/* Precompute a bunch of the scaling factors that are used per-packet
	 * based on SRTT of 100ms
	 */

	beta_scale = 8*(BICTCP_BETA_SCALE+beta) / 3
		/ (BICTCP_BETA_SCALE - beta);

	cube_rtt_scale = (bic_scale * 10);	/* 1024*c/rtt */

	/* calculate the "K" for (wmax-cwnd) = c/rtt * K^3
	 *  so K = cubic_root( (wmax-cwnd)*rtt/c )
	 * the unit of K is bictcp_HZ=2^10, not HZ
	 *
	 *  c = bic_scale >> 10
	 *  rtt = 100ms
	 *
	 * the following code has been designed and tested for
	 * cwnd < 1 million packets
	 * RTT < 100 seconds
	 * HZ < 1,000,00  (corresponding to 10 nano-second)
	 */

	/* 1/c * 2^2*bictcp_HZ * srtt */
	cube_factor = 1ull << (10+3*BICTCP_HZ); /* 2^40 */

	/* divide by bic_scale and by constant Srtt (100ms) */
	do_div(cube_factor, bic_scale * 10);

	return tcp_register_congestion_control(&lolatcp);
}

static void __exit lolatcp_unregister(void)
{
	tcp_unregister_congestion_control(&lolatcp);
}

module_init(lolatcp_register);
module_exit(lolatcp_unregister);

MODULE_AUTHOR("Felix Neumeister");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("LOLA TCP");
MODULE_VERSION("1.5");
