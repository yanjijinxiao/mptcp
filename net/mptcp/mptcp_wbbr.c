/* Bottleneck Bandwidth and RTT (BBR) congestion control
 *
 * BBR congestion control computes the sending rate based on the delivery
 * rate (throughput) estimated from ACKs. In a nutshell:
 *
 *   On each ACK, update our model of the network path:
 *      bottleneck_bandwidth = windowed_max(delivered / elapsed, 10 round trips)
 *      min_rtt = windowed_min(rtt, 10 seconds)
 *   pacing_rate = pacing_gain * bottleneck_bandwidth
 *   cwnd = max(cwnd_gain * bottleneck_bandwidth * min_rtt, 4)
 *
 * The core algorithm does not react directly to packet losses or delays,
 * although BBR may adjust the size of next send per ACK when loss is
 * observed, or adjust the sending rate if it estimates there is a
 * traffic policer, in order to keep the drop rate reasonable.
 *
 * BBR is described in detail in:
 *   "BBR: Congestion-Based Congestion Control",
 *   Neal Cardwell, Yuchung Cheng, C. Stephen Gunn, Soheil Hassas Yeganeh,
 *   Van Jacobson. ACM Queue, Vol. 14 No. 5, September-October 2016.
 *
 * There is a public e-mail list for discussing BBR development and testing:
 *   https://groups.google.com/forum/#!forum/bbr-dev
 *
 * NOTE: BBR *must* be used with the fq qdisc ("man tc-fq") with pacing enabled,
 * since pacing is integral to the BBR design and implementation.
 * BBR without pacing would not function properly, and may incur unnecessary
 * high packet loss rates.
 */
#include <linux/module.h>
#include <net/tcp.h>
#include <net/mptcp.h>
#include <linux/inet_diag.h>
#include <linux/inet.h>
#include <linux/random.h>
#include <linux/win_minmax.h>

/* Scale factor for rate in pkt/uSec unit to avoid truncation in bandwidth
 * estimation. The rate unit ~= (1500 bytes / 1 usec / 2^24) ~= 715 bps.
 * This handles bandwidths from 0.06pps (715bps) to 256Mpps (3Tbps) in a u32.
 * Since the minimum window is >=4 packets, the lower bound isn't
 * an issue. The upper bound isn't an issue with existing technologies.
 */
#define BW_SCALE 24
#define BW_UNIT (1 << BW_SCALE)

#define WBBR_SCALE 8	/* scaling factor for fractions in WBBR (e.g. gains) */
#define WBBR_UNIT (1 << WBBR_SCALE)

/* WBBR has the following modes for deciding how fast to send: */
enum wbbr_mode {
	WBBR_STARTUP,	/* ramp up sending rate rapidly to fill pipe */
	WBBR_DRAIN,	/* drain any queue created during startup */
	WBBR_PROBE_BW,	/* discover, share bw: pace around estimated bw */
	WBBR_PROBE_RTT,	/* cut cwnd to min to probe min_rtt */
};

/* WBBR congestion control block */
struct wbbr {
	u32	min_rtt_us;	        /* min RTT in min_rtt_win_sec window */
	u32	min_rtt_stamp;	        /* timestamp of min_rtt_us */
	u32	probe_rtt_done_stamp;   /* end time for WBBR_PROBE_RTT mode */
	struct minmax bw;	/* Max recent delivery rate in pkts/uS << 24 */
	u32	rtt_cnt;	    /* count of packet-timed rounds elapsed */
	u32     next_rtt_delivered; /* scb->tx.delivered at end of round */
	struct skb_mstamp cycle_mstamp;  /* time of this cycle phase start */
	u32     mode:3,		     /* current wbbr_mode in state machine */
		prev_ca_state:3,     /* CA state on previous ACK */
		packet_conservation:1,  /* use packet conservation? */
		restore_cwnd:1,	     /* decided to revert cwnd to old value */
		round_start:1,	     /* start of packet-timed tx->ack round? */
		tso_segs_goal:7,     /* segments we want in each skb we send */
		idle_restart:1,	     /* restarting after idle? */
		probe_rtt_round_done:1,  /* a WBBR_PROBE_RTT round at 4 pkts? */
		unused:5,
		lt_is_sampling:1,    /* taking long-term ("LT") samples now? */
		lt_rtt_cnt:7,	     /* round trips in long-term interval */
		lt_use_bw:1;	     /* use lt_bw as our bw estimate? */
	u32	lt_bw;		     /* LT est delivery rate in pkts/uS << 24 */
	u32	lt_last_delivered;   /* LT intvl start: tp->delivered */
	u32	lt_last_stamp;	     /* LT intvl start: tp->delivered_mstamp */
	u32	lt_last_lost;	     /* LT intvl start: tp->lost */
	u32	pacing_gain:10,	/* current gain for setting pacing rate */
		cwnd_gain:10,	/* current gain for setting cwnd */
		full_bw_cnt:3,	/* number of rounds without large bw gains */
		cycle_idx:3,	/* current index in pacing_gain cycle array */
		has_seen_rtt:1, /* have we seen an RTT sample yet? */
		unused_b:5;
	u32	prior_cwnd;	/* prior cwnd upon entering loss recovery */
	u32	full_bw;	/* recent bw, to estimate if pipe is full */
    u32 instant_rate;
};

#define CYCLE_LEN	8	/* number of phases in a pacing gain cycle */

