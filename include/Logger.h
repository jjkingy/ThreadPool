#ifndef LOGGER_H
#define LOGGER_H

#include <string>
#include <mutex>

enum class LogLevel {
  NONE = 0,
  ERROR = 1,
  WARN = 2,
  INFO = 3,
  DEBUG = 4
};


class Logger {
public:
  Logger(LogLevel level = LogLevel::INFO, bool consoleOutput = true,
        const std::string& logFile = "");

  // 写日志
  void log(LogLevel msgLevel, const std::string& message);

  // 设置日志级别
  void setLevel(LogLevel newLevel);

  // 设置日志文件
  void setLogFile(const std::string& filename);

  // 启用/禁用控制台输出
  void setConsoleOutput(bool enable);
  

private:
  LogLevel level;
  bool consoleOutput;
  std::string logFile;
  std::mutex mutex;
};

#endif