#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "next_wifi.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    NEXT_WIFI_RX_CAPACITY = 512,
    NEXT_WIFI_TX_CAPACITY = 64,
    NEXT_WIFI_LINE_CAPACITY = 512,
    NEXT_WIFI_MAX_CONNECTIONS = 5,
    NEXT_WIFI_MAX_SEND = 65535,
    NEXT_WIFI_SOCKET_CHUNK = 192
};

typedef struct NextWifiConnection {
    SOCKET socket;
    bool active;
    bool udp;
    char type[4];
    char host[256];
    uint16_t remote_port;
    uint16_t local_port;
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
    char line[NEXT_WIFI_LINE_CAPACITY];
    size_t line_length;
    bool line_pending;
    uint8_t *send_payload;
    size_t send_length;
    size_t send_received;
    int send_connection;
    NextWifiConnection connections[NEXT_WIFI_MAX_CONNECTIONS];
};

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

static void next_wifi_worker_reset(NextWifi *wifi)
{
    next_wifi_close_all(wifi);
    free(wifi->send_payload);
    wifi->send_payload = NULL;
    wifi->send_length = 0;
    wifi->send_received = 0;
    wifi->send_connection = 0;
    wifi->line_length = 0;
    wifi->line_pending = false;
    wifi->echo = true;
    wifi->multiplex = false;
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

static void next_wifi_connection_notice(NextWifi *wifi, int id, const char *notice)
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
    {
        struct sockaddr_storage local_address;
        int local_size = sizeof(local_address);
        if (getsockname(connected_socket, (struct sockaddr *)&local_address,
                        &local_size) == 0)
        {
            if (local_address.ss_family == AF_INET)
            {
                wifi->connections[id].local_port =
                    ntohs(((struct sockaddr_in *)&local_address)->sin_port);
            }
            else if (local_address.ss_family == AF_INET6)
            {
                wifi->connections[id].local_port =
                    ntohs(((struct sockaddr_in6 *)&local_address)->sin6_port);
            }
        }
    }
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

static void next_wifi_report_status(NextWifi *wifi)
{
    char response[384];
    bool any_active = false;
    for (unsigned i = 0; i < NEXT_WIFI_MAX_CONNECTIONS; ++i)
    {
        any_active = any_active || wifi->connections[i].active;
    }
    snprintf(response, sizeof(response), "STATUS:%d\r\n",
             any_active ? 3 : 2);
    next_wifi_queue_text(wifi, response);
    for (unsigned i = 0; i < NEXT_WIFI_MAX_CONNECTIONS; ++i)
    {
        const NextWifiConnection *connection = &wifi->connections[i];
        if (!connection->active)
            continue;
        snprintf(response, sizeof(response),
                 "+CIPSTATUS:%u,\"%s\",\"%s\",%u,%u,0\r\n",
                 i, connection->type, connection->host,
                 connection->remote_port, connection->local_port);
        next_wifi_queue_text(wifi, response);
    }
    next_wifi_queue_text(wifi, "OK\r\n");
}

static void next_wifi_command(NextWifi *wifi, const char *command)
{
    if (strcmp(command, "AT") == 0)
    {
        next_wifi_queue_text(wifi, "OK\r\n");
    }
    else if (strcmp(command, "ATE0") == 0 || strcmp(command, "ATE1") == 0)
    {
        wifi->echo = command[3] == '1';
        next_wifi_queue_text(wifi, "OK\r\n");
    }
    else if (strcmp(command, "AT+RST") == 0)
    {
        next_wifi_worker_reset(wifi);
        next_wifi_queue_text(wifi, "OK\r\n\r\nready\r\n");
    }
    else if (strcmp(command, "AT+RESTORE") == 0)
    {
        next_wifi_worker_reset(wifi);
        next_wifi_queue_text(wifi, "OK\r\n\r\nready\r\n");
    }
    else if (strcmp(command, "AT+GMR") == 0)
    {
        next_wifi_queue_text(
            wifi,
            "AT version:1.7.4.0-emulated\r\nSDK version:host-winsock\r\nOK\r\n");
    }
    else if (strcmp(command, "AT+CWMODE?") == 0 ||
             strcmp(command, "AT+CWMODE_CUR?") == 0 ||
             strcmp(command, "AT+CWMODE_DEF?") == 0)
    {
        next_wifi_queue_text(wifi, "+CWMODE:1\r\nOK\r\n");
    }
    else if (next_wifi_starts_with(command, "AT+CWMODE=") ||
             next_wifi_starts_with(command, "AT+CWMODE_CUR=") ||
             next_wifi_starts_with(command, "AT+CWMODE_DEF="))
    {
        next_wifi_queue_text(wifi, "OK\r\n");
    }
    else if (strcmp(command, "AT+CWJAP?") == 0 ||
             strcmp(command, "AT+CWJAP_CUR?") == 0)
    {
        next_wifi_queue_text(
            wifi,
            "+CWJAP:\"HOST NETWORK\",\"00:00:00:00:00:00\",1,-30\r\nOK\r\n");
    }
    else if (next_wifi_starts_with(command, "AT+CWJAP=") ||
             next_wifi_starts_with(command, "AT+CWJAP_CUR=") ||
             next_wifi_starts_with(command, "AT+CWJAP_DEF="))
    {
        next_wifi_queue_text(wifi, "WIFI CONNECTED\r\nWIFI GOT IP\r\nOK\r\n");
    }
    else if (strcmp(command, "AT+CWQAP") == 0)
    {
        next_wifi_queue_text(wifi, "WIFI DISCONNECT\r\nOK\r\n");
    }
    else if (strcmp(command, "AT+CWLAP") == 0)
    {
        next_wifi_queue_text(
            wifi,
            "+CWLAP:(3,\"HOST NETWORK\",-30,\"00:00:00:00:00:00\",1)\r\nOK\r\n");
    }
    else if (strcmp(command, "AT+CIFSR") == 0)
    {
        next_wifi_queue_text(
            wifi,
            "+CIFSR:STAIP,\"127.0.0.1\"\r\n+CIFSR:STAMAC,\"00:00:00:00:00:00\"\r\nOK\r\n");
    }
    else if (strcmp(command, "AT+CIPMUX?") == 0)
    {
        next_wifi_queue_text(wifi, wifi->multiplex
            ? "+CIPMUX:1\r\nOK\r\n" : "+CIPMUX:0\r\nOK\r\n");
    }
    else if (strcmp(command, "AT+CIPMUX=0") == 0 ||
             strcmp(command, "AT+CIPMUX=1") == 0)
    {
        wifi->multiplex = command[strlen(command) - 1] == '1';
        next_wifi_queue_text(wifi, "OK\r\n");
    }
    else if (next_wifi_starts_with(command, "AT+CIPSTART="))
    {
        int id;
        unsigned port;
        char type[8];
        char host[256];
        if (next_wifi_parse_cipstart(command + strlen("AT+CIPSTART="),
                                     wifi->multiplex, &id, type, sizeof(type),
                                     host, sizeof(host), &port) &&
            next_wifi_open_connection(wifi, id, type, host, port))
        {
            next_wifi_connection_notice(wifi, id, "CONNECT");
            next_wifi_queue_text(wifi, "OK\r\n");
        }
        else
        {
            next_wifi_queue_text(wifi, "ERROR\r\n");
        }
    }
    else if (next_wifi_starts_with(command, "AT+CIPSEND="))
    {
        next_wifi_begin_send(wifi, command + strlen("AT+CIPSEND="));
    }
    else if (strcmp(command, "AT+CIPCLOSE") == 0 ||
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
            next_wifi_close_connection(&wifi->connections[id]);
            next_wifi_connection_notice(wifi, id, "CLOSED");
            next_wifi_queue_text(wifi, "OK\r\n");
        }
        else
        {
            next_wifi_queue_text(wifi, "ERROR\r\n");
        }
    }
    else if (strcmp(command, "AT+CIPSTATUS") == 0)
    {
        next_wifi_report_status(wifi);
    }
    else if (strcmp(command, "AT+CIPMODE=0") == 0 ||
             next_wifi_starts_with(command, "AT+CIPDINFO=") ||
             next_wifi_starts_with(command, "AT+CWAUTOCONN=") ||
             next_wifi_starts_with(command, "AT+UART_CUR=") ||
             next_wifi_starts_with(command, "AT+UART_DEF=") ||
             next_wifi_starts_with(command, "AT+SYSMSG=") ||
             next_wifi_starts_with(command, "AT+RFPOWER="))
    {
        next_wifi_queue_text(wifi, "OK\r\n");
    }
    else
    {
        next_wifi_queue_text(wifi, "ERROR\r\n");
    }
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
    char header[48];
    size_t free_space;
    size_t maximum;
    int received;

    if (!connection->active)
        return;
    free_space = next_wifi_rx_free(wifi);
    maximum = wifi->multiplex ? 18 : 16;
    if (free_space <= maximum)
        return;
    maximum = free_space - maximum;
    if (maximum > sizeof(data))
        maximum = sizeof(data);
    received = recv(connection->socket, (char *)data, (int)maximum, 0);
    if (received > 0)
    {
        if (wifi->multiplex)
            snprintf(header, sizeof(header), "+IPD,%d,%d:", id, received);
        else
            snprintf(header, sizeof(header), "+IPD,%d:", received);
        next_wifi_queue_text(wifi, header);
        next_wifi_queue_bytes(wifi, data, (size_t)received);
    }
    else if (received == 0 ||
             (received == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK))
    {
        next_wifi_close_connection(connection);
        next_wifi_connection_notice(wifi, id, "CLOSED");
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
