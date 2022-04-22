#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>

int main() {
    fork();
    printf("Novo processo criado!\n");
}
