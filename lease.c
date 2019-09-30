/* SPDX-License-Identifier: MIT
 *
 * Copyright (C) 2019 WireGuard LLC. All Rights Reserved.
 */
#define _GNU_SOURCE

#include <arpa/inet.h>
#include <inttypes.h>
#include <libmnl/libmnl.h>
#include <linux/rtnetlink.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <time.h>
#include <string.h>

#include "common.h"
#include "dbg.h"
#include "khash.h"
#include "lease.h"
#include "netlink.h"
#include "radix-trie.h"
#include "random.h"

#define TIME_T_MAX (((time_t)1 << (sizeof(time_t) * CHAR_BIT - 2)) - 1) * 2 + 1

static struct ip_pool pool;
static time_t gexpires = TIME_T_MAX;
static bool synchronized;

KHASH_MAP_INIT_WGKEY(leaseht, struct wg_dynamic_lease *)
khash_t(leaseht) *leases_ht = NULL;

static time_t get_monotonic_time()
{
	struct timespec monotime;
#ifdef __linux__
	if (clock_gettime(CLOCK_BOOTTIME, &monotime))
		fatal("clock_gettime(CLOCK_BOOTTIME)");
#else
	/* CLOCK_MONOTONIC works on openbsd, but apparently not (yet) on
	 * freebsd: https://lists.freebsd.org/pipermail/freebsd-hackers/2018-June/052899.html
	 */
	if (clock_gettime(CLOCK_MONOTONIC, &monotime))
		fatal("clock_gettime(CLOCK_MONOTONIC)");

#endif
	/* TODO: what about darwin? */

	return monotime.tv_sec;
}

void leases_init(char *fname, struct mnl_socket *nlsock)
{
	struct nlmsghdr *nlh;
	struct rtmsg *rtm;
	char buf[MNL_NLMSG_HDRLEN + MNL_ALIGN(sizeof *rtm)];
	unsigned int seq;

	synchronized = false;
	leases_ht = kh_init(leaseht);
	ipp_init(&pool);

	nlh = mnl_nlmsg_put_header(buf);
	nlh->nlmsg_type = RTM_GETROUTE;
	nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
	nlh->nlmsg_seq = seq = time(NULL);
	rtm = mnl_nlmsg_put_extra_header(nlh, sizeof(struct rtmsg));
	rtm->rtm_family = 0; /* both ipv4 and ipv6 */

	if (mnl_socket_sendto(nlsock, nlh, nlh->nlmsg_len) < 0)
		fatal("mnl_socket_sendto()");

	leases_update_pools(nlsock);
	synchronized = true;

	UNUSED(fname); /* TODO: open file and initialize from it */
}

void leases_free()
{
	if (leases_ht) {
		for (khint_t k = 0; k < kh_end(leases_ht); ++k)
			if (kh_exist(leases_ht, k))
				free((char *)kh_key(leases_ht, k));
	}
	kh_destroy(leaseht, leases_ht);

	ipp_free(&pool);
}

