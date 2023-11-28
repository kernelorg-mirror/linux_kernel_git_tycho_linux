// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE
#include <sys/socket.h>
#include <limits.h>
#include <string.h>
#include <signal.h>
#include <syscall.h>
#include <sched.h>
#include <poll.h>

#include "../kselftest.h"
#include "pidfd.h"

static int sys_waitid(int which, pid_t pid, siginfo_t *info, int options,
		      struct rusage *ru)
{
	return syscall(__NR_waitid, which, pid, info, options, ru);
}

// glibc defaults to 8MB stacks
#define STACK_SIZE (8 * 1024 * 1024)
static char stack[STACK_SIZE];

static int thread_sleep(void *)
{
	while (1)
		sleep(100);
	return 1;
}

static int fork_task_with_thread(int (*fn)(void *), int sk_pair[2],
				 pid_t *tgl, pid_t *thread, int *tgl_pidfd,
				 int *thread_pidfd)
{
	*tgl_pidfd = *thread_pidfd = -1;

	*tgl = fork();
	if (*tgl < 0) {
		perror("fork");
		return -1;
	}

	if (!*tgl) {
		int flags = CLONE_THREAD | CLONE_VM | CLONE_SIGHAND;
		pid_t t;

		t = clone(fn, stack + STACK_SIZE, flags, sk_pair);
		if (t < 0) {
			perror("clone");
			exit(1);
		}

		close(sk_pair[1]);

		if (write(sk_pair[0], &t, sizeof(t)) != sizeof(t)) {
			perror("read");
			exit(1);
		}

		// wait to get killed for various reasons by the tests.
		while (1)
			sleep(100);
	}

	errno = 0;
	if (read(sk_pair[1], thread, sizeof(*thread)) != sizeof(*thread)) {
		perror("read");
		goto cleanup;
	}

	*tgl_pidfd = sys_pidfd_open(*tgl, 0);
	if (*tgl_pidfd < 0) {
		perror("pidfd_open tgl");
		goto cleanup;
	}

	*thread_pidfd = sys_pidfd_open(*thread, 0);
	if (*thread_pidfd < 0) {
		perror("pidfd");
		goto cleanup;
	}

	return 0;

cleanup:
	kill(*tgl, SIGKILL);
	if (*tgl_pidfd >= 0)
		close(*tgl_pidfd);
	if (*thread_pidfd >= 0)
		close(*thread_pidfd);
	return -1;
}

static int test_non_tgl_basic(void)
{
	pid_t tgl, thread;
	int sk_pair[2], status;
	int tgl_pidfd = -1, thread_pidfd = -1;
	int ret = KSFT_FAIL;

	if (socketpair(PF_LOCAL, SOCK_SEQPACKET, 0, sk_pair) < 0) {
		ksft_print_msg("socketpair failed %s\n", strerror(errno));
		return KSFT_FAIL;
	}

	if (fork_task_with_thread(thread_sleep, sk_pair, &tgl, &thread,
				  &tgl_pidfd, &thread_pidfd) < 0) {
		return KSFT_FAIL;
	}

	/*
	 * KILL of a thread should still kill everyone
	 */
	if (sys_pidfd_send_signal(thread_pidfd, SIGKILL, NULL, 0)) {
		perror("pidfd_send_signal");
		goto cleanup;
	}

	errno = 0;
	if (waitpid(tgl, &status, 0) != tgl) {
		perror("waitpid tgl");
		goto cleanup;
	}

	if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGKILL) {
		ksft_print_msg("bad exit status %x\n", status);
		goto cleanup;
	}

	ret = KSFT_PASS;

cleanup:
	close(sk_pair[0]);
	close(sk_pair[1]);
	close(tgl_pidfd);
	close(thread_pidfd);
	return ret;
}

static int thread_exec(void *arg)
{
	int *sk_pair = arg;
	pid_t thread;

	if (read(sk_pair[0], &thread, sizeof(thread)) != sizeof(thread)) {
		perror("read");
		exit(1);
	}

	execlp("/bin/true", "/bin/true", NULL);
	return 1;
}

