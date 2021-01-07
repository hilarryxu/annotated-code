/*
 * weighttp - a lightweight and simple webserver benchmarking tool
 *
 * Author:
 *     Copyright (c) 2009-2011 Thomas Porzelt
 *
 * License:
 *     MIT, see COPYING file
 */

#include "weighttp.h"

static uint8_t client_parse(Client *client, int size);
static void client_io_cb(struct ev_loop *loop, ev_io *w, int revents);
static void client_set_events(Client *client, int events);

/*
static void client_add_events(Client *client, int events);
static void client_rem_events(Client *client, int events);

static void client_add_events(Client *client, int events) {
	struct ev_loop *loop = client->worker->loop;
	ev_io *watcher = &client->sock_watcher;

	if ((watcher->events & events) == events)
		return;

	ev_io_stop(loop, watcher);
	ev_io_set(watcher, watcher->fd, watcher->events | events);
	ev_io_start(loop, watcher);
}

static void client_rem_events(Client *client, int events) {
	struct ev_loop *loop = client->worker->loop;
	ev_io *watcher = &client->sock_watcher;

	if (0 == (watcher->events & events))
		return;

	ev_io_stop(loop, watcher);
	ev_io_set(watcher, watcher->fd, watcher->events & ~events);
	ev_io_start(loop, watcher);
}
*/


//---------------------------------------------------------------------
// 更新事件监听
//---------------------------------------------------------------------
static void client_set_events(Client *client, int events) {
	struct ev_loop *loop = client->worker->loop;
	ev_io *watcher = &client->sock_watcher;

    // 没有变化就跳过
	if (events == (watcher->events & (EV_READ | EV_WRITE)))
		return;

    // 先移除
	ev_io_stop(loop, watcher);
    // 设置新的 events 参数
	ev_io_set(watcher, watcher->fd, (watcher->events & ~(EV_READ | EV_WRITE)) | events);
    // 在添加
	ev_io_start(loop, watcher);
}


//---------------------------------------------------------------------
// 创建客户端
//---------------------------------------------------------------------
Client *client_new(Worker *worker) {
	Client *client;

	client = W_MALLOC(Client, 1);
    // 初始状态位 CLIENT_START
	client->state = CLIENT_START;
	client->worker = worker;
	client->sock_watcher.fd = -1;
	client->sock_watcher.data = client;
    // 响应 body 长度初始化为 -1
	client->content_length = -1;
	client->buffer_offset = 0;
	client->request_offset = 0;
    // 是否启用 KeepAlive
	client->keepalive = client->worker->config->keep_alive;
    // 是否为 chunked 编码方式
	client->chunked = 0;
	client->chunk_size = -1;
	client->chunk_received = 0;

	return client;
}


//---------------------------------------------------------------------
// 释放客户端
//---------------------------------------------------------------------
void client_free(Client *client) {
	if (client->sock_watcher.fd != -1) {
		ev_io_stop(client->worker->loop, &client->sock_watcher);
		shutdown(client->sock_watcher.fd, SHUT_WR);
		close(client->sock_watcher.fd);
	}

	free(client);
}


//---------------------------------------------------------------------
// 重置客户端
//
// 关闭旧的套接字
// 状态重置为 CLIENT_START
//---------------------------------------------------------------------
static void client_reset(Client *client) {
	//printf("keep alive: %d\n", client->keepalive);
    // 判断 keepalive
	if (!client->keepalive) {
		if (client->sock_watcher.fd != -1) {
            // 移除监听器并关闭套接字
			ev_io_stop(client->worker->loop, &client->sock_watcher);
			shutdown(client->sock_watcher.fd, SHUT_WR);
			close(client->sock_watcher.fd);
			client->sock_watcher.fd = -1;
		}

		client->state = CLIENT_START;
	} else {
        // 继续监听可写事件，发送新的请求
		client_set_events(client, EV_WRITE);
		client->state = CLIENT_WRITING;
		client->worker->stats.req_started++;
	}

    // 重置其他属性
	client->parser_state = PARSER_START;
	client->buffer_offset = 0;
	client->parser_offset = 0;
	client->request_offset = 0;
	client->ts_start = 0;
	client->ts_end = 0;
	client->status_success = 0;
	client->success = 0;
	client->content_length = -1;
	client->bytes_received = 0;
	client->header_size = 0;
	client->keepalive = client->worker->config->keep_alive;
	client->chunked = 0;
	client->chunk_size = -1;
	client->chunk_received = 0;
}