struct wg_dynamic_lease *new_lease(wg_key pubkey, uint32_t leasetime,
				   struct in_addr *ipv4, struct in6_addr *ipv6,
				   const struct in6_addr *lladdr)
{
	struct wg_dynamic_lease *lease, *parent;
	uint64_t index_l;
	uint32_t index, index_h;
	struct timespec tp;
	khiter_t k;
	int ret;
	bool wants_ipv4 = !ipv4 || ipv4->s_addr;
	bool wants_ipv6 = !ipv6 || !IN6_IS_ADDR_UNSPECIFIED(ipv6);

	lease = malloc(sizeof *lease);
	if (!lease)
		fatal("malloc()");

	if (wants_ipv4 && !pool.total_ipv4)
		return NULL; /* no ipv4 addresses available */

	if (wants_ipv6 && !pool.totalh_ipv6 && !pool.totall_ipv6)
		return NULL; /* no ipv6 addresses available */

	if (wants_ipv4) {
		if (!ipv4) {
			index = random_bounded(pool.total_ipv4 - 1);
			debug("new_lease(v4): %u of %u\n", index,
			      pool.total_ipv4);

			ipp_addnth_v4(&pool, &lease->ipv4, index);
		} else {
			if (ipp_add_v4(&pool, ipv4, 32))
				return NULL;

			memcpy(&lease->ipv4, ipv4, sizeof *ipv4);
		}
	}

	if (wants_ipv6) {
		if (!ipv6) {
			if (pool.totalh_ipv6 > 0) {
				index_l = random_bounded(UINT64_MAX);
				index_h = random_bounded(pool.totalh_ipv6 - 1);
			} else {
				index_l = random_bounded(pool.totall_ipv6 - 1);
				index_h = 0;
			}

			debug("new_lease(v6): %u:%ju of %u:%ju\n", index_h,
			      index_l, pool.totalh_ipv6, pool.totall_ipv6);
			ipp_addnth_v6(&pool, &lease->ipv6, index_l, index_h);
		} else {
			if (ipp_add_v6(&pool, ipv6, 128)) {
				if (!ipv4 || ipv4->s_addr)
					ipp_del_v4(&pool, ipv4, 32);

				return NULL;
			}

			memcpy(&lease->ipv6, ipv6, sizeof *ipv6);
		}
	}

	memcpy(&lease->lladdr, lladdr, sizeof(lease->lladdr));

	if (clock_gettime(CLOCK_REALTIME, &tp))
		fatal("clock_gettime(CLOCK_REALTIME)");

	lease->start_real = tp.tv_sec;
	lease->start_mono = get_monotonic_time();
	lease->leasetime = leasetime;
	lease->next = NULL;

	wg_key *pubcopy = malloc(sizeof(wg_key));
	if (!pubcopy)
		fatal("malloc()");

	memcpy(pubcopy, pubkey, sizeof(wg_key));
	k = kh_put(leaseht, leases_ht, *pubcopy, &ret);
	if (ret < 0) {
		fatal("kh_put()");
	} else if (ret == 0) {
		parent = kh_value(leases_ht, k);
		while (parent->next)
			parent = parent->next;

		parent->next = lease;
	} else {
		kh_value(leases_ht, k) = lease;
	}

	if (lease->start_mono + lease->leasetime < gexpires)
		gexpires = lease->start_mono + lease->leasetime;

	/* TODO: add record to file */

	return lease;
}

struct wg_dynamic_lease *get_leases(wg_key pubkey)
{
	khiter_t k = kh_get(leaseht, leases_ht, pubkey);

	if (k == kh_end(leases_ht))
		return NULL;
	else
		return kh_val(leases_ht, k);
}

bool extend_lease(struct wg_dynamic_lease *lease, uint32_t leasetime)
{
	UNUSED(lease);
	UNUSED(leasetime);
	return false;
}

struct allowedips_update {
	wg_key peer_pubkey;
	struct in_addr ipv4;
	struct in6_addr ipv6;
	struct in6_addr lladdr;
};

static char *updates_to_str(const struct allowedips_update *u)
{
	static char buf[4096];
	wg_key_b64_string pubkey_asc;
	char v4[INET_ADDRSTRLEN], v6[INET6_ADDRSTRLEN], ll[INET6_ADDRSTRLEN];


	if (!u)
		return "(null)";

	wg_key_to_base64(pubkey_asc, u->peer_pubkey);
	inet_ntop(AF_INET, &u->ipv4, v4, sizeof v4);
	inet_ntop(AF_INET6, &u->ipv6, v6, sizeof v6);
	inet_ntop(AF_INET6, &u->lladdr, ll, sizeof ll);
	snprintf(buf, sizeof buf, "(%p) %s [%s] [%s]", u, v4, v6, ll);

	return buf;
}

