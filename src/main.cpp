#include <chrono>
#include <coroutine>
#include <deque>
#include <queue>
#include <span>
#include <thread>
#include <rbtree.hpp>
#include <debug.hpp>

using namespace std::chrono_literals;


template <class T = void>
struct NonVoidHelper {
    using Type = T;
};

template <>
struct NonVoidHelper<void> {
    using Type = NonVoidHelper;

    explicit NonVoidHelper() = default;
    // 表示这个结构体有一个显式的默认构造函数
    // explicit 关键字防止了构造函数的隐式转换
    // 而 = default 表示使用编译器生成的默认构造函数。
};

// 封装未初始化的值模板
template <class T>
struct Uninitialized {
    // 不会自动调用成员mValue的构造函数来初始化
    // 因此其内存释放也需要额外管理
    union {
        T mValue;
    };

    Uninitialized() noexcept {}
    Uninitialized(Uninitialized &&) = delete;
    ~Uninitialized() noexcept {}

    // 手动调用 T 类型对象的析构函数,Union需要显式析构
    T moveValue() {
        T ret(std::move(mValue));
        mValue.~T();
        return ret;
    }

    template <class... Ts> void putValue(Ts &&...args) {
        // addressof()获取地址
        new (std::addressof(mValue)) T(std::forward<Ts>(args)...);
        //定位new表达式（placement new）
        //它允许你在已经分配的内存上直接构造对象
        //手动构造一个类型为 T 的对象
        //并将其放置在 mResult 所指向的内存地址上
        // forward<Ts>保证了参数 args 的完美转发
        // 即保持了参数的原始值类别（左值或右值）。
    }
};

template <>
struct Uninitialized<void> {
    auto moveValue() {
        return NonVoidHelper<>{};
    }

    void putValue(NonVoidHelper<>) {}
};
//特化版本，它们处理常量类型、左值引用类型和右值引用类型的情况
template <class T> struct Uninitialized<T const> : Uninitialized<T> {};

template <class T>
struct Uninitialized<T &> : Uninitialized<std::reference_wrapper<T>> {};

template <class T> struct Uninitialized<T &&> : Uninitialized<T> {};

// 自行定义了Awaiter与Awaitable 可以对其功能进行拓展
// 需要对其进行拓展的原因是RetType和NonVoidRetType
template <class A>
concept Awaiter = requires(A a, std::coroutine_handle<> h) {
    { a.await_ready() };
    { a.await_suspend(h) };
    { a.await_resume() };
};

template <class A>
concept Awaitable = Awaiter<A> || requires(A a) {
    { a.operator co_await() } -> Awaiter;
};

template <class A> struct AwaitableTraits;

template <Awaiter A> struct AwaitableTraits<A> {
    //在编译时推导出 A 类型的 await_resume 成员函数的返回类型，而不需要构造 A 类型的对象
    using RetType = decltype(std::declval<A>().await_resume());
    using NonVoidRetType = NonVoidHelper<RetType>::Type;
};

template <class A>
    requires(!Awaiter<A> && Awaitable<A>)
struct AwaitableTraits<A>
    : AwaitableTraits<decltype(std::declval<A>().operator co_await())> {};

// 协程句柄安全转换
// 将coroutine_handle<P>的协程句柄转换为coroutine_handle<To>
// 其中 P 必须是从 To 派生的类型
template <class To, std::derived_from<To> P>
constexpr std::coroutine_handle<To> staticHandleCast(std::coroutine_handle<P> coroutine) {
    return std::coroutine_handle<To>::from_address(coroutine.address());
}


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

    void return_value(T &&ret) {
        mResult.putValue(std::move(ret));
    }

        // co_return value 的调用
    void return_value(T const &ret) {
        mResult.putValue(ret);
    }

    T ReturnResult() {
        if(mException) [[unlikely]] {
            std::rethrow_exception(mException);
        }
        return mResult.moveValue();
    }
    // 获取当协程首次挂起时返回给调用者的结果
    // 将结果保存为局部变量
    std::coroutine_handle<Promise> get_return_object() {
        return std::coroutine_handle<Promise>::from_promise(*this);
    }
    // 防止对象初始化,但需要通过mResult.~T()的方式手动释放内存

    std::coroutine_handle<> mPrevious{};
    std::exception_ptr mException{};
    Uninitialized<T> mResult;

    Promise &operator=(Promise &&) = delete;
    // 删掉默认五个函数
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

    void return_void() noexcept {}

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

    Promise &operator=(Promise &&) = delete;
    // 删掉了默认五个函数
    //保持了类的平凡性（triviality）和标准布局（standard layout）
    //平凡的类型通常可以安全地进行内存复制操作
    //如memcpy，并且它们的对象在内存中的布局与C语言中的结构体兼容。
    //类型如果是标准布局的，它的内存布局将与C语言中的结构体相同
};

template <class T = void, class P = Promise<T>>
struct Task {
    using promise_type = P;

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

// 继承自红黑树，可以按照时间排列，唤醒协程
struct SleepUntilPromise : RbTree<SleepUntilPromise>::RbNode, Promise<void> {
    std::chrono::system_clock::time_point mExpireTime;

