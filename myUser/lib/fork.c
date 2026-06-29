#include "syscall.h"

int fork(void){
    return (int)syscall(__NR_fork);
}