#define _GNU_SOURCE 1      /* this macro is needed to define struct ucred */
#include <arpa/inet.h>     /* inet_ntop() */
#include <errno.h>         /* printf */
#include <linux/netlink.h> /*netlink macros and structures */
#include <linux/nl80211.h> /* 802.11 netlink interface */
#include <net/if.h>
#include <stdbool.h> /* bool, true, false macros */
#include <stdio.h>   /* printf */
#include <string.h>
#include <sys/socket.h> /*struct ucred */
#include <time.h>
#include <unistd.h> /* close() */

/* libnl-3 */
#include <netlink/attr.h>
#include <netlink/msg.h>
#include <netlink/netlink.h>

/*libnl-gen-3*/
#include <netlink/genl/ctrl.h>
#include <netlink/genl/genl.h>

#include "nl80211_attrs_map.h" /* netlink attribute types names */

/* used macros */
#ifndef NL_OWN_PORT
#define NL_OWN_PORT (1 << 2)
#endif
#define ETH_ALEN 6

/* cli arguments parse macro and functions */
#define NEXT_ARG()                         \
  do {                                     \
    argv++;                                \
    if (--argc <= 0) incomplete_command(); \
  } while (0)
#define NEXT_ARG_OK() (argc - 1 > 0)
#define PREV_ARG() \
  do {             \
    argv--;        \
    argc++;        \
  } while (0)

static char *argv0; /* ptr to the program name string */
static void usage(void) {
  fprintf(stdout, ""
                  "Usage:   %s [options] [command value] ... [command value]    \n"
                  "options: -b\tshow brief only                                 \n"
                  "command: dev | mac | help                                    \n"
                  "\n"
                  "Example: %s dev wlan0 mac 00:ff:12:a3:e3                     \n"
                  "         %s dev wlan0                                        \n"
                  "\n",
          argv0, argv0, argv0);
  exit(-1);
}

enum plink_state {
  LISTEN,
  OPN_SNT,
  OPN_RCVD,
  CNF_RCVD,
  ESTAB,
  HOLDING,
  BLOCKED
};

// struct nl_sock {
//     struct sockaddr_nl s_local;
//     struct sockaddr_nl s_peer;
//     int s_fd;
//     int s_proto;
//     unsigned int s_seq_next;
//     unsigned int s_seq_expect;
//     int s_flags;
//     struct nl_cb *s_cb;
//     size_t s_bufsize;
// };

struct nl80211_state {
  struct nl_sock *nl_sock;
  int nl80211_id;
} nl80211State = {
    .nl_sock = NULL,
    .nl80211_id = 0};

static void print_power_mode(struct nlattr *a) {
  enum nl80211_mesh_power_mode pm = nla_get_u32(a);

  switch (pm) {
  case NL80211_MESH_POWER_ACTIVE:
    printf("ACTIVE");
    break;
  case NL80211_MESH_POWER_LIGHT_SLEEP:
    printf("LIGHT SLEEP");
    break;
  case NL80211_MESH_POWER_DEEP_SLEEP:
    printf("DEEP SLEEP");
    break;
  default:
    printf("UNKNOWN");
    break;
  }
}

int parse_txq_stats(char *buf, int buflen, struct nlattr *tid_stats_attr, int header,
                    int tid, const char *indent);