/* Window length of bw filter (in rounds): */
static const int wbbr_bw_rtts = CYCLE_LEN + 2;
/* Window length of min_rtt filter (in sec): */
static const u32 wbbr_min_rtt_win_sec = 10;
/* Minimum time (in ms) spent at wbbr_cwnd_min_target in WBBR_PROBE_RTT mode: */
static const u32 wbbr_probe_rtt_mode_ms = 200;
/* Skip TSO below the following bandwidth (bits/sec): */
static const int wbbr_min_tso_rate = 1200000;

/* We use a high_gain value of 2/ln(2) because it's the smallest pacing gain
 * that will allow a smoothly increasing pacing rate that will double each RTT
 * and send the same number of packets per RTT that an un-paced, slow-starting
 * Reno or CUBIC flow would:
 */
static const int wbbr_high_gain  = WBBR_UNIT * 2885 / 1000 + 1;
/* The pacing gain of 1/high_gain in WBBR_DRAIN is calculated to typically drain
 * the queue created in WBBR_STARTUP in a single round:
 */
static const int wbbr_drain_gain = WBBR_UNIT * 1000 / 2885;
/* The gain for deriving steady-state cwnd tolerates delayed/stretched ACKs: */
static const int wbbr_cwnd_gain  = WBBR_UNIT * 2;
/* The pacing_gain values for the PROBE_BW gain cycle, to discover/share bw: */
static const int wbbr_pacing_gain[] = {
	WBBR_UNIT * 5 / 4,	/* probe for more available bw */
	WBBR_UNIT * 3 / 4,	/* drain queue and/or yield bw to other flows */
	WBBR_UNIT, WBBR_UNIT, WBBR_UNIT,	/* cruise at 1.0*bw to utilize pipe, */
	WBBR_UNIT, WBBR_UNIT, WBBR_UNIT	/* without creating excess queue... */
};
/* Randomize the starting gain cycling phase over N phases: */
static const u32 wbbr_cycle_rand = 7;

/* Try to keep at least this many packets in flight, if things go smoothly. For
 * smooth functioning, a sliding window protocol ACKing every other packet
 * needs at least 4 packets in flight:
 */
static const u32 wbbr_cwnd_min_target = 4;

/* To estimate if WBBR_STARTUP mode (i.e. high_gain) has filled pipe... */
/* If bw has increased significantly (1.25x), there may be more bw available: */
static const u32 wbbr_full_bw_thresh = WBBR_UNIT * 5 / 4;
/* But after 3 rounds w/o significant bw growth, estimate pipe is full: */
static const u32 wbbr_full_bw_cnt = 3;

/* "long-term" ("LT") bandwidth estimator parameters... */
/* The minimum number of rounds in an LT bw sampling interval: */
static const u32 wbbr_lt_intvl_min_rtts = 4;
/* If lost/delivered ratio > 20%, interval is "lossy" and we may be policed: */
static const u32 wbbr_lt_loss_thresh = 50;
/* If 2 intervals have a bw ratio <= 1/8, their bw is "consistent": */
static const u32 wbbr_lt_bw_ratio = WBBR_UNIT / 8;
/* If 2 intervals have a bw diff <= 4 Kbit/sec their bw is "consistent": */
static const u32 wbbr_lt_bw_diff = 4000 / 8;
/* If we estimate we're policed, use lt_bw for this many round trips: */
static const u32 wbbr_lt_bw_max_rtts = 48;

/* Do we estimate that STARTUP filled the pipe? */
static bool wbbr_full_bw_reached(const struct sock *sk)
{
	const struct wbbr *wbbr = inet_csk_ca(sk);

	return wbbr->full_bw_cnt >= wbbr_full_bw_cnt;
}

/* Return the windowed max recent bandwidth sample, in pkts/uS << BW_SCALE. */
static u32 wbbr_max_bw(const struct sock *sk)
{
	struct wbbr *wbbr = inet_csk_ca(sk);

	return minmax_get(&wbbr->bw);
}

/* Return the estimated bandwidth of the path, in pkts/uS << BW_SCALE. */
static u32 wbbr_bw(const struct sock *sk)
{
	struct wbbr *wbbr = inet_csk_ca(sk);

	return wbbr->lt_use_bw ? wbbr->lt_bw : wbbr_max_bw(sk);
}

/* Return rate in bytes per second, optionally with a gain.
 * The order here is chosen carefully to avoid overflow of u64. This should
 * work for input rates of up to 2.9Tbit/sec and gain of 2.89x.
 */
static u64 wbbr_rate_bytes_per_sec(struct sock *sk, u64 rate, int gain)
{
	rate *= tcp_mss_to_mtu(sk, tcp_sk(sk)->mss_cache);
	rate *= gain;
	rate >>= WBBR_SCALE;
	rate *= USEC_PER_SEC;
	return rate >> BW_SCALE;
}

/* Convert a WBBR bw and gain factor to a pacing rate in bytes per second. */
static u32 wbbr_bw_to_pacing_rate(struct sock *sk, u32 bw, int gain)
{
	u64 rate = bw;

	rate = wbbr_rate_bytes_per_sec(sk, rate, gain);
	rate = min_t(u64, rate, sk->sk_max_pacing_rate);
	return rate;
}

/* Initialize pacing rate to: high_gain * init_cwnd / RTT. */
static void wbbr_init_pacing_rate_from_rtt(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct wbbr *wbbr = inet_csk_ca(sk);
	u64 bw;
	u32 rtt_us;

	if (tp->srtt_us) {		/* any RTT sample yet? */
		rtt_us = max(tp->srtt_us >> 3, 1U);
		wbbr->has_seen_rtt = 1;
	} else {			 /* no RTT sample yet */
		rtt_us = USEC_PER_MSEC;	 /* use nominal default RTT */
	}
	bw = (u64)tp->snd_cwnd * BW_UNIT;
	do_div(bw, rtt_us);
	sk->sk_pacing_rate = wbbr_bw_to_pacing_rate(sk, bw, wbbr_high_gain);
}

