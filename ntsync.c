// SPDX-License-Identifier: GPL-2.0-only
/*
 * ntsync.c - Kernel driver for NT synchronization primitives
 *
 * Copyright (C) 2024 Elizabeth Figura <zfigura@codeweavers.com>
 */

#include <linux/anon_inodes.h>
#include <linux/atomic.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/overflow.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <uapi/linux/ntsync.h>

#define NTSYNC_NAME	"ntsync"

enum ntsync_type {
	NTSYNC_TYPE_SEM,
	NTSYNC_TYPE_MUTEX,
	NTSYNC_TYPE_EVENT,
};

/*
 * Individual synchronization primitives are represented by
 * struct ntsync_obj, and each primitive is backed by a file.
 *
 * The whole namespace is represented by a struct ntsync_device also
 * backed by a file.
 *
 * Both rely on struct file for reference counting. Individual
 * ntsync_obj objects take a reference to the device when created.
 * Wait operations take a reference to each object being waited on for
 * the duration of the wait.
 */

struct ntsync_obj {
	spinlock_t lock;

	enum ntsync_type type;

	struct file *file;
	struct ntsync_device *dev;

	/* The following fields are protected by the object lock. */
	union {
		struct {
			__u32 count;
			__u32 max;
		} sem;
		struct {
			__u32 count;
			__u32 owner;
			bool ownerdead;
		} mutex;
		struct {
			bool manual;
			bool signaled;
		} event;
	} u;

	/*
	 * any_waiters is protected by the object lock, but all_waiters is
	 * protected by the device wait_all_lock.
	 */
	struct list_head any_waiters;
	struct list_head all_waiters;

	/*
	 * Hint describing how many tasks are queued on this object in a
	 * wait-all operation.
	 *
	 * Any time we do a wake, we may need to wake "all" waiters as well as
	 * "any" waiters. In order to atomically wake "all" waiters, we must
	 * lock all of the objects, and that means grabbing the wait_all_lock
	 * below (and, due to lock ordering rules, before locking this object).
	 * However, wait-all is a rare operation, and grabbing the wait-all
	 * lock for every wake would create unnecessary contention.
	 * Therefore we first check whether all_hint is zero, and, if it is,
	 * we skip trying to wake "all" waiters.
	 *
	 * This hint isn't protected by any lock. It might change during the
	 * course of a wake, but there's no meaningful race there; it's only a
	 * hint.
	 *
	 * Since wait requests must originate from user-space threads, we're
	 * limited here by PID_MAX_LIMIT, so there's no risk of overflow.
	 */
	atomic_t all_hint;
};

struct ntsync_q_entry {
	struct list_head node;
	struct ntsync_q *q;
	struct ntsync_obj *obj;
	__u32 index;
};

struct ntsync_q {
	struct task_struct *task;
	__u32 owner;

	/*
	 * Protected via atomic_try_cmpxchg(). Only the thread that wins the
	 * compare-and-swap may actually change object states and wake this
	 * task.
	 */
	atomic_t signaled;

	bool all;
	bool ownerdead;
	__u32 count;
	struct ntsync_q_entry entries[];
};

struct ntsync_device {
	/*
	 * Wait-all operations must atomically grab all objects, and be totally
	 * ordered with respect to each other and wait-any operations.
	 * If one thread is trying to acquire several objects, another thread
	 * cannot touch the object at the same time.
	 *
	 * We achieve this by grabbing multiple object locks at the same time.
	 * However, this creates a lock ordering problem. To solve that problem,
	 * wait_all_lock is taken first whenever multiple objects must be locked
	 * at the same time.
	 */
	spinlock_t wait_all_lock;

	struct file *file;
};

static bool is_signaled(struct ntsync_obj *obj, __u32 owner)
{
	lockdep_assert_held(&obj->lock);

	switch (obj->type) {
	case NTSYNC_TYPE_SEM:
		return !!obj->u.sem.count;
	case NTSYNC_TYPE_MUTEX:
		if (obj->u.mutex.owner && obj->u.mutex.owner != owner)
			return false;
		return obj->u.mutex.count < UINT_MAX;
	case NTSYNC_TYPE_EVENT:
		return obj->u.event.signaled;
	}

	WARN(1, "bad object type %#x\n", obj->type);
	return false;
}

