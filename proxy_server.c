#include "proxy_parse.h"

#include <bits/pthreadtypes.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netdb.h>
#include <time.h>
#include <pthread.h>
#include <semaphore.h>

#define PORT 8000
#define MAX_CLIENTS 10
#define MAX_BYTES 4096
#define MAX_CACHE_ELEMENT_SIZE 10 * (1 << 10)
#define MAX_CACHE_SIZE 10 * MAX_CACHE_ELEMENT_SIZE

void panic(char * msg) {
  perror(msg);
  exit(1);
}


struct cache_element {
  char * data;
  unsigned int data_size;
  char * url;
  time_t last_used_at;
  struct cache_element * next;
  struct cache_element * prev;
};

struct cache_list {
  struct cache_element * head;
  struct cache_element * tail;
  unsigned int length;
};

struct cache_element * find_cache_element(char * url);
void add_cache_element(char * data, unsigned int data_size, char * url);
void remove_cache_element();
void display_cache_list();

struct cache_element * head;
struct cache_element * tail;
unsigned int cache_size = 0;

sem_t semaphore;
unsigned int port_number = PORT;
unsigned int proxy_socket_fd = 0;

char * get_url_str(struct ParsedRequest * parsed_req) {
  unsigned int protocol_len = strlen(parsed_req->protocol);
  unsigned int host_len = strlen(parsed_req->host);
  unsigned int port_len = (parsed_req->port) ? strlen(parsed_req->port) : 0;
  unsigned int path_len = strlen(parsed_req->path);

  // [protocol]://[host]:[port][path]\0
  char * url_str = malloc(protocol_len + 3 + host_len + 1 + port_len + path_len + 1);
  
  /* build url string */
  unsigned int idx = 0;

  for (unsigned int i = 0; i < protocol_len; i++) {
    url_str[idx++] = parsed_req->protocol[i];
  }
  strcpy((url_str + idx), "://");
  idx += 3;

  for (unsigned int i = 0; i < host_len; i++) {
    url_str[idx++] = parsed_req->host[i];
  }

  if (port_len != 0) {
    url_str[idx++] = ':';
  
    for (unsigned int i = 0; i < port_len; i++) {
      url_str[idx++] = parsed_req->port[i];
    }
  }

  for (unsigned int i = 0; i < path_len; i++) {
    url_str[idx++] = parsed_req->path[i];
  }

  url_str[idx++] = '\0';

  return url_str;
}

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
  /*printf("\n");*/
  /*printf("dest_host->h_length: %d\n", dest_host->h_length);*/
  /*printf("dest_host->h_addr_list (first item): %s\n", inet_ntoa(*((struct in_addr *)dest_host->h_addr)));*/
  /* ************ */

  if (connect(dest_socket_fd, (struct sockaddr *)&server_addr, (socklen_t)sizeof(server_addr)) < 0) {
    fprintf(stderr, "Failed to connect to destination server.\n");
    return -1;
  }

  return dest_socket_fd;
}


