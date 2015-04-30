/*******************************************************************************
Copyright 2010 Broadcom Corporation.  All rights reserved.

Unless you and Broadcom execute a separate written software license agreement
governing use of this software, this software is licensed to you under the
terms of the GNU General Public License version 2, available at
http://www.gnu.org/copyleft/gpl.html (the "GPL").

Notwithstanding the above, under no circumstances may you combine this software
in any way with any other Broadcom software provided under a license other than
the GPL, without Broadcom's express prior written consent.
*******************************************************************************/

#define pr_fmt(fmt) "<%s::%s::%d> " fmt "\n", common->mm_name,\
				 __func__, __LINE__ \

#include "mm_common.h"
#include "mm_core.h"
#include "mm_dvfs.h"
#include "mm_prof.h"


/* The following varliables in this block shall
	be accessed with mm_fmwk_mutex protection */

DEFINE_MUTEX(mm_fmwk_mutex);
LIST_HEAD(mm_dev_list);
static struct workqueue_struct *single_wq;
static char *single_wq_name = "mm_wq";
wait_queue_head_t mm_queue;
DEFINE_MUTEX(mm_common_mutex);

/* MM Framework globals end*/

void mm_common_enable_clock(struct mm_common *common)
{
	if (common->mm_hw_is_on == 0) {
		if (common->common_clk) {
			clk_enable(common->common_clk);
			if (strncmp(common->mm_name, "mm_h264", 7))
				clk_reset(common->common_clk);
			}
		pr_debug("mm common clock turned on ");
		atomic_notifier_call_chain(&common->notifier_head, \
				MM_FMWK_NOTIFY_CLK_ENABLE, NULL); \
		}

	common->mm_hw_is_on++;
}

void mm_common_disable_clock(struct mm_common *common)
{
	BUG_ON(common->mm_hw_is_on == 0);

	common->mm_hw_is_on--;

	if (common->mm_hw_is_on == 0) {
		pr_debug("mm common clock turned off ");
		if (common->common_clk)
			clk_disable(common->common_clk);
		atomic_notifier_call_chain(&common->notifier_head, \
				MM_FMWK_NOTIFY_CLK_DISABLE, NULL); \
		}
}

static struct dev_job_list *mm_common_alloc_job(\
			struct file_private_data *private)
{
	struct dev_job_list *job = kmalloc(sizeof(struct dev_job_list),\
						GFP_KERNEL);
	if (!job)
		return NULL;

	job->filp = private;
	job->job.size = 0;
	job->added2core = false;
	job->successor = NULL;
	job->predecessor = NULL;
	INIT_LIST_HEAD(&job->wait_list);
	INIT_LIST_HEAD(&job->file_list);
	plist_node_init(&job->core_list, private->prio);
	job->job.type = INTERLOCK_WAITING_JOB;
	job->job.id = 0;
	job->job.data = NULL;
	job->job.size = 0;
	job->job.status = MM_JOB_STATUS_READY;

	return job;
}

void mm_common_priority_update(struct dev_job_list *to, int prio)
{
	struct dev_job_list *job = NULL;
	struct dev_job_list *temp = NULL;

	if (to == NULL)
		return;
	if (to->filp->prio <= prio)
		return;

	list_for_each_entry_safe(job, temp, &(to->filp->write_head), \
							file_list) { \
		if (job->job.type == INTERLOCK_WAITING_JOB)
			mm_common_priority_update(job->predecessor, prio);
		else
			mm_core_move_job(job, \
	job->filp->common->mm_core[(job->job.type&0xFF0000)>>16], prio); \

		if (job == to)
			break;
		}
}

struct job_maint_work {
	struct work_struct work;
	struct file_private_data *filp;
	struct mm_common *common;

	union {
		struct dev_job_list *job_list;
		struct dev_status_list *job_status;
		struct file_private_data *filp;
		struct read {
			struct dev_job_list **job_list;
			struct file_private_data *filp;
			} rd;
		struct interlock {
			struct dev_job_list *from;
			struct dev_job_list *to;
			struct dev_status_list *status;
			} il;
		} u;

};

#define SCHEDULE_JOB_WORK(a, b, c)				\
{								\
	struct job_maint_work _a;				\
	INIT_WORK(&(_a.work), c);				\
	_a.u.a = b;						\
	queue_work_on(0, common->single_wq, &(_a.work));	\
	flush_work_sync(&(_a.work));				\
}

