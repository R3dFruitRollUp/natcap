/*
 * Author: Chen Minqiang <ptpt52@gmail.com>
 *  Date : Thu, 30 Aug 2018 11:25:35 +0800
 *
 * This file is part of the natcap.
 *
 * natcap is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * natcap is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with natcap; see the file COPYING. If not, see
 * <http://www.gnu.org/licenses/>.
 */
#include <linux/ctype.h>
#include <linux/device.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <linux/if_arp.h>
#include <linux/init.h>
#include <linux/ip.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/inetdevice.h>
#include <linux/netfilter.h>
#include <linux/skbuff.h>
#include <linux/string.h>
#include <linux/tcp.h>
#include <linux/udp.h>
#include <linux/version.h>
#include <linux/vmalloc.h>
#include <net/netfilter/nf_conntrack.h>
#include <net/netfilter/nf_conntrack_helper.h>
#include <net/netfilter/nf_conntrack_acct.h>
#include "natcap_common.h"
#include "natcap_peer.h"
#include "natcap_client.h"

#define MAX_PEER_PORT_MAP 65536
static struct nf_conn **peer_port_map;

static int peer_port_map_init(void)
{
	peer_port_map = vmalloc(sizeof(struct nf_conn *) * MAX_PEER_PORT_MAP);
	if (peer_port_map == NULL) {
		return -ENOMEM;
	}
	memset(peer_port_map, 0, sizeof(struct nf_conn *) * MAX_PEER_PORT_MAP);

	return 0;
}

static void peer_port_map_exit(void)
{
	int i;

	for (i = 0; i < MAX_PEER_PORT_MAP; i++) {
		if (peer_port_map[i] != NULL) {
			nf_ct_put(peer_port_map[i]);
			peer_port_map[i] = NULL;
		}
	}
}

static __be16 alloc_peer_port(struct nf_conn *ct, const unsigned char *mac)
{
	static unsigned int seed_rnd;
	unsigned short port;
	unsigned int hash;
	unsigned int data = get_byte4(mac);

	get_random_once(&seed_rnd, sizeof(seed_rnd));

	hash = jhash2(&data, 1, get_byte2(mac + 4)^seed_rnd);

	port = 1024 + hash % (MAX_PEER_PORT_MAP - 1024);

	for (; port < MAX_PEER_PORT_MAP - 1; port++) {
		if (peer_port_map[port] == NULL) {
			peer_port_map[port] = ct;
			nf_conntrack_get(&ct->ct_general);
			return htons(port);
		}
	}

	for (port = 1024; port < 1024 + hash % (MAX_PEER_PORT_MAP - 1024); port++) {
		if (peer_port_map[port] == NULL) {
			peer_port_map[port] = ct;
			nf_conntrack_get(&ct->ct_general);
			return htons(port);
		}
	}

	return 0;
}

#define MAX_PEER_SERVER 8
struct peer_server_node peer_server[MAX_PEER_SERVER];

static struct sk_buff *peer_user_uskbs[NR_CPUS];
#define PEER_USKB_SIZE (sizeof(struct iphdr) + sizeof(struct udphdr))
#define PEER_FAKEUSER_DADDR __constant_htonl(0x7ffffffe)

static inline struct sk_buff *uskb_of_this_cpu(int id)
{
	BUG_ON(id >= NR_CPUS);
	if (!peer_user_uskbs[id]) {
		peer_user_uskbs[id] = __alloc_skb(PEER_USKB_SIZE, GFP_ATOMIC, 0, numa_node_id());
	}
	return peer_user_uskbs[id];
}

#define NATCAP_PEER_USER_TIMEOUT 180

void natcap_user_timeout_touch(struct nf_conn *ct, unsigned long timeout)
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 9, 0)
		unsigned long newtimeout = jiffies + timeout * HZ;
		if (newtimeout - ct->timeout.expires > HZ) {
			mod_timer_pending(&ct->timeout, newtimeout);
		}
#else
		ct->timeout = jiffies + timeout * HZ;
#endif
}

struct nf_conn *peer_client_expect_in(__be32 saddr, __be32 daddr, __be16 sport, __be16 dport, int pi, __be32 *seq)
{
	struct nf_conn *user;
	struct nf_ct_ext *new = NULL;
	enum ip_conntrack_info ctinfo;
	unsigned int newoff = 0;
	int ret;
	struct sk_buff *uskb;
	struct iphdr *iph;
	struct udphdr *udph;

