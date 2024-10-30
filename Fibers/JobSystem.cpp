#include "JobSystem.h"
#include <Windows.h>
#include <assert.h>

thread_local FiberHandle tCurrentFiber = nullptr;
thread_local UsedFiber* tFiberToBeUnlockedAfterSwitch = nullptr;
thread_local FiberHandle tFiberToBeAddedToPool = nullptr;

// In order to prevent unfortunate situation, when counter gets decremented to 0 after waiting fiber is added to wait list, 
// but before that waiting fiber switched to another (pulled from fiber pool), we need this extra lock, 
// that we take in WaitForCounter, and release in WorkerMainLoop after switch is performed
struct UsedFiber 
{
	FiberHandle fiber{};
	SpinLock lock{};
};

JobSystem::JobSystem(const Args& args)
	:
	mThreads(args.mNumThreads),
	mFiberPool(args.mNumFibers),
	mJobQueue(args.mQueueSize)
{
	for (int i = 0; i < mThreads.size(); ++i)
	{
		mThreads[i] = std::thread(ThreadWorkerEntry, this);

		// Set affinity
		HANDLE handle = reinterpret_cast<HANDLE>(mThreads[i].native_handle());
		DWORD_PTR affinityMask = DWORD_PTR(1) << i;
		DWORD_PTR result = SetThreadAffinityMask(handle, affinityMask);
		assert(result != 0 && "Failed while setting thread affinity");
	}

	for (int i = 0; i < mFiberPool.Capacity(); ++i)
	{
		mFiberPool.PushBack(CreateFiber(args.mFiberStackSize, FiberWorkerEntry, this));
	}
}

JobSystem::~JobSystem()
{
	// TODO: Clean up fibers
}

void JobSystem::ShutDown()
{
	mShutDown = true;
	Join();
}

void JobSystem::RunJobs(JobDecl* jobs, uint32_t numOfJobs, Counter* counter)
{
	assert(jobs != nullptr && "Jobs always has to be present when running jobs... duh...");
	assert(counter != nullptr && "Counter always has to be present when running jobs");

	*counter = numOfJobs;

	for (uint32_t i = 0; i < numOfJobs; ++i)
	{
		jobs[i].mCounter = counter;
		mJobQueue.PushBack(jobs[i]);
	}
}

void JobSystem::WaitForCounter(Counter* counter)
{
	assert(counter != nullptr && "Counter always has to be present when waiting for it...");

	UsedFiber usedFiber{ tCurrentFiber };
	usedFiber.lock.Lock();

	// Add itself to the waitlist
	assert(tCurrentFiber != nullptr);
	mWaitListLock.Lock();
	mWaitList[counter] = &usedFiber;
	mWaitListLock.Unlock();

	if (*counter == 0)
	{
		std::lock_guard<SpinLock> guard(mWaitListLock);

		// We are here in one of 2 scenarios:
		// 1. Jobs was completed before we added ourselfs to wait list, or jobs were completed after we added ourselfs to wait list, 
		// but last job didn't take a mWaitListLock before us, so we just remove ourselves from wait list and continue execution.
		// 2. Jobs were completed after we added ourselfs to wait list and last job took mWaitListLock before us removed us from wait list,
		// and now it's spinning on StatefullFiber::m_lock, so we have to switch to free fiber, 
		// so we go to another fiber (and then releasing fiber lock) as fast as possible.

		auto itr = mWaitList.find(counter);

		if (itr != mWaitList.end())
		{
			// 1. Jobs were already completed, we remove ourselves from wait list and continue execution
			mWaitList.erase(counter);
			return;
		}

		// 2. Counter not equal to 0 has the same logic
	}

	std::optional<FiberHandle> workerFiber = mFiberPool.PopFront();
	assert(workerFiber.has_value() && "No more fibers available!");
	tCurrentFiber = workerFiber.value();

	// Fiber we switch to will unlock the lock on UsedFiber in FiberWorkerEntry
	tFiberToBeUnlockedAfterSwitch = &usedFiber;

	SwitchToFiber(workerFiber.value());

	// Fiber is done with work, so we are back, now add fiber that we switched from to fiber pool, set it to nullptr afterwards.
	// tFiberToBeUnlockedAfterSwitch cannot be null, because we can get here only when someone pulled us
	// from wait list and then switched to us, so we have to add previous fiber to the fiber pool.
	assert(tCurrentFiber != nullptr);
	assert(tFiberToBeAddedToPool != nullptr);
	mFiberPool.PushBack(tFiberToBeAddedToPool);
	tFiberToBeAddedToPool = nullptr;
}

void JobSystem::ThreadWorkerEntry(void* userData)
{
	JobSystem& jobSystem = *reinterpret_cast<JobSystem*>(userData);

	tCurrentFiber = ConvertThreadToFiber(nullptr);
	FiberWorkerEntry(userData);
	ConvertFiberToThread();
}

void JobSystem::FiberWorkerEntry(void* userData)
{
	JobSystem& jobSystem = *reinterpret_cast<JobSystem*>(userData);

	while (!jobSystem.IsShuttingDown())
	{
		if (tFiberToBeUnlockedAfterSwitch != nullptr)
		{
			tFiberToBeUnlockedAfterSwitch->lock.Unlock();
			tFiberToBeUnlockedAfterSwitch = nullptr;
		}

		std::optional<JobDecl> job = jobSystem.mJobQueue.PopFront();

		if (job.has_value())
		{
			FiberJobEntry(job.value(), jobSystem);
		}
		else
		{
			_mm_pause();
		}
	}
}

void JobSystem::FiberJobEntry(JobDecl job, JobSystem& system)
{
	job.mFunction(job.mParam);
	Counter* counter = job.mCounter;

	(*counter)--;

	if (*counter == 0)
	{
		system.mWaitListLock.Lock();
		auto itr = system.mWaitList.find(counter);

		// If counter is decremented before JobSystem::WaitForCounter() adds fiber to wait list,
		// or after fiber was added to wait list, but also after the WaitForCounter() noticed, 
		// that counter is 0 and  already removed itself from wait list. 
		// This situation is gonna be detecded in fiber that called JobSystem::WaitForCounter(),
		// so here, we just release mWaitListLock.
		if (itr == system.mWaitList.end())
		{
			system.mWaitListLock.Unlock();
			return;
		}

		UsedFiber* awaitingFiber = itr->second;
		assert(awaitingFiber->fiber != nullptr);
		system.mWaitList.erase(counter);
		system.mWaitListLock.Unlock(); // We have to relese it before we try to obtain the lock on fiber in order to avoid deadlock

		// When we call RunJobs and then WaitForCounter somewhere else, 
		// awaiting fiber (added to wait list) could have still not switched to
		// another fiber from pool, so we spin until that happens
		awaitingFiber->lock.Lock();
		// And imediately unlock, because awaitingFiber is now trully awaiting and it was the only purpose of this lock
		awaitingFiber->lock.Unlock();

		// Save current fiber to be added to fiber pool after switch is done
		tFiberToBeAddedToPool = tCurrentFiber;
		tCurrentFiber = awaitingFiber->fiber;
		// Switch to fiber pulled from wait list
		SwitchToFiber(awaitingFiber->fiber);

		// We push previous fiber to fiber pool only if we were on wait list and we came back from it.
		// Here, we weren't, so we are back again only because someone else got pushed to wait list,
		// so we can't add him to the pool, so tFiberToBeAddedToPool has to be nullptr
		assert(tFiberToBeAddedToPool == nullptr);
		assert(tCurrentFiber != nullptr);
	}
}
