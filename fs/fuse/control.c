/*
  FUSE: Filesystem in Userspace
  Copyright (C) 2001-2008  Miklos Szeredi <miklos@szeredi.hu>

  This program can be distributed under the terms of the GNU GPL.
  See the file COPYING.
*/

#include "fuse_i.h"

#include <linux/init.h>
#include <linux/module.h>
#include <linux/fs_context.h>

#define FUSE_CTL_SUPER_MAGIC 0x65735543

/*
 * This is non-NULL when the single instance of the control filesystem
 * exists.  Protected by fuse_mutex
 */
static struct super_block *fuse_control_sb;

static struct fuse_mount *fuse_ctl_file_conn_get(struct file *file)
{
	struct fuse_mount *fm;
	mutex_lock(&fuse_mutex);
	fm = file_inode(file)->i_private;
	if (fm)
		fm = fuse_mount_get(fm);
	mutex_unlock(&fuse_mutex);
	return fm;
}

static ssize_t fuse_mount_abort_write(struct file *file, const char __user *buf,
				      size_t count, loff_t *ppos)
{
	struct fuse_mount *fm = fuse_ctl_file_conn_get(file);
	if (fm) {
		if (fm->fc->abort_err)
			fm->fc->aborted = true;
		fuse_abort_conn(fm->fc);
		fuse_mount_put(fm);
	}
	return count;
}

static ssize_t fuse_mount_waiting_read(struct file *file, char __user *buf,
				       size_t len, loff_t *ppos)
{
	char tmp[32];
	size_t size;

	if (!*ppos) {
		long value;
		struct fuse_mount *fm = fuse_ctl_file_conn_get(file);
		if (!fm)
			return 0;

		value = atomic_read(&fm->fc->num_waiting);
		file->private_data = (void *)value;
		fuse_mount_put(fm);
	}
	size = sprintf(tmp, "%ld\n", (long)file->private_data);
	return simple_read_from_buffer(buf, len, ppos, tmp, size);
}

static ssize_t fuse_mount_limit_read(struct file *file, char __user *buf,
				     size_t len, loff_t *ppos, unsigned val)
{
	char tmp[32];
	size_t size = sprintf(tmp, "%u\n", val);

	return simple_read_from_buffer(buf, len, ppos, tmp, size);
}

static ssize_t fuse_mount_limit_write(struct file *file, const char __user *buf,
				      size_t count, loff_t *ppos, unsigned *val,
				      unsigned global_limit)
{
	unsigned long t;
	unsigned limit = (1 << 16) - 1;
	int err;

	if (*ppos)
		return -EINVAL;

	err = kstrtoul_from_user(buf, count, 0, &t);
	if (err)
		return err;

	if (!capable(CAP_SYS_ADMIN))
		limit = min(limit, global_limit);

	if (t > limit)
		return -EINVAL;

	*val = t;

	return count;
}

static ssize_t fuse_mount_max_background_read(struct file *file,
					      char __user *buf, size_t len,
					      loff_t *ppos)
{
	struct fuse_mount *fm;
	unsigned val;

	fm = fuse_ctl_file_conn_get(file);
	if (!fm)
		return 0;

	val = READ_ONCE(fm->fc->max_background);
	fuse_mount_put(fm);

	return fuse_mount_limit_read(file, buf, len, ppos, val);
}

static ssize_t fuse_mount_max_background_write(struct file *file,
					       const char __user *buf,
					       size_t count, loff_t *ppos)
{
	unsigned uninitialized_var(val);
	ssize_t ret;

	ret = fuse_mount_limit_write(file, buf, count, ppos, &val,
				    max_user_bgreq);
	if (ret > 0) {
		struct fuse_mount *fm = fuse_ctl_file_conn_get(file);
		if (fm) {
			struct fuse_conn *fc = fm->fc;

			spin_lock(&fc->bg_lock);
			fc->max_background = val;
			fc->blocked = fc->num_background >= fc->max_background;
			if (!fc->blocked)
				wake_up(&fc->blocked_waitq);
			spin_unlock(&fc->bg_lock);
			fuse_mount_put(fm);
		}
	}

	return ret;
}