	uskb = uskb_of_this_cpu(smp_processor_id());
	if (uskb == NULL) {
		return NULL;
	}
	skb_reset_transport_header(uskb);
	skb_reset_network_header(uskb);
	skb_reset_mac_len(uskb);

	uskb->protocol = __constant_htons(ETH_P_IP);
	skb_set_tail_pointer(uskb, PEER_USKB_SIZE);
	uskb->len = PEER_USKB_SIZE;
	uskb->pkt_type = PACKET_HOST;
	uskb->transport_header = uskb->network_header + sizeof(struct iphdr);

	iph = ip_hdr(uskb);
	iph->version = 4;
	iph->ihl = 5;
	iph->saddr = saddr;
	iph->daddr = daddr;
	iph->tos = 0;
	iph->tot_len = htons(PEER_USKB_SIZE);
	iph->ttl=255;
	iph->protocol = IPPROTO_UDP;
	iph->id = __constant_htons(0xDEAD);
	iph->frag_off = 0;
	iph->check = 0;
	iph->check = ip_fast_csum(iph, iph->ihl);

	udph = (struct udphdr *)((char *)iph + sizeof(struct iphdr));
	udph->source = sport;
	udph->dest = dport;
	udph->len = __constant_htons(sizeof(struct udphdr));
	udph->check = 0;

	ret = nf_conntrack_in(&init_net, PF_INET, NF_INET_PRE_ROUTING, uskb);
	if (ret != NF_ACCEPT) {
		return NULL;
	}
	user = nf_ct_get(uskb, &ctinfo);

	if (!user) {
		NATCAP_ERROR("fakeuser create for ct[%pI4:%u->%pI4:%u] failed\n", &saddr, ntohs(sport), &daddr, ntohs(dport));
		return NULL;
	}

	if (!user->ext) {
		NATCAP_ERROR("fakeuser create for ct[%pI4:%u->%pI4:%u] failed, user->ext is NULL\n", &saddr, ntohs(sport), &daddr, ntohs(dport));
		skb_nfct_reset(uskb);
		return NULL;
	}
	if (!nf_ct_is_confirmed(user) && !(IPS_NATCAP_PEER & user->status) && !test_and_set_bit(IPS_NATCAP_PEER_BIT, &user->status)) {
		newoff = ALIGN(user->ext->len, __ALIGN_64BITS);
		new = __krealloc(user->ext, newoff + sizeof(struct fakeuser_expect), GFP_ATOMIC);
		if (!new) {
			NATCAP_ERROR("fakeuser create for ct[%pI4:%u->%pI4:%u] failed, realloc user->ext failed\n", &saddr, ntohs(sport), &daddr, ntohs(dport));
			skb_nfct_reset(uskb);
			return NULL;
		}

		if (user->ext != new) {
			kfree_rcu(user->ext, rcu);
			rcu_assign_pointer(user->ext, new);
		}
		new->len = newoff;
		memset((void *)new + newoff, 0, sizeof(struct fakeuser_expect));

		peer_fakeuser_expect(user)->pi = pi;
		if (seq) {
			//XXX BUG_ON(seq == NULL); just make happy
			peer_fakeuser_expect(user)->local_seq = ntohl(*seq);
		}
	}

	ret = nf_conntrack_confirm(uskb);
	if (ret != NF_ACCEPT) {
		skb_nfct_reset(uskb);
		return NULL;
	}

	skb_nfct_reset(uskb);

	if (seq) {
		*seq = htonl(peer_fakeuser_expect(user)->local_seq);
	}
	NATCAP_INFO("fakeuser create user[%pI4:%u->%pI4:%u] pi=%d upi=%d\n", &saddr, ntohs(sport), &daddr, ntohs(dport), pi, peer_fakeuser_expect(user)->pi);

	return user;
}

