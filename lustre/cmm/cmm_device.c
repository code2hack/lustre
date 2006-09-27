/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 *  lustre/cmm/cmm_device.c
 *  Lustre Cluster Metadata Manager (cmm)
 *
 *  Copyright (c) 2006 Cluster File Systems, Inc.
 *   Author: Mike Pershin <tappro@clusterfs.com>
 *
 *   This file is part of the Lustre file system, http://www.lustre.org
 *   Lustre is a trademark of Cluster File Systems, Inc.
 *
 *   You may have signed or agreed to another license before downloading
 *   this software.  If so, you are bound by the terms and conditions
 *   of that agreement, and the following does not apply to you.  See the
 *   LICENSE file included with this distribution for more information.
 *
 *   If you did not agree to a different license, then this copy of Lustre
 *   is open source software; you can redistribute it and/or modify it
 *   under the terms of version 2 of the GNU General Public License as
 *   published by the Free Software Foundation.
 *
 *   In either case, Lustre is distributed in the hope that it will be
 *   useful, but WITHOUT ANY WARRANTY; without even the implied warranty
 *   of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   license text for more details.
 */

#ifndef EXPORT_SYMTAB
# define EXPORT_SYMTAB
#endif
#define DEBUG_SUBSYSTEM S_MDS

#include <linux/module.h>

#include <obd.h>
#include <obd_class.h>
#include <lprocfs_status.h>
#include <lustre_ver.h>
#include "cmm_internal.h"
#include "mdc_internal.h"

static struct obd_ops cmm_obd_device_ops = {
        .o_owner           = THIS_MODULE
};

static struct lu_device_operations cmm_lu_ops;

static inline int lu_device_is_cmm(struct lu_device *d)
{
	return ergo(d != NULL && d->ld_ops != NULL, d->ld_ops == &cmm_lu_ops);
}

int cmm_root_get(const struct lu_context *ctx, struct md_device *md,
                 struct lu_fid *fid, struct md_ucred *uc)
{
        struct cmm_device *cmm_dev = md2cmm_dev(md);
        /* valid only on master MDS */
        if (cmm_dev->cmm_local_num == 0)
                return cmm_child_ops(cmm_dev)->mdo_root_get(ctx,
                                     cmm_dev->cmm_child, fid, uc);
        else
                return -EINVAL;
}

static int cmm_statfs(const struct lu_context *ctxt, struct md_device *md,
                      struct kstatfs *sfs, struct md_ucred *uc) {
        struct cmm_device *cmm_dev = md2cmm_dev(md);
	int rc;

        ENTRY;
        rc = cmm_child_ops(cmm_dev)->mdo_statfs(ctxt,
                                                cmm_dev->cmm_child, sfs, uc);
        RETURN (rc);
}

static int cmm_maxsize_get(const struct lu_context *ctxt, struct md_device *md,
                           int *md_size, int *cookie_size, struct md_ucred *uc)
{
        struct cmm_device *cmm_dev = md2cmm_dev(md);
        int rc;
        ENTRY;
        rc = cmm_child_ops(cmm_dev)->mdo_maxsize_get(ctxt, cmm_dev->cmm_child,
                                                     md_size, cookie_size, uc);
        RETURN(rc);
}

static int cmm_init_capa_keys(struct md_device *md,
                              struct lustre_capa_key *keys)
{
        struct cmm_device *cmm_dev = md2cmm_dev(md);
        int rc;
        ENTRY;
        LASSERT(cmm_child_ops(cmm_dev)->mdo_init_capa_keys);
        rc = cmm_child_ops(cmm_dev)->mdo_init_capa_keys(cmm_dev->cmm_child,
                                                        keys);
        RETURN(rc);
}

static int cmm_update_capa_key(const struct lu_context *ctxt,
                               struct md_device *md,
                               struct lustre_capa_key *key)
{
        struct cmm_device *cmm_dev = md2cmm_dev(md);
        int rc;
        ENTRY;
        rc = cmm_child_ops(cmm_dev)->mdo_update_capa_key(ctxt,
                                                         cmm_dev->cmm_child,
                                                         key);
        RETURN(rc);
}

