#include <chrono>
#include <coroutine>
#include <deque>
#include <queue>
#include <thread>
#include <debug.hpp>

using namespace std::chrono_literals;

struct RepeatAwaiter // awaiter(原始指针) / awaitable(operator->)
{
    bool await_ready() const noexcept { return false; }
    // 销毁操作，return true说明协程结果已经得到，不需要执行
    // 结果一般都是false（肯定不销毁啦）

    std::coroutine_handle<> await_suspend(std::coroutine_handle<> coroutine) const noexcept {
        if (coroutine.done())
            return std::noop_coroutine(); // 代表不需要挂起,会立即执行
        else
            return coroutine;
    }
    // 挂起操作，传入coroutine_handle类型的参数，在函数中调用handle.resume()，就可以恢复协程

    void await_resume() const noexcept {}
    // 恢复操作，返回值就是co_await的返回值
};

struct PreviousAwaiter {
    std::coroutine_handle<> mPrevious;

    bool await_ready() const noexcept { return false; }

    std::coroutine_handle<> await_suspend(std::coroutine_handle<> coroutine) const noexcept {
        // 等待mPrevious,不为空则移交控制权
        if (mPrevious){
            return mPrevious;
        }else{
            return std::noop_coroutine();
        }
    }

    void await_resume() const noexcept {}
};

template <class T>
struct Promise {
    // 开始挂起
    // 表达式恢复（无论是立即还是异步）时
    // 协程开始执行你编写的协程体语句。
    auto initial_suspend() noexcept {
        return std::suspend_always();
    }
    // 结束挂起
    auto final_suspend() noexcept {
        return PreviousAwaiter(mPrevious);
    }
    // 句柄中错误
    // 如果执行离开 body-statements 是由于未处理的异常，则：
    //1. 捕获异常并在catch块内调用promise.unhandled_exception()
    //2. 调用promise.final_suspend()并co_await结果。 
    void unhandled_exception() noexcept {
        mException = std::current_exception();
    }
    // co_yeild value 的调用
    auto yield_value(T ret) noexcept {
        new(&mResult) T(std::move(ret));
        //定位new表达式（placement new）
        //它允许你在已经分配的内存上直接构造对象
        //手动构造一个类型为 T 的对象
        //并将其放置在 mResult 所指向的内存地址上
        return std::suspend_always();
    }
    // co_return value 的调用
    void return_value(T ret) noexcept {
        new(&mResult) T(std::move(ret));
    }

    T ReturnResult() {
        if(mException) [[unlikely]] {
            std::rethrow_exception(mException);
        }
        T ret = std::move(mResult);
        // 手动调用 T 类型对象的析构函数,Union需要显式析构
        mResult.~T();
        return ret;
    }
    // 获取当协程首次挂起时返回给调用者的结果
    // 将结果保存为局部变量
    std::coroutine_handle<Promise> get_return_object() {
        return std::coroutine_handle<Promise>::from_promise(*this);
    }
    // 防止对象初始化,但需要通过mResult.~T()的方式手动释放内存

    std::coroutine_handle<> mPrevious{};
    std::exception_ptr mException{};
    union {
        // 注意U别大写hh
        T mResult;
    };

    Promise() noexcept {}
    Promise(Promise &&) = delete;
    ~Promise() {}
};

// void类型不能被构造或赋值，需要模板特化
template <>
struct Promise<void> {
    auto initial_suspend() noexcept {
        return std::suspend_always();
    }

    auto final_suspend() noexcept {
        return PreviousAwaiter(mPrevious);
    }

    void unhandled_exception() noexcept {
        mException = std::current_exception();
    }

    void return_void() noexcept {
    }

    void ReturnResult() {
        if (mException) [[unlikely]] {
            std::rethrow_exception(mException);
        }
    }

    std::coroutine_handle<Promise> get_return_object() {
        return std::coroutine_handle<Promise>::from_promise(*this);
    }

    std::coroutine_handle<> mPrevious{};
    std::exception_ptr mException{};

    Promise() = default;// 相比于重写默认构造函数效率更高
    Promise(Promise &&) = delete;
    ~Promise() = default;
    //保持了类的平凡性（triviality）和标准布局（standard layout）
    //平凡的类型通常可以安全地进行内存复制操作
    //如memcpy，并且它们的对象在内存中的布局与C语言中的结构体兼容。
    //类型如果是标准布局的，它的内存布局将与C语言中的结构体相同
};

template <class T = void>
struct Task {
    using promise_type = Promise<T>;

    Task(std::coroutine_handle<promise_type> coroutine) noexcept
        : mCoroutine(coroutine) {}
    // 删除移动构造函数，防止非预期复制
    Task(Task &&) = delete;
    // 析构时，保证协程资源释放
    ~Task() {
        mCoroutine.destroy();
    }

