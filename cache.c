#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#include "cache.h"

struct CacheEntry
{
    char key[512];

    char *response;
    int response_size;

    time_t saved_time;

    int valid;
};

struct CacheEntry cache[MAX_CACHE_ENTRIES];
int check_cache(const char *key, char *response, int *response_size)
{
    int i;

    for (i = 0; i < MAX_CACHE_ENTRIES; i++)
    {
        if (cache[i].valid == 1)
        {
            time_t current_time = time(NULL);

            /* Check if cache entry has expired */
            if (current_time - cache[i].saved_time > CACHE_TIME)
            {
                printf("Cache expired: %s\n", cache[i].key);

                free(cache[i].response);

                cache[i].response = NULL;
                cache[i].valid = 0;

                continue;
            }
            if (strcmp(cache[i].key, key) == 0)
            {
                memcpy(response,
                       cache[i].response,
                       cache[i].response_size);

                *response_size = cache[i].response_size;

                printf("CACHE HIT: %s\n", key);

                return 1;
            }
        }
    }

    printf("CACHE MISS: %s\n", key);

    return 0;
}
void save_cache(const char *key,
                const char *response,
                int response_size)
{
    int i;
    int position = -1;
    if (response_size > MAX_CACHE_SIZE)
    {
        printf("Response too large. Not cached.\n");
        return;
    }
    for (i = 0; i < MAX_CACHE_ENTRIES; i++)
    {
        if (cache[i].valid == 0)
        {
            position = i;
            break;
        }
    }
    if (position == -1)
    {
        position = 0;

        for (i = 1; i < MAX_CACHE_ENTRIES; i++)
        {
            if (cache[i].saved_time < cache[position].saved_time)
            {
                position = i;
            }
        }

        printf("Cache full. Replacing old entry.\n");

        free(cache[position].response);
    }
    strncpy(cache[position].key,
            key,
            sizeof(cache[position].key) - 1);

    cache[position].key[sizeof(cache[position].key) - 1] = '\0';
    cache[position].response = malloc(response_size);

    if (cache[position].response == NULL)
    {
        printf("Memory allocation failed. Response not cached.\n");
        return;
    }
    memcpy(cache[position].response,
           response,
           response_size);

    cache[position].response_size = response_size;

    cache[position].saved_time = time(NULL);

    cache[position].valid = 1;

    printf("Response saved in cache: %s\n", key);
}
void clear_cache()
{
    int i;

    for (i = 0; i < MAX_CACHE_ENTRIES; i++)
    {
        if (cache[i].valid == 1)
        {
            free(cache[i].response);

            cache[i].response = NULL;
            cache[i].valid = 0;
        }
    }

    printf("Cache cleared.\n");
}
