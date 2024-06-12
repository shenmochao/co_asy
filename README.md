# 协程库相关

尚未完成，正在学习epoll相关和协程实现

## 概要

协程库可以围绕`co_return`、`co_yield`和`co_await`三个关键字展开

首先，我们需要一个Awaiter （协程等待者）来定义`co_await`的行为，通常需要**就绪、挂起、恢复**三个方法来实现对协程的控制

接着，我们需要一个Awaitable（可等待对象），从中文名可以看出，这是一个可以被`co_await`的对象，能够**重载`co_await`并初始化一个Awaiter**

随后，我们需要定义Promise，以定义协程的**入口和退出点**，还需要包含**异常处理**和**`co_return`、`co_yield`的定义**，如`co_return void`和`co_yield value`

最后，我们能够使用上述方法封装协程的执行，类中应当包含协程句柄，使用包含Promise的Task类，我们需要在类中提供获取协程结果的接口

但实际上，一个协程的Task到底需要些什么？
* 定义co_await的行为，对于**就绪、挂起、恢复**三个方法来实现对协程的控制
	* 就绪：没啥变化，一般都是返回 bool false
	* 挂起：指明协程调度
	* 恢复：可用于传值
* 包装co_await，将其返回值设置为协程等待者（Awaiter），可以视为控制器的包装
* 需要指定一个promise_type
将以上信息全部封装就可以成为一个协程任务类了，所以，Awaitable类在很多时候可以不显式实现

在许多时候，Promise中的Awaiter是用于定义协程开始和最终暂停时的行为，而每个Task中的Awaiter可以自行决定在该任务中协程暂停和恢复时的行为，所以，一个协程任务可以有多种Awaiter
## 具体的功能实现：
#### RepeatAwaiter 
* `bool await_ready` 是否准备好销毁
* `await_suspend` 挂起
* `await_resume` 恢复
#### RepeatAwaitable
* 重载 `co_await`，初始化一个RepeatAwaiter 
### Promise
值：
* mRetValue
方法：
* `initial_suspend`开始挂起：表达式恢复（无论是立即还是异步）时，协程开始执行你编写的协程体语句。
* `final_suspend` 结束挂起
* `unhandled_exception` 句柄中错误：如果执行离开 body-statements 是由于未处理的异常，则：
	1. 捕获异常并在catch块内调用`promise.unhandled_exception()`
	2. 调用`promise.final_suspend()`并`co_await`结果。
* `yield_value` ：`co_yeild value` 的调用
* `return_void` ：`co_return void` 的调用
注：

 `Promise`在执行开始挂起和结束挂起时都会调用`std::suspend_always`，这是一个`Awaiter` ，包含了：
 - `await_ready`：总是返回 `false`，表示协程应该挂起而不是继续执行。
- `await_suspend`：一个空操作，不做任何事情。
- `await_resume`：一个空操作，不返回任何值。
开始挂起时调用，是为了**不立即开始执行**协程体。这允许调用者有机会**获取协程的句柄**，以便可以在适当的时候恢复协程。
结束挂起时调用是为了在协程完全结束之前执行一些清理工作，或者**从协程中提取返回值**。

### Task
值：
* mCoroutine 持有协程句柄，通过这个句柄，可以执行如下操作：
	- **恢复协程**：调用 `mCoroutine.resume()` 可以恢复挂起的协程，继续执行协程体中的代码。
	- **检查协程状态**：调用 `mCoroutine.done()` 可以检查协程是否已经执行完毕。
	- **访问协程的承诺Promise**：通过 `mCoroutine.promise()` 可以访问协程的 `Promise` 对象，从而获取或设置协程的返回值`t.mCoroutine.promise().mRetValue`。
方法：
* `using promise_type = Promise;`这是一个类型别名声明，它定义了`Promise`作为`promise_type`的同义词
* 构造函数，接收一个协程句柄
使用：
直接定义：`Task hello(){}`即可，让函数返回一个Task类型的对象，其中包含了协程句柄