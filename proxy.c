#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netdb.h>
#include <pthread.h>

#include "cache.h"
#include "access_control.h"
#include "logger.h"

#define PROXY_PORT 8888
#define BUFFER_SIZE 8192


int get_host(char *request, char *host)
{
    char *start;
    char *end;

    start = strstr(request, "Host:");

    if (start == NULL)
        return 0;

    start += 5;

    while (*start == ' ')
        start++;

    end = strstr(start, "\r\n");

    if (end == NULL)
        return 0;

    int length = end - start;

    if (length >= 256)
        return 0;

    strncpy(host, start, length);
    host[length] = '\0';

    char *colon = strchr(host, ':');

    if (colon != NULL)
        *colon = '\0';

    return 1;
}


int send_all(int socket_fd, const char *data, int size)
{
    int total_sent = 0;

    while (total_sent < length)
    {
        int sent = send(socket_fd,
                        data + total_sent,
                        length - total_sent,
                        0);

        if (sent <= 0)
        {
            return -1;
        }

        total_sent += sent;
    }

    return 0;
}

static void send_error_response(int client_socket,
                                int status_code,
                                const char *status_text,
                                const char *message)
{
    char response[1024];
    int message_length;
    int response_length;

    message_length = (int)strlen(message);

    response_length = snprintf(
        response,
        sizeof(response),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "\r\n"
        "%s",
        status_code,
        status_text,
        message_length,
        message
    );

    if (response_length > 0)
    {
        send_all(client_socket,
                 response,
                 response_length);
    }
}

static int get_header_value(const char *request,
                            const char *header_name,
                            char *value,
                            int value_size)
{
    const char *line;
    size_t header_length;

    if (request == NULL ||
        header_name == NULL ||
        value == NULL ||
        value_size <= 0)
    {
        return -1;
    }

    header_length = strlen(header_name);
    line = request;

    while (*line != '\0')
    {
        const char *line_end;
        const char *colon;

        line_end = strstr(line, "\r\n");

        if (line_end == NULL)
        {
            break;
        }

        if (line_end == line)
        {
            break;
        }

        colon = strchr(line, ':');

        if (colon != NULL &&
            (size_t)(colon - line) == header_length &&
            strncasecmp(line,
                        header_name,
                        header_length) == 0)
        {
            const char *start;
            const char *end;
            int length;

            start = colon + 1;

            while (start < line_end &&
                   (*start == ' ' || *start == '\t'))
            {
                start++;
            }

            end = line_end;

            while (end > start &&
                   (end[-1] == ' ' || end[-1] == '\t'))
            {
                end--;
            }

            length = (int)(end - start);

            if (length >= value_size)
            {
                length = value_size - 1;
            }

            memcpy(value, start, length);
            value[length] = '\0';

            return 0;
        }

        line = line_end + 2;
    }

    return -1;
}

static int get_host(const char *request,
                    char *host,
                    int host_size)
{
    char host_value[512];
    char *colon;

    if (get_header_value(request,
                         "Host",
                         host_value,
                         sizeof(host_value)) != 0)
    {
        return -1;
    }

    colon = strchr(host_value, ':');

    if (colon != NULL)
    {
        *colon = '\0';
    }

    if (host_value[0] == '\0')
    {
        return -1;
    }

    strncpy(host,
            host_value,
            host_size - 1);

    host[host_size - 1] = '\0';

    return 0;
}

static int request_has_connection_header(const char *request)
{
    const char *line;

    line = request;

    while (*line != '\0')
    {
        const char *line_end;
        const char *colon;

        line_end = strstr(line, "\r\n");

        if (line_end == NULL || line_end == line)
        {
            break;
        }

        colon = strchr(line, ':');

        if (colon != NULL &&
            (size_t)(colon - line) == strlen("Connection") &&
            strncasecmp(line,
                        "Connection",
                        strlen("Connection")) == 0)
        {
            return 1;
        }

        line = line_end + 2;
    }

    return 0;
}

