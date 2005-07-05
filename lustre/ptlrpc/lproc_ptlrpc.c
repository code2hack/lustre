/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 *  Copyright (C) 2002 Cluster File Systems, Inc.
 *
 *   This file is part of Lustre, http://www.lustre.org.
 *
 *   Lustre is free software; you can redistribute it and/or
 *   modify it under the terms of version 2 of the GNU General Public
 *   License as published by the Free Software Foundation.
 *
 *   Lustre is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with Lustre; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */
#define DEBUG_SUBSYSTEM S_CLASS

#include <linux/obd_support.h>
#include <linux/obd.h>
#include <linux/lprocfs_status.h>
#include <linux/lustre_idl.h>
#include <linux/lustre_net.h>
#include <linux/obd_class.h>
#include <linux/seq_file.h>
#include "ptlrpc_internal.h"


struct ll_rpc_opcode {
     __u32       opcode;
     const char *opname;
} ll_rpc_opcode_table[LUSTRE_MAX_OPCODES] = {
        { OST_REPLY,        "ost_reply" },
        { OST_GETATTR,      "ost_getattr" },
        { OST_SETATTR,      "ost_setattr" },
        { OST_READ,         "ost_read" },
        { OST_WRITE,        "ost_write" },
        { OST_CREATE ,      "ost_create" },
        { OST_DESTROY,      "ost_destroy" },
        { OST_GET_INFO,     "ost_get_info" },
        { OST_CONNECT,      "ost_connect" },
        { OST_DISCONNECT,   "ost_disconnect" },
        { OST_PUNCH,        "ost_punch" },
        { OST_OPEN,         "ost_open" },
        { OST_CLOSE,        "ost_close" },
        { OST_STATFS,       "ost_statfs" },
        { OST_SAN_READ,     "ost_san_read" },
        { OST_SAN_WRITE,    "ost_san_write" },
        { OST_SYNC,         "ost_sync" },
        { OST_SET_INFO,     "ost_set_info" },
        { OST_QUOTACHECK,   "ost_quotacheck" },
        { OST_QUOTACTL,     "ost_quotactl" },
        { MDS_GETATTR,      "mds_getattr" },
        { MDS_GETATTR_NAME, "mds_getattr_name" },
        { MDS_CLOSE,        "mds_close" },
        { MDS_REINT,        "mds_reint" },
        { MDS_READPAGE,     "mds_readpage" },
        { MDS_CONNECT,      "mds_connect" },
        { MDS_DISCONNECT,   "mds_disconnect" },
        { MDS_GETSTATUS,    "mds_getstatus" },
        { MDS_STATFS,       "mds_statfs" },
        { MDS_PIN,          "mds_pin" },
        { MDS_UNPIN,        "mds_unpin" },
        { MDS_SYNC,         "mds_sync" },
        { MDS_DONE_WRITING, "mds_done_writing" },
        { MDS_SET_INFO,     "mds_set_info" },
        { MDS_QUOTACHECK,   "mds_quotacheck" },
        { MDS_QUOTACTL,     "mds_quotactl" },
        { LDLM_ENQUEUE,     "ldlm_enqueue" },
        { LDLM_CONVERT,     "ldlm_convert" },
        { LDLM_CANCEL,      "ldlm_cancel" },
        { LDLM_BL_CALLBACK, "ldlm_bl_callback" },
        { LDLM_CP_CALLBACK, "ldlm_cp_callback" },
        { LDLM_GL_CALLBACK, "ldlm_gl_callback" },
        { PTLBD_QUERY,      "ptlbd_query" },
        { PTLBD_READ,       "ptlbd_read" },
        { PTLBD_WRITE,      "ptlbd_write" },
        { PTLBD_FLUSH,      "ptlbd_flush" },
        { PTLBD_CONNECT,    "ptlbd_connect" },
        { PTLBD_DISCONNECT, "ptlbd_disconnect" },
        { OBD_PING,         "obd_ping" },
        { OBD_LOG_CANCEL,   "llog_origin_handle_cancel"},
};

const char* ll_opcode2str(__u32 opcode)
{
        /* When one of the assertions below fail, chances are that:
         *     1) A new opcode was added in lustre_idl.h, but was
         *        is missing from the table above.
         * or  2) The opcode space was renumbered or rearranged,
         *        and the opcode_offset() function in
         *        ptlrpc_internals.h needs to be modified.
         */
        __u32 offset = opcode_offset(opcode);
        LASSERT(offset < LUSTRE_MAX_OPCODES);
        LASSERT(ll_rpc_opcode_table[offset].opcode == opcode);
        return ll_rpc_opcode_table[offset].opname;
}

