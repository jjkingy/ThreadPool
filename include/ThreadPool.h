#ifndef THREAD_POOL_H
#define THREAD_POOL_H

#include <vector>              // 用于存储工作线程
#include <queue>               // 用于实现任务队列
#include <memory>              // 智能指针
#include <thread>              // C++11线程库
#include <mutex>               // 互斥锁，保证线程安全
#include <condition_variable>  // 条件变量，用于线程通信
#include <future>              // 异步任务结果
#include <functional>          // std::function，用于封装任务
#include <stdexcept>           // 标准异常
#include <atomic>              // 原子操作，线程安全的变量
#include <unordered_set>
#include <unordered_map>

#include "TaskInfo.h"
#include "Logger.h"
#include "ThreadPoolMetrics.h"


class ThreadPool {
public:
  ThreadPool(size_t threads, LogLevel loglevel = LogLevel::INFO,
            bool consoleLog = true, const std::string& logFile = "");

  //禁用拷贝构造函数和赋值操作符
  //一份池子 一份所有权 明令禁止拷贝赋值(内部很多资源不可复制)
  ThreadPool(const ThreadPool&) = delete;
  ThreadPool& operator=(const ThreadPool&) = delete;

  ~ThreadPool();


  // 提交任务到线程池
  template<class F, class... Args>
  auto enqueue(F&& f, Args&&... args) 
    -> std::future<typename std::invoke_result<F, Args...>::type>;

  // 带优先级的任务提交
  template<class F, class... Args>
  auto enqueueWithPriority(TaskPriority priority, F&& f, Args&&... args)
    -> std::future<typename std::invoke_result<F, Args...>::type>;
  
  // 带ID和描述得任务提交
  template<class F, class... Args>
  auto enqueueWithInfo(std::string TaskId, std::string description,
                      TaskPriority priority, F&& f, Args&&... args)
    -> std::future<typename std::invoke_result<F, Args...>::type>;

  // 获取工作线程数量
  size_t getThreadCount() const;
  
  // 获取当前活跃的线程数量
  size_t getActiveThreadCount() const;
  
  // 获取当前队列中等待执行的任务数量
  size_t getTaskCount();

    // 获取当前等待任务的线程数量
  size_t getWaitingThreadCount() const;
  
  // 获取已完成的任务数量
  size_t getCompletedTaskCount() const;

  size_t getFailedTaskCount() const;
  
  //状态查询方法
  bool isStopped() const { return stop; }

  //动态调整大小
  void resize(size_t threads);  

  //暂停线程池
  void pause();

  //恢复线程池
  void resume();

  //等待所有任务完成
  void waitForTasks();

  //清空任务队列
  void clearTasks();


  // 获取性能报告
  std::string getMetricsReport() const;

  // 设置日志级别
  void setLogLevel(LogLevel level);

private:
  //线程工作函数 从任务队列中获取任务并执行任务
  void workerThread(size_t id);

  // 创建任务函数
  template<class F, class... Args>
  auto createTaskFunction(std::shared_ptr<std::promise<typename std::invoke_result<F, Args...>::type>> promise,
                          F&& f, Args&&... args)
    ->std::function<void()>;                      

  // 记录任务提交日志
  void logTaskSubmission(const std::string& taskId, const std::string& description,
                        TaskPriority priority);

  std::unordered_set<size_t> threadsToStop; //需要停止的线程ID
  std::unordered_map<std::string, size_t> taskIdMap;  //任务映射表
  std::vector<std::thread> workers; //工作线程容器
  std::priority_queue<TaskInfo> tasks;  //任务队列

  //同步机制
  std::mutex queue_mutex;
  std::condition_variable condition;
  std::condition_variable waitCondition;

  std::atomic<bool> stop{false};
  std::atomic<bool> paused{false};

  Logger logger;
  ThreadPoolMetrics metrics;
  // //计数器
  // std::atomic<size_t> activeThreads{0};
  // std::atomic<size_t> completedTasks{0};
  // std::atomic<size_t> failedTasks{0};

};

// // 模板函数实现
// template<class F, class... Args>
// auto ThreadPool::enqueue(F&& f, Args&&... args) 
//   -> std::future<typename std::invoke_result<F, Args...>::type> {

//   using return_type = typename std::invoke_result<F, Args...>::type;

//   //创建任务包装
//   auto task = std::make_shared<std::packaged_task<return_type()>>(
//     std::bind(std::forward<F>(f), std::forward<Args>(args)...)
//   );

//   //得到与task关联的future对象
//   std::future<return_type> result = task->get_future();

//   //添加任务到任务队列
//   {
//     std::unique_lock<std::mutex> lock(queue_mutex);

//     if(stop) {
//       throw std::runtime_error("enqueue on stopped threadpool");
//     }

//     tasks.emplace([task]() { (*task)(); });
//   }

//   condition.notify_one();
//   return result;
// }

#include "ThreadPool.inl"

#endif