#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "strutils.h"

void strchrdel(char *str, char garbage) {
	char *src, *dst;
	for (src = dst = str; *src != '\0'; src++) {
		*dst = *src;
		if (*dst != garbage) dst++;
	}
	*dst = '\0';
}
