/*
 * Copyright (c) 2016, Network RADIUS SARL <license@networkradius.com>
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions are met:
 *    * Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *    * Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer in the
 *      documentation and/or other materials provided with the distribution.
 *    * Neither the name of Network RADIUS SARL nor the
 *      names of its contributors may be used to endorse or promote products
 *      derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL <COPYRIGHT HOLDER> BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/**
 * $Id$
 * @file rlm_sigtran/rlm_sigtran.c
 * @brief Implement a SCTP/M3UA/SCCP/TCAP/MAP stack
 *
 * @copyright 2016 Network RADIUS SARL <license@networkradius.com>
 */
RCSID("$Id$")

#define LOG_PREFIX "rlm_sigtran (%s) - "
#define LOG_PREFIX_ARGS inst->name

#include <osmocom/core/linuxlist.h>

#include "libosmo-m3ua/include/bsc_data.h"
#include "libosmo-m3ua/include/sctp_m3ua.h"

#include <freeradius-devel/radiusd.h>
#include <freeradius-devel/modules.h>
#include <freeradius-devel/rad_assert.h>

#include "sigtran.h"
#include <assert.h>
#include <limits.h>

#if !defined(PIPE_BUF) && defined(_POSIX_PIPE_BUF)
#  define PIPE_BUF _POSIX_PIPE_BUF
#endif

#ifdef PIPE_BUF
static_assert(sizeof(void *) < PIPE_BUF, "PIPE_BUF must be large enough to accommodate a pointer");
#endif

static uint32_t	sigtran_instances = 0;

unsigned int __hack_opc, __hack_dpc;

fr_thread_local_setup(int *, req_pipe);

static const FR_NAME_NUMBER m3ua_traffic_mode_table[] = {
	{ "override",  1 },
	{ "loadshare", 2 },
	{ "broadcast", 3 },
	{  NULL, 0 }
};

static const CONF_PARSER sctp_config[] = {
	{ FR_CONF_OFFSET("server", PW_TYPE_COMBO_IP_ADDR, rlm_sigtran_t, conn_conf.sctp_dst_ipaddr) },
	{ FR_CONF_OFFSET("port", PW_TYPE_SHORT, rlm_sigtran_t, conn_conf.sctp_dst_port), .dflt = "2905" },

	{ FR_CONF_OFFSET("src_ipaddr", PW_TYPE_COMBO_IP_ADDR, rlm_sigtran_t, conn_conf.sctp_src_ipaddr ) },
	{ FR_CONF_OFFSET("src_port", PW_TYPE_SHORT, rlm_sigtran_t, conn_conf.sctp_src_port), .dflt = "0" },

	{ FR_CONF_OFFSET("timeout", PW_TYPE_INTEGER, rlm_sigtran_t, conn_conf.sctp_timeout), .dflt = "5" },

	CONF_PARSER_TERMINATOR
};

static const CONF_PARSER m3ua_route[] = {
	{ FR_CONF_IS_SET_OFFSET("dpc", PW_TYPE_INTEGER, sigtran_m3ua_route_t, dpc) },
	{ FR_CONF_OFFSET("opc", PW_TYPE_INTEGER | PW_TYPE_MULTI, sigtran_m3ua_route_t, opc) },
	{ FR_CONF_OFFSET("si", PW_TYPE_INTEGER | PW_TYPE_MULTI, sigtran_m3ua_route_t, si) },

	CONF_PARSER_TERMINATOR
};

static const CONF_PARSER m3ua_config[] = {
	{ FR_CONF_OFFSET("link_index", PW_TYPE_SHORT, rlm_sigtran_t, conn_conf.m3ua_link_index) },
	{ FR_CONF_OFFSET("routing_ctx", PW_TYPE_SHORT, rlm_sigtran_t, conn_conf.m3ua_routing_context) },
	{ FR_CONF_OFFSET("traffic_mode", PW_TYPE_STRING, rlm_sigtran_t, conn_conf.m3ua_traffic_mode_str), .dflt = "loadshare" },
	{ FR_CONF_OFFSET("ack_timeout", PW_TYPE_INTEGER, rlm_sigtran_t, conn_conf.m3ua_ack_timeout), .dflt = "2" },
	{ FR_CONF_OFFSET("beat_interval", PW_TYPE_INTEGER, rlm_sigtran_t, conn_conf.m3ua_beat_interval), .dflt = "0" },

	{ FR_CONF_IS_SET_OFFSET("route", PW_TYPE_SUBSECTION, rlm_sigtran_t, conn_conf.m3ua_routes), .subcs = (void const *) m3ua_route },

	CONF_PARSER_TERMINATOR
};

