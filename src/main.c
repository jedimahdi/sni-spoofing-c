#include <errno.h>
#include <net/if.h>
#include <signal.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <net/ethernet.h>
#include <linux/ip.h>
#include <linux/tcp.h>
#include <linux/if_packet.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>

#include "utils.h"
#include "pm/loop.h"
#include "pm/writer.h"
#include "timer.h"

#define BUFFER_SIZE 16384
#define MAX_EVENTS 64
#define MAX_PACKET_SIZE 65536
#define MAX_PORT UINT16_MAX

#define CONNECT_TIMEOUT_MS 5000 // 5 seconds
#define IDLE_TIMEOUT_MS 60000 // 60 seconds

typedef enum {
  CONN_CONNECTING,
  CONN_WAIT_FOR_SYN,
  CONN_WAIT_FOR_ACK,
  CONN_RELAY,
} connection_state;

typedef struct connection_t connection_t;

struct connection_t {
  connection_state state;
  u16 local_port;
  u8 fake_payload[CLIENT_HELLO_SIZE];

  int upstream_fd;
  pm_writer upstream_writer;
  bool upstream_done;

  int client_fd;
  u16 client_isn;
  pm_writer client_writer;
  bool client_done;

  u64 last_event_ms;

  connection_t *next;
};

typedef struct {
  const char *connect_ip;
  const char *connect_port;
  const char *listen_host;
  const char *listen_port;
  const char *fake_sni;

  char local_ip[INET_ADDRSTRLEN];
  struct sockaddr_in connect_addr;

  char iface_name[IFNAMSIZ];
  int iface_index;
} config_t;

config_t cfg;

connection_t *connections[MAX_PORT];
connection_t *free_connections = NULL;

connection_t *new_connection(int client_fd, int upstream_fd) {
  connection_t *conn;

  if (free_connections) {
    conn = free_connections;
    free_connections = conn->next;

    pm_writer_reset(&conn->client_writer);
    pm_writer_reset(&conn->upstream_writer);

    conn->client_writer.fd = client_fd;
    conn->upstream_writer.fd = upstream_fd;
  } else {
    conn = malloc(sizeof(*conn));
    pm_writer_init(&conn->client_writer, client_fd, BUFFER_SIZE);
    pm_writer_init(&conn->upstream_writer, upstream_fd, BUFFER_SIZE);
  }

  conn->state = CONN_CONNECTING;
  conn->upstream_fd = upstream_fd;
  conn->client_fd = client_fd;
  conn->client_done = false;
  conn->upstream_done = false;
  conn->next = NULL;
  return conn;
}

static void close_connection(connection_t *conn, pm_loop *loop) {
  LOG_INFO("[Client] fd=%d closing", conn->client_fd);

  conn->next = free_connections;
  free_connections = conn;

  pm_loop_del(loop, conn->client_fd);
  pm_loop_del(loop, conn->upstream_fd);
  close(conn->client_fd);
  close(conn->upstream_fd);
}

void update_events(connection_t *conn, pm_loop *loop) {
  pm_event_mask mask = 0;
  if (!conn->upstream_done) mask |= PM_EVENT_READABLE;
  if (!pm_writer_is_empty(&conn->upstream_writer)) mask |= PM_EVENT_WRITABLE;
  pm_loop_mod(loop, conn->upstream_fd, mask);

  mask = 0;
  if (!conn->client_done) mask |= PM_EVENT_READABLE;
  if (!pm_writer_is_empty(&conn->client_writer)) mask |= PM_EVENT_WRITABLE;
  pm_loop_mod(loop, conn->client_fd, mask);
}

