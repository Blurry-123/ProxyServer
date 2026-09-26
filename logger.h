#ifndef LOGGER_H
#define LOGGER_H

void log_request(const char *host);
void log_error(const char *message);
void log_info(const char *message);
void log_response_time(double time_taken);

#endif