/*
 * Copyright (c) 2009-2012, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "redis.h"
#include <sys/uio.h>
#include <math.h>

static void setProtocolError(redisClient *c, int pos);

/* To evaluate the output buffer size of a client we need to get size of
 * allocated objects, however we can't used zmalloc_size() directly on sds
 * strings because of the trick they use to work (the header is before the
 * returned pointer), so we use this helper function. */
size_t zmalloc_size_sds(sds s) {
    return zmalloc_size(s-sizeof(struct sdshdr));
}

void *dupClientReplyValue(void *o) {
    incrRefCount((robj*)o);
    return o;
}

int listMatchObjects(void *a, void *b) {
    return equalStringObjects(a,b);
}

redisClient *createClient(int fd) {
    redisClient *c = zmalloc(sizeof(redisClient));

    /* passing -1 as fd it is possible to create a non connected client.
     * This is useful since all the Redis commands needs to be executed
     * in the context of a client. When commands are executed in other
     * contexts (for instance a Lua script) we need a non connected client. */
    if (fd != -1) {
        anetNonBlock(NULL,fd);
        anetEnableTcpNoDelay(NULL,fd);
        if (server.tcpkeepalive)
            anetKeepAlive(NULL,fd,server.tcpkeepalive);

        // 为接收到的套接字注册监听事件
        // readQueryFromClient() 应该为处理客户端请求的函数
        if (aeCreateFileEvent(server.el,fd,AE_READABLE,
            readQueryFromClient, c) == AE_ERR)
        {
            close(fd);
            zfree(c);
            return NULL;
        }
    }

    // 为客户端挑选存储的数据库。注意，redis 内部会有多个数据集
    // 默认为0
    selectDb(c,0);

    c->fd = fd;
    c->name = NULL;
    c->bufpos = 0;
    c->querybuf = sdsempty();
    c->querybuf_peak = 0;
    c->reqtype = 0;
    c->argc = 0;
    c->argv = NULL;
    c->cmd = c->lastcmd = NULL;
    c->multibulklen = 0;
    c->bulklen = -1;
    c->sentlen = 0;
    c->flags = 0;
    c->ctime = c->lastinteraction = server.unixtime;
    c->authenticated = 0;
    c->replstate = REDIS_REPL_NONE;
    c->reploff = 0;
    c->repl_ack_off = 0;
    c->repl_ack_time = 0;
    c->slave_listening_port = 0;
    c->reply = listCreate();
    c->reply_bytes = 0;
    c->obuf_soft_limit_reached_time = 0;
    listSetFreeMethod(c->reply,decrRefCountVoid);
    listSetDupMethod(c->reply,dupClientReplyValue);
    c->bpop.keys = dictCreate(&setDictType,NULL);
    c->bpop.timeout = 0;
    c->bpop.target = NULL;
    c->watched_keys = listCreate();
    c->pubsub_channels = dictCreate(&setDictType,NULL);
    c->pubsub_patterns = listCreate();
    listSetFreeMethod(c->pubsub_patterns,decrRefCountVoid);
    listSetMatchMethod(c->pubsub_patterns,listMatchObjects);
    if (fd != -1) listAddNodeTail(server.clients,c);
    initClientMultiState(c);
    return c;
}

/* This function is called every time we are going to transmit new data
 * to the client. The behavior is the following:
 *
 * If the client should receive new data (normal clients will) the function
 * returns REDIS_OK, and make sure to install the write handler in our event
 * loop so that when the socket is writable new data gets written.
 *
 * If the client should not receive new data, because it is a fake client,
 * a master, a slave not yet online, or because the setup of the write handler
 * failed, the function returns REDIS_ERR.
 *
 * Typically gets called every time a reply is built, before adding more
 * data to the clients output buffers. If the function returns REDIS_ERR no
 * data should be appended to the output buffers. */
int prepareClientToWrite(redisClient *c) {
    // lua
    if (c->flags & REDIS_LUA_CLIENT) return REDIS_OK;
    // master节点且没有设置REDIS_MASTER_FORCE_REPLY
    if ((c->flags & REDIS_MASTER) &&
        !(c->flags & REDIS_MASTER_FORCE_REPLY)) return REDIS_ERR;

    // redisClient->fd <= 0 表示是虚拟的客户端，在 AOF 恢复数据的时候能用得到
    if (c->fd <= 0) return REDIS_ERR; /* Fake client */

    if (c->bufpos == 0 && listLength(c->reply) == 0 &&
        (c->replstate == REDIS_REPL_NONE ||
         c->replstate == REDIS_REPL_ONLINE) &&
         // TODO: epoll 使用的水平触发还是边缘触发
         // 这里注册写事件到epoll
        aeCreateFileEvent(server.el, c->fd, AE_WRITABLE,
        sendReplyToClient, c) == AE_ERR) return REDIS_ERR;
    return REDIS_OK;
}

/* Create a duplicate of the last object in the reply list when
 * it is not exclusively owned by the reply list. */
robj *dupLastObjectIfNeeded(list *reply) {
    robj *new, *cur;
    listNode *ln;
    redisAssert(listLength(reply) > 0);
    ln = listLast(reply);
    cur = listNodeValue(ln);
    if (cur->refcount > 1) {
        new = dupStringObject(cur);
        decrRefCount(cur);
        listNodeValue(ln) = new;
    }
    return listNodeValue(ln);
}

/* -----------------------------------------------------------------------------
 * Low level functions to add more data to output buffers.
 * -------------------------------------------------------------------------- */

int _addReplyToBuffer(redisClient *c, char *s, size_t len) {
    // 剩余可用的空间
    size_t available = sizeof(c->buf)-c->bufpos;

    // 已经被标记为尽快结束连接，则丢弃数据，尽快返回
    if (c->flags & REDIS_CLOSE_AFTER_REPLY) return REDIS_OK;

    // 如果在回复列表上还有数据，则不能往静态回复数据空间添加任何的数据
    /* If there already are entries in the reply list, we cannot
     * add anything more to the static buffer. */
    if (listLength(c->reply) > 0) return REDIS_ERR;

    // 数据空间可能不够，则返回错误
    /* Check that the buffer has enough space available for this string. */
    if (len > available) return REDIS_ERR;

    // 拷贝数据到静态回复数据空间
    memcpy(c->buf+c->bufpos,s,len);
    c->bufpos+=len;
    return REDIS_OK;
}

void _addReplyObjectToList(redisClient *c, robj *o) {
    robj *tail;

    if (c->flags & REDIS_CLOSE_AFTER_REPLY) return;

    if (listLength(c->reply) == 0) {
        incrRefCount(o);
        listAddNodeTail(c->reply,o);
        c->reply_bytes += zmalloc_size_sds(o->ptr);
    } else {
        tail = listNodeValue(listLast(c->reply));

        /* Append to this object when possible. */
        if (tail->ptr != NULL &&
            sdslen(tail->ptr)+sdslen(o->ptr) <= REDIS_REPLY_CHUNK_BYTES)
        {
            c->reply_bytes -= zmalloc_size_sds(tail->ptr);
            tail = dupLastObjectIfNeeded(c->reply);
            tail->ptr = sdscatlen(tail->ptr,o->ptr,sdslen(o->ptr));
            c->reply_bytes += zmalloc_size_sds(tail->ptr);
        } else {
            incrRefCount(o);
            listAddNodeTail(c->reply,o);
            c->reply_bytes += zmalloc_size_sds(o->ptr);
        }
    }
    asyncCloseClientOnOutputBufferLimitReached(c);
}

/* This method takes responsibility over the sds. When it is no longer
 * needed it will be free'd, otherwise it ends up in a robj. */