static void update_allowed_ips_bulk(const char *devname,
				    const struct allowedips_update *updates,
				    int nupdates)
{
	wg_peer peers[WG_DYNAMIC_LEASE_CHUNKSIZE] = { 0 };
	wg_allowedip allowedips[3 * WG_DYNAMIC_LEASE_CHUNKSIZE] = { 0 };
	wg_device dev = { 0 };
	wg_peer **pp = &dev.first_peer;

	int peer_idx = 0;
	int allowedips_idx = 0;
	for (int i = 0; i < nupdates; i++) {

		debug("setting allowedips for %s\n", updates_to_str(&updates[i]));

		peers[peer_idx].flags |= WGPEER_REPLACE_ALLOWEDIPS;
		memcpy(peers[peer_idx].public_key, updates[i].peer_pubkey, sizeof(wg_key));
		wg_allowedip **aipp = &peers[peer_idx].first_allowedip;

		/* TODO: move to a function */
		if (updates[i].ipv4.s_addr) {
			allowedips[allowedips_idx] = (wg_allowedip){
				.family = AF_INET,
				.cidr = 32,
				.ip4 = updates[i].ipv4,
			};
			*aipp = &allowedips[allowedips_idx];
			aipp = &allowedips[allowedips_idx].next_allowedip;
			++allowedips_idx;
		}
		if (!IN6_IS_ADDR_UNSPECIFIED(&updates[i].ipv6)) {
			allowedips[allowedips_idx] = (wg_allowedip){
				.family = AF_INET6,
				.cidr = 128,
				.ip6 = updates[i].ipv6,
			};
			*aipp = &allowedips[allowedips_idx];
			aipp = &allowedips[allowedips_idx].next_allowedip;
			++allowedips_idx;
		}
		if (!IN6_IS_ADDR_UNSPECIFIED(&updates[i].lladdr)) {
			allowedips[allowedips_idx] = (wg_allowedip){
				.family = AF_INET6,
				.cidr = 128,
				.ip6 = updates[i].lladdr,
			};
			*aipp = &allowedips[allowedips_idx];
			//aipp = &allowedips[allowedips_idx].next_allowedip;
			++allowedips_idx;
		}

		*pp = &peers[peer_idx];
		pp = &peers[peer_idx].next_peer;
		++peer_idx;
	}

	strncpy(dev.name, devname, sizeof(dev.name) - 1);
	if (wg_set_device(&dev))
		fatal("wg_set_device()");
}

void update_allowed_ips(const char *devname, wg_key peer_pubkey,
			const struct wg_dynamic_lease *lease)
{
	struct allowedips_update update;

	memcpy(update.peer_pubkey, peer_pubkey, sizeof(wg_key));
	memcpy(&update.ipv4, &lease->ipv4, sizeof(struct in_addr));
	memcpy(&update.ipv6, &lease->ipv6, sizeof(struct in6_addr));
	memcpy(&update.lladdr, &lease->lladdr, sizeof(struct in6_addr));

	update_allowed_ips_bulk(devname, &update, 1);
}

int leases_refresh(const char *devname)
{
	time_t cur_time = get_monotonic_time();
	struct allowedips_update updates[WG_DYNAMIC_LEASE_CHUNKSIZE] = { 0 };

	if (cur_time < gexpires)
		return MIN(INT_MAX / 1000, gexpires - cur_time);

	gexpires = TIME_T_MAX;

	int i = 0;
	for (khint_t k = kh_begin(leases_ht); k != kh_end(leases_ht); ++k) {
		if (!kh_exist(leases_ht, k))
			continue;

		struct wg_dynamic_lease **pp = &kh_val(leases_ht, k), *tmp;
		while (*pp) {
			struct in_addr *ipv4 = &(*pp)->ipv4;
			struct in6_addr *ipv6 = &(*pp)->ipv6;
			time_t expires = (*pp)->start_mono + (*pp)->leasetime;
			if (cur_time >= expires) {
				if (ipv4->s_addr)
					ipp_del_v4(&pool, ipv4, 32);

				if (!IN6_IS_ADDR_UNSPECIFIED(ipv6))
					ipp_del_v6(&pool, ipv6, 128);

				memcpy(updates[i].peer_pubkey, kh_key(leases_ht, k),
				       sizeof(wg_key));
				updates[i].lladdr = (*pp)->lladdr;
				{
					wg_key_b64_string pubkey_asc;
					wg_key_to_base64(pubkey_asc,
							 updates[i].peer_pubkey);
					debug("Peer losing its lease: %s\n",
					      pubkey_asc);
				}
				i++;
				if (i == WG_DYNAMIC_LEASE_CHUNKSIZE) {
					update_allowed_ips_bulk(devname, updates, i);
					i = 0;
					memset(updates, 0, sizeof updates);
				}

				tmp = *pp;
				*pp = (*pp)->next;
				free(tmp);
			} else {
				if (expires < gexpires)
					gexpires = expires;

				pp = &(*pp)->next;
			}
		}

		if (i)
			update_allowed_ips_bulk(devname, updates, i);

		if (!kh_val(leases_ht, k)) {
			free((char *)kh_key(leases_ht, k));
			kh_del(leaseht, leases_ht, k);
		}
	}

	return MIN(INT_MAX / 1000, gexpires - cur_time);
}

