#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>
#include <dirent.h>
#include <sys/stat.h>

#define MAX_LINE 1024
#define MAX_ARGS 64

static int read_line(char *buf, size_t size) {
    if (!fgets(buf, size, stdin)) return 0;
    size_t n = strlen(buf);
    if (n > 0 && buf[n-1] == '\n') buf[n-1] = '\0';
    return 1;
}

static int tokenize(char *line, char *tokens[]) {
    int i = 0;
    char *p = strtok(line, " \t");
    while (p && i < MAX_ARGS-1) {
        tokens[i++] = p;
        p = strtok(NULL, " \t");
    }
    tokens[i] = NULL;
    return i;
}

static int build_argv(char *tokens[], int s, int e,
                      char *argv[], int *argc,
                      char **infile, char **outfile) {

    *argc = 0; *infile = NULL; *outfile = NULL;

    for (int i = s; i <= e; i++) {
        if (!strcmp(tokens[i], "<")) {
            if (i+1 <= e) *infile = tokens[++i];
            else return 0;
        } else if (!strcmp(tokens[i], ">")) {
            if (i+1 <= e) *outfile = tokens[++i];
            else return 0;
        } else {
            argv[*argc] = tokens[i];
            (*argc)++;
        }
    }
    argv[*argc] = NULL;
    return 1;
}

