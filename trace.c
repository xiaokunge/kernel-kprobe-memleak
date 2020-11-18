// SPDX-License-Identifier: GPL-3.0
/*
 * trace.c
 *
 * The ring buffer based tracing information store.
 */
#define pr_fmt(fmt) CONFIG_MODULE_NAME ": " fmt

#include <linux/proc_fs.h>
#include <linux/slab.h>
#include "trace.h"
#include "kprobe.h"

#define PROC_NAME	"tracing"

#define PRINT_EVENT_ID_MAX	\
	((1 << (sizeof(((struct print_event_entry *)0)->id) * 8)) - 1)

struct print_event_iterator {
	struct mutex			mutex;
	struct ring_buffer		*buffer;

	/* The below is zeroed out in pipe_read */
	struct trace_seq		seq;
	struct print_event_entry	*ent;
	unsigned long			lost_events;
	int				cpu;
	u64				ts;
	/* All new field here will be zeroed out in pipe_read */
};

/* Defined in linker script */
extern struct print_event_class * const __start_print_event_class[];
extern struct print_event_class * const __stop_print_event_class[];

static struct ring_buffer *ring_buffer;
static int (*ring_buffer_waiting)(struct ring_buffer *buffer, int cpu,
				  bool full);

static int kallsyms_lookup_symbols(void)
{
	ring_buffer_waiting = (void *)kallsyms_lookup_name("ring_buffer_wait");

	return ring_buffer_waiting ? 0 : -ENODEV;
}

static int trace_open_pipe(struct inode *inode, struct file *filp)
{
	struct print_event_iterator *iter;

	iter = kzalloc(sizeof(*iter), GFP_KERNEL);
	if (!iter)
		return -ENOMEM;

	trace_seq_init(&iter->seq);
	mutex_init(&iter->mutex);
	iter->buffer = PDE_DATA(inode);
	filp->private_data = iter;
	nonseekable_open(inode, filp);

	return 0;
}

static int is_trace_empty(struct print_event_iterator *iter)
{
	int cpu;

	for_each_possible_cpu(cpu) {
		if (!ring_buffer_empty_cpu(iter->buffer, cpu))
			return 0;
	}

	return 1;
}

/* Must be called with iter->mutex held. */
static int trace_wait_pipe(struct file *filp)
{
	struct print_event_iterator *iter = filp->private_data;
	int ret;

	while (is_trace_empty(iter)) {

		if ((filp->f_flags & O_NONBLOCK))
			return -EAGAIN;

		mutex_unlock(&iter->mutex);

		ret = ring_buffer_waiting(iter->buffer, RING_BUFFER_ALL_CPUS,
					  false);

		mutex_lock(&iter->mutex);

		if (ret)
			return ret;
	}

	return 1;
}

static struct print_event_entry *
peek_next_entry(struct print_event_iterator *iter, int cpu, u64 *ts,
		unsigned long *lost_events)
{
	struct ring_buffer_event *event;

	event = ring_buffer_peek(iter->buffer, cpu, ts, lost_events);

	if (event)
		return ring_buffer_event_data(event);

	return NULL;
}

static struct print_event_entry *
__find_next_entry(struct print_event_iterator *iter, int *ent_cpu,
		  unsigned long *missing_events, u64 *ent_ts)
{
	struct ring_buffer *buffer = iter->buffer;
	struct print_event_entry *ent, *next = NULL;
	unsigned long lost_events = 0, next_lost = 0;
	u64 next_ts = 0, ts;
	int next_cpu = -1;
	int cpu;

	for_each_possible_cpu(cpu) {

		if (ring_buffer_empty_cpu(buffer, cpu))
			continue;

		ent = peek_next_entry(iter, cpu, &ts, &lost_events);

		/*
		 * Pick the entry with the smallest timestamp:
		 */
		if (ent && (!next || ts < next_ts)) {
			next = ent;
			next_cpu = cpu;
			next_ts = ts;
			next_lost = lost_events;
		}
	}

	if (ent_cpu)
		*ent_cpu = next_cpu;

	if (ent_ts)
		*ent_ts = next_ts;

	if (missing_events)
		*missing_events = next_lost;

	return next;
}

/* Find the next real entry, and increment the iterator to the next entry */
static void *trace_next_entry_inc(struct print_event_iterator *iter)
{
	iter->ent = __find_next_entry(iter, &iter->cpu,
				      &iter->lost_events, &iter->ts);

	return iter->ent ? iter : NULL;
}

static struct print_event_class *find_print_event(int id)
{
	if (likely(id < (__stop_print_event_class - __start_print_event_class)))
		return __start_print_event_class[id];

	return NULL;
}

static enum print_line_t print_trace_fmt_line(struct print_event_iterator *iter)
{
	struct trace_seq *seq = &iter->seq;
	struct print_event_entry *entry;
	struct print_event_class *class;

	entry = iter->ent;
	class = find_print_event(entry->id);

	if (trace_seq_has_overflowed(seq))
		return TRACE_TYPE_PARTIAL_LINE;

