#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/kernel.h>	/* printk(), min() */
#include <linux/slab.h>		/* kmalloc() */
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/fs.h>		/* everything... */
#include <linux/proc_fs.h>
#include <linux/errno.h>	/* error codes */
#include <linux/types.h>	/* size_t */
#include <linux/fcntl.h>
#include <linux/poll.h>
#include <linux/cdev.h>
#include <linux/seq_file.h>
#include <asm/uaccess.h>
#include <linux/init.h>
#include <multipipe.h>
MODULE_LICENSE("DUAL BSD/GPL")

struct multipipe {
	wait_queue_head_t inq, outq;
	char *buffer, *end;
	int buffersize;
	struct mutex mutex;
	struct cdev cdev; 
}
static int multipipe_devs = NR_DEVS;
int multipipe_buffer =  MULTIPIPE_BUFFER;
static struct multipipe *multipipe_devices;

static int open(struct inode *inode, struct file *filp)
{
	struct multipipe *dev;
	
	dev = container_of(inode->i_cdev, struct multipipe, cdev);
	filp->private_data = dev;
	
	if (mutex_lock_interruptible(&dev->mutex))
		return -ERESTARTSYS;
	if (!dev->buffer) {
		dev->buffer = kmalloc(multipipe_buffer, GFP_KERNEL);
		if (!dev->buffer) {
			mutex_unlock(&dev->mutex);
			return -ENOMEM;
		}
	}
	dev->buffersize = multipipe_buffer;
	dev->end = dev->buffer + dev->buffersize;
	dev->rp = dev->wp = dev->buffer	
	
	if (filp->f_mode & FMODE_READ)
		dev->nreaders++;
	if (filp->f_mode & FMODE_WRITE)
		dev->nwriters++;
	mutex_unlock(&dev->mutex);

	return nonseekable_open(inode, filp);
}

static int release(struct inode *inode, struct file *filp)
{
	struct multipipe *dev = filp->private_data;
	mutex_lock(&dev->mutex);
	if (filp->f_mode & FMODE_READ)
		dev->nreaders--;
	if (filp->f_mode & FMODE_WRITE)
		dev->nwriters--;
	if (dev->nreaders + dev->nwriters == 0) {
		kfree(dev->buffer);
		dev->buffer = NULL;
	}
	mutex_unlock(&dev->mutex);
	return 0;
}

static ssize_t read (struct file *filp, char __user *buf, size_t count, loff_t *f_pos)
{
	struct multipipe *dev = filp->private_data;

	if (mutex_lock_interruptible(&dev->mutex))
		return -ERESTARTSYS;

	while (dev->rp == dev->wp) { 
		mutex_unlock(&dev->mutex); 
		if (filp->f_flags & O_NONBLOCK)
			return -EAGAIN;
		PDEBUG("\"%s\" reading: going to sleep\n", current->comm);
		if (wait_event_interruptible(dev->inq, (dev->rp != dev->wp)))
			return -ERESTARTSYS; /* signal: tell the fs layer to handle it */
		/* otherwise loop, but first reacquire the lock */
		if (mutex_lock_interruptible(&dev->mutex))
			return -ERESTARTSYS;
	}
	if (dev->wp > dev->rp)
		count = min(count, (size_t)(dev->wp - dev->rp));
	else 
		count = min(count, (size_t)(dev->end - dev->rp));
	if (copy_to_user(buf, dev->rp, count)) {
		mutex_unlock (&dev->mutex);
		return -EFAULT;
	}
	dev->rp += count;
	if (dev->rp == dev->end)
		dev->rp = dev->buffer; /* wrapped */
	mutex_unlock (&dev->mutex);

	/* finally, awake any writers and return */
	wake_up_interruptible(&dev->outq);
	PDEBUG("\"%s\" did read %li bytes\n",current->comm, (long)count);
	return count;
}


static ssize_t write(struct file *filp, const char __user *buf, size_t count, loff_t *f_pos)
{
	struct multipipe *dev = filp->private_data;
	int result;

	if (mutex_lock_interruptible(&dev->mutex))
		return -ERESTARTSYS;

	/* Make sure there's space to write */
	result = getwritespace(dev, filp);
	if (result)
		return result; 
	count = min(count, (size_t)spacefree(dev));
	if (dev->wp >= dev->rp)
		count = min(count, (size_t)(dev->end - dev->wp)); 
	else /* the write pointer has wrapped, fill up to rp-1 */
		count = min(count, (size_t)(dev->rp - dev->wp - 1));
	PDEBUG("Going to accept %li bytes to %p from %p\n", (long)count, dev->wp, buf);
	if (copy_from_user(dev->wp, buf, count)) {
		mutex_unlock (&dev->mutex);
		return -EFAULT;
	}
	dev->wp += count;
	if (dev->wp == dev->end)
		dev->wp = dev->buffer; /* wrapped */
	mutex_unlock(&dev->mutex);

	/* finally, awake any reader */
	wake_up_interruptible(&dev->inq);  /* blocked in read() and select() */

	/* and signal asynchronous readers, explained late in chapter 5 */
	if (dev->async_queue)
		kill_fasync(&dev->async_queue, SIGIO, POLL_IN);
	PDEBUG("\"%s\" did write %li bytes\n",current->comm, (long)count);
	return count;
}

