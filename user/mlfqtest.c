#include "kernel/types.h"
#include "user/user.h"

static void
charge_ticks(int count)
{
  volatile uint work = 0;

  for (int i = 0; i < count; i++) {
    uint start = uptime();
    while (uptime() == start)
      work++;
    pause(1);
  }
}

static void
allotment_test(void)
{
  int pid = fork();

  if (pid < 0) {
    printf("mlfqtest: fork failed\n");
    exit(1);
  }
  if (pid == 0) {
    pid = getpid();
    charge_ticks(4);
    nice(pid, 5);
    charge_ticks(6);
    exit(0);
  }
  wait(0);
}

static void
nice_change_test(void)
{
  int pid = fork();

  if (pid < 0) {
    printf("mlfqtest: fork failed\n");
    exit(1);
  }
  if (pid == 0) {
    pid = getpid();
    charge_ticks(4);
    nice(pid, -10); // nice -10 starts in Q2; this must reset used to zero.
    charge_ticks(1);
    nice(pid, -1); // nice -11 stays in Q2; this must not reset used.
    charge_ticks(1);
    exit(0);
  }
  wait(0);
}

int
main(void)
{
  if (set_scheduler(3) < 0) {
    printf("mlfqtest: set_scheduler(3) failed\n");
    exit(1);
  }
  printf("mlfqtest: Rule 4 carries 4 Q1 ticks over sleeps and a same-band nice change; 6 more should demote\n");
  printf("mlfqtest: Rule 6 changes Q1 to Q2 after 4 ticks, then checks reset and same-band preservation\n");
  startLogging();
  allotment_test();
  nice_change_test();
  stopLogging();
  printf("mlfqtest complete\n");
  exit(0);
}