int parse_txq_stats(char *buf, int buflen, struct nlattr *tid_stats_attr, int header,
                    int tid, const char *indent) {
  struct nlattr *txqstats_info[NL80211_TXQ_STATS_MAX + 1], *txqinfo;
  static struct nla_policy txqstats_policy[NL80211_TXQ_STATS_MAX + 1] = {
      [NL80211_TXQ_STATS_BACKLOG_BYTES] = {.type = NLA_U32},
      [NL80211_TXQ_STATS_BACKLOG_PACKETS] = {.type = NLA_U32},
      [NL80211_TXQ_STATS_FLOWS] = {.type = NLA_U32},
      [NL80211_TXQ_STATS_DROPS] = {.type = NLA_U32},
      [NL80211_TXQ_STATS_ECN_MARKS] = {.type = NLA_U32},
      [NL80211_TXQ_STATS_OVERLIMIT] = {.type = NLA_U32},
      [NL80211_TXQ_STATS_COLLISIONS] = {.type = NLA_U32},
      [NL80211_TXQ_STATS_TX_BYTES] = {.type = NLA_U32},
      [NL80211_TXQ_STATS_TX_PACKETS] = {.type = NLA_U32},
  };
  char *pos = buf;
  if (nla_parse_nested(txqstats_info, NL80211_TXQ_STATS_MAX, tid_stats_attr,
                       txqstats_policy)) {
    printf("failed to parse nested TXQ stats attributes!");
    return 0;
  }

  if (header)
    pos += snprintf(buf, buflen, "\n%s\t%s\tqsz-byt\t"
                                 "qsz-pkt\tflows\tdrops\tmarks\toverlmt\t"
                                 "hashcol\ttx-bytes\ttx-packets",
                    indent,
                    tid >= 0 ? "TID" : "");

  pos += snprintf(pos, buflen - (pos - buf), "\n%s\t", indent);
  if (tid >= 0)
    pos += snprintf(pos, buflen - (pos - buf), "%d", tid);

#define PRINT_STAT(key, spacer)                         \
  do {                                                  \
    txqinfo = txqstats_info[NL80211_TXQ_STATS_##key];   \
    pos += snprintf(pos, buflen - (pos - buf), spacer); \
    if (txqinfo)                                        \
      pos += snprintf(pos, buflen - (pos - buf), "%u",  \
                      nla_get_u32(txqinfo));            \
  } while (0)

  PRINT_STAT(BACKLOG_BYTES, "\t");
  PRINT_STAT(BACKLOG_PACKETS, "\t");
  PRINT_STAT(FLOWS, "\t");
  PRINT_STAT(DROPS, "\t");
  PRINT_STAT(ECN_MARKS, "\t");
  PRINT_STAT(OVERLIMIT, "\t");
  PRINT_STAT(COLLISIONS, "\t");
  PRINT_STAT(TX_BYTES, "\t");
  PRINT_STAT(TX_PACKETS, "\t\t");

#undef PRINT_STAT

  return pos - buf;
}

static void parse_tid_stats(struct nlattr *tid_stats_attr) {
  struct nlattr *stats_info[NL80211_TID_STATS_MAX + 1], *tidattr, *info;
  static struct nla_policy stats_policy[NL80211_TID_STATS_MAX + 1] = {
      [NL80211_TID_STATS_RX_MSDU] = {.type = NLA_U64},
      [NL80211_TID_STATS_TX_MSDU] = {.type = NLA_U64},
      [NL80211_TID_STATS_TX_MSDU_RETRIES] = {.type = NLA_U64},
      [NL80211_TID_STATS_TX_MSDU_FAILED] = {.type = NLA_U64},
      [NL80211_TID_STATS_TXQ_STATS] = {.type = NLA_NESTED},
  };
  int rem, i = 0;
  char txqbuf[2000] = {}, *pos = txqbuf;
  int buflen = sizeof(txqbuf), foundtxq = 0;

  printf("\n\tMSDU:\n\t\tTID\trx\ttx\ttx retries\ttx failed");
  nla_for_each_nested(tidattr, tid_stats_attr, rem) {
    if (nla_parse_nested(stats_info, NL80211_TID_STATS_MAX,
                         tidattr, stats_policy)) {
      printf("failed to parse nested stats attributes!");
      return;
    }
    printf("\n\t\t%d", i);
    info = stats_info[NL80211_TID_STATS_RX_MSDU];
    if (info)
      printf("\t%llu", (unsigned long long)nla_get_u64(info));
    info = stats_info[NL80211_TID_STATS_TX_MSDU];
    if (info)
      printf("\t%llu", (unsigned long long)nla_get_u64(info));
    info = stats_info[NL80211_TID_STATS_TX_MSDU_RETRIES];
    if (info)
      printf("\t%llu", (unsigned long long)nla_get_u64(info));
    info = stats_info[NL80211_TID_STATS_TX_MSDU_FAILED];
    if (info)
      printf("\t\t%llu", (unsigned long long)nla_get_u64(info));
    info = stats_info[NL80211_TID_STATS_TXQ_STATS];
    if (info) {
      pos += parse_txq_stats(pos, buflen - (pos - txqbuf), info, !foundtxq, i, "\t");
      foundtxq = 1;
    }

    i++;
  }

  if (foundtxq)
    printf("\n\tTXQs:%s", txqbuf);
}

static void parse_bss_param(struct nlattr *bss_param_attr) {
  struct nlattr *bss_param_info[NL80211_STA_BSS_PARAM_MAX + 1], *info;
  static struct nla_policy bss_poilcy[NL80211_STA_BSS_PARAM_MAX + 1] = {
      [NL80211_STA_BSS_PARAM_CTS_PROT] = {.type = NLA_FLAG},
      [NL80211_STA_BSS_PARAM_SHORT_PREAMBLE] = {.type = NLA_FLAG},
      [NL80211_STA_BSS_PARAM_SHORT_SLOT_TIME] = {.type = NLA_FLAG},
      [NL80211_STA_BSS_PARAM_DTIM_PERIOD] = {.type = NLA_U8},
      [NL80211_STA_BSS_PARAM_BEACON_INTERVAL] = {.type = NLA_U16},
  };

  if (nla_parse_nested(bss_param_info, NL80211_STA_BSS_PARAM_MAX,
                       bss_param_attr, bss_poilcy)) {
    printf("failed to parse nested bss param attributes!");
  }

  info = bss_param_info[NL80211_STA_BSS_PARAM_DTIM_PERIOD];
  if (info)
    printf("\n\tDTIM period:\t%u", nla_get_u8(info));
  info = bss_param_info[NL80211_STA_BSS_PARAM_BEACON_INTERVAL];
  if (info)
    printf("\n\tbeacon interval:%u", nla_get_u16(info));
  info = bss_param_info[NL80211_STA_BSS_PARAM_CTS_PROT];
  if (info) {
    printf("\n\tCTS protection:");
    if (nla_get_u16(info))
      printf("\tyes");
    else
      printf("\tno");
  }
  info = bss_param_info[NL80211_STA_BSS_PARAM_SHORT_PREAMBLE];
  if (info) {
    printf("\n\tshort preamble:");
    if (nla_get_u16(info))
      printf("\tyes");
    else
      printf("\tno");
  }
  info = bss_param_info[NL80211_STA_BSS_PARAM_SHORT_SLOT_TIME];
  if (info) {
    printf("\n\tshort slot time:");
    if (nla_get_u16(info))
      printf("yes");
    else
      printf("no");
  }
}

static char *get_chain_signal(struct nlattr *attr_list) {
  struct nlattr *attr;
  static char buf[64];
  char *cur = buf;
  int i = 0, rem;
  const char *prefix;

  if (!attr_list)
    return "";

  nla_for_each_nested(attr, attr_list, rem) {
    if (i++ > 0)
      prefix = ", ";
    else
      prefix = "[";

    cur += snprintf(cur, sizeof(buf) - (cur - buf), "%s%d", prefix,
                    (int8_t)nla_get_u8(attr));
  }

  if (i)
    snprintf(cur, sizeof(buf) - (cur - buf), "] ");

  return buf;
}

static void parse_bitrate(struct nlattr *bitrate_attr, char *buf, int buflen) {
  int rate = 0;
  char *pos = buf;
  struct nlattr *rinfo[NL80211_RATE_INFO_MAX + 1];
  static struct nla_policy rate_policy[NL80211_RATE_INFO_MAX + 1] = {
      [NL80211_RATE_INFO_BITRATE] = {.type = NLA_U16},
      [NL80211_RATE_INFO_BITRATE32] = {.type = NLA_U32},
      [NL80211_RATE_INFO_MCS] = {.type = NLA_U8},
      [NL80211_RATE_INFO_40_MHZ_WIDTH] = {.type = NLA_FLAG},
      [NL80211_RATE_INFO_SHORT_GI] = {.type = NLA_FLAG},
  };

  if (nla_parse_nested(rinfo, NL80211_RATE_INFO_MAX,
                       bitrate_attr, rate_policy)) {
    snprintf(buf, buflen, "failed to parse nested rate attributes!");
    return;
  }

  if (rinfo[NL80211_RATE_INFO_BITRATE32])
    rate = nla_get_u32(rinfo[NL80211_RATE_INFO_BITRATE32]);
  else if (rinfo[NL80211_RATE_INFO_BITRATE])
    rate = nla_get_u16(rinfo[NL80211_RATE_INFO_BITRATE]);
  if (rate > 0)
    pos += snprintf(pos, buflen - (pos - buf),
                    "%d.%d MBit/s", rate / 10, rate % 10);
  else
    pos += snprintf(pos, buflen - (pos - buf), "(unknown)");

  if (rinfo[NL80211_RATE_INFO_MCS])
    pos += snprintf(pos, buflen - (pos - buf),
                    " MCS %d", nla_get_u8(rinfo[NL80211_RATE_INFO_MCS]));
  if (rinfo[NL80211_RATE_INFO_VHT_MCS])
    pos += snprintf(pos, buflen - (pos - buf),
                    " VHT-MCS %d", nla_get_u8(rinfo[NL80211_RATE_INFO_VHT_MCS]));
  if (rinfo[NL80211_RATE_INFO_40_MHZ_WIDTH])
    pos += snprintf(pos, buflen - (pos - buf), " 40MHz");
  if (rinfo[NL80211_RATE_INFO_80_MHZ_WIDTH])
    pos += snprintf(pos, buflen - (pos - buf), " 80MHz");
  if (rinfo[NL80211_RATE_INFO_80P80_MHZ_WIDTH])
    pos += snprintf(pos, buflen - (pos - buf), " 80P80MHz");
  if (rinfo[NL80211_RATE_INFO_160_MHZ_WIDTH])
    pos += snprintf(pos, buflen - (pos - buf), " 160MHz");
  if (rinfo[NL80211_RATE_INFO_320_MHZ_WIDTH])
    pos += snprintf(pos, buflen - (pos - buf), " 320MHz");
  if (rinfo[NL80211_RATE_INFO_SHORT_GI])
    pos += snprintf(pos, buflen - (pos - buf), " short GI");
  if (rinfo[NL80211_RATE_INFO_VHT_NSS])
    pos += snprintf(pos, buflen - (pos - buf),
                    " VHT-NSS %d", nla_get_u8(rinfo[NL80211_RATE_INFO_VHT_NSS]));
  if (rinfo[NL80211_RATE_INFO_HE_MCS])
    pos += snprintf(pos, buflen - (pos - buf),
                    " HE-MCS %d", nla_get_u8(rinfo[NL80211_RATE_INFO_HE_MCS]));
  if (rinfo[NL80211_RATE_INFO_HE_NSS])
    pos += snprintf(pos, buflen - (pos - buf),
                    " HE-NSS %d", nla_get_u8(rinfo[NL80211_RATE_INFO_HE_NSS]));
  if (rinfo[NL80211_RATE_INFO_HE_GI])
    pos += snprintf(pos, buflen - (pos - buf),
                    " HE-GI %d", nla_get_u8(rinfo[NL80211_RATE_INFO_HE_GI]));
  if (rinfo[NL80211_RATE_INFO_HE_DCM])
    pos += snprintf(pos, buflen - (pos - buf),
                    " HE-DCM %d", nla_get_u8(rinfo[NL80211_RATE_INFO_HE_DCM]));
  if (rinfo[NL80211_RATE_INFO_HE_RU_ALLOC])
    pos += snprintf(pos, buflen - (pos - buf),
                    " HE-RU-ALLOC %d", nla_get_u8(rinfo[NL80211_RATE_INFO_HE_RU_ALLOC]));
  if (rinfo[NL80211_RATE_INFO_EHT_MCS])
    pos += snprintf(pos, buflen - (pos - buf),
                    " EHT-MCS %d", nla_get_u8(rinfo[NL80211_RATE_INFO_EHT_MCS]));
  if (rinfo[NL80211_RATE_INFO_EHT_NSS])
    pos += snprintf(pos, buflen - (pos - buf),
                    " EHT-NSS %d", nla_get_u8(rinfo[NL80211_RATE_INFO_EHT_NSS]));
  if (rinfo[NL80211_RATE_INFO_EHT_GI])
    pos += snprintf(pos, buflen - (pos - buf),
                    " EHT-GI %d", nla_get_u8(rinfo[NL80211_RATE_INFO_EHT_GI]));
  if (rinfo[NL80211_RATE_INFO_EHT_RU_ALLOC])
    pos += snprintf(pos, buflen - (pos - buf),
                    " EHT-RU-ALLOC %d", nla_get_u8(rinfo[NL80211_RATE_INFO_EHT_RU_ALLOC]));
}

static int mac_addr_atoi(uint8_t *mac, const char *hex) {
  if (hex == NULL) return 1;
  if (strlen(hex) != sizeof("FF:FF:FF:FF:FF:FF") - 1) {
    printf("len: %zu %zu\n", strlen(hex), (size_t)sizeof("FF:FF:FF:FF:FF:FF") - 1);
    return 0;
  }
  mac[0] = strtol(&hex[0], NULL, 16);
  mac[1] = strtol(&hex[3], NULL, 16);
  mac[2] = strtol(&hex[6], NULL, 16);
  mac[3] = strtol(&hex[9], NULL, 16);
  mac[4] = strtol(&hex[12], NULL, 16);
  mac[5] = strtol(&hex[15], NULL, 16);
  return 1;
}

static const char *get_nl_attr_type(unsigned type) {
  size_t i;

  for (i = 0; i < sizeof nl_attr_map / sizeof nl_attr_map[0]; i++) {
    if (type == nl_attr_map[i].type) {
      return nl_attr_map[i].name;
    }
  }

  return "unknown";
}

static int nl_cb_brief(struct nl_msg *msg, void *arg) {
  struct nlmsghdr *ret_hdr = nlmsg_hdr(msg);
  struct nlattr *tb_msg[NL80211_ATTR_MAX + 1];

  if (ret_hdr->nlmsg_type != nl80211State.nl80211_id) return NL_STOP;
  struct genlmsghdr *gnlh = (struct genlmsghdr *)nlmsg_data(ret_hdr);
  nla_parse(tb_msg, NL80211_ATTR_MAX, genlmsg_attrdata(gnlh, 0), genlmsg_attrlen(gnlh, 0), NULL);

  /*
   *  <------- NLA_HDRLEN ------> <-- NLA_ALIGN(payload)-->
   * +---------------------+- - -+- - - - - - - - - -+- - -+
   * |        Header       | Pad |     Payload       | Pad |
   * |   (struct nlattr)   | ing |                   | ing |
   * +---------------------+- - -+- - - - - - - - - -+- - -+
   *  <-------------- nlattr->nla_len -------------->
   */

  if (tb_msg[NL80211_ATTR_MAC]) {
    void *p = tb_msg[NL80211_ATTR_MAC];
    uint8_t *data = p + NLA_HDRLEN;
    printf("%02X:%02X:%02X:%02X:%02X:%02X\n",
           data[0], data[1], data[2], data[3], data[4], data[5]);
  }

  return NL_SKIP;
}
static int nl_cb(struct nl_msg *msg, void *arg) {
  struct nlmsghdr *ret_hdr = nlmsg_hdr(msg);
  struct nlattr *tb_msg[NL80211_ATTR_MAX + 1];
  char *chain;
  struct timeval now;
  unsigned long long now_ms;
  char state_name[10];
  struct nl80211_sta_flag_update *sta_flags;

  gettimeofday(&now, NULL);
  now_ms = now.tv_sec * 1000ULL;
  now_ms += (now.tv_usec / 1000);

  if (ret_hdr->nlmsg_type != nl80211State.nl80211_id) return NL_STOP;

  struct genlmsghdr *gnlh = (struct genlmsghdr *)nlmsg_data(ret_hdr);

  nla_parse(tb_msg, NL80211_ATTR_MAX, genlmsg_attrdata(gnlh, 0), genlmsg_attrlen(gnlh, 0), NULL);

  for (int i = 0; i < NL80211_ATTR_MAX; i++) {
    if (tb_msg[i] == NULL) continue;
    printf("attr. type: %d %s\n", tb_msg[i]->nla_type, get_nl_attr_type(tb_msg[i]->nla_type));
  }
  /*
   *  <------- NLA_HDRLEN ------> <-- NLA_ALIGN(payload)-->
   * +---------------------+- - -+- - - - - - - - - -+- - -+
   * |        Header       | Pad |     Payload       | Pad |
   * |   (struct nlattr)   | ing |                   | ing |
   * +---------------------+- - -+- - - - - - - - - -+- - -+
   *  <-------------- nlattr->nla_len -------------->
   */
  char str[100];

  if (tb_msg[NL80211_ATTR_IFINDEX]) {
    void *p = tb_msg[NL80211_ATTR_IFINDEX];
    int *data = p + NLA_HDRLEN;
    if_indextoname(*data, str);
    printf("dev idx: %d if: %s\n", *data, str);
  }

  if (tb_msg[NL80211_ATTR_MAC]) {
    void *p = tb_msg[NL80211_ATTR_MAC];
    uint8_t *data = p + NLA_HDRLEN;
    printf("mac: %02X:%02X:%02X:%02X:%02X:%02X\n",
           data[0], data[1], data[2], data[3], data[4], data[5]);
  }

  if (!tb_msg[NL80211_ATTR_STA_INFO]) {
    fprintf(stderr, "sta stats missing!\n");
    return NL_SKIP;
  }
  if (nla_parse_nested(tb_msg, NL80211_STA_INFO_MAX,
                       tb_msg[NL80211_ATTR_STA_INFO],
                       stats_policy)) {
    fprintf(stderr, "failed to parse nested attributes!\n");
    return NL_SKIP;
  }

  if (tb_msg[NL80211_STA_INFO_INACTIVE_TIME])
    printf("\n\tinactive time:\t%u ms",
           nla_get_u32(tb_msg[NL80211_STA_INFO_INACTIVE_TIME]));
  if (tb_msg[NL80211_STA_INFO_RX_BYTES64])
    printf("\n\trx bytes:\t%llu",
           (unsigned long long)nla_get_u64(tb_msg[NL80211_STA_INFO_RX_BYTES64]));
  else if (tb_msg[NL80211_STA_INFO_RX_BYTES])
    printf("\n\trx bytes:\t%u",
           nla_get_u32(tb_msg[NL80211_STA_INFO_RX_BYTES]));
  if (tb_msg[NL80211_STA_INFO_RX_PACKETS])
    printf("\n\trx packets:\t%u",
           nla_get_u32(tb_msg[NL80211_STA_INFO_RX_PACKETS]));
  if (tb_msg[NL80211_STA_INFO_TX_BYTES64])
    printf("\n\ttx bytes:\t%llu",
           (unsigned long long)nla_get_u64(tb_msg[NL80211_STA_INFO_TX_BYTES64]));
  else if (tb_msg[NL80211_STA_INFO_TX_BYTES])
    printf("\n\ttx bytes:\t%u",
           nla_get_u32(tb_msg[NL80211_STA_INFO_TX_BYTES]));
  if (tb_msg[NL80211_STA_INFO_TX_PACKETS])
    printf("\n\ttx packets:\t%u",
           nla_get_u32(tb_msg[NL80211_STA_INFO_TX_PACKETS]));
  if (tb_msg[NL80211_STA_INFO_TX_RETRIES])
    printf("\n\ttx retries:\t%u",
           nla_get_u32(tb_msg[NL80211_STA_INFO_TX_RETRIES]));
  if (tb_msg[NL80211_STA_INFO_TX_FAILED])
    printf("\n\ttx failed:\t%u",
           nla_get_u32(tb_msg[NL80211_STA_INFO_TX_FAILED]));
  if (tb_msg[NL80211_STA_INFO_BEACON_LOSS])
    printf("\n\tbeacon loss:\t%u",
           nla_get_u32(tb_msg[NL80211_STA_INFO_BEACON_LOSS]));
  if (tb_msg[NL80211_STA_INFO_BEACON_RX])
    printf("\n\tbeacon rx:\t%llu",
           (unsigned long long)nla_get_u64(tb_msg[NL80211_STA_INFO_BEACON_RX]));
  if (tb_msg[NL80211_STA_INFO_RX_DROP_MISC])
    printf("\n\trx drop misc:\t%llu",
           (unsigned long long)nla_get_u64(tb_msg[NL80211_STA_INFO_RX_DROP_MISC]));

  chain = get_chain_signal(tb_msg[NL80211_STA_INFO_CHAIN_SIGNAL]);
  if (tb_msg[NL80211_STA_INFO_SIGNAL])
    printf("\n\tsignal:  \t%d %sdBm",
           (int8_t)nla_get_u8(tb_msg[NL80211_STA_INFO_SIGNAL]),
           chain);

  chain = get_chain_signal(tb_msg[NL80211_STA_INFO_CHAIN_SIGNAL_AVG]);
  if (tb_msg[NL80211_STA_INFO_SIGNAL_AVG])
    printf("\n\tsignal avg:\t%d %sdBm",
           (int8_t)nla_get_u8(tb_msg[NL80211_STA_INFO_SIGNAL_AVG]),
           chain);

  if (tb_msg[NL80211_STA_INFO_BEACON_SIGNAL_AVG])
    printf("\n\tbeacon signal avg:\t%d dBm",
           (int8_t)nla_get_u8(tb_msg[NL80211_STA_INFO_BEACON_SIGNAL_AVG]));
  if (tb_msg[NL80211_STA_INFO_T_OFFSET])
    printf("\n\tToffset:\t%llu us",
           (unsigned long long)nla_get_u64(tb_msg[NL80211_STA_INFO_T_OFFSET]));

  if (tb_msg[NL80211_STA_INFO_TX_BITRATE]) {
    char buf[100];

    parse_bitrate(tb_msg[NL80211_STA_INFO_TX_BITRATE], buf, sizeof(buf));
    printf("\n\ttx bitrate:\t%s", buf);
  }

  if (tb_msg[NL80211_STA_INFO_TX_DURATION])
    printf("\n\ttx duration:\t%lld us",
           (unsigned long long)nla_get_u64(tb_msg[NL80211_STA_INFO_TX_DURATION]));

  if (tb_msg[NL80211_STA_INFO_RX_BITRATE]) {
    char buf[100];

    parse_bitrate(tb_msg[NL80211_STA_INFO_RX_BITRATE], buf, sizeof(buf));
    printf("\n\trx bitrate:\t%s", buf);
  }

  if (tb_msg[NL80211_STA_INFO_RX_DURATION])
    printf("\n\trx duration:\t%lld us",
           (unsigned long long)nla_get_u64(tb_msg[NL80211_STA_INFO_RX_DURATION]));

  if (tb_msg[NL80211_STA_INFO_ACK_SIGNAL])
    printf("\n\tlast ack signal:%d dBm",
           (int8_t)nla_get_u8(tb_msg[NL80211_STA_INFO_ACK_SIGNAL]));

  if (tb_msg[NL80211_STA_INFO_ACK_SIGNAL_AVG])
    printf("\n\tavg ack signal:\t%d dBm",
           (int8_t)nla_get_u8(tb_msg[NL80211_STA_INFO_ACK_SIGNAL_AVG]));

  if (tb_msg[NL80211_STA_INFO_AIRTIME_WEIGHT]) {
    printf("\n\tairtime weight: %d", nla_get_u16(tb_msg[NL80211_STA_INFO_AIRTIME_WEIGHT]));
  }

  if (tb_msg[NL80211_STA_INFO_EXPECTED_THROUGHPUT]) {
    uint32_t thr;

    thr = nla_get_u32(tb_msg[NL80211_STA_INFO_EXPECTED_THROUGHPUT]);
    /* convert in Mbps but scale by 1000 to save kbps units */
    thr = thr * 1000 / 1024;

    printf("\n\texpected throughput:\t%u.%uMbps",
           thr / 1000, thr % 1000);
  }

  if (tb_msg[NL80211_STA_INFO_LLID])
    printf("\n\tmesh llid:\t%d",
           nla_get_u16(tb_msg[NL80211_STA_INFO_LLID]));
  if (tb_msg[NL80211_STA_INFO_PLID])
    printf("\n\tmesh plid:\t%d",
           nla_get_u16(tb_msg[NL80211_STA_INFO_PLID]));
  if (tb_msg[NL80211_STA_INFO_PLINK_STATE]) {
    switch (nla_get_u8(tb_msg[NL80211_STA_INFO_PLINK_STATE])) {
    case LISTEN:
      strcpy(state_name, "LISTEN");
      break;
    case OPN_SNT:
      strcpy(state_name, "OPN_SNT");
      break;
    case OPN_RCVD:
      strcpy(state_name, "OPN_RCVD");
      break;
    case CNF_RCVD:
      strcpy(state_name, "CNF_RCVD");
      break;
    case ESTAB:
      strcpy(state_name, "ESTAB");
      break;
    case HOLDING:
      strcpy(state_name, "HOLDING");
      break;
    case BLOCKED:
      strcpy(state_name, "BLOCKED");
      break;
    default:
      strcpy(state_name, "UNKNOWN");
      break;
    }
    printf("\n\tmesh plink:\t%s", state_name);
  }
  if (tb_msg[NL80211_STA_INFO_AIRTIME_LINK_METRIC])
    printf("\n\tmesh airtime link metric: %d",
           nla_get_u32(tb_msg[NL80211_STA_INFO_AIRTIME_LINK_METRIC]));
  if (tb_msg[NL80211_STA_INFO_CONNECTED_TO_GATE])
    printf("\n\tmesh connected to gate:\t%s",
           nla_get_u8(tb_msg[NL80211_STA_INFO_CONNECTED_TO_GATE]) ? "yes" : "no");
  if (tb_msg[NL80211_STA_INFO_CONNECTED_TO_AS])
    printf("\n\tmesh connected to auth server:\t%s",
           nla_get_u8(tb_msg[NL80211_STA_INFO_CONNECTED_TO_AS]) ? "yes" : "no");

  if (tb_msg[NL80211_STA_INFO_LOCAL_PM]) {
    printf("\n\tmesh local PS mode:\t");
    print_power_mode(tb_msg[NL80211_STA_INFO_LOCAL_PM]);
  }
  if (tb_msg[NL80211_STA_INFO_PEER_PM]) {
    printf("\n\tmesh peer PS mode:\t");
    print_power_mode(tb_msg[NL80211_STA_INFO_PEER_PM]);
  }
  if (tb_msg[NL80211_STA_INFO_NONPEER_PM]) {
    printf("\n\tmesh non-peer PS mode:\t");
    print_power_mode(tb_msg[NL80211_STA_INFO_NONPEER_PM]);
  }

  if (tb_msg[NL80211_STA_INFO_STA_FLAGS]) {
    sta_flags = (struct nl80211_sta_flag_update *)
        nla_data(tb_msg[NL80211_STA_INFO_STA_FLAGS]);

    if (sta_flags->mask & NL80211_STA_FLAG_AUTHORIZED) {
      printf("\n\tauthorized:\t");
      if (sta_flags->set & NL80211_STA_FLAG_AUTHORIZED)
        printf("yes");
      else
        printf("no");
    }

    if (sta_flags->mask & NL80211_STA_FLAG_AUTHENTICATED) {
      printf("\n\tauthenticated:\t");
      if (sta_flags->set & NL80211_STA_FLAG_AUTHENTICATED)
        printf("yes");
      else
        printf("no");
    }

    if (sta_flags->mask & NL80211_STA_FLAG_ASSOCIATED) {
      printf("\n\tassociated:\t");
      if (sta_flags->set & NL80211_STA_FLAG_ASSOCIATED)
        printf("yes");
      else
        printf("no");
    }

    if (sta_flags->mask & NL80211_STA_FLAG_SHORT_PREAMBLE) {
      printf("\n\tpreamble:\t");
      if (sta_flags->set & NL80211_STA_FLAG_SHORT_PREAMBLE)
        printf("short");
      else
        printf("long");
    }

    if (sta_flags->mask & NL80211_STA_FLAG_WME) {
      printf("\n\tWMM/WME:\t");
      if (sta_flags->set & NL80211_STA_FLAG_WME)
        printf("yes");
      else
        printf("no");
    }

    if (sta_flags->mask & NL80211_STA_FLAG_MFP) {
      printf("\n\tMFP:\t\t");
      if (sta_flags->set & NL80211_STA_FLAG_MFP)
        printf("yes");
      else
        printf("no");
    }

    if (sta_flags->mask & NL80211_STA_FLAG_TDLS_PEER) {
      printf("\n\tTDLS peer:\t");
      if (sta_flags->set & NL80211_STA_FLAG_TDLS_PEER)
        printf("yes");
      else
        printf("no");
    }
  }

  if (tb_msg[NL80211_STA_INFO_TID_STATS] && arg != NULL &&
      !strcmp((char *)arg, "-v"))
    parse_tid_stats(tb_msg[NL80211_STA_INFO_TID_STATS]);
  if (tb_msg[NL80211_STA_INFO_BSS_PARAM])
    parse_bss_param(tb_msg[NL80211_STA_INFO_BSS_PARAM]);
  if (tb_msg[NL80211_STA_INFO_CONNECTED_TIME])
    printf("\n\tconnected time:\t%u seconds",
           nla_get_u32(tb_msg[NL80211_STA_INFO_CONNECTED_TIME]));
  if (tb_msg[NL80211_STA_INFO_ASSOC_AT_BOOTTIME]) {
    unsigned long long bt;
    struct timespec now_ts;
    unsigned long long boot_ns;
    unsigned long long assoc_at_ms;

    clock_gettime(CLOCK_BOOTTIME, &now_ts);
    boot_ns = now_ts.tv_sec * 1000000000ULL;
    boot_ns += now_ts.tv_nsec;

    bt = (unsigned long long)nla_get_u64(tb_msg[NL80211_STA_INFO_ASSOC_AT_BOOTTIME]);
    printf("\n\tassociated at [boottime]:\t%llu.%.3llus",
           bt / 1000000000, (bt % 1000000000) / 1000000);
    assoc_at_ms = now_ms - ((boot_ns - bt) / 1000000);
    printf("\n\tassociated at:\t%llu ms", assoc_at_ms);
  }

  printf("\n\tcurrent time:\t%llu ms\n", now_ms);
  return NL_SKIP;
}

/* Returns true if 'prefix' is a not empty prefix of 'string'. */
static bool matches(const char *prefix, const char *string) {
  if (!*prefix)
    return false;
  while (*string && *prefix == *string) {
    prefix++;
    string++;
  }
  return !*prefix;
}

static void incomplete_command(void) {
  fprintf(stderr, "Command line is not complete. Try option \"help\"\n");
  exit(-1);
}

static int nl80211_cmd_get_station(const char *dev, const char *mac, int flags, int is_brief) {
  int ret; /* to store returning values */
  /* nl_socket_alloc(), genl_connect() replacement */
  struct nl_sock sk = {
      .s_fd = -1,
      .s_cb = nl_cb_alloc(NL_CB_DEFAULT), /* callback */
      .s_local.nl_family = AF_NETLINK,
      .s_peer.nl_family = AF_NETLINK,
      .s_seq_expect = time(NULL),
      .s_seq_next = time(NULL),

      /* the port is 0 (unspecified), meaning NL_OWN_PORT */
      .s_flags = NL_OWN_PORT,
  };

  sk.s_fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_GENERIC);
  nl80211State.nl_sock = &sk;

  // find the nl80211 driver ID
  nl80211State.nl80211_id = genl_ctrl_resolve(&sk, "nl80211");

  // attach a callback
  if (is_brief) {
    nl_socket_modify_cb(&sk, NL_CB_VALID, NL_CB_CUSTOM, nl_cb_brief, NULL);
  } else {
    nl_socket_modify_cb(&sk, NL_CB_VALID, NL_CB_CUSTOM, nl_cb, NULL);
  }

  // allocate a message
  struct nl_msg *msg = nlmsg_alloc();

  int if_index = if_nametoindex(dev);
  if (if_index == 0) if_index = -1;

  uint8_t mac_addr[ETH_ALEN];
  if (!mac_addr_atoi((uint8_t *)&mac_addr, mac)) {
    fprintf(stderr, "invalid mac address\n");
    return 2;
  }

  enum nl80211_commands cmd = NL80211_CMD_GET_STATION;

  // setup the message
  genlmsg_put(msg, 0, 0, nl80211State.nl80211_id, 0, flags, cmd, 0);

  // add message attributes
  NLA_PUT_U32(msg, NL80211_ATTR_IFINDEX, if_index);

  if (mac != NULL) {
    NLA_PUT(msg, NL80211_ATTR_MAC, ETH_ALEN, mac_addr);
  }
  // send the message (this frees it)
  ret = nl_send_auto_complete(&sk, msg);
  if (ret < 0) {
    /* -ret bacause nl commands returns negative error code if false */
    fprintf(stderr, "nl_send_auto_complete: %d %s\n", ret, strerror(-ret));
    return (ret);
  }

  // block for message to return
  ret = nl_recvmsgs_default(&sk);

  if (ret < 0) {
    /* -ret bacause nl commands returns negative error code if false */
    fprintf(stderr, "nl_send_auto_complete: %d %s\n", ret, strerror(-ret));
  }

  return (ret);

nla_put_failure: /* this tag is used in NLA_PUT macros */
  nlmsg_free(msg);
  return -ENOBUFS;
}

int main(int argc, char **argv) {
  int ret;
  char *dev = NULL, *mac = NULL;
  int is_brief = 0;
  int flags = 0; /* netlink generic msg flags */
  /* cli arguments parse */
  argv0 = *argv; /* first arg is program name */
  while (argc > 1) {
    NEXT_ARG();
    if (matches(*argv, "dev")) {
      NEXT_ARG();
      dev = *argv; /* device interface name e.g. wlan0 */
    } else if (matches(*argv, "mac")) {
      NEXT_ARG();
      mac = *argv; /* mac address e.g. aa:bb:cc:dd:ee:ff */
    } else if (matches(*argv, "help")) {
      usage();
    } else if (matches(*argv, "-b")) {
      is_brief = 1;
    } else {
      usage();
    }
  }

  if (dev == NULL) {
    incomplete_command();
  }
  if (mac == NULL) { /* if mac is not set than set flags to make dump */
    flags = NLM_F_DUMP;
  }

  ret = nl80211_cmd_get_station(dev, mac, flags, is_brief);
  return -ret;
}