void _addReplySdsToList(redisClient *c, sds s) {
    robj *tail;

    if (c->flags & REDIS_CLOSE_AFTER_REPLY) {
        sdsfree(s);
        return;
    }

    if (listLength(c->reply) == 0) {
        listAddNodeTail(c->reply,createObject(REDIS_STRING,s));
        c->reply_bytes += zmalloc_size_sds(s);
    } else {
        tail = listNodeValue(listLast(c->reply));

        /* Append to this object when possible. */
        if (tail->ptr != NULL &&
            sdslen(tail->ptr)+sdslen(s) <= REDIS_REPLY_CHUNK_BYTES)
        {
            c->reply_bytes -= zmalloc_size_sds(tail->ptr);
            tail = dupLastObjectIfNeeded(c->reply);
            tail->ptr = sdscatlen(tail->ptr,s,sdslen(s));
            c->reply_bytes += zmalloc_size_sds(tail->ptr);
            sdsfree(s);
        } else {
            listAddNodeTail(c->reply,createObject(REDIS_STRING,s));
            c->reply_bytes += zmalloc_size_sds(s);
        }
    }
    asyncCloseClientOnOutputBufferLimitReached(c);
}

void _addReplyStringToList(redisClient *c, char *s, size_t len) {
    robj *tail;

    if (c->flags & REDIS_CLOSE_AFTER_REPLY) return;

    if (listLength(c->reply) == 0) {
        robj *o = createStringObject(s,len);

        listAddNodeTail(c->reply,o);
        c->reply_bytes += zmalloc_size_sds(o->ptr);
    } else {
        tail = listNodeValue(listLast(c->reply));

        /* Append to this object when possible. */
        if (tail->ptr != NULL &&
            sdslen(tail->ptr)+len <= REDIS_REPLY_CHUNK_BYTES)
        {
            c->reply_bytes -= zmalloc_size_sds(tail->ptr);
            tail = dupLastObjectIfNeeded(c->reply);
            tail->ptr = sdscatlen(tail->ptr,s,len);
            c->reply_bytes += zmalloc_size_sds(tail->ptr);
        } else {
            robj *o = createStringObject(s,len);

            listAddNodeTail(c->reply,o);
            c->reply_bytes += zmalloc_size_sds(o->ptr);
        }
    }
    asyncCloseClientOnOutputBufferLimitReached(c);
}

/* -----------------------------------------------------------------------------
 * Higher level functions to queue data on the client output buffer.
 * The following functions are the ones that commands implementations will call.
 * -------------------------------------------------------------------------- */

void addReply(redisClient *c, robj *obj) {
    // prepareClientToWrite() 向事件中心注册写事件，响应函数为 sendReplyToClient()
    // 待下面的操作结束后，会再一次进入事件中心，写事件将会被激活
    // prepareClientToWrite 返回REDIS_OK 表明当前replay需要向输出缓冲区中写数据
    // 返回 != REDIS_OK 表明不需要 比如加载AOF的伪客户端
    // 当为lua客户端时 直接返回REDIS_OK 且不需要注册写事件
    if (prepareClientToWrite(c) != REDIS_OK) return;

    /* This is an important place where we can avoid copy-on-write
     * when there is a saving child running, avoiding touching the
     * refcount field of the object if it's not needed.
     *
     * If the encoding is RAW and there is room in the static buffer
     * we'll be able to send the object to the client without
     * messing with its page. */

    // 只有静态回复数据空间为空的时候才可以向其中添加数据，
    // 否则只能将数据添加到回复数据列表（当 _addReplyToBuffer() 返回 REDIS_ERR 时）
    if (obj->encoding == REDIS_ENCODING_RAW) {
        if (_addReplyToBuffer(c,obj->ptr,sdslen(obj->ptr)) != REDIS_OK)
            _addReplyObjectToList(c,obj);
    } else if (obj->encoding == REDIS_ENCODING_INT) {
        /* Optimization: if there is room in the static buffer for 32 bytes
         * (more than the max chars a 64 bit integer can take as string) we
         * avoid decoding the object and go for the lower level approach. */
        if (listLength(c->reply) == 0 && (sizeof(c->buf) - c->bufpos) >= 32) {
            char buf[32];
            int len;

            len = ll2string(buf,sizeof(buf),(long)obj->ptr);
            if (_addReplyToBuffer(c,buf,len) == REDIS_OK)
                return;
            /* else... continue with the normal code path, but should never
             * happen actually since we verified there is room. */
        }
        obj = getDecodedObject(obj);
        if (_addReplyToBuffer(c,obj->ptr,sdslen(obj->ptr)) != REDIS_OK)
            _addReplyObjectToList(c,obj);
        decrRefCount(obj);
    } else {
        redisPanic("Wrong obj->encoding in addReply()");
    }
}

void addReplySds(redisClient *c, sds s) {
    // 注册写事件，准备回复客户端
    if (prepareClientToWrite(c) != REDIS_OK) {
        /* The caller expects the sds to be free'd. */
        sdsfree(s);
        return;
    }
    if (_addReplyToBuffer(c,s,sdslen(s)) == REDIS_OK) {
        sdsfree(s);
    } else {
        /* This method free's the sds when it is no longer needed. */
        _addReplySdsToList(c,s);
    }
}

void addReplyString(redisClient *c, char *s, size_t len) {
    if (prepareClientToWrite(c) != REDIS_OK) return;
    if (_addReplyToBuffer(c,s,len) != REDIS_OK)
        _addReplyStringToList(c,s,len);
}

void addReplyErrorLength(redisClient *c, char *s, size_t len) {
    addReplyString(c,"-ERR ",5);
    addReplyString(c,s,len);
    addReplyString(c,"\r\n",2);
}

void addReplyError(redisClient *c, char *err) {
    addReplyErrorLength(c,err,strlen(err));
}

void addReplyErrorFormat(redisClient *c, const char *fmt, ...) {
    size_t l, j;
    va_list ap;
    va_start(ap,fmt);
    sds s = sdscatvprintf(sdsempty(),fmt,ap);
    va_end(ap);
    /* Make sure there are no newlines in the string, otherwise invalid protocol
     * is emitted. */
    l = sdslen(s);
    for (j = 0; j < l; j++) {
        if (s[j] == '\r' || s[j] == '\n') s[j] = ' ';
    }
    addReplyErrorLength(c,s,sdslen(s));
    sdsfree(s);
}

void addReplyStatusLength(redisClient *c, char *s, size_t len) {
    addReplyString(c,"+",1);
    addReplyString(c,s,len);
    addReplyString(c,"\r\n",2);
}

void addReplyStatus(redisClient *c, char *status) {
    addReplyStatusLength(c,status,strlen(status));
}

void addReplyStatusFormat(redisClient *c, const char *fmt, ...) {
    va_list ap;
    va_start(ap,fmt);
    sds s = sdscatvprintf(sdsempty(),fmt,ap);
    va_end(ap);
    addReplyStatusLength(c,s,sdslen(s));
    sdsfree(s);
}

/* Adds an empty object to the reply list that will contain the multi bulk
 * length, which is not known when this function is called. */
void *addDeferredMultiBulkLength(redisClient *c) {
    /* Note that we install the write event here even if the object is not
     * ready to be sent, since we are sure that before returning to the
     * event loop setDeferredMultiBulkLength() will be called. */
    if (prepareClientToWrite(c) != REDIS_OK) return NULL;
    listAddNodeTail(c->reply,createObject(REDIS_STRING,NULL));
    return listLast(c->reply);
}

