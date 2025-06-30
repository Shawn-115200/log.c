//  gcc main.c   src/log.c  -I./src -lpthread  -g  -DLOG_USE_COLOR 

#include <stdio.h>
#include <unistd.h>
#include <stdbool.h>
#include <time.h>
#include <signal.h>
#include <pthread.h>
#include "log.h"

// 宏定义配置
#define LOG_INFO_FILE      "/tmp/log_info.txt"
#define LOG_LEVEL          LOG_DEBUG
#define THREAD_SLEEP_TIME  1
#define TIME_STR_LEN       26

// 全局互斥锁用于日志线程安全
pthread_mutex_t MUTEX_LOG;

// 线程控制变量
volatile bool thread_running = true;
pthread_t print_thread;

// 信号处理函数
void signal_handler(int sig) {
	if (sig == SIGINT) {
		log_warn("\nReceived SIGINT (Ctrl+C), shutting down gracefully...\n");
		thread_running = false;
	}
}

// 日志锁回调函数
void log_lock(bool lock, void *udata) {
	pthread_mutex_t *lock_ptr = (pthread_mutex_t *)udata;
	if (lock)
		pthread_mutex_lock(lock_ptr);
	else
		pthread_mutex_unlock(lock_ptr);
}

// 打印线程函数
void* print_thread_func(void* arg) {
	int counter = 0;
	log_warn("Print thread started...\n");
	
	while (thread_running) {
		// 获取当前时间
		time_t now = time(NULL);
		struct tm *tm_info = localtime(&now);
		char time_str[TIME_STR_LEN];
		strftime(time_str, TIME_STR_LEN, "%Y-%m-%d %H:%M:%S", tm_info);
		
		// 打印信息
		log_warn("[%s] Thread print count: %d\n", time_str, counter++);
		
		// 简单的日志输出，不使用手动锁
		log_info("Thread print count: %d", counter - 1);
		
		// 使用更短的睡眠时间，确保能及时响应退出信号
		for (int i = 0; i < THREAD_SLEEP_TIME * 10 && thread_running; i++) {
			usleep(100000); // 休眠0.1秒
		}
	}
	
	log_warn("Print thread exiting...\n");
	return NULL;
}

int main() {
	// 注册信号处理器
	signal(SIGINT, signal_handler);
	
	// 设置日志等级和输出
	log_set_level(LOG_LEVEL);
	log_set_quiet(0);

	// 初始化互斥锁
	if (pthread_mutex_init(&MUTEX_LOG, NULL) != 0) {
		fprintf(stderr, "Failed to initialize mutex\n");
		return -1;
	}

	// 设置日志锁
	log_set_lock(log_lock, &MUTEX_LOG);

	// 打开信息日志文件
	FILE *fp_info = fopen(LOG_INFO_FILE, "ab");
	if (fp_info == NULL) {
		fprintf(stderr, "Failed to open %s\n", LOG_INFO_FILE);
		pthread_mutex_destroy(&MUTEX_LOG);
		return -1;
	}
	log_add_fp(fp_info, LOG_INFO);

	// 日志输出示例
	log_debug("debug");
	log_info("info");
	log_warn("warn");

	// 线程安全日志输出
	log_info("I'm threadsafe");

	// 创建打印线程
	if (pthread_create(&print_thread, NULL, print_thread_func, NULL) != 0) {
		fprintf(stderr, "Failed to create print thread\n");
		fclose(fp_info);
		pthread_mutex_destroy(&MUTEX_LOG);
		return -1;
	}
	
	log_warn("Print thread created successfully.\n");
	log_warn("Press Enter or Ctrl+C to stop the program...\n");
	
	// 等待用户输入或信号来停止线程
	log_warn("Waiting for exit signal...\n");
	
	// 等待回车键或信号
	while (thread_running) {
		sleep(1);
	}
	
	// 停止线程
	thread_running = false;
	log_warn("Stopping print thread...\n");
	
	// 等待线程结束
	if (pthread_join(print_thread, NULL) != 0) {
		fprintf(stderr, "Failed to join print thread, trying to cancel...\n");
		// 如果join失败，尝试取消线程
		if (pthread_cancel(print_thread) == 0) {
			log_warn("Thread cancelled successfully.\n");
		} else {
			fprintf(stderr, "Failed to cancel thread\n");
		}
	} else {
		log_warn("Print thread joined successfully.\n");
	}

	log_warn("Program terminated gracefully.\n");

	// 清理资源
	fclose(fp_info);
	pthread_mutex_destroy(&MUTEX_LOG);

	return 0;
}