int peer_user_expect_in(__be32 saddr, __be32 daddr, __be16 sport, __be16 dport, const unsigned char *client_mac)
{
	int i;
	int ret;
	struct peer_tuple *pt = NULL;
	struct user_expect *ue;
	struct nf_conn *user;
	struct nf_ct_ext *new = NULL;
	enum ip_conntrack_info ctinfo;
	unsigned int newoff = 0;
	struct sk_buff *uskb;
	struct iphdr *iph;
	struct udphdr *udph;
	unsigned long last_jiffies = jiffies;

	uskb = uskb_of_this_cpu(smp_processor_id());
	if (uskb == NULL) {
		return -1;
	}
	skb_reset_transport_header(uskb);
	skb_reset_network_header(uskb);
	skb_reset_mac_len(uskb);

	uskb->protocol = __constant_htons(ETH_P_IP);
	skb_set_tail_pointer(uskb, PEER_USKB_SIZE);
	uskb->len = PEER_USKB_SIZE;
	uskb->pkt_type = PACKET_HOST;
	uskb->transport_header = uskb->network_header + sizeof(struct iphdr);

	iph = ip_hdr(uskb);
	iph->version = 4;
	iph->ihl = 5;
	iph->saddr = get_byte4(client_mac);
	iph->daddr = PEER_FAKEUSER_DADDR;
	iph->tos = 0;
	iph->tot_len = htons(PEER_USKB_SIZE);
	iph->ttl=255;
	iph->protocol = IPPROTO_UDP;
	iph->id = __constant_htons(0xDEAD);
	iph->frag_off = 0;
	iph->check = 0;
	iph->check = ip_fast_csum(iph, iph->ihl);

	udph = (struct udphdr *)((char *)iph + sizeof(struct iphdr));
	udph->source = get_byte2(client_mac + 4);
	udph->dest = __constant_htons(65535);
	udph->len = __constant_htons(sizeof(struct udphdr));
	udph->check = 0;

	ret = nf_conntrack_in(&init_net, PF_INET, NF_INET_PRE_ROUTING, uskb);
	if (ret != NF_ACCEPT) {
		return -1;
	}
	user = nf_ct_get(uskb, &ctinfo);

	if (!user) {
		NATCAP_ERROR("user [%02X:%02X:%02X:%02X:%02X:%02X] ct[%pI4:%u->%pI4:%u] failed\n",
				client_mac[0], client_mac[1], client_mac[2], client_mac[3], client_mac[4], client_mac[5],
				&saddr, ntohs(sport), &daddr, ntohs(dport));
		return -1;
	}

	if (!user->ext) {
		NATCAP_ERROR("user [%02X:%02X:%02X:%02X:%02X:%02X] ct[%pI4:%u->%pI4:%u] failed, user->ext is NULL\n",
				client_mac[0], client_mac[1], client_mac[2], client_mac[3], client_mac[4], client_mac[5],
				&saddr, ntohs(sport), &daddr, ntohs(dport));
		skb_nfct_reset(uskb);
		return -1;
	}
	if (!nf_ct_is_confirmed(user) && !(IPS_NATCAP_PEER & user->status) && !test_and_set_bit(IPS_NATCAP_PEER_BIT, &user->status)) {
		newoff = ALIGN(user->ext->len, __ALIGN_64BITS);
		new = __krealloc(user->ext, newoff + sizeof(struct user_expect), GFP_ATOMIC);
		if (!new) {
			NATCAP_ERROR("user [%02X:%02X:%02X:%02X:%02X:%02X] ct[%pI4:%u->%pI4:%u] failed, realloc user->ext failed\n",
					client_mac[0], client_mac[1], client_mac[2], client_mac[3], client_mac[4], client_mac[5],
					&saddr, ntohs(sport), &daddr, ntohs(dport));
			skb_nfct_reset(uskb);
			return -1;
		}

		if (user->ext != new) {
			kfree_rcu(user->ext, rcu);
			rcu_assign_pointer(user->ext, new);
		}
		new->len = newoff;
		memset((void *)new + newoff, 0, sizeof(struct user_expect));

		peer_user_expect(user)->ip = saddr;
		peer_user_expect(user)->map_port = alloc_peer_port(user, client_mac);
	}

	ret = nf_conntrack_confirm(uskb);
	if (ret != NF_ACCEPT) {
		skb_nfct_reset(uskb);
		return -1;
	}

	ue = peer_user_expect(user);
	ue->last_active = last_jiffies;

	for (i = 0; i < MAX_PEER_TUPLE; i++) {
		if (ue->tuple[i].sip == saddr && ue->tuple[i].dip == daddr && ue->tuple[i].sport == sport && ue->tuple[i].dport) {
			pt = &ue->tuple[i];
			pt->last_active = last_jiffies;
		}
	}
	if (pt == NULL) {
		unsigned long maxdiff = 0;
		for (i = 0; i < MAX_PEER_TUPLE; i++) {
			if (maxdiff < (last_jiffies - ue->tuple[i].last_active)) {
				maxdiff = last_jiffies - ue->tuple[i].last_active;
				pt = &ue->tuple[i];
			}
		}
		if (pt) {
			NATCAP_INFO("user [%02X:%02X:%02X:%02X:%02X:%02X] ct[%pI4:%u->%pI4:%u] @map_port=%u new session in\n",
					client_mac[0], client_mac[1], client_mac[2], client_mac[3], client_mac[4], client_mac[5],
					&saddr, ntohs(sport), &daddr, ntohs(dport), ntohs(ue->map_port));
			pt->sip = saddr;
			pt->dip = daddr;
			pt->sport = sport;
			pt->dport = dport;
			pt->last_active = last_jiffies;
		}
	}

	if (user != peer_port_map[ntohs(ue->map_port)]) {
		ue->map_port = alloc_peer_port(user, client_mac);
		NATCAP_INFO("user [%02X:%02X:%02X:%02X:%02X:%02X] ct[%pI4:%u->%pI4:%u] @map_port=%u reuse update mapping\n",
				client_mac[0], client_mac[1], client_mac[2], client_mac[3], client_mac[4], client_mac[5],
				&saddr, ntohs(sport), &daddr, ntohs(dport), ntohs(ue->map_port));
	}

	skb_nfct_reset(uskb);

	natcap_user_timeout_touch(user, NATCAP_PEER_USER_TIMEOUT);

	return 0;
}

