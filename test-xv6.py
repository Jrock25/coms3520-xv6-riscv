# // Jayden Sorter
#!/usr/bin/env python3

#
# python script that tests xv6 without having to boot it and type to its shell
#
# ./test-xv6.py usertests  (runs usertests)
# ./test-xv6.py -q usertests (runs the quick tests of usertests)
# ./test-xv6.py crash  (runs the crash tests)
# ./test-xv6.py log (runs the log crash test)

import argparse, os, inspect, re, signal, subprocess, sys, time
from subprocess import run

sys.stdout.reconfigure(line_buffering=True)

parser = argparse.ArgumentParser()
parser.add_argument('testrex', help="test name or regular expression")
parser.add_argument("-q", action='store_true', help="usertests quick")
args = parser.parse_args()

class QEMU(object):

    def __init__(self, reset=False, image_file=None):
        if reset:
            self.build_xv6()
            self.reset_fs()
        if image_file:
            q = ["qemu-system-riscv64", "-machine", "virt", "-bios", "none",
                 "-kernel", "kernel/kernel", "-m", "128M", "-smp", "1",
                 "-nographic", "-global", "virtio-mmio.force-legacy=false",
                 "-drive", f"file={image_file},if=none,format=raw,id=x0",
                 "-device", "virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0"]
        else:
            q = ["make", "qemu"]
        self.proc = subprocess.Popen(q, stdin=subprocess.PIPE,
                                      stdout=subprocess.PIPE,
                                      stderr=subprocess.STDOUT)
        os.set_blocking(self.proc.stdout.fileno(), False)
        self.output = ""
        self.outbytes = bytearray()
        self.reported = 0
        time.sleep(1)

    def reset_fs(self):
        try:
            run(["rm", "-f", "fs.img"], check=True)
            run(["make", "fs.img"], check=True)
        except subprocess.CalledProcessError as e:
            print(f"Command failed with exit code {e.returncode}")

    def build_xv6(self):
        try:
            run(["make", "kernel/kernel"], check=True)
        except subprocess.CalledProcessError as e:
            print(f"Command failed with exit code {e.returncode}")

    def save_output(self, path="test-xv6.out"):
        try:
            with open(path, "w") as f:
                f.write(self.output)
        except OSError as e:
            print("Provided a bad results path. Error:", e)
        
    def cmd(self, c):
        if isinstance(c, str):
            c = c.encode('utf-8')
        self.proc.stdin.write(c)
        self.proc.stdin.flush()

    def wait_for(self, regexp, start=0, timeout=30):
        deadline = time.time() + timeout
        while time.time() < deadline:
            self.read()
            if re.search(regexp, self.output[start:]):
                return self.output[start:]
            if self.proc.poll() is not None:
                break
            time.sleep(0.1)
        self.save_output()
        raise RuntimeError(f"Timed out waiting for {regexp!r}")

    def command(self, command, timeout=30):
        self.read()
        start = len(self.output)
        self.cmd(command + "\n")
        return self.wait_for(r"\n\$ ", start=start, timeout=timeout)
        
    def crash(self):
        ps = run(['ps', '-opid', '--no-headers', '--ppid', str(self.proc.pid)], stdout=subprocess.PIPE, encoding='utf8')
        kids = [int(line) for line in ps.stdout.splitlines()]
        if len(kids) == 0:
            print("no qemu")
            sys.exit(1)
        print("kill", kids[0])
        os.kill(kids[0], signal.SIGKILL)

    def stop(self):
        self.proc.terminate()
        try:
            self.proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait()

    def read(self):
        while True:
            try:
                buf = os.read(self.proc.stdout.fileno(), 4096)
            except BlockingIOError:
                break
            if len(buf) == 0:  # qemu exited
                break
            self.outbytes.extend(buf)
        self.output = self.outbytes.decode("utf-8", "replace")

    def lines(self):
        return self.output.splitlines()

    def error(self, *regexps):
        print("FAIL: match failed", regexps)
        self.save_output()
        self.stop()
        sys.exit(1)

    def match(self, *regexps, exit=True):
        found = False
        for line in self.lines():
            if any(re.match(r, line) for r in regexps):
                print(line)
                found = True
        if not found and exit:
            self.error(*regexps)
        return found

    # Print the lines matching regexp that have arrived since the last
    # call.  A trailing partial line is left for the next call, so that
    # each line is printed once, after all of it has been read.
    def progress(self, regexp):
        end = self.output.rfind("\n") + 1
        if end <= self.reported:
            return
        for line in self.output[self.reported:end].splitlines():
            if re.match(regexp, line):
                print(line)
        self.reported = end

    def monitor(self, *regexps, progress="", timeout):
        deadline = time.time() + timeout
        while True:
            time.sleep(1)
            timeleft = deadline - time.time()
            if timeleft < 0:
                self.error(*regexps)
            self.read()
            if progress:
                self.progress(progress)
            if self.match(*regexps, exit=False):
                return

