/*
 * Copyright (C) ST-Ericsson AB 2010
 * Author:	Sjur Brendeland/sjur.brandeland@stericsson.com
 * License terms: GNU General Public License (GPL) version 2
 */

#include <linux/stddef.h>
#include <linux/spinlock.h>
#include <linux/slab.h>
#include <net/caif/caif_layer.h>
#include <net/caif/cfpkt.h>
#include <net/caif/cfctrl.h>

#define container_obj(layr) container_of(layr, struct cfctrl, serv.layer)
#define UTILITY_NAME_LENGTH 16
#define CFPKT_CTRL_PKT_LEN 20


#ifdef CAIF_NO_LOOP
static int handle_loop(struct cfctrl *ctrl,
			      int cmd, struct cfpkt *pkt){
	return CAIF_FAILURE;
}
#else
static int handle_loop(struct cfctrl *ctrl,
		int cmd, struct cfpkt *pkt);
#endif
static int cfctrl_recv(struct cflayer *layr, struct cfpkt *pkt);
static void cfctrl_ctrlcmd(struct cflayer *layr, enum caif_ctrlcmd ctrl,
			   int phyid);


struct cflayer *cfctrl_create(void)
{
	struct cfctrl *this =
		kmalloc(sizeof(struct cfctrl), GFP_ATOMIC);
	if (!this) {
		pr_warning("CAIF: %s(): Out of memory\n", __func__);
		return NULL;
	}
	caif_assert(offsetof(struct cfctrl, serv.layer) == 0);
	memset(this, 0, sizeof(*this));
	spin_lock_init(&this->info_list_lock);
	atomic_set(&this->req_seq_no, 1);
	atomic_set(&this->rsp_seq_no, 1);
	this->serv.dev_info.id = 0xff;
	this->serv.layer.id = 0;
	this->serv.layer.receive = cfctrl_recv;
	sprintf(this->serv.layer.name, "ctrl");
	this->serv.layer.ctrlcmd = cfctrl_ctrlcmd;
	spin_lock_init(&this->loop_linkid_lock);
	this->loop_linkid = 1;
	return &this->serv.layer;
}

static bool param_eq(struct cfctrl_link_param *p1, struct cfctrl_link_param *p2)
{
	bool eq =
	    p1->linktype == p2->linktype &&
	    p1->priority == p2->priority &&
	    p1->phyid == p2->phyid &&
	    p1->endpoint == p2->endpoint && p1->chtype == p2->chtype;

	if (!eq)
		return false;

	switch (p1->linktype) {
	case CFCTRL_SRV_VEI:
		return true;
	case CFCTRL_SRV_DATAGRAM:
		return p1->u.datagram.connid == p2->u.datagram.connid;
	case CFCTRL_SRV_RFM:
		return
		    p1->u.rfm.connid == p2->u.rfm.connid &&
		    strcmp(p1->u.rfm.volume, p2->u.rfm.volume) == 0;
	case CFCTRL_SRV_UTIL:
		return
		    p1->u.utility.fifosize_kb == p2->u.utility.fifosize_kb
		    && p1->u.utility.fifosize_bufs ==
		    p2->u.utility.fifosize_bufs
		    && strcmp(p1->u.utility.name, p2->u.utility.name) == 0
		    && p1->u.utility.paramlen == p2->u.utility.paramlen
		    && memcmp(p1->u.utility.params, p2->u.utility.params,
			      p1->u.utility.paramlen) == 0;

	case CFCTRL_SRV_VIDEO:
		return p1->u.video.connid == p2->u.video.connid;
	case CFCTRL_SRV_DBG:
		return true;
	case CFCTRL_SRV_DECM:
		return false;
	default:
		return false;
	}
	return false;
}

bool cfctrl_req_eq(struct cfctrl_request_info *r1,
		   struct cfctrl_request_info *r2)
{
	if (r1->cmd != r2->cmd)
		return false;
	if (r1->cmd == CFCTRL_CMD_LINK_SETUP)
		return param_eq(&r1->param, &r2->param);
	else
		return r1->channel_id == r2->channel_id;
}

