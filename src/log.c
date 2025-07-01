/*
 * Copyright (c) 2020 rxi
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "log.h"
#include <sys/stat.h>
#include <string.h>
#include <stdlib.h>

typedef struct {
	log_LogFn fn;
	void *udata;
	int level;
	char *filename;  // 添加文件名字段
} Callback;

static struct {
	void *udata;
	log_LockFn lock;
	int level;
	bool quiet;
	Callback callbacks[MAX_CALLBACKS];
} L;

#define ANSI_COLOR_RESET     "\x1b[0m"
#define ANSI_COLOR_TRACE     "\x1b[94m"  // 亮蓝色
#define ANSI_COLOR_DEBUG     "\x1b[36m"  // 青色
#define ANSI_COLOR_INFO      "\x1b[32m"  // green
#define ANSI_COLOR_WARN      "\x1b[33m"  // yellow
#define ANSI_COLOR_ERROR     "\x1b[31m"  // red
#define ANSI_COLOR_FATAL     "\x1b[35m"  // 品红色

#ifdef STDOUT_LOG_USE_COLOR
static const char *level_colors[] = {
	ANSI_COLOR_TRACE,
	ANSI_COLOR_DEBUG,
	ANSI_COLOR_INFO,
	ANSI_COLOR_WARN,
	ANSI_COLOR_ERROR,
	ANSI_COLOR_FATAL
};
#endif

static const char *level_strings[] = {
	"TRACE",
	"DEBUG",
	"INFO",
	"WARN",
	"ERROR",
	"FATAL"
};


// 提取文件名，只保留最后一个部分
static const char* get_short_filename(const char* full_path)
{
	if (!full_path) {
		return "unknown";
	}

	const char* last_slash = strrchr(full_path, '/');
	if (last_slash) {
		return last_slash + 1;  // 跳过斜杠
	}

	return full_path;  // 如果没有斜杠，返回原字符串
}

static void stdout_callback(log_Event *ev)
{
	char buf[16];
	buf[strftime(buf, sizeof(buf), "%H:%M:%S", ev->time)] = '\0';
#ifdef STDOUT_LOG_USE_COLOR
	fprintf(
		ev->udata, "%s %s%-5s\x1b[0m \x1b[90m%s:%d:\x1b[0m ",
		buf, level_colors[ev->level], level_strings[ev->level],
		get_short_filename(ev->file), ev->line);
#else
	fprintf(
		ev->udata, "%s %-5s %s:%d: ",
		buf, level_strings[ev->level], get_short_filename(ev->file), ev->line);
#endif
	vfprintf(ev->udata, ev->fmt, ev->ap);
//  fprintf(ev->udata, "\n");   //不自动追加  回车换行符
	fflush(ev->udata);
}

// 检查文件大小
static long get_file_size(const char *filename)
{
	struct stat st;
	if (stat(filename, &st) == 0) {
		return st.st_size;
	}
	return 0;
}

// 重命名文件
static int rename_file(const char *oldname, const char *newname)
{
	return rename(oldname, newname);
}

// 删除文件
static int delete_file(const char *filename)
{
	return remove(filename);
}

// 日志轮转函数
static void rotate_log_file(const char *filename)
{
	if (!filename) {
		return;
	}

	// 输出调试信息
#if 0
	fprintf(stderr, "Log rotation triggered for: %s\n", filename);
#endif

	// 删除最旧的文件（如果存在）
	char old_filename[256];
	snprintf(old_filename, sizeof(old_filename), "%s.%d", filename,
			 MAX_LOG_FILES - 1);
	delete_file(old_filename);

	// 重命名现有文件
	for (int i = MAX_LOG_FILES - 2; i >= 0; i--) {
		char src_filename[256];
		char dst_filename[256];

		if (i == 0) {
			snprintf(src_filename, sizeof(src_filename), "%s", filename);
		} else {
			snprintf(src_filename, sizeof(src_filename), "%s.%d", filename, i);
		}
		snprintf(dst_filename, sizeof(dst_filename), "%s.%d", filename, i + 1);

		if (rename_file(src_filename, dst_filename) == 0) {
#if 0
			fprintf(stderr, "Renamed: %s -> %s\n", src_filename, dst_filename);
#endif
		}
	}

	// 重新打开文件（清空内容）
	FILE *fp = fopen(filename, "w");
	if (fp) {
		fclose(fp);
#if 0
		fprintf(stderr, "Created new log file: %s\n", filename);
#endif
	}
}

static void file_callback(log_Event *ev)
{
	// 检查并轮转日志文件
	for (int i = 0; i < MAX_CALLBACKS && L.callbacks[i].fn; i++) {
		Callback *cb = &L.callbacks[i];
		if (cb->fn == file_callback && cb->filename) {
			// 检查当前文件大小
			long current_size = get_file_size(cb->filename);

			// 估算即将写入的内容大小（粗略估算）
			char test_buf[256];
			va_list ap_copy;
			va_copy(ap_copy, ev->ap);
			int estimated_size = vsnprintf(test_buf, sizeof(test_buf), ev->fmt, ap_copy);
			va_end(ap_copy);

			// 如果写入后会超过限制，先轮转
			if (current_size + estimated_size > MAX_LOG_SIZE) {
#if 0
				fprintf(stderr, "Pre-rotation check: current=%ld, estimated=%d, max=%d\n",
						current_size, estimated_size, MAX_LOG_SIZE);
#endif
				rotate_log_file(cb->filename);

				// 重新打开文件指针，确保写入到新文件
				FILE *old_fp = (FILE*)cb->udata;
				fclose(old_fp);
				FILE *new_fp = fopen(cb->filename, "a");
				if (new_fp) {
					cb->udata = new_fp;
					ev->udata = new_fp;
				} else {
					// 如果无法打开新文件，使用stderr作为备选
					ev->udata = stderr;
				}
			}
			break;
		}
	}

	char buf[64];
	buf[strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", ev->time)] = '\0';
	fprintf(
		ev->udata, "%s %-5s %s:%d: ",
		buf, level_strings[ev->level], get_short_filename(ev->file), ev->line);
	vfprintf(ev->udata, ev->fmt, ev->ap);
	//fprintf(ev->udata, "\n");  //不自动追加  回车换行符
	fflush(ev->udata);
}


static void lock(void)
{
	if (L.lock) {
		L.lock(true, L.udata);
	}
}


static void unlock(void)
{
	if (L.lock) {
		L.lock(false, L.udata);
	}
}


const char* log_level_string(int level)
{
	return level_strings[level];
}


void log_set_lock(log_LockFn fn, void *udata)
{
	L.lock = fn;
	L.udata = udata;
}


void log_set_level(int level)
{
	L.level = level;
}


void log_set_quiet(bool enable)
{
	L.quiet = enable;
}


int log_add_callback(log_LogFn fn, void *udata, int level)
{
	for (int i = 0; i < MAX_CALLBACKS; i++) {
		if (!L.callbacks[i].fn) {
			L.callbacks[i] = (Callback) {
				fn, udata, level
			};
			return 0;
		}
	}
	return -1;
}


int log_add_fp(FILE *fp, int level)
{
	return log_add_callback(file_callback, fp, level);
}

// 新增：带文件名的日志文件添加函数
int log_add_fp_with_name(FILE *fp, int level, const char *filename)
{
	for (int i = 0; i < MAX_CALLBACKS; i++) {
		if (!L.callbacks[i].fn) {
			L.callbacks[i] = (Callback) {
				file_callback, fp, level, strdup(filename)
			};
			return 0;
		}
	}
	return -1;
}


static void init_event(log_Event *ev, void *udata)
{
	if (!ev->time) {
		time_t t = time(NULL);
		ev->time = localtime(&t);
	}
	ev->udata = udata;
}


void log_log(int level, const char *file, int line, const char *fmt, ...)
{
	log_Event ev = {
		.fmt   = fmt,
		.file  = file,
		.line  = line,
		.level = level,
	};

	lock();

	if (!L.quiet && level >= L.level) {
		init_event(&ev, stderr);
		va_start(ev.ap, fmt);
		stdout_callback(&ev);
		va_end(ev.ap);
	}

	for (int i = 0; i < MAX_CALLBACKS && L.callbacks[i].fn; i++) {
		Callback *cb = &L.callbacks[i];
		if (level >= cb->level) {
			init_event(&ev, cb->udata);
			va_start(ev.ap, fmt);
			cb->fn(&ev);
			va_end(ev.ap);
		}
	}

	unlock();
}
