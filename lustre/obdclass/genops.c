/*
 *  linux/fs/ext2_obd/sim_obd.c
 *
 * These are the only exported functions; they provide the simulated object-
 * oriented disk.
 *
 */

#include <asm/uaccess.h>
#include <linux/sched.h>
#include <linux/stat.h>
#include <linux/string.h>
#include <linux/locks.h>
#include <linux/quotaops.h>
#include <linux/list.h>
#include <linux/file.h>
#include <linux/iobuf.h>
#include <asm/bitops.h>
#include <asm/byteorder.h>
#include <linux/obd_support.h>
#include <linux/obd_class.h>


extern struct obd_device obd_dev[MAX_OBD_DEVICES];

/* map connection to client */
struct obd_client *gen_client(int cli_id)
{
	struct obd_device * obddev;
	struct list_head * lh, * next;
	struct obd_client * cli;
	int a;

	for (a = 0; a < MAX_OBD_DEVICES; a++) {
		obddev = &obd_dev[a];

		lh = next = &obddev->obd_gen_clients;
		while ((lh = lh->next) != &obddev->obd_gen_clients) {
			cli = list_entry(lh, struct obd_client, cli_chain);
			
			if (cli->cli_id == cli_id)
				return cli;
		}
	}

	return NULL;
} /* obd_client */



/* a connection defines a context in which preallocation can be managed. */ 
int gen_connect (struct obd_device *obddev, 
			struct obd_conn_info * conninfo)
{
	struct obd_client * cli;

	OBD_ALLOC(cli, struct obd_client *, sizeof(struct obd_client));
	if ( !cli ) {
		printk("obd_connect (minor %d): no memory!\n", 
		       obddev->obd_minor);
		return -ENOMEM;
	}

	INIT_LIST_HEAD(&cli->cli_prealloc_inodes);
	/* this should probably spinlocked? */
	cli->cli_id = ++obddev->obd_gen_last_id;
	cli->cli_prealloc_quota = 0;
	cli->cli_obd = obddev;
	list_add(&(cli->cli_chain), obddev->obd_gen_clients.prev);

	CDEBUG(D_IOCTL, "connect: new ID %u\n", cli->cli_id);
	conninfo->conn_id = cli->cli_id;
	return 0;
} /* gen_obd_connect */


int gen_disconnect(unsigned int conn_id)
{
	struct obd_client * cli;

	ENTRY;

	if (!(cli = gen_client(conn_id))) {
		CDEBUG(D_IOCTL, "disconnect: attempting to free "
		       "nonexistent client %u\n", conn_id);
		return -EINVAL;
	}

	list_del(&(cli->cli_chain));
	OBD_FREE(cli, sizeof(struct obd_client));

	CDEBUG(D_IOCTL, "disconnect: ID %u\n", conn_id);

	EXIT;
	return 0;
} /* gen_obd_disconnect */


/* 
 *   raid1 defines a number of connections to child devices,
 *   used to make calls to these devices.
 *   data holds nothing
 */ 
int gen_multi_setup(struct obd_device *obddev, int len, void *data)
{
	int i;
	struct obd_device *rdev = obddev->obd_multi_dev[0];

	for (i = 0 ; i < obddev->obd_multi_count ; i++ ) {
		int rc;
		struct obd_device *child = rdev + i;
		rc  = OBP(child, connect)(child, &rdev->obd_multi_conns[i]);

		if ( rc != 0 ) {
			/* XXX disconnect others */
			return -EINVAL;
		}
	}		
	return 0;
}

int gen_multi_cleanup(struct obd_device * obddev)
{
	int i;
	struct obd_device **rdev = obddev->obd_multi_dev;

	for (i = 0 ; i < obddev->obd_multi_count ; i++ ) {
		int rc;
		struct obd_device *child = *(rdev + i);
		rc  = OBP(child, cleanup)(child);
		*(rdev + i) = NULL;

		if ( rc != 0 ) {
			/* XXX disconnect others */
			return -EINVAL;
		}
	}
	gen_cleanup(obddev);
	return 0;
} /* sim_cleanup_obddev */


int gen_multi_attach(struct obd_device *obddev, int len, void *data)
{
	int i;
	int count;
	struct obd_device *rdev = obddev->obd_multi_dev[0];

	count = len/sizeof(int);
	obddev->obd_multi_count = count;
	for (i=0 ; i<count ; i++) {
		rdev = &obd_dev[*((int *)data + i)];
		rdev = rdev + 1;
		CDEBUG(D_IOCTL, "OBD RAID1: replicator %d is of type %s\n", i,
		       (rdev + i)->obd_type->typ_name);
	}
	return 0;
}