/* Populate the length object and try gluing it to the next chunk. */
void setDeferredMultiBulkLength(redisClient *c, void *node, long length) {
    listNode *ln = (listNode*)node;
    robj *len, *next;

    /* Abort when *node is NULL (see addDeferredMultiBulkLength). */
    if (node == NULL) return;

    len = listNodeValue(ln);
    len->ptr = sdscatprintf(sdsempty(),"*%ld\r\n",length);
    c->reply_bytes += zmalloc_size_sds(len->ptr);
    if (ln->next != NULL) {
        next = listNodeValue(ln->next);

        /* Only glue when the next node is non-NULL (an sds in this case) */
        if (next->ptr != NULL) {
            c->reply_bytes -= zmalloc_size_sds(len->ptr);
            c->reply_bytes -= zmalloc_size_sds(next->ptr);
            len->ptr = sdscatlen(len->ptr,next->ptr,sdslen(next->ptr));
            c->reply_bytes += zmalloc_size_sds(len->ptr);
            listDelNode(c->reply,ln->next);
        }
    }
    asyncCloseClientOnOutputBufferLimitReached(c);
}

/* Add a double as a bulk reply */
void addReplyDouble(redisClient *c, double d) {
    char dbuf[128], sbuf[128];
    int dlen, slen;
    if (isinf(d)) {
        /* Libc in odd systems (Hi Solaris!) will format infinite in a
         * different way, so better to handle it in an explicit way. */
        addReplyBulkCString(c, d > 0 ? "inf" : "-inf");
    } else {
        dlen = snprintf(dbuf,sizeof(dbuf),"%.17g",d);
        slen = snprintf(sbuf,sizeof(sbuf),"$%d\r\n%s\r\n",dlen,dbuf);
        addReplyString(c,sbuf,slen);
    }
}

/* Add a long long as integer reply or bulk len / multi bulk count.
 * Basically this is used to output <prefix><long long><crlf>. */
void addReplyLongLongWithPrefix(redisClient *c, long long ll, char prefix) {
    char buf[128];
    int len;

    // 发送头，尽量使用共享对象
    /* Things like $3\r\n or *2\r\n are emitted very often by the protocol
     * so we have a few shared objects to use if the integer is small
     * like it is most of the times. */
    if (prefix == '*' && ll < REDIS_SHARED_BULKHDR_LEN) {
        addReply(c,shared.mbulkhdr[ll]);
        return;
    } else if (prefix == '$' && ll < REDIS_SHARED_BULKHDR_LEN) {
        addReply(c,shared.bulkhdr[ll]);
        return;
    }

    buf[0] = prefix;
    len = ll2string(buf+1,sizeof(buf)-1,ll);
    buf[len+1] = '\r';
    buf[len+2] = '\n';
    addReplyString(c,buf,len+3);
}

void addReplyLongLong(redisClient *c, long long ll) {
    if (ll == 0)
        addReply(c,shared.czero);
    else if (ll == 1)
        addReply(c,shared.cone);
    else
        addReplyLongLongWithPrefix(c,ll,':');
}

void addReplyMultiBulkLen(redisClient *c, long length) {
    // 尽量使用共享对象
    if (length < REDIS_SHARED_BULKHDR_LEN)
        addReply(c,shared.mbulkhdr[length]);
    else
        addReplyLongLongWithPrefix(c,length,'*');
}

/* Create the length prefix of a bulk reply, example: $2234 */
void addReplyBulkLen(redisClient *c, robj *obj) {
    size_t len;

    if (obj->encoding == REDIS_ENCODING_RAW) {
        len = sdslen(obj->ptr);
    } else {
        long n = (long)obj->ptr;

        /* Compute how many bytes will take this integer as a radix 10 string */
        len = 1;
        if (n < 0) {
            len++;
            n = -n;
        }
        while((n = n/10) != 0) {
            len++;
        }
    }

    if (len < REDIS_SHARED_BULKHDR_LEN)
        addReply(c,shared.bulkhdr[len]);
    else
        addReplyLongLongWithPrefix(c,len,'$');
}

/* Add a Redis Object as a bulk reply */
void addReplyBulk(redisClient *c, robj *obj) {
    addReplyBulkLen(c,obj);
    addReply(c,obj);
    addReply(c,shared.crlf);
}

/* Add a C buffer as bulk reply */
void addReplyBulkCBuffer(redisClient *c, void *p, size_t len) {
    addReplyLongLongWithPrefix(c,len,'$');
    addReplyString(c,p,len);
    addReply(c,shared.crlf);
}

/* Add a C nul term string as bulk reply */
void addReplyBulkCString(redisClient *c, char *s) {
    if (s == NULL) {
        addReply(c,shared.nullbulk);
    } else {
        addReplyBulkCBuffer(c,s,strlen(s));
    }
}

/* Add a long long as a bulk reply */
void addReplyBulkLongLong(redisClient *c, long long ll) {
    char buf[64];
    int len;

    len = ll2string(buf,64,ll);
    addReplyBulkCBuffer(c,buf,len);
}

/* Copy 'src' client output buffers into 'dst' client output buffers.
 * The function takes care of freeing the old output buffers of the
 * destination client. */
void copyClientOutputBuffer(redisClient *dst, redisClient *src) {
    listRelease(dst->reply);
    dst->reply = listDup(src->reply);
    memcpy(dst->buf,src->buf,src->bufpos);
    dst->bufpos = src->bufpos;
    dst->reply_bytes = src->reply_bytes;
}

// acceptCommonHandler() 会由 acceptTcpHandler() 函数调用
// fd为新建立的客户端连接的fd
static void acceptCommonHandler(int fd, int flags) {
    // 之前已经接收了客户端的套接字，接下来创建与客户端对应的 redisClient 对象
    redisClient *c;

    // createClient() 的作用创建 redisClient 对象，为与客户端连接的套接字注册了事件
    if ((c = createClient(fd)) == NULL) {
        redisLog(REDIS_WARNING,
            "Error registering fd event for the new client: %s (fd=%d)",
            strerror(errno),fd);
        close(fd); /* May be already closed, just ignore errors */
        return;
    }

    // 如果超出了最大客户端限制，则需要关闭
    /* If maxclient directive is set and this is one client more... close the
     * connection. Note that we create the client instead to check before
     * for this condition, since now the socket is already set in non-blocking
     * mode and we can send an error for free using the Kernel I/O */
    if (listLength(server.clients) > server.maxclients) {
        char *err = "-ERR max number of clients reached\r\n";

        /* That's a best effort error message, don't check write errors */
        if (write(c->fd,err,strlen(err)) == -1) {
            /* Nothing to do, Just to avoid the warning... */
        }
        server.stat_rejected_conn++;
        freeClient(c);
        return;
    }
    server.stat_numconnections++;
    c->flags |= flags;
}

// 用于 TCP 接收请求的处理函数
// 有新的客户端连接
void acceptTcpHandler(aeEventLoop *el, int fd, void *privdata, int mask) {
    int cport, cfd;
    char cip[REDIS_IP_STR_LEN];
    REDIS_NOTUSED(el);
    REDIS_NOTUSED(mask);
    REDIS_NOTUSED(privdata);

    // 接收客户端请求
    cfd = anetTcpAccept(server.neterr, fd, cip, sizeof(cip), &cport);

    // 出错
    if (cfd == AE_ERR) {
        redisLog(REDIS_WARNING,"Accepting client connection: %s", server.neterr);
        return;
    }

    // 记录
    redisLog(REDIS_VERBOSE,"Accepted %s:%d", cip, cport);

    // 真正有意思的地方
    acceptCommonHandler(cfd,0);
}

void acceptUnixHandler(aeEventLoop *el, int fd, void *privdata, int mask) {
    int cfd;
    REDIS_NOTUSED(el);
    REDIS_NOTUSED(mask);
    REDIS_NOTUSED(privdata);

    cfd = anetUnixAccept(server.neterr, fd);
    if (cfd == AE_ERR) {
        redisLog(REDIS_WARNING,"Accepting client connection: %s", server.neterr);
        return;
    }
    redisLog(REDIS_VERBOSE,"Accepted connection to %s", server.unixsocket);
    acceptCommonHandler(cfd,REDIS_UNIX_SOCKET);
}


