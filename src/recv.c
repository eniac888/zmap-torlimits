/*
 * ZMap Copyright 2013 Regents of the University of Michigan
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may not
 * use this file except in compliance with the License. You may obtain a copy
 * of the License at http://www.apache.org/licenses/LICENSE-2.0
 */
#include "recv.h"

#include <assert.h>

#include "../lib/includes.h"
#include "../lib/logger.h"
#include "../lib/pbm.h"

#include <pthread.h>
#include <unistd.h>

#include "recv-internal.h"
#include "state.h"
#include "validate.h"
#include "fieldset.h"
#include "expression.h"
#include "probe_modules/probe_modules.h"
#include "output_modules/output_modules.h"

static u_char fake_eth_hdr[65535];

// bitmap of observed IP addresses
static uint8_t **seen = NULL;

void handle_packet(uint32_t buflen, const u_char *bytes) {
	if ((sizeof(struct ip) + (zconf.send_ip_pkts ? 0 : sizeof(struct ether_header))) > buflen) {
		// buffer not large enough to contain ethernet
		// and ip headers. further action would overrun buf
		return;
	}
	struct ip *ip_hdr = (struct ip *) &bytes[(zconf.send_ip_pkts ? 0 : sizeof(struct ether_header))];
    
	uint32_t src_ip = ip_hdr->ip_src.s_addr;
    
	uint32_t validation[VALIDATE_BYTES/sizeof(uint8_t)];
    
    if (ip_hdr->ip_p == IPPROTO_TCP) {
	validate_gen(ip_hdr->ip_dst.s_addr, ip_hdr->ip_src.s_addr, (uint8_t *) validation);
    }
    
    else if (ip_hdr->ip_p == IPPROTO_ICMP) {
        
        //debug
        //log_warn("monitor","VALIDATE_ICMP_PKT");
        
        //Bano: Check if packet length is enough for validation
        uint32_t min_len = 4*ip_hdr->ip_hl + 8 + sizeof(struct ip) + sizeof(struct tcphdr);
        if (buflen < min_len) {
                // Not enough information for us to validate
                zrecv.icmp_badlen++;
                return;
            }
        
        struct icmp *icmp = (struct icmp*) ((char *) ip_hdr + 4*ip_hdr->ip_hl);
        
        // Bano: Handling only ICMP error messages
        
        if ((icmp->icmp_type != ICMP_UNREACH) && (icmp->icmp_type != ICMP_SOURCEQUENCH) && (icmp->icmp_type != ICMP_REDIRECT) && (icmp->icmp_type != ICMP_TIMXCEED) && (icmp->icmp_type != ICMP_PARAMPROB)) {
            //debug
            //log_warn("monitor","VALIDATE_ICMPTYPE_FAIL: bad type");
            return;
            }
        
        struct ip *ip_inner = (struct ip*) ((char *) icmp+8);
        
        //Bano: I modified this check slightly
        // Now we know the actual inner ip length, we should recheck the buffer
		/*
        if (buflen < 4*ip_inner->ip_hl - sizeof(struct ip) + min_len) {
			return;
		}
         */
        
        // Bano: Ours is a TCP syn scan!
        if (ip_inner->ip_p != IPPROTO_TCP) {
            //debug
            //log_warn("monitor","VALIDATE_ICMP_FAIL: not inner tcp");
            return;
        }
        
        struct in_addr inner_src_ip = ip_inner->ip_src;
        struct in_addr inner_dst_ip = ip_inner->ip_dst;
            
        validate_gen(inner_src_ip.s_addr, inner_dst_ip.s_addr, (uint8_t *) validation);
    }
    
	if (!zconf.probe_module->validate_packet(ip_hdr, buflen - (zconf.send_ip_pkts ? 0 : sizeof(struct ether_header)),
                                             &src_ip, validation)) {
        //debug
        //log_warn("monitor","VALIDATE_FAIL in recv.c");
		return;
	}
    
	int is_repeat = 0; //= pbm_check(seen, ntohl(src_ip));
    
	fieldset_t *fs = fs_new_fieldset();
	fs_add_ip_fields(fs, ip_hdr);
	// HACK:
	// probe modules (for whatever reason) expect the full ethernet frame
	// in process_packet. For VPN, we only get back an IP frame.
	// Here, we fake an ethernet frame (which is initialized to
	// have ETH_P_IP proto and 00s for dest/src).
	if (zconf.send_ip_pkts) {
		if (buflen > sizeof(fake_eth_hdr)) {
			buflen = sizeof(fake_eth_hdr);
		}
		memcpy(&fake_eth_hdr[sizeof(struct ether_header)], bytes, buflen);
		bytes = fake_eth_hdr;
	}
	zconf.probe_module->process_packet(bytes, buflen, fs);
    
    //Bano: This is hacky, ideally should add this in packet process function.
    // Also during postprocessing, remember to do validation+1 for tcp
    fs_add_uint64(fs, "validation", validation[0]);
    
	fs_add_system_fields(fs, is_repeat, zsend.complete);
	int success_index = zconf.fsconf.success_index;
	assert(success_index < fs->len);
	int is_success = fs_get_uint64_by_index(fs, success_index);
    
	if (is_success) {
		zrecv.success_total++;
		if (!is_repeat) {
			zrecv.success_unique++;
			pbm_set(seen, ntohl(src_ip));
		}
		if (zsend.complete) {
			zrecv.cooldown_total++;
			if (!is_repeat) {
				zrecv.cooldown_unique++;
			}
		}
	} else {
		zrecv.failure_total++;
	}
	// probe module includes app_success field
	if (zconf.fsconf.app_success_index >= 0) {
		int is_app_success = fs_get_uint64_by_index(fs,
                                                    zconf.fsconf.app_success_index);
		if (is_app_success) {
			zrecv.app_success_total++;
			if (!is_repeat) {
				zrecv.app_success_unique++;
			}
		}
	}
    
	fieldset_t *o = NULL;
	// we need to translate the data provided by the probe module
	// into a fieldset that can be used by the output module
	if (!is_success && zconf.filter_unsuccessful) {
		goto cleanup;
	}
	if (is_repeat && zconf.filter_duplicates) {
		goto cleanup;
	}
	if (!evaluate_expression(zconf.filter.expression, fs)) {
		goto cleanup;
	}
	o = translate_fieldset(fs, &zconf.fsconf.translation);
	if (zconf.output_module && zconf.output_module->process_ip) {
		zconf.output_module->process_ip(o);
	}
cleanup:
	fs_free(fs);
	free(o);
	if (zconf.output_module && zconf.output_module->update
        && !(zrecv.success_unique % zconf.output_module->update_interval)) {
		zconf.output_module->update(&zconf, &zsend, &zrecv);
	}
}