//---------------------------------------------------------------------
// 连接服务器
//---------------------------------------------------------------------
static uint8_t client_connect(Client *client) {
	//printf("connecting...\n");
	start:

	if (-1 == connect(client->sock_watcher.fd, client->worker->config->saddr->ai_addr, client->worker->config->saddr->ai_addrlen)) {
		switch (errno) {
			case EINPROGRESS:
			case EALREADY:
				/* async connect now in progress */
                // 正在连接 -> CLIENT_CONNECTING
				client->state = CLIENT_CONNECTING;
				return 1;
			case EISCONN:
                // 已连接
				break;
			case EINTR:
                // 继续尝试连接
				goto start;
			default:
			{
                // 连接失败
				strerror_r(errno, client->buffer, sizeof(client->buffer));
				W_ERROR("connect() failed: %s (%d)", client->buffer, errno);
				return 0;
			}
		}
	}

	/* successfully connected */
    // 成功连接 -> CLIENT_WRITING
	client->state = CLIENT_WRITING;
	return 1;
}


//---------------------------------------------------------------------
// 事件响应回调
//---------------------------------------------------------------------
static void client_io_cb(struct ev_loop *loop, ev_io *w, int revents) {
	Client *client = w->data;

	UNUSED(loop);
	UNUSED(revents);

    // 执行状态机
	client_state_machine(client);
}