    struct Awaiter {
        bool await_ready() const noexcept { return false; }
        // 类型安全的，因为只接受promise_type类型的Promise对象
        std::coroutine_handle<promise_type> await_suspend(std::coroutine_handle<> coroutine) const noexcept {
            mCoroutine.promise().mPrevious = coroutine;
            return mCoroutine;
        }

        T await_resume() const {
            return mCoroutine.promise().ReturnResult();
        }

        std::coroutine_handle<promise_type> mCoroutine;
    };

    Awaiter operator co_await() const noexcept {
        return Awaiter(mCoroutine);
    }
    // 允许 Task 对象被隐式转换为 std::coroutine_handle<>
    operator std::coroutine_handle<>() const noexcept {
        return mCoroutine;
    }

    std::coroutine_handle<promise_type> mCoroutine;
};
// 调度器
struct Loop{
    // 就绪队列，存储句柄
    std::deque<std::coroutine_handle<>> mReadyQueue;
    // 时间表项
    struct TimerEntry{
        // 过期时间点
        std::chrono::system_clock::time_point expireTime;
        // 目标协程句柄
        std::coroutine_handle<> coroutine;
        // 重载比较"<"运算符
        bool operator<(TimerEntry const &that) const noexcept {
            return expireTime > that.expireTime;
        }
    };
    // 优先队列（大顶堆）
    std::priority_queue<TimerEntry> mTimerHeap;
    // 加入任务队列
    void addTask(std::coroutine_handle<> coroutine) {
        mReadyQueue.push_front(coroutine);
    }
    // 时间点，哪个协程
    void addTimer(std::chrono::system_clock::time_point expireTime, std::coroutine_handle<> coroutine) {
        mTimerHeap.push({expireTime, coroutine});
    }
    void runAll() {
        while (!mTimerHeap.empty()||!mReadyQueue.empty()) {
            while (!mReadyQueue.empty()) {
                // 就绪队列非空则取出恢复
                std::coroutine_handle<> coroutine = mReadyQueue.front();
                mReadyQueue.pop_front();
                coroutine.resume();
            }
            // 堆顶不空且时间点小于当前时间点则出堆，否则等待到该时间
            // 因为堆顶时间最晚，所以如果该协程被恢复，则说明全部协程都可以被恢复
            if (!mTimerHeap.empty()) {
                std::chrono::system_clock::time_point nowTime = std::chrono::system_clock::now();
                TimerEntry timer = std::move(mTimerHeap.top());
                if (timer.expireTime < nowTime) {
                    mTimerHeap.pop();
                    timer.coroutine.resume();
                } else {
                    std::this_thread::sleep_until(timer.expireTime);
                }
            }
        }
    }
    // 删除除了构造函数外的所有函数
    // 如果是Loop (Loop &&) = delete 就是默认删掉五个函数
    Loop &operator=(Loop &&) = delete;
};

Loop &getLoop() {
    // 静态全局返回，单例模式
    // 多线程安全，不会构造两次
    static Loop loop;
    return loop;
}

struct SleepAwaiter {
    bool await_ready() const noexcept {
        return false;
    }

    void await_suspend(std::coroutine_handle<> coroutine) const {
        getLoop().addTimer(expireTime, coroutine);
    }

    void await_resume() const noexcept {
    }

    std::chrono::system_clock::time_point expireTime;
};

// 睡眠到什么时间点
Task<void> sleep_until(std::chrono::system_clock::time_point expireTime) {
    co_await SleepAwaiter(expireTime);
    co_return;
}

// 睡眠一段时间
Task<void> sleep_for(std::chrono::system_clock::duration duration) {
    // 时间点加时间段等于时间点
    co_await SleepAwaiter(std::chrono::system_clock::now() + duration);
    co_return;
}

Task<int> hello1() {
    debug(), "hello1开始睡1秒";
    co_await sleep_for(1s); // 1s 等价于 std::chrono::seconds(1)
    debug(), "hello1睡醒了";
    co_return 1;
}

Task<int> hello2() {
    debug(), "hello2开始睡2秒";
    co_await sleep_for(2s); // 2s 等价于 std::chrono::seconds(2)
    debug(), "hello2睡醒了";
    co_return 2;
}

int main() {
    auto t1 = hello1();
    auto t2 = hello2();
    getLoop().addTask(t1);// 加入任务队列
    getLoop().addTask(t2);
    getLoop().runAll();
    debug(), "主函数中得到hello1结果:", t1.mCoroutine.promise().ReturnResult();
    debug(), "主函数中得到hello2结果:", t2.mCoroutine.promise().ReturnResult();
    return 0;
}
