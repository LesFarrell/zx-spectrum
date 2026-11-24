#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <icmpapi.h>

#include "next_wifi.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum {
    NEXT_WIFI_RX_CAPACITY = 4096,
    NEXT_WIFI_TX_CAPACITY = 64,
    NEXT_WIFI_LINE_CAPACITY = 512,
    NEXT_WIFI_MAX_CONNECTIONS = 5,
    NEXT_WIFI_MAX_SEND = 65535,
    NEXT_WIFI_SOCKET_CHUNK = 192,
    NEXT_WIFI_PASSIVE_CAPACITY = 2048,
    NEXT_WIFI_IPD_HEADROOM = 64
};

typedef struct NextWifiConnection {
    SOCKET socket;
    bool active;
    bool udp;
    bool server;
    char type[4];
    char host[256];
    char peer_host[64];
    uint16_t remote_port;
    uint16_t local_port;
    uint64_t last_activity;
    uint8_t passive[NEXT_WIFI_PASSIVE_CAPACITY];
    size_t passive_length;
} NextWifiConnection;

struct NextWifi {
    CRITICAL_SECTION lock;
    HANDLE wake_event;
    HANDLE thread;
    bool stop_requested;
    bool enabled;
    bool reset_requested;
    bool winsock_ready;

    uint8_t rx[NEXT_WIFI_RX_CAPACITY];
    size_t rx_read;
    size_t rx_write;
    size_t rx_count;
    uint8_t tx[NEXT_WIFI_TX_CAPACITY];
    size_t tx_read;
    size_t tx_write;
    size_t tx_count;
    bool rx_overflow;

    uint8_t uart_control;
    uint8_t uart_frame;
    uint32_t uart_prescaler;

    bool worker_enabled;
    bool echo;
    bool multiplex;
    bool cipmode;
    bool cipdinfo;
    bool recv_mode;
    bool transparent;
    unsigned transparent_pluses;
    uint8_t wifi_mode;
    bool station_connected;
    char station_ssid[33];
    int station_channel;
    int station_rssi;
    char hostname[64];
    char ap_ssid[33];
    char ap_password[65];
    unsigned ap_channel;
    unsigned ap_encryption;
    unsigned ap_max_connections;
    unsigned ap_hidden;
    char station_ip[16];
    char station_gateway[16];
    char station_netmask[16];
    char ap_ip[16];
    char ap_gateway[16];
    char ap_netmask[16];
    char station_mac[18];
    char ap_mac[18];
    char dns_server[3][16];
    unsigned dns_count;
    unsigned sntp_enabled;
    int sntp_timezone;
    char sntp_server[3][64];
    unsigned dhcp_station;
    unsigned dhcp_ap;
    unsigned close_mode;
    unsigned reconnection_interval;
    bool mdns_enabled;
    char mdns_host[64];
    char mdns_server[64];
    unsigned mdns_port;
    bool wps_enabled;
    unsigned sysmsg;
    unsigned sysstore;
    unsigned autoconn;
    unsigned sleep_mode;
    unsigned rfpower;
    unsigned ipv6;
    unsigned check_seq;

    SOCKET listener;
    bool server_enabled;
    unsigned server_port;
    unsigned server_max_connections;
    uint32_t server_timeout;

    char line[NEXT_WIFI_LINE_CAPACITY];
    size_t line_length;
    bool line_pending;
    uint8_t *send_payload;
    size_t send_length;
    size_t send_received;
    int send_connection;
    NextWifiConnection connections[NEXT_WIFI_MAX_CONNECTIONS];
};

static void next_wifi_close_connection(NextWifiConnection *connection);
static void next_wifi_close_listener(NextWifi *wifi);
static void next_wifi_worker_reset(NextWifi *wifi);
static void next_wifi_queue_text(NextWifi *wifi, const char *text);
static void next_wifi_connection_notice(NextWifi *wifi, int id, const char *notice);
static void next_wifi_inject_ipd(NextWifi *wifi, int id,
                                 const NextWifiConnection *connection,
                                 const void *data, size_t size);

static void next_wifi_close_connection(NextWifiConnection *connection)
{
    if (connection->socket != INVALID_SOCKET)
    {
        closesocket(connection->socket);
    }
    memset(connection, 0, sizeof(*connection));
    connection->socket = INVALID_SOCKET;
}

static void next_wifi_close_all(NextWifi *wifi)
{
    for (unsigned i = 0; i < NEXT_WIFI_MAX_CONNECTIONS; ++i)
    {
        next_wifi_close_connection(&wifi->connections[i]);
    }
}

static void next_wifi_close_listener(NextWifi *wifi)
{
    if (wifi->listener != INVALID_SOCKET)
    {
        closesocket(wifi->listener);
    }
    wifi->listener = INVALID_SOCKET;
    wifi->server_enabled = false;
    wifi->server_port = 0;
}

static void next_wifi_worker_reset(NextWifi *wifi)
{
    next_wifi_close_all(wifi);
    next_wifi_close_listener(wifi);
    free(wifi->send_payload);
    wifi->send_payload = NULL;
    wifi->send_length = 0;
    wifi->send_received = 0;
    wifi->send_connection = 0;
    wifi->line_length = 0;
    wifi->line_pending = false;
    wifi->echo = true;
    wifi->multiplex = false;
    wifi->cipmode = false;
    wifi->cipdinfo = false;
    wifi->recv_mode = false;
    wifi->transparent = false;
    wifi->transparent_pluses = 0;
    wifi->wifi_mode = 1;
    wifi->station_connected = true;
    snprintf(wifi->station_ssid, sizeof(wifi->station_ssid), "%s",
             "HOST NETWORK");
    wifi->station_channel = 1;
    wifi->station_rssi = -30;
    snprintf(wifi->hostname, sizeof(wifi->hostname), "%s", "espressif");
    snprintf(wifi->ap_ssid, sizeof(wifi->ap_ssid), "%s", "HOST NETWORK");
    snprintf(wifi->ap_password, sizeof(wifi->ap_password), "%s", "");
    wifi->ap_channel = 1;
    wifi->ap_encryption = 0;
    wifi->ap_max_connections = 4;
    wifi->ap_hidden = 0;
    snprintf(wifi->station_ip, sizeof(wifi->station_ip), "%s", "127.0.0.1");
    snprintf(wifi->station_gateway, sizeof(wifi->station_gateway), "%s",
             "192.168.1.1");
    snprintf(wifi->station_netmask, sizeof(wifi->station_netmask), "%s",
             "255.255.255.0");
    snprintf(wifi->ap_ip, sizeof(wifi->ap_ip), "%s", "192.168.4.1");
    snprintf(wifi->ap_gateway, sizeof(wifi->ap_gateway), "%s", "192.168.4.1");
    snprintf(wifi->ap_netmask, sizeof(wifi->ap_netmask), "%s",
             "255.255.255.0");
    snprintf(wifi->station_mac, sizeof(wifi->station_mac), "%s",
             "00:00:00:00:00:00");
    snprintf(wifi->ap_mac, sizeof(wifi->ap_mac), "%s", "02:00:00:00:00:00");
    snprintf(wifi->dns_server[0], sizeof(wifi->dns_server[0]), "%s",
             "8.8.8.8");
    wifi->dns_count = 1;
    wifi->sntp_enabled = 0;
    wifi->sntp_timezone = 0;
    snprintf(wifi->sntp_server[0], sizeof(wifi->sntp_server[0]), "%s",
             "pool.ntp.org");
    wifi->dhcp_station = 1;
    wifi->dhcp_ap = 0;
    wifi->close_mode = 0;
    wifi->reconnection_interval = 0;
    wifi->mdns_enabled = false;
    snprintf(wifi->mdns_host, sizeof(wifi->mdns_host), "%s", "espressif");
    snprintf(wifi->mdns_server, sizeof(wifi->mdns_server), "%s", "local");
    wifi->mdns_port = 80;
    wifi->wps_enabled = false;
    wifi->sysmsg = 0;
    wifi->sysstore = 0;
    wifi->autoconn = 0;
    wifi->sleep_mode = 0;
    wifi->rfpower = 40;
    wifi->ipv6 = 0;
    wifi->check_seq = 0;
    wifi->server_max_connections = NEXT_WIFI_MAX_CONNECTIONS;
    wifi->server_timeout = 180;
}

static size_t next_wifi_rx_free(NextWifi *wifi)
{
    size_t free_space;
    EnterCriticalSection(&wifi->lock);
    free_space = NEXT_WIFI_RX_CAPACITY - wifi->rx_count;
    LeaveCriticalSection(&wifi->lock);
    return free_space;
}

static void next_wifi_queue_bytes(NextWifi *wifi, const void *data, size_t size)
{
    const uint8_t *bytes = (const uint8_t *)data;
    size_t queued = 0;
    EnterCriticalSection(&wifi->lock);
    while (queued < size && wifi->rx_count < NEXT_WIFI_RX_CAPACITY)
    {
        wifi->rx[wifi->rx_write] = bytes[queued++];
        wifi->rx_write = (wifi->rx_write + 1) % NEXT_WIFI_RX_CAPACITY;
        ++wifi->rx_count;
    }
    if (queued < size)
        wifi->rx_overflow = true;
    LeaveCriticalSection(&wifi->lock);
}

static void next_wifi_queue_text(NextWifi *wifi, const char *text)
{
    next_wifi_queue_bytes(wifi, text, strlen(text));
}

static bool next_wifi_starts_with(const char *text, const char *prefix)
{
    return strncmp(text, prefix, strlen(prefix)) == 0;
}

static const char *next_wifi_trim_quotes(const char *text)
{
    while (*text == ' ' || *text == '\t')
        ++text;
    if (*text == '"')
        ++text;
    return text;
}

static void next_wifi_copy_unquoted(char *destination, size_t size,
                                   const char *text)
{
    size_t length = 0;
    while (text[length] != '\0' && text[length] != '"' &&
           text[length] != ',' && length + 1 < size)
    {
        destination[length] = text[length];
        ++length;
    }
    while (length > 0 &&
           (destination[length - 1] == ' ' || destination[length - 1] == '\t'))
    {
        --length;
    }
    destination[length] = '\0';
}

static bool next_wifi_parse_uint(const char *text, unsigned *value)
{
    char *end = NULL;
    unsigned long parsed;
    if (text == NULL)
        return false;
    while (*text == ' ' || *text == '\t')
        ++text;
    if (*text < '0' || *text > '9')
        return false;
    parsed = strtoul(text, &end, 10);
    if (end == text)
        return false;
    while (*end == ' ' || *end == '\t')
        ++end;
    if (*end != '\0')
        return false;
    *value = (unsigned)parsed;
    return true;
}

static void next_wifi_connection_notice(NextWifi *wifi, int id,
                                        const char *notice)
{
    char response[48];
    if (wifi->multiplex)
    {
        snprintf(response, sizeof(response), "%d,%s\r\n", id, notice);
    }
    else
    {
        snprintf(response, sizeof(response), "%s\r\n", notice);
    }
    next_wifi_queue_text(wifi, response);
}

static bool next_wifi_wait_for_socket(SOCKET socket, bool write_ready)
{
    fd_set read_set;
    fd_set write_set;
    fd_set error_set;
    struct timeval timeout;

    FD_ZERO(&read_set);
    FD_ZERO(&write_set);
    FD_ZERO(&error_set);
    if (write_ready)
        FD_SET(socket, &write_set);
    else
        FD_SET(socket, &read_set);
    FD_SET(socket, &error_set);
    timeout.tv_sec = 5;
    timeout.tv_usec = 0;
    return select(0, write_ready ? NULL : &read_set,
                  write_ready ? &write_set : NULL, &error_set, &timeout) > 0 &&
           !FD_ISSET(socket, &error_set);
}

static void next_wifi_update_peer(NextWifiConnection *connection)
{
    struct sockaddr_storage address;
    int address_size = sizeof(address);
    char host[64];
    char service[16];

    connection->peer_host[0] = '\0';
    if (getpeername(connection->socket, (struct sockaddr *)&address,
                    &address_size) == 0)
    {
        if (getnameinfo((struct sockaddr *)&address, (socklen_t)address_size,
                        host, (DWORD)sizeof(host), service,
                        (DWORD)sizeof(service),
                        NI_NUMERICHOST | NI_NUMERICSERV) == 0)
        {
            snprintf(connection->peer_host, sizeof(connection->peer_host),
                     "%s", host);
            connection->remote_port = (uint16_t)strtoul(service, NULL, 10);
        }
    }

    address_size = sizeof(address);
    if (getsockname(connection->socket, (struct sockaddr *)&address,
                    &address_size) == 0)
    {
        if (address.ss_family == AF_INET)
        {
            connection->local_port =
                ntohs(((struct sockaddr_in *)&address)->sin_port);
        }
        else if (address.ss_family == AF_INET6)
        {
            connection->local_port =
                ntohs(((struct sockaddr_in6 *)&address)->sin6_port);
        }
    }
}

