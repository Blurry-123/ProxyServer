#ifndef CACHE_H
#define CACHE_H

#define MAX_CACHE_ENTRIES 5
#define MAX_CACHE_SIZE (1024 * 1024)
#define CACHE_TIME 60

int check_cache(const char *key, char *response, int *response_size);

void save_cache(const char *key,
                const char *response,
                int response_size);

void clear_cache();

#endif