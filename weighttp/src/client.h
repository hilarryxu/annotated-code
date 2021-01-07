/*
 * weighttp - a lightweight and simple webserver benchmarking tool
 *
 * Author:
 *     Copyright (c) 2009-2011 Thomas Porzelt
 *
 * License:
 *     MIT, see COPYING file
 */


//---------------------------------------------------------------------
// 客户端
//---------------------------------------------------------------------
struct Client {
    // 状态机状态
	enum {
		CLIENT_START,
		CLIENT_CONNECTING,
		CLIENT_WRITING,
		CLIENT_READING,
		CLIENT_ERROR,
		CLIENT_END
	} state;

    // 解析响应状态
	enum {
		PARSER_START,
		PARSER_HEADER,
		PARSER_BODY
	} parser_state;

    // worker
	Worker *worker;
    // fd watcher
	ev_io sock_watcher;
	uint32_t buffer_offset;
	uint32_t parser_offset;
	uint32_t request_offset;
    // 处理起始结束时间戳
	ev_tstamp ts_start;
	ev_tstamp ts_end;
	uint8_t keepalive;
	uint8_t success;
	uint8_t status_success;
	uint8_t chunked;
	int64_t chunk_size;
	int64_t chunk_received;
	int64_t content_length;
	uint64_t bytes_received; /* including http header */
	uint16_t header_size;

    // 缓冲区 32KB
	char buffer[CLIENT_BUFFER_SIZE];
};

Client *client_new(Worker *worker);
void client_free(Client *client);
void client_state_machine(Client *client);