static bool next_wifi_open_connection(
    NextWifi *wifi,
    int id,
    const char *type,
    const char *host,
    unsigned port)
{
    struct addrinfo hints;
    struct addrinfo *addresses = NULL;
    struct addrinfo *address;
    char service[16];
    SOCKET connected_socket = INVALID_SOCKET;
    const bool udp = strcmp(type, "UDP") == 0;

    if (!wifi->winsock_ready || id < 0 || id >= NEXT_WIFI_MAX_CONNECTIONS ||
        host[0] == '\0' || port == 0 || port > 65535 ||
        (!udp && strcmp(type, "TCP") != 0))
    {
        return false;
    }

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = udp ? SOCK_DGRAM : SOCK_STREAM;
    hints.ai_protocol = udp ? IPPROTO_UDP : IPPROTO_TCP;
    snprintf(service, sizeof(service), "%u", port);
    if (getaddrinfo(host, service, &hints, &addresses) != 0)
        return false;

    for (address = addresses; address != NULL; address = address->ai_next)
    {
        u_long nonblocking = 1;
        int result;
        int socket_error = 0;
        int socket_error_size = sizeof(socket_error);

        connected_socket = socket(address->ai_family, address->ai_socktype,
                                  address->ai_protocol);
        if (connected_socket == INVALID_SOCKET)
            continue;
        ioctlsocket(connected_socket, FIONBIO, &nonblocking);
        result = connect(connected_socket, address->ai_addr,
                         (int)address->ai_addrlen);
        if (result == 0 ||
            (WSAGetLastError() == WSAEWOULDBLOCK &&
             next_wifi_wait_for_socket(connected_socket, true) &&
             getsockopt(connected_socket, SOL_SOCKET, SO_ERROR,
                        (char *)&socket_error,
                        &socket_error_size) == 0 && socket_error == 0))
        {
            break;
        }
        closesocket(connected_socket);
        connected_socket = INVALID_SOCKET;
    }
    freeaddrinfo(addresses);
    if (connected_socket == INVALID_SOCKET)
        return false;

    next_wifi_close_connection(&wifi->connections[id]);
    wifi->connections[id].socket = connected_socket;
    wifi->connections[id].active = true;
    wifi->connections[id].udp = udp;
    snprintf(wifi->connections[id].type,
             sizeof(wifi->connections[id].type), "%s", type);
    snprintf(wifi->connections[id].host,
             sizeof(wifi->connections[id].host), "%s", host);
    wifi->connections[id].remote_port = (uint16_t)port;
    wifi->connections[id].last_activity = GetTickCount64();
    next_wifi_update_peer(&wifi->connections[id]);
    return true;
}

static bool next_wifi_open_listener(NextWifi *wifi, unsigned port)
{
    struct sockaddr_in address;
    u_long nonblocking = 1;
    int reuse = 1;
    SOCKET listener;

    if (!wifi->winsock_ready || port == 0 || port > 65535)
        return false;

    listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET)
        return false;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse,
               sizeof(reuse));
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons((uint16_t)port);
    if (bind(listener, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(listener, SOMAXCONN) != 0)
    {
        closesocket(listener);
        return false;
    }
    ioctlsocket(listener, FIONBIO, &nonblocking);

    next_wifi_close_listener(wifi);
    wifi->listener = listener;
    wifi->server_enabled = true;
    wifi->server_port = port;
    return true;
}

static bool next_wifi_send_socket(NextWifiConnection *connection,
                                  const uint8_t *data, size_t size)
{
    size_t sent = 0;
    while (sent < size)
    {
        const int amount = send(connection->socket, (const char *)data + sent,
                                (int)(size - sent), 0);
        if (amount > 0)
        {
            sent += (size_t)amount;
            continue;
        }
        if (amount == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK &&
            next_wifi_wait_for_socket(connection->socket, true))
        {
            continue;
        }
        return false;
    }
    return true;
}

static bool next_wifi_send_bytes(NextWifi *wifi, int id, const void *data,
                                 size_t size)
{
    NextWifiConnection *connection = &wifi->connections[id];
    if (!connection->active)
        return false;
    if (next_wifi_send_socket(connection, (const uint8_t *)data, size))
    {
        connection->last_activity = GetTickCount64();
        return true;
    }
    next_wifi_close_connection(connection);
    next_wifi_connection_notice(wifi, id, "CLOSED");
    return false;
}

static void next_wifi_format_ipd(NextWifi *wifi, int id,
                                 const NextWifiConnection *connection,
                                 size_t length, char *buffer, size_t size)
{
    if (wifi->multiplex)
    {
        if (wifi->cipdinfo && connection != NULL)
        {
            snprintf(buffer, size, "+IPD,%d,%u,\"%s\",%u:", id,
                     (unsigned)length, connection->peer_host,
                     connection->remote_port);
        }
        else
        {
            snprintf(buffer, size, "+IPD,%d,%u:", id, (unsigned)length);
        }
    }
    else
    {
        if (wifi->cipdinfo && connection != NULL)
        {
            snprintf(buffer, size, "+IPD,%u,\"%s\",%u:", (unsigned)length,
                     connection->peer_host, connection->remote_port);
        }
        else
        {
            snprintf(buffer, size, "+IPD,%u:", (unsigned)length);
        }
    }
}

static void next_wifi_inject_ipd(NextWifi *wifi, int id,
                                 const NextWifiConnection *connection,
                                 const void *data, size_t size)
{
    char header[NEXT_WIFI_IPD_HEADROOM];
    next_wifi_format_ipd(wifi, id, connection, size, header, sizeof(header));
    next_wifi_queue_text(wifi, header);
    next_wifi_queue_bytes(wifi, data, size);
}

static size_t next_wifi_deliver_passive(NextWifi *wifi, int id)
{
    NextWifiConnection *connection = &wifi->connections[id];
    char header[NEXT_WIFI_IPD_HEADROOM];
    size_t free_space;
    size_t deliver;
    size_t header_size;

    if (!connection->active || connection->passive_length == 0)
        return 0;
    free_space = next_wifi_rx_free(wifi);
    if (free_space <= NEXT_WIFI_IPD_HEADROOM)
        return 0;
    deliver = connection->passive_length;
    if (deliver > free_space - NEXT_WIFI_IPD_HEADROOM)
        deliver = free_space - NEXT_WIFI_IPD_HEADROOM;
    next_wifi_format_ipd(wifi, id, connection, deliver, header,
                         sizeof(header));
    header_size = strlen(header);
    if (header_size + deliver > free_space)
    {
        deliver = free_space > header_size ? free_space - header_size : 0;
        if (deliver == 0)
            return 0;
        next_wifi_format_ipd(wifi, id, connection, deliver, header,
                             sizeof(header));
    }
    next_wifi_queue_text(wifi, header);
    next_wifi_queue_bytes(wifi, connection->passive, deliver);
    memmove(connection->passive, connection->passive + deliver,
            connection->passive_length - deliver);
    connection->passive_length -= deliver;
    return deliver;
}

static void next_wifi_finish_send(NextWifi *wifi)
{
    NextWifiConnection *connection = NULL;
    bool sent = false;
    if (wifi->send_connection >= 0 &&
        wifi->send_connection < NEXT_WIFI_MAX_CONNECTIONS)
    {
        connection = &wifi->connections[wifi->send_connection];
    }
    if (connection != NULL && connection->active)
    {
        sent = next_wifi_send_socket(connection, wifi->send_payload,
                                     wifi->send_length);
    }
    free(wifi->send_payload);
    wifi->send_payload = NULL;
    wifi->send_length = 0;
    wifi->send_received = 0;
    next_wifi_queue_text(wifi, sent ? "\r\nSEND OK\r\n" : "\r\nSEND FAIL\r\n");
    if (!sent && connection != NULL)
    {
        next_wifi_close_connection(connection);
        next_wifi_connection_notice(wifi, wifi->send_connection, "CLOSED");
    }
}

static bool next_wifi_parse_cipstart(
    const char *arguments,
    bool multiplex,
    int *id,
    char *type,
    size_t type_size,
    char *host,
    size_t host_size,
    unsigned *port)
{
    int parsed_id = 0;
    char parsed_type[8];
    char parsed_host[256];
    unsigned parsed_port;
    int fields;

    if (multiplex)
    {
        fields = sscanf(arguments, "%d,\"%7[^\"]\",\"%255[^\"]\",%u",
                        &parsed_id, parsed_type, parsed_host, &parsed_port);
        if (fields != 4)
            return false;
    }
    else
    {
        fields = sscanf(arguments, "\"%7[^\"]\",\"%255[^\"]\",%u",
                        parsed_type, parsed_host, &parsed_port);
        if (fields != 3)
            return false;
    }
    for (char *p = parsed_type; *p != '\0'; ++p)
        *p = (char)toupper((unsigned char)*p);
    *id = parsed_id;
    *port = parsed_port;
    snprintf(type, type_size, "%s", parsed_type);
    snprintf(host, host_size, "%s", parsed_host);
    return true;
}

static void next_wifi_begin_send(NextWifi *wifi, const char *arguments)
{
    int id = 0;
    unsigned length = 0;
    int fields;
    if (wifi->multiplex)
    {
        fields = sscanf(arguments, "%d,%u", &id, &length);
        if (fields != 2)
            length = 0;
    }
    else if (sscanf(arguments, "%u", &length) != 1)
    {
        length = 0;
    }
    if (id < 0 || id >= NEXT_WIFI_MAX_CONNECTIONS || length == 0 ||
        length > NEXT_WIFI_MAX_SEND || !wifi->connections[id].active)
    {
        next_wifi_queue_text(wifi, "ERROR\r\n");
        return;
    }
    wifi->send_payload = (uint8_t *)malloc(length);
    if (wifi->send_payload == NULL)
    {
        next_wifi_queue_text(wifi, "ERROR\r\n");
        return;
    }
    wifi->send_length = length;
    wifi->send_received = 0;
    wifi->send_connection = id;
    next_wifi_queue_text(wifi, "OK\r\n>");
}

static void next_wifi_begin_transparent(NextWifi *wifi)
{
    if (wifi->multiplex || !wifi->connections[0].active)
    {
        next_wifi_queue_text(wifi, "ERROR\r\n");
        return;
    }
    wifi->transparent = true;
    wifi->transparent_pluses = 0;
    next_wifi_queue_text(wifi, "OK\r\n>");
}

static void next_wifi_flush_pluses(NextWifi *wifi)
{
    uint8_t pluses[3];
    size_t count = wifi->transparent_pluses;
    if (count == 0)
        return;
    memset(pluses, '+', count);
    wifi->transparent_pluses = 0;
    next_wifi_send_bytes(wifi, 0, pluses, count);
}

static void next_wifi_process_transparent(NextWifi *wifi, uint8_t value)
{
    if (!wifi->connections[0].active)
    {
        wifi->transparent = false;
        wifi->transparent_pluses = 0;
        next_wifi_queue_text(wifi, "\r\nCLOSED\r\n");
        return;
    }
    if (value == '+')
    {
        if (wifi->transparent_pluses < 3)
            ++wifi->transparent_pluses;
        if (wifi->transparent_pluses == 3)
        {
            wifi->transparent_pluses = 0;
            wifi->transparent = false;
            next_wifi_queue_text(wifi, "OK\r\n");
        }
        return;
    }
    if (wifi->transparent_pluses != 0)
        next_wifi_flush_pluses(wifi);
    if (!wifi->transparent)
        return;
    if (!next_wifi_send_bytes(wifi, 0, &value, 1))
    {
        wifi->transparent = false;
        wifi->transparent_pluses = 0;
    }
}

static void next_wifi_report_status(NextWifi *wifi)
{
    char response[384];
    int status;
    if (!wifi->station_connected)
    {
        status = 5;
    }
    else
    {
        bool any_active = false;
        for (unsigned i = 0; i < NEXT_WIFI_MAX_CONNECTIONS; ++i)
            any_active = any_active || wifi->connections[i].active;
        status = any_active ? 3 : 2;
    }
    snprintf(response, sizeof(response), "STATUS:%d\r\n", status);
    next_wifi_queue_text(wifi, response);
    for (unsigned i = 0; i < NEXT_WIFI_MAX_CONNECTIONS; ++i)
    {
        const NextWifiConnection *connection = &wifi->connections[i];
        const char *host;
        if (!connection->active)
            continue;
        host = connection->peer_host[0] != '\0' ? connection->peer_host
                                               : connection->host;
        snprintf(response, sizeof(response),
                 "+CIPSTATUS:%u,\"%s\",\"%s\",%u,%u,%d\r\n",
                 i, connection->type, host, connection->remote_port,
                 connection->local_port, connection->server ? 1 : 0);
        next_wifi_queue_text(wifi, response);
    }
    next_wifi_queue_text(wifi, "OK\r\n");
}

static bool next_wifi_resolve_host(const char *host, char *address,
                                   size_t address_size)
{
    struct addrinfo hints;
    struct addrinfo *addresses = NULL;
    bool resolved = false;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, NULL, &hints, &addresses) == 0 &&
        addresses != NULL)
    {
        resolved = getnameinfo(addresses->ai_addr,
                               (socklen_t)addresses->ai_addrlen, address,
                               (DWORD)address_size, NULL, 0,
                               NI_NUMERICHOST) == 0;
    }
    if (addresses != NULL)
        freeaddrinfo(addresses);
    return resolved;
}