static struct md_device_operations cmm_md_ops = {
        .mdo_statfs         = cmm_statfs,
        .mdo_root_get       = cmm_root_get,
        .mdo_maxsize_get    = cmm_maxsize_get,
        .mdo_init_capa_keys = cmm_init_capa_keys,
        .mdo_update_capa_key= cmm_update_capa_key,
};

extern struct lu_device_type mdc_device_type;

/* --- cmm_lu_operations --- */
/* add new MDC to the CMM, create MDC lu_device and connect it to mdc_obd */
static int cmm_add_mdc(const struct lu_context *ctx,
                       struct cmm_device *cm, struct lustre_cfg *cfg)
{
        struct lu_device_type *ldt = &mdc_device_type;
        char *p, *num = lustre_cfg_string(cfg, 2);
        struct mdc_device *mc, *tmp;
        struct lu_fld_target target;
        struct lu_device *ld;
        struct lu_site *ls;
        mdsno_t mdc_num;
        int rc;
        ENTRY;

        /* find out that there is no such mdc */
        LASSERT(num);
        mdc_num = simple_strtol(num, &p, 10);
        if (*p) {
                CERROR("Invalid index in lustre_cgf, offset 2\n");
                RETURN(-EINVAL);
        }

        spin_lock(&cm->cmm_tgt_guard);
        list_for_each_entry_safe(mc, tmp, &cm->cmm_targets,
                                 mc_linkage) {
                if (mc->mc_num == mdc_num) {
                        spin_unlock(&cm->cmm_tgt_guard);
                        RETURN(-EEXIST);
                }
        }
        spin_unlock(&cm->cmm_tgt_guard);
        ld = ldt->ldt_ops->ldto_device_alloc(ctx, ldt, cfg);
        ld->ld_site = cmm2lu_dev(cm)->ld_site;

        rc = ldt->ldt_ops->ldto_device_init(ctx, ld, NULL);
        if (rc) {
                ldt->ldt_ops->ldto_device_free(ctx, ld);
                RETURN (rc);
        }
        /* pass config to the just created MDC */
        rc = ld->ld_ops->ldo_process_config(ctx, ld, cfg);
        if (rc == 0) {
                spin_lock(&cm->cmm_tgt_guard);
                list_for_each_entry_safe(mc, tmp, &cm->cmm_targets,
                                         mc_linkage) {
                        if (mc->mc_num == mdc_num) {
                                spin_unlock(&cm->cmm_tgt_guard);
                                ldt->ldt_ops->ldto_device_fini(ctx, ld);
                                ldt->ldt_ops->ldto_device_free(ctx, ld);
                                RETURN(-EEXIST);
                        }
                }
                mc = lu2mdc_dev(ld);
                list_add_tail(&mc->mc_linkage, &cm->cmm_targets);
                cm->cmm_tgt_count++;
                spin_unlock(&cm->cmm_tgt_guard);

                lu_device_get(cmm2lu_dev(cm));

                ls = cm->cmm_md_dev.md_lu_dev.ld_site;

                target.ft_srv = NULL;
                target.ft_idx = mc->mc_num;
                target.ft_exp = mc->mc_desc.cl_exp;
        
                fld_client_add_target(ls->ls_client_fld, &target);
        }
        RETURN(rc);
}

static void cmm_device_shutdown(const struct lu_context *ctx,
                                struct cmm_device *cm)
{
        struct mdc_device *mc, *tmp;
        ENTRY;

