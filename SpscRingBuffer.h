#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>

/// @brief 单生产者单消费者（SPSC）无锁环形缓冲区
/// @tparam T 存储元素的类型，必须满足 std::is_trivially_destructible<T>::value
///
/// 设计特点：
/// - 容量自动向上取整为 2 的幂，使用位运算（& mask_）代替取模，提升性能
/// - 使用 Placement New 手动管理元素生命周期，支持非默认构造类型
/// - 引入本地缓存（headCached_ / tailCached_）减少对另一线程原子变量的跨核读取
/// - 严格 SPSC 语义：一个线程只调用 push/emplace，另一个线程只调用 pop
/// - 通过 alignas(64) 分离生产者和消费者的热路径数据，避免伪共享（False Sharing）
///
/// @note 本实现仅支持平凡析构类型（trivially destructible types），
///       因此无需显式调用析构函数，简化了生命周期管理。
/// @note 仅使用 C++14 标准特性
template <typename T>
class SpscRingBuffer {
	// ─── 编译期约束检查 ──────────────────────────────────────────────

	// T 不能是引用类型，因为需要在内部存储元素
	static_assert(!std::is_reference<T>::value,
		"T must not be a reference type");

	// 本简化版本要求 T 是平凡析构类型，这样可以安全地跳过析构调用
	// 常见的 POD 类型、简单结构体都满足此要求
	static_assert(std::is_trivially_destructible<T>::value,
		"T must be trivially destructible for this simplified version");

	// 无锁环形缓冲区依赖“先移动赋值、后更新 head_”的顺序来保证正确性。
	// 如果 T 的移动赋值可能抛异常，一旦异常抛出会导致缓冲区状态不一致。
	// 因此要求移动构造和移动赋值必须是 noexcept 的（无锁数据结构的常见约束）。
	static_assert(std::is_nothrow_move_constructible<T>::value,
		"T must be nothrow move constructible for lock-free safety");
	static_assert(std::is_nothrow_move_assignable<T>::value,
		"T must be nothrow move assignable for lock-free safety");

public:
	/// @brief 构造函数
	/// @param capacity 期望的缓冲区容量，实际容量会自动向上取整到最近的 2 的幂（最小为 2）
	explicit SpscRingBuffer(std::size_t capacity)
		: capacity_(nextPowerOf2(checkCapacity(capacity)))
		, mask_(capacity_ - 1)                                  // 掩码用于快速取模
		, buffer_(allocateAligned(capacity_))                   // 分配对齐内存
		, head_(0)                                              // 消费者索引
		, tailCached_(0)                                        // 消费者本地缓存
		, tail_(0)                                              // 生产者索引
		, headCached_(0)                                        // 生产者本地缓存
	{
		// 构造函数仅分配内存，不构造任何元素
		// 元素在 push/emplace 时通过 placement new 构造
	}

	/// @brief 析构函数：释放原始内存
	/// @note 由于 T 是平凡析构类型，无需逐个销毁元素，直接释放内存即可
	~SpscRingBuffer() {
		freeAligned(buffer_);
	}

	// ─── 禁止拷贝与移动 ──────────────────────────────────────────────
	// 原因：避免多线程环境下出现悬空指针或双重释放
	SpscRingBuffer(const SpscRingBuffer&) = delete;
	SpscRingBuffer& operator=(const SpscRingBuffer&) = delete;
	SpscRingBuffer(SpscRingBuffer&&) = delete;
	SpscRingBuffer& operator=(SpscRingBuffer&&) = delete;

	// ──────────────────────────────────────────────────────────────────
	// 生产者接口（仅生产者线程调用！）
	// ──────────────────────────────────────────────────────────────────

	/// @brief 就地构造元素并推入缓冲区
	/// @tparam Args 构造参数类型包
	/// @param args 传递给 T 构造函数的参数
	/// @return 成功返回 true；缓冲区满时返回 false
	///
	/// @note 此函数只能由生产者线程调用
	template <typename... Args>
	bool emplace(Args&&... args) {
		// 读取当前 tail 值（生产者自己写入，relaxed 足够）
		const std::size_t t = tail_.load(std::memory_order_relaxed);

		// ── 快速路径：使用本地缓存的 headCached_ 判断是否满 ──
		// 避免每次 push 都跨核读取 head_（原子操作开销大）
		if (t - headCached_ >= capacity_) {
			// 本地缓存显示可能已满，从全局 head_ 获取最新值
			// acquire 语义确保能看到消费者对 head_ 的所有更新
			headCached_ = head_.load(std::memory_order_acquire);

			// 二次确认：如果确实满了则返回 false
			if (t - headCached_ >= capacity_) {
				return false;
			}
		}

		// ── 在对应槽位上构造对象 ──
		// 使用 placement new 在预分配的内存上构造
		new (&buffer_[t & mask_]) T(std::forward<Args>(args)...);

		// ── 更新 tail_ ──
		// release 语义确保上面的构造操作对消费者可见
		// 消费者读取 tail_ 时使用 acquire 语义，形成同步
		tail_.store(t + 1, std::memory_order_release);
		return true;
	}

