/*
 * Runs a command on a pseudo-terminal (no echo) with lines of input, and prints what it wrote.
 *
 *   ptyrun ANSWERS-FILE COMMAND...
 *
 * Each line of ANSWERS-FILE is typed in turn; a line holding only ^D ends the input of one
 * question. The reference scenarios (tests/reference/scenarios.sh) use it for an interactive make.
 */
#define _XOPEN_SOURCE 700
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    if (argc < 3) {
        fputs("usage: ptyrun ANSWERS-FILE COMMAND...\n", stderr);
        return 2;
    }
    FILE *in = fopen(argv[1], "r");
    if (!in) { perror(argv[1]); return 2; }
    int master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0 || grantpt(master) || unlockpt(master)) { perror("posix_openpt"); return 2; }
    char *slave_name = ptsname(master);
    int pre = open(slave_name, O_RDWR | O_NOCTTY);  /* no echo before anything is typed */
    struct termios t;
    if (pre < 0 || tcgetattr(pre, &t)) { perror(slave_name); return 2; }
    t.c_lflag &= ~(tcflag_t)ECHO;
    t.c_oflag &= ~(tcflag_t)ONLCR;                  /* "\n" stays "\n" */
    tcsetattr(pre, TCSANOW, &t);
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return 2; }
    if (pid == 0) {
        setsid();
        int slave = open(slave_name, O_RDWR);
        if (slave < 0) _exit(127);
        close(pre);
        dup2(slave, 0);
        dup2(slave, 1);
        dup2(slave, 2);
        if (slave > 2) close(slave);
        close(master);
        execvp(argv[2], argv + 2);
        _exit(127);
    }
    close(pre);
    char line[4096];
    while (fgets(line, sizeof line, in)) {           /* typed ahead: the terminal keeps it */
        if (!strcmp(line, "^D\n")) {
            if (write(master, "\004", 1) < 0) break;
        } else if (write(master, line, strlen(line)) < 0) {
            break;
        }
    }
    fclose(in);
    char buf[65536];
    for (;;) {
        ssize_t n = read(master, buf, sizeof buf);
        if (n > 0) { fwrite(buf, 1, (size_t)n, stdout); continue; }
        if (n < 0 && errno == EINTR) continue;
        break;                                       /* EIO once the command has gone */
    }
    int status;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}
