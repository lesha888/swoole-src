/*
 +----------------------------------------------------------------------+
 | Swoole                                                               |
 +----------------------------------------------------------------------+
 | Copyright (c) 2012-2015 The Swoole Group                             |
 +----------------------------------------------------------------------+
 | This source file is subject to version 2.0 of the Apache license,    |
 | that is bundled with this package in the file LICENSE, and is        |
 | available through the world-wide-web at the following url:           |
 | http://www.apache.org/licenses/LICENSE-2.0.html                      |
 | If you did not receive a copy of the Apache2.0 license and are unable|
 | to obtain it through the world-wide-web, please send a note to       |
 | license@swoole.com so we can mail you a copy immediately.            |
 +----------------------------------------------------------------------+
 | Author: Tianfeng Han  <mikan.tenny@gmail.com>                        |
 +----------------------------------------------------------------------+
 */

#include "swoole.h"
#include "server.h"

#include <sys/stat.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL        0
#endif

int swConnection_onSendfile(swConnection *conn, swBuffer_chunk *chunk)
{
    int ret;
    swTask_sendfile *task = chunk->store.ptr;

#ifdef HAVE_TCP_NOPUSH
    if (task->offset == 0 && conn->tcp_nopush == 0)
    {
        /**
         * disable tcp_nodelay
         */
        if (conn->tcp_nodelay)
        {
            int tcp_nodelay = 0;
            if (setsockopt(conn->fd, IPPROTO_TCP, TCP_NODELAY, (const void *) &tcp_nodelay, sizeof(int)) == -1)
            {
                swWarn("setsockopt(TCP_NODELAY) failed. Error: %s[%d]", strerror(errno), errno);
            }
        }
        /**
         * enable tcp_nopush
         */
        if (swSocket_tcp_nopush(conn->fd, 1) == -1)
        {
            swWarn("swSocket_tcp_nopush() failed. Error: %s[%d]", strerror(errno), errno);
        }
        conn->tcp_nopush = 1;
    }
#endif

    int sendn = (task->length - task->offset > SW_SENDFILE_CHUNK_SIZE) ? SW_SENDFILE_CHUNK_SIZE : task->length - task->offset;

#ifdef SW_USE_OPENSSL
    if (conn->ssl)
    {
        ret = swSSL_sendfile(conn, task->fd, &task->offset, sendn);
    }
    else
#endif
    {
        ret = swoole_sendfile(conn->fd, task->fd, &task->offset, sendn);
    }

    swTrace("ret=%d|task->offset=%ld|sendn=%d|filesize=%ld", ret, (long)task->offset, sendn, task->length);

    if (ret <= 0)
    {
        switch (swConnection_error(errno))
        {
        case SW_ERROR:
            swSysError("sendfile(%s, %ld, %d) failed.", task->filename, (long)task->offset, sendn);
            swBuffer_pop_chunk(conn->out_buffer, chunk);
            return SW_OK;
        case SW_CLOSE:
            conn->close_wait = 1;
            return SW_ERR;
        case SW_WAIT:
            conn->send_wait = 1;
            return SW_ERR;
        default:
            break;
        }
    }

    //sendfile finish
    if (task->offset >= task->length)
    {
        swBuffer_pop_chunk(conn->out_buffer, chunk);

#ifdef HAVE_TCP_NOPUSH
        /**
         * disable tcp_nopush
         */
        if (swSocket_tcp_nopush(conn->fd, 0) == -1)
        {
            swWarn("swSocket_tcp_nopush() failed. Error: %s[%d]", strerror(errno), errno);
        }
        conn->tcp_nopush = 0;

        /**
         * enable tcp_nodelay
         */
        if (conn->tcp_nodelay)
        {
            int value = 1;
            if (setsockopt(conn->fd, IPPROTO_TCP, TCP_NODELAY, (const void *) &value, sizeof(int)) == -1)
            {
                swWarn("setsockopt(TCP_NODELAY) failed. Error: %s[%d]", strerror(errno), errno);
            }
        }
#endif
    }
    return SW_OK;
}

/**
 * send buffer to client
 */
int swConnection_buffer_send(swConnection *conn)
{
    int ret, sendn;

    swBuffer *buffer = conn->out_buffer;
    swBuffer_chunk *chunk = swBuffer_get_chunk(buffer);
    sendn = chunk->length - chunk->offset;

    if (sendn == 0)
    {
        swBuffer_pop_chunk(buffer, chunk);
        return SW_OK;
    }

    ret = swConnection_send(conn, (char*) chunk->store.ptr + chunk->offset, sendn, 0);
    if (ret < 0)
    {
        switch (swConnection_error(errno))
        {
        case SW_ERROR:
            swWarn("send to fd[%d] failed. Error: %s[%d]", conn->fd, strerror(errno), errno);
            break;
        case SW_CLOSE:
            conn->close_errno = errno;
            conn->close_wait = 1;
            return SW_ERR;
        case SW_WAIT:
            conn->send_wait = 1;
            return SW_ERR;
        default:
            break;
        }
        return SW_OK;
    }
    //chunk full send
    else if (ret == sendn || sendn == 0)
    {
        swBuffer_pop_chunk(buffer, chunk);
    }
    else
    {
        chunk->offset += ret;
    }
    return SW_OK;
}

swString* swConnection_get_string_buffer(swConnection *conn)
{
    swString *buffer = conn->object;
    if (buffer == NULL)
    {
        return swString_new(SW_BUFFER_SIZE);
    }
    else
    {
        return buffer;
    }
}

static char tmp_address[INET6_ADDRSTRLEN];

