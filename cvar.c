#include "types.h"
#include "defs.h"
#include "proc.h"
#include "cvar.h"

void initcvar(struct cvar *cv) {
    initlock(&cv->mutex, "");
    cv->head = cv->tail = 0;
    cv->cond = 1; 
    // may assume only one thread calls init, so no need to use the mutex here.
}

void cvar_wait(struct cvar *cv, struct spinlock *mutex) {
    acquire(&cv->mutex);
    if ((cv->tail - cv->head+QUEUESIZE)%QUEUESIZE == QUEUESIZE-1) {
        release(&cv->mutex);
        return; // queue is full
    }
    if (cv->cond) {
        // add to queue
        cv->waiting[cv->tail++] = myproc();
        cv->tail %= QUEUESIZE;
        sleep(myproc(), &mutex); // sleep till awoken by cvar_signal. Yeah, weird it's sleeping on itself but okay
    } cv->cond = 1;
    release(&cv->mutex);
}

void cvar_signal(struct cvar *cv) {
    acquire(&cv->mutex);
    cv->cond = 0;
    // if empty queue, done
    if (cv->head == cv->tail) {
        release(&cv->mutex);
        return;
    }
    // pop from queue
    struct proc *p = cv->waiting[cv->head++];
    cv->head %= QUEUESIZE;
    wakeup(p);
    release(&cv->mutex);
}

void cvar_broadcast(struct cvar *cv) {
    struct proc *p;
    acquire(&cv->mutex);
    cv->cond = 0;
    while (cv->head != cv->tail) {
        p = cv->waiting[cv->head++];
        cv->head %= QUEUESIZE;
        wakeup(p);
    }
    release(&cv->mutex);
}