/**
 * @file not.c  SIP Event Notify
 *
 * Copyright (C) 2010 Creytiv.com
 */
#include <string.h> // todo: remove
#include <re_types.h>
#include <re_mem.h>
#include <re_mbuf.h>
#include <re_sa.h>
#include <re_list.h>
#include <re_hash.h>
#include <re_fmt.h>
#include <re_uri.h>
#include <re_sys.h>
#include <re_tmr.h>
#include <re_sip.h>
#include <re_sipevent.h>
#include "sipevent.h"


static int notify_request(struct sipnot *not, bool reset_ls);


static void internal_close_handler(int err, const struct sip_msg *msg,
				   void *arg)
{
	(void)err;
	(void)msg;
	(void)arg;
}


static bool terminate(struct sipnot *not, enum sipevent_reason reason)
{
	not->terminated = true;
	not->reason     = reason;
	not->closeh     = internal_close_handler;

	if (not->req) {
		mem_ref(not);
		return true;
	}

	if (not->subscribed && !notify_request(not, true)) {
		mem_ref(not);
		return true;
	}

	return false;
}


static void destructor(void *arg)
{
	struct sipnot *not = arg;

	tmr_cancel(&not->tmr);

	if (!not->terminated) {

		if (terminate(not, SIPEVENT_NORESOURCE))
			return;
	}

	hash_unlink(&not->he);
	mem_deref(not->req);
	mem_deref(not->dlg);
	mem_deref(not->auth);
	mem_deref(not->mb);
	mem_deref(not->event);
	mem_deref(not->id);
	mem_deref(not->cuser);
	mem_deref(not->hdrs);
	mem_deref(not->ctype);
	mem_deref(not->sock);
	mem_deref(not->sip);
}


static void sipnot_terminate(struct sipnot *not, int err,
			     const struct sip_msg *msg,
			     enum sipevent_reason reason)
{
	sipevent_close_h *closeh;
	void *arg;

	closeh = not->closeh;
	arg    = not->arg;

	tmr_cancel(&not->tmr);
	(void)terminate(not, reason);

	closeh(err, msg, arg);
}


static void tmr_handler(void *arg)
{
	struct sipnot *not = arg;

	if (not->terminated)
		return;

	re_printf("subscription expired\n");

	sipnot_terminate(not, ETIMEDOUT, NULL, SIPEVENT_TIMEOUT);
}


void sipnot_refresh(struct sipnot *not, uint32_t expires)
{
	expires = min(expires, not->expires_max);

	re_printf("will expire in %u secs\n", expires);

	tmr_start(&not->tmr, expires * 1000, tmr_handler, not);
}


static void response_handler(int err, const struct sip_msg *msg, void *arg)
{
	struct sipnot *not = arg;

	if (err)
		re_printf("notify reply: %s\n", strerror(err));
	else
		re_printf("notify reply: %u %r\n", msg->scode, &msg->reason);

	if (err) {
		if (err == ETIMEDOUT)
			not->subscribed = false;
		goto out;
	}

	if (sip_request_loops(&not->ls, msg->scode)) {
		not->subscribed = false;
		goto out;
	}

	if (msg->scode < 200) {
		return;
	}
	else if (msg->scode < 300) {

		(void)sip_dialog_update(not->dlg, msg);
	}
	else {
		switch (msg->scode) {

		case 401:
		case 407:
			err = sip_auth_authenticate(not->auth, msg);
			if (err) {
				err = (err == EAUTH) ? 0 : err;
				break;
			}

			err = notify_request(not, false);
			if (err)
				break;

			return;
		}

		not->subscribed = false;
	}

 out:
	if (not->termsent) {
		mem_deref(not);
	}
	else if (not->terminated) {
		if (!not->subscribed || notify_request(not, true))
			mem_deref(not);
	}
	else if (!not->subscribed) {
		sipnot_terminate(not, err, msg, -1);
	}
	else if (not->notify_pending) {
		re_printf("sending queued request\n");
		(void)notify_request(not, true);
	}
}


static int send_handler(enum sip_transp tp, const struct sa *src,
			const struct sa *dst, struct mbuf *mb, void *arg)
{
	struct sipnot *not = arg;
	(void)dst;

	return mbuf_printf(mb, "Contact: <sip:%s@%J%s>\r\n",
                           not->cuser, src, sip_transp_param(tp));
}


static int print_event(struct re_printf *pf, const struct sipnot *not)
{
	if (not->id)
		return re_hprintf(pf, "%s;id=%s", not->event, not->id);
	else
		return re_hprintf(pf, "%s", not->event);
}


static int print_substate(struct re_printf *pf, const struct sipnot *not)
{
	if (not->terminated) {
		return re_hprintf(pf, "terminated;reason=%s",
				  sipevent_reason_name(not->reason));
	}
	else {
		uint32_t expires;

		expires = (uint32_t)(tmr_get_expire(&not->tmr) / 1000);

		return re_hprintf(pf, "%s;expires=%u",
				  not->mb ? "active" : "pending",
				  expires);
	}
}


static int print_content(struct re_printf *pf, const struct sipnot *not)
{
	if (!not->mb)
		return re_hprintf(pf,
				  "Content-Length: 0\r\n"
				  "\r\n");
	else
		return re_hprintf(pf,
				  "Content-Type: %s\r\n"
				  "Content-Length: %zu\r\n"
				  "\r\n"
				  "%b",
				  not->ctype,
				  mbuf_get_left(not->mb),
				  mbuf_buf(not->mb),
				  mbuf_get_left(not->mb));
}