/* Insert request at the end */
void cfctrl_insert_req(struct cfctrl *ctrl,
			      struct cfctrl_request_info *req)
{
	struct cfctrl_request_info *p;
	spin_lock(&ctrl->info_list_lock);
	req->next = NULL;
	atomic_inc(&ctrl->req_seq_no);
	req->sequence_no = atomic_read(&ctrl->req_seq_no);
	if (ctrl->first_req == NULL) {
		ctrl->first_req = req;
		spin_unlock(&ctrl->info_list_lock);
		return;
	}
	p = ctrl->first_req;
	while (p->next != NULL)
		p = p->next;
	p->next = req;
	spin_unlock(&ctrl->info_list_lock);
}

static void cfctrl_insert_req2(struct cfctrl *ctrl, enum cfctrl_cmd cmd,
			       u8 linkid, struct cflayer *user_layer)
{
	struct cfctrl_request_info *req = kmalloc(sizeof(*req), GFP_KERNEL);
	if (!req) {
		pr_warning("CAIF: %s(): Out of memory\n", __func__);
		return;
	}
	req->client_layer = user_layer;
	req->cmd = cmd;
	req->channel_id = linkid;
	cfctrl_insert_req(ctrl, req);
}

/* Compare and remove request */
struct cfctrl_request_info *cfctrl_remove_req(struct cfctrl *ctrl,
					      struct cfctrl_request_info *req)
{
	struct cfctrl_request_info *p;
	struct cfctrl_request_info *ret;

	spin_lock(&ctrl->info_list_lock);
	if (ctrl->first_req == NULL) {
		spin_unlock(&ctrl->info_list_lock);
		return NULL;
	}

	if (cfctrl_req_eq(req, ctrl->first_req)) {
		ret = ctrl->first_req;
		caif_assert(ctrl->first_req);
		atomic_set(&ctrl->rsp_seq_no,
				 ctrl->first_req->sequence_no);
		ctrl->first_req = ctrl->first_req->next;
		spin_unlock(&ctrl->info_list_lock);
		return ret;
	}

	p = ctrl->first_req;

	while (p->next != NULL) {
		if (cfctrl_req_eq(req, p->next)) {
			pr_warning("CAIF: %s(): Requests are not "
					"received in order\n",
					__func__);
			ret = p->next;
			atomic_set(&ctrl->rsp_seq_no,
					p->next->sequence_no);
			p->next = p->next->next;
			spin_unlock(&ctrl->info_list_lock);
			return ret;
		}
		p = p->next;
	}
	spin_unlock(&ctrl->info_list_lock);

	pr_warning("CAIF: %s(): Request does not match\n",
		   __func__);
	return NULL;
}

struct cfctrl_rsp *cfctrl_get_respfuncs(struct cflayer *layer)
{
	struct cfctrl *this = container_obj(layer);
	return &this->res;
}

void cfctrl_set_dnlayer(struct cflayer *this, struct cflayer *dn)
{
	this->dn = dn;
}

void cfctrl_set_uplayer(struct cflayer *this, struct cflayer *up)
{
	this->up = up;
}

static void init_info(struct caif_payload_info *info, struct cfctrl *cfctrl)
{
	info->hdr_len = 0;
	info->channel_id = cfctrl->serv.layer.id;
	info->dev_info = &cfctrl->serv.dev_info;
}

void cfctrl_enum_req(struct cflayer *layer, u8 physlinkid)
{
	struct cfctrl *cfctrl = container_obj(layer);
	int ret;
	struct cfpkt *pkt = cfpkt_create(CFPKT_CTRL_PKT_LEN);
	if (!pkt) {
		pr_warning("CAIF: %s(): Out of memory\n", __func__);
		return;
	}
	caif_assert(offsetof(struct cfctrl, serv.layer) == 0);
	init_info(cfpkt_info(pkt), cfctrl);
	cfpkt_info(pkt)->dev_info->id = physlinkid;
	cfctrl->serv.dev_info.id = physlinkid;
	cfpkt_addbdy(pkt, CFCTRL_CMD_ENUM);
	cfpkt_addbdy(pkt, physlinkid);
	ret =
	    cfctrl->serv.layer.dn->transmit(cfctrl->serv.layer.dn, pkt);
	if (ret < 0) {
		pr_err("CAIF: %s(): Could not transmit enum message\n",
			__func__);
		cfpkt_destroy(pkt);
	}
}