/*
 * "locked_obj" is an optional pointer to an object which is already locked and
 * should not be locked again. This is necessary so that changing an object's
 * state and waking it can be a single atomic operation.
 */
static void try_wake_all(struct ntsync_device *dev, struct ntsync_q *q,
			 struct ntsync_obj *locked_obj)
{
	__u32 count = q->count;
	bool can_wake = true;
	int signaled = -1;
	__u32 i;

	lockdep_assert_held(&dev->wait_all_lock);
	if (locked_obj)
		lockdep_assert_held(&locked_obj->lock);

	for (i = 0; i < count; i++) {
		if (q->entries[i].obj != locked_obj)
			spin_lock_nest_lock(&q->entries[i].obj->lock, &dev->wait_all_lock);
	}

	for (i = 0; i < count; i++) {
		if (!is_signaled(q->entries[i].obj, q->owner)) {
			can_wake = false;
			break;
		}
	}

	if (can_wake && atomic_try_cmpxchg(&q->signaled, &signaled, 0)) {
		for (i = 0; i < count; i++) {
			struct ntsync_obj *obj = q->entries[i].obj;

			switch (obj->type) {
			case NTSYNC_TYPE_SEM:
				obj->u.sem.count--;
				break;
			case NTSYNC_TYPE_MUTEX:
				if (obj->u.mutex.ownerdead)
					q->ownerdead = true;
				obj->u.mutex.ownerdead = false;
				obj->u.mutex.count++;
				obj->u.mutex.owner = q->owner;
				break;
			case NTSYNC_TYPE_EVENT:
				if (!obj->u.event.manual)
					obj->u.event.signaled = false;
				break;
			}
		}
		wake_up_process(q->task);
	}

	for (i = 0; i < count; i++) {
		if (q->entries[i].obj != locked_obj)
			spin_unlock(&q->entries[i].obj->lock);
	}
}

static void try_wake_all_obj(struct ntsync_device *dev, struct ntsync_obj *obj)
{
	struct ntsync_q_entry *entry;

	lockdep_assert_held(&dev->wait_all_lock);
	lockdep_assert_held(&obj->lock);

	list_for_each_entry(entry, &obj->all_waiters, node)
		try_wake_all(dev, entry->q, obj);
}

static void try_wake_any_sem(struct ntsync_obj *sem)
{
	struct ntsync_q_entry *entry;

	lockdep_assert_held(&sem->lock);

	list_for_each_entry(entry, &sem->any_waiters, node) {
		struct ntsync_q *q = entry->q;
		int signaled = -1;

		if (!sem->u.sem.count)
			break;

		if (atomic_try_cmpxchg(&q->signaled, &signaled, entry->index)) {
			sem->u.sem.count--;
			wake_up_process(q->task);
		}
	}
}

static void try_wake_any_mutex(struct ntsync_obj *mutex)
{
	struct ntsync_q_entry *entry;

	lockdep_assert_held(&mutex->lock);

	list_for_each_entry(entry, &mutex->any_waiters, node) {
		struct ntsync_q *q = entry->q;
		int signaled = -1;

		if (mutex->u.mutex.count == UINT_MAX)
			break;
		if (mutex->u.mutex.owner && mutex->u.mutex.owner != q->owner)
			continue;

		if (atomic_try_cmpxchg(&q->signaled, &signaled, entry->index)) {
			if (mutex->u.mutex.ownerdead)
				q->ownerdead = true;
			mutex->u.mutex.ownerdead = false;
			mutex->u.mutex.count++;
			mutex->u.mutex.owner = q->owner;
			wake_up_process(q->task);
		}
	}
}

static void try_wake_any_event(struct ntsync_obj *event)
{
	struct ntsync_q_entry *entry;

	lockdep_assert_held(&event->lock);

	list_for_each_entry(entry, &event->any_waiters, node) {
		struct ntsync_q *q = entry->q;
		int signaled = -1;

		if (!event->u.event.signaled)
			break;

		if (atomic_try_cmpxchg(&q->signaled, &signaled, entry->index)) {
			if (!event->u.event.manual)
				event->u.event.signaled = false;
			wake_up_process(q->task);
		}
	}
}

