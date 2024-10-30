#pragma once

#include <atomic>
#include <emmintrin.h>

// Based on https://www.slideshare.net/ssuser052dd11/igc2018-amd-don-woligroski-why-ryzen page 43

class SpinLock
{
public:
	void Lock() 
	{
		while (true) 
		{
			while (mLock)
			{
				_mm_pause();
			}

			if (!mLock.exchange(true))
			{
				break;
			}
		}
	}

	void Unlock() 
	{
		mLock.store(false);
	}

	// For the compiler
	void unlock() { Unlock(); }
	void lock() { Lock(); }

private:
	std::atomic<bool> mLock = false;
};