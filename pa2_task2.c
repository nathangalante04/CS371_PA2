/*
# Copyright 2026 University of Kentucky
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# SPDX-License-Identifier: Apache-2.0
*/

/*
# Student #1: Nathan Galante
# Student #2: David Eliassen
# Student #3:
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <pthread.h>
#include <errno.h>
#include <fcntl.h>

#define MAX_EVENTS 64
#define MESSAGE_SIZE 32
#define DEFAULT_CLIENT_THREADS 4

#define WINDOW_SIZE 64
#define EPOLL_TIMEOUT_MS 10
#define RETRANSMIT_TIMEOUT_US 100000
#define MAX_SERVER_CLIENTS 4096

char *server_ip = "127.0.0.1";
int server_port = 12345;
int num_client_threads = DEFAULT_CLIENT_THREADS;
int num_requests = 1000000;

typedef struct {
    int seq_num;
    char data[MESSAGE_SIZE - (int)sizeof(int)];
} packet_t;

typedef struct {
    int ack_num;
} ack_t;

typedef struct {
    struct timeval send_time;
    int state;
    packet_t pkt;
} packet_info_t;

typedef struct {
    int epoll_fd;
    int socket_fd;
    long long total_rtt;
    long total_messages;
    float request_rate;
    long tx_cnt;    
    long rx_cnt;      
    long retrans_cnt;  
} client_thread_data_t;

typedef struct {
    int used;
    struct sockaddr_in addr;
    int expected_seq;
    int last_acked;
} client_state_t;

static void die(const char *msg) {
    perror(msg);
    exit(EXIT_FAILURE);
}

static int make_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        return -1;
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        return -1;
    }
    return 0;
}

static long long time_diff_us(struct timeval start, struct timeval end) {
    return (end.tv_sec - start.tv_sec) * 1000000LL +
           (end.tv_usec - start.tv_usec);
}

static int same_client(const struct sockaddr_in *a, const struct sockaddr_in *b) {
    return a->sin_family == b->sin_family &&
           a->sin_port == b->sin_port &&
           a->sin_addr.s_addr == b->sin_addr.s_addr;
}

void *client_thread_func(void *arg) {
    client_thread_data_t *data = (client_thread_data_t *)arg;
    struct epoll_event event, events[MAX_EVENTS];
    ack_t ack_buf;
    struct timeval thread_start, thread_end;

    packet_info_t *window = calloc((size_t)num_requests, sizeof(packet_info_t));
    if (window == NULL) {
        perror("calloc");
        close(data->socket_fd);
        close(data->epoll_fd);
        return NULL;
    }

    data->total_rtt = 0;
    data->total_messages = 0;
    data->request_rate = 0.0f;
    data->tx_cnt = 0;
    data->rx_cnt = 0;
    data->retrans_cnt = 0;

    event.events = EPOLLIN;
    event.data.fd = data->socket_fd;
    if (epoll_ctl(data->epoll_fd, EPOLL_CTL_ADD, data->socket_fd, &event) == -1) {
        perror("epoll_ctl client");
        free(window);
        close(data->socket_fd);
        close(data->epoll_fd);
        return NULL;
    }

    gettimeofday(&thread_start, NULL);

    int base_seq = 0;
    int next_seq = 0;

    while (data->rx_cnt < num_requests) {
        while (next_seq < num_requests && next_seq < base_seq + WINDOW_SIZE) {
            packet_t pkt;
            memset(&pkt, 0, sizeof(pkt));
            pkt.seq_num = next_seq;
            snprintf(pkt.data, sizeof(pkt.data), "MSG-%08d", next_seq);

            gettimeofday(&window[next_seq].send_time, NULL);
            window[next_seq].pkt = pkt;
            window[next_seq].state = 1;

            ssize_t sent = send(data->socket_fd, &pkt, sizeof(pkt), 0);
            if (sent == -1) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                }
                perror("send");
                break;
            }

            data->tx_cnt++;
            next_seq++;
        }

        int n_events = epoll_wait(data->epoll_fd, events, MAX_EVENTS, EPOLL_TIMEOUT_MS);
        if (n_events == -1) {
            if (errno == EINTR) {
                continue;
            }
            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < n_events; i++) {
            if (events[i].data.fd == data->socket_fd) {
                while (1) {
                    struct timeval now;
                    gettimeofday(&now, NULL);

                    ssize_t n = recv(data->socket_fd, &ack_buf, sizeof(ack_buf), 0);
                    if (n == -1) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            break;
                        }
                        perror("recv");
                        break;
                    }

                    if (n == 0) {
                        break;
                    }

                    if (n != (ssize_t)sizeof(ack_buf)) {
                        continue;
                    }

                    if (ack_buf.ack_num >= base_seq && ack_buf.ack_num < num_requests) {
                        for (int s = base_seq; s <= ack_buf.ack_num; s++) {
                            if (window[s].state == 1) {
                                long long rtt = time_diff_us(window[s].send_time, now);
                                data->total_rtt += rtt;
                                data->total_messages++;
                                data->rx_cnt++;
                                window[s].state = 2;
                            }
                        }

                        while (base_seq < num_requests && window[base_seq].state == 2) {
                            base_seq++;
                        }
                    }
                }
            }
        }

        if (base_seq < next_seq && window[base_seq].state == 1) {
            struct timeval now;
            gettimeofday(&now, NULL);

            long long age_us = time_diff_us(window[base_seq].send_time, now);
            if (age_us > RETRANSMIT_TIMEOUT_US) {
                for (int s = base_seq; s < next_seq; s++) {
                    if (window[s].state == 1) {
                        ssize_t sent = send(data->socket_fd, &window[s].pkt, sizeof(packet_t), 0);
                        if (sent == -1) {
                            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                                break;
                            }
                            perror("retransmit send");
                            break;
                        }
                        gettimeofday(&window[s].send_time, NULL);
                        data->retrans_cnt++;
                    }
                }
            }
        }
    }

    gettimeofday(&thread_end, NULL);
    long long elapsed_us = time_diff_us(thread_start, thread_end);
    if (elapsed_us > 0) {
        data->request_rate = (float)data->rx_cnt * 1000000.0f / (float)elapsed_us;
    }

    free(window);
    close(data->socket_fd);
    close(data->epoll_fd);
    return NULL;
}

void run_client() {
    pthread_t threads[num_client_threads];
    client_thread_data_t thread_data[num_client_threads];
    struct sockaddr_in server_addr;

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(server_port);

    if (inet_pton(AF_INET, server_ip, &server_addr.sin_addr) <= 0) {
        die("inet_pton");
    }

    for (int i = 0; i < num_client_threads; i++) {
        thread_data[i].epoll_fd = epoll_create1(0);
        if (thread_data[i].epoll_fd == -1) {
            die("epoll_create1");
        }

        thread_data[i].socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (thread_data[i].socket_fd == -1) {
            die("socket");
        }

        if (make_nonblocking(thread_data[i].socket_fd) == -1) {
            die("make_nonblocking client");
        }

        if (connect(thread_data[i].socket_fd,
                    (struct sockaddr *)&server_addr,
                    sizeof(server_addr)) == -1) {
            die("connect");
        }
    }

    for (int i = 0; i < num_client_threads; i++) {
        if (pthread_create(&threads[i], NULL, client_thread_func, &thread_data[i]) != 0) {
            die("pthread_create");
        }
    }

    long long total_rtt = 0;
    long total_messages = 0;
    float total_request_rate = 0.0f;
    long total_tx_cnt = 0;
    long total_rx_cnt = 0;
    long total_retrans_cnt = 0;

    for (int i = 0; i < num_client_threads; i++) {
        if (pthread_join(threads[i], NULL) != 0) {
            die("pthread_join");
        }

        total_rtt += thread_data[i].total_rtt;
        total_messages += thread_data[i].total_messages;
        total_request_rate += thread_data[i].request_rate;
        total_tx_cnt += thread_data[i].tx_cnt;
        total_rx_cnt += thread_data[i].rx_cnt;
        total_retrans_cnt += thread_data[i].retrans_cnt;

        printf("Thread %d: tx=%ld rx=%ld retrans=%ld avg_rtt=%lld us req_rate=%.2f msg/s\n",
               i,
               thread_data[i].tx_cnt,
               thread_data[i].rx_cnt,
               thread_data[i].retrans_cnt,
               (thread_data[i].total_messages > 0)
                   ? (thread_data[i].total_rtt / thread_data[i].total_messages)
                   : 0,
               thread_data[i].request_rate);
    }

    if (total_messages > 0) {
        printf("Average RTT: %lld us\n", total_rtt / total_messages);
    } else {
        printf("Average RTT: N/A (no ACKs received)\n");
    }

    printf("Total Request Rate: %f messages/s\n", total_request_rate);
    printf("Total Sent: %ld\n", total_tx_cnt);
    printf("Total Received: %ld\n", total_rx_cnt);
    printf("Retransmissions: %ld\n", total_retrans_cnt);
    printf("Lost Packets: %ld\n", total_tx_cnt-total_rx_cnt);
}

void run_server() {
    int server_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (server_fd == -1) {
        die("server socket");
    }

    if (make_nonblocking(server_fd) == -1) {
        die("make_nonblocking server");
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(server_port);

    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1) {
        die("bind");
    }

    int epoll_fd = epoll_create1(0);
    if (epoll_fd == -1) {
        die("epoll_create1 server");
    }

    struct epoll_event event, events[MAX_EVENTS];
    event.events = EPOLLIN;
    event.data.fd = server_fd;

    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &event) == -1) {
        die("epoll_ctl server");
    }

    client_state_t clients[MAX_SERVER_CLIENTS];
    memset(clients, 0, sizeof(clients));

    while (1) {
        int n_events = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        if (n_events == -1) {
            if (errno == EINTR) {
                continue;
            }
            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < n_events; i++) {
            if (events[i].data.fd == server_fd) {
                while (1) {
                    packet_t pkt;
                    struct sockaddr_in client_addr;
                    socklen_t client_len = sizeof(client_addr);

                    ssize_t n = recvfrom(server_fd, &pkt, sizeof(pkt), 0,
                                         (struct sockaddr *)&client_addr, &client_len);

                    if (n == -1) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            break;
                        }
                        perror("recvfrom");
                        break;
                    }

                    if (n != (ssize_t)sizeof(pkt)) {
                        continue;
                    }

                    int idx = -1;
                    for (int j = 0; j < MAX_SERVER_CLIENTS; j++) {
                        if (clients[j].used && same_client(&clients[j].addr, &client_addr)) {
                            idx = j;
                            break;
                        }
                    }

                    if (idx == -1) {
                        for (int j = 0; j < MAX_SERVER_CLIENTS; j++) {
                            if (!clients[j].used) {
                                clients[j].used = 1;
                                clients[j].addr = client_addr;
                                clients[j].expected_seq = 0;
                                clients[j].last_acked = -1;
                                idx = j;
                                break;
                            }
                        }
                    }

                    if (idx == -1) {
                        continue;
                    }

                    if (pkt.seq_num == clients[idx].expected_seq) {
                        clients[idx].last_acked = pkt.seq_num;
                        clients[idx].expected_seq++;
                    }

                    if (clients[idx].last_acked >= 0) {
                        ack_t ack;
                        ack.ack_num = clients[idx].last_acked;

                        if (sendto(server_fd, &ack, sizeof(ack), 0,
                                   (struct sockaddr *)&client_addr, client_len) == -1) {
                            perror("sendto ack");
                        }
                    }
                }
            }
        }
    }

    close(server_fd);
    close(epoll_fd);
}

int main(int argc, char *argv[]) {
    if (argc > 1 && strcmp(argv[1], "server") == 0) {
        if (argc > 2) {
            server_ip = argv[2];
        }
        if (argc > 3) {
            server_port = atoi(argv[3]);
        }
        run_server();
    } else if (argc > 1 && strcmp(argv[1], "client") == 0) {
        if (argc > 2) {
            server_ip = argv[2];
        }
        if (argc > 3) {
            server_port = atoi(argv[3]);
        }
        if (argc > 4) {
            num_client_threads = atoi(argv[4]);
        }
        if (argc > 5) {
            num_requests = atoi(argv[5]);
        }
        run_client();
    } else {
        printf("Usage: %s <server|client> [server_ip server_port num_client_threads num_requests]\n", argv[0]);
    }

    return 0;
}