/*
 * Actually change the semaphore state, returning -EOVERFLOW if it is made
 * invalid.
 */
static int post_sem_state(struct ntsync_obj *sem, __u32 count)
{
	__u32 sum;

	lockdep_assert_held(&sem->lock);

	if (check_add_overflow(sem->u.sem.count, count, &sum) ||
	    sum > sem->u.sem.max)
		return -EOVERFLOW;

	sem->u.sem.count = sum;
	return 0;
}

static int ntsync_sem_post(struct ntsync_obj *sem, void __user *argp)
{
	struct ntsync_device *dev = sem->dev;
	__u32 __user *user_args = argp;
	__u32 prev_count;
	__u32 args;
	int ret;

	if (copy_from_user(&args, argp, sizeof(args)))
		return -EFAULT;

	if (sem->type != NTSYNC_TYPE_SEM)
		return -EINVAL;

	if (atomic_read(&sem->all_hint) > 0) {
		spin_lock(&dev->wait_all_lock);
		spin_lock_nest_lock(&sem->lock, &dev->wait_all_lock);

		prev_count = sem->u.sem.count;
		ret = post_sem_state(sem, args);
		if (!ret) {
			try_wake_all_obj(dev, sem);
			try_wake_any_sem(sem);
		}

		spin_unlock(&sem->lock);
		spin_unlock(&dev->wait_all_lock);
	} else {
		spin_lock(&sem->lock);

		prev_count = sem->u.sem.count;
		ret = post_sem_state(sem, args);
		if (!ret)
			try_wake_any_sem(sem);

		spin_unlock(&sem->lock);
	}

	if (!ret && put_user(prev_count, user_args))
		ret = -EFAULT;

	return ret;
}

/*
 * Actually change the mutex state, returning -EPERM if not the owner.
 */
static int unlock_mutex_state(struct ntsync_obj *mutex,
			      const struct ntsync_mutex_args *args)
{
	lockdep_assert_held(&mutex->lock);

	if (mutex->u.mutex.owner != args->owner)
		return -EPERM;

	if (!--mutex->u.mutex.count)
		mutex->u.mutex.owner = 0;
	return 0;
}

static int ntsync_mutex_unlock(struct ntsync_obj *mutex, void __user *argp)
{
	struct ntsync_mutex_args __user *user_args = argp;
	struct ntsync_device *dev = mutex->dev;
	struct ntsync_mutex_args args;
	__u32 prev_count;
	int ret;

	if (copy_from_user(&args, argp, sizeof(args)))
		return -EFAULT;
	if (!args.owner)
		return -EINVAL;

	if (mutex->type != NTSYNC_TYPE_MUTEX)
		return -EINVAL;

	if (atomic_read(&mutex->all_hint) > 0) {
		spin_lock(&dev->wait_all_lock);
		spin_lock_nest_lock(&mutex->lock, &dev->wait_all_lock);

		prev_count = mutex->u.mutex.count;
		ret = unlock_mutex_state(mutex, &args);
		if (!ret) {
			try_wake_all_obj(dev, mutex);
			try_wake_any_mutex(mutex);
		}

		spin_unlock(&mutex->lock);
		spin_unlock(&dev->wait_all_lock);
	} else {
		spin_lock(&mutex->lock);

		prev_count = mutex->u.mutex.count;
		ret = unlock_mutex_state(mutex, &args);
		if (!ret)
			try_wake_any_mutex(mutex);

		spin_unlock(&mutex->lock);
	}

	if (!ret && put_user(prev_count, &user_args->count))
		ret = -EFAULT;

	return ret;
}

/*
 * Actually change the mutex state to mark its owner as dead,
 * returning -EPERM if not the owner.
 */
static int kill_mutex_state(struct ntsync_obj *mutex, __u32 owner)
{
	lockdep_assert_held(&mutex->lock);

	if (mutex->u.mutex.owner != owner)
		return -EPERM;

	mutex->u.mutex.ownerdead = true;
	mutex->u.mutex.owner = 0;
	mutex->u.mutex.count = 0;
	return 0;
}

