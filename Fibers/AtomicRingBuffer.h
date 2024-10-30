#pragma once

#include <atomic>
#include <mutex>
#include <assert.h>
#include <optional>
#include "SpinLock.h"

template <class T>
class AtomicRingBuffer
{
public:
	AtomicRingBuffer(size_t capacity)
		:
		mCapacity(capacity)
	{
		mQueue = new T[capacity];
	}

	~AtomicRingBuffer()
	{
		delete[] mQueue;
	}

	inline void PushBack(T data) 
	{
		std::lock_guard<SpinLock> guard(mWriterSpinLock);
		size_t next = (mHead + 1) % mCapacity;

		// If already full, this will fail
		assert(next != mTail);

		mQueue[mHead] = data;
		mHead = next;
	}

	inline std::optional<T> PopFront() 
	{
		if (mHead != mTail)
		{
			std::lock_guard<SpinLock> guard(mReaderSpinLock);

			if (mHead != mTail)
			{
				T data = mQueue[mTail];
				mTail = (mTail + 1) % mCapacity;
				return data;
			}
		}

		return std::nullopt;
	}

	// -1, because if m_head == m_tail, the RingBuffer is empty,
	// If m_head == (m_tail - 1), the RingBuffer is full
	const size_t Capacity() { return mCapacity - 1; }

private:
	T* mQueue;
	const size_t mCapacity;

	alignas(64) SpinLock mWriterSpinLock;
	alignas(64) SpinLock mReaderSpinLock;
	
	// Atomics are needed, so updates for mHead and mTail won't
	// get rearanged and we can easly have two seperate locks for
	// readers and writers
	alignas(64)	std::atomic<size_t>	mHead = 0;
	alignas(64) std::atomic<size_t>	mTail = 0;
};