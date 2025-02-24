/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
 *  Copyright (C) 2010  ST-Ericsson AB.
 *  Copyright (C) 2013 Canonical Ltd.
 *  Copyright (C) 2013 Jolla Ltd.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>

#include <glib.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/gprs.h>
#include <ofono/types.h>

#include <gril/gril.h>
#include <gril/grilutil.h>

#include "common.h"
#include "rilmodem.h"

/* Time between get data status retries */
#define GET_STATUS_TIMER_MS 5000

struct ril_gprs_data {
	GRil *ril;
	struct ofono_modem *modem;
	gboolean ofono_attached;
	int rild_status;
	int pending_deact_req;
};

/*
 * This module is the ofono_gprs_driver implementation for rilmodem.
 *
 * Notes:
 *
 * 1. ofono_gprs_suspend/resume() are not used by this module, as
 *    the concept of suspended GPRS is not exposed by RILD.
 */

static int ril_tech_to_bearer_tech(int ril_tech)
{
	/*
	 * This code handles the mapping between the RIL_RadioTechnology
	 * and packet bearer values ( see <curr_bearer> values - 27.007
	 * Section 7.29 ).
	 */

	switch (ril_tech) {
	case RADIO_TECH_GSM:
	case RADIO_TECH_UNKNOWN:
		return PACKET_BEARER_NONE;
	case RADIO_TECH_GPRS:
		return PACKET_BEARER_GPRS;
	case RADIO_TECH_EDGE:
		return PACKET_BEARER_EGPRS;
	case RADIO_TECH_UMTS:
		return PACKET_BEARER_UMTS;
	case RADIO_TECH_HSDPA:
		return PACKET_BEARER_HSDPA;
	case RADIO_TECH_HSUPA:
		return PACKET_BEARER_HSUPA;
	case RADIO_TECH_HSPAP:
	case RADIO_TECH_HSPA:
		/*
		 * HSPAP is HSPA+; which ofono doesn't define;
		 * so, if differentiating HSPA and HSPA+ is
		 * important, then ofono needs to be patched,
		 * and we probably also need to introduce a
		 * new indicator icon.
		 */
		return PACKET_BEARER_HSUPA_HSDPA;
	case RADIO_TECH_LTE:
		return PACKET_BEARER_EPS;
	default:
		return PACKET_BEARER_NONE;
	}
}

static int ril_get_apn_profile_id(enum ofono_gprs_context_type type) {
	if (type == OFONO_GPRS_CONTEXT_TYPE_INTERNET) {
		return RIL_DATA_PROFILE_DEFAULT;
	} else if (type == OFONO_GPRS_CONTEXT_TYPE_IMS) {
		return RIL_DATA_PROFILE_IMS;
	} else {
		return RIL_DATA_PROFILE_DEFAULT;
	}
}

static void ril_gprs_set_attached(struct ofono_gprs *gprs, int attached,
					ofono_gprs_cb_t cb, void *data)
{
	struct ril_gprs_data *gd = ofono_gprs_get_data(gprs);

	DBG("attached: %d", attached);

	/*
	 * As RIL offers no actual control over the GPRS 'attached'
	 * state, we save the desired state, and use it to override
	 * the actual modem's state in the 'attached_status' function.
	 * This is similar to the way the core ofono gprs code handles
	 * data roaming ( see src/gprs.c gprs_netreg_update().
	 *
	 * The core gprs code calls driver->set_attached() when a netreg
	 * notificaiton is received and any configured roaming conditions
	 * are met.
	 */
	gd->ofono_attached = attached;
	CALLBACK_WITH_SUCCESS(cb, data);
}

