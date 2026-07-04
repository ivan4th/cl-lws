/* Shared transport glue: per-connection tx queue (one lws_write per
 * WRITEABLE, partial-write handling), the slave listener vhost, the
 * master TCP client connect, and the serial/fd RTU transport. */

#include <libwebsockets.h>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#include "csmb-private.h"

/* ---- per-connection tx queue ---- */

int csmb_conn_send(csmb_conn *conn, const uint8_t *frame, size_t len)
{
    csmb_txbuf *b = malloc(sizeof(*b) + LWS_PRE + len);

    if (!b)
        return CSMB_ENOMEM;
    b->next = NULL;
    b->len = len;
    b->off = 0;
    memcpy(b->data + LWS_PRE, frame, len);
    if (conn->tx_tail)
        conn->tx_tail->next = b;
    else
        conn->tx_head = b;
    conn->tx_tail = b;
    if (conn->wsi)
        lws_callback_on_writable(conn->wsi);
    return CSMB_OK;
}

int csmb_conn_writable(csmb_conn *conn, struct lws *wsi)
{
    csmb_txbuf *b = conn->tx_head;
    uint8_t *payload;
    size_t remaining;
    int written;

    if (!b)
        return conn->close_requested ? -1 : 0;

    payload = b->data + LWS_PRE + b->off;
    remaining = b->len - b->off;
    written = lws_write(wsi, payload, remaining, LWS_WRITE_RAW);
    if (written < 0)
        return -1;
    b->off += (size_t)written;
    if (b->off >= b->len) {
        conn->tx_head = b->next;
        if (!conn->tx_head)
            conn->tx_tail = NULL;
        free(b);
    }
    if (conn->tx_head)
        lws_callback_on_writable(wsi);
    else if (conn->close_requested)
        return -1;
    return 0;
}

void csmb_conn_free(csmb_conn *conn)
{
    csmb_txbuf *b = conn->tx_head;

    while (b) {
        csmb_txbuf *bn = b->next;
        free(b);
        b = bn;
    }
    /* serial/fd conns own their descriptor (lws does not close raw files) */
    if (csmb_conn_is_serial(conn) && conn->fd >= 0)
        close(conn->fd);
    free(conn);
}

/* ---- slave listener vhost ----
 *
 * One explicit vhost per listener (cl-lws raw-listen style).  lws keeps
 * the vhost-name / listen_accept_* / iface string pointers without
 * copying, so they are heap-allocated with the listener's lifetime.  The
 * owning engine goes in the vhost user pointer so LWS_CALLBACK_RAW_ADOPT
 * can route the accepted socket. */

int csmb_transport_listen(csmb_listener *ln, struct lws_context *cx,
                          const struct lws_protocols *protocols,
                          const csmb_transport *tr, void *owner)
{
    static unsigned seq;
    struct lws_context_creation_info info;
    char namebuf[48];

    memset(ln, 0, sizeof(*ln));
    snprintf(namebuf, sizeof(namebuf), "csmb-slave-%u", ++seq);
    ln->vhost_name = strdup(namebuf);
    ln->role_str = strdup("raw-skt");
    ln->proto_str = strdup("cs-modbus");
    if (tr->host_or_device)
        ln->iface_str = strdup(tr->host_or_device);
    if (!ln->vhost_name || !ln->role_str || !ln->proto_str ||
        (tr->host_or_device && !ln->iface_str)) {
        csmb_transport_listen_close(ln);
        return CSMB_ENOMEM;
    }

    memset(&info, 0, sizeof(info));
    info.port = tr->port;
    info.iface = ln->iface_str;            /* NULL = any interface */
    info.protocols = protocols;
    info.options = LWS_SERVER_OPTION_ADOPT_APPLY_LISTEN_ACCEPT_CONFIG;
    info.listen_accept_role = ln->role_str;
    info.listen_accept_protocol = ln->proto_str;
    info.vhost_name = ln->vhost_name;
    info.user = owner;

    ln->vhost = lws_create_vhost(cx, &info);
    if (!ln->vhost) {
        csmb_transport_listen_close(ln);
        return CSMB_ETRANSPORT;
    }
    return CSMB_OK;
}

void csmb_transport_listen_close(csmb_listener *ln)
{
    if (ln->vhost) {
        lws_vhost_destroy(ln->vhost);
        ln->vhost = NULL;
    }
    free(ln->vhost_name);
    free(ln->role_str);
    free(ln->proto_str);
    free(ln->iface_str);
    ln->vhost_name = ln->role_str = ln->proto_str = ln->iface_str = NULL;
}

/* ---- master outgoing TCP connect ----
 *
 * A raw-skt client connection bound to the "cs-modbus" protocol on
 * CLIENT_VHOST; the csmb_conn is the wsi's opaque user data, so
 * RAW_CONNECTED / CLIENT_CONNECTION_ERROR / RAW_RX / RAW_CLOSE route
 * back to it. */