static int test_non_tgl_exec(void)
{
	pid_t tgl, thread;
	int sk_pair[2];
	int tgl_pidfd = -1, thread_pidfd = -1;
	int ret = KSFT_FAIL, ready;
	struct pollfd pollfd;

	if (socketpair(PF_LOCAL, SOCK_SEQPACKET, 0, sk_pair) < 0) {
		ksft_print_msg("socketpair failed %s\n", strerror(errno));
		return KSFT_FAIL;
	}

	if (fork_task_with_thread(thread_exec, sk_pair, &tgl, &thread,
				  &tgl_pidfd, &thread_pidfd) < 0) {
		return KSFT_FAIL;
	}

	if (write(sk_pair[1], &thread, sizeof(thread)) != sizeof(thread)) {
		perror("read");
		goto cleanup;
	}

	// thread will exec(), so this pidfd should eventually be dead (i.e.
	// poll should return).
	pollfd.fd = thread_pidfd;
	pollfd.events = POLLIN;

	ready = poll(&pollfd, 1, -1);
	if (ready == -1) {
		perror("poll");
		goto cleanup;
	}

	if (ready != 1) {
		ksft_print_msg("bad poll result %d\n", ready);
		goto cleanup;
	}

	if (pollfd.revents != POLLIN) {
		ksft_print_msg("bad poll revents: %x\n", pollfd.revents);
		goto cleanup;
	}

	errno = 0;
	if (sys_pidfd_getfd(thread_pidfd, 0, 0) > 0) {
		ksft_print_msg("got a real fd");
		goto cleanup;
	}

	if (errno != ESRCH) {
		ksft_print_msg("polling invalid thread didn't give ESRCH");
		goto cleanup;
	}

	ret = KSFT_PASS;

cleanup:
	close(sk_pair[0]);
	close(sk_pair[1]);
	close(tgl_pidfd);
	close(thread_pidfd);
	kill(tgl, SIGKILL);
	waitpid(tgl, NULL, 0);
	return ret;
}

int thread_wait_exit(void *arg)
{
	int *sk_pair = arg;
	pid_t thread;

	if (read(sk_pair[0], &thread, sizeof(thread)) != sizeof(thread)) {
		perror("read");
		exit(1);
	}

	return 0;
}

static int test_non_tgl_poll_exit(void)
{
	pid_t tgl, thread, writer;
	int sk_pair[2], ready;
	int tgl_pidfd = -1, thread_pidfd = -1;
	int ret = KSFT_FAIL;
	struct pollfd pollfd;

	if (socketpair(PF_LOCAL, SOCK_SEQPACKET, 0, sk_pair) < 0) {
		ksft_print_msg("socketpair failed %s\n", strerror(errno));
		return KSFT_FAIL;
	}

	if (fork_task_with_thread(thread_wait_exit, sk_pair, &tgl, &thread,
				  &tgl_pidfd, &thread_pidfd) < 0) {
		return KSFT_FAIL;
	}

	writer = fork();
	if (writer < 0) {
		perror("fork");
		return KSFT_FAIL;
	}

	if (writer == 0) {
		sleep(3);
		if (write(sk_pair[1], &thread, sizeof(thread)) != sizeof(thread)) {
			perror("read");
			exit(1);
		}
		exit(0);
	}

	// thread will exit, so this pidfd should eventually be dead (i.e.
	// poll should return).
	pollfd.fd = thread_pidfd;
	pollfd.events = POLLIN;

	ready = poll(&pollfd, 1, -1);
	if (ready == -1) {
		perror("poll");
		goto cleanup;
	}

	if (ready != 1) {
		ksft_print_msg("bad poll result %d\n", ready);
		goto cleanup;
	}

	if (pollfd.revents != POLLIN) {
		ksft_print_msg("bad poll revents: %x\n", pollfd.revents);
		goto cleanup;
	}

	if (pollfd.fd != thread_pidfd) {
		ksft_print_msg("bad poll fd: %x\n", pollfd.fd);
		goto cleanup;
	}

	ret = KSFT_PASS;

cleanup:
	close(sk_pair[0]);
	close(sk_pair[1]);
	close(tgl_pidfd);
	close(thread_pidfd);
	kill(tgl, SIGKILL);
	waitpid(tgl, NULL, 0);
	waitpid(thread, NULL, 0);
	return ret;
}