static int ntsync_mutex_kill(struct ntsync_obj *mutex, void __user *argp)
{
	struct ntsync_device *dev = mutex->dev;
	__u32 owner;
	int ret;

	if (get_user(owner, (__u32 __user *)argp))
		return -EFAULT;
	if (!owner)
		return -EINVAL;

	if (mutex->type != NTSYNC_TYPE_MUTEX)
		return -EINVAL;

	if (atomic_read(&mutex->all_hint) > 0) {
		spin_lock(&dev->wait_all_lock);
		spin_lock_nest_lock(&mutex->lock, &dev->wait_all_lock);

		ret = kill_mutex_state(mutex, owner);
		if (!ret) {
			try_wake_all_obj(dev, mutex);
			try_wake_any_mutex(mutex);
		}

		spin_unlock(&mutex->lock);
		spin_unlock(&dev->wait_all_lock);
	} else {
		spin_lock(&mutex->lock);

		ret = kill_mutex_state(mutex, owner);
		if (!ret)
			try_wake_any_mutex(mutex);

		spin_unlock(&mutex->lock);
	}

	return ret;
}

static int ntsync_event_set(struct ntsync_obj *event, void __user *argp, bool pulse)
{
	struct ntsync_device *dev = event->dev;
	__u32 prev_state;

	if (event->type != NTSYNC_TYPE_EVENT)
		return -EINVAL;

	if (atomic_read(&event->all_hint) > 0) {
		spin_lock(&dev->wait_all_lock);
		spin_lock_nest_lock(&event->lock, &dev->wait_all_lock);

		prev_state = event->u.event.signaled;
		event->u.event.signaled = true;
		try_wake_all_obj(dev, event);
		try_wake_any_event(event);
		if (pulse)
			event->u.event.signaled = false;

		spin_unlock(&event->lock);
		spin_unlock(&dev->wait_all_lock);
	} else {
		spin_lock(&event->lock);

		prev_state = event->u.event.signaled;
		event->u.event.signaled = true;
		try_wake_any_event(event);
		if (pulse)
			event->u.event.signaled = false;

		spin_unlock(&event->lock);
	}

	if (put_user(prev_state, (__u32 __user *)argp))
		return -EFAULT;

	return 0;
}

static int ntsync_event_reset(struct ntsync_obj *event, void __user *argp)
{
	__u32 prev_state;

	if (event->type != NTSYNC_TYPE_EVENT)
		return -EINVAL;

	spin_lock(&event->lock);

	prev_state = event->u.event.signaled;
	event->u.event.signaled = false;

	spin_unlock(&event->lock);

	if (put_user(prev_state, (__u32 __user *)argp))
		return -EFAULT;

	return 0;
}

static int ntsync_sem_read(struct ntsync_obj *sem, void __user *argp)
{
	struct ntsync_sem_args __user *user_args = argp;
	struct ntsync_sem_args args;

	if (sem->type != NTSYNC_TYPE_SEM)
		return -EINVAL;

	args.sem = 0;
	spin_lock(&sem->lock);
	args.count = sem->u.sem.count;
	args.max = sem->u.sem.max;
	spin_unlock(&sem->lock);

	if (copy_to_user(user_args, &args, sizeof(args)))
		return -EFAULT;
	return 0;
}

static int ntsync_mutex_read(struct ntsync_obj *mutex, void __user *argp)
{
	struct ntsync_mutex_args __user *user_args = argp;
	struct ntsync_mutex_args args;
	int ret;

	if (mutex->type != NTSYNC_TYPE_MUTEX)
		return -EINVAL;

	args.mutex = 0;
	spin_lock(&mutex->lock);
	args.count = mutex->u.mutex.count;
	args.owner = mutex->u.mutex.owner;
	ret = mutex->u.mutex.ownerdead ? -EOWNERDEAD : 0;
	spin_unlock(&mutex->lock);

	if (copy_to_user(user_args, &args, sizeof(args)))
		return -EFAULT;
	return ret;
}