static void freeClientArgv(redisClient *c) {
    int j;
    for (j = 0; j < c->argc; j++)
        decrRefCount(c->argv[j]);
    c->argc = 0;
    c->cmd = NULL;
}

// 断开所有从机连接，在链式连接中用到
/* Close all the slaves connections. This is useful in chained replication
 * when we resync with our own master and want to force all our slaves to
 * resync with us as well. */
void disconnectSlaves(void) {
    while (listLength(server.slaves)) {
        listNode *ln = listFirst(server.slaves);
        freeClient((redisClient*)ln->value);
    }
}

/* This function is called when the slave lose the connection with the
 * master into an unexpected way. */
void replicationHandleMasterDisconnection(void) {
    server.master = NULL;
    server.repl_state = REDIS_REPL_CONNECT;
    server.repl_down_since = server.unixtime;

    /* We lost connection with our master, force our slaves to resync
     * with us as well to load the new data set.
     *
     * If server.masterhost is NULL the user called SLAVEOF NO ONE so
     * slave resync is not needed. */
     // SLAVEOF
     // 该命令用于修改SLAVE服务器的复制设置。如果一个Redis服务器已经处于SLAVE状态，SLAVEOF NO ONE命令将关闭当前服务器的被复制状态，与此同时将该服务器切换到MASTER状态。该命令的参数将指定MASTER服务器的监听IP和端口。还有一种情况是，当前服务器已经是另外一台MASTER的SLAVE了，在执行该命令后，当前服务器将终止和之前MASTER之间的复制关系，而将成为新MASTER的SLAVE，之前MASTER中的数据也将被清空，改为新MASTER中的数据。然而如果在当前SLAVE服务器上执行的是SLAVEOF NO ONE命令，那么该服务器只是中断与当前MASTER的复制关系，并升级为独立的MASTER，其中的数据也不会被清空。
    if (server.masterhost != NULL) disconnectSlaves();
}

void freeClient(redisClient *c) {
    listNode *ln;

    /* If this is marked as current client unset it */
    if (server.current_client == c) server.current_client = NULL;

    // 如果此机为从机，已经连接主机，可能需要保存主机状态信息，以便进行 PSYNC
    /* If it is our master that's beging disconnected we should make sure
     * to cache the state to try a partial resynchronization later.
     *
     * Note that before doing this we make sure that the client is not in
     * some unexpected state, by checking its flags. */
    if (server.master && c->flags & REDIS_MASTER) {
        redisLog(REDIS_WARNING,"Connection with master lost.");
        if (!(c->flags & (REDIS_CLOSE_AFTER_REPLY|
                          REDIS_CLOSE_ASAP|
                          REDIS_BLOCKED|
                          REDIS_UNBLOCKED)))
        {
            replicationCacheMaster(c);
            return;
        }
    }

    /* Log link disconnection with slave */
    if (c->flags & REDIS_SLAVE) {
        char ip[REDIS_IP_STR_LEN];

        if (anetPeerToString(c->fd,ip,sizeof(ip),NULL) != -1) {
            redisLog(REDIS_WARNING,"Connection with slave %s:%d lost.",
                ip, c->slave_listening_port);
        }
    }

    /* Free the query buffer */
    sdsfree(c->querybuf);
    c->querybuf = NULL;

    /* Deallocate structures used to block on blocking ops. */
    if (c->flags & REDIS_BLOCKED)
        unblockClientWaitingData(c);
    dictRelease(c->bpop.keys);

    /* UNWATCH all the keys */
    unwatchAllKeys(c);
    listRelease(c->watched_keys);

    // 取消所有的订阅
    /* Unsubscribe from all the pubsub channels */
    pubsubUnsubscribeAllChannels(c,0);
    pubsubUnsubscribeAllPatterns(c,0);
    dictRelease(c->pubsub_channels);
    listRelease(c->pubsub_patterns);

    // 关闭套接字，注销事件
    /* Close socket, unregister events, and remove list of replies and
     * accumulated arguments. */
    if (c->fd != -1) {
        aeDeleteFileEvent(server.el,c->fd,AE_READABLE);
        aeDeleteFileEvent(server.el,c->fd,AE_WRITABLE);
        close(c->fd);
    }
    listRelease(c->reply);
    freeClientArgv(c);

    /* Remove from the list of clients */
    if (c->fd != -1) {
        ln = listSearchKey(server.clients,c);
        redisAssert(ln != NULL);
        listDelNode(server.clients,ln);
    }

    /* When client was just unblocked because of a blocking operation,
     * remove it from the list of unblocked clients. */
    if (c->flags & REDIS_UNBLOCKED) {
        ln = listSearchKey(server.unblocked_clients,c);
        redisAssert(ln != NULL);
        listDelNode(server.unblocked_clients,ln);
    }

    // c 为从机，断开主从连接
    /* Master/slave cleanup Case 1:
     * we lost the connection with a slave. */
    if (c->flags & REDIS_SLAVE) {
        // 关闭 RDB 文件
        if (c->replstate == REDIS_REPL_SEND_BULK && c->repldbfd != -1)
            close(c->repldbfd);

        // 从监视/从机链表中删除
        list *l = (c->flags & REDIS_MONITOR) ? server.monitors : server.slaves;
        ln = listSearchKey(l,c);
        redisAssert(ln != NULL);
        listDelNode(l,ln);

        // redis 在一段时间没有从机连接的时候会释放积压空间
        /* We need to remember the time when we started to have zero
         * attached slaves, as after some time we'll free the replication
         * backlog. */
        if (c->flags & REDIS_SLAVE && listLength(server.slaves) == 0)
            server.repl_no_slaves_since = server.unixtime;
        refreshGoodSlavesCount();
    }

    // c 为主机，断开主从连接
    /* Master/slave cleanup Case 2:
     * we lost the connection with the master. */
    if (c->flags & REDIS_MASTER) replicationHandleMasterDisconnection();

    // 在待关闭链表中删除
    /* If this client was scheduled for async freeing we need to remove it
     * from the queue. */
    if (c->flags & REDIS_CLOSE_ASAP) {
        ln = listSearchKey(server.clients_to_close,c);
        redisAssert(ln != NULL);
        listDelNode(server.clients_to_close,ln);
    }

    /* Release other dynamically allocated client structure fields,
     * and finally release the client structure itself. */
    if (c->name) decrRefCount(c->name);
    zfree(c->argv);
    freeClientMultiState(c);
    zfree(c);
}

/* Schedule a client to free it at a safe time in the serverCron() function.
 * This function is useful when we need to terminate a client but we are in
 * a context where calling freeClient() is not possible, because the client
 * should be valid for the continuation of the flow of the program. */
void freeClientAsync(redisClient *c) {
    if (c->flags & REDIS_CLOSE_ASAP) return;
    c->flags |= REDIS_CLOSE_ASAP;
    // 添加到待关闭队列
    listAddNodeTail(server.clients_to_close,c);
}

void freeClientsInAsyncFreeQueue(void) {
    while (listLength(server.clients_to_close)) {
        listNode *ln = listFirst(server.clients_to_close);
        redisClient *c = listNodeValue(ln);

        c->flags &= ~REDIS_CLOSE_ASAP;
        freeClient(c);
        listDelNode(server.clients_to_close,ln);
    }
}

