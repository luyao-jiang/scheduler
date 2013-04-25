/*
 *  Flash I/O scheduler for SSD.
 *
 *  Operating System Project
 */

// FIXME: some not need
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/blkdev.h>
#include <linux/elevator.h>
#include <linux/bio.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/compiler.h>
// #include <linux/rbtree.h>

#define DEBUG_FLASH 1

static const int sync_expire = HZ / 2;  /* expire time for read and sync write */
static const int async_expire = 5 * HZ; /* expire time for async writes, can be tuned */
static const int async_starved = 3;    /* max times sync reqs can starve async reqs */
static const int bundle_size = 10000; /* bundling size in bytes, corresponds to chunk_size*parallel_degree; 1MB*/

struct flash_data {
	/*
	 * run time data
	 */

	/*
	 * 3 Additional queues used to handle incoming reqs: sync, async and bundle, all are FIFO queues
	   0: for sync(read + sync writes); 1: for async
	 */
	struct list_head fifo_list[2];
	struct list_head bundle_list;

	unsigned int sync_issued;		/* times sync reqs have starved async writes */

	/*
	 * settings that change how the i/o scheduler behaves
	 */
	// int fifo_expire[2]; 0 for sync; 1 for async
	int fifo_expire[2];
	int async_starved;
	int bundle_size;	
};


/*
 * add rq to fifo
 */
static void
flash_add_request(struct request_queue *q, struct request *rq)
{
	struct flash_data *fd = q->elevator->elevator_data;
	// data_type: whether curr req is sync or async: 0 for sync; 1 for async;
	// const int data_type = !rq_is_sync(rq);
	// FIXME: OST cannot gen async writes: In order to trigger bundle, set sync only to contain read reqs for now;
	// READ 0; WRITE 1;
	const int data_type = rq_data_dir(rq);

	/*
	 * set expire time and add to associated fifo list
	 */
	rq_set_fifo_time(rq, jiffies + fd->fifo_expire[data_type]);
	list_add_tail(&rq->queuelist, &fd->fifo_list[data_type]);
	
	#ifdef DEBUG_FLASH
	if(data_type == 0)
		printk("sync req of size %d with beginning sector %zd and end sector %zd added\n", rq->__data_len, blk_rq_pos(rq), blk_rq_pos(rq)+blk_rq_sectors(rq));
	else if(data_type == 1)
		printk("async req of size %d with beginning sector %zd and end sector %zd added\n", rq->__data_len, blk_rq_pos(rq), blk_rq_pos(rq)+blk_rq_sectors(rq));
	else
		printk("unrecognize req type\n");
	#endif
}


/*  Implement elevator_merge_fn: do nothing for now
    Implement elevator_merged_fn: If larger than predefined bundle size, remove from async queue and add to tail of bundle queue; 
*/
static void flash_merged_request(struct request_queue *q,
				    struct request *req, int type)
{
	struct flash_data *fd = q->elevator->elevator_data;
	// FIXME:
	// const int data_type = !rq_is_sync(req);
	const int data_type = rq_data_dir(req);

	// BUG if req already in bundle queue
	/* FIXME: how to check if a req belong to certain queue? */

	// if req >= bundle_size, delete from async_fifo queue, add tail to bundle queue
	#ifdef DEBUG_FLASH
	printk("req is of size %d after merging\n", req->__data_len);
	#endif

	// only kick req into bundle queue if req is async
	if(req->__data_len >= fd->bundle_size && data_type == 1)
	{
		/* did both delete and init */
		rq_fifo_clear(req); 
		list_add_tail(&req->queuelist, &fd->bundle_list);
		#ifdef DEBUG_FLASH
		printk("req of type %d of size %d is inserted to bundle queue\n", data_type, req->__data_len);
		#endif
	}
}

/* 
   FIXME: if curr req being handled has been put into bundle queue, it cannot be used for merge any more
   Implement elevator_allow_merge_fn to control back merge in elevator level;
   if req already in bundle queue(req size >= bundle before merge, do not allow merge)
   sync req is allowed to merge, but will not be kicked into bundle queue
   if allow merge, return 1; else return 0;
*/
static int
flash_allow_merge(struct request_queue *q, struct request *req,
		  struct bio *bio)
{
	struct flash_data *fd = q->elevator->elevator_data;