static int ntsync_event_read(struct ntsync_obj *event, void __user *argp)
{
	struct ntsync_event_args __user *user_args = argp;
	struct ntsync_event_args args;

	if (event->type != NTSYNC_TYPE_EVENT)
		return -EINVAL;

	args.event = 0;
	spin_lock(&event->lock);
	args.manual = event->u.event.manual;
	args.signaled = event->u.event.signaled;
	spin_unlock(&event->lock);

	if (copy_to_user(user_args, &args, sizeof(args)))
		return -EFAULT;
	return 0;
}

static int ntsync_obj_release(struct inode *inode, struct file *file)
{
	struct ntsync_obj *obj = file->private_data;

	fput(obj->dev->file);
	kfree(obj);

	return 0;
}

static long ntsync_obj_ioctl(struct file *file, unsigned int cmd,
			     unsigned long parm)
{
	struct ntsync_obj *obj = file->private_data;
	void __user *argp = (void __user *)parm;

	switch (cmd) {
	case NTSYNC_IOC_SEM_POST:
		return ntsync_sem_post(obj, argp);
	case NTSYNC_IOC_SEM_READ:
		return ntsync_sem_read(obj, argp);
	case NTSYNC_IOC_MUTEX_UNLOCK:
		return ntsync_mutex_unlock(obj, argp);
	case NTSYNC_IOC_MUTEX_KILL:
		return ntsync_mutex_kill(obj, argp);
	case NTSYNC_IOC_MUTEX_READ:
		return ntsync_mutex_read(obj, argp);
	case NTSYNC_IOC_EVENT_SET:
		return ntsync_event_set(obj, argp, false);
	case NTSYNC_IOC_EVENT_RESET:
		return ntsync_event_reset(obj, argp);
	case NTSYNC_IOC_EVENT_PULSE:
		return ntsync_event_set(obj, argp, true);
	case NTSYNC_IOC_EVENT_READ:
		return ntsync_event_read(obj, argp);
	default:
		return -ENOIOCTLCMD;
	}
}

static const struct file_operations ntsync_obj_fops = {
	.owner		= THIS_MODULE,
	.release	= ntsync_obj_release,
	.unlocked_ioctl	= ntsync_obj_ioctl,
	.compat_ioctl	= compat_ptr_ioctl,
	.llseek		= no_llseek,
};

static struct ntsync_obj *ntsync_alloc_obj(struct ntsync_device *dev,
					   enum ntsync_type type)
{
	struct ntsync_obj *obj;

	obj = kzalloc(sizeof(*obj), GFP_KERNEL);
	if (!obj)
		return NULL;
	obj->type = type;
	obj->dev = dev;
	get_file(dev->file);
	spin_lock_init(&obj->lock);
	INIT_LIST_HEAD(&obj->any_waiters);
	INIT_LIST_HEAD(&obj->all_waiters);
	atomic_set(&obj->all_hint, 0);

	return obj;
}

static int ntsync_obj_get_fd(struct ntsync_obj *obj)
{
	struct file *file;
	int fd;

	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0)
		return fd;
	file = anon_inode_getfile("ntsync", &ntsync_obj_fops, obj, O_RDWR);
	if (IS_ERR(file)) {
		put_unused_fd(fd);
		return PTR_ERR(file);
	}
	obj->file = file;
	fd_install(fd, file);

	return fd;
}

static int ntsync_create_sem(struct ntsync_device *dev, void __user *argp)
{
	struct ntsync_sem_args __user *user_args = argp;
	struct ntsync_sem_args args;
	struct ntsync_obj *sem;
	int fd;

	if (copy_from_user(&args, argp, sizeof(args)))
		return -EFAULT;

	if (args.count > args.max)
		return -EINVAL;

	sem = ntsync_alloc_obj(dev, NTSYNC_TYPE_SEM);
	if (!sem)
		return -ENOMEM;
	sem->u.sem.count = args.count;
	sem->u.sem.max = args.max;
	fd = ntsync_obj_get_fd(sem);
	if (fd < 0) {
		kfree(sem);
		return fd;
	}

	return put_user(fd, &user_args->sem);
}

