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
  CANCELED
};

struct TaskInfo {
  std::function<void()> task; 
  TaskPriority priority;  
  TaskStatus status{ TaskStatus::WAITING};
  std::string taskId;
  std::string description;
  std::string errorMessage;
  std::chrono::steady_clock::time_point submitTime;

  TaskInfo(std::function<void()> t = nullptr,
          TaskPriority p = TaskPriority::MEDIUM,
          std::string id = "",
          std::string desc = "");
  bool operator<(const TaskInfo& other) const;
};

std::string taskStatusToString(TaskStatus status);

std::string priorityToString(TaskPriority prioruty);



#endif