void handle_upstream(int _fd, pm_loop *loop, pm_event_mask events, void *ud) {
  connection_t *conn = ud;

  if (events & PM_EVENT_HANGUP) {
    LOG_WARN("[Upstream] hangup");
    close_connection(conn, loop);
    return;
  }

  if (events & PM_EVENT_ERROR) {
    LOG_WARN("[Upstream]  err");
    close_connection(conn, loop);
    return;
  }

  if (events & PM_EVENT_READABLE) {
    u8 buf[BUFFER_SIZE];
    ssize_t nread = recv(conn->upstream_fd, buf, sizeof(buf), 0);
    if (nread < 0 && errno != EAGAIN) {
      close_connection(conn, loop);
      return;
    } else if (nread == 0) {
      conn->upstream_done = true;
    } else {
      pm_write_result wr = pm_writer_write_bytes(&conn->client_writer, buf, (size_t)nread);
      if (wr.status == PM_WRITE_ERR) {
        close_connection(conn, loop);
        return;
      }
    }
  }

  if (events & PM_EVENT_WRITABLE) {
    pm_write_status st = pm_writer_flush(&conn->upstream_writer);
    if (st == PM_WRITE_ERR) {
      close_connection(conn, loop);
      return;
    }
  }

  if (conn->upstream_done) {
    close_connection(conn, loop);
    return;
  }

  update_events(conn, loop);
}

void handle_client(int _fd, pm_loop *loop, pm_event_mask events, void *ud) {
  connection_t *conn = ud;

  if (events & (PM_EVENT_HANGUP | PM_EVENT_ERROR)) {
    LOG_WARN("[Client] hangup | err");
    close_connection(conn, loop);
    return;
  }

  if (events & PM_EVENT_READABLE) {
    u8 buf[BUFFER_SIZE];
    ssize_t nread = recv(conn->client_fd, buf, sizeof(buf), 0);
    if (nread < 0 && errno != EAGAIN) {
      close_connection(conn, loop);
      return;
    } else if (nread == 0) {
      conn->client_done = true;
    } else {
      pm_write_result wr = pm_writer_write_bytes(&conn->upstream_writer, buf, (size_t)nread);
      if (wr.status == PM_WRITE_ERR) {
        close_connection(conn, loop);
        return;
      }
    }
  }

  if (events & PM_EVENT_WRITABLE) {
    pm_write_status st = pm_writer_flush(&conn->client_writer);
    if (st == PM_WRITE_ERR) {
      LOG_WARN("[Client] fd=%d, write error", conn->client_fd);
      close_connection(conn, loop);
      return;
    }
  }

  update_events(conn, loop);
}

void on_accept(int server_fd, pm_loop *loop, pm_event_mask events, void *ud) {
  (void)ud;
  (void)events;

  printf("before loop\n");
  while (true) {
  printf("before accept\n");
    int client_fd = accept4(server_fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
  printf("after accept\n");
    if (client_fd < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        break;
      }
      if (errno == EINTR) continue;
      if (errno == EMFILE || errno == ENFILE) {
        LOG_ERROR("accept4: too many open files");
        break;
      }

      LOG_ERROR("accept4: %s", strerror(errno));
      break;
    }

    int upstream_fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (upstream_fd < 0) {
      LOG_ERROR("socket: %s", strerror(errno));
      close(client_fd);
      return;
    }

    connection_t *conn = new_connection(client_fd, upstream_fd);

    int ret = connect(upstream_fd, (struct sockaddr *)&cfg.connect_addr, sizeof(cfg.connect_addr));
    if (ret < 0) {
      if (errno == EINPROGRESS) {
        conn->state = CONN_CONNECTING;
        continue;
      } else {
        LOG_WARN("connect(upstream %d): %s", upstream_fd, strerror(errno));
        close(client_fd);
        close(upstream_fd);
        break;
      }
    }

    conn->state = CONN_RELAY;

    // struct sockaddr_in local;
    // socklen_t len = sizeof(local);
    // getsockname(upstream_fd, (struct sockaddr *)&local, &len);
    // u16 local_port = ntohs(local.sin_port);
    // connections[local_port] = conn;
    // build_client_hello(cfg.fake_sni, conn->fake_payload);

    LOG_INFO("[ACCEPT] fd=%d upstream_fd=%d", client_fd, upstream_fd);

    pm_loop_add(loop, client_fd, 0, handle_client, conn);
    pm_loop_add(loop, upstream_fd, 0, handle_upstream, conn);
  }
}

