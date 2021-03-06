#include <stdio.h>
#include <string.h>
#include <poll.h>
#include <errno.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include "pthreadpool_pipe.h"
#include "pthreadpool_tevent.h"

static int test_init(void)
{
	struct pthreadpool_pipe *p;
	int ret;

	ret = pthreadpool_pipe_init(1, &p);
	if (ret != 0) {
		fprintf(stderr, "pthreadpool_pipe_init failed: %s\n",
			strerror(ret));
		return -1;
	}
	ret = pthreadpool_pipe_destroy(p);
	if (ret != 0) {
		fprintf(stderr, "pthreadpool_pipe_destroy failed: %s\n",
			strerror(ret));
		return -1;
	}
	return 0;
}

static void test_sleep(void *ptr)
{
	int *ptimeout = (int *)ptr;
	int ret;
	ret = poll(NULL, 0, *ptimeout);
	if (ret != 0) {
		fprintf(stderr, "poll returned %d (%s)\n",
			ret, strerror(errno));
	}
}

static int test_jobs(int num_threads, int num_jobs)
{
	char *finished;
	struct pthreadpool_pipe *p;
	int timeout = 1;
	int i, ret;

	finished = (char *)calloc(1, num_jobs);
	if (finished == NULL) {
		fprintf(stderr, "calloc failed\n");
		return -1;
	}

	ret = pthreadpool_pipe_init(num_threads, &p);
	if (ret != 0) {
		fprintf(stderr, "pthreadpool_pipe_init failed: %s\n",
			strerror(ret));
		return -1;
	}

	for (i=0; i<num_jobs; i++) {
		ret = pthreadpool_pipe_add_job(p, i, test_sleep, &timeout);
		if (ret != 0) {
			fprintf(stderr, "pthreadpool_pipe_add_job failed: "
				"%s\n", strerror(ret));
			return -1;
		}
	}

	for (i=0; i<num_jobs; i++) {
		int jobid = -1;
		ret = pthreadpool_pipe_finished_jobs(p, &jobid, 1);
		if (ret < 0) {
			fprintf(stderr, "pthreadpool_pipe_finished_jobs "
				"failed: %s\n", strerror(-ret));
			return -1;
		}
		if ((ret != 1) || (jobid >= num_jobs)) {
			fprintf(stderr, "invalid job number %d\n", jobid);
			return -1;
		}
		finished[jobid] += 1;
	}

	for (i=0; i<num_jobs; i++) {
		if (finished[i] != 1) {
			fprintf(stderr, "finished[%d] = %d\n",
				i, finished[i]);
			return -1;
		}
	}

	ret = pthreadpool_pipe_destroy(p);
	if (ret != 0) {
		fprintf(stderr, "pthreadpool_pipe_destroy failed: %s\n",
			strerror(ret));
		return -1;
	}

	free(finished);
	return 0;
}

static int test_busydestroy(void)
{
	struct pthreadpool_pipe *p;
	int timeout = 50;
	struct pollfd pfd;
	int ret, jobid;

	ret = pthreadpool_pipe_init(1, &p);
	if (ret != 0) {
		fprintf(stderr, "pthreadpool_pipe_init failed: %s\n",
			strerror(ret));
		return -1;
	}
	ret = pthreadpool_pipe_add_job(p, 1, test_sleep, &timeout);
	if (ret != 0) {
		fprintf(stderr, "pthreadpool_pipe_add_job failed: %s\n",
			strerror(ret));
		return -1;
	}
	ret = pthreadpool_pipe_destroy(p);
	if (ret != EBUSY) {
		fprintf(stderr, "Could destroy a busy pool\n");
		return -1;
	}

	pfd.fd = pthreadpool_pipe_signal_fd(p);
	pfd.events = POLLIN|POLLERR;

	poll(&pfd, 1, -1);

	ret = pthreadpool_pipe_finished_jobs(p, &jobid, 1);
	if (ret < 0) {
		fprintf(stderr, "pthreadpool_pipe_finished_jobs failed: %s\n",
			strerror(-ret));
		return -1;
	}

	ret = pthreadpool_pipe_destroy(p);
	if (ret != 0) {
		fprintf(stderr, "pthreadpool_pipe_destroy failed: %s\n",
			strerror(ret));
		return -1;
	}
	return 0;
}