/* Pace using current bw estimate and a gain factor. In order to help drive the
 * network toward lower queues while maintaining high utilization and low
 * latency, the average pacing rate aims to be slightly (~1%) lower than the
 * estimated bandwidth. This is an important aspect of the design. In this
 * implementation this slightly lower pacing rate is achieved implicitly by not
 * including link-layer headers in the packet size used for the pacing rate.
 */
static void wbbr_set_pacing_rate(struct sock *sk, u32 bw, int gain)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct wbbr *wbbr = inet_csk_ca(sk);
	u32 rate = wbbr_bw_to_pacing_rate(sk, bw, gain);

	if (unlikely(!wbbr->has_seen_rtt && tp->srtt_us))
		wbbr_init_pacing_rate_from_rtt(sk);
	if (wbbr_full_bw_reached(sk) || rate > sk->sk_pacing_rate)
		sk->sk_pacing_rate = rate;
}

/* Return count of segments we want in the skbs we send, or 0 for default. */
static u32 wbbr_tso_segs_goal(struct sock *sk)
{
	struct wbbr *wbbr = inet_csk_ca(sk);

	return wbbr->tso_segs_goal;
}

static void wbbr_set_tso_segs_goal(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct wbbr *wbbr = inet_csk_ca(sk);
	u32 min_segs;

	min_segs = sk->sk_pacing_rate < (wbbr_min_tso_rate >> 3) ? 1 : 2;
	wbbr->tso_segs_goal = min(tcp_tso_autosize(sk, tp->mss_cache, min_segs),
				 0x7FU);
}

/* Save "last known good" cwnd so we can restore it after losses or PROBE_RTT */
static void wbbr_save_cwnd(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct wbbr *wbbr = inet_csk_ca(sk);

	if (wbbr->prev_ca_state < TCP_CA_Recovery && wbbr->mode != WBBR_PROBE_RTT)
		wbbr->prior_cwnd = tp->snd_cwnd;  /* this cwnd is good enough */
	else  /* loss recovery or WBBR_PROBE_RTT have temporarily cut cwnd */
		wbbr->prior_cwnd = max(wbbr->prior_cwnd, tp->snd_cwnd);
}

static void wbbr_cwnd_event(struct sock *sk, enum tcp_ca_event event)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct wbbr *wbbr = inet_csk_ca(sk);

	if (event == CA_EVENT_TX_START && tp->app_limited) {
		wbbr->idle_restart = 1;
		/* Avoid pointless buffer overflows: pace at est. bw if we don't
		 * need more speed (we're restarting from idle and app-limited).
		 */
		if (wbbr->mode == WBBR_PROBE_BW)
			wbbr_set_pacing_rate(sk, wbbr_bw(sk), WBBR_UNIT);
	}
}

/* Find target cwnd. Right-size the cwnd based on min RTT and the
 * estimated bottleneck bandwidth:
 *
 * cwnd = bw * min_rtt * gain = BDP * gain
 *
 * The key factor, gain, controls the amount of queue. While a small gain
 * builds a smaller queue, it becomes more vulnerable to noise in RTT
 * measurements (e.g., delayed ACKs or other ACK compression effects). This
 * noise may cause WBBR to under-estimate the rate.
 *
 * To achieve full performance in high-speed paths, we budget enough cwnd to
 * fit full-sized skbs in-flight on both end hosts to fully utilize the path:
 *   - one skb in sending host Qdisc,
 *   - one skb in sending host TSO/GSO engine
 *   - one skb being received by receiver host LRO/GRO/delayed-ACK engine
 * Don't worry, at low rates (wbbr_min_tso_rate) this won't bloat cwnd because
 * in such cases tso_segs_goal is 1. The minimum cwnd is 4 packets,
 * which allows 2 outstanding 2-packet sequences, to try to keep pipe
 * full even with ACK-every-other-packet delayed ACKs.
 */
static u32 wbbr_target_cwnd(struct sock *sk, u32 bw, int gain)
{
	struct wbbr *wbbr = inet_csk_ca(sk);
	u32 cwnd;
	u64 w;

	/* If we've never had a valid RTT sample, cap cwnd at the initial
	 * default. This should only happen when the connection is not using TCP
	 * timestamps and has retransmitted all of the SYN/SYNACK/data packets
	 * ACKed so far. In this case, an RTO can cut cwnd to 1, in which
	 * case we need to slow-start up toward something safe: TCP_INIT_CWND.
	 */
	if (unlikely(wbbr->min_rtt_us == ~0U))	 /* no valid RTT samples yet? */
		return TCP_INIT_CWND;  /* be safe: cap at default initial cwnd*/

	w = (u64)bw * wbbr->min_rtt_us;

	/* Apply a gain to the given value, then remove the BW_SCALE shift. */
	cwnd = (((w * gain) >> WBBR_SCALE) + BW_UNIT - 1) / BW_UNIT;

	/* Allow enough full-sized skbs in flight to utilize end systems. */
	cwnd += 3 * wbbr->tso_segs_goal;

	/* Reduce delayed ACKs by rounding up cwnd to the next even number. */
	cwnd = (cwnd + 1) & ~1U;

	return cwnd;
}