static const CONF_PARSER mtp3_config[] = {
	{ FR_CONF_OFFSET("dpc", PW_TYPE_INTEGER | PW_TYPE_REQUIRED, rlm_sigtran_t, conn_conf.mtp3_dpc) },
	{ FR_CONF_OFFSET("opc", PW_TYPE_INTEGER | PW_TYPE_REQUIRED, rlm_sigtran_t, conn_conf.mtp3_opc) },

	CONF_PARSER_TERMINATOR
};

static const CONF_PARSER sccp_global_title[] = {
	{ FR_CONF_OFFSET("address", PW_TYPE_STRING, sigtran_sccp_global_title_t, address) },
	{ FR_CONF_IS_SET_OFFSET("tt", PW_TYPE_BYTE, sigtran_sccp_global_title_t, tt) },
	{ FR_CONF_IS_SET_OFFSET("nai", PW_TYPE_BYTE, sigtran_sccp_global_title_t, nai) },
	{ FR_CONF_IS_SET_OFFSET("np", PW_TYPE_BYTE, sigtran_sccp_global_title_t, np) },
	{ FR_CONF_IS_SET_OFFSET("es", PW_TYPE_BYTE, sigtran_sccp_global_title_t, es) },

	CONF_PARSER_TERMINATOR
};

static const CONF_PARSER sccp_address[] = {
	{ FR_CONF_IS_SET_OFFSET("pc", PW_TYPE_INTEGER, sigtran_sccp_address_t, pc) },
	{ FR_CONF_IS_SET_OFFSET("ssn", PW_TYPE_BYTE, sigtran_sccp_address_t, ssn) },
	{ FR_CONF_IS_SET_OFFSET("gt", PW_TYPE_SUBSECTION, sigtran_sccp_address_t, gt), .subcs = (void const *) sccp_global_title },

	CONF_PARSER_TERMINATOR
};

static const CONF_PARSER sccp_config[] = {
	{ FR_CONF_OFFSET("ai8", PW_TYPE_BOOLEAN, rlm_sigtran_t, conn_conf.sccp_ai8) },
	{ FR_CONF_OFFSET("route_on_ssn", PW_TYPE_BOOLEAN, rlm_sigtran_t, conn_conf.sccp_route_on_ssn) },

	{ FR_CONF_OFFSET("called", PW_TYPE_SUBSECTION, rlm_sigtran_t, conn_conf.sccp_called), .subcs = (void const *) sccp_address },
	{ FR_CONF_OFFSET("calling", PW_TYPE_SUBSECTION, rlm_sigtran_t, conn_conf.sccp_calling), .subcs = (void const *) sccp_address },

	CONF_PARSER_TERMINATOR
};

static const CONF_PARSER map_config[] = {
	{ FR_CONF_OFFSET("version", PW_TYPE_TMPL, rlm_sigtran_t, conn_conf.map_version), .dflt = "2", .quote = T_BARE_WORD},

	CONF_PARSER_TERMINATOR
};

static const CONF_PARSER module_config[] = {
	{ FR_CONF_POINTER("sctp", PW_TYPE_SUBSECTION, NULL), .subcs = (void const *) sctp_config },
	{ FR_CONF_POINTER("m3ua", PW_TYPE_SUBSECTION, NULL), .subcs = (void const *) m3ua_config },
	{ FR_CONF_POINTER("mtp3", PW_TYPE_SUBSECTION, NULL), .subcs = (void const *) mtp3_config },
	{ FR_CONF_POINTER("sccp", PW_TYPE_SUBSECTION, NULL), .subcs = (void const *) sccp_config },
	{ FR_CONF_POINTER("map", PW_TYPE_SUBSECTION, NULL), .subcs = (void const *) map_config },

	{ FR_CONF_OFFSET("imsi", PW_TYPE_TMPL | PW_TYPE_REQUIRED, rlm_sigtran_t, imsi) },

	CONF_PARSER_TERMINATOR
};

/** Signal the multiplexer that this thread is exiting
 *
 */
