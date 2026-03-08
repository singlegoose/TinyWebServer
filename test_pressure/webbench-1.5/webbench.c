/*
 * (C) Radim Kolar 1997-2004
 * This is free software, see GNU Public License version 2 for
 * details.
 *
 * Simple forking WWW Server benchmark:
 *
 * Usage:
 *   webbench --help
 *
 * Return codes:
 *    0 - sucess
 *    1 - benchmark failed (server is not on-line)
 *    2 - bad param
 *    3 - internal error, fork failed
 *
 */
#include "socket.c"
#include <unistd.h>
#include <sys/param.h>
#include <rpc/types.h>
#include <getopt.h>
#include <strings.h>
#include <time.h>
#include <signal.h>

/* 全局变量 */
volatile int timerexpired = 0; // 时间是否到期的标志
int speed = 0;                 // 成功请求数
int failed = 0;                // 失败请求数
int bytes = 0;                 // 传输的字节数

/* 全局配置 */
int http10 = 1; /* 0 - http/0.9, 1 - http/1.0, 2 - http/1.1 */
/* Allow: GET, HEAD, OPTIONS, TRACE */
#define METHOD_GET 0          // GET 请求方法
#define METHOD_HEAD 1         // HEAD 请求方法
#define METHOD_OPTIONS 2      // OPTIONS 请求方法
#define METHOD_TRACE 3        // TRACE 请求方法
#define PROGRAM_VERSION "1.5" // 程序版本
int method = METHOD_GET;      // 默认使用 GET 方法
int clients = 1;              // 默认客户端数量
int force = 0;                // 是否强制不等待服务器响应
int force_reload = 0;         // 是否强制刷新
int proxyport = 80;           // 代理端口
char *proxyhost = NULL;       // 代理主机
int benchtime = 30;           // 基准测试时间（秒）

/* 内部变量 */
int mypipe[2];              // 管道，用于子进程与父进程通信
char host[MAXHOSTNAMELEN];  // 目标主机名
#define REQUEST_SIZE 2048   // 请求大小
char request[REQUEST_SIZE]; // 请求缓冲区

/* 长选项定义 */
static const struct option long_options[] =
    {
        {"force", no_argument, &force, 1},                 // 强制不等待响应
        {"reload", no_argument, &force_reload, 1},         // 强制刷新
        {"time", required_argument, NULL, 't'},            // 测试时间
        {"help", no_argument, NULL, '?'},                  // 帮助信息
        {"http09", no_argument, NULL, '9'},                // 使用 HTTP/0.9
        {"http10", no_argument, NULL, '1'},                // 使用 HTTP/1.0
        {"http11", no_argument, NULL, '2'},                // 使用 HTTP/1.1
        {"get", no_argument, &method, METHOD_GET},         // 使用 GET 方法
        {"head", no_argument, &method, METHOD_HEAD},       // 使用 HEAD 方法
        {"options", no_argument, &method, METHOD_OPTIONS}, // 使用 OPTIONS 方法
        {"trace", no_argument, &method, METHOD_TRACE},     // 使用 TRACE 方法
        {"version", no_argument, NULL, 'V'},               // 显示版本
        {"proxy", required_argument, NULL, 'p'},           // 使用代理
        {"clients", required_argument, NULL, 'c'},         // 客户端数量
        {NULL, 0, NULL, 0}};

/* 函数声明 */
static void benchcore(const char *host, const int port, const char *request); // 核心测试函数
static int bench(void);                                                       // 测试主函数
static void build_request(const char *url);                                   // 构建 HTTP 请求

/**
 * 信号处理函数，处理 SIGALRM 信号
 * @param signal 信号编号
 */
static void alarm_handler(int signal)
{
   timerexpired = 1; // 设置时间到期标志
}

/**
 * 显示使用帮助信息
 */