//---------------------------------------------------------------------
// 状态机
//---------------------------------------------------------------------
void client_state_machine(Client *client) {
	int r;
	Config *config = client->worker->config;

	start:
	//printf("state: %d\n", client->state);
	switch (client->state) {
        // 初始状态，发起连接
		case CLIENT_START:
            // 已启动请求数加 1
			client->worker->stats.req_started++;

            // 新建一个套接字
			do {
				r = socket(config->saddr->ai_family, config->saddr->ai_socktype, config->saddr->ai_protocol);
			} while (-1 == r && errno == EINTR);

            // 创建套接字失败 -> CLIENT_ERROR
			if (-1 == r) {
				client->state = CLIENT_ERROR;
				strerror_r(errno, client->buffer, sizeof(client->buffer));
				W_ERROR("socket() failed: %s (%d)", client->buffer, errno);
				goto start;
			}

			/* set non-blocking */
            // 设置为非阻塞
			fcntl(r, F_SETFL, O_NONBLOCK | O_RDWR);

            // 初始化事件监听器，并添加到 worker 对应的 ioloop 上
			ev_init(&client->sock_watcher, client_io_cb);
            // 监听可写事件（准备连接服务器）
			ev_io_set(&client->sock_watcher, r, EV_WRITE);
			ev_io_start(client->worker->loop, &client->sock_watcher);

            // 开始连接服务器
			if (!client_connect(client)) {
                // 连接失败 -> CLIENT_ERROR
				client->state = CLIENT_ERROR;
				goto start;
			} else {
                // 监听可写事件（准备发送请求）
				client_set_events(client, EV_WRITE);
                // 连接成功，返回（等待可以发送请求）
				return;
			}
        // 重连
		case CLIENT_CONNECTING:
			if (!client_connect(client)) {
				client->state = CLIENT_ERROR;
				goto start;
			}
        // 发送请求
		case CLIENT_WRITING:
			while (1) {
                // 尝试发送剩余请求
				r = write(client->sock_watcher.fd, &config->request[client->request_offset], config->request_size - client->request_offset);
				//printf("write(%d - %d = %d): %d\n", config->request_size, client->request_offset, config->request_size - client->request_offset, r);
				if (r == -1) {
                    // 处理出错情况
					/* error */
                    // EINTR 继续 write
					if (errno == EINTR)
						continue;
                    // 其他错误码表示失败 -> CLIENT_ERROR
					strerror_r(errno, client->buffer, sizeof(client->buffer));
					W_ERROR("write() failed: %s (%d)", client->buffer, errno);
					client->state = CLIENT_ERROR;
					goto start;
				} else if (r != 0) {
					/* success */
                    // 成功发送一部分数据
                    // 调整 request_offset
					client->request_offset += r;
                    // 判断是否发送完了
					if (client->request_offset == config->request_size) {
						/* whole request was sent, start reading */
                        // 开始监听可读事件，准备读响应 -> CLIENT_READING
						client->state = CLIENT_READING;
						client_set_events(client, EV_READ);
					}

                    // 发送一部分数据就返回（等待下次事件到来）
					return;
				} else {
					/* disconnect */
                    // 服务端那边断开了连接 -> CLIENT_END
                    // 准备关闭连接
					client->state = CLIENT_END;
					goto start;
				}
			}
        // 读响应
		case CLIENT_READING:
			while (1) {
                // 读响应
                // buffer 结尾留了一个字节
				r = read(client->sock_watcher.fd, &client->buffer[client->buffer_offset], sizeof(client->buffer) - client->buffer_offset - 1);
				//printf("read(): %d, offset was: %d\n", r, client->buffer_offset);
				if (r == -1) {
                    // 处理出错情况
					/* error */
                    // EINTR 继续 read
					if (errno == EINTR)
						continue;
                    // 其他错误码表示失败 -> CLIENT_ERROR
					strerror_r(errno, client->buffer, sizeof(client->buffer));
					W_ERROR("read() failed: %s (%d)", client->buffer, errno);
					client->state = CLIENT_ERROR;
				} else if (r != 0) {
					/* success */
                    // 更新 bytes_received 收到的数据
					client->bytes_received += r;
                    // 调整 buffer_offset 偏移量
					client->buffer_offset += r;
                    // 更新 worker 中的统计
					client->worker->stats.bytes_total += r;

					if (client->buffer_offset >= sizeof(client->buffer)) {
                        // 响应包太大了 -> CLIENT_ERROR
						/* too big response header */
						client->state = CLIENT_ERROR;
						break;
					}
                    // 没读一点数据就将结尾处设置为 '\0'
					client->buffer[client->buffer_offset] = '\0';
					//printf("buffer:\n==========\n%s\n==========\n", client->buffer);
					if (!client_parse(client, r)) {
                        // 解析失败 -> CLIENT_ERROR
                        // 记录错误信息并准备关闭连接
						client->state = CLIENT_ERROR;
						//printf("parser failed\n");
						break;
					} else {
                        // 响应正常读出处理完了 -> CLIENT_END
						if (client->state == CLIENT_END)
							goto start;
						else
                            // 函数返回（等待下一次可读事件到来）
							return;
					}
				} else {
					/* disconnect */
                    // 服务端那边断开了连接
					if (client->parser_state == PARSER_BODY && !client->keepalive && client->status_success
						&& !client->chunked && client->content_length == -1) {
                        // body 数据正常读取完了
                        // -> CLIENT_END
						client->success = 1;
						client->state = CLIENT_END;
					} else {
                        // -> CLIENT_ERROR
						client->state = CLIENT_ERROR;
					}

					goto start;
				}
			}
            // 这里没有 break，会继续判断 state == CLIENT_ERROR

        // 出错了
		case CLIENT_ERROR:
			//printf("client error\n");
            // 请求失败计数器加 1
			client->worker->stats.req_error++;
			client->keepalive = 0;
            // 标记客户端状态为失败
			client->success = 0;
            // -> CLIENT_END
			client->state = CLIENT_END;
            // 这里没有 break 会继续处理 CLIENT_END
        // 准备结束本次连接
		case CLIENT_END:
			/* update worker stats */
            // 请求完成数加 1
			client->worker->stats.req_done++;

			if (client->success) {
                // 更新成功计数和读到的 body 字节数
				client->worker->stats.req_success++;
				client->worker->stats.bytes_body += client->bytes_received - client->header_size;
			} else {
                // 更新失败计数
				client->worker->stats.req_failed++;
			}

			/* print progress every 10% done */
            // 每完成 10% 打印一下当前进度
			if (client->worker->id == 1 && client->worker->stats.req_done % client->worker->progress_interval == 0) {
				printf("progress: %3d%% done\n",
					(int) (client->worker->stats.req_done * 100 / client->worker->stats.req_todo)
				);
			}

			if (client->worker->stats.req_started == client->worker->stats.req_todo) {
                // 全部请求已发送
				/* this worker has started all requests */
				client->keepalive = 0;
				client_reset(client);

				if (client->worker->stats.req_done == client->worker->stats.req_todo) {
					/* this worker has finished all requests */
                    // 全部请求已完成
                    // worker 对应的 ioloop 引用计数减 1
					ev_unref(client->worker->loop);
				}
			} else {
                // 重置客户端并继续开始发送请求
				client_reset(client);
				goto start;
			}
	}
}



