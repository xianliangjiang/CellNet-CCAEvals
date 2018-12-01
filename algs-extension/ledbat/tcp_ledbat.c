/*LEDBAT Congestion Control Algorithm
  Stefan Fisches, Mirja Kuehlewind, Uni Stuttgart/ETH

  Low Extra Delay Background Transport (LEDBAT) RFC6817
  see <https://tools.ietf.org/html/rfc6817>
*/

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h> 

#include <linux/mm.h>
#include <linux/skbuff.h>
#include <linux/inet_diag.h>
#include <net/tcp.h>
#include <linux/random.h>

#define GAIN 1 /* GAIN MUST be set to 1 or less. */
#define ALLOWED_INCREASE 1 /* ALLOWED_INCREASE SHOULD be 1, and it MUST be greater than 0 */
#define MIN_CWND 2U

#define HZ_WEIGHT 3

static int target __read_mostly = 100;
module_param(target, int, 0);
MODULE_PARM_DESC(target, "TARGET is the maximum queueing delay that LEDBAT itself may introduce in the network.");

/* Current_FILTER SHOULD be 1;
 * it MAY be tuned so that it is at least 1 and no more than cwnd/2 
 */
static int current_filter __read_mostly = 2; 
module_param(current_filter, int, 0);
MODULE_PARM_DESC(current_filter, "Maintain a list of CURRENT_FILTER last delays observed.");

/* BASE_HISTORY SHOULD be 2;
 * it MUST be no less than 2 and SHOULD NOT be more than 10 
 */
static int base_history __read_mostly = 2;
module_param(base_history, int, 0);
MODULE_PARM_DESC(base_history, "Maintain BASE_HISTORY delay-minima where each minimum is measured over a period of a minute.");


struct ledbat_list {
	u32 *buffer;
	u8 next;
	u8 len;
};
   
/* ledbat structure */
struct ledbat {
  u32 base_delay;
  s32 cwnd_cnt;

  struct ledbat_list current_delays;
  struct ledbat_list base_delays; 

  u32 last_rollover; /* This is to be interpreted as time */
  
  u32 remote_hz;
  u32 last_local_ts;
  u32 last_remote_ts;
  u32 local_time_offset;
  u32 remote_time_offset;
  
};


static void ledbat_init_list(struct ledbat_list *list, int len)
{
	list->buffer = kmalloc( len * sizeof(u32), GFP_KERNEL);
        if (list->buffer !=  NULL) {
           int i;
           for (i=0; i<len; i++)
             list->buffer[i] = UINT_MAX;
        }
	list->len = len;
	list->next = 0;
}


static void tcp_ledbat_init(struct sock *sk){  

  struct ledbat *ledbat = inet_csk_ca(sk);

  ledbat->base_delay = UINT_MAX;
  ledbat->cwnd_cnt = 0; 

  ledbat_init_list(&ledbat->current_delays, current_filter);
  ledbat_init_list(&ledbat->base_delays, base_history);

  ledbat->last_rollover = 0;

  ledbat->local_time_offset = 0;
  ledbat->remote_time_offset = 0;
  ledbat->last_local_ts=0;
  ledbat->last_remote_ts=0;
  ledbat->remote_hz = HZ;

}

static void tcp_ledbat_release(struct sock *sk){

  struct ledbat *ledbat = inet_csk_ca(sk);
  kfree(ledbat->current_delays.buffer);
  kfree(ledbat->base_delays.buffer);

}

void tcp_ledbat_update_current_delay(struct sock *sk, int delay){

  struct ledbat *ledbat = inet_csk_ca(sk);

  /* Maintain a list of CURRENT_FILTER last delays observed. */
  /* delete first item in current_delays list
   * append delay to current_delays list
   */

  ledbat->current_delays.buffer[ledbat->current_delays.next] = delay;
  ledbat->current_delays.next++; 
  if (ledbat->current_delays.next == current_filter)
     ledbat->current_delays.next=0; 
 
}

void tcp_ledbat_update_base_delay(struct sock *sk, u32 delay) {

  struct ledbat *ledbat = inet_csk_ca(sk);

  /* Maintain BASE_HISTORY min delays. Each represents a minute.*/
  /* if round_to_minute(now) != round_to_minute(last_rollover)
   *   last_rollover = now
   *   forget the earliest of base delays
   *   add delay to the end of base_delays
   * else
   *   last of base_delays = min(last of base_delays, delay)
   */

  if (get_seconds() >= ledbat->last_rollover + 60) { 
     ledbat->last_rollover = get_seconds(); 
     ledbat->base_delays.next++; 
     if (ledbat->base_delays.next == base_history)
        ledbat->base_delays.next=0; 
     ledbat->base_delays.buffer[ledbat->base_delays.next] = delay;
  } else {
     ledbat->base_delays.buffer[ledbat->base_delays.next] = 
          min(ledbat->base_delays.buffer[ledbat->base_delays.next], delay);
  }

}

u32 tcp_ledbat_get_min_from_list(struct ledbat_list *list) {  

  u32 min_delay = UINT_MAX;
  int i;
  for (i=0; i<list->len; i++) {
     min_delay = min(list->buffer[i], min_delay);
  }
  return min_delay;

}

static inline u32 time_in_ms(void)
{
#if HZ < 1000
	return ktime_to_ms(ktime_get_real());
#else
	return jiffies_to_msecs(jiffies);
#endif
}

/* curently not used because doesn't work correctly 
 * as you should only estimate the HZ is there is no queuing delay...
 */
