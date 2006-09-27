/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 *  Copyright (c) 2004 - 2005 Cluster File Systems, Inc.
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
 */

#include <linux/fs.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/smp_lock.h>

#define DEBUG_SUBSYSTEM S_LLITE

#include <obd_support.h>
#include <lustre_lite.h>
#include <lustre_dlm.h>
#include <lustre_ver.h>
#include <lustre_mdc.h>
#include <linux/lustre_acl.h>

#if 0
#ifndef POSIX_ACL_XATTR_ACCESS
#ifndef XATTR_NAME_ACL_ACCESS
#define XATTR_NAME_ACL_ACCESS   "system.posix_acl_access"
#endif
#define POSIX_ACL_XATTR_ACCESS XATTR_NAME_ACL_ACCESS
#endif
#ifndef POSIX_ACL_XATTR_DEFAULT
#ifndef XATTR_NAME_ACL_DEFAULT
#define XATTR_NAME_ACL_DEFAULT  "system.posix_acl_default"
#endif
#define POSIX_ACL_XATTR_DEFAULT XATTR_NAME_ACL_DEFAULT
#endif
#endif

#include "llite_internal.h"

#define XATTR_USER_PREFIX       "user."
#define XATTR_TRUSTED_PREFIX    "trusted."
#define XATTR_SECURITY_PREFIX   "security."

#define XATTR_USER_T            (1)
#define XATTR_TRUSTED_T         (2)
#define XATTR_SECURITY_T        (3)
#define XATTR_ACL_ACCESS_T      (4)
#define XATTR_ACL_DEFAULT_T     (5)
#define XATTR_OTHER_T           (6)

static
int get_xattr_type(const char *name)
{
        if (!strcmp(name, POSIX_ACL_XATTR_ACCESS))
                return XATTR_ACL_ACCESS_T;

        if (!strcmp(name, POSIX_ACL_XATTR_DEFAULT))
                return XATTR_ACL_DEFAULT_T;

        if (!strncmp(name, XATTR_USER_PREFIX,
                     sizeof(XATTR_USER_PREFIX) - 1))
                return XATTR_USER_T;

        if (!strncmp(name, XATTR_TRUSTED_PREFIX,
                     sizeof(XATTR_TRUSTED_PREFIX) - 1))
                return XATTR_TRUSTED_T;

        if (!strncmp(name, XATTR_SECURITY_PREFIX,
                     sizeof(XATTR_SECURITY_PREFIX) - 1))
                return XATTR_SECURITY_T;

        return XATTR_OTHER_T;
}

static
int xattr_type_filter(struct ll_sb_info *sbi, int xattr_type)
{
        if ((xattr_type == XATTR_ACL_ACCESS_T ||
             xattr_type == XATTR_ACL_DEFAULT_T) &&
            !(sbi->ll_flags & LL_SBI_ACL))
                return -EOPNOTSUPP;

        if (xattr_type == XATTR_USER_T && !(sbi->ll_flags & LL_SBI_USER_XATTR))
                return -EOPNOTSUPP;
        if (xattr_type == XATTR_TRUSTED_T && !capable(CAP_SYS_ADMIN))
                return -EPERM;
        if (xattr_type == XATTR_OTHER_T)
                return -EOPNOTSUPP;

        return 0;
}

static
int ll_setxattr_common(struct inode *inode, const char *name,
                       const void *value, size_t size,
                       int flags, __u64 valid)
{
        struct ll_sb_info *sbi = ll_i2sbi(inode);
        struct ptlrpc_request *req;
        int xattr_type, rc;
        struct obd_capa *oc;
        ENTRY;

        lprocfs_counter_incr(sbi->ll_stats, LPROC_LL_SETXATTR);

        xattr_type = get_xattr_type(name);
        rc = xattr_type_filter(sbi, xattr_type);
        if (rc)
                RETURN(rc);

        /* b10667: ignore lustre special xattr for now */
        if (xattr_type == XATTR_TRUSTED_T && strcmp(name, "trusted.lov") == 0)
                RETURN(0);

        oc = ll_i2mdscapa(inode);
        rc = md_setxattr(sbi->ll_md_exp, ll_inode2fid(inode), oc, valid, name,
                         value, size, 0, flags, &req);
        capa_put(oc);
        if (rc) {
                if (rc == -EOPNOTSUPP && xattr_type == XATTR_USER_T) {
                        LCONSOLE_INFO("Disabling user_xattr feature because "
                                      "it is not supported on the server\n"); 
                        sbi->ll_flags &= ~LL_SBI_USER_XATTR;
                }
                RETURN(rc);
        }

        ptlrpc_req_finished(req);
        RETURN(0);
}

int ll_setxattr(struct dentry *dentry, const char *name,
                const void *value, size_t size, int flags)
{
        struct inode *inode = dentry->d_inode;

        LASSERT(inode);
        LASSERT(name);

        CDEBUG(D_VFSTRACE, "VFS Op:inode=%lu/%u(%p), xattr %s\n",
               inode->i_ino, inode->i_generation, inode, name);

        return ll_setxattr_common(inode, name, value, size, flags,
                                  OBD_MD_FLXATTR);
}