void cfctrl_linkup_request(struct cflayer *layer,
			   struct cfctrl_link_param *param,
			   struct cflayer *user_layer)
{
	struct cfctrl *cfctrl = container_obj(layer);
	u32 tmp32;
	u16 tmp16;
	u8 tmp8;
	struct cfctrl_request_info *req;
	int ret;
	char utility_name[16];
	struct cfpkt *pkt = cfpkt_create(CFPKT_CTRL_PKT_LEN);
	if (!pkt) {
		pr_warning("CAIF: %s(): Out of memory\n", __func__);
		return;
	}
	cfpkt_addbdy(pkt, CFCTRL_CMD_LINK_SETUP);
	cfpkt_addbdy(pkt, (param->chtype << 4) + param->linktype);
	cfpkt_addbdy(pkt, (param->priority << 3) + param->phyid);
	cfpkt_addbdy(pkt, param->endpoint & 0x03);

	switch (param->linktype) {
	case CFCTRL_SRV_VEI:
		break;
	case CFCTRL_SRV_VIDEO:
		cfpkt_addbdy(pkt, (u8) param->u.video.connid);
		break;
	case CFCTRL_SRV_DBG:
		break;
	case CFCTRL_SRV_DATAGRAM:
		tmp32 = cpu_to_le32(param->u.datagram.connid);
		cfpkt_add_body(pkt, &tmp32, 4);
		break;
	case CFCTRL_SRV_RFM:
		/* Construct a frame, convert DatagramConnectionID to network
		 * format long and copy it out...
		 */
		tmp32 = cpu_to_le32(param->u.rfm.connid);
		cfpkt_add_body(pkt, &tmp32, 4);
		/* Add volume name, including zero termination... */
		cfpkt_add_body(pkt, param->u.rfm.volume,
			       strlen(param->u.rfm.volume) + 1);
		break;
	case CFCTRL_SRV_UTIL:
		tmp16 = cpu_to_le16(param->u.utility.fifosize_kb);
		cfpkt_add_body(pkt, &tmp16, 2);
		tmp16 = cpu_to_le16(param->u.utility.fifosize_bufs);
		cfpkt_add_body(pkt, &tmp16, 2);
		memset(utility_name, 0, sizeof(utility_name));
		strncpy(utility_name, param->u.utility.name,
			UTILITY_NAME_LENGTH - 1);
		cfpkt_add_body(pkt, utility_name, UTILITY_NAME_LENGTH);
		tmp8 = param->u.utility.paramlen;
		cfpkt_add_body(pkt, &tmp8, 1);
		cfpkt_add_body(pkt, param->u.utility.params,
			       param->u.utility.paramlen);
		break;
	default:
		pr_warning("CAIF: %s():Request setup of bad link type = %d\n",
			   __func__, param->linktype);
	}
	req = kmalloc(sizeof(*req), GFP_KERNEL);
	if (!req) {
		pr_warning("CAIF: %s(): Out of memory\n", __func__);
		return;
	}
	memset(req, 0, sizeof(*req));
	req->client_layer = user_layer;
	req->cmd = CFCTRL_CMD_LINK_SETUP;
	req->param = *param;
	cfctrl_insert_req(cfctrl, req);
	init_info(cfpkt_info(pkt), cfctrl);
	cfpkt_info(pkt)->dev_info->id = param->phyid;
	ret =
	    cfctrl->serv.layer.dn->transmit(cfctrl->serv.layer.dn, pkt);
	if (ret < 0) {
		pr_err("CAIF: %s(): Could not transmit linksetup request\n",
			__func__);
		cfpkt_destroy(pkt);
	}
}