/* An optimization in WBBR to reduce losses: On the first round of recovery, we
 * follow the packet conservation principle: send P packets per P packets acked.
 * After that, we slow-start and send at most 2*P packets per P packets acked.
 * After recovery finishes, or upon undo, we restore the cwnd we had when
 * recovery started (capped by the target cwnd based on estimated BDP).
 *
 * TODO(ycheng/ncardwell): implement a rate-based approach.
 */
static bool wbbr_set_cwnd_to_recover_or_restore(
	struct sock *sk, const struct rate_sample *rs, u32 acked, u32 *new_cwnd)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct wbbr *wbbr = inet_csk_ca(sk);
	u8 prev_state = wbbr->prev_ca_state, state = inet_csk(sk)->icsk_ca_state;
	u32 cwnd = tp->snd_cwnd;

	/* An ACK for P pkts should release at most 2*P packets. We do this
	 * in two steps. First, here we deduct the number of lost packets.
	 * Then, in wbbr_set_cwnd() we slow start up toward the target cwnd.
	 */
	if (rs->losses > 0)
		cwnd = max_t(s32, cwnd - rs->losses, 1);

	if (state == TCP_CA_Recovery && prev_state != TCP_CA_Recovery) {
		/* Starting 1st round of Recovery, so do packet conservation. */
		wbbr->packet_conservation = 1;
		wbbr->next_rtt_delivered = tp->delivered;  /* start round now */
		/* Cut unused cwnd from app behavior, TSQ, or TSO deferral: */
		cwnd = tcp_packets_in_flight(tp) + acked;
	} else if (prev_state >= TCP_CA_Recovery && state < TCP_CA_Recovery) {
		/* Exiting loss recovery; restore cwnd saved before recovery. */
		wbbr->restore_cwnd = 1;
		wbbr->packet_conservation = 0;
	}
	wbbr->prev_ca_state = state;

	if (wbbr->restore_cwnd) {
		/* Restore cwnd after exiting loss recovery or PROBE_RTT. */
		cwnd = max(cwnd, wbbr->prior_cwnd);
		wbbr->restore_cwnd = 0;
	}

	if (wbbr->packet_conservation) {
		*new_cwnd = max(cwnd, tcp_packets_in_flight(tp) + acked);
		return true;	/* yes, using packet conservation */
	}
	*new_cwnd = cwnd;
	return false;
}

/* Slow-start up toward target cwnd (if bw estimate is growing, or packet loss
 * has drawn us down below target), or snap down to target if we're above it.
 */
static void wbbr_set_cwnd(struct sock *sk, const struct rate_sample *rs,
			 u32 acked, u32 bw, int gain)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct wbbr *wbbr = inet_csk_ca(sk);
	u32 cwnd = 0, target_cwnd = 0;

	if (!acked)
		return;

	if (wbbr_set_cwnd_to_recover_or_restore(sk, rs, acked, &cwnd))
		goto done;

	/* If we're below target cwnd, slow start cwnd toward target cwnd. */
	target_cwnd = wbbr_target_cwnd(sk, bw, gain);
	if (wbbr_full_bw_reached(sk))  /* only cut cwnd if we filled the pipe */
		cwnd = min(cwnd + acked, target_cwnd);
	else if (cwnd < target_cwnd || tp->delivered < TCP_INIT_CWND)
		cwnd = cwnd + acked;
	cwnd = max(cwnd, wbbr_cwnd_min_target);

done:
	tp->snd_cwnd = min(cwnd, tp->snd_cwnd_clamp);	/* apply global cap */
	if (wbbr->mode == WBBR_PROBE_RTT)  /* drain queue, refresh min_rtt */
		tp->snd_cwnd = min(tp->snd_cwnd, wbbr_cwnd_min_target);
}

/* End cycle phase if it's time and/or we hit the phase's in-flight target. */
static bool wbbr_is_next_cycle_phase(struct sock *sk,
				    const struct rate_sample *rs)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct wbbr *wbbr = inet_csk_ca(sk);
	bool is_full_length =
		skb_mstamp_us_delta(&tp->delivered_mstamp, &wbbr->cycle_mstamp) >
		wbbr->min_rtt_us;
	u32 inflight, bw;

	/* The pacing_gain of 1.0 paces at the estimated bw to try to fully
	 * use the pipe without increasing the queue.
	 */
	if (wbbr->pacing_gain == WBBR_UNIT)
		return is_full_length;		/* just use wall clock time */

	inflight = rs->prior_in_flight;  /* what was in-flight before ACK? */
	bw = wbbr_max_bw(sk);

	/* A pacing_gain > 1.0 probes for bw by trying to raise inflight to at
	 * least pacing_gain*BDP; this may take more than min_rtt if min_rtt is
	 * small (e.g. on a LAN). We do not persist if packets are lost, since
	 * a path with small buffers may not hold that much.
	 */
	if (wbbr->pacing_gain > WBBR_UNIT)
		return is_full_length &&
			(rs->losses ||  /* perhaps pacing_gain*BDP won't fit */
			 inflight >= wbbr_target_cwnd(sk, bw, wbbr->pacing_gain));

	/* A pacing_gain < 1.0 tries to drain extra queue we added if bw
	 * probing didn't find more bw. If inflight falls to match BDP then we
	 * estimate queue is drained; persisting would underutilize the pipe.
	 */
	return is_full_length ||
		inflight <= wbbr_target_cwnd(sk, bw, WBBR_UNIT);
}