static int getwritespace(struct scull_pipe *dev, struct file *filp)
{
	while (spacefree(dev) == 0) { 
		DEFINE_WAIT(wait);
		mutex_unlock(&dev->mutex);
		if (filp->f_flags & O_NONBLOCK)
			return -EAGAIN;
		PDEBUG("\"%s\" writing: going to sleep\n",current->comm);
		prepare_to_wait(&dev->outq, &wait, TASK_INTERRUPTIBLE);
		if (spacefree(dev) == 0)
			schedule();
		finish_wait(&dev->outq, &wait);
		if (signal_pending(current))
			return -ERESTARTSYS; /* signal: tell the fs layer to handle it */
		if (mutex_lock_interruptible(&dev->mutex))
			return -ERESTARTSYS;
	}
	return 0;
}	

/* How much space is free? */
static int spacefree(struct scull_pipe *dev)
{
	if (dev->rp == dev->wp)
		return dev->buffersize - 1;
	return ((dev->rp + dev->buffersize - dev->wp) % dev->buffersize) - 1;
}

static unsigned int poll(struct file *filp, poll_table *wait)
{
	struct multipipe *dev = filp->private_data;
	unsigned int mask = 0;

	/*
	 * The buffer is circular; it is considered full
	 * if "wp" is right behind "rp" and empty if the
	 * two are equal.
	 */
	mutex_lock(&dev->mutex);
	poll_wait(filp, &dev->inq,  wait);
	poll_wait(filp, &dev->outq, wait);
	if (dev->rp != dev->wp)
		mask |= POLLIN | POLLRDNORM;	/* readable */
	if (spacefree(dev))
		mask |= POLLOUT | POLLWRNORM;	/* writable */
	mutex_unlock(&dev->mutex);
	return mask;
}


static int fasync(int fd, struct file *filp, int mode)
{
	struct multipipe *dev = filp->private_data;

	return fasync_helper(fd, filp, mode, &dev->async_queue);
}
struct file_operations multipipe_fops = {
	.owner =	THIS_MODULE,
	.llseek =	no_llseek,
	.read =		read,
	.write =	write,
	.poll =		poll,
	.unlocked_ioctl =	ioctl,
	.open =		open,
	.release =	release,
	.fasync =	fasync,
};

static void setup_cdev(struct multipipe *dev, int index)
{
	int err, devno = multipipe_devno + index;
    
	cdev_init(&dev->cdev, &scull_pipe_fops);
	dev->cdev.owner = THIS_MODULE;
	err = cdev_add (&dev->cdev, devno, 1);
	if (err)
		printk(KERN_NOTICE "Error %d adding multipipe%d", err, index);
}

int init(dev_t firstdev)
{
	int i, result;

	result = register_chrdev_region(firstdev, nr_devs, "multipipe");
	if (result < 0) {
		printk(KERN_NOTICE "Unable to get multipipe region %d\n", result);
		return 0;
	}
	multipipe_devno = firstdev;
	multipipe_devices = kmalloc(nr_devs * sizeof(struct multipipe), GFP_KERNEL);
	if (multipipe_devices == NULL) {
		unregister_chrdev_region(firstdev, nr_devs);
		return 0;
	}
	memset(multipipe_devices, 0, nr_devs * sizeof(struct multipipe));
	for (i = 0; i < nr_devs; i++) {
		init_waitqueue_head(&(multipipe_devices[i].inq));
		init_waitqueue_head(&(multipipe_devices[i].outq));
		mutex_init(&multipipe_devices[i].mutex);
		setup_cdev(multipipe_devices + i, i);
	}
	return nr_devs;
}

void scull_p_cleanup(void)
{
	int i;
	if (!multipipe_devices)
		return; /* nothing else to release */

	for (i = 0; i < scull_p_nr_devs; i++) {
		cdev_del(&multipipe_devices[i].cdev);
		kfree(multipipe_devices[i].buffer);
	}
	kfree(multipipe_devices);
	unregister_chrdev_region(scull_p_devno, scull_p_nr_devs);
	multipipe_devices = NULL; 
}
//allocating major number
result = alloc_chrdev_region(&dev, scull_minor, scull_nr_devs,"multipipe");
scull_major = MAJOR(dev);
if (result < 0) {
printk(KERN_WARNING "scull: can't get major %d\n", scull_major);
return result;