static void usage(void)
{
   fprintf(stderr,
           "webbench [option]... URL\n"
           "  -f|--force               Don't wait for reply from server.\n"
           "  -r|--reload              Send reload request - Pragma: no-cache.\n"
           "  -t|--time <sec>          Run benchmark for <sec> seconds. Default 30.\n"
           "  -p|--proxy <server:port> Use proxy server for request.\n"
           "  -c|--clients <n>         Run <n> HTTP clients at once. Default one.\n"
           "  -9|--http09              Use HTTP/0.9 style requests.\n"
           "  -1|--http10              Use HTTP/1.0 protocol.\n"
           "  -2|--http11              Use HTTP/1.1 protocol.\n"
           "  --get                    Use GET request method.\n"
           "  --head                   Use HEAD request method.\n"
           "  --options                Use OPTIONS request method.\n"
           "  --trace                  Use TRACE request method.\n"
           "  -?|-h|--help             This information.\n"
           "  -V|--version             Display program version.\n");
};

/**
 * 主函数
 * @param argc 命令行参数数量
 * @param argv 命令行参数
 * @return 退出状态码
 */
int main(int argc, char *argv[])
{
   int opt = 0;
   int options_index = 0;
   char *tmp = NULL;

   if (argc == 1)
   {
      usage();
      return 2;
   }

   while ((opt = getopt_long(argc, argv, "912Vfrt:p:c:?h", long_options, &options_index)) != EOF)
   {
      switch (opt)
      {
      case 0:
         break;
      case 'f':
         force = 1;
         break; // 强制不等待响应
      case 'r':
         force_reload = 1;
         break; // 强制刷新
      case '9':
         http10 = 0;
         break; // 使用 HTTP/0.9
      case '1':
         http10 = 1;
         break; // 使用 HTTP/1.0
      case '2':
         http10 = 2;
         break; // 使用 HTTP/1.1
      case 'V':
         printf(PROGRAM_VERSION "\n");
         exit(0); // 显示版本
      case 't':
         benchtime = atoi(optarg);
         break; // 设置测试时间
      case 'p':
         /* 解析代理服务器地址和端口 */
         tmp = strrchr(optarg, ':');
         proxyhost = optarg;
         if (tmp == NULL)
         {
            break;
         }
         if (tmp == optarg)
         {
            fprintf(stderr, "Error in option --proxy %s: Missing hostname.\n", optarg);
            return 2;
         }
         if (tmp == optarg + strlen(optarg) - 1)
         {
            fprintf(stderr, "Error in option --proxy %s Port number is missing.\n", optarg);
            return 2;
         }
         *tmp = '\0';
         proxyport = atoi(tmp + 1);
         break;
      case ':':
      case 'h':
      case '?':
         usage();
         return 2;
         break; // 显示帮助信息
      case 'c':
         clients = atoi(optarg);
         break; // 设置客户端数量
      }
   }

   if (optind == argc)
   {
      fprintf(stderr, "webbench: Missing URL!\n");
      usage();
      return 2;
   }

   if (clients == 0)
      clients = 1; // 确保客户端数量至少为1
   if (benchtime == 0)
      benchtime = 60; // 确保测试时间至少为60秒

   /* 显示版权信息 */
   fprintf(stderr, "Webbench - Simple Web Benchmark " PROGRAM_VERSION "\n"
                   "Copyright (c) Radim Kolar 1997-2004, GPL Open Source Software.\n");
   build_request(argv[optind]); // 构建 HTTP 请求

   /* 显示测试信息 */
   printf("\nBenchmarking: ");
   switch (method)
   {
   case METHOD_GET:
   default:
      printf("GET");
      break;
   case METHOD_OPTIONS:
      printf("OPTIONS");
      break;
   case METHOD_HEAD:
      printf("HEAD");
      break;
   case METHOD_TRACE:
      printf("TRACE");
      break;
   }
   printf(" %s", argv[optind]);
   switch (http10)
   {
   case 0:
      printf(" (using HTTP/0.9)");
      break;
   case 2:
      printf(" (using HTTP/1.1)");
      break;
   }
   printf("\n");
   if (clients == 1)
      printf("1 client");
   else
      printf("%d clients", clients);

   printf(", running %d sec", benchtime);
   if (force)
      printf(", early socket close");
   if (proxyhost != NULL)
      printf(", via proxy server %s:%d", proxyhost, proxyport);
   if (force_reload)
      printf(", forcing reload");
   printf(".\n");
   return bench(); // 执行测试
}

