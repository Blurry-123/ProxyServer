#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netdb.h>

#define PROXY_PORT 8888
#define BUFFER_SIZE 8192

// Get the hostname from the Host header
int get_host(char *request, char *host)
{
    char *start;
    char *end;

    start = strstr(request, "Host:");

    if (start == NULL)
        return 0;

    start += 5;

    // Skip spaces
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

    // Remove port if Host is like example.com:80
    char *colon = strchr(host, ':');

    if (colon != NULL)
        *colon = '\0';

    return 1;
}

// Handle one client
void handle_client(int client_socket)
{
    char buffer[BUFFER_SIZE];
    char host[256];

    // Receive HTTP request from browser/client
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

    // Find the Host header
    if (!get_host(buffer, host))
    {
        printf("Could not find Host header\n");
        close(client_socket);
        return;
    }

    printf("Requested host: %s\n", host);

    // Create socket for destination server
    int server_socket = socket(AF_INET, SOCK_STREAM, 0);

    if (server_socket < 0)
    {
        perror("Destination socket creation failed");
        close(client_socket);
        return;
    }

    // Find IP address of destination server
    struct hostent *server = gethostbyname(host);

    if (server == NULL)
    {
        printf("Could not find server: %s\n", host);
        close(server_socket);
        close(client_socket);
        return;
    }

    // Prepare destination address
    struct sockaddr_in server_address;

    memset(&server_address, 0, sizeof(server_address));

    server_address.sin_family = AF_INET;
    server_address.sin_port = htons(80);

    memcpy(&server_address.sin_addr,
           server->h_addr,
           server->h_length);

    // Connect proxy to destination server
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

    // Send client's request to destination server
    if (send(server_socket,
             buffer,
             bytes_received,
             0) < 0)
    {
        perror("Failed to send request");
        close(server_socket);
        close(client_socket);
        return;
    }

    printf("Request forwarded to server\n");

    // Receive response from destination server
    // and send it back to client
    while (1)
    {
        int response_size = recv(server_socket,
                                 buffer,
                                 BUFFER_SIZE,
                                 0);

        if (response_size <= 0)
            break;

        int total_sent = 0;

        while (total_sent < response_size)
        {
            int sent = send(client_socket,
                            buffer + total_sent,
                            response_size - total_sent,
                            0);

            if (sent <= 0)
                break;

            total_sent += sent;
        }
    }

    printf("Response sent back to client\n");

    // Close both connections
    close(server_socket);
    close(client_socket);
}

int main()
{
    int proxy_socket;
    int client_socket;

    struct sockaddr_in proxy_address;
    struct sockaddr_in client_address;

    socklen_t client_length = sizeof(client_address);

    // 1. Create proxy socket
    proxy_socket = socket(AF_INET, SOCK_STREAM, 0);

    if (proxy_socket < 0)
    {
        perror("Socket creation failed");
        return 1;
    }

    printf("Proxy socket created successfully\n");

    // Allow reuse of port
    int option = 1;

    setsockopt(proxy_socket,
               SOL_SOCKET,
               SO_REUSEADDR,
               &option,
               sizeof(option));

    // 2. Configure proxy address
    memset(&proxy_address, 0, sizeof(proxy_address));

    proxy_address.sin_family = AF_INET;
    proxy_address.sin_addr.s_addr = INADDR_ANY;
    proxy_address.sin_port = htons(PROXY_PORT);

    // 3. Bind socket to port 8888
    if (bind(proxy_socket,
             (struct sockaddr *)&proxy_address,
             sizeof(proxy_address)) < 0)
    {
        perror("Bind failed");
        close(proxy_socket);
        return 1;
    }

    printf("Proxy bound to port %d\n", PROXY_PORT);

    // 4. Listen for clients
    if (listen(proxy_socket, 5) < 0)
    {
        perror("Listen failed");
        close(proxy_socket);
        return 1;
    }

    printf("Proxy server is listening...\n");
    printf("Waiting for HTTP clients...\n");

    // 5. Accept clients continuously
    while (1)
    {
        client_socket = accept(proxy_socket,
                                (struct sockaddr *)&client_address,
                                &client_length);

        if (client_socket < 0)
        {
            perror("Accept failed");
            continue;
        }

        printf("\nClient connected!\n");

        // Handle client
        handle_client(client_socket);
    }

    close(proxy_socket);

    return 0;
}