// 作为回调函数，将客户端的发送缓冲区中的数据发送给客户端
void sendReplyToClient(aeEventLoop *el, int fd, void *privdata, int mask) {
    redisClient *c = privdata;
    int nwritten = 0, totwritten = 0, objlen;
    size_t objmem;
    robj *o;
    REDIS_NOTUSED(el);
    REDIS_NOTUSED(mask);

    // 当前发送缓冲区中有数据要发送
    while(c->bufpos > 0 || listLength(c->reply)) {
        // 当前的client 固定发送缓冲区中有使用数据
        if (c->bufpos > 0) {
            // 将buf中的剩余c->bufpos- c->sentlen长的数据发送出去
            nwritten = write(fd,c->buf+c->sentlen,c->bufpos-c->sentlen);
            // TCP栈的发送缓冲区没有空间 或者发生了错误 直接跳出循环
            if (nwritten <= 0) break;
            c->sentlen += nwritten;
            // 当前已经发送的数据量
            totwritten += nwritten;

            /* If the buffer was sent, set bufpos to zero to continue with
             * the remainder of the reply. */
            // 如果数据已经全部发送完 则重置buf
            if (c->sentlen == c->bufpos) {
                c->bufpos = 0;
                c->sentlen = 0;
            }
        } else { //当前使用c->reply大发送缓冲区
            // 取下回复列表中的节点
            o = listNodeValue(listFirst(c->reply));
            objlen = sdslen(o->ptr);
            objmem = zmalloc_size_sds(o->ptr);

            // 数据为空，将此节点从回复列表中删除
            if (objlen == 0) {
                listDelNode(c->reply,listFirst(c->reply));
                continue;
            }

            nwritten = write(fd, ((char*)o->ptr)+c->sentlen,objlen-c->sentlen);
            if (nwritten <= 0) break;
            c->sentlen += nwritten;
            totwritten += nwritten;

            /* If we fully sent the object on head go to the next one */
            if (c->sentlen == objlen) {
                listDelNode(c->reply,listFirst(c->reply));
                c->sentlen = 0;
                c->reply_bytes -= objmem;
            }
        }
        /* Note that we avoid to send more than REDIS_MAX_WRITE_PER_EVENT
         * bytes, in a single threaded server it's a good idea to serve
         * other clients as well, even if a very large request comes from
         * super fast link that is always able to accept data (in real world
         * scenario think about 'KEYS *' against the loopback interface).
         *
         * However if we are over the maxmemory limit we ignore that and
         * just deliver as much data as it is possible to deliver. */
        // 当已经发送的数据很大时 > 64K 并且在没设置最大内存限制，或者尚未超过设置的最大内存限制的条件下，直接退出循环，停止发送
        // 因为一直发送大量数据会占用过长事件的时间 影响其他任务 虽然这里fd是非阻塞接口?
        if (totwritten > REDIS_MAX_WRITE_PER_EVENT &&
            (server.maxmemory == 0 ||
             zmalloc_used_memory() < server.maxmemory)) break;
    }

    // 错误处理
    if (nwritten == -1) {
        // tcp发送缓冲区没有空间
        if (errno == EAGAIN) {
            nwritten = 0;
        } else {
            redisLog(REDIS_VERBOSE,
                "Error writing to client: %s", strerror(errno));
            freeClient(c);
            return;
        }
    }

    if (totwritten > 0) {
        /* For clients representing masters we don't count sending data
         * as an interaction, since we always send REPLCONF ACK commands
         * that take some time to just fill the socket output buffer.
         * We just rely on data / pings received for timeout detection. */
        if (!(c->flags & REDIS_MASTER)) c->lastinteraction = server.unixtime;
    }

    // 如果回复数据已经全部发送，删除之前注册的可写事件
    if (c->bufpos == 0 && listLength(c->reply) == 0) {
        c->sentlen = 0;
        aeDeleteFileEvent(server.el,c->fd,AE_WRITABLE);

        // 如果设置了REDIS_CLOSE_AFTER_REPLY 则直接断开连接
        /* Close connection after entire reply has been sent. */
        if (c->flags & REDIS_CLOSE_AFTER_REPLY) freeClient(c);
    }
}

/* resetClient prepare the client to process the next command */
void resetClient(redisClient *c) {
    freeClientArgv(c);
    c->reqtype = 0;
    c->multibulklen = 0;
    c->bulklen = -1;
    /* We clear the ASKING flag as well if we are not inside a MULTI. */
    if (!(c->flags & REDIS_MULTI)) c->flags &= (~REDIS_ASKING);
}

// 解析单行命令
int processInlineBuffer(redisClient *c) {
    char *newline;
    int argc, j;
    sds *argv, aux;
    size_t querylen;

    /* Search for end of line */
    newline = strchr(c->querybuf,'\n');

    // 如果只是换行，不做任何处理
    /* Nothing to do without a \r\n */
    if (newline == NULL) {
        if (sdslen(c->querybuf) > REDIS_INLINE_MAX_SIZE) {
            addReplyError(c,"Protocol error: too big inline request");
            setProtocolError(c,0);
        }
        return REDIS_ERR;
    }

    /* Handle the \r\n case. */
    if (newline && newline != c->querybuf && *(newline-1) == '\r')
        newline--;

    /* Split the input buffer up to the \r\n */
    querylen = newline-(c->querybuf);
    aux = sdsnewlen(c->querybuf,querylen);
    argv = sdssplitargs(aux,&argc);
    sdsfree(aux);
    if (argv == NULL) {
        addReplyError(c,"Protocol error: unbalanced quotes in request");
        setProtocolError(c,0);
        return REDIS_ERR;
    }

    /* Newline from slaves can be used to refresh the last ACK time.
     * This is useful for a slave to ping back while loading a big
     * RDB file. */
    if (querylen == 0 && c->flags & REDIS_SLAVE)
        c->repl_ack_time = server.unixtime;

    /* Leave data after the first line of the query in the buffer */
    sdsrange(c->querybuf,querylen+2,-1);

    /* Setup argv array on client structure */
    if (c->argv) zfree(c->argv);
    c->argv = zmalloc(sizeof(robj*)*argc);

    // 为所有的参数建立 redis 对象
    /* Create redis objects for all arguments. */
    for (c->argc = 0, j = 0; j < argc; j++) {
        if (sdslen(argv[j])) {
            c->argv[c->argc] = createObject(REDIS_STRING,argv[j]);
            c->argc++;
        } else {
            sdsfree(argv[j]);
        }
    }
    zfree(argv);
    return REDIS_OK;
}

/* Helper function. Trims query buffer to make the function that processes
 * multi bulk requests idempotent. */
static void setProtocolError(redisClient *c, int pos) {
    if (server.verbosity >= REDIS_VERBOSE) {
        sds client = getClientInfoString(c);
        redisLog(REDIS_VERBOSE,
            "Protocol error from client: %s", client);
        sdsfree(client);
    }
    c->flags |= REDIS_CLOSE_AFTER_REPLY;
    sdsrange(c->querybuf,pos,-1);
}


