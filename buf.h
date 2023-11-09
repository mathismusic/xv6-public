struct buf {
  int flags; // valid, dirty
  uint dev;
  uint blockno;
  struct sleeplock lock;
  uint refcnt;
  // doubly linked list
  struct buf *prev; // LRU cache list - for a bit of performance
  struct buf *next;
  struct buf *qnext; // disk queue - what's this?
  uchar data[BSIZE]; // buffer data
};
#define B_VALID 0x2  // buffer has been read from disk
#define B_DIRTY 0x4  // buffer needs to be written to disk

