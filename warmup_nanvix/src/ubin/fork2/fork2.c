#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>

/*
         PAI
          |
    ______________
    |    |   |   |
    F0  F1  F2  F3
*/

int main() {
    int pid;

    for (int i = 0; i < 4; ++i) {
        pid = fork();

        /* Forks the current process.
           For the parent process, fork() returns the process ID of the child process.
           For the child process, fork() returns zero.
           Upon failure, a negative error code is returned instead.
        */

        if (pid > 0) {  // child process
            printf("Processo pai %d criou %d\n", getpid(), pid);
            fflush(stdout);
        } else { // parent process
            printf("Processso filho %d\n", getpid());
            fflush(stdout);
            break;
        }
    }
    return 0;
}