static inline void natcap_peer_reply_pong(const struct net_device *dev, struct sk_buff *oskb)
{
	struct sk_buff *nskb;
	struct ethhdr *neth, *oeth;
	struct iphdr *niph, *oiph;
	struct tcphdr *otcph, *ntcph;
	struct natcap_TCPOPT *tcpopt;
	int offset, header_len;
	int add_len = ALIGN(sizeof(struct natcap_TCPOPT_header) + sizeof(struct natcap_TCPOPT_peer), sizeof(unsigned int));

	oeth = (struct ethhdr *)skb_mac_header(oskb);
	oiph = ip_hdr(oskb);
	otcph = (struct tcphdr *)((void *)oiph + oiph->ihl * 4);

	offset = sizeof(struct iphdr) + sizeof(struct tcphdr) + add_len + TCPOLEN_MSS - oskb->len;
	header_len = offset < 0 ? 0 : offset;
	nskb = skb_copy_expand(oskb, skb_headroom(oskb), header_len, GFP_ATOMIC);
	if (!nskb) {
		NATCAP_ERROR(DEBUG_FMT_PREFIX "alloc_skb fail\n", DEBUG_ARG_PREFIX);
		return;
	}
	if (offset <= 0) {
		if (pskb_trim(nskb, nskb->len + offset)) {
			NATCAP_ERROR(DEBUG_FMT_PREFIX "pskb_trim fail: len=%d, offset=%d\n", DEBUG_ARG_PREFIX, nskb->len, offset);
			consume_skb(nskb);
			return;
		}
	} else {
		nskb->len += offset;
		nskb->tail += offset;
	}

	neth = eth_hdr(nskb);
	memcpy(neth->h_dest, oeth->h_source, ETH_ALEN);
	memcpy(neth->h_source, oeth->h_dest, ETH_ALEN);
	//neth->h_proto = htons(ETH_P_IP);

	niph = ip_hdr(nskb);
	memset(niph, 0, sizeof(struct iphdr));
	niph->saddr = oiph->daddr;
	niph->daddr = oiph->saddr;
	niph->version = oiph->version;
	niph->ihl = 5;
	niph->tos = 0;
	niph->tot_len = htons(nskb->len);
	niph->ttl = 255;
	niph->protocol = IPPROTO_TCP;
	niph->id = __constant_htons(0xDEAD);
	niph->frag_off = 0x0;

	ntcph = (struct tcphdr *)((char *)ip_hdr(nskb) + sizeof(struct iphdr));
	ntcph->source = otcph->dest;
	ntcph->dest = otcph->source;
	ntcph->seq = jiffies;
	ntcph->ack_seq = htonl(ntohl(otcph->seq) + ntohs(oiph->tot_len) - oiph->ihl * 4 - otcph->doff * 4 + 1);
	ntcph->res1 = 0;
	ntcph->doff = (sizeof(struct tcphdr) + add_len + TCPOLEN_MSS) / 4;
	ntcph->syn = 1;
	ntcph->rst = 0;
	ntcph->psh = 0;
	ntcph->ack = 1;
	ntcph->fin = 0;
	ntcph->urg = 0;
	ntcph->ece = 0;
	ntcph->cwr = 0;
	ntcph->window = __constant_htons(65535);
	ntcph->check = 0;
	ntcph->urg_ptr = 0;

	tcpopt = (struct natcap_TCPOPT *)((void *)ntcph + sizeof(struct tcphdr));
	tcpopt->header.type = NATCAP_TCPOPT_TYPE_PEER;
	tcpopt->header.opcode = TCPOPT_PEER;
	tcpopt->header.opsize = add_len;
	tcpopt->header.encryption = 0;
	tcpopt->peer.data.ip = niph->saddr;
	memcpy(tcpopt->peer.data.mac_addr, default_mac_addr, ETH_ALEN);
	//just set a mss we do not care what it is
	set_byte1((void *)tcpopt + add_len + 0, TCPOPT_MSS);
	set_byte1((void *)tcpopt + add_len + 1, TCPOLEN_MSS);
	set_byte2((void *)tcpopt + add_len + 2, ntohs(IPV4_MIN_MTU - (sizeof(struct iphdr) + sizeof(struct tcphdr))));

	nskb->ip_summed = CHECKSUM_UNNECESSARY;
	skb_rcsum_tcpudp(nskb);

	skb_push(nskb, (char *)niph - (char *)neth);
	nskb->dev = (struct net_device *)dev;

	dev_queue_xmit(nskb);
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 13, 0)
static unsigned int natcap_peer_pre_in_hook(unsigned int hooknum,
		struct sk_buff *skb,
		const struct net_device *in,
		const struct net_device *out,
		int (*okfn)(struct sk_buff *))
{
	//u_int8_t pf = PF_INET;
#elif LINUX_VERSION_CODE < KERNEL_VERSION(4, 1, 0)
static unsigned int natcap_peer_pre_in_hook(const struct nf_hook_ops *ops,
		struct sk_buff *skb,
		const struct net_device *in,
		const struct net_device *out,
		int (*okfn)(struct sk_buff *))
{
	//u_int8_t pf = ops->pf;
	unsigned int hooknum = ops->hooknum;
#elif LINUX_VERSION_CODE < KERNEL_VERSION(4, 4, 0)
static unsigned int natcap_peer_pre_in_hook(const struct nf_hook_ops *ops,
		struct sk_buff *skb,
		const struct nf_hook_state *state)
{
	//u_int8_t pf = state->pf;
	unsigned int hooknum = state->hook;
	const struct net_device *in = state->in;
	const struct net_device *out = state->out;
#else
static unsigned int natcap_peer_pre_in_hook(void *priv,
		struct sk_buff *skb,
		const struct nf_hook_state *state)
{
	//u_int8_t pf = state->pf;
	unsigned int hooknum = state->hook;
	const struct net_device *in = state->in;
	const struct net_device *out = state->out;
#endif
	struct iphdr *iph;
	void *l4;
	struct net *net = &init_net;
	struct natcap_TCPOPT *tcpopt;
	int ret;
	int size;

	if (in)
		net = dev_net(in);
	else if (out)
		net = dev_net(out);

	iph = ip_hdr(skb);
	if (iph->protocol != IPPROTO_TCP) {
		return NF_ACCEPT;
	}
	if (skb->len < iph->ihl * 4 + sizeof(struct tcphdr)) {
		return NF_ACCEPT;
	}
	if (!pskb_may_pull(skb, iph->ihl * 4 + sizeof(struct tcphdr))) {
		return NF_ACCEPT;
	}
	iph = ip_hdr(skb);
	l4 = (void *)iph + iph->ihl * 4;

	if (!TCPH(l4)->syn) {
		return NF_ACCEPT;
	}
	size = ALIGN(sizeof(struct natcap_TCPOPT_header) + sizeof(struct natcap_TCPOPT_peer), sizeof(unsigned int));
	if (!pskb_may_pull(skb, iph->ihl * 4 + sizeof(struct tcphdr) + size)) {
		return NF_ACCEPT;
	}
	iph = ip_hdr(skb);
	l4 = (void *)iph + iph->ihl * 4;
	tcpopt = (struct natcap_TCPOPT *)(l4 + sizeof(struct tcphdr));
	if (tcpopt->header.type != NATCAP_TCPOPT_TYPE_PEER || tcpopt->header.opcode != TCPOPT_PEER) {
		return NF_ACCEPT;
	}

	if (TCPH(l4)->ack) {
		//got syn ack
		struct nf_conntrack_tuple tuple;
		struct nf_conntrack_tuple_hash *h;

		memset(&tuple, 0, sizeof(tuple));
		tuple.src.u3.ip = iph->saddr;
		tuple.src.u.udp.port = TCPH(l4)->source;
		tuple.dst.u3.ip = iph->daddr;
		tuple.dst.u.udp.port = TCPH(l4)->dest;
		tuple.src.l3num = PF_INET;
		tuple.dst.protonum = IPPROTO_UDP;

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 3, 0)
		h = nf_conntrack_find_get(net, NF_CT_DEFAULT_ZONE, &tuple);
#else
		h = nf_conntrack_find_get(net, &nf_ct_zone_dflt, &tuple);
#endif
		if (h) {
			struct nf_conn *ct = nf_ct_tuplehash_to_ctrack(h);
			struct nf_conn *user;

			if ((IPS_NATCAP_PEER & ct->status) && NF_CT_DIRECTION(h) == IP_CT_DIR_REPLY) {
				//
				user = peer_client_expect_in(iph->saddr, iph->daddr, TCPH(l4)->source, TCPH(l4)->dest, 0, NULL);
				if (ct == user) {
					NATCAP_INFO("(PPI)" DEBUG_TCP_FMT ": get pong in\n", DEBUG_TCP_ARG(iph,l4));
					//TODO send ack back?
					peer_fakeuser_expect(user)->remote_seq = ntohl(TCPH(l4)->seq);
					nf_ct_put(ct);
					consume_skb(skb);
					return NF_STOLEN;
				}
			}
			nf_ct_put(ct);
		}
	} else {
		//got syn
		__be32 client_ip;
		unsigned char client_mac[ETH_ALEN];

		client_ip = tcpopt->peer.data.ip;
		memcpy(client_mac, tcpopt->peer.data.mac_addr, ETH_ALEN);

		ret = peer_user_expect_in(iph->saddr, iph->daddr, TCPH(l4)->source, TCPH(l4)->dest, client_mac);
		if (ret == 0) {
			//XXX send syn ack back
			NATCAP_INFO("(PPI)" DEBUG_TCP_FMT ": send pong out\n", DEBUG_TCP_ARG(iph,l4));
			natcap_peer_reply_pong(in, skb);
		}

		consume_skb(skb);
		return NF_STOLEN;
	}

	return NF_ACCEPT;
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 13, 0)
static unsigned int natcap_peer_post_out_hook(unsigned int hooknum,
		struct sk_buff *skb,
		const struct net_device *in,
		const struct net_device *out,
		int (*okfn)(struct sk_buff *))
{
	//u_int8_t pf = PF_INET;
#elif LINUX_VERSION_CODE < KERNEL_VERSION(4, 1, 0)
static unsigned int natcap_peer_post_out_hook(const struct nf_hook_ops *ops,
		struct sk_buff *skb,
		const struct net_device *in,
		const struct net_device *out,
		int (*okfn)(struct sk_buff *))
{
	//u_int8_t pf = ops->pf;
	unsigned int hooknum = ops->hooknum;
#elif LINUX_VERSION_CODE < KERNEL_VERSION(4, 4, 0)
static unsigned int natcap_peer_post_out_hook(const struct nf_hook_ops *ops,
		struct sk_buff *skb,
		const struct nf_hook_state *state)
{
	//u_int8_t pf = state->pf;
	unsigned int hooknum = state->hook;
	//const struct net_device *in = state->in;
	//const struct net_device *out = state->out;
#else
static unsigned int natcap_peer_post_out_hook(void *priv,
		struct sk_buff *skb,
		const struct nf_hook_state *state)
{
	//u_int8_t pf = state->pf;
	unsigned int hooknum = state->hook;
	//const struct net_device *in = state->in;
	//const struct net_device *out = state->out;
#endif
	//int ret = 0;
	struct nf_conn *ct;
	struct sk_buff *skb2;
	struct iphdr *iph;
	void *l4;
	//struct net *net = &init_net;
	struct natcap_TCPOPT *tcpopt;
	int offset, header_len;
	int size;
	struct peer_server_node *ps = NULL;
	int psi, pi;
	unsigned int mss;

	//if (disabled)
	//	return NF_ACCEPT;

	//if (in)
	//	net = dev_net(in);
	//else if (out)
	//	net = dev_net(out);

	iph = ip_hdr(skb);
	if (iph->protocol != IPPROTO_ICMP) {
		return NF_ACCEPT;
	}
	/*
	if (iph->daddr != peer_icmp_dst) {
		return NF_ACCEPT;
	}
	*/
	if (iph->ttl != 1) {
		return NF_ACCEPT;
	}
	l4 = (void *)iph + iph->ihl * 4;
	for (psi = 0; psi < MAX_PEER_SERVER; psi++) {
		if (peer_server[psi].ip == iph->daddr) {
			ps = &peer_server[psi];
			break;
		}
	}
	if (ps == NULL) {
		for (psi = 0; psi < MAX_PEER_SERVER; psi++) {
			if (peer_server[psi].ip == 0) {
				peer_server[psi].ip = iph->daddr;
				ps = &peer_server[psi];
				break;
			}
		}
	}
	if (ps == NULL) {
		return NF_STOLEN;
	}
	pi = ntohs(ICMPH(l4)->un.echo.sequence) % MAX_PEER_SERVER_PORT;
	if (ps->port_map[pi].sport == 0) {
		ps->port_map[pi].sport = htons(1024 + prandom_u32() % (65535 - 1024 + 1));
		ps->port_map[pi].dport = htons(1024 + prandom_u32() % (65535 - 1024 + 1));
	}

	NATCAP_INFO("(PPO)" DEBUG_ICMP_FMT ": ping out\n", DEBUG_ICMP_ARG(iph,l4));

	size = ALIGN(sizeof(struct natcap_TCPOPT_header) + sizeof(struct natcap_TCPOPT_peer), sizeof(unsigned int));
	offset = iph->ihl * 4 + sizeof(struct tcphdr) + size + TCPOLEN_MSS - skb->len;
	header_len = offset < 0 ? 0 : offset;
	skb2 = skb_copy_expand(skb, skb_headroom(skb), header_len, GFP_ATOMIC);
	if (!skb2) {
		NATCAP_ERROR(DEBUG_FMT_PREFIX "alloc_skb fail\n", DEBUG_ARG_PREFIX);
		return NF_DROP;
	}
	if (offset <= 0) {
		if (pskb_trim(skb2, skb2->len + offset)) {
			NATCAP_ERROR(DEBUG_FMT_PREFIX "pskb_trim fail: len=%d, offset=%d\n", DEBUG_ARG_PREFIX, skb2->len, offset);
			consume_skb(skb2);
			return NF_DROP;
		}
	} else {
		skb2->len += offset;
		skb2->tail += offset;
	}

	skb_nfct_reset(skb2);
	iph = ip_hdr(skb2);
	l4 = (void *)iph + iph->ihl * 4;

	skb2->protocol = htons(ETH_P_IP);
	iph->protocol = IPPROTO_TCP;
	iph->daddr = ps->ip;
	iph->tot_len = htons(skb2->len);
	iph->ttl = 255;

	TCPH(l4)->source = ps->port_map[pi].sport;
	TCPH(l4)->dest = ps->port_map[pi].dport;
	TCPH(l4)->seq = htonl(jiffies);
	TCPH(l4)->ack_seq = 0;
	TCPH(l4)->res1 = 0;
	TCPH(l4)->doff = (sizeof(struct tcphdr) + size + TCPOLEN_MSS) / 4;
	TCPH(l4)->syn = 1;
	TCPH(l4)->rst = 0;
	TCPH(l4)->psh = 0;
	TCPH(l4)->ack = 0;
	TCPH(l4)->fin = 0;
	TCPH(l4)->urg = 0;
	TCPH(l4)->ece = 0;
	TCPH(l4)->cwr = 0;
	TCPH(l4)->window = __constant_htons(65535);
	TCPH(l4)->check = 0;
	TCPH(l4)->urg_ptr = 0;

	tcpopt = (struct natcap_TCPOPT *)(l4 + sizeof(struct tcphdr));
	tcpopt->header.type = NATCAP_TCPOPT_TYPE_PEER;
	tcpopt->header.opcode = TCPOPT_PEER;
	tcpopt->header.opsize = size;
	tcpopt->header.encryption = 0;
	tcpopt->peer.data.ip = iph->saddr;
	memcpy(tcpopt->peer.data.mac_addr, default_mac_addr, ETH_ALEN);

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 7, 0)
	mss = ip_skb_dst_mtu(skb2);
#else
	mss = ip_skb_dst_mtu(NULL, skb2);
#endif
	if (mss < IPV4_MIN_MTU) {
		mss = IPV4_MIN_MTU;
	}
	mss = mss - (sizeof(struct iphdr) + sizeof(struct tcphdr));

	//MUST set mss
	set_byte1((void *)tcpopt + size + 0, TCPOPT_MSS);
	set_byte1((void *)tcpopt + size + 1, TCPOLEN_MSS);
	set_byte2((void *)tcpopt + size + 2, ntohs(mss));

	iph = ip_hdr(skb2);
	l4 = (void *)iph + iph->ihl * 4;

	ct = peer_client_expect_in(iph->saddr, iph->daddr, TCPH(l4)->source, TCPH(l4)->dest, pi, &TCPH(l4)->seq);
	if (ct == NULL) {
		consume_skb(skb2);
		goto out;
	}

	skb_rcsum_tcpudp(skb2);

	NF_OKFN(skb2);

out:
	consume_skb(skb);
	return NF_STOLEN;
}

static struct nf_hook_ops peer_hooks[] = {
	{
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 4, 0)
		.owner = THIS_MODULE,
#endif
		.hook = natcap_peer_pre_in_hook,
		.pf = PF_INET,
		.hooknum = NF_INET_PRE_ROUTING,
		.priority = NF_IP_PRI_CONNTRACK - 5,
	},
	{
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 4, 0)
		.owner = THIS_MODULE,
#endif
		.hook = natcap_peer_post_out_hook,
		.pf = PF_INET,
		.hooknum = NF_INET_POST_ROUTING,
		.priority = NF_IP_PRI_LAST - 5,
	},
};

int natcap_peer_init(void)
{
	int i;
	int ret = 0;

	need_conntrack();
	memset(peer_server, 0, sizeof(peer_server));

	for (i = 0; i < NR_CPUS; i++) {
		peer_user_uskbs[i] = NULL;
	}

	if (mode == PEER_MODE) {
		default_mac_addr_init();
	}

	ret = peer_port_map_init();
	if (ret != 0)
		goto peer_port_map_init_failed;

	ret = nf_register_hooks(peer_hooks, ARRAY_SIZE(peer_hooks));
	if (ret != 0)
		goto nf_register_hooks_failed;

	return 0;

nf_register_hooks_failed:
	peer_port_map_exit();
peer_port_map_init_failed:
	return ret;
}

void natcap_peer_exit(void)
{
	int i;

	nf_unregister_hooks(peer_hooks, ARRAY_SIZE(peer_hooks));

	for (i = 0; i < NR_CPUS; i++) {
		if (peer_user_uskbs[i]) {
			kfree(peer_user_uskbs[i]);
			peer_user_uskbs[i] = NULL;
		}
	}

	peer_port_map_exit();
}