static int build_forward_request(const char *request,
                                 char *output,
                                 int output_size)
{
    const char *header_end;
    int header_length;
    int new_length;

    if (request == NULL || output == NULL)
    {
        return -1;
    }

    /*
     * If the client already supplied a Connection header,
     * forward the request unchanged.
     */
    if (request_has_connection_header(request))
    {
        int request_length = (int)strlen(request);

        if (request_length >= output_size)
        {
            return -1;
        }

        memcpy(output, request, request_length + 1);

        return request_length;
    }

    header_end = strstr(request, "\r\n\r\n");

    if (header_end == NULL)
    {
        return -1;
    }

    /*
     * header_end points to the CRLF that begins the
     * final "\r\n\r\n".
     *
     * We replace:
     *
     *     \r\n\r\n
     *
     * with:
     *
     *     \r\nConnection: close\r\n\r\n
     */
    header_length = (int)(header_end - request);

    new_length = header_length
                 + (int)strlen("\r\nConnection: close\r\n\r\n");

    if (new_length >= output_size)
    {
        return -1;
    }

    memcpy(output,
           request,
           header_length);

    memcpy(output + header_length,
           "\r\nConnection: close\r\n\r\n",
           strlen("\r\nConnection: close\r\n\r\n"));

    output[new_length] = '\0';

    return new_length;
}