static const char *next_wifi_next_token(const char *text, char *destination,
                                        size_t size)
{
    size_t length = 0;
    while (*text == ' ' || *text == '\t' || *text == ',')
        ++text;
    if (*text == '"')
    {
        ++text;
        while (*text != '\0' && *text != '"' && length + 1 < size)
            destination[length++] = *text++;
        if (*text == '"')
            ++text;
        while (*text != '\0' && *text != ',')
            ++text;
        if (*text == ',')
            ++text;
    }
    else
    {
        while (*text != '\0' && *text != ',' && length + 1 < size)
            destination[length++] = *text++;
        while (length > 0 &&
               (destination[length - 1] == ' ' ||
                destination[length - 1] == '\t'))
        {
            --length;
        }
        if (*text == ',')
            ++text;
    }
    destination[length] = '\0';
    return text;
}

static bool next_wifi_parse_timezone(const char *text, int *minutes)
{
    bool negative = false;
    unsigned value = 0;
    unsigned hours;
    unsigned extra;
    while (*text == ' ' || *text == '\t')
        ++text;
    if (*text == '+')
        ++text;
    else if (*text == '-')
    {
        negative = true;
        ++text;
    }
    if (*text < '0' || *text > '9')
        return false;
    while (*text >= '0' && *text <= '9')
        value = value * 10 + (unsigned)(*text++ - '0');
    if (value > 1400)
        return false;
    if (value >= 100)
    {
        hours = value / 100;
        extra = value % 100;
    }
    else
    {
        hours = value;
        extra = 0;
    }
    if (hours > 14 || extra > 59)
        return false;
    *minutes = (int)(hours * 60 + extra);
    if (negative)
        *minutes = -*minutes;
    return true;
}

static int next_wifi_ping_host(const char *host, unsigned timeout_ms)
{
    struct addrinfo hints;
    struct addrinfo *addresses = NULL;
    struct sockaddr_in target;
    char request[32];
    uint8_t reply[sizeof(ICMP_ECHO_REPLY) + sizeof(request) + 8];
    HANDLE icmp;
    DWORD result;
    int rtt = -1;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, NULL, &hints, &addresses) != 0 ||
        addresses == NULL)
    {
        return -1;
    }
    memcpy(&target, addresses->ai_addr, sizeof(target));
    freeaddrinfo(addresses);

    icmp = IcmpCreateFile();
    if (icmp == INVALID_HANDLE_VALUE)
        return -1;
    memset(request, 'a', sizeof(request));
    result = IcmpSendEcho(icmp, target.sin_addr.s_addr, request,
                          sizeof(request), NULL, reply, sizeof(reply),
                          timeout_ms);
    if (result != 0)
    {
        const ICMP_ECHO_REPLY *echo = (const ICMP_ECHO_REPLY *)reply;
        if (echo->Status == IP_SUCCESS)
            rtt = (int)echo->RoundTripTime;
    }
    IcmpCloseHandle(icmp);
    return rtt;
}

static void next_wifi_format_timezone(int minutes, char *buffer, size_t size)
{
    const int absolute = minutes < 0 ? -minutes : minutes;
    const char *sign = minutes < 0 ? "-" : "";
    if (absolute % 60 == 0)
        snprintf(buffer, size, "%s%d", sign, absolute / 60);
    else
        snprintf(buffer, size, "%s%02d%02d", sign, absolute / 60,
                 absolute % 60);
}

static void next_wifi_format_sntp_time(const NextWifi *wifi, char *buffer,
                                       size_t size)
{
    time_t now = time(NULL) + (time_t)wifi->sntp_timezone * 60;
    struct tm value;
    if (gmtime_s(&value, &now) != 0 ||
        strftime(buffer, size, "%a %b %d %H:%M:%S %Y", &value) == 0)
    {
        snprintf(buffer, size, "%s", "");
    }
}

static bool next_wifi_handle_system_command(NextWifi *wifi,
                                            const char *command)
{
    if (strcmp(command, "AT") == 0)
    {
        next_wifi_queue_text(wifi, "OK\r\n");
        return true;
    }
    if (strcmp(command, "ATE0") == 0 || strcmp(command, "ATE1") == 0)
    {
        wifi->echo = command[3] == '1';
        next_wifi_queue_text(wifi, "OK\r\n");
        return true;
    }
    if (strcmp(command, "AT+RST") == 0 ||
        strcmp(command, "AT+RESTORE") == 0)
    {
        next_wifi_worker_reset(wifi);
        next_wifi_queue_text(wifi, "OK\r\n\r\nready\r\n");
        return true;
    }
    if (strcmp(command, "AT+GMR") == 0)
    {
        next_wifi_queue_text(
            wifi,
            "AT version:1.7.4.0-emulated\r\n"
            "SDK version:host-winsock\r\nOK\r\n");
        return true;
    }
    if (strcmp(command, "AT+UART?") == 0 ||
        strcmp(command, "AT+UART_CUR?") == 0 ||
        strcmp(command, "AT+UART_DEF?") == 0)
    {
        next_wifi_queue_text(wifi, "+UART:115200,8,1,0,0\r\nOK\r\n");
        return true;
    }
    if (next_wifi_starts_with(command, "AT+UART=") ||
        next_wifi_starts_with(command, "AT+UART_CUR=") ||
        next_wifi_starts_with(command, "AT+UART_DEF="))
    {
        next_wifi_queue_text(wifi, "OK\r\n");
        return true;
    }
    if (strcmp(command, "AT+SYSRAM?") == 0)
    {
        next_wifi_queue_text(wifi, "+SYSRAM:81920,81920\r\nOK\r\n");
        return true;
    }
    if (strcmp(command, "AT+SLEEP?") == 0)
    {
        char response[32];
        snprintf(response, sizeof(response), "+SLEEP:%u\r\nOK\r\n",
                 wifi->sleep_mode);
        next_wifi_queue_text(wifi, response);
        return true;
    }
    if (strcmp(command, "AT+RFPOWER?") == 0)
    {
        char response[32];
        snprintf(response, sizeof(response), "+RFPOWER:%u\r\nOK\r\n",
                 wifi->rfpower);
        next_wifi_queue_text(wifi, response);
        return true;
    }
    if (strcmp(command, "AT+CWAUTOCONN?") == 0)
    {
        char response[32];
        snprintf(response, sizeof(response), "+CWAUTOCONN:%u\r\nOK\r\n",
                 wifi->autoconn);
        next_wifi_queue_text(wifi, response);
        return true;
    }
    if (strcmp(command, "AT+CIPV6?") == 0)
    {
        char response[32];
        snprintf(response, sizeof(response), "+CIPV6:%u\r\nOK\r\n",
                 wifi->ipv6);
        next_wifi_queue_text(wifi, response);
        return true;
    }
    if (strcmp(command, "AT+SYSMSG?") == 0)
    {
        char response[32];
        snprintf(response, sizeof(response), "+SYSMSG:%u\r\nOK\r\n",
                 wifi->sysmsg);
        next_wifi_queue_text(wifi, response);
        return true;
    }
    if (strcmp(command, "AT+SYSSTORE?") == 0)
    {
        char response[32];
        snprintf(response, sizeof(response), "+SYSSTORE:%u\r\nOK\r\n",
                 wifi->sysstore);
        next_wifi_queue_text(wifi, response);
        return true;
    }
    if (strcmp(command, "AT+CWHOSTNAME?") == 0)
    {
        char response[96];
        snprintf(response, sizeof(response), "+CWHOSTNAME:%s\r\nOK\r\n",
                 wifi->hostname);
        next_wifi_queue_text(wifi, response);
        return true;
    }
    if (next_wifi_starts_with(command, "AT+CWHOSTNAME="))
    {
        char name[64];
        next_wifi_next_token(command + strlen("AT+CWHOSTNAME="), name,
                             sizeof(name));
        if (name[0] != '\0')
        {
            snprintf(wifi->hostname, sizeof(wifi->hostname), "%s", name);
            next_wifi_queue_text(wifi, "OK\r\n");
        }
        else
        {
            next_wifi_queue_text(wifi, "ERROR\r\n");
        }
        return true;
    }
    if (next_wifi_starts_with(command, "AT+PING="))
    {
        char host[256];
        char response[64];
        int rtt;
        next_wifi_next_token(command + strlen("AT+PING="), host, sizeof(host));
        rtt = host[0] != '\0' ? next_wifi_ping_host(host, 5000) : -1;
        if (rtt >= 0)
        {
            snprintf(response, sizeof(response), "+PING:%d\r\nOK\r\n", rtt);
            next_wifi_queue_text(wifi, response);
        }
        else
        {
            next_wifi_queue_text(wifi, "+PING:TIMEOUT\r\nERROR\r\n");
        }
        return true;
    }
    if (strcmp(command, "AT+PING=?") == 0)
    {
        next_wifi_queue_text(wifi, "+PING:(host)\r\nOK\r\n");
        return true;
    }
    if (next_wifi_starts_with(command, "AT+GSLP="))
    {
        unsigned duration;
        if (next_wifi_parse_uint(command + strlen("AT+GSLP="), &duration))
            next_wifi_queue_text(wifi, "OK\r\n");
        else
            next_wifi_queue_text(wifi, "ERROR\r\n");
        return true;
    }
    if (strcmp(command, "AT+CIUPDATE") == 0)
    {
        next_wifi_queue_text(wifi, "ERROR\r\n");
        return true;
    }
    if (strcmp(command, "AT+CIUPDATE?") == 0)
    {
        next_wifi_queue_text(wifi, "+CIPUPDATE:0\r\nOK\r\n");
        return true;
    }
    if (next_wifi_starts_with(command, "AT+SYSMSG=") ||
        next_wifi_starts_with(command, "AT+SYSSTORE=") ||
        next_wifi_starts_with(command, "AT+SLEEP=") ||
        next_wifi_starts_with(command, "AT+RFPOWER=") ||
        next_wifi_starts_with(command, "AT+CWAUTOCONN=") ||
        next_wifi_starts_with(command, "AT+SYSCFG=") ||
        next_wifi_starts_with(command, "AT+CIPV6=") ||
        next_wifi_starts_with(command, "AT+CIOBAUD=") ||
        strcmp(command, "AT&F") == 0 ||
        strcmp(command, "AT&W") == 0 ||
        strcmp(command, "AT&R") == 0)
    {
        const char *value = strchr(command, '=');
        unsigned parsed;
        if (value != NULL && next_wifi_parse_uint(value + 1, &parsed))
        {
            if (next_wifi_starts_with(command, "AT+SYSMSG="))
                wifi->sysmsg = parsed;
            else if (next_wifi_starts_with(command, "AT+SYSSTORE="))
                wifi->sysstore = parsed;
            else if (next_wifi_starts_with(command, "AT+SLEEP="))
                wifi->sleep_mode = parsed;
            else if (next_wifi_starts_with(command, "AT+RFPOWER="))
                wifi->rfpower = parsed;
            else if (next_wifi_starts_with(command, "AT+CWAUTOCONN="))
                wifi->autoconn = parsed;
            else if (next_wifi_starts_with(command, "AT+CIPV6="))
                wifi->ipv6 = parsed;
        }
        else if (value != NULL &&
                 !next_wifi_starts_with(command, "AT+SYSSTORE=") &&
                 !next_wifi_starts_with(command, "AT+SYSCFG="))
        {
            next_wifi_queue_text(wifi, "ERROR\r\n");
            return true;
        }
        next_wifi_queue_text(wifi, "OK\r\n");
        return true;
    }
    if (strcmp(command, "AT+ADC?") == 0 ||
        strcmp(command, "AT+SYSADC?") == 0)
    {
        next_wifi_queue_text(wifi, "+ADC:0\r\nOK\r\n");
        return true;
    }
    if (strcmp(command, "AT+RFVDD?") == 0)
    {
        next_wifi_queue_text(wifi, "+RFVDD:0\r\nOK\r\n");
        return true;
    }
    if (strcmp(command, "AT+CMD?") == 0 ||
        strcmp(command, "AT+SYSMFG?") == 0)
    {
        next_wifi_queue_text(wifi, "OK\r\n");
        return true;
    }
    if (next_wifi_starts_with(command, "AT+SYSIOSETCFG=") ||
        next_wifi_starts_with(command, "AT+SYSCFG="))
    {
        next_wifi_queue_text(wifi, "OK\r\n");
        return true;
    }
    if (next_wifi_starts_with(command, "AT+SYSGPIO"))
    {
        if (strcmp(command, "AT+SYSGPIOREAD=13") == 0)
            next_wifi_queue_text(wifi, "+SYSGPIOREAD:0\r\nOK\r\n");
        else
            next_wifi_queue_text(wifi, "OK\r\n");
        return true;
    }
    if (strcmp(command, "AT+SYSIOSETCFG=?") == 0 ||
        strcmp(command, "AT+SYSIOGETCFG") == 0)
    {
        next_wifi_queue_text(wifi, "OK\r\n");
        return true;
    }
    return false;
}

