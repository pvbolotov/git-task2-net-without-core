/*
 *  linux/net/sunrpc/gss_krb5_seal.c
 *
 *  Adapted from MIT Kerberos 5-1.2.1 lib/gssapi/krb5/k5seal.c
 *
 *  Copyright (c) 2000-2008 The Regents of the University of Michigan.
 *  All rights reserved.
 *
 *  Andy Adamson	<andros@umich.edu>
 *  J. Bruce Fields	<bfields@umich.edu>
 */

/*
 * Copyright 1993 by OpenVision Technologies, Inc.
 *
 * Permission to use, copy, modify, distribute, and sell this software
 * and its documentation for any purpose is hereby granted without fee,
 * provided that the above copyright notice appears in all copies and
 * that both that copyright notice and this permission notice appear in
 * supporting documentation, and that the name of OpenVision not be used
 * in advertising or publicity pertaining to distribution of the software
 * without specific, written prior permission. OpenVision makes no
 * representations about the suitability of this software for any
 * purpose.  It is provided "as is" without express or implied warranty.
 *
 * OPENVISION DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO
 * EVENT SHALL OPENVISION BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF
 * USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR
 * OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

/*
 * Copyright (C) 1998 by the FundsXpress, INC.
 *
 * All rights reserved.
 *
 * Export of this software from the United States of America may require
 * a specific license from the United States Government.  It is the
 * responsibility of any person or organization contemplating export to
 * obtain such a license before exporting.
 *
 * WITHIN THAT CONSTRAINT, permission to use, copy, modify, and
 * distribute this software and its documentation for any purpose and
 * without fee is hereby granted, provided that the above copyright
 * notice appear in all copies and that both that copyright notice and
 * this permission notice appear in supporting documentation, and that
 * the name of FundsXpress. not be used in advertising or publicity pertaining
 * to distribution of the software without specific, written prior
 * permission.  FundsXpress makes no representations about the suitability of
 * this software for any purpose.  It is provided "as is" without express
 * or implied warranty.
 *
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND WITHOUT ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, WITHOUT LIMITATION, THE IMPLIED
 * WARRANTIES OF MERCHANTIBILITY AND FITNESS FOR A PARTICULAR PURPOSE.
 */

#include <linux/types.h>
#include <linux/jiffies.h>
#include <linux/sunrpc/gss_krb5.h>
#include <linux/random.h>
#include <linux/crypto.h>

#ifdef RPC_DEBUG
# define RPCDBG_FACILITY        RPCDBG_AUTH
#endif

DEFINE_SPINLOCK(krb5_seq_lock);

static char *
setup_token(struct krb5_ctx *ctx, struct xdr_netobj *token)
{
	__be16 *ptr, *krb5_hdr;
	int body_size = GSS_KRB5_TOK_HDR_LEN + ctx->gk5e->cksumlength;

	token->len = g_token_size(&ctx->mech_used, body_size);

	ptr = (__be16 *)token->data;
	g_make_token_header(&ctx->mech_used, body_size, (unsigned char **)&ptr);

	/* ptr now at start of header described in rfc 1964, section 1.2.1: */
	krb5_hdr = ptr;
	*ptr++ = KG_TOK_MIC_MSG;
	*ptr++ = cpu_to_le16(ctx->gk5e->signalg);
	*ptr++ = SEAL_ALG_NONE;
	*ptr++ = 0xffff;

	return (char *)krb5_hdr;
}

static u32
gss_get_mic_v1(struct krb5_ctx *ctx, struct xdr_buf *text,
		struct xdr_netobj *token)
{
	char			cksumdata[GSS_KRB5_MAX_CKSUM_LEN];
	struct xdr_netobj	md5cksum = {.len = sizeof(cksumdata),
					    .data = cksumdata};
	void			*ptr;
	s32			now;
	u32			seq_send;
	u8			*cksumkey;

	dprintk("RPC:       %s\n", __func__);
	BUG_ON(ctx == NULL);

	now = get_seconds();

	ptr = setup_token(ctx, token);

	if (ctx->gk5e->keyed_cksum)
		cksumkey = ctx->cksum;
	else
		cksumkey = NULL;

	if (make_checksum(ctx, ptr, 8, text, 0, cksumkey, &md5cksum))
		return GSS_S_FAILURE;

	memcpy(ptr + GSS_KRB5_TOK_HDR_LEN, md5cksum.data, md5cksum.len);

	spin_lock(&krb5_seq_lock);
	seq_send = ctx->seq_send++;
	spin_unlock(&krb5_seq_lock);

	if (krb5_make_seq_num(ctx->seq, ctx->initiate ? 0 : 0xff,
			      seq_send, ptr + GSS_KRB5_TOK_HDR_LEN,
			      ptr + 8))
		return GSS_S_FAILURE;

	return (ctx->endtime < now) ? GSS_S_CONTEXT_EXPIRED : GSS_S_COMPLETE;
}

u32
gss_get_mic_kerberos(struct gss_ctx *gss_ctx, struct xdr_buf *text,
		     struct xdr_netobj *token)
{
	struct krb5_ctx		*ctx = gss_ctx->internal_ctx_id;

	switch (ctx->enctype) {
	default:
		BUG();
	case ENCTYPE_DES_CBC_RAW:
	case ENCTYPE_DES3_CBC_RAW:
		return gss_get_mic_v1(ctx, text, token);
	}
}