static int ntsync_create_mutex(struct ntsync_device *dev, void __user *argp)
{
	struct ntsync_mutex_args __user *user_args = argp;
	struct ntsync_mutex_args args;
	struct ntsync_obj *mutex;
	int fd;

	if (copy_from_user(&args, argp, sizeof(args)))
		return -EFAULT;

	if (!args.owner != !args.count)
		return -EINVAL;

	mutex = ntsync_alloc_obj(dev, NTSYNC_TYPE_MUTEX);
	if (!mutex)
		return -ENOMEM;
	mutex->u.mutex.count = args.count;
	mutex->u.mutex.owner = args.owner;
	fd = ntsync_obj_get_fd(mutex);
	if (fd < 0) {
		kfree(mutex);
		return fd;
	}

	return put_user(fd, &user_args->mutex);
}

static int ntsync_create_event(struct ntsync_device *dev, void __user *argp)
{
	struct ntsync_event_args __user *user_args = argp;
	struct ntsync_event_args args;
	struct ntsync_obj *event;
	int fd;

	if (copy_from_user(&args, argp, sizeof(args)))
		return -EFAULT;

	event = ntsync_alloc_obj(dev, NTSYNC_TYPE_EVENT);
	if (!event)
		return -ENOMEM;
	event->u.event.manual = args.manual;
	event->u.event.signaled = args.signaled;
	fd = ntsync_obj_get_fd(event);
	if (fd < 0) {
		kfree(event);
		return fd;
	}

	return put_user(fd, &user_args->event);
}

static struct ntsync_obj *get_obj(struct ntsync_device *dev, int fd)
{
	struct file *file = fget(fd);
	struct ntsync_obj *obj;

	if (!file)
		return NULL;

	if (file->f_op != &ntsync_obj_fops) {
		fput(file);
		return NULL;
	}

	obj = file->private_data;
	if (obj->dev != dev) {
		fput(file);
		return NULL;
	}

	return obj;
}

static void put_obj(struct ntsync_obj *obj)
{
	fput(obj->file);
}

static int ntsync_schedule(const struct ntsync_q *q, const struct ntsync_wait_args *args)
{
	ktime_t timeout = ns_to_ktime(args->timeout);
	clockid_t clock = CLOCK_MONOTONIC;
	ktime_t *timeout_ptr;
	int ret = 0;

	timeout_ptr = (args->timeout == U64_MAX ? NULL : &timeout);

	if (args->flags & NTSYNC_WAIT_REALTIME)
		clock = CLOCK_REALTIME;

	do {
		if (signal_pending(current)) {
			ret = -ERESTARTSYS;
			break;
		}

		set_current_state(TASK_INTERRUPTIBLE);
		if (atomic_read(&q->signaled) != -1) {
			ret = 0;
			break;
		}
		ret = schedule_hrtimeout_range_clock(timeout_ptr, 0, HRTIMER_MODE_ABS, clock);
	} while (ret < 0);
	__set_current_state(TASK_RUNNING);

	return ret;
}

/*
 * Allocate and initialize the ntsync_q structure, but do not queue us yet.
 */
static int setup_wait(struct ntsync_device *dev,
		      const struct ntsync_wait_args *args, bool all,
		      struct ntsync_q **ret_q)
{
	int fds[NTSYNC_MAX_WAIT_COUNT + 1];
	const __u32 count = args->count;
	struct ntsync_q *q;
	__u32 total_count;
	__u32 i, j;

	if (!args->owner)
		return -EINVAL;

	if (args->pad || (args->flags & ~NTSYNC_WAIT_REALTIME))
		return -EINVAL;

	if (args->count > NTSYNC_MAX_WAIT_COUNT)
		return -EINVAL;

	total_count = count;
	if (args->alert)
		total_count++;

	if (copy_from_user(fds, u64_to_user_ptr(args->objs),
			   array_size(count, sizeof(*fds))))
		return -EFAULT;
	if (args->alert)
		fds[count] = args->alert;

	q = kmalloc(struct_size(q, entries, total_count), GFP_KERNEL);
	if (!q)
		return -ENOMEM;
	q->task = current;
	q->owner = args->owner;
	atomic_set(&q->signaled, -1);
	q->all = all;
	q->ownerdead = false;
	q->count = count;

