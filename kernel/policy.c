// Project 1C: scheduling policies.
//
// This is the file you modify. The scheduler framework in proc.c
// calls the functions below; it already handles locking and the
// context switch, so a policy only decides which process runs next
// and when the running process should be preempted.
//
// Each policy has an ID. Select one at run time with the
// set_scheduler(id) system call (for example: wl 2 1). To try a
// different setting, such as another quantum, give it its own ID.
//
// Rules for policy code (Section 2.2 of the project document):
//  1. The system has one CPU. These functions run with interrupts off
//     and must not sleep or acquire p->lock (the framework holds the
//     locks it needs).
//  2. policy_pick_next_proc() must only return a RUNNABLE process, or
//     0 if there is none.
//  3. A scheduler meant to be fair must not starve a job: every ready
//     job eventually runs. (Strict priority, ID 2, is the one
//     deliberate exception.)
//  4. A scheduler must not recognize a workload: do not read
//     p->wl_slot, workload IDs, process names, or pids to decide how
//     to treat a process.
//  5. policy_init(id) may be called more than once for the same ID,
//     including at the start of every workload run. Each call must
//     reset all of the scheduler's global state.
//  6. Do not modify the protected files (kernel/workload.c,
//     kernel/workload.h, user/wl.c).

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

#define SCHED_RR     0 // round robin, 10-tick (100 ms) quantum: stock xv6
#define SCHED_RR1    1 // round robin, 1-tick (10 ms) quantum
#define SCHED_PRIO   2 // strict priority by nice (Project 1B), 10-tick quantum
#define SCHED_MLFQ   3 // multi-level feedback queue
#define SCHED_CFS    4 // weighted virtual-runtime fair share
#define SCHED_CFS_MIN4 5 // CFS-like fair share with 4-tick minimum slice
#define SCHED_RR5    6 // Feature 7: round robin, 5-tick quantum
#define SCHED_MLFQ_B30 10 // MLFQ with 30-tick boost interval
#define SCHED_MLFQ_Q1_5 11 // MLFQ with 5-tick Q1 quantum
#define SCHED_MLFQ_Q0_30 12 // MLFQ with 30-tick Q0 quantum

#define MLFQ_LEVELS 3
#define MLFQ_Q0 0
#define MLFQ_Q1 1
#define MLFQ_Q2 2
#define MLFQ_DEFAULT_BOOST_INTERVAL 60

// Linux CFS weights for nice -20 .. 19, indexed by nice + 20
// (nice 0 has weight 1024). This is the same table user/wl.c uses
// for the fairness metric. For example, a fair-share scheduler can
// advance a process's vrt by (1 << 24) / nice_weight[p->nice + 20]
// for each tick it runs.
const int nice_weight[40] = {
  88761, 71755, 56483, 46273, 36291, 29154, 23254, 18705, 14949, 11916,
  9548,  7620,  6100,  4904,  3906,  3121,  2501,  1991,  1586,  1277,
  1024,  820,   655,   526,   423,   335,   272,   215,   172,   137,
  110,   87,    70,    56,    45,    36,    29,    23,    18,    15,
};

static int quantum = 10; // ticks a process may run before preemption
static int last = -1;    // index in proc[] of the last process chosen
static int cfs_last = -1;
static int cfs_resetting;
static int cfs_min_slice = 1;
static int mlfq_last[MLFQ_LEVELS];
static uint mlfq_boost_tick;

static int mlfq_boost_interval = MLFQ_DEFAULT_BOOST_INTERVAL;
static int mlfq_quantum[MLFQ_LEVELS] = {15, 10, 2};
static const int mlfq_allotment[MLFQ_LEVELS] = {0, 10, 2};

static int
nice_level(int nice)
{
  if (nice <= -10)
    return MLFQ_Q2;
  if (nice <= 10)
    return MLFQ_Q1;
  return MLFQ_Q0;
}

static int
is_mlfq_policy(int id)
{
  return id == SCHED_MLFQ || id == SCHED_MLFQ_B30 ||
         id == SCHED_MLFQ_Q1_5 || id == SCHED_MLFQ_Q0_30;
}

static void
set_level(struct proc *p, int level)
{
  int old = p->level;

  if (old == level)
    return;
  p->level = level;
  if (logging)
    printk("level %d %d->%d at %d\n", p->pid, old, level, ticks);
}

