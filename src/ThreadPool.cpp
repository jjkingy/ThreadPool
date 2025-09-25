#include "ThreadPool.h"
#include <iostream>

ThreadPool::ThreadPool(size_t threads, LogLevel loglevel, bool consoleLog, const std::string& logFile) 
    : logger(loglevel, consoleLog, logFile) {
    logger.log(LogLevel::INFO, "线程池创建,工作线程数 " + std::to_string(threads));
    for(size_t i = 0; i < threads; ++i) {
        workers.emplace_back(
            [this, i]() { this->workerThread(i);});
    }
}

ThreadPool::~ThreadPool() {

    {   //设置stop变量为ture
        std::unique_lock<std::mutex> lock(queue_mutex);
        stop = true;
        logger.log(LogLevel::INFO, "线程池正在关闭...");
    }

    condition.notify_all();

    for(std::thread& worker : workers){
        if(worker.joinable()) {
            worker.join();
        }
    }

    logger.log(LogLevel::INFO, "线程池关闭");
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
        bool hasTask = false;

        //获取任务
        {
            std::unique_lock<std::mutex> lock(this->queue_mutex);
            
            condition.wait(lock, [this, id]() {
                return this->stop ||    //线程池停止
                    (!this->paused && !this->tasks.empty()) ||    //线程有任务要执行
                    (this->threadsToStop.find(id) != threadsToStop.end());  //线程池要清理该线程
            });

            //停止 > 中止 > 有任务
            if(this->stop) {
                logger.log(LogLevel::DEBUG, "工作线程 " + std::to_string(id) + " 停止(线程池关闭)");
                return;
            }

            if(this->threadsToStop.find(id) != this->threadsToStop.end()) {
                this->threadsToStop.erase(id);
                logger.log(LogLevel::DEBUG, "工作线程 " + std::to_string(id) + " 停止（线程池调整大小）");
                return;
            }
            
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
                break;
            }

            if(hasTask && taskPtr) {
                std::string taskDesc = taskPtr->taskId.empty() ? "匿名任务" : "任务" + taskPtr->taskId;
                if(!taskPtr->description.empty()) {
                    taskDesc += " (" + taskPtr->description + ")";
                }
                logger.log(LogLevel::DEBUG, "工作线程" + std::to_string(id) + "开始执行 " + taskDesc);                   
            }
        }

        // 执行任务并处理异常
        if(hasTask && taskPtr && taskPtr->task) {
            ++metrics.activeThreads;  // 增加活跃线程计数
            taskPtr->status = TaskStatus::RUNNING;
            metrics.updateActiveThreads(metrics.activeThreads);

            auto startTime = std::chrono::steady_clock::now();
            try {
                taskPtr->task();
                taskPtr->status = TaskStatus::COMPLETED;
                ++metrics.completedTasks;
            } catch(const std::exception& e) {
                taskPtr->status = TaskStatus::FAILED;
                taskPtr->errorMessage = e.what();
                ++metrics.failedTasks;
                logger.log(LogLevel::ERROR, "任务异常: " + std::string(e.what()));
            } catch(...) {
                taskPtr->status = TaskStatus::FAILED;
                taskPtr->errorMessage = "未知异常";
                metrics.failedTasks++;
                logger.log(LogLevel::ERROR, "任务发生未知异常");
            }

            auto endTime = std::chrono::steady_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(endTime - startTime);
            metrics.addTaskTime(duration.count());

            --metrics.activeThreads;   // 减少活跃线程计数
            //这里任务已经执行完了 从taskIdMap中移除任务
            {
                std::lock_guard<std::mutex> lock(queue_mutex);
                if(!taskPtr->taskId.empty()) {
                    taskIdMap.erase(taskPtr->taskId);
                }
            }
            waitCondition.notify_all(); //这里任务完成可能出发waitForAll条件 所以要通知所有等待waitcondition的线程

            //记录日志
            std::string taskDesc = taskPtr->taskId.empty() ? "匿名任务" : "任务 " + taskPtr->taskId;
            std::string statusStr = taskStatusToString(taskPtr->status);
            
            logger.log(LogLevel::DEBUG,
                    "工作线程" + std::to_string(id) + " " + statusStr + "" + taskDesc +
                    "(用时 " + std::to_string(duration.count() / 1000000.0) + "ms");
        }

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

    //分线程增大与线程池减小两种情况
    size_t oldSize = workers.size();

    std::cout << "调整线程池大小 " << oldSize << "-> " << threads << std::endl;

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
bool ThreadPool::cancleTask(const std::string& taskId) {
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

// std::string taskStatusToString(TaskStatus status) {
//     switch (status) {
//         case TaskStatus::WAITING:   return "等待中";
//         case TaskStatus::RUNNING:   return "正在执行";
//         case TaskStatus::COMPLETED: return "已完成";
//         case TaskStatus::FAILED:    return "失败";
//         case TaskStatus::CANCELED:  return "已取消";
//         default:                    return "未知状态";
//     }
// }