    auto get_return_object() {
        return std::coroutine_handle<SleepUntilPromise>::from_promise(*this);
    }

    SleepUntilPromise &operator=(SleepUntilPromise &&) = delete;

    friend bool operator<(SleepUntilPromise const &lhs, SleepUntilPromise const &rhs) noexcept {
        return lhs.mExpireTime < rhs.mExpireTime;
    }
};

// 调度器
struct Loop {
    // 构建红黑树，时间早的默认在前
    RbTree<SleepUntilPromise> mRbTimer{};
    // 增加结点
    void addTimer(SleepUntilPromise &promise) {
        mRbTimer.insert(promise);
    }

    void run(std::coroutine_handle<> coroutine) {
        while (!coroutine.done()) {
            // 协程未执行完时，恢复协程继续执行
            coroutine.resume();
            while (!mRbTimer.empty()) {
                // 树不为空时
                if (!mRbTimer.empty()) {
                    auto nowTime = std::chrono::system_clock::now();
                    // 获取最早的时间点
                    auto &promise = mRbTimer.front();
                    // 早于当前时间则删除结点，否则睡眠到该时间点
                    if (promise.mExpireTime < nowTime) {
                        mRbTimer.erase(promise);
                        std::coroutine_handle<SleepUntilPromise>::from_promise(promise).resume();
                    } else {
                        std::this_thread::sleep_until(promise.mExpireTime);
                    }
                }
            }
        }
    }

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

    void await_suspend(std::coroutine_handle<SleepUntilPromise> coroutine) const {
        auto &promise = coroutine.promise();
        promise.mExpireTime = mExpireTime;
        loop.addTimer(promise);
    }

    void await_resume() const noexcept {}

    Loop &loop;
    std::chrono::system_clock::time_point mExpireTime;
};

// 睡眠到什么时间点
Task<void, SleepUntilPromise> sleep_until(std::chrono::system_clock::time_point expireTime) {
    auto &loop = getLoop();
    co_await SleepAwaiter(loop, expireTime);
}

// 睡眠一段时间
Task<void, SleepUntilPromise> sleep_for(std::chrono::system_clock::duration duration) {
    // 时间点加时间段等于时间点
    auto &loop = getLoop();
    co_await SleepAwaiter(loop, std::chrono::system_clock::now() + duration);
    co_return;
}

struct ReturnPreviousPromise {
    auto initial_suspend() noexcept {
        return std::suspend_always();
    }

    auto final_suspend() noexcept {
        return PreviousAwaiter(mPrevious);
    }

    void unhandled_exception() {
        throw;
    }

    void return_value(std::coroutine_handle<> previous) noexcept {
        mPrevious = previous;
    }

    auto get_return_object() {
        return std::coroutine_handle<ReturnPreviousPromise>::from_promise(
            *this);
    }

    std::coroutine_handle<> mPrevious{};

    ReturnPreviousPromise &operator=(ReturnPreviousPromise &&) = delete;
};

struct ReturnPreviousTask {
    // 为什么不直接用Promise<std::coroutine_handle<>>?
    // 为什么不直接用Task<std::coroutine_handle<>>?
    using promise_type = ReturnPreviousPromise;

    ReturnPreviousTask(std::coroutine_handle<promise_type> coroutine) noexcept
        : mCoroutine(coroutine) {}

    ReturnPreviousTask(ReturnPreviousTask &&) = delete;

    ~ReturnPreviousTask() {
        mCoroutine.destroy();
    }

    std::coroutine_handle<promise_type> mCoroutine;
};

struct WhenAllCtlBlock {
    std::size_t mCount;
    std::coroutine_handle<> mPrevious{};
    std::exception_ptr mException{};
};

struct WhenAllAwaiter {
    bool await_ready() const noexcept {
        return false;
    }

    std::coroutine_handle<>
    await_suspend(std::coroutine_handle<> coroutine) const {
        if (mTasks.empty()) return coroutine;
        mControl.mPrevious = coroutine;
        for (auto const &t: mTasks.subspan(0, mTasks.size() - 1))
            t.mCoroutine.resume();
        return mTasks.back().mCoroutine;
    }

    void await_resume() const {
        if (mControl.mException) [[unlikely]] {
            std::rethrow_exception(mControl.mException);
        }
    }

