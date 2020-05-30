#pragma once

#include <linux/types.h>

struct tcp_info_ext_v2 {
    __u8 tcpi_state;
    __u8 tcpi_ca_state;
    __u8 tcpi_retransmits;
    __u8 tcpi_probes;
    __u8 tcpi_backoff;
    __u8 tcpi_options;
    __u8 tcpi_snd_wscale : 4, tcpi_rcv_wscale : 4;
    __u8 tcpi_delivery_rate_app_limited:1;

    __u32 tcpi_rto;
    __u32 tcpi_ato;
    __u32 tcpi_snd_mss;
    __u32 tcpi_rcv_mss;

    __u32 tcpi_unacked;
    __u32 tcpi_sacked;
    __u32 tcpi_lost;
    __u32 tcpi_retrans;
    __u32 tcpi_fackets;

    /* Times. */
    __u32 tcpi_last_data_sent;
    __u32 tcpi_last_ack_sent;     /* Not remembered, sorry. */
    __u32 tcpi_last_data_recv;
    __u32 tcpi_last_ack_recv;

    /* Metrics. */
    __u32 tcpi_pmtu;
    __u32 tcpi_rcv_ssthresh;
    __u32 tcpi_rtt;
    __u32 tcpi_rttvar;
    __u32 tcpi_snd_ssthresh;
    __u32 tcpi_snd_cwnd;
    __u32 tcpi_advmss;
    __u32 tcpi_reordering;

    __u32 tcpi_rcv_rtt;
    __u32 tcpi_rcv_space;

    __u32 tcpi_total_retrans;

    __u64 tcpi_pacing_rate;
    __u64 tcpi_max_pacing_rate;
    __u64 tcpi_bytes_acked;    /* RFC4898 tcpEStatsAppHCThruOctetsAcked */
    __u64 tcpi_bytes_received; /* RFC4898 tcpEStatsAppHCThruOctetsReceived */
    __u32 tcpi_segs_out;     /* RFC4898 tcpEStatsPerfSegsOut */
    __u32 tcpi_segs_in;     /* RFC4898 tcpEStatsPerfSegsIn */

    __u32 tcpi_notsent_bytes;
    __u32 tcpi_min_rtt;
    __u32 tcpi_data_segs_in; /* RFC4898 tcpEStatsDataSegsIn */
    __u32 tcpi_data_segs_out; /* RFC4898 tcpEStatsDataSegsOut */

    __u64 tcpi_delivery_rate;

    __u64 tcpi_busy_time;      /* Time (usec) busy sending data */
    __u64 tcpi_rwnd_limited;   /* Time (usec) limited by receive window */
    __u64 tcpi_sndbuf_limited; /* Time (usec) limited by send buffer */

    __u32 tcpi_delivered;
    __u32 tcpi_delivered_ce;

    __u64 tcpi_bytes_sent;     /* RFC4898 tcpEStatsPerfHCDataOctetsOut */
    __u64 tcpi_bytes_retrans;  /* RFC4898 tcpEStatsPerfOctetsRetrans */
    __u32 tcpi_dsack_dups;     /* RFC4898 tcpEStatsStackDSACKDups */
    __u32 tcpi_reord_seen;     /* reordering events seen */
};

/* Pre-4.19 kernels legacy tcp_info format */

struct tcp_info_ext_v1 {
    __u8    tcpi_state;
    __u8    tcpi_ca_state;
    __u8    tcpi_retransmits;
    __u8    tcpi_probes;
    __u8    tcpi_backoff;
    __u8    tcpi_options;
    __u8    tcpi_snd_wscale : 4, tcpi_rcv_wscale : 4;

    __u32   tcpi_rto;
    __u32   tcpi_ato;
    __u32   tcpi_snd_mss;
    __u32   tcpi_rcv_mss;

    __u32   tcpi_unacked;
    __u32   tcpi_sacked;
    __u32   tcpi_lost;
    __u32   tcpi_retrans;
    __u32   tcpi_fackets;

    /* Times. */
    __u32   tcpi_last_data_sent;
    __u32   tcpi_last_ack_sent;     /* Not remembered, sorry. */
    __u32   tcpi_last_data_recv;
    __u32   tcpi_last_ack_recv;

    /* Metrics. */
    __u32   tcpi_pmtu;
    __u32   tcpi_rcv_ssthresh;
    __u32   tcpi_rtt;
    __u32   tcpi_rttvar;
    __u32   tcpi_snd_ssthresh;
    __u32   tcpi_snd_cwnd;
    __u32   tcpi_advmss;
    __u32   tcpi_reordering;

    __u32   tcpi_rcv_rtt;
    __u32   tcpi_rcv_space;

    __u32   tcpi_total_retrans;

    __u64   tcpi_pacing_rate;
    __u64   tcpi_max_pacing_rate;
    __u64   tcpi_bytes_acked;    /* RFC4898 tcpEStatsAppHCThruOctetsAcked */
    __u64   tcpi_bytes_received; /* RFC4898 tcpEStatsAppHCThruOctetsReceived */
    __u32   tcpi_segs_out;       /* RFC4898 tcpEStatsPerfSegsOut */
    __u32   tcpi_segs_in;        /* RFC4898 tcpEStatsPerfSegsIn */
};