// processMultibulkBuffer就是主要的解析buffer中的内容 将解析出的命令和参数放入对应argv中
// 对于set a 1这条命令，server端收到的内容应该是: *3\r\n$3\r\nset\r\n$1\r\na\r\n$1\r\n1\r\n：
int processMultibulkBuffer(redisClient *c) {
    char *newline = NULL;
    int pos = 0, ok;
    long long ll;

    if (c->multibulklen == 0) {
        /* The client should have been reset */
        redisAssertWithInfo(c,NULL,c->argc == 0);

        /* Multi bulk length cannot be read without a \r\n */
        newline = strchr(c->querybuf,'\r');
        if (newline == NULL) {
            if (sdslen(c->querybuf) > REDIS_INLINE_MAX_SIZE) {
                addReplyError(c,"Protocol error: too big mbulk count string");
                setProtocolError(c,0);
            }
            return REDIS_ERR;
        }

        /* Buffer should also contain \n */
        if (newline-(c->querybuf) > ((signed)sdslen(c->querybuf)-2))
            return REDIS_ERR;

        /* We know for sure there is a whole line since newline != NULL,
         * so go ahead and find out the multi bulk length. */
        redisAssertWithInfo(c,NULL,c->querybuf[0] == '*');
        ok = string2ll(c->querybuf+1,newline-(c->querybuf+1),&ll);
        if (!ok || ll > 1024*1024) {
            addReplyError(c,"Protocol error: invalid multibulk length");
            setProtocolError(c,pos);
            return REDIS_ERR;
        }

        pos = (newline-c->querybuf)+2;
        if (ll <= 0) {
            sdsrange(c->querybuf,pos,-1);
            return REDIS_OK;
        }

        c->multibulklen = ll;

        /* Setup argv array on client structure */
        if (c->argv) zfree(c->argv);
        c->argv = zmalloc(sizeof(robj*)*c->multibulklen);
    }

    redisAssertWithInfo(c,NULL,c->multibulklen > 0);
    while(c->multibulklen) {
        /* Read bulk length if unknown */
        if (c->bulklen == -1) {
            newline = strchr(c->querybuf+pos,'\r');
            if (newline == NULL) {
                if (sdslen(c->querybuf) > REDIS_INLINE_MAX_SIZE) {
                    addReplyError(c,"Protocol error: too big bulk count string");
                    setProtocolError(c,0);
                }
                break;
            }

            /* Buffer should also contain \n */
            if (newline-(c->querybuf) > ((signed)sdslen(c->querybuf)-2))
                break;

            if (c->querybuf[pos] != '$') {
                addReplyErrorFormat(c,
                    "Protocol error: expected '$', got '%c'",
                    c->querybuf[pos]);
                setProtocolError(c,pos);
                return REDIS_ERR;
            }

            ok = string2ll(c->querybuf+pos+1,newline-(c->querybuf+pos+1),&ll);
            if (!ok || ll < 0 || ll > 512*1024*1024) {
                addReplyError(c,"Protocol error: invalid bulk length");
                setProtocolError(c,pos);
                return REDIS_ERR;
            }

            pos += newline-(c->querybuf+pos)+2;
            if (ll >= REDIS_MBULK_BIG_ARG) {
                size_t qblen;

                /* If we are going to read a large object from network
                 * try to make it likely that it will start at c->querybuf
                 * boundary so that we can optimize object creation
                 * avoiding a large copy of data. */
                sdsrange(c->querybuf,pos,-1);
                pos = 0;
                qblen = sdslen(c->querybuf);
                /* Hint the sds library about the amount of bytes this string is
                 * going to contain. */
                if (qblen < ll+2)
                    c->querybuf = sdsMakeRoomFor(c->querybuf,ll+2-qblen);
            }
            c->bulklen = ll;
        }

        /* Read bulk argument */
        if (sdslen(c->querybuf)-pos < (unsigned)(c->bulklen+2)) {
            /* Not enough data (+2 == trailing \r\n) */
            break;
        } else {
            /* Optimization: if the buffer contains JUST our bulk element
             * instead of creating a new object by *copying* the sds we
             * just use the current sds string. */
            if (pos == 0 &&
                c->bulklen >= REDIS_MBULK_BIG_ARG &&
                (signed) sdslen(c->querybuf) == c->bulklen+2)
            {
                c->argv[c->argc++] = createObject(REDIS_STRING,c->querybuf);
                sdsIncrLen(c->querybuf,-2); /* remove CRLF */
                c->querybuf = sdsempty();
                /* Assume that if we saw a fat argument we'll see another one
                 * likely... */
                c->querybuf = sdsMakeRoomFor(c->querybuf,c->bulklen+2);
                pos = 0;
            } else {
                c->argv[c->argc++] =
                    createStringObject(c->querybuf+pos,c->bulklen);
                pos += c->bulklen+2;
            }
            c->bulklen = -1;
            c->multibulklen--;
        }
    }

    /* Trim to pos */
    if (pos) sdsrange(c->querybuf,pos,-1);

    /* We're done when c->multibulk == 0 */
    if (c->multibulklen == 0) return REDIS_OK;

    /* Still not read to process the command */
    return REDIS_ERR;
}

void processInputBuffer(redisClient *c) {
    /* Keep processing while there is something in the input buffer */
    while(sdslen(c->querybuf)) {
        // 已经被阻塞的客户端，在什么情况下会被阻塞？
        // 执行lua? blop? monitor?
        /* Immediately abort if the client is in the middle of something. */
        if (c->flags & REDIS_BLOCKED) return;

        /* REDIS_CLOSE_AFTER_REPLY closes the connection once the reply is
         * written to the client. Make sure to not let the reply grow after
         * this flag has been set (i.e. don't process more commands). */
        if (c->flags & REDIS_CLOSE_AFTER_REPLY) return;

        // 请求的类型，处理的过程不一样
        /* Determine request type when unknown. */
        if (!c->reqtype) {
            // 1）收到数据第一个字符为 *，表示接受到的数据多行请求
            if (c->querybuf[0] == '*') {
                c->reqtype = REDIS_REQ_MULTIBULK;

            // 2）单行请求
            } else {
                c->reqtype = REDIS_REQ_INLINE;
            }
        }

        // 解析单行请求
        if (c->reqtype == REDIS_REQ_INLINE) {
            if (processInlineBuffer(c) != REDIS_OK) break;
            // 解析多行请求
        } else if (c->reqtype == REDIS_REQ_MULTIBULK) {
            if (processMultibulkBuffer(c) != REDIS_OK) break;
        } else {
            // 未知
            redisPanic("Unknown request type");
        }

        /* Multibulk processing could see a <= 0 length. */
        if (c->argc == 0) {
            resetClient(c);
        } else {
            /* Only reset the client when the command was executed. */
            if (processCommand(c) == REDIS_OK)
                // 重置已经填充的某些 redisClient 选项，已准备响应客户端命令
                resetClient(c);
        }
    }
}

// 处理clinet发送的请求
void readQueryFromClient(aeEventLoop *el, int fd, void *privdata, int mask) {
    redisClient *c = (redisClient*) privdata;
    int nread, readlen;
    size_t qblen;
    REDIS_NOTUSED(el);
    REDIS_NOTUSED(mask);

    server.current_client = c;
    readlen = REDIS_IOBUF_LEN;
    /* If this is a multi bulk request, and we are processing a bulk reply
     * that is large enough, try to maximize the probability that the query
     * buffer contains exactly the SDS string representing the object, even
     * at the risk of requiring more read(2) calls. This way the function
     * processMultiBulkBuffer() can avoid copying buffers to create the
     * Redis Object representing the argument. */
    if (c->reqtype == REDIS_REQ_MULTIBULK && c->multibulklen && c->bulklen != -1
        && c->bulklen >= REDIS_MBULK_BIG_ARG)
    {
        int remaining = (unsigned)(c->bulklen+2)-sdslen(c->querybuf);

        if (remaining < readlen) readlen = remaining;
    }

    qblen = sdslen(c->querybuf);
    // 记录输出缓冲区的大小峰值
    if (c->querybuf_peak < qblen) c->querybuf_peak = qblen;
    // 为读准备空间
    c->querybuf = sdsMakeRoomFor(c->querybuf, readlen);

    // 将fd中的数据读到输入缓冲区的querybuf+qblen位置 最多读readlen
    nread = read(fd, c->querybuf+qblen, readlen);

    // 错误
    if (nread == -1) {
        if (errno == EAGAIN) {
            nread = 0;
        } else {
            redisLog(REDIS_VERBOSE, "Reading from client: %s",strerror(errno));
            freeClient(c);
            return;
        }

    // fd读到0 说明client tcp断开了
    } else if (nread == 0) {
        redisLog(REDIS_VERBOSE, "Client closed connection");
        freeClient(c);
        return;
    }

    // 从套接字读到有数据
    if (nread) {
        // 扩容sds为下一次读做准备
        sdsIncrLen(c->querybuf,nread);
        // 更新交互时间
        c->lastinteraction = server.unixtime;
        if (c->flags & REDIS_MASTER) c->reploff += nread;
    } else {
        server.current_client = NULL;
        return;
    }

    // 读到的数据大小超出了限定值
    if (sdslen(c->querybuf) > server.client_max_querybuf_len) {
        sds ci = getClientInfoString(c), bytes = sdsempty();

        bytes = sdscatrepr(bytes,c->querybuf,64);
        redisLog(REDIS_WARNING,"Closing client that reached max query buffer length: %s (qbuf initial bytes: %s)", ci, bytes);
        sdsfree(ci);
        sdsfree(bytes);

        // freeClient() 做了好多工作
        freeClient(c);
        return;
    }

    // 开始处理读到的数据
    processInputBuffer(c);
    server.current_client = NULL;
}

