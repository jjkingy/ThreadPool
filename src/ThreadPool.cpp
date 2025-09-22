#include "ThreadPool.h"
#include <iostream>

ThreadPool::ThreadPool(size_t threads) {
    std::cout << "线程池构造函数被调用，创建线程数: " << threads << std::endl;
    for(size_t i = 0; i < threads; ++i) {
        workers.emplace_back(
            [this, i]() { this->workerThread(i);});
    }

    std::cout << "所有线程创建完毕" << std::endl;
}

ThreadPool::~ThreadPool() {
    std::cout << "线程池析构函数被调用" << std::endl;
    
    {   //设置stop变量为ture
        std::unique_lock<std::mutex> lock(queue_mutex);
        stop = true;
    }

    condition.notify_all();

    for(std::thread& worker : workers){
        if(worker.joinable()) {
            worker.join();
        }
    }

    std::cout << "线程池已经析构" << std::endl;
}

//需要检查自己是否能退出
//现在每一个worker有一个唯一id 便于管理
void ThreadPool::workerThread(size_t id) {

    //无限循环运行
    while(true) {
        std::function<void()> task;

        //获取任务
        {
            std::unique_lock<std::mutex> lock(this->queue_mutex);
            
            condition.wait(lock, [this, id]() {
                return this->stop ||    //线程池停止
                    (!this->paused && !this->tasks.empty()) ||    //线程有任务要执行
                    (this->threadsToStop.find(id) != threadsToStop.end());  //线程池要清理线程
            });

            //停止 > 中止 > 有任务
            if(this->stop) {
                return;
            }

            if(this->threadsToStop.find(id) != this->threadsToStop.end()) {
                this->threadsToStop.erase(id);
                return;
            }
            
            if(!this->tasks.empty() && !this->paused) {
                task = std::move(tasks.front());
                tasks.pop();
            }

        }

        // 执行任务并处理异常
        if(task) {
            ++activeThreads;  // 增加活跃线程计数
            try {
                task();
                ++completedTasks;
            } catch(const std::exception& e) {
                std::cerr << "异常发生在任务中: " << e.what() << std::endl;
                ++failedTasks;
            } catch(...) {
                std::cerr << "未知异常发生在任务中" << std::endl;
                ++failedTasks;
            }
            --activeThreads;   // 减少活跃线程计数
            waitCondition.notify_all(); //这里任务完成可能出发waitForAll条件 所以要通知所有等待waitcondition的线程
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
    return completedTasks;
}

size_t ThreadPool::getActiveThreadCount() const {
    return activeThreads;
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
        return (tasks.empty() && activeThreads == 0) || stop;
    });
    std::cout << "所有任务已完成" << std::endl;
}

//一个非常巧妙清空STL容器的方法
//用一个空的容器做置换 快速move并且可以返还内存 还能把析构放在锁之外完成 提升速度
void ThreadPool::clearTasks() {
    std::unique_lock<std::mutex> lock(queue_mutex);
    size_t taskCount = tasks.size();

    std::queue<std::function<void()>> emptyQueue;
    std::swap(tasks, emptyQueue);

    std::cout << "清空任务队列: " << taskCount << " 个任务被移除" << std::endl;
}

size_t ThreadPool::getFailedTaskCount() const {
    return failedTasks;
}