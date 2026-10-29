#define _POSIX_C_SOURCE 200809L
#include "application.h"
int main(int argc, char **argv) {
  struct swappy_state state = {0};
  state.argc = argc;
  state.argv = argv;
  if (!application_init(&state)) return EXIT_FAILURE;
  int ret = application_run(&state);
  application_finish(&state);
  return ret;
}
