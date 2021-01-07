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


//---------------------------------------------------------------------
// 初始化 Worker 结构
//---------------------------------------------------------------------
Worker *worker_new(uint8_t id, Config *config, uint16_t num_clients, uint64_t num_requests) {
	Worker *worker;
	uint16_t i;

	worker = W_MALLOC(Worker, 1);
	worker->id = id;
    // 每个 worker 线程拥有一个独立的 ioloop
	worker->loop = ev_loop_new(0);
    // 增加 ioloop 引用计数
	ev_ref(worker->loop);
	worker->config = config;
	worker->num_clients = num_clients;
    // req_todo 总共要发送的请求数
	worker->stats.req_todo = num_requests;
	worker->progress_interval = num_requests / 10;

	if (worker->progress_interval == 0)
		worker->progress_interval = 1;

	worker->clients = W_MALLOC(Client*, num_clients);

    // 创建 clients
	for (i = 0; i < num_clients; i++) {
		if (NULL == (worker->clients[i] = client_new(worker)))
			return NULL;
	}

	return worker;
}


//---------------------------------------------------------------------
// 释放 worker
//---------------------------------------------------------------------
void worker_free(Worker *worker) {
	uint16_t i;

    // 释放 clients
	for (i = 0; i < worker->num_clients; i++)
		client_free(worker->clients[i]);

	free(worker->clients);
	free(worker);
}


//---------------------------------------------------------------------
// worker 线程运行函数
//---------------------------------------------------------------------
void *worker_thread(void* arg) {
	uint16_t i;
	Worker *worker = (Worker*)arg;

	/* start all clients */
    // 启动 clients 状态机
	for (i = 0; i < worker->num_clients; i++) {
		if (worker->stats.req_started < worker->stats.req_todo)
			client_state_machine(worker->clients[i]);
	}

    // 执行 IO 事件循环
	ev_loop(worker->loop, 0);

	ev_loop_destroy(worker->loop);

	return NULL;
}