    WhenAllCtlBlock &mControl;
    std::span<ReturnPreviousTask const> mTasks;
};

template <class T>
ReturnPreviousTask whenAllHelper(auto const &t, WhenAllCtlBlock &control,
                                 Uninitialized<T> &result) {
    try {
        // 等待任务 t 完成，并将结果存储在 result 中
        result.putValue(co_await t);
    } catch (...) {
        // 如果任务 t 抛出异常，捕获异常并存储在控制块中
        control.mException = std::current_exception();
        // 提前返回之前挂起的协程句柄
        co_return control.mPrevious;
    }
    --control.mCount;
    // 如果所有任务都已完成（计数为0），返回之前挂起的协程句柄
    if (control.mCount == 0) {
        co_return control.mPrevious;
    }
    // 还有任务没完成，返回nullptr
    co_return nullptr;
}

template <std::size_t... Is, class... Ts>
Task<std::tuple<typename AwaitableTraits<Ts>::NonVoidRetType...>>
whenAllImpl(std::index_sequence<Is...>, Ts &&...ts) {
    // 创建控制块对象
    WhenAllCtlBlock control{sizeof...(Ts)};
    // 用于存储每个异步操作的结果，同时留着空间未初始化
    std::tuple<Uninitialized<typename AwaitableTraits<Ts>::RetType>...> result;
    // 创建了一个任务数组
    ReturnPreviousTask taskArray[]{whenAllHelper(ts, control, std::get<Is>(result))...};
    // 挂起等待
    co_await WhenAllAwaiter(control, taskArray);
    // 返回结果
    co_return std::tuple<typename AwaitableTraits<Ts>::NonVoidRetType...>(
        std::get<Is>(result).moveValue()...);
}


// 编译时检查，确保传入的异步操作数量不为零
template <Awaitable... Ts>
    requires(sizeof...(Ts) != 0)
auto when_all(Ts &&...ts) {
    // （编译时生成的索引序列, 任务）
    return whenAllImpl(std::make_index_sequence<sizeof...(Ts)>{},
                       std::forward<Ts>(ts)...);
}

struct WhenAnyCtlBlock {
    static constexpr std::size_t kNullIndex = std::size_t(-1);
    // 初始化为最大值，表示开始时没有任何协程完成
    std::size_t mIndex{kNullIndex};
    std::coroutine_handle<> mPrevious{};
    std::exception_ptr mException{};
};

struct WhenAnyAwaiter {
    bool await_ready() const noexcept {
        return false;
    }

    std::coroutine_handle<>
    await_suspend(std::coroutine_handle<> coroutine) const {
        if (mTasks.empty()) return coroutine;
        mControl.mPrevious = coroutine;
        for (auto const &t: mTasks.subspan(0, mTasks.size() - 1))
            t.mCoroutine.resume();
        return mTasks.back().mCoroutine;
    }

    void await_resume() const {
        // 在恢复前抛出异常
        if (mControl.mException) [[unlikely]] {
            std::rethrow_exception(mControl.mException);
        }
    }

    WhenAnyCtlBlock &mControl;
    std::span<ReturnPreviousTask const> mTasks;
};

template <class T>
ReturnPreviousTask whenAnyHelper(auto const &t, WhenAnyCtlBlock &control,
                                 Uninitialized<T> &result, std::size_t index) {
    try {
        result.putValue(co_await t);
    } catch (...) {
        control.mException = std::current_exception();
        co_return control.mPrevious;
    }
    --control.mIndex = index;
    // 有任务完成就返回
    co_return control.mPrevious;
}

template <std::size_t... Is, class... Ts>
// variant 只有其中一个为true，其中不能有void
Task<std::variant<typename AwaitableTraits<Ts>::NonVoidRetType...>>
whenAnyImpl(std::index_sequence<Is...>, Ts &&...ts) {
    WhenAnyCtlBlock control{};
    std::tuple<Uninitialized<typename AwaitableTraits<Ts>::RetType>...> result;
    ReturnPreviousTask taskArray[]{whenAnyHelper(ts, control, std::get<Is>(result), Is)...};
    co_await WhenAnyAwaiter(control, taskArray);
    Uninitialized<std::variant<typename AwaitableTraits<Ts>::NonVoidRetType...>> varResult;
    // 折叠表达式，执行左边语句，然后执行右边语句，最后返回右边表达式的结果
    // 遍历所有可能的索引 Is，并检查 control.mIndex 是否等于每个索引。
    // 如果是，它将使用 std::in_place_index<Is> 来构造 varResult 中的正确类型，并将对应的结果移动到变体中
    // 返回值为0
    ((control.mIndex == Is && (varResult.putValue(
        std::in_place_index<Is>, std::get<Is>(result).moveValue()), 0)), ...);
    // moveValue是对Uninitialized类中union成员的析构
    co_return varResult.moveValue();
}

template <Awaitable... Ts>
    requires(sizeof...(Ts) != 0)
auto when_any(Ts &&...ts) {
    return whenAnyImpl(std::make_index_sequence<sizeof...(Ts)>{},
                       std::forward<Ts>(ts)...);
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

Task<int> hello() {
    auto a = co_await hello1();
    debug(), "hello: a = ", a;
    auto b = co_await hello2();
    debug(), "hello: b = ", b;
    debug(), "hello开始等1和2";
    auto v = co_await when_any(hello2(), hello1());
    debug(), "hello看到", (int)v.index() + 1, "睡醒了";
    co_return std::get<1>(v);
}

int main() {
    auto t = hello();
    getLoop().run(t);
    debug(), "主函数中得到hello结果:", t.mCoroutine.promise().ReturnResult();
    return 0;
}
