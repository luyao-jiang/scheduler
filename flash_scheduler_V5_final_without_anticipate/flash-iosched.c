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
#include <linux/rbtree.h>

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
	/* rb tree to enhance merging */
	struct rb_root sort_list[2];

	unsigned int sync_issued;		/* times sync reqs have starved async writes */

	/*
	 * settings that change how the i/o scheduler behaves
	 */
	// int fifo_expire[2]; 0 for sync; 1 for async
	int fifo_expire[2];
	int async_starved;
	int bundle_size;	
};

static inline void
flash_move_to_dispatch(struct flash_data *fd, struct request *rq);

static inline struct rb_root *
flash_rb_root(struct flash_data *fd, struct request *rq)
{
	// FIXME: data_type 
	const int data_type = !rq_is_sync(rq);
	// int data_type = rq_data_dir(rq);
	return &fd->sort_list[data_type];
}


static void
flash_add_rq_rb(struct flash_data *fd, struct request *rq)
{
	struct rb_root *root = flash_rb_root(fd, rq);
	struct request *__alias;
	
	/* FIXME: in which case would an incoming req already exists in elevator queue? Issue it right away */
	while (unlikely(__alias = elv_rb_add(root, rq)))
		flash_move_to_dispatch(fd, __alias);
}

/*
 * add rq to fifo
 */
