#include "spinlock.h"
#define QUEUESIZE 64
struct cvar {
    struct spinlock mutex; // atomizer
    int cond; // is the condition true or false
    struct proc *waiting[QUEUESIZE]; // queue
    int head, tail; // queue
};