static int test_non_tgl_exit_poll(void)
{
	pid_t tgl, thread;
	int sk_pair[2];
	int tgl_pidfd = -1, thread_pidfd = -1;
	int ret = KSFT_FAIL, ready;
	struct pollfd pollfd;
	siginfo_t siginfo;

	if (socketpair(PF_LOCAL, SOCK_SEQPACKET, 0, sk_pair) < 0) {
		ksft_print_msg("socketpair failed %s\n", strerror(errno));
		return KSFT_FAIL;
	}

	if (fork_task_with_thread(thread_wait_exit, sk_pair, &tgl, &thread,
				  &tgl_pidfd, &thread_pidfd) < 0) {
		return KSFT_FAIL;
	}

	if (write(sk_pair[1], &thread, sizeof(thread)) != sizeof(thread)) {
		perror("write");
		goto cleanup;
	}

	// thread will exit, so this pidfd should eventually be dead (i.e.
	// poll should return).
	pollfd.fd = thread_pidfd;
	pollfd.events = POLLIN;

	ready = poll(&pollfd, 1, -1);
	if (ready == -1) {
		perror("poll");
		goto cleanup;
	}

	if (ready != 1) {
		ksft_print_msg("bad poll result %d\n", ready);
		goto cleanup;
	}

	if (pollfd.revents != POLLIN) {
		ksft_print_msg("bad poll revents: %x\n", pollfd.revents);
		goto cleanup;
	}

	errno = 0;
	if (sys_pidfd_getfd(thread_pidfd, 0, 0) > 0) {
		ksft_print_msg("got a real pidfd");
		goto cleanup;
	}

	if (errno != ESRCH) {
		ksft_print_msg("polling invalid thread didn't give ESRCH");
		goto cleanup;
	}

	// ok, but the thread group *leader* should still be alive
	pollfd.fd = tgl_pidfd;
	ready = poll(&pollfd, 1, 1);
	if (ready == -1) {
		perror("poll");
		goto cleanup;
	}

	if (ready != 0) {
		ksft_print_msg("polling leader returned something?! %x", pollfd.revents);
		goto cleanup;
	}

	errno = 0;
	if (sys_waitid(P_PIDFD, thread_pidfd, &siginfo, WEXITED, NULL) == 0) {
		ksft_print_msg("got a real wait value");
		goto cleanup;
	}

	if (errno != ECHILD) {
		ksft_print_msg("waiting invalid thread didn't give ECHILD: %d\n", errno);
		goto cleanup;
	}

	close(thread_pidfd);

	ret = KSFT_PASS;

cleanup:
	kill(tgl, SIGKILL);
	waitpid(tgl, NULL, 0);
	waitpid(thread, NULL, 0);
	return ret;
}

/*
 * This is a test for the first half of the scenario in
 * https://lore.kernel.org/all/20231207-avancieren-unbezahlbar-9258f45ec3ec@brauner/
 *
 * i.e we expect:
 *
 *      waitid(P_PIDFD, tg_pidfd)               waitid(P_PIDFD, t_pidfd)
 *      tg_pid[PIDTYPE_TGID] == tsk1            t_pid[PIDTYPE_TGID] == NULL
 *      => succeeds                             => fails
 */
static int test_non_tgl_waitid_exit(void)
{
	pid_t tgl, thread, writer;
	int sk_pair[2], status;
	int tgl_pidfd = -1, thread_pidfd = -1;
	int ret = KSFT_FAIL;
	siginfo_t siginfo;

	if (socketpair(PF_LOCAL, SOCK_SEQPACKET, 0, sk_pair) < 0) {
		ksft_print_msg("socketpair failed %s\n", strerror(errno));
		return KSFT_FAIL;
	}

	if (fork_task_with_thread(thread_wait_exit, sk_pair, &tgl, &thread,
				  &tgl_pidfd, &thread_pidfd) < 0) {
		return KSFT_FAIL;
	}

	writer = fork();
	if (writer < 0) {
		perror("fork");
		return KSFT_FAIL;
	}

	if (writer == 0) {
		sleep(3);
		if (write(sk_pair[1], &thread, sizeof(thread)) != sizeof(thread)) {
			perror("read");
			exit(1);
		}
		exit(0);
	}

	errno = 0;
	if (sys_waitid(P_PIDFD, thread_pidfd, &siginfo, __WALL | WEXITED, NULL) == 0
			|| errno != ECHILD) {
		ksft_print_msg("waitid didn't give ECHILD");
		goto cleanup;
	}
	close(thread_pidfd);

	kill(tgl, SIGKILL);

	errno = 0;
	if (sys_waitid(P_PIDFD, tgl_pidfd, &siginfo, __WALL | WEXITED, NULL) != 0) {
		perror("waitid");
		ksft_print_msg("waitid failed %d\n", errno);
		goto cleanup;
	}
	close(tgl_pidfd);

	if (siginfo.si_code != CLD_KILLED || siginfo.si_status != SIGKILL) {
		ksft_print_msg("bad exit code %x status %x\n", siginfo.si_code, siginfo.si_status);
		goto cleanup;
	}

	ret = KSFT_PASS;

cleanup:
	kill(tgl, SIGKILL);
	waitpid(tgl, &status, 0);
	waitpid(writer, NULL, 0);
	return ret;
}

