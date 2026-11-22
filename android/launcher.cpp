#include <fcntl.h>
#include <grp.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

constexpr uid_t shell_uid = 2000;
constexpr gid_t shell_gid = 2000;
constexpr gid_t inet_gid = 3003;
constexpr char runtime[] = "/system/bin/app_process64";

bool open_configuration(const char* path) {
    const int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    if (fd == -1) {
        std::perror("SynClip: open configuration");
        return false;
    }

    struct stat file{};
    if (fstat(fd, &file) != 0 || !S_ISREG(file.st_mode) || file.st_uid != 0 ||
        (file.st_mode & (S_IRWXG | S_IRWXO)) != 0 || file.st_size > 4096) {
        std::fputs("SynClip: configuration must be a root-only regular file of at most 4096 bytes\n", stderr);
        close(fd);
        return false;
    }
    if (fd != STDIN_FILENO) {
        const int result = dup2(fd, STDIN_FILENO);
        close(fd);
        if (result == -1) {
            std::perror("SynClip: pass configuration to runtime");
            return false;
        }
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 4 || argv[1][0] != '/' || argv[2][0] != '/' ||
        (argv[3][0] != '/' && std::strcmp(argv[3], "--check") != 0)) {
        std::fputs("Usage: synclip-android-launcher <absolute dex jar> <absolute library> <absolute config | --check>\n", stderr);
        return 2;
    }
    if (geteuid() != 0) {
        std::fputs("SynClip: launcher requires root\n", stderr);
        return 2;
    }

    const bool check = std::strcmp(argv[3], "--check") == 0;
    if (!check && !open_configuration(argv[3])) {
        return 2;
    }
    if (setenv("CLASSPATH", argv[1], 1) != 0) {
        std::perror("SynClip: set class path");
        return 2;
    }

    // The clipboard service validates the Binder caller against com.android.shell.
    // Drop credentials before app_process starts ART and opens Binder.
    if (prctl(PR_SET_NO_NEW_PRIVS, 1L, 0L, 0L, 0L) != 0 ||
        prctl(PR_SET_KEEPCAPS, 0L, 0L, 0L, 0L) != 0 ||
        setgroups(1, &inet_gid) != 0 || setresgid(shell_gid, shell_gid, shell_gid) != 0 ||
        setresuid(shell_uid, shell_uid, shell_uid) != 0) {
        std::perror("SynClip: switch to Android shell credentials");
        return 2;
    }

    execl(runtime, runtime, "/system/bin", "--nice-name=synclip-android",
          "synclip.ClipboardBridge", argv[2], check ? "--check" : "--run",
          static_cast<char*>(nullptr));
    std::perror("SynClip: start Android runtime");
    return 2;
}
