#include "ThreadPool.h"
#include <iostream>

// 构造函数
ThreadPool::ThreadPool(size_t threads, LogLevel logLevel, bool consoleLog, const std::string& logFile)
    : maxThreads(std::max(threads * 2, static_cast<size_t>(std::thread::hardware_concurrency())))
    , logger(logLevel, consoleLog, logFile) {

    // 确保初始线程数不超过最大线程数
    threads = std::min(threads, maxThreads);
    logger.log(LogLevel::INFO, "线程池创建，工作线程数: " + std::to_string(threads) +
        ", 最大线程数: " + std::to_string(maxThreads));

    for(size_t i = 0; i < threads; ++i) {
        workers.emplace_back(
            [this, i]() { this->workerThread(i);});
    }
}

ThreadPool::~ThreadPool() {

    {   //stop是atomic变量 为什么这里还要加锁？
        //此时mutex不是保护stop 而是为了保护condition.wait逻辑完成性
        //在condition.wait中 条件检查和进入等待之间不是原子操作
        //若不加锁 则可能出现有thread错过notify_all通知从而永远等待
        std::unique_lock<std::mutex> lock(queue_mutex);
        stop = true;
    }
    logger.log(LogLevel::INFO, "线程池正在关闭...");

    condition.notify_all();

    for(std::thread& worker : workers){
        if(worker.joinable()) {
            worker.join();
        }
    }

    logger.log(LogLevel::INFO, "线程池关闭");
}

// 设置最大线程数
void ThreadPool::setMaxThreads(size_t max) {
    std::unique_lock<std::mutex> lock(queue_mutex);

    // 不允许设置小于当前线程数的最大线程数
    if (max < workers.size()) {
        throw std::runtime_error("Cannot set max threads less than current thread count");
    }

    maxThreads = max;
    logger.log(LogLevel::INFO, "设置最大线程数: " + std::to_string(maxThreads));
}

// 获取最大线程数
size_t ThreadPool::getMaxThreads() const {
    return maxThreads;
}

//需要检查自己是否能退出
//现在每一个worker有一个唯一id 便于管理
void ThreadPool::workerThread(size_t id) {
    logger.log(LogLevel::DEBUG, "工作线程 " + std::to_string(id) + "启动");

    //无限循环运行
    while(true) {
        // std::function<void()> task;
        // TaskInfo task{nullptr};
        std::shared_ptr<TaskInfo> taskPtr = nullptr;
        TaskFetchResult result = getNextTask(id, taskPtr);
        
        switch (result) {
            case TaskFetchResult::SHOULD_EXIT: return;
            case TaskFetchResult::NO_TASK: continue;    //继续运行
            case TaskFetchResult::HAS_TASK:
                if(taskPtr && taskPtr->task) {
                    executeTask(id, taskPtr);
                }
                break;
        }
    }
}

TaskFetchResult ThreadPool::getNextTask(size_t id, std::shared_ptr<TaskInfo>& taskPtr) {
    std::unique_lock<std::mutex> lock(this->queue_mutex);

    condition.wait(lock, [this, id]() {
        return this->stop ||    //线程池停止
            (!this->paused && !this->tasks.empty()) ||    //线程有任务要执行
            (this->threadsToStop.find(id) != threadsToStop.end());  //线程池要清理该线程
    });

    //停止 > 中止 > 有任务
    if(this->stop) {
        logger.log(LogLevel::DEBUG, "工作线程 " + std::to_string(id) + " 停止(线程池关闭)");
        return TaskFetchResult::SHOULD_EXIT;
    }

    if(this->threadsToStop.find(id) != this->threadsToStop.end()) {
        this->threadsToStop.erase(id);
        logger.log(LogLevel::DEBUG, "工作线程 " + std::to_string(id) + " 停止（线程池调整大小）");
        return TaskFetchResult::SHOULD_EXIT;
    }

    bool hasTask = false;

    //优先级队列不支持move操作
    //CANCLED只能处理记录taskId的任务 
    //因为需要跳过CANCLED任务 所以这里要不断循环直到成功获取任务(不然只执行一次就睡太浪费了)
    while(!this->tasks.empty() && !this->paused) {
        TaskInfo task = this->tasks.top();
        this->tasks.pop();

        if(!task.taskId.empty()) {
            auto it = taskIdMap.find(task.taskId);
            if(it != taskIdMap.end()) {
                taskPtr = it->second;
                if(taskPtr->status == TaskStatus::CANCELED) {
                    logger.log(LogLevel::DEBUG, "跳过已经取消的任务 " + task.taskId);
                    continue;   //继续尝试获取下一个任务
                }
            } 
        } else {
            taskPtr = std::make_shared<TaskInfo>(task);
        }
        hasTask = true;
        //记录日志
        std::string taskDesc = taskPtr->taskId.empty() ? "匿名任务" : "任务" + taskPtr->taskId;
        if(!taskPtr->description.empty()) {
            taskDesc += " (" + taskPtr->description + ")";
        }
        logger.log(LogLevel::DEBUG, "工作线程" + std::to_string(id) + "开始执行 " + taskDesc);      

        break;
    }

    return hasTask ? TaskFetchResult::HAS_TASK : TaskFetchResult::NO_TASK;
}

