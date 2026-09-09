#include <stdio.h>
#include <stdarg.h>

int __android_log_print(int prio, const char *tag,  const char *fmt, ...)
{
   va_list args;
   va_start(args, fmt);
   printf("__android_log_print: %d %s: ", prio, tag);
   vprintf(fmt, args);
   va_end(args);
   printf("\n");
   return 0;
}