static bool next_wifi_handle_wifi_command(NextWifi *wifi,
                                          const char *command)
{
    const bool read_mode = strcmp(command, "AT+CWMODE?") == 0 ||
                           strcmp(command, "AT+CWMODE_CUR?") == 0 ||
                           strcmp(command, "AT+CWMODE_DEF?") == 0;
    if (read_mode)
    {
        char response[32];
        snprintf(response, sizeof(response), "+CWMODE:%u\r\nOK\r\n",
                 wifi->wifi_mode);
        next_wifi_queue_text(wifi, response);
        return true;
    }
    if (strcmp(command, "AT+CWMODE=?") == 0 ||
        strcmp(command, "AT+CWMODE_CUR=?") == 0 ||
        strcmp(command, "AT+CWMODE_DEF=?") == 0)
    {
        next_wifi_queue_text(wifi, "+CWMODE:(1-3)\r\nOK\r\n");
        return true;
    }
    if (next_wifi_starts_with(command, "AT+CWMODE=") ||
        next_wifi_starts_with(command, "AT+CWMODE_CUR=") ||
        next_wifi_starts_with(command, "AT+CWMODE_DEF="))
    {
        unsigned mode;
        const char *arguments = strchr(command, '=') + 1;
        if (next_wifi_parse_uint(arguments, &mode) && mode >= 1 && mode <= 3)
        {
            wifi->wifi_mode = (uint8_t)mode;
            next_wifi_queue_text(wifi, "OK\r\n");
        }
        else
        {
            next_wifi_queue_text(wifi, "ERROR\r\n");
        }
        return true;
    }
    if (strcmp(command, "AT+CWJAP?") == 0 ||
        strcmp(command, "AT+CWJAP_CUR?") == 0 ||
        strcmp(command, "AT+CWJAP_DEF?") == 0)
    {
        char response[160];
        if (!wifi->station_connected)
        {
            next_wifi_queue_text(wifi, "No AP\r\n");
            return true;
        }
        snprintf(response, sizeof(response),
                 "+CWJAP:\"%s\",\"%s\",%d,%d\r\nOK\r\n",
                 wifi->station_ssid, wifi->station_mac,
                 wifi->station_channel, wifi->station_rssi);
        next_wifi_queue_text(wifi, response);
        return true;
    }
    if (strcmp(command, "AT+CWJAP=?") == 0 ||
        strcmp(command, "AT+CWJAP_CUR=?") == 0 ||
        strcmp(command, "AT+CWJAP_DEF=?") == 0)
    {
        next_wifi_queue_text(wifi, "+CWJAP:(SSID,password)\r\nOK\r\n");
        return true;
    }
    if (next_wifi_starts_with(command, "AT+CWJAP=") ||
        next_wifi_starts_with(command, "AT+CWJAP_CUR=") ||
        next_wifi_starts_with(command, "AT+CWJAP_DEF="))
    {
        const char *arguments = strchr(command, '=') + 1;
        const char *ssid = next_wifi_trim_quotes(arguments);
        next_wifi_copy_unquoted(wifi->station_ssid,
                                sizeof(wifi->station_ssid), ssid);
        if (wifi->station_ssid[0] == '\0')
            snprintf(wifi->station_ssid, sizeof(wifi->station_ssid), "%s",
                     "HOST NETWORK");
        wifi->station_connected = true;
        next_wifi_queue_text(
            wifi, "WIFI CONNECTED\r\nWIFI GOT IP\r\n\r\nOK\r\n");
        return true;
    }
    if (strcmp(command, "AT+CWQAP") == 0)
    {
        wifi->station_connected = false;
        next_wifi_queue_text(wifi, "WIFI DISCONNECT\r\nOK\r\n");
        return true;
    }
    if (strcmp(command, "AT+CWLAP") == 0 ||
        next_wifi_starts_with(command, "AT+CWLAP="))
    {
        next_wifi_queue_text(
            wifi,
            "+CWLAP:(3,\"HOST NETWORK\",-30,\"00:00:00:00:00:00\",1)\r\n"
            "+CWLAP:(2,\"HOST GUEST\",-58,\"02:00:00:00:00:01\",6)\r\nOK\r\n");
        return true;
    }
    if (next_wifi_starts_with(command, "AT+CWLAPOPT="))
    {
        next_wifi_queue_text(wifi, "OK\r\n");
        return true;
    }
    if (strcmp(command, "AT+CWLIF") == 0)
    {
        if (wifi->station_connected)
        {
            char response[96];
            snprintf(response, sizeof(response), "+CWLIF:%s,%s\r\nOK\r\n",
                     wifi->station_ip, wifi->station_mac);
            next_wifi_queue_text(wifi, response);
        }
        else
        {
            next_wifi_queue_text(wifi, "OK\r\n");
        }
        return true;
    }
    if (strcmp(command, "AT+CIFSR") == 0)
    {
        char response[192];
        if (!wifi->station_connected && (wifi->wifi_mode & 1) != 0)
        {
            next_wifi_queue_text(wifi, "ERROR\r\n");
            return true;
        }
        if (wifi->station_connected)
        {
            snprintf(response, sizeof(response),
                     "+CIFSR:STAIP,\"%s\"\r\n"
                     "+CIFSR:STAMAC,\"%s\"\r\n",
                     wifi->station_ip, wifi->station_mac);
            next_wifi_queue_text(wifi, response);
        }
        if ((wifi->wifi_mode & 2) != 0)
        {
            snprintf(response, sizeof(response),
                     "+CIFSR:APIP,\"%s\"\r\n"
                     "+CIFSR:APMAC,\"%s\"\r\n",
                     wifi->ap_ip, wifi->ap_mac);
            next_wifi_queue_text(wifi, response);
        }
        next_wifi_queue_text(wifi, "OK\r\n");
        return true;
    }
    if (strcmp(command, "AT+CIPSTA?") == 0 ||
        strcmp(command, "AT+CIPSTA_CUR?") == 0 ||
        strcmp(command, "AT+CIPSTA_DEF?") == 0)
    {
        char response[224];
        snprintf(response, sizeof(response),
                 "+CIPSTA:ip:\"%s\"\r\n"
                 "+CIPSTA:gateway:\"%s\"\r\n"
                 "+CIPSTA:netmask:\"%s\"\r\nOK\r\n",
                 wifi->station_connected ? wifi->station_ip : "0.0.0.0",
                 wifi->station_gateway, wifi->station_netmask);
        next_wifi_queue_text(wifi, response);
        return true;
    }
    if (strcmp(command, "AT+CIPSTA=?") == 0 ||
        strcmp(command, "AT+CIPSTA_CUR=?") == 0 ||
        strcmp(command, "AT+CIPSTA_DEF=?") == 0)
    {
        next_wifi_queue_text(wifi,
                             "+CIPSTA:(ip,gateway,netmask)\r\nOK\r\n");
        return true;
    }
    if (next_wifi_starts_with(command, "AT+CIPSTA=") ||
        next_wifi_starts_with(command, "AT+CIPSTA_CUR=") ||
        next_wifi_starts_with(command, "AT+CIPSTA_DEF="))
    {
        const char *cursor = strchr(command, '=') + 1;
        char ip[16];
        char gateway[16];
        char netmask[16];
        cursor = next_wifi_next_token(cursor, ip, sizeof(ip));
        cursor = next_wifi_next_token(cursor, gateway, sizeof(gateway));
        next_wifi_next_token(cursor, netmask, sizeof(netmask));
        if (ip[0] != '\0' && gateway[0] != '\0' && netmask[0] != '\0')
        {
            snprintf(wifi->station_ip, sizeof(wifi->station_ip), "%s", ip);
            snprintf(wifi->station_gateway, sizeof(wifi->station_gateway),
                     "%s", gateway);
            snprintf(wifi->station_netmask, sizeof(wifi->station_netmask),
                     "%s", netmask);
            next_wifi_queue_text(wifi, "OK\r\n");
        }
        else
        {
            next_wifi_queue_text(wifi, "ERROR\r\n");
        }
        return true;
    }
    if (strcmp(command, "AT+CIPAP?") == 0 ||
        strcmp(command, "AT+CIPAP_CUR?") == 0 ||
        strcmp(command, "AT+CIPAP_DEF?") == 0)
    {
        char response[224];
        snprintf(response, sizeof(response),
                 "+CIPAP:ip:\"%s\"\r\n"
                 "+CIPAP:gateway:\"%s\"\r\n"
                 "+CIPAP:netmask:\"%s\"\r\nOK\r\n",
                 wifi->ap_ip, wifi->ap_gateway, wifi->ap_netmask);
        next_wifi_queue_text(wifi, response);
        return true;
    }
    if (strcmp(command, "AT+CIPAP=?") == 0 ||
        strcmp(command, "AT+CIPAP_CUR=?") == 0 ||
        strcmp(command, "AT+CIPAP_DEF=?") == 0)
    {
        next_wifi_queue_text(wifi, "+CIPAP:(ip,gateway,netmask)\r\nOK\r\n");
        return true;
    }
    if (next_wifi_starts_with(command, "AT+CIPAP=") ||
        next_wifi_starts_with(command, "AT+CIPAP_CUR=") ||
        next_wifi_starts_with(command, "AT+CIPAP_DEF="))
    {
        const char *cursor = strchr(command, '=') + 1;
        char ip[16];
        char gateway[16];
        char netmask[16];
        cursor = next_wifi_next_token(cursor, ip, sizeof(ip));
        cursor = next_wifi_next_token(cursor, gateway, sizeof(gateway));
        next_wifi_next_token(cursor, netmask, sizeof(netmask));
        if (ip[0] != '\0' && gateway[0] != '\0' && netmask[0] != '\0')
        {
            snprintf(wifi->ap_ip, sizeof(wifi->ap_ip), "%s", ip);
            snprintf(wifi->ap_gateway, sizeof(wifi->ap_gateway), "%s",
                     gateway);
            snprintf(wifi->ap_netmask, sizeof(wifi->ap_netmask), "%s",
                     netmask);
            next_wifi_queue_text(wifi, "OK\r\n");
        }
        else
        {
            next_wifi_queue_text(wifi, "ERROR\r\n");
        }
        return true;
    }
    if (strcmp(command, "AT+CIPSTAMAC?") == 0 ||
        strcmp(command, "AT+CIPSTAMAC_CUR?") == 0 ||
        strcmp(command, "AT+CIPSTAMAC_DEF?") == 0)
    {
        char response[64];
        snprintf(response, sizeof(response), "+CIPSTAMAC:\"%s\"\r\nOK\r\n",
                 wifi->station_mac);
        next_wifi_queue_text(wifi, response);
        return true;
    }
    if (strcmp(command, "AT+CIPAPMAC?") == 0 ||
        strcmp(command, "AT+CIPAPMAC_CUR?") == 0 ||
        strcmp(command, "AT+CIPAPMAC_DEF?") == 0)
    {
        char response[64];
        snprintf(response, sizeof(response), "+CIPAPMAC:\"%s\"\r\nOK\r\n",
                 wifi->ap_mac);
        next_wifi_queue_text(wifi, response);
        return true;
    }
    if (next_wifi_starts_with(command, "AT+CIPSTAMAC") ||
        next_wifi_starts_with(command, "AT+CIPAPMAC"))
    {
        const bool station = next_wifi_starts_with(command, "AT+CIPSTAMAC");
        char mac[18];
        const char *value = strchr(command, '=');
        if (value != NULL)
        {
            next_wifi_next_token(value + 1, mac, sizeof(mac));
            if (mac[0] != '\0')
            {
                snprintf(station ? wifi->station_mac : wifi->ap_mac,
                         sizeof(wifi->station_mac), "%s", mac);
            }
        }
        next_wifi_queue_text(wifi, "OK\r\n");
        return true;
    }
    if (strcmp(command, "AT+CIPDNS?") == 0 ||
        strcmp(command, "AT+CIPDNS_CUR?") == 0 ||
        strcmp(command, "AT+CIPDNS_DEF?") == 0)
    {
        char response[160];
        size_t position = 0;
        int written = snprintf(response, sizeof(response), "+CIPDNS:%u",
                               wifi->dns_count != 0 ? 1u : 0u);
        if (written < 0 || (size_t)written >= sizeof(response))
        {
            next_wifi_queue_text(wifi, "ERROR\r\n");
            return true;
        }
        position = (size_t)written;
        for (unsigned i = 0; i < wifi->dns_count && i < 3; ++i)
        {
            written = snprintf(response + position, sizeof(response) - position,
                               ",\"%s\"", wifi->dns_server[i]);
            if (written < 0 || (size_t)written >= sizeof(response) - position)
            {
                next_wifi_queue_text(wifi, "ERROR\r\n");
                return true;
            }
            position += (size_t)written;
        }
        written = snprintf(response + position, sizeof(response) - position,
                           "\r\nOK\r\n");
        if (written < 0 || (size_t)written >= sizeof(response) - position)
        {
            next_wifi_queue_text(wifi, "ERROR\r\n");
            return true;
        }
        next_wifi_queue_text(wifi, response);
        return true;
    }
    if (strcmp(command, "AT+CIPDNS=?") == 0 ||
        strcmp(command, "AT+CIPDNS_CUR=?") == 0 ||
        strcmp(command, "AT+CIPDNS_DEF=?") == 0)
    {
        next_wifi_queue_text(wifi, "+CIPDNS:(enable,server)\r\nOK\r\n");
        return true;
    }
    if (next_wifi_starts_with(command, "AT+CIPDNS=") ||
        next_wifi_starts_with(command, "AT+CIPDNS_CUR=") ||
        next_wifi_starts_with(command, "AT+CIPDNS_DEF="))
    {
        const char *cursor = strchr(command, '=') + 1;
        char enable[8];
        unsigned enabled = 0;
        unsigned count = 0;
        cursor = next_wifi_next_token(cursor, enable, sizeof(enable));
        if (next_wifi_parse_uint(enable, &enabled))
        {
            while (count < 3)
            {
                char server[16];
                cursor = next_wifi_next_token(cursor, server, sizeof(server));
                if (server[0] == '\0')
                    break;
                snprintf(wifi->dns_server[count],
                         sizeof(wifi->dns_server[count]), "%s", server);
                ++count;
            }
            wifi->dns_count = enabled ? count : 0;
            if (enabled && wifi->dns_count == 0)
            {
                snprintf(wifi->dns_server[0],
                         sizeof(wifi->dns_server[0]), "%s", "8.8.8.8");
                wifi->dns_count = 1;
            }
            next_wifi_queue_text(wifi, "OK\r\n");
        }
        else
        {
            next_wifi_queue_text(wifi, "ERROR\r\n");
        }
        return true;
    }
    if (strcmp(command, "AT+CWCOUNTRY?") == 0 ||
        strcmp(command, "AT+CWCOUNTRY_CUR?") == 0 ||
        strcmp(command, "AT+CWCOUNTRY_DEF?") == 0)
    {
        next_wifi_queue_text(wifi,
                             "+CWCOUNTRY:0,\"CN\",1,13\r\nOK\r\n");
        return true;
    }
    if (strcmp(command, "AT+CWCOUNTRY=?") == 0 ||
        strcmp(command, "AT+CWCOUNTRY_CUR=?") == 0 ||
        strcmp(command, "AT+CWCOUNTRY_DEF=?") == 0)
    {
        next_wifi_queue_text(wifi,
                             "+CWCOUNTRY:(0-4),(CN,JP,US),(1-14),(1-14)\r\n"
                             "OK\r\n");
        return true;
    }
    if (next_wifi_starts_with(command, "AT+CWCOUNTRY"))
    {
        next_wifi_queue_text(wifi, "OK\r\n");
        return true;
    }
    if (strcmp(command, "AT+CWSAP?") == 0 ||
        strcmp(command, "AT+CWSAP_CUR?") == 0 ||
        strcmp(command, "AT+CWSAP_DEF?") == 0)
    {
        char response[224];
        snprintf(response, sizeof(response),
                 "+CWSAP:\"%s\",\"%s\",%u,%u,%u,%u\r\nOK\r\n",
                 wifi->ap_ssid, wifi->ap_password, wifi->ap_channel,
                 wifi->ap_encryption, wifi->ap_max_connections,
                 wifi->ap_hidden);
        next_wifi_queue_text(wifi, response);
        return true;
    }
    if (strcmp(command, "AT+CWSAP=?") == 0 ||
        strcmp(command, "AT+CWSAP_CUR=?") == 0 ||
        strcmp(command, "AT+CWSAP_DEF=?") == 0)
    {
        next_wifi_queue_text(
            wifi,
            "+CWSAP:(SSID,password,channel,ecn,max connection,ssid hidden)\r\n"
            "OK\r\n");
        return true;
    }
    if (next_wifi_starts_with(command, "AT+CWSAP=") ||
        next_wifi_starts_with(command, "AT+CWSAP_CUR=") ||
        next_wifi_starts_with(command, "AT+CWSAP_DEF="))
    {
        const char *cursor = strchr(command, '=') + 1;
        char ssid[33];
        char password[65];
        char channel[8];
        char encryption[8];
        char maximum[8];
        char hidden[8];
        unsigned parsed;
        cursor = next_wifi_next_token(cursor, ssid, sizeof(ssid));
        cursor = next_wifi_next_token(cursor, password, sizeof(password));
        cursor = next_wifi_next_token(cursor, channel, sizeof(channel));
        cursor = next_wifi_next_token(cursor, encryption, sizeof(encryption));
        cursor = next_wifi_next_token(cursor, maximum, sizeof(maximum));
        next_wifi_next_token(cursor, hidden, sizeof(hidden));
        if (ssid[0] != '\0' && next_wifi_parse_uint(channel, &parsed) &&
            parsed >= 1 && parsed <= 13)
        {
            snprintf(wifi->ap_ssid, sizeof(wifi->ap_ssid), "%s", ssid);
            snprintf(wifi->ap_password, sizeof(wifi->ap_password), "%s",
                     password);
            wifi->ap_channel = parsed;
            if (next_wifi_parse_uint(encryption, &parsed) && parsed <= 4)
                wifi->ap_encryption = parsed;
            if (next_wifi_parse_uint(maximum, &parsed) && parsed >= 1)
                wifi->ap_max_connections = parsed;
            if (next_wifi_parse_uint(hidden, &parsed) && parsed <= 1)
                wifi->ap_hidden = parsed;
            wifi->wps_enabled = false;
            next_wifi_queue_text(wifi, "OK\r\n");
        }
        else
        {
            next_wifi_queue_text(wifi, "ERROR\r\n");
        }
        return true;
    }
    if (strcmp(command, "AT+CWDHCP?") == 0)
    {
        char response[40];
        snprintf(response, sizeof(response), "+CWDHCP:%u,%u\r\nOK\r\n",
                 wifi->dhcp_station, wifi->dhcp_ap);
        next_wifi_queue_text(wifi, response);
        return true;
    }
    if (strcmp(command, "AT+CWDHCP_CUR?") == 0 ||
        strcmp(command, "AT+CWDHCP_DEF?") == 0)
    {
        char response[40];
        snprintf(response, sizeof(response), "+CWDHCP:%u\r\nOK\r\n",
                 wifi->dhcp_station);
        next_wifi_queue_text(wifi, response);
        return true;
    }
    if (strcmp(command, "AT+CWDHCP=?") == 0 ||
        strcmp(command, "AT+CWDHCP_CUR=?") == 0 ||
        strcmp(command, "AT+CWDHCP_DEF=?") == 0)
    {
        next_wifi_queue_text(wifi, "+CWDHCP:(0-2),(0-1)\r\nOK\r\n");
        return true;
    }
    if (next_wifi_starts_with(command, "AT+CWDHCP=") ||
        next_wifi_starts_with(command, "AT+CWDHCP_CUR=") ||
        next_wifi_starts_with(command, "AT+CWDHCP_DEF="))
    {
        const char *cursor = strchr(command, '=') + 1;
        const bool extended = next_wifi_starts_with(command, "AT+CWDHCP=");
        char first[8];
        char second[8];
        unsigned mode = 2;
        unsigned enabled = 0;
        cursor = next_wifi_next_token(cursor, first, sizeof(first));
        next_wifi_next_token(cursor, second, sizeof(second));
        if (extended)
        {
            if (!next_wifi_parse_uint(first, &mode) || mode > 2 ||
                !next_wifi_parse_uint(second, &enabled) || enabled > 1)
            {
                next_wifi_queue_text(wifi, "ERROR\r\n");
                return true;
            }
        }
        else if (!next_wifi_parse_uint(first, &enabled) || enabled > 1)
        {
            next_wifi_queue_text(wifi, "ERROR\r\n");
            return true;
        }
        if ((mode & 1) != 0)
            wifi->dhcp_station = enabled;
        if ((mode & 2) != 0)
            wifi->dhcp_ap = enabled;
        next_wifi_queue_text(wifi, "OK\r\n");
        return true;
    }
    if (next_wifi_starts_with(command, "AT+CWJEAP="))
    {
        next_wifi_queue_text(wifi, "OK\r\n");
        return true;
    }
    if (strcmp(command, "AT+CWSTARTDISCOVER") == 0 ||
        strcmp(command, "AT+CWSTOPDISCOVER") == 0)
    {
        next_wifi_queue_text(wifi, "OK\r\n");
        return true;
    }
    if (strcmp(command, "AT+MDNS?") == 0)
    {
        char response[192];
        snprintf(response, sizeof(response),
                 "+MDNS:%u,\"%s\",\"%s\",%u\r\nOK\r\n",
                 wifi->mdns_enabled ? 1u : 0u, wifi->mdns_host,
                 wifi->mdns_server, wifi->mdns_port);
        next_wifi_queue_text(wifi, response);
        return true;
    }
    if (next_wifi_starts_with(command, "AT+MDNS="))
    {
        const char *cursor = strchr(command, '=') + 1;
        char enabled[8];
        char host[64];
        char server[64];
        char port[8];
        unsigned parsed;
        cursor = next_wifi_next_token(cursor, enabled, sizeof(enabled));
        cursor = next_wifi_next_token(cursor, host, sizeof(host));
        cursor = next_wifi_next_token(cursor, server, sizeof(server));
        next_wifi_next_token(cursor, port, sizeof(port));
        if (!next_wifi_parse_uint(enabled, &parsed) || parsed > 1)
        {
            next_wifi_queue_text(wifi, "ERROR\r\n");
            return true;
        }
        wifi->mdns_enabled = parsed != 0;
        if (!wifi->mdns_enabled)
        {
            next_wifi_queue_text(wifi, "OK\r\n");
            return true;
        }
        if (host[0] != '\0')
            snprintf(wifi->mdns_host, sizeof(wifi->mdns_host), "%s", host);
        if (server[0] != '\0')
            snprintf(wifi->mdns_server, sizeof(wifi->mdns_server), "%s",
                     server);
        if (next_wifi_parse_uint(port, &parsed) && parsed != 0)
            wifi->mdns_port = parsed;
        next_wifi_queue_text(wifi, "OK\r\n");
        return true;
    }
    if (strcmp(command, "AT+CIPSNTPCFG?") == 0)
    {
        char response[128];
        char timezone[16];
        next_wifi_format_timezone(wifi->sntp_timezone, timezone,
                                  sizeof(timezone));
        snprintf(response, sizeof(response),
                 "+CIPSNTPCFG:%u,%s,\"%s\"\r\nOK\r\n", wifi->sntp_enabled,
                 timezone, wifi->sntp_server[0]);
        next_wifi_queue_text(wifi, response);
        return true;
    }
    if (next_wifi_starts_with(command, "AT+CIPSNTPCFG="))
    {
        const char *cursor = strchr(command, '=') + 1;
        char enabled[8];
        char timezone[16];
        unsigned parsed;
        int minutes;
        cursor = next_wifi_next_token(cursor, enabled, sizeof(enabled));
        cursor = next_wifi_next_token(cursor, timezone, sizeof(timezone));
        if (!next_wifi_parse_uint(enabled, &parsed) || parsed > 1 ||
            !next_wifi_parse_timezone(timezone, &minutes))
        {
            next_wifi_queue_text(wifi, "ERROR\r\n");
            return true;
        }
        wifi->sntp_enabled = parsed;
        wifi->sntp_timezone = minutes;
        for (unsigned i = 0; i < 3; ++i)
        {
            char server[64];
            cursor = next_wifi_next_token(cursor, server, sizeof(server));
            if (server[0] == '\0')
                break;
            snprintf(wifi->sntp_server[i], sizeof(wifi->sntp_server[i]),
                     "%s", server);
        }
        next_wifi_queue_text(wifi, "OK\r\n");
        return true;
    }
    if (strcmp(command, "AT+CIPSNTPTIME?") == 0)
    {
        char timestamp[64];
        char response[96];
        if (!wifi->sntp_enabled)
        {
            next_wifi_queue_text(wifi, "ERROR\r\n");
            return true;
        }
        next_wifi_format_sntp_time(wifi, timestamp, sizeof(timestamp));
        snprintf(response, sizeof(response), "+CIPSNTPTIME:%s\r\nOK\r\n",
                 timestamp);
        next_wifi_queue_text(wifi, response);
        return true;
    }
    if (strcmp(command, "AT+CIPSNTPTIME") == 0)
    {
        char timestamp[64];
        char response[96];
        next_wifi_format_sntp_time(wifi, timestamp, sizeof(timestamp));
        snprintf(response, sizeof(response), "+CIPSNTPTIME:%s\r\nOK\r\n",
                 timestamp);
        next_wifi_queue_text(wifi, response);
        return true;
    }
    if (strcmp(command, "AT+CWLAPOPT?") == 0)
    {
        next_wifi_queue_text(wifi, "+CWLAPOPT:127,0\r\nOK\r\n");
        return true;
    }
    if (strcmp(command, "AT+CWSTARTSMART") == 0 ||
        strcmp(command, "AT+CWSTOPSMART") == 0)
    {
        wifi->wps_enabled = strcmp(command, "AT+CWSTARTSMART") == 0;
        next_wifi_queue_text(wifi, "OK\r\n");
        return true;
    }
    if (strcmp(command, "AT+WPS?") == 0)
    {
        char response[32];
        snprintf(response, sizeof(response), "+WPS:%u\r\nOK\r\n",
                 wifi->wps_enabled ? 1u : 0u);
        next_wifi_queue_text(wifi, response);
        return true;
    }
    if (next_wifi_starts_with(command, "AT+WPS=") ||
        strcmp(command, "AT+WPSSTART") == 0 ||
        strcmp(command, "AT+WPSSTOP") == 0)
    {
        if (strcmp(command, "AT+WPSSTART") == 0)
            wifi->wps_enabled = true;
        else if (strcmp(command, "AT+WPSSTOP") == 0)
            wifi->wps_enabled = false;
        else
        {
            const char *value = strchr(command, '=');
            unsigned parsed = 0;
            if (value != NULL)
                next_wifi_parse_uint(value + 1, &parsed);
            wifi->wps_enabled = parsed != 0;
        }
        next_wifi_queue_text(wifi, "OK\r\n");
        return true;
    }
    return false;
}

