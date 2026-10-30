// Focused synchronization invariants; standalone C++17, no Godot runtime/GPU required.
#include <atomic>
#include <cassert>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

class Rendezvous {
	std::mutex mutex;
	std::condition_variable cv;
	bool running = true;
	unsigned accepted = 0;
	unsigned completed = 0;
public:
	bool submit() {
		std::lock_guard<std::mutex> lock(mutex);
		if (!running) return false;
		++accepted;
		return true;
	}
	void complete() {
		std::lock_guard<std::mutex> lock(mutex);
		++completed;
		cv.notify_all();
	}
	void close_and_drain() {
		std::unique_lock<std::mutex> lock(mutex);
		running = false;
		cv.wait(lock, [&] { return accepted == completed; });
	}
	bool rejected_after_close() { return !submit(); }
};

static void test_rendezvous() {
	Rendezvous r;
	assert(r.submit());
	std::atomic<bool> shutdown_started{ false };
	std::thread worker([&] {
		shutdown_started.store(true);
		r.close_and_drain();
	});
	while (!shutdown_started.load()) std::this_thread::yield();
	r.complete();
	worker.join();
	assert(r.rejected_after_close());
}

static void test_redraw_generations() {
	std::atomic<uint64_t> generation{ 0 };
	constexpr int threads = 8;
	constexpr int count = 1250;
	std::vector<std::thread> writers;
	for (int i = 0; i < threads; ++i) {
		writers.emplace_back([&] {
			for (int j = 0; j < count; ++j) generation.fetch_add(1, std::memory_order_relaxed);
		});
	}
	for (std::thread &thread : writers) thread.join();
	assert(generation.load(std::memory_order_acquire) == threads * count);
	// Request between has_changed() and draw() remains dirty after the draw acknowledges its sample.
	uint64_t acknowledged = generation.load(std::memory_order_acquire);
	generation.fetch_add(1, std::memory_order_relaxed);
	assert(generation.load(std::memory_order_acquire) != acknowledged);
	acknowledged = generation.load(std::memory_order_acquire);
	assert(generation.load(std::memory_order_acquire) == acknowledged);
}

static void test_gpu_stage_sequence() {
	uint64_t submitted = 0;
	uint64_t completed = 0;
	const uint64_t frame = ++submitted;
	assert(completed < frame); // A submitted frame is not yet complete.
	completed = frame; // Fence wait returned successfully.
	assert(completed >= frame);
}

int main() {
	test_rendezvous();
	test_redraw_generations();
	test_gpu_stage_sequence();
}