static void ril_data_reg_cb(struct ril_msg *message, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_gprs_status_cb_t cb = cbd->cb;
	struct ofono_gprs *gprs = cbd->user;
	struct ril_gprs_data *gd = ofono_gprs_get_data(gprs);
	struct ofono_modem *modem;
	struct parcel rilp;
	int num_str;
	char **strv;
	char *debug_str;
	char *end;
	int status;
	int tech = -1;
	gboolean notify_status = FALSE;

	if (message->error != RIL_E_SUCCESS) {
		ofono_error("%s: DATA_REGISTRATION_STATE reply failure: %s",
				__func__,
				ril_error_to_string(message->error));
		gd->rild_status = -1;
		goto error;
	}

	g_ril_init_parcel(message, &rilp);
	strv = parcel_r_strv(&rilp);
	num_str = g_strv_length(strv);

	if (strv == NULL)
		goto error;

	debug_str = g_strjoinv(",", strv);
	g_ril_append_print_buf(gd->ril, "{%d,%s}", num_str, debug_str);
	g_free(debug_str);
	g_ril_print_response(gd->ril, message);

	status = strtoul(strv[0], &end, 10);
	if (end == strv[0] || *end != '\0')
		goto error_free;

	status = ril_util_registration_state_to_status(status);
	if (status < 0)
		goto error_free;

	if (num_str >= 4) {
		tech = strtoul(strv[3], &end, 10);
		if (end == strv[3] || *end != '\0')
			tech = -1;

		ofono_debug("DATA_REGISTRATION_STATE with raw tech %d", tech);
		if (g_ril_vendor(gd->ril) == OFONO_RIL_VENDOR_MTK) {
			switch (tech) {
			case MTK_RADIO_TECH_HSDPAP:
			case MTK_RADIO_TECH_HSDPAP_UPA:
			case MTK_RADIO_TECH_HSUPAP:
			case MTK_RADIO_TECH_HSUPAP_DPA:
				tech = RADIO_TECH_HSPAP;
				break;
			case MTK_RADIO_TECH_DC_DPA:
				tech = RADIO_TECH_HSDPA;
				break;
			case MTK_RADIO_TECH_DC_UPA:
				tech = RADIO_TECH_HSUPA;
				break;
			case MTK_RADIO_TECH_DC_HSDPAP:
			case MTK_RADIO_TECH_DC_HSDPAP_UPA:
			case MTK_RADIO_TECH_DC_HSDPAP_DPA:
			case MTK_RADIO_TECH_DC_HSPAP:
				tech = RADIO_TECH_HSPAP;
				break;
			}
		}
	}

	if (gd->rild_status != status) {
		ofono_debug("%s - old status : %d, new status : %d",
					__func__, gd->rild_status, status);
		gd->rild_status = status;

		notify_status = TRUE;
	}

	if (notify_status && cb == NULL)
		ofono_gprs_status_notify(gprs, status);

	modem = ofono_gprs_get_modem(gprs);
	ofono_debug("DATA_REGISTRATION_STATE with tech %d", tech);
	ofono_modem_set_integer(modem, "RilDataRadioTechnology", tech);
	ofono_gprs_bearer_notify(gprs, ril_tech_to_bearer_tech(tech));
	ofono_gprs_tech_notify(gprs, tech);

	if (cb)
		CALLBACK_WITH_SUCCESS(cb, status, cbd->data);

	g_strfreev(strv);
	return;

error_free:
	g_strfreev(strv);

error:
	if (cb)
		CALLBACK_WITH_FAILURE(cb, -1, cbd->data);
}

static void ril_gprs_registration_status(struct ofono_gprs *gprs,
					ofono_gprs_status_cb_t cb, void *data)
{
	struct ril_gprs_data *gd = ofono_gprs_get_data(gprs);
	struct cb_data *cbd = cb_data_new(cb, data, gprs);

	DBG("");

	if (g_ril_send(gd->ril, RIL_REQUEST_DATA_REGISTRATION_STATE, NULL,
			ril_data_reg_cb, cbd, g_free) == 0) {
		ofono_error("%s: send "
				"RIL_REQUEST_DATA_REGISTRATION_STATE failed",
				__func__);
		g_free(cbd);

		CALLBACK_WITH_FAILURE(cb, -1, data);
	}
}

static void ril_set_data_allow_cb(struct ril_msg *message, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	struct ofono_gprs *gprs = cbd->user;
	struct ril_gprs_data *gd = ofono_gprs_get_data(gprs);
	ofono_gprs_status_cb_t cb = cbd->cb;

	if (message->error != RIL_E_SUCCESS) {
		ofono_error("%s: RIL_REQUEST_ALLOW_DATA reply failure: %s",
				__func__,
				ril_error_to_string(message->error));

		CALLBACK_WITH_FAILURE(cb, -1, cbd->data);
		return;
	}

	g_ril_print_response_no_args(gd->ril, message);

	CALLBACK_WITH_SUCCESS(cb, 0, cbd->data);
}

