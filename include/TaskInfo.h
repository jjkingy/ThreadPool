#ifndef TASK_INFO_H
#define TASK_INFO_H

#include <string>
#include <chrono>
#include <functional>

//任务优先级
enum class TaskPriority {
  LOW,
  MEDIUM,
  HIGH,
  CRITICAL
};


//任务状态
enum class TaskStatus {
  WAITING,
  RUNNING,
  COMPLETED,
  FAILED,
  CANCELED,
  NOT_FOUND
};

// 获取任务的结果枚举
enum class TaskFetchResult {
  SHOULD_EXIT,  // 线程应该退出
  NO_TASK,      // 没有任务，但应该继续运行
  HAS_TASK      // 成功获取了任务
};

struct TaskInfo {
  std::function<void()> task; 
  TaskPriority priority;  
  TaskStatus status{ TaskStatus::WAITING};
  std::string taskId;
  std::string description;
  std::string errorMessage;
  std::chrono::steady_clock::time_point submitTime;
  std::chrono::milliseconds timeout{0}; //任务超时时间(毫秒) 0表示无超时限制

  TaskInfo(std::function<void()> t = nullptr,
          TaskPriority p = TaskPriority::MEDIUM,
          std::string id = "",
          std::string desc = "",
          std::chrono::milliseconds  timeout = std::chrono::milliseconds(0));
  bool operator<(const TaskInfo& other) const;
};

std::string taskStatusToString(TaskStatus status);

std::string priorityToString(TaskPriority prioruty);



#endif