def crash_log():
    q = QEMU(True)
    q.cmd("logstress f0 f1 f2 f3 f4 f5\n")
    time.sleep(2)
    q.crash()
    q.stop()

def recover_log():
    q = QEMU()
    time.sleep(2)
    q.read()
    ok = q.match('^recovering', exit=False)
    if ok:
        q.cmd("ls\n")
        q.monitor('f5', timeout=30)
    q.stop()
    return ok

def forphan():
    q = QEMU(True)
    q.cmd("forphan\n")
    q.monitor('wait', timeout=30)
    q.crash()
    q.stop()

def dorphan():
    q = QEMU(True)
    q.cmd("dorphan\n")
    q.monitor('wait', timeout=30)
    q.crash()
    q.stop()

def recover_orphan():
    q = QEMU()
    q.monitor('^ireclaim', timeout=30)
    q.stop()

def test_log():
    print("Test recovery of log")
    for i in range(20):
        crash_log()
        ok = recover_log()
        if ok:
            print("OK")
            return
        print("log attempt ", i+1)
    print("FAIL")
    sys.exit(1)
    
def test_forphan():
    print("Test recovery of an orphaned file")
    forphan()
    recover_orphan()
    print("OK")

def test_dorphan():
    print("Test recovery of an orphaned file")
    dorphan()
    recover_orphan()
    print("OK")

def test_crash():
    test_log()
    test_forphan()
    test_dorphan()

def test_usertests(test=""):
    timeout = 600
    opt = ""
    if args.q:
        opt = " -q"
        timeout = 300
    elif test != "":
        opt += " " + test
    q = QEMU(True)
    q.cmd("usertests" + opt + "\n")
    q.monitor('^ALL TESTS PASSED', progress='test', timeout=timeout)
    q.stop()

