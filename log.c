#include "types.h"
#include "defs.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"

// Simple logging that allows concurrent FS system calls.
//
// A log transaction contains the updates of multiple FS system
// calls. The logging system only commits when there are
// no FS system calls active. Thus there is never
// any reasoning required about whether a commit might
// write an uncommitted system call's updates to disk.
//
// A system call should call begin_op()/end_op() to mark
// its start and end. Usually begin_op() just increments
// the count of in-progress FS system calls and returns.
// But if it thinks the log is close to running out, it
// sleeps until the last outstanding end_op() commits.
//
// The log is a physical re-do log containing disk blocks.
// The on-disk log format:
//   header block, containing block #s for block A, B, C, ...
//   block A
//   block B
//   block C
//   ...
// Log appends are synchronous.

// Contents of the header block, used for both the on-disk header block
// and to keep track in memory of logged block #s before commit.
struct logheader {
  int n; // how many blocks in the log
  int block[LOGSIZE]; // block numbers to be committed
};

// the logbook. It is on disk, and this is an in-memory copy of it.
struct log {
  struct spinlock lock; // spinlock for the log
  int start; // block number of first log block in disk
  int size; // number of logged blocks in the log (on disk)
  int outstanding; // how many FS sys calls are executing.
  int committing;  // in commit(), please wait.
  int dev; // log is specific to device ofc
  struct logheader lh; // in-memory copy of header (the header is stored on disk for recovery purposes)
};

struct log log; // global log

static void recover_from_log(void);
static void commit();

void
initlog(int dev)
{
  // logheader must be at most one block big
  if (sizeof(struct logheader) >= BSIZE)
    panic("initlog: too big logheader");

  struct superblock sb; // create superblock struct
  initlock(&log.lock, "log"); // initialize log lock
  readsb(dev, &sb); // read superblock from disk
  // initialize log parameters from superblock
  log.start = sb.logstart;
  log.size = sb.nlog; // number of blocks in the log
  log.dev = dev;
  // recover uncommitted transactions from log
  recover_from_log();
}

// Copy committed blocks from log to their home location
static void
install_trans(void) // install transaction
{
  int tail;

  for (tail = 0; tail < log.lh.n; tail++) {
    struct buf *lbuf = bread(log.dev, log.start+tail+1); // read log block into memory
    struct buf *dbuf = bread(log.dev, log.lh.block[tail]); // read dst block (or create buf for it)
    
    memmove(dbuf->data, lbuf->data, BSIZE);  // copy block to dst - possibly overwriting its content
    
    bwrite(dbuf);  // write dst to disk (which calls iderw which enqueues the write to disk)

    // done using buffers
    brelse(lbuf);
    brelse(dbuf);
  }
}

// Read the log header from disk into the in-memory log header
static void
read_head(void)
{
  struct buf *buf = bread(log.dev, log.start);
  struct logheader *lh = (struct logheader *) (buf->data); // interpret as logheader. Why not log.lh = *(struct logheader *) (buf->data); ?
  int i;
  // copy over
  log.lh.n = lh->n;
  for (i = 0; i < log.lh.n; i++) {
    log.lh.block[i] = lh->block[i];
  }
  brelse(buf);
}

// Write in-memory log header to disk.
// This is the true point at which the
// current transaction commits.
static void
write_head(void)
{
  struct buf *buf = bread(log.dev, log.start);
  struct logheader *hb = (struct logheader *) (buf->data);
  int i;
  // reverse copy
  hb->n = log.lh.n;
  for (i = 0; i < log.lh.n; i++) {
    hb->block[i] = log.lh.block[i];
  }
  bwrite(buf); // push logheader to disk
  brelse(buf);
}

static void
recover_from_log(void)
{
  read_head(); // get logheader
  install_trans(); // if committed, copy from log to disk
  log.lh.n = 0; // clear log in memory
  write_head(); // clear the log on disk
}