#ifdef LPROCFS
void ptlrpc_lprocfs_register(struct proc_dir_entry *root, char *dir,
                             char *name, struct proc_dir_entry **procroot_ret,
                             struct lprocfs_stats **stats_ret)
{
        struct proc_dir_entry *svc_procroot;
        struct lprocfs_stats *svc_stats;
        int i, rc;
        unsigned int svc_counter_config = LPROCFS_CNTR_AVGMINMAX |
                                          LPROCFS_CNTR_STDDEV;

        LASSERT(*procroot_ret == NULL);
        LASSERT(*stats_ret == NULL);

        svc_stats = lprocfs_alloc_stats(PTLRPC_LAST_CNTR + LUSTRE_MAX_OPCODES);
        if (svc_stats == NULL)
                return;

        if (dir) {
                svc_procroot = lprocfs_register(dir, root, NULL, NULL);
                if (IS_ERR(svc_procroot)) {
                        lprocfs_free_stats(svc_stats);
                        return;
                }
        } else {
                svc_procroot = root;
        }

        lprocfs_counter_init(svc_stats, PTLRPC_REQWAIT_CNTR,
                             svc_counter_config, "req_waittime", "usec");
        lprocfs_counter_init(svc_stats, PTLRPC_REQQDEPTH_CNTR,
                             svc_counter_config, "req_qdepth", "reqs");
        lprocfs_counter_init(svc_stats, PTLRPC_REQACTIVE_CNTR,
                             svc_counter_config, "req_active", "reqs");
        lprocfs_counter_init(svc_stats, PTLRPC_REQBUF_AVAIL_CNTR,
                             svc_counter_config, "reqbuf_avail", "bufs");
        for (i = 0; i < LUSTRE_MAX_OPCODES; i++) {
                __u32 opcode = ll_rpc_opcode_table[i].opcode;
                lprocfs_counter_init(svc_stats, PTLRPC_LAST_CNTR + i,
                                     svc_counter_config, ll_opcode2str(opcode),
                                     "usec");
        }

        rc = lprocfs_register_stats(svc_procroot, name, svc_stats);
        if (rc < 0) {
                if (dir)
                        lprocfs_remove(svc_procroot);
                lprocfs_free_stats(svc_stats);
        } else {
                if (dir)
                        *procroot_ret = svc_procroot;
                *stats_ret = svc_stats;
        }
}

static int
ptlrpc_lprocfs_read_req_history_len(char *page, char **start, off_t off,
                                    int count, int *eof, void *data)
{
        struct ptlrpc_service *svc = data;

        *eof = 1;
        return snprintf(page, count, "%d\n", svc->srv_n_history_rqbds);
}

static int
ptlrpc_lprocfs_read_req_history_max(char *page, char **start, off_t off,
                                    int count, int *eof, void *data)
{
        struct ptlrpc_service *svc = data;

        *eof = 1;
        return snprintf(page, count, "%d\n", svc->srv_max_history_rqbds);
}

static int
ptlrpc_lprocfs_write_req_history_max(struct file *file, const char *buffer,
                                     unsigned long count, void *data)
{
        struct ptlrpc_service *svc = data;
        int                    bufpages;
        unsigned long          flags;
        int                    val;
        int                    rc = lprocfs_write_helper(buffer, count, &val);

        if (rc < 0)
                return rc;

        if (val < 0)
                return -ERANGE;

        /* This sanity check is more of an insanity check; we can still
         * hose a kernel by allowing the request history to grow too
         * far. */
        bufpages = (svc->srv_buf_size + PAGE_SIZE - 1)/PAGE_SIZE;
        if (val > num_physpages/(2*bufpages))
                return -ERANGE;

        spin_lock_irqsave(&svc->srv_lock, flags);
        svc->srv_max_history_rqbds = val;
        spin_unlock_irqrestore(&svc->srv_lock, flags);

        return count;
}

struct ptlrpc_srh_iterator {
        __u64                  srhi_seq;
        struct ptlrpc_request *srhi_req;
};

int
ptlrpc_lprocfs_svc_req_history_seek(struct ptlrpc_service *svc,
                                    struct ptlrpc_srh_iterator *srhi,
                                    __u64 seq)
{
        struct list_head      *e;
        struct ptlrpc_request *req;

        if (srhi->srhi_req != NULL &&
            srhi->srhi_seq > svc->srv_request_max_cull_seq &&
            srhi->srhi_seq <= seq) {
                /* If srhi_req was set previously, hasn't been culled and
                 * we're searching for a seq on or after it (i.e. more
                 * recent), search from it onwards.
                 * Since the service history is LRU (i.e. culled reqs will
                 * be near the head), we shouldn't have to do long
                 * re-scans */
                LASSERT (srhi->srhi_seq == srhi->srhi_req->rq_history_seq);
                LASSERT (!list_empty(&svc->srv_request_history));
                e = &srhi->srhi_req->rq_history_list;
        } else {
                /* search from start */
                e = svc->srv_request_history.next;
        }

        while (e != &svc->srv_request_history) {
                req = list_entry(e, struct ptlrpc_request, rq_history_list);

                if (req->rq_history_seq >= seq) {
                        srhi->srhi_seq = req->rq_history_seq;
                        srhi->srhi_req = req;
                        return 0;
                }
                e = e->next;
        }

        return -ENOENT;
}