void mm_common_add_job(struct work_struct *work)
{
	struct job_maint_work *maint_job = container_of(work, \
					struct job_maint_work,\
					 work);
	struct dev_job_list *job = maint_job->u.job_list;
	struct file_private_data *filp = job->filp;
	struct mm_common *common = filp->common;

	struct mm_core *core_dev = common->mm_core[\
				(job->job.type&0xFF0000)>>16];

	mutex_lock(&mm_common_mutex);
	job->job.status = MM_JOB_STATUS_READY;
	if (filp->interlock_count == 0)
		mm_core_add_job(job, core_dev);
	list_add_tail(&(job->file_list), &(filp->write_head));
	atomic_notifier_call_chain(&common->notifier_head, \
				MM_FMWK_NOTIFY_JOB_ADD, NULL);
	mutex_unlock(&mm_common_mutex);
}

#define SCHEDULE_ADD_WORK(b)	SCHEDULE_JOB_WORK(job_list, \
					b, mm_common_add_job)

void mm_common_wait_job(struct work_struct *work)
{
	struct job_maint_work *maint_job = container_of(work, \
					struct job_maint_work, \
					work);
	struct dev_job_list *from = maint_job->u.il.from;
	struct dev_job_list *to = maint_job->u.il.to;
	struct dev_status_list *status = maint_job->u.il.status;
	struct file_private_data *to_filp = to->filp;

	mutex_lock(&mm_common_mutex);
	if (status) {
		if (status->status.status == MM_JOB_STATUS_INVALID) {
			list_add_tail(&status->wait_list, &to->wait_list);
			}
		else {
			list_del_init(&status->wait_list);
			mutex_unlock(&mm_common_mutex);
			return;
			}
		}
	if (from) {
		struct file_private_data *from_filp = from->filp;
		from->job.status = MM_JOB_STATUS_READY;
		list_add_tail(&(from->file_list), &(from_filp->write_head));
		from->successor = to;
		to->predecessor = from;
		to_filp->interlock_count++;
		mm_common_priority_update(to->predecessor, to->filp->prio);
		}

	to->job.status = MM_JOB_STATUS_READY;
	list_add_tail(&(to->file_list), &(to_filp->write_head));

	if ((from == NULL) && list_is_singular(&to_filp->write_head)) {
		mm_common_interlock_completion(to);
		}
	else if (from != NULL) {
		struct file_private_data *from_filp = from->filp;
		if (list_is_singular(&from_filp->write_head))
			mm_common_interlock_completion(from);
		}
	mutex_unlock(&mm_common_mutex);
}

#define SCHEDULE_INTERLOCK_WORK(b)	SCHEDULE_JOB_WORK(il, \
					b, mm_common_wait_job)\

void mm_common_read_job(struct work_struct *work)
{
	struct job_maint_work *maint_job = container_of(work, \
					struct job_maint_work,\
					work);
	struct dev_job_list **job_list = maint_job->u.rd.job_list;
	struct file_private_data *filp = maint_job->u.rd.filp;

	mutex_lock(&mm_common_mutex);
	if (filp->read_count > 0) {
		struct dev_job_list *job =
			list_first_entry(&(filp->read_head),\
				 struct dev_job_list, file_list);\
		list_del_init(&job->file_list);
		*job_list = job;
		filp->read_count--;
		}
	else {
			*job_list = NULL;
		}
	mutex_unlock(&mm_common_mutex);

}
#define SCHEDULE_READ_WORK(b)	SCHEDULE_JOB_WORK(rd, b, mm_common_read_job)