/**
 * 构建 HTTP 请求
 * @param url 目标 URL
 */
void build_request(const char *url)
{
   char tmp[10];
   int i;

   bzero(host, MAXHOSTNAMELEN);
   bzero(request, REQUEST_SIZE);

   if (force_reload && proxyhost != NULL && http10 < 1)
      http10 = 1;
   if (method == METHOD_HEAD && http10 < 1)
      http10 = 1;
   if (method == METHOD_OPTIONS && http10 < 2)
      http10 = 2;
   if (method == METHOD_TRACE && http10 < 2)
      http10 = 2;

   switch (method)
   {
   default:
   case METHOD_GET:
      strcpy(request, "GET");
      break;
   case METHOD_HEAD:
      strcpy(request, "HEAD");
      break;
   case METHOD_OPTIONS:
      strcpy(request, "OPTIONS");
      break;
   case METHOD_TRACE:
      strcpy(request, "TRACE");
      break;
   }

   strcat(request, " ");

   if (NULL == strstr(url, "://"))
   {
      fprintf(stderr, "\n%s: is not a valid URL.\n", url);
      exit(2);
   }
   if (strlen(url) > 1500)
   {
      fprintf(stderr, "URL is too long.\n");
      exit(2);
   }
   if (proxyhost == NULL)
      if (0 != strncasecmp("http://", url, 7))
      {
         fprintf(stderr, "\nOnly HTTP protocol is directly supported, set --proxy for others.\n");
         exit(2);
      }
   /* 协议/主机分隔符 */
   i = strstr(url, "://") - url + 3;

   if (strchr(url + i, '/') == NULL)
   {
      fprintf(stderr, "\nInvalid URL syntax - hostname don't ends with '/'.\n");
      exit(2);
   }
   if (proxyhost == NULL)
   {
      /* 从主机名获取端口 */
      if (index(url + i, ':') != NULL &&
          index(url + i, ':') < index(url + i, '/'))
      {
         strncpy(host, url + i, strchr(url + i, ':') - url - i);
         bzero(tmp, 10);
         strncpy(tmp, index(url + i, ':') + 1, strchr(url + i, '/') - index(url + i, ':') - 1);
         proxyport = atoi(tmp);
         if (proxyport == 0)
            proxyport = 80;
      }
      else
      {
         strncpy(host, url + i, strcspn(url + i, "/"));
      }
      strcat(request + strlen(request), url + i + strcspn(url + i, "/"));
   }
   else
   {
      strcat(request, url);
   }
   if (http10 == 1)
      strcat(request, " HTTP/1.0");
   else if (http10 == 2)
      strcat(request, " HTTP/1.1");
   strcat(request, "\r\n");
   if (http10 > 0)
      strcat(request, "User-Agent: WebBench " PROGRAM_VERSION "\r\n");
   if (proxyhost == NULL && http10 > 0)
   {
      strcat(request, "Host: ");
      strcat(request, host);
      strcat(request, "\r\n");
   }
   if (force_reload && proxyhost != NULL)
   {
      strcat(request, "Pragma: no-cache\r\n");
   }
   if (http10 > 1)
      strcat(request, "Connection: close\r\n");
   /* 在末尾添加空行 */
   if (http10 > 0)
      strcat(request, "\r\n");
}

/**
 * 执行基准测试
 * @return 测试结果
 */