static void *
ptlrpc_lprocfs_svc_req_history_start(struct seq_file *s, loff_t *pos)
{
        struct ptlrpc_service       *svc = s->private;
        struct ptlrpc_srh_iterator  *srhi;
        unsigned long                flags;
        int                          rc;

        OBD_ALLOC(srhi, sizeof(*srhi));
        if (srhi == NULL)
                return NULL;

        srhi->srhi_seq = 0;
        srhi->srhi_req = NULL;

        spin_lock_irqsave(&svc->srv_lock, flags);
        rc = ptlrpc_lprocfs_svc_req_history_seek(svc, srhi, *pos);
        spin_unlock_irqrestore(&svc->srv_lock, flags);

        if (rc == 0) {
                *pos = srhi->srhi_seq;
                return srhi;
        }

        OBD_FREE(srhi, sizeof(*srhi));
        return NULL;
}

static void
ptlrpc_lprocfs_svc_req_history_stop(struct seq_file *s, void *iter)
{
        struct ptlrpc_srh_iterator *srhi = iter;

        if (srhi != NULL)
                OBD_FREE(srhi, sizeof(*srhi));
}

static void *
ptlrpc_lprocfs_svc_req_history_next(struct seq_file *s,
                                    void *iter, loff_t *pos)
{
        struct ptlrpc_service       *svc = s->private;
        struct ptlrpc_srh_iterator  *srhi = iter;
        unsigned long                flags;
        int                          rc;

        spin_lock_irqsave(&svc->srv_lock, flags);
        rc = ptlrpc_lprocfs_svc_req_history_seek(svc, srhi, *pos + 1);
        spin_unlock_irqrestore(&svc->srv_lock, flags);

        if (rc != 0) {
                OBD_FREE(srhi, sizeof(*srhi));
                return NULL;
        }

        *pos = srhi->srhi_seq;
        return srhi;
}

static int ptlrpc_lprocfs_svc_req_history_show(struct seq_file *s, void *iter)
{
        struct ptlrpc_service      *svc = s->private;
        struct ptlrpc_srh_iterator *srhi = iter;
        struct ptlrpc_request      *req;
        unsigned long               flags;
        int                         rc;

        spin_lock_irqsave(&svc->srv_lock, flags);

        rc = ptlrpc_lprocfs_svc_req_history_seek(svc, srhi, srhi->srhi_seq);

        if (rc == 0) {
                req = srhi->srhi_req;

                /* Print common req fields.
                 * CAVEAT EMPTOR: we're racing with the service handler
                 * here.  The request could contain any old crap, so you
                 * must be just as careful as the service's request
                 * parser. Currently I only print stuff here I know is OK
                 * to look at coz it was set up in request_in_callback()!!! */
                seq_printf(s, LPD64":%s:"LPD64":%d:%s ",
                           req->rq_history_seq, libcfs_id2str(req->rq_peer),
                           req->rq_xid, req->rq_reqlen,ptlrpc_rqphase2str(req));

                if (svc->srv_request_history_print_fn == NULL)
                        seq_printf(s, "\n");
                else
                        svc->srv_request_history_print_fn(s, srhi->srhi_req);
        }

        spin_unlock_irqrestore(&svc->srv_lock, flags);

        return rc;
}

static int
ptlrpc_lprocfs_svc_req_history_open(struct inode *inode, struct file *file)
{
        static struct seq_operations sops = {
                .start = ptlrpc_lprocfs_svc_req_history_start,
                .stop  = ptlrpc_lprocfs_svc_req_history_stop,
                .next  = ptlrpc_lprocfs_svc_req_history_next,
                .show  = ptlrpc_lprocfs_svc_req_history_show,
        };
        struct proc_dir_entry *dp = PDE(inode);
        struct seq_file       *seqf;
        int                    rc;

        rc = seq_open(file, &sops);

        if (rc == 0) {
                seqf = file->private_data;
                seqf->private = dp->data;
        }

        return rc;
}