	if (class)
		return class->format(seq, entry);

	trace_seq_printf(seq, "Unknown id %d\n", entry->id);

	return trace_handle_return(seq);
}

static ssize_t trace_read_pipe(struct file *filp, char __user *ubuf,
			       size_t cnt, loff_t *ppos)
{
	ssize_t sret;
	struct print_event_iterator *iter = filp->private_data;
	static DEFINE_MUTEX(access_lock);

	/*
	 * Avoid more than one consumer on a single file descriptor
	 * This is just a matter of traces coherency, the ring buffer itself
	 * is protected.
	 */
	mutex_lock(&iter->mutex);

	sret = trace_seq_to_user(&iter->seq, ubuf, cnt);
	if (sret != -EBUSY)
		goto out;

	trace_seq_init(&iter->seq);

waitagain:
	if (fatal_signal_pending(current))
		goto out;

	sret = trace_wait_pipe(filp);
	if (sret <= 0)
		goto out;

	/* stop when tracing is finished */
	if (is_trace_empty(iter)) {
		sret = 0;
		goto out;
	}

	if (cnt >= PAGE_SIZE)
		cnt = PAGE_SIZE - 1;

	memset(&iter->seq, 0,
	       sizeof(*iter) - offsetof(struct print_event_iterator, seq));
	trace_seq_init(&iter->seq);

	mutex_lock(&access_lock);
	while (trace_next_entry_inc(iter) != NULL) {
		enum print_line_t ret;
		int save_len = iter->seq.seq.len;

		ret = print_trace_fmt_line(iter);
		if (ret == TRACE_TYPE_PARTIAL_LINE) {
			/* don't print partial lines */
			iter->seq.seq.len = save_len;
			break;
		}
		if (ret != TRACE_TYPE_NO_CONSUME)
			ring_buffer_consume(iter->buffer, iter->cpu, &iter->ts,
					    &iter->lost_events);

		if (trace_seq_used(&iter->seq) >= cnt)
			break;

		/*
		 * Setting the full flag means we reached the trace_seq buffer
		 * size and we should leave by partial output condition above.
		 * One of the trace_seq_* functions is not used properly.
		 */
		WARN_ONCE(iter->seq.full, "full flag set for trace id: %d",
			  iter->ent->id);
	}
	mutex_unlock(&access_lock);

	/* Now copy what we have to the user */
	sret = trace_seq_to_user(&iter->seq, ubuf, cnt);
	if (iter->seq.seq.readpos >= trace_seq_used(&iter->seq))
		trace_seq_init(&iter->seq);

	/*
	 * If there was nothing to send to user, in spite of consuming trace
	 * entries, go back to wait for more entries.
	 */
	if (sret == -EBUSY)
		goto waitagain;

out:
	mutex_unlock(&iter->mutex);

	return sret;
}

static int trace_release_pipe(struct inode *inode, struct file *file)
{
	struct print_event_iterator *iter = file->private_data;

	mutex_destroy(&iter->mutex);
	kfree(iter);

	return 0;
}

static const struct file_operations trace_pipe_fops = {
	.owner		= THIS_MODULE,
	.open		= trace_open_pipe,
	.read		= trace_read_pipe,
	.release	= trace_release_pipe,
	.llseek		= no_llseek,
};

static inline int num_print_event_class(void)
{
	return __stop_print_event_class - __start_print_event_class;
}

static int __init print_event_init(void)
{
	int id = 0;
	int num_class = num_print_event_class();
	struct print_event_class * const *class_ptr;
	struct proc_dir_entry *parent_dir;

	if (num_class == 0)
		return 0;

	if (num_class >= PRINT_EVENT_ID_MAX)
		return -EINVAL;

	if (kallsyms_lookup_symbols())
		return -ENODEV;

	ring_buffer = ring_buffer_alloc(RB_BUFFER_SIZE, RB_FL_OVERWRITE);
	if (!ring_buffer)
		return -ENOMEM;

	parent_dir = proc_mkdir(PROC_NAME, NULL);
	if (!parent_dir)
		goto free;

	if (!proc_create_data("trace_pipe", S_IRUSR, parent_dir,
			      &trace_pipe_fops, ring_buffer))
		goto remove_proc;

	for (class_ptr = __start_print_event_class;
	     class_ptr < __stop_print_event_class; class_ptr++) {
		struct print_event_class *class = *class_ptr;

		class->id = id++;
		class->buffer = ring_buffer;
	}
	pr_info("create %d print event class\n", num_class);

	return 0;

remove_proc:
	remove_proc_subtree(PROC_NAME, NULL);
free:
	kfree(ring_buffer);

	return -ENOMEM;
}

static void print_event_exit(void)
{
	int num_class = num_print_event_class();

	if (num_class == 0)
		return;
	remove_proc_subtree(PROC_NAME, NULL);
	ring_buffer_free(ring_buffer);

	pr_info("destroy %d print event class\n", num_class);
}

KPROBE_INITCALL(print_event_init, print_event_exit);
