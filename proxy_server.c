#include "proxy_parse.h"

#include <asm-generic/socket.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netdb.h>

#define PORT 8000
#define MAX_CLIENTS 10
#define MAX_BYTES 4096

void panic(char * msg) {
  perror(msg);
  exit(1);
}


typedef struct {
  char * data;
} cache_element;


unsigned int port_number = PORT;
unsigned int proxy_socket_fd = 0;


/* Create a new socket and initiate connection with dest_host_addr at dest_port.
   Returns a file descriptor for the new socket, or -1 for errors.  */
int get_dest_socket(char * dest_host_addr, unsigned int dest_port) {
  int dest_socket_fd = socket(AF_INET, SOCK_STREAM, 0);

  if (dest_socket_fd < 0) {
    printf("Failed to create socket for destination server.\n");
    return -1;
  }

  struct hostent * dest_host = gethostbyname(dest_host_addr);

  if (dest_host == NULL) {
    fprintf(stderr, "No such host exists.\n");
    return -1;
  }

  struct sockaddr_in server_addr;
  bzero(&server_addr, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(dest_port);
  memcpy(&server_addr.sin_addr.s_addr, dest_host->h_addr, dest_host->h_length);

  /* Debug output */
  printf("dest_host->h_addr_list (first item): %s\n", dest_host->h_addr);
  /* ************ */

  if (connect(dest_socket_fd, (struct sockaddr *)&server_addr, (socklen_t)sizeof(server_addr)) < 0) {
    fprintf(stderr, "Failed to connect to destination server.\n");
    return -1;
  }

  return dest_socket_fd;
}


int handle_client_request(int * client_socket_fd, struct ParsedRequest * parsed_req) {
  /* buffer to store the request to be dispatched */
  char * req_buf = malloc(sizeof(char) * MAX_BYTES);
  /* build http request */
  strcpy(req_buf, "GET ");
  strcat(req_buf, parsed_req->path);
  strcat(req_buf, " ");
  strcat(req_buf, parsed_req->version);
  strcat(req_buf, "\r\n");

  size_t req_buf_len = strlen(req_buf);

  if (ParsedHeader_set(parsed_req, "Connection", "close") < 0) {
    printf("Unable to set header key on ParsedRequest object.\n");
  }

  if (ParsedRequest_unparse_headers(parsed_req, req_buf + req_buf_len, MAX_BYTES - req_buf_len) < 0) {
    printf("Unable to unparse ParsedRequest object into buffer.\n");
  }

  /* buffer length must be updated due to unparsing of headers */
  req_buf_len = strlen(req_buf);

  unsigned int dest_server_port = (parsed_req->port != NULL) ? atoi(parsed_req->port) : 80;

  int dest_socket_fd = get_dest_socket(parsed_req->host, dest_server_port);
  if (dest_socket_fd < 0) { return -1; }

  /* debug output */
  printf("\nRequest buffer:\n%s", req_buf);
  /* ************ */

  if (send(dest_socket_fd, req_buf, req_buf_len, 0) < 0) {
    fprintf(stderr, "Failed to send request to destination over socket.\n");
    return -1;
  }
  
  char * res_buf = req_buf;
  size_t res_buf_len = MAX_BYTES;
  bzero(res_buf, res_buf_len);

  int len_data_recieved = recv(dest_socket_fd, res_buf, res_buf_len - 1, 0);

  /* debug output */
  printf("\nResponse buffer:\n%s", res_buf);
  /* ************ */

  while (len_data_recieved > 0) {
    if (send(*client_socket_fd, res_buf, len_data_recieved, 0) < 0) {
      fprintf(stderr, "Failed to send data to client.\n");
      return -1;
    }

    bzero(res_buf, res_buf_len);

    len_data_recieved = recv(dest_socket_fd, res_buf, res_buf_len - 1, 0);
  }

  return len_data_recieved;
}


/* Checks if the HTTP version of the request is either 1 or similar to 1.
   Returns 1 if condition is TRUE and 0 if condition is FALSE.  */
int check_http_version(char * http_version) {
  // check if HTTP version is 1 (or similar to 1)
  return !strncmp(http_version, "HTTP/1.0", 8) || !strncmp(http_version, "HTTP/1.1", 8);
}


void handle_client_connection(int * client_socket_fd){
  char * client_req_buffer = (char *)calloc(MAX_BYTES, sizeof(char));
  bzero(client_req_buffer, MAX_BYTES);

  unsigned int len_client_req = 0, is_data_recieved = 0;

  do {
    is_data_recieved = recv(*client_socket_fd, client_req_buffer + len_client_req, MAX_BYTES - len_client_req, 0);
    len_client_req = strlen(client_req_buffer);

    /* Debug output */
    /*printf("Is data recieved: %d\n", is_data_recieved);*/
    /*printf("Client request length: %d\n", len_client_req);*/
    /*printf("Client req buffer :-\n%s\n", client_req_buffer);*/
    /*printf("Is strstr NULL? : %d\n", (strstr(client_req_buffer, "\r\n\r\n") == NULL));*/
    /* ************ */

    if (strstr(client_req_buffer, "\r\n\r\n") != NULL) {
      printf("End of client request reached, breaking loop...\n");
      break;
    }
  } while (is_data_recieved > 0);

  if (is_data_recieved > 0) {
    struct ParsedRequest * parsed_req = ParsedRequest_create();

    if (ParsedRequest_parse(parsed_req, client_req_buffer, len_client_req)  == 0) {
      if (!strcmp(parsed_req->method, "GET")) {
        if (parsed_req->host && parsed_req->path && check_http_version(parsed_req->version)) {
          handle_client_request(client_socket_fd, parsed_req);
        }
        else {
          printf("Bad request, or HTTP version other than 1.\n");
        }
      }
      else {
        printf("This application does not support processing any request method apart from 'GET'.\n");
      }
    }
    else {
      printf("Failed to parse request.\n");
    }

    ParsedRequest_destroy(parsed_req);
  }
  else {
    printf("Client disconnected.\n");
  }

  shutdown(*client_socket_fd, SHUT_RDWR);
  close(*client_socket_fd);
  free(client_req_buffer);
}


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

    handle_client_connection(&client_socket_fd);
  }

  close(proxy_socket_fd);

  return 0;
}