static bool next_wifi_handle_connection_command(NextWifi *wifi,
                                                const char *command)
{
    if (strcmp(command, "AT+CIPMUX?") == 0)
    {
        next_wifi_queue_text(wifi, wifi->multiplex
            ? "+CIPMUX:1\r\nOK\r\n" : "+CIPMUX:0\r\nOK\r\n");
        return true;
    }
    if (strcmp(command, "AT+CIPMUX=0") == 0 ||
        strcmp(command, "AT+CIPMUX=1") == 0)
    {
        const bool enabled = command[strlen(command) - 1] == '1';
        bool busy = wifi->listener != INVALID_SOCKET;
        for (unsigned i = 0; i < NEXT_WIFI_MAX_CONNECTIONS; ++i)
            busy = busy || wifi->connections[i].active;
        if (enabled != wifi->multiplex && busy)
        {
            next_wifi_queue_text(wifi, "ERROR\r\n");
            return true;
        }
        wifi->multiplex = enabled;
        if (!enabled)
        {
            wifi->cipmode = false;
            wifi->transparent = false;
        }
        next_wifi_queue_text(wifi, "OK\r\n");
        return true;
    }
    if (strcmp(command, "AT+CIPMODE?") == 0)
    {
        next_wifi_queue_text(wifi, wifi->cipmode
            ? "+CIPMODE:1\r\nOK\r\n" : "+CIPMODE:0\r\nOK\r\n");
        return true;
    }
    if (strcmp(command, "AT+CIPMODE=0") == 0 ||
        strcmp(command, "AT+CIPMODE=1") == 0)
    {
        const bool enabled = command[strlen(command) - 1] == '1';
        if (enabled && (wifi->multiplex ||
                        wifi->listener != INVALID_SOCKET))
        {
            next_wifi_queue_text(wifi, "ERROR\r\n");
            return true;
        }
        wifi->cipmode = enabled;
        if (!enabled)
        {
            wifi->transparent = false;
            wifi->transparent_pluses = 0;
        }
        next_wifi_queue_text(wifi, "OK\r\n");
        return true;
    }
    if (strcmp(command, "AT+CIPDINFO?") == 0)
    {
        next_wifi_queue_text(wifi, wifi->cipdinfo
            ? "+CIPDINFO:1\r\nOK\r\n" : "+CIPDINFO:0\r\nOK\r\n");
        return true;
    }
    if (strcmp(command, "AT+CIPDINFO=0") == 0 ||
        strcmp(command, "AT+CIPDINFO=1") == 0)
    {
        wifi->cipdinfo = command[strlen(command) - 1] == '1';
        next_wifi_queue_text(wifi, "OK\r\n");
        return true;
    }
    if (strcmp(command, "AT+CIPRECVCFG?") == 0)
    {
        next_wifi_queue_text(wifi, wifi->recv_mode
            ? "+CIPRECVCFG:1\r\nOK\r\n" : "+CIPRECVCFG:0\r\nOK\r\n");
        return true;
    }
    if (strcmp(command, "AT+CIPRECVCFG=0") == 0 ||
        strcmp(command, "AT+CIPRECVCFG=1") == 0)
    {
        const bool enabled = command[strlen(command) - 1] == '1';
        wifi->recv_mode = enabled;
        if (!enabled)
        {
            for (int i = 0; i < NEXT_WIFI_MAX_CONNECTIONS; ++i)
                next_wifi_deliver_passive(wifi, i);
        }
        next_wifi_queue_text(wifi, "OK\r\n");
        return true;
    }
    if (strcmp(command, "AT+CIPRECVMODE?") == 0)
    {
        next_wifi_queue_text(wifi, wifi->recv_mode
            ? "+CIPRECVMODE:1\r\nOK\r\n" : "+CIPRECVMODE:0\r\nOK\r\n");
        return true;
    }
    if (strcmp(command, "AT+CIPRECVLEN?") == 0)
    {
        char response[128];
        size_t position = 0;
        bool first = true;
        int written = snprintf(response, sizeof(response), "+CIPRECVLEN:");
        if (written < 0 || (size_t)written >= sizeof(response))
        {
            next_wifi_queue_text(wifi, "ERROR\r\n");
            return true;
        }
        position = (size_t)written;
        for (unsigned i = 0; i < NEXT_WIFI_MAX_CONNECTIONS; ++i)
        {
            if (!wifi->connections[i].active)
                continue;
            written = snprintf(
                response + position, sizeof(response) - position, "%s%u",
                first ? "" : ",", (unsigned)wifi->connections[i].passive_length);
            if (written < 0 || (size_t)written >= sizeof(response) - position)
            {
                next_wifi_queue_text(wifi, "ERROR\r\n");
                return true;
            }
            position += (size_t)written;
            first = false;
        }
        written = snprintf(response + position, sizeof(response) - position,
                           "\r\nOK\r\n");
        if (written < 0 || (size_t)written >= sizeof(response) - position)
        {
            next_wifi_queue_text(wifi, "ERROR\r\n");
            return true;
        }
        next_wifi_queue_text(wifi, response);
        return true;
    }
    if (next_wifi_starts_with(command, "AT+CIPRECVDATA="))
    {
        int id = 0;
        unsigned length = 0;
        int fields;
        const char *arguments = command + strlen("AT+CIPRECVDATA=");
        if (wifi->multiplex)
        {
            fields = sscanf(arguments, "%d,%u", &id, &length);
            if (fields != 2)
                length = 0;
        }
        else if (sscanf(arguments, "%u", &length) != 1)
        {
            length = 0;
        }
        if (id < 0 || id >= NEXT_WIFI_MAX_CONNECTIONS || length == 0 ||
            !wifi->connections[id].active ||
            wifi->connections[id].passive_length == 0)
        {
            next_wifi_queue_text(wifi, "ERROR\r\n");
            return true;
        }
        if (length > wifi->connections[id].passive_length)
            length = (unsigned)wifi->connections[id].passive_length;
        {
            char header[48];
            NextWifiConnection *connection = &wifi->connections[id];
            if (wifi->multiplex)
                snprintf(header, sizeof(header), "+CIPRECVDATA:%d,%u,",
                         id, length);
            else
                snprintf(header, sizeof(header), "+CIPRECVDATA:%u,", length);
            next_wifi_queue_text(wifi, header);
            next_wifi_queue_bytes(wifi, connection->passive, length);
            memmove(connection->passive, connection->passive + length,
                    connection->passive_length - length);
            connection->passive_length -= length;
            next_wifi_queue_text(wifi, "\r\nOK\r\n");
        }
        return true;
    }
    if (strcmp(command, "AT+CIPSERVER?") == 0)
    {
        char response[48];
        if (wifi->server_enabled)
        {
            snprintf(response, sizeof(response), "+CIPSERVER:1,%u\r\nOK\r\n",
                     wifi->server_port);
        }
        else
        {
            snprintf(response, sizeof(response), "+CIPSERVER:0\r\nOK\r\n");
        }
        next_wifi_queue_text(wifi, response);
        return true;
    }
    if (strcmp(command, "AT+CIPSERVER=0") == 0)
    {
        for (unsigned i = 0; i < NEXT_WIFI_MAX_CONNECTIONS; ++i)
        {
            if (wifi->connections[i].active && wifi->connections[i].server)
            {
                next_wifi_close_connection(&wifi->connections[i]);
                next_wifi_connection_notice(wifi, (int)i, "CLOSED");
            }
        }
        next_wifi_close_listener(wifi);
        next_wifi_queue_text(wifi, "OK\r\n");
        return true;
    }
    if (next_wifi_starts_with(command, "AT+CIPSERVER="))
    {
        int mode = 0;
        unsigned port = 0;
        const char *arguments = command + strlen("AT+CIPSERVER=");
        int fields = sscanf(arguments, "%d,%u", &mode, &port);
        if (!wifi->multiplex)
        {
            next_wifi_queue_text(wifi, "ERROR\r\n");
            return true;
        }
        if (mode != 1 || fields != 2 || !next_wifi_open_listener(wifi, port))
        {
            next_wifi_queue_text(wifi, "ERROR\r\n");
            return true;
        }
        next_wifi_queue_text(wifi, "OK\r\n");
        return true;
    }
    if (strcmp(command, "AT+CIPSERVERMAXCONN?") == 0)
    {
        char response[48];
        snprintf(response, sizeof(response), "+CIPSERVERMAXCONN:%u\r\nOK\r\n",
                 wifi->server_max_connections);
        next_wifi_queue_text(wifi, response);
        return true;
    }
    if (next_wifi_starts_with(command, "AT+CIPSERVERMAXCONN="))
    {
        unsigned maximum;
        if (next_wifi_parse_uint(command + strlen("AT+CIPSERVERMAXCONN="),
                                 &maximum) &&
            maximum >= 1 && maximum <= NEXT_WIFI_MAX_CONNECTIONS)
        {
            wifi->server_max_connections = maximum;
            next_wifi_queue_text(wifi, "OK\r\n");
        }
        else
        {
            next_wifi_queue_text(wifi, "ERROR\r\n");
        }
        return true;
    }
    if (strcmp(command, "AT+CIPSTO?") == 0)
    {
        char response[48];
        snprintf(response, sizeof(response), "+CIPSTO:%u\r\nOK\r\n",
                 (unsigned)wifi->server_timeout);
        next_wifi_queue_text(wifi, response);
        return true;
    }
    if (next_wifi_starts_with(command, "AT+CIPSTO="))
    {
        unsigned timeout;
        if (next_wifi_parse_uint(command + strlen("AT+CIPSTO="), &timeout) &&
            timeout <= 7200)
        {
            wifi->server_timeout = timeout;
            next_wifi_queue_text(wifi, "OK\r\n");
        }
        else
        {
            next_wifi_queue_text(wifi, "ERROR\r\n");
        }
        return true;
    }
    if (next_wifi_starts_with(command, "AT+CIPSTART="))
    {
        int id;
        unsigned port;
        char type[8];
        char host[256];
        bool has_slot = true;
        if (!next_wifi_parse_cipstart(command + strlen("AT+CIPSTART="),
                                      wifi->multiplex, &id, type, sizeof(type),
                                      host, sizeof(host), &port))
        {
            next_wifi_queue_text(wifi, "ERROR\r\n");
            return true;
        }
        if (id < 0 || id >= NEXT_WIFI_MAX_CONNECTIONS)
        {
            next_wifi_queue_text(wifi, "ERROR\r\n");
            return true;
        }
        for (unsigned i = 0; i < NEXT_WIFI_MAX_CONNECTIONS; ++i)
            has_slot = has_slot && !wifi->connections[i].active;
        if (wifi->connections[id].active ||
            (!wifi->multiplex && !has_slot))
        {
            next_wifi_queue_text(wifi,
                                 "ALREADY CONNECTED\r\nERROR\r\n");
            return true;
        }
        if (next_wifi_open_connection(wifi, id, type, host, port))
        {
            next_wifi_connection_notice(wifi, id, "CONNECT");
            next_wifi_queue_text(wifi, "OK\r\n");
        }
        else
        {
            next_wifi_queue_text(wifi, "ERROR\r\n");
        }
        return true;
    }
    if (strcmp(command, "AT+CIPSEND") == 0)
    {
        if (wifi->cipmode && !wifi->multiplex &&
            wifi->connections[0].active)
        {
            next_wifi_begin_transparent(wifi);
        }
        else
        {
            next_wifi_queue_text(wifi, "ERROR\r\n");
        }
        return true;
    }
    if (next_wifi_starts_with(command, "AT+CIPSEND="))
    {
        next_wifi_begin_send(wifi, command + strlen("AT+CIPSEND="));
        return true;
    }
    if (next_wifi_starts_with(command, "AT+CIPSENDEX="))
    {
        next_wifi_begin_send(wifi, command + strlen("AT+CIPSENDEX="));
        return true;
    }
    if (next_wifi_starts_with(command, "AT+CIPSENDBUF="))
    {
        next_wifi_begin_send(wifi, command + strlen("AT+CIPSENDBUF="));
        return true;
    }
    if (strcmp(command, "AT+CIPCLOSE") == 0 ||
        next_wifi_starts_with(command, "AT+CIPCLOSE="))
    {
        int id = 0;
        if (wifi->multiplex &&
            (!next_wifi_starts_with(command, "AT+CIPCLOSE=") ||
             sscanf(command + strlen("AT+CIPCLOSE="), "%d", &id) != 1))
        {
            next_wifi_queue_text(wifi, "ERROR\r\n");
        }
        else if (id >= 0 && id < NEXT_WIFI_MAX_CONNECTIONS &&
                 wifi->connections[id].active)
        {
            wifi->transparent = false;
            wifi->transparent_pluses = 0;
            next_wifi_close_connection(&wifi->connections[id]);
            next_wifi_connection_notice(wifi, id, "CLOSED");
            next_wifi_queue_text(wifi, "OK\r\n");
        }
        else
        {
            next_wifi_queue_text(wifi, "ERROR\r\n");
        }
        return true;
    }
    if (strcmp(command, "AT+CIPSTATUS") == 0)
    {
        next_wifi_report_status(wifi);
        return true;
    }
    if (next_wifi_starts_with(command, "AT+CIPDOMAIN="))
    {
        char host[256];
        char address[64];
        const char *arguments = command + strlen("AT+CIPDOMAIN=");
        char response[96];
        next_wifi_copy_unquoted(host, sizeof(host),
                                next_wifi_trim_quotes(arguments));
        if (wifi->station_connected && host[0] != '\0' &&
            next_wifi_resolve_host(host, address, sizeof(address)))
        {
            snprintf(response, sizeof(response), "+CIPDOMAIN:%s\r\nOK\r\n",
                     address);
            next_wifi_queue_text(wifi, response);
        }
        else
        {
            next_wifi_queue_text(wifi, "ERROR\r\n");
        }
        return true;
    }
    if (strcmp(command, "AT+CIPBUFSTATUS?") == 0)
    {
        next_wifi_queue_text(wifi, "+CIPBUFSTATUS:0,0,0\r\nOK\r\n");
        return true;
    }
    if (strcmp(command, "AT+CIPBUFRESET") == 0)
    {
        next_wifi_queue_text(wifi, "OK\r\n");
        return true;
    }
    if (next_wifi_starts_with(command, "AT+CIPSTARTEX="))
    {
        int id;
        unsigned port;
        char type[8];
        char host[256];
        if (!next_wifi_parse_cipstart(command + strlen("AT+CIPSTARTEX="),
                                      wifi->multiplex, &id, type, sizeof(type),
                                      host, sizeof(host), &port))
        {
            next_wifi_queue_text(wifi, "ERROR\r\n");
            return true;
        }
        if (id < 0 || id >= NEXT_WIFI_MAX_CONNECTIONS)
        {
            next_wifi_queue_text(wifi, "ERROR\r\n");
            return true;
        }
        if (wifi->connections[id].active ||
            next_wifi_open_connection(wifi, id, type, host, port) == false)
        {
            next_wifi_queue_text(wifi, "ERROR\r\n");
            return true;
        }
        next_wifi_connection_notice(wifi, id, "CONNECT");
        next_wifi_queue_text(wifi, "OK\r\n");
        return true;
    }
    if (strcmp(command, "AT+CIPSTATE?") == 0)
    {
        char response[192];
        for (unsigned i = 0; i < NEXT_WIFI_MAX_CONNECTIONS; ++i)
        {
            const NextWifiConnection *connection = &wifi->connections[i];
            if (!connection->active)
                continue;
            snprintf(response, sizeof(response),
                     "+CIPSTATE:%u,\"%s\",\"%s\",%u,%u,0\r\n", i,
                     connection->type,
                     connection->peer_host[0] != '\0' ? connection->peer_host
                                                     : connection->host,
                     connection->remote_port, connection->local_port);
            next_wifi_queue_text(wifi, response);
        }
        next_wifi_queue_text(wifi, "OK\r\n");
        return true;
    }
    if (strcmp(command, "AT+CIPSTATE=?") == 0)
    {
        next_wifi_queue_text(wifi, "+CIPSTATE:(link ID)\r\nOK\r\n");
        return true;
    }
    if (strcmp(command, "AT+CIPCLOSEMODE?") == 0)
    {
        char response[40];
        snprintf(response, sizeof(response), "+CIPCLOSEMODE:%u\r\nOK\r\n",
                 wifi->close_mode);
        next_wifi_queue_text(wifi, response);
        return true;
    }
    if (next_wifi_starts_with(command, "AT+CIPCLOSEMODE="))
    {
        unsigned mode;
        if (next_wifi_parse_uint(command + strlen("AT+CIPCLOSEMODE="),
                                 &mode) &&
            mode <= 1)
        {
            wifi->close_mode = mode;
            next_wifi_queue_text(wifi, "OK\r\n");
        }
        else
        {
            next_wifi_queue_text(wifi, "ERROR\r\n");
        }
        return true;
    }
    if (strcmp(command, "AT+CIPCHECKSEQ?") == 0)
    {
        char response[40];
        snprintf(response, sizeof(response), "+CIPCHECKSEQ:%u\r\nOK\r\n",
                 wifi->check_seq);
        next_wifi_queue_text(wifi, response);
        return true;
    }
    if (next_wifi_starts_with(command, "AT+CIPCHECKSEQ="))
    {
        unsigned enabled;
        if (next_wifi_parse_uint(command + strlen("AT+CIPCHECKSEQ="),
                                 &enabled) &&
            enabled <= 1)
        {
            wifi->check_seq = enabled;
            next_wifi_queue_text(wifi, "OK\r\n");
        }
        else
        {
            next_wifi_queue_text(wifi, "ERROR\r\n");
        }
        return true;
    }
    if (strcmp(command, "AT+CIPRECONNINTV?") == 0)
    {
        char response[40];
        snprintf(response, sizeof(response), "+CIPRECONNINTV:%u\r\nOK\r\n",
                 wifi->reconnection_interval);
        next_wifi_queue_text(wifi, response);
        return true;
    }
    if (next_wifi_starts_with(command, "AT+CIPRECONNINTV="))
    {
        unsigned interval;
        if (next_wifi_parse_uint(command + strlen("AT+CIPRECONNINTV="),
                                 &interval) &&
            interval >= 1 && interval <= 3600)
        {
            wifi->reconnection_interval = interval;
            next_wifi_queue_text(wifi, "OK\r\n");
        }
        else
        {
            next_wifi_queue_text(wifi, "ERROR\r\n");
        }
        return true;
    }
    if (strcmp(command, "AT+CIPTCPOPT?") == 0)
    {
        next_wifi_queue_text(wifi, "+CIPTCPOPT:0,0,0,0\r\nOK\r\n");
        return true;
    }
    if (next_wifi_starts_with(command, "AT+CIPTCPOPT="))
    {
        next_wifi_queue_text(wifi, "OK\r\n");
        return true;
    }
    if (strcmp(command, "AT+CIPIOSET?") == 0)
    {
        next_wifi_queue_text(wifi, "+CIPIOSET:0,0\r\nOK\r\n");
        return true;
    }
    if (next_wifi_starts_with(command, "AT+CIPIOSET=") ||
        strcmp(command, "AT+CIPIOGET") == 0)
    {
        next_wifi_queue_text(wifi, "OK\r\n");
        return true;
    }
    if (strcmp(command, "AT+CIPMODE=?") == 0)
    {
        next_wifi_queue_text(wifi, "+CIPMODE:(0-1)\r\nOK\r\n");
        return true;
    }
    if (strcmp(command, "AT+CIPMUX=?") == 0)
    {
        next_wifi_queue_text(wifi, "+CIPMUX:(0-1)\r\nOK\r\n");
        return true;
    }
    if (strcmp(command, "AT+CIPDINFO=?") == 0)
    {
        next_wifi_queue_text(wifi, "+CIPDINFO:(0-1)\r\nOK\r\n");
        return true;
    }
    if (strcmp(command, "AT+CIPRECVCFG=?") == 0 ||
        strcmp(command, "AT+CIPRECVMODE=?") == 0)
    {
        next_wifi_queue_text(wifi, "+CIPRECVMODE:(0-1)\r\nOK\r\n");
        return true;
    }
    if (strcmp(command, "AT+CIPRECVMODE=0") == 0 ||
        strcmp(command, "AT+CIPRECVMODE=1") == 0)
    {
        const bool passive = command[strlen(command) - 1] == '1';
        wifi->recv_mode = passive;
        if (!passive)
        {
            for (int i = 0; i < NEXT_WIFI_MAX_CONNECTIONS; ++i)
                next_wifi_deliver_passive(wifi, i);
        }
        next_wifi_queue_text(wifi, "OK\r\n");
        return true;
    }
    if (next_wifi_starts_with(command, "AT+CIPSSLCCONF?"))
    {
        next_wifi_queue_text(wifi,
                             "+CIPSSLCCONF:0,0,0,\"\"\r\nOK\r\n");
        return true;
    }
    if (next_wifi_starts_with(command, "AT+CIPSSLSIZE?"))
    {
        next_wifi_queue_text(wifi, "+CIPSSLSIZE:4096\r\nOK\r\n");
        return true;
    }
    if (next_wifi_starts_with(command, "AT+CIPSSLSIZE=") ||
        next_wifi_starts_with(command, "AT+CIPSSLCCONF=") ||
        next_wifi_starts_with(command, "AT+CIPSSLSNI=") ||
        next_wifi_starts_with(command, "AT+CIPSSLCALPN=") ||
        next_wifi_starts_with(command, "AT+CIPSSLCCN=") ||
        next_wifi_starts_with(command, "AT+CIPSSLCPSK=") ||
        next_wifi_starts_with(command, "AT+CIPSSLCERT=") ||
        next_wifi_starts_with(command, "AT+CIPSSLKEY=") ||
        next_wifi_starts_with(command, "AT+SAVETRANSLINK="))
    {
        next_wifi_queue_text(wifi, "OK\r\n");
        return true;
    }
    if (next_wifi_starts_with(command, "AT+CIPSSLCONN="))
    {
        next_wifi_queue_text(wifi, "ERROR\r\n");
        return true;
    }
    return false;
}