static void estimate_remote_HZ(struct sock *sk){

   struct tcp_sock *tp = tcp_sk(sk);
   struct ledbat *ledbat = inet_csk_ca(sk);

   int local_delta = 1;
   int remote_delta = 1;
 
    //check which one has the higher delta -> this is the fine grained clock
    //we know our own clock granularity
    //HZ_remote= remote_delta * HZ_local /our_delta
   if (ledbat->last_remote_ts!=0 && tp->rx_opt.rcv_tsval != ledbat->last_remote_ts 
	&& ledbat->last_local_ts!=0 && tp->rx_opt.rcv_tsecr != ledbat->last_local_ts){
      remote_delta = tp->rx_opt.rcv_tsval - ledbat->last_remote_ts;
      local_delta = tp->rx_opt.rcv_tsecr - ledbat->last_local_ts;

      u32 tmp_remote_hz = HZ * (remote_delta) / (local_delta);

      ledbat->remote_hz = ledbat->remote_hz - (ledbat->remote_hz >> HZ_WEIGHT) +
                          (tmp_remote_hz >> HZ_WEIGHT);
   }

   //remember last HZ value for remote and local
   ledbat->last_remote_ts = tp->rx_opt.rcv_tsval;
   ledbat->last_local_ts = tp->rx_opt.rcv_tsecr;
}

void tcp_ledbat_cong_avoid(struct sock *sk, u32 ack, u32 acked) {

   struct tcp_sock *tp = tcp_sk(sk);  
   struct ledbat *ledbat = inet_csk_ca(sk);

   u32 delay = 0;
   u32 queuing_delay;
   int off_target;
   u32 cwnd;
   u32 max_allowed_cwnd;

   //estimate the remote peers time granularity -> doesn't work and therefore not used
   //estimate_remote_HZ(sk);

   // remember first timestamp of local and remote host as base
   if (ledbat->remote_time_offset == 0)
     ledbat->remote_time_offset = tp->rx_opt.rcv_tsval;
   if (ledbat->local_time_offset == 0)
     ledbat->local_time_offset = tp->rx_opt.rcv_tsecr;
   
   //calculate current OWD
   //delay * 1000 * 1/HZ; -> Result in [s]. Multiply by 1000 for [ms]
   u32 time = (tp->rx_opt.rcv_tsval - ledbat->remote_time_offset)*1000/ledbat->remote_hz;
   u32 remote_time = (tp->rx_opt.rcv_tsecr - ledbat->local_time_offset)*1000/HZ;
   if (time > remote_time)
      delay = time - remote_time;
   
   // update delays
   tcp_ledbat_update_base_delay(sk, delay);
   tcp_ledbat_update_current_delay(sk, delay);
   ledbat->base_delay = min(ledbat->base_delay, delay);

   // calculate queuing delay
   if (ledbat->current_delays.buffer!=NULL && ledbat->base_delays.buffer!=NULL) {
      queuing_delay = tcp_ledbat_get_min_from_list(&ledbat->current_delays) - 
   	   tcp_ledbat_get_min_from_list(&ledbat->base_delays);
   } else {
      queuing_delay = delay - ledbat->base_delay;
   }

   /* don't change cwnd is not cwnd-limited */
   if (!tcp_is_cwnd_limited(sk))
	return;

   /* In "safe" area, increase exponentially. */
   if (tp->snd_cwnd <= tp->snd_ssthresh) {
	acked = tcp_slow_start(tp, acked);
	if (!acked)
	   return;
   }

   /* LEDABT cwnd increase/decrease */
   cwnd = tp->snd_cwnd;
   off_target = target - queuing_delay;
   ledbat->cwnd_cnt += GAIN * off_target * acked;
   if (abs(ledbat->cwnd_cnt) >= tp->snd_cwnd*target) {
      long inc =  ledbat->cwnd_cnt/((long)target)/((long)tp->snd_cwnd);
      cwnd += inc;
      ledbat->cwnd_cnt -= inc*tp->snd_cwnd*target;
   }

   // From RFC6817: max_allowed_cwnd = flightsize + ALLOWED_INCREASE * MSS
   max_allowed_cwnd = tp->packets_out + acked + ALLOWED_INCREASE;
   cwnd = min(cwnd, max_allowed_cwnd); 
   // or
   // cwnd = max(MIN_CWND, min(cwnd, tp->snd_cwnd_clamp));

   // set cwnd
   tp->snd_cwnd = max(MIN_CWND, cwnd);

   // also adapt ssthreash if the cwnd is reduced!
   if (tp->snd_cwnd <= tp->snd_ssthresh)
      tp->snd_ssthresh = tp->snd_cwnd-1;
}
EXPORT_SYMBOL_GPL(tcp_ledbat_cong_avoid);


static struct tcp_congestion_ops tcp_ledbat = {
  .init = tcp_ledbat_init,
  .ssthresh = tcp_reno_ssthresh,
  .release = tcp_ledbat_release,
  .cong_avoid = tcp_ledbat_cong_avoid,
  .name = "ledbat",
};
  
static int __init tcp_ledbat_register(void){
  tcp_register_congestion_control(&tcp_ledbat);
}

static void __exit tcp_ledbat_unregister(void){
  tcp_unregister_congestion_control(&tcp_ledbat);
}

module_init(tcp_ledbat_register);
module_exit(tcp_ledbat_unregister);

MODULE_AUTHOR("Mirja Kuehlewind");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("TCP LEDBAT");
MODULE_VERSION("0.3");