static void exec_single(char *argv[], char *infile, char *outfile, int bg) {
    pid_t pid = fork();

    if (pid == 0) {
        signal(SIGINT, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);

        if (infile) {
            int fd = open(infile, O_RDONLY);
            dup2(fd, STDIN_FILENO);
            close(fd);
        }
        if (outfile) {
            int fd = open(outfile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            dup2(fd, STDOUT_FILENO);
            close(fd);
        }
        execvp(argv[0], argv);
        perror("exec");
        exit(1);
    }

    if (!bg) waitpid(pid, NULL, 0);
    else printf("[bg pid %d]\n", pid);
}

static void exec_pipe(char *tokens[], int start, int mid, int end, int bg) {
    char *argvL[MAX_ARGS], *argvR[MAX_ARGS];
    char *inL, *outL, *inR, *outR;
    int argcL, argcR;

    if (!build_argv(tokens, start, mid-1, argvL, &argcL, &inL, &outL)) return;
    if (!build_argv(tokens, mid+1, end, argvR, &argcR, &inR, &outR)) return;

    int fd[2];
    pipe(fd);

    pid_t left = fork();
    if (left == 0) {
        signal(SIGINT, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);

        dup2(fd[1], STDOUT_FILENO);
        close(fd[0]); close(fd[1]);

        if (inL) {
            int fdin = open(inL, O_RDONLY);
            dup2(fdin, STDIN_FILENO);
            close(fdin);
        }
        if (outL) {
            int fdout = open(outL, O_CREAT|O_TRUNC|O_WRONLY, 0644);
            dup2(fdout, STDOUT_FILENO);
            close(fdout);
        }
        execvp(argvL[0], argvL);
        perror("exec left");
        exit(1);
    }

    pid_t right = fork();
    if (right == 0) {
        signal(SIGINT, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);

        dup2(fd[0], STDIN_FILENO);
        close(fd[0]); close(fd[1]);

        if (outR) {
            int fdout = open(outR, O_CREAT|O_TRUNC|O_WRONLY, 0644);
            dup2(fdout, STDOUT_FILENO);
            close(fdout);
        }
        if (inR) {
            int fdin = open(inR, O_RDONLY);
            dup2(fdin, STDIN_FILENO);
            close(fdin);
        }
        execvp(argvR[0], argvR);
        perror("exec right");
        exit(1);
    }

    close(fd[0]); close(fd[1]);
    if (!bg) {
        waitpid(left, NULL, 0);
        waitpid(right, NULL, 0);
    } else {
        printf("[bg pipe %d,%d]\n", left, right);
    }
}

void builtin_pwd() {
    char buf[1024];
    if (getcwd(buf, sizeof(buf)))
        printf("%s\n", buf);
}

void builtin_cd(char *path) {
    if (!path) { printf("cd: path required\n"); return; }
    if (chdir(path) < 0)
        perror("cd");
}

void builtin_ls(const char *path)
{
    if (!path) path = ".";

    DIR *dir = opendir(path);
    if (!dir) {
        perror("ls");
        return;
    }

    struct dirent *ent;
    char *names[1024];   // 파일 이름 저장
    int count = 0;

    // 1) 파일 목록 읽기 (숨김 파일 제외)
    while ((ent = readdir(dir)) != NULL) {
        // .으로 시작하는 숨김 파일 제외
        if (ent->d_name[0] == '.') continue;

        // 일반 파일만 출력 (DT_REG 또는 stat)
        char fullpath[1024];
        struct stat st;
        snprintf(fullpath, sizeof(fullpath), "%s/%s", path, ent->d_name);
        if (stat(fullpath, &st) == 0 && S_ISREG(st.st_mode)) {
            names[count++] = strdup(ent->d_name);
        }
    }
    closedir(dir);

    // 2) 알파벳 순 정렬
    for (int i = 0; i < count - 1; i++) {
        for (int j = i + 1; j < count; j++) {
            if (strcmp(names[i], names[j]) > 0) {
                char *tmp = names[i];
                names[i] = names[j];
                names[j] = tmp;
            }
        }
    }

    // 3) 출력
    int maxlen = 0;
    for (int i = 0; i < count; i++) {
        int len = strlen(names[i]);
        if (len > maxlen) maxlen = len;
    }
    int cols = 120 / (maxlen + 2);
    if (cols < 1) cols = 1;

    for (int i = 0; i < count; i++) {
        printf("%-*s", maxlen + 2, names[i]);
        if ((i + 1) % cols == 0) printf("\n");
        free(names[i]); // strdup 해제
    }
    printf("\n");
}


void builtin_mkdir(char *path) {
    if (!path) { printf("mkdir: dir required\n"); return; }
    if (mkdir(path, 0755) < 0)
        perror("mkdir");
}

void builtin_rmdir(char *path) {
    if (!path) { printf("rmdir: dir required\n"); return; }
    if (rmdir(path) < 0)
        perror("rmdir");
}

void builtin_rm(char *path) {
    if (!path) { printf("rm: file required\n"); return; }
    if (unlink(path) < 0)
        perror("rm");
}

void builtin_cat(char *file) {
    if (!file) { printf("cat: file required\n"); return; }

    FILE *fp = fopen(file, "r");
    if (!fp) { perror("cat"); return; }

    int c;
    while ((c = fgetc(fp)) != EOF)
        putchar(c);
    fclose(fp);
}

int main() {
    char line[MAX_LINE];
    char *tok[MAX_ARGS];

    signal(SIGINT, SIG_IGN);
    signal(SIGQUIT, SIG_IGN);

    while (1) {
        printf("myshell> ");
        fflush(stdout);

        if (!read_line(line, sizeof(line))) break;
        if (line[0] == '\0') continue;

        int nt = tokenize(line, tok);
        if (nt == 0) continue;

        if (!strcmp(tok[0], "exit")) break;

        if (!strcmp(tok[0], "pwd")) {
            builtin_pwd();
            continue;
        }

        if (!strcmp(tok[0], "cd")) {
            builtin_cd(tok[1]);
            continue;
        }
        
        if (!strcmp(tok[0], "ls")) {
            builtin_ls(tok[1]);
            continue;
        }
        
        if (!strcmp(tok[0], "mkdir")) {
            builtin_mkdir(tok[1]);
            continue;
        }
        
        if (!strcmp(tok[0], "rmdir")) {
            builtin_rmdir(tok[1]);
            continue;
        }
        
        if (!strcmp(tok[0], "rm")) {
            builtin_rm(tok[1]);
            continue;
        }
        
        if (!strcmp(tok[0], "cat")) {
            builtin_cat(tok[1]);
            continue;
        }

        int bg = 0;
        if (!strcmp(tok[nt-1], "&")) {
            bg = 1;
            tok[nt-1] = NULL;
            nt--;
        }

        int p = -1;
        for (int i = 0; i < nt; i++)
            if (!strcmp(tok[i], "|")) { p = i; break; }

        if (p == -1) {
            char *argv[MAX_ARGS];
            char *infile, *outfile;
            int argc;

            if (!build_argv(tok, 0, nt-1, argv, &argc, &infile, &outfile))
                continue;
            if (argc == 0) continue;

            exec_single(argv, infile, outfile, bg);
        } else {
            exec_pipe(tok, 0, p, nt-1, bg);
        }
    }
    return 0;
}