//---------------------------------------------------------------------
// 解析响应
//
// size 本次读到的数据大小
//
// 返回 1 表示解析正常，可能还需继续读取更多数据
// 返回 0 表示失败
//---------------------------------------------------------------------
static uint8_t client_parse(Client *client, int size) {
	char *end, *str;
	uint16_t status_code;

    // HTTP/1.1 200 OK
    // Connection: keep-alive
    // Content-Length: 3
    // Content-Type: text/html; charset=UTF-8
    // Date: Thu, 07 Jan 2021 02:54:52 GMT
    // Etag: "671a3a6616f89e1656efeaef17173bb7aa82f87a"
    // Server: TornadoServer/6.0.4
    // 
    // ABC

	switch (client->parser_state) {
        // 开始解析
		case PARSER_START:
			//printf("parse (START):\n%s\n", &client->buffer[client->parser_offset]);
			/* look for HTTP/1.1 200 OK */
            // 读到的数据不够，等待继续读
			if (client->buffer_offset < sizeof("HTTP/1.1 200\r\n"))
				return 1;

            // 只支持 HTTP 1.1
			if (strncmp(client->buffer, "HTTP/1.1 ", sizeof("HTTP/1.1 ")-1) != 0)
				return 0;

			// now the status code
            // 解析 HTTP 状态码
			status_code = 0;
			str = client->buffer + sizeof("HTTP/1.1 ")-1;
			for (end = str + 3; str != end; str++) {
				if (*str < '0' || *str > '9')
					return 0;

				status_code *= 10;
				status_code += *str - '0';
			}

			// look for next \r\n
            // 查找 \r\n 且缓冲区大小不能超过 1024
			end = memchr(end, '\r', client->buffer_offset);
			if (!end || *(end+1) != '\n')
				return (!end || *(end+1) == '\0') && client->buffer_offset < 1024 ? 1 : 0;

            // 处理 HTTP 状态码
			if (status_code >= 200 && status_code < 300) {
				client->worker->stats.req_2xx++;
				client->status_success = 1;
			} else if (status_code < 400) {
				client->worker->stats.req_3xx++;
				client->status_success = 1;
			} else if (status_code < 500) {
				client->worker->stats.req_4xx++;
			} else if (status_code < 600) {
				client->worker->stats.req_5xx++;
			} else {
				// invalid status code
				return 0;
			}

			client->parser_offset = end + 2 - client->buffer;
            // -> PARSER_HEADER
            // 继续解析 headers
			client->parser_state = PARSER_HEADER;
        // 解析 headers
		case PARSER_HEADER:
			//printf("parse (HEADER)\n");
			/* look for Content-Length and Connection header */
            // 循环查找 \r\n
			while (NULL != (end = memchr(&client->buffer[client->parser_offset], '\r', client->buffer_offset - client->parser_offset))) {
				if (*(end+1) != '\n')
					return *(end+1) == '\0' && client->buffer_offset - client->parser_offset < 1024 ? 1 : 0;

                // end <-> parser_offset
                //     parser_offset
                //     end
                // \r\n\r\nBODY
				if (end == &client->buffer[client->parser_offset]) {
					/* body reached */
                    // -> PARSER_BODY
					client->parser_state = PARSER_BODY;
					client->header_size = end + 2 - client->buffer;
					//printf("body reached\n");

                    // FIXME(xcc): size - client->header_size < 0
					return client_parse(client, size - client->header_size);
				}

                // parser_offset                      end
                // Date: Thu, 07 Jan 2021 02:54:52 GMT\0\n ...
				*end = '\0';
				str = &client->buffer[client->parser_offset];
				//printf("checking header: '%s'\n", str);

				if (strncasecmp(str, "Content-Length: ", sizeof("Content-Length: ")-1) == 0) {
					/* content length header */
                    // 解析 content_length
					client->content_length = str_to_uint64(str + sizeof("Content-Length: ") - 1);
				} else if (strncasecmp(str, "Connection: ", sizeof("Connection: ")-1) == 0) {
					/* connection header */
                    // 解析 keepalive
					str += sizeof("Connection: ") - 1;

					if (strncasecmp(str, "close", sizeof("close")-1) == 0)
						client->keepalive = 0;
					else if (strncasecmp(str, "keep-alive", sizeof("keep-alive")-1) == 0)
						client->keepalive = client->worker->config->keep_alive;
					else
						return 0;
				} else if (strncasecmp(str, "Transfer-Encoding: ", sizeof("Transfer-Encoding: ")-1) == 0) {
					/* transfer encoding header */
                    // 解析 Transfer-Encoding
					str += sizeof("Transfer-Encoding: ") - 1;

					if (strncasecmp(str, "chunked", sizeof("chunked")-1) == 0)
						client->chunked = 1;
					else
                        // 返回失败（仅支持 chunked）
						return 0;
				}


                // end
                // \r\n\r\nBODY
                // FIXME(xcc): 会有访问越界问题吗？
				if (*(end+2) == '\r' && *(end+3) == '\n') {
					/* body reached */
                    // -> PARSER_BODY
					client->parser_state = PARSER_BODY;
                    // 头部长度 ...\r\n\r\n
					client->header_size = end + 4 - client->buffer;
                    // 调整 parser_offset 偏移量
                    //            parser_offset
                    // ...\r\n\r\nBODY
					client->parser_offset = client->header_size;
					//printf("body reached\n");

                    // 解析 body
                    // 第二个参数为扣除头部当前缓冲区剩余字节数
					return client_parse(client, size - client->header_size);
				}

                // 调整 parser_offset
                //                                        parser_offset
                // Date: Thu, 07 Jan 2021 02:54:52 GMT\r\n...
				client->parser_offset = end - client->buffer + 2;
			}

            // 等待读取更多数据
			return 1;
        // 解析 body
		case PARSER_BODY:
			//printf("parse (BODY)\n");
			/* do nothing, just consume the data */
			/*printf("content-l: %"PRIu64", header: %d, recevied: %"PRIu64"\n",
			client->content_length, client->header_size, client->bytes_received);*/

            // chunked 编码方式的数据读取
            // NOTE(xcc): 不关心暂时跳过
			if (client->chunked) {
				int consume_max;

				str = &client->buffer[client->parser_offset];
				/*printf("parsing chunk: '%s'\n(%"PRIi64" received, %"PRIi64" size, %d parser offset)\n",
					str, client->chunk_received, client->chunk_size, client->parser_offset
				);*/

				if (client->chunk_size == -1) {
					/* read chunk size */
					client->chunk_size = 0;
					client->chunk_received = 0;
					end = str + size;

					for (; str < end; str++) {
						if (*str == ';' || *str == '\r')
							break;

						client->chunk_size *= 16;
						if (*str >= '0' && *str <= '9')
							client->chunk_size += *str - '0';
						else if (*str >= 'A' && *str <= 'Z')
							client->chunk_size += 10 + *str - 'A';
						else if (*str >= 'a' && *str <= 'z')
							client->chunk_size += 10 + *str - 'a';
						else
							return 0; /*(src < end checked above)*/
					}

					if (str[0] != '\r') {
						str = memchr(str, '\r', end-str);
						if (!str) {
							client->chunk_size = -1;
							return size < 1024 ? 1 : 0;
						}
					}
					if (str[1] != '\n') {
						client->chunk_size = -1;
						return str+1 == end ? 1 : 0;
					}
					str += 2;

					//printf("---------- chunk size: %"PRIi64", %d read, %d offset, data: '%s'\n", client->chunk_size, size, client->parser_offset, str);

					size -= str - &client->buffer[client->parser_offset];
					client->parser_offset = str - client->buffer;

					if (client->chunk_size == 0) {
						/* chunk of size 0 marks end of content body */
						client->state = CLIENT_END;
						client->success = client->status_success ? 1 : 0;
						return 1;
					}
				}

				/* consume chunk till chunk_size is reached */
				consume_max = client->chunk_size - client->chunk_received;

				if (size < consume_max)
					consume_max = size;

				client->chunk_received += consume_max;
				client->parser_offset += consume_max;

				//printf("---------- chunk consuming: %d, received: %"PRIi64" of %"PRIi64", offset: %d\n", consume_max, client->chunk_received, client->chunk_size, client->parser_offset);

				if (client->chunk_received == client->chunk_size) {
					if (size - consume_max < 2)
						return 1;

					if (client->buffer[client->parser_offset] != '\r' || client->buffer[client->parser_offset+1] != '\n')
						return 0;

					/* got whole chunk, next! */
					//printf("---------- got whole chunk!!\n");
					client->chunk_size = -1;
					client->chunk_received = 0;
					client->parser_offset += 2;
					consume_max += 2;

					/* there is stuff left to parse */
					if (size - consume_max > 0)
						return client_parse(client, size - consume_max);
				}

				client->parser_offset = 0;
				client->buffer_offset = 0;

				return 1;
			} else {
                // 普通方式，带 content_length 的
				/* not chunked, just consume all data till content-length is reached */
				client->buffer_offset = 0;

				if (client->content_length == -1)
                    // 头部没读到 content_length，解析失败
					return 0;

				if (client->bytes_received == (uint64_t) (client->header_size + client->content_length)) {
                    // 判断是否读完了 body
					/* full response received */
                    // -> CLIENT_END
                    // 响应读取完毕准备关闭连接
					client->state = CLIENT_END;
					client->success = client->status_success ? 1 : 0;
				}
			}

            // 等待读取更多数据
			return 1;
	}

    // 解析正常
	return 1;
}
