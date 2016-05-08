#ifndef MY_COMPILER_C
#define MY_COMPILER_C

#include <string.h>

/* http://stackoverflow.com/a/3553321 */
#define member_size(type, member)   sizeof (((type *)0)->member)
#define array_size(array)           (sizeof(array) / sizeof((array)[0]))
#define array_member_size(type, array_member)  array_size(((type *)0)->array_member)
#define __FILENAME__                (strrchr(__FILE__, '/') ? strrchr(__FILE__, '/') + 1 : __FILE__)

#endif