void mm_common_release_jobs(struct work_struct *work)
{
	struct job_maint_work *maint_job = container_of(work, \
					struct job_maint_work, \
					work);
	struct file_private_data *filp = maint_job->u.filp;
	struct mm_common *common = filp->common;
	struct dev_job_list *job = NULL;
	struct dev_job_list *temp = NULL;


	mutex_lock(&mm_common_mutex);
	list_for_each_entry_safe(job, temp, &(filp->write_head), file_list) {
		pr_err("this  = %p[%x] next = %p, prev= %p", &job->file_list,\
			 filp->prio, job->file_list.next, job->file_list.prev);
		}

	while (0 == list_empty(&filp->write_head)) {
		job = list_first_entry(&filp->write_head, struct dev_job_list, \
								file_list);
		if (job->job.type != INTERLOCK_WAITING_JOB) {
				mm_core_abort_job(job, \
		common->mm_core[(job->job.type&0xFF0000)>>16]);
				mm_common_job_completion(job, \
				common->mm_core[(job->job.type&0xFF0000)>>16]);
				atomic_notifier_call_chain( \
				&common->notifier_head, \
				MM_FMWK_NOTIFY_JOB_REMOVE, NULL);
				}
			else {
				mm_common_interlock_completion(job);
				}
		}

	list_for_each_entry_safe(job, temp, &(filp->read_head), file_list) {
		list_del_init(&job->file_list);
		kfree(job->job.data);
		kfree(job);
		job = NULL;
		}
	filp->read_count = -1;
	pr_debug(" %p %d", filp, filp->read_count);
	wake_up_all(&filp->queue);
	mutex_unlock(&mm_common_mutex);
}

#define SCHEDULE_RELEASE_WORK(b)	SCHEDULE_JOB_WORK(filp, \
					b, mm_common_release_jobs)

void mm_common_interlock_completion(struct dev_job_list *job)
{
	struct file_private_data *filp = job->filp;
	struct mm_common *common = filp->common;

	BUG_ON(job->job.type != INTERLOCK_WAITING_JOB);
	job->job.type = 0;

	list_del_init(&job->file_list);

	if (job->predecessor) {
		BUG_ON(job->filp->interlock_count == 0);
		job->filp->interlock_count--;
		job->predecessor->successor = NULL;
		job->predecessor = NULL;
		}
	if (job->successor)
		mm_common_interlock_completion(job->successor);

	if (0 == list_empty(&filp->write_head)) {
		struct dev_job_list *temp_wait_job = NULL;
		struct dev_job_list *wait_job = list_first_entry( \
						&(filp->write_head), \
						struct dev_job_list, file_list);

		if ((wait_job->job.type == INTERLOCK_WAITING_JOB) &&
			(wait_job->predecessor == NULL)) {
				mm_common_interlock_completion(wait_job);
				}
		if (wait_job->job.type != INTERLOCK_WAITING_JOB) {
			list_for_each_entry_safe(wait_job, temp_wait_job, \
				&(job->filp->write_head), file_list) {
				if (wait_job->job.type != INTERLOCK_WAITING_JOB)
					mm_core_add_job(wait_job, \
			common->mm_core[(wait_job->job.type&0xFF0000)>>16]);
				else
					break;
				}
			}
		}
	if (0 == list_empty(&job->wait_list)) {
		struct dev_status_list *wait_list = NULL;
		struct dev_status_list *temp_wait_list = NULL;
		list_for_each_entry_safe(wait_list, \
					temp_wait_list, \
					&(job->wait_list), \
					wait_list) {
			/*pr_err("interlock completing job %x %x %x", \
						job, \
						wait_list, \
						job->job.status);*/
			list_del_init(&wait_list->wait_list);
			wait_list->status.status = job->job.status;
			}
		wake_up_all(&mm_queue);
		}

	kfree(job);
}

void mm_common_job_completion(struct dev_job_list *job, void *core)
{
	struct file_private_data *filp = job->filp;
	struct mm_core *core_dev = (struct mm_core *)core;
	struct mm_common *common = filp->common;

	list_del_init(&job->file_list);
	mm_core_remove_job(job, core_dev);
	atomic_notifier_call_chain(&common->notifier_head, \
	MM_FMWK_NOTIFY_JOB_COMPLETE, (void *) job->job.type);

	if (filp->readable) {
		filp->read_count++;
		list_add_tail(&(job->file_list), &(filp->read_head));
		wake_up_all(&filp->queue);
		}
	else {
		kfree(job->job.data);
		kfree(job);
		}

	if (0 == list_empty(&filp->write_head)) {
		struct dev_job_list *wait_job = list_first_entry( \
						&(filp->write_head), \
						struct dev_job_list, file_list);
		if ((wait_job->job.type == INTERLOCK_WAITING_JOB) &&
			(wait_job->predecessor == NULL)) {
				mm_common_interlock_completion(wait_job);
				}
		}

}