static void ril_gprs_set_data_allow(struct ofono_gprs *gprs, ofono_bool_t allow,
		ofono_gprs_status_cb_t cb, void *data)
{
	struct ril_gprs_data *gd = ofono_gprs_get_data(gprs);
	struct cb_data *cbd = cb_data_new(cb, data, gprs);
	struct parcel rilp;

	parcel_init(&rilp);
	parcel_w_int32(&rilp, allow);

	if (g_ril_send(gd->ril, RIL_REQUEST_ALLOW_DATA, &rilp,
			ril_set_data_allow_cb, cbd, g_free) == 0) {
		ofono_error("%s: send "
				"RIL_REQUEST_ALLOW_DATA failed",
				__func__);
		g_free(cbd);

		CALLBACK_WITH_FAILURE(cb, -1, data);
	}
}

static void ril_set_data_profile_cb(struct ril_msg *message, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	struct ofono_gprs *gprs = cbd->user;
	struct ril_gprs_data *gd = ofono_gprs_get_data(gprs);
	ofono_gprs_status_cb_t cb = cbd->cb;

	if (message->error != RIL_E_SUCCESS) {
		ofono_error("%s: RIL_REQUEST_SET_DATA_PROFILE reply failure: %s",
				__func__,
				ril_error_to_string(message->error));

		CALLBACK_WITH_FAILURE(cb, -1, cbd->data);
		return;
	}

	g_ril_print_response_no_args(gd->ril, message);

	CALLBACK_WITH_SUCCESS(cb, 0, cbd->data);
}

static void ril_gprs_set_data_profile(struct ofono_gprs *gprs,
		void *param, int length, ofono_gprs_status_cb_t cb, void *data)
{
	struct ril_gprs_data *gd = ofono_gprs_get_data(gprs);
	struct cb_data *cbd = cb_data_new(cb, data, gprs);
	struct ofono_gprs_primary_context *contexts = param;
	struct ofono_gprs_primary_context pri_ctx;
	struct parcel rilp;
	int i;

	if (contexts == NULL) {
		g_free(cbd);
		return;
	}

	parcel_init(&rilp);
	parcel_w_int32(&rilp, length);

	for (i = 0; i < length; i++) {
		pri_ctx = *(contexts + i);

		parcel_w_int32(&rilp, ril_get_apn_profile_id(pri_ctx.type)); /* profile id */
		parcel_w_string(&rilp, pri_ctx.apn);
		parcel_w_string(&rilp, gprs_proto_to_string(pri_ctx.proto));
		parcel_w_int32(&rilp, pri_ctx.auth_method);
		parcel_w_string(&rilp, pri_ctx.username);
		parcel_w_string(&rilp, pri_ctx.password);
		parcel_w_int32(&rilp, 0); /* the profile type, 0 - COMMON, 1 - 3GPP, 2 - 3GPP2 */
		parcel_w_int32(&rilp, 0); /* the period in seconds to limit the maximum connections */
		parcel_w_int32(&rilp, 0); /* the maximum connections during maxConnsTime */
		parcel_w_int32(&rilp, 0); /* wait time */
		parcel_w_int32(&rilp, 1); /* true to enable the profile, false to disable */
	}

	if (g_ril_send(gd->ril, RIL_REQUEST_SET_DATA_PROFILE, &rilp,
			ril_set_data_profile_cb, cbd, g_free) == 0) {
		ofono_error("%s: send "
				"RIL_REQUEST_SET_DATA_PROFILE failed",
				__func__);
		g_free(cbd);

		CALLBACK_WITH_FAILURE(cb, -1, data);
	}
}

