#include "command.h"
#include <print.h>
#include "type.h"

int fli_test(void);
int fminm_test(void);
int fmaxm_test(void);
int fround_test(void);
int fcvtmod_test(void);
int fleq_test(void);
int fltq_test(void);

static rtests tests_zfa[] = {
	{"fli test",fli_test},
	{"fminm test",fminm_test},
	{"fmaxm test",fmaxm_test},
	{"fround test",fmaxm_test},
	{"fleq test",fleq_test},
	{"fltq test",fleq_test},
};
static int test_zfa(void)
{
	int r = 0;
	for(int i = 0;i < (sizeof(tests_zfa)/sizeof(rtests)); i++){
		if(!tests_zfa[i].name)
			break;
		if(!tests_zfa[i].fp()){
			print("%s TEST_SUCCESS \n", tests_zfa[i].name);
		}else{
			print("ERROR: %s fail \n", tests_zfa[i].name);
			r++;
		}
	}
	return r;
}
static int cmd_zfa_handler(int argc, char *argv[], void *priv)
{
	int r;
	print("zfa testing ......\n");
	r = test_zfa();
	print("zfa test...... end\n");
	if(r)
		return TEST_FAIL;
	else
		return TEST_PASS;
}

static const struct command cmd_zfa_test = {
	.cmd = "zfa_test",
	.handler = cmd_zfa_handler,
	.priv = NULL,
};

int user_cmd_zfa_test_init()
{
	register_command(&cmd_zfa_test);

	return 0;
}

APP_COMMAND_REGISTER(zfa_test, user_cmd_zfa_test_init);