static void *handle_client(void *arg)
{
    int client_socket;
    char buffer[BUFFER_SIZE];
    char forward_request[BUFFER_SIZE];
    char host[512];

    int bytes_received;
    int request_length;

    char *cache_response;
    int cache_response_size;

    int server_socket;
    struct hostent *server_host;
    struct sockaddr_in server_address;

    char *full_response;
    int total_response_size;
    int response_capacity;

    struct timespec start_time;
    struct timespec end_time;

    double elapsed_time;

    client_socket = *((int *)arg);
    free(arg);

    clock_gettime(CLOCK_MONOTONIC, &start_time);

    /*
     * Set client receive timeout.
     */
    {
        struct timeval timeout;

        timeout.tv_sec = CLIENT_TIMEOUT;
        timeout.tv_usec = 0;

        setsockopt(client_socket,
                   SOL_SOCKET,
                   SO_RCVTIMEO,
                   &timeout,
                   sizeof(timeout));
    }

    /*
     * Read the complete HTTP request headers.
     */
    bytes_received = 0;

    while (bytes_received < BUFFER_SIZE - 1)
    {
        int received;

        received = recv(client_socket,
                        buffer + bytes_received,
                        BUFFER_SIZE - 1 - bytes_received,
                        0);

        if (received > 0)
        {
            bytes_received += received;
            buffer[bytes_received] = '\0';

            if (strstr(buffer, "\r\n\r\n") != NULL)
            {
                break;
            }
        }
        else if (received == 0)
        {
            break;
        }
        else
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                log_error("Client request timed out");
            }
            else
            {
                log_error("Error receiving client request");
            }

            close(client_socket);
            return NULL;
        }
    }

    buffer[bytes_received] = '\0';

    /*
     * Make sure we actually received a complete HTTP header.
     */
    if (bytes_received == 0 ||
        strstr(buffer, "\r\n\r\n") == NULL)
    {
        log_error("Malformed or incomplete HTTP request");

        send_error_response(client_socket,
                            400,
                            "Bad Request",
                            "Bad Request");

        close(client_socket);
        return NULL;
    }

    /*
     * Only GET and HEAD are supported.
     */
    if (strncmp(buffer, "GET ", 4) != 0 &&
        strncmp(buffer, "HEAD ", 5) != 0)
    {
        log_error("Unsupported HTTP method");

        send_error_response(client_socket,
                            400,
                            "Bad Request",
                            "Only GET and HEAD are supported");

        close(client_socket);
        return NULL;
    }

    /*
     * Get Host header.
     */
    if (get_host(buffer,
                 host,
                 sizeof(host)) != 0)
    {
        log_error("Host header missing");

        send_error_response(client_socket,
                            400,
                            "Bad Request",
                            "Host header required");

        close(client_socket);
        return NULL;
    }

    log_request(host);

    printf("HTTP request received for host: %s\n", host);

    /*
     * Access control.
     */
    if (is_blocked(host))
    {
        log_info("Request blocked by access control");

        send_error_response(client_socket,
                            403,
                            "Forbidden",
                            "Access denied");

        close(client_socket);
        return NULL;
    }

    /*
     * Use the complete request as the cache key.
     */
    request_length = bytes_received;

    if (request_length >= 512)
    {
        /*
         * The cache key has a fixed maximum size.
         * We simply skip caching for oversized requests.
         */
        cache_response = NULL;
        cache_response_size = 0;
    }
    else
    {
        cache_response = malloc(MAX_CACHE_SIZE);

        if (cache_response == NULL)
        {
            log_error("Cache memory allocation failed");
            cache_response_size = 0;
        }
        else
        {
            cache_response_size = 0;

            if (check_cache(buffer,
                            cache_response,
                            &cache_response_size))
            {
                printf("Sending cached response to client\n");

                send_all(client_socket,
                         cache_response,
                         cache_response_size);

                free(cache_response);

                close(client_socket);

                clock_gettime(CLOCK_MONOTONIC, &end_time);

                elapsed_time =
                    (end_time.tv_sec - start_time.tv_sec) +
                    (end_time.tv_nsec - start_time.tv_nsec) / 1000000000.0;

                log_response_time(elapsed_time);

                return NULL;
            }
        }
    }

    if (cache_response != NULL)
    {
        free(cache_response);
        cache_response = NULL;
    }

    /*
     * Resolve destination host.
     */
    server_host = gethostbyname(host);

    if (server_host == NULL)
    {
        log_error("Could not resolve destination host");

        send_error_response(client_socket,
                            502,
                            "Bad Gateway",
                            "Could not resolve destination host");

        close(client_socket);
        return NULL;
    }

    /*
     * Create destination socket.
     */
    server_socket = socket(AF_INET,
                           SOCK_STREAM,
                           0);

    if (server_socket < 0)
    {
        log_error("Could not create destination socket");

        send_error_response(client_socket,
                            502,
                            "Bad Gateway",
                            "Could not create destination socket");

        close(client_socket);
        return NULL;
    }

    /*
     * Set destination timeout.
     */
    {
        struct timeval timeout;

        timeout.tv_sec = SERVER_TIMEOUT;
        timeout.tv_usec = 0;

        setsockopt(server_socket,
                   SOL_SOCKET,
                   SO_RCVTIMEO,
                   &timeout,
                   sizeof(timeout));

        setsockopt(server_socket,
                   SOL_SOCKET,
                   SO_SNDTIMEO,
                   &timeout,
                   sizeof(timeout));
    }

    memset(&server_address,
           0,
           sizeof(server_address));

    server_address.sin_family = AF_INET;
    server_address.sin_port = htons(80);

    memcpy(&server_address.sin_addr,
           server_host->h_addr_list[0],
           server_host->h_length);

    /*
     * Connect to destination.
     */
    if (connect(server_socket,
                (struct sockaddr *)&server_address,
                sizeof(server_address)) < 0)
    {
        log_error("Could not connect to destination");

        send_error_response(client_socket,
                            502,
                            "Bad Gateway",
                            "Could not connect to destination");

        close(server_socket);
        close(client_socket);

        return NULL;
    }

    printf("Connected to destination: %s\n", host);

    /*
     * Build a forward request with Connection: close.
     */
    {
        int forward_length;

        forward_length = build_forward_request(
            buffer,
            forward_request,
            sizeof(forward_request));

        if (forward_length < 0)
        {
            log_error("Could not build forward request");

            send_error_response(client_socket,
                                400,
                                "Bad Request",
                                "Request too large or malformed");

            close(server_socket);
            close(client_socket);

            return NULL;
        }

        /*
         * Forward request to destination.
         */
        if (send_all(server_socket,
                     forward_request,
                     forward_length) < 0)
        {
            log_error("Failed to forward request");

            send_error_response(client_socket,
                                502,
                                "Bad Gateway",
                                "Failed to forward request");

            close(server_socket);
            close(client_socket);

            return NULL;
        }
    }

    printf("Request forwarded to destination\n");

    /*
     * Tell destination that no more request data will be sent.
     */
    shutdown(server_socket, SHUT_WR);

    /*
     * Receive destination response.
     */
    response_capacity = BUFFER_SIZE;

    full_response = malloc(response_capacity);

    if (full_response == NULL)
    {
        log_error("Response memory allocation failed");

        close(server_socket);
        close(client_socket);

        return NULL;
    }

    total_response_size = 0;

    while (1)
    {
        char response_buffer[BUFFER_SIZE];
        int received;

        received = recv(server_socket,
                        response_buffer,
                        sizeof(response_buffer),
                        0);

        if (received > 0)
        {
            /*
             * Expand response buffer if necessary.
             */
            if (total_response_size + received > response_capacity)
            {
                int new_capacity;
                char *new_response;

                new_capacity = response_capacity * 2;

                while (new_capacity <
                       total_response_size + received)
                {
                    new_capacity *= 2;
                }

                /*
                 * Never cache responses larger than MAX_CACHE_SIZE,
                 * but we still allow forwarding larger responses.
                 */
                new_response = realloc(full_response,
                                       new_capacity);

                if (new_response == NULL)
                {
                    log_error("Response memory reallocation failed");

                    free(full_response);
                    close(server_socket);
                    close(client_socket);

                    return NULL;
                }

                full_response = new_response;
                response_capacity = new_capacity;
            }

            memcpy(full_response + total_response_size,
                   response_buffer,
                   received);

            total_response_size += received;
        }
        else if (received == 0)
        {
            /*
             * Destination closed connection normally.
             */
            break;
        }
        else
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                log_error("Destination response timed out");

                if (total_response_size == 0)
                {
                    send_error_response(client_socket,
                                        504,
                                        "Gateway Timeout",
                                        "Destination response timed out");

                    free(full_response);
                    close(server_socket);
                    close(client_socket);

                    return NULL;
                }

                break;
            }

            log_error("Error receiving destination response");
            break;
        }
    }

    close(server_socket);

    printf("Response received: %d bytes\n",
           total_response_size);

    /*
     * Send response to client.
     */
    if (total_response_size > 0)
    {
        if (send_all(client_socket,
                     full_response,
                     total_response_size) < 0)
        {
            log_error("Failed to send response to client");
        }
        else
        {
            printf("Response sent to client\n");
        }
    }
    else
    {
        log_error("Destination returned empty response");

        send_error_response(client_socket,
                            502,
                            "Bad Gateway",
                            "Destination returned empty response");
    }

    /*
     * Cache response if it is within the cache size limit.
     */
    if (total_response_size > 0 &&
        total_response_size <= MAX_CACHE_SIZE)
    {
        save_cache(buffer,
                   full_response,
                   total_response_size);
    }

    free(full_response);

    close(client_socket);

    clock_gettime(CLOCK_MONOTONIC, &end_time);

    elapsed_time =
        (end_time.tv_sec - start_time.tv_sec) +
        (end_time.tv_nsec - start_time.tv_nsec) / 1000000000.0;

    printf("Request processing time: %.3f seconds\n",
           elapsed_time);

    log_response_time(elapsed_time);

    return NULL;
}