static int
mlfq_boost(void)
{
  int boosted = 0;

  while ((uint)(ticks - mlfq_boost_tick) >= mlfq_boost_interval) {
    mlfq_boost_tick += mlfq_boost_interval;
    for (struct proc *p = proc; p < &proc[NPROC]; p++) {
      if (p->state == UNUSED)
        continue;
      set_level(p, p->start_level > MLFQ_Q1 ? p->start_level : MLFQ_Q1);
      p->used = 0;
    }
    boosted = 1;
  }
  return boosted;
}

static struct proc *
pick_mlfq(void)
{
  mlfq_boost();
  for (int level = MLFQ_Q2; level >= MLFQ_Q0; level--) {
    for (int i = 1; i <= NPROC; i++) {
      struct proc *p = &proc[(mlfq_last[level] + i) % NPROC];
      if (p->state == RUNNABLE && p->level == level) {
        mlfq_last[level] = p - proc;
        return p;
      }
    }
  }
  return 0;
}

static int
higher_level_runnable(struct proc *p)
{
  for (struct proc *q = proc; q < &proc[NPROC]; q++)
    if (q->state == RUNNABLE && q->level > p->level)
      return 1;
  return 0;
}

static int
min_active_vruntime(struct proc *exclude, uint64 *min)
{
  int found = 0;

  for (struct proc *p = proc; p < &proc[NPROC]; p++) {
    if (p == exclude ||
        (p->state != RUNNING && p->state != RUNNABLE))
      continue;
    if (!found || p->vrt < *min) {
      *min = p->vrt;
      found = 1;
    }
  }
  return found;
}

static struct proc *
pick_cfs(void)
{
  struct proc *best = 0;

  cfs_resetting = 0;
  for (int i = 1; i <= NPROC; i++) {
    struct proc *p = &proc[(cfs_last + i) % NPROC];
    if (p->state == RUNNABLE &&
        (best == 0 || p->vrt < best->vrt))
      best = p;
  }
  if (best)
    cfs_last = best - proc;
  return best;
}

// Called by set_scheduler(id), and again when a workload starts.
// Set up global state for policy id.
// Return 0 if id is a known policy, -1 otherwise.
int
policy_init(int id)
{
  switch (id) {
  case SCHED_RR:
  case SCHED_PRIO:
    quantum = 10;
    break;
  case SCHED_MLFQ:
  case SCHED_MLFQ_B30:
  case SCHED_MLFQ_Q1_5:
  case SCHED_MLFQ_Q0_30:
    quantum = 10;
    mlfq_boost_interval = MLFQ_DEFAULT_BOOST_INTERVAL;
    mlfq_quantum[MLFQ_Q0] = 15;
    mlfq_quantum[MLFQ_Q1] = 10;
    mlfq_quantum[MLFQ_Q2] = 2;
    if (id == SCHED_MLFQ_B30)
      mlfq_boost_interval = 30;
    else if (id == SCHED_MLFQ_Q1_5)
      mlfq_quantum[MLFQ_Q1] = 5;
    else if (id == SCHED_MLFQ_Q0_30)
      mlfq_quantum[MLFQ_Q0] = 30;
    break;
  case SCHED_CFS:
    quantum = 1;
    break;
  case SCHED_CFS_MIN4:
    quantum = 1;
    break;
  case SCHED_RR1:
    quantum = 1;
    break;
  case SCHED_RR5:
    quantum = 5;
    break;
  default:
    return -1;
  }
  last = -1;
  cfs_last = -1;
  cfs_min_slice = id == SCHED_CFS_MIN4 ? 4 : 1;
  cfs_resetting = id == SCHED_CFS || id == SCHED_CFS_MIN4;
  for (int i = 0; i < MLFQ_LEVELS; i++)
    mlfq_last[i] = -1;
  mlfq_boost_tick = ticks;
  return 0;
}

// Called when a process is created, and for every process when the
// policy changes or a workload starts. Initialize any per-process
// policy fields.
void
policy_proc_init(struct proc *p)
{
  p->qticks = 0;
  // Level 0 is the lowest queue (Q0); Q2 is the highest. An MLFQ must
  // set the starting level from p->nice here: workload_start() calls
  // this for every process after the jobs' nice values are set.
  p->start_level = nice_level(p->nice);
  p->level = p->start_level;
  p->used = 0;
  p->vrt = 0;
    if ((sched_current == SCHED_CFS ||
      sched_current == SCHED_CFS_MIN4) && !cfs_resetting) {
    uint64 min;
    if (min_active_vruntime(p, &min) && p->vrt < min)
      p->vrt = min;
  }
}