/*
 * This is a test for the first second half of the scenario in
 * https://lore.kernel.org/all/20231207-avancieren-unbezahlbar-9258f45ec3ec@brauner/
 *
 * i.e after the exec, we expect:
 *
 *      waitid(P_PIDFD, tg_pidfd)		waitid(P_PIDFD, t_pid)
 *      tg_pid[PIDTYPE_TGID] == tsk2		t_pid[PIDTYPE_TGID] == NULL
 *      => succeeds				=> fails
 */
static int test_non_tgl_waitid_exec(void)
{
	pid_t tgl, thread, writer;
	int sk_pair[2], status;
	int tgl_pidfd = -1, thread_pidfd = -1;
	int ret = KSFT_FAIL;
	siginfo_t siginfo;

	if (socketpair(PF_LOCAL, SOCK_SEQPACKET, 0, sk_pair) < 0) {
		ksft_print_msg("socketpair failed %s\n", strerror(errno));
		return KSFT_FAIL;
	}

	if (fork_task_with_thread(thread_exec, sk_pair, &tgl, &thread,
				  &tgl_pidfd, &thread_pidfd) < 0) {
		return KSFT_FAIL;
	}

	writer = fork();
	if (writer < 0) {
		perror("fork");
		return KSFT_FAIL;
	}

	if (writer == 0) {
		sleep(3);
		if (write(sk_pair[1], &thread, sizeof(thread)) != sizeof(thread)) {
			perror("read");
			exit(1);
		}
		exit(0);
	}

	errno = 0;
	if (sys_waitid(P_PIDFD, thread_pidfd, &siginfo, __WALL | WEXITED, NULL) == 0
			|| errno != ECHILD) {
		ksft_print_msg("waitid didn't give ECHILD");
		goto cleanup;
	}
	close(thread_pidfd);

	kill(tgl, SIGKILL);

	errno = 0;
	if (sys_waitid(P_PIDFD, tgl_pidfd, &siginfo, __WALL | WEXITED, NULL) != 0) {
		perror("waitid");
		ksft_print_msg("waitid failed %d\n", errno);
		goto cleanup;
	}
	close(tgl_pidfd);

	if (siginfo.si_code != CLD_KILLED || siginfo.si_status != SIGKILL) {
		ksft_print_msg("bad exit code %x status %x\n", siginfo.si_code, siginfo.si_status);
		goto cleanup;
	}

	ret = KSFT_PASS;
cleanup:
	kill(tgl, SIGKILL);
	kill(thread, SIGKILL);
	waitpid(tgl, &status, 0);
	waitpid(thread, &status, 0);
	waitpid(writer, &status, 0);
	return ret;
}

static int test_clone_thread_pidfd(void)
{
	pid_t pid;
	int flags = CLONE_THREAD | CLONE_VM | CLONE_SIGHAND | CLONE_PIDFD;
	int pidfd;

	pid = clone(thread_sleep, stack + STACK_SIZE, flags, NULL, &pidfd);
	if (pid < 0) {
		perror("clone");
		return KSFT_FAIL;
	}

	close(pidfd);
	return KSFT_PASS;
}

#define T(x) { x, #x }
struct pidfd_non_tgl_test {
	int (*fn)();
	const char *name;
} tests[] = {
	T(test_non_tgl_basic),
	T(test_non_tgl_exec),
	T(test_non_tgl_poll_exit),
	T(test_non_tgl_exit_poll),
	T(test_non_tgl_waitid_exit),
	T(test_non_tgl_waitid_exec),
	T(test_clone_thread_pidfd),
};
#undef T

int main(int argc, char *argv[])
{
	int i, ret = EXIT_SUCCESS;

	for (i = 0; i < ARRAY_SIZE(tests); i++) {
		switch (tests[i].fn()) {
		case KSFT_PASS:
			ksft_test_result_pass("%s\n", tests[i].name);
			break;
		case KSFT_SKIP:
			ksft_test_result_skip("%s\n", tests[i].name);
			break;
		default:
			ret = EXIT_FAILURE;
			ksft_test_result_fail("%s\n", tests[i].name);
			break;
		}
	}

	return ret;
}