void ThreadPool::executeTask(size_t id, std::shared_ptr<TaskInfo> taskPtr) {
    ++metrics.activeThreads;  // 增加活跃线程计数
    taskPtr->status = TaskStatus::RUNNING;
    metrics.updateActiveThreads(metrics.activeThreads);

    auto startTime = std::chrono::steady_clock::now();
    //增加超时机制 主线程监督子线程执行

    try {
        taskPtr->task();

        //到这里说明已经完成了
        taskPtr->status = TaskStatus::COMPLETED;
        metrics.completedTasks++;

    } catch(const std::exception& e) {
        taskPtr->status = TaskStatus::FAILED;
        taskPtr->errorMessage = e.what();
        logger.log(LogLevel::DEBUG, "工作线程 " + std::to_string(id) + "处理任务完成: "
                    + taskStatusToString(taskPtr->status));
    } catch(...) {
        taskPtr->status = TaskStatus::FAILED;
        taskPtr->errorMessage = "未知异常";
        logger.log(LogLevel::DEBUG, "工作线程 " + std::to_string(id) + "处理任务完成: "
                    + taskStatusToString(taskPtr->status));

    }

    auto endTime = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(endTime - startTime);
    metrics.addTaskTime(duration.count());

    --metrics.activeThreads;   // 减少活跃线程计数
    waitCondition.notify_all();
    cleanupTask(taskPtr);
    logTaskCompletion(id, taskPtr, duration);

    /*这里是严重bug 任务超时以后只是捕获异常，并没有结束运行
    如果此时任务执行还没有结束 就移除任务 后面promise无法真却被设置*/
    //这里任务已经执行完了 从taskIdMap中移除任务
    // {
    //     std::lock_guard<std::mutex> lock(queue_mutex);
    //     if(!taskPtr->taskId.empty()) {
    //         taskIdMap.erase(taskPtr->taskId);
    //     }
    // }

    // //记录日志
    // std::string taskDesc = taskPtr->taskId.empty() ? "匿名任务" : "任务 " + taskPtr->taskId;
    // std::string statusStr = taskStatusToString(taskPtr->status);
    
    // logger.log(LogLevel::DEBUG,
    //         "工作线程" + std::to_string(id) + " " + statusStr + "" + taskDesc +
    //         "(用时 " + std::to_string(duration.count() / 1000000.0) + "ms");
}


void ThreadPool::recordTaskFailure(const std::string& errorMessage, bool isTimeout) {
    if (isTimeout) {
        metrics.timeOutTasks++;
        logger.log(LogLevel::ERROR, "任务超时: " + errorMessage);
    } else {
        metrics.failedTasks++;
        logger.log(LogLevel::ERROR, "任务异常: " + errorMessage);
    }
}

size_t ThreadPool::getThreadCount() const {
    return workers.size();
}

size_t ThreadPool::getTaskCount() {
    std::unique_lock<std::mutex> lock(queue_mutex);
    return tasks.size();
}

size_t ThreadPool::getCompletedTaskCount() const {
    return metrics.completedTasks;
}

size_t ThreadPool::getActiveThreadCount() const {
    return metrics.activeThreads;
}

size_t ThreadPool::getWaitingThreadCount() const {
    size_t total = getThreadCount();
    size_t active = getActiveThreadCount();
    return total - active;
}


//动态调整线程池的大小 使用unordered_set管理需要停止的线程ID
void ThreadPool::resize(size_t threads) {
    std::unique_lock<std::mutex> lock(queue_mutex);

    if(stop){
        throw std::runtime_error("resize on stopped ThreadPool");
    }
    
    threads = std::min(threads, maxThreads);

    //分线程增大与线程池减小两种情况
    size_t oldSize = workers.size();

    logger.log(LogLevel::INFO, "调整线程池大小: " + std::to_string(oldSize) +
        " -> " + std::to_string(threads) +
        " (最大: " + std::to_string(maxThreads) + ")");

    if(threads > oldSize){
        workers.reserve(threads);
        for(size_t i = oldSize; i < threads; ++i) {
            workers.emplace_back([this, i]() {  this->workerThread(i);  });
        }
        std::cout << "增加了 " << (threads - oldSize)<< "个工作线程" << std::endl;

    } else if(threads < oldSize){
        threadsToStop.clear();  //清空之前的集合
        for(size_t i = threads; i < oldSize; ++i){
            threadsToStop.insert(i);
        }

        //释放锁 通知所有线程需要清空部分线程
        lock.unlock();
        condition.notify_all();

        //等待清空线程运行结束
        for(size_t i = threads; i < oldSize; ++i) {
            if(workers[i].joinable()) {
                workers[i].join();
            }
        }

        //重新获取并调整大小
        lock.lock();
        workers.resize(threads);
        std::cout << "减少了 " << oldSize - threads << " 个工作线程" << std::endl;
    }

}

