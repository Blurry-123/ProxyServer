#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netdb.h>
#include <pthread.h>
#include <time.h>

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

    while (total_sent < size)
    {
        int sent = send(socket_fd,
                        data + total_sent,
                        size - total_sent,
                        0);

        if (sent <= 0)
            return 0;

        total_sent += sent;
    }

    return 1;
}


void handle_client(int client_socket)
{
    char buffer[BUFFER_SIZE];
    char host[256];

    int bytes_received = recv(client_socket,
                              buffer,
                              BUFFER_SIZE - 1,
                              0);

    if (bytes_received <= 0)
    {
        log_error("Failed to receive request from client");

        close(client_socket);
        return;
    }

    buffer[bytes_received] = '\0';

    printf("\n----- HTTP REQUEST -----\n");
    printf("%s\n", buffer);


    /*
     * Get destination host from HTTP request
     */
    if (!get_host(buffer, host))
    {
        printf("Could not find Host header\n");

        log_error("Could not find Host header");

        close(client_socket);
        return;
    }

    printf("Requested host: %s\n", host);

    log_request(host);


    /*
     * Access control
     */
    if (is_blocked(host))
{
    printf("ACCESS CONTROL: Request blocked\n");

    const char *blocked_message =
        "<html><body>Access Denied</body></html>";

    int blocked_length = (int)strlen(blocked_message);

    char blocked_response[512];

    snprintf(blocked_response,
             sizeof(blocked_response),
             "HTTP/1.1 403 Forbidden\r\n"
             "Content-Type: text/html\r\n"
             "Content-Length: %d\r\n"
             "Connection: close\r\n"
             "\r\n"
             "%s",
             blocked_length,
             blocked_message);

    send_all(client_socket,
             blocked_response,
             strlen(blocked_response));

    log_info("ACCESS CONTROL: Request blocked");

    close(client_socket);
    return;
}

    /*
     * Create cache key
     */
    char cache_key[BUFFER_SIZE];

    strncpy(cache_key,
            buffer,
            sizeof(cache_key) - 1);

    cache_key[sizeof(cache_key) - 1] = '\0';


    /*
     * Check cache
     */
    char *cached_response = malloc(MAX_CACHE_SIZE);

    if (cached_response == NULL)
    {
        printf("Could not allocate cache buffer\n");

        log_error("Could not allocate cache buffer");

        close(client_socket);
        return;
    }

    int cached_size = 0;


    if (check_cache(cache_key,
                    cached_response,
                    &cached_size))
    {
        printf("CACHE HIT\n");
        printf("Sending cached response to client\n");

        log_info("Cache hit");

        if (!send_all(client_socket,
                      cached_response,
                      cached_size))
        {
            log_error("Failed to send cached response");
        }

        free(cached_response);

        close(client_socket);
        return;
    }

    printf("CACHE MISS\n");

    log_info("Cache miss");

    free(cached_response);


    /*
     * Create socket for destination server
     */
    int server_socket = socket(AF_INET, SOCK_STREAM, 0);

    if (server_socket < 0)
    {
        perror("Destination socket creation failed");

        log_error("Destination socket creation failed");

        close(client_socket);
        return;
    }


    /*
     * Find destination server
     */
    struct hostent *server = gethostbyname(host);

    if (server == NULL)
    {
        printf("Could not find server: %s\n", host);

        log_error("Could not find destination server");

        close(server_socket);
        close(client_socket);

        return;
    }
    printf("Destination server found: %s\n", server->h_name);


    /*
     * Prepare destination server address
     */
    struct sockaddr_in server_address;

    memset(&server_address, 0, sizeof(server_address));

    server_address.sin_family = AF_INET;

    server_address.sin_port = htons(80);

    memcpy(&server_address.sin_addr,
           server->h_addr,
           server->h_length);


    /*
     * Connect to destination server
     */
    if (connect(server_socket,
                (struct sockaddr *)&server_address,
                sizeof(server_address)) < 0)
    {
        perror("Connection to destination server failed");

        log_error("Connection to destination server failed");

        close(server_socket);
        close(client_socket);

        return;
    }

    printf("Connected to %s\n", host);

    log_info("Connected to destination server");


    /*
     * Forward request to destination server
     */
    if (!send_all(server_socket,
                  buffer,
                  bytes_received))
    {
        perror("Failed to send request");

        log_error("Failed to send request to destination server");

        close(server_socket);
        close(client_socket);

        return;
    }
    shutdown(server_socket, SHUT_WR);

    printf("Request forwarded to server\n");

    log_info("Request forwarded to destination server");


    /*
     * Receive complete response
     */
    int response_capacity = BUFFER_SIZE;

    int total_response_size = 0;

    char *complete_response =
        malloc(response_capacity);

    if (complete_response == NULL)
    {
        printf("Memory allocation failed\n");

        log_error("Memory allocation failed for response");

        close(server_socket);
        close(client_socket);

        return;
    }


    while (1)
    {
        int response_size = recv(server_socket,
                                 buffer,
                                 BUFFER_SIZE,
                                 0);

        if (response_size <= 0)
            break;


        /*
         * Increase memory if necessary
         */
        while (total_response_size + response_size >
               response_capacity)
        {
            response_capacity *= 2;

            char *new_response =
                realloc(complete_response,
                        response_capacity);

            if (new_response == NULL)
            {
                printf("Memory allocation failed\n");

                log_error("Memory reallocation failed");

                free(complete_response);

                close(server_socket);
                close(client_socket);

                return;
            }

            complete_response = new_response;
        }


        memcpy(complete_response + total_response_size,
               buffer,
               response_size);

        total_response_size += response_size;
    }


    printf("Received %d bytes from server\n",
           total_response_size);


    /*
     * Send response back to client
     */
    if (!send_all(client_socket,
                  complete_response,
                  total_response_size))
    {
        printf("Failed to send complete response\n");

        log_error("Failed to send complete response");
    }
    else
    {
        printf("Response sent back to client\n");

        log_info("Response sent back to client");
    }


    /*
     * Save response in cache
     */
    if (total_response_size <= MAX_CACHE_SIZE)
    {
        save_cache(cache_key,
                   complete_response,
                   total_response_size);

        printf("Response saved in cache\n");

        log_info("Response saved in cache");
    }
    else
    {
        printf("Response too large. Not cached.\n");

        log_info("Response too large, not cached");
    }


    free(complete_response);

    close(server_socket);
    close(client_socket);
}