void on_packet(int raw_sock, pm_loop *loop, pm_event_mask events, void *ud) {
  (void)ud;
  (void)events;
  u8 buf[MAX_PACKET_SIZE];
  ssize_t nread = recv(raw_sock, buf, sizeof(buf), 0);
  if (nread < 0) {
    LOG_ERROR("recv on_packet: %s", strerror(errno));
    return;
  }

  struct ethhdr *eth = (struct ethhdr *)buf;
  u16 ethtype = ntohs(eth->h_proto);

  if (ethtype != ETH_P_IP) return;

  struct iphdr *ip = (struct iphdr *)(buf + sizeof(struct ethhdr));
  uint32_t ip_header_len = ip->ihl * 4;

  struct tcphdr *tcp = (struct tcphdr *)((uint8_t *)ip + ip_header_len);

  uint16_t src_port = ntohs(tcp->source);
  uint16_t dest_port = ntohs(tcp->dest);
  uint32_t seq_num = ntohl(tcp->seq);

  connection_t *conn = connections[src_port];
  if (conn == NULL) return;

  char src_ip[INET_ADDRSTRLEN];
  inet_ntop(AF_INET, &ip->saddr, src_ip, sizeof(src_ip));

  char dest_ip[INET_ADDRSTRLEN];
  inet_ntop(AF_INET, &ip->daddr, dest_ip, sizeof(dest_ip));

  if (conn->state == CONN_WAIT_FOR_SYN && tcp->syn) {
    LOG_INFO("[PACKET] SYN isn=%u %s:%u -> %s:%u", seq_num, src_ip, src_port, dest_ip, dest_port);
    conn->state = CONN_WAIT_FOR_ACK;
    conn->client_isn = seq_num;
  } else if (conn->state == CONN_WAIT_FOR_ACK && tcp->ack) {
    LOG_INFO("[PACKET] ACK isn=%u %s:%u -> %s:%u", seq_num, src_ip, src_port, dest_ip, dest_port);
    connections[src_port] = NULL;

    uint8_t fake_frame[2048];
    size_t fake_frame_len = 0;
    build_fake_frame(buf, nread, conn->client_isn, conn->fake_payload, CLIENT_HELLO_SIZE, fake_frame, &fake_frame_len);

    struct sockaddr_ll sll = {0};
    sll.sll_ifindex = cfg.iface_index;
    sll.sll_halen = 6;
    memcpy(sll.sll_addr, eth->h_dest, 6);

    // usleep(100);
    if (sendto(raw_sock, fake_frame, fake_frame_len, 0, (struct sockaddr *)&sll, sizeof(sll)) <= 0) {
      LOG_WARN("[PACKET] fd=%d, Fake ClientHello injection failed: %s", conn->client_fd, strerror(errno));
      close_connection(conn, loop);
      return;
    }
    LOG_INFO("[PACKET] fd=%d, Fake ClientHello injected successfully (port %u)", conn->client_fd, src_port);

    pm_loop_mod(loop, conn->client_fd, PM_EVENT_WRITABLE);
    pm_loop_mod(loop, conn->upstream_fd, PM_EVENT_WRITABLE);
    conn->state = CONN_RELAY;
  }
}

void usage() {
  fprintf(stderr, "usage: sni-spoofing-c [options]\n\n");
  fprintf(stderr, "OPTIONS\n");
  fprintf(stderr, "  -h, --help    \n");
  fprintf(stderr, "  --fake-sni    \thcaptcha.com\n");
  fprintf(stderr, "  --connect-ip  \t104.19.229.21\n");
  fprintf(stderr, "  --connect-port\t443\n");
  fprintf(stderr, "  --listen-host \t127.0.0.1\n");
  fprintf(stderr, "  --listen-port \t40443\n");
}

