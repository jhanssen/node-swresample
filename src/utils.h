#ifndef UTILS_H
#define UTILS_H

#include <nan.h>
#include <queue>
#include <errno.h>

#define EINTRWRAP(var, op)                      \
    do {                                        \
        var = op;                               \
    } while (var == -1 && errno == EINTR);

class Condition;

class Mutex
{
public:
    Mutex()
    {
        uv_mutex_init(&mMutex);
    }
    ~Mutex()
    {
        uv_mutex_destroy(&mMutex);
    }

    void lock()
    {
        uv_mutex_lock(&mMutex);
    }
    void unlock()
    {
        uv_mutex_unlock(&mMutex);
    }

private:
    uv_mutex_t mMutex;

    friend class Condition;
};

class MutexLocker
{
public:
    MutexLocker(Mutex* m)
        : mMutex(m)
    {
        mMutex->lock();
    }

    ~MutexLocker()
    {
        mMutex->unlock();
    }

private:
    Mutex* mMutex;
};

class Condition
{
public:
    Condition()
    {
        uv_cond_init(&mCond);
    }

    ~Condition()
    {
        uv_cond_destroy(&mCond);
    }

    void wait(Mutex* mutex)
    {
        uv_cond_wait(&mCond, &mutex->mMutex);
    }

    int waitUntil(Mutex* mutex, uint64_t timeout)
    {
        return uv_cond_timedwait(&mCond, &mutex->mMutex, timeout);
    }

    void signal()
    {
        uv_cond_signal(&mCond);
    }

    void broadcast()
    {
        uv_cond_broadcast(&mCond);
    }

private:
    uv_cond_t mCond;
};

template<typename T>
class Queue
{
public:
    Queue()
    {
    }

    ~Queue()
    {
    }

    void push(T&& t)
    {
        MutexLocker locker(&mMutex);
        mContainer.push(std::forward<T>(t));
    }

    T pop(bool* ok = 0)
    {
        MutexLocker locker(&mMutex);
        if (!mContainer.empty()) {
            if (ok)
                *ok = true;
            const T t = std::move(mContainer.front());
            mContainer.pop();
            return t;
        } else {
            if (ok)
                *ok = false;
            return T();
        }
    }

private:
    Mutex mMutex;
    std::queue<T> mContainer;
};

template<typename T>
class WaitQueue
{
public:
    WaitQueue()
    {
    }

    ~WaitQueue()
    {
    }

    void push(T&& t)
    {
        MutexLocker locker(&mMutex);
        mContainer.push(std::forward<T>(t));
        // assume only one consumer
        mCondition.signal();
    }

    T wait()
    {
        MutexLocker locker(&mMutex);
        while (mContainer.empty()) {
            mCondition.wait(&mMutex);
        }
        const T t = std::move(mContainer.front());
        mContainer.pop();
        return t;
    }

    T waitUntil(uint64_t timeout, bool* ok = nullptr)
    {
        MutexLocker locker(&mMutex);
        if (mContainer.empty()) {
            mCondition.waitUntil(&mMutex, timeout);
        }
        if (mContainer.empty()) {
            // assume timeout
            if (ok)
                *ok = false;
            return T();
        }
        if (ok)
            *ok = true;
        const T t = std::move(mContainer.front());
        mContainer.pop();
        return t;
    }

private:
    Mutex mMutex;
    Condition mCondition;
    std::queue<T> mContainer;
};

#endif
