#include "Logger.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <chrono>


Logger::Logger(LogLevel level, bool consoleOutput, const std::string& logFile) 
  : level(level)
  , consoleOutput(consoleOutput)
  , logFile(logFile) {}

// 写日志
void Logger::log(LogLevel msgLevel, const std::string& message) {
  if(level == LogLevel::NONE || msgLevel > level) return;

  std::lock_guard<std::mutex> lock(mutex);
  auto now = std::chrono::system_clock::now();
  auto now_time_t = std::chrono::system_clock::to_time_t(now);

  //把time_t格式的时间转换成带格式的字符串
  //把 time_t 转换成 本地时间（struct tm* 类型，里边有年、月、日、小时、分钟、秒）
  std::stringstream timestamp;
  timestamp << std::put_time(std::localtime(&now_time_t), "%Y-%m-%d, %H:%M:%S");

  std::string levelStr;
  switch(msgLevel) {
    case LogLevel::ERROR: levelStr = "错误"; break;
    case LogLevel::INFO:  levelStr = "信息"; break;
    case LogLevel::DEBUG: levelStr = "调试"; break;
    default: levelStr = "位置";
  }

  std::stringstream logMsg;
  logMsg << "[" << timestamp.str() << "] [" << levelStr << "]" << message;

  if(consoleOutput) {
    if(msgLevel == LogLevel::ERROR) {
      std::cerr << logMsg.str() << std::endl;
    } else {
      std::cout << logMsg.str() << std::endl;
    }
  }
  if(!logFile.empty()) {
    try {
      std::ofstream file(logFile, std::ios::app);
      if(file.is_open()) {
        file << logMsg.str() << std::endl;
      }
    } catch(...) {
      if(consoleOutput) {
        std::cerr << "无法写入日志文件" << logFile << std::endl;
      }
    }
  }

}

// 设置日志级别
void Logger::setLevel(LogLevel newLevel) {
  level = newLevel;
}

// 设置日志文件
void Logger::setLogFile(const std::string& filename) {
  std::lock_guard<std::mutex> lock(mutex);
  logFile = filename;
}

// 启用/禁用控制台输出
void Logger::setConsoleOutput(bool enable) {
  std::lock_guard<std::mutex> lock(mutex);
  consoleOutput = enable;
}