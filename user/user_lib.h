/*
 * header file to be used by applications.
 */
#define abs(x) ((x) > 0 ? (x) : -(x))

int printu(const char *s, ...);
int exit(int code);
void* better_malloc(int n);
void better_free(void* va);