int handle_client_request(int * client_socket_fd, struct ParsedRequest * parsed_req) {
  /* Debug output */
  printf("\n");
  printf("parsed_req->host : %s\n", parsed_req->host);
  printf("parsed_req->path : %s\n", parsed_req->path);
  printf("parsed_req->protocol : %s\n", parsed_req->protocol);
  printf("\n");
  /* ************ */

  /* get url string to be used while storing cache */
  char * url_str = get_url_str(parsed_req);

  /* Debug output */
  printf("url_str: %s\n", url_str);
  /* ************ */

  /* check if present in cache */
  struct cache_element * element = find_cache_element(url_str);

  if (element != NULL) {
    /* Debug output */
    /*printf("element->data_size: %d\n", element->data_size);*/
    /*printf("element->data:\n%s\n", element->data);*/
    /* ************ */
    
    if (send(*client_socket_fd, element->data, element->data_size, 0) < 0) {
      fprintf(stderr, "Failed to send data to client.\n");
      return -1;
    }

    display_cache_list();  // debug output

    return element->data_size;
  }

  /* buffer to store the request to be dispatched */
  char * req_buf = calloc(MAX_BYTES, sizeof(char));
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
  /*printf("\nRequest buffer:\n%s", req_buf);*/
  /* ************ */

  if (send(dest_socket_fd, req_buf, req_buf_len, 0) < 0) {
    fprintf(stderr, "Failed to send request to destination over socket.\n");
    return -1;
  }
  
  char * res_buf = req_buf;
  size_t res_buf_len = MAX_BYTES;
  bzero(res_buf, res_buf_len);

  int len_data_recieved = recv(dest_socket_fd, res_buf, res_buf_len - 1, 0);

  /* initialise buffer to store response for caching */
  char * cache_buf = NULL;
  unsigned int cache_buf_idx = 0;

  /* debug output */
  /*printf("\nlen_data_recieved: %d\n", len_data_recieved);*/
  /*printf("\nResponse buffer:\n%s", res_buf);*/
  /* ************ */

  while (len_data_recieved > 0) {
    cache_buf = realloc(cache_buf, (cache_buf_idx + len_data_recieved));
    memcpy((cache_buf + cache_buf_idx), res_buf, len_data_recieved);
    cache_buf_idx += len_data_recieved;

    /* Debug output */
    /*printf("\ncache_buf:\n%s\n", cache_buf);*/
    /*printf("\n");*/
    /* ************ */
    
    if (send(*client_socket_fd, res_buf, len_data_recieved, 0) < 0) {
      fprintf(stderr, "Failed to send data to client.\n");
      return -1;
    }

    bzero(res_buf, res_buf_len);

    len_data_recieved = recv(dest_socket_fd, res_buf, res_buf_len - 1, 0);

    /* debug output */
    /*printf("\nlen_data_recieved: %d\n", len_data_recieved);*/
    if (len_data_recieved == 0) {
      printf("End of response reached... exiting loop.\n\n");
    }
    /*else {  */
    /*  printf("\nres_buf:\n%s\n", res_buf);*/
    /*}*/
    /* ************ */
  }

  /* append null character */
  cache_buf[cache_buf_idx++] = '\0';

  add_cache_element(cache_buf, cache_buf_idx, url_str);

  /* debug output */
  display_cache_list();
  /* ************ */

  return cache_buf_idx;
}


/* Checks if the HTTP version of the request is either 1 or similar to 1.
   Returns 1 if condition is TRUE and 0 if condition is FALSE.  */
int check_http_version(char * http_version) {
  // check if HTTP version is 1 (or similar to 1)
  return !strncmp(http_version, "HTTP/1.0", 8) || !strncmp(http_version, "HTTP/1.1", 8);
}

/* Sends HTTP response over socket SOCKET_FD according to HTTP status code ERR_CODE.
   Does nothing if an invalid status code is passed.  */
void sendErrMsg(int socket_fd, int err_code) {
  char res_str[1024]; // to store response string
  char err_desc[50]; // to store error description string
  char currentTime[50];
  time_t now = time(0);

	struct tm data = *gmtime(&now);
	strftime(currentTime,sizeof(currentTime),"%a, %d %b %Y %H:%M:%S %Z", &data);

	switch(err_code) {
		case 400:
      snprintf(err_desc, sizeof(err_desc), "400 Bad Request");
			printf("%s\n", err_desc);
			break;

		case 403:
      snprintf(err_desc, sizeof(err_desc), "403 Forbidden");
			printf("%s\n", err_desc);
			break;

		case 404:
      snprintf(err_desc, sizeof(err_desc), "404 Not Found");
			printf("%s\n", err_desc);
			break;

		case 500:
      snprintf(err_desc, sizeof(err_desc), "500 Internal Server Error");
			printf("%s\n", err_desc);
			break;

		case 501:
      snprintf(err_desc, sizeof(err_desc), "501 Not Implemented");
			printf("%s\n", err_desc);
			break;

		case 505:
      snprintf(err_desc, sizeof(err_desc), "505 HTTP Version Not Supported");
			printf("%s\n", err_desc);
			break;

    default:
      fprintf(stderr, "Invalid error status code.\n");
      return;
	}

  snprintf(res_str, sizeof(res_str), "HTTP/1.1 %s\r\nConnection: close\r\nContent-Length: 0\r\nServer: Proxy-Server\r\nDate: %s\r\n\r\n", err_desc, currentTime);

  /* Debug output */
  printf("\n%s\n", res_str);
  /* ************ */

	send(socket_fd, res_str, strlen(res_str), 0);
}