	/// @brief 推入左值引用
	/// @param value 要推入的元素（左值）
	/// @return 成功返回 true；缓冲区满时返回 false
	bool push(const T& value) {
		return emplace(value);
	}

	/// @brief 推入右值引用（移动语义）
	/// @param value 要推入的元素（右值）
	/// @return 成功返回 true；缓冲区满时返回 false
	bool push(T&& value) {
		return emplace(std::move(value));
	}

	// ──────────────────────────────────────────────────────────────────
	// 消费者接口（仅消费者线程调用！）
	// ──────────────────────────────────────────────────────────────────

	/// @brief 弹出一个元素到 out 参数
	/// @param out 接收弹出元素的输出参数
	/// @return 成功返回 true；缓冲区空时返回 false
	///
	/// @note 此函数只能由消费者线程调用
	bool pop(T& out) {
		// 读取当前 head 值（消费者自己写入，relaxed 足够）
		const std::size_t h = head_.load(std::memory_order_relaxed);

		// ── 快速路径：使用本地缓存的 tailCached_ 判断是否空 ──
		// 避免每次 pop 都跨核读取 tail_（原子操作开销大）
		if (h == tailCached_) {
			// 本地缓存显示可能为空，从全局 tail_ 获取最新值
			// acquire 语义确保能看到生产者对 tail_ 的所有更新
			tailCached_ = tail_.load(std::memory_order_acquire);

			// 二次确认：如果确实为空则返回 false
			if (h == tailCached_) {
				return false;
			}
		}

		// ── 移动出元素 ──
		// 从缓冲区移动赋值到输出参数
		out = std::move(buffer_[h & mask_]);

		// ── 更新 head_ ──
		// release 语义确保上面的移动操作对生产者可见
		// 生产者读取 head_ 时使用 acquire 语义，形成同步
		// 注意：由于 T 是平凡析构类型，无需显式调用析构函数
		head_.store(h + 1, std::memory_order_release);
		return true;
	}

	/// @brief 仅弹出（丢弃）队首元素，不取出值
	/// @return 成功返回 true；缓冲区空时返回 false
	///
	/// @note 此函数只能由消费者线程调用
	bool pop() {
		const std::size_t h = head_.load(std::memory_order_relaxed);

		if (h == tailCached_) {
			tailCached_ = tail_.load(std::memory_order_acquire);
			if (h == tailCached_) {
				return false;
			}
		}

		// 平凡析构类型：无需调用 ~T()，直接推进 head_ 即可
		head_.store(h + 1, std::memory_order_release);
		return true;
	}

	// ──────────────────────────────────────────────────────────────────
	// 查询接口
	// ──────────────────────────────────────────────────────────────────

	/// @brief 获取队首元素指针
	/// @return 非空时返回指向队首元素的 const 指针；队列为空时返回 nullptr
	///
	/// @warning ⚠️ 此函数只能由消费者线程调用！
	/// @note 原因：pop() 内部先移动元素、再 release 更新 head_，
	///       如果由非消费者线程在这个窗口期调用 front()，
	///       可能读到已被移动的对象，属于未定义行为。
	const T* front() const noexcept {
		const std::size_t h = head_.load(std::memory_order_relaxed);
		const std::size_t t = tail_.load(std::memory_order_acquire);
		return (h == t) ? nullptr : &buffer_[h & mask_];
	}

	/// @brief 获取队尾元素指针
	/// @return 非空时返回指向队尾元素的 const 指针；队列为空时返回 nullptr
	///
	/// @warning ⚠️ 此函数只能由消费者线程调用！
	const T* back() const noexcept {
		const std::size_t h = head_.load(std::memory_order_relaxed);
		const std::size_t t = tail_.load(std::memory_order_acquire);
		return (h == t) ? nullptr : &buffer_[(t - 1) & mask_];
	}

	/// @brief 获取当前元素数量（近似值）
	/// @note 可在任意线程调用，但结果仅为近似值（可能在调用瞬间发生变化）
	std::size_t size() const noexcept {
		return tail_.load(std::memory_order_relaxed)
			- head_.load(std::memory_order_relaxed);
	}

	/// @brief 判断是否为空（近似值）
	bool empty() const noexcept {
		return size() == 0;
	}

	/// @brief 判断是否已满（近似值）
	bool full() const noexcept {
		return size() >= capacity_;
	}

	/// @brief 获取缓冲区总容量
	std::size_t capacity() const noexcept {
		return capacity_;
	}