//条件变量会影响线程condition等待 无需主动调整
void ThreadPool::pause() {
    std::unique_lock<std::mutex> lock(queue_mutex);
    paused = true;
    std::cout << "线程池已停止" << std::endl;
}

void ThreadPool::resume() {
    {
        std::unique_lock<std::mutex> lock(queue_mutex);
        paused = false;
        std::cout << "线程池已恢复" << std::endl;
    }
    //唤醒所有线程 通知他们线程池要恢复了
    condition.notify_all();
}

//使用条件变量condition_wait
void ThreadPool::waitForTasks() {
    std::unique_lock<std::mutex> lock(queue_mutex);
    std::cout << "等待所有任务完成...." << std::endl;
    waitCondition.wait(lock, [this]() {
        //任务队列空 并且所有正在完成的任务都完成
        return (tasks.empty() && metrics.activeThreads == 0) || stop;
    });
    std::cout << "所有任务已完成" << std::endl;
}

//一个非常巧妙清空STL容器的方法
//用一个空的容器做置换 快速move并且可以返还内存 还能把析构放在锁之外完成 提升速度
void ThreadPool::clearTasks() {
    std::unique_lock<std::mutex> lock(queue_mutex);
    size_t taskCount = tasks.size();

    //清空任务队列和ID映射表
    std::priority_queue<TaskInfo> emptyQueue;
    std::swap(tasks, emptyQueue);
    taskIdMap.clear();

    logger.log(LogLevel::INFO, "清空任务队列: " + std::to_string(taskCount) + " 个任务被移除");
}

size_t ThreadPool::getFailedTaskCount() const {
    return metrics.failedTasks;
}

// 获取性能报告
std::string ThreadPool::getMetricsReport() const {
    return metrics.getReport();
}

// 设置日志级别
void ThreadPool::setLogLevel(LogLevel level) {
    logger.setLevel(level);
}

// 记录任务提交日志
void ThreadPool::logTaskSubmission(const std::string& taskId, const std::string& description,
                                   TaskPriority priority) {
    std::string priorityStr = priorityToString(priority);
    
    if (!taskId.empty() || !description.empty()) {
        logger.log(LogLevel::DEBUG, "提交任务 " + taskId + " (" + description + 
                   ") 优先级: " + priorityStr);
    } else {
        logger.log(LogLevel::DEBUG, "提交" + priorityStr + "优先级任务");
    }
}

//只能取消等待中的任务
bool ThreadPool::cancelTask(const std::string& taskId) {
    std::unique_lock<std::mutex> lock(queue_mutex);

    auto it = taskIdMap.find(taskId);
    if(it == taskIdMap.end()) {
        logger.log(LogLevel::ERROR, "尝试取消不存在的任务 " + taskId);
        return false;
    }

    auto& taskInfoPtr = it->second;
    if(taskInfoPtr->status == TaskStatus::RUNNING) {
        logger.log(LogLevel::ERROR, "无法取消正在执行的任务 " + taskId);
        return false;
    }
    if(taskInfoPtr->status == TaskStatus::COMPLETED ||
        taskInfoPtr->status == TaskStatus::CANCELED ||
        taskInfoPtr->status == TaskStatus::FAILED) {
        logger.log(LogLevel::ERROR, "任务 " + taskId + " 已经终止: " + 
                    taskStatusToString(taskInfoPtr->status));
        return false;
    }

    taskInfoPtr->status = TaskStatus::CANCELED;
    logger.log(LogLevel::INFO, "成功取消任务 " + taskId);
    //不会直接从工作队列中移除 只更新状态
    //worker遇到CANCLED状态任务会直接跳过
    return true;
}

// 清理任务
void ThreadPool::cleanupTask(std::shared_ptr<TaskInfo> taskPtr) {
    std::lock_guard<std::mutex> lock(queue_mutex);
    if (!taskPtr->taskId.empty()) {
        taskIdMap.erase(taskPtr->taskId);
    }
    waitCondition.notify_all();
}

// 记录任务完成日志
void ThreadPool::logTaskCompletion(size_t id, std::shared_ptr<TaskInfo> taskPtr, const std::chrono::nanoseconds& duration) {
    std::string taskDesc = taskPtr->taskId.empty() ? "匿名任务" : "任务 " + taskPtr->taskId;
    std::string statusStr = taskStatusToString(taskPtr->status);

    logger.log(LogLevel::DEBUG,
        "工作线程 " + std::to_string(id) + " " + statusStr + " " + taskDesc +
        " (用时: " + std::to_string(duration.count() / 1000000.0) + "ms)");
}


TaskStatus ThreadPool::getTaskStatus(const std::string& taskId)
 {
    std::lock_guard<std::mutex> lock(queue_mutex);
    auto it = taskIdMap.find(taskId);
    if(it != taskIdMap.end()) {
        return it->second->status;
    }
    return TaskStatus::NOT_FOUND;
 }

// 获取任务状态字符串
std::string ThreadPool::getTaskStatusString(const std::string& taskId) {
    return taskStatusToString(getTaskStatus(taskId));
}