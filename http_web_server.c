#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/time.h>
#include <string.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netdb.h>
#include <pthread.h>
#include <assert.h>
#include <ctype.h>

typedef struct {
    int port;
    int socket_fd;
    struct sockaddr_in sockaddr;
} host_config;

typedef struct client_connection client_connection;

struct client_connection {
    int socket_fd;
    int thread_id;
    struct sockaddr_in address;
    char* ip;
    client_connection* client_connections;
    int alive;
} ;

typedef struct {
    client_connection from;
    char* content;
    client_connection to;
    int sid_to;
} socket_message;

#define BUFFER_LENGTH 1024

int new_sock_fd()
{
    int domain = AF_INET; 
    int type = SOCK_STREAM; 
    int socket_fd = socket(domain, type, 0);
    if (socket_fd == -1) {
        exit(EXIT_FAILURE);
        printf("Failure on creation of socket.");
    }
    return socket_fd;
}

int port_from_args(int argc, char** argv)
{
    if (argc == 1) {
        printf("Port is required.");
        exit(EXIT_FAILURE);
    }
    int port = atoi(argv[1]);
    assert(port > 0);
    return port;
}

void bind_socket_to_address(host_config host_config)
{
	host_config.sockaddr.sin_family = AF_INET;
	host_config.sockaddr.sin_addr.s_addr = INADDR_ANY;
	host_config.sockaddr.sin_port = htons(host_config.port);
    int bind_result = bind(host_config.socket_fd, (struct sockaddr*)&host_config.sockaddr, sizeof(host_config.sockaddr));
    if (bind_result == -1) {
        shutdown(host_config.socket_fd, SHUT_RDWR);
		perror("bind failed");
		exit(EXIT_FAILURE);        
    }
}

void start_listening(host_config host_config)
{
    int max_connections = 100;
    if (listen(host_config.socket_fd, max_connections) == -1) {
        shutdown(host_config.socket_fd, SHUT_RDWR);
		perror("listen");
		exit(EXIT_FAILURE);
	}
}

client_connection incoming_connection(host_config host_config)
{
    client_connection client_connection;
    struct sockaddr_in sockaddr;
    int addr_len;
    int socket_fd;

    addr_len = sizeof(sockaddr);
    client_connection.socket_fd = accept(host_config.socket_fd, (struct sockaddr*) &sockaddr, (socklen_t*) &addr_len);
    if (client_connection.socket_fd == -1) {
        shutdown(host_config.socket_fd, SHUT_RDWR);
        perror("accept");
        exit(EXIT_FAILURE);
    }
    client_connection.ip = inet_ntoa(sockaddr.sin_addr);
    client_connection.address = sockaddr;
    client_connection.alive = 1;
    printf("Incoming connection from %s:%d with socket_id %d\n", client_connection.ip, ntohs(client_connection.address.sin_port), client_connection.socket_fd);
    return client_connection;
}


void handle_connection(client_connection client_connection)
{
    int buffer_bytes;
    for (char buffer[BUFFER_LENGTH] = { 0 };; memset(buffer, 0x0, BUFFER_LENGTH)) {
        buffer_bytes = read(client_connection.socket_fd, buffer, 1024);
        if (buffer_bytes) {
            printf("%s:%d says: %s\n", client_connection.ip, ntohs(client_connection.address.sin_port), buffer);
        }
        if (!strcmp(buffer, "FI\r\n")) break;
    }
    close(client_connection.socket_fd);
    printf("Connection closed with %d.\n", client_connection.socket_fd);
}



socket_message msg_from_buffer(char* buffer, client_connection from)
{
    socket_message msg;
    client_connection client_connection;
    int i, j, k, ascii_code;
    char message[1000] = {0};
    char to_str[5] = {0};
    int map_message;

    msg.from = from;
    map_message = 1;
    j = 0;
    k = 0;
    for(int i = 0; buffer[i] != '\0'; i++) {
        if (buffer[i] == ';') {
            map_message = 0;
            continue;
        }
        if (map_message) {
            message[j++] = buffer[i];
        } else if (isdigit(buffer[i])) {
            to_str[k++] = buffer[i];
        }
    }
    strcat(message, "\n");
    msg.content = message;
    msg.sid_to = atoi(to_str);

    for(int i = 0; i < 10000; i++) {
        client_connection = from.client_connections[i];
        if (client_connection.alive && client_connection.socket_fd == msg.sid_to) {
            msg.to = client_connection;
            return msg;
        }
    }

    msg.sid_to = 0; // In case of failure or invalid destination
    return msg;
}

int send_message(socket_message msg)
{
    printf("Sending message: %s\n", msg.content);
    return send(msg.to.socket_fd, msg.content, strlen(msg.content), 0);
}

void* handle_connection_routine(void *client_connection_void_ptr)
{
    client_connection *client_connection_ptr = client_connection_void_ptr;
    client_connection client_connection = *client_connection_ptr;
    socket_message msg;
    int buffer_bytes;

    printf("Starting Thread from client %d\n", client_connection.socket_fd);

    msg.content = "0\n";
    msg.to = client_connection;
    msg.sid_to = client_connection.socket_fd;

    if (send_message(msg) == -1) {
        printf("Failure on communication with client.\n");
    }

    for (char buffer[BUFFER_LENGTH] = { 0 };; memset(buffer, 0x0, BUFFER_LENGTH)) {
        buffer_bytes = read(client_connection.socket_fd, buffer, 1024);
        if (buffer_bytes) {
            printf("%s:%d says: %s\n", client_connection.ip, ntohs(client_connection.address.sin_port), buffer);
            msg = msg_from_buffer(buffer, client_connection);
            if (msg.sid_to) {
                if (send_message(msg) > 0) {
                    printf("Message sent successfully.\n");
                } else {
                    printf("Failure on message sending.\n");
                }
            } else {
                printf("Unavailable destination.\n");
            }
        }
        if (!strcmp(buffer, "\r\n")) break;
    }
    client_connection.alive = 0;
    close(client_connection.socket_fd);
    printf("Connection closed with %d.\n", client_connection.socket_fd);
}

void handle_connections(host_config host_config)
{
    client_connection *client_connections;
    int max_connections;
    int i;
    pthread_t *threads;

    max_connections = 10000;
    client_connections = (client_connection *) malloc(sizeof(client_connection) * max_connections);
    threads = (pthread_t *) malloc(sizeof(pthread_t) * max_connections);

    for (i = 0;;) {
        client_connections[i] = incoming_connection(host_config);
        client_connections[i].client_connections = client_connections;
        pthread_create(&threads[i], NULL, handle_connection_routine, (void *) &client_connections[i]);
        i++;
    }
    free(client_connections);
    shutdown(host_config.socket_fd, SHUT_RDWR);
}

int main(int argc, char** argv)
{
    host_config host_config;

    host_config.socket_fd = new_sock_fd();
    host_config.port = port_from_args(argc, argv);
    bind_socket_to_address(host_config);
    start_listening(host_config);
    handle_connections(host_config);
    
    return EXIT_SUCCESS;
}