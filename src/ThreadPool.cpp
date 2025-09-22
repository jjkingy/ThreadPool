#include "ThreadPool.h"
#include <iostream>

// 构造函数 - 第一天只是简单的占位实现
ThreadPool::ThreadPool(size_t threads) {
    std::cout << "线程池构造函数被调用，创建线程数: " << threads << std::endl;
    for(size_t i = 0; i < threads; ++i) {
        workers.emplace_back(
            [this]() { this->workerThread();});
    }

    std::cout << "所有线程创建完毕" << std::endl;
}

// 析构函数 - 第一天只是简单的占位实现
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

void ThreadPool::workerThread() {
    //无限循环运行
    while(true) {
        std::function<void()> task;

        //获取任务
        {
            std::unique_lock<std::mutex> lock(this->queue_mutex);
            
            condition.wait(lock, [this]() {
                return this->stop || !this->tasks.empty();
            });

            //如果线程池停止或者任务队列为空则可以退出线程
            if(this->stop || this->tasks.empty()){
                return;
            }
            
            if(!this->tasks.empty()) {
                task = std::move(tasks.front());
                tasks.pop();
            }

        }

        //执行任务
        if(task) {
            ++activeThreads;
            try {
                task();
            } catch(...) {
                //确保发生异常不会终止程序
            }
            //无论成功还是失败都计数
            ++completedTasks;
            --activeThreads;
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