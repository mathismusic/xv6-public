struct file {
  enum { FD_NONE, FD_PIPE, FD_INODE } type; // all data transfer is through some kind of file
  int ref; // reference count
  char readable;
  char writable;
  struct pipe *pipe; // in case it is a pipe.
  struct inode *ip; // pointer to low-level name and details
  uint off; // offset (relevant when open).
};
// note that many copies of the same file may be open with different offsets. Note that it is even possible (and that is the reason for the refcount variable) - for example, on forks or dups - that there could be two file descriptors to the same file struct. In this case, there is just one common offset, and updating any of these fds will change the common offset.


// in-memory copy of an inode
struct inode {
  uint dev;           // Device number
  uint inum;          // Inode number
  int ref;            // Reference count
  struct sleeplock lock; // protects everything below here - all dinode fields
  int valid;          // inode has been read from disk? i.e. is this actually inode #inum?

  short type;         // copy of disk inode
  short major; // major device number (T_DEV only)
  short minor;
  short nlink; // number of (hard) links to inode in file system (the refcount equivalent for inodes)
  uint size;
  uint addrs[NDIRECT+1]; // the subdirectories of the inode - eh? each is the address of a block of the file system
};

// table mapping major device number to
// device functions
struct devsw {
  int (*read)(struct inode*, char*, int);
  int (*write)(struct inode*, char*, int);
};

extern struct devsw devsw[];

#define CONSOLE 1
