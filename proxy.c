#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netdb.h>

#include "cache.h"
#include "access_control.h"

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
        close(client_socket);
        return;
    }

    buffer[bytes_received] = '\0';

    printf("\n----- HTTP REQUEST -----\n");
    printf("%s\n", buffer);


    if (!get_host(buffer, host))
    {
        printf("Could not find Host header\n");

        close(client_socket);
        return;
    }

    printf("Requested host: %s\n", host);

    if (is_blocked(host))
    {
        const char *blocked_message =
            "HTTP/1.1 403 Forbidden\r\n"
            "Content-Type: text/html\r\n"
            "Content-Length: 45\r\n"
            "Connection: close\r\n"
            "\r\n"
            "<html><body>Access Denied</body></html>";

        send_all(client_socket,
                 blocked_message,
                 strlen(blocked_message));

        printf("ACCESS CONTROL: Request blocked\n");

        close(client_socket);
        return;
    }


    char cache_key[BUFFER_SIZE];

    strncpy(cache_key,
            buffer,
            sizeof(cache_key) - 1);

    cache_key[sizeof(cache_key) - 1] = '\0';



    char *cached_response = malloc(MAX_CACHE_SIZE);

    if (cached_response == NULL)
    {
        printf("Could not allocate cache buffer\n");

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

        send_all(client_socket,
                 cached_response,
                 cached_size);

        free(cached_response);

        close(client_socket);
        return;
    }

    printf("CACHE MISS\n");

    free(cached_response);

    int server_socket = socket(AF_INET, SOCK_STREAM, 0);

    if (server_socket < 0)
    {
        perror("Destination socket creation failed");

        close(client_socket);
        return;
    }


   
    struct hostent *server = gethostbyname(host);

    if (server == NULL)
    {
        printf("Could not find server: %s\n", host);

        close(server_socket);
        close(client_socket);

        return;
    }


    struct sockaddr_in server_address;

    memset(&server_address, 0, sizeof(server_address));

    server_address.sin_family = AF_INET;
    server_address.sin_port = htons(80);

    memcpy(&server_address.sin_addr,
           server->h_addr,
           server->h_length);


    if (connect(server_socket,
                (struct sockaddr *)&server_address,
                sizeof(server_address)) < 0)
    {
        perror("Connection to destination server failed");

        close(server_socket);
        close(client_socket);

        return;
    }

    printf("Connected to %s\n", host);



    if (!send_all(server_socket,
                  buffer,
                  bytes_received))
    {
        perror("Failed to send request");

        close(server_socket);
        close(client_socket);

        return;
    }

    printf("Request forwarded to server\n");


    int response_capacity = BUFFER_SIZE;
    int total_response_size = 0;

    char *complete_response =
        malloc(response_capacity);

    if (complete_response == NULL)
    {
        printf("Memory allocation failed\n");

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


        /* Increase memory if necessary */
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



    if (!send_all(client_socket,
                  complete_response,
                  total_response_size))
    {
        printf("Failed to send complete response\n");
    }
    else
    {
        printf("Response sent back to client\n");
    }



    if (total_response_size <= MAX_CACHE_SIZE)
    {
        save_cache(cache_key,
                   complete_response,
                   total_response_size);

        printf("Response saved in cache\n");
    }
    else
    {
        printf("Response too large. Not cached.\n");
    }


    free(complete_response);


    close(server_socket);
    close(client_socket);
}


int main()
{
    int proxy_socket;
    int client_socket;

    struct sockaddr_in proxy_address;
    struct sockaddr_in client_address;

    socklen_t client_length =
        sizeof(client_address);



    proxy_socket =
        socket(AF_INET, SOCK_STREAM, 0);

    if (proxy_socket < 0)
    {
        perror("Socket creation failed");
        return 1;
    }

    printf("Proxy socket created successfully\n");


    int option = 1;

    setsockopt(proxy_socket,
               SOL_SOCKET,
               SO_REUSEADDR,
               &option,
               sizeof(option));



    memset(&proxy_address,
           0,
           sizeof(proxy_address));

    proxy_address.sin_family = AF_INET;

    proxy_address.sin_addr.s_addr =
        INADDR_ANY;

    proxy_address.sin_port =
        htons(PROXY_PORT);


    if (bind(proxy_socket,
             (struct sockaddr *)&proxy_address,
             sizeof(proxy_address)) < 0)
    {
        perror("Bind failed");

        close(proxy_socket);
        return 1;
    }

    printf("Proxy bound to port %d\n",
           PROXY_PORT);



    if (listen(proxy_socket, 5) < 0)
    {
        perror("Listen failed");

        close(proxy_socket);
        return 1;
    }

    printf("Proxy server is listening...\n");
    printf("Waiting for HTTP clients...\n");



    while (1)
    {
        client_length =
            sizeof(client_address);

        client_socket =
            accept(proxy_socket,
                   (struct sockaddr *)&client_address,
                   &client_length);

        if (client_socket < 0)
        {
            perror("Accept failed");
            continue;
        }

        printf("\nClient connected!\n");


        handle_client(client_socket);
    }


    close(proxy_socket);

    return 0;
}