static void wbbr_advance_cycle_phase(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct wbbr *wbbr = inet_csk_ca(sk);

	wbbr->cycle_idx = (wbbr->cycle_idx + 1) & (CYCLE_LEN - 1);
	wbbr->cycle_mstamp = tp->delivered_mstamp;
	wbbr->pacing_gain = wbbr_pacing_gain[wbbr->cycle_idx];
}

/* Gain cycling: cycle pacing gain to converge to fair share of available bw. */
static void wbbr_update_cycle_phase(struct sock *sk,
				   const struct rate_sample *rs)
{
	struct wbbr *wbbr = inet_csk_ca(sk);

	if ((wbbr->mode == WBBR_PROBE_BW) && !wbbr->lt_use_bw &&
	    wbbr_is_next_cycle_phase(sk, rs))
		wbbr_advance_cycle_phase(sk);
}

static void wbbr_reset_startup_mode(struct sock *sk)
{
	struct wbbr *wbbr = inet_csk_ca(sk);

	wbbr->mode = WBBR_STARTUP;
	wbbr->pacing_gain = wbbr_high_gain;
	wbbr->cwnd_gain	 = wbbr_high_gain;
}

static void wbbr_reset_probe_bw_mode(struct sock *sk)
{
	struct wbbr *wbbr = inet_csk_ca(sk);

	wbbr->mode = WBBR_PROBE_BW;
	wbbr->pacing_gain = WBBR_UNIT;
	wbbr->cwnd_gain = wbbr_cwnd_gain;
	wbbr->cycle_idx = CYCLE_LEN - 1 - prandom_u32_max(wbbr_cycle_rand);
	wbbr_advance_cycle_phase(sk);	/* flip to next phase of gain cycle */
}

static void wbbr_reset_mode(struct sock *sk)
{
	if (!wbbr_full_bw_reached(sk))
		wbbr_reset_startup_mode(sk);
	else
		wbbr_reset_probe_bw_mode(sk);
}

/* Start a new long-term sampling interval. */
static void wbbr_reset_lt_bw_sampling_interval(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct wbbr *wbbr = inet_csk_ca(sk);

	wbbr->lt_last_stamp = tp->delivered_mstamp.stamp_jiffies;
	wbbr->lt_last_delivered = tp->delivered;
	wbbr->lt_last_lost = tp->lost;
	wbbr->lt_rtt_cnt = 0;
}

/* Completely reset long-term bandwidth sampling. */
static void wbbr_reset_lt_bw_sampling(struct sock *sk)
{
	struct wbbr *wbbr = inet_csk_ca(sk);

	wbbr->lt_bw = 0;
	wbbr->lt_use_bw = 0;
	wbbr->lt_is_sampling = false;
	wbbr_reset_lt_bw_sampling_interval(sk);
}

/* Long-term bw sampling interval is done. Estimate whether we're policed. */
static void wbbr_lt_bw_interval_done(struct sock *sk, u32 bw)
{
	struct wbbr *wbbr = inet_csk_ca(sk);
	u32 diff;

	if (wbbr->lt_bw) {  /* do we have bw from a previous interval? */
		/* Is new bw close to the lt_bw from the previous interval? */
		diff = abs(bw - wbbr->lt_bw);
		if ((diff * WBBR_UNIT <= wbbr_lt_bw_ratio * wbbr->lt_bw) ||
		    (wbbr_rate_bytes_per_sec(sk, diff, WBBR_UNIT) <=
		     wbbr_lt_bw_diff)) {
			/* All criteria are met; estimate we're policed. */
			wbbr->lt_bw = (bw + wbbr->lt_bw) >> 1;  /* avg 2 intvls */
			wbbr->lt_use_bw = 1;
			wbbr->pacing_gain = WBBR_UNIT;  /* try to avoid drops */
			wbbr->lt_rtt_cnt = 0;
			return;
		}
	}
	wbbr->lt_bw = bw;
	wbbr_reset_lt_bw_sampling_interval(sk);
}

/* Token-bucket traffic policers are common (see "An Internet-Wide Analysis of
 * Traffic Policing", SIGCOMM 2016). WBBR detects token-bucket policers and
 * explicitly models their policed rate, to reduce unnecessary losses. We
 * estimate that we're policed if we see 2 consecutive sampling intervals with
 * consistent throughput and high packet loss. If we think we're being policed,
 * set lt_bw to the "long-term" average delivery rate from those 2 intervals.
 */