// Round robin: the next RUNNABLE process after the last one chosen,
// scanning proc[] in a circle so no process is starved.
static struct proc *
pick_rr(void)
{
  for (int i = 1; i <= NPROC; i++) {
    struct proc *p = &proc[(last + i) % NPROC];
    if (p->state == RUNNABLE) {
      last = p - proc;
      return p;
    }
  }
  return 0;
}

// Strict priority: a RUNNABLE process with the lowest nice value;
// processes with equal nice take turns in round-robin order.
static struct proc *
pick_prio(void)
{
  int best = 100;

  for (struct proc *p = proc; p < &proc[NPROC]; p++)
    if (p->state == RUNNABLE && p->nice < best)
      best = p->nice;
  for (int i = 1; i <= NPROC; i++) {
    struct proc *p = &proc[(last + i) % NPROC];
    if (p->state == RUNNABLE && p->nice == best) {
      last = p - proc;
      return p;
    }
  }
  return 0;
}

// Choose the next process to run, or return 0 if none is RUNNABLE.
struct proc *
policy_pick_next_proc(void)
{
  switch (sched_current) {
  case SCHED_CFS:
  case SCHED_CFS_MIN4:
    return pick_cfs();
  case SCHED_MLFQ:
  case SCHED_MLFQ_B30:
  case SCHED_MLFQ_Q1_5:
  case SCHED_MLFQ_Q0_30:
    return pick_mlfq();
  case SCHED_PRIO:
    return pick_prio();
  default:
    return pick_rr();
  }
}

// Called just before the framework switches to p.
void
policy_dispatch(struct proc *p)
{
  p->qticks = 0; // a new quantum starts
}

// Called on every timer tick while p is running.
// Return 1 to preempt p, or 0 to let it keep running.
// A process is charged only for ticks that arrive while it is running.
// A loop of pause(1) wakes at a tick, runs briefly, and sleeps again
// before the next one, so it is never charged and cannot test a rule
// such as an allotment kept across sleeps. A test that is charged must
// keep the CPU across a tick, for example by spinning until uptime()
// changes before each pause(1); workload jobs are charged correctly.
int
policy_tick(struct proc *p)
{
  if (sched_current == SCHED_CFS ||
      sched_current == SCHED_CFS_MIN4) {
    uint64 min;
    uint64 delta = ((uint64)1 << 24) / nice_weight[p->nice + 20];

    p->qticks++;
    p->vrt += delta;
    if (p->qticks < cfs_min_slice)
      return 0;
    return min_active_vruntime(p, &min) && p->vrt >= min;
  }
  if (is_mlfq_policy(sched_current)) {
    p->qticks++;
    p->used++;
    if (mlfq_boost())
      return 1;
    if (p->level > MLFQ_Q0 &&
        p->used >= mlfq_allotment[p->level]) {
      set_level(p, p->level - 1);
      p->used = 0;
      return 1;
    }
    if (p->qticks >= mlfq_quantum[p->level] ||
        higher_level_runnable(p))
      return 1;
    return 0;
  }
  p->qticks++;
  return p->qticks >= quantum;
}

// Called when p wakes from sleep (for example, when its simulated I/O
// or a pause() ends) and becomes RUNNABLE again. Use it to decide how
// a waking process re-enters the policy; for example, a fair-share or
// stride policy may move a process that slept a long time up to the
// current minimum, so it cannot claim the CPU for the time it slept.
void
policy_wakeup(struct proc *p)
{
  if (sched_current == SCHED_CFS ||
      sched_current == SCHED_CFS_MIN4) {
    uint64 min;
    if (min_active_vruntime(p, &min) && p->vrt < min)
      p->vrt = min;
    return;
  }
  if (is_mlfq_policy(sched_current))
    mlfq_boost();
}

// Called whenever p's nice value changes.
void
policy_nice(struct proc *p)
{
  if (is_mlfq_policy(sched_current)) {
    int level;

    mlfq_boost();
    level = nice_level(p->nice);
    if (level != p->start_level) {
      p->start_level = level;
      p->used = 0;
      set_level(p, level);
    }
  }
}