static void next_wifi_command(NextWifi *wifi, char *command)
{
    char *end;

    while (*command == ' ' || *command == '\t')
        ++command;
    end = command + strlen(command);
    while (end > command &&
           (end[-1] == ' ' || end[-1] == '\t' ||
            end[-1] == '\r' || end[-1] == '\n'))
    {
        --end;
    }
    *end = '\0';
    if (*command == '\0')
        return;

    if (next_wifi_handle_system_command(wifi, command) ||
        next_wifi_handle_wifi_command(wifi, command) ||
        next_wifi_handle_connection_command(wifi, command))
    {
        return;
    }
    next_wifi_queue_text(wifi, "ERROR\r\n");
}

static void next_wifi_process_tx_byte(NextWifi *wifi, uint8_t value)
{
    if (wifi->send_payload != NULL)
    {
        wifi->send_payload[wifi->send_received++] = value;
        if (wifi->send_received == wifi->send_length)
            next_wifi_finish_send(wifi);
        return;
    }
    if (wifi->transparent)
    {
        next_wifi_process_transparent(wifi, value);
        return;
    }
    if (wifi->line_pending)
    {
        if (value == '\n' && wifi->echo)
            next_wifi_queue_bytes(wifi, &value, 1);
        wifi->line_pending = false;
        wifi->line[wifi->line_length] = '\0';
        next_wifi_command(wifi, wifi->line);
        wifi->line_length = 0;
        if (value == '\n')
            return;
    }
    if (wifi->echo)
        next_wifi_queue_bytes(wifi, &value, 1);
    if (value == '\r')
    {
        wifi->line_pending = true;
        return;
    }
    if (value == '\n')
    {
        if (wifi->line_length != 0)
        {
            wifi->line[wifi->line_length] = '\0';
            next_wifi_command(wifi, wifi->line);
            wifi->line_length = 0;
        }
        return;
    }
    if (wifi->line_length + 1 < sizeof(wifi->line))
    {
        wifi->line[wifi->line_length++] = (char)value;
    }
    else
    {
        wifi->line_length = 0;
        next_wifi_queue_text(wifi, "ERROR\r\n");
    }
}