static void wbbr_lt_bw_sampling(struct sock *sk, const struct rate_sample *rs)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct wbbr *wbbr = inet_csk_ca(sk);
	u32 lost, delivered;
	u64 bw;
	s32 t;

	if (wbbr->lt_use_bw) {	/* already using long-term rate, lt_bw? */
		if (wbbr->mode == WBBR_PROBE_BW && wbbr->round_start &&
		    ++wbbr->lt_rtt_cnt >= wbbr_lt_bw_max_rtts) {
			wbbr_reset_lt_bw_sampling(sk);    /* stop using lt_bw */
			wbbr_reset_probe_bw_mode(sk);  /* restart gain cycling */
		}
		return;
	}

	/* Wait for the first loss before sampling, to let the policer exhaust
	 * its tokens and estimate the steady-state rate allowed by the policer.
	 * Starting samples earlier includes bursts that over-estimate the bw.
	 */
	if (!wbbr->lt_is_sampling) {
		if (!rs->losses)
			return;
		wbbr_reset_lt_bw_sampling_interval(sk);
		wbbr->lt_is_sampling = true;
	}

	/* To avoid underestimates, reset sampling if we run out of data. */
	if (rs->is_app_limited) {
		wbbr_reset_lt_bw_sampling(sk);
		return;
	}

	if (wbbr->round_start)
		wbbr->lt_rtt_cnt++;	/* count round trips in this interval */
	if (wbbr->lt_rtt_cnt < wbbr_lt_intvl_min_rtts)
		return;		/* sampling interval needs to be longer */
	if (wbbr->lt_rtt_cnt > 4 * wbbr_lt_intvl_min_rtts) {
		wbbr_reset_lt_bw_sampling(sk);  /* interval is too long */
		return;
	}

	/* End sampling interval when a packet is lost, so we estimate the
	 * policer tokens were exhausted. Stopping the sampling before the
	 * tokens are exhausted under-estimates the policed rate.
	 */
	if (!rs->losses)
		return;

	/* Calculate packets lost and delivered in sampling interval. */
	lost = tp->lost - wbbr->lt_last_lost;
	delivered = tp->delivered - wbbr->lt_last_delivered;
	/* Is loss rate (lost/delivered) >= lt_loss_thresh? If not, wait. */
	if (!delivered || (lost << WBBR_SCALE) < wbbr_lt_loss_thresh * delivered)
		return;

	/* Find average delivery rate in this sampling interval. */
	t = (s32)(tp->delivered_mstamp.stamp_jiffies - wbbr->lt_last_stamp);
	if (t < 1)
		return;		/* interval is less than one jiffy, so wait */
	t = jiffies_to_usecs(t);
	/* Interval long enough for jiffies_to_usecs() to return a bogus 0? */
	if (t < 1) {
		wbbr_reset_lt_bw_sampling(sk);  /* interval too long; reset */
		return;
	}
	bw = (u64)delivered * BW_UNIT;
	do_div(bw, t);
	wbbr_lt_bw_interval_done(sk, bw);
}

/* Estimate the bandwidth based on how fast packets are delivered */
static void wbbr_update_bw(struct sock *sk, const struct rate_sample *rs)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct wbbr *wbbr = inet_csk_ca(sk);
	u64 bw;

	wbbr->round_start = 0;
	if (rs->delivered < 0 || rs->interval_us <= 0)
		return; /* Not a valid observation */

	/* See if we've reached the next RTT */
	if (!before(rs->prior_delivered, wbbr->next_rtt_delivered)) {
		wbbr->next_rtt_delivered = tp->delivered;
		wbbr->rtt_cnt++;
		wbbr->round_start = 1;
		wbbr->packet_conservation = 0;
	}

	wbbr_lt_bw_sampling(sk, rs);

	/* Divide delivered by the interval to find a (lower bound) bottleneck
	 * bandwidth sample. Delivered is in packets and interval_us in uS and
	 * ratio will be <<1 for most connections. So delivered is first scaled.
	 */
	bw = (u64)rs->delivered * BW_UNIT;
	do_div(bw, rs->interval_us);

	/* If this sample is application-limited, it is likely to have a very
	 * low delivered count that represents application behavior rather than
	 * the available network rate. Such a sample could drag down estimated
	 * bw, causing needless slow-down. Thus, to continue to send at the
	 * last measured network rate, we filter out app-limited samples unless
	 * they describe the path bw at least as well as our bw model.
	 *
	 * So the goal during app-limited phase is to proceed with the best
	 * network rate no matter how long. We automatically leave this
	 * phase when app writes faster than the network can deliver :)
	 */
	if (!rs->is_app_limited || bw >= wbbr_max_bw(sk)) {
		/* Incorporate new sample into our max bw filter. */
		minmax_running_max(&wbbr->bw, wbbr_bw_rtts, wbbr->rtt_cnt, bw);
	}
}

/* Estimate when the pipe is full, using the change in delivery rate: WBBR
 * estimates that STARTUP filled the pipe if the estimated bw hasn't changed by
 * at least wbbr_full_bw_thresh (25%) after wbbr_full_bw_cnt (3) non-app-limited
 * rounds. Why 3 rounds: 1: rwin autotuning grows the rwin, 2: we fill the
 * higher rwin, 3: we get higher delivery rate samples. Or transient
 * cross-traffic or radio noise can go away. CUBIC Hystart shares a similar
 * design goal, but uses delay and inter-ACK spacing instead of bandwidth.
 */
static void wbbr_check_full_bw_reached(struct sock *sk,
				      const struct rate_sample *rs)
{
	struct wbbr *wbbr = inet_csk_ca(sk);
	u32 bw_thresh;

	if (wbbr_full_bw_reached(sk) || !wbbr->round_start || rs->is_app_limited)
		return;

	bw_thresh = (u64)wbbr->full_bw * wbbr_full_bw_thresh >> WBBR_SCALE;
	if (wbbr_max_bw(sk) >= bw_thresh) {
		wbbr->full_bw = wbbr_max_bw(sk);
		wbbr->full_bw_cnt = 0;
		return;
	}
	++wbbr->full_bw_cnt;
}

/* If pipe is probably full, drain the queue and then enter steady-state. */
static void wbbr_check_drain(struct sock *sk, const struct rate_sample *rs)
{
	struct wbbr *wbbr = inet_csk_ca(sk);

	if (wbbr->mode == WBBR_STARTUP && wbbr_full_bw_reached(sk)) {
		wbbr->mode = WBBR_DRAIN;	/* drain queue we created */
		wbbr->pacing_gain = wbbr_drain_gain;	/* pace slow to drain */
		wbbr->cwnd_gain = wbbr_high_gain;	/* maintain cwnd */
	}	/* fall through to check if in-flight is already small: */
	if (wbbr->mode == WBBR_DRAIN &&
	    tcp_packets_in_flight(tcp_sk(sk)) <=
	    wbbr_target_cwnd(sk, wbbr_max_bw(sk), WBBR_UNIT))
		wbbr_reset_probe_bw_mode(sk);  /* we estimate queue is drained */
}