void ptlrpc_lprocfs_register_service(struct proc_dir_entry *entry,
                                     struct ptlrpc_service *svc)
{
        struct lprocfs_vars lproc_vars[] = {
                {.name       = "req_buffer_history_len",
                 .write_fptr = NULL,
                 .read_fptr  = ptlrpc_lprocfs_read_req_history_len,
                 .data       = svc},
                {.name       = "req_buffer_history_max",
                 .write_fptr = ptlrpc_lprocfs_write_req_history_max,
                 .read_fptr  = ptlrpc_lprocfs_read_req_history_max,
                 .data       = svc},
                {NULL}
        };
        static struct file_operations req_history_fops = {
                .owner       = THIS_MODULE,
                .open        = ptlrpc_lprocfs_svc_req_history_open,
                .read        = seq_read,
                .llseek      = seq_lseek,
                .release     = seq_release,
        };
        struct proc_dir_entry *req_history;

        ptlrpc_lprocfs_register(entry, svc->srv_name,
                                "stats", &svc->srv_procroot,
                                &svc->srv_stats);

        if (svc->srv_procroot == NULL)
                return;

        lprocfs_add_vars(svc->srv_procroot, lproc_vars, NULL);

        req_history = create_proc_entry("req_history", 0400,
                                        svc->srv_procroot);
        if (req_history != NULL) {
                req_history->data = svc;
                req_history->proc_fops = &req_history_fops;
        }
}

void ptlrpc_lprocfs_register_obd(struct obd_device *obddev)
{
        ptlrpc_lprocfs_register(obddev->obd_proc_entry, NULL, "stats",
                                &obddev->obd_svc_procroot,
                                &obddev->obd_svc_stats);
}
EXPORT_SYMBOL(ptlrpc_lprocfs_register_obd);

void ptlrpc_lprocfs_rpc_sent(struct ptlrpc_request *req)
{
        struct lprocfs_stats *svc_stats;
        int opc =  opcode_offset(req->rq_reqmsg->opc);

        svc_stats = req->rq_import->imp_obd->obd_svc_stats;
        if (svc_stats == NULL || opc <= 0)
                return;
        LASSERT(opc < LUSTRE_MAX_OPCODES);
        lprocfs_counter_add(svc_stats, opc + PTLRPC_LAST_CNTR, 0);
}

void ptlrpc_lprocfs_unregister_service(struct ptlrpc_service *svc)
{
        if (svc->srv_procroot != NULL) {
                lprocfs_remove(svc->srv_procroot);
                svc->srv_procroot = NULL;
        }
        if (svc->srv_stats) {
                lprocfs_free_stats(svc->srv_stats);
                svc->srv_stats = NULL;
        }
}

void ptlrpc_lprocfs_unregister_obd(struct obd_device *obd)
{
        if (obd->obd_svc_procroot) {
                lprocfs_remove(obd->obd_svc_procroot);
                obd->obd_svc_procroot = NULL;
        }
        if (obd->obd_svc_stats) {
                lprocfs_free_stats(obd->obd_svc_stats);
                obd->obd_svc_stats = NULL;
        }
}
EXPORT_SYMBOL(ptlrpc_lprocfs_unregister_obd);


int lprocfs_wr_evict_client(struct file *file, const char *buffer,
                            unsigned long count, void *data)
{
        struct obd_device *obd = data;
        struct obd_export *doomed_exp = NULL;
        struct obd_uuid doomed;
        struct list_head *p;
        char tmpbuf[sizeof(doomed)];

        sscanf(buffer, "%40s", tmpbuf);
        obd_str2uuid(&doomed, tmpbuf);

        spin_lock(&obd->obd_dev_lock);
        list_for_each(p, &obd->obd_exports) {
                doomed_exp = list_entry(p, struct obd_export, exp_obd_chain);
                if (obd_uuid_equals(&doomed, &doomed_exp->exp_client_uuid)) {
                        class_export_get(doomed_exp);
                        break;
                }
                doomed_exp = NULL;
        }
        spin_unlock(&obd->obd_dev_lock);

        if (doomed_exp == NULL) {
                CERROR("can't disconnect %s: no export found\n", doomed.uuid);
        } else {
                CERROR("evicting %s at adminstrative request\n", doomed.uuid);
                class_fail_export(doomed_exp);
                class_export_put(doomed_exp);
        }
        return count;
}
EXPORT_SYMBOL(lprocfs_wr_evict_client);

int lprocfs_wr_ping(struct file *file, const char *buffer,
                    unsigned long count, void *data)
{
        struct obd_device *obd = data;
        struct ptlrpc_request *req;
        int rc;
        ENTRY;

        req = ptlrpc_prep_req(obd->u.cli.cl_import, OBD_PING, 0, NULL, NULL);
        if (req == NULL)
                RETURN(-ENOMEM);

        req->rq_replen = lustre_msg_size(0, NULL);
        req->rq_send_state = LUSTRE_IMP_FULL;
        req->rq_no_resend = 1;

        rc = ptlrpc_queue_wait(req);

        ptlrpc_req_finished(req);
        if (rc >= 0)
                RETURN(count);
        RETURN(rc);
}
EXPORT_SYMBOL(lprocfs_wr_ping);

#endif /* LPROCFS */
