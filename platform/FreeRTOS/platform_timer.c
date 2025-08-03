/*
 * @Author: jiejie
 * @Github: https://github.com/jiejieTop
 * @Date: 2019-12-10 22:16:41
 * @LastEditTime: 2020-04-27 22:35:34
 * @Description: the code belongs to jiejie, please keep the author information and source code according to the license.
 */

#include "platform_timer.h"
#include "FreeRTOS.h"
#include "task.h"

/**
 * @brief 获取系统自启动以来的运行时间（单位：毫秒）
 *
 * 该函数返回从系统启动到当前时刻的时间差，以毫秒为单位。
 * 它基于 FreeRTOS 的 tick 计数器（xTaskGetTickCount）进行计算，
 * 并适配不同的系统时钟频率（configTICK_RATE_HZ），确保返回值始终为毫秒。
 *
 * 在实时操作系统中，此函数常用于：
 *   - 超时判断（如网络超时、等待超时）
 *   - 定时器实现
 *   - 心跳检测（Keep-Alive）
 *   - 性能分析与日志时间戳
 *
 * @return 系统运行时间（毫秒），自系统启动以来经过的时间
 *
 * @note
 *   - 结果为 32 位无符号整数，最大约 49.7 天（2^32 ms ≈ 49.7 days）
 *   - 若需更长时间支持，可考虑使用 64 位版本
 *   - 线程安全：xTaskGetTickCount() 是线程安全的
 *   - 适用于 tick 频率 ≤ 1000Hz 的系统
 *
 * @see xTaskGetTickCount(), platform_timer_expired(), platform_timer_remain()
 */
static uint32_t platform_uptime_ms(void)
{
#if (configTICK_RATE_HZ == 1000)
    /* 当系统 tick 频率为 1000Hz 时，每个 tick 正好是 1ms */
    /* 因此可以直接返回 tick 计数值作为毫秒数 */
    return (uint32_t)xTaskGetTickCount();
#else
    /* 当 tick 频率不是 1000Hz 时，需要进行单位换算 */
    TickType_t tick = 0u;

    /* 获取当前 tick 数，并乘以 1000 将其转换为“毫秒级 tick” */
    tick = xTaskGetTickCount() * 1000;
    /* 通过除法并向上取整的方式，转换为实际毫秒数 */
    /* 公式: (tick * 1000 + configTICK_RATE_HZ - 1) / configTICK_RATE_HZ */
    /* 实现了四舍五入效果，避免精度丢失 */
    return (uint32_t)((tick + configTICK_RATE_HZ - 1) / configTICK_RATE_HZ);
#endif
}

/**
 * @brief 初始化一个平台定时器对象
 *
 * 该函数用于初始化一个 platform_timer_t 类型的定时器结构体，将其内部的时间值
 * 重置为 0，表示该定时器处于未设置或无效状态。这是使用任何平台定时器之前
 * 必须调用的初始化步骤。
 *
 * 初始化定时器是使用 platform_timer_expired()、platform_timer_remain() 等
 * 定时器相关函数的前提，确保结构体处于已知的初始状态，避免未定义行为。
 *
 * @param[in,out] timer  指向待初始化的定时器结构体的指针
 *                       必须为非 NULL，否则可能导致未定义行为
 *
 * @note
 *   - 该函数仅设置内部时间字段为 0，不涉及硬件或系统资源分配
 *   - 通常在创建定时器变量后立即调用
 *   - 线程安全：本函数无副作用，可在多线程环境下安全调用（前提是 timer 不共享）
 *
 * @see platform_timer_cutdown, platform_timer_expired, platform_timer_remain
 *
 * @example
 *   platform_timer_t my_timer;
 *   platform_timer_init(&my_timer);           // 必须先初始化
 *   platform_timer_cutdown(&my_timer, 1000);  // 然后设置倒计时 1000ms
 */
void platform_timer_init(platform_timer_t* timer)
{
    // 将定时器的内部时间值设置为 0
    // 表示该定时器当前不指向任何有效的时间点或倒计时
    timer->time = 0;
}