/* The goal of PROBE_RTT mode is to have WBBR flows cooperatively and
 * periodically drain the bottleneck queue, to converge to measure the true
 * min_rtt (unloaded propagation delay). This allows the flows to keep queues
 * small (reducing queuing delay and packet loss) and achieve fairness among
 * WBBR flows.
 *
 * The min_rtt filter window is 10 seconds. When the min_rtt estimate expires,
 * we enter PROBE_RTT mode and cap the cwnd at wbbr_cwnd_min_target=4 packets.
 * After at least wbbr_probe_rtt_mode_ms=200ms and at least one packet-timed
 * round trip elapsed with that flight size <= 4, we leave PROBE_RTT mode and
 * re-enter the previous mode. WBBR uses 200ms to approximately bound the
 * performance penalty of PROBE_RTT's cwnd capping to roughly 2% (200ms/10s).
 *
 * Note that flows need only pay 2% if they are busy sending over the last 10
 * seconds. Interactive applications (e.g., Web, RPCs, video chunks) often have
 * natural silences or low-rate periods within 10 seconds where the rate is low
 * enough for long enough to drain its queue in the bottleneck. We pick up
 * these min RTT measurements opportunistically with our min_rtt filter. :-)
 */
static void wbbr_update_min_rtt(struct sock *sk, const struct rate_sample *rs)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct wbbr *wbbr = inet_csk_ca(sk);
	bool filter_expired;

	/* Track min RTT seen in the min_rtt_win_sec filter window: */
	filter_expired = after(tcp_time_stamp,
			       wbbr->min_rtt_stamp + wbbr_min_rtt_win_sec * HZ);
	if (rs->rtt_us >= 0 &&
	    (rs->rtt_us <= wbbr->min_rtt_us || filter_expired)) {
		wbbr->min_rtt_us = rs->rtt_us;
		wbbr->min_rtt_stamp = tcp_time_stamp;
	}

	if (wbbr_probe_rtt_mode_ms > 0 && filter_expired &&
	    !wbbr->idle_restart && wbbr->mode != WBBR_PROBE_RTT) {
		wbbr->mode = WBBR_PROBE_RTT;  /* dip, drain queue */
		wbbr->pacing_gain = WBBR_UNIT;
		wbbr->cwnd_gain = WBBR_UNIT;
		wbbr_save_cwnd(sk);  /* note cwnd so we can restore it */
		wbbr->probe_rtt_done_stamp = 0;
	}

	if (wbbr->mode == WBBR_PROBE_RTT) {
		/* Ignore low rate samples during this mode. */
		tp->app_limited =
			(tp->delivered + tcp_packets_in_flight(tp)) ? : 1;
		/* Maintain min packets in flight for max(200 ms, 1 round). */
		if (!wbbr->probe_rtt_done_stamp &&
		    tcp_packets_in_flight(tp) <= wbbr_cwnd_min_target) {
			wbbr->probe_rtt_done_stamp = tcp_time_stamp +
				msecs_to_jiffies(wbbr_probe_rtt_mode_ms);
			wbbr->probe_rtt_round_done = 0;
			wbbr->next_rtt_delivered = tp->delivered;
		} else if (wbbr->probe_rtt_done_stamp) {
			if (wbbr->round_start)
				wbbr->probe_rtt_round_done = 1;
			if (wbbr->probe_rtt_round_done &&
			    after(tcp_time_stamp, wbbr->probe_rtt_done_stamp)) {
				wbbr->min_rtt_stamp = tcp_time_stamp;
				wbbr->restore_cwnd = 1;  /* snap to prior_cwnd */
				wbbr_reset_mode(sk);
			}
		}
	}
	wbbr->idle_restart = 0;
}

static void wbbr_update_model(struct sock *sk, const struct rate_sample *rs)
{
	wbbr_update_bw(sk, rs);
	wbbr_update_cycle_phase(sk, rs);
	wbbr_check_full_bw_reached(sk, rs);
	wbbr_check_drain(sk, rs);
	wbbr_update_min_rtt(sk, rs);
}

static u64 wbbr_weight(const struct mptcp_cb *mpcb, const struct sock *sk)
{
	u64 total_rate = 0;
	struct sock *sub_sk;
	const struct wbbr *wbbr = inet_csk_ca(sk);

	if (!mpcb)
		return WBBR_UNIT;


	mptcp_for_each_sk(mpcb, sub_sk) {
		struct wbbr *sub_wbbr = inet_csk_ca(sub_sk);
        u32 bw = minmax_get(&wbbr->bw);
		/* sampled_rtt is initialized by 0 */
		if (mptcp_sk_can_send(sub_sk))
			total_rate += sub_wbbr->instant_rate;
	}

	if (total_rate && wbbr->instant_rate)
		return div64_u64(wbbr->instant_rate * WBBR_UNIT, total_rate);
	else
		return WBBR_UNIT;
}

