// Jayden Sorter
#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

static void
priority_child(int gate_read, int gate_write)
{
  char token;
  int pid;
  int value;
  uint start;
  volatile uint work = 0;

  close(gate_write);
  if (read(gate_read, &token, 1) != 1) {
    printf("priority child: gate read failed\n");
    exit(1);
  }
  close(gate_read);

  pid = getpid();
  value = nice(pid, 0);
  printf("priority child start pid=%d nice=%d\n", pid, value);
  start = uptime();
  while (uptime() - start < 4)
    work++;
  printf("priority child finished pid=%d nice=%d\n", pid, value);
  exit(0);
}

static void
equal_child(int gate_read, int gate_write)
{
  char token;
  int pid;
  uint start;
  volatile uint work = 0;

  close(gate_write);
  if (read(gate_read, &token, 1) != 1) {
    printf("equal child: gate read failed\n");
    exit(1);
  }
  close(gate_read);

  pid = getpid();
  printf("equal child start pid=%d nice=%d\n", pid, nice(pid, 0));
  start = uptime();
  while (uptime() - start < 10)
    work++;
  printf("equal child finished pid=%d\n", pid);
  exit(0);
}

static void
priority_test(void)
{
  int gate[2];
  int plus10;
  int plus5;
  char tokens[2] = {'a', 'b'};

  startlogging();
  if (nice(getpid(), -20) != -20) {
    printf("priority test: failed to set parent nice value\n");
    stopLogging();
    exit(1);
  }
  if (pipe(gate) < 0) {
    printf("priority test: pipe failed\n");
    stopLogging();
    exit(1);
  }

  plus10 = fork();
  if (plus10 < 0) {
    printf("priority test: first fork failed\n");
    stopLogging();
    exit(1);
  }
  if (plus10 == 0)
    priority_child(gate[0], gate[1]);
  if (nice(plus10, 10) != 10) {
    printf("priority test: failed to set +10 child\n");
    stopLogging();
    exit(1);
  }

  plus5 = fork();
  if (plus5 < 0) {
    printf("priority test: second fork failed\n");
    stopLogging();
    exit(1);
  }
  if (plus5 == 0)
    priority_child(gate[0], gate[1]);
  if (nice(plus5, 5) != 5) {
    printf("priority test: failed to set +5 child\n");
    stopLogging();
    exit(1);
  }

  close(gate[0]);
  printf("priority test: both children assigned; releasing gate\n");
  if (write(gate[1], tokens, sizeof(tokens)) != sizeof(tokens)) {
    printf("priority test: gate write failed\n");
    close(gate[1]);
    stopLogging();
    exit(1);
  }
  close(gate[1]);

  wait(0);
  wait(0);
  stopLogging();
  printf("priority test complete; expected order: nice 5, then nice 10\n");
  exit(0);
}

static void
equal_test(void)
{
  int gate[2];
  int first;
  int second;
  char tokens[2] = {'a', 'b'};

  startlogging();
  if (nice(getpid(), -20) != -20 || pipe(gate) < 0) {
    printf("equal test: setup failed\n");
    stopLogging();
    exit(1);
  }

  first = fork();
  if (first < 0) {
    printf("equal test: first fork failed\n");
    stopLogging();
    exit(1);
  }
  if (first == 0)
    equal_child(gate[0], gate[1]);

  second = fork();
  if (second < 0) {
    printf("equal test: second fork failed\n");
    stopLogging();
    exit(1);
  }
  if (second == 0)
    equal_child(gate[0], gate[1]);

  close(gate[0]);
  printf("equal test: children %d and %d assigned; releasing gate\n",
         first, second);
  if (write(gate[1], tokens, sizeof(tokens)) != sizeof(tokens)) {
    printf("equal test: gate write failed\n");
    close(gate[1]);
    stopLogging();
    exit(1);
  }
  close(gate[1]);

  wait(0);
  wait(0);
  stopLogging();
  printf("equal test complete\n");
  exit(0);
}

static void
basic_test(void)
{
  int pid;

  startlogging();
  pid = fork();
  if (pid < 0) {
    stopLogging();
    exit(1);
  }
  if (pid == 0) {
    uint start;
    volatile uint work = 0;

    if (nice(getpid(), 10) != 10) {
      printf("logtest child nice failed\n");
      exit(1);
    }
    start = uptime();
    while (uptime() - start < 4)
      work++;
    printf("logtest child finished pid=%d nice=10\n", getpid());
    exit(0);
  }

  if (wait(0) != pid) {
    printf("logtest wait failed\n");
    stopLogging();
    exit(1);
  }
  stopLogging();
  printf("logtest complete\n");
  exit(0);
}

int
main(int argc, char *argv[])
{
  if (argc > 1 && strcmp(argv[1], "priority") == 0)
    priority_test();
  if (argc > 1 && strcmp(argv[1], "equal") == 0)
    equal_test();
  basic_test();
}