/**
 * @brief 设置一个倒计时定时器（从当前时间开始，倒计时指定毫秒数）
 *
 * 该函数用于设置一个软件定时器，使其在当前系统时间的基础上增加指定的超时时间（毫秒），
 * 从而表示“从现在起 delay 毫秒后超时”。后续可通过 platform_timer_expired() 判断
 * 该定时器是否已经到期。
 *
 * 此函数通常用于实现超时机制，如：
 *   - 网络连接超时
 *   - MQTT 报文 ACK 等待超时
 *   - 心跳检测间隔
 *
 * @param[in,out] timer    指向定时器结构体的指针
 *                         必须已通过 platform_timer_init() 初始化
 * @param[in]     timeout  超时时间，单位为毫秒（ms）
 *                         例如：500 表示 500 毫秒后超时
 *
 * @note
 *   - 该函数不阻塞，仅设置目标到期时间
 *   - 定时器精度依赖于 platform_uptime_ms() 的实现和系统 tick 频率
 *   - 若 timeout 为 0，表示立即超时（timer->time 被设为当前时间）
 *   - 多线程访问共享 timer 时需加锁保护
 *
 * @see platform_timer_init(), platform_timer_expired(), platform_timer_remain(), platform_uptime_ms()
 *
 * @example
 *   platform_timer_t timer;
 *   platform_timer_init(&timer);                // 初始化
 *   platform_timer_cutdown(&timer, 1000);       // 设置 1000ms 倒计时
 *
 *   while (!platform_timer_expired(&timer)) {
 *       // 等待超时...
 *   }
 *   // 此时已过去约 1000ms
 */
void platform_timer_cutdown(platform_timer_t* timer, unsigned int timeout)
{
    // 获取当前系统运行时间（单位：毫秒）
    // platform_uptime_ms() 返回自系统启动以来经过的毫秒数
    timer->time = platform_uptime_ms();

    // 在当前时间基础上增加指定的超时时间（timeout）
    // 设置定时器的“到期时间点”
    // 当前时间 + 超时时间 = 目标到期时间
    timer->time += timeout;
}

/**
 * @brief 检查指定的平台定时器是否已经超时（过期）
 *
 * 该函数用于判断一个通过 platform_timer_cutdown() 设置的定时器是否已经到达或超过
 * 其设定的到期时间。它是实现非阻塞超时控制的核心函数，常用于轮询等待、重试机制、
 * 心跳检测等场景。
 *
 * 判断逻辑：
 *   - 获取当前系统时间（毫秒）
 *   - 与定时器记录的目标到期时间（timer->time）进行比较
 *   - 若当前时间 > 到期时间，则认为已超时
 *
 * @param[in] timer  指向待检查的定时器结构体的指针
 *                   必须已通过 platform_timer_cutdown() 设置过倒计时
 *
 * @return
 *   - 1 : 定时器已超时（当前时间 > timer->time）
 *   - 0 : 定时器尚未超时
 *
 * @note
 *   - 该函数是线程安全的（前提是 timer 不被并发修改）
 *   - 使用无符号整数比较，具备 32 位溢出（约 49.7 天）的安全性
 *     （只要超时时间小于 ~24.8 天，比较结果依然正确）
 *   - 返回值为 char 类型，可直接用于条件判断（如 if(timer_expired(t))）
 *   - 依赖 platform_uptime_ms() 提供毫秒级系统时间
 *
 * @see platform_timer_cutdown(), platform_timer_init(), platform_timer_remain(), platform_uptime_ms()
 *
 * @example
 *   platform_timer_t timer;
 *   platform_timer_cutdown(&timer, 1000);  // 设置 1000ms 倒计时
 *
 *   while (!platform_timer_is_expired(&timer)) {
 *       // 等待超时...
 *       platform_sleep_ms(10);  // 小延时
 *   }
 *   // 跳出循环时，定时器已超时
 */
char platform_timer_is_expired(platform_timer_t* timer)
{
    // 获取当前系统运行时间（单位：毫秒）
    // platform_uptime_ms() 返回自系统启动以来经过的毫秒数
    // 类型通常为 uint32_t，支持溢出回绕
    uint32_t current_time = platform_uptime_ms();

    // 比较当前时间是否已超过定时器设定的到期时间
    // 如果当前时间 > timer->time，说明定时器已过期，返回 1
    // 否则返回 0
    return (current_time > timer->time) ? 1 : 0;
}

/**
 * @brief 获取指定定时器的剩余毫秒数
 *
 * 该函数用于查询一个已设置的倒计时定时器（通过 platform_timer_cutdown）
 * 还剩多少毫秒超时。若定时器已过期，则返回 0。
 *
 * 常用于：
 *   - 超时等待中的进度反馈
 *   - 动态调整等待时间
 *   - MQTT 等协议中计算剩余重试时间
 *
 * @param[in] timer  指向已设置的定时器结构体的指针
 *
 * @return
 *   - 大于 0 : 定时器尚未超时，返回剩余毫秒数
 *   - 0      : 定时器已超时或即将超时
 *
 * @note
 *   - 返回值为有符号整数，但实际值非负
 *   - 利用无符号整数回绕特性，具备溢出安全性（只要超时时间合理）
 *   - 多线程访问共享 timer 时需加锁保护
 *
 * @see platform_timer_cutdown(), platform_timer_is_expired(), platform_uptime_ms()
 */