void init_config(int argc, char *argv[]) {
  memset(&cfg, 0, sizeof(cfg));

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
      usage();
      exit(0);
    } else if (strcmp(argv[i], "--connect-ip") == 0) {
      cfg.connect_ip = argv[++i];
    } else if (strcmp(argv[i], "--connect-port") == 0) {
      cfg.connect_port = argv[++i];
    } else if (strcmp(argv[i], "--listen-host") == 0) {
      cfg.listen_host = argv[++i];
    } else if (strcmp(argv[i], "--listen-port") == 0) {
      cfg.listen_port = argv[++i];
    } else if (strcmp(argv[i], "--fake-sni") == 0) {
      cfg.fake_sni = argv[++i];
    }
  }

  if (cfg.connect_ip == NULL) cfg.connect_ip = "104.18.32.47";
  if (cfg.connect_port == NULL) cfg.connect_port = "443";
  if (cfg.fake_sni == NULL) cfg.fake_sni = "hcaptcha.com";
  if (cfg.listen_host == NULL) cfg.listen_host = "127.0.0.1";
  if (cfg.listen_port == NULL) cfg.listen_port = "40443";

  get_local_iface(cfg.connect_ip, cfg.local_ip, sizeof(cfg.local_ip), cfg.iface_name, sizeof(cfg.iface_name), &cfg.iface_index);

  struct addrinfo hints = {0};
  struct addrinfo *res;
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;

  int ret = getaddrinfo(cfg.connect_ip, cfg.connect_port, &hints, &res);
  if (ret != 0) {
    LOG_ERROR("getaddrinfo(%s:%s): %s", cfg.connect_ip, cfg.connect_port, gai_strerror(ret));
    exit(1);
  }
  memcpy(&cfg.connect_addr, res->ai_addr, res->ai_addrlen);

  freeaddrinfo(res);
}

void loop_run_timed(pm_loop *l, timer_heap *timers) {
  l->running = true;

  while (l->running) {
    uint64_t now = get_time_ms();
    int timeout_ms = -1;

    timer_node *next_timer = timer_heap_peak(timers);
    if (next_timer != NULL) {
      if (next_timer->expire_at_ms <= now) {
        timeout_ms = 0;
      } else {
        timeout_ms = (int)(next_timer->expire_at_ms - now);
      }
    }

    pm_loop_once(l, timeout_ms);

    now = get_time_ms();
    while (true) {
      timer_node *t = timer_heap_peak(timers);
      if (t == NULL || t->expire_at_ms > now) break;

      timer_node expired = timer_heap_pop(timers);
      connection_t *conn = (connection_t *)expired.user_data;

      uint64_t time_since_last_event = now - conn->last_event_ms;

      if (time_since_last_event >= IDLE_TIMEOUT_MS) {
        LOG_INFO("Connection %d timed out", conn->client_fd);
        close_connection(conn, l);
      } else {
        uint64_t new_expire = conn->last_event_ms + IDLE_TIMEOUT_MS;
        timer_heap_push(timers, new_expire, conn);
      }
    }
  }
}

int main(int argc, char *argv[]) {
  signal(SIGPIPE, SIG_IGN);

  init_config(argc, argv);

  LOG_INFO("CONFIG: fake-sni=%s, connect-ip=%s, connect-port=%s, listen-host=%s, listen-port=%s",
           cfg.fake_sni, cfg.connect_ip, cfg.connect_port, cfg.listen_host, cfg.listen_port);

  // int raw_sock = rawsocket(cfg.iface_index);
  // set_nonblocking(raw_sock);
  // LOG_INFO("Listening on interface %s(%d)...", cfg.iface_name, cfg.iface_index);

  int server_sock = listen_tcp(cfg.listen_host, cfg.listen_port);
  set_nonblocking(server_sock);
  LOG_INFO("Proxy listening on %s:%s", cfg.listen_host, cfg.listen_port);

  pm_loop loop;
  pm_loop_init(&loop);
  // pm_loop_add(&loop, raw_sock, PM_EVENT_READABLE, on_packet, NULL);
  pm_loop_add(&loop, server_sock, PM_EVENT_READABLE, on_accept, NULL);

  timer_heap timers;
  timer_heap_init(&timers, 128);

  loop_run_timed(&loop, &timers);

  return 0;
}