int main(void)
{
    int server_socket;
    int client_socket;

    struct sockaddr_in server_address;
    struct sockaddr_in client_address;

    socklen_t client_length;

    int reuse_address;

    printf("Starting proxy server...\n");

    /*
     * Prevent crashes when sending to a disconnected client.
     */
    signal(SIGPIPE, SIG_IGN);

    /*
     * Create listening socket.
     */
    server_socket = socket(AF_INET,
                           SOCK_STREAM,
                           0);

    if (server_socket < 0)
    {
        perror("socket");
        return 1;
    }

    /*
     * Allow quick reuse of the port.
     */
    reuse_address = 1;

    setsockopt(server_socket,
               SOL_SOCKET,
               SO_REUSEADDR,
               &reuse_address,
               sizeof(reuse_address));

    memset(&server_address,
           0,
           sizeof(server_address));

    server_address.sin_family = AF_INET;
    server_address.sin_addr.s_addr = INADDR_ANY;
    server_address.sin_port = htons(PROXY_PORT);

    /*
     * Bind socket.
     */
    if (bind(server_socket,
             (struct sockaddr *)&server_address,
             sizeof(server_address)) < 0)
    {
        perror("bind");
        close(server_socket);
        return 1;
    }

    /*
     * Start listening.
     */
    if (listen(server_socket, 20) < 0)
    {
        perror("listen");
        close(server_socket);
        return 1;
    }

    printf("Proxy server started on port %d\n",
           PROXY_PORT);

    /*
     * Accept clients continuously.
     */
    while (1)
    {
        pthread_t thread;
        int *client_socket_ptr;

        client_length = sizeof(client_address);

        client_socket = accept(
            server_socket,
            (struct sockaddr *)&client_address,
            &client_length);

        if (client_socket < 0)
        {
            perror("accept");
            continue;
        }

        client_socket_ptr = malloc(sizeof(int));

        if (client_socket_ptr == NULL)
        {
            fprintf(stderr,
                    "Could not allocate client socket memory\n");

            close(client_socket);
            continue;
        }

        *client_socket_ptr = client_socket;

        if (pthread_create(&thread,
                           NULL,
                           handle_client,
                           client_socket_ptr) != 0)
        {
            printf("Could not create thread\n");

            log_error("Could not create client thread");

            close(client_socket);

            free(client_socket_ptr);
            close(client_socket);

            continue;
        }


        /*
         * Detach thread so that it
         * automatically cleans up
         */
        pthread_detach(thread_id);
    }


    close(proxy_socket);

    return 0;
}
