/* Host execution of exactly the production parser/filter; no serial or sockets. */
#include "ekf_service.h"
#include <stdio.h>
#include <string.h>
int main(void) {
  ekf_service_t s = {0};
  char line[EKF_SERVICE_LINE], out[EKF_SERVICE_LINE];
  while (fgets(line, sizeof(line), stdin)) {
    size_t length = strlen(line);
    if (!length || line[length - 1] != '\n') {
      fprintf(stderr, "oversized/incomplete line\n");
      ekf_service_clear(&s);
      return 2;
    }
    line[length - 1] = 0;
    if (!ekf_service_command(&s, line, 0, out)) {
      fprintf(stderr, "unknown namespace\n");
      ekf_service_clear(&s);
      return 2;
    }
    puts(out);
  }
  ekf_service_clear(&s);
  return ferror(stdin) ? 1 : 0;
}