        /* finish all mdc devices */
        spin_lock(&cm->cmm_tgt_guard);
        list_for_each_entry_safe(mc, tmp, &cm->cmm_targets, mc_linkage) {
                struct lu_device *ld_m = mdc2lu_dev(mc);

                list_del_init(&mc->mc_linkage);
                lu_device_put(cmm2lu_dev(cm));
                ld_m->ld_type->ldt_ops->ldto_device_fini(ctx, ld_m);
                ld_m->ld_type->ldt_ops->ldto_device_free(ctx, ld_m);
                cm->cmm_tgt_count--;
        }
        spin_unlock(&cm->cmm_tgt_guard);

        EXIT;
}
static int cmm_device_mount(const struct lu_context *ctx,
                            struct cmm_device *m, struct lustre_cfg *cfg)
{
        const char *index = lustre_cfg_string(cfg, 2);
        char *p;
        
        LASSERT(index != NULL);

        m->cmm_local_num = simple_strtol(index, &p, 10);
        if (*p) {
                CERROR("Invalid index in lustre_cgf\n");
                RETURN(-EINVAL);
        }
        
        RETURN(0);
}

static int cmm_process_config(const struct lu_context *ctx,
                              struct lu_device *d, struct lustre_cfg *cfg)
{
        struct cmm_device *m = lu2cmm_dev(d);
        struct lu_device *next = md2lu_dev(m->cmm_child);
        int err;
        ENTRY;

        switch(cfg->lcfg_command) {
        case LCFG_ADD_MDC:
                err = cmm_add_mdc(ctx, m, cfg);
                /* the first ADD_MDC can be counted as setup is finished */
                if ((m->cmm_flags & CMM_INITIALIZED) == 0)
                        m->cmm_flags |= CMM_INITIALIZED;
                break;
        case LCFG_SETUP:
        {
                /* lower layers should be set up at first */
                err = next->ld_ops->ldo_process_config(ctx, next, cfg);
                if (err == 0)
                        err = cmm_device_mount(ctx, m, cfg);
                break;
        }
        case LCFG_CLEANUP:
        {
                cmm_device_shutdown(ctx, m);
        }
        default:
                err = next->ld_ops->ldo_process_config(ctx, next, cfg);
        }
        RETURN(err);
}

static int cmm_recovery_complete(const struct lu_context *ctxt,
                                 struct lu_device *d)
{
        struct cmm_device *m = lu2cmm_dev(d);
        struct lu_device *next = md2lu_dev(m->cmm_child);
        int rc;
        ENTRY;
        rc = next->ld_ops->ldo_recovery_complete(ctxt, next);
        RETURN(rc);
}

static struct lu_device_operations cmm_lu_ops = {
	.ldo_object_alloc      = cmm_object_alloc,
        .ldo_process_config    = cmm_process_config,
        .ldo_recovery_complete = cmm_recovery_complete
};

/* --- lu_device_type operations --- */
int cmm_upcall(const struct lu_context *ctxt, struct md_device *md,
               enum md_upcall_event ev)
{
        struct md_device *upcall_dev;
        int rc;
        ENTRY;

        upcall_dev = md->md_upcall.mu_upcall_dev;

        LASSERT(upcall_dev);
        rc = upcall_dev->md_upcall.mu_upcall(ctxt, md->md_upcall.mu_upcall_dev, ev);

        RETURN(rc);
}

static struct lu_device *cmm_device_alloc(const struct lu_context *ctx,
                                          struct lu_device_type *t,
                                          struct lustre_cfg *cfg)
{
        struct lu_device  *l;
        struct cmm_device *m;

        ENTRY;

        OBD_ALLOC_PTR(m);
        if (m == NULL) {
                l = ERR_PTR(-ENOMEM);
        } else {
                md_device_init(&m->cmm_md_dev, t);
                m->cmm_md_dev.md_ops = &cmm_md_ops;
                m->cmm_md_dev.md_upcall.mu_upcall = cmm_upcall;
	        l = cmm2lu_dev(m);
                l->ld_ops = &cmm_lu_ops;
        }

        RETURN (l);
}