static void next_wifi_poll_connection(NextWifi *wifi, int id)
{
    NextWifiConnection *connection = &wifi->connections[id];
    uint8_t data[NEXT_WIFI_SOCKET_CHUNK];
    size_t free_space;
    size_t maximum;
    int received;

    if (!connection->active)
        return;

    if (wifi->server_enabled && connection->server &&
        wifi->server_timeout != 0 &&
        GetTickCount64() - connection->last_activity >
            (uint64_t)wifi->server_timeout * 1000)
    {
        next_wifi_close_connection(connection);
        next_wifi_connection_notice(wifi, id, "CLOSED");
        return;
    }

    if (wifi->transparent && id == 0)
    {
        free_space = next_wifi_rx_free(wifi);
        if (free_space == 0)
            return;
        maximum = free_space > sizeof(data) ? sizeof(data) : free_space;
        received = recv(connection->socket, (char *)data, (int)maximum, 0);
        if (received > 0)
        {
            connection->last_activity = GetTickCount64();
            next_wifi_queue_bytes(wifi, data, (size_t)received);
        }
        else if (received == 0 ||
                 (received == SOCKET_ERROR &&
                  WSAGetLastError() != WSAEWOULDBLOCK))
        {
            next_wifi_close_connection(connection);
            wifi->transparent = false;
            wifi->transparent_pluses = 0;
            next_wifi_queue_text(wifi, "\r\nCLOSED\r\n");
        }
        return;
    }

    if (wifi->recv_mode)
    {
        size_t room;
        if (connection->passive_length >= sizeof(connection->passive))
            return;
        room = sizeof(connection->passive) - connection->passive_length;
        if (room > sizeof(data))
            room = sizeof(data);
        received = recv(connection->socket, (char *)data, (int)room, 0);
        if (received > 0)
        {
            connection->last_activity = GetTickCount64();
            memcpy(connection->passive + connection->passive_length, data,
                   (size_t)received);
            connection->passive_length += (size_t)received;
        }
        else if (received == 0 ||
                 (received == SOCKET_ERROR &&
                  WSAGetLastError() != WSAEWOULDBLOCK))
        {
            next_wifi_close_connection(connection);
            next_wifi_connection_notice(wifi, id, "CLOSED");
        }
        return;
    }

    if (connection->passive_length != 0)
        next_wifi_deliver_passive(wifi, id);

    free_space = next_wifi_rx_free(wifi);
    if (free_space <= NEXT_WIFI_IPD_HEADROOM)
        return;
    maximum = free_space - NEXT_WIFI_IPD_HEADROOM;
    if (maximum > sizeof(data))
        maximum = sizeof(data);
    received = recv(connection->socket, (char *)data, (int)maximum, 0);
    if (received > 0)
    {
        connection->last_activity = GetTickCount64();
        next_wifi_inject_ipd(wifi, id, connection, data, (size_t)received);
    }
    else if (received == 0 ||
             (received == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK))
    {
        next_wifi_close_connection(connection);
        next_wifi_connection_notice(wifi, id, "CLOSED");
    }
}

