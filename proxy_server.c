#include "proxy_parse.h"

#include <asm-generic/socket.h>
#include <stdio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#define MAX_CLIENTS 10
#define PORT 8000

void panic(char * msg) {
  perror(msg);
  exit(1);
}

typedef struct {
  char * data;
} cache_element;

unsigned int port_number = PORT;
unsigned int proxy_socket_fd = 0;

int main() {
  int client_socket_fd, client_len;
  struct sockaddr_in server_addr, client_addr;

  printf("Starting proxy server at port %d...\n", port_number);

  if ((proxy_socket_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
    panic("Failed to create socket");
  }
  
  unsigned int reuse = 1;
  if (setsockopt(proxy_socket_fd, SOL_SOCKET, SO_REUSEADDR, (const int *)&reuse, sizeof(reuse)) < 0) {
    panic("Failed to set socket options");
  }

  bzero((char *)&server_addr, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(port_number);
  server_addr.sin_addr.s_addr = INADDR_ANY;

  if (bind(proxy_socket_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
    char msg_buffer[50];
    snprintf(msg_buffer, sizeof(msg_buffer), "Failed to bind to port %d", port_number);

    panic(msg_buffer);
  }

  printf("Binding to port %d\n", port_number);

  if (listen(proxy_socket_fd, MAX_CLIENTS) < 0) {
    panic("Failed to listen");
  }

  while (1) {
    bzero((char *)&client_addr, sizeof(client_addr));
    client_len = sizeof(client_addr);

    if ((client_socket_fd = accept(proxy_socket_fd, (struct sockaddr*)&client_addr, (socklen_t *)&client_len)) < 0) {
      panic("Failed to accept connection.");
    }
  
    /* Printing connected client info */
    struct in_addr ip_addr = client_addr.sin_addr;
    char str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &ip_addr, str, INET_ADDRSTRLEN);
    printf("Connection: %s:%d\n", str, ntohs(client_addr.sin_port));
    /* ****************************** */
  }

  close(proxy_socket_fd);

  return 0;
}