static void cmm_device_free(const struct lu_context *ctx, struct lu_device *d)
{
        struct cmm_device *m = lu2cmm_dev(d);

        LASSERT(m->cmm_tgt_count == 0);
        LASSERT(list_empty(&m->cmm_targets));
	md_device_fini(&m->cmm_md_dev);
        OBD_FREE_PTR(m);
}

/* context key constructor/destructor */
static void *cmm_thread_init(const struct lu_context *ctx,
                             struct lu_context_key *key)
{
        struct cmm_thread_info *info;

        CLASSERT(CFS_PAGE_SIZE >= sizeof *info);
        OBD_ALLOC_PTR(info);
        if (info == NULL)
                info = ERR_PTR(-ENOMEM);
        return info;
}

static void cmm_thread_fini(const struct lu_context *ctx,
                            struct lu_context_key *key, void *data)
{
        struct cmm_thread_info *info = data;
        OBD_FREE_PTR(info);
}

struct lu_context_key cmm_thread_key = {
        .lct_tags = LCT_MD_THREAD,
        .lct_init = cmm_thread_init,
        .lct_fini = cmm_thread_fini
};

static int cmm_type_init(struct lu_device_type *t)
{
        return lu_context_key_register(&cmm_thread_key);
}

static void cmm_type_fini(struct lu_device_type *t)
{
        lu_context_key_degister(&cmm_thread_key);
}

static int cmm_device_init(const struct lu_context *ctx,
                           struct lu_device *d, struct lu_device *next)
{
        struct cmm_device *m = lu2cmm_dev(d);
        int err = 0;
        ENTRY;

        spin_lock_init(&m->cmm_tgt_guard);
        INIT_LIST_HEAD(&m->cmm_targets);
        m->cmm_tgt_count = 0;
        m->cmm_child = lu2md_dev(next);

        RETURN(err);
}

static struct lu_device *cmm_device_fini(const struct lu_context *ctx,
                                         struct lu_device *ld)
{
	struct cmm_device *cm = lu2cmm_dev(ld);
        ENTRY;
        RETURN (md2lu_dev(cm->cmm_child));
}

static struct lu_device_type_operations cmm_device_type_ops = {
        .ldto_init = cmm_type_init,
        .ldto_fini = cmm_type_fini,

        .ldto_device_alloc = cmm_device_alloc,
        .ldto_device_free  = cmm_device_free,

        .ldto_device_init = cmm_device_init,
        .ldto_device_fini = cmm_device_fini
};

static struct lu_device_type cmm_device_type = {
        .ldt_tags     = LU_DEVICE_MD,
        .ldt_name     = LUSTRE_CMM_NAME,
        .ldt_ops      = &cmm_device_type_ops,
        .ldt_ctx_tags = LCT_MD_THREAD | LCT_DT_THREAD
};

struct lprocfs_vars lprocfs_cmm_obd_vars[] = {
        { 0 }
};

struct lprocfs_vars lprocfs_cmm_module_vars[] = {
        { 0 }
};

LPROCFS_INIT_VARS(cmm, lprocfs_cmm_module_vars, lprocfs_cmm_obd_vars);

static int __init cmm_mod_init(void)
{
        struct lprocfs_static_vars lvars;

        printk(KERN_INFO "Lustre: Clustered Metadata Manager; info@clusterfs.com\n");

        lprocfs_init_vars(cmm, &lvars);
        return class_register_type(&cmm_obd_device_ops, NULL, lvars.module_vars,
                                   LUSTRE_CMM_NAME, &cmm_device_type);
}

static void __exit cmm_mod_exit(void)
{
        class_unregister_type(LUSTRE_CMM_NAME);
}

MODULE_AUTHOR("Cluster File Systems, Inc. <info@clusterfs.com>");
MODULE_DESCRIPTION("Lustre Clustered Metadata Manager ("LUSTRE_CMM_NAME")");
MODULE_LICENSE("GPL");

cfs_module(cmm, "0.1.0", cmm_mod_init, cmm_mod_exit);