static int mm_file_open(struct inode *inode, struct file *filp)
{
	struct miscdevice *miscdev = filp->private_data;
	struct mm_common *common = container_of(miscdev, \
					struct mm_common, mdev);
	struct file_private_data *private = kmalloc( \
			sizeof(struct file_private_data), GFP_KERNEL);

	private->common = common;
	private->interlock_count = 0;
	private->prio = current->prio;
	private->read_count = 0;
	private->readable = ((filp->f_mode & FMODE_READ) == FMODE_READ);
	init_waitqueue_head(&private->queue);

	INIT_LIST_HEAD(&private->read_head);
	INIT_LIST_HEAD(&private->write_head);
	pr_debug(" %p ", private);

	filp->private_data = private;

	return 0;
}

static int mm_file_release(struct inode *inode, struct file *filp)
{
	struct file_private_data *private = filp->private_data;
	struct mm_common *common = private->common;

	/* Free all jobs posted using this file */
	SCHEDULE_RELEASE_WORK(private);
	kfree(private);
	return 0;
}

static bool is_validate_file(struct file *filp);

static loff_t mm_file_lseek(struct file *filp, loff_t offset, int ignore)
{
	struct file_private_data *private = filp->private_data;
	struct mm_common *common = private->common;

	struct file *input = fget(offset);

	if (input == NULL) {
		return -EINVAL;
	}
	if (is_validate_file(input)) {
		struct file_private_data *in_private = input->private_data;
		struct interlock il;

		il.to = mm_common_alloc_job(private);
		il.from = mm_common_alloc_job(in_private);
		il.status = NULL;

		SCHEDULE_INTERLOCK_WORK(il);

		fput(input);
		}
	else {
		pr_err("unable to find file");
		}
	return 0;
}

static int mm_file_write(struct file *filp, const char __user *buf,
			size_t size, loff_t *offset)
{
	struct file_private_data *private = filp->private_data;
	struct mm_common *common = private->common;
	struct dev_job_list *mm_job_node = mm_common_alloc_job(private);

	if (!mm_job_node)
		return -ENOMEM;

	mm_job_node->job.size = size - 8;
	if (size < 8)
		goto out;

	if (copy_from_user(&(mm_job_node->job.type), buf, \
				sizeof(mm_job_node->job.type))) {
		pr_err("copy_from_user failed for type");
		goto out;
		}
	size -= sizeof(mm_job_node->job.type);
	buf += sizeof(mm_job_node->job.type);
	if (copy_from_user(&(mm_job_node->job.id), buf , \
				sizeof(mm_job_node->job.id))) {
		pr_err("copy_from_user failed for type");
		goto out;
		}
	size -= sizeof(mm_job_node->job.id);
	buf += sizeof(mm_job_node->job.id);
	if (size > 0) {
		void *job_post = NULL;
		uint32_t *ptr ;
		job_post = kmalloc(size, GFP_KERNEL);
		mm_job_node->job.data = job_post;
		ptr = (uint32_t *)job_post;
		if (copy_from_user(job_post, buf, size)) {
			pr_err("MM_IOCTL_POST_JOB data copy_from_user failed");
			kfree(job_post);
			goto out;
			}

		pr_debug("mm_file_write %x %x %x %x %x %x %x", \
					mm_job_node->job.size,
					mm_job_node->job.type,
					mm_job_node->job.id,
					ptr[0], ptr[1], ptr[2], ptr[3]);
		BUG_ON(((mm_job_node->job.type&0xFF0000)>>16) \
					>= MAX_ASYMMETRIC_PROC);
		BUG_ON(common->mm_core[(mm_job_node->job.type&0xFF0000)>>16] \
								== NULL); \
		SCHEDULE_ADD_WORK(mm_job_node);
		}
	else {
		pr_err("zero size write");
		goto out;
		}

	return 0;
out:
	kfree(mm_job_node);
	return 0;
}

