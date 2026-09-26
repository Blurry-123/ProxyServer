#include <stdio.h>
#include<string.h>
#include <time.h>

#include "logger.h"

void write_log(const char *message)
{
    FILE *file;
    time_t current_time;
    char *time_string;

    file = fopen("proxy.log", "a");

    if (file == NULL)
    {
        return;
    }

    current_time = time(NULL);
    time_string = ctime(&current_time);

    if (time_string != NULL)
    {
        time_string[strcspn(time_string, "\n")] = '\0';
        fprintf(file, "[%s] %s\n", time_string, message);
    }

    fclose(file);
}


void log_request(const char *host)
{
    char message[300];

    snprintf(message,
             sizeof(message),
             "HTTP request received for host: %s",
             host);

    write_log(message);
}


void log_error(const char *message)
{
    char log_message[400];

    snprintf(log_message,
             sizeof(log_message),
             "ERROR: %s",
             message);

    write_log(log_message);
}


void log_info(const char *message)
{
    write_log(message);
}


void log_response_time(double time_taken)
{
    char message[200];

    snprintf(message,
             sizeof(message),
             "Request processing time: %.3f seconds",
             time_taken);

    write_log(message);
}