static void _req_pipe_unregister(void *fd_ptr)
{
	int fd = *talloc_get_type_abort(fd_ptr, int);

	sigtran_client_thread_unregister(fd);	/* Also closes our side */
}

static rlm_rcode_t CC_HINT(nonnull) mod_authorize(void *instance, UNUSED void *thread, REQUEST *request)
{
	rlm_sigtran_t const	*inst = instance;
	int			*fd_ptr, fd;

	/*
	 *	Retrieve the thread specific pipe we use
	 *	to communicate with the multiplexer.
	 */
	fd_ptr = req_pipe;
	if (!fd_ptr) {
		fd_ptr = talloc(NULL, int);
		fd = sigtran_client_thread_register();
		if (fd < 0) {
			RERROR("Failed registering thread with multiplexer");
			talloc_free(fd_ptr);
			return RLM_MODULE_FAIL;
		}
		*fd_ptr = fd;
		fr_thread_local_set_destructor(req_pipe, _req_pipe_unregister, fd_ptr);
	} else {
		fd = *fd_ptr;
	}

	return sigtran_client_map_send_auth_info(inst, request, inst->conn, fd);
}


/** Convert our sccp address config structure into sockaddr_sccp
 *
 * @param ctx to allocated address in.
 * @param inst of rlm_sigtran.
 * @param out Where to write the parsed data.
 * @param conf to parse.
 * @param cs specifying sccp address.
 * @return
 *	- 0 on success.
 *	- -1 on failure.
 */
static int sigtran_sccp_sockaddr_from_conf(TALLOC_CTX *ctx, rlm_sigtran_t *inst,
					   struct sockaddr_sccp *out,
					   sigtran_sccp_address_t *conf, CONF_SECTION *cs)
{
	/*
	 *	Fixme should be conf->gt_is_set
	 */
	if (!conf->ssn_is_set && !conf->pc_is_set && !conf->gt.address) {
		cf_log_err_cs(cs, "At least one of 'pc', 'ssn', or 'gt', must be set");
		return -1;
	}

	if (conf->ssn_is_set) out->ssn = conf->ssn;
	if (conf->pc_is_set) {
		if (conf->pc > 16777215) {
			cf_log_err_cs(cs, "Invalid value \"%d\" for 'pc', must be between 0-"
				      STRINGIFY(16777215), conf->pc);
			return -1;
		}
		out->use_poi = 1;

		memcpy(&out->poi, &conf->pc, sizeof(out->poi));
	}

	/*
	 *	Fixme should be conf->gt_is_set && conf->gt.address
	 *	But we don't have subsection presence checks yet.
	 */
	if (conf->gt_is_set || conf->gt.address) {
		int	gti_ind = SCCP_TITLE_IND_NONE;
		size_t	i;
		size_t	len = talloc_array_length(conf->gt.address) - 1;

		if (conf->gt.nai_is_set && (conf->gt.nai & 0x80)) {
			cf_log_err_cs(cs, "Global title 'nai' must be between 0-127");
			return -1;
		}

		if (conf->gt.tt_is_set) {
			if ((conf->gt.np_is_set && !conf->gt.es_is_set) ||
			    (!conf->gt.np_is_set && conf->gt.np_is_set)) {
				cf_log_err_cs(cs, "Global title 'np' and 'es' must be "
					      "specified together");
				return -1;
			}

			if (conf->gt.np) {
				cf_log_err_cs(cs, "Global title 'np' must be between 0-15");
				return -1;
			}

			if (conf->gt.es > 0x0f) {
				cf_log_err_cs(cs, "Global title 'es' must be between 0-15");
				return -1;
			}

			if (conf->gt.np_is_set) {
				gti_ind = conf->gt.nai_is_set ? SCCP_TITLE_IND_TRANS_NUM_ENC_NATURE :
								SCCP_TITLE_IND_TRANS_NUM_ENC;
			} else {
				gti_ind = SCCP_TITLE_IND_TRANSLATION_ONLY;
			}
		} else if (conf->gt.nai_is_set) {
			gti_ind = SCCP_TITLE_IND_NATURE_ONLY;
		}

		for (i = 0; i < len; i++) {
			if (!is_char_tbcd[(uint8_t)conf->gt.address[i]]) {
				cf_log_err_cs(cs, "Global title address contains invalid digit \"%c\".  "
					      "Valid digits are [0-9#*a-c]", conf->gt.address[i]);
				return -1;
			}
		}

		if (sigtran_sccp_global_title(ctx, &out->gti_data, gti_ind, conf->gt.address,
					      conf->gt.tt, conf->gt.np, conf->gt.es, conf->gt.nai) < 0) return -1;
		out->gti_len = talloc_array_length(out->gti_data);
		out->gti_ind = gti_ind;
		out->national = 1;

		/*
		 *	Print out the constructed global title blob.
		 */
		if (DEBUG_ENABLED4) {
			char *hex;

			hex = fr_abin2hex(ctx, out->gti_data, out->gti_len);
			DEBUG4("gt_ind: 0x%x", out->gti_ind);
			DEBUG4("digits: 0x%s (%i)", hex, out->gti_len);
			talloc_free(hex);
		}
	}
	return 0;
}