int recv_run(pthread_mutex_t *recv_ready_mutex)
{
	log_trace("recv", "recv thread started");
	log_debug("recv", "capturing responses on %s", zconf.iface);
	if (!zconf.dryrun) {
		recv_init();
	}
	if (zconf.send_ip_pkts) {
		struct ether_header *eth = (struct ether_header *) fake_eth_hdr;
		memset(fake_eth_hdr, 0, sizeof(fake_eth_hdr));
		eth->ether_type = htons(ETHERTYPE_IP);
	}
	// initialize paged bitmap
	seen = pbm_init();
	if (zconf.filter_duplicates) {
		log_debug("recv", "duplicate responses will be excluded from output");
	} else {
		log_debug("recv", "duplicate responses will be included in output");
	}
	if (zconf.filter_unsuccessful) {
		log_debug("recv", "unsuccessful responses will be excluded from output");
	} else {
		log_debug("recv", "unsuccessful responses will be included in output");
	}
    
	pthread_mutex_lock(recv_ready_mutex);
	zconf.recv_ready = 1;
	pthread_mutex_unlock(recv_ready_mutex);
	zrecv.start = now();

        // Bano: update stats and make first entry to the log file
        recv_update_stats();
        log_warn("monitor INITIAL", "total dropped (pcap: %u + iface: %u))",zrecv.pcap_drop,zrecv.pcap_ifdrop);
                         
	if (zconf.max_results == 0) {
		zconf.max_results = -1;
	}
    
	do {
		if (zconf.dryrun) {
			sleep(1);
		} else {
			recv_packets();
			if (zconf.max_results && zrecv.success_unique >= zconf.max_results) {
				break;
			}
		}
		/*
		if(zsend.complete)
                        {
                        lock_file(stdout);
                        fprintf(stdout,"^zsend.complete at %f\n",now());
                        unlock_file(stdout);
                        }
		*/
		} while (!(zsend.complete && (now()-zsend.finish > zconf.cooldown_secs)));

	//Bano: Uncomment for debugging
	/*
	if(now()-zsend.finish > zconf.cooldown_secs)
                        {
                        lock_file(stdout);
                        fprintf(stdout,"^zsend.cooldown done at %f\n",now());
                        unlock_file(stdout);
                        }
	*/
	zrecv.finish = now();

	// get final pcap statistics before closing
	recv_update_stats();
	if (!zconf.dryrun) {
		pthread_mutex_lock(recv_ready_mutex);
		recv_cleanup();
		pthread_mutex_unlock(recv_ready_mutex);
	}
	zrecv.complete = 1;
	log_debug("recv", "thread finished");
	return 0;
}
