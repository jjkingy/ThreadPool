#ifndef THREAD_POOL_INL
#define THREAD_POOL_INL

// 提交任务到线程池
template<class F, class... Args>
auto ThreadPool::enqueue(F&& f, Args&&... args)
  -> std::future<typename std::invoke_result<F, Args...>::type> {
  return enqueueWithPriority(TaskPriority::MEDIUM,
      std::forward<F>(f),
      std::forward<Args>(args)...);
}


// 打包任务函数
template<class F, class... Args>
auto ThreadPool::createTaskFunction(std::shared_ptr<std::promise<typename std::invoke_result<F, Args...>::type>> promise,
                        F&& f, Args&&... args)
  ->std::function<void()> {
    using return_type = typename std::invoke_result<F, Args...>::type;

    //返回一个lambda lambda不能直接捕获可变参数包
    //tuple是一个可变参数模板，存储任意类型任意数量得值
    return [promise, f = std::forward<F>(f), 
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
      catch(const std::exception&) {
        promise->set_exception(std::current_exception());
        throw;
      }
      catch(...) {
        //处理其他类型的异常
        promise->set_exception(std::current_exception());
        throw;
      }

    };
}

// 带优先级的任务提交
template<class F, class... Args>
auto ThreadPool::enqueueWithPriority(TaskPriority priority, F&& f, Args&&... args)
  -> std::future<typename std::invoke_result<F, Args...>::type> {
    
    //复用端口
    return enqueueWithInfo("", "", priority, std::forward<F>(f), std::forward<Args>(args)...);
}

// 带ID和描述得任务提交
template<class F, class... Args>
auto ThreadPool::enqueueWithInfo(std::string taskId, std::string description,
                    TaskPriority priority, F&& f, Args&&... args)
  -> std::future<typename std::invoke_result<F, Args...>::type> {
  
  using return_type = typename std::invoke_result<F, Args...>::type;

  //在锁之外创建promise
  auto promise = std::make_shared<std::promise<return_type>>();
  std::future<return_type> result = promise->get_future();

  auto taskFunction = createTaskFunction(promise, std::forward<F>(f), std::forward<Args>(args)...);

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
      description
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





#endif