static ssize_t fuse_mount_congestion_threshold_read(struct file *file,
						    char __user *buf, size_t len,
						    loff_t *ppos)
{
	struct fuse_mount *fm;
	unsigned val;

	fm = fuse_ctl_file_conn_get(file);
	if (!fm)
		return 0;

	val = READ_ONCE(fm->fc->congestion_threshold);
	fuse_mount_put(fm);

	return fuse_mount_limit_read(file, buf, len, ppos, val);
}

static ssize_t fuse_mount_congestion_threshold_write(struct file *file,
						     const char __user *buf,
						     size_t count, loff_t *ppos)
{
	unsigned uninitialized_var(val);
	struct fuse_mount *fm;
	struct fuse_conn *fc;
	ssize_t ret;

	ret = fuse_mount_limit_write(file, buf, count, ppos, &val,
				    max_user_congthresh);
	if (ret <= 0)
		goto out;
	fm = fuse_ctl_file_conn_get(file);
	if (!fm)
		goto out;
	fc = fm->fc;

	spin_lock(&fc->bg_lock);
	fc->congestion_threshold = val;
	if (fm->sb) {
		if (fc->num_background < fc->congestion_threshold) {
			clear_bdi_congested(fm->sb->s_bdi, BLK_RW_SYNC);
			clear_bdi_congested(fm->sb->s_bdi, BLK_RW_ASYNC);
		} else {
			set_bdi_congested(fm->sb->s_bdi, BLK_RW_SYNC);
			set_bdi_congested(fm->sb->s_bdi, BLK_RW_ASYNC);
		}
	}
	spin_unlock(&fc->bg_lock);
	fuse_mount_put(fm);
out:
	return ret;
}

static const struct file_operations fuse_ctl_abort_ops = {
	.open = nonseekable_open,
	.write = fuse_mount_abort_write,
	.llseek = no_llseek,
};

static const struct file_operations fuse_ctl_waiting_ops = {
	.open = nonseekable_open,
	.read = fuse_mount_waiting_read,
	.llseek = no_llseek,
};

static const struct file_operations fuse_mount_max_background_ops = {
	.open = nonseekable_open,
	.read = fuse_mount_max_background_read,
	.write = fuse_mount_max_background_write,
	.llseek = no_llseek,
};

static const struct file_operations fuse_mount_congestion_threshold_ops = {
	.open = nonseekable_open,
	.read = fuse_mount_congestion_threshold_read,
	.write = fuse_mount_congestion_threshold_write,
	.llseek = no_llseek,
};

static struct dentry *fuse_ctl_add_dentry(struct dentry *parent,
					  struct fuse_mount *fm,
					  const char *name,
					  int mode, int nlink,
					  const struct inode_operations *iop,
					  const struct file_operations *fop)
{
	struct dentry *dentry;
	struct inode *inode;

	BUG_ON(fm->ctl_ndents >= FUSE_CTL_NUM_DENTRIES);
	dentry = d_alloc_name(parent, name);
	if (!dentry)
		return NULL;

	inode = new_inode(fuse_control_sb);
	if (!inode) {
		dput(dentry);
		return NULL;
	}

	inode->i_ino = get_next_ino();
	inode->i_mode = mode;
	inode->i_uid = fm->fc->user_id;
	inode->i_gid = fm->fc->group_id;
	inode->i_atime = inode->i_mtime = inode->i_ctime = current_time(inode);
	/* setting ->i_op to NULL is not allowed */
	if (iop)
		inode->i_op = iop;
	inode->i_fop = fop;
	set_nlink(inode, nlink);
	inode->i_private = fm;
	d_add(dentry, inode);

	fm->ctl_dentry[fm->ctl_ndents++] = dentry;

	return dentry;
}

/*
 * Add a connection to the control filesystem (if it exists).  Caller
 * must hold fuse_mutex
 */