static void
flash_add_request(struct request_queue *q, struct request *rq)
{
	struct flash_data *fd = q->elevator->elevator_data;
	// data_type: whether curr req is sync or async: 0 for sync; 1 for async;
	const int data_type = !rq_is_sync(rq);
	// FIXME: OST cannot gen async writes: In order to trigger bundle, set sync only to contain read reqs for now;
	// READ 0; WRITE 1;
	// const int data_type = rq_data_dir(rq);
	
	/* insert req into rbtree */
	flash_add_rq_rb(fd, rq);

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
	const int data_type = !rq_is_sync(req);
	// const int data_type = rq_data_dir(req);

	// front merge is disabled in flash scheduler, however is still in use in merge cache;
	// if merge type is front merge, need to reposition the req in rbtree
	if (type == ELEVATOR_FRONT_MERGE) {
		elv_rb_del(flash_rb_root(fd, req), req);
		flash_add_rq_rb(fd, req);
	}

	// BUG if req already in bundle queue
	/* FIXME: how to check if a req belong to certain queue? */

	// if req >= bundle_size, delete from async_fifo queue, add tail to bundle queue
	#ifdef DEBUG_FLASH
	printk("req is of size %d after merging\n", req->__data_len);
	#endif

	// only kick req into bundle queue if req is async
	// FIXME: remove req > bundle_size from rbtree in case that other async req attemps to merge with it
	// assume req in bundle queue is not eligible for merging or being merged any more
	if(req->__data_len >= fd->bundle_size && data_type == 1)
	{
		/* did both delete and init */
		elv_rb_del(flash_rb_root(fd, req), req);
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
	printk("Curr req has sector %d phys_segs %d bio has sector %d phys_segs %d\n", blk_rq_sectors(req), bio_sectors(bio), req->nr_phys_segments, bio_phys_segments(q, bio));
	printk("Curr queue has max_sector_limit %d max_hw_sector_limit %d max_hw_segs_limit %d max_phys_segs_limit %d\n", queue_max_sectors(q), queue_max_hw_sectors(q), queue_max_hw_segments(q), queue_max_phys_segments(q));
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
	const int data_type = !rq_is_sync(req);
	// FIXME:
	// const int data_type = rq_data_dir(req);

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
	/* delete from both sort list and fifo list */
	elv_rb_del(flash_rb_root(fd, next), next);
	rq_fifo_clear(next);
	
	/* task 3 only kick into bundle queue if req is async */
	if(req->__data_len >= fd->bundle_size && data_type == 1)
	{
		/* delete from respective sort list */
		elv_rb_del(flash_rb_root(fd, req), req);
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
flash_move_to_dispatch(struct flash_data *fd, struct request *rq)
{
	struct request_queue *q = rq->q;
	struct request *__alias = NULL;

	/* delete the req from rbtree if it's still in the tree(might be deleted already when inserting to bundle queue) */
	if((__alias = elv_rb_find(flash_rb_root(fd, rq), blk_rq_pos(rq))) != NULL)
	{
		// if still in rbtree
		WARN_ON(__alias != rq);
		#ifdef DEBUG_FLASH
		if(__alias != rq)
			printk("alias should be equal to original req!\n");
		#endif
		
		elv_rb_del(flash_rb_root(fd, rq), rq);
	}
	

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
/* FIXME: rbtree used */
/*
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
*/

/*
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
*/

static int flash_queue_empty(struct request_queue *q)
{
	struct flash_data *fd = q->elevator->elevator_data;

	return list_empty(&fd->fifo_list[0])
		&& list_empty(&fd->fifo_list[1])
		&& list_empty(&fd->bundle_list);
	//	&& list_empty(&fd->sort_list[0])
	//	&& list_empty(&fd->sort_list[1]);
}

static void flash_exit_queue(struct elevator_queue *e)
{
	struct flash_data *fd = e->elevator_data;

	BUG_ON(!list_empty(&fd->fifo_list[0]));
	BUG_ON(!list_empty(&fd->fifo_list[1]));
	BUG_ON(!list_empty(&fd->bundle_list));
//	BUG_ON(!list_empty(&fd->sort_list[0]));
//	BUG_ON(!list_empty(&fd->sort_list[1]));

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
	fd->sort_list[0] = RB_ROOT;
	fd->sort_list[1] = RB_ROOT;
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
 */
 static ssize_t
flash_var_show(int var, char *page)
{
	return sprintf(page, "%d\n", var);
}

static ssize_t
flash_var_store(int *var, const char *page, size_t count)
{
	char *p = (char *) page;

	*var = simple_strtol(p, &p, 10);
	return count;
}

#define SHOW_FUNCTION(__FUNC, __VAR, __CONV)				\
static ssize_t __FUNC(struct elevator_queue *e, char *page)		\
{									\
	struct flash_data *fd = e->elevator_data;			\
	int __data = __VAR;						\
	if (__CONV)							\
		__data = jiffies_to_msecs(__data);			\
	return flash_var_show(__data, (page));			\
}

SHOW_FUNCTION(flash_sync_expire_show, fd->fifo_expire[0], 1);
SHOW_FUNCTION(flash_async_expire_show, fd->fifo_expire[1], 1);
SHOW_FUNCTION(flash_async_starved_show, fd->async_starved, 0);
SHOW_FUNCTION(flash_bundle_size_show, fd->bundle_size, 0);
#undef SHOW_FUNCTION

#define STORE_FUNCTION(__FUNC, __PTR, MIN, MAX, __CONV)			\
static ssize_t __FUNC(struct elevator_queue *e, const char *page, size_t count)	\
{									\
	struct flash_data *fd = e->elevator_data;			\
	int __data;							\
	int ret = flash_var_store(&__data, (page), count);		\
	if (__data < (MIN))						\
		__data = (MIN);						\
	else if (__data > (MAX))					\
		__data = (MAX);						\
	if (__CONV)							\
		*(__PTR) = msecs_to_jiffies(__data);			\
	else								\
		*(__PTR) = __data;					\
	return ret;							\
}

STORE_FUNCTION(flash_sync_expire_store, &fd->fifo_expire[0], 0, INT_MAX, 1);
STORE_FUNCTION(flash_async_expire_store, &fd->fifo_expire[1], 0, INT_MAX, 1);
STORE_FUNCTION(flash_async_starved_store, &fd->async_starved, INT_MIN, INT_MAX, 0);
STORE_FUNCTION(flash_bundle_size_store, &fd->bundle_size, 0, INT_MAX, 0);
#undef STORE_FUNCTION

#define DD_ATTR(name) \
	__ATTR(name, S_IRUGO|S_IWUSR, flash_##name##_show, \
				      flash_##name##_store)

static struct elv_fs_entry flash_attrs[] = {
	DD_ATTR(sync_expire),
	DD_ATTR(async_expire),
	DD_ATTR(async_starved),
	DD_ATTR(bundle_size),
	__ATTR_NULL
};

static struct elevator_type iosched_flash = {
	.ops = {
		.elevator_merged_fn =		flash_merged_request,
		.elevator_merge_req_fn =	flash_merged_requests,
		.elevator_dispatch_fn =		flash_dispatch_requests,
		.elevator_add_req_fn =		flash_add_request,
		.elevator_queue_empty_fn =	flash_queue_empty,
	//	.elevator_former_req_fn =	flash_former_request,
	//	.elevator_latter_req_fn =	flash_latter_request,
		.elevator_former_req_fn =	elv_rb_former_request,
		.elevator_latter_req_fn =	elv_rb_latter_request,
		.elevator_allow_merge_fn = 	flash_allow_merge,
		.elevator_init_fn =		flash_init_queue,
		.elevator_exit_fn =		flash_exit_queue,
	},

	.elevator_attrs = flash_attrs,
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
