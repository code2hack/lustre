/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 *  Copyright (C) 2001 Cluster File Systems, Inc. <braam@clusterfs.com>
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
 * Basic Lustre library routines. 
 *
 */

#ifndef _LUSTRE_LIB_H
#define _LUSTRE_LIB_H

#include <asm/types.h>

#ifndef __KERNEL__
# include <string.h>
#endif

#ifdef __KERNEL__
/* page.c */
inline void lustre_put_page(struct page *page);
struct page *lustre_get_page_read(struct inode *dir, unsigned long index);
struct page *lustre_get_page_write(struct inode *dir, unsigned long index);
int lustre_commit_page(struct page *page, unsigned from, unsigned to);

/* simple.c */
struct obd_run_ctxt;
void push_ctxt(struct obd_run_ctxt *save, struct obd_run_ctxt *new);
void pop_ctxt(struct obd_run_ctxt *saved);
int simple_mkdir(struct dentry *dir, char *name, int mode);
int lustre_fread(struct file *file, char *str, int len, loff_t *off);
int lustre_fwrite(struct file *file, const char *str, int len, loff_t *off);
int lustre_fsync(struct file *file);

#define ASSERT_MAX_SIZE_MB 50000ULL
#define ASSERT_PAGE_INDEX(index, OP)                                    \
do { if (index > ASSERT_MAX_SIZE_MB << (20 - PAGE_SHIFT)) {             \
        CERROR("bad page index %lu > %Lu\n", index,                     \
               ASSERT_MAX_SIZE_MB << (20 - PAGE_SHIFT));                \
        portal_debug = ~0UL;                                            \
        OP;                                                             \
}} while(0)

#define ASSERT_FILE_OFFSET(offset, OP)                                  \
do { if (offset > ASSERT_MAX_SIZE_MB << 20) {                           \
        CERROR("bad file offset %Lu > %Lu\n", offset,                   \
               ASSERT_MAX_SIZE_MB << 20);                               \
        portal_debug = ~0UL;                                            \
        OP;                                                             \
}} while(0)

static inline void ll_sleep(int t)
{
        set_current_state(TASK_INTERRUPTIBLE);
        schedule_timeout(t * HZ);
        set_current_state(TASK_RUNNING);
}
#endif

#include <linux/portals_lib.h>

#endif /* _LUSTRE_LIB_H */