// called at the start of each FS system call.
void
begin_op(void)
{
  acquire(&log.lock); // acquire lock on log

  while(1){ // many threads can be in this loop

    // wait for commit to finish before starting new transaction
    if(log.committing){
      sleep(&log, &log.lock);
    } 
    // can we start a new transaction for this new syscall?
    else 
      // no
      if(log.lh.n + (log.outstanding+1)*MAXOPBLOCKS > LOGSIZE){
        // this op might exhaust log space; wait for commit.
        sleep(&log, &log.lock);
      } 
      // yes
      else {
        log.outstanding += 1; // one more syscall
        release(&log.lock); // updated log, release lock
        break;
    }
  }
}

// called at the end of each FS system call.
// commits if this was the last outstanding operation.
void
end_op(void)
{
  int do_commit = 0; // should we commit?

  acquire(&log.lock);
  log.outstanding -= 1; // first up, one syscall less
  if(log.committing)
    panic("log.committing");
  
  // if no more outstanding syscalls still running, commit
  if(log.outstanding == 0){
    do_commit = 1;
    log.committing = 1; // decided to commit, variable part of the log to let the rest of the log know that we are committing now, and to wait for us to finish commiting the whole log
  } else {
    // begin_op() may be waiting for log space,
    // and decrementing log.outstanding has decreased
    // the amount of reserved space.
    wakeup(&log); // wakeup all processes waiting on log
  }
  release(&log.lock);

  // decided to commit
  if(do_commit){
    // call commit w/o holding locks, since not allowed
    // to sleep with locks.
    commit(); // commit the transaction.
    acquire(&log.lock);
    log.committing = 0;
    wakeup(&log); // wake up all processes waiting on log (which we did not previously do, they are all sleeping waiting for the commit to conclude)
    release(&log.lock);
  }
}

// Copy modified blocks from cache to log.
static void
write_log(void)
{
  int tail;

  for (tail = 0; tail < log.lh.n; tail++) {
    struct buf *to = bread(log.dev, log.start+tail+1); // log block
    struct buf *from = bread(log.dev, log.lh.block[tail]); // cache block
    memmove(to->data, from->data, BSIZE);
    bwrite(to);  // write the log
    brelse(from);
    brelse(to);
  }
}

static void
commit()
{
  if (log.lh.n > 0) {
    write_log();     // Write modified blocks from cache to log
    write_head();    // Write header to disk -- the real commit
    /* if the system crashes before completing the above - nothing is in the logheader @log.start. Which means that none of these changes to the blocks will be reflected on disk (whichever ones happened will be on log, but none on disk - that is why we need a log; as a sort of safety against incomplete changes to disk blocks). There is twice the copying yes, but in the interest of safety this is probably worth the extra cost. */
    install_trans(); // Now install writes to home locations on disk - notice that this can crash and the next time the system starts up the log which was saved on disk will be used to recover the changes to the blocks! And then install_trans will run again trying to push these back to disk. see recover_from_log() for the recovery process.
    log.lh.n = 0;
    write_head();    // Erase the transaction from the log
  }
}

// Caller has modified b->data and is done with the buffer.
// Record the block number and pin in the cache with B_DIRTY.
// commit()/write_log() will do the disk write.
//
// log_write() replaces bwrite(); a typical use is:
//   bp = bread(...)
//   modify bp->data[]
//   log_write(bp)
//   brelse(bp)
void
log_write(struct buf *b)
{
  int i;

  // can't do such a big transaction
  if (log.lh.n >= LOGSIZE || log.lh.n >= log.size - 1)
    panic("too big a transaction");
  
  // no outstanding transaction (begin_op was not called), panic.
  if (log.outstanding < 1)
    panic("log_write outside of trans");

  acquire(&log.lock);

  // if this block is already part of the current transaction (0 to log.lh.n - 1), do nothing
  for (i = 0; i < log.lh.n; i++) {
    if (log.lh.block[i] == b->blockno)   // log absorbtion, update the block all you want
      break;
  }
  log.lh.block[i] = b->blockno; // lol, even if we pass the if () above we reassign
  
  // if a new block was added to the log, update log.lh.n
  if (i == log.lh.n)
    log.lh.n++; // < log.size

  b->flags |= B_DIRTY; // needs to be written to disk
  release(&log.lock); // done with log
}

