#pragma once

#include <stdint.h>
#include <thread>
#include <atomic>
#include <unordered_map>
#include "AtomicRingBuffer.h"

typedef void* FiberHandle;

// Entry point for each job
using JobEntry = void(void* param);

// Counter used for synchronizng jobs
using Counter = std::atomic<uint32_t>;

// Job decleration for adding jobs
struct JobDecl
{
	JobEntry* mFunction = nullptr;
	void* mParam = nullptr;
	Counter* mCounter;
};

class JobSystem
{
public:
	struct Args
	{
		Args()
			:
			mNumThreads(std::thread::hardware_concurrency())
		{};

		uint8_t mNumThreads;
		uint16_t mNumFibers = 512;
		uint32_t mFiberStackSize = 512 * 1024;
		size_t mQueueSize = 1024;
	};

	JobSystem(const Args& args = {});
	~JobSystem();
	void ShutDown();

	void RunJobs(JobDecl* jobs, uint32_t numOfJobs, Counter* counter);
	void WaitForCounter(Counter* counter);

	void Join()
	{
		for (uint32_t i = 0; i < mThreads.size(); ++i)
		{
			mThreads[i].join();
		}
	}
	
	bool IsShuttingDown() const { return mShutDown; }

private:
	static void ThreadWorkerEntry(void* userData);
	static void FiberWorkerEntry(void* userData);
	static void FiberJobEntry(JobDecl job, JobSystem& system);

	std::vector<std::thread> mThreads{};

	AtomicRingBuffer<FiberHandle> mFiberPool;
	AtomicRingBuffer<JobDecl> mJobQueue;

	alignas(64) SpinLock mWaitListLock {};
	std::unordered_map<Counter*, struct UsedFiber*> mWaitList{};

	std::atomic<bool> mShutDown = false;
};