static void next_wifi_poll_listener(NextWifi *wifi)
{
    for (;;)
    {
        struct sockaddr_storage address;
        int address_size = sizeof(address);
        int slot = -1;
        SOCKET accepted;
        unsigned server_count = 0;

        if (!wifi->server_enabled || wifi->listener == INVALID_SOCKET)
            return;
        for (unsigned i = 0; i < NEXT_WIFI_MAX_CONNECTIONS; ++i)
        {
            if (wifi->connections[i].active && wifi->connections[i].server)
                ++server_count;
        }
        for (unsigned i = 0; i < NEXT_WIFI_MAX_CONNECTIONS; ++i)
        {
            if (!wifi->connections[i].active)
            {
                slot = (int)i;
                break;
            }
        }
        if (slot < 0 || server_count >= wifi->server_max_connections)
            return;

        accepted = accept(wifi->listener, (struct sockaddr *)&address,
                          &address_size);
        if (accepted == INVALID_SOCKET)
            return;
        {
            u_long nonblocking = 1;
            ioctlsocket(accepted, FIONBIO, &nonblocking);
        }
        next_wifi_close_connection(&wifi->connections[slot]);
        wifi->connections[slot].socket = accepted;
        wifi->connections[slot].active = true;
        wifi->connections[slot].server = true;
        snprintf(wifi->connections[slot].type,
                 sizeof(wifi->connections[slot].type), "%s", "TCP");
        snprintf(wifi->connections[slot].host,
                 sizeof(wifi->connections[slot].host), "%s", "0.0.0.0");
        wifi->connections[slot].last_activity = GetTickCount64();
        next_wifi_update_peer(&wifi->connections[slot]);
        next_wifi_connection_notice(wifi, slot, "CONNECT");
    }
}

static DWORD WINAPI next_wifi_thread(void *parameter)
{
    NextWifi *wifi = (NextWifi *)parameter;
    next_wifi_worker_reset(wifi);
    for (;;)
    {
        uint8_t bytes[256];
        size_t count = 0;
        bool stop;
        bool enabled;
        bool reset;

        EnterCriticalSection(&wifi->lock);
        stop = wifi->stop_requested;
        enabled = wifi->enabled;
        reset = wifi->reset_requested;
        wifi->reset_requested = false;
        while (count < sizeof(bytes) && wifi->tx_count != 0)
        {
            bytes[count++] = wifi->tx[wifi->tx_read];
            wifi->tx_read = (wifi->tx_read + 1) % NEXT_WIFI_TX_CAPACITY;
            --wifi->tx_count;
        }
        LeaveCriticalSection(&wifi->lock);

        if (stop)
            break;
        if (reset || enabled != wifi->worker_enabled)
        {
            next_wifi_worker_reset(wifi);
            wifi->worker_enabled = enabled;
        }
        if (enabled)
        {
            for (size_t i = 0; i < count; ++i)
                next_wifi_process_tx_byte(wifi, bytes[i]);
            next_wifi_poll_listener(wifi);
            for (int i = 0; i < NEXT_WIFI_MAX_CONNECTIONS; ++i)
                next_wifi_poll_connection(wifi, i);
        }
        WaitForSingleObject(wifi->wake_event, count != 0 ? 0 : 5);
    }
    next_wifi_worker_reset(wifi);
    return 0;
}

NextWifi *next_wifi_create(void)
{
    WSADATA data;
    NextWifi *wifi = (NextWifi *)calloc(1, sizeof(*wifi));
    if (wifi == NULL)
        return NULL;
    InitializeCriticalSection(&wifi->lock);
    wifi->wake_event = CreateEventA(NULL, FALSE, FALSE, NULL);
    wifi->winsock_ready = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    wifi->listener = INVALID_SOCKET;
    for (unsigned i = 0; i < NEXT_WIFI_MAX_CONNECTIONS; ++i)
        wifi->connections[i].socket = INVALID_SOCKET;
    if (wifi->wake_event == NULL)
    {
        if (wifi->winsock_ready)
            WSACleanup();
        DeleteCriticalSection(&wifi->lock);
        free(wifi);
        return NULL;
    }
    wifi->thread = CreateThread(NULL, 0, next_wifi_thread, wifi, 0, NULL);
    if (wifi->thread == NULL)
    {
        CloseHandle(wifi->wake_event);
        if (wifi->winsock_ready)
            WSACleanup();
        DeleteCriticalSection(&wifi->lock);
        free(wifi);
        return NULL;
    }
    return wifi;
}

void next_wifi_destroy(NextWifi *wifi)
{
    if (wifi == NULL)
        return;
    EnterCriticalSection(&wifi->lock);
    wifi->stop_requested = true;
    LeaveCriticalSection(&wifi->lock);
    SetEvent(wifi->wake_event);
    WaitForSingleObject(wifi->thread, INFINITE);
    CloseHandle(wifi->thread);
    CloseHandle(wifi->wake_event);
    if (wifi->winsock_ready)
        WSACleanup();
    DeleteCriticalSection(&wifi->lock);
    free(wifi);
}

void next_wifi_reset(NextWifi *wifi)
{
    if (wifi == NULL)
        return;
    EnterCriticalSection(&wifi->lock);
    wifi->rx_read = 0;
    wifi->rx_write = 0;
    wifi->rx_count = 0;
    wifi->tx_read = 0;
    wifi->tx_write = 0;
    wifi->tx_count = 0;
    wifi->rx_overflow = false;
    wifi->uart_control = 0;
    wifi->uart_frame = 0x18;
    wifi->uart_prescaler = 0;
    wifi->reset_requested = true;
    LeaveCriticalSection(&wifi->lock);
    SetEvent(wifi->wake_event);
}

void next_wifi_set_enabled(NextWifi *wifi, bool enabled)
{
    if (wifi == NULL)
        return;
    EnterCriticalSection(&wifi->lock);
    wifi->enabled = enabled;
    wifi->rx_read = 0;
    wifi->rx_write = 0;
    wifi->rx_count = 0;
    wifi->tx_read = 0;
    wifi->tx_write = 0;
    wifi->tx_count = 0;
    wifi->rx_overflow = false;
    LeaveCriticalSection(&wifi->lock);
    SetEvent(wifi->wake_event);
}

bool next_wifi_is_enabled(NextWifi *wifi)
{
    bool enabled;
    if (wifi == NULL)
        return false;
    EnterCriticalSection(&wifi->lock);
    enabled = wifi->enabled;
    LeaveCriticalSection(&wifi->lock);
    return enabled;
}

uint8_t next_wifi_read_status(NextWifi *wifi)
{
    uint8_t status = 0x10;
    if (wifi == NULL)
        return 0;
    EnterCriticalSection(&wifi->lock);
    if (wifi->rx_count != 0)
        status |= 0x01;
    if (wifi->tx_count == NEXT_WIFI_TX_CAPACITY)
        status |= 0x02;
    if (wifi->rx_overflow)
        status |= 0x04;
    if (wifi->rx_count >= NEXT_WIFI_RX_CAPACITY * 3 / 4)
        status |= 0x08;
    if (wifi->tx_count != 0)
        status &= (uint8_t)~0x10;
    wifi->rx_overflow = false;
    LeaveCriticalSection(&wifi->lock);
    return status;
}

uint8_t next_wifi_read_rx(NextWifi *wifi)
{
    uint8_t value = 0;
    if (wifi == NULL)
        return 0;
    EnterCriticalSection(&wifi->lock);
    if (wifi->enabled && wifi->rx_count != 0)
    {
        value = wifi->rx[wifi->rx_read];
        wifi->rx_read = (wifi->rx_read + 1) % NEXT_WIFI_RX_CAPACITY;
        --wifi->rx_count;
    }
    LeaveCriticalSection(&wifi->lock);
    SetEvent(wifi->wake_event);
    return value;
}

void next_wifi_write_tx(NextWifi *wifi, uint8_t value)
{
    if (wifi == NULL)
        return;
    EnterCriticalSection(&wifi->lock);
    if (wifi->enabled && wifi->tx_count < NEXT_WIFI_TX_CAPACITY)
    {
        wifi->tx[wifi->tx_write] = value;
        wifi->tx_write = (wifi->tx_write + 1) % NEXT_WIFI_TX_CAPACITY;
        ++wifi->tx_count;
    }
    LeaveCriticalSection(&wifi->lock);
    SetEvent(wifi->wake_event);
}

uint8_t next_wifi_read_control(NextWifi *wifi)
{
    uint8_t value = 0;
    if (wifi == NULL)
        return 0;
    EnterCriticalSection(&wifi->lock);
    value = wifi->uart_control;
    LeaveCriticalSection(&wifi->lock);
    return value;
}

void next_wifi_write_control(NextWifi *wifi, uint8_t value)
{
    if (wifi == NULL)
        return;
    EnterCriticalSection(&wifi->lock);
    wifi->uart_control = value;
    if ((value & 0x10) != 0)
    {
        wifi->uart_prescaler =
            (wifi->uart_prescaler & 0x3FFFu) | ((uint32_t)(value & 0x07) << 14);
    }
    LeaveCriticalSection(&wifi->lock);
}

void next_wifi_write_prescaler(NextWifi *wifi, uint8_t value)
{
    if (wifi == NULL)
        return;
    EnterCriticalSection(&wifi->lock);
    if ((value & 0x80) != 0)
    {
        wifi->uart_prescaler =
            (wifi->uart_prescaler & 0x007Fu) | ((uint32_t)(value & 0x7F) << 7);
    }
    else
    {
        wifi->uart_prescaler =
            (wifi->uart_prescaler & 0x3F80u) | (value & 0x7F);
    }
    LeaveCriticalSection(&wifi->lock);
}

uint8_t next_wifi_read_frame(NextWifi *wifi)
{
    uint8_t value = 0x18;
    if (wifi == NULL)
        return value;
    EnterCriticalSection(&wifi->lock);
    value = wifi->uart_frame;
    LeaveCriticalSection(&wifi->lock);
    return value;
}

void next_wifi_write_frame(NextWifi *wifi, uint8_t value)
{
    if (wifi == NULL)
        return;
    EnterCriticalSection(&wifi->lock);
    wifi->uart_frame = value & 0x7F;
    if ((value & 0x80) != 0)
    {
        wifi->rx_read = 0;
        wifi->rx_write = 0;
        wifi->rx_count = 0;
        wifi->tx_read = 0;
        wifi->tx_write = 0;
        wifi->tx_count = 0;
        wifi->rx_overflow = false;
    }
    LeaveCriticalSection(&wifi->lock);
}
