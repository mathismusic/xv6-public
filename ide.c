// Simple PIO-based (non-DMA) IDE driver code.

#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "x86.h"
#include "traps.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"

#define SECTOR_SIZE   512 // bytes per sector. A block is made of multiple sectors.
#define IDE_BSY       0x80 // busy
#define IDE_DRDY      0x40 // drive ready
#define IDE_DF        0x20 // drive fault
#define IDE_ERR       0x01 // error

#define IDE_CMD_READ  0x20 // read command
#define IDE_CMD_WRITE 0x30 // write command
#define IDE_CMD_RDMUL 0xc4 // read multiple
#define IDE_CMD_WRMUL 0xc5 // write multiple

/* Buf is an in-memory copy of a device block */

/* inb and outb are wrappers for the in and out x86 instructions. */

/*

Control Register:
  Address 0x3F6 = 0x08 (0000 1RE0): R=reset,
                  E=0 means "enable interrupt"
Command Block Registers:
  Address 0x1F0 = Data Port // put data to be written here, find data read here
  Address 0x1F1 = Error
  Address 0x1F2 = Sector Count
  Address 0x1F3 = LBA low byte // LBA is the logical block address - the address of the block on the disk.
  Address 0x1F4 = LBA mid byte
  Address 0x1F5 = LBA hi  byte
  Address 0x1F6 = 1B1D TOP4LBA: B=LBA, D=drive
  Address 0x1F7 = Command/status

Status Register (Address 0x1F7): 76543210
   BUSY  READY FAULT SEEK  DRQ  CORR IDDEX ERROR

Error Register (Address 0x1F1): (check when ERROR==1) 
    7     6     5     4     3     2     1   0
   BBK    UNC   MC   IDNF  MCR  ABRT T0NF AMNF

   BBK  = Bad Block
   UNC  = Uncorrectable data error
   MC   = Media Changed
   IDNF = ID mark Not Found
   MCR  = Media Change Requested
   ABRT = Command aborted
   T0NF = Track 0 Not Found
   AMNF = Address Mark Not Found

*/

// idequeue points to the buf now being read/written to the disk.
// idequeue->qnext points to the next buf to be processed.
// You must hold idelock while manipulating queue - since the queue is shared across CPUs

static struct spinlock idelock; // lock on the queue
static struct buf *idequeue; // queue of requests required to be written to disk

static int havedisk1;
static void idestart(struct buf*);

// Wait for IDE disk to become ready.
static int
idewait(int checkerr)
{
  int r; // to store status

  // keep spinning while busy and not ready. If busy but still ready, we can proceed.
  while(((r = inb(0x1f7)) & (IDE_BSY|IDE_DRDY)) != IDE_DRDY)
    ;

  // check for errors if checkerr is set
  if(checkerr && (r & (IDE_DF|IDE_ERR)) != 0)
    return -1;

  // ready to process a request  
  return 0;
}

void
ideinit(void) // initialize the disk
{
  int i;

  initlock(&idelock, "ide"); // set up spinlock for the queue
  ioapicenable(IRQ_IDE, ncpu - 1); // enable interrupts from the disk as interrupt number IRQ_IDE
  idewait(0); // wait for disk to be ready (don't check for errors)

  // Check if disk 1 is present (that is, there is a second disk)
  outb(0x1f6, 0xe0 | (1<<4)); /* syntax of outb: void outb(int port, int data) - write data to port */

  // wait for the disk to respond on port 0x1f7 whether it is present
  for(i=0; i<1000; i++){
    if(inb(0x1f7) != 0){ // if the disk is present, inb(0x1f7) will return a non-zero value
      havedisk1 = 1;
      break;
    }
  }

  // Switch back to disk 0 once done
  outb(0x1f6, 0xe0 | (0<<4)); // write to switch back to disk 0
}

// Start the request for b. Caller must hold idelock.
static void
idestart(struct buf *b)
{
  if(b == 0)
    panic("idestart");
  if(b->blockno >= FSSIZE)
    panic("incorrect blockno");

  int sector_per_block =  BSIZE/SECTOR_SIZE;
  int sector = b->blockno * sector_per_block;
  int read_cmd = (sector_per_block == 1) ? IDE_CMD_READ :  IDE_CMD_RDMUL; // read multiple sectors if sector_per_block > 1
  int write_cmd = (sector_per_block == 1) ? IDE_CMD_WRITE : IDE_CMD_WRMUL;

  if (sector_per_block > 7) panic("idestart"); // can't handle more than 7 sectors per block

  idewait(0); // wait till ready

  // interact with disk (PIO so in and out instructions), setting up registers for request b.
  outb(0x3f6, 0);  // enable interrupts
  outb(0x1f2, sector_per_block);  // number of sectors
  outb(0x1f3, sector & 0xff); // low byte of LBA = sector number
  outb(0x1f4, (sector >> 8) & 0xff);
  outb(0x1f5, (sector >> 16) & 0xff);
  outb(0x1f6, 0xe0 | ((b->dev&1)<<4) | ((sector>>24)&0x0f));

  // if this is a write
  if(b->flags & B_DIRTY){
    outb(0x1f7, write_cmd);
    outsl(0x1f0, b->data, BSIZE/4); // put data to be written in the data port as a string of 4B values - so BSIZE/4 values, which can be found at address b->data in memory
  } else {
    outb(0x1f7, read_cmd); // simply read sector (or sectors). The data will be available at the data port.
  }
}

// Interrupt handler. called in trap.c when IRQ_IDE is triggered.
void
ideintr(void)
{
  struct buf *b;

  // First queued buffer is the active request.
  acquire(&idelock);

  // nothing to do, say ok and return
  if((b = idequeue) == 0){
    release(&idelock);
    return;
  }
  // update the queue, the current request is done (current request is still at b btw)
  idequeue = b->qnext;

  // Read data if needed.
  if(!(b->flags & B_DIRTY) && idewait(1) >= 0) // if the request is a read, and there are no errors
    insl(0x1f0, b->data, BSIZE/4); // read from data port into b->data

  b->flags |= B_VALID; // set the valid flag
  b->flags &= ~B_DIRTY; // not dirty yet

  // Wake process waiting for this buf.
  wakeup(b);

  // Start disk on next buf in queue if job exists. Cool.
  if(idequeue != 0)
    idestart(idequeue);

  release(&idelock);
}

//PAGEBREAK!
// Sync buf with disk.
// If B_DIRTY is set, write buf to disk, clear B_DIRTY, set B_VALID.
// Else if B_VALID is not set, read buf from disk, set B_VALID.
void
iderw(struct buf *b)
{
  struct buf **pp;

  if(!holdingsleep(&b->lock))
    panic("iderw: buf not locked");
  // if valid and not dirty, we have nothing to sync at all
  if((b->flags & (B_VALID|B_DIRTY)) == B_VALID)
    panic("iderw: nothing to do");
  // can't sync with nonexistent disk
  if(b->dev != 0 && !havedisk1)
    panic("iderw: ide disk 1 not present");

  acquire(&idelock);  //DOC:acquire-lock

  // Append b to idequeue at the end
  b->qnext = 0;
  for(pp=&idequeue; *pp; pp=&(*pp)->qnext)  //DOC:insert-queue
    ;
  *pp = b;

  // Start disk if necessary (queue was empty, we had nothing to do, now we do).
  if(idequeue == b)
    idestart(b);

  // Wait for request to finish (not spin but sleep).
  while((b->flags & (B_VALID|B_DIRTY)) != B_VALID){
    sleep(b, &idelock);
  }

  release(&idelock);
}