/*
 *    remove all connections to this device
 *    close all connections to lower devices
 *    needed for forced unloads of OBD client drivers
 */
int gen_multi_cleanup_device(struct obd_device *obddev)
{
	int i;
	struct obd_device **rdev;

	rdev =  obddev->obd_multi_dev;
	for (i = 0 ; i < obddev->obd_multi_count ; i++ ) {
		int rc;
		struct obd_device *child = *(rdev + i);
		rc  = OBP(child, disconnect)
			(obddev->obd_multi_conns[i].conn_id);

		if ( rc != 0 ) {
			printk("OBD multi cleanup dev: disconnect failure %d\n", child->obd_minor);
		}
		*(rdev + i) = NULL;
	}		
	return 0;
} /* gen_multi_cleanup_device */


/*
 *    forced cleanup of the device:
 *    - remove connections from the device
 *    - cleanup the device afterwards
 */
int gen_cleanup(struct obd_device * obddev)
{
	struct list_head * lh, * tmp;
	struct obd_client * cli;

	ENTRY;

	lh = tmp = &obddev->obd_gen_clients;
	while ((tmp = tmp->next) != lh) {
		cli = list_entry(tmp, struct obd_client, cli_chain);
		CDEBUG(D_IOCTL, "Disconnecting obd_connection %d, at %p\n",
		       cli->cli_id, cli);
		OBP(obddev, disconnect)(cli->cli_id);
	}

	return OBP(obddev, cleanup_device)(obddev);
} /* sim_cleanup_device */

/* 
 * Wait for a page to get unlocked.
 *
 * This must be called with the caller "holding" the page,
 * ie with increased "page->count" so that the page won't
 * go away during the wait..
 */
static void my__wait_on_page(struct page *page)
{
	struct task_struct *tsk = current;
	DECLARE_WAITQUEUE(wait, tsk);

	add_wait_queue(&page->wait, &wait);
	do {
		run_task_queue(&tq_disk);
		set_task_state(tsk, TASK_UNINTERRUPTIBLE);
		if (!PageLocked(page))
			break;
		schedule();
	} while (PageLocked(page));
	tsk->state = TASK_RUNNING;
	remove_wait_queue(&page->wait, &wait);
}

/*
 * Get an exclusive lock on the page..
 */
static void lck_page(struct page *page)
{
	while (TryLockPage(page))
		my__wait_on_page(page);
}

/* obdo's must be correctly filled in by caller! */
int gen_copy_data(struct obd_device *obddev, int conn_id, 
		       obdattr *source, obdattr *target)
{
	struct page *page;
<<<<<<< genops.c
	int res;
	int err;
	int i;
=======
	unsigned long index = 0;
	int rc;
>>>>>>> 1.5

<<<<<<< genops.c
	page = __get_pages(GFP_USER, 0);
	if (!page) 
		return EIO;
	lck_page(page);
=======
	page = alloc_page(GFP_USER);
	if ( !page ) 
		return -ENOMEM;
>>>>>>> 1.5

	err = 0;
	i = 0;
	while (1) {
		CDEBUG(D_IOCTL, "i %d, size %ld\n", i, source->i_size);
		err = 0;
		if ( i >= (source->i_size >> PAGE_SHIFT) ) 
			break;
	
<<<<<<< genops.c
		page->offset = i << PAGE_SHIFT;
		err = -EIO;
		CDEBUG(D_IOCTL, "i %d, offset %ld, size %ld\n", i, 
		       page->offset, source->i_size);
		res = OBP(obddev, brw)(READ, conn_id, source, page, 0);
		CDEBUG(D_IOCTL, "Result reading %d\n", res);
		if ( res < 0 ) 
			break;
=======
	lck_page(page);
	
	while (index < ((src->i_size + PAGE_SIZE - 1) >> PAGE_SHIFT)) {
		
		page->index = index;
		rc = OBP(conn->oc_dev, brw)(READ, conn, src, page, 0);
>>>>>>> 1.5

		res = OBP(obddev, brw)(WRITE, conn_id, target, page, 1);
		CDEBUG(D_IOCTL, "Result writing %d\n", res);
		if ( res < 0 ) 
			break;

<<<<<<< genops.c
		i++;
=======
		rc = OBP(conn->oc_dev,brw)(WRITE, conn, tgt, page, 1);
		if ( rc != PAGE_SIZE)
			break;
		
		index ++;
>>>>>>> 1.5
	}
	UnlockPage(page);
	__free_page(page);

	target->i_size = source->i_size;
	OBP(obddev, setattr)(conn_id, target);

	return err;
}