static void wbbr_main(struct sock *sk, const struct rate_sample *rs)
{
    struct tcp_sock *tp = tcp_sk(sk);
	struct wbbr *wbbr = inet_csk_ca(sk);
	u32 bw;

	wbbr_update_model(sk, rs);

	bw = wbbr_bw(sk);
    wbbr->instant_rate = bw;

    wbbr_set_pacing_rate(sk, bw, (int)((wbbr->pacing_gain * wbbr_weight(tp->mpcb, sk)) >> WBBR_SCALE));
	wbbr_set_tso_segs_goal(sk);
	wbbr_set_cwnd(sk, rs, rs->acked_sacked, bw, wbbr->cwnd_gain);
}

static void wbbr_init(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct wbbr *wbbr = inet_csk_ca(sk);

	wbbr->prior_cwnd = 0;
	wbbr->tso_segs_goal = 0;	 /* default segs per skb until first ACK */
	wbbr->rtt_cnt = 0;
	wbbr->next_rtt_delivered = 0;
	wbbr->prev_ca_state = TCP_CA_Open;
	wbbr->packet_conservation = 0;

	wbbr->probe_rtt_done_stamp = 0;
	wbbr->probe_rtt_round_done = 0;
	wbbr->min_rtt_us = tcp_min_rtt(tp);
	wbbr->min_rtt_stamp = tcp_time_stamp;

	minmax_reset(&wbbr->bw, wbbr->rtt_cnt, 0);  /* init max bw to 0 */

	wbbr->has_seen_rtt = 0;
	wbbr_init_pacing_rate_from_rtt(sk);

	wbbr->restore_cwnd = 0;
	wbbr->round_start = 0;
	wbbr->idle_restart = 0;
	wbbr->full_bw = 0;
	wbbr->full_bw_cnt = 0;
	wbbr->cycle_mstamp.v64 = 0;
	wbbr->cycle_idx = 0;
    wbbr->instant_rate = 0;
	wbbr_reset_lt_bw_sampling(sk);
	wbbr_reset_startup_mode(sk);
}

static u32 wbbr_sndbuf_expand(struct sock *sk)
{
	/* Provision 3 * cwnd since WBBR may slow-start even during recovery. */
	return 3;
}

/* In theory WBBR does not need to undo the cwnd since it does not
 * always reduce cwnd on losses (see wbbr_main()). Keep it for now.
 */
static u32 wbbr_undo_cwnd(struct sock *sk)
{
	return tcp_sk(sk)->snd_cwnd;
}

/* Entering loss recovery, so save cwnd for when we exit or undo recovery. */
static u32 wbbr_ssthresh(struct sock *sk)
{
	wbbr_save_cwnd(sk);
	return TCP_INFINITE_SSTHRESH;	 /* WBBR does not use ssthresh */
}

static size_t wbbr_get_info(struct sock *sk, u32 ext, int *attr,
			   union tcp_cc_info *info)
{
	if (ext & (1 << (INET_DIAG_BBRINFO - 1)) ||
	    ext & (1 << (INET_DIAG_VEGASINFO - 1))) {
		struct tcp_sock *tp = tcp_sk(sk);
		struct wbbr *wbbr = inet_csk_ca(sk);
		u64 bw = wbbr_bw(sk);

		bw = bw * tp->mss_cache * USEC_PER_SEC >> BW_SCALE;
		memset(&info->bbr, 0, sizeof(info->bbr));
		info->bbr.bbr_bw_lo		= (u32)bw;
		info->bbr.bbr_bw_hi		= (u32)(bw >> 32);
		info->bbr.bbr_min_rtt		= wbbr->min_rtt_us;
		info->bbr.bbr_pacing_gain	= wbbr->pacing_gain;
		info->bbr.bbr_cwnd_gain		= wbbr->cwnd_gain;
		*attr = INET_DIAG_BBRINFO;
		return sizeof(info->bbr);
	}
	return 0;
}

static void wbbr_set_state(struct sock *sk, u8 new_state)
{
	struct wbbr *wbbr = inet_csk_ca(sk);

	if (new_state == TCP_CA_Loss) {
		struct rate_sample rs = { .losses = 1 };

		wbbr->prev_ca_state = TCP_CA_Loss;
		wbbr->full_bw = 0;
		wbbr->round_start = 1;	/* treat RTO like end of a round */
		wbbr_lt_bw_sampling(sk, &rs);
	}
}

static struct tcp_congestion_ops tcp_wbbr_cong_ops __read_mostly = {
	.flags		= TCP_CONG_NON_RESTRICTED,
	.name		= "wbbr",
	.owner		= THIS_MODULE,
	.init		= wbbr_init,
	.cong_control	= wbbr_main,
	.sndbuf_expand	= wbbr_sndbuf_expand,
	.undo_cwnd	= wbbr_undo_cwnd,
	.cwnd_event	= wbbr_cwnd_event,
	.ssthresh	= wbbr_ssthresh,
	.tso_segs_goal	= wbbr_tso_segs_goal,
	.get_info	= wbbr_get_info,
	.set_state	= wbbr_set_state,
};

static int __init wbbr_register(void)
{
	BUILD_BUG_ON(sizeof(struct wbbr) > ICSK_CA_PRIV_SIZE);
	return tcp_register_congestion_control(&tcp_wbbr_cong_ops);
}

static void __exit wbbr_unregister(void)
{
	tcp_unregister_congestion_control(&tcp_wbbr_cong_ops);
}

module_init(wbbr_register);
module_exit(wbbr_unregister);

MODULE_AUTHOR("Xiao Jin <yanjijinxiao@gmail.com>");
MODULE_LICENSE("Dual BSD/GPL");
MODULE_DESCRIPTION("MPTCP wBBR");
