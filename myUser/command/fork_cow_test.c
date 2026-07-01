#include "command.h"
#include "result.h"
#include "print.h"
#include <type.h>
#include "result.h"

int fork(void);
static volatile int cow_val = 100;

static int fork_cow_test_handler(int argc, char *argv[], void *priv)
{
	int pid;

	cow_val = 100;
	print("[fork_cow] before fork : cow_val=%d\n", cow_val);
	pid = fork();

	if (pid == 0) {
		cow_val = 200;
		print("[child] cow_val=%d (expect 200)\n", cow_val);
		while (1) {
		}
	} else {
		for (volatile long i = 0; i < 20000000; i++)
			;
		print("[parent]  child_pid=%d cow_val=%d\n", pid, cow_val);
        if (cow_val==100) {
            print("COW isolation: TEST PASS\n");
        }else {
            print("COW isolation: TEST FAIL\n");
        }
	}
    return TEST_PASS;
}

static const struct command fork_cow_test={
    .cmd = "fork_cow_test",
    .handler=fork_cow_test_handler,
    .priv=NULL,
};

int user_cmd_fork_cow_test_init(){
    register_command(&fork_cow_test);
    return 0;
}
APP_COMMAND_REGISTER(fork_cow_test, user_cmd_fork_cow_test_init);