void handle_client_connection(int * client_socket_fd) {
  sem_wait(&semaphore);

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
          if (handle_client_request(client_socket_fd, parsed_req) == -1) {
            sendErrMsg(*client_socket_fd, 500);
          }
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

  sem_post(&semaphore);

  free(client_req_buffer);
}


int main() {
  int client_socket_fd, client_len;
  struct sockaddr_in server_addr, client_addr;

  sem_init(&semaphore, 0, MAX_CLIENTS);
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

  pthread_t active_threads[MAX_CLIENTS];
  unsigned int current_thread_idx;

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

    pthread_create(&active_threads[current_thread_idx++], NULL, (void *)handle_client_connection, (void *)&client_socket_fd);
    current_thread_idx %= MAX_CLIENTS;
  }

  close(proxy_socket_fd);

  return 0;
}

/* Returns pointer to cache_element associated to the URL,
   if present in cache, else returns NULL.  */
struct cache_element * find_cache_element(char * url) {
  struct cache_element * curr = head;

  while (curr != NULL) {
    if (!strcmp(curr->url, url)) {
      printf("url found in cache.\n");
      printf("last used at: %ld\n", curr->last_used_at);
      /* update last used time */
      curr->last_used_at = time(NULL);
    
      /* move to head if not already at head */
      if (curr != head) {
        /* adjust tail if curr is last but not only element */
        if (curr == tail) {
          tail = curr->prev;
        }

        (curr->prev)->next = curr->next;
        curr->prev = NULL;
        
        head->prev = curr;
        curr->next = head;

        head = curr;
      }

      return head;
    }

    curr = curr->next;
  }

  printf("url not found in cache.\n");
  return NULL;
}

/* frees memory used by the cache_element  */
void free_cache_element(struct cache_element * element) {
  free(element->data);
  free(element->url);
  free(element);
}

/* Adds cache_element to the cache.  */
void add_cache_element(char * data, unsigned int data_size, char * url) {
  unsigned int element_size = 1 + data_size + strlen(url) + sizeof(struct cache_element);

  if (element_size > MAX_CACHE_ELEMENT_SIZE) {
    printf("Will not be stored in cache as data size exceeds cache element size limit.\n");
    return;
  }
  
  while (cache_size + element_size > MAX_CACHE_SIZE) {
    remove_cache_element();
  }

  struct cache_element * element = malloc(sizeof(struct cache_element));
  element->url = malloc(strlen(url) + 1);
  strcpy(element->url, url);
  element->data = malloc(data_size + 1);
  strcpy(element->data, data);
  element->data_size = data_size;
  element->last_used_at = time(NULL);

  element->prev = NULL;
  element->next = head;

  /* if only element in cache */
  if (tail == NULL) {
    tail = element;
  }
  else {
    head->prev = element;
  }

  head = element;

  /* update cache size */
  cache_size += element_size;
}

/* Removes least recently used element from the cache.  */
void remove_cache_element() {
  if (head == NULL) {
    printf("Unable to remove from cache as cache is empty.\n");
    return;
  }

  /* least recently used cache element will always be at tail */
  struct cache_element * temp = tail;

  printf("Removing cache element with url: %s\n", tail->url);

  if (head == tail) { // only element in cache
    head = NULL;
    tail = NULL;
  }
  else {
    (tail->prev)->next = NULL;
    tail = tail->prev;
  }

  /* update cache size */
  unsigned int total_element_size = temp->data_size + strlen(temp->url) + 1 + sizeof(struct cache_element);
  cache_size -= total_element_size;

  free_cache_element(temp);
}

void display_cache_list() {
  struct cache_element * ptr = head;

  printf("Cache list :-\n");

  while (ptr != NULL) {
    printf("url: %s\n", ptr->url);
    /*printf("data:\n%s\n", ptr->data);*/
    printf("last used at: %ld\n", ptr->last_used_at);
    printf("\n");
    ptr = ptr->next;
  }

  printf("\n");
}
