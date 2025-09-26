#ifndef THREAD_POOL_INL
#define THREAD_POOL_INL

// 提交任务到线程池
template<class F, class... Args>
auto ThreadPool::enqueue(F&& f, Args&&... args)
  -> std::future<typename std::invoke_result<F, Args...>::type> {
  return enqueueWithPriority(TaskPriority::MEDIUM,
    std::chrono::milliseconds(0),
    std::forward<F>(f),
    std::forward<Args>(args)...);
}


// 打包任务函数
template<class F, class... Args>
auto ThreadPool::createSimpleTask(std::shared_ptr<std::promise<typename std::invoke_result<F, Args...>::type>> promise,
                        F&& f, Args&&... args)
  ->std::function<void()> {
    using return_type = typename std::invoke_result<F, Args...>::type;

    //返回一个lambda lambda不能直接捕获可变参数包
    //tuple是一个可变参数模板，存储任意类型任意数量得值
    return [this, promise, f = std::forward<F>(f), 
      args = std::make_tuple(std::forward<Args>(args)...)]() mutable {
      try {
        //set_value把结果塞进去
        if constexpr(std::is_void_v<return_type>) {
          std::apply(f, args);
          promise->set_value();
        } else {
          promise->set_value(std::apply(f, args));
        }
      }
      catch(const std::exception& e) {
        this->recordTaskFailure(e.what(), false);
        promise->set_exception(std::current_exception());
      }
      catch(...) {
        //处理其他类型的异常
        this->recordTaskFailure("未知异常", false);
        promise->set_exception(std::current_exception());
        throw;
      }

    };
}


// 创建带超时处理的任务函数 在lambda中处理promise 和进行超时处理
template<class F, class... Args>
auto ThreadPool::createTaskWithTimeoutHandling(
  std::shared_ptr<std::promise<typename std::invoke_result<F, Args...>::type>> promise,
  std::chrono::milliseconds timeout,
  F&& f, Args&&... args) -> std::function<void()> {

  using return_type = typename std::invoke_result<F, Args...>::type;

  return [this, promise, timeout,
    f = std::forward<F>(f),
    args = std::make_tuple(std::forward<Args>(args)...)]() mutable {
    
    try {
      // 使用 std::async 执行任务并处理超时
      auto future = std::async(std::launch::async, [&]() {
          return std::apply(f, args);
      });
      
      auto status = future.wait_for(timeout);
      
      if (status == std::future_status::timeout) {
          // 超时处理
          std::string errorMessage = "Task timed out after " + 
                                    std::to_string(timeout.count()) + "ms";
          
          // 记录超时统计和日志
          this->recordTaskFailure(errorMessage, true);
          
          // 设置 promise 异常状态
          promise->set_exception(std::make_exception_ptr(
              std::runtime_error(errorMessage)));
          return;
      }
      
      // 任务正常完成，获取结果并设置到 promise
      if constexpr (std::is_void_v<return_type>) {
          future.get(); // 如果任务有异常，这里会抛出
          promise->set_value();
      } else {
          promise->set_value(future.get());
      }
      
    } catch (const std::exception& e) {
      // 处理任务执行异常（非超时）
      std::string errorMessage = e.what();
      
      // 检查是否已经是超时异常（避免重复处理）
      if (errorMessage.find("timed out") == std::string::npos) {
          this->recordTaskFailure(errorMessage, false);
      }
      
      promise->set_exception(std::current_exception());
    } catch (...) {
      // 处理未知异常
      this->recordTaskFailure("未知异常", false);
      promise->set_exception(std::current_exception());
    }
  };
}

// 带优先级的任务提交
template<class F, class... Args>
auto ThreadPool::enqueueWithPriority(TaskPriority priority, std::chrono::milliseconds timeout, 
  F&& f, Args&&... args)
  -> std::future<typename std::invoke_result<F, Args...>::type> {
    
    //复用端口
    return enqueueWithInfo("", "", priority, timeout, std::forward<F>(f), std::forward<Args>(args)...);
}

// 带ID和描述得任务提交
template<class F, class... Args>
auto ThreadPool::enqueueWithInfo(std::string taskId, std::string description,
                    TaskPriority priority, std::chrono::milliseconds timeout, F&& f, Args&&... args)
  -> std::future<typename std::invoke_result<F, Args...>::type> {
  
  using return_type = typename std::invoke_result<F, Args...>::type;

  //在锁之外创建promise
  auto promise = std::make_shared<std::promise<return_type>>();
  std::future<return_type> result = promise->get_future();
  
  std::function<void()> taskFunction;

  if(timeout.count() > 0) {
    taskFunction = createTaskWithTimeoutHandling(promise, timeout, std::forward<F>(f),
                                                std::forward<Args>(args)...);
  } else {
    taskFunction = createSimpleTask(promise, std::forward<F>(f), std::forward<Args>(args)...);
  }
  

  {
    std::unique_lock<std::mutex> lock(queue_mutex);

    if(stop) {
      throw std::runtime_error("enqueue on stopped ThreadPool");
    }

    //检查任务ID是否存在 任务是否唯一(可以通过map设置某些任务唯一)
    if(!taskId.empty() && taskIdMap.find(taskId) != taskIdMap.end()) {
      throw std::runtime_error("Task ID " + taskId + " already exists");
    }
    
    //记录任务提交日志
    logTaskSubmission(taskId, description, priority);

    auto taskInfoPtr = std::make_shared<TaskInfo>(
      std::move(taskFunction),
      priority,
      taskId,
      description,
      timeout
    );
    //把唯一任务的共享指针添加到map里面
    if(!taskId.empty()) {
      taskIdMap[taskId] = taskInfoPtr;
    }
    tasks.push(*taskInfoPtr);

    //更新性能指标
    metrics.totalTasks++;
    metrics.updateQueueSize(tasks.size());

  }
  condition.notify_one();
  return result;
}

// 批量提交任务（可选超时参数）
template<class F>
std::vector<std::future<void>> ThreadPool::enqueueMany(const std::vector<F>& tasks,
  TaskPriority priority, std::chrono::milliseconds timeout) {
  std::vector<std::future<void>> futures;
  futures.reserve(tasks.size());

  for (const auto& task : tasks) {
      // 对于每个已经绑定了所有参数的任务对象，直接提交
      futures.push_back(enqueueWithInfo("", "", priority, timeout, task));
  }

  return futures;  // 返回future集合，允许调用者等待任务完成
}

// 带任务ID前缀的批量提交任务（可选超时参数）
template<class F>
std::vector<std::future<void>> ThreadPool::enqueueManyWithIdPrefix(const std::string& idPrefix,
  const std::string& descriptionPrefix,
  const std::vector<F>& tasks,
  TaskPriority priority, std::chrono::milliseconds timeout) {

  std::vector<std::future<void>> futures;
  futures.reserve(tasks.size());

  for (size_t i = 0; i < tasks.size(); ++i) {
      std::string taskId = idPrefix + "-" + std::to_string(i);
      std::string description = descriptionPrefix + " " + std::to_string(i);
      futures.push_back(enqueueWithInfo(taskId, description, priority, timeout, tasks[i]));
  }

  return futures;  // 返回future集合，允许调用者等待任务完成
}


#endif