	/// @brief 清空所有元素
	///
	/// @warning ⚠️ 非线程安全！必须在确保无并发访问时调用
	///
	/// @note 由于 T 是平凡析构类型，只需重置指针即可，
	///       旧元素占用的内存会被后续写入覆盖。
	void clear_unsafe() noexcept {
		const std::size_t t = tail_.load(std::memory_order_relaxed);

		// 将所有指针重置到同一位置（清空状态）
		head_.store(t, std::memory_order_relaxed);
		tail_.store(t, std::memory_order_relaxed);
		tailCached_ = t;
		headCached_ = t;

		// 注意：不调用析构函数（平凡析构类型不需要）
		// 旧数据留在内存中，但不再可访问，后续 push 会覆盖
	}

private:
	// ─── 工具函数 ──────────────────────────────────────────────────────

	/// <summary>
	/// 要求Capacity不小于 2，且不超过 1u << 20
	/// </summary>
	/// <param name="c"></param>
	/// <returns></returns>
	static std::size_t checkCapacity(std::size_t c) {
		constexpr std::size_t kMinCap = 2;
		constexpr std::size_t kMaxCap = 1u << 20;

		if (c < kMinCap) return kMinCap;
		if (c > kMaxCap) return kMaxCap;
		return c;
	}

	/// @brief 将 v 向上取整到最近的 2 的幂
	/// @param v 输入值
	/// @return 不小于 v 的最小的 2 的幂
	/// @note 若 v 本身已是 2 的幂，则返回 v 本身
	static std::size_t nextPowerOf2(std::size_t v) noexcept {
		std::size_t p = 1;
		while (true) {
			if (p >= v) break;
			p <<= 1;
		}
		return p;
	}

	/// @brief 按 alignof(T) 对齐分配 count 个 T 大小的原始内存
	/// @param count 元素个数
	/// @return 指向对齐内存的指针
	/// @throw std::bad_alloc 内存分配失败
	/// @throw std::length_error count * sizeof(T) 溢出 size_t
	///
	/// @note 仅使用 C++14 标准库特性：
	///       - 使用 std::malloc 分配内存
	///       - 手动偏移实现对齐，不依赖 C++17 的 std::align_val_t
	///       - 内存布局：[原始指针(8字节)] [填充] [对齐后的内存]
	static T* allocateAligned(std::size_t count) {
		// 确定对齐要求：至少为 std::max_align_t 的对齐值
		constexpr std::size_t alignment =
			alignof(T) > alignof(std::max_align_t) ? alignof(T) : alignof(std::max_align_t);

		// ── 溢出检查 ──
		if (count != 0 && sizeof(T) > (static_cast<std::size_t>(-1) / count)) {
			throw std::length_error("SpscRingBuffer: requested capacity overflows size_t");
		}
		const std::size_t bytes = count * sizeof(T);

		// ── 计算所需总内存 ──
		// 需要额外分配 alignment 字节用于对齐调整，再加 sizeof(void*) 存储原始指针
		const std::size_t totalBytes = bytes + alignment + sizeof(void*);
		void* raw = std::malloc(totalBytes);
		if (!raw) {
			throw std::bad_alloc();
		}

		// ── 计算对齐后的地址 ──
		// 在 raw 之后预留 sizeof(void*) 字节存放原始指针
		std::uintptr_t rawAddr = reinterpret_cast<std::uintptr_t>(raw) + sizeof(void*);
		// 向上对齐到 alignment 的倍数
		std::uintptr_t alignedAddr = (rawAddr + alignment - 1) & ~(alignment - 1);

		void* alignedPtr = reinterpret_cast<void*>(alignedAddr);

		// ── 存储原始指针以便 freeAligned 使用 ──
		// 在对齐地址前面存下原始指针
		std::memcpy(reinterpret_cast<void*>(alignedAddr - sizeof(void*)), &raw, sizeof(void*));

		return static_cast<T*>(alignedPtr);
	}

	/// @brief 释放由 allocateAligned 分配的内存
	/// @param ptr 由 allocateAligned 返回的指针
	static void freeAligned(T* ptr) noexcept {
		if (!ptr) return;

		void* alignedPtr = static_cast<void*>(ptr);
		std::uintptr_t alignedAddr = reinterpret_cast<std::uintptr_t>(alignedPtr);

		// 从对齐地址前面读取原始指针
		void* raw = nullptr;
		std::memcpy(&raw, reinterpret_cast<void*>(alignedAddr - sizeof(void*)), sizeof(void*));

		std::free(raw);
	}

	// ─── 成员变量 ──────────────────────────────────────────────────────

	const std::size_t capacity_;   ///< 实际容量（2 的幂）
	const std::size_t mask_;       ///< 取模掩码 = capacity_ - 1
	T* const buffer_;              ///< 原始内存指针（对齐分配，构造函数中赋值）

	// ── 消费者热路径数据（独占一个缓存行） ──
	// head_：由消费者写入，生产者读取
	// tailCached_：消费者私有的本地缓存，无需同步
	alignas(64) std::atomic<std::size_t> head_;
	std::size_t tailCached_;

	// ── 生产者热路径数据（独占一个缓存行） ──
	// tail_：由生产者写入，消费者读取
	// headCached_：生产者私有的本地缓存，无需同步
	alignas(64) std::atomic<std::size_t> tail_;
	std::size_t headCached_;
};