static int mm_file_read(struct file *filp, \
			char __user *buf, \
			size_t size, \
			loff_t *offset)
{
	struct file_private_data *private = filp->private_data;
	struct mm_common *common = private->common;
	struct read rd;
	struct dev_job_list *job = NULL;
	size_t bytes_read = 0;

	rd.job_list = &job;
	rd.filp = private;

/*	pr_err("something");*/
	SCHEDULE_READ_WORK(rd);

	if (job == NULL)
		goto mm_file_read_end;

	if (job->job.id) {
		if (copy_to_user(buf, &job->job.status, \
				sizeof(job->job.status))) {
			pr_err("copy_to_user failed");
			goto mm_file_read_end;
			}
		bytes_read += sizeof(job->job.status);
		buf +=  sizeof(job->job.status);
		if (copy_to_user(buf, &job->job.id, \
				sizeof(job->job.id))) {
			pr_err("copy_to_user failed");
			goto mm_file_read_end;
			}
		bytes_read += sizeof(job->job.id);
		buf +=  sizeof(job->job.id);
		if (copy_to_user(buf, job->job.data, \
					job->job.size)) {
			pr_err("copy_to_user failed");
			goto mm_file_read_end;
			}
		bytes_read += job->job.size;
		}
	kfree(job->job.data);
	kfree(job);
	return bytes_read;
mm_file_read_end:
	return 0;
}

static unsigned int mm_file_poll(struct file *filp, \
			struct poll_table_struct *wait)
{
	struct file_private_data *private = filp->private_data;
	struct mm_common *common = private->common;

	poll_wait(filp, &private->queue, wait);

	if (private->read_count != 0) {
		pr_debug(" %p %d", private, private->read_count);
		return POLLIN | POLLRDNORM;
		}

	return 0;
}

int mm_file_fsync(struct file *filp, loff_t p1, loff_t p2, int datasync)
{
	struct file_private_data *private = filp->private_data;
	struct mm_common *common = private->common;
	struct interlock il;
	struct dev_status_list job_status;

	INIT_LIST_HEAD(&job_status.wait_list);
	job_status.filp = private;
	job_status.status.status = MM_JOB_STATUS_INVALID;
	job_status.status.id = 0;
	il.status = &job_status;
	il.from = NULL;
	il.to = mm_common_alloc_job(private);
/*	pr_err("++ %d",current->pid);*/
/*	pr_err("waiting job %x %x",il.to,il.status);*/
	SCHEDULE_INTERLOCK_WORK(il);

#if 1
	wait_event(mm_queue, job_status.status.status != MM_JOB_STATUS_INVALID);
#else
	if (wait_event_interruptible(mm_queue, job_status.status.status \
					!= MM_JOB_STATUS_INVALID)) {
		/*Task interrupted... Ensure to remove from the waitlist*/
		pr_err("Task interrupted");
		SCHEDULE_INTERLOCK_WORK(il);
		}
#endif
/*	pr_err("-- %d",current->pid);*/
	return 0;
}

static long mm_file_ioctl(struct file *filp, \
			unsigned int cmd, \
			unsigned long arg)
{
	int ret = 0;
	struct file_private_data *private = filp->private_data;
	struct mm_common *common = private->common;

	if ((_IOC_TYPE(cmd) != MM_DEV_MAGIC) || (_IOC_NR(cmd) > MM_CMD_LAST))
		return -ENOTTY;

	if (_IOC_DIR(cmd) & _IOC_READ)
		ret = !access_ok(VERIFY_WRITE, (void *)arg, _IOC_SIZE(cmd));

	if (_IOC_DIR(cmd) & _IOC_WRITE)
		ret |= !access_ok(VERIFY_READ, (void *)arg, _IOC_SIZE(cmd));

	if (ret) {
		pr_err("ioctl[0x%08x]  failed[%d]", cmd, ret);
		return -EFAULT;
	}

	switch (cmd) {
	case MM_IOCTL_VERSION_REQ:
		if (common->version_info.version_info_ptr != NULL) {
			mm_version_info_t *user_virsion_info =
					(mm_version_info_t *)arg;
			if (user_virsion_info->size <
					common->version_info.size)
				ret = -EINVAL;
			else
				ret = copy_to_user(
					user_virsion_info->version_info_ptr,
					common->version_info.version_info_ptr,
					common->version_info.size);
		}
	break;
	default:
		pr_err("cmd[0x%08x] not supported", cmd);
		ret = -EINVAL;
		break;
	}
	return ret;
}

static const struct file_operations mm_fops = {
	.open = mm_file_open,
	.release = mm_file_release,
	.llseek = mm_file_lseek,
	.write = mm_file_write,
	.read = mm_file_read,
	.poll = mm_file_poll,
	.fsync = mm_file_fsync,
	.unlocked_ioctl = mm_file_ioctl
};

