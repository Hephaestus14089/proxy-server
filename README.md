# proxy-server

A multi-threaded HTTP proxy server written in C, with Least Recently Used cache implemented.

## Feature implementation

- LRU cache using Linked List
- Thread safety in LRU cache critical sections using mutex lock
- Multi-threaded client socket connection handling using non-binary semaphore