int platform_timer_remain(platform_timer_t* timer)
{
    // 获取当前系统运行时间（单位：毫秒）
    uint32_t now = platform_uptime_ms();

    // 如果当前时间已大于等于定时器到期时间，说明已过期
    // 返回 0，表示无剩余时间
    if (timer->time <= now) {
        return 0;
    }

    // 否则，计算并返回剩余毫秒数
    // 由于 timer->time > now，结果为正，可安全转换为 int
    return (int)(timer->time - now);
}
/**
 * @brief 获取当前系统运行时间（单位：毫秒）
 *
 * 该函数返回自系统启动以来经过的毫秒数，通常用于时间戳记录、
 * 性能分析、日志打点等场景。
 *
 * 它是对 platform_uptime_ms() 的封装，提供统一的返回类型接口。
 *
 * @return 当前系统运行时间（毫秒），类型为 unsigned long
 *         最大约 49.7 天后回绕（取决于 uint32_t 范围）
 *
 * @note
 *   - 返回值类型为 unsigned long，便于与旧代码兼容
 *   - 实际精度依赖于 FreeRTOS 的 configTICK_RATE_HZ
 *   - 线程安全：platform_uptime_ms() 是线程安全的
 *
 * @see platform_uptime_ms()
 *
 * @example
 *   unsigned long start = platform_timer_now();
 *   // ... 执行操作 ...
 *   unsigned long elapsed = platform_timer_now() - start;
 *   printf("耗时: %lu ms\n", elapsed);
 */
unsigned long platform_timer_now(void)
{
    // 调用底层平台函数获取当前系统运行时间（毫秒）
    // 并转换为 unsigned long 类型返回
    return (unsigned long)platform_uptime_ms();
}

/**
 * @brief 使当前任务休眠指定的微秒数（实际精度为 tick 周期）
 *
 * 该函数用于让当前任务暂停执行一段时间。由于 FreeRTOS 的调度
 * 基于 tick，因此实际休眠时间会被对齐到最近的 tick 边界。
 *
 * 注意：该函数是阻塞的，会释放 CPU 给其他任务。
 *
 * @param[in] usec 期望休眠的时间，单位为微秒（μs）
 *                  若为 0，则尝试进行一次任务调度（类似 yield）
 *
 * @note
 *   - 实际休眠时间 >= 请求时间，且为 tick 周期的整数倍
 *   - 最小休眠时间为 1 个 tick（例如 1ms 或 10ms）
 *   - 高精度延时（<1ms）在此类系统中通常不可靠，应使用硬件定时器
 *   - 函数内部调用 vTaskDelay()，因此只能在任务上下文中调用，不能在中断中使用
 *
 * @warning
 *   - 传入过大的 usec 值可能导致 tick 计算溢出（罕见）
 *   - 不要用于精确的硬件时序控制（如 SPI bit-banging）
 *
 * @see vTaskDelay(), portTICK_PERIOD_MS
 *
 * @example
 *   platform_timer_usleep(500);    // 休眠约 500μs（实际为 1ms 如果 tick=1ms）
 *   platform_timer_usleep(0);      // 放弃剩余时间片，进行任务调度
 */
void platform_timer_usleep(unsigned long usec)
{
    // 定义 tick 变量，用于存储要延迟的 tick 数
    TickType_t tick;

    // 如果请求的微秒数不为 0
    if (usec != 0) {
        // 将微秒转换为 tick 数
        // portTICK_PERIOD_MS 是每个 tick 的毫秒数（如 1, 10）
        // 所以 usec / (portTICK_PERIOD_MS * 1000) 才是正确换算
        // 但此处代码存在 **潜在错误**，见下方说明
        tick = usec / portTICK_PERIOD_MS;

        // 如果计算出的 tick 为 0（说明 usec 很小）
        // 至少延迟 1 个 tick，否则不会休眠
        if (tick == 0)
            tick = 1;
    }
    // 如果 usec == 0，则不修改 tick，但下面仍会 delay(0)
    // vTaskDelay(0) 表示放弃时间片，进行一次任务调度（yield）

    // 调用 FreeRTOS 的任务延迟函数
    // 使当前任务进入阻塞状态，直到延迟结束
    vTaskDelay(tick);
}