void getClientsMaxBuffers(unsigned long *longest_output_list,
                          unsigned long *biggest_input_buffer) {
    redisClient *c;
    listNode *ln;
    listIter li;
    unsigned long lol = 0, bib = 0;

    listRewind(server.clients,&li);
    while ((ln = listNext(&li)) != NULL) {
        c = listNodeValue(ln);

        if (listLength(c->reply) > lol) lol = listLength(c->reply);
        if (sdslen(c->querybuf) > bib) bib = sdslen(c->querybuf);
    }
    *longest_output_list = lol;
    *biggest_input_buffer = bib;
}

/* This is a helper function for getClientPeerId().
 * It writes the specified ip/port to "peerid" as a null termiated string
 * in the form ip:port if ip does not contain ":" itself, otherwise
 * [ip]:port format is used (for IPv6 addresses basically). */
void formatPeerId(char *peerid, size_t peerid_len, char *ip, int port) {
    if (strchr(ip,':'))
        snprintf(peerid,peerid_len,"[%s]:%d",ip,port);
    else
        snprintf(peerid,peerid_len,"%s:%d",ip,port);
}

/* A Redis "Peer ID" is a colon separated ip:port pair.
 * For IPv4 it's in the form x.y.z.k:pork, example: "127.0.0.1:1234".
 * For IPv6 addresses we use [] around the IP part, like in "[::1]:1234".
 * For Unix socekts we use path:0, like in "/tmp/redis:0".
 *
 * A Peer ID always fits inside a buffer of REDIS_PEER_ID_LEN bytes, including
 * the null term.
 *
 * The function returns REDIS_OK on succcess, and REDIS_ERR on failure.
 *
 * On failure the function still populates 'peerid' with the "?:0" string
 * in case you want to relax error checking or need to display something
 * anyway (see anetPeerToString implementation for more info). */
int getClientPeerId(redisClient *client, char *peerid, size_t peerid_len) {
    char ip[REDIS_IP_STR_LEN];
    int port;

    if (client->flags & REDIS_UNIX_SOCKET) {
        /* Unix socket client. */
        snprintf(peerid,peerid_len,"%s:0",server.unixsocket);
        return REDIS_OK;
    } else {
        /* TCP client. */
        int retval = anetPeerToString(client->fd,ip,sizeof(ip),&port);
        formatPeerId(peerid,peerid_len,ip,port);
        return (retval == -1) ? REDIS_ERR : REDIS_OK;
    }
}

/* Turn a Redis client into an sds string representing its state. */
sds getClientInfoString(redisClient *client) {
    char peerid[REDIS_PEER_ID_LEN], flags[16], events[3], *p;
    int emask;

    getClientPeerId(client,peerid,sizeof(peerid));
    p = flags;
    if (client->flags & REDIS_SLAVE) {
        if (client->flags & REDIS_MONITOR)
            *p++ = 'O';
        else
            *p++ = 'S';
    }
    if (client->flags & REDIS_MASTER) *p++ = 'M';
    if (client->flags & REDIS_MULTI) *p++ = 'x';
    if (client->flags & REDIS_BLOCKED) *p++ = 'b';
    if (client->flags & REDIS_DIRTY_CAS) *p++ = 'd';
    if (client->flags & REDIS_CLOSE_AFTER_REPLY) *p++ = 'c';
    if (client->flags & REDIS_UNBLOCKED) *p++ = 'u';
    if (client->flags & REDIS_CLOSE_ASAP) *p++ = 'A';
    if (client->flags & REDIS_UNIX_SOCKET) *p++ = 'U';
    if (p == flags) *p++ = 'N';
    *p++ = '\0';

    emask = client->fd == -1 ? 0 : aeGetFileEvents(server.el,client->fd);
    p = events;
    if (emask & AE_READABLE) *p++ = 'r';
    if (emask & AE_WRITABLE) *p++ = 'w';
    *p = '\0';
    return sdscatprintf(sdsempty(),
        "addr=%s fd=%d name=%s age=%ld idle=%ld flags=%s db=%d sub=%d psub=%d multi=%d qbuf=%lu qbuf-free=%lu obl=%lu oll=%lu omem=%lu events=%s cmd=%s",
        peerid,
        client->fd,
        client->name ? (char*)client->name->ptr : "",
        (long)(server.unixtime - client->ctime),
        (long)(server.unixtime - client->lastinteraction),
        flags,
        client->db->id,
        (int) dictSize(client->pubsub_channels),
        (int) listLength(client->pubsub_patterns),
        (client->flags & REDIS_MULTI) ? client->mstate.count : -1,
        (unsigned long) sdslen(client->querybuf),
        (unsigned long) sdsavail(client->querybuf),
        (unsigned long) client->bufpos,
        (unsigned long) listLength(client->reply),
        getClientOutputBufferMemoryUsage(client),
        events,
        client->lastcmd ? client->lastcmd->name : "NULL");
}

sds getAllClientsInfoString(void) {
    listNode *ln;
    listIter li;
    redisClient *client;
    sds o = sdsempty();

    listRewind(server.clients,&li);
    while ((ln = listNext(&li)) != NULL) {
        sds cs;

        client = listNodeValue(ln);
        cs = getClientInfoString(client);
        o = sdscatsds(o,cs);
        sdsfree(cs);
        o = sdscatlen(o,"\n",1);
    }
    return o;
}

void clientCommand(redisClient *c) {
    listNode *ln;
    listIter li;
    redisClient *client;

    if (!strcasecmp(c->argv[1]->ptr,"list") && c->argc == 2) {
        sds o = getAllClientsInfoString();
        addReplyBulkCBuffer(c,o,sdslen(o));
        sdsfree(o);
    } else if (!strcasecmp(c->argv[1]->ptr,"kill") && c->argc == 3) {
        listRewind(server.clients,&li);
        while ((ln = listNext(&li)) != NULL) {
            char peerid[REDIS_PEER_ID_LEN];

            client = listNodeValue(ln);
            if (getClientPeerId(client,peerid,sizeof(peerid)) == REDIS_ERR)
                continue;
            if (strcmp(peerid,c->argv[2]->ptr) == 0) {
                addReply(c,shared.ok);
                if (c == client) {
                    client->flags |= REDIS_CLOSE_AFTER_REPLY;
                } else {
                    freeClient(client);
                }
                return;
            }
        }
        addReplyError(c,"No such client");
    } else if (!strcasecmp(c->argv[1]->ptr,"setname") && c->argc == 3) {
        int j, len = sdslen(c->argv[2]->ptr);
        char *p = c->argv[2]->ptr;

        /* Setting the client name to an empty string actually removes
         * the current name. */
        if (len == 0) {
            if (c->name) decrRefCount(c->name);
            c->name = NULL;
            addReply(c,shared.ok);
            return;
        }

        /* Otherwise check if the charset is ok. We need to do this otherwise
         * CLIENT LIST format will break. You should always be able to
         * split by space to get the different fields. */
        for (j = 0; j < len; j++) {
            if (p[j] < '!' || p[j] > '~') { /* ASCII is assumed. */
                addReplyError(c,
                    "Client names cannot contain spaces, "
                    "newlines or special characters.");
                return;
            }
        }
        if (c->name) decrRefCount(c->name);
        c->name = c->argv[2];
        incrRefCount(c->name);
        addReply(c,shared.ok);
    } else if (!strcasecmp(c->argv[1]->ptr,"getname") && c->argc == 2) {
        if (c->name)
            addReplyBulk(c,c->name);
        else
            addReply(c,shared.nullbulk);
    } else {
        addReplyError(c, "Syntax error, try CLIENT (LIST | KILL ip:port | GETNAME | SETNAME connection-name)");
    }
}