static int data_ipv4_attr_cb(const struct nlattr *attr, void *data)
{
	const struct nlattr **tb = data;
	int type = mnl_attr_get_type(attr);

	switch (type) {
	case RTA_DST:
	case RTA_GATEWAY:
		if (mnl_attr_validate(attr, MNL_TYPE_U32) < 0) {
			log_err("mnl_attr_validate: %s\n", strerror(errno));
			return MNL_CB_ERROR;
		}
		break;
	default:
		return MNL_CB_OK;
	}
	tb[type] = attr;
	return MNL_CB_OK;
}

static int data_ipv6_attr_cb(const struct nlattr *attr, void *data)
{
	const struct nlattr **tb = data;
	int type = mnl_attr_get_type(attr);

	switch (type) {
	case RTA_DST:
	case RTA_GATEWAY:
		if (mnl_attr_validate2(attr, MNL_TYPE_BINARY,
				       sizeof(struct in6_addr)) < 0) {
			log_err("mnl_attr_validate: %s\n", strerror(errno));
			return MNL_CB_ERROR;
		}
		break;
	default:
		return MNL_CB_OK;
	}
	tb[type] = attr;
	return MNL_CB_OK;
}

static int process_nlpacket_cb(const struct nlmsghdr *nlh, void *data)
{
	struct nlattr *tb[RTA_MAX + 1] = {};
	struct rtmsg *rm = mnl_nlmsg_get_payload(nlh);
	UNUSED(data);

	if (rm->rtm_family == AF_INET)
		mnl_attr_parse(nlh, sizeof(*rm), data_ipv4_attr_cb, tb);
	else if (rm->rtm_family == AF_INET6)
		mnl_attr_parse(nlh, sizeof(*rm), data_ipv6_attr_cb, tb);

	if (tb[RTA_GATEWAY])
		return MNL_CB_OK;

	if (!tb[RTA_DST]) {
		debug("Netlink packet without RTA_DST, ignoring\n");
		return MNL_CB_OK;
	}

	void *addr = mnl_attr_get_payload(tb[RTA_DST]);
	if (rm->rtm_family == AF_INET6 &&
	    (is_link_local(addr) || IN6_IS_ADDR_MULTICAST(addr)))
		return MNL_CB_OK;

	if (nlh->nlmsg_type == RTM_NEWROUTE) {
		if (rm->rtm_family == AF_INET) {
			if (ipp_addpool_v4(&pool, addr, rm->rtm_dst_len))
				die("ipp_addpool_v4()\n");
		} else if (rm->rtm_family == AF_INET6) {
			if (ipp_addpool_v6(&pool, addr, rm->rtm_dst_len))
				die("ipp_addpool_v6()\n");
		}
	} else if (nlh->nlmsg_type == RTM_DELROUTE) {
		if (rm->rtm_family == AF_INET) {
			if (ipp_removepool_v4(&pool, addr) && synchronized)
				die("ipp_removepool_v4()\n");
		} else if (rm->rtm_family == AF_INET6) {
			if (ipp_removepool_v6(&pool, addr) && synchronized)
				die("ipp_removepool_v6()\n");
		}
	}

	return MNL_CB_OK;
}

void leases_update_pools(struct mnl_socket *nlsock)
{
	int ret;
	char buf[MNL_SOCKET_BUFFER_SIZE];

	while ((ret = mnl_socket_recvfrom(nlsock, buf, sizeof buf)) > 0) {
		if (mnl_cb_run(buf, ret, 0, 0, process_nlpacket_cb, NULL) == -1)
			fatal("mnl_cb_run()");
	}

	if (ret == -1 && errno != EAGAIN && errno != EWOULDBLOCK)
		fatal("mnl_socket_recvfrom()");
}
