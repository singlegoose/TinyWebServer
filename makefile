# 设置默认编译器为 g++
CXX ?= g++

# 调试模式开关，默认为 1（开启）
DEBUG ?= 1
ifeq ($(DEBUG), 1)
    # 开启调试模式，添加 -g 编译选项
    CXXFLAGS += -g
else
    # 关闭调试模式，添加 -O2 优化选项
    CXXFLAGS += -O2

endif

# 构建目标：server
# 依赖文件包括：
# - main.cpp：主入口文件
# - ./timer/lst_timer.cpp：定时器实现
# - ./http/http_conn.cpp：HTTP连接处理
# - ./log/log.cpp：日志系统实现
# - ./CGImysql/sql_connection_pool.cpp：数据库连接池实现
# - webserver.cpp：Web服务器核心实现
# - config.cpp：配置文件处理
server: main.cpp  ./timer/lst_timer.cpp ./http/http_conn.cpp ./log/log.cpp ./CGImysql/sql_connection_pool.cpp  webserver.cpp config.cpp
	# 编译命令：使用 g++ 编译所有依赖文件，生成可执行文件 server
	# 链接 pthread 库（用于线程操作）和 mysqlclient 库（用于数据库操作）
	$(CXX) -o server  $^ $(CXXFLAGS) -lpthread -lmysqlclient

# 清理规则：删除生成的可执行文件 server
clean:
	rm  -r server