static int notify_request(struct sipnot *not, bool reset_ls)
{
	if (reset_ls)
		sip_loopstate_reset(&not->ls);

	if (not->terminated)
		not->termsent = true;

	not->notify_pending = false;

	return sip_drequestf(&not->req, not->sip, true, "NOTIFY",
			     not->dlg, 0, not->auth,
			     send_handler, response_handler, not,
			     "Event: %H\r\n"
			     "Subscription-State: %H\r\n"
			     "%s"
			     "%H",
			     print_event, not,
			     print_substate, not,
			     not->hdrs,
			     print_content, not);
}


int sipnot_notify(struct sipnot *not)
{
	if (not->req) {
		re_printf("waiting for previous request to complete\n");
		not->notify_pending = true;
		return 0;
	}

	return notify_request(not, true);
}


int sipnot_reply(struct sipnot *not, const struct sip_msg *msg,
		 uint16_t scode, const char *reason)
{
	uint32_t expires;

	expires = (uint32_t)(tmr_get_expire(&not->tmr) / 1000);

	return sip_treplyf(NULL, NULL, not->sip, msg, true, scode, reason,
			   "Contact: <sip:%s@%J%s>\r\n"
			   "Expires: %u\r\n"
			   "Content-Length: 0\r\n"
			   "\r\n",
			   not->cuser, &msg->dst, sip_transp_param(msg->tp),
			   expires);
}


int sipevent_accept(struct sipnot **notp, struct sipevent_sock *sock,
		    const struct sip_msg *msg, struct sip_dialog *dlg,
		    const struct sipevent_event *event,
		    uint16_t scode, const char *reason, uint32_t expires_min,
		    uint32_t expires_dfl, uint32_t expires_max,
		    const char *cuser, const char *ctype,
		    sip_auth_h *authh, void *aarg, bool aref,
		    sipevent_close_h *closeh, void *arg, const char *fmt, ...)
{
	struct sipnot *not;
	uint32_t expires;
	int err;

	if (!notp || !sock || !msg || !scode || !reason || !expires_dfl ||
	    !expires_max || !cuser || !ctype || expires_dfl < expires_min)
		return EINVAL;

	not = mem_zalloc(sizeof(*not), destructor);
	if (!not)
		return ENOMEM;

	if (!pl_strcmp(&msg->met, "REFER")) {

		err = str_dup(&not->event, "refer");
		if (err)
			goto out;

		err = re_sdprintf(&not->id, "%u", msg->cseq.num);
		if (err)
			goto out;
	}
	else {
		if (!event) {
			err = EINVAL;
			goto out;
		}

		err = pl_strdup(&not->event, &event->event);
		if (err)
			goto out;

		if (pl_isset(&event->id)) {

			err = pl_strdup(&not->id, &event->id);
			if (err)
				goto out;
		}
	}

	if (dlg) {
		not->dlg = mem_ref(dlg);
	}
	else {
		err = sip_dialog_accept(&not->dlg, msg);
		if (err)
			goto out;
	}

	hash_append(sock->ht_not,
		    hash_joaat_str(sip_dialog_callid(not->dlg)),
		    &not->he, not);

	err = sip_auth_alloc(&not->auth, authh, aarg, aref);
	if (err)
		goto out;

	err = str_dup(&not->cuser, cuser);
	if (err)
		goto out;

	err = str_dup(&not->ctype, ctype);
	if (err)
		goto out;

	if (fmt) {
		va_list ap;

		va_start(ap, fmt);
		err = re_vsdprintf(&not->hdrs, fmt, ap);
		va_end(ap);
		if (err)
			goto out;
	}

	not->expires_min = expires_min;
	not->expires_dfl = expires_dfl;
	not->expires_max = expires_max;
	not->sock   = mem_ref(sock);
	not->sip    = mem_ref(sock->sip);
	not->closeh = closeh ? closeh : internal_close_handler;
	not->arg    = arg;

	if (pl_isset(&msg->expires))
		expires = pl_u32(&msg->expires);
	else
		expires = not->expires_dfl;

	sipnot_refresh(not, expires);

	err = sipnot_reply(not, msg, scode, reason);
	if (err)
		goto out;

	not->subscribed = true;

 out:
	if (err)
		mem_deref(not);
	else
		*notp = not;

	return err;
}


int sipevent_notify(struct sipnot *not, struct mbuf *mb, bool term,
		    enum sipevent_reason reason)
{
	if (!not || not->terminated)
		return EINVAL;

	if (mb || !term) {
		mem_deref(not->mb);
		not->mb = mem_ref(mb);
	}

	if (term) {
		tmr_cancel(&not->tmr);
		(void)terminate(not, reason);
		return 0;
	}

	return sipnot_notify(not);
}


int sipevent_notifyf(struct sipnot *not, struct mbuf **mbp, bool term,
		     enum sipevent_reason reason, const char *fmt, ...)
{
	struct mbuf *mb;
	va_list ap;
	int err;

	if (!not || not->terminated || !fmt)
		return EINVAL;

	if (mbp && *mbp)
		return sipevent_notify(not, *mbp, term, reason);

	mb = mbuf_alloc(1024);
	if (!mb)
		return ENOMEM;

	va_start(ap, fmt);
	err = mbuf_vprintf(mb, fmt, ap);
	va_end(ap);
	if (err)
		goto out;

	mb->pos = 0;

	err = sipevent_notify(not, mb, term, reason);
	if (err)
		goto out;

 out:
	if (err || !mbp)
		mem_deref(mb);
	else
		*mbp = mb;

	return err;
}
