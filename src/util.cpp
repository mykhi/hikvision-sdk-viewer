#include "util.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

volatile sig_atomic_t g_stop = 0;

long long now_ms() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

NET_DVR_TIME to_dvr(time_t t) {
    struct tm lt;
    localtime_r(&t, &lt);
    NET_DVR_TIME d = {};
    d.dwYear = lt.tm_year + 1900, d.dwMonth = lt.tm_mon + 1, d.dwDay = lt.tm_mday;
    d.dwHour = lt.tm_hour, d.dwMinute = lt.tm_min, d.dwSecond = lt.tm_sec;
    return d;
}

time_t from_dvr(const NET_DVR_TIME &d) {
    struct tm lt = {};
    lt.tm_year = d.dwYear - 1900, lt.tm_mon = d.dwMonth - 1, lt.tm_mday = d.dwDay;
    lt.tm_hour = d.dwHour, lt.tm_min = d.dwMinute, lt.tm_sec = d.dwSecond;
    lt.tm_isdst = -1;
    return mktime(&lt);
}

std::string fmt(time_t t, const char *f) {
    struct tm lt;
    localtime_r(&t, &lt);
    char buf[64];
    strftime(buf, sizeof(buf), f, &lt);
    return buf;
}

time_t parse_time(const char *s) {
    time_t now = time(nullptr);
    if (s[0] == '-') {
        char *end;
        long v = strtol(s + 1, &end, 10);
        long unit = !strcmp(end, "s") ? 1 : !strcmp(end, "m") ? 60 : !strcmp(end, "h") ? 3600 : !strcmp(end, "d") ? 86400 : 0;
        if (end != s + 1 && unit) return now - v * unit;
    } else {
        struct tm today;
        localtime_r(&now, &today);
        today.tm_hour = today.tm_min = today.tm_sec = 0;
        for (const char *f : {"%Y-%m-%d %H:%M:%S", "%Y-%m-%d %H:%M", "%Y-%m-%d", "%H:%M:%S", "%H:%M"}) {
            struct tm t = today;
            const char *end = strptime(s, f, &t);
            if (end && *end == '\0') {
                t.tm_isdst = -1;
                return mktime(&t);
            }
        }
    }
    fprintf(stderr, "cannot parse time \"%s\"\n", s);
    exit(2);
}

static pid_t spawn(std::vector<const char *> argv, int in_fd, int out_fd, bool quiet) {
    pid_t pid = fork();
    if (pid == 0) {
        if (in_fd >= 0) dup2(in_fd, STDIN_FILENO);
        if (out_fd >= 0) dup2(out_fd, STDOUT_FILENO);
        if (quiet) {
            int null = open("/dev/null", O_WRONLY);
            dup2(null, STDERR_FILENO);
        }
        for (int fd = 3; fd < 256; fd++) close(fd);
        argv.push_back(nullptr);
        execvp(argv[0], (char *const *)argv.data());
        _exit(127);
    }
    return pid;
}

static int wait_status(pid_t pid) {
    int status = 0;
    if (pid < 0 || waitpid(pid, &status, 0) < 0 || !WIFEXITED(status)) return -1;
    return WEXITSTATUS(status) == 127 ? -1 : WEXITSTATUS(status);
}

int run(std::vector<const char *> argv, const std::string *input) {
    int fd[2] = {-1, -1};
    if (input && pipe(fd)) return -1;
    pid_t pid = spawn(argv, fd[0], -1, false);
    if (input) {
        close(fd[0]);
        if (pid > 0 && write(fd[1], input->data(), input->size()) < 0) perror("write");
        close(fd[1]);
    }
    return wait_status(pid);
}

std::string run_line(std::vector<const char *> argv) {
    int fd[2];
    if (pipe(fd)) return "";
    pid_t pid = spawn(argv, -1, fd[1], true);
    close(fd[1]);
    std::string out;
    char buf[256];
    ssize_t n;
    while (pid > 0 && (n = read(fd[0], buf, sizeof(buf))) > 0) out.append(buf, n);
    close(fd[0]);
    if (wait_status(pid) != 0) return "";
    return out.substr(0, out.find('\n'));
}

bool have_command(const char *name) {
    std::string path = getenv("PATH") ? getenv("PATH") : "/usr/bin:/bin";
    for (size_t start = 0; start <= path.size();) {
        size_t end = path.find(':', start);
        if (end == std::string::npos) end = path.size();
        std::string candidate = path.substr(start, end - start) + "/" + name;
        if (access(candidate.c_str(), X_OK) == 0) return true;
        start = end + 1;
    }
    return false;
}

std::string ask(const std::string &question, bool secret) {
    FILE *tty = fopen("/dev/tty", "r+");
    if (!tty) return "";
    fprintf(tty, "%s", question.c_str());
    fflush(tty);
    termios old, quiet;
    if (secret) {
        tcgetattr(fileno(tty), &old);
        quiet = old;
        quiet.c_lflag &= ~ECHO;
        tcsetattr(fileno(tty), TCSAFLUSH, &quiet);
    }
    char buf[512] = {0};
    bool ok = fgets(buf, sizeof(buf), tty) != nullptr;
    if (secret) {
        tcsetattr(fileno(tty), TCSAFLUSH, &old);
        fprintf(tty, "\n");
    }
    fclose(tty);
    std::string answer = ok ? std::string(buf, strcspn(buf, "\r\n")) : "";
    explicit_bzero(buf, sizeof(buf));
    return answer;
}