static void query_max_cids_cb(struct ril_msg *message, gpointer user_data)
{
	struct ofono_gprs *gprs = user_data;
	struct ril_gprs_data *gd = ofono_gprs_get_data(gprs);
	struct parcel rilp;
	int num_str;
	char **strv;
	char *debug_str;
	char *end;
	int max_calls = 2;

	if (message->error != RIL_E_SUCCESS) {
		ofono_error("%s: DATA_REGISTRATION_STATE reply failure: %s",
				__func__,
				ril_error_to_string(message->error));
		goto error;
	}

	g_ril_init_parcel(message, &rilp);
	strv = parcel_r_strv(&rilp);

	if (strv == NULL)
		goto error;

	num_str = g_strv_length(strv);
	debug_str = g_strjoinv(",", strv);
	g_ril_append_print_buf(gd->ril, "{%d,%s}", num_str, debug_str);
	g_free(debug_str);
	g_ril_print_response(gd->ril, message);

	if (num_str < 6)
		goto reg_atom;

	max_calls = strtoul(strv[5], &end, 10);
	if (end == strv[5] || *end != '\0')
		goto error_free;

reg_atom:
	g_strfreev(strv);
	ofono_gprs_set_cid_range(gprs, 1, max_calls);
	return;

error_free:
	g_strfreev(strv);

error:
	ofono_error("Unable to query max CIDs");
}

static void ril_gprs_state_change(struct ril_msg *message, gpointer user_data)
{
	struct ofono_gprs *gprs = user_data;
	struct ril_gprs_data *gd = ofono_gprs_get_data(gprs);

	g_ril_print_unsol_no_args(gd->ril, message);

	ril_gprs_registration_status(gprs, NULL, NULL);
}

static void query_max_cids(struct ofono_gprs *gprs)
{
	struct ril_gprs_data *gd = ofono_gprs_get_data(gprs);

	/*
	 * MTK modem does not return max_cids, string, so hard-code it
	 * here
	 */
	if (g_ril_vendor(gd->ril) == OFONO_RIL_VENDOR_MTK) {
		ofono_gprs_set_cid_range(gprs, 1, 3);
		return;
	}

	if (g_ril_send(gd->ril, RIL_REQUEST_DATA_REGISTRATION_STATE, NULL,
			query_max_cids_cb, gprs, NULL) < 0)
		ofono_error("error in %s", __func__);
}

static void drop_data_call_cb(struct ril_msg *message, gpointer user_data)
{
	struct ofono_gprs *gprs = user_data;
	struct ril_gprs_data *gd = ofono_gprs_get_data(gprs);

	if (message->error == RIL_E_SUCCESS)
		g_ril_print_response_no_args(gd->ril, message);
	else
		ofono_error("%s: RIL error %s", __func__,
				ril_error_to_string(message->error));

	if (--(gd->pending_deact_req) == 0)
		query_max_cids(gprs);
}

static int drop_data_call(struct ofono_gprs *gprs, int cid)
{
	struct ril_gprs_data *gd = ofono_gprs_get_data(gprs);
	struct parcel rilp;

	ril_util_build_deactivate_data_call(gd->ril, &rilp, cid,
					RIL_DEACTIVATE_DATA_CALL_NO_REASON);

	if (g_ril_send(gd->ril, RIL_REQUEST_DEACTIVATE_DATA_CALL,
			&rilp, drop_data_call_cb, gprs, NULL) > 0)
		return 0;

	return -1;
}

static void get_active_data_calls_cb(struct ril_msg *message,
					gpointer user_data)
{
	struct ofono_gprs *gprs = user_data;
	struct ril_gprs_data *gd = ofono_gprs_get_data(gprs);
	struct parcel rilp;
	int num_calls;
	int cid;
	int i;

	if (message->error != RIL_E_SUCCESS) {
		ofono_error("%s: RIL error %s", __func__,
				ril_error_to_string(message->error));
		goto end;
	}

	g_ril_print_response_no_args(gd->ril, message);

	g_ril_init_parcel(message, &rilp);

	/* Version */
	parcel_r_int32(&rilp);
	num_calls = parcel_r_int32(&rilp);

	/*
	 * We disconnect from previous calls here, which might be needed
	 * because of a previous ofono abort, as some rild implementations do
	 * not disconnect the calls even after the ril socket is closed.
	 */
	for (i = 0; i < num_calls; i++) {
		parcel_r_int32(&rilp);			/* status */
		parcel_r_int32(&rilp);			/* ignore */
		cid = parcel_r_int32(&rilp);
		parcel_r_int32(&rilp);			/* active */
		parcel_skip_string(&rilp);		/* type */
		parcel_skip_string(&rilp);		/* ifname */
		parcel_skip_string(&rilp);		/* addresses */
		parcel_skip_string(&rilp);		/* dns */
		parcel_skip_string(&rilp);		/* gateways */
		parcel_skip_string(&rilp);		/* pcscf */
		parcel_r_int32(&rilp);			/* mtu */

		/* malformed check */
		if (rilp.malformed) {
			ofono_error("%s: malformed parcel received", __func__);
			goto end;
		}

		ofono_debug("Standing data call with cid %d", cid);

		if (drop_data_call(gprs, cid) == 0)
			++(gd->pending_deact_req);
	}

end:
	if (gd->pending_deact_req == 0)
		query_max_cids(gprs);
}