	for (i = 0; i < total_count; i++) {
		struct ntsync_q_entry *entry = &q->entries[i];
		struct ntsync_obj *obj = get_obj(dev, fds[i]);

		if (!obj)
			goto err;

		if (all) {
			/* Check that the objects are all distinct. */
			for (j = 0; j < i; j++) {
				if (obj == q->entries[j].obj) {
					put_obj(obj);
					goto err;
				}
			}
		}

		entry->obj = obj;
		entry->q = q;
		entry->index = i;
	}

	*ret_q = q;
	return 0;

err:
	for (j = 0; j < i; j++)
		put_obj(q->entries[j].obj);
	kfree(q);
	return -EINVAL;
}

static void try_wake_any_obj(struct ntsync_obj *obj)
{
	switch (obj->type) {
	case NTSYNC_TYPE_SEM:
		try_wake_any_sem(obj);
		break;
	case NTSYNC_TYPE_MUTEX:
		try_wake_any_mutex(obj);
		break;
	case NTSYNC_TYPE_EVENT:
		try_wake_any_event(obj);
		break;
	}
}

static int ntsync_wait_any(struct ntsync_device *dev, void __user *argp)
{
	struct ntsync_wait_args args;
	__u32 i, total_count;
	struct ntsync_q *q;
	int signaled;
	int ret;

	if (copy_from_user(&args, argp, sizeof(args)))
		return -EFAULT;

	ret = setup_wait(dev, &args, false, &q);
	if (ret < 0)
		return ret;

	total_count = args.count;
	if (args.alert)
		total_count++;

	/* queue ourselves */

	for (i = 0; i < total_count; i++) {
		struct ntsync_q_entry *entry = &q->entries[i];
		struct ntsync_obj *obj = entry->obj;

		spin_lock(&obj->lock);
		list_add_tail(&entry->node, &obj->any_waiters);
		spin_unlock(&obj->lock);
	}

	/*
	 * Check if we are already signaled.
	 *
	 * Note that the API requires that normal objects are checked before
	 * the alert event. Hence we queue the alert event last, and check
	 * objects in order.
	 */

	for (i = 0; i < total_count; i++) {
		struct ntsync_obj *obj = q->entries[i].obj;

		if (atomic_read(&q->signaled) != -1)
			break;

		spin_lock(&obj->lock);
		try_wake_any_obj(obj);
		spin_unlock(&obj->lock);
	}

	/* sleep */

	ret = ntsync_schedule(q, &args);

	/* and finally, unqueue */

	for (i = 0; i < total_count; i++) {
		struct ntsync_q_entry *entry = &q->entries[i];
		struct ntsync_obj *obj = entry->obj;

		spin_lock(&obj->lock);
		list_del(&entry->node);
		spin_unlock(&obj->lock);

		put_obj(obj);
	}

	signaled = atomic_read(&q->signaled);
	if (signaled != -1) {
		struct ntsync_wait_args __user *user_args = argp;

		/* even if we caught a signal, we need to communicate success */
		ret = q->ownerdead ? -EOWNERDEAD : 0;

		if (put_user(signaled, &user_args->index))
			ret = -EFAULT;
	} else if (!ret) {
		ret = -ETIMEDOUT;
	}

	kfree(q);
	return ret;
}

