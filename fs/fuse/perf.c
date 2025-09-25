// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2024 Yoonho Shin <yoonho.shin@samsung.com>
 */

#include "fuse_i.h"

#include <uapi/linux/fuse.h>
#include <linux/proc_fs.h>

/* The opcode must be smaller than 64 to be stored in a single struct xa_node */
#define FUSE_PERF_FIELD_OPCODE GENMASK(5, 0)
#define FUSE_PERF_FIELD_UID GENMASK(63, 6)

/*
 * XArray can store intergers between 0 and LONG_MAX. So, fuse perf is turned on
 * only when BITS_PER_LONG is 64.
 */
#define FUSE_PERF_FILED_CNT GENMASK(15, 0)
#define FUSE_PERF_FILED_SUM GENMASK(47, 16)
#define FUSE_PERF_FILED_WORST GENMASK(62, 48)

#define FUSE_PERF_CNT_MAX ((unsigned long)U16_MAX)
#define FUSE_PERF_SUM_MAX ((unsigned long)U32_MAX)
#define FUSE_PERF_WORST_MAX ((unsigned long)S16_MAX)

#define FUSE_PERF_MAX_OPCODE 62
#define FUSE_PERF_OPCODE_CANONICAL_PATH 63

struct fuse_perf_xa {
	struct xarray xa;
	refcount_t refcount;
};

static void __fuse_perf_xa_init(struct fuse_perf_xa *perf_xa)
{
	xa_init(&perf_xa->xa);
	refcount_set(&perf_xa->refcount, 1);
}

static void __fuse_perf_xa_destroy(struct fuse_perf_xa *perf_xa)
{
	if (!perf_xa)
		return;

	xa_destroy(&perf_xa->xa);
	kfree(perf_xa);
}

struct fuse_perf_struct {
	/*
	 * index: 58 bits uid, 6 bits opcode
	 * entry: 15 bits worst (ms), 32 bits sum (ms), 16 bits cnt
	 */
	struct fuse_perf_xa *perf_xa;

	/* To maintain reference through multiple read syscalls*/
	struct fuse_perf_xa *perf_xa_to_read;

	/* Used in proc */
	bool is_eof;

	/* /proc/fuse-perf/<dev id> */
	struct proc_dir_entry *proc_entry;

	/* Protect proc_entry */
	struct mutex lock;

	/* Used when accessing perf_xa */
	spinlock_t spinlock;

	atomic_long_t last_read;
};

/**************************************
 * etc
 **************************************/

void fuse_perf_check_last_read(struct fuse_conn *fc)
{
	long last_read;
	struct fuse_perf_struct *perf_struct = fc->perf_struct;
	struct fuse_perf_xa *perf_xa = NULL;
	struct fuse_perf_xa *perf_xa_to_read = NULL;

	if (!perf_struct)
		return;

	last_read = atomic_long_read(&perf_struct->last_read);
	if (time_is_after_jiffies(last_read + msecs_to_jiffies(60000)))
		return;

	spin_lock(&perf_struct->spinlock);
	perf_xa = xchg(&perf_struct->perf_xa, NULL);
	spin_unlock(&perf_struct->spinlock);

	if (mutex_trylock(&perf_struct->lock)) {
		perf_xa_to_read = xchg(&perf_struct->perf_xa_to_read, NULL);
		mutex_unlock(&perf_struct->lock);
	}

	if (perf_xa && refcount_dec_and_test(&perf_xa->refcount))
		__fuse_perf_xa_destroy(perf_xa);
	if (perf_xa_to_read && refcount_dec_and_test(&perf_xa_to_read->refcount))
		__fuse_perf_xa_destroy(perf_xa_to_read);
}

/**************************************
 * proc
 **************************************/

static struct proc_dir_entry *fuse_perf_proc_dir;

int fuse_perf_proc_init(void)
{
	fuse_perf_proc_dir = proc_mkdir("fuse_perf", NULL);
	if (!fuse_perf_proc_dir)
		return 1;
	return 0;
}

void fuse_perf_proc_cleanup(void)
{
	proc_remove(fuse_perf_proc_dir);
}