int cfctrl_linkdown_req(struct cflayer *layer, u8 channelid,
				struct cflayer *client)
{
	int ret;
	struct cfctrl *cfctrl = container_obj(layer);
	struct cfpkt *pkt = cfpkt_create(CFPKT_CTRL_PKT_LEN);
	if (!pkt) {
		pr_warning("CAIF: %s(): Out of memory\n", __func__);
		return -ENOMEM;
	}
	cfctrl_insert_req2(cfctrl, CFCTRL_CMD_LINK_DESTROY, channelid, client);
	cfpkt_addbdy(pkt, CFCTRL_CMD_LINK_DESTROY);
	cfpkt_addbdy(pkt, channelid);
	init_info(cfpkt_info(pkt), cfctrl);
	ret =
	    cfctrl->serv.layer.dn->transmit(cfctrl->serv.layer.dn, pkt);
	if (ret < 0) {
		pr_err("CAIF: %s(): Could not transmit link-down request\n",
			__func__);
		cfpkt_destroy(pkt);
	}
	return ret;
}

void cfctrl_sleep_req(struct cflayer *layer)
{
	int ret;
	struct cfctrl *cfctrl = container_obj(layer);
	struct cfpkt *pkt = cfpkt_create(CFPKT_CTRL_PKT_LEN);
	if (!pkt) {
		pr_warning("CAIF: %s(): Out of memory\n", __func__);
		return;
	}
	cfpkt_addbdy(pkt, CFCTRL_CMD_SLEEP);
	init_info(cfpkt_info(pkt), cfctrl);
	ret =
	    cfctrl->serv.layer.dn->transmit(cfctrl->serv.layer.dn, pkt);
	if (ret < 0)
		cfpkt_destroy(pkt);
}

void cfctrl_wake_req(struct cflayer *layer)
{
	int ret;
	struct cfctrl *cfctrl = container_obj(layer);
	struct cfpkt *pkt = cfpkt_create(CFPKT_CTRL_PKT_LEN);
	if (!pkt) {
		pr_warning("CAIF: %s(): Out of memory\n", __func__);
		return;
	}
	cfpkt_addbdy(pkt, CFCTRL_CMD_WAKE);
	init_info(cfpkt_info(pkt), cfctrl);
	ret =
	    cfctrl->serv.layer.dn->transmit(cfctrl->serv.layer.dn, pkt);
	if (ret < 0)
		cfpkt_destroy(pkt);
}

void cfctrl_getstartreason_req(struct cflayer *layer)
{
	int ret;
	struct cfctrl *cfctrl = container_obj(layer);
	struct cfpkt *pkt = cfpkt_create(CFPKT_CTRL_PKT_LEN);
	if (!pkt) {
		pr_warning("CAIF: %s(): Out of memory\n", __func__);
		return;
	}
	cfpkt_addbdy(pkt, CFCTRL_CMD_START_REASON);
	init_info(cfpkt_info(pkt), cfctrl);
	ret =
	    cfctrl->serv.layer.dn->transmit(cfctrl->serv.layer.dn, pkt);
	if (ret < 0)
		cfpkt_destroy(pkt);
}


