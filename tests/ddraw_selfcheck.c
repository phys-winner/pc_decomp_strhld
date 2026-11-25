#include <stdio.h>
#include <stdarg.h>
int stronghold_renderer_selftest(void);
void void_log(const char *fmt,...){va_list ap;va_start(ap,fmt);vprintf(fmt,ap);putchar('\n');va_end(ap);}
int main(void){return stronghold_renderer_selftest();}