char* swConnection_get_ip(swConnection *conn)
{
    if (conn->socket_type == SW_SOCK_TCP || conn->socket_type == SW_SOCK_UDP)
    {
        return inet_ntoa(conn->info.addr.inet_v4.sin_addr);
    }
    else if (conn->socket_type == SW_SOCK_TCP6 || conn->socket_type == SW_SOCK_UDP6)
    {
        if (inet_ntop(AF_INET6, &conn->info.addr.inet_v6.sin6_addr, tmp_address, sizeof(tmp_address)))
        {
            return tmp_address;
        }
    }
#ifndef _WIN32
    else if (conn->socket_type == SW_SOCK_UNIX_STREAM || conn->socket_type == SW_SOCK_UNIX_DGRAM)
    {
        return conn->info.addr.un.sun_path;
    }
#endif
    return "unknown";
}

int swConnection_get_port(swConnection *conn)
{
    if (conn->socket_type == SW_SOCK_TCP)
    {
        return ntohs(conn->info.addr.inet_v4.sin_port);
    }
    else
    {
        return ntohs(conn->info.addr.inet_v6.sin6_port);
    }
}

void swConnection_sendfile_destructor(swBuffer_chunk *chunk)
{
    swTask_sendfile *task = chunk->store.ptr;
    close(task->fd);
    sw_free(task->filename);
    sw_free(task);
}

int swConnection_sendfile(swConnection *conn, char *filename, off_t offset, size_t length)
{
    if (conn->out_buffer == NULL)
    {
        conn->out_buffer = swBuffer_new(SW_BUFFER_SIZE);
        if (conn->out_buffer == NULL)
        {
            return SW_ERR;
        }
    }

    swBuffer_chunk error_chunk;
    swTask_sendfile *task = sw_malloc(sizeof(swTask_sendfile));
    if (task == NULL)
    {
        swWarn("malloc for swTask_sendfile failed.");
        return SW_ERR;
    }
    bzero(task, sizeof(swTask_sendfile));

    task->filename = sw_strdup(filename);
    int file_fd = open(filename, O_RDONLY);
    if (file_fd < 0)
    {
        sw_free(task->filename);
        sw_free(task);
        swSysError("open(%s) failed.", filename);
        return SW_OK;
    }
    task->fd = file_fd;
    task->offset = offset;

    struct stat file_stat;
    if (fstat(file_fd, &file_stat) < 0)
    {
        swSysError("fstat(%s) failed.", filename);
        error_chunk.store.ptr = task;
        swConnection_sendfile_destructor(&error_chunk);
        return SW_ERR;
    }
    if (offset < 0 || (length + offset > file_stat.st_size))
    {
        swoole_error_log(SW_LOG_WARNING, SW_ERROR_INVALID_PARAMS, "length or offset is invalid.");
        error_chunk.store.ptr = task;
        swConnection_sendfile_destructor(&error_chunk);
        return SW_OK;
    }
    if (length == 0)
    {
        task->length = file_stat.st_size;
    }
    else
    {
        task->length = length + offset;
    }

    swBuffer_chunk *chunk = swBuffer_new_chunk(conn->out_buffer, SW_CHUNK_SENDFILE, 0);
    if (chunk == NULL)
    {
        swWarn("get out_buffer chunk failed.");
        error_chunk.store.ptr = task;
        swConnection_sendfile_destructor(&error_chunk);
        return SW_ERR;
    }

    chunk->store.ptr = (void *) task;
    chunk->destroy = swConnection_sendfile_destructor;

    return SW_OK;
}


void swConnection_clear_string_buffer(swConnection *conn)
{
    swString *buffer = conn->object;
    if (buffer != NULL)
    {
        swString_free(buffer);
        conn->object = NULL;
    }
}

swBuffer_chunk* swConnection_get_in_buffer(swConnection *conn)
{
    swBuffer_chunk *chunk = NULL;
    swBuffer *buffer;

    if (conn->in_buffer == NULL)
    {
        buffer = swBuffer_new(SW_BUFFER_SIZE);
        //buffer create failed
        if (buffer == NULL)
        {
            return NULL;
        }
        //new chunk
        chunk = swBuffer_new_chunk(buffer, SW_CHUNK_DATA, buffer->chunk_size);
        if (chunk == NULL)
        {
            sw_free(buffer);
            return NULL;
        }
        conn->in_buffer = buffer;
    }
    else
    {
        buffer = conn->in_buffer;
        chunk = buffer->tail;
        if (chunk == NULL || chunk->length == buffer->chunk_size)
        {
            chunk = swBuffer_new_chunk(buffer, SW_CHUNK_DATA, buffer->chunk_size);
        }
    }
    return chunk;
}

swBuffer_chunk* swConnection_get_out_buffer(swConnection *conn, uint32_t type)
{
    swBuffer_chunk *chunk;
    if (conn->out_buffer == NULL)
    {
        conn->out_buffer = swBuffer_new(SW_BUFFER_SIZE);
        if (conn->out_buffer == NULL)
        {
            return NULL;
        }
    }
    if (type == SW_CHUNK_SENDFILE)
    {
        chunk = swBuffer_new_chunk(conn->out_buffer, SW_CHUNK_SENDFILE, 0);
    }
    else
    {
        chunk = swBuffer_get_chunk(conn->out_buffer);
        if (chunk == NULL)
        {
            chunk = swBuffer_new_chunk(conn->out_buffer, SW_CHUNK_DATA, conn->out_buffer->chunk_size);
        }
    }
    return chunk;
}
