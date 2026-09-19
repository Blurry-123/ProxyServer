#include <stdio.h>
#include <string.h>

#include "access_control.h"
const char *blocked_domains[] =
{
    "facebook.com",
    "instagram.com",
    "example.com"
};


#define BLOCKED_COUNT 3
int is_blocked(const char *host)
{
    int i;

    for (i = 0; i < BLOCKED_COUNT; i++)
    {
        if (strstr(host, blocked_domains[i]) != NULL)
        {
            printf("ACCESS DENIED: %s\n", host);

            return 1;
        }
    }

    printf("ACCESS ALLOWED: %s\n", host);

    return 0;
}