static int bench(void)
{
   int i, j, k;
   pid_t pid = 0;
   FILE *f;

   /* 检查目标服务器是否可用 */
   i = Socket(proxyhost == NULL ? host : proxyhost, proxyport);
   if (i < 0)
   {
      fprintf(stderr, "\nConnect to server failed. Aborting benchmark.\n");
      return 1;
   }
   close(i);

   /* 创建管道 */
   if (pipe(mypipe))
   {
      perror("pipe failed.");
      return 3;
   }

   /*  fork 子进程 */
   for (i = 0; i < clients; i++)
   {
      pid = fork();
      if (pid <= (pid_t)0)
      {
         /* 子进程或错误 */
         sleep(1); /* 让子进程更快启动 */
         break;
      }
   }

   if (pid < (pid_t)0)
   {
      fprintf(stderr, "problems forking worker no. %d\n", i);
      perror("fork failed.");
      return 3;
   }

   if (pid == (pid_t)0)
   {
      /* 子进程 */
      if (proxyhost == NULL)
         benchcore(host, proxyport, request);
      else
         benchcore(proxyhost, proxyport, request);

      /* 将结果写入管道 */
      f = fdopen(mypipe[1], "w");
      if (f == NULL)
      {
         perror("open pipe for writing failed.");
         return 3;
      }
      fprintf(f, "%d %d %d\n", speed, failed, bytes);
      fclose(f);
      return 0;
   }
   else
   {
      f = fdopen(mypipe[0], "r");
      if (f == NULL)
      {
         perror("open pipe for reading failed.");
         return 3;
      }
      setvbuf(f, NULL, _IONBF, 0);
      speed = 0;
      failed = 0;
      bytes = 0;

      while (1)
      {
         pid = fscanf(f, "%d %d %d", &i, &j, &k);
         if (pid < 2)
         {
            fprintf(stderr, "Some of our childrens died.\n");
            break;
         }
         speed += i;
         failed += j;
         bytes += k;
         if (--clients == 0)
            break;
      }
      fclose(f);

      printf("\nSpeed=%d pages/min, %d bytes/sec.\nRequests: %d susceed, %d failed.\n",
             (int)((speed + failed) / (benchtime / 60.0f)),
             (int)(bytes / (float)benchtime),
             speed,
             failed);
   }
   return i;
}

/**
 * 核心测试函数，执行实际的 HTTP 请求
 * @param host 目标主机
 * @param port 目标端口
 * @param req HTTP 请求
 */
void benchcore(const char *host, const int port, const char *req)
{
   int rlen;
   char buf[1500];
   int s, i;
   struct sigaction sa;

   /* 设置信号处理 */
   sa.sa_handler = alarm_handler;
   sa.sa_flags = 0;
   if (sigaction(SIGALRM, &sa, NULL))
      exit(3);
   alarm(benchtime); // 设置闹钟

   rlen = strlen(req);
nexttry:
   while (1)
   {
      if (timerexpired) // 时间到期
      {
         if (failed > 0)
         {
            failed--;
         }
         return;
      }
      s = Socket(host, port); // 创建套接字
      if (s < 0)
      {
         failed++;
         continue;
      }
      if (rlen != write(s, req, rlen))
      {
         failed++;
         close(s);
         continue;
      } // 发送请求
      if (http10 == 0)
         if (shutdown(s, 1))
         {
            failed++;
            close(s);
            continue;
         } // HTTP/0.9 模式下关闭写端
      if (force == 0) // 非强制模式下，读取响应
      {
         /* 读取所有可用数据 */
         while (1)
         {
            if (timerexpired)
               break;
            i = read(s, buf, 1500);
            if (i < 0)
            {
               failed++;
               close(s);
               goto nexttry;
            }
            else if (i == 0)
               break;
            else
               bytes += i; // 累计传输字节数
         }
      }
      if (close(s))
      {
         failed++;
         continue;
      } // 关闭套接字
      speed++; // 成功请求数加1
   }
}