static bool is_validate_file(struct file *filp)
{
	return (filp->f_op == (&mm_fops));
}

void *mm_fmwk_register(const char *name, const char *clk_name,
						unsigned int count,
						MM_CORE_HW_IFC *core_param,
						MM_DVFS_HW_IFC *dvfs_param,
						MM_PROF_HW_IFC *prof_param)
{
	int ret = 0;
	int i = 0;
	struct mm_common *common = NULL;

	BUG_ON(count >= MAX_ASYMMETRIC_PROC);
	if (name == NULL)
		return NULL;

	common = kmalloc(sizeof(struct mm_common), GFP_KERNEL);
	if (!common)
		return NULL;
	memset(common, 0, sizeof(struct mm_common));

	INIT_LIST_HEAD(&common->device_list);
	common->mm_hw_is_on = 0;
	ATOMIC_INIT_NOTIFIER_HEAD(&common->notifier_head);

	/*get common clock*/
	if (clk_name) {
		common->common_clk = clk_get(NULL, clk_name);
		if (!common->common_clk) {
			pr_err("error get clock %s for %s dev", clk_name, name);
			ret = -EIO;
			}
		}

	common->mm_name = kmalloc(strlen(name)+1, GFP_KERNEL);
	strncpy(common->mm_name, name, strlen(name) + 1);

	common->mdev.minor = MISC_DYNAMIC_MINOR;
	common->mdev.name = common->mm_name;
	common->mdev.fops = &mm_fops;
	common->mdev.parent = NULL;

	ret = misc_register(&common->mdev);
	if (ret) {
		pr_err("failed to register misc device.");
		goto err_register;
	}

	common->debugfs_dir = debugfs_create_dir(common->mm_name, NULL);
	if (!common->debugfs_dir) {
		pr_err("Error %ld creating debugfs dir for %s",
		PTR_ERR(common->debugfs_dir), common->mm_name);
		goto err_register;
	}

	for (i = 0; i < count; i++) {
		common->mm_core[i] = mm_core_init(common, name, &core_param[i]);
		if (common->mm_core[i] == NULL) {
			pr_err("Error creating Core instance for core-%d in %s",
							   i, common->mm_name);
			goto err_register;
			}
		}

#ifdef CONFIG_KONA_PI_MGR
	common->mm_dvfs = mm_dvfs_init(common, name, dvfs_param);
#else
	common->mm_dvfs = NULL;
#endif
	common->mm_prof = mm_prof_init(common, name, prof_param);

	mutex_lock(&mm_fmwk_mutex);
	if (single_wq == NULL) {
		init_waitqueue_head(&mm_queue);
		single_wq = alloc_workqueue(single_wq_name,
				WQ_NON_REENTRANT, 1);
		if (single_wq == NULL) {
			mutex_unlock(&mm_fmwk_mutex);
			goto err_register;
			}
		}
	common->single_wq = single_wq;
	list_add_tail(&common->device_list, &mm_dev_list);
	mutex_unlock(&mm_fmwk_mutex);

	return common;

err_register:
	pr_err("Error in dev_init for %s", name);
	mm_fmwk_unregister(common);
	return NULL;
}

void mm_fmwk_unregister(void *dev_name)
{
	struct mm_common *common = NULL;
	struct mm_common *temp = NULL;
	bool found = false;
	int i;

	mutex_lock(&mm_fmwk_mutex);
	list_for_each_entry_safe(common, temp, &mm_dev_list, device_list) {
		if (common == dev_name) {
			list_del_init(&common->device_list);
			found = true;
			break;
			}
		}
	mutex_unlock(&mm_fmwk_mutex);

	if (common->mm_prof)
		mm_prof_exit(common->mm_prof);
#ifdef CONFIG_KONA_PI_MGR
	if (common->mm_dvfs)
		mm_dvfs_exit(common->mm_dvfs);
#else
	common->mm_dvfs = NULL;
#endif
	for (i = 0; \
		(i < MAX_ASYMMETRIC_PROC) && (common->mm_core[i] != NULL); \
		i++)
		mm_core_exit(common->mm_core[i]);
	if (common->debugfs_dir)
		debugfs_remove_recursive(common->debugfs_dir);

	misc_deregister(&common->mdev);

	common->single_wq = NULL;
		kfree(common->mm_name);
		kfree(common);
}