static int cfctrl_recv(struct cflayer *layer, struct cfpkt *pkt)
{
	u8 cmdrsp;
	u8 cmd;
	int ret = -1;
	u16 tmp16;
	u8 len;
	u8 param[255];
	u8 linkid;
	struct cfctrl *cfctrl = container_obj(layer);
	struct cfctrl_request_info rsp, *req;


	cfpkt_extr_head(pkt, &cmdrsp, 1);
	cmd = cmdrsp & CFCTRL_CMD_MASK;
	if (cmd != CFCTRL_CMD_LINK_ERR
	    && CFCTRL_RSP_BIT != (CFCTRL_RSP_BIT & cmdrsp)) {
		if (handle_loop(cfctrl, cmd, pkt) == CAIF_FAILURE) {
			pr_info("CAIF: %s() CAIF Protocol error:"
				"Response bit not set\n", __func__);
			goto error;
		}
	}

	switch (cmd) {
	case CFCTRL_CMD_LINK_SETUP:
		{
			enum cfctrl_srv serv;
			enum cfctrl_srv servtype;
			u8 endpoint;
			u8 physlinkid;
			u8 prio;
			u8 tmp;
			u32 tmp32;
			u8 *cp;
			int i;
			struct cfctrl_link_param linkparam;
			memset(&linkparam, 0, sizeof(linkparam));

			cfpkt_extr_head(pkt, &tmp, 1);

			serv = tmp & CFCTRL_SRV_MASK;
			linkparam.linktype = serv;

			servtype = tmp >> 4;
			linkparam.chtype = servtype;

			cfpkt_extr_head(pkt, &tmp, 1);
			physlinkid = tmp & 0x07;
			prio = tmp >> 3;

			linkparam.priority = prio;
			linkparam.phyid = physlinkid;
			cfpkt_extr_head(pkt, &endpoint, 1);
			linkparam.endpoint = endpoint & 0x03;

			switch (serv) {
			case CFCTRL_SRV_VEI:
			case CFCTRL_SRV_DBG:
				/* Link ID */
				cfpkt_extr_head(pkt, &linkid, 1);
				break;
			case CFCTRL_SRV_VIDEO:
				cfpkt_extr_head(pkt, &tmp, 1);
				linkparam.u.video.connid = tmp;
				/* Link ID */
				cfpkt_extr_head(pkt, &linkid, 1);
				break;

			case CFCTRL_SRV_DATAGRAM:
				cfpkt_extr_head(pkt, &tmp32, 4);
				linkparam.u.datagram.connid =
				    le32_to_cpu(tmp32);
				/* Link ID */
				cfpkt_extr_head(pkt, &linkid, 1);
				break;
			case CFCTRL_SRV_RFM:
				/* Construct a frame, convert
				 * DatagramConnectionID
				 * to network format long and copy it out...
				 */
				cfpkt_extr_head(pkt, &tmp32, 4);
				linkparam.u.rfm.connid =
				  le32_to_cpu(tmp32);
				cp = (u8 *) linkparam.u.rfm.volume;
				for (cfpkt_extr_head(pkt, &tmp, 1);
				     cfpkt_more(pkt) && tmp != '\0';
				     cfpkt_extr_head(pkt, &tmp, 1))
					*cp++ = tmp;
				*cp = '\0';

				/* Link ID */
				cfpkt_extr_head(pkt, &linkid, 1);

				break;
			case CFCTRL_SRV_UTIL:
				/* Construct a frame, convert
				 * DatagramConnectionID
				 * to network format long and copy it out...
				 */
				/* Fifosize KB */
				cfpkt_extr_head(pkt, &tmp16, 2);
				linkparam.u.utility.fifosize_kb =
				    le16_to_cpu(tmp16);
				/* Fifosize bufs */
				cfpkt_extr_head(pkt, &tmp16, 2);
				linkparam.u.utility.fifosize_bufs =
				    le16_to_cpu(tmp16);
				/* name */
				cp = (u8 *) linkparam.u.utility.name;
				caif_assert(sizeof(linkparam.u.utility.name)
					     >= UTILITY_NAME_LENGTH);
				for (i = 0;
				     i < UTILITY_NAME_LENGTH
				     && cfpkt_more(pkt); i++) {
					cfpkt_extr_head(pkt, &tmp, 1);
					*cp++ = tmp;
				}
				/* Length */
				cfpkt_extr_head(pkt, &len, 1);
				linkparam.u.utility.paramlen = len;
				/* Param Data */
				cp = linkparam.u.utility.params;
				while (cfpkt_more(pkt) && len--) {
					cfpkt_extr_head(pkt, &tmp, 1);
					*cp++ = tmp;
				}
				/* Link ID */
				cfpkt_extr_head(pkt, &linkid, 1);
				/* Length */
				cfpkt_extr_head(pkt, &len, 1);
				/* Param Data */
				cfpkt_extr_head(pkt, &param, len);
				break;
			default:
				pr_warning("CAIF: %s(): Request setup "
					   "- invalid link type (%d)",
					   __func__, serv);
				goto error;
			}

			rsp.cmd = cmd;
			rsp.param = linkparam;
			req = cfctrl_remove_req(cfctrl, &rsp);

			if (CFCTRL_ERR_BIT == (CFCTRL_ERR_BIT & cmdrsp) ||
				cfpkt_erroneous(pkt)) {
				pr_err("CAIF: %s(): Invalid O/E bit or parse "
				       "error on CAIF control channel",
					__func__);
				cfctrl->res.reject_rsp(cfctrl->serv.layer.up,
						       0,
						       req ? req->client_layer
						       : NULL);
			} else {
				cfctrl->res.linksetup_rsp(cfctrl->serv.
							  layer.up, linkid,
							  serv, physlinkid,
							  req ? req->
							  client_layer : NULL);
			}

			if (req != NULL)
				kfree(req);
		}
		break;
	case CFCTRL_CMD_LINK_DESTROY:
		cfpkt_extr_head(pkt, &linkid, 1);
		rsp.cmd = cmd;
		rsp.channel_id = linkid;
		req = cfctrl_remove_req(cfctrl, &rsp);
		cfctrl->res.linkdestroy_rsp(cfctrl->serv.layer.up, linkid,
					    req ? req->client_layer : NULL);
		if (req != NULL)
			kfree(req);
		break;
	case CFCTRL_CMD_LINK_ERR:
		pr_err("CAIF: %s(): Frame Error Indication received\n",
			__func__);
		cfctrl->res.linkerror_ind();
		break;
	case CFCTRL_CMD_ENUM:
		cfctrl->res.enum_rsp();
		break;
	case CFCTRL_CMD_SLEEP:
		cfctrl->res.sleep_rsp();
		break;
	case CFCTRL_CMD_WAKE:
		cfctrl->res.wake_rsp();
		break;
	case CFCTRL_CMD_LINK_RECONF:
		cfctrl->res.restart_rsp();
		break;
	case CFCTRL_CMD_RADIO_SET:
		cfctrl->res.radioset_rsp();
		break;
	default:
		pr_err("CAIF: %s(): Unrecognized Control Frame\n", __func__);
		goto error;
		break;
	}
	ret = 0;
error:
	cfpkt_destroy(pkt);
	return ret;
}