static int mod_instantiate(CONF_SECTION *conf, void *instance)
{
	rlm_sigtran_t *inst = instance;

	inst->name = cf_section_name2(conf);
	if (!inst->name) inst->name = cf_section_name1(conf);

	/*
	 *	Translate traffic mode string to integer
	 */
	inst->conn_conf.m3ua_traffic_mode = fr_str2int(m3ua_traffic_mode_table,
						       inst->conn_conf.m3ua_traffic_mode_str, -1);
	if (inst->conn_conf.m3ua_traffic_mode < 0) {
		cf_log_err_cs(conf, "Invalid 'm3ua_traffic_mode' value \"%s\", expected 'override', "
			      "'loadshare' or 'broadcast'", inst->conn_conf.m3ua_traffic_mode_str);
		return -1;
	}

#define MTP3_PC_CHECK(_x) \
	do { \
		if (inst->conn_conf.mtp3_##_x > 16777215) { \
			cf_log_err_cs(conf, "Invalid value \"%d\" for '#_x', must be between 0-16777215", \
				      inst->conn_conf.mtp3_##_x); \
			return -1; \
		} \
		__hack_##_x = inst->conn_conf.mtp3_##_x; \
	} while (0)

	MTP3_PC_CHECK(dpc);
	MTP3_PC_CHECK(opc);

	if (sigtran_sccp_sockaddr_from_conf(inst, inst, &inst->conn_conf.sccp_called_sockaddr,
					    &inst->conn_conf.sccp_called, conf) < 0) return -1;
	if (sigtran_sccp_sockaddr_from_conf(inst, inst, &inst->conn_conf.sccp_calling_sockaddr,
					    &inst->conn_conf.sccp_calling, conf) < 0) return -1;

	/*
	 *	If this is the first instance of rlm_sigtran
	 *	We spawn a new thread to run all the libosmo-* I/O
	 *	and events.
	 *
	 *	We talk to the thread using the ctrl_pipe, with
	 *	each thread registering its own pipe via the ctrl_pipe.
	 *
	 *	This makes it really easy to collect and distribute
	 *	requests/responses, whilst using libosmo in a
	 *	threadsafe way.
	 */
	if (sigtran_instances == 0) sigtran_event_start();
 	sigtran_instances++;

	/*
	 *	Should bring the SCTP/M3UA/MTP3/SCCP link up.
	 */
	if (sigtran_client_link_up(&inst->conn, &inst->conn_conf) < 0) return -1;

	return 0;
}

/**
 * Cleanup internal state.
 */
static int mod_detach(UNUSED void *instance)
{
	rlm_sigtran_t *inst = instance;

	sigtran_client_link_down(&inst->conn);

	if ((--sigtran_instances) == 0) sigtran_event_exit();

	return 0;
}

/*
 *	The module name should be the only globally exported symbol.
 *	That is, everything else should be 'static'.
 *
 *	If the module needs to temporarily modify it's instantiation
 *	data, the type should be changed to RLM_TYPE_THREAD_UNSAFE.
 *	The server will then take care of ensuring that the module
 *	is single-threaded.
 */
extern rad_module_t rlm_sigtran;
rad_module_t rlm_sigtran = {
	.magic		= RLM_MODULE_INIT,
	.name		= "sigtran",
	.type		= RLM_TYPE_THREAD_SAFE,
	.inst_size	= sizeof(rlm_sigtran_t),
	.config		= module_config,
	.instantiate	= mod_instantiate,
	.detach		= mod_detach,
	.methods = {
		[MOD_AUTHORIZE]		= mod_authorize,
	}
};