int fuse_ctl_add_conn(struct fuse_mount *fm)
{
	struct dentry *parent;
	char name[32];

	if (!fuse_control_sb)
		return 0;

	parent = fuse_control_sb->s_root;
	inc_nlink(d_inode(parent));
	sprintf(name, "%u", fm->dev);
	parent = fuse_ctl_add_dentry(parent, fm, name, S_IFDIR | 0500, 2,
				     &simple_dir_inode_operations,
				     &simple_dir_operations);
	if (!parent)
		goto err;

	if (!fuse_ctl_add_dentry(parent, fm, "waiting", S_IFREG | 0400, 1,
				 NULL, &fuse_ctl_waiting_ops) ||
	    !fuse_ctl_add_dentry(parent, fm, "abort", S_IFREG | 0200, 1,
				 NULL, &fuse_ctl_abort_ops) ||
	    !fuse_ctl_add_dentry(parent, fm, "max_background", S_IFREG | 0600,
				 1, NULL, &fuse_mount_max_background_ops) ||
	    !fuse_ctl_add_dentry(parent, fm, "congestion_threshold",
				 S_IFREG | 0600, 1, NULL,
				 &fuse_mount_congestion_threshold_ops))
		goto err;

	return 0;

 err:
	fuse_ctl_remove_conn(fm);
	return -ENOMEM;
}

/*
 * Remove a connection from the control filesystem (if it exists).
 * Caller must hold fuse_mutex
 */
void fuse_ctl_remove_conn(struct fuse_mount *fm)
{
	int i;

	if (!fuse_control_sb)
		return;

	for (i = fm->ctl_ndents - 1; i >= 0; i--) {
		struct dentry *dentry = fm->ctl_dentry[i];
		d_inode(dentry)->i_private = NULL;
		if (!i) {
			/* Get rid of submounts: */
			d_invalidate(dentry);
		}
		dput(dentry);
	}
	drop_nlink(d_inode(fuse_control_sb->s_root));
}

static int fuse_ctl_fill_super(struct super_block *sb, struct fs_context *fctx)
{
	static const struct tree_descr empty_descr = {""};
	struct fuse_mount *fm;
	int err;

	err = simple_fill_super(sb, FUSE_CTL_SUPER_MAGIC, &empty_descr);
	if (err)
		return err;

	mutex_lock(&fuse_mutex);
	BUG_ON(fuse_control_sb);
	fuse_control_sb = sb;
	list_for_each_entry(fm, &fuse_mount_list, entry) {
		err = fuse_ctl_add_conn(fm);
		if (err) {
			fuse_control_sb = NULL;
			mutex_unlock(&fuse_mutex);
			return err;
		}
	}
	mutex_unlock(&fuse_mutex);

	return 0;
}

static int fuse_ctl_get_tree(struct fs_context *fc)
{
	return get_tree_single(fc, fuse_ctl_fill_super);
}

static const struct fs_context_operations fuse_ctl_context_ops = {
	.get_tree	= fuse_ctl_get_tree,
};

static int fuse_ctl_init_fs_context(struct fs_context *fc)
{
	fc->ops = &fuse_ctl_context_ops;
	return 0;
}

static void fuse_ctl_kill_sb(struct super_block *sb)
{
	struct fuse_mount *fm;

	mutex_lock(&fuse_mutex);
	fuse_control_sb = NULL;
	list_for_each_entry(fm, &fuse_mount_list, entry)
		fm->ctl_ndents = 0;
	mutex_unlock(&fuse_mutex);

	kill_litter_super(sb);
}

static struct file_system_type fuse_ctl_fs_type = {
	.owner		= THIS_MODULE,
	.name		= "fusectl",
	.init_fs_context = fuse_ctl_init_fs_context,
	.kill_sb	= fuse_ctl_kill_sb,
};
MODULE_ALIAS_FS("fusectl");

int __init fuse_ctl_init(void)
{
	return register_filesystem(&fuse_ctl_fs_type);
}

void __exit fuse_ctl_cleanup(void)
{
	unregister_filesystem(&fuse_ctl_fs_type);
}