def test_feature_matrix():
        print("Build kernel and isolated test image")
        run(["make", "kernel/kernel"], check=True)
        run(["make", "testfs.img"], check=True)

        q = QEMU(image_file="testfs.img")
        transcript = []

        def verify(condition, row, observed):
         if not condition:
             q.read()
             q.save_output("test-features.out")
             raise AssertionError(f"FAIL {row}: {observed}")
         print(f"PASS {row}: {observed}")

        try:
         q.wait_for(r"\$ ", timeout=30)

         nice_output = q.command("nicetest")
         transcript.append("$ nicetest\n" + nice_output)
         nice_pid_match = re.search(r"logging off probe pid=(\d+)", nice_output)
         verify(nice_pid_match is not None, "nice test ran", "nicetest emitted probe marker")
         nice_pid = nice_pid_match.group(1)
         for expected in (
             "positive increment: 5 (expected 5)",
             "negative increment: -3 (expected -3)",
         ):
             verify(expected in nice_output, "increment semantics", expected)
         for expected in (
             "upper clamp: 19 (expected 19)",
             "lower clamp: -20 (expected -20)",
             "INT_MAX clamp: 19 (expected 19)",
             "INT_MIN clamp: -20 (expected -20)",
         ):
             verify(expected in nice_output, "clamp boundaries", expected)

         valid_zero = f"nice set to 0 for {nice_pid}"
         off_start = nice_output.index("logging off probe")
         logging_start = nice_output.index("Logging Started")
         logging_on_marker = nice_output.index("logging on probes")
         logging_stop = nice_output.index("Logging Stopped")
         stopped_marker = nice_output.index("logging stopped probe")
         verify(valid_zero not in nice_output[off_start:logging_start],
             "logging disabled", "no nice diagnostic before Logging Started")
         verify(re.search(r"nice set to -?\d+ for \d+",
                          nice_output[off_start:logging_start]) is None,
             "logging-off nice trace", "no nice-change diagnostic before Logging Started")
         verify("running " not in nice_output[:logging_start],
             "logging-off dispatch trace", "no scheduler dispatch trace before Logging Started")
         on_output = nice_output[logging_start:logging_stop]
         enabled_events = nice_output[logging_on_marker:logging_stop]
         verify(valid_zero in enabled_events and "nice set to 0 for 999999" not in enabled_events,
             "invalid pid vs valid nice zero",
             f"valid PID {nice_pid} logged at 0; invalid PID returned 0 without a log")
         verify(bool(re.search(r"running \d+ at \d+", enabled_events)),
             "dispatch logging enabled", "running PID at tick observed while enabled")
         after_stop = nice_output[logging_stop:]
         verify("running " not in after_stop and valid_zero not in after_stop and
                f"nice set to 1 for {nice_pid}" not in after_stop,
             "logging stopped", "no dispatch or nice diagnostic after Logging Stopped")
         verify(f"fork child default: 0 (expected 0)" in nice_output,
             "fork child nice default", "child queried nice 0 after parent reached -20")
         verify("nicetest passed" in nice_output and stopped_marker < len(nice_output),
             "nice test completion", "nicetest passed and returned to shell")

         basic_output = q.command("logtest")
         transcript.append("$ logtest\n" + basic_output)
         child_match = re.search(r"logtest child finished pid=(\d+) nice=10", basic_output)
         verify("Logging Started" in basic_output and "Logging Stopped" in basic_output,
             "Feature 5 start/stop", "both required logging messages observed")
         verify(child_match is not None, "Feature 5 child nice/work", "child finished bounded work at nice 10")
         if child_match:
             child_pid = child_match.group(1)
             verify(f"nice set to 10 for {child_pid}" in basic_output,
                 "Feature 5 child nice log", f"nice set to 10 for {child_pid}")
             verify(bool(re.search(rf"running {child_pid} at \d+", basic_output)),
                 "Feature 4 dispatch trace", f"dispatch recorded for child PID {child_pid}")
         finish_index = basic_output.find("logtest child finished")
         stop_index = basic_output.find("Logging Stopped")
         verify(finish_index >= 0 and stop_index > finish_index and
             "logtest complete" in basic_output[stop_index:],
             "Feature 5 wait and exit order", "child finished, then logging stopped, then parent completed")
         verify("running " not in basic_output[stop_index:],
             "Feature 4 disabled after stop", "no dispatch trace after Logging Stopped")

         priority_output = q.command("logtest priority")
         transcript.append("$ logtest priority\n" + priority_output)
         starts = {int(value): int(pid) for pid, value in
                re.findall(r"priority child start pid=(\d+) nice=(\d+)", priority_output)}
         release_index = priority_output.find("priority test: both children assigned; releasing gate")
         verify(set(starts.keys()) == {5, 10}, "priority test setup",
             "observed one +5 child and one +10 child")
         if set(starts.keys()) == {5, 10} and release_index >= 0:
             after_release = priority_output[release_index:]
             dispatches = [(int(match.group(1)), match.start()) for match in
                     re.finditer(r"running (\d+) at \d+", after_release)]
             pid_for_nice = starts
             first5 = next((i for pid, i in dispatches if pid == pid_for_nice[5]), None)
             first10 = next((i for pid, i in dispatches if pid == pid_for_nice[10]), None)
             verify(first5 is not None and first10 is not None and first5 < first10,
                 "strict priority with lower-nice child created second",
                 f"+5 PID {pid_for_nice[5]} dispatched before +10 PID {pid_for_nice[10]}")
         finishes = re.findall(r"priority child finished pid=(\d+) nice=(\d+)", priority_output)
         verify(len(finishes) == 2 and finishes[0][1] == "5" and finishes[1][1] == "10",
             "priority work order", "+5 child completed before +10 child")
         verify(priority_output.index("Logging Stopped") > priority_output.index("priority child finished"),
             "priority wait before stop", "both children completed before Logging Stopped")

         equal_output = q.command("logtest equal")
         transcript.append("$ logtest equal\n" + equal_output)
         equal_match = re.search(r"equal test: children (\d+) and (\d+) assigned; releasing gate",
                        equal_output)
         verify(equal_match is not None, "equal-priority test setup", "two children released together")
         if equal_match:
             equal_pids = {int(equal_match.group(1)), int(equal_match.group(2))}
             start = equal_output.index(equal_match.group(0))
             end = equal_output.find("equal child finished", start)
             active_output = equal_output[start:] if end < 0 else equal_output[start:end]
             turns = [int(pid) for pid in
                re.findall(r"running (\d+) at \d+", active_output)
                if int(pid) in equal_pids]
             enough_turns = len(turns) >= 6
             alternating = enough_turns and all(turns[i] != turns[i - 1]
                                  for i in range(1, 6))
             verify(alternating, "equal-nice turn-taking",
                 f"first dispatches for child PIDs: {turns[:8]}")
         verify(len(re.findall(r"equal child finished pid=\d+", equal_output)) == 2 and
             "equal test complete" in equal_output,
             "equal-priority bounded completion", "both CPU-bound children finished and parent exited")

         q.read()
         q.save_output("test-features.out")
         print("Saved full QEMU transcript to test-features.out")
        finally:
         q.stop()

def main():
    print(args)
    rex = r'%s' % args.testrex
    funcs = [(obj,name) for name,obj in inspect.getmembers(sys.modules[__name__]) 
                     if (inspect.isfunction(obj) and 
                         name.startswith('test'))]
    none = True
    for (f,n) in funcs:
        if re.search(rex, n):
            none = False
            f()
    if none:
        test_usertests(test=args.testrex)

main()
