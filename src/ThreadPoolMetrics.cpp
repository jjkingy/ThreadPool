#include "ThreadPoolMetrics.h"
#include <sstream>
#include <iomanip>

// 构造函数
ThreadPoolMetrics::ThreadPoolMetrics() : startTime(std::chrono::steady_clock::now()) {}

// 更新队列大小并记录峰值
void ThreadPoolMetrics::updateQueueSize(size_t size) {
  /*
  load可以显示指定内存序参数
  memory_order_relaxed：只保证原子性，不保证顺序性。
  memory_order_acquire：当前线程之后的读写不能重排到这个 load 之前。
  memory_order_seq_cst：顺序一致性，最严格，所有线程看到的顺序相同*/
  size_t currentPeak = peakQueueSize.load();
  
  //并发编程原语 用来在多线程条件下更新
  //如果当前原子值 == expected，则更新为 desired，返回 true。
  //否则，返回 false，并把实际值写回 expected。
  while(size > currentPeak && !peakQueueSize.compare_exchange_weak(currentPeak, size)){
    // 如果更新失败，currentPeak会被更新为当前值(把当前的实际值写回currentPeak)，然后重试
  }

}

// 更新活跃线程数并记录峰值
void ThreadPoolMetrics::updateActiveThreads(size_t count) {
  activeThreads.store(count);
  size_t currentPeak = peakThreads.load();
  while(count > currentPeak && !peakThreads.compare_exchange_weak(currentPeak, count)) {

  }

}

// 添加任务执行时间
void ThreadPoolMetrics::addTaskTime(uint64_t timeNs) {
  totalTaskTimeNs.fetch_add(timeNs);
}

// 获取平均任务执行时间（毫秒）
double ThreadPoolMetrics::getAverageTaskTime() const {
  size_t completed = completedTasks.load();
  if(completed == 0)  return 0.0;
  return static_cast<double>(totalTaskTimeNs.load()) / completed / 1000000.0;
}

// 获取线程池运行时间（秒）
double ThreadPoolMetrics::getUptime() const {
  auto now = std::chrono::steady_clock::now();
  return std::chrono::duration<double>(now - startTime).count();
}

// 获取任务吞吐量（每秒任务数）
double ThreadPoolMetrics::getThroughput() const {
  double uptime = getUptime();
  if(uptime <= 0.0) return 0.0;
  return static_cast<double>(completedTasks.load()) / uptime;
}

// 获取性能报告
std::string ThreadPoolMetrics::getReport() const {
  std::stringstream ss;
  ss << "线程池性能报告:" << std::endl;
  ss << "  运行时间: " << getUptime() << " 秒" << std::endl;
  ss << "  总任务数: " << totalTasks.load() << std::endl;
  ss << "  已完成任务数: " << completedTasks.load() << std::endl;
  ss << "  失败任务数: " << failedTasks.load() << std::endl;
  ss << "  当前活跃线程数: " << activeThreads.load() << std::endl;
  ss << "  峰值活跃线程数: " << peakThreads.load() << std::endl;
  ss << "  峰值队列大小: " << peakQueueSize.load() << std::endl;
  ss << "  平均任务执行时间: " << getAverageTaskTime() << " 毫秒" << std::endl;
  ss << "  任务吞吐量: " << getThroughput() << " 任务/秒" << std::endl;
  return ss.str();
}