	#ifdef DEBUG_FLASH
	printk("Allow merge func is called\n");
	#endif

	if(req->__data_len >= fd->bundle_size)
		return 0;
	
	#ifdef DEBEG_FLASH
	printk("req of size %d is allowed to merge\n", req->__data_len);
	#endif

	return 1;
	
}

/* 
   This function does 3 tasks:
   1 check if next expires before req, is so set expire time of req to be the expire time of next
   2 delete next from async fifo queue
   3 check if merged req size >= bundle_size; if so, delete req from async fifo queue, reinit and insert it to bundle queue
 */
static void
flash_merged_requests(struct request_queue *q, struct request *req,
			 struct request *next)
{
	struct flash_data *fd = q->elevator->elevator_data;
	// const int data_type = !rq_is_sync(req);
	// FIXME:
	const int data_type = rq_data_dir(req);

	/*
	 * if next expires before rq, assign its expire time to rq
	 * and move into next position (next will be deleted) in fifo
	 */
	// TODO: why need to check if async queue is empty here?
	if (!list_empty(&req->queuelist) && !list_empty(&next->queuelist)) {
		if (time_before(rq_fifo_time(next), rq_fifo_time(req))) {
			list_move(&req->queuelist, &next->queuelist);
			rq_set_fifo_time(req, rq_fifo_time(next));
		}
	}

	/* delete next */
	rq_fifo_clear(next);
	
	/* task 3 only kick into bundle queue if req is async */
	if(req->__data_len >= fd->bundle_size && data_type == 1)
	{
		/* did both delete and init */
		rq_fifo_clear(req); 
		list_add_tail(&req->queuelist, &fd->bundle_list);
		
		#ifdef DEBUG_FLASH
		printk("req of type %d of size %d is inserted to bundle queue\n", data_type, req->__data_len);
		#endif
	}

}

/*
 * move request from additional fifo list to dispatch queue.
 */
static inline void
flash_move_to_dispatch(struct flash_data *dd, struct request *rq)
{
	struct request_queue *q = rq->q;

	/* remove rq from its associated fifo queue and reinit */
	rq_fifo_clear(rq);
	elv_dispatch_add_tail(q, rq);
	#ifdef DEBUG_FLASH
	printk("req of size %d is moved to dispatch queue\n", rq->__data_len);
	#endif
}

/*
 * deadline_check_fifo returns 0 if there are no expired requests on the fifo,
 * 1 otherwise. Requires !list_empty(&fd->fifo_list[data_type])
 */
static inline int deadline_check_fifo(struct flash_data *fd, int ddir)
{
	struct request *rq;
	
	// if no req on given list, return 0: not expire;
	if(list_empty(&fd->fifo_list[ddir]))
		return 0;

	rq = rq_entry_fifo(fd->fifo_list[ddir].next);

	/*
	 * rq is expired!
	 */
	if (time_after(jiffies, rq_fifo_time(rq)))
		return 1;

	return 0;
}

/*
   Simple dispatching algorithm for now
   prioritize sync queue; 
   issue sync reqs for async_starved times;
   if no async write expires, issue bundled writes;
   if dispatched successfully, return 1; else return 0;
 */
static int flash_dispatch_requests(struct request_queue *q, int force)
{
	struct flash_data *fd = q->elevator->elevator_data;
	// syncs is 1 indicates sync queue is not empty;
	const int syncs = !list_empty(&fd->fifo_list[0]);
	const int asyncs = !list_empty(&fd->fifo_list[1]);
	const int bundles = !list_empty(&fd->bundle_list);
	struct request *rq;
//	int data_type;
	
	// sync queue not empty and sync req not starve async reqs
	if(syncs && (fd->sync_issued < fd->async_starved))
	{
		rq = rq_entry_fifo(fd->fifo_list[0].next);
		fd->sync_issued ++;
		goto dispatch_request;
	}

	// schedule bundle
	fd->sync_issued = 0;
	// if bundles not empty and no async writes expire
	if(bundles && !deadline_check_fifo(fd, 1))
	{
		rq = rq_entry_fifo(fd->bundle_list.next);
		goto dispatch_request;
	}

	// if async writes queue not empty and there is async writes expire
	// FIXME: This is like anticipatory algorithm, might cause system hang
	// if(asyncs && deadline_check_fifo(fd, 1))
	if(asyncs)
	{
		rq = rq_entry_fifo(fd->fifo_list[1].next);
		goto dispatch_request;
	}
	
	// dispatch not successful
	return 0;

dispatch_request:
	/*
	 * rq is the selected appropriate request. move rq to dispatch queue(request queue)
	 */
	flash_move_to_dispatch(fd, rq);

	return 1;
}

/*
  former and latter req: Implement interface to attemp front merge and back merge
  For simplicity, get former and back request in fifo queue for now
  FIXME: it makes more sense to get the next req in term of sector number
*/
static struct request *
flash_former_request(struct request_queue *q, struct request *rq)
{
	struct flash_data *fd = q->elevator->elevator_data;
	// const int data_type = !rq_is_sync(rq);
	// FIXME:
	const int data_type = rq_data_dir(rq);
	
	// bug if data_type not 1, FIXME: wrong, sync can also be merged
	// WARN_ON(data_type != 1);

	// if no former req
	if(rq->queuelist.prev == &fd->fifo_list[data_type])
		return NULL;
	return list_entry(rq->queuelist.prev, struct request, queuelist);
}

static struct request *
flash_latter_request(struct request_queue *q, struct request *rq)
{
	struct flash_data *fd = q->elevator->elevator_data;
	// const int data_type = !rq_is_sync(rq);
	// FIXME:
	const int data_type = rq_data_dir(rq);
	
	// bug if data_type not 1 FIXME: not right, sync can also be merged
	// WARN_ON(data_type != 1);

	// if no latter req
	if(rq->queuelist.next == &fd->fifo_list[data_type])
		return NULL;
	return list_entry(rq->queuelist.next, struct request, queuelist);
}

static int flash_queue_empty(struct request_queue *q)
{
	struct flash_data *fd = q->elevator->elevator_data;

	return list_empty(&fd->fifo_list[0])
		&& list_empty(&fd->fifo_list[1])
		&& list_empty(&fd->bundle_list);
}

static void flash_exit_queue(struct elevator_queue *e)
{
	struct flash_data *fd = e->elevator_data;

	BUG_ON(!list_empty(&fd->fifo_list[0]));
	BUG_ON(!list_empty(&fd->fifo_list[1]));
	BUG_ON(!list_empty(&fd->bundle_list));

	kfree(fd);
}

/*
 * initialize elevator private data (flash_data).
 */
static void *flash_init_queue(struct request_queue *q)
{
	struct flash_data *fd;

	fd = kmalloc_node(sizeof(*fd), GFP_KERNEL | __GFP_ZERO, q->node);
	if (!fd)
		return NULL;

	INIT_LIST_HEAD(&fd->fifo_list[0]);
	INIT_LIST_HEAD(&fd->fifo_list[1]);
	INIT_LIST_HEAD(&fd->bundle_list);
	fd->fifo_expire[0] = sync_expire;
	fd->fifo_expire[1] = async_expire;
	fd->async_starved = async_starved;
	fd->sync_issued = 0;
	fd->bundle_size = bundle_size;
	return fd;
}

/*
 * sysfs parts below
   FIXME: ignore sysfs parts for now; need for param tuning later
 */

static struct elevator_type iosched_flash = {
	.ops = {
		.elevator_merged_fn =		flash_merged_request,
		.elevator_merge_req_fn =	flash_merged_requests,
		.elevator_dispatch_fn =		flash_dispatch_requests,
		.elevator_add_req_fn =		flash_add_request,
		.elevator_queue_empty_fn =	flash_queue_empty,
		.elevator_former_req_fn =	flash_former_request,
		.elevator_latter_req_fn =	flash_latter_request,
		.elevator_allow_merge_fn = 	flash_allow_merge,
		.elevator_init_fn =		flash_init_queue,
		.elevator_exit_fn =		flash_exit_queue,
	},

//	.elevator_attrs = deadline_attrs,
	.elevator_name = "flash",
	.elevator_owner = THIS_MODULE,
};

static int __init flash_init(void)
{
	elv_register(&iosched_flash);

	return 0;
}

static void __exit flash_exit(void)
{
	elv_unregister(&iosched_flash);
}

module_init(flash_init);
module_exit(flash_exit);

MODULE_AUTHOR("KJ");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("flash IO scheduler");