int ll_removexattr(struct dentry *dentry, const char *name)
{
        struct inode *inode = dentry->d_inode;

        LASSERT(inode);
        LASSERT(name);

        CDEBUG(D_VFSTRACE, "VFS Op:inode=%lu/%u(%p), xattr %s\n",
               inode->i_ino, inode->i_generation, inode, name);

        return ll_setxattr_common(inode, name, NULL, 0, 0,
                                  OBD_MD_FLXATTRRM);
}

static
int ll_getxattr_common(struct inode *inode, const char *name,
                       void *buffer, size_t size, __u64 valid)
{
        struct ll_sb_info *sbi = ll_i2sbi(inode);
        struct ptlrpc_request *req = NULL;
        struct mdt_body *body;
        int xattr_type, rc;
        void *xdata;
        struct obd_capa *oc;
        ENTRY;

        CDEBUG(D_VFSTRACE, "VFS Op:inode=%lu/%u(%p)\n",
               inode->i_ino, inode->i_generation, inode);

        lprocfs_counter_incr(sbi->ll_stats, LPROC_LL_GETXATTR);

        /* listxattr have slightly different behavior from of ext3:
         * without 'user_xattr' ext3 will list all xattr names but
         * filtered out "^user..*"; we list them all for simplicity.
         */
        if (!name) {
                xattr_type = XATTR_OTHER_T;
                goto do_getxattr;
        }

        xattr_type = get_xattr_type(name);
        rc = xattr_type_filter(sbi, xattr_type);
        if (rc)
                RETURN(rc);

        /* posix acl is under protection of LOOKUP lock. when calling to this,
         * we just have path resolution to the target inode, so we have great
         * chance that cached ACL is uptodate.
         */
#ifdef CONFIG_FS_POSIX_ACL
        if (xattr_type == XATTR_ACL_ACCESS_T) {
                struct ll_inode_info *lli = ll_i2info(inode);
                struct posix_acl *acl;

                spin_lock(&lli->lli_lock);
                acl = posix_acl_dup(lli->lli_posix_acl);
                spin_unlock(&lli->lli_lock);

                if (!acl)
                        RETURN(-ENODATA);

                rc = posix_acl_to_xattr(acl, buffer, size);
                posix_acl_release(acl);
                RETURN(rc);
        }
#endif

do_getxattr:
        oc = ll_i2mdscapa(inode);
        rc = md_getxattr(sbi->ll_md_exp, ll_inode2fid(inode), oc, valid, name,
                         NULL, 0, size, 0, &req);
        capa_put(oc);
        if (rc) {
                if (rc == -EOPNOTSUPP && xattr_type == XATTR_USER_T) {
                        LCONSOLE_INFO("Disabling user_xattr feature because "
                                      "it is not supported on the server\n"); 
                        sbi->ll_flags &= ~LL_SBI_USER_XATTR;
                }
                RETURN(rc);
        }

        body = lustre_msg_buf(req->rq_repmsg, REPLY_REC_OFF, sizeof(*body));
        LASSERT(body);
        LASSERT_REPSWABBED(req, REPLY_REC_OFF);

        /* only detect the xattr size */
        if (size == 0)
                GOTO(out, rc = body->eadatasize);

        if (size < body->eadatasize) {
                CERROR("server bug: replied size %u > %u\n",
                       body->eadatasize, (int)size);
                GOTO(out, rc = -ERANGE);
        }

        if (lustre_msg_bufcount(req->rq_repmsg) < 3) {
                CERROR("reply bufcount %u\n",
                       lustre_msg_bufcount(req->rq_repmsg));
                GOTO(out, rc = -EFAULT);
        }

        /* do not need swab xattr data */
        LASSERT_REPSWAB(req, REPLY_REC_OFF + 1);
        xdata = lustre_msg_buf(req->rq_repmsg, REPLY_REC_OFF + 1,
                               body->eadatasize);
        if (!xdata) {
                CERROR("can't extract: %u : %u\n", body->eadatasize,
                       lustre_msg_buflen(req->rq_repmsg, REPLY_REC_OFF + 1));
                GOTO(out, rc = -EFAULT);
        }

        LASSERT(buffer);
        memcpy(buffer, xdata, body->eadatasize);
        rc = body->eadatasize;
out:
        ptlrpc_req_finished(req);
        RETURN(rc);
}

ssize_t ll_getxattr(struct dentry *dentry, const char *name,
                    void *buffer, size_t size)
{
        struct inode *inode = dentry->d_inode;

        LASSERT(inode);
        LASSERT(name);

        CDEBUG(D_VFSTRACE, "VFS Op:inode=%lu/%u(%p), xattr %s\n",
               inode->i_ino, inode->i_generation, inode, name);

        return ll_getxattr_common(inode, name, buffer, size, OBD_MD_FLXATTR);
}

ssize_t ll_listxattr(struct dentry *dentry, char *buffer, size_t size)
{
        struct inode *inode = dentry->d_inode;

        LASSERT(inode);

        CDEBUG(D_VFSTRACE, "VFS Op:inode=%lu/%u(%p)\n",
               inode->i_ino, inode->i_generation, inode);

        return ll_getxattr_common(inode, NULL, buffer, size, OBD_MD_FLXATTRLS);
}