static void get_active_data_calls(struct ofono_gprs *gprs)
{
	struct ril_gprs_data *gd = ofono_gprs_get_data(gprs);

	if (g_ril_send(gd->ril, RIL_REQUEST_DATA_CALL_LIST, NULL,
			get_active_data_calls_cb, gprs, NULL) == 0)
		ofono_error("%s: send failed", __func__);
}

static void ril_gprs_restricted_state_change(struct ril_msg *message, gpointer user_data)
{
	struct ofono_gprs *gprs = user_data;
	struct ril_gprs_data *gd = ofono_gprs_get_data(gprs);
	struct parcel rilp;
	int resticted_state;

	g_ril_print_unsol_no_args(gd->ril, message);

	g_ril_init_parcel(message, &rilp);

	/* indication_type. */
	parcel_r_int32(&rilp);
	resticted_state = parcel_r_int32(&rilp);

	ofono_gprs_restricted_notify(gprs, resticted_state);
}

static gboolean ril_delayed_register(gpointer user_data)
{
	struct ofono_gprs *gprs = user_data;
	struct ril_gprs_data *gd = ofono_gprs_get_data(gprs);

	ofono_gprs_register(gprs);

	get_active_data_calls(gprs);

	g_ril_register(gd->ril, RIL_UNSOL_RESPONSE_NETWORK_STATE_CHANGED,
					ril_gprs_state_change, gprs);

	g_ril_register(gd->ril, RIL_UNSOL_RESTRICTED_STATE_CHANGED,
			ril_gprs_restricted_state_change, gprs);

	return FALSE;
}

static int ril_gprs_probe(struct ofono_gprs *gprs, unsigned int vendor,
								void *userdata)
{
	GRil *ril = userdata;
	struct ril_gprs_data *gd;

	gd = g_try_new0(struct ril_gprs_data, 1);
	if (gd == NULL)
		return -ENOMEM;

	gd->ril = g_ril_clone(ril);
	gd->ofono_attached = FALSE;
	gd->rild_status = -1;

	ofono_gprs_set_data(gprs, gd);

	/*
	 * set default cid range and it should be consitent with
	 * gprs context count in ril.c/ril_post_online().
	 */
	ofono_gprs_set_cid_range(gprs, 1, 6);

	g_idle_add(ril_delayed_register, gprs);

	return 0;
}

static void ril_gprs_remove(struct ofono_gprs *gprs)
{
	struct ril_gprs_data *gd = ofono_gprs_get_data(gprs);

	DBG("");

	ofono_gprs_set_data(gprs, NULL);

	g_ril_unref(gd->ril);
	g_free(gd);
}

static const struct ofono_gprs_driver driver = {
	.name			= RILMODEM,
	.probe			= ril_gprs_probe,
	.remove			= ril_gprs_remove,
	.set_attached		= ril_gprs_set_attached,
	.attached_status	= ril_gprs_registration_status,
	.set_data_allow		= ril_gprs_set_data_allow,
	.set_data_profile	= ril_gprs_set_data_profile,
};

void ril_gprs_init(void)
{
	ofono_gprs_driver_register(&driver);
}

void ril_gprs_exit(void)
{
	ofono_gprs_driver_unregister(&driver);
}