/* Rewrite the command vector of the client. All the new objects ref count
 * is incremented. The old command vector is freed, and the old objects
 * ref count is decremented. */
void rewriteClientCommandVector(redisClient *c, int argc, ...) {
    va_list ap;
    int j;
    robj **argv; /* The new argument vector */

    argv = zmalloc(sizeof(robj*)*argc);
    va_start(ap,argc);
    for (j = 0; j < argc; j++) {
        robj *a;

        a = va_arg(ap, robj*);
        argv[j] = a;
        incrRefCount(a);
    }
    /* We free the objects in the original vector at the end, so we are
     * sure that if the same objects are reused in the new vector the
     * refcount gets incremented before it gets decremented. */
    for (j = 0; j < c->argc; j++) decrRefCount(c->argv[j]);
    zfree(c->argv);
    /* Replace argv and argc with our new versions. */
    c->argv = argv;
    c->argc = argc;
    c->cmd = lookupCommandOrOriginal(c->argv[0]->ptr);
    redisAssertWithInfo(c,NULL,c->cmd != NULL);
    va_end(ap);
}

/* Rewrite a single item in the command vector.
 * The new val ref count is incremented, and the old decremented. */
void rewriteClientCommandArgument(redisClient *c, int i, robj *newval) {
    robj *oldval;

    redisAssertWithInfo(c,NULL,i < c->argc);
    oldval = c->argv[i];
    c->argv[i] = newval;
    incrRefCount(newval);
    decrRefCount(oldval);

    /* If this is the command name make sure to fix c->cmd. */
    if (i == 0) {
        c->cmd = lookupCommandOrOriginal(c->argv[0]->ptr);
        redisAssertWithInfo(c,NULL,c->cmd != NULL);
    }
}

/* This function returns the number of bytes that Redis is virtually
 * using to store the reply still not read by the client.
 * It is "virtual" since the reply output list may contain objects that
 * are shared and are not really using additional memory.
 *
 * The function returns the total sum of the length of all the objects
 * stored in the output list, plus the memory used to allocate every
 * list node. The static reply buffer is not taken into account since it
 * is allocated anyway.
 *
 * Note: this function is very fast so can be called as many time as
 * the caller wishes. The main usage of this function currently is
 * enforcing the client output length limits. */
unsigned long getClientOutputBufferMemoryUsage(redisClient *c) {
    unsigned long list_item_size = sizeof(listNode)+sizeof(robj);

    return c->reply_bytes + (list_item_size*listLength(c->reply));
}

/* Get the class of a client, used in order to enforce limits to different
 * classes of clients.
 *
 * The function will return one of the following:
 * REDIS_CLIENT_LIMIT_CLASS_NORMAL -> Normal client
 * REDIS_CLIENT_LIMIT_CLASS_SLAVE  -> Slave or client executing MONITOR command
 * REDIS_CLIENT_LIMIT_CLASS_PUBSUB -> Client subscribed to Pub/Sub channels
 */
int getClientLimitClass(redisClient *c) {
    if (c->flags & REDIS_SLAVE) return REDIS_CLIENT_LIMIT_CLASS_SLAVE;
    if (dictSize(c->pubsub_channels) || listLength(c->pubsub_patterns))
        return REDIS_CLIENT_LIMIT_CLASS_PUBSUB;
    return REDIS_CLIENT_LIMIT_CLASS_NORMAL;
}

int getClientLimitClassByName(char *name) {
    if (!strcasecmp(name,"normal")) return REDIS_CLIENT_LIMIT_CLASS_NORMAL;
    else if (!strcasecmp(name,"slave")) return REDIS_CLIENT_LIMIT_CLASS_SLAVE;
    else if (!strcasecmp(name,"pubsub")) return REDIS_CLIENT_LIMIT_CLASS_PUBSUB;
    else return -1;
}

char *getClientLimitClassName(int class) {
    switch(class) {
    case REDIS_CLIENT_LIMIT_CLASS_NORMAL:   return "normal";
    case REDIS_CLIENT_LIMIT_CLASS_SLAVE:    return "slave";
    case REDIS_CLIENT_LIMIT_CLASS_PUBSUB:   return "pubsub";
    default:                                return NULL;
    }
}

/* The function checks if the client reached output buffer soft or hard
 * limit, and also update the state needed to check the soft limit as
 * a side effect.
 *
 * Return value: non-zero if the client reached the soft or the hard limit.
 *               Otherwise zero is returned. */
int checkClientOutputBufferLimits(redisClient *c) {
    int soft = 0, hard = 0, class;
    unsigned long used_mem = getClientOutputBufferMemoryUsage(c);

    class = getClientLimitClass(c);
    if (server.client_obuf_limits[class].hard_limit_bytes &&
        used_mem >= server.client_obuf_limits[class].hard_limit_bytes)
        hard = 1;
    if (server.client_obuf_limits[class].soft_limit_bytes &&
        used_mem >= server.client_obuf_limits[class].soft_limit_bytes)
        soft = 1;

    /* We need to check if the soft limit is reached continuously for the
     * specified amount of seconds. */
    if (soft) {
        if (c->obuf_soft_limit_reached_time == 0) {
            c->obuf_soft_limit_reached_time = server.unixtime;
            soft = 0; /* First time we see the soft limit reached */
        } else {
            time_t elapsed = server.unixtime - c->obuf_soft_limit_reached_time;

            if (elapsed <=
                server.client_obuf_limits[class].soft_limit_seconds) {
                soft = 0; /* The client still did not reached the max number of
                             seconds for the soft limit to be considered
                             reached. */
            }
        }
    } else {
        c->obuf_soft_limit_reached_time = 0;
    }
    return soft || hard;
}

/* Asynchronously close a client if soft or hard limit is reached on the
 * output buffer size. The caller can check if the client will be closed
 * checking if the client REDIS_CLOSE_ASAP flag is set.
 *
 * Note: we need to close the client asynchronously because this function is
 * called from contexts where the client can't be freed safely, i.e. from the
 * lower level functions pushing data inside the client output buffers. */
void asyncCloseClientOnOutputBufferLimitReached(redisClient *c) {
    redisAssert(c->reply_bytes < ULONG_MAX-(1024*64));
    if (c->reply_bytes == 0 || c->flags & REDIS_CLOSE_ASAP) return;
    if (checkClientOutputBufferLimits(c)) {
        sds client = getClientInfoString(c);

        freeClientAsync(c);
        redisLog(REDIS_WARNING,"Client %s scheduled to be closed ASAP for overcoming of output buffer limits.", client);
        sdsfree(client);
    }
}

// freeMemoryIfNeeded() 会调用此函数，将从机回复空间中数据发送给从机，
// 为的是不让数据延迟太久
/* Helper function used by freeMemoryIfNeeded() in order to flush slaves
 * output buffers without returning control to the event loop. */
void flushSlavesOutputBuffers(void) {
    listIter li;
    listNode *ln;

    listRewind(server.slaves,&li);
    while((ln = listNext(&li))) {
        redisClient *slave = listNodeValue(ln);
        int events;

        events = aeGetFileEvents(server.el,slave->fd);
        if (events & AE_WRITABLE &&
            slave->replstate == REDIS_REPL_ONLINE &&
            listLength(slave->reply))
        {
            sendReplyToClient(server.el,slave->fd,slave,0);
        }
    }
}

