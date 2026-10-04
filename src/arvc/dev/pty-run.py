#!/usr/bin/env python3
"""Runs a command on a pseudo-terminal (no echo) with lines of input; prints what it wrote.

    python3 dev/pty-run.py ANSWERS-FILE COMMAND...

Each line of ANSWERS-FILE is typed in turn; a line holding only ^D ends the input there.
check.sh uses it to compare the questions of an interactive `make` with the Python arv's."""
import os, pty, sys, termios

answers = open(sys.argv[1], encoding="utf-8").read()
pid, fd = pty.fork()
if pid == 0:
    attrs = termios.tcgetattr(0)
    attrs[3] &= ~termios.ECHO
    termios.tcsetattr(0, termios.TCSANOW, attrs)
    os.execvp(sys.argv[2], sys.argv[2:])
os.write(fd, answers.replace("^D\n", "\x04").encode("utf-8"))
out = b""
while True:
    try:
        chunk = os.read(fd, 65536)
    except OSError:
        break
    if not chunk:
        break
    out += chunk
_, status = os.waitpid(pid, 0)
sys.stdout.write(out.decode("utf-8", "replace").replace("\r\n", "\n"))
sys.exit(os.waitstatus_to_exitcode(status))