/*
 * Thread function
 *
 * Each client gets its own thread.
 */
void *client_thread(void *arg)
{
    int client_socket;

    clock_t start_time;
    clock_t end_time;

    double time_taken;


    /*
     * Get client socket from argument
     */
    client_socket = *(int *)arg;

    free(arg);


    /*
     * Start performance measurement
     */
    start_time = clock();

    printf("Client thread started\n");

    log_info("Client thread started");


    /*
     * Handle the client
     */
    handle_client(client_socket);


    /*
     * End performance measurement
     */
    end_time = clock();

    time_taken =
        (double)(end_time - start_time)
        / CLOCKS_PER_SEC;


    printf("Request processing time: %.3f seconds\n",
           time_taken);

    log_response_time(time_taken);

    log_info("Client thread finished");


    return NULL;
}


int main()
{
    int proxy_socket;

    int client_socket;

    struct sockaddr_in proxy_address;

    struct sockaddr_in client_address;

    socklen_t client_length =
        sizeof(client_address);


    /*
     * Create proxy socket
     */
    proxy_socket =
        socket(AF_INET, SOCK_STREAM, 0);

    if (proxy_socket < 0)
    {
        perror("Socket creation failed");

        log_error("Proxy socket creation failed");

        return 1;
    }

    printf("Proxy socket created successfully\n");


    /*
     * Allow reuse of proxy port
     */
    int option = 1;

    setsockopt(proxy_socket,
               SOL_SOCKET,
               SO_REUSEADDR,
               &option,
               sizeof(option));


    /*
     * Prepare proxy address
     */
    memset(&proxy_address,
           0,
           sizeof(proxy_address));

    proxy_address.sin_family =
        AF_INET;

    proxy_address.sin_addr.s_addr =
        INADDR_ANY;

    proxy_address.sin_port =
        htons(PROXY_PORT);


    /*
     * Bind proxy socket
     */
    if (bind(proxy_socket,
             (struct sockaddr *)&proxy_address,
             sizeof(proxy_address)) < 0)
    {
        perror("Bind failed");

        log_error("Proxy bind failed");

        close(proxy_socket);

        return 1;
    }

    printf("Proxy bound to port %d\n",
           PROXY_PORT);


    /*
     * Start listening
     */
    if (listen(proxy_socket, 5) < 0)
    {
        perror("Listen failed");

        log_error("Proxy listen failed");

        close(proxy_socket);

        return 1;
    }

    printf("Proxy server is listening...\n");

    printf("Waiting for HTTP clients...\n");


    /*
     * Accept clients continuously
     */
    while (1)
    {
        pthread_t thread_id;

        int *client_socket_ptr;


        client_length =
            sizeof(client_address);


        client_socket =
            accept(proxy_socket,
                   (struct sockaddr *)&client_address,
                   &client_length);


        if (client_socket < 0)
        {
            perror("Accept failed");

            log_error("Accept failed");

            continue;
        }


        printf("\nClient connected!\n");

        log_info("New client connected");


        /*
         * Allocate memory for client socket
         */
        client_socket_ptr =
            malloc(sizeof(int));


        if (client_socket_ptr == NULL)
        {
            printf("Memory allocation failed\n");

            log_error("Memory allocation failed for client socket");

            close(client_socket);

            continue;
        }


        *client_socket_ptr =
            client_socket;


        /*
         * Create a new thread
         */
        if (pthread_create(&thread_id,
                           NULL,
                           client_thread,
                           client_socket_ptr) != 0)
        {
            printf("Could not create thread\n");

            log_error("Could not create client thread");

            close(client_socket);

            free(client_socket_ptr);

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