static int ntsync_wait_all(struct ntsync_device *dev, void __user *argp)
{
	struct ntsync_wait_args args;
	struct ntsync_q *q;
	int signaled;
	__u32 i;
	int ret;

	if (copy_from_user(&args, argp, sizeof(args)))
		return -EFAULT;

	ret = setup_wait(dev, &args, true, &q);
	if (ret < 0)
		return ret;

	/* queue ourselves */

	spin_lock(&dev->wait_all_lock);

	for (i = 0; i < args.count; i++) {
		struct ntsync_q_entry *entry = &q->entries[i];
		struct ntsync_obj *obj = entry->obj;

		atomic_inc(&obj->all_hint);

		/*
		 * obj->all_waiters is protected by dev->wait_all_lock rather
		 * than obj->lock, so there is no need to acquire obj->lock
		 * here.
		 */
		list_add_tail(&entry->node, &obj->all_waiters);
	}
	if (args.alert) {
		struct ntsync_q_entry *entry = &q->entries[args.count];
		struct ntsync_obj *obj = entry->obj;

		spin_lock_nest_lock(&obj->lock, &dev->wait_all_lock);
		list_add_tail(&entry->node, &obj->any_waiters);
		spin_unlock(&obj->lock);
	}

	/* check if we are already signaled */

	try_wake_all(dev, q, NULL);

	spin_unlock(&dev->wait_all_lock);

	/*
	 * Check if the alert event is signaled, making sure to do so only
	 * after checking if the other objects are signaled.
	 */

	if (args.alert) {
		struct ntsync_obj *obj = q->entries[args.count].obj;

		if (atomic_read(&q->signaled) == -1) {
			spin_lock(&obj->lock);
			try_wake_any_obj(obj);
			spin_unlock(&obj->lock);
		}
	}

	/* sleep */

	ret = ntsync_schedule(q, &args);

	/* and finally, unqueue */

	spin_lock(&dev->wait_all_lock);

	for (i = 0; i < args.count; i++) {
		struct ntsync_q_entry *entry = &q->entries[i];
		struct ntsync_obj *obj = entry->obj;

		/*
		 * obj->all_waiters is protected by dev->wait_all_lock rather
		 * than obj->lock, so there is no need to acquire it here.
		 */
		list_del(&entry->node);

		atomic_dec(&obj->all_hint);

		put_obj(obj);
	}
	if (args.alert) {
		struct ntsync_q_entry *entry = &q->entries[args.count];
		struct ntsync_obj *obj = entry->obj;

		spin_lock_nest_lock(&obj->lock, &dev->wait_all_lock);
		list_del(&entry->node);
		spin_unlock(&obj->lock);

		put_obj(obj);
	}

	spin_unlock(&dev->wait_all_lock);

	signaled = atomic_read(&q->signaled);
	if (signaled != -1) {
		struct ntsync_wait_args __user *user_args = argp;

		/* even if we caught a signal, we need to communicate success */
		ret = q->ownerdead ? -EOWNERDEAD : 0;

		if (put_user(signaled, &user_args->index))
			ret = -EFAULT;
	} else if (!ret) {
		ret = -ETIMEDOUT;
	}

	kfree(q);
	return ret;
}

static int ntsync_char_open(struct inode *inode, struct file *file)
{
	struct ntsync_device *dev;

	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	spin_lock_init(&dev->wait_all_lock);

	file->private_data = dev;
	dev->file = file;
	return nonseekable_open(inode, file);
}

static int ntsync_char_release(struct inode *inode, struct file *file)
{
	struct ntsync_device *dev = file->private_data;

	kfree(dev);

	return 0;
}

static long ntsync_char_ioctl(struct file *file, unsigned int cmd,
			      unsigned long parm)
{
	struct ntsync_device *dev = file->private_data;
	void __user *argp = (void __user *)parm;

	switch (cmd) {
	case NTSYNC_IOC_CREATE_EVENT:
		return ntsync_create_event(dev, argp);
	case NTSYNC_IOC_CREATE_MUTEX:
		return ntsync_create_mutex(dev, argp);
	case NTSYNC_IOC_CREATE_SEM:
		return ntsync_create_sem(dev, argp);
	case NTSYNC_IOC_WAIT_ALL:
		return ntsync_wait_all(dev, argp);
	case NTSYNC_IOC_WAIT_ANY:
		return ntsync_wait_any(dev, argp);
	default:
		return -ENOIOCTLCMD;
	}
}

static const struct file_operations ntsync_fops = {
	.owner		= THIS_MODULE,
	.open		= ntsync_char_open,
	.release	= ntsync_char_release,
	.unlocked_ioctl	= ntsync_char_ioctl,
	.compat_ioctl	= compat_ptr_ioctl,
	.llseek		= no_llseek,
};

static struct miscdevice ntsync_misc = {
	.minor		= MISC_DYNAMIC_MINOR,
	.name		= NTSYNC_NAME,
	.fops		= &ntsync_fops,
};

module_misc_device(ntsync_misc);

MODULE_AUTHOR("Elizabeth Figura <zfigura@codeweavers.com>");
MODULE_DESCRIPTION("Kernel driver for NT synchronization primitives");
MODULE_LICENSE("GPL");