csmb_conn *csmb_transport_connect(struct lws_context *cx,
                                  struct lws_vhost *client_vhost,
                                  const csmb_transport *tr, void *owner)
{
    struct lws_client_connect_info info;
    csmb_conn *conn = calloc(1, sizeof(*conn));
    struct lws *wsi;

    if (!conn)
        return NULL;
    conn->role = CSMB_CONN_MASTER_TCP;
    conn->owner = owner;
    conn->fd = -1;
    csmb_mbap_parser_init(&conn->mbap);

    memset(&info, 0, sizeof(info));
    info.context = cx;
    info.vhost = client_vhost;
    info.address = tr->host_or_device;
    info.host = tr->host_or_device;
    info.port = tr->port;
    info.method = "RAW";
    info.local_protocol_name = "cs-modbus";
    info.opaque_user_data = conn;
    info.pwsi = &wsi;

    wsi = lws_client_connect_via_info(&info);
    if (!wsi) {
        free(conn);
        return NULL;
    }
    conn->wsi = wsi;
    lws_set_opaque_user_data(wsi, conn);
    return conn;
}

/* ---- serial / fd RTU transport ---- */

static speed_t baud_to_speed(int baud)
{
    switch (baud) {
    case 1200:   return B1200;
    case 2400:   return B2400;
    case 4800:   return B4800;
    case 9600:   return B9600;
    case 19200:  return B19200;
    case 38400:  return B38400;
    case 57600:  return B57600;
    case 115200: return B115200;
    case 230400: return B230400;
    default:     return B9600;
    }
}

int csmb_serial_open(const csmb_transport *tr)
{
    int fd, baud = tr->baud ? tr->baud : 9600;
    speed_t sp = baud_to_speed(baud);
    struct termios tio;

    if (!tr->host_or_device)
        return -1;
    fd = open(tr->host_or_device, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0)
        return -1;

    if (tcgetattr(fd, &tio) == 0) {
        cfmakeraw(&tio);
        /* a pty may reject baud changes; tolerate it (log + continue) */
        if (cfsetispeed(&tio, sp) != 0 || cfsetospeed(&tio, sp) != 0)
            lwsl_warn("csmb: cfsetspeed(%d) failed on %s, continuing\n",
                      baud, tr->host_or_device);
        tio.c_cflag &= ~(tcflag_t)CSIZE;
        tio.c_cflag |= (tr->data_bits == 7) ? CS7 : CS8;
        if (tr->parity == CSMB_PARITY_NONE) {
            tio.c_cflag &= ~(tcflag_t)PARENB;
        } else {
            tio.c_cflag |= PARENB;
            if (tr->parity == CSMB_PARITY_ODD)
                tio.c_cflag |= PARODD;
            else
                tio.c_cflag &= ~(tcflag_t)PARODD;
        }
        if (tr->stop_bits == 2)
            tio.c_cflag |= CSTOPB;
        else
            tio.c_cflag &= ~(tcflag_t)CSTOPB;
        tio.c_cflag |= CLOCAL | CREAD;
        tio.c_cc[VMIN] = 1;
        tio.c_cc[VTIME] = 0;
        if (tcsetattr(fd, TCSANOW, &tio) != 0)
            lwsl_warn("csmb: tcsetattr failed on %s, continuing\n",
                      tr->host_or_device);
        tcflush(fd, TCIOFLUSH);
    } else {
        lwsl_warn("csmb: tcgetattr failed on %s, continuing raw\n",
                  tr->host_or_device);
    }
    return fd;
}

uint32_t csmb_serial_t35_us(const csmb_transport *tr)
{
    int baud;
    uint64_t us;

    if (tr->t35_us)
        return tr->t35_us;
    baud = tr->baud ? tr->baud : 9600;
    /* 3.5 characters * ~11 bits = 38.5 bits; us = 38.5e6 / baud */
    us = (uint64_t)38500000 / (uint64_t)baud;
    if (us < 1750)
        us = 1750;
    return (uint32_t)us;
}

csmb_conn *csmb_transport_adopt_fd(struct lws_vhost *vhost, int fd, int role,
                                   void *owner, int slave_mode)
{
    lws_sock_file_fd_type u;
    struct lws *wsi;
    csmb_conn *conn = calloc(1, sizeof(*conn));

    if (!conn)
        return NULL;
    conn->role = role;
    conn->owner = owner;
    conn->fd = fd;
    csmb_rtu_parser_init(&conn->rtu, slave_mode);

    memset(&u, 0, sizeof(u));
    u.filefd = (lws_filefd_type)(long long)fd;
    wsi = lws_adopt_descriptor_vhost(vhost, LWS_ADOPT_RAW_FILE_DESC, u,
                                     "cs-modbus", NULL);
    if (!wsi) {
        free(conn);
        return NULL;
    }
    conn->wsi = wsi;
    lws_set_opaque_user_data(wsi, conn);
    return conn;
}

int csmb_transport_serial_vhost(csmb_listener *ln, struct lws_context *cx,
                                const struct lws_protocols *protocols,
                                void *owner)
{
    static unsigned seq;
    struct lws_context_creation_info info;
    char namebuf[48];

    memset(ln, 0, sizeof(*ln));
    snprintf(namebuf, sizeof(namebuf), "csmb-serial-%u", ++seq);
    ln->vhost_name = strdup(namebuf);
    ln->proto_str = strdup("cs-modbus");
    if (!ln->vhost_name || !ln->proto_str) {
        csmb_transport_listen_close(ln);
        return CSMB_ENOMEM;
    }

    memset(&info, 0, sizeof(info));
    info.port = CONTEXT_PORT_NO_LISTEN;
    info.protocols = protocols;
    info.vhost_name = ln->vhost_name;
    info.user = owner;

    ln->vhost = lws_create_vhost(cx, &info);
    if (!ln->vhost) {
        csmb_transport_listen_close(ln);
        return CSMB_ETRANSPORT;
    }
    return CSMB_OK;
}