static int test_fork(void)
{
	struct pthreadpool_pipe *p;
	pid_t child, waited;
	int status, ret;

	ret = pthreadpool_pipe_init(1, &p);
	if (ret != 0) {
		fprintf(stderr, "pthreadpool_pipe_init failed: %s\n",
			strerror(ret));
		return -1;
	}
	ret = pthreadpool_pipe_destroy(p);
	if (ret != 0) {
		fprintf(stderr, "pthreadpool_pipe_destroy failed: %s\n",
			strerror(ret));
		return -1;
	}

	child = fork();
	if (child < 0) {
		perror("fork failed");
		return -1;
	}
	if (child == 0) {
		exit(0);
	}
	waited = wait(&status);
	if (waited == -1) {
		perror("wait failed");
		return -1;
	}
	if (waited != child) {
		fprintf(stderr, "expected child %d, got %d\n",
			(int)child, (int)waited);
		return -1;
	}
	return 0;
}

static void test_tevent_wait(void *private_data)
{
	int *timeout = private_data;
	poll(NULL, 0, *timeout);
}

static int test_tevent_1(void)
{
	struct tevent_context *ev;
	struct pthreadpool_tevent *pool;
	struct tevent_req *req1, *req2;
	int timeout10 = 10;
	int timeout100 = 100;
	int ret;
	bool ok;

	ev = tevent_context_init(NULL);
	if (ev == NULL) {
		ret = errno;
		fprintf(stderr, "tevent_context_init failed: %s\n",
			strerror(ret));
		return ret;
	}
	ret = pthreadpool_tevent_init(ev, 0, &pool);
	if (ret != 0) {
		fprintf(stderr, "pthreadpool_tevent_init failed: %s\n",
			strerror(ret));
		TALLOC_FREE(ev);
		return ret;
	}
	req1 = pthreadpool_tevent_job_send(
		ev, ev, pool, test_tevent_wait, &timeout10);
	if (req1 == NULL) {
		fprintf(stderr, "pthreadpool_tevent_job_send failed\n");
		TALLOC_FREE(ev);
		return ENOMEM;
	}
	req2 = pthreadpool_tevent_job_send(
		ev, ev, pool, test_tevent_wait, &timeout100);
	if (req2 == NULL) {
		fprintf(stderr, "pthreadpool_tevent_job_send failed\n");
		TALLOC_FREE(ev);
		return ENOMEM;
	}
	ok = tevent_req_poll(req2, ev);
	if (!ok) {
		ret = errno;
		fprintf(stderr, "tevent_req_poll failed: %s\n",
			strerror(ret));
		TALLOC_FREE(ev);
		return ret;
	}
	ret = pthreadpool_tevent_job_recv(req1);
	TALLOC_FREE(req1);
	if (ret != 0) {
		fprintf(stderr, "tevent_req_poll failed: %s\n",
			strerror(ret));
		TALLOC_FREE(ev);
		return ret;
	}

	TALLOC_FREE(req2);

	ret = tevent_loop_wait(ev);
	if (ret != 0) {
		fprintf(stderr, "tevent_loop_wait failed\n");
		return ret;
	}

	TALLOC_FREE(pool);
	TALLOC_FREE(ev);
	return 0;
}

int main(void)
{
	int ret;

	ret = test_tevent_1();
	if (ret != 0) {
		fprintf(stderr, "test_event_1 failed: %s\n",
			strerror(ret));
		return 1;
	}

	ret = test_init();
	if (ret != 0) {
		fprintf(stderr, "test_init failed\n");
		return 1;
	}

	ret = test_fork();
	if (ret != 0) {
		fprintf(stderr, "test_fork failed\n");
		return 1;
	}

	ret = test_jobs(10, 10000);
	if (ret != 0) {
		fprintf(stderr, "test_jobs failed\n");
		return 1;
	}

	ret = test_busydestroy();
	if (ret != 0) {
		fprintf(stderr, "test_busydestroy failed\n");
		return 1;
	}

	printf("success\n");
	return 0;
}
