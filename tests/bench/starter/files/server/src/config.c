#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"

#define DEFAULT_PORT 8080

Config loadconfig(char *path)
{
    Config c = {DEFAULT_PORT, 4};
    FILE *f = fopen(path, "r");
    char line[256];
    while (f && fgets(line, sizeof(line), f)) {
        if (!strncmp(line, "port=", 5))    c.port    = atoi(line + 5);
        if (!strncmp(line, "workers=", 8)) c.workers = atoi(line + 8);
    }
    if (f) fclose(f);
    return c;
}
