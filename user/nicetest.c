// Jayden Sorter
#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

static void
check(char *label, int actual, int expected)
{
  printf("%s: %d (expected %d)\n", label, actual, expected);
  if (actual != expected)
    exit(1);
}

int
main(void)
{
  int pid = getpid();
  int child;
  int status;
  int logging_child;

  check("default", nice(pid, 0), 0);
  check("positive increment", nice(pid, 5), 5);
  check("negative increment", nice(pid, -8), -3);
  check("upper clamp", nice(pid, 100), 19);
  check("lower clamp", nice(pid, -100), -20);
  check("INT_MAX clamp", nice(pid, 2147483647), 19);
  check("INT_MIN clamp", nice(pid, (-2147483647 - 1)), -20);
  check("invalid pid", nice(999999, 1), 0);

  child = fork();
  if (child < 0) {
    printf("fork failed\n");
    exit(1);
  }
  if (child == 0) {
    check("fork child default", nice(getpid(), 0), 0);
    exit(0);
  }
  if (wait(&status) != child || status != 0) {
    printf("fork child nice test failed\n");
    exit(1);
  }

  printf("logging off probe pid=%d\n", pid);
  check("logging-off update", nice(pid, 1), -19);
  startlogging();
  printf("logging on probes valid pid=%d invalid pid=999999\n", pid);
  logging_child = fork();
  if (logging_child < 0) {
    printf("logging child fork failed\n");
    stopLogging();
    exit(1);
  }
  if (logging_child == 0)
    exit(0);
  check("logging child reaped", wait(&status), logging_child);
  check("logging child status", status, 0);
  check("valid nice zero", nice(pid, 19), 0);
  check("invalid pid", nice(999999, 1), 0);
  stopLogging();
  printf("logging stopped probe pid=%d\n", pid);
  check("logging-stopped update", nice(pid, 1), 1);

  printf("nicetest passed\n");
  exit(0);
}