static void *fuse_perf_seq_start(struct seq_file *s, loff_t *pos)
{
	struct xa_state *xas;
	struct inode *inode = file_inode(s->file);
	struct fuse_conn *fc = pde_data(inode);
	struct fuse_perf_struct *perf_struct = fc->perf_struct;

	if (!perf_struct)
		return NULL;

	mutex_lock(&perf_struct->lock);

	if (*pos == ULONG_MAX)
		return NULL;

	xas = kzalloc(sizeof(struct xa_state), GFP_KERNEL);
	if (!xas)
		return NULL;
	s->private = xas;

	if (*pos == 0 && !perf_struct->perf_xa_to_read) {
		struct fuse_perf_xa *new;

		new = kzalloc(sizeof(struct fuse_perf_xa), GFP_KERNEL);
		if (new)
			__fuse_perf_xa_init(new);

		/* just store NULL even if memory allocation fails */
		spin_lock(&perf_struct->spinlock);
		xchg(&perf_struct->perf_xa_to_read, perf_struct->perf_xa);
		xchg(&perf_struct->perf_xa, new);
		spin_unlock(&perf_struct->spinlock);

		perf_struct->is_eof = false;
		atomic_long_set(&perf_struct->last_read, jiffies);
	}
	if (!perf_struct->perf_xa_to_read)
		return NULL;

	xas->xa = &perf_struct->perf_xa_to_read->xa;
	xas_set(xas, *pos);

	/*
	 * Because of start() -> show() sequence, next() must be performed to
	 * go to the index of first entry.
	 */
	if (!xas_find(xas, ULONG_MAX)) {
		*pos = ULONG_MAX;
		perf_struct->is_eof = true;
		return NULL;
	}
	*pos = xas->xa_index;

	return xas;
}

static void *fuse_perf_seq_next(struct seq_file *s, void *v, loff_t *pos)
{
	struct inode *inode = file_inode(s->file);
	struct fuse_conn *fc = pde_data(inode);
	struct fuse_perf_struct *perf_struct = fc->perf_struct;
	struct xa_state *xas = s->private;

	if (!xas_find(xas, ULONG_MAX)) {
		*pos = ULONG_MAX;
		perf_struct->is_eof = true;
		return NULL;
	}
	*pos = xas->xa_index;

	return xas;
}

static void fuse_perf_seq_stop(struct seq_file *s, void *v)
{
	struct xa_state *xas;
	struct inode *inode = file_inode(s->file);
	struct fuse_conn *fc = pde_data(inode);
	struct fuse_perf_struct *perf_struct = fc->perf_struct;

	if (!perf_struct)
		return;

	if (perf_struct->is_eof) {
		struct fuse_perf_xa *temp;

		temp = xchg(&perf_struct->perf_xa_to_read, NULL);
		if (refcount_dec_and_test(&temp->refcount))
			__fuse_perf_xa_destroy(temp);
		perf_struct->is_eof = false;
	}

	xas = xchg(&s->private, NULL);
	if (xas)
		kfree(xas);

	mutex_unlock(&perf_struct->lock);
}

static int fuse_perf_seq_show(struct seq_file *s, void *v)
{
	struct xa_state *xas = s->private;
	void *entryp;
	unsigned long val;
	uint32_t uid, opcode;
	unsigned long sum, worst, cnt;

	entryp = xas->xa_node->slots[xas->xa_offset];
	val = xa_to_value(entryp);

	uid = FIELD_GET(FUSE_PERF_FIELD_UID, xas->xa_index);
	opcode = FIELD_GET(FUSE_PERF_FIELD_OPCODE, xas->xa_index);

	sum = FIELD_GET(FUSE_PERF_FILED_SUM, val);
	cnt = FIELD_GET(FUSE_PERF_FILED_CNT, val);
	worst = FIELD_GET(FUSE_PERF_FILED_WORST, val);

	seq_printf(s, "uid: %u, opcode: %u, sum: %lu, cnt: %lu, worst: %lu\n",
			uid,
			opcode,
			sum,
			cnt,
			worst);
	return 0;
}

static const struct seq_operations fuse_perf_seq_ops = {
	.start = fuse_perf_seq_start,
	.next = fuse_perf_seq_next,
	.stop = fuse_perf_seq_stop,
	.show = fuse_perf_seq_show
};

/**************************************
 * main
 **************************************/

static bool __fuse_perf_op(uint32_t opcode)
{
	if (opcode > FUSE_PERF_MAX_OPCODE && opcode != FUSE_CANONICAL_PATH)
		return false;
	return true;
}

/*
 * return: 0 if now allowed
 */
static unsigned long __fuse_perf_make_index(struct fuse_req *req)
{
	unsigned long index;
	uint32_t uid = req->in.h.uid;
	uint32_t opcode = req->in.h.opcode;

	if (!__fuse_perf_op(opcode))
		return 0;
	if (opcode == FUSE_CANONICAL_PATH)
		opcode = FUSE_PERF_OPCODE_CANONICAL_PATH;

	index = FIELD_PREP(FUSE_PERF_FIELD_UID, uid) |
		FIELD_PREP(FUSE_PERF_FIELD_OPCODE, opcode);
	return index;
}

void fuse_perf_start_hook(struct fuse_req *req)
{
	struct fuse_perf_struct *perf_struct = req->fm->fc->perf_struct;
	uint32_t opcode = req->in.h.opcode;

	if (!perf_struct)
		return;
	if (!__fuse_perf_op(opcode))
		return;

	req->dispatch_time = ktime_get();
}

static void __fuse_perf_update_data(struct fuse_perf_xa *perf_xa,
		unsigned long index,
		unsigned long duration)
{
	struct xarray *xa = &perf_xa->xa;
	XA_STATE(xas, xa, index);
	void *old, *new;
	unsigned long val, worst, sum, cnt;

	xas_lock(&xas);

	old = xas_load(&xas);
	if (!old)
		old = xa_mk_value(0);

	val = xa_to_value(old);

	worst = FIELD_GET(FUSE_PERF_FILED_WORST, val);
	worst = clamp(duration, worst, FUSE_PERF_WORST_MAX);

	sum = FIELD_GET(FUSE_PERF_FILED_SUM, val);
	sum = min(sum + duration, FUSE_PERF_SUM_MAX);

	cnt = FIELD_GET(FUSE_PERF_FILED_CNT, val);
	cnt = min(cnt + 1, FUSE_PERF_CNT_MAX);

	val = FIELD_PREP(FUSE_PERF_FILED_WORST, worst) |
		FIELD_PREP(FUSE_PERF_FILED_SUM, sum) |
		FIELD_PREP(FUSE_PERF_FILED_CNT, cnt);

	new = xa_mk_value(val);

	/* Don't retry because it's not important enough */
	xas_store(&xas, new);

	xas_unlock(&xas);
}

void fuse_perf_end_hook(struct fuse_req *req)
{
	struct fuse_perf_struct *perf_struct = req->fm->fc->perf_struct;
	struct fuse_perf_xa *perf_xa;
	unsigned long index;
	unsigned long duration;

	if (!perf_struct)
		return;

	index = __fuse_perf_make_index(req);
	if (index == 0)
		return;

	spin_lock(&perf_struct->spinlock);
	perf_xa = perf_struct->perf_xa;
	if (!perf_xa) {
		spin_unlock(&perf_struct->spinlock);
		return;
	}
	refcount_inc(&perf_xa->refcount);
	spin_unlock(&perf_struct->spinlock);

	duration = (unsigned long)(ktime_get() - req->dispatch_time) / 1000000;
	__fuse_perf_update_data(perf_xa, index, duration);

	if (refcount_dec_and_test(&perf_xa->refcount))
		__fuse_perf_xa_destroy(perf_xa);
}

void fuse_perf_init(struct fuse_conn *fc)
{
	struct fuse_perf_struct *perf_struct;

	if (!fc->perf_node_name || !fuse_perf_proc_dir)
		return;

	perf_struct = kzalloc(sizeof(struct fuse_perf_struct), GFP_KERNEL);
	if (!perf_struct)
		return;

	mutex_init(&perf_struct->lock);
	spin_lock_init(&perf_struct->spinlock);

	perf_struct->proc_entry = proc_create_seq_data(fc->perf_node_name,
			0,
			fuse_perf_proc_dir,
			&fuse_perf_seq_ops,
			fc);
	if (!perf_struct->proc_entry)
		goto free_perf_struct;

	perf_struct->perf_xa = kzalloc(sizeof(struct fuse_perf_xa), GFP_KERNEL);
	if (perf_struct->perf_xa)
		__fuse_perf_xa_init(perf_struct->perf_xa);
	atomic_long_set(&perf_struct->last_read, jiffies);

	fc->perf_struct = perf_struct;
	return;

free_perf_struct:
	kfree(perf_struct);
}

void fuse_perf_destroy(struct fuse_conn *fc)
{
	struct fuse_perf_struct *perf_struct;

	perf_struct = xchg(&fc->perf_struct, NULL);
	if (!perf_struct)
		return;

	proc_remove(perf_struct->proc_entry);
	if (perf_struct->perf_xa)
		__fuse_perf_xa_destroy(perf_struct->perf_xa);
	if (perf_struct->perf_xa_to_read)
		__fuse_perf_xa_destroy(perf_struct->perf_xa_to_read);

	kfree(perf_struct);
}