static void cfctrl_ctrlcmd(struct cflayer *layr, enum caif_ctrlcmd ctrl,
			int phyid)
{
	struct cfctrl *this = container_obj(layr);
	switch (ctrl) {
	case _CAIF_CTRLCMD_PHYIF_FLOW_OFF_IND:
	case CAIF_CTRLCMD_FLOW_OFF_IND:
		spin_lock(&this->info_list_lock);
		if (this->first_req != NULL) {
			pr_warning("CAIF: %s(): Received flow off in "
				   "control layer", __func__);
		}
		spin_unlock(&this->info_list_lock);
		break;
	default:
		break;
	}
}

#ifndef CAIF_NO_LOOP
static int handle_loop(struct cfctrl *ctrl, int cmd, struct cfpkt *pkt)
{
	static int last_linkid;
	u8 linkid, linktype, tmp;
	switch (cmd) {
	case CFCTRL_CMD_LINK_SETUP:
		spin_lock(&ctrl->loop_linkid_lock);
		for (linkid = last_linkid + 1; linkid < 255; linkid++)
			if (!ctrl->loop_linkused[linkid])
				goto found;
		for (linkid = last_linkid - 1; linkid > 0; linkid--)
			if (!ctrl->loop_linkused[linkid])
				goto found;
		spin_unlock(&ctrl->loop_linkid_lock);
		return -EINVAL;
found:
		if (!ctrl->loop_linkused[linkid])
			ctrl->loop_linkused[linkid] = 1;

		last_linkid = linkid;

		cfpkt_add_trail(pkt, &linkid, 1);
		spin_unlock(&ctrl->loop_linkid_lock);
		cfpkt_peek_head(pkt, &linktype, 1);
		if (linktype ==  CFCTRL_SRV_UTIL) {
			tmp = 0x01;
			cfpkt_add_trail(pkt, &tmp, 1);
			cfpkt_add_trail(pkt, &tmp, 1);
		}
		break;

	case CFCTRL_CMD_LINK_DESTROY:
		spin_lock(&ctrl->loop_linkid_lock);
		cfpkt_peek_head(pkt, &linkid, 1);
		ctrl->loop_linkused[linkid] = 0;
		spin_unlock(&ctrl->loop_linkid_lock);
		break;
	default:
		break;
	}
	return CAIF_SUCCESS;
}
#endif
