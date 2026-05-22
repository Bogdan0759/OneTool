#include <sprot/sprot.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

static ssize_t recv_full(int fd, void *buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        ssize_t r = recv(fd, (char *)buf + got, n - got, 0);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (r == 0) {
            errno = ECONNRESET;
            return -1;
        }
        got += (size_t)r;
    }
    return (ssize_t)got;
}

int sprot_send_message(int sock_fd, const sprot_header_t *header, const void *body, size_t body_len, int attach_fd) {
    struct iovec iov[2];
    struct msghdr msg;
    union {
        char buf[CMSG_SPACE(sizeof(int))];
        struct cmsghdr align;
    } cmsg_buf;
    sprot_header_t hdr;
    int iov_count = 1;

    if (header == NULL || sock_fd < 0) {
        errno = EINVAL;
        return -1;
    }
    if (body_len > SPROT_MAX_MESSAGE - sizeof(hdr)) {
        errno = EMSGSIZE;
        return -1;
    }

    hdr = *header;
    hdr.length = (uint32_t)(sizeof(hdr) + body_len);
    if (attach_fd >= 0) {
        hdr.flags |= SPROT_FLAG_HAS_FD;
    } else {
        hdr.flags &= ~SPROT_FLAG_HAS_FD;
    }

    iov[0].iov_base = &hdr;
    iov[0].iov_len = sizeof(hdr);
    if (body_len > 0) {
        iov[1].iov_base = (void *)body;
        iov[1].iov_len = body_len;
        iov_count = 2;
    }

    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = iov;
    msg.msg_iovlen = (size_t)iov_count;

    if (attach_fd >= 0) {
        memset(&cmsg_buf, 0, sizeof(cmsg_buf));
        msg.msg_control = cmsg_buf.buf;
        msg.msg_controllen = sizeof(cmsg_buf.buf);
        struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(cmsg), &attach_fd, sizeof(int));
    }

    for (;;) {
        ssize_t s = sendmsg(sock_fd, &msg, MSG_NOSIGNAL);
        if (s < 0 && errno == EINTR) continue;
        if (s < 0) return -1;
        if ((size_t)s == hdr.length) return 0;
        errno = EIO;
        return -1;
    }
}

int sprot_recv_message(int sock_fd, sprot_header_t *header_out, void *body_out, size_t body_cap, int *received_fd) {
    sprot_header_t hdr;
    struct msghdr msg;
    struct iovec iov;
    union {
        char buf[CMSG_SPACE(sizeof(int))];
        struct cmsghdr align;
    } cmsg_buf;

    if (sock_fd < 0 || header_out == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (received_fd != NULL) *received_fd = -1;

    iov.iov_base = &hdr;
    iov.iov_len = sizeof(hdr);
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    memset(&cmsg_buf, 0, sizeof(cmsg_buf));
    msg.msg_control = cmsg_buf.buf;
    msg.msg_controllen = sizeof(cmsg_buf.buf);

    for (;;) {
        ssize_t r = recvmsg(sock_fd, &msg, 0);
        if (r < 0 && errno == EINTR) continue;
        if (r < 0) return -1;
        if (r == 0) { errno = ECONNRESET; return -1; }
        if (r != (ssize_t)sizeof(hdr)) { errno = EBADMSG; return -1; }
        break;
    }

    if (hdr.length < sizeof(hdr) || hdr.length > SPROT_MAX_MESSAGE) {
        errno = EBADMSG;
        return -1;
    }
    size_t body_len = hdr.length - sizeof(hdr);
    if (body_len > body_cap && body_out != NULL) {
        errno = EOVERFLOW;
        return -1;
    }

    int fd_in_msg = -1;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&msg); c != NULL; c = CMSG_NXTHDR(&msg, c)) {
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
            memcpy(&fd_in_msg, CMSG_DATA(c), sizeof(int));
            break;
        }
    }

    if (body_len > 0 && body_out != NULL) {
        if (recv_full(sock_fd, body_out, body_len) < 0) {
            if (fd_in_msg >= 0) close(fd_in_msg);
            return -1;
        }
    } else if (body_len > 0) {
        char drop[256];
        size_t left = body_len;
        while (left > 0) {
            size_t want = left > sizeof(drop) ? sizeof(drop) : left;
            ssize_t r = recv(sock_fd, drop, want, 0);
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) {
                if (fd_in_msg >= 0) close(fd_in_msg);
                return -1;
            }
            left -= (size_t)r;
        }
    }

    *header_out = hdr;
    if (received_fd != NULL) {
        *received_fd = fd_in_msg;
    } else if (fd_in_msg >= 0) {
        close(fd_in_msg);
    }
    return 0;
}

const char *sprot_msg_type_name(uint16_t type) {
    switch (type) {
        case SPROT_REQ_HELLO:             return "REQ_HELLO";
        case SPROT_REQ_SURFACE_CREATE:    return "REQ_SURFACE_CREATE";
        case SPROT_REQ_SURFACE_ATTACH:    return "REQ_SURFACE_ATTACH";
        case SPROT_REQ_SURFACE_DAMAGE:    return "REQ_SURFACE_DAMAGE";
        case SPROT_REQ_SURFACE_COMMIT:    return "REQ_SURFACE_COMMIT";
        case SPROT_REQ_SURFACE_DESTROY:   return "REQ_SURFACE_DESTROY";
        case SPROT_REQ_SURFACE_FRAME:     return "REQ_SURFACE_FRAME";
        case SPROT_REQ_SURFACE_SET_TITLE: return "REQ_SURFACE_SET_TITLE";
        case SPROT_REQ_PING:              return "REQ_PING";
        case SPROT_EVT_WELCOME:           return "EVT_WELCOME";
        case SPROT_EVT_SURFACE_CREATED:   return "EVT_SURFACE_CREATED";
        case SPROT_EVT_SURFACE_CONFIGURE: return "EVT_SURFACE_CONFIGURE";
        case SPROT_EVT_SURFACE_CLOSE:     return "EVT_SURFACE_CLOSE";
        case SPROT_EVT_SURFACE_FRAME:     return "EVT_SURFACE_FRAME";
        case SPROT_EVT_POINTER_MOTION:    return "EVT_POINTER_MOTION";
        case SPROT_EVT_POINTER_BUTTON:    return "EVT_POINTER_BUTTON";
        case SPROT_EVT_POINTER_ENTER:     return "EVT_POINTER_ENTER";
        case SPROT_EVT_POINTER_LEAVE:     return "EVT_POINTER_LEAVE";
        case SPROT_EVT_KEY:               return "EVT_KEY";
        case SPROT_EVT_PONG:              return "EVT_PONG";
        case SPROT_EVT_ERROR:             return "EVT_ERROR";
        default:                          return "UNKNOWN";
    }
}
