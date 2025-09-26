#include "TaskInfo.h"
#include <thread>

TaskInfo::TaskInfo(std::function<void()> t, TaskPriority p,
                  std::string id, std::string desc, std::chrono::milliseconds timeout)
  : task(std::move(t))
  , priority(p)
  , submitTime(std::chrono::steady_clock::now())
  , taskId(std::move(id))
  , description(std::move(desc))
  , timeout(timeout)
{
  // 为了避免时间精度问题，添加微小的延迟确保时间戳不同
  // std::this_thread::sleep_for(std::chrono::nanoseconds(1));
}


//我该不该让位给他
bool TaskInfo::operator<(const TaskInfo& other) const {
  if(priority != other.priority) {
    return priority < other.priority;
  }

  return submitTime > other.submitTime; //FIFO
}

std::string taskStatusToString(TaskStatus status) {
  switch (status) {
  case TaskStatus::WAITING:   return "等待中";
  case TaskStatus::RUNNING:   return "正在执行";
  case TaskStatus::COMPLETED: return "已完成";
  case TaskStatus::FAILED:    return "失败";
  case TaskStatus::CANCELED:  return "已取消";
  case TaskStatus::NOT_FOUND: return "任务不存在";
  default:                    return "未知状态";
  }
}

std::string priorityToString(TaskPriority priority) {
  switch (priority) {
  case TaskPriority::LOW:      return "低";
  case TaskPriority::MEDIUM:   return "中";
  case TaskPriority::HIGH:     return "高";
  case TaskPriority::CRITICAL: return "关键";
